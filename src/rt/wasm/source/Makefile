# Force-include the PAL header.  $(src) is .../wasm/source; go up one level
# to reach wasm_nautilus.h which lives in the wasm/ directory.
ccflags-y += -include $(src)/../pal.h

# Inherit the same warning suppression and fortification strip from the parent.
ccflags-y += -Wno-unused-variable -Wno-unused-function -Wno-implicit-function-declaration
ccflags-y += -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0

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
