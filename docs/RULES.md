# RembrandtGPU — notes for the assistant

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
**APP TEXT IS FIXED (2np/2nr, 2026-10-22), in the assistant AND IntelliJ IDEA.**
The defect is the swizzle mode the **userspace** Metal driver picks for client
textures - 2lg found that place on 2026-10-20 and filed it "out of this
plugin's reach", which was right about the place and wrong about the reach.
**Lilu's user patcher is DISABLED on Big Sur and newer and cannot be
re-enabled** (rule 22, 2of): `isUserDisabled |= getKernelVersion() >=
BigSur`, so 2lm/2lp/2nb/2nm/2oa/2od all registered with `NoError` and never
existed. Do not write another `BinaryModInfo` here. The *mechanism* is still
open - every kernel symbol it needs is present and `KernelPatcher` works -
so this plugin does it itself, and **it works**: 2oi hooked
`_task_set_main_thread_qos` and `_vm_shared_region_slide` and placed the cave
into the shared cache of a process that was never given
`DYLD_INSERT_LIBRARIES`, verified from that process's own memory
(`-rbgpumtlkpatch`). Stage B (`-rbgpumtlkfix`) adds the detour, and 2ok placed
**both** cave and detour into a live TextEdit, verified from its memory, with
the app still running and no crash. What is NOT yet shown is that the cave's
code ever *executed*: 2oj established that native AppKit apps draw text
through CALayer backing stores and may never call `DetermineSwizzleMode`, so
"TextEdit did not crash" fits both "the cave is correct" and "it was never
called". The oracle is **IntelliJ IDEA** (`rbgpumtlkscope=idea`), which is
broken unwrapped and fixed by this exact policy through the DYLD trampoline -
it answers whether the per-app wrappers can be retired. Do not scope this at
the assistant, which hosts the session.

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

Persist it per app with `tools/rbgpu-app.sh install "<App>"`, which builds a
Dock-able `~/Applications/<App> (GPU).app`. Never a launchd environment
(rule 21). **Apple platform binaries (Safari, Finder, WindowServer) strip
DYLD_INSERT_LIBRARIES and cannot be reached this way at all.**

**Nor is TextEdit, or any native AppKit app** - 2oj/2ok: this defect is the
mode chosen for client `MTLTexture`s, which Electron and JBR hit by rendering
text into plain textures, while AppKit text goes through CALayer backing
stores that reach neither this chooser nor the kernel align manager (2nj,
Defect B). TextEdit renders correctly at baseline, so it can only ever show
"did not crash".

**texprobe is NOT an oracle for this defect** - it is clean at baseline (0/12,
every footprint 1.00x) because its round trip is self-consistent. Only a real
application tells you anything.

**Still open, and a SEPARATE defect:** a CoreAnimation layer backing store
comes out rearranged in about half of allocations (`tools/screenprobe.sh`
reproduces it on demand; 2na has the exact permutation). Forcing the chooser
does NOT fix it (2nd), because those surfaces reach neither this path nor the
kernel's align manager (2nj). Do not conflate the two.

**There is NO global fix, and 2ns closed all three routes.** gbAddrConfig is
already correct, so the kext cannot fix the math; `AMD_DeviceSettings` is
built in userspace by `amdMtl_HWL_InitDeviceSettings` from hwInfo, so it is
not a second kernel channel; and `LSEnvironment` in the real app's Info.plist
invalidates the bundle signature, after which LaunchServices refuses the app
outright (**error -54**, measured on IntelliJ, backups in
`logs/plist-backups/`). The decision is made in the userspace driver, one copy
per process. `tools/rbgpu-app.sh install-all` wraps every injectable app in
one pass. **Apple platform binaries and sandboxed App Store apps stay broken.**

**Note for any session run inside the the assistant app:** it is itself launched with
`DYLD_INSERT_LIBRARIES` set, so every shell inherits it and `open -a` passes
it on. Launch anything with `env -u DYLD_INSERT_LIBRARIES` or it starts under
whatever patch this the assistant is running - which once looked exactly like a
regression (2ns).

**The display half has never started** — Apple's controller matches Navi 21
PCI IDs, ours is `0x16811002`, and spoofing `device-id` kills the accelerator.

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
- The user runs `sudo tools/load.sh`, `sudo tools/unload.sh`,
  `sudo tools/efi.sh …` and `tools/plugin_check.sh`.
  **the assistant never runs these and never asks for the password.**
- **Never install into the EFI or `/Library/Extensions`.**
- Before any load, **commit** so a hang loses nothing. The user allows the assistant
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
    `BinaryModInfo` for this machine.** This rule has now been wrong twice
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
    script run from the assistant Code cannot see the the assistant app: `pgrep -x the assistant`
    returns nothing while `ps` lists it. This is what made 2no's first launch
    of the assistant "exit cleanly with no crash report" - the launcher's
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
- **`-rbgpulinear` is now a SUSPECT in the text defect (2mm), not neutral.**
  It forces client surfaces to LINEAR while the userspace driver lays them out
  in 64 KB tiles; every geometry whose tiled footprint exceeds its pixel bytes
  renders wrong and every geometry where the two agree renders correctly. Test
  pending: remove it and the height sweep must go clean. The note below is the
  pre-2mm reading and its ~80 % widget win still stands.
- **`-rbgpulinear` belongs IN and is currently OUT.** Boot 104 removed it and
  the widget/Dock/chrome artifacts came straight back on a healthy chip, so
  its ~80 % is real and not an artefact of boot 98's broken state. Restore:
  `sudo tools/efi.sh bootarg add -rbgpulinear && sudo nvram rbgpu-arm=1`
  It does **not** affect the text in either direction (2ln).
- **`-rbgpumtllinear`** (2lm) is **IN** - this line said "built but OUT"
  until 2026-10-22, when `efi.sh status` showed it in boot-args alongside
  `-rbgpuhwblt` (rule 15: read the config, not this file). A Lilu user patch
  forcing `DetermineSwizzleMode` to LINEAR in **Terminal only**. It is inert
  for the usual 2oc reason - no file to read patterns from - but it is NOT
  harmless to the 2od work: `kRb2lmMod` and `kRb2lpMod` declare the same
  dylib path and register FIRST, and `mapAddresses` fills only the first
  matching mod per `.map` line, so the real `startTEXT` lands on one of them
  rather than on ours. `rbgpu_mtlcave_mapped_mod()` exists for exactly this.

- **`-rbgpumtlkpatch` / `-rbgpumtlkfix`** (2og/2oi) are the kernel-side
  patcher, which **WORKS**: it hooks `_vm_shared_region_slide` and
  `_task_set_main_thread_qos` and places bytes with `_vm_map_write_user`,
  because Lilu's user patcher cannot run here at all (rule 22). 2oi placed
  and verified the cave in a live process. `-rbgpumtlkpatch` is stage A
  (inert cave only); `-rbgpumtlkfix` adds the detour and makes it live.
  `rbgpumtlkscope=<substring>` retargets by process name without a rebuild
  (default `cavecheck`). Three things learned there:
  **`proc_selfname` is the parent at this hook point** - the exec'ing process
  is only reachable as `proc_name(proc_pid(get_bsdtask_info(task)))`;
  **plain `VM_PROT_READ|VM_PROT_WRITE` succeeds** on a cache page, so Lilu's
  `CS_ENFORCEMENT` surgery is never needed; and **the write is per-process
  copy-on-write**, so the cave must be verified in *this* process before the
  detour is written. The qos hook runs on **every** exec, so a fault in it
  makes the machine unbootable - rescue with `-rbgpuoff`. Read `mtlk_*` from
  the IORegistry; `mtlk_verify=1` is the only line that proves anything.

## Rescue

`-rbgpuoff` in boot-args: the kext still loads, no peer instance exists.
