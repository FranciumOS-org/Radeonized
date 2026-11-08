# HANDOFF — where this is, and what to do now

**Last milestone: 302.** The stream encoder read is built, hosttested and
committed. **One hardware step is pending: install and boot.**

Read this, then `docs/EXTERNAL-DISPLAY.md`. The journal under tags `300`,
`301`, `302` has the evidence.

---

## STEP 1 — install, arm, boot

```
sudo tools/fbinstall.sh
```

Then check the UUID landed **inside the collection** (rule 15 — boot 470 was
lost to exactly this, and 2xx cost nine boots to it):

```
kmutil inspect -B /Library/KernelCollections/AuxiliaryKernelExtensions.kc \
  --show-kext-uuids | grep -i rembrandt
```

The built UUID is **8220AD09-3191-38BB-8D10-4AB3EE94BB57**.

**The arm is already set** from boot 471 — `-rbgpufbdpalt`, `-rbgpufbdig`,
`-rbgpufbaltq`, `-rbgpufbfe`, `wdt=-1`. The stream read shares that set and
needs no new token. Verify it after the boot, from the running system
(rule 43), and read `rbgpu-boot` first (rule 8).

Cable plugged in **at boot** (2FB).

---

## STEP 2 — read the stream encoder

```
tools/strenclook.sh
```

It prints the **gate** before any value and **exits** on `HTOTAL_BAD` or
`BASE_ZERO` rather than printing words under a wrong address.

### Written down before the boot

- **Instance 0 must come back RUNNING, and the base must CONFIRM.** DIG0 is
  the panel: lit, composited, 143.83 Hz native. If either fails, the offsets
  in `rb_strenc.h` are wrong and **nothing else in the sample is a reading** —
  fix the addresses first.
- **DIG2 is expected OFF with blank MSA registers.** Nothing has ever
  programmed them; `grep -rn DP_VID_STREAM src/` found nothing before 302.
  That is **not a null result** — it is the positive identification of what is
  missing from the chain, and instance 0's populated fields are the list of
  what a write would have to set.
- **If DIG2 reads with MSA timing already programmed**, the firmware did more
  at boot than this project has assumed, and the diff against instance 0 names
  exactly which fields differ.
- `SWEEP: UNIFORM` is a **positive defect** here, not an uninterpretable null
  as it is for the DIO mux: instance 0 runs a stream and the others do not, so
  five identical words mean the 0x100 instance arithmetic did nothing.

### The witness, and why it matters more than the values

`DP_VID_STREAM_CNTL` carries `DP_VID_STREAM_ENABLE` (bit 0, what a driver
writes) and `DP_VID_STREAM_STATUS` (bit 16, what the **hardware** answers).
`enc2_stream_encoder_dp_unblank` writes the first then `REG_WAIT`s on the
second, which is upstream stating in its own code that the second is not a
readback of the first.

**This is the first verdict register this line has had.** Everything before it
either answered our own writes — `DIG_BE_CNTL`, `TP_SEL`, `PIXEL_RATE_CNTL`, a
block agreeing with itself — or was the wrong register (`RDPCSTX2_PHY_CNTL6`,
retired in 301). `RBFBStrencEnMask` and `RBFBStrencStatusMask` are published
**separately**, never collapsed: `applied` without `changed` is not evidence
(2E6).

---

## STEP 3 — the write, and what has to be true first

`rb_strenc.h` has **no setter** and nothing may add one until a boot has
returned `BASE_CONFIRMED` with instance 0 `RUNNING` (rule 47 asked of a
write). Once it has, the write set is known and short:

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
