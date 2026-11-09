/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * bootloader/main.c - A/B slot selection for U41.
 *
 *
 * WHY THIS EXISTS
 * ---------------
 * The brief described "a hardware process that points our B program to
 * 0x08000".  On this part there is no such process.  RM0377 3.3.2 titles the
 * feature "Bank swapping (category 5 devices only)", and the STM32L051K6 is a
 * 32 Kbyte category 3 device: single bank, 256 pages of 128 bytes, no BFB2
 * option bit, no dual-bank boot.  The core always starts at 0x08000000.
 *
 * So the swap is done here, in software.  This code owns 0x08000000, runs for
 * a few milliseconds at every power-up, decides which of the two application
 * slots to run, and jumps to it.  From the application's point of view the
 * effect is exactly what was asked for: write the inactive slot while the
 * active one keeps the board alive, cycle power, come up on the new image.
 *
 *
 * WHAT IT DOES, IN ORDER
 * ----------------------
 *   1. Put the rails in a known-off state.  At reset every GPIO is an input,
 *      so the enables would otherwise float until the application claims
 *      them.
 *   2. Load the boot metadata from EEPROM.
 *   3. If a PENDING image is waiting, validate it and adopt it.  This is the
 *      step that increments flash_seq, which is why flash_seq only advances
 *      across a real power cycle.
 *   4. If a TRIAL image has used up its attempts without confirming, roll
 *      back to the other slot.
 *   5. Validate the chosen slot; fall back to the other one if it is bad.
 *   6. Jump.
 *
 *
 * WHAT IT DELIBERATELY DOES NOT DO
 * --------------------------------
 * No SPI, no OTA, no communication of any kind.  Keeping the bootloader
 * inert makes it small and makes it something we never have to update in the
 * field.  Recovery from "both slots bad" is J12 and an ST-Link, which is the
 * one path that cannot itself be broken by a bad update.
 */

#include <string.h>

#include "stm32l0xx.h"
#include "u41_board.h"
#include "u41_flash.h"
#include "u41_layout.h"
#include "u41_meta.h"
#include "u41_proto.h"
#include "u41_sys.h"

#define BOOT_VERSION  U41_VERSION(1, 0, 0)

/*
 * Drive every rail enable low and make them outputs.
 *
 * This runs on every reset, including a watchdog reset while the SoM is up.
 * That is intentional: if the power manager has lost its mind we want the
 * rails down and a clean cold start of everything, not a half-controlled
 * board.  It does mean an unexpected U41 reset yanks power from the SoM.
 * There is no way to avoid that on this hardware - U41 is the only thing
 * holding SOM_PSU_EN - so it is called out here rather than hidden.
 */
static void rails_safe_state(void)
{
	RCC->IOPENR |= RCC_IOPENR_GPIOAEN | RCC_IOPENR_GPIOBEN;

	U41_GPIO_CLR(U41_SOM_PSU_EN_PORT,  U41_SOM_PSU_EN_PIN);
	U41_GPIO_CLR(U41_WIFI_PSU_EN_PORT, U41_WIFI_PSU_EN_PIN);
	U41_GPIO_CLR(U41_LTE_PSU_EN_PORT,  U41_LTE_PSU_EN_PIN);
	U41_GPIO_CLR(U41_SCAP_EN_PORT,     U41_SCAP_EN_PIN);
	U41_GPIO_CLR(U41_PSE_PWR_EN_PORT,  U41_PSE_PWR_EN_PIN);

	u41_gpio_mode(U41_SOM_PSU_EN_PORT,  U41_SOM_PSU_EN_PIN,  U41_MODE_OUTPUT);
	u41_gpio_mode(U41_WIFI_PSU_EN_PORT, U41_WIFI_PSU_EN_PIN, U41_MODE_OUTPUT);
	u41_gpio_mode(U41_LTE_PSU_EN_PORT,  U41_LTE_PSU_EN_PIN,  U41_MODE_OUTPUT);
	u41_gpio_mode(U41_SCAP_EN_PORT,     U41_SCAP_EN_PIN,     U41_MODE_OUTPUT);
	u41_gpio_mode(U41_PSE_PWR_EN_PORT,  U41_PSE_PWR_EN_PIN,  U41_MODE_OUTPUT);
}

/*
 * Validate the image in a slot.
 *
 * Checks, in order of how cheap they are:
 *   - the header magic and version,
 *   - that the image was linked for this slot (load_addr), which is what
 *     catches an A image accidentally written into B,
 *   - a plausible size,
 *   - the initial stack pointer points into SRAM,
 *   - the reset vector points into this slot,
 *   - CRC32 over the whole image with the CRC field itself read as zero.
 *
 * Returns 1 and stores the image CRC in *crc_out on success.
 */
static int slot_is_valid(uint8_t slot, uint32_t *crc_out)
{
	uint32_t base = u41_slot_base(slot);
	const u41_image_hdr_t *hdr =
		(const u41_image_hdr_t *)(base + U41_IMAGE_HDR_OFFSET);
	uint32_t sp, pc, crc, zero = 0UL;
	uint32_t before, after;

	if (hdr->magic != U41_IMAGE_MAGIC)
		return 0;
	if (hdr->hdr_version != U41_IMAGE_HDR_VERSION)
		return 0;
	if (hdr->slot != slot)
		return 0;
	if (hdr->load_addr != base)
		return 0;
	if (hdr->image_size < (U41_IMAGE_HDR_OFFSET + sizeof(*hdr)))
		return 0;
	if (hdr->image_size > U41_SLOT_SIZE)
		return 0;

	sp = *(const uint32_t *)base;
	pc = *(const uint32_t *)(base + 4UL);

	if (sp < SRAM_BASE || sp > (SRAM_BASE + (8UL * 1024UL)))
		return 0;
	if (pc < base || pc >= (base + hdr->image_size))
		return 0;

	/*
	 * CRC the image in three pieces, treating the 4 bytes of
	 * image_crc32 as zero.  mkimage.py computes it the same way, so the
	 * checksum can be stamped into the image it covers.
	 */
	before = U41_IMAGE_CRC_FIELD_OFFSET;
	after  = hdr->image_size - before - 4UL;

	crc = u41_crc32_update(0xFFFFFFFFUL, (const void *)base, before);
	crc = u41_crc32_update(crc, &zero, 4UL);
	crc = u41_crc32_update(crc, (const void *)(base + before + 4UL), after);
	crc = u41_crc32_final(crc);

	if (crc != hdr->image_crc32)
		return 0;

	if (crc_out)
		*crc_out = crc;

	return 1;
}

/* Hand control to a slot.  Does not return. */
__attribute__((noreturn))
static void jump_to_slot(uint8_t slot)
{
	uint32_t base = u41_slot_base(slot);
	uint32_t sp   = *(const uint32_t *)base;
	uint32_t pc   = *(const uint32_t *)(base + 4UL);
	void (*entry)(void) = (void (*)(void))pc;

	/*
	 * Undo everything we turned on, so the application starts from the
	 * same state it would see after a bare reset.  SysTick in particular
	 * must be off or its interrupt fires against the application's
	 * not-yet-installed vector table.
	 */
	SysTick->CTRL = 0;
	SysTick->LOAD = 0;
	SysTick->VAL  = 0;

	__disable_irq();
	NVIC->ICER[0] = 0xFFFFFFFFUL;
	NVIC->ICPR[0] = 0xFFFFFFFFUL;

	SCB->VTOR = base;
	__DSB();
	__ISB();

	__set_MSP(sp);
	__enable_irq();

	entry();

	for (;;)
		;
}

/*
 * Nothing to run.  Both slots failed validation, which after an A/B update
 * should be impossible - it means either a virgin part that has only had the
 * bootloader flashed, or physical flash damage.
 *
 * We bring up SOM_PSU_EN anyway so the rest of the board still powers on and
 * an operator can see the unit is alive, then sit in a loop feeding the
 * watchdog.  Recovery is J12 + ST-Link; see tools/flash-stlink.sh.
 */
__attribute__((noreturn))
static void recovery_hold(void)
{
	U41_GPIO_SET(U41_SOM_PSU_EN_PORT, U41_SOM_PSU_EN_PIN);

	for (;;) {
		u41_iwdg_feed();
		__WFI();
	}
}

int main(void)
{
	u41_bootmeta_t meta;
	uint8_t chosen;
	uint32_t crc = 0;
	int dirty = 0;

	u41_sys_clock_init();
	rails_safe_state();

	/*
	 * 28 s, the longest the IWDG can be set to here.  The bootloader
	 * itself finishes in single-digit milliseconds; this is armed early so
	 * that a hang anywhere below still recovers, and the application
	 * re-arms it with a tighter period once it is running.
	 */
	u41_iwdg_start(28000);

	(void)u41_meta_load(&meta);

	/* ---- adopt a pending image ---------------------------------- */
	if (meta.state == U41_BOOT_PENDING &&
	    meta.pending_slot != U41_SLOT_NONE) {
		if (slot_is_valid(meta.pending_slot, &crc)) {
			if (meta.pending_slot == U41_SLOT_A)
				meta.crc_a = crc;
			else
				meta.crc_b = crc;

			meta.active_slot   = meta.pending_slot;
			meta.pending_slot  = U41_SLOT_NONE;
			meta.state         = U41_BOOT_TRIAL;
			meta.boot_attempts = 1;

			/*
			 * The counter the OTA test watches.  It moves here and
			 * nowhere else, so seeing it change proves the write
			 * completed AND the power cycle took effect.
			 */
			meta.flash_seq++;
		} else {
			/* Written image did not survive; stay where we are. */
			meta.pending_slot = U41_SLOT_NONE;
			meta.state        = U41_BOOT_IDLE;
		}
		dirty = 1;
	}

	/* ---- roll back an image that never confirmed ----------------- */
	else if (meta.state == U41_BOOT_TRIAL) {
		if (meta.boot_attempts >= U41_MAX_TRIAL_BOOTS) {
			uint8_t fallback = U41_SLOT_OTHER(meta.active_slot);

			if (slot_is_valid(fallback, &crc)) {
				meta.active_slot = fallback;
				meta.state       = U41_BOOT_IDLE;
			} else {
				/*
				 * Nowhere to roll back to.  Keep trying the
				 * trial image: a slot that boots but never
				 * confirms is still better than no firmware.
				 */
				meta.state = U41_BOOT_IDLE;
			}
			meta.boot_attempts = 0;
		} else {
			meta.boot_attempts++;
		}
		dirty = 1;
	}

	/* ---- detect an image that arrived over SWD ------------------- */
	/*
	 * The brief says a J12 flash leaves flash_seq at 0 and OTA updates
	 * then count 1, 2, 3...  An ST-Link writes flash directly and has no
	 * way to tell us it did, so we work it out: every image adopted
	 * through the OTA path had its CRC recorded in the metadata at the
	 * same moment.  If the slot we are about to run does not match the
	 * CRC on file, it got there some other way - which on this board means
	 * SWD - and the counter goes back to zero.
	 *
	 * A virgin part lands here too: blank EEPROM gives crc_a = 0, which
	 * cannot match a real image, so a first-ever flash reports 0 as well.
	 */
	if (slot_is_valid(meta.active_slot, &crc)) {
		uint32_t recorded = (meta.active_slot == U41_SLOT_A)
				  ? meta.crc_a : meta.crc_b;

		if (crc != recorded) {
			if (meta.active_slot == U41_SLOT_A)
				meta.crc_a = crc;
			else
				meta.crc_b = crc;

			meta.flash_seq     = 0;
			meta.state         = U41_BOOT_IDLE;
			meta.pending_slot  = U41_SLOT_NONE;
			meta.boot_attempts = 0;
			dirty = 1;
		}
	}

	if (dirty)
		(void)u41_meta_store(&meta);

	/* ---- pick a slot and go -------------------------------------- */
	chosen = meta.active_slot;

	if (!slot_is_valid(chosen, NULL)) {
		uint8_t other = U41_SLOT_OTHER(chosen);

		if (!slot_is_valid(other, NULL))
			recovery_hold();

		chosen = other;

		/*
		 * Record the correction so the next boot does not have to
		 * rediscover it, and so the host can see which slot is really
		 * running.
		 */
		meta.active_slot   = chosen;
		meta.state         = U41_BOOT_IDLE;
		meta.boot_attempts = 0;
		(void)u41_meta_store(&meta);
	}

	jump_to_slot(chosen);
}
