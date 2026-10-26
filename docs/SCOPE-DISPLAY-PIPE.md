# Scope: making a display-pipe transaction complete

Written 2026-10-26 (2s4), from static reads only — no boot was spent. It
answers the question "what exactly would DCN 3.1.2 mean, how big is it, and
what is the minimum path to a completed flip?" **The headline is that the
question was wrong**, and the measured answer is much smaller.

## 1. What has to happen, restated precisely

From 2s2 and 2s3, the requirement is **not** "implement
`IOPresentmentBindIOAccelSurfaceToResource` / `SetPlaneState`" and **not**
"advertise a capability". It is exactly one thing:

> A queued `IOAccelDisplayPipeTransaction2` must be **retired**, so the
> 4-deep ring in `IOAccelDisplayPipe` drains and
> `wait_for_queue_slot_gated`'s `commandSleep(event, THREAD_UNINT)` returns.

2s3 closed the only alternative: both `DisplayPipeSupported` and
`TransactionsSupported` must be non-zero or `IOPresentmentCreateForRegistryID`
fails outright, so there is no "pipe without transactions" mode to opt into.

## 2. The retirement path, measured end to end

Ring indices on `IOAccelDisplayPipe`: **`0x238` consumed, `0x23c` produced**,
difference of 4 = full (2s2).

- **Producer**, one site: `IOAccelDisplayPipe::transaction_queue_gated(
  IOAccelDisplayPipeTransaction2 *)` increments `0x23c` at `0x14e604ee`.
- **Consumer**, two sites: **`IOAccelDisplayPipe::event_interrupt_gated()`**
  increments `0x238` at `0x14e5ff06` and `0x14e5ff60`.

So **the ring is drained by an interrupt**, and nothing drains it in software.
The chain into it, each link read:

```
IOFramebuffer 'vbl ' interrupt
  -> IOAccelDisplayPipe::signalVBLInterrupt(void *)            0x14e5dcb8
       touches this+0x268, then: if (this->0x281) ...
  -> IOAccelDisplayPipe::signalTransactionInterrupt(void *)    0x14e5e0ec
       if (this->0x2a4 == 0) return;  stamps 0x2d0; calls through this->0xd0
  -> IOAccelDisplayPipe::transaction_interrupt_gated()         0x14e60184
       jmp ...
  -> IOAccelDisplayPipe::event_interrupt_gated()               0x14e5fc22
       increments 0x238  == THE RING DRAINS
```

`this->0x281` is set to 1 by `IOAccelDisplayPipe::enableTransactionInterrupt()`
(`0x14e609ba`, which also calls `this->vt[0x878]()`), so the transaction
interrupt must be enabled for the vblank to drive retirement.

### And the interrupt source is the framebuffer's vblank

`IOAccelDisplayPipe::displayModeDidChange()` (`0x14e5db14`):

```
if (this->0xd8 == NULL) {
    fb = this->0x98;
    ret = fb->vt[0xad0]( 0x76626c20 /* 'vbl ' */, signalVBLInterrupt,
                         this, 0, &this->0xd8 );
    if (ret == 0) fb->vt[0xae0]( this->0xd8, 0 );
}
```

`0x76626c20` is `'vbl '`. The five-argument shape (fourcc, proc, target, ref,
out-ref) is `IOFramebuffer::registerForInterruptType`, and the call
immediately after it on the stored ref is `setInterruptState(ref, 0)`.

**Not asserted from a slot table:** the attempt to name slots `0xad0`/`0xae0`
from the extracted `IOGraphicsFamily` landed past its symbol coverage and
returned nonsense, so it is excluded as evidence. The identification rests on
(a) the `'vbl '` fourcc, (b) the argument shape, (c) the immediately following
two-argument call on the out-ref, and (d) **boot 138's own measurement**: our
`-rbgpufbvbl` hook on `IONDRVFramebuffer::registerForInterruptType` captured a
`'vbl '` proc (`fb_vbl_bound 1`) and macOS then called
`setInterruptState(ourRef, 0)` (`fb_vbl_state 0`) — the exact pairing this code
performs. Slot `0x9b0` of the same vtable is already pinned to
`getVRAMRange` by 2pq/boot 144, so slot numbers here are meaningful.

## 3. The vendor side already exists, and that is the finding

`AMDRadeonX6000` contains **73** `IOAccelDisplayPipe` symbol references and a
complete vendor subclass:

- `AMDRadeonX6000_AMDAccelDisplayPipe::init(IOGraphicsAccelerator2 *,
  IOAccelDisplayMachine *, **IOFramebuffer ***, unsigned int)`
- `submitTransaction`, `executeTransaction`, `performTransaction`,
  `validateTransaction`, **`isTransactionComplete`**
- `initializeTransaction`, `uninitializeTransaction`,
  `initFramebufferResource`, `initAffineTransform`
- plus `AMDRadeonX6000_AMDHWDisplay::getDisplayPipeTransactionFlip(unsigned,
  IOAccelResource2 *, IOAccelResource2 *, _eAMDSwapInterval, _AMD_PIPE_FLIP *,
  bool, AMDAccelDisplayPipeTransactionPipeRotation, ...)`

**So Apple's AMD driver already implements the whole vendor transaction and
flip protocol, in the kext this project already makes run.** Nothing here has
to be written from Linux's `dcn31`. `getDisplayPipeTransactionFlip` has **no
caller inside AMDRadeonX6000** — it is invoked from IOAcceleratorFamily2
through a virtual slot, which is why 2pf had to route it to observe it and why
a direct-branch search finds nothing.

And 2pq already measured the register question: a flip touches **only OTG
(0 of 434 registers moved) and HUBP (0 of 80 moved)** between dcn302 and
dcn312 — **bit-identical**. DCHUBBUB's 74/77 differences are a *modeset*
requirement, and the firmware already did the modeset. So if Apple's existing
flip code runs, the registers it writes are correct for this chip.

## 4. Minimum path, in increasing cost

**Tier 1 — deliver the vblank we already capture. No DCN write. Hypothesis,
cheap, falsifiable.**
Boot 138 measured `fb_vbl_bound 1` with **`fb_vbl_sent` absent**: our hook
captured a `'vbl '` proc and delivered it **zero** times, serving all 23 553
frames from `vblp_ticks` (a direct `handleVBL`) instead. If the proc we
captured is `IOAccelDisplayPipe::signalVBLInterrupt`, then transaction
retirement has been starved **by our own hook** since 2oz. The instrument for
this already exists (`fb_vbl_sent`), the change is to call the proc, and no
DCN register is touched. **NOT established** — the captured proc's identity was
never recorded, and `setInterruptState(ourRef, 0)` suggests macOS disabled it
immediately, which has to be handled.
*First step is a read, not a boot*: log the captured proc's address and compare
it against `IOAccelDisplayPipe::signalVBLInterrupt`'s slid address.

**Tier 2 — get `AMDRadeonX6000_AMDAccelDisplayPipe` instantiated.**
Its `init` takes an `IOFramebuffer *`, `IOAccelDisplayMachine::found_framebuffer`
filters nothing (2pp), and our `IONDRVFramebuffer` already carries
`IOAccelIndex`/`IOAccelRevision`/`IOAccelTypes`. What creates it, and whether
it is created for our framebuffer, is **not yet measured** — that is the next
read, and it is a read, not a boot.

**Tier 3 — DCN 3.1.2 from Linux's `dcn31`.**
Only needed if Tier 2 shows Apple's pipe cannot bind to a non-AMD
framebuffer at all. Even then 2pq's register finding means the *flip* is not
the hard part; a modeset would be, and the firmware has already done one.

## 5. What this does NOT claim

- No boot has tested any of this. Every tier above is a static read.
- The live-instance check was attempted and **discarded**: `ioreg -c <class>
  -r -d1 | grep -c '+-o'` returned 0 for the accelerator that is demonstrably
  present, and 1 for an `AppleGraphicsDeviceControl` another form counted as 2.
  The instrument is unreliable for counting; nothing was concluded from it.
- Whether `displayModeDidChange` ever runs here, and whether `0x281`/`0x2a4`
  are set, is unmeasured.
- The default configuration still must not open the AGDC gate (2rz): boot 164
  showed that reaching this code path without a completion source wedges the
  machine.
