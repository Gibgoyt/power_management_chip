/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * u41_ota.h - receives a firmware image into the inactive slot.
 */

#ifndef U41_OTA_H
#define U41_OTA_H

#include <stdint.h>
#include "u41_meta.h"
#include "u41_proto.h"

/* Hand the OTA layer the metadata the application loaded at startup. */
void u41_ota_init(const u41_bootmeta_t *meta);

/*
 * Do a slice of background work - currently one page erase per call.
 *
 * Erasing a 14 KiB slot is 112 pages at ~3.9 ms each, about 440 ms in total.
 * Doing that in one go would stall the power manager for the whole time, so
 * it is spread across main loop iterations instead.  Returns without touching
 * flash unless u41_pm_flash_window_open() agrees it is safe.
 */
void u41_ota_poll(void);

/* Command handlers.  Each returns a u41_status_t. */
uint8_t u41_ota_begin(uint32_t image_size, uint32_t crc32, uint32_t version);
uint8_t u41_ota_data(uint32_t offset, const uint8_t *data, uint8_t len);
uint8_t u41_ota_end(void);
uint8_t u41_ota_abort(void);
void    u41_ota_status(u41_ota_status_t *out);

/* Clear a TRIAL image's probation.  Returns a u41_status_t. */
uint8_t u41_ota_confirm(void);

/* Current view of the boot metadata, for U41_CMD_GET_INFO. */
const u41_bootmeta_t *u41_ota_meta(void);

#endif /* U41_OTA_H */
