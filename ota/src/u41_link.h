/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * u41_link.h - spidev transport to U41 from the i.MX8 SoM.
 */

#ifndef U41_LINK_H
#define U41_LINK_H

#include <stdint.h>

/* u41_layout.h for the slot constants, u41_meta.h for the boot states -
 * both are needed by the string helpers below. */
#include "u41_layout.h"
#include "u41_meta.h"
#include "u41_proto.h"

/*
 * Default SPI clock.
 *
 * U41 runs from the 16 MHz HSI and its SPI block is a slave, so RM0377 caps
 * SCK at f_PCLK/4 = 4 MHz.  1 MHz leaves a wide margin and still pushes a
 * 6 KiB image in well under a second of wire time; the transfer is dominated
 * by the ~4 ms flash burn per 64-byte chunk, not by the clock.
 */
#define U41_LINK_DEFAULT_HZ   1000000u

/* Give up on a command after this many retries against U41_ST_BUSY. */
#define U41_LINK_MAX_RETRIES  400

struct u41_link {
	int      fd;
	uint32_t speed_hz;
	uint8_t  seq;
	int      verbose;
};

/* Returns 0 on success, -errno on failure. */
int u41_link_open(struct u41_link *lnk, const char *dev, uint32_t speed_hz);
void u41_link_close(struct u41_link *lnk);

/*
 * Send one command and collect its response.
 *
 * Handles the delayed-response protocol described in u41_proto.h: the frame
 * is sent, the guard time observed, then PING frames are clocked until the
 * device returns a response whose sequence byte matches.  U41_ST_BUSY is
 * retried transparently.
 *
 * resp_payload may be NULL.  *resp_len, if given, receives the payload
 * length.  Returns the device's u41_status_t on a completed exchange, or a
 * negative errno if the transport itself failed.
 */
int u41_link_cmd(struct u41_link *lnk,
		 uint8_t cmd, uint32_t arg,
		 const void *payload, uint8_t payload_len,
		 void *resp_payload, uint8_t *resp_len,
		 unsigned guard_us);

const char *u41_status_str(int status);
const char *u41_slot_str(unsigned slot);
const char *u41_boot_state_str(unsigned state);
const char *u41_ota_phase_str(unsigned phase);
const char *u41_pm_state_str(unsigned state);

#endif /* U41_LINK_H */
