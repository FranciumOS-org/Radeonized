# HANDOFF — next session

**State: the display pipe is ACTIVE and transactions flow. The blocker moved
two layers downstream.** Read `docs/RULES.md`'s 2wb block first; `docs/JOURNAL.md`
has the full read under `2wb`.

## 1. What boot 191 settled

Publishing `ATY,fb_offset` (8-byte `OSData`, non-zero) opened the whole chain:

    ax0_fi340 16777216   ax0_fi34c 8294400   ax0_r380 1
    ax0_active       1   ax0_gate        0   "ALL THREE OPEN"

`IOAccelDisplayPipe::isActive()` is true for the first time.
`Fallback ... submission failed` went **69 / 73 / 3** across boots 189/190/191,
and the 3 are all inside 90 ms at the moment the pipe went active, followed by
113 s of silence. **`fb_vbl_ot_armed` is no longer the question** — Apple's VBL
handler 1 fired (`RBFBVBL1Fires = 1`, against 0 fires over 375 s in 2w0).

## 2. The frontier

122 times, 00:24:23.734 → 00:25:25.045:

```
IOAccelDisplayPipeTransaction2::set_transaction_args(sIOAccelDisplayPipeTransactionArgs *)
    returning error 0xe00002c7 for transaction(N).
AMDRadeonAccelerator [0]: transaction ID (N) generated error (0xe00002c7).
    dirtyBits=0x3c1, options=0x801
```

`0xe00002c7` = `kIOReturnUnsupported` (`IOReturn.h:109`).

**Next step needs NO boot**: disassemble
`IOAccelDisplayPipeTransaction2::set_transaction_args` in
`build/re/IOAcceleratorFamily2` and enumerate every path returning it.
Measured inputs: `dirtyBits = 0x3c1`, `options = 0x801` (×120) and `0x800`
(×2). **Rule 13 first** — this project has already been burned by this exact
constant, since request 0x1B's *success* path returns `kIOReturnUnsupported`
(2ha). **Rule 35** — if the chain ends at our own code, instrument our end in
the same change, not a milestone later. **Rule 14** — enumerate *every* exit
before reading an absence (2w7 cost three boots to that).

## 3. The picture is still wrong on purpose

`-rbgpufbaty` publishes a 16 MB offset while OTG0 still scans VRAM offset 0, so
the compositor renders where nothing is displayed. That was deliberate and it
is now the only remaining *deliberate* defect. Fixing it means programming the
DCN 3.1.2 `HUBP*_DCSURF_PRIMARY_SURFACE_ADDRESS*` pair from
`docs/linux-ref/` (`DCN_VERSION_3_1` grouping, `dcn_3_1_2` offsets — check the
grouping before adding a register). **This kext has never written to the
display engine**, so that is its own milestone with its own risk: a wrong write
blanks the panel. Do it after `set_transaction_args`, not before — a correct
image through a pipe that rejects every transaction is worth nothing.

The alternative, cheaper, and worth considering first: find what AMD's own
driver puts in `this->0x7e0` and whether the scanout can simply stay at 0 with
a different non-zero encoding. Not investigated.

## 4. Arming, unchanged

`build/out` holds fbkext `59124B3A` and plugin `64D3D687`, both installed and
live. `-rbgpufbaty` and `-rbgpuaccelprobe` are in boot-args. Each armed boot
needs the one-shots again (rule 9/34):

```
sudo nvram rbgpu-arm=1 rbgpu-agdc=1 rbgpu-iop=1 rbgpu-fb=1
```

Reading order: `rbgpu-boot`, then `kexts.txt` **or** `RBFBAtyArmed` as the
fallback identity check, then both `*_oneshot`, then `diag_used` vs
`diag_max`, then the `ax0_*` keys. **Run `plugin_check.sh` to completion** —
boots 188 and 191 were both cut short and lost their `display-log.txt`;
boot 191's was only recoverable because the surviving evidence happened to be
ERROR-level.

## 5. Carried over, unchanged

- The **wedge**: `channel 52 GFX event timeout`, `checkGPUProgress - Signaling
  hardware error`, `no channel associated with stamp_idx N`, from ~6 s after the
  first submit failure. 41 `event timeout` lines in boot 189. Consequence or
  independent second defect — **not established**.
- `IOAccelSurface::set_id_mode: Surface mode contains bad bits` x4, unexplained.
- 2sv's controlled zero for `IOAccelDisplayPipeCapabilities` against 2u1/2u2/2w0's
  three non-zero reads.
- `IOAccelDisplayPipeUserClient2` counting 2 with `IONDRVFramebuffer`, 1 with ours.
- WindowServer CPU% never attributed in a controlled read.
- **~70 unpushed commits. Pushing needs asking.**
