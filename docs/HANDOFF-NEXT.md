# HANDOFF — next step

**Rewritten 2026-11-06 at boot 451 (2ED); next step updated at 2F4 — the clock RUNS, the sink TAKES WRITES, the training pattern is built.** `docs/RULES.md` is the reference card:
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

**Where it stands:** the clock runs on pipe 1 (boot 455), the firmware is
measured NOT to train this link (2F3) so training is ours, and the sink takes
DPCD writes (boot 457). The next step is **2F4's training pattern on the
wire**, built and unrun — item 1 under "Then, in order". It needs an EFI edit
for `-rbgpufbtrain` as well as an install.

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

### DONE: the pixel clock runs. Boot 455 executed the write.

2EF built the read, boots 452 and 453 confirmed the base and reproduced each
other byte for byte (rule 44 satisfied), 2F1 built the three writes, and
**boot 455 ran them.** `OTG1_PIXEL_RATE_CNTL` on pipe 1:

| | word |
|---|---|
| before — 2F0 **and** 2F1, two boots, pipes 1-3 byte-identical | `0x8000c000` |
| the word this kext writes, `rb_dpdto_rate_word(0x8000c000, 1)` | `0x8000d010` |
| after | **`0x8000d090`** |

The difference is **exactly `0x80`** — `DPDTO_ENABLE_STATUS`, which
`rb_dpdto_rate_word()` cannot set because it only touches `DP_DTO_ENABLE` and
`SRC_SEL`. 2F0 attested both of that bit's states on this chip beforehand.
PHASE 148 500 000, MODULO 600 000 000 copied off pipe 0; `DTBCLK_DTO_DIV` 8 and
`DIO_FIFO_ERROR` 3 carried through the RMW untouched.

**The in-boot control** — from the periodic sample, same window, so neither our
own store read back (rule 20) nor a cross-boot comparison (rule 44): pipes 2
and 3 still hold `0x8000c000` byte-identical with PHASE/MODULO 0, and pipe 0 is
untouched in all three registers. **The change is confined to the pipe the dial
named.** The panel kept scanning, which discharges the DOMAIN-collision safety
case by observation rather than by argument.

**It is a clock, NOT a picture.** No OTG timing on pipe 1, no trained link, no
stream mapped to the encoder, no surface. A dark monitor is the expected
reading and is not evidence against any of this (rule 45).

### What is NOT established, so it does not get assumed later

- **What `PIPE_DTO_SRC_SEL` selects.** Linux's 1 was written and the DTO
  started; the firmware's 0 is on the panel's pipe and that works too. Two
  words are not a mechanism (rule 17). `srcsel=0` is still the A/B, but on a
  live pipe it needs a **reboot** first — nothing here turns a DTO off.
- **PHASE as a verdict.** 2F0 measured pipe 0's as a round 343.000000 MHz
  against a raster wanting 342.29, DRR ruled out: PHASE is the clock the
  firmware *requested*. Ours coming back is rule 20's defect and was known in
  advance to agree. Published raw, read by nothing.
- **`DIO_FIFO_ERROR`.** It stayed **3** through the DTO starting, so it does
  not clear on the DTO alone. A reading; nothing consumes it.

### If you need to write again

A second `dmubbl dto` in the same boot is refused **45** — "that pipe's DP DTO
is already enabled" — which is the guard working *and* an independent reading
of the status bit. **Reboot to write again.**

2F2 split the published fields in two, because 2F1's single set let that
refusal republish the successful write as `verdict -1 NOT RUN` with
`before == after`:

- `RBFBDtoCall*` — what the **most recent call** asked for and saw, including
  `RBFBDtoCallLive`, the target pipe's live word. On a refusal this is the only
  thing that call established.
- `RBFBDtoProg*` / `RBFBDtoRate*` — the record of the last call that actually
  **wrote**. A refusal touches none of it.

`RBFBDtoProgCalls` and `RBFBDtoProgRan` are both printed now, so "two calls,
one write" is visible at the tool boundary (rule 14).

**Framebuffer 981AD563** is 2F2; `C2AC37B9` is 2F1 and is what boot 455 ran.
2F2 changes no register and adds no write — it is the instrument fix plus ten
hosttest cases — so installing it is optional and only buys a correct readout
of the next write.

### Then, in order

1. **Link training over AUX2** — driver-side, not DMUB: `link_dp_training.c`
   and `link_dp_training_8b_10b.c` contain **zero** `dmub` references (2BE), so
   the CR/EQ loops, the DPCD writes and the lane settings are ours, over the
   channel 2BB/2BD proved end to end.

   **2F3's DPCD write WORKS** (boot 457): `dpcdwrite 0x100 0x14` on AUX2
   ACKed, with `before 0x06` — `DP_LINK_BW_1_62`, the untrained default — and
   `after 0x14`, HBR2. `applied 1 changed 1`, so the byte that came back is
   not the byte that was there and no rule-20 defect applies. The sink is now
   told to expect HBR2.

   **2F4 built the TRANSMITTER half and no boot has run it.** This is the step
   that turns "the sink will listen" into "this chip is driving something".

   `-rbgpufbtrain` is **not** in this machine's boot-args, so this one needs an
   EFI edit first (rule 43 — `nvram` is overwritten from OpenCore's plist):

   ```bash
   sudo tools/efi.sh bootarg add -rbgpufbtrain
   ```

   ```bash
   sudo tools/fbinstall.sh
   ```

   Reboot, confirm **6A8AE92D** inside the collection (rule 15), then read the
   block before writing anything:

   ```bash
   build/out/dptrain
   ```

   **Check `panel DP0 reads as a TRAINED link: YES` first.** That is the whole
   identification — the firmware trained the panel and it is scanning out, so
   its `DP_LINK_CNTL` must show both `TRAINING_COMPLETE` and `DP_LINK_STATUS`.
   A `NO` there means the block is not what we think it is and the write will
   refuse with code 64; nothing else in the output would be worth reading.

   Then:

   ```bash
   build/out/dptrain go
   ```

   **How to read it, written down first.** `PATTERN_SET` is the **expected**
   verdict: our four registers took cleanly and the sink's lane status did not
   move, which is what should happen while the transmitter is not yet driving
   at the link rate. `SINK_MOVED` means the receiver changed state after our
   pattern — re-run to see if it repeats. `CR_DONE` is more than this step is
   owed and must be re-run before it is believed (rule 44). `NO_STORE` is the
   only reading that condemns the block, and it makes the address the first
   suspect rather than the sink — `clean 0` in particular means an RMW
   disturbed a neighbour, which is 2EC's failure.

   A dark monitor remains the expected reading throughout.

2. **`enc31_map_stream_to_link`** (`link_hwss_dio.c:63`) — a second FE-to-link
   register, not located. Named in 2EC as not reproduced.
3. **A second OTG and HUBP.** The pipe domains need no powering up — 2DE read
   DOMAIN0..3 all `0x10000000`, identical, so that came off the list.
4. **A second `IOFramebuffer`.** `getDisplayModeCount()` has returned 1 since
   2sl and every boot has been read against that; 2xx is what a WindowServer
   that dislikes its framebuffer costs, so this is the step to stage carefully.

### The free measurement, TAKEN (2F3): the firmware does not train this link

With the display plugged in **before power-on**, nothing appears on it — and
this chassis' OpenCore picker is the **text-mode** one, so there was no
graphical pre-boot output to look for. The user reports the same chassis lights
no external display pre-boot **through the 4050 under Windows**, which is the
control: it is the chassis firmware's behaviour, not the 680M's and not ours.
The sink's LED never leaves standby through macOS boot.

**So link training is ours.** The eDP panel's route is not available here: 2wh
wrote HUBP surface addresses onto a pipe the firmware had already lit and
trained nothing.

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
