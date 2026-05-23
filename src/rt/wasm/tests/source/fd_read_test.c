/*
 * fd_read_test.c - WASI fd_read test for wasm3-on-Nautilus
 *
 * Exports fd_read_test(unused: u32) -> i32.
 * Tests the fd_read host binding without blocking on stdin:
 *   - fd=1 (stdout) must return EBADF (8) immediately.
 *   - fd=0 (stdin) would block; tested manually, not here.
 *
 * Uses WASI import directly; compiled with -nostdlib --no-entry.
 */

#include <stdint.h>

__attribute__((import_module("wasi_snapshot_preview1"), import_name("fd_read")))
int wasi_fd_read(int fd, void *iovs, int iovs_len, uint32_t *nread);

__attribute__((visibility("default")))
__attribute__((export_name("fd_read_test")))
int fd_read_test(unsigned int unused)
{
    (void)unused;
    char buf[4];
    uint32_t nread = 0;

    /* iovec layout: { buf_offset: u32, buf_len: u32 } */
    uint32_t iov[2] = { (uint32_t)(uintptr_t)buf, sizeof(buf) };

    /* fd=1 (stdout) must return EBADF=8 without blocking */
    if (wasi_fd_read(1, iov, 1, &nread) != 8) return -1;

    return 0;
}
