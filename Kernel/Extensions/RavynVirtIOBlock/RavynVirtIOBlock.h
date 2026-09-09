/*
 * RavynVirtIOBlock: virtio block device over the virtio-mmio transport
 * (QEMU virt). One virtqueue, legacy (v1) and modern (v2) transports.
 *
 * Copyright (C) 2026 ravynOS Project. MIT licensed.
 */

#ifndef _RAVYN_VIRTIO_BLOCK_H
#define _RAVYN_VIRTIO_BLOCK_H

#include <IOKit/IOService.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/storage/IOBlockStorageDevice.h>

/* virtio-mmio registers */
enum {
	kVIOMagic          = 0x000,
	kVIOVersion        = 0x004,
	kVIODeviceID       = 0x008,
	kVIOVendorID       = 0x00c,
	kVIODeviceFeatures = 0x010,
	kVIODeviceFeatSel  = 0x014,
	kVIODriverFeatures = 0x020,
	kVIODriverFeatSel  = 0x024,
	kVIOGuestPageSize  = 0x028,   /* legacy */
	kVIOQueueSel       = 0x030,
	kVIOQueueNumMax    = 0x034,
	kVIOQueueNum       = 0x038,
	kVIOQueueAlign     = 0x03c,   /* legacy */
	kVIOQueuePFN       = 0x040,   /* legacy */
	kVIOQueueReady     = 0x044,   /* modern */
	kVIOQueueNotify    = 0x050,
	kVIOInterruptStatus = 0x060,
	kVIOInterruptACK   = 0x064,
	kVIOStatus         = 0x070,
	kVIOQueueDescLow   = 0x080,
	kVIOQueueDescHigh  = 0x084,
	kVIOQueueDriverLow = 0x090,
	kVIOQueueDriverHigh = 0x094,
	kVIOQueueDeviceLow = 0x0a0,
	kVIOQueueDeviceHigh = 0x0a4,
	kVIOConfig         = 0x100,
};

enum {
	kVIOStatusAcknowledge = 1,
	kVIOStatusDriver      = 2,
	kVIOStatusDriverOK    = 4,
	kVIOStatusFeaturesOK  = 8,
	kVIOStatusFailed      = 128,
};

enum {
	kVIOMagicValue   = 0x74726976,  /* "virt" */
	kVIODeviceBlock  = 2,
	kVIOBlkFeatRO    = 1u << 5,
	kVIOBlkFeatBlkSize = 1u << 6,
	kVIOBlkFeatFlush = 1u << 9,
	kVIOFeatVersion1 = 1u << 0,     /* bit 32, in the second feature word */
};

/* virtqueue (virtio 1.0 split ring, same layout the legacy transport uses) */
struct vring_desc {
	uint64_t addr;
	uint32_t len;
	uint16_t flags;
	uint16_t next;
};
enum { kVringDescNext = 1, kVringDescWrite = 2 };

struct vring_avail {
	uint16_t flags;
	uint16_t idx;
	uint16_t ring[];
};

struct vring_used_elem {
	uint32_t id;
	uint32_t len;
};

struct vring_used {
	uint16_t flags;
	uint16_t idx;
	struct vring_used_elem ring[];
};

struct virtio_blk_req_hdr {
	uint32_t type;      /* 0 read, 1 write, 4 flush */
	uint32_t reserved;
	uint64_t sector;
};
enum { kVIOBlkTypeIn = 0, kVIOBlkTypeOut = 1, kVIOBlkTypeFlush = 4 };

#define kVIOQueueSizeMax  128
#define kVIORequestSlots  32

class RavynVirtIOBlock : public IOBlockStorageDevice
{
	OSDeclareDefaultStructors(RavynVirtIOBlock);

protected:
	struct Request {
		bool                 inUse;
		bool                 sync;           /* flush: completion unused, waiter sleeps */
		uint16_t             head;           /* first descriptor of the chain */
		uint16_t             ndesc;
		IOMemoryDescriptor  *buffer;
		uint64_t             bytes;
		IOStorageCompletion  completion;
		IOReturn             result;
		bool                 done;
	};

	IOMemoryMap                *_map;
	volatile uint8_t           *_regs;
	uint32_t                    _version;
	uint32_t                    _features;
	uint64_t                    _capacity;      /* 512-byte sectors */
	uint32_t                    _blockSize;
	bool                        _readOnly;
	bool                        _hasFlush;

	IOWorkLoop                 *_workLoop;
	IOInterruptEventSource     *_intSource;
	IOLock                     *_lock;

	IOBufferMemoryDescriptor   *_ringMem;
	uint16_t                    _qsize;
	struct vring_desc          *_desc;
	struct vring_avail         *_avail;
	struct vring_used          *_used;
	uint16_t                    _freeHead;
	uint16_t                    _nfree;
	uint16_t                    _lastUsed;

	IOBufferMemoryDescriptor   *_reqMem;        /* headers + status bytes, DMA-able */
	struct virtio_blk_req_hdr  *_hdrs;
	uint8_t                    *_statuses;
	uint64_t                    _reqPhys;
	Request                     _req[kVIORequestSlots];
	uint16_t                    _descOwner[kVIOQueueSizeMax];

	int                         _debugCount;
	char                        _vendor[16];
	char                        _product[32];
	char                        _revision[8];

	uint32_t reg32(uint32_t off) const;
	void     write32(uint32_t off, uint32_t value);
	bool     resetAndNegotiate();
	bool     setupQueue();
	int      allocSlot();
	void     freeChain(uint16_t head);
	IOReturn submit(int slot, uint32_t type, uint64_t sector, IOMemoryDescriptor *buffer, uint64_t bytes);
	void     handleInterrupt(IOInterruptEventSource *sender, int count);
	static void interruptOccurred(OSObject *owner, IOInterruptEventSource *sender, int count);

public:
	virtual IOService *probe(IOService *provider, SInt32 *score) APPLE_KEXT_OVERRIDE;
	virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	virtual void stop(IOService *provider) APPLE_KEXT_OVERRIDE;
	virtual void free(void) APPLE_KEXT_OVERRIDE;

	virtual IOReturn doAsyncReadWrite(IOMemoryDescriptor *buffer, UInt64 block, UInt64 nblks,
	    IOStorageAttributes *attributes, IOStorageCompletion *completion) APPLE_KEXT_OVERRIDE;
	virtual IOReturn doSynchronize(UInt64 block, UInt64 nblks, IOStorageSynchronizeOptions options = 0) APPLE_KEXT_OVERRIDE;
	virtual IOReturn doEjectMedia(void) APPLE_KEXT_OVERRIDE;
	virtual IOReturn doFormatMedia(UInt64 byteCapacity) APPLE_KEXT_OVERRIDE;
	virtual UInt32   doGetFormatCapacities(UInt64 *capacities, UInt32 capacitiesMaxCount) const APPLE_KEXT_OVERRIDE;
	virtual char    *getVendorString(void) APPLE_KEXT_OVERRIDE;
	virtual char    *getProductString(void) APPLE_KEXT_OVERRIDE;
	virtual char    *getRevisionString(void) APPLE_KEXT_OVERRIDE;
	virtual char    *getAdditionalDeviceInfoString(void) APPLE_KEXT_OVERRIDE;
	virtual IOReturn reportBlockSize(UInt64 *blockSize) APPLE_KEXT_OVERRIDE;
	virtual IOReturn reportEjectability(bool *isEjectable) APPLE_KEXT_OVERRIDE;
	virtual IOReturn reportMaxValidBlock(UInt64 *maxBlock) APPLE_KEXT_OVERRIDE;
	virtual IOReturn reportMediaState(bool *mediaPresent, bool *changedState = 0) APPLE_KEXT_OVERRIDE;
	virtual IOReturn reportRemovability(bool *isRemovable) APPLE_KEXT_OVERRIDE;
	virtual IOReturn reportWriteProtection(bool *isWriteProtected) APPLE_KEXT_OVERRIDE;
};

#endif /* _RAVYN_VIRTIO_BLOCK_H */
