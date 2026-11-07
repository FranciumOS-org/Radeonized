# HANDOFF — next step

**Rewritten 2026-11-06 at boot 451 (2ED); next step updated at 2F1, boot 453.** `docs/RULES.md` is the reference card:
standing rules, chip facts, the boot-arg contract. `docs/JOURNAL.md` is the
evidence record, grepped by milestone tag. **This file is only what is in
flight.**

---

## The machine works. The live line is the EXTERNAL DISPLAY over USB-C.

An armed boot is an accelerated Mac on no boot-args of ours but `wdt=-1`:
Metal bit-exact, `AMD Radeon 680M` / 4 GB / **Metal 3** in System Profiler,
`RBAMDFramebuffer` owning the panel, Apple's `AMDRadeonX6000` compositing for
it, the brightness slider working, a refresh ladder derived from the panel, USB
on real interrupts, and **~137 fps interleaved with the flicker protection
still armed** (2D3's `sub` 4, derived by default since 2D5 — the user's own
"yup all lags fixed, damn" on boot 426, with the control run both ways).

**Requires iGPU-only / Eco mode in the ASUS firmware setup.** Rule 47.

| line | state |
|---|---|
| arming by default, boot sentinel | **DONE** |
| brightness slider + keys | **DONE** (2zw, Eco mode is the precondition) |
| refresh ladder, mode switching | **DONE** (2y5-2z1) |
| GPU name / 4 GB / Metal 3 | **DONE** (2C9-2CE, default-on) |
| the frame-rate ceiling | **CLOSED** — eight models died; it was our own hold, and the lever was retire occasions per vblank (2D0-2D5) |
| efficiency / power | **MOVED OUT** → `~/Developer/Econized` |
| **the external display** | **IN FLIGHT — the three DTO writes are BUILT (2F1) and unrun. Install `C2AC37B9`, reboot, `dmubbl dto`** |
| the wedge | **OPEN**, independent defect |

---

## IN FLIGHT: the external display, and what the next command is

### Established end to end, each by measurement

The port is **HPD instance 2 = AUX engine 2 = UNIPHY_C / phyid 2 = DIG2**, and
four independent instruments say so without deriving from each other: the HPD
sink bit following the cable (2B4), `AUX_CONTROL.AUX_HPD_SEL` (2B9),
`DIG_BE_CNTL.DIG_HPD_SELECT` (2E8), and the VBIOS display-object table in our
own `firmware/680m-vbios.rom` (2E1).

- **Wired to the 680M in Eco mode** — 2B4, with the internal panel's HPD0 as
  the control that never moved.
- **The sink answers our own transactions** — 2BB read a full DPCD (DP 1.2,
  HBR2, **2 lanes**, and `DP_EDP_CONFIGURATION_CAP` 0x00 proving it is not the
  laptop's own panel replying), 2BD a full EDID over I2C-over-AUX: **MSI
  MP2412C**, native **100 Hz** at 228 MHz, which fits the ~8.64 Gbps link.
- **DP alt mode IS granted** — 2EB's `QUERY_DP_ALT` matched the ROM's
  `DP_IS_USB_C` bit for bit across all five phys, with `is_dp4` independently
  agreeing with both the sink's lane count and the panel's four MPLLs.
- **The firmware EXECUTES `DMUB_CMD__VBIOS` commands** — 2E9's
  `DIG_BE_EN_CNTL.DIG_ENABLE` 0→1 on the targeted instance alone, reproduced on
  boot 451 (2ED), so rule 44 is satisfied.
- **…and ANSWERS them** — 2EB's reply path at REGION4. Note `ret_status` stays
  **set**, which Linux's own criterion would have discarded.
- **The firmware configured that encoder for this sink** — `DP_CONFIG` = 2
  lanes on DIG2, 4 on the panel's DIG0, 0 on the three never brought up (2E7).
- **The front end is connected, by our own write** — 2EC/2ED,
  `DIG_BE_CNTL.DIG_FE_SOURCE_SELECT` 0 → 0x4, applied/changed/clean all 1.
  **The first verified write this project has made to this block.**

### THE NEXT STEP: run the write. It is built; no boot has executed it.

2EF built the read, boot 452 confirmed it (2F0) and **boot 453 reproduced that
read byte for byte** — PHASE[0] 343.000000 MHz, MODULO 600 000 000 exactly,
pipes 1-3 byte-identical at `0x8000c000`, the alias control reading
`DOMAIN0/1_PG_STATUS` as `0x10000000` at BASE_IDX 2. So **rule 44 is satisfied
for the base confirmation the write's safety rests on.**

**2F1 built the three writes and ran none of them.** Framebuffer
**C2AC37B9**; `6B64DDD5` is what is installed. `-rbgpufbdto` is already in this
machine's boot-args, so this is an install and a reboot, nothing else:

```bash
sudo tools/fbinstall.sh
```

Then reboot, confirm the loaded binary **inside the collection** (rule 15):

```bash
kmutil inspect -B /Library/KernelCollections/AuxiliaryKernelExtensions.kc --show-kext-uuids | grep -i rembrandt
```

It must read **C2AC37B9**. Then:

```bash
build/out/dmubbl dto
```

That programs pipe 1 — the first pipe that is **not** the panel's, and *not* a
derived pipe-to-Type-C mapping, which this project has never established — with
2BD's preferred timing (148.5 MHz, off this sink's own EDID; its 100 Hz mode is
`dmubbl dto 228000`). `dto help` lists `pipe=` and `srcsel=`.

### What the write does, and what bounds it

Three registers at **BASE_IDX 1 = 0xc0**, in Linux's own order (PHASE, MODULO,
then the enable, as an RMW that keeps `DTBCLK_DTO_DIV` and the error bits —
what `REG_UPDATE_2` compiles to):

1. `DP_DTO1_PHASE` = the mode's pixel clock in Hz.
2. `DP_DTO1_MODULO` = **copied off pipe 0**, not written down. PHASE/MODULO is a
   **ratio**, so reusing the firmware's own reference makes pipe 1 as accurate
   as the panel's and **the true reference never has to be known** — which is
   also why 2F0's open ~0.2 % DPREFCLK question does not block this.
3. `OTG1_PIXEL_RATE_CNTL`: `DP_DTO1_ENABLE`, plus `PIPE1_DTO_SRC_SEL`.

**The gate is THIS BOOT'S OWN base verdict**, re-sampled inside the call: the
write is refused unless PHASE[0] reproduces the live raster at 0xc0. At the
other candidate base these offsets are the power gating of the pipe scanning
the panel. Pipe 0 is refused by name in the kext *and* the tool; a pipe whose
DTO is already enabled is refused independently.

**`SRC_SEL` is decided, not defaulted away.** Linux writes 1, the firmware left
pipe 0's at 0 and the panel works. The default is **1**, because
`dcn31_program_pix_clk` is the *driver* path for a DP pipe programmed from
scratch — this situation — while the firmware's 0 came from atom bytecode on
eDP. `srcsel=0` tries the other side, interleaved in the same boot (rule 44).

### How to read the result — written down before the boot

- **STARTED** (`RBFBDtoProgVerdict` 1): `DPDTO_ENABLE_STATUS` went 0 → 1. That
  is the hardware's own bit, not one we wrote, and 2F0 attested both its states
  on this chip first. **It means a clock, NOT a picture** — there is no OTG
  timing on pipe 1, no trained link, no stream mapped to the encoder and no
  surface, so a dark monitor is the **expected** reading.
- **STILL** (0): the enable read back and the status bit did not follow, with
  `changed` 1 saying the word moved. A real negative about this block.
  Candidates, none measured: something else needs enabling (DISPCLK to the
  pipe, `OTG_PIXEL_RATE_SOURCE`), or `SRC_SEL` is the wrong way round — one
  interleaved command away.
- **Refusal 42**: nothing was written and the gate worked. Read `RBFBDtoBase`,
  `RBFBDtoExpectedHz`, `RBFBDto0Phase`.

**Do NOT read PHASE back as the verdict.** 2F0 measured pipe 0's as a round
343.000000 MHz against a raster wanting 342.29, DRR ruled out — PHASE is the
clock the firmware *requested*, so our own number coming back is rule 20's
defect and is known in advance to happen. It is published raw and read by
nothing. `OTG_DIO_FIFO_ERROR` is likewise still unusable: 3 on every dark pipe
and 0 on pipe 0, never read on a pipe being brought up.

### Then, in order

1. **Link training over AUX2** — driver-side, not DMUB: `link_dp_training.c`
   and `link_dp_training_8b_10b.c` contain **zero** `dmub` references (2BE), so
   the CR/EQ loops, the DPCD writes and the lane settings are ours, over the
   channel 2BB/2BD proved end to end.
2. **`enc31_map_stream_to_link`** (`link_hwss_dio.c:63`) — a second FE-to-link
   register, not located. Named in 2EC as not reproduced.
3. **A second OTG and HUBP.** The pipe domains need no powering up — 2DE read
   DOMAIN0..3 all `0x10000000`, identical, so that came off the list.
4. **A second `IOFramebuffer`.** `getDisplayModeCount()` has returned 1 since
   2sl and every boot has been read against that; 2xx is what a WindowServer
   that dislikes its framebuffer costs, so this is the step to stage carefully.

### Free measurement nobody has taken

**Plug the display in before power-on** and watch for the OpenCore picker or
the Apple logo on it. The firmware's GOP drives whatever it is wired to, so an
image there says the firmware trained the link — which is the whole reason the
internal panel was tractable (2wh wrote HUBP surface addresses onto a pipe the
firmware had already lit, and never trained anything). No image means training
is ours, which is item 1 above.

### Two chassis traps that have each cost milestones

- **Hybrid vs Eco mode** (rule 47). Eco takes the dGPU off the PCI bus
  entirely. HDMI-A is dGPU-wired and so is mutually exclusive with brightness.
- **The Type-C subsystem can DIE** in a way that survives a reboot *and* a
  reinstall, cleared only by a **30-second power-key hold** (2D8). Three
  milestones were spent explaining a correct null. The cheap proxy is
  `ioreg -p IOUSB`: a port enumerating nothing on a data cable means the
  subsystem is down, which no DCN register can tell you. **If HPD2 reads 0 on a
  cable the user says is in, ask about the power-key hold before reading the
  code.**

---

## OPEN, not in flight

- **The wedge** — work submitted that never retires, on GFX/SDMA/compute user
  queues. Independent second defect; this is the "random lag" the user reports.
  2wt's `--queues 4` trigger no longer fires (14-49 ms where it cost 6.1-10.2
  s), but boot 409 logged a real one: channel 52 GFX, stamp frozen at 0x27a1b,
  `GPURestartBegin`→`End` spanning **3.5 s**. `stamp_ticks` 125666 retires the
  "nothing polls it" family. Rarer, not gone.
- **`powermetrics` has NEVER been run** against the 2D5 default, which arms
  **576 interior thread_calls/s** at native. This is the one real argument
  against that default: 2C8 showed `idle %` cannot resolve it (97.5-98.4 % from
  144 to 720 occasions/s with `sys` *decreasing*), and the cost the user cares
  about is deep-package idle residency, which `idle %` cannot see at all.
- **Ventura and Sonoma claim nothing** in `src/common/rb_osver.h`, which is the
  honest state. Sequoia claims the object offsets and a cave row; Tahoe claims
  both plus the USB table.
- **The 2C2 drain dial bug** — `drainset 1` returned SUCCESS three times and
  `vblret_drain` stayed 8. Undiagnosed; 2D1 found a live candidate cause (the
  diag refresh dropping rather than deferring) and instrumented it
  (`drain_recv`/`drain_want`/`drain_sets`), still unexercised. The negative
  result it sits under — polling `checkTimestamps()` harder is measured dead —
  is unaffected.
- **`UPSTREAM.md` entries owed** for recently-read Linux references:
  `link_hwss_dio.c`, `link_dpms.c`, `command_table_helper2_dce112.c`,
  `dal_asic_id.h`, `dpcs_4_2_0_offset.h`, `dcn31_dio_link_encoder.c`.
- `com.nullmoth.NVRMAGDC` unaccounted for on this machine.
- The `/tmp/rb253` git worktree still needs cleaning up.
