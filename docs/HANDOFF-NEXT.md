# HANDOFF — next step (2wh, 2026-10-28)

Live handoff. `docs/HANDOFF.md` is stale except its §7 rules and §6 procedure.
`docs/JOURNAL.md` is the evidence journal — grep it by milestone tag.

## 1. Where this stands

**The display pipe accepts transactions** (2we, boot 193) and **the surface
address is measured** (2wg, boot 194). The transaction struct is confirmed
field for field from four directions. The address the compositor renders into
is **`0xf4_1002_a000`**, an MC address 256.164 MB into the measured carve-out,
carried at `_AMD_PIPE_FLIP +0x08`/`+0x0c` as a **low/high pair in exactly the
layout DCN's register pair wants**.

**What was still missing: nothing was ever written to the display engine.**
2wh is that write, and it is built but **not yet booted**.

## 2. One armed boot. The build is made.

`build/out` holds fbkext **`D07493B1`** and plugin **`0D1F953A`** (unchanged,
already in the EFI and proven by boots 193/194 — **no plugin rebuild, no
`efi.sh install`**).

Install the framebuffer kext to the **auxiliary collection** (rule 15 — the
only artifact that changed):

```bash
sudo rm -rf /Library/Extensions/RBAMDFramebuffer.kext
sudo cp -R build/out/RBAMDFramebuffer.kext /Library/Extensions/
sudo kmutil create -n aux -z --volume-root /
```

`kmutil create` may print `Code=34 Missing Developer Kit` over a write that
**landed** (2wa) — verify the artifact, never the exit status (rule 26).

**Add one boot-arg**, `-rbgpufbhubp` (it also needs `-rbgpufbamtr`, already in):

```bash
sudo tools/efi.sh bootarg add -rbgpufbhubp
```

Arm all four one-shots (rule 34):

```bash
sudo nvram rbgpu-arm=1 rbgpu-agdc=1 rbgpu-iop=1 rbgpu-fb=1
```

**Then, in the armed boot, over SSH, BEFORE rebooting** (rule 10 — the last
two captures were each cut to 6 files by an early reboot, which is why 2wg
could check no regression control):

```bash
sudo tools/plugin_check.sh
```

## 3. What this write does, and its one risk

HUBP is DCN's display-data fetch block. The register pair is the address it
reads scanout pixels from, double-buffered and latched at the next vblank. It
changes **which bytes in VRAM reach the panel and nothing else** — no timing,
no clocks, no pipe topology, no power state. OTG keeps scanning at 143.833 Hz
either way.

**The risk is the panel, not the machine.** A wrong value shows garbage or
black until the next correct write; rule 12 says a black panel is
indistinguishable from a hang, so **keep the SSH session** and read `ioreg`.
`-rbgpufbhubp` out of boot-args reverses the whole thing, and the kext
restores the firmware's address on stop.

## 4. What to read, in order

1. **Arm first** (rules 8/15/34): `rbgpu-boot` must be **195**; read it from
   the `rbgpu-boot` line, **not** by grepping the file — `rbgpu-diag-prev` is
   the previous boot's table and 2wg nearly lost a boot to exactly that.
   `armed=1`, both one-shots 0, `agdc_gamma_elems1 = 0x100`, fb UUID
   `D07493B1`, plugin `0D1F953A`. `RBFBHubpArmed` exists only in the new
   binary.
2. **The write's own arm check, before anything else means anything**:
   `RBFBHubpReady` **true**, `RBFBHubpRefuse` **0**, `RBFBHubpOrig`
   **`0xf400000000`**. If `Ready` is false, read `Refuse` and stop: **5** = the
   DMU segment is wrong and we were never addressing DCN; **6/7/8** = the pipe
   is not what 2os measured (pitch / format / address-high); **9** = the DCN
   denylist caught us; **2/3/4** = no PCI device, map failed, BAR5 too small.
3. **Did the write reach the chip**: `RBFBHubpWrites` > 0 and
   **`RBFBHubpReadback == RBFBHubpLast`**. That is the milestone whatever the
   screen does. `RBFBHubpRejects` with `RejectAddr`/`RejectWhy` says if any
   address was refused and which was the first.
4. **What the screen did** — the one thing only the user can report:
   - **legible desktop** → the swizzle already agreed, the display half is done;
   - **regular, repeating, block-structured wrongness** → tiling mismatch; read
     `RBFBHubpTiling` and `RBFBHubpAddrCfg`, and the next step is a **tiling
     write, not an address hunt**;
   - **black or noise** with `Readback == Last` → the write landed and
     something the guards do not cover is wrong;
   - **nothing changed at all** with `Readback == Last` → DCN ignores the
     stores and wants `DCSURF_FLIP_CONTROL` / the `SURFACE_UPDATE_LOCK` dance
     this build deliberately omits. Then the address was never the missing
     piece and the lock sequence is.
   - **tearing** is expected and acceptable; we write unsynchronised to vblank.
5. **Regression controls** — all were zero in 193 and must stay zero, with
   `display_change_handler` **26** as the control in the same capture:
   `set_transaction_args … 0x2c7`, `Failed to submit transaction`,
   `Fallback … submission failed`, `DisplayPipeState`, `no gamma`. **2wg could
   check none of these** because its capture had no display log. Get the full
   capture this time.

## 5. The other two open things

- **The wedge**, established as an **independent second defect** (2we): boot
  193 had no submit failures at all and the wedge was unchanged in shape and
  slightly larger. Stamp retirement — submitted, never retires, hang check says
  not hung, restart cannot find the channel. It has never had a milestone of
  its own, and it is now the only other defect.
- **Unattributed**: `Setting main display … offline` ×7 and
  `CGXConstructVirtualFramebuffer` ×7 (new in 193), `bad bits` 4 unchanged five
  boots, `RBFBVBL1Fires` 0 — less interesting now that transactions arrive
  through `setAttribute` rather than the VBL ring.

## 6. Standing

Grep `docs/JOURNAL.md` before reading any Apple function — **eight**
milestones have re-read something already settled. ~85 unpushed commits;
**pushing needs asking**.
