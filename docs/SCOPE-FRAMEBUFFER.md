# Scoping the AMD framebuffer stack — what it would actually take

Written 2026-10-25, after 2qr, entirely from static measurement on this
machine. No boot was spent on it. Every number below is reproducible with the
commands in the footnotes; nothing here is estimated from general knowledge.

The question: 2qr closed the display chain at "AGDC has no vendor information
for this GPU, because the AMD framebuffer stack never instantiates". So what
would supplying that stack cost?

---

## 1. The short answer

**The hard blocker is not the amount of code. It is that the one kext able to
subclass `IOFramebuffer` cannot be loaded by this boot path.** The DCN 3.1.2
work — the part that sounds frightening — turns out to be the *smaller* half,
because 94.7 % of the display registers Apple already compiles are at
identical offsets on this chip.

Three routes exist. Two are closed by measurement. The third has never been
tried and is the only thing worth arguing about.

---

## 2. What is missing, precisely

From 2qr, measured in WindowServer on boot 150:

```
(IOPresentment) Error: Unable to get AGDC information.  Return code: 0x10000003
```

`AppleGPUWrangler`'s `debug-gpus` row for our GPU, read live:

```
{"id"=64455, "acc"=0x1000003C0, "fbs"=(0x1000003BE),
 "gpuc"=0, "agdp"=0, "pci"=0x1000000F9, "cc"=3, ...}
```

Decoded out of `AppleGPUWrangler`'s own dictionary builder: **`gpuc` is
`gpuStruct + 0x10`** (an `OSNumber::withNumber(..., 64)` of that field) and
**`agdp` is a registry ID taken from the object at `gpuStruct + 0x68`**. Our
framebuffer and accelerator are correctly paired in the row; both of these are
zero. They are what IOPresentment asks for (`AGDCVendorInfo`,
`AGDCGPUCapability` in its string table) and they are filled by the vendor's
AGDC plugin, which ships with the AMD framebuffer stack.

---

## 3. Blocker 1 — the kernel collection. This is the real one.

Live addresses, this boot:

| kext | load address | collection |
|---|---|---|
| `com.apple.iokit.IOPCIFamily` | `0xffffff8002886000` | **boot KC** |
| `com.rembrandtgpu.plugin` (ours) | `0xffffff8004043000` | **boot KC** |
| `as.vit9696.WhateverGreen` | `0xffffff80056c7000` | **boot KC** |
| `com.apple.iokit.IOGraphicsFamily` | `0xffffff7f95c94000` | **system KC** |
| `com.apple.iokit.IONDRVSupport` | `0xffffff7f95ce8000` | **system KC** |

Two distinct address ranges, `0xffffff80…` against `0xffffff7f9…`. OpenCore
injects us into the **boot** KC; `IOGraphicsFamily` lives in the **system** KC.
A kext that declares `OSBundleLibraries → com.apple.iokit.IOGraphicsFamily`
cannot have that dependency satisfied there, so it fails to load — and because
our plugin is what makes the accelerator work, a failed load takes the working
desktop with it.

2oz already built that subclass and compiled it: `IOFramebuffer.h` ships in
Kernel.framework, there are only four pure virtuals, and it resolved **356 of
356** imports. It was correct code. It cannot be loaded from here.

`/Library/Extensions` is not an escape — Auxiliary KC loads far too late for a
framebuffer, and the project forbids installing there anyway.

### But the blocker is narrower than it looks, and this is the key finding

Our plugin's declared `OSBundleLibraries` are **only** Lilu, IOPCIFamily and
the five `com.apple.kpi.*`. It declares nothing for IOGraphicsFamily. And yet
it already **routes 35 `IONDRVFramebuffer` call sites** and solves symbols
inside `IOGraphicsFamily`, `IONDRVSupport`, `IOAcceleratorFamily2` and four AMD
kexts — across that very KC boundary, every armed boot, for months.

So the boundary blocks **static linking** (a subclass needs its vtable and
symbol imports bound at load time). It does **not** block calling into, or
routing, code in the system KC. Those are different mechanisms, and this
project has been using the second one all along.

---

## 4. Blocker 2 — Apple's own framebuffer will not match, and could not drive it

Making `AMDRadeonX6000Framebuffer` load instead of writing one:

* Its personality matches Navi 21 PCI IDs; ours is `0x16811002`. **2lj measured
  that spoofing `device-id` kills the accelerator** — this whole plugin works by
  making Apple *accept* `0x1681`, and reporting another id bypasses every patch
  that does so. `-rbgpufbmatch` must stay out.
* It is **not loaded at all** on this machine, so its OSMetaClasses are not even
  registered.
* And it contains no code for this chip. Source-path strings in
  `AMDRadeonX6000Framebuffer` (10 MB, 2339 defined symbols, 176 vtables —
  this, not `AMDSupport`, is where Apple's DAL/DC lives):

| binary | `dcn30` | `dcn302` | `dcn21` | `dcn31`/`dcn312` | `dml` |
|---|---|---|---|---|---|
| `AMDRadeonX6000Framebuffer` | 58 | 15 | 25 | **0** | 128 |
| `AMDSupport` | 0 | 0 | 0 | 0 | 0 |
| `AMDFramebuffer` | 0 | 0 | 0 | 0 | 0 |

So a matching personality would instantiate a controller with no resource pool,
hwseq, clk_mgr or DML for DCN 3.1.2.

---

## 5. The DCN 3.1.2 delta is small, and it is the good news

Comparing Linux's `dcn_3_0_2_offset.h` (the newest DCN Apple actually compiles)
against `dcn_3_1_2_offset.h` (this chip), normalising the `mm` and `reg`
prefixes and comparing **both** the offset and its `BASE_IDX`:

```
dcn302 6996 regs, dcn312 6401 regs, 5630 names in common
only on our chip: 771     absent on our chip: 1366
TOTAL common 5630, moved 298  -> 94.7 % identical
```

Per block:

| block | common | moved |
|---|---|---|
| `CM` (colour management) | 1000 | **0** |
| `DP` | 559 | **0** |
| `HUBP` (incl. `HUBPREQ`) | 444 | **0** |
| `OTG` | 434 | **0** |
| `MPCC` | 408 | **0** |
| `CURSOR` | 68 | **0** |
| `DSCL` / `DSCC` / `DMCUB` / `ABM` / `OPP` / `DPP` / `HPD` | — | **0** |
| `MPC` | 622 | 53 |
| `DC` | 295 | 91 |
| **`DCHUBBUB`** | **77** | **74** |
| `AFMT` / `VPG` / `DCN` / `DIG` | — | 19 / 10 / 9 / 5 |

**Everything on the scanout path — OTG timing, HUBP surface address, cursor,
DP, colour management — is bit-identical.** The one block that genuinely moved
is `DCHUBBUB`, the memory-hub/bandwidth configuration, which is a **modeset**
concern — and the firmware has already done the modeset: `OTG0_CONTROL =
0x00011201` with DMCUB firmware 4.0.15.47 running (measured by the level-2
probe).

This is the second independent confirmation of the 2pq reading, computed from
the headers here rather than recalled.

---

## 6. The three routes, and what each really costs

### Route A — publish our own `IOFramebuffer` subclass, statically linked
**Closed.** Blocker 1. Built and compiled in 2oz; cannot be loaded from the
boot KC. Would need to be in the system KC, which is not ours to modify, or the
Aux KC, which loads too late and is forbidden here.

### Route B — make Apple's stack match and drive our chip
**Closed, twice.** Blocker 2: the id spoof kills the accelerator (measured,
2lj), and there is no DCN 3.1.2 code to run even if it matched. "Patch dcn302
into dcn312 behaviour inside a 10 MB binary we do not compile" is not a port,
it is an open-ended patching campaign against `MPC` (53), `DC` (91) and
`DCHUBBUB` (74) — and the last of those is the modeset block.

### Route C — construct the framebuffer at runtime, no declared dependency
**Never tried. The only route not closed by a measurement.**

The observation in §3 is the whole argument: we already execute and route
system-KC code without declaring it. What a subclass additionally needs is an
`OSMetaClass`-registered C++ class whose vtable exists at runtime — allocated
and populated by us, with `OSMetaClass`'s constructor called by hand (2qm
already measured its layout: `reserved` +0x08, `superClassLink` +0x10, the name
`OSSymbol*` at +0x18, `classSize` +0x20, from `is_io_object_get_class` and the
constructor's single store).

Honest assessment of Route C:

* **Not obviously impossible.** Every symbol needed (`IOFramebuffer`'s vtable,
  its metaclass, `IOService::registerService`) is resolvable the same way the
  AMD symbols already are.
* **Less fragile than first written here (corrected 2qt).** This section
  originally said a hand-built vtable would have to synthesise every slot, and
  that overstated it: `IOFramebuffer`'s vtable is at least `0x9b0` bytes
  (~311 slots — 2pq resolved `getVRAMRange` at slot `0x9b0`), but a subclass
  does not *invent* those. It **copies** `__ZTV13IOFramebuffer` and overwrites
  only the handful of slots it overrides, which is what the compiler does. The
  risk is a wrong slot *index*, not 311 wrong addresses.
* **Still genuinely hairy.** A copied vtable with one index wrong is a kernel
  panic on the path that owns the display, and slot indices are not stable
  across OS updates — the fail-safe property that makes `-rbgpumtlkfix`
  acceptable (read back and compare before writing) does not transfer to a
  vtable we assemble.
* **`sizeof(IOFramebuffer)` = `0x1d0` and its superclass is
  `IOGraphicsDevice`**, not `IOService` — read statically from
  `IOFramebuffer::MetaClass::MetaClass()` at IOGraphicsFamily `0x14fb7b6a`,
  which passes `("IOFramebuffer", &IOGraphicsDevice::gMetaClass, 0x1d0)` to
  `OSMetaClass`'s constructor. A subclass must chain through
  `IOGraphicsDevice`.
* **And it only gets us to the start line.** With the framebuffer published we
  would still owe: AGDC vendor info and GPU capability (§2), which means
  impersonating a vendor AGDC plugin; then
  `IOPresentmentBindIOAccelSurfaceToResource` and `IOPresentmentSetPlaneState`,
  which `IONDRVFramebuffer` does not implement and which 2ql named as the
  IOPresentment *world* rather than one call; then the OTG/HUBP programming
  that §5 says is the easy part.

---

## 7. Recommendation

**The DCN port is not the expensive part; the load path is.** If Route C's
runtime-constructed class is rejected as too fragile — and that is a reasonable
call, because a wrong vtable slot is a panic on the display path with no
fail-safe — then **the display half is closed, not merely paused**, and it
should be written down as closed so no future session re-derives §3 for a
fifth time.

What the machine already has, and keeps, either way: a usable accelerated
desktop with flawless rendering, 130–137 Hz pacing, the correct GPU name and
4 GB VRAM reported, and bit-exact Metal compute. What it does not get is a
GPU-composited desktop, which costs ~48 % of one WindowServer thread in
software and shows as ~9 fps animation.

The one piece of Route C worth doing *first*, if it is ever attempted, is the
cheapest and most decisive: **establish whether an `OSMetaClass` can be
registered at runtime from the boot KC at all**, with a throwaway class that
subclasses nothing dangerous and is never attached to a provider. That is a
contained experiment with a clear negative, and it gates everything else.

---

### Footnotes — how to reproduce each measurement

* §2 `debug-gpus`: `ioreg -l -w0 -c AppleGPUWrangler | grep debug-gpus`.
  The `gpuc`/`agdp` field decode:
  `python3 tools/re/strxref.py build/re/AppleGPUWrangler <agw.dis> gpuc agdp`,
  then read `0x13df09c2` and `0x13df0a10`.
* §3 collections: `kextstat | grep -E 'IOGraphicsFamily|IOPCIFamily|rembrandt'`.
  Declared libraries: `OSBundleLibraries` in `src/plugin/Info.plist`.
  Routed-but-undeclared: `grep -c IONDRVFramebuffer src/plugin/kern_start.cpp`.
* §4 DAL location: `strings -a build/re/<kext> | grep -c dcn30` and friends.
* §5 register delta: the normalising comparison of
  `docs/linux-ref/dcn_3_0_2_offset.h` against `dcn_3_1_2_offset.h`, matching on
  name with the `mm`/`reg` prefix stripped and comparing `(offset, BASE_IDX)`.
