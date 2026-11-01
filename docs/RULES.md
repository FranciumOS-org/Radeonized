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
is what fixed the flicker, at 27 % of the frame rate (2xo).

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
- **Brightness — THE REGISTER WORK IS DONE AND THE PANEL DOES NOT FOLLOW IT
  (2zj, boot 270).** The slider moves, tracks our value, and changes nothing.
  **Nothing else writes `BL_PWM_CNTL`**: across a 40 s quiet window
  `RBFBBlLiveReads` climbed 10472 → 20218 with `RBFBBlLiveDiffs` **25 → 25**
  and the count pinned at what we wrote, so ABM in the DMCUB is eliminated and
  the register sits enabled, fractional, period 4000, holding our duty cycle
  with the panel unmoved. **This panel's backlight does not follow PWRSEQ0's
  BL_PWM** — 2or's own falsification test, reached by a route a zero period
  cannot detect. Apple has no competing path either: `AppleIntelPanelA` *is*
  matched on PNLF but publishes `{"commit"={"reg"=0}}` and no `brightness` at
  all, because it is the *Intel* backlight driver. **2zl says why, and eDP AUX
  was the wrong guess**: on DCN 3.1 the CPU never writes `BL_PWM_CNTL` to
  change brightness. `dcn31_panel_cntl.c` has **no `.set_backlight_level`** —
  every member of `dcn31_link_panel_cntl_funcs` is a DMUB round trip — and
  `dcn21_set_backlight_level` takes the register-writing `dce110` path only
  `if (res_pool->dmcu)`, the old DMCU this chip has not got, and otherwise ends
  at `dmub_abm_set_backlight()`. The AUX control types exist but only under
  `edp_set_backlight_level_nits`. **So 2xs transcribed `dce_panel_cntl.c`:
  right offsets (2xp checked those), wrong METHOD for the version grouping**
  (rule 46) — the register is real and writable, just not the live input to the
  PWM, which is why nothing overwrote it. Apple has no path to borrow either:
  `AMDRadeonX6000Framebuffer` has `initializeDmcuB` and two DMCUB version reads
  and **no abm/backlight symbol at all** (3314 symbols as the control), and
  `AMDSupport`'s `ATIPanel` route needs an `ATIController`, which never
  instantiates here. **THE DMUB RING IS FREE, measured (2zl, boot 271):**
  `RBFBDmubWptrMoves` **0** and `RptrMoves` **0** across 125 samples, with
  `WPTR == RPTR == 2368` and `InboxSize` 8192 — and 2368 is **37 × 64**, i.e.
  37 `dmub_rb_cmd`s at bring-up and then nothing, the live confirmation of the
  static read. **So a DMUB ABM command is a live route.**
  **2zn designed it, boot 272 refuted it, and 2zo rewrote it to stop having
  a convention. Nothing has touched hardware.** 2zn named the mailbox window
  (CW4, from `dmub_srv.c`) and wrote `DMUB_CW4_BASE` as **0x80000000** -
  which is boot 271's `DMCUB_INBOX1_BASE_ADDRESS` reading, not the header's
  value. `dmub_srv.c:84` says **0x64000000**, and the hosttest had asserted
  the constant equalled the register, which **compared a reading with
  itself**. Rule 26 extended: **a constant is an instrument too.** The
  window address fields are **29 bits** (`sh_mask.h:5881`, enable at 31), so
  `0x64000000 & 0x1FFFFFFF` is **0x04000000** - exactly boot 272's CW4_BASE,
  so **CW4 IS the mailbox**, 64 KB of it. What is genuinely open:
  `DMCUB_INBOX1_BASE_ADDRESS` reads **0x80000000**, which is neither value,
  and **nothing locates from it any more**. The address kind was predicted
  wrong too: CW4_OFFSET reads **0xf4_ffe40000**, an MC address, because this
  machine's FIRMWARE wrote it and not Linux - so `mc2pa()` does apply and
  the ring is at system-physical **0x8_7FE4_0000**.
  **2zo's fix is to search instead of name.** `rb_dmub_win_locate()`
  resolves one window from its own registers (span from base/top under the
  29-bit mask; the offset classified MC-or-system-physical **by range**, the
  two ranges disjoint by construction so it is a measurement), and the kext
  maps **every** enabled window that resolves, reads its memory, and counts
  the leading run of plausible `dmub_cmd_header`s. **The window carrying the
  firmware's own bring-up commands IS the mailbox** - boot 271 says that run
  is **37**. `rb_dmub_sig_ok(run, bad)` is the decision: `run >= 2 &&
  bad == 0`, two being the floor because the plausibility test is
  deliberately weak and one lucky word is ~1 in 5. No window passing means
  nothing mapped and nothing sent. All seven windows and the per-window
  (resolve, run, bad, abm) are published whole (`RBFBDmubWindows`,
  `RBFBDmubWinScan`) - the TABLE is what refuted 2zn, and a search that
  published only its winner would have had nothing to say.
  Two commands, **separately triggered**: `SET_BACKLIGHT` alone first,
  `SET_PIPE` only if the panel does not move - `dcn21_set_backlight_level`
  binds ABM to the pipe first, but the firmware's bring-up may already have,
  and sending both would make a moving panel ambiguous between them (rule 45
  again). `dmub_srv.c:1037-1041` is the whole push: 64 bytes at WPTR, read
  every byte back (`dmub_rb_flush_pending`'s entire body is a write-combine
  flush), write WPTR - no interrupt, no doorbell register. **RPTR reaching
  our WPTR is the only acknowledgement** and it is counted (`RBFBDmubAcks` /
  `AckTimeouts`): the microcontroller CONSUMED the command, which is not the
  panel moving but does separate "never arrived" from "arrived and did
  nothing". **The slider is deliberately not wired to it** -
  `-rbgpufbdmubbl` arms it and `build/out/dmubbl` is the only trigger, and
  the tool **refuses to print a reading against an older kext** (absent keys
  read back as -1, and -1 as 32-bit hex is indistinguishable from a
  register - boot 272 printed exactly that). 77 hosttest assertions off boot
  272's own registers, verified failing in both directions.
  **What boot 272 proved works:** the locate refused, nothing was mapped,
  both sends returned `kIOReturnNotPermitted` with `last refusal 2`, and no
  byte reached any ring. 2zn's staging is why a wrong design cost one boot.
  **What is withdrawn is only the panel claim** (rule 45) — everything below is register
  and parameter plumbing and all of it still stands.
  **2xs (boot 249)** `-rbgpufbbacklight` writes the PWM duty
  cycle — note `BL_PWM_PERIOD` is **4000** with `BITCNT` **12** and
  `BL_PWM_FRACTIONAL_EN` SET, so the count field is fixed point with 4
  fractional bits and a count larger than the period is not a saturated PWM
  (2y2 nearly reported that as a bug). **2xy (boot 250)**
  `-rbgpufbbuiltin` answers `kConnectionFlags` with `kIOConnectionBuiltIn` and
  `getAppleSense` with `kPanelTFTConnect`, so `AppleBacklightDisplay` matches
  the connection. **2y1 (boot 252)** `-rbgpufbblparm` publishes a full-length
  parameter name from our own `IODisplayParameterHandler`, attached as a
  service-plane child of the framebuffer — 2y0 PANICKED on boot 251 doing this
  by mutating the dictionary it fetched with `getProperty` (rule 42), and that
  panic's own backtrace is what confirmed the mechanism. **2y2 (boot 253)**
  publishes the key the UI actually writes: the brightness UI is
  **CoreBrightness** (`BrightnessSystem`, `BrightnessSystemClient`) and it
  writes **`linear-brightness`**, never `kIODisplayBrightnessKey` —
  `RBFBBlParmSetsLin` 258 against `RBFBBlParmSetsPerc` 2 settles it. A set
  naming an unpublished key is dropped by `IODisplay` with nothing recorded
  anywhere, which is why `"brightness"` alone gave a slider that rendered and
  did nothing. `usable-linear-brightness` reports the ceiling and its writes
  are **declined and counted** (`RBFBBlParmSetsUsable` 2 - it is a real case).
  `'brig'` in the dictionary is stale by design: it belongs to
  `IOFramebufferParameterHandler`'s own dictionary, so do not read it as the
  current level. `./build/out/blset <0..1>` still drives it from the shell —
  and note what it measures: the API return code, not the panel. That is how
  2zj's gap survived four milestones.
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
  13888, timer-driven and vblank-driven alike. **Something sets ~73 and it is
  none of the things measured so far.**
  **2zk found one thing that was never measured**: `publishVBLState()` runs
  **227 times a second** from `setInterruptState` — twice per transaction,
  because Apple arms and disarms the vblank around each one — at ~110
  `setProperty` calls each, on Apple's own thread, inside the call that gates
  every frame. Rule 16, unexamined through six dead models. Throttled by
  `rbgpufbpubms` (default 500 ms, `0` is the A side). **Boot 271 measured it at
  60 Hz: 98.6 % of the publishes elided, and six paired runs gave 59.43 mean
  over runs 2-6 (±1.2 %) against boot 268's 49.0, with `fed` 97-99 % where it
  was ~82 % and `mean vblanks` 1.000-1.002.** NOT a claim yet — rule 44 wants
  two boots per side and 2zg is the entry that exists because six consistent
  samples on one boot did not reproduce. **2zm removes that cost:**
  `setProperties` accepts `{"RBFBPubMs": N}` and `build/out/pubms` (`make
  pubms`) writes it, so the A/B/A fits in ONE boot — six runs, `pubms 0`, six,
  `pubms 500`, six. Every dial this project owned was a boot-arg until now,
  which is why 2xn/2zb/2zc cost three boots for three points of one curve.
  Counters live in `ioreg -rl -c RembrandtGPUPeer`'s `RBGPUDiag` (**decimal**);
  `nvram rbgpu-diag` omits them because the note caps near 520 bytes while
  `diag_used` 693 of 768 says the table is not full - absence from one is not
  absence from the other.
- **The wedge**, an independent second defect: work submitted that never
  retires, on GFX/SDMA/compute user queues. `metal_probe --queues 4` reproduces
  it in userspace with no kext and no reboot (2wt). Not the display blocker
  (2x0, rule 39).
- **HDMI is the full project** and the panel is wired to the dGPU, so it is
  unreachable rather than merely unbuilt (2xq/2xr).
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

**Tunables:** `rbgpufbpubms=<0..10000>` (2zk - the frame-path publish
throttle in ms, default **500**; `0` restores the unthrottled pre-2zk
behaviour and is the A side. `publishVBLState()` is ~110 `setProperty` calls
and boot 270 measured it at **227/s** from `setInterruptState`, because Apple
arms and disarms the vblank twice per transaction - ~25 000 registry
insertions a second inside the call that gates every frame, and the one
suspect in the frame path that six dead models of the ~73 sets/s ceiling never
examined. Not yet claimed as the cause: rule 44 wants two boots per side),
`rbgpuschedtickus=<500..1000000>` (**6944 is a measured
interior optimum** - 2xn/2zb/2zc put 2000, 6944 and 13888 at 51.33, 74.6 and
49.2 sets/s, so both directions cost a third of the frame rate; do not change
it without reading 2zc), `rbgpuvblhz=<millihertz>` (20000-400000),
`rbgpufbvrammb=<256..16384>`, `rbgpufbvramrangemb=<8..256>`,
`rbgpufbhubpmax=<N>` (0 = unlimited), `rbgpufbatymb=<1..3072>`,
`rbgpufbblfloor=`, `rbgpumtlkscope=` / `rbgpumtlkexclude=` (exclude wins),
`rbprobe_level=N`, `rbprobe_smu=1`. Rule 40: pass them as numbers, read them
with `rbgpu_boot_argn_u32()`.
