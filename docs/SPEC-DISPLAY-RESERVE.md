# Panic 2026-10-06 14:34: display_mode_did_change returns false

Read from macOS 15.8.1 (AMDRadeonX6000 7.0.25, IOAcceleratorFamily2) with
tools/re. Addresses are file addresses of the extracted kexts in build/re;
resolve by name.

## The chain

```
IOFramebuffer::open -> deliverDisplayModeDidChangeNotification
  IOAccelDisplayPipe::display_change_handler
    IOAccelDisplayMachine::display_mode_did_change(uint)          IOAcceleratorFamily2 0x14e2148e
      this->vtable[0x870]()  = AMDAccelDisplayMachine::displayModeDidChange   X6000 0xbe48428
        returns set_display_mode_and_vram()                                   X6000 0xbe48604
      false -> panic, unless accel(+0x128) byte +0xc91 has bit 0x20 set (then it just returns false)
```

`set_display_mode_and_vram` returns false at exactly one place: the loop
over display pipes (0xbe48913..0xbe48a98). For each pipe that is enabled
(pipe+0x35c == 0, +0x35d == 1, +0x35e == 1):

```
mem    = hw->vtable[0x2d8]()            = hw+0x370, an AMDGFX10HWMemory (AMDGFX10Hardware::allocateAMDHWMemory)
size   = max(pipe+0x348 (u32), pipe+0x34c (u32))
mem->safeFree(pipe+0x370 element)                         vtable 0x1a0
offset = pipe+0x340 (u64) + mem->vtable[0x1e0]()  (getter: mem+0x50)
ok = mem->reserve(&pipe+0x370, offset, size, 1, 0x1e)     vtable 0x198
if !ok: accel+0x380 ->vtable[0x158](); accel->vtable[0x948](); retry reserve once
if still !ok: return false   -> the panic
```

`AMDHWMemory::reserve(AMDMemoryElement*, u64 offset, u64 size, bool, _eOP_ORIGINATOR)`
(X6000 0xbe8c454, defined in X6000, 226 bytes) fails when its range allocator
(mem+0x68, an IOAccelMemoryAllocator) is NULL or when
`IOAccelMemoryAllocator::reserve(elem, offset, size)` (IOAcceleratorFamily2,
a kernel-collection import for X6000: never route it by X6000's id) refuses
the range.

So the accelerator is trying to fence off the boot framebuffer's memory
inside its own VRAM allocator, and the range it computes is not one the
allocator manages. The pipe fields (+0x340 offset, +0x348/+0x34c sizes) are
filled by `AMDGFX10Display::getDisplayInfo` (X6000 0xbec0a0e; the Navi 21
display helper is hw+0x398) from the IONDRVFramebuffer.

Likely cause (to confirm by logging, not assumed): Apple's memory model.
Navi 21 expects the scan-out buffer inside VRAM behind BAR0; on the 680M the
GOP framebuffer is at carve-out offset 0 (GPU 0xF4_0000_0000, system
physical 0x7_8000_0000) and BAR0 windows only 256 MB. If the offset is
computed from the framebuffer's physical address against a VRAM base that
does not match, it lands outside the allocator's range. This is item C4 in
docs/APPLE-STACK.md.

## Next step: log only (no behaviour change)

Hook `__ZN26AMDRadeonX6000_AMDHWMemory7reserveEP16AMDMemoryElementyyb15_eOP_ORIGINATOR`
in AMDRadeonX6000 (long enough for a Lilu route). Log, before and after the
original:

- `this`, offset, size, the bool, originator, return value;
- `this+0x50` (the base added to the pipe offset) and `this+0x68` (the
  allocator; NULL means the allocator never got set up);
- once per boot, the same for the first few successful reserves, to see
  what ranges the allocator does accept.

That one boot tells which case it is: no allocator, offset outside range,
or an overlap with an earlier reservation. Fixing it (correct offset, or a
reservation made where the allocator expects it) comes after.

Avoiding the panic is a separate question: bit 0x20 of the accelerator's
byte +0xc91 turns the panic into a plain false. What sets that bit is not
traced yet.
