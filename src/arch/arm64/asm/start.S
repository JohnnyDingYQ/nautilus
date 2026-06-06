/*
 * This file is part of the Nautilus AeroKernel developed
 * by the Constellation, Interweaving, Hobbes, and V3VEE
 * Projects with funding from the United States National
 * Science Foundation and the Department of Energy.
 *
 * The V3VEE Project is a joint project between Northwestern University
 * and the University of New Mexico.  The Hobbes Project is a collaboration
 * led by Sandia National Laboratories that includes several national
 * laboratories and universities. The Interweaving Project is a
 * joint project between Northwestern University and Illinois Institute
 * of Technology.   The Constellation Project is a joint project
 * between Northwestern University, Carnegie Mellon University,
 * and Illinois Institute of Technology.
 * You can find out more at:
 * http://www.v3vee.org
 * http://xstack.sandia.gov/hobbes
 * http://interweaving.org
 * http://constellation-project.net
 *
 * Copyright (c) 2023, Kevin Hayes <kjhayes@u.northwestern.edu>
                       Nick Wanninger <ncw@u.northwestern.edu>
 * Copyright (c) 2023, The V3VEE Project  <http://www.v3vee.org>
 *                     The Hobbes Project <http://xstack.sandia.gov/hobbes>
 * All rights reserved.
 *
 * Authors: Kevin Hayes <kjhayes@u.northwestern.edu>
 *          Nick Wanninger <ncw@u.northwestern.edu>
 *
 * This is free software.  You are permitted to use,
 * redistribute, and modify it as specified in the file "LICENSE.txt".
 */


# start.S - Boot level functionality for Nautilus on arm64 (modified from ChariotOS)

# AArch64 mode

# To keep this in the first portion of the binary.
.section .text.boot

.extern _loadEnd
.extern _loadStart

linux_header:
b nautilus_entry
b nautilus_entry
.quad 0 // Offset
.quad 0 // Size
.quad 0 // Flags
.quad 0 // RES
.quad 0 // RES
.quad 0 // RES
.word 0x644d5241 // Magic
.word 0 // RES

.section .text
.extern install_iv_tables_el1
.global nautilus_entry
# Entry point for the kernel. Registers:
# x0 -> 32 bit pointer to DTB in memory (primary core only) / 0 (secondary cores)
# x1 -> 0
# x2 -> 0
# x3 -> 0
# x4 -> 32 bit kernel entry point, _start location

tmp                     .req x9

nautilus_entry:
    # Disable Interrupts
    msr DAIFSet, 0b1111
    # Save the DTB (ARM64 boot protocol: x0 = physical address of DTB).
    # QEMU only sets x0 when booting a flat ARM64 Image binary; when loading
    # an ELF directly via -kernel, x0 is 0.  Fall back to the QEMU virt
    # default: QEMU places the DTB at the start of RAM (0x40000000).
    mov x20, x0
    cbnz x20, 1f
    ldr x20, =0x40000000
1:

    # Exceptions will try to access the CPU struct if TPIDR_EL1 is non-null
    msr TPIDR_EL1, xzr

    # Incase we boot in EL2
    bl try_el2_to_el1

    # Check processor ID is zero (executing on main core), else hang
    # (According to the linux boot doc for ARM64, this should always pass,
    #  because non-BSP's should loop in bootloader reserved memory)
    mrs     x1, mpidr_el1
    and     x1, x1, 3
    cbz     x1, bsp_entry

secondary_loop:
    /* enable caches so atomics and spinlocks work */
    mrs     tmp, sctlr_el1
    orr     tmp, tmp, (1<<12) /* Enable icache */
    orr     tmp, tmp, (1<<2)  /* Enable dcache/ucache */
    orr     tmp, tmp, (1<<3)  /* Enable Stack Alignment Check EL1 */
    orr     tmp, tmp, (1<<4)  /* Enable Stack Alignment Check EL0 */
    bic     tmp, tmp, (1<<1)  /* Disable Alignment Checking for EL1 EL0 */
    msr     sctlr_el1, tmp

    # We're not on the main core, so hang in an infinite wait loop
1:  wfe
    b  1b

bsp_entry:  # We're on the main core!

    # Configure the processor to use a different stack for each EL
    msr SPsel, 1
    
    # Set stack to start below our code
    ldr     x0, =boot_stack_end
    mov     sp, x0

    # Install the interrupt vector tables
    bl      install_iv_tables_el1

    # Jump to our kernel_main routine in C (make sure it doesn't return)
4:  

    # restore DTB 
    mov x0, x20
    bl      init_boot_stack
    
    mov sp, x0
    mov x29, x0

    bl      init_threaded

    # In case it does return, halt the master core too
    # Load a obvious value into x29 so it will be clear to the debugger
    ldr x1, =0xCAFEBABE
    b .


.global secondary_start
secondary_start:

    // We pass a stack through context_id (save it incase we are in EL2)
    mov x20, x0

    bl try_el2_to_el1

    mov sp, x20
    
    # Exceptions will try to access the CPU struct if TPIDR_EL1 is non-null
    msr TPIDR_EL1, xzr

    bl install_iv_tables_el1

    bl secondary_init_boot_stack

    # Swap stacks
    mov sp, x0
    mov x29, x0

    bl secondary_init_threaded

    ldr x1, =0xCAFEBABE
    b .

try_el2_to_el1:

    # This function is based on Linux's "el2_setup" from "arch/arm/kernel/head.S"
    
    mrs x0, CurrentEL
    # EL2, because we care about CurrentEL{3:2} and CurrentEL{1:0} are RES0
    cmp x0, 8 
    b.eq 1f

    # We're not in EL2
    ret
1:

    # Install the exception handlers (So if we double fault we can still hopefully get some error output)
    # This comes at some pretty high performance costs
    //mov x28, lr
    //bl install_iv_tables_el2
    //mov lr, x28

    # EL1 should be 64bit
    mov x0, (1<<31)
    msr HCR_EL2, x0

    # Make sure timers are enabled
    mrs x0, CNTHCTL_EL2
    orr x0, x0, 3
    msr CNTHCTL_EL2, x0

    # Setup Processor ID's
    mrs x0, midr_el1
    mrs x1, mpidr_el1
    msr vpidr_el2, x0
    msr vmpidr_el2, x1

    # Make sure the MMU is disabled
    ldr x0, =0x30d00800
    msr SCTLR_EL1, x0

    # Allow access to FP and SIMD
    mov x0, 0x33FF
    msr CPTR_EL2, x0

    msr HSTR_EL2, xzr

    # Return from EL2
    mov x0, 0x3C5 
    msr SPSR_EL2, x0
    msr ELR_EL2, lr
    eret

.section .data
.global boot_stack_end
.align 16
boot_stack:
.space 4096
boot_stack_end:
