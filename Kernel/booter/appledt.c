/*
 * Builder for the flattened device tree format xnu's pexpert consumes
 * (pexpert/pexpert/device_tree.h):
 *
 *   node:     uint32 nProperties, uint32 nChildren, props[], children[]
 *   property: char name[32], uint32 length, value[] padded to 4 bytes
 *
 * Nodes are emitted depth-first, so the builder keeps a stack of open
 * nodes and patches their property/child counts when they close. Every
 * node gets a "name" property, which is how DTFindEntry("name", ...) and
 * DTLookupEntry("/path") find things.
 */

#include "booter.h"

#define ADT_NAME_LEN 32
#define ADT_MAX_DEPTH 16

struct adt {
	uint8_t *buf;
	size_t cap;
	size_t len;
	size_t node_off[ADT_MAX_DEPTH];  /* offsets of open nodes' headers */
	int depth;
};

static struct adt gadt;

static void
put32(uint8_t *p, uint32_t v)
{
	memcpy(p, &v, 4);
}

static uint32_t
get32(const uint8_t *p)
{
	uint32_t v;
	memcpy(&v, p, 4);
	return v;
}

static void *
reserve(struct adt *t, size_t n)
{
	if (t->len + n > t->cap) {
		panic("device tree buffer too small (%lu + %lu > %lu)", (unsigned long)t->len, (unsigned long)n, (unsigned long)t->cap);
	}
	void *p = t->buf + t->len;
	memset(p, 0, n);
	t->len += n;
	return p;
}

struct adt *
adt_create(size_t capacity)
{
	gadt.buf = efi_alloc(capacity);
	gadt.cap = capacity;
	gadt.len = 0;
	gadt.depth = 0;
	return &gadt;
}

int
adt_begin_node(struct adt *t, const char *name)
{
	if (t->depth >= ADT_MAX_DEPTH) {
		panic("device tree nesting too deep");
	}
	if (t->depth > 0) {
		uint8_t *parent = t->buf + t->node_off[t->depth - 1];
		put32(parent + 4, get32(parent + 4) + 1);
	}
	t->node_off[t->depth++] = t->len;
	reserve(t, 8);
	adt_prop_str(t, "name", name);
	return t->depth;
}

void
adt_end_node(struct adt *t)
{
	if (t->depth <= 0) {
		panic("device tree: end_node without begin_node");
	}
	t->depth--;
}

void
adt_prop(struct adt *t, const char *name, const void *value, uint32_t len)
{
	uint8_t *node = t->buf + t->node_off[t->depth - 1];
	uint8_t *p;

	/* Properties must precede children; the builder only allows that order. */
	if (get32(node + 4) != 0) {
		panic("device tree: property '%s' added after children", name);
	}
	put32(node, get32(node) + 1);

	p = reserve(t, ADT_NAME_LEN + 4 + ((len + 3) & ~3u));
	strlcpy((char *)p, name, ADT_NAME_LEN);
	put32(p + ADT_NAME_LEN, len);
	if (len) {
		memcpy(p + ADT_NAME_LEN + 4, value, len);
	}
}

void
adt_prop_str(struct adt *t, const char *name, const char *value)
{
	adt_prop(t, name, value, (uint32_t)strlen(value) + 1);
}

void
adt_prop_u32(struct adt *t, const char *name, uint32_t value)
{
	adt_prop(t, name, &value, 4);
}

void
adt_prop_u64(struct adt *t, const char *name, uint64_t value)
{
	adt_prop(t, name, &value, 8);
}

void *
adt_finish(struct adt *t, size_t *len)
{
	if (t->depth != 0) {
		panic("device tree: %d node(s) still open", t->depth);
	}
	*len = t->len;
	return t->buf;
}

/*
 * Walk a finished tree and return a pointer to the value of `prop` in the
 * first node named `node` (searching every level), or NULL.
 */
void *
adt_find_prop(void *tree, size_t len, const char *node, const char *prop)
{
	uint8_t *p = tree;
	uint8_t *end = p + len;
	/* iterative walk with an explicit stack of remaining-children counts */
	uint32_t remaining[ADT_MAX_DEPTH];
	int depth = 0;

	remaining[0] = 1;
	while (p + 8 <= end && depth >= 0) {
		if (remaining[depth] == 0) {
			depth--;
			continue;
		}
		remaining[depth]--;
		uint32_t nprops = get32(p);
		uint32_t nchildren = get32(p + 4);
		p += 8;
		bool match = false;
		void *value = NULL;
		for (uint32_t i = 0; i < nprops && p + ADT_NAME_LEN + 4 <= end; i++) {
			const char *pname = (const char *)p;
			uint32_t plen = get32(p + ADT_NAME_LEN);
			uint8_t *pval = p + ADT_NAME_LEN + 4;
			if (strcmp(pname, "name") == 0 && strcmp((const char *)pval, node) == 0) {
				match = true;
			}
			if (strcmp(pname, prop) == 0) {
				value = pval;
			}
			p = pval + ((plen + 3) & ~3u);
		}
		if (match && value) {
			return value;
		}
		if (nchildren) {
			if (depth + 1 >= ADT_MAX_DEPTH) {
				return NULL;
			}
			remaining[++depth] = nchildren;
		}
	}
	return NULL;
}
