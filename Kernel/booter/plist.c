/*
 * Just enough XML plist parsing for com.ravynos.boot.plist:
 *   <key>Kernel</key><string>\ravynos\kernel</string>
 *   <key>Kernel Flags</key><string>-v serial=3</string>
 */

#include "booter.h"

static const char *
skip_ws(const char *p, const char *end)
{
	while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) {
		p++;
	}
	return p;
}

static const char *
find(const char *p, const char *end, const char *needle)
{
	size_t n = strlen(needle);
	for (; p + n <= end; p++) {
		if (memcmp(p, needle, n) == 0) {
			return p;
		}
	}
	return NULL;
}

static void
decode_entities(char *s)
{
	static const struct { const char *from; char to; } table[] = {
		{ "&amp;", '&' }, { "&lt;", '<' }, { "&gt;", '>' }, { "&quot;", '"' }, { "&apos;", '\'' },
	};
	char *w = s;
	while (*s) {
		bool matched = false;
		for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
			size_t n = strlen(table[i].from);
			if (strncmp(s, table[i].from, n) == 0) {
				*w++ = table[i].to;
				s += n;
				matched = true;
				break;
			}
		}
		if (!matched) {
			*w++ = *s++;
		}
	}
	*w = 0;
}

void
plist_parse_boot_config(const char *xml, size_t len, struct boot_config *out)
{
	const char *p = xml;
	const char *end = xml + len;

	while ((p = find(p, end, "<key>")) != NULL) {
		p += 5;
		const char *kend = find(p, end, "</key>");
		if (!kend) {
			break;
		}
		char key[64];
		size_t klen = (size_t)(kend - p);
		if (klen >= sizeof(key)) {
			klen = sizeof(key) - 1;
		}
		memcpy(key, p, klen);
		key[klen] = 0;
		p = skip_ws(kend + 6, end);

		if (p + 8 <= end && memcmp(p, "<string>", 8) == 0) {
			p += 8;
			const char *vend = find(p, end, "</string>");
			if (!vend) {
				break;
			}
			char *dst = NULL;
			size_t cap = 0;
			if (strcmp(key, "Kernel") == 0) {
				dst = out->kernel;
				cap = sizeof(out->kernel);
			} else if (strcmp(key, "Kernel Flags") == 0) {
				dst = out->kernel_flags;
				cap = sizeof(out->kernel_flags);
			}
			if (dst) {
				size_t vlen = (size_t)(vend - p);
				if (vlen >= cap) {
					vlen = cap - 1;
				}
				memcpy(dst, p, vlen);
				dst[vlen] = 0;
				decode_entities(dst);
			}
			p = vend + 9;
		} else if (p + 9 <= end && memcmp(p, "<string/>", 9) == 0) {
			p += 9;
		}
	}
}
