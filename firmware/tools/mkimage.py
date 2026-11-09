#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0+
"""
mkimage.py - stamp image_size and image_crc32 into a U41 application binary.

The image header sits at a fixed offset inside the image and the CRC covers
the header itself, so the checksum cannot be computed at compile time.  This
runs over objcopy's output and fills in the two fields the linker could not.

The CRC is taken over the whole padded image with the four bytes of
image_crc32 read as zero.  bootloader/main.c and app/ota.c compute it exactly
the same way; if you change the convention here, change it in all three.

The image is padded up to a 64-byte boundary because 64 bytes is the STM32L0
half-page, the unit the OTA path programs in.  Erased flash on this part reads
as 0x00, so zero padding is what is already there and the CRC over the padded
length still matches what the device reads back.
"""

import argparse
import binascii
import struct
import sys

# Must match include/u41_layout.h.
IMAGE_MAGIC = 0x49313455
HDR_VERSION = 1
HDR_OFFSET = 0x100
HDR_SIZE = 32

SLOT_A_BASE = 0x08001000
SLOT_B_BASE = 0x08004800
SLOT_SIZE = 14 * 1024

HALFPAGE = 64

# uint32 magic, uint16 hdr_version, uint16 slot, uint32 load_addr,
# uint32 image_size, uint32 image_crc32, uint32 fw_version,
# uint32 build_id, uint32 reserved
HDR_FMT = "<IHHIIIIII"

CRC_FIELD_OFFSET = HDR_OFFSET + 16


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input", help="raw binary from objcopy -O binary")
    ap.add_argument("output", help="stamped binary")
    ap.add_argument("--slot", type=int, choices=(0, 1), required=True,
                    help="0 = A, 1 = B")
    ap.add_argument("--build-id", type=lambda s: int(s, 0), default=0)
    args = ap.parse_args()

    with open(args.input, "rb") as fh:
        img = bytearray(fh.read())

    if len(img) < HDR_OFFSET + HDR_SIZE:
        print(f"mkimage: {args.input} is only {len(img)} bytes, too small to "
              f"contain a header at 0x{HDR_OFFSET:x}", file=sys.stderr)
        return 1

    # Pad to a whole half page so the OTA transfer is a round number of
    # programming operations.
    if len(img) % HALFPAGE:
        img.extend(b"\x00" * (HALFPAGE - (len(img) % HALFPAGE)))

    if len(img) > SLOT_SIZE:
        print(f"mkimage: image is {len(img)} bytes, slot is {SLOT_SIZE}",
              file=sys.stderr)
        return 1

    fields = list(struct.unpack_from(HDR_FMT, img, HDR_OFFSET))
    magic, hdr_version, slot, load_addr = fields[0], fields[1], fields[2], fields[3]

    if magic != IMAGE_MAGIC:
        print(f"mkimage: no image header at 0x{HDR_OFFSET:x} "
              f"(found 0x{magic:08x}, expected 0x{IMAGE_MAGIC:08x}). "
              f"Is .image_header being placed by the linker script?",
              file=sys.stderr)
        return 1

    if hdr_version != HDR_VERSION:
        print(f"mkimage: header version {hdr_version}, expected {HDR_VERSION}",
              file=sys.stderr)
        return 1

    expect_base = SLOT_B_BASE if args.slot == 1 else SLOT_A_BASE

    # Catch a build linked for one slot but stamped for the other.  The
    # bootloader would reject it at runtime anyway; failing here is a much
    # cheaper way to find out.
    if slot != args.slot:
        print(f"mkimage: header says slot {slot} but --slot {args.slot} "
              f"was requested", file=sys.stderr)
        return 1

    if load_addr != expect_base:
        print(f"mkimage: header load_addr is 0x{load_addr:08x}, expected "
              f"0x{expect_base:08x} for slot {args.slot}", file=sys.stderr)
        return 1

    size = len(img)

    fields[4] = size   # image_size
    fields[5] = 0      # image_crc32, zeroed for the CRC pass
    fields[7] = args.build_id
    struct.pack_into(HDR_FMT, img, HDR_OFFSET, *fields)

    crc = binascii.crc32(bytes(img)) & 0xFFFFFFFF

    struct.pack_into("<I", img, CRC_FIELD_OFFSET, crc)

    with open(args.output, "wb") as fh:
        fh.write(img)

    slot_name = "B" if args.slot == 1 else "A"
    print(f"mkimage: slot {slot_name} @ 0x{expect_base:08x}  "
          f"size {size} bytes ({100.0 * size / SLOT_SIZE:.1f}% of slot)  "
          f"crc32 0x{crc:08x}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
