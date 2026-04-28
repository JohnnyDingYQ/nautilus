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
#include "extra/fib32.wasm.h"

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
 * Math stubs — satisfy float opcode link-time references.
 * GCC builtins lower to inline SSE2 instructions; no libm needed.
 * fib32 is integer-only so these are never called at runtime.
 * __attribute__((weak)) lets a kernel-provided strong symbol win if present.
 * ----------------------------------------------------------------------- */
__attribute__((weak)) double copysign(double x, double y)  { return __builtin_copysign(x, y); }
__attribute__((weak)) float  copysignf(float x, float y)   { return __builtin_copysignf(x, y); }
__attribute__((weak)) float  fabsf(float x)                { return __builtin_fabsf(x); }

__attribute__((weak)) float sqrtf(float x)
{
    float r;
    __asm__ volatile ("sqrtss %1, %0" : "=x"(r) : "x"(x));
    return r;
}

__attribute__((weak)) double trunc(double x) { return (double)(long long)x; }
__attribute__((weak)) float  truncf(float x) { return (float)(long long)(double)x; }
__attribute__((weak)) double rint(double x)
{
    long long i = (long long)(x + (x >= 0.0 ? 0.5 : -0.5));
    return (double)i;
}
__attribute__((weak)) float  rintf(float x)  { return (float)rint((double)x); }
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

static M3Result link_nautilus_wasi(IM3Module module)
{
    M3Result res;

#define LINK(mod, name, sig, fn) \
    res = m3_LinkRawFunction(module, mod, name, sig, fn); \
    if (res && res != m3Err_functionLookupFailed) return res;

    LINK("wasi_snapshot_preview1", "fd_write",  "i(iiii)", &naut_wasi_fd_write)
    LINK("wasi_unstable",          "fd_write",  "i(iiii)", &naut_wasi_fd_write)
    LINK("wasi_snapshot_preview1", "proc_exit", "v(i)",    &naut_wasi_proc_exit)
    LINK("wasi_unstable",          "proc_exit", "v(i)",    &naut_wasi_proc_exit)

#undef LINK
    return m3Err_none;
}

/* -----------------------------------------------------------------------
 * Core runner
 * ----------------------------------------------------------------------- */

static int run_wasm(const uint8_t *wasm_bytes, uint32_t wasm_len,
                    const char *func_name, uint32_t arg)
{
    M3Result res;

    IM3Environment env = m3_NewEnvironment();
    if (!env) { printk("wasm: failed to create environment\n"); return -1; }

    IM3Runtime runtime = m3_NewRuntime(env, WASM_STACK_SIZE, NULL);
    if (!runtime) {
        printk("wasm: failed to create runtime\n");
        m3_FreeEnvironment(env);
        return -1;
    }

    IM3Module module;
    res = m3_ParseModule(env, &module, wasm_bytes, wasm_len);
    if (res) { printk("wasm: parse error: %s\n", res); goto cleanup; }

    res = m3_LoadModule(runtime, module);
    if (res) { printk("wasm: load error: %s\n", res); goto cleanup; }

    res = link_nautilus_wasi(module);
    if (res) { printk("wasm: wasi link error: %s\n", res); goto cleanup; }

    IM3Function func;
    res = m3_FindFunction(&func, runtime, func_name);
    if (res) {
        printk("wasm: function '%s' not found: %s\n", func_name, res);
        goto cleanup;
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

cleanup:
    m3_FreeRuntime(runtime);
    m3_FreeEnvironment(env);
    return res ? -1 : 0;
}

/* -----------------------------------------------------------------------
 * Shell command: "wasm fib <n>"
 * ----------------------------------------------------------------------- */

/* strcmp needs no libc — provide a minimal version to avoid pulling in string.h */
static int wasm_strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

static int handle_wasm_cmd(char *buf, void *priv)
{
    char subcmd[32] = {0};
    unsigned int arg = 10;

    /* Skip leading command token ("wasm"), then parse subcmd and optional n.
     * Avoids sscanf which resolves to __isoc99_sscanf (libc-only symbol). */
    const char *p = buf;
    while (*p && *p != ' ') p++;   /* skip "wasm" */
    while (*p == ' ') p++;          /* skip spaces */

    /* copy subcmd */
    int i = 0;
    while (*p && *p != ' ' && i < 31) subcmd[i++] = *p++;
    subcmd[i] = '\0';

    while (*p == ' ') p++;          /* skip spaces */
    if (*p >= '0' && *p <= '9') {
        arg = 0;
        while (*p >= '0' && *p <= '9') arg = arg * 10 + (*p++ - '0');
    }

    if (subcmd[0] == '\0' || wasm_strcmp(subcmd, "fib") == 0) {
        printk("wasm: running fib(%u) from embedded fib32.wasm\n", arg);
        run_wasm(fib32_wasm, fib32_wasm_len, "fib", arg);
    } else {
        printk("wasm: usage: wasm fib <n>\n");
    }

    return 0;
}

static struct shell_cmd_impl wasm_impl = {
    .cmd      = "wasm",
    .help_str = "wasm fib <n>  -- run embedded fib32 WASM module",
    .handler  = handle_wasm_cmd,
};
nk_register_shell_cmd(wasm_impl);
