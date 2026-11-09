/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * app/main.c - the U41 application: power manager plus OTA target.
 *
 * Built twice from identical source, once linked for slot A and once for slot
 * B.  U41_BUILD_SLOT comes from the Makefile and is the only difference
 * between the two binaries.
 *
 * The main loop is deliberately flat.  Power sequencing is the job that must
 * not be starved, so nothing here blocks: the SPI transport is polled, the
 * OTA layer does at most one page erase per pass, and the state machine gets
 * a look-in every iteration.
 */

#include <string.h>

#include "stm32l0xx.h"
#include "u41_board.h"
#include "u41_layout.h"
#include "u41_meta.h"
#include "u41_ota.h"
#include "u41_pm.h"
#include "u41_proto.h"
#include "u41_spi.h"
#include "u41_sys.h"

#ifndef U41_BUILD_SLOT
#error "U41_BUILD_SLOT must be defined as 0 (slot A) or 1 (slot B)"
#endif

#ifndef U41_FW_VERSION
#define U41_FW_VERSION  U41_VERSION(1, 0, 0)
#endif

#ifndef U41_BUILD_ID
#define U41_BUILD_ID    0UL
#endif

/*
 * The image header the bootloader looks for.
 *
 * image_size and image_crc32 are left at zero here and stamped into the
 * binary afterwards by tools/mkimage.py - the CRC covers the header itself,
 * so it cannot be known at compile time.  Everything else is fixed by the
 * build and is what lets the bootloader reject an image written into the
 * wrong slot.
 */
__attribute__((section(".image_header"), used))
const u41_image_hdr_t g_image_hdr = {
	.magic       = U41_IMAGE_MAGIC,
	.hdr_version = U41_IMAGE_HDR_VERSION,
	.slot        = U41_BUILD_SLOT,
	.load_addr   = (U41_BUILD_SLOT == U41_SLOT_B) ? U41_SLOT_B_BASE
						      : U41_SLOT_A_BASE,
	.image_size  = 0,
	.image_crc32 = 0,
	.fw_version  = U41_FW_VERSION,
	.build_id    = U41_BUILD_ID,
	.reserved    = 0,
};

#define APP_VERSION  U41_VERSION(1, 0, 0)

static uint8_t rx[U41_FRAME_SIZE];

static uint32_t get_le32(const uint8_t *p)
{
	return (uint32_t)p[0]
	     | ((uint32_t)p[1] << 8)
	     | ((uint32_t)p[2] << 16)
	     | ((uint32_t)p[3] << 24);
}

static void handle_get_info(uint8_t seq)
{
	const u41_bootmeta_t *meta = u41_ota_meta();
	u41_info_t info;

	memset(&info, 0, sizeof(info));

	info.magic         = U41_INFO_MAGIC;
	info.proto_version = U41_PROTO_VERSION;
	info.active_slot   = U41_BUILD_SLOT;
	info.pending_slot  = meta->pending_slot;
	info.boot_state    = meta->state;
	/*
	 * The counter the acceptance test watches.  0 straight after a J12
	 * flash, then 1, 2, 3 ... one per power cycle that adopted a new
	 * image.  It is read from EEPROM, so it survives everything short of
	 * a chip erase.
	 */
	info.flash_seq     = meta->flash_seq;
	info.fw_version    = g_image_hdr.fw_version;
	info.fw_crc32      = g_image_hdr.image_crc32;
	info.boot_version  = APP_VERSION;
	info.uptime_ms     = u41_millis();
	info.reset_cause   = u41_reset_cause();
	info.boot_attempts = meta->boot_attempts;

	u41_spi_reply(U41_CMD_GET_INFO, seq, U41_ST_OK, &info, sizeof(info));
}

static void handle_frame(const uint8_t *f)
{
	uint8_t  cmd = f[U41_OFF_CMD];
	uint8_t  seq = f[U41_OFF_SEQ];
	uint8_t  len = f[U41_OFF_LEN];
	uint32_t arg = get_le32(&f[U41_OFF_ARG32]);
	const uint8_t *payload = &f[U41_OFF_PAYLOAD];

	switch (cmd) {
	case U41_CMD_PING:
		/*
		 * PING is how the host clocks out the answer to the previous
		 * command, so replying to it must not clobber a reply that is
		 * still waiting to be read.  Only answer if nothing else is
		 * staged - u41_spi_reply() overwrites unconditionally, so the
		 * ordering here is what makes the poll loop work.
		 */
		u41_spi_reply_status(U41_CMD_PING, seq, U41_ST_OK);
		break;

	case U41_CMD_GET_INFO:
		handle_get_info(seq);
		break;

	case U41_CMD_GET_PM_STATE: {
		u41_pm_status_t st;

		u41_pm_status(&st);
		u41_spi_reply(cmd, seq, U41_ST_OK, &st, sizeof(st));
		break;
	}

	case U41_CMD_OTA_BEGIN: {
		uint32_t crc, ver;
		uint8_t rc;

		if (len < 8u) {
			u41_spi_reply_status(cmd, seq, U41_ST_BAD_LEN);
			break;
		}

		crc = get_le32(&payload[0]);
		ver = get_le32(&payload[4]);
		rc  = u41_ota_begin(arg, crc, ver);

		u41_spi_reply_status(cmd, seq, rc);
		break;
	}

	case U41_CMD_OTA_DATA: {
		uint8_t rc = u41_ota_data(arg, payload, len);

		u41_spi_reply_status(cmd, seq, rc);
		break;
	}

	case U41_CMD_OTA_END:
		u41_spi_reply_status(cmd, seq, u41_ota_end());
		break;

	case U41_CMD_OTA_ABORT:
		u41_spi_reply_status(cmd, seq, u41_ota_abort());
		break;

	case U41_CMD_OTA_STATUS: {
		u41_ota_status_t st;

		u41_ota_status(&st);
		u41_spi_reply(cmd, seq, U41_ST_OK, &st, sizeof(st));
		break;
	}

	case U41_CMD_CONFIRM:
		u41_spi_reply_status(cmd, seq, u41_ota_confirm());
		break;

	case U41_CMD_SET_RAIL: {
		int on = (len >= 1u) && (payload[0] != 0u);

		u41_spi_reply_status(cmd, seq, u41_pm_set_rail(arg, on));
		break;
	}

	case U41_CMD_KEEPALIVE:
		u41_pm_keepalive();
		u41_spi_reply_status(cmd, seq, U41_ST_OK);
		break;

	default:
		u41_spi_reply_status(cmd, seq, U41_ST_BAD_CMD);
		break;
	}
}

int main(void)
{
	u41_bootmeta_t meta;

	u41_sys_clock_init();
	u41_tick_init();

	u41_pm_init();

	(void)u41_meta_load(&meta);
	u41_ota_init(&meta);

	u41_spi_init();

	/*
	 * 8 s.  Long enough that a page erase (~4 ms) or even a run of them
	 * cannot trip it, short enough that a genuinely wedged power manager
	 * gets restarted before anyone notices.  A reset lands back in the
	 * bootloader, which re-validates the slot - so a watchdog bite on a
	 * bad TRIAL image is also what drives the rollback.
	 */
	u41_iwdg_start(8000);

	for (;;) {
		u41_iwdg_feed();

		u41_pm_poll();

		if (u41_spi_poll(rx))
			handle_frame(rx);

		u41_ota_poll();
	}
}
