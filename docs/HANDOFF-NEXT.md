# HANDOFF — where this is, and what to do next

**Written 2026-11-07, after 2F7. Read `CLAUDE.md` first, then this, then
`docs/EXTERNAL-DISPLAY.md` for the external-display record in full.**

---

## Current state, as measured

- **Boot 459 is still the live boot** (`kern.boottime` Sat Nov 7 01:26:17 2026,
  `rbgpu-boot` 459). 2F7 ran entirely inside it, no reboot.
- **`build/out/RBAMDFramebuffer.kext` is now `A50B24F4-D737-38CE-8940-580772405B3E`**
  — the **2F6** build, built for the first time in 2F7. The **live** collection
  still carries `89A6F41C` (2F5). **They no longer agree**, so boot 459's
  readings belong to 2F5 and nothing new should be read against them.
- `make kext tools`, `make fbkext`, `make dptrain` all clean. Hosttest **all
  passed** — and note it needs the capture table as an argument
  (`./build/out/hosttest tests/data/rembrandt-680m-tufa15.bin`); run bare it
  reports 2 FAILs that are its own missing input, not defects.
- `-rbgpufbtrain` is in boot-args and arms the AUX half itself.

### What 2F7 settled

- **Both DPCD-store hypotheses are DEAD.** The "stores only when the
  transmitter just moved" one was killed by its own pre-registered test: `TP_SEL
  0x1 → 0x2 CHANGED, moved 1 observable 1` and the sink **ACKed `0x03` and
  stored nothing**. The replacement "only `0x02` stores" died minutes later to a
  bare `dpcdwrite 0x102 0x02` that also did not store.
- **The control fired exactly as predicted**: the second `tps=3` gave
  `moved 0 observable 0`, `VERDICT 4 NO-OP`.
- **The sink's whole DPCD now reads zero.** `0x000..0x003` ACKs with
  `00 00 00 80` (revision 0, max rate 0, max lanes 0 — no sink reports that);
  `0x100` reads `00 00` where **boot 457 read `0x06` then `0x14`**; `0x202`
  DEFERs on every attempt; `0x600 = 0x01` ACKs and stores nothing.
- **Therefore an ACK is not a reading**, and every `ACK_IGNORED` / `changed 0`
  verdict is computed from these same reads. 2F6's "three outcomes at one DPCD
  address" stands as a reading of its moment but **cannot be reproduced**.
- The sink is still there: `RBFBHpd2Status` = `0xFF0FF013` (`INT_STATUS`,
  `SENSE`, `SENSE_DELAYED` all set), and a DEFER is the sink declining an
  address, which is proof the channel reaches it.

---

## STEP 1 — get the sink answering again. This is the blocker.

Nothing sink-side means anything until this is fixed, so it sits **in front of**
the atom transmitter ENABLE.

**2F7a narrowed it.** After a monitor power-cycle `dpcdread 0x0 4 2` still read
`00 00 00 80`, and the **EDID over I2C-over-AUX** — a different endpoint on the
same AUX engine, which read `MSI MP2412C` in 2BD — now returns **128 bytes of
`0x00`**, `header BAD`. Both endpoints zero rules out the DP receiver's own
state as the sole cause. Caveat: `RBFBHpdChanges` is **0** for the whole boot,
so the HPD line is not known to have moved and the power-cycle is a weaker arm
than intended.

What is left, in order of what to test:

1. **The Type-C port or the AUX channel.** CLAUDE.md's rule 47 already records
   this symptom shape: this chassis' Type-C subsystem can die in a way that
   survives a reboot and a reinstall and is cleared only by a **30-second
   power-key hold**.
2. **We did it.** Boot 457 read both endpoints correctly. Since then we wrote
   `0x100` = `0x14` (HBR2) into a receiver whose caps now read rate 0, wrote
   training patterns to `0x102`, and moved `TP_SEL` on DP2 three times. Not an
   established mechanism, but the first candidate that is ours.
3. **Our read path**, which would have to be zeroing *both* the native and the
   I2C paths to explain this.

### The run that settles it

Do these in this order, and do not let step 3 happen before step 2.

1. Shut down, then hold the power key for 30 seconds (rule 47's recovery).
2. Boot, and **read the EDID before writing anything**:
   `build/out/edidread`.
3. Only then anything else.

- **EDID reads `MSI MP2412C`** → this boot's zeros were a state the port or our
  writes entered. Candidate 2 goes to the front, and every write since boot 457
  is a suspect. Re-read `dpcdread 0x0 4 2` immediately after, before any write,
  to get a clean caps reading this project has never had.
- **EDID still zero** → the port is the suspect, no sink-side reading is
  available, and nothing further in the external-display chain can be verified
  until it comes back.

**Do not** re-run `dpcdwrite 0x600 0x01` as a D3/ordering probe. 2F5's ordering
hypothesis is withdrawn and 2F7 already ran that write for a different reason:
it ACKed, stored nothing, and changed neither `0x000` nor `0x100`.

## STEP 2 — install the 2F6 build

`build/out` now holds it and the UUID has moved off `89A6F41C`.

```bash
sudo tools/fbinstall.sh
```

```bash
kmutil inspect -B /Library/KernelCollections/AuxiliaryKernelExtensions.kc --show-kext-uuids | grep -i rembrandt
```

Expect **`A50B24F4-D737-38CE-8940-580772405B3E`** inside the collection, then
reboot and confirm it again from the running system (rule 15).

2F6 earns its install now for a **measured** reason, not tidiness: `0x202`
DEFERs on every attempt and the 2F5 build is single-shot on it. After that boot
the DPCD paths retry a DEFER 8 times at 1 ms and publish
`RBFBAuxDpcdDefers` / `RBFBAuxWDefers`, so a recovered defer, an exhausted
bound and a single NACK become three readings instead of one. It also prints
**`ACK + OTHER`** when the sink moves a byte to a value of its own.

---

## STEP 3 — the atom transmitter ENABLE at the link rate

Still the next thing that **does not exist**, and still the step between "a
pattern is set on our side" and "lanes are driving". It is now **behind** step 1,
because its verdict would have to come from the sink.

What must be respected:

- **Order matters:** FE connect → configure encoder → transmitter ENABLE. The
  atom transmitter command carries **no front end, by design**.
- The DMUB `DMUB_CMD__VBIOS` path **executes** (`DIG_BE_EN_CNTL.DIG_ENABLE`
  0→1 on the targeted instance alone, two boots) **and answers** (reply at
  REGION4; `ret_status` stays SET, which Linux's own criterion would discard).
  2E9 paid for that.
- **PHY0 / DIG0 / front end DIGA are the panel's and are refused by name** in
  every half of every one of these experiments. Keep it that way.
- Rule 46 before anything is written: discharge the **method's** version
  grouping in `dcn31_resource.c` by source, not by reasoning, exactly as 2F4
  did for `.dp_set_phy_pattern`.
- Rule 47 asked of a write: establish the live word decodes as the register the
  conclusion assumes **before** writing it.

After the ENABLE: link training over AUX2 (driver-side — zero `dmub` references
in `link_dp_training*.c`), `enc31_map_stream_to_link`, a second OTG and HUBP,
and a second `IOFramebuffer`.

**A dark monitor is still the expected reading** at every step above.

---

## Open, not on the critical path

- The **wedge**: work submitted that never retires — channel-52 GFX, 3.5 s, the
  "random lag". `stamp_ticks` 125666 retires the "nothing polls it" family.
- `powermetrics` has **never been run** against the default 576 interior
  thread_calls/s, the one real argument against the 2D5 default.
- `rb_osver.h` claims **nothing** for Ventura or Sonoma.
- **Owed:** `docs/UPSTREAM.md` entries for the Linux files read across 2F3-2F6
  (`drm_dp.h`, `dcn10_link_encoder.c`, `dcn31_dio_link_encoder.c`,
  `dce_aux.c`).

---

## Withdrawn, so they are not re-tried

1. **The D3 / ordering hypothesis** (2F5). Boot 459 killed it: nothing about
   ordering differed between the two boots.
2. **2F4's "a training pattern on the wire".** Boot 458's four writes moved not
   one bit; `applied 1` was honest because every target state was already
   present on a dark transmitter (rule 20's own defect).
3. **"The sink stores `0x102` only when the transmitter just moved"** (2F6's
   hypothesis). 2F7 produced the direct counterexample.
4. **"Only `0x02` stores at `0x102`"** (2F7's own replacement, same day).
   A bare `dpcdwrite 0x102 0x02` did not store.
5. Any claim resting on `changed` or `before`/`after` at a DPCD address **while
   the DPCD reads zero**. That is the whole `ACK_IGNORED` family.
