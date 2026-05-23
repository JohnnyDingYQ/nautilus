/*
 * random_test.c - WASI random_get test for wasm3-on-Nautilus
 *
 * Exports random_test(unused: u32) -> i32.
 * Calls random_get twice and verifies that:
 *   1. Both calls succeed (return 0).
 *   2. The two 16-byte buffers differ in at least one byte.
 *
 * Uses WASI import directly; compiled with -nostdlib --no-entry.
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

    if (wasi_random_get(a, 16) != 0) return -1;
    if (wasi_random_get(b, 16) != 0) return -2;

    for (i = 0; i < 16; i++)
        if (a[i] != b[i]) return 0;   /* at least one byte differs — pass */

    return -3;  /* all bytes identical: RNG appears broken */
}
