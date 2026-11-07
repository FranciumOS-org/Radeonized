# Handoff — next step

**Written 2026-11-07, after 2FA. Last boot: 461 (2F9).**

Read `CLAUDE.md` first, then this. `docs/EXTERNAL-DISPLAY.md` has the full
state of the one open goal; `docs/JOURNAL.md` under tag `2FA` has the detail
behind everything below.

---

## Two warnings before you touch anything

1. **`dptrain go tps=3` DEGRADES THIS SINK and the recovery is a REBOOT.**
   2F8, two boots, with a healthy before-and-after control. A sink with a good
   EDID and a readable DPCD goes to `00 00 00 81 ... 84 00` with `0x202`
   DEFERring, immediately after one run. `tps=2` does not bring it back, nor
   `0x102`=`0x00`, nor `0x600`=`0x01`; further writes make it worse. All of
   2F7 was the aftermath of this, and its two dead hypotheses were killed by
   invalid data. The tool now refuses `tps=3`/`tps=4` (exit 3). **Use
   `tps=2`.**

2. **Check the sink is healthy before every experiment**, with
   `./build/out/edidread` — header OK and checksum OK — and `dpcdread 0x0 4`
   reading `12 14 c2 81`. A degraded sink looks exactly like a failing
   transmitter. The user's dGPU being powered on also blanks these reads
   (chassis must be in iGPU-only / Eco mode, rule 47).

---

## The state of the chain

Boot 461 (2F9) ran **the whole of the rest of the chain in one boot**, each
step with its own in-boot control, and **the sink saw nothing**:

- `dmubbl dig fe` — DIG2's back end enabled, front end connected
  (`DIG_BE_CNTL` `0x00010000 -> 0x20000000`, `DIG_BE_EN_CNTL` `0x100 -> 0x101`;
  DIG0/1/3/4 byte-identical as controls).
- `dmubbl dto 148500` — pixel clock running (`PIXEL_RATE_CNTL`
  `0x8000c000 -> 0x8000d090`, `DPDTO_ENABLE_STATUS` `0 -> 1`). **kHz, not Hz** —
  the tool now refuses an argument over 1 000 000.
- `dptrain go tps=2` — `TP_SEL 0 -> 1` on DP2.
- **DPCD `0x202` read `0x00` before and after.** No clock recovery.
- `RDPCSTX2_PHY_CNTL6` never moved, and re-sending the transmitter ENABLE
  *after* the clock was running did not move it either, so **order is not the
  missing thing**.

**The atom transmitter ENABLE at the link rate already exists.** `dmubbl dig`
carries `RB_DIG_TYPEC_SYMCLK` 54000 = HBR2 and has executed since 2E2. An
earlier handoff of mine called it the blocker without reading the tool that
sends it. Do not repeat that: read the tool before naming a blocker.

---

## Why the next step is the raster, not the transmitter

`0x202` reading `0x00` is **not** evidence against the transmitter or the PHY.
There is no raster on pipe 1 (`OTG1_CONTROL` `0x300` = off, in every boot
since this line opened, against OTG0's `0x00011201`) and no stream mapped to
DIG2. A clock with no raster and no stream has nothing to send, so `0x00` is
the expected reading — rule 47 in its general form.

It is also the only remaining step with a **monotonic** verdict available
(rule 31): `OTG1_FRAME_COUNT` advancing, rather than a readback a working
block with nothing attached would produce identically.

---

## 2FA is built and never run. Do this.

`src/common/rb_otgprog.h` + the kext fields/writes/dial + `tools/otgprog.m`.
`make hosttest` green, fbkext UUID **`22977266-BB99-3D16-B151-0DA3BB6DF416`**.

**The arm is `-rbgpufbraster`.** Not `-rbgpufbotg` — that is refused, because
`-rbgpufbotgany` already exists and would have it as a prefix (rule 30; the
scan was verified with `-rbgpufbdtox` as a known-colliding control). The dial
does **not** arm `-rbgpufbdto` for you: refusal 45 names the missing clock
instead, deliberately.

### The two commands for the user

```
sudo tools/efi.sh bootarg add -rbgpufbraster
```

```
sudo tools/fbinstall.sh
```

Then verify the UUID **inside the collection**, not in `/Library/Extensions`
(rule 15 — 2xx cost nine boots to this exact gap):

```
kmutil inspect -B /Library/KernelCollections/AuxiliaryKernelExtensions.kc --show-kext-uuids | grep -i rembrandt
```

Expect `22977266`. Then reboot.

### In the armed boot, in this order

1. **Rule 8 first**: `rbgpu-boot` must have increased, or nothing in NVRAM is
   this boot's. Then `rbgpu-boot`, `kern.boottime`, and the UUID again.
2. `./build/out/otgprog` with no arguments — the survey. **OTG0 must read
   `0x00011201` with `MASTER_EN` and a frame count that ADVANCES across two
   samples.** That is the positive control for the entire instrument. If the
   tool prints "the LOADED kext does not publish RBFBOtgR\*", the install did
   not take and nothing below means anything.
   (That line exists because the first smoke run printed `MASTER_EN` for all
   four pipes off absent keys read as `-1` — rule 14's named trap, in a tool
   written with the rule in front of me.)
3. Check the sink is healthy (above) and `-rbgpufbdto` is armed, or
   `otgprog` will refuse 45.
4. `dmubbl dto 148500` — the clock first, because the raster's write gate
   wants it.
5. `./build/out/otgprog 1080p60 pipe=1 go` — the raster. Pipe 0 is refused by
   name in the tool and in the kext.
6. **The verdict is `RBFBOtgR1Frames` advancing across periodic samples**, not
   the register readback and not a sleep-and-compare of the dialled value
   (rule 31, which has cost four false MISMATCHes). The tool polls it.

### What a null would and would not mean

If the writes land, read back, and `OTG1_FRAME_COUNT` **does not advance**,
that is a real finding and it is about the enable, not the timing — check
`otgProgVerdict` against `V_NOT_ENABLED` and read `OTG1_CONTROL`'s
`MASTER_EN` bit in the after-sample. If the frame count **does** advance, the
raster exists and the next question is the stream.

---

## After the raster

In order, and each is a thing that does not exist yet:

1. **`enc31_map_stream_to_link`** — no stream is mapped to DIG2. Discharge
   rule 46 by source in `dcn31_resource.c` first, as 2FA did for the OTG
   path; the OPTC split (timing from DCN10, enable from DCN31) says not to
   assume the DIO functions all come from one file either.
2. **A surface** on pipe 1 — HUBP, address, swizzle.
3. **A second `IOFramebuffer`.** `getDisplayModeCount()` has returned 1 since
   2sl.

**PHY0 / DIG0 / front end DIGA are the panel's and are refused by name** in
every half of every one of these experiments. Keep it that way.

---

## Open, off the critical path

- **Why DPCD `0x102` stores only from inside `dptrain`'s own call** (~100 µs
  after the register writes, n=2 boots) and not from a separate call with the
  TX unmoved. The arm that was meant to separate movement from proximity is
  **void** — it used `tps=3` and the sink was already degrading. Re-run on a
  healthy sink with `tps=2` only.
- **`RDPCSTX*_PHY_CNTL6` needs replacing as `dig`'s verdict register.** It is
  the legacy fallback Linux reads only when DMCUB is absent, and it already
  disagreed with `QUERY_DP_ALT` on all five PHYs. It is as much a suspect as
  the PHY is.
- **The wedge** — work submitted that never retires, channel-52 GFX, 3.5 s,
  the "random lag".
- **`powermetrics` has never been run** against the default 576 interior
  thread_calls/s, which is the one real argument against the 2D5 default.
- **Ventura and Sonoma claim nothing** in `rb_osver.h`.
