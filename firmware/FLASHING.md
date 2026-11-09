# Flashing U41 over J12 with an ST-Link

End-to-end guide for programming the STM32L051 (U41) with an ST-Link over the J12
SWD header — build the firmware, package it by commit hash, and flash it with the
interactive tool. **No STM32CubeProgrammer, no ST-Link GUI, no OpenOCD** — just the
`stlink` CLI (`st-info`/`st-flash`, v1.8.0+) and `arm-none-eabi-gcc`.

> J12 is the **only** way to program U41 from blank — the SoM has no SWD/reset to
> it (see [`../README.md`](../README.md) and
> [`../../../docs/stm32l051/j12/README.md`](../../../docs/stm32l051/j12/README.md)).
> After the bootloader is installed this way, the SoM can update the *application*
> over SPI (OTA) — see [`../../../docs/rio_4.04/OTA-VIABILITY.md`](../../../docs/rio_4.04/OTA-VIABILITY.md).

## 1. Build + package

```sh
cd yocto/u41/firmware
make            # builds build/u41-boot.bin, u41-app-a.bin, u41-app-b.bin
make package    # bundles them under yocto/firmware/flash_bins/<hash>/
```

`make package` creates a directory named by the git commit it was built from
(a dirty worktree gets a `-dirty` suffix), containing:

```
yocto/firmware/flash_bins/<hash>/
    u41-boot.bin      -> 0x08000000   (bootloader, 4 KB region)
    u41-app-a.bin     -> 0x08001000   (slot A, 14 KB region)
    u41-app-b.bin     -> 0x08004800   (slot B, 14 KB region)
    MANIFEST.txt      commit, build id, version, per-image size + flash address
    SHA256SUMS        checksums of the three .bin files
```

Only `MANIFEST.txt` and `SHA256SUMS` are tracked in git — the `.bin`s are
gitignored because they rebuild from source. This keeps a tiny, permanent in-git
record of every hash that was ever packaged, without committing binaries.

Both application slots are flashed with the **same** image, so the board starts
life with a known-good fallback already in slot B for the first OTA to roll back to.

## 2. Wire J12 to the ST-Link

Full pinout + the reset caveat: [`../../../docs/stm32l051/j12/README.md`](../../../docs/stm32l051/j12/README.md).

```
   J12                              ST-Link/V2
   1  PM_3V3   ──► VAPP / VTref    (SENSE ONLY — do not power the board from the probe)
   2  SWCLK    ──► SWCLK
   3  I_GND    ──► GND
   4  SWDIO    ──► SWDIO
   5  nRST     ──  leave UNCONNECTED  (DNP on this board — no reset line)
   6  n/c
```

- **Power the board externally first**, so `PM_3V3` (VTref) is live.
- **J12.5 nRST is unrouted** → the tools always `--hot-plug`, never
  `--connect-under-reset`.
- Prefer flashing with the **SoM off**: halting U41 freezes the rails it gates, so
  a halt with Linux up can hard-cut the SoM. U41 runs from `PM_3V3` (present on
  input power) regardless, so it can be programmed with the rest of the board down.

## 3. Flash

```sh
./toolchain/stm-link/probe.sh                       # read-only link check first
./toolchain/stm-link/flash-u41.sh \
    --image yocto/firmware/flash_bins/<hash> --backup
```

`flash-u41.sh` is interactive:

1. **Preflight** — `st-info --probe`; if the programmer is present but no target
   answers (chipid `0x000`), it prints the J12 wiring checklist and stops.
2. **Plan** — prints the bundle `MANIFEST.txt`, verifies `SHA256SUMS`, and shows
   the write targets and that it **mass-erases** first (clears RDP + all slots).
3. **`--backup`** (optional, non-blocking) — dumps the current flash + EEPROM +
   option bytes to `toolchain/sdimg/u41_firmware/` first; skipped (not aborted) if
   RDP blocks the read; refuses to overwrite an existing dump.
4. **Confirm** — type `YES` (or pass `--yes` for scripted runs).
5. **Write** — delegates to
   [`tools/flash-stlink.sh`](tools/flash-stlink.sh), which mass-erases, writes
   boot + slot A + slot B, and reads each back to verify.

`make flash-j12` is the shortcut for step 3 without the interactive front-end /
packaging (it builds and calls `flash-stlink.sh build/` directly).

## 4. Confirm

Power-cycle the board. From the SoM, once Linux is up:

```sh
u41-ota info        # expect: slot A, state IDLE, flash_seq 0
```

`flash_seq` is 0 after a J12 flash and increments only on a power cycle that adopts
a new OTA image — so it is the proof the flash + power-cycle took.

## See also
- [`../README.md`](../README.md) — the firmware/OTA overview and the hardware constraints
- [`../../../docs/stm32l051/j12/README.md`](../../../docs/stm32l051/j12/README.md) — J12 pinout, reset caveat, S4/BOOT0 recovery
- [`../../../toolchain/stm-link/README.md`](../../../toolchain/stm-link/README.md) — the ST-Link tool details
- [`../../../docs/rio_4.04/OTA-VIABILITY.md`](../../../docs/rio_4.04/OTA-VIABILITY.md) — J12 as the OTA trust anchor
