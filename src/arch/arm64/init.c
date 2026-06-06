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
 * Copyright (c) 2023, The V3VEE Project  <http://www.v3vee.org>
 *                     The Hobbes Project <http://xstack.sandia.gov/hobbes>
 * All rights reserved.
 *
 * Authors: Kevin Hayes <kjhayes@u.northwestern.edu>
 *
 * This is free software.  You are permitted to use,
 * redistribute, and modify it as specified in the file "LICENSE.txt".
 */


#define __NAUTILUS_MAIN__

#include<nautilus/nautilus.h>
#include<nautilus/init.h>
#include<nautilus/printk.h>
#include<nautilus/naut_string.h>
#include<nautilus/arch.h>
#include<nautilus/dev.h>
#include<nautilus/chardev.h>
#include<nautilus/blkdev.h>
#include<nautilus/netdev.h>
#include<nautilus/gpudev.h>
#include<nautilus/irqdev.h>
#include<nautilus/gpiodev.h>
#include<nautilus/gpio.h>
#include<nautilus/iomap.h>
#include<nautilus/arch.h>
#include<nautilus/interrupt.h>
#include<nautilus/mm.h>
#include<nautilus/waitqueue.h>
#include<nautilus/future.h>
#include<nautilus/timer.h>
#include<nautilus/random.h>
#include<nautilus/semaphore.h>
#include<nautilus/msg_queue.h>
#include<nautilus/group_sched.h>
#include<nautilus/idle.h>
#include<nautilus/barrier.h>
#include<nautilus/smp.h>
#include<nautilus/vc.h>
#include<nautilus/fs.h>
#include<nautilus/shell.h>

#include<nautilus/ipi.h>

#include<nautilus/of/dt.h>

#include<arch/arm64/unimpl.h>
#include<arch/arm64/sys_reg.h>
#include<arch/arm64/excp.h>
#include<arch/arm64/timer.h>
#include<arch/arm64/psci.h>
#include<arch/arm64/paging.h>
#include<arch/arm64/fpu.h>

#include<dev/pci.h>
#ifdef NAUT_CONFIG_OF_PCI
#include<dev/pci/of.h>
#endif

#if defined(NAUT_CONFIG_GIC_VERSION_2) || defined(NAUT_CONFIG_GIC_VERSION_2M)
#include<dev/gicv2.h>
#elif defined(NAUT_CONFIG_GIC_VERSION_3)
#include<dev/gicv3.h>
#endif

#ifdef NAUT_CONFIG_VIRTIO_PCI
#include<dev/virtio_pci.h>
#endif

#ifndef NAUT_CONFIG_DEBUG_PRINTS
#undef DEBUG_PRINT
#define DEBUG_PRINT(fmt, args...)
#endif

#define INIT_PRINT(fmt, args...) INFO_PRINT("init: " fmt, ##args)
#define INIT_DEBUG(fmt, args...) DEBUG_PRINT("init: " fmt, ##args)
#define INIT_ERROR(fmt, args...) ERROR_PRINT("init: " fmt, ##args)
#define INIT_WARN(fmt, args...) WARN_PRINT("init: " fmt, ##args)

void smp_ap_stack_switch(void *new_stack, void *new_base, void *survive);

int arch_paging_init(struct nk_mem_info *mm_info, void *fdt);

#define QUANTUM_IN_NS (1000000000ULL / NAUT_CONFIG_HZ)
struct nk_sched_config sched_cfg = {
    .util_limit = NAUT_CONFIG_UTILIZATION_LIMIT * 10000ULL,  // convert percent to 10^-6 units
    .sporadic_reservation = NAUT_CONFIG_SPORADIC_RESERVATION * 10000ULL,  // ..
    .aperiodic_reservation = NAUT_CONFIG_APERIODIC_RESERVATION * 10000ULL,  // ..
    .aperiodic_quantum = QUANTUM_IN_NS,
    .aperiodic_default_priority = QUANTUM_IN_NS,
};

extern int smp_init_tpidr(void); 

static inline void per_cpu_sys_ctrl_reg_init(void) {

  sctlr_el1_t sctlr;
  LOAD_SYS_REG(SCTLR_EL1, sctlr.raw);

  sctlr.align_check_en = 0;
  sctlr.unaligned_acc_en = 1;
  sctlr.write_exec_never = 0;
  sctlr.instr_cacheability_ctrl = 1;
  sctlr.data_cacheability_ctrl = 1;

#ifdef NAUT_CONFIG_DEBUG_PRINTS
  //dump_sctlr_el1(sctlr);
#endif

  STORE_SYS_REG(SCTLR_EL1, sctlr.raw);
}

static inline int init_core_barrier(struct sys_info *sys) {
  sys->core_barrier = (nk_barrier_t *)malloc(sizeof(nk_barrier_t));
  if (!sys->core_barrier) {
    ERROR_PRINT("Could not allocate core barrier\n");
    return -1;
  }
  memset(sys->core_barrier, 0, sizeof(nk_barrier_t));

  if (nk_barrier_init(sys->core_barrier, sys->num_cpus) != 0) {
    ERROR_PRINT("Could not create core barrier\n");
    return -1;
  }

  INIT_PRINT("Initialized the core barrier\n");

  return 0;
}

volatile static uint8_t __secondary_init_first_phase_complete = 0;
volatile static uint8_t __secondary_init_second_phase_token = 0;

void * secondary_init_boot_stack(void) {

  fpu_init_percpu(&nautilus_info);

  if(smp_init_tpidr()) {
    INIT_ERROR("Could not set TPIDR_EL1 for CPU!\n");
    panic("Could not set TPIDR_EL1 for CPU! panicking...\n");
  }

  INIT_PRINT("Starting CPU %u...\n", my_cpu_id());

  per_cpu_sys_ctrl_reg_init();
 
  INIT_PRINT("per_cpu init paging\n"); 
  per_cpu_paging_init();

  // Signal that we reached the end of the first phase
  __secondary_init_first_phase_complete = 1;

  // Wait for the second_phase_token to allow us to pass
  while(__secondary_init_second_phase_token < my_cpu_id()) {} 

  // We should have atomics and kmem now
  INIT_PRINT("Starting second phase of secondary_init\n");

#if defined(NAUT_CONFIG_GIC_VERSION_2) || defined(NAUT_CONFIG_GIC_VERSION_2M)
  if(gicv2_percpu_init()) {
    panic("Failed to initialize GICv2 on CPU %u!\n", my_cpu_id());  
  }
#elif defined(NAUT_CONFIG_GIC_VERSION_3)
  if(gicv3_percpu_init()) {
    panic("Failed to initialize GICv3 on CPU %u!\n", my_cpu_id());  
  }
#else
#error "Invalid GIC Version!"
#endif

  INIT_PRINT("Initialized the IRQ chip!\n", my_cpu_id());

  if(smp_xcall_init_queue(nautilus_info.sys.cpus[my_cpu_id()])) {
      INIT_WARN("Failed to initialize XCALL queue on cpu %u!\n", my_cpu_id());
  }

  nk_rand_init(nautilus_info.sys.cpus[my_cpu_id()]);

  nk_sched_init_ap(&sched_cfg);

  // nk_sched_init_ap should have allocated us a stack, so we need to switch to it
  // (previously we should have been using the shared boot stack)
  return get_cur_thread()->rsp;
}

void secondary_init_threaded(void) {
  nk_thread_name(get_cur_thread(), "secondary_init");

  INIT_PRINT("ARM64: successfully started CPU %d\n", my_cpu_id()); 
  if(__secondary_init_second_phase_token+1 < nautilus_info.sys.num_cpus) {
    INIT_PRINT("Letting CPU %u start it's second phase!\n", __secondary_init_second_phase_token+1);
  } else {
    INIT_PRINT("Final AP finished initializing\n");
  }
  __secondary_init_second_phase_token += 1;

  nk_sched_start();
 
  // Enable interrupts
#ifndef NAUT_CONFIG_BEANDIP
  arch_enable_ints(); 
#endif

  INIT_PRINT("Interrupts are now enabled\n");

  //enable the timer
  percpu_timer_init();

  INIT_PRINT("Promoting secondary init thread to idle!\n");
  idle(NULL, NULL);
}

extern void secondary_start(void);

static int start_secondaries(struct sys_info *sys) {
  INIT_PRINT("Starting secondary processors\n");

  for(uint64_t i = 1; i < sys->num_cpus; i++) {
    __secondary_init_first_phase_complete = 0;
    // Initialize the stack

    void *stack_base = malloc(4096);
    if(stack_base == NULL) {
      INIT_ERROR("Could not allocate a stack for secondary core: %u\n", i);
      return -1;
    }
    stack_base += 4096;

    mpidr_el1_t mpid;
    mpid.raw = 0;
    mpid.aff0 = sys->cpus[i]->aff0;
    mpid.aff1 = sys->cpus[i]->aff1;
    mpid.aff2 = sys->cpus[i]->aff2;
    mpid.aff3 = sys->cpus[i]->aff3;

    INIT_PRINT("Trying to start secondary core: %u\n", i);
    if(psci_cpu_on(mpid.raw, (void*)secondary_start, (uint64_t)stack_base)) {
      INIT_ERROR("PSCI Error: psci_cpu_on failed for CPU %u!\n", i);
    }

    while(!__secondary_init_first_phase_complete) {
      // Wait for the secondary cpu to finish the first phase
      // (We need to start them one by one because atomics aren't enabled yet)
    }
  }

  // All of the CPU's are past the first stage (and should have enabled MMU's)

  INIT_PRINT("All CPU's are past the first stage!\n");
  INIT_PRINT("Enabling atomics\n");
  extern int __atomics_enabled;
  __atomics_enabled = 1;
  INIT_PRINT("Atomics enabled!\n");
  return 0;
}

static int finish_secondaries(struct sys_info *sys) {
  INIT_PRINT("Starting CPU 1's second phase...\n");
  __secondary_init_second_phase_token = 1;

  while(__secondary_init_second_phase_token < sys->num_cpus) {}
  INIT_PRINT("Finished initializing all CPU's!\n");
  return 0;
}

/* Faking some vc stuff */

uint16_t
vga_make_entry (char c, uint8_t color)
{
    uint16_t c16 = c;
    uint16_t color16 = color;
    return c16 | color16 << 8;
}
// (I want to stick this somewhere else later on (or make it not needed))
//


extern void* _bssEnd[];
extern void* _bssStart[];


//#define ROCKCHIP

static int rockchip_io_mapped = 0;
int rockchip_leds(int val) {
#ifdef ROCKCHIP 
  void *bank_1_reg_base = (void*)0xff720000;
  if(!rockchip_io_mapped) {
    nk_io_map(bank_1_reg_base, 0x1000, 0);
    rockchip_io_mapped = 1;
  }
  int diy_led_index = 0x2;
  int work_led_index = 0xB;

  uint32_t diy_bit = (1<<diy_led_index);
  uint32_t work_bit = (1<<work_led_index);

  int dr_offset = 0;
  int ddr_offset = 4;

  volatile uint32_t *dr_reg = bank_1_reg_base + dr_offset;
  volatile uint32_t *ddr_reg = bank_1_reg_base + ddr_offset;

  *ddr_reg |= diy_bit | work_bit;
  asm volatile ("isb" ::: "memory");

  val &= 0b11;

  if(val & 0b01) {
    *dr_reg |= diy_bit;
  } else {
    *dr_reg &= ~diy_bit;
  }

  if(val & 0b10) {
    *dr_reg |= work_bit;
  } else {
    *dr_reg &= ~work_bit;
  }
#endif
  return 0;
}

int rockchip_halt_and_flash(int val0, int val1, int fast) {
#ifdef ROCKCHIP
  uint64_t delay = 0xFFFF;
  if(!fast) {
    delay = delay * 16;
  }

  int i = 0;
  while(1) { // halt loop
    i = i ? 0 : 1;
    if(i == 0) {
      rockchip_leds(val0);
    } else {
      rockchip_leds(val1);
    }
    for(volatile int i = 0; i < delay; i++) {
      // delay loop
    }
  }

  // Error if we return
  return -1;
#endif
  return 0;
}

#ifdef NAUT_CONFIG_BEANDIP
__attribute__((annotate("nohook")))
#endif
void * init_boot_stack(unsigned long dtb, unsigned long x1, unsigned long x2, unsigned long x3) {

  // Zero out .bss
  nk_low_level_memset((void*)_bssStart, 0, (uint64_t)_bssEnd - (uint64_t)_bssStart);
 
  // Enable the FPU
  fpu_init_percpu(&nautilus_info);

  nautilus_info.sys.dtb = (struct dtb_fdt_header*)dtb;

  mpidr_el1_t mpidr_el1;
  LOAD_SYS_REG(MPIDR_EL1, mpidr_el1.raw);
  nautilus_info.sys.bsp_aff0 = mpidr_el1.aff0;
  nautilus_info.sys.bsp_aff1 = mpidr_el1.aff1;
  nautilus_info.sys.bsp_aff2 = mpidr_el1.aff2;
  nautilus_info.sys.bsp_aff3 = mpidr_el1.aff3;

  nk_handle_init_stage_silent();

  printk("initializing in EL%u\n", arm64_get_current_el());

  per_cpu_sys_ctrl_reg_init();

  nk_handle_init_stage_static();
  
  // Setup the temporary boot-time allocator
  mm_boot_init(dtb);
 
  // Initialize SMP using the dtb
  arch_smp_early_init(&nautilus_info);

  // Set our thread pointer id register for the BSP
  if(smp_init_tpidr()) {
    INIT_ERROR("Could not set TPIDR_EL1 for BSP!\n");
    panic("Could not set TPIDR_EL1 for BSP!\n");
  }
  
  // Initialize NUMA
  if(arch_numa_init(&(nautilus_info.sys))) {
    INIT_ERROR("Could not get NUMA information!\n");
    panic("Could not get NUMA information!\n");
  }

  nk_handle_init_stage_boot();

  // Can't go in a "boot" init function because it disables the boot allocator
  nk_kmem_init();
  mm_boot_kmem_init();

  nk_handle_init_stage_kmem();

  // Do this early to speed up the boot process with data caches enabled
  // (Plus atomics might not work without cacheability because ARM is weird)
  if(arch_paging_init(&(nautilus_info.sys.mem), (void*)dtb)) {
    INIT_ERROR("Failed to initialize paging!\n");
    panic("Failed to initialize paging globally!\n");
  }

  per_cpu_paging_init();
 
  if(start_secondaries(&(nautilus_info.sys))) {
    INIT_ERROR("Failed to start secondaries!\n");
    panic("Failed to start secondaries!\n");
  }
  
  nk_gpio_init();

  nk_wait_queue_init();
  nk_future_init();
  nk_timer_init();
  nk_rand_init(nautilus_info.sys.cpus[my_cpu_id()]);
  nk_semaphore_init();
  nk_msg_queue_init();

  nk_sched_init(&sched_cfg);

  nk_thread_group_init();
  nk_group_sched_init();

  init_core_barrier(&(nautilus_info.sys));

  return get_cur_thread()->rsp;
}

void init_threaded(void) {
  INIT_DEBUG("Swapped to the thread stack!\n");

  nk_thread_name(get_cur_thread(), "init");

  nk_handle_init_stage_subsys();

#ifdef NAUT_CONFIG_OF_PCI 
  if(!of_pci_init()) {
    pci_dump_device_list();
  }
#endif

#if defined(NAUT_CONFIG_GIC_VERSION_2) || defined(NAUT_CONFIG_GIC_VERSION_2M)
  if(gicv2_init()) {
    panic("Failed to initialize GICv2!\n");  
  } 
#elif defined(NAUT_CONFIG_GIC_VERSION_3)
  if(gicv3_init()) {
    panic("Failed to initialize GICv3!\n");  
  }
#else
#error "Invalid GIC Version!"
#endif

#if defined(NAUT_CONFIG_GIC_VERSION_2) || defined(NAUT_CONFIG_GIC_VERSION_2M)
  if(gicv2_percpu_init()) {
    panic("Failed to initialize GICv2 on the BSP!\n");  
  }
#elif defined(NAUT_CONFIG_GIC_VERSION_3)
  if(gicv3_percpu_init()) {
    panic("Failed to initialize GICv3 on the BSP!\n");
  }
#else
#error "Invalid GIC Version!"
#endif

  INIT_DEBUG("Initialized the Interrupt Controller\n");

  if(smp_setup_xcall_bsp(nautilus_info.sys.cpus[my_cpu_id()])) {
      INIT_WARN("Failed to initialize XCALL on bsp!\n");
  }

  // Now we should be able to install irq handlers
#ifdef NAUT_CONFIG_VIRTIO_PCI
  virtio_pci_init(&nautilus_info);
  INIT_DEBUG("virtio pci inited!\n");
#endif

  global_timer_init();

  // Let the secondary processors into their second phase
  finish_secondaries(&(nautilus_info.sys)); 

  INIT_DEBUG("Starting the scheduler on BSP\n");
  // Start the scheduler
  nk_sched_start(); 
  INIT_DEBUG("Scheduler started!\n");

  /*
  INIT_DEBUG("Printing CPU states:\n");
  for(int i = 0; i < nautilus_info.sys.num_cpus; i++) {
    INIT_DEBUG("Dumping CPU %u\n", i);
    dump_cpu(nautilus_info.sys.cpus[i]);
  }
  */

  //enable the timer
  percpu_timer_init();
  arch_set_timer(arch_realtime_to_cycles(sched_cfg.aperiodic_quantum));
 
  nk_handle_init_stage_sched();

  // Enable interrupts
  arch_enable_ints(); 

  INIT_PRINT("Interrupts are now enabled\n"); 

  nk_handle_init_stage_driver();

  nk_handle_init_stage_fs();

#ifdef NAUT_CONFIG_IPI_STRESS
  ipi_stress_init();
#endif

  nk_handle_init_stage_launch();

#ifdef NAUT_CONFIG_WASM_RT
  {
    extern int handle_wasm_test(void);
    INIT_PRINT("ARM64: running wasm test suite\n");
    handle_wasm_test();
    INIT_PRINT("ARM64: wasm test suite complete\n");
  }
#endif

  INIT_PRINT("Promoting init thread to idle\n");

  idle(NULL,NULL);
}

