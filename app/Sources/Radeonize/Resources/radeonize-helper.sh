#!/bin/sh
# radeonize-helper.sh - the only part of Radeonize that runs as root.
#
# The app calls it through one administrator prompt per step:
#
#   efi-install  DEVICE STAGE BACKUP OWNER   add RembrandtGPU.kext to the EFI after Lilu
#   efi-restore  DEVICE BACKUP OWNER         put the saved EFI state back
#   fb-install   STAGE BACKUP OWNER          install RBAMDFramebuffer.kext into the aux collection
#   fb-restore   BACKUP OWNER                put the saved aux collection (and kext) back
#
# Each step saves what it is about to replace, writes, and verifies before it
# returns. The logic follows tools/efi.sh and tools/fbinstall.sh in the
# Radeonized repo; keep the two in step when either changes.
set -eu

die() { echo "radeonize: $*" >&2; exit 1; }
[ "$(id -u)" = 0 ] || die "must run as root (the app asks for your password)"

BOOT=/System/Library/KernelCollections/BootKernelExtensions.kc
SYS=/System/Library/KernelCollections/SystemKernelExtensions.kc
LIVE=/Library/KernelCollections/AuxiliaryKernelExtensions.kc
SCRATCH=/private/var/tmp/radeonize-aux.kc
FBEXT=/Library/Extensions/RBAMDFramebuffer.kext

MP=""
DEV=""
MOUNTED_BY_US=""

cleanup() {
    if [ -n "$MOUNTED_BY_US" ]; then
        diskutil unmount "$DEV" >/dev/null 2>&1 || true
    fi
}
trap cleanup EXIT

# --- EFI ------------------------------------------------------------------

mount_efi() {
    DEV="$1"
    MP="$(diskutil info "$DEV" | sed -n 's/^ *Mount Point: *//p' | sed 's/ *$//')"
    if [ -z "$MP" ]; then
        diskutil mount "$DEV" >/dev/null || die "could not mount $DEV"
        MOUNTED_BY_US=1
        MP="$(diskutil info "$DEV" | sed -n 's/^ *Mount Point: *//p' | sed 's/ *$//')"
    fi
    [ -n "$MP" ] || die "$DEV has no mount point"
}

check_efi() {
    [ -f "$MP/EFI/OC/config.plist" ] || die "$DEV has no EFI/OC/config.plist; it is not an OpenCore EFI"
}

# edit_config PATH add|remove: touches only the RembrandtGPU entry in Kernel -> Add.
edit_config() {
    /usr/bin/python3 - "$1" "$2" <<'PY'
import os, plistlib, sys
path, op = sys.argv[1], sys.argv[2]
BUNDLE = "RembrandtGPU.kext"
ENTRY = {
    "Arch": "x86_64",
    "BundlePath": BUNDLE,
    "Comment": "RembrandtGPU (Radeon 680M)",
    "Enabled": True,
    "ExecutablePath": "Contents/MacOS/RembrandtGPU",
    "MaxKernel": "",
    "MinKernel": "",
    "PlistPath": "Contents/Info.plist",
}
with open(path, "rb") as f:
    cfg = plistlib.load(f)
lst = cfg["Kernel"]["Add"]
def hits(bundle):
    return [i for i, e in enumerate(lst) if isinstance(e, dict) and e.get("BundlePath") == bundle]
for i in reversed(hits(BUNDLE)):
    del lst[i]
if op == "add":
    lilu = [i for i in hits("Lilu.kext") if lst[i].get("Enabled")]
    if not lilu:
        sys.exit("no enabled Lilu.kext in Kernel -> Add")
    lst.insert(lilu[0] + 1, dict(ENTRY))
tmp = path + ".radeonize"
with open(tmp, "wb") as f:
    plistlib.dump(cfg, f, sort_keys=False)
os.replace(tmp, path)
PY
}

# restore_efi_files BACKUP OC: copy the saved config and kext back over the EFI.
restore_efi_files() {
    cp "$1/config.plist" "$2/config.plist"
    rm -rf "$2/Kexts/RembrandtGPU.kext"
    if [ "$(cat "$1/had-kext")" = yes ]; then
        ditto --norsrc --noextattr "$1/RembrandtGPU.kext" "$2/Kexts/RembrandtGPU.kext"
    fi
    sync
}

efi_install() {
    dev="$1"; stage="$2"; backup="$3"; owner="$4"
    kext="$stage/RembrandtGPU.kext"
    [ -f "$kext/Contents/MacOS/RembrandtGPU" ] || die "no RembrandtGPU.kext binary in $stage"
    plutil -lint "$kext/Contents/Info.plist" >/dev/null || die "Info.plist in $kext does not lint"

    mount_efi "$dev"
    check_efi
    oc="$MP/EFI/OC"
    [ -d "$oc/Kexts/Lilu.kext" ] || die "Lilu.kext is not in EFI/OC/Kexts; install Lilu first"

    mkdir -p "$backup"
    echo "$dev" > "$backup/efi-device"
    cp "$oc/config.plist" "$backup/config.plist"
    if [ -d "$oc/Kexts/RembrandtGPU.kext" ]; then
        cp -R "$oc/Kexts/RembrandtGPU.kext" "$backup/RembrandtGPU.kext"
        echo yes > "$backup/had-kext"
    else
        echo no > "$backup/had-kext"
    fi
    echo "saved the EFI state to $backup"

    rm -rf "$oc/Kexts/RembrandtGPU.kext"
    ditto --norsrc --noextattr "$kext" "$oc/Kexts/RembrandtGPU.kext"
    if ! edit_config "$oc/config.plist" add || ! plutil -lint "$oc/config.plist" >/dev/null; then
        restore_efi_files "$backup" "$oc"
        die "config.plist edit failed; the EFI has been put back as it was"
    fi
    sync
    chown -R "$owner" "$backup"
    echo "installed RembrandtGPU.kext on $dev"
}

efi_restore() {
    dev="$1"; backup="$2"; owner="$3"
    [ -f "$backup/config.plist" ] || die "no config.plist in $backup"
    mount_efi "$dev"
    check_efi
    restore_efi_files "$backup" "$MP/EFI/OC"
    plutil -lint "$MP/EFI/OC/config.plist" >/dev/null || die "restored config.plist does not lint"
    echo "restored the EFI on $dev from $backup"
}

# --- Framebuffer (aux kernel collection) -----------------------------------

# fb_rollback BACKUP: put the live collection and /Library/Extensions back.
fb_rollback() {
    cp "$1/AuxiliaryKernelExtensions.kc" "$LIVE" || true
    rm -rf "$FBEXT"
    if [ "$(cat "$1/had-ext")" = yes ]; then
        cp -R "$1/RBAMDFramebuffer.kext" "$FBEXT" || true
        chown -R root:wheel "$FBEXT" || true
    fi
}

fb_fail() {
    echo "radeonize: $1" >&2
    fb_rollback "$2"
    rm -f "$SCRATCH"
    die "framebuffer install stopped; the previous state is back in place"
}

fb_install() {
    src="$1"; backup="$2"; owner="$3"
    bin="$src/Contents/MacOS/RBAMDFramebuffer"
    [ -f "$bin" ] || die "no RBAMDFramebuffer binary in $src"
    [ -f "$BOOT" ] && [ -f "$SYS" ] && [ -f "$LIVE" ] || die "kernel collections not found"
    uuid="$(dwarfdump --uuid "$bin" | awk '{print $2}')"
    [ -n "$uuid" ] || die "could not read a UUID from $bin"
    echo "installing RBAMDFramebuffer $uuid"

    # Save the live collection and any kext already in /Library/Extensions.
    mkdir -p "$backup"
    cp "$LIVE" "$backup/AuxiliaryKernelExtensions.kc"
    if [ -d "$FBEXT" ]; then
        cp -R "$FBEXT" "$backup/RBAMDFramebuffer.kext"
        echo yes > "$backup/had-ext"
    else
        echo no > "$backup/had-ext"
    fi

    rm -rf "$FBEXT"
    cp -R "$src" "$FBEXT" || fb_fail "could not copy the kext into /Library/Extensions" "$backup"
    chown -R root:wheel "$FBEXT"

    # Dry run to a scratch path. kmutil exits 0 even when it refuses, so the
    # output file and its contents are the real check.
    rm -f "$SCRATCH"
    kmutil create -n aux -a x86_64 --allow-missing-kdk -B "$BOOT" -S "$SYS" -A "$SCRATCH" -r /Library/Extensions >/dev/null 2>&1 || true
    [ -s "$SCRATCH" ] || fb_fail "the dry run wrote nothing (kmutil refused)" "$backup"
    kmutil inspect -B "$SCRATCH" --show-kext-uuids 2>/dev/null | grep -qi "$uuid" \
        || fb_fail "the dry-run collection does not carry $uuid" "$backup"
    echo "dry run carries $uuid"

    # Real write, then verify the UUID inside the live collection.
    kmutil create -n aux -a x86_64 --allow-missing-kdk -B "$BOOT" -S "$SYS" -A "$LIVE" -r /Library/Extensions >/dev/null 2>&1 || true
    kmutil inspect -B "$LIVE" --show-kext-uuids 2>/dev/null | grep -qi "$uuid" \
        || fb_fail "the live collection does not carry $uuid" "$backup"

    rm -f "$SCRATCH"
    sync
    chown -R "$owner" "$backup"
    echo "INSTALLED. $LIVE carries $uuid. Reboot before you test."
}

fb_restore() {
    backup="$1"; owner="$2"
    [ -f "$backup/AuxiliaryKernelExtensions.kc" ] || die "no collection backup in $backup"
    cp "$backup/AuxiliaryKernelExtensions.kc" "$LIVE"
    rm -rf "$FBEXT"
    if [ "$(cat "$backup/had-ext")" = yes ]; then
        cp -R "$backup/RBAMDFramebuffer.kext" "$FBEXT"
        chown -R root:wheel "$FBEXT"
    fi
    sync
    echo "restored $LIVE from $backup. Reboot to take effect."
}

# --- dispatch ----------------------------------------------------------------

cmd="${1:-}"
[ $# -gt 0 ] && shift

check_owner() {
    id "$1" >/dev/null 2>&1 || die "no such user: $1"
}
check_path() {
    case "$1" in /*) ;; *) die "expected an absolute path, got: $1" ;; esac
}

case "$cmd" in
    efi-install)
        [ $# -eq 4 ] || die "usage: efi-install DEVICE STAGE BACKUP OWNER"
        check_path "$2"; check_path "$3"; check_owner "$4"
        efi_install "$@" ;;
    efi-restore)
        [ $# -eq 3 ] || die "usage: efi-restore DEVICE BACKUP OWNER"
        check_path "$2"; check_owner "$3"
        efi_restore "$@" ;;
    fb-install)
        [ $# -eq 3 ] || die "usage: fb-install STAGE BACKUP OWNER"
        check_path "$1"; check_path "$2"; check_owner "$3"
        fb_install "$@" ;;
    fb-restore)
        [ $# -eq 2 ] || die "usage: fb-restore BACKUP OWNER"
        check_path "$1"; check_owner "$2"
        fb_restore "$@" ;;
    *)
        die "unknown command '$cmd'" ;;
esac
