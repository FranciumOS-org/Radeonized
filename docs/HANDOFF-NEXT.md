# HANDOFF — one install, one boot, two answers

## Install

Framebuffer UUID **99122618-EC61-3E7B-B428-6CD5D0044DDC**.

```bash
sudo tools/fbinstall.sh
```

Reboot **with the Type-C cable plugged in before boot** (2FB — plugged in
afterwards, every AUX read refuses on a nonexistent engine).

```bash
kmutil inspect -B /Library/KernelCollections/AuxiliaryKernelExtensions.kc --show-kext-uuids | grep -i rembrandt
```

```bash
sysctl -n kern.bootargs | tr ' \t' '\n\n' | grep -E '^-rbgpu'
```

## Question 1 — is the PHY alive? Read-only, costs nothing

```bash
bash tools/phylook.sh
```

**PHY0 is the positive control**: UNIPHY_A carries the panel's live DP link, so
every hardware-answered bit has a known-good value in the same sample.
PHY1/3/4 were never brought up. **PHY2 is the subject.**

Read the SWEEP line first: UNIFORM means the 0xd8 stride did nothing and
nothing below is a finding. Then PHY2's verdict:

- **ALIVE** — reference clock detected and firmware loaded. The PHY is up and
  the blocker is narrower than the PHY itself.
- **CLOCKLESS** — firmware loaded, the detector says no clock. That would be
  the first positive identification of the blocker this line has had.
- **NO_SRAM** — a clock but no firmware image.
- **DEAD** — neither.

Check `clkdet result is BELIEVABLE` on the same instance. With
`REF_CLKDET_EN` clear a 0 result means nothing, and the tool says so in words
rather than letting the verdict stand on it.

## Question 2 — does `DP_VID_M` really refuse a store?

Boot 478 inferred it from a zero bit in a changed mask, which cannot tell a
dropped store from a store of the value already present. The kext now
publishes `was -> want -> got` per write and the tool prints it. Same boot,
this order; the raster and clock are not persistent:

```bash
env -u DYLD_INSERT_LIBRARIES ./build/out/dmubbl dto 148500
```

```bash
env -u DYLD_INSERT_LIBRARIES ./build/out/otgprog go 1080p60
```

```bash
env -u DYLD_INSERT_LIBRARIES ./build/out/strencprog go
```

```bash
env -u DYLD_INSERT_LIBRARIES ./build/out/dptrain go tps=2
```

```bash
env -u DYLD_INSERT_LIBRARIES ./build/out/strencprog unblank
```

Look for `*** DID NOT TAKE ***` in the per-write table. If `DP_VID_M` shows
`0x00000000 -> 0x00002333 -> 0x00000000`, the store is dropped and that is a
register-level fact about DIG2. If it shows `-> 0x00002333`, boot 478's
changed-mask reading was wrong and this says so.

`strencprog go` leaves the stream disabled on purpose so `dptrain` can run —
boot 477's refusal 66. **Never `tps=3` or `tps=4`: they degrade the sink until
a reboot** (2F8), and the tool refuses them.

## What is already excluded, so nothing re-tests it

DP-alt disable (301 and 306's elimination), **lane count and scrambler**
(DIG2 already correct, read in boot 478), the back-end enable (477), the held
steer FIFO (477, and in-boot A/B in 478), `DP_VID_STREAM_DIS_DEFER` (source
grounds), the raster and pixel clock (2FB-2FE), `PIPE_DTO_SRC_SEL` (468),
hpdsel and power gating (469), the stream/training order (305b, boot 478), and
the **DPIA branch** (USB4-only, source grounds).

The sink has stated the blocker directly: DPCD `0x205` = `0x00`,
`RECEIVE_PORT_0_STATUS` clear — no sync on the main link — with `0x204` bit 7
`LINK_STATUS_UPDATED` set, so it is awake and reacting. DPCD `0x202` has read
`0x00` in six boots (459, 461, 464, 468, 469, 478).

## If the PHY reads ALIVE

The remaining untried surface is **lane power / MPLL**, which has never been
located. 301 established that `RDPCSPIPE_PHY_CNTL6` carries only DPALT_DP4 /
DISABLE / DISABLE_ACK — no MPLL, no reference clock, no PSTATE — and that
conflating those questions is what made `0x00063333` look like a verdict for
five boots. `dpcs_4_2_0_offset.h` has `RDPCSTX_PHY_CNTL1..17`,
`RDPCS_TX_CR_ADDR`/`DATA` and `RDPCS_TX_PLL_UPDATE_*` unread; the PHY is
programmed by firmware through the atom table, so expect to *read* these, not
write them.

## Rules this step stands on

- **Rule 15** — the UUID that matters is inside the collection.
- **Rule 26** — a search is an instrument: 306 lost a conclusion to a grep
  pattern that listed six tokens when seventeen were present. Read the
  unfiltered value.
- **Rule 20** — `changed` beside `applied`, and now `got` beside `want`.
- **Rule 47** — Eco / iGPU-only mode, cable in at boot.
