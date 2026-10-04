#!/bin/sh
# Unload the probe kext loaded by tools/load.sh.
#
#   sudo tools/unload.sh
#
# Only a person runs this (see CLAUDE.md).
set -eu

[ "$(id -u)" = 0 ] || { echo "run with sudo: sudo tools/unload.sh" >&2; exit 1; }

sync
kmutil unload -b com.rembrandtgpu.probe
sleep 1
/sbin/dmesg | grep 'RembrandtProbe' | tail -3
if kmutil showloaded 2>/dev/null | grep -q com.rembrandtgpu.probe; then
    echo "still loaded" >&2
    exit 1
fi
echo "unloaded"
