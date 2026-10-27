# RembrandtGPU — notes for the assistant

> **New session: read `docs/HANDOFF.md` FIRST** — current state, frontier,
> verified facts, procedure. `docs/JOURNAL.md` is the chronological evidence
> journal (~34 000 lines): authoritative for what was measured, in order,
> including every withdrawn reading, and **never to be trimmed**. Grep it by
> milestone tag (`2ks`, `2s4`) for the history of one step; never read it
> whole. `docs/HANDOFF-FULL.md` is a stale dump from 2026-10-05 — ignore it.
>
> This file was ~2800 lines of milestone narrative until 2026-10-26 and is now
> a reference card. **Nothing was deleted, only moved**: every milestone block
> it used to carry is in `docs/JOURNAL.md` under its tag, and the old version
> is in git history. Keep it short — put findings in the journal, not here.

Goal: graphics acceleration for the Radeon 680M (Rembrandt, `1002:1681` rev
0a, GC 10.3.3 / DCN 3.1.2 / PSP+SMU 13.0.x / SDMA 5.2.3) on macOS. Plan:
`docs/PLAN.md`. Machine: ASUS TUF A15 FA507NU (Ryzen 7 7735HS), macOS 15.8.1
(24H32), OpenCore. Boot-args include `-wegnoegpu`; csr-active-config **0xA43**
(measured from NVRAM. SIP reports Filesystem Protections *disabled*, yet the
dyld cryptex path still refuses writes with EROFS because its files carry the
`restricted` flag, enforced independently of the CSR bits).

## Where this stands

**The rendering half works.** An armed boot is a usable, accelerated Mac: the
CP halt is fixed (`-rbgpuvramadj`, 2ks/2ku/2kw), zero VM faults, Metal compute
bit-exact, System Profiler reads `AMD Radeon 680M` / 4 GB. The swizzle defect
(wrong texture layout, seen as broken text and images) is fixed **by default**
in two halves that are both needed — the kernel linear force and the
kernel-side userspace patcher — with no boot-arg or manual setup (2on/2oo/2op,
boot 132: *"not a single artifact or even an off pixel"*).

**The display half composites in software, and the chain is fully measured.**
The desktop runs on `IONDRVFramebuffer`; `CompositorSW` rasterises the layer
tree on the CPU at ~48 % of WindowServer's main thread (2pd). The reason is
not memory, not a flag and not the AGDC gate:

- **The AGDC gate is SOLVED** (2rx/2rz). Publishing an IOService that
  `safeMetaCast`s to `AppleGraphicsDeviceControl` makes the wrangler pair it
  (2r0, boot 159), and boot 164 won the race with every AGDC error gone.
- **Winning the gate wedges the machine** (2rz, boots 156/164/169/170), but
  **WHERE it wedges is no longer claimed** (2si). 2s2's
  `wait_for_queue_slot_gated` location is **withdrawn**: that function sleeps
  iff `this->0x23c - this->0x238 == 4` (a four-entry ring, base 0x118 stride
  0x48, depth a compiled constant), and boot 168 measured that nothing was
  ever queued — so the difference is 0 and it returns on its second
  instruction. It cannot block an idle pipe. The best current reading, from 56
  byte-identical `DisplayPipeState` dumps with `Clean` absent from 156 KB, is a
  **userspace livelock in `CoreDisplay::DisplayPipe::Commit`**
  (`0x7ff80411633c`) — a kernel sleep would log one dump, not 56. That is an
  inference from the dump count, **not** a measurement; what Commit retries on
  is the next read. So the default
  configuration deliberately does **not** open the gate, which boots 165–167
  verified keeps a working accelerated desktop. **`-rbgpuagdcearly` stays OUT**
  unless the boot is a deliberate, instrumented experiment: boot 164 shows the
  wedge still **writes its NVRAM note** and still **consumes its one-shots**,
  so it does not re-arm itself and a plain power cycle comes up clean (2s8).
- **The cheap way round is measured dead** (2s3): both `DisplayPipeSupported`
  and `TransactionsSupported` must be non-zero or
  `IOPresentmentCreateForRegistryID` fails. There is no transactions-off mode.
- **The ring is drained only by the framebuffer's `'vbl '` interrupt** (2s4),
  and **`AMDRadeonX6000` already contains the whole vendor side** —
  `AMDRadeonX6000_AMDAccelDisplayPipe` with `submitTransaction`,
  `executeTransaction`, `validateTransaction`, and
  `AMDHWDisplay::getDisplayPipeTransactionFlip`. So "write DCN 3.1.2 from
  Linux's `dcn31`" is not what the measurement supports; `docs/SCOPE-DISPLAY-PIPE.md`
  has the three tiers.
- **The display pipe DOES run here**, and the arm is self-service (2s5, 2s7
  confirmed it on hardware): `IOAccelDisplayPipe::displayModeDidChange`
  registers `signalVBLInterrupt` for `'vbl '` on our `IONDRVFramebuffer`, then
  disables it; `transaction_queue_gated` → `enableTransactionInterrupt` →
  `enableVBLInterrupt` re-arms it by itself when a transaction is queued.
  **Boot 168 measured that no transaction has EVER been queued** — past the
  gate, with the table not full (`diag_used` 571 of 768), `fb_vbl_ot_armed` is
  absent. So "the ring filled and nothing retired it" is excluded (2s9).
- **Our AGDC bridge is ELIMINATED as the cause** (2s9/2sa). Boot 168 found it
  refusing `0x921` all boot, because `sampleTiming()` ran 1085 ms before the
  framebuffer existed and nothing re-sampled. Build `8B261646` re-samples on
  framebuffer arrival (own thread_call, scoped to `early`), and boot 169
  measured it work: `agdc_resample_valid 1`, `agdc_tim_cached 2`, the panel's
  own timing from OTG0, **`agdc_link_result 0`**. The negotiation is now
  complete and correct — the remaining `0x711` refusals are the fifteen type
  bits Apple's own driver also refuses (`rb_agdc.h:182`, measured:
  `agdc_extcap_calls` 16, scaler answered, Cursor last). **And
  `fb_vbl_ot_armed` is still absent**, so the second gate is elsewhere.
- **What the wedge looks like from WindowServer's side** (2sa): no AGDC error
  in the log at all, and 56 byte-identical `DisplayPipeState` dumps in which
  `Clean` never appears once. `Fetch - Pipe` and `EOTF - GPU`, so it IS
  routing through the pipe; `System Gamma - Unknown` is the one operation that
  does not resolve. We answer **one** of the five capability types a real AMD
  Mac implements (1 CSC, 2 Gamma, 4 Degamma, 0x10 Scaler, 0x8000 Cursor).
- **Answering all five capability types changes NOTHING** (2sc/2sd). Boot 170
  answered 5 of 16 instead of 1 (`agdc_extcap_answered 5`, the Cursor query
  returning 0 instead of −2, `agdc_failed_calls` down by exactly four), and
  the display log is **byte-identical** to boot 169's — same 56 dumps, `Clean`
  still absent, **`System Gamma - Unknown` unchanged** — with
  `fb_vbl_ot_armed` still absent. So 2sb's correlation is unsupported. Types
  1, 4 and 0x8000 are eliminated; **type 2 is not**, because 2sc had to
  substitute its channel count (`vt[0x1b8](0xf)`, on an object we lack) and an
  empty gamma capability is plausibly indistinguishable from a refused one.
- **The gate is a PLANE COUNT, and the capability interface we have been
  answering is the wrong one** (2se). `SetPlaneState` and
  `BindIOAccelSurfaceToResource` are **userspace** (IOPresentment in the dyld
  cache), so no vtable slot of ours is missing — the pre-statement had the
  wrong shape. `IOPresentmentSetPlaneState` refuses on its second instruction:
  the plane count is a **byte at pipe+0x10**, set by
  `_IOPresentmentSetPlaneCount`, and IOPresentment's own error table names the
  result **"Plane out of range"**. A count of 0 means nothing can ever be
  staged — which is `fb_vbl_ot_armed` absent and `Clean` never appearing.
  The set IOPresentment runs on comes from a **CFDictionary**
  (`IOPresentmentCapabilitiesStructureFromDictionary` `0x7ff812e8df1c`) keyed
  `PlaneArray`, `GammaArray`, `CSCArray`, `LinearizationArray`, the scaling
  and cursor groups. **2sf corrected 2se here**: that dictionary IS built from
  the AGDC replies — `IOPresentmentCreateCapabilities` → `0x7ff812e95370` →
  `__GatherAGDCDisplayPipelineCapabilities` (`0x7ff812e979dc`), the address
  `rb_agdc.h:182` has cited since 2rf — so it is the same interface, not a
  separate one.
- **`PlaneArray` is the one group the capability types do not feed** (2sf), and
  it carries the count that gates everything: `CSCArray`←1, `GammaArray`←2,
  `LinearizationArray`←4, the two scaling arrays←0x10, `CursorArray`←0x8000,
  and `PlaneArray`←**nothing**. That reconciles 2sd — 2sc filled the five
  groups that gate nothing. `_IOPresentmentSetPlaneCount` has no caller inside
  IOPresentment, so CoreDisplay sets it from its own `IOPCapabilities`.
- **The plane count does NOT come from AGDC** (2sg). The entire AGDC input to
  `IOPresentmentCreateCapabilities` is three functions: 0x711 sixteen times
  (`__GatherAGDCDisplayPipelineCapabilities`), 0x921 once
  (`__GatherAGDCLogicalDeviceCapabilities`), and `__GatherAGDCLinkCapabilities`,
  which bit 0 of link-config +0x08 gates and which **2rg already measured
  inert** — all it writes is `0x19c78`, with four writers and zero readers in
  the image. Boot 170 answered all five types with no change, and we answer
  0x921 correctly and copy it wholesale. So there is no AGDC query carrying a
  plane count, and `PlaneArray` having no type is not a naming accident.
  **Next read: the `IOAccelDisplayPipe` plane interface in
  `IOAcceleratorFamily2`** — kernel side, where `nm` and `dumptable.py` work —
  not more dyld cache. IOPresentment links `IOAccelDisplayPipe*` stubs, and
  `AMDRadeonX6000` already implements that vendor half (2s4).
- **Grep `docs/JOURNAL.md` before reading any Apple function.** 2sg re-read a
  function 2rg had already read by address, and 2rg had already settled the
  question being asked. Third recorded instance of this error.
- **A boot of `8B261646` + `-rbgpuagdcearly` hung at the progress bar with no
  SSH** and needed a force restart (2sa). That boot was UNARMED
  (`agdc_error 3`, `agdc_oneshot 2`), so the bridge was inert in it and the
  cause is **unexplained**. It is a worse failure mode than the wedge, because
  it leaves no SSH window.

**THE IOPRESENTMENT PATH IS OPTIONAL — a working driver skips it** (2sj).
`luuumi-dev/nullarc` (Phoenix, RDNA 3, **DCN 3.1.4**, the same
`DCN_VERSION_3_1` grouping as our 3.1.2; MIT/GPL-2.0 own code, PolyForm-NC for
`src/phxaccel`, **no AI restriction**) gets WindowServer compositing at 120 Hz
with a framebuffer that uses **none** of IOPresentment, AGDC transactions,
`SetPlaneState`, `BindIOAccelSurfaceToResource` or plane state — they do not
appear in `phxfb.cpp` at all. `PhxAMDFB` subclasses **`IOFramebuffer`**, takes
over the firmware-lit GOP mode, **writes nothing to the display engine** (it
only *reads* OTG0), returns the boot console surface from `getApertureRange`,
reports one mode at a measured refresh, drives up to four VBL handlers from a
`thread_call`, and exposes `callPlatformFunction` VRAM/GPU-VA hooks; its
**accelerator** composites. Every one of those inputs this project already
measures (2pr, 2rn, 2s5, 2s9). So 2s2–2si's eliminations stand as facts but
their relevance does not, and **the frontier is a framebuffer kext, not more
IOPresentment**. We are better placed than nullarc: gfx1033 is inside Apple's
supported generation and our accelerator already works, so only the
framebuffer half is missing. **NOT established**: whether Apple's
`AMDRadeonX6000` will composite for a non-Apple `IOFramebuffer` — nullarc's
compositor is their own. **Unverified, do not act on it**: their comment that
CoreDisplay gates IOPresentment on "AMD" in the class name; the bare "AMD" in
that region of the cache is a hang-watchdog vendor tag, not a comparison.
Also measured (2sj): CoreDisplay's hang watchdog never fired in either wedged
boot, so there was no transaction to hang.

**THE DISPLAY HALF IS OPEN AGAIN, AND THE FRAMEBUFFER IS BUILT** (2sl/2sm).
`kmutil libraries` resolves `__ZTV13IOFramebuffer`, `gMetaClass`, `start` and
the `OSMetaClass` constructor out of **`SystemKernelExtensions.kc`** for a kext
that declares `IOGraphicsFamily` — so Route A was never closed, only untested.
nullarc confirms the split from a working driver: `PhxBringup.kext` to
`EFI/OC/Kexts`, `PhxAccel.kext` (which holds `PhxAMDFB`) to the **auxiliary
collection**. `src/fbkext/RBAMDFramebuffer` is built against that, and its
vtable is verified slot by slot: Apple's `__ZTV13IOFramebuffer` is 2824 bytes,
ours is **the same 2824**, and all 22 overrides land on the measured offsets
with three inherited slots as controls. What is **not** established: that the
load is *accepted* (stage 1 measures it — `kmutil libraries` does not check
authorization), that `IOFramebuffer::start()` completes on a non-Apple subclass
here (stage 2), and whether `AMDRadeonX6000` will composite for a non-Apple
`IOFramebuffer` at all. **And opening the gate is a different input to the same
machinery that wedges** — 2qp already opened `UseIOPresentment` by patching the
classifier and met *"System set to use IOP but no display pipes are present"*.
Nothing measured says the wedge will or will not come back.

Also withdrawn in 2sl: 2sj's caveat that nullarc's class-name claim was
unverified. **2ql had already measured that gate**, and `make useiop` prints it
live.

**Do not propose page flipping again without new evidence** (2pf): with
`-rbgpuflipprobe` routed, `getDisplayPipeTransactionFlip` was never called
once in a whole boot. **Do not force `UseIOPresentment` true** (2ql): 72 call
sites, and it selects the whole IOPresentment world rather than an allocator.

## Layout

- `src/common/` — freestanding C shared by the kext and host tools: IP
  discovery parser (`rb_discovery.c`), read-only probe (`rb_probe.c`),
  register list (`rb_regs.h`, every offset cites its Linux header), SMU
  queries (`rb_smu.c`, `rb_smu13.c`), OTG timing converter (`rb_otg.h`), AGDC
  structures (`rb_agdc.h`).
- `src/kext/` — `RembrandtProbe`, an IOService in its own IOMatchCategory
  beside the boot framebuffer driver. Probes once in `start()`, publishes
  `RBProbe` properties, unmaps everything.
- `src/fbkext/` — `RBAMDFramebuffer`, an `IOFramebuffer` subclass for the
  firmware-lit panel. Its **class name carries "AMD"**, which is the one thing
  that opens CoreDisplay's IOPresentment gate (2ql). It writes nothing to the
  display engine, delivers `'vbl '` from a `thread_call` at the measured
  refresh, and its personality **displaces** `IONDRVFramebuffer` (same provider,
  name and `IOMatchCategory`, score 30000 vs 20000) because `present` is the OR
  over all framebuffers. `make fbkext`; loaded by the user with
  `sudo tools/fbload.sh` — **never installed by the assistant**. 2sm.
- `src/plugin/` — the Lilu plugin: `kern_start.cpp` (hooks and probes),
  `RembrandtGPUPeer.cpp` (the IOService peer, the diag table and the NVRAM
  hot-key list), `rbgpu_agdc.cpp` (the AGDC bridge), `rbgpu_agdc_otg.cpp` (the
  read-only BAR5 OTG sampler). Boots from the EFI.
- `tools/hosttest/` — the probe against a simulated chip + parser fuzzing,
  ASan/UBSan. `make kext` runs it first; never load a kext whose hosttest
  fails. Add a case whenever `src/common` changes, and check the new test
  fails when the code it guards is broken.
- `docs/linux-ref/` — unmodified Linux files the code follows (`UPSTREAM.md`
  records each commit; fetch from torvalds/linux).
- `tools/re/` — static analysis. Kexts: `kc_extract.py` (kext out of a kernel
  collection as a standalone Mach-O), `machoread.py` (chained pointers,
  strings, symbols), `dumptable.py` (dump a data symbol / vtable, decoding
  pointers — **the instrument that works on a KC vtable**), `kc_stubs.py`
  (branch stubs → names), `vtslots.py` (boot-KC vtable offsets → methods),
  `strxref.py`, `switches.py`, `ptrsym.py`, `vcall.py` (every
  `call *slot(%reg)`), `rbpslot.py`, `fldstore.py`. dyld cache: `dsc.py`
  (reads all seven files; `ripall` for "does anything reference X",
  `dataxref`, `macho` wrapper so objdump works), `dscinv.sh` (**build this
  first** — an `atos` grid names every function in seconds), `dscsym.sh`,
  `dscdis.sh`, `ripxref.py`. Output to `build/re/`; findings in
  `docs/APPLE-STACK.md`.

## Workflow

- Build: `make kext tools` (CommandLineTools only, no Xcode); **`make plugin`
  for the Lilu plugin — `make kext tools` does NOT rebuild it.** The kext step
  resolves every import with `kmutil libraries`.
- **Always check the UUID moved** after a build
  (`dwarfdump --uuid build/out/...`). Two boots were lost to a stale build.
- After any plugin change, check: `kmutil libraries -p build/out/RembrandtGPU.kext`
  resolves everything with **0 from `SystemKernelExtensions.kc`** (the 2oz
  trap), and `grep -c IOGraphicsFamily src/plugin/Info.plist` is **0**.
  **This rule is about the PLUGIN only.** It is injected by OpenCore into
  `BootKernelExtensions.kc`, where a system-KC dependency cannot bind. A kext
  loaded at runtime or from the auxiliary collection links against every loaded
  collection, so `src/fbkext` **requires** `IOGraphicsFamily` out of
  `SystemKernelExtensions.kc` and `make fbkext` fails the build if it is absent
  (2sl, measured; 2oz/2qs/2ql each reasoned about the load path instead of
  testing it). Two kexts, opposite requirements, each enforced separately.
- Probe hardware rules: no writes except `MM_INDEX`/`MM_INDEX_HI` (enforced in
  `rb_probe.c`, asserted by the hosttest), never enable PCI decoding or bus
  mastering, never map BAR0's first 16 MB (the boot framebuffer's), read only
  inside the carve-out. Levels 2 (DCN) and 3 (+ GC) are opt-in: `RBProbeLevel`
  in `src/kext/Info.plist`, or `rbprobe_level=N`.
- SMU query rules (`rb_smu.c`; opt-in `make kext SMU=1`, plist `RBSMUQuery`,
  boot-arg `rbprobe_smu=1`): writes only to MP1 C2PMSG_66/82/90 of an MP1
  13.x, only allowlisted query messages (GetSmuVersion, GetDriverIfVersion,
  GetEnabledSmuFeatures), nothing sent unless the response register reads 1,
  stop at the first non-OK answer. Linux's `smu_msg_v1_send_msg`.
- Register gates follow Linux's **version grouping**, not the exact version:
  this chip reports MMHUB 2.4.0 (`mmhub_v2_3` offsets) and DMU 3.1.3
  (`DCN_VERSION_3_1`, `dcn_3_1_2` offsets). Check the grouping before adding one.
- Results land in `logs/<timestamp>-plugin/`.
  `tests/data/rembrandt-680m-tufa15.bin` is this machine's real IP discovery
  table; the hosttest parses it and runs the full probe on it.

## Hard rules

- **Always ask the user before loading or unloading a kext.** A bad load can
  hang or panic the machine and ends the session.
- The user runs `sudo tools/load.sh` (optional probe level: `sudo tools/load.sh 2`,
  applied to the staged copy only), `sudo tools/unload.sh`,
  `sudo tools/efi.sh …` and `tools/plugin_check.sh`.
  **the assistant never runs these and never asks for the password.**
- **Never install into the EFI or `/Library/Extensions`.** The framebuffer
  kext's third stage needs the auxiliary collection, which is an install and so
  is the user's own step; the assistant builds it and hands over the commands.
- The user runs `sudo tools/fbload.sh [beside]`. Arming needs **both**
  `-rbgpuownfb` in boot-args **and** `sudo nvram rbgpu-fb=1` (2sr). The boot-arg
  is the durable disarm; **the NVRAM one-shot is NOT** — `rbgpu-fb` survived a
  reboot after being removed and read back absent (2sq), because
  `IODTNVRAM::removePropertyWithGUIDAndName` calls no sync at all. So between
  experiments take the boot-arg out with
  `sudo tools/efi.sh bootarg remove -rbgpuownfb`, exactly as rule 9 says.
- **Never run `kmutil unload` on the framebuffer kext — it PANICS** (2sq).
  `IOFramebuffer::stop+0x48` reads `*(this->0x1c8)`, which is NULL on an
  instance that never completed configuration. To remove the kext, **reboot**.
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

## Rules that cost boots to learn

Each was paid for on hardware. `docs/JOURNAL.md` has the evidence under the
tag in brackets; `docs/HANDOFF.md` §7 states them with their reasoning.

**Routing Apple's code**

1. Never route a name unless `nm -m build/re/<kext>` shows it **defined**
   there — `solveSymbol` does not skip imports, it returns the bare slide and
   `routeMultiple` writes a jump there. [2c]
2. Disassemble a symbol's first 16 bytes and **refuse if a rel8/rel32 branch
   begins within the first 5**: Lilu copies displaced instructions
   byte-for-byte, so a short branch keeps its displacement and lands in
   Lilu's own buffer. [2gb]
3. **Grep the mangled symbol across the whole plugin before adding a route.**
   A second `RouteRequest` for an already-routed function **panics outright**.
   A new transform goes **inside the existing hook**. [2kn]
4. **`RouteRequest::from` is NOT written back** by Lilu 1.7.3, though the
   header documents it as such. Call `solveSymbol` yourself. A doc comment is
   not a measurement. [2kp]
5. **Cross-check every hand-written mangled name against `nm`** — a wrong
   length prefix makes `solveSymbol` return 0 and silently unscopes the fix
   (`IOCommandGate` is 13 characters, not 14). [2kr, 2s2]
6. **When a fix moves upstream, REMOVE the downstream patch it replaces, in
   the same change.** "Redundant" and "inert" differ — 2ku and 2ki composed
   and the halt came back. [2kv]
7. vtable dumps: **call offset = dump offset − 0x10**. A *non-virtual* call
   through a vtable **symbol** uses dump offsets directly. [2gp]

**Reading a boot**

8. **Read `rbgpu-boot` FIRST.** If it has not increased since the last
   capture, the boot was never armed and nothing in NVRAM is this boot's.
   `rbgpu-diag` carries no nonce, so "armed=1 reads live" certifies a **stale**
   capture. [2iz]
9. **The one-shots are one-shot** — the peer consumes and deletes them. Every
   armed boot needs them set again; replacing the kext and editing boot-args
   does **not** arm. And a one-shot can fail to clear (2qy, `mcr_oneshot 4`),
   so between experiments take the *boot-arg* out: the feature needs the arg
   *and* the key, so without the arg a stuck key is inert. [2iz, 2qy, 2rh]
10. **Run `sudo tools/plugin_check.sh` in the armed boot, over SSH, before
    rebooting.** It saves to `logs/<timestamp>-plugin/` on disk, which
    survives a power cycle; live NVRAM afterwards does not. **Look there
    before reading live NVRAM.** A boot that wedges leaves NVRAM only — so a
    key that must survive a wedge has to be in the peer's hot-key list. [2jg,
    2gq, 2gw, 2s5]
11. **A note is evidence only when its `b=` nonce is new.** [2gd]
12. **A stale note says nothing about where a boot got to** — not even weak
    evidence of an early stop. A user rebooting a black screen looks exactly
    like a hang. [2gw]
13. **A return code is not a result until the code that sets it has been
    read.** A named constant tells you what the NAME means, not what the
    function meant by it: request 0x1B's *success* path returns
    `kIOReturnUnsupported`, and `waitForStampTimeout` never waits. [2ha, 2s2]
14. **Absence is not a result.** A silent `return`, a full diag table and a
    missing NVRAM write look identical. Every path out of a probe must say
    something, and **check `diag_used` against `diag_max` before concluding
    anything from an absent key** — equal means the table filled and absence
    carries no information. Slots 0 and 1 are seeded with those two numbers so
    they cannot be evicted. A **last-wins key cannot express a per-entry
    fact**: publish one key per entry. [2eh, 2ek, 2fb, 2db, 2ds, 2qw, 2s6]
15. Before interpreting anything, check the loaded UUID against `build/out`,
    which one-shots were left set, and which boot-args are actually in
    `config.plist`. [2cu, 2cv]

**Instrumenting**

16. **The instrument is a suspect.** It has cost a wedge (2fo), a crawl
    mistaken for a hang (2ez), two panics (2fz/2ga, 2gg) and three destroyed
    captures (2rh's unguarded `sample` on a wedged WindowServer). When a
    measurement and a suspect are both new, **remove the suspect** rather than
    reasoning about it.
17. **Never substitute a value and then narrate it as a hardware result.**
    [2cy, which cost fifteen milestones]
18. **Do not build a table from n=1 rows.** [2ev, 2fs]
19. **A NULL check is not a pointer check** — and use the right yardstick.
    `0xffffff8000000000` is the **kernel's** base, correct for a kalloc'd
    object and **wrong for a kext symbol**; for those the floor is
    **`0xffffff7f80000000`**. The lesson that generalises: **a refusal path
    must publish the value it refused**, and **a captured pointer needs a
    captured control pointer in the same boot** — 2s6 published an address
    with nothing known beside it and could not tell a wrong capture from a
    wrong expectation. [2gh, 2qu, 2qv, 2s6]
20. **A registration that reports success is not evidence that a patch
    applied.** Log the enabling field itself and refuse when it is disabled.
    [2lq]
21. **Never put `DYLD_INSERT_LIBRARIES` in a launchd environment, and test a
    recovery procedure in the situation it exists for.** `launchctl setenv` is
    system-wide and dyld **kills** any process that cannot load the dylib. The
    documented undo addressed the wrong domain; the working form is
    `sudo launchctl asuser $(id -u) launchctl unsetenv DYLD_INSERT_LIBRARIES`.
    Inject per process. [2mq]
22. **Lilu's user patcher DOES NOT RUN on this OS at all** —
    `isUserDisabled = … || getKernelVersion() >= BigSur`, a `||`, so no
    boot-arg re-enables it; every layer then reports success and nothing
    exists. **Do not write another Lilu `BinaryModInfo` for this machine**
    (that code was deleted in 2op). The reusable lesson: **check that a
    subsystem is ENABLED on this OS before reading how it behaves.** What
    survives: verify a patch from the target process's own memory. [2nn, 2nz,
    2oc, 2of]
23. **`pgrep` cannot see the process a script runs inside, or its ancestors.**
    Use `ps -axo pid=,command=` with a prefix match. [2nu]
24. **The dyld shared cache is inside a grafted, Apple-signed cryptex** and
    cannot be modified — sealed volume, IM4P root hash, Apple IM4M ticket. Do
    not spend Recovery trips looking for it as a file. [2ny]
25. **Read the machine, not the manual.** Four Recovery trips were each ended
    by something asserted from general macOS knowledge instead of measured
    here (this is OpenCore, so it is SPACE at the picker, not `Cmd-R`; the
    Data volume is `MACOS - Data`). [2ny]
26. **A search is an instrument too.** A grep returning nothing is not
    evidence of absence until checked against a known-present case. Burned
    repeatedly: `objdump` separates mnemonic and operands with a **tab**;
    `strings -a` silently under-reports on a 373 MB KC (use `mmap` +
    `bytes.find`); `otool -tV` resolves symbol names in call operands, so a
    numeric search for a target address finds nothing; a `callq *0xNNN(` grep
    cannot see a **tail jump through a loaded register** — nor a *direct*
    tail `jmp`, which made `IODTNVRAMVariables::removeProperty` look as though
    it called nothing (2sr); `ioreg.txt` in a
    capture is two lines, not a registry dump; **`ioreg -c X -k Y` does not
    filter** and printed only `+-o Root` (2sn — use `ioreg -rl -c X`); a
    tool's **exit code can be its verdict**, so `make useiop && …` reported
    "failed" under a perfectly good report (2so); and **when xrefing a string,
    xref the string's START** (find the preceding NUL first) — a substring's
    address can never be referenced. [2lw, 2rk, 2r4, 2s0, 2s5, 2sn, 2so, 2sr]
30. **Boot-arg names: the KERNEL is exact, our greps are not.**
    `PE_parse_boot_argn` does `strncmp(token, name, token_len)` and then
    **requires `token_len == strlen(name)`** (`0xffffff8000b92a7c`), so it
    never prefix-matches — `-rbgpufbvram` cannot satisfy a check for
    `-rbgpufb`, and the project's existing prefix pairs are safe. Lilu's
    `checkKernelArgument` wraps the same function. But a shell
    `grep -q -- '-rbgpufb'` **does** match `-rbgpufbvbl`, `-rbgpufbvram` and
    `-rbgpufbaccel`, and that false positive landed in a **safety** check that
    then read as armed. Test a whole token
    (`tr ' \t' '\n\n' | grep -qx`), and do not name an arg so that it
    prefixes an existing one. [2ss]
27. **No vnode work at plugin init — the root filesystem is not mounted yet.**
    `RGPU::init()` runs during early kernel bootstrap, so a file read there
    panics before the progress bar, with no report written. Read files from a
    callback that runs at exec time. Note that `-rbgpuoff` stops `init()` from
    running at all, so such a boot is **not** a control for the init path.
    [2oe]
28. **Attribute every log line to a boot by its timestamp AND its pid before
    using it.** `/usr/bin/log show --last boot` means "the boot I am in now",
    and `--start/--end` is not a reliable filter either. **Pids are reused, so
    a timestamp window does not separate boots** — the reliable separator is
    the `Darwin Kernel Version` banner, once per boot. And `log` is a zsh
    builtin that shadows `/usr/bin/log`: always the absolute path, always with
    a known-present control. [2q0, 2s1]
29. **Derive a kernel slide from a pointer you solved yourself, never from
    `kextstat`.** Boot 167 measured the slide two ways in one boot: our own
    solved `handleVBL` gave `0xffffff7f966e9000` and resolved
    `signalVBLInterrupt` to the exact captured address, while `kextstat`'s
    reported base implied `0xffffff7f80ce9000` — off by `0x15a00000`. Boot
    166's identity check failed for that reason alone. [2s6, 2s7]

**The recurring error, stated once**

Thirty-odd recorded instances of reading a value without first establishing
what makes it meaningful. The practice that keeps catching it: **say in the
code what would have to be true for the reading to mean anything, and log it
when it is not.** Two variants worth naming: **read the evidence already in
hand before designing the next experiment** (2kx/2la/2lc all tested something
the user's photograph had already excluded; 2s5 found the answer in nine
captures nobody had read together), and **find the consumer before writing to
a field** (2q9/2qa, which then saved a boot twice over in 2s3).

## Chip facts established on hardware

- **GC seg0 = 0x1260** (agreed four independent ways), seg1 = 0xa000. Apple's
  encoded register ids are `(dword offset << 32) | 11`, 11 = GC_HWID. Offsets
  from `docs/linux-ref/gc_10_3_0_offset.h`; byte address is `(seg + dword) * 4`.
- Register accessor slots on `pm4+0x30` / `hw->0x368`: `[0x148]` resolves an
  id to a dword offset, `[0x118]` **reads**, `[0x120]` **writes**. [2fd, 2fg]
- Carve-out MC `0xF4_0000_0000`, system-physical `0x7_8000_0000`
  (`FB_OFFSET` 0x780), BAR0 `0x8_8000_0000` (**and the GOP framebuffer is
  BAR0 offset 0**, measured in 2pr). Apple's GPUVM space is `0xFF_BF..`.
  **`mc2pa(mc) = mc - vram_start + vram_base_offset`** — the one missing term
  that was the whole CP halt.
- **One** SDMA instance; Apple's Navi 21 path builds four (`hw->0xc2 * 2 + 2`).
  [2gc, 2gi]
- The hw engine array is `hw + 0x3b0 + engine*8`: `[0]` PM4/GFX, `[1..4]` the
  SDMA engines. The SDMA *walk* bases at `hw + 0x3b8`, so `engine[i]` is hw
  index `i + 1`. [2gu, 2gv]
- `GB_ADDR_CONFIG` is dword `0x13de`, BASE_IDX 0; reads **0x242** (4 pipes,
  4 packers, 1 SE).
- `AddrSwizzleMode`: 0 = LINEAR, 9 = 64KB_S, 22 = 4KB_D_X, 25 = 64KB_S_X,
  27 = 64KB_R_X. The `_X` family is what the swizzle defect comes from; **why
  is still NOT established** — 2nr blamed Navi 21's pipe count and 2ns
  measured that false (gbAddrConfig reads 0x242, agreeing with the chip).
- **`getPDEValue` returns flags only** — the address is ORed in by its caller.
  Never hook it to correct an address. [2ko]
- The panel: 1920x1080, `H_TOTAL` 2106, `V_TOTAL` 1129, pixel clock
  **341 987 984 Hz**, scanning at ~143.88 Hz. OTG0 is already running
  (`OTG0_CONTROL = 0x00011201`, OTG1-3 off, DMCUB 4.0.15.47), DMU segment 2 =
  **0x34c0**, BAR5 is 512 KB. Measure the refresh by **latching both endpoints
  on a `OTG0_FRAME_COUNT` edge**, never over a fixed window (2rn: a fixed
  window can only resolve ±1 frame, which cost 3.3 % against 0.03 %).
- `IODetailedTimingInformationV2` is 0xa0 bytes with `pixelClock` at 0x28;
  `AGDCLinkConfig_t` is 0xb0 and embeds it at **+0x04** (2rd/2rg).
- `IOFramebuffer` vtable, call offsets: `0x9b0` `getVRAMRange`, `0xad0`
  `registerForInterruptType`, `0xae0` `setInterruptState` (2pq, 2s5).

## Boot-args

The authoritative list with the reason for each is **`docs/HANDOFF.md` §9**.

**Must stay OUT:**
- `-rbgpufbmatch` — spoofs `device-id` and **kills the accelerator**; this
  plugin works by making Apple *accept* `0x1681`.
- `-rbgpupdefix` — composes with the upstream fix, the halt returns.
- `-rbgpuagdcearly` — wins the AGDC race and **wedges the desktop** (2rz).
- `-rbgpufbvramrange` — the hypothesis is dead (2pr); costs a 64 MB kernel
  mapping and buys nothing.
- `-rbgpunodisppanic`, `-rbgpusdmahook`, `-rbgpustampraw`, `-rbgpuregdump`,
  `debug=0x100`.
- **Never arm `-rbgpuforceenable` without `wdt=-1`** — otherwise the frozen
  boot panics at 120 s with no usable SSH window. [2hh]

**Default-on, no arg needed:** the kernel linear force (off with
`-rbgpunolinear`) and the kernel-side swizzle patcher (off with
`-rbgpumtlkoff`; `rbgpumtlkscope=` / `rbgpumtlkexclude=` override the scope,
and exclude always wins). Both are needed — boot 131 removed the force and the
desktop regressed. The patcher **fails safe**: every site is read back and
compared before anything is written, so an OS update that moves the driver
leaves things unpatched rather than wrong, and `mtlk_moved_verdict 1` is the
signal that happened. It runs on **every exec**, so a fault there makes the
machine unbootable — rescue with `-rbgpuoff`. `mtlk_verify=1` is the only key
that proves a write landed.

**Off by default, in use:** `-rbgpuvbl` + `-rbgpuvblpump` + `-rbgpufbvbl` (the
vblank; `fbvbl` takes the proc through the documented API and the pump is its
delivery mechanism — **do not remove the pump**, boot 138 delivered only
through it), `-rbgpufbvram` (the VRAM/model labels — a string literal we write
and a measured 4096 MB we publish; macOS detects neither), `-rbgpufbaccel`
(claims `kDisplayModeAcceleratorBackedFlag`; verified to take and to change
nothing downstream, 2q7), `-rbgpuiop` (opens the IOPresentment vendor gate;
correct, verified, inert without its one-shot — **keep it**), `-rbgpuagdc`
(the AGDC bridge), `-rbgpuflipprobe` (already answered, writes nothing), `-rbgpuagdccaps`
(answers AGDC extended-capability types 1 CSC, 2 Gamma, 4 Degamma and 0x8000
Cursor as well as 0x10 Scaler — off by default because three of the four carry
a substituted field, 2sc).
`-rbgpumcnew` / `-rbgpumcreg` are the runtime-metaclass experiment: `mcreg` is
the user's decision because its failure mode is a **hang**, not a panic.

**Tunables:** `rbgpuvblhz=<millihertz>` (vblank rate, refused outside
20000-400000), `rbgpufbvrammb=<N>` (the published VRAM size, refused outside
256-16384), `rbgpufbvramrangemb=<N>` (8-256, default 64),
`rbgpumtlkpatch` (selects the inert cave-only patcher stage),
`-rbgpuagdcpolicy` (leaves `NeedsPolicyDriver` absent, for the 2rk A/B),
`rbprobe_level=N`, `rbprobe_smu=1`.

**Gone, harmless if left in boot-args:** `-rbgpumtllinear`, `-rbgpuhwblt`,
`-rbgpumtlcave`, `-rbgpumtlfix`, `-rbgpulinear`, `-rbgpumtlkfix`. None is read
anywhere; remove when convenient with `sudo tools/efi.sh bootarg remove <arg>`.

**Note for any session run inside the the assistant app:** it is launched with
`DYLD_INSERT_LIBRARIES` set, so every shell inherits it and `open -a` passes
it on. Launch anything with `env -u DYLD_INSERT_LIBRARIES` or it starts under
whatever patch this the assistant is running — which once looked exactly like a
regression (2ns).

## Rescue

`-rbgpuoff` in boot-args: the kext still loads, no peer instance exists.
**Clear the one-shots before it comes back out.**
