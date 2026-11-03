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
#   sudo tools/efi.sh csr show                 # the SIP mask, decoded
#   sudo tools/efi.sh csr add|remove BIT...    # e.g. task_for_pid dtrace
#   sudo tools/efi.sh defaults    # take our now-redundant tokens out of boot-args (2xw)
#   sudo tools/efi.sh usbintr0 on|off|status  # the XHCI interrupter-0 patches
#   sudo tools/efi.sh usbpoke on|off          # XHCIPoke.kext (the 10 ms polling fallback)
#
# Only a person runs this (see docs/RULES.md). Every change first saves the EFI's
# config.plist to logs/efi-backups/<time>/; edits go through
# tools/efi_config.py, which touches only the RembrandtGPU entry and
# boot-args. Changes take effect at the next reboot. Keep a known-good EFI to
# boot from (the boot menu's backup entry) in case the new one does not boot.
set -eu

[ "$(id -u)" = 0 ] || { echo "run with sudo: sudo tools/efi.sh ${1:-status}" >&2; exit 1; }
cmd="${1:-}"
case "$cmd" in install|uninstall|on|off|status|restore|sysreport-on|sysreport-off|vbios|bootarg|csr|usbintr0|usbpoke|defaults) ;; *)
    sed -n '2,18p' "$0" | sed 's/^# \{0,1\}//'; exit 2 ;;
esac

root="$(cd "$(dirname "$0")/.." && pwd)"
editor="$root/tools/efi_config.py"
usbeditor="$root/tools/xhci_intr0.py"
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
usbedit() { python3 "$usbeditor" "$config" "$@"; }

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
defaults)
    # 2xw: the 45 tokens are the default now, so take them out of boot-args and
    # then say what is left to check by hand. Negative tokens are kept - they
    # are the user turning something off - and so is everything not ours.
    backup
    edit bootarg strip
    sync
    echo
    # 2xx: a "0" in rbgpu-tick or rbgpu-fb is only a disarm once rbgpu-gen
    # exists - before that it is the value a pre-2xw boot wrote when it spent
    # the one-shot, and the plugin ignores it and then migrates it. Report the
    # same rule the two kexts apply, or this is an instrument that lies.
    gen="$(nvram rbgpu-gen 2>/dev/null | sed 's/^rbgpu-gen[[:space:]]*//')"
    if [ -z "$gen" ]; then
        echo "rbgpu-gen: absent - the next armed boot migrates the two keys a"
        echo "           pre-2xw build consumed by overwrite (2xx)."
    else
        echo "rbgpu-gen: \"$gen\" - migrated, so every \"0\" below is yours."
    fi
    echo
    echo "NVRAM keys that would DISARM a half (2xw reads them as disarms now):"
    for k in rbgpu-arm rbgpu-fb rbgpu-agdc rbgpu-iop rbgpu-tick; do
        v="$(nvram "$k" 2>/dev/null | sed "s/^$k[[:space:]]*//")"
        legacy=no
        case "$k" in rbgpu-tick|rbgpu-fb) legacy=yes ;; esac
        if [ -z "$v" ]; then
            echo "  $k: absent (armed)"
        elif [ "$v" = "0" ] && [ "$legacy" = yes ] && [ -z "$gen" ]; then
            echo "  $k: \"0\" - IGNORED, a spent pre-2xw one-shot, not a disarm (2xx)"
        elif [ "$v" = "0" ]; then
            echo "  $k: \"0\" - DISARMED. Re-arm with: sudo nvram -d $k"
        else
            echo "  $k: \"$v\" (armed; any value but \"0\" counts)"
        fi
    done
    echo "  rbgpu-safe: $(nvram rbgpu-safe 2>/dev/null | sed 's/^rbgpu-safe[[:space:]]*//' || echo absent) \
(the boot sentinel writes this one; \"1\" at the start of a boot is normal)"
    echo
    echo "Reboot. Then: ioreg -rl -c RembrandtGPUPeer | grep -o '\"RBGPUDiag\" = {.*}'"
    ;;
bootarg)
    # 2xx: SEVERAL tokens in one call. The WindowServer bisect needs a whole
    # group of derived negatives turned on for one boot, and one `efi.sh`
    # invocation per token meant one mount, one backup and one sync each -
    # twenty backup directories for a single experiment, and a typo in any of
    # them silently measured the wrong configuration. One backup, one sync,
    # and the arguments are applied in order.
    op="${2:-}"; shift 2 2>/dev/null || true
    case "$op" in add|remove) ;; *) echo "usage: sudo tools/efi.sh bootarg add|remove ARG [ARG...]" >&2; exit 2 ;; esac
    [ "$#" -gt 0 ] || { echo "usage: sudo tools/efi.sh bootarg add|remove ARG [ARG...]" >&2; exit 2; }
    backup
    for arg in "$@"; do
        edit bootarg "$op" "$arg"
    done
    sync
    ;;
csr)
    # The SIP mask the kernel reads at boot. NVRAM -> Delete lists
    # csr-active-config, so OpenCore rewrites it from config.plist every boot
    # and `nvram csr-active-config=...` never survives a restart.
    op="${2:-}"; shift 2 2>/dev/null || true
    case "$op" in
    show) edit csr show ;;
    add|remove)
        [ "$#" -gt 0 ] || { echo "usage: sudo tools/efi.sh csr add|remove BIT [BIT...]" >&2; exit 2; }
        backup
        edit csr "$op" "$@"
        sync
        ;;
    *) echo "usage: sudo tools/efi.sh csr show|add BIT...|remove BIT..." >&2; exit 2 ;;
    esac
    ;;
usbintr0)
    # Force every Transfer TRB to interrupter 0, the only one that can raise an
    # interrupt on this machine's single-MSI xHCI controllers. XHCIPoke.kext is
    # the polling fallback for the same defect; leave it enabled for the first
    # boot after this, then compare its counters (see docs/HANDOFF-NEXT.md).
    case "${2:-}" in
    status) usbedit status ;;
    on)
        python3 "$usbeditor" verify || { echo "not installing: the patch table does not match this kernel" >&2; exit 1; }
        backup
        usbedit add
        sync
        usbedit status
        echo "done. Reboot, then read: ioreg -rl -c XHCIPoke | grep -o '\"Controllers\".*'"
        ;;
    off)
        backup
        usbedit remove
        sync
        usbedit status
        ;;
    *) echo "usage: sudo tools/efi.sh usbintr0 on|off|status" >&2; exit 2 ;;
    esac
    ;;
usbpoke)
    case "${2:-}" in
    on|off)
        backup
        usbedit poke "$2"
        sync
        usbedit status
        ;;
    *) echo "usage: sudo tools/efi.sh usbpoke on|off" >&2; exit 2 ;;
    esac
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
