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

/* ---- machine description gathered from the firmware ---- */
struct machine {
	const char *model;
	const char *soc;            /* "bcm2712" or "qemu-virt" */
	uint64_t uart_base, uart_size;
	uint64_t gicd_base, gicd_size;
	uint64_t gicc_base, gicc_size;
	uint64_t cntfrq;
	uint32_t boot_mpidr;
	unsigned ncpus;
	uint32_t cpu_mpidr[8];
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

/* Read a whole file from the boot volume. Returns 0 with a message when absent. */
int
efi_read_file(const char *path, void **data, size_t *size)
{
	static EFI_GUID info_guid = EFI_FILE_INFO_GUID;
	static EFI_FILE_PROTOCOL *root;
	EFI_FILE_PROTOCOL *file = NULL;
	CHAR16 wpath[260];
	uint8_t info_buf[sizeof(EFI_FILE_INFO) + 2 * 260];
	UINTN info_size = sizeof(info_buf);
	EFI_STATUS st;
	size_t i;

	if (!root) {
		root = open_boot_volume();
	}
	for (i = 0; path[i] && i < 259; i++) {
		wpath[i] = (CHAR16)(path[i] == '/' ? '\\' : path[i]);
	}
	wpath[i] = 0;

	st = root->Open(root, &file, wpath, EFI_FILE_MODE_READ, 0);
	if (EFI_ERROR(st) || !file) {
		printf("open %s: 0x%lx\n", path, (unsigned long)st);
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

/* ==================================================================== */
/* Hardware discovery from the firmware's FDT                            */
/* ==================================================================== */

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
	}

	/* GICv2: distributor then CPU interface */
	node = fdt_node_by_compatible(f, "arm,gic-400", -1);
	if (node < 0) {
		node = fdt_node_by_compatible(f, "arm,cortex-a15-gic", -1);
	}
	if (node < 0) {
		node = fdt_node_by_compatible(f, "arm,cortex-a7-gic", -1);
	}
	if (node >= 0) {
		fdt_reg(f, node, 0, &m->gicd_base, &m->gicd_size);
		fdt_reg(f, node, 1, &m->gicc_base, &m->gicc_size);
		if (m->gicd_size < 0x1000) {
			m->gicd_size = 0x1000;
		}
		if (m->gicc_size < 0x2000) {
			m->gicc_size = 0x2000;
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
	UINTN key;
	UINTN desc_size;
	UINT32 desc_version;
};

static void
get_memory_map(struct memmap *mm, bool allocate)
{
	EFI_STATUS st;

	if (allocate) {
		mm->size = 0;
		mm->map = NULL;
		st = gBS->GetMemoryMap(&mm->size, NULL, &mm->key, &mm->desc_size, &mm->desc_version);
		if (st != EFI_BUFFER_TOO_SMALL) {
			panic("GetMemoryMap size query: 0x%lx", (unsigned long)st);
		}
		/* room for the descriptors our own allocations add */
		mm->size += 8 * mm->desc_size;
		mm->map = efi_alloc(mm->size);
	}
	st = gBS->GetMemoryMap(&mm->size, mm->map, &mm->key, &mm->desc_size, &mm->desc_version);
	if (EFI_ERROR(st)) {
		panic("GetMemoryMap: 0x%lx", (unsigned long)st);
	}
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
/* Apple device tree                                                     */
/* ==================================================================== */

static void *
build_device_tree(const struct machine *m, const char *cmdline, size_t *len_out)
{
	struct adt *t = adt_create(64 * 1024);
	uint64_t soc_base, soc_end, ranges[3], reg[4];
	uint32_t zero = 0, one = 1;
	char buf[128];

	/* /arm-io ranges: every peripheral is an offset from soc_base */
	soc_base = m->uart_base;
	if (m->gicd_base && m->gicd_base < soc_base) {
		soc_base = m->gicd_base;
	}
	if (m->gicc_base && m->gicc_base < soc_base) {
		soc_base = m->gicc_base;
	}
	soc_base = ALIGN_DOWN(soc_base, 0x100000);
	soc_end = m->uart_base + m->uart_size;
	if (m->gicd_base + m->gicd_size > soc_end) {
		soc_end = m->gicd_base + m->gicd_size;
	}
	if (m->gicc_base + m->gicc_size > soc_end) {
		soc_end = m->gicc_base + m->gicc_size;
	}

	adt_begin_node(t, "device-tree");
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
		adt_end_node(t);
	}
	if (m->gicd_base) {
		adt_begin_node(t, "interrupt-controller");
		adt_prop_str(t, "compatible", "arm,gic-400");
		adt_prop_str(t, "interrupt-controller", "master");
		reg[0] = m->gicd_base - soc_base;
		reg[1] = m->gicd_size;
		reg[2] = m->gicc_base - soc_base;
		reg[3] = m->gicc_size;
		adt_prop(t, "reg", reg, 32);
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
fill_video(struct Boot_Video *v)
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
	v->v_display = 1;
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
	struct memmap mm;
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
	printf("uart: 0x%lx+0x%lx  gicd: 0x%lx+0x%lx  gicc: 0x%lx+0x%lx\n",
	    m.uart_base, m.uart_size, m.gicd_base, m.gicd_size, m.gicc_base, m.gicc_size);
	if (!m.uart_base) {
		printf("warning: no PL011 found; the kernel will have no console\n");
	}
	if (!m.gicd_base || !m.gicc_base) {
		panic("no GICv2 found in the device tree");
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
	void *dt = build_device_tree(&m, cfg.kernel_flags, &dt_len);
	uint64_t kernel_span = img.vm_end - img.vm_base;
	uint64_t region_span = ALIGN_UP(kernel_span, ALIGN_16K) + ALIGN_UP(dt_len, ALIGN_16K)
	    + ALIGN_UP(sizeof(struct boot_args), ALIGN_16K) + BOOTSTRAP_TABLE_SIZE;
	uint64_t kernel_offset = img.vm_base - KERNEL_VIRT_BASE;
	uint64_t run_end = phys_base + mem_size;
	uint64_t kernel_phys = 0;
	for (uint64_t pb = phys_base; pb + kernel_offset + region_span <= run_end; pb += ALIGN_2MB) {
		EFI_PHYSICAL_ADDRESS addr = pb + kernel_offset;
		/* Check the map first: a failed AllocatePages makes EDK2 log a warning. */
		if (!range_is_free(&mm, addr, addr + region_span)) {
			continue;
		}
		EFI_STATUS s = gBS->AllocatePages(AllocateAddress, EfiLoaderData, EFI_SIZE_TO_PAGES(region_span), &addr);
		if (!EFI_ERROR(s)) {
			phys_base = pb;
			mem_size = run_end - pb;
			kernel_phys = addr;
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
	uint64_t dt_phys = ALIGN_UP(kernel_phys_end, ALIGN_16K);
	uint64_t args_phys = ALIGN_UP(dt_phys + dt_len, ALIGN_16K);
	uint64_t top = ALIGN_UP(args_phys + sizeof(struct boot_args), ALIGN_16K);
	uint64_t region_end = top + BOOTSTRAP_TABLE_SIZE;
	printf("ram: kernel owns 0x%lx-0x%lx (%lu MB); loader region 0x%lx-0x%lx\n",
	    phys_base, phys_base + mem_size, mem_size >> 20, kernel_phys, region_end);

	/* 5. Place kernel, device tree and boot_args */
	printf("loading kernel at 0x%lx\n", kernel_phys);
	macho_load(kernel_file, kernel_size, &img, img.vm_base, (uint8_t *)(uintptr_t)kernel_phys);
	memset((void *)(uintptr_t)kernel_phys_end, 0, region_end - kernel_phys_end);
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
	}

	struct boot_args *args = (struct boot_args *)(uintptr_t)args_phys;
	memset(args, 0, sizeof(*args));
	args->Revision = kBootArgsRevision2;
	args->Version = kBootArgsVersion2;
	args->virtBase = KERNEL_VIRT_BASE;
	args->physBase = phys_base;
	args->memSize = mem_size;
	args->memSizeActual = mem_total;
	args->topOfKernelData = top;
	args->machineType = 0;
	args->deviceTreeP = (void *)(uintptr_t)(KERNEL_VIRT_BASE + (dt_phys - phys_base));
	args->deviceTreeLength = (uint32_t)dt_len;
	strlcpy(args->CommandLine, cfg.kernel_flags, sizeof(args->CommandLine));
	args->bootFlags = 0;
	fill_video(&args->Video);

	uint64_t entry_phys = phys_base + (img.entry - KERNEL_VIRT_BASE);
	printf("device tree: 0x%lx (%lu bytes)  boot_args: 0x%lx  topOfKernelData: 0x%lx\n", dt_phys, (unsigned long)dt_len, args_phys, top);
	printf("entering kernel at 0x%lx (va 0x%lx) with x0 = 0x%lx\n", entry_phys, img.entry, args_phys);

	/* 6. Leave UEFI */
	get_memory_map(&mm, false);
	EFI_STATUS s = gBS->ExitBootServices(image, mm.key);
	if (EFI_ERROR(s)) {
		/* the map changed under us: refresh once and retry */
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
