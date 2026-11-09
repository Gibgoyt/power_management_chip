/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * u41_link.c - spidev transport to U41.
 *
 * One full-duplex 80-byte transfer per transaction.  Because the slave cannot
 * answer inside the transaction that carries the question (see the DELAYED
 * RESPONSE note in u41_proto.h), every command here is followed by PING
 * frames until the matching response comes back.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <linux/spi/spidev.h>
#include <sys/ioctl.h>

#include "u41_link.h"

static void put_le32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v);
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

static uint32_t get_le32(const uint8_t *p)
{
	return (uint32_t)p[0]
	     | ((uint32_t)p[1] << 8)
	     | ((uint32_t)p[2] << 16)
	     | ((uint32_t)p[3] << 24);
}

static void sleep_us(unsigned us)
{
	struct timespec ts;

	ts.tv_sec  = us / 1000000u;
	ts.tv_nsec = (long)(us % 1000000u) * 1000L;

	while (nanosleep(&ts, &ts) == -1 && errno == EINTR)
		;
}

int u41_link_open(struct u41_link *lnk, const char *dev, uint32_t speed_hz)
{
	uint8_t  mode = SPI_MODE_0;
	uint8_t  bits = 8;
	uint32_t hz   = speed_hz ? speed_hz : U41_LINK_DEFAULT_HZ;

	memset(lnk, 0, sizeof(*lnk));

	lnk->fd = open(dev, O_RDWR);
	if (lnk->fd < 0)
		return -errno;

	/*
	 * Mode 0, MSB first, 8 bits - matched to the slave configuration in
	 * the firmware's spi_slave.c.  The STM32L051 SPI block only does MSB
	 * first, so there is nothing to negotiate.
	 */
	if (ioctl(lnk->fd, SPI_IOC_WR_MODE, &mode) < 0)
		goto fail;
	if (ioctl(lnk->fd, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0)
		goto fail;
	if (ioctl(lnk->fd, SPI_IOC_WR_MAX_SPEED_HZ, &hz) < 0)
		goto fail;

	/* Read back: the driver may have rounded the clock down. */
	if (ioctl(lnk->fd, SPI_IOC_RD_MAX_SPEED_HZ, &hz) < 0)
		goto fail;

	lnk->speed_hz = hz;
	lnk->seq      = 1;

	return 0;

fail:
	{
		int err = -errno;

		close(lnk->fd);
		lnk->fd = -1;
		return err;
	}
}

void u41_link_close(struct u41_link *lnk)
{
	if (lnk->fd >= 0) {
		close(lnk->fd);
		lnk->fd = -1;
	}
}

static int xfer(struct u41_link *lnk, const uint8_t *tx, uint8_t *rx)
{
	struct spi_ioc_transfer tr;

	memset(&tr, 0, sizeof(tr));
	tr.tx_buf        = (unsigned long)tx;
	tr.rx_buf        = (unsigned long)rx;
	tr.len           = U41_FRAME_SIZE;
	tr.speed_hz      = lnk->speed_hz;
	tr.bits_per_word = 8;
	/*
	 * Deassert chip select between transfers.  The slave uses the NSS
	 * rising edge as its frame boundary, so holding CS across two frames
	 * would merge them into one oversized transaction that it discards.
	 */
	tr.cs_change     = 0;

	if (ioctl(lnk->fd, SPI_IOC_MESSAGE(1), &tr) < 0)
		return -errno;

	return 0;
}

static void build_frame(uint8_t *f, uint8_t cmd, uint8_t seq, uint32_t arg,
			const void *payload, uint8_t len)
{
	memset(f, 0, U41_FRAME_SIZE);

	f[U41_OFF_SOF]    = U41_SOF_REQ;
	f[U41_OFF_CMD]    = cmd;
	f[U41_OFF_SEQ]    = seq;
	f[U41_OFF_STATUS] = 0;
	f[U41_OFF_LEN]    = len;

	put_le32(&f[U41_OFF_ARG32], arg);

	if (payload && len)
		memcpy(&f[U41_OFF_PAYLOAD], payload, len);

	put_le32(&f[U41_OFF_CRC32], u41_crc32(f, U41_CRC_LEN));
}

/* Is this a well-formed response to sequence number `seq`? */
static int response_matches(const uint8_t *r, uint8_t seq)
{
	if (r[U41_OFF_SOF] != U41_SOF_RESP)
		return 0;
	if (r[U41_OFF_SEQ] != seq)
		return 0;
	if (get_le32(&r[U41_OFF_CRC32]) != u41_crc32(r, U41_CRC_LEN))
		return 0;

	return 1;
}

int u41_link_cmd(struct u41_link *lnk,
		 uint8_t cmd, uint32_t arg,
		 const void *payload, uint8_t payload_len,
		 void *resp_payload, uint8_t *resp_len,
		 unsigned guard_us)
{
	uint8_t tx[U41_FRAME_SIZE], rx[U41_FRAME_SIZE];
	uint8_t ping[U41_FRAME_SIZE];
	unsigned guard = guard_us ? guard_us : U41_GUARD_US;
	int attempt;
	int rc;

retry_command:
	{
		uint8_t seq = lnk->seq++;

		if (lnk->seq == 0)
			lnk->seq = 1;   /* 0 is never used, so it cannot alias */

		build_frame(tx, cmd, seq, arg, payload, payload_len);

		rc = xfer(lnk, tx, rx);
		if (rc < 0)
			return rc;

		sleep_us(guard);

		/*
		 * Clock PING frames until the answer to `seq` appears.  A PING
		 * has no side effects, so this is safe to repeat: it is purely
		 * a way to give the slave 80 more clocks to shift its response
		 * out on MISO.
		 */
		for (attempt = 0; attempt < U41_LINK_MAX_RETRIES; attempt++) {
			build_frame(ping, U41_CMD_PING, 0, 0, NULL, 0);

			rc = xfer(lnk, ping, rx);
			if (rc < 0)
				return rc;

			if (response_matches(rx, seq)) {
				int status = rx[U41_OFF_STATUS];

				if (status == U41_ST_BUSY) {
					/*
					 * The slave is inside a flash
					 * operation.  Back off for a whole
					 * burn time and re-issue the command;
					 * every command that can return BUSY
					 * is idempotent.
					 */
					sleep_us(U41_FLASH_GUARD_US);
					goto retry_command;
				}

				if (resp_len)
					*resp_len = rx[U41_OFF_LEN];

				if (resp_payload && rx[U41_OFF_LEN])
					memcpy(resp_payload,
					       &rx[U41_OFF_PAYLOAD],
					       rx[U41_OFF_LEN]);

				return status;
			}

			sleep_us(guard);
		}
	}

	return U41_ST_NO_RESPONSE;
}

const char *u41_status_str(int status)
{
	switch (status) {
	case U41_ST_OK:          return "ok";
	case U41_ST_BUSY:        return "busy";
	case U41_ST_BAD_CRC:     return "bad frame CRC";
	case U41_ST_BAD_SOF:     return "bad start-of-frame";
	case U41_ST_BAD_CMD:     return "unknown command";
	case U41_ST_BAD_ARG:     return "bad argument";
	case U41_ST_BAD_LEN:     return "bad length";
	case U41_ST_BAD_STATE:   return "command not valid in this state";
	case U41_ST_FLASH_ERR:   return "flash error";
	case U41_ST_VERIFY_FAIL: return "image verification failed";
	case U41_ST_NO_RESPONSE: return "no response from U41";
	default:                 return "unknown status";
	}
}

const char *u41_slot_str(unsigned slot)
{
	switch (slot) {
	case U41_SLOT_A:    return "A";
	case U41_SLOT_B:    return "B";
	case U41_SLOT_NONE: return "none";
	default:            return "?";
	}
}

const char *u41_boot_state_str(unsigned state)
{
	switch (state) {
	case U41_BOOT_IDLE:    return "IDLE";
	case U41_BOOT_PENDING: return "PENDING (awaiting power cycle)";
	case U41_BOOT_TRIAL:   return "TRIAL (awaiting confirm)";
	default:               return "?";
	}
}

const char *u41_ota_phase_str(unsigned phase)
{
	switch (phase) {
	case U41_OTA_IDLE:      return "idle";
	case U41_OTA_ERASING:   return "erasing";
	case U41_OTA_RECEIVING: return "receiving";
	case U41_OTA_VERIFYING: return "verifying";
	case U41_OTA_PENDING:   return "pending";
	case U41_OTA_ERROR:     return "error";
	default:                return "?";
	}
}

const char *u41_pm_state_str(unsigned state)
{
	switch (state) {
	case U41_PM_BOOT:       return "BOOT";
	case U41_PM_OFF:        return "OFF";
	case U41_PM_PRECHARGE:  return "PRECHARGE";
	case U41_PM_POWER_UP:   return "POWER_UP";
	case U41_PM_RUN:        return "RUN";
	case U41_PM_GRACEFUL:   return "GRACEFUL";
	case U41_PM_POWER_DOWN: return "POWER_DOWN";
	case U41_PM_FAULT:      return "FAULT";
	default:                return "?";
	}
}
