# HANDOFF — what to do now

**State: 305 is built and not yet booted.** Framebuffer UUID
**`F4351B09-8037-3147-B221-D87984F2DC33`**. This build can **write** the DP
stream on DIG2 — the first write this project makes to that block — but only
when armed, triggered, and licensed by its own sample.

Boot 474 passed the gate 304 built: `RBFBStrencMsaMismatch` **0**, `Base`
CONFIRMED, `ActiveAgree` 1, panel RUNNING, and PARAM3's sync and polarity
inputs read rather than assumed.

---

## Step 1 — add the arm, install, boot

**The cable must be plugged in at boot** (2FB). Plugged in afterwards every
AUX read returns refusal 3 and `AUX_EN` reads clear.

**The boot-arg is already added** (boot 475 confirmed it in the kernel's own
`kern.bootargs`). Boot 475 refused anyway because `hubpSetup()` cleared the
arm after `start()` parsed it; that is fixed in this build. **Do not re-add
the token** — `strencprog` now cross-checks it against `kern.bootargs` and
will say so if the flag is ever clobbered again.

```bash
sudo tools/fbinstall.sh
```

Then the UUID **inside the collection**, not in `/Library/Extensions` (rule
15, which cost nine boots in 2xx):

```bash
kmutil inspect -B /Library/KernelCollections/AuxiliaryKernelExtensions.kc --show-kext-uuids | grep -i rembrandt
```

It must read `F4351B09-8037-3147-B221-D87984F2DC33`. Reboot, then verify the
arm **from the running system** — a boot-arg that reads back is not one that
survived (rule 43):

```bash
nvram boot-args | tr ' \t' '\n\n' | grep -qx -- '-rbgpufbstrm' && echo ARMED || echo NOT ARMED
```

## Step 2 — the raster, in this same boot

**The pipe 1 raster is not persistent.** Boot 474 read all eight of its OTG
words as zero with `-rbgpufbraster` armed, because the arm only permits and
`otgprog … go` is what writes. So it must be programmed in the same boot as
the stream, or `strencprog` refuses with code 5 and names this step.

```bash
tools/strencprog
```

Read-only. It prints the licence first and writes nothing. Both must hold:
`RBFBStrencMsaMismatch` **0** and pipe 1's `DeriveRc` **0**. If pipe 1 is not
0, program its raster with `otgprog` (1920x1080 at 60, the sink's EDID
preferred timing, 148500 kHz) and the DTO, then run `strencprog` again.

## Step 3 — the stream

```bash
sudo build/out/strencprog go
```

Defaults are DIG2 from OTG1 at 148500 kHz over HBR2 (`rate=20`), which gives
`m_vid` 9011 and `n_vid` `0x8000`. DIG0 and OTG0 are refused **by name** in
both the tool and the kext.

**The verdict is `DP_VID_STREAM_CNTL` bit 16 `STATUS`, not bit 0 `ENABLE`.**
Bit 0 is what we wrote; bit 16 is what the hardware answers, and upstream
`REG_WAIT`s on it. The tool prints both separately:

- **VERDICT 1 RUNNING** — ENABLE 1 and STATUS 1. The block says it is carrying
  a stream. **This is not a picture** (see below).
- **VERDICT 2 NO_STATUS** — we asked and the hardware did not answer. The most
  informative single result this line could get: it would be the first time
  the encoder has been asked for a stream and declined, and the refusal is
  the hardware's, not ours.
- **VERDICT 3 NOT_ENABLED** — the write did not take at all. Check
  `RBFBStrmChanged`.
- **refusal 54 STATUS_STUCK** — `STATUS` never cleared after `ENABLE` 0 and
  the rest of the sequence was deliberately **not** sent.

Also read, in the same output: `DP_STEER_FIFO` bit 0 must now be **0** (DIG2's
held reset, set in boots 472/473/474, is cleared by the unblank's own pulse),
and `DIG_FE_CNTL` bits 2:0 must now be **1**. That second one has **never been
written before 305** — see the journal; it was the front-end-to-OTG mux, a
different register from 2EC's one-hot BE→FE mux, and DIG2 had it pointed at
the panel's pipe.

## Step 4 — ask the SINK, which is the only witness that counts

Our own registers can only say our stores landed; taking that as the answer is
rule 20's defect.

```bash
sudo build/out/dptrain go tps=2
```

Then read DPCD `0x202` over AUX2, **before and after**, since "CR not done" is
the state it is already in and only a **change** is evidence. `0x202` has read
`0x00` in every boot that has tried, and the blocker is still unnamed.

**`tps=3` and `tps=4` DEGRADE this sink and the recovery is a REBOOT** (2F8,
two boots, and all of 2F7 was its aftermath). The tool refuses them. Use
`tps=2`.

## What a RUNNING stream does and does not mean

**A stream is not a picture.** There is no surface on pipe 1 — no HUBP, no
address, no swizzle — so even STATUS 1 means the encoder is carrying a raster
whose pixels are whatever the unprogrammed HUBP feeds it. That is a real
intermediate result and should be recorded as one, not as success and not as
failure. A second `IOFramebuffer` is also still missing;
`getDisplayModeCount()` has returned 1 since 2sl.

## Rescue, if the boot goes wrong

- `sudo nvram rbgpu-arm=0` — accelerator off next boot.
- **`-rbgpuoff`** — the plugin loads and does nothing. The documented rescue.
- The **boot sentinel** needs no command: a boot that does not survive 60 s
  makes the next one stock.
- **Never `kmutil unload` the framebuffer kext — it PANICS** (2sq). Reboot.
- The Type-C subsystem can die in a way that survives a reboot and a
  reinstall, cleared only by a **30-second power-key hold** (2DD).
- The chassis must be in **iGPU-only / Eco mode** (rule 47).
