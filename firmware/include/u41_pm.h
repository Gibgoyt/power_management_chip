/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * u41_pm.h - power sequencing and supervision for the RIO carrier board.
 */

#ifndef U41_PM_H
#define U41_PM_H

#include <stdint.h>
#include "u41_proto.h"

/*
 * Resistor dividers, straight off the pwr_mgmt sheet:
 *
 *   INPUT_V (PA0): I_12V --[R183 3.3K]--+--[R186 1.1K]-- I_GND
 *   CAP_V   (PA1): S_12V --[R187 3.3K]--+--[R189 1.1K]-- I_GND
 *
 * Both give 1.1 / (3.3 + 1.1) = 1/4, so the rail voltage is four times the
 * voltage at the ADC pin.  With VDDA = PM_3V3 that puts full scale at 13.2 V,
 * comfortably above the 12 V nominal rails.
 */
#define U41_INPUT_V_DIVIDER   4u
#define U41_CAP_V_DIVIDER     4u

/*
 * Thresholds, in millivolts at the rail (not at the ADC pin).
 *
 * Two independent signals, two independent uses.  Get them straight - the
 * board review found the earlier firmware conflated them:
 *
 *   INPUT_V = I_12V / 4   -> "is the M1 24->12V brick delivering?"  UV/OV
 *                            SUPERVISION of the input rail with a small band to
 *                            avoid chatter at the edge.  This is NOT the
 *                            supercap hysteresis.
 *
 *   CAP_V   = S_12V / 4   -> the SUPERCAP charge level.  THIS is where the
 *                            manager's charge hysteresis lives (below).
 *
 * See docs/rio_4.04/{POWER-ARCHITECTURE,U41-INTEGRATION,SUPERCAP-SUBSYSTEM}.md.
 *
 *
 * INPUT-RAIL UV/OV SUPERVISION (INPUT_V band)
 * -------------------------------------------
 * A two-line band so a rail hovering at the edge does not chatter power-up /
 * power-down: come up only above U41_PM_INPUT_ON_MV, drop only below
 * U41_PM_INPUT_OFF_MV, hold in between.  This supervises the M1 output; it does
 * NOT decide supercap charging.
 */
#define U41_PM_INPUT_ON_MV      9000u   /* input healthy -> allow power-up     */
#define U41_PM_INPUT_OFF_MV     8000u   /* input too low -> shut down          */

_Static_assert(U41_PM_INPUT_OFF_MV < U41_PM_INPUT_ON_MV,
	       "input OFF threshold must be below the ON threshold");

/*
 * SUPERCAP CHARGE HYSTERESIS (CAP_V band -> SCAP_EN)   <-- the manager's ask
 * --------------------------------------------------
 * The supercap charger (U17) is an eFuse - a current-limited on/off switch,
 * NOT a regulator - so it cannot hold a charge voltage on its own.  Firmware
 * does it: charge (SCAP_EN=1) until CAP_V reaches HIGH, stop (SCAP_EN=0), and
 * do not resume until CAP_V droops below LOW.  The band between HIGH and LOW
 * stops the charger chattering on/off as the pack self-discharges and tops up.
 *
 *   CAP_V  HIGH ______________  stop charging here
 *          LOW  --------------  resume charging below here
 *               (hold the charger state in the band between)
 *
 * These values are DERIVED FROM THE VEL13353R8257G DATASHEET (the reworked stack;
 * the schematic's 6x HS1225 kept exploding - see docs/rio_4.04/SUPERCAP-SUBSYSTEM.md).
 * The bank is 3x VEL13353R8257G in SERIES, 3.8 V/cell, 250 F/cell:
 *
 *   - Rated stack max        = 3 * 3.8 V = 11.4 V
 *   - Zener clamp (D14/15/16) = 3 * 3.6 V = 10.8 V   (hard ceiling)
 *   - Charge target           = 10.5 V = 3.5 V/cell  (below the clamp, so no
 *                                                      single cell exceeds 3.8 V
 *                                                      even under imbalance)
 *
 * CAP_V = S_12V / 4, and these macros are stack-referred millivolts, so HIGH is
 * the S_12V voltage at which we stop charging.  One check remains before full
 * field trust: a BENCH MEASUREMENT of S_12V at full charge, to confirm the divider
 * and the 3-series topology (docs/rio_4.04/SUPERCAP-SUBSYSTEM.md, "still TO CONFIRM").
 */
#define U41_PM_CAP_CHARGE_HIGH_MV  10500u  /* 3.5 V/cell; stop charging at this CAP_V */
#define U41_PM_CAP_CHARGE_LOW_MV    9500u  /* resume charging below this             */

_Static_assert(U41_PM_CAP_CHARGE_LOW_MV < U41_PM_CAP_CHARGE_HIGH_MV,
	       "cap charge LOW threshold must be below HIGH");

/*
 * Minimum CAP_V for the caps to be considered "charged enough to power the
 * board up" - i.e. enough stored energy for a graceful shutdown if input is
 * lost right after boot.  Distinct from the charge band: this gates PRECHARGE
 * -> POWER_UP.  Set at the charge-band LOW (9.5 V / ~3.17 V per cell): if the caps
 * are at least topped up to the resume-charging mark there is ample shutdown energy.
 * Confirm against a bench S_12V measurement with the real 3-series stack.
 */
#define U41_PM_CAP_READY_MV      9500u   /* enough charge to start up (= band LOW) */

/* Timings, milliseconds. */
#define U41_PM_PRECHARGE_MAX_MS   15000u  /* give up waiting for the caps    */
#define U41_PM_PG_TIMEOUT_MS       1000u  /* a rail must assert PG this fast */
#define U41_PM_RAIL_STAGGER_MS      100u  /* gap between bringing rails up   */

/*
 * How long the SoM gets to shut down after ignition drops.
 *
 * meta-rio's imx8qm-rio.dts refers to "the PIC micro's ~30 s ignition
 * timeout", so 30 s is what the Linux side already expects.  Keeping the same
 * number means the existing som-fb-keepalive service behaves as before.
 */
#define U41_PM_GRACEFUL_MS        30000u

/*
 * SOM_FB watchdog.  Once the SoM has been seen alive, losing the keep-alive
 * for this long means Linux has hung and the board gets power cycled.
 */
#define U41_PM_SOMFB_TIMEOUT_MS   60000u

/* Debounce for the ignition input. */
#define U41_PM_IGN_DEBOUNCE_MS      200u

void u41_pm_init(void);

/*
 * Run one pass of the state machine.  Called from the main loop; must be
 * cheap and must never block, because the same loop also services SPI.
 */
void u41_pm_poll(void);

/* Fill *out for a U41_CMD_GET_PM_STATE response. */
void u41_pm_status(u41_pm_status_t *out);

/* Manual rail override from the host.  Returns a u41_status_t. */
uint8_t u41_pm_set_rail(uint32_t rail, int on);

/* Host-side keep-alive, an alternative to wiggling SOM_FB. */
void u41_pm_keepalive(void);

/*
 * True when it is safe to spend milliseconds inside the flash controller:
 * the SoM is up, the rails are stable and no sequencing is in progress.  The
 * OTA path checks this before every erase or program so a stall can never
 * land in the middle of a power transition.
 */
int u41_pm_flash_window_open(void);

u41_pm_state_t u41_pm_state(void);

#endif /* U41_PM_H */
