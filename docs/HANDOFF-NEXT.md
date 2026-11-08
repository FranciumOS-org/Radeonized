# HANDOFF — where this is, and what to do now

**Last milestone: 302.** The stream encoder read is built, hosttested and
committed. **One hardware step is pending: install and boot.**

Read this, then `docs/EXTERNAL-DISPLAY.md`. The journal under tags `300`,
`301`, `302` has the evidence.

---

## STEP 1 — install and boot 473

Boot 472 read the block and the addresses are **confirmed**. It refused on a
convention (MSA holds the true total, the OTG register holds `total - 1`), that
is fixed, and **the corrected build has never run**.

```
sudo tools/fbinstall.sh
```

Check the UUID **inside the collection** (rule 15 — boot 470 was lost to
exactly this):

```
kmutil inspect -B /Library/KernelCollections/AuxiliaryKernelExtensions.kc \
  --show-kext-uuids | grep -i rembrandt
```

Built UUID: **D4D18678-93E7-3CB3-9360-CD48C6406A0F**.

The arm is already set — `-rbgpufbdpalt`, `-rbgpufbdig`, `-rbgpufbaltq`,
`-rbgpufbfe`, `wdt=-1`. Cable in **at boot** (2FB). Read `rbgpu-boot` first
(rule 8).

```
tools/strenclook.sh
```

**Two things must both hold before anything is written** (rule 47 asked of a
write): `RBFBStrencBase` = **CONFIRMED** and `RBFBStrencActiveAgree` = **1**,
in the same boot. The second is the stronger of the two — DRR cannot move the
active raster, so it needs no window.

---

## STEP 2 — what boot 472 already established

| | |
|---|---|
| addresses | **confirmed** three ways: the totals at +1, `MSA_PARAM4` = 1920x1080, `MSA_PARAM2` VSTART 20 |
| panel instance | **RUNNING**; `En`/`Status`/`Run` masks all `0x01`; sweep DIFFERS |
| DIG2's MSA registers | **all blank** — nothing has ever programmed a stream there |

And **two per-instance differences on DIG2**, each with a four-instance
in-boot control, on registers this kext had never read:

| register | DIG0 (panel) | DIG1/3/4 | **DIG2** |
|---|---|---|---|
| `DP_STEER_FIFO_RESET` | 0 | 0 | **1** |
| `DP_VID_STREAM_DIS_DEFER` | 2 | 2 | **0** |

The steer FIFO carries the stream encoder's symbols toward the link, and
`enc2_stream_encoder_dp_unblank` pulses that bit 1 then 0 — so a resting 1 is a
**held reset** on the exact instance thirteen boots have been driving.
`DIS_DEFER` = 2 is the reset value (three never-brought-up instances read it),
so DIG2's 0 was **written by the firmware at boot**.

**Neither is established as the blocker, and do not write it down as one.**
DIG2 is the only instance with a cable attached, so a different firmware path
there is expected (rule 47), and **no stream has ever been programmed on DIG2
at all** — a held FIFO reset cannot be shown to block symbols that do not
exist (rule 18 on n=1). What it does establish: the firmware did more on DIG2
at boot than this project assumed, and the write set now has two values to
**clear** rather than set — which is what a write designed off instance 0's
live words catches and one designed off upstream alone would not (rule 25).

---

## STEP 3 — the write, and what has to be true first

`rb_strenc.h` has **no setter** and nothing may add one until a boot has
returned `BASE_CONFIRMED`, `ActiveAgree` **1**, and instance 0 `RUNNING`
(rule 47 asked of a write). Boot 472 gave the third and the addresses; boot 473
is the test of the first two. Once they hold, the write set is known and short,
and **every field now has a live source in instance 0's own words**:

1. **`setup_dio_stream_attribute`** → `enc1_stream_encoder_dp_set_stream_attribute`
   (`dcn10_stream_encoder.c:245`): eight registers — `DP_PIXEL_FORMAT`,
   `DP_MSA_COLORIMETRY`, `DP_MSA_MISC`, `DP_MSA_TIMING_PARAM1..4` — plus
   `enc2`'s one addition, `DP_SEC_FRAMING4.DP_SST_SDP_SPLITTING`.
2. **`enc2_stream_encoder_dp_unblank`** (`dcn20_stream_encoder.c:490`): five —
   `DP_VID_N`, `DP_VID_M`, `DP_VID_TIMING`, `DP_STEER_FIFO` (reset pulse),
   `DP_VID_STREAM_CNTL` — with `DIG_FE_CNTL.DIG_START` toggled in between.

Every value has a live source: instance 0's own words for the shape, and the
sink's EDID preferred timing (148 500 kHz, already used by `dmubbl dto`) for
the numbers. **DP0/DIGA is the panel's and must be refused by name** in both
halves, as in every experiment on this line.

**And two values on DIG2 have to be CLEARED, not set** — boot 472's finding,
and neither is in upstream's write list because upstream never inherits a
firmware-touched instance: `DP_STEER_FIFO_RESET` (held at 1) and whatever
`DP_VID_STREAM_DIS_DEFER` 0 implies against everything else's 2. The unblank
sequence pulses the FIFO reset anyway, so step 2 covers the first by
construction; the second needs deciding before it is written, not during.

**`setup_dio_stream_encoder` is NOT part of this.** Its `enable_stream`,
`map_stream_to_link`, `set_input_mode` and `enable_fifo` are all absent from
`dcn30_str_enc_funcs`, so it reduces to `connect_dig_be_to_fe` — done in 2EC.

Still absent after that: a **surface** on pipe 1 (HUBP, address, swizzle) and
a second `IOFramebuffer` (`getDisplayModeCount()` has returned 1 since 2sl).

---

## The state of the blocker

**Not named, and the candidate list is the shortest it has ever been.**

| candidate | killed by |
|---|---|
| the second raster | 2FD — OTG1 honours its timing |
| the pixel clock | 2FE — `PIPE_DTO_SRC_SEL`, ratio 0.9997 at `srcsel=0` |
| the DIO encoder-type mux | 2FC — all six zero, closed as a negative |
| `hpdsel` | 300 — in-boot A/B, both sides acked, PHY2 unmoved |
| power gating | 300 — `RBFBPgCandidate` -1, `dmubbl dom` refusal 24 |
| "the analog PHY is off" | 301 — the reading was the wrong register |

301 retired `0x00063333` with positive evidence: in one sample,
`RDPCSPIPE0_PHY_CNTL6` read not-disabled and **agreed** with the ROM-attested
`QUERY_DP_ALT`, while `RDPCSPIPE1` disagreed (so the agreement discriminated
rather than passing on an all-zero field) and the sweep reported DIFFERS.
`RDPCSTX2`'s word contradicts both the independent instrument and the register
Linux actually reads for UNIPHY_C.

**Lane power remains unanswered in either direction.** `RDPCSPIPE_PHY_CNTL6`
defines three DPALT bits and nothing else — no MPLL, no reference clock, no
PSTATE. Do not report any reading from it as an answer to that question.

---

## Hard constraints that have each cost a boot

- **`dptrain go tps=3` / `tps=4` DEGRADE this sink**, recovery is a **reboot**
  (2F8). The tool refuses them. **Use `tps=2`.** All of 2F7 was the aftermath.
- **PHY0 / DIG0 / front end DIGA / DP0 are the panel's** and are refused by
  name in every half of every experiment on this line.
- **The chassis must be in iGPU-only / Eco mode** (rule 47). `./build/out/blset`
  reports the mode first; a `vendor-id` `0x10de` on the bus means hybrid.
- `make plugin`, `make dmubbl` and `make pubms` are **their own targets** —
  `make kext tools` builds none of them (2E9 lost a reading to this).
- **Check the UUID in the COLLECTION, not `/Library/Extensions`** — boot 470.
- The Type-C subsystem on this chassis can die in a way that survives a reboot
  and is cleared only by a **30-second power-key hold** (2DD).
- **The break-it harness reads stderr too.** 302's sweep hid a caught break by
  grepping only stdout; `-Werror` writes to stderr and `make hosttest` then
  runs the previous binary with zero FAIL lines. Rule 48's trap, sixth time.

---

## Also open, off the critical path

DPCD `0x204` = `0x80` (`LINK_STATUS_UPDATED` with `INTERLANE_ALIGN_DONE`
clear, n=1, sticky). `OTG_DIO_FIFO_ERROR` stays 3 on a pipe with the DTO
running and the raster scanning — **untested with a stream, which 302 now
makes testable**. Why DPCD `0x102` stores only from inside `dptrain`'s own
call (the old arm is void: it used `tps=3`). The caller that programs
`DIO_LINK*_CNTL` is not identified. The **wedge** (channel-52 GFX, 3.5 s, the
"random lag"). `powermetrics` has never been run against the 576 interior
thread_calls/s. Ventura and Sonoma claim nothing in `rb_osver.h`.
