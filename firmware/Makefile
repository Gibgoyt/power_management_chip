# SPDX-License-Identifier: GPL-2.0+
#
# U41 firmware - STM32L051K6U6TR power manager for the Sedna RIO v4.04.
#
# Produces three binaries:
#
#   build/u41-boot.bin    bootloader, linked at 0x08000000
#   build/u41-app-a.bin   application, linked at 0x08001000
#   build/u41-app-b.bin   application, linked at 0x08004800
#
# The two application binaries are built from identical source and differ only
# in their link address.  The part has no hardware bank swapping (it is a
# category 3 device; RM0377 3.3.2 restricts that to category 5), so an image
# cannot be relocated at run time - each slot needs its own build.
#
# Usage:
#   make                 build everything
#   make package         bundle the images under yocto/firmware/flash_bins/<hash>/
#                        (for toolchain/stm-link/flash-u41.sh --image <bundle>)
#   make flash-j12       full chip program over J12 with an ST-Link
#   make size            per-image flash and RAM usage
#   make clean

CROSS   ?= arm-none-eabi-
CC       = $(CROSS)gcc
OBJCOPY  = $(CROSS)objcopy
SIZE     = $(CROSS)size
PYTHON  ?= python3

BUILD    = build
TOOLS    = tools

# Bumped by hand.  Reported over U41_CMD_GET_INFO as fw_version.
VER_MAJOR ?= 1
VER_MINOR ?= 0
VER_PATCH ?= 0

# Stamped into the image header so a binary can be traced back to a build.
BUILD_ID ?= $(shell date -u +%s)

# BRING-UP ONLY: force ignition "on" so the power manager sequences the SoM even
# when PB8/IGN reads off on the bench (ignition front-end under investigation).
# Set to 0 (or unset) for a shipping build.  See
# docs/todos/bohemoth-001-get-u41-firmware-working.md.
U41_BRINGUP_IGNORE_IGNITION ?= 1

# Must match include/u41_layout.h.
BOOT_ORIGIN   = 0x08000000
BOOT_LENGTH   = 4K
SLOT_A_ORIGIN = 0x08001000
SLOT_B_ORIGIN = 0x08004800
SLOT_LENGTH   = 14K
IMAGE_HDR_OFF = 0x100

ARCH = -mcpu=cortex-m0plus -mthumb -mfloat-abi=soft

INCLUDES = -Iinclude -Ivendor/cmsis -Ivendor/stm32l0

CFLAGS = $(ARCH) $(INCLUDES) \
	-std=gnu11 \
	-Os \
	-g3 \
	-ffunction-sections -fdata-sections \
	-fno-common \
	-fno-builtin \
	-Wall -Wextra -Wshadow -Wundef \
	-Wno-unused-parameter \
	-DSTM32L051xx \
	-DU41_FW_VERSION="((($(VER_MAJOR))<<16)|(($(VER_MINOR))<<8)|($(VER_PATCH)))" \
	-DU41_BUILD_ID=$(BUILD_ID)UL \
	-DU41_BRINGUP_IGNORE_IGNITION=$(U41_BRINGUP_IGNORE_IGNITION)

LDFLAGS = $(ARCH) \
	-nostartfiles \
	-nostdlib \
	-Wl,--gc-sections \
	-Wl,--build-id=none \
	-Wl,--no-warn-rwx-segments

# libgcc supplies the Cortex-M0 helpers (32-bit division, mainly) that the
# compiler emits calls to.  -nostdlib drops it, so ask for it back explicitly.
LIBS = -lgcc

# ---------------------------------------------------------------- libc choice
#
# The firmware uses exactly two libc functions, memcpy and memset (nothing
# else - no malloc, printf or string ops).  Choose where they come from:
#
#   make LIBC=new    (default) newlib's libc.a from the toolchain.  This is the
#                    historical behaviour: even under -nostdlib the compiler
#                    emits memcpy/memset calls and the linker resolves them from
#                    newlib.  Defines USE_NEW_LIBC.
#
#   make LIBC=musl   the vendored musl memcpy.c/memset.c (our_libc/musl/), which
#                    are byte-for-byte upstream musl.  Defines USE_MUSL_LIBC.
#
#   make LIBC=our    our own memcpy/memset, hand-written in ARMv6-M assembly
#                    (our_libc/*.S).  Fully self-contained, no external libc at
#                    all.  Defines USE_OUR_LIBC.
#
# See our_libc/README.md.
LIBC ?= new

# LIBC_OBJ_* are extra objects folded into each link; LD_KEEP_LIBC controls
# whether the linker script keeps newlib's libc.a (only the 'new' branch does).
LD_KEEP_LIBC =

ifeq ($(LIBC),new)
  CFLAGS += -DUSE_NEW_LIBC
  # Link newlib for memcpy/memset.  -nostdlib drops the default libraries, so
  # ask for libc back explicitly; the linker script must also NOT discard it.
  # newlib's _sbrk/_write stubs are never referenced (we call no other libc),
  # so nothing else is pulled in - only memcpy.o and memset.o land.
  LD_KEEP_LIBC = -DU41_KEEP_LIBC
  LIBC_OBJ_A    =
  LIBC_OBJ_B    =
  LIBC_OBJ_BOOT =
  LIBS += -lc
else ifeq ($(LIBC),musl)
  CFLAGS += -DUSE_MUSL_LIBC
  LIBC_OBJ_A    = $(BUILD)/app_a/our_libc/musl/memcpy.o $(BUILD)/app_a/our_libc/musl/memset.o
  LIBC_OBJ_B    = $(BUILD)/app_b/our_libc/musl/memcpy.o $(BUILD)/app_b/our_libc/musl/memset.o
  LIBC_OBJ_BOOT = $(BUILD)/boot/our_libc/musl/memcpy.o  $(BUILD)/boot/our_libc/musl/memset.o
else ifeq ($(LIBC),our)
  CFLAGS += -DUSE_OUR_LIBC
  LIBC_OBJ_A    = $(BUILD)/app_a/our_libc/memcpy.o $(BUILD)/app_a/our_libc/memset.o
  LIBC_OBJ_B    = $(BUILD)/app_b/our_libc/memcpy.o $(BUILD)/app_b/our_libc/memset.o
  LIBC_OBJ_BOOT = $(BUILD)/boot/our_libc/memcpy.o  $(BUILD)/boot/our_libc/memset.o
else
  $(error LIBC must be one of: new, musl, our (got '$(LIBC)'))
endif

# Extra flags for the (unmodified) musl sources: put our minimal endian.h on the
# include path so musl's `#include <endian.h>` resolves in a -nostdlib build.
# <string.h> and <stdint.h> are freestanding and come from the toolchain.
MUSL_INC = -Iour_libc/musl

COMMON_SRC = \
	common/startup.c \
	common/sys.c \
	common/crc32.c \
	common/flash.c \
	common/meta.c

BOOT_SRC = $(COMMON_SRC) bootloader/main.c

APP_SRC = $(COMMON_SRC) \
	app/main.c \
	app/pm.c \
	app/spi_slave.c \
	app/ota.c

BOOT_OBJ  = $(patsubst %.c,$(BUILD)/boot/%.o,$(BOOT_SRC))
APP_A_OBJ = $(patsubst %.c,$(BUILD)/app_a/%.o,$(APP_SRC))
APP_B_OBJ = $(patsubst %.c,$(BUILD)/app_b/%.o,$(APP_SRC))

TARGETS = $(BUILD)/u41-boot.bin $(BUILD)/u41-app-a.bin $(BUILD)/u41-app-b.bin

.PHONY: all
all: $(TARGETS) size

# ---------------------------------------------------------------- linker

# One template, three scripts.  See common/u41.ld.in.
$(BUILD)/boot.ld: common/u41.ld.in | $(BUILD)
	$(CC) -E -P -x c -DU41_LD_ORIGIN=$(BOOT_ORIGIN) \
		-DU41_LD_LENGTH=$(BOOT_LENGTH) $(LD_KEEP_LIBC) $< -o $@

$(BUILD)/app_a.ld: common/u41.ld.in | $(BUILD)
	$(CC) -E -P -x c -DU41_LD_ORIGIN=$(SLOT_A_ORIGIN) \
		-DU41_LD_LENGTH=$(SLOT_LENGTH) \
		-DU41_LD_IMAGE_HEADER=$(IMAGE_HDR_OFF) $(LD_KEEP_LIBC) $< -o $@

$(BUILD)/app_b.ld: common/u41.ld.in | $(BUILD)
	$(CC) -E -P -x c -DU41_LD_ORIGIN=$(SLOT_B_ORIGIN) \
		-DU41_LD_LENGTH=$(SLOT_LENGTH) \
		-DU41_LD_IMAGE_HEADER=$(IMAGE_HDR_OFF) $(LD_KEEP_LIBC) $< -o $@

# ---------------------------------------------------------------- compile

$(BUILD)/boot/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -DU41_BUILD_BOOTLOADER=1 -MMD -MP -c $< -o $@

$(BUILD)/app_a/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -DU41_BUILD_SLOT=0 -MMD -MP -c $< -o $@

$(BUILD)/app_b/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -DU41_BUILD_SLOT=1 -MMD -MP -c $< -o $@

# our_libc assembly (LIBC=our).  The .S files are ARMv6-M Thumb; the C
# preprocessor runs first (capital .S), though these use no macros.
$(BUILD)/boot/our_libc/%.o: our_libc/%.S
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/app_a/our_libc/%.o: our_libc/%.S
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/app_b/our_libc/%.o: our_libc/%.S
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# Vendored musl sources (LIBC=musl), compiled unmodified with our_libc/musl on
# the include path (for endian.h).  -fno-builtin stays so the compiler does not
# turn the musl memcpy body into a call to itself.
$(BUILD)/boot/our_libc/musl/%.o: our_libc/musl/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(MUSL_INC) -MMD -MP -c $< -o $@

$(BUILD)/app_a/our_libc/musl/%.o: our_libc/musl/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(MUSL_INC) -MMD -MP -c $< -o $@

$(BUILD)/app_b/our_libc/musl/%.o: our_libc/musl/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(MUSL_INC) -MMD -MP -c $< -o $@

# ------------------------------------------------------------------ link

$(BUILD)/u41-boot.elf: $(BOOT_OBJ) $(LIBC_OBJ_BOOT) $(BUILD)/boot.ld
	$(CC) $(LDFLAGS) -T$(BUILD)/boot.ld -Wl,-Map=$(@:.elf=.map) \
		$(BOOT_OBJ) $(LIBC_OBJ_BOOT) $(LIBS) -o $@

$(BUILD)/u41-app-a.elf: $(APP_A_OBJ) $(LIBC_OBJ_A) $(BUILD)/app_a.ld
	$(CC) $(LDFLAGS) -T$(BUILD)/app_a.ld -Wl,-Map=$(@:.elf=.map) \
		$(APP_A_OBJ) $(LIBC_OBJ_A) $(LIBS) -o $@

$(BUILD)/u41-app-b.elf: $(APP_B_OBJ) $(LIBC_OBJ_B) $(BUILD)/app_b.ld
	$(CC) $(LDFLAGS) -T$(BUILD)/app_b.ld -Wl,-Map=$(@:.elf=.map) \
		$(APP_B_OBJ) $(LIBC_OBJ_B) $(LIBS) -o $@

# ------------------------------------------------------------------ image

# The bootloader owns the reset vector and is never OTA'd, so it needs no
# header and no CRC stamp.
$(BUILD)/u41-boot.bin: $(BUILD)/u41-boot.elf
	$(OBJCOPY) -O binary $< $@

$(BUILD)/u41-app-a.bin: $(BUILD)/u41-app-a.elf
	$(OBJCOPY) -O binary $< $(@:.bin=.raw)
	$(PYTHON) $(TOOLS)/mkimage.py --slot 0 --build-id $(BUILD_ID) \
		$(@:.bin=.raw) $@

$(BUILD)/u41-app-b.bin: $(BUILD)/u41-app-b.elf
	$(OBJCOPY) -O binary $< $(@:.bin=.raw)
	$(PYTHON) $(TOOLS)/mkimage.py --slot 1 --build-id $(BUILD_ID) \
		$(@:.bin=.raw) $@

$(BUILD):
	@mkdir -p $(BUILD)

# ------------------------------------------------------------------ misc

.PHONY: size
size: $(BUILD)/u41-boot.elf $(BUILD)/u41-app-a.elf $(BUILD)/u41-app-b.elf
	@echo
	@echo "  region sizes (text+data must fit the region, bss+stack must fit 8K SRAM)"
	@$(SIZE) $^
	@echo
	@$(PYTHON) $(TOOLS)/checksize.py \
		--boot $(BUILD)/u41-boot.bin \
		--app-a $(BUILD)/u41-app-a.bin \
		--app-b $(BUILD)/u41-app-b.bin

.PHONY: flash-j12
flash-j12: all
	$(TOOLS)/flash-stlink.sh $(BUILD)

# ---------------------------------------------------------------- packaging
#
# 'make package' bundles the three flashable binaries under a directory named by
# the git commit they were built from, at the repo-root path
#
#     yocto/firmware/flash_bins/<hash>/
#         u41-boot.bin  u41-app-a.bin  u41-app-b.bin   (the images)
#         MANIFEST.txt                                 (provenance, tracked)
#         SHA256SUMS                                   (checksums, tracked)
#
# The ST-Link flasher (toolchain/stm-link/flash-u41.sh --image <bundle>) consumes
# a bundle directly - it has exactly the three filenames flash-stlink.sh expects,
# so the flasher passes the bundle dir straight through to it.
#
# Only MANIFEST.txt + SHA256SUMS are tracked in git; the .bin files are gitignored
# because they are reproducible from source (same rule as build/).  '<hash>' is the
# short commit; a dirty worktree gets a '-dirty' suffix so an uncommitted build is
# never mistaken for a clean release.  Outside a git repo it becomes 'nogit'.

# Path from u41/firmware/ up to the yocto repo root, then firmware/flash_bins.
FLASH_BINS = ../../firmware/flash_bins

GIT_HASH := $(shell \
	h=$$(git rev-parse --short HEAD 2>/dev/null) || h=nogit; \
	if [ "$$h" != nogit ] && [ -n "$$(git status --porcelain 2>/dev/null)" ]; then \
		h="$$h-dirty"; \
	fi; \
	printf '%s' "$$h")

.PHONY: package
package: all
	@dst="$(FLASH_BINS)/$(GIT_HASH)"; \
	mkdir -p "$$dst"; \
	cp $(BUILD)/u41-boot.bin $(BUILD)/u41-app-a.bin $(BUILD)/u41-app-b.bin "$$dst/"; \
	{ \
		echo "# U41 (STM32L051K6) firmware bundle"; \
		echo "commit:     $(GIT_HASH)"; \
		echo "build_id:   $(BUILD_ID)"; \
		echo "version:    $(VER_MAJOR).$(VER_MINOR).$(VER_PATCH)"; \
		echo "packaged:   $$(date -u +%Y-%m-%dT%H:%M:%SZ)"; \
		echo; \
		echo "# image                addr           size(bytes)"; \
		printf 'u41-boot.bin          %s   %s\n'  "$(BOOT_ORIGIN)"   "$$(stat -c %s $(BUILD)/u41-boot.bin)"; \
		printf 'u41-app-a.bin         %s   %s\n'  "$(SLOT_A_ORIGIN)" "$$(stat -c %s $(BUILD)/u41-app-a.bin)"; \
		printf 'u41-app-b.bin         %s   %s\n'  "$(SLOT_B_ORIGIN)" "$$(stat -c %s $(BUILD)/u41-app-b.bin)"; \
	} > "$$dst/MANIFEST.txt"; \
	( cd "$$dst" && sha256sum u41-boot.bin u41-app-a.bin u41-app-b.bin > SHA256SUMS ); \
	echo "packaged -> $$dst"; \
	cat "$$dst/MANIFEST.txt"

# ------------------------------------------------------------- libc self-test
#
# Actually EXECUTE the memcpy/memset under test and compare against a longhand
# reference across every alignment and length.  Not a compile check: the shipped
# ARM assembly (LIBC=our) and the vendored musl C (LIBC=musl) really run.
#
# How: build a freestanding ARM *Linux* ELF (our_libc/test/runtime.c provides
# _start and raw-SVC write/exit, no libc) and run it under qemu-arm-static, which
# is Linux user-mode emulation and forwards the syscalls.  We use the same
# arm-none-eabi compiler and Cortex-M0+ Thumb as the firmware, so the exact code
# under test is what executes.
#
# -fno-builtin and -fno-tree-loop-distribute-patterns on the test itself stop the
# compiler from turning the reference byte loops into calls to the very memcpy/
# memset we are testing, which would make the comparison circular.
QEMU        ?= qemu-arm-static
TEST_CC     ?= $(CROSS)gcc
TEST_ARCH    = -mcpu=cortex-m0plus -mthumb -mfloat-abi=soft
TEST_CFLAGS  = $(TEST_ARCH) -Os -ffreestanding -nostdlib -fno-builtin \
	       -fno-tree-loop-distribute-patterns -Wall -Wextra
TEST_BUILD   = $(BUILD)/libc-test

# The test is built for both mem* providers via one canned rule.
define RUN_LIBC_TEST
	@mkdir -p $(TEST_BUILD)
	@$(TEST_CC) $(TEST_ARCH) -Os $(4) -c $(2) -o $(TEST_BUILD)/$(1)_memcpy.o
	@$(TEST_CC) $(TEST_ARCH) -Os $(4) -c $(3) -o $(TEST_BUILD)/$(1)_memset.o
	@$(TEST_CC) $(TEST_CFLAGS) -c our_libc/test/test_mem.c -o $(TEST_BUILD)/$(1)_test_mem.o
	@$(TEST_CC) $(TEST_CFLAGS) -c our_libc/test/runtime.c -o $(TEST_BUILD)/$(1)_runtime.o
	@$(TEST_CC) $(TEST_ARCH) -nostdlib -static -e _start \
		$(TEST_BUILD)/$(1)_test_mem.o $(TEST_BUILD)/$(1)_runtime.o \
		$(TEST_BUILD)/$(1)_memcpy.o $(TEST_BUILD)/$(1)_memset.o \
		-lgcc -o $(TEST_BUILD)/test_$(1)
	@$(QEMU) $(TEST_BUILD)/test_$(1)
endef

.PHONY: libc-check
libc-check:
	@command -v $(QEMU) >/dev/null 2>&1 || { \
		echo "libc-check: $(QEMU) not found; install qemu-user-static"; exit 1; }
	@echo "== LIBC=our (hand-written assembly) =="
	$(call RUN_LIBC_TEST,our,our_libc/memcpy.S,our_libc/memset.S,)
	@echo
	@echo "== LIBC=musl (vendored musl C) =="
	$(call RUN_LIBC_TEST,musl,our_libc/musl/memcpy.c,our_libc/musl/memset.c,$(MUSL_INC) -fno-builtin)

.PHONY: clean
clean:
	rm -rf $(BUILD)

-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)
