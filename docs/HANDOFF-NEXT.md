# Handoff — 2026-10-27, after 2v8

Read this, then `docs/RULES.md`, then grep `docs/JOURNAL.md` for the tags below.
All hard rules in `docs/RULES.md` remain in force, unchanged.

## What was achieved in the session that wrote this (2u5 → 2v7)

**The gamma chain was traced end to end and then CLOSED.** Our AGDC
attribute-`0x711` type-2 (Gamma) reply entry's `+0x28` is the CFDictionary key
**`MaximumSize`**, which CoreDisplay reads as the gamma table entry count.
Chain, all read statically:

`rb_agdc_gamma_entry()` → `IOPresentmentCapabilitiesDisplayPipe::IOPresentmentCreateGammaDictionary`
(`0x7ff812e9b8e0`) → `IOPresentment::__gammaFromDictionary` (`0x7ff812e8fb40`)
→ `__fieldsFromDictionary` (`0x7ff812e8f866`) → CoreDisplay's 0x38-stride
entry `+0x30` → `CoreDisplay::IOPCapabilities::GammaChannelTableEntryCountAndType`
(`0x7ff80405ddd8`), which logged *"…plane with no 3-channel gamma table
support"* whenever it was zero.

**Boot 182 is the result.** `RB_AGDC_GAMMA_CHANNELS = 0x400` (1024) in build
`7C1FED31`, verified landed via the NVRAM hot-key `agdc_gamma_elems1 = 400`.
Normalised over 66 `DisplayPipeState` dumps:

| | boot 179 | boot 181 | **boot 182** |
|---|---|---|---|
| `no 3-channel gamma table support` | 7.12 | 7.01 | **0** |
| `no 3-channel linearization table support` | 1.01 | 1.01 | 2.03 |
| `Fallback GPU Display Pipe transaction submission failed` | 1.00 | 1.00 | 1.00 |
| `Clean` | 0 | 0 | 0 |
| `System Gamma` | Unknown | Unknown | Unknown |
| **`EOTF`** | GPU | GPU | **Pipe** |

`EOTF` moving from `GPU` to `Pipe` is the **first time in this whole sequence
that any of the eight display-pipe operations changed target**. It was not
predicted and is recorded as a measurement, not an explanation.

**2v7 then withdrew 2v6's "linearization is the new blocker".** Apple's own
`getFbCapabilityForDegamma` (`0xbfed0dc`) writes `Type = 1` for both degamma
entries, `AGDCLinearizationTypeToIOPresentment` (`0x7ff812e98f0c`) is an
identity map, and CoreDisplay's linearization predicate accepts only `Type`
bits in mask **`0x3fcc`** (bits 2,3,6,7,8,9,10,11,12,13) — never bit 0. So a
**real** AMD Mac logs that WARN too. And `DisplayPipe::AddLinearization`
(`0x7ff8041092b4`) propagates nothing: after the WARN it sets the
identity/bypass flag at `txn+0x12c`, writes `txn+0x130 = plane`,
`txn+0xa8 = 1`, and returns void. **Do not raise the degamma `Type` off 1**
(rule 17; it would contradict Apple's own literal).

Other things settled without spending a boot:
- `CoreDisplay::UseIOPGamma` (`0x7ff8040ea95c`) is **eliminated**: the four
  `useIOP_gamma_on{IG,AMD,NVIDIA,APV}` CFPreferences all read
  `1 (absent -> default)` on this machine (`build/out/useiop` now prints them).
- `2v3`'s field map was off by one slot and is corrected by `2v5`/`2v7`: in our
  **0x32c** AGDC reply entry the layout is `+0x04` gate, `+0x0c`
  `NonAtomicApplicationPeriod`, `+0x10` `Type`, `+0x14` `FixedFormat`, `+0x18`
  `ConfigurationID`, `+0x20` `MinimumValue`, `+0x24` `MaximumValue`, `+0x28`
  **`MaximumSize`**. Confirmed twice, independently, from the gamma and the
  linearization dictionary builders.
- `2u9`'s second gamma entry is implemented (`rb_agdc_gamma_entry2`) and
  hosttested but **unreachable**, because `RB_AGDC_GAMMA_SEGMENTS` is back to
  **1**: boot 181 measured two entries buying nothing, and 2v5 explained why —
  Apple skips entry 2 on its `Type` of 1.

Tooling fixed on the way (each had destroyed a capture):
- `tools/plugin_check.sh`: new `rb_bounded` wrapper; `kmutil showloaded`
  capped at 20 s (it ate boot 180's whole second half); the display-log block
  moved **before** the kext listing, with a `process == "WindowServer"`
  control and a 6×15 s retry loop (boot 178's log was captured at +28 s and
  came back 0 bytes).
- `tools/re/dsc.py`: `slide_regions` / `dataxref` were **single-file** — 2r2's
  defect repeated in the data searches. Now cache-wide via a new
  `cache_paths()`; slide regions went 2 → **14** across 7 files. Note
  `ripall` sees only RIP-relative **code** references, so a cfstring reached
  through a pointer slot is invisible to it — run `dataxref` first, then
  `ripall` on the slot it finds (this nearly produced a false "the keys exist
  nowhere" conclusion; recorded as 2v4).
- `tools/hosttest/agdc_test.c`: the 2u9 assertions compared a field against
  the macro that wrote it, so the suite could not fail. All gamma assertions
  are now **literals** (`0x400u`, `0x33000000u`, `0x43000000u`, `0x200u`) and
  were verified to fail when the constants are zeroed.

## What to do next

**2v8 answered step 1 of the previous handoff, and the answer closed the
line.** Read `docs/JOURNAL.md` under `2v8` first — it withdraws three of
2v7's closing statements. In short:

- The target table is at **`0x7ff8404fc2f8`** with **four** entries,
  `Unknown`(0), `GPU`(1), `Pipe`(2), `None`(3). `Unknown` is **enum 0**, not
  a printer default. 2v7 read the table one slot high.
- `System Gamma`'s target (`pipe->0x728 + 0x40`) has exactly **one** writer,
  in `DisplayPipe::Commit` at `0x7ff804116f3e`, gated on the transaction's
  system-gamma optional `txn->0x1a0`, whose only setter is
  `CoreDisplay::Display::SetSystemGamma(double)` (`0x7ff8040a19f0`).
  `UpdateTargetStateColorProcessing` (`0x7ff8041079be`) assigns Linearize,
  CSC, Regamma and EOTF and **never** System Gamma.
- So **`System Gamma - Unknown` is a symptom of "no transaction has ever been
  committed", not an independent blocker**, and nothing our kext replies can
  change it. It also explains 2v6's unexplained `EOTF` `GPU`->`Pipe` move.
- **`Clean` was never printable.** The byte prints `Dirty` or spaces;
  controlled counts in boot 182's capture are `Dirty` 528 = 66 x 8, `Bypass`
  0, `Clean` 0. Drop it from the open list; the real content is that **all
  eight operations are Dirty and none is Bypass in all 66 dumps**.

**So the frontier is back where 2s9/2sv left it, now with the gamma side
provably clear: nothing has ever queued a transaction.** `fb_vbl_ot_armed` is
absent with `diag_used` 549-552 of 768, so rule 14's check passes and the
absence carries information.

The next step is to find **what should queue the first transaction and does
not**. Two static reads, neither needing a boot:

1. `CoreDisplay::DisplayPipe::BuildTransactions(UseCountProxy<DisplaySurface*>)`
   (`0x7ff80410be58`) and `DisplayPipe::RunFullDisplayPipe`
   (`0x7ff80410e2c2`) — find the predicate that decides between the full pipe
   and `RunGPUDisplayPipe` (`0x7ff804112c9c`), which is the path that produces
   *"Fallback GPU Display Pipe transaction submission failed"* (1.00/dump,
   still unexplained).
2. `AreOperationsSupportedInHW` (`0x7ff8041087fe`) reads every target with
   `cmpl $0x2` (== Pipe). Boot 182 has **two** Pipe (Fetch, EOTF) and five
   None. Establish what that function returns for that mix and who consumes
   it — it is the most likely gate between "capabilities are fine" and "no
   transaction is built".

Useful command form:

```bash
env -u DYLD_INSERT_LIBRARIES sh tools/re/dscdis.sh 0x7ff80410be58 0xe42
```

**3. Still open, carried forward:**
- The **wedge** is unexplained; boots 177–182 all wedged. 2s8 holds — a plain
  power cycle comes up clean, the one-shots are consumed, nothing re-arms.
- `Fallback GPU Display Pipe transaction submission failed`, 1.00/dump, cause
  unknown. (`Clean` is struck from this list by 2v8 — it was never printable.)
- `fb_vbl_ot_armed` is still **absent**, with `diag_used` 549–552 of 768, so
  rule 14's check passes and the absence carries information.
- Audit the three remaining transforms on the dead
  `IONDRVFramebuffer::enableController` hook for **type** as well as
  reachability: 2pa's VRAM publish, 2qv's metaclass read, 2qx.
- Reconcile 2sv's controlled zero for `IOAccelDisplayPipeCapabilities`
  against 2u1/2u2, and the `IOAccelDisplayPipeUserClient2` count of 2 with
  `IONDRVFramebuffer` vs 1 with ours.
- **~60 unpushed commits.** Pushing was conditionally authorized earlier and
  is blocked only on the network: the hotspot is IPv6-only, so
  `github.com` / `api.github.com` are unreachable without Cloudflare WARP
  (`raw.githubusercontent.com` does work). Ask before pushing.

## Current build state

`src/common/rb_agdc.h`:

```
RB_AGDC_GAMMA_SEGMENTS   1       /* vt[0x118](0x17)+1; Apple writes entry 2 only at >=2 */
RB_AGDC_GAMMA_CHANNELS   0x400   /* entry 1 +0x28 = MaximumSize. A SUBSTITUTE (rule 17). */
RB_AGDC_GAMMA_ELEMENTS   0x200   /* Apple's literal for entry 2's +0x28 */
RB_AGDC_GAMMA_MIN_F      0x33000000   /* powf(2,-25) */
RB_AGDC_GAMMA_MAX_F      0x43000000   /* powf(2,7)   */
```

Nothing has changed in the kext in 2v8 either — it was a pure static read.
Boot 182 ran build `7C1FED31` (`7C1FED31-711A-3A1F-B035-6FDEAAC9DF9C`) with
`-rbgpuagdccaps` armed. Nothing has changed in the kext since, so no new build
or boot is pending — the next step is a static read.
