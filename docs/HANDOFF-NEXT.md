# HANDOFF — what to do now

**State: 304 is built and not yet booted.** Framebuffer UUID
**`4ADE62B7-8AF9-3895-970D-2A0737EF487F`**. Read-only. No setter exists, no new
boot-arg token was added, and nothing on this line writes yet.

---

## Step 1 — install and boot it

**The cable must be plugged in at boot** (2FB). Plugged in afterwards every
AUX read returns refusal 3 and `AUX_EN` reads clear, because the firmware's
connect path is what writes it, at boot.

```
sudo tools/fbinstall.sh
```

Then check the UUID **inside the collection**, not in `/Library/Extensions` —
different facts, and 2xx cost nine boots to this exact gap (rule 15):

```
kmutil inspect -B /Library/KernelCollections/AuxiliaryKernelExtensions.kc --show-kext-uuids | grep -i rembrandt
```

It must read `4ADE62B7-8AF9-3895-970D-2A0737EF487F`. Reboot, then:

```
tools/strenclook.sh
```

## Step 2 — the one number this boot is for

**`RBFBStrencMsaMismatch`.** The tool prints it first and does not recompute
it.

- **0** — the derivation reproduced DIG0's four live MSA words bit for bit.
  The arithmetic that would program DIG2 is correct on hardware that is
  actually running a DP stream. **This is what licenses the write** (rule 47
  asked of a write), and Step 3 may proceed.
- **4 (PARAM3 alone)** — a **genuine finding, not a regression.** The
  hosttest's sync-width and polarity inputs were back-derived from PARAM3's own
  expected output, which is circular (rule 17), so this boot is the first real
  check of those fields. Read `RBFBStrencP0Otg2/3/6/7` — the raw
  `OTG_H_SYNC_A`, `OTG_H_SYNC_A_CNTL`, `OTG_V_SYNC_A`, `OTG_V_SYNC_A_CNTL`
  words — and fix the derivation against them. Do not write anything until it
  is 0.
- **16** — the derivation was **refused**, not disagreed with. Read
  `RBFBStrencP0DeriveRc`: 1 no args, 2 total zero, 3 blank zero, 4 blank
  `END >= START`, 5 active zero, 6 active >= total, 7 sync width zero, 8 sync
  START non-zero, 9 field overflow. "Could not derive" is not "derived and
  agreed" (rule 14).
- **1, 2 or 8** — before concluding the addresses are wrong, **check the
  conventions**. Boot 472 refused the base check for the off-by-one alone: the
  OTG register holds `total - 1`, MSA holds the true total.

Also worth recording from the same sample, costing nothing: whether
`RBFBStrencP1DeriveRc` is 0, i.e. whether **pipe 1 has a raster good enough to
derive a stream from at all.** It has no control and must not be treated as
one.

## Step 3 — the write, once the mismatch is 0

The write set is known, short, and every field now has either a live source in
instance 0's own words or a formula validated against them.

1. `setup_dio_stream_attribute` -> `enc1_stream_encoder_dp_set_stream_attribute`
   (`dcn10_stream_encoder.c:245`): `DP_PIXEL_FORMAT` (RGB444 + 8bpc, as the
   panel's `0x01000000`), `DP_MSA_COLORIMETRY` (MISC0 `0x20` = 8bpc sRGB, as
   the panel's `0x20000000`), `DP_MSA_MISC`, and the four
   `DP_MSA_TIMING_PARAM1..4` **from `rb_strenc_derive_msa()` on pipe 1**, plus
   `enc2`'s one extra write, `DP_SEC_FRAMING4.DP_SST_SDP_SPLITTING`.
2. `enc2_stream_encoder_dp_unblank` (`dcn20_stream_encoder.c:468`):
   `DP_VID_M_N_GEN_EN` **0**, then `DP_VID_N` `0x8000` and `DP_VID_M`
   **9011** (148500 kHz over HBR2), then `GEN_EN` back to **1** — M is
   hardware-generated and the written value only **primes** it, which is why
   the panel's own M drifted `0xa2a6` -> `0xa2a4` between boots and must never
   be copied. Then `DP_VID_STREAM_ENABLE` 0 and **`REG_WAIT` on bit 16
   `STATUS`, not bit 0** — that split is the witness this line has lacked
   since 2EB. Then `DIG_FE_CNTL.DIG_START` 1, delay, 0. Then
   `DP_STEER_FIFO_RESET` 1, delay, **0** — which is what clears DIG2's held
   reset, by construction rather than as a special case. Then
   `DP_VID_STREAM_ENABLE` 1 and read `STATUS` back.
3. **Do not program `DP_VID_STREAM_DIS_DEFER`.** DIG2's 0 against everything
   else's 2 is real and firmware-written, but `dcn20_stream_encoder.c:541`
   says *"this register has no effect on enable transition"* and upstream
   stopped writing it. It is retired as a candidate blocker.

**Arming it** (rule 30's scan is already done): `-rbgpufbstrm` is **clean** —
not an existing token and not a prefix of one, and neither is the derived
negative `-rbgpunofbstrm`. Opt-in tokens are not covered by
`rb_defaults.h`'s hosttest, so that manual scan is the check. Nothing may be
written until a tool names a target, pipe 0 / DIG0 / PHY0 / front end DIGA /
DP0 are **refused by name in both halves**, and the write is declined unless
**this boot's own sample** returned `RBFBStrencMsaMismatch` 0 — the arm is part
of the experiment (rule 34).

## What is still missing after that

A **surface** on pipe 1 (HUBP, address, swizzle) and a second
`IOFramebuffer` — `getDisplayModeCount()` has returned 1 since 2sl. A stream
with no surface carries blank pixels, which is a real intermediate result and
should be read as one, not mistaken for failure.

## Hazards on this line, unchanged

- **`dptrain go tps=3`/`tps=4` DEGRADE this sink and the recovery is a
  reboot** (2F8). The tool refuses them. Use `tps=2`.
- The Type-C subsystem can die in a way that survives a reboot and a
  reinstall, cleared only by a **30-second power-key hold** (2DD).
- The chassis must be in **iGPU-only / Eco mode** (rule 47). `./build/out/blset`
  reports the mode first.
- **Never `kmutil unload` the framebuffer kext — it PANICS** (2sq). Reboot.
