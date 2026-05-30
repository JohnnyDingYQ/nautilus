/*
 * runner.h - Wasm3 module lifecycle and execution interface.
 */

#ifndef WASM_RUNNER_H
#define WASM_RUNNER_H

#include "source/wasm3.h"

/* Parse, load, and WASI-link a module.  On success the env and runtime are
 * live and must be freed via wasm_teardown().  On failure they are freed
 * internally and the error string is returned. */
M3Result wasm_setup(const uint8_t *bytes, uint32_t len,
                    IM3Environment *env_out, IM3Runtime *runtime_out);

void wasm_teardown(IM3Environment env, IM3Runtime runtime);

/* Run a named export with a single u32 argument; prints the result. */
int run_wasm(const uint8_t *bytes, uint32_t len,
             const char *func_name, uint32_t arg);

/* Load a .wasm file from the Nautilus filesystem and call _start or main. */
int run_wasm_file(const char *path);

/* Run the embedded fib32 demo with argument n. */
void run_wasm_fib(unsigned int n);

#endif /* WASM_RUNNER_H */
