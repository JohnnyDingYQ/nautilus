/*
 * tests.h - Interface for the wasm built-in test suite.
 */

#ifndef WASM_TESTS_H
#define WASM_TESTS_H

/* Run all entries in the wasm_tests[] table. Returns 0 if all pass. */
int handle_wasm_test(void);

#endif /* WASM_TESTS_H */
