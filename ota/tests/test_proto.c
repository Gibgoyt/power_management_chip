/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * test_proto.c - host-side checks that need no hardware.
 *
 * These exist because the expensive failures in this project are all
 * disagreements between two pieces of code that cannot be run side by side:
 * the firmware on a 32 KiB MCU and this tool on the SoM.  If the CRC
 * convention, the frame layout or the image-header maths drift apart, the
 * symptom is an OTA that fails verification after a minute of writing, or
 * worse, one that passes and then does not boot.
 *
 * Everything checked here is checked against the same headers and the same
 * crc32.c the firmware compiles.
 */

#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#include "u41_layout.h"
#include "u41_meta.h"
#include "u41_proto.h"

static int failures;

#define CHECK(cond, ...) do { \
	if (!(cond)) { \
		printf("FAIL %s:%d: ", __FILE__, __LINE__); \
		printf(__VA_ARGS__); \
		printf("\n"); \
		failures++; \
	} \
} while (0)

static void put_le32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v);
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

/*
 * The CRC must be bit-for-bit the standard IEEE 802.3 one, because
 * mkimage.py stamps images using Python's binascii.crc32 while the device
 * verifies them with crc32.c.  If these two ever diverge, every OTA fails
 * verification and the reason is not obvious from the error.
 */
static void test_crc32_is_standard(void)
{
	/* The canonical check value: CRC32("123456789") == 0xCBF43926. */
	uint32_t crc = u41_crc32("123456789", 9);

	CHECK(crc == 0xCBF43926u,
	      "CRC32(\"123456789\") = 0x%08x, expected 0xCBF43926. "
	      "crc32.c no longer matches Python's binascii.crc32, so "
	      "mkimage.py and the device will disagree.", crc);

	CHECK(u41_crc32("", 0) == 0u, "CRC32 of an empty buffer should be 0");

	/* Incremental and one-shot must agree; the device CRCs in pieces. */
	{
		uint32_t inc = 0xFFFFFFFFu;

		inc = u41_crc32_update(inc, "1234", 4);
		inc = u41_crc32_update(inc, "56789", 5);
		inc = u41_crc32_final(inc);

		CHECK(inc == crc,
		      "incremental CRC 0x%08x != one-shot 0x%08x", inc, crc);
	}
}

/* The wire format is fixed; a silent change here breaks every deployed unit. */
static void test_frame_layout(void)
{
	CHECK(U41_FRAME_SIZE == 80, "frame size is %u, expected 80",
	      U41_FRAME_SIZE);
	CHECK(U41_OFF_CRC32 + 4 == U41_FRAME_SIZE,
	      "CRC must be the last field in the frame");
	CHECK(U41_OFF_PAYLOAD + U41_PAYLOAD_MAX == U41_OFF_CRC32,
	      "payload must run right up to the CRC");
	CHECK(U41_CRC_LEN == U41_OFF_CRC32,
	      "CRC must cover everything before itself");

	/*
	 * One payload is one half page.  This is what lets a single OTA_DATA
	 * frame map onto exactly one STM32L0 program operation.
	 */
	CHECK(U41_PAYLOAD_MAX == U41_FLASH_HALFPAGE_SIZE,
	      "payload (%u) should equal the flash half page (%lu)",
	      U41_PAYLOAD_MAX, (unsigned long)U41_FLASH_HALFPAGE_SIZE);
}

/* Structs cross the wire byte for byte, so their sizes are part of the ABI. */
static void test_struct_sizes(void)
{
	CHECK(sizeof(u41_image_hdr_t) == 32,
	      "image header is %zu bytes, expected 32",
	      sizeof(u41_image_hdr_t));
	CHECK(sizeof(u41_bootmeta_t) == 32,
	      "boot metadata is %zu bytes, expected 32",
	      sizeof(u41_bootmeta_t));
	CHECK(sizeof(u41_info_t) <= U41_PAYLOAD_MAX,
	      "info payload is %zu bytes, max is %u",
	      sizeof(u41_info_t), U41_PAYLOAD_MAX);
	CHECK(sizeof(u41_pm_status_t) <= U41_PAYLOAD_MAX,
	      "pm payload is %zu bytes", sizeof(u41_pm_status_t));
	CHECK(sizeof(u41_ota_status_t) <= U41_PAYLOAD_MAX,
	      "ota payload is %zu bytes", sizeof(u41_ota_status_t));

	/* offsetof of image_crc32 must match the constant used for the hole. */
	CHECK(U41_IMAGE_CRC_FIELD_OFFSET ==
	      U41_IMAGE_HDR_OFFSET + offsetof(u41_image_hdr_t, image_crc32),
	      "U41_IMAGE_CRC_FIELD_OFFSET (0x%lx) does not match the real "
	      "offset of image_crc32 (0x%lx)",
	      (unsigned long)U41_IMAGE_CRC_FIELD_OFFSET,
	      (unsigned long)(U41_IMAGE_HDR_OFFSET +
			      offsetof(u41_image_hdr_t, image_crc32)));
}

/*
 * The memory map has to close exactly.  32 KiB is all there is, and an
 * arithmetic slip would put slot B's tail past the end of flash - which the
 * linker would not catch, because each slot is linked independently.
 */
static void test_memory_map(void)
{
	CHECK(U41_BOOT_SIZE + 2 * U41_SLOT_SIZE == U41_FLASH_SIZE,
	      "bootloader (%lu) + 2 slots (%lu) = %lu, flash is %lu",
	      (unsigned long)U41_BOOT_SIZE, (unsigned long)U41_SLOT_SIZE,
	      (unsigned long)(U41_BOOT_SIZE + 2 * U41_SLOT_SIZE),
	      (unsigned long)U41_FLASH_SIZE);

	CHECK(U41_SLOT_A_BASE == U41_BOOT_BASE + U41_BOOT_SIZE,
	      "slot A must start where the bootloader ends");
	CHECK(U41_SLOT_B_BASE == U41_SLOT_A_BASE + U41_SLOT_SIZE,
	      "slot B must start where slot A ends");
	CHECK(U41_SLOT_B_BASE + U41_SLOT_SIZE == U41_FLASH_END,
	      "slot B must end exactly at the end of flash");

	/* ARMv6-M VTOR only implements bits [31:8]. */
	CHECK((U41_SLOT_A_BASE & 0xFFu) == 0,
	      "slot A base 0x%08lx is not 256-byte aligned",
	      (unsigned long)U41_SLOT_A_BASE);
	CHECK((U41_SLOT_B_BASE & 0xFFu) == 0,
	      "slot B base 0x%08lx is not 256-byte aligned",
	      (unsigned long)U41_SLOT_B_BASE);

	/* Every region boundary must land on a flash page. */
	CHECK((U41_BOOT_SIZE % U41_FLASH_PAGE_SIZE) == 0,
	      "bootloader is not a whole number of pages");
	CHECK((U41_SLOT_SIZE % U41_FLASH_PAGE_SIZE) == 0,
	      "slot size is not a whole number of pages");
	CHECK(U41_SLOT_PAGES == 112, "slot should be 112 pages, got %lu",
	      (unsigned long)U41_SLOT_PAGES);

	/* The header must fit inside the slot, after the vector table. */
	CHECK(U41_IMAGE_HDR_OFFSET >= 192,
	      "image header at 0x%lx would overlap the 192-byte vector table",
	      (unsigned long)U41_IMAGE_HDR_OFFSET);

	CHECK(U41_SLOT_OTHER(U41_SLOT_A) == U41_SLOT_B, "A's other slot is B");
	CHECK(U41_SLOT_OTHER(U41_SLOT_B) == U41_SLOT_A, "B's other slot is A");

	/* Two metadata records must not overlap. */
	CHECK(U41_META_SLOT_0_OFFSET + U41_META_RECORD_SIZE <=
	      U41_META_SLOT_1_OFFSET,
	      "the two EEPROM metadata records overlap");
	CHECK(U41_META_SLOT_1_OFFSET + U41_META_RECORD_SIZE <= U41_EEPROM_SIZE,
	      "metadata record 1 runs past the end of EEPROM");
}

/*
 * Build a frame the way u41_link.c does and take it apart the way the
 * firmware's spi_slave.c does, checking the CRC survives the round trip.
 */
static void test_frame_roundtrip(void)
{
	uint8_t f[U41_FRAME_SIZE];
	uint8_t payload[U41_PAYLOAD_MAX];
	unsigned i;
	uint32_t crc, readback;

	for (i = 0; i < sizeof(payload); i++)
		payload[i] = (uint8_t)(i * 7u + 3u);

	memset(f, 0, sizeof(f));
	f[U41_OFF_SOF]    = U41_SOF_REQ;
	f[U41_OFF_CMD]    = U41_CMD_OTA_DATA;
	f[U41_OFF_SEQ]    = 0x42;
	f[U41_OFF_STATUS] = 0;
	f[U41_OFF_LEN]    = U41_PAYLOAD_MAX;
	put_le32(&f[U41_OFF_ARG32], 0x00001234u);
	memcpy(&f[U41_OFF_PAYLOAD], payload, sizeof(payload));

	crc = u41_crc32(f, U41_CRC_LEN);
	put_le32(&f[U41_OFF_CRC32], crc);

	readback = (uint32_t)f[U41_OFF_CRC32]
		 | ((uint32_t)f[U41_OFF_CRC32 + 1] << 8)
		 | ((uint32_t)f[U41_OFF_CRC32 + 2] << 16)
		 | ((uint32_t)f[U41_OFF_CRC32 + 3] << 24);

	CHECK(readback == crc, "little-endian CRC round trip failed");
	CHECK(u41_crc32(f, U41_CRC_LEN) == crc, "CRC is not stable");
	CHECK(memcmp(&f[U41_OFF_PAYLOAD], payload, sizeof(payload)) == 0,
	      "payload was corrupted by framing");

	/* A single flipped bit anywhere must invalidate the frame. */
	f[U41_OFF_PAYLOAD + 5] ^= 0x01u;
	CHECK(u41_crc32(f, U41_CRC_LEN) != crc,
	      "CRC did not change after a payload bit flip");
}

/*
 * Reproduce the image CRC convention: CRC over the whole image with the four
 * bytes of image_crc32 read as zero.  mkimage.py, bootloader/main.c and
 * app/ota.c all have to agree on this, and there is no way to check them
 * against each other at run time.
 */
static void test_image_crc_convention(void)
{
	size_t len = 1024;
	uint8_t *img = calloc(1, len);
	u41_image_hdr_t *hdr;
	uint32_t crc, zero = 0, recomputed;
	size_t before, after;
	unsigned i;

	if (!img) {
		printf("FAIL: out of memory\n");
		failures++;
		return;
	}

	for (i = 0; i < len; i++)
		img[i] = (uint8_t)(i * 31u + 11u);

	hdr = (u41_image_hdr_t *)(img + U41_IMAGE_HDR_OFFSET);
	memset(hdr, 0, sizeof(*hdr));
	hdr->magic       = U41_IMAGE_MAGIC;
	hdr->hdr_version = U41_IMAGE_HDR_VERSION;
	hdr->slot        = U41_SLOT_A;
	hdr->load_addr   = U41_SLOT_A_BASE;
	hdr->image_size  = (uint32_t)len;
	hdr->image_crc32 = 0;

	/* Stamp, the way mkimage.py does: CRC the buffer with the hole zeroed. */
	crc = u41_crc32(img, (uint32_t)len);
	hdr->image_crc32 = crc;

	/* Verify, the way the bootloader does: three pieces with a zero hole. */
	before = U41_IMAGE_CRC_FIELD_OFFSET;
	after  = len - before - 4;

	recomputed = u41_crc32_update(0xFFFFFFFFu, img, (uint32_t)before);
	recomputed = u41_crc32_update(recomputed, &zero, 4);
	recomputed = u41_crc32_update(recomputed, img + before + 4,
				      (uint32_t)after);
	recomputed = u41_crc32_final(recomputed);

	CHECK(recomputed == crc,
	      "image CRC convention mismatch: stamped 0x%08x, "
	      "bootloader would compute 0x%08x", crc, recomputed);

	free(img);
}

int main(void)
{
	printf("u41 protocol self-test\n");

	test_crc32_is_standard();
	test_frame_layout();
	test_struct_sizes();
	test_memory_map();
	test_frame_roundtrip();
	test_image_crc_convention();

	if (failures) {
		printf("\n%d check(s) failed\n", failures);
		return 1;
	}

	printf("all checks passed\n");

	return 0;
}
