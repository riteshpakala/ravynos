/*
 * ARM GICv3 operations for pexpert/arm/pe_gic.c: QEMU virt with
 * gic-version=3 (the only configuration HVF and most KVM hosts accept)
 * and, later, Apple's Virtualization.framework. Everything runs at
 * non-secure EL1: affinity routing on, group 1 non-secure interrupts, the
 * CPU interface through the ICC_* system registers.
 *
 * Only compiled into board configurations that define PE_GIC.
 */

#include <pexpert/pexpert.h>
#include <pexpert/boot.h>
#include <pexpert/protos.h>
#include <pexpert/device_tree.h>
#include <pexpert/arm64/board_config.h>
#include <machine/machine_routines.h>
#include <kern/debug.h>

#if defined(PE_GIC)

#include <pexpert/arm64/GICv3.h>

#define GICR_MAX_FRAMES 8   /* CPUs we map redistributors for */

#define GIC_ISB()   __asm__ volatile ("isb sy" ::: "memory")
#define GIC_DSB()   __asm__ volatile ("dsb sy" ::: "memory")

static vm_offset_t gicd_base;
static vm_offset_t gicr_base;       /* first redistributor frame */
static vm_offset_t gicr_mapped_size;
static uint32_t gicv3_nirqs;

#define GICD_RD(off)        (*(volatile uint32_t *)(gicd_base + (off)))
#define GICD_WR(off, v)     do { *(volatile uint32_t *)(gicd_base + (off)) = (v); } while (0)
#define GICD_WR64(off, v)   do { *(volatile uint64_t *)(gicd_base + (off)) = (v); } while (0)
#define RD32(base, off)     (*(volatile uint32_t *)((base) + (off)))
#define RD64(base, off)     (*(volatile uint64_t *)((base) + (off)))
#define WR32(base, off, v)  do { *(volatile uint32_t *)((base) + (off)) = (v); } while (0)

static uint64_t
gicv3_mpidr_affinity(void)
{
	uint64_t mpidr;
	__asm__ volatile ("mrs %0, MPIDR_EL1" : "=r"(mpidr));
	/* Aff3 lives in bits 39:32; pack as GICR_TYPER/IROUTER expect (Aff3 in 31:24) */
	return (mpidr & 0xffffff) | (((mpidr >> 32) & 0xff) << 24);
}

static void
gicd_wait_rwp(void)
{
	while (GICD_RD(GICD_V3_CTLR) & GICD_V3_CTLR_RWP) {
	}
}

/* The redistributor frame pair belonging to the calling CPU. */
static vm_offset_t
gicv3_this_redistributor(void)
{
	uint64_t want = gicv3_mpidr_affinity();
	uint32_t frames = (uint32_t)(gicr_mapped_size / GICR_FRAME_SIZE);

	for (uint32_t i = 0; i < frames; i++) {
		vm_offset_t rd = gicr_base + (vm_offset_t)i * GICR_FRAME_SIZE;
		uint64_t typer = RD64(rd, GICR_TYPER);
		if ((typer >> 32) == want) {
			return rd;
		}
		if (typer & GICR_TYPER_LAST) {
			break;
		}
	}
	kprintf("gicv3: no redistributor for affinity 0x%llx; using frame 0\n", want);
	return gicr_base;
}

static void
gicv3_cpu_init(void)
{
	vm_offset_t rd, sgi;
	uint32_t waker;

	if (gicr_base == 0) {
		return;
	}
	rd = gicv3_this_redistributor();
	sgi = rd + GICR_SGI_OFFSET;

	/* Wake the redistributor */
	waker = RD32(rd, GICR_WAKER);
	WR32(rd, GICR_WAKER, waker & ~GICR_WAKER_PROCESSOR_SLEEP);
	while (RD32(rd, GICR_WAKER) & GICR_WAKER_CHILDREN_ASLEEP) {
	}

	/* SGIs and PPIs: off, clean, group 1 NS, default priority */
	WR32(sgi, GICR_ICENABLER0, 0xffffffff);
	WR32(sgi, GICR_ICPENDR0, 0xffffffff);
	WR32(sgi, GICR_ICACTIVER0, 0xffffffff);
	WR32(sgi, GICR_IGROUPR0, 0xffffffff);
	for (uint32_t i = 0; i < 8; i++) {
		WR32(sgi, GICR_IPRIORITYR(i), 0xA0A0A0A0);
	}
	while (RD32(rd, GICR_CTLR) & GICR_CTLR_RWP) {
	}

	/* CPU interface via system registers */
	ICC_WRITE(ICC_SRE_EL1, ICC_READ(ICC_SRE_EL1) | ICC_SRE_SRE);
	GIC_ISB();
	if ((ICC_READ(ICC_SRE_EL1) & ICC_SRE_SRE) == 0) {
		panic("gicv3: ICC_SRE_EL1.SRE stays 0; EL2/EL3 did not grant system-register access");
	}
	ICC_WRITE(ICC_PMR_EL1, GIC_PMR_UNMASK_ALL);
	ICC_WRITE(ICC_BPR1_EL1, 0);
	ICC_WRITE(ICC_CTLR_EL1, 0);         /* EOImode 0: EOIR does priority drop + deactivate */
	ICC_WRITE(ICC_IGRPEN1_EL1, 1);
	GIC_ISB();

	/* The generic timer (physical, or virtual under a hypervisor) is the scheduler's heartbeat. */
	WR32(sgi, GICR_ISENABLER0, (1u << GIC_PPI_CNTP) | (1u << GIC_PPI_CNTV));
}

static void
gicv3_distributor_reset(void)
{
	uint32_t i;
	uint64_t self = gicv3_mpidr_affinity();

	GICD_WR(GICD_V3_CTLR, 0);
	gicd_wait_rwp();
	GICD_WR(GICD_V3_CTLR, GICD_V3_CTLR_ARE_NS);
	gicd_wait_rwp();

	/* SPIs: off, clean, group 1 NS, default priority, level, routed to this CPU */
	for (i = 1; i < gicv3_nirqs / 32; i++) {
		GICD_WR(GICD_V3_ICENABLER(i), 0xffffffff);
		GICD_WR(GICD_V3_ICPENDR(i), 0xffffffff);
		GICD_WR(GICD_V3_ICACTIVER(i), 0xffffffff);
		GICD_WR(GICD_V3_IGROUPR(i), 0xffffffff);
	}
	for (i = GIC_SPI_BASE / 4; i < gicv3_nirqs / 4; i++) {
		GICD_WR(GICD_V3_IPRIORITYR(i), 0xA0A0A0A0);
	}
	for (i = GIC_SPI_BASE / 16; i < gicv3_nirqs / 16; i++) {
		GICD_WR(GICD_V3_ICFGR(i), 0);
	}
	for (i = GIC_SPI_BASE; i < gicv3_nirqs; i++) {
		GICD_WR64(GICD_V3_IROUTER(i), self);
	}
	gicd_wait_rwp();

	GICD_WR(GICD_V3_CTLR, GICD_V3_CTLR_ARE_NS | GICD_V3_CTLR_ENABLE_G1NS);
	gicd_wait_rwp();
}

static uint32_t
gicv3_init(vm_offset_t soc_phys, const uintptr_t *reg, uint32_t nreg)
{
	if (nreg < 4) {
		kprintf("gicv3: reg must carry GICD and GICR ranges\n");
		return 0;
	}
	gicr_mapped_size = reg[3];
	if (gicr_mapped_size > GICR_MAX_FRAMES * GICR_FRAME_SIZE) {
		gicr_mapped_size = GICR_MAX_FRAMES * GICR_FRAME_SIZE;
	}
	gicd_base = ml_io_map(soc_phys + reg[0], reg[1]);
	gicr_base = ml_io_map(soc_phys + reg[2], gicr_mapped_size);
	if (gicd_base == 0 || gicr_base == 0) {
		kprintf("gicv3: could not map GICD/GICR\n");
		return 0;
	}

	gicv3_nirqs = GICD_V3_TYPER_ITLINES(GICD_RD(GICD_V3_TYPER));
	if (gicv3_nirqs > GIC_MAX_INTID) {
		gicv3_nirqs = GIC_MAX_INTID;
	}
	kprintf("gicv3: GICD 0x%lx GICR 0x%lx (%u frames), %u interrupts, IIDR 0x%08x/0x%08x\n",
	    (unsigned long)gicd_base, (unsigned long)gicr_base,
	    (uint32_t)(gicr_mapped_size / GICR_FRAME_SIZE), gicv3_nirqs,
	    GICD_RD(GICD_V3_IIDR), RD32(gicr_base, GICR_IIDR));

	gicv3_distributor_reset();
	return gicv3_nirqs;
}

static uint32_t
gicv3_ack(void)
{
	uint32_t raw = (uint32_t)ICC_READ(ICC_IAR1_EL1);
	GIC_DSB();
	return raw;
}

static void
gicv3_eoi(uint32_t raw)
{
	ICC_WRITE(ICC_EOIR1_EL1, raw);
	GIC_ISB();
}

static void
gicv3_enable(uint32_t intid)
{
	if (intid < GIC_SPI_BASE) {
		WR32(gicv3_this_redistributor() + GICR_SGI_OFFSET, GICR_ISENABLER0, 1u << intid);
	} else {
		GICD_WR(GICD_V3_ISENABLER(intid / 32), 1u << (intid % 32));
	}
}

static void
gicv3_disable(uint32_t intid)
{
	if (intid < GIC_SPI_BASE) {
		WR32(gicv3_this_redistributor() + GICR_SGI_OFFSET, GICR_ICENABLER0, 1u << intid);
	} else {
		GICD_WR(GICD_V3_ICENABLER(intid / 32), 1u << (intid % 32));
	}
}

static void
gicv3_set_priority(uint32_t intid, uint8_t priority)
{
	uint32_t shift = 8 * (intid % 4);
	if (intid < GIC_SPI_BASE) {
		vm_offset_t sgi = gicv3_this_redistributor() + GICR_SGI_OFFSET;
		uint32_t value = RD32(sgi, GICR_IPRIORITYR(intid / 4)) & ~(0xffu << shift);
		WR32(sgi, GICR_IPRIORITYR(intid / 4), value | ((uint32_t)priority << shift));
	} else {
		uint32_t reg = GICD_V3_IPRIORITYR(intid / 4);
		uint32_t value = GICD_RD(reg) & ~(0xffu << shift);
		GICD_WR(reg, value | ((uint32_t)priority << shift));
	}
}

static void
gicv3_set_edge(uint32_t intid, bool edge)
{
	uint32_t reg = GICD_V3_ICFGR(intid / 16);
	uint32_t shift = 2 * (intid % 16) + 1;
	uint32_t value = GICD_RD(reg) & ~(1u << shift);
	if (edge) {
		value |= 1u << shift;
	}
	GICD_WR(reg, value);
}

static void
gicv3_send_sgi(uint32_t intid, uint32_t cpu_mask)
{
	/* Target-list form: cluster 0, Aff0 CPUs in cpu_mask */
	uint64_t value = ((uint64_t)(intid & 0xf) << 24) | (cpu_mask & 0xffff);
	GIC_DSB();
	ICC_WRITE(ICC_SGI1R_EL1, value);
	GIC_ISB();
}

const struct pe_gic_ops pe_gicv3_ops = {
	.name = "GICv3",
	.version = 3,
	.intid_mask = ICC_IAR_INTID_MASK,
	.init = gicv3_init,
	.cpu_init = gicv3_cpu_init,
	.ack = gicv3_ack,
	.eoi = gicv3_eoi,
	.enable = gicv3_enable,
	.disable = gicv3_disable,
	.set_priority = gicv3_set_priority,
	.set_edge = gicv3_set_edge,
	.send_sgi = gicv3_send_sgi,
};

#endif /* PE_GIC */
