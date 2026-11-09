/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * spi_slave.c - SPI1 in slave mode on PA4..PA7.
 *
 * The i.MX8 is the master.  Bytes arrive in the SPI1 interrupt, which does
 * nothing but move a byte in and a byte out; all interpretation happens in
 * the main loop via u41_spi_poll().  Keeping the ISR that small matters
 * because the main loop periodically disappears into the flash controller for
 * ~4 ms at a time and we want the peripheral to keep up on either side of
 * that.
 *
 * Frame synchronisation uses NSS.  The master asserts it for exactly one
 * 80-byte frame, so a rising edge is an unambiguous frame boundary and a
 * desynchronised byte stream self-corrects on the next transaction rather
 * than staying broken forever.  Hardware NSS on PA4 handles the peripheral
 * side; we watch the pin's level for the framing.
 */

#include <string.h>

#include "stm32l0xx.h"
#include "u41_board.h"
#include "u41_spi.h"

static volatile uint8_t rx_buf[U41_FRAME_SIZE];
static volatile uint8_t tx_buf[U41_FRAME_SIZE];
static volatile uint16_t rx_len;
static volatile uint16_t tx_pos;
static volatile uint8_t frame_ready;
static volatile uint8_t nss_was_low;

static uint8_t work[U41_FRAME_SIZE];

static uint32_t frame_crc(const uint8_t *f)
{
	return u41_crc32(f, U41_CRC_LEN);
}

static void put_le32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v);
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

void u41_spi_init(void)
{
	RCC->IOPENR  |= RCC_IOPENR_GPIOAEN;
	RCC->APB2ENR |= RCC_APB2ENR_SPI1EN;

	/*
	 * PA4 NSS, PA5 SCK, PA6 MISO, PA7 MOSI - all AF0 on this part.
	 * MISO is the only one we drive; the rest are inputs from the master.
	 */
	u41_gpio_af(U41_SPI_PORT, U41_SPI_NSS_PIN,  U41_SPI_AF);
	u41_gpio_af(U41_SPI_PORT, U41_SPI_SCK_PIN,  U41_SPI_AF);
	u41_gpio_af(U41_SPI_PORT, U41_SPI_MISO_PIN, U41_SPI_AF);
	u41_gpio_af(U41_SPI_PORT, U41_SPI_MOSI_PIN, U41_SPI_AF);

	/* MISO needs to keep up with a 4 MHz clock; the rest do not matter. */
	u41_gpio_ospeed(U41_SPI_PORT, U41_SPI_MISO_PIN, 3u);

	/*
	 * Idle NSS is high (the master's chip select is active low), so pull
	 * it up.  Without this an unconnected or tri-stated master would leave
	 * the peripheral permanently selected.
	 */
	u41_gpio_pull(U41_SPI_PORT, U41_SPI_NSS_PIN, 1u);

	SPI1->CR1 = 0;
	SPI1->CR2 = 0;

	/*
	 * Slave, mode 0, MSB first, 8-bit, hardware NSS.
	 *
	 * The STM32L051 carries the classic SPI block, not the newer one with
	 * a configurable DS field and FRXTH: frame size is the single DFF bit
	 * and 8 bits is DFF clear.  So the whole configuration is "leave CR1
	 * at zero except for SPE":
	 *
	 *   MSTR = 0        slave
	 *   CPOL = 0        mode 0, matching the host tool
	 *   CPHA = 0
	 *   LSBFIRST = 0    MSB first
	 *   DFF = 0         8-bit frames
	 *   SSM = 0         use the real NSS pin, not a software bit
	 */

	/* Preload something harmless so the first transaction is not garbage. */
	memset((void *)tx_buf, 0, sizeof(tx_buf));
	tx_buf[U41_OFF_SOF]    = U41_SOF_RESP;
	tx_buf[U41_OFF_STATUS] = U41_ST_BUSY;
	put_le32(&((uint8_t *)tx_buf)[U41_OFF_CRC32],
		 frame_crc((const uint8_t *)tx_buf));

	rx_len      = 0;
	tx_pos      = 0;
	frame_ready = 0;
	nss_was_low = 0;

	SPI1->CR2 |= SPI_CR2_RXNEIE;
	SPI1->CR1 |= SPI_CR1_SPE;

	NVIC_SetPriority(SPI1_IRQn, 0);   /* highest: it must not miss a byte */
	NVIC_EnableIRQ(SPI1_IRQn);
}

void SPI1_IRQHandler(void)
{
	uint32_t sr = SPI1->SR;

	if ((sr & SPI_SR_OVR) != 0UL) {
		/*
		 * We fell behind - almost certainly because the main loop was
		 * stalled inside a flash operation.  Clear the flag and drop
		 * the partial frame; the host will retry after its guard time.
		 */
		(void)SPI1->DR;
		(void)SPI1->SR;
		rx_len = 0;
		tx_pos = 0;
		return;
	}

	if ((sr & SPI_SR_RXNE) == 0UL)
		return;

	{
		uint8_t b = *(volatile uint8_t *)&SPI1->DR;

		if (rx_len < U41_FRAME_SIZE)
			rx_buf[rx_len++] = b;
	}

	/*
	 * Load the next outgoing byte.  Once the staged response is exhausted
	 * we send 0x00, which the host recognises as "no response yet"
	 * because the SOF will not match.
	 */
	if ((SPI1->SR & SPI_SR_TXE) != 0UL) {
		uint8_t out = (tx_pos < U41_FRAME_SIZE) ? tx_buf[tx_pos] : 0x00u;

		if (tx_pos < U41_FRAME_SIZE)
			tx_pos++;

		*(volatile uint8_t *)&SPI1->DR = out;
	}
}

int u41_spi_poll(uint8_t *frame)
{
	int nss_high = (U41_GPIO_GET(U41_SPI_PORT, U41_SPI_NSS_PIN) != 0UL);

	/*
	 * Frame boundary detection.  We only look at a buffer once NSS has
	 * gone low (transaction started) and come back high (transaction
	 * finished), which is what makes a truncated or over-long transfer
	 * recoverable instead of permanently shifting the byte stream.
	 */
	if (!nss_high) {
		nss_was_low = 1;
		return 0;
	}

	if (!nss_was_low)
		return 0;

	nss_was_low = 0;

	{
		uint32_t primask = __get_PRIMASK();
		uint16_t len;

		__disable_irq();
		len = rx_len;
		if (len == U41_FRAME_SIZE)
			memcpy(work, (const void *)rx_buf, U41_FRAME_SIZE);
		rx_len = 0;
		tx_pos = 0;
		__set_PRIMASK(primask);

		if (len != U41_FRAME_SIZE)
			return 0;
	}

	if (work[U41_OFF_SOF] != U41_SOF_REQ) {
		u41_spi_reply_status(work[U41_OFF_CMD], work[U41_OFF_SEQ],
				     U41_ST_BAD_SOF);
		return 0;
	}

	{
		uint32_t want = (uint32_t)work[U41_OFF_CRC32]
			      | ((uint32_t)work[U41_OFF_CRC32 + 1] << 8)
			      | ((uint32_t)work[U41_OFF_CRC32 + 2] << 16)
			      | ((uint32_t)work[U41_OFF_CRC32 + 3] << 24);

		if (want != frame_crc(work)) {
			u41_spi_reply_status(work[U41_OFF_CMD],
					     work[U41_OFF_SEQ],
					     U41_ST_BAD_CRC);
			return 0;
		}
	}

	if (work[U41_OFF_LEN] > U41_PAYLOAD_MAX) {
		u41_spi_reply_status(work[U41_OFF_CMD], work[U41_OFF_SEQ],
				     U41_ST_BAD_LEN);
		return 0;
	}

	memcpy(frame, work, U41_FRAME_SIZE);
	frame_ready = 1;

	return 1;
}

void u41_spi_reply(uint8_t cmd, uint8_t seq, uint8_t status,
		   const void *payload, uint8_t len)
{
	uint8_t tmp[U41_FRAME_SIZE];
	uint32_t primask;

	if (len > U41_PAYLOAD_MAX)
		len = U41_PAYLOAD_MAX;

	memset(tmp, 0, sizeof(tmp));
	tmp[U41_OFF_SOF]    = U41_SOF_RESP;
	tmp[U41_OFF_CMD]    = cmd;
	tmp[U41_OFF_SEQ]    = seq;
	tmp[U41_OFF_STATUS] = status;
	tmp[U41_OFF_LEN]    = len;

	if (payload != 0 && len > 0u)
		memcpy(&tmp[U41_OFF_PAYLOAD], payload, len);

	put_le32(&tmp[U41_OFF_CRC32], frame_crc(tmp));

	/*
	 * Swap the staged buffer in with interrupts off.  The ISR reads
	 * tx_buf byte by byte, so a partial update could send the first half
	 * of one response and the second half of another - which would fail
	 * the host's CRC check, but noisily and confusingly.
	 */
	primask = __get_PRIMASK();
	__disable_irq();
	memcpy((void *)tx_buf, tmp, U41_FRAME_SIZE);
	tx_pos = 0;
	__set_PRIMASK(primask);

	frame_ready = 0;
}

void u41_spi_reply_status(uint8_t cmd, uint8_t seq, uint8_t status)
{
	u41_spi_reply(cmd, seq, status, 0, 0);
}
