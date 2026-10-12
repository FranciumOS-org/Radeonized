#!/bin/sh
# Put the RembrandtGPU Lilu plugin into the OpenCore EFI, or take it out.
#
#   sudo tools/efi.sh install     # build, copy the kext, add it after Lilu
#   sudo tools/efi.sh off         # add -rbgpuoff to boot-args (plugin stays, does nothing)
#   sudo tools/efi.sh on          # remove -rbgpuoff
#   sudo tools/efi.sh uninstall   # remove the entry, the kext and -rbgpuoff
#   sudo tools/efi.sh status      # what the EFI has now
#   sudo tools/efi.sh restore     # put back the config.plist saved before the last change
#   sudo tools/efi.sh sysreport-on   # OpenCore dumps ACPI tables (VFCT = VBIOS) at next boot
#   sudo tools/efi.sh vbios          # after that boot: extract the 680M VBIOS, turn SysReport off
#   sudo tools/efi.sh sysreport-off
#   sudo tools/efi.sh bootarg add|remove ARG   # e.g. msgbuf=1048576 (bigger kernel log buffer)
#
# Only a person runs this (see docs/RULES.md). Every change first saves the EFI's
# config.plist to logs/efi-backups/<time>/; edits go through
# tools/efi_config.py, which touches only the RembrandtGPU entry and
# boot-args. Changes take effect at the next reboot. Keep a known-good EFI to
# boot from (the boot menu's backup entry) in case the new one does not boot.
set -eu

[ "$(id -u)" = 0 ] || { echo "run with sudo: sudo tools/efi.sh ${1:-status}" >&2; exit 1; }
cmd="${1:-}"
case "$cmd" in install|uninstall|on|off|status|restore|sysreport-on|sysreport-off|vbios|bootarg) ;; *)
    sed -n '2,16p' "$0" | sed 's/^# \{0,1\}//'; exit 2 ;;
esac

root="$(cd "$(dirname "$0")/.." && pwd)"
editor="$root/tools/efi_config.py"
kext="$root/build/out/RembrandtGPU.kext"
user="${SUDO_USER:-root}"

# --- find and mount the EFI partition that holds OpenCore ---------------
mounted_by_us=""
cleanup() {
    for d in $mounted_by_us; do diskutil unmount "$d" >/dev/null 2>&1 || true; done
}
trap cleanup EXIT INT TERM

mountpoint_of() { diskutil info "$1" | sed -n 's/^ *Mount Point: *//p'; }

efi=""
for dev in $(diskutil list | awk '$2 == "EFI" { print $NF }'); do
    mp="$(mountpoint_of "$dev")"
    if [ -z "$mp" ]; then
        diskutil mount "$dev" >/dev/null 2>&1 || continue
        mounted_by_us="$mounted_by_us $dev"
        mp="$(mountpoint_of "$dev")"
    fi
    if [ -n "$mp" ] && [ -f "$mp/EFI/OC/config.plist" ]; then
        [ -z "$efi" ] || { echo "more than one EFI partition has EFI/OC/config.plist ($efi, $mp); stopping" >&2; exit 1; }
        efi="$mp"; efidev="$dev"
    fi
done
[ -n "$efi" ] || { echo "no EFI partition with EFI/OC/config.plist found" >&2; exit 1; }

# Keep Spotlight off the EFI partition. Indexing a mounted FAT32 ESP has
# corrupted this one once (2026-10-12), which cost a restore from
# logs/efi-backups. `.metadata_never_index` at the volume root tells Spotlight
# to skip it, and mdutil turns off any index already running. Both are
# best-effort: a failure here must not stop an install.
if [ ! -e "$efi/.metadata_never_index" ]; then
    touch "$efi/.metadata_never_index" 2>/dev/null \
        && echo "  (created $efi/.metadata_never_index so Spotlight skips the EFI)"
fi
mdutil -i off "$efi" >/dev/null 2>&1 || true
mdutil -X "$efi" >/dev/null 2>&1 || true

oc="$efi/EFI/OC"
config="$oc/config.plist"
echo "OpenCore EFI: $efidev at $efi"

backup() {
    dir="$root/logs/efi-backups/$(date +%Y%m%d-%H%M%S)-$cmd"
    mkdir -p "$dir"
    cp "$config" "$dir/config.plist"
    chown -R "$user" "$root/logs/efi-backups"
    echo "saved config.plist to ${dir#$root/}"
}

edit() { python3 "$editor" "$config" "$@"; }

show_status() {
    edit status
    if [ -d "$oc/Kexts/RembrandtGPU.kext" ]; then
        v="$(plutil -extract CFBundleVersion raw "$oc/Kexts/RembrandtGPU.kext/Contents/Info.plist" 2>/dev/null || echo '?')"
        u="$(dwarfdump --uuid "$oc/Kexts/RembrandtGPU.kext/Contents/MacOS/RembrandtGPU" 2>/dev/null | awk '{print $2}')"
        echo "EFI/OC/Kexts/RembrandtGPU.kext: present, version $v, UUID ${u:-?}"
    else
        echo "EFI/OC/Kexts/RembrandtGPU.kext: not present"
    fi
}

case "$cmd" in
status)
    show_status
    ;;
install)
    [ -d "$oc/Kexts/Lilu.kext" ] || { echo "no Lilu.kext in EFI/OC/Kexts; stopping" >&2; exit 1; }
    echo "building as $user"
    # GNU make 3.81 (macOS) compares whole seconds; recompile the plugin's
    # own sources so a same-second edit can never ship a stale binary.
    rm -f "$root"/build/plugin/kern_start.o "$root"/build/plugin/RembrandtGPUPeer.o
    sudo -u "$user" make -C "$root" plugin
    plutil -lint -s "$kext/Contents/Info.plist"
    [ -f "$kext/Contents/MacOS/RembrandtGPU" ] || { echo "no binary in $kext" >&2; exit 1; }
    backup
    rm -rf "$oc/Kexts/RembrandtGPU.kext"
    ditto --norsrc --noextattr "$kext" "$oc/Kexts/RembrandtGPU.kext"
    edit add-kext
    sync
    show_status
    echo "done. Reboot, then run: tools/plugin_check.sh"
    ;;
uninstall)
    backup
    edit remove-kext
    edit bootarg remove -rbgpuoff
    rm -rf "$oc/Kexts/RembrandtGPU.kext"
    sync
    show_status
    ;;
off)
    backup
    edit bootarg add -rbgpuoff
    sync
    ;;
on)
    backup
    edit bootarg remove -rbgpuoff
    sync
    ;;
bootarg)
    op="${2:-}"; arg="${3:-}"
    case "$op" in add|remove) ;; *) echo "usage: sudo tools/efi.sh bootarg add|remove ARG" >&2; exit 2 ;; esac
    [ -n "$arg" ] || { echo "usage: sudo tools/efi.sh bootarg add|remove ARG" >&2; exit 2; }
    backup
    edit bootarg "$op" "$arg"
    sync
    ;;
sysreport-on)
    if [ -d "$efi/SysReport" ]; then
        echo "note: $efi/SysReport already exists; OpenCore may not overwrite it."
        echo "      'sudo tools/efi.sh vbios' looks there first."
    fi
    backup
    edit sysreport on
    sync
    echo "reboot once (needs the DEBUG OpenCore), then: sudo tools/efi.sh vbios"
    ;;
sysreport-off)
    backup
    edit sysreport off
    sync
    ;;
vbios)
    vfct="$(ls "$efi"/SysReport/ACPI/VFCT*.aml 2>/dev/null | head -1)"
    if [ -z "$vfct" ]; then
        echo "no SysReport/ACPI/VFCT*.aml on the EFI." >&2
        ls "$efi/SysReport/ACPI" 2>/dev/null | head -40 >&2 || echo "(no SysReport folder: did the boot use the DEBUG OpenCore with SysReport on?)" >&2
        exit 1
    fi
    out="$root/firmware"
    mkdir -p "$out"
    cp "$vfct" "$out/VFCT.aml"
    python3 "$root/tools/vfct.py" "$out/VFCT.aml" 1002:1681 "$out/680m-vbios.rom"
    shasum -a 256 "$out/680m-vbios.rom"
    chown -R "$user" "$out"
    backup
    edit sysreport off
    sync
    echo "VBIOS in firmware/ (not committed). SysReport is off again."
    ;;
restore)
    last="$(ls -d "$root"/logs/efi-backups/*/ 2>/dev/null | tail -1)"
    [ -n "$last" ] || { echo "no backups in logs/efi-backups" >&2; exit 1; }
    echo "this puts back ${last#$root/}config.plist over the EFI's config.plist"
    printf "type yes to continue: "
    read -r ans
    [ "$ans" = yes ] || { echo "nothing changed"; exit 1; }
    plutil -lint -s "${last}config.plist"
    cp "${last}config.plist" "$config"
    sync
    show_status
    ;;
esac
