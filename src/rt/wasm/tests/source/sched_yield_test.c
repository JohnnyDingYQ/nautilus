/*
 * sched_yield_test.c - WASI sched_yield test for wasm3-on-Nautilus
 *
 * Exports sched_yield_test(unused: u32) -> i32.
 * Calls sched_yield 10 times in a loop; verifies each call returns 0
 * (success).
 *
 * Compiled with -nostdlib --no-entry.
 */

__attribute__((import_module("wasi_snapshot_preview1"), import_name("sched_yield")))
int wasi_sched_yield(void);

__attribute__((visibility("default")))
__attribute__((export_name("sched_yield_test")))
int sched_yield_test(unsigned int unused)
{
    (void)unused;
    int i;
    for (i = 0; i < 10; i++)
        if (wasi_sched_yield() != 0) return -1;
    return 0;
}
