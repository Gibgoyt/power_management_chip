/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * u41_meta.h - A/B boot control block, stored in the STM32L051 data EEPROM.
 *
 * Why EEPROM and not flash: the data EEPROM is word-writable with an implicit
 * erase (RM0377 3.3.4), so updating the boot record is one short operation and
 * does not require erasing a 128-byte flash page that also holds code.  Two
 * redundant records are kept and the reader picks the newest one that passes
 * CRC, so a power loss during a metadata write can never leave the device
 * without a usable record.
 *
 * flash_seq is the field the OTA test procedure watches.  It starts at 0 after
 * an initial J12/SWD flash and increments by one on every boot that
 * successfully ADOPTS a new image.  It deliberately does not increment when
 * the image is merely written - only when a power cycle has actually handed
 * control to the new slot.  That makes it a direct proof that the power cycle
 * did what it was supposed to.
 */

#ifndef U41_META_H
#define U41_META_H

#include <stdint.h>
#include "u41_layout.h"

#define U41_META_MAGIC        0x4D313455UL   /* 'U41M' little endian */
#define U41_META_VERSION      1u

/* Boot states. */
enum {
	/* active_slot is confirmed good, nothing in flight. */
	U41_BOOT_IDLE    = 0,
	/* pending_slot holds a fully written, CRC-verified image.  It is
	 * adopted on the next power cycle; nothing has changed yet. */
	U41_BOOT_PENDING = 1,
	/* The bootloader has adopted a new image and is giving it a limited
	 * number of boots to call U41_CMD_CONFIRM.  If it does not, the
	 * bootloader rolls back to the previous slot. */
	U41_BOOT_TRIAL   = 2,
};

/* How many boots a TRIAL image gets before the bootloader rolls it back. */
#define U41_MAX_TRIAL_BOOTS   3u

typedef struct __attribute__((packed)) {
	uint32_t magic;         /* U41_META_MAGIC                            */
	uint32_t seq;           /* record sequence; newest valid record wins */
	uint32_t flash_seq;     /* adopted-image counter, starts at 0        */
	uint32_t crc_a;         /* last verified CRC32 of slot A             */
	uint32_t crc_b;         /* last verified CRC32 of slot B             */
	uint16_t version;       /* U41_META_VERSION                          */
	uint8_t  active_slot;   /* U41_SLOT_A / U41_SLOT_B                   */
	uint8_t  pending_slot;  /* U41_SLOT_* or U41_SLOT_NONE               */
	uint8_t  state;         /* U41_BOOT_*                                */
	uint8_t  boot_attempts; /* boots taken by the current TRIAL image    */
	uint16_t reserved;
	uint32_t crc32;         /* CRC32 over the preceding 28 bytes         */
} u41_bootmeta_t;

_Static_assert(sizeof(u41_bootmeta_t) == U41_META_RECORD_SIZE,
	       "boot metadata record must be 32 bytes");

/* Bytes covered by u41_bootmeta_t::crc32. */
#define U41_META_CRC_LEN  (U41_META_RECORD_SIZE - 4UL)

/*
 * Read the newest valid record into *out.
 *
 * Returns 1 if a valid record was found, 0 if neither copy is usable (blank
 * EEPROM on a virgin part, or both records corrupt).  On a 0 return *out is
 * filled with the power-on defaults: slot A active, flash_seq 0, IDLE.
 */
int u41_meta_load(u41_bootmeta_t *out);

/*
 * Write *meta to the older of the two records, bumping seq first so that the
 * new record becomes the newest.  The other record is left untouched, which is
 * what makes this power-fail safe.
 *
 * Returns 0 on success, negative on EEPROM error.
 */
int u41_meta_store(u41_bootmeta_t *meta);

/* Fill *meta with the virgin-part defaults. */
void u41_meta_defaults(u41_bootmeta_t *meta);

#endif /* U41_META_H */
