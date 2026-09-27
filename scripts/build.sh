#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
#
# Build the firmware for the selected board and panel config.  `./build.sh` calls
# this script; it is also usable on its own.
#
#   scripts/build.sh [-b pico|pico2] [-c <config>] [-j N]
#   scripts/build.sh --clean
#
# Without -b/-c the choice in .pud-config is used (see scripts/lunch.sh), and
# without that the CMakeLists.txt default.
#
# The build directory follows the board: build/ for RP2040, build-pico2/ for
# RP2350.  That is the AGENTS.md rule, not a preference -- an RP2040 image will
# not boot on a Pico 2, so the two never share a directory.

set -euo pipefail

HERE=$(cd "$(dirname "$0")/.." && pwd)
STATE=$HERE/.pud-config
INFO=$HERE/scripts/config-info.sh

die() { printf 'build: %s\n' "$*" >&2; exit 1; }

board=
config=
jobs=$(nproc 2>/dev/null || echo 4)
clean=0

while [ $# -gt 0 ]; do
    case "$1" in
        -b|--board)  board=$2; shift 2 ;;
        -c|--config) config=${2%.cmake}; shift 2 ;;
        -j|--jobs)   jobs=$2; shift 2 ;;
        -j[0-9]*)    jobs=${1#-j}; shift ;;     # make -j8 habit
        --jobs=*)    jobs=${1#--jobs=}; shift ;;
        --clean|-C)  clean=1; shift ;;
        -h|--help)   sed -n '3,17p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *)           die "unknown argument: $1 (try --help)" ;;
    esac
done

state_get() {
    [ -f "$STATE" ] || return 1
    grep -m1 -E "^$1=" "$STATE" | cut -d= -f2- || true
}

if [ -z "$board" ]; then
    board=$(state_get BOARD || true)
    board=${board:-pico2}
fi
if [ -z "$config" ]; then
    config=$(state_get CONFIG || true)
    if [ -z "$config" ]; then
        config=$(grep -m1 -E '^[[:space:]]*set\(PUD_CONFIG[[:space:]]' "$HERE/CMakeLists.txt" |
                     sed -E 's/.*set\(PUD_CONFIG[[:space:]]+"?([^")]+)"?.*/\1/' || true)
    fi
fi

case "$board" in
    pico)  builddir=$HERE/build ;;
    pico2) builddir=$HERE/build-pico2 ;;
    *)     die "not a board: $board (pico | pico2)" ;;
esac
[ -n "$config" ] || die "no config selected -- run ./build.sh lunch"
[ -f "$HERE/configs/$config.cmake" ] || die "no such config: configs/$config.cmake"

if [ "$clean" -eq 1 ]; then
    [ -d "$builddir" ] || { echo "nothing to clean: $builddir"; exit 0; }
    rm -rf "$builddir"
    echo "removed $(basename "$builddir")/"
    exit 0
fi

# The SDK is not vendored: it is either in the environment or in one of the
# places a Pico checkout usually lands.
if [ -z "${PICO_SDK_PATH:-}" ]; then
    for candidate in "$HOME/.pico-sdk" "$HOME/pico/pico-sdk" /opt/pico-sdk; do
        if [ -f "$candidate/pico_sdk_init.cmake" ]; then
            PICO_SDK_PATH=$candidate
            break
        fi
    done
fi
[ -n "${PICO_SDK_PATH:-}" ] || die "PICO_SDK_PATH is not set and no SDK was found
  (looked in ~/.pico-sdk, ~/pico/pico-sdk, /opt/pico-sdk): export PICO_SDK_PATH=<pico-sdk>"
export PICO_SDK_PATH

command -v arm-none-eabi-gcc >/dev/null 2>&1 ||
    die "arm-none-eabi-gcc is not in PATH (it comes with a Pico SDK toolchain)"

printf 'board   %s -> %s/\n' "$board" "$(basename "$builddir")"
printf 'config  %s (configs/%s.cmake)\n' "$config" "$config"
"$INFO" --long "$config" | sed -n '3,7p' | sed 's/^/        /'
printf 'sdk     %s\n' "$PICO_SDK_PATH"
printf '\n'

cmake -S "$HERE" -B "$builddir" -DPICO_BOARD="$board" -DPUD_CONFIG="$config"
cmake --build "$builddir" -j "$jobs"

printf '\n'
for artifact in "$builddir/pico-usb-display.uf2" "$builddir/pico-usb-display.elf"; do
    [ -f "$artifact" ] && printf 'built   %s (%s bytes)\n' "$artifact" "$(stat -c %s "$artifact")"
done
printf 'flash   %s/pico-usb-display.uf2 (BOOTSEL drag & drop, or OpenOCD/picotool)\n' "$builddir"
