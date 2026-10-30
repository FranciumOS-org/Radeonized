# HANDOFF — next step

**Written 2026-10-30, after 2xw. `docs/RULES.md` has the standing rules, the chip
facts and the boot-arg contract; this file is only what is in flight.**

---

## THE MACHINE WORKS. THE NEXT BOOT IS THE FIRST ONE THAT NEEDS NO BOOT-ARGS.

Three things landed on 2026-10-30 and two of them are already confirmed on
hardware:

| | state |
|---|---|
| **2xu** the Electron/the assistant-app artifacts | **FIXED, boot 231** — `mtlk2_uc_hk 1`, `mtlk2_uc 196`, `mtlk2_patched 43`, zero failures |
| **2xv** USB on real interrupts | **FIXED**, user-confirmed, the 10 ms poll is gone |
| **2xw** armed by default, no boot-args | **BUILT, NOT YET BOOTED** — this is the next boot |

## WHAT TO INSTALL AND WHAT TO READ

    plugin  build/out/RembrandtGPU.kext      64957171     -> the OpenCore EFI
    fbkext  build/out/RBAMDFramebuffer.kext  33D9C184     -> the auxiliary collection

    sudo tools/efi.sh install          # the plugin
    # the framebuffer is the user's install into /Library/Extensions

**Boot-args: take ours out**, with `sudo tools/efi.sh defaults` - one edit,
and it also prints the state of the five keys that would disarm a half. Every
`-rbgpu*` token in `rb_defaults.h` can go; they are the
default now and any that stay are redundant, not harmful, so the install is
safe either way. **Keep `wdt=-1`** — `-rbgpuforceenable` is default-on and that
is Apple's arg, which we cannot set (2hh: without it a frozen boot panics at
120 s with no SSH window). Keep `keepsyms=1`, `msgbuf=1048576`, `-wegnoegpu`,
`amfi*`, `alcid=66`, `xhcipoke=10` (inert once 2xv's patches are in, and the
poke's counters are still the instrument for them) and anything not ours.

**NVRAM: nothing to set.** The five one-shots are no longer arms. If
`rbgpu-arm`, `rbgpu-fb`, `rbgpu-agdc`, `rbgpu-iop` or `rbgpu-tick` is left
over from an earlier boot it is harmless unless it reads `0`, which now means
*disarmed* — so clear any that do:

    nvram rbgpu-arm rbgpu-fb rbgpu-agdc rbgpu-iop rbgpu-tick 2>&1   # look first
    sudo nvram -d rbgpu-fb                                          # if one reads 0

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
                      3 rbgpu-arm=0, 5 stock, 6 the latch
    ioreg -rl -c RBAMDFramebuffer | grep RBFBArmWhy    # 0 = took the panel

**Said before the boot:**
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

Small, and each one is named in `docs/RULES.md` with its journal tag:

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

## WHAT THE USER RUNS, NOT the assistant

`sudo tools/efi.sh install`, `sudo tools/efi.sh bootarg add|remove <arg>`,
`sudo tools/efi.sh usbintr0 on` / `usbpoke off`, `sudo tools/load.sh`,
`sudo tools/unload.sh`, `sudo tools/fbload.sh`, `tools/plugin_check.sh`.
**the assistant never runs these and never asks for the password**, never installs
into the EFI or `/Library/Extensions`, and asks before any kext load.
**Never `kmutil unload` the framebuffer kext — it panics**; reboot instead.
Commit freely; **ask before pushing**.
