# Force-include the PAL header into every compilation unit in this directory.
# This provides the allocator macros (malloc→kmem_malloc etc.), math redirects,
# and kernel extern declarations without touching any upstream Wasm3 source.
ccflags-y += -include $(src)/pal.h

# Suppress warnings from upstream Wasm3 code we don't control.
ccflags-y += -Wno-unused-variable -Wno-unused-function -Wno-implicit-function-declaration

# glibc fortification (__memset_chk, __snprintf_chk, etc.) requires libc at
# link time — unavailable in the kernel.  Strip _FORTIFY_SOURCE entirely.
ccflags-y += -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0

# Core Wasm3 interpreter (upstream source lives in source/)
obj-y += source/

# Nautilus glue
obj-y += pal.o
obj-y += runner.o
obj-y += wasi.o
obj-y += tests.o
obj-y += commands.o
