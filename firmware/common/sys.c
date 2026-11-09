/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * sys.c - clock, millisecond tick and independent watchdog.
 */

#include "u41_sys.h"
#include "stm32l0xx.h"

/*
 * Referenced by ST's system_stm32l0xx.h.  We do not link ST's
 * system_stm32l0xx.c (it drags in clock config we do differently), so the
 * definition lives here.
 */
uint32_t SystemCoreClock = U41_SYSCLK_HZ;

static volatile uint32_t s_millis;
static uint32_t s_reset_cause;

void u41_sys_clock_init(void)
{
	/*
	 * Latch the reset cause before anything clears it.  The host reads
	 * this over U41_CMD_GET_INFO, which makes "did the board actually
	 * power cycle, or did the watchdog bite?" answerable from Linux.
	 */
	s_reset_cause = RCC->CSR;
	RCC->CSR |= RCC_CSR_RMVF;

	/*
	 * One wait state is required above 8 MHz at this voltage range
	 * (RM0377 3.3.3, Table 6).  Set it before raising the clock, never
	 * after.
	 */
	FLASH->ACR |= FLASH_ACR_LATENCY;
	while ((FLASH->ACR & FLASH_ACR_LATENCY) == 0UL)
		;

	/* HSI16 on, no divider. */
	RCC->CR |= RCC_CR_HSION;
	while ((RCC->CR & RCC_CR_HSIRDY) == 0UL)
		;

	RCC->CFGR = (RCC->CFGR & ~RCC_CFGR_SW) | RCC_CFGR_SW_HSI;
	while ((RCC->CFGR & RCC_CFGR_SWS) != RCC_CFGR_SWS_HSI)
		;

	/* AHB and both APBs undivided: 16 MHz everywhere. */
	RCC->CFGR &= ~(RCC_CFGR_HPRE | RCC_CFGR_PPRE1 | RCC_CFGR_PPRE2);

	SystemCoreClock = U41_SYSCLK_HZ;
}

uint32_t u41_reset_cause(void)
{
	return s_reset_cause;
}

void u41_tick_init(void)
{
	s_millis = 0;
	SysTick_Config(U41_SYSCLK_HZ / 1000UL);
}

void SysTick_Handler(void)
{
	s_millis++;
}

uint32_t u41_millis(void)
{
	return s_millis;
}

void u41_delay_ms(uint32_t ms)
{
	uint32_t start = u41_millis();

	while ((u41_millis() - start) < ms)
		__NOP();
}

void u41_iwdg_start(uint32_t period_ms)
{
	/*
	 * LSI is nominally 37 kHz on this family (datasheet gives 26..56 kHz
	 * over process and temperature, which is why the period below is
	 * generous rather than tight).  With prescaler /256 one tick is about
	 * 6.9 ms, so the reload counts periods of ~6.9 ms up to the 12-bit
	 * maximum of 4095, i.e. about 28 s.
	 */
	uint32_t reload = (period_ms * 37UL) / 256UL;

	if (reload > 0x0FFFUL)
		reload = 0x0FFFUL;
	if (reload == 0UL)
		reload = 1UL;

	/*
	 * NEVER spin forever on a peripheral flag here.  On real RIO 4.04
	 * hardware the IWDG->SR (PVU/RVU) bits were observed to stay set
	 * (SR=0x3) indefinitely even with LSIRDY=1, which hung the bootloader
	 * at this function before it could ever power the SoM (all GPIO ODR
	 * stayed 0, board idled ~0.233 A).  Diagnosed 2026-09-13 over SWD; see
	 * docs/todos/bohemoth-001-get-u41-firmware-working.md.
	 *
	 * Both waits are therefore BOUNDED.  The values written to PR/RLR are
	 * latched regardless; starting the watchdog (0xCCCC) does not require
	 * SR to have cleared first, and a slightly early start only means the
	 * first watchdog window uses the reset-default prescaler for a few LSI
	 * cycles - harmless for a 28 s / 8 s period.  Correctness of the
	 * watchdog is not compromised; an un-cleared sync flag must not brick
	 * the board.
	 *
	 * The bound is a plain instruction-count budget (no SysTick dependency,
	 * since this runs before/independently of the tick).  ~1e6 iterations at
	 * 16 MHz is tens of ms, far longer than the few LSI cycles the sync
	 * legitimately needs.
	 */
	uint32_t guard;

	RCC->CSR |= RCC_CSR_LSION;
	for (guard = 0; guard < 1000000UL; guard++) {
		if ((RCC->CSR & RCC_CSR_LSIRDY) != 0UL)
			break;
	}

	IWDG->KR  = 0x5555U;              /* enable register access  */
	IWDG->PR  = IWDG_PR_PR_0 | IWDG_PR_PR_1 | IWDG_PR_PR_2;  /* /256 */
	IWDG->RLR = reload;

	for (guard = 0; guard < 1000000UL; guard++) {
		if (IWDG->SR == 0U)
			break;
	}

	IWDG->KR = 0xAAAAU;               /* reload                  */
	IWDG->KR = 0xCCCCU;               /* start; cannot be stopped */
}

void u41_iwdg_feed(void)
{
	IWDG->KR = 0xAAAAU;
}
