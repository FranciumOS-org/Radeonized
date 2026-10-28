# HANDOFF — next session

**State: the pipe is ACTIVE, the failure code has moved from
`kIOReturnNotReady` to `kIOReturnUnsupported` and now reaches CoreDisplay, and
one capability number we publish is 1024 where Apple's own accelerator refuses
anything over 256.** Read `docs/RULES.md`'s **2wd** block first, then 2wc and 2wb.
`docs/JOURNAL.md` has the full reads under those tags.

## 1. Where boot 192 left it

Complete capture, `logs/20261028-005656-plugin`, `rbgpu-boot` 192, armed, both
one-shots 0, `diag_used` 663/768, **rule 15 satisfied on both kexts** (plugin
`64D3D687`, fb `6F1EB613`).

- The ATY chain from 2w9/2wb is **reproducible**: `fi340 16777216`, `r380 1`,
  `active 1`, `gate 0`, all three samples identical.
- **`'amtr'` is answered**: `RBFBAmtrGets 3`, `RBFBAmtrSets 0`. 2wc's chain
  down to the attribute is a real call path; nothing reached `setAttribute`.
- **The refusals did not move**: `set_transaction_args ... 0xe00002c7`
  122 → 132, same `dirtyBits 0x3c1 / options 0x801`,
  `display_change_handler` 26 as the control in both. So `vt[0x190]` is not,
  or not the only, thing refusing — and boot 192 **cannot** tell "we opened
  gate 1 and met gate 2" from "gate 1 was never shut". Do not write either as
  established.
- **What it did settle**: `Failed to submit transaction ... IOReturn: 0x` is
  132 × `0x2c7` + 2 × `0x2bc`, where boots 185/186 had 118/152 **all `0x2d8`**.
  The `kIOReturnNotReady` era is over, confirmed from the consumer.
  `Fallback` 66, `DisplayPipeState` 66, **132 = 2 × 66**.

## 2. The experiment to run: ONE armed boot of plugin `0D1F953A`

`RB_AGDC_GAMMA_CHANNELS` 0x400 → **0x100**. `executeTransaction` refuses the
whole transaction when the **pipe post-gamma** table carries more than 256
entries (`cmpl $0x100,%r13d; ja` at `0xbe4a350`), and that count is
`*(uint32_t *)(table->0x10)` — the first dword of the data **WindowServer
supplied**, so the size is CoreDisplay's choice and `MaximumSize` is the only
lever we hold. Boot 192 measured `agdc_gamma_elems1 = 1024`.

2wc had excluded this ceiling using the `LUT` line in `DisplayPipeState` — the
dump's **per-plane** gamma, under `Plane Index: 0`, whose dirty bit `0x4` is
**clear** in `0x3c1`. Wrong table; rule 33's third instance.

**The plugin boots from the OpenCore EFI** (rule 15 — each artifact has its own
install path and a rebuild refreshes none of them):

```bash
sudo tools/efi.sh install
```

The framebuffer is **unchanged** — `6F1EB613` is already in
`/Library/Extensions` and in the AuxKC; do not reinstall it. No boot-arg
change: `-rbgpufbaty`, `-rbgpufbamtr`, `-rbgpuaccelprobe` and
`-rbgpuagdccaps` are all already there. Then, every armed boot (rule 9/34 —
all four, every time):

```bash
sudo nvram rbgpu-arm=1 rbgpu-agdc=1 rbgpu-iop=1 rbgpu-fb=1
```

**Run `sudo tools/plugin_check.sh` to completion** over SSH before rebooting.

## 3. What to read, in order

1. `rbgpu-boot` — must have increased (rule 8).
2. Identity for **both** kexts (rule 15): `kexts.txt` must show plugin
   **`0D1F953A`** and fb **`6F1EB613`**. The fallback identity check for the
   framebuffer is `RBFBAmtrArmed` / `RBFBAtyArmed` in `framebuffer.plist`
   (published *unconditionally*); for the plugin this boot it is
   **`agdc_gamma_elems1 = 256`**, which is also the experiment's own arm check.
3. All four `*_oneshot` keys (rule 34), then `diag_used` vs `diag_max` (rule 14).
4. `agdc_gamma_elems1` from `peer.plist`, then the `ax*_` probe keys as the
   control that the ATY chain is still open.
5. `ioreg -rl -c RBAMDFramebuffer`: `RBFBAmtrGets`, `RBFBAmtrSets`,
   `RBFBAmtrSnapped`, `RBFBAmtrPtr`, `RBFBAmtrRefused`, `RBFBAmtrSnap`.
6. The kernel half of the log **by sender** (rule 36), counting
   `set_transaction_args` against `display_change_handler` as the control, and
   the `IOReturn: 0x` histogram (132 × `0x2c7` in boot 192).

**Stated before the boot:**

- **`agdc_gamma_elems1` must read 256.** If it reads 1024 the plugin did not
  move and nothing else in the capture means anything (rule 15).
- Refusals → **0** and `RBFBAmtrSets` non-zero = the ceiling was the gate, and
  §4 becomes live.
- Refusals → 0 but `RBFBAmtrSets` still 0 = it validates and CoreDisplay still
  will not submit. Read `Fallback` and `DisplayPipeState` next, not the kernel.
- Refusals still ~132 = the ceiling is **properly** excluded this time, and
  the unconditional `vt[0x190]` gate is the live candidate again. **Then probe
  it rather than guessing a third time**: publish
  `((uint8_t *)hwd->0x40)[fbIdx*0xd0 + 1]` and the whole dword beside it from
  `-rbgpuaccelprobe`, which already walks to `pipe->0x330`. 2w7 and 2wa were
  both settled that way; 2wc was not.
- `no gamma` must stay **0** (2u6). 256 is non-zero, so the WARN that fired
  only at zero should not return — if it does, that is its own finding.
- **The picture will still be wrong.** Nothing is written to DCN and
  `-rbgpufbaty` still publishes a 16 MB offset while OTG0 scans offset 0. The
  user reported boot 191 as the Apple logo and progress bar with the OS live
  underneath over SSH, which is exactly what a never-composited panel looks
  like.

## 4. Then: `RBFBAmtrSnap` is the prize (still unreached — Sets was 0 in 192)

The pointer `setAttribute('amtr', value)` receives is **`&pipe->0x410`** —
the transaction struct `executeTransaction` has just finished filling. Known
field map, relative to that pointer:

| off | what `executeTransaction` writes |
|---|---|
| +0x00 | flag word: ORs 0x1, 0x2, 0x4, 0x8, 0x10, 0x20, 0x40, 0x100, 0x201 |
| +0x08 | `0x418`, mode bits 1/2 from the WSAA state and options |
| +0x10 | plane `IOAccelResource2` (`0x420`) |
| +0x18, +0x20 | `0x428`, `0x430` — destination rect / pipe scaler |
| +0x30, +0x40, +0x50 | `0x440` colour matrix, `0x450`, `0x460` |
| +0x210, +0x218 | `0x620` surface size, `0x628/0x62c` width/height from the txn |
| +0x220 … | `0x630` onward: the gamma tables, 0x1010 bytes each |

`AMDFramebuffer` passes exactly this to
`handleDisplayPipeTransaction(_AMD_PIPE_TRANSACTION *)` (`0x2b4df0`), which is
where the real DCN programming lives. Reading our snapshot against that
function's disassembly is the next static step, and it is also the answer to
§5 — the surface address the pipe wants is **in the transaction**, so the
scanout no longer has to be guessed at.

## 5. The picture, deliberately wrong (unchanged from 2wb)

Pointing the scanout at the published offset means the DCN 3.1.2
`HUBP*_DCSURF_PRIMARY_SURFACE_ADDRESS*` pair, from `docs/linux-ref/`
(`DCN_VERSION_3_1` grouping, `dcn_3_1_2` offsets — check the grouping before
adding a register). **This kext has never written to the display engine**, so
it is its own milestone with its own risk: a wrong write blanks the panel.
Do it only once the transactions are accepted, and prefer taking the address
out of `RBFBAmtrSnap` over inventing one.

## 6. Carried over, unchanged

- The **wedge**: `channel 52 GFX event timeout`, `checkGPUProgress - Signaling
  hardware error`, `no channel associated with stamp_idx N`, from ~6 s after
  the first submit failure. Consequence or independent second defect — **not
  established**.
- `IOAccelSurface::set_id_mode: Surface mode contains bad bits` x4, unexplained.
- 2sv's controlled zero for `IOAccelDisplayPipeCapabilities` against
  2u1/2u2/2w0's three non-zero reads.
- `IOAccelDisplayPipeUserClient2` counting 2 with `IONDRVFramebuffer`, 1 with ours.
- WindowServer CPU% never attributed in a controlled read.
- **Boot 191 showed the Apple logo and progress bar and never reached the
  desktop**, with the OS live underneath over SSH — the user's own report. That
  is a reading, not a wedge diagnosis: with every transaction refused, nothing
  was ever composited to the panel, which is exactly what a frozen boot screen
  looks like. If `6F1EB613` accepts transactions and the screen still never
  changes, the scanout address (§5) is the whole remaining gap.
- **~76 unpushed commits. Pushing needs asking.**
