#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0+
"""
checksize.py - report how much of each flash region is used.

32 Kbytes is not a lot to fit a bootloader and two application slots into, so
this prints the budget after every build and exits non-zero if anything has
overflowed.  The linker already errors on a region overflow; this catches the
subtler case of an image that fits but leaves no headroom for the next change.
"""

import argparse
import os
import sys

BOOT_SIZE = 4 * 1024
SLOT_SIZE = 14 * 1024
TOTAL = 32 * 1024

# Warn above this fraction of a region.
WARN_AT = 0.85


def report(name: str, path: str, limit: int) -> tuple[int, bool]:
    if not os.path.exists(path):
        print(f"  {name:<12} (not built)")
        return 0, False

    size = os.path.getsize(path)
    pct = 100.0 * size / limit
    free = limit - size

    if size > limit:
        flag = "OVERFLOW"
        bad = True
    elif pct >= WARN_AT * 100.0:
        flag = "tight"
        bad = False
    else:
        flag = ""
        bad = False

    print(f"  {name:<12} {size:6d} / {limit:5d} bytes  "
          f"({pct:5.1f}%, {free:5d} free) {flag}")

    return size, bad


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--boot", required=True)
    ap.add_argument("--app-a", required=True)
    ap.add_argument("--app-b", required=True)
    args = ap.parse_args()

    print("  flash budget (STM32L051K6: 32 KiB total)")

    boot, bad1 = report("bootloader", args.boot, BOOT_SIZE)
    a, bad2 = report("slot A", args.app_a, SLOT_SIZE)
    b, bad3 = report("slot B", args.app_b, SLOT_SIZE)

    print(f"  {'-' * 58}")
    print(f"  {'total':<12} {boot + a + b:6d} / {TOTAL:5d} bytes")
    print()

    if bad1 or bad2 or bad3:
        print("checksize: at least one image does not fit", file=sys.stderr)
        return 1

    # A and B are built from the same source; a size difference between them
    # means something is accidentally address-dependent and is worth knowing
    # about, because it is the kind of thing that makes only one slot work.
    if a and b and a != b:
        print(f"checksize: note - slot A and slot B images differ in size "
              f"({a} vs {b}). That is expected only if literal pools moved; "
              f"check the map files if it is large.", file=sys.stderr)

    return 0


if __name__ == "__main__":
    sys.exit(main())
