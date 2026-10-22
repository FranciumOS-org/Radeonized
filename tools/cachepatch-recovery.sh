#!/bin/sh
# Install or restore the patched dyld subcache FROM RECOVERY (2nx).
#
# A running system cannot touch the cryptex that holds the shared cache - not
# the file, not even its flags - with ST_RDONLY False, Filesystem Protections
# disabled, Authenticated Root disabled and ALLOW_UNRESTRICTED_FS set. Both
# `cp` and `chflags` return EROFS. Recovery is not under that policy.
#
# Getting there on THIS machine: it is a hackintosh, so Cmd-R does nothing
# (PollAppleHotKeys is False). At the OpenCore picker press SPACE - that stops
# the 3 s countdown and un-hides auxiliary entries, where macOS Recovery is
# (HideAuxiliary is True, ScanPolicy is 0 so it is scanned).
#
# In Recovery:  Utilities -> Terminal, then
#
#   diskutil mount Preboot
#   diskutil mount "MACOS - Data"
#   sh "/Volumes/MACOS - Data/Users/ehsan/Developer/RembrandtGPU/tools/cachepatch-recovery.sh" install
#   sh "/Volumes/MACOS - Data/Users/ehsan/Developer/RembrandtGPU/tools/cachepatch-recovery.sh" restore
#
# No python and no perl: Recovery has neither. Verification is dd + od, which
# it does have. Rule 20 still applies here - the script reads the bytes back
# out of the installed file and says what is actually there.
set -u
SUBNAME=dyld_shared_cache_x86_64h.03
BLK=36149          # 0x8d35a11 / 4096
IN=2577            # 0x8d35a11 % 4096
PATCHED_SIG="e9 ea ff 3b 00"
ORIG_SIG="55 48 89 e5 41"

say() { echo "$@"; }
die() { echo "ERROR: $*" >&2; exit 1; }

# Recovery does NOT mount Preboot under /Volumes. Measured on this machine
# (2026-10-22, in Recovery): it is already mounted at /System/Volumes/Preboot,
# READ-ONLY, on a device whose number differs from the booted system's
# (disk3s2 there, disk2s2 when booted). Mounting it again at /Volumes/Preboot
# fails with "Resource busy" because it is already mounted. So look in both
# places, never assume a device number, and remount read-write in place.
SEARCH_DIRS="/System/Volumes/Preboot/Cryptexes/OS/System/Library/dyld
/Volumes/Preboot/Cryptexes/OS/System/Library/dyld"
find_dyld_dir() {
    for d in $SEARCH_DIRS; do
        [ -d "$d" ] && { echo "$d"; return 0; }
    done
    for d in /Volumes/*/Cryptexes/OS/System/Library/dyld; do
        [ -d "$d" ] && { echo "$d"; return 0; }
    done
    return 1
}
find_backup_dir() {
    for d in /Volumes/*/Users/*/rbgpu-cache-backup \
             /System/Volumes/Data/Users/*/rbgpu-cache-backup \
             /Users/*/rbgpu-cache-backup; do
        [ -d "$d" ] && { echo "$d"; return 0; }
    done
    return 1
}
# mount point that actually holds a path, without assuming a device number
mount_point_of() { df "$1" 2>/dev/null | tail -1 | awk '{for(i=9;i<=NF;i++)printf "%s%s",$i,(i<NF?" ":"")}'; }
# first five bytes of the function, without python
sig_at() { dd if="$1" bs=4096 skip=$BLK count=1 2>/dev/null | od -An -tx1 -j $IN -N 5 | tr -s ' ' | sed 's/^ //;s/ $//'; }

DIR=$(find_dyld_dir) || {
    say "searched:"; for d in $SEARCH_DIRS /Volumes/*/Cryptexes/OS/System/Library/dyld; do say "  $d"; done
    say ""; say "mounted apfs volumes:"; mount | grep apfs | sed 's/^/  /'
    die "no Cryptexes dyld directory found. If Preboot is not listed above, run: diskutil mount Preboot"
}
BK=$(find_backup_dir) || die "no rbgpu-cache-backup found - mount your Data volume (diskutil mount \"MACOS - Data\")"
SUB="$DIR/$SUBNAME"
say "dyld directory : $DIR"
say "backup folder  : $BK"
[ -f "$SUB" ] || die "no $SUBNAME in $DIR"
say "current bytes  : $(sig_at "$SUB")"

# Recovery mounts Preboot read-only. Remount in place; mounting it a second
# time elsewhere just returns "Resource busy".
MP=$(mount_point_of "$DIR")
say "mount point    : ${MP:-unknown}"
if mount | grep -F " on ${MP} " | grep -q read-only; then
    say "it is mounted READ-ONLY - remounting read-write ..."
    mount -uw "$MP" || die "could not remount $MP read-write"
    mount | grep -F " on ${MP} " | grep -q read-only && die "$MP is STILL read-only"
    say "remounted read-write."
fi

case "${1:-}" in
install) SRC="$BK/patched.03";                 WANT="$PATCHED_SIG"; WORD="patched" ;;
restore) SRC="$BK/$SUBNAME.orig";              WANT="$ORIG_SIG";    WORD="original" ;;
*) sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'; exit 2 ;;
esac

[ -f "$SRC" ] || die "source missing: $SRC"
say "source         : $SRC  ($(($(wc -c < "$SRC") / 1048576)) MB)"
[ "$(wc -c < "$SRC")" = "$(wc -c < "$SUB")" ] || die "source and target differ in SIZE - refusing"
say "source bytes   : $(sig_at "$SRC")   (expecting $WANT)"
[ "$(sig_at "$SRC")" = "$WANT" ] || die "the source does not carry the $WORD signature - refusing"

say ""
say "installing the $WORD subcache ..."
chflags norestricted "$DIR" 2>/dev/null
cp "$SRC" "$SUB.new" || die "copy failed (space? $(df -h "$DIR" | tail -1))"
chown 0:80 "$SUB.new" 2>/dev/null
chmod 0755 "$SUB.new" 2>/dev/null
mv -f "$SUB.new" "$SUB" || die "rename failed"
chflags restricted "$SUB" 2>/dev/null
chflags restricted "$DIR" 2>/dev/null
sync

# Rule 20: read it back rather than announcing success.
GOT=$(sig_at "$SUB")
say ""
say "bytes now in place: $GOT"
if [ "$GOT" = "$WANT" ]; then
    say "OK - the $WORD subcache is installed. Reboot."
    [ "${1:-}" = install ] && say "After booting, re-arm or you get no GPU:  sudo nvram rbgpu-arm=1 && sudo reboot"
    exit 0
fi
say "MISMATCH - expected $WANT. The volume may not have taken the write." >&2
exit 1
