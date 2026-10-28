# HANDOFF — next step (2wi, 2026-10-28)

Live handoff. `docs/HANDOFF.md` is stale except its §7 rules and §6 procedure.
`docs/JOURNAL.md` is the evidence journal — grep it by milestone tag.

## 1. THE DISPLAY WORKS

Boot 195: the HUBP write landed and **the panel showed the composited,
accelerated desktop, correctly**. Nothing but that write can change what the
panel shows, so the whole chain is proven — the setup gates, the address at
`_AMD_PIPE_FLIP +0x08`/`+0x0c`, the store reaching the chip — and **the swizzle
already agreed**, which was 2wh's one unmeasured risk. **No tiling write is
needed.** The display half of this project is solved.

**The one thing wrong with it is how often it changes: about one frame every
10 seconds.**

## 2. That rate is the TRANSACTION rate, and it was measured before the write existed

| boot | uptime at capture | `RBFBAmtrSets` | one frame every |
|---|---|---|---|
| 193 | 167 s | 13 | 12.8 s |
| 194 | 183 s | 17 | **10.8 s** |

Neither build wrote HUBP at all. The user saw ~10 s. **The write is not a
throttle** — it displays every transaction it is handed, and Apple's
accelerator hands over 0.09 per second. 2wh did not cause the rate; it made it
visible.

## 3. The next experiment: one armed boot, build is made

`build/out` holds fbkext **`452E6EE1`**; plugin **`0D1F953A`** unchanged and
already in the EFI (**no plugin rebuild, no `efi.sh install`**).

**Boot-args: remove `-rbgpuoff`, add `-rbgpufbretire`, keep `-rbgpufbhubp`
and `-rbgpufbamtr`.**

```bash
sudo tools/efi.sh bootarg remove -rbgpuoff
```

```bash
sudo tools/efi.sh bootarg add -rbgpufbretire
```

Install the framebuffer kext to the **auxiliary collection** (rule 15 — the
only artifact that changed):

```bash
sudo rm -rf /Library/Extensions/RBAMDFramebuffer.kext
sudo cp -R build/out/RBAMDFramebuffer.kext /Library/Extensions/
sudo kmutil create -n aux -z --volume-root /
```

`kmutil create` may print `Code=34` over a write that **landed** (2wa) — verify
the artifact, never the exit status.

Arm all four one-shots (rule 34):

```bash
sudo nvram rbgpu-arm=1 rbgpu-agdc=1 rbgpu-iop=1 rbgpu-fb=1
```

## 4. Getting a capture this time

Boot 195 left **no evidence at all** because the desktop was too slow to use.
Two ways round it, and both are now in place:

- **SSH in from another machine** and run `sudo tools/plugin_check.sh` there.
  The OS is live and responsive underneath; only the screen is slow.
- **Failing that, the crumb now carries the answer and survives a power
  cycle.** Build `452E6EE1` extends `rbgpu-fbcrumb`, and `setProperty` on
  `/options` does sync (2sr), unlike `removeProperty`:

```bash
nvram rbgpu-fbcrumb
```

  reads `b=<boot> s=<stage> f=<vblFires> e=<enables> r=<running> h=<hubp
  refusal, 0 = set up> w=<hubp writes> k=<1 iff the pair read back what we
  wrote> t=<amtrSets>`. **Check `b=` against `nvram rbgpu-boot` first** — rule
  11, the crumb persists and a stale one reads exactly like this boot's.

## 5. What to read, in order

1. **Arm first** (rules 8/15/34): `rbgpu-boot` must be **196**. Read the
   `rbgpu-boot` *line*, not a grep of the file — `rbgpu-diag-prev` is the
   previous boot's table and 2wg nearly lost a boot to exactly that.
   `armed=1`, one-shots 0, `agdc_gamma_elems1 = 0x100`, fb UUID `452E6EE1`.
2. **Did the write still land**: crumb `h=0`, `w>0`, `k=1`, or the registry's
   `RBFBHubpReady` / `Writes` / `Readback == Last`.
3. **The experiment**: `RBFBRetireArmed` true, **`RBFBRetireEnables` > 0**,
   and then **`RBFBVBL1Fires`**.
   - Enables > 0, fires > 0, **frame rate up** → the completion path was the
     answer and the display is done.
   - Enables > 0, **fires still 0** → our own delivery is skipping that slot;
     the bug is in `vblDeliver`, not in Apple's accelerator.
   - Fires > 0 but **no rate change** → the VBL completion path is
     **eliminated**, and the wedge is what is left.
   - Enables 0 → no transaction arrived, so read `RBFBAmtrSets` and go back to
     the submit path.
4. **The rate itself**, which is the actual measurement: `RBFBAmtrSets`
   against uptime, compared against 193's 0.078 /s and 194's 0.093 /s. **That
   number, not the screen, is the instrument** — the screen only shows it.
5. **Regression controls**, all zero in 193, with `display_change_handler` 26
   as the control in the same capture: `set_transaction_args … 0x2c7`,
   `Failed to submit transaction`, `Fallback … submission failed`,
   `DisplayPipeState`, `no gamma`. **Neither 194 nor 195 could check these.**

## 6. Known gap, worth fixing when convenient

`-rbgpufbhubp` has **no one-shot of its own** — two boot-args and nothing else
— and `rbgpu-fb` does not clear (2sq), so the write re-arms on every power
cycle. Boot 195 had to be rescued with `-rbgpuoff`. Rule 9 exists so a bad
boot cannot repeat, and the most dangerous feature in the project was built
without it. The boot-arg is the documented durable disarm and it worked, but
this should not depend on that.

## 7. The other open thing

**The wedge** (2we): submitted, never retires, hang check says not hung,
restart cannot find the channel. Established as an independent second defect
in 2we, and now a live candidate for the frame rate. It has never had a
milestone of its own. Whether it and the VBL path are one defect is **not
established** — the wedge is on GFX/SDMA channels, a different mechanism.

## 8. Standing

Grep `docs/JOURNAL.md` before reading any Apple function — eight milestones
have re-read something already settled. ~88 unpushed commits; **pushing needs
asking**.
