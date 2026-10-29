# HANDOFF — next step

**The machine has done 632 buffers/s.** Boot 199 measured **1.582 ms** per
serial command buffer (632/s, 2377/s pipelined) inside its first two minutes,
with `rbgpu-fb=0` so the framebuffer was not involved. 144 Hz needs 6.944 ms.
**The hardware is 4.4x faster than the target already** — so nothing has to get
faster, something has to stop degrading it.

## Three regimes, one binary

    1.582 ms   632 buffers/s    boot 199, before its first channel timeout
    ~180 ms    5.4-5.7/s        boots 198, 199 late, 200 late
    3.7-11 s   0.1-0.3/s        boot 200 while wedged

**Wedge churn explains the last two**, measured both ways inside boot 200
alone: continuous churn from +20 s to +460 s while it ran at seconds per
buffer (82 `event timeout`, 503 `GPURestart` against a 1040-line control), and
**zero** of those in a 3-minute window once it had settled to 184 ms. Boot 199
is the same from the other side — churn ran for two minutes, stopped, and the
rate settled at 175.7 ms.

**The hypothesis**: the GPU is healthy until the first wedge and degraded
afterwards, permanently, even once the churn stops. 1.582 ms is the only
reading ever taken before a boot's first channel timeout, and that timeout
lands at **+19-20 s** in both boots that logged one. If it holds, the wedge is
not a second defect beside a quantum — **it is the whole defect**, and the
"175 ms tick" is its aftermath.

**NOT established.** n=1 for the fast reading, and its exact timestamp was
never recorded. 2ws's characterisation of the slow state (the sawtooth,
T = 175.77 ms at R = 0.966, per-client phase) stands as a description and is
**unexplained** by this hypothesis, which says nothing about why a degraded
state should be periodic at all.

Dead ends, all measured, do not revisit: `advance()` as the gate (runs 35-290/s
against 0.1-5.7 buffers/s completing); a queue-slot lottery (`--perqueue` holds
five queues and times each separately — 186.1/190.0/186.6/188.6/188.0 ms,
ratio 1.0x); load in either direction (saturating, keep-alive, and concurrent
clients all leave the tick intact); inducing the wedge deliberately while
already at 180 ms (180.0/178.7/179.6 ms).

## Do this next — one boot, plugin `F991B506`

Userspace cannot test the hypothesis: `tools/ratesweep.sh` could not start
before **+117 s** because no terminal exists that early, and that is exactly
the window in question. So the instrument is now in the kext.

`rbgpu_rate_start_2wx()` samples `advance()` and `submitCommandBuffer` per
**10 s window** and writes the series to **`rbgpu-rate`** after each window,
for eighteen windows = the first 180 s, then stops so nothing later overwrites
the early window. `advance()` ran ~290/s in boot 199's fast window and ~35/s
in boot 200's slow one — an 8x split over the same phase window in both — so it
discriminates the regimes from inside the kernel.

### Commands (the user's own steps)

    sudo tools/efi.sh install
    sudo nvram rbgpu-arm=1
    # reboot, then whenever convenient:
    nvram rbgpu-rate
    nvram rbgpu-sched

`-rbgpuschedprobe` is **already in boot-args** and 2wx shares it, so no
boot-arg change. Leave `rbgpu-fb` at 0 (rule 38). Nothing needs running early —
that is the point of moving the sampler into the kext.

Then, to place the first wedge against the series:

    /usr/bin/log show --last 20m --info --predicate 'senderImagePath CONTAINS "AMDRadeonX6000" OR senderImagePath CONTAINS "IOAcceleratorFamily2"' | grep "event timeout" | head -3

### Said before the boot

- `a=` high (order 2000-3000 per window) in the first window or two, then
  falling ~8x, **with the first `event timeout` at that same boundary** →
  confirmed: the healthy state exists and the wedge ends it.
- `a=` flat and low from window 0 → boot 199's 1.582 ms was something else and
  "before the first timeout" is not the distinction.
- `a=` high throughout while the rate is still slow → `advance()` is not a
  proxy either and the series is worth nothing.

Rules in force: **8** read `rbgpu-boot` first; **15** the plugin comes from the
OpenCore EFI, check the loaded UUID is `F991B506`; **34** `rbgpu-arm` is the
one-shot this needs, verify it was consumed; **38** `rbgpu-fb` stays 0.

## The instrument

`make metal_probe`, run everything with `env -u DYLD_INSERT_LIBRARIES`:

    --bench [N]     serial + pipelined with the full split columns
    --perqueue [K]  K queues held at once, each timed separately
    --queues [K]    K>=4 reproduces the wedge in ~6 s, no kext, no reboot
    --phase [N]     breaks the phase lock; reads the period off the sawtooth
    --lattice [N]   fits period and absolute phase, comparable across processes
    --busy [S] --depth [D] --heavy [H]   a load generator that can saturate
    --engines [N]   compute vs blit vs render

`sh tools/ratesweep.sh [secs] [interval]` samples the rate against
seconds-since-boot. Reads only.

## Housekeeping

Commits since the last push are local. **Pushing needs asking** (docs/RULES.md).
