# HANDOFF — where this is, and what to do next

**Written 2026-11-07, after 2F6 (`64fc2e0`). Read `CLAUDE.md` first, then this.**

---

## Current state, as measured

- **Boot 459.** Framebuffer collection UUID **`89A6F41C-C283-37D0-BA8D-C86C17820ED9`**
  = the **2F5** build. Verified inside the collection *and* against
  `build/out`, which has deliberately **not** been rebuilt since, so the two
  still agree and boot 459's readings can still be re-read against the same
  kext.
- **2F6 (`64fc2e0`) is committed and pushed but NOT INSTALLED.** Its changes are
  instrument-only (a fifth DPCD write verdict, defer retries, tool wording).
  Nothing in boot 459's readings came from it.
- `-rbgpufbtrain` is in boot-args and arms the AUX half itself (rule 34). No
  EFI edit is needed for anything below.

### The external-display chain, as it stands

| step | state |
|---|---|
| port identification (HPD2 = AUX2 = DIG2 = UNIPHY_C) | **done**, four independent instruments |
| DP alt mode granted | **done** (DMUB `QUERY_DP_ALT` + the ROM) |
| EDID over I2C-over-AUX | **done** |
| DP pixel clock (DTO on pipe 1) | **done**, boot 455 |
| sink takes native DPCD writes | **done**, boot 457 (`0x100` `0x06 -> 0x14`) |
| **DP transmitter block answers our writes** | **done**, boot 459 (`TP_SEL 0 -> 1` on DP2, with an in-boot control) |
| atom transmitter ENABLE at the link rate | **DOES NOT EXIST** — the blocker |
| `enc31_map_stream_to_link` | not started |
| second OTG + HUBP | not started |
| second `IOFramebuffer` | not started (`getDisplayModeCount()` has returned 1 since 2sl) |

**A dark monitor is still the expected reading.** Nothing above puts a picture
on the external display, and nothing should be read as if it might.

---

## STEP 1 — free, this boot, no build, no install

The open question from boot 459: **does the sink store DPCD `0x102` only when
the transmitter side has just moved?** Across four observations the only write
that stored is the only one where `TP_SEL` changed — but that is **n=1 for the
storing case**, so it is a hypothesis, not a finding (rules 18, 44).

Run these two, in this order, in the same boot:

```bash
build/out/dptrain go tps=3
```

```bash
build/out/dptrain go tps=3
```

### Predictions, written down BEFORE the run

- **First call.** `TP_SEL` is `1` now; TPS3 encodes as hardware `2`, so the TX
  side moves. Expect `moved 1  observable 1`, `TP_SEL 0x1 -> 0x2 CHANGED`, and
  **the DPCD write to store `0x03`**.
- **Second call.** `TP_SEL` is already `2`; nothing moves. Expect
  `moved 0  observable 0` and **`VERDICT 4  NO-OP`** — 2F5's own verdict, doing
  its job. Prediction: **the DPCD write does NOT store.**

### How to read it

The DPCD half is **not** in `dptrain`'s own output on this build beyond its
return code. Read it from the AUX write record after each call:

```bash
ioreg -rl -c RBAMDFramebuffer | grep RBFBAuxW
```

- `RBFBAuxWVerdict 1` = ACK_STORED, `2` = ACK_IGNORED, `3` = ACK_BLIND,
  `0` = no ACK. Read `Before`, `After` and `Changed` beside it — **`Changed 1`
  with `Applied 0` means the byte moved to a value the sink chose**, which the
  2F5 build mislabels as IGNORED (2F6 fixes the label, not the numbers).
- `RBFBAuxWCalls` is monotonic; poll it past its previous value rather than
  comparing snapshots (rule 31).

### What each outcome means

- **First stores, second does not** → the hypothesis holds on both sides with a
  control, inside one boot. That is a real finding about this sink and it goes
  in the journal as 2F7.
- **Both store** → the hypothesis is dead; boot 458's ignore was something else
  (most likely transient). Say so plainly and move to step 2.
- **Neither stores** → boot 459's single store is the outlier and needs its own
  explanation before anything is built on it.
- **A `DEFER`** → the 2F5 build is single-shot on it and will print it as "no
  ACK". Re-run the command; that is exactly what 2F6 fixes.

---

## STEP 2 — install 2F6, then re-read

Instrument-only, but it changes how every later DPCD reading prints.

```bash
sudo tools/fbinstall.sh
```

```bash
kmutil inspect -B /Library/KernelCollections/AuxiliaryKernelExtensions.kc --show-kext-uuids | grep -i rembrandt
```

`make fbkext` has **not** been run since 2F6 was committed, so build it first
and check the UUID moved off `89A6F41C` before installing. Then reboot and
confirm the new UUID **inside the collection** (rule 15).

After that boot, `dpcdwrite` and `dpcdread` will:
- print **`ACK + OTHER`** when the sink moves a byte to a value of its own,
  instead of claiming "the byte did not change" beside a `changed 1`;
- retry a **DEFER** up to 8 times, 1 ms apart, and publish
  `RBFBAuxDpcdDefers` / `RBFBAuxWDefers` so a recovered defer, an exhausted
  bound and a single NACK are three readings and not one.

---

## STEP 3 — the blocker: the atom transmitter ENABLE at the link rate

This is the next thing that **does not exist**, and it is the step between "a
pattern is set on our side" and "lanes are driving".

What is already established and must be respected:

- **Order matters:** FE connect → configure encoder → transmitter ENABLE. The
  atom transmitter command carries **no front end, by design**.
- The DMUB `DMUB_CMD__VBIOS` path **executes** (`DIG_BE_EN_CNTL.DIG_ENABLE`
  0→1 on the targeted instance alone, two boots) **and answers**
  (reply at REGION4; `ret_status` stays SET, which Linux's own criterion would
  discard). 2E9 paid for that.
- **PHY0 / DIG0 / front end DIGA are the panel's and are refused by name** in
  every half of every one of these experiments. Keep it that way.
- Rule 46 before anything is written: discharge the **method's** version
  grouping in `dcn31_resource.c` by source, not by reasoning, exactly as 2F4
  did for `.dp_set_phy_pattern`.
- Rule 47 asked of a write: establish the live word decodes as the register the
  conclusion assumes **before** writing it.

After the ENABLE, the remaining chain is link training over AUX2 (driver-side —
there are zero `dmub` references in `link_dp_training*.c`),
`enc31_map_stream_to_link`, a second OTG and HUBP, and a second
`IOFramebuffer`.

---

## Open, not on the critical path

- The **wedge**: work submitted that never retires — channel-52 GFX, 3.5 s, the
  "random lag". `stamp_ticks` 125666 retires the "nothing polls it" family.
- `powermetrics` has **never been run** against the default 576 interior
  thread_calls/s, which is the one real argument against the 2D5 default.
- `rb_osver.h` claims **nothing** for Ventura or Sonoma.
- **Owed:** `docs/UPSTREAM.md` entries for the Linux files read across 2F3-2F6
  (`drm_dp.h`, `dcn10_link_encoder.c`, `dcn31_dio_link_encoder.c`,
  `dce_aux.c`).

---

## Two things I got wrong, so they are not re-tried

1. **The D3 / ordering hypothesis is WITHDRAWN.** 2F5 recorded boot 458's
   `ACK_IGNORED` as probably a receiver left in D3 with `0x600` never set to
   D0. Boot 459 killed it: nothing about ordering differed between the two
   boots — no `0x600`, no `0x100`/`0x101`, same engine, same sink, same `0x00`
   starting byte. **Do not run `dpcdwrite 0x600 0x01` as that probe.**
2. **2F4's "a training pattern on the wire" is WITHDRAWN.** Boot 458's four
   writes moved not one bit; `applied 1` was honest because every target state
   was already present on a dark transmitter (rule 20's own defect). `TP_SEL`
   first genuinely moved in boot 459, with `tps=2`.

The DEFER retry in 2F6 is a **real** defect fixed, but it explains none of the
storing behaviour — it is robustness, not an answer.
