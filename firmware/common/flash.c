/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * flash.c - STM32L051 NVM driver.  See u41_flash.h for why this lives in RAM.
 *
 * Register sequences follow RM0377 sections 3.3.3 (unlocking), 3.3.4 (data
 * EEPROM and program memory writes) and 3.3.5 (erasing).
 */

#include "u41_flash.h"
#include "stm32l0xx.h"

/*
 * Placed in SRAM by the linker script.  Everything the core executes between
 * "start the operation" and "BSY has cleared" must be in here.
 */
#define RAMFUNC __attribute__((section(".ramfunc"), noinline, long_call))

/* Errors the controller can raise on a program or erase. */
#define U41_FLASH_SR_ERRORS \
	(FLASH_SR_WRPERR | FLASH_SR_PGAERR | FLASH_SR_SIZERR | \
	 FLASH_SR_OPTVERR | FLASH_SR_FWWERR | FLASH_SR_NOTZEROERR)

/*
 * Spin until the controller reports idle.
 *
 * The bound is a loop count rather than a timer read: reading a timer is
 * fine (peripherals are not stalled) but keeping this self-contained means
 * the routine has no dependency on anything outside .ramfunc.  At 16 MHz a
 * 3.9 ms operation needs well under 100k iterations; 2,000,000 is a runaway
 * guard, not a timeout anybody should ever hit.
 */
RAMFUNC static int flash_wait_idle(void)
{
	uint32_t guard = 2000000UL;

	while ((FLASH->SR & FLASH_SR_BSY) != 0UL) {
		if (--guard == 0UL)
			return U41_FLASH_E_HW;
	}

	if ((FLASH->SR & U41_FLASH_SR_ERRORS) != 0UL) {
		FLASH->SR = U41_FLASH_SR_ERRORS;   /* w1c */
		return U41_FLASH_E_HW;
	}

	if ((FLASH->SR & FLASH_SR_EOP) != 0UL)
		FLASH->SR = FLASH_SR_EOP;

	return U41_FLASH_OK;
}

int u41_flash_unlock(void)
{
	if ((FLASH->PECR & FLASH_PECR_PELOCK) != 0UL) {
		FLASH->PEKEYR = U41_FLASH_PEKEY1;
		FLASH->PEKEYR = U41_FLASH_PEKEY2;

		if ((FLASH->PECR & FLASH_PECR_PELOCK) != 0UL)
			return U41_FLASH_E_LOCKED;
	}

	if ((FLASH->PECR & FLASH_PECR_PRGLOCK) != 0UL) {
		FLASH->PRGKEYR = U41_FLASH_PRGKEY1;
		FLASH->PRGKEYR = U41_FLASH_PRGKEY2;

		if ((FLASH->PECR & FLASH_PECR_PRGLOCK) != 0UL)
			return U41_FLASH_E_LOCKED;
	}

	return U41_FLASH_OK;
}

void u41_flash_lock(void)
{
	FLASH->PECR |= FLASH_PECR_PRGLOCK;
	FLASH->PECR |= FLASH_PECR_PELOCK;
}

/*
 * RM0377 3.3.5: set ERASE and PROG, then write 0x00000000 anywhere in the
 * page.  The write is what launches the erase.
 */
RAMFUNC static int flash_erase_page_ram(uint32_t addr)
{
	int rc;

	rc = flash_wait_idle();
	if (rc != U41_FLASH_OK)
		return rc;

	FLASH->PECR |= FLASH_PECR_ERASE | FLASH_PECR_PROG;

	*(volatile uint32_t *)addr = 0x00000000UL;

	rc = flash_wait_idle();

	FLASH->PECR &= ~(FLASH_PECR_ERASE | FLASH_PECR_PROG);

	return rc;
}

int u41_flash_erase_page(uint32_t addr)
{
	uint32_t primask;
	int rc;

	if ((addr & (U41_FLASH_WORD_SIZE - 1UL)) != 0UL)
		return U41_FLASH_E_ALIGN;
	if (addr < U41_FLASH_BASE || addr >= U41_FLASH_END)
		return U41_FLASH_E_RANGE;

	primask = __get_PRIMASK();
	__disable_irq();
	rc = flash_erase_page_ram(addr);
	__set_PRIMASK(primask);

	return rc;
}

/*
 * RM0377 3.3.4, half-page programming: set FPRG and PROG, then write exactly
 * 16 words back to back.  The controller latches them and burns the half page
 * when the 16th word lands.  The loop must not be interrupted, hence the
 * caller disabling interrupts and this function living in SRAM.
 */
RAMFUNC static int flash_program_halfpage_ram(uint32_t addr, const uint32_t *data)
{
	volatile uint32_t *dst = (volatile uint32_t *)addr;
	uint32_t i;
	int rc;

	rc = flash_wait_idle();
	if (rc != U41_FLASH_OK)
		return rc;

	FLASH->PECR |= FLASH_PECR_FPRG | FLASH_PECR_PROG;

	for (i = 0; i < (U41_FLASH_HALFPAGE_SIZE / U41_FLASH_WORD_SIZE); i++)
		dst[i] = data[i];

	rc = flash_wait_idle();

	FLASH->PECR &= ~(FLASH_PECR_FPRG | FLASH_PECR_PROG);

	return rc;
}

int u41_flash_program_halfpage(uint32_t addr, const uint32_t *data)
{
	uint32_t primask;
	int rc;

	if ((addr & (U41_FLASH_HALFPAGE_SIZE - 1UL)) != 0UL)
		return U41_FLASH_E_ALIGN;
	if (addr < U41_FLASH_BASE ||
	    (addr + U41_FLASH_HALFPAGE_SIZE) > U41_FLASH_END)
		return U41_FLASH_E_RANGE;
	if (((uint32_t)data & (U41_FLASH_WORD_SIZE - 1UL)) != 0UL)
		return U41_FLASH_E_ALIGN;

	primask = __get_PRIMASK();
	__disable_irq();
	rc = flash_program_halfpage_ram(addr, data);
	__set_PRIMASK(primask);

	return rc;
}

/*
 * Single-word program.  Used for the trailing partial half page of an image
 * and for EEPROM writes.  A word write to program memory needs PROG clear and
 * the target already erased; a word write to the EEPROM erases implicitly.
 */
RAMFUNC static int flash_program_word_ram(uint32_t addr, uint32_t value)
{
	int rc;

	rc = flash_wait_idle();
	if (rc != U41_FLASH_OK)
		return rc;

	*(volatile uint32_t *)addr = value;

	return flash_wait_idle();
}

int u41_flash_program_words(uint32_t addr, const uint32_t *data, uint32_t len_words)
{
	uint32_t primask;
	uint32_t i;
	int rc = U41_FLASH_OK;

	if ((addr & (U41_FLASH_WORD_SIZE - 1UL)) != 0UL)
		return U41_FLASH_E_ALIGN;
	if (addr < U41_FLASH_BASE ||
	    (addr + len_words * U41_FLASH_WORD_SIZE) > U41_FLASH_END)
		return U41_FLASH_E_RANGE;

	for (i = 0; i < len_words; i++) {
		/*
		 * Words that are already 0x00000000 are skipped.  An erased
		 * STM32L0 flash cell reads as zero, so writing zero over zero
		 * would raise NOTZEROERR for no benefit.
		 */
		if (data[i] == 0UL)
			continue;

		primask = __get_PRIMASK();
		__disable_irq();
		rc = flash_program_word_ram(addr + i * U41_FLASH_WORD_SIZE,
					    data[i]);
		__set_PRIMASK(primask);

		if (rc != U41_FLASH_OK)
			break;
	}

	return rc;
}

int u41_flash_is_erased(uint32_t addr, uint32_t len)
{
	const volatile uint8_t *p = (const volatile uint8_t *)addr;
	uint32_t i;

	for (i = 0; i < len; i++) {
		if (p[i] != 0x00u)
			return 0;
	}

	return 1;
}

int u41_eeprom_write_word(uint32_t addr, uint32_t value)
{
	uint32_t primask;
	int rc;

	if ((addr & (U41_FLASH_WORD_SIZE - 1UL)) != 0UL)
		return U41_FLASH_E_ALIGN;
	if (addr < U41_EEPROM_BASE ||
	    (addr + U41_FLASH_WORD_SIZE) > (U41_EEPROM_BASE + U41_EEPROM_SIZE))
		return U41_FLASH_E_RANGE;

	/* Already correct?  Skip the wear. */
	if (*(volatile uint32_t *)addr == value)
		return U41_FLASH_OK;

	primask = __get_PRIMASK();
	__disable_irq();
	rc = flash_program_word_ram(addr, value);
	__set_PRIMASK(primask);

	return rc;
}

int u41_eeprom_write(uint32_t addr, const void *data, uint32_t len)
{
	const uint32_t *w = (const uint32_t *)data;
	uint32_t i;
	int rc;

	if ((len & (U41_FLASH_WORD_SIZE - 1UL)) != 0UL)
		return U41_FLASH_E_ALIGN;

	for (i = 0; i < len / U41_FLASH_WORD_SIZE; i++) {
		rc = u41_eeprom_write_word(addr + i * U41_FLASH_WORD_SIZE, w[i]);
		if (rc != U41_FLASH_OK)
			return rc;
	}

	return U41_FLASH_OK;
}
