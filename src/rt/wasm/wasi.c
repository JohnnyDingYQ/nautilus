/*
 * wasi.c - WASI host function implementations for Wasm3 on Nautilus.
 *
 * Implements all wasi_snapshot_preview1 host imports plus the custom
 * naut::scratch_write / naut::scratch_read bindings.  Registers everything
 * via link_nautilus_wasi(), which wasm_setup() calls before execution.
 *
 * wasm_nautilus.h is force-included by the Makefile, providing printk and
 * the PAL macros.
 */

#include "source/wasm3.h"
#include "wasi.h"

/* Kernel functions used by WASI handlers. */
extern int      printk(const char *fmt, ...);
extern uint64_t nk_sched_get_realtime(void);
extern void     nk_get_rand_bytes(uint8_t *buf, unsigned len);
extern int      nk_vc_getchar(void);
extern void     nk_yield(void);

/* -----------------------------------------------------------------------
 * Scratch buffer — kernel-side staging area exposed as naut::scratch_write
 * and naut::scratch_read.  Tests bidirectional wasm linear memory passing.
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
 * WASI host functions
 * ----------------------------------------------------------------------- */

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
    m3ApiReturn(0);
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
    m3ApiWriteMem64(res_ptr, 1ULL);
    m3ApiReturn(0);
}

/*
 * fd_close(fd) -> errno
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
 * All fds are character devices (non-seekable).
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

/* -----------------------------------------------------------------------
 * Registration
 * ----------------------------------------------------------------------- */

M3Result link_nautilus_wasi(IM3Module module)
{
    M3Result res;

#define LINK(mod, name, sig, fn) \
    res = m3_LinkRawFunction(module, mod, name, sig, fn); \
    if (res && res != m3Err_functionLookupFailed) return res;

    LINK("wasi_snapshot_preview1", "fd_write",          "i(iiii)",  &naut_wasi_fd_write)
    LINK("wasi_unstable",          "fd_write",          "i(iiii)",  &naut_wasi_fd_write)
    LINK("wasi_snapshot_preview1", "proc_exit",         "v(i)",     &naut_wasi_proc_exit)
    LINK("wasi_unstable",          "proc_exit",         "v(i)",     &naut_wasi_proc_exit)
    LINK("wasi_snapshot_preview1", "fd_fdstat_get",     "i(ii)",    &naut_wasi_fd_fdstat_get)
    LINK("wasi_unstable",          "fd_fdstat_get",     "i(ii)",    &naut_wasi_fd_fdstat_get)
    LINK("wasi_snapshot_preview1", "environ_sizes_get", "i(ii)",    &naut_wasi_environ_sizes_get)
    LINK("wasi_unstable",          "environ_sizes_get", "i(ii)",    &naut_wasi_environ_sizes_get)
    LINK("wasi_snapshot_preview1", "environ_get",       "i(ii)",    &naut_wasi_environ_get)
    LINK("wasi_unstable",          "environ_get",       "i(ii)",    &naut_wasi_environ_get)
    LINK("wasi_snapshot_preview1", "args_sizes_get",    "i(ii)",    &naut_wasi_args_sizes_get)
    LINK("wasi_unstable",          "args_sizes_get",    "i(ii)",    &naut_wasi_args_sizes_get)
    LINK("wasi_snapshot_preview1", "args_get",          "i(ii)",    &naut_wasi_args_get)
    LINK("wasi_unstable",          "args_get",          "i(ii)",    &naut_wasi_args_get)
    LINK("wasi_snapshot_preview1", "clock_time_get",    "i(iIi)",   &naut_wasi_clock_time_get)
    LINK("wasi_unstable",          "clock_time_get",    "i(iIi)",   &naut_wasi_clock_time_get)
    LINK("wasi_snapshot_preview1", "random_get",        "i(ii)",    &naut_wasi_random_get)
    LINK("wasi_unstable",          "random_get",        "i(ii)",    &naut_wasi_random_get)
    LINK("wasi_snapshot_preview1", "fd_read",           "i(iiii)",  &naut_wasi_fd_read)
    LINK("wasi_unstable",          "fd_read",           "i(iiii)",  &naut_wasi_fd_read)
    LINK("wasi_snapshot_preview1", "sched_yield",       "i()",      &naut_wasi_sched_yield)
    LINK("wasi_unstable",          "sched_yield",       "i()",      &naut_wasi_sched_yield)
    LINK("wasi_snapshot_preview1", "clock_res_get",     "i(ii)",    &naut_wasi_clock_res_get)
    LINK("wasi_unstable",          "clock_res_get",     "i(ii)",    &naut_wasi_clock_res_get)
    LINK("wasi_snapshot_preview1", "fd_close",          "i(i)",     &naut_wasi_fd_close)
    LINK("wasi_unstable",          "fd_close",          "i(i)",     &naut_wasi_fd_close)
    LINK("wasi_snapshot_preview1", "fd_seek",           "i(iIii)",  &naut_wasi_fd_seek)
    LINK("wasi_unstable",          "fd_seek",           "i(iIii)",  &naut_wasi_fd_seek)

    LINK("naut", "scratch_write", "i(ii)", &naut_scratch_write)
    LINK("naut", "scratch_read",  "i(ii)", &naut_scratch_read)

#undef LINK
    return m3Err_none;
}
