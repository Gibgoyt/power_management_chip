/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * u41_flash.h - NVM driver for the STM32L051 (category 3, single bank).
 *
 * Everything in here that touches the flash controller executes from SRAM.
 * RM0377 3.3.1:
 *
 *   "When the NVM features only one bank, it is not possible to read the NVM
 *    during a write/erase operation.  If a write/erase operation is ongoing,
 *    the reading will be in a wait state until the write/erase operation
 *    completes, stalling the master that requested the read operation."
 *
 * Instruction fetch is a read.  If the erase loop lived in flash the core
 * would stall on its own next instruction, so the routines below are placed
 * in the .ramfunc section and copied to SRAM by the startup code.
 *
 * Interrupts are disabled for the duration of each operation, for the same
 * reason: an ISR vectoring into flash mid-erase would stall too.  Each call
 * blocks for roughly 3.9 ms, so callers must keep them one page at a time and
 * let the main loop run in between.
 */

#ifndef U41_FLASH_H
#define U41_FLASH_H

#include <stdint.h>
#include "u41_layout.h"

/* FLASH_PEKEYR / FLASH_PRGKEYR unlock sequences (RM0377 3.3.3). */
#define U41_FLASH_PEKEY1        0x89ABCDEFUL
#define U41_FLASH_PEKEY2        0x02030405UL
#define U41_FLASH_PRGKEY1       0x8C9DAEBFUL
#define U41_FLASH_PRGKEY2       0x13141516UL

enum {
	U41_FLASH_OK        =  0,
	U41_FLASH_E_LOCKED  = -1,
	U41_FLASH_E_ALIGN   = -2,
	U41_FLASH_E_RANGE   = -3,
	U41_FLASH_E_HW      = -4,  /* WRPERR / PGAERR / SIZERR / NOTZEROERR */
	U41_FLASH_E_VERIFY  = -5,
};

/* Unlock PECR and the program-memory latch.  Returns U41_FLASH_OK or negative. */
int u41_flash_unlock(void);

/* Re-arm both locks.  Always call this once an update is finished. */
void u41_flash_lock(void);

/*
 * Erase one 128-byte page.  addr may be any address inside the page but must
 * be word aligned.  Blocks ~3.9 ms with interrupts off.
 */
int u41_flash_erase_page(uint32_t addr);

/*
 * Program one 64-byte half page.  addr must be 64-byte aligned and the target
 * half page must already be erased.  data must be 4-byte aligned and 64 bytes
 * long.  Blocks ~3.9 ms with interrupts off.
 */
int u41_flash_program_halfpage(uint32_t addr, const uint32_t *data);

/*
 * Program a run of words one at a time.  Slower than the half-page path but
 * usable for a partial trailing chunk.  len_words words are written starting
 * at addr, which must be word aligned.
 */
int u41_flash_program_words(uint32_t addr, const uint32_t *data, uint32_t len_words);

/* True if every byte in [addr, addr+len) reads as 0x00. */
int u41_flash_is_erased(uint32_t addr, uint32_t len);

/*
 * Write one 32-bit word of data EEPROM.  The controller erases automatically
 * when needed, so this is a single short operation rather than an
 * erase/program pair.  addr must lie inside the EEPROM and be word aligned.
 */
int u41_eeprom_write_word(uint32_t addr, uint32_t value);

/* Write len bytes to the EEPROM.  addr and len must both be word aligned. */
int u41_eeprom_write(uint32_t addr, const void *data, uint32_t len);

#endif /* U41_FLASH_H */
