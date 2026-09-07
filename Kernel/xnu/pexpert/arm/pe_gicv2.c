/*
 * ARM GICv2 (GIC-400) driver for the ravynOS arm64 bring-up targets.
 *
 * xnu's own timer path expects the ARM generic timer to arrive as an FIQ,
 * which is what Apple's AIC does. A GIC-400 seen from non-secure EL1
 * delivers every group-1 interrupt, the timer PPI included, as an IRQ. So
 * this driver takes the IRQ, acknowledges it, and routes the timer to the
 * same code sleh_fiq would have run; everything else goes to whoever
 * registered for it (the platform expert's IOInterruptController), and
 * unclaimed lines are masked so a stray device cannot storm the CPU.
 *
 * Only compiled into board configurations that define GICV2.
 */

#include <pexpert/pexpert.h>
#include <pexpert/boot.h>
#include <pexpert/protos.h>
#include <pexpert/device_tree.h>
#include <pexpert/arm64/board_config.h>
#include <machine/machine_routines.h>
#include <kern/debug.h>

#if defined(GICV2)

#include <pexpert/arm64/GICv2.h>

static vm_offset_t gicd_base;
static vm_offset_t gicc_base;
static uint32_t gic_nirqs;
static bool gic_ready;

#define GICD_RD(off)        (*(volatile uint32_t *)(gicd_base + (off)))
#define GICD_WR(off, v)     do { *(volatile uint32_t *)(gicd_base + (off)) = (v); } while (0)
#define GICC_RD(off)        (*(volatile uint32_t *)(gicc_base + (off)))
#define GICC_WR(off, v)     do { *(volatile uint32_t *)(gicc_base + (off)) = (v); } while (0)

#define GIC_DEFAULT_PRIORITY 0xA0
#define GIC_PMR_UNMASK_ALL   0xF0

struct gic_registration {
	pe_gicv2_handler_t handler;
	void *refcon;
};
static struct gic_registration gic_handlers[GIC_MAX_INTID];
static uint32_t gic_unclaimed_reports;
static uint64_t gic_timer_irqs;

/* osfmk/arm64/sleh.c: inter-processor interrupt dispatch */
extern void cpu_signal_handler(void);

static void
gicv2_distributor_reset(void)
{
	uint32_t i;

	GICD_WR(GICD_CTLR, 0);

	/* Everything off, nothing pending, all SPIs to CPU 0 at default priority. */
	for (i = 0; i < gic_nirqs / 32; i++) {
		GICD_WR(GICD_ICENABLER(i), 0xffffffff);
		GICD_WR(GICD_ICPENDR(i), 0xffffffff);
		GICD_WR(GICD_ICACTIVER(i), 0xffffffff);
	}
	for (i = 0; i < gic_nirqs / 4; i++) {
		GICD_WR(GICD_IPRIORITYR(i), 0xA0A0A0A0);
	}
	for (i = GIC_SPI_BASE / 4; i < gic_nirqs / 4; i++) {
		GICD_WR(GICD_ITARGETSR(i), 0x01010101);
	}
	/* SPIs level-triggered by default */
	for (i = GIC_SPI_BASE / 16; i < gic_nirqs / 16; i++) {
		GICD_WR(GICD_ICFGR(i), 0);
	}

	GICD_WR(GICD_CTLR, GICD_CTLR_ENABLE);
}

void
pe_gicv2_cpu_init(void)
{
	if (gicc_base == 0) {
		return;
	}
	/* Banked per CPU: SGIs and PPIs off and clean, then let everything through. */
	GICD_WR(GICD_ICENABLER(0), 0xffffffff);
	GICD_WR(GICD_ICPENDR(0), 0xffffffff);
	GICD_WR(GICD_ICACTIVER(0), 0xffffffff);
	GICD_WR(GICD_IPRIORITYR(0), 0xA0A0A0A0);
	GICD_WR(GICD_IPRIORITYR(1), 0xA0A0A0A0);
	GICD_WR(GICD_IPRIORITYR(2), 0xA0A0A0A0);
	GICD_WR(GICD_IPRIORITYR(3), 0xA0A0A0A0);
	GICD_WR(GICD_IPRIORITYR(4), 0xA0A0A0A0);
	GICD_WR(GICD_IPRIORITYR(5), 0xA0A0A0A0);
	GICD_WR(GICD_IPRIORITYR(6), 0xA0A0A0A0);
	GICD_WR(GICD_IPRIORITYR(7), 0xA0A0A0A0);

	GICC_WR(GICC_PMR, GIC_PMR_UNMASK_ALL);
	GICC_WR(GICC_BPR, 0);
	GICC_WR(GICC_CTLR, GICC_CTLR_ENABLE);

	/* The physical timer is the scheduler's heartbeat. */
	GICD_WR(GICD_ISENABLER(0), 1u << GIC_PPI_CNTP);
}

uint32_t
pe_gicv2_init(vm_offset_t soc_phys)
{
	DTEntry entryP;
	uintptr_t *reg_prop;
	uint32_t prop_size;

	if (gic_ready) {
		return 1;
	}
	if (DTFindEntry("interrupt-controller", "master", &entryP) != kSuccess) {
		kprintf("pe_gicv2_init: no interrupt-controller node\n");
		return 0;
	}
	if (DTGetProperty(entryP, "reg", (void **)&reg_prop, &prop_size) != kSuccess ||
	    prop_size < 4 * sizeof(uintptr_t)) {
		kprintf("pe_gicv2_init: interrupt-controller reg must carry GICD and GICC ranges\n");
		return 0;
	}

	gicd_base = ml_io_map(soc_phys + reg_prop[0], reg_prop[1]);
	gicc_base = ml_io_map(soc_phys + reg_prop[2], reg_prop[3]);
	if (gicd_base == 0 || gicc_base == 0) {
		kprintf("pe_gicv2_init: could not map GICD/GICC\n");
		return 0;
	}

	gic_nirqs = GICD_TYPER_ITLINES(GICD_RD(GICD_TYPER));
	if (gic_nirqs > GIC_MAX_INTID) {
		gic_nirqs = GIC_MAX_INTID;
	}
	kprintf("pe_gicv2_init: GICD 0x%lx GICC 0x%lx, %u interrupts, IIDR 0x%08x/0x%08x\n",
	    (unsigned long)gicd_base, (unsigned long)gicc_base, gic_nirqs,
	    GICD_RD(GICD_IIDR), GICC_RD(GICC_IIDR));

	gicv2_distributor_reset();
	pe_gicv2_cpu_init();
	gic_ready = true;
	return 1;
}

bool
pe_gicv2_present(void)
{
	return gic_ready;
}

uint32_t
pe_gicv2_num_interrupts(void)
{
	return gic_nirqs;
}

void
pe_gicv2_irq_handler(__unused void *target, __unused void *refcon,
    __unused void *nub, __unused int source)
{
	uint32_t iar, intid;
	int handled = 0;

	/* Drain every pending interrupt before returning to the exception path. */
	for (;;) {
		iar = GICC_RD(GICC_IAR);
		intid = iar & GICC_IAR_INTID_MASK;
		if (intid >= GIC_MAX_INTID) {
			/* 1020..1023: spurious or nothing left */
			break;
		}
		handled++;

		if (intid == GIC_PPI_CNTP) {
			if (gic_timer_irqs++ == 0) {
				kprintf("pe_gicv2: first generic timer interrupt (PPI %u) delivered as IRQ\n", intid);
			}
			ml_arm_generic_timer_irq();
		} else if (intid < GIC_PPI_BASE) {
			/* SGI: inter-processor signal (SMP comes later) */
			cpu_signal_handler();
		} else if (gic_handlers[intid].handler != NULL) {
			gic_handlers[intid].handler(gic_handlers[intid].refcon, intid);
		} else {
			/* Nobody owns this line; mask it so it cannot storm us. */
			GICD_WR(GICD_ICENABLER(intid / 32), 1u << (intid % 32));
			if (gic_unclaimed_reports < 16) {
				gic_unclaimed_reports++;
				kprintf("pe_gicv2: unclaimed interrupt %u masked\n", intid);
			}
		}

		GICC_WR(GICC_EOIR, iar);
	}
	(void)handled;
}

int
pe_gicv2_register(uint32_t intid, pe_gicv2_handler_t handler, void *refcon)
{
	if (intid >= gic_nirqs || intid < GIC_PPI_BASE) {
		return 0;
	}
	gic_handlers[intid].refcon = refcon;
	gic_handlers[intid].handler = handler;
	return 1;
}

void
pe_gicv2_unregister(uint32_t intid)
{
	if (intid >= gic_nirqs) {
		return;
	}
	pe_gicv2_disable(intid);
	gic_handlers[intid].handler = NULL;
	gic_handlers[intid].refcon = NULL;
}

void
pe_gicv2_enable(uint32_t intid)
{
	if (intid >= gic_nirqs) {
		return;
	}
	GICD_WR(GICD_ISENABLER(intid / 32), 1u << (intid % 32));
}

void
pe_gicv2_disable(uint32_t intid)
{
	if (intid >= gic_nirqs) {
		return;
	}
	GICD_WR(GICD_ICENABLER(intid / 32), 1u << (intid % 32));
}

void
pe_gicv2_set_priority(uint32_t intid, uint8_t priority)
{
	uint32_t reg, shift, value;

	if (intid >= gic_nirqs) {
		return;
	}
	reg = GICD_IPRIORITYR(intid / 4);
	shift = 8 * (intid % 4);
	value = GICD_RD(reg) & ~(0xffu << shift);
	GICD_WR(reg, value | ((uint32_t)priority << shift));
}

void
pe_gicv2_set_edge(uint32_t intid, bool edge)
{
	uint32_t reg, shift, value;

	if (intid < GIC_SPI_BASE || intid >= gic_nirqs) {
		return;
	}
	reg = GICD_ICFGR(intid / 16);
	shift = 2 * (intid % 16) + 1;
	value = GICD_RD(reg) & ~(1u << shift);
	if (edge) {
		value |= 1u << shift;
	}
	GICD_WR(reg, value);
}

void
pe_gicv2_send_sgi(uint32_t intid, uint32_t cpu_mask)
{
	if (intid >= GIC_PPI_BASE) {
		return;
	}
	GICD_WR(GICD_SGIR, ((cpu_mask & 0xff) << 16) | (intid & 0xf));
}

#endif /* GICV2 */
