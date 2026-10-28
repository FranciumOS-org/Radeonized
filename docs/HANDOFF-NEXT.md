# HANDOFF — next session

**State: the display pipe ACCEPTS transactions. Every failure in the chain is
at zero, and Apple's accelerator is handing cooked transactions to our
framebuffer. What remains is that we answer them without drawing anything.**
Read `CLAUDE.md`'s **2we** block first, then 2wd/2wc/2wb. `docs/JOURNAL.md`
has the full reads under those tags.

## 1. Where boot 193 left it

`logs/20261028-101029-plugin`, `rbgpu-boot` 193, armed, both one-shots 0,
`diag_used` 664/768, 20 files. Rule 15 on both kexts: plugin **`0D1F953A`**,
fb **`6F1EB613`**. Arm check for the experiment itself:
**`agdc_gamma_elems1 = 256`**.

With `display_change_handler` **26** as the control in all five boots:

| | 191 | 192 | **193** |
|---|---|---|---|
| `set_transaction_args … 0xe00002c7` | 122 | 132 | **0** |
| `Failed to submit transaction` | — | 134 | **0** |
| `Fallback … submission failed` | 3 | 66 | **0** |
| `DisplayPipeState` | — | 66 | **0** |
| `RBFBAmtrSets` | — | 0 | **13** |

`Metal compositor activated` ×4. `no gamma` 0. `RBFBAmtrSnapped` true, and the
snapshot's pointer is **exactly `pipe + 0x410`**, with all five of its interior
pointers matching the `leaq` addresses in `executeTransaction` — five controls
on 2wc's field map in one reading. The flag word `0x75` decomposes exactly as
`dirtyBits 0x3c1` predicts, **including `0x20`, the pipe post-gamma block that
was refusing everything one boot earlier**.

**Both of this session's changes were necessary**: the `'amtr'` answer (2wc)
and the 256 gamma ceiling (2wd). 193 proves the first was necessary and not
sufficient; 192 proves the second alone would not have been reached.

## 2. The next step needs NO boot

Read **`AMDFramebuffer::handleDisplayPipeTransaction` (`0x2b4df0`)** against
the snapshot's five pointers and work out what each sub-struct holds:

| snapshot | pipe offset | what we know |
|---|---|---|
| +0x10 | `pipe + 0x36b0` | plane state — **carries the surface the compositor rendered into** |
| +0x20 | `pipe + 0x3660` | written in the plane dst-rect block (flag 0x4) |
| +0x30 | `pipe + 0x3680` | colour matrix (flag 0x10) |
| +0x38 | `pipe + 0x2650` | pipe post-gamma (flag 0x20) |
| +0x40 | `pipe + 0x38d0` | pre-CSC linearization (flag 0x40) |

The prize is the surface address at `pipe + 0x36b0`. That is what DCN 3.1.2's
`HUBP*_DCSURF_PRIMARY_SURFACE_ADDRESS*` pair has to be pointed at, and
**taking it out of the transaction is strictly better than inventing one** —
it is the only number in this project that is not a substitute (rule 17).

Grep `docs/JOURNAL.md` before reading any Apple function in that path; three
milestones have now re-read something already settled.

## 3. Then, on its own risk: the first write to the display engine

This kext has **never** written to DCN. A wrong write blanks the panel.
Registers from `docs/linux-ref/`, `DCN_VERSION_3_1` grouping, `dcn_3_1_2`
offsets — **check the grouping before adding one**. Own milestone, own
boot-arg, off by default, and the probe rules in `CLAUDE.md` still apply to
everything that is not that one deliberate write.

Until then the picture stays wrong by construction:
`setAttribute('amtr', …)` answers success without doing the work, and
`-rbgpufbaty` publishes a 16 MB offset while OTG0 scans VRAM offset 0.

## 4. The wedge is now the only other defect, and it is independent

First `checkGPUProgress` at 10:07:57.763, 15 s after boot, running the whole
2 m 48 s capture: 63 `Signaling hardware error`, 36 `channel N is not hung`,
36 `no channel associated with stamp_idx N`, 26 `SDMA0_PAGE event timeout`,
9 GFX, 1 VMPT, 3 `GPURestartDeferred`.

2w4 saw it start six seconds after the first submit failure and could not say
which way causation ran. **Boot 193 has no submit failures at all and the
wedge is unchanged in shape and slightly larger in count** — so it is not
caused by the display path. Clean differential, n=1. It is a stamp-retirement
failure: work submitted, never retires, hang check says not hung, restart
cannot find the channel. It has never had a milestone of its own.

## 5. Arming, unchanged

Nothing to install — plugin `0D1F953A` is in the EFI, fb `6F1EB613` is in
`/Library/Extensions` and the AuxKC, and boot-args already carry
`-rbgpufbaty`, `-rbgpufbamtr`, `-rbgpuagdc`, `-rbgpuagdccaps`,
`-rbgpuaccelprobe`, `-rbgpuownfb`. Every armed boot still needs all four
one-shots (rule 9/34):

```bash
sudo nvram rbgpu-arm=1 rbgpu-agdc=1 rbgpu-iop=1 rbgpu-fb=1
```

Reading order: `rbgpu-boot`, both kext UUIDs, the four `*_oneshot` keys,
`diag_used` vs `diag_max`, `agdc_gamma_elems1`, the `ax0_*` probe keys as the
control that the ATY chain is still open, then `RBFBAmtr*`, then the kernel
log **by sender** with `display_change_handler` as the control (rule 36).

## 6. Carried over, unexplained

- `Setting main display 0x… offline` ×7 and `Setting offline display 0x… main
  in CGXConstructVirtualFramebuffer` ×7, both ERROR, **new in 193** and absent
  from 192. Normal mode-set that the earlier boots never reached, or a new
  defect — **not established**.
- `IOAccelSurface::set_id_mode: Surface mode contains bad bits` ×4, unchanged
  across five boots.
- `RBFBVBL1Fires` 0 with `RBFBVBLFires` 21218, where 191 saw Apple's handler
  fire once. Now less interesting: the transactions go to `setAttribute`, not
  through the VBL ring.
- 2sv's controlled zero for `IOAccelDisplayPipeCapabilities` against
  2u1/2u2/2w0's three non-zero reads.
- `IOAccelDisplayPipeUserClient2` counting 2 with `IONDRVFramebuffer`, 1 with ours.
- WindowServer CPU% never attributed in a controlled read.
- **~79 unpushed commits. Pushing needs asking.**
