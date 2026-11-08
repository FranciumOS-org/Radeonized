# HANDOFF — where this is now, and what to do next

## State after boot 478

**The encoder side is finished and the split is proved.** `strencprog go` ->
`dptrain go tps=2` -> `strencprog unblank` ran end to end with no refusal 66:
the pixel clock starts, OTG1 scans, the MSA words are programmed and verified
against the live panel, `DIG_FE_CNTL.DIG_SOURCE_SELECT` points at OTG1, DIG2's
three-boot held `DP_STEER_FIFO` reset is cleared, and `DP_VID_STREAM_ENABLE`
is set.

**`DP_VID_STREAM_STATUS` stays 0, and the reason is upstream of the encoder.**
DPCD `0x202` has read `0x00` in six boots now (459, 461, 464, 468, 469, 478):
no clock recovery. Boot 478 got the sink to say it directly —
**`0x205` = `0x00`, `RECEIVE_PORT_0_STATUS` clear: it detects no sync on the
main link** — while **`0x204` bit 7 `LINK_STATUS_UPDATED` is set**, so the
sink is awake and reacting.

**The blocker is still not named.** 301 retired "the analog PHY is off". The
split was a real defect and fixing it changed nothing about the link, so it is
not a candidate either.

## Open and unexplained, from boot 478

**`DP_VID_M` on DIG2 will not take a store.** It reads `0x00000000` after
9011 (`0x2333`) is written, while `DP_VID_N` in the same loop, same instance,
same 24-bit mask shape takes its `0x8000` and reads back. The readback is in
the same instruction stream as the store, so it is not a later overwrite.
Eliminated in-boot: the **held FIFO reset** (A/B in both FIFO states, both
refuse). **Not** isolated: DIG1 and DIG3 as a four-instance control — they
retain no stores at all (`0x8000` comes back `0x2000`, the ENABLE bit does not
survive to the next read), so they are in a different power state and do not
answer the question.

The instrument for this now exists and did not in 478: the kext publishes
**`RBFBStrmW<n>Was` / `Want` / `Got`** per write and `strencprog` prints the
table and names a store that **DID NOT TAKE**. Read that before theorising.

## Do this next

**1. Install and boot.** Framebuffer UUID
**27BF9539-A203-3D7C-9C3D-8927CAD60EC0**.

```bash
sudo tools/fbinstall.sh
```

Reboot **with the Type-C cable plugged in before boot** (2FB).

```bash
sysctl -n kern.bootargs | tr ' \t' '\n\n' | grep -qx -- -rbgpufbstrm && echo ARMED || echo NOT ARMED
```

**2. Re-run the chain and read the new per-write table.** Same boot, this
order; the raster and the clock are not persistent.

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

The question for this boot is narrow: **does the per-write table confirm
`DP_VID_M`'s store is dropped**, with `was -> wanted -> got` read directly
rather than inferred? If it is confirmed, that is a register-level fact about
DIG2 worth its own line of work. If the table shows it taking, then boot 478's
changed-mask reading was wrong and says so.

**3. The blocker itself.** Nothing on the encoder side will move it. The
remaining untried surfaces, in the order their evidence is strongest:

- **Which RDPCSPIPE instance is UNIPHY_C.** Boot 471 left this an **OR**:
  RDPCSPIPE0 reads NOT DISABLED, RDPCSPIPE1 reads DISABLED+acked, and with
  three USB-C PHYs over two registers (upstream's own
  `//[Note] Hack. RDPCSPIPE only has 2 instances.`) rule 18 refuses the
  identification. `QUERY_DP_ALT` is ROM-attested on three boots and only PHY2
  reads `alt_disable` 0. Naming the instance would turn an OR into a reading.
- **The lane power / MPLL question, which has never been asked.** 301
  established that `RDPCSPIPE_PHY_CNTL6` carries **only** DPALT_DP4 / DISABLE /
  DISABLE_ACK — **no MPLL, no reference clock, no PSTATE** — and that
  conflating the two questions is what made `0x00063333` look like a verdict
  for five boots. Whatever powers the lanes is a different register and has
  not been located.

## Rules this step is standing on

- **Rule 15** — the UUID that matters is the one inside the collection:
  `kmutil inspect -B /Library/KernelCollections/AuxiliaryKernelExtensions.kc --show-kext-uuids | grep -i rembrandt`
- **Rule 20** — `changed` beside `applied`, and now `got` beside `want`.
- **Rule 39** — DIG1/DIG3 as a control is the live example: a control in a
  configuration that differs in an uncontrolled way settles nothing.
- **Rule 47** — Eco / iGPU-only mode, cable in at boot.
- `dptrain tps=3`/`tps=4` **degrade this sink until a reboot** (2F8).
