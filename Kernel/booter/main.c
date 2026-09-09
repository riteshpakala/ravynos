/*
 * ravynOS AArch64 UEFI booter.
 *
 * Loads the xnu kernel from the boot volume, describes the machine to it
 * the way iBoot would (Apple flattened device tree + boot_args), leaves
 * UEFI, and jumps in. Works on the Raspberry Pi 5 (rpi5-uefi firmware) and
 * on QEMU's virt machine (EDK2 ArmVirt); the hardware description comes
 * from the firmware's FDT, so nothing here is board specific.
 *
 * Boot volume layout (written by MaryPi):
 *   \EFI\BOOT\BOOTAA64.EFI            this program
 *   \ravynos\com.ravynos.boot.plist   Kernel, Kernel Flags
 *   \ravynos\kernel                   xnu (arm64 Mach-O), or \ravynos\kernelcache
 *   \bcm2712-rpi-5-b.dtb              fallback FDT when the firmware publishes none
 */

#include "booter.h"

EFI_SYSTEM_TABLE  *gST;
EFI_BOOT_SERVICES *gBS;
EFI_HANDLE         gImageHandle;

/* ---- xnu boot_args (pexpert/pexpert/arm64/boot.h) ---- */
#define BOOT_LINE_LENGTH 608
#define kBootArgsRevision2 2
#define kBootArgsVersion2  2

/*
 * xnu declares these as "unsigned long" (8 bytes in the LP64 kernel). The
 * booter targets aarch64-windows, where long is 4 bytes, so spell out the
 * width or everything after Video in boot_args lands 24 bytes early.
 */
struct Boot_Video {
	uint64_t v_baseAddr;
	uint64_t v_display;
	uint64_t v_rowBytes;
	uint64_t v_width;
	uint64_t v_height;
	uint64_t v_depth;
};

_Static_assert(sizeof(struct Boot_Video) == 48, "Boot_Video must match xnu (LP64)");

struct boot_args {
	uint16_t Revision;
	uint16_t Version;
	uint64_t virtBase;
	uint64_t physBase;
	uint64_t memSize;
	uint64_t topOfKernelData;
	struct Boot_Video Video;
	uint32_t machineType;
	void    *deviceTreeP;
	uint32_t deviceTreeLength;
	char     CommandLine[BOOT_LINE_LENGTH];
	uint64_t bootFlags;
	uint64_t memSizeActual;
};

_Static_assert(sizeof(struct boot_args) == 736, "boot_args must match xnu pexpert/arm64/boot.h");

/* xnu arm64 link layout (makedefs/MakeInc.def): the kernel expects to run
 * at virtBase + (vmaddr - virtBase) with a 32 MB low-globals window, 32 MB
 * of slide room and 48 MB of prelinked kexts below it. */
#define KERNEL_VIRT_BASE      0xfffffff000000000ULL
#define BOOTSTRAP_TABLE_SIZE  (8 * 4096)
#define ALIGN_2MB             0x200000ULL
#define ALIGN_16K             0x4000ULL
#define ALIGN_UP(x, a)        (((x) + (a) - 1) & ~((a) - 1))
#define ALIGN_DOWN(x, a)      ((x) & ~((a) - 1))
#define MAX_VIRTIO            32

/* ---- machine description gathered from the firmware ---- */
struct machine {
	const char *model;
	const char *soc;            /* "bcm2712" or "qemu-virt" */
	uint64_t uart_base, uart_size;
	unsigned gic_version;       /* 2: GICD + GICC; 3: GICD + GICR */
	uint64_t gicd_base, gicd_size;
	uint64_t gicc_base, gicc_size;   /* CPU interface (v2) or redistributors (v3) */
	uint64_t cntfrq;
	uint32_t boot_mpidr;
	unsigned ncpus;
	uint32_t cpu_mpidr[8];
	uint32_t uart_intid;        /* GIC INTID of the console UART (0 = unknown) */
	unsigned nvirtio;           /* virtio-mmio transports (QEMU virt) */
	struct {
		uint64_t base, size;
		uint32_t intid;
	} virtio[MAX_VIRTIO];
};

/* ==================================================================== */
/* UEFI helpers                                                          */
/* ==================================================================== */

bool
efi_guid_equal(const EFI_GUID *a, const EFI_GUID *b)
{
	return memcmp(a, b, sizeof(EFI_GUID)) == 0;
}

void *
efi_alloc(size_t bytes)
{
	void *p = NULL;
	EFI_STATUS st = gBS->AllocatePool(EfiLoaderData, bytes, &p);
	if (EFI_ERROR(st) || !p) {
		panic("AllocatePool(%lu) failed: 0x%lx", (unsigned long)bytes, (unsigned long)st);
	}
	memset(p, 0, bytes);
	return p;
}

void *
efi_alloc_pages(size_t bytes, EFI_MEMORY_TYPE type)
{
	EFI_PHYSICAL_ADDRESS addr = 0;
	EFI_STATUS st = gBS->AllocatePages(AllocateAnyPages, type, EFI_SIZE_TO_PAGES(bytes), &addr);
	if (EFI_ERROR(st)) {
		panic("AllocatePages(%lu) failed: 0x%lx", (unsigned long)bytes, (unsigned long)st);
	}
	return (void *)(uintptr_t)addr;
}

void *
efi_find_config_table(const EFI_GUID *guid)
{
	for (UINTN i = 0; i < gST->NumberOfTableEntries; i++) {
		if (efi_guid_equal(&gST->ConfigurationTable[i].VendorGuid, guid)) {
			return gST->ConfigurationTable[i].VendorTable;
		}
	}
	return NULL;
}

static EFI_FILE_PROTOCOL *
open_boot_volume(void)
{
	static EFI_GUID loaded_image_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
	static EFI_GUID fs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
	EFI_LOADED_IMAGE_PROTOCOL *image = NULL;
	EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = NULL;
	EFI_FILE_PROTOCOL *root = NULL;
	EFI_STATUS st;

	st = gBS->HandleProtocol(gImageHandle, &loaded_image_guid, (void **)&image);
	if (EFI_ERROR(st) || !image) {
		panic("no loaded image protocol (0x%lx)", (unsigned long)st);
	}
	st = gBS->HandleProtocol(image->DeviceHandle, &fs_guid, (void **)&fs);
	if (EFI_ERROR(st) || !fs) {
		panic("boot device has no simple file system (0x%lx)", (unsigned long)st);
	}
	st = fs->OpenVolume(fs, &root);
	if (EFI_ERROR(st) || !root) {
		panic("OpenVolume failed (0x%lx)", (unsigned long)st);
	}
	return root;
}

static EFI_FILE_PROTOCOL *gBootRoot;

static void
to_wpath(const char *path, CHAR16 *wpath, size_t cap)
{
	size_t i;

	for (i = 0; path[i] && i + 1 < cap; i++) {
		wpath[i] = (CHAR16)(path[i] == '/' ? '\\' : path[i]);
	}
	wpath[i] = 0;
}

/* Read a whole file from the boot volume. Returns 0 (with a message unless quiet) when absent. */
static int
read_file(const char *path, void **data, size_t *size, bool quiet)
{
	static EFI_GUID info_guid = EFI_FILE_INFO_GUID;
	EFI_FILE_PROTOCOL *file = NULL;
	CHAR16 wpath[260];
	uint8_t info_buf[sizeof(EFI_FILE_INFO) + 2 * 260];
	UINTN info_size = sizeof(info_buf);
	EFI_STATUS st;

	if (!gBootRoot) {
		gBootRoot = open_boot_volume();
	}
	to_wpath(path, wpath, 260);

	st = gBootRoot->Open(gBootRoot, &file, wpath, EFI_FILE_MODE_READ, 0);
	if (EFI_ERROR(st) || !file) {
		if (!quiet) {
			printf("open %s: 0x%lx\n", path, (unsigned long)st);
		}
		return 0;
	}
	st = file->GetInfo(file, &info_guid, &info_size, info_buf);
	if (EFI_ERROR(st)) {
		file->Close(file);
		printf("stat %s: 0x%lx\n", path, (unsigned long)st);
		return 0;
	}
	UINTN len = (UINTN)((EFI_FILE_INFO *)info_buf)->FileSize;
	void *buf = efi_alloc_pages(len + 1, EfiLoaderData);
	UINTN got = len;
	st = file->Read(file, &got, buf);
	file->Close(file);
	if (EFI_ERROR(st) || got != len) {
		printf("read %s: 0x%lx (%lu of %lu bytes)\n", path, (unsigned long)st, (unsigned long)got, (unsigned long)len);
		return 0;
	}
	((uint8_t *)buf)[len] = 0;
	*data = buf;
	*size = len;
	return 1;
}

int
efi_read_file(const char *path, void **data, size_t *size)
{
	return read_file(path, data, size, false);
}

int
efi_read_file_quiet(const char *path, void **data, size_t *size)
{
	return read_file(path, data, size, true);
}

/* Enumerate a directory on the boot volume. Returns the entry count or -1. */
int
efi_read_dir(const char *path, int (*cb)(const char *name, bool is_dir, void *ctx), void *ctx)
{
	EFI_FILE_PROTOCOL *dir = NULL;
	CHAR16 wpath[260];
	uint8_t buf[sizeof(EFI_FILE_INFO) + 2 * 260];
	EFI_STATUS st;
	int count = 0;

	if (!gBootRoot) {
		gBootRoot = open_boot_volume();
	}
	to_wpath(path, wpath, 260);
	st = gBootRoot->Open(gBootRoot, &dir, wpath, EFI_FILE_MODE_READ, 0);
	if (EFI_ERROR(st) || !dir) {
		return -1;
	}
	for (;;) {
		UINTN size = sizeof(buf);
		char name[128];
		size_t i;

		st = dir->Read(dir, &size, buf);
		if (EFI_ERROR(st) || size == 0) {
			break;
		}
		EFI_FILE_INFO *fi = (EFI_FILE_INFO *)buf;
		for (i = 0; fi->FileName[i] && i < sizeof(name) - 1; i++) {
			name[i] = (char)fi->FileName[i];
		}
		name[i] = 0;
		if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
			continue;
		}
		count++;
		if (cb(name, (fi->Attribute & 0x10) != 0, ctx)) {
			break;
		}
	}
	dir->Close(dir);
	return count;
}

/* ==================================================================== */
/* Hardware discovery from the firmware's FDT                            */
/* ==================================================================== */

/*
 * GIC INTID of a node's first interrupt. GIC bindings use three cells
 * (type, number, flags): SPIs start at 32, PPIs at 16.
 */
static uint32_t
fdt_intid(struct fdt *f, int node)
{
	uint32_t len;
	const uint8_t *p = fdt_getprop(f, node, "interrupts", &len);

	if (!p || len < 4) {
		return 0;
	}
	if (len >= 12) {
		uint32_t type = fdt32(p), num = fdt32(p + 4);
		return type == 0 ? num + 32 : type == 1 ? num + 16 : num;
	}
	return fdt32(p);
}

static void
discover_machine(struct fdt *f, struct machine *m)
{
	int node;
	uint32_t len;
	const char *model;

	memset(m, 0, sizeof(*m));
	m->model = "unknown";
	m->soc = "qemu-virt";

	if ((model = fdt_getprop(f, 0, "model", &len)) != NULL && len > 1) {
		m->model = model;
	}
	if (fdt_node_by_compatible(f, "brcm,bcm2712", -1) >= 0 || strstr(m->model, "Raspberry Pi 5")) {
		m->soc = "bcm2712";
	}

	/* Console UART: the first PL011, or the one "stdout-path" names. */
	node = -1;
	{
		int chosen = fdt_node_by_path(f, "/chosen");
		const char *stdout_path = chosen >= 0 ? fdt_getprop(f, chosen, "stdout-path", &len) : NULL;
		if (stdout_path) {
			char path[128];
			strlcpy(path, stdout_path, sizeof(path));
			char *colon = strchr(path, ':');
			if (colon) {
				*colon = 0;
			}
			if (path[0] == '/') {
				node = fdt_node_by_path(f, path);
			} else {
				/* alias */
				int aliases = fdt_node_by_path(f, "/aliases");
				const char *target = aliases >= 0 ? fdt_getprop(f, aliases, path, &len) : NULL;
				if (target) {
					node = fdt_node_by_path(f, target);
				}
			}
			if (node >= 0) {
				const char *compat = fdt_getprop(f, node, "compatible", &len);
				if (!compat || !strstr(compat, "pl011")) {
					node = -1;
				}
			}
		}
	}
	if (node < 0) {
		node = fdt_node_by_compatible(f, "arm,pl011", -1);
	}
	if (node >= 0) {
		fdt_reg(f, node, 0, &m->uart_base, &m->uart_size);
		if (m->uart_size < 0x1000) {
			m->uart_size = 0x1000;
		}
		m->uart_intid = fdt_intid(f, node);
	}

	/* virtio-mmio transports (QEMU virt): the block device lives on one of them */
	for (node = fdt_node_by_compatible(f, "virtio,mmio", -1); node >= 0 && m->nvirtio < MAX_VIRTIO;
	    node = fdt_node_by_compatible(f, "virtio,mmio", node)) {
		uint64_t base = 0, size = 0;
		if (!fdt_reg(f, node, 0, &base, &size) || base == 0) {
			continue;
		}
		m->virtio[m->nvirtio].base = base;
		m->virtio[m->nvirtio].size = size ? size : 0x200;
		m->virtio[m->nvirtio].intid = fdt_intid(f, node);
		m->nvirtio++;
	}

	/* GICv3: distributor then redistributor region */
	node = fdt_node_by_compatible(f, "arm,gic-v3", -1);
	if (node >= 0) {
		m->gic_version = 3;
		fdt_reg(f, node, 0, &m->gicd_base, &m->gicd_size);
		fdt_reg(f, node, 1, &m->gicc_base, &m->gicc_size);
		if (m->gicd_size < 0x10000) {
			m->gicd_size = 0x10000;
		}
		if (m->gicc_size < 0x20000) {
			m->gicc_size = 0x20000;
		}
	} else {
		/* GICv2: distributor then CPU interface */
		node = fdt_node_by_compatible(f, "arm,gic-400", -1);
		if (node < 0) {
			node = fdt_node_by_compatible(f, "arm,cortex-a15-gic", -1);
		}
		if (node < 0) {
			node = fdt_node_by_compatible(f, "arm,cortex-a7-gic", -1);
		}
		if (node >= 0) {
			m->gic_version = 2;
			fdt_reg(f, node, 0, &m->gicd_base, &m->gicd_size);
			fdt_reg(f, node, 1, &m->gicc_base, &m->gicc_size);
			if (m->gicd_size < 0x1000) {
				m->gicd_size = 0x1000;
			}
			if (m->gicc_size < 0x2000) {
				m->gicc_size = 0x2000;
			}
		}
	}

	/* CPUs */
	{
		int cpus = fdt_node_by_path(f, "/cpus");
		int cpu = cpus >= 0 ? fdt_first_subnode(f, cpus) : -1;
		while (cpu >= 0 && m->ncpus < 8) {
			const char *type = fdt_getprop(f, cpu, "device_type", &len);
			if (type && strcmp(type, "cpu") == 0) {
				uint64_t reg = 0;
				fdt_reg(f, cpu, 0, &reg, NULL);
				m->cpu_mpidr[m->ncpus++] = (uint32_t)(reg & 0xffffff);
			}
			cpu = fdt_next_subnode(f, cpu);
		}
	}
	m->cntfrq = machine_cntfrq();
	m->boot_mpidr = (uint32_t)(machine_mpidr() & 0xffffff);
}

/* ==================================================================== */
/* Memory                                                                */
/* ==================================================================== */

struct memmap {
	EFI_MEMORY_DESCRIPTOR *map;
	UINTN size;
	UINTN capacity;
	UINTN key;
	UINTN desc_size;
	UINT32 desc_version;
};

/*
 * Fetch the memory map into mm->map, growing the buffer as needed. Every
 * allocation can add descriptors, so this loops until the map fits; the
 * buffer keeps generous slack so the final call before ExitBootServices
 * (which may not allocate) succeeds on the first try.
 */
static void
get_memory_map(struct memmap *mm, bool allocate)
{
	EFI_STATUS st;

	if (allocate && mm->map == NULL) {
		mm->size = 0;
		st = gBS->GetMemoryMap(&mm->size, NULL, &mm->key, &mm->desc_size, &mm->desc_version);
		if (st != EFI_BUFFER_TOO_SMALL) {
			panic("GetMemoryMap size query: 0x%lx", (unsigned long)st);
		}
		mm->size += 32 * mm->desc_size;
		mm->capacity = mm->size;
		mm->map = efi_alloc(mm->size);
	}
	for (int attempt = 0; attempt < 8; attempt++) {
		mm->size = mm->capacity;
		st = gBS->GetMemoryMap(&mm->size, mm->map, &mm->key, &mm->desc_size, &mm->desc_version);
		if (!EFI_ERROR(st)) {
			return;
		}
		if (st != EFI_BUFFER_TOO_SMALL || !allocate) {
			break;
		}
		gBS->FreePool(mm->map);
		mm->capacity = mm->size + 32 * mm->desc_size;
		mm->map = efi_alloc(mm->capacity);
	}
	panic("GetMemoryMap: 0x%lx (need %lu bytes, have %lu)", (unsigned long)st,
	    (unsigned long)mm->size, (unsigned long)mm->capacity);
}

static bool
usable_ram(UINT32 type)
{
	switch (type) {
	case EfiLoaderCode:
	case EfiLoaderData:
	case EfiBootServicesCode:
	case EfiBootServicesData:
	case EfiRuntimeServicesCode:
	case EfiRuntimeServicesData:
	case EfiConventionalMemory:
	case EfiACPIReclaimMemory:
	case EfiACPIMemoryNVS:
		return true;
	default:
		return false;
	}
}

/* Largest run of contiguous RAM the kernel may own; 2 MB aligned. */
static void
choose_ram(struct memmap *mm, uint64_t *base, uint64_t *size, uint64_t *total)
{
	uint64_t run_start = 0, run_end = 0;
	uint64_t best_start = 0, best_end = 0;
	uint64_t sum = 0;
	bool in_run = false;

	for (UINTN off = 0; off < mm->size; off += mm->desc_size) {
		EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR *)((uint8_t *)mm->map + off);
		uint64_t start = d->PhysicalStart;
		uint64_t end = start + d->NumberOfPages * EFI_PAGE_SIZE;
		if (usable_ram(d->Type)) {
			sum += end - start;
			if (in_run && start == run_end) {
				run_end = end;
			} else {
				if (in_run && run_end - run_start > best_end - best_start) {
					best_start = run_start;
					best_end = run_end;
				}
				run_start = start;
				run_end = end;
				in_run = true;
			}
		}
	}
	if (in_run && run_end - run_start > best_end - best_start) {
		best_start = run_start;
		best_end = run_end;
	}
	*base = ALIGN_UP(best_start, ALIGN_2MB);
	*size = ALIGN_DOWN(best_end, ALIGN_2MB) - *base;
	*total = sum;
}

/* True when [start, end) lies entirely inside EfiConventionalMemory. */
static bool
range_is_free(struct memmap *mm, uint64_t start, uint64_t end)
{
	uint64_t covered = start;

	while (covered < end) {
		bool advanced = false;
		for (UINTN off = 0; off < mm->size; off += mm->desc_size) {
			EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR *)((uint8_t *)mm->map + off);
			uint64_t ds = d->PhysicalStart;
			uint64_t de = ds + d->NumberOfPages * EFI_PAGE_SIZE;
			if (d->Type == EfiConventionalMemory && ds <= covered && covered < de) {
				covered = de;
				advanced = true;
				break;
			}
		}
		if (!advanced) {
			return false;
		}
	}
	return true;
}

static void
print_memory_map(struct memmap *mm)
{
	static const char *names[] = {
		"Reserved", "LoaderCode", "LoaderData", "BSCode", "BSData", "RTCode", "RTData",
		"Conventional", "Unusable", "ACPIReclaim", "ACPINVS", "MMIO", "MMIOPort", "PalCode", "Persistent",
	};
	for (UINTN off = 0; off < mm->size; off += mm->desc_size) {
		EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR *)((uint8_t *)mm->map + off);
		const char *name = d->Type < sizeof(names) / sizeof(names[0]) ? names[d->Type] : "?";
		printf("  %012lx-%012lx %-12s\n", d->PhysicalStart,
		    d->PhysicalStart + d->NumberOfPages * EFI_PAGE_SIZE, name);
	}
}

/* ==================================================================== */
/* Entropy: xnu refuses to boot without 64 bytes of random-seed          */
/* ==================================================================== */

#define RANDOM_SEED_BYTES 64

static void
fill_random_seed(uint8_t *seed, size_t len)
{
	static EFI_GUID rng_guid = EFI_RNG_PROTOCOL_GUID;
	EFI_RNG_PROTOCOL *rng = NULL;

	if (!EFI_ERROR(gBS->LocateProtocol(&rng_guid, NULL, (void **)&rng)) && rng &&
	    !EFI_ERROR(rng->GetRNG(rng, NULL, len, seed))) {
		printf("random-seed: %lu bytes from the UEFI RNG protocol\n", (unsigned long)len);
		return;
	}

	/*
	 * No hardware RNG exposed (QEMU without virtio-rng, or older rpi5-uefi).
	 * Mix the cycle counter, the physical counter and a few CPU ids through
	 * a xorshift generator. This is NOT cryptographic entropy; it only lets
	 * the bring-up kernel get past its boot-time seeding.
	 */
	uint64_t x = machine_cntfrq() ^ (machine_midr() << 32) ^ machine_mpidr();
	uint64_t cnt;
	__asm__ volatile ("mrs %0, CNTPCT_EL0" : "=r"(cnt));
	x ^= cnt * 0x9E3779B97F4A7C15ULL;
	for (size_t i = 0; i < len; i++) {
		__asm__ volatile ("mrs %0, CNTPCT_EL0" : "=r"(cnt));
		x ^= cnt;
		x ^= x << 13;
		x ^= x >> 7;
		x ^= x << 17;
		seed[i] = (uint8_t)(x >> ((i % 8) * 8));
	}
	printf("random-seed: no UEFI RNG protocol; %lu bytes of weak counter-derived entropy (bring-up only)\n", (unsigned long)len);
}

/* ==================================================================== */
/* Kernel extensions                                                     */
/*                                                                       */
/* Every *.kext under \ravynos\Extensions (and the kernel's own          */
/* System.kext\PlugIns) is read into memory and handed to xnu through   */
/* /chosen/memory-map "Driver-*" entries, the way boot.efi did before    */
/* prelinked kernelcaches. The kernel links them at boot with kxld.      */
/* ==================================================================== */

#define MAX_KEXTS      48
#define EXTENSIONS_DIR "\\ravynos\\Extensions"

struct kext_file {
	char     name[64];      /* bundle directory name without .kext */
	char     path[160];     /* bundle path reported to the kernel */
	void    *plist;
	size_t   plist_len;
	void    *exec;
	size_t   exec_len;
	uint64_t offset;        /* block start, relative to the kext area */
	uint64_t block_len;
	uint64_t phys;          /* block start once placed */
};

struct kext_set {
	struct kext_file k[MAX_KEXTS];
	unsigned count;
	uint64_t span;          /* bytes of the whole area, 16K aligned */
};

/* Mirrors xnu's _BooterKextFileInfo (libkern/c++/OSKext.cpp). */
struct booter_kext_info {
	uint32_t infoDictPhysAddr, infoDictLength;
	uint32_t executablePhysAddr, executableLength;
	uint32_t bundlePathPhysAddr, bundlePathLength;
};

#define KEXT_INFO_SIZE 64   /* info struct, padded; the bundle path follows */

struct kext_scan {
	struct kext_set *set;
	const char *dir;
};

static bool
ends_with(const char *s, const char *suffix)
{
	size_t n = strlen(s), m = strlen(suffix);
	return n >= m && memcmp(s + n - m, suffix, m) == 0;
}

static int
kext_dir_entry(const char *name, bool is_dir, void *ctx)
{
	struct kext_scan *scan = ctx;
	struct kext_set *set = scan->set;
	char base[200], file_path[260], exec_name[64];
	size_t n = strlen(name);
	bool contents = true;

	if (!is_dir || n <= 5 || !ends_with(name, ".kext")) {
		return 0;
	}
	if (set->count >= MAX_KEXTS) {
		printf("kext: more than %d bundles; skipping %s\n", MAX_KEXTS, name);
		return 0;
	}
	struct kext_file *k = &set->k[set->count];
	memset(k, 0, sizeof(*k));
	if (n - 5 >= sizeof(k->name)) {
		n = sizeof(k->name) - 1 + 5;
	}
	memcpy(k->name, name, n - 5);
	k->name[n - 5] = 0;
	snprintf(base, sizeof(base), "%s\\%s", scan->dir, name);

	/* Bundle layout: Contents\Info.plist + Contents\MacOS\<exe>, or flat (System.kext plug-ins). */
	snprintf(file_path, sizeof(file_path), "%s\\Contents\\Info.plist", base);
	if (!efi_read_file_quiet(file_path, &k->plist, &k->plist_len)) {
		contents = false;
		snprintf(file_path, sizeof(file_path), "%s\\Info.plist", base);
		if (!efi_read_file_quiet(file_path, &k->plist, &k->plist_len)) {
			printf("kext: %s has no Info.plist; skipped\n", name);
			return 0;
		}
	}
	if (plist_get_string(k->plist, k->plist_len, "CFBundleExecutable", exec_name, sizeof(exec_name))) {
		if (contents) {
			snprintf(file_path, sizeof(file_path), "%s\\Contents\\MacOS\\%s", base, exec_name);
		} else {
			snprintf(file_path, sizeof(file_path), "%s\\%s", base, exec_name);
		}
		if (!efi_read_file_quiet(file_path, &k->exec, &k->exec_len)) {
			printf("kext: %s: executable %s missing; skipped\n", name, exec_name);
			return 0;
		}
	}
	snprintf(k->path, sizeof(k->path), "/System/Library/Extensions/%s", name);
	set->count++;

	/* Nested plug-ins (hfs.kext/Contents/PlugIns/hfs_encodings.kext) ship with their parent. */
	if (contents) {
		char plugins[260];
		struct kext_scan nested = { set, plugins };
		snprintf(plugins, sizeof(plugins), "%s\\Contents\\PlugIns", base);
		efi_read_dir(plugins, kext_dir_entry, &nested);
	}
	return 0;
}

static void
load_kexts(struct kext_set *set)
{
	static const char *dirs[] = { EXTENSIONS_DIR, EXTENSIONS_DIR "\\System.kext\\PlugIns" };
	uint64_t cur = 0;

	memset(set, 0, sizeof(*set));
	for (unsigned d = 0; d < sizeof(dirs) / sizeof(dirs[0]); d++) {
		struct kext_scan scan = { set, dirs[d] };
		if (efi_read_dir(dirs[d], kext_dir_entry, &scan) < 0) {
			printf("kexts: no %s\n", dirs[d]);
		}
	}

	/* Lay the blocks out: [info + path][plist][executable, 16K aligned] */
	for (unsigned i = 0; i < set->count; i++) {
		struct kext_file *k = &set->k[i];
		uint64_t blk = ALIGN_UP(cur, ALIGN_16K);
		uint64_t plist_off = ALIGN_UP(KEXT_INFO_SIZE + strlen(k->path) + 1, 16);
		uint64_t end = blk + plist_off + k->plist_len + 1;
		if (k->exec) {
			end = ALIGN_UP(end, ALIGN_16K) + k->exec_len;
		}
		k->offset = blk;
		k->block_len = end - blk;
		cur = end;
	}
	set->span = ALIGN_UP(cur, ALIGN_16K);
	if (set->count) {
		printf("kexts: %u bundle(s) from %s, %lu KB\n", set->count, EXTENSIONS_DIR, (unsigned long)(set->span >> 10));
	} else {
		printf("kexts: none (the kernel will stop at the platform driver panic)\n");
	}
}

/* Copy the bundles into their final blocks at physical address base. */
static void
place_kexts(struct kext_set *set, uint64_t base)
{
	if (set->count && base + set->span > 0xFFFFFFFFULL) {
		panic("kernel extensions must sit below 4 GiB (xnu keeps 32-bit addresses); RAM run starts at 0x%lx", base);
	}
	for (unsigned i = 0; i < set->count; i++) {
		struct kext_file *k = &set->k[i];
		uint8_t *blk = (uint8_t *)(uintptr_t)(base + k->offset);
		struct booter_kext_info *info = (struct booter_kext_info *)blk;
		size_t plen = strlen(k->path) + 1;
		uint64_t plist_off = ALIGN_UP(KEXT_INFO_SIZE + plen, 16);
		uint64_t exec_off = 0;

		k->phys = base + k->offset;
		memcpy(blk + KEXT_INFO_SIZE, k->path, plen);
		memcpy(blk + plist_off, k->plist, k->plist_len);
		blk[plist_off + k->plist_len] = 0;
		if (k->exec) {
			exec_off = ALIGN_UP(plist_off + k->plist_len + 1, ALIGN_16K);
			memcpy(blk + exec_off, k->exec, k->exec_len);
		}
		info->infoDictPhysAddr = (uint32_t)(k->phys + plist_off);
		info->infoDictLength = (uint32_t)k->plist_len;
		info->executablePhysAddr = k->exec ? (uint32_t)(k->phys + exec_off) : 0;
		info->executableLength = (uint32_t)k->exec_len;
		info->bundlePathPhysAddr = (uint32_t)(k->phys + KEXT_INFO_SIZE);
		info->bundlePathLength = (uint32_t)plen;
	}
}

static void
kext_entry_name(const struct kext_file *k, char *buf, size_t cap)
{
	/* device tree property names are limited to 31 characters */
	snprintf(buf, cap, "Driver-%s", k->name);
	if (strlen(buf) > 31) {
		buf[31] = 0;
	}
}

/* ==================================================================== */
/* Apple device tree                                                     */
/* ==================================================================== */

#define GIC_PHANDLE 1

static void *
build_device_tree(const struct machine *m, const char *cmdline, const struct kext_set *kexts, size_t *len_out)
{
	struct adt *t = adt_create(64 * 1024);
	uint64_t soc_base, soc_end, ranges[3], reg[4];
	uint32_t zero = 0, one = 1, two = 2;
	char buf[128];

	/* /arm-io ranges: every peripheral is an offset from soc_base */
	soc_base = m->uart_base;
	if (m->gicd_base && m->gicd_base < soc_base) {
		soc_base = m->gicd_base;
	}
	if (m->gicc_base && m->gicc_base < soc_base) {
		soc_base = m->gicc_base;
	}
	for (unsigned i = 0; i < m->nvirtio; i++) {
		if (m->virtio[i].base < soc_base) {
			soc_base = m->virtio[i].base;
		}
	}
	soc_base = ALIGN_DOWN(soc_base, 0x100000);
	soc_end = m->uart_base + m->uart_size;
	if (m->gicd_base + m->gicd_size > soc_end) {
		soc_end = m->gicd_base + m->gicd_size;
	}
	if (m->gicc_base + m->gicc_size > soc_end) {
		soc_end = m->gicc_base + m->gicc_size;
	}
	for (unsigned i = 0; i < m->nvirtio; i++) {
		if (m->virtio[i].base + m->virtio[i].size > soc_end) {
			soc_end = m->virtio[i].base + m->virtio[i].size;
		}
	}
	soc_end = ALIGN_UP(soc_end, 0x1000);

	adt_begin_node(t, "device-tree");
	/* reg/ranges use 64-bit address and size cells throughout */
	adt_prop_u32(t, "#address-cells", two);
	adt_prop_u32(t, "#size-cells", two);
	adt_prop_str(t, "model", m->model);
	adt_prop_str(t, "target-type", strcmp(m->soc, "bcm2712") == 0 ? "Pi5" : "QEMUvirt");
	snprintf(buf, sizeof(buf), "%s", m->soc);
	adt_prop_str(t, "compatible", buf);
	adt_prop_str(t, "platform-name", m->soc);
	adt_prop_str(t, "firmware-version", "ravynOS booter " BOOTER_VERSION);
	adt_prop_str(t, "secure-root-prefix", "");

	adt_begin_node(t, "chosen");
	adt_prop_u32(t, "debug-enabled", one);
	adt_prop_str(t, "boot-args", cmdline);
	{
		uint8_t seed[RANDOM_SEED_BYTES];
		fill_random_seed(seed, sizeof(seed));
		adt_prop(t, "random-seed", seed, sizeof(seed));
	}
	adt_prop_str(t, "booter-name", "ravynOS booter");
	adt_prop_str(t, "booter-version", BOOTER_VERSION);
	adt_begin_node(t, "memory-map");
	/* patched with real addresses once the tree and boot_args are placed */
	memset(reg, 0, sizeof(reg));
	adt_prop(t, "DeviceTree", reg, 16);
	adt_prop(t, "BootArgs", reg, 16);
	/* Driver-* entries are {paddr, length} as 32-bit pairs (xnu _DeviceTreeBuffer) */
	for (unsigned i = 0; kexts && i < kexts->count; i++) {
		kext_entry_name(&kexts->k[i], buf, sizeof(buf));
		adt_prop(t, buf, reg, 8);
	}
	adt_end_node(t); /* memory-map */
	adt_end_node(t); /* chosen */

	adt_begin_node(t, "cpus");
	for (unsigned i = 0; i < (m->ncpus ? m->ncpus : 1); i++) {
		uint32_t mpidr = m->ncpus ? m->cpu_mpidr[i] : m->boot_mpidr;
		bool is_boot = mpidr == m->boot_mpidr;
		/* Only the boot CPU is described for now: no PSCI CPU_ON support yet. */
		if (!is_boot) {
			continue;
		}
		snprintf(buf, sizeof(buf), "cpu%u", 0);
		adt_begin_node(t, buf);
		adt_prop_str(t, "device_type", "cpu");
		adt_prop_u32(t, "cpu-id", 0);
		adt_prop_u32(t, "reg", mpidr);
		adt_prop_u32(t, "cluster-id", zero);
		adt_prop_str(t, "state", "running");
		adt_prop_u32(t, "timebase-frequency", (uint32_t)m->cntfrq);
		adt_prop_u32(t, "fixed-frequency", (uint32_t)m->cntfrq);
		adt_prop_u32(t, "clock-frequency", strcmp(m->soc, "bcm2712") == 0 ? 2400000000u : 1000000000u);
		adt_prop_u32(t, "bus-frequency", 100000000u);
		adt_prop_u32(t, "peripheral-frequency", 100000000u);
		adt_prop_u32(t, "memory-frequency", 100000000u);
		adt_end_node(t);
	}
	adt_end_node(t); /* cpus */

	adt_begin_node(t, "arm-io");
	snprintf(buf, sizeof(buf), "%s-io", m->soc);
	adt_prop_str(t, "device_type", buf);
	adt_prop_u32(t, "#address-cells", two);
	adt_prop_u32(t, "#size-cells", two);
	ranges[0] = 0;
	ranges[1] = soc_base;
	ranges[2] = soc_end - soc_base;
	adt_prop(t, "ranges", ranges, sizeof(ranges));
	adt_prop_u32(t, "chip-revision", zero);
	adt_prop_str(t, "compatible", m->soc);

	if (m->uart_base) {
		adt_begin_node(t, "pl011");
		adt_prop_str(t, "compatible", "arm,pl011");
		reg[0] = m->uart_base - soc_base;
		reg[1] = m->uart_size;
		adt_prop(t, "reg", reg, 16);
		if (m->uart_intid) {
			adt_prop_u32(t, "interrupts", m->uart_intid);
			adt_prop_u32(t, "interrupt-parent", GIC_PHANDLE);
		}
		adt_end_node(t);
	}
	if (m->gicd_base) {
		adt_begin_node(t, "interrupt-controller");
		adt_prop_str(t, "compatible", m->gic_version == 3 ? "arm,gic-v3" : "arm,gic-400");
		adt_prop_str(t, "interrupt-controller", "master");
		adt_prop_u32(t, "gic-version", m->gic_version);
		/* IOKit: interrupt specifiers are one cell (the GIC INTID); nubs refer to us by phandle */
		adt_prop_u32(t, "AAPL,phandle", GIC_PHANDLE);
		adt_prop_u32(t, "#interrupt-cells", one);
		reg[0] = m->gicd_base - soc_base;
		reg[1] = m->gicd_size;
		reg[2] = m->gicc_base - soc_base;
		reg[3] = m->gicc_size;
		adt_prop(t, "reg", reg, 32);
		adt_end_node(t);
	}
	for (unsigned i = 0; i < m->nvirtio; i++) {
		adt_begin_node(t, "virtio");
		adt_prop_str(t, "compatible", "virtio,mmio");
		adt_prop_str(t, "device_type", "virtio-mmio");
		reg[0] = m->virtio[i].base - soc_base;
		reg[1] = m->virtio[i].size;
		adt_prop(t, "reg", reg, 16);
		adt_prop_u32(t, "interrupts", m->virtio[i].intid);
		adt_prop_u32(t, "interrupt-parent", GIC_PHANDLE);
		adt_end_node(t);
	}
	adt_end_node(t); /* arm-io */

	adt_begin_node(t, "defaults");
	adt_end_node(t);

	adt_end_node(t); /* device-tree */
	return adt_finish(t, len_out);
}

/* ==================================================================== */
/* Video                                                                 */
/* ==================================================================== */

static void
fill_video(struct Boot_Video *v, bool text_mode)
{
	static EFI_GUID gop_guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
	EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = NULL;

	memset(v, 0, sizeof(*v));
	if (EFI_ERROR(gBS->LocateProtocol(&gop_guid, NULL, (void **)&gop)) || !gop || !gop->Mode || !gop->Mode->Info) {
		printf("video: no GOP framebuffer; serial only\n");
		return;
	}
	EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info = gop->Mode->Info;
	if (info->PixelFormat == PixelBltOnly || gop->Mode->FrameBufferBase == 0) {
		printf("video: GOP has no linear framebuffer; serial only\n");
		return;
	}
	v->v_baseAddr = (uint64_t)gop->Mode->FrameBufferBase;
	/* 0 = text console on the framebuffer (verbose boot), 1 = graphics/progress */
	v->v_display = text_mode ? 0 : 1;
	v->v_rowBytes = info->PixelsPerScanLine * 4;
	v->v_width = info->HorizontalResolution;
	v->v_height = info->VerticalResolution;
	v->v_depth = 32;
	printf("video: %lux%lu 32bpp at 0x%lx (row %lu bytes)\n", v->v_width, v->v_height, v->v_baseAddr, v->v_rowBytes);
}

/* ==================================================================== */
/* main                                                                  */
/* ==================================================================== */

EFI_STATUS
efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st)
{
	static EFI_GUID fdt_guid = EFI_DT_TABLE_GUID;
	struct boot_config cfg;
	struct machine m;
	struct memmap mm = { 0 };
	struct kernel_image img;
	void *kernel_file = NULL, *fdt_blob = NULL, *plist = NULL;
	size_t kernel_size = 0, plist_size = 0;
	struct fdt *f;

	gST = st;
	gBS = st->BootServices;
	gImageHandle = image;
	console_init(st);
	gBS->SetWatchdogTimer(0, 0, 0, NULL);

	printf("\nravynOS booter " BOOTER_VERSION " (ravynOS " BOOTER_PROD_VERSION ", Darwin " BOOTER_DARWIN_VERSION ")\n");
	printf("running at EL%lu, MIDR 0x%08lx, MPIDR 0x%lx, CNTFRQ %lu Hz\n",
	    machine_current_el(), machine_midr(), machine_mpidr(), machine_cntfrq());

	/* 1. Boot configuration */
	memset(&cfg, 0, sizeof(cfg));
	strlcpy(cfg.kernel, "\\ravynos\\kernel", sizeof(cfg.kernel));
	strlcpy(cfg.kernel_flags, "-v serial=3 debug=0x8 cpus=1", sizeof(cfg.kernel_flags));
	if (efi_read_file("\\ravynos\\com.ravynos.boot.plist", &plist, &plist_size)) {
		plist_parse_boot_config(plist, plist_size, &cfg);
	} else {
		printf("no com.ravynos.boot.plist; using defaults\n");
	}
	printf("kernel: %s\nflags:  %s\n", cfg.kernel, cfg.kernel_flags);

	/* 2. Kernel */
	if (!efi_read_file(cfg.kernel, &kernel_file, &kernel_size)) {
		if (!efi_read_file("\\ravynos\\kernelcache", &kernel_file, &kernel_size)) {
			panic("cannot read the kernel (%s)", cfg.kernel);
		}
	}
	if (!macho_inspect(kernel_file, kernel_size, &img)) {
		panic("%s is not a loadable arm64 kernel", cfg.kernel);
	}
	printf("kernel: %lu bytes, va 0x%lx-0x%lx, entry 0x%lx\n", (unsigned long)kernel_size, img.vm_base, img.vm_end, img.entry);
	if (img.vm_base < KERNEL_VIRT_BASE) {
		panic("kernel linked below 0x%lx", KERNEL_VIRT_BASE);
	}

	/* 2b. Kernel extensions */
	struct kext_set kexts;
	load_kexts(&kexts);

	/* 3. Hardware description */
	fdt_blob = efi_find_config_table(&fdt_guid);
	if (fdt_blob) {
		printf("fdt: from firmware at %p\n", fdt_blob);
	} else {
		size_t n;
		if (efi_read_file("\\bcm2712-rpi-5-b.dtb", &fdt_blob, &n)) {
			printf("fdt: firmware published none; using \\bcm2712-rpi-5-b.dtb\n");
		}
	}
	f = fdt_blob ? fdt_open(fdt_blob) : NULL;
	if (!f) {
		panic("no device tree: enable Device Tree in the UEFI settings or put bcm2712-rpi-5-b.dtb on the card");
	}
	discover_machine(f, &m);
	printf("machine: %s (%s), %u cpu(s), boot MPIDR 0x%x\n", m.model, m.soc, m.ncpus, m.boot_mpidr);
	printf("uart: 0x%lx+0x%lx  GICv%u gicd: 0x%lx+0x%lx  %s: 0x%lx+0x%lx\n",
	    m.uart_base, m.uart_size, m.gic_version, m.gicd_base, m.gicd_size,
	    m.gic_version == 3 ? "gicr" : "gicc", m.gicc_base, m.gicc_size);
	if (!m.uart_base) {
		printf("warning: no PL011 found; the kernel will have no console\n");
	}
	if (!m.gicd_base || !m.gicc_base) {
		panic("no GICv2/GICv3 found in the device tree");
	}

	/* 4. RAM: the kernel owns [physBase, physBase + memSize) */
	get_memory_map(&mm, true);
	uint64_t phys_base, mem_size, mem_total;
	choose_ram(&mm, &phys_base, &mem_size, &mem_total);
	printf("ram: %lu MB total; largest usable run 0x%lx-0x%lx (%lu MB)\n",
	    mem_total >> 20, phys_base, phys_base + mem_size, mem_size >> 20);

	/*
	 * The kernel must sit at physBase + (vmaddr - virtBase) because it is
	 * loaded unslid. Firmware allocations (and our own file buffers) may
	 * occupy that spot for the lowest physBase, so walk the RAM run in
	 * 2 MiB steps until the whole region, from the kernel's first byte to
	 * the end of the bootstrap page tables, can be claimed from UEFI.
	 */
	size_t dt_len = 0;
	void *dt = build_device_tree(&m, cfg.kernel_flags, &kexts, &dt_len);
	uint64_t kernel_span = img.vm_end - img.vm_base;
	uint64_t region_span = ALIGN_UP(kernel_span, ALIGN_16K) + kexts.span + ALIGN_UP(dt_len, ALIGN_16K)
	    + ALIGN_UP(sizeof(struct boot_args), ALIGN_16K) + BOOTSTRAP_TABLE_SIZE;
	/*
	 * virtBase: xnu maps [virtBase, ...) onto [physBase, ...) and keeps its
	 * "low globals" window at 0xfffffff000000000; the kernel heap is laid out
	 * relative to that window, so virtBase must not coincide with it. Like
	 * iBoot, use the 2 MiB block that holds __TEXT (0xfffffff007000000): the
	 * kernel image then starts 16 KiB into the region and no RAM is wasted.
	 */
	uint64_t virt_base = ALIGN_DOWN(img.vm_base, ALIGN_2MB);
	uint64_t kernel_offset = img.vm_base - virt_base;
	uint64_t run_end = phys_base + mem_size;
	uint64_t kernel_phys = 0;
	for (uint64_t pb = phys_base; pb + kernel_offset + region_span <= run_end; pb += ALIGN_2MB) {
		EFI_PHYSICAL_ADDRESS addr = pb;
		/* Check the map first: a failed AllocatePages makes EDK2 log a warning. */
		if (!range_is_free(&mm, addr, addr + kernel_offset + region_span)) {
			continue;
		}
		EFI_STATUS s = gBS->AllocatePages(AllocateAddress, EfiLoaderData, EFI_SIZE_TO_PAGES(kernel_offset + region_span), &addr);
		if (!EFI_ERROR(s)) {
			phys_base = pb;
			mem_size = run_end - pb;
			kernel_phys = pb + kernel_offset;
			break;
		}
	}
	if (kernel_phys == 0) {
		print_memory_map(&mm);
		panic("no free 0x%lx-byte region for the kernel in RAM 0x%lx-0x%lx", region_span, phys_base, run_end);
	}
	if (phys_base != ALIGN_DOWN(run_end - mem_size, ALIGN_2MB)) {
		printf("ram: physBase moved up to 0x%lx to clear firmware allocations\n", phys_base);
	}
	uint64_t kernel_phys_end = kernel_phys + kernel_span;
	uint64_t kexts_phys = ALIGN_UP(kernel_phys_end, ALIGN_16K);
	uint64_t dt_phys = ALIGN_UP(kexts_phys + kexts.span, ALIGN_16K);
	uint64_t args_phys = ALIGN_UP(dt_phys + dt_len, ALIGN_16K);
	uint64_t top = ALIGN_UP(args_phys + sizeof(struct boot_args), ALIGN_16K);
	uint64_t region_end = top + BOOTSTRAP_TABLE_SIZE;
	printf("ram: kernel owns 0x%lx-0x%lx (%lu MB); loader region 0x%lx-0x%lx\n",
	    phys_base, phys_base + mem_size, mem_size >> 20, kernel_phys, region_end);

	/* 5. Place kernel, device tree and boot_args */
	printf("loading kernel at 0x%lx\n", kernel_phys);
	macho_load(kernel_file, kernel_size, &img, img.vm_base, (uint8_t *)(uintptr_t)kernel_phys);
	memset((void *)(uintptr_t)kernel_phys_end, 0, region_end - kernel_phys_end);
	place_kexts(&kexts, kexts_phys);
	memcpy((void *)(uintptr_t)dt_phys, dt, dt_len);
	{
		uint64_t *entry;
		entry = adt_find_prop((void *)(uintptr_t)dt_phys, dt_len, "memory-map", "DeviceTree");
		if (entry) {
			entry[0] = dt_phys;
			entry[1] = dt_len;
		}
		entry = adt_find_prop((void *)(uintptr_t)dt_phys, dt_len, "memory-map", "BootArgs");
		if (entry) {
			entry[0] = args_phys;
			entry[1] = sizeof(struct boot_args);
		}
		for (unsigned i = 0; i < kexts.count; i++) {
			char name[64];
			kext_entry_name(&kexts.k[i], name, sizeof(name));
			uint32_t *drv = adt_find_prop((void *)(uintptr_t)dt_phys, dt_len, "memory-map", name);
			if (!drv) {
				panic("device tree: lost %s", name);
			}
			drv[0] = (uint32_t)kexts.k[i].phys;
			drv[1] = (uint32_t)kexts.k[i].block_len;
		}
	}
	if (kexts.count) {
		printf("kexts: %u bundle(s) at 0x%lx-0x%lx\n", kexts.count, kexts_phys, kexts_phys + kexts.span);
		for (unsigned i = 0; i < kexts.count; i++) {
			printf("  %-28s plist %6lu B  exec %8lu B  at 0x%lx\n", kexts.k[i].name,
			    (unsigned long)kexts.k[i].plist_len, (unsigned long)kexts.k[i].exec_len, kexts.k[i].phys);
		}
	}

	struct boot_args *args = (struct boot_args *)(uintptr_t)args_phys;
	memset(args, 0, sizeof(*args));
	args->Revision = kBootArgsRevision2;
	args->Version = kBootArgsVersion2;
	args->virtBase = virt_base;
	args->physBase = phys_base;
	args->memSize = mem_size;
	args->memSizeActual = mem_total;
	args->topOfKernelData = top;
	args->machineType = 0;
	args->deviceTreeP = (void *)(uintptr_t)(virt_base + (dt_phys - phys_base));
	args->deviceTreeLength = (uint32_t)dt_len;
	strlcpy(args->CommandLine, cfg.kernel_flags, sizeof(args->CommandLine));
	args->bootFlags = 0;
	fill_video(&args->Video, strstr(cfg.kernel_flags, "-v") != NULL);

	uint64_t entry_phys = phys_base + (img.entry - virt_base);
	printf("device tree: 0x%lx (%lu bytes)  boot_args: 0x%lx  topOfKernelData: 0x%lx\n", dt_phys, (unsigned long)dt_len, args_phys, top);
	printf("virtBase 0x%lx -> physBase 0x%lx (%lu MB)\n", virt_base, phys_base, mem_size >> 20);
	printf("entering kernel at 0x%lx (va 0x%lx) with x0 = 0x%lx\n", entry_phys, img.entry, args_phys);

	/* 6. Leave UEFI */
	get_memory_map(&mm, true);
	EFI_STATUS s = gBS->ExitBootServices(image, mm.key);
	if (EFI_ERROR(s)) {
		/* the map changed under us: refresh (no allocation now) and retry */
		get_memory_map(&mm, false);
		s = gBS->ExitBootServices(image, mm.key);
		if (EFI_ERROR(s)) {
			panic("ExitBootServices: 0x%lx", (unsigned long)s);
		}
	}
	console_init(NULL);
	console_set_pl011(m.uart_base);
	console_puts("booter: boot services exited; cleaning caches and jumping\n");

	machine_clean_dcache((void *)(uintptr_t)kernel_phys, region_end - kernel_phys);
	machine_enter_kernel(entry_phys, args_phys);
}
