# Handoff — next step

**Written 2026-11-07, after 2FD. Last boot: 467.**

Read `CLAUDE.md` first, then this. `docs/JOURNAL.md` under `2FD` has boot 467
in detail; `docs/EXTERNAL-DISPLAY.md` has the full state of the goal.

---

## Three things before you touch anything

1. **PLUG THE TYPE-C CABLE IN BEFORE BOOTING.** The firmware enables the AUX
   engines on a connect path that runs at boot and we never write `AUX_EN`.
   Plugged in afterwards, every AUX read returns refusal 3 on engine "AUX5"
   (= `RB_AUX_COUNT`, none) and it looks like a dead port. 2FB.

2. **`dptrain go tps=3` DEGRADES THIS SINK and the recovery is a REBOOT**
   (2F8, two boots, with a healthy before/after control). The tool refuses
   `tps=3`/`tps=4`. **Use `tps=2`.**

3. **Check the sink before every experiment.** `./build/out/edidread` (header
   OK, checksum OK, `MSI MP2412C`) and `dpcdread 0x0 4` reading
   `12 14 c2 81`. A degraded sink looks exactly like a failing transmitter.
   The chassis must be in iGPU-only / Eco mode.

---

## Where the chain is after boot 467

**The raster honours its own timing. The CLOCK is what is wrong.** 2FD ran the
discriminating experiment: only H_TOTAL changed, 2200 -> 4400, clock held at
148500, refresh asked as 30 so the clock-agreement gate stays honest.

```
  product 2 475 000 (2FB)   496.35 Hz    implied 1228.47 MHz
  product 4 950 000 (2FD)   243.88 Hz    implied 1207.19 MHz
  predicted 248.18 if the OTG honours its timing
```

The rate halved. So the counter *is* driven by our raster — section 2 of 2FB
needs no re-reading — and ~1.2 GHz (2 x the 600 MHz `dprefclk`) is now n=2 at
two different rasters. Still a candidate, not an identification (rule 18).

**Both fields the old handoff pointed at are dead.** All four pipes'
`PIXEL_RATE_CNTL`: `PIXEL_RATE_SOURCE` 0 and `DTBCLK_DTO_DIV` 8 **on the
working panel pipe too**, and `DISPOUT_HALF_RATE_EN` 0 everywhere. Neither
distinguishes our pipe from one that clocks correctly.

**The only programmable field that differs from the panel's pipe is
`PIPE_DTO_SRC_SEL`** — panel 0, ours 1 — and it differs because `rb_dpdto.h`
chose Linux's value. That argument is still right about the source and is not
withdrawn; what is new is measurement against it.

---

## The next experiment, and it costs one boot

The DTO refuses a second write on a live pipe, so this cannot be an in-boot
A/B: the pipe must be dark at the start. Cable in, then:

```bash
./build/out/edidread && ./build/out/dpcdread 0x0 4
./build/out/dmubbl dto 148500 pipe=1 srcsel=0
./build/out/otgprog go 1080p60 pipe=1
```

then measure both counters over >= 20 s against wall time:

```bash
cd /Users/ehsan/Developer/RembrandtGPU
r() { ioreg -rl -c RBAMDFramebuffer | grep -oE '"RBFBOtgR[01]Frames" = [0-9]+' | grep -oE '[0-9]+$' | tr '\n' ' '; }
a=$(r); t0=$(python3 -c 'import time;print(repr(time.time()))'); sleep 25; b=$(r); t1=$(python3 -c 'import time;print(repr(time.time()))'); echo "$a | $b | $t0 $t1"
```

- **60 Hz** → `PIPE_DTO_SRC_SEL` is the factor. Then `rb_dpdto.h`'s default
  changes, with 2FD and this boot as the reason, and the chain can be re-run
  at a rate the sink was told to expect.
- **still ~496 Hz** → SRC_SEL is not it. `DTBCLK_DTO_DIV` 8 is the same on a
  working pipe, so the next place to look is what *feeds* the DTO rather than
  the fields beside it.

**Do not re-run `dig fe` or `dptrain` until the clock is right.** 461 and 464
both produced DPCD `0x202` = `0x00`, and a third one at ~8x the sink's
expected rate carries no information. That is why boot 467 did not run them.

### How to measure the rate

Both counters in one periodic sample, against wall time, over >= 20 s. **The
panel's rate is not a constant** — the DRR ladder moves it, and it read
144.69 Hz in boot 467 against 61.40 in boot 464. Use wall time, never a ratio
to the panel. 2FB got this wrong on its first pass.

---

## Closed in 2FC / 2FD — do not re-open

- **The DIO encoder-type mux.** All six `DIO_LINK{A..F}_CNTL` read zero, before
  and after our writes, with an in-boot base control (PHY0 `0x00194444` out of
  the same aperture). The mux is not steering DIG2 away from PHY2. It is **not**
  "confirmed on DIG": `ENCODER_TYPE_DIG` is zero. `tools/diolook.sh`.
- **`enc31_map_stream_to_link` as the next step.** Wrong name: it is the HPO DP
  128b/132b encoder's and our 8b/10b link never goes through it. This chip's
  DIO stream encoder is `dcn30_dio_stream_encoder_construct` over DCN10's
  register struct (`dcn31_resource.c:1323`, `:361`). The connect that name
  implied was `dcn10_link_encoder_connect_dig_be_to_fe`, **done in 2EC**.
- **`PIXEL_RATE_SOURCE` and `DTBCLK_DTO_DIV` as the clock difference.**
  Identical on the working panel pipe.

---

## After the clock

In order. Each is a thing that does not exist yet:

1. **A stream on DIG2** — read `dcn30_dio_stream_encoder.c`, **not** a dcn31
   file (2FC discharged rule 46 here; the two-file split is 2FA's again).
2. **A surface** on pipe 1 — HUBP, address, swizzle.
3. **A second `IOFramebuffer`.** `getDisplayModeCount()` has returned 1 since
   2sl.

**PHY0 / DIG0 / front end DIGA are the panel's and are refused by name** in
every half of every one of these experiments. Keep it that way.

---

## Tool notes

- `RBFBAuxEn` and `RBFBHpdSink` are **masks**, not indexes (2FB, rule 33).
- `./build/out/pubms` is **not built** by `make tools` — `make pubms`. Nor is
  `dmubbl` — `make dmubbl`. Rule 15 applies to tools.
- `dmubbl dto` takes **kHz** and refuses anything over 1 000 000.
- `otgprog` takes all eight timings piecewise (`hactive= hfront= hsync=
  htotal= vactive= vfront= vsync= vtotal=` with `refresh=`), and its gate
  (refusal **47**) needs `htotal x vtotal x refresh` to equal the live DTO
  PHASE within 0.5 %. **Check your mode is possible before spending the
  boot**: `htotal` must exceed `hactive + hfront + hsync`. The 2FB handoff
  shipped a mode that failed this and would have cost boot 467.
- `otgprog` prints `PANEL CONTROL OK` as its own line. If it says FAILED, the
  kext is not addressing the OTG block and nothing else in its output means
  anything (2FA-a, refusal 48).

---

## Open, off the critical path

- **`OTG_DIO_FIFO_ERROR`** reads 3 on dark pipes, 0 on the panel's, and 2FD
  established it stays **3** on a pipe with the DTO running and the raster
  scanning. So it does not clear on a clock plus an OTG. A stream is untested.
  Published raw, read by nothing.
- **Why DPCD `0x102` stores only from inside `dptrain`'s own call** (~100 us
  after the register writes, n=2 boots). The arm meant to separate movement
  from proximity is **void** — it used `tps=3` and the sink was degrading.
  Re-run on a healthy sink with `tps=2` only.
- **`RDPCSTX*_PHY_CNTL6` needs replacing as `dig`'s verdict register.** It is
  the legacy fallback Linux reads only when DMCUB is absent and it has
  disagreed with `QUERY_DP_ALT` on all five PHYs since 2EB. 2FC's
  `DIO_LINK*_CNTL` is **not** a replacement — it is all zeros.
- **The caller of `set_dio_phy_mux` is not identified.** Neither
  `link_hwss_dio.c` nor `link_hwss_hpo_dp.c` calls it. Must be answered before
  anything writes that register.
- **The wedge** — work submitted that never retires, channel-52 GFX, 3.5 s.
- **`powermetrics` has never been run** against the default 576 interior
  thread_calls/s, the one real argument against the 2D5 default.
- **Ventura and Sonoma claim nothing** in `rb_osver.h`.
