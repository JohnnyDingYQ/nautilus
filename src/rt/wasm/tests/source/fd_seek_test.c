/*
 * fd_seek_test.c - WASI fd_seek test for wasm3-on-Nautilus
 *
 * Exports fd_seek_test(unused: u32) -> i32.
 * Attempts to seek on stdout (fd=1) and stderr (fd=2); both are character
 * devices so both must return ESPIPE (29).
 *
 * Compiled with -nostdlib --no-entry.
 */

#include <stdint.h>

__attribute__((import_module("wasi_snapshot_preview1"), import_name("fd_seek")))
int wasi_fd_seek(int fd, int64_t offset, int whence, uint64_t *newoffset);

__attribute__((visibility("default")))
__attribute__((export_name("fd_seek_test")))
int fd_seek_test(unsigned int unused)
{
    (void)unused;
    uint64_t newoff = 0;

    /* SEEK_SET=0; stdout is a tty — must return ESPIPE (29) */
    if (wasi_fd_seek(1, 0, 0, &newoff) != 29) return -1;

    /* stderr likewise */
    if (wasi_fd_seek(2, 0, 0, &newoff) != 29) return -2;

    return 0;
}
