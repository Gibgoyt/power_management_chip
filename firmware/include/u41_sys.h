/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * u41_sys.h - clock, tick and watchdog helpers shared by both images.
 */

#ifndef U41_SYS_H
#define U41_SYS_H

#include <stdint.h>

/*
 * The board has no external crystal wired to U41: PC14/PC15 (pins 2 and 3)
 * are unconnected on the pwr_mgmt sheet.  So we run from the 16 MHz HSI,
 * which the datasheet trims to +/-1% over the industrial range.  That is
 * ample for a 1 MHz SPI slave and for second-scale power sequencing.
 */
#define U41_SYSCLK_HZ   16000000UL

/* Switch to HSI16 and set one flash wait state. */
void u41_sys_clock_init(void);

/* 1 kHz SysTick. */
void u41_tick_init(void);
uint32_t u41_millis(void);
void u41_delay_ms(uint32_t ms);

/*
 * Independent watchdog.  Clocked from the LSI, so it survives anything that
 * happens to the main clock tree.  Started with a period long enough to
 * tolerate the longest flash stall the app can hit while still catching a
 * genuinely wedged power manager.
 */
void u41_iwdg_start(uint32_t period_ms);
void u41_iwdg_feed(void);

/* Raw RCC_CSR sampled before the reset flags were cleared. */
uint32_t u41_reset_cause(void);

#endif /* U41_SYS_H */
