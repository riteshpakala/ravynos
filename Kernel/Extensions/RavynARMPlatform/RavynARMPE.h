/*
 * RavynARMPE: IOKit platform expert for the ravynOS arm64 bring-up boards
 * (Raspberry Pi 5 / BCM2712 and QEMU virt). The booter describes the machine
 * in an Apple-style device tree; this driver publishes its nodes as
 * IOPlatformDevice nubs so the interrupt controller, storage and console
 * drivers can match them.
 *
 * Copyright (C) 2026 ravynOS Project. MIT licensed.
 */

#ifndef _RAVYN_ARM_PE_H
#define _RAVYN_ARM_PE_H

#include <IOKit/IOPlatformExpert.h>

class RavynARMPE : public IODTPlatformExpert
{
	OSDeclareDefaultStructors(RavynARMPE);

public:
	virtual IOService *probe(IOService *provider, SInt32 *score) APPLE_KEXT_OVERRIDE;
	virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	virtual void processTopLevel(IORegistryEntry *root) APPLE_KEXT_OVERRIDE;
	virtual const char *deleteList(void) APPLE_KEXT_OVERRIDE;
	virtual const char *excludeList(void) APPLE_KEXT_OVERRIDE;
	virtual bool getMachineName(char *name, int maxLength) APPLE_KEXT_OVERRIDE;
};

#endif /* _RAVYN_ARM_PE_H */
