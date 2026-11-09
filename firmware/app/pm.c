/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * pm.c - power sequencing and supervision.
 *
 * This is the part of the firmware that must never stop running.  It holds
 * SOM_PSU_EN, so if it stalls or faults the i.MX8 loses power - including
 * while the SoM is in the middle of pushing an OTA image to us.  Everything
 * here is non-blocking for that reason: u41_pm_poll() does one pass and
 * returns, and the OTA code asks u41_pm_flash_window_open() before it burns
 * anything.
 *
 * State machine:
 *
 *      BOOT
 *        |
 *        v
 *      OFF <-------------------------------+
 *        | ignition on, input rail healthy |
 *        v                                 |
 *      PRECHARGE  (SCAP_EN, wait for caps) |
 *        |                                 |
 *        v                                 |
 *      POWER_UP   (SOM, then WiFi, LTE)    |
 *        |                                 |
 *        v                                 |
 *      RUN -- ignition off --> GRACEFUL ---+
 *        |                        |        |
 *        |                        v        |
 *        +------------------> POWER_DOWN --+
 *        |
 *        v  OC_PG low, input critical, or a rail refused to come up
 *      FAULT
 */

#include <string.h>

#include "stm32l0xx.h"
#include "u41_board.h"
#include "u41_hyst.h"
#include "u41_pm.h"
#include "u41_sys.h"

/* Factory VREFINT, measured at VDDA = 3.000 V (datasheet Table 23). */
#define VREFINT_CAL_ADDR   ((const volatile uint16_t *)0x1FF80078UL)
#define VREFINT_CAL_VDDA_MV  3000UL

/* ADC channel 17 is VREFINT on this family. */
#define ADC_CH_VREFINT     17u

/*
 * Slow inter-sample filter on the two rail voltages.
 *
 * The hardware oversampler (see adc_init) removes the fast per-conversion
 * noise inside one adc_sample() call.  This EMA sits on top and smooths the
 * slow ripple *between* the 100 ms samples, so a rail that is genuinely
 * hovering near a threshold produces a steady reading rather than one that
 * dithers a few millivolts each cycle.
 *
 * new_avg = old_avg + (sample - old_avg) / 2^SHIFT.  Shift 3 is a ~8-sample
 * time constant: at 100 ms per sample that is a ~0.8 s settle, fast enough to
 * catch a real power loss and slow enough to ignore ripple.  This is a filter
 * on the *reading*, distinct from the ON/OFF hysteresis on the *decision*
 * (u41_pm.h) - the two solve different problems and are both needed.
 */
#define U41_ADC_EMA_SHIFT   3u

static struct {
	u41_pm_state_t state;
	uint32_t       state_since;

	uint16_t       input_mv;
	uint16_t       cap_mv;
	uint16_t       vdda_mv;

	/* EMA accumulators, in the same millivolt units, seeded on first sample. */
	uint16_t       input_mv_ema;
	uint16_t       cap_mv_ema;
	uint8_t        ema_seeded;

	uint32_t       last_adc_ms;

	/* ignition debounce */
	uint8_t        ign_raw;
	uint8_t        ign_stable;
	uint32_t       ign_change_ms;

	/*
	 * Latched result of the input-rail supervision band.  1 = input is
	 * "good" (rose past ON, not yet fallen past OFF), 0 = "bad".  Held here
	 * so the band's memory survives between samples.
	 */
	uint8_t        input_good;

	/*
	 * Latched supercap charger state.  1 = charging (SCAP_EN asserted),
	 * 0 = charged/holding.  Updated each poll from the CAP_V hysteresis band
	 * so the charger tops the caps up to HIGH then stops until they droop
	 * below LOW - the manager's charge hysteresis.
	 */
	uint8_t        cap_charging;

	/* SOM_FB / host keep-alive supervision */
	uint8_t        som_seen_alive;
	uint32_t       last_alive_ms;

	/* manual overrides requested over SPI */
	uint8_t        wifi_forced_off;
	uint8_t        lte_forced_off;
} pm;

/* ------------------------------------------------------------------ ADC */

static void adc_init(void)
{
	RCC->APB2ENR |= RCC_APB2ENR_ADC1EN;

	/* PA0 and PA1 to analogue mode. */
	u41_gpio_mode(U41_INPUT_V_PORT, U41_INPUT_V_PIN, U41_MODE_ANALOG);
	u41_gpio_mode(U41_CAP_V_PORT,   U41_CAP_V_PIN,   U41_MODE_ANALOG);

	/* Make sure the ADC is disabled before calibrating (RM0377 13.3.3). */
	if ((ADC1->CR & ADC_CR_ADEN) != 0UL) {
		ADC1->CR |= ADC_CR_ADDIS;
		while ((ADC1->CR & ADC_CR_ADEN) != 0UL)
			;
	}

	ADC1->CFGR1 = 0;                       /* 12-bit, right aligned      */

	/*
	 * CFGR2: PCLK/2 clock, plus the hardware oversampler.
	 *
	 * This is the fix for "the ADC is not averaging, just continuously
	 * reading".  Rather than average in software, the STM32L0 ADC has a
	 * built-in oversampler (RM0377 13.8): it runs N back-to-back
	 * conversions, sums them, and right-shifts the sum, all in hardware,
	 * so a single EOC hands back an already-averaged result.  Doing it in
	 * hardware means it cannot be defeated by an interrupt stealing cycles
	 * mid-accumulation, and it costs no code.
	 *
	 *   OVSE            enable
	 *   OVSR = 011      ratio 16x  (sum of 16 conversions)
	 *   OVSS = 0100     right-shift 4  (16x sum back down to 12-bit scale)
	 *
	 * 16x buys ~2 extra noise-free bits.  At the long sample time below,
	 * 16 conversions still complete in well under 1 ms, so adc_sample()
	 * stays cheap enough to call from the main loop.
	 */
	ADC1->CFGR2 = ADC_CFGR2_CKMODE_0
		    | ADC_CFGR2_OVSE
		    | ADC_CFGR2_OVSR_0 | ADC_CFGR2_OVSR_1   /* 011 = 16x   */
		    | ADC_CFGR2_OVSS_2;                     /* 0100 = >>4  */

	ADC1->SMPR  = ADC_SMPR_SMP_0 | ADC_SMPR_SMP_1 | ADC_SMPR_SMP_2;
					       /* longest sample time: the
						* dividers are 3.3K/1.1K, so
						* the source impedance is ~825
						* ohm and needs the time      */

	ADC1->CR |= ADC_CR_ADCAL;
	while ((ADC1->CR & ADC_CR_ADCAL) != 0UL)
		;

	/* VREFINT needs enabling in the common register before it can be read. */
	ADC->CCR |= ADC_CCR_VREFEN;

	ADC1->ISR |= ADC_ISR_ADRDY;
	ADC1->CR  |= ADC_CR_ADEN;
	while ((ADC1->ISR & ADC_ISR_ADRDY) == 0UL)
		;
}

/*
 * One averaged reading of a channel.
 *
 * The oversampler is enabled (adc_init), so the single conversion started
 * here is really 16 conversions summed and shifted in hardware - the EOC
 * below fires once, when the whole averaged result is ready, and DR holds the
 * averaged value.  Do NOT re-add a software averaging loop around this: the
 * averaging already happened.
 */
static uint16_t adc_read(unsigned channel)
{
	uint32_t guard = 100000UL;

	ADC1->CHSELR = (1UL << channel);
	ADC1->CR |= ADC_CR_ADSTART;

	while ((ADC1->ISR & ADC_ISR_EOC) == 0UL) {
		if (--guard == 0UL)
			return 0;
	}

	return (uint16_t)ADC1->DR;
}

/* One EMA step: pull the running average a fraction of the way to the sample. */
static uint16_t ema_step(uint16_t avg, uint16_t sample)
{
	/* Signed intermediate so a falling rail is tracked as fast as a rising one. */
	int32_t a = (int32_t)avg;

	a += ((int32_t)sample - a) >> U41_ADC_EMA_SHIFT;

	if (a < 0)
		a = 0;
	if (a > 0xFFFF)
		a = 0xFFFF;

	return (uint16_t)a;
}

/*
 * Convert a raw reading to millivolts at the rail.
 *
 * VDDA is derived from VREFINT rather than assumed to be 3300 mV.  PM_3V3 is
 * a real regulator with real tolerance, and every threshold in this file is a
 * decision about whether to power the vehicle's camera system, so it is worth
 * the handful of instructions to measure it.  RM0377 12.4.34:
 *
 *      VDDA = VDDA_Charac x VREFINT_CAL / VREFINT_DATA
 */
static void adc_sample(void)
{
	uint32_t vrefint = adc_read(ADC_CH_VREFINT);
	uint32_t cal     = *VREFINT_CAL_ADDR;
	uint32_t raw_in, raw_cap;

	if (vrefint == 0UL || cal == 0UL)
		pm.vdda_mv = 3300u;   /* fall back to nominal */
	else
		pm.vdda_mv = (uint16_t)((VREFINT_CAL_VDDA_MV * cal) / vrefint);

	raw_in  = adc_read(U41_INPUT_V_ADC_CH);
	raw_cap = adc_read(U41_CAP_V_ADC_CH);

	{
		uint16_t in_mv  = (uint16_t)((raw_in  * pm.vdda_mv * U41_INPUT_V_DIVIDER) / 4095UL);
		uint16_t cap_mv = (uint16_t)((raw_cap * pm.vdda_mv * U41_CAP_V_DIVIDER)   / 4095UL);

		/*
		 * Seed the EMA on the first sample rather than let it crawl up
		 * from zero, so the power-up decision is made against a real
		 * reading, not a filtered-from-cold one.
		 */
		if (!pm.ema_seeded) {
			pm.input_mv_ema = in_mv;
			pm.cap_mv_ema   = cap_mv;
			pm.ema_seeded   = 1;
		} else {
			pm.input_mv_ema = ema_step(pm.input_mv_ema, in_mv);
			pm.cap_mv_ema   = ema_step(pm.cap_mv_ema,   cap_mv);
		}

		/*
		 * Publish the filtered values.  Every threshold comparison and
		 * the GET_PM_STATE report use pm.input_mv / pm.cap_mv, so they
		 * all see the smoothed reading.
		 */
		pm.input_mv = pm.input_mv_ema;
		pm.cap_mv   = pm.cap_mv_ema;
	}
}

/* --------------------------------------------------------------- GPIO */

static void gpio_init(void)
{
	RCC->IOPENR |= RCC_IOPENR_GPIOAEN | RCC_IOPENR_GPIOBEN;

	/*
	 * The bootloader already drove the enables low and made them outputs.
	 * Re-asserting that here keeps the application correct if it is ever
	 * started directly by a debugger with no bootloader in front of it.
	 */
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

	/* Inputs.  Power-good lines and OC_PG are driven by their supplies. */
	u41_gpio_mode(U41_SOM_PSU_PG_PORT,  U41_SOM_PSU_PG_PIN,  U41_MODE_INPUT);
	u41_gpio_mode(U41_WIFI_PSU_PG_PORT, U41_WIFI_PSU_PG_PIN, U41_MODE_INPUT);
	u41_gpio_mode(U41_LTE_PSU_PG_PORT,  U41_LTE_PSU_PG_PIN,  U41_MODE_INPUT);
	u41_gpio_mode(U41_OC_PG_PORT,       U41_OC_PG_PIN,       U41_MODE_INPUT);
	u41_gpio_mode(U41_IGN_INP_PORT,     U41_IGN_INP_PIN,     U41_MODE_INPUT);

	/*
	 * SOM_FB comes from PCA9534 U19 P7, which is high-Z until Linux has
	 * probed the expander and the gpio-hog has fired.  Pull it to the
	 * not-alive level so a SoM that never boots reads as dead rather than
	 * as whatever the line happens to float to.
	 */
	u41_gpio_mode(U41_SOM_FB_PORT, U41_SOM_FB_PIN, U41_MODE_INPUT);
	u41_gpio_pull(U41_SOM_FB_PORT, U41_SOM_FB_PIN,
		      U41_SOM_FB_ALIVE_LEVEL ? 2u : 1u);

	/*
	 * PWR_SCL / PWR_SDA are left as inputs.  They have 2.2K pullups on the
	 * board and this firmware does not drive that bus yet; configuring
	 * them as anything else risks holding it down.
	 */
	u41_gpio_mode(U41_PWR_I2C_PORT, U41_PWR_SCL_PIN, U41_MODE_INPUT);
	u41_gpio_mode(U41_PWR_I2C_PORT, U41_PWR_SDA_PIN, U41_MODE_INPUT);
}

static int som_fb_alive(void)
{
	return U41_GPIO_GET(U41_SOM_FB_PORT, U41_SOM_FB_PIN) ==
	       (uint32_t)U41_SOM_FB_ALIVE_LEVEL;
}

static void enter(u41_pm_state_t next)
{
	if (pm.state == next)
		return;

	pm.state       = next;
	pm.state_since = u41_millis();
}

static uint32_t in_state_ms(void)
{
	return u41_millis() - pm.state_since;
}

/*
 * The two-threshold hysteresis band (u41_hysteresis) lives in u41_hyst.h so
 * the host-side test can exercise the exact same function.  See that header
 * and the ON/OFF band drawing in u41_pm.h.
 */

/*
 * Debounced ignition.  Bouncing here would power cycle the whole unit.
 *
 * IGN_INP (PB8) is a clean 3.3V logic level, conditioned on the board by the
 * Overview opto (U1 ACPL-M43T) + MOSFET (Q1 DMN2004) front-end.  It arrives
 * ALREADY active-low - ignition-on pulls PB8 low (see u41_board.h and
 * docs/rio_4.04/IGNITION-PATH.md) - so we convert the raw pin level to the
 * logical ignition state up front and debounce that.  Everything downstream
 * reads pm.ign_stable as "ignition is on (1) / off (0)".
 *
 * Debounce is the ONLY conditioning needed here: the front-end already squares
 * the edge, so there is no slow analog ramp to band against.  (The analog
 * hysteresis in this firmware belongs to the SUPERCAP charge voltage, CAP_V,
 * not to ignition - see u41_ota / the charge band below.)
 */
static void ignition_poll(void)
{
	uint32_t level = U41_GPIO_GET(U41_IGN_INP_PORT, U41_IGN_INP_PIN);
	uint8_t  ign   = (level == (uint32_t)U41_IGN_ACTIVE_LEVEL) ? 1u : 0u;

#if defined(U41_BRINGUP_IGNORE_IGNITION) && U41_BRINGUP_IGNORE_IGNITION
	/*
	 * BRING-UP AID (not for production).  On the bench, PB8 was observed
	 * stuck HIGH (= ignition-off) even with the vehicle ignition asserted,
	 * so the power manager parked in OFF and never powered the SoM.  The
	 * ignition front-end (opto U1 + Q1, pulled up by R2 to SOM_3V3) is under
	 * investigation - notably SOM_3V3 is absent until the SoM is powered, so
	 * the pull-up reference for NetD2_A may be missing at power-on.  To let
	 * the rest of the sequencing (precharge -> SOM_PSU_EN -> rails -> RUN) be
	 * validated meanwhile, force ignition "on".  Enable with
	 * -DU41_BRINGUP_IGNORE_IGNITION=1 in the Makefile.  MUST be off for a
	 * shipping build.  See docs/todos/bohemoth-001-get-u41-firmware-working.md.
	 */
	ign = 1u;
#endif

	if (ign != pm.ign_raw) {
		pm.ign_raw       = ign;
		pm.ign_change_ms = u41_millis();
		return;
	}

	if (pm.ign_stable != ign &&
	    (u41_millis() - pm.ign_change_ms) >= U41_PM_IGN_DEBOUNCE_MS)
		pm.ign_stable = ign;
}

static void rails_all_off(void)
{
	U41_GPIO_CLR(U41_LTE_PSU_EN_PORT,  U41_LTE_PSU_EN_PIN);
	U41_GPIO_CLR(U41_WIFI_PSU_EN_PORT, U41_WIFI_PSU_EN_PIN);
	U41_GPIO_CLR(U41_SOM_PSU_EN_PORT,  U41_SOM_PSU_EN_PIN);
	U41_GPIO_CLR(U41_PSE_PWR_EN_PORT,  U41_PSE_PWR_EN_PIN);
	U41_GPIO_CLR(U41_SCAP_EN_PORT,     U41_SCAP_EN_PIN);
	pm.cap_charging = 0;
}

/*
 * Supercap charge controller - the manager's charge hysteresis.
 *
 * U17 is an eFuse, not a regulator (docs/rio_4.04/SUPERCAP-SUBSYSTEM.md), so
 * the charge set-point is ours: run a two-threshold band on CAP_V and drive
 * SCAP_EN from it.  Charge until CAP_V reaches HIGH, stop, and do not resume
 * until it droops below LOW.  Called every poll in the states where charging
 * is appropriate; NOT called while powering down (rails_all_off drops
 * SCAP_EN and clears the flag).
 *
 * `enable` lets a state suppress charging entirely (e.g. OFF before ignition):
 * when false the charger is held off but the flag is left as-is so the band
 * resumes cleanly next time it is enabled.
 */
static void charger_update(int enable)
{
	if (!enable) {
		U41_GPIO_CLR(U41_SCAP_EN_PORT, U41_SCAP_EN_PIN);
		return;
	}

	pm.cap_charging = (uint8_t)u41_charge_hysteresis(pm.cap_charging,
							 pm.cap_mv,
							 U41_PM_CAP_CHARGE_HIGH_MV,
							 U41_PM_CAP_CHARGE_LOW_MV);

	U41_GPIO_WRITE(U41_SCAP_EN_PORT, U41_SCAP_EN_PIN, pm.cap_charging);
}

/* ---------------------------------------------------------------- API */

void u41_pm_init(void)
{
	memset(&pm, 0, sizeof(pm));

	gpio_init();
	adc_init();

	pm.state       = U41_PM_BOOT;
	pm.state_since = u41_millis();
	/* Seed from the logical ignition state (active-low), same as ignition_poll(). */
	pm.ign_raw     = (U41_GPIO_GET(U41_IGN_INP_PORT, U41_IGN_INP_PIN) ==
			  (uint32_t)U41_IGN_ACTIVE_LEVEL) ? 1u : 0u;
	pm.ign_stable  = pm.ign_raw;

	adc_sample();
	pm.last_adc_ms = u41_millis();

	/*
	 * Seed the hysteresis latch from the first reading: input is "good" only
	 * if it is already at or above the ON line.  Starting from 0 and letting
	 * the band decide would be wrong here - a rail sitting in the band at
	 * boot must read as not-yet-good, which the >= ON test gives us.
	 */
	pm.input_good = (pm.input_mv >= U41_PM_INPUT_ON_MV) ? 1u : 0u;
}

void u41_pm_poll(void)
{
	uint32_t now = u41_millis();

	ignition_poll();

	/* 100 ms is plenty for rails that move on the scale of seconds. */
	if ((now - pm.last_adc_ms) >= 100u) {
		adc_sample();
		pm.last_adc_ms = now;
	}

	/*
	 * Update the input-rail hysteresis band once per poll, from the
	 * filtered voltage.  Every state that cares about input power reads
	 * pm.input_good rather than comparing pm.input_mv against a bare
	 * threshold, so the ON/OFF decision is made in exactly one place.
	 */
	pm.input_good = (uint8_t)u41_hysteresis(pm.input_good, pm.input_mv,
						U41_PM_INPUT_ON_MV,
						U41_PM_INPUT_OFF_MV);

	/*
	 * Supercap charge hysteresis, driven once per poll from CAP_V.  Charge
	 * only while the input is healthy and we are in a state that is bringing
	 * the board up or running it - i.e. PRECHARGE, POWER_UP and RUN.  In
	 * OFF/GRACEFUL/POWER_DOWN/FAULT/BOOT the charger is held off (in the
	 * powered-down states rails_all_off has already dropped SCAP_EN).
	 * Charging also stops if the input rail itself sags, since there is then
	 * no energy to charge from.
	 */
	{
		int charge_ok = pm.input_good &&
			(pm.state == U41_PM_PRECHARGE ||
			 pm.state == U41_PM_POWER_UP  ||
			 pm.state == U41_PM_RUN);

		charger_update(charge_ok);
	}

	if (som_fb_alive()) {
		pm.som_seen_alive = 1;
		pm.last_alive_ms  = now;
	}

	/*
	 * OC_PG is checked in every state that has rails up.  An over-current
	 * is the one condition worth reacting to faster than the state
	 * machine's normal cadence.
	 */
#if !(defined(U41_BRINGUP_IGNORE_IGNITION) && U41_BRINGUP_IGNORE_IGNITION)
	if (pm.state == U41_PM_RUN || pm.state == U41_PM_POWER_UP ||
	    pm.state == U41_PM_GRACEFUL) {
		if (U41_GPIO_GET(U41_OC_PG_PORT, U41_OC_PG_PIN) == 0UL) {
			rails_all_off();
			enter(U41_PM_FAULT);
			return;
		}
	}
#else
	/*
	 * BRING-UP: OC_PG (PA8) reads 0 on the bench (like IGN, it may depend on
	 * a rail/reference that is absent until the SoM is up), which tripped an
	 * instant FAULT in POWER_UP and dropped the rails before the SoM could
	 * come up.  Suppressed under the bring-up flag so the enables stay
	 * asserted and the SoM power path can be validated.  MUST be off for a
	 * shipping build.  See docs/todos/bohemoth-001-get-u41-firmware-working.md.
	 */
#endif

	switch (pm.state) {
	case U41_PM_BOOT:
		rails_all_off();
		enter(U41_PM_OFF);
		break;

	case U41_PM_OFF:
		if (!pm.ign_stable)
			break;
		/*
		 * Power up only once the input has risen past the ON line.
		 * pm.input_good carries the hysteresis, so a rail hovering in
		 * the band does not start-stop-start.
		 */
		if (!pm.input_good)
			break;

		/* SCAP_EN is driven by charger_update() in poll(); just advance. */
		enter(U41_PM_PRECHARGE);
		break;

	case U41_PM_PRECHARGE:
		if (!pm.ign_stable) {
			rails_all_off();
			enter(U41_PM_OFF);
			break;
		}

		/*
		 * Move on once the caps are up, or once we have waited long
		 * enough.  Timing out is not a failure: on a warm restart the
		 * caps may already be charged and the comparator threshold is
		 * only an estimate until someone measures the real hold-up.
		 */
		if (pm.cap_mv >= U41_PM_CAP_READY_MV ||
		    in_state_ms() >= U41_PM_PRECHARGE_MAX_MS) {
			U41_GPIO_SET(U41_SOM_PSU_EN_PORT, U41_SOM_PSU_EN_PIN);
			enter(U41_PM_POWER_UP);
		}
		break;

	case U41_PM_POWER_UP:
		if (U41_GPIO_GET(U41_SOM_PSU_PG_PORT, U41_SOM_PSU_PG_PIN)
#if defined(U41_BRINGUP_IGNORE_IGNITION) && U41_BRINGUP_IGNORE_IGNITION
		    /*
		     * BRING-UP: proceed after the stagger even if SOM_PSU_PG
		     * (PB0) never asserts, so a missing/late power-good line
		     * cannot block the SoM bring-up.  Off for shipping builds.
		     */
		    || (in_state_ms() >= U41_PM_RAIL_STAGGER_MS)
#endif
		   ) {
			if (in_state_ms() < U41_PM_RAIL_STAGGER_MS)
				break;

			if (!pm.wifi_forced_off)
				U41_GPIO_SET(U41_WIFI_PSU_EN_PORT,
					     U41_WIFI_PSU_EN_PIN);
			if (!pm.lte_forced_off)
				U41_GPIO_SET(U41_LTE_PSU_EN_PORT,
					     U41_LTE_PSU_EN_PIN);

			U41_GPIO_SET(U41_PSE_PWR_EN_PORT, U41_PSE_PWR_EN_PIN);

			pm.som_seen_alive = 0;
			pm.last_alive_ms  = u41_millis();
			enter(U41_PM_RUN);
			break;
		}

		if (in_state_ms() >= U41_PM_PG_TIMEOUT_MS) {
			/* The SoM rail never came up.  Do not keep trying. */
			rails_all_off();
			enter(U41_PM_FAULT);
		}
		break;

	case U41_PM_RUN:
		/*
		 * Shut down only once the input has fallen past the OFF line
		 * (pm.input_good has gone false).  The same band that gated
		 * power-up now gates power-down, so the two decisions can never
		 * disagree about "the same voltage" the way a single threshold
		 * did.
		 */
		if (!pm.input_good) {
			enter(U41_PM_GRACEFUL);
			break;
		}

		if (!pm.ign_stable) {
			enter(U41_PM_GRACEFUL);
			break;
		}

		/*
		 * SOM_FB watchdog.  Only armed once the SoM has been seen
		 * alive at least once, so a slow-booting or deliberately
		 * headless SoM is not power cycled forever.
		 */
		if (pm.som_seen_alive &&
		    (now - pm.last_alive_ms) >= U41_PM_SOMFB_TIMEOUT_MS) {
			rails_all_off();
			enter(U41_PM_OFF);
		}
		break;

	case U41_PM_GRACEFUL:
		/*
		 * Ignition came back and the input recovered above the ON line:
		 * carry on.  Using pm.input_good (not a bare compare) means a
		 * brief dip into the band during shutdown does not bounce us
		 * back into RUN.
		 */
		if (pm.ign_stable && pm.input_good) {
			enter(U41_PM_RUN);
			break;
		}

		/*
		 * Linux dropping the keep-alive is how it says "I am done".
		 * Take it and cut power early rather than burning the rest of
		 * the 30 s on supercap charge.
		 */
		if (pm.som_seen_alive && !som_fb_alive() &&
		    in_state_ms() > 2000u) {
			enter(U41_PM_POWER_DOWN);
			break;
		}

		if (in_state_ms() >= U41_PM_GRACEFUL_MS)
			enter(U41_PM_POWER_DOWN);
		break;

	case U41_PM_POWER_DOWN:
		U41_GPIO_CLR(U41_LTE_PSU_EN_PORT,  U41_LTE_PSU_EN_PIN);
		U41_GPIO_CLR(U41_WIFI_PSU_EN_PORT, U41_WIFI_PSU_EN_PIN);
		U41_GPIO_CLR(U41_PSE_PWR_EN_PORT,  U41_PSE_PWR_EN_PIN);

		if (in_state_ms() >= U41_PM_RAIL_STAGGER_MS) {
			rails_all_off();
			pm.som_seen_alive = 0;
			enter(U41_PM_OFF);
		}
		break;

	case U41_PM_FAULT:
		/*
		 * Latch until the operator removes ignition.  Retrying into an
		 * over-current would just cycle the fault.
		 */
		rails_all_off();
		if (!pm.ign_stable)
			enter(U41_PM_OFF);
		break;
	}
}

void u41_pm_status(u41_pm_status_t *out)
{
	memset(out, 0, sizeof(*out));

	out->input_mv    = pm.input_mv;
	out->cap_mv      = pm.cap_mv;
	out->pm_state    = (uint8_t)pm.state;
	out->ignition    = pm.ign_stable;
	out->som_fb      = (uint8_t)som_fb_alive();
	out->oc_pg       = (uint8_t)U41_GPIO_GET(U41_OC_PG_PORT, U41_OC_PG_PIN);
	out->som_psu_en  = (uint8_t)U41_GPIO_GET(U41_SOM_PSU_EN_PORT,  U41_SOM_PSU_EN_PIN);
	out->wifi_psu_en = (uint8_t)U41_GPIO_GET(U41_WIFI_PSU_EN_PORT, U41_WIFI_PSU_EN_PIN);
	out->lte_psu_en  = (uint8_t)U41_GPIO_GET(U41_LTE_PSU_EN_PORT,  U41_LTE_PSU_EN_PIN);
	out->scap_en     = (uint8_t)U41_GPIO_GET(U41_SCAP_EN_PORT,     U41_SCAP_EN_PIN);
	out->pse_pwr_en  = (uint8_t)U41_GPIO_GET(U41_PSE_PWR_EN_PORT,  U41_PSE_PWR_EN_PIN);
	out->som_psu_pg  = (uint8_t)U41_GPIO_GET(U41_SOM_PSU_PG_PORT,  U41_SOM_PSU_PG_PIN);
	out->wifi_psu_pg = (uint8_t)U41_GPIO_GET(U41_WIFI_PSU_PG_PORT, U41_WIFI_PSU_PG_PIN);
	out->lte_psu_pg  = (uint8_t)U41_GPIO_GET(U41_LTE_PSU_PG_PORT,  U41_LTE_PSU_PG_PIN);
	out->state_ms    = in_state_ms();
}

uint8_t u41_pm_set_rail(uint32_t rail, int on)
{
	switch (rail) {
	case U41_RAIL_WIFI:
		pm.wifi_forced_off = on ? 0u : 1u;
		U41_GPIO_WRITE(U41_WIFI_PSU_EN_PORT, U41_WIFI_PSU_EN_PIN, on);
		return U41_ST_OK;

	case U41_RAIL_LTE:
		pm.lte_forced_off = on ? 0u : 1u;
		U41_GPIO_WRITE(U41_LTE_PSU_EN_PORT, U41_LTE_PSU_EN_PIN, on);
		return U41_ST_OK;

	case U41_RAIL_SCAP:
		U41_GPIO_WRITE(U41_SCAP_EN_PORT, U41_SCAP_EN_PIN, on);
		return U41_ST_OK;

	case U41_RAIL_PSE:
		U41_GPIO_WRITE(U41_PSE_PWR_EN_PORT, U41_PSE_PWR_EN_PIN, on);
		return U41_ST_OK;

	default:
		return U41_ST_BAD_ARG;
	}
}

void u41_pm_keepalive(void)
{
	pm.som_seen_alive = 1;
	pm.last_alive_ms  = u41_millis();
}

int u41_pm_flash_window_open(void)
{
	/*
	 * An erase or program stalls the core for ~4 ms.  That is harmless in
	 * RUN, where the rails are steady and nothing is being sequenced, and
	 * unacceptable anywhere else - PRECHARGE and POWER_UP are timing rail
	 * transitions, and GRACEFUL is counting down a shutdown.
	 *
	 * In practice this is never restrictive: the only way an OTA can be in
	 * progress is if Linux is running, which means we are in RUN.
	 */
	return pm.state == U41_PM_RUN;
}

u41_pm_state_t u41_pm_state(void)
{
	return pm.state;
}
