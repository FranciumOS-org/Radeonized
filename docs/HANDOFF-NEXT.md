# HANDOFF — where this is now, and what to do next

## State

**305 wrote the stream on DIG2 in boot 477 and the hardware declined it.**
19/19 writes applied, every mechanical thing worked — the four MSA words read
back exactly, DIG2's three-boot held `DP_STEER_FIFO` reset cleared by the
unblank's own pulse, `DIG_FE_CNTL.DIG_SOURCE_SELECT` -> 1 = OTG1 (a step this
project had never performed), `DP_VID_STREAM_CNTL 0x00100000 -> 0x00100001` —
and **bit 16 STATUS read 0**. Verdict 2, NO_STATUS. DIG0's live panel stream
reads ENABLE 1 / STATUS 1 in the same sample; DIG1/3/4 read 0/0.

Eliminated in that boot: **reading too early** (~2900 vblanks at STATUS 0,
OTG1 2849 -> 2969 frames = 60.0 Hz) and **the back end being disabled**
(`dmubbl dig fe noenc` moved `DIG_BE_EN_CNTL` `0x100 -> 0x101` on DIG2 with a
four-instance control; STATUS unchanged).

**The finding was that the write list locked out its own next step.**
`dptrain go tps=2` refused 66 `RB_DPTX_W_STREAM_LIVE`. 305 had collapsed
`dp_set_stream_attribute` and `dp_unblank` into one list, and upstream keeps
them apart *because link training runs between them* (`enable_link_dp`).

**305b split them. Built, hosttested, not booted.**

## Do this next

**1. Install and boot.** Framebuffer UUID **5AA98CD9-8CFE-33D1-B0D8-A623843C5B29**.

```bash
sudo tools/fbinstall.sh
```

Then reboot **with the Type-C cable plugged in before boot** (2FB: plugged in
afterwards, every AUX read refuses 3 on a nonexistent engine). `-rbgpufbstrm`
should already be in the boot-args from boot 475; verify from the running
system, not from `nvram`:

```bash
sysctl -n kern.bootargs | tr ' \t' '\n\n' | grep -qx -- -rbgpufbstrm && echo ARMED || echo NOT ARMED
```

**2. In that boot, in this order — all four steps, same boot.** The raster and
the clock are **not persistent**; `strencprog` refuses without them.

```bash
sudo build/out/dmubbl dto 148500
```

```bash
sudo build/out/otgprog go 1080p60
```

```bash
sudo build/out/strencprog go
```

```bash
sudo build/out/dptrain go tps=2
```

```bash
sudo build/out/strencprog unblank
```

`strencprog go` leaves `DP_VID_STREAM_ENABLE` 0 on purpose — that is what
lets `dptrain` run at all. **Never `tps=3` or `tps=4`: they degrade the sink
until a reboot** (2F8), and the tool refuses them.

**3. The question this boot asks.** `dptrain` has never moved DPCD `0x202`
off `0x00` — no clock recovery, across boots 459, 461, 464, 468, 469. If it
still reads `0x00`, the training itself is the blocker and the stream's
STATUS 0 is downstream of it. If `0x202` finally moves and
`strencprog unblank` then reads **STATUS 1**, the encoder chain is complete
and what remains is a **surface** on pipe 1 and a second `IOFramebuffer`.

Both outcomes are results. Report `RBFBStrmVerdict`, `RBFBStrmCntlAfter` and
DPCD `0x202` before and after, and keep the four-instance control in the same
sample.

## Rules this step is standing on

- **Rule 15** — check the UUID *inside the collection*, not in
  `/Library/Extensions`:
  `kmutil inspect -B /Library/KernelCollections/AuxiliaryKernelExtensions.kc --show-kext-uuids | grep -i rembrandt`
- **Rule 34** — the arm is part of the experiment. `strencprog` cross-checks
  `-rbgpufbstrm` against `kern.bootargs` itself and says so if the kernel has
  the token but the kext read NOT ARMED (boot 475's defect).
- **Rule 47** — Eco / iGPU-only mode in the chassis firmware, and the cable
  in at boot.
- **Rule 31** — `RBFBStrm*` is published periodically; `strencprog` re-samples
  inside the call rather than trusting the published licence.
