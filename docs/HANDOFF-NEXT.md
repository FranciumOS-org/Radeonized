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

**The blocker is NOT named.** "The analog PHY is off" was withdrawn in 300 and
**retired as a misread in 301**, with positive evidence against it rather than
suspicion.

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

## STEP 1 — nothing is pending on hardware for the DP-alt question

**Boot 471 answered it.** `tools/dpcspipelook.sh` reported **AGREES** with
`RBFBDpcsPipeSweep` = **DIFFERS**, the control:

| instance | dword | word | verdict |
|---|---|---|---|
| RDPCSPIPE0 | 0x2d73 | `0x00000000` | NOT DISABLED |
| RDPCSPIPE1 | 0x2e4b | `0x00060000` | DISABLED, set and acked |

One instance agrees with `QUERY_DP_ALT` and the other actively disagrees, so
the agreement discriminated rather than passing on a field that reads 0
everywhere. In the same sample: RDPCSTX0 `0x00194444`, RDPCSTX1 `0x00023333`
(DISABLE set, ACK clear — settling), RDPCSTX2-4 `0x00063333`.

**`0x00063333` is retired as a misread** — it contradicts both the ROM-attested
`QUERY_DP_ALT` and the register Linux actually reads for UNIPHY_C, with an
in-boot control on each side. Do not reopen it without new evidence.

**Two things it does not establish, and neither may be claimed:** which
`RDPCSPIPE` instance is UNIPHY_C (three USB-C PHYs against two registers,
rule 18 — the agreement is an OR), and **whether a lane is powered**. That
register defines `DPALT_DP4`, `DPALT_DISABLE` and `DPALT_DISABLE_ACK` and
nothing else: no MPLL, no reference clock, no PSTATE.

---

## STEP 2 — the candidate list, and what is left on it

Dead, each by measurement with a control:

| candidate | killed by |
|---|---|
| the second raster | 2FD — OTG1 honours its timing |
| the pixel clock | 2FE — `PIPE_DTO_SRC_SEL`, ratio 0.9997 at `srcsel=0` |
| the DIO encoder-type mux | 2FC — all six zero, closed as a negative |
| `hpdsel` | 300 — in-boot A/B, both sides acked, PHY2 unmoved |
| power gating | 300 — `RBFBPgCandidate` -1, `dmubbl dom` refusal 24 |
| "the analog PHY is off" | 301 — the reading was the wrong register |

The atom transmitter ENABLE has executed since 2E2, the DMCUB consumes and
acks every command, and the DIG front end connects. **Nothing on the old list
survives.**

---

## STEP 3 — where the chain goes next, and this is now the whole of it

Three things are **absent** from the chain and **none has ever been
attempted**. At least one of them is why the sink sees nothing:

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

A link carrying no stream and scanning out no surface is consistent with every
reading taken across all thirteen boots of this line, and it requires no dark
PHY to explain. **The sink's silence stops being evidence about the PHY the
moment there is nothing for the PHY to carry.** Start with the stream: it is
the only one of the three whose registers this kext has never touched, and
`dcn30_dio_stream_encoder.c` has not been read yet.

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
