# HANDOFF — read this first

**Last milestone: 310d. Boot 485 sent `SET_PHY_FSM` and the firmware does not
answer. Nothing is pending on the machine; nothing needs installing.**

## Where the one open goal stands

A picture on the external display over Type-C DP alt mode. The blocker is
named and has been since 307: **PHY2 (UNIPHY_C) is held in reset.**
`PHY_RESET` 1 against the live panel's 0, no `TX_CLOCK_ON`, no FIFO, firmware
loaded. Everything downstream follows from it and is fully measured — no clock
recovery, DPCD `0x202` `0x00`, `0x205` bit 0 clear, `DP_VID_M` refusing a
store, the sink seeing nothing while our clock, raster, MSA and transmitter
writes all land and read back.

**310d closed the last candidate we could name.** `SET_PHY_FSM` (DMUB VBIOS
sub-type **29**) is the one command into the PHY's state machine; its enum
names PHY2's measured RESET and the panel's TX_EN; its payload is documented
"passed in/out" with a `uint32_t status`. Boot 485 sent it four times — states
2 RESET_RELEASED, 3 SRAM_LOAD_DONE, 10 TX_EN, then 2 again — `refusal 0` each,
and every time:

- the **whole 64-byte reply slot** came back bit-for-bit as we wrote it
  (`verdict 2 UNCHANGED`, `decode 77 NO_REPLY`),
- PHY2's nine registers moved **nothing**,
- all **four control instances** held still, so a change would have been
  attributable.

**And the negative is bracketed inside that same boot**: the ring drained
(`Rptr == Wptr == 2432`, 1 ack — DMCUB consumed the slot), and `dmubbl dpaltq`
on the same ring returned a **ROM MATCH** (`is_usb` mask `0x1c`,
`alt_disable` 0 on PHY2 alone). A sibling command in the same family replies
correctly; 29 is consumed and answered with nothing. **DMCUB 4.0.15.47 does
not act on sub-type 29.**

## What is dead, so it is not re-opened

Everything on our side of the mailbox:

- **307** — the atom transmitter ENABLE does not release the reset, in-boot
  A/B, four-instance control. `ran` is only a mailbox ack.
- **308** — the SRAM-load handshake. `EXT_LD_DONE`/`SRAM_BYPASS` separate live
  from dark, not PHY2 from PHY3.
- **308b** — the ROM. Slot 39 `dig1transmittercontrol` is `0x0000`, so the
  firmware servicing our command is DMCUB's image, not the ROM.
- **309 / 309b** — the ownership registers, and the register survey **as a
  method**: nine registers of ~41 cannot tell PHY2 from byte-identical dark
  PHY3, while ROM-attested `QUERY_DP_ALT` can. Three surveys failed the same
  way. More registers from that file is not a next step.
- **310d** — the FSM command.
- Four more died from source with no boots (310): DCN31 NULLs **all five**
  `dpcs_*` bringup hooks, `dcn20`'s own sequence needs
  `debug.avoid_vbios_exec_table`, every payload field is verified against
  source, and `set_dio_phy_mux` is called only by the HPO path.
- The **CR window** (`RDPCS_TX_CR_ADDR`/`CR_DATA`) is **not a read** —
  choosing a target means writing the firmware's own latched cursor
  mid-sequence. Deprioritised, not surveyed.

## What is actually left

Two things, neither a register and neither cheap:

1. **DMCUB's image** (4.0.15.47, CW0/CW1). It services the command that
   replies and the one that does not, and it is the only party that has ever
   moved this PHY. This is static work on firmware we already have on disk —
   no boots.
2. **Evidence from a machine where this PHY has been brought up.** No sibling
   project has it: nullarc (780M, nearest chip) lists external displays under
   "Not yet" with no DPCS material; Cobalt64 (RDNA 4) reads ~35 RDPCSTX
   registers but writes no reset release; UnifiedRed is DCE 11 era.

Also never attempted, and all downstream of the reset so none of them can work
first: a stream on a **trained** DIG2 link, a **surface** on pipe 1, a second
`IOFramebuffer` (`getDisplayModeCount()` has returned 1 since 2sl).

## Standing conditions for any work here

- **Cable in at boot** (2FB), or every AUX read refuses. `RBFBAuxEn` mask 5
  and `RBFBHpdSink` 5 confirm it — they are **masks, not indexes**.
- **Eco / iGPU-only in the chassis firmware** (rule 47), or nothing we write
  drives anything.
- `dptrain tps=3`/`tps=4` **degrade the sink until a reboot** — `tps=2` only.
- The Type-C subsystem can die in a way that survives a reboot; cleared only
  by a 30-second power-key hold (2DD).
- `dmubbl dig` and `dmubbl dto` **write with no argument**. `dmubbl fsm` and
  `fsm <state>` do not — only `fsm go <state>` sends.

## Other open lines, untouched

- The **wedge**: work submitted that never retires — channel-52 GFX, 3.5 s,
  the "random lag".
- `powermetrics` has **never been run** against the default 576 interior
  thread_calls/s, the one real argument against the 2D5 default.
- Ventura and Sonoma claim **nothing** in `rb_osver.h`.
