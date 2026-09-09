/*
 * RavynVirtIOBlock: virtio-blk over virtio-mmio for the QEMU virt test bed.
 *
 * The transport nub is an IOPlatformDevice published by RavynARMPE from the
 * booter's device tree ("virtio" nodes with reg + interrupts). probe() keeps
 * only the transport that carries a block device. Requests go through one
 * split virtqueue; completion runs from the interrupt event source.
 *
 * Copyright (C) 2026 ravynOS Project. MIT licensed.
 */

#include <IOKit/IOInterrupts.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/storage/IOStorage.h>
#include <libkern/OSByteOrder.h>
#include "RavynVirtIOBlock.h"

#define super IOBlockStorageDevice
OSDefineMetaClassAndStructors(RavynVirtIOBlock, IOBlockStorageDevice);

#define VIO_LOG(fmt, ...) IOLog("RavynVirtIOBlock: " fmt "\n", ##__VA_ARGS__)
#define VIO_BARRIER()     __asm__ __volatile__("dmb ish" ::: "memory")

#define kSector 512ULL

uint32_t
RavynVirtIOBlock::reg32(uint32_t off) const
{
	return *(volatile uint32_t *)(_regs + off);
}

void
RavynVirtIOBlock::write32(uint32_t off, uint32_t value)
{
	*(volatile uint32_t *)(_regs + off) = value;
}

/* ------------------------------------------------------------------------ */

IOService *
RavynVirtIOBlock::probe(IOService *provider, SInt32 *score)
{
	IOMemoryMap *map;
	IOService *result = NULL;

	if (!super::probe(provider, score)) {
		return NULL;
	}
	map = provider->mapDeviceMemoryWithIndex(0, kIOMapInhibitCache);
	if (!map) {
		return NULL;
	}
	volatile uint8_t *regs = (volatile uint8_t *)map->getVirtualAddress();
	uint32_t magic = *(volatile uint32_t *)(regs + kVIOMagic);
	uint32_t version = *(volatile uint32_t *)(regs + kVIOVersion);
	uint32_t device = *(volatile uint32_t *)(regs + kVIODeviceID);
	if (magic == kVIOMagicValue && (version == 1 || version == 2) && device == kVIODeviceBlock) {
		result = this;
	}
	map->release();
	return result;
}

bool
RavynVirtIOBlock::resetAndNegotiate()
{
	uint32_t features;

	write32(kVIOStatus, 0);               /* reset */
	VIO_BARRIER();
	write32(kVIOStatus, kVIOStatusAcknowledge);
	write32(kVIOStatus, kVIOStatusAcknowledge | kVIOStatusDriver);

	write32(kVIODeviceFeatSel, 0);
	features = reg32(kVIODeviceFeatures);
	_features = features;
	_readOnly = (features & kVIOBlkFeatRO) != 0;
	_hasFlush = (features & kVIOBlkFeatFlush) != 0;

	/* Accept only what we use: read-only and flush from word 0, VERSION_1 for modern. */
	write32(kVIODriverFeatSel, 0);
	write32(kVIODriverFeatures, features & (kVIOBlkFeatRO | kVIOBlkFeatFlush | kVIOBlkFeatBlkSize));
	if (_version >= 2) {
		write32(kVIODriverFeatSel, 1);
		write32(kVIODriverFeatures, kVIOFeatVersion1);
		write32(kVIOStatus, kVIOStatusAcknowledge | kVIOStatusDriver | kVIOStatusFeaturesOK);
		if ((reg32(kVIOStatus) & kVIOStatusFeaturesOK) == 0) {
			VIO_LOG("device rejected our feature set");
			return false;
		}
	} else {
		write32(kVIOGuestPageSize, 4096);
	}

	_capacity = OSReadLittleInt32((void *)_regs, kVIOConfig) |
	    ((uint64_t)OSReadLittleInt32((void *)_regs, kVIOConfig + 4) << 32);
	_blockSize = kSector;
	if (features & kVIOBlkFeatBlkSize) {
		uint32_t bs = OSReadLittleInt32((void *)_regs, kVIOConfig + 0x14);
		if (bs >= 512 && bs <= 4096 && (bs & (bs - 1)) == 0) {
			_blockSize = bs;
		}
	}
	return true;
}

bool
RavynVirtIOBlock::setupQueue()
{
	uint32_t max;
	size_t descBytes, availBytes, usedOff, usedBytes, total;
	IOPhysicalAddress phys;

	write32(kVIOQueueSel, 0);
	max = reg32(kVIOQueueNumMax);
	if (max == 0) {
		VIO_LOG("queue 0 unavailable");
		return false;
	}
	_qsize = (uint16_t)(max < kVIOQueueSizeMax ? max : kVIOQueueSizeMax);

	descBytes = sizeof(struct vring_desc) * _qsize;
	availBytes = sizeof(struct vring_avail) + sizeof(uint16_t) * (_qsize + 1);
	usedOff = (descBytes + availBytes + 4095) & ~4095UL;
	usedBytes = sizeof(struct vring_used) + sizeof(struct vring_used_elem) * _qsize + sizeof(uint16_t);
	total = usedOff + usedBytes;

	_ringMem = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
	    kIODirectionInOut | kIOMemoryPhysicallyContiguous, total, 0x00000000FFFFF000ULL);
	if (!_ringMem || _ringMem->prepare() != kIOReturnSuccess) {
		VIO_LOG("no contiguous memory for the virtqueue");
		return false;
	}
	bzero(_ringMem->getBytesNoCopy(), total);
	_desc = (struct vring_desc *)_ringMem->getBytesNoCopy();
	_avail = (struct vring_avail *)((uint8_t *)_desc + descBytes);
	_used = (struct vring_used *)((uint8_t *)_desc + usedOff);
	phys = _ringMem->getPhysicalSegment(0, NULL, kIOMemoryMapperNone);

	for (uint16_t i = 0; i < _qsize; i++) {
		_desc[i].next = i + 1;
	}
	_freeHead = 0;
	_nfree = _qsize;
	_lastUsed = 0;

	write32(kVIOQueueNum, _qsize);
	if (_version >= 2) {
		write32(kVIOQueueDescLow, (uint32_t)phys);
		write32(kVIOQueueDescHigh, (uint32_t)(phys >> 32));
		write32(kVIOQueueDriverLow, (uint32_t)(phys + descBytes));
		write32(kVIOQueueDriverHigh, (uint32_t)((phys + descBytes) >> 32));
		write32(kVIOQueueDeviceLow, (uint32_t)(phys + usedOff));
		write32(kVIOQueueDeviceHigh, (uint32_t)((phys + usedOff) >> 32));
		write32(kVIOQueueReady, 1);
	} else {
		write32(kVIOQueueAlign, 4096);
		write32(kVIOQueuePFN, (uint32_t)(phys >> 12));
	}

	/* request headers and status bytes, one slot each */
	size_t reqBytes = kVIORequestSlots * (sizeof(struct virtio_blk_req_hdr) + 16);
	_reqMem = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
	    kIODirectionInOut | kIOMemoryPhysicallyContiguous, reqBytes, 0x00000000FFFFF000ULL);
	if (!_reqMem || _reqMem->prepare() != kIOReturnSuccess) {
		return false;
	}
	bzero(_reqMem->getBytesNoCopy(), reqBytes);
	_hdrs = (struct virtio_blk_req_hdr *)_reqMem->getBytesNoCopy();
	_statuses = (uint8_t *)(_hdrs + kVIORequestSlots);
	_reqPhys = _reqMem->getPhysicalSegment(0, NULL, kIOMemoryMapperNone);
	return true;
}

bool
RavynVirtIOBlock::start(IOService *provider)
{
	if (!super::start(provider)) {
		return false;
	}

	_lock = IOLockAlloc();
	_map = provider->mapDeviceMemoryWithIndex(0, kIOMapInhibitCache);
	if (!_lock || !_map) {
		return false;
	}
	_regs = (volatile uint8_t *)_map->getVirtualAddress();
	_version = reg32(kVIOVersion);

	if (!resetAndNegotiate() || !setupQueue()) {
		write32(kVIOStatus, kVIOStatusFailed);
		return false;
	}

	_workLoop = IOWorkLoop::workLoop();
	_intSource = IOInterruptEventSource::interruptEventSource(this,
	    (IOInterruptEventSource::Action)&RavynVirtIOBlock::interruptOccurred, provider, 0);
	if (!_workLoop || !_intSource || _workLoop->addEventSource(_intSource) != kIOReturnSuccess) {
		VIO_LOG("cannot register the transport interrupt");
		return false;
	}
	_intSource->enable();

	write32(kVIOStatus, reg32(kVIOStatus) | kVIOStatusDriverOK);
	{
		int type = -1;
		provider->getInterruptType(0, &type);
		kprintf("RavynVirtIOBlock: interrupt 0 type %d, status %x\n", type, reg32(kVIOStatus));
	}

	strlcpy(_vendor, "QEMU", sizeof(_vendor));
	strlcpy(_product, "virtio-blk", sizeof(_product));
	snprintf(_revision, sizeof(_revision), "v%u", _version);
	setProperty(kIOBlockStorageDeviceTypeKey, kIOBlockStorageDeviceTypeGeneric);
	setName("virtio-blk");

	VIO_LOG("virtio-mmio v%u, %llu MB (%llu sectors), block %u, queue %u%s%s",
	    _version, (_capacity * kSector) >> 20, _capacity, _blockSize, _qsize,
	    _readOnly ? ", read-only" : "", _hasFlush ? ", flush" : "");

	registerService();
	return true;
}

void
RavynVirtIOBlock::stop(IOService *provider)
{
	if (_intSource) {
		_intSource->disable();
		if (_workLoop) {
			_workLoop->removeEventSource(_intSource);
		}
	}
	if (_regs) {
		write32(kVIOStatus, 0);
	}
	super::stop(provider);
}

void
RavynVirtIOBlock::free(void)
{
	if (_intSource) { _intSource->release(); _intSource = NULL; }
	if (_workLoop) { _workLoop->release(); _workLoop = NULL; }
	if (_ringMem) { _ringMem->complete(); _ringMem->release(); _ringMem = NULL; }
	if (_reqMem) { _reqMem->complete(); _reqMem->release(); _reqMem = NULL; }
	if (_map) { _map->release(); _map = NULL; }
	if (_lock) { IOLockFree(_lock); _lock = NULL; }
	super::free();
}

/* ------------------------------------------------------------------------ */
/* virtqueue bookkeeping (call with _lock held)                             */

int
RavynVirtIOBlock::allocSlot()
{
	for (int i = 0; i < kVIORequestSlots; i++) {
		if (!_req[i].inUse) {
			_req[i].inUse = true;
			return i;
		}
	}
	return -1;
}

void
RavynVirtIOBlock::freeChain(uint16_t head)
{
	uint16_t idx = head;
	for (;;) {
		uint16_t next = _desc[idx].next;
		bool more = (_desc[idx].flags & kVringDescNext) != 0;
		_desc[idx].next = _freeHead;
		_desc[idx].flags = 0;
		_freeHead = idx;
		_nfree++;
		if (!more) {
			break;
		}
		idx = next;
	}
}

/* Build header + data + status descriptors and kick the device. _lock held. */
IOReturn
RavynVirtIOBlock::submit(int slot, uint32_t type, uint64_t sector, IOMemoryDescriptor *buffer, uint64_t bytes)
{
	Request *r = &_req[slot];
	IOByteCount offset = 0;
	uint16_t chain[kVIOQueueSizeMax];
	uint16_t n = 0;

	/* Count segments first so a request never half-allocates the ring. */
	uint16_t need = 2;
	if (buffer && bytes) {
		while (offset < bytes) {
			IOByteCount len = 0;
			if (!buffer->getPhysicalSegment(offset, &len, kIOMemoryMapperNone) || len == 0) {
				return kIOReturnVMError;
			}
			if (len > bytes - offset) {
				len = bytes - offset;
			}
			offset += len;
			need++;
		}
	}
	if (need > _qsize) {
		return kIOReturnNoSpace;
	}
	if (need > _nfree) {
		return kIOReturnBusy;
	}

	/* pull descriptors */
	for (uint16_t i = 0; i < need; i++) {
		chain[n++] = _freeHead;
		_freeHead = _desc[_freeHead].next;
		_nfree--;
	}

	_hdrs[slot].type = type;
	_hdrs[slot].reserved = 0;
	_hdrs[slot].sector = sector;
	_statuses[slot] = 0xff;

	_desc[chain[0]].addr = _reqPhys + slot * sizeof(struct virtio_blk_req_hdr);
	_desc[chain[0]].len = sizeof(struct virtio_blk_req_hdr);
	_desc[chain[0]].flags = kVringDescNext;
	_desc[chain[0]].next = chain[1];

	uint16_t di = 1;
	offset = 0;
	if (buffer && bytes) {
		bool isRead = (type == kVIOBlkTypeIn);
		while (offset < bytes) {
			IOByteCount len = 0;
			addr64_t pa = buffer->getPhysicalSegment(offset, &len, kIOMemoryMapperNone);
			if (len > bytes - offset) {
				len = bytes - offset;
			}
			_desc[chain[di]].addr = pa;
			_desc[chain[di]].len = (uint32_t)len;
			_desc[chain[di]].flags = kVringDescNext | (isRead ? kVringDescWrite : 0);
			_desc[chain[di]].next = chain[di + 1];
			di++;
			offset += len;
		}
	}
	_desc[chain[di]].addr = _reqPhys + kVIORequestSlots * sizeof(struct virtio_blk_req_hdr) + slot;
	_desc[chain[di]].len = 1;
	_desc[chain[di]].flags = kVringDescWrite;
	_desc[chain[di]].next = 0;

	r->head = chain[0];
	r->ndesc = need;
	_descOwner[chain[0]] = (uint16_t)slot;

	VIO_BARRIER();
	_avail->ring[_avail->idx % _qsize] = chain[0];
	VIO_BARRIER();
	_avail->idx++;
	VIO_BARRIER();
	write32(kVIOQueueNotify, 0);
	if (_debugCount < 8) {
		_debugCount++;
		kprintf("RavynVirtIOBlock: submit slot %d type %u sector %llu bytes %llu descs %u head %u avail %u used %u irqstat %x\n",
		    slot, type, sector, bytes, need, chain[0], _avail->idx, _used->idx, reg32(kVIOInterruptStatus));
	}
	return kIOReturnSuccess;
}

void
RavynVirtIOBlock::interruptOccurred(OSObject *owner, IOInterruptEventSource *sender, int count)
{
	RavynVirtIOBlock *self = OSDynamicCast(RavynVirtIOBlock, owner);
	if (self) {
		self->handleInterrupt(sender, count);
	}
}

void
RavynVirtIOBlock::handleInterrupt(IOInterruptEventSource *sender, int count)
{
	uint32_t status = reg32(kVIOInterruptStatus);
	if (status) {
		write32(kVIOInterruptACK, status);
	}
	if (_debugCount < 8) {
		kprintf("RavynVirtIOBlock: interrupt status %x used %u last %u\n", status, _used->idx, _lastUsed);
	}

	IOLockLock(_lock);
	VIO_BARRIER();
	while (_lastUsed != _used->idx) {
		struct vring_used_elem *e = &_used->ring[_lastUsed % _qsize];
		uint16_t head = (uint16_t)e->id;
		int slot = head < _qsize ? _descOwner[head] : -1;
		_lastUsed++;
		if (slot < 0 || slot >= kVIORequestSlots || !_req[slot].inUse || _req[slot].head != head) {
			VIO_LOG("used ring: unexpected descriptor %u", head);
			continue;
		}
		Request *r = &_req[slot];
		uint8_t st = _statuses[slot];
		freeChain(head);
		r->result = (st == 0) ? kIOReturnSuccess : (st == 2 ? kIOReturnUnsupported : kIOReturnIOError);
		r->done = true;
		if (!r->sync) {
			IOMemoryDescriptor *buffer = r->buffer;
			IOStorageCompletion completion = r->completion;
			uint64_t bytes = r->bytes;
			IOReturn result = r->result;
			r->inUse = false;
			r->buffer = NULL;
			IOLockUnlock(_lock);
			if (buffer) {
				buffer->complete();
			}
			IOStorage::complete(&completion, result, result == kIOReturnSuccess ? bytes : 0);
			IOLockLock(_lock);
		}
	}
	IOLockWakeup(_lock, this, false);
	IOLockUnlock(_lock);
}

/* ------------------------------------------------------------------------ */
/* IOBlockStorageDevice                                                     */

IOReturn
RavynVirtIOBlock::doAsyncReadWrite(IOMemoryDescriptor *buffer, UInt64 block, UInt64 nblks,
    IOStorageAttributes *attributes, IOStorageCompletion *completion)
{
	IOReturn ret;
	int slot;

	if (!buffer || nblks == 0 || block + nblks > _capacity) {
		IOStorage::complete(completion, kIOReturnBadArgument, 0);
		return kIOReturnSuccess;
	}
	bool isWrite = (buffer->getDirection() & kIODirectionOut) != 0;
	if (isWrite && _readOnly) {
		IOStorage::complete(completion, kIOReturnNotWritable, 0);
		return kIOReturnSuccess;
	}
	ret = buffer->prepare();
	if (ret != kIOReturnSuccess) {
		IOStorage::complete(completion, ret, 0);
		return kIOReturnSuccess;
	}

	IOLockLock(_lock);
	for (;;) {
		slot = allocSlot();
		if (slot >= 0) {
			Request *r = &_req[slot];
			r->sync = false;
			r->done = false;
			r->buffer = buffer;
			r->bytes = nblks * kSector;
			r->completion = completion ? *completion : IOStorageCompletion();
			ret = submit(slot, isWrite ? kVIOBlkTypeOut : kVIOBlkTypeIn, block, buffer, r->bytes);
			if (ret == kIOReturnSuccess) {
				break;
			}
			r->inUse = false;
			r->buffer = NULL;
			if (ret != kIOReturnBusy) {
				IOLockUnlock(_lock);
				buffer->complete();
				IOStorage::complete(completion, ret, 0);
				return kIOReturnSuccess;
			}
		}
		/* no slot or no descriptors: wait for completions */
		IOLockSleep(_lock, this, THREAD_UNINT);
	}
	IOLockUnlock(_lock);
	return kIOReturnSuccess;
}

IOReturn
RavynVirtIOBlock::doSynchronize(UInt64 block, UInt64 nblks, IOStorageSynchronizeOptions options)
{
	IOReturn ret;
	int slot;

	if (!_hasFlush) {
		return kIOReturnSuccess;
	}
	IOLockLock(_lock);
	for (;;) {
		slot = allocSlot();
		if (slot >= 0) {
			Request *r = &_req[slot];
			r->sync = true;
			r->done = false;
			r->buffer = NULL;
			r->bytes = 0;
			ret = submit(slot, kVIOBlkTypeFlush, 0, NULL, 0);
			if (ret == kIOReturnSuccess) {
				break;
			}
			r->inUse = false;
			if (ret != kIOReturnBusy) {
				IOLockUnlock(_lock);
				return ret;
			}
		}
		IOLockSleep(_lock, this, THREAD_UNINT);
	}
	while (!_req[slot].done) {
		IOLockSleep(_lock, this, THREAD_UNINT);
	}
	ret = _req[slot].result;
	_req[slot].inUse = false;
	IOLockUnlock(_lock);
	return ret;
}

IOReturn RavynVirtIOBlock::doEjectMedia(void)              { return kIOReturnUnsupported; }
IOReturn RavynVirtIOBlock::doFormatMedia(UInt64 byteCapacity) { return kIOReturnUnsupported; }

UInt32
RavynVirtIOBlock::doGetFormatCapacities(UInt64 *capacities, UInt32 capacitiesMaxCount) const
{
	if (capacities && capacitiesMaxCount >= 1) {
		capacities[0] = _capacity * kSector;
	}
	return 1;
}

char *RavynVirtIOBlock::getVendorString(void)               { return _vendor; }
char *RavynVirtIOBlock::getProductString(void)              { return _product; }
char *RavynVirtIOBlock::getRevisionString(void)             { return _revision; }
char *RavynVirtIOBlock::getAdditionalDeviceInfoString(void) { return (char *)""; }

IOReturn
RavynVirtIOBlock::reportBlockSize(UInt64 *blockSize)
{
	/* requests are issued in 512-byte sectors regardless of the device's preferred size */
	*blockSize = kSector;
	return kIOReturnSuccess;
}

IOReturn RavynVirtIOBlock::reportEjectability(bool *isEjectable) { *isEjectable = false; return kIOReturnSuccess; }
IOReturn RavynVirtIOBlock::reportMaxValidBlock(UInt64 *maxBlock) { *maxBlock = _capacity ? _capacity - 1 : 0; return kIOReturnSuccess; }
IOReturn RavynVirtIOBlock::reportRemovability(bool *isRemovable) { *isRemovable = false; return kIOReturnSuccess; }
IOReturn RavynVirtIOBlock::reportWriteProtection(bool *isWriteProtected) { *isWriteProtected = _readOnly; return kIOReturnSuccess; }

IOReturn
RavynVirtIOBlock::reportMediaState(bool *mediaPresent, bool *changedState)
{
	if (mediaPresent) *mediaPresent = (_capacity != 0);
	if (changedState) *changedState = false;
	return kIOReturnSuccess;
}
