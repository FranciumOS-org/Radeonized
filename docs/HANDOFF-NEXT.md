# HANDOFF — where this is, and what to do now

**Read `CLAUDE.md` first, then this. `docs/EXTERNAL-DISPLAY.md` has the full
record of the one open goal.**

## State

Boot 479 is the last hardware boot. Framebuffer UUID **99122618**, installed
and verified inside the live auxiliary collection. Nothing is pending an
install: **307 spent no new boots and needs none to be understood.**

**THE BLOCKER IS NAMED. PHY2 — the Type-C port's RDPCSTX PHY — IS HELD IN
RESET**, with the live panel's PHY0 as the positive control in the same
sample:

| | PHY0 (panel, live) | PHY2 (the subject) |
|---|---|---|
| `PHY_RESET` | 0 — released | **1 — HELD** |
| `TX_CLOCK_ON` | set | clear |
| `SRAMCLK_CLOCK_ON` | set | clear |
| `TX_FIFO_EN` | set | clear |
| `SRAM_INIT_DONE` | set | **set** — firmware loaded |

PHY2 is byte-identical to PHY3, which nobody has touched. Everything
downstream follows: no symbol clock, so DPCD `0x202` reads `0x00`, `0x205`
bit 0 is clear, and `DP_VID_M` will not take a store.

**And the atom transmitter ENABLE does not release it.** In-boot A/B, four
other instances as the control: sent twice at phyid 2 / action 1 / digmode 0 /
lanenum 2 / symclk 54000, `refusal 0`, `ran 2` — PHY2's three words unchanged
to the bit.

**But `ran` is a mailbox ack, not execution.** `transmitter_control_v1_7`
calls `transmitter_control_dmcub` (a `void`) and then
`return BP_RESULT_OK` unconditionally. **Linux cannot tell whether the atom
table ran either.** So PHY2 unmoved does *not* mean the table is the wrong
mechanism.

## What is already excluded, so do not re-open it

- **A driver-side PHY bringup.** DCN31 NULLs `dpcs_initialize_phy`,
  `dpcs_configure_phypll`, `dpcs_configure_dpcs`, `dpcs_enable_dpcs` and
  `set_phy_source`. There is nothing of ours missing.
- **The `avoid_vbios_exec_table` path.** Debug-only, default false, and it
  needs exactly those NULLs. **The VBIOS table is the only mechanism.**
- **Our payload.** Every field verified against the source: symclk 54000 =
  20 x 27000 / 10; `digfe_sel` 0 unconditionally by the helper; `connobj_id`
  0 on a `cntl = {0}`; and **`pll_id` is not in the v1.7 payload at all**.
- **The DIO PHY mux** (`set_dio_phy_mux` is HPO-only), **a DP-alt acquire**
  (none exists in the driver), the encoder side entirely (306), and
  `DIS_DEFER`, the held FIFO, the DPIA branch, the RDPCSPIPE instance.

## Do this next

**Everything between us and the firmware is now accounted for**, so do not
spend a boot re-checking it:

- **The payload is confirmed byte by byte in the memory DMCUB reads** —
  `02 01 00 02 f0 d2 00 00 03 00 00 f4` = phyid 2, ENABLE, DP, 2 lanes,
  symclk 54000, hpdsel 3, digfe 0, connobj 0, HPO 244.
- **DMCUB consumed it** — `WPTR/RPTR 0x9c0/0x9c0`, acks 2/0.
- **The reply slot cannot carry execution evidence, by design.**
  `transmitter_control_dmcub_v1_7` uses `DM_DMUB_WAIT_TYPE_WAIT`;
  `query_dp_alt_from_dmub` uses `WAIT_WITH_REPLY`. The DIG1 transmitter
  control never asks for a reply, so there is no `ret_status` to fetch. **An
  earlier draft of this file made that the next step. It does not exist.**
- **The mailbox is eliminated, same boot, with a ROM cross-check.** The
  `QUERY_DP_ALT` sweep round-trips on all five PHYs, the echo field matches
  each phy_id, the `is_usb` mask `0x1c` matches the VBIOS ROM from 2E1, and
  **PHY2 still reports `alt_disable` 0 / `is_dp4` 0** — DP alt granted, two
  lanes, unchanged by the ENABLE.

**So the gap is inside the firmware's handling of this command.** Either it
declines on a precondition we have not identified, or what it runs for a USB-C
PHY does something other than release reset. Start there, and note that
**sending more of what we already send cannot answer it.**

Two directions, neither of which needs a boot to begin:

1. **The SRAM-load handshake — the one live candidate (308).** Decoded per
   field, the live panel has `SRAM_EXT_LD_DONE` **1** and `SRAM_BYPASS` **1**
   with its PHY out of reset; PHY2 has both **0** with its PHY held.
   `SRAM_INIT_DONE` is set on **both**, so it carries no information.
   `rb_dpcsphy_sram_diff()` publishes the comparison against the panel, and
   `tools/phylook.sh` names which fields differ. **It needs a boot to read on
   the installed kext (UUID `DDFF93EB`) — that is the next reading.** Hold the
   two caveats: the bits are **one observation, not two** (they move together
   in boot 479's words), and **nothing in this tree says who writes them**,
   which is why they sit in their own UNATTRIBUTED set with no setter.
   **Do not write them** until a source names the owner.

   **The ROM is eliminated as a place to look (308b).** Slot 39
   `dig1transmittercontrol` in the master command table at `0xa480` is
   **`0x0000`** — absent, along with every other display-encoder entry. The
   firmware servicing this command is **DMCUB's own image** (4.0.15.47, in
   CW0/CW1), not the ROM.

   **No sibling project has the answer (308).** nullarc — Radeon 780M,
   Phoenix, DCN 3.1.4, the nearest chip to ours — lists external displays
   under "Not yet" and has no DPCS material. Cobalt64 is RDNA 4 and reads the
   block without writing a reset release. UnifiedRed is DCE 11 era.
2. **Find the precondition from the other side.** The port reports DP alt
   granted and two lanes, AUX2 works, the cable was in at boot, EDID read, the
   DTO runs, the raster scans. What a USB-C PHY release needs beyond that —
   a USB-subsystem handoff, a PHY power state, a SMU/PMFW step — is not named
   anywhere in the driver, which is itself the finding (there is no acquire in
   `dcn31_dio_link_encoder.c`).

**Do not write `PHY_RESET`.** `rb_dpcsphy.h` is read-only by design: on this
chip the bit's owner is the firmware, and on the panel the firmware is what
released it.

## Traps this milestone paid for

- **`dmubbl dig` WRITES WITH NO ARGUMENT** — phyid defaults to PHY2. A bare
  run typed to read the usage performs the atom ENABLE. This is boot 475's
  `dmubbl dto` trap with a different verb; both are now named on the card.
- **`tps=3` / `tps=4` degrade the sink until a reboot.** Use `tps=2`.
- **The cable must be plugged in at boot.**
- `tools/phylook.sh` prints hex with an ANSWERED/DRIVEN decode, and `ABSENT`
  rather than `0` for a key the loaded kext does not publish.
- A non-5 verdict on the live panel means **the loaded kext predates the
  licence gate** — read the raw words, not the verdict.
- Boot 479's words **cannot separate** `TX_CLOCK_ON` from `TX_CLK_EN`, nor
  `TX_FIFO_EN` from the per-lane enables: each pair moves together there. The
  hosttest pins the bits synthetically; hardware separation needs a boot where
  one is set without the other.
