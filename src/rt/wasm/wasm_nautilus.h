/*
 * wasm_nautilus.h - Platform Abstraction Layer for Wasm3 on Nautilus
 *
 * Force-included into every Wasm3 compilation unit via -include in the Makefile.
 *
 * DESIGN CONSTRAINT: Do NOT include any Nautilus header that redefines standard
 * C types (libccompat.h redefines FILE, off_t, memset, locale_t, abs, ...).
 * Wasm3's own headers pull in <stdlib.h> and <stdio.h> which define those types
 * correctly; mixing in Nautilus's redefinitions causes fatal conflicts.
 *
 * Instead we use <stddef.h>/<stdint.h> (from the compiler's own internal headers,
 * which define only size_t, NULL, uintN_t — never conflicting with glibc) and
 * forward-declare the exact kernel symbols we need.
 *
 * OBJECT-LIKE MACROS for allocators: when stdlib.h processes its own declarations
 * such as "extern void *malloc(size_t)", the token 'malloc' is an identifier
 * followed by '(' so a function-like macro would expand the argument list and
 * produce garbage. An object-like macro just renames the identifier, so
 * "extern void *malloc(size_t)" becomes "extern void *kmem_malloc(size_t)" —
 * a valid re-declaration of the kernel function.
 */

#ifndef WASM_NAUTILUS_H
#define WASM_NAUTILUS_H

/* Safe: these live in the compiler's own include directory, never in /usr/include.
 * They define only size_t, NULL, uintN_t, bool — no FILE, off_t, etc. */
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* -----------------------------------------------------------------------
 * Forward-declare kernel symbols with their actual signatures.
 * Resolved at link time against the Nautilus kernel; no header needed.
 * ----------------------------------------------------------------------- */
extern void *kmem_malloc(size_t size);
extern void *kmem_mallocz(size_t size);   /* zeroing malloc */
extern void *kmem_realloc(void *ptr, size_t size);
extern void  kmem_free(void *addr);
extern int   printk(const char *fmt, ...);

/* calloc(n, s) replacement; implemented in wasm_nautilus.c */
extern void *naut_calloc_fn(size_t n, size_t size);
/* abort() replacement; implemented in wasm_nautilus.c */
extern void  naut_abort_fn(void) __attribute__((noreturn));

/* -----------------------------------------------------------------------
 * Object-like macros — rename stdlib.h's allocator identifiers to kernel
 * equivalents at compile time.  The downstream .c files call kmem_malloc
 * etc. directly after preprocessing; no libc symbols appear in the object.
 * ----------------------------------------------------------------------- */
#define malloc  kmem_malloc
#define calloc  naut_calloc_fn
#define realloc kmem_realloc
#define free    kmem_free
#define abort   naut_abort_fn

/* fprintf(stream, fmt, ...) → drop the FILE* and call printk.
 * Function-like here is fine: all fprintf calls we care about have
 * a stream argument to drop, and stdio.h's own declaration of fprintf
 * expands safely into a re-declaration of printk. */
#define fprintf(f, ...) printk(__VA_ARGS__)

/* -----------------------------------------------------------------------
 * sqrt/sqrtf — libccompat.c provides strong sqrt/sqrtf stubs that return
 * their input unchanged (kernel placeholder).  Redirect to our own SSE2
 * implementations so wasm3's f32.sqrt / f64.sqrt opcodes compute real roots.
 * Object-like macros: the stdlib.h declaration "extern double sqrt(double)"
 * becomes "extern double naut_sqrt_fn(double)" — a valid redeclaration.
 * ----------------------------------------------------------------------- */
extern double naut_sqrt_fn(double x);
extern float  naut_sqrtf_fn(float x);
extern double naut_fabs_fn(double x);
extern double naut_ceil_fn(double x);
extern double naut_floor_fn(double x);
#define sqrt  naut_sqrt_fn
#define sqrtf naut_sqrtf_fn
#define fabs  naut_fabs_fn
#define ceil  naut_ceil_fn
#define floor naut_floor_fn

/* -----------------------------------------------------------------------
 * Float support — m3_math_utils.h uses isnan/signbit/NAN from <math.h>
 * which is unavailable in the kernel.  Map to GCC builtins instead.
 * ----------------------------------------------------------------------- */
#ifndef isnan
#  define isnan(x)   __builtin_isnan(x)
#endif
#ifndef signbit
#  define signbit(x) __builtin_signbit(x)
#endif
#ifndef NAN
#  define NAN        __builtin_nanf("")
#endif

#endif /* WASM_NAUTILUS_H */
