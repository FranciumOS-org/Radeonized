# Handoff — next step

**Written 2026-11-07, after 2FE. Last boot: 468.**

Read `docs/RULES.md` first, then this. `docs/JOURNAL.md` under `2FE` has boot 468.

---

## Three things before you touch anything

1. **PLUG THE TYPE-C CABLE IN BEFORE BOOTING.** The firmware enables the AUX
   engines on a connect path that runs at boot and we never write `AUX_EN`.
   Plugged in afterwards every AUX read returns refusal 3 on engine "AUX5"
   (= `RB_AUX_COUNT`, none) and it looks like a dead port. 2FB.

2. **`dptrain go tps=3` DEGRADES THIS SINK and the recovery is a REBOOT**
   (2F8, two boots, with a healthy before/after control). The tool refuses
   `tps=3`/`tps=4`. **Use `tps=2`.**

3. **Check the sink before every experiment.** `./build/out/edidread` (header
   OK, checksum OK, `MSI MP2412C`) and `dpcdread 0x0 4` reading
   `12 14 c2 81`. A degraded sink looks exactly like a failing transmitter.
   The chassis must be in iGPU-only / Eco mode.

**Install the framebuffer first: UUID `F004AF7C`, `sudo tools/fbinstall.sh`.**
It carries the SRC_SEL default 2FE measured. Without it, `dmubbl dto` with
`srcsel` omitted still programs the old value.

---

## THE BLOCKER, as sharply as the evidence allows

**The digital transmitter answers every write we make. The analog PHY is off.**

```
  PHY0  0x00194444   4 MPLLs on (TX0-3), DPALT_DP4 1, ref_clk en 1 req 1
  PHY2  0x00063333   no MPLL, no ref clock, pstate 3 3 3 3,
                     DPALT_DISABLE asserted AND acked
```

`TP_SEL`, `DIG_BE_CNTL.FE_SEL`, the DP DTO and the OTG all take our writes and
read back changed. PHY2 has no reference clock, not one enabled MPLL, and all
four lanes parked. PHY0 is the in-boot control and shows what an active PHY
looks like on this chip. **Four boots — 459, 461, 464, 468 — have left
`0x00063333` untouched.**

So the question has changed shape. It is no longer "did our write land" but
**"what powers up an RDPCSTX lane"**. The atom transmitter ENABLE at the link
rate has executed since 2E2 (`dmubbl dig` carries `RB_DIG_TYPEC_SYMCLK` 54000
= HBR2) and does not do it.

### Two cautions before designing that experiment

- **`PHY_CNTL6` is a suspect, not a witness.** It is the legacy fallback Linux
  reads only when DMCUB is absent, and it has disagreed with `QUERY_DP_ALT` on
  all five PHYs since 2EB — 2EB measured alt mode **granted** on PHY2 while
  this register says `DPALT_DISABLE` asserted *and* acked. One of the two is
  wrong and which is **not established**.
- **`rb_dpalt.h` has no setter by design** and Linux marks that write path
  "avoid if possible". Do not add one to test a hypothesis.

**So the first move is a verdict register that is not `PHY_CNTL6`.** The
handoff has asked for one since 2F9; 2FC's `DIO_LINK*_CNTL` was tried and is
all zeros, so it is not it. Candidates not yet read, in `dpcs_4_2_0_offset.h`
and `dcn_3_1_2_offset.h`: the rest of the `RDPCSTX2_*` block
(`RDPCSTX_PHY_CNTL0..5`, `_PHY_FUSE*`, `_PLL_UPDATE_*`), and
`DP_DPHY_INTERNAL_CTRL` / `DP_LINK_CNTL` on DIG2. Read before writing, and
discharge rule 46 on whichever function Linux uses to power a lane —
`enc31_hw_init`, `dcn10_link_encoder_setup`, `dpcs31_program_eq_setting` are
in `dcn31_dio_link_encoder.c`, already vendored.

---

## Settled in 2FE — do not re-open

- **The 8.27x raster rate was `PIPE_DTO_SRC_SEL`.** Boot 468 with `srcsel=0`:
  59.98 Hz against 60 asked, implied 148.46 MHz against 148.50, ratio 0.9997.
  Boots 464 and 467 at `srcsel=1`: 8.2726 and 8.1293. **The 2 x 600 MHz
  candidate is dead as a mechanism** — it was never a clock source.
- **The wrong clock was NOT the blocker.** The chain was re-run at 59.98 Hz
  this boot: `dig fe` moved `FE_SEL 0 -> 0x4` (changed 1), `dptrain tps=2`
  moved `TP_SEL 0 -> 1`, and DPCD `0x202` read `0x00` before and after. The
  defect was real, it is fixed, and the symptom did not move.
- **The DIO encoder-type mux.** All six `DIO_LINK{A..F}_CNTL` zero, before and
  after our writes, with an in-boot base control. Not steering DIG2 away from
  PHY2. **Not** "confirmed on DIG" — `ENCODER_TYPE_DIG` is zero.
- **`enc31_map_stream_to_link` as the next step.** Wrong name: it is the HPO DP
  128b/132b encoder's. This chip's DIO stream encoder is
  `dcn30_dio_stream_encoder_construct` over DCN10's register struct
  (`dcn31_resource.c:1323`, `:361`). The connect it implied was
  `dcn10_link_encoder_connect_dig_be_to_fe`, **done in 2EC**.
- **`PIXEL_RATE_SOURCE` and `DTBCLK_DTO_DIV`.** Identical on the working panel
  pipe (0 and 8), so neither was ever the difference.

---

## How to run the chain, once there is something to test

Cable in, sink checked, then:

```bash
./build/out/dmubbl dto 148500 pipe=1
./build/out/otgprog go 1080p60 pipe=1
./build/out/dmubbl dig fe
./build/out/dptrain go tps=2
./build/out/dpcdread 0x202 4
./tools/dpaltlook.sh
```

`srcsel` is now omitted on purpose — the default is the measured 0. The DTO
refuses a second write on a live pipe, so the pipe must be dark at the start
of the boot; `otgprog` with no `go` reports that.

### How to measure a raster rate

Both counters in one periodic sample, against wall time, over >= 20 s. **The
panel's rate is not a constant** — the DRR ladder moves it: 144.05 Hz in boot
468, 144.69 in 467, 61.40 in 464. Use wall time, never a ratio to the panel.

```bash
cd /Users/you/Developer/RembrandtGPU
r() { ioreg -rl -c RBAMDFramebuffer | grep -oE '"RBFBOtgR[01]Frames" = [0-9]+' | grep -oE '[0-9]+$' | tr '\n' ' '; }
a=$(r); t0=$(python3 -c 'import time;print(repr(time.time()))'); sleep 25; b=$(r); t1=$(python3 -c 'import time;print(repr(time.time()))'); echo "$a | $b | $t0 $t1"
```

`otgprog` takes all eight timings piecewise with `refresh=`, and its gate
(refusal **47**) needs `htotal x vtotal x refresh` to equal the live DTO PHASE
within 0.5 %. **Check the mode is possible before spending the boot**:
`htotal` must exceed `hactive + hfront + hsync`. The 2FB handoff shipped a
mode that failed this.

---

## After the PHY

1. **A stream on DIG2** — read `dcn30_dio_stream_encoder.c`, **not** a dcn31
   file (2FC discharged rule 46 here).
2. **A surface** on pipe 1 — HUBP, address, swizzle.
3. **A second `IOFramebuffer`.** `getDisplayModeCount()` has returned 1 since
   2sl.

**PHY0 / DIG0 / front end DIGA are the panel's and are refused by name** in
every half of every one of these experiments. Keep it that way.

---

## Tool notes

- `RBFBAuxEn` and `RBFBHpdSink` are **masks**, not indexes (2FB, rule 33).
- `make pubms` and `make dmubbl` are their own targets — `make tools` builds
  neither. Rule 15 applies to tools.
- `dmubbl dto` takes **kHz** and refuses anything over 1 000 000.
- `otgprog` prints `PANEL CONTROL OK` as its own line. If it says FAILED the
  kext is not addressing the OTG block and nothing else in its output means
  anything (2FA-a, refusal 48).

---

## Open, off the critical path

- **DPCD `0x204` = `0x80`** (boot 468) = `DP_LINK_STATUS_UPDATED` with
  `DP_INTERLANE_ALIGN_DONE` clear (`drm_dp.h:822-829`). The sink is noticing
  something while reporting no CR. Sticky by definition (rule 33) and n=1.
- **`OTG_DIO_FIFO_ERROR`** reads 3 on dark pipes, 0 on the panel's, and 2FD
  established it stays 3 on a pipe with the DTO running and the raster
  scanning. A stream is untested. Published raw, read by nothing.
- **Why DPCD `0x102` stores only from inside `dptrain`'s own call** (~100 us
  after the register writes, n=2 boots). The arm meant to separate movement
  from proximity is **void** — it used `tps=3` on a degrading sink. Re-run on
  a healthy sink with `tps=2` only.
- **The caller of `set_dio_phy_mux` is not identified.** Neither
  `link_hwss_dio.c` nor `link_hwss_hpo_dp.c` calls it. Must be answered before
  anything writes that register.
- **The wedge** — work submitted that never retires, channel-52 GFX, 3.5 s.
- **`powermetrics` has never been run** against the default 576 interior
  thread_calls/s, the one real argument against the 2D5 default.
- **Ventura and Sonoma claim nothing** in `rb_osver.h`.
