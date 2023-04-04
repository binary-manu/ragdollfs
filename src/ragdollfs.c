#define _DEFAULT_SOURCE

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <time.h>
#include <inttypes.h>
#include <stdbool.h>
#include <assert.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <syslog.h>

#define FUSE_USE_VERSION 31
#include <fuse.h>

#define when_false_log(expr, label, ...)  \
do {                                      \
    if (!(expr)) {                        \
        logger(__VA_ARGS__);              \
        goto label;                       \
    }                                     \
} while (0)

#define MIN(a, b) ({  \
    __auto_type _a = (a); \
    __auto_type _b = (b); \
    (_a < _b) ? _a : _b;  \
})

static void logger_impl_syslog(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsyslog(LOG_ERR | LOG_DAEMON, fmt, args);
    va_end(args);
}

static void logger_impl_stderr(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");
    va_end(args);
}

// Start logging to stderr, then switch to syslog after daemonization
static void (*logger)(const char *, ...) = logger_impl_stderr;

/** Default filename of the single cat file exposed by the filesystem.
 * Can be overridden via --filename.
 */
static const char * const RAGDOLLFS_DEFAULT_CATFILE_BASENAME = "single.cat";
/** Pathnames of segments passed to --segments are separated by this
 * character.
*/
static const char         RAGDOLLFS_SEGMENT_SEP              = ':';

/** Options parsed by fuse_parse_opt are stored here. */
struct options {
    int64_t     skip;          /**< How many bytes to skip at the start of the first segment. */
    int64_t     overlap;       /**< How many bytes to skip at the start of subsequent segments. */
    // This is not const because we strtok it
    char *       segments;      /**< Pathnames of segments, with separators in between. */
    const char * catFileName;   /**< Basename of the cat file. */
} options;

/** Represents a single file to use as a segment.
 * A segment can skip some bytes at the start: such amount is stored in `ssStart`.
 * The first usable byte has offset `jsStart` in the cat file.
 * 
 * Example: two segments of 128 bytes each and we skip 16 bytes from each.
 * Both structures will have ssStart == 16. The first segment has jsStart == 0, while the second
 * has jsStart == 112.
*/
struct segment {
    const char * path;      /**< Full pathname of the backing file. */
    off_t        ssStart;   /**< [S]egment [S]pace Start: how many bytes to skip at the begining of this segment. */
    off_t        jsStart;   /**< [J]oint [S]pace Start: the offset of the first usable byte from this file within the cat file. */
    off_t        size;      /**< Usable bytes from this file: excludes skipped bytes at the beginning. */
    int          fd;        /**< File descriptor reading from the segment's backing file. */
};

/** A position brings together a segment and an offset inside the segment */
struct position {
    const struct segment * segment;     /**< Points to a struct describing the segment. */
    off_t                  offInSeg;    /**< Position within the segment. Always 0 <= offInSeg <= segment->size */
};

/** Returns true if @param p points to the end of its segment. */
static bool isEOS(const struct position * restrict p) {
    return p->offInSeg >= p->segment->size;
}

/** Maps a position to an absolute file offset within the segment's backing file. */
static off_t positionToFileOffset(const struct position * restrict p) {
    return p->segment->ssStart + p->offInSeg;
}

/** Application-wide context. */
struct privdata {
    struct options  options;    /**< CLI options as parsed. */
    struct segment* segments;   /**< Per-segment structures. */
    off_t           totalSize;  /**< Combined size of all segments. Can be 0. */
    size_t          nSegments;  /**< Number of segments making up the catfile. Can be 0. */
    time_t          mtime;      /**< Used for the catfile atime and mtime. */
};

#define ADDOPT(t, f) {(t), offsetof(struct options, f), 1}
static struct fuse_opt optionsDescriptor[] = {
    ADDOPT("--skip=%"SCNd64,    skip),
    ADDOPT("--overlap=%"SCNd64, overlap),
    ADDOPT("--segments=%s",     segments),
    ADDOPT("--filename=%s",     catFileName),
    FUSE_OPT_END
};

static int ragdollfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
        off_t off, struct fuse_file_info *fi) {
    if (strcmp(path, "/")) {
        return -ENOENT;
    }

    const struct fuse_context *const fusectx = fuse_get_context();
    const struct privdata *privdata = fusectx->private_data;

    int error = 0;
    error |= filler(buf, ".", NULL, 0);
    error |= filler(buf, "..", NULL, 0);
    error |= filler(buf, privdata->options.catFileName, NULL, 0);

    return error ? -EIO : 0;
}

static int ragdollfs_getattr(const char *path, struct stat *statinfo) {
    const struct fuse_context *const fusectx = fuse_get_context();
    const struct privdata *privdata = fusectx->private_data;

    // Files and directories belong to the mount owner.
    statinfo->st_uid = fusectx->uid;
    statinfo->st_gid = fusectx->gid;
    // Give elements a meaningful time
    statinfo->st_mtime = privdata->mtime;

    if (!strcmp(path, "/")) {
        // dr-xr-x---
        statinfo->st_mode = S_IFDIR | S_IRUSR | S_IXUSR | S_IRGRP | S_IXGRP;
    } else if (!strcmp(path + 1, privdata->options.catFileName)) {
        // -r--r-----
        statinfo->st_mode = S_IFREG | S_IRUSR | S_IRGRP;
        statinfo->st_size = privdata->totalSize;
    } else {
        return -ENOENT;
    }

    return 0;
}

/** Orders segments by jsStart. */
static int compareSegmentsByJSStart(const void *vkey, const void *vitem) {
    const off_t * const key = vkey;
    const struct segment * const item = vitem;
    if      (*key <  item->jsStart)              return -1;
    else if (*key >= item->jsStart + item->size) return  1;
    else                                         return  0;
}

/** Finds a segment that contains a specific file offset and returns a position for
 * that offset. If no segment contains the offset, the returned position points to
 * the end of the last segment, so that isEOS is true.
*/
static struct position findSegment(off_t off, size_t nSegs, const struct segment *segs) {
    const struct segment *cur = bsearch(&off, segs, nSegs, sizeof(struct segment),
        &compareSegmentsByJSStart);
    if (!cur) {
        return (struct position){
            .segment = &segs[nSegs - 1],
            .offInSeg = segs[nSegs - 1].size
        };
    }
    return (struct position){
        .segment = cur,
        .offInSeg = off - cur->jsStart
    };
}

/** Reads data from a single segment, starting at @param position.
 * Reads that span many segments need to be handled by the caller.
 * The return value is either the amount of bytes read or a negative value.
 * The position is modified to account for the bytes read.
*/
static ssize_t readFromSegment(struct position * restrict fp, char *buf, size_t sz) {
    // This read try to read up to sz bytes from this segment starting at the current
    // segment offset. Since the underflying file may have been resized since it was
    // opened the call may return less data (even zero).
    // EINTR is not handled because fd must point to a regular file, so this read is
    // never "slow".
    ssize_t xfer = pread(fp->segment->fd, buf, MIN(sz, fp->segment->size - fp->offInSeg),
        positionToFileOffset(fp));
    if (xfer >= 0) {
        fp->offInSeg += xfer;
    } else {
        logger("error while reading from segment %s: %s", fp->segment->path, strerror(errno));
    }
    return xfer;
}

static int ragdollfs_open(const char *path, struct fuse_file_info *fi) {

    const struct fuse_context *fusectx = fuse_get_context();
    struct privdata *priv = fusectx->private_data;

    if (strcmp(priv->options.catFileName, path + 1)) {
        return -ENOENT;
    }

    if ((fi->flags & O_ACCMODE) != O_RDONLY) {
        return -EPERM;
    }

    return 0;
}

static int ragdollfs_release(const char *path, struct fuse_file_info *fi) {
    return 0;
}

static int ragdollfs_read(const char *path, char *buf, size_t sz, off_t off,
        struct fuse_file_info *fi) {
    const struct fuse_context *fusectx = fuse_get_context();
    struct privdata *priv = fusectx->private_data;

    // Don't proceed if there are no segments, as priv->segments points to
    // uninitialized space.
    if (!priv->nSegments) {
        return 0;
    }

    struct position fp = findSegment(off, priv->nSegments, priv->segments);

    ssize_t xfer = 0;
    size_t remaining = sz;

    while (remaining) {
        xfer = readFromSegment(&fp, buf, remaining);
        if (xfer < 0) {
            break;
        }
        remaining -= xfer;
        buf += xfer;

        bool needSwitch = isEOS(&fp);
        if (xfer == 0 && !needSwitch) {
            // If we get here, we cannot get more bytes from the underlying
            // file, but haven't read the expected amount of data yet.
            // The file was likely truncated.
            xfer = -1;
            break;
        }

        // Switch to the next segment, if any
        if (needSwitch) {
            fp.segment++;
            fp.offInSeg = 0;
            if (fp.segment - priv->segments >= priv->nSegments) {
                break;
            }
        }
    }
    return (xfer < 0) ? -EIO : sz - remaining;
}

static void *ragdollfs_init(struct fuse_conn_info *conn) {
    return fuse_get_context()->private_data;
}

static struct fuse_operations ragdollfs_ops = {
    .readdir    = &ragdollfs_readdir,
    .getattr    = &ragdollfs_getattr,
    .read       = &ragdollfs_read,
    .init       = &ragdollfs_init,
    .open       = &ragdollfs_open,
    .release    = &ragdollfs_release,
};

/** Parse raw options into usable data structures. */
static int fixOptions(struct privdata *priv) {
    struct options *opts = &priv->options;
    if (!opts->segments || !*opts->segments) {
        logger("no segments given");
        return -1;
    }
    priv->nSegments = 1;
    const size_t optLen = strlen(opts->segments);
    for (size_t i = 0; i < optLen; i++) {
        priv->nSegments += opts->segments[i] == RAGDOLLFS_SEGMENT_SEP;
    }
    // Note that we preallocate enough array elements to hold all expected
    // segments, but segments which do not contribute bytes (i.e. empty files)
    // will be ignored. The array can thus contain less initialized entries
    // than its full length. It can even be fully uninitialized if all
    // segments are discarded.
    priv->segments = malloc(sizeof(struct segment) * (priv->nSegments));
    when_false_log(priv->segments, die, "out of memory");

    when_false_log(opts->skip >= 0, die, "skip cannot be negative");
    when_false_log(opts->overlap >= 0, die, "overlap cannot be negative");
    when_false_log(*opts->catFileName, die, "empty filename");
    when_false_log(!strchr(opts->catFileName, '/'), die, "filename cannot contains slashes");

    char *seg = strtok(opts->segments, (const char[]){RAGDOLLFS_SEGMENT_SEP, 0});
    off_t totalLen = 0;
    off_t skipAtStart = priv->options.skip;
    size_t segIdx = 0;
    do  {
        struct stat segStats;
        when_false_log(!stat(seg, &segStats), die,
            "unable to stat segment %s for reading: %s", seg, strerror(errno));
        // We only accept regular files
        when_false_log(S_ISREG(segStats.st_mode), die,
            "segment %s is not a regular file", seg);
        // Skip segments which would not contribute bytes to the concatenation
        if (segStats.st_size <= skipAtStart) {
            priv->nSegments--;
            continue;
        }

        const int fd = open(seg, O_RDONLY);
        when_false_log(fd >= 0, die,
            "unable to open segment %s for reading: %s", seg, strerror(errno));

        struct segment *segToFill = &priv->segments[segIdx];
        // Turn relative paths into absolute paths, because we daemonize
        *segToFill = (struct segment){
            .path    = realpath(seg, NULL),
            .jsStart = totalLen,
            .ssStart = skipAtStart,
            .size    = segStats.st_size - skipAtStart,
            .fd      = fd
        };
        when_false_log(segToFill->path, die, "out of memory");
        // Total size may overflow. This check requires building with -fwrapv
        when_false_log(totalLen + segToFill->size > totalLen, die,
            "the total size of the catfile is too large");
        totalLen += segToFill->size;
        segIdx++;
    } while (
        skipAtStart = priv->options.overlap,  // Ignored in the condition
        (seg = strtok(NULL, ":"))
    );
    priv->totalSize = totalLen;

    return 0;

die:

    free(priv->segments);
    priv->segments = NULL;
    return -1;
}

static void show_help(const char *progname)
{
    printf("usage: %s [options] <mountpoint>\n\n", progname);
    printf("File-system specific options:\n"
           "    --filename=<s>              Name of the concatenated file\n"
           "                                (default: \"single.cat\")\n"
           "    --skip=<d>                  Skip d bytes from the first segment\n"
           "                                (default: 0)\n"
           "    --overlap=<d>               Skip d bytes from other segments\n"
           "                                (default: 0)\n"
           "    --segments=<path:path:...>  List of files to join, separated by :\n"
           "\n");
}
 

int main(int argc, char **argv) {
    struct privdata privdata = {
        .mtime = time(NULL),
        .options.skip = 0,
        .options.overlap = 0,
        .options.catFileName = RAGDOLLFS_DEFAULT_CATFILE_BASENAME,
    };

    if (argc < 2) {
        show_help(argv[0]);
        return EXIT_SUCCESS;
    }

    struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
    if (fuse_opt_parse(&args, &privdata.options, optionsDescriptor, NULL) < 0 ||
            fixOptions(&privdata)) {
        return EXIT_FAILURE;
    }

    logger = logger_impl_syslog;
    return fuse_main(args.argc, args.argv, &ragdollfs_ops, &privdata);
}
