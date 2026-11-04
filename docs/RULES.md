# RembrandtGPU — notes for the assistant

> **Read `docs/HANDOFF-NEXT.md` next** — current state and what to do now.
> `docs/JOURNAL.md` (~48 000 lines) is the chronological evidence record:
> authoritative for what was measured, in order, including every withdrawn
> reading, and **never to be trimmed**. Grep it by milestone tag (`2ks`, `2v8`,
> `2xw`); never read it whole.
>
> **This file is a reference card and must stay one.** It was 2 800 lines of
> milestone narrative twice over; both times everything in it was already in
> the journal under its tag. Put findings in the journal. Nothing is ever
> deleted here, only moved — and git history holds every earlier version,
> including the removed `docs/HANDOFF.md` and `docs/HANDOFF-FULL.md`.

Goal: graphics acceleration for the Radeon 680M (Rembrandt, `1002:1681` rev 0a,
GC 10.3.3 / DCN 3.1.2 / PSP+SMU 13.0.x / SDMA 5.2.3) on macOS **Ventura and
up** (2xz: `src/common/rb_osver.h` says what that claims per version, which is
not the same thing for every version). Plan: `docs/PLAN.md`. Machine: ASUS TUF A15 FA507NU (Ryzen 7 7735HS), **macOS 26.7.2
(25G332, Darwin 25.6.0)**, OpenCore. csr-active-config **0xA43** (SIP reports
Filesystem Protections disabled, yet the dyld cryptex path still refuses writes
with EROFS because its files carry the `restricted` flag, enforced
independently of the CSR bits).

## Where this stands

**It works.** An armed boot is an accelerated Mac: Metal compute bit-exact,
System Profiler reads `AMD Radeon 680M` / 4 GB, our own `RBAMDFramebuffer`
owns the panel, Apple's `AMDRadeonX6000` composites for it, SkyLight logs
`Metal compositor activated`, and the desktop runs at ~82 transactions/s with
no flicker and no tearing. USB runs on real interrupts. The remaining asks are
small and named below.

**A "0" our own old builds wrote is not a disarm (2xx).** Two of the five
keys were consumed by *overwrite* before 2xw — the way a one-shot was spent
was to write a literal `"0"` into it — which is exactly the value 2xw's reader
treats as the user's durable disarm. So the first 2xw boot came up with the
scheduler tick off and the panel declined, and the desktop went back to one
frame per 10 s: boot 236's own record reads `tick_armed=0 tick_oneshot=3`. The
value cannot be told from a hand-typed `=0`, so they are told apart by
generation: `rbgpu-gen` is written once, before it exists a `"0"` in
`rbgpu-tick` or `rbgpu-fb` is ignored, after it exists every `"0"` is obeyed.
`rb_key_disarms()` in `rb_defaults.h` is the rule and both kexts ask it, and
`rbgpu_nvram_consume_oneshot_ow` is **deleted** so nothing can write that
value again. The other three keys were consumed by removal and were never
ambiguous. Rule 37, asked the other way round: **what does a stale value our
own earlier build wrote look like to the new reader?**

**Arming is now the default (2xw).** Empty boot-args give the configuration
boot 231 ran: `src/common/rb_defaults.h` holds those 45 tokens and everything
in it is on. The five NVRAM one-shots are gone in that direction —
`nvram rbgpu-arm=0` / `-agdc` / `-iop` / `-tick` / `-fb` are durable disarms
the plugin never writes. The safety net is the **boot sentinel**: `rbgpu-safe`
is set before anything is armed and cleared from a thread_call 60 s later, so a
boot that panics, hangs or is power-cycled inside that minute leaves it set,
and the next boot comes up stock and clears it. One bad boot costs one stock
boot. `-rbgpunosentinel` opts out; a deliberate restart inside the minute also
costs one stock boot.

**Which macOS, and what has been measured on it (2xz).** "Supports Ventura and
up" is two claims and `src/common/rb_osver.h` keeps them apart, because
conflating them is how a wrong address gets written to a chip. **(1)** The kext
LOADS from Darwin 22 up: everything done by symbol name is version-independent
and rule 1 already makes each of those fail safe. **(2)** A version-dependent
artifact has been MEASURED on a given OS - per OS, per artifact, recorded as a
bit, and never inferable from a version number. Sequoia claims the object
offsets (derived there) and a cave row; Tahoe claims both plus the USB table -
the offsets not because they were re-derived but because **boot 247 is an
accelerated desktop running them**. Ventura and Sonoma claim **nothing**, which
is the honest state. On an OS with no `RB_OSD_KEXTOFF` claim `rbgpu_stock()` is
true, so the whole default-on table comes up off and the boot says so with the
Darwin number in it; `-rbgpuosforce` arms it for the boot that goes and
measures. Both kexts read `version_major` and ask the same function (rule 38).
`make osrange` fails the build if the declared Lilu range drifts from the
matrix - it was Monterey..Sequoia on a Tahoe machine, loading only because
`-lilubetaall` is in boot-args. Every boot publishes `os_major`, `os_derived`,
`os_forced`, `os_measured`, and `rbgpu_nvram_armed` why **9** is "no measured
offsets for this OS", distinct from 5's "the user asked for stock".

**The cave is a table, and the generator derives it (2xz).**
`src/plugin/mtlcave/darwin<N>.h` holds one row per OS build;
`rbgpu_mtlcave.h` is the table and the lookup. A row says where to LOOK - what
authorises a write is still the byte-pattern compare in every process, so a row
right for the major and wrong for the point release writes nothing and 2r6's
verdict fires. `make mtlcave` derives this machine's row off its own cache
(symbol, prologue uniqueness, a unique zero-run anchor; the mangled length
prefix computed, rule 5) and names the two edits left by hand. It reproduced
2xt's by-hand derivation byte for byte. `make cavecheck` reads the row for its
own kernel and said CAVE+DETOUR PRESENT on the live boot 247 desktop.

**How the display half got here**, each step measured and in the journal:
`RBAMDFramebuffer` subclasses `IOFramebuffer` and displaces
`IONDRVFramebuffer` (2sl/2sm/2st) — its class name carrying "AMD" is what
opens CoreDisplay's IOPresentment vendor gate (2ql/2t7). `ATY,fb_offset` and
`ATY,fb_size` open `initFramebufferResource` (2w9/2wa). Answering the `'amtr'`
framebuffer attribute is the gate on every display-pipe transaction (2wc/2wd),
and `setAttribute('amtr')` hands us the `_AMD_PIPE_FLIP` carrying the
compositor's surface address (2wf/2wg). Writing that address to HUBP0's
primary-surface pair is what puts the composited desktop on the panel
(2wh/2wi). `-rbgpuschedtick` ticking Apple's `AMDSWScheduler::checkTimestamps()`
at the panel's 6944 µs is what makes frames retire (2xe/2xi) — 2 transactions
in 30 s became 3375. Answering `kIOReturnNotReady` while our flip is unlatched
is what fixed the flicker, at 27 % of the frame rate (2xo) - **the cost is
really ~36 % (2A4), 2A6 reproduced the flicker without it, and 2A8 measured
the wait at 1.00 vblanks and 2B0 measured the whole DISTRIBUTION as bimodal at
that boundary with no early tail - so the hold pays the HARDWARE and no
throttle or partial hold can exist **GIVEN ONE PENDING FLIP SLOT, which 2B1
found is a BIT THAT READS 0** (`SURFACE_TRIPLE_BUFFER_ENABLE`). The line is
NOT closed; it has exactly one untried lever.** ~91 sets/s with a correct desktop
is one vblank of vsync latch per frame plus Apple's pipeline, not a ceiling to
be beaten.

**The swizzle fix has two halves and both are needed**: the kernel linear force
and the kernel-side userspace patcher (2on–2op). macOS 26 broke the userspace
half twice — the driver moved and was rebuilt (2xt, re-derived in
`src/plugin/rbgpu_mtlcave.h`), and the exec-time hook cannot read the target's
map at all (2xu: `mtlk_probe_read` 1 = KERN_INVALID_ADDRESS, 0 of 1536
patched). The fix is to place the patch from the client's own process instead,
at `IOGraphicsAccelerator2::newUserClient`, and boot 231 measured it working:
`mtlk2_uc_hk 1`, `mtlk2_uc 196`, **`mtlk2_patched 43`**, zero failures. Both
`mtlk_syms` meanings matter: **63 = 0x3F is "all resolved" now**, not 15.

**USB: real interrupts, not a 10 ms poll (2xv).** The five xHCI controllers
expose one MSI vector each, so only interrupter 0 can interrupt;
`AppleUSBXHCI` targets interrupters 1 and 2 for transfer events. Nine OpenCore
patches zero the Interrupter Target field. `tools/xhci_intr0.py` holds the
table and refuses to install one that does not match the running kernel;
`tools/re/xhci_intr0_derive.py` re-derives it after an OS update.

**Open, small, and each one named:**
- **Brightness — WORKS (2zw, boot 282). The chassis must be in iGPU-only /
  Eco mode.** The slider and the keyboard keys both move the panel, driven by
  our own code: `RBAMDFramebuffer` owns the panel with `AppleBacklightDisplay`
  matched on it, `RBFBBlSets` 247 with `RBFBBlParmSetsLin` **244** (that is
  CoreBrightness writing `linear-brightness`, 2y2's identification),
  `RBFBBlLastLevel` 44160 -> `RBFBBlLastCount` 43125 == `RBFBBlReadback`
  43125, `RBFBBlRefuse` 0, `RBFBBlLiveDiffs` 0, and `IODisplayParameters`
  carrying `brightness`/`linear-brightness` at **min 5120** (our floor) and
  **max 65536** (our scale).
  **What was wrong for six milestones was not the code - it was the firmware
  graphics mode.** What is MEASURED, and all that should be recorded (2zx):
  in hybrid mode the 680M's `BL_PWM_CNTL` write lands, reads back
  bit-identical, is contested by nobody and the panel does not move; in Eco
  mode the identical code moves it. **The mechanism is NOT established** - a
  MUX on the backlight PWM pin, the EC owning the PWM, or the firmware
  configuring the backlight onto a different source per mode would all give
  that reading, and nothing discriminates them. (2zw asserted "the MUX routes
  the panel and its backlight to the dGPU"; the panel half cannot be right,
  since our HUBP0 writes have been scanning the desktop onto it since boot
  ~230.) So the register was real, writable and attached to **nothing**. Every
  reading 2zj-2zv took was correct and every inference from them was wrong,
  because they shared one unexamined premise: that the output was connected to
  the silicon under test. It explains all of it - the register holding our duty
  cycle uncontested across 40 s (2zj: nobody contested it because nobody was
  using it), the DMUB ABM commands acked on the right ring at all three control
  types (2zs/2zt/2zu), and Apple's own stack publishing no brightness parameter
  at all (2zv). **The configuration requirement is part of what an armed boot
  means: iGPU-only / Eco mode in the ASUS firmware setup** - and
  `./build/out/blset` now REPORTS the mode before anything else, because Eco
  mode takes the dGPU off the PCI bus entirely, so a device with vendor-id
  **0x10de** present is an unambiguous "hybrid mode, the backlight will not
  reach the panel" (2zx; the match is anchored on the `vendor-id` property,
  since an unanchored `grep -ic 10de` over ioreg returned 3 on a machine with
  no NVIDIA device at all - rule 26 in the same session that wrote rule 47).
  **2zl IS WITHDRAWN.** It said 2xs had transcribed `dce_panel_cntl.c`'s direct
  PWM write onto a chip whose `dc` would never call it - rule 46, right
  offsets, wrong method. False: the direct `BL_PWM_CNTL` write **does** drive
  this panel on this chip. The Linux reading was accurate
  (`dcn31_panel_cntl.c` really has no `.set_backlight_level`) and the inference
  was wrong, because what Linux's `dc` chooses to call says nothing about what
  the hardware block can do. **2xs was right from the start and stood
  unimproved for six milestones while being described as a mistake.**
  The plumbing underneath, all still true: **2xs** writes the PWM duty cycle
  (`BL_PWM_PERIOD` **4000**, `BITCNT` **12**, `BL_PWM_FRACTIONAL_EN` set, so
  the count is fixed point with 4 fractional bits and a count above the period
  is not a saturated PWM - 2y2 nearly reported that as a bug). **2xy**
  answers `kConnectionFlags` with `kIOConnectionBuiltIn` and `getAppleSense`
  with `kPanelTFTConnect`, so `AppleBacklightDisplay` matches. **2y1**
  publishes a full-length parameter name from our own
  `IODisplayParameterHandler` - 2y0 PANICKED doing this by mutating a
  dictionary fetched with `getProperty` (rule 42). **2y2** publishes the key
  the UI writes: **CoreBrightness**, `linear-brightness`, never
  `kIODisplayBrightnessKey`; a set naming an unpublished key is dropped by
  `IODisplay` with nothing recorded anywhere.
  `usable-linear-brightness` reports the ceiling and its writes are declined
  and counted. `'brig'` in the dictionary belongs to
  `IOFramebufferParameterHandler` and is stale by design - do not read it as
  the current level. `./build/out/blset <0..1> [brightness|brig|lin]` drives it
  from the shell and `build/out/dmubbl` is the DMUB instrument; the whole
  2zn-2zu DMUB apparatus is **not needed for brightness** and is kept because
  the mailbox identification and the GPINT round trip are real findings about
  this firmware (journal 2zs).
  **The full narrative of the six dead milestones is in the journal under
  2zj, 2zl, 2zn-2zv** - including every withdrawn reading, the trace-buffer
  write, the out-of-bounds scan index, and why each instrument could not
  discriminate what it was built for.
- **Refresh rate — the panel accepts 60 Hz, measured (2y5, boot 255).** Writing
  `OTG0_V_TOTAL_MIN`/`MAX` with the `MIN_SEL`/`MAX_SEL` bits of
  `V_TOTAL_CONTROL` changed the live raster from 143843 mHz to **60025** at
  2707 lines and the revert put 143821 back, **with no black screen and no link
  retrain** - because `OTG_V_BLANK_START_END` (1311820: start 1100, end 20)
  pins the 1080-line active window independently of V_TOTAL, so a longer
  V_TOTAL is a longer vertical blank and nothing else. `OTG_V_TOTAL` itself is
  Linux's *modeset* path and wants the pipe locked; MIN/MAX is the
  variable-refresh path, designed to be written while scanning. The firmware
  has DRR off (MIN/MAX/MID/CONTROL all **0**, boot 254), so the restore state
  was *read* rather than inferred - and `rb_drr_check` REFUSES on a non-zero
  `V_TOTAL_CONTROL`, because then the restore values are unknown.
  **The pixel clock is derived, not read:** it comes from the DP PLL and this
  project has never located it, but `clk = mHz * H_TOTAL * V_TOTAL` =
  342 011 761 Hz from boot 255's own three measurements, and the hosttest makes
  it round-trip back to V_TOTAL 1129 - a derived clock that reproduces its own
  input. **2y6 turns it into a mode** (`-rbgpufbmodes`, opt-in): two modes, 60 Hz
  advertised at what 2707 lines actually gives (**59992 mHz**) rather than at
  the 60000 asked for, and `setDisplayMode` moves the timing **and** the VBL
  period together - `rb_drr.h` derives the 16.16 rate from the line count and
  the period from that rate, so there is one rate per mode instead of three
  expressions that can drift. **DONE on hardware, boot 256**: the Displays panel
  offers the choice, `system_profiler` independently reports
  `1920 x 1080 @ 60.00Hz`, and `RBFBVBLPeriodNs` moved to 16665556 with
  `RBFBVBLRetunes` 1 - the frame clock followed the timing, which is the whole
  difference from 2y5. **The line count must be DERIVED, not written down:**
  boot 256 measured the raster at 143765 mHz where boot 255 read 143843 (78 mHz
  apart, 0.054 %, the shape of DP spread-spectrum clocking), so it chose **2705**
  lines and landed at 60.004 Hz where a hardcoded 2707 would have given 59.96.
  `RBFBModeRate60` is therefore not a constant between boots - a changed value
  is not a finding. The switch BACK is exercised too (boot 256:
  `RBFBModeSwitches` 2, `RBFBVBLPeriodNs` returning to the exact 2xe constant
  6952507). **Our frame clock is accurate in both modes** - boot 257's
  `RBFBVBLModeMhz` 60033 against a requested 60002 is +0.05 % - and the -5 %
  that 2y7 reported was `animtest` dividing a STALE published counter by a
  fresh wall clock (2y9, rule 31). Per-mode rates are published as
  `RBFBVBLMode{Fires,Ns,Enables,Mhz}` because `VBLFires`/`UptimeNs` are
  cumulative across modes and their ratio is a blend.
  **2z0 derives the whole ladder from the panel** rather than hardcoding one
  rate: `rb_drr_build_modes()` offers native plus every ladder entry slower
  than it, so a 75 Hz panel sees 75/60/50/48 and **nothing faster than native
  is ever offered**. Boot 258 published eight modes
  (`1129@143862 ... 2707@60000 ... 3384@47997`) and a third distinct clock,
  342 056 937 - three boots, three clocks, two different line counts for the
  same requested 60 Hz. **2z1 is the trap underneath it:** `modeApply` checked
  the LIVE `V_TOTAL_CONTROL` against `rb_drr_check`, which refuses a non-zero
  one as "the firmware owns DRR" - but **our own arming is what makes it
  non-zero**, so entering 60 Hz refused every further mode and the
  precondition had become its own violation. `rb_drr_check_owned()` takes the
  live value *and* the setup snapshot; the live CONTROL is now read only to be
  checked, never built on. Two further facts from that boot: **macOS remembers
  the mode selection across a reboot** and restores it through
  `setDisplayMode` before anything asks, so a boot can come up non-native; and
  `CGCompleteDisplayConfiguration` **returns success over a mode change that
  never reached the kext** (rule 13 on an Apple API) - read `RBFBModeCur`, not
  the return code. `tools/modeset.m` (`make modeset`) lists and selects modes
  from the shell, userspace only.
- **The frame rate: the retire tick is the binding constraint, and 6944 µs is
  an INTERIOR optimum (2zc, boot 265).** The dose-response, 143.8 Hz `sets/s`
  against `rbgpuschedtickus`: **2000 → 51.33** (2xn), **6944 → 74.6** (2zb),
  **13888 → 49.2** (2zc). Both directions cost a third of the frame rate, and
  the peak sits exactly at the native panel period. **A cost that rises in both
  directions is not a rate limit** - a rate limit is one-sided - it is two
  periodic timers beating, the retire `thread_call` against the VBL
  `thread_call`. So the fix is to **retire frames from the VBL handler**, which
  deletes the second timer rather than tuning it; that now rests on a measured
  two-sided curve instead of on an inference, which matters because **five
  models of this ceiling have died**, three of them to residual arithmetic.
  **Do not take another dose point.**
  What 2zb established and 2zc did not disturb: a flip latches in **1.0-1.5
  vblanks, never past 3**, and it is **not a HUBP backlog** - `Preempted` 7 of
  19 669 and `PendHits` **0** of 19 669, two instruments at different frame
  phases agreeing every new flip finds the previous one already latched. The
  cap is **ours, not the workload's**: 1, 2 and 3 concurrent `animtest`
  instances give 72.43 / 68.53 / 76.15 sets/s, flat under triple demand.
  **2zb's `min(latch_ceiling, ~75)` describes the stock tick only** - the ~75
  term is a function of `tick_us`, not a constant of the hardware, which is how
  2zc refuted it: `actual / latch_ceiling` at 60 Hz was 97-100 % in all three
  of 2zb's passes and 83-87 % with the tick doubled. The `kIOReturnNotReady`
  hold is **already conditional on the live PENDING bit** (2xn) with the
  deadline only a backstop, so "make the hold conditional" is not an available
  dial; `RBFBAmtrHoldExpired` 0 says the backstop is never reached.
  `tools/paired.sh` pairs the latch histogram with the frame rate over one
  **measured** window and **refuses any row whose `vbl/s` disagrees with the
  mode's own rate by over 5 %** - that guard exists because without it the
  script reported a 194 Hz panel, having divided a real counter delta by
  `animtest`'s `--secs` argument rather than the elapsed time (2zb).
  **And one sample cannot resolve a configuration change (2ze, boot 266):**
  ten runs at 143.85 Hz on one boot gave 63.98-100.77, mean 73.1, median 70.0 -
  a ~±10 % spread with a 40 % outlier - so **a comparison needs n>=6 per side
  at 144 Hz**, or should be made at 60 Hz, where `actual / latch_ceiling` pins
  at 100 % run after run. The first run of boot 266 read 100.77 and would have
  recorded 2zd as a success on a boot whose own counters said the handover
  never ran. This also weakens 2zb's "74.6 at 144 = 74.8 at 120, therefore a
  hard cap", which rested on two samples per mode and sits inside the band;
  2zc's -34 % is outside it and stands.
- **Retiring from the vblank: WITHDRAWN as a throughput win, kept for variance
  (2zd/2zf/2zg, boots 267-268).** `-rbgpuretirevbl` on **both** kexts moves
  frame retirement onto the vblank. Boot 267 measured +24 % at native (90.4
  sets/s, n=6); **boot 268 measured 72.50 (n=12) in the same configuration with
  the gate provably inert, against a 73.1 pre-handover baseline - so the gain
  is WITHDRAWN.** Boot-to-boot variance exceeds within-boot variance, so
  2ze's "n>=6 per side" is not enough: **a configuration claim needs >=2 boots
  per side, or an A/B inside one boot.** Rule 18 one level up - do not build a
  table from n=1 **boots**.
  **The latch was a correlate, not a constraint.** `mean vblanks` is 1.00 iff
  the vblank drives, in both modes on both boots (native/vblank 1.00-1.16,
  60 Hz/timer 1.04-1.48, 60 Hz/vblank 1.00-1.02, native/timer 1.10-1.63) - so
  the beat IS the latch - **and removing it moved throughput by nothing**
  (72.5 with it gone vs 73.1 with it present). 2zb's "latch ceiling" was
  measuring a symptom, and 2zd's 110-120 prediction was built on it. Sixth
  model of this ceiling to die.
  **What survives: the variance collapses when the vblank drives** - native
  ±3.5 % (n=12) against ±20 % with a 40 % outlier (n=10) timer-driven, and
  60 Hz ±0.4 % against ±17 %. Four datasets, both modes, both drivers,
  reproduced across boots. The beat was the noise source all along, which is
  what every variance surprise in 2zb/2ze was. Same mean, far steadier
  delivery.
  **And the mode gate works**: below native the vblank cannot supply the retire
  RATE (retirement ran at a fixed 6944 µs - 144/s - whatever the display did),
  so 2zf measured 60 Hz at 30.04, exactly half its vblank rate;
  `rb_retire_vbl_suits()` hands those modes back to the timer and boot 268
  recovered 60 Hz to 49.0 with `ModeSkips` climbing, `vblret_src` 0,
  `vblret_take` 1 and `vblret_hand` 2 on the return to native - every
  transition of the state machine now exercised on hardware.
  **Still open, and now the only thing open: `fed` is ~50 % at native** with
  the latch ceiling at the panel rate, so half of every frame period is
  unaccounted for. The mean has sat at 70-75 sets/s through tick 2000, 6944,
  13888, timer-driven and vblank-driven alike, and through the publish throttle
  both ways (2zz). **Something sets ~73, it is none of the seven things
  measured so far, and 2zz showed it does not respond to our own frame-path
  cost at all.**
  **2zk's suspect is RETIRED, and it is the seventh model to die (2zz, boot
  282).** `publishVBLState()` really does run **227 times a second** from
  `setInterruptState` - twice per transaction, because Apple arms and disarms
  the vblank around each one - at ~110 `setProperty` calls each, on Apple's own
  thread, inside the call that gates every frame, which is ~25 000 registry
  insertions a second and was rule 16 unexamined through six dead models.
  **It costs 1.4 % of the frame rate.** 2zm made the dial writable at runtime
  (`setProperties` takes `{"RBFBPubMs": N}`, `build/out/pubms` writes it) so
  `tools/pubms_aba.sh` could run the whole A/B/A in ONE boot at native: 18 runs
  of 20 s, A=500 / B=0 / A=500, giving **A1 71.23, B 70.18, A2 71.06** - the A
  phases agreeing to **0.2 %** and B at **-1.4 %**, inside the band. The
  throttle stays (the cost is real and the default is still 500 ms) but it is
  not the frontier. Boot 271's 49.0 -> 59.43 at 60 Hz was one side on one boot
  and could not separate the dial from the boot; this is what rule 44's "or an
  A/B inside one boot" was for, and it cost one boot instead of four.
  **The result that outlived the question: `fed` is 48.0-50.3 % in ALL EIGHTEEN
  runs** - two configurations differing by ~98 % of the frame path's registry
  traffic, and the fed fraction does not move beyond its own noise. So the
  missing half of every frame period is **insensitive to our own bookkeeping**,
  which retires that whole family of hypotheses rather than just this one. The
  variance also reproduces 2zg's surviving half independently: all 18 runs span
  65.86-73.42 (+-5.4 %) vblank-driven, against 2ze's 63.98-100.77 timer-driven.
  **Do not propose an eighth model from the counters already in hand.** Seven
  dials have been moved - tick down, tick up, the timer beat, the latch, the
  workload, the HUBP backlog, the publish cost - and all seven returned 70-75.
  The next step must measure where the time in a frame period actually goes,
  not test another hypothesis by reading `sets/s`.
  **2A0 is that measurement, and it starts by re-reading a number already in
  hand.** `fed` is `AmtrSets/s / vbl/s`, so `fed ~49 %` never meant "half the
  frame period is unaccounted for" - it means **the compositor hands us a
  transaction only on every second vblank**, and boot 271 at 60 Hz read `fed`
  **97-99 %**, feeding every vblank there. So the ceiling is an absolute rate
  near 73/s rather than a fraction of the refresh: a **period**, ~13.7 ms,
  which is measured by timing it. `src/common/rb_frametime.h` tiles that
  interval into `work` (inside `setAttribute('amtr')`) and `gap` (between
  transactions), with our `'amtr'` GETs and our vblank handler accumulated
  separately inside the gap, so the remainder is time Apple had the frame and
  did not call us. **If `upstream` dominates, the ceiling is upstream of this
  kext and no dial of ours can move it** - which explains all seven dead
  models at once instead of adding an eighth. `sudo tools/ftwindow.sh` brackets
  one native window and prints the budget; `build/out/ftbudget` reads it and
  `ftbudget reset|off` is the dial (`{"RBFBFtOn": 0|1}`, runtime, which also
  zeroes the window). Three things make it an instrument rather than a number:
  **the self-check** - `RBFBFtRateMhz` is sets/s from the timings and
  `paired.sh` is sets/s from a counter delta, and `ftwindow.sh` refuses its own
  verdict if the two disagree by over 5 % (the guard 2zb's 194 Hz panel
  lacked); **the gap bound** - `8 x vblPeriodNs`, because the desktop is idle
  most of the time and one idle second averaged into 100 samples of 13.7 ms
  reads as the frame cost with nothing published to say so, so it is dropped
  and COUNTED; and **the off switch**, since rule 16 makes the instrument a
  suspect. One thing it is honest about rather than silent on: the 2xo
  `kIOReturnNotReady` hold is ours but Apple spends the wait, so its cost sits
  inside `upstream` - read `RBFBAmtrHeld`/`RBFBAmtrGets` to attribute it.
  **2A1 RAN IT, and the answer is that the ceiling is not ours (boot 283).**
  One 20 s native window, cross-check passing (**budget 95.51 /s vs
  paired.sh's 93.95 /s, +1.7 %**): `work` **0.003 ms**, `gap` 10.467 ms, GET
  0.002 ms, vblank 0.023 ms, so **ours 0.044 ms (0.4 %) and upstream 10.426 ms
  (99.5 %)** of a 10.470 ms interval. **Our entire cost per transaction is 44
  microseconds.** That is why all seven dials returned 70-75 - there was never
  enough of our own time in a frame period for any of them to be the ceiling,
  and 2zz's -1.4 % was the right order of magnitude for the whole of our
  bookkeeping. Two gap samples were dropped above the bound and counted, which
  is the instrument working rather than averaging a long interval in.
  **That boot also read `sets/s` 93.95 and `fed` 65.6 %, and the user reported
  the desktop "lowkey way smoother" before seeing a number** - the one link a
  human can see (rule 45), unprompted and agreeing with the counter. **Not a
  claim:** n=1 on one boot, and 2zg exists because boot 267's 90.4 did not
  reproduce on boot 268 (72.50). 2A0 adds two `mach_absolute_time()` reads per
  transaction and changes nothing about frame delivery, so it cannot be the
  cause. Rule 44 still wants two boots or an A/B.
  **The one piece of `upstream` we own is the 2xo hold, and its cost is
  UNMEASURED** - `held/gets` was 38 % over the window, but Apple spends the
  wait inside `isTransactionComplete` (2x6) and the budget cannot attribute
  it. A first reading of 2A1 called 38 % held inconsistent with `mean vblanks`
  1.001 and multiplied 1.2 held GETs by a vblank to claim ~8.3 ms; **both
  steps are withdrawn in the journal** - the GETs arrive ~3.2x per transaction
  so 38 % pending is consistent, and `amtrHoldExpired` 0 says only that the
  bit clears before the deadline, never how fast Apple retries. **2A1 adds the
  measurement with its control:** `RBFBFtRetryNs` is the gap from a GET we HELD
  to the next GET, `RBFBFtPollNs` the same gap after we answered ready - one
  quantity, one clock, one window, split only by what we told Apple. Alike
  means the hold is not the ceiling and `upstream` is Apple's pacing; a vblank
  against microseconds means the hold is the ceiling and it is ours.
  `ftbudget` prints the ratio and REFUSES the comparison under 30 samples a
  side (rule 18).
  **2A2 ANSWERED IT: the hold owns ~21 % of the interval, and 2A1's verdict
  was wrong (boot 284).** `retry` **4.384 ms** (n 2606) against `poll`
  **2.771 ms** (n 3059) = a **1.613 ms** penalty x **1.70** holds per
  transaction = **2.75 ms of 12.993 ms, 21.1 %**. 2A1's `ftbudget` printed
  "the hold does NOT delay Apple measurably" off a ratio under **2.0x**; that
  threshold was invented and **the ratio is the wrong statistic** - what costs
  frame rate is penalty x holds-per-transaction. **The premise is now checked
  rather than assumed:** the attribution is valid only if the GET gaps account
  for the interval, and they do to 0.1 % (`4.384x2606 + 2.771x3059` over 1531
  transactions = 13.00 ms vs 12.993 ms), published as `RBFBFtGetsTilePm` and
  printed BEFORE the share, with the share refused outside 900-1100 and the
  tiling deliberately NOT clamped at 1000 since 1300 is a refusal. This is the
  same multiplication 2A1 withdrew - the difference is that the factor is
  MEASURED here, not invented.
  **It corroborates 2xo independently**, which recorded this hold costing
  **27 %** of the frame rate by a different route four milestones earlier.
  Without it the interval would be 10.25 ms -> **~97/s against 75.72**.
  **So it is a TRADE and the dial is a dial, not a removal:** the hold is what
  stopped the compositor drawing into the live scanout, and 2xo's whole-frame
  flicker is what turning it off costs. `{"RBFBNotReady": 0|1}` via
  `build/out/holdset`, with `RBFBAmtrNotReady` publishing the live state so an
  A/B can verify its side (rule 20) and the write zeroing the frame-time
  window (rule 39). **`sudo tools/holdaba.sh`** runs A/B/A at native in one
  boot, refuses a boot with the hold unarmed (no A side, rule 34), and says in
  its banner that **the flicker during B is data** - a fast AND clean B would
  mean 2xo's diagnosis was wrong (rule 45).
  **2A4: THE HOLD WAS THE CEILING - 141.24 sets/s at `fed` 98 %, and the
  frame-rate line is CLOSED (boot 286).** A/B/A in one boot, A = hold ON:
  **A1 89.41, B 141.24, A2 92.62**, A1->A2 drift **+3.6 %** (inside the band,
  so B is attributable) and **B vs mean(A) +55.2 %**. The panel scans at
  143.4 Hz and B delivers **96.1-98.3 %** of its vblanks. The answer was our
  own `kIOReturnNotReady` hold and the ceiling was never 73.
  **2A6 WITHDRAWS the "no flicker" half of this. THE FLICKER IS REAL and 2xo
  STANDS.** In ordinary use on boot 287 with the hold off, a the assistant animation
  gave **"the top part of the assistant blended with wallpaper behind with 10
  flickers, then it stopped"** - the compositor drawing into a surface still
  being scanned, which is exactly 2xo's hazard seen from the outside. 2A4's
  ten seconds of `holdlook.sh` and 2A5's minutes of use missed it because
  **`animtest` is one steady stream of flips and an idle desktop submits
  almost nothing**, while the trigger is **animated window content
  compositing OVER something else** - a bursty transient neither produces.
  **A benchmark is not a workload, and "a rare event did not occur" is not
  "it cannot".** Rule 45 again, in the expected direction.
  **2A7: the user resolved the ambiguity - the flicker has NEVER been seen
  with the hold ON.** So "80 % less" meant less than expected, and **2xo
  stands in full.** The configuration question is CLOSED: the hold stays
  default-on, 141 is not available, and ~91 with a correct desktop is the
  shipping behaviour. ("then it stopped" still hints the hazard is tied to the
  appearance transient rather than steady animation - unmeasured.)
  **And 2A6's MECHANISM is corrected.** It said the hardware "finishes well
  inside the next vblank" and blamed Apple's retry granularity; but
  `rb_hubp.h` records the flip as **vsync'd** (`RB_HUBP_FLIP_TYPE` 0), so
  `SURFACE_FLIP_PENDING` **cannot** clear before the next vblank, and 2A5's
  `mean vblanks` 1.066 says it clears *at* about the next one. Two numbers
  (4.384 vs 2.771 ms) were turned into a mechanism - rule 17's shape, in a
  session that had already caught itself twice.
  What survives is weaker and testable: **one vblank of every hold is physics,
  and an ask that misses the latch boundary costs a whole extra round trip.**
  No-hold ran 7.08 ms per transaction (~1 vblank), hold-on ~11 ms (~1.6), and
  the missing ~0.6 vblank is either that phase or something else. **2A7
  measures it rather than arguing**: `ftLatchWait` is the wall time from a
  run's first held GET to the GET that sees the flip latched, `ftHoldRunAsks`
  is how many asks that took, and `ftbudget` prints the wait IN VBLANKS beside
  the ask count. **~1 vblank / ~1 ask means the hold pays the hardware, ~36 %
  is simply the price of the 2xo fix, and the frame-rate line is closed for
  good; ~2 vblanks / ~2 asks means the phase is the cost and there is
  something to attack.** Instrumentation only - nothing about the hold
  changed. Framebuffer **614ED4BC**.
  **2A8 ANSWERED IT: PHYSICS. latch wait 6.975 ms = 1.00 vblanks, n 1856,
  asks 1.31 (boot 288, cross-check +1.7 %).** Against a 6.953 ms period that
  is **1.003 vblanks**: the flip is vsync-latched and cannot resolve sooner,
  so **there is nothing to recover and THE FRAME-RATE LINE IS CLOSED.** Eight
  models died, the answer was our own hold, and the hold is right. **Do not
  reopen it without new evidence.**
  **And the instrument refuted itself in the same output.** It printed "the
  hold owns 42.3 % -> **~162.3 /s** without it" on a panel that scans at
  143.8 Hz - not optimistic but **impossible**, and 2A4 had already measured
  the no-hold rate at 141. **2A4's "a marginal measurement is a lower bound"
  is itself corrected: it is not a bound at all, it is a different quantity**,
  answering "what does this cost inside the current regime" and never "what
  happens if it goes" - only an A/B answers that.
  `rb_ft_hold_free_interval_ns()` now returns 0 (**no usable prediction**) as
  soon as the implied interval falls under one vblank, and `ftbudget` says why
  and points at `holdaba.sh`; the share is still printed, only the rate is
  refused. Hosttested against boot 288's own numbers, both sides of the
  boundary, with a control so the check cannot be a constant 0.
  **2A9 reopens exactly one question, and only because 2A8 closed it on a
  MEAN.** The user runs hold-OFF for 144 Hz and asked for a middle setting -
  "feed 91 % of the vblanks instead of 98 %". **That dial cannot work: the
  hazard is PER FLIP**, so 7 % fewer flips is 7 % less flicker for 7 % less
  frame rate, and the flicker is already rare enough that the dilution is
  invisible. There is no safe intermediate either - anything answering `ready`
  while our flip is unlatched carries the full hazard, and the 91-vs-141 gap
  is Apple's post-ready pipeline, which only overlaps our latch if a second
  flip is in flight, which is what the hold forbids. **But 1.00 vblanks is a
  mean**, and a mean cannot tell "every flip needs the whole period" (no
  middle, 2A8 stands) from "most resolve at once and a TAIL is exposed" (a
  partial hold removes most of the hazard for a fraction of the cost). So
  `rb_ft_hist` counts the distribution in **quarters of a vblank** and
  `RBFBFtPendEarlyPm` is the answer: ~0 is no middle, large names the prize.
  **The instrument was blind in the regime under investigation** -
  `hubpHoldNotReady()` returned on `!amtrNotReady` BEFORE reading
  `SURFACE_FLIP_PENDING`, so turning the hold off turned the measurement off.
  Split into `hubpFlipPending()` (read, no side effect) and
  `hubpHoldNotReady(pending)` (decide). **General shape: when a dial turns a
  feature off, check whether it also turns the MEASUREMENT off** - an A/B
  whose B side cannot be instrumented is not an A/B. A PENDING run files in
  both regimes, so the hold-on reading is the CONTROL and must not be read as
  evidence about hold-off (rule 39 applied to a histogram; `ftbudget` prints
  the regime on every line and the setter zeroes the histogram on a switch).
  With the hold on it reproduces `ftLatchWait`, refused over +-5 % - a free
  cross-check. **Nothing is claimed: no reading has been taken, and 2A8's
  closure stands until the histogram says otherwise WITH THE HOLD OFF.**
  Framebuffer **3E09FBD5**. Caught pre-build: `hubpFlipCtl` is 2xj's one-shot
  setup snapshot published as `RBFBHubpFlipCtl`, and reusing it would have
  overwritten a published setup fact with frame-path noise ~300x/s with no
  symptom - rule 42's "who owns this?" asked of a scalar.
  **2B0 ANSWERED 2A9: NO MIDDLE SETTING, and 2A9's verdict line was WRONG
  (boot 289).** Five windows, both regimes: **the first three quarters of the
  vblank period are EMPTY in every one.** The distribution is **bimodal at the
  vblank boundary** - [0.75,1.00) 27-53 % and [>=1.00) 47-73 %, with buckets
  0-2 at 0.0-0.8 %. Every flip needs essentially the whole period, so there is
  no tail to exempt and **a partial hold has nothing to be partial about.**
  **2A8 stands; 91 with a correct desktop or ~140 with occasional flicker is
  the entire menu, and the choice is the user's.**
  **And `ftbudget` printed "A MIDDLE SETTING MAY EXIST: 53 %" off this very
  data.** `rb_ft_hist_early_pm()` returned "everything not in the overflow
  bucket"; bucket 3 is **0.75-1.00 of a period**, i.e. 5.2-6.9 ms of exposed
  scanout, which is the whole hazard and not a reduced one. The bucket counts
  "resolved before a FULL period elapsed" and the verdict needed "resolved
  early enough to be SAFE". **Third recorded instance of one error: a
  statistic must be derived from what the consumer DOES with it, not from
  whichever summary is easiest** (2A1's invented 2.0x, 2A2's "the ratio is the
  wrong statistic", now this - one milestone after writing 2A2 down). The
  histogram was published RAW and refuted its own verdict in the same output,
  which is why this cost a verdict and not a milestone. Corrected to buckets
  0-2; boot 289 reads **4-7 per-mille against a 100 per-mille threshold**, two
  orders clear, **stated because after 2A1 a threshold must be SHOWN not to be
  load-bearing.** `ftbudget` now computes the verdict from the raw
  `RBFBFtPendQ*` buckets, so the interpretation no longer depends on which
  build is installed.
  **The rule-39 caveat is DISCHARGED:** pending wait is 1.00-1.01 vblanks with
  the hold ON and **1.01 with it OFF**, same shape - the regime change does not
  move the latch phase, so the verdict rests on the regime that actually
  flickers.
  Also from boot 289: **`preempted` moves +4/+2 (hold on) to +81/+26 (hold
  off)** - `rb_hubp_latch_write()` counting a flip programmed while the
  previous was still unlatched, so 2zb's "negligible" was negligible *because
  of the hold*. Not the flicker (that is the compositor drawing into a live
  surface; a preempted flip never got scanned) - related, counted, different.
  **Hold-off 139.24/139.60 sets/s at `fed` 98.1/98.2 % reproduces 2A4's 141.24
  on a second boot**, so rule 44 is satisfied and ~140 is a finding.
  Framebuffer **BB81C37D**; the corrected `early_pm` only changes a number the
  tool no longer reads, so **no reinstall is needed.**
  **2B1: "THE FRAME-RATE LINE IS CLOSED" WAS WRONG, and the lever is a
  HARDWARE BIT THAT READS 0.** 2A8 and 2B0 are both correct and both assume
  **one pending flip slot**. `DCSURF_FLIP_CONTROL2` has read **0x40** on every
  boot since 216 - `PENDING_MIN_TIME` 64 with **`SURFACE_TRIPLE_BUFFER_ENABLE`
  CLEAR** - and `rb_hubp.h` has documented that bit since 2xm, written a guard
  for it (`rb_hubp_flip_would_discard`, inert *because* it is off) and **never
  asked whether to set it**; the comment says "triple buffering is the
  compositor's to change", inherited from a stock-driver model that does not
  apply when **we** write HUBP and Apple's compositor does not know it exists.
  **Rule 47 asked of a WRITE instead of a read: before calling a limit
  physics, establish the hardware is configured the way the conclusion
  assumes.**
  **What two slots change:** programming an address no longer discards an
  unlatched flip (boot 289: `preempted` **+81/+26** with the hold off against
  **+4/+2** with it on, so 2zb's "negligible" was negligible *because of the
  hold*), and there is **one more frame of pipeline** between Apple accepting a
  transaction and that surface being scanned - exactly the slack the fused
  release event denies us, since
  `IOPAttachment::HandleTransactionCompletionEvent` does the surface release
  **and** `DisplayPipe::NotifyIsReady()` in ONE event. **A mechanism for "full
  rate AND no flicker", which no dial so far could have produced.**
  **Method grouped correctly (rule 46):** `dcn31_hubp_funcs` maps
  `.hubp_enable_tripleBuffer = hubp2_enable_triplebuffer` **verbatim** - a
  plain RMW, no lock, no sequencing (`UPSTREAM.md` has the three paths).
  **NOT reproduced:** Linux gates it behind `dc->debug.enable_tri_buf` and
  pairs a triple-buffered flip with `triplebuffer_lock`/`unlock` on the timing
  generator instead of the normal pair, and **we set the HUBP bit without that
  protocol** - rule 46 one level deeper, so the failure mode is unknown (a
  tear, a dropped frame or a wedge; not a panic).
  **Runtime dial, OFF by default:** `{"RBFBTriple": 0|1}` via
  `build/out/tripleset`, because what it must buy is "hold OFF **and** no
  flicker" - four combinations A/B'd against `holdset` in one boot, with the
  user's eyes as the only flicker instrument (rule 45; 2A6 is what `animtest`
  standing in for them costs). Snapshot-based restore (`tripleset 0` puts the
  firmware's own word back), an unrecognised `FLIP_CONTROL2` **refused** with
  the value published, and the **hardware readback** decides success, not the
  store (rules 19/20/13). Framebuffer **60F39344**.
  **2B2: IT WORKS - 141.03/143.12 sets/s at `fed` 96.9/98.2 %, and the
  flicker is GONE from ordinary use (boot 290).** `tripleset 1` +
  `holdset 0`, two 20 s windows, cross-check +1.6 %/+0.1 %. The user, rule
  45, unprompted: **"artifacts only happen on fast typing now... doing
  animations and even the assistant code doesn't trigger artifacts anymore"** - so
  2A6's own trigger no longer reproduces, and ~142 against the shipping ~91
  is **+56 %**. **The measurement agrees with the MECHANISM, not just the
  outcome:** 2B1 predicted one more frame of pipeline and NOT a faster latch,
  and the latch did not move - `mean vblanks` 1.000/1.002 and the pending
  wait still **bimodal at the boundary** (52.0/48.0, first three quarters
  empty), identical to 2B0. So 2A8 and 2B0 are both still true: the hazard
  was never the latch time, it was the OVERLAP, and the slot count sets the
  overlap. `preempted` corroborates independently and in the predicted
  direction: **+81/+26 -> +13/+15**, a 5x drop with the hold still off.
  **What is LEFT is the same hazard at a higher rate: fast key repeat.**
  Holding Delete still artifacts, which is consistent with `preempt` being
  +13/+15 rather than 0 - a THIRD flip inside one period still preempts and
  there is no third slot, so this is not another dial of the same kind. The
  exposure window is one generation further away, not closed.
  **2B7: THE BIT IS INERT. Interleaved 0/1/0/1 with the hold OFF, all four
  cross-checks passing: rate 95.4 (off) vs 96.9 (on), +1.6 %, and pending wait
  1.535 vs 1.525 - NO DIFFERENCE.** With 2B6's hold-ON pair that is **both
  regimes, interleaved, one boot: `SURFACE_TRIPLE_BUFFER_ENABLE` changes no
  quantity this project can measure.** It arms correctly (readback 0x440,
  `PENDING_MIN_TIME` preserved) and does nothing. **The dial stays OFF for a
  measured reason now, not a precautionary one.**
  **AND `preempt` DOES NOT ORDER BY THE DIAL, which withdraws 2B2's strongest
  argument**: +15, +5, +2, **+121**, with the largest on a triple=1 run. 2B2
  called +81/+26 -> +13/+15 "independent corroboration in the predicted
  direction"; it was variance read as mechanism because it pointed the expected
  way - **rule 17 again, on a counter 2B0 had just finished calling "related,
  counted, different".** A counter that moves in a NON-INTERLEAVED comparison
  has not been shown to move because of the change.
  **What survives is one flicker observation, and it is confounded**: it was
  made at 141-143 sets/s with a 1.01-vblank latch, and the flicker is a race
  whose odds depend on both; this boot feeds 93-97 at 1.5 vblanks. Not
  withdrawn as an observation (rule 45), but not evidence for the dial.
  **RULE 44 IS WRONG AS WRITTEN, and this is the real result of 2B5-2B7.**
  Its "or an A/B inside one boot" is refuted: earlier in boot 292 the hold-off
  configuration read **83-84** and an hour later the same configuration read
  **93-97**, so **the boot drifted ~15 % inside itself** - which IS the
  +15.1 % A1->A2 drift that made 2B6 unattributable. The drift happened inside
  the instrument meant to be immune to it. **An A/B inside one boot is only
  valid INTERLEAVED**, at single-window granularity; `holdaba.sh`'s A1x6/Bx6/
  A2x6 shape cannot tell a dial from a trend. This invalidates nothing measured
  interleaved or self-consistently (2A1/2A2's budget, 2A8's latch, 2B0's
  distribution, 2zz's A/B/A at 0.2 %) and weakens every single-window
  comparison from 2zb onward.
  **The frontier is ONE question: what drifts, within a boot and between boots,
  while `ours` stays pinned at 0.4 %?** Four boot-level occurrences plus one
  within-boot. `preempt` +2 -> +121 inside a fixed configuration is the only
  counter yet seen that moves with it and is neither `fed` nor the latch.
  **The next measurement must establish the TIME COURSE** - one fixed
  configuration, the same window repeated every few minutes from boot, rate /
  latch / `preempt` against uptime. Every experiment from 2zb to 2B7 compared
  configurations instead, and **no eighth model is to be proposed from counters
  already in hand.**
  **2B6: 2B2 IS WITHDRAWN AS A CLAIM. The A/B/A refused its own attribution -
  A1 76.93, B 95.93, A2 88.54, so A1->A2 drift +15.1 % against B's +15.9 %:
  THE DRIFT IS THE SAME SIZE AS THE EFFECT** (`holdaba.sh`'s own ~8 % rule,
  which 2A4 passed at +3.6 %), with four of A2's six hold-ON runs inside B's
  range. The configuration was verified per phase, so the dial moved and the
  MACHINE is what drifted. **This boot is slow in BOTH regimes** - hold-ON
  76.9-88.5 against the ~92 of 2A3/2A4/2B0, hold-OFF 95.9 against ~140.
  **The one new fact worth carrying: the latch separates the regimes.** Hold
  ON reads **1.00/1.01 vblanks** with the 2A7 cross-check at +0.0 % and the
  usual bimodal shape - identical to 2A8's boot, nothing abnormal - while hold
  OFF reads **1.73-1.81**. So this boot's problem **only manifests when flips
  are PIPELINED**; `preempt` +0/+2 with the hold on against +17/+18/+60/+120
  without it is the same division. Stated as a division, **not a mechanism -
  no eighth model from counters in hand.**
  **And one result IS valid, the control 2B1 needed: with the hold ON the
  second slot changes nothing** (83.61 vs 82.65, `preempt` +0 vs +2,
  cross-checks +1.1/+0.9 %) - expected, since the hold forbids a second
  in-flight flip, and it shows arming the bit is harmless by itself.
  **Method error, mine:** `holdaba.sh` restores the shipping configuration as
  its last act, so the `tripleset 0/1` pair handed over after it ran with the
  hold ON and measured that control instead of 2B1's claim. **A dial's state at
  the start of an experiment is part of its arm (rule 34), and a script that
  restores a default is what invalidates the next command typed after it.**
  2B1's direct A/B needs `holdset 0` first and has NOT been run.
  **The shipping default (hold ON, triple OFF) is unchanged and its latch is
  1.00 vblanks on every boot that measured it.** Its RATE is now ~92 on three
  boots and ~77-88 here - the **fourth** time two boots in one configuration
  came apart by 15-70 % with `ours` pinned at 0.4 % (2A1/2A2, 2zg, 2B5, this).
  **That is the frontier here, it is not a dial, and no single-boot reading can
  resolve it.** Cheapest discriminating step, not another dial: take the
  hold-OFF latch reading in the FIRST MINUTES of a fresh boot, since `preempt`
  grows within a boot. 1.0 early and 1.7 later means the no-hold regime
  degrades over a boot's life; 1.7 immediately makes it a property of the boot.
  **2B5: IT DID NOT REPRODUCE. Boot 292, same dials, cable out: 84.43/83.02
  sets/s with pending wait 1.73/1.81 vblanks** (cross-checks +1.0/+0.4 %, hold
  provably off - cumulative `held/gets` frozen at 5638 while gets climbed to
  55 517). **So 2B2's +56 % is one boot, and rule 44 bit where 2B2 said it
  might.** What moved is the LATCH while `ours` stayed 0.4 % on both boots -
  a display-engine quantity, so the difference is not ours; **third time this
  project has seen two boots in one configuration with `ours` identical and
  the rate 40-70 % apart** (2A1/2A2, 2zg, this), still unexplained, no model
  proposed. `preempt` also climbed WITHIN the boot - +17, +18, +60, +120 over
  four windows - which is not variance and is unexplained.
  **The next measurement is the A/B inside ONE boot, which needs no reboot**:
  `holdset 1` / `holdset 0` with triple left on, and `tripleset 0` with the
  hold off. Comparing across boots cannot separate the dials from the boot.
  **Two instrument defects found on the way (2B5).**
  **(1) `RBFBAmtrNotReadyArmed` is a DUPLICATE of `RBFBAmtrNotReady`** -
  `RBAMDFramebufferVBL.cpp:227` publishes `amtrNotReady` for both, so the key
  whose name claims "the boot set this up" carries the live value and reads
  `No` after a `holdset 0` on a boot that armed it fine. **No boot can say
  afterwards whether the hold was armed at setup** (the fact rule 34 wants),
  and **"it can only arm what the boot set up", written in this card, describes
  no code** - the setter has no such gate. 2A3 inverted: a property that moves
  when its name says it cannot. Fix is an `amtrNotReadySetup` snapshot in
  `start()`, bundled rather than given its own boot.
  **(2) 2B2 was labelled "boot 290" without reading `rbgpu-boot`** (it reads
  **292** now) - rule 8, the first rule in the reading-a-boot section.
  **RULE 44 WAS NOT SATISFIED: 2B2 was one boot, n=2 windows, and 2A4 is what
  one boot of this kind of good news cost last time.** The difference is that
  the clean report comes from ordinary use over a working session - 2A6's
  identified workload - and the mechanism predicted the result's shape in
  advance. A second boot turns it into a finding, and costs two commands
  because the dial is runtime-only. **Nothing arms it by default yet**, and
  Linux's `triplebuffer_lock` protocol is still not reproduced, so the rare
  failure modes 2B1 named are untested rather than excluded.
  **And on "does another driver fix this":** not a hardware gap and not an
  RDNA2-vs-3 thing - the bit exists on RDNA 3 and Linux `amdgpu` runs this
  chip at full refresh with no flicker. What Linux has is the **release path**
  (`dma-fence` + DRM vblank events signal buffer release independently of
  accepting the next flip); macOS **fuses** those two into one CoreDisplay
  event, which is the structural reason we pay a vblank. The NVIDIA driver is
  irrelevant (different display engine, and Eco mode takes the dGPU off the
  bus). **NootedRed drives Rembrandt on macOS and cannot be read** (ChefKiss
  OKL §2/§3) - if it runs this chip at full refresh without flicker that is an
  existence proof, but it must come from the user in their own words.
  **The only loose thread left here: the FIRST-RUN TRANSIENT** (2A4 - run 1 of
  a phase reads ~73-76 where runs 2-6 read ~92), and **2B0 is the first new
  information about it: run 1 is more contended in EVERY counter, not just
  `sets/s`** - hold-on `mean vblanks` 1.022 -> 1.002, hold-off `preempt`
  +81 -> +26, `bin2+` 7.1 % -> 0.2 %, `asks per run` 1.59 -> 1.00. It has
  biased every single-window measurement since 2zb and nobody has looked at
  it.
  **The trade is therefore real - but the cost is Apple's RETRY GRANULARITY,
  not the serialisation.** 2A2 measured the post-hold gap at 4.384 ms against
  2.771 ms after a ready answer, while the flip latches in ~1.07 vblanks
  (2A5): we answer NotReady, the hardware finishes well inside the next
  vblank, and Apple does not ask again for ~1.6 ms. **The next experiment is
  "hold for the actual latch, not until Apple's next poll"** - open, and
  constrained by rule 31 (no blocking on Apple's thread) with 2x6's
  `isTransactionComplete` as the waiter, so a bounded wait there is a design
  decision whose failure mode is a wedge. **Nothing changed on this evidence.**
  **The default was never flipped, and that is what went right:**
  `-rbgpufbnotready` stayed in `rb_defaults.h` through 2A4 and 2A5 because the
  evidence was ten seconds of one workload, so this cost a reverted boot-arg
  instead of a regression in every armed boot.
  **Both earlier estimates of the hold were LOWER BOUNDS, for one reason.**
  2A2 put it at 21 % and 2xo at 27 %; it is **~36 %** of the achievable rate
  (1 - 91.02/141.24). The retry-gap arithmetic was sound and its tiling
  premise held, but it measured the **marginal** penalty per held GET with the
  GET count and the feed rate held FIXED - and removing the hold takes `fed`
  from ~63 % to ~98 %, so it is a regime change, not a subtraction. **General
  lesson: a marginal measurement is a lower bound on the effect of removing
  the thing.**
  **2A3's "two states" is WITHDRAWN.** A1 run 1 read 73.57 / `fed` 51.5 % and
  runs 2-6 averaged 92.58 on the SAME boot in the SAME configuration -
  excluding first runs, A1 92.58 against A2 92.62, **0.04 % apart**. Boot
  284's 75.72 was one window (a first run), 2zy's boot 271 already discarded
  run 1, and the 70-75 era is at least partly **a first-run transient**. What
  the transient IS remains unmeasured. 2A3's own warning about n=1 boots
  applied to itself one milestone later.
  **The default stays hold-ON: 2A6 reproduced the flicker (boot 287).** The
  boot-arg `-rbgpunofbnotready` (derived by `rb_off_token()`, honoured by
  `argOn()`) is how a boot runs without the hold, and `holdset 1` restores it
  live. 2A5's boot 287 ran the whole boot exposed and was clean for minutes
  before the hazard appeared, which is what a rare event looks like. `mean
  vblanks` 1.066 with the hold off (2A5) says the exposure window is at most
  ~6.9 ms - **much smaller than when 2xo was written, and not zero**, which is
  the honest reading of both the ~80 % the user reported and the flicker they
  still saw.
  **2A3: the A/B refused its own B phase, and it was right to (boot 285).**
  `holdset 0` set `amtrNotReady` and the REGISTRY went on saying `Yes`,
  because `RBFBAmtrNotReady` was published in `start()` - right for a
  boot-arg, wrong for a runtime dial, since `start()` runs once. **Rule 31
  with the terms swapped: a property that is never republished cannot move at
  all**, so reading it back only ever confirms the one-time publish. The
  general rule, new: **any field a runtime dial can change must be published
  everywhere it is read, and the periodic publish is the one that tracks.**
  Fixed in `publishVBLState()`, and **`holdset` now verifies from the registry
  after one publish interval instead of trusting `kern_return_t`** (which was
  SUCCESS and correct - it says the setter ran, not that the state is
  readable, rule 13) and exits 3 on a mismatch. `holdaba.sh`'s own
  read-the-property guard caught it and restored A, which is what it is for.
  **A1 (hold ON, the shipping configuration) is six runs at mean 92.94**,
  89.93-96.21, `fed` 62.6-65.9 %.
  **THE MACHINE HAS TWO STATES AND THIS IS NOT YET EXPLAINED.** boot 283
  93.95 / fed 65.6 (n=1), boot 284 75.72 / fed 52.8 (n=1), boot 285 92.94 /
  fed 62.6-65.9 (n=6), against 70-75 across the whole 2zb-2zz era - and the
  builds do not order it (2A0 fast, 2A1 slow, 2A2 fast, where 2A1 only added
  accumulators). `fed` tracks it while `ours` was 0.4 % on both measured
  boots, so whatever it is changes **how often the compositor hands us a
  transaction**, not how fast we handle one; the latch differed too (1.001 vs
  1.194 mean vblanks), which is the first place to look. **The question is no
  longer "what sets 73" but "what differs at boot between the two states" -
  and no model or dial is proposed from three boots, two of them n=1.**
  **And boot 283's 93.95 did not reproduce: boot 284 read 75.72, so the
  "smoother" desktop was boot-to-boot variance.** 2A1 recorded it as an
  observation and not a claim, which is the only reason it cost one window
  instead of a milestone. What DID reproduce is the split itself - **ours
  0.4 % on both boots** with frame rates 24 % apart, which makes it a two-boot
  claim: nothing this kext does inside a frame period is the ceiling. Also
  recorded, as two boots and therefore not a finding: `actual /
  latch_ceiling` was **63-66 % on both** while the ceiling itself moved 19 %
  (1.001 -> 1.194 mean vblanks).
  Counters live in `ioreg -rl -c RembrandtGPUPeer`'s `RBGPUDiag` (**decimal**);
  `nvram rbgpu-diag` omits them because the note caps near 520 bytes while
  `diag_used` 693 of 768 says the table is not full - absence from one is not
  absence from the other.
- **CONCURRENT QUEUES SERIALISE AT ONE PER VBLANK, AND IT IS NOT
  `checkTimestamps()` (2C2/2C3, boots 409/410).** `metal_probe --queues N`
  starts queue 0's first buffer at **13.3-14.5 ms** (n=12, tight) and every
  queue after it **+6.9 to +7.2 ms**, with the absolute slots landing at the
  same times for N = 2, 4 and 8 - a fixed lattice, not contention.
  `RBFBVBLPeriodNs` 6952507, so the spacing IS the vblank period;
  `vblret_src` 1 says the vblank owns retirement. **This is what "efficiency
  good, performance not" is**: one steady stream gets 144 retires/s, which is
  all the compositor wants (2A1: our whole cost is 44 us), and the price falls
  entirely on work needing SEVERAL units of GPU work finished before anything
  can be drawn - Settings' icon loads, menu blurs, animation first frames.
  2A1-2B7 measured the frame path exhaustively and never measured a SECOND
  concurrent stream.
  **The obvious fix is MEASURED DEAD.** `rbgpuretiredrain=` /
  `{"RBGPURetireDrain": N}` polls `checkTimestamps()` up to 8 times per offer;
  six interleaved `--queues 8` runs gave 55.295-56.308 ms (+-0.9 %) with
  `vblret_extra` **50897** proving the polls ran, and the one true drain=1 run
  sits inside that range. **Default stays 1. Do not propose polling it harder
  again.** The A/B degenerated - `drainset 1` returned SUCCESS three times and
  `vblret_drain` stayed 8, so the A side is n=1 - which is fatal to a positive
  claim and harmless to this negative one, since the runs that provably DID
  drain are the ones indistinguishable from the one that did not. That dial
  bug is **undiagnosed**: `drain_refused` absent, three writers and none
  writes 8, the code symmetric in 1 and 8. 2C3 instruments it rather than
  reasoning further (`drain_recv`, `drain_want`, `drain_sets` - rule 14).
  **NEXT, and it needs no boot after one install:** 2ww's unanswerable
  question is now takeable. `advance()`'s count only ever went to
  `rbgpu-sched`, which flushes at fixed phase marks - two reads three seconds
  apart both gave `adv=48183` - so it could never be delta-read. 2C3 publishes
  `adv_calls`/`adv_sub`/`adv_submax`/`adv_qids`/`adv_hk` periodically. Read
  `adv_calls` either side of ONE `metal_probe --queues 8`: **eight advances
  for eight queues** means one channel per advance and calling `advance()`
  directly is the lever (we already hold `orig_sched_advance_2wv`); **one or
  two** means `advance()` is not the gate and the serialisation is on the
  submit side, which no dial of ours reaches. `smax` 589 already argues
  against one-item-per-call. **Not inferred:** that the gate is
  `IOInterruptEventSource` signal coalescing - it would explain the null
  result exactly, and nothing has observed one call to `advance()` (rule 35).
- **The wedge**, an independent second defect: work submitted that never
  retires, on GFX/SDMA/compute user queues. **2wt's `--queues 4` trigger no
  longer fires** - 14-49 ms where it cost 6.1-10.2 s - but boot 409 logged a
  real one of its own: channel 52 GFX, stamp frozen at 0x27a1b,
  `GPURestartBegin` to `GPURestartEnd` spanning **3.5 s**. So it is rarer, not
  gone, and it is the "random" lag the user reports. **`stamp_ticks` 125666
  says our retire tick polled 125 thousand times while that stamp never
  advanced**, which retires the "nothing polls it" family for the wedge as
  2xe did for the display. Not the display blocker (2x0, rule 39).
- **A USB4 TYPE-C PORT ON THIS CHASSIS CARRIES DP ALT MODE FROM THE iGPU
  (user, 2026-11-02) - which the HDMI-A bullet below does NOT cover.** It is a
  different connector from the HDMI-A jack, so "HDMI needs hybrid mode" is a
  fact about that jack and not about display output in Eco mode. Plugged in
  live it is **black** and macOS sees one display
  (`system_profiler`: Color LCD only, Connection Type Internal). Two things
  are established and the third is not:
  **(1)** macOS has no USB4/Thunderbolt router here (`SPThunderboltDataType`:
  "No hardware was found"), and that does **not** block DP alt mode - the
  lanes are switched by the Type-C PD controller, not by an OS driver, so the
  only OS-side requirement is that something drives the GPU's DIG and a second
  pipe. **(2)** We drive exactly one head: `RBAMDFramebuffer` writes OTG0 and
  HUBP0 and nothing else, and `getDisplayModeCount()` has returned 1 since
  2sl, so a black external display is the **expected** reading whether or not
  the port is wired to the 680M. **(3) NOT established: whether the port is
  wired to the 680M's display engine in Eco mode at all** - rule 47 asked of
  a new output, and the `otg_ctl1..3` = 0x300 in `RBGPUDiag` **cannot answer
  it**, because `rbgpu_agdc_otg.cpp` samples once at init and that boot had no
  cable (rule 33: establish when a reading was taken).
  **2B3 BUILDS THE INSTRUMENT: the five HPD blocks, read-only.**
  `src/common/rb_hpd.h` - `regHPD0_DC_HPD_INT_STATUS` 0x1f14,
  `..._CONTROL` 0x1f16, stride 8 dwords, HPD0..HPD4, all **BASE_IDX 2**, the
  same base the OTG and HUBP reads already come from, so **no new mapping and
  no new write surface** and **no setter**. `DC_HPD_EN` (bit 28) is published
  beside the sense, because with EN clear SENSE reads 0 whatever is plugged in
  and "no sink" would be indistinguishable from "the instrument is off" (rule
  14, and 2zw's shape one level out). The aperture bound in `hubpSetup()` had
  to **grow** with it - HPD at 0x1f36 is above every register this kext
  previously touched - and is now one comparison against `rb_hpd_dw_max()`.
  Resampled every periodic publish so a hotplug needs no reboot, with
  `RBFBHpdSinkEver` ORing every sample (rule 31). **The reading is the
  DIFFERENCE**: `tools/hpdlook.sh` cable out, then cable in; the instance whose
  bit moved is the port, and one sample identifies nothing (rule 18) because
  the DCN-instance-to-connector mapping is in no header.
  Framebuffer **A5BDF3D2**.
  **2B4 ANSWERED IT: the Type-C port is HPD2 and IS wired to the 680M in Eco
  mode.** Three samples, one boot: HPD2's sink bit follows the cable (0x05 ->
  0x01 -> 0x05, `RBFBHpdChanges` 2) while **HPD0 - the internal panel - never
  moves**, which is the control, and all five blocks were enabled so every "no
  sink" is a real reading. **The opposite of the HDMI-A verdict below**: that
  jack is dGPU-wired, this port reaches the 680M with the dGPU off the bus
  entirely. HPD2 reads **0xff0ff013** - both sense bits plus
  `DC_HPD_INT_STATUS` **latched and unserviced**, which is correct since
  nothing in macOS knows the connector exists; HPD0 reads 0x14000012, the same
  sense bits with bit 0 clear. (`hpdlook.sh` printed that word as
  `0xffffffffff0ff013`: **ioreg renders a 32-bit OSNumber with bit 31 set as a
  sign-extended 64-bit decimal.** The verdict column was right because it
  comes from the kext's mask - 2B0's "publish the raw beside the summary"
  paying off in the other direction. Fixed at the source in 2B8 by publishing
  at 64 bits; **several other 32-bit keys have the same trap**.)
  **2B8 IS THE NEXT STEP AND IT IS A SURVEY, NOT A TRANSACTION.** A DPCD read
  would be **uninterpretable**: an AUX timeout is indistinguishable between an
  unconfigured engine, an unwired channel and a silent sink (rule 14, the gap
  2zw's six milestones lived in). So `src/common/rb_aux.h` reads the five DP
  AUX engines - `AUX_CONTROL` 0x1f50, `SW_CONTROL` 0x1f51, `ARB_CONTROL`
  0x1f52, `INT_CONTROL` 0x1f53, `SW_STATUS` 0x1f54, `SW_DATA` 0x1f56,
  `DPHY_RX_CONTROL1` 0x1f5b, stride **0x1c**, all **BASE_IDX 2**, so no new
  mapping - and **decides nothing else. There is no writer**: a transaction
  takes `AUX_ARB_CONTROL` on a block the **DMCUB also arbitrates for**, and
  taking that lock wrongly on the engine driving the live panel loses the
  display.
  **The reference instance is the instrument**: the firmware trained eDP on
  this die, so one engine is in a known-working configuration now, which beats
  any absolute judgement - **2zw's lesson applied BEFORE the fact.**
  `rb_aux_matches_ref()` compares `AUX_EN` + the DPHY receive timing and
  **returns 0 when the reference itself is not enabled.** Instance 0 is the
  assumed reference and every raw word is published, because the
  AUX-to-connector mapping is in **no header** - as the HPD one was not.
  **Rule 46 discharged in the resource file, not by reasoning**: these are
  **DCE's** registers on a DCN chip, the exact 2xs/2zl trap, and
  `dcn31_resource.c`'s `dcn31_aux_engine_create()` calls
  `dce110_aux_engine_construct()` with `AUX_COMMON_REG_LIST0(id)` - **DCN 3.1
  uses the DCE110 AUX engine verbatim.** That construct touches **no
  hardware**, so whatever configures these engines is elsewhere.
  **Bundled into the same build** (each needed one and none deserved its own
  boot): **`RBFBOtgLive0..3`**, re-read every publish - 2B5 found **both**
  earlier OTG readings are setup one-shots, so nothing could say whether a
  pipe lit with the cable in; **`RBFBAmtrNotReadyArmed` is now a real
  snapshot**; and the HPD raw words publish at 64 bits.
  `tools/auxlook.sh` reads it. Framebuffer **E24F30C2**.
  **2B9 ANSWERED IT, with the best of the four outcomes (boot 293).** AUX2 is
  **enabled with `dphy` 0x00016400, BIT-IDENTICAL to the panel's AUX0**,
  against 0x00004500 on the three never brought up. **`AUX_EN` mask 0x05 is
  the same pair as 2B4's HPD sink mask 0x05** - two independent instruments,
  neither derived from the other.
  **THE MAPPING WAS A FIELD ALL ALONG: `AUX_CONTROL.AUX_HPD_SEL` (bits 20-22)
  reads 0 on AUX0 and 2 on AUX2**, the only difference between those two
  control words. So the engine names its own connector, and 2B8's "the
  AUX-to-connector mapping is in no header" - written twice, with a
  cable-unplugging method built around it - was wrong; `rb_aux_for_hpd()` now
  looks it up. **Same shape as 2B1's triple-buffer bit: documented in a file
  already in the tree and never asked about. When a fact looks unobtainable,
  read the register definition before designing an experiment to infer it.**
  **AND A SINK HAS ALREADY REPLIED ON AUX2**: `AUX_SW_DONE` set with
  `AUX_SW_REPLY_BYTE_COUNT` **2 on AUX0, 1 on AUX2**. The AUX channel is proven
  end to end **without this project issuing a transaction** - the firmware
  probed that connector. **Past tense on purpose: the status is STICKY**, so it
  says the channel reaches a sink, not that the sink answers now (rule 33 of a
  status register). `rb_aux_sink_replied()` needs enabled + DONE + a non-zero
  count, and DONE with a zero count must NOT read as a reply.
  `arb` 0 on both (nothing holds the lock) and **OTG1..3 all 0x300 LIVE with
  the cable in** - answerable for the first time - so no pipe is lit and the
  pipe chain is entirely ours.
  **Every remaining unknown is DOWNSTREAM of AUX**: DPCD, EDID, link training,
  then OTG/HUBP/DIG and a second `IOFramebuffer`. Long but located.
  **And the next step's risk is better than it looked**: a DPCD read takes
  `AUX_ARB_CONTROL` on **AUX2**, an engine driving nothing this desktop needs,
  with the panel's AUX0 untouched. The DMCUB still arbitrates - now over a
  spare engine rather than the one holding the display up.
  **No rebuild was needed**: the raw words were already published, so
  `auxlook.sh` decodes it from the registry. Publishing raw beside summary paid
  for itself a third time (2B0, 2B4, this).
  **2BA IS THE FIRST WRITE OUTSIDE THE PANEL'S OWN PIPE (user's decision,
  2026-11-02): a native AUX read of the sink's DPCD.** Opt-in
  **`-rbgpufbaux`**, and even armed **nothing is sent until
  `build/out/dpcdread` names an address** (2zn's shape;
  `RBFBAuxDpcdAttempts` says so). What bounds it: it runs on the engine the
  **hardware** says is bound to the target HPD (2B9's `AUX_HPD_SEL`, default
  HPD2, so the panel's engine is never the target); **the arbitration is
  REFUSED, not raced** - `AUX_ARB_CONTROL.AUX_REG_RW_CNTL_STATUS` names the
  owner and we decline on `DMCU_CAN_ACCESS_AUX`, which is **2B8's reason for
  not writing turned into a readable field**; it is a native **READ**, so it
  cannot change the sink; every poll is bounded (600 x 10 us) and it runs from
  `setProperties`, never the frame path (rule 31 not engaged); and the release
  is unconditional on every path out, since an engine left with our REQ bit is
  one the DMCUB cannot take back. **A dark engine is refused, not enabled.**
  **Two of Linux's steps are skipped for a reason in the register list, not a
  judgement**: `AUX_COMMON_REG_LIST0` has seven registers and `AUXN_IMPCAL` is
  not one, so Linux's own `if (REG(AUXN_IMPCAL))` is false here; and AUX_EN is
  already set (2B9), so `acquire_engine()`'s enable path does not run.
  **THE ONE THING NOT REPRODUCED HAS A DETECTABLE SIGNATURE, and that is the
  design**: Linux reads `AUX_SW_DATA` in a loop expecting successive bytes with
  `AUX_SW_AUTOINCREMENT_DISABLE` **set**, and this project does not know
  whether the index advances anyway - so the sequence is transcribed exactly
  and `rb_dpcd_reply_suspect_norepeat()` flags an all-identical reply (what a
  stuck index looks like; a real DPCD 0x000 block differs byte to byte).
  `dpcdread` then **refuses to decode a suspect reply at all** - a plausible
  decode off a stuck index is worse than none - and `rb_dpcd_rev_plausible()`
  refuses a first byte outside DP 1.0-1.4 so register noise announces itself.
  **Every constant is read, not recalled** (the mirror of rule 17): DP_READ
  **0x90** and the ACK/NACK/DEFER codes from `dc_ddc_types.h` (vendored), the
  layout from `dce_aux.c`'s `COMPOSE_AUX_SW_DATA_*`, the masks from the sh_mask
  header. **The refusal is a CODE** (rule 14) - 6 is "the DMCUB owns it",
  3 is "no engine bound to that HPD", and a single bool would merge them.
  Caught while writing: the arming comment claimed the only shared prefix was
  `-rbgpufb`, but **`-rbgpufbaccel` exists**, so it is `-rbgpufba` diverging at
  u/c - rule 30 still holds, and **`rb_defaults.h`'s hosttest does not cover an
  opt-in token**, so it was checked by hand. Framebuffer **49174106**.
  **2BB ANSWERED IT: THE AUX CHANNEL WORKS. ACK, 16 bytes of real DPCD from
  the Type-C sink on boot 294** - `arb` 0x0, refusal 0, `SW_STATUS` 0x11000001
  (DONE plus a reply count of 17 = status + the 16 asked for, so the
  transaction is consistent before any byte is read):
  `12 14 c2 81 01 1d 01 81 02 00 04 04 3e 00 84 00`. **DP 1.2, HBR2
  (5.4 Gbps/lane), 2 lanes**, every offset and bit checked against
  `docs/linux-ref/drm_dp.h` (vendored, not recalled).
  **THREE BYTES ARE CONTROLS, which is why this is proof and not a plausible
  number.** `DP_EDP_CONFIGURATION_CAP` (0x0d) is **0x00 - NOT an eDP panel**,
  so this is not the laptop's own display answering, which was the single
  biggest way the experiment could have fooled itself; **2B9's `AUX_HPD_SEL`
  routing is confirmed by the sink's own answer.** **2 lanes is physically
  right for Type-C** (DP alt mode keeps two pairs for USB 3) in a way no
  misread field would land on. And `DP_LOCAL_EDID_PRESENT` (0x08 bit 1) is
  set, with 0x0c declaring the I2C-over-AUX speeds reading it needs - **the
  next step is named by the data, not guessed.**
  **AND 2BA's OPEN QUESTION IS ANSWERED BY THE SAME READING:** sixteen
  DISTINCT bytes came back, so `AUX_SW_DATA`'s index **does** advance on read
  even with `AUX_SW_AUTOINCREMENT_DISABLE` set - Linux's loop is right and the
  field is narrower than its name. The suspicion check passed, which is the
  good outcome: **an instrument built to catch one specific lie retired the
  uncertainty it was guarding.**
  **Bandwidth:** 2 x 5.4 = 10.8 Gbps raw, ~8.64 after 8b/10b; 1080p100 at
  24 bpp needs ~4.98 and fits, 1440p100 needs ~8.85 and would not. **The
  sink's native mode comes from the EDID and has NOT been read.**
  **What it does NOT establish: a trained link, an EDID, a pipe or a
  display** - each its own step. What it removes is every doubt upstream of
  them. **Next: EDID over I2C-over-AUX** (DDC 0x50), a different transaction
  type - `I2C_READ_MOT` and the middle-of-transaction protocol, which
  `dce_aux.c` has and `rb_dpcd.h`'s encoder already takes an action byte for.
  Also now worth reading: the extended receiver caps at 0x02200, since 0x0e
  bit 7 says they exist.
  **2BC BUILDS THE EDID READ (I2C-over-AUX, the MOT protocol). Hosttested, not
  run.** `auxTransact()` is now general - `dce_aux.c` branches on nothing but
  `is_write`, so one function serves the DP read and all four I2C forms, and
  `auxDpcdRead()` is a wrapper. The sequence: an **I2C WRITE with MOT**
  carrying the byte offset (**the only write to a sink this project makes**),
  **eight 16-byte READs** (16 is the AUX payload max), and **the last drops
  MOT** to end the I2C transaction - leaving it set holds the sink's DDC bus
  and the next reader finds it busy, so `rb_edid_read_action()` owns that and
  the hosttest pins every chunk. **A zero-length request is THREE bytes, not
  four** (`length = request->length ? 4 : 3`), which 2BA's DPCD-only encoder
  refused and which the MOT protocol needs; the general encoder also refuses an
  action with its low nibble set, since that nibble is the address's.
  **DEFER is normal**, bounded at 8 retries, with `RBFBEdidDefers` published so
  "worked after retries" is visible. **Two INDEPENDENT checks before anything
  is parsed**: the fixed header and the 128-byte checksum - a partial read can
  pass the header and fail the sum, and **128 zeros pass the sum and fail the
  header** - and `edidread` refuses to parse unless both pass. Published as one
  `OSData` blob, not 128 keys (rule 16: 128 more `setProperty` calls on Apple's
  thread would make the instrument the defect).
  **A hosttest case passed for the WRONG REASON and only the break-it check
  found it**: it asserted the length guard on a block an earlier case had
  corrupted, so it returned 0 for the corruption and deleting the guard changed
  nothing. **Rule 26 on a test: a case that passes for the wrong reason is not
  coverage.** **And 2B1's `-Werror` trap fired again** - the first break left
  `n` unused, the build died, and the `FAIL` grep came back empty, which reads
  exactly like "the test does not catch it". **Second instance; grep for
  `error` as well as `FAIL`, or read the exit status.**
  `build/out/edidread [block [hpd]]`, which derives the refresh from the
  timing totals (a wrong field then reads as an absurd rate, not a plausible
  one) and compares the mode's bandwidth against 2BB's measured link.
  Framebuffer **67F28153**.
  **2BD: THE EDID IS READ - "MSI MP2412C" off the wire (boot 295).** 128
  bytes, header OK, checksum OK, refusal 0, **`defers 16`** - the retry loop
  was **load-bearing**, and without it the read would have failed
  indistinguishably from an unreachable sink. The model string is unfakeable:
  the manufacturer is **5-bit-packed** in bytes 8-9 (0x3669 -> M,S,I), the name
  is in a **0xFC-tagged** descriptor, and the block **sums to zero**. Panel
  52 x 29 cm (~23.4 in), gamma 2.20, digital input - none needed, all agreeing.
  **AND THE 100 Hz MODE IS IN THE SECOND DESCRIPTOR, WHICH THE TOOL DID NOT
  READ.** It parsed only 0x36 and printed **60.00 Hz** - true of the
  *preferred* timing, read as the whole answer, on a monitor the user had
  already said is 100 Hz. All four slots: 0x36 = 1920x1080 @ 60.00 Hz
  (148.5 MHz), **0x48 = 1920x1080 @ 99.65 Hz (228 MHz)**, 0x5a = name,
  **0x6c = range limits, V 48-100 Hz, max clock 230 MHz**. **Three independent
  corroborations of 100 Hz** (the timing, `vmax` 100, and 228 under the stated
  230), and **5.47 Gbps against the ~8.64 2BB measured - it FITS**, so DP alt
  mode's lane count is not the obstacle. Fixed structurally: all four slots,
  the range-limits descriptor since it bounds the others, and a "fastest mode"
  line checked against the **measured** link. **A correct reading of ONE
  element of a list, presented as the reading of the list** - 2B0's bucket 3
  and 2A1's threshold are the same error about which quantity answers the
  question; this one is about which SLOT.
  Smaller, same breath: decoding by hand I **retyped** the dump instead of
  parsing it, dropped a byte, and got "V 0-48 Hz" and a 0.017 Hz descriptor.
  **The nonsense is why it cost nothing** - which is the reason every refresh
  is DERIVED from the totals rather than read. Do not hand-transcribe hex.
  **Established end to end now: wired (2B4), engine configured and bound
  (2B9), our own transactions carried (2BB), full capability and timing set
  readable (2BD). Every remaining step is OUTPUT-side**: link training, a
  second OTG and HUBP, a DIG encoder, and a second `IOFramebuffer` -
  `getDisplayModeCount()` has returned 1 since 2sl.
  **2BE SPLITS THE OUTPUT SIDE, AS A SOURCE READ WITH NOTHING MEASURED.**
  Asked whether DMCUB can train the link for us, the answer is no *and* the
  better half is yes. **Training is ours**: `link_dp_training.c` and
  `link_dp_training_8b_10b.c` contain **zero** `dmub` references, so the
  CR/EQ loops, the DPCD writes and the lane settings are driver-side - over
  the AUX channel 2BB/2BD already proved. **Every HARDWARE bring-up step is a
  DMUB command**, because `dcn31_resource.c:934` sets
  `.dmub_command_table = true` (and `:919` `.disable_dmcu = true`), so the
  atom BIOS table runs on DMCUB: `DMUB_CMD__VBIOS` (128) sub-types **0**
  DIGx encoder control, **1** DIG1 transmitter control, **2 SET_PIXEL_CLOCK**,
  **3** display power gating, **15** LVTMA. **The pixel clock this card calls
  "never located" is sub-type 2** - not verified on this chip, but no longer
  unlocated - and the inbox to reach all of it is the 2zn-2zu apparatus.
  **The cheapest next step is a READ**: `..._TRANSMITTER_QUERY_DP_ALT` (26)
  returns `is_usb`, `is_dp_alt_disable` and **`is_dp4`**, the chip's own view
  of the lane count 2BB measured as 2 from the sink. DMCUB 4.0.15.47 passes
  `has_query_dp_alt()` (which excludes only 4.0.0-4.0.10).
  **And the register path is the RISKY one, which inverts the usual
  preference here**: `RDPCSTX_PHY_CNTL6.RDPCS_PHY_DPALT_DISABLE` looks exactly
  like 2B4's and 2B9's read-only surveys, but Linux marks it "Legacy path,
  avoid if possible" and prefers DMUB to avoid **hanging the RDCPSPIPE** -
  rule 16 asked of a read. **Unestablished and needed first: which UNIPHY
  carries HPD2** (look for a register field before designing an experiment -
  2B9's lesson), whether the PHY is flagged `DP_IS_USB_C` at all (VBIOS
  connector data, not a register), and whether an inbox reply can be read back
  (2zs's round trip was a GPINT). `YELLOW_CARP_B0` is **0x20, an internal
  rev** - not our PCI rev 0x0a; do not conflate them.
  **2BF closes the first of those by a NEGATIVE: there is no HPD-to-PHY
  register.** Every `HPD`-named field in `dcn_3_1_2_sh_mask.h` is
  `AUX_HPD_SEL` (2B9's), the four `AUX_*_HPD_DISCON` controls, or a
  `DC_GPIO_HPD*` pad control; `dcn31_resource.c:1170` indexes
  `link_enc_hpd_regs[enc_init_data->hpd_source]` and `hpd_source` comes from
  the **VBIOS display-object table**, which is ROM, not a register. 2B9's
  method was right to try and the answer is no - **do not search for this
  again.** **Instead the chip names the port itself:** `QUERY_DP_ALT` takes
  `phy_id` IN and returns `is_usb` OUT, so a sweep of `phy_id` 0..5 identifies
  the USB-C PHY with no index-convention inference, carries its own control
  (the eDP panel is not USB-C, so all-set or all-clear across six ids is a
  refusal), checks `is_dp4` against the 2 lanes 2BB read from the sink, and
  **needs no display attached.** Its cost is that it PRODUCES on the DMUB
  inbox rather than reading a register, and the reply path is what 2zs never
  exercised (a GPINT, not an inbox reply) - a new surface in 2BA's sense, so
  the user's decision.
  **2C0 REVERSES 2BE's recommendation, and the evidence was already in the
  journal when 2BE wrote it.** Setting out to build the DMUB sweep, the first
  check was whether this firmware answers an inbox command at all - and
  **2zr measured that it does NOT**: boot 275's `QUERY_FEATURE_CAPS` came back
  byte-identical with `ret_status` still set, no payload. `QUERY_DP_ALT`
  replies by that same mechanism, so the sweep would have reproduced a known
  failure. **GPINT cannot substitute** - 45 `DMUB_GPINT__*` commands, none for
  DP alt. So the register it is, and the hang Linux warns of is conditioned on
  DMCUB not running, which is **measured false** here (2zs's round trip).
  **Rule 46 in the resource file a third time:** `dcn31_resource.c:86` includes
  **`dpcs_4_2_0_offset.h`** - DCN 3.1.2 uses **DPCS 4.2.0**, and `RDPCS`
  appears ZERO times in `dcn_3_1_2_offset.h`, so a guess at `dpcs_3_1_4` (which
  exists) would have been wrong. `regRDPCSTX0..4_RDPCSTX_PHY_CNTL6` =
  **0x2946 + i*0xd8**, BASE_IDX 2, five instances.
  **The register is richer in the way that matters:** `PHY_CNTL6` carries four
  per-lane `DP_TX{0..3}_{PSTATE,MPLL_EN}` pairs plus `DP_REF_CLK_EN`/`REQ`, so
  the panel's own PHY must read active - **a positive control inside the
  sample**, and an independent reading of the 2 lanes 2BB got from the sink's
  DPCD. **And poorer in one: there is no `is_usb` bit**, so `DPALT_DISABLE`
  clear is ambiguous between "active alt mode" and "not a Type-C PHY at all" -
  2zw's shape exactly, so the enum has an explicit `ALT_AMBIGUOUS` member and
  **the identification is a DIFFERENCE across a cable event** (2B4's method).
  Opt-in **`-rbgpufbdpalt`**, read-only, no setter; the aperture bound is
  deliberately NOT folded into `needed`, so an unarmed boot is unchanged and a
  too-small aperture fails only the PHY read, never the panel.
  `tools/dpaltlook.sh`. Framebuffer **13533FFA**. **Built and hosttested, NOT
  RUN** - no boot has taken a reading.
  **2C1 RAN IT (boot 297): it did not hang, and THE CONTROL PASSED STRONGLY.**
  PHY0 reads **0x00194444** - all four MPLLs on, **PSTATE 0** on every lane,
  ref clock **enabled and requested** - a PHY in full operation, which random
  memory and a wrong base or stride do not produce, so **0x2946 + i*0xd8 is
  confirmed on hardware**. Active mask **0x01**: PHY0 is the panel and nothing
  else is driven. The dark four sit at PSTATE 3 with MPLLs off.
  **`DPALT_DP4` is NOT an alt-mode field: it is SET on the eDP panel's PHY**,
  which is not Type-C and not in alt mode, and that PHY independently shows
  four MPLLs - so the bit reads "four-lane configuration" and is a second
  reading of the lane count. 2BB's 2 lanes is the Type-C SINK and is not in
  tension; that PHY is dark.
  **And the dark four are NOT alike - PHY1 `0x00023333` has `DPALT_DISABLE`
  asserted and NOT acked, PHY2-4 `0x00063333` have both.** `dpaltlook.sh`
  printed "dark" for all four because its state line let the POWER axis win
  and never reached the alt-mode axis - power and alt mode are independent,
  `rb_dpalt_verdict()` gets it right, and **the tool had reimplemented the
  decision in a shell heredoc instead of calling it**, so hosttested logic was
  correct while the thing a human reads was wrong. Both axes printed now.
  Also fixed: `transitions` read **1 with no cable ever attached**, the
  counter recording its own first sample.
  **NOT identified and not guessed: which PHY is Type-C.** PHY1 is the odd one
  out, but 2B4 put Type-C on HPD**2** and 2B9 on AUX**2**, so the numbering
  does not line up - either PHY instances are numbered independently or PHY1
  differs for an unrelated reason, and one sample cannot say (2BF declined
  exactly this index inference). **The baseline is half a measurement; the
  cable-in run is the other half**, and it is usable because the no-cable
  state is NOT uniform. Framebuffer **5AB98303** (counter fix); the tool fix
  is userspace only.
  **The free measurement is PRE-BOOT**: plug the display in before power-on
  and watch for the OpenCore picker / Apple logo on it. The firmware's GOP
  drives whatever it is wired to, so an image there says the port reaches the
  680M in this mode **and** that the firmware trained the link - which is the
  whole reason the internal panel was tractable (2wh wrote HUBP surface
  addresses onto a pipe the firmware had already lit, and never trained
  anything). No image pre-boot means link training is ours, which is DP AUX,
  EDID and `dc_link`'s DMUB-assisted sequence - the largest single piece of
  work left in the display path. The same boot reads `otg_ctl1..3` with the
  cable present, which is the instrument we have.
- **HDMI is the full project, and on THIS chassis it is mutually exclusive
  with brightness (2xq/2xr, settled by 2zx).** The physical HDMI connector is
  wired to the **dGPU** entry, so it works in hybrid and dGPU-only mode and
  **cannot** work in iGPU-only mode - 2xq was right about this machine, and
  2zw's reframing of it as "a fact about the MUX mode" was wrong. Brightness
  needs Eco mode, HDMI needs hybrid, so today it is one or the other. The only
  part of 2zw that survives is that the arrangement is **per-chassis** and
  another laptop may differ, which is why `blset` reports what it finds
  instead of assuming this one.
  **A dGPU driver cannot help in Eco mode**: Eco removes the dGPU from the PCI
  bus entirely (measured, boot 282 - zero `0x10de` devices), so there is
  nothing to bind to. In HYBRID mode it could, and the right shape is not the
  "wake the firmware only" idea: if the port belongs to the dGPU and the dGPU
  has a driver, HDMI is **that driver's** job as a second GPU, with our
  framebuffer keeping the panel - the ordinary multi-GPU case, no
  reverse-PRIME path of ours. **NullMoth's `nvidia-macos-driver`** (README and
  press only, source NOT read) is Turing-Blackwell on NVIDIA's own open GPU
  kernel modules r610 with unmodified GSP firmware, Metal via AIR -> SPIR-V ->
  NVK. Blockers: it targets **macOS 15 Sequoia with no Tahoe claim** and this
  machine is 26.7.2; **Ada is pending validation** (validated on an RTX 5060,
  this chassis is an RTX 4050). Its licence is source-available and
  **non-commercial, not open source** - **read the LICENCE before reading the
  code**, which is why the ChefKiss rule below exists.
- **Every Apple address in a COMMENT here and in the journal is a macOS 15
  address.** `AMDRadeonX6000MTLDriver __TEXT` 0x7FFB08BF3000 →
  **0x7FFB110D4000**; CoreDisplay 0x7FF80404C000 → **0x7FF805348000**;
  SkyLight 0x7FF807B42000 → **0x7FF8091F1000**. Re-derive before any static
  read: **`tools/re/dscbase.sh <Lib>`** reads the live unslid `__TEXT` base and
  size out of `dyld_info -segments`, and dscsym.sh / dscinv.sh take their bases
  from it instead of a table (2xz - the table was stale and both tools reported
  confidently anyway). In LIVE code the only hardcoded Apple address left is
  `RBGPU_IOP_SITE_VM`, stale and failing safe (2t7 won that gate unpatched, so
  it is not load-bearing); everything else is a symbol lookup or a cave-table
  row.
- **ASan is broken on CLT 16.4 + macOS 26**: a two-line `printf` program
  SIGILLs. The Makefile probes it and degrades loudly (`SAN=auto`); every
  assertion passes unsanitized. Never commit a change whose only passing run
  was unsanitized. Rule 16.

**Closed — do not re-open:** page flipping without new evidence (2pf: with
`-rbgpuflipprobe` routed, `getDisplayPipeTransactionFlip` was never called
once); forcing `UseIOPresentment` (2ql: 72 call sites, it selects the whole
IOPresentment world); the AGDC capability surface, every type eliminated by
measurement (2sd/2v6/2w1); the plane-count line (2u3/2v9 both directions);
`System Gamma - Unknown`, which was never ours to answer (2v8); tiling writes
(2wi: the swizzle already agreed); `-rbgpuagdcearly`, which wedges (2rz).

## Layout

- `src/common/` — freestanding C shared by the kext, the plugin and the host
  tests: IP discovery (`rb_discovery.c`), read-only probe (`rb_probe.c`),
  registers (`rb_regs.h`, every offset cites its Linux header), SMU
  (`rb_smu.c`, `rb_smu13.c`), OTG timing (`rb_otg.h`), AGDC (`rb_agdc.h`),
  HUBP decisions (`rb_hubp.h`), backlight (`rb_backlight.h`), **the default-on
  token table (`rb_defaults.h`)**. Pure logic lives here so the hosttest can
  decide everything the kext decides.
- `src/kext/` — `RembrandtProbe`, read-only, publishes `RBProbe*` and unmaps.
- `src/fbkext/` — `RBAMDFramebuffer`, the `IOFramebuffer` for the firmware-lit
  panel: delivers `'vbl '` from a `thread_call` at the measured refresh,
  answers `'amtr'`, writes HUBP0's surface address per flip. `make fbkext`;
  **installed by the user** into the auxiliary collection.
- `src/plugin/` — the Lilu plugin: `kern_start.cpp` (hooks, probes, the
  swizzle patcher), `RembrandtGPUPeer.cpp` (the IOService peer, the diag
  table, the NVRAM hot keys, the default-on gate and the sentinel),
  `rbgpu_agdc.cpp` (the AGDC bridge), `rbgpu_agdc_otg.cpp` (the read-only BAR5
  OTG sampler). Boots from the OpenCore EFI.
- `tools/hosttest/` — the probe against a simulated chip plus parser fuzzing.
  `make kext` runs it first; never load a kext whose hosttest fails. Add a case
  whenever `src/common` changes, and check the new case fails when the code it
  guards is broken.
- `tools/re/` — static analysis. Kexts: `kc_extract.py`, `machoread.py`,
  `dumptable.py` (the instrument that works on a KC vtable), `kc_stubs.py`,
  `vtslots.py`, `strxref.py`, `switches.py`, `ptrsym.py`, `vcall.py`,
  `fldstore.py`. dyld cache: `dsc.py`, `dyldcache.py`, `dscinv.sh` (build this
  first — an `atos` grid names every function in seconds), `dscsym.sh`,
  `dscdis.sh`, `ripxref.py`. Output to `build/re/`, findings in
  `docs/APPLE-STACK.md`.
- `tools/xhci_intr0.py`, `tools/re/xhci_intr0_derive.py` — the USB
  interrupter-0 patches (2xv), tested by `tools/xhci_intr0_test.py`.
- `tools/earlybench.sh` — a `RunAtLoad` daemon that benches the GPU in the
  +0..+20 s window no terminal reaches. ONE queue, never `--queues` (a known
  wedge trigger). The user installs it.
- `docs/linux-ref/` — unmodified Linux files the code follows; `UPSTREAM.md`
  records each commit.

## Workflow

- `make kext tools`, `make fbkext`, and **`make plugin` for the Lilu plugin —
  `make kext tools` does NOT rebuild it.** CommandLineTools only, no Xcode.
- **Always check the UUID moved** (`dwarfdump --uuid build/out/...`). Two boots
  were lost to a stale build - and 2y3 found `make fbkext` printing `KEXT` over
  objects it had not recompiled, because the Makefile's `-include` of the
  depfiles never named `FBKEXT_OBJS`: for every milestone before 2y3, a
  src/fbkext object rebuilt only when `RBAMDFramebuffer.hpp` changed, so a
  `src/common` header edit alone rebuilt **nothing**. Fixed, and verified in
  both directions. The UUID check is what caught it, which is why it is a hard
  rule and not advice.
- **And for `src/fbkext`, check the UUID inside the COLLECTION, not in
  `/Library/Extensions`** — they are different facts and three boots went to
  the gap (2xy):
  `kmutil inspect -B /Library/KernelCollections/AuxiliaryKernelExtensions.kc
  --show-kext-uuids | grep rembrandt`.
- **Installing the framebuffer is ONE command: `sudo tools/fbinstall.sh`**
  (2zt). It reads the built binary's UUID, stages into `/Library/Extensions`,
  builds the auxiliary collection to a **scratch** path first, refuses unless
  that collection actually carries the UUID, backs the live one up, writes it,
  and verifies the UUID **inside the live collection** - restoring the backup
  if it is absent. Both expensive failure modes below live in the gaps between
  the hand-typed commands it replaces, which is why it is a script and not a
  checklist.
- **`kmutil install --update-all` is the WRONG TOOL and cannot work on this
  machine.** It rebuilds the boot and system collections as well, which needs
  a kernel variant and a KDK this machine does not have, and it dies there
  *before* reaching the auxiliary collection:
  `failed to build release collection: Unable to resolve dependencies:
  'com.apple.driver.SCSIDeviceSpecifics' names a dependency on
  'com.apple.kpi.mach', which was not found` plus four
  `Could not find: kernel mach-o at /System/Library/Kernels/kernel.<variant>`.
  Build **only the auxiliary collection**, linking against the boot and system
  collections that are already there: `kmutil create -n aux -a x86_64
  --allow-missing-kdk -B <BootKernelExtensions.kc> -S
  <SystemKernelExtensions.kc> -A <AuxiliaryKernelExtensions.kc> -r
  /Library/Extensions`. **`--allow-missing-kdk` is required on `create` too**,
  and without it the command prints `Missing Developer Kit`, writes nothing and
  **exits 0** — so check the output file, never the exit code (rule 26).
  Point `-A` at a scratch path first: it needs no `sudo`, costs nothing, and
  `kmutil inspect --show-kext-uuids` on the result settles it before the live
  collection is touched. Back the live one up; it is what the next boot loads.
- After any plugin change: `kmutil libraries -p build/out/RembrandtGPU.kext`
  must resolve everything with **0 from `SystemKernelExtensions.kc`** (the 2oz
  trap) and `grep -c IOGraphicsFamily src/plugin/Info.plist` must be **0**.
  The plugin is injected into `BootKernelExtensions.kc`, where a system-KC
  dependency cannot bind. **`src/fbkext` is the opposite** — loaded from the
  auxiliary collection, it *requires* `IOGraphicsFamily` out of
  `SystemKernelExtensions.kc`, and `make fbkext` fails if it is absent (2sl).
- Probe rules: no writes except `MM_INDEX`/`MM_INDEX_HI` (enforced in
  `rb_probe.c`, asserted by the hosttest), never enable PCI decoding or bus
  mastering, never map BAR0's first 16 MB, read only inside the carve-out.
- SMU rules (`make kext SMU=1`): writes only to MP1 C2PMSG_66/82/90 of an MP1
  13.x, only allowlisted query messages, nothing sent unless the response
  register reads 1, stop at the first non-OK answer.
- Register gates follow Linux's **version grouping**, not the exact version:
  this chip reports MMHUB 2.4.0 (`mmhub_v2_3`) and DMU 3.1.3
  (`DCN_VERSION_3_1`, `dcn_3_1_2`). Check the grouping before adding one.
- Captures land in `logs/<timestamp>-plugin/`.
- **`make clean` deletes `build/re/` and every extracted Apple binary with
  it** (`rm -rf $(BUILD_DIR)`). They are slow to rebuild - `kc_extract.py` over
  a 369 MB system collection - and their absence looks exactly like a finding:
  a `strings`/`grep` over a path that no longer exists returns nothing (2xy,
  caught only by rule 26's control). Re-extract before trusting a static
  search that comes back empty.

## Hard rules

- **Always ask before loading or unloading a kext.** A bad load can hang or
  panic the machine and ends the session.
- The user runs `sudo tools/load.sh`, `sudo tools/unload.sh`,
  `sudo tools/fbload.sh`, `sudo tools/efi.sh …` and `tools/plugin_check.sh`.
  **the assistant never runs these and never asks for the password.**
- **Never install into the EFI or `/Library/Extensions`.** the assistant builds and
  hands over the commands.
- **Never run `kmutil unload` on the framebuffer kext — it PANICS** (2sq):
  `IOFramebuffer::stop+0x48` reads `*(this->0x1c8)`, NULL on an instance that
  never completed configuration. To remove it, reboot.
- Before any load, **commit** so a hang loses nothing. the assistant may commit at any
  time without asking (2026-10-04), and **pushing no longer needs asking
  either** (user, 2026-10-30: "also push whenever u need").
- **Deliberate diagnostic panics are off the table** (user's decision,
  2026-10-12).
- **Adding an SMU message group is the user's decision** — say what it changes
  on the chip.
- **NootedRed and NootRX are under the ChefKiss Open Knowledge License v1,
  whose §2 and §3 forbid AI reading, summarizing, analyzing or deriving output
  from the work. Do not read or analyze them.** Findings from those projects
  must come from the user in their own words.
- **Note for any session in the the assistant app:** it runs with
  `DYLD_INSERT_LIBRARIES` set and every shell inherits it. Launch anything with
  `env -u DYLD_INSERT_LIBRARIES` or it starts under whatever patch this the assistant
  is running (2ns).

## Rules that cost boots to learn

Each was paid for on hardware; the tag in brackets is the journal's evidence.
The numbering is cited all over the journal — **keep the numbers**.

**Routing Apple's code**

1. Never route a name unless `nm -m` shows it **defined** there; `solveSymbol`
   returns the bare slide for an import and `routeMultiple` writes a jump
   there. [2c]
2. Refuse a target whose first five bytes contain a rel8/rel32 branch — Lilu
   copies displaced instructions byte for byte. [2gb, 2xu]
3. Grep the mangled symbol across the plugin first: a second `RouteRequest`
   for a routed function **panics**. A new transform goes inside the existing
   hook. [2kn]
4. **`RouteRequest::from` is NOT written back** by Lilu 1.7.3 though the header
   says so. Call `solveSymbol` yourself. [2kp]
5. Cross-check hand-written mangled names against `nm` — a wrong length prefix
   silently unscopes the fix. [2kr, 2s2]
6. When a fix moves upstream, **remove the downstream patch in the same
   change**. "Redundant" and "inert" differ. [2kv]
7. vtable dumps: **call offset = dump offset − 0x10**; a non-virtual call
   through a vtable *symbol* uses dump offsets directly. [2gp]

**Reading a boot**

8. **Read `rbgpu-boot` first.** No increase means the boot was never armed and
   nothing in NVRAM is this boot's. Boot numbers are the nonce or nothing.
   [2iz, 2wl]
9. One-shots are one-shot — **and since 2xw there are none left to arm**;
   `rbgpu-*=0` is a durable disarm and the sentinel is the recovery. The
   surviving lesson: a feature needs a boot-arg *and* a key only when a stuck
   key must be harmless. And when you invert what a key MEANS, ask what values
   your own earlier builds left in it — `rbgpu-tick` and `rbgpu-fb` held the
   `"0"` the old consume-by-overwrite path wrote, and the inversion read it as
   the user's disarm. [2iz, 2qy, 2xw, 2xx]
10. **Capture in the armed boot, over SSH, before rebooting.** What must
    survive an uncapturable boot has to be in NVRAM and **periodic**, not a set
    of fixed marks. [2jg, 2gw, 2s5, 2wl]
11. A note is evidence only when its `b=` nonce is new. [2gd]
12. A stale note says nothing about where a boot got to. A user rebooting a
    black screen looks exactly like a hang. [2gw]
13. **A return code is not a result until the code that sets it has been
    read** — request 0x1B's *success* path returns `kIOReturnUnsupported`.
    [2ha, 2s2]
14. **Absence is not a result.** A silent return, a full diag table and a
    missing write look identical. Every path out of a probe must say
    something; check `diag_used` against `diag_max` first; a last-wins key
    cannot express a per-entry fact; and the absence of a log means nothing
    until **every** exit has been enumerated. [2eh, 2ds, 2qw, 2s6, 2w7]
15. Check the loaded UUID, the keys and the boot-args **for every artifact the
    boot loads** — the plugin from the EFI, the framebuffer from
    `/Library/Extensions`. A rebuild refreshes neither. A key the new code
    publishes *unconditionally* is the cheapest proof the new binary is
    running. **2xx is the expensive case: nine boots, a WindowServer crash
    loop and a nine-stage boot-arg bisect, all because the plugin was
    reinstalled every time and the framebuffer never was.** Every "cleared"
    result in that bisect was taken against a two-milestone-old second binary.
    `kextstat | grep rembrandt` against the UUIDs the build printed is the
    first reading of any boot, not an afterthought. [2cu, 2w6, 2wa, 2xx]

**Instrumenting**

16. **The instrument is a suspect.** It has cost a wedge, a crawl mistaken for
    a hang, two panics, three destroyed captures and a build that looked like a
    failing test (ASan). When a measurement and a suspect are both new, remove
    the suspect rather than reasoning about it. [2fo, 2ez, 2rh, 2xt]
17. **Never substitute a value and then narrate it as a hardware result.**
    [2cy, which cost fifteen milestones]
18. Do not build a table from n=1 rows, and two numbers agreeing to one figure
    is not an identification. [2ev, 2wt]
19. A NULL check is not a pointer check, and use the right floor —
    `0xffffff8000000000` for a kalloc'd object, **`0xffffff7f80000000`** for a
    kext symbol. A refusal path must publish the value it refused, and a
    captured pointer needs a captured control pointer in the same boot. [2gh,
    2qu, 2s6]
20. A registration that reports success is not evidence that a patch applied.
    Log the enabling field itself. [2lq]
21. **Never put `DYLD_INSERT_LIBRARIES` in a launchd environment**, and test a
    recovery procedure in the situation it exists for. Inject per process.
    [2mq]
22. **Lilu's user patcher does not run on this OS at all** —
    `isUserDisabled = … || getKernelVersion() >= BigSur`. Check a subsystem is
    *enabled* before reading how it behaves. Verify a patch from the target
    process's own memory. [2nn, 2of]
23. `pgrep` cannot see the process a script runs inside, or its ancestors; use
    `ps -axo pid=,command=` with a prefix match. [2nu]
24. The dyld shared cache is inside a grafted, Apple-signed cryptex and cannot
    be modified. [2ny]
25. **Read the machine, not the manual.** Four Recovery trips died on things
    asserted from general macOS knowledge. [2ny]
26. **A search is an instrument too** — a grep returning nothing proves nothing
    until checked against a known-present case. Paid for in: `objdump`'s tab
    separator; `strings -a` under-reporting on a 373 MB KC; `otool -tV`
    resolving names so a numeric search misses; a tail `jmp` invisible to a
    `callq *0x…(` grep; `ioreg -c X -k Y` not filtering (use `ioreg -rl -c X`);
    a tool's exit code read as its verdict, and the mirror — a loud error over
    a write that landed; **`timeout` does not exist on macOS**; a word the
    producing code never contains (`Clean`, five boots); a constant the
    assembler built by **arithmetic**; a `sort -u` read as a count; and **a key
    name that is a prefix of another key's name** (`rbgpu-diag` matching
    `rbgpu-diag-prev`). Anchor the match or read the value, never the file; and
    xref a string's **start**. **And the NVRAM diag table prints every number
    as hex** - until 2zc with no `0x`, so `tick_us=3640` was 13888 and looked
    exactly like a refused parse, and 2z4's `pg_ticks=24` was 36. Ask what base
    a number is in before reading it. [2v8, 2wc, 2wg, 2v9, 2zc]
27. **No vnode work at plugin init** — the root filesystem is not mounted yet.
    `-rbgpuoff` stops `init()` entirely, so such a boot is not a control for
    it. [2oe]
28. Attribute every log line to a boot by timestamp **and** pid; pids are
    reused, so the reliable separator is the `Darwin Kernel Version` banner.
    `log` is a zsh builtin — use `/usr/bin/log`. [2q0, 2s1]
29. **Derive a kernel slide from a pointer you solved yourself, never from
    `kextstat`** — off by `0x15a00000` in one boot. [2s6, 2s7]
30. Boot-arg names: the kernel's parse is **exact** (`token_len ==
    strlen(name)`), our shell greps are not. Test a whole token
    (`tr ' \t' '\n\n' | grep -qx`), and do not name an arg so it prefixes an
    existing one — `rb_defaults.h`'s hosttest now asserts that for the derived
    negatives. [2ss, 2xw]
31. **`IOLog` is not an instrument** — in `src/fbkext` or in the plugin.
    32 554 kernel-sender lines reached one boot's log and not one was ours.
    `setProperty` it and read `ioreg -rl -c <class>`, or publish a diag key.
    **And what you then read is published PERIODICALLY, so it is stale by
    design: a counter that has not moved YET is not a counter that did not
    move.** Read it twice, or after at least one publish interval, before
    "it did not change" is a result — boot 249 had `RBFBBlSets` still reading
    1 immediately after a write that had in fact landed, and it read 2 seconds
    later. [2sv, 2xm, 2xs]
32. Read a `char*[]` table's **length** table too, and check the slot count
    against the bound in the code. An enum value of 0 is a value; an unwritten
    field is an absence that looks the same. [2v8]
33. **Before reading anything from the live machine, establish which boot it
    is** — `kern.boottime` against the capture's, `rbgpu-boot` against the last
    nonce. And **an index is not a count**: name the object a number belongs to
    before retiring a line with it. [2v9, 2u4, 2wd]
34. An arm instruction must list every one-shot the experiment needs and the
    reader must check each before interpreting anything. 2xw removed the
    one-shots; what remains is to **say which keys and args the experiment
    depends on** and verify them first. [2w0]
35. **When a static chain ends at our own code, instrument our end first.**
    2v9 disassembled five layers correctly and attached them to the one link
    that was fine. [2w1]
36. A log capture has a **level**, a **predicate** and a **window**, and all
    three silently drop the answer: `log show` omits INFO without `--info`,
    INFO does not survive a reboot, FAULT does, and matching one function name
    instead of the **sender** hid 213 lines including the wedge. [2u5, 2w2,
    2w4]
37. **A feature must stay disarmable from the machine it runs on.**
    `removeProperty` reaches no sync; `setProperty` does. Consume by
    **overwrite** on a path measured to persist, and ask what recovery looks
    like when a feature works *and* when it half-works. [2sq, 2wj]
38. **Two halves that are separately armed must check each other.** The
    framebuffer writes HUBP from `setAttribute('amtr')` and nowhere else, so
    the panel taken with no accelerator underneath freezes on the last frame —
    guaranteed. Since 2xw this is a check in `armedOnce()`, not a comment.
    [2wp, 2xw]
39. **A measurement of a defect taken in a configuration that does not exhibit
    the defect does not locate the defect.** Say which configuration a reading
    comes from, and whether that configuration has the symptom, before
    promoting it to a frontier. An instrument that needs its own load must
    publish the load beside the result. [2wy, 2x0]
40. **Boot-arg VALUES come back as a binary int, not ASCII.** The correct idiom
    is `int v = 0; PE_parse_boot_argn(name, &v, sizeof(v))`
    (`rbgpu_boot_argn_u32()`). A default that reads correctly is not evidence
    that the tunable works. [2xm]

41. **A constant is only a constant on the OS it was measured on - and an
    instrument that re-measures something every run can still be subtracting a
    stale base.** `dscsym.sh` measured the shared-cache slide on every single
    invocation, said so in its own header comment, and then subtracted a macOS
    15 SkyLight base from it, so `atos` answered for an address 0x16AF000 away
    from the one asked about and never once said anything was wrong. The fix is
    structural, not a corrected number: derive the base too
    (`tools/re/dscbase.sh`), and for anything that cannot be derived at use
    time, record PER OS whether it was measured there and refuse to use it
    where it was not (`src/common/rb_osver.h`). Ask of every written-down
    address: **which OS was this measured on, and is that the one running?**
    [2xz]

42. **A collection you did not allocate is not yours to write to.**
    `getProperty` returns a borrowed pointer to something the registry owns,
    and IOKit enforces that with a **panic**, not a return code — so there is
    no failure path to test and no counter that can record it, and the first
    wrong write ends the boot. Ask of every `OSDictionary*`/`OSArray*`/`OSSet*`
    before writing: **who allocated this?** "I fetched it from a service" means
    copy it first (`OSDictionary::withDictionary`), mutate the copy, then
    `setProperty` the copy. The two pointers look identical and the immutable
    one is indistinguishable by value. The mirror of rule 13: not every call
    that can fail reports failure. [2y1]

43. **A value you read back is not a value that SURVIVES.** `nvram boot-args`
    accepts a write, reads back correctly, and is then **overwritten from
    OpenCore's `config.plist` on the next boot** - so four boots (260-263) were
    spent arming `-rbgpucgcg`, `-rbgpupg` and `-rbgpustock` and not one was in
    effect on the boot that followed. The verification step made it worse:
    reading the token back before rebooting passed every time and manufactured
    confidence, because it is a true statement about the current value and says
    nothing about the next boot's. Boot-args on this machine are changed with
    **`sudo tools/efi.sh bootarg add|remove ARG [ARG...]`** (2xx built it to
    take several at once), never with `nvram`. Rule 34 asks you to verify an
    experiment's arm; verify it **after the boot, from the running system**,
    which is the only side that matters - and ask of any value written for a
    future boot: **who else writes this, and when?** [2z7]

44. **A measurement needs enough samples to resolve the difference it claims,
    and "enough" is counted in BOOTS, not rows.** 2ze: ten runs at 143.85 Hz
    on ONE boot gave 63.98-100.77, mean 73.1, median 70.0 - so one 20 s sample
    cannot resolve under ~20 %, and the first run of that boot read 100.77 and
    would have recorded a feature as a 38 % success on a boot whose own
    counters said it never ran. The prescription that followed ("n>=6 per
    side") was then ALSO wrong: 2zg found boot 267's six internally consistent
    samples (78.14-94.90, mean 90.4) irreproducible, boot 268 giving 72.50
    (n=12) in the same configuration with the gate provably inert.
    **Boot-to-boot variance exceeds within-boot variance**, so six samples
    inside one boot measure that boot. A configuration claim needs **at least
    two boots per side**, or an A/B inside one boot. Rule 18 applied to boots
    rather than rows - and the variance itself can be the finding: the two
    timers beating were the noise source, so the same change that bought no
    mean bought +-3.5 % where it had been +-20 %. [2ze, 2zg]

45. **A chain of verified links is not a verified chain.** Brightness had
    four milestones, every link checked - the arithmetic two independent ways,
    the floor, the `AppleBacklightDisplay` match, the registry-collection
    panic, the `linear-brightness` identification - and the **last** link, the
    panel actually changing, was never instrumented. 2xs even stated the rule
    it was about to break ("a no-op round trip proves the store executed and
    nothing about the panel changing... which is why the effect needed its own
    instrument") and then named `blset` as that instrument, which measures the
    API return code. The only panel evidence in the whole chain was one
    unprompted user sentence on boot 253, and boot 270 does not reproduce it.
    When the deliverable is something a **human observes** - a panel, a sound,
    a frame - that observation is a measurement like any other and needs its
    own instrument, control and repeat. Ask which link in the chain the user
    can actually see, and whether anything measures *that*. [2zj]

46. **Check the version grouping of the METHOD, not just of the offsets.**
    2xs transcribed `dce_panel_cntl.c`'s direct `BL_PWM_CNTL` write onto a
    DCN 3.1.2 chip; 2xp then verified all four offsets against
    `dcn_3_1_2_offset.h` and they were **right**, which is exactly why it
    stood for four milestones. On this generation `dcn31_panel_cntl.c` has no
    `.set_backlight_level` at all and the level goes to
    `dmub_abm_set_backlight()`, so the register exists at the right address
    and is the wrong control surface. A register that is real, writable and
    reads back can still have a different owner - here the DMCUB, which reads
    it at `HW_INIT` and reports it through a query rather than polling it.
    Ask which Linux file the *function* comes from and whether this chip's
    `dc` would call it: `dce_*` vs `dcn31_*` is not a naming detail. [2zl]

47. **Before concluding that a register is the wrong control surface,
    establish that the output is WIRED to this chip right now.** On this
    chassis the MUX routes the eDP panel *and its backlight* to the dGPU in
    hybrid mode, so the 680M's `BL_PWM_CNTL` was a real, writable, perfectly
    functioning register attached to **nothing** - and a write that lands,
    reads back bit-identical and is contested by nobody is the **expected**
    reading of a working block with nothing attached to it. It is
    indistinguishable from success at the register and from failure at the
    panel, which is why 2zj's strongest-looking evidence (`RBFBBlLiveDiffs`
    pinned across 40 s, "so nothing else writes it") was actually the
    signature of a disconnected block. Six milestones, ten boots and three
    hypotheses went into the gap between "the chip accepted the value" and
    "the chip drives the thing", and the fix was a firmware setup menu. Ask it
    of any output - display, backlight, audio codec, USB port. On a laptop
    with switchable graphics the answer changes without a line of code
    changing, so **the firmware setup mode is part of the configuration a
    reading comes from** - rule 39 one level out from software. [2zw]

**The recurring error, stated once.** Thirty-odd recorded instances of reading
a value without first establishing what makes it meaningful. The practice that
keeps catching it: **say in the code what would have to be true for the reading
to mean anything, and log it when it is not.** Two variants: **read the
evidence already in hand before designing the next experiment** (2kx/2la/2lc
tested something a photograph had already excluded; 2s5 found the answer in
nine captures nobody had read together), and **find the consumer before writing
to a field** (2q9/2qa, which saved a boot twice over in 2s3).

## Chip facts established on hardware

- **GC seg0 = 0x1260** (four independent ways), seg1 = 0xa000. Apple's encoded
  register ids are `(dword offset << 32) | 11`, 11 = GC_HWID; byte address is
  `(seg + dword) * 4`. Offsets from `docs/linux-ref/gc_10_3_0_offset.h`.
- Accessor slots on `pm4+0x30` / `hw->0x368`: `[0x148]` id → dword offset,
  `[0x118]` **read**, `[0x120]` **write**. [2fd, 2fg]
- Carve-out MC `0xF4_0000_0000`, system-physical `0x7_8000_0000`
  (`FB_OFFSET` 0x780), BAR0 `0x8_8000_0000`, and **the GOP framebuffer is BAR0
  offset 0** (2pr). Apple's GPUVM space is `0xFF_BF..`.
  **`mc2pa(mc) = mc - vram_start + vram_base_offset`** — the missing term that
  was the whole CP halt.
- **One** SDMA instance; Apple's Navi 21 path builds four (`hw->0xc2 * 2 + 2`).
  The engine array is `hw + 0x3b0 + engine*8` (`[0]` PM4/GFX, `[1..4]` SDMA);
  the SDMA walk bases at `hw + 0x3b8`. [2gc, 2gi, 2gu]
- `GB_ADDR_CONFIG` is dword `0x13de` BASE_IDX 0, reads **0x242** (4 pipes,
  4 packers, 1 SE).
- `AddrSwizzleMode`: 0 LINEAR, 9 64KB_S, 22 4KB_D_X, 25 64KB_S_X, 27 64KB_R_X.
  The `_X` family is the swizzle defect; **why is still not established** —
  2nr blamed Navi 21's pipe count and 2ns measured that false.
- **`getPDEValue` returns flags only** — the address is ORed in by its caller.
  Never hook it to correct an address. [2ko]
- The panel: 1920x1080, `H_TOTAL` 2106, `V_TOTAL` 1129, pixel clock
  **341 987 984 Hz** - and **342 011 761 Hz** derived from boot 255's own
  measurement as mHz * H_TOTAL * V_TOTAL, 0.007 % apart, the derived one being
  the one that round-trips back to V_TOTAL 1129 - **143.83 Hz** measured.
  `V_BLANK_START_END` is 1311820 (start 1100, end 20), which is why V_TOTAL can
  be changed without touching the active raster. OTG0 is already running
  (`OTG0_CONTROL = 0x00011201`, OTG1-3 at 0x300 = off, DMCUB 4.0.15.47), DMU
  segment 2 = **0x34c0**, BAR5 is 512 KB. Measure the refresh by latching both
  endpoints on an `OTG0_FRAME_COUNT` edge, never over a fixed window (2rn).
- A Transfer TRB's dword 2 (`+0x8`) is
  `length[16:0] | TD-size[21:17] | interrupter[31:22]` — the USB fix's whole
  basis (2xv).
- `IODetailedTimingInformationV2` is 0xa0 bytes, `pixelClock` at 0x28;
  `AGDCLinkConfig_t` is 0xb0 and embeds it at **+0x04** (2rd/2rg).
- `IOFramebuffer` vtable call offsets: `0x9b0` `getVRAMRange`, `0xad0`
  `registerForInterruptType`, `0xae0` `setInterruptState` (2pq, 2s5).

## Boot-args

**THE FIRMWARE MUX MODE IS PART OF THE CONFIGURATION (2zw).** The chassis
must be in **iGPU-only / Eco** mode, set in the ASUS firmware setup (the user
switches it from Windows). In hybrid (iGPU + dGPU) mode the eDP panel and its
backlight are wired to the dGPU, and the 680M's backlight registers drive
nothing - which cost six milestones before rule 47 was written down. Read the
mode before reading anything about an output.

**The right boot-args are none of ours.** `src/common/rb_defaults.h` is the
list and the hosttest checks it; read that file, not a copy of it here.
**`sudo tools/efi.sh defaults`** takes the redundant ones out of the EFI in one
edit - exactly that table, matched by name - keeps every disarm and everything
not ours (`wdt=-1` above all), reports any `rbgpu` token it kept, and then
prints which NVRAM keys would disarm a half.

**Rescue, in order of bluntness:**
- `sudo nvram rbgpu-arm=0` — accelerator off next boot (and the framebuffer
  declines the panel with it, rule 38). Also `rbgpu-fb`, `rbgpu-agdc`,
  `rbgpu-iop`, `rbgpu-tick`, each `=0`. Re-arm with `sudo nvram -d <key>`.
- `-rbgpunoschedtick`, `-rbgpunofbhubp`, … — one feature off, derived from its
  own name: `-rbgpuno` + the rest, or for the two already-negative tokens
  (`-rbgpunocgcg`, `-rbgpunogates`) the positive form.
- **`-rbgpustock`** — every default off, the plugin otherwise alive.
- **`-rbgpuoff`** — the plugin loads and does nothing, and the framebuffer
  declines the panel. The documented rescue.
- The **boot sentinel** needs no command: a boot that does not survive 60 s
  makes the next one stock automatically.

**Still opt-in, and why:**
`-rbgpuosforce` (2xz - arms the default-on table on an
OS none of the measured offsets came from; a safety gate's override is never
default-on),
`-rbgpufbmodes` (2y6 - the
timing write itself is measured working, but this advertises a SECOND display
mode, and `getDisplayModeCount()` has returned 1 since 2sl with every boot read
against that; 2xx is what a WindowServer that dislikes its framebuffer costs.
Mutually exclusive with `-rbgpufbdrr`, checked in `drrSetup()` rather than
documented, because the experiment would see our own `V_TOTAL_CONTROL` and
refuse with "the firmware owns DRR"),
`-rbgpufbdpalt` (2C0 - the RDPCSTX PHY read. Read-only
with no setter, but it is the one register block in this kext whose documented
failure mode is a **hang**: Linux marks the path "Legacy path, avoid if
possible". The hang is conditioned on DMCUB not running and 2zs measured it
running, so the condition is measured false rather than assumed - which is an
argument for trying it, not for defaulting it on),
`-rbgpuagdcearly` (**wedges the desktop**, 2rz), `-rbgpufbdmubbl` (2zn - producing on
DMCUB's inbox 1 is a new hardware-write surface, and even armed nothing is
sent until `build/out/dmubbl` names a level; the locate and the bring-up scan
are read-only and run unarmed), `-rbgpufbmatch` (spoofs
`device-id` and **kills the accelerator**), `-rbgpupdefix` (composes with the
upstream fix and the halt returns), `-rbgpufbvramrange` (hypothesis dead,
2pr), `-rbgpumcnew`/`-rbgpumcreg` (`mcreg`'s failure mode is a **hang**, so it
is the user's decision), `-rbgpunodisppanic`, `-rbgpusdmahook`,
`-rbgpustampraw`, `-rbgpuregdump`, `-rbgpubeat`. **`-rbgpuforceenable` is now
default-on and still wants `wdt=-1` in boot-args** — it is Apple's arg, we
cannot set it, and without it a frozen boot panics at 120 s with no SSH window
(2hh). Keep `wdt=-1`.

**Runtime dials** (`setProperties`, no boot needed - 2zm, and 2zz is what
they bought): `{"RBFBPubMs": N}` via `build/out/pubms`, and
`{"RBFBFtOn": 0|1}` via `build/out/ftbudget` (2A0 - the frame-time budget,
whose write also zeroes the measurement window), and
`{"RBFBNotReady": 0|1}` via `build/out/holdset` (2A2 - the 2xo hold, worth
~21 % of the frame rate and traded against 2xo's whole-frame flicker; it can
only arm what the boot set up, and `RBFBAmtrNotReady` publishes the live
state).
**`{"RBFBTriple": 0|1}` via `build/out/tripleset` (2B1 - DCN's SECOND pending
flip slot, `SURFACE_TRIPLE_BUFFER_ENABLE`, which reads 0 on every boot since
216. OFF by default and a new hardware-write surface in the display path:
Linux pairs this with a timing-generator lock we do not reproduce, so the
failure mode is a tear, a dropped frame or a wedge. The combination worth
trying is `holdset 0` + `tripleset 1`, judged by the SCREEN during real window
animations, not by `animtest`.)**

**Tunables:** `rbgpufbpubms=<0..10000>` (2zk - the frame-path publish
throttle in ms, default **500**; `0` restores the unthrottled pre-2zk
behaviour and is the A side. `publishVBLState()` is ~110 `setProperty` calls
and boot 270 measured it at **227/s** from `setInterruptState`, because Apple
arms and disarms the vblank twice per transaction - ~25 000 registry
insertions a second inside the call that gates every frame, and the one
suspect in the frame path that six dead models of the ~73 sets/s ceiling never
examined - and **2zz measured it at -1.4 %**, so the cost is real and is not
the ceiling; keep the default),
`rbgpuschedtickus=<500..1000000>` (**6944 is a measured
interior optimum** - 2xn/2zb/2zc put 2000, 6944 and 13888 at 51.33, 74.6 and
49.2 sets/s, so both directions cost a third of the frame rate; do not change
it without reading 2zc), `rbgpuvblhz=<millihertz>` (20000-400000),
`rbgpufbvrammb=<256..16384>`, `rbgpufbvramrangemb=<8..256>`,
`rbgpufbhubpmax=<N>` (0 = unlimited), `rbgpufbatymb=<1..3072>`,
`rbgpufbblfloor=`, `rbgpumtlkscope=` / `rbgpumtlkexclude=` (exclude wins),
`rbprobe_level=N`, `rbprobe_smu=1`. Rule 40: pass them as numbers, read them
with `rbgpu_boot_argn_u32()`.
