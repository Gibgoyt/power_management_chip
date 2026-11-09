/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * memset.S - hand-written memset for the STM32L051 (Cortex-M0+, ARMv6-M Thumb).
 *
 * Selected by -DUSE_OUR_LIBC.  AAPCS:
 *
 *   void *memset(void *dest, int c, size_t n);
 *     r0 = dest, r1 = c (only the low byte matters), r2 = n, returns dest.
 *
 * The bulk of our memset() calls zero a struct or a frame buffer, so the fill
 * byte is almost always 0 and the buffers are word aligned.  This still handles
 * any byte value and any alignment correctly:
 *   1. build a word made of four copies of the fill byte,
 *   2. reach a word boundary byte by byte,
 *   3. store whole words while >= 4 bytes remain,
 *   4. finish the tail byte by byte.
 *
 * ARMv6-M: Thumb only, low registers, no IT blocks - kept deliberately plain.
 */

	.syntax unified
	.thumb
	.text
	.align	2
	.global	memset
	.thumb_func
	.type	memset, %function

memset:
	push	{r0}			/* preserve dest for the return value */
	push	{r4}			/* scratch */

	uxtb	r1, r1			/* c &= 0xFF - only the low byte is used */

	/* Build r4 = c:c:c:c so we can store a word at a time. */
	lsls	r4, r1, #8
	orrs	r4, r4, r1		/* r4 = 0x0000_cccc */
	lsls	r3, r4, #16
	orrs	r4, r4, r3		/* r4 = 0xcccc_cccc */

	cmp	r2, #4
	blo	2f

	/* Byte-fill up to a word boundary (0-3 bytes). */
1:
	mov	r3, r0
	lsls	r3, r3, #30		/* word aligned? */
	beq	10f
	strb	r1, [r0]
	adds	r0, r0, #1
	subs	r2, r2, #1
	b	1b

	/* Word-fill loop. */
10:
	cmp	r2, #4
	blo	2f
	str	r4, [r0]
	adds	r0, r0, #4
	subs	r2, r2, #4
	b	10b

	/* Byte tail. */
2:
	cmp	r2, #0
	beq	3f
	strb	r1, [r0]
	adds	r0, r0, #1
	subs	r2, r2, #1
	b	2b

3:
	pop	{r4}
	pop	{r0}			/* return original dest */
	bx	lr

	.size	memset, . - memset
