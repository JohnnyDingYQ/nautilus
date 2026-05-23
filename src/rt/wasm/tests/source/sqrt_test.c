/*
 * sqrt_test.c - float opcode test for wasm3-on-Nautilus
 *
 * Exports sqrt_scaled(n) = (i64)floor(sqrt((double)n) * 1_000_000).
 * Scaling to i64 avoids floating-point equality issues in the test framework.
 *
 * Compiled with -nostdlib / --no-entry so the module has no WASI imports
 * and no _start — it is called directly as a named export.
 *
 * __builtin_sqrt() emits the f64.sqrt wasm opcode at -O2, which exercises
 * the sqrtsd SSE2 stub in wasm_nautilus.c.
 */

__attribute__((visibility("default")))
__attribute__((export_name("sqrt_scaled")))
long long sqrt_scaled(unsigned int n)
{
    double r = __builtin_sqrt((double)n);
    return (long long)(r * 1000000.0);
}
