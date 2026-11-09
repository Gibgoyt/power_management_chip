#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0+
#
# install-to-meta-rio.sh - stage the u41 tree into meta-rio as a Yocto recipe.
#
# BitBake wants a recipe's sources underneath the recipe directory, but the
# working tree keeps them at yocto/u41/ where they are easy to build and test
# by hand.  This script bridges the two: it copies the whole u41 tree into
# meta-rio/recipes-u41/u41-ota/files/u41/ in the layout the recipe expects.
#
# firmware/ and ota/ are copied together and must stay siblings.  The host
# tool compiles u41_proto.h and crc32.c straight out of ../firmware so that
# the device and the tool cannot disagree about a frame layout or a checksum;
# splitting them would defeat that.
#
# Re-run this after changing anything under yocto/u41/.
#
# Usage:
#   ./install-to-meta-rio.sh [path-to-yocto-tree]

set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
u41_dir="$(cd "$here/../.." && pwd)"          # yocto/u41
yocto_dir="${1:-$(cd "$u41_dir/.." && pwd)}"  # yocto

layer="$yocto_dir/meta-rio"
recipe_dir="$layer/recipes-u41/u41-ota"
files_dir="$recipe_dir/files"

die() { printf 'install-to-meta-rio: %s\n' "$*" >&2; exit 1; }

[ -d "$layer" ] || die "no meta-rio layer at $layer"
[ -d "$u41_dir/firmware" ] || die "no firmware tree at $u41_dir/firmware"
[ -d "$u41_dir/ota" ] || die "no ota tree at $u41_dir/ota"

echo "source : $u41_dir"
echo "target : $recipe_dir"

mkdir -p "$files_dir"

# Wipe the staged copy rather than merge into it, so a file deleted upstream
# does not linger and get compiled forever.
rm -rf "$files_dir/u41"
mkdir -p "$files_dir/u41"

copy() {
	local rel="$1"
	mkdir -p "$files_dir/u41/$(dirname "$rel")"
	cp -a "$u41_dir/$rel" "$files_dir/u41/$rel"
}

copy README.md
copy firmware/include
copy firmware/common
copy firmware/app
copy firmware/bootloader
copy firmware/tools
copy firmware/vendor
copy firmware/Makefile
copy ota/src
copy ota/tests
copy ota/dts
copy ota/Makefile

# Prebuilt images, if the firmware has been built.  Optional: the recipe only
# ships them when present, and they are what lets a field unit run an OTA with
# nothing else copied over.
if [ -d "$u41_dir/firmware/build" ]; then
	mkdir -p "$files_dir/u41/firmware/build"
	for f in u41-boot.bin u41-app-a.bin u41-app-b.bin; do
		[ -f "$u41_dir/firmware/build/$f" ] && \
			cp -a "$u41_dir/firmware/build/$f" \
			      "$files_dir/u41/firmware/build/$f"
	done
	echo "note   : included prebuilt images from firmware/build"
else
	echo "note   : no firmware/build - run 'make' in firmware/ to ship images"
fi

# Drop build artefacts that would otherwise be baked into the recipe.
rm -rf "$files_dir/u41/ota/build"
find "$files_dir/u41" -name '*.o' -delete
find "$files_dir/u41" -name '*.d' -delete

cp -a "$here/u41-ota_1.0.bb" "$recipe_dir/"
cp -a "$here/files/u41-ota-confirm.service" "$files_dir/"

echo
echo "staged."
echo
cat <<EOF
Next steps:

  1. Add the recipe to your image, e.g. in a machine or image .bbappend:

         IMAGE_INSTALL:append:rio = " u41-ota"

  2. Wire up the device tree.  Add to
     meta-rio/recipes-kernel/linux/files/imx8qm-rio.dts:

         #include "imx8qm-rio-u41.dtsi"

     and DELETE the existing block, which would otherwise override it:

         &lpspi1 {
                 status = "disabled";
         };

     Copy the fragment in:

         cp $u41_dir/ota/dts/imx8qm-rio-u41.dtsi \\
            $layer/recipes-kernel/linux/files/

     and add it to SRC_URI in linux-imx_%.bbappend, plus an install line in
     do_patch:append:rio() next to the other DTSI files.

  3. Two items in that fragment need confirming against real hardware - the
     LPSPI instance and the chip-select pad macro.  They are documented at
     the top of imx8qm-rio-u41.dtsi.

  4. Run the protocol self-test on the build host (it cannot run during a
     cross build):

         cd $u41_dir/ota && make check
EOF
