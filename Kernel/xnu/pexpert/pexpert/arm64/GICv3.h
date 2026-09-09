/*
 * ARM GICv3 register map and CPU-interface system registers. The driver
 * lives in pexpert/arm/pe_gicv3.c and is reached through pexpert/arm64/GIC.h.
 * Only the non-secure EL1 view is used.
 */

#ifndef _PEXPERT_ARM64_GICV3_H
#define _PEXPERT_ARM64_GICV3_H

#include <pexpert/arm64/GIC.h>

#ifndef ASSEMBLER

/* Distributor (affinity routing enabled) */
#define GICD_V3_CTLR                0x0000
#define GICD_V3_TYPER               0x0004
#define GICD_V3_IIDR                0x0008
#define GICD_V3_IGROUPR(n)          (0x0080 + 4 * (n))
#define GICD_V3_ISENABLER(n)        (0x0100 + 4 * (n))
#define GICD_V3_ICENABLER(n)        (0x0180 + 4 * (n))
#define GICD_V3_ISPENDR(n)          (0x0200 + 4 * (n))
#define GICD_V3_ICPENDR(n)          (0x0280 + 4 * (n))
#define GICD_V3_ISACTIVER(n)        (0x0300 + 4 * (n))
#define GICD_V3_ICACTIVER(n)        (0x0380 + 4 * (n))
#define GICD_V3_IPRIORITYR(n)       (0x0400 + 4 * (n))
#define GICD_V3_ICFGR(n)            (0x0C00 + 4 * (n))
#define GICD_V3_IROUTER(n)          (0x6000 + 8 * (n))

#define GICD_V3_CTLR_ENABLE_G1NS    (1u << 1)
#define GICD_V3_CTLR_ARE_NS         (1u << 4)
#define GICD_V3_CTLR_RWP            (1u << 31)
#define GICD_V3_TYPER_ITLINES(v)    (32 * (((v) & 0x1f) + 1))

/* Redistributor: one frame pair per CPU */
#define GICR_FRAME_SIZE             0x20000
#define GICR_SGI_OFFSET             0x10000
#define GICR_CTLR                   0x0000
#define GICR_IIDR                   0x0004
#define GICR_TYPER                  0x0008      /* 64-bit */
#define GICR_WAKER                  0x0014
#define GICR_CTLR_RWP               (1u << 3)
#define GICR_TYPER_LAST             (1ull << 4)
#define GICR_WAKER_PROCESSOR_SLEEP  (1u << 1)
#define GICR_WAKER_CHILDREN_ASLEEP  (1u << 2)
/* SGI frame */
#define GICR_IGROUPR0               0x0080
#define GICR_ISENABLER0             0x0100
#define GICR_ICENABLER0             0x0180
#define GICR_ISPENDR0               0x0200
#define GICR_ICPENDR0               0x0280
#define GICR_ISACTIVER0             0x0300
#define GICR_ICACTIVER0             0x0380
#define GICR_IPRIORITYR(n)          (0x0400 + 4 * (n))
#define GICR_ICFGR0                 0x0C00
#define GICR_ICFGR1                 0x0C04

/* CPU interface system registers (S<op0>_<op1>_C<CRn>_C<CRm>_<op2>) */
#define ICC_PMR_EL1                 "S3_0_C4_C6_0"
#define ICC_IAR1_EL1                "S3_0_C12_C12_0"
#define ICC_EOIR1_EL1               "S3_0_C12_C12_1"
#define ICC_BPR1_EL1                "S3_0_C12_C12_3"
#define ICC_CTLR_EL1                "S3_0_C12_C12_4"
#define ICC_SRE_EL1                 "S3_0_C12_C12_5"
#define ICC_IGRPEN1_EL1             "S3_0_C12_C12_7"
#define ICC_SGI1R_EL1               "S3_0_C12_C11_5"

#define ICC_SRE_SRE                 (1u << 0)
#define ICC_IAR_INTID_MASK          0xffffff

#define ICC_READ(reg)   ({ uint64_t _v; __asm__ volatile ("mrs %0, " reg : "=r"(_v)); _v; })
#define ICC_WRITE(reg, v) do { uint64_t _v = (uint64_t)(v); __asm__ volatile ("msr " reg ", %0" : : "r"(_v)); } while (0)

#endif /* ! ASSEMBLER */

#endif /* ! _PEXPERT_ARM64_GICV3_H */
