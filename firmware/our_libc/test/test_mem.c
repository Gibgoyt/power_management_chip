/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * test_mem.c - correctness check for our memcpy / memset implementations.
 *
 * memcpy/memset are exactly the kind of code that is easy to get subtly wrong at
 * the word-alignment boundaries, so this hammers every combination that matters:
 * source and destination at each of 4 alignments, and every length from 0 up
 * through several words, comparing the result byte-for-byte against a trusted
 * reference (the host's own memcpy/memset, renamed so it cannot clash).
 *
 * The implementation under test is linked in separately:
 *   - LIBC=our  : our_libc/memcpy.S + memset.S, cross-compiled for ARM and run
 *                 under qemu-arm-static, so the actual shipped assembly executes.
 *   - LIBC=musl : the vendored musl memcpy.c/memset.c, same way.
 * The firmware Makefile target `libc-check` builds and runs both.
 *
 * The functions under test are declared here; whichever object is linked
 * provides them.
 */

#include <stddef.h>
#include <stdint.h>

/* Provided by the object under test (assembly, or vendored musl C). */
void *memcpy(void *dest, const void *src, size_t n);
void *memset(void *dest, int c, size_t n);

/*
 * Minimal freestanding output, from runtime.c - the test runs under
 * qemu-arm-static with no libc, so there is no printf.  These cover everything
 * the diagnostics below need.
 */
void print(const char *s);
void print_u(unsigned v);
void print_hex2(unsigned v);

/* Trusted reference, written out longhand so it needs no libc of its own. */
static void ref_memcpy(uint8_t *d, const uint8_t *s, size_t n)
{
	while (n--)
		*d++ = *s++;
}

static void ref_memset(uint8_t *d, int c, size_t n)
{
	while (n--)
		*d++ = (uint8_t)c;
}

#define PAD      8u          /* guard bytes around each buffer */
#define MAXLEN   40u         /* covers several words plus tails */
#define BUFSZ    (PAD + MAXLEN + PAD)

static int failures;

static void fill_pattern(uint8_t *b, size_t n, unsigned seed)
{
	size_t i;

	for (i = 0; i < n; i++)
		b[i] = (uint8_t)(seed * 131u + i * 17u + 7u);
}

/*
 * One memcpy case: dest at doff, src at soff, length len.  Verifies the copied
 * region matches, and - just as important - that nothing outside [dest,dest+len)
 * was touched.
 */
static void check_memcpy(unsigned doff, unsigned soff, size_t len)
{
	uint8_t dut[BUFSZ], ref[BUFSZ], src[BUFSZ];
	uint8_t *dd = dut + PAD + doff;
	uint8_t *rd = ref + PAD + doff;
	uint8_t *ss = src + PAD + soff;
	size_t i;

	fill_pattern(dut, BUFSZ, 1);
	fill_pattern(ref, BUFSZ, 1);       /* identical starting content */
	fill_pattern(src, BUFSZ, 99);

	memcpy(dd, ss, len);               /* implementation under test */
	ref_memcpy(rd, ss, len);           /* reference                 */

	for (i = 0; i < BUFSZ; i++) {
		if (dut[i] != ref[i]) {
			print("FAIL memcpy doff="); print_u(doff);
			print(" soff="); print_u(soff);
			print(" len="); print_u((unsigned)len);
			print(" byte="); print_u((unsigned)i);
			print(" got 0x"); print_hex2(dut[i]);
			print(" want 0x"); print_hex2(ref[i]);
			print("\n");
			failures++;
			return;
		}
	}
}

static void check_memset(unsigned doff, size_t len, int c)
{
	uint8_t dut[BUFSZ], ref[BUFSZ];
	uint8_t *dd = dut + PAD + doff;
	uint8_t *rd = ref + PAD + doff;
	size_t i;

	fill_pattern(dut, BUFSZ, 5);
	fill_pattern(ref, BUFSZ, 5);

	memset(dd, c, len);
	ref_memset(rd, c, len);

	for (i = 0; i < BUFSZ; i++) {
		if (dut[i] != ref[i]) {
			print("FAIL memset doff="); print_u(doff);
			print(" len="); print_u((unsigned)len);
			print(" c="); print_u((unsigned)c);
			print(" byte="); print_u((unsigned)i);
			print(" got 0x"); print_hex2(dut[i]);
			print(" want 0x"); print_hex2(ref[i]);
			print("\n");
			failures++;
			return;
		}
	}
}

int test_main(void)
{
	unsigned doff, soff;
	size_t len;

	print("our_libc mem* test\n");

	/* memcpy: every dest/src alignment, every length. */
	for (doff = 0; doff < 4; doff++)
		for (soff = 0; soff < 4; soff++)
			for (len = 0; len <= MAXLEN; len++)
				check_memcpy(doff, soff, len);

	/* memset: every alignment, every length, a few fill bytes. */
	for (doff = 0; doff < 4; doff++)
		for (len = 0; len <= MAXLEN; len++) {
			check_memset(doff, len, 0x00);
			check_memset(doff, len, 0xFF);
			check_memset(doff, len, 0xA5);
			/* Non-zero high bits in c must be ignored (c is a byte). */
			check_memset(doff, len, 0x1234);
		}

	if (failures) {
		print("\n"); print_u((unsigned)failures);
		print(" case(s) failed\n");
		return 1;
	}

	print("all mem* cases passed\n");
	return 0;
}
