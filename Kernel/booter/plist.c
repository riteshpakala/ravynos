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

int
plist_get_string(const char *xml, size_t len, const char *key, char *out, size_t cap)
{
	const char *p = xml;
	const char *end = xml + len;
	size_t keylen = strlen(key);

	while ((p = find(p, end, "<key>")) != NULL) {
		p += 5;
		const char *kend = find(p, end, "</key>");
		if (!kend) {
			break;
		}
		bool match = (size_t)(kend - p) == keylen && memcmp(p, key, keylen) == 0;
		p = skip_ws(kend + 6, end);

		if (p + 8 <= end && memcmp(p, "<string>", 8) == 0) {
			p += 8;
			const char *vend = find(p, end, "</string>");
			if (!vend) {
				break;
			}
			if (match) {
				size_t vlen = (size_t)(vend - p);
				if (vlen >= cap) {
					vlen = cap - 1;
				}
				memcpy(out, p, vlen);
				out[vlen] = 0;
				decode_entities(out);
				return 1;
			}
			p = vend + 9;
		} else if (match) {
			/* present but not a string (or empty) */
			return 0;
		}
	}
	return 0;
}

void
plist_parse_boot_config(const char *xml, size_t len, struct boot_config *out)
{
	char value[sizeof(out->kernel_flags)];

	if (plist_get_string(xml, len, "Kernel", value, sizeof(out->kernel))) {
		strlcpy(out->kernel, value, sizeof(out->kernel));
	}
	if (plist_get_string(xml, len, "Kernel Flags", value, sizeof(out->kernel_flags))) {
		strlcpy(out->kernel_flags, value, sizeof(out->kernel_flags));
	}
}
