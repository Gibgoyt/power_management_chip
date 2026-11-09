/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * u41_layout.h — Flash / EEPROM memory map for U41 (STM32L051K6U6TR).
 *
 * Part facts, taken from the datasheet and RM0377 (docs/stm32l051/):
 *
 *   STM32L051K6U6TR : UFQFPN32, "6" suffix = 32 Kbytes Flash.
 *                     8 Kbytes SRAM, 2 Kbytes data EEPROM.
 *                     Category 3 device.
 *
 *   RM0377 Table 7 note 1:
 *     "For 32 Kbyte category 3 devices, the Flash program memory is divided
 *      into 256 pages of 128 bytes each."
 *
 *   RM0377 3.3.2 "Bank swapping (category 5 devices only)":
 *     bank swapping / dual-bank boot DOES NOT EXIST on this part.  There is
 *     no hardware mechanism that remaps a second image to 0x08000000.  The
 *     A/B swap in this project is done entirely in software by the
 *     bootloader in bootloader/main.c.
 *
 *   RM0377 3.3.1:
 *     "When the NVM features only one bank, it is not possible to read the
 *      NVM during a write/erase operation ... stalling the master that
 *      requested the read operation."
 *     => every erase/program routine must execute from SRAM.  See
 *        common/flash.c and the .ramfunc section in the linker scripts.
 *
 * Layout (32 Kbytes total):
 *
 *   0x08000000 .. 0x08000FFF   4 KiB   bootloader        (32 pages)
 *   0x08001000 .. 0x080047FF  14 KiB   application slot A (112 pages)
 *   0x08004800 .. 0x08007FFF  14 KiB   application slot B (112 pages)
 *
 *   0x08080000 .. 0x080807FF   2 KiB   data EEPROM
 *       +0x000  boot metadata record 0   (32 bytes)
 *       +0x020  boot metadata record 1   (32 bytes)
 *       +0x040  .. free for application use
 *
 * Both slot bases are 256-byte aligned, which the Cortex-M0+ VTOR requires
 * (ARMv6-M: VTOR[31:8], table must be aligned to a power of two >= the table
 * size; 48 entries * 4 = 192 bytes -> 256).
 */

#ifndef U41_LAYOUT_H
#define U41_LAYOUT_H

#include <stdint.h>

/* ------------------------------------------------------------------ flash */

#define U41_FLASH_BASE          0x08000000UL
#define U41_FLASH_SIZE          (32UL * 1024UL)
#define U41_FLASH_END           (U41_FLASH_BASE + U41_FLASH_SIZE)

/* RM0377 Table 7: 128-byte pages, half-page (64 byte) program granularity. */
#define U41_FLASH_PAGE_SIZE     128UL
#define U41_FLASH_HALFPAGE_SIZE 64UL
#define U41_FLASH_WORD_SIZE     4UL

#define U41_BOOT_BASE           U41_FLASH_BASE
#define U41_BOOT_SIZE           (4UL * 1024UL)

#define U41_SLOT_SIZE           (14UL * 1024UL)
#define U41_SLOT_A_BASE         (U41_BOOT_BASE + U41_BOOT_SIZE)     /* 0x08001000 */
#define U41_SLOT_B_BASE         (U41_SLOT_A_BASE + U41_SLOT_SIZE)   /* 0x08004800 */

#define U41_SLOT_PAGES          (U41_SLOT_SIZE / U41_FLASH_PAGE_SIZE)   /* 112 */

/* Slot identifiers. */
#define U41_SLOT_A              0u
#define U41_SLOT_B              1u
#define U41_SLOT_NONE           0xFFu

#define U41_SLOT_OTHER(s)       ((uint8_t)((s) == U41_SLOT_A ? U41_SLOT_B : U41_SLOT_A))

/* ----------------------------------------------------------------- eeprom */

#define U41_EEPROM_BASE         0x08080000UL
#define U41_EEPROM_SIZE         (2UL * 1024UL)

#define U41_META_SLOT_0_OFFSET  0x000UL
#define U41_META_SLOT_1_OFFSET  0x020UL
#define U41_META_RECORD_SIZE    32UL

/* -------------------------------------------------------------- image hdr */

/*
 * Every application image carries a 32-byte header placed at slot_base +
 * U41_IMAGE_HDR_OFFSET.  The offset sits above the 192-byte vector table and
 * below the 256-byte VTOR alignment boundary is NOT possible, so the header
 * lives immediately after the aligned vector region.
 */
#define U41_IMAGE_HDR_OFFSET    0x100UL
#define U41_IMAGE_MAGIC         0x49313455UL   /* 'U41I' little endian */
#define U41_IMAGE_HDR_VERSION   1u

typedef struct __attribute__((packed)) {
	uint32_t magic;        /* U41_IMAGE_MAGIC                                */
	uint16_t hdr_version;  /* U41_IMAGE_HDR_VERSION                          */
	uint16_t slot;         /* slot this image was linked for (U41_SLOT_A/B)  */
	uint32_t load_addr;    /* expected slot base — guards a mislinked image  */
	uint32_t image_size;   /* total image bytes, from slot_base              */
	uint32_t image_crc32;  /* CRC32 of the image with THIS FIELD READ AS 0   */
	uint32_t fw_version;   /* U41_VERSION(maj,min,patch)                     */
	uint32_t build_id;     /* free-form; mkimage stamps the build epoch      */
	uint32_t reserved;
} u41_image_hdr_t;

_Static_assert(sizeof(u41_image_hdr_t) == 32, "image header must be 32 bytes");

#define U41_VERSION(maj, min, patch) \
	((((uint32_t)(maj) & 0xFFu) << 16) | \
	 (((uint32_t)(min) & 0xFFu) <<  8) | \
	  ((uint32_t)(patch) & 0xFFu))

#define U41_VERSION_MAJOR(v)  (((v) >> 16) & 0xFFu)
#define U41_VERSION_MINOR(v)  (((v) >>  8) & 0xFFu)
#define U41_VERSION_PATCH(v)  ( (v)        & 0xFFu)

/* Byte offset of image_crc32 inside the header — the CRC hole. */
#define U41_IMAGE_CRC_FIELD_OFFSET \
	(U41_IMAGE_HDR_OFFSET + 16UL)

/* --------------------------------------------------------------- helpers */

static inline uint32_t u41_slot_base(uint8_t slot)
{
	return (slot == U41_SLOT_B) ? U41_SLOT_B_BASE : U41_SLOT_A_BASE;
}

#endif /* U41_LAYOUT_H */
