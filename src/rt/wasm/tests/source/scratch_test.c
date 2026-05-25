/*
 * scratch_test.c - host binding round-trip test for wasm3-on-Nautilus
 *
 * Exports scratch_roundtrip(unused: i32) -> i32.
 * Writes a known byte pattern to the kernel scratch buffer via
 * naut.scratch_write, reads it back via naut.scratch_read, and
 * compares byte-by-byte.  Returns 0 on pass, negative on failure.
 *
 * Two distinct patterns are written sequentially.  The second write
 * verifies that the host buffer is actually updated on each call and
 * is not simply returning a stale value from a prior test run.
 *
 * Compiled with -nostdlib / --no-entry (no WASI imports, no _start).
 * The dummy i32 parameter matches the wasm_eval calling convention.
 */

#include <stdint.h>

__attribute__((import_module("naut"), import_name("scratch_write")))
uint32_t naut_scratch_write(const void *ptr, uint32_t len);

__attribute__((import_module("naut"), import_name("scratch_read")))
uint32_t naut_scratch_read(void *ptr, uint32_t max);

__attribute__((visibility("default")))
__attribute__((export_name("scratch_roundtrip")))
int scratch_roundtrip(unsigned int unused)
{
    (void)unused;

    const unsigned char msg1[] = { 'n','a','u','t','i','l','u','s' };
    const unsigned char msg2[] = { '1','2','3','4','5','6','7','8' };
    unsigned char buf[8];
    uint32_t i;

    /* --- pass 1: write msg1, read back --- */
    if (naut_scratch_write(msg1, 8) != 8) return -1;

    for (i = 0; i < 8; i++) buf[i] = 0;
    if (naut_scratch_read(buf, 8) != 8) return -2;
    for (i = 0; i < 8; i++)
        if (buf[i] != msg1[i]) return -3;

    /* --- pass 2: write msg2, verify it overwrites msg1 --- */
    if (naut_scratch_write(msg2, 8) != 8) return -4;

    for (i = 0; i < 8; i++) buf[i] = 0;
    if (naut_scratch_read(buf, 8) != 8) return -5;
    for (i = 0; i < 8; i++)
        if (buf[i] != msg2[i]) return -6;

    return 0;
}
