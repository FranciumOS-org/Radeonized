# HANDOFF — next session

**State: the display pipe is ACTIVE, transactions flow, and the one thing that
refused all 122 of them is now understood and fixed in our own kext.** Read
`docs/RULES.md`'s **2wc** block first, then 2wb; `docs/JOURNAL.md` has the full
reads under those tags.

## 1. The chain, now closed end to end

Boot 191 (2wb): publishing `ATY,fb_offset` as a non-zero 8-byte `OSData` made
`IOAccelDisplayPipe::isActive()` true for the first time in this project, and
transactions were created, numbered and handed to the kernel —
`ax0_fi340 16777216`, `ax0_r380 1`, `ax0_active 1`, `ax0_gate 0`, and
`Fallback ... submission failed` **69 / 73 / 3** across boots 189/190/191.

2wc (static, no boot): every one of the 122 transactions was then refused by
**`AMDRadeonX6000_AMDAccelDisplayPipe::executeTransaction`'s first test**:

    al = pipe->0x330->vt[0x190](getFramebufferIndex())
    if (!txn || !al) return 0xe00002c2 + 5*(txn != NULL)      /* == 0x2c7 */

`vt[0x190]` (dump `0x1a0`, rule 7) reads bit 8 of a per-display dword, and the
**sole writer** is `AMDHWDisplay::getDisplayInfo` at `0xbe87165`:

    r = fb->getAttribute('amtr', &out)      /* call 0xa30 -> dump 0xa40 */
    if (r == 0 || r == kIOReturnNotReady) dstate |= 0x100; else &= ~0x100

`RBAMDFramebuffer` never handled `'amtr'`, so `IOFramebuffer`'s base answered
`kIOReturnUnsupported` and the bit was clear on every call. **One selector, 122
refusals, no variance.** The gamma ceiling in the same function is excluded by
the captures (256-entry LUT in 71 of 73 dumps, 1024 in 2).

## 2. The experiment to run: ONE armed boot of fbkext `6F1EB613`

`build/out` holds fbkext **`6F1EB613`** (new) and plugin **`64D3D687`**
(unchanged, already installed in the EFI).

**Rule 15 — the framebuffer that owns the panel boots from the AUXILIARY
COLLECTION, and a rebuild refreshes nothing.** Boot 190 was lost to exactly
this omission. The install is the user's step:

```bash
sudo rm -rf /Library/Extensions/RBAMDFramebuffer.kext
sudo cp -R build/out/RBAMDFramebuffer.kext /Library/Extensions/
sudo chown -R root:wheel /Library/Extensions/RBAMDFramebuffer.kext
sudo kmutil create -n aux -z --volume-root /
```

`kmutil create` may print `Code=34 "Missing Developer Kit"` **and still have
rebuilt the collection** (2wa, rule 26's mirror). **Verify the artifact, never
the exit status**: search the AuxKC for the new UUID's raw bytes with controls,
or just check `RBFBAmtrArmed` exists in the boot's `framebuffer.plist`.

Boot-arg to add (the only new one):

```bash
sudo tools/efi.sh bootarg add -rbgpufbamtr
```

`-rbgpufbaty` and `-rbgpuaccelprobe` are already in boot-args and must stay.
Then, every armed boot (rule 9/34 — all four, every time):

```bash
sudo nvram rbgpu-arm=1 rbgpu-agdc=1 rbgpu-iop=1 rbgpu-fb=1
```

**Run `sudo tools/plugin_check.sh` to completion** before rebooting. Boots 188
and 191 were both cut short and lost their `display-log.txt`; 191's evidence
survived only because it happened to be ERROR-level, and INFO does not survive
a reboot (rule 36).

## 3. What to read, in order

1. `rbgpu-boot` — must have increased (rule 8).
2. Identity for **both** kexts (rule 15): `kexts.txt`, or the fallback
   `RBFBAmtrArmed` / `RBFBAtyArmed` in `framebuffer.plist` — both are published
   *unconditionally* by the new code, so either one present proves `6F1EB613`.
3. All four `*_oneshot` keys (rule 34), then `diag_used` vs `diag_max` (rule 14).
4. The new keys from `ioreg -rl -c RBAMDFramebuffer`:
   `RBFBAmtrGets`, `RBFBAmtrSets`, `RBFBAmtrSnapped`, `RBFBAmtrPtr`,
   `RBFBAmtrRefused`, `RBFBAmtrSnap` (0x80 raw bytes).
5. The kernel half of the log **by sender** (rule 36), counting
   `set_transaction_args` and `display_change_handler` as the control.

**Stated before the boot:**

- Expect `RBFBAmtrGets` > 0, `RBFBAmtrSets` > 0, `RBFBAmtrSnapped` true, and
  the `set_transaction_args ... 0xe00002c7` line **gone**.
- Gets move, sets do not → validation passed and the **submit** path is gated
  somewhere else; `executeTransaction(txn, true)` is the arm to read, since
  everything gated on its `bool` was skipped in the validate path.
- Neither moves → `getDisplayInfo` is not reaching us, and the chain above the
  attribute is wrong. Do not reason past that; read it.
- **The picture will still be wrong.** We answer the transaction, we do not
  execute it, and `-rbgpufbaty` still publishes a 16 MB offset while OTG0
  scans VRAM offset 0.

## 4. Then: `RBFBAmtrSnap` is the prize

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
- **~72 unpushed commits. Pushing needs asking.**
