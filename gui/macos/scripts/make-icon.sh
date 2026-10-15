#!/bin/sh
# Lace macOS frontend
# Resources/Lace.icns from the GTK frontend's SVG icon (one icon for both
# apps). Needs rsvg-convert (brew install librsvg); iconutil and sips are
# macOS's. The result is committed, so a build needs none of this.
#
# Usage: scripts/make-icon.sh
#
# (c) iloveyou, 2025. MIT License.
# https://github.com/stychos/lace

set -eu
cd "$(dirname "$0")/.."
svg=../gtk/resources/icons/scalable/apps/dev.hexed.lace.svg
set=$(mktemp -d /tmp/lace-icon.XXXXXX)/Lace.iconset
mkdir -p "$set"
# macOS icons leave a margin around the shape: the SVG already keeps 8 of
# 128 on each side, which is the Apple grid's margin
for size in 16 32 128 256 512; do
  rsvg-convert -w $size -h $size "$svg" -o "$set/icon_${size}x${size}.png"
  double=$((size * 2))
  rsvg-convert -w $double -h $double "$svg" -o "$set/icon_${size}x${size}@2x.png"
done
iconutil -c icns "$set" -o Resources/Lace.icns
rm -rf "$(dirname "$set")"
echo "wrote Resources/Lace.icns"
