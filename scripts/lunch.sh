#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
#
# Choose what ./build.sh builds and how it flashes: the board (RP2040 or RP2350),
# the panel config from configs/, and the flash method.  The choice is remembered
# in .pud-config at the repository root (not committed), so `./build.sh` and
# `./build.sh flash` need no arguments.
#
#   ./scripts/lunch.sh                 interactive: board, config, flash method
#   ./scripts/lunch.sh <config>        set the config, keep the rest
#   ./scripts/lunch.sh <board> <config>
#   ./scripts/lunch.sh -f blackmagic   set just the flash method
#
# The board is not implied by the config: the config files branch on PICO_BOARD
# for the overclock profile, and the build directory follows the board -- build/
# is an RP2040 build, build-pico2/ an RP2350 one (see AGENTS.md: an image meant
# for a Pico 2 has to come out of build-pico2/).

set -euo pipefail

HERE=$(cd "$(dirname "$0")/.." && pwd)
STATE=$HERE/.pud-config
INFO=$HERE/scripts/config-info.sh

die() { printf 'lunch: %s\n' "$*" >&2; exit 1; }

state_get() {
    [ -f "$STATE" ] || return 1
    grep -m1 -E "^$1=" "$STATE" | cut -d= -f2- || true
}

# The default the plain `cmake ..` build uses, so both paths agree.
cmake_default_config() {
    grep -m1 -E '^[[:space:]]*set\(PUD_CONFIG[[:space:]]' "$HERE/CMakeLists.txt" |
        sed -E 's/.*set\(PUD_CONFIG[[:space:]]+"?([^")]+)"?.*/\1/' || true
}

board=$(state_get BOARD || true)
config=$(state_get CONFIG || true)
flash=$(state_get FLASH || true)
[ -n "$board" ] || board=pico2
[ -n "$config" ] || config=$(cmake_default_config)
[ -n "$config" ] || config=pico_dm_qd3503728
[ -n "$flash" ] || flash=picotool

board_name() {
    case "$1" in
        pico)  echo "pico (RP2040), build/" ;;
        pico2) echo "pico2 (RP2350), build-pico2/" ;;
        *)     echo "?" ;;
    esac
}

flash_name() {
    case "$1" in
        picotool)   echo "picotool (BOOTSEL over USB, this machine)" ;;
        openocd)    echo "openocd (CMSIS-DAP, this machine)" ;;
        gdb)        echo "gdb -> a running GDB server (OpenOCD, localhost:3333)" ;;
        blackmagic) echo "Black Magic Probe: gdb -> its GDB server (/dev/ttyACM0)" ;;
        none)       echo "none (build only, flash by hand)" ;;
        *)          echo "?" ;;
    esac
}

valid_board() { [ "$1" = pico ] || [ "$1" = pico2 ]; }

valid_config() { [ -f "$HERE/configs/$1.cmake" ]; }

valid_flash() {
    case "$1" in
        picotool|openocd|gdb|blackmagic|none) return 0 ;;
        *) return 1 ;;
    esac
}

# Which of the tools this method needs is actually installed here; the answer
# differs per machine (openocd and gdb live on the debug bench, not necessarily
# on the build host), so say it instead of letting the flash fail later.
flash_tools() {
    local missing=
    case "$1" in
        picotool)   command -v "${PUD_PICOTOOL:-picotool}" >/dev/null 2>&1 ||
                        missing=${PUD_PICOTOOL:-picotool} ;;
        openocd)    command -v "${PUD_OPENOCD:-openocd}" >/dev/null 2>&1 ||
                        missing=${PUD_OPENOCD:-openocd} ;;
        gdb|blackmagic)
                    for c in gdb-multiarch arm-none-eabi-gdb gdb; do
                        command -v "$c" >/dev/null 2>&1 && return 0
                    done
                    missing=gdb ;;
    esac
    [ -n "$missing" ] && printf ' [not installed here: %s]' "$missing"
    return 0
}

# --- non-interactive: arguments name the board, the config and/or the method --
while [ $# -gt 0 ]; do
    case "$1" in
        -f|--flash)  flash=$2; shift 2 ;;
        --flash=*)   flash=${1#--flash=}; shift ;;
        *)           break ;;
    esac
done

if [ $# -gt 0 ]; then
    if valid_board "$1"; then
        board=$1
        shift
    fi
    if [ $# -gt 0 ]; then
        config=${1%.cmake}
        valid_config "$config" || die "no such config: configs/$config.cmake"
        shift
    fi
    [ $# -eq 0 ] || die "too many arguments (usage: $0 [board] [config])"
    valid_flash "$flash" || die "not a flash method: $flash"
else
    # --- interactive -------------------------------------------------------
    printf 'board:\n'
    printf '  1  pico   (RP2040)  -> build/\n'
    printf '  2  pico2  (RP2350)  -> build-pico2/\n'
    printf 'board [1/2, Enter keeps %s]: ' "$board"
    read -r answer || true
    case "$answer" in
        1) board=pico ;;
        2) board=pico2 ;;
        "") ;;
        *) valid_board "$answer" || die "not a board: $answer"; board=$answer ;;
    esac

    printf '\nconfigs (configs/*.cmake):\n'
    i=0
    names=()
    while IFS= read -r name; do
        i=$((i + 1))
        names+=("$name")
        printf '  %2d  %s\n' "$i" "$("$INFO" "$name")"
    done < <("$INFO" --names)

    printf 'config [1-%d, name, Enter keeps %s]: ' "$i" "$config"
    read -r answer || true
    case "$answer" in
        "") ;;
        *[!0-9]*) config=${answer%.cmake}; valid_config "$config" ||
            die "no such config: configs/$config.cmake" ;;
        *) [ "$answer" -ge 1 ] && [ "$answer" -le "$i" ] ||
            die "no such entry: $answer (1-$i)"
           config=${names[$((answer - 1))]} ;;
    esac

    printf '\nflash methods (./build.sh flash):\n'
    printf '  1  picotool    BOOTSEL over USB, on this machine%s\n' "$(flash_tools picotool)"
    printf '  2  openocd     CMSIS-DAP with openocd, on this machine%s\n' "$(flash_tools openocd)"
    printf '  3  gdb         gdb -> a running GDB server (OpenOCD), localhost:3333%s\n' "$(flash_tools gdb)"
    printf '  4  blackmagic  Black Magic Probe: gdb -> its GDB server, /dev/ttyACM0%s\n' "$(flash_tools blackmagic)"
    printf '  5  none        build only, flash by hand\n'
    printf 'flash [1-5, name, Enter keeps %s]: ' "$flash"
    read -r answer || true
    case "$answer" in
        1) flash=picotool ;;
        2) flash=openocd ;;
        3) flash=gdb ;;
        4) flash=blackmagic ;;
        5) flash=none ;;
        "") ;;
        *) valid_flash "$answer" || die "not a flash method: $answer"
           flash=$answer ;;
    esac
fi

printf 'BOARD=%s\nCONFIG=%s\nFLASH=%s\n' "$board" "$config" "$flash" > "$STATE"

printf '\nselected: %s\n' "$("$INFO" --long "$config" | sed -n '2,7p' | sed 's/^/  /')"
printf 'board:    %s\n' "$(board_name "$board")"
printf 'flash:    %s\n' "$(flash_name "$flash")"
printf 'state:    .pud-config (not committed; ./build.sh picks it up)\n'
printf '\nnext: ./build.sh\n'
