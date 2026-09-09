/*
 * RavynGIC: the IOKit face of the GIC.
 *
 * pexpert already programs the distributor/CPU interface and owns the
 * top-level IRQ handler (it needs the timer long before IOKit exists). This
 * class hands IOKit vectors to that dispatcher: enabling a vector registers a
 * per-INTID callback with pexpert, which calls back into the IOInterruptVector
 * the way AppleAPIC / AppleInterruptController do.
 *
 * Copyright (C) 2026 ravynOS Project. MIT licensed.
 */

#include <IOKit/IOLib.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IODeviceTreeSupport.h>
extern "C" {
#include <pexpert/arm64/GIC.h>
}
#include "RavynGIC.h"

#define super IOInterruptController
OSDefineMetaClassAndStructors(RavynGIC, IOInterruptController);

#define GIC_LOG(fmt, ...) IOLog("RavynGIC: " fmt "\n", ##__VA_ARGS__)

bool
RavynGIC::start(IOService *provider)
{
	if (!super::start(provider)) {
		return false;
	}
	if (!pe_gic_present()) {
		GIC_LOG("pexpert found no GIC; not starting");
		return false;
	}

	_nvectors = pe_gic_num_interrupts();
	vectors = IONew(IOInterruptVector, _nvectors);
	if (!vectors) {
		return false;
	}
	bzero(vectors, sizeof(IOInterruptVector) * _nvectors);
	for (uint32_t i = 0; i < _nvectors; i++) {
		vectors[i].interruptLock = IOLockAlloc();
		if (!vectors[i].interruptLock) {
			return false;
		}
	}

	/* Nubs name us by the phandle the booter put on the interrupt-controller node. */
	_name = IODTInterruptControllerName(provider);
	if (!_name) {
		GIC_LOG("provider has no AAPL,phandle");
		return false;
	}
	setProperty("InterruptControllerName", (OSObject *)_name);
	setProperty("VectorCount", _nvectors, 32);
	getPlatform()->registerInterruptController((OSSymbol *)_name, this);
	registerService();

	GIC_LOG("GICv%u, %u interrupt IDs, registered as %s", pe_gic_version(), _nvectors, _name->getCStringNoCopy());
	return true;
}

void
RavynGIC::free(void)
{
	if (vectors) {
		for (uint32_t i = 0; i < _nvectors; i++) {
			if (vectors[i].interruptLock) {
				IOLockFree(vectors[i].interruptLock);
			}
		}
		IODelete(vectors, IOInterruptVector, _nvectors);
		vectors = NULL;
	}
	if (_name) {
		_name->release();
		_name = NULL;
	}
	super::free();
}

int
RavynGIC::getVectorType(IOInterruptVectorNumber vectorNumber, IOInterruptVector *vector)
{
	/* PL011, virtio-mmio and the Pi 5 peripherals are all level triggered. */
	return kIOInterruptTypeLevel;
}

bool
RavynGIC::vectorCanBeShared(IOInterruptVectorNumber vectorNumber, IOInterruptVector *vector)
{
	return true;
}

void
RavynGIC::initVector(IOInterruptVectorNumber vectorNumber, IOInterruptVector *vector)
{
	if (vectorNumber >= GIC_SPI_BASE) {
		pe_gic_set_edge((uint32_t)vectorNumber, false);
	}
	pe_gic_set_priority((uint32_t)vectorNumber, GIC_DEFAULT_PRIORITY);
}

void
RavynGIC::enableVector(IOInterruptVectorNumber vectorNumber, IOInterruptVector *vector)
{
	if (!pe_gic_register((uint32_t)vectorNumber, dispatch, this)) {
		GIC_LOG("cannot route INTID %d", (int)vectorNumber);
		return;
	}
	pe_gic_enable((uint32_t)vectorNumber);
}

void
RavynGIC::disableVectorHard(IOInterruptVectorNumber vectorNumber, IOInterruptVector *vector)
{
	pe_gic_disable((uint32_t)vectorNumber);
}

void
RavynGIC::causeVector(IOInterruptVectorNumber vectorNumber, IOInterruptVector *vector)
{
	if (vectorNumber < GIC_PPI_BASE) {
		pe_gic_send_sgi((uint32_t)vectorNumber, 1);
	}
}

/* Called from pexpert's IRQ dispatcher with interrupts disabled; EOI follows our return. */
void
RavynGIC::dispatch(void *refcon, uint32_t intid)
{
	RavynGIC *self = (RavynGIC *)refcon;
	IOInterruptVector *vector;

	if (intid >= self->_nvectors) {
		return;
	}
	vector = &self->vectors[intid];
	vector->interruptActive = 1;

	if (!vector->interruptDisabledSoft && vector->interruptRegistered) {
		vector->handler(vector->target, vector->refCon, vector->nub, vector->source);
		/* the handler may ask for the source to stay masked (IOFilterInterruptEventSource) */
		if (vector->interruptDisabledSoft) {
			vector->interruptDisabledHard = 1;
			pe_gic_disable(intid);
		}
	} else {
		vector->interruptDisabledHard = 1;
		pe_gic_disable(intid);
	}

	vector->interruptActive = 0;
}
