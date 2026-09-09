/*
 * Version-neutral ARM Generic Interrupt Controller support for the ravynOS
 * arm64 bring-up targets (Raspberry Pi 5 / BCM2712 with a GIC-400, QEMU
 * virt with GICv2 or GICv3, later Virtualization.framework with GICv3).
 *
 * The pexpert layer owns the controller so the kernel can take timer
 * interrupts before any IOKit platform driver exists; the platform
 * expert's IOInterruptController later hooks in through the
 * register/enable/disable calls below. The GIC version comes from the
 * "gic-version" property of the "interrupt-controller" device tree node
 * that the ravynOS booter builds (2 when absent).
 */

#ifndef _PEXPERT_ARM64_GIC_H
#define _PEXPERT_ARM64_GIC_H

#include <stdint.h>
#include <stdbool.h>
#include <mach/vm_types.h>

#ifndef ASSEMBLER

/* Interrupt numbers (INTIDs) */
#define GIC_SGI_BASE            0
#define GIC_PPI_BASE            16
#define GIC_SPI_BASE            32
#define GIC_PPI_CNTP            30      /* EL1 physical timer, the one xnu uses */
#define GIC_PPI_CNTV            27      /* EL1 virtual timer (unused) */
#define GIC_MAX_INTID           1020
#define GIC_INTID_SPURIOUS      1023

#define GIC_DEFAULT_PRIORITY    0xA0
#define GIC_PMR_UNMASK_ALL      0xF0

typedef void (*pe_gic_handler_t)(void *refcon, uint32_t intid);

/*
 * What a controller version provides. `init` receives the device tree
 * "reg" cells (offsets from the arm-io base, then sizes) and returns the
 * number of interrupt lines, or 0 on failure. `ack` returns the raw
 * acknowledge value; the INTID is (value & intid_mask) and the same raw
 * value is handed back to `eoi`.
 */
struct pe_gic_ops {
	const char *name;
	uint32_t    version;
	uint32_t    intid_mask;
	uint32_t  (*init)(vm_offset_t soc_phys, const uintptr_t *reg, uint32_t nreg);
	void      (*cpu_init)(void);
	uint32_t  (*ack)(void);
	void      (*eoi)(uint32_t raw);
	void      (*enable)(uint32_t intid);
	void      (*disable)(uint32_t intid);
	void      (*set_priority)(uint32_t intid, uint8_t priority);
	void      (*set_edge)(uint32_t intid, bool edge);
	void      (*send_sgi)(uint32_t intid, uint32_t cpu_mask);
};

extern const struct pe_gic_ops pe_gicv2_ops;
extern const struct pe_gic_ops pe_gicv3_ops;

/* Boot CPU: map and reset the controller described by the device tree. */
extern uint32_t pe_gic_init(vm_offset_t soc_phys);

/* True once pe_gic_init() succeeded; version is 2 or 3 (0 when absent). */
extern bool pe_gic_present(void);
extern uint32_t pe_gic_version(void);

/* Number of interrupt lines the distributor implements. */
extern uint32_t pe_gic_num_interrupts(void);

/* Per-CPU interface initialization for secondary cores (SMP, later). */
extern void pe_gic_cpu_init(void);

/* The handler installed through ml_install_interrupt_handler(). */
extern void pe_gic_irq_handler(void *target, void *refcon, void *nub, int source);

/* Hooks for the IOKit interrupt controller. */
extern int  pe_gic_register(uint32_t intid, pe_gic_handler_t handler, void *refcon);
extern void pe_gic_unregister(uint32_t intid);
extern void pe_gic_enable(uint32_t intid);
extern void pe_gic_disable(uint32_t intid);
extern void pe_gic_set_priority(uint32_t intid, uint8_t priority);
extern void pe_gic_set_edge(uint32_t intid, bool edge);
extern void pe_gic_send_sgi(uint32_t intid, uint32_t cpu_mask);

#endif /* ! ASSEMBLER */

#endif /* ! _PEXPERT_ARM64_GIC_H */
