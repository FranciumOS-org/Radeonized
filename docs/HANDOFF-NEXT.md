# HANDOFF — next step

**State: boot 198, armed, usable, framebuffer OUT.** `rbgpu-boot` 198,
`rbgpu-arm` consumed, `rbgpu-fb` **0**, `IONDRVFramebuffer` owns the panel,
`RBAMDFramebuffer` not loaded. The desktop is software-composited and the
machine is comfortable to work on. Nothing below needs the display half.

## The frontier, in one number

**Every GPU submission waits ~175 ms before the chip starts it**, and that is
the only thing capping the frame rate. Measured on this boot with
`build/out/metal_probe`, no kext involved (2ws/2wt/2wu):

    commit -> scheduled        0.092 ms    the driver has handed it on
    commit -> GPUStart       174.855 ms    THE GPU DOES NOT START
    GPUStart -> GPUEnd         0.014 ms
    GPUEnd  -> caller return   0.042 ms    completion is instant

144 Hz needs 6.944 ms per frame. The chip does the work in **14 microseconds**
and a batch of 40 buffers drains in one tick, so **nothing needs to get
faster** — the release quantum has to go away.

Established about the quantum: one tick shared by every engine (compute
174.4, blit 173.1, render 173.8 ms) and every queue in a process; free-running
at T = 175.77 ms with lock R = 0.966 over 191 periods; load-independent; it
releases an unbounded batch at once; and **two processes get the same period at
different phase**, which rules out any chip-level cause (doorbell, MES
timeslice, clock gating would all share phase). So the hold is **per client, in
software**.

**2wq and 2wr are withdrawn** — they concluded the completion signal was the
defect, from two clocks that both sit downstream of execution.

## The boot is BUILT and ready — plugin `0EE32DFA`

`make plugin` done, checks clean: UUID moved to **0EE32DFA**, `kmutil libraries`
resolves everything with **0** from `SystemKernelExtensions.kc` (the 2oz trap),
`grep -c IOGraphicsFamily src/plugin/Info.plist` = **0**.

It adds `-rbgpuschedprobe` (2wv), observe-only, hooking
`AMDSWScheduler::submitCommandBuffer` and `AMDSWScheduler::advance()` and
publishing `rbgpu-sched`. Rules checked before writing it: **rule 1** both
defined in the kext per `nm -m`; **rule 3** neither already routed (only
`addHWChannel` is); **rule 5** the mangled length prefix is
`19submitCommandBuffer`, not 18, which a hand-written name would have got
wrong; **rule 2** both prologues are `pushq %rbp / movq %rsp,%rbp / pushq r1x`
with no relative branch in the patch window; **rule 30** `-rbgpuschedprobe`
neither prefixes nor is prefixed by any of the 74 existing tokens. **rule 31**
the hooks touch only plain statics — no allocation, no logging, and no
`rbgpu_diag_set_num`, which takes `g_diagLock` and would deadlock exactly as
2fz measured; the note is written from the phase flush instead.

`deferredCallback` is deliberately **not** hooked: it is ten bytes and its tail
`jmp` begins at offset 5, inside rule 2's window — and it tail-jumps into
`advance()`, so it is already counted there.

### Commands (the user's own steps)

    sudo tools/efi.sh install
    sudo tools/efi.sh bootarg add -rbgpuschedprobe
    sudo nvram rbgpu-arm=1
    # reboot, then EARLY in the boot:
    env -u DYLD_INSERT_LIBRARIES ./build/out/metal_probe --bench 120
    sudo tools/plugin_check.sh
    nvram rbgpu-sched

**Leave `rbgpu-fb` at 0 and `-rbgpuownfb` OUT of boot-args** (rule 38). The
flush runs at every phase (+12 s, +100 s, +160 s), so the later notes include
whatever the probe did; `--bench 120` takes about 21 s at the current rate.

**Read `rbgpu-boot` first** (rule 8) and check `rbgpu-arm` was consumed
(rule 34) before interpreting anything.

## What the counters mean

Instrument the three functions the shape points at and publish counters. All
addresses are in `build/re/AMDRadeonX6000`, verified by `nm -m`:

| function | address |
|---|---|
| `AMDSWScheduler::submitCommandBuffer(unsigned long, AMD_SUBMIT_COMMAND_BUFFER_INFO*, AMD_BARRIER_INFO*)` | `0xbe942d0` |
| `AMDSWScheduler::advance()` | `0xbe950e0` |
| `AMDSWScheduler::deferredCallback(OSObject*, IOInterruptEventSource*, int)` | `0xbe93e18` |

`advance()` walks every `HwChannelCtx` (0x98 stride, count at `+0x60`) and
pushes the ones marked at `+0x71` through `advance(HwChannelCtx&)`
(`0xbe9442a`). Its **only two reachers** are `deferredCallback` and
`timerCallback` (`0xbe9513c`) — and `timerCallback` is **referenced nowhere
else in the kext**, so that timer is never installed.

Publish per second: a call count for each of the three, and the delta from a
`submitCommandBuffer` to the next `advance()`.

**Say the outcome before the boot.**
- `advance()` at **~5.7/s** while `submitCommandBuffer` is far higher → the
  release is `advance()`, and the fix is to drive it from the submit path.
- `advance()` tracking `submitCommandBuffer` **one for one** → the hold is
  below it, and `advance(HwChannelCtx&)`'s own gates are next:
  `AMDUserQueue::isAttached` (`0xbe94730`) then `userQueueSwitch`
  (`0xbe94762`).

Rules that apply: **rule 15** — the plugin comes from the OpenCore EFI, so this
needs `sudo tools/efi.sh install`, and check the loaded UUID. **Rule 34** — arm
`rbgpu-arm` and say so. **Rule 38** — leave `rbgpu-fb` at **0** and
`-rbgpuownfb` out of boot-args; the framebuffer takes the panel and writes HUBP
only from `setAttribute('amtr')`, so arming it here would freeze the screen
with nothing to show. **Rule 31** — publish to the registry, `IOLog` is
invisible from the fbkext and the counters must be readable with `ioreg`.

## The second defect, free to reproduce

`./build/out/metal_probe --queues 4` wedges the GPU in about six seconds, with
no kext, no arming and no reboot (2wt). K<=3 is normal; K>=4 costs 6.1-10.2 s
and logs 2we's signature — `event timeout`, `checkGPUProgress`, `not hung`,
`no channel associated`, `GPURestart`. The channels name themselves:
`ComputeUQ2`, `ComputeUQ3`, `ComputeUQ6`, `SDMA0`, each with one pending stamp.
A Metal command queue becomes a **compute user queue** and the later ones never
retire. Same shape as the established **one SDMA instance where Apple's Navi 21
path builds four** (2gc/2gi).

This is independent of the tick (K=1..3 wedge nothing and still wait 175 ms).
Chase it *after* the tick — the tick is what sets the ceiling.

## The instrument

`make metal_probe`, then with `env -u DYLD_INSERT_LIBRARIES`:

    --bench [N]     serial + pipelined, with the full split columns
    --phase [N]     wait d after each completion; reads the period off the sawtooth
    --lattice [N]   fit the period and the absolute phase (comparable across processes)
    --busy [S]      keep work in flight from another process
    --heavy [N]     scale real GPU time, the control that validates the timestamps
    --queues [K]    K command queues in one process; K>=4 wedges
    --engines [N]   compute vs blit vs render

Captures: `logs/metalphase.txt`, `logs/metallattice.txt`, `logs/metalsplit.txt`,
`logs/metalpipe.txt`, `logs/metalsched.txt`, `logs/metalengines.txt`.

## Housekeeping

**121 commits are unpushed.** Pushing needs asking (docs/RULES.md).
