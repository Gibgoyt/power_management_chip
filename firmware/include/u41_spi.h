/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * u41_spi.h - SPI1 slave transport for the PM_SPI link to the i.MX8.
 */

#ifndef U41_SPI_H
#define U41_SPI_H

#include <stdint.h>
#include "u41_proto.h"

void u41_spi_init(void);

/*
 * Poll for a complete inbound frame.
 *
 * Returns 1 and copies U41_FRAME_SIZE bytes into frame when the host has
 * clocked a whole frame in and released NSS, otherwise 0.  The CRC has
 * already been checked; a frame that fails it is answered automatically with
 * U41_ST_BAD_CRC and never reaches the caller.
 */
int u41_spi_poll(uint8_t *frame);

/*
 * Stage the response to the frame just handled.  It is shifted out during the
 * host's next transaction - see the DELAYED RESPONSE note in u41_proto.h.
 * The CRC is computed here, so callers only fill in the fields.
 */
void u41_spi_reply(uint8_t cmd, uint8_t seq, uint8_t status,
		   const void *payload, uint8_t len);

/* Stage a bare status with no payload. */
void u41_spi_reply_status(uint8_t cmd, uint8_t seq, uint8_t status);

#endif /* U41_SPI_H */
