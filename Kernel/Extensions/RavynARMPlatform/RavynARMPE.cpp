/*
 * RavynARMPE: platform expert for the arm64 bring-up boards.
 *
 * Copyright (C) 2026 ravynOS Project. MIT licensed.
 */

#include <IOKit/IOLib.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOCPU.h>
#include "RavynARMPE.h"

#define super IODTPlatformExpert
OSDefineMetaClassAndStructors(RavynARMPE, IODTPlatformExpert);

#define PE_LOG(fmt, ...) IOLog("RavynARMPE: " fmt "\n", ##__VA_ARGS__)

IOService *
RavynARMPE::probe(IOService *provider, SInt32 *score)
{
	if (!super::probe(provider, score)) {
		return NULL;
	}
	if (score) {
		*score = 10000;
	}
	return this;
}

bool
RavynARMPE::start(IOService *provider)
{
	char machine[64] = "?";

	/* IOPlatformExpert::start -> configure -> processTopLevel */
	if (!super::start(provider)) {
		PE_LOG("IODTPlatformExpert::start failed");
		return false;
	}

	getMachineName(machine, sizeof(machine));
	PE_LOG("platform \"%s\" published (booter device tree)", machine);

	/*
	 * The CPU interrupt controller is what tells the kernel how many CPUs
	 * exist (ml_init_max_cpus); vm_commpage_init() blocks until someone
	 * does. Apple's platform experts create one per board; ours counts the
	 * cpu nodes the booter described (only the boot CPU for now).
	 */
	{
		unsigned cpus = 0;
		IORegistryEntry *cpusNode = provider->childFromPath("cpus", gIODTPlane);
		if (cpusNode) {
			OSIterator *iter = cpusNode->getChildIterator(gIODTPlane);
			if (iter) {
				while (iter->getNextObject()) {
					cpus++;
				}
				iter->release();
			}
			cpusNode->release();
		}
		if (cpus == 0) {
			cpus = 1;
		}
		IOCPUInterruptController *cic = new IOCPUInterruptController;
		if (cic && cic->initCPUInterruptController((int)cpus) == kIOReturnSuccess) {
			cic->attach(this);
			cic->registerCPUInterruptController();
			PE_LOG("%u cpu(s) registered with the kernel", cpus);
		} else {
			PE_LOG("could not create the CPU interrupt controller");
			if (cic) {
				cic->release();
			}
		}
	}

	/* Let AppleFileSystemDriver root by UUID if the booter ever supplies one. */
	IORegistryEntry *chosen = IORegistryEntry::fromPath("/chosen", gIODTPlane);
	if (chosen) {
		OSData *uuid = OSDynamicCast(OSData, chosen->getProperty("boot-uuid"));
		if (uuid) {
			OSString *str = OSString::withCString((const char *)uuid->getBytesNoCopy());
			if (str) {
				IOService::publishResource("boot-uuid", str);
				str->release();
			}
		}
		chosen->release();
	}

	registerService();
	return true;
}

/*
 * IODTPlatformExpert publishes the top-level device tree nodes (minus
 * excludeList). Our peripherals live under /arm-io, so publish those too,
 * directly under the platform expert: the interrupt controller, the UART and
 * every virtio transport become IOPlatformDevice nubs with "reg" resolved
 * through /arm-io's ranges and interrupts resolved through the GIC phandle.
 */
void
RavynARMPE::processTopLevel(IORegistryEntry *root)
{
	super::processTopLevel(root);

	IORegistryEntry *io = root->childFromPath("arm-io", gIODTPlane);
	if (io) {
		createNubs(this, IODTFindMatchingEntries(io, kIODTExclusive, NULL));
		io->release();
	} else {
		PE_LOG("no /arm-io in the device tree; no peripherals published");
	}
}

const char *
RavynARMPE::deleteList(void)
{
	return "('packages', 'psuedo-hid', 'psuedo-sound', 'multiboot')";
}

const char *
RavynARMPE::excludeList(void)
{
	return "('chosen', 'memory', 'openprom', 'AAPL,ROM', 'rom', 'options', 'aliases', 'defaults', 'arm-io')";
}

bool
RavynARMPE::getMachineName(char *name, int maxLength)
{
	/* the root node's "compatible" (qemu-virt, bcm2712) is what the booter set */
	return super::getMachineName(name, maxLength);
}
