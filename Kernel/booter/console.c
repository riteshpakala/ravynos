/*
 * Console output for the ravynOS AArch64 booter: UEFI text output while
 * boot services are up, a raw PL011 afterwards, and a small printf.
 */

#include "booter.h"

static EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *con_out;
static volatile uint32_t *pl011;

void
console_init(EFI_SYSTEM_TABLE *st)
{
	con_out = st ? st->ConOut : NULL;
}

void
console_set_pl011(uint64_t base)
{
	pl011 = base ? (volatile uint32_t *)(uintptr_t)base : NULL;
}

static void
pl011_putc(char c)
{
	/* FR is at 0x18, TXFF is bit 5; DR is at 0 */
	while (pl011[0x18 / 4] & (1u << 5)) {
	}
	pl011[0] = (uint32_t)(unsigned char)c;
}

void
console_putc(char c)
{
	if (c == '\n') {
		console_putc('\r');
	}
	if (pl011) {
		pl011_putc(c);
	}
	if (con_out) {
		CHAR16 s[2] = { (CHAR16)(unsigned char)c, 0 };
		con_out->OutputString(con_out, s);
	}
}

void
console_puts(const char *s)
{
	while (*s) {
		console_putc(*s++);
	}
}

/* ---- printf: %s %c %d %u %x %lx %llx %p %zu with width and zero padding ---- */

struct out {
	char *buf;
	size_t size;
	size_t pos;
};

static void
out_char(struct out *o, char c)
{
	if (o->buf) {
		if (o->pos + 1 < o->size) {
			o->buf[o->pos] = c;
		}
	} else {
		console_putc(c);
	}
	o->pos++;
}

static void
out_num(struct out *o, unsigned long long v, unsigned base, bool neg, int width, bool zero, bool upper)
{
	char tmp[32];
	int n = 0;
	const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";

	do {
		tmp[n++] = digits[v % base];
		v /= base;
	} while (v);
	if (neg) {
		tmp[n++] = '-';
	}
	while (n < width) {
		tmp[n++] = zero ? '0' : ' ';
	}
	while (n--) {
		out_char(o, tmp[n]);
	}
}

static int
do_vprintf(struct out *o, const char *fmt, va_list ap)
{
	for (; *fmt; fmt++) {
		if (*fmt != '%') {
			out_char(o, *fmt);
			continue;
		}
		fmt++;
		bool zero = false;
		bool left = false;
		int width = 0;
		int longs = 0;
		if (*fmt == '-') {
			left = true;
			fmt++;
		}
		if (*fmt == '0') {
			zero = true;
			fmt++;
		}
		while (*fmt >= '0' && *fmt <= '9') {
			width = width * 10 + (*fmt - '0');
			fmt++;
		}
		while (*fmt == 'l' || *fmt == 'z') {
			longs++;
			fmt++;
		}
		switch (*fmt) {
		case 's': {
			const char *s = va_arg(ap, const char *);
			if (!s) {
				s = "(null)";
			}
			int n = (int)strlen(s);
			if (!left) {
				for (int i = n; i < width; i++) {
					out_char(o, ' ');
				}
			}
			while (*s) {
				out_char(o, *s++);
			}
			if (left) {
				for (int i = n; i < width; i++) {
					out_char(o, ' ');
				}
			}
			break;
		}
		case 'c':
			out_char(o, (char)va_arg(ap, int));
			break;
		case 'd': {
			long long v = longs ? va_arg(ap, long long) : va_arg(ap, int);
			bool neg = v < 0;
			out_num(o, neg ? (unsigned long long)(-v) : (unsigned long long)v, 10, neg, width, zero, false);
			break;
		}
		case 'u': {
			unsigned long long v = longs ? va_arg(ap, unsigned long long) : va_arg(ap, unsigned);
			out_num(o, v, 10, false, width, zero, false);
			break;
		}
		case 'x':
		case 'X': {
			unsigned long long v = longs ? va_arg(ap, unsigned long long) : va_arg(ap, unsigned);
			out_num(o, v, 16, false, width, zero, *fmt == 'X');
			break;
		}
		case 'p':
			out_char(o, '0');
			out_char(o, 'x');
			out_num(o, (unsigned long long)(uintptr_t)va_arg(ap, void *), 16, false, 0, false, false);
			break;
		case '%':
			out_char(o, '%');
			break;
		default:
			out_char(o, '%');
			out_char(o, *fmt);
			break;
		}
	}
	if (o->buf && o->size) {
		o->buf[o->pos < o->size ? o->pos : o->size - 1] = 0;
	}
	return (int)o->pos;
}

int
vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
	struct out o = { buf, size, 0 };
	return do_vprintf(&o, fmt, ap);
}

int
snprintf(char *buf, size_t size, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(buf, size, fmt, ap);
	va_end(ap);
	return n;
}

void
printf(const char *fmt, ...)
{
	struct out o = { NULL, 0, 0 };
	va_list ap;
	va_start(ap, fmt);
	do_vprintf(&o, fmt, ap);
	va_end(ap);
}

void
panic(const char *fmt, ...)
{
	struct out o = { NULL, 0, 0 };
	va_list ap;
	console_puts("\nbooter panic: ");
	va_start(ap, fmt);
	do_vprintf(&o, fmt, ap);
	va_end(ap);
	console_puts("\nhalted.\n");
	for (;;) {
		__asm__ volatile ("wfe");
	}
}
