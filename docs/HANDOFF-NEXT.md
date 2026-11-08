# HANDOFF — where this is, and what to do now

**Read `docs/RULES.md` first, then this. `docs/EXTERNAL-DISPLAY.md` has the full
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

1. **Get DMCUB's own execution evidence for the DIG1 transmitter control.**
   An ack says the slot was consumed; it has said nothing since boot 273. The
   question is whether the table runs and what it returns. `dmubbl`'s reply
   slot and verdict already exist for the caps command — the same treatment
   for `dig` is the reading, and it is **unbooted work**.
2. **Then: what else does the firmware want before it acts on phyid 2.** The
   port is in DP alt mode (`QUERY_DP_ALT`, ROM-attested on four boots, PHY2
   the only USB-C PHY with `alt_disable` 0), the cable was in at boot, AUX2
   works, EDID read. Something between that and the PHY release is missing,
   and it is the firmware's side of the line.

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
