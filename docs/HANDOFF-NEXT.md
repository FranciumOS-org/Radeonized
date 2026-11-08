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

**Install `16CB216D` and read it. One boot, one question.**

```
sudo tools/fbinstall.sh
```
then reboot **with the cable plugged in**, and:
```
sh tools/phylook.sh
```

**The question: is PHY2's register interface BLOCKED?**
`RDPCSTX_DPALT_CONTROL_REG` carries `RDPCS_ALLOW_DRIVER_ACCESS` and
`RDPCS_DRIVER_ACCESS_BLOCKED`. If PHY2 reads BLOCKED where the live panel
reads ALLOWED **and** dark USB-C PHY3 does not, it accounts for everything at
once with no new mechanism: `PHY_RESET` never clearing, the atom ENABLE moving
nothing, and **`DP_VID_M` refusing a store** — unexplained since boot 478 and
exactly what a blocked register interface looks like.

**Read both diffs, not one.** `RBFBPhy2AccessDiffPanel` and
`RBFBPhy2AccessDiffDark`. A difference against the panel alone is the generic
"never brought up" signature and means nothing — **that is precisely how 308's
candidate died.** `phylook.sh` says which case it is in prose.

Three other readings come free in the same boot: DMCU's `FORCE_TX_CLK_DIS`,
a **third** source on `DPALT_DISABLE` (`RDPCSTX_DMCU_DPALT_PHY_CNTL6`, beside
RDPCSPIPE's and 301's retired RDPCSTX read — three sources can name an odd one
out where 301's two could only be OR-ed), and `PHY_ENC_TYPE_SEL`.

**Do not write any of them.** There is no setter and the ownership register
must not get one: a driver that writes `ALLOW_DRIVER_ACCESS` to grant itself
access is not a driver that was granted it.

**If PHY2 reads ALLOWED and unblocked**, the ownership line is closed too, and
what remains unread is the other ~26 registers of the block — `PHY_CNTL1..17`,
`PHY_FUSE0..3`, `RDPCS_TX_CR_ADDR`/`CR_DATA` (the indirect PHY window),
`PLL_UPDATE_*`. Cobalt64's Navi 48 survey reads all of them; we read nine.

## What is already excluded, so do not re-open it

- **308's SRAM handshake.** `EXT_LD_DONE` / `SRAM_BYPASS` set on the panel and
  clear on **all four** dark instances. Live-versus-dark, not PHY2.
- **The ROM.** Slot 39 `dig1transmittercontrol` in the master command table at
  `0xa480` is `0x0000`, with every display-encoder entry 0. The firmware
  servicing our command is **DMCUB's image** (4.0.15.47, CW0/CW1).
- **Every sibling project.** nullarc (780M, Phoenix, DCN 3.1.4 — the nearest
  chip to ours) lists external displays under "Not yet" with no DPCS material;
  Cobalt64 (RDNA 4) reads the block but writes no reset release; UnifiedRed is
  DCE 11 era.
- **A driver-side PHY bringup.** DCN31 NULLs `dpcs_initialize_phy`,
  `dpcs_configure_phypll`, `dpcs_configure_dpcs`, `dpcs_enable_dpcs` and
  `set_phy_source`. The `avoid_vbios_exec_table` path is debug-only and needs
  exactly those NULLs.
- **Our atom payload**, confirmed byte by byte in the memory DMCUB reads:
  `02 01 00 02 f0 d2 00 00 03 00 00 f4`. And `pll_id` is not in the v1.7
  payload at all.
- **The mailbox.** `QUERY_DP_ALT` round-trips on all five PHYs with per-send
  echo and a ROM match, in the same boots where the ENABLE moves nothing.
- **The reply slot as execution evidence.** `transmitter_control_dmcub_v1_7`
  uses `DM_DMUB_WAIT_TYPE_WAIT`, so no `ret_status` is ever written for this
  command.
- The DIO PHY mux (`set_dio_phy_mux` is HPO-only), a DP-alt acquire (none in
  the driver), the encoder side entirely (306), `DIS_DEFER`, the held FIFO,
  the DPIA branch, the RDPCSPIPE instance.

## Traps this milestone paid for

- **`dmubbl dig` WRITES WITH NO ARGUMENT** — phyid defaults to PHY2, so a bare
  run typed to read the usage performs the atom ENABLE. Boot 475's `dmubbl
  dto` trap with a different verb; both are named on the card.
- **`tps=3` / `tps=4` degrade the sink until a reboot.** Use `tps=2`.
- **The cable must be plugged in at boot.**
- A non-5 PHY verdict on the live panel means the loaded kext predates the
  licence gate — read the raw words.
- `phylook.sh` prints hex with an ANSWERED/DRIVEN decode and says **ABSENT**,
  never `0`, for a key the loaded kext does not publish.
- Three pairs this block's own data cannot separate:
  `TX_CLOCK_ON`/`TX_CLK_EN`, `TX_FIFO_EN`/the lane enables, and
  `EXT_LD_DONE`/`SRAM_BYPASS`. Each moves together in every sample so far;
  the hosttest pins them with synthetic words.
- A break that fires nothing because it **broke nothing** is not missing
  coverage. The disjointness check is per register on purpose.
