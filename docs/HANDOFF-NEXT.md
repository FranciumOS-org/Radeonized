# HANDOFF — next step (2wf, 2026-10-28)

Live handoff. `docs/HANDOFF.md` is stale except its §7 rules and §6 procedure.
`docs/JOURNAL.md` is the evidence journal — grep it by milestone tag.

## 1. Where this stands

**The display pipe accepts transactions** (2we, boot 193). Every failure in the
chain is at zero against `display_change_handler` 26 as the control:
`set_transaction_args … 0xe00002c7` 132 → 0, `Failed to submit transaction`
134 → 0, `Fallback … submission failed` 66 → 0, `DisplayPipeState` 66 → 0.
`RBFBAmtrSets` 0 → **13**: Apple's accelerator hands cooked display-pipe
transactions to our framebuffer.

**And the transaction is now read end to end** (2wf, static). The struct is
confirmed field for field from three directions — the live pointer in boot
193's snapshot, `executeTransaction`'s `leaq` addresses, and every per-bit
handler in `AMDFramebuffer::handleDisplayPipeTransaction`:

| snapshot | pipe offset | what it is |
|---|---|---|
| +0x00 | — | flag word, `0x75`; AMD supports `0x7d`, so **all of ours** |
| +0x08 | — | u32 transaction ID |
| +0x10 | `pipe + 0x36b0` | **`_AMD_PIPE_FLIP`, 0x220 bytes — carries the GPU address** |
| +0x20 | `pipe + 0x3660` | dst rect, 4 float pairs (0x20 bytes) |
| +0x30 | `pipe + 0x3680` | colour matrix (0x30 bytes) |
| +0x38 | `pipe + 0x2650` | pipe post-gamma → `setGammaTable` |
| +0x40 | `pipe + 0x38d0` | pre-CSC linearization |

**What is not fixed: nothing is written to the display engine.** We answer the
transaction without doing the work, so the compositor believes 13 frames landed
and none did, and `-rbgpufbaty` publishes 16 MB while OTG0 scans VRAM offset 0.
The pipe is accepted; the picture is not drawn.

## 2. The next step is ONE armed boot, and the build is already made

`build/out` holds fbkext **`A454F066`** (the only thing that changed) and
plugin **`0D1F953A`** (unchanged, already installed in the EFI and proven by
boot 193 — **no plugin rebuild and no `efi.sh install`**).

Rule 15's install for this boot is the **auxiliary collection only**:

```bash
sudo rm -rf /Library/Extensions/RBAMDFramebuffer.kext
sudo cp -R build/out/RBAMDFramebuffer.kext /Library/Extensions/
sudo kmutil create -n aux -z --volume-root /
```

`kmutil create` may print `Code=34 Missing Developer Kit` over a write that
**landed** (2wa) — verify the artifact, never the exit status (rule 26).

Boot-args unchanged, `-rbgpufbaty` and `-rbgpufbamtr` both in. Arm all four
one-shots (rule 34):

```bash
sudo nvram rbgpu-arm=1 rbgpu-agdc=1 rbgpu-iop=1 rbgpu-fb=1
```

Then in the armed boot, over SSH, **before rebooting**: `sudo tools/plugin_check.sh`.

## 3. What to check, in order

1. **Arm first** (rules 8/15/34): `rbgpu-boot` must be **194**; `agdc_oneshot`
   and `iop_oneshot` both 0; `agdc_error 0`; `diag_used` against `diag_max`;
   fb UUID **`A454F066`**, plugin **`0D1F953A`**. `RBFBAmtrFlipSnapped` exists
   at all only in the new binary — the cheapest independent check (rule 15).
2. **`RBFBAmtrFlipSnapped` must be `true`.** Bit `0x1` is SET in `0x75`, so
   `txn->0x10` is valid. False, or a non-zero `RBFBAmtrFlipRefused`, means the
   flag word and the pointer disagree and 2we's decode is wrong somewhere.
3. **The three predicted fields**, which test 2wf's static read rather than
   assume it: `RBFBAmtrFlipFmt` should be one of `0/6/7/8/9`,
   `RBFBAmtrFlipRot` small, and `RBFBAmtrFlipA58` a plausible GPU VA.
   `A58` non-zero and plausible = the static read landed. `A58` zero = the
   prediction was wrong and **nothing is lost**: decode `RBFBAmtrFlipSnap`
   (0x220 bytes) and the address is somewhere in it. That is exactly why the
   blob is published beside the three fields.
4. **Decode the blob either way**, as 2we did for the 0x80 header. The
   addresses are GPU VAs (`base + offset` off a `vt[0x158]` call,
   `0xbe88846`), **not** VRAM byte offsets like `ATY,fb_offset` — keep the two
   kinds of number apart.
5. **Regression controls**, all of which were zero in 193 and must stay zero:
   `set_transaction_args … 0x2c7`, `Failed to submit transaction`,
   `Fallback … submission failed`, `DisplayPipeState`, `no gamma`. With
   `display_change_handler` 26 as the control. And `agdc_gamma_elems1` must
   still read **256**.
6. The picture is **expected to stay wrong** — nothing is written to DCN in
   this build. The screen is not the instrument; SSH and `ioreg` are.

## 4. After that: the first write to the display engine

Its own milestone, its own risk, its own boot-arg, and **not** to be bundled
with step 3. Once the address is measured, the DCN 3.1.2
`HUBP*_DCSURF_PRIMARY_SURFACE_ADDRESS*` pair is what it has to be pointed at.
Registers from `docs/linux-ref/`, `DCN_VERSION_3_1` grouping, `dcn_3_1_2`
offsets, **grouping checked before adding one**. A wrong write blanks the
panel, and rule 12 says a black panel is indistinguishable from a hang — so a
readback and a restore path come with it. The current correct scanout value is
known and measured, which is what makes the write cheap to validate.

Note 2wf's finding before planning this: `AMDFramebuffer`'s own route ends at
`ATIController` dump slot `0x8d0`, the pure-virtual stub shared by 122 slots,
and no controller instantiates here. **Even Apple's own framebuffer could not
have completed this flip on this machine** — the register write was always
ours. Do not go looking for an Apple function that will do it.

## 5. The other two open things

- **The wedge**, now established as an **independent second defect** (2we):
  boot 193 has no submit failures at all and the wedge is unchanged in shape
  and slightly larger. Stamp retirement — submitted, never retires, hang check
  says not hung, restart cannot find the channel. It has never had a milestone
  of its own.
- **Unattributed in 193**: `Setting main display 0x… offline` ×7 and
  `Setting offline display 0x… main in CGXConstructVirtualFramebuffer` ×7,
  both ERROR, absent from 192 — a normal mode-set the earlier boots never
  reached, or a new defect; **not established**. `bad bits` 4, unchanged five
  boots. `RBFBVBL1Fires` 0 with `RBFBVBLFires` 21218, and now less interesting
  since transactions arrive through `setAttribute`, not the VBL ring.

## 6. Standing

Grep `docs/JOURNAL.md` before reading any Apple function — **eight** milestones
have now re-read something already settled. ~82 unpushed commits; **pushing
needs asking**.
