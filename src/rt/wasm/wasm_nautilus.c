/*
 * wasm_nautilus.c - Wasm3 glue for Nautilus
 *
 * Include order matters:
 *   1. wasm_nautilus.h is force-included first (via Makefile -include).
 *      It defines malloc/free/etc. as object-like macros pointing at kernel
 *      functions, but does NOT pull in Nautilus type-redefining headers.
 *   2. wasm3.h (and transitively stdlib.h/stdio.h) defines real POSIX types.
 *   3. shell.h is NOT included — its nk_launch_shell declaration uses
 *      nk_thread_id_t which pulls in the full Nautilus type chain.  Instead
 *      we copy only struct shell_cmd_impl and nk_register_shell_cmd inline.
 */

#include "wasm3.h"
#include "m3_env.h"
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

/*
 * Explicit kernel symbol declarations.  wasm_nautilus.h provides these via
 * -include, but the kernel build's warning flags can cause the compiler to
 * treat them as implicit (returning int) if the force-include is shadowed.
 * Repeating them here after wasm3.h (which defines size_t via stdlib.h)
 * guarantees correct 64-bit pointer return types throughout this file.
 */
extern void *kmem_malloc(size_t size);
extern void *kmem_mallocz(size_t size);
extern void  kmem_free(void *addr);
extern int   printk(const char *fmt, ...);
extern uint64_t nk_sched_get_realtime(void);
extern void     nk_get_rand_bytes(uint8_t *buf, unsigned len);
extern int      nk_vc_getchar(void);
extern void     nk_yield(void);

/* -----------------------------------------------------------------------
 * PAL implementations declared in wasm_nautilus.h
 * ----------------------------------------------------------------------- */

void *naut_calloc_fn(size_t n, size_t size)
{
    return kmem_mallocz(n * size);
}

void naut_abort_fn(void)
{
    /* printk is declared in wasm_nautilus.h */
    printk("wasm3: abort() called\n");
    while (1) {}
    __builtin_unreachable();
}

/*
 * glibc's stdlib.h does "#undef calloc" before its own declaration, wiping
 * the object-like macro from wasm_nautilus.h.  Provide a real symbol so the
 * linker can resolve calls that slipped through after that undef.
 */
void *calloc(size_t n, size_t size)
{
    return kmem_mallocz(n * size);
}

/*
 * m3_env.c uses strtoul/strtoull to parse argv strings in m3_CallArgv.
 * The kernel has no libc, so provide minimal implementations.
 * Note that there are other implementations in the kernel.
 * Redundancy at this magnitude is acceptable.
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
 * Math stubs — GCC builtins / inline SSE2; no libm needed.
 * __attribute__((weak)) lets a kernel-provided strong symbol win if present.
 * ----------------------------------------------------------------------- */

/* --- float variants (f32) ---
 * libccompat.c has no f32 math stubs so weak symbols suffice here. */
__attribute__((weak)) float  copysignf(float x, float y)   { return __builtin_copysignf(x, y); }
__attribute__((weak)) float  fabsf(float x)                { return __builtin_fabsf(x); }
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

__attribute__((weak)) float  truncf(float x) { return (float)(long long)(double)x; }
__attribute__((weak)) float  rintf(float x)
{
    long long i = (long long)((double)x + ((double)x >= 0.0 ? 0.5 : -0.5));
    return (float)i;
}
__attribute__((weak)) float  floorf(float x)
{
    long long i = (long long)(double)x;
    return (float)((double)x < (double)i ? (double)(i - 1) : (double)i);
}
__attribute__((weak)) float  ceilf(float x)
{
    long long i = (long long)(double)x;
    return (float)((double)x > (double)i ? (double)(i + 1) : (double)i);
}

/* --- double variants (f64) ---
 * libccompat.c has strong stubs for fabs/ceil/floor/sqrt that return x
 * unchanged.  Use private symbols (declared in wasm_nautilus.h) so the
 * macros in that header redirect wasm3's calls here instead.
 * m3_math_utils.h re-applies those macros after <math.h>'s internal #undefs
 * so the tokens in m3_exec.h still expand to these functions at compile time. */
__attribute__((weak)) double copysign(double x, double y)  { return __builtin_copysign(x, y); }
double naut_fabs_fn(double x)  { return __builtin_fabs(x); }
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
__attribute__((weak)) double trunc(double x) { return (double)(long long)x; }
__attribute__((weak)) double rint(double x)
{
    long long i = (long long)(x + (x >= 0.0 ? 0.5 : -0.5));
    return (double)i;
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

/*
 * math.h may "#undef sqrtf" before its own declaration, wiping the macro from
 * wasm_nautilus.h.  libccompat has no sqrtf stub, so provide a real symbol as
 * a belt-and-suspenders fallback — same pattern as calloc above.
 * (sqrt/fabs/floor/ceil are NOT provided here because libccompat already has
 * strong definitions for those; the macro fix in m3_math_utils.h ensures wasm3
 * never reaches the symbol level for those names.)
 */
float sqrtf(float x) { return naut_sqrtf_fn(x); }

/* -----------------------------------------------------------------------
 * Fortification stubs — Ubuntu 24.04's GCC 13 spec file re-applies
 * _FORTIFY_SOURCE=2 after command-line flags, inlining __chk wrappers into
 * our object files.  Provide the missing __chk symbols here.
 * 
 * This is useful when building through a dockerfile
 *
 * memset/memcpy: use byte loops so we never call the fortified names.
 * snprintf/vsnprintf: alias the kernel's real symbol via __asm__ to bypass
 *   the stdio2.h inline fortified wrapper.
 * ----------------------------------------------------------------------- */

/* Alias kernel's vsnprintf symbol directly, skipping the fortified inline. */
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

/* -----------------------------------------------------------------------
 * Shell integration
 *
 * Copy only what we need from <nautilus/shell.h> to avoid pulling in the
 * full Nautilus header chain (which redefines POSIX types via libccompat.h).
 * shell.h itself has no #include directives, so this copy stays in sync.
 * ----------------------------------------------------------------------- */
struct shell_cmd_impl {
    char *cmd;
    char *help_str;
    int (*handler)(char *buf, void *priv);
};

#define nk_register_shell_cmd(cmd) \
    static struct shell_cmd_impl * _nk_cmd_##cmd \
    __attribute__((used)) \
    __attribute__((unused, __section__(".shell_cmds"), \
        aligned(sizeof(void*)))) \
    = &cmd

/* -----------------------------------------------------------------------
 * Filesystem forward declarations
 *
 * Same rationale as shell.h above: <nautilus/fs.h> includes naut_types.h
 * which redefines POSIX types and conflicts with wasm3's headers.
 * Forward-declare only the handful of symbols we need here.
 * ----------------------------------------------------------------------- */
struct nk_fs_open_file_state;
typedef struct nk_fs_open_file_state *nk_fs_fd_t;
#define FS_BAD_FD     ((nk_fs_fd_t)-1UL)
#define FS_FD_ERR(fd) ((fd) == FS_BAD_FD)

struct nk_fs_stat { uint64_t st_size; };

extern nk_fs_fd_t nk_fs_open(char *path, int flags, int mode);
extern int        nk_fs_fstat(nk_fs_fd_t fd, struct nk_fs_stat *st);
extern ssize_t    nk_fs_read(nk_fs_fd_t fd, void *buf, size_t len);
extern int        nk_fs_close(nk_fs_fd_t fd);

/* -----------------------------------------------------------------------
 * Scratch buffer — a fixed kernel-side staging area exposed to wasm modules
 * as "naut".scratch_write / "naut".scratch_read.  Used to test bidirectional
 * memory passing between wasm linear memory and the host.
 * ----------------------------------------------------------------------- */

#define WASM_SCRATCH_SIZE 256
static uint8_t  wasm_scratch[WASM_SCRATCH_SIZE];
static uint32_t wasm_scratch_len;

/*
 * scratch_write(ptr: i32, len: i32) -> i32
 * Copy up to WASM_SCRATCH_SIZE bytes from wasm linear memory into the
 * kernel scratch buffer.  Returns the number of bytes written.
 */
m3ApiRawFunction(naut_scratch_write)
{
    m3ApiReturnType (uint32_t)
    m3ApiGetArgMem  (const uint8_t *, ptr)
    m3ApiGetArg     (uint32_t, len)

    if (len > WASM_SCRATCH_SIZE) len = WASM_SCRATCH_SIZE;
    m3ApiCheckMem(ptr, len);
    __builtin_memcpy(wasm_scratch, ptr, len);
    wasm_scratch_len = len;
    m3ApiReturn(len);
}

/*
 * scratch_read(ptr: i32, max: i32) -> i32
 * Copy up to max bytes from the kernel scratch buffer into wasm linear
 * memory.  Returns the number of bytes copied.
 */
m3ApiRawFunction(naut_scratch_read)
{
    m3ApiReturnType (uint32_t)
    m3ApiGetArgMem  (uint8_t *, ptr)
    m3ApiGetArg     (uint32_t, max)

    uint32_t n = wasm_scratch_len < max ? wasm_scratch_len : max;
    m3ApiCheckMem(ptr, n);
    __builtin_memcpy(ptr, wasm_scratch, n);
    m3ApiReturn(n);
}

/* -----------------------------------------------------------------------
 * Minimal WASI stubs
 * ----------------------------------------------------------------------- */

#define WASM_STACK_SIZE (64 * 1024)

/*
 * fd_write(fd, iovs_ptr, iovs_len, nwritten_ptr) -> errno
 *
 * iov layout (WASM linear memory): { buf: i32, buf_len: i32 }
 * All fds redirect to printk.
 */
m3ApiRawFunction(naut_wasi_fd_write)
{
    m3ApiReturnType  (uint32_t)
    m3ApiGetArg      (uint32_t, fd)
    m3ApiGetArgMem   (uint32_t *, iovs)
    m3ApiGetArg      (uint32_t, iovs_len)
    m3ApiGetArgMem   (uint32_t *, nwritten)

    (void)fd;

    uint32_t total = 0;
    for (uint32_t i = 0; i < iovs_len; i++) {
        uint32_t buf_off = m3ApiReadMem32(&iovs[i * 2]);
        uint32_t buf_len = m3ApiReadMem32(&iovs[i * 2 + 1]);
        if (buf_len == 0) continue;
        const char *buf = (const char *)m3ApiOffsetToPtr(buf_off);
        m3ApiCheckMem(buf, buf_len);
        for (uint32_t j = 0; j < buf_len; j++)
            printk("%c", buf[j]);
        total += buf_len;
    }
    m3ApiWriteMem32(nwritten, total);
    m3ApiReturn(0);  /* __WASI_ERRNO_SUCCESS */
}

/* proc_exit: signal normal WASM module exit */
m3ApiRawFunction(naut_wasi_proc_exit)
{
    m3ApiGetArg(uint32_t, code)
    (void)code;
    m3ApiTrap(m3Err_trapExit);
}

/*
 * fd_fdstat_get(fd, stat_ptr) -> errno
 *
 * wasi_fdstat layout (24 bytes):
 *   u8  fs_filetype       (offset  0)  2 = character device
 *   u8  pad               (offset  1)
 *   u16 fs_flags          (offset  2)
 *   u8  pad[4]            (offset  4)
 *   u64 fs_rights_base    (offset  8)  all rights granted
 *   u64 fs_rights_inherit (offset 16)
 *
 * printf checks fs_rights_base & FD_WRITE before calling fd_write.
 * Grant all rights so every fd appears writable.
 */
m3ApiRawFunction(naut_wasi_fd_fdstat_get)
{
    m3ApiReturnType  (uint32_t)
    m3ApiGetArg      (uint32_t, fd)
    m3ApiGetArgMem   (uint8_t *, stat)

    (void)fd;
    m3ApiCheckMem(stat, 24);

    for (int i = 0; i < 24; i++) stat[i] = 0;
    stat[0] = 2;                        /* WASI_FILETYPE_CHARACTER_DEVICE */
    uint64_t all_rights = ~(uint64_t)0;
    __builtin_memcpy(stat + 8,  &all_rights, 8);
    __builtin_memcpy(stat + 16, &all_rights, 8);

    m3ApiReturn(0);
}

/*
 * environ_sizes_get(count_ptr, buf_size_ptr) -> errno
 * Report an empty environment so libc startup succeeds.
 */
m3ApiRawFunction(naut_wasi_environ_sizes_get)
{
    m3ApiReturnType  (uint32_t)
    m3ApiGetArgMem   (uint32_t *, count)
    m3ApiGetArgMem   (uint32_t *, buf_size)

    m3ApiCheckMem(count,    4);
    m3ApiCheckMem(buf_size, 4);
    m3ApiWriteMem32(count,    0);
    m3ApiWriteMem32(buf_size, 0);
    m3ApiReturn(0);
}

/* environ_get(environ_ptr, buf_ptr) -> errno */
m3ApiRawFunction(naut_wasi_environ_get)
{
    m3ApiReturnType  (uint32_t)
    m3ApiGetArg      (uint32_t, environ_ptr)
    m3ApiGetArg      (uint32_t, buf_ptr)

    (void)environ_ptr; (void)buf_ptr;
    m3ApiReturn(0);
}

/*
 * args_sizes_get(argc_ptr, buf_size_ptr) -> errno
 * Expose argv = ["wasm"] so main(argc, argv) gets argc=1.
 */
m3ApiRawFunction(naut_wasi_args_sizes_get)
{
    m3ApiReturnType  (uint32_t)
    m3ApiGetArgMem   (uint32_t *, argc)
    m3ApiGetArgMem   (uint32_t *, buf_size)

    m3ApiCheckMem(argc,     4);
    m3ApiCheckMem(buf_size, 4);
    m3ApiWriteMem32(argc,     1);   /* one argument */
    m3ApiWriteMem32(buf_size, 5);   /* "wasm\0" */
    m3ApiReturn(0);
}

/*
 * args_get(argv_ptr, buf_ptr) -> errno
 * Write argv[0] = "wasm" into the provided buffers.
 */
m3ApiRawFunction(naut_wasi_args_get)
{
    m3ApiReturnType  (uint32_t)
    m3ApiGetArgMem   (uint32_t *, argv)
    m3ApiGetArgMem   (uint8_t *,  buf)

    m3ApiCheckMem(argv, 4);
    m3ApiCheckMem(buf,  5);

    /* argv[0] points to buf */
    uint32_t buf_off = (uint32_t)m3ApiPtrToOffset(buf);
    m3ApiWriteMem32(argv, buf_off);

    buf[0] = 'w'; buf[1] = 'a'; buf[2] = 's'; buf[3] = 'm'; buf[4] = '\0';
    m3ApiReturn(0);
}

/*
 * clock_time_get(clock_id, precision, time_ptr) -> errno
 *
 * Both REALTIME (0) and MONOTONIC (1) map to nk_sched_get_realtime(), which
 * returns nanoseconds since boot.  The precision argument is advisory and
 * ignored.
 */
m3ApiRawFunction(naut_wasi_clock_time_get)
{
    m3ApiReturnType  (uint32_t)
    m3ApiGetArg      (uint32_t, clock_id)
    m3ApiGetArg      (uint64_t, precision)
    m3ApiGetArgMem   (uint64_t *, time_ptr)

    (void)clock_id;
    (void)precision;

    m3ApiCheckMem(time_ptr, 8);
    m3ApiWriteMem64(time_ptr, nk_sched_get_realtime());
    m3ApiReturn(0);
}

/*
 * random_get(buf_ptr, buf_len) -> errno
 */
m3ApiRawFunction(naut_wasi_random_get)
{
    m3ApiReturnType  (uint32_t)
    m3ApiGetArgMem   (uint8_t *, buf)
    m3ApiGetArg      (uint32_t, buf_len)

    m3ApiCheckMem(buf, buf_len);
    nk_get_rand_bytes(buf, buf_len);
    m3ApiReturn(0);
}

/*
 * fd_read(fd, iovs_ptr, iovs_len, nread_ptr) -> errno
 *
 * Only fd=0 (stdin) is supported; other fds return EBADF (8).
 * Each iovec is { buf_offset: u32, buf_len: u32 }.
 */
m3ApiRawFunction(naut_wasi_fd_read)
{
    m3ApiReturnType  (uint32_t)
    m3ApiGetArg      (uint32_t, fd)
    m3ApiGetArgMem   (uint32_t *, iovs)
    m3ApiGetArg      (uint32_t, iovs_len)
    m3ApiGetArgMem   (uint32_t *, nread)

    if (fd != 0) m3ApiReturn(8);  /* __WASI_ERRNO_BADF */

    uint32_t total = 0;
    for (uint32_t i = 0; i < iovs_len; i++) {
        uint32_t buf_off = m3ApiReadMem32(&iovs[i * 2]);
        uint32_t buf_len = m3ApiReadMem32(&iovs[i * 2 + 1]);
        if (buf_len == 0) continue;
        uint8_t *buf = (uint8_t *)m3ApiOffsetToPtr(buf_off);
        m3ApiCheckMem(buf, buf_len);
        for (uint32_t j = 0; j < buf_len; j++) {
            int c = nk_vc_getchar();
            if (c < 0) { m3ApiWriteMem32(nread, total); m3ApiReturn(0); }
            buf[j] = (uint8_t)c;
            total++;
        }
    }
    m3ApiWriteMem32(nread, total);
    m3ApiReturn(0);
}

/*
 * sched_yield() -> errno
 *
 * Yields the current Nautilus thread to the scheduler.
 */
m3ApiRawFunction(naut_wasi_sched_yield)
{
    m3ApiReturnType(uint32_t)
    nk_yield();
    m3ApiReturn(0);
}

/*
 * clock_res_get(clock_id, resolution_ptr) -> errno
 *
 * Reports 1 ns resolution for all clock IDs; matches nk_sched_get_realtime
 * which returns nanoseconds.  Writes u64 to resolution_ptr.
 */
m3ApiRawFunction(naut_wasi_clock_res_get)
{
    m3ApiReturnType  (uint32_t)
    m3ApiGetArg      (uint32_t, clock_id)
    m3ApiGetArgMem   (uint64_t *, res_ptr)

    (void)clock_id;
    m3ApiCheckMem(res_ptr, 8);
    m3ApiWriteMem64(res_ptr, 1ULL);  /* 1 nanosecond */
    m3ApiReturn(0);
}

/*
 * fd_close(fd) -> errno
 *
 * No-op: we have no fd table, so every close succeeds.
 */
m3ApiRawFunction(naut_wasi_fd_close)
{
    m3ApiReturnType(uint32_t)
    m3ApiGetArg    (uint32_t, fd)
    (void)fd;
    m3ApiReturn(0);
}

/*
 * fd_seek(fd, offset, whence, newoffset_ptr) -> errno
 *
 * All our fds are character devices (non-seekable).
 * Return ESPIPE (29) so callers fall back to non-seekable behaviour.
 */
m3ApiRawFunction(naut_wasi_fd_seek)
{
    m3ApiReturnType  (uint32_t)
    m3ApiGetArg      (uint32_t, fd)
    m3ApiGetArg      (uint64_t, offset)
    m3ApiGetArg      (uint32_t, whence)
    m3ApiGetArgMem   (uint64_t *, newoffset_ptr)

    (void)fd; (void)offset; (void)whence; (void)newoffset_ptr;
    m3ApiReturn(29);  /* __WASI_ERRNO_SPIPE */
}

static M3Result link_nautilus_wasi(IM3Module module)
{
    M3Result res;

#define LINK(mod, name, sig, fn) \
    res = m3_LinkRawFunction(module, mod, name, sig, fn); \
    if (res && res != m3Err_functionLookupFailed) return res;

    LINK("wasi_snapshot_preview1", "fd_write",           "i(iiii)", &naut_wasi_fd_write)
    LINK("wasi_unstable",          "fd_write",           "i(iiii)", &naut_wasi_fd_write)
    LINK("wasi_snapshot_preview1", "proc_exit",          "v(i)",    &naut_wasi_proc_exit)
    LINK("wasi_unstable",          "proc_exit",          "v(i)",    &naut_wasi_proc_exit)
    LINK("wasi_snapshot_preview1", "fd_fdstat_get",      "i(ii)",   &naut_wasi_fd_fdstat_get)
    LINK("wasi_unstable",          "fd_fdstat_get",      "i(ii)",   &naut_wasi_fd_fdstat_get)
    LINK("wasi_snapshot_preview1", "environ_sizes_get",  "i(ii)",   &naut_wasi_environ_sizes_get)
    LINK("wasi_unstable",          "environ_sizes_get",  "i(ii)",   &naut_wasi_environ_sizes_get)
    LINK("wasi_snapshot_preview1", "environ_get",        "i(ii)",   &naut_wasi_environ_get)
    LINK("wasi_unstable",          "environ_get",        "i(ii)",   &naut_wasi_environ_get)
    LINK("wasi_snapshot_preview1", "args_sizes_get",     "i(ii)",   &naut_wasi_args_sizes_get)
    LINK("wasi_unstable",          "args_sizes_get",     "i(ii)",   &naut_wasi_args_sizes_get)
    LINK("wasi_snapshot_preview1", "args_get",           "i(ii)",   &naut_wasi_args_get)
    LINK("wasi_unstable",          "args_get",           "i(ii)",   &naut_wasi_args_get)
    LINK("wasi_snapshot_preview1", "clock_time_get",     "i(iIi)",  &naut_wasi_clock_time_get)
    LINK("wasi_unstable",          "clock_time_get",     "i(iIi)",  &naut_wasi_clock_time_get)
    LINK("wasi_snapshot_preview1", "random_get",         "i(ii)",   &naut_wasi_random_get)
    LINK("wasi_unstable",          "random_get",         "i(ii)",   &naut_wasi_random_get)
    LINK("wasi_snapshot_preview1", "fd_read",            "i(iiii)",  &naut_wasi_fd_read)
    LINK("wasi_unstable",          "fd_read",            "i(iiii)",  &naut_wasi_fd_read)
    LINK("wasi_snapshot_preview1", "sched_yield",        "i()",      &naut_wasi_sched_yield)
    LINK("wasi_unstable",          "sched_yield",        "i()",      &naut_wasi_sched_yield)
    LINK("wasi_snapshot_preview1", "clock_res_get",      "i(ii)",    &naut_wasi_clock_res_get)
    LINK("wasi_unstable",          "clock_res_get",      "i(ii)",    &naut_wasi_clock_res_get)
    LINK("wasi_snapshot_preview1", "fd_close",           "i(i)",     &naut_wasi_fd_close)
    LINK("wasi_unstable",          "fd_close",           "i(i)",     &naut_wasi_fd_close)
    LINK("wasi_snapshot_preview1", "fd_seek",            "i(iIii)",  &naut_wasi_fd_seek)
    LINK("wasi_unstable",          "fd_seek",            "i(iIii)",  &naut_wasi_fd_seek)

    LINK("naut", "scratch_write", "i(ii)", &naut_scratch_write)
    LINK("naut", "scratch_read",  "i(ii)", &naut_scratch_read)

#undef LINK
    return m3Err_none;
}

/* -----------------------------------------------------------------------
 * Shared setup/teardown helpers
 * ----------------------------------------------------------------------- */

/*
 * Parse, load, and WASI-link a module into a freshly created runtime.
 * On success, *env_out and *runtime_out are live and must be freed by the
 * caller via wasm_teardown().  On failure both are freed internally and
 * the error string is returned.
 */
static M3Result wasm_setup(const uint8_t *bytes, uint32_t len,
                            IM3Environment *env_out, IM3Runtime *runtime_out)
{
    *env_out     = NULL;
    *runtime_out = NULL;

    IM3Environment env = m3_NewEnvironment();
    if (!env) { printk("wasm: failed to create environment\n"); return "env alloc"; }

    IM3Runtime runtime = m3_NewRuntime(env, WASM_STACK_SIZE, NULL);
    if (!runtime) {
        printk("wasm: failed to create runtime\n");
        m3_FreeEnvironment(env);
        return "runtime alloc";
    }

    IM3Module module;
    M3Result res = m3_ParseModule(env, &module, bytes, len);
    if (res) { printk("wasm: parse error: %s\n", res); goto fail; }

    res = m3_LoadModule(runtime, module);
    if (res) { printk("wasm: load error: %s\n", res); goto fail; }

    res = link_nautilus_wasi(module);
    if (res) { printk("wasm: wasi link error: %s\n", res); goto fail; }

    *env_out     = env;
    *runtime_out = runtime;
    return m3Err_none;

fail:
    m3_FreeRuntime(runtime);
    m3_FreeEnvironment(env);
    return res;
}

static void wasm_teardown(IM3Environment env, IM3Runtime runtime)
{
    m3_FreeRuntime(runtime);
    m3_FreeEnvironment(env);
}

/* -----------------------------------------------------------------------
 * Core runner — named function with one uint32 argument, prints result.
 * ----------------------------------------------------------------------- */

static int run_wasm(const uint8_t *wasm_bytes, uint32_t wasm_len,
                    const char *func_name, uint32_t arg)
{
    IM3Environment env;
    IM3Runtime runtime;
    M3Result res = wasm_setup(wasm_bytes, wasm_len, &env, &runtime);
    if (res) return -1;

    IM3Function func;
    res = m3_FindFunction(&func, runtime, func_name);
    if (res) {
        printk("wasm: function '%s' not found: %s\n", func_name, res);
        goto done;
    }

    char arg_str[32];
    snprintf(arg_str, sizeof(arg_str), "%u", arg);
    const char *args[1] = { arg_str };
    res = m3_CallArgv(func, 1, args);

    if (res == m3Err_trapExit)
        res = m3Err_none;

    if (res) {
        M3ErrorInfo info;
        m3_GetErrorInfo(runtime, &info);
        printk("wasm: runtime error: %s (%s)\n", res, info.message);
    } else {
        uint64_t ret = 0;
        m3_GetResultsV(func, &ret);
        printk("wasm: %s(%u) = %llu\n", func_name, arg,
               (unsigned long long)ret);
    }

done:
    wasm_teardown(env, runtime);
    return res ? -1 : 0;
}

/* -----------------------------------------------------------------------
 * File-based runner: load a .wasm binary from the filesystem and call
 * its entry point (_start for WASI programs, main as fallback).
 * ----------------------------------------------------------------------- */

static int run_wasm_file(const char *path)
{
    struct nk_fs_stat st;
    printk("wasm: opening '%s'\n", path);
    nk_fs_fd_t fd = nk_fs_open((char *)path, 1 /* O_RDONLY */, 0);
    if (FS_FD_ERR(fd)) {
        printk("wasm: cannot open '%s'\n", path);
        return -1;
    }

    if (nk_fs_fstat(fd, &st) < 0 || st.st_size == 0) {
        printk("wasm: cannot stat '%s'\n", path);
        nk_fs_close(fd);
        return -1;
    }

    uint8_t *buf = kmem_malloc((size_t)st.st_size);
    if (!buf) {
        printk("wasm: out of memory allocating %llu bytes\n",
               (unsigned long long)st.st_size);
        nk_fs_close(fd);
        return -1;
    }

    ssize_t n = nk_fs_read(fd, buf, (size_t)st.st_size);
    nk_fs_close(fd);
    if (n != (ssize_t)st.st_size) {
        printk("wasm: read error on '%s' (got %ld of %llu bytes)\n",
               path, (long)n, (unsigned long long)st.st_size);
        kmem_free(buf);
        return -1;
    }

    IM3Environment env;
    IM3Runtime runtime;
    M3Result res = wasm_setup(buf, (uint32_t)n, &env, &runtime);
    if (res) { kmem_free(buf); return -1; }

    /* Try standard WASI entry point first, then fall back to main. */
    IM3Function func;
    res = m3_FindFunction(&func, runtime, "_start");
    if (res) res = m3_FindFunction(&func, runtime, "main");
    if (res) {
        printk("wasm: no '_start' or 'main' export found in '%s'\n", path);
        goto done;
    }

    res = m3_CallArgv(func, 0, NULL);
    if (res == m3Err_trapExit)
        res = m3Err_none;

    if (res) {
        M3ErrorInfo info;
        m3_GetErrorInfo(runtime, &info);
        printk("wasm: runtime error: %s (%s)\n", res, info.message);
    }

done:
    wasm_teardown(env, runtime);
    kmem_free(buf);
    return res ? -1 : 0;
}

/* -----------------------------------------------------------------------
 * Test suite: "wasm test"
 *
 * func names vary per entry; sqrt_scaled returns floor(sqrt(n)*1e6) as i64
 * so float results can be compared exactly without epsilon logic.
 *
 *   fib32         "fib"          i32 recursive,  arg=24 → 46368
 *   fib32_tail    "fib"          i32 tail-call,  arg=24 → 46368
 *   fib64         "fib"          i64 recursive,  arg=24 → 46368
 *   sqrt(2)       "sqrt_scaled"  f64.sqrt + mul, arg=2  → 1414213
 *   sqrt(3)       "sqrt_scaled"                  arg=3  → 1732050
 *   sqrt(5)       "sqrt_scaled"                  arg=5  → 2236067
 *   sqrt(7)       "sqrt_scaled"                  arg=7  → 2645751
 *   scratch       "scratch_roundtrip" round-trip memory passing → 0
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
    { "sqrt(7)",    "sqrt_scaled",      sqrt_test_wasm,    sizeof(sqrt_test_wasm),    7, 2645751ULL },
    { "scratch",   "scratch_roundtrip",scratch_test_wasm,  sizeof(scratch_test_wasm),  0, 0ULL },
    { "clock",     "clock_test",       clock_test_wasm,    sizeof(clock_test_wasm),    0, 0ULL },
    { "random",    "random_test",      random_test_wasm,   sizeof(random_test_wasm),   0, 0ULL },
    { "fd_read",    "fd_read_test",     fd_read_test_wasm,    sizeof(fd_read_test_wasm),    0, 0ULL },
    { "sched_yield","sched_yield_test", sched_yield_test_wasm,sizeof(sched_yield_test_wasm),0, 0ULL },
    { "clock_res",  "clock_res_test",   clock_res_test_wasm,  sizeof(clock_res_test_wasm),  0, 0ULL },
    { "fd_close",   "fd_close_test",    fd_close_test_wasm,   sizeof(fd_close_test_wasm),   0, 0ULL },
    { "fd_seek",    "fd_seek_test",     fd_seek_test_wasm,    sizeof(fd_seek_test_wasm),    0, 0ULL },
    { NULL, NULL, NULL, 0, 0, 0 }
};

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

static int handle_wasm_test(void)
{
    int passed = 0, total = 0;

    for (int i = 0; wasm_tests[i].name; i++) {
        total++;
        uint64_t result = 0;
        int err = wasm_eval(wasm_tests[i].bytes, wasm_tests[i].len,
                            wasm_tests[i].func, wasm_tests[i].arg, &result);
        if (err) {
            printk("[wasm-test] %-12s FAIL (setup error)\n",
                   wasm_tests[i].name);
        } else if (result != wasm_tests[i].expected) {
            printk("[wasm-test] %-12s FAIL (got %llu, expected %llu)\n",
                   wasm_tests[i].name,
                   (unsigned long long)result,
                   (unsigned long long)wasm_tests[i].expected);
        } else {
            printk("[wasm-test] %-12s PASS  %s(%u) = %llu\n",
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

/* -----------------------------------------------------------------------
 * Shell command: "wasm fib <n>" / "wasm test" / "wasm <path>"
 * ----------------------------------------------------------------------- */

/* strcmp needs no libc — provide a minimal version to avoid pulling in string.h */
static int wasm_strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

static int handle_wasm_cmd(char *buf, void *priv)
{
    /* Skip the leading "wasm" token. */
    const char *p = buf;
    while (*p && *p != ' ') p++;
    while (*p == ' ') p++;

    /* p now points at the first argument (or '\0' if none). */
    const char *arg_start = p;

    /* Peek at first token to distinguish "fib" from a filesystem path. */
    char subcmd[8] = {0};
    int i = 0;
    while (*p && *p != ' ' && i < 7) subcmd[i++] = *p++;
    subcmd[i] = '\0';

    if (wasm_strcmp(subcmd, "test") == 0) {
        handle_wasm_test();
    } else if (subcmd[0] == '\0' || wasm_strcmp(subcmd, "fib") == 0) {
        /* "wasm [fib [n]]" — run embedded fibonacci module. */
        unsigned int arg = 10;
        while (*p == ' ') p++;
        if (*p >= '0' && *p <= '9') {
            arg = 0;
            while (*p >= '0' && *p <= '9') arg = arg * 10 + (*p++ - '0');
        }
        printk("wasm: running fib(%u) from embedded fib32.wasm\n", arg);
        run_wasm(fib32_wasm, fib32_wasm_len, "fib", arg);
    } else {
        /* Any other token is treated as a filesystem path.
         * Copy arg_start into a local buffer, stopping at whitespace, so that
         * the full path (potentially longer than subcmd's 8-char peek) is used
         * and any trailing spaces/newlines from the raw command line are stripped. */
        char path[256] = {0};
        const char *s = arg_start;
        int j = 0;
        while (*s && *s != ' ' && *s != '\t' && *s != '\n' && j < 255)
            path[j++] = *s++;
        path[j] = '\0';
        run_wasm_file(path);
    }

    return 0;
}

static struct shell_cmd_impl wasm_impl = {
    .cmd      = "wasm",
    .help_str = "wasm test  -- run embedded test suite; wasm fib <n>  -- run fib32; wasm <path>  -- run WASM file",
    .handler  = handle_wasm_cmd,
};
nk_register_shell_cmd(wasm_impl);
