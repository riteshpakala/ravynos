/*
 * Version-neutral GIC handling for the ravynOS arm64 bring-up targets.
 *
 * xnu's own timer path expects the ARM generic timer to arrive as an FIQ,
 * which is what Apple's AIC does. A GIC seen from non-secure EL1 delivers
 * every group-1 interrupt, the timer PPI included, as an IRQ. So the shared
 * handler here takes the IRQ, acknowledges it through the version-specific
 * ops, and routes the timer to the same code sleh_fiq would have run;
 * everything else goes to whoever registered for it (the platform expert's
 * IOInterruptController), and unclaimed lines are masked so a stray device
 * cannot storm the CPU.
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

#include <pexpert/arm64/GIC.h>

static const struct pe_gic_ops *gic;
static uint32_t gic_nirqs;
static bool gic_ready;

struct gic_registration {
	pe_gic_handler_t handler;
	void *refcon;
};
static struct gic_registration gic_handlers[GIC_MAX_INTID];
static uint32_t gic_unclaimed_reports;
static uint64_t gic_timer_irqs;

/* osfmk/arm64/sleh.c: inter-processor interrupt dispatch */
extern void cpu_signal_handler(void);

uint32_t
pe_gic_init(vm_offset_t soc_phys)
{
	DTEntry entryP;
	uintptr_t *reg_prop;
	uint32_t *version_prop;
	uint32_t reg_size, version_size, version = 2;

	if (gic_ready) {
		return 1;
	}
	if (DTFindEntry("interrupt-controller", "master", &entryP) != kSuccess) {
		kprintf("pe_gic_init: no interrupt-controller node\n");
		return 0;
	}
	if (DTGetProperty(entryP, "reg", (void **)&reg_prop, &reg_size) != kSuccess ||
	    reg_size < 4 * sizeof(uintptr_t)) {
		kprintf("pe_gic_init: interrupt-controller reg must carry two (base, size) ranges\n");
		return 0;
	}
	if (DTGetProperty(entryP, "gic-version", (void **)&version_prop, &version_size) == kSuccess &&
	    version_size >= sizeof(uint32_t)) {
		version = *version_prop;
	}

	switch (version) {
	case 2:
		gic = &pe_gicv2_ops;
		break;
	case 3:
		gic = &pe_gicv3_ops;
		break;
	default:
		kprintf("pe_gic_init: unsupported gic-version %u\n", version);
		return 0;
	}

	gic_nirqs = gic->init(soc_phys, reg_prop, reg_size / sizeof(uintptr_t));
	if (gic_nirqs == 0) {
		kprintf("pe_gic_init: %s initialization failed\n", gic->name);
		gic = NULL;
		return 0;
	}
	if (gic_nirqs > GIC_MAX_INTID) {
		gic_nirqs = GIC_MAX_INTID;
	}
	gic->cpu_init();
	gic_ready = true;
	kprintf("pe_gic_init: %s ready, %u interrupts\n", gic->name, gic_nirqs);
	return 1;
}

bool
pe_gic_present(void)
{
	return gic_ready;
}

uint32_t
pe_gic_version(void)
{
	return gic_ready ? gic->version : 0;
}

uint32_t
pe_gic_num_interrupts(void)
{
	return gic_nirqs;
}

void
pe_gic_cpu_init(void)
{
	if (gic != NULL) {
		gic->cpu_init();
	}
}

void
pe_gic_irq_handler(__unused void *target, __unused void *refcon,
    __unused void *nub, __unused int source)
{
	uint32_t raw, intid;

	if (!gic_ready) {
		return;
	}

	/* Drain every pending interrupt before returning to the exception path. */
	for (;;) {
		raw = gic->ack();
		intid = raw & gic->intid_mask;
		if (intid >= GIC_MAX_INTID) {
			/* 1020..1023: spurious or nothing left */
			break;
		}

		if (intid == GIC_PPI_CNTP || intid == GIC_PPI_CNTV) {
			if (gic_timer_irqs++ == 0) {
				kprintf("pe_gic: first generic timer interrupt (PPI %u) delivered as IRQ via %s\n",
				    intid, gic->name);
			}
			ml_arm_generic_timer_irq();
		} else if (intid < GIC_PPI_BASE) {
			/* SGI: inter-processor signal (SMP comes later) */
			cpu_signal_handler();
		} else if (gic_handlers[intid].handler != NULL) {
			gic_handlers[intid].handler(gic_handlers[intid].refcon, intid);
		} else {
			/* Nobody owns this line; mask it so it cannot storm us. */
			gic->disable(intid);
			if (gic_unclaimed_reports < 16) {
				gic_unclaimed_reports++;
				kprintf("pe_gic: unclaimed interrupt %u masked\n", intid);
			}
		}

		gic->eoi(raw);
	}
}

int
pe_gic_register(uint32_t intid, pe_gic_handler_t handler, void *refcon)
{
	if (!gic_ready || intid >= gic_nirqs || intid < GIC_PPI_BASE) {
		return 0;
	}
	gic_handlers[intid].refcon = refcon;
	gic_handlers[intid].handler = handler;
	return 1;
}

void
pe_gic_unregister(uint32_t intid)
{
	if (!gic_ready || intid >= gic_nirqs) {
		return;
	}
	gic->disable(intid);
	gic_handlers[intid].handler = NULL;
	gic_handlers[intid].refcon = NULL;
}

void
pe_gic_enable(uint32_t intid)
{
	if (gic_ready && intid < gic_nirqs) {
		gic->enable(intid);
	}
}

void
pe_gic_disable(uint32_t intid)
{
	if (gic_ready && intid < gic_nirqs) {
		gic->disable(intid);
	}
}

void
pe_gic_set_priority(uint32_t intid, uint8_t priority)
{
	if (gic_ready && intid < gic_nirqs) {
		gic->set_priority(intid, priority);
	}
}

void
pe_gic_set_edge(uint32_t intid, bool edge)
{
	if (gic_ready && intid >= GIC_SPI_BASE && intid < gic_nirqs) {
		gic->set_edge(intid, edge);
	}
}

void
pe_gic_send_sgi(uint32_t intid, uint32_t cpu_mask)
{
	if (gic_ready && intid < GIC_PPI_BASE) {
		gic->send_sgi(intid, cpu_mask);
	}
}

#endif /* PE_GIC */
