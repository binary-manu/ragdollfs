# A `catfs` implementation for cat lovers

`ragdollfs` is concatenating FUSE filesystem. It takes a set of
independent files and creates a virtual filesystem that exposes a single
file containing the concatenation of individual segments.

A number of implementations of this concept exist: see the
[Alternatives](#alternatives) section for a partial list. This one adds some
features about handling segments which partially overlap (the last bytes of a
segment are identical to the first bytes of the next segment, and only one copy
should be retained). This kind of behaviour is common in the split MPEG TS files
produced by some PVR devices.

The name is obviously a homage to the ragdoll cat breed and an attempt to
avoid creating yet another `catfs` repo:

<div>
    <img width="256px" src="https://www.thesprucepets.com/thmb/vcUVLdknHr2_srEhcoqZxAU931Q=/960x0/filters:no_upscale():max_bytes(150000):strip_icc()/56242993_352522172040814_8005034287740231326_n-b4d042a7fa974a83a18d4c914e60a150.jpg">
</div>

## Building

Provided you have a C11 or above compiler installed as well as FUSE
development files, go to the project root and type:

```bash
cmake -DCMAKE_BUILD_TYPE=Release .
make
```

then copy `src/ragdollfs` wherever you like.

## Usage

```bash
ragdollfs [--skip=SKIP] [--overlap=OVERLAP] [--filename=FILENAME] \
  --segments=/path/to/file1:/path/to/file2:...:/path/to/fileN \
  /path/to/mountpoint
```

`SKIP` and `OVERLAP` are two optional non-negative integers. `SKIP`
gives a number of bytes to skip at the beginning of the first segment,
while `OVERLAP` does the same for subsequent segments. They are kept
separate because sometimes PVR recorders put garbled data at the
beginning of the first record chunk: this is useful to skip over junk.
These two values never extend to different segments: if a segment is
smaller than the relevant option value, it is ignored altogether.

`FILENAME` controls the name of the concatenated file. If not specified,
it is named `single.cat`. Slashes cannot appear inside this option.

`--segments` defines a list of one or more files to be concatenated,
separated by `:`, just like `PATH`. Each entry must be a regular file or
a symbolic link to a regular file.

The program dameonizes after correct setup, and errors are logged to
syslog under the `daemon` facility.

## Caveats

This implementation is optimized for a small number (tens to hundreds)
of segments. In particular, every segment is opened once at program
startup and never closed. This has benefits: there is no overhead in
repeatedly opening files when doing random accesses, and the contents
of segments remain accessible even if underlying files are deleted.
However, this requires one open file per segment, so it can reach
the per-process file limit.

Segment sizes are never re-read if they change, so that the concatenated
file remains of constant size. If a segment size changes during the process
lifetime, the follow two things happen:

* if a segment grows, the excess data will be inaccessible;
* if it shrinks, the first attempt to read beyond the new end of file
  triggers a read error.

The concatenated file is read-only.

## Example

Join a PVR recording made of 3 MPEG TS segments into a single file,
while skipping 48128 bytes at the beginning of each segment but the
first. This number equals 188 * 256, and results in skipping exactly
256 TS packets of 188 bytes. Some recorders duplicate the last 256
packets from a segment into the next even if no overlap is requested.

```bash
ragdollfs --overlap=48128 --segments=000.ts:001.ts:002.ts \
  --filename=single.ts ~/mnt
```

<a name="alternatives"></a>
## Alternatives

* https://github.com/Lex-2008/catfs is
  optimized for a very large number of files.

<!-- vi: set tw=72 et sw=2 fo=tcroqan autoindent: -->
