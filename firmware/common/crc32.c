/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * crc32.c - IEEE 802.3 CRC32, the same one zlib uses.
 *
 * Reflected algorithm, polynomial 0xEDB88320, init 0xFFFFFFFF, final xor
 * 0xFFFFFFFF.  A 16-entry nibble table costs 64 bytes of flash and runs about
 * four times faster than the bitwise loop, which matters because the
 * bootloader CRCs a whole 14 KiB slot on every boot.
 *
 * Compiled into both the firmware and the Linux host tool so the two sides
 * cannot disagree about an image checksum.
 */

#include "u41_proto.h"

static const uint32_t crc32_nibble[16] = {
	0x00000000UL, 0x1DB71064UL, 0x3B6E20C8UL, 0x26D930ACUL,
	0x76DC4190UL, 0x6B6B51F4UL, 0x4DB26158UL, 0x5005713CUL,
	0xEDB88320UL, 0xF00F9344UL, 0xD6D6A3E8UL, 0xCB61B38CUL,
	0x9B64C2B0UL, 0x86D3D2D4UL, 0xA00AE278UL, 0xBDBDF21CUL,
};

uint32_t u41_crc32_update(uint32_t crc, const void *data, uint32_t len)
{
	const uint8_t *p = (const uint8_t *)data;

	while (len--) {
		crc ^= *p++;
		crc = (crc >> 4) ^ crc32_nibble[crc & 0x0Fu];
		crc = (crc >> 4) ^ crc32_nibble[crc & 0x0Fu];
	}

	return crc;
}

uint32_t u41_crc32_final(uint32_t crc)
{
	return crc ^ 0xFFFFFFFFUL;
}

uint32_t u41_crc32(const void *data, uint32_t len)
{
	return u41_crc32_final(u41_crc32_update(0xFFFFFFFFUL, data, len));
}
