/*
 * random_test.c - WASI random_get test for wasm3-on-Nautilus
 *
 * Exports random_test(unused: u32) -> i32.
 * Calls random_get twice and verifies that:
 *   1. Both calls succeed (return 0).
 *   2. The first buffer is not all-zeros (detects a fill-with-zero stub).
 *   3. The two 16-byte buffers differ in at least one byte.
 *
 * Compiled with -nostdlib --no-entry.
 */

#include <stdint.h>

__attribute__((import_module("wasi_snapshot_preview1"), import_name("random_get")))
int wasi_random_get(void *buf, uint32_t buf_len);

__attribute__((visibility("default")))
__attribute__((export_name("random_test")))
int random_test(unsigned int unused)
{
    (void)unused;
    unsigned char a[16], b[16];
    uint32_t i;
    int nonzero = 0;

    if (wasi_random_get(a, 16) != 0) return -1;

    /* first buffer must not be all-zeros — catches a stub that fills with 0 */
    for (i = 0; i < 16; i++)
        if (a[i] != 0) { nonzero = 1; break; }
    if (!nonzero) return -4;

    if (wasi_random_get(b, 16) != 0) return -2;

    /* two consecutive calls must produce different output */
    for (i = 0; i < 16; i++)
        if (a[i] != b[i]) return 0;

    return -3;  /* all bytes identical: RNG appears broken */
}
