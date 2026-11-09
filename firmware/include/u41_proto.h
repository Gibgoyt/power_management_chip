/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * u41_proto.h - PM_SPI wire protocol between the i.MX8 SoM (master) and
 *               U41 / STM32L051 (slave).
 *
 * This header is shared verbatim by the firmware and by the Linux host tool
 * in yocto/u41/ota/.  Do not add anything here that is specific to either
 * side.
 *
 *
 * TRANSPORT
 * ---------
 * SPI mode 0 (CPOL=0, CPHA=0), MSB first, 8 bits per word, hardware NSS on
 * PA4.  U41 runs from the 16 MHz HSI; RM0377 requires the slave SCK to stay
 * at or below f_PCLK/4, so the master must not exceed 4 MHz.  The host tool
 * defaults to 1 MHz, which leaves plenty of margin and costs nothing on a
 * 14 KiB image.
 *
 *
 * DELAYED RESPONSE
 * ----------------
 * A command cannot be answered inside the transaction that carries it: the
 * slave has not seen the whole frame until the last byte has been shifted in.
 * Worse, U41 is a single-bank part - RM0377 3.3.1 says an erase or program
 * stalls every read of the NVM, including instruction fetch - so while a page
 * is being burned the CPU cannot service SPI at all.
 *
 * The protocol therefore works like this:
 *
 *     transaction N     host sends command N        slave shifts out the
 *                                                   response to command N-1
 *     ... host waits U41_GUARD_US, or longer if the last response said BUSY
 *     transaction N+1   host sends command N+1       slave shifts out the
 *                                                   response to command N
 *
 * To read the answer to a command with no follow-up work, the host issues
 * U41_CMD_PING and reads the response that comes back with it.  Every
 * response echoes the sequence byte of the command it answers, so the host
 * can always tell whether it is looking at a stale frame.
 *
 * If the slave has not finished the previous command, it answers U41_ST_BUSY
 * and the host retries.  No command is ever lost by retrying a PING.
 *
 *
 * FRAMING
 * -------
 * Both directions use the same fixed 80-byte frame.  Fixed size keeps the
 * slave ISR trivial and means the host can use a single full-duplex
 * SPI_IOC_MESSAGE.
 *
 *      offset  size  field
 *      ------  ----  -----------------------------------------------
 *           0     1  SOF     0xA5 request / 0x5A response
 *           1     1  CMD     u41_cmd_t, echoed in the response
 *           2     1  SEQ     host transaction counter, echoed
 *           3     1  STATUS  0 in requests, u41_status_t in responses
 *           4     1  LEN     payload bytes in use, 0..64
 *           5     3  RSVD    zero
 *           8     4  ARG32   command argument, little endian
 *          12    64  PAYLOAD
 *          76     4  CRC32   over bytes 0..75, little endian
 *
 * The 64-byte payload is not arbitrary: it is exactly one STM32L0 half-page,
 * the largest unit the flash controller programs in one go (RM0377 3.3.4), so
 * one full OTA frame maps to one program operation.
 */

#ifndef U41_PROTO_H
#define U41_PROTO_H

#include <stdint.h>

#define U41_PROTO_VERSION       1u

#define U41_SOF_REQ             0xA5u
#define U41_SOF_RESP            0x5Au

#define U41_FRAME_SIZE          80u
#define U41_PAYLOAD_MAX         64u

#define U41_OFF_SOF             0u
#define U41_OFF_CMD             1u
#define U41_OFF_SEQ             2u
#define U41_OFF_STATUS          3u
#define U41_OFF_LEN             4u
#define U41_OFF_RSVD            5u
#define U41_OFF_ARG32           8u
#define U41_OFF_PAYLOAD         12u
#define U41_OFF_CRC32           76u

/* Bytes covered by the trailing CRC32. */
#define U41_CRC_LEN             U41_OFF_CRC32

/*
 * Minimum host delay between the end of one transaction and the start of the
 * next, in microseconds.  Covers the slave's frame handling when no flash
 * operation is involved.
 */
#define U41_GUARD_US            300u

/*
 * Worst-case stall for a single flash operation, in microseconds.  RM0377
 * gives 3.94 ms typical for both a page erase and a half-page program on this
 * family; 8 ms is a comfortable ceiling.  The host waits this long after any
 * command that can touch flash before it polls.
 */
#define U41_FLASH_GUARD_US      8000u

/* Commands. */
typedef enum {
	/* No side effects.  Used to clock out the previous response. */
	U41_CMD_PING          = 0x01,
	/* Identity, running version, active slot and flash_seq. */
	U41_CMD_GET_INFO      = 0x02,
	/* Rail states, ignition, ADC readings. */
	U41_CMD_GET_PM_STATE  = 0x03,

	/* ARG32 = image size in bytes.
	 * payload[0..3]  = expected CRC32 of the image
	 * payload[4..7]  = firmware version
	 * Starts erasing the inactive slot; poll U41_CMD_OTA_STATUS. */
	U41_CMD_OTA_BEGIN     = 0x10,
	/* ARG32 = byte offset from the slot base, must be 64-byte aligned.
	 * LEN   = 1..64 payload bytes. */
	U41_CMD_OTA_DATA      = 0x11,
	/* Verify the written image and mark it PENDING. */
	U41_CMD_OTA_END       = 0x12,
	/* Discard an in-flight transfer. */
	U41_CMD_OTA_ABORT     = 0x13,
	/* Phase, bytes accepted, erase progress. */
	U41_CMD_OTA_STATUS    = 0x14,

	/* Confirm a TRIAL image so the bootloader stops counting attempts. */
	U41_CMD_CONFIRM       = 0x20,
	/* ARG32 = u41_rail_t, payload[0] = 0 off / 1 on. */
	U41_CMD_SET_RAIL      = 0x21,
	/* Feed the host watchdog without using the SOM_FB line. */
	U41_CMD_KEEPALIVE     = 0x22,
} u41_cmd_t;

/* Status codes returned in the STATUS byte. */
typedef enum {
	U41_ST_OK             = 0x00,
	U41_ST_BUSY           = 0x01,  /* still working, retry             */
	U41_ST_BAD_CRC        = 0x02,  /* frame CRC32 mismatch             */
	U41_ST_BAD_SOF        = 0x03,
	U41_ST_BAD_CMD        = 0x04,
	U41_ST_BAD_ARG        = 0x05,
	U41_ST_BAD_LEN        = 0x06,
	U41_ST_BAD_STATE      = 0x07,  /* command not legal right now      */
	U41_ST_FLASH_ERR      = 0x08,
	U41_ST_VERIFY_FAIL    = 0x09,  /* written image CRC did not match  */
	U41_ST_NO_RESPONSE    = 0x0A,  /* host-side only: nothing came back */
} u41_status_t;

/* Rails addressable through U41_CMD_SET_RAIL. */
typedef enum {
	U41_RAIL_WIFI     = 0,
	U41_RAIL_LTE      = 1,
	U41_RAIL_SCAP     = 2,
	U41_RAIL_PSE      = 3,
	/*
	 * SOM_PSU_EN is deliberately absent.  It is the rail the caller is
	 * running from; letting Linux switch it off over SPI would be a
	 * self-inflicted power cut with no way back.
	 */
} u41_rail_t;

/* OTA phases reported by U41_CMD_OTA_STATUS. */
typedef enum {
	U41_OTA_IDLE      = 0,
	U41_OTA_ERASING   = 1,
	U41_OTA_RECEIVING = 2,
	U41_OTA_VERIFYING = 3,
	U41_OTA_PENDING   = 4,  /* verified, waiting for the power cycle */
	U41_OTA_ERROR     = 5,
} u41_ota_phase_t;

#define U41_INFO_MAGIC   0x46313455UL   /* 'U41F' little endian */

/* Payload of a U41_CMD_GET_INFO response. */
typedef struct __attribute__((packed)) {
	uint32_t magic;          /* U41_INFO_MAGIC                        */
	uint8_t  proto_version;  /* U41_PROTO_VERSION                     */
	uint8_t  active_slot;    /* slot currently executing              */
	uint8_t  pending_slot;   /* slot awaiting a power cycle, or 0xFF  */
	uint8_t  boot_state;     /* U41_BOOT_* from u41_meta.h            */
	uint32_t flash_seq;      /* the iterating nonce, 0 after J12 flash */
	uint32_t fw_version;     /* running image version                 */
	uint32_t fw_crc32;       /* running image CRC32                   */
	uint32_t boot_version;   /* bootloader version                    */
	uint32_t uptime_ms;
	uint32_t reset_cause;    /* raw RCC_CSR at boot                   */
	uint8_t  boot_attempts;  /* TRIAL boots consumed                  */
	uint8_t  rsvd[3];
} u41_info_t;

_Static_assert(sizeof(u41_info_t) <= U41_PAYLOAD_MAX, "info payload too big");

/* Payload of a U41_CMD_GET_PM_STATE response. */
typedef struct __attribute__((packed)) {
	uint16_t input_mv;      /* INPUT_V, millivolts at the divider tap */
	uint16_t cap_mv;        /* CAP_V,   millivolts at the divider tap */
	uint8_t  pm_state;      /* u41_pm_state_t                        */
	uint8_t  ignition;      /* IGN_INP level                          */
	uint8_t  som_fb;        /* SOM_FB level                           */
	uint8_t  oc_pg;         /* OC_PG level                            */
	uint8_t  som_psu_en;
	uint8_t  wifi_psu_en;
	uint8_t  lte_psu_en;
	uint8_t  scap_en;
	uint8_t  pse_pwr_en;
	uint8_t  som_psu_pg;
	uint8_t  wifi_psu_pg;
	uint8_t  lte_psu_pg;
	uint32_t state_ms;      /* time in the current PM state           */
} u41_pm_status_t;

_Static_assert(sizeof(u41_pm_status_t) <= U41_PAYLOAD_MAX, "pm payload too big");

/* Payload of a U41_CMD_OTA_STATUS response. */
typedef struct __attribute__((packed)) {
	uint8_t  phase;         /* u41_ota_phase_t                        */
	uint8_t  target_slot;
	uint8_t  last_error;    /* u41_status_t of the failing step       */
	uint8_t  rsvd;
	uint32_t image_size;    /* as declared by OTA_BEGIN               */
	uint32_t bytes_written;
	uint32_t pages_erased;
	uint32_t pages_total;
} u41_ota_status_t;

_Static_assert(sizeof(u41_ota_status_t) <= U41_PAYLOAD_MAX, "ota payload too big");

/* Power-management states, shared so the host can print them. */
typedef enum {
	U41_PM_BOOT       = 0,  /* just out of reset, rails off           */
	U41_PM_OFF        = 1,  /* waiting for ignition                   */
	U41_PM_PRECHARGE  = 2,  /* charging the supercaps                 */
	U41_PM_POWER_UP   = 3,  /* SOM_PSU_EN asserted, waiting for PG    */
	U41_PM_RUN        = 4,  /* everything up, SOM_FB watchdog armed   */
	U41_PM_GRACEFUL   = 5,  /* ignition gone, giving Linux time       */
	U41_PM_POWER_DOWN = 6,  /* dropping rails in order                */
	U41_PM_FAULT      = 7,  /* OC_PG or a rail refused to come up     */
} u41_pm_state_t;

/* CRC32, IEEE 802.3, reflected, init 0xFFFFFFFF, final xor 0xFFFFFFFF. */
uint32_t u41_crc32(const void *data, uint32_t len);
uint32_t u41_crc32_update(uint32_t crc, const void *data, uint32_t len);
uint32_t u41_crc32_final(uint32_t crc);

#endif /* U41_PROTO_H */
