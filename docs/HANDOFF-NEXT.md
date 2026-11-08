# HANDOFF — where this is, and what to do next

**Last milestone: 300.** Boot 469 ran, three candidates died, and then the
project's own tooling showed that the conclusion five boots had been building
on was read off the wrong register.

**Read this file, then `docs/EXTERNAL-DISPLAY.md`'s 300 section.** The journal
under tag `300` has the full evidence.

---

## The state in one paragraph

An armed boot is an accelerated Mac and that half is done. The one open goal
is a picture on the external display over Type-C DP alt mode. DP alt mode is
granted on PHY2 (DMUB `QUERY_DP_ALT`, ROM-matched twice). The pixel clock
runs on pipe 1's DP DTO at the right rate (2FE fixed `PIPE_DTO_SRC_SEL`;
ratio 0.9997 at `srcsel=0`). The second raster scans. The DIG front end
connects, the transmitter answers every write, the DMCUB consumes and acks
every command we send it. **The sink still sees nothing** — DPCD `0x202`
reads `0x00` before and after, five boots running.

**The blocker is NOT named.** "The analog PHY is off" was withdrawn in 300.

---

## What 300 withdrew, and why it matters for what you do next

Five boots (459, 461, 464, 468, 469) read `RDPCSTX2_RDPCSTX_PHY_CNTL6` =
`0x00063333`, concluded PHY2's analog lane was dark, and wrote that into
`CLAUDE.md` and `docs/EXTERNAL-DISPLAY.md` as the named blocker.

`dmubbl dpaltq` has been printing, since 2EA:

> `RDPCSTX2_PHY_CNTL6 is the WRONG REGISTER for this part - Linux reads
> RDPCSPIPE_PHY_CNTL6 for UNIPHY_C on YELLOW_CARP_B0`

Nobody ran `dpaltq` and read its output until boot 469. **Do not design the
next experiment before reading what is already in hand** — this is the most
expensive instance of that error in the record.

What survives of the old claim: `RDPCSTX2` differs from `RDPCSTX0`
(`0x00194444`) in an in-boot byte-level comparison, which is a real fact about
that block. But it means "no MPLL, no reference clock" **only if those fields
are what that address decodes as for this PHY**, and that is now open.
**Lane power is unanswered in either direction, and no reading in boot 470
will answer it.**

---

## STEP 1 — install and boot (this is the only step pending on hardware)

The framebuffer is built and the hosttest passes, including the seven-guard
rule-48 break-it run on the new decoder.

```
sudo tools/fbinstall.sh
```

Then check the UUID landed **inside the collection**, not in
`/Library/Extensions` (rule 15 — 2xx cost nine boots to this):

```
kmutil inspect -B /Library/KernelCollections/AuxiliaryKernelExtensions.kc \
  --show-kext-uuids | grep -i rembrandt
```

### The arm, which is part of the experiment (rule 34)

**TWO tokens, both required:**

```
sudo tools/efi.sh bootarg add -rbgpufbdig -rbgpufbaltq
```

`-rbgpufbdig` arms the register read. `-rbgpufbaltq` arms `QUERY_DP_ALT`,
which is **the thing the read is compared against**. With only the first, the
agreement publishes 0 — which means "not evaluated", not "disagrees" — and the
boot produces raw words with nothing to test them against.

**The cable must be plugged in at boot (2FB).** Plugged in afterwards, every
AUX read returns refusal 3 and `AUX_EN` reads clear; the firmware's connect
path writes it, at boot, and we never do.

Verify the arm **after** the boot, from the running system (rule 43), and read
`rbgpu-boot` first — no increase means the boot was never armed and nothing in
NVRAM is this boot's (rule 8).

---

## STEP 2 — in the armed boot, in this order

```
build/out/dmubbl dpaltq
tools/dpcspipelook.sh
```

Order matters: the agreement is only computed in a publish that happens after
`dpaltq` has answered, and the publish is periodic (`RBFBPubMs`, default
500 ms), so give it a second.

### What each outcome means, written down before the boot

| `RBFBDpcsPipeAgree` | reading |
|---|---|
| **0** | `QUERY_DP_ALT` did not answer for PHY2 — **no comparison was made.** Not a disagreement. Check the arm. |
| **1** | An instance's `DPALT_DISABLE` matches the DMUB. **The first corroboration in a register** that PHY2's DP-alt state is granted, from the block Linux actually reads for UNIPHY_C. It **retires `0x00063333`** as a misread of the wrong register. |
| **2** | Neither matches. **The larger finding:** two attested instruments disagree, with the ROM match on the DMUB's side. Then the question is which instrument, and the next step is a *third* one — not another read of either of these two. |

`RBFBDpcsPipeSweep` is the control: only **DIFFERS** proves these are two
registers and not one address read twice, which matters more than usual
because upstream's own table aliases indexes 2, 3 and 4 onto them.

### What this read CANNOT do, and do not let a 1 suggest otherwise

`RDPCSPIPE_PHY_CNTL6` defines **three** fields —
`DPALT_DP4`, `DPALT_DISABLE`, `DPALT_DISABLE_ACK`
(`dpcs_4_2_0_offset.h:11945-11957`). **No MPLL, no reference clock, no
PSTATE.** "Is DP alt granted" and "is the MPLL running" are two questions, and
treating them as one is precisely what made `0x00063333` look like a single
verdict for five boots. Nothing in boot 470 says whether a lane is powered.

Which instance is UNIPHY_C is **not** established and must not be claimed:
three PHYs report as USB-C (`is_usb` mask `0x1c`) against two registers, so
the mapping cannot be one-to-one. The agreement is an OR over both.

---

## STEP 3 — where the chain actually goes next

Independently of boot 470's verdict, three things are **absent** from the
chain and at least one of them is probably the reason the sink sees nothing:

1. **A stream on DIG2.** There is no stream encoder programmed at all. This
   chip's DIO stream encoder is built by
   `dcn30_dio_stream_encoder_construct` over DCN10's register struct
   (`dcn31_resource.c:1323`, `:361`) — **read `dcn30_dio_stream_encoder.c`,
   not a dcn31 file.** 2FC corrected the earlier handoff here:
   `enc31_map_stream_to_link` is the **HPO DP** (128b/132b) encoder's and our
   8b/10b link never goes through it.
2. **A surface on pipe 1** — HUBP, address, swizzle. Nothing is being scanned
   out; the raster scans blanking.
3. **A second `IOFramebuffer`.** `getDisplayModeCount()` has returned 1 since
   2sl.

A link with no stream and no surface transmitting nothing the sink recognises
is consistent with every reading taken so far, and it does not require a dark
PHY to explain.

---

## Hard constraints that have each cost a boot

- **`dptrain go tps=3` / `tps=4` DEGRADE this sink** and the recovery is a
  **reboot** (2F8). The tool refuses them; `force` overrides. **Use `tps=2`.**
  All of 2F7 was the aftermath of this, so its two dead hypotheses were killed
  by invalid data.
- **PHY0 / DIG0 / front end DIGA are the panel's** and are refused by name in
  every half of every one of these experiments.
- **The chassis must be in iGPU-only / Eco mode** (rule 47). A `vendor-id`
  `0x10de` on the bus means hybrid; `./build/out/blset` reports the mode first.
- `make plugin`, `make dmubbl` and `make pubms` are **their own targets** —
  `make kext tools` builds none of them (2E9 lost a reading to this).
- The Type-C subsystem on this chassis can die in a way that survives a reboot
  and is cleared only by a **30-second power-key hold** (2DD).

---

## Also open, off the critical path

DPCD `0x204` = `0x80` (`LINK_STATUS_UPDATED` with `INTERLANE_ALIGN_DONE`
clear, n=1, sticky). `OTG_DIO_FIFO_ERROR` stays 3 on a pipe with the DTO
running and the raster scanning — untested with a stream. Why DPCD `0x102`
stores only from inside `dptrain`'s own call (the old arm is void: it used
`tps=3`). The caller that programs `DIO_LINK*_CNTL` is not identified. The
**wedge** (channel-52 GFX, 3.5 s, the "random lag"). `powermetrics` has never
been run against the 576 interior thread_calls/s. Ventura and Sonoma claim
nothing in `rb_osver.h`.
