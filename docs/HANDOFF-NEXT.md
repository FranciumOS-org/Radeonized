# HANDOFF — next step

**Rewritten 2026-10-31 at boot 268, after the 2z-series closed out the frame-rate
work. `docs/RULES.md` has the standing rules, the chip facts and the boot-arg
contract; `docs/JOURNAL.md` is the evidence record, grepped by milestone tag
(`2zc`, `2zg`); this file is only what is in flight.**

---

## THE MACHINE WORKS, ON NO BOOT-ARGS OF OURS BUT TWO.

An armed boot is an accelerated Mac: Metal compute bit-exact, `AMD Radeon
680M` / 4 GB in System Profiler, our `RBAMDFramebuffer` owning the panel,
Apple's `AMDRadeonX6000` compositing for it, no flicker, no tearing, USB on
real interrupts, the brightness slider moving the panel, and a Displays panel
offering eight refresh rates derived from the panel itself.

| | state |
|---|---|
| **2xw** armed by default, no boot-args | WORKING |
| **2xy/2y1/2y2** brightness slider | **DONE**, boots 249-253 |
| **2y5-2z1** refresh-rate ladder, mode switching | **DONE**, boots 255-259 |
| **2z8** efficiency | **CLOSED** — not a RembrandtGPU problem; ~35 W is the platform floor |
| **2zc** the retire tick is a two-sided cost | **MEASURED**, boot 265 |
| **2zd/2zf/2zg** retire from the vblank | **DONE and gated**, boots 267-268 |
| **the last ~50 % of the frame rate** | **OPEN, and it is the only thing open** |

### Power is a SEPARATE project now: `~/Developer/Econized`

The ~35 W idle moved out of this repo on 2026-10-31 (2z9, 2zh). It lives in
**`~/Developer/Econized`**, its own git repo (local only, no remote yet). Read
its `docs/HANDOFF.md` first; it is self-contained. Two things from 2zh qualify
"platform floor" in the table above: XNU already idles with `sti; hlt`, so
porting the SMCAMDProcessor fork's PM dispatch gains nothing; and the RTX
4050's PCIe link is UP (`GPP0` Gen4 x8, link active) under `-wegnoegpu`, so
the dGPU is hidden but not powered off. Econized's first instrument, the
`EconRAPL` package wattmeter, is built and waiting for its first load.
**Journal tag note:** 2zh was first committed as "2za" on a side branch while
another session used 2za for latch latency on main. 2zh is its real tag.

### Boot-args in the EFI right now

    rbgpumtlkscope=*  -rbgpufbbacklight  -rbgpufbbuiltin  -rbgpufbblparm
    -rbgpufbmodes     -rbgpuretirevbl
    (plus wdt=-1, which is Apple's and must stay)

`-rbgpufbmodes` and `-rbgpuretirevbl` are the two opt-ins worth keeping on.
Everything else of ours is default-on since 2xw.

### What boot 268 reads, as the baseline to compare against

    kextstat   plugin 2B35749B   fb D79228C7
    ioreg      1 RBAMDFramebuffer, 0 IONDRVFramebuffer
    RBFBModeCur        = 100 (native, 143898 mHz)
    RBFBVblRetArmed    = 1      Drove = Calls, Refused = 0
    RBFBVblRetModeSkips= 0 at native, climbing at 60 Hz   <- the mode gate
    peer: vblret_src=1 vblret_hand=1 vblret_take=0 tick_us=6944
          armed=1 gen=1 tick_armed=1 agdc_on=1

    sets/s at native   72.5  (n=12, 69.41-74.53)
    sets/s at 60 Hz    49.0  (n=6, timer-driven by the gate)
    mean vblanks       1.00 when the vblank drives, 1.04-1.63 when the timer does

**Two places to read counters, in two different bases.** The framebuffer's are
`ioreg -rl -c RBAMDFramebuffer` (`RBFB*`). The plugin's are the `RBGPUDiag`
dictionary in `ioreg -rl -c RembrandtGPUPeer`, which prints **decimal**.
`nvram rbgpu-diag` prints **hex with an `0x` prefix since 2zc** and is
**truncated near 520 bytes**, so most keys never reach it even though
`diag_used` 693 of `diag_max` 768 says the table is not full — absence from
`nvram` is not absence from the table (rule 14, rule 26).

### Installed, and this is the part that bit

    plugin  build/out/RembrandtGPU.kext      2B35749B   -> the OpenCore EFI
    fbkext  build/out/RBAMDFramebuffer.kext  D79228C7   -> /Library/Extensions

(Those are what boot 268 is running. BOTH move on any rebuild, so take the
UUID from `dwarfdump --uuid build/out/...` at the time you build, never from
this file — it is a record of one boot, not a constant.)

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
    sudo kmutil create -n aux -a x86_64 --allow-missing-kdk \
        -B /System/Library/KernelCollections/BootKernelExtensions.kc \
        -S /System/Library/KernelCollections/SystemKernelExtensions.kc \
        -A /Library/KernelCollections/AuxiliaryKernelExtensions.kc \
        -r /Library/Extensions

**This exact invocation was run to a scratch output path before being handed
over**, which is how it should always be settled: `-A` anywhere writable costs
nothing, needs no `sudo`, and the result can be inspected. It produced a
collection with `com.rembrandtgpu.fb` at the new UUID and every OTHER kext at
a byte-identical one, with only `org.franciumos.mhelper.driver` dropping out -
correct, since its bundle lived in `/private/var/tmp` and is gone. Do that
again rather than aiming a first attempt at the live file.

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
on disk, so no kernel variant is involved - `/System/Library/Kernels/` here
holds `kernel` and a dozen Apple-Silicon `kernel.release.t*` and nothing else.

**`--allow-missing-kdk` is still required on the `create` form**, and leaving
it off is its own silent failure: `kmutil create -n aux` without it prints only
`Missing Developer Kit ...` and writes nothing, while exiting 0. An exit code
is not a verdict (rule 26) - the output file's existence is.

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


## THE ONE THING LEFT: WHERE HALF OF EVERY FRAME PERIOD GOES

`fed` is **~50 %** at native — 72.5 sets/s against a 144 Hz panel — and the
latch ceiling now sits at the panel rate itself, so nothing currently measured
accounts for the other half.

**What has been eliminated, each by measurement, and must not be re-opened
without new evidence:**

| candidate | how it died |
|---|---|
| demand / the workload | 1, 2, 3 concurrent `animtest`: 72.43 / 68.53 / 76.15. Flat under triple demand (2zb) |
| a HUBP backlog | `Preempted` 7 of 19 669, `PendHits` **0** of 19 669 (2zb) |
| the `kIOReturnNotReady` hold | already conditional on the live PENDING bit; `AmtrHoldExpired` 0, the backstop is never reached (2xn) |
| flip latch latency | removed entirely by 2zd — `mean vblanks` 1.63 → 1.00 — **and throughput did not move** (2zg) |
| the retire tick's period | 6944 µs is a measured interior optimum; 2000 and 13888 both cost a third (2zc) |
| the retire tick's phase | fixed by driving from the vblank; buys variance, not mean (2zg) |

**The mean has sat at 70-75 sets/s through every configuration ever tried.**
Tick 2000, 6944, 13888; timer-driven and vblank-driven; one workload and
three. Something sets ~73 and it is none of the above.

**So the next step is an instrument, not another configuration sweep.** The
question that has never been asked directly: *where does the frame period
actually go?* The useful shape is a timestamp at each stage of one frame —
`setAttribute('amtr')` arriving, the HUBP write, the latch, the retire, the
next `getAttribute` — accumulated as a histogram per stage, published
per-stage. 2zb's latch sampler is the model: cheap, on a path that already
runs, decided in `src/common` so the hosttest checks it first.

### SIX MODELS OF THIS CEILING HAVE DIED. READ THIS BEFORE BUILDING A SEVENTH.

1. An absolute ~76 frames/s cap (2y7) — looked right, then wasn't, then partly was.
2. A latch-phase half-rate model (2z2).
3. A fixed ~14 ms latency (2z2).
4. A latency *distribution* that shifts with the frame period (2za/2zb) — the
   bin-2 share alternated across modes and would not reproduce.
5. `min(latch_ceiling, ~75)` as two hardware bounds (2zb) — the ~75 turned out
   to be a function of `tick_us`.
6. The latch as a *constraint* (2zd) — it was a correlate; deleting it moved
   nothing.

Every one died the same way: structure read out of too little data, then
engineered against. **Measure the quantity directly and let the instrument be
able to refute the model** — that is what 2za did right even though its model
was wrong.

## THE TWO SAMPLING RULES THIS COST US, BOTH LEARNED THE HARD WAY

**One sample cannot resolve a configuration change (2ze).** Ten runs at
143.85 Hz on one boot: 63.98-100.77, mean 73.1, median 70.0. The *first* run of
that boot read 100.77 and would have recorded a feature as a 38 % success on a
boot whose own counters said it never ran.

**And six samples inside one boot measure that boot, not that configuration
(2zg).** Boot 267 had six internally consistent samples (78.14-94.90, mean
90.4) and boot 268 gave 72.50 (n=12) in the same configuration with the gate
provably inert. **Boot-to-boot variance exceeds within-boot variance.** A
configuration claim needs **at least two boots per side**, or an A/B inside one
boot. This is rule 18 applied to boots rather than rows, and it is rule 44 in
`docs/RULES.md`.

`tools/paired.sh [secs]` is the instrument: it pairs the latch histogram with
the frame rate over one **measured** window and refuses any row whose `vbl/s`
disagrees with the mode's own rate by more than 5 %. That guard exists because
without it the script reported a **194 Hz panel**, having divided a real
counter delta by `animtest`'s `--secs` argument instead of the elapsed time.
`JOBS=N` runs N concurrent workloads. `./build/out/modeset <hz>` switches mode
from the shell with no reboot — but read `RBFBModeCur`, never the return code
(2z1).

## STILL OPEN, NOT BLOCKING, EACH NAMED IN docs/RULES.md

1. **The wedge**, an independent second defect: work submitted that never
   retires on GFX/SDMA/compute user queues. `metal_probe --queues 4`
   reproduces it in userspace with no kext and no reboot (2wt). Not the
   display blocker (2x0).
2. **Metal score ~11k.** The only live lead is re-deriving the 254-byte
   per-OS code cave in `rbgpu_mtlcave.h` to target `64KB_S` (mode 9) instead
   of LINEAR. **Both halves of the swizzle fix must agree or the display
   corrupts** — it is not a one-constant change. `linear_force` survives as a
   suspect here and only here; 2z8 demoted it as a power cause (3.78 flips/s
   on a static desktop has no bandwidth to pay for).
3. **HDMI is unreachable**, wired to the dGPU (2xr), not merely unbuilt.
4. **Ventura and Sonoma claim nothing** in `rb_osver.h`. See the OS coverage
   section below.
5. Housekeeping: `UPSTREAM.md` entries owed for 2xs' three `docs/linux-ref/`
   files; `com.nullmoth.NVRMAGDC` never accounted for.

## EFFICIENCY IS CLOSED, AND HANDED OFF (2z8/2z9)

Stock 34.82 W against armed 36.44/41.35 W — **our whole stack is ≤4 W, inside
the noise**, and ~35 W is the platform floor because a 7735HS has no CPU
power-management driver under macOS. At that floor a 45 W adapter has ~10 W of
headroom, which is the user's charging symptom exactly. CPU power work lives
in a **separate repo, `~/Developer/Econized`**, not here.

Two instrument lessons from that work, both expensive: `InstantAmperage ×
Voltage` is **not a power sensor** (filtered, goes stale, disagreed with itself
by 10×) — `tools/watts.py` measures energy by `AppleRawCurrentCapacity` drain
instead, with a `--selftest` and a plausibility bound, because a 1000× unit
error passed every ratio check. And `nvram boot-args` **does not survive a
reboot on this machine** (rule 43).


## IF IT GOES WRONG

In order of bluntness, and none of them needs another OS:

    sudo nvram rbgpu-arm=0                       # accelerator off, and the panel with it
    sudo tools/efi.sh bootarg add -rbgpustock    # every default off
    sudo tools/efi.sh bootarg add -rbgpuoff      # the plugin does nothing

and if the machine cannot be driven at all, **do nothing**: the boot sentinel
makes the next boot stock by itself. That is the whole point of 2xw. One bad
boot costs one stock boot.

To back a single feature out, derive the negative from its own name:
`-rbgpunoschedtick`, `-rbgpunofbhubp`, and for this series simply drop
`-rbgpuretirevbl`. **Never `kmutil unload` the framebuffer kext — it panics
(2sq); reboot instead.**


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

## WHAT THE USER RUNS, NOT the assistant

`sudo tools/efi.sh install`, `sudo tools/efi.sh bootarg add|remove <arg>`,
`sudo tools/efi.sh usbintr0 on` / `usbpoke off`, `sudo tools/load.sh`,
`sudo tools/unload.sh`, `sudo tools/fbload.sh`, `tools/plugin_check.sh`.
**the assistant never runs these and never asks for the password**, never installs
into the EFI or `/Library/Extensions`, and asks before any kext load.
**Never `kmutil unload` the framebuffer kext — it panics**; reboot instead.
Commit freely; **pushing was authorised on 2026-10-30** ("also push whenever u
need") — before that it needed asking every time.
