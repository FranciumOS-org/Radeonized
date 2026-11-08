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

## THE NEXT STEP: `DMUB_CMD__VBIOS_TRANSMITTER_SET_PHY_FSM = 29`

**310 found it statically, no boot.** It supersedes both of 309b's candidates.

`dmub_cmd.h`, same VBIOS command family as the atom ENABLE (1) and
`QUERY_DP_ALT` (26). Doc comment: *"Control PHY FSM"*.

```
enum dmub_phy_fsm_state {
    0 POWER_UP_DEFAULT,  1 RESET,           2 RESET_RELEASED,
    3 SRAM_LOAD_DONE,    4 INITIALIZED,     5 CALIBRATED, ...
    9 PLL_EN,           10 TX_EN, ...
}
```

**PHY2 measures as state 1. The live panel measures as state 10. 308 spent a
milestone on state 3.** Three milestones surveyed this state machine's output
register by register; the machine has a command.

```c
struct dmub_rb_cmd_transmitter_set_phy_fsm_data {   /* "passed in/out" */
    uint8_t  phy_id;        /* 2 = UNIPHYC - comment VERBATIM query_dp_alt's */
    uint8_t  mode; uint8_t lane_num;
    uint32_t symclk_100Hz;
    struct phy_test_mode test_mode;
    enum dmub_phy_fsm_state state;
    uint32_t status;        /* <- the execution evidence command 1 cannot give */
    uint8_t  pad;
};
```

Its union member sits **immediately after `query_dp_alt`'s** (7821 vs 7817) —
the command we already send on this ring, with a ROM-matching answer on five
boots, using `WAIT_TYPE_WAIT_WITH_REPLY`. **The reply path is already proven on
this machine.**

**Nobody has ever sent command 29** — not upstream, not us. Upstream only sets
`use_phy_fsm` inside PSR copy_settings, which is consistent with DCN31 NULLing
every `dpcs_*` bringup: the FSM is the firmware's, and this is the one command
into it.

### Do it in this order

1. ~~Build it without sending.~~ **DONE in 310b.** `src/common/rb_phyfsm.h`,
   `tools/hosttest/phyfsm_test.c`, `dmubbl fsm [state] [phy]`. It returns
   before `findFB()`, so it cannot reach a send however it is invoked. Payload
   24, header `0x18001d80`, PHY0 refused by name, states on an allowlist,
   `status` prefilled with a sentinel. 22 break-it guards, 20 caught by a
   failing test, two recorded as results. `dmubbl` UUID ACDAA6D5.
   **The units question is SETTLED and the earlier note was wrong twice:** the
   atom field is `symclk_10khz` (54000) and command 29's is `symclk_100Hz`
   (**5400000**) — a different name and a factor of **100**, not ten. Pinned by
   a test, including one that asserts it is not 540000.
   Run `./build/out/dmubbl fsm` to see the exact slot; it needs no arm and no
   framebuffer.
2. ~~Write the send.~~ **DONE in 310c, NOT RUN.** `-rbgpufbfsm`,
   `src/fbkext/RBAMDFramebufferPhyFsm.cpp`, `dmubbl fsm go <state> [phy]`.
   Framebuffer UUID **147D6474**, `dmubbl` **FAD8BBBD**. **Neither is
   installed and nothing has been sent.**
3. **Now it needs a boot, and that is the user's call.** To run it:

   ```
   sudo tools/efi.sh bootarg add -rbgpufbfsm
   sudo tools/fbinstall.sh
   ```

   then reboot **with the Type-C cable already plugged in** (2FB: plugged in
   afterwards, every AUX read refuses and the port never got its `AUX_EN`),
   and in the armed boot, over SSH:

   ```
   ./build/out/dmubbl fsm
   ```

   to see the slot without sending, then

   ```
   ./build/out/dmubbl fsm go 2
   ```

   for RESET_RELEASED. Check first that the framebuffer UUID **inside the
   collection** is 147D6474 and that `-rbgpufbfsm` is in this boot's args
   (rule 15 — 2xx cost nine boots to this exact gap).

   **Batch the boot** (the standing instruction): `dmubbl fsm go 2` first,
   then `3` SRAM_LOAD_DONE and `10` TX_EN if 2 is answered or inert, then
   `./build/out/phylook.sh` and `dpcdread 0x202 0x205` to see whether
   anything downstream changed. Each `go` publishes its own before/after with
   the four-instance control, so several asks in one boot do not spoil each
   other — but note that state is cumulative on the hardware, so the ORDER
   is part of the reading.

   **What each outcome means:**
   - `decode 0` — DMCUB answered. Sub-type 29 is implemented and reached
     phyid 2. The status word is reported, not judged; its meaning is not
     established anywhere readable.
   - `decode 77 NO_REPLY` — DMCUB consumed the slot and wrote nothing. **That
     is a result, not a failure**: the first direct execution evidence this
     line has had, since command 1 has no reply field to ask.
   - `changed` non-zero with `ctlChanged` 0 — PHY2 moved and the controls held
     still. That is the thing 307, 309 and 309b all failed to produce.
   - `ctlChanged` non-zero — **the sample is contaminated**, something else is
     writing the block, and rule 17 applies.
   - `refuse 82` — the panel does not read as a brought-up PHY this sample, so
     the instrument is suspect and the reading would mean nothing. Re-run; if
     it persists, the survey is broken this boot, not the port.

4. **Then journal it either way.** Sending 29 is a **write of a new firmware command**,
   so it is their decision (the card's rule for SMU message groups, same
   spirit). Say what it changes: one new `DMUB_CMD__VBIOS` command on the ring
   we already drive, **phyid 2 only**, PHY0 refused by name, `status` read back
   and published, with the nine-register PHY survey as the in-boot before/after
   and PHY1/3/4 as the four-instance control.
3. **Pick the target state deliberately.** `RESET_RELEASED` (2) is the minimal
   ask and matches the measured defect exactly. Whether the firmware accepts
   RESET -> RESET_RELEASED directly or wants the ladder is **not established**,
   so the `status` field is the point of the first send, not the picture.

**Not established, and say so in the journal:** that the firmware acts on 29
for a DP-alt port, that the direct transition is legal, or that DMCUB
4.0.15.47 implements 29 at all — our image is one build, the header is
upstream's.

### The CR window is NOT a read — deprioritised

`RDPCS_TX_CR_ADDR` (0x2934 + n*0xd8) and `CR_DATA` (+1) are plain 16-bit value
fields: **no busy bit, no trigger bit, no status**, a second name for the same
address (`DPCSSYS_CR<n>_*`), and **no vendored `.c` user**. So `CR_DATA`
returns whatever register the **firmware's own latched address** selects, and
choosing one means **writing CR_ADDR** and clobbering that latch mid-sequence.
The live word decodes not as a register but as somebody else's cursor. Not a
read experiment at all.

Of the ~41 registers per instance, only `RDPCSTX_SCRATCH` (0x2ae7) and
`DEBUG_CONFIG` (0x2aed) are worth a line in some future survey. 309b's
conclusion stands: more registers from this file is the same experiment a
fourth time.

## Also open, unrelated to the display

- The **wedge**: work submitted that never retires, channel-52 GFX, 3.5 s,
  the "random lag". `stamp_ticks` 125666 retires the "nothing polls it" family.
- `powermetrics` has **never been run** against the default 576 interior
  thread_calls/s — the one real argument against the 2D5 default.
- Ventura and Sonoma claim **nothing** in `rb_osver.h`.
