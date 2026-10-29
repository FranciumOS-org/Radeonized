# RembrandtGPU — notes for Claude

> **New session: read `docs/HANDOFF-NEXT.md` FIRST** — current state,
> frontier, what to do next. It is the live handoff.
> **`docs/HANDOFF.md` is STALE (2026-10-26 / 2ry) — it predates the
> framebuffer takeover and the whole gamma chain. Read it only for its §7
> rules and §6 procedure, never for the frontier.**
> `docs/JOURNAL.md` is the chronological evidence journal (~41 000 lines):
> authoritative for what was measured, in order, including every withdrawn
> reading, and **never to be trimmed**. Grep it by milestone tag (`2ks`,
> `2s4`, `2v8`) for the history of one step; never read it whole.
> `docs/HANDOFF-FULL.md` is a stale dump from 2026-10-05 — ignore it.
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

**THE DISPLAY HALF IS FIXED (2xi, boot 215).** `-rbgpuschedtick` ticks Apple's
`AMDSWScheduler::checkTimestamps()` at the panel's 6944 us, and the chain 2xe
read end to end now runs: the stamps retire, the transactions complete, the
surfaces are released, readiness is regained and the compositor draws the next
frame. **Same `animtest` binary and the same 30 s test**: boot 210's
continuously animating window gave **2** `RBFBAmtrSets`; boot 215 gives
**3375 = 111.98/s**, so **1687x** with nothing about the measurement changed.
`advance()`'s gaps went `gmean` 72 321 -> **2 921 us** and `gmax` 6 005 202 ->
**88 346 us**, with the histogram's dominant bucket being [4096, 8192) us -
our own period. Against `display_change_handler` = 26 as the control,
`transaction stamp index N time out` went 219/48 -> **0**, and the WEDGE went
`event timeout` 26-46 -> **0** (a larger claim than the tick was built to
make, **n=1**, but coherent: 2we read the wedge as a *retirement* failure and
`checkTimestamps` is the retirement poll). The clock is 143.446 Hz vs the
panel's 143.830 (0.27 %), **but its jitter WIDENED** - stddev 6.2 % -> 13.9 %,
spread 5.96-10.66 -> 3.81-29.01 ms - cause **NOT established**.
**NOT DONE**: 111.98/s is **78 %** of 143.83 and the idle desktop reads 68-90,
so the last ~22 % is a new, smaller question and it is **not known** whether it
is demand-limited or still gated. Arm with all five one-shots
(`rbgpu-arm rbgpu-agdc rbgpu-iop rbgpu-fb rbgpu-tick`) and **read `tarm`
first**.

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
  **That read is DONE and the line of inquiry is RETIRED** (2u3/2u4): slot
  `0x8d0` is `IOAccelDisplayPipe::copyCapabilities`, *inherited*, a
  three-instruction read of `IOAccelDisplayPipeCapabilities` — there is no
  second capabilities channel — and boot 177 measured **`Plane Index: 0` in
  all 58 `DisplayPipeState` dumps**, so the count was never zero and never
  the gate. 2se/2sf/2sg's measurements stand; the inference stacked on them
  does not.
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

**THE FRAMEBUFFER WORKS AND THE ACCELERATOR PAIRS WITH IT** (2st/2su/2sv).
`RBAMDFramebuffer` displaced `IONDRVFramebuffer`, owns the panel, and
`displaypolicyd`, `WindowServer` and `SafeEjectGPUService` each print
`accel 0x1000004c8 /AMDRadeonX6000_AMDNavi21GraphicsAccelerator` against
`fb0:0 0x1000004c5 /RBAMDFramebuffer`. So **2sj's one stated unknown is
answered**: Apple's accelerator does pair with a non-Apple `IOFramebuffer`.
`UseIOPresentment()` is 1, `iop_saw_ws`/`iop_ready` are 1, and SkyLight logs
**`Metal compositor activated`** where the unarmed-accelerator boot logged
`Software`. **But the desktop is still laggy**, and WindowServer measures
**0.0 % CPU with every sample thread idle** — so it is idle waiting for frames,
not compositing slowly, which is the opposite failure from every earlier boot.
`fb_vbl_ot_armed` is still absent with the table only 480 of 768 full, so
**no transaction has ever been queued** even now. Suspect: our VBL thread_call
only runs while a handler is enabled, and 2s5 measured Apple registering the
`'vbl '` handler and then disabling it until a transaction is queued.
**2sy IS WITHDRAWN - APPLE'S DISPLAY-PIPE PATH IS NOT CLOSED HERE (2u0).**
**2pp measured `IOAccelDisplayPipeCapabilities = {DisplayPipeSupported=Yes,
TransactionsSupported=Yes}` on our own accelerator** on 2026-10-23, with
WindowServer's `IOAccelDisplayPipeUserClient2` beside it (journal line 24847),
and the ONLY setter of that name in `AMDRadeonX6000` is inside
`populateAccelConfig` (2sv §3). So that function **ran, published and did not
panic** - which kills 2sy's inference from "no boot panicked" to "it never
executes", and makes **2sw/2sx open again** rather than moot (2sx's
`CFG_USE_DPT` default of 1 now *explains* 2pp instead of being irrelevant).
**THE A/B IS RUN, AND ARM A HAS EVERYTHING (2u1/2u2).** Both arms publish the
property with both keys `Yes`:

| | gate 1 (vendor mask) | the two capability flags |
|---|---|---|
| `IONDRVFramebuffer` | FAILS - mask `0x01` (2ql) | PASSES (2u1) |
| **`RBAMDFramebuffer`** | **PASSES** - mask `0x04` (2t7) | **PASSES** (2u2) |

So **2u0's prime suspect is WITHDRAWN** - the framebuffer takeover does NOT
suppress the publish - and the framebuffer kext is a clear net gain: it buys
gate 1 and costs nothing. 2s3's two checks are satisfied in the armed
configuration. **Unreconciled, not dismissed**: 2sv's controlled zero for the
same property on 2026-10-26 23:42, with an unarmed accelerator excluded (boot
171 reads `armed=1` seven minutes later). Anything built on "the capabilities
are missing" must be re-checked against 2u2. Also unexplained:
`IOAccelDisplayPipeUserClient2` counts 2 with `IONDRVFramebuffer` and 1 with
ours.

**BOOT 177 OPENED THE WHOLE CHAIN, AND APPLE NAMES GAMMA (2u4).** Armed, with
the framebuffer and the bridge both up: `agdc_say = "PUBLISHED"`,
`agdc_error 0`, and in the display log *"Unable to get AGDC information"*,
*"System set to use IOP but no display pipes are present"* and *"Unable to
find display pipe"* each count **0**. So **2r5's empty association map is
FILLED** — `agdc_user_vendor 4` means IOPresentment opened the type-2 user
client and called selector 1 (`kAGDCVendorInfo`) four times, which was the
map's one statically unverifiable condition — `IOPresentmentCreateForRegistryID`
succeeded, a `DisplayPipe` exists, and `gUseIOP` was never latched. 2t8/2t9
both verified (`agdc_tim_ndrv_err 8`, OTG timing, no panic). The remaining
blocker is named in words: **`Attempting to get capabilities from plane with
no gamma`** (x7) -> *"Fallback GPU Display Pipe transaction submission
failed"*, with `System Gamma - Unknown` the one unresolved operation and
`Clean` absent from 58 dumps. That is **capability type 2**, the one suspect
2sd could not eliminate — and `-rbgpuagdccaps` was ABSENT in 177, so type 2
was gated off and never answered.

**THE GAMMA ARM LANDS, BUT ITS READOUT WAS LOST (2u5, boot 178).** With
`-rbgpuagdccaps` on: `agdc_extcap_all 1`, `agdc_extcap_answered` **3 -> 15**
(five types x three rounds), `agdc_failed_calls` **46 -> 34** (exactly the four
new types x three rounds), `agdc_extcap_gated` gone, `agdc_last_ret 0`. So
**Gamma was answered**. But `display-log.txt` is **0 bytes** because the
capture ran at **+28 s** (boot 177's ran at +46 s and got 2225 lines), and the
box was power cycled at +63 s, so that boot is unrecoverable. Per rule 14 the
zero is **not** a result: 2u4's prediction is **still open**. The repeat needs
no code and no new arg — only the capture taken later in the boot.
**Check `wc -l display-log.txt` against a known-present control in the same
capture before reading anything into it** (rule 26's shape, applied to a log
window). And `-rbgpuagdccaps`'s gamma answer carries a **substituted** channel
count (2sc): rule 17 — no reading from it is a hardware fact.

**THE GAMMA CHAIN IS TRACED, FIXED AND MEASURED (2u6-2v7, boot 182).** Our
AGDC attribute-`0x711` type-2 (Gamma) reply entry's `+0x28` is the dictionary
key **`MaximumSize`** — the gamma table entry count CoreDisplay reads:
`rb_agdc_gamma_entry()` -> `IOPresentmentCreateGammaDictionary`
(`0x7ff812e9b8e0`) -> `__gammaFromDictionary` (`0x7ff812e8fb40`) ->
`__fieldsFromDictionary` (`0x7ff812e8f866`) -> CoreDisplay's 0x38-stride entry
`+0x30` -> `IOPCapabilities::GammaChannelTableEntryCountAndType`
(`0x7ff80405ddd8`). With `RB_AGDC_GAMMA_CHANNELS = 0x400` (build `7C1FED31`,
hot-key `agdc_gamma_elems1 = 400`) boot 182's *"plane with no 3-channel gamma
table support"* went **7.12/dump -> 0**, and the capture shows
`LUT: R/G/B - [0..0.500489..1] with 1024 entries`. The 0x32c reply entry's map
is `+0x04` gate, `+0x0c` `NonAtomicApplicationPeriod`, `+0x10` `Type`, `+0x14`
`FixedFormat`, `+0x18` `ConfigurationID`, `+0x20` `MinimumValue`, `+0x24`
`MaximumValue`, `+0x28` `MaximumSize`. `RB_AGDC_GAMMA_SEGMENTS` is back to
**1** — Apple skips entry 2 at a `Type` of 1.
**2v7 withdrew 2v6's "linearization is the new blocker"**: Apple's own
`getFbCapabilityForDegamma` (`0xbfed0dc`) writes `Type = 1` for both degamma
entries and CoreDisplay's predicate accepts only mask `0x3fcc`, never bit 0,
so a **real** AMD Mac logs that WARN too, and `AddLinearization`
(`0x7ff8041092b4`) propagates nothing. **Do not raise the degamma `Type` off
1** (rule 17). `CoreDisplay::UseIOPGamma` (`0x7ff8040ea95c`) is eliminated —
all four `useIOP_gamma_on{IG,AMD,NVIDIA,APV}` prefs read `1 (absent)`;
`build/out/useiop` prints them.

**`System Gamma - Unknown` IS SOLVED, AND IT WAS NEVER OURS TO ANSWER
(2v8).** Static read, no boot. `DisplayPipeState`'s printer
(`0x7ff80410d1b6`) walks `0xc`-stride entries: `+0x00` op (`char*[9]` at
`0x7ff8404fc2b0`, bound `ja 8`), `+0x04` target (`char*[4]` at
**`0x7ff8404fc2f8`**, bound `ja 3`), `+0x08` Bypass byte, `+0x09` Dirty byte;
each table has a parallel **length** table (`0x7ff804144f58`,
`0x7ff804144fa0`). 2v7 read the target table **one slot high**: it is
`Unknown`(0), `GPU`(1), `Pipe`(2), `None`(3), so **`Unknown` is enum 0, not a
default** — the `ja` branch prints nothing at all. The live array is
`pipe->0x728`, target of op k at `0x4 + 0xc*k`, System Gamma being k=5 ->
**`0x40`**. `UpdateTargetStateColorProcessing` (`0x7ff8041079be`) writes
`0x28` Linearize, `0x34` CSC, `0x4c` Regamma and `0x58` EOTF (hardcoded `$2`)
— and **never `0x40`**. Its four early returns are also why **2v6's
unexplained `EOTF` `GPU`->`Pipe` move** happened: boots 179/181 bailed before
the writer block, boot 182 reached it. The **sole** writer of `0x40` is inside
`DisplayPipe::Commit` (`0x7ff804116f3e`), gated on `txn->0x1a0`, whose only
setter is `CoreDisplay::Display::SetSystemGamma(double)` (`0x7ff8040a19f0`).
So `Unknown` means **no transaction has ever been committed carrying a system
gamma** — the same silence as `fb_vbl_ot_armed` being absent, not an
independent blocker. **Do not spend another boot, AGDC field or capability
type on System Gamma.**

**THE SUBMIT PATH IS READ END TO END, AND THE PLANE-COUNT LINE IS BACK
(2v9).** No boot. The **66 dumps and the 66 failures are ONE event**: a single
straight-line block in `DisplayPipe::Commit` logs *"Fallback GPU Display Pipe
transaction submission failed"* (`0x7ff804118a4a`), prints the
`DisplayPipeState` (`0x7ff804118a72`), then *"Transaction Target State"* and
*"DisplaySurface Size"* — four counts of **66** in boot 182's capture. So the
dumps were never a separate phenomenon; they are the failure's own
diagnostic, and every reading since 2sa that built on "56/58/66 identical
dumps" was describing one failing submit retried.
That block runs only on the **GPU FALLBACK** path (`RunGPUDisplayPipe`
`0x7ff804112c9c` then `BuildTransactions`), entered via `jmp 0x118983` from
the `pipe->0x290` branch — CoreDisplay has already abandoned the hardware
pipe. (**Unresolved**: the other entry, `je` at `0x7ff804117ff7`, fires only
on an empty vector from `0x7ff80410ddff`, which cannot return empty — all
nine cases write at least `'ARGB'`.)
`IOPTransaction::Submit` (`0x7ff8040c8a56`) fails at its **first** step and
**logs nothing**, which matches 66 failures with zero IOPresentment errors in
2205 lines: `IOPresentmentBegin` -> `vt[0x78]`
(`0x7ff812e9cf8a`) -> `cmpb %r12b, %al; jbe` where `al` is the **plane count**
byte `backend+0x11` and `r12b` is 0 — so **a plane count of 0 is exactly and
only what makes it return NULL**. That byte = `CFArrayGetCount(caps
["DeviceArray"])`, and `DeviceArray` collapses to NULL if
`CreatePlaneDictionary` (`0x7ff812e9aca4`) does, which happens **iff
`caps->0x434 != 1`** — its one fatal branch. `0x434` is set by a single
instruction (`0x7ff812e97b13`) in `__GatherAGDCDisplayPipelineCapabilities`,
only when the **first** 0x711 query — capability type **`0x10` (Scaler)**,
capacity 8 — returns IOReturn 0, a non-zero out-length and a non-zero count.
Which is exactly what 2rf wrote at `rb_agdc.h:189`, now confirmed from the
consuming side three layers down.
**2se and 2sf are REINSTATED and 2u3/2u4's retirement of them is WITHDRAWN.**
2u4 retired the line because boot 177 read `Plane Index: 0` in all 58 dumps —
but that is CoreDisplay's **index** of the plane being described (0 = the
first one, the normal value), not IOPresentment's plane **count**. An index is
not a count; name the object a number belongs to before retiring a line with
it.

**The live machine is NOT the armed boot, and two instruments lied (2v9).**
`kern.boottime` 16:09:59 vs the capture's 16:07:01, `rbgpu-boot` still 182 and
both one-shots absent: the running box was **never armed**, so the absence of
`RBAMDFramebuffer`, the accelerator, `RembrandtGPUAGDC` and
`com.rembrandtgpu.fb` from it means nothing, and WindowServer at 207.9 % there
is not the boot-182 wedge. **`timeout` does not exist on macOS** — three
`ioreg` reads came back empty as `timeout: command not found` and looked
exactly like "service absent"; only a control in the same command
(`ioreg -rl -c IOPCIDevice` -> 4577 lines) caught it. **Before reading
anything from the live machine, establish which boot it is.**

**THE CONSUMER IS NAMED, AND READINESS IS A SURFACE POOL (2xe).** No boot;
boot 210, armed, exhibiting. **An animating window produces NOTHING**: a
`CABasicAnimation` repeating forever in a visible window gave `RBFBAmtrSets`
**2** in 30 s where `screencapture -v` gave **1827** (~61/s) in the same
minute - **900x**, so "an idle desktop composites nothing, which is normal" is
**dead**, and so is frame-demand as the lever. **WindowServer says why in
words**: `[ Watchdog ] display 5b81c5c0 has become stuck after 10.00390s of
unreadiness (2402 failures)`, and five readings give one constant -
1440/5.99733, 1438/5.98810, 2402/10.00390, 1042/4.33713, 1382/5.75765 =
**240.1 failures/s**. **The predicate is read end to end**:
`update_display_callback` (`0x7ff807e21dab`) -> `vt[0x4b8]` =
`WS::Displays::CDDisplay::is_ready` (`0x7ff807d6c14e`) ->
`CoreDisplay::Display::IsReady` (`0x7ff80408e316`) ->
**`DisplaySurfacePool::IsReady`** (`0x7ff804134cbe`) = *"some surface in the
pool has use count `+0x170` == 0"*. Apple names the field itself -
`UseCountProxy<CoreDisplay::DisplaySurface*>`, incremented by `GetNextSurface`
and released via `DisplaySurface::AccessComplete`. **And both the release and
the push notification come from ONE function**:
`CoreDisplay::IOPAttachment::HandleTransactionCompletionEvent`, which calls
`GetAndRemovePresentSurfaceForTransactionSurface` and
`DisplayPipe::NotifyIsReady` (what SkyLight's `display_ready_cd_callback` is
registered for). **The frame cadence hangs on the kernel's
transaction-completion event.** In the same configuration: 48
`transaction stamp index 59 time out` at a steady **1.0 Hz** over 63 s, with
exactly **three six-second gaps** and exactly **three** transactions - n=3,
a correspondence, not a law. **Wrinkle, not papered over**: `regained
readiness` was 0 in that window while 3 frames presented, so those three are
not accounted for by a readiness transition, and the surface chain is a
**static** read - nothing observed one surface's use count.
**WITHDRAWN IN THE SAME MILESTONE - do not build the tick yet.** The static
chain is sound: `AMDSWScheduler::initialize` creates its event source with
**`provider = 0`** (software, so there is no missing GPU interrupt to find),
`timerCallback`/`statCallback` have **zero** references in the kext, the
source is signalled from only `submitCommandBuffer` / `timestampUpdated` /
`swTimestampUpdated` / `checkTimestamps`, and `checkTimestamps` - the only
hardware poll - has **seven callers and every one is a waiter**. But "nothing
kicks the scheduler when idle, so tick `advance()`" is **refuted by 2ww**,
which measured `sub=12191 adv=46457`, mean gap 3.4 ms: `advance()` runs four
times more often than submits and is not starved. **Read the evidence already
in hand.** `advance()` is cheap and safe to call (its own `IOLock`, a no-op
when `ctx->0x71` is clear), so the option stays open - it is just not
indicated. **The deciding measurement is the one 2ww named**: `rbgpu-sched`
flushes at three fixed phase marks, so `advance()`'s rate in the SLOW state
has never been read.
**THE VBLANK WAS NEVER A CLOCK - OURS, AND FIXED (fbkext `39190382`).**
CVDisplayLink's `CVTimeStamp` hostTime over 5510 intervals: mean **7.4240 ms
= 134.70 Hz** against the panel's 6.9527 ms = 143.83 Hz, stddev 0.4615 ms
(6.2 %), spread 5.96-10.66 ms, and the nominal refresh read **115.94 /
119.97 / 120.00 / 133.72 Hz** across four runs - **a clock whose rate moves
with load is not a clock**. `armVBL()` computed `now + period` at the END of
`deliverVBL()`, so every tick paid for the handler before it: 0.47 ms, every
time, forever. Fixed by advancing the PREVIOUS deadline, with a **counted**
resync when a whole period is missed (arming into the past fires a catch-up
burst of ticks the panel never had). Mutation-tested: restoring `now + period`
fails the new assertion at tick 1.
**AND THE PROJECT'S RATE FORMULA WAS WRONG TWICE OVER.** `sets / (fires /
143.83)` (2wj, used by 2wi/2xa/2xd) divided by a rate that was ~134.7, and
`RBFBVBLFires` was published only every **1200 fires** - a staircase with
8.3-second treads, so a 25 s window carried +/-33 % error on its own time
base. **2xa's "71.30 == AnyDesk's 71.3 to three significant figures" is NOT
SUPPORTED**; its 1426x conclusion survives, the precision claim does not. Now
publishes **`RBFBVBLUptimeNs`** beside the counter (take elapsed time from
that, never from 143.83) plus `RBFBVBLResyncs`, at 288 fires. Rule 16: the
instrument was a suspect and both its errors pointed the same way.

**THE DISPLAY TRANSACTION WAITS ON A GPU STAMP THAT NEVER RETIRES (2x6).**
No boot - captures already on disk, plus a static read. The frozen
GPU-framebuffer boot names its own blocker **219 times**:
`IOAccelDisplayPipe (fbindex=0) transaction stamp index 59 time out`, over six
minutes, always stamp 59, interleaved line for line with the wedge
(`channel 12 SDMA0_PAGE event timeout` / `is not hung, keep running` /
`no channel associated with stamp_idx 12`).
**THE DIFFERENTIAL IS CLEAN BOTH WAYS**, ten captures, `display_change_handler`
as the control: CPU-framebuffer boots that **HAVE** the wedge (30 and 34
`event timeout`) log **ZERO** stamp time outs, GPU-framebuffer boots with no
transactions log zero, and where transactions flow the count scales with them -
`RBFBAmtrSets` 4 -> 43, 14 -> 103, 41 -> 219. So the reading is taken in the
configuration that exhibits the defect and the one that lacks the symptom gives
zero: **rule 39 satisfied in both directions**, which is what 2wz failed.
**2wl's ELIMINATION OF THE WEDGE IS WITHDRAWN** - it was reasoning, not
measurement. *"A GPU wedged at 0.2 buffers/s cannot make the screen smooth on
demand"* treats the wedge as a **throughput ceiling**; 2we had already read it
right - *"work is submitted and never retires, the hang check says not hung,
the restart cannot find the channel"* - and a **retirement** failure is kicked
by new submissions. Typing submits work. A connected AnyDesk session submits it
continuously. An idle desktop submits none. **2x0 and rule 39 still stand as
written**; the error was dropping the wedge on 2wl's argument instead of
re-measuring it in the exhibiting configuration, when the captures that settle
it were already on disk.
**Nothing about the driver changes** - 2x5 stands, `RBFBHubpWrites` tracks
`RBFBAmtrSets` exactly with 2 rejects a boot. The **frontier** is corrected:
not "macOS does not draw" but "the transaction it is drawing for cannot retire,
so it does not draw the next one".
**THE KERNEL RETIREMENT CHAIN IS READ END TO END AND IS ARMED.**
`transaction_queue_gated` (`0x14e60466`) sets `pipe->0x2a4 = 1` and calls AMD's
`enableTransactionInterrupt` (dump `0x898`), which calls `enableVBLInterrupt`
(dump `0x888`, refcount `pipe->0x2a8`, `fb->vt[0xae0](pipe->0xd8, 1)`) and sets
`pipe->0x281 = 1`; `signalVBLInterrupt` (`0x14e5dcb8`) calls
`signalTransactionInterrupt` (`0x14e5e0ec`) iff `0x281`, which returns early
unless `0x2a4` and otherwise signals `pipe->0xd0`. `signalTransactionInterrupt`
has **exactly one caller** and `signalVBLInterrupt` is registered **once**, for
`'vbl '`, at `0x14e5db41` in `displayModeDidChange` - **there is no second
interrupt type and no page-flip-complete interrupt we are failing to deliver**,
and both gates are set on the first transaction because `submitTransaction` is
called **from** `transaction_queue_gated`. So 2x1 is reconfirmed from the other
side: **the `'vbl '` retirement 2x1 forced is a DIFFERENT OBJECT from the GPU
stamp**, which is why forcing it moved nothing. Rule 33 again.
**ELIMINATED, recorded so no boot is spent on them**:
`isTransactionComplete` (`0xbe4a794`) decides the current transaction by
`fb->getAttribute('amtr', NULL) == 0` and ours returns 0 unconditionally, i.e.
**always complete** - the optimistic answer, so it cannot stall anything;
`beginTransaction` (`0x14e609fa`) and `signalTransactionComplete` (`0x14e60a00`)
are **empty base stubs**; and the 300 ms constant in *"IOAccelDisplayPipe"*
*"Transaction time out after 300ms"* appears **0 times** in the capture, so the
~10 s figure is not that waiter.
**THE TEST NEEDS NO BUILD, NO BOOT, NO KEXT AND NO TCC GRANT.** A connected
AnyDesk session both **captures the screen** and **keeps the GPU busy**
encoding; 2x5 never separated them. `build/out/metal_probe --busy` is
continuous GPU work on **ONE** queue (never `--queues`, 2wt's wedge trigger)
with **no capture at all**:
`env -u DYLD_INSERT_LIBRARIES build/out/metal_probe --busy 45 --heavy 20000 --depth 16`.
Smooth for 45 s then frozen again = the lever is **GPU activity**, capture is
dead, the defect is stamp retirement. Still frozen = capture comes back, test
it with Cmd-Shift-5. The tool prints its own buffer rate, so a dead run cannot
read as a negative result.
**WHERE THE FIX GOES IF IT CONFIRMS** - 2ws named it one milestone early: the
release decision is **per client, in software**, at `AMDSWScheduler::advance()`
(`0xbe950e0`), reachable only from `deferredCallback(IOInterruptEventSource *)`
(`0xbe93e18`) and from `timerCallback` (`0xbe9513c`), **which is never
installed**; and `enableStampInterrupt`/`disableStampInterrupt`
(`0x14e2653c`/`0x14e26542`) are **empty base stubs, never overridden**. So the
only installed trigger is an interrupt event source, and nothing advances until
some unrelated interrupt fires - which is exactly what typing and a busy
encoder supply. **And we already own a 143.83 Hz timer in the right context**:
the framebuffer's VBL `thread_call`. Ticking the advance from it is **6.95 ms**
against 2ws's free-running **175.77 ms** - 25x, and exactly the target.
**NOT ESTABLISHED**: nothing here observes one call to `advance()` (rule 35 -
instrument our end first), and **not established** that the stamp timeout and
the wedge are one defect - the wedge is on GFX/SDMA/Compute user queues and the
display stamp is index 59 on `fbindex=0` (rule 18).

**THE WEDGE IS A 265x DEFECT, AND IT IS THE SECOND PROBLEM (2wy-2x0).**
A `RunAtLoad` daemon (`tools/earlybench.sh`) put a single-queue bench in the
window no terminal reaches. Boot 203 straddled both edges of the churn
(+21 s to +84 s): **pre 22.4/35.5/47.1 ms, during 3870-6104 ms, and the first
sample after the last timeout (+85 s) back to 26.985 ms** - a **265x collapse
and a 143x recovery**, in one boot.
**So 2ww/2wx's "healthy until the first wedge, then permanently degraded" is
MEASURED FALSE**: the boot's fastest reading, **7.645 ms = 130.8 buffers/s**,
came 32 s AFTER the churn. **And the 175 ms quantum is NOT a quantum** -
boot 203 spans **7.645 to 6104 ms, an 800x continuum** with no stable value
near 175 ms. 2ws's lattice (T=175.77, R=0.966) is **not** withdrawn; it
measured a *state*. **Do not reduce it to "an average"** - R=0.966 is far too
tight for an 800x spread and nothing reconciles them yet.
**144 Hz needs 6.944 ms. Measured 7.645 ms (91 %), and boot 199's 632/s is
4.4x. THE HARDWARE DOES THE TARGET RATE; nothing needs to get faster.**
**BUT 2wz's "the frontier is the wedge" IS WITHDRAWN (2x0, the user's
correction).** Boot 203 is an `rbgpu-fb=0` boot - **it had the wedge and its
desktop was fine** (~25 fps, usable), because a CPU-composited desktop never
submits GPU work and so cannot see a GPU wedge. Every slow reading in 2wz is
**metal_probe's**, not the product's. **And the wedge is positively eliminated
as the GPU-FB blocker by 2wl**: typing made boot 196 smooth *while typing*, and
a GPU wedged at 0.2 buffers/s cannot do that on demand. **2wl's frontier
stands - why does a change on screen not produce a transaction unless it is a
keystroke?** Supply is the binding constraint; the wedge caps any GPU client
while it runs and is the second problem, not the first. **The ~10 s figure now
has THREE independent mechanisms predicting it** (2wi's 0.093 sets/s, 2wt's
6-10 s cliff, 2wz's 0.2 buffers/s) - rule 18 applies with more force, not less,
and the unification they suggest is refuted by the typing observation.
**ELIMINATED in passing**: contention (submits are LOW while slow - 82/29/44 -
and HIGH while fast - 2523/4597); and `adv/sub` as a regime discriminator
(2wy's proposal, withdrawn one milestone later - three fast readings give 0.66,
3.81 and 15.22). **INSTRUMENT**: Metal reached the daemon in 1 of 2 boots
(boot 202 logged `0 device(s)` on all 58 samples), and the daemon's header
`rbgpu-boot-at-start` **lags the nonce by one** - it runs before the peer
increments it, the same trap as 2wg's `RBFBCrumbBoot`.

**THE LIMITER IS ONE NUMBER AND IT IS NOT WHERE 2wq/2wr SAID (2ws/2wt/2wu).**
No boot - all of this is `build/out/metal_probe` on the live armed boot 198,
with `RBAMDFramebuffer` **not loaded**, so the display half is not involved.
**THE SPLIT, from one host timebase:**

    commit -> scheduled        0.092 ms   the driver has handed it on
    commit -> kernelStart      0.025 ms
    commit -> GPUStart       174.855 ms   THE GPU DOES NOT START
    GPUStart -> GPUEnd         0.014 ms
    GPUEnd  -> caller return   0.042 ms   completion is instant

So **the completion path is fine and the work waits 175 ms before the chip
begins it**. **2wr's "the completion signal is the defect" and 2wq's "the
notification is the whole defect" are WITHDRAWN** - their measurement (handler
and wait 9 us apart) stands, but both of those clocks are *downstream of
execution* and could not see a pre-execution delay. Rule 33 in a new form:
**name which side of the work a latency is on before attributing it.**
**THE READING IS VALIDATED** (rule 16): a control kernel with a caller-chosen
loop count moves GPU time over a **420x** range (0.014 -> 0.232 -> 5.896 ms)
while `commit->GPUstart` stays pinned to the tick, so `GPUStartTime`/`GPUEndTime`
are real GPU timestamps and not a detection time.
**IT IS A TICK.** 12 pipelined buffers all start within **1.391 ms** of each
other after a 176 ms idle wait, then run 12 us apart - one event releases the
whole batch. Load-independent: a second process holding 8 buffers in flight for
26 s changes nothing and itself gets **8 x 5.78 ticks/s = 46.2 buffers/s**.
Free-running: a lattice fit over absolute `mach_absolute_time` completions at
random phase locks at **T = 175.77 ms, R = 0.966 over 191 periods**, and
rejects every round candidate (25 vblanks 173.76 ms R=0.13, 6 Hz R=0.05) - so
the period is a number nobody wrote down.
**2wq's "too tight for a uniform poll" was simply wrong**: a serial loop
PHASE-LOCKS to the tick, so it always just misses one and waits a full period.
`--phase` breaks the lock and reads the period off a clean sawtooth - `d+wall`
constant at 173.7 ms to d=170, then **348 ms = 2T**.
**EVERY ENGINE, AND EVERY QUEUE IN A PROCESS, SHARE ONE TICK**: compute
174.385, blit/SDMA 173.103, render/graphics 173.766 ms; K=2 and K=3 command
queues released together (spread 0.26 / 0.39 ms). **So it caps the desktop** -
the graphics ring is ticked identically. 144 Hz needs **6.944 ms**; this is
**25x** too slow. Nothing needs to get faster; the quantum has to go.
**AND IT IS PER CLIENT, WHICH RULES OUT THE CHIP.** Two processes submitting
concurrently read the same period at **different phase** (118.172 vs 31.929 ms
in the shared mach epoch). A doorbell the CP cannot see, an MES timeslice and
clock gating are all chip properties and would give every client the same
phase. **The release decision is made per client, in software** - which points
at `AMDSWScheduler::advance()` (`0xbe950e0`), reachable only from
`deferredCallback(IOInterruptEventSource*)` (`0xbe93e18`) and the
**never-installed** `timerCallback` (`0xbe9513c`).
**ELIMINATED by measurement**: the whole timestamp-interrupt line
(`useTimestampInterrupts` `0xbea961c`, `enableTimestampInterrupt` `0xbe85904`
gated on `channel->0xa8`, the empty base stubs `enableStampInterrupt`/
`disableStampInterrupt` at `0x14e2653c`/`0x14e26542`) - it is about
*completion*, and completion takes 42 us. Also the 100 us poll loops
(`waitForHwStamp` `0xbe85536`, `waitForTimestamp` `0xbe46f9a`,
`IAMDHWInterface::pollWait` `0xbeab6e0` = `assert_wait_timeout(100 us)` +
`thread_block`): far too fast to be 175 ms, and on the wrong side of the work.
**NOT ESTABLISHED**: that `advance()` is the gate. Nothing above observes a
single call inside the driver - rule 35 exactly.

**THE WEDGE IS NOW A SIX-SECOND USERSPACE COMMAND (2wt).** `metal_probe
--queues K`: K<=3 is normal (171-173 ms), **K>=4 costs 6.1-10.2 s** and varies
run to run, the shape of detect-and-recover. A sender-matched capture over a
K=4 run is 2we's signature exactly - 5 `event timeout`, 16 `checkGPUProgress`,
16 `Signaling hardware error`, 5 `not hung`, 5 `no channel associated`, 412
`GPURestart`, against 478 lines as the control. **The channels name
themselves**: `channel 5 ComputeUQ2`, `channel 6 ComputeUQ3`, `channel 9
ComputeUQ6`, `channel 33 SDMA0`, each with a single pending stamp. So a Metal
command queue becomes a **compute user queue** and the later ones never retire
- the same shape as the one queue-count fact already established here, **one
SDMA instance where Apple's Navi 21 path builds four** (2gc/2gi). Since 2we the
wedge was only ever visible inside an armed boot; it now needs no kext, no
arming and no reboot. **A LEAD, NOT A CLAIM**: 2wi's desktop ran at *"a new
frame about every 10 seconds"* and this cliff costs 6-10 s, but 2wi's 10 s was
*also* matched quantitatively by the transaction rate (0.093 sets/s = one per
10.8 s), a different mechanism. Two numbers agreeing to one figure is not an
identification (rule 18).
**TWO INDEPENDENT DEFECTS**: the tick caps the rate at 5.7 Hz; the wedge adds
multi-second spikes on top. K=1..3 wedge nothing and still wait 175 ms.

**BOOT 196: THE CHAIN IS NOT SLOW, IT IS UNASKED (2wl).** The user installed
`F730123C` and armed `-rbgpufbretire` unprompted, and ran the box for **twenty
minutes**. Verified from the live machine, not the handoff:
`/Library/Extensions/RBAMDFramebuffer.kext` is `F730123C` and `boot-args` ends
`-rbgpufbaty -rbgpufbamtr -rbgpufbhubp -rbgpufbretire`. Rule 33: the machine
read afterwards booted at 14:55, one minute after the photo, `armed=0`, no
`RBAMDFramebuffer` - the armed boot is over and only NVRAM survives.
**THE DESKTOP IS CORRECT AND FULLY COMPOSITED** - translucent dock, colour
widgets, window shadows, a sheet, menu-bar blur, and System Information reading
**AMD Radeon 680M / GPU / 4 GB / Metal Supported / 1920x1080 @ 144.00Hz /
24-Bit ARGB8888 / Main Display Yes / Online Yes**.
**THE NEW FACT CHANGES THE QUESTION.** Typing into Spotlight made the frames
**smooth while typing**, and **adding characters counted, deleting did not**.
So the chain delivered at keystroke rate through the same
`setAttribute('amtr')` -> `hubpProgram` path that idles at 0.09 Hz. **A
timeout, a throttle and a missing vblank retirement all fail to explain a
chain that runs fast on command.** The frontier is no longer "why is our frame
delivery slow" but **"why does a change on screen not produce a transaction
unless it is a keystroke"** - which is upstream of everything this project
has built. 2wi's two candidates were both framed as throttles; neither fits.
**AND THE BOOT MEASURED SEVENTY SECONDS OF ITSELF.** `rbgpu-fbcrumb` reads
`b=195 s=12 f=10000 e=11 r=1 h=0 w=4 k=0 t=4` - the HUBP write set up, 4
writes, VBL running, 4 transactions, **as of 10000 fires = 69.5 s**, because
the crumb was written at four fixed counts (100/1000/10000/100000) and the
fourth is 11.6 minutes away. A twenty-minute boot left a record of its first
minute, and `RBFBRetireKicks`/`RBFBRetireDelivered` are registry-only, so
**`-rbgpufbretire` is STILL UNTESTED, third boot running.** **Rule 10,
sharper: what must survive an uncapturable boot has to be PERIODIC, not a set
of marks.**
**NUMBERING (rule 8)**: `rbgpu-boot` is **196** and the 14:18 capture read
195, so the twenty-minute boot is nonce 196 and the boot numbers in 2wi/2wj/2wk
were narrative, not nonces. Boot numbers are the nonce or they are nothing.
**Build `A356BE21` (2wl) + `7A6A56A9` (2wm, the one experiment).** The crumb is **periodic** (every
10000 fires, `s=` says which sample, so the last one is the whole boot); `p=`
is the fullest ~7 s window of transactions the boot ever saw (a total over a
boot length cannot tell a burst from a trickle, and this boot contained both);
`n=` counts transactions with flag bit `0x1` clear, the ones 2wh skips, so
"handed a content update and dropped it" differs from "never handed one"; and
`K=`/`D=` carry the retire kick. **Said before the boot**: `p=` in the tens
means the display half is finished and the defect is entirely upstream; `p=` at
1 or 2 means the smoothness did not come through our write and 2wi needs
re-examining; `n=` non-zero is ours and immediate.
**AND THE MACHINE IS CAPTURABLE NOW** - typing is the one thing that updates
smoothly, which is what a terminal is. `sudo tools/plugin_check.sh` in the
armed boot carries `RBFBRetireKicks/Delivered`, `RBFBVBL1Fires`, `RBFBAmtrSets`
and the display log together, which no amount of NVRAM can.

**BOOT 196: THE DISPLAY REPRODUCES, AND THE ONE-SHOT IS FIXED BY OVERWRITING
IT (2wj).** The user restarted **without** installing `452E6EE1` and
**without** `-rbgpufbretire`, so boot 196 is boot 195's configuration again:
same composited accelerated desktop, same ~1 frame per 10 s. **n=2 for 2wi**,
and **`-rbgpufbretire` is STILL UNTESTED.**
**THE SERIOUS FINDING IS OUR OWN CODE.** `rbgpu-fb` never consumed, so the
framebuffer took over on every power cycle and the HUBP write with it (it has
no one-shot of its own, 2wi) - and at the 0.09 Hz transaction rate **there is
no usable desktop from which to edit boot-args**, so the only way out was to
boot **WINDOWS** and edit the OpenCore config there. **A feature that cannot
be disarmed from the machine it runs on is a worse failure mode than anything
it was guarding against.** "The boot-arg is the durable disarm" has been the
answer since 2sr, but it assumes a machine usable enough to run `efi.sh` -
which 2wh removed as a side effect of succeeding.
**AND THE FIX WAS ALREADY IN THAT FILE, MEASURED, SINCE 2sr.** The consume was
`options->removeProperty("rbgpu-fb")`, and 2sr's own comment forty lines above
says why that cannot work: `removeProperty` tail-jumps to
`removePropertyWithGUIDAndName` which calls **no sync**, while `setProperty`
reaches `setPropertyWithGUIDAndName` and **does** - which is exactly why
`rbgpu-fbcrumb` has persisted reliably all along **through the same registry
entry**. The two facts sat two screens apart for eleven milestones without
being put together. **So CONSUME BY OVERWRITE, NOT BY REMOVAL**: build
`DEB8918D` writes a literal `"0"`, and the arming check twenty lines above
**already** treats `"0"` as not armed for both `OSData` and `OSString`, so
nothing else had to change. `IONVRAM-FORCESYNCNOW-PROPERTY` is kept but
evidently did **not** rescue the removal across boots 191-196, so on this
machine it is no substitute for using a syncing path.
**WHAT THIS CHANGES ABOUT PAST BOOTS**: the takeover was effectively **sticky
from whenever it was last armed**, which is why it has never once failed to arm
since 2st. **No earlier conclusion is invalidated** - the takeover is confirmed
in every boot by `RBAMDFramebuffer`'s own registry keys, not by the one-shot -
but "the fb armed again" was never evidence that a fresh key had been set. Rule
9 held for the *plugin's* keys (`rbgpu-boot` increments, so those really were
consumed) and not for this one.
**AFTER THIS BUILD ONE POWER CYCLE IS A FULL DISARM**: `rbgpu-fb` reads `0`,
`RBAMDFramebuffer` declines the panel, `IONDRVFramebuffer` matches as usual,
and the desktop comes back on software compositing - **usable enough to run
`efi.sh`**. A separate `rbgpu-hubp` one-shot is **available and not taken**:
one power cycle already disarms the whole chain and the smaller change is the
better one.
**AND THE CRUMB ALREADY MEASURES THE FRAME RATE WITH NO CAPTURE** - 2wi's
instrument is better than it was designed to be. It carries `f=` vblFires and
`t=` amtrSets, and the VBL thread_call runs at the panel's 143.83 Hz, so
**transactions/s = `t / (f / 143.83)`** from one `nvram rbgpu-fbcrumb` read
after a power cycle. That resolves 0.09 Hz against 1 Hz or 60 Hz with room to
spare, which is all the precision this question needs.

**BOOT 195: THE DISPLAY WORKS. THE GPU COMPOSITES TO THE PANEL (2wi).** The
HUBP write landed. Reported from the screen - the one instrument only the user
can read: frozen at first, then the cursor, then the lock screen after ~30 s,
**showing correctly and accelerated**, but a new frame only about every 10
seconds. **No capture** - the box could not be driven well enough to run
`plugin_check.sh` - so every `RBFBHubp*` value is **unmeasured and that absence
is not a result** (rule 14).
**WHAT THE SCREEN ESTABLISHES ANYWAY.** Nothing but the HUBP write can change
what the panel shows; before 2wh it displayed the firmware's GOP buffer at
VRAM offset 0, static, forever. A correct moving accelerated desktop therefore
proves the whole chain at once: all three `hubpSetup` gates passed (a refusal
writes nothing), `rb_hubp_check_addr` accepted the address, **the store reached
the chip**, and the address at `_AMD_PIPE_FLIP +0x08`/`+0x0c` is the right one -
2wg's field identification confirmed by the display itself. **AND THE SWIZZLE
ALREADY AGREED**: 2wh's one stated unmeasured risk, the thing most likely to
make the experiment ambiguous. A tiled surface fetched as linear gives regular
block-structured garbage; the image was *correct*. So `DCSURF_ADDR_CONFIG` /
`DCSURF_TILING_CONFIG` already describe the compositor's layout and **no tiling
write is needed** - that branch is CLOSED. **The display half of this project
is solved.**
**AND THE FRAME RATE WAS PREDICTED QUANTITATIVELY TWO BOOTS BEFORE IT WAS
SEEN.** `RBFBAmtrSets` was **13 over 167 s** (boot 193) and **17 over 183 s**
(boot 194), with no HUBP write in either build:

    boot 193   0.078 sets/s   one frame every 12.8 s
    boot 194   0.093 sets/s   one frame every 10.8 s

The user saw "about every 10 seconds". **10.8 s, from data taken before the
code existed.** So **1 frame per 10 s is the TRANSACTION rate, not a defect in
the write** - it faithfully displays every transaction it is handed, and
Apple's accelerator hands over 0.09 per second. **That rate was always there;
2wh did not cause it, 2wh made it VISIBLE.** Same shape as 2su's
"WindowServer idle at 0.0 %": the system was never compositing fast, and now
the symptom has a number.
**THE FRONTIER IS THE TRANSACTION RATE, AND THERE IS A NAMED CANDIDATE.**
`RBFBVBL1Fires` has read 0 for three milestones as "not attributed" and is now
the prime suspect. `RBAMDFramebufferVBL.cpp:204` registers **every** handler
`enabled = true` deliberately, yet boot 194 read `RBFBVBL1Enabled` **False**
with **0 fires** - so Apple registered its `'vbl '` handler, called
`setInterruptState(0)` on its own slot, and **never re-enabled it with 17
transactions in flight**. 2s5 measured the re-arm path
(`transaction_queue_gated` -> `enableTransactionInterrupt` ->
`enableVBLInterrupt`); it is evidently not running. If Apple's accelerator
never learns a vblank happened it can never retire a transaction and submit the
next, and **~10 s is the shape of a TIMEOUT, not of a throttle**. Boot 191 saw
handler 1 fire **once**, the only time ever.
**NOT ESTABLISHED - both candidates stay live**: (1) that VBL completion path;
(2) **the wedge** (2we), work submitted and never retiring. 2we's finding that
the wedge is an *independent* second defect still holds, but independent is not
irrelevant - it is now in the critical path for frame rate. **Whether they are
the same defect is NOT established** and must not be written as one: the wedge
is on GFX/SDMA channels, a different mechanism from the display interrupt.
**THE SAFETY GAP THIS BOOT EXPOSED IN OUR OWN ARMING.** The write re-armed on
every power cycle and had to be rescued with `-rbgpuoff`. Two causes compose:
`rbgpu-fb` **does not clear** (2sq - `removeProperty` calls no sync, commented
at `RBAMDFramebuffer.cpp:47`), so the takeover re-arms by itself; and
**`-rbgpufbhubp` has no one-shot of its own** - it is two boot-args, so once
the framebuffer is up the write arms unconditionally. Every other dangerous
feature needs a one-shot *and* an arg precisely so a bad boot cannot repeat
(rule 9), and the most dangerous one was built without one. **The boot-arg is
the documented durable disarm and it worked**, but the feature should not have
depended on that.
**Build `452E6EE1`, two changes.** (1) The crumb now carries `h=` refusal code,
`w=` writes, `k=` 1 iff the pair read back what we last wrote, `t=` amtrSets -
because boot 195 proved the display works and left **no evidence**, and rule 10
says what must survive an uncapturable boot has to be in NVRAM, where
`setProperty` does sync (2sr). **So read `nvram rbgpu-fbcrumb` even after a
power cycle.** (2) `-rbgpufbretire` re-enables the `'vbl '` slots we did not
register, once a transaction has actually arrived - the same condition Apple's
own `enableTransactionInterrupt` waits for. Field writes only (rule 31), its
own boot-arg so it stays separable from 2wh.
**NEXT**: `-rbgpufbretire` added, `-rbgpufbhubp` kept, **`-rbgpuoff` removed**.
If `RBFBRetireEnables` > 0 and `RBFBVBL1Fires` > 0 and the frame rate rises,
the completion path was the answer. Enables > 0 with fires still 0 means our
own delivery skips it. Fires > 0 with no rate change eliminates the VBL path
and leaves the wedge.

**THE FIRST WRITE TO THE DISPLAY ENGINE, BUILT (2wh, fbkext `D07493B1`).** No
boot yet. **What it does on the chip**: HUBP is DCN's display-data fetch block
and `HUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS(_HIGH)` is the address it reads
scanout pixels from, double-buffered, latched at the next vblank. It changes
**which bytes in VRAM reach the panel and nothing else** - no timing, no
clocks, no topology, no power state. A wrong value shows garbage or black
until the next correct write; one boot-arg reverses it.
**IT IS THE NARROWEST POSSIBLE WRITE**, because two measurements taken many
milestones apart agree: 2os's live pipe reads pitch `0x077f` (1920), format
`0x0008` (ARGB8888), address `0xf4:0x00000000`, and 2wg's surface is 1920
pitch, 4 bpp, `0xf4:0x1002a000`. **Pitch, format and the HIGH dword already
match - exactly one dword changes.** And the register **takes an MC address,
measured not derived**: the pair holds the carve-out base today. 2os's
`mc2pa(0xf400000000) = 0x7_8000_0000` is the *equivalent*, not the contents -
**mc2pa is NOT applied.**
**ALL DECISION LOGIC IS `src/common/rb_hubp.h`**, pure and freestanding;
`src/fbkext/RBAMDFramebufferHUBP.cpp` is only the mechanism. **Everything the
kext decides, the hosttest decides first** (`hubp_tests()`, 40 assertions) -
the only honest way to write code that can blank the panel. The offset table is
**cross-checked against `rb_regs.h`**, written from the same Linux header by a
different hand at a different time; the 0xdc stride is **asserted, never
used**; the **pitch-1 trap** is asserted explicitly (`0x77f` accepted, `1920`
REFUSED, because 2os annotates `0x077f` as "1920 px"); and the realistic wrong
inputs are named - a system-physical address (below the carve-out) and a GPUVM
one (above) are both refused. **Mutation tested both ways**: deleting the
format gate does not compile (`-Werror -Wunused-parameter`), and mutating the
measured constant `0x8` -> `0xa` fails the assertion.
**THREE GATES BEFORE ANY WRITE**, each publishing why it refused (rule 14):
2oq's four-register OTG signature (refuse 5 - the DMU segment `0x34c0` is a
constant, and a constant is an assumption); 2os's three HUBP0 values (6/7/8);
and the project's existing **`rb_dcn_is_denied()`** policy (9), re-checked on
the machine so a future edit cannot bypass it. Then per address: non-zero, in
the measured carve-out, **256-byte aligned** (DCN latches in 256-byte units,
so the low 8 bits are not address bits), HIGH unchanged, and one frame fits.
**WRITTEN PER TRANSACTION, NOT ONCE** - 2wg said so before the code existed:
a swap chain moves the surface each flip, so one write shows one stale frame.
**Gated on flag-word bit `0x1`**, the same test 2wf read the consumer making
before it touches `txn->0x10`; without it a colour-matrix-only transaction
would re-write a stale address. `hubpProgram()` runs on the accelerator's
thread with its locks held and so **allocates nothing, logs nothing, blocks on
nothing** (rule 31) - the BAR5 map is made once in `start()`, the VBL
`thread_call` publishes. HIGH before LOW, because DCN latches on the LOW write.
Read back **once**, on write #1: a register reading back what we wrote is the
difference between "the store executed" and "the store reached the chip"
(2lq in its register form).
**THE ONE UNMEASURED THING, AND THIS BUILD READS IT**: the **swizzle**. Pitch
and format match but tiling need not - the GOP framebuffer is linear and a
GPU-rendered surface may be `_X` tiled. `DCSURF_ADDR_CONFIG` /
`DCSURF_TILING_CONFIG` have **never been read here**, so both are published.
Deliberately **not gates** (nothing says what they should be, and refusing on
them would substitute a value - rule 17): they exist so a **scrambled image is
distinguishable from a wrong address**, the one ambiguity this experiment
cannot otherwise resolve.
**SAID BEFORE THE BOOT.** Arm `-rbgpufbhubp`, which **also requires
`-rbgpufbamtr`** (the address only arrives through `setAttribute('amtr')`);
`rbgpufbhubpmax=<N>` bounds the writes, absent = unlimited. Rule 30 checked, no
token prefixes another. **Check the arm FIRST**: `RBFBHubpReady` **true**,
`RBFBHubpRefuse` **0**, `RBFBHubpOrig` **`0xf400000000`**. If `Ready` is false
read `Refuse` and stop. Then: `RBFBHubpWrites > 0` with
**`RBFBHubpReadback == RBFBHubpLast`** means the write reached the chip, which
is the milestone whatever the screen does. A legible desktop = the swizzle
already agreed and the display half is DONE. A **regular, repeating,
block-structured** wrongness = tiling mismatch, and the next step is a tiling
write, **not** an address hunt. Black/noise with `Readback == Last` = the write
landed and something the guards do not cover is wrong. **Nothing changing at
all with `Readback == Last`** = DCN ignores the stores and wants
`DCSURF_FLIP_CONTROL` / the `SURFACE_UPDATE_LOCK` dance this build
deliberately omits. Tearing is expected and acceptable. **The screen is not
the instrument** - SSH and `ioreg` are, and rule 10 says capture in the armed
boot before rebooting: the last two captures were each cut to 6 files by an
early reboot, which is why 2wg could check no regression control.
**INSTALL**: plugin **`0D1F953A` unchanged in the EFI** (no rebuild, no
`efi.sh install`); fbkext **`D07493B1`** to the **auxiliary collection**.

**BOOT 194: THE SURFACE ADDRESS IS MEASURED, AND IT IS A LOW/HIGH PAIR
(2wg).** `rbgpu-boot` **194**, `armed=1`, both samples all-open (`gate=0
enabled=1 active=1 defer=0 vram=1 vmap=1`), `diag_used` 663/768, and the
experiment's own arm check passed: **`agdc_gamma_elems1 = 0x100` = 256**.
Only **6 files** (rebooted early again). Rule 15 met without `kexts.txt`:
`RBFBAmtrFlip*` exists and only `A454F066` publishes it. **Rule 8 met by
differential**: `RBFBCrumbBoot` is 192 in the boot-193 capture and 193 in this
one - it lags the nonce by exactly one in *both*, because the fbkext reads
`rbgpu-boot` before the peer increments it.
**A GREP READ THE WRONG BOOT'S TABLE.** The first pass over `nvram.txt` gave
`armed=0 agdc_oneshot=2 agdc_error=3 diag_used=0xa3`, all of it from
**`rbgpu-diag-prev`** - the previous boot's table, under a key whose name is a
superstring of the one matched. It read exactly like an unarmed boot and would
have thrown the measurement away; caught only because `agdc_gamma_elems1` was
published at all, which an unarmed bridge cannot do. **Rule 26: a key name can
be a PREFIX of another key's name, and a grep with no line structure crosses
the boundary.**
**THE STRUCT IS CONFIRMED A THIRD TIME, LIVE.** `RBFBAmtrPtr
0xffffff953cdba410`, `RBFBAmtrFlipPtr 0xffffff953cdbd6b0`, difference
**0x32a0** = `0x36b0 - 0x410`, both giving pipe `0xffffff953cdba000` two ways.
So `txn->0x10 == pipe + 0x36b0` is measured in a second boot at a different
kernel address.
**THE `_AMD_PIPE_FLIP`, MEASURED:**

    +0x00 u32 6          pixel format      +0x04 u8 0   rotation
    +0x05 u8  2                            +0x08 u32 0x1002a000  ADDRESS LOW
    +0x0c u32 0x000000f4 ADDRESS HIGH      +0x14 u32 1920  width/pitch
    +0x18 u32 4          bytes/pixel       +0x28 f32 1920.0  width
    +0x2c f32 1080.0     height            +0x30 u32 4
    +0x38 u32 2          +0x3c u32 4       +0x40..+0x21f  ALL ZERO

**`+0x28`/`+0x2c` decoding as exactly `1920.0f`/`1080.0f` is the control that
proves the struct is real** - two IEEE floats matching the panel's measured
mode at an offset nothing predicted them at - and `+0x14 = 1920`, `+0x18 = 4`
agree with `ax0_w`/`ax0_bpp` from a separate instrument in the same boot.
**THE SURFACE ADDRESS IS `0xf4_1002_a000`** - the measured carve-out MC base is
`0xF4_0000_0000`, so **0x1002a000 = 256.164 MB** into VRAM. An **MC** address,
not GPUVM (`0xFF_BF..`), which is what HUBP wants. **And it is stored as a
LOW/HIGH PAIR** at `+0x08`/`+0x0c` - exactly the layout of DCN's
`HUBP*_DCSURF_PRIMARY_SURFACE_ADDRESS` and `..._HIGH`. No other field in the
struct is split that way, which is strong independent corroboration of which
field it is.
**2wf's PREDICTION WAS WRONG AND THE INSTRUMENT ABSORBED IT.**
`RBFBAmtrFlipA58` reads **0**. 2wf named `flip->0x58` a **candidate, not a
result**, and published the raw blob beside it so a wrong guess would cost
nothing - it cost nothing. The `0xbe885a4` store is on a secondary/cursor
branch that did not run, consistent with `+0x40..0x21f` all zero and with
`executeTransaction` passing `ecx = 0` for the second `IOAccelResource2`.
`Fmt = 6` **is** in the predicted `{0,6,7,8,9}` and `Rot = 0` is small, so two
of three landed. Rule 35 working as intended.
**WHAT THIS BOOT DOES NOT SAY**: no display log and no `ioreg`, so every 2we
regression control (`set_transaction_args … 0x2c7`, `Failed to submit`,
`Fallback`, `DisplayPipeState`, `no gamma`, the wedge, `display_change_handler`
as the control) is **uncheckable and its absence is NOT a result** (rules
14/36). Positive evidence the chain still runs: `RBFBAmtrSets` **13 -> 17**,
`RBFBAmtrGets` 29 -> 37. `RBFBVBLFires` 23098, `RBFBVBL1Fires` still 0.
**THE ADDRESS IS PER-FLIP, NOT A CONSTANT** - said before the write is
attempted. We snapshot only the FIRST transaction and 17 arrived; a compositor
swap chain hands a different surface each flip. So pointing the scanout at
`0xf4_1002_a000` once would show **one stale frame**. The write must happen
**per transaction**, and `setAttribute('amtr', …)` is exactly the hook called
with each one. That changes the next step from "write a constant" to "write
the address this transaction carries". Also hold separate: the surface is at
256.164 MB while `ATY,fb_offset` publishes **16 MB** (2w9's deliberately
untruthful value, since the gate rejects 0) - the ATY offset only has to be
non-zero to open `initFramebufferResource`. **Do not try to reconcile them.**
**NEXT**: the first write this project has ever made to the display engine,
its own milestone and its own boot-arg - take `flip->0x08`/`+0x0c` out of each
transaction and program the DCN 3.1.2 `HUBP*_DCSURF_PRIMARY_SURFACE_ADDRESS`
pair, with a readback and a restore path.

**THE TRANSACTION IS READ END TO END, AND THE ADDRESS IS ALREADY IN IT
(2wf).** Static, no boot. `handleDisplayPipeTransaction` (`AMDFramebuffer`
`0x2b4df0`) is compiled **unoptimized**, so it reads almost like source.
`SUPPORTED_TRANSACTIONS` (`0x2bec98`, 8 bytes) = **`0x7d`**, and boot 193's
flag word `0x75` satisfies `0x75 & ~0x7d == 0`: **every bit we carry is
supported** (bit `0x2`, plane gamma, is the one bit absent from `0x7d`, and is
clear in `0x75` anyway). The body is `flags = txn->0x00 & 0x7d`, then one block
per bit, each `if (this->0x1198 & bit) dequeue(bit); else if (flags & bit)
handle()`. **Every handler dereferences exactly the pointer 2we's snapshot
recorded** - 0x40->`txn->0x40`, 0x20->`txn->0x38` (`setGammaTable`),
0x10->`txn->0x30` (`memcpy` of **0x30** bytes, the matrix size exactly),
0x4->`txn->0x20`, 0x1->`txn->0x10` - so the field map is confirmed from the
consumer side. `txn->0x08` is the u32 transaction ID
(`processDisplayPipeTransaction(txn->0x8, false)` in the tail). Its own gates
are AMDFramebuffer state: `0x908`/`0x820` bit 1 -> **`kIOReturnOffline`**
(`0x2d7`), `0xdd8` call/`0xe00` -> **`kIOReturnNotPermitted`** (`0x2e2`).
**THE PLANE STATE IS AN `_AMD_PIPE_FLIP`, 0x220 BYTES, AND IT IS `txn->0x10`.**
Produced by `executeTransaction` at `0xbe49cf6`: `r9 = pipe + 0x36b0` as the
out-parameter of `pipe->0x330->vt[0x198]` =
**`AMDHWDisplay::getDisplayPipeTransactionFlip`** (`0xbe88392`, dump `0x1a8`,
one slot past 2wc's `'amtr'` gate `vt[0x190]`), whose demangled signature
carries two `_AMD_PIPE_FLIP *` out-parameters - and the call site fills **one
or the other, never both** (`r9` on submit, the last stack arg on validate).
`flip->0x00` u32 format (`0/6/7/8/9`), `+0x04` rotation byte, `+0x06` the u16
the caller fills after. **Three independent agreements on 0x220**:
AMDFramebuffer's `memcpy(this + 0xf10, ps, 0x220)`; `pipe+0x36b0 + 0x220 ==
0x38d0`, the next pointer the snapshot already carries; and
`bzero(flip + 5, 0x21b)` in the writer itself.
**THE ADDRESS IS A GPU VA, from a leaf helper at `0xbe88846`**:
`mem = res->0x88; base = mem->vt[0x158](0,0); k = (uint8_t)res->0x14;
off = (k==0||k==0x80) ? res->0xf8 : (k==0x82) ? (u32)res->0xfc : 0;
return base + off`. Called on each `IOAccelResource2` (primary, secondary),
then both are passed as `%rcx`/`%r8` to a `vt[0x230]` on a third object which
does the storing. **THE CHAIN IS DELIBERATELY NOT FOLLOWED to the store
offset** - three more vtable layers in three more classes, and rules 33/35
(seven chains attached one link off; 2v9's five correct layers on the one fine
link). `flip->0x58` at `0xbe885a4` is the one address store seen directly and
is a **candidate, not a result**. It does not need deriving: **Apple fills the
struct before `setAttribute('amtr')` is called**, so the real addresses are in
memory we already hold a pointer to. Measure, do not derive (rule 17).
**2pf's "never called once" is neither refuted nor relied on** - it was
measured when no transaction had ever executed, so it is not evidence either
way now.
**AND THE AMDFRAMEBUFFER PATH COULD NEVER HAVE WORKED HERE** - independent
confirmation of 2sj. `processDisplayPipeTransaction` ends with
`this->0xdd8->vt[0x8c0](this->0xd40)`; rule 7 -> dump `0x8d0` of
`__ZTV13ATIController` = `0x975800`, a filler shared by **122** slots, i.e. the
pure-virtual stub. `ATIController` does not implement the plane-state program;
a subclass does, and none instantiates here (journal 34452). **Even Apple's own
`AMDFramebuffer` could not have completed this flip on this machine** - the
register write was always ours, not a consequence of the takeover.
**Build `A454F066`, NO new boot-arg**: `-rbgpufbamtr` now also follows
`txn->0x10` and takes all `0x220` bytes. `amtrFlipSnapped` is a separate flag
from `amtrSnapped` so "header read, plane state not" differs from "never
called". Published: `RBFBAmtrFlipSnapped/Ptr/Refused/Snap` plus
`RBFBAmtrFlipFmt` (+0x00), `RBFBAmtrFlipRot` (+0x04), `RBFBAmtrFlipA58`
(+0x58) - **the three fields the static read predicts, so the boot tests it**.
**Said before the boot**: bit `0x1` is SET in `0x75`, so `RBFBAmtrFlipSnapped`
must be **true**; false or a non-zero `Refused` means the flag word and the
pointer disagree and 2we's decode is wrong. `Fmt` in `0/6/7/8/9`, `Rot` small.
`A58` a plausible GPU VA = the static read landed; zero = the blob says where
the address really is and nothing is lost. **The picture stays wrong either
way** - nothing is written to the display engine in this build.
**NEXT**: one armed boot, plugin **`0D1F953A` unchanged in the EFI** (no
rebuild, no `efi.sh install`), fbkext **`A454F066` installed to the auxiliary
collection** - the only artifact that changed, and rule 15's install for it.

**BOOT 193: THE DISPLAY PIPE ACCEPTS TRANSACTIONS. EVERY FAILURE IN THE CHAIN
IS GONE (2we).** `rbgpu-boot` 193, armed, both one-shots 0, `agdc_error 0`,
`diag_used` 664/768, 20 files. Rule 15 on both: plugin **`0D1F953A`**, fb
`6F1EB613`. The experiment's own arm check passed: **`agdc_gamma_elems1 =
256`**. With `display_change_handler` **26** as the control in all five boots:

    set_transaction_args ... 0xe00002c7   122 / 132 ->  0
    transaction ID (N) generated error    122 / 132 ->  0
    Failed to submit transaction                134 ->  0   (no IOReturn: 0x at all)
    Fallback ... submission failed          69/73/3/66 ->  0
    DisplayPipeState                         69/73/66 ->  0
    3-channel linearization WARN                134 ->  3
    no gamma                                      0 ->  0   (2u6 holds at 256)

`Metal compositor activated` x4. **TRANSACTIONS REACH OUR FRAMEBUFFER**:
`RBFBAmtrSets` **0 -> 13**, `RBFBAmtrGets` 3 -> 29, `RBFBAmtrSnapped` true,
`RBFBAmtrPtr 0xffffff916784a410`. `setAttribute('amtr', &pipe->0x410)` lives
past `executeTransaction`'s `bool` arm, so the **submit** path runs.
**BOTH CHANGES WERE LOAD-BEARING, and that is now established.** 2wd could not
separate "gate 1 opened and gate 2 met" from "gate 1 was never shut". Boot 193
settles it the only way consistent with both: the `'amtr'` answer was
**necessary and not sufficient** - it cannot be sufficient (192 refused
everything with it in) and it cannot be unnecessary (the only route to
`setAttribute` is through the first test `vt[0x190]` gates, and sets moved
0 -> 13 with nothing else in that code changed). Two sequential gates, both
ours.
**THE TRANSACTION STRUCT IS CONFIRMED FIELD FOR FIELD FROM A LIVE POINTER.**
`RBFBAmtrPtr` is **exactly `pipe + 0x410`** (pipe = `0xffffff916784a000`), and
every pointer in the 0x80-byte snapshot is one of the `leaq 0xNNNN(%rbx)`
addresses `executeTransaction` loads: `+0x10` = pipe+**0x36b0** (plane state),
`+0x20` = pipe+**0x3660**, `+0x30` = pipe+**0x3680** (colour matrix), `+0x38`
= pipe+**0x2650**, `+0x40` = pipe+**0x38d0**; `+0x18`/`+0x28`/`+0x48..0x7f`
zero. Five independent controls on 2wc's field map in one reading.
**And the flag word is `0x75`, which decomposes exactly as `dirtyBits 0x3c1`
predicts on the submit arm**: 0x1 plane resource, 0x4 plane dst rect, 0x10
colour matrix, **0x20 PIPE POST-GAMMA**, 0x40 pre-CSC linearization SET; 0x2
(plane gamma), 0x8 (pipe pre-gamma) and 0x100 (postCSC VID) clear because
their dirty bits are. `0x20` is written at `0xbe4a2b2`, the instruction right
after the ceiling test that was refusing everything one boot earlier.
**THE WEDGE IS AN INDEPENDENT SECOND DEFECT.** First `checkGPUProgress` at
10:07:57.763, 15 s after boot, running the whole capture: 63 `Signaling
hardware error`, 36 `not hung`, 36 `no channel associated`, 26 `SDMA0_PAGE
event timeout`, 9 GFX, 1 VMPT, 3 `GPURestartDeferred`. 2w4 saw it start six
seconds after the first submit failure and could not say which way the
causation ran; **boot 193 has no submit failures at all and the wedge is
unchanged in shape and slightly larger**. Not caused by the display path -
clean differential, n=1. Stamp retirement: submitted, never retires, hang
check says not hung, restart cannot find the channel.
**NEW, NOT ATTRIBUTED**: `Setting main display 0x... offline` x7 and `Setting
offline display 0x... main in CGXConstructVirtualFramebuffer` x7, both ERROR,
absent from boot 192 - normal mode-set the earlier boots never reached, or a
new defect, **not established**. `RBFBVBL1Fires` 0 with `RBFBVBLFires 21218`,
also not attributed and now less interesting: the transactions go to
`setAttribute`, not through the VBL ring. `bad bits` 4, unchanged five boots.
**WHAT IS NOT FIXED**: nothing is written to the display engine.
`setAttribute('amtr', ...)` answers success without doing the work, so the
compositor believes 13 transactions landed and none did, and `-rbgpufbaty`
still publishes 16 MB while OTG0 scans offset 0. **The pipe is accepted; the
picture is not drawn** - exactly the state 2w9 aimed at.
**NEXT, no boot**: read `handleDisplayPipeTransaction` (`AMDFramebuffer`
`0x2b4df0`) against the snapshot's five pointers, above all the plane state at
**pipe+0x36b0**, which carries the surface the compositor rendered into. That
address is what DCN 3.1.2's `HUBP*_DCSURF_PRIMARY_SURFACE_ADDRESS*` pair must
be pointed at, and taking it out of the transaction beats inventing it.

**BOOT 192: `'amtr'` IS ANSWERED, THE REFUSALS DO NOT MOVE, AND THE GAMMA
CEILING IS BACK (2wd).** `rbgpu-boot` 192, armed, both one-shots 0,
`diag_used` 663/768, **all 17 files** - the first complete capture in four
boots. **Rule 15 met on BOTH kexts for the first time since 2w6**: plugin
`64D3D687`, fb **`6F1EB613`**, so the AuxKC install worked and `kmutil
create`'s `Code=34` was again a loud error over a write that landed.
**The ATY chain is reproducible** - all three samples byte-identical to boot
191's (`fi340 16777216`, `r380 1`, `active 1`, `gate 0`, `b35d/e/f 1`,
`w/h/bpp/fmt 1920/1080/4/4`).
**`'amtr'` WAS ASKED FOR AND ANSWERED**: `RBFBAmtrGets` **3**, `RBFBAmtrSets`
**0**, `Snapped` false, `Refused` 0. So 2wc's path down to the attribute is a
**real call path** - `getDisplayInfo` queried it once per call and we returned
success - and nothing ever reached `setAttribute`, which only the `bool` arm
calls.
**AND THE REFUSALS DID NOT MOVE**: `set_transaction_args ... 0xe00002c7`
**122 -> 132** with `dirtyBits 0x3c1 / options 0x801` unchanged and
`display_change_handler` 26 as the control in both. **So `vt[0x190]` is not,
or not the only, thing refusing.** Boot 192 **cannot** separate "we opened
gate 1 and met gate 2" from "gate 1 was never shut" - same count, same code,
two sites. Do not write either as established.
**What it DID settle, from the consumer**: `Failed to submit transaction ...
IOReturn: 0x` is 134 lines, **132 of `0x2c7`** and 2 of `0x2bc`, where boots
185/186 measured that same message 118/152 times **all `0x2d8`**
(`kIOReturnNotReady`). The NotReady era is over and the new code propagates
into CoreDisplay. `Fallback` 66, `DisplayPipeState` 66, and **132 = 2 x 66** -
exactly 2w3's "Submit runs ~twice per Commit". `no gamma` 0, `bad bits` 4, the
wedge still there (34 `event timeout`, 53 `checkGPUProgress`), and
`RBFBVBLFires 13200` with **`RBFBVBL1Fires` 0** where 191 saw it fire once -
not attributed.
**THE PIPE POST-GAMMA SIZE IS CoreDisplay's, AND WE ADVERTISE 1024.**
`IOAccelDisplayPipeGammaTable2::getTableCount` (`0x14e62a92`) is
`movq 0x10(%rdi),%rax; movl (%rax),%eax` - the first dword of the
`IOAccelDisplayPipeGammaTableData` the **userspace client supplied**
(`withGammaTableData` `0x14e62682`, `set_pipe_postgamma_table` `0x14e5e7e6`).
So it is not a hardware fact and the only lever we hold is the AGDC type-2
entry `+0x28` = `MaximumSize` = `RB_AGDC_GAMMA_CHANNELS`. Boot 192 measured it
live: **`agdc_gamma_elems1 = 1024`** against a consumer that refuses anything
over **0x100**.
**Plugin `0D1F953A`: `RB_AGDC_GAMMA_CHANNELS` 0x400 -> 0x100.** Still a
substitute (rule 17) but bounded by a **measured fact about the consumer**
instead of by a table CoreDisplay happened to print; non-zero, so 2u6's "plane
with no 3-channel gamma table support" (which fired only at ZERO) stays fixed,
with boot 192's count of 0 as the control. The hosttest keeps the **literal**
(2v1: a field compared against the macro that wrote it is not a test) and
gains the ceiling as its own assertion.
**Said before the boot**: `agdc_gamma_elems1` must read **256** or nothing
else in the capture means anything. Refusals -> 0 and `RBFBAmtrSets` non-zero
= the ceiling was the gate. Refusals -> 0 but sets still 0 = it validates and
CoreDisplay still will not submit; read `Fallback`/`DisplayPipeState`, not the
kernel. Refusals still ~132 = the ceiling is **properly** excluded and
`vt[0x190]` is the live candidate again - and then **probe it**, publishing
`((uint8_t *)hwd->0x40)[fbIdx*0xd0 + 1]` and the whole dword beside it, rather
than guessing a third time. 2w7 and 2wa were both settled that way; 2wc was
not. **The picture stays wrong either way.**
**NEXT**: one armed boot of plugin `0D1F953A` **installed into the OpenCore
EFI** (`sudo tools/efi.sh install`), fbkext `6F1EB613` unchanged in
`/Library/Extensions`. No boot-arg change.

**`kIOReturnUnsupported` IS ONE FRAMEBUFFER ATTRIBUTE WE NEVER ANSWERED
(2wc).** Static read, no boot; 2wb's frontier is closed and the defect is in
our own kext. `set_transaction_args` (`0x14e5ea68`) has ONE `retq` and only one
site that can return an arbitrary code: `pipe->vt[0x8a0](txn)` at `0x14e5f4bf`,
which is dump `0x8b0` = `IOAccelDisplayPipe::validateTransaction` (base:
`xorl %eax,%eax; ret`). The override `AMDRadeonX6000_AMDAccelDisplayPipe::
validateTransaction` (`0xbe49a9e`) is a kdebug wrapper around
**`executeTransaction(txn, false)`** (`0xbe49b54`), whose **FIRST** test is

    al = pipe->0x330->vt[0x190](getFramebufferIndex())
    if (!txn || !al) return 0xe00002c2 + 5*(txn != NULL)     /* == 0x2c7 */

**A grep for `2c7` finds five literal sites and MISSES this one** - the
assembler writes it `leal (%rax,%rax,4),%r12d; addl $0xe00002c2,%r12d`. Rule
26's sixth instance: **a constant can be a sum.** The other five are each
gated on a dirty bit; with the measured `dirtyBits 0x3c1` three are clear
(0x4/0x20/0x400), one needs `== 0x100` exactly, and the live one refuses a
**gamma table of more than 256 entries** (`cmpl $0x100,%r13d; ja`, `r13 =
getTableCount()`). **THAT CEILING'S EXCLUSION IS
WITHDRAWN BY 2wd - read 2wd, not the next three sentences.** 2wc wrote: the LUT
CoreDisplay actually submits is 256 entries in 71 of 73 dumps in boot 190 and
67 of 69 in 189, 1024 in only 2 of each, so two crossings in seventy-three
cannot make 122 refusals out of 122. **Those dumps are headed `Plane Index: 0`
and their LUT is the PLANE gamma, dirty bit 0x4, which is CLEAR in 0x3c1**; the
gate is bit 0x80, the PIPE post-gamma, which `DisplayPipeState` never prints.
(The `sort -u` lesson stands on its own: **a de-duplicated list is not a
count**.)
**AND THE GATE IS A FRAMEBUFFER ATTRIBUTE.** Rule 7: call `0x190` → dump
`0x1a0` (controls: dump `0x138` = `isDisplayEnabled` per 2w9, dump `0x258` =
`AMDDCNDisplay::isDisplayControlEnabled` per 2wa), an unnamed accessor at
`0xbec02ce` = `((uint8_t *)this->0x40)[idx*0xd0 + 1] & 1` - bit 8 of the dword
heading a `IOMalloc(this->0x58 * 0xd0)` per-display array. Its **sole** writer
is **`AMDHWDisplay::getDisplayInfo`** (`0xbe8700e`), forty bytes past 2w9's
ATY read:

    be87100  r = fb->vt[0xa30]('wsaa', &out)   /* the control */
    be87165  r = fb->vt[0xa30]('amtr', &out)   /* call 0xa30 -> dump 0xa40 = getAttribute */
    be8717e  if (r == 0 || r == kIOReturnNotReady) dstate |= 0x100; else &= ~0x100

`0x77736161` = `'wsaa'` = `kIOWindowServerActiveAttribute` (the control, a
documented attribute in the same instruction shape); `0x616d7472` = `'amtr'`,
a real `AMDFramebuffer` attribute with a case in **both** its `setAttribute`
(`0x2a580b`) and `getAttribute` (`0x2a6cb1`) and **zero** code references in
`IOGraphicsFamily`. Their bodies give the protocol:
`getAttribute` = capability bit, else `NotReady` while
`getOutstandingDisplayPipeTransactions()`, else 0, **never writing `*out`**;
`setAttribute` = `BadArgument` on null, `NotReady` if `tryAcquireIOPGuard()`
fails, else `handleDisplayPipeTransaction((_AMD_PIPE_TRANSACTION *)value)`.
**`RBAMDFramebuffer` handled ONE selector (`kIOHardwareCursorAttribute`) and
fell through to the base, which answers an unknown attribute with
`kIOReturnUnsupported`** - so bit 8 was clear every time, and that is 122
refusals with no variance across 62 seconds. A table size, a plane count or a
capability reply would have varied; a missing attribute cannot. **Seventh time
a correct static chain was attached one link off, and the second answer found
inside `getDisplayInfo`.**
**Build `6F1EB613`, `-rbgpufbamtr`, off by default**: `getAttribute('amtr')`
→ success (counted, `*value` untouched, copying AMDFramebuffer);
`setAttribute('amtr', v)` → `BadArgument` on 0 else success, and the **first**
call snapshots 0x80 bytes from `v` into `RBFBAmtrSnap` (rule 19: floor is the
**kernel** base `0xffffff8000000000`, and `RBFBAmtrRefused` publishes a
pointer refused). Neither handler allocates - they run on the accelerator's
thread with its locks held and the VBL `thread_call` publishes
`RBFBAmtrGets/Sets/Snapped/Ptr` (rule 31); `RBFBAmtrArmed` is published
**unconditionally** in `start()` beside `RBFBAtyArmed` (rule 15).
`v` is **`&pipe->0x410`**, the struct `executeTransaction` just filled - flag
word at +0x00 (0x1/0x2/0x4/0x8/0x10/0x20/0x40/0x100/0x201), plane resource
+0x10, dst rect +0x18/+0x20, colour matrix +0x30, gamma from +0x220 - so the
snapshot is **the first cooked display-pipe transaction this project has been
handed**, and the surface address in it is what the scanout has to be pointed
at.
**Said before the boot**: the transaction is **answered, not executed**, so
the picture stays wrong; expect `RBFBAmtrGets` > 0, `RBFBAmtrSets` > 0, and
the `set_transaction_args` line **gone**. Gets move but not sets → validation
passed, the submit path is gated elsewhere. Neither moves → `getDisplayInfo`
is not reaching us and the chain above the attribute is wrong.
**NEXT**: one armed boot of fbkext `6F1EB613` **installed to the auxiliary
collection** (rule 15 - boot 190 was lost to exactly that omission), with
`-rbgpufbaty` and `-rbgpufbamtr` both in boot-args.

**BOOT 191: THE PIPE IS ACTIVE AND TRANSACTIONS FLOW FOR THE FIRST TIME
(2wb).** `rbgpu-boot` 191, armed, both one-shots 0, `diag_used` 661/768. Only
6 files (rebooted ~52 s into the capture), both gaps recovered. Rule 15 met
without `kexts.txt`: **`RBFBAtyArmed = True`**, a key the 2w9 code publishes
*unconditionally*, so only `59124B3A` can produce it - 2wa's own prescription
used one boot later. The bytes are right: `ATY,fb_offset` =
`00 00 00 01 00 00 00 00` (16777216), `ATY,fb_size` = `00 90 7e 00 ...`
(8294400), 8 bytes each.
**THE GATE IS OPEN.** All three samples identical:

    ax0_fi340 16777216 (was 0)   ax0_fi34c 8294400 (was 0)
    ax0_r380         1 (was 0)   ax0_active       1 (was 0)
    ax0_gate         0 (was 3)   "ALL THREE OPEN - NotReady is elsewhere"

`r380` is `initFramebufferResource`'s own return, so none of its seven silent
exits fired; `active` is `pipe->0x298`. **`isActive()` is true for the first
time in this project**, and every prediction written down in 2w9/2wa landed.
**THE SUBMIT FAILURES COLLAPSED, at a level that survives a reboot.** The live
box is NOT boot 191 (rule 33), so INFO is gone and **`IOReturn: 0x2d8`
reading 0 is NOT a result** (that message is INFO-level, 2w2). `Fallback ...
submission failed` is ERROR and ERROR persists. Re-queried by sender, one pid:
**69 / 73 / 3** for boots 189 / 190 / 191, `display_change_handler` 26 in all
three as the control. And the three are at **00:24:23.762/.766/.849**, inside
90 ms, at the same instant as the last `display_change_handler` - then
**113 seconds of silence**. It failed until the pipe went active, then stopped.
**THE NEW FRONTIER IS PAST EVERYTHING WE HAVE EVER REACHED**, 122 times from
00:24:23.734 to 00:25:25.045:

    IOAccelDisplayPipeTransaction2::set_transaction_args(...)
        returning error 0xe00002c7 for transaction(N)
    AMDRadeonAccelerator [0]: transaction ID (N) generated error (0xe00002c7).
        dirtyBits=0x3c1, options=0x801

`0xe00002c7` is **`kIOReturnUnsupported`** (`IOReturn.h:109`, this machine).
**Transactions are created, numbered and handed to the kernel.** Every
milestone since 2s9 recorded "no transaction has EVER been queued"; that is
now **false**, and what changed is one 8-byte registry property. It also
explains **`RBFBVBL1Fires = 1`** where 2w0 measured Apple's handler 1 at 0
fires over 375 s - `enableTransactionInterrupt` -> `enableVBLInterrupt` (2s5)
finally ran.
**Rule 13 before anything is built on that code**: this project has already
been burned by `kIOReturnUnsupported` specifically - request 0x1B's *success*
path returns it (2ha). **Disassemble `set_transaction_args` first.**
**Unchanged**: the wedge (22 `event timeout`, 33 `checkGPUProgress`, 22 `no
channel associated`, first at 00:24:29.6), `bad bits` 4. **Not measured**: what
the screen showed - by construction it should be wrong, since the compositor
was told 16 MB while OTG0 still scans offset 0.
**NEXT, no boot**: disassemble
`IOAccelDisplayPipeTransaction2::set_transaction_args` in
`IOAcceleratorFamily2`, find every `kIOReturnUnsupported` path, with
`dirtyBits = 0x3c1` and `options = 0x801` as the measured inputs.

**BOOT 190: THE FBKEXT NEVER CHANGED, AND `ATY,fb_size` IS NOT A GATE (2wa).**
`rbgpu-boot` 190, armed, both one-shots 0, `diag_used` 662/768, capture
complete. **Rule 15 failed on the SECOND kext**: plugin `64D3D687` correct,
but `com.rembrandtgpu.fb` loaded **`45588775`** while `build/out` has
`59124B3A`. `-rbgpufbaty` WAS in boot-args and `ATY,fb_offset`,
`ATY,fb_size` and **`RBFBAtyArmed`** are all absent against three present
controls - and `RBFBAtyArmed` is published **unconditionally** by the new code,
so its absence proves the old binary independently of the UUID.
**It is not `fbload.sh`** (it `rm -rf`s and re-copies from `build/out` every
run). The kext that owns the panel comes up at boot from the **auxiliary
collection**, i.e. from **`/Library/Extensions/RBAMDFramebuffer.kext`**, which
holds `45588775`. A rebuild never touches it. **2w9's handoff repeated the very
omission it was written to fix** - it named `efi.sh install` for the plugin and
nothing for the installed framebuffer. **Every artifact a boot loads has its
own install path, and a rebuild refreshes none of them.**
**What the boot bought: `ax0_b35d = 1`** - with `ATY,fb_size` absent, which 2w9
said was impossible. The cause is the early-out 2w9 quoted and reasoned past:
`bool a = this->vt[0x248](); if (a || !fb) return a;`. `vt[0x248]` (dump
`0x258`) is **`AMDDCNDisplay::isDisplayControlEnabled`** (`0xbec012e`), a
**live register read** - `this->0x28`'s `vt[0x118]` (the project's known READ
slot, 2fd/2fg) on id `this->0x5c0 + 0x38*idx`, masked with `this->0x6f0`. **The
firmware-lit panel satisfies that gate for free** (OTG0 is already running,
`OTG0_CONTROL = 0x00011201`). It is the hardware answering, not us.
**So 2w9 is CORRECTED: `ATY,fb_size` is NOT a gate here** - it only feeds
`info+0x0c`, and `max(info+0x08, info+0x0c)` makes it optional because
`info+0x08` is already 8294400. **Only `ATY,fb_offset` is load-bearing** and
`pipe->0x340 == 0` is the single fatal check. Everything else is byte-identical
to boot 189 (`fi340` 0, `fi34c` 0, `active` 0, `gate` 3, `0x2d8` 148,
`init_framebuffer_resource` 0, `display_change_handler` 26 as the control) -
which is a clean control on the probe extension itself.
Fifth time a correct disassembly was attached one link off, second in two
milestones that the probe caught it: **when a function has an early-out above
the branch you care about, read what makes the early-out fire before claiming
the branch is reachable.**
**NEXT needs NO new build** - `build/out` already holds fbkext `59124B3A` and
plugin `64D3D687`. The framebuffer kext must be **installed** (the user's
step).

**BOOT 189: THE DEFECT IS TWO REGISTRY PROPERTIES WE NEVER PUBLISHED (2w9).**
Build `C025F308`, armed, both one-shots 0, `diag_used` **658**/768, all three
samples byte-identical. **`ax0_w/h/bpp/fmt` = 1920 / 1080 / 4 / 4**, so
`getPixelInformation` succeeded and the base's half is correct; the failure is
unambiguously inside AMD's hook, as 2w8 predicted. And
**`ax0_fi348` = 8294400 = 1920x1080x4 EXACTLY while `ax0_fi340` = 0** - the
size right, the address zero.
**THE CHAIN IS NOW CLOSED, READ END TO END.**
`set_display_mode_and_vram` (`0xbe48604`) passes **`&pipe->0x340`** as the
`_FRAMEBUFFER_INFO*` out-parameter of `machine->0x188`'s slot `0x130` and then
requires `ok`, `pipe->0x35d == 1` and `pipe->0x340 != 0`. Rule 7 on
`AMDRadeonX6000_AMDHWDisplay`: call `0x128` -> dump `0x138` =
**`isDisplayEnabled`**, call `0x130` -> dump `0x140` =
**`getDisplayInfo(unsigned, bool, bool, void*, _FRAMEBUFFER_INFO*)`**. And:

    getDisplayInfo (0xbe8700e)
        info+0x00 = *(uint64_t *)OSDynamicCast(OSData,
                        fb->getProperty("ATY,fb_offset"))->getBytesNoCopy()
        info+0x0c = *(uint32_t *)  ...  ("ATY,fb_size")   ...
    isDisplayEnabled (0xbe86f9c)
        returns *(uint64_t *)ATY,fb_size != 0      -> pipe->0x35d

**So BOTH properties are hard gates and both values must be NON-ZERO**, and
`ATY,fb_offset` is a byte **OFFSET**, not an address: `initFramebufferResource`
computes `pipe->0x340 + hw->0x370->vt[0x1e0]()` (= `mem->0x50`, the VRAM base)
and reserves `max(info+0x08, info+0x0c)` there. `pipe->0x328` is
`accel->0x1a38` = **`AMDRadeonX6000_AMDNavi21Hardware`** (`newHWInterface`
`0xbe82fda`), whose `vt[0x2f8]` returns `hw->0x398` = `pipe->0x330`, the
HWDisplay - closing that loop too.
**The class is `OSData`, established from the USE not the GOT**: the consumer
calls `vt[0x178]` and then dereferences the result, so it is
`getBytesNoCopy()`. An `OSNumber` would have returned the value itself.
**WE PUBLISHED NEITHER.** `grep -c ATY framebuffer.plist` = 0 against a valid
in-file control (`IOFBMemorySize`, `IOFBGammaCount`, the `RBFB*` family all
present). `fi340 = 0` and `fi34c = 0` are **exactly those two absences
agreeing** - two fields, one cause.
**AND NEITHER STRING EXISTS IN `IOGraphicsFamily` OR `IONDRVSupport`**
(control: `ATY,SurfInfo` present in `AMDSupport`), so the `IONDRVFramebuffer`
desktop could never have got a hardware display pipe either. The producers are
`AMDFramebuffer` / `AMDRadeonX6000Framebuffer` (`setProperty(key, bytes, 8)`
via `vt[0x288]`, from `this->0x7e0` and
`this->0x800 ? : this->0x850 * this->0x870`); the consumer is `AMDRadeonX6000`.
**2w7 IS CORRECTED**: `configureDisplay` (`0xbe87bd4`) is slot `0x138`'s real
target but **never touches its `_FRAMEBUFFER_INFO*`** - `%rcx` is clobbered at
`0xbe87c34` and its `%rbx` is the `IOAccelResource2`. `getDisplayInfo` is the
filler. Third milestone in a row where the right static chain was attached one
link off; the probe caught it again.
**NEXT, fbkext `59124B3A` + plugin `64D3D687`**: `-rbgpufbaty` publishes both
as 8-byte `OSData`, offset default **16 MB**, tunable `rbgpufbatymb=<1-3072>`.
**The truthful offset is 0 and 0 cannot be used** - the gate rejects it and our
scanout IS at VRAM offset 0 (2pr). **So say the outcome before the boot: if the
chain works the pipe goes ACTIVE and the panel shows a STALE or WRONG image.**
A working PIPE with a wrong IMAGE is the point; SSH and the probe are the
instruments, not the screen. Nothing is written to the display engine, so this
cannot wedge DCN, and one boot-arg reverses it. Pointing the scanout at the
offset needs DCN 3.1.2 primary-surface writes - a separate, narrower step
**once the chain is proven**. `ax*_b35d` added: never probed, now known to be a
gate, and `1` iff `ATY,fb_size` landed.

**BOOT 188: THE PROBE ANSWERS, AND IT REFUTES 2w4/2w5 (2w7).** `rbgpu-boot`
188, armed, both one-shots 0, `diag_used` **613**/768. The capture is
incomplete (six files - the box rebooted ~40 s into `plugin_check.sh`), but
the `ax*` keys exist and **only build `D69D9B33` can produce them**, which
satisfies rule 15 another way. All three samples (25/45/75 s) agree:

    ax_enabled 1   accel->0xc78 & 0x2  SET        ax_vram 1   accel->0x180
    ax_flags   2                                  ax_vmap 1   accel->0x188
    ax_defer   0   pipe->0x282                    ax_fbs  1   machine->0x108
    ax_active  0   pipe->0x298  CLEAR             ax_n/via 1 / 1
    ax_gate    3   "pipe NOT active (0x298)"      sizes 8168 / 408 / 15216

**So the accelerator-enabled bit is ELIMINATED and `pipe->0x298` is the
gate** - the exact opposite of two milestones of deduction. `enabled 1` with
`vram 1` and `vmap 1` is three fields agreeing (the enable is called only
when `0x180 != 0`), so the pointer chain is right. **`getVRAMRange()` and
`find_vram_descriptor` are correct and off the list.**
**WHY THE DEDUCTION FAILED: 2w3 MISCOUNTED THE EXITS.** It said
`init_framebuffer_resource` (`0x14e5d7ca`) has three failure exits all
logging at FAULT. It has **five, and two are silent**:
`fb->vt[0x9f0]()` (`getCurrentDisplayMode`) non-zero at `0x14e5d836`, and
`pipe->vt[0x858](0, resource)` NULL at `0x14e5d9d9`. Three boots of
FAULT-absence were read as success. **Rule 14's sharper form: the absence of
a log is informative only once EVERY exit has been enumerated.**
Ours returns `kIOReturnSuccess` from `getCurrentDisplayMode`
(`RBAMDFramebuffer.cpp:515`), so **the failure is the vendor hook**: call
offset `0x858` = dump `0x868` (rule 7) =
`IOAccelDisplayPipe::initFramebufferResource`, overridden by
**`AMDRadeonX6000_AMDAccelDisplayPipe::initFramebufferResource`**
(`0xbe49356`), which returns 0 silently on any of: `resource` NULL,
`getFramebuffer()` (`pipe->0x98`) NULL, `index != 0`, **`pipe->0x340 == 0`**,
**`pipe->0x348 == 0`**, `pipe->0x35f != 1`, or **`pipe->0x380 == 0`** (the
value it returns). `0x340`/`0x348` are the `_FRAMEBUFFER_INFO` that
`AMDRadeonX6000_AMDHWDisplay::configureDisplay(fbIndex, index, info,
resource)` (its `vt[0x138]`, `0xbe87bd4`) must fill, reached through
`pipe->0x330 = accel->0x1a38->vt[0x2f8]()`.
**NEXT, build `C025F308`**: the same probe, extended to publish those fields
plus the base's own progress markers - `0x284` fb index, `0x28c` format,
`0x290` width, `0x294` height, `0x296` bytes/pixel. **If `w/h/bpp/fmt` read
1920 / 1080 / 4 / 4 then `getPixelInformation` succeeded and the failure is
unambiguously inside AMD's hook**, and `fi340` / `fi348` / `r380` / `b35e` /
`b35f` say which of its returns fired. One boot.

**BOOT 186: THE PROBE NEVER RAN, AND THE TWO ELIMINATIONS RECONFIRMED
(2w5).** Armed, `rbgpu-boot` 186, both one-shots 0, `diag_used` 564/768,
`-rbgpuaccelprobe` in boot-args, build `D8B1115C` loaded - **and not one
`ax*` key**, not even the unconditional `ax_say`. The fault is the **call
site**: 2w3 put `rbgpu_accelprobe_start()` beside `rbgpu_heartbeat_start()`
in `kern_start.cpp` because a grep found that name, and never read what gates
it - `if (stage >= 40)` **and** `if (checkKernelArgument("-rbgpubeat"))`, and
`-rbgpubeat` has not been in boot-args since 2fg. Rule 16 on a probe's own
call site. Moved into `RembrandtGPUPeer.cpp` right after
`rbgpu_diag_set_num("armed", ...)`, which runs in the peer's `start()` every
boot; build **`D69D9B33`**.
**But the boot was NOT wasted.** 2w4's sender predicate works and turns 186
into an independent confirmation: **801 kernel lines**, **26
`display_change_handler`** (the kernel-half control), 494 `[INFO]`, 152
`Failed to submit transaction` **all `IOReturn: 0x2d8`**, 75 `Fallback`,
46/46 `event timeout`/`not hung`, 4 `bad bits`, and
**`init_framebuffer_resource` 0** - so `pipe->0x298` is set, captured
directly this time instead of re-queried. The 13-notification sequence is
**identical to 185's**, and the last `event 93` (WSAADidExitDefer, which
zeroes `pipe->0x282` unconditionally) is at 22:58:51.552 while the last
submit failure is at **23:02:06.182 - 194 seconds later** (2w4 had 51).
**So `accel->0xc78 & 0x2` is the gate in two boots.** New shape seen:
`hardwareErrorEvent(): GPURestartDeferred`.

**THE KERNEL HALF OF OUR OWN CAPTURE WAS ONE FUNCTION NAME (2w4).** No boot.
`dl_pred`'s only kernel clause was `eventMessage CONTAINS "checkGPUProgress"`
- a literal - so **every other** IOAcceleratorFamily2 / AMDRadeonX6000 message
was filtered out, and **2w3's "no `init_framebuffer_resource` FAULT" argument
had no control**. Re-querying boot 185's window by **sender** recovered **213
lines** the capture never had (FAULT persists across a reboot; INFO does not).
Saved as `logs/20261027-185657-plugin/kernel-accel-log.txt`.
**`IOAccelDisplayPipe::display_change_handler` logs EVERY framebuffer
notification at FAULT with its `kIOFBNotify*` number**, and calls
`find_vram_descriptor()` unconditionally before dispatching. Boot 185 got
**13**: `1`/`2` (ModeWill/DidChange) x3, `40` OnlineChange, `91`/`93`
(WSAAWillExitDefer / DidExitDefer), `90`/`92` (WSAAWill/DidEnterDefer),
`91`/`93` again at 18:56:05.543. So our notification delivery works and
`found_framebuffer` ran.
**TWO OF THE THREE GATES ARE ELIMINATED.** `wsaaDidExitDefer` (`0x14e5dfe6`)
zeroes `pipe->0x282` **unconditionally**; event 93 last fired at
**18:56:05.543** and the failures run 18:56:05.516-18:56:07 **and again at
18:56:39 and 18:56:55** - so the defer gate is open. And with 213
IOAcceleratorFamily2 FAULTs from the same thread as a real control,
`init_framebuffer_resource` logged nothing, so `pipe->0x298` is set.
**By elimination the gate is `accel->0xc78 & 0x2`, the accelerator-enabled
bit**, whose skip condition is `accel->0x180 == 0`. Also named: the vendor
gate above it, `callq *0x870` = dump 0x880 (rule 7) =
`AMDRadeonX6000_AMDAccelDisplayMachine::displayModeDidChange()` (`0xbe48428`),
which returns `set_display_mode_and_vram()` - true at least once, since the
pipe loop ran.
**AND THE WEDGE IS NAMED.** From **18:56:11.410**, six seconds AFTER the
first submit failure, repeating every ~5-6 s to 18:56:50:
`channel 52 GFX event timeout; pending stamps [1..289]`,
`channel 12 / 53 SDMA0_PAGE event timeout; pending stamps [1..191]/[1..184]`,
`channel N is not hung, keep running` (x18),
`checkGPUProgress - Signaling hardware error on channel 12/25/52/53` (x22),
`GPURestartSignaled`/`Enqueued` (x22 each), `restart_channel: GPURestartSkipped`
and **`no channel associated with stamp_idx N`** (x18). Work is submitted and
**never retires**; the hang check says not hung; the restart cannot find the
channel. That is a **stamp-retirement** failure and it starts AFTER the
display failures, so it is a consequence or an independent second defect -
**not** established which. Also new: **`IOAccelSurface::set_id_mode: Surface
mode contains bad bits`** x4 at 18:56:05.034, unexplained.
`tools/plugin_check.sh` now matches by sender and prints a kernel-line count
with a `display_change_handler` control.

**BOOT 185: THE ERROR IS NAMED, AND IT IS THREE BITS IN APPLE'S KERNEL
(2w3).** 2w2's `--info` fix worked: **388 `[INFO]` lines** where boots 177-184
had zero (controls: 203 `[ERROR]`, 139 `[WARN]`), and the message that had
been invisible for eight boots is there **118** times -
**`Failed to submit transaction: IOPRESENTMENT_METHOD_FAILURE: IOReturn:
0x2d8`**. `0x2d8` is **`kIOReturnNotReady`** (IOReturn.h:126 on this
machine). CoreDisplay's encoder packs `(ioreturn_low10 << 22) | 0x8f`, so the
whole error is one kernel return code.
Arm verified first (rule 34): `rbgpu-boot` 185, `agdc_oneshot 0`,
`iop_oneshot 0`, `agdc_error 0`, `agdc_ec_scaler` **0x196C_00_01_00** again,
`diag_used` **564** of 768. `Fallback ... submission failed` **58** (so
Submit runs ~twice per Commit), `Failed to create IOPresentment transaction`
**0** - 2w2's reading of `IOPresentmentBegin` confirmed in a capture that
now carries the level it would have been logged at.
**IOPresentment ITSELF is talking**, newly visible at INFO:
*"Valid ADGC node found for this device"*, *"AGDC CSC / Gamma /
Linearization / Plane Scaler / Cursor successful"* (x3 each, three gathers),
*"Capabilities conversion successful"*, *"Successfully created presentment
object type: 1024"*. **2w1's elimination of the whole AGDC reply surface is
now confirmed by the consumer in words** - and before each failure it echoes
our degamma entry back intact (`type 0x1 cid 0x20 min 0 max 1 num 1`).
**The failing call is `IOAccelDisplayPipeUserClient2::transactionEnd`**
(IOAcceleratorFamily2 `0x14e64620`), the only `kIOReturnNotReady` in the
display-pipe path. `IOPresentmentEnd` is a trampoline (validate, tail-jump
`obj->0xa0` vtable slot `0x80`), so the code is the kernel's. Exactly three
conditions produce it:

| gate | meaning | writer |
|---|---|---|
| `accel->0xc78 & 0x2` clear | accelerator not enabled | `enableAccelerator()` only |
| `pipe->0x282 != 0` | WSAA defer in progress | `wsaaWill/DidEnter/ExitDefer` |
| `pipe->0x298 == 0` | `IOAccelDisplayPipe::isActive()` false | `init_framebuffer_resource` only |

**`0x298` is almost certainly SET**: `init_framebuffer_resource`
(`0x14e5d7ca`) is called **unconditionally** by
`IOAccelDisplayPipe::displayModeDidChange` (`0x14e5db14`), which is the
**only site in the whole kext** that registers `'vbl '` - and boot 183
measured that registration on our framebuffer. All three of its failure
exits log at os_log type **0x11 = FAULT** and none of them appears -
**but 2w3's control for that was invalid** and 2w4 had to re-establish it by
capturing by sender; read 2w4, not this sentence.
**The prime suspect is `accel->0xc78` bit 1.** `IOGraphicsAccelerator2::start`
clears it; the sole setter is `enableAccelerator()`, called only from
`IOAccelDisplayMachine::display_mode_did_change` (`0x14e2148e`) - and that
function **skips the enable but still runs the per-pipe
`displayModeDidChange` loop** when **`accel->0x180 == 0`**. That one branch
fits every measurement: VBL registered, framebuffer resource inited,
accelerator never enabled, `transactionEnd` answering NotReady 118 times.
**`accel->0x180` is the VRAM descriptor, and it comes from OUR
framebuffer**: `find_vram_descriptor()` (`0x14e53922`, called from
`found_framebuffer` and `display_change_handler`) does `fb->vt[0x9b0]()` =
**`IOFramebuffer::getVRAMRange()`** (2pq's slot), stores it at `0x180`, maps
it (`vt[0x200](1)` -> `0x188`, `vt[0x118]()` -> `0x190`) and **on a failed
map releases it and writes `0x180 = 0`**.
`RBAMDFramebuffer::getVRAMRange()` returns the retained aperture, so the
suspect is the **map** - or `found_framebuffer` never running for us.
Offsets are from the accessors, not guessed: `machine+0x108` count
(`getFramebufferCount`), `machine+0x88+8*i` pipe (`getDisplayPipe`),
`pipe+0x298` (`isActive`), `accel+0x378` machine (`find_vram_descriptor`).
**So the defect is in NONE of our replies** - not AGDC, not the plane count,
not gamma, not a capability type. It is one enable bit.
**NEXT IS A READ, build `D69D9B33`** (2w5 moved its call site): `-rbgpuaccelprobe` schedules three
read-only samples (25/45/75 s) that find the accelerator by class, check
`getClassSize()` before touching an offset (rule 19, and refuse loudly),
and publish `ax<i>_gate` (0 all open, 1 accel not enabled, 2 defer,
3 not active) plus `enabled/active/defer/vram/vmap/fbs`, the numeric keys
first in the hot list because the boot wedges and the note truncates.

**`Submit()` IS READ IN FULL, AND OUR CAPTURE HAS BEEN DROPPING THE CAUSE FOR
EIGHT BOOTS (2w2).** No boot. `IOPTransaction::Submit` (`0x7ff8040c8a56`) has
one epilogue returning `%ebx` and three exits: success
(`Submitted IOP Transaction`), **`Failed to create IOPresentment
transaction`** at `0x7ff8040c8dde` when `IOPresentmentBegin` returns NULL, and
**`Failed to submit transaction: %s`** at `0x7ff8040c97e9` when the final
call, **`IOPresentmentEnd`** (`0x7ff812e89211`), returns non-zero.
CoreDisplay's three logging wrappers differ only in a level and a tag:
`0x7ff80405f052` = 0x10 **`[ERROR]`**, `0x7ff80405ec06` = 0x2 `[WARN]`,
`0x7ff80405ed79` = **0x1 `[INFO]`**.
**So `IOPresentmentBegin` SUCCEEDED in boot 184** — its failure is
`[ERROR]`-level, the same wrapper as the `Fallback …` line that appears 56
times, and it is absent from a capture carrying 197 `[ERROR]` and 134
`[WARN]` CoreDisplay lines. **2v9 is refuted a second, independent way**, and
the plane count is fine.
**The failure is `IOPresentmentEnd`, and its error string has never been
captured**: `plugin_check.sh` ran `log show` **without `--info`**, so every
`OS_LOG_TYPE_INFO` line was dropped — and that is exactly where the one
message naming the error lives. Boots 177-184 captured the *consequence*
(`Fallback … submission failed`, ERROR, from `Commit` after Submit returns
false) and never the *cause*. **It cannot be recovered afterwards** — INFO
messages do not persist across a reboot; re-querying boot 184 with
`--info --debug` gives 2290 lines, the 56-line control, **3** `[INFO]` total
and none of Submit's. Fixed in `tools/plugin_check.sh` (`--info`, plus an
`[INFO]`-count control). **No new build needed — the next armed boot answers
it.**

**BOOT 184: THE WHOLE AGDC REPLY SURFACE IS ELIMINATED BY MEASUREMENT, AND
2v9 IS REFUTED (2w1).** Armed, wedged, captured. All four one-shots consumed
(`agdc_oneshot 0`, `iop_oneshot 0`), `rbgpu-boot` 184, `diag_used` **565 of
768** so absences are informative. **`agdc_ec_scaler = 0x196C_00_01_00`** —
out-length `0x196c`, framebuffer index `0`, count `1`, return `0`: *exactly*
the shape 2v9 said the chain requires. The other four match
(`degamma` `...0200`, two entries). `agdc_extcap_calls` **48** = 16 types x
**three** gathers, `agdc_extcap_answered` 15, `agdc_error 0`,
`agdc_link_result 0`, `agdc_user_vendor 4`.
**And the display log is shape-identical to boot 182's** — normalised and
diffed, the only difference in either direction is a mach **port number**
(51715 vs 50947). `Fallback ... submission failed` **56**, `no gamma` **0**,
`no display pipes are present` 0, and **`fb_vbl_ot_armed` absent from the
FULL table**, not just the truncated note.
So **2v9's "a plane count of 0 is exactly and only what makes
`IOPresentmentBegin` return NULL" is REFUTED**: every condition it named is
satisfied three times over and `Submit()` still fails. Its *reading* of the
chain stands — every link was disassembled — but the inference that **our
reply** was the broken link does not. Fourth time a correct static chain has
been attached to the wrong end. **Every AGDC capability type is now
eliminated by measurement** (1/4/0x8000 by 2sd, 2 by 2v6, 0x10 by 2w1):
spend no further boot on that interface.
The defect is **strictly past our replies** — either `Submit()` failing at
`IOPresentmentCreateResource` / `BindIOSurfaceToResourcePlane` /
`SetPlaneState` (2se's "Plane out of range"), or a capabilities object that
is not the one `__GatherAGDCDisplayPipelineCapabilities` fills.
Also found: **`pipe->0x290`, the flag choosing the hardware pipe over the GPU
fallback, is a transaction-carried `std::optional`** (`txn+0x180` value,
`txn+0x181` flag, `0x7ff804116eba`) — the same shape 2v8 found for System
Gamma, so that choice is CoreDisplay's own and not ours to set.
**No instrument currently reaches the remaining failure**: it is userspace
inside WindowServer, the `sample` is forbidden (2rh), `ioptest` aborts
without the entitlement (2qm), and our kext sees only the AGDC surface, which
is provably correct.

**BOOT 183: NO WEDGE, AND THE VBL HALF IS ELIMINATED (2w0).** Build
`8887186E` booted clean and carried **no measurement** — 2v9's handoff named
only two of the four one-shots, so `agdc_oneshot=2` and `iop_oneshot=2` ("not
armed") and every `agdc_ec_*` key is absent with `diag_used` 479 of 768, so
that absence means the bridge never ran. What boot 183 **is** worth:
**a clean no-wedge differential** — 177-182 all had the AGDC bridge armed and
all wedged; 183 is the same build family and boot-args with the bridge
**unarmed**, framebuffer and accelerator up, and the desktop stayed usable for
minutes. n=1, but it points at the bridge, not the framebuffer takeover.
**And the VBL side is now positively correct**, from the registry (rule 31):
`RBFBVBLRunning Yes`, `RBFBVBLFires 54000` in ~375 s = **144 Hz** (the panel's
measured 143.88), handler 0 ours and enabled, **handler 1 Apple's, registered
and left disabled with 0 fires** — exactly 2s5's prediction. So the clock and
the registration are right and `fb_vbl_ot_armed` is still absent: **no
transaction has ever been queued, and the VBL half is no longer a candidate
for why.** Also live: `IOAccelDisplayPipeCapabilities` with both keys `Yes`
(**2u2 reconfirmed**, 2sv's zero is the outlier twice over), `VRAM,totalMB`
4096, SkyLight logging `Metal compositor`.
**Unexplained and NOT attributed**: WindowServer at **200.2 %**, against
2su's 0.0 % idle — but the user was driving the machine and 2v9's *unarmed*
boot read 207.9 %, so nothing separates CPU compositing from a busy desktop.
Needs its own controlled read.
**Arm ALL FOUR**: `sudo nvram rbgpu-arm=1 rbgpu-agdc=1 rbgpu-iop=1
rbgpu-fb=1`.

**`Clean` NEVER APPEARED BECAUSE THE PRINTER CANNOT PRINT IT (2v8).** That
byte prints `"Dirty "` or six spaces; the string `Clean` is not in
CoreDisplay. Controlled against the capture: `DisplayPipeState` 66, **`Dirty`
528** (= 66 x 8), `Bypass` 0, `Clean` 0. The claim "`Clean` has never
appeared", carried since 2sa, is **vacuous** — rule 26, and the most expensive
instance yet. The real content is the opposite: **all eight operations are
Dirty in all 66 dumps and none is Bypass.** Remove `Clean` from every
open-question list.

The paragraph below is kept for its disassembly, which stands; its conclusion
does not. (2sw/2sx superseded by 2sy, 2sy superseded by 2u0.)
`populateAccelConfig` derefs `accel+0x368` with **no NULL check** after casting
to `ATIController`, and the journal already recorded that `ATIController`
**never instantiates here** (line 34452) because `0x1681` matches no controller
personality (`RembrandtGPUPeer.cpp:577`). No boot panicked there, so
**`populateAccelConfig` never runs** - 2sw's gate A and 2sx's gate B were both
branches inside a function that does not execute. The only route to a
controller is `-rbgpufbmatch`, which **kills the accelerator**, so the path is
closed, not gated. **That confirms 2sj from this machine: skip IOPresentment.**
So the fix is local to our kext and is in build **770AE129**:
`RBAMDFramebufferVBL.cpp` now registers `'vbl '` **enabled**, because Apple's
enable comes from `enableTransactionInterrupt` off a queue that can never fill
here (2s5/2s9) - which is why WindowServer sat idle at 0.0 %. Next armed boot
reads `RBFBVBLFires` / `RBFBVBLRegistered` / `RBFBVBLRunning` from `ioreg`
(rule 31 - our `IOLog` is invisible).

**GATE 1 IS WON, AND THE LATCH IS WHY IT DOES NOT SHOW** (2t7, boot 174, no
boot spent). `ioreg` finds **exactly one** IOFramebuffer and it is ours, so no
unclassifiable framebuffer contributes bit `0x01` any more: the vendor mask
reads **0x04** and `UseIOPresentment()` returns **true, unpatched** - proven by
boot 174 logging *"System set to use IOP but no display pipes are present"*,
which sits past that call's own `testb/je`. 2qp needed a classifier patch for
that line; we get it from the class name. **`gUseIOP` is a one-way LATCH**, so
`CGXMappedDisplayStart` finds the `DisplayPipe` map empty, writes 0, and all 72
later call sites - `GetNextSurface` among them - see false. That dissolves
2t6's addendum, which is **withdrawn** along with both candidates it named: the
preference read was corroborated (`useiop` via CFPreferences, and boot 149's
os_state dump), and ordering was never the risk because `UseIOPresentment()`
runs `GetSystemGPUVendors`' `dispatch_once` itself. `0x7ff8438eac90` is the
entry count of CoreDisplay's `unordered_map<unsigned long long,
CoreDisplay::DisplayPipe>`. The remaining link is **2r5's empty AGDC
association map**, unchanged, and **our bridge was UNARMED in boot 174**
(`agdc_say = "UNARMED: no class or service created"`, `agdc_oneshot 2`, zero
`RembrandtGPUAGDC` instances), so `agdcinfo`'s `candidates 0` is **not** a
verdict on it. Next experiment needs **no new code**: an armed boot with
`-rbgpuagdc` *and* its one-shot, then `build/out/agdcinfo` over SSH. Timing is
tight - boot 174 latched at **+6.5 s**, 2rw measured the bridge at **<= +5128
ms** - and **`-rbgpuagdcearly` still stays OUT**; if the race is lost, make the
bridge earlier, not the gate.

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
  `sudo tools/fbload.sh` — **never installed by Claude**. 2sm.
- `src/plugin/` — the Lilu plugin: `kern_start.cpp` (hooks and probes),
  `RembrandtGPUPeer.cpp` (the IOService peer, the diag table and the NVRAM
  hot-key list), `rbgpu_agdc.cpp` (the AGDC bridge), `rbgpu_agdc_otg.cpp` (the
  read-only BAR5 OTG sampler). Boots from the EFI.
- `tools/earlybench.sh` + `tools/com.rembrandtgpu.earlybench.plist` — a
  `RunAtLoad` daemon that benches the GPU in the +0..+20 s window no terminal
  can reach, self-stopping at +180 s, logging seconds-since-boot to
  `/var/log/rbgpu-earlybench.log`. A **120x discriminator, not a rate
  measurement** (`--bench 2`); ONE queue, never `--queues`, which is a known
  wedge trigger. **The user installs it** (`/usr/local/libexec`,
  `/Library/LaunchDaemons`); Claude never does. 2wy/2wz.
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
  **Claude never runs these and never asks for the password.**
- **Never install into the EFI or `/Library/Extensions`.** The framebuffer
  kext's third stage needs the auxiliary collection, which is an install and so
  is the user's own step; Claude builds it and hands over the commands.
- The user runs `sudo tools/fbload.sh [beside]`. Arming needs **both**
  `-rbgpuownfb` in boot-args **and** `sudo nvram rbgpu-fb=1` (2sr). The boot-arg
  is the durable disarm. **The one-shot DID NOT consume until 2wj** —
  `rbgpu-fb` survived a reboot after being removed (2sq), because
  `IODTNVRAM::removePropertyWithGUIDAndName` calls no sync at all, so the
  takeover was effectively sticky for six boots and boot 196 had to be
  disarmed from **Windows**. 2wj fixes it by **overwriting the key with a
  literal `"0"` instead of removing it**, on the `setProperty` path 2sr
  measured as the only one that syncs, so a plain power cycle is now a full
  disarm. Between experiments still take the boot-arg out with
  `sudo tools/efi.sh bootarg remove -rbgpuownfb`, exactly as rule 9 says.
- **Never run `kmutil unload` on the framebuffer kext — it PANICS** (2sq).
  `IOFramebuffer::stop+0x48` reads `*(this->0x1c8)`, which is NULL on an
  instance that never completed configuration. To remove the kext, **reboot**.
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
    `config.plist`. **And check it for EVERY kext the boot loads, not just the
    one that changed**: each artifact has its own install path and a rebuild
    refreshes none of them. The plugin comes from the **OpenCore EFI**
    (`sudo tools/efi.sh install`, 2w6) and the framebuffer that owns the panel
    comes from the **auxiliary collection**, i.e. from
    `/Library/Extensions/RBAMDFramebuffer.kext` (2wa) - `fbload.sh` is not that
    path, it stages a fresh copy under `/private/var/tmp` for stages 1 and 2
    only. Boots 187 and 190 were each lost to one of the two. **An arm
    instruction must name the install for every artifact it depends on**, and a
    key the new code publishes *unconditionally* is the cheapest independent
    check that the new binary is the one running. [2cu, 2cv, 2w6, 2wa]

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
31. **`IOLog` is NOT an instrument in `src/fbkext` — publish to the registry.**
    32 554 kernel-sender lines reached the unified log in one boot and **not one
    was ours**, while `VRAM,totalMB` and `model` in `ioreg` proved `start()`
    had run. Why is unmeasured. So an fbkext diagnostic that only logs is
    invisible, and the absence of its line says nothing: `setProperty` it and
    read it with `ioreg -rl -c RBAMDFramebuffer`. [2sv]
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
    "failed" under a perfectly good report (2so); and the **mirror** of that, a
    loud specific error sitting on top of a write that LANDED - `kmutil create
    -n aux` errored `Code=34 Missing Developer Kit` while the AuxKC was
    rebuilt correctly, proven by searching the collection for the raw UUID
    bytes with four controls (2wa). **Verify the artifact, never the exit
    status, in both directions**; **`timeout` does not exist
    on macOS**, so three `ioreg` reads came back as `timeout: command not
    found` and looked exactly like "the service is absent" (2v9); **a grep for
    a word the producing code never contains returns 0 forever** — `Clean` was counted
    across five boots and six milestones while CoreDisplay's printer only ever
    writes `Dirty` or spaces (2v8, the most expensive instance); **a numeric
    grep for a constant cannot see a constant the assembler built by
    ARITHMETIC** - `kIOReturnUnsupported` reached `executeTransaction`'s
    return as `leal (%rax,%rax,4); addl $0xe00002c2`, and five literal sites
    were enumerated while the live one was invisible (2wc); **a `sort -u` is
    not a count** - de-duplicating the LUT sizes in a capture showed 1024 and
    256 and read as "it sends 1024" when 71 of 73 were 256 (2wc); **a KEY NAME CAN BE A PREFIX OF ANOTHER KEY'S NAME** - a
    flat grep over `nvram.txt` for `rbgpu-diag` matched **`rbgpu-diag-prev`**,
    the PREVIOUS boot's table, and reported `armed=0 agdc_oneshot=2
    agdc_error=3`, which reads exactly like an unarmed boot and nearly threw
    away boot 194 (2wg); anchor the match or read the value, never the file;
    and **when
    xrefing a string, xref the string's START** (find the preceding NUL first)
    — a substring's address can never be referenced.
    [2lw, 2rk, 2r4, 2s0, 2s5, 2sn, 2so, 2sr, 2v8]
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
40. **Boot-arg VALUES: a NUMBER comes back as a BINARY INT, not ASCII.**
    `PE_parse_boot_argn` with `force_string = 0` (what the public entry point
    passes) stores a numeric value as a raw integer of the caller's width -
    `0xffffff8000b92c77` jump-tables on `max_len - 1` for 1..8 and sends
    **max_len > 8 to the same 4-byte `movl`** - and then returns 1. So
    `char buf[24]` plus an ASCII digit loop reads `D0 07 00 00` for `=2000`,
    parses 0, and refuses it; boot 217 ran the scheduler tick at its 6944 us
    default with `rbgpuschedtickus=2000` in boot-args and 2xl's free A/B
    silently did not happen. The correct idiom is
    `int v = 0; PE_parse_boot_argn(name, &v, sizeof(v))`, now in
    `rbgpu_boot_argn_u32()`. The project carried BOTH idioms for twenty
    milestones with no measurement separating them, because **no boot had ever
    passed any of these tunables a value** - a default that reads correctly is
    not evidence that the tunable works. Corollary, rule 31 one layer up:
    **`IOLog` is not an instrument in the PLUGIN either** - the refusal was
    logged and the unified log does not have it. [2xm]
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
36. **A log capture has a LEVEL, a PREDICATE and a WINDOW, and all three can
    silently drop the answer.** `log show` omits `OS_LOG_TYPE_INFO` unless
    `--info` is passed, and INFO messages do not survive a reboot, so they
    must be captured in the armed boot; FAULT ones do survive. 2u5 lost a
    boot to the window; 2w2 lost **eight** to the level; 2w3 nearly lost a
    conclusion to the predicate, whose entire kernel half was the literal
    `eventMessage CONTAINS "checkGPUProgress"` - so 213 IOAcceleratorFamily2
    and AMDRadeonX6000 FAULT lines, including the wedge itself, were never
    captured. **Match a subsystem by SENDER, not by one function name**, and
    count what you need against a known-present control in the same capture.
    [2w2, 2w4]
35. **When a static chain ends at our own code, instrument our end FIRST and
    prove it correct before building on the chain.** 2v9 disassembled five
    layers correctly and attached them to the one link that turned out to be
    fine; `agdc_ec_scaler` settled it in one boot and could have been written
    in the same change as the chain. [2w1]
34. **An arm instruction must list EVERY one-shot the experiment needs, and
    the reader must check each `*_oneshot` key before interpreting anything.**
    2v9's handoff named `rbgpu-arm` and `rbgpu-fb` and omitted `rbgpu-agdc`
    and `rbgpu-iop`; boot 183 came up looking completely healthy and carried
    no data at all. The keys that would have caught it were already in the
    note and already read 2 = not armed. Rule 9 says the one-shots are
    one-shot; this adds **say which ones, and verify them first**. [2w0]
37. **A one-shot that cannot consume itself is not a one-shot, and a feature
    must stay disarmable FROM THE MACHINE IT RUNS ON.** `rbgpu-fb` was
    consumed with `removeProperty`, which reaches no sync, while the crumb in
    the same file used `setProperty`, which does - two facts two screens apart
    for eleven milestones. The takeover therefore re-armed on every power
    cycle, and once 2wh succeeded the desktop ran at 0.09 Hz, so there was no
    usable machine from which to edit boot-args: boot 196 had to be disarmed
    by booting **Windows**. **Consume by OVERWRITE on a path measured to
    persist**, and before arming anything that can take the display, ask what
    the recovery looks like when it works *and* when it half-works. A
    `FORCESYNCNOW` key is not a substitute for using a syncing path. [2sq,
    2wj]
38. **Two halves that are separately armed must still check EACH OTHER.**
    `-rbgpuownfb`/`rbgpu-fb` arm the framebuffer and `-rbgpuarm`/`rbgpu-arm`
    arm the accelerator, deliberately separately so the halves stay testable
    alone. But the framebuffer writes HUBP from `setAttribute('amtr')` and
    from nothing else, so with the panel taken and no accelerator underneath,
    the screen freezes on whatever was last drawn - **guaranteed**, not
    merely possible. It happened after boot 197: fb armed, plugin did not,
    `amtrSets 0` at 70 s, and a machine that ran healthily for **ten minutes**
    sat behind a frozen progress bar through three power cycles. Rule 9 was
    satisfied by each half and the PAIR was never checked. 2wp adds a
    no-transaction deadline that consumes `rbgpu-fb` for the next boot, so the
    failure costs one power cycle instead of unbounded ones. **Before arming
    anything that takes the display, name what it depends on and make it
    refuse, or self-disarm, when that thing is absent.** [2wp]
39. **A measurement of a defect, taken in a configuration that does not
    exhibit the defect, does not locate the defect.** Boot 203 measured the
    wedge collapsing GPU throughput 265x - in an `rbgpu-fb=0` boot whose
    desktop was a healthy, usable ~25 fps throughout, because a CPU-composited
    desktop never submits GPU work and cannot see a GPU wedge. 2wz promoted
    that to "the frontier is the wedge"; the user corrected it in one line, and
    2wl's typing observation had already eliminated the wedge as the GPU-FB
    blocker. **Say which configuration a reading comes from, and whether that
    configuration has the symptom, before promoting it to a frontier.** Rule
    33's shape - name the object a number belongs to - applied to a whole
    experiment instead of a field. **Corollary**: an instrument that needs its
    own load (2wy's `a=`/`u=` read an idle GPU) must publish the load beside
    the result, or a dead window reads as a slow one. [2wy, 2wz, 2x0]
33. **Before reading anything from the LIVE machine, establish which boot it
    is**: `kern.boottime` against the capture's, and `rbgpu-boot` against the
    last nonce. The running box is not the armed boot merely because the armed
    boot's capture is the newest on disk — 2v9 read a whole graphics stack as
    "torn down during the wedge" when it was simply an unarmed boot two
    minutes later. **And an index is not a count**: 2u4 retired the
    plane-count line using `Plane Index: 0`, which is CoreDisplay's index of
    the first plane, not IOPresentment's count. Name the object a number
    belongs to before retiring a line with it. **Third instance, and it cost a
    boot**: 2wc retired the post-gamma ceiling using the `LUT` line in
    `DisplayPipeState`, which is the dump's **per-plane** gamma under
    `Plane Index: 0`, while the gate is on the **per-pipe** post-gamma that
    those dumps never print (2wd). [2v9, 2wd]
32. **Read a `char*[]` table's LENGTH table too, and check the slot count
    against the bound in the code.** Cache string tables are concatenated with
    no separators, so names read without lengths run together and an
    off-by-one slot looks plausible. 2v7 took three names off the target table
    one slot high, concluded `Unknown` was the printer's default for an
    out-of-range value, and pointed the next step at the wrong question; the
    bound sat in the instruction above the LEA all along (`cmpq $0x3, %rax`),
    and the real table has four entries with `Unknown` at index 0. Corollary:
    **an enum value of 0 is a value, but an unwritten field is an absence that
    looks exactly like it** — only the writer tells them apart. [2v8]
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
(the AGDC bridge), `-rbgpuaccelprobe` (2w3 - three read-only samples of the
three bits behind `kIOReturnNotReady`; reads only, refuses on a class too
small for the offsets), `-rbgpuschedtick` (2xg - a 143.83 Hz
`thread_call` calling Apple's `AMDSWScheduler::checkTimestamps()`, the sole
hardware stamp poll and the only thing that sets the `ctx->0x71` flag
`advance()` needs. **Its failure mode is a PANIC**, so it needs
`sudo nvram rbgpu-tick=1` as well, consumed by OVERWRITE before the first tick
arms - rule 37. It turns `-rbgpuschedprobe`'s routes on by itself, because the
scheduler pointer it calls arrives only as those hooks' first argument. Read
`tarm`/`tc`/`tk`/`tr` in `rbgpu-sched` BEFORE anything else in the boot;
`tunable rbgpuschedtickus=<500..1000000>` - **which never worked until 2xm**:
boot 217 had it set to 2000 and ticked at 6944 anyway, because the value came
back as a binary int and was parsed as ASCII. Rule 40. `tick_us_refused` now
says when one is rejected. **2xn ran it and 2000 us is WORSE**: `advance()`'s
gaps tightened as the theory predicts (`gmean` 2712 -> 1538, `gmax` 54191 ->
13183, the dominant histogram bucket straddling the new period) but `sets/s`
went **71.97 -> 51.33** and our own vblank cadence **141.377 -> 133.878 Hz**,
because a 3.5x faster `thread_call` competes with the VBL one that delivers
frames. **Two variables differed** (`-rbgpufbflippend` came on in the same
boot), so this is not attributed to the period alone - but **6944 is the better
value on the evidence in hand**, and the clean A/B is one token either way), `-rbgpufbaty` (2w9 - publishes
`ATY,fb_offset` and `ATY,fb_size`, the two properties `AMDHWDisplay` reads off
the framebuffer; **the offset it publishes is deliberately not the truthful 0**,
so the composited image lands where the scanout is not reading - expect a wrong
picture with an active pipe), `-rbgpufbamtr` (2wc/2wf - answers the
`'amtr'` framebuffer attribute, which is the gate on every display-pipe
transaction, and snapshots the first transaction struct AMD hands us **plus
the whole 0x220-byte `_AMD_PIPE_FLIP` it points at**, which already carries
the GPU address of the surface the compositor rendered into;
answered, NOT executed, so the picture stays wrong), `-rbgpufbhubp` (2wh - **the first write to the display
engine**: HUBP0's primary surface address, taken from each transaction's
`_AMD_PIPE_FLIP` and written per flip. Requires `-rbgpufbamtr`. Three gates
before any write and six per address, all in `src/common/rb_hubp.h` and all
run by the hosttest; restores the firmware's address on stop. **This one can
blank the panel** - a wrong value shows garbage until the next correct write,
and rule 12 says a black panel is indistinguishable from a hang, so keep SSH
and read `ioreg`, not the screen. **2wi: it WORKED** - the panel showed the
composited accelerated desktop, and the swizzle already agreed. It has **no
one-shot of its own**, so with `rbgpu-fb` failing to clear (2sq) it re-arms
every power cycle; the durable disarm is removing the boot-arg - and
**2wj made `rbgpu-fb` consume properly, so a plain power cycle now disarms the
whole chain and the desktop comes back usable**),
`-rbgpufbflipall` (2wm - drops 2wh's flag-word bit `0x1` gate so a content
update into an UNCHANGED surface is written too. Boot 196's asymmetry - adding
a character updated the panel, deleting one did not - is exactly what being
handed such transactions and refusing them looks like, because adding forces a
new surface and deleting reuses the allocation. **Not established**: boot 195
wrote all 4 of its 4 transactions, but that is an idle desktop and the typing
burst was never measured. The decision is `rb_hubp_should_write()` in
`src/common/rb_hubp.h` with eight hosttest assertions; `RBFBAmtrNoFlip` / the
crumb's `n=` says in the SAME boot whether the arg had anything to do, so the
arg and its instrument are independent and one boot settles it. Rule 30: NOT
`-rbgpufbhubpall`, which `-rbgpufbhubp` would prefix),
`-rbgpufbflippend` (2xm - **the tearing fix**, requires `-rbgpufbhubp`.
Boot 217 read `RBFBHubpPendHits` **593** of `RBFBHubpPendChecks` **33877** =
**1.75 % of flips, 1.26 a second**, programmed over a flip DCN had armed and
not yet latched - which with 2xk's measured `SURFACE_FLIP_TYPE = 0` (latched
at VUPDATE) replaces the armed address and changes the frame partway down the
panel. The decision is `rb_hubp_flip_would_discard()` in
`src/common/rb_hubp.h`, mutation-tested two ways, and it defers only when
`SURFACE_TRIPLE_BUFFER_ENABLE` is clear - one pending slot - which boots 216
and 217 both measured (`FLIP_CONTROL2 = 0x40`). **A deferral DROPS that
frame**: the previous address latches, and `setAttribute('amtr')` already
answers success instantly (2we) so the compositor never waits on us. One
stale frame against one torn frame. **NOT established that 1.75 % accounts
for all the tearing** - the user's "appears for a second" is far longer than
one frame at 144 Hz, and 2xk's second candidate (a surface recycled while
still being scanned) is untested. Publishes `RBFBHubpDeferArmed` /
`RBFBHubpDeferrals`; one token reverses it without losing 2wh's write),
`-rbgpufbnotready` (2xn - **the flicker fix that matches its SHAPE**, requires
`-rbgpufbhubp`. Boot 219 ran `-rbgpufbflippend` and the user reported the
flicker ~30 % reduced, still present, and **"across the whole screen"** - and a
flip programmed over an unlatched one can only corrupt the frame BELOW the
scanline, so 2xm is a real mechanism and not the main one. The main one is ours
since 2wc: Apple's `AMDFramebuffer::getAttribute('amtr')` (`0x2a6faa`) returns
**`kIOReturnNotReady` while `getOutstandingDisplayPipeTransactions()`**, and
ours returns success **unconditionally** - so the compositor is told every
transaction completed instantly and is free to draw into a surface that is
still the live scanout, which corrupts the WHOLE frame. Answering NotReady does
**not** close the gate 2wc opened: `getDisplayInfo` at `0xbe8717e` sets bit
0x100 on `r == 0 || r == kIOReturnNotReady`, measured. Our stand-in for
"outstanding" is DCN's `SURFACE_FLIP_PENDING`, and the triple-buffer guard
deliberately does NOT apply (our address is unlatched either way).
**THE HOLD IS BOUNDED AND THAT IS THE SAFETY PROPERTY**: 2x6 read
`isTransactionComplete` (`0xbe4a794`) deciding by `getAttribute('amtr',NULL) ==
0`, so NotReady makes Apple's accelerator **WAIT** - today's unconditional 0
cannot stall anything and this can. `rb_hubp_hold_notready()` expires after two
vblank periods and a deadline never set reads 0 = already expired, so the arm
fails safe; **removing the bound FAILS the hosttest**. Read `RBFBAmtrHeld`
(non-zero = firing), `RBFBAmtrHoldExpired` (large vs Held = the bound is
deciding, not the hardware - re-read the period, do not raise it blindly), and
`Held` zero with `RBFBAmtrNotReadyArmed` true **eliminates** this candidate.
Risk is a stuttering or frozen desktop; one token reverses it),
`-rbgpufbretire` (2wi - re-enables the `'vbl '` slots Apple registered and
then disabled, once a transaction has arrived, on the suspicion that the
accelerator never learns a vblank happened and so never retires a transaction.
Requires `-rbgpufbamtr`-era transactions to be flowing to do anything),
`-rbgpuflipprobe` (already answered, writes nothing), `-rbgpuagdccaps`
(answers AGDC extended-capability types 1 CSC, 2 Gamma, 4 Degamma and 0x8000
Cursor as well as 0x10 Scaler — off by default because three of the four carry
a substituted field, 2sc).
`-rbgpumcnew` / `-rbgpumcreg` are the runtime-metaclass experiment: `mcreg` is
the user's decision because its failure mode is a **hang**, not a panic.

**Tunables:** `rbgpuvblhz=<millihertz>` (vblank rate, refused outside
20000-400000), `rbgpufbvrammb=<N>` (the published VRAM size, refused outside
256-16384), `rbgpufbvramrangemb=<N>` (8-256, default 64),
`rbgpufbhubpmax=<N>` (bounds 2wh's HUBP writes; absent or 0 = unlimited, which
is what a live display needs - a small N is for a deliberately conservative
first boot),
`rbgpumtlkpatch` (selects the inert cave-only patcher stage),
`-rbgpuagdcpolicy` (leaves `NeedsPolicyDriver` absent, for the 2rk A/B),
`rbprobe_level=N`, `rbprobe_smu=1`.

**Gone, harmless if left in boot-args:** `-rbgpumtllinear`, `-rbgpuhwblt`,
`-rbgpumtlcave`, `-rbgpumtlfix`, `-rbgpulinear`, `-rbgpumtlkfix`. None is read
anywhere; remove when convenient with `sudo tools/efi.sh bootarg remove <arg>`.

**Note for any session run inside the Claude app:** it is launched with
`DYLD_INSERT_LIBRARIES` set, so every shell inherits it and `open -a` passes
it on. Launch anything with `env -u DYLD_INSERT_LIBRARIES` or it starts under
whatever patch this Claude is running — which once looked exactly like a
regression (2ns).

## Rescue

`-rbgpuoff` in boot-args: the kext still loads, no peer instance exists.
**Clear the one-shots before it comes back out.**
