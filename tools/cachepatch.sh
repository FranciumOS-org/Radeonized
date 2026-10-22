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
BK=/Users/you/rbgpu-cache-backup
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

    echo "staging on the same volume ..."
    cp "$NEW" "$SUB.new"
    chown root:admin "$SUB.new"
    chmod 0755 "$SUB.new"
    sync
    echo "atomic rename into place ..."
    mv -f "$SUB.new" "$SUB"
    sync
    echo
    echo "installed. REBOOT for it to take effect."
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
