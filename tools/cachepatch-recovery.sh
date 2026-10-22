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

# Recovery runs from its OWN APFS container. Measured here on 2026-10-22:
#
#   /dev/disk3s1 on /                        <- Recovery's own system volume
#   /dev/disk3s2 on /System/Volumes/Preboot  <- Recovery's own Preboot
#   /dev/disk4s1 on /Volumes/MACOS - Data    <- the real install
#   /dev/disk4s4 on /Volumes/MACOS           <- the real install
#
# So /System/Volumes/Preboot in Recovery is the WRONG Preboot and has no
# Cryptexes at all, and the right one (disk4s2) is not mounted. Device numbers
# also differ from the booted system (disk2 there), so nothing may be
# hardcoded. The only reliable anchor is the backup folder: whatever container
# holds it is the user's install, and that container's Preboot is the target.

find_backup_dir() {
    for d in /Volumes/*/Users/*/rbgpu-cache-backup \
             /System/Volumes/Data/Users/*/rbgpu-cache-backup \
             /Users/*/rbgpu-cache-backup; do
        [ -d "$d" ] && { echo "$d"; return 0; }
    done
    return 1
}
mount_point_of() { df "$1" 2>/dev/null | tail -1 | awk '{for(i=9;i<=NF;i++)printf "%s%s",$i,(i<NF?" ":"")}'; }
container_of()   { df "$1" 2>/dev/null | tail -1 | awk '{print $1}' | sed 's|/dev/||; s|s[0-9][0-9]*$||'; }
mount_point_of_dev() {
    diskutil info "$1" 2>/dev/null | awk -F: '/Mount Point/ {sub(/^ +/,"",$2); print $2; exit}'
}

BK=$(find_backup_dir) || die "no rbgpu-cache-backup found - run: diskutil mount \"MACOS - Data\""
say "backup folder  : $BK"
CONT=$(container_of "$BK")
[ -n "$CONT" ] || die "could not work out which APFS container holds $BK"
say "that container : $CONT  (this is your install, not Recovery's)"

PB=$(diskutil list "$CONT" 2>/dev/null | awk '/APFS Volume Preboot/ {print $NF; exit}')
[ -n "$PB" ] || { diskutil list "$CONT"; die "no Preboot volume in $CONT"; }
say "its Preboot    : $PB"

PBMP=$(mount_point_of_dev "$PB")
case "$PBMP" in
    ""|"Not applicable"*|"Not mounted"*)
        say "not mounted - mounting it ..."
        diskutil mount "$PB" >/dev/null 2>&1 || die "could not mount $PB"
        PBMP=$(mount_point_of_dev "$PB")
        ;;
esac
[ -n "$PBMP" ] || die "$PB has no mount point even after mounting"
say "mounted at     : $PBMP"

DIR="$PBMP/Cryptexes/OS/System/Library/dyld"
[ -d "$DIR" ] || {
    say "contents of $PBMP:"; ls "$PBMP" 2>/dev/null | sed 's/^/  /'
    die "no Cryptexes/OS/System/Library/dyld under $PBMP"
}
say "dyld directory : $DIR"

SUB="$DIR/$SUBNAME"
[ -f "$SUB" ] || die "no $SUBNAME in $DIR"
say "current bytes  : $(sig_at "$SUB")"

# Recovery mounts volumes read-only. Remount in place.
if mount | grep -F " on ${PBMP} " | grep -q read-only; then
    say "it is mounted READ-ONLY - remounting read-write ..."
    mount -uw "$PBMP" || die "could not remount $PBMP read-write"
    mount | grep -F " on ${PBMP} " | grep -q read-only && die "$PBMP is STILL read-only"
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
