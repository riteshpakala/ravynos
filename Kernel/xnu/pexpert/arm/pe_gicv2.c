/*
 * ARM GICv2 (GIC-400) operations for pexpert/arm/pe_gic.c: the Raspberry
 * Pi 5's interrupt controller and QEMU virt with gic-version=2.
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

#include <pexpert/arm64/GICv2.h>

static vm_offset_t gicd_base;
static vm_offset_t gicc_base;
static uint32_t gicv2_nirqs;

#define GICD_RD(off)        (*(volatile uint32_t *)(gicd_base + (off)))
#define GICD_WR(off, v)     do { *(volatile uint32_t *)(gicd_base + (off)) = (v); } while (0)
#define GICC_RD(off)        (*(volatile uint32_t *)(gicc_base + (off)))
#define GICC_WR(off, v)     do { *(volatile uint32_t *)(gicc_base + (off)) = (v); } while (0)

static void
gicv2_distributor_reset(void)
{
	uint32_t i;

	GICD_WR(GICD_CTLR, 0);

	/* Everything off, nothing pending, all SPIs to CPU 0 at default priority. */
	for (i = 0; i < gicv2_nirqs / 32; i++) {
		GICD_WR(GICD_ICENABLER(i), 0xffffffff);
		GICD_WR(GICD_ICPENDR(i), 0xffffffff);
		GICD_WR(GICD_ICACTIVER(i), 0xffffffff);
	}
	for (i = 0; i < gicv2_nirqs / 4; i++) {
		GICD_WR(GICD_IPRIORITYR(i), 0xA0A0A0A0);
	}
	for (i = GIC_SPI_BASE / 4; i < gicv2_nirqs / 4; i++) {
		GICD_WR(GICD_ITARGETSR(i), 0x01010101);
	}
	/* SPIs level-triggered by default */
	for (i = GIC_SPI_BASE / 16; i < gicv2_nirqs / 16; i++) {
		GICD_WR(GICD_ICFGR(i), 0);
	}

	GICD_WR(GICD_CTLR, GICD_CTLR_ENABLE);
}

static void
gicv2_cpu_init(void)
{
	if (gicc_base == 0) {
		return;
	}
	/* Banked per CPU: SGIs and PPIs off and clean, then let everything through. */
	GICD_WR(GICD_ICENABLER(0), 0xffffffff);
	GICD_WR(GICD_ICPENDR(0), 0xffffffff);
	GICD_WR(GICD_ICACTIVER(0), 0xffffffff);
	for (uint32_t i = 0; i < 8; i++) {
		GICD_WR(GICD_IPRIORITYR(i), 0xA0A0A0A0);
	}

	GICC_WR(GICC_PMR, GIC_PMR_UNMASK_ALL);
	GICC_WR(GICC_BPR, 0);
	GICC_WR(GICC_CTLR, GICC_CTLR_ENABLE);

	/* The generic timer (physical, or virtual under a hypervisor) is the scheduler's heartbeat. */
	GICD_WR(GICD_ISENABLER(0), (1u << GIC_PPI_CNTP) | (1u << GIC_PPI_CNTV));
}

static uint32_t
gicv2_init(vm_offset_t soc_phys, const uintptr_t *reg, uint32_t nreg)
{
	if (nreg < 4) {
		kprintf("gicv2: reg must carry GICD and GICC ranges\n");
		return 0;
	}
	gicd_base = ml_io_map(soc_phys + reg[0], reg[1]);
	gicc_base = ml_io_map(soc_phys + reg[2], reg[3]);
	if (gicd_base == 0 || gicc_base == 0) {
		kprintf("gicv2: could not map GICD/GICC\n");
		return 0;
	}

	gicv2_nirqs = GICD_TYPER_ITLINES(GICD_RD(GICD_TYPER));
	if (gicv2_nirqs > GIC_MAX_INTID) {
		gicv2_nirqs = GIC_MAX_INTID;
	}
	kprintf("gicv2: GICD 0x%lx GICC 0x%lx, %u interrupts, IIDR 0x%08x/0x%08x\n",
	    (unsigned long)gicd_base, (unsigned long)gicc_base, gicv2_nirqs,
	    GICD_RD(GICD_IIDR), GICC_RD(GICC_IIDR));

	gicv2_distributor_reset();
	return gicv2_nirqs;
}

static uint32_t
gicv2_ack(void)
{
	return GICC_RD(GICC_IAR);
}

static void
gicv2_eoi(uint32_t raw)
{
	GICC_WR(GICC_EOIR, raw);
}

static void
gicv2_enable(uint32_t intid)
{
	GICD_WR(GICD_ISENABLER(intid / 32), 1u << (intid % 32));
}

static void
gicv2_disable(uint32_t intid)
{
	GICD_WR(GICD_ICENABLER(intid / 32), 1u << (intid % 32));
}

static void
gicv2_set_priority(uint32_t intid, uint8_t priority)
{
	uint32_t reg = GICD_IPRIORITYR(intid / 4);
	uint32_t shift = 8 * (intid % 4);
	uint32_t value = GICD_RD(reg) & ~(0xffu << shift);
	GICD_WR(reg, value | ((uint32_t)priority << shift));
}

static void
gicv2_set_edge(uint32_t intid, bool edge)
{
	uint32_t reg = GICD_ICFGR(intid / 16);
	uint32_t shift = 2 * (intid % 16) + 1;
	uint32_t value = GICD_RD(reg) & ~(1u << shift);
	if (edge) {
		value |= 1u << shift;
	}
	GICD_WR(reg, value);
}

static void
gicv2_send_sgi(uint32_t intid, uint32_t cpu_mask)
{
	GICD_WR(GICD_SGIR, ((cpu_mask & 0xff) << 16) | (intid & 0xf));
}

const struct pe_gic_ops pe_gicv2_ops = {
	.name = "GICv2",
	.version = 2,
	.intid_mask = GICC_IAR_INTID_MASK,
	.init = gicv2_init,
	.cpu_init = gicv2_cpu_init,
	.ack = gicv2_ack,
	.eoi = gicv2_eoi,
	.enable = gicv2_enable,
	.disable = gicv2_disable,
	.set_priority = gicv2_set_priority,
	.set_edge = gicv2_set_edge,
	.send_sgi = gicv2_send_sgi,
};

#endif /* PE_GIC */
