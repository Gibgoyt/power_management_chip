# U41 — power-management MCU firmware and OTA

U41 is the **STM32L051K6U6TR** on the Sedna RIO v4.04 carrier board. It owns
the board's power: ignition sensing, supercapacitor precharge, and the enable
lines for the i.MX8 SoM, WiFi and LTE rails.

This directory holds both halves of the story:

| Path | What it is |
|---|---|
| `firmware/` | Firmware for U41 itself — bootloader plus A/B application |
| `firmware/FLASHING.md` | **How to build, package, and flash U41 over J12 with an ST-Link** (`make package` → `flash-u41.sh`) |
| `ota/` | `u41-ota`, the Linux tool that updates U41 from the SoM |

For a plain-language summary of the flashing/OTA split, the ADC-averaging fix and
the hysteresis fix — written to answer review questions — see
[`docs/stm32l051/u41/ARCHITECTURE-NOTE.md`](../../docs/stm32l051/u41/ARCHITECTURE-NOTE.md).

---

## The hardware constraint everything follows from

Two facts from the RIO v4.04 schematic
(`toolchain/rio_board_schematics/rio_4.04/`) shape this entire design.

**1. The SoM has no reset and no SWD to U41.**

Tracing the extracted netlist, the nets `JTAG_M_TCK/SWCLK`,
`JTAG_M_TMS/SWDIO`, `JTAG_M_RESET`, `MCU_RESET` and `PM_RESET` appear only on
the `pwr_mgmt` sheet. `IMX8_Module.xml` does not contain any of them. The
only ports present on *both* sheets are `PM_SPI_CS`, `PM_SPI_SCK`,
`PM_SPI_MISO` and `PM_SPI_MOSI`.

So Linux can talk to U41 over SPI, and that is all. It cannot halt it, reset
it, or drop it into ST's built-in bootloader — `BOOT0` is on DIP switch S4
with a 10K pulldown (R188), not routed to J5.

**2. This part has no hardware bank swapping.**

The brief described "a hardware process that points our B program to
0x08000". That mechanism does not exist on this device. RM0377 §3.3.2 is
titled *"Bank swapping (category 5 devices only)"*, and the STM32L051K6 is a
32 Kbyte **category 3** part: single bank, 256 pages of 128 bytes, no BFB2
option bit. The core always starts at `0x08000000`.

**The A/B swap is therefore done in software**, by a bootloader that owns
`0x08000000`. The externally visible behaviour is exactly what was asked
for — write the inactive image, cycle power, come up on the new one — but the
mechanism is our own code, not silicon.

A third fact makes the RAM-resident flash driver mandatory. RM0377 §3.3.1:

> When the NVM features only one bank, it is not possible to read the NVM
> during a write/erase operation […] stalling the master that requested the
> read operation.

Instruction fetch is a read. An erase loop running from flash would stall
fetching its own next instruction, so everything in `common/flash.c` is placed
in `.ramfunc` and copied to SRAM at reset.

---

## Memory map

```
0x08000000 ┌─────────────────────────┐
           │ bootloader       4 KiB  │  owns the reset vector, never OTA'd
0x08001000 ├─────────────────────────┤
           │ application A   14 KiB  │
0x08004800 ├─────────────────────────┤
           │ application B   14 KiB  │
0x08008000 └─────────────────────────┘

0x08080000   data EEPROM      2 KiB
             +0x00  boot metadata record 0
             +0x20  boot metadata record 1   (redundant, newest valid wins)
```

Current usage — plenty of headroom:

```
bootloader     2588 /  4096 bytes  (63.2%)
slot A         6144 / 14336 bytes  (42.9%)
slot B         6144 / 14336 bytes  (42.9%)
```

Slot A and slot B are **separate builds of identical source**, differing only
in link address. Since the part cannot relocate code, each slot needs its own
binary. `u41-ota` picks the right one for you.

---

## Path 1 — flashing over J12 with an ST-Link

This is the factory and recovery path. It is the only way to recover a board
with both slots corrupt, and it cannot itself be broken by a bad update.

### Wiring

J12 is a 6-pin 2.54 mm header (PH1-06-UA):

| Pin | Net | ST-Link |
|---|---|---|
| 1 | `PM_3V3` | VAPP / target voltage sense |
| 2 | `JTAG_M_TCK/SWCLK` | SWCLK (U41 PA14, pin 24) |
| 3 | `I_GND` | GND |
| 4 | `JTAG_M_TMS/SWDIO` | SWDIO (U41 PA13, pin 23) |
| 5 | `JTAG_M_RESET` | NRST (U41 pin 4) |
| 6 | *(no net)* | — |

R192, R193 and R194 are all **DNP** in the BOM, so there are no pullups or
pulldowns fitted on these lines — the ST-Link drives them directly.

> **Do not power the board from the ST-Link's 3V3 pin.** `PM_3V3` feeds more
> than U41 and an ST-Link cannot source it. Connect J12.1 for sensing only.
> U41 runs from `PM_3V3`, which is present whenever the board has input power
> and does not depend on the rails U41 itself controls — so you can program it
> with the rest of the board off, which is the recommended way.

### Build and flash

```sh
cd firmware
make
make flash-j12
```

This mass-erases, then writes all three images and verifies each by reading
back. Both slots get the same firmware, so the board starts life with a
known-good fallback already in B — the very first OTA has something to roll
back to.

After a power cycle the board comes up on slot A with `flash_seq` at **0**.

---

## Path 2 — OTA from the i.MX8 SoM over J5

`PM_SPI_CS` is J5 pin 176 (SoM `GPIO2_IO03`). The bus is shared with
`GNSS_SPI_CS` (J5.174) and `I_SPI_CS` (J5.52), so each slave has its own GPIO
chip select.

### Device tree

`ota/dts/imx8qm-rio-u41.dtsi` adds the spidev node. Include it from
`meta-rio/recipes-kernel/linux/files/imx8qm-rio.dts` and remove the
`&lpspi1 { status = "disabled"; }` block that currently overrides it.

> **Two things need confirming on real hardware** and are flagged in that
> file: which LPSPI instance the SoM's `SPI3_*` connector pins map to, and the
> pad macro for the `GPIO2_IO03` chip select. Neither is determinable from the
> schematic alone. The CS pinctrl group is deliberately left empty rather than
> guessed — a wrong pad macro would mux some unrelated signal to GPIO.

### Updating

```sh
u41-ota info                      # note the current slot and flash_seq
u41-ota flash /path/to/firmware/build
                                  # writes whichever slot is inactive
# ---- POWER CYCLE THE UNIT ----
u41-ota info                      # flash_seq has incremented; new slot active
u41-ota confirm                   # make it permanent
```

`u41-ota flash` accepts a directory and picks `u41-app-a.bin` or
`u41-app-b.bin` to match the inactive slot. Give it a specific file and it
will refuse if that file targets the wrong slot.

### Why a power cycle, and why a reboot is not enough

`reboot` restarts Linux. U41 keeps running straight through it — it is a
separate MCU with its own power domain, and there is no reset line from the
SoM. The new image is only adopted by the U41 bootloader, which only runs
when U41 itself is reset. That means removing input power.

---

## The A/B state machine

Boot metadata lives in the data EEPROM as two redundant 32-byte records. A
write always targets the record with the *lower* sequence number, so the newer
record is never the one being modified — a power loss during a metadata write
always leaves one intact record behind.

```
   IDLE ──── OTA_END ────▶ PENDING
     ▲                        │
     │                   power cycle
     │                        │
     │                        ▼
     │                     TRIAL ──── confirm ────▶ IDLE
     │                        │
     └──── 3 boots, no confirm ┘   (roll back to the other slot)
```

- **`OTA_END`** verifies what is actually in flash — header, slot, load
  address and CRC32 — then marks the slot `PENDING`. It does **not** change
  the active slot. U41 keeps running the old image.
- **The next power cycle** is where the swap happens. The bootloader
  re-validates the pending image, adopts it, and marks it `TRIAL`.
- **`u41-ota confirm`** clears the trial. The `u41-ota-confirm.service`
  systemd unit does this automatically 30 seconds into `multi-user.target`.
- **No confirmation** after 3 power cycles rolls back to the previous slot.

A failure anywhere before `OTA_END` leaves the active slot untouched. The
worst case is a wasted erase.

### `flash_seq` — the update counter

This is the field to watch when testing.

- **0** after a J12/SWD flash.
- **+1 on every power cycle that adopts a new image** — and nowhere else.

It deliberately does *not* increment when an image is merely written. Seeing
it change is proof that both the write completed *and* the power cycle took
effect, which is precisely what the acceptance test needs to demonstrate.

The reset-to-zero on a J12 flash needs no special tooling: the bootloader
notices that the running slot's CRC no longer matches what the metadata
recorded, concludes the image arrived by some route other than OTA, and zeroes
the counter itself.

---

## Protocol

80-byte fixed frames over SPI mode 0, MSB first, 1 MHz default.

U41 runs from the 16 MHz HSI and its SPI block is a **slave**, so RM0377 caps
SCK at `f_PCLK/4` = 4 MHz. The tool warns if you ask for more.

The 64-byte payload is not arbitrary — it is exactly one STM32L0 half page,
the largest unit the flash controller programs in one operation, so one
`OTA_DATA` frame maps onto one program cycle.

**Responses are delayed by one transaction.** The slave cannot answer inside
the transaction carrying the question, and during a flash burn it cannot
service SPI at all. So the host sends a command, waits a guard time, then
clocks `PING` frames until the response with the matching sequence byte comes
back. `u41_link_cmd()` handles all of this.

`u41_proto.h` is the single source of truth and is compiled into both sides,
along with `crc32.c` — there is deliberately only one copy of each.

---

## Building

```sh
cd firmware && make          # bootloader + both slots, with a size report
cd ota      && make          # the host tool
cd ota      && make check    # protocol self-test, no hardware needed
```

`make check` verifies that the CRC matches Python's `binascii.crc32` (used by
`mkimage.py`), that the frame layout is intact, that the struct sizes still
fit a payload, and that the memory map closes exactly on 32 KiB. These are
the failures that would otherwise show up as an OTA that fails verification
after a minute of writing.

Requires `arm-none-eabi-gcc`, `python3`, and `stlink` for the J12 path.

---

## Layout

```
firmware/
├── include/          shared headers (layout, protocol, metadata, board map)
├── common/           startup, clocks, RAM-resident flash driver, CRC32
├── bootloader/       A/B slot selection and rollback
├── app/              power management, SPI slave, OTA receiver
├── tools/            mkimage.py, checksize.py, flash-stlink.sh
└── vendor/
    ├── cmsis/        ARM CMSIS 6 core headers (Apache-2.0)
    └── stm32l0/      ST CMSIS device headers for STM32L051 (Apache-2.0)

ota/
├── src/              u41-ota and the spidev transport
├── tests/            protocol self-test
├── dts/              device tree fragment for the PM_SPI bus
└── recipes/          Yocto recipe and systemd unit
```

---

## Open items

These need hardware or a decision, and are flagged in the code where they
matter:

1. **LPSPI instance and CS pad macro** — see `ota/dts/imx8qm-rio-u41.dtsi`.
2. **`SOM_FB` polarity.** The gpio-hog in `imx8qm-rio.dts` and its comment say
   high means alive; the `som-fb-keepalive_1.0.bb` DESCRIPTION in the same
   layer says the service drives it low. We follow the hog. If that is
   backwards, flip `U41_SOM_FB_ALIVE_LEVEL` in `firmware/include/u41_board.h`
   and nothing else changes.
3. **Voltage thresholds** in `firmware/include/u41_pm.h` are conservative
   starting values. `U41_PM_CAP_READY_MV` in particular should be set from a
   measured supercapacitor hold-up time.
4. **An unexpected U41 reset drops the SoM's power.** The bootloader puts all
   rails in a known-off state on every reset, including a watchdog reset. This
   is deliberate — a power manager that has lost its state should not keep
   holding rails up — but it does mean a U41 watchdog bite cold-starts the
   whole board. There is no way around it on this hardware: U41 is the only
   thing holding `SOM_PSU_EN`.
