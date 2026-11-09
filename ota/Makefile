# SPDX-License-Identifier: GPL-2.0+
#
# u41-ota - Linux host tool for updating the U41 power manager over PM_SPI.
#
# Builds for the i.MX8 SoM (aarch64) under Yocto, and natively on a
# workstation for the protocol self-test.
#
# The protocol headers and the CRC implementation are pulled straight from the
# firmware tree rather than duplicated.  If the two sides ever disagreed about
# a frame layout or a checksum the failure would be maddening to debug, so
# there is deliberately only one copy of each.

CC      ?= gcc
PYTHON  ?= python3

FW_DIR  = ../firmware
BUILD   = build

CFLAGS  ?= -O2 -g
CFLAGS  += -std=gnu11 -Wall -Wextra -Wshadow -Wundef -Wno-unused-parameter
CFLAGS  += -I$(FW_DIR)/include -Isrc

LDFLAGS ?=

SRC = \
	src/u41-ota.c \
	src/u41_link.c \
	$(FW_DIR)/common/crc32.c

OBJ = $(patsubst %.c,$(BUILD)/%.o,$(notdir $(SRC)))

TEST_SRC = \
	tests/test_proto.c \
	src/u41_link.c \
	$(FW_DIR)/common/crc32.c

TEST_OBJ = $(patsubst %.c,$(BUILD)/test_%.o,$(notdir $(TEST_SRC)))

VPATH = src:tests:$(FW_DIR)/common

.PHONY: all
all: $(BUILD)/u41-ota

$(BUILD)/u41-ota: $(OBJ)
	$(CC) $(CFLAGS) $^ $(LDFLAGS) -o $@

$(BUILD)/%.o: %.c | $(BUILD)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/test_%.o: %.c | $(BUILD)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD):
	@mkdir -p $(BUILD)

# Runs on the build host, no hardware needed.
#   test_proto : frame layout, CRC and image-header maths agree with the firmware
#   test_hyst  : the input-rail hysteresis matches the design image (no chatter
#                in the band), exercising the exact u41_hysteresis() that ships
.PHONY: check
check: $(BUILD)/test_proto $(BUILD)/test_hyst
	./$(BUILD)/test_proto
	./$(BUILD)/test_hyst

$(BUILD)/test_proto: $(TEST_OBJ)
	$(CC) $(CFLAGS) $^ $(LDFLAGS) -o $@

# test_hyst is self-contained: just the test and the header-only helper.
$(BUILD)/test_hyst: $(BUILD)/test_test_hyst.o
	$(CC) $(CFLAGS) $^ $(LDFLAGS) -o $@

.PHONY: install
install: all
	install -d $(DESTDIR)/usr/bin
	install -m 0755 $(BUILD)/u41-ota $(DESTDIR)/usr/bin/u41-ota

.PHONY: clean
clean:
	rm -rf $(BUILD)

-include $(wildcard $(BUILD)/*.d)
