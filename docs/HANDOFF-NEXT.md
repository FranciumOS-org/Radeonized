# HANDOFF-NEXT — where this is, and what to do now

**Last boot: 482.** Framebuffer **16CB216D** (installed and verified inside
the live collection). Nothing is pending an install.

Everything else about the project is in `docs/RULES.md` (the reference card) and
`docs/JOURNAL.md` (the evidence record, grep by tag). `docs/EXTERNAL-DISPLAY.md`
holds the one open goal in full. This file is only "what next".

---

## The state in four sentences

The panel works and the machine is an accelerated Mac. The external display
over Type-C DP alt mode has a granted port, EDID, a running pixel clock, a
scanning second raster, a correct MSA derivation, and a stream write that
applies 19/19 — and **no picture**, because **PHY2 is held in reset**
(`PHY_RESET` 1 against the live panel's 0, no `TX_CLOCK_ON`, no `TX_FIFO_EN`).
Every downstream symptom follows from that: no clock recovery, DPCD `0x202`
`0x00`, `0x205` bit 0 clear, `DP_VID_M` refusing a store, stream `STATUS` 0.

**The VBIOS atom transmitter ENABLE is the only mechanism that exists** to
release it — the DCN31 driver NULLs every `dpcs_*` bringup function and has no
DP-alt acquire — and it moves nothing, with our payload verified field by field
and `ran` known to be only a mailbox ack.

## What 307-309b eliminated, so nobody pays for it twice

- **The atom ENABLE's effect**: in-boot A/B, four-instance control, **nine**
  registers byte-identical before and after (482). Not "we sent it wrong" —
  every payload field is verified against the source and `pll_id` is not in
  the v1.7 payload at all.
- **The mailbox**: payload confirmed in the memory DMCUB reads, RPTR advanced,
  and `QUERY_DP_ALT` round-trips through the same ring in the same boot.
- **The ROM**: slot 39 `dig1transmittercontrol` = `0x0000`, every
  display-encoder entry 0. The firmware servicing the command is **DMCUB's
  image** (4.0.15.47), not the ROM.
- **308's SRAM handshake**: `EXT_LD_DONE`/`SRAM_BYPASS` separate live from
  dark, not PHY2 from anything (481).
- **309's ownership registers**: `DPALT_CONTROL_REG` reads 0 on the **live
  working panel**, so `ALLOW_DRIVER_ACCESS` cannot mean what it says here;
  `DIS_BLOCK` 0 everywhere (482).
- **`0x00063333` as grant state**: it reads identically on PHY2, PHY3 and
  PHY4 while `QUERY_DP_ALT` separates them (482).
- **Four candidates on source grounds, zero boots**: no driver-side PHY
  bringup; `avoid_vbios_exec_table` is debug-only; `set_dio_phy_mux` is
  HPO-only; the DPIA branch is USB4 tunnelling.
- **Three sibling projects**: nullarc (the nearest chip) has no DPCS material,
  Cobalt64 reads the block but releases no reset, UnifiedRed is DCE 11.

## THE REASON TO STOP SURVEYING

**Three surveys of RDPCSTX — five registers, then nine — have each failed in
exactly the same way: none can distinguish PHY2 from PHY3.** PHY2 is the port
with DP alt granted; PHY3 is a USB-C port with it disabled. They read
byte-identical everywhere we look. The only instrument that has ever told them
apart is DMCUB's `QUERY_DP_ALT`, ROM-attested on five boots.

So the state that makes PHY2 drivable **is not in the part of this register
file we can read**, and reading the remaining ~26 registers is the same
experiment a third time. **Do not propose it as the next step.** The candidate
rate across 307, 308 and 309 was three proposed and three killed, each inside
one boot of being proposed — the instrument design is working and the *place*
is wrong.

## The two places left, neither free

### 1. The PHY's internal registers, via `RDPCS_TX_CR_ADDR` / `CR_DATA`

The indirect window into the PHY's own (non-RDPCSTX) register space, which is
where a reset-release handshake would live if it is software-reachable at all.

**This is not a free read.** Cobalt64's Navi 48 survey reads `CR_ADDR` and
deliberately does **not** read `CR_DATA`, on the suspicion that a read there
is itself the bus trigger. Their word is SUSPECTED, not established, and we
have no second source.

Before this costs a boot it needs, statically: the `_sh_mask` field layout for
both registers; whether `CR_DATA` is read-triggering in the DCN31 headers or
any vendored `.c`; and a read on the **live panel first** as the positive
control, since a trigger that disturbs a working link is the failure mode that
costs a reboot. If the trigger question cannot be settled from source, it is a
decision for the user, not an assumption — the Type-C subsystem on this
chassis has already died once in a way only a 30-second power-key hold cleared
(2DD).

### 2. DMCUB's image

What services both the command that does nothing and the query that works.
4.0.15.47, CW0/CW1, already located and already parsed for the ring. The
question is what the firmware wants before it acts on phyid 2 — and since
`transmitter_control_dmcub` never asks for a reply (`WAIT_TYPE_WAIT`, not
`WAIT_WITH_REPLY`, unlike the query), **DMCUB's own execution evidence has
never been obtained**. Static work, no boot needed to start.

**Start with the static half of either.** Both are reads of things already on
disk, and the boot budget has bought three dead candidates in three boots —
the next boot should be spent on a question the source could not answer.

## Also open, unrelated to the display

- The **wedge**: work submitted that never retires, channel-52 GFX, 3.5 s,
  the "random lag". `stamp_ticks` 125666 retires the "nothing polls it" family.
- `powermetrics` has **never been run** against the default 576 interior
  thread_calls/s — the one real argument against the 2D5 default.
- Ventura and Sonoma claim **nothing** in `rb_osver.h`.
