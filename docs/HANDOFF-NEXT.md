# HANDOFF — next step

**Written 2026-10-30, after 2xx resolved; updated the same day for 2xy and
2xz. `CLAUDE.md` has the standing rules,
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
| **2xy** the System Settings brightness slider | built, **NEEDS A BOOT** |
| **2xz** Ventura-and-up support matrix, cave table | built, no boot needed on Tahoe |

### Installed, and this is the part that bit

    plugin  build/out/RembrandtGPU.kext      43775C76   -> the OpenCore EFI
    fbkext  build/out/RBAMDFramebuffer.kext  EBF63EB0   -> /Library/Extensions

(boot 247's were FE3200B0 / 353A1D24. Both moved for 2xy/2xz; the plugin's
moves again on any rebuild, so take the UUID from the build, not from here.)

**Install BOTH, every time, and check both UUIDs in `kextstat` before reading
anything else.** The nine-boot crash loop and the whole boot-arg bisect of
2xx happened because the plugin was reinstalled nine times and the framebuffer
never was; the loaded `C7278266` predated 2xw and had neither the `rbgpu-gen`
rule nor the rule-38 check nor the `RBGPUStock` handshake. `RBFBArmWhy = 10`
is the cheapest proof the new framebuffer is the one running, because only
2xw+ can publish it.

    sudo tools/efi.sh install
    sudo cp -R build/out/RBAMDFramebuffer.kext /Library/Extensions/
    sudo cp /Library/KernelCollections/AuxiliaryKernelExtensions.kc \
            /Library/KernelCollections/AuxiliaryKernelExtensions.kc.bak
    sudo kmutil create -n aux -a x86_64 \
        -B /System/Library/KernelCollections/BootKernelExtensions.kc \
        -S /System/Library/KernelCollections/SystemKernelExtensions.kc \
        -A /Library/KernelCollections/AuxiliaryKernelExtensions.kc \
        -r /Library/Extensions

**`kmutil install --update-all` is the WRONG TOOL here and three boots went to
believing it was the right one.** `--update-all` means every collection, so it
tries to rebuild the boot kernel collection and the system kext collection as
well, for every kernel variant - and this machine has no KDK for 25G332 and no
`/System/Library/Kernels/kernel.{debug,development,kasan,research}`. It dies
there, *before* the auxiliary collection is ever built:

    failed to build release collection:
      Unable to resolve dependencies: 'com.apple.driver.SCSIDeviceSpecifics'
      names a dependency on 'com.apple.kpi.mach', which was not found.

That is an APPLE kext failing against an APPLE KPI, which is the tell that the
rebuild being attempted is not the one we want. `-n aux` builds only the
auxiliary collection and LINKS against the boot and system collections already
on disk, so no KDK and no kernel variant is involved.

Keep the backup. `AuxiliaryKernelExtensions.kc` is what the next boot loads,
and `sudo cp` the `.bak` back over it if a build produces something worse.

Two false leads recorded so that they are not re-run: `--force` and
`--allow-missing-kdk` do not help, and neither does parking
`HoRNDIS.kext` - it is missing from the collection, and it does declare
`com.apple.kpi.unsupported`, but it is NOT what fails the build and the error
never mentioned it. That was an inference from "which bundle is missing" made
while only the tail of the error was in hand, and it was wrong.

### VERIFY THE COLLECTION BEFORE REBOOTING - this is the whole lesson


    kmutil inspect -B /Library/KernelCollections/AuxiliaryKernelExtensions.kc \
        --show-kext-uuids | grep rembrandt

That prints the UUID **actually inside the collection**, which is the only
thing a boot will load. It must be the UUID `make fbkext` just printed. The
file's mtime is a weaker check and the command's exit code is no check at all
(rule 26, both directions).

Two things that do NOT work and were tried: `strings -a` on the `.kc` cannot
see a kext's strings at all - the CONTROL token `rbgpufbbacklight`, which is
certainly in there, also counts 0 (rule 26's own `strings -a` entry, now paid
for twice) - and `kmutil rebuild` is not a kmutil verb.

Also in that collection, for awareness: `com.nullmoth.NVRMAGDC` and
`org.franciumos.mhelper.driver` (the latter staged out of `/private/var/tmp`).
NVRMAGDC is AGDC-adjacent and has never been accounted for in any reading here.

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

## THE NEXT BOOT: BRIGHTNESS (2xs + 2xy)

Both halves of the backlight are built and **neither has ever been booted**.
They are separately armed because rule 38's question has different answers for
them: the write reaches a PWM duty cycle, the match reaches which `IODisplay`
subclass owns the connection - and through it the display-prefs key, mirroring
and the wrangler's idle dimming.

Install both kexts as above, then:

    sudo tools/efi.sh bootarg remove -rbgpunosentinel
    sudo tools/efi.sh bootarg add -rbgpufbbacklight -rbgpufbbuiltin
    nvram boot-args                  # VERIFY before rebooting: -rbgpuoff must
                                     # NOT be there, and the two new tokens must

Removing `-rbgpunosentinel` first is deliberate: these two args have never run,
so this is exactly the boot the sentinel exists for. If it does not survive 60
seconds the next boot comes up stock on its own and costs nothing.

**What to read, in this order.** Rule 8 first (`nvram rbgpu-boot` must have
increased), then rule 34 - the args this experiment depends on are
`-rbgpufbbacklight` and `-rbgpufbbuiltin`, so check they are in `boot-args`
before interpreting a single counter.

    ioreg -rl -c RBAMDFramebuffer | grep -E 'RBFBBl|RBFBArmWhy'
        RBFBBlArmed     1 = -rbgpufbbacklight was seen
        RBFBBlBuiltIn   1 = -rbgpufbbuiltin was seen
        RBFBBlReady     1 = BAR5 mapped and PWRSEQ0 identified
        RBFBBlRefuse    0 = set up; else WHY it will not write (2xs' codes)
        RBFBBlFlagsGets  how many times anything asked kConnectionFlags
        RBFBBlSenseGets  ...and getAppleSense
        RBFBBlLevel0 / RBFBBlLastLevel / RBFBBlLastCount / RBFBBlReadback
        RBFBBlSets / RBFBBlGets / RBFBBlParmAdds / RBFBBlPendTimeouts

    ioreg -rl -c IODisplayConnect | grep -E 'class |IODisplayConnectFlags'

**Said before the boot, so the reading cannot be bent to fit (rule 14):**

* The *only* thing that settles 2xy is the class name under `display0`. Today
  it is `AppleDisplay` with `IODisplayConnectFlags = <00000000>`. Success is
  `AppleBacklightDisplay` (or `IOBacklightDisplay`) and flags `<00080000>` -
  0x800 byte-swapped, since `ioreg` prints the raw data.
* `RBFBBlFlagsGets` **0** means nothing ever asked, so IOBacklightDisplay never
  probed this connection and the two answers are not the reason it did not
  match. That is a different failure from `RBFBBlFlagsGets` non-zero with
  `RBFBBlSenseGets` 0, which means it asked for the flags, did not like them,
  and stopped - and both are different again from both counters non-zero with
  `AppleDisplay` still matching, which means the probe passed and something
  else won the category. Three outcomes, three distinct readings.
* A brightness slider in System Settings is the user-visible form, but it is
  **downstream of the class name** - do not read its absence as 2xy failing
  until the class has been checked.
* `RBFBBlReady 0` with `RBFBBlRefuse` non-zero is 2xs failing, not 2xy. The two
  are independent by construction and must be attributed separately.
* The panel going **dim** is 2xs' documented worst case, not a hang: the floor
  is non-zero (`rbgpufbblfloor=`, refused outside 1 %..50 %) precisely so the
  screen can never be driven black. `sudo nvram rbgpu-fb=0` or removing
  `-rbgpufbbacklight` undoes it.

Outstanding paperwork once it has booted: the three new `docs/linux-ref/` files
in `UPSTREAM.md`, and a 2xs journal entry (2xy's is written).

### Open, and none of it blocking
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

1. **Brightness** — both halves built, neither booted. The whole procedure and
   what each outcome would mean is under "THE NEXT BOOT" above.
2. **The last ~22 % of the frame rate** — 82/s against the panel's 143.83. Not
   established whether that is demand-limited or still gated.
3. **The wedge**, the independent second defect. `metal_probe --queues 4`
   reproduces it with no kext, no arming and no reboot (2wt).
4. **HDMI is unreachable** — wired to the dGPU (2xr), not merely unbuilt.

## OS COVERAGE, AND WHAT IT DOES AND DOES NOT CLAIM (2xz)

`src/common/rb_osver.h` is the matrix and `tools/hosttest/osver_test.c` pins
it. Darwin 22 (Ventura) up is the declared range, and within it:

| Darwin | OS | measured here | in practice |
|---|---|---|---|
| 22 | Ventura | nothing | loads, comes up **stock**, every probe reports |
| 23 | Sonoma | nothing | same |
| 24 | Sequoia | offsets, cave row | the OS it was all derived on |
| 25 | Tahoe | offsets, cave row, USB | the machine, boot 247 |

An OS with nothing measured comes up stock on purpose: the object offsets into
Apple's driver were measured, not inferred, and writing a ring pointer through
an offset that moved is an unbounded write into another driver's object.
`-rbgpuosforce` arms the table anyway, for the boot that goes and measures -
and that boot should keep the sentinel on.

**To bring up a third OS**, in order and none of it needing a boot until the
last step: `make mtlcave` on that machine (derives its cave row off its own
cache), add the include and the row to `src/plugin/rbgpu_mtlcave.h`, add
`RB_OSD_MTLCAVE` to that Darwin's row in `rb_osver.h` with a note saying where
it came from, `make hosttest` (it cross-checks those two and fails if only one
was done), then one `-rbgpuosforce` boot with the sentinel armed, reading
`os_major` / `os_derived` / `os_forced` and every ring counter before trusting
any of them.

## THE TRAP, NOW HALF-CLOSED

**Every Apple address in a COMMENT in this repo and in the journal is a macOS
15 address.** `AMDRadeonX6000MTLDriver __TEXT` moved 0x7FFB08BF3000 →
0x7FFB110D4000, CoreDisplay 0x7FF80404C000 → 0x7FF805348000 and SkyLight
0x7FF807B42000 → 0x7FF8091F1000. **`tools/re/dscsym.sh` and `dscinv.sh` no
longer hardcode anything** - `tools/re/dscbase.sh <Lib>` reads the live unslid
`__TEXT` base and size out of `dyld_info -segments` and both take their bases
from it (2xz: dscsym.sh had been measuring the slide every run and subtracting
a stale base from it, so `atos` answered for an address 0x16AF000 off, every
time, silently). In live code the only hardcoded Apple address left is
`RBGPU_IOP_SITE_VM`, which is stale and fails safe. The cache is at
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
Commit freely; **pushing was authorised on 2026-10-30** ("also push whenever u
need") — before that it needed asking every time.
