#!/bin/sh
# radeonize-helper.sh - the only part of Radeonize that runs as root.
#
# The app calls it through one administrator prompt per step:
#
#   efi-install  DEVICE STAGE BACKUP OWNER           add RembrandtGPU.kext to the EFI after Lilu
#   efi-restore  DEVICE BACKUP OWNER                 put the saved EFI state back
#   fb-install   KEXT BACKUP OWNER UUID              install RBAMDFramebuffer.kext into the aux collection
#   fb-restore   BACKUP OWNER                        put the saved aux collection (and kext) back
#
# Uses only tools in the base system. No Xcode or Command Line Tools: the app
# reads the framebuffer's UUID itself and passes it in, and config.plist is
# edited with plutil.
#
# Progress: the app sets RADEONIZE_PROGRESS to a log file it reads while this
# script runs. "@ K/N LABEL" starts step K of N; any other line is a log line.
# Writing to that file never stops a step.
#
# Each step saves what it is about to replace, writes, and verifies before it
# returns. The logic follows tools/efi.sh and tools/fbinstall.sh in the
# Radeonized repo; keep the two in step when either changes.
set -eu

say() {
    [ -n "${RADEONIZE_PROGRESS:-}" ] && printf '%s\n' "$*" >> "$RADEONIZE_PROGRESS" 2>/dev/null
    return 0
}
step() { say "@ $1/$2 $3"; }
warn() { say "radeonize: $*"; echo "radeonize: $*" >&2; }
die() { warn "$*"; exit 1; }

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
    say "  EFI is mounted at $MP"
}

check_efi() {
    [ -f "$MP/EFI/OC/config.plist" ] || die "$DEV has no EFI/OC/config.plist; it is not an OpenCore EFI"
    say "  found EFI/OC/config.plist"
}

# config.plist helpers. Each uses plutil only; a missing key or index makes plutil
# exit non-zero, which is how the loops below find the end of Kernel -> Add.
add_count() {   # CONFIG -> number of entries in Kernel -> Add
    i=0
    while plutil -type "Kernel.Add.$i" "$1" >/dev/null 2>&1; do i=$((i+1)); done
    echo "$i"
}
bundle_at() {   # CONFIG INDEX -> BundlePath of that entry, or empty
    plutil -extract "Kernel.Add.$2.BundlePath" raw -o - "$1" 2>/dev/null || true
}
enabled_at() {  # CONFIG INDEX -> true or false, or empty
    plutil -extract "Kernel.Add.$2.Enabled" raw -o - "$1" 2>/dev/null || true
}

# edit_config CONFIG add|remove: touches only the RembrandtGPU entry in Kernel -> Add.
edit_config() {
    cfg="$1"; op="$2"; tmp="$cfg.radeonize"
    cp "$cfg" "$tmp"

    # 1. Drop our entries, highest index first so the lower indices stay valid.
    n="$(add_count "$tmp")"
    i=$((n - 1))
    while [ "$i" -ge 0 ]; do
        if [ "$(bundle_at "$tmp" "$i")" = "RembrandtGPU.kext" ]; then
            plutil -remove "Kernel.Add.$i" "$tmp"
            say "  removed an old RembrandtGPU entry (index $i)"
        fi
        i=$((i - 1))
    done

    # 2. For add, place the entry right after the first enabled Lilu.kext.
    if [ "$op" = add ]; then
        n="$(add_count "$tmp")"
        lilu=""
        i=0
        while [ "$i" -lt "$n" ]; do
            if [ -z "$lilu" ] && [ "$(bundle_at "$tmp" "$i")" = "Lilu.kext" ] && [ "$(enabled_at "$tmp" "$i")" = true ]; then
                lilu="$i"
            fi
            i=$((i + 1))
        done
        if [ -z "$lilu" ]; then
            rm -f "$tmp"
            warn "no enabled Lilu.kext in Kernel -> Add"
            return 1
        fi
        say "  placing RembrandtGPU after Lilu (index $lilu)"
        plutil -insert "Kernel.Add.$((lilu + 1))" -xml \
            '<dict><key>Arch</key><string>x86_64</string><key>BundlePath</key><string>RembrandtGPU.kext</string><key>Comment</key><string>RembrandtGPU (Radeon 680M)</string><key>Enabled</key><true/><key>ExecutablePath</key><string>Contents/MacOS/RembrandtGPU</string><key>MaxKernel</key><string></string><key>MinKernel</key><string></string><key>PlistPath</key><string>Contents/Info.plist</string></dict>' \
            "$tmp"
    fi

    plutil -lint "$tmp" >/dev/null || { rm -f "$tmp"; warn "edited config does not lint"; return 1; }
    mv "$tmp" "$cfg"
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

    step 1 8 "Checking the RembrandtGPU.kext from the release"
    [ -f "$kext/Contents/MacOS/RembrandtGPU" ] || die "no RembrandtGPU.kext binary in $stage"
    plutil -lint "$kext/Contents/Info.plist" >/dev/null || die "Info.plist in $kext does not lint"

    step 2 8 "Mounting the EFI partition $dev"
    mount_efi "$dev"

    step 3 8 "Checking OpenCore's config.plist"
    check_efi
    oc="$MP/EFI/OC"
    plutil -type Kernel.Add "$oc/config.plist" >/dev/null 2>&1 \
        || die "$dev: config.plist has no Kernel -> Add array"
    say "  config.plist has a Kernel -> Add list"

    step 4 8 "Saving the current EFI state"
    mkdir -p "$backup"
    echo "$dev" > "$backup/efi-device"
    cp "$oc/config.plist" "$backup/config.plist"
    if [ -d "$oc/Kexts/RembrandtGPU.kext" ]; then
        cp -R "$oc/Kexts/RembrandtGPU.kext" "$backup/RembrandtGPU.kext"
        echo yes > "$backup/had-kext"
        say "  kept the existing RembrandtGPU.kext"
    else
        echo no > "$backup/had-kext"
    fi
    say "  saved to $backup"

    step 5 8 "Copying RembrandtGPU.kext into the EFI"
    rm -rf "$oc/Kexts/RembrandtGPU.kext"
    ditto --norsrc --noextattr "$kext" "$oc/Kexts/RembrandtGPU.kext"

    # Rule 15: check the copy is the staged binary, not just that a file exists.
    step 6 8 "Checking the copied binary is the one from the release"
    if ! cmp -s "$kext/Contents/MacOS/RembrandtGPU" "$oc/Kexts/RembrandtGPU.kext/Contents/MacOS/RembrandtGPU"; then
        restore_efi_files "$backup" "$oc"
        die "the copied kext does not match the staged binary; the EFI has been put back as it was"
    fi

    step 7 8 "Adding RembrandtGPU.kext to config.plist after Lilu"
    if ! edit_config "$oc/config.plist" add; then
        restore_efi_files "$backup" "$oc"
        die "config.plist edit failed; the EFI has been put back as it was"
    fi

    step 8 8 "Writing to disk"
    sync
    chown -R "$owner" "$backup"
    echo "installed RembrandtGPU.kext on $dev"
}

efi_restore() {
    dev="$1"; backup="$2"; owner="$3"

    step 1 4 "Checking the saved EFI state in $backup"
    [ -f "$backup/config.plist" ] || die "no config.plist in $backup"

    step 2 4 "Mounting the EFI partition $dev"
    mount_efi "$dev"
    check_efi

    step 3 4 "Putting back config.plist and RembrandtGPU.kext"
    restore_efi_files "$backup" "$MP/EFI/OC"

    step 4 4 "Checking the restored config.plist"
    plutil -lint "$MP/EFI/OC/config.plist" >/dev/null || die "restored config.plist does not lint"
    echo "restored the EFI on $dev from $backup"
}

# --- Framebuffer (aux kernel collection) -----------------------------------

# had_aux BACKUP: a backup from before had-aux existed always had a collection.
had_aux() {
    [ "$(cat "$1/had-aux" 2>/dev/null || echo yes)" = yes ]
}

# fb_rollback BACKUP: put the live collection and /Library/Extensions back.
fb_rollback() {
    if had_aux "$1"; then
        cp "$1/AuxiliaryKernelExtensions.kc" "$LIVE" || true
    else
        rm -f "$LIVE" || true
    fi
    rm -rf "$FBEXT"
    if [ "$(cat "$1/had-ext" 2>/dev/null || echo yes)" = yes ] && [ -d "$1/RBAMDFramebuffer.kext" ]; then
        cp -R "$1/RBAMDFramebuffer.kext" "$FBEXT" || true
        chown -R root:wheel "$FBEXT" || true
    fi
    say "  the previous collection and kext are back in place"
}

fb_fail() {
    warn "$1"
    fb_rollback "$2"
    rm -f "$SCRATCH"
    die "framebuffer install stopped; the previous state is back in place"
}

fb_install() {
    src="$1"; backup="$2"; owner="$3"; uuid="$4"
    bin="$src/Contents/MacOS/RBAMDFramebuffer"

    step 1 8 "Checking the kernel collections and the UUID"
    [ -f "$bin" ] || die "no RBAMDFramebuffer binary in $src"
    [ -f "$BOOT" ] && [ -f "$SYS" ] || die "kernel collections not found"
    echo "$uuid" | grep -qE '^[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{12}$' \
        || die "not a UUID: $uuid"
    say "  RBAMDFramebuffer $uuid"

    step 2 8 "Saving the current framebuffer state"
    # Save the live collection (if there is one) and any kext already in /Library/Extensions.
    mkdir -p "$backup"
    if [ -f "$LIVE" ]; then
        cp "$LIVE" "$backup/AuxiliaryKernelExtensions.kc"
        echo yes > "$backup/had-aux"
    else
        echo no > "$backup/had-aux"
    fi
    if [ -d "$FBEXT" ]; then
        cp -R "$FBEXT" "$backup/RBAMDFramebuffer.kext"
        echo yes > "$backup/had-ext"
    else
        echo no > "$backup/had-ext"
    fi
    say "  saved to $backup"

    step 3 8 "Copying RBAMDFramebuffer.kext into /Library/Extensions"
    rm -rf "$FBEXT"
    cp -R "$src" "$FBEXT" || fb_fail "could not copy the kext into /Library/Extensions" "$backup"
    chown -R root:wheel "$FBEXT"

    step 4 8 "Checking the copied binary is the one from the release"
    cmp -s "$bin" "$FBEXT/Contents/MacOS/RBAMDFramebuffer" \
        || fb_fail "the copied framebuffer binary does not match the staged one" "$backup"

    # Dry run to a scratch path. kmutil exits 0 even when it refuses, so the
    # output file and its contents are the real check.
    step 5 8 "Test-building an aux collection (a dry run; this can take a few minutes)"
    rm -f "$SCRATCH"
    kmutil create -n aux -a x86_64 --allow-missing-kdk -B "$BOOT" -S "$SYS" -A "$SCRATCH" -r /Library/Extensions \
        >"$backup/kmutil-dry-run.txt" 2>&1 || true
    [ -s "$SCRATCH" ] || fb_fail "the dry run wrote nothing (kmutil refused; see kmutil-dry-run.txt in the backup)" "$backup"

    step 6 8 "Checking the test collection carries the new UUID"
    kmutil inspect -B "$SCRATCH" --show-kext-uuids 2>/dev/null | grep -qiF "$uuid" \
        || fb_fail "the dry-run collection does not carry $uuid" "$backup"

    # Real write, then verify the UUID inside the live collection. The directory
    # is created if it is missing, so a Mac with no aux collection is not an error.
    step 7 8 "Building the live aux collection (this can take a few minutes)"
    mkdir -p "$(dirname "$LIVE")"
    kmutil create -n aux -a x86_64 --allow-missing-kdk -B "$BOOT" -S "$SYS" -A "$LIVE" -r /Library/Extensions \
        >"$backup/kmutil-live.txt" 2>&1 || true

    step 8 8 "Checking the live collection carries the new UUID"
    kmutil inspect -B "$LIVE" --show-kext-uuids 2>/dev/null | grep -qiF "$uuid" \
        || fb_fail "the live collection does not carry $uuid" "$backup"

    rm -f "$SCRATCH"
    sync
    chown -R "$owner" "$backup"
    echo "INSTALLED. $LIVE carries $uuid. Reboot before you test."
}

fb_restore() {
    backup="$1"; owner="$2"

    step 1 3 "Putting back the saved aux collection"
    if had_aux "$backup"; then
        [ -f "$backup/AuxiliaryKernelExtensions.kc" ] || die "no collection backup in $backup"
        cp "$backup/AuxiliaryKernelExtensions.kc" "$LIVE"
    else
        # There was no collection before the install, so there should be none now.
        rm -f "$LIVE"
    fi

    step 2 3 "Putting back RBAMDFramebuffer.kext"
    rm -rf "$FBEXT"
    if [ "$(cat "$backup/had-ext" 2>/dev/null || echo yes)" = yes ] && [ -d "$backup/RBAMDFramebuffer.kext" ]; then
        cp -R "$backup/RBAMDFramebuffer.kext" "$FBEXT"
        chown -R root:wheel "$FBEXT"
    fi

    step 3 3 "Writing to disk"
    sync
    echo "restored the framebuffer state from $backup. Reboot to take effect."
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
        [ $# -eq 4 ] || die "usage: fb-install KEXT BACKUP OWNER UUID"
        check_path "$1"; check_path "$2"; check_owner "$3"
        fb_install "$@" ;;
    fb-restore)
        [ $# -eq 2 ] || die "usage: fb-restore BACKUP OWNER"
        check_path "$1"; check_owner "$2"
        fb_restore "$@" ;;
    *)
        die "unknown command '$cmd'" ;;
esac
