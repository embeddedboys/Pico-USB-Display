#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
#
# Describe one board config from configs/.
#
#   scripts/config-info.sh pico_dm_qd3503728          one summary line
#   scripts/config-info.sh --long pico_dm_qd3503728   every field we can read
#   scripts/config-info.sh --names                    just the names, one per line
#
# A config is a plain list of `set(KEY value)` lines, and the same name exists in
# the library's configs/ too: the repository's own configs/ is what the build
# includes, so a local tweak of a panel does not have to go into the library.
# The driven resolution applies the rotation here (1/3 turn the frame on its
# side) -- the same rule the firmware and the drivers use, see
# notes/usb-protocol.md.

set -euo pipefail

HERE=$(cd "$(dirname "$0")/.." && pwd)
CONFIGS=$HERE/configs

usage() {
    sed -n '3,15p' "$0" | sed 's/^# \{0,1\}//'
    exit "${1:-0}"
}

mode=short
case "${1:-}" in
    -l|--long)  mode=long; shift ;;
    --names)    mode=names; shift ;;
    -h|--help)  usage 0 ;;
esac

if [ "$mode" = names ]; then
    [ $# -eq 0 ] || usage 2
    (cd "$CONFIGS" && ls -1 *.cmake 2>/dev/null | sed 's/\.cmake$//')
    exit 0
fi

name=${1:-}
[ -n "$name" ] || usage 2
name=${name%.cmake}
file=$CONFIGS/$name.cmake
[ -f "$file" ] || { echo "no such config: configs/$name.cmake" >&2; exit 1; }

# First assignment wins, which is also how CMake sees a variable set twice.
val() {
    grep -m1 -E "^[[:space:]]*set\($1[[:space:]]" "$file" 2>/dev/null |
        sed -E "s/^[[:space:]]*set\($1[[:space:]]+([^)#[:space:]]+).*/\1/" ||
        true
}

driver() {
    grep -m1 -E "^[[:space:]]*set\(TFT_DRV_USE_[A-Z0-9_]+[[:space:]]+1" "$file" |
        sed -E 's/^[[:space:]]*set\(TFT_DRV_USE_([A-Z0-9_]+)[[:space:]]+1.*/\1/' || true
}

touch() {
    if [ "$(val INDEV_DRV_NOT_USED)" = "1" ]; then
        echo none
        return
    fi
    grep -m1 -E "^[[:space:]]*set\(INDEV_DRV_USE_[A-Z0-9_]+[[:space:]]+1" "$file" |
        sed -E 's/^[[:space:]]*set\(INDEV_DRV_USE_([A-Z0-9_]+)[[:space:]]+1.*/\1/' || true
}

hor=$(val TFT_HOR_RES)
ver=$(val TFT_VER_RES)
rot=$(val TFT_ROTATION)
rot=${rot:-0}
if [ -z "$hor" ] || [ -z "$ver" ]; then
    echo "$name: no TFT_HOR_RES/TFT_VER_RES in configs/$name.cmake" >&2
    exit 1
fi

if [ $((rot % 2)) -eq 1 ]; then driven="${ver}x${hor}"; else driven="${hor}x${ver}"; fi

case "$(val TFT_BUS_TYPE)" in
    0) bus=SPI ;;
    1) bus=8080 ;;
    2) bus=I2C ;;
    *) bus="?" ;;
esac

drv=$(driver); [ -n "$drv" ] || drv="?"
tch=$(touch); [ -n "$tch" ] || tch="?"

ovc=$(val OVERCLOCK_ENABLED); ovc=${ovc:-0}
ocp=$(val OVERCLOCK_PROFILE)

if [ "$mode" = long ]; then
    printf 'config    %s\n' "$name"
    printf 'file      configs/%s.cmake\n' "$name"
    printf 'panel     %s\n' "$drv"
    printf 'bus       %s\n' "$bus"
    printf 'native    %sx%s (TFT_HOR_RES x TFT_VER_RES)\n' "$hor" "$ver"
    printf 'rotation  %s -> driven %s\n' "$rot" "$driven"
    printf 'touch     %s\n' "$tch"
    if [ "$ovc" = "1" ]; then
        printf 'overclock profile %s\n' "${ocp:-?}"
    else
        printf 'overclock off\n'
    fi
else
    printf '%-28s %-9s %-5s %-9s touch:%s\n' "$name" "$drv" "$bus" "$driven" "$tch"
fi
