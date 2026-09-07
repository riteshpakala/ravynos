/*
 * Flattened device tree (Linux FDT / DTB) reader for the ravynOS booter.
 *
 * The firmware (rpi5-uefi, QEMU's ArmVirt EDK2) publishes the FDT through
 * the EFI configuration table. Only reading is supported: node lookup by
 * path, compatible string or name prefix, property access, and "reg"
 * decoding with bus-address translation through parent "ranges".
 */

#include "booter.h"

#define FDT_MAGIC       0xd00dfeed
#define FDT_BEGIN_NODE  1
#define FDT_END_NODE    2
#define FDT_PROP        3
#define FDT_NOP         4
#define FDT_END         9

struct fdt_header {
	uint32_t magic;
	uint32_t totalsize;
	uint32_t off_dt_struct;
	uint32_t off_dt_strings;
	uint32_t off_mem_rsvmap;
	uint32_t version;
	uint32_t last_comp_version;
	uint32_t boot_cpuid_phys;
	uint32_t size_dt_strings;
	uint32_t size_dt_struct;
};

struct fdt {
	const uint8_t *blob;
	const uint8_t *structs;
	const char *strings;
	uint32_t struct_size;
};

static struct fdt gfdt;

uint32_t
fdt32(const void *p)
{
	const uint8_t *b = p;
	return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
}

static uint64_t __attribute__((unused))
fdt64(const void *p)
{
	const uint8_t *b = p;
	return ((uint64_t)fdt32(b) << 32) | fdt32(b + 4);
}

static uint64_t
fdt_cells(const uint8_t *p, uint32_t cells)
{
	uint64_t v = 0;
	while (cells--) {
		v = (v << 32) | fdt32(p);
		p += 4;
	}
	return v;
}

struct fdt *
fdt_open(void *blob)
{
	const struct fdt_header *h = blob;
	if (!blob || fdt32(&h->magic) != FDT_MAGIC) {
		return NULL;
	}
	gfdt.blob = blob;
	gfdt.structs = gfdt.blob + fdt32(&h->off_dt_struct);
	gfdt.strings = (const char *)gfdt.blob + fdt32(&h->off_dt_strings);
	gfdt.struct_size = fdt32(&h->size_dt_struct);
	return &gfdt;
}

static inline uint32_t
tok(struct fdt *f, int off)
{
	return fdt32(f->structs + off);
}

static inline int
align4(int off)
{
	return (off + 3) & ~3;
}

/* Offset of the token after the node header (name) at `node`. */
static int
node_body(struct fdt *f, int node)
{
	const char *name = (const char *)f->structs + node + 4;
	return align4(node + 4 + (int)strlen(name) + 1);
}

/* Skip one property token at `off`. */
static int
skip_prop(struct fdt *f, int off)
{
	uint32_t len = tok(f, off + 4);
	return align4(off + 12 + (int)len);
}

/* Offset just past the node (its FDT_END_NODE). */
static int
node_end(struct fdt *f, int node)
{
	int off = node_body(f, node);
	int depth = 1;
	while (off < (int)f->struct_size) {
		uint32_t t = tok(f, off);
		switch (t) {
		case FDT_BEGIN_NODE:
			depth++;
			off = node_body(f, off);
			break;
		case FDT_END_NODE:
			depth--;
			off += 4;
			if (depth == 0) {
				return off;
			}
			break;
		case FDT_PROP:
			off = skip_prop(f, off);
			break;
		case FDT_NOP:
			off += 4;
			break;
		default:
			return -1;
		}
	}
	return -1;
}

const char *
fdt_node_name(struct fdt *f, int node)
{
	return (const char *)f->structs + node + 4;
}

int
fdt_first_subnode(struct fdt *f, int node)
{
	int off = node_body(f, node);
	while (off < (int)f->struct_size) {
		uint32_t t = tok(f, off);
		if (t == FDT_BEGIN_NODE) {
			return off;
		}
		if (t == FDT_PROP) {
			off = skip_prop(f, off);
		} else if (t == FDT_NOP) {
			off += 4;
		} else {
			return -1;
		}
	}
	return -1;
}

int
fdt_next_subnode(struct fdt *f, int node)
{
	int off = node_end(f, node);
	while (off >= 0 && off < (int)f->struct_size) {
		uint32_t t = tok(f, off);
		if (t == FDT_BEGIN_NODE) {
			return off;
		}
		if (t == FDT_NOP) {
			off += 4;
		} else {
			return -1;
		}
	}
	return -1;
}

/* Depth-first successor of `node` in the whole tree (any depth), or -1. */
static int
next_node(struct fdt *f, int node)
{
	int off = node_body(f, node);
	while (off >= 0 && off < (int)f->struct_size) {
		uint32_t t = tok(f, off);
		switch (t) {
		case FDT_BEGIN_NODE:
			return off;
		case FDT_END_NODE:
		case FDT_NOP:
			off += 4;
			break;
		case FDT_PROP:
			off = skip_prop(f, off);
			break;
		default:
			return -1;
		}
	}
	return -1;
}

const void *
fdt_getprop(struct fdt *f, int node, const char *name, uint32_t *len)
{
	int off = node_body(f, node);
	while (off < (int)f->struct_size) {
		uint32_t t = tok(f, off);
		if (t == FDT_PROP) {
			uint32_t plen = tok(f, off + 4);
			const char *pname = f->strings + tok(f, off + 8);
			if (strcmp(pname, name) == 0) {
				if (len) {
					*len = plen;
				}
				return f->structs + off + 12;
			}
			off = skip_prop(f, off);
		} else if (t == FDT_NOP) {
			off += 4;
		} else {
			break;
		}
	}
	if (len) {
		*len = 0;
	}
	return NULL;
}

uint32_t
fdt_get_u32(struct fdt *f, int node, const char *name, uint32_t dflt)
{
	uint32_t len;
	const void *p = fdt_getprop(f, node, name, &len);
	return (p && len >= 4) ? fdt32(p) : dflt;
}

int
fdt_parent(struct fdt *f, int node)
{
	int off = 0;
	int parent = -1;
	int stack[32];
	int depth = 0;

	if (node == 0) {
		return -1;
	}
	while (off < (int)f->struct_size) {
		uint32_t t = tok(f, off);
		switch (t) {
		case FDT_BEGIN_NODE:
			if (off == node) {
				return parent;
			}
			if (depth < 32) {
				stack[depth] = parent;
			}
			depth++;
			parent = off;
			off = node_body(f, off);
			break;
		case FDT_END_NODE:
			depth--;
			parent = depth < 32 && depth >= 0 ? stack[depth] : -1;
			off += 4;
			break;
		case FDT_PROP:
			off = skip_prop(f, off);
			break;
		case FDT_NOP:
			off += 4;
			break;
		default:
			return -1;
		}
	}
	return -1;
}

int
fdt_node_by_path(struct fdt *f, const char *path)
{
	int node = 0;
	if (tok(f, 0) != FDT_BEGIN_NODE) {
		return -1;
	}
	while (*path == '/') {
		path++;
	}
	while (*path) {
		const char *slash = strchr(path, '/');
		size_t n = slash ? (size_t)(slash - path) : strlen(path);
		int child = fdt_first_subnode(f, node);
		int found = -1;
		while (child >= 0) {
			const char *name = fdt_node_name(f, child);
			size_t base = strlen(name);
			const char *at = strchr(name, '@');
			if (at) {
				base = (size_t)(at - name);
			}
			if ((strlen(name) == n && strncmp(name, path, n) == 0) ||
			    (base == n && strncmp(name, path, n) == 0 && !strchr(path, '@'))) {
				found = child;
				break;
			}
			child = fdt_next_subnode(f, child);
		}
		if (found < 0) {
			return -1;
		}
		node = found;
		path += n;
		while (*path == '/') {
			path++;
		}
	}
	return node;
}

static bool
compatible_matches(const char *list, uint32_t len, const char *want)
{
	const char *end = list + len;
	while (list < end) {
		if (strcmp(list, want) == 0) {
			return true;
		}
		list += strlen(list) + 1;
	}
	return false;
}

int
fdt_node_by_compatible(struct fdt *f, const char *compatible, int start)
{
	int node = start < 0 ? 0 : next_node(f, start);
	while (node >= 0) {
		uint32_t len;
		const char *list = fdt_getprop(f, node, "compatible", &len);
		if (list && compatible_matches(list, len, compatible)) {
			return node;
		}
		node = next_node(f, node);
	}
	return -1;
}

int
fdt_node_by_name_prefix(struct fdt *f, const char *prefix, int start)
{
	size_t n = strlen(prefix);
	int node = start < 0 ? 0 : next_node(f, start);
	while (node >= 0) {
		if (strncmp(fdt_node_name(f, node), prefix, n) == 0) {
			return node;
		}
		node = next_node(f, node);
	}
	return -1;
}

static uint32_t
cells_of(struct fdt *f, int node, const char *name, uint32_t dflt)
{
	if (node < 0) {
		return dflt;
	}
	return fdt_get_u32(f, node, name, dflt);
}

/* Translate a child bus address of `node` up through its parents' "ranges". */
uint64_t
fdt_translate(struct fdt *f, int node, uint64_t addr)
{
	int parent = fdt_parent(f, node);
	while (parent >= 0) {
		int grand = fdt_parent(f, parent);
		uint32_t child_ac = cells_of(f, parent, "#address-cells", 2);
		uint32_t child_sc = cells_of(f, parent, "#size-cells", 1);
		uint32_t parent_ac = cells_of(f, grand, "#address-cells", 2);
		uint32_t len;
		const uint8_t *ranges = fdt_getprop(f, parent, "ranges", &len);
		if (ranges && len > 0) {
			uint32_t entry = 4 * (child_ac + parent_ac + child_sc);
			bool translated = false;
			for (uint32_t off = 0; off + entry <= len; off += entry) {
				uint64_t child = fdt_cells(ranges + off, child_ac);
				uint64_t pbase = fdt_cells(ranges + off + 4 * child_ac, parent_ac);
				uint64_t size = fdt_cells(ranges + off + 4 * (child_ac + parent_ac), child_sc);
				if (addr >= child && addr < child + size) {
					addr = addr - child + pbase;
					translated = true;
					break;
				}
			}
			if (!translated) {
				/* No matching range: leave the address alone (identity). */
			}
		}
		node = parent;
		parent = grand;
	}
	return addr;
}

int
fdt_reg(struct fdt *f, int node, int index, uint64_t *addr, uint64_t *size)
{
	int parent = fdt_parent(f, node);
	uint32_t ac = cells_of(f, parent, "#address-cells", 2);
	uint32_t sc = cells_of(f, parent, "#size-cells", 1);
	uint32_t len;
	const uint8_t *reg = fdt_getprop(f, node, "reg", &len);
	uint32_t entry = 4 * (ac + sc);
	uint32_t off = (uint32_t)index * entry;

	if (!reg || off + entry > len) {
		return 0;
	}
	if (addr) {
		*addr = fdt_translate(f, node, fdt_cells(reg + off, ac));
	}
	if (size) {
		*size = fdt_cells(reg + off + 4 * ac, sc);
	}
	return 1;
}
