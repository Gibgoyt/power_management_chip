/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * memcpy.S - hand-written memcpy for the STM32L051 (Cortex-M0+, ARMv6-M Thumb).
 *
 * Selected by -DUSE_OUR_LIBC.  Replaces the toolchain's memcpy so the firmware
 * carries no external libc at all.  The compiler emits `bl memcpy` for struct
 * copies and for the explicit memcpy() calls in the SPI and OTA paths, so this
 * symbol must exist and must follow the AAPCS exactly:
 *
 *   void *memcpy(void *dest, const void *src, size_t n);
 *     r0 = dest, r1 = src, r2 = n, returns dest in r0.
 *
 * ARMv6-M notes (Cortex-M0+ is Thumb-only, no Thumb-2):
 *   - no IT blocks, no conditional instructions other than branches
 *   - LDR/STR with low registers only; LDM/STM only on r0-r7
 *   - so this is deliberately plain: a word loop for the aligned bulk, then a
 *     byte tail.  No fancy unrolling - correctness and small size first, and on
 *     an M0+ the memory bus makes cleverness pointless anyway.
 *
 * Strategy:
 *   1. If dest and src share the same 32-bit alignment, copy words while >= 4
 *      bytes remain, then finish the tail byte by byte.
 *   2. Otherwise (misaligned relative to each other), copy byte by byte.
 * All our real uses (frame buffers, EEPROM records, image half-pages) are word
 * aligned, so path 1 is the common one; path 2 is there for correctness.
 */

	.syntax unified
	.thumb
	.text
	.align	2
	.global	memcpy
	.thumb_func
	.type	memcpy, %function

memcpy:
	push	{r0}			/* keep dest to return it (r0 is clobbered) */
	push	{r4}			/* r4 used as a scratch temp; save it       */

	/* If fewer than 4 bytes, skip straight to the byte loop. */
	cmp	r2, #4
	blo	2f

	/* Word path only if dest and src have the same low 2 address bits. */
	mov	r3, r0
	eors	r3, r3, r1		/* r3 = (dest ^ src) low bits differ? */
	lsls	r3, r3, #30		/* keep only bits [1:0] */
	bne	2f			/* different alignment -> byte copy   */

	/* Advance both to a word boundary one byte at a time (0-3 bytes). */
1:
	mov	r3, r0
	lsls	r3, r3, #30		/* dest word aligned yet? */
	beq	10f
	ldrb	r3, [r1]
	strb	r3, [r0]
	adds	r0, r0, #1
	adds	r1, r1, #1
	subs	r2, r2, #1
	b	1b

	/* Aligned word loop: copy 4 bytes at a time while >= 4 remain. */
10:
	cmp	r2, #4
	blo	2f
	ldr	r3, [r1]
	str	r3, [r0]
	adds	r0, r0, #4
	adds	r1, r1, #4
	subs	r2, r2, #4
	b	10b

	/* Byte tail / misaligned fallback. */
2:
	cmp	r2, #0
	beq	3f
	ldrb	r3, [r1]
	strb	r3, [r0]
	adds	r0, r0, #1
	adds	r1, r1, #1
	subs	r2, r2, #1
	b	2b

3:
	pop	{r4}
	pop	{r0}			/* restore original dest as return value */
	bx	lr

	.size	memcpy, . - memcpy
