/*
 * fd_close_test.c - WASI fd_close test for wasm3-on-Nautilus
 *
 * Exports fd_close_test(unused: u32) -> i32.
 * Closes several file descriptors (stdout, stderr, and an arbitrary fd)
 * and verifies each returns 0 (success / no-op).
 *
 * Compiled with -nostdlib --no-entry.
 */

__attribute__((import_module("wasi_snapshot_preview1"), import_name("fd_close")))
int wasi_fd_close(int fd);

__attribute__((visibility("default")))
__attribute__((export_name("fd_close_test")))
int fd_close_test(unsigned int unused)
{
    (void)unused;
    if (wasi_fd_close(1) != 0) return -1;  /* stdout */
    if (wasi_fd_close(2) != 0) return -2;  /* stderr */
    if (wasi_fd_close(9) != 0) return -3;  /* arbitrary fd */
    return 0;
}
