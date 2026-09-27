#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
#
# Firmware build front-end.
#
#   ./build.sh                    build the selected board and panel config
#   ./build.sh lunch              choose them (configs/), remembered in .pud-config
#   ./build.sh flash              flash the built firmware the same way
#   ./build.sh config             show what is selected
#   ./build.sh configs            list the available panel configs
#   ./build.sh clean              remove the selected board's build directory
#   ./build.sh help
#
# Options (they apply to the build, and to `lunch` when it is given a config):
#   -b, --board pico|pico2        RP2040 (build/) or RP2350 (build-pico2/)
#   -c, --config <name>           a config from configs/ (see ./build.sh configs)
#   -j, --jobs N                  parallel build jobs (default: nproc)
#   -f, --flash <method>          lunch: which flash method to remember
#
# `flash` takes its own options (`./build.sh flash --help`):
#
#   ./build.sh flash -n           print the flash command, run nothing
#   ./build.sh flash -m gdb       flash with another method once
#   ./build.sh flash -t <target>  another GDB server / probe port
#
# The work lives in scripts/: lunch.sh picks the target and the flash method,
# build.sh configures and builds it, flash.sh flashes it, config-info.sh
# describes a config.  A plain `cmake ..` still works and uses the default in
# CMakeLists.txt.

set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
SCRIPTS=$HERE/scripts

usage() { sed -n '4,29p' "$0" | sed 's/^# \{0,1\}//'; }

cmd=${1:-build}
case "$cmd" in
    lunch)
        shift
        exec "$SCRIPTS/lunch.sh" "$@"
        ;;
    flash)
        shift
        exec "$SCRIPTS/flash.sh" "$@"
        ;;
    config)
        shift
        if [ $# -gt 0 ]; then
            exec "$SCRIPTS/config-info.sh" --long "$@"
        fi
        exec "$SCRIPTS/lunch.sh" "$@"
        ;;
    configs)
        shift
        [ $# -eq 0 ] || { "$SCRIPTS/config-info.sh" --long "$@" ; exit $? ; }
        printf '%-28s %-9s %-5s %-9s %s\n' config panel bus driven 'touch:'
        for name in $("$SCRIPTS/config-info.sh" --names); do
            "$SCRIPTS/config-info.sh" "$name"
        done
        ;;
    clean)
        shift
        exec "$SCRIPTS/build.sh" --clean "$@"
        ;;
    help|-h|--help)
        usage
        ;;
    build|"")
        shift || true
        exec "$SCRIPTS/build.sh" "$@"
        ;;
    *)
        # `./build.sh -b pico` and friends land here: no command word, just options
        exec "$SCRIPTS/build.sh" "$@"
        ;;
esac
