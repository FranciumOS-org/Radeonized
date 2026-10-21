# The text-rendering defect — problem statement

**Project.** A Lilu kernel plugin makes Apple's Navi 21 macOS GPU stack
(`AMDRadeonX6000` + `AMDRadeonX6800HWLibs` + `AMDRadeonX6000HWServicesNavi` +
`IOAcceleratorFamily2`) drive a **Radeon 680M** (Rembrandt APU, `1002:1681`
rev 0a, GC 10.3.3 / DCN 3.1.2 / PSP+SMU 13.0.x / SDMA 5.2.3) on **macOS
15.8.1 (24H32)**, ASUS TUF A15 FA507NU, booted from the EFI via OpenCore.
The plugin works by making Apple **accept** device `0x1681` rather than
spoofing a Navi 21 id — spoofing `device-id` kills the accelerator outright.

## What already works

An armed boot is a **usable, accelerated Mac**. The user logs in and works in
it. Specifically:

- **Zero GPU VM faults.** Apple's own poller reads `c=0 f=0 r=0 o=0 fw=0`
  with fault reporting fully enabled (`vmfc=0x1ffc`), across boots 93–104.
- **No CP halt.** `CP_ME_CNTL` reads 0; the halt sampler reports
  `NEVER HALTED in 0.5–12s` every boot.
- **Metal works end to end.** `MTLCopyAllDevices` returns the device;
  `newCommandQueue` succeeds; a runtime-compiled 4096-thread compute kernel
  returns **bit-exact** results.
- **WindowServer composites through Metal** on `AMDRadeonX6000MTLDriver`.
- The fix that got here was one missing term: Apple's
  `AMDHWMemory::adjustVRAMAddress` returns `mc - vram_start`, and the GPUVM
  walker needs `mc - vram_start + vram_base_offset`
  (Linux's `amdgpu_gmc_vram_mc2pa`), applied at three call sites.

## The symptom

**Body-text glyphs are scrambled and unreadable.** Window chrome, icons, the
Dock and widgets render correctly.

The single most constraining observation, from a photograph:

> In **one Safari window**, the page's **text renders perfectly** while an
> embedded **image (the Wikipedia globe) is scrambled**. Meanwhile
> **Terminal's text is scrambled.**

Same process, same window, same surface — two different outcomes. That
excludes anything per-process, per-surface, or per-window.

Also: iMessage and Finder render fully correctly, including while moving.

## What is EXCLUDED, with the evidence

This is the important part: **it is not a layout/tiling/swizzle problem.**
Five independent attempts at a layout parameter all failed, and two boots
bracket the question directly.

| # | Hypothesis | How it died |
|---|---|---|
| 1 | Tiling params wrong | `_gc_get_tiling_params_10_1` just reads `GB_ADDR_CONFIG`; correct |
| 2 | Kernel AddrLib geometry wrong | `ADDR_CREATE_INPUT` carries the chip's own `GB_ADDR_CONFIG = 0x242` (4 pipes, 4 packers, 1 SE) |
| 3 | Rotation / `_X` swizzle variants | Remapping 27→25 changed nothing (`remapped=9`) |
| 4 | **Userspace** AddrLib misconfigured | **All four inputs identical to the kernel's** — see below |
| 5 | Per-resource swizzle mode | Boots 103 and 104 bracket it — see below |

### 4 — userspace AddrLib is configured identically

`AMDRadeonX6000MTLDriver` (in the dyld shared cache) carries its **own
complete AddrLib**. Its configuration comes from the kernel through one
`memcpy`:

```
AMDRadeonX6000_AMDAccelDevice::getHardwareInfo(_sAMD_GET_HW_INFO_VALUES*)
  0xbe47ca4   memcpy(vals, (hw->0x1a38)->vtable[0x1c0](), 0x204)

then in userspace:
gfx10_HwInfoInitData          0x7ffb08cb4218  movl 0xa0(%rsi), %r9d
                              0x7ffb08cb421f  movl %r9d, 0xa4(%rdi)
amdMtl_InitAddrLibInterface   0x7ffb08c9409f  chipFamily   = hwInfo->0x00
                                              chipRevision = hwInfo->0x10
amdMtl_HWL_InitAddrLibHwRegFields
                              0x7ffb08bfde4d  chipEngine = 0xd
                                              regValue.gbAddrConfig = hwInfo->0xa4
  -> AddrCreate -> Addr2ComputeSurfaceInfo -> every client texture
```

Measured (`rbgpu-hwi`, boot 102/104):

| input | kernel | userspace |
|---|---|---|
| chipEngine | 0xd | 0xd |
| chipFamily | 0x8f (`FAMILY_NV`) | 0x8f |
| chipRevision | 0x28 | 0x28 |
| gbAddrConfig | 0x242 | 0x242 |

Also from that struct: `deviceID=0x1681`, `numRasterPipe=4`,
`numShaderPipes=6` (6 WGPs = this chip's 12 CUs — correct), and
`getHardwareInfo` **returns 0** (Apple accepts its own record; all four
clocks nonzero, so the long-standing `gfx_min = gfx_max = 0` does not reach
this path).

### 5 — the two bracketing boots

A per-resource swizzle mode lives at `res + 0x1dc` bits 0:4 in
`AMDRadeonX6000_AMDAccelResourceAddr2::initialize` (0xbe56894), read off
Apple's own `andl $0x1f` at `be569d8`.

```
boot 103, -rbgpulinear IN   seen=12224  m0=12224                       (all linear)
boot 104, -rbgpulinear OUT  seen=4788   m0=4598 m2=8 m22=125 m27=57    (190 tiled)
```

**The glyphs are identically scrambled in both.** With every client resource
linear, and with 4% tiled, the text looks the same. Confirmed on the screen,
not inferred.

What `-rbgpulinear` *does* affect: the widget/Dock/chrome artifacts come
straight back when it is removed. So there are **two distinct defects**, and
only the non-text one is layout.

### Also excluded

- **DCC metadata** — `dcc calls=0` with the hook confirmed live (`hk=1`), for
  the kernel's surfaces.
- **Addressing in general** — zero VM faults; the GPU writes where it is told.
- **Compute correctness** — 4096-element kernel bit-exact.
- **Display/DCN** — not involved: no framebuffer kext is loaded at all (see
  below), and the desktop runs on the boot framebuffer with the accelerator
  attached.

## The shape of what is left

Both AddrLibs agree, so a client texture is written by the CPU and read by
the GPU through the *same* equation; a self-consistent surface cannot
scramble from a layout mismatch. Combined with the Safari datum (text fine,
image scrambled, same surface), the suspicion is:

> **Content that round-trips through CPU memory as a bitmap scrambles;
> content the GPU rasterizes directly is fine.**

Decoded images and glyph atlases are both CPU-produced bitmaps that are then
uploaded and blitted. Window chrome, icons and Safari's text may be
GPU-rasterized or take a different upload path. **This is a hypothesis, not a
measurement** — it has not been tested.

Candidate mechanisms, none yet examined:

1. **The blit/upload path.** How a CPU-written bitmap's layout is declared to
   the GPU when it is copied into a texture — a descriptor or blit-shader
   assumption that disagrees with how the CPU wrote it, independent of the
   AddrLib mode both sides agree on.
2. **The glyph atlas specifically.** What format/layout CoreText's atlas is
   uploaded with, and why Safari's text path differs from Terminal's.
3. **Pipe/bank XOR or a shader constant** rather than the swizzle mode —
   something that shifts *within* a correctly-sized, correctly-tiled surface.
4. **`numShaderPipes = 6`** is correct for this chip but Navi 21 has far
   more; any Apple code path that derives a shader-side constant from a Navi
   21 assumption would corrupt GPU-side addressing without any AddrLib
   involvement.

## Secondary, separate, and known

- **The display half has never started.** No framebuffer kext loads:
  `AMDFramebuffer` matches `IOProviderClass = AtiFbStub`, no `AtiFbStub` is
  published, because `AMDRadeonNavi21Controller` matches on
  `IOPCIMatch 0x73A01002 0x73A21002 …` and ours is `0x16811002`. Spoofing
  `device-id` to match **kills the accelerator** (it bypasses every patch
  that makes Apple accept `0x1681`). Unexamined prerequisite: whether a
  personality with `IOClass = AMDRadeonX6000_AmdRadeonControllerNavi21` is
  even linkable from an EFI-loaded kext, since
  `AMDRadeonX6000Framebuffer.kext` lives in `SystemKernelExtensions.kc`.
- **"7 MB VRAM" in About This Mac is cosmetic** — Metal reports
  `recommendedMax = 4096 MB`; System Profiler reads `IOFBMemorySize =
  8294400` (1920×1080×4) from the boot framebuffer.
- **`gfx_min = gfx_max = 0`** — the SMU reports no GFX clock range, GFX DPM
  never started. Not the mechanism of anything currently broken (the CP is
  clocked and executing), and it does not reach the hw-info struct. Still
  unexplained.
- **`rbgpu-vmid` reads `av=N fail=N`** — every `assignVMID` reports failure by
  the hook's own criterion while VMIDs are plainly handed out. The criterion
  has never been read against the code that sets it.

## Tooling available

- `tools/re/` — static analysis of Apple's kexts: extraction from kernel
  collections, chained-pointer Mach-O reading, vtable slot → method, string
  xref, jump tables.
- `tools/re/dyldcache.py` **(new)** — reads any image out of the dyld shared
  cache (this machine has no Xcode and no `dyld_shared_cache_util`) and writes
  a compact Mach-O that `objdump` disassembles at the cache's own VM
  addresses. `tools/re/mtldis.sh` disassembles one symbol by regex.
  `build/re/mtl-syms.txt` holds all 20,282 `AMDRadeonX6000MTLDriver` symbols.
- Instrumentation reaches the kernel only. **Lilu's user patcher can
  find/replace bytes in a userspace image but has no per-call callback**, so
  userspace functions can be *forced* but never *logged*. Userspace
  observation has to be done by reading what reaches the kernel.

## Constraints on any proposed fix

- Each hardware experiment costs a reboot and a possible hang; the machine is
  the user's daily driver.
- Deliberate diagnostic panics are off the table.
- One variable per boot.
- A second `RouteRequest` on a function the plugin already routed **panics
  Lilu outright** — a new transform goes inside the existing hook.
- Never route a symbol unless `nm -m` shows it **defined** in that kext, and
  refuse any symbol whose first 5 bytes contain a rel8/rel32 branch (Lilu's
  trampoline copies displaced instructions verbatim).

## The question

**What makes a CPU-produced bitmap — a decoded image, a glyph atlas — render
scrambled on this chip, when the surface's swizzle mode is identical on both
sides of the kernel boundary, there are zero VM faults, and GPU compute is
bit-exact?**
