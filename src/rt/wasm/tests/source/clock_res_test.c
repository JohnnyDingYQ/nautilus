/*
 * clock_res_test.c - WASI clock_res_get test for wasm3-on-Nautilus
 *
 * Exports clock_res_test(unused: u32) -> i32.
 * Queries the resolution of both REALTIME (0) and MONOTONIC (1) clocks;
 * verifies both calls succeed and both resolutions are non-zero.
 *
 * Compiled with -nostdlib --no-entry.
 */

#include <stdint.h>

__attribute__((import_module("wasi_snapshot_preview1"), import_name("clock_res_get")))
int wasi_clock_res_get(int clock_id, uint64_t *res_out);

__attribute__((visibility("default")))
__attribute__((export_name("clock_res_test")))
int clock_res_test(unsigned int unused)
{
    (void)unused;
    uint64_t res = 0;

    if (wasi_clock_res_get(0, &res) != 0) return -1;  /* REALTIME */
    if (res == 0) return -2;

    res = 0;
    if (wasi_clock_res_get(1, &res) != 0) return -3;  /* MONOTONIC */
    if (res == 0) return -4;

    return 0;
}
