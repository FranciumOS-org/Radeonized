# Apple's AMD Navi stack on macOS 15.8.1, measured against the 680M

Static analysis, 2026-10-04. Nothing here was loaded. Binaries come from
`/System/Library/KernelCollections/SystemKernelExtensions.kc` of this
machine (macOS 15.8.1, 24H32; AMD kexts 7.0.25). The hardware facts come
from the probes recorded in CLAUDE.md, the Linux facts from torvalds/linux
(docs/linux-ref/UPSTREAM.md).

Status marks: **have** = Apple's code already covers the 680M's version;
**adapt** = Apple's code exists for a neighbouring version and Linux gives
the difference; **missing** = nothing to start from in Apple's code.

## How to reproduce

```bash
KC=/System/Library/KernelCollections/SystemKernelExtensions.kc
python3 tools/re/kc_extract.py $KC --list | grep -i amd
python3 tools/re/kc_extract.py $KC com.apple.kext.AMDRadeonX6800HWLibs build/re/AMDRadeonX6800HWLibs
python3 tools/re/dumptable.py build/re/AMDRadeonX6800HWLibs _vm_ip_version_mapping
xcrun llvm-objdump -d --no-show-raw-insn build/re/AMDRadeonX6800HWLibs
```

`kc_extract.py` rebuilds a fileset entry as a standalone Mach-O with its
symbols; `machoread.py` decodes the collection's chained pointers
(target = low 30 bits); `dumptable.py` prints a data symbol word by word.

## The stack

| Kext | Role | Symbols |
|---|---|---|
| AMDRadeonX6000HWServices | matches the PCI IDs, reads the board's "project name", loads one HWLibs plug-in | 429 |
| AMDRadeonX6800HWLibs (one of 7) | AMD's shared hardware library: IP init, firmware, PSP, power management | 12171 |
| AMDRadeonX6000Framebuffer | display: AMD's DAL/DC and DMUB, IOFramebuffer | 3314 |
| AMDRadeonX6000 | the accelerator: GFX10 rings, VM, SDMA, Metal back end | 6048 |
| AMDRadeonX6000MTLDriver (user space) | Metal; emits gfx10.3 PM4 | not examined |

HWLibs choice: `AMDRadeonX6000_AMDRadeonHWServicesNavi::ProjectNameTable`
maps the board's `ATY,<name>` to a plug-in:

| Project name | Plug-in |
|---|---|
| ATY,Belknap / ATY,Carswell | **Load6800 (Navi 21)** |
| ATY,Henbury | Load6810 |
| ATY,Sunbeam | Load6300 |
| ATY,Ikaheka / ATY,Keelback | Load6200 |
| ATY,Python / ATY,Adder | Load6700 |
| ATY,Boa | Load6100 |

## Findings

### HWLibs (AMDRadeonX6800HWLibs)

It is the same kind of code as Linux's amdgpu: per-IP blocks dispatched by IP
version. `_vm_ip_version_mapping` is keyed `major | minor<<16 | rev<<32` and
has its own entry for GC 10.3.3.

| Item | Status | Evidence / what to do |
|---|---|---|
| GPU VM for GC 10.3.3 | **have** | `vm_10_3_3_sw_init`, `vm_10_3_3_hw_init`, `vm_10_3_3_event_notification` |
| MMHUB 2.3 golden registers | **have** | `mmhub_2_3_0_initialize_registers` (Linux drives MMHUB 2.4 with v2.3) |
| DMCUB for DCN 3.1 | **have** (constants) | `dmcub_dcn31_get_fw_constrant`; whether a DCN 3.1 DMCUB image is present is open |
| SDMA 5.2.3 firmware slot | **have** (slot) | `sdma_5_2_3_ucode`: 0x8400 bytes, same size/format as Linux `yellow_carp_sdma.bin` but a different build (25-31 % equal dwords); Linux's image can be supplied |
| GC firmware (PFP/ME/CE/MEC/RLC) | **missing** | only `gc_10_3_*_ucode` (Navi 21). Supply `yellow_carp_{pfp,me,ce,mec,mec2,rlc}.bin`, as NootedRED does for Renoir through `populateFirmwareDirectory` |
| GC golden settings for 10.3.3 | **missing** | only `gc_10_3_initialize_registers` and `gc_10_3_1_...`; Linux has `golden_settings_gc_10_3_3` (gfx_v10_0.c) |
| PSP | **adapt** | only `*_11_0` code. On this APU the PSP's secure OS is already running, so only the ring/command path matters, and its registers are identical in v11 and v13 (C2PMSG_64/69/70/71, _81): Linux `psp_v13_0_ring_create` bare-metal path = v11's |
| SMU / power management | **adapt (largest HWLibs item)** | `smu_11_0_7` (Navi 21 dGPU) and `PhwRenoir` (SMU 12 APU) exist; nothing for SMU 13. The message set to implement is Linux `yellow_carp_ppt.c`; the probe confirmed driver interface 4 |
| Device / caps tables | **missing (small)** | `CAIL_ASIC_CAPS_TABLE` (20 KB) and `deviceTypeTable` list Navi 1x/2x only, no `0x1681`; NootedRED patches the same tables for Renoir |
| IP base addresses | **open** | HWLibs enumerates IPs itself (`create_discovery_tbl` -> `hwip_discover_ver1/2/3`, `enumerate_rsmu_table_ver3`) with built-in base arrays (`_GC_BASE`, `_MMHUB_BASE`, ...). Several match this chip's table exactly (DCN, NBIO, VCN, FUSE, THM), others differ in layout; whether they are rewritten at run time is not established |

### Accelerator (AMDRadeonX6000)

Mostly generic `AMDGFX10*` classes (GFX10 hardware, PM4 engine, SDMA,
VMM, GART, rings); Navi 21 adds four subclasses.

| Item | Status | Evidence / what to do |
|---|---|---|
| Command processor, rings, PM4, SDMA engine | **have** | GFX10 classes; the 680M is gfx10.3 like Navi 21 |
| GC hub | **have (likely)** | `AMDGCHub_10_1_0 ... 10_3_0, 10_3_4`; Linux uses one gfxhub_v2_1 for all GC 10.3.x |
| MM hub | **missing** | `AMDMMHub_2_0_0 / 2_0_2 / 2_1_0 / 2_1_1` only; a 2.3 class is needed (Linux `mmhub_v2_3.c`: VM registers, invalidation engine) |
| Shader-engine layout | **adapt** | `AMDNavi21Hardware::setupAndInitializeHWCapabilities`, `setShaderEngineMemConfig`: 680M is 1 SE x 2 SA x 3 WGP (12 CU), wave32 (discovery GC info) |
| Power-up, VM init | **adapt** | `AMDNavi21Hardware::powerUp`, `initializeVmHardware` |
| Memory model | **adapt** | Navi 21 assumes VRAM behind BAR0. Here: 4 GB carve-out at system-physical 0x7_8000_0000, GPU address 0xF4_0000_0000, BAR0 only windows 256 MB. NootedRED's `adjustVRAMAddress` / reserved-VRAM hooks are the pattern |

### Framebuffer (AMDRadeonX6000Framebuffer)

| Item | Status | Evidence / what to do |
|---|---|---|
| DCN 3.1 display code | **missing (largest item overall)** | DAL strings show dce80-120, dcn10, dcn20, **dcn21**, dcn30, dcn302 only (AMD source tree "SunburstE") |
| APU plumbing (ATOM BIOS from ACPI, IGP VRAM info, reserved VRAM, boot watermarks) | **adapt** | NootedRED patches these exact functions in this kext for Renoir (DCN 2.1) |

Options for the display:
1. Run DCN 3.1 on Apple's DCN 3.0 code with register remaps. DCN 3.1 changes the
   DIO, DCCG, clock and power-state handling and leans on DMUB more; high risk.
2. Bring Linux's DC (dcn31 resource, hwseq, DMUB service) into our own
   framebuffer, the way Navi48-MacOS compiled Linux's DCN 4.1 code with a shim
   (`tools/dcn41/linuxshim`). Large but known to work.
3. Keep the firmware's framebuffer for scan-out and accelerate behind it,
   with our own peer for the accelerator. Viable: see the next section.

## Can the accelerator run without Apple's framebuffer? (option 3)

Yes, by design, with a small peer object of our own. Traced in
AMDRadeonX6000 on 15.8.1 (`tools/re/kc_stubs.py` resolves the kernel calls):

- The accelerator does not link against the framebuffer kext; it matches the
  PCI device itself (`IOMatchCategory` IOAccelerator) and talks to its display
  driver through one call: `peer->callPlatformFunction("SpecialAMDKey", true,
  &request, p2, p3, p4)` (the symbol is created in
  `AMDGraphicsAccelerator::start`).
- `configureDevice()` finds the peer with `initLinkToPeer()`: the first client
  of the PCI device whose `IOMatchCategory` is `"ATIFramebuffer"`, else
  `"IOFramebuffer"`. On this machine the second one is the boot
  IONDRVFramebuffer (`IOMatchCategory = "IOFramebuffer"`, checked in ioreg).
- The accelerator supports an NDRV framebuffer peer explicitly:
  `NDRVGetSurfaceInfo`, `reserveNDRVSpace` (keeps the boot framebuffer's
  memory out of its allocator), and it sets `ATY,SurfInfo` /
  `ATY,PageFlipEnable` on the peer.
- `powerUpHW()` has exactly one caller: the accelerator's own
  `callPlatformFunction`, request 6. Apple's `AmdRadeonController::powerUp`
  sends request 6 and then 0x1B ("Controller is enabled, finish
  initialization"; NootedRED: `IOFBRequestControllerEnabled = 0x1B`);
  `AmdPowerPlayHelper::powerUp` sends 3, `wake` 6 and 7, `sleep` 0x11, `stop`
  0x1C (all `messageAccelerator`, vtable slot 0xa00, decoded from the call
  sites).
- The accelerator's own message dispatch has 28 cases (`cmpl $0x1b`); request
  6 reaches `powerUpHW()`, 0x1B the "controller enabled" path.

Navi48-MacOS runs Apple's accelerator this way on real hardware
(`Navi48AccelPeer`), and its notes give the traps:

1. Without a peer that claims `"ATIFramebuffer"`, the accelerator links to the
   generic IOFramebuffer, asks it `SpecialAMDKey` with waitForFunction = true,
   and **blocks forever** (nobody handles the symbol). The peer must exist
   before the accelerator starts.
2. Request 0x14 (isDeviceValid): answer `*p2 = 1`, or the accelerator
   invalidates the device. Request 0x00 (accelerator started, p2 = the
   accelerator): answer success.
3. A failure inside `AMDGraphicsAccelerator::start()` on unsupported hardware
   ends in a page fault in its own error path (stop() clears the event logs,
   the shared epilogue dereferences one). Any experiment must expect that.
4. Navi48 never got request 6 sent, so `powerUpHW()` never ran and they
   rebuilt its tail by hand (enableAllocations, reserveNDRVSpace,
   setVirtualSpaceReady, createBltMgr). Our peer can instead send 6 and 0x1B
   as Apple's controller does, so Apple's own code runs in Apple's order.
5. WindowServer pairs a display with an accelerator through properties
   (`IOAccelFindAccelerator`); Navi48 publishes them on its framebuffer.

So the display question does not block acceleration: the firmware
framebuffer keeps scan-out (fixed mode, 1920x1080), a peer kext answers the
accelerator, and DCN 3.1 can be done later. The critical path becomes the
library layer: HWServices loading X6800HWLibs for 0x1681, then PSP, SMU 13,
GC firmware and golden settings, MMHUB 2.3 and the memory model.

## Start-up path for 0x1681, in order, with what each step needs

Traced statically on 15.8.1 (`tools/re/strxref.py` finds string users,
`kc_stubs.py annotate` names kernel calls). "Patch" = what our kext / plug-in
must do at that point; addresses are this build's.

### A. Getting Apple's kexts to start at all

| # | Step in Apple's code | What happens on a 680M today | Patch |
|---|---|---|---|
| A1 | HWServices and the accelerator match `IOPCIMatch` lists plus `IOPropertyMatch` `LoadHWServices` / `LoadAccelerator` = true | 0x1681 is in no list; nothing sets the properties | personalities for `AMDRadeonX6000_AMDRadeonHWServicesNavi` and `AMDRadeonX6000_AMDNavi21GraphicsAccelerator` matching 0x16811002; set `LoadHWServices`, then `LoadAccelerator` on the PCI device (Apple's GPU wrangler does it in that order, `AmdGpuWrangler::vendor_doDeviceAttribute`) |
| A2 | `HWServicesAbstract::readProjectName` reads the PCI device's **`@0,name`**, `getMatchProperty` maps it through `ProjectNameTable` | no `@0,name` -> no HWLibs plug-in | `@0,name = "ATY,Belknap"` (-> Load6800 = X6800HWLibs) |
| A3 | the accelerator finds its display peer (`initLinkToPeer`) | would pick IONDRVFramebuffer and block forever | our peer in `IOMatchCategory` "ATIFramebuffer", present before A1's accelerator start; answers 0x00 and 0x14, sends 6 then 0x1B |

### B. HWLibs (X6800HWLibs) bring-up

| # | Step | On a 680M | Patch |
|---|---|---|---|
| B1 | `AmdTtlServices::AmdTtlServices` looks the device ID up in `deviceTypeTable` (0x128dbd60: 84 `{id, type}`; type 6 = Navi 21 0x73a0-0x73bf) | not found: logs "Unable to find matching firmware device type for device ID", continues without a type | add 0x1681 -> 6 |
| B2 | `ipconfig_create` reads `IP_DISCOVERY_VERSION` (0x16a00; reads 2 here) -> `hwip_discover_ver2`: waits for C2PMSG_33 bit 31, reads RCC_CONFIG_MEMSIZE, maps MEMSIZE - 64 KB, checks 0x28211407, `parse_ip_discovery_v2_tbl` | **expected to work**: same method as Linux and our probe; the parser checks the IPDS/HARV signatures and no table version | none expected; verify on hardware (IP table v4, 32-bit bases; DBGU_IO and NBIF have 6 bases, the parser may cap at 5) |
| B3 | `smu_init_function_pointer_list`: `smu_get_hw_version` maps the MP1 version to 0-0x14; only 6/8/9/10 accepted (mask 0x740), all -> `smu_11_0_7_create_function_pointer_list` | MP1 13.0.3 -> **"Unsupported hw version!"**, returns failure | install an SMU 13 function list (the 27 slots of `smu_11_0_7_create_function_pointer_list`: internal sw/hw init, dpm, gfx_off, power_feature_caps, ...), implemented from Linux yellow_carp_ppt.c over the mailbox in rb_smu.c. Largest single item. **Library done (2026-10-05): src/common/rb_smu13.{c,h}**; slot-by-slot spec: docs/SPEC-SMU-SLOTS.md |
| B4 | `psp_asic_type_init` sets the PSP type from the MP0 version; only `mp0_11_0_0_*` exists | MP0 13.0.3 unmapped (exact failure not traced) | map onto the 11.0 paths: the ring registers match v13, and the secure OS is already running, so the bootloader paths are skipped (`mp0_is_sos_alive` reads C2PMSG_81) |
| B5 | `sdma_init_function_pointer_list` compares `major<<16 | minor`, has 5.2 (0x50002) | SDMA 5.2.3 likely accepted; `sdma_5_2_3_ucode` slot exists | supply `yellow_carp_sdma.bin` |
| B6 | GC firmware by device type (only `gc_10_3_*`, Navi 21) | Navi 21 firmware would be loaded into GC 10.3.3 | supply `yellow_carp_{pfp,me,ce,mec,mec2,rlc}.bin` (NootedRED: `populateFirmwareDirectory`, `AmdTtlFwServices::getIpFw`) |
| B7 | GC golden registers (`gc_10_3_initialize_registers`) | Navi 21 values | Linux `golden_settings_gc_10_3_3` |
| B8 | `findDeviceAsicCaps` / `isAsicCapEnabled` look up `CAIL_ASIC_CAPS_TABLE` | no 0x1681 entry | add an entry (NootedRED patches the same table) |

### C. Accelerator (AMDRadeonX6000)

| # | Step | On a 680M | Patch |
|---|---|---|---|
| C1 | `AMDNavi21Hardware::newGCHub` -> `AMDGCHub_10_3_0` | fits: Linux drives all GC 10.3.x with one gfxhub | none |
| C2 | `AMDNavi21Hardware::newMMHub` -> `AMDMMHub_2_1_0` | wrong registers: MMHUB 2.3/2.4 moved every VM register, and not by one shift (invalidation engines regrouped per engine: ENG0 REQ/ACK/SEM 0x138e3/0x138f5/0x138d1 -> 0x1aa01/0x1aa02/0x1aa00) | a `MMHub_2_3` class: `fillVMRegisters`, `getValue_VM_INVALIDATE_ENG_REQ` from Linux mmhub_v2_3.c |
| C3 | `setupAndInitializeHWCapabilities`, `setShaderEngineMemConfig` | Navi 21 layout (4 SE) | 1 SE x 2 SA x 3 WGP, wave32 |
| C4 | memory model: VRAM behind BAR0 | carve-out 4 GB at phys 0x7_8000_0000, BAR0 256 MB | NootedRED's reserved-VRAM / `adjustVRAMAddress` pattern |
| C5 | `start()` failure path | page fault in Apple's own error path (Navi48) | guard while bringing up |

### Open, to settle before writing code

1. Loading: A1-A3 and the B/C hooks are a Lilu plug-in's job. Lilu is
   already loaded on this machine (1.7.3), and the AMD kexts are not loaded
   until A1's properties appear, so a plug-in loaded after boot might still
   see them load; whether Lilu accepts a plug-in that late is to be checked.
   Otherwise the plug-in goes into the EFI, which CLAUDE.md keeps for the
   user to do.
2. B2 and B4 need a hardware run to confirm.

## Summary

The library layer is closer than expected: AMD built Rembrandt-class pieces
(GC 10.3.3 VM, MMHUB 2.3, DCN 3.1 DMCUB constants, SDMA 5.2.3) into the
library Apple ships for Navi 21, and the PSP command path is register
compatible. What has to be written is concentrated in four places:

1. Power management for SMU 13 (Linux yellow_carp_ppt.c), in HWLibs.
2. An MMHUB 2.3 class and the APU memory model, in the accelerator.
3. Firmware and golden settings for GC 10.3.3, plus device/caps table entries.
4. Display: DCN 3.1 does not exist in Apple's framebuffer.

Items 1-3 are NootedRED-shaped work (hooks and patches on Apple's code). Item
4 no longer blocks: option 3 below lets acceleration come first.

## Open questions, in order

1. ~~Does Apple's accelerator need Apple's framebuffer kext?~~ No, with a peer
   (section above).
2. Are HWLibs' built-in IP base arrays overwritten at run time from the
   chip, or used as-is?
3. Is there a DCN 3.1 DMCUB firmware image in HWLibs or the framebuffer?
4. How does HWLibs pick the power-management back end (`PhwRenoir`,
   `PhwNavi21`, `PhwSwSmu`, `PhwDummy`) for a device?
