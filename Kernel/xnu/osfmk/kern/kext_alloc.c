/*
 * Copyright (c) 2008 Apple Inc. All rights reserved.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_START@
 *
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. The rights granted to you under the License
 * may not be used to create, or enable the creation or redistribution of,
 * unlawful or unlicensed copies of an Apple operating system, or to
 * circumvent, violate, or enable the circumvention or violation of, any
 * terms of an Apple operating system software license agreement.
 *
 * Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this file.
 *
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 */
#include <kern/assert.h>
#include <kern/debug.h>
#include <kern/kext_alloc.h>
#include <kern/misc_protos.h>

#include <mach/host_priv_server.h>
#include <mach/kern_return.h>
#include <mach/mach_vm.h>
#include <mach/vm_map.h>
#include <mach/vm_types.h>

#include <mach-o/loader.h>
#include <libkern/kernel_mach_header.h>
#include <san/kasan.h>

#define KASLR_IOREG_DEBUG 0


vm_map_t g_kext_map = 0;
#if KASLR_IOREG_DEBUG || (defined(__arm64__) && CONFIG_KEXT_BASEMENT)
/* arm64: sleh.c needs the bounds to tell basement faults from static-region faults */
mach_vm_offset_t kext_alloc_base = 0;
mach_vm_offset_t kext_alloc_max = 0;
#else
static mach_vm_offset_t kext_alloc_base = 0;
static mach_vm_offset_t kext_alloc_max = 0;
#endif
#if CONFIG_KEXT_BASEMENT
static mach_vm_offset_t kext_post_boot_base = 0;
#endif

#if defined(__arm64__) && CONFIG_KEXT_BASEMENT
/*
 * arm64 bring-up boards (BCM2712) link kexts at boot with the in-kernel
 * linker. The basement sits directly below the kernel's __TEXT, in the
 * virtual-address hole the booter leaves between virtBase and the kernel
 * image; pmap_virtual_region() leaves exactly that hole unreserved.
 */
#define KEXT_ALLOC_MAX_OFFSET   KEXT_BASEMENT_SIZE
#endif

#if defined(__arm64__) && CONFIG_KEXT_BASEMENT
/*
 * The basement ends where the kernel's static reservation begins (gVirtBase
 * rounded down to the bootstrap block size, see pmap_virtual_region()) and
 * extends KEXT_BASEMENT_SIZE below that. kmem_init() reserves it without the
 * permanent flag and kext_alloc_init() replaces that reservation with the
 * kext sub-map, which is how the x86_64 basement works too.
 */
void
kext_basement_bounds(vm_offset_t *base, vm_offset_t *top)
{
	extern vm_offset_t gVirtBase;
	kernel_segment_command_t *text = getsegbyname(SEG_TEXT);
	vm_offset_t text_start = vm_map_trunc_page(text->vmaddr, VM_MAP_PAGE_MASK(kernel_map));
	vm_offset_t static_start = gVirtBase &
	    (TEST_PAGE_SIZE_4K ? 0xFFFFFFFFFF800000ULL : 0xFFFFFFFFFE000000ULL);

	if (static_start > text_start) {
		static_start = text_start;
	}
	*top = static_start;
	*base = static_start - KEXT_BASEMENT_SIZE;
}
#endif

/*
 * On x86_64 systems, kernel extension text must remain within 2GB of the
 * kernel's text segment.  To ensure this happens, we snag 2GB of kernel VM
 * as early as possible for kext allocations.
 */
void
kext_alloc_init(void)
{
#if CONFIG_KEXT_BASEMENT
	kern_return_t rval = 0;
	kernel_segment_command_t *text = NULL;
	kernel_segment_command_t *prelinkTextSegment = NULL;
	mach_vm_offset_t text_end, text_start;
	mach_vm_size_t text_size;
	mach_vm_size_t kext_alloc_size;

	/* Determine the start of the kernel's __TEXT segment and determine the
	 * lower bound of the allocated submap for kext allocations.
	 */

	text = getsegbyname(SEG_TEXT);
	text_start = vm_map_trunc_page(text->vmaddr,
	    VM_MAP_PAGE_MASK(kernel_map));
#if defined(__arm64__)
	{
		vm_offset_t basement_base, basement_top;

		kext_basement_bounds(&basement_base, &basement_top);
		text_end = vm_map_round_page(text->vmaddr + text->vmsize,
		    VM_MAP_PAGE_MASK(kernel_map));
		text_size = text_end - text_start;

		kext_alloc_base = basement_base;
		kext_alloc_size = basement_top - basement_base;
		kext_alloc_max = basement_top;
	}
	if (kext_alloc_base < VM_MIN_KERNEL_AND_KEXT_ADDRESS) {
		panic("kext_alloc_init: basement 0x%llx below the kernel map", kext_alloc_base);
	}
#else
	text_start &= ~((512ULL * 1024 * 1024 * 1024) - 1);
	text_end = vm_map_round_page(text->vmaddr + text->vmsize,
	    VM_MAP_PAGE_MASK(kernel_map));
	text_size = text_end - text_start;

	kext_alloc_base = KEXT_ALLOC_BASE(text_end);
	kext_alloc_size = KEXT_ALLOC_SIZE(text_size);
	kext_alloc_max = kext_alloc_base + kext_alloc_size;
#endif

	/* Post boot kext allocation will start after the prelinked kexts */
	prelinkTextSegment = getsegbyname("__PRELINK_TEXT");
	if (prelinkTextSegment) {
		/* use kext_post_boot_base to start allocations past all the prelinked
		 * kexts
		 */
		kext_post_boot_base =
		    vm_map_round_page(kext_alloc_base + prelinkTextSegment->vmsize,
		    VM_MAP_PAGE_MASK(kernel_map));
	} else {
		kext_post_boot_base = kext_alloc_base;
	}

	/* Allocate the sub block of the kernel map */
	rval = kmem_suballoc(kernel_map, (vm_offset_t *) &kext_alloc_base,
	    kext_alloc_size, /* pageable */ TRUE,
	    VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE,
	    VM_MAP_KERNEL_FLAGS_NONE, VM_KERN_MEMORY_KEXT,
	    &g_kext_map);
	if (rval != KERN_SUCCESS) {
		panic("kext_alloc_init: kmem_suballoc failed 0x%x\n", rval);
	}

	if ((kext_alloc_base + kext_alloc_size) > kext_alloc_max) {
		panic("kext_alloc_init: failed to get first 2GB\n");
	}

	if (kernel_map->min_offset > kext_alloc_base) {
		kernel_map->min_offset = kext_alloc_base;
	}

	printf("kext submap [0x%lx - 0x%lx], kernel text [0x%lx - 0x%lx]\n",
	    VM_KERNEL_UNSLIDE(kext_alloc_base),
	    VM_KERNEL_UNSLIDE(kext_alloc_max),
	    VM_KERNEL_UNSLIDE(text->vmaddr),
	    VM_KERNEL_UNSLIDE(text->vmaddr + text->vmsize));

#else
	g_kext_map = kernel_map;
	kext_alloc_base = VM_MIN_KERNEL_ADDRESS;
	kext_alloc_max = VM_MAX_KERNEL_ADDRESS;
#endif /* CONFIG_KEXT_BASEMENT */
}

kern_return_t
kext_alloc(vm_offset_t *_addr, vm_size_t size, boolean_t fixed)
{
	kern_return_t rval = 0;
#if CONFIG_KEXT_BASEMENT
	mach_vm_offset_t addr = (fixed) ? *_addr : kext_post_boot_base;
#else
	mach_vm_offset_t addr = (fixed) ? *_addr : kext_alloc_base;
#endif
	int flags = (fixed) ? VM_FLAGS_FIXED : VM_FLAGS_ANYWHERE;

#if CONFIG_KEXT_BASEMENT
	/* Allocate the kext virtual memory
	 * 10608884 - use mach_vm_map since we want VM_FLAGS_ANYWHERE allocated past
	 * kext_post_boot_base (when possible).  mach_vm_allocate will always
	 * start at 0 into the map no matter what you pass in addr.  We want non
	 * fixed (post boot) kext allocations to start looking for free space
	 * just past where prelinked kexts have loaded.
	 */
	rval = mach_vm_map_kernel(g_kext_map,
	    &addr,
	    size,
	    0,
	    flags,
	    VM_MAP_KERNEL_FLAGS_NONE,
	    VM_KERN_MEMORY_KEXT,
	    MACH_PORT_NULL,
	    0,
	    TRUE,
	    VM_PROT_DEFAULT,
	    VM_PROT_ALL,
	    VM_INHERIT_DEFAULT);
	if (rval != KERN_SUCCESS) {
		printf("mach_vm_map failed - %d\n", rval);
		goto finish;
	}
#else
	rval = mach_vm_allocate_kernel(g_kext_map, &addr, size, flags, VM_KERN_MEMORY_KEXT);
	if (rval != KERN_SUCCESS) {
		printf("vm_allocate failed - %d\n", rval);
		goto finish;
	}
#endif

	/* Check that the memory is reachable by kernel text */
	if ((addr + size) > kext_alloc_max) {
		kext_free((vm_offset_t)addr, size);
		rval = KERN_INVALID_ADDRESS;
		goto finish;
	}

#if defined(__arm64__)
	/*
	 * The basement sits inside what the exception handler treats as the
	 * static region, so lazy zero-fill faults there would panic. Populate
	 * and wire the pages now; the linker writes every one of them anyway.
	 */
	rval = vm_map_wire_kernel(g_kext_map, addr, addr + size,
	    VM_PROT_READ | VM_PROT_WRITE, VM_KERN_MEMORY_KEXT, FALSE);
	if (rval != KERN_SUCCESS) {
		printf("kext_alloc: wiring 0x%llx+0x%llx failed - %d\n", addr, (uint64_t)size, rval);
		kext_free((vm_offset_t)addr, size);
		goto finish;
	}
#endif

	*_addr = (vm_offset_t)addr;
	rval = KERN_SUCCESS;
#if KASAN
	kasan_notify_address(addr, size);
#endif

finish:
	return rval;
}

void
kext_free(vm_offset_t addr, vm_size_t size)
{
	kern_return_t rval;

	rval = mach_vm_deallocate(g_kext_map, addr, size);
	assert(rval == KERN_SUCCESS);
}
