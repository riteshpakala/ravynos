/*
 * RavynGIC: IOInterruptController on top of the pexpert GIC driver.
 *
 * Copyright (C) 2026 ravynOS Project. MIT licensed.
 */

#ifndef _RAVYN_GIC_H
#define _RAVYN_GIC_H

#include <IOKit/IOInterrupts.h>
#include <IOKit/IOInterruptController.h>

class RavynGIC : public IOInterruptController
{
	OSDeclareDefaultStructors(RavynGIC);

protected:
	uint32_t        _nvectors;
	const OSSymbol *_name;

	static void dispatch(void *refcon, uint32_t intid);

public:
	virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	virtual void free(void) APPLE_KEXT_OVERRIDE;

	virtual int  getVectorType(IOInterruptVectorNumber vectorNumber, IOInterruptVector *vector) APPLE_KEXT_OVERRIDE;
	virtual bool vectorCanBeShared(IOInterruptVectorNumber vectorNumber, IOInterruptVector *vector) APPLE_KEXT_OVERRIDE;
	virtual void initVector(IOInterruptVectorNumber vectorNumber, IOInterruptVector *vector) APPLE_KEXT_OVERRIDE;
	virtual void enableVector(IOInterruptVectorNumber vectorNumber, IOInterruptVector *vector) APPLE_KEXT_OVERRIDE;
	virtual void disableVectorHard(IOInterruptVectorNumber vectorNumber, IOInterruptVector *vector) APPLE_KEXT_OVERRIDE;
	virtual void causeVector(IOInterruptVectorNumber vectorNumber, IOInterruptVector *vector) APPLE_KEXT_OVERRIDE;
};

#endif /* _RAVYN_GIC_H */
