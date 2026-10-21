# RembrandtGPU — handoff state (2026-10-20, after 2lj)

Apple's Navi 21 macOS GPU stack drives a **Radeon 680M** (Rembrandt APU,
`1002:1681` rev 0a, GC 10.3.3 / DCN 3.1.2 / PSP+SMU 13.0.x / SDMA 5.2.3) on
macOS 15.8.1 (24H32), ASUS TUF A15 FA507NU, via a Lilu plugin booted from the
EFI.

This file is the current **state**. `docs/JOURNAL.md` (~17 000 lines) is the
chronological evidence journal — authoritative for what was measured, in
order, including every withdrawn reading, and **not to be trimmed**. Grep it by
milestone tag (`2ks`, `2lg`) for the history of one step; never read it whole.
`docs/HANDOFF-FULL.md` is a stale dump from 2026-10-05 — ignore it.

---

## 1. State in one paragraph

**An armed boot is a usable, accelerated Mac.** The user logs in, uses the
desktop, and texts from it. The CP halt that blocked this project for its
entire 2e–2k history is **fixed** (2ks/2ku/2kw, boot 89 onward): one missing
addition, `vram_base_offset`, applied to Apple's `adjustVRAMAddress` at three
call sites. Zero VM faults, no halt, no freeze, VMIDs 1–8 all translate.
WindowServer composites through Metal on `AMDRadeonX6000MTLDriver`; a
4096-element compute kernel returns **bit-exact** results.

**The one remaining defect is rendering: body text is unreadable.**
`-rbgpulinear` forces linear surfaces and fixed ~80 % of the corruption
(widgets, Dock, menu bar, desktop). What it did not fix is **app-owned
textures**, whose layout is computed entirely in **userspace** by
`AMDRadeonX6000MTLDriver` — the kernel is never asked. That is the frontier.

The **display half has never started**: no framebuffer kext loads, because
Apple's controller personality matches on Navi 21 PCI IDs and ours is
`0x16811002`. The desktop we have runs on the boot framebuffer with the
accelerator attached, which is why *About This Mac* still reads 7 MB.

---

## 2. THE FRONTIER — userspace texture tiling

### What is established

| Evidence | Reading |
|---|---|
| `-rbgpulinear` fixes widgets/Dock/menu bar/chrome, not app text | two different layout paths |
| `getSurfaceInfo2 calls=68` == `getPreferredSwizzleMode2 calls=68`, against `rbgpu-res calls=5458` | the kernel lays out 68 surfaces; clients create 5458 |
| `modes=00000001` | every kernel-side surface is mode 0 (LINEAR) under the flag |
| `IOAccelDevice2::get_config` is a 0x40-byte struct with **no geometry** (2lh) | userspace does not learn the tiling config from the kernel |
| `dcc calls=0` with `hk=1` | DCC metadata excluded |
| `a0=242` — the chip's own `GB_ADDR_CONFIG`: 4 pipes, 4 packers, 1 SE | kernel AddrLib geometry is **correct** |
| rotation remap 27→25, `remapped=9`, no visual change (2lc) | rotation excluded |
| compute PASS, bit-exact, 4096 elements | the shader cores and the memory path are fine |
| the photo: correct window chrome **and** scrambled glyphs in one surface | not a global layout parameter |

**2lg/2lh concluded "computed in userspace, out of this plugin's reach".
2lk REFUTES that.** 2lh read one channel — `IOAccelDevice2::get_config`, a
0x40-byte struct with no geometry — and generalised. There is a second
channel, and it carries exactly the geometry AddrLib needs:

```
AMDRadeonX6000_AMDAccelDevice::getHardwareInfo(_sAMD_GET_HW_INFO_VALUES*)
  0xbe47ca4   memcpy(vals, (hw->0x1a38)->vtable[0x1c0](), 0x204)

gfx10_HwInfoInitData          0x7ffb08cb4218  movl 0xa0(%rsi), %r9d
  (userspace)                 0x7ffb08cb421f  movl %r9d, 0xa4(%rdi)
amdMtl_InitAddrLibInterface   0x7ffb08c9409f  chipFamily   = hwInfo->0x00
                                              chipRevision = hwInfo->0x10
amdMtl_HWL_InitAddrLibHwRegFields
                              0x7ffb08bfde4d  regValue.gbAddrConfig
                                                = hwInfo->0xa4
  -> AddrCreate -> Addr2ComputeSurfaceInfo -> every client texture
```

**The swizzle equation every client texture uses comes from one dword:
`_sAMD_GET_HW_INFO_VALUES + 0xa0`**, copied verbatim with no transformation
from a struct this plugin can reach. The struct's fields are named by Apple's
own validation strings (0xbf3deda, 0xbf3df56):

```
+0x00  uint16  deviceID            +0xc0  uint64  refClkFreq
+0x08  uint32  (-> chipFamily)     +0xc8  uint64  sysClkFreq
+0x18  uint64  numRasterPipe       +0xd0  uint64  memClkFreq
+0x20  uint64  numShaderPipes      +0xd8  uint64  cgRefClkFreq
+0xa0  uint32  GB_ADDR_CONFIG
```

Apple **rejects its own record** with `0xE00002BC` if any of those is zero,
which makes this one boot answer a second question too: `gfx_min = gfx_max =
0` has been standing since 2di, so `sysClkFreq = 0` is live and would mean
userspace never gets a valid record at all.

**Boot 102 measured it, and the geometry is RIGHT on both sides.**

```
rbgpu-hwi = b=102 hk=1 calls=40 null=0 ret=00000000 dev=1681 fam=00000028
            ras=4 shd=6 gbac=00000242 hw=00000242 MATCH(hypothesis dead)
            | ref=10000 sys=20000 mem=20000 cg=10000
```

`gbac = hw = 0x242` — userspace receives the chip's own `GB_ADDR_CONFIG`.
The fallback lead (`chipFamily`/`chipRevision`) died statically in the same
step: `gfx10_HwInfoInitData`'s family selection is a `cmpl N / ja` **range**
cascade, and `kernel->0x08 = 0x28` takes the `<= 0x31` arm, which writes
`chipFamily = 0x8f` — AddrLib's `FAMILY_NV`, exactly what 2kz measured the
kernel's own AddrLib getting. All four inputs agree:

| input | kernel (2kz) | userspace (2lk) |
|---|---|---|
| chipEngine | 0xd | 0xd (hardcoded) |
| chipFamily | 0x8f | 0x8f (immediate) |
| chipRevision | 0x28 | 0x28 |
| gbAddrConfig | 0x242 | 0x242 |

**There is no AddrLib configuration difference anywhere in this stack.**

Settled for free: `ret=00000000` means Apple **accepts** its own record and
all four clocks are nonzero, so `gfx_min = gfx_max = 0` (2di) does not reach
this struct — that path is closed. And `ras=4 shd=6` is the 680M's real
topology (6 WGPs = 12 CUs), not Navi 21's.

**That is the fourth global-parameter hypothesis to die** — 2kx tiling
params, 2la kernel AddrLib geometry, 2lc rotation, 2lk userspace geometry —
and all four were excluded in advance by the same photograph: correct chrome
and scrambled glyphs *in one surface*. The frontier is not a global layout
parameter and never was.

Tooling, new and reusable: `tools/re/dyldcache.py` reads any image out of the
dyld shared cache (no Xcode on this machine, no `dyld_shared_cache_util`) and
writes a compact Mach-O `objdump` reads at the cache's own VM addresses;
`tools/re/mtldis.sh` disassembles one symbol by regex.
`build/re/mtl-syms.txt` holds all 20 282 symbols.

### Three hypotheses already refuted — do not re-run them

Each aimed at a *global* layout parameter, which the photo had already
excluded. This is the recurring error in a new dress: **failing to read the
evidence already in hand before designing the next experiment.**

- **2kx** tiling params — correct.
- **2la** AddrLib geometry (kernel side) — correct.
- **2lc** rotation — no effect.

### The next step

Both AddrLibs agree, so a client texture is written by the CPU and read by
the GPU through the **same** equation. A self-consistent surface cannot
scramble from a layout mismatch. So either the surfaces that scramble are not
laid out through AddrLib at all, or they are laid out correctly and corrupted
by something else — the glyph atlas's producer, a blit/compute path's own
addressing, or DCC metadata on a render target (`dcc calls=0` excluded DCC
for the 74 **kernel** surfaces only, never for client ones).

**A userspace observation hook cannot exist.** Lilu's user patcher offers
only `BinaryModInfo`/`BinaryModPatch` — a byte find/replace, with
`onProcLoad`'s callback firing once per binary load in the kernel, never per
call in userspace. Userspace can be **forced**, not **read**.

So observation moved kernel-side, to where userspace's choice arrives anyway:

- **2ll** (built, observe-only, this boot): `res + 0x1dc` bits 0:4 are a
  client resource's swizzle mode, read off Apple's own
  `andl $0x1f` at `be569d8` in `AMDAccelResourceAddr2::initialize`. A
  histogram over all ~4287 resources, in note `rbgpu-mode`. It rides **inside
  the existing 2le hook**, because a second route on an already-routed
  function panics Lilu (2kn).
- **2lm** (built, **unarmed**): force `amdMtl_HWL_DetermineSwizzleMode`
  (0x7ffb08d35a11) to return LINEAR — the userspace counterpart of
  `-rbgpulinear`. The return value *is* the mode (all nine call sites pass
  `%eax` on as one), the `uint&` is a separate out-field at `rsp+8` on entry,
  and the find pattern is 32 bytes because 14 matches **2099** functions in
  that driver. Scoped to Terminal alone, since a wrong stub in a system Metal
  driver costs the login session. Behind `-rbgpumtllinear`, OUT by default.

**Boot 104 settled it with `-rbgpulinear` OUT: the histogram SPREADS**, so
`res + 0x1dc` is genuinely per-resource.

```
rbgpu-mode = b=104 seen=4788 bad=0 | m0=4598 m2=8 m22=125 m27=57 | lin=2
rbgpu-si   = b=104 calls=83 forced=0 already=0 modes=08400004
```

`0x08400004` is bits 2, 22 and 27 — exactly the histogram's non-zero modes,
from an independent hook. **190 of 4788 client resources are tiled**: 125 in
mode 22 (4KB_D_X), 57 in mode 27 (64KB_R_X), 8 in mode 2. The other 4598 are
linear with nothing forcing them.

This also **corrects boot 103's writeup**, which argued `-rbgpulinear` could
not reach this field because 2la's hook fired 70 times against 12224
resources. Boot 103 recorded *zero* non-`m0` out of 12224; at boot 104's
proportion ~500 tiled should have appeared. So the arg does drive client
resources linear, and `vt[0x1b8]` reads a mode the align manager **cached**,
which those 70 calls set and many resources share. A call-count mismatch is
not proof of independence when an unexcluded cache sits between.

**The fix shape improves:** the proper fix touches ~4% of resources and keeps
tiling everywhere else, instead of forcing all 4788.

`-rbgpulinear` is currently **OUT**. Whether it is still earning its place
needs the screen, not a note — it bought ~80 % in boot 98, before the halt
fix was complete at three sites and before faults reached zero, and has never
been retested on a healthy chip.

---

## 3. THE FIX THAT MADE THE DESKTOP WORK (2ks/2ku/2kw)

Apple's `AMDHWMemory::adjustVRAMAddress` (0xbe8cb90, nine instructions)
returns `mc - vram_start`. The GPUVM walker needs
`mc - vram_start + vram_base_offset` — Linux's `amdgpu_gmc_vram_mc2pa`.
`-rbgpuvramadj` adds that term, **only** for callers inside three named
functions:

| site | what it programs | boot 93 |
|---|---|---|
| `AMDHWVMContext::mapVMPT` (0xbe8e624) | page-table tier entries | `map=12086` |
| `AMDHWVMM::assignVMID` (0xbe91376) | `info->0x18`, the PD base in the invalidate request | `asg=11` |
| `AMDGFX10HIQHWChannel::fillMapProcessPacket` (0xbec7de6) | the PD base in the MES `PM4_MES_MAP_PROCESS` packet | `mes=29` |

`calls=12126 inmap=12126 fixed=12126`, no out-of-scope callers.
**The third site matters because the MES hands out its own VMIDs** and
`assignVMID` never sees them — `vmids` went `000e` → `01fe`.

The chain it closed: `allocVMBlock` hands out a page-table block as an MC
address → `mapVMPT` strips the MC base through `adjustVRAMAddress` →
`getPDEValue(level, 0)` adds **flags only** → the entry carries a
carve-out-relative address short by `0x7_8000_0000` → the walk fails → CPF
cannot fetch the IB → the CP's engines park at `CP_ME_CNTL = 0x15000000` (the
power-on value, never a write) → the timestamp never retires →
`resetHardwareAndReplay` waits forever → WindowServer blocks on its mutex →
freeze.

Boot 93, every indicator clean:

```
rbgpu-fault = c=0 f=0 r=0 o=0 fw=0      vmfc=1ffc (reporting fully on)
rbgpu-mew   = n=24 NEVER HALTED 0.5-12s
rbgpu-ring  = r==w at all phases, w advancing
```

**2em and 2ki are retroactively explained**: both were undoing this one
subtraction downstream, after the fact. 2ki (`-rbgpupdefix`) is now harmful —
it composes with the upstream fix (`87b000000 -> ffb000001`, boot 91) and
brings the halt back. It must stay OUT.

---

## 4. The display half

`AMDFramebuffer` matches `IOProviderClass = AtiFbStub`. No `AtiFbStub` is ever
published, because no controller personality matches our device:

```
AMDRadeonNavi21Controller  IOPCIMatch 0x73A01002 0x73A21002 0x73A31002
                                      0x73AB1002 0x73AE1002 0x73AF1002 0x73BF1002
ours                                  0x16811002
```

Chain: **controller → `AtiFbStub` → `AMDFramebuffer` → DCN.** `AMDSupport` has
no children; no framebuffer kext is loaded.

**Spoofing `device-id` does NOT work** (2lj, boot 101). `-rbgpufbmatch`
injected `device-id = 0x73A0`; HWServicesNavi matched, and **the accelerator
never instantiated**. This plugin works by making Apple **ACCEPT** `0x1681`
(`deviceTypeTable`, `DeviceCapabilityTbl`, the TTL spoof's `dev_id == 0x1681`
gate); reporting another id bypasses every one of those patches. It also
produced no `AtiFbStub`, so it bought nothing. **Must stay OUT.**

The route that remains is making Apple's *personality* accept `0x16811002`.
**Unexamined prerequisite:** whether a personality with
`IOClass = AMDRadeonX6000_AmdRadeonControllerNavi21` is reachable from an
EFI-loaded kext at all — `AMDRadeonX6000Framebuffer.kext` lives in
`SystemKernelExtensions.kc` and may need `OSBundleLibraries` that cannot link.
**Establish that before spending a boot.** If it cannot link, this is an
OpenCore-level personality patch, not this plugin's job.

The 7 MB is cosmetic: Metal reports `recommendedMax = 4096 MB`; System
Profiler reads `IOFBMemorySize = 8294400` (1920×1080×4) from the boot
framebuffer.

---

## 5. Verified on hardware — do NOT re-derive

- **The chip.** GC 10.3.3, DMU 3.1.3, MMHUB/ATHUB 2.4.0, MP0/MP1 13.0.3,
  SDMA 5.2.3 (**one instance**), VCN 3.1.1. 1 SE × 2 SA × 3 WGP = 12 CUs.
- **Carve-out.** MC `0xF4_0000_0000`, system-physical `0x7_8000_0000`
  (`GCMC_VM_FB_OFFSET` = 0x780), BAR0 256 MB at `0x8_8000_0000`.
  Apple's GPUVM space is `0xFF_BF..`. `mc2pa(mc) = mc - vram_start +
  vram_base_offset`.
- **seg0 = 0x1260** for GC `BASE_IDX 0`, agreed four independent ways;
  seg1 = 0xa000. Byte address is `(seg + dword_offset) * 4`. Apple's encoded
  register ids are `(dword offset << 32) | 11`, 11 = GC_HWID.
- **The accessor slots** on `pm4+0x30` / `hw->0x368`: `[0x148]` resolves an
  encoded id to a dword offset, `[0x118]` READS it, `[0x120]` WRITES it.
- **The hw engine array** is `hw + 0x3b0 + engine*8`: `[0]` PM4/GFX,
  `[1..4]` the four SDMA engines. The SDMA *walk* bases at `hw + 0x3b8`, so
  `engine[i]` is hw index `i + 1`.
- **`GB_ADDR_CONFIG`** is dword `0x13de`, BASE_IDX 0; this chip reads `0x242`.
  Fields per `gc_10_3_0_sh_mask.h`: NUM_PIPES[2:0], PIPE_INTERLEAVE_SIZE[5:3],
  MAX_COMPRESSED_FRAGS[7:6], NUM_PKRS[10:8], NUM_SHADER_ENGINES[21:19],
  NUM_RB_PER_SE[27:26].
- **`AddrSwizzleMode`**: 0 = LINEAR, 9 = 64KB_S, 22 = 4KB_D_X, 25 = 64KB_S_X,
  27 = 64KB_R_X. Apple's `getAddrSwizzleMode` table (0xbf4efa0) is identity
  for supported modes, `0x21` = unsupported.
- **`getPDEValue` returns FLAGS ONLY** (2ko): over 108 calls,
  `L0 f=2000000000000001`, `L1 f=100000000000001`, address bits 47:12 zero.
  The address is ORed in by the caller. Never hook it to correct an address.
- **`BOOTLOAD_COMPLETE = 1`** since 2jq: Apple's `_gc_10_3_get_fw_constants`
  asks for 21 firmware entries and 2aj supplied 7, so the RLC had been booting
  **Navi 21's** `rlc_lx6_iram`/`rlc_lx6_dram`. `-rbgpurlcsub` supplies
  yellow_carp's own as sub-image ids 26/27/28/29/30. Never remove it.
- **`powerUpHW` succeeds**, eight consecutive boots.
- **Request 0x1B completes** (2ha): its `0xE00002C7` is set on the *success*
  path at 0xbe418cf. Both failure paths return `0xE00002D8`.
- **Metal works.** `MTLCopyAllDevices` returns 1 device; `newCommandQueue`
  succeeds; a runtime-compiled 4096-thread kernel returns correct results.
- **`-rbgpuforceenable`** sets `fEnabled` (bit 0x2 at `accel+0xc78`), whose
  only setter is `IOGraphicsAccelerator2::enableAccelerator()` — unreachable
  because 2ct refuses the display pipe, which is why every armed boot was
  black before 2hh.

### Key addresses (AMDRadeonX6000 unless noted)

```
0xbe8cb90  AMDHWMemory::adjustVRAMAddress        <- THE FIX SITE
0xbe8e624  AMDHWVMContext::mapVMPT
0xbe91376  AMDHWVMM::assignVMID
0xbec7de6  AMDGFX10HIQHWChannel::fillMapProcessPacket
0xbe9b9de  AMDGFX10VMM::getPDEValue              (flags only)
0xbe99342  AMDHWAlignManager2::init
0xbf4efa0  getAddrSwizzleMode table
0xbea1190  AMDGFX10PM4Engine::doStart
0xbea8ec9  powerUpHWEngines      walks hw+0x3B0, 11 entries, vtable[0x138]
0xbea8fea  startHWEngines        walks hw+0x3B8, hw->0xc2*2+2, vtable[0x148]
0xbeca1f0  Navi21Hardware::allocateHWEngines
0xbe85536  waitForHwStamp
0x126b002e _gc_get_tiling_params_10_1            (HWLibs; reads GB_ADDR_CONFIG)
0x14e20282 IOAccelDevice2::get_config            (IOAcceleratorFamily2)
```

Extracted kexts in `build/re/`: `AMDRadeonX6000`, `AMDRadeonX6800HWLibs`,
`AMDRadeonX6000HWServicesNavi`, `IOAcceleratorFamily2`, plus (from
`SystemKernelExtensions.kc`, not the boot collection) `AMDFramebuffer`,
`AMDSupport`, `AMDRadeonX6000Framebuffer`.

---

## 6. Open, unexplained, owed

- **`rbgpu-vmid` reads `av=N fail=N` equal** — every `assignVMID` reports
  failure by the hook's own criterion while VMIDs are plainly handed out.
  `fail` has never been read against the code that sets it. The 2ha rule
  applies: a return code is not a result until the code that sets it has been
  read.
- **`gfx_min = gfx_max = 0`** since 2di: the SMU reports no GFX clock range,
  GFX DPM never started. 2hl measured the CP clocked and executing, so this is
  **not** the mechanism of anything currently broken. Why it reads zero is
  still open.
- **The diag table is full at `d=73`** — new `rbgpu_diag_set_num` entries
  silently do not appear. Values that matter must ride an NVRAM note.
- **MMHUB's fault status register reads all-ones** on this chip, and Apple's
  `getVMFaultInfo` fills its struct whenever `(addr != 0 || status != 0)`, so
  Apple reports a **phantom hub-1 fault** on every poll. Never treat an Apple
  hub-1 fault report on this chip as real. (Downstream of the fault stream;
  gone since 2kw, `fw=0`.)
- **The peer's `/options` lookup is tried once** (`IORegistryEntry::fromPath`).
  On a boot where the peer runs early (0.52 s vs the usual 1.9 s) it loses the
  race, logs `/options not found`, and silently does not arm —
  indistinguishable from the user forgetting. `rbgpu-arm` stays set, so a plain
  reboot recovers. A bounded retry is owed. [2gy]
- **A panic can lose an NVRAM note** whose nonce would otherwise make it
  readable (2gg), and a note's write can fail to survive a power cycle (2gc).
  Cause unknown.

### Bypasses never justified

- **2am / 2an / 2ao** — `_gc_fw_autoload_is_completed`,
  `_gc_check_register_equal_ext` (RLC safe mode) and
  `_sdma_5_2_1_fw_autoload_is_completed` forced true, blanket. 2jq largely
  explains the first (the firmware tables were wrong, not the latch), and 2iv
  found `RLC_SAFE_MODE` genuinely stuck with Linux's own exit sequence unable
  to clear it across 1000 polls. 2an is **not** vindicated.
- **2cc** — `AMDVCN2EncChannel::allocateMemoryResources` stubbed true.
- **2be** — the accelerator's `stop()` patched to RET; the trap-3 guard it
  silences was never understood.
- **2ct** — the display pipe refused outright. `-rbgpudisplaypipe` (2kt) now
  adopts it instead; 2ct's premise died when `powerUpHW` began succeeding.

### Not started

VCN, GFX DPM, DCN 3.1.2 proper, the real framebuffer.

---

## 7. The recurring error — read before concluding anything

**Twenty-six recorded instances.** The shape is always the same: *a value is
read without first establishing what would make it meaningful*, or *absence is
read as a result*, or *a table is built from n=1 rows*. The 2kx/2la/2lc trio
above is the most recent, and is the variant worth naming:

> **Read the evidence already in hand before designing the next experiment.**
> All three experiments tested a *global* layout parameter that the user's
> photograph — correct chrome and scrambled glyphs in the same surface — had
> already excluded.

### The rules in force

1. **Say in the code what would have to be true for a reading to mean
   anything, and log it when it is not.**
2. **Absence is not a result.** Every path out of a probe writes a record
   naming which path it took. A silent `return`, a full diag table and a
   missing NVRAM write all look identical to "the code never got there".
3. **A measurement behind a varying gate inherits the variance.** Put probes
   where the code always reaches.
4. **A diagnostic that substitutes its own value must never narrate that value
   as a hardware result.** [2cy, which cost fifteen milestones]
5. **An overwrite NVRAM variable is this boot's only if `b=` is new.** [2gd]
6. **One variable per boot.**
7. **The instrument is a suspect.** When a measurement and a suspect are both
   new, remove the suspect rather than reasoning about it.
8. **A return code is not a result until the code that sets it has been
   read.** A named constant tells you what the NAME means, not what the
   function meant by it. [2ha]
9. **A stale note is no evidence about where a boot got to** — not weak
   evidence of an early stop. A user rebooting a black screen looks exactly
   like a hang. [2gw]
10. **A NULL check is not a pointer check.** Validate derived pointers as
    kernel VAs (`>= 0xffffff8000000000`) before dereferencing. [2gh]
11. **When a fix moves upstream, REMOVE the downstream patch it replaces, in
    the same change.** "Redundant" and "inert" are different things. [2kv]

### Routing Apple's code — five rules, each paid for on hardware

1. **Never route a name unless `nm -m build/re/<kext>` shows it DEFINED
   there.** `solveSymbol` does not skip imports; it returns the bare slide and
   `routeMultiple` writes a jump there. [2c]
2. **Disassemble the first 16 bytes and refuse if a rel8/rel32 branch begins
   within the first 5.** Lilu copies displaced instructions byte-for-byte into
   a trampoline, so a short branch keeps its original displacement and lands in
   Lilu's own buffer. [2gb, "rule 9"]
3. **Grep the mangled symbol across the whole plugin before adding a route.**
   A second `RouteRequest` for an already-routed function **panics the machine
   outright** — Lilu finds the first route's `E9` jmp and refuses. A new
   transform goes **inside the existing hook**. [2kn]
4. **`RouteRequest::from` is NOT written back** by Lilu 1.7.3, despite
   `kern_patcher.hpp` documenting it as such. Call `solveSymbol` yourself.
   [2kp]
5. **Cross-check every hand-written mangled name against `nm`.** Three length
   prefixes were wrong in one change; each would have made `solveSymbol`
   return 0 and silently unscope the fix. [2kr]

vtable dumps: **call offset = dump offset − 0x10** (the address point is
symbol + 0x10). A *non-virtual* call through a vtable **symbol** uses dump
offsets directly. [2gp]

---

## 8. Procedure — who runs what

**the assistant never runs `efi.sh`, `load.sh` or `unload.sh`, and never asks for the
password.** the assistant may commit at any time without asking; **pushing needs
asking**. **Loading or unloading a kext needs asking.** Deliberate diagnostic
panics are off the table (user's decision, 2026-10-12). Adding an SMU message
group is the user's decision — say what it changes on the chip.

**License constraint:** NootedRed and NootRX are under the ChefKiss Open
Knowledge License v1, whose §2 and §3 forbid AI reading, summarizing,
analyzing or deriving output from the work. **Do not read or analyze them.**
Findings from those projects must come from the user in their own words.

### Build

```
make plugin          # the Lilu plugin. `make kext tools` does NOT rebuild it.
make hosttest        # must pass before any load
dwarfdump --uuid build/out/RembrandtGPU.kext/Contents/MacOS/RembrandtGPU
```

**Always check the UUID moved.** Two boots have been lost to a stale build.

### Arm a boot

```
sudo tools/efi.sh install && sudo tools/efi.sh status
sudo tools/efi.sh on && sudo nvram rbgpu-arm=1 && sudo reboot
```

**`rbgpu-arm` is one-shot** — the peer consumes it and deletes it. Every armed
boot needs it set again, and an install that only replaces the kext and edits
boot-args does **not** arm. After a panic, check `nvram rbgpu-arm` anyway
(2gg found it still at 1).

### Read a boot — in the ARMED boot, before any reboot

```
sudo tools/plugin_check.sh
```

It writes `logs/<timestamp>-plugin/` to **disk**, which survives the power
cycle; live NVRAM read after a reboot does not. Boots 57/58/59 each wrote an
early note and lost every late one, and three experiments were spent blaming
the code. The user cannot run the assistant from an armed boot, so the capture has to
be a file. **Look in `logs/<timestamp>-plugin/` before reading live NVRAM.**
A 0-byte `pci.txt` means the IGPU registry subtree was locked — the hard-wedge
fingerprint.

### Before interpreting ANY boot

```
nvram rbgpu-boot                        FIRST. If it has not increased since
                                        the last capture, the boot was never
                                        armed and NOTHING in NVRAM is this
                                        boot's. `rbgpu-diag` carries no nonce,
                                        so "armed=1 reads live" certifies a
                                        STALE capture. [2iz]
kmutil showloaded | grep rembrandt      vs dwarfdump --uuid build/out/...
nvram boot-args                         which flags were ACTUALLY set
ls -lt /Library/Logs/DiagnosticReports/*.panic | head
grep -o 'com.rembrandtgpu.plugin(1.0)\[[0-9A-F-]*\]' <the .panic>
                                        WHICH BUILD panicked — not your memory
                                        of it.
```

### Rescue

`-rbgpuoff` in boot-args: the kext still loads, no peer instance exists.

### NVRAM channels (the ones that matter now)

```
rbgpu-boot    per-boot sequence counter; every note is prefixed b=<n>
rbgpu-arm     one-shot arming
rbgpu-adj     the mc2pa fix: calls / inmap / fixed / out-of-scope callers
rbgpu-fault   Apple's own VM fault records (c/f/r/o/fw)
rbgpu-ring    ring pointers, CP_ME_CNTL, GRBM_STATUS at three phases
rbgpu-surf    getSurfaceInfo2 / swizzle modes
rbgpu-res     IOAccelNewResourceArgs dumps
rbgpu-si      the surface-info rewrite counters
rbgpu-addr    GB_ADDR_CONFIG, decoded
rbgpu-disp    the display-pipe adoption
rbgpu-diag    the diag summary — FULL at d=73, carries no nonce
```

---

## 9. Boot-args — what must stay IN and OUT

**IN**, each proven:

```
-rbgpusync570 -rbgpuspoofttl -rbgpupsphw     the TTL/PSP path
-rbgpusmubringup -rbgpuptbase                the SMU link and the GART fix
-rbgpustampwait                              the bounded (2en) real wait
-rbgpunogates                                the configuration that works
-rbgpusdmaone                                what makes powerUpHW succeed
-rbgpuvmpteng1                               engine-2 requests -> engine 1,
                                             the one SDMA instance (2gw)
-rbgpuforce1b                                sends request 0x1B
-rbgpurlcsub                                 what makes the RLC autoload
                                             COMPLETE (2jq) — never remove
-rbgpunocgcg                                 CGCG off; takes, but NOT a fix
-rbgpumewatch                                the timed CP/RLC/GFXOFF samples
-rbgpuforceenable + wdt=-1                   the accelerated display.
                                             NEVER arm the first without
                                             the second — without it the
                                             frozen boot panics at 120 s with
                                             no SSH window (2hh)
-rbgpuvramadj                                2ks/2ku/2kw: THE FIX
-rbgpudisplaypipe                            2kt: adopt the display pipe
(-rbgpulinear was REMOVED before boot 104 to settle 2ll; it is a
 blanket force on all ~4788 client resources to correct the ~190
 that are tiled. Re-add with:
   sudo tools/efi.sh bootarg add -rbgpulinear && sudo nvram rbgpu-arm=1)
```

**OUT**, each proven:

| flag | why |
|---|---|
| `-rbgpufbmatch` | spoofs `device-id`; **kills the accelerator** (2lj) |
| `-rbgpupdefix` | 2ki; composes with the upstream fix, halt returns (2kv) |
| `-rbgpunodisppanic` | publishes `IOPCITunnelled`, costs the UI (2cv) |
| `-rbgpusdmahook` | routes a function whose prologue breaks rule 9 (2gb) |
| `-rbgpustampraw` | Apple's unbounded wait; wedged the machine (2en) |
| `-rbgpuregdump` | the GC register dump stalls non-deterministically (2fe) |
| `debug=0x100` | halts on the panic screen instead of writing a report |

`wdt=-1` disables Apple's **userspace** watchdog — read, not assumed:
`IOWatchdog::start` tests bit 0 of `wdt`, logs "watchdog disabled by boot-arg"
and zeroes `+0x89/+0xa8/+0xac`. It changes nothing on the chip.

---

## 10. Where to go next

1. **Readable text (the user's stated priority).** §2: the AddrLib
   configuration is now proven identical on both sides, so the next step is a
   userspace *observation* of what client textures actually get — not another
   global parameter.
2. **The display chain.** §4: answer the linkability question **statically**
   before spending a boot. If `AMDRadeonX6000Framebuffer` cannot be linked
   from an EFI-loaded kext, this becomes an OpenCore personality patch and
   leaves this repo.
3. **The `av=N fail=N` debt.** §6: one disassembly, no boot.
4. **`-rbgpulinear` is already dropped** (boot 104). Re-add it only if the
   screen says it still earns its bandwidth.
