# HANDOFF — next step

**Written 2026-10-30, after 2xx resolved. `CLAUDE.md` has the standing rules,
the chip facts and the boot-arg contract; this file is only what is in flight.**

---

## THE MACHINE WORKS, ON NO BOOT-ARGS OF OURS BUT ONE.

Boot 247 is an accelerated, smooth desktop at the panel's refresh with the
full default configuration and nothing disarmed. Confirmed on hardware by the
user and by its own counters.

| | state |
|---|---|
| **2xu** Electron/Claude-app artifacts | FIXED, boot 231 |
| **2xv** USB on real interrupts | FIXED, user-confirmed |
| **2xw** armed by default, no boot-args | **WORKING**, boot 247 |
| **2xx** our own `"0"` read as a disarm | **FIXED**, boot 237 then 247 |
| **2xx** the WindowServer crash loop | **FIXED** — it was a stale fbkext, rule 15 |

### Installed, and this is the part that bit

    plugin  build/out/RembrandtGPU.kext      FE3200B0   -> the OpenCore EFI
    fbkext  build/out/RBAMDFramebuffer.kext  353A1D24   -> /Library/Extensions

**Install BOTH, every time, and check both UUIDs in `kextstat` before reading
anything else.** The nine-boot crash loop and the whole boot-arg bisect of
2xx happened because the plugin was reinstalled nine times and the framebuffer
never was; the loaded `C7278266` predated 2xw and had neither the `rbgpu-gen`
rule nor the rule-38 check nor the `RBGPUStock` handshake. `RBFBArmWhy = 10`
is the cheapest proof the new framebuffer is the one running, because only
2xw+ can publish it.

    sudo tools/efi.sh install
    sudo cp -R build/out/RBAMDFramebuffer.kext /Library/Extensions/
    sudo kmutil install --update-all     # the KDK warnings are for the
                                         # debug/kasan variants and are noise;
                                         # check AuxiliaryKernelExtensions.kc's
                                         # mtime and `kmutil inspect -B` it

**Boot-args: `wdt=-1` and nothing of ours.** `rbgpumtlkscope=*` is harmless and
`-rbgpunosentinel` is worth keeping only while rebooting repeatedly - put it
back out for normal use, since it is the only recovery from a boot that dies
in its first minute.

### What boot 247 reads, as the baseline to compare against

    kextstat   plugin FE3200B0   fb 353A1D24
    ioreg      1 RBAMDFramebuffer, 0 IONDRVFramebuffer
    RBFBArmWhy      = 10
    RBFBVBLFires    = 142.0 Hz    against the panel's measured 143.83
    RBFBAmtrSets    = 31.7/s      mostly-idle desktop; 82/s was measured busy
    RBFBHubpWrites  = one per set
    peer: armed=1 arm_why=0 gen=1 tick_armed=1 tick_us=6944
          cap_tbl_patched=1 agdc_on=1 sentinel=2

### Open, and none of it blocking

- **Brightness.** `-rbgpufbbacklight` is built and hosttested and has **never
  been booted** (2xs). Its follow-on is `kConnectionFlags` at
  `src/fbkext/RBAMDFramebuffer.cpp:898` plus `getAppleSense`, which changes
  which `IODisplay` subclass matches, so it needs its own arg and boot.
- **The frame rate ceiling.** 82/s busy against 143.83; never established
  whether that is demand-limited or still gated (2xi/2xo).
- **`-rbgpunocaptbl` is a diagnostic, not a configuration.** Boot 246 measured
  `_ttl_hw_init` returning `kIOReturnError` without the `_DeviceCapabilityTbl`
  clone, so the record is load-bearing for binding. Keep it out of boot-args.
- **The wedge**, independent (2wt/2x0). **HDMI** is unreachable, wired to the
  dGPU (2xq/2xr).

**Read these first, in this order (rule 8, then rule 34):**

    nvram rbgpu-boot                     # must have increased
    ioreg -rl -c RembrandtGPUPeer | grep -o '"RBGPUDiag" = {.*}'
        sentinel      1 armed and the clear is scheduled, 4 THIS IS A RECOVERY
                      BOOT (the previous one did not survive 60 s), 2 opted out
                      with -rbgpunosentinel, 3 /options unreadable, 0 allocate
                      failed
        safe_latch    1 only on a recovery boot
        sentinel_cleared  1 the boot survived its first minute
        arm_why       0 armed by default, 1 /options unreachable (armed anyway),
                      3 rbgpu-arm=0, 5 stock, 6 the latch, 8 a "0" ignored as a
                      spent pre-2xw one-shot (2xx)
        gen           2 migrated THIS boot, 1 already migrated, 0 the write did
                      not read back, 3 /options unreachable
        gen_fixed     how many stale keys were rewritten - expect 2 on the
                      first 2xx boot of this machine and 0 forever after
        tick_armed / tick_oneshot   the regression 2xx fixes: tick_armed must be
                      1, and tick_oneshot 0 (or 8 on the migrating boot), never 3
    ioreg -rl -c RBAMDFramebuffer | grep RBFBArmWhy    # 0 = took the panel

**Said before the boot:**
* `gen 2` + `gen_fixed 2` + `tick_armed 1` + `RBFBArmWhy 0` + a desktop that
  moves without typing = 2xx is done. `gen_fixed 0` with `gen 2` would mean the
  keys had already been cleared by hand and 2xx was never exercised.
* `sentinel 1` + `arm_why 0` + `RBFBArmWhy 0` + an accelerated desktop = 2xw is
  done and the project needs no boot-args.
* `sentinel 4` on the FIRST boot after installing means the install boot itself
  did not survive a minute — read `rbgpu-fbcrumb` and the display log, and
  remember that boot came up **stock**, so nothing in it measures the armed
  configuration (rule 39).
* **No `RBAMDFramebuffer` in `ioreg` at all** = it declined. There is no
  service to publish why, so read it from outside: `-rbgpuoff`/`-rbgpustock`/
  `-rbgpunoownfb` in boot-args, `rbgpu-fb=0`, `rbgpu-arm=0`, `rbgpu-safe` still
  set, or the peer having run first and said stock. Those are the only six
  reasons.
* `RBFBArmWhy` **10** means the framebuffer probed AFTER the peer and took the
  peer's own answer - correct, but it is the opposite of the order 2wg
  measured, so note it: `RBGPUStock` on the peer is then the authoritative
  input and the sentinel latch is not read at all.
* A desktop that is correct but frozen is rule 38's failure: the panel taken
  with no accelerator underneath. One power cycle fixes it by itself now,
  because the sentinel latch will be set.

## IF IT GOES WRONG

In order of bluntness, and none of them needs another OS:

    sudo nvram rbgpu-arm=0          # accelerator off, and the panel with it
    sudo tools/efi.sh bootarg add -rbgpustock    # every default off
    sudo tools/efi.sh bootarg add -rbgpuoff      # the plugin does nothing

and if the machine cannot be driven at all, **do nothing**: the sentinel makes
the next boot stock by itself. That is the whole point of 2xw.

---

## WHAT IS ACTUALLY LEFT

Small, and each one is named in `CLAUDE.md` with its journal tag:

1. **Brightness.** `-rbgpufbbacklight` is built, hosttested, mutation-tested
   and **never booted** (2xs). Arm check first: `RBFBBlArmed` / `RBFBBlReady` /
   `RBFBBlRefuse`. Outstanding paperwork for it: the three new
   `docs/linux-ref/` files in `UPSTREAM.md` and a 2xs journal entry. The
   follow-on that brings the real System Settings slider is making
   `AppleBacklightDisplay` match (`kConnectionFlags` at
   `src/fbkext/RBAMDFramebuffer.cpp:898`, plus `getAppleSense`) — it changes
   which `IODisplay` subclass matches, so it needs its own arg and its own boot.
2. **The last ~22 % of the frame rate** — 82/s against the panel's 143.83. Not
   established whether that is demand-limited or still gated.
3. **The wedge**, the independent second defect. `metal_probe --queues 4`
   reproduces it with no kext, no arming and no reboot (2wt).
4. **HDMI is unreachable** — wired to the dGPU (2xr), not merely unbuilt.

## THE TRAP THAT IS STILL LIVE

**Every Apple address in this repo and in the journal is a macOS 15 address.**
`AMDRadeonX6000MTLDriver __TEXT` moved 0x7FFB08BF3000 → 0x7FFB110D4000 and
CoreDisplay 0x7FF80404C000 → 0x7FF805348000. `tools/re/dscsym.sh` still
hardcodes the old bases. The cache is at
`/System/Volumes/Preboot/Cryptexes/OS/System/Library/dyld/` (7 files) and
`tools/re/dyldcache.py` reads it — `Cache.images()` returns `(loadAddress,
textSize, path)`, `symtab(cache, Image)` returns `(addr, name, ntype)`.
Re-derive before any static read, and note that `mtlk_syms` is a **six-bit**
mask now: all resolved is **63**, not 15.

**And ASan is broken on this toolchain** (CLT 16.4 + macOS 26): a two-line
`printf` program SIGILLs under `-fsanitize=address`, which once made
`efi.sh install` die with a bare `Illegal instruction: 4` that looked exactly
like a failing hosttest. `SAN=auto` probes and degrades loudly; every assertion
passes unsanitized. The real fix is updating CommandLineTools.

## WHAT THE USER RUNS, NOT CLAUDE

`sudo tools/efi.sh install`, `sudo tools/efi.sh bootarg add|remove <arg>`,
`sudo tools/efi.sh usbintr0 on` / `usbpoke off`, `sudo tools/load.sh`,
`sudo tools/unload.sh`, `sudo tools/fbload.sh`, `tools/plugin_check.sh`.
**Claude never runs these and never asks for the password**, never installs
into the EFI or `/Library/Extensions`, and asks before any kext load.
**Never `kmutil unload` the framebuffer kext — it panics**; reboot instead.
Commit freely; **ask before pushing**.
