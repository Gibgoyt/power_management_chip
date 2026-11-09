/* SPDX-License-Identifier: MIT */
/*
 * endian.h - minimal freestanding <endian.h> for the vendored musl memcpy.c.
 *
 * Upstream musl's memcpy.c does `#include <endian.h>` and tests
 * `#if __BYTE_ORDER == __LITTLE_ENDIAN`.  A -nostdlib bare-metal build has no
 * <endian.h>, so this supplies exactly those three macros from the compiler's
 * own byte-order built-ins.  It is only on the include path for the LIBC=musl
 * compile (see the Makefile), so it cannot affect anything else.
 *
 * The STM32L0 core is little-endian (the only mode it supports); the values
 * here come from GCC, so they are correct by construction rather than assumed.
 */

#ifndef U41_MUSL_ENDIAN_H
#define U41_MUSL_ENDIAN_H

#define __LITTLE_ENDIAN  __ORDER_LITTLE_ENDIAN__
#define __BIG_ENDIAN     __ORDER_BIG_ENDIAN__
#define __BYTE_ORDER     __BYTE_ORDER__

#endif /* U41_MUSL_ENDIAN_H */
