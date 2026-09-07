/*
 * ARM Generic Interrupt Controller v2 (GIC-400) support for the ravynOS
 * arm64 bring-up targets (Raspberry Pi 5 / BCM2712, QEMU virt).
 *
 * The pexpert layer owns the distributor and CPU interface so the kernel
 * can take timer interrupts before any IOKit platform driver exists; the
 * platform expert's IOInterruptController later hooks in through the
 * register/enable/disable calls below.
 */

#ifndef _PEXPERT_ARM64_GICV2_H
#define _PEXPERT_ARM64_GICV2_H

#include <stdint.h>
#include <stdbool.h>
#include <mach/vm_types.h>

#ifndef ASSEMBLER

/* Distributor register offsets */
#define GICD_CTLR               0x000
#define GICD_TYPER              0x004
#define GICD_IIDR               0x008
#define GICD_IGROUPR(n)         (0x080 + 4 * (n))
#define GICD_ISENABLER(n)       (0x100 + 4 * (n))
#define GICD_ICENABLER(n)       (0x180 + 4 * (n))
#define GICD_ISPENDR(n)         (0x200 + 4 * (n))
#define GICD_ICPENDR(n)         (0x280 + 4 * (n))
#define GICD_ISACTIVER(n)       (0x300 + 4 * (n))
#define GICD_ICACTIVER(n)       (0x380 + 4 * (n))
#define GICD_IPRIORITYR(n)      (0x400 + 4 * (n))
#define GICD_ITARGETSR(n)       (0x800 + 4 * (n))
#define GICD_ICFGR(n)           (0xC00 + 4 * (n))
#define GICD_SGIR               0xF00

#define GICD_CTLR_ENABLE        0x1
#define GICD_TYPER_ITLINES(v)   (32 * (((v) & 0x1f) + 1))

/* CPU interface register offsets */
#define GICC_CTLR               0x000
#define GICC_PMR                0x004
#define GICC_BPR                0x008
#define GICC_IAR                0x00C
#define GICC_EOIR               0x010
#define GICC_RPR                0x014
#define GICC_HPPIR              0x018
#define GICC_IIDR               0x0FC

#define GICC_CTLR_ENABLE        0x1
#define GICC_IAR_INTID_MASK     0x3ff
#define GICC_INTID_SPURIOUS     1023

/* Interrupt numbers */
#define GIC_SGI_BASE            0
#define GIC_PPI_BASE            16
#define GIC_SPI_BASE            32
#define GIC_PPI_CNTP            30      /* EL1 physical timer, the one xnu uses */
#define GIC_PPI_CNTV            27      /* EL1 virtual timer (unused) */
#define GIC_MAX_INTID           1020

typedef void (*pe_gicv2_handler_t)(void *refcon, uint32_t intid);

/*
 * Map the distributor and CPU interface from the "interrupt-controller"
 * device tree node (reg = [gicd_off, gicd_size, gicc_off, gicc_size],
 * offsets from the arm-io base), reset the distributor, and enable the
 * timer PPI. Returns 0 on failure.
 */
extern uint32_t pe_gicv2_init(vm_offset_t soc_phys);

/* True once pe_gicv2_init() succeeded. */
extern bool pe_gicv2_present(void);

/* Number of interrupt lines the distributor implements. */
extern uint32_t pe_gicv2_num_interrupts(void);

/* Per-CPU interface initialization for secondary cores (SMP, later). */
extern void pe_gicv2_cpu_init(void);

/* The handler installed through ml_install_interrupt_handler(). */
extern void pe_gicv2_irq_handler(void *target, void *refcon, void *nub, int source);

/* Hooks for the IOKit interrupt controller. */
extern int pe_gicv2_register(uint32_t intid, pe_gicv2_handler_t handler, void *refcon);
extern void pe_gicv2_unregister(uint32_t intid);
extern void pe_gicv2_enable(uint32_t intid);
extern void pe_gicv2_disable(uint32_t intid);
extern void pe_gicv2_set_priority(uint32_t intid, uint8_t priority);
extern void pe_gicv2_set_edge(uint32_t intid, bool edge);
extern void pe_gicv2_send_sgi(uint32_t intid, uint32_t cpu_mask);

#endif /* ! ASSEMBLER */

#endif /* ! _PEXPERT_ARM64_GICV2_H */
