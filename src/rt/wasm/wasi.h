/*
 * wasi.h - WASI host binding interface.
 *
 * Exposes link_nautilus_wasi(), called by wasm_setup() after a module is
 * parsed and loaded to register all host imports before execution.
 */

#ifndef WASM_WASI_H
#define WASM_WASI_H

#include "source/wasm3.h"

M3Result link_nautilus_wasi(IM3Module module);

#endif /* WASM_WASI_H */
