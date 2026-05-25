/*
 * sched_yield_test.c - WASI sched_yield test for wasm3-on-Nautilus
 *
 * Exports sched_yield_test(unused: u32) -> i32.
 * Calls sched_yield 10 times and cross-checks with clock_time_get:
 * the monotonic clock must advance after the yields, confirming that
 * CPU time was actually consumed in the scheduler rather than the
 * call being silently stubbed out.
 *
 * Compiled with -nostdlib --no-entry.
 */

#include <stdint.h>

__attribute__((import_module("wasi_snapshot_preview1"), import_name("sched_yield")))
int wasi_sched_yield(void);

__attribute__((import_module("wasi_snapshot_preview1"), import_name("clock_time_get")))
int wasi_clock_time_get(int clock_id, uint64_t precision, uint64_t *time_out);

__attribute__((visibility("default")))
__attribute__((export_name("sched_yield_test")))
int sched_yield_test(unsigned int unused)
{
    (void)unused;
    uint64_t t1 = 0, t2 = 0;
    int i;

    if (wasi_clock_time_get(1, 0, &t1) != 0) return -1;

    for (i = 0; i < 10; i++)
        if (wasi_sched_yield() != 0) return -2;

    if (wasi_clock_time_get(1, 0, &t2) != 0) return -3;
    if (t2 <= t1) return -4;  /* time must advance: yields took real CPU time */

    return 0;
}
