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
  is the durable disarm; **the NVRAM one-shot is NOT** — `rbgpu-fb` survived a
  reboot after being removed and read back absent (2sq), because
  `IODTNVRAM::removePropertyWithGUIDAndName` calls no sync at all. So between
  experiments take the boot-arg out with
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
    writes `Dirty` or spaces (2v8, the most expensive instance); and **when
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
33. **Before reading anything from the LIVE machine, establish which boot it
    is**: `kern.boottime` against the capture's, and `rbgpu-boot` against the
    last nonce. The running box is not the armed boot merely because the armed
    boot's capture is the newest on disk — 2v9 read a whole graphics stack as
    "torn down during the wedge" when it was simply an unarmed boot two
    minutes later. **And an index is not a count**: 2u4 retired the
    plane-count line using `Plane Index: 0`, which is CoreDisplay's index of
    the first plane, not IOPresentment's count. Name the object a number
    belongs to before retiring a line with it. [2v9]
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
small for the offsets), `-rbgpufbaty` (2w9 - publishes
`ATY,fb_offset` and `ATY,fb_size`, the two properties `AMDHWDisplay` reads off
the framebuffer; **the offset it publishes is deliberately not the truthful 0**,
so the composited image lands where the scanout is not reading - expect a wrong
picture with an active pipe), `-rbgpuflipprobe` (already answered, writes nothing), `-rbgpuagdccaps`
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

**Note for any session run inside the Claude app:** it is launched with
`DYLD_INSERT_LIBRARIES` set, so every shell inherits it and `open -a` passes
it on. Launch anything with `env -u DYLD_INSERT_LIBRARIES` or it starts under
whatever patch this Claude is running — which once looked exactly like a
regression (2ns).

## Rescue

`-rbgpuoff` in boot-args: the kext still loads, no peer instance exists.
**Clear the one-shots before it comes back out.**
