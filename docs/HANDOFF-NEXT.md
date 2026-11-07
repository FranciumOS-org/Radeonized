# Handoff — next step

**Written 2026-11-07, after 2FB. Last boot: 464.**

Read `CLAUDE.md` first, then this. `docs/EXTERNAL-DISPLAY.md` has the full
state of the goal; `docs/JOURNAL.md` under `2FB` has boot 464 in detail.

---

## Three things before you touch anything

1. **PLUG THE TYPE-C CABLE IN BEFORE BOOTING.** The firmware enables the AUX
   engines on a connect path that runs at boot, and we never write `AUX_EN`.
   Plug in afterwards and every AUX read returns **refusal 3** on engine
   "AUX5" (= `RB_AUX_COUNT`, none), with `AUX_EN` clear on AUX1-4 and
   `HPD2_STATUS` reading `0xff000013` instead of `0xff0ff013`. HPD still
   senses, because HPD is a raw pin, so this looks like a dead port and is
   not. 2FB.

2. **`dptrain go tps=3` DEGRADES THIS SINK and the recovery is a REBOOT**
   (2F8, two boots, with a healthy before/after control). The tool refuses
   `tps=3`/`tps=4`. **Use `tps=2`.**

3. **Check the sink before every experiment.** `./build/out/edidread` (header
   OK, checksum OK) and `dpcdread 0x0 4` reading `12 14 c2 81`. A degraded
   sink looks exactly like a failing transmitter. The chassis must be in
   iGPU-only / Eco mode.

---

## Where the chain actually is

Everything on our side of the link now takes its writes and reads them back,
and **the sink still sees no symbols.** Boot 464, one boot, each step with its
own verdict:

| Step | Result |
|---|---|
| `dmubbl dto 148500` | `PIXEL_RATE_CNTL 0x8000c000 -> 0x8000d090`, **`DPDTO_ENABLE_STATUS 0 -> 1`** |
| `otgprog go 1080p60 pipe=1` | **`OTG1_CONTROL 0x300 -> 0x00011201`**, 16/16 writes, counter advancing |
| `dmubbl dig fe` | `DIG_BE_CNTL 0x20000000 -> 0x20000400`, `FE_SEL 0 -> 0x4` |
| `dptrain go tps=2` | `TP_SEL 0 -> 1`, applied/clean/moved/observable all 1 |
| **DPCD `0x202`** | **`0x00` before and after. No clock recovery.** |
| `RDPCSTX2_PHY_CNTL6` | `0x00063333`, **unmoved, third boot** |

**2F9's defence of that `0x00` is spent.** It said "a clock with no raster and
no stream has nothing to send". There is now a raster, and the answer did not
change. Do not re-run that argument.

---

## The one new measurement, and the next experiment

**The raster runs at 8.2726× its programmed timing.** Measured against wall
time over 20.064 s, both counters latched in the same periodic sample:

```
  panel OTG0 =  61.40 Hz      (the DRR ladder, at 60 - NOT 143.83)
  OTG1       = 496.35 Hz
  implied pixel clock = 1228.47 MHz   (programmed 148.50)
```

The live registers hold `H_TOTAL` 2199 (= 2200), `V_TOTAL` 1124 (= 1125),
`PHASE` 148 500 000, `MODULO` 600 000 000, `PIXEL_RATE_CNTL 0x8000d090`. So
the timing is programmed, the DTO is programmed, and the rate matches neither.

**This supports exactly one claim: OTG1 is not being clocked by the DTO we
programmed.** It does not name a mechanism. 2 × the 600 MHz reference is
1200 MHz, 2.3 % away and inside the measurement's own error, which makes it a
candidate and not an identification (rule 18). **Do not write that down as the
answer.**

### The experiment that settles it, and it costs one boot

The in-boot A/B is **not available**: `dmubbl dto` refuses a second write on a
live pipe by design, and nothing turns a DTO off. So:

**Reboot (cable in), run the DTO at 148500 as usual, then program a raster
with a DIFFERENT `H_TOTAL × V_TOTAL`** — not a different clock. `otgprog`
takes the mode piecewise, so:

```bash
./build/out/otgprog go pipe=1 refresh=60 hactive=1920 hfront=88 hsync=44 htotal=1100 vactive=1080 vfront=4 vsync=5 vtotal=1125
```

Half the H_TOTAL, same everything else. Then measure the rate the same way.

- Rate **doubles** → the OTG honours its own timing and the *clock* is wrong.
  The next question is which clock OTG1 is on, and `PIPE_DTO_SRC_SEL` /
  `DTBCLK_DTO_DIV` in `PIXEL_RATE_CNTL` are where to look.
- Rate **does not move** → the counter is not driven by our raster at all,
  and section 2 of 2FB has to be re-read with that in mind.

The mode will fail the clock-agreement gate (refusal **47**) because
1100 × 1125 × 60 ≠ 148.5 MHz. That gate exists for a reason; **either pick a
clock that matches the new product (`dmubbl dto 74250` for 1100 × 1125 × 60 =
74.25 MHz) — which keeps the gate honest and still changes the product — or
decide deliberately to widen it, and say so in the journal.** Changing the
clock *and* the product does not separate them, so prefer: keep the clock at
148500 and pick a product that matches it, e.g. `htotal=1100 vtotal=2250`
(1100 × 2250 × 60 = 148.5 MHz). **That changes H_TOTAL and V_TOTAL while
holding the clock and the frame rate, which is the clean form.**

### How to measure the rate

Both counters in one periodic sample, against wall time, over ≥20 s. The
panel's rate is **not** 143.83 at the moment of reading — the DRR ladder moves
it, and 2FB got this wrong on its first pass. Use wall time, not a ratio to
the panel.

```bash
cd /Users/ehsan/Developer/RembrandtGPU
r() { ioreg -rl -c RBAMDFramebuffer | grep -oE '"RBFBOtgR[01]Frames" = [0-9]+' | grep -oE '[0-9]+$' | tr '\n' ' '; }
a=$(r); t0=$(python3 -c 'import time;print(time.time())'); sleep 20; b=$(r); t1=$(python3 -c 'import time;print(time.time())'); echo "$a | $b | $t0 $t1"
```

---

## After that

In order. Each is a thing that does not exist yet:

1. **`enc31_map_stream_to_link`** — no stream is mapped to DIG2. Discharge
   rule 46 by source in `dcn31_resource.c` first, as 2FA did for the OTG path.
   The OPTC split there (timing from DCN10, enable from DCN31) says not to
   assume the DIO functions all come from one file either.
2. **A surface** on pipe 1 — HUBP, address, swizzle.
3. **A second `IOFramebuffer`.** `getDisplayModeCount()` has returned 1 since
   2sl.

**PHY0 / DIG0 / front end DIGA are the panel's and are refused by name** in
every half of every one of these experiments. Keep it that way.

---

## Tool notes from 2FB

- `RBFBAuxEn` and `RBFBHpdSink` are **masks**, not indexes. I read both as
  scalars in one investigation (`AuxEn` = 1 as "enabled yes"; `HpdSink` = 5 as
  "engine 5"). Rule 33's shape: an index is not a count, and a mask is
  neither. Print values in hex beside their names; that is what caught it.
- `./build/out/pubms` is **not built** by `make tools` — `make pubms`. Rule 15
  applies to tools.
- `dmubbl dto` takes **kHz**. Hz overflows the field and the tool refuses
  anything over 1 000 000.
- `otgprog` prints `PANEL CONTROL OK` as its own line. If it says FAILED, the
  kext is not addressing the OTG block and nothing else in the output means
  anything (2FA-a, refusal 48).

---

## Open, off the critical path

- **Why DPCD `0x102` stores only from inside `dptrain`'s own call** (~100 µs
  after the register writes, n=2 boots). The arm meant to separate movement
  from proximity is **void** — it used `tps=3` and the sink was degrading.
  Re-run on a healthy sink with `tps=2` only.
- **`RDPCSTX*_PHY_CNTL6` needs replacing as `dig`'s verdict register.** It is
  the legacy fallback Linux reads only when DMCUB is absent, and it already
  disagreed with `QUERY_DP_ALT` on all five PHYs.
- **The wedge** — work submitted that never retires, channel-52 GFX, 3.5 s.
- **`powermetrics` has never been run** against the default 576 interior
  thread_calls/s, the one real argument against the 2D5 default.
- **Ventura and Sonoma claim nothing** in `rb_osver.h`.
