/*
 * Freestanding string routines for the ravynOS AArch64 booter.
 */

#include "booter.h"

void *
memcpy(void *dst, const void *src, size_t n)
{
	uint8_t *d = dst;
	const uint8_t *s = src;
	while (n--) {
		*d++ = *s++;
	}
	return dst;
}

void *
memmove(void *dst, const void *src, size_t n)
{
	uint8_t *d = dst;
	const uint8_t *s = src;
	if (d < s) {
		while (n--) {
			*d++ = *s++;
		}
	} else {
		d += n;
		s += n;
		while (n--) {
			*--d = *--s;
		}
	}
	return dst;
}

void *
memset(void *dst, int c, size_t n)
{
	uint8_t *d = dst;
	while (n--) {
		*d++ = (uint8_t)c;
	}
	return dst;
}

int
memcmp(const void *a, const void *b, size_t n)
{
	const uint8_t *x = a, *y = b;
	while (n--) {
		if (*x != *y) {
			return *x - *y;
		}
		x++;
		y++;
	}
	return 0;
}

size_t
strlen(const char *s)
{
	size_t n = 0;
	while (s[n]) {
		n++;
	}
	return n;
}

int
strcmp(const char *a, const char *b)
{
	while (*a && *a == *b) {
		a++;
		b++;
	}
	return (unsigned char)*a - (unsigned char)*b;
}

int
strncmp(const char *a, const char *b, size_t n)
{
	while (n && *a && *a == *b) {
		a++;
		b++;
		n--;
	}
	return n ? (unsigned char)*a - (unsigned char)*b : 0;
}

char *
strcpy(char *dst, const char *src)
{
	char *d = dst;
	while ((*d++ = *src++)) {
	}
	return dst;
}

size_t
strlcpy(char *dst, const char *src, size_t size)
{
	size_t n = strlen(src);
	if (size) {
		size_t copy = n < size - 1 ? n : size - 1;
		memcpy(dst, src, copy);
		dst[copy] = 0;
	}
	return n;
}

size_t
strlcat(char *dst, const char *src, size_t size)
{
	size_t have = strlen(dst);
	if (have >= size) {
		return have + strlen(src);
	}
	return have + strlcpy(dst + have, src, size - have);
}

char *
strchr(const char *s, int c)
{
	for (; *s; s++) {
		if (*s == (char)c) {
			return (char *)s;
		}
	}
	return c == 0 ? (char *)s : NULL;
}

char *
strstr(const char *haystack, const char *needle)
{
	size_t n = strlen(needle);
	if (n == 0) {
		return (char *)haystack;
	}
	for (; *haystack; haystack++) {
		if (strncmp(haystack, needle, n) == 0) {
			return (char *)haystack;
		}
	}
	return NULL;
}

unsigned long long
strtoull(const char *s, char **end, int base)
{
	unsigned long long v = 0;
	while (*s == ' ' || *s == '\t') {
		s++;
	}
	if ((base == 0 || base == 16) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
		base = 16;
		s += 2;
	} else if (base == 0) {
		base = 10;
	}
	for (;;) {
		int d;
		if (*s >= '0' && *s <= '9') {
			d = *s - '0';
		} else if (*s >= 'a' && *s <= 'f') {
			d = *s - 'a' + 10;
		} else if (*s >= 'A' && *s <= 'F') {
			d = *s - 'A' + 10;
		} else {
			break;
		}
		if (d >= base) {
			break;
		}
		v = v * (unsigned)base + (unsigned)d;
		s++;
	}
	if (end) {
		*end = (char *)s;
	}
	return v;
}
