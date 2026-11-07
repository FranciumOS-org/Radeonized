# HANDOFF — where this is, and what to do next

**Written 2026-11-07, after 2F8. Read `CLAUDE.md` first, then this, then
`docs/EXTERNAL-DISPLAY.md`.**

---

## Read this before touching the monitor

**`dptrain go tps=3` degrades this sink and the only recovery is a reboot.**
2F8 measured it in two boots. The tool now refuses `tps=3`/`tps=4` with exit 3
(`force` overrides). **Use `tps=2`.** All of 2F7 — "the sink's DPCD reads zero",
both of its hypotheses and both of their funerals — was the aftermath of two
`tps=3` runs at the top of boot 459.

**The sink is degraded right now.** Boot 460 ended with `0x202` DEFERring, caps
reading `00 00 00 81 00 00 00 00 00 00 00 00 00 00 84 00` and the EDID header
BAD. **Nothing sink-side can be measured until a reboot.**

## Current state

- **Boot 460**, boottime Sat Nov 7 02:08:51 2026.
- Live collection **`89A6F41C`** = the **2F5** build. `build/out` holds
  **`A50B24F4`** = **2F6**. `sudo tools/fbinstall.sh` has not been run.
- `-rbgpufbtrain`, `-rbgpufbaux`, `-rbgpufbdto` are in boot-args.
- No `0x10de` on the PCI bus, so the chassis is in iGPU-only mode.
- `make kext tools`, `make fbkext`, `make dptrain` clean. Hosttest **all
  passed** — it needs the capture table as an argument
  (`./build/out/hosttest tests/data/rembrandt-680m-tufa15.bin`); run bare it
  reports 2 FAILs that are its own missing input.

### What is established about this sink

- Healthy, it reads **byte-identical to 2D8 and 2BB**:
  `12 14 c2 81 01 1d 01 81 02 00 04 04 3e 00 84 00` — revision 1.2, HBR2,
  2 lanes — with EDID header and checksum OK.
- A fresh boot leaves `0x100` = `0x06` (untrained), `0x102` = `0x00`,
  `0x202` = `0x00` and readable, `0x600` = `0x01` already D0. Our boot-457
  `0x100` = `0x14` does **not** survive a reboot.
- `dptrain go tps=2` gives the first honest **PATTERN SET**: `TP_SEL 0 -> 1
  CHANGED`, `moved 1 observable 1`, and both lane-status reads succeeding.
- DPCD `0x102`: stores `0x02` **only** when written inside `dptrain`'s own call
  (n=2 boots); a separate-call write of the same value with the TX unmoved does
  **not** store (in-boot control). `0x01` drives the byte to `0x00`, `0x03`
  never stores — both reproduced in two boots. **Transmitter movement is not
  the discriminator; same-call proximity is.**
- One EDID read needed **16 defers** and still succeeded, which is the sink's
  own argument for 2F6's retry bound.

---

## STEP 1 — reboot, then install 2F6 in the same trip

The sink needs the reboot anyway, so spend it once.

```bash
sudo tools/fbinstall.sh
```

```bash
kmutil inspect -B /Library/KernelCollections/AuxiliaryKernelExtensions.kc --show-kext-uuids | grep -i rembrandt
```

Expect **`A50B24F4-D737-38CE-8940-580772405B3E`** inside the collection, then
reboot and confirm it again from the running system (rule 15).

After that boot, verify the sink is healthy **before** writing anything:

```bash
build/out/edidread
```

`header OK   checksum OK` is the gate. If it is BAD on a fresh boot, stop and
say so — that would be new.

---

## STEP 2 — the atom transmitter ENABLE at the link rate

Still the next thing that **does not exist**, and still the step between "a
pattern is set on our side" and "lanes are driving". The sink side is now
instrumented well enough to give it a verdict: `0x202` reads on a healthy sink,
so `CR_DONE` is finally observable.

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
- **Budget one reboot per sink-degrading mistake.** That is now a known price,
  not a surprise.

After the ENABLE: link training over AUX2 (driver-side — zero `dmub` references
in `link_dp_training*.c`), `enc31_map_stream_to_link`, a second OTG and HUBP,
and a second `IOFramebuffer`.

**A dark monitor is still the expected reading** at every step above.

---

## Open, not on the critical path

- The surviving DPCD-store question: **why does `0x102` store only from inside
  the same call?** The honest shape is proximity in time; nothing establishes
  the mechanism, and 2F8's attempt to separate movement from proximity was
  **void** because the sink was already degrading from the `tps=3` that arm
  used. Re-run it on a healthy sink with `tps=2` only.
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

1. **The D3 / ordering hypothesis** (2F5). Boot 459 killed it; 2F8 adds that
   `0x600` already reads `0x01` on a fresh boot with nothing of ours setting it.
2. **2F4's "a training pattern on the wire".** Boot 458's four writes moved not
   one bit on a dark transmitter (rule 20's own defect).
3. **"The sink stores `0x102` only when the transmitter just moved"** (2F6).
   Dead — but 2F7's counterexample was right for the wrong reason, since that
   boot's reads were the TPS3 aftermath. 2F8 killed it properly, with a control.
4. **"Only `0x02` stores at `0x102`"** as a *value* rule. `0x02` also fails to
   store from a separate call.
5. **2F7 and 2F7a/b's sink-side readings as statements about the sink.** They
   were taken while the sink was degraded by our own `tps=3`. The port and
   read-path eliminations in 2F7b stand, because both rested on controls.
