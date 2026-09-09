/*
 * ravyninit: a freestanding process 1 for the ravynOS arm64 bring-up.
 *
 * The kernel execs /sbin/launchd once the root filesystem is mounted. Until
 * the real launchd and libSystem exist for arm64, MaryPi installs this
 * program there instead. It talks to the kernel with raw system calls only
 * (no dyld, no libSystem), prints a banner on /dev/console and then offers a
 * tiny shell on the console so the boot can be poked at from the serial line.
 *
 * Copyright (C) 2026 ravynOS Project. MIT licensed.
 */

typedef unsigned long size_t;
typedef long ssize_t;
typedef unsigned long long uint64_t;
typedef unsigned int uint32_t;
typedef unsigned short uint16_t;
typedef unsigned char uint8_t;

/* Darwin arm64 BSD system calls: number in x16, args in x0..x7, svc #0x80.
 * On error the carry flag is set and x0 holds errno. */
#define SYS_exit            1
#define SYS_fork            2
#define SYS_read            3
#define SYS_write           4
#define SYS_open            5
#define SYS_close           6
#define SYS_wait4           7
#define SYS_getpid          20
#define SYS_reboot          55
#define SYS_execve          59
#define SYS_sync            36
#define SYS_mkdir           136
#define SYS_getdirentries64 344
#define SYS_sysctl          202
#define SYS_select          93

#define O_RDONLY  0
#define O_RDWR    2
#define O_CREAT   0x200
#define O_TRUNC   0x400

static long errno_last;

static long
sys(long n, long a0, long a1, long a2, long a3, long a4, long a5)
{
	register long x16 __asm__("x16") = n;
	register long x0 __asm__("x0") = a0;
	register long x1 __asm__("x1") = a1;
	register long x2 __asm__("x2") = a2;
	register long x3 __asm__("x3") = a3;
	register long x4 __asm__("x4") = a4;
	register long x5 __asm__("x5") = a5;
	long carry;

	__asm__ volatile (
		"svc #0x80\n"
		"cset %1, cs\n"
		: "+r"(x0), "=r"(carry)
		: "r"(x16), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5)
		: "memory", "cc");
	if (carry) {
		errno_last = x0;
		return -1;
	}
	return x0;
}

#define SYS0(n)                 sys(n, 0, 0, 0, 0, 0, 0)
#define SYS1(n, a)              sys(n, (long)(a), 0, 0, 0, 0, 0)
#define SYS2(n, a, b)           sys(n, (long)(a), (long)(b), 0, 0, 0, 0)
#define SYS3(n, a, b, c)        sys(n, (long)(a), (long)(b), (long)(c), 0, 0, 0)
#define SYS4(n, a, b, c, d)     sys(n, (long)(a), (long)(b), (long)(c), (long)(d), 0, 0)
#define SYS5(n, a, b, c, d, e)  sys(n, (long)(a), (long)(b), (long)(c), (long)(d), (long)(e), 0)
#define SYS6(n, a, b, c, d, e, f) sys(n, (long)(a), (long)(b), (long)(c), (long)(d), (long)(e), (long)(f))

/* ---- tiny libc ---- */

static size_t
strlen(const char *s)
{
	size_t n = 0;
	while (s[n]) {
		n++;
	}
	return n;
}

static int
strcmp(const char *a, const char *b)
{
	while (*a && *a == *b) {
		a++;
		b++;
	}
	return (unsigned char)*a - (unsigned char)*b;
}

static int
strncmp(const char *a, const char *b, size_t n)
{
	while (n && *a && *a == *b) {
		a++;
		b++;
		n--;
	}
	return n ? (unsigned char)*a - (unsigned char)*b : 0;
}

static void *
memcpy(void *d, const void *s, size_t n)
{
	unsigned char *dp = d;
	const unsigned char *sp = s;
	while (n--) {
		*dp++ = *sp++;
	}
	return d;
}

static void *
memset(void *d, int c, size_t n)
{
	unsigned char *dp = d;
	while (n--) {
		*dp++ = (unsigned char)c;
	}
	return d;
}

static int console = -1;

static void
puts_fd(int fd, const char *s)
{
	size_t len = strlen(s);
	while (len) {
		long n = SYS3(SYS_write, fd, s, len);
		if (n <= 0) {
			return;
		}
		s += n;
		len -= (size_t)n;
	}
}

static void
out(const char *s)
{
	puts_fd(console, s);
}

static void
out_num(unsigned long long v, int base)
{
	char buf[32];
	int i = sizeof(buf) - 1;
	buf[i] = 0;
	if (v == 0) {
		buf[--i] = '0';
	}
	while (v) {
		int d = (int)(v % (unsigned)base);
		buf[--i] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
		v /= (unsigned)base;
	}
	out(&buf[i]);
}

static void
out_err(const char *what)
{
	out(what);
	out(": errno ");
	out_num((unsigned long long)errno_last, 10);
	out("\n");
}

/* ---- commands ---- */

struct dirent64 {
	uint64_t d_ino;
	uint64_t d_seekoff;
	uint16_t d_reclen;
	uint16_t d_namlen;
	uint8_t  d_type;
	char     d_name[1024];
};

static void
cmd_ls(const char *path)
{
	static char buf[8192];
	long fd = SYS2(SYS_open, path, O_RDONLY);
	long long pos = 0;

	if (fd < 0) {
		out_err(path);
		return;
	}
	for (;;) {
		long n = SYS4(SYS_getdirentries64, fd, buf, sizeof(buf), &pos);
		if (n <= 0) {
			break;
		}
		long off = 0;
		while (off < n) {
			struct dirent64 *d = (struct dirent64 *)(buf + off);
			if (d->d_reclen == 0) {
				break;
			}
			out(d->d_name);
			out(d->d_type == 4 ? "/\n" : "\n");
			off += d->d_reclen;
		}
	}
	SYS1(SYS_close, fd);
}

static void
cmd_cat(const char *path)
{
	static char buf[1024];
	long fd = SYS2(SYS_open, path, O_RDONLY);

	if (fd < 0) {
		out_err(path);
		return;
	}
	for (;;) {
		long n = SYS3(SYS_read, fd, buf, sizeof(buf) - 1);
		if (n <= 0) {
			break;
		}
		buf[n] = 0;
		out(buf);
	}
	SYS1(SYS_close, fd);
}

static void
cmd_sysctl_string(const char *label, int mib0, int mib1)
{
	static char value[512];
	int mib[2] = { mib0, mib1 };
	size_t len = sizeof(value) - 1;

	memset(value, 0, sizeof(value));
	if (SYS6(SYS_sysctl, mib, 2, value, &len, 0, 0) < 0) {
		out_err(label);
		return;
	}
	out(label);
	out(": ");
	out(value);
	out("\n");
}

static void
cmd_sysctl_u64(const char *label, int mib0, int mib1)
{
	int mib[2] = { mib0, mib1 };
	uint64_t value = 0;
	size_t len = sizeof(value);

	if (SYS6(SYS_sysctl, mib, 2, &value, &len, 0, 0) < 0) {
		out_err(label);
		return;
	}
	out(label);
	out(": ");
	out_num(value, 10);
	out("\n");
}

static void
cmd_write(const char *path, const char *text)
{
	long fd = SYS3(SYS_open, path, O_RDWR | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		out_err(path);
		return;
	}
	puts_fd((int)fd, text);
	puts_fd((int)fd, "\n");
	SYS1(SYS_close, fd);
	out("wrote ");
	out(path);
	out("\n");
}

static void
cmd_help(void)
{
	out("commands: help, uname, mem, pid, ls [dir], cat <file>, write <file> <text>, mkdir <dir>, sync, echo ..., reboot, halt\n");
}

static void
run_command(char *line)
{
	char *arg = line;
	while (*arg && *arg != ' ') {
		arg++;
	}
	if (*arg) {
		*arg++ = 0;
		while (*arg == ' ') {
			arg++;
		}
	}

	if (!*line) {
		return;
	} else if (strcmp(line, "help") == 0 || strcmp(line, "?") == 0) {
		cmd_help();
	} else if (strcmp(line, "uname") == 0) {
		cmd_sysctl_string("kern.ostype", 1, 1);
		cmd_sysctl_string("kern.osrelease", 1, 2);
		cmd_sysctl_string("kern.version", 1, 4);
		cmd_sysctl_string("hw.machine", 6, 1);
	} else if (strcmp(line, "mem") == 0) {
		cmd_sysctl_u64("hw.memsize", 6, 24);
		cmd_sysctl_u64("hw.ncpu", 6, 3);
	} else if (strcmp(line, "pid") == 0) {
		out("pid ");
		out_num((unsigned long long)SYS0(SYS_getpid), 10);
		out("\n");
	} else if (strcmp(line, "ls") == 0) {
		cmd_ls(*arg ? arg : "/");
	} else if (strcmp(line, "cat") == 0) {
		if (*arg) {
			cmd_cat(arg);
		}
	} else if (strcmp(line, "write") == 0) {
		char *text = arg;
		while (*text && *text != ' ') {
			text++;
		}
		if (*text) {
			*text++ = 0;
		}
		if (*arg) {
			cmd_write(arg, text);
		}
	} else if (strcmp(line, "mkdir") == 0) {
		if (*arg && SYS2(SYS_mkdir, arg, 0755) < 0) {
			out_err(arg);
		}
	} else if (strcmp(line, "sync") == 0) {
		SYS0(SYS_sync);
		out("synced\n");
	} else if (strcmp(line, "echo") == 0) {
		out(arg);
		out("\n");
	} else if (strcmp(line, "reboot") == 0) {
		SYS0(SYS_sync);
		SYS2(SYS_reboot, 0, 0);
		out_err("reboot");
	} else if (strcmp(line, "halt") == 0) {
		SYS0(SYS_sync);
		SYS2(SYS_reboot, 0x8 /* RB_HALT */, 0);
		out_err("halt");
	} else {
		out("unknown command: ");
		out(line);
		out("\n");
		cmd_help();
	}
}

static void
shell(void)
{
	static char line[256];
	size_t len = 0;

	out("ravynOS# ");
	for (;;) {
		char c;
		long n = SYS3(SYS_read, console, &c, 1);
		if (n <= 0) {
			/* no console input (file-backed serial): idle without spinning */
			struct { long tv_sec; int tv_usec; } tv = { 1, 0 };
			SYS5(SYS_select, 0, 0, 0, 0, &tv);
			continue;
		}
		if (c == '\r' || c == '\n') {
			out("\n");
			line[len] = 0;
			run_command(line);
			len = 0;
			out("ravynOS# ");
		} else if (c == 0x7f || c == '\b') {
			if (len) {
				len--;
				out("\b \b");
			}
		} else if (c >= ' ' && len < sizeof(line) - 1) {
			line[len++] = c;
			SYS3(SYS_write, console, &c, 1);
		}
	}
}

void
ravyninit_main(void)
{
	console = (int)SYS2(SYS_open, "/dev/console", O_RDWR);
	if (console < 0) {
		/* nowhere to talk; keep process 1 alive anyway */
		for (;;) {
			struct { long tv_sec; int tv_usec; } tv = { 10, 0 };
			SYS5(SYS_select, 0, 0, 0, 0, &tv);
		}
	}
	out("\n");
	out("ravynOS init: userland is alive on arm64 (process ");
	out_num((unsigned long long)SYS0(SYS_getpid), 10);
	out(")\n");
	out("This is ravyninit, a freestanding stand-in for launchd. Type 'help'.\n");
	shell();
}
