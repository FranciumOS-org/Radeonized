#!/bin/sh
# Install or restore the patched dyld subcache FROM RECOVERY (2nx/2ny).
#
# A booted macOS cannot touch the cryptex that holds the shared cache: both
# `cp` and `chflags` return EROFS with ST_RDONLY False, Filesystem Protections
# disabled, Authenticated Root disabled and ALLOW_UNRESTRICTED_FS set.
#
# Getting into Recovery on THIS machine: it is a hackintosh, so Cmd-R does
# nothing (PollAppleHotKeys is False). At the OpenCore picker press SPACE -
# that stops the 3 s countdown and un-hides auxiliary entries, where macOS
# Recovery is (HideAuxiliary True, ScanPolicy 0).
#
#   diskutil mount "MACOS - Data"
#   sh "/Volumes/MACOS - Data/Users/ehsan/Developer/RembrandtGPU/tools/cachepatch-recovery.sh" install
#   sh "/Volumes/MACOS - Data/Users/ehsan/Developer/RembrandtGPU/tools/cachepatch-recovery.sh" restore
#
# Nothing is assumed about paths or device numbers, because every assumption
# made so far has been wrong on this machine:
#
#  * Recovery boots from its OWN APFS container, so /System/Volumes/Preboot
#    there is Recovery's Preboot and has no Cryptexes at all; the real one was
#    disk4s2, unmounted, while the booted system calls the same thing disk2s2.
#    The only stable anchor is the backup folder - whatever container holds it
#    is the user's install.
#  * On the raw volume the booted-system path
#    Cryptexes/OS/System/Library/dyld did not exist, although when booted it
#    is a plain directory on that very volume. So the subcache is SEARCHED
#    for, and every copy found is patched - there are two on this machine,
#    Cryptexes/OS/... and Cryptexes/Incoming/OS/... , separate inodes with
#    identical contents.
#
# No python and no perl: Recovery has had neither since 12.3. Verification is
# dd + od. Rule 20 applies - every file is read back after writing.
set -u
SUBNAME=dyld_shared_cache_x86_64h.03
BLK=36149          # 0x8d35a11 / 4096
IN=2577            # 0x8d35a11 % 4096
PATCHED_SIG="e9 ea ff 3b 00"
ORIG_SIG="55 48 89 e5 41"

say() { echo "$@"; }
die() { echo "ERROR: $*" >&2; exit 1; }
sig_at() { dd if="$1" bs=4096 skip=$BLK count=1 2>/dev/null | od -An -tx1 -j $IN -N 5 | tr -s ' ' | sed 's/^ //;s/ $//'; }
container_of() { df "$1" 2>/dev/null | tail -1 | awk '{print $1}' | sed 's|/dev/||; s|s[0-9][0-9]*$||'; }
mp_of_dev() { diskutil info "$1" 2>/dev/null | awk -F: '/Mount Point/ {sub(/^ +/,"",$2); print $2; exit}'; }

case "${1:-}" in install|restore) ACTION=$1 ;; *) sed -n '2,30p' "$0" | sed 's/^# \{0,1\}//'; exit 2 ;; esac

# ---- find the user's install via the backup folder ----
BK=""
for d in /Volumes/*/Users/*/rbgpu-cache-backup /System/Volumes/Data/Users/*/rbgpu-cache-backup /Users/*/rbgpu-cache-backup; do
    [ -d "$d" ] && { BK=$d; break; }
done
[ -n "$BK" ] || die "no rbgpu-cache-backup found - run: diskutil mount \"MACOS - Data\""
say "backup folder  : $BK"
CONT=$(container_of "$BK"); [ -n "$CONT" ] || die "cannot determine the container of $BK"
say "your container : $CONT   (Recovery's own is a different one)"
PB=$(diskutil list "$CONT" 2>/dev/null | awk '/APFS Volume Preboot/ {print $NF; exit}')
[ -n "$PB" ] || { diskutil list "$CONT"; die "no Preboot volume in $CONT"; }
PBMP=$(mp_of_dev "$PB")
case "$PBMP" in ""|"Not applicable"*|"Not mounted"*)
    say "Preboot $PB not mounted - mounting ..."
    diskutil mount "$PB" >/dev/null 2>&1 || die "could not mount $PB"
    PBMP=$(mp_of_dev "$PB") ;;
esac
[ -n "$PBMP" ] || die "$PB has no mount point"
say "Preboot        : $PB at $PBMP"

if mount | grep -F " on ${PBMP} " | grep -q read-only; then
    say "remounting $PBMP read-write ..."
    mount -uw "$PBMP" || die "could not remount $PBMP read-write"
    mount | grep -F " on ${PBMP} " | grep -q read-only && die "$PBMP is STILL read-only"
fi

# ---- always show the structure; it is what three trips were spent learning ----
say ""
say "structure of $PBMP:"
ls -1 "$PBMP" 2>/dev/null | sed 's/^/    /'
for sub in Cryptexes Cryptexes/OS Cryptexes/OS/System/Library/dyld Cryptexes/Incoming/OS/System/Library/dyld com.apple.security.cryptexd; do
    say "  $sub:"; ls -1 "$PBMP/$sub" 2>/dev/null | head -8 | sed 's/^/    /' || say "    (absent)"
done

# ---- every copy of the subcache on that volume ----
say ""
say "searching $PBMP for $SUBNAME ..."
TARGETS=$(find "$PBMP" -type f -name "$SUBNAME" 2>/dev/null)
[ -n "$TARGETS" ] || {
    say "none found. Any dyld cache at all:"
    find "$PBMP" -name 'dyld_shared_cache*' 2>/dev/null | head -10 | sed 's/^/    /'
    say "Disk images (a cryptex payload may be one):"
    find "$PBMP" \( -name '*.dmg' -o -name '*.aar' -o -name '*.img4' \) 2>/dev/null | head -10 | sed 's/^/    /'
    die "cannot locate $SUBNAME on $PBMP - send this listing"
}
echo "$TARGETS" | sed 's/^/    /'

if [ "$ACTION" = install ]; then SRC="$BK/patched.03"; WANT="$PATCHED_SIG"; WORD=patched
else SRC="$BK/$SUBNAME.orig"; WANT="$ORIG_SIG"; WORD=original; fi
[ -f "$SRC" ] || die "source missing: $SRC"
say ""
say "source         : $SRC"
say "source bytes   : $(sig_at "$SRC")   (expecting $WANT)"
[ "$(sig_at "$SRC")" = "$WANT" ] || die "the source does not carry the $WORD signature - refusing"

OK=0; FAIL=0
echo "$TARGETS" | while IFS= read -r T; do
    [ -n "$T" ] || continue
    say ""
    say "--> $T"
    say "    before: $(sig_at "$T")   size $(wc -c < "$T")"
    if [ "$(wc -c < "$SRC")" != "$(wc -c < "$T")" ]; then
        say "    SIZE MISMATCH - skipping this one"; continue
    fi
    TD=$(dirname "$T")
    chflags norestricted "$TD" 2>/dev/null
    chflags norestricted "$T" 2>/dev/null
    if ! cp "$SRC" "$T.new" 2>/dev/null; then
        say "    copy FAILED (still read-only?) - skipping"; rm -f "$T.new" 2>/dev/null; continue
    fi
    chown 0:80 "$T.new" 2>/dev/null; chmod 0755 "$T.new" 2>/dev/null
    mv -f "$T.new" "$T" 2>/dev/null || { say "    rename FAILED"; continue; }
    chflags restricted "$T" 2>/dev/null
    chflags restricted "$TD" 2>/dev/null
    sync
    G=$(sig_at "$T")
    if [ "$G" = "$WANT" ]; then say "    after : $G   OK"; else say "    after : $G   MISMATCH"; fi
done

say ""
say "Done. Every line above that says OK is a copy now carrying the $WORD cache."
say "If at least one says OK, reboot. Then:  sudo nvram rbgpu-arm=1 && sudo reboot"
