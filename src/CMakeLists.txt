set(C_STANDARD 11)
set(C_STANDARD_REQUIRED ON)

include(FindPkgConfig)
pkg_check_modules(FUSE REQUIRED fuse)

set(EXTRA_CFLAGS -Wall -fwrapv)

add_executable(ragdollfs ragdollfs.c)
target_compile_options(ragdollfs PRIVATE ${FUSE_CFLAGS} ${EXTRA_CFLAGS})
target_link_options(ragdollfs PRIVATE ${FUSE_LDFLAGS})
