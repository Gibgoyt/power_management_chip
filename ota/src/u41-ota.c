/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * u41-ota - update the U41 power-management MCU from Linux on the i.MX8 SoM.
 *
 *
 * WHAT THIS CAN AND CANNOT DO
 * ---------------------------
 * The only wire between the SoM and U41 is PM_SPI: four signals, CS on J5 pin
 * 176 (SoM GPIO2_IO03).  SWD and NRST go to J12 and nowhere else, and BOOT0
 * is on DIP switch S4, so from here there is no way to halt U41, reset it, or
 * drop it into ST's built-in bootloader.  Everything has to go through
 * firmware that is already running.
 *
 * That is exactly why the firmware is A/B.  This tool writes the slot U41 is
 * NOT executing from.  The running image keeps the board powered throughout -
 * including the rail this process is running on - and nothing switches over
 * until the next power cycle.
 *
 *
 * UPDATE FLOW
 * -----------
 *      u41-ota info                     see the current slot and flash_seq
 *      u41-ota flash u41-app-b.bin      write the inactive slot, mark PENDING
 *      (power cycle the unit)
 *      u41-ota info                     flash_seq has incremented, new slot
 *      u41-ota confirm                  clear the trial, make it permanent
 *
 * If the new image never confirms, the bootloader rolls back after
 * U41_MAX_TRIAL_BOOTS power cycles.
 *
 *
 * WHICH IMAGE TO SEND
 * -------------------
 * There are two application binaries because the part cannot relocate code -
 * it is a category 3 STM32L0 with no bank swapping (RM0377 3.3.2), so slot A
 * and slot B need separate builds.  Pass the directory containing both and
 * this tool picks the right one, or name the exact file and it will refuse if
 * it targets the wrong slot.
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/stat.h>

#include "u41_layout.h"
#include "u41_link.h"
#include "u41_meta.h"
#include "u41_proto.h"

#define DEFAULT_SPIDEV  "/dev/spidev0.0"

/* Give a slot erase this long before deciding it has stalled. */
#define ERASE_TIMEOUT_S  30

static const char *g_dev = DEFAULT_SPIDEV;
static uint32_t g_speed;
static int g_verbose;

static void put_le32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v);
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

static void usage(FILE *out)
{
	fprintf(out,
"Usage: u41-ota [options] <command> [args]\n"
"\n"
"Commands:\n"
"  info                  show slot, boot state, flash_seq and version\n"
"  pm                    show power-management state, rails and voltages\n"
"  status                show OTA transfer state\n"
"  flash <path>          write firmware to the inactive slot\n"
"                        <path> may be a .bin or a directory holding\n"
"                        u41-app-a.bin and u41-app-b.bin\n"
"  confirm               confirm a TRIAL image so it becomes permanent\n"
"  abort                 discard a staged or in-flight update\n"
"  rail <name> <on|off>  wifi | lte | scap | pse\n"
"  keepalive             feed the SOM_FB watchdog once\n"
"\n"
"Options:\n"
"  -d, --device <dev>    spidev node (default %s)\n"
"  -s, --speed <hz>      SPI clock (default %u, U41 cannot exceed 4000000)\n"
"  -v, --verbose\n"
"  -h, --help\n",
		DEFAULT_SPIDEV, U41_LINK_DEFAULT_HZ);
}

static int open_link(struct u41_link *lnk)
{
	int rc = u41_link_open(lnk, g_dev, g_speed);

	if (rc < 0) {
		fprintf(stderr, "u41-ota: cannot open %s: %s\n",
			g_dev, strerror(-rc));
		if (rc == -ENOENT)
			fprintf(stderr,
"        No spidev node.  The PM_SPI bus needs a spidev child in the device\n"
"        tree with cs-gpios pointing at PM_SPI_CS (SoM GPIO2_IO03, J5.176).\n"
"        See yocto/u41/ota/dts/.\n");
		return rc;
	}

	lnk->verbose = g_verbose;

	if (g_verbose)
		fprintf(stderr, "u41-ota: %s at %u Hz\n", g_dev, lnk->speed_hz);

	return 0;
}

static int cmd_info(struct u41_link *lnk)
{
	u41_info_t info;
	uint8_t len = 0;
	int st;

	memset(&info, 0, sizeof(info));

	st = u41_link_cmd(lnk, U41_CMD_GET_INFO, 0, NULL, 0,
			  &info, &len, 0);
	if (st != U41_ST_OK) {
		fprintf(stderr, "u41-ota: GET_INFO failed: %s\n",
			u41_status_str(st));
		return 1;
	}

	if (len < sizeof(info) || info.magic != U41_INFO_MAGIC) {
		fprintf(stderr,
			"u41-ota: bad info payload (len %u, magic 0x%08x)\n",
			len, info.magic);
		return 1;
	}

	printf("U41 power manager\n");
	printf("  protocol      %u\n", info.proto_version);
	printf("  running slot  %s\n", u41_slot_str(info.active_slot));
	printf("  pending slot  %s\n", u41_slot_str(info.pending_slot));
	printf("  boot state    %s\n", u41_boot_state_str(info.boot_state));
	printf("  boot attempts %u\n", info.boot_attempts);
	/*
	 * The acceptance-test counter.  0 straight after a J12 flash, then one
	 * increment per power cycle that adopted a new image.
	 */
	printf("  flash_seq     %u\n", info.flash_seq);
	printf("  fw version    %u.%u.%u\n",
	       U41_VERSION_MAJOR(info.fw_version),
	       U41_VERSION_MINOR(info.fw_version),
	       U41_VERSION_PATCH(info.fw_version));
	printf("  fw crc32      0x%08x\n", info.fw_crc32);
	printf("  uptime        %u.%03u s\n",
	       info.uptime_ms / 1000u, info.uptime_ms % 1000u);
	printf("  reset cause   0x%08x (raw RCC_CSR)\n", info.reset_cause);

	return 0;
}

static int cmd_pm(struct u41_link *lnk)
{
	u41_pm_status_t st;
	uint8_t len = 0;
	int rc;

	memset(&st, 0, sizeof(st));

	rc = u41_link_cmd(lnk, U41_CMD_GET_PM_STATE, 0, NULL, 0,
			  &st, &len, 0);
	if (rc != U41_ST_OK) {
		fprintf(stderr, "u41-ota: GET_PM_STATE failed: %s\n",
			u41_status_str(rc));
		return 1;
	}

	printf("Power management\n");
	printf("  state         %s (%u.%03u s)\n",
	       u41_pm_state_str(st.pm_state),
	       st.state_ms / 1000u, st.state_ms % 1000u);
	printf("  input         %u.%03u V\n",
	       st.input_mv / 1000u, st.input_mv % 1000u);
	printf("  supercap      %u.%03u V\n",
	       st.cap_mv / 1000u, st.cap_mv % 1000u);
	printf("  ignition      %s\n", st.ignition ? "on" : "off");
	printf("  SOM_FB        %s\n", st.som_fb ? "alive" : "not alive");
	printf("  OC_PG         %s\n", st.oc_pg ? "ok" : "FAULT");
	printf("  rails         SOM %s/%s  WiFi %s/%s  LTE %s/%s\n",
	       st.som_psu_en  ? "en" : "--", st.som_psu_pg  ? "pg" : "--",
	       st.wifi_psu_en ? "en" : "--", st.wifi_psu_pg ? "pg" : "--",
	       st.lte_psu_en  ? "en" : "--", st.lte_psu_pg  ? "pg" : "--");
	printf("  scap charger  %s\n", st.scap_en    ? "on" : "off");
	printf("  PSE power     %s\n", st.pse_pwr_en ? "on" : "off");

	return 0;
}

static int fetch_ota_status(struct u41_link *lnk, u41_ota_status_t *out)
{
	uint8_t len = 0;
	int st;

	memset(out, 0, sizeof(*out));

	st = u41_link_cmd(lnk, U41_CMD_OTA_STATUS, 0, NULL, 0, out, &len, 0);
	if (st != U41_ST_OK)
		return st;

	return U41_ST_OK;
}

static int cmd_status(struct u41_link *lnk)
{
	u41_ota_status_t st;
	int rc = fetch_ota_status(lnk, &st);

	if (rc != U41_ST_OK) {
		fprintf(stderr, "u41-ota: OTA_STATUS failed: %s\n",
			u41_status_str(rc));
		return 1;
	}

	printf("OTA\n");
	printf("  phase         %s\n", u41_ota_phase_str(st.phase));
	printf("  target slot   %s\n", u41_slot_str(st.target_slot));
	printf("  last error    %s\n", u41_status_str(st.last_error));
	printf("  image size    %u\n", st.image_size);
	printf("  written       %u\n", st.bytes_written);
	printf("  erased        %u / %u pages\n",
	       st.pages_erased, st.pages_total);

	return 0;
}

/* Read a whole file.  Returns a malloc'd buffer, or NULL. */
static uint8_t *read_file(const char *path, size_t *out_len)
{
	FILE *fh = fopen(path, "rb");
	uint8_t *buf;
	long len;

	if (!fh) {
		fprintf(stderr, "u41-ota: %s: %s\n", path, strerror(errno));
		return NULL;
	}

	if (fseek(fh, 0, SEEK_END) != 0 || (len = ftell(fh)) < 0) {
		fprintf(stderr, "u41-ota: %s: cannot determine size\n", path);
		fclose(fh);
		return NULL;
	}
	rewind(fh);

	buf = malloc((size_t)len);
	if (!buf) {
		fclose(fh);
		return NULL;
	}

	if (fread(buf, 1, (size_t)len, fh) != (size_t)len) {
		fprintf(stderr, "u41-ota: %s: short read\n", path);
		free(buf);
		fclose(fh);
		return NULL;
	}

	fclose(fh);
	*out_len = (size_t)len;

	return buf;
}

/*
 * Validate an image locally before sending a single byte of it.
 *
 * The device checks all of this again after the transfer, but doing it here
 * turns "the update failed after 40 seconds of writing" into an immediate,
 * specific error message.
 */
static int check_image(const uint8_t *img, size_t len, unsigned want_slot)
{
	const u41_image_hdr_t *hdr;
	uint32_t expect_base;

	if (len < U41_IMAGE_HDR_OFFSET + sizeof(*hdr)) {
		fprintf(stderr, "u41-ota: image is only %zu bytes\n", len);
		return -1;
	}

	if (len > U41_SLOT_SIZE) {
		fprintf(stderr,
			"u41-ota: image is %zu bytes, a slot holds %lu\n",
			len, (unsigned long)U41_SLOT_SIZE);
		return -1;
	}

	if (len % U41_FLASH_HALFPAGE_SIZE) {
		fprintf(stderr,
			"u41-ota: image length %zu is not a multiple of %lu.\n"
			"        Run it through firmware/tools/mkimage.py.\n",
			len, (unsigned long)U41_FLASH_HALFPAGE_SIZE);
		return -1;
	}

	hdr = (const u41_image_hdr_t *)(img + U41_IMAGE_HDR_OFFSET);

	if (hdr->magic != U41_IMAGE_MAGIC) {
		fprintf(stderr,
			"u41-ota: no image header at 0x%lx (found 0x%08x).\n"
			"        This does not look like a U41 image.\n",
			(unsigned long)U41_IMAGE_HDR_OFFSET, hdr->magic);
		return -1;
	}

	if (hdr->slot != want_slot) {
		fprintf(stderr,
			"u41-ota: image is built for slot %s but slot %s is the\n"
			"        inactive one.  Send the other binary.\n",
			u41_slot_str(hdr->slot), u41_slot_str(want_slot));
		return -1;
	}

	expect_base = (want_slot == U41_SLOT_B) ? U41_SLOT_B_BASE
						: U41_SLOT_A_BASE;

	if (hdr->load_addr != expect_base) {
		fprintf(stderr,
			"u41-ota: image load address is 0x%08x, expected 0x%08x\n",
			hdr->load_addr, expect_base);
		return -1;
	}

	if (hdr->image_size != len) {
		fprintf(stderr,
			"u41-ota: header says %u bytes, file is %zu\n",
			hdr->image_size, len);
		return -1;
	}

	/* Recompute the CRC the same way the device will. */
	{
		uint32_t zero = 0;
		uint32_t crc;
		size_t before = U41_IMAGE_CRC_FIELD_OFFSET;

		crc = u41_crc32_update(0xFFFFFFFFu, img, before);
		crc = u41_crc32_update(crc, &zero, 4);
		crc = u41_crc32_update(crc, img + before + 4,
				       (uint32_t)(len - before - 4));
		crc = u41_crc32_final(crc);

		if (crc != hdr->image_crc32) {
			fprintf(stderr,
				"u41-ota: image CRC mismatch: header says "
				"0x%08x, computed 0x%08x\n",
				hdr->image_crc32, crc);
			return -1;
		}
	}

	return 0;
}

/*
 * Resolve the path the user gave us to the binary for `slot`.
 *
 * A directory is the convenient case: point at the firmware build directory
 * and let the tool pick, since which slot is inactive is something only the
 * device knows.
 */
static char *resolve_image_path(const char *path, unsigned slot)
{
	struct stat sb;
	char *out;
	size_t n;

	if (stat(path, &sb) != 0) {
		fprintf(stderr, "u41-ota: %s: %s\n", path, strerror(errno));
		return NULL;
	}

	if (!S_ISDIR(sb.st_mode))
		return strdup(path);

	n = strlen(path) + 32;
	out = malloc(n);
	if (!out)
		return NULL;

	snprintf(out, n, "%s/u41-app-%c.bin", path,
		 (slot == U41_SLOT_B) ? 'b' : 'a');

	if (stat(out, &sb) != 0) {
		fprintf(stderr, "u41-ota: %s: %s\n", out, strerror(errno));
		free(out);
		return NULL;
	}

	return out;
}

static int wait_for_erase(struct u41_link *lnk)
{
	time_t deadline = time(NULL) + ERASE_TIMEOUT_S;
	u41_ota_status_t st;
	uint32_t last_reported = UINT32_MAX;

	for (;;) {
		int rc = fetch_ota_status(lnk, &st);

		if (rc != U41_ST_OK) {
			fprintf(stderr, "u41-ota: OTA_STATUS failed: %s\n",
				u41_status_str(rc));
			return -1;
		}

		if (st.phase == U41_OTA_ERROR) {
			fprintf(stderr, "u41-ota: erase failed: %s\n",
				u41_status_str(st.last_error));
			return -1;
		}

		if (st.phase == U41_OTA_RECEIVING)
			break;

		if (st.phase != U41_OTA_ERASING) {
			fprintf(stderr,
				"u41-ota: unexpected phase '%s' during erase\n",
				u41_ota_phase_str(st.phase));
			return -1;
		}

		if (st.pages_erased != last_reported) {
			printf("\r  erasing slot %s: %u / %u pages",
			       u41_slot_str(st.target_slot),
			       st.pages_erased, st.pages_total);
			fflush(stdout);
			last_reported = st.pages_erased;
		}

		if (time(NULL) > deadline) {
			/*
			 * The erase only advances while the power manager is
			 * in RUN, so a stall here usually means the board is
			 * sequencing rails or shutting down rather than that
			 * the flash is broken.
			 */
			fprintf(stderr,
"\nu41-ota: erase stalled at %u/%u pages.\n"
"        The device only erases while power management is in RUN.\n"
"        Check 'u41-ota pm'.\n",
				st.pages_erased, st.pages_total);
			return -1;
		}

		usleep(20000);
	}

	printf("\r  erasing slot %s: %u / %u pages  done\n",
	       u41_slot_str(st.target_slot), st.pages_total, st.pages_total);

	return 0;
}

static int cmd_flash(struct u41_link *lnk, const char *path)
{
	u41_info_t info;
	u41_ota_status_t ost;
	uint8_t len = 0;
	uint8_t begin_payload[8];
	unsigned target;
	char *img_path = NULL;
	uint8_t *img = NULL;
	size_t img_len = 0;
	const u41_image_hdr_t *hdr;
	size_t offset;
	int st;
	int ret = 1;

	memset(&info, 0, sizeof(info));

	st = u41_link_cmd(lnk, U41_CMD_GET_INFO, 0, NULL, 0, &info, &len, 0);
	if (st != U41_ST_OK || info.magic != U41_INFO_MAGIC) {
		fprintf(stderr, "u41-ota: cannot read device info: %s\n",
			u41_status_str(st));
		return 1;
	}

	/* The device writes the slot it is not running from; mirror that. */
	target = (info.active_slot == U41_SLOT_A) ? U41_SLOT_B : U41_SLOT_A;

	printf("Running slot %s, will write slot %s.\n",
	       u41_slot_str(info.active_slot), u41_slot_str(target));

	img_path = resolve_image_path(path, target);
	if (!img_path)
		return 1;

	img = read_file(img_path, &img_len);
	if (!img)
		goto out;

	if (check_image(img, img_len, target) != 0)
		goto out;

	hdr = (const u41_image_hdr_t *)(img + U41_IMAGE_HDR_OFFSET);

	printf("Image %s: %zu bytes, version %u.%u.%u, crc32 0x%08x\n",
	       img_path, img_len,
	       U41_VERSION_MAJOR(hdr->fw_version),
	       U41_VERSION_MINOR(hdr->fw_version),
	       U41_VERSION_PATCH(hdr->fw_version),
	       hdr->image_crc32);

	/* Clear anything left over from an interrupted attempt. */
	(void)u41_link_cmd(lnk, U41_CMD_OTA_ABORT, 0, NULL, 0, NULL, NULL, 0);

	put_le32(&begin_payload[0], hdr->image_crc32);
	put_le32(&begin_payload[4], hdr->fw_version);

	st = u41_link_cmd(lnk, U41_CMD_OTA_BEGIN, (uint32_t)img_len,
			  begin_payload, sizeof(begin_payload),
			  NULL, NULL, U41_FLASH_GUARD_US);
	if (st != U41_ST_OK) {
		fprintf(stderr, "u41-ota: OTA_BEGIN rejected: %s\n",
			u41_status_str(st));
		goto out;
	}

	if (wait_for_erase(lnk) != 0)
		goto out;

	for (offset = 0; offset < img_len; offset += U41_FLASH_HALFPAGE_SIZE) {
		size_t chunk = img_len - offset;

		if (chunk > U41_FLASH_HALFPAGE_SIZE)
			chunk = U41_FLASH_HALFPAGE_SIZE;

		st = u41_link_cmd(lnk, U41_CMD_OTA_DATA, (uint32_t)offset,
				  img + offset, (uint8_t)chunk,
				  NULL, NULL, U41_FLASH_GUARD_US);
		if (st != U41_ST_OK) {
			fprintf(stderr,
				"\nu41-ota: write failed at offset %zu: %s\n",
				offset, u41_status_str(st));
			goto out;
		}

		if ((offset % 1024) == 0 || (offset + chunk) >= img_len) {
			printf("\r  writing: %zu / %zu bytes (%.0f%%)",
			       offset + chunk, img_len,
			       100.0 * (double)(offset + chunk) / (double)img_len);
			fflush(stdout);
		}
	}

	printf("\n");

	st = u41_link_cmd(lnk, U41_CMD_OTA_END, 0, NULL, 0, NULL, NULL,
			  U41_FLASH_GUARD_US);
	if (st != U41_ST_OK) {
		fprintf(stderr, "u41-ota: OTA_END failed: %s\n",
			u41_status_str(st));
		(void)fetch_ota_status(lnk, &ost);
		fprintf(stderr, "        phase %s, last error %s\n",
			u41_ota_phase_str(ost.phase),
			u41_status_str(ost.last_error));
		goto out;
	}

	printf("\n");
	printf("Slot %s written and verified, staged as PENDING.\n",
	       u41_slot_str(target));
	printf("\n");
	printf("Nothing has changed yet: U41 is still running slot %s and\n",
	       u41_slot_str(info.active_slot));
	printf("still holding the board's power rails.\n");
	printf("\n");
	printf("POWER CYCLE THE UNIT to switch over.  A soft reboot of Linux\n");
	printf("is not enough - U41 keeps running across that, and there is no\n");
	printf("reset line from the SoM to U41 on this board.\n");
	printf("\n");
	printf("After the power cycle:\n");
	printf("  u41-ota info      flash_seq should read %u (was %u)\n",
	       info.flash_seq + 1u, info.flash_seq);
	printf("  u41-ota confirm   make the new image permanent\n");

	ret = 0;

out:
	free(img);
	free(img_path);

	return ret;
}

static int cmd_confirm(struct u41_link *lnk)
{
	int st = u41_link_cmd(lnk, U41_CMD_CONFIRM, 0, NULL, 0, NULL, NULL, 0);

	if (st != U41_ST_OK) {
		fprintf(stderr, "u41-ota: confirm failed: %s\n",
			u41_status_str(st));
		return 1;
	}

	printf("Confirmed.  The running image is now permanent.\n");

	return 0;
}

static int cmd_abort(struct u41_link *lnk)
{
	int st = u41_link_cmd(lnk, U41_CMD_OTA_ABORT, 0, NULL, 0, NULL, NULL,
			      U41_FLASH_GUARD_US);

	if (st != U41_ST_OK) {
		fprintf(stderr, "u41-ota: abort failed: %s\n",
			u41_status_str(st));
		return 1;
	}

	printf("Aborted.  Any staged image has been withdrawn.\n");

	return 0;
}

static int cmd_rail(struct u41_link *lnk, const char *name, const char *state)
{
	uint8_t on;
	uint32_t rail;
	int st;

	if (!strcmp(name, "wifi"))
		rail = U41_RAIL_WIFI;
	else if (!strcmp(name, "lte"))
		rail = U41_RAIL_LTE;
	else if (!strcmp(name, "scap"))
		rail = U41_RAIL_SCAP;
	else if (!strcmp(name, "pse"))
		rail = U41_RAIL_PSE;
	else {
		fprintf(stderr,
			"u41-ota: unknown rail '%s' (wifi, lte, scap, pse)\n",
			name);
		/*
		 * SOM is not in the list on purpose: it is the rail this
		 * process is running from.
		 */
		if (!strcmp(name, "som"))
			fprintf(stderr,
"        The SOM rail is not switchable over SPI - it powers the SoM this\n"
"        command is running on, so turning it off could not be undone.\n");
		return 1;
	}

	if (!strcmp(state, "on"))
		on = 1;
	else if (!strcmp(state, "off"))
		on = 0;
	else {
		fprintf(stderr, "u41-ota: expected 'on' or 'off'\n");
		return 1;
	}

	st = u41_link_cmd(lnk, U41_CMD_SET_RAIL, rail, &on, 1, NULL, NULL, 0);
	if (st != U41_ST_OK) {
		fprintf(stderr, "u41-ota: set rail failed: %s\n",
			u41_status_str(st));
		return 1;
	}

	printf("%s rail %s\n", name, on ? "on" : "off");

	return 0;
}

static int cmd_keepalive(struct u41_link *lnk)
{
	int st = u41_link_cmd(lnk, U41_CMD_KEEPALIVE, 0, NULL, 0, NULL, NULL, 0);

	if (st != U41_ST_OK) {
		fprintf(stderr, "u41-ota: keepalive failed: %s\n",
			u41_status_str(st));
		return 1;
	}

	return 0;
}

int main(int argc, char **argv)
{
	struct u41_link lnk;
	const char *cmd;
	int i;
	int ret;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-d") || !strcmp(argv[i], "--device")) {
			if (++i >= argc)
				goto bad_args;
			g_dev = argv[i];
		} else if (!strcmp(argv[i], "-s") || !strcmp(argv[i], "--speed")) {
			if (++i >= argc)
				goto bad_args;
			g_speed = (uint32_t)strtoul(argv[i], NULL, 0);
			if (g_speed > 4000000u)
				fprintf(stderr,
"u41-ota: warning: %u Hz exceeds the 4 MHz ceiling for a 16 MHz STM32L0\n"
"        slave (RM0377 requires SCK <= f_PCLK/4).  Expect frame errors.\n",
					g_speed);
		} else if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--verbose")) {
			g_verbose = 1;
		} else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
			usage(stdout);
			return 0;
		} else {
			break;
		}
	}

	if (i >= argc)
		goto bad_args;

	cmd = argv[i++];

	if (open_link(&lnk) < 0)
		return 1;

	if (!strcmp(cmd, "info")) {
		ret = cmd_info(&lnk);
	} else if (!strcmp(cmd, "pm")) {
		ret = cmd_pm(&lnk);
	} else if (!strcmp(cmd, "status")) {
		ret = cmd_status(&lnk);
	} else if (!strcmp(cmd, "flash")) {
		if (i >= argc) {
			fprintf(stderr, "u41-ota: flash needs a path\n");
			ret = 1;
		} else {
			ret = cmd_flash(&lnk, argv[i]);
		}
	} else if (!strcmp(cmd, "confirm")) {
		ret = cmd_confirm(&lnk);
	} else if (!strcmp(cmd, "abort")) {
		ret = cmd_abort(&lnk);
	} else if (!strcmp(cmd, "rail")) {
		if (i + 1 >= argc) {
			fprintf(stderr, "u41-ota: rail needs a name and on/off\n");
			ret = 1;
		} else {
			ret = cmd_rail(&lnk, argv[i], argv[i + 1]);
		}
	} else if (!strcmp(cmd, "keepalive")) {
		ret = cmd_keepalive(&lnk);
	} else {
		fprintf(stderr, "u41-ota: unknown command '%s'\n", cmd);
		usage(stderr);
		ret = 1;
	}

	u41_link_close(&lnk);

	return ret;

bad_args:
	usage(stderr);
	return 1;
}
