/*
 * tests.c - Built-in wasm test suite for `wasm test`.
 *
 * Owns the wasm_tests[] table, wasm_eval(), and handle_wasm_test().
 * Calls wasm_setup()/wasm_teardown() from wasm_nautilus.c to instantiate
 * each module; does not touch WASI bindings or the interpreter internals.
 *
 * wasm_nautilus.h is force-included by the Makefile, providing printk and
 * the PAL macros (malloc → kmem_malloc, etc.).
 */

#include "source/wasm3.h"
#include "runner.h"
#include "tests.h"

/* Embedded wasm binaries — each is a C byte array from xxd -i. */
#include "tests/fib32.wasm.h"
#include "tests/fib32_tail.wasm.h"
#include "tests/fib64.wasm.h"
#include "tests/sqrt_test.wasm.h"
#include "tests/scratch_test.wasm.h"
#include "tests/clock_test.wasm.h"
#include "tests/random_test.wasm.h"
#include "tests/fd_read_test.wasm.h"
#include "tests/sched_yield_test.wasm.h"
#include "tests/clock_res_test.wasm.h"
#include "tests/fd_close_test.wasm.h"
#include "tests/fd_seek_test.wasm.h"
#include "tests/wasi_integration_test.wasm.h"

extern int printk(const char *fmt, ...);
extern int snprintf(char *buf, size_t size, const char *fmt, ...);

/* -----------------------------------------------------------------------
 * Test table
 *
 *   fib32/fib32_tail/fib64  recursive and tail-call Fibonacci
 *   sqrt(2/3/5/7)           f64.sqrt correctness via integer-scaled result
 *   scratch                 naut::scratch_write/read host-binding round-trip
 *   clock                   clock_time_get monotonicity
 *   random                  random_get produces non-zero, varying output
 *   fd_read                 fd_read on stdout returns EBADF without blocking
 *   sched_yield             sched_yield returns 0; clock advances
 *   clock_res               clock_res_get returns 1 ns for both clock IDs
 *   fd_close                fd_close returns 0 (no-op)
 *   fd_seek                 fd_seek returns ESPIPE (29)
 *   wasi_integration        all 13 WASI bindings + scratch in one module
 * ----------------------------------------------------------------------- */
static const struct {
    const char    *name;
    const char    *func;
    const uint8_t *bytes;
    uint32_t       len;
    uint32_t       arg;
    uint64_t       expected;
} wasm_tests[] = {
    { "fib32",      "fib",          fib32_wasm,      sizeof(fib32_wasm),      24, 46368ULL   },
    { "fib32_tail", "fib",          fib32_tail_wasm, sizeof(fib32_tail_wasm), 24, 46368ULL   },
    { "fib64",      "fib",          fib64_wasm,      sizeof(fib64_wasm),      24, 46368ULL   },
    { "sqrt(2)",    "sqrt_scaled",  sqrt_test_wasm,  sizeof(sqrt_test_wasm),   2, 1414213ULL },
    { "sqrt(3)",    "sqrt_scaled",  sqrt_test_wasm,  sizeof(sqrt_test_wasm),   3, 1732050ULL },
    { "sqrt(5)",    "sqrt_scaled",  sqrt_test_wasm,  sizeof(sqrt_test_wasm),   5, 2236067ULL },
    { "sqrt(7)",    "sqrt_scaled",  sqrt_test_wasm,  sizeof(sqrt_test_wasm),   7, 2645751ULL },
    { "scratch",    "scratch_roundtrip",    scratch_test_wasm,         sizeof(scratch_test_wasm),         0, 0ULL },
    { "clock",      "clock_test",          clock_test_wasm,           sizeof(clock_test_wasm),           0, 0ULL },
    { "random",     "random_test",         random_test_wasm,          sizeof(random_test_wasm),          0, 0ULL },
    { "fd_read",    "fd_read_test",        fd_read_test_wasm,         sizeof(fd_read_test_wasm),         0, 0ULL },
    { "sched_yield","sched_yield_test",    sched_yield_test_wasm,     sizeof(sched_yield_test_wasm),     0, 0ULL },
    { "clock_res",  "clock_res_test",      clock_res_test_wasm,       sizeof(clock_res_test_wasm),       0, 0ULL },
    { "fd_close",   "fd_close_test",       fd_close_test_wasm,        sizeof(fd_close_test_wasm),        0, 0ULL },
    { "fd_seek",    "fd_seek_test",        fd_seek_test_wasm,         sizeof(fd_seek_test_wasm),         0, 0ULL },
    { "wasi_integration", "wasi_integration_test",
      wasi_integration_test_wasm, sizeof(wasi_integration_test_wasm), 0, 0ULL },
    { NULL, NULL, NULL, 0, 0, 0 }
};

/* Run one test: instantiate, find export, call with one u32 arg, return result. */
static int wasm_eval(const uint8_t *bytes, uint32_t len, const char *func_name,
                     uint32_t arg, uint64_t *result_out)
{
    IM3Environment env;
    IM3Runtime runtime;
    M3Result res = wasm_setup(bytes, len, &env, &runtime);
    if (res) return -1;

    IM3Function func;
    res = m3_FindFunction(&func, runtime, func_name);
    if (res) { wasm_teardown(env, runtime); return -1; }

    char arg_str[32];
    snprintf(arg_str, sizeof(arg_str), "%u", arg);
    const char *args[1] = { arg_str };
    res = m3_CallArgv(func, 1, args);
    if (res) { wasm_teardown(env, runtime); return -1; }

    uint64_t ret = 0;
    m3_GetResultsV(func, &ret);
    *result_out = ret;
    wasm_teardown(env, runtime);
    return 0;
}

int handle_wasm_test(void)
{
    int passed = 0, total = 0;

    for (int i = 0; wasm_tests[i].name; i++) {
        total++;
        uint64_t result = 0;
        int err = wasm_eval(wasm_tests[i].bytes, wasm_tests[i].len,
                            wasm_tests[i].func, wasm_tests[i].arg, &result);
        if (err) {
            printk("[wasm-test] %-20s FAIL (setup error)\n",
                   wasm_tests[i].name);
        } else if (result != wasm_tests[i].expected) {
            printk("[wasm-test] %-20s FAIL (got %llu, expected %llu)\n",
                   wasm_tests[i].name,
                   (unsigned long long)result,
                   (unsigned long long)wasm_tests[i].expected);
        } else {
            printk("[wasm-test] %-20s PASS  %s(%u) = %llu\n",
                   wasm_tests[i].name,
                   wasm_tests[i].func,
                   wasm_tests[i].arg,
                   (unsigned long long)result);
            passed++;
        }
    }

    printk("[wasm-test] %d/%d passed\n", passed, total);
    return (passed == total) ? 0 : -1;
}
