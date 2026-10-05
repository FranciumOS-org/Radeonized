# A1-A3 in detail: getting Apple's AMD kexts onto the 680M

What it takes for AMDRadeonX6000HWServices and the AMDRadeonX6000
accelerator to attach to the 680M (`1002:1681`) at all, and to talk to a
display peer of ours instead of the boot framebuffer. Read from macOS 15.8.1
(kexts 7.0.25) with tools/re. Addresses are file addresses of the extracted
kexts in build/re; resolve by name.

Nothing here patches Apple code. A1 is property data, A2 one property, and A3
an IOService of our own that speaks Apple's published protocol. The B items
(HWLibs) come after.

## Overview

```
our peer (IOMatchCategory "ATIFramebuffer", on the 680M's IOPCIDevice)
  start():
    set @0,name = "ATY,Belknap"                         A2
    set RBGPULoad, LoadHWServices, LoadAccelerator = true   A1
    pci->registerService()          -> HWServices and accelerator personalities match
accelerator start()
  initLinkToPeer("ATIFramebuffer")  -> finds our peer (A3)
  peer->callPlatformFunction("SpecialAMDKey", false, &0x00 Register, accel, 0, 0)
our peer, after Register (not inside the call):
  accel->callPlatformFunction("SpecialAMDKey", true, &0x06 PowerPlay Ready, ...)   -> powerUpHW()
  accel->callPlatformFunction("SpecialAMDKey", true, &0x1B Controller Enabled, ...)
```

`-rbgpuoff` must leave the peer out entirely. With no peer, nothing sets the
properties, and Apple's kexts never match the 680M.

## A1: matching

Apple's matching entries need two things: the device ID in `IOPCIMatch`, and a
boolean property on the IOPCIDevice (`IOPropertyMatch`):

| Kext | IOClass | IOMatchCategory | IOPropertyMatch | Apple's IOPCIMatch |
|---|---|---|---|---|
| AMDRadeonX6000HWServices | `AMDRadeonX6000_AMDRadeonHWServicesNavi` | AMDRadeonX6000HWServices | `LoadHWServices` = true | Navi 10/12/14/21/23 IDs |
| AMDRadeonX6000 | `AMDRadeonX6000_AMDNavi21GraphicsAccelerator` | IOAccelerator | `LoadAccelerator` = true | 0x73A2/AB/BF/A3 |

**Who sets the properties on a real Mac:** the GPU wrangler. Its
`vendor_doDeviceAttribute`, attribute 3, is present in both AMDSupport
(`AtiGpuWrangler`) and the framebuffer (`AmdGpuWrangler`). For value bit 1 it
sets `LoadHWServices` and `LoadAccelerator` together, back to back, then calls
`registerService()` on the PCI device. Bits 0-1 also set `LoadAMDSupport` and
`LoadController`. Both kexts then match at once; there is no ordering between
them. (APPLE-STACK.md said "in that order"; they are set in the same call.)

**For 0x1681:** `docs/spec-data/personalities-A1.plist` holds two personalities
for the plugin's Info.plist. They are Apple's Navi 21 entries, copied verbatim
from the installed kexts, with two changes each:
- `IOPCIMatch` = `0x16811002`;
- `IOPropertyMatch` gains **`RBGPULoad` = true**, a property only our peer sets.

AMDSupport's wrangler is already attached to this iGPU. If anything ever asked
it for attribute 3 without the gate, it would start Apple's kexts with no peer:
the accelerator would block forever (APPLE-STACK.md, Navi48 trap 1). With the
gate, only our peer can start them.

The peer, once it is ready to answer requests:
1. sets `RBGPULoad`, `LoadHWServices` and `LoadAccelerator` (OSBoolean true)
   on the IOPCIDevice;
2. calls `registerService()` on the IOPCIDevice, as the wrangler does.

## A2: project name

`AMDRadeonHWServicesAbstract::readProjectName` reads **`@0,name`** from the
PCI device. It accepts OSData or OSString (two casts:
`OSData::metaClass`, then `OSString::metaClass`), and `getMatchProperty` maps
it through `ProjectNameTable`. `"ATY,Belknap"` selects Load6800 = X6800HWLibs.
The peer sets it before step 1 above. (An OpenCore DeviceProperties entry
would also work, but then it is not behind `-rbgpuoff`.)

## A3: the peer

An IOService of our own, matched on the 680M's IOPCIDevice with
`IOMatchCategory` = **`ATIFramebuffer`**. That category is what
`initLinkToPeer` looks for first; without it, the accelerator falls back to
IONDRVFramebuffer and blocks. The peer sits beside IONDRVFramebuffer and
AMDSupport and touches no registers. The boot framebuffer keeps the screen.

### The protocol

Both directions use `callPlatformFunction(const OSSymbol *"SpecialAMDKey",
bool waitForFunction, void *p1, void *p2, void *p3, void *p4)` (IOService
vtable +0x6B8):
- `p1` points to a u32 request number;
- the rest depend on the request;
- unknown symbols go to `IOService::callPlatformFunction` (the superclass).

Request names come from the framebuffer's own table
(`AmdRadeonController::stringifyAcceleratorMessage`):

| # | Name | # | Name |
|---|---|---|---|
| 0x00 | Register | 0x10 | Driver Will Sleep |
| 0x01 | GetFedsInfo | 0x11 | Driver Did Sleep |
| 0x02 | CAIL_IRI | 0x12 | GfxOff |
| 0x03 | PPLIB_IRI | 0x13 | Gpu Panic |
| 0x04 | GetSurfaceInfo | 0x14 | Gpu Status |
| 0x05 | WSAA State | 0x15 | Request Power Change |
| 0x06 | PowerPlay Ready | 0x16 | Reserve NDRV Space |
| 0x07 | Driver Will Wake | 0x17 | ASIC Reset State |
| 0x08 | Driver Did Wake | 0x18 | CWEDDE PM Passthrough |
| 0x09 | CP Event | 0x19 | Query GPU Status |
| 0x0A | CP Downstream Event | 0x1A | Get DCC Capabilities |
| 0x0B | Request Drop Registration | 0x1B | Controller Enabled |
| 0x0C | Wrangler Request Accelerator Registration | 0x1C | Controller Disabled |
| 0x0D | Wrangler Request Controller Registration | 0x1D | DfCState |
| 0x0E | Broadcast Request Device Disconnected | 0x1E | Unknown |
| 0x0F | Request ATOM BIOS Buffer | | |

### Accelerator -> peer (what the peer must answer)

From every `callPlatformFunction` through the peer pointer (accelerator+0x1F40,
symbol at +0x1F48) and `sendRequestToController`:

| Request | Sent by | Arguments | Peer's answer |
|---|---|---|---|
| 0x00 Register | `AMDGraphicsAccelerator::start` | wait = **false**, p2 = the accelerator | Keep p2 (the accelerator; retain it). Return kIOReturnSuccess. Then, **after returning** (thread call or the peer's workloop, never inside this call), send 0x06 and 0x1B (below). |
| 0x0B Request Drop Registration | `AMDGraphicsAccelerator::stop` | wait = true, p2 = the accelerator | Forget and release the accelerator. Return success. |
| 0x17 ASIC Reset State | `AMDAccelChannel::resetHardwareAndReplay` (GPU hang recovery) | | Return kIOReturnUnsupported at first; log it. |
| 0x1A Get DCC Capabilities | `AMDAccelResourceAddr2::shouldAllocScanoutDcc` | | Return kIOReturnUnsupported: the NDRV framebuffer cannot scan out compressed (DCC) surfaces. Log it. |
| 0x14 Gpu Status | (Navi48: the accelerator invalidates the device unless `*p2 = 1`) | | `*(u32 *)p2 = 1`, return success. The sending site was not found statically. Answer it anyway. |
| anything else | | | Log the number, return kIOReturnUnsupported. |

`findAndRegisterPowerService` also sends requests 0x00 and 0x01, but to a
separate power service (accelerator+0x1F60, symbol at +0x1F50), not to the
peer. It runs only when accelerator+0x1F58 is set. It then waits at most 10 s
(`waitForMatchingService`, 10^10 ns) for a service named
`AMDRadeonPowerService`, which no kext here provides, and `start()` ignores its
result. So the worst case is a 10 s delay, not a failure.

### Peer -> accelerator

Apple's controller wraps these in `AmdRadeonController::messageAccelerator(req, a, b, c)`:

```
accel->callPlatformFunction(SpecialAMDKey, true, &u32 req, a ? a : &u32 1, b, c)
```

(The controller keeps the accelerator at +0x7960 and the symbol at +0x7950.)
After Register, the peer sends, in this order:
1. **0x06 PowerPlay Ready**. The accelerator's only path to `powerUpHW()`.
2. **0x1B Controller Enabled**: "finish initialization".

This is the sequence `AmdRadeonController::powerUp` uses. Later, for sleep
and wake, the peer forwards 0x10/0x11 and 0x07/0x08 the way
`AmdPowerPlayHelper` does (APPLE-STACK.md). That is not needed for the
first test.

### Display pairing (later)

WindowServer pairs a display with an accelerator through properties
(`IOAccelFindAccelerator`); Navi48 publishes them on its framebuffer. This is
not needed to see the accelerator start, and is traced separately.

## What the first boot with A1-A3 should show

Everything else must stay as it is today: no HWLibs patches yet. Expected:

1. The peer attaches (ioreg: our class on IGPU, category ATIFramebuffer).
2. HWServices attaches and selects X6800HWLibs from `@0,name`.
3. HWLibs starts and fails, either at B0 (no VBIOS, below) or at B1/B3. Its
   own log lines ("Unable to find matching firmware device type",
   "Unsupported hw version!") say where.
4. The accelerator starts, registers with the peer (request 0x00 is logged),
   then fails because HWLibs failed. **Navi48 trap 3:** the accelerator's own
   error path in `start()` page-faults on unsupported hardware.

So the first A1-A3 boot is expected to panic at step 4. Two ways to keep it
from panicking:
- (a) **Recommended.** Add only the HWServices personality first (no
  accelerator personality). That boot tests A2, the peer, and how far HWLibs
  gets, and cannot hit trap 3.
- (b) Add both personalities, with the C5 guard in place.

## Prerequisite found on the way: B0, the VBIOS

HWLibs gets the VBIOS through `AmdTtlServices::cosGetVbiosRomImage`:
- first the PCI device's **`ATY,bin_image`** property;
- then `/efi` `device-properties` (the EFI boot ROM property);
- it checks the 0x55AA ROM signature.

Neither exists on this machine. On an APU the VBIOS sits in the ACPI **VFCT**
table, which is where Linux reads it from (`amdgpu_acpi_vfct_bios`). macOS
15.8.1 does not expose ACPI tables in the registry, so it cannot be read from
here. It can come from:
- OpenCore's SysReport, which dumps the ACPI tables (needs the DEBUG
  OpenCore, which this machine runs), or
- an ACPI dump from Linux.

The peer then sets `ATY,bin_image` (the VBIOS image from VFCT) before step 1.
Getting the dump is a step for the user.

### B0 result (2026-10-05)

`sudo tools/efi.sh vbios` found 2 VFCT entries, one for 1002:1681 at bus 54
device 0 function 0, plus an empty one:
- 44544 bytes, ATOM check and option-ROM checksum both OK;
- sha256 `27ed5802eb119409e9711bd7e3a8383c9c5725389960c6eebc154737fd8203d9`;
- kept in firmware/ (not committed).

`tools/atomdump.py` shows:

- "Rembrandt Generic VBIOS", part 113-REMBRANDT-X37, ATOMBIOSBK-AMD
  VER032.017.000.055.000000, 04/12/22, config "RmbGenericVb". ROM header v2.3;
  its subsystem field reads 1002:1002 (the PCI subsystem is 1043:14C3).
- Data tables present:
  - firmwareinfo v3.4, smu_info v3.1, gfx_info v2.6;
  - dce_info v4.4, displayobjectinfo v1.4, lcd_info v3.1;
  - integratedsysteminfo v2.2 (the APU table), gpio_pin_lut, vram_usagebyfirmware;
  - and five sw_datatables.
- Absent (the dGPU tables): powerplayinfo, smc_dpm_info, umc_info, vram_info,
  asic_profiling_info, voltageobject_info.
  - Apple's Navi 21 SMU path reads its pptable from powerplayinfo
    (`smu_svc_read_pptable_from_vbios`). The B3 hook replaces that path.
  - Any other reader of the missing tables will show up in HWLibs' logs
    ("Could not retrieve ... from VBIOS").
- 8 of 81 command tables.

## Open

1. The site that sends 0x14 Gpu Status: not found statically, so the answer
   above follows Navi48.
2. The display pairing properties.
