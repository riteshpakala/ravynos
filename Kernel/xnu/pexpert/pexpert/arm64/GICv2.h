/*
 * ARM GICv2 (GIC-400) register map. The driver lives in
 * pexpert/arm/pe_gicv2.c and is reached through pexpert/arm64/GIC.h.
 */

#ifndef _PEXPERT_ARM64_GICV2_H
#define _PEXPERT_ARM64_GICV2_H

#include <pexpert/arm64/GIC.h>

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

#endif /* ! ASSEMBLER */

#endif /* ! _PEXPERT_ARM64_GICV2_H */
