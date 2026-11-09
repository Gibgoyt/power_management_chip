/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * test_hyst.c - prove the input-rail hysteresis behaves like the design image.
 *
 * The manager's two sketches:
 *   1.png  a single threshold line - the slow RC voltage curve wobbles across
 *          it near the peak, so ON and OFF land on "the same voltage" and the
 *          state chatters on/off/on/off.
 *   2.png  two lines, a higher ON and a lower OFF, with the band between them
 *          holding state - one clean ON on the way up, one clean OFF on the
 *          way down, no chatter.
 *
 * This test builds the same rising-then-falling curve, runs it through the
 * exact u41_hysteresis() the firmware ships (u41_hyst.h), and asserts:
 *   - it turns ON exactly once, on the way up, at/after the ON line
 *   - noisy wobble while sitting in the band does NOT toggle it
 *   - it turns OFF exactly once, on the way down, at/below the OFF line
 *   - a single-threshold comparator on the same curve WOULD chatter,
 *     so the test is proving a real difference, not a tautology.
 */

#include <stdint.h>
#include <stdio.h>

#include "u41_hyst.h"

/* Match u41_pm.h.  Kept as locals so the test is self-contained. */
#define ON_MV   9000u
#define OFF_MV  8000u

static int failures;

#define CHECK(cond, ...) do { \
	if (!(cond)) { \
		printf("FAIL %s:%d: ", __FILE__, __LINE__); \
		printf(__VA_ARGS__); \
		printf("\n"); \
		failures++; \
	} \
} while (0)

/*
 * A deterministic pseudo-random wobble, so the test is reproducible and needs
 * no <math.h> or rand() seeding.  Returns a small signed jitter in millivolts.
 */
static int wobble(unsigned i)
{
	/* cheap LCG, then fold to roughly +/-250 mV */
	uint32_t x = i * 1103515245u + 12345u;

	return (int)((x >> 16) % 501u) - 250;
}

/*
 * Build the curve from the images: a slow exponential-ish rise to a peak just
 * above the ON line, a noisy dwell at the peak (this is where a single
 * threshold chatters), then a slow decay back down through OFF.  Values in mV.
 */
static uint16_t curve(unsigned i, unsigned n)
{
	unsigned peak = n / 2u;
	int base;

	if (i <= peak) {
		/* rise from ~6000 to ~9300 (just past ON) */
		base = 6000 + (int)((3300u * i) / peak);
	} else {
		/* decay from ~9300 back to ~6000 */
		unsigned j = i - peak;
		unsigned tail = n - peak;

		base = 9300 - (int)((3300u * j) / tail);
	}

	/* Add wobble only near the peak, where the curve lingers in/near the band. */
	if (i > peak - (n / 10u) && i < peak + (n / 10u))
		base += wobble(i);

	if (base < 0)
		base = 0;
	if (base > 0xFFFF)
		base = 0xFFFF;

	return (uint16_t)base;
}

int main(void)
{
	const unsigned N = 400;
	unsigned i;

	int hy = 0;               /* hysteresis state           */
	int single = 0;           /* naive single-threshold state */

	int hy_on_edges = 0, hy_off_edges = 0;
	int single_edges = 0;
	int on_seen_at = -1, off_seen_at = -1;

	printf("u41 hysteresis test (image-2 behaviour)\n");

	for (i = 0; i < N; i++) {
		uint16_t v = curve(i, N);

		/* The shipped hysteresis. */
		int prev_hy = hy;
		hy = u41_hysteresis(hy, v, ON_MV, OFF_MV);

		if (hy != prev_hy) {
			if (hy) {
				hy_on_edges++;
				on_seen_at = (int)i;
				/* Must only ever assert at/above the ON line. */
				CHECK(v >= ON_MV,
				      "asserted ON at %u mV, below ON line %u",
				      v, ON_MV);
			} else {
				hy_off_edges++;
				off_seen_at = (int)i;
				/* Must only ever deassert below the OFF line. */
				CHECK(v < OFF_MV,
				      "deasserted OFF at %u mV, at/above OFF line %u",
				      v, OFF_MV);
			}
		}

		/*
		 * The bug being fixed: a single threshold at the midpoint.  Count
		 * how many times it toggles on the identical curve, to prove the
		 * curve really does wobble across a single line.
		 */
		int prev_single = single;
		single = (v >= (ON_MV + OFF_MV) / 2u) ? 1 : 0;
		if (single != prev_single)
			single_edges++;
	}

	/* Hysteresis: exactly one ON and one OFF edge over the whole curve. */
	CHECK(hy_on_edges == 1, "expected 1 ON edge, got %d", hy_on_edges);
	CHECK(hy_off_edges == 1, "expected 1 OFF edge, got %d", hy_off_edges);
	CHECK(on_seen_at >= 0 && off_seen_at > on_seen_at,
	      "OFF (%d) must come after ON (%d)", off_seen_at, on_seen_at);

	/*
	 * Proof this test is meaningful: the same curve, judged by a single
	 * threshold, chatters (more than the 2 edges a clean signal would give).
	 * If this ever drops to 2, the curve stopped wobbling and the test is no
	 * longer exercising the bug.
	 */
	CHECK(single_edges > 2,
	      "single-threshold comparator did not chatter (%d edges) - the "
	      "test curve is not exercising the band, so it proves nothing",
	      single_edges);

	printf("  hysteresis edges : %d ON, %d OFF (want 1, 1)\n",
	       hy_on_edges, hy_off_edges);
	printf("  single-threshold : %d edges on the same curve (chatters)\n",
	       single_edges);

	/* Direct band checks, independent of the curve. */
	CHECK(u41_hysteresis(0, ON_MV, ON_MV, OFF_MV) == 1,
	      "value exactly at ON must assert");
	CHECK(u41_hysteresis(1, OFF_MV, ON_MV, OFF_MV) == 1,
	      "value exactly at OFF must hold (OFF is exclusive)");
	CHECK(u41_hysteresis(1, OFF_MV - 1u, ON_MV, OFF_MV) == 0,
	      "value just below OFF must deassert");
	CHECK(u41_hysteresis(0, (ON_MV + OFF_MV) / 2u, ON_MV, OFF_MV) == 0,
	      "mid-band from OFF stays off");
	CHECK(u41_hysteresis(1, (ON_MV + OFF_MV) / 2u, ON_MV, OFF_MV) == 1,
	      "mid-band from ON stays on");

	/*
	 * Supercap charge-band behaviour (the manager's charge hysteresis).
	 *
	 * Model the real charger loop: CAP_V rises while charging, and once the
	 * charger stops the pack slowly self-discharges under load.  The
	 * hysteresis must charge up to HIGH, stop, let it droop to below LOW,
	 * resume - one clean toggle per crossing, never chattering while the
	 * voltage dithers inside the band.  Uses the same u41_hysteresis() the
	 * firmware's charger_update() calls, with the charging flag replacing
	 * the on/off flag.
	 */
	{
		const uint16_t CAP_HIGH = 10500u;   /* matches u41_pm.h defaults */
		const uint16_t CAP_LOW  =  9500u;
		int charging = 1;                   /* start charging from empty */
		uint16_t v = 8000u;                 /* below LOW: definitely charging */
		int toggles = 0;
		int prev = charging;
		unsigned step;

		/* A few full charge/discharge cycles with dither near the edges. */
		for (step = 0; step < 2000u; step++) {
			charging = u41_charge_hysteresis(charging, v,
							 CAP_HIGH, CAP_LOW);

			if (charging != prev) {
				toggles++;
				/* Must only resume charging strictly below LOW... */
				if (charging)
					CHECK(v < CAP_LOW,
					      "charger resumed at %u mV, not below LOW %u",
					      v, CAP_LOW);
				else /* ...and only stop at/above HIGH. */
					CHECK(v >= CAP_HIGH,
					      "charger stopped at %u mV, below HIGH %u",
					      v, CAP_HIGH);
				prev = charging;
			}

			/*
			 * Physics of the loop: rise while charging, fall while
			 * holding, plus a small deterministic dither so the band
			 * is genuinely exercised near its edges.
			 */
			int dither = (int)((step * 2654435761u) >> 28) % 40 - 20;
			int nv = (int)v + (charging ? 25 : -18) + dither;

			if (nv < 7000) nv = 7000;
			if (nv > 12000) nv = 12000;
			v = (uint16_t)nv;
		}

		/*
		 * Over ~2000 steps of a slow triangle between ~9.5 and ~10.5V we
		 * expect several clean toggles (charge/stop cycles), and CRUCIALLY
		 * far fewer than a naive single-threshold comparator would produce.
		 * A single mid-band threshold on this dithered signal chatters on
		 * essentially every step near the middle; the band must not.
		 */
		CHECK(toggles >= 2,
		      "charge band never cycled (%d toggles) - test not exercising it",
		      toggles);
		CHECK(toggles < 100,
		      "charge band chattered (%d toggles) - hysteresis not holding "
		      "in the band", toggles);

		printf("  charge band      : %d charger toggles over 2000 steps "
		       "(no chatter)\n", toggles);
	}

	if (failures) {
		printf("\n%d check(s) failed\n", failures);
		return 1;
	}

	printf("all checks passed\n");
	return 0;
}
