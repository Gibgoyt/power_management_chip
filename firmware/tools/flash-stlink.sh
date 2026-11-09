#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0+
#
# flash-stlink.sh - program U41 over J12 with an ST-Link.
#
#
# WIRING
# ------
# J12 is a 6-pin 2.54 mm header (PH1-06-UA) on the pwr_mgmt sheet:
#
#     J12.1  PM_3V3              -> ST-Link VAPP / target voltage sense
#     J12.2  JTAG_M_TCK/SWCLK    -> ST-Link SWCLK   (U41 PA14)
#     J12.3  I_GND               -> ST-Link GND
#     J12.4  JTAG_M_TMS/SWDIO    -> ST-Link SWDIO   (U41 PA13)
#     J12.5  JTAG_M_RESET        -> ST-Link NRST    (U41 NRST, pin 4)
#     J12.6  (no net)
#
# R192, R193 and R194 are all marked DNP in the BOM, so there are no pullups
# or pulldowns fitted on SWDIO, RESET or SWCLK - the ST-Link drives them
# directly.  NRST is also driven by U42, a TPS3808G30 supervisor, and pulled
# up by R191 10K.
#
# This is the only path that can recover a board with both application slots
# corrupt.  The SoM cannot do it: SWD and NRST are not routed to J5.
#
#
# POWER
# -----
# U41 runs from PM_3V3, which is present whenever the board has input power -
# it does not depend on the rails U41 itself controls.  So the target can be
# programmed with the rest of the board off, and that is the recommended way
# to do it: no SoM running, nothing to interrupt.
#
# Do not power the board from the ST-Link's 3V3 pin.  PM_3V3 feeds more than
# U41 and an ST-Link cannot source it.  Connect J12.1 for voltage sensing
# only.
#
#
# WHAT GETS WRITTEN
# -----------------
#   0x08000000  u41-boot.bin    bootloader
#   0x08001000  u41-app-a.bin   application, slot A
#   0x08004800  u41-app-b.bin   application, slot B
#
# Both slots are written with the same firmware.  That means the board starts
# life with a known-good fallback already in B, so the very first OTA has
# something to roll back to.
#
# flash_seq is not touched here.  The bootloader notices that the slot CRC no
# longer matches what the metadata recorded and resets the counter to 0 by
# itself, which is what makes "J12 flash reads 0, then OTA counts up" work
# without this script having to write EEPROM.

set -euo pipefail

BUILD_DIR="${1:-build}"

BOOT_ADDR=0x08000000
SLOT_A_ADDR=0x08001000
SLOT_B_ADDR=0x08004800

BOOT_BIN="$BUILD_DIR/u41-boot.bin"
APP_A_BIN="$BUILD_DIR/u41-app-a.bin"
APP_B_BIN="$BUILD_DIR/u41-app-b.bin"

die() { printf 'flash-stlink: %s\n' "$*" >&2; exit 1; }

for f in "$BOOT_BIN" "$APP_A_BIN" "$APP_B_BIN"; do
	[ -f "$f" ] || die "missing $f - run 'make' first"
done

command -v st-flash >/dev/null 2>&1 || \
	die "st-flash not found (Arch: pacman -S stlink)"

# Refuse to write an oversized image rather than let st-flash spill one region
# into the next.
check_size() {
	local f="$1" limit="$2" name="$3"
	local sz
	sz=$(stat -c %s "$f")
	[ "$sz" -le "$limit" ] || \
		die "$name is $sz bytes, region is only $limit"
}

check_size "$BOOT_BIN"  4096  "bootloader"
check_size "$APP_A_BIN" 14336 "slot A image"
check_size "$APP_B_BIN" 14336 "slot B image"

echo "== target"
st-info --probe || die "no ST-Link found, or the target is not responding"

echo
echo "== mass erase"
# Clears both slots and the bootloader.  Data EEPROM is deliberately left
# alone: st-flash does not touch it, and the bootloader's CRC check handles
# resetting flash_seq.
st-flash erase

echo
echo "== bootloader -> $BOOT_ADDR"
st-flash --reset write "$BOOT_BIN" "$BOOT_ADDR"

echo
echo "== slot A -> $SLOT_A_ADDR"
st-flash --reset write "$APP_A_BIN" "$SLOT_A_ADDR"

echo
echo "== slot B -> $SLOT_B_ADDR"
st-flash --reset write "$APP_B_BIN" "$SLOT_B_ADDR"

echo
echo "== verify"
for spec in "$BOOT_BIN:$BOOT_ADDR" "$APP_A_BIN:$SLOT_A_ADDR" "$APP_B_BIN:$SLOT_B_ADDR"; do
	f="${spec%:*}"
	addr="${spec##*:}"
	sz=$(stat -c %s "$f")
	tmp=$(mktemp)
	st-flash read "$tmp" "$addr" "$sz" >/dev/null 2>&1
	if cmp -s "$f" "$tmp"; then
		echo "  ok   $(basename "$f") @ $addr"
	else
		rm -f "$tmp"
		die "verify failed for $(basename "$f") @ $addr"
	fi
	rm -f "$tmp"
done

cat <<'EOF'

== done

Power cycle the board.  U41 will come up on slot A with flash_seq back at 0.

Confirm from the SoM once Linux is up:

    u41-ota info

Expect:  slot A, state IDLE, flash_seq 0.
EOF
