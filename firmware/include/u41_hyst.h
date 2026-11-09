/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * u41_hyst.h - two-threshold hysteresis, as a header-only pure function.
 *
 * This is the ON/OFF band drawn in u41_pm.h: assert once a value climbs to or
 * past on_mv, deassert once it falls below off_mv, hold the current state in
 * between.  It is the fix for a slow RC voltage curve chattering a decision as
 * it wobbles across a single threshold.
 *
 * It lives in its own header, with no hardware dependency, for one reason: the
 * host-side test (ota/tests/test_hyst.c) compiles this exact function and
 * drives it with the ignition/input voltage curve from the design images, so
 * the no-chatter-in-band behaviour is proven against the code that actually
 * ships - not a re-implementation of it.
 *
 * Requires off_mv < on_mv; callers assert that where their threshold pairs are
 * defined (see u41_pm.h).
 */

#ifndef U41_HYST_H
#define U41_HYST_H

#include <stdint.h>

/*
 * cur      current boolean state (0 or 1)
 * v        fresh reading, same units as the thresholds
 * on_mv    assert at or above this
 * off_mv   deassert below this  (must be < on_mv)
 *
 * returns  the next boolean state
 */
static inline int u41_hysteresis(int cur, uint16_t v,
				 uint16_t on_mv, uint16_t off_mv)
{
	if (v >= on_mv)
		return 1;
	if (v < off_mv)
		return 0;

	return cur;   /* inside the band: hold */
}

/*
 * Charge-band variant: the SAME two-line band, but for a controller whose
 * output should be ASSERTED at LOW voltage and DEASSERTED at HIGH voltage -
 * a battery/supercap charger.  Charge (return 1) once v falls below low_mv,
 * stop (return 0) once v reaches high_mv, hold in between.
 *
 * This is the polarity-inverted twin of u41_hysteresis(): there, "asserted"
 * means "voltage is high enough"; here it means "voltage is low enough that we
 * still need to charge".  Kept as its own function so the intent is explicit
 * at the call site and neither side has to reason about an inversion.
 *
 * Requires low_mv < high_mv.
 */
static inline int u41_charge_hysteresis(int charging, uint16_t v,
					uint16_t high_mv, uint16_t low_mv)
{
	if (v >= high_mv)
		return 0;   /* full enough - stop charging  */
	if (v < low_mv)
		return 1;   /* drooped - resume charging     */

	return charging;    /* inside the band: hold        */
}

#endif /* U41_HYST_H */
