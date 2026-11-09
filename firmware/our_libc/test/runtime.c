/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * runtime.c - a tiny freestanding runtime so the mem* test can run under
 *             qemu-arm-static with no libc at all.
 *
 * qemu-arm-static is Linux user-mode emulation: it runs an ARM Linux ELF and
 * forwards real syscalls.  We don't have an arm-linux cross toolchain, only the
 * bare-metal arm-none-eabi one, so we cannot link glibc/newlib for stdio.
 * Instead this provides just enough - _start, write(), exit() via raw SVC, and a
 * minimal print - for test_mem.c to report and set an exit code.  This is test
 * scaffolding only; none of it ships in the firmware.
 *
 * ARM EABI Linux syscall convention: number in r7, args in r0.., SVC #0.
 */

#include <stddef.h>

#define SYS_exit   1
#define SYS_write  4

static long syscall3(long n, long a, long b, long c)
{
	register long r7 __asm__("r7") = n;
	register long r0 __asm__("r0") = a;
	register long r1 __asm__("r1") = b;
	register long r2 __asm__("r2") = c;

	__asm__ volatile("svc #0"
			 : "+r"(r0)
			 : "r"(r7), "r"(r1), "r"(r2)
			 : "memory");
	return r0;
}

void _exit(int code)
{
	syscall3(SYS_exit, code, 0, 0);
	for (;;)
		;
}

/* Minimal stdout writer used by the test's print helpers. */
static void out(const char *s, size_t n)
{
	syscall3(SYS_write, 1, (long)s, (long)n);
}

static size_t slen(const char *s)
{
	size_t n = 0;

	while (s[n])
		n++;
	return n;
}

void print(const char *s)
{
	out(s, slen(s));
}

/* Print an unsigned decimal - enough for the test's diagnostics. */
void print_u(unsigned v)
{
	char buf[12];
	int i = (int)sizeof(buf);

	buf[--i] = '\0';
	if (v == 0)
		buf[--i] = '0';
	while (v && i > 0) {
		buf[--i] = (char)('0' + (v % 10u));
		v /= 10u;
	}
	print(&buf[i]);
}

/* Print a two-digit hex byte. */
void print_hex2(unsigned v)
{
	static const char h[] = "0123456789abcdef";
	char buf[3];

	buf[0] = h[(v >> 4) & 0xF];
	buf[1] = h[v & 0xF];
	buf[2] = '\0';
	print(buf);
}

extern int test_main(void);

void _start(void)
{
	_exit(test_main());
}
