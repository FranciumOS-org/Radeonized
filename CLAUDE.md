# RembrandtGPU — notes for Claude

> **New session: read `docs/HANDOFF.md` FIRST.** It is the current *state* —
> what works, what is open, the frontier, the verified facts and the
> procedure. `docs/JOURNAL.md` is the chronological evidence journal
> (~17 000 lines): authoritative for what was measured, in order, including
> every withdrawn reading, and **not to be trimmed**. Grep it by milestone tag
> (`2ks`, `2lg`) for the history of one step; never read it whole.
> `docs/HANDOFF-FULL.md` is a stale dump from 2026-10-05 — ignore it.

Goal: graphics acceleration for the Radeon 680M (Rembrandt, `1002:1681` rev
0a, GC 10.3.3 / DCN 3.1.2 / PSP+SMU 13.0.x / SDMA 5.2.3) on macOS. Plan:
`docs/PLAN.md`. Machine: ASUS TUF A15 FA507NU (Ryzen 7 7735HS), macOS 15.8.1
(24H32), OpenCore. Boot-args include `-wegnoegpu`; csr-active-config **0xA43** (not
0xA03 as this line said until 2026-10-22; measured from NVRAM. SIP reports
Filesystem Protections *disabled*, yet the dyld cryptex path still refuses
writes with EROFS because its files carry the `restricted` flag, which the
sealed-OS policy enforces independently of the CSR bits).

## Status in three lines

**An armed boot is a usable, accelerated Mac** — the CP halt is fixed
(`-rbgpuvramadj`, 2ks/2ku/2kw), zero VM faults, Metal compute bit-exact.

**THE UNIVERSAL FIX IS DONE (2on, boot 130).** `-rbgpumtlkfix` with
`rbgpumtlkscope=*` fixes **every process on the system**, Safari included,
with no wrapper, no injected dylib and no change to any app bundle - so it
survives app updates. 2045 of ~2049 execs patched. The kext hooks
`_vm_shared_region_slide` for the cache slide and
`_task_set_main_thread_qos` for a per-process point at exec, then places a
224-byte cave in `__TEXT,__const` and a 5-byte detour over
`amdMtl_HWL_DetermineSwizzleMode` with `_vm_map_write_user`, verifying both
from the target's own memory. **Lilu's user patcher is irrelevant here - it
is disabled on Big Sur and newer and cannot be re-enabled (rule 22).**

**The defect was never only about text.** It is the swizzle mode the
*userspace* Metal driver picks for client textures; images were affected too
(2on). Everything from 2lg to 2ol called it "the text defect" because text is
where it was noticed. Policy `fix`: LINEAR for non-render-targets, render
targets mapped off the `_X` family (21->5, 22->6, 23->6, 25->9, 26->10,
27->10; Z modes 20/24 untouched), keyed on `usage` OR'd from descriptor
**+0x68 and +0xb0** (JBR leaves +0x68 unset on a render target).
**WHY the `_X` family is wrong is still NOT established** - 2nr blamed Navi
21's pipe count and 2ns measured that false: AddrLib's whole configuration is
`AMDCommonHwInfoRec+0xa4` = gbAddrConfig, which reads 0x242, agreeing with
the chip. 2na's XOR-linear bit matrix still points at the `_X` XOR term, just
not at the pipe count.

**`tools/rbgpu-app.sh`, `tools/mtllinear-app.sh` and the `(GPU)` wrappers are
GONE (2op)** - superseded by the kext path, which reaches what they never
could: platform binaries and sandboxed App Store apps. All 11 wrappers were
removed in 2on and both scripts are deleted. A leftover wrapper elsewhere is a
safe no-op (2om). 2ns's "there is NO global fix" was right about the place and
wrong about the reach, exactly as 2lg was.

**BOTH HALVES ARE NOW DEFAULT-ON AND BOTH ARE NEEDED (2oo).** The kernel-side
linear force is on unless `-rbgpunolinear`; the userspace patcher is on unless
`-rbgpumtlkoff`. Boot 131 removed the force and the desktop regressed - apps
correct at launch, wrong again after idle or a tab switch, Terminal broken
regardless - because native AppKit apps never reach the userspace chooser
(2oj). **No boot-arg or manual setup is required for a working desktop.**

**BOOT 132 CONFIRMED IT, FROM THE DEFAULTS (2op).** The user: *"everything is
flawless, not a single artifact or even an off pixel"*. Both halves ran from
their defaults, checked rather than assumed: `-rbgpumtlkfix` and
`-rbgpulinear` survive only inside log-message text and are **not read**
anywhere, so the args still sitting in boot-args enabled nothing. Only the
scope string came from an arg (`rbgpumtlkscope=*`, same value as the default).

Use `tools/mtlswizzle.c`, a **trampoline** hook, not `tools/mtllinear.c`'s
constant stub: the stub can only be blanket, and blanket LINEAR **crashes**
JetBrains, because JBR renders every Swing surface into a plain MTLTexture and
a linear surface is not a legal render target (Electron survives it only
because Skia's render targets are IOSurface-backed and never reach this
chooser). Policy `RBGPU_SWIZZLE=fix` is the one to use: LINEAR for
non-render-targets, and render targets mapped **off the `_X` family**
(22->6, 26->10, 27->10; Z modes untouched). `fix` keeps the surfaces **tiled**,
so it is a better answer than 2np's LINEAR, not merely a wider one.
**WHY it works is NOT established** - 2nr said the `_X` pipe-bank XOR is built
from Navi 21's pipe count, and 2ns measured that false: AddrLib's whole
configuration is `AMDCommonHwInfoRec+0xa4` = gbAddrConfig, and it reads 0x242,
agreeing with the chip exactly. 2na's exact XOR-linear bit matrix still points
at the `_X` XOR term, just not at the pipe count.

`usage` is at descriptor **+0x68 as requested and +0xb0 as resolved** - always
OR them. JBR leaves usage unset, so +0x68 reads 0 on a render target.

For a one-off test in a single process you can still inject
`tools/mtlswizzle.c` with `RBGPU_SWIZZLE=fix`; never a launchd environment
(rule 21). **Apple platform binaries strip `DYLD_INSERT_LIBRARIES` and cannot
be reached by injection at all** - which is why the kext path matters, and
2on proved Safari is both affected and fixed by it.

**Nor is TextEdit, or any native AppKit app** - 2oj/2ok: this defect is the
mode chosen for client `MTLTexture`s, which Electron and JBR hit by rendering
text into plain textures, while AppKit text goes through CALayer backing
stores that do not reach this chooser (2nj, Defect B). TextEdit renders
correctly at baseline, so it can only ever show "did not crash".
**2nj's second half - that they do not reach the kernel align manager
either - is now in doubt**: boot 131 broke Terminal with the force removed
and boot 132 fixed it with the force defaulted on, so something on that path
*is* reachable kernel-side. Unresolved, and recorded as the loose thread in
2op rather than smoothed over.

**texprobe is NOT an oracle for this defect** - it is clean at baseline (0/12,
every footprint 1.00x) because its round trip is self-consistent. Only a real
application tells you anything.

**Defect B IS CLOSED (2op)** - the CoreAnimation layer backing store that came
out rearranged in about half of allocations no longer reproduces.
`tools/screenprobe.sh` on boot 132: **32 of 32 captures CLEAN**, row and
column jumps 0, against **20/32 and 18/32 scrambled** on the identical
`refresh` x4 instrument before the fix - and every one of the 28 earlier runs
had scrambled captures, the three just before the fix being 100 % scrambled.
2on was right to warn against assuming the universal fix reached it; the
measurement answered the other way. The "wrong after idle or a tab switch"
symptom of boot 131 was this, so **no third defect need be hypothesised**.
Boot 132 also proved the detour a third independent way: screenprobe's own
in-process search finds the 32-byte prologue of
`amdMtl_HWL_DetermineSwizzleMode` **0x** in its own `__TEXT`, where 14
launches before the fix all read 1x. That verdict used to print
`INCONCLUSIVE` - it had only two branches and the kext's 5-byte detour leaves
*neither* pattern findable, so the success case read like a failure; fixed in
`tools/screenprobe.swift`.

**Note for any session run inside the Claude app:** it is itself launched with
`DYLD_INSERT_LIBRARIES` set, so every shell inherits it and `open -a` passes
it on. Launch anything with `env -u DYLD_INSERT_LIBRARIES` or it starts under
whatever patch this Claude is running - which once looked exactly like a
regression (2ns).

**THE DISPLAY HALF IS HALF-FIXED, AND THE REAL DEFECT IS NAMED (2pd/2pf,
boot 138).** The pacing is **fixed**: CoreVideo nominal went **30.000 ->
129.267 Hz** and the measured callback rate 29.99 -> **134.10 Hz**, a 4.3x
improvement the user felt before reading a number. The string is **fixed**:
System Profiler reads `AMD Radeon 680M` / `VRAM (Total): 4 GB` - *published by
us, not detected*. **But the stalls are NOT fixed and the cause is now
measured**: `sample WindowServer` shows `CompositorSW::CompositeLayersToDestination`
at **48 % of the main thread**, rasterising the whole layer tree on the CPU via
`CA::OGL::SW::` in **RGBA half-float**. Chain: no DCN 3.1.2 -> the display is
`IONDRVFramebuffer`, whose destination is a raw CPU-mapped range, **not an
IOSurface and not a Metal texture** -> SkyLight's only two destination
constructors take exactly those, so `CompositorMetal` (which IS activated, per
the log) has nothing to render the display into -> software compositing ->
~9 fps, multi-second blurs, 30 s wallpaper changes.
**PAGE FLIPPING IS NOT THE FIX, and this was MEASURED, not argued (2pf):**
with `-rbgpuflipprobe` armed and routed, `AMDRadeonX6000_AMDHWDisplay::getDisplayPipeTransactionFlip`
was **never called once** in a whole boot. The flip is downstream of a decision
already lost; a `HUBP0_PRIMARY_SURFACE_ADDRESS` write would have fixed nothing
and risked a black panel. **Do not propose it again without new evidence.** The
real lever is making the display's destination a GPU surface, i.e. an
accelerated framebuffer owning the display - which needs the DCN 3.1.2 code
macOS lacks, written from Linux's `dcn31` reference. Name it as a large body of
work, not one more hook. Still cheap and unspent: phase-lock the vblank by
polling `OTG0_FRAME_COUNT` (read-only; the timer's 7.736 ms vs the panel's
6.952 ms is ~0.78 ms of workloop overhead, which is why nominal reads 129 and
not 143.833).

**The display half has never started**, and 2oq surveyed why, statically.
Apple's controller personality matches Navi 21 PCI IDs and ours is
`0x16811002`; spoofing `device-id` kills the accelerator. But the personality
route is **pointless**: **macOS contains no DCN 3.1.2 code at all** - Apple's
DAL compiles `dcn10/20/21/30/302`, and `dcn31`/`dcn312`/`vba_31`/`yellow_carp`
are 0 hits in every AMD kext against a control of 58 for `dcn30`. This chip is
DCN 3.1.2 / DMU 3.1.3.

**A SUBCLASSED `IOFramebuffer` IS NOT POSSIBLE FROM THIS KEXT (2oz).** It was
built and it compiled: `IOFramebuffer.h` ships in Kernel.framework (no Xcode,
no KDK), only four pure virtuals, and a subclass resolved **356 of 356**
imports through `com.apple.iokit.IOGraphicsFamily` - Apple does the same in
`AMDFramebuffer`. Displacing `.Display_boot` would also have worked: same
provider (IOPCIDevice), same category (`IOFramebuffer`), its post-probe score
is **20000**, and xnu's `probeCandidates` was *read* - `probe()` returning NULL
does `continue`, and the start loop is `// start the best (until success) of
each category` - so **both** failure modes fall back. What kills it is the
**kernel collection**: IOGraphicsFamily is in SystemKernelExtensions.kc,
IOPCIFamily in BootKernelExtensions.kc, and OpenCore injects us into the
**boot** KC. `kextstat` proves it on this machine - **WhateverGreen depends on
Lilu and IOPCIFamily, NOT on IOGraphicsFamily.** Shipping that dependency
would have failed the plugin's load and taken the accelerator with it.
`/Library/Extensions` is no escape (Auxiliary KC, loads too late, and
forbidden). **Own the methods instead**: every framebuffer method is defined in
IONDRVSupport and therefore routable - mode list, timings, aperture, cursor,
gamma, attributes, the interrupt trio - which is the same technique that fixed
the accelerator, and functionally the same thing.

**The desktop runs on `IONDRVFramebuffer` with `IOFB0Hz = Yes` and one fixed
mode.** No vblank is why animations are timer-paced; one mode is why nothing
switches; the 7 MB is the same fact. **But the firmware already brought DCN
3.1.2 up** - `OTG0_CONTROL = 0x00011201` (master enable), OTG1-3 off, DMCUB
firmware 4.0.15.47 running - so the route is to **adopt the running pipe**,
not to bring DCN up: read OTG0's timing, publish our own `IOFramebuffer`
(IOGraphicsFamily is a public KPI), then take OTG0's vblank and flip
`HUBP0_PRIMARY_SURFACE_ADDRESS`. The last two **write DCN registers, which is
the user's decision** - the probe allows no writes but `MM_INDEX`, and a wrong
OTG/HUBP write blanks the panel (rule 12). Only the vblank step fixes
animations. `docs/HANDOFF.md` §4 and §10 have the detail.

Details, evidence and next steps: `docs/HANDOFF.md`.

## Layout

- `src/common/` — freestanding C shared by the kext and host tools: the IP
  discovery parser (`rb_discovery.c`), the read-only probe (`rb_probe.c`), the
  register list (`rb_regs.h`, every offset cites its Linux header), the SMU
  queries (`rb_smu.c`, `rb_smu13.c`). Message groups (`RB_SMU13_ALLOW_*`)
  start at queries only; allowing any other group on hardware is the user's
  decision. Tested in `tools/hosttest/smu13_test.c` against a simulated SMU
  whose tables are Linux's own structs.
- `src/kext/` — `RembrandtProbe`, an IOService in its own IOMatchCategory
  beside the boot framebuffer driver. Runs the probe once in `start()`,
  publishes `RBProbe` properties, unmaps everything.
- `src/plugin/` — the Lilu plugin (`kern_start.cpp` holds the hooks and
  probes, `RembrandtGPUPeer.cpp` the IOService peer). Boots from the EFI.
- `tools/hosttest/` — the probe against a simulated chip + parser fuzzing,
  ASan/UBSan. `make kext` runs it first; never load a kext whose hosttest
  fails. Add a case whenever `src/common` changes, and check a new test fails
  when the code it guards is broken.
- `docs/linux-ref/` — unmodified Linux files the code follows
  (`UPSTREAM.md` records each commit; fetch from torvalds/linux).
- `tools/re/` — static analysis of Apple's AMD kexts: `kc_extract.py` (kext
  out of a kernel collection as a standalone Mach-O), `machoread.py` (chained
  pointers, strings, symbols), `dumptable.py`, `kc_stubs.py` (kernel calls
  through branch stubs → names; `annotate` for objdump output), `vtslots.py`
  (IOService-family vtable offsets → methods), `strxref.py` (functions using a
  C string), `switches.py` (jump tables: case → handler), `ptrsym.py` (names
  every `RBGPULog` pointer inside a loaded AMD kext; run by
  `plugin_check.sh`). Output to `build/re/`. Findings:
  `docs/APPLE-STACK.md`.

## Workflow

- Build: `make kext tools` (CommandLineTools only, no Xcode); `make plugin`
  for the Lilu plugin — **`make kext tools` does NOT rebuild it**. The kext
  step resolves every import with `kmutil libraries`.
- **Always check the UUID moved** after a build
  (`dwarfdump --uuid build/out/...`). Two boots were lost to a stale build.
- The probe's hardware rules: no writes except `MM_INDEX`/`MM_INDEX_HI`
  (enforced in `rb_probe.c`, asserted by the hosttest), never enable PCI
  decoding or bus mastering, never map BAR0's first 16 MB (the boot
  framebuffer's), read only inside the carve-out. Levels 2 (DCN) and 3 (+ GC)
  are opt-in: `RBProbeLevel` in `src/kext/Info.plist`, or `rbprobe_level=N`.
- The SMU queries' hardware rules (`rb_smu.c`; opt-in `make kext SMU=1`,
  plist `RBSMUQuery`, boot-arg `rbprobe_smu=1`): writes only to MP1
  C2PMSG_66/82/90 of an MP1 13.x, only allowlisted query messages
  (GetSmuVersion, GetDriverIfVersion, GetEnabledSmuFeatures), nothing sent
  unless the response register reads 1, stop at the first non-OK answer.
  Linux's protocol (`smu_msg_v1_send_msg`).
- Register gates follow Linux's **version grouping**, not the exact version:
  this chip reports MMHUB 2.4.0 (Linux: `mmhub_v2_3` offsets) and DMU 3.1.3
  (Linux: `DCN_VERSION_3_1`, `dcn_3_1_2` offsets). Check the grouping before
  adding a gate.
- Results land in `logs/<timestamp>/`.
  `tests/data/rembrandt-680m-tufa15.bin` is this machine's real IP discovery
  table (2026-10-03); the hosttest parses it and runs the full probe on it.

## Hard rules

- **Always ask the user before loading or unloading a kext.** A bad load can
  hang or panic the machine and ends the session.
- The user runs `sudo tools/load.sh` (2oq: takes an optional probe level,
  `sudo tools/load.sh 2` for the display engine - applied to the staged copy
  only, so the committed default stays 1), `sudo tools/unload.sh`,
  `sudo tools/efi.sh …` and `tools/plugin_check.sh`.
  **Claude never runs these and never asks for the password.**
- **Never install into the EFI or `/Library/Extensions`.**
- Before any load, **commit** so a hang loses nothing. The user allows Claude
  to commit at any time without asking (2026-10-04); **pushing still needs
  asking**.
- **Deliberate diagnostic panics are off the table** (user's decision,
  2026-10-12).
- **Adding an SMU message group is the user's decision** — say what it changes
  on the chip.
- **NootedRed and NootRX are under the ChefKiss Open Knowledge License v1,
  whose §2 and §3 forbid AI reading, summarizing, analyzing or deriving output
  from the work. Do not read or analyze them.** Findings from those projects
  must come from the user in their own words.
- `efi.sh` saves `config.plist` to `logs/efi-backups/` first; it edits via
  `tools/efi_config.py` (tested by `tools/efi_config_test.py`).
  `efi.sh sysreport-on` + reboot + `efi.sh vbios` pulls the 680M VBIOS out of
  the ACPI VFCT table (`tools/vfct.py`) into `firmware/` (gitignored).

## Rules that cost boots to learn

Each was paid for on hardware. `docs/JOURNAL.md` has the evidence under the
tag in brackets; `docs/HANDOFF.md` §7 states them with their reasoning.

**Routing Apple's code**

1. Never route a name unless `nm -m build/re/<kext>` shows it **defined**
   there. `solveSymbol` does not skip imports — it returns the bare slide and
   `routeMultiple` writes a jump there. [2c]
2. Disassemble a symbol's first 16 bytes and **refuse if a rel8/rel32 branch
   begins within the first 5**. Lilu copies displaced instructions
   byte-for-byte into a trampoline, so a short branch keeps its original
   displacement and lands in Lilu's own buffer. [2gb]
3. **Grep the mangled symbol across the whole plugin before adding a route.**
   A second `RouteRequest` for an already-routed function **panics the machine
   outright**. A new transform goes **inside the existing hook**. [2kn]
4. **`RouteRequest::from` is NOT written back** by Lilu 1.7.3, although
   `kern_patcher.hpp` documents it as such. Call `solveSymbol` yourself. A doc
   comment is not a measurement. [2kp]
5. **Cross-check every hand-written mangled name against `nm`.** Three length
   prefixes were wrong in one change; each would have made `solveSymbol`
   return 0 and silently unscope the fix. [2kr]
6. **When a fix moves upstream, REMOVE the downstream patch it replaces, in
   the same change.** "Redundant" and "inert" are different things — 2ku and
   2ki composed and the halt came back. [2kv]
7. vtable dumps: **call offset = dump offset − 0x10**. A *non-virtual* call
   through a vtable **symbol** uses dump offsets directly. [2gp]

**Reading a boot**

8. **Read `rbgpu-boot` FIRST.** If it has not increased since the last
   capture, the boot was never armed and *nothing* in NVRAM is this boot's.
   `rbgpu-diag` carries no nonce, so "armed=1 reads live" certifies a **stale**
   capture. [2iz]
9. **`rbgpu-arm` is one-shot** — the peer consumes and deletes it. Every armed
   boot needs it set again; an install that only replaces the kext and edits
   boot-args does **not** arm. [2iz]
10. **Run `sudo tools/plugin_check.sh` in the armed boot, over SSH, before
    rebooting.** It saves the capture to `logs/<timestamp>-plugin/` on disk,
    which survives a power cycle; live NVRAM afterwards does not. Three
    experiments were spent blaming the code for this. **Look in
    `logs/<timestamp>-plugin/` before reading live NVRAM.** [2jg, 2gq, 2gw]
11. **A note is evidence only when its `b=` nonce is new.** [2gd]
12. **A stale note is no evidence about where a boot got to** — not weak
    evidence of an early stop. A user rebooting a black screen looks exactly
    like a hang. [2gw]
13. **A return code is not a result until the code that sets it has been
    read.** A named constant tells you what the NAME means, not what the
    function meant by it. Request 0x1B's *success* path returns
    `kIOReturnUnsupported`. [2ha]
14. **Absence is not a result.** A silent `return` inside an instrument, a
    full diag table and a missing NVRAM write all look identical to "the code
    never got there". Every path out of a probe must say something.
    [2eh, 2ek, 2fb]
15. Before interpreting anything, check the loaded UUID against `build/out`,
    whether `rbgpu-arm` was left set, and which boot-args are actually in
    `config.plist`. [2cu, 2cv]

**Instrumenting**

16. **The instrument is a suspect.** It has cost a wedge (2fo), a crawl
    mistaken for a hang (2ez) and two panics (2fz/2ga, 2gg). When a
    measurement and a suspect are both new, **remove the suspect** rather than
    reasoning about it.
17. **Never substitute a value and then narrate it as a hardware result.**
    [2cy, which cost fifteen milestones]
18. **Do not build a table from n=1 rows.** [2ev, 2fs]
19. **A NULL check is not a pointer check.** Validate derived pointers as
    kernel VAs (`>= 0xffffff8000000000`) before dereferencing. [2gh]
20. **A registration that reports success is not evidence that a patch
    applied.** A Lilu `BinaryModPatch` with `section == 0`
    (`SectionDisabled`) is skipped and its file never read, while
    `onProcLoad` still returns `NoError` - so the hook logged `ARMED` and
    patched nothing. Log the enabling field itself and refuse when it is
    disabled. [2lq]
21. **Never put `DYLD_INSERT_LIBRARIES` in a launchd environment, and test a
    recovery procedure in the situation it exists for.** `launchctl setenv` is
    system-wide, not per-app, and dyld **kills** any process that cannot load
    an inserted dylib - including every sandboxed app, which cannot read one
    from a home directory. Every newly launched app died; the machine needed a
    reboot. The documented undo, `launchctl unsetenv` over SSH, addresses a
    different domain than the Aqua session and did nothing - the working form
    is `sudo launchctl asuser $(id -u) launchctl unsetenv DYLD_INSERT_LIBRARIES`.
    Inject per process, where the blast radius is one process started on
    purpose. [2mq]
22. **Lilu's user patcher DOES NOT RUN on this OS at all.** Lilu's
    `Configuration::getBootArguments` sets
    `isUserDisabled = ... || getKernelVersion() >= KernelVersion::BigSur`,
    and that term is unconditionally true on Sequoia (24 >= 20). It is a
    `||`, so **no boot-arg re-enables it**. `UserPatcher::init` then returns
    `true` without registering its kauth listener and `registerPatches`
    returns `true` without reading a file, parsing the map or hooking
    anything - every layer reports success and nothing exists. That, and
    nothing else, is why 2lm, 2lp, 2nb, 2nm, 2oa and 2od were all inert
    while `onProcLoad` returned `NoError`. **Do not write another Lilu
    `BinaryModInfo` for this machine.** All of that code was DELETED in 2op
    (~590 lines), so there is no longer a working example in the tree to copy
    by accident. This rule has now been wrong twice
    and under-specified once: the earlier readings (a mod can only patch an
    existing file; addresses come from the `.map` but bytes from a file) are
    accurate descriptions of code that never executes here. The reusable
    lesson: **check that a subsystem is ENABLED on this OS before reading
    how it behaves** - one `grep isUserDisabled` would have saved three
    milestones and a panicked boot. What survives: **verify a patch from the
    target process's own memory** (`tools/cavecheck`), and note that a
    `MatchAny "/"` proc callback firing ZERO times is the cheapest proof the
    listener does not exist. The *mechanism* Lilu would have used is still
    open to us directly - see 2of. [2nn, 2nz, 2oc, 2of]

23. **`pgrep` cannot see the process a script is running inside.** BSD
    `pgrep` never matches the invoking process or **its ancestors**, so a
    script run from Claude Code cannot see the Claude app: `pgrep -x Claude`
    returns nothing while `ps` lists it. This is what made 2no's first launch
    of Claude "exit cleanly with no crash report" - the launcher's
    `running()` said no, the quit-and-wait was skipped, and the second copy
    forwarded into Electron's single-instance lock and died. The lock was the
    symptom; the blind check was the cause. Use
    `ps -axo pid=,command=` with a prefix match. [2nu]
24. **The dyld shared cache is inside a GRAFTED, Apple-signed cryptex.**
    `<Preboot>/<UUID>/cryptex1/current/os.dmg` is grafted into
    `Cryptexes/OS` at boot, which is why the tree reports Preboot's own
    `st_dev`, has no `mount` entry, is read-only to root (EROFS on `cp` AND
    `chflags`, which no CSR bit lifts) and is **absent when the volume is
    mounted raw in Recovery**. The image is raw UDRW, but the volume inside
    is sealed, its root hash is an IM4P and that hash is covered by an
    Apple-signed IM4M ticket - so it cannot be modified. Do not spend
    Recovery trips looking for the cache as a file. This rule is about the
    cache ON DISK and is unrelated to rule 22, which is about how Lilu finds
    patch bytes - editing the cryptex was never needed (2oc). [2ny]
25. **Read the machine, not the manual.** Four Recovery trips were each ended
    by something asserted from general macOS knowledge instead of measured
    here: `Cmd-R` (this is OpenCore, `PollAppleHotKeys` False - it is SPACE at
    the picker), the Data volume's name (`MACOS - Data`), Recovery booting
    from its own APFS container, and a booted-system path existing on a raw
    volume. The OpenCore config that answered the first was already in
    `logs/efi-backups/`. [2ny]
26. **A search is an instrument too.** A grep that returns nothing is not
    evidence of absence until it has been checked against a case known to
    be present. `objdump` separates mnemonic and operands with a **tab**, so
    a space-based pattern silently matched nothing and produced a confident
    wrong negative. [2lw]

27. **No vnode work at plugin init - the root filesystem is not mounted
    yet.** `RGPU::init()` runs at kext load during early kernel bootstrap.
    `FileIO::readFileToBuffer` goes through `vnode_open`, so a file probe
    there panics the boot **before the Apple progress bar appears**, with no
    `.panic` report written because the log machinery is not up either. Lilu
    makes the same call and deliberately defers it to `registerPatches`, at
    user-patcher init. Read a file from a callback that runs at exec time
    instead. The probe that did this existed only to report a precondition
    (rule 14) and so was rule 16 exactly: the instrument is a suspect. Note
    also that `-rbgpuoff` disables the plugin through
    `PluginConfiguration::bootargOff`, so `init()` never runs - a boot with
    it set is **not** a control for anything on the init path. [2oe]

28. **Attribute every log line to a boot by its timestamp AND its pid before
    using it.** `/usr/bin/log show --last boot` means "the boot I am in now",
    so it silently changes meaning the moment the machine reboots mid-session -
    rule 8 for the unified log, in a tool rule 8 does not mention. It made an
    `isAcc=NO` from an unaccelerated boot look like the answer, when the
    accelerated boot logs the same line twice and the second says **`isAcc=YES`**;
    and it made `GPU: FB: 0 of 1 opened` look like a failure in the boot under
    investigation when those lines were `WindowServer[161]` and
    `WindowServer[358]` from other boots. `--start/--end` is **not** a reliable
    filter either: a 22:30-22:50 window returned entries timestamped 21:28 and
    01:28. The per-line timestamp and pid are trustworthy; the range predicate
    is not. And **`log` is a zsh builtin** that shadows `/usr/bin/log` and
    answers `too many arguments`, which is indistinguishable from an empty log -
    always the absolute path, always with a known-present control. [2q0]

**The recurring error, stated once**

Thirty recorded instances of reading a value without first establishing
what makes it meaningful. The practice that keeps catching it: **say in the
code what would have to be true for the reading to mean anything, and log it
when it is not.** The 2kx/2la/2lc trio adds a variant worth naming — **read
the evidence already in hand before designing the next experiment**: all three
tested a global layout parameter that the user's photograph had already
excluded.

## Chip facts established on hardware

- **GC seg0 = 0x1260** (agreed four independent ways), seg1 = 0xa000. Apple's
  encoded register ids are `(dword offset << 32) | 11`, 11 = GC_HWID. Offsets
  from `docs/linux-ref/gc_10_3_0_offset.h`; byte address is
  `(seg + dword) * 4`.
- The register accessor's slots on `pm4+0x30` / `hw->0x368`: `[0x148]`
  resolves an id to a dword offset, `[0x118]` **reads** it, `[0x120]`
  **writes** it. [2fd, 2fg]
- Carve-out MC `0xF4_0000_0000`, system-physical `0x7_8000_0000`
  (`FB_OFFSET` 0x780), BAR0 `0x8_8000_0000`. Apple's GPUVM space is
  `0xFF_BF..`. **`mc2pa(mc) = mc - vram_start + vram_base_offset`** — the one
  missing term that was the whole CP halt.
- **One** SDMA instance; Apple's Navi 21 path builds four engines
  (`hw->0xc2 * 2 + 2`). [2gc, 2gi]
- The hw engine array is `hw + 0x3b0 + engine*8`: `[0]` PM4/GFX, `[1..4]` the
  four SDMA engines. The SDMA *walk* bases at `hw + 0x3b8`, so `engine[i]` is
  hw index `i + 1`. [2gu, 2gv]
- `GB_ADDR_CONFIG` is dword `0x13de`, BASE_IDX 0; this chip reads **0x242**
  (4 pipes, 4 packers, 1 SE). Fields per `gc_10_3_0_sh_mask.h`.
- `AddrSwizzleMode`: 0 = LINEAR, 9 = 64KB_S, 22 = 4KB_D_X, 25 = 64KB_S_X,
  27 = 64KB_R_X.
- **`getPDEValue` returns flags only** — the address is ORed in by its caller.
  Never hook it to correct an address. [2ko]

## Boot-args

The authoritative list, with the reason for each, is **`docs/HANDOFF.md` §9**.
The two that bite hardest:

- **Never arm `-rbgpuforceenable` without `wdt=-1`.** Without it the frozen
  boot panics at 120 s with no usable SSH window. [2hh]
- **Must stay OUT:** `-rbgpufbmatch` (spoofs `device-id`, **kills the
  accelerator** — this plugin works by making Apple *accept* `0x1681`),
  `-rbgpupdefix` (composes with the upstream fix, halt returns),
  `-rbgpunodisppanic`, `-rbgpusdmahook`, `-rbgpustampraw`, `-rbgpuregdump`,
  `debug=0x100`.
- **The linear force is ON BY DEFAULT (2oo) - `-rbgpulinear` is gone.** The
  arg is no longer read anywhere in the plugin; the behaviour it used to gate
  is now the default, and **`-rbgpunolinear`** turns it off. **You need it as
  well as the userspace fix**: boot 131 removed it and apps came up correct,
  then went wrong again after idle or a tab switch, with Terminal and other
  native apps broken regardless. The two cover different paths - native AppKit
  apps never reach the userspace chooser at all (2oj). This also retires 2mm's
  suspicion of the arg and explains it: the force made the KERNEL compute
  linear while USERSPACE still tiled, and the universal fix removes that
  disagreement for non-render-targets. Why it does not also disagree for the
  render targets userspace keeps tiled is **not** established.
- **`-rbgpumtllinear`, `-rbgpuhwblt`, `-rbgpumtlcave` and `-rbgpumtlfix` NO
  LONGER EXIST (2op).** All four were Lilu `BinaryModInfo` paths that could
  never apply (rule 22), and 2op deleted the code - ~590 lines - under rule 6:
  when a fix moves upstream, remove the downstream patch it replaces. None of
  the four strings is read anywhere in the plugin now, so leaving them in
  boot-args is harmless and does nothing. Take them out when convenient:
  `sudo tools/efi.sh bootarg remove -rbgpuhwblt` and the same for
  `-rbgpumtlcave`. Their history is in `docs/JOURNAL.md` under 2lm/2lp/2oa/2oc/2od.

- **`-rbgpumtlkpatch` / `-rbgpumtlkfix`** (2og/2oi) are the kernel-side
  patcher, which **WORKS**: it hooks `_vm_shared_region_slide` and
  `_task_set_main_thread_qos` and places bytes with `_vm_map_write_user`,
  because Lilu's user patcher cannot run here at all (rule 22). 2oi placed
  and verified the cave in a live process. **It is ON BY DEFAULT since 2oo**:
  stage B, scope `*`, no boot-arg needed. **`-rbgpumtlkoff`** disables it,
  `-rbgpumtlkpatch` selects the inert cave-only stage, and
  `rbgpumtlkscope=a,b,c` still overrides the scope. Defaulting it on is safe
  because it **fails safe**: every site is read back and compared before
  anything is written, so an OS update that moves the driver means nothing is
  patched rather than something wrong being patched. Boot 131 showed
  **WindowServer is safe to patch** - no exclusion is needed.
  `rbgpumtlkexclude=a,b` is checked FIRST and always wins: the escape hatch
  for an app that misbehaves, settable over SSH without a rebuild. Read
  `mtlk_patched` / `mtlk_failed`; detailed keys are throttled to the first 8
  patches because this runs on ~2000 execs per boot. **A leftover
  `(GPU)` wrapper is a safe no-op** - `mtlswizzle` needs exactly one match of
  a 32-byte pattern, the detour changes the first five bytes and the cave's
  trampoline holds only 30, so it finds 0 and refuses; do not shorten
  `kFind` below 31 bytes. Three things learned earlier:
  **`proc_selfname` is the parent at this hook point** - the exec'ing process
  is only reachable as `proc_name(proc_pid(get_bsdtask_info(task)))`;
  **plain `VM_PROT_READ|VM_PROT_WRITE` succeeds** on a cache page, so Lilu's
  `CS_ENFORCEMENT` surgery is never needed; and **the write is per-process
  copy-on-write**, so the cave must be verified in *this* process before the
  detour is written. The qos hook runs on **every** exec, so a fault in it
  makes the machine unbootable - rescue with `-rbgpuoff`. Read `mtlk_*` from
  the IORegistry; `mtlk_verify=1` is the only line that proves anything.

- **`-rbgpuvbl` / `-rbgpuvblpump`** (2ou/2ov) are the 30 Hz fix, both OFF by
  default. `tools/dispclk` measured the animation clock at **exactly 30.00 Hz**
  while the panel scans at 143.833 Hz - CoreVideo's fallback for a display with
  no refresh clock, which the boot framebuffer is. **`-rbgpuvbl`** fills
  `IODisplayModeInformation.refreshRate`, and boot 134 proved it takes
  (`CGDisplayModeGetRefreshRate` 0 -> 144.000) **but does not change the
  pacing** - nominal stayed 30, so that field is CoreGraphics' mode listing and
  not CoreVideo's clock. **`-rbgpuvblpump`** is the actual fix: CoreVideo paces
  off `StdFBShmem_t.vblDelta`, only `IOFramebuffer::handleVBL` writes it, and
  it only runs when a `'vbl '` `_VSLService` fires - which never exists here,
  because only an NDRV creates one. The pump calls `handleVBL(fb, nullptr)` on
  a timer, which `VSLDoInterruptService`'s own code shows is equivalent. The
  framebuffer pointer comes free from the `-rbgpuvbl` hook, so **both are
  needed**. `rbgpuvblhz=<millihertz>` sets the rate (refused outside
  20000-400000). **No DCN register is written** and it is not phase-locked, so
  it fixes pacing, not tearing. Read it with `tools/dispclk`: nominal 30.00
  means it did not take. The pump calls into IOFramebuffer 144x/s from a
  workloop - if that path needs a lock WindowServer holds the desktop can
  wedge, so rescue with `-rbgpuoff`.
- **`-rbgpufbvbl`** (2oz) is the vblank taken through the **documented API**
  and supersedes `-rbgpuvblpump` as the primary path; it implies the pump's
  timer, which is its delivery mechanism. `IOFramebuffer.h` says the class
  "will call its subclasses to set callbacks to be called on interrupt
  events", so IOFramebuffer **hands the driver the proc**; we route
  `IONDRVFramebuffer::registerForInterruptType`, keep the proc on `'vbl '`,
  return success and call it ourselves. Its disassembly closes the 30 Hz
  chain and confirms the `_VSLService` layout a third way (head `fb+0x268`,
  type `+0x08`, next `+0x28`): `"Ignore VBL"` is absent, so the path taken is
  the **empty list** - no NDRV ever called `VSLNewInterruptService` - and it
  tail-calls super, installing nothing. `unregisterInterrupt` and
  `setInterruptState` are routed too, so a sleep that disables the vblank is
  obeyed; handing back our own ref is safe by measurement - both only
  **compare** it against node addresses and never dereference it. Fixes
  **pacing**, not tearing: phase lock needs `OTG0_FRAME_COUNT` polling, which
  is read-only and is the next step. Writes **no DCN register**. Read it with
  `tools/dispclk`; rule 14 keys: `fb_vbl_calls` + `fb_vbl_t1..t8` (was the
  door knocked on, with what types), `fb_vbl_bound`/`fb_vbl_origret` (we took
  it), `fb_vbl_sent` (proc deliveries) vs `vblp_ticks` (fallback deliveries).
  **BOOT 138 RESULT: it works, but via the FALLBACK.** `fb_vbl_bound = 1` so
  the proc was captured, then macOS called `setInterruptState(ourRef, 0)` -
  `fb_vbl_state = 0` - and `fb_vbl_sent` is absent: **zero** deliveries through
  the proc. All 23553 frames came from `vblp_ticks`, 2ov's direct `handleVBL`
  call. So **2oy's ordering fix is what fixed the pacing** (`vblp_idle_nohandler
  = 682` measures the race it was built for), and keeping the fallback instead
  of deleting it is the only reason the boot delivered anything. Rule 6 does
  NOT apply yet - do not remove the pump. Also: `fb_vbl_origret` is
  **`0xE00002BE` = kIOReturnNoResources**, not the `0xE00002C7` 2oz predicted -
  that constant sits on the `"Ignore VBL"` path, which is not the path taken.
  And `registerForInterruptType` is called **54** times with types like
  `0xff0000e9` / `0x60000400` that are not fourccs, so `'vbl '` arrives after
  the 8-call `fb_vbl_t1..t8` window.
- **`-rbgpufbvram`** (2pa) publishes the label the "7 MB" is missing:
  `VRAM,totalMB` (default 4096, override `rbgpufbvrammb=<N>`, refused outside
  256-16384) and `VRAM,totalsize` on **both** the framebuffer and its provider,
  plus `model` = "AMD Radeon 680M" on the provider for the absent "Chipset
  Model" line. Routed on `IONDRVFramebuffer::enableController`, NOT done from
  `RembrandtGPUPeer::start()`, which runs before IONDRVSupport loads and is the
  boot-137 race (2oy). The cast to `IOService` is checked by asking the object
  `getMetaClass()->getClassName()` and refusing unless it is
  `IONDRVFramebuffer`. Allocates nothing - the 4096 MB is already there. Read
  it with `system_profiler SPDisplaysDataType`; "7 MB" means it did not take.
  Keys: `fbvram` (3 = published, 1 = bad pointer, 2 = wrong class, with
  `fbvram_class`), `fbvram_props`, `fbvram_routed`. **BOOT 138: CONFIRMED** -
  `fbvram 3`, `fbvram_props 5`, `fbvram_mb 4096`, and System Profiler reads
  `Chipset Model: AMD Radeon 680M` with `VRAM (Total): 4 GB`. Note for honesty
  when reporting it: the name is a **string literal we write**, and the 4096 MB
  is measured but also published by us - macOS detects neither.
- **`-rbgpuflipprobe`** (2pe) is observe-only and **has already returned its
  answer** (2pf): routed successfully, `getDisplayPipeTransactionFlip` never
  called once. Keep it only if re-testing that after a change; it writes
  nothing.
- **`-rbgpufbvramrange`** (2pq) is the flip window, **off by default**. 2pp
  measured that Apple's accelerator is **already bound to our
  IONDRVFramebuffer** (`IOAccelIndex`/`IOAccelRevision`/`IOAccelTypes` are
  published on `.Display_boot`), advertises `DisplayPipeSupported=Yes` and
  `TransactionsSupported=Yes`, and that **WindowServer holds an
  `IOAccelDisplayPipeUserClient2`** - because
  `IOAccelDisplayMachine::found_framebuffer` filters NOTHING. What it lacks is
  room: `IOGraphicsAccelerator2::find_vram_descriptor` takes its whole notion
  of scanout VRAM from slot 0x9b0 = `IOFramebuffer::getVRAMRange`, which
  returns **8294400 bytes - one buffer, room for zero others**, and a flip
  needs a second. This routes `IONDRVFramebuffer::getVRAMRange` and reports a
  bigger range **to one return address only** - the call inside
  `find_vram_descriptor` at `+0x4a`, verified by reading its six bytes
  (`ff 91 b0 09 00 00`) back from the running kernel first. All 14 other call
  sites keep the original, so **`IOFBMemorySize` staying 8294400 is the
  control**. The range is built at the original's own physical base and only
  longer, so offset 0 stays the live scanout and failure still shows a picture.
  `rbgpufbvramrangemb=<N>` clamped 8-256 (BAR0 is 256 MB, measured); default
  64. Writes **NO DCN register**: a flip touches only OTG (0/434 moved) and
  HUBP (0/80), both bit-identical dcn302 vs dcn312 - DCHUBBUB's 74/77 is a
  **modeset** requirement and the firmware already did it. **That is the "new
  evidence" the page-flipping prohibition above asks for.** Keys:
  `fbvr_routed`, `fbvr_verify`, `fbvr_orig_len` (**the gate - if it is not
  8294400 nothing else means anything**), `fbvr_hit`, `fbvr_big`, `fbvr_fail`,
  `fbvr_calls`, `fbvr_ret1..8`. **BOOT 144: the hook worked and the hypothesis
  is DEAD (2pr).** `fbvr_verify 1`, `fbvr_orig_len 8294400`, `fbvr_orig_base
  0x8_8000_0000` (**BAR0's base - the GOP framebuffer IS BAR0 offset 0**,
  measured for the first time), `fbvr_biglen 64 MB`, `fbvr_hit 2`,
  `fbvr_big 1`, and `fbvr_ret1 == fbvr_fvd + 0x50` so it fired at exactly the
  intended call site. The control held - `IOFBMemorySize` still 8294400, no
  regression, vblank 133.801/137.04 Hz. **And `bufferFlipCount` and
  `bufferSwapCount` are still 0 with `fliprq_calls` absent.** Room was never
  what was missing. Keep the arg OUT; it costs a 64 MB kernel mapping and buys
  nothing.
- **SKYLIGHT IS NOW READABLE, AND `CompositorSW` IS PINNED DOWN (2ps/2pt).**
  `tools/re/dsc.py` reads the dyld shared cache directly - VA <-> file offset
  from the header mappings, fast RIP-relative LEA xrefs, and a minimal Mach-O
  wrapper so `objdump` disassembles cache-resident code - because nothing else
  here can (`otool` will not open the path, `llvm-objdump` has no `-b binary`,
  no `llvm-mc`, no capstone). Symbolize **without root**: the cache slide is
  per-boot and system-wide, so dlopen SkyLight in a scratch process,
  `vmmap <pid> | grep SkyLight` for the `__TEXT` base, then `atos -p <pid>` on
  `unslid + (base - 0x7FF807B42000)`. **Measure the slide every boot** - reusing
  an old one makes `atos` answer `libBLAS` for everything.
  **Established:** `CompositorSW::CompositeLayersToDestination` is at unslid
  `0x7ff807c3e820` - the function 2pd measured at 48 % of WindowServer's main
  thread - and it gates on `dest->type`, an **int32 at destination+0x10, which
  must be 1 or 2** (type 1 served from `dest->0x18`, type 2 from `dest->0x98`),
  after two bounds checks (`0x2c <= 0x34`, `0x30 <= 0x38`).
  `SLDisplayStreamCreateCommon` writes type **3**.
  **CORRECTION, and it matters: the `Unsupported sw composite destination.` log
  line is NOT the display defect (2pt).** 2pr called it the real blocker on the
  strength of 46 994 hits in boot 143 and 31 299 in boot 144 - **both armed,
  with no unarmed control**. On a later boot with the hooks live (`dispclk`
  nominal 131.671 Hz; note `rbgpu-arm` gates only the NVRAM capture, **not** the
  hooks) the count over 20 minutes is **1**. It tracks machine activity, and
  type 3 is a **display stream** (screen capture). Also, CompositorSW plainly
  **accepts** the display's destination - 2pd measured it doing real work, and a
  rejected destination returns immediately - so the display destination's type
  is already 1 or 2. **The open question is why the display composite goes to
  `CompositorSW` instead of `CompositorMetal`**, not whether the destination is
  valid. `MetalContext::StartComposite(WSCompositeDestination*, MTLLoadAction,
  MTLStoreAction)` is the Metal entry point. Read 2ps and 2pt together; 2pr's
  headline is retracted there.
- **THE SOFTWARE-COMPOSITING CHAIN IS NOW CLOSED END TO END, AND ITS LAST LINK
  IS ONE USERSPACE CALL (2px/2py).** SkyLight routes a display to
  `CompositorSW` when its accelerator id is 0 (2pv), and the id comes from
  CoreDisplay, not from any framebuffer property:
  `MPAcceleratorForDisplayDevice` returns `mpPriv->accelerator`. **The field
  names are Apple's own**, decoded from
  `CoreDisplay::CreateMPPrivDict(MPPriv const*)`, which pairs each field with a
  CFString key - `+0x58 service`, **`+0x5c accelerator`**, `+0x60 unit`,
  `+0x64 aliasID`, `+0x74 deviceFlags`, `+0x98 fbIndex` - so nothing here rests
  on offset arithmetic. Its **single writer** is
  `CoreDisplay::Framebuffer::InitializeMPPrivAccelerator()`
  (`mpPriv->accelerator = fb->0x18`), and `fb->0x18` is written only by the
  `dispatch_once` block of `CoreDisplay::GPU::UpdateAcceleratorForMetal()` from
  **`CoreDisplay::FindAcceleratorWithMTLDevice()`**, which is just:
  `MTLCopyAllDevices()`, then the first device with
  `isLowPower && !isRemovable`, else the first `!isRemovable`, and
  `[d acceleratorPort]`; **0 if none**, in which case the fill loop never runs
  and every `mpPriv->accelerator` stays 0. **`make accelport` reproduces that
  exactly in userspace** - no kext, no boot-arg, one second. **It has NOT
  answered the question yet**: run on the session's boot it prints 0 and
  declares itself INCONCLUSIVE (`rc=3`), because that boot has **zero
  `IOAccelerator` services** (`AMDRadeonX6800_AMDRadeonHWLibsX6800` is
  `!registered, !matched`), so Metal has no device for an unrelated reason.
  **Run it in an armed boot.** Non-zero there means the id is fine and the gate
  is the loop's other precondition - `gpu->0x58 == gpu->0x60`, i.e. the
  GPUWrangler gave this `CoreDisplay::GPU` no `Framebuffer` at all - which
  points at `GPUController::ProcessGPUAdded` and `GPUWranglerDevices()`.
  **BOOT 145 ANSWERED IT, AND THE ANSWER RETRACTS 2px AND 2pv (2pz/2q0):
  `accelport` returns NON-ZERO** (10243/10755 across two boots), and
  `SLSDisplayCopyCurrentMetalDevice` hands back our 680M, so the `accelId == 0`
  test never fires and 2px's "the id is 0" was measured in an unaccelerated
  boot **and** through a client process whose code path is not
  `CDDisplay::accelerator`. Everything upstream of the compositor choice is
  healthy in an accelerated boot - accelerator service up, Metal device
  present, `GPU: FB: 1 of 1 opened`, **`isAcc=YES`**, capability flag 1
  (`__WSSystemCanCompositeWithMetal` sets it iff some Metal device is not
  `isHeadless`, and ours is `isHeadless=0`), "Metal compositor activated"
  logged twice - **and `CDDisplay::render_update` still gets `CompositorSW`,
  10 of 11 samples.** Do NOT reason "Metal ran somewhere, so the mode and
  capability globals must be set": `CaptureSurfaceMetal::Populate` calls
  `WSGetCompositorMetal` **directly** and is not one of
  `WSGetCompositorForAccelerator`'s nine callers, so that inference is unsound
  (2q0 retracts it). Two candidates survive: **`CDDisplay->0x158 == NULL`**,
  which makes `CDDisplay::accelerator()` return 0 in six instructions without
  consulting CoreDisplay at all, and **2pd's original reading** - the
  destination is a raw CPU-mapped range, not a GPU surface, and
  `CaptureSurfaceMetal::WorksWithCompositor(Compositor*)` shows surfaces do
  validate compositors. Note `callxref` finds **zero** callers of
  `CDDisplay::compositor()` and `WorksWithCompositor` because both are
  virtual - a limit of the instrument, not dead code. Still unanswered from
  2px: whether our display uses the **MP** or the **VFB** ops table, though
  `kCGDisplayIsVirtualDevice = 0` and a VFB **delegates** the accelerator query
  to the backing device's own ops table anyway.
  **Run `sudo tools/compositor_check.sh` in an armed boot** - it prints its
  preconditions, both oracles, the boot's CoreDisplay log and which caller got
  which compositor.
  **Build `tools/re/dscinv.sh <image>` FIRST** before any byte scan: a cached
  `atos` grid over the whole `__TEXT` names every function in seconds (2304 in
  CoreDisplay, 13069 in SkyLight) and a NAME has now found the answer three
  times where byte scans missed it. Its caveat: a function no grid point lands
  inside is absent, so an absent name is not an absent function. New cache tooling, all under `tools/re/`: `dscsym.sh` /
  `dscsym_many.sh` (symbolize unslid addresses, slide measured **every run**;
  the `_many` form symbolized a 0x40 grid over CoreDisplay's `__TEXT` into its
  complete **2304-function inventory** in three seconds, which is what found
  the names - cheaper than any byte scan), `dscdis.sh`, `ripxref.py` (all
  RIP-relative encodings, not just `lea`) and `fldstore.py` (every store whose
  written bytes **overlap** a field - 216 sites where 2px's single-encoding
  search found ten).
- **The "7 MB" is NOT a memory limit** and not why anything is choppy.
  `tools/vramtest` allocates and reads back 1 GB plus an 8K texture;
  `recommendedMaxWorkingSetSize`, `RCC_CONFIG_MEMSIZE` and
  `MMMC_VM_FB_LOCATION_TOP` all say 4096 MB. 8294400 bytes is exactly ONE
  1920x1080x4 buffer - `IONDRVFramebuffer`'s own scanout surface, which System
  Profiler falls back to because no `VRAM,totalMB` property exists (2or). The
  stock framebuffer IS the culprit for the Hz menu, brightness, the string and
  the pacing - but by timing, not by memory (2ov). **2pd refines this and the
  user was right to keep pushing:** the number is cosmetic, but what it
  indicates is not. One fixed CPU-mapped buffer is not a GPU surface, and that
  - not its size - is why WindowServer composites in software. "The 7 MB is
  cosmetic" was true of the label and wrong about the defect.

## Rescue

`-rbgpuoff` in boot-args: the kext still loads, no peer instance exists.
