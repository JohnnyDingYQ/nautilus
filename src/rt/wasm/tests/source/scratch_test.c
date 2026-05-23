/*
 * scratch_test.c - host binding round-trip test for wasm3-on-Nautilus
 *
 * Exports scratch_roundtrip(unused: i32) -> i32.
 * Writes a known byte pattern to the kernel scratch buffer via
 * naut.scratch_write, reads it back via naut.scratch_read, and
 * compares byte-by-byte.  Returns 0 on pass, negative on failure.
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

    const unsigned char msg[] = { 'n','a','u','t','i','l','u','s' };
    unsigned char buf[8];
    uint32_t i;

    uint32_t written = naut_scratch_write(msg, 8);
    if (written != 8) return -1;

    for (i = 0; i < 8; i++) buf[i] = 0;

    uint32_t read = naut_scratch_read(buf, 8);
    if (read != 8) return -2;

    for (i = 0; i < 8; i++)
        if (buf[i] != msg[i]) return -3;

    return 0;
}
