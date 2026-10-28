# HANDOFF — next step (2wl, 2026-10-28)

## Where this stands

**The display half works.** Boot 196 ran a correct, fully composited,
accelerated desktop for twenty minutes: translucent dock, colour widgets,
window shadows, System Information reading `AMD Radeon 680M / 4 GB / Metal
Supported / 1920x1080 @ 144.00Hz / Main Display Yes`. The HUBP write (2wh)
drives the panel and the swizzle already agreed (2wi).

**The open defect is the transaction rate, and 2wl reframed it.** The user
reported that **typing into Spotlight makes the frames smooth while typing**,
and that **adding characters counts while deleting does not**. The chain
therefore delivers at keystroke rate on demand through the same
`setAttribute('amtr')` → `hubpProgram` path that idles at 0.09 Hz. A timeout,
a throttle and a missing vblank retirement all fail to explain that. The
question is now **why a change on screen does not produce a transaction
unless it is a keystroke**, which is upstream of everything this project has
built.

**`-rbgpufbretire` is still untested, three boots running.** It was armed and
installed in boot 196, but its two counters are registry-only and the boot was
never captured.

## The build

**fbkext `7A6A56A9`** — 2wl's instrumentation plus 2wm's one experiment.

- The crumb is **periodic**: 100, 1000, then every 10000 fires (~70 s), with
  `s=` the sample index (12 at 70 s, 29 at twenty minutes). The last crumb in
  NVRAM is the whole boot. Boot 196 left a record of its first minute because
  the old form wrote at four fixed counts.
- `p=` — the fullest 1000-fire (~7 s) window of transactions the boot ever
  saw. A total over a boot length cannot tell a burst from a trickle.
- `n=` — transactions whose flag word has bit `0x1` clear, the ones 2wh skips.
- `K=`/`D=` — the 2wk retire kick and its delivery.
- **`-rbgpufbflipall` (2wm)** — drops the flag-word bit `0x1` gate so a content
  update into an unchanged surface is written too. Your adding-vs-deleting
  asymmetry is exactly what being handed such transactions and refusing them
  would look like: adding a character forces a new surface, deleting reuses
  the one already allocated. Off unless the arg is in boot-args, and `n=`
  says in the same boot whether it had anything to do.

Plugin `0D1F953A` is unchanged; **do not rebuild or reinstall the EFI.**

## What the user does

1. Install the framebuffer kext (auxiliary collection — the only artifact that
   changed):

```bash
sudo tools/fbload.sh
```

2. Set every one-shot again (rule 34 — all four):

```bash
sudo nvram rbgpu-arm=1 rbgpu-agdc=1 rbgpu-iop=1 rbgpu-fb=1
```

3. Boot-args: keep `-rbgpufbaty -rbgpufbamtr -rbgpufbhubp -rbgpufbretire` and
   **add one**:

```bash
sudo tools/efi.sh bootarg add -rbgpufbflipall
```

4. Reboot, let it come up, **then type**: a Terminal window is usable, because
   typing is the one interaction that updates the screen smoothly. Run the
   capture **in the armed boot** (or over SSH from another machine):

```bash
sudo tools/plugin_check.sh
```

   That one capture carries `RBFBRetireKicks`, `RBFBRetireDelivered`,
   `RBFBVBL1Fires`, `RBFBAmtrSets`, `RBFBAmtrPeak`, `RBFBAmtrNoFlip` and the
   display log together, which no amount of NVRAM can. Let the boot run for
   several minutes first, and drive it a little — open a window, type, move
   the mouse — so the rate bucket has something to measure.

5. If the box cannot be driven at all, a plain power cycle is now a full
   disarm (2wj), and `nvram rbgpu-fbcrumb` afterwards still carries the whole
   boot.

## Read it in this order

1. `rbgpu-boot` — must be **197**. If it is still 196 the boot was not armed
   and nothing else means anything (rule 8).
2. `rbgpu-fbcrumb` — `b=` must be 196 (it lags the nonce by one, 2wg).
3. `p=` — **tens** → the chain runs fast on demand, the display half is
   finished, and the remaining defect is entirely upstream in what makes
   WindowServer compose. **1 or 2** → the typing smoothness did not come
   through our HUBP write, and 2wi's "nothing but the HUBP write can change
   what the panel shows" must be re-examined before anything is built on it.
4. `n=` — the 2wm discriminator, and the arg and the counter are independent,
   which is what lets one boot settle it. **0** → no bit-clear transaction
   ever arrived, `-rbgpufbflipall` is provably inert and the line is closed.
   **Large with the rate up** → that was it. **Large with the rate
   unchanged** → we were dropping frames, drawing them changes nothing, and
   bit `0x1` was right all along.
5. `K=`/`D=` — the retire kick. With a capture, read `RBFBVBL1Fires` beside
   them: fires still 0 with `D=` > 0 means our delivery reaches the slot and
   Apple does nothing with it.

## The one command that answers the next question (2wo)

Boot 197 settled the display half: every failure in that chain reads 0, the
retire kick lands (79 of 80), and `-rbgpufbflipall` is provably inert
(`AmtrNoFlip 0`). What is left is the stamp-retirement defect — GFX submits
2.40 stamps/s and retires 2.09 while the ring reads `r == w` with no VM
faults, so the chip is immediate and the driver is slow to notice.

`build/out/metal_probe` is already built and needs no GUI. In the **armed
boot**, blind-type one line (typing is the interaction that works):

```bash
cd Developer/RembrandtGPU && ./build/out/metal_probe --bench 40 > logs/metalbench.txt 2>&1
```

Then reboot and I read the file. It submits real GPU work on channels that
already time out, so it can hang the GPU — the recovery is a reboot, the same
as always, and every iteration is flushed so a hang keeps what completed.

Read it as: **wall ≈ 500 ms with gpu in microseconds** → the completion path
is the defect, proven on the rendering side with no display involvement.
**wall tracking gpu** → GPU work completes promptly and boot 197's
"same order of magnitude" was coincidence. **pipelined ≫ serial** → a
per-buffer notification cost; **pipelined ≈ serial** → a real throughput
ceiling, a different defect.

## Standing rules that bite here

- the assistant never runs `fbload.sh`, `load.sh`, `efi.sh` or `plugin_check.sh`, and
  never installs into the EFI or `/Library/Extensions`.
- Never `kmutil unload` the framebuffer kext — it panics. Reboot instead.
- `~92` commits are unpushed. Pushing needs asking.
