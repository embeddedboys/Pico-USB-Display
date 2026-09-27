#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
#
# Flash the firmware ./build.sh built, with the method `./build.sh lunch` chose
# (remembered in .pud-config).
#
#   ./build.sh flash                  use the method in .pud-config
#   ./build.sh flash -m blackmagic    use another method once
#   ./build.sh flash -n               print the command, run nothing
#   ./build.sh flash -t <target>      override the GDB server / probe port
#   ./build.sh flash --reboot         picotool: put a running board into BOOTSEL
#                                     first (through the firmware's reset
#                                     interface; a CMSIS-DAP probe as fallback)
#
# Methods (lunch writes one of these into FLASH=):
#   picotool    board in BOOTSEL mode, over USB, on this machine
#   openocd     CMSIS-DAP with openocd on this machine
#   gdb         gdb -> an already running GDB server (OpenOCD), localhost:3333
#   blackmagic  Black Magic Probe: gdb -> the probe's GDB server (/dev/ttyACM0)
#   none        build only, flash by hand
#
# The command lines are the ones in notes/build-and-flash.md; that file records
# which of them have actually been run against a board and which have not.
#
# Env: PUD_GDB, PUD_PICOTOOL, PUD_OPENOCD, PUD_GDB_TARGET, PUD_BMP_PORT,
#      PUD_VID / PUD_PID (default 0x2e8a / 0x0001, the firmware's own IDs).

set -euo pipefail

HERE=$(cd "$(dirname "$0")/.." && pwd)
STATE=$HERE/.pud-config

die() { printf 'flash: %s\n' "$*" >&2; exit 1; }

state_get() {
    [ -f "$STATE" ] || return 1
    grep -m1 -E "^$1=" "$STATE" | cut -d= -f2- || true
}

method=$(state_get FLASH || true)
target=
dry=0
reboot=0

# The firmware's IDs, for the picotool calls that have to name the device:
# picotool's default device filter only knows the bootrom and the SDK's CDC PIDs
# (see reboot_to_bootsel below).
PUD_VID=${PUD_VID:-0x2e8a}
PUD_PID=${PUD_PID:-0x0001}

while [ $# -gt 0 ]; do
    case "$1" in
        -m|--method)  method=$2; shift 2 ;;
        --method=*)   method=${1#--method=}; shift ;;
        -t|--target)  target=$2; shift 2 ;;
        --target=*)   target=${1#--target=}; shift ;;
        -n|--dry-run) dry=1; shift ;;
        -r|--reboot)  reboot=1; shift ;;
        -h|--help)    sed -n '4,25p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *)            die "unknown argument: $1 (try --help)" ;;
    esac
done

[ -n "$method" ] || die "no flash method selected -- run ./build.sh lunch"
[ "$method" != none ] ||
    die "flash method is 'none' (build only) -- run ./build.sh lunch to pick one"

board=$(state_get BOARD || true)
board=${board:-pico2}
case "$board" in
    pico)  builddir=$HERE/build;       tcfg=rp2040 ;;
    pico2) builddir=$HERE/build-pico2; tcfg=rp2350 ;;
    *)     die "not a board: $board (pico | pico2)" ;;
esac

elf=$builddir/pico-usb-display.elf
uf2=$builddir/pico-usb-display.uf2

# Echo the command line, and run it unless -n was given.  Arguments with spaces
# (the gdb -ex lines) are quoted so that the echo can be pasted into a shell.
run() {
    local arg sep=
    printf '+ '
    for arg in "$@"; do
        case "$arg" in
            *[[:space:]]*) printf '%s"%s"' "$sep" "$arg" ;;
            *)             printf '%s%s' "$sep" "$arg" ;;
        esac
        sep=' '
    done
    printf '\n'
    [ "$dry" -eq 1 ] || "$@"
}

# A missing tool is fatal, except under -n, where seeing the command line is the
# whole point (openocd and the GDB server usually live on the debug bench, not on
# the build host).
need_tool() {
    command -v "$1" >/dev/null 2>&1 && return 0
    if [ "$dry" -eq 1 ]; then
        printf 'flash: note: %s is not installed here\n' "$1" >&2
        return 0
    fi
    die "$1 not found ($2)"
}

# The gdb names differ per distribution; PUD_GDB overrides the search.
find_gdb() {
    local candidate
    for candidate in gdb-multiarch arm-none-eabi-gdb gdb; do
        command -v "$candidate" >/dev/null 2>&1 && { echo "$candidate"; return 0; }
    done
    return 1
}

# Is the board sitting in BOOTSEL, reachable by picotool?
bootsel_present() {
    "$PICOTOOL" info -d 2>&1 | grep -qv 'No accessible'
}

# The board reboots when it is asked to, so give it a few seconds to come back.
wait_for_bootsel() {
    local i
    for i in $(seq 1 20); do
        sleep 0.5
        bootsel_present && return 0
    done
    return 1
}

# Put the board into BOOTSEL without touching the button (picotool --reboot).
#
# The firmware exposes picoboot's reset interface (0xff/0x00/0x01, see
# usbd_vendor.h), so picotool can ask a *running* board to reboot: that is the
# first attempt, and it needs no debugger at all.  The --vid/--pid have to be
# spelled out: picotool's device filter knows only the bootrom PIDs and the SDK's
# CDC ones, so with its default filter it gives up ("No accessible RP-series
# devices in BOOTSEL mode were found") before it ever looks for a reset
# interface.  Note the option order -- picotool rejects device-selection options
# that come after the command's own flags.
#
# If that fails (no reset interface, old firmware, no permissions), fall back to
# a CMSIS-DAP probe: the RP2040 bootrom's reset_usb_boot (ROM function "UB") does
# the same.  That call resets the chip inside itself, so openocd reports a failed
# ROM call even though the board ends up in BOOTSEL -- hence the polling below
# rather than an exit status.
reboot_to_bootsel() {
    if "$PICOTOOL" reboot --vid "$PUD_VID" --pid "$PUD_PID" -f -u >/dev/null 2>&1 &&
        wait_for_bootsel; then
        return 0
    fi
    if [ "$tcfg" != rp2040 ] || ! command -v "${PUD_OPENOCD:-openocd}" >/dev/null 2>&1; then
        die "the board is not in BOOTSEL and did not accept a reboot request
  (no picoboot reset interface in the running firmware?): hold BOOTSEL while
  plugging it in, or attach a CMSIS-DAP probe to use the bootrom route"
    fi
    printf 'flash: asking the bootrom for BOOTSEL through the probe\n'
    "${PUD_OPENOCD:-openocd}" -f interface/cmsis-dap.cfg -c "adapter speed 10000" \
        -f "target/$tcfg.cfg" -c "init" -c "targets $tcfg.core0" -c "halt" \
        -c "flash probe 0" -c "rp2xxx rom_api_call UB 0 0" -c "shutdown" \
        >/dev/null 2>&1
    wait_for_bootsel
}

case "$method" in
    picotool)
        PICOTOOL=${PUD_PICOTOOL:-picotool}
        need_tool "$PICOTOOL" "set PUD_PICOTOOL=<path>"
        [ -f "$uf2" ] || die "no $uf2 -- run ./build.sh first"
        # A board in BOOTSEL mode is what picotool can talk to; a board running
        # the firmware has to be sent there first (--reboot).
        if [ "$reboot" -eq 1 ] && [ "$dry" -eq 0 ] && ! bootsel_present; then
            reboot_to_bootsel || die "could not get the board into BOOTSEL"
        fi
        if [ "$dry" -eq 0 ] && ! bootsel_present; then
            die "no board in BOOTSEL mode: hold BOOTSEL while plugging it in
  (or ./build.sh flash --reboot to reset a board that is still running)"
        fi
        printf 'board   %s (%s)\n' "$board" "$(basename "$builddir")"
        printf 'method  picotool -> %s\n\n' "$(basename "$uf2")"
        run "$PICOTOOL" load -v -x "$uf2"
        ;;
    openocd)
        OPENOCD=${PUD_OPENOCD:-openocd}
        need_tool "$OPENOCD" "install openocd, or set PUD_OPENOCD=<path>"
        [ -f "$elf" ] || die "no $elf -- run ./build.sh first"
        printf 'board   %s (%s)\n' "$board" "$(basename "$builddir")"
        printf 'method  openocd (CMSIS-DAP) -> target/%s.cfg\n\n' "$tcfg"
        run "$OPENOCD" -f interface/cmsis-dap.cfg -c "adapter speed 10000" \
            -f "target/$tcfg.cfg" \
            -c "program $elf verify reset exit"
        ;;
    gdb|blackmagic)
        GDB=${PUD_GDB:-$(find_gdb || true)}
        if [ -z "$GDB" ]; then
            if [ "$dry" -eq 0 ]; then
                die "no gdb found (install gdb-multiarch, or set PUD_GDB=<path>)"
            fi
            GDB=gdb-multiarch       # name it as the notes do, for the echo
            printf 'flash: note: no gdb found here, showing the gdb-multiarch line\n' >&2
        fi
        [ -f "$elf" ] || die "no $elf -- run ./build.sh first"
        if [ "$method" = blackmagic ]; then
            server=${target:-${PUD_BMP_PORT:-/dev/ttyACM0}}
            [ "$dry" -eq 1 ] || [ -e "$server" ] ||
                die "no Black Magic Probe at $server (plug it in, or -t <cdc device>)"
            printf 'board   %s (%s)\n' "$board" "$(basename "$builddir")"
            printf 'method  blackmagic (%s) -> %s\n\n' "$GDB" "$server"
            run "$GDB" -q -nh -ex "set pagination off" -ex "set confirm off" \
                -ex "file $elf" \
                -ex "target extended-remote $server" \
                -ex "monitor swdp_scan" \
                -ex "attach 1" \
                -ex "load" \
                -ex "compare-sections" \
                -ex "kill" \
                -ex "quit"
        else
            server=${target:-${PUD_GDB_TARGET:-localhost:3333}}
            if [ "$dry" -eq 0 ]; then
                case "$server" in
                    *:*) timeout 3 bash -c "exec 3<>/dev/tcp/${server%%:*}/${server##*:}" \
                             2>/dev/null ||
                             die "nothing listening on $server
  (start the GDB server first, or -t <host:port>)" ;;
                    *)   [ -e "$server" ] ||
                             die "no GDB server device: $server (or -t <host:port>)" ;;
                esac
            fi
            printf 'board   %s (%s)\n' "$board" "$(basename "$builddir")"
            printf 'method  gdb (%s) -> %s\n\n' "$GDB" "$server"
            # This is the sequence notes/build-and-flash.md verified on the
            # panel: load, then reset into the freshly written flash.
            run "$GDB" -q -nh -ex "set pagination off" -ex "set confirm off" \
                -ex "file $elf" \
                -ex "target extended-remote $server" \
                -ex "monitor reset halt" \
                -ex "load" \
                -ex "monitor reset run" \
                -ex "detach" \
                -ex "quit"
        fi
        ;;
    *)
        die "unknown flash method: $method (picotool | openocd | gdb | blackmagic | none)"
        ;;
esac

if [ "$dry" -eq 1 ]; then
    printf '\n(dry run: nothing was executed)\n'
fi
exit 0
