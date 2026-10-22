#!/bin/sh
# Install the patched dyld shared cache subcache (2nw). RUN WITH sudo.
#
#   python3 tools/cachepatch.py build /tmp/patched.03   # first, as yourself
#   sudo tools/cachepatch.sh install /tmp/patched.03
#   sudo tools/cachepatch.sh restore                    # undo
#
# The replace is done with rename(2), never by writing into the original file.
# That file is mapped by the kernel for the shared region of EVERY running
# process; writing into it in place would corrupt a live system immediately.
# A rename swaps the directory entry and leaves the old inode alive until the
# last mapping goes, which is at reboot.
#
# Recovery if the machine does not boot: docs/CACHE-RECOVERY.md
set -e
DIR=/System/Volumes/Preboot/Cryptexes/OS/System/Library/dyld
SUB="$DIR/dyld_shared_cache_x86_64h.03"
BK=/Users/ehsan/rbgpu-cache-backup
ORIG="$BK/dyld_shared_cache_x86_64h.03.orig"

[ "$(id -u)" = 0 ] || { echo "run me with sudo" >&2; exit 1; }

case "${1:-}" in
install)
    NEW=${2:?usage: $0 install <patched.03>}
    [ -f "$NEW" ] || { echo "no such file: $NEW" >&2; exit 1; }
    echo "verifying the patched file before touching anything ..."
    python3 "$(dirname "$0")/cachepatch.py" verify "$NEW"

    mkdir -p "$BK"
    if [ ! -f "$ORIG" ]; then
        echo "backing up the original -> $ORIG"
        cp -p "$SUB" "$ORIG"
        sync
    else
        echo "backup already present, keeping it: $ORIG"
    fi
    # Checksum the backup so a restore can be trusted later.
    [ -f "$ORIG.sha256" ] || shasum -a 256 "$ORIG" > "$ORIG.sha256"

    # The cryptex path carries the 'restricted' flag and the first install
    # attempt failed with EROFS creating the staging file - even though the
    # Preboot mount is read-write (statvfs ST_RDONLY False) and SIP reports
    # "Filesystem Protections: disabled" with ALLOW_UNRESTRICTED_FS set. The
    # flag is enforced by the sealed-OS policy, not by the CSR bits. Clear it
    # for the duration and put it back on every exit path, including failure.
    DFLAGS=$(stat -f %Sf "$DIR")
    FFLAGS=$(stat -f %Sf "$SUB")
    echo "directory flags: ${DFLAGS:-none}   file flags: ${FFLAGS:-none}"
    restore_flags() {
        case "$DFLAGS" in *restricted*) chflags restricted "$DIR" 2>/dev/null || true ;; esac
    }
    trap 'restore_flags' EXIT INT TERM
    case "$DFLAGS" in
        *restricted*)
            echo "clearing the 'restricted' flag on the dyld directory (restored on exit) ..."
            if ! chflags norestricted "$DIR" 2>/dev/null; then
                # chflags itself returning EROFS means the running kernel will
                # not let the cryptex subtree be touched at all, which neither
                # ST_RDONLY, "Filesystem Protections: disabled" nor
                # "Authenticated Root: disabled" accounts for. One thing left
                # to try from a running system: an explicit rw remount.
                echo "chflags refused; trying an explicit rw remount of Preboot ..."
                mount -uw /System/Volumes/Preboot 2>/dev/null || true
                if ! chflags norestricted "$DIR" 2>/dev/null; then
                    echo >&2
                    echo "The cryptex path cannot be modified from a running system." >&2
                    echo "This is NOT a failure of the patch - the patched file verifies." >&2
                    echo "Use the Recovery route: docs/CACHE-RECOVERY.md, section" >&2
                    echo "'Installing from Recovery'." >&2
                    exit 1
                fi
            fi
            ;;
    esac

    echo "staging on the same volume (753 MB) ..."
    if ! cp "$NEW" "$SUB.new"; then
        echo "STAGING COPY FAILED - nothing was changed. df:" >&2
        df -h /System/Volumes/Preboot >&2
        rm -f "$SUB.new" 2>/dev/null || true
        echo "If this is still 'Read-only file system', use the Recovery route" >&2
        echo "in docs/CACHE-RECOVERY.md - flags are not enforced there." >&2
        exit 1
    fi
    chown root:admin "$SUB.new" || { echo "chown failed" >&2; rm -f "$SUB.new"; exit 1; }
    chmod 0755 "$SUB.new"       || { echo "chmod failed" >&2; rm -f "$SUB.new"; exit 1; }
    sync
    echo "staged $(stat -f %z "$SUB.new") bytes; renaming into place ..."
    mv -f "$SUB.new" "$SUB" || { echo "RENAME FAILED" >&2; exit 1; }
    case "$FFLAGS" in *restricted*) chflags restricted "$SUB" 2>/dev/null || true ;; esac
    sync

    # Rule 20, which the first version of this script broke: a script saying
    # "installed" is not evidence that anything changed. Read the live file
    # back and say what is actually there.
    echo
    echo "verifying the LIVE file after the rename:"
    if python3 "$(dirname "$0")/cachepatch.py" verify "$SUB"; then
        echo
        echo "INSTALLED AND VERIFIED ON DISK. REBOOT for it to take effect."
    else
        echo
        echo "THE LIVE FILE DOES NOT CARRY THE PATCH - restoring and aborting." >&2
        cp "$ORIG" "$SUB.restore" && chown root:admin "$SUB.restore" \
            && chmod 0755 "$SUB.restore" && sync && mv -f "$SUB.restore" "$SUB" && sync
        echo "original restored. Nothing to reboot for." >&2
        exit 1
    fi
    echo "If the machine does not boot, see docs/CACHE-RECOVERY.md -"
    echo "the backup is $ORIG"
    ;;
restore)
    [ -f "$ORIG" ] || { echo "no backup at $ORIG" >&2; exit 1; }
    shasum -a 256 -c "$ORIG.sha256" || { echo "BACKUP CHECKSUM FAILED - not restoring" >&2; exit 1; }
    cp "$ORIG" "$SUB.restore"
    chown root:admin "$SUB.restore"; chmod 0755 "$SUB.restore"
    sync
    mv -f "$SUB.restore" "$SUB"
    sync
    echo "restored the original. REBOOT."
    ;;
*)
    sed -n '2,16p' "$0" | sed 's/^# \{0,1\}//'
    exit 2
    ;;
esac
