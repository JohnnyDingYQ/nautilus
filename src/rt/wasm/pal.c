/*
 * pal.c - Platform Abstraction Layer implementations for Wasm3 on Nautilus.
 *
 * Provides the libc symbols wasm3 expects (calloc, abort, strtoul, math
 * functions) and the GCC fortification stubs injected by Ubuntu 24.04's
 * GCC 13 spec file.  None of these touch the wasm3 interpreter or WASI
 * — they exist purely to satisfy the kernel's no-libc environment.
 *
 * pal.h is force-included by the Makefile, providing all kernel externs
 * and the allocator/math macros.
 */

/*
 * Include explicitly: pal.c does not pull in source/wasm3.h (and therefore
 * not stdlib.h/stddef.h), so size_t would be undefined even with pal.h
 * force-included.  pal.h itself needs size_t to write its extern declarations,
 * so it includes <stddef.h> — but only after the force-include is processed,
 * which happens before pal.c's own content.  Adding it here makes pal.c
 * self-contained regardless of force-include ordering.
 */
#include <stddef.h>

extern int printk(const char *fmt, ...);
extern void *kmem_mallocz(size_t size);

/* -----------------------------------------------------------------------
 * Allocator and runtime stubs
 * ----------------------------------------------------------------------- */

void *naut_calloc_fn(size_t n, size_t size)
{
    return kmem_mallocz(n * size);
}

void naut_abort_fn(void)
{
    printk("wasm3: abort() called\n");
    while (1) {}
    __builtin_unreachable();
}

/* Real abort symbol — fallback for any TU where pal.h's macro is wiped. */
#undef abort
void __attribute__((noreturn)) abort(void)
{
    naut_abort_fn();
    __builtin_unreachable();
}

/* Software popcount — used when the target CPU does not expose hardware popcnt.
 * The integration branch compiles without -mpopcnt so GCC emits calls to this
 * GCC runtime helper for the wasm3 i32.popcnt / i64.popcnt opcodes. */
int __popcountdi2(unsigned long long a)
{
    a = a - ((a >> 1) & 0x5555555555555555ULL);
    a = (a & 0x3333333333333333ULL) + ((a >> 2) & 0x3333333333333333ULL);
    a = (a + (a >> 4)) & 0x0f0f0f0f0f0f0f0fULL;
    return (int)((a * 0x0101010101010101ULL) >> 56);
}

/*
 * pal.h macros calloc → naut_calloc_fn.  Files that include stdlib.h get
 * that macro wiped by stdlib.h's own "#undef calloc" before its declaration,
 * leaving calls unresolved.  Provide the real symbol here for those cases.
 * Undef first so the macro doesn't silently rename this definition too.
 */
#undef calloc
void *calloc(size_t n, size_t size)
{
    return kmem_mallocz(n * size);
}

/*
 * m3_env.c uses strtoul/strtoull to parse argv strings in m3_CallArgv.
 * The kernel has no libc, so provide minimal implementations.
 */
unsigned long strtoul(const char *s, char **endptr, int base)
{
    unsigned long val = 0;
    if (base == 0 || base == 10) {
        while (*s >= '0' && *s <= '9')
            val = val * 10 + (*s++ - '0');
    }
    if (endptr) *endptr = (char *)s;
    return val;
}

unsigned long long strtoull(const char *s, char **endptr, int base)
{
    unsigned long long val = 0;
    if (base == 0 || base == 10) {
        while (*s >= '0' && *s <= '9')
            val = val * 10 + (*s++ - '0');
    }
    if (endptr) *endptr = (char *)s;
    return val;
}

/* -----------------------------------------------------------------------
 * Math stubs — GCC builtins / inline asm; no libm needed.
 * __attribute__((weak)) lets a kernel-provided strong symbol win if present.
 * ----------------------------------------------------------------------- */

/* --- float variants (f32) ---
 * libccompat.c has no f32 stubs so weak symbols suffice here. */
__attribute__((weak)) float copysignf(float x, float y) { return __builtin_copysignf(x, y); }
__attribute__((weak)) float fabsf(float x)               { return __builtin_fabsf(x); }
__attribute__((weak)) float truncf(float x) { return (float)(long long)(double)x; }
__attribute__((weak)) float rintf(float x)
{
    long long i = (long long)((double)x + ((double)x >= 0.0 ? 0.5 : -0.5));
    return (float)i;
}
__attribute__((weak)) float floorf(float x)
{
    long long i = (long long)(double)x;
    return (float)((double)x < (double)i ? (double)(i - 1) : (double)i);
}
__attribute__((weak)) float ceilf(float x)
{
    long long i = (long long)(double)x;
    return (float)((double)x > (double)i ? (double)(i + 1) : (double)i);
}

float naut_sqrtf_fn(float x)
{
    float r;
    /*
     * Inline asm bypasses libccompat.c's strong sqrtf stub entirely —
     * the instruction is emitted directly without going through the linker.
     * x86_64: sqrtss (SSE2).  ARM64: fsqrt single-precision.
     * Generic fallback uses __builtin_sqrtf; safe on targets where
     * libccompat does not define a conflicting strong symbol.
     * NOTE: ARM64 path SHOULD work but is untested — Nautilus ARM64 support
     * is a work-in-progress.
     */
#if defined(__x86_64__) || defined(__i386__)
    __asm__ volatile ("sqrtss %1, %0" : "=x"(r) : "x"(x));
#elif defined(__aarch64__)
    __asm__ volatile ("fsqrt %s0, %s1" : "=w"(r) : "w"(x));
#else
    r = __builtin_sqrtf(x);
#endif
    return r;
}

/*
 * Same pattern as calloc above: undef before defining so pal.h's
 * sqrtf → naut_sqrtf_fn macro doesn't rename this into a duplicate.
 */
#undef sqrtf
float sqrtf(float x) { return naut_sqrtf_fn(x); }

/* --- double variants (f64) ---
 * libccompat.c has strong identity stubs for fabs/ceil/floor/sqrt.
 * Use private naut_*_fn names so pal.h's macros route wasm3 here. */
__attribute__((weak)) double copysign(double x, double y) { return __builtin_copysign(x, y); }
__attribute__((weak)) double trunc(double x) { return (double)(long long)x; }
__attribute__((weak)) double rint(double x)
{
    long long i = (long long)(x + (x >= 0.0 ? 0.5 : -0.5));
    return (double)i;
}

double naut_fabs_fn(double x) { return __builtin_fabs(x); }

double naut_sqrt_fn(double x)
{
    double r;
    /*
     * Same bypass strategy as naut_sqrtf_fn above.
     * x86_64: sqrtsd (SSE2).  ARM64: fsqrt double-precision.
     * NOTE: ARM64 path SHOULD work but is untested — Nautilus ARM64 support
     * is a work-in-progress.
     */
#if defined(__x86_64__) || defined(__i386__)
    __asm__ volatile ("sqrtsd %1, %0" : "=x"(r) : "x"(x));
#elif defined(__aarch64__)
    __asm__ volatile ("fsqrt %d0, %d1" : "=w"(r) : "w"(x));
#else
    r = __builtin_sqrt(x);
#endif
    return r;
}

double naut_floor_fn(double x)
{
    long long i = (long long)x;
    return (double)x < (double)i ? (double)(i - 1) : (double)i;
}

double naut_ceil_fn(double x)
{
    long long i = (long long)x;
    return (double)x > (double)i ? (double)(i + 1) : (double)i;
}

/* -----------------------------------------------------------------------
 * Fortification stubs — Ubuntu 24.04's GCC 13 spec file re-applies
 * _FORTIFY_SOURCE=2 after command-line flags, inlining __chk wrappers into
 * our object files.  Provide the missing __chk symbols here.
 *
 * memset/memcpy: byte loops so we never call the fortified names.
 * snprintf/vsnprintf: alias the kernel's real symbol via __asm__ to bypass
 *   the stdio2.h inline fortified wrapper.
 * ----------------------------------------------------------------------- */

extern int kernel_vsnprintf_raw(char *buf, size_t sz, const char *fmt,
                                __builtin_va_list ap)
    __asm__("vsnprintf");

__attribute__((weak))
void *__memset_chk(void *dst, int c, size_t n, size_t dstlen)
{
    unsigned char *p = dst;
    while (n--) *p++ = (unsigned char)c;
    return dst;
}

__attribute__((weak))
void *__memcpy_chk(void *dst, const void *src, size_t n, size_t dstlen)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    while (n--) *d++ = *s++;
    return dst;
}

__attribute__((weak))
int __vsnprintf_chk(char *buf, size_t size, int flags, size_t slen,
                    const char *fmt, __builtin_va_list ap)
{
    return kernel_vsnprintf_raw(buf, size, fmt, ap);
}

__attribute__((weak))
int __snprintf_chk(char *buf, size_t size, int flags, size_t slen,
                   const char *fmt, ...)
{
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    int r = kernel_vsnprintf_raw(buf, size, fmt, ap);
    __builtin_va_end(ap);
    return r;
}
