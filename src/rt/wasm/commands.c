/*
 * commands.c - Nautilus shell command registration for the wasm runtime.
 *
 * Registers the "wasm" shell command and dispatches subcommands to the
 * runtime functions in wasm_nautilus.c.  Isolated here so that shell
 * plumbing is separate from the interpreter core and WASI host bindings.
 *
 * wasm_nautilus.h is force-included by the Makefile, providing printk and
 * the PAL macros.  No other Nautilus headers are needed.
 */

#include "runner.h"
#include "tests.h"

extern int printk(const char *fmt, ...);

/* -----------------------------------------------------------------------
 * Shell integration
 *
 * Copied from <nautilus/shell.h> to avoid pulling in the full Nautilus
 * header chain (nk_thread_id_t etc. redefine POSIX types used by wasm3).
 * ----------------------------------------------------------------------- */
struct shell_cmd_impl {
    char *cmd;
    char *help_str;
    int (*handler)(char *buf, void *priv);
};

#define nk_register_shell_cmd(cmd) \
    const static struct shell_cmd_impl * _nk_cmd_##cmd \
    __attribute__((used, unused, __section__(".shell_cmd."#cmd), \
        aligned(sizeof(void*)))) \
    = &cmd

/* strcmp substitute — avoids pulling in string.h */
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

    const char *arg_start = p;

    /* Peek at the first argument token. */
    char subcmd[8] = {0};
    int i = 0;
    while (*p && *p != ' ' && i < 7) subcmd[i++] = *p++;
    subcmd[i] = '\0';

    if (wasm_strcmp(subcmd, "test") == 0) {
        handle_wasm_test();
    } else if (subcmd[0] == '\0' || wasm_strcmp(subcmd, "fib") == 0) {
        /* "wasm [fib [n]]" — run the embedded fibonacci demo. */
        unsigned int arg = 10;
        while (*p == ' ') p++;
        if (*p >= '0' && *p <= '9') {
            arg = 0;
            while (*p >= '0' && *p <= '9') arg = arg * 10 + (*p++ - '0');
        }
        printk("wasm: running fib(%u) from embedded fib32.wasm\n", arg);
        run_wasm_fib(arg);
    } else {
        /* Any other token is treated as a filesystem path. */
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
    .help_str = "wasm test  -- run embedded test suite; "
                "wasm fib <n>  -- run fib32; "
                "wasm <path>  -- run WASM file",
    .handler  = handle_wasm_cmd,
};
nk_register_shell_cmd(wasm_impl);
