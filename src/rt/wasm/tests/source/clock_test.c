/*
 * clock_test.c - WASI clock_time_get test for wasm3-on-Nautilus
 *
 * Exports clock_test(unused: u32) -> i32.
 * Calls clock_time_get twice with a busy loop between them and verifies
 * the timestamp is non-zero and strictly increases (monotonic).
 *
 * Loop count is 1 000 000 to give coarse-resolution timers enough time
 * to advance at least one tick between the two readings.
 *
 * Compiled with -nostdlib --no-entry.
 */

#include <stdint.h>

__attribute__((import_module("wasi_snapshot_preview1"), import_name("clock_time_get")))
int wasi_clock_time_get(int clock_id, uint64_t precision, uint64_t *time_out);

__attribute__((visibility("default")))
__attribute__((export_name("clock_test")))
int clock_test(unsigned int unused)
{
    (void)unused;
    uint64_t t1 = 0, t2 = 0;
    volatile unsigned int i;

    if (wasi_clock_time_get(1, 0, &t1) != 0) return -1;  /* MONOTONIC */
    if (t1 == 0) return -2;                               /* must be non-zero */

    for (i = 0; i < 1000000; i++) {}                      /* let time advance */

    if (wasi_clock_time_get(1, 0, &t2) != 0) return -3;
    if (t2 <= t1) return -4;                              /* must increase */

    return 0;
}
