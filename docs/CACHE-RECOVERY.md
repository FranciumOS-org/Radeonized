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

---

# Installing from Recovery

A running system **cannot** modify the cryptex that holds the shared cache.
Measured on 2026-10-22, boot 123, with every switch that should have allowed
it already off:

    statvfs(.../Cryptexes/OS/System/Library/dyld)   ST_RDONLY = False
    diskutil info /System/Volumes/Preboot           Volume Read-Only: No
    csrutil status                        Filesystem Protections: disabled
    csrutil authenticated-root status                        disabled
    csr-active-config 0xA43                   ALLOW_UNRESTRICTED_FS is set

and yet, as root:

    cp     .../dyld_shared_cache_x86_64h.03.new : Read-only file system
    chflags norestricted .../dyld              : Read-only file system

`chflags` failing the same way is the telling part - it is not the file, it is
the subtree. So the copy has to happen from Recovery, where that policy is not
in force.

## Steps

1. Reboot holding **Cmd-R** (Intel) to enter Recovery.
2. **Utilities -> Terminal**.
3. Find and mount the two volumes. Names vary, so look first:

        diskutil list
        diskutil mount Preboot
        diskutil mount "Macintosh HD - Data"      # whatever your Data volume is called

4. Confirm both paths exist before copying anything:

        ls /Volumes/Preboot/Cryptexes/OS/System/Library/dyld/
        ls /Volumes/*/Users/you/rbgpu-cache-backup/

5. Copy the patched subcache in. Use the real Data mount point from step 4:

        D=/Volumes/Preboot/Cryptexes/OS/System/Library/dyld
        S="/Volumes/Macintosh HD - Data/Users/you/rbgpu-cache-backup"
        chflags norestricted "$D" 2>/dev/null
        cp "$S/patched.03" "$D/dyld_shared_cache_x86_64h.03.new"
        chown 0:80 "$D/dyld_shared_cache_x86_64h.03.new"
        chmod 0755 "$D/dyld_shared_cache_x86_64h.03.new"
        mv -f "$D/dyld_shared_cache_x86_64h.03.new" "$D/dyld_shared_cache_x86_64h.03"
        chflags restricted "$D/dyld_shared_cache_x86_64h.03" 2>/dev/null
        chflags restricted "$D" 2>/dev/null
        sync

6. Reboot normally, then **re-arm** or you come up with no GPU at all:

        sudo nvram rbgpu-arm=1 && sudo reboot

## If it does not boot afterwards

Exactly the same procedure with `dyld_shared_cache_x86_64h.03.orig` in place
of `patched.03`. That file is the untouched original and has a `.sha256`
beside it.

**Doing the install this way has one real advantage:** it exercises the same
Recovery path that the rollback needs, so by the time you might need to undo
it you will already have done it once. That is the thing rule 21 says to do
and that nobody ever does.
