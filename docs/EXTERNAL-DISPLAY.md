# The external display path (Type-C DP alt mode)

**The evidence record for the one open goal: a picture on the USB-C monitor.**
Moved out of `CLAUDE.md` 2026-11-07 (2F7) because the card is a reference card
and this had grown to 130 lines of narrative in it. Nothing is deleted; the
journal under each tag is still authoritative, and `docs/HANDOFF-NEXT.md` holds
what to do next.

---

### The external display path (Type-C DP alt mode), as measured

Port = **HPD instance 2** = **AUX engine 2** = **UNIPHY_C / phyid 2** = **DIG2**
(four independent instruments agree: HPD sink bit following the cable,
`AUX_CONTROL.AUX_HPD_SEL`, `DIG_BE_CNTL.DIG_HPD_SELECT`, and the VBIOS display
-object table in our own `firmware/680m-vbios.rom`). Sink: **MSI MP2412C**, DP
1.2, HBR2, **2 lanes**, EDID read over I2C-over-AUX, native **100 Hz** (fits the
~8.64 Gbps link). DP alt mode **is granted** (DMUB `QUERY_DP_ALT`, matching the
ROM bit for bit on all five phys; `RDPCSTX2_PHY_CNTL6` disagrees and is the
legacy fallback Linux only reads when DMCUB is absent). **The firmware executes
`DMUB_CMD__VBIOS` commands** (`DIG_BE_EN_CNTL.DIG_ENABLE` 0→1 on the targeted
instance alone, two boots) **and answers them** (the reply path works at
REGION4; `ret_status` stays SET, which Linux's own criterion would discard).
The firmware configured that encoder (`DP_CONFIG` = 2 lanes on DIG2, 4 on the
panel's DIG0, 0 on the three never brought up). Remaining: the **pixel clock**, which on a DP
link is **three register writes of ours** and not the VBIOS command (2EE read
the caller: `dcn31_program_pix_clk` programs `DP_DTO*_PHASE`/`MODULO`/
`PIXEL_RATE_CNTL` itself and reaches `set_pixel_clock` only for HDMI/DVI).
Those registers are **BASE_IDX 1 = 0xc0**, whose offsets collide with the
DOMAIN power-gating block at BASE_IDX 2 — and boots 452 and 453 both
**CONFIRMED** the base on the chip (2F0/2F1, so rule 44 is satisfied for it), with the alias control reading `DOMAIN0/1_PG_STATUS` as
`0x10000000` there. From that boot: **`dprefclk` = 600 MHz exactly**
(`MODULO[0]`, never located before); the panel's DTO does drive the panel
(enable and status both set); pipes 1-3 are dark and byte-identical. Two
cautions for the write: `PIPE0_DTO_SRC_SEL` is **0** where Linux writes 1 and
the panel works anyway, and **PHASE is the requested clock, not a readback**
(343.000000 MHz against a raster wanting 342.288, DRR ruled out) so it cannot
be the verdict register. **THE PIXEL CLOCK IS RUNNING** (2F1 built the writes, boot 455 ran them).
Pipe 1's `OTG1_PIXEL_RATE_CNTL` went `0x8000c000` → **`0x8000d090`**: our RMW
(`DP_DTO_ENABLE` + `PIPE_DTO_SRC_SEL`, `DTBCLK_DTO_DIV` 8 and
`DIO_FIFO_ERROR` 3 carried through) **plus exactly one bit we do not write,
`DPDTO_ENABLE_STATUS`**, whose two states 2F0 attested here first in two boots.
PHASE 148 500 000 (the sink's own preferred timing), MODULO **copied off
pipe 0** — PHASE/MODULO is a ratio, so the true reference never has to be
known. The in-boot control is better than the write path's readback: in the
same periodic sample pipes 2-3 still hold the dark word **byte-identical**
with PHASE/MODULO 0, and the panel's pipe 0 is untouched, so the change is
confined to the pipe the dial named. The panel kept scanning, which discharges
the DOMAIN-collision safety case by observation. **It is a clock, NOT a
picture** — no OTG timing on that pipe, no trained link, no stream mapped, no
surface. `SRC_SEL` is Linux's 1 where the firmware left pipe 0's at 0; what it
selects is still not established and the `srcsel=0` dial now needs a reboot
first, because nothing turns a live DTO off. A second `dmubbl dto` is refused
**45** by the already-live guard, which is also an independent reading of the
status bit. 2F2 split the published fields in two (`dtoCall*` = this call,
`dtoProg*` = the last call that WROTE) because 2F1's single set let that
refusal republish the successful write as `verdict -1 NOT RUN`.
**THE FIRMWARE DOES NOT TRAIN THIS LINK** (2F3, measured with a control): with
the display plugged in before power-on nothing appears - this chassis' OpenCore
picker is the text-mode one, and the user reports no pre-boot external output
through the 4050 under Windows either - and the sink's LED never leaves standby
through macOS boot. So training is **ours**, not an assumption.
**THE SINK TAKES WRITES** (2F3, boot 457). `dpcdwrite 0x100 0x14` on AUX2:
ACK, `before 0x06` (`DP_LINK_BW_1_62`, the untrained default) -> `after 0x14`
(HBR2, this sink's own maximum), `applied 1 changed 1`. The before value is not
the one written, so no rule-20 defect; the ACK is separately the sink's own
reply. **The first write this project has landed on a sink** - 2BD's EDID
offset byte goes to the monitor's DDC EEPROM, not its DP receiver. The fix was
one equality test that excluded `DP_WRITE` 0x80 and would have encoded it with
the READ length and no payload, silently. `dpcdwrite` refuses the panel by name
**three times** (tool by HPD, kext by HPD, kext by AUX engine) and
**allowlists** the address to the link-config block (0x100-0x108, 0x600,
`drm_dp.h`'s by line).
**2F4 BUILT THE TRANSMITTER HALF; BOOT 458 RAN IT AND IT MOVED NOTHING** (2F5).
All four of Linux's TPS1 writes are **no-ops on a dark transmitter** and
`applied 1` was honest every time, because every target state was already
present: TPS1 encodes as hardware pattern **0** and a dark `TP_SEL` reads 0,
and the three RMWs each clear a bit a dark instance does not have set. So
`applied` was exactly **rule 20's** defect and 2F4's "a training pattern on the
wire" is **withdrawn**. Two more defects in the same run: the sink's lane-status
after-read FAILED and 2F4's verdict fell through to `PATTERN_SET`, printing the
run's own instrument failure as its expected outcome; and the DPCD 0x102 write's
code was captured and never published. 2F5 added `moved` and `observable`
(neither a success criterion - they stop a no-op being read as evidence about
the sink), verdicts `NO_OP` 4 and `SINK_BLIND` 5, with **`NO_OP` outranking
`CR_DONE`** on purpose (rule 17). **THE ONE REAL READING** had to be dug out of
`RBFBAuxW`: the sink **ACKed `TRAINING_PATTERN_SET` and did not store it**
(`before 0x00 -> after 0x00`, `changed 0`, verdict 2 `ACK_IGNORED`) - against
2F3's own 0x100 `0x06 -> 0x14` which DID store, same engine, same sink. 2F5's
ordering hypothesis (a receiver in D3, `0x600` never set) is **WITHDRAWN** by
boot 459: nothing about ordering differed between the two boots.
**THE DP TRANSMITTER BLOCK ANSWERS OUR WRITES** (2F6, boot 459).
`dptrain go tps=2` moved DP2's `TP_SEL` **0 -> 1** with
`applied 1 clean 1 moved 1 observable 1` - TPS2 encodes as hardware 1 against a
dark 0, so unlike TPS1 it could be seen. The in-boot control is the periodic
five-instance sample: DP1/DP3/DP4 still read `TP_SEL` 0 byte-identical and the
panel's DP0 is untouched at 2, so the change is confined to the instance
addressed. The three RMWs stay no-ops for 2F5's reason. **THREE OUTCOMES AT
DPCD 0x102 IN ONE BOOT:** that run's own write stored (`0x00 -> 0x02`,
ACK_STORED); a bare `dpcdwrite 0x102 0x01` **CLEARED** the field
(`0x02 -> 0x00`, `changed 1` - the byte MOVED, and not to what we wrote, which
2F6 gave its own verdict `ACK_OTHER` 4 because the old code printed "the byte
did not change" beside a published `changed 1`); a bare `dpcdwrite 0x102 0x02`
did not move it; and `0x102 0x00` came back **DEFER**. The pattern across all
four: **the only write that stored is the only one where the TX side had just
moved** - a HYPOTHESIS at n=1 for the storing case, falsifiable in-boot by
`dptrain go tps=3` (TX moves, predicts STORED) then the same again (`moved 0`
-> `NO_OP`, predicts not stored). **BOTH NATIVE DPCD PATHS WERE SINGLE-SHOT ON
DEFER** - boot 458 on the read, boot 459 on the write, while the I2C path in
the same file has retried since 2BD - so 2F6 bounded both at
`RB_AUX_DEFER_RETRIES` 8 x `RB_AUX_DEFER_DELAY_US` 1000, **deliberately tighter
in time than Linux's 50 ms** (`dce_aux.c:822-827` needs 7 retries AND 50 ms;
ours is the count alone, because every caller is a dial and `IODelay` spins).
Boot 459's own lane-status reads both succeeded, so the read defer was
transient and this fix explains nothing - it is robustness. Also recorded:
the panel's own live `TP_SEL` reads **0x2**, so this register does hold non-zero
values on this chip.
2F4's build - `rb_dptx.h` plus
`build/out/dptrain`, the four writes of
`dcn10_link_encoder_set_dp_phy_pattern_training_pattern` (rule 46 discharged in
`dcn31_resource.c`: `.dp_set_phy_pattern` IS `dcn10_...`). Registers 0x2108 /
0x210c / 0x2117 / 0x2118 / 0x211d, stride 0x100, **BASE_IDX 2** (NOT the DTO's
BASE_IDX 1), and the aperture is discharged by 2E7 having already read
`rb_enc.h`'s 0x251e on all five instances. **The identification is the panel's
own DP0 reading as a trained link** - the firmware trained it and it is
scanning out now - and the write is refused (code 64) without it.
**THE TWO PATTERN ENCODINGS DIFFER:** the hardware register takes 0..3 for
TPS1..TPS4, DPCD 0x102 takes 1/2/3/**7**, and the two zeros mean opposite
things (DPCD 0 = training off, hardware 0 = TPS1). Everything speaks TPSn and
`rb_dptx.h` converts. Verdict = **the sink's own lane status** at DPCD 0x202,
read before AND after. `PATTERN_SET` is the **expected** reading until the
transmitter is enabled at the link rate - but boot 458 got it for the wrong
reasons twice over, so read `moved` and `observable` beside it. Needs
**`-rbgpufbtrain`**, which arms the AUX half itself (rule 34).
Then link training over AUX2 (driver-side — zero `dmub`
references in `link_dp_training*.c`), `enc31_map_stream_to_link`, a second OTG
and HUBP, and a second `IOFramebuffer` (`getDisplayModeCount()` has returned 1
since 2sl). **Order matters:** FE connect → configure encoder → transmitter
ENABLE; the atom transmitter command carries no front end **by design**.
