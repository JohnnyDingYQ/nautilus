# Force-include the PAL header into every Wasm3 compilation unit.
# This provides calloc, fprintf->printk, and abort->panic without
# touching any upstream Wasm3 source file.
ccflags-y += -include $(src)/wasm_nautilus.h

# Suppress warnings from upstream Wasm3 code we don't control
ccflags-y += -Wno-unused-variable -Wno-unused-function -Wno-implicit-function-declaration

# glibc fortification (__memset_chk, __snprintf_chk, etc.) requires libc at
# link time — unavailable in the kernel.  Strip _FORTIFY_SOURCE entirely.
ccflags-y += -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0

# Float opcodes enabled; all required libm symbols are stubbed in wasm_nautilus.c.

# Core Wasm3 interpreter (upstream, unmodified)
obj-y += \
	m3_bind.o    \
	m3_code.o    \
	m3_compile.o \
	m3_core.o    \
	m3_env.o     \
	m3_exec.o    \
	m3_function.o \
	m3_info.o    \
	m3_module.o  \
	m3_parse.o

# Nautilus glue (WASI stubs + shell command)
obj-y += wasm_nautilus.o
