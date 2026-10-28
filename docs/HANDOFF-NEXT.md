# HANDOFF — next step (2wj, 2026-10-28)

Live handoff. `docs/HANDOFF.md` is stale except its §7 rules and §6 procedure.
`docs/JOURNAL.md` is the evidence journal — grep it by milestone tag.

## 1. THE DISPLAY WORKS, reproducibly

Boots 195 and 196: the HUBP write lands and **the panel shows the composited,
accelerated desktop, correctly**. Nothing but that write can change what the
panel shows, so the whole chain is proven, and **the swizzle already agreed**
— 2wh's one unmeasured risk. **No tiling write is needed.** n=2.

**The only thing wrong is the rate: about one frame every 10 seconds.**

## 2. That rate is the TRANSACTION rate, measured before the write existed

| boot | uptime | `RBFBAmtrSets` | one frame every |
|---|---|---|---|
| 193 | 167 s | 13 | 12.8 s |
| 194 | 183 s | 17 | **10.8 s** |

Neither build wrote HUBP. **The write is not a throttle** — it displays every
transaction it is handed, and Apple's accelerator hands over 0.09 per second.
2wh did not cause the rate; it made it visible.

## 3. Current machine state, and why the next install is safe

`-rbgpuoff` is in boot-args (added from Windows), so the plugin peer does not
run and the desktop is on `IONDRVFramebuffer` — **usable**. That is the right
state to install from.

Boot 196 had to be rescued from Windows because `rbgpu-fb` never consumed.
**2wj fixes that**: the key is now overwritten with `"0"` on the `setProperty`
path 2sr measured as syncing, instead of removed on a path that reaches no
sync. **After this build a plain power cycle is a full disarm** — the panel
goes back to `IONDRVFramebuffer` and the desktop comes back usable. No Windows
trip.

## 4. One armed boot. Build is made.

fbkext **`DEB8918D`**; plugin **`0D1F953A`** unchanged and already in the EFI
(**no plugin rebuild, no `efi.sh install`**).

Install the framebuffer kext to the **auxiliary collection** (rule 15 — the
only artifact that changed):

```bash
sudo rm -rf /Library/Extensions/RBAMDFramebuffer.kext
sudo cp -R build/out/RBAMDFramebuffer.kext /Library/Extensions/
sudo kmutil create -n aux -z --volume-root /
```

`kmutil create` may print `Code=34` over a write that **landed** (2wa) — verify
the artifact, never the exit status.

Boot-args: **remove `-rbgpuoff`, add `-rbgpufbretire`**, keep `-rbgpufbhubp`
and `-rbgpufbamtr`:

```bash
sudo tools/efi.sh bootarg remove -rbgpuoff
```

```bash
sudo tools/efi.sh bootarg add -rbgpufbretire
```

Arm all four one-shots (rule 34):

```bash
sudo nvram rbgpu-arm=1 rbgpu-agdc=1 rbgpu-iop=1 rbgpu-fb=1
```

## 5. Reading it when the desktop is too slow to use

**The crumb alone now gives the frame rate, after a power cycle, with no
capture.** It carries `f=` vblFires and `t=` amtrSets, and the VBL thread_call
runs at the panel's 143.83 Hz:

```bash
nvram rbgpu-fbcrumb
```

→ `b=<boot> s=<stage> f=<vblFires> e=<enables> r=<running> h=<hubp refusal,
0 = set up> w=<hubp writes> k=<1 iff the pair read back what we wrote>
t=<amtrSets>`, and

    transactions per second = t / (f / 143.83)

**Check `b=` against `nvram rbgpu-boot` first** (rule 11) — the crumb persists
and a stale one reads exactly like this boot's. 0.09 /s means nothing changed;
anything near 1 /s or above means the experiment worked.

A real capture is still better if SSH from another machine is possible:
`sudo tools/plugin_check.sh` in the armed boot, before rebooting (rule 10).
**Neither 194, 195 nor 196 could check a single regression control.**

## 6. What the experiment decides

`-rbgpufbretire` re-enables the `'vbl '` slots Apple registered and then
disabled, once a transaction has arrived — the same condition 2s5 measured
`enableTransactionInterrupt` waiting for. The suspicion is that Apple's
accelerator never learns a vblank happened, so it never retires a transaction
and never submits the next, and **~10 s is the shape of a timeout, not of a
throttle**. `RBFBVBL1Fires` has read 0 for four milestones with transactions
in flight; boot 191 fired it **once**, the only time ever.

- `RBFBRetireEnables` > 0, `RBFBVBL1Fires` > 0, **rate up** → that was it, and
  the display is done.
- Enables > 0, **fires still 0** → our own `vblDeliver` is skipping that slot;
  the bug is ours, not Apple's.
- Fires > 0, **rate unchanged** → the VBL completion path is **eliminated**,
  and the wedge is what is left.
- Enables 0 → no transaction arrived; read `RBFBAmtrSets` and go back to the
  submit path.

## 7. The other open thing

**The wedge** (2we): submitted, never retires, hang check says not hung,
restart cannot find the channel. An independent second defect, and now a live
candidate for the frame rate. It has never had a milestone of its own.
Whether it and the VBL path are one defect is **not established** — the wedge
is on GFX/SDMA channels, a different mechanism.

## 8. Standing

Grep `docs/JOURNAL.md` before reading any Apple function — eight milestones
have re-read something already settled. ~90 unpushed commits; **pushing needs
asking**.
