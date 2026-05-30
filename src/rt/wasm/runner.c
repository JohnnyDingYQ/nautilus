/*
 * runner.c - Wasm3 module lifecycle and execution for Nautilus.
 *
 * Owns wasm_setup/wasm_teardown (parse + WASI-link a module), run_wasm
 * (in-memory call with one u32 arg), run_wasm_file (filesystem loader),
 * and run_wasm_fib (embedded fibonacci demo).
 *
 * pal.h is force-included by the Makefile, providing the allocator macros
 * and kernel extern declarations.
 */

#include "source/wasm3.h"
#include "source/m3_env.h"
#include "wasi.h"
#include "runner.h"

/*
 * Repeated explicitly after wasm3.h's stdlib chain to ensure correct
 * 64-bit signatures; GCC 13 may warn if it sees the call before the
 * force-included pal.h declaration is reprocessed.
 */
extern void *kmem_malloc(size_t size);
extern void  kmem_free(void *addr);
extern int   printk(const char *fmt, ...);

/* -----------------------------------------------------------------------
 * Filesystem forward declarations
 *
 * <nautilus/fs.h> includes naut_types.h which redefines POSIX types and
 * conflicts with wasm3's headers.  Forward-declare only what we need.
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

/* fib32_wasm is defined in tests.c (via tests/fib32.wasm.h). */
extern unsigned char fib32_wasm[];
extern unsigned int  fib32_wasm_len;

#define WASM_STACK_SIZE (64 * 1024)

/* -----------------------------------------------------------------------
 * Lifecycle helpers
 * ----------------------------------------------------------------------- */

M3Result wasm_setup(const uint8_t *bytes, uint32_t len,
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

void wasm_teardown(IM3Environment env, IM3Runtime runtime)
{
    m3_FreeRuntime(runtime);
    m3_FreeEnvironment(env);
}

/* -----------------------------------------------------------------------
 * Runners
 * ----------------------------------------------------------------------- */

int run_wasm(const uint8_t *wasm_bytes, uint32_t wasm_len,
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

int run_wasm_file(const char *path)
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

void run_wasm_fib(unsigned int n)
{
    run_wasm(fib32_wasm, fib32_wasm_len, "fib", n);
}
