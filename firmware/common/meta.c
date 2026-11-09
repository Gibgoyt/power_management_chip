/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * meta.c - A/B boot control block in data EEPROM.
 *
 * Two records live at U41_EEPROM_BASE + 0x00 and + 0x20.  A write always goes
 * to the record with the LOWER seq, so the record with the higher seq is
 * never the one being modified.  Whatever happens to the power rail during a
 * write, at least one intact record survives, and the reader picks the
 * highest-seq record that passes CRC.
 *
 * A record is 32 bytes = 8 words.  Each word is a separate EEPROM write of
 * roughly 3.9 ms, so a full record update takes about 30 ms worst case.  That
 * only happens on a boot that changes state, never in the steady state.
 */

#include <string.h>

#include "u41_meta.h"
#include "u41_flash.h"
#include "u41_proto.h"

static uint32_t meta_addr(unsigned index)
{
	return U41_EEPROM_BASE +
	       (index == 0 ? U41_META_SLOT_0_OFFSET : U41_META_SLOT_1_OFFSET);
}

static int meta_valid(const u41_bootmeta_t *m)
{
	if (m->magic != U41_META_MAGIC)
		return 0;
	if (m->version != U41_META_VERSION)
		return 0;
	if (m->crc32 != u41_crc32(m, U41_META_CRC_LEN))
		return 0;
	if (m->active_slot != U41_SLOT_A && m->active_slot != U41_SLOT_B)
		return 0;
	if (m->pending_slot != U41_SLOT_A &&
	    m->pending_slot != U41_SLOT_B &&
	    m->pending_slot != U41_SLOT_NONE)
		return 0;
	if (m->state > U41_BOOT_TRIAL)
		return 0;

	return 1;
}

void u41_meta_defaults(u41_bootmeta_t *meta)
{
	memset(meta, 0, sizeof(*meta));

	meta->magic        = U41_META_MAGIC;
	meta->version      = U41_META_VERSION;
	meta->seq          = 0;
	/*
	 * flash_seq starts at zero.  A part that has only ever been programmed
	 * over J12 reports 0, and every adopted OTA image bumps it, so the
	 * value doubles as "how many OTA cycles has this board completed".
	 */
	meta->flash_seq    = 0;
	meta->active_slot  = U41_SLOT_A;
	meta->pending_slot = U41_SLOT_NONE;
	meta->state        = U41_BOOT_IDLE;
	meta->crc32        = u41_crc32(meta, U41_META_CRC_LEN);
}

int u41_meta_load(u41_bootmeta_t *out)
{
	u41_bootmeta_t rec[2];
	int ok[2];
	unsigned i;

	for (i = 0; i < 2; i++) {
		memcpy(&rec[i], (const void *)meta_addr(i), sizeof(rec[i]));
		ok[i] = meta_valid(&rec[i]);
	}

	if (ok[0] && ok[1]) {
		/*
		 * Both intact.  Higher seq wins.  seq is a 32-bit counter that
		 * only advances on a state change, so it cannot realistically
		 * wrap in the life of the board.
		 */
		*out = (rec[0].seq >= rec[1].seq) ? rec[0] : rec[1];
		return 1;
	}

	if (ok[0]) {
		*out = rec[0];
		return 1;
	}

	if (ok[1]) {
		*out = rec[1];
		return 1;
	}

	u41_meta_defaults(out);
	return 0;
}

int u41_meta_store(u41_bootmeta_t *meta)
{
	u41_bootmeta_t rec[2];
	int ok[2];
	unsigned target;
	unsigned i;
	int rc;

	for (i = 0; i < 2; i++) {
		memcpy(&rec[i], (const void *)meta_addr(i), sizeof(rec[i]));
		ok[i] = meta_valid(&rec[i]);
	}

	/* Overwrite whichever record we can afford to lose. */
	if (!ok[0])
		target = 0;
	else if (!ok[1])
		target = 1;
	else
		target = (rec[0].seq <= rec[1].seq) ? 0u : 1u;

	/* Make the new record strictly newer than both. */
	meta->seq = 1u;
	for (i = 0; i < 2; i++) {
		if (ok[i] && rec[i].seq >= meta->seq)
			meta->seq = rec[i].seq + 1u;
	}

	meta->magic   = U41_META_MAGIC;
	meta->version = U41_META_VERSION;
	meta->crc32   = u41_crc32(meta, U41_META_CRC_LEN);

	rc = u41_flash_unlock();
	if (rc != U41_FLASH_OK)
		return rc;

	rc = u41_eeprom_write(meta_addr(target), meta, sizeof(*meta));

	u41_flash_lock();

	return rc;
}
