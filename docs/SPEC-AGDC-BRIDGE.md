# Per-GPU AGDC bridge — 2ra

Built 2026-10-25. **Boot 155 refused before registration; fixed in 2rb.**
The four-argument Lilu symbol solver treated kernel lookups as range [0,0),
so the four kernel functions all resolved to zero. The new lookup uses the
two-argument resolver, records each raw result, and applies explicit bounds
only to kext symbols. Its regression test fails with the old overload.
The corrected build still needs a user-run boot test. This supplies the missing service
at the measured IOPresentment blocker; it does not implement DCN mode setting.

`AppleGPUWrangler` is the only existing `AppleGraphicsDeviceControl` service,
and IOPresentment explicitly skips it. The new **RembrandtGPUAGDC** service
attaches to the real 1002:1681 PCI device and reports the existing
`IONDRVFramebuffer` as its one framebuffer. It leaves that framebuffer in
control of the panel.

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
or blank the panel; they have not been tested.

The user runs installation/reboot, per docs/RULES.md. This test also needs the
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
