# HANDOFF — next step

**Rewritten 2026-11-06 at boot 451 (2ED); next step updated at 2F0, boot 452.** `docs/RULES.md` is the reference card:
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
| **the external display** | **IN FLIGHT — base CONFIRMED on boot 452; the three DTO writes are next (2F0)** |
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

### THE NEXT STEP: the three DTO writes on pipe 1 (the read came back CONFIRMED)

2EF built the read and **boot 452 confirmed it** (2F0): `RBFBDtoBase` = 0,
`RBFBDtoSeg1` = 0xc0, `RBFBDtoPipe` = 0. **BASE_IDX 1 = 0xc0 is established on
the chip**, and the alias control read `DOMAIN0/1_PG_STATUS` as `0x10000000` at
BASE_IDX 2 — the value 2DE measured, so the collision was real and is now
demonstrated, not argued.

What the boot handed over:

- **`dprefclk` = 600 000 000 Hz exactly** (`MODULO[0]`), read from the chip.
  Linux takes it from the SMU and this project had never located it.
- **The panel's DTO drives the panel** — `DP_DTO0_ENABLE` and
  `DPDTO0_ENABLE_STATUS` both set, which confirms 2EE's source read by
  measurement rather than by reading `dc`.
- **Pipes 1-3 are byte-identical and dark** (`0x8000c000`, PHASE and MODULO 0),
  so pipe 1 is a clean target with a three-instance control already taken.

### The write, and the two things 2F0 says about it

Three registers at **BASE_IDX 1 = 0xc0**, pipe 1 (dwords `0x84`/`0x85`/`0x86`
before the base), for the sink's mode from 2BD's EDID:

1. `DP_DTO1_PHASE`  = the mode's pixel clock in Hz.
2. `DP_DTO1_MODULO` = **600 000 000** — the firmware's own value for the panel.
   PHASE/MODULO is a RATIO, so reusing it makes pipe 1 as accurate as pipe 0
   and the true reference never has to be known. Linux does the same: one
   `dp_dto_ref_khz` for every pipe.
3. `OTG1_PIXEL_RATE_CNTL`: set `DP_DTO1_ENABLE` (bit 4).

**`PIPE1_DTO_SRC_SEL` (bit 12) is the open decision.**
`dcn31_program_pix_clk` writes it to 1 alongside the enable. The firmware left
pipe 0's at **0** and the panel has scanned out for 452 boots, so 0 is
sufficient on this chip by existence proof. Evidence is on the side of copying
the firmware; decide it explicitly, and if both are tried, interleaved.

**Do NOT use PHASE as the verdict register.** 2F0 measured pipe 0's PHASE as a
round 343.000000 MHz against a raster wanting 342.288 MHz — 0.208 %, with DRR
extension ruled out (`V_TOTAL_MIN/MAX/MID/CNTL` all 0). PHASE is the clock the
firmware *requested*, not a readback of what the hardware achieved, so a
readback agreeing with what we wrote is rule 20's defect and we know in advance
it would agree. Candidate verdict registers instead: `DPDTO1_ENABLE_STATUS`
(the hardware's own answer, which pipe 0 shows tracking the enable) and
`OTG1_DIO_FIFO_ERROR`, which reads 3 on every dark pipe and 0 on the live one —
**untested against a pipe being brought up, so establish it before trusting
it.**

The write needs a target dial the way every write in this kext does, and the
mode is 2BD's: **MSI MP2412C, 228 MHz, 100 Hz, 2 lanes.**

Why the PHY still reads `STILL` and why that is not an obstacle: a transmitter
with an enabled back end and a connected front end but no clock has nothing to
light; and **the PHY register may be the wrong instrument entirely** — 2E9
found this command's product sits one layer above RDPCSTX, and 2EB found
`RDPCSTX2_PHY_CNTL6` contradicting the chip's own alt-mode answer, making it the
legacy fallback Linux reads only when DMCUB is absent. Pick the verdict register
from the layer the command acts on.

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
