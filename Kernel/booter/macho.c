/*
 * Mach-O 64 loader for the xnu kernel image.
 *
 * The kernel is a static, position-independent Mach-O linked at
 * 0xfffffff007004000 (see xnu makedefs/MakeInc.def LDFLAGS_KERNEL_GENARM64).
 * The booter loads it unslid: every segment with a non-zero vmsize is
 * copied to phys_base + (vmaddr - vm_base), zero-filling the difference
 * between vmsize and filesize. The entry point comes from LC_UNIXTHREAD.
 */

#include "booter.h"

#define MH_MAGIC_64      0xfeedfacf
#define LC_SEGMENT_64    0x19
#define LC_UNIXTHREAD    0x5
#define ARM_THREAD_STATE64 6
#define CPU_TYPE_ARM64   0x0100000c

struct mach_header_64 {
	uint32_t magic, cputype, cpusubtype, filetype, ncmds, sizeofcmds, flags, reserved;
};

struct load_command {
	uint32_t cmd, cmdsize;
};

struct segment_command_64 {
	uint32_t cmd, cmdsize;
	char segname[16];
	uint64_t vmaddr, vmsize, fileoff, filesize;
	uint32_t maxprot, initprot, nsects, flags;
};

#define PAGE_ALIGN_UP(x) (((x) + 0xfff) & ~0xfffULL)

int
macho_inspect(const void *file, size_t size, struct kernel_image *out)
{
	const struct mach_header_64 *mh = file;
	const uint8_t *p, *end;

	if (size < sizeof(*mh) || mh->magic != MH_MAGIC_64) {
		printf("kernel: not a 64-bit Mach-O (magic 0x%08x)\n", size >= 4 ? mh->magic : 0);
		return 0;
	}
	if (mh->cputype != CPU_TYPE_ARM64) {
		printf("kernel: cputype 0x%08x is not arm64\n", mh->cputype);
		return 0;
	}
	memset(out, 0, sizeof(*out));
	out->vm_base = ~0ULL;

	p = (const uint8_t *)file + sizeof(*mh);
	end = p + mh->sizeofcmds;
	for (uint32_t i = 0; i < mh->ncmds && p + sizeof(struct load_command) <= end; i++) {
		const struct load_command *lc = (const struct load_command *)p;
		if (lc->cmdsize < sizeof(*lc) || p + lc->cmdsize > end) {
			printf("kernel: bad load command %u\n", i);
			return 0;
		}
		if (lc->cmd == LC_SEGMENT_64) {
			const struct segment_command_64 *sg = (const struct segment_command_64 *)p;
			if (sg->vmsize != 0) {
				if (sg->vmaddr < out->vm_base) {
					out->vm_base = sg->vmaddr;
				}
				if (PAGE_ALIGN_UP(sg->vmaddr + sg->vmsize) > out->vm_end) {
					out->vm_end = PAGE_ALIGN_UP(sg->vmaddr + sg->vmsize);
				}
				if (strncmp(sg->segname, "__TEXT", 16) == 0) {
					out->text_base = sg->vmaddr;
				}
				if (sg->fileoff + sg->filesize > size) {
					printf("kernel: segment %s extends past end of file\n", sg->segname);
					return 0;
				}
			}
		} else if (lc->cmd == LC_UNIXTHREAD) {
			/* flavor, count, then arm_thread_state64: x0..x28, fp, lr, sp, pc */
			const uint32_t *w = (const uint32_t *)(p + 8);
			if (w[0] == ARM_THREAD_STATE64 && lc->cmdsize >= 8 + 8 + 33 * 8) {
				uint64_t pc;
				memcpy(&pc, p + 16 + 32 * 8, 8);
				out->entry = pc;
			}
		}
		p += lc->cmdsize;
	}
	if (out->vm_base == ~0ULL || out->entry == 0) {
		printf("kernel: no loadable segments or no entry point\n");
		return 0;
	}
	return 1;
}

int
macho_load(const void *file, size_t size, const struct kernel_image *img, uint64_t vm_base, uint8_t *phys_base)
{
	const struct mach_header_64 *mh = file;
	const uint8_t *p = (const uint8_t *)file + sizeof(*mh);

	(void)size;
	for (uint32_t i = 0; i < mh->ncmds; i++) {
		const struct load_command *lc = (const struct load_command *)p;
		if (lc->cmd == LC_SEGMENT_64) {
			const struct segment_command_64 *sg = (const struct segment_command_64 *)p;
			if (sg->vmsize != 0) {
				uint8_t *dst = phys_base + (sg->vmaddr - vm_base);
				memcpy(dst, (const uint8_t *)file + sg->fileoff, sg->filesize);
				if (sg->vmsize > sg->filesize) {
					memset(dst + sg->filesize, 0, sg->vmsize - sg->filesize);
				}
				printf("  %-16s va 0x%lx size 0x%lx -> pa %p\n", sg->segname,
				    sg->vmaddr, sg->vmsize, (void *)dst);
			}
		}
		p += lc->cmdsize;
	}
	(void)img;
	return 1;
}
