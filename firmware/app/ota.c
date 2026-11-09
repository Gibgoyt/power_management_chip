/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * ota.c - receive a firmware image into the inactive slot.
 *
 * The whole point of the A/B split is visible here: every flash operation in
 * this file targets the slot we are NOT executing from.  The running image
 * keeps SOM_PSU_EN asserted and keeps servicing the SoM for the entire
 * update, and nothing observable changes until the next power cycle, when the
 * bootloader adopts what we wrote.
 *
 * Sequence from the host's side:
 *
 *   OTA_BEGIN    declare size, CRC and version; slot erase starts
 *   OTA_STATUS   poll until phase leaves ERASING
 *   OTA_DATA x N 64 bytes per frame, ascending offsets
 *   OTA_END      re-read the slot, check the CRC, mark it PENDING
 *   (power cycle)
 *
 * A failure at any point before OTA_END leaves the active slot untouched, so
 * the worst case is a wasted erase.
 */

#include <string.h>

#include "u41_flash.h"
#include "u41_layout.h"
#include "u41_meta.h"
#include "u41_ota.h"
#include "u41_pm.h"
#include "u41_proto.h"

static struct {
	u41_bootmeta_t meta;

	uint8_t  phase;        /* u41_ota_phase_t */
	uint8_t  target;       /* slot being written */
	uint8_t  last_error;

	uint32_t image_size;
	uint32_t want_crc;
	uint32_t version;

	uint32_t bytes_written;
	uint32_t next_offset;   /* offset the next OTA_DATA must carry */

	uint32_t pages_erased;
	uint32_t pages_total;

	int      unlocked;
} ota;

static void ota_reset(void)
{
	if (ota.unlocked) {
		u41_flash_lock();
		ota.unlocked = 0;
	}

	ota.phase         = U41_OTA_IDLE;
	ota.image_size    = 0;
	ota.want_crc      = 0;
	ota.version       = 0;
	ota.bytes_written = 0;
	ota.next_offset   = 0;
	ota.pages_erased  = 0;
	ota.pages_total   = 0;
}

void u41_ota_init(const u41_bootmeta_t *meta)
{
	memset(&ota, 0, sizeof(ota));
	ota.meta   = *meta;
	ota.phase  = U41_OTA_IDLE;
	ota.target = U41_SLOT_OTHER(meta->active_slot);
}

const u41_bootmeta_t *u41_ota_meta(void)
{
	return &ota.meta;
}

void u41_ota_poll(void)
{
	uint32_t addr;
	int rc;

	if (ota.phase != U41_OTA_ERASING)
		return;

	/*
	 * Never stall the core outside a stable RUN state.  If the board is
	 * sequencing rails or counting down a shutdown, the erase simply waits
	 * - the host is polling OTA_STATUS and will see ERASING for longer.
	 */
	if (!u41_pm_flash_window_open())
		return;

	if (ota.pages_erased >= ota.pages_total) {
		ota.phase = U41_OTA_RECEIVING;
		return;
	}

	addr = u41_slot_base(ota.target) +
	       ota.pages_erased * U41_FLASH_PAGE_SIZE;

	rc = u41_flash_erase_page(addr);
	if (rc != U41_FLASH_OK) {
		ota.last_error = U41_ST_FLASH_ERR;
		ota.phase      = U41_OTA_ERROR;
		ota_reset();
		ota.phase      = U41_OTA_ERROR;
		return;
	}

	/* One page per pass, so the power manager runs between erases. */
	ota.pages_erased++;
}

uint8_t u41_ota_begin(uint32_t image_size, uint32_t crc32, uint32_t version)
{
	if (image_size < (U41_IMAGE_HDR_OFFSET + sizeof(u41_image_hdr_t)))
		return U41_ST_BAD_ARG;
	if (image_size > U41_SLOT_SIZE)
		return U41_ST_BAD_ARG;

	if (ota.phase == U41_OTA_ERASING || ota.phase == U41_OTA_VERIFYING)
		return U41_ST_BUSY;

	/*
	 * Always write the slot we are not running from.  The host does not
	 * get to choose: picking the wrong one would erase the executing
	 * image and take the board down mid-update.
	 */
	ota.target = U41_SLOT_OTHER(ota.meta.active_slot);

	ota_reset();

	ota.image_size  = image_size;
	ota.want_crc    = crc32;
	ota.version     = version;
	ota.pages_total = (image_size + U41_FLASH_PAGE_SIZE - 1UL) /
			  U41_FLASH_PAGE_SIZE;
	ota.last_error  = U41_ST_OK;

	if (u41_flash_unlock() != U41_FLASH_OK) {
		ota.last_error = U41_ST_FLASH_ERR;
		ota.phase      = U41_OTA_ERROR;
		return U41_ST_FLASH_ERR;
	}
	ota.unlocked = 1;

	ota.phase = U41_OTA_ERASING;

	return U41_ST_OK;
}

uint8_t u41_ota_data(uint32_t offset, const uint8_t *data, uint8_t len)
{
	/* Word-aligned scratch: the flash controller writes 32 bits at a time. */
	static uint32_t chunk[U41_FLASH_HALFPAGE_SIZE / 4u];
	uint32_t addr;
	int rc;

	if (ota.phase == U41_OTA_ERASING)
		return U41_ST_BUSY;
	if (ota.phase != U41_OTA_RECEIVING)
		return U41_ST_BAD_STATE;

	if (len == 0u || len > U41_FLASH_HALFPAGE_SIZE)
		return U41_ST_BAD_LEN;
	if ((offset % U41_FLASH_HALFPAGE_SIZE) != 0UL)
		return U41_ST_BAD_ARG;
	if ((offset + len) > ota.image_size)
		return U41_ST_BAD_ARG;

	/*
	 * Strictly ascending, no gaps.  Allowing random access would mean
	 * tracking which half pages have been written, and there is no reason
	 * for the host to need it.  A repeat of the previous offset is
	 * accepted as an idempotent retry.
	 */
	if (offset != ota.next_offset) {
		if (offset + U41_FLASH_HALFPAGE_SIZE == ota.next_offset)
			return U41_ST_OK;   /* duplicate frame, already written */
		return U41_ST_BAD_ARG;
	}

	if (!u41_pm_flash_window_open())
		return U41_ST_BUSY;

	/*
	 * Short final chunk is zero padded.  Erased flash reads as 0x00 on
	 * this part, so padding with zeros writes nothing and leaves the tail
	 * of the image in its erased state - which is exactly what the CRC
	 * over image_size bytes expects, because mkimage pads the same way.
	 */
	memset(chunk, 0, sizeof(chunk));
	memcpy(chunk, data, len);

	addr = u41_slot_base(ota.target) + offset;

	if (len == U41_FLASH_HALFPAGE_SIZE) {
		rc = u41_flash_program_halfpage(addr, chunk);
	} else {
		uint32_t words = (len + 3UL) / 4UL;

		rc = u41_flash_program_words(addr, chunk, words);
	}

	if (rc != U41_FLASH_OK) {
		ota.last_error = U41_ST_FLASH_ERR;
		ota.phase      = U41_OTA_ERROR;
		return U41_ST_FLASH_ERR;
	}

	ota.bytes_written += len;
	ota.next_offset    = offset + U41_FLASH_HALFPAGE_SIZE;

	return U41_ST_OK;
}

uint8_t u41_ota_end(void)
{
	uint32_t base = u41_slot_base(ota.target);
	const u41_image_hdr_t *hdr;
	uint32_t crc, zero = 0UL;
	uint32_t before, after;

	if (ota.phase == U41_OTA_ERASING)
		return U41_ST_BUSY;
	if (ota.phase != U41_OTA_RECEIVING)
		return U41_ST_BAD_STATE;

	if (ota.bytes_written < ota.image_size)
		return U41_ST_BAD_STATE;

	ota.phase = U41_OTA_VERIFYING;

	hdr = (const u41_image_hdr_t *)(base + U41_IMAGE_HDR_OFFSET);

	/*
	 * Verify what is actually in flash, not what we think we sent.  This
	 * is the check that catches a half-written page from a brownout, a bit
	 * flip, or an image built for the wrong slot.
	 */
	if (hdr->magic != U41_IMAGE_MAGIC ||
	    hdr->hdr_version != U41_IMAGE_HDR_VERSION ||
	    hdr->slot != ota.target ||
	    hdr->load_addr != base ||
	    hdr->image_size != ota.image_size) {
		ota.last_error = U41_ST_VERIFY_FAIL;
		ota.phase      = U41_OTA_ERROR;
		return U41_ST_VERIFY_FAIL;
	}

	before = U41_IMAGE_CRC_FIELD_OFFSET;
	after  = ota.image_size - before - 4UL;

	crc = u41_crc32_update(0xFFFFFFFFUL, (const void *)base, before);
	crc = u41_crc32_update(crc, &zero, 4UL);
	crc = u41_crc32_update(crc, (const void *)(base + before + 4UL), after);
	crc = u41_crc32_final(crc);

	if (crc != hdr->image_crc32 || crc != ota.want_crc) {
		ota.last_error = U41_ST_VERIFY_FAIL;
		ota.phase      = U41_OTA_ERROR;
		return U41_ST_VERIFY_FAIL;
	}

	u41_flash_lock();
	ota.unlocked = 0;

	/*
	 * Arm the swap.  Note what this does NOT do: it does not change
	 * active_slot.  We keep running from the current image; the bootloader
	 * makes the switch on the next power cycle, and only if the image
	 * still validates then.
	 */
	if (ota.target == U41_SLOT_A)
		ota.meta.crc_a = crc;
	else
		ota.meta.crc_b = crc;

	ota.meta.pending_slot = ota.target;
	ota.meta.state        = U41_BOOT_PENDING;

	if (u41_meta_store(&ota.meta) != 0) {
		ota.last_error = U41_ST_FLASH_ERR;
		ota.phase      = U41_OTA_ERROR;
		return U41_ST_FLASH_ERR;
	}

	ota.phase = U41_OTA_PENDING;

	return U41_ST_OK;
}

uint8_t u41_ota_abort(void)
{
	if (ota.phase == U41_OTA_PENDING) {
		/*
		 * Withdraw a staged image.  The slot keeps its contents but
		 * the bootloader will no longer look at it.
		 */
		ota.meta.pending_slot = U41_SLOT_NONE;
		ota.meta.state        = U41_BOOT_IDLE;
		(void)u41_meta_store(&ota.meta);
	}

	ota_reset();
	ota.last_error = U41_ST_OK;

	return U41_ST_OK;
}

void u41_ota_status(u41_ota_status_t *out)
{
	memset(out, 0, sizeof(*out));

	out->phase         = ota.phase;
	out->target_slot   = ota.target;
	out->last_error    = ota.last_error;
	out->image_size    = ota.image_size;
	out->bytes_written = ota.bytes_written;
	out->pages_erased  = ota.pages_erased;
	out->pages_total   = ota.pages_total;
}

uint8_t u41_ota_confirm(void)
{
	if (ota.meta.state != U41_BOOT_TRIAL)
		return U41_ST_OK;   /* nothing on probation; not an error */

	ota.meta.state         = U41_BOOT_IDLE;
	ota.meta.boot_attempts = 0;

	if (u41_meta_store(&ota.meta) != 0)
		return U41_ST_FLASH_ERR;

	return U41_ST_OK;
}
