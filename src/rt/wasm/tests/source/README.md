# Test Source Files

This directory contains the C source files for the wasm3-on-Nautilus test suite. Each `.c` file is compiled to a `.wasm` binary and then embedded as a C header (`tests/<name>.wasm.h`) for inclusion directly into the kernel image.

## Prerequisites

- **WASI SDK** — default install path `/opt/wasi-sdk`. Override with `WASI_SDK=/your/path`.
  Download from: https://github.com/WebAssembly/wasi-sdk/releases
- **xxd** or **wasm-pack** for converting `.wasm` → `.wasm.h` (see Embedding step below).

## Tests with C source

| Source file | Exported function | Compile flags | Description |
|---|---|---|---|
| `sqrt_test.c` | `sqrt_scaled` | `-nostdlib --no-entry` | Computes `floor(sqrt(n) * 1e6)` as `i64` using `f64.sqrt`. No WASI imports. |
| `scratch_test.c` | `scratch_roundtrip` | `-nostdlib --no-entry` | Writes a byte pattern via `naut::scratch_write`, reads it back via `naut::scratch_read`. Imports two custom host functions from the `naut` module. |

## Tests without C source (binary-only)

| Header file | Exported function | Description |
|---|---|---|
| `fib32.wasm.h` | `fib` | Recursive Fibonacci, i32 return. Binary-only — no C or WAT source available. |
| `fib32_tail.wasm.h` | `fib` | Tail-recursive Fibonacci, i32 return. Binary-only. |
| `fib64.wasm.h` | `fib` | Recursive Fibonacci, i64 return. Binary-only. |

## Compiling a test to `.wasm`

### No WASI imports (pure computation or custom host imports)

```sh
WASI_SDK=/opt/wasi-sdk
CC=$WASI_SDK/bin/clang
SYSROOT=$WASI_SDK/share/wasi-sysroot

$CC --sysroot=$SYSROOT -O2 -nostdlib -Wl,--no-entry \
    -Wl,--export=<function_name> \
    -o <name>.wasm <name>.c
```

Use this for `sqrt_test.c` and `scratch_test.c`. The `-nostdlib --no-entry` flags produce a module with no WASI imports and no `_start` — the kernel calls the named export directly.

### WASI program (has `main`, uses `printf`, etc.)

```sh
$CC --sysroot=$SYSROOT -O2 -o <name>.wasm <name>.c
```

Use this for programs compiled with the standard WASI libc (e.g., `hello.c`). The resulting module imports `wasi_snapshot_preview1::fd_write` and others, which are satisfied by the WASI stubs in `wasm_nautilus.c`.

## Embedding as a kernel header (`.wasm` → `.wasm.h`)

After compilation, convert the binary to a C byte array:

```sh
# Using xxd (available on most Linux/macOS systems)
xxd -i <name>.wasm > ../tests/<name>.wasm.h
```

`xxd -i` produces a declaration of the form:

```c
unsigned char name_wasm[] = { 0x00, 0x61, ... };
unsigned int  name_wasm_len = <N>;
```

The variable name is derived from the filename by replacing `.` and `-` with `_`. Verify it matches what `wasm_nautilus.c` includes (e.g., `fib32_wasm`, `sqrt_test_wasm`, `scratch_test_wasm`).

Place the generated header in `src/rt/wasm/tests/` and rebuild the kernel. No Makefile changes are needed — the header is `#include`d directly in `wasm_nautilus.c`.

## Adding a new test

1. Write `<name>.c` in this directory.
2. Compile to `<name>.wasm` using the appropriate flags above.
3. Embed with `xxd -i <name>.wasm > ../tests/<name>.wasm.h`.
4. Add an `#include "tests/<name>.wasm.h"` line in `wasm_nautilus.c`.
5. Add an entry to the `wasm_tests[]` table in `wasm_nautilus.c`:

```c
{ "<display-name>", "<export-function>", <name>_wasm, sizeof(<name>_wasm), <arg>, <expected> },
```

6. Update `docs/changes_summary.md` and the test table in that file.
