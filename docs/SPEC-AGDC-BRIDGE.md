# Per-GPU AGDC bridge — 2ra

Built 2026-10-25. **Boot 155 refused before registration; fixed in 2rb.**
The four-argument Lilu symbol solver treated kernel lookups as range [0,0),
so the four kernel functions all resolved to zero. The new lookup uses the
two-argument resolver, records each raw result, and applies explicit bounds
only to kext symbols. Its regression test fails with the old overload.
Boot 156 verified the corrected build's publication and user-client queries,
but the panel froze at the next protocol step. The bridge supplies the missing
service; it does not implement DCN mode setting.

Before the bridge, `AppleGPUWrangler` was the only matching AGDC service,
and IOPresentment explicitly skips it. The new **RembrandtGPUAGDC** service
attaches to the real 1002:1681 PCI device and reports the existing
`IONDRVFramebuffer` as its one framebuffer. It leaves that framebuffer in
control of the panel.

## Boot 156: publication proven, presentation incomplete

The 2rb build (667372BC) registered and started the service, and WindowServer
queried attributes 1/0x980. The panel retained the Apple logo; SSH and registry
walks remained responsive. All experiment one-shots cleared. This is not a
successful display backend. Do not repeat this experiment just for 2rc's
diagnostic rebuild.

The last recorded user request was **0x921**. IOPresentment's
`__GatherAGDCLogicalDeviceCapabilities` (0x7ff812e9781a) submits a **0xb0-byte
input/output** structure whose first word selects the framebuffer. Apple's
AMD vendor dispatch, table at 0xbfec050 entry 1, leads via 0xbfebd9f to
**getLinkConfig (0xc009184)**. It checks the selected framebuffer against
the capability framebuffer count and returns **AGDCLinkConfig_t**: flags at
+0x08, timing fields at +0x10..+0x9b, stream address at +0xa4 and translated
stream state at +0xac. These need the real boot pipe's state. An offline
all-zero reply cannot describe the running panel.

After that query succeeds, IOPresentment calls
`__GatherAGDCDisplayPipelineCapabilities` (0x7ff812e979dc), using **0x711**
and a **0x196c-byte input/output** structure. Apple's dispatch maps it to
`getFbExtendedCapability`. Capability dictionaries must describe implemented
plane operations; the actual bind/transaction/scanout backend is still needed.
Neither this stage nor a page flip was established in boot 156.

2rc removes the vendor callback's blanket input-length rejection. Supported
attributes still require their exact valid sizes; unknown operations return
Unsupported without modifying output. Vendor attribute/input/output sizes
are recorded before dispatch. `agdc_last_ret`, `agdc_failed_attr/ret` and
`agdc_logical_calls/in/out/ret` record Apple's filtered result, including
refusals that never reach the vendor callback. The old `unsupported=3` was
not the next missing operation: nonzero-input requests returned before that
key was updated. plugin_check now preserves boot arguments, boot-session UUID,
display logs and (when root) a three-second WindowServer sample.

## 2rd: attribute 0x921 is implemented from the real boot timing

`AGDCLinkConfig_t` is **not** a new AMD structure. It is 0xb0 bytes and embeds
the **public** `IODetailedTimingInformationV2` at **+0x04**, with the three
`__reservedA` words the header documents as "Init to 0" reused by AGDC:

| offset | field |
|---|---|
| +0x00 | framebuffer index — **IOPresentment writes it**, Apple reads it back out of the *output* buffer |
| +0x04 | link id (`0xFFFFFFFF` = none) — overlays `__reservedA[0]` |
| +0x08 | flags, u64, low byte only — overlays `__reservedA[1..2]` |
| +0x10..+0xa3 | the V2 body from `horizontalScaledInset` on |
| +0xa4 | stream index — Apple's own value is `fb->0xec + 1`, 0 when `0xff` |
| +0xa8 | `fb->0xf0` |
| +0xac | stream state — `translateAmdStreamState`, whose out-of-range result is `0x80000000` |

Measured in `getLinkConfig` (`0xc009184`) and verified by `offsetof`, not by
counting header lines: `sizeof(V2)` is 0xa0, so `0x04 + 0xa0 = 0xa4` is exactly
the next written field and the whole structure sums to the 0xb0 Apple's own
length check demands. Detail and the three independent confirmations:
JOURNAL **2rd**.

**The timing comes from the boot framebuffer, not from DCN and not from
invention.** `IONDRVFramebuffer::getCurrentDisplayMode` and
`getTimingInfoForDisplayMode` are both **defined** in IONDRVSupport (rule 1,
`nm`) and **neither is routed** anywhere, so `processNDRV` resolves and *calls*
them — rule 3 untouched, nothing patched, no register read or write. The reply
is refused unless the driver sets `kIODetailedTimingValid` and both
`horizontalActive` and `verticalActive` are non-zero.

**A missing or invalid timing is a refusal, never a zero fill.** 2rd measured
that IOPresentment validates **no** timing field: it copies `reply+0x10..0x9b`
into its own cache and returns success either way. A zero-filled reply would
therefore *pass* this stage and silently poison that cache — the failure that
would be hardest to find later. Without real timing the bridge returns
Unsupported and leaves IOPresentment exactly where boot 156 left it.

**Flags bit 0 stays clear, bit 3 is set.** Apple sets bit 3 unconditionally.
Bit 0 is the only field IOPresentment acts on: `0x7ff812e98af6` reads it and,
when set, issues selector **0x925** (`getPortStatus`, 0xc bytes) and then a
0x24-byte follow-up. Setting it would claim a port sub-protocol the bridge does
not implement. Apple's dispatch table at `0xbfec050` holds **int32 offsets from
its own base**, not pointers.

**What the bridge reports is "one framebuffer, no AGDC link or stream":** link
id `0xFFFFFFFF`, stream index 0, stream state `0x80000000`, which are Apple's
own values for "none" and "unknown". That is what this bridge actually knows.
**Not established:** whether anything past this stage requires a real link —
this stage does not check, and the two link constants are single named values
in `rb_agdc.h` so a later boot's evidence can change them in one line.

Reachability is no longer an inference: `0x921` is traced through Apple's
`filtered_doDeviceAttribute` — not 0x712/0x713, outside the 0x4000 range, so
`0x13dfd1f1 → 0x13dfd204 → 0x13dfcf0d`, which calls **`*0x858(%rax)`**, the
vendor slot this bridge overrides.

## 2rf: attribute 0x711 needs exactly one capability

`AGDCFBGetExtendedCapability_t` is a **query**: `u8` framebuffer index at
+0x00, `u32` type at +0x04, `u32` count at +0x08 (**in** the caller's capacity,
**out** the number filled), then **8 entries of 0x32c** from +0x0c. The
arithmetic closes three separate measurements: an entry is 0x32c because
`getFbCapabilityForScaler` bzeroes `entry+0x08` for 0x324; `8 * 0x32c + 0xc`
is exactly the 0x196c Apple's filter demands; and 8 is the capacity
IOPresentment pre-loads.

IOPresentment's `__GatherAGDCDisplayPipelineCapabilities` (`0x7ff812e979dc`)
issues **sixteen** 0x711 queries, one per type bit — and eleven of them are for
types **Apple itself does not implement**, so it tolerates failure by design.
`0x2006` comes from exactly two sites, both belonging to the **first** query,
type **0x10, the scaler**: a non-zero IOReturn and a returned count of zero.
Later failures fall into the next query, and the last is preceded by
`xorl %r14d,%r14d` so even its failure returns success. A count of 9 or more is
clamped to 8.

**So the stage reduces to: answer type 0x10 with count ≥ 1, refuse the rest.**
IOPresentment reads nothing from the entries here either — only the count,
stored at `this->0x42c` before it sets `this->0x434` bit 0.

The bridge reproduces Apple's own **no-scaling** entry (`0xbfed05a`) with the
extents taken from the same real timing 0x921 uses. Any other type returns
Unsupported **without writing a byte**; index ≠ 0, a missing timing or a zero
active dimension refuse; entries 1..7 stay entirely zero, because
IOPresentment offered room for eight and a stale byte there would be a
capability nobody implements.

**NOT established, and not guessed:** which slot in each pair is width and
which is height. `updateStreamComposition` (`0xbfe9270`) fixes the *pairing*
(`fb->0x71c` with `0xc0`, `0x720` with `0xc4`, a letterbox fit) but not the
axis names. It cannot change the entry's meaning — source and destination
extents are equal, so transposing both pairs still says 1:1 — and the host
test asserts that equality rather than the axis assignment. Detail: JOURNAL
**2rf**.

## 2rg: the 0x921 cache is the advertised capability, and it feeds one formula

`IOPresentmentCapabilities::__gatherCapabilities()` (`0x7ff812e95370`, 0x104
bytes) sits above both gathers. It aborts only when
`__GatherAGDCLogicalDeviceCapabilities` returns non-zero; then it stores
**60.0f** (`0x42700000`) and overwrites it with

> **refresh = pixelClock / ((hActive + hBlanking) × (vActive + vBlanking))**

whenever the product exceeds the `ucomiss` constant at `0x7ff812ea3468`, which
reads **`0.0f`** — a divide-by-zero guard, not a plausibility test.

**This corrects 2rd's "a cache it never reads in this image."** The cache has
24 reads, and `IOPresentmentCreateEndPointDictionary` (`0x7ff812e9550c`)
republishes fourteen of its fields as CFNumbers under Apple's own keys —
`PixelClock`, `MinPixelClock`, `MaxPixelClock`, `HorizontalActive`,
`HorizontalBlanking`, `HorizontalSyncOffset`, `HorizontalSyncPulseWidth`,
`VerticalActive`, `VerticalBlanking`, `VerticalSyncOffset`,
`VerticalSyncPulseWidth`, `HorizontalSyncConfig`, `VerticalSyncConfig`,
`VerticalBlankingExtension` — field for field, in order, matching the public
header. That is the strongest confirmation yet that the V2 struct sits at
`reply+0x04`: Apple names the fields, so no offset matching of ours is
involved. The derived values go out as **`LooksLikeWidth`** /
**`LooksLikeHeight`** and, from one address, **`RefreshTime`** /
**`MinRefreshTime`** / **`MaxRefreshTime`**.

So 2rd holds exactly as far as it was measured: IOPresentment **validates** no
timing field, and **consumes** nearly all of them.

**One new refusal.** A zero `pixelClock` passes the `> 0.0f` guard — the guard
tests the totals, not the clock — and publishes a 0.0 Hz refresh, where a zero
total would leave Apple's stated 60.0f. `rb_agdc_link_config` refuses it before
touching the output. `IOPresentmentGetCurrentEndPointDimensions`
(`0x7ff812e96ed6`) independently justifies the existing zero-active refusal: it
returns having written **neither** output in that case, leaving the caller's
values untouched. It also shows `horizontalScaled`/`verticalScaled`
(V2+0x18/+0x1c) **override** the actives when non-zero, which the bridge
inherits from the NDRV by copying the V2 body wholesale.

**flags bit 0 is safe, and 2rd overstated it.** Bit 0 (`0x7ff812e979b7`) calls
`__GatherAGDCLinkCapabilities(reply+0xa4)` (`0x7ff812e98af6`): selector 0x925
(0xc bytes) then 0x988 (0x24 bytes), **every** failure tolerated, returning
void with the success code already zeroed at `0x7ff812e979b4` in a callee-saved
register, and writing only `0x19c78` — **four writers, zero readers in the
whole image**. Bit 0 stays clear because the bridge knows of no link to
describe, which is honesty rather than hazard avoidance. `reply+0xa4`/`+0xa8`
are **one** field: a single `movq` passed as `AGDCStreamAddress_t`, so the zero
at `+0xa8` is the address's high half, not padding. Detail: JOURNAL **2rg**.

## Implementation

`src/plugin/rbgpu_agdc.cpp` solves AGDC's superclass, constructor, destructor
and two vtables at its kext-load callback. It copies the live tables and
overrides four instance slots: complete/deleting destructors, `getMetaClass`
and the pure vendor callback at **0x850**. The metaclass's allocator at
**0x88** is overridden too. Every inherited method comes from Apple's table.
There is no declared AGDC/IOGraphicsFamily dependency in Info.plist.

The runtime guards check the table extents/terminators, all six relevant
instance slots by their separately solved symbols, the metaclass allocator,
the sole pure-virtual slot and the routed function's first eight bytes. The
worker additionally verifies registered superclass identity, size **0x110**
and `IOService` superclass. An unknown layout refuses.

The existing `enableController` hook hands off the boot framebuffer. A
thread-call worker runs after that hook, waiting at most 100 retries of
100 ms for the registered AGDC superclass and the existing AppleGPUWrangler.
Either kext-load/framebuffer ordering can wake the worker. It registers a
runtime metaclass using 2qy's tested pre/ctor/post sequence, then constructs,
initializes and attaches the service. Apple's `AGDC::start` publishes it.
The initial instance reference is pinned for the boot, including on start
failure: returning false after cross-kext registration does not undo it (2pl).

## The two attributes

The reply builders are freestanding, bounded and host-tested in
`src/common/rb_agdc.h`.

* **1 / kAGDCVendorInfo**: exactly **0x2c** bytes, interface 0x30000, name
  AMD, vendor 0x1002, role **2**. These values are read from Apple's own
  `AmdAgdcServices::getVendorInfo` at **0xbffb59a**; the AMD framebuffer's
  wrangler uses role **7**, which IOPresentment rejects.
* **0x980 / kAGDCGPUCapability**: exactly **0xdc** bytes, one framebuffer at
  +0x20/+0x30, PCI object at +0x34 and framebuffer object at +0x3c. Other
  feature masks are zero: the bridge implements no links, VRR or DSC.

**Those two capability entries are kernel object pointers, not registry IDs.**
`getGpuCapability` stores Apple's controller's PCI/framebuffer pointers;
`AGDC::filtered_doDeviceAttribute` converts them at **0x13dfd167** before
copying the reply to userspace. The bridge preserves that path unchanged.
Writing IDs in the vendor callback would make Apple dereference IDs as
pointers. Returning the kernel form directly would expose kernel addresses.

Attribute 1 normally falls into the global plugin table, so one hook on
`filtered_doDeviceAttribute` answers **only attribute 1 on our exact service
pointer**. All other services/attributes, including 0x980, use Apple's code.
`newUserClient`, `authorizeTask` and the private entitlement requirements are
unchanged. `sudo agdcinfo` therefore remains unable to query the replies;
WindowServer's own IOPresentment path is the runtime test.

## Activation and recovery

**Off by default.** Both `-rbgpuagdc` and the independent **rbgpu-agdc**
one-shot are required. The one-shot must clear through the existing
fail-closed helper before even the filtered hook is placed. No EFI or NVRAM
setting was changed while implementing this. The bridge contains no DCN
register writes, but newly reachable Apple presentation paths can still hang
or blank the panel; boot 156 retained the Apple logo.

The user runs installation/reboot, per CLAUDE.md. This test also needs the
already verified IOP vendor-gate patch armed in the same boot:

```sh
sudo tools/efi.sh install && sudo tools/efi.sh status
sudo tools/efi.sh bootarg add -rbgpuagdc
sudo tools/efi.sh bootarg add -rbgpuiop
sudo tools/efi.sh on
sudo nvram rbgpu-agdc=1 rbgpu-iop=1 rbgpu-arm=1
sudo reboot
```

After login, before another reboot:

```sh
sudo tools/plugin_check.sh
sudo tools/compositor_check.sh
```

A power cycle should return to baseline after the one-shots have cleared;
`-rbgpuoff` remains the full rescue. Do not rearm blindly after a failure.

## Reading the result

First verify a new `rbgpu-boot` and loaded UUID against `build/out`.
`agdc_*` has priority in the fixed NVRAM summary; `rbgpu-agdc` is included in
plugin_check's variable list. Full details remain in RBGPUDiag/RBGPULog.

| Key | Expected / meaning |
|---|---|
| agdc_oneshot | 0 consumed; 1 options missing; 2 absent; 3 literal 0; 4 clear failed, refused |
| agdc_error | 0 no refusal; codes below |
| agdc_solved | **0x3ff / 1023**; all ten inputs found. Bits 5..8 are the four kernel symbols rejected in boot 155 |
| agdc_stage | 10 layout verified; 20 one-shot consumed; 30 filter routed; 40 before class registration; 41 returned; 60 before start; 70 returned |
| agdc_mc_post / agdc_mc_ok | 0 / 1 |
| agdc_started | 1; also inspect a live RembrandtGPUAGDC service under the PCI device |
| agdc_user_vendor / agdc_user_cap | User-kernel attribute calls, distinct from AGDC::start's own vendor calls |
| agdc_last_attr / agdc_vendor_unsupported | Next protocol request to implement if presentation stops farther down |
| agdc_tim_src | **3** required: both IONDRVSupport timing symbols resolved. 0/1/2 means 0x921 is refused |
| agdc_tim_raw_cur / agdc_tim_raw_info | Raw solve results, published only when one failed (2rb's lesson) |
| agdc_link_calls / agdc_link_result | 0x921 requests reaching the vendor callback, and the builder's verdict (0 = replied) |
| agdc_tim_err | **0** required. 1 getCurrentDisplayMode failed; 2 getTimingInfoForDisplayMode failed; 3 kIODetailedTimingValid clear; 4 zero active pixels; **5 zero pixelClock** (2rg) |
| agdc_tim_mode / agdc_tim_h / agdc_tim_v | The mode and the active pixels actually handed to IOPresentment — if these are not the panel's, nothing downstream means anything |
| agdc_tim_hblank / agdc_tim_vblank / agdc_tim_clk | The other three fields Apple's refresh formula consumes (2rg). A zero clock is refused: it passes Apple's `> 0.0f` guard and would publish 0.0 Hz |
| agdc_tim_mhz | **The refresh in millihertz that Apple's OWN formula will produce** from this timing — `pixelClock / ((hActive+hBlank) * (vActive+vBlank))`, published even on a refusal. Compare against the panel's **143 833**. **0** or **60 000** means the NDRV's detailed timing is incomplete, and that is the next thing to fix rather than the bridge |
| agdc_tim_flags | The driver's own timing flags; bit 31 is kIODetailedTimingValid |
| agdc_extcap_calls / agdc_extcap_answered | 0x711 requests seen, and how many were answered (expect 1 of ~16: only type 0x10) |
| agdc_extcap_type | The type of the LAST 0x711 seen. If this is always 0 the vendor callback is being handed a separate zeroed output copy, and every type is refused for that reason rather than for being unimplemented |
| agdc_extcap_result | 0 when the scaler reply was built; -2 for a type we refuse, which is expected for 15 of the 16 |
| iop_verify | 1; otherwise WindowServer never passed the earlier gate |

Refusal codes: 1 symbols/ranges; 2 vtable/prologue; 3 unarmed; 4 route;
5 framebuffer class; 6 PCI identity; 7 worker allocation; 8 readiness timeout;
9 superclass layout; 10 metaclass registration; 11 registered name;
12 allocation; 13 init; 14 properties/attach; 15 start failure (instance pinned).

`agdc_base` / `agdc_size` and the raw `agdc_vt`, `agdc_mvt`, `agdc_mc`,
`agdc_ctor`, `agdc_dtor`, `agdc_mcc`, `agdc_pre`, `agdc_post`, `agdc_pure`,
`agdc_filter` addresses remain in RBGPUDiag even after early log lines have
been evicted. `agdc_bad_slot` identifies a failed instance-vtable check.

**A published service is a partial result.** The full success criterion is
WindowServer creating a display pipe, obtaining an IOSurface-backed display
surface and compositing through Metal. Missing plane-state/bind support may
be the next blocker; neither host tests nor successful registration prove it.

## 2rk - the policy-driver opt-out

`AppleGraphicsDevicePolicy` matches on `IOProviderClass =
AppleGraphicsDeviceControl` with no PCI or board-id gate, and its `probe`
(`0x13e0989a`) returns a match with score 1000 unless the provider carries
`NeedsPolicyDriver` as an `OSBoolean` whose `getValue()` is false. An absent
property therefore recruits it. The bridge publishes it **false** between
`init()` and `attach()` - where AGDP's own nested AGDC does the same
(`0x13e0e800`).

| key | meaning |
|---|---|
| `agdc_policy_optout` | 1 = `NeedsPolicyDriver=false` published; 0 = `-rbgpuagdcpolicy`, property left absent |
| verdict 16 | the property could not be published; the bridge refused |

Boot-arg `-rbgpuagdcpolicy` leaves the property absent, for an A/B test of
whether the policy driver is what freezes the panel. `AGDC::start` never reads
the `0x980` capability reply - it branches only on `vendorInfo+0x28`, which it
tests twice (see JOURNAL 2rk for the stack-slot proof).
