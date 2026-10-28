# HANDOFF — next session

**State: the display half's defect is NAMED and a fix is built but NOT YET
BOOTED.** Read `CLAUDE.md`'s 2w9 block first; `docs/JOURNAL.md` has the full
read under the tag `2w9`.

## 1. What boot 189 settled

`kIOReturnNotReady` (140x) comes from
`IOAccelDisplayPipeUserClient2::transactionEnd` because
`IOAccelDisplayPipe::isActive()` is false, because `pipe->0x298` was never set,
because `AMDRadeonX6000_AMDAccelDisplayPipe::initFramebufferResource`
(`0xbe49356`) returns 0 silently on `pipe->0x340 == 0`.

**`pipe->0x340` and `pipe->0x34c` come from two registry properties on the
framebuffer, and `RBAMDFramebuffer` published neither:**

| property | format | consumed by | effect if absent |
|---|---|---|---|
| `ATY,fb_offset` | 8-byte `OSData`, `uint64` | `getDisplayInfo` -> `info+0x00` | `pipe->0x340 = 0` -> **fatal** |
| `ATY,fb_size` | 8-byte `OSData`, `uint64` | `getDisplayInfo` -> `info+0x0c`, **and** `isDisplayEnabled` | `pipe->0x35d = 0` -> **fatal** |

Both must be **non-zero**. `ATY,fb_offset` is a byte **offset** relative to the
VRAM base (`hw->0x370->vt[0x1e0]()` = `mem->0x50`), not an address.

Measured gate table, boot 189, all three samples identical:

| field | read | verdict |
|---|---|---|
| `accel->0xc78 & 0x2` | 1 | eliminated |
| `pipe->0x282` | 0 | eliminated |
| `pipe->0x298` | **0** | **the gate** |
| `w / h / bpp / fmt` | 1920 / 1080 / 4 / 4 | `getPixelInformation` correct |
| `fi348` | 8294400 | exact, correct |
| `fi340` / `fi34c` | **0 / 0** | the two absent properties |
| `pfb`, `hw328`, `hw330`, `vram`, `vmap` | all 1 | the pointer chain is right |

## 2. The next boot — ONE boot, builds already made

Builds: **plugin `64D3D687`**, **fbkext `59124B3A`**. Both UUIDs verified moved;
`kmutil libraries -p build/out/RembrandtGPU.kext | grep -c SystemKernelExtensions.kc`
= 0; `grep -c IOGraphicsFamily src/plugin/Info.plist` = 0.

The plugin boots **from the OpenCore EFI** — a rebuild does nothing until it is
installed (2w6 lost a boot to exactly this). The new boot-arg must be added too.

Commands for the user to run (Claude never runs these and never asks for the
password):

```
sudo tools/efi.sh install
sudo tools/efi.sh bootarg add -rbgpufbaty
sudo nvram rbgpu-arm=1 rbgpu-agdc=1 rbgpu-iop=1 rbgpu-fb=1
sudo reboot
```

Then after boot: `sudo tools/fbload.sh`, wait past **75 s** (the probe's last
sample), and `sudo tools/plugin_check.sh` over SSH **before** rebooting
(rule 10). Let it finish — boots 188's capture was cut off at six files.

**EXPECT A WRONG OR STALE PICTURE.** The offset published is 16 MB while the
display engine still scans VRAM offset 0, because the gate rejects the truthful
0. A working pipe with a wrong image is the intended result of this boot. The
screen is not the instrument; SSH and the probe are. Nothing is written to the
display engine, so DCN cannot be wedged, and removing `-rbgpufbaty` reverts it.

## 3. Reading order for that capture

1. `rbgpu-boot` must be **190** (rule 8).
2. `kexts.txt` must show **`64D3D687`** and **`59124B3A`** (rule 15).
3. `agdc_oneshot` / `iop_oneshot` must both be **0** (rule 34).
4. `diag_used` vs `diag_max` before reading any absence (rule 14).
5. `ioreg -rl -c RBAMDFramebuffer`: `RBFBAtyArmed Yes`, `RBFBAtyOffset
   16777216`, `RBFBAtySize 8294400`. If `RBFBAtyArmed` is `No` the boot-arg did
   not land and nothing else in the capture means anything.
6. Then, in this order: `ax0_b35d`, `ax0_fi340`, `ax0_fi34c`, `ax0_active`,
   `ax0_gate`, `ax0_say`.

Predictions written down before the boot:

- `ax0_b35d` **1**, `ax0_fi340` **16777216**, `ax0_fi34c` **8294400**
- `ax0_active` **1**, `ax0_gate` **0**, `ax0_say` "ALL THREE OPEN"
- `Failed to submit transaction` and `IOReturn: 0x2d8` → **0**
- `fb_vbl_ot_armed` **present for the first time** (a transaction finally queued)

Branches if it does not come out that way:

- `b35d 0` → the property did not land as `OSData`, or the arg is absent. Check 5 first.
- `b35d 1`, `fi340 16777216`, `active 0` → the remaining silent returns inside
  `initFramebufferResource` are `index != 0` and `reserveFrameBuffer` failing;
  `ax0_r380` names which.
- `active 1` but submits still fail → the defect moves past `transactionEnd` for
  the first time; capture with `--info` (rule 36) and read the new message.

## 4. If it works: the follow-on, already scoped

Point the scanout at the offset we told the accelerator about, i.e. program the
DCN 3.1.2 `HUBP*_DCSURF_PRIMARY_SURFACE_ADDRESS*` pair from
`docs/linux-ref/` (`DCN_VERSION_3_1` grouping, `dcn_3_1_2` offsets — check the
grouping before adding a register, per the workflow note). **This kext has never
written to the display engine**, so that is its own milestone with its own
risk: a wrong write blanks the panel. Do it only with the chain already proven
and with the image as the only remaining defect.

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
