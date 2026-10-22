# Recovering from a bad shared-cache patch

`tools/cachepatch.sh install` replaces
`/System/Volumes/Preboot/Cryptexes/OS/System/Library/dyld/dyld_shared_cache_x86_64h.03`.
If dyld refuses the modified cache, **nothing launches** - that is not a bad
boot, it is no boot. This is the way back.

The backup is at

    /Users/you/rbgpu-cache-backup/dyld_shared_cache_x86_64h.03.orig

on the **Data** volume, deliberately not on Preboot, with a `.sha256` beside it.

## First try: the machine still boots but apps misbehave

    sudo tools/cachepatch.sh restore
    sudo reboot

## If it does not boot

1. Power on holding **Cmd-R** (Intel) for Recovery.
2. **Utilities -> Terminal**.
3. Find the volumes - Recovery mounts them under `/Volumes`:

        diskutil list
        diskutil mount <the Preboot volume identifier>
        diskutil mount <the Data volume identifier>

4. Copy the backup back. The paths under Recovery are prefixed by the mount
   point, so check with `ls` before copying:

        ls /Volumes/Preboot/Cryptexes/OS/System/Library/dyld/
        cp "/Volumes/Macintosh HD - Data/Users/you/rbgpu-cache-backup/dyld_shared_cache_x86_64h.03.orig" \
           /Volumes/Preboot/Cryptexes/OS/System/Library/dyld/dyld_shared_cache_x86_64h.03.restore
        mv -f /Volumes/Preboot/Cryptexes/OS/System/Library/dyld/dyld_shared_cache_x86_64h.03.restore \
              /Volumes/Preboot/Cryptexes/OS/System/Library/dyld/dyld_shared_cache_x86_64h.03

5. Reboot.

## If Recovery itself will not come up

Recovery has its own cache and is not affected by this file. Failing that, an
**external macOS installer USB** boots independently and its Terminal can do
exactly the same copy.

## Why this is recoverable at all

The install never writes into the live file. It stages a copy on the same
volume and uses `rename(2)`, so the inode every running process has mapped is
untouched and only the directory entry changes. Nothing is destroyed; the
original bytes are in the backup and the old inode survives until reboot.
