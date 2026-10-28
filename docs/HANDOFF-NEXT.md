# HANDOFF-NEXT — the live handoff

Last updated **2026-10-27**, after milestone **2w5** (boot 186).
Read this first, then `docs/RULES.md`'s rules, then grep `docs/JOURNAL.md` by tag.

---

## 1. Where the display half stands

The rendering half works. The display half composites in software, and the
chain is now traced from WindowServer all the way into Apple's kernel driver.
**Boot 185 named the failure.**

    WindowServer
      -> CoreDisplay::DisplayPipe::Commit
      -> RunGPUDisplayPipe (the GPU FALLBACK path)
      -> BuildTransactions
      -> IOPTransaction::Submit            0x7ff8040c8a56
         -> IOPresentmentBegin             SUCCEEDS (2w2, confirmed 2w3)
         -> CreateResource / BindIOSurfaceToResourcePlane / SetPlaneState
         -> IOPresentmentEnd               0x7ff812e89211  <-- FAILS
            -> (trampoline: obj->0xa0, vtable slot 0x80)
            -> IOAccelDisplayPipeUserClient2::transactionEnd
                                           IOAcceleratorFamily2 0x14e64620
               returns kIOReturnNotReady (0x2d8), 118 times

`Submit` logs that at **`[INFO]`** level, which `plugin_check.sh` dropped for
eight boots (2w2, rule 36). With `--info` it reads:

    [INFO] - Failed to submit transaction:
            IOPRESENTMENT_METHOD_FAILURE: IOReturn: 0x2d8

## 2. The three gates in `transactionEnd`

| gate | meaning | only writer | status |
|---|---|---|---|
| `accel->0xc78 & 0x2` clear | accelerator not enabled | `IOGraphicsAccelerator2::enableAccelerator()` | **THE GATE, by elimination** |
| `pipe->0x282 != 0` | WSAA defer in progress | `wsaaWill/DidEnter/ExitDefer` | **ELIMINATED (2w4)** |
| `pipe->0x298 == 0` | `isActive()` false | `init_framebuffer_resource` | **ELIMINATED (2w4)** |

**Defer (2w4):** `wsaaDidExitDefer` (`0x14e5dfe6`) zeroes `0x282`
unconditionally and returns true. Boot 185's last `kIOFBNotifyWSAADidExitDefer`
(event 93) was at **18:56:05.543**; the submit failures run
18:56:05.516–18:56:07 **and again at 18:56:39 and 18:56:55**.

**isActive (2w4):** `init_framebuffer_resource` (`0x14e5d7ca`) is called
**unconditionally** by `IOAccelDisplayPipe::displayModeDidChange`
(`0x14e5db14`), the only site in the kext that registers `'vbl '` — which
boot 183 measured on our framebuffer. All three of its failure exits log at
os_log type 0x11 = FAULT, and against a genuine control (213
IOAcceleratorFamily2 FAULTs from the same thread, recovered by capturing by
**sender** — 2w3's control was invalid) not one appears.

**So the enable bit is the gate.** It is skipped, while the per-pipe loop
still runs, when `accel->0x180 == 0`. The vendor gate above it is
`AMDRadeonX6000_AMDAccelDisplayMachine::displayModeDidChange()` (`0xbe48428`,
call offset 0x870 = dump 0x880, rule 7), which returns
`set_display_mode_and_vram()`; it returned true at least once, since the pipe
loop ran.

Why the enable bit is the suspect: `IOGraphicsAccelerator2::start` clears it,
`enableAccelerator()` is its sole setter, and that is called only from
`IOAccelDisplayMachine::display_mode_did_change` (`0x14e2148e`), which
**skips the enable but still runs the per-pipe `displayModeDidChange` loop**
when `accel->0x180 == 0`. That single branch explains every measurement at
once: VBL registered, framebuffer resource inited, accelerator never
enabled, NotReady 118 times.

`accel->0x180` is the **VRAM descriptor**, and it comes from our framebuffer:
`find_vram_descriptor()` (`0x14e53922`) calls `fb->vt[0x9b0]()` =
`IOFramebuffer::getVRAMRange()` (2pq's slot), stores it at `0x180`, maps it
(`vt[0x200](1)` -> `0x188`, `vt[0x118]()` -> `0x190`), and **on a failed map
releases it and writes `0x180 = 0`**.
`RBAMDFramebuffer::getVRAMRange()` (`src/fbkext/RBAMDFramebuffer.cpp:437`)
returns the retained aperture, so the suspect is the **map**, or
`found_framebuffer` never running for our framebuffer.

## 3. What is eliminated — do not spend a boot on these

- **Every AGDC capability type** (1/4/0x8000 by 2sd, 2 by 2v6, 0x10 by 2w1).
  2w3 has IOPresentment saying *"AGDC CSC / Gamma / Linearization / Plane
  Scaler / Cursor successful"* three times each, in its own words.
- **The plane count** — `IOPresentmentBegin` never fails (2w2, 2w3).
- **System Gamma `Unknown`** — enum 0, never ours to write (2v8).
- **`Clean` never appearing** — the printer cannot print it (2v8).
- **The VBL half** — 144 Hz, right handler, right registration (2w0).
- **The transactions-off route** — there isn't one (2s3).
- **Page flipping** — never called in a whole boot (2pf).

## 4. The next experiment: ONE armed boot, build `D69D9B33`

A read, not a change. **Boot 186 already tried it and the probe never ran** —
2w3 had put its call next to `rbgpu_heartbeat_start()`, which sits behind
`-rbgpubeat` (absent since 2fg), so build `D8B1115C` produced not one `ax*`
key. The call now lives in the peer's `start()` and is gated only by its own
arg (2w5, build `D69D9B33`). The boot-arg is already in `config.plist`.

`-rbgpuaccelprobe` schedules three read-only samples at
25 / 45 / 75 s that find the accelerator by class, verify
`getMetaClass()->getClassSize()` covers the offset before touching it
(rule 19 — and refuse, publishing what was refused), and publish:

    ax<i>_gate      0 all three open, 1 accel not enabled, 2 defer, 3 not active
    ax<i>_enabled   accel->0xc78 bit 1
    ax<i>_active    pipe->0x298
    ax<i>_defer     pipe->0x282
    ax<i>_vram      accel->0x180 non-NULL     <-- the enable gate's own input
    ax<i>_vmap      accel->0x188 non-NULL
    ax<i>_fbs       machine->0x108
    ax<i>_say       the sentence (allowed to fall off the 512-byte note)

The numeric keys are first in `kRbgpuNvramHotKeys` because the boot wedges.

### Arm it (the user runs these; the assistant never does)

The boot-arg is already in `config.plist` (boot 186 had it). For the boot
itself — **all four one-shots** (rule 34):

```bash
sudo nvram rbgpu-arm=1 rbgpu-agdc=1 rbgpu-iop=1 rbgpu-fb=1
```

```bash
sudo reboot
```

After boot: `sudo tools/fbload.sh`, wait past 75 s, then **over SSH, before
rebooting** (rule 10):

```bash
sudo tools/plugin_check.sh
```

### Reading it

1. `rbgpu-boot` must be **187** (rule 8).
2. `agdc_oneshot`, `iop_oneshot` must be **0**, not 2 (rule 34).
2b. **`ax_say` must exist at all** — if it is absent the probe did not run
    again and nothing else in step 4 means anything (2w5).
3. `diag_used` vs `diag_max` (rule 14).
4. `ax0_gate` / `ax2_gate`. If **1**, read `ax0_vram`: zero means
   `getVRAMRange()`'s descriptor never survived its map, and the fix is in
   `src/fbkext/RBAMDFramebuffer.cpp`; non-zero means `display_mode_did_change`
   never reached the enable for the other reason (`this->0x10c` nesting or
   `vt[0x870]`).
5. `display-log.txt` must still show non-zero `[INFO]` **and** non-zero
   kernel lines, with `display_change_handler` non-zero as the kernel-half
   control (rule 36 — the summary prints a WARNING for either).

## 4b. The wedge is named (2w4) — a SECOND, separate defect

From **18:56:11.410**, six seconds after the first submit failure, repeating
every ~5–6 s to 18:56:50:

    channel 52 GFX event timeout;        pending stamps [1..289]
    channel 12 SDMA0_PAGE event timeout; pending stamps [1..191]
    channel 53 SDMA0_PAGE event timeout; pending stamps [1..184]
    channel N is not hung, keep running                                  x18
    checkGPUProgress - Signaling hardware error on channel 12/25/52/53   x22
    signalHardwareError: GPURestartSignaled / GPURestartEnqueued    x22 each
    hardwareErrorEvent: setting restart type to 2                        x22
    restart_channel: GPURestartSkipped / no channel associated with
                     stamp_idx N (type 2)                                x18

Work is submitted and **never retires**; AMD's own hang check answers *"not
hung, keep running"*; the restart path cannot find the channel for the stamp
and skips. A **stamp-retirement** failure, beginning **after** the display
failures. Whether it is their consequence or an independent second defect is
**not established** — do not assume either.

Also new and unexplained: **`IOAccelSurface::set_id_mode(uint32_t, uint32_t):
Surface mode contains bad bits`**, x4 at 18:56:05.034, ~0.5 s before the
first submit failure.

`tools/plugin_check.sh` now captures all of this by default (sender clauses
for `IOAcceleratorFamily2` and `AMDRadeonX6000`, plus a kernel-line count and
a `display_change_handler` control in the summary).

## 5. Still open, carried

- **The wedge** is now named (section 4b) but not explained: why stamps stop
  retiring, and why `restart_channel` cannot find the channel. 177-182 and
  184-185 all wedged with the AGDC bridge armed; boot 183 with it unarmed did
  not. n=1 differential.
- `pipe->0x290`, the hardware-pipe-vs-fallback choice, is a
  transaction-carried `std::optional` (`txn+0x180`/`+0x181`) — CoreDisplay's
  own, not ours (2w1).
- `fb_vbl_ot_armed` still absent: no transaction has ever been queued.
- The `je` at `0x7ff804117ff7` into the fallback cannot fire; which of
  `0x290`'s conditions takes the `jmp` at `0x11812b` is unestablished.
- WindowServer at 200.2 % (boot 183) vs 2su's 0.0 % idle — recorded, **not**
  attributed.
- 2sv's controlled zero for `IOAccelDisplayPipeCapabilities` against
  2u1/2u2/2w0's three non-zero reads.
- `IOAccelDisplayPipeUserClient2` counting 2 with `IONDRVFramebuffer` and 1
  with ours.
- ~62 unpushed commits. **Pushing needs asking.**
