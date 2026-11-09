# SPDX-License-Identifier: GPL-2.0+
SUMMARY = "U41 power-manager firmware update tool"
DESCRIPTION = "Command line tool that talks to the U41 STM32L051 power \
management MCU over the PM_SPI link (J5 pin 176, SoM GPIO2_IO03) and performs \
A/B firmware updates.  U41 writes the application slot it is not executing \
from, so the board stays powered for the whole update and the switch happens \
on the next power cycle.  Also reports rail state, input and supercapacitor \
voltages, and the flash_seq counter used to verify that a power cycle adopted \
a new image."
HOMEPAGE = "https://github.com/ooga-booga-1/yocto"

LICENSE = "GPL-2.0-or-later"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/GPL-2.0-or-later;md5=fed54355545ffd980b814dab4a3b312c"

FILESEXTRAPATHS:prepend := "${THISDIR}/files:"

# The whole u41 tree is staged as one directory, firmware/ and ota/ together.
#
# They are not separable: the host tool compiles u41_proto.h, u41_layout.h,
# u41_meta.h and crc32.c straight out of ../firmware, so that the device and
# the tool cannot end up disagreeing about a frame layout or a checksum.  The
# ota/Makefile finds them at ../firmware, which is exactly the layout in the
# source tree, so copying u41/ wholesale is both the simplest and the only
# arrangement that needs no path rewriting.
#
# Run recipes/install-to-meta-rio.sh to populate files/u41.
SRC_URI = " \
    file://u41/ \
    file://u41-ota-confirm.service \
"

S = "${WORKDIR}/u41/ota"

inherit systemd

# Confirms a TRIAL image once userspace has come up far enough to prove the
# new firmware is behaving.  Without it the bootloader rolls back after
# U41_MAX_TRIAL_BOOTS power cycles.
SYSTEMD_SERVICE:${PN} = "u41-ota-confirm.service"
SYSTEMD_AUTO_ENABLE = "enable"

EXTRA_OEMAKE = "'CC=${CC}' 'CFLAGS=${CFLAGS}' 'LDFLAGS=${LDFLAGS}'"

do_compile() {
    oe_runmake
}

# The self-test needs no hardware: it checks that the frame layout, the CRC
# convention and the flash memory map agree between this tool and the
# firmware.  Running it at build time catches a mismatch before an image can
# ever reach a board, which is much cheaper than discovering it as an OTA that
# fails verification after a minute of writing.
#
# It is built for the target, so only run it when we can actually execute the
# result - a cross build cannot.
addtask do_check after do_compile before do_install
do_check() {
    if [ "${HOST_ARCH}" = "${BUILD_ARCH}" ]; then
        oe_runmake check
    else
        bbnote "u41-ota: skipping protocol self-test (cross build, ${BUILD_ARCH} -> ${HOST_ARCH})."
        bbnote "u41-ota: run 'make check' in yocto/u41/ota on the build host instead."
    fi
}

do_install() {
    oe_runmake install DESTDIR=${D}

    install -d ${D}${systemd_system_unitdir}
    install -m 0644 ${WORKDIR}/u41-ota-confirm.service \
        ${D}${systemd_system_unitdir}/u41-ota-confirm.service

    # Ship the firmware images alongside the tool so an OTA can be performed
    # on a unit in the field with nothing else copied over.
    if [ -f ${WORKDIR}/u41/firmware/build/u41-app-a.bin ]; then
        install -d ${D}${datadir}/u41-firmware
        install -m 0644 ${WORKDIR}/u41/firmware/build/u41-app-*.bin \
            ${D}${datadir}/u41-firmware/
    fi

    install -d ${D}${docdir}/u41-ota
    install -m 0644 ${WORKDIR}/u41/README.md ${D}${docdir}/u41-ota/README.md
}

FILES:${PN} = " \
    ${bindir}/u41-ota \
    ${systemd_system_unitdir}/u41-ota-confirm.service \
    ${datadir}/u41-firmware \
    ${docdir}/u41-ota \
"

# spidev must exist for the tool to be useful; see dts/imx8qm-rio-u41.dtsi.
RRECOMMENDS:${PN} = "kernel-module-spidev"

# Only meaningful on the RIO carrier, which is the board that has U41 on it.
COMPATIBLE_MACHINE = "rio"
