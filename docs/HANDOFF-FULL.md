> **STALE — do not use. Superseded by `docs/HANDOFF.md`.**
>
> This file is a verbatim dump of the repo as it stood on 2026-10-05, at boot
> 27, when TTL init still failed with status 4. Everything after that —
> the yellow_carp firmware substitution, TTL init succeeding, the SMU link,
> the whole KIQ/fence investigation (2ag through 2ea) — is absent, and its
> "where it stands" section is wrong by about a hundred boots. The repo's own
> files are the current source; `docs/HANDOFF.md` is the current state.

# RembrandtGPU — full project handoff (2026-10-05)

This one file holds the whole project: goal, machine, rules, workflow,
every boot result, the static analysis of Apple's AMD stack, and the
current source code. Sections after this header are copies of the repo's
files, each one under a `===== FILE: <path> =====` line.

## 1. Goal and machine

- Goal: graphics acceleration (Metal) for the AMD Radeon 680M iGPU
  (Rembrandt, PCI `1002:1681` rev 0a) on macOS 15.8.1 (24H32).
- Laptop: ASUS TUF A15 FA507NU, Ryzen 7 7735HS, OpenCore, Lilu 1.7.3.
  csr-active-config 0xA03. Today the 680M runs only on the firmware
  framebuffer (IONDRVFramebuffer + AMDSupport on `IGPU@0`).
- The chip's IP versions (from its own discovery table): GC 10.3.3,
  DCN/DMU 3.1.3 (Linux: DCN 3.1.2 code), MMHUB 2.4.0 (Linux: mmhub_v2_3),
  MP0 (PSP) and MP1 (SMU) 13.0.3, SDMA 5.2.3, VCN 3.1.1. 12 CUs
  (1 SE x 2 SA x 3 WGP), wave32. 4 GB carve-out at system-physical
  0x7_8000_0000 = GPU address 0xF4_0000_0000.

## 2. The route

Reuse Apple's own Navi 21 (RDNA2, gfx10.3) driver stack —
AMDRadeonX6000HWServices + AMDRadeonX6800HWLibs + AMDRadeonX6000
accelerator — on the 680M, the way NootedRed does for Vega APUs. A Lilu
plugin (RembrandtGPU.kext, loaded from the EFI by OpenCore) does three
things:

1. It adds matching personalities, plus a peer IOService in
   `IOMatchCategory "ATIFramebuffer"` that sets `@0,name = ATY,Belknap`
   (this selects X6800HWLibs).
2. It patches HWLibs tables and hooks HWLibs functions.
3. It logs everything into registry properties (`RBGPULog` and
   `RBGPUDiag`), because early-boot IOLog/kprintf output is not kept.

The display stays on the firmware framebuffer. DCN 3.1 is not in Apple's
framebuffer kext and is a later problem.

## 3. Where it stands (after boot 27)

Apple's TTL (HWLibs) init now runs this far under the boot-args below,
still as a dry run (`_ttl_hw_init` is gated off):

```
AmdTtlServices::initialize(input)
  +0x570 synced from +0x958 (-rbgpusync570)          passes
  _ttl_initialize -> _ttlIsSupportedDevice           passes (cloned capability entry)
  _ttl_sw_init: per-IP SW_INIT via _IpiExecuteSwipEntryPoint
    client 3                                         ok
    client 5                                         ok
    client 6  PSP  (_psp_sw_init)                    ok  (asic_type 19 -> 8 swap for _psp_init_pfn_ptr, then 19)
    client 8  SMU  (plugin's own SMU 13 slot table)  ok  (queries only)
    client 7  GC   (_gc_sw_init 0x126a7f74)          FAILS: returns 2
      _gc_get_hw_version -> 0x000A0303 (GC 10.3.3)
      _gc_set_fw_entry_info (0x126aa16b) -> 2   <- first failure
      (_gc_init_fcn_ptr_list is never reached)
  -> _ttl_initialize status 4 -> X6000 "TTL::initialize() Failed! 0xe00002bc"
```

Why the GC fails: HWLibs only has Navi 21 (GC 10.3.0) GC microcode
(`_gc_10_3_*_ucode`). `_gc_set_fw_entry_info` has a GC-version check
(`cmpl $0xa0300` at 0x126aa1ce) before `_gc_10_3_get_fw_constants`. The GC
firmware is loaded through the PSP, which checks a per-ASIC signature, so
Navi 21 images cannot run on the 680M.

User's decision after boot 27: try the 680M's own GC firmware (Linux
linux-firmware `amdgpu/yellow_carp_*.bin`). The files are in `firmware/linux/`
(gitignored, never committed). the assistant is not working on the
firmware-replacement step. Everything up to boot 27, and this file, is the
project as it stood before that step.

Safety note from the assistant: `-rbgpupsphw` is the switch that lets `_ttl_hw_init`
really run. That means real PSP commands and firmware loads to the PSP, which
on this APU is the CPU's own security processor. Keep it off unless you
accept that risk. In the same hw_init, the SMU slot (`rb_smu13_bringup`) would
send SetDriverDramAddrHigh/Low with an unverified DMA address. The boot-25
"carve-out verification FAIL" is still open.

## 4. Boot-args and plugin switches

Current boot-args:
`debug=0x100 keepsyms=1 -vi2c-force-polling -lilubetaall -wegnoegpu msgbuf=1048576 -rbgpusync570 -rbgpuspoofttl`

| Switch | Effect |
|---|---|
| `-rbgpuoff` | plugin loads, does nothing (no peer, Apple kexts never match) |
| `-rbgpudbg` / `-rbgpubeta` | Lilu debug / beta |
| `-rbgpusync570` | copy AmdTtlServices +0x958 (firmware directory) into +0x570 before initialize(input) |
| `-rbgpuspoofttl` | device/capability table entries for 0x1681, _ttlIsSupportedDevice override, PSP asic_type swap, GC experiments |
| `-rbgpupsphw` | NOT set. Lets `_ttl_hw_init` run for real (see the safety note) |
| `-rbgpusafettl` | older experiment (milestone 2h) |
| `rbprobe_level=N`, `rbprobe_smu=1` | the separate probe kext only |

## 5. How the work is done (workflow)

- The user runs every privileged command: `sudo tools/efi.sh install|off|on|...`
  (installs the plugin into the EFI after backing up config.plist), reboot,
  then `tools/plugin_check.sh`. It saves `logs/<ts>-plugin/peer.plist` and
  prints RBGPULog, RBGPUDiag, and `tools/re/ptrsym.py` names for the log's
  pointers.
- Build: `make plugin` (CommandLineTools only, no Xcode). `make kext tools`
  builds the probe and runs the hosttests first.
- Static analysis: `tools/re/kc_extract.py` pulls a kext out of
  `/System/Library/KernelCollections/SystemKernelExtensions.kc` into
  `build/re/<kext>`. Disassemble with
  `xcrun llvm-objdump -d --no-show-raw-insn build/re/AMDRadeonX6800HWLibs`.
  All addresses in this file are file addresses of those extracted kexts;
  resolve by symbol name.
- Each round: write the hook code. Check that every routed symbol is
  DEFINED in the kext (`nm -m build/re/<kext>`). Build, commit, install,
  reboot, run `plugin_check.sh`, and add the result to docs/RULES.md as
  "Boot N".

## 6. Hard-won rules (break these and it panics or silently fails)

1. Lilu `solveSymbol` returns the bare KASLR slide for IMPORTED symbols.
   Never route a name by a kext id unless `nm -m` shows it defined there.
   `_kprintf` / `_IOLog` are kernel imports: route them on the kernel.
2. A Lilu long jump is 14 bytes (6 + sizeof(uintptr_t)). Never hook a
   function shorter than that. Known short ones: `_psp_asd_init` 8 bytes,
   `_gc_bist_register` 8, `_gc_get_oca_data_buffer_size` 11.
3. Kernel stack frame limit: one function's frame must stay under 1024
   bytes. That is why the install is split into routeTTL / routePSPBasics /
   routePSPRingTmr / routeGC. A `RouteRequest` array of ~31 entries in one
   function hit 1112 bytes.
4. No function-local `static` with a non-constant initializer: kexts lack the
   guard-variable runtime.
5. Accelerator class names carry the `AMDRadeonX6000_` prefix, e.g.
   `__ZN37AMDRadeonX6000_AMDGraphicsAccelerator5startEP9IOService`.
6. The peer must exist before the accelerator starts. Otherwise the
   accelerator links to IONDRVFramebuffer, calls SpecialAMDKey with
   wait=true, and blocks forever. Answer request 0x14 with `*p2 = 1`.
7. macOS GNU make 3.81 compares whole seconds. `efi.sh install` recompiles
   for that reason, and `plugin_check.sh` checks the loaded UUID matches
   build/out.
8. The RBGPULog ring is 256 lines. Boot 27 filled it and dropped the first
   lines. Raise it (e.g. 512) if more logging is added.
9. SMU: the plugin's SMU 13 library (`src/common/rb_smu13.c`) is allowed
   QUERIES ONLY (`RB_SMU13_ALLOW_QUERY`). Allowing any other message group
   on hardware is the user's decision: state what it changes on the chip.
10. Recurring paste bugs to check every round: `uint64_ver` instead of
    `uint64_t ver` in the SMU typedefs, and the switch-status log written as
    `x ? "ENABLED" : "ENABLED"`. Also check that no hook was dropped from
    routeTTL. The four TTL hooks, including -rbgpusync570, vanished once.

## 7. Key HWLibs addresses / facts (file addresses, 15.8.1, kexts 7.0.25)

| Thing | Where / value |
|---|---|
| `__ZL15deviceTypeTable` | 84 `{id, type}`; entry 66 -> 0x1681 / type 6 |
| `_DeviceCapabilityTbl` | 93 entries x 0x50; entry 0 cloned from entry 60 (0x73A2), id 0x1681, +0x18 = +0x20 = 0xdeadcafe |
| `_DevGetDeviceInfoEntry` | 0x12646a40; compares +0x10, +0x18, +0x20 (each equal or 0xdeadcafe) |
| `setFirmwareDirectory` | 0x12660f40, stores dir at this+0x958 |
| `_psp_sw_init` | 0x12648430 |
| `_psp_init_pfn_ptr` | 0x126487c8; has tables for asic types 8..15 only; 13.0.3 maps to 19 |
| `_gc_sw_init` | 0x126a7f74 |
| `_gc_set_fw_entry_info` | 0x126aa16b; `cmpl $0xa0300` at 0x126aa1ce |
| `_gc_init_fcn_ptr_list` | 0x35 sub-version mask at 0x126a93b2 (later check, not reached) |
| SWIP client ids | 3, 5, 6 = PSP, 8 = SMU, 7 = GC, 2, 14, 9, 15, 10 (in init order) |
| SW_INIT return | 1 = ok, 0 = fail; `_ttl_initialize` status 4 -> 0xe00002bc |

## 8. Plan beyond the GC (from docs/APPLE-STACK.md)

After GC SW_INIT, still to do: SDMA 5.2.3 (slot exists), GC golden
settings for 10.3.3, CAIL_ASIC_CAPS_TABLE entry, then in the accelerator
an MMHUB 2.3 class (C2), the shader-engine layout (C3), the carve-out
memory model (C4), and a guard for start()'s failure path (C5). Then the
peer sends requests 6 and 0x1B so `powerUpHW()` runs.

## 9. Linux references

Unmodified upstream files are in `docs/linux-ref/` (not copied here; commits
in UPSTREAM.md): discovery.h, amdgpu_discovery.{c,h}, soc15_hw_ip.h,
mp_13_0_2_offset.h, smu_v13_0_1_{ppsmc,pmfw}.h,
smu13_driver_if_yellow_carp.h, yellow_carp_ppt.{c,h}, amdgpu_ucode.h.

## 10. What follows in this file

docs/RULES.md (rules + all 27 boots), docs/PLAN.md, docs/APPLE-STACK.md,
docs/SPEC-A-START.md, docs/SPEC-SMU-SLOTS.md, the boot-27 RBGPULog and
RBGPUDiag, then the source code: src/plugin/*, src/common/*,
tools/plugin_check.sh, tools/re/ptrsym.py.


===== FILE: docs/RULES.md =====

# RembrandtGPU — notes for the assistant

Goal: graphics acceleration for the Radeon 680M (Rembrandt, `1002:1681` rev 0a,
GC 10.3.3 / DCN 3.1.2 / PSP+SMU 13.0.x / SDMA 5.2.3) on macOS. Plan:
docs/PLAN.md. Same machine as ~/Developer/AirPort_RTW89: ASUS TUF A15 FA507NU
(Ryzen 7 7735HS), macOS 15.8.1 (24H32), OpenCore, no GPU driver today (the 680M
runs on the firmware framebuffer, IONDRVFramebuffer + AMDSupport attached to
`IGPU@0`). Boot-args include `-wegnoegpu`; csr-active-config 0xA03.

## Layout

- `src/common/` — freestanding C shared by the kext and host tools: the IP
  discovery parser (`rb_discovery.c`), the read-only probe (`rb_probe.c`), the
  register list (`rb_regs.h`, every offset cites its Linux header), the SMU
  queries (`rb_smu.c`) and `rb_smu13.c`, SMU 13 power management ported from
  Linux yellow_carp_ppt.c for the plugin (not used by the probe kext). Its
  message groups (RB_SMU13_ALLOW_*) start at queries only; allowing any other
  group on hardware is the user's decision, like the probe's allowlist.
  Tested in tools/hosttest/smu13_test.c against a simulated SMU whose tables
  are Linux's own structs.
- `src/kext/` — `RembrandtProbe`, an IOService in its own IOMatchCategory beside
  the boot framebuffer driver. Runs the probe once in start(), publishes
  `RBProbe` properties, unmaps everything.
- `tools/hosttest/` — the probe against a simulated chip + parser fuzzing,
  ASan/UBSan. `make kext` runs it first; never load a kext whose hosttest fails.
  Add a case whenever src/common changes, and check a new test fails when the
  code it guards is broken.
- `docs/linux-ref/` — unmodified Linux files the code follows (UPSTREAM.md).
- `tools/re/` — static analysis of Apple's AMD kexts: `kc_extract.py` (kext out
  of the kernel collection as a standalone Mach-O), `machoread.py` (chained
  pointers, strings, symbols), `dumptable.py`, `kc_stubs.py` (kernel calls
  through the collection's branch stubs -> names; `annotate` for objdump
  output), `vtslots.py` (IOService-family vtable offsets -> methods), `strxref.py`
  (functions that use a C string), `switches.py` (jump tables: case -> handler),
  `ptrsym.py` (names every RBGPULog pointer inside a loaded AMD kext; run by
  plugin_check.sh). Output
  goes to `build/re/`.
  Findings: docs/APPLE-STACK.md.

## Workflow

- Build: `make kext tools` (CommandLineTools only, no Xcode). The kext step
  also resolves every import with `kmutil libraries`.
- The probe's hardware rules: no writes except MM_INDEX / MM_INDEX_HI (enforced
  in rb_probe.c and asserted by the hosttest), never enable PCI decoding or bus
  mastering, never map BAR0's first 16 MB (the boot framebuffer's), read only
  inside the carve-out. Levels 2 (DCN registers) and 3 (+ GC registers) are
  opt-in: `RBProbeLevel` in src/kext/Info.plist, or boot-arg rbprobe_level=N.
- The SMU queries' hardware rules (rb_smu.c, opt-in: `make kext SMU=1`, plist
  RBSMUQuery, boot-arg rbprobe_smu=1): writes only to MP1 C2PMSG_66/82/90 of an
  MP1 13.x, only allowlisted query messages (GetSmuVersion, GetDriverIfVersion,
  GetEnabledSmuFeatures), nothing sent unless the response register reads 1,
  stop at the first non-OK answer. Linux's protocol (smu_msg_v1_send_msg).
  Adding a message to the allowlist is a decision for the user: say what it
  changes on the chip.
- Register gates follow Linux's version grouping, not the exact version: this
  chip reports MMHUB 2.4.0 (Linux: mmhub_v2_3, 2.3.0 offsets) and DMU 3.1.3
  (Linux: DCN_VERSION_3_1, dcn_3_1_2 offsets). Check Linux's grouping before
  adding a gate.
- Linux sources: fetch from torvalds/linux and record the commit in
  docs/linux-ref/UPSTREAM.md.

## Loading kexts — rules

- **Always ask the user before loading or unloading a kext.** A bad load can hang
  or panic the machine and ends this session.
- The user runs `sudo tools/load.sh` (stages a root-owned copy in
  /private/var/tmp, loads, prints the kernel messages and tools/report.py) and
  `sudo tools/unload.sh`. the assistant never runs these and never asks for the password.
- Never install into the EFI or /Library/Extensions.
- The Lilu plugin (src/plugin, `make plugin`) boots from the EFI. The user runs
  `sudo tools/efi.sh install|off|on|uninstall|status|restore` (saves
  config.plist to logs/efi-backups/ first; edits via tools/efi_config.py, tested
  by tools/efi_config_test.py) and `tools/plugin_check.sh` after a reboot.
  the assistant never runs efi.sh. `efi.sh sysreport-on` + reboot + `efi.sh vbios`
  pulls the 680M VBIOS out of the ACPI VFCT table (tools/vfct.py, tested by
  tools/vfct_test.py) into firmware/ (gitignored: AMD/ASUS firmware).
- Before any load, commit so a hang loses nothing. The user allows the assistant to
  commit at any time without asking (2026-10-04); pushing still needs asking.
- After a reboot: `/Library/Logs/DiagnosticReports/*.panic`, and
  `log show --last boot --predicate 'eventMessage CONTAINS "RembrandtProbe"'`.
- Results land in `logs/<timestamp>/` (probe.plist, discovery.bin).
  `tests/data/rembrandt-680m-tufa15.bin` is this machine's real table (first
  probe, 2026-10-03); the hosttest parses it and runs the full probe on it.

## What the first probe found (2026-10-03, level 1)

Carve-out 4096 MB (RCC_CONFIG_MEMSIZE); discovery table at 0xffff0000, read
through MM_INDEX_HI, binary v1.2 / IP table v4 with 32-bit bases, 34 IPs,
all checksums good. GC 10.3.3, DMU 3.1.3, MMHUB 2.4.0, ATHUB 2.4.0,
MP0/MP1 13.0.3, SDMA 5.2.3, VCN 3.1.1, HDP 5.2.0, NBIF 7.5.0, OSSSYS 5.2.1.
GC info: 1 SE x 2 SA x 3 WGP = 12 CUs, wave32. Harvest table lists only
hw_ids 320/321, which Linux ignores. PSP secure OS running (C2PMSG_81
0x139ca7), C2PMSG_35 reads all-ones, IFWI done (C2PMSG_33 bit 31). SMU
mailbox idle: message 0, argument 0, response 1 (OK). C2PMSG_81 differs
between boots (0x139ca7, 0x129d6e): nonzero means "secure OS alive", not a version.

## Second probe (2026-10-03, level 2)

- Carve-out: GPU address 0xF4_0000_0000-0xF4_FFFF_FFFF (MMMC_VM_FB_LOCATION
  0xf400-0xf4ff), system-physical 0x7_8000_0000 (MMMC_VM_FB_OFFSET 0x780), so
  30-34 GiB. BAR0 (256 MB at 0x8_8000_0000) windows its first 256 MB. Linux on
  an x86-64 APU uses FB_OFFSET as the CPU aperture for the whole carve-out.
  AGP_BASE 0.
- Display: one pipe lit. OTG0 on (total 2107x1130), HUBP0 scanning ARGB8888
  from 0xF4_0000_0000 = carve-out offset 0 (the GOP framebuffer). OTG1-3 off.
- DMCUB enabled and idle-waiting; SCRATCH0 = 0x2 (mailbox ready, not DAL fw):
  the GOP/VBIOS DMCUB image is running. Linux's DM reloads its own image.

## Third probe (2026-10-04, level 1 + SMU queries) — first mailbox writes

All four queries answered OK, 12 writes, none refused. Mailbox before: msg 0,
arg 0, resp 1.
- SMU firmware 0x04453900: program 4, version 69.57.0.
- Driver interface version 4 = SMU13_YELLOW_CARP_DRIVER_IF_VERSION: Linux's
  yellow_carp_ppt.c tables match this firmware.
- Enabled features 0x1497e1fd_ebfffdff (47), among them GFX_DPM, DS_GFXCLK,
  GFX_TEMP_VMIN, GFX_DEM, ATHUB_PG, VCN_DPM, DCFCLK_DPM, S0I3, STAPM. Off:
  ULV, ZSTATES, DS_FCLK, FUSE_PG, CORE_DLDO, RSMU/SMN/MSMU_LOW_POWER, SMART_SHIFT.
- First answer took 307 us (the SMU waking from deep sleep), later ones 8-12 us.

## Lilu plugin milestone 0 (2026-10-05)

Empty RembrandtGPU.kext installed with tools/efi.sh: OpenCore injected it,
Lilu 1.7.3 accepted it, the RembrandtGPU IOService is live under IOResources.
Its SYSLOG line from pluginStart() is in neither `log show --last boot` nor
dmesg: early-boot plugin messages are not kept, so the registry instance is
the check (tools/plugin_check.sh). With -rbgpuoff the kext is still injected
and loaded but no instance exists: the off-switch works. Milestone 0 done.

## Milestone 1 (2026-10-05): peer + HWServices on the 680M

Booted cleanly, no panic. On IGPU@0: RembrandtGPUPeer (ATIFramebuffer),
AMDSupport, IONDRVFramebuffer (screen unchanged), and
AMDRadeonX6000_AMDRadeonHWServicesNavi with `LoadPlugIn = Load6800`, whose
child AMDRadeonX6800_AMDRadeonHWLibsX6800 is attached (!registered). The GPU
carries @0,name ATY,Belknap, ATY,bin_image (44544 bytes), RBGPULoad,
LoadHWServices, LoadAccelerator. So A1 (gated matching) and A2 (project name
-> X6800HWLibs) work on hardware.
HWServicesNavi derives from HWServicesSWIP, whose start() calls
createTtlInterface: the AmdTtlServices constructor ran (PCI config reads,
BAR5 mapping, B1 device-type lookup -> type 9). It logs with kprintf, which
does not reach `log show`. No IP was initialised: that starts with the
accelerator. The peer's IOLog lines are not in `log show` either (early boot);
the registry is the check.

## Milestone 2, first boots (2026-10-05)

Boot 1 froze after the lock screen: panic Kernel-2026-10-05-014447 (uptime
18 s), "NMIPI for unresponsive processor: TLB flush timeout", CPU 0 inside
com.rtw89.driver `_rtw89_pci_napi_poll`. No AMD or RembrandtGPU frame. Both
earlier panics (2026-10-01) are rtw89 double faults in
AirPort_RTW89::nativeRequest. So the Wi-Fi driver is the known crasher; the
GPU bring-up as a trigger is not excluded.
Boot 2 (forced restart) came up: peer, HWServices, and AMDRadeonX6000 +
X6800HWLibs loaded, but no accelerator instance stays on IGPU@0 (start or
probe failed without a panic). Nothing from the plugin is in `log show`
(18k kernel lines this boot) or the dmesg grep, with msgbuf=1048576: IOLog
from the plugin is not usable as evidence here. Diagnostics have to go into
registry properties.
Boot 3 (milestone 2b, registry diagnostics): no panic. SMU hook routed but
never called; the start() guard did not install (it looked up
`__ZN22AMDGraphicsAccelerator...`; the real names carry the
`AMDRadeonX6000_` prefix, `__ZN37AMDRadeonX6000_AMDGraphicsAccelerator5startEP9IOService`).
The accelerator sent the peer request 0x14 (Gpu Status) 7x, answered
*p2 = 1, never 0x00 Register, and does not stay attached: its start() fails
early, before Register and before HWLibs reaches SMU init. Next: capture
Apple's own kprintf/IOLog output into RBGPULog to see why.

Lilu pitfall (found reviewing milestone 2c, 2026-10-05): MachInfo::solveSymbol
does not skip undefined (imported) symbols; it returns n_value + kaslr_slide,
the bare slide, and routeMultiple then writes a jump there. Never route a name
by a kext's id unless `nm -m build/re/<kext>` shows it defined in that kext
(_kprintf / _IOLog are imports in all AMD kexts; they live in the kernel).
_smu_cgs_free_memory does not exist in X6800HWLibs (only _smu_cgs_alloc_memory).

Boot 4 (2026-10-05 02:08) ran milestone 2b again (UUID B3D22192), not 2c: macOS's
GNU make 3.81 compares whole seconds, and a same-second stash/pop left 2b
objects newer-or-equal to the 2c sources. No panic. efi.sh install now
recompiles the plugin sources and prints the installed UUID; plugin_check.sh
says whether the loaded plugin is build/out. Never swap sources in and out
of the tree around a build; use a separate checkout.

Boot 5 (2026-10-05 02:11, milestone 2c, UUID B17E6175): no panic. Capture
works (kprintf/IOLog routed on the kernel, filtered by caller). Apple's own
log, in order: HWLibs "Unable to find matching firmware device type for device
ID: 0x1681"; X6000 "Calling TTL::initialize()" (BootLoader 0x00280066, POST
Code/Ready 0xffffffff, nonlocal mem limit 1 GB, doorbell aperture 2 MB);
HWLibs "TTL Interface: Boot mode Normal"; X6000 "TTL::initialize() Failed!
Error code: 0xe00002bc"; then the failure path: NULL TTL Handle, FW
Attestation not supported, SWIP_Error_msgs null, PSP info (tOS status
0x0012054f, tOS/BootLoader 0.28.0.66, Sec. Policy 13.24.0.110), m_hTtl NULL,
hdcp_services terminate 0xe00002c2, queryEngineQueueCount(VIDEO_DECODE)
failed, driver state trace u64AllAccelStart 0x20c5. Failure path ran without
a panic although the guard did not install ("failed to locate or patch stop()
call site in start()"). SMU hook still not reached (TTL init fails first).
Captured entries are cut at ~256 chars. Full log: logs/20261005-021213-plugin/peer.plist.

Boot 6 (2026-10-05 02:30, milestone 2d, UUID F02659D0): no panic. Multi-line
capture works (PSP block and state trace whole). __ZL15deviceTypeTable resolved
(0xffffff7fa51c4d60) and 0x1681 -> type 6 written at empty index 83 before
HWLibs ran; HWLibs still logs "Unable to find matching firmware device type
for device ID: 0x1681" ~2 ms later, and TTL::initialize() fails 0xe00002bc
as before. So that error does not come from a lookup that this table entry
satisfies. PSP: tOS status 0x0011a215 (varies by boot), BootLoader Ready /
POST code / FW Validation Info 0xffffffff, tOS PC/panic code 0. RSMU timeout
log: 2 events, [0] ID 0x6 CLKA0 read Addr 0x5b388 aperture 0, [1] GC read
Addr 0x01ef0 aperture 1. Driver state trace: accelStart_entry/exit 1,
accelStarted 0, initLinkToPeer 1, createHWHandler 1, initExternalInterfaces 1;
createHWInterface, configureDevice, hardwareInit, initializeTtl and every
later step 0; u64AllPowerUp 0. Full log: logs/20261005-023111-plugin/peer.plist.

Boot 7 (2026-10-05 09:25, milestone 2e, UUID DE6217E7): no panic.
deviceTypeTable entry 60 (0x73A0, type 6) replaced with 0x1681 (Gemini's
note said index 66 / 0x5D; the code chose 60). Read-back 0x1681 / type 6:
the write to HWLibs __TEXT,__const sticks. "Unable to find matching firmware
device type" is gone. TTL::initialize() still fails 0xe00002bc, and the rest
of the log is unchanged (NULL TTL handle, PSP Ready/POST 0xffffffff, same two
RSMU timeout events, state trace u64AllAccelStart 0x20c5, nothing past
initExternalInterfaces). tOS status 0x0011e396. Full log:
logs/20261005-092629-plugin/peer.plist.

Boot 8 (2026-10-05 09:46, milestone 2f, UUID F8037B38): no panic.
deviceTypeTable entry 66 (0x005D) -> 0x1681 / type 6, read-back OK.
__ZN14AmdTtlServices10initializeEv hooked (ttl_init_routed 1) but never
called: no "entered" line, ttl_init_ret is only the 0 set at install. So the
0xe00002bc after "Boot mode Normal" comes before the inner initialize(). Rest
of the log identical to boot 7 (tOS status 0x0011f4a6). Full log:
logs/20261005-094651-plugin/peer.plist.

Boot 9 (2026-10-05 09:54, milestone 2g, UUID 9B0BCC61): no panic. Outer
__ZN14AmdTtlServices10initializeEP30_TtlLibraryInitializationInput hooked:
called once, returned 0xE00002BC itself. Input quadwords [0]=0xffffffc085c28690
[1]=0xffffffc085c286a8 [2]=0 [4]=0x40000000 [5]=doorbell base [6]=0x200000;
AmdTtlServices state before the call: +0x560/0x568/0x570/0x578/0x598 all 0.
Matches the path Gemini read from the disassembly: input[2]==0 and this+0x570==0
-> 0xe00002bc before createPciDeviceCache/initialize(). The inner initialize()
hook again not called. plugin_check now prints RBGPUDiag numbers unsigned
(ioreg -a gives signed 64-bit). Full log: logs/20261005-095513-plugin/peer.plist.

Boot 10 (2026-10-05 10:06, milestone 2h, UUID B80CDCB5): no panic.
__ZN14AmdTtlServices20setFirmwareDirectoryEP20AMDFirmwareDirectory IS called,
once, ~0.7 ms before TTL::initialize(): this=0xffffff9045ac0400,
dir=0xffffff86adf31e00. initialize(input) still sees +0x570 == 0 and returns
0xe00002bc. initialize()'s `this` is not logged, so it is open whether it is
the same object, or whether setFirmwareDirectory stores elsewhere than 0x570.
tOS status 0x0010ab0d. Full log: logs/20261005-100702-plugin/peer.plist.
setFirmwareDirectory (0x12660f40) stores the directory at this+0x958 (returns
0xe00002bc for a null dir); it does not touch +0x570. In initialize(input),
+0x570 is only written from input->field_10 (non-null) — boot 9/10 input[2]=0.

Boot 11 (2026-10-05 10:33, milestone 2i, UUID A18DCB2B): no panic. Same
AmdTtlServices object (0xffffff8bad3a8000) in setFirmwareDirectory and
initialize(input); +0x570 is 0 before and after setFirmwareDirectory (which
writes +0x958). So +0x570 can only come from input->field_10, and the
accelerator (X6000) passes field_10 = 0. tOS status 0x0011cca7. Full log:
logs/20261005-103402-plugin/peer.plist.

Boot 12 (2026-10-05 10:43, milestone 2j, UUID 5EB49C53, boot-arg
-rbgpusync570): no panic. setFirmwareDirectory: +0x958 = dir
0xffffff8baab13500, +0x570 still 0; the plugin copied +0x958 into +0x570 just
before initialize(input). First time past that check: HWLibs "initialize
normal", inner __ZN14AmdTtlServices10initializeEv entered (same object), then
HWLibs "ttl_initialize failed status = 0x00000001"; inner and outer
initialize return 0xE00002C7 (kIOReturnUnsupported), X6000 "TTL::initialize()
Failed! Error code: 0xe00002c7". Failure path otherwise unchanged (state
trace u64AllAccelStart 0x20c5). Whether +0x570 really should hold the
firmware directory is unverified. tOS status 0x0011d344. Full log:
logs/20261005-104409-plugin/peer.plist.

Boot 13 (2026-10-05 10:49, milestone 2k, UUID 1CFB2533, -rbgpusync570): no
panic. _ttl_initialize hooked: entered with rdi=0xffffffaa95beb818 (the
0xa8-byte block initialize() builds at -0xe8(%rbp)) and rsi=this+0x38, returned
status 1 about 3 us later. The call site (0x12660232-0x1266023d) loads only
rdi and rsi, so the hook's arg3..arg6 (1, 0xffffffff, 0x10001, 1) are leftover
registers. tOS status 0x0011922c. Full log: logs/20261005-104921-plugin/peer.plist.

Boot 14 (2026-10-05 10:58, milestone 2l, UUID 62DAC5BA, -rbgpusync570): no
panic. _ttl_initialize parameter block (0xa8 bytes, read only):
+0x00..0x18 pointers into the AmdTtlServices object (this+0x50, +0x3f0, +0x528,
+0x540); +0x20 0x0000003600001681 (device 0x1681, 0x36 = 54 as in "[54:0:0]");
+0x28 0; +0x30 0xffffffd2d7814000; +0x38 doorbell base; +0x40 0x200000;
+0x48..0x60 0; +0x68 0x10000000; +0x70 0x40000000; +0x78 0; +0x80
0x0000000a00000000; +0x88 0; +0x90 this+0x5a0; +0x98/+0xa0 0. Still status 1.
tOS status 0x00119c10. Full log: logs/20261005-105923-plugin/peer.plist.

Boot 15 (2026-10-05 11:13, milestone 2m, UUID BE062C1E, -rbgpusync570
-rbgpuspoofttl): no panic. _ttlIsSupportedDevice(params+0x20): devId 0x1681,
bus 54; the hook swapped the ID to 0x73BF for the original call and it still
returned 0. So the check is not the device ID alone (or 0x73BF is not in its
list). _ttl_initialize -> status 1; _ttl_sw_init / _ttl_hw_init never ran, no
GPU programming happened. tOS status 0x0011f061. Full log:
logs/20261005-111348-plugin/peer.plist.

Boot 16 (2026-10-05 12:59, milestone 2n, UUID D656E607, -rbgpusync570 only):
no panic. _ttlIsSupportedDevice: devId 0x1681, bus 54, attr (params+0x84) =
0xA; original returned 0, no override (no -rbgpuspoofttl), so _ttl_hw_init
did not run. _DeviceCapabilityTbl: 93 entries of 0x50; Navi 21 entries have
+0x20 = 0 (73a0/a1/a2/a3/ab/ae) or a revision-like value (73a5/73af 0xc0,
73bf 0x40..0xd3); none matches attr 0xA, Fiji/Vega use 0xdeadcafe.
tOS status 0x0010b43f. Full log: logs/20261005-130013-plugin/peer.plist.

Boot 17 (2026-10-05 13:02, milestone 2n, UUID D656E607, -rbgpusync570
-rbgpuspoofttl): no panic, no hang. _ttlIsSupportedDevice original 0, override
-> 1; _ttl_sw_init ran for the first time: HWLibs "AMD TTL COS: [54:0:0]:
Write PCI Config offset 0x60 size 2 data 0x2930" and "[54:0:1]: ... data
0x20" (first hardware writes by Apple's code on this chip; config space resets
at power-on), then ASSERTs: ttlSetDeviceCapabilityEntry (ttl_device.c:588)
"Could not find device info table entry!"; ipi_bgm_create (ipi_bgm.c:187)
"Failed to create bgm context"; IpiValidateTopology (ttl_ipi.c:616)
"pBgmContext is null!"; TlsSwInit (ttl_tls.c:129) "Failed to create TLS".
_ttl_initialize status 4 -> 0xe00002bc. _ttl_hw_init did not run. tOS status
0x00117135. Full log: logs/20261005-130250-plugin/peer.plist.

Boot 18 (2026-10-05 13:28, milestone 2p, UUID 57F7D172, -rbgpusync570
-rbgpuspoofttl): no panic, no hang. _DeviceCapabilityTbl resolved
(0xffffff7f9d9d7ed0); entry 0 (0x7300) overwritten with a copy of entry 60
(0x73A2), device ID set to 0x1681, read-back 0x1681. _ttlIsSupportedDevice
original still 0 (attr 0xA), override -> 1. _ttl_sw_init: same two PCI
config 0x60 writes, then the SAME assert chain: ttlSetDeviceCapabilityEntry
(ttl_device.c:588) "Could not find device info table entry!", ipi_bgm_create,
IpiValidateTopology, TlsSwInit; _ttl_initialize status 4 -> 0xe00002bc.
So a 0x1681 device ID in _DeviceCapabilityTbl (cloned entry's +0x20 = 0) is
not what ttlSetDeviceCapabilityEntry looks for. _ttl_hw_init did not run.
tOS status 0x00116e2e. Full log: logs/20261005-132913-plugin/peer.plist.

Boot 19 (2026-10-05 13:35, milestone 2q, UUID CDBE2280, -rbgpusync570
-rbgpuspoofttl): no panic, no hang. Cloned _DeviceCapabilityTbl entry 0 now
0x1681 with +0x20 = 0xA. _ttlIsSupportedDevice ORIGINAL returned 1 (first
time without the override): the table entry satisfies _DevIsSupportedDevice.
_ttl_sw_init: same two PCI config 0x60 writes, then the SAME assert chain
(ttlSetDeviceCapabilityEntry ttl_device.c:588 "Could not find device info
table entry!", ipi_bgm_create, IpiValidateTopology, TlsSwInit); status 4 ->
0xe00002bc. So ttlSetDeviceCapabilityEntry (0x1264698d) does not find its
entry by device ID + attr in _DeviceCapabilityTbl. _ttl_hw_init did not run.
tOS status 0x0011395f. Full log: logs/20261005-133521-plugin/peer.plist.

Boot 20 (2026-10-05 13:42, milestone 2r, UUID A121DF03, -rbgpusync570
-rbgpuspoofttl): no panic, no hang. Capability entry 0 = 0x1681 with
+0x18 = +0x20 = 0xdeadcafe. _ttlIsSupportedDevice original 1. NEW: no
ttlSetDeviceCapabilityEntry / ipi_bgm / IpiValidateTopology asserts: the
device info entry is found and the IP topology passes. After the two PCI
config 0x60 writes, ~0.5 ms later the first IP block's SW init fails:
"TTL Event source_id=2, event_id=0x900c0401, type 3, hw_id=0,
swip_client_id 6 (SW_IP_CLIENT_ID__PSP, EVENT__SW_INIT)"; ASSERTs
logSwipFailure (ttl_tls.c:504) "PSP init/power-up failed",
TlsExecuteIpEntrySeq (ttl_tls.c:357) "SWIP init/power-up failed", TlsSwInit
(ttl_tls.c:129) "Failed to create TLS"; status 4 -> 0xe00002bc. The SWIP
error log in the failure path now carries this PSP event. PSP registers in
the dump unchanged (tOS 0.28.0.66, Ready/POST 0xffffffff, PC 0).
_ttl_hw_init did not run. This is the PSP (MP0 13.0.x vs Navi 21's PSP 11)
wall. tOS status 0x00113b1b. Full log: logs/20261005-134308-plugin/peer.plist.

Boot 21 (2026-10-05 13:52, milestone 2t, UUID 747C7A65, -rbgpusync570
-rbgpuspoofttl): no panic, no hang. _IpiExecuteSwipEntryPoint logged
(instance 0xffffffd7b21a0000, context 0 each time), in order:
client 3 event 0 -> 1 (8 us); client 5 event 0 -> 1 (32 us); client 6 (PSP)
event 0 (SW_INIT) -> 0 after 21 us; then the PSP assert chain as boot 20;
then client 5 event 2 -> 1 and client 3 event 2 -> 1 (the two that had
initialised, in reverse order); _ttl_initialize status 4. The PSP SW_INIT
failure takes ~21 us, so it is decided without waiting on hardware.
tOS status 0x00117443. Full log: logs/20261005-135303-plugin/peer.plist.

Boot 22 (2026-10-05 14:18, milestone 2v, UUID 09DC6974, -rbgpusync570
-rbgpuspoofttl): no panic, no hang. _IpiGetDiscoveryInfo logged 12 calls
(instance 0xffffffd7d8d6b000): a first pass over clients 3, 5, 6, 8, 7, 2,
14, 9, 15, 10 — client 3 -> NULL, all others non-NULL (consecutive pointers
0xc64 apart, client 5 at 0xffffffd7d9437ecc); then client 5 again inside its
SW_INIT, and client 6 inside _ipi_psp_sw_init (dev 0xffffffd7d8ec4000) ->
0xffffffd7d9438b30, NOT NULL. _ipi_psp_sw_init still returned 0, ~20 us
after that call. So the PSP SW_INIT failure is after the discovery lookup
(Gemini's NULL-discovery explanation ruled out). Client 3's SW_INIT succeeds
although its discovery info is NULL. tOS status 0x0012e33a. Full log:
logs/20261005-141942-plugin/peer.plist.

Boot 23 (2026-10-05 14:59, milestone 2w, UUID 1A2123CD, -rbgpusync570
-rbgpuspoofttl): no panic, no hang. _ipi_psp_sw_init's callback at dev+0x8
= 0xffffff7fb0131430 = HWLibs 0x12648430 _psp_sw_init (named by ptrsym.py).
_ipi_psp_sw_init returned 0 ~13 us after the (non-NULL) discovery lookup, so
_psp_sw_init returns nonzero. Rest identical to boot 22. tOS status
0x0012e20d. Full log: logs/20261005-145934-plugin/peer.plist.

Boot 24 (2026-10-05 15:10, milestone 2x, UUID 15B0DDBB, -rbgpusync570
-rbgpuspoofttl): no panic, no hang. Inside the PSP SW_INIT, in order:
_psp_sw_init(p1 size 0x228, +8 = 1, +218/+220 non-NULL; p2 size 0x10) passes
its argument checks; _psp_cos_alloc_memory(size 0x7e00) -> 0; 
_psp_asic_type_init reads the discovery copy as MP0 13.0.3 (+8 = 0xd,
+0xc = 0, +0x10 = 3: the chip's real PSP version) and returns 0 with
asic_type (buf+0x7d44) = 19; _psp_init_pfn_ptr(asic_type 19) returns 1;
_psp_sw_init returns 2 (_psp_read_config not reached). So HWLibs maps PSP
13.0.3 to an asic type (19) but _psp_init_pfn_ptr has no function set for
it. tOS status 0x0012e851. Full log: logs/20261005-151045-plugin/peer.plist.

Boot 25 (2026-10-05 17:32, milestone 2aa dry run, UUID 0782EA5A,
-rbgpusync570 -rbgpuspoofttl, no -rbgpupsphw): no panic, no hang. The
asic_type 19 -> 8 swap bound the PSP _11_0 table (ring_init/ring_create/
is_sos_running _11_0, named by ptrsym.py); _psp_read_config ran (flags +0x28
= 0); _psp_sw_init returned 0 and PSP (client 6) SW_INIT passed. Client 8
(SMU) SW_INIT then ran the plugin's own SMU 13 slot table for the first time
(smu_init_function_pointer_list ver 0xd rev 0x3; slot_internal_sw_init:
rb_smu13_init, sends nothing; smu_cgs_alloc_memory gpu=0xffffffe0e044a000,
"carve-out verification FAIL"; the code then allows RB_SMU13_ALLOW_TABLES,
committed in 40bd4c6 — beyond queries, the user's decision, not given).
Client 7 (GC) SW_INIT failed after 20 us: logSwipFailure ttl_tls.c:507 "GC
init/power-up failed"; teardown event 2 for clients 8, 6, 5, 3;
_ttl_initialize status 4. _ttl_hw_init was never reached, so the dry-run
gate was not exercised and no PSP/SMU message was sent. HAZARD before any
-rbgpupsphw boot: slot_internal_hw_init -> rb_smu13_bringup would send
SetDriverDramAddrHigh/Low with that unverified address and TransferTable
(the SMU writes a table there). tOS status 0x00116165. Full log:
logs/20261005-173249-plugin/peer.plist.

Boot 26 (2026-10-05 18:16, milestone 2ad dry run, UUID D15DFDB4,
-rbgpusync570 -rbgpuspoofttl): no panic, no hang. Same path as boot 25 to
the GC; SMU slot now "queries only". GC discovery info (client 7): 1
instance (span 0x210 < 0x235), GC 10.3.3 (major 10, minor 3, rev 3: the
chip's real GC). _ipi_gc_sw_init's callback = HWLibs 0x126a7f74 _gc_sw_init
(ptrsym.py), called with p1 size 0x238 (+0xc/+0x10/+0x14 = 0xa/3/3), p2
size 0x428; it returned 2 after 18 us; _ipi_gc_sw_init -> 0, GC assert
chain, status 4. _IpiSetFwEntry ran once, inside the SMU SW_INIT (fw_id 0
-> 1). The *_routed diag keys now read 0 although every hook installed (the
2ac install loop logs instead of setting them). tOS status 0x0011876f. Full
log: logs/20261005-181648-plugin/peer.plist.

Boot 27 (2026-10-05 18:26, milestone 2ae dry run, UUID 6A1480F0,
-rbgpusync570 -rbgpuspoofttl): no panic, no hang. Inside _gc_sw_init:
_gc_get_hw_version -> 0x000A0303; _gc_get_config ran; _gc_get_hw_config_tbl_info
-> table size (+0xd38) 0x64; _gc_set_fw_entry_info(gc_buf, 0x30000000a, 3,
p2) called _gc_get_hw_version again and returned 2; _gc_sw_init returned 2.
_gc_init_fcn_ptr_list was NOT reached, so the 2ae 10.3.3 -> 10.3.0 swap never
ran: the first GC failure is the firmware-entry step, which comes before the
function-table step (boot 26 failed here too; the static reading that blamed
the 0x35 mask was a later check). The RBGPULog ring (256) now drops the
first lines (log starts at "HWLibs loaded"). tOS status 0x00115136. Full log:
logs/20261005-182716-plugin/peer.plist.

Decision (2026-10-05, after boot 27): Gemini confirmed HWLibs embeds only
Navi 21 (GC 10.3.0) GC microcode (_gc_10_3_*_ucode descriptors in __DATA,
loaded through the PSP, which checks the per-ASIC signature), so a version
swap at _gc_set_fw_entry_info cannot work. The user chose to try the 680M's
own firmware (Linux linux-firmware amdgpu/yellow_carp_*.bin) in place of the
embedded images. Firmware stays in firmware/ (gitignored). 2af (ring 512,
read-only fw hooks) was not committed: its paste dropped the four TTL hooks
incl. -rbgpusync570.


===== FILE: docs/PLAN.md =====

# Plan

The route: reuse Apple's own Navi 21 (RDNA2, gfx10.3) driver stack —
AMDRadeonX6000 accelerator, X6000 Metal driver, X6000Framebuffer — on the 680M,
whose graphics core is the same gfx10.3 generation, and replace what differs on
an APU. That is what NootedRed did for Vega APUs with Apple's Vega (X5000)
driver. Unlike Navi48-MacOS (RX 9070 XT), no command-stream translation should
be needed: Apple's Metal driver already emits gfx10.3 PM4.

## Step 0 — read-only probe (done: levels 1 and 2 ran, see docs/RULES.md)

Map the register BAR, read the IP discovery table and the PSP/SMU/memory-hub
state, change nothing. Answers: is the table where Linux looks for it, which IP
versions does this chip report, is the PSP's secure OS already running, does
the SMU answer, where is the carve-out in system RAM, does BAR0 window the
carve-out directly.

## Next steps (each its own kext stage, each read-mostly before it writes)

1. SMU messages: ask the SMU its firmware version, driver-interface version
   and enabled features (first register writes, via the mailbox Linux uses;
   yellow_carp_ppt.c). Built: rb_smu.c, `make kext SMU=1`.
2. PSP: check the TOC/TA state Linux checks before loading firmware
   (psp_v13_0.c); load nothing yet.
3. Hook Apple's X6000 Navi 21 accelerator from a Lilu plugin: identify the
   Navi 21 code paths that touch PSP 11 / SMU 11 / VRAM, map them against
   NootedRed's X6000FB.cpp, which already does part of this for Renoir.
   Static analysis done: docs/APPLE-STACK.md. Display (no DCN 3.1 in Apple's
   framebuffer) decides the route and comes first.
4. Memory: Apple's driver assumes VRAM; the carve-out (step 0 tells where and
   how large) has to look like it.
5. Display: Apple's X6000Framebuffer knows DCN 3.0, the 680M has DCN 3.1.2.
   The least certain part.

## References

- Linux amdgpu (yellow_carp / Rembrandt paths) — the hardware truth.
- NootedRed (ChefKissInc) — APU patches on Apple's AMD drivers.
- Navi48-MacOS (Almosst-DEV) — hooks into AMDRadeonX6000 on macOS 26.6.2;
  offsets do not carry over to 15.8.1.


===== FILE: docs/APPLE-STACK.md =====

# Apple's AMD Navi stack on macOS 15.8.1, measured against the 680M

Static analysis, 2026-10-04. Nothing here was loaded. Binaries come from
`/System/Library/KernelCollections/SystemKernelExtensions.kc` of this
machine (macOS 15.8.1, 24H32; AMD kexts 7.0.25). The hardware facts come
from the probes recorded in docs/RULES.md, the Linux facts from torvalds/linux
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

Detailed spec: docs/SPEC-A-START.md (also adds B0: the VBIOS image, `ATY,bin_image`).

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
   Otherwise the plug-in goes into the EFI, which docs/RULES.md keeps for the
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


===== FILE: docs/SPEC-A-START.md =====

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


===== FILE: docs/SPEC-SMU-SLOTS.md =====

# B3 in detail: SMU 13 behind Apple's SMU slots

What the hook layer has to do so AMDRadeonX6800HWLibs runs its power
management on the 680M's MP1 13.0.3 through `src/common/rb_smu13.{c,h}`.
Everything here was read from macOS 15.8.1's AMDRadeonX6800HWLibs (kexts
7.0.25) with tools/re (`switches.py` decodes the command tables). Addresses are
file addresses of the extracted kext (`build/re/AMDRadeonX6800HWLibs`); resolve
by symbol name, never by address. All names below are in the kext's symbol
table (local `t` symbols).

This item is only reached once HWServices loads X6800HWLibs for 0x1681 and
discovery passes (APPLE-STACK.md A1-A3, B1, B2). It is not the first hook.

## 1. How HWLibs reaches the SMU

```
ipi_smu_create_context            IP block 8; IpiSetSwipEntryPoints(8, {sw_init, sw_exit, hw_init, hw_exit})
  smu_sw_init(ctx, ...)           allocates the SMU context (below), then
    smu_init_function_pointer_list(ctx, ver, rev)     <-- fails today: "Unsupported hw version!"
    smu_internal_sw_init          -> slot +0x650, then smu_get_fw_constants -> slot +0x738
  smu_hw_init(ctx, ?, iface)      -> slot +0x658; then fills `iface` with the dispatchers
  smu_hw_exit / smu_sw_exit       -> slots +0x668 / +0x660
ipi_smu_notify_event              -> iface event_notify -> slot +0x680
ipiSmuPerformFullAsicReset        -> iface full_asic_reset -> slot +0x678
```

Outside the SMU code, HWLibs calls only those two interface entries (events
and full reset). The other dispatchers (`smu_dpm_control`,
`smu_power_control`, `smu_gfx_off_control`, ...) are published in the
interface table but no HWLibs code calls them. Whether the accelerator kext
calls them is open (section 8).

### Why it fails

`smu_get_hw_version(ver, rev)` packs `major<<16 | minor<<8 | rev`, where
`major = ver & 0xff`, `minor = (ver >> 32) & 0xff`, `rev = rev & 0xff`, and maps
the result to an index:

| MP1 | index | MP1 | index |
|---|---|---|---|
| 9.0.0 | 0x00 | 11.0.13 | 0x0A |
| 9.0.1 | 0x01 | 11.5.0 | 0x0B |
| 10.0.0 | 0x02 | 12.0.0 | 0x0C |
| 11.0.0 | 0x03 | 12.0.1 | 0x0D |
| 11.0.2 | 0x04 | 13.0.0 | 0x0E |
| 11.0.5 | 0x05 | 13.0.1 | 0x0F |
| 11.0.7 | 0x06 | **13.0.3 (680M)** | **0x10** |
| 11.0.9 | 0x07 | 13.0.4 | 0x11 |
| 11.0.11 | 0x08 | 13.0.5 | 0x12 |
| 11.0.12 | 0x09 | anything else | 0x14 |

`smu_init_function_pointer_list` stores the index at ctx+0x278, zeroes the
slots (ctx+0x650, 0xF0 bytes), calls `smu_read_config_space(ctx)`, and then:
- if `ctx+0x288 & 1` is clear, it accepts only indexes 6, 8, 9 and 10 (mask
  0x740) and calls `smu_11_0_7_create_function_pointer_list` for them;
  anything else calls `smu_assertion` ("Unsupported hw version!") and
  returns 2;
- if `ctx+0x288 & 1` is set, it builds the list only when bit 1 is also set
  and the index is 6; otherwise it returns 0 with every slot empty;
- in both cases, if `ctx+0x2e8 == 1`, it then calls `smu_update_function_pointers`.

## 2. The hook (H-SMU-1)

Replace `smu_init_function_pointer_list(ctx, ver, rev)`:

1. Compute the index with Apple's own `smu_get_hw_version(ver, rev)` (pure, no
   side effects).
2. Not 0x10: call the original unchanged and return its result. Every other
   ASIC keeps Apple's path.
3. 0x10: do what the original does on success, with our slots:
   - store 0x10 at ctx+0x278;
   - zero ctx+0x650 .. ctx+0x740;
   - call `smu_read_config_space(ctx)`;
   - fill the slots of section 5 (leave the rest zero);
   - return 0.

   Do not call the original first: its failure path runs `smu_assertion`,
   which hands the message to an OS-services callback (ctx+0x10 -> +0xF8)
   whose behaviour was not traced. Skip `smu_update_function_pointers` too:
   ctx+0x2e8 == 1 is an emulation mode, which a 0x1681 never uses.

The `-rbgpuoff` boot argument must leave this hook uninstalled.

## 3. The SMU context (what the slots receive as `ctx`)

| Offset | Meaning | Used for |
|---|---|---|
| +0x008 | register services (CGS), see below | registers, GPU memory |
| +0x010 | OS services, see below | delay, system memory, logging |
| +0x044 | u32 array: register base (dword) per base index | `smu_cgs_*_register` adds `[idx]` |
| +0x278 | u32 MP1 version index (0x10 for us) | set by the hook |
| +0x27C | u32 register-window limit (bytes) | bounds check |
| +0x280 | u8: bounds check enabled | |
| +0x288 | u16 flags (bit 0, bit 1: see section 1) | |
| +0x2C0 | u32 SMU 11 response timeout | not needed |
| +0x360 | SMU 11 table context (pptable, FB allocations) | **not ours, do not use** |
| +0x650 .. +0x738 | the 27 slots | section 5 |

Our own state (an `struct rb_smu13` plus the driver-table handle) has no field
in this context. Keep it in the plugin, keyed by `ctx` (one GPU: a single
static is enough). Do not reuse +0x360: common code
(`smu_query_feature_enabled`, `smu_svc_*`, the dispatchers) reads it.

### Register services (`*(ctx+0x8)`)

| Offset | Function | Signature as `smu_cgs_*` calls it |
|---|---|---|
| +0x08 | handle | first argument of every call below |
| +0x10 | allocate GPU memory | `(handle, req*, out*)`; `smu_cgs_alloc_memory` wraps it |
| +0x38 | write register | `(handle, dword_index, value, flags)` |
| +0x40 | read register | `u32 (handle, dword_index, flags)` |
| +0xB8 / +0xC0 | write / read beyond the window (indirect) | not needed |

Apple's own SMU 11 code goes through the wrappers
`smu_cgs_read_register(ctx, reg, base_idx, flags, 4)` and
`smu_cgs_write_register(ctx, reg, base_idx, value, 4, 0)`. They add
`ctx+0x44[base_idx]` to `reg` and pass the absolute dword index to +0x40 / +0x38.
`smu_11_0_7_send_message` uses base index 0 with 0x282 / 0x292 / 0x29A. These
are the same C2PMSG_66 / 82 / 90 registers that rb_smu13 uses, so
`ctx+0x44[0]` should be MP1's base (0x16000 on this machine).

### OS services (`*(ctx+0x10)`)

| Offset | Function |
|---|---|
| +0x08 | handle |
| +0x10 | allocate system memory (`smu_cos_alloc_memory`) |
| +0x80 | `delay_us(handle, us)` (`smu_cos_delay_in_us`) |
| +0xF8 | assertion / log callback (`smu_assertion`) |

## 4. Gluing rb_smu13 to the context

- `struct rb_hw` for `rb_smu13_init`:
  - `rreg(byte_off)`: call the CGS read (+0x40) with `byte_off / 4`.
  - `wreg(byte_off, v)`: call the CGS write (+0x38) with `byte_off / 4`.
  - `delay_us`: `smu_cos_delay_in_us`.
  - `mmio_len`: ctx+0x27C.
  - rb_smu13 only ever writes the three mailbox registers.
- `struct rb_disc` for `rb_smu13_init`: a two-entry table built by hand.
  - MP1, version 13.0.3, `base[0] = ctx+0x44[0]`.
  - SMUIO, `base[0] = 0x16800`, only for `rb_smu13_get_gfxoff_status`. The
    SMUIO base is not in the SMU context; leave the entry out and that one
    call returns `RB_SMU13_E_INVAL`.
- Driver table (`struct rb_smu13_dma`): allocate it once, in the sw_init slot,
  with `smu_cgs_alloc_memory(ctx, 0x1000, 0x1000, 1, &handle, &gpu_addr, &cpu_addr)`.
  This is the call, size class, alignment and domain (1) Apple uses for its
  own driver table (`smu_11_0_7_core_initialize_smc_tables`: size 0x684,
  align 0x1000, domain 1).
  - `mc = gpu_addr`, `cpu = cpu_addr`, `size = 0x1000`.
  - `flush` / `invalidate`: NULL for the first test (see section 7).
- Message groups: `rb_smu13_init` allows queries only. Which other groups to
  allow is a decision for the user (section 6).

## 5. Slots

Return codes the dispatchers and callers expect, from Apple's slots:
0 OK, 1 bad parameter, 2 failure, 3 timeout (`smu_11_0_7_send_message`), and 4
not supported. A dispatcher whose slot is NULL returns 4 itself.

rb_smu13 errors map to these as follows:

| rb_smu13 error | Apple code |
|---|---|
| `E_INVAL` | 1 |
| `E_FW`, `E_VERSION`, `E_NODATA` | 2 |
| `E_BUSY`, `E_TIMEOUT`, `E_DEAD` | 3 |
| `E_NOT_ALLOWED` | 4 |

| Slot | Apple name | Signature | Ours |
|---|---|---|---|
| +0x650 | internal_sw_init | `(ctx, ...)` | Allocate our state and the driver table (section 4); `rb_smu13_init`. Return 0, or 2 if allocation fails. |
| +0x658 | internal_hw_init | `(ctx)` | `rb_smu13_bringup`: versions (interface must be 4), feature mask, driver table address, DPM clock table. Linux's APU path; none of Apple's dGPU steps (VBIOS pptable, DC BTC, memory pool) apply. |
| +0x660 | internal_sw_exit | `(ctx)` | Free the driver table and our state. |
| +0x668 | internal_hw_exit | `(ctx)` | Apple: unload message, then table teardown. Ours: `rb_smu13_prepare_unload` (needs ALLOW_UNLOAD). Linux skips it when entering s0ix. |
| +0x678 | full_asic_reset | `(ctx, ...)` | Leave NULL (4). Linux's APU reset is mode 2 (`rb_smu13_mode2_reset`); wire it only after a reset is understood end to end. |
| +0x680 | notify_event | `(ctx, struct {u32 size=0x10; u32 event; u64 *param}*, ...)` | SMU events 0, 4, 6, 10: Apple runs `power_up` (re-init after resume); ours runs `rb_smu13_bringup` again (Linux re-runs hw setup on resume). Event 9: Apple switches DF C-states (no Yellow Carp equivalent): return 0. Event 11 (debug dump): return 0. Others: 0. Event > 11: 2. |
| +0x6E0 | gfx_off_control | `(ctx, u32 op)` | op 0 enable, 1 disable, else 4. Enable: `rb_smu13_gfx_off_control(1)`, preceded once by `rb_smu13_enable_gfxoff` (Linux's post_init). Disable: `rb_smu13_gfx_off_control(0)`. Needs ALLOW_GFXOFF. |
| +0x6E8 | system_features_control | `(ctx, u64 *mask)` | Apple enables DPM features. On Yellow Carp the firmware already runs them all (47 enabled on this machine) and Linux sends nothing on enable. Return 0. |
| +0x6F8 | power_feature_caps | `(ctx, u64 *caps)` | Apple ORs capability bits from SMU 11 feature bits. Start with `*caps = 0`, return 0, and map individual bits only once a caller is seen reading them. |
| +0x720 | unload_smu | `(ctx, ...)` | `rb_smu13_prepare_unload`. |
| +0x738 | get_ucode_consts | `(ctx, out[16])` | Leave NULL. It describes SMU firmware for the driver to load, and on an APU the firmware is already running. `smu_get_fw_constants` then reports "no firmware" (to verify, section 7). |
| +0x6B8 | dpm_control | `(ctx, u32 cmd, in*, out*)` | NULL first (section 8 lists Apple's commands and the rb_smu13 call each maps to). |
| +0x6C0 | power_control | `(ctx, u32 cmd, in*, out*)` | NULL first. |
| others | overdrive, thermal, fan, ips, azalia, ulv, fullscreen, soft_table, pm_log, push_to_stb, dummy_hw_init | | NULL (4). |

## 6. Decisions for the user before the first boot with this hook

Each RB_SMU13_ALLOW_* group changes what the hook may send. Minimum for
bring-up:

- **ALLOW_TABLES**: sets the driver table address and copies the DPM clock
  table into it. It changes only where the SMU writes table data, into memory
  the driver owns.
- **ALLOW_GFXOFF** (needed for slot +0x6E0): lets the SMU power the graphics
  core off when idle. Linux does this on every boot of this chip.
- **ALLOW_UNLOAD** (+0x668 / +0x720): tells the SMU the driver is going away.
  It runs only at shutdown or driver unload.

Leave CLOCKS, MMPOWER and RESET off until a caller needs them.

## 7. To check on the first boot (log each one)

1. `ctx+0x44[0]` is 0x16000 and `ctx+0x27C` covers it (the glue's MP1 base).
2. `smu_cgs_alloc_memory` returns 0, and `gpu_addr` lies in the carve-out
   (0xF4_0000_0000 - 0xF4_FFFF_FFFF).
3. `rb_smu13_bringup` returns 0, and the DPM table looks sane: GFX min/max
   in MHz, 1-8 SOC levels, 1-4 DF P-states.
4. The DPM table read back unchanged with NULL flush/invalidate. If it is all
   zero or stale, add HDP flush/invalidate (Linux: `amdgpu_hdp_flush` /
   `_invalidate`), whose Apple equivalent is not traced yet.
5. `smu_get_fw_constants` with slot +0x738 empty: sw_init still succeeds.
6. Which dispatchers return 4, and who called them, if anything shows up in
   the log.

## 8. Open: the control commands

Apple's command tables (`tools/re/switches.py`), for when a caller turns up:

`dpm_control` (+0x6B8):
- 0 force highest, 1 force lowest, 2 unforce: `rb_smu13_set_performance_level` HIGH / LOW / AUTO.
- 3 get clock limits (in, out), 0xA get all queried clock limits: `rb_smu13_get_dpm_ultimate_freq`.
- 4 set clock limit (in), 0xB set all clock limits: `rb_smu13_set_soft_freq_range`.
- 5 get current DPM (out): `rb_smu13_get_current_clk`.
- 6 set workload policy; 7/8 UCLK fast switching on/off; 0xC get DPM table: `rb_smu13_get_dpm_freq_by_index`.
- 0xD voltage limits; 0xE PCIe parameters (no PCIe link on an APU); 0x12 copy of 6 pptable dwords; 0x13 disable DPM prefetcher.
- 9, 0xF-0x11: assertion (invalid).

`power_control` (+0x6C0): 0 power source (AC/DC), 1/2/3 set / get / restore
power limit, 4 query feature enabled, 5 send message, 6/7 deep sleep on/off,
8/9 power gating on/off, 0xA power limit by DPM, 0xB/0xC send message with
parameter, 0xD/0xE graphics power optimisation off/on, 0xF SmartShift delta gain.

The structs behind `in` / `out` are not traced. Trace them only for commands
seen being called.


===== BOOT 27 LOG (logs/20261005-182716-plugin/peer.plist) =====

```
RBGPULog:
  [4843287723] HWLibs loaded at 0xffffff7fa597b000 (size 0xc45000)
  [4843294636] Patching com.apple.kext.AMDRadeonX6800HWLibs
  [4843467631] HWLibs: resolved __ZL15deviceTypeTable symbol at 0xffffff7fa5dc4d60
  [4843476597] HWLibs: patched __ZL15deviceTypeTable at index 66: 0x005D -> 0x1681 (type 6, verified readback: 0x1681, type 6)
  [4843543964] HWLibs: resolved _DeviceCapabilityTbl symbol at 0xffffff7fa5dd7ed0
  [4843553071] HWLibs: patched _DeviceCapabilityTbl at index 0: cloned from index 60, set deviceId=0x1681, +0x18=0xdeadcafe, +0x20=0xdeadcafe (readback id: 0x1681)
  [4843604116] HWLibs: hooked __ZN14AmdTtlServices10initializeEP30_TtlLibraryInitializationInput
  [4843645514] HWLibs: hooked __ZN14AmdTtlServices10initializeEv
  [4843679848] HWLibs: hooked __ZN14AmdTtlServices20setFirmwareDirectoryEP20AMDFirmwareDirectory
  [4843696860] HWLibs: hooked _ttl_initialize
  [4843715696] HWLibs: hooked _ttlIsSupportedDevice
  [4843732377] HWLibs: hooked _ttl_hw_init
  [4843750180] HWLibs: hooked _IpiExecuteSwipEntryPoint
  [4843767813] HWLibs: hooked _IpiGetDiscoveryInfo
  [4843786699] HWLibs: hooked _ipi_psp_sw_init
  [4843805223] HWLibs: hooked _ipi_psp_hw_init
  [4843823828] HWLibs: hooked _psp_sw_init
  [4843855538] HWLibs: hooked _psp_hw_init
  [4843874213] HWLibs: hooked _psp_cos_alloc_memory
  [4843892487] HWLibs: hooked _psp_asic_type_init
  [4843910922] HWLibs: hooked _psp_init_pfn_ptr
  [4843928955] HWLibs: hooked _psp_read_config
  [4843947250] HWLibs: hooked _psp_allocate_private_mem
  [4843965524] HWLibs: hooked _psp_ring_init
  [4843983908] HWLibs: hooked _psp_ring_create
  [4844002573] HWLibs: hooked _psp_bootloader_is_sos_running
  [4844020948] HWLibs: hooked _psp_cmd_km_init
  [4844039513] HWLibs: hooked _psp_tmr_init
  [4844058148] HWLibs: hooked _psp_tmr_load
  [4844079237] HWLibs: hooked _psp_dtm_init
  [4844104675] HWLibs: hooked _psp_np_fw_load
  [4844132156] HWLibs: hooked _ipi_gc_sw_init
  [4844157153] HWLibs: hooked _gc_sw_init
  [4844181909] HWLibs: hooked _gc_get_hw_version
  [4844206115] HWLibs: hooked _gc_get_config
  [4844230731] HWLibs: hooked _gc_get_hw_config_tbl_info
  [4844255648] HWLibs: hooked _gc_set_fw_entry_info
  [4844280304] HWLibs: hooked _gc_init_fcn_ptr_list
  [4844306844] HWLibs: hooked _gc_get_doorbell_requirements
  [4844326521] HWLibs: hooked _ttlDevGetDoorbellRange
  [4844343583] HWLibs: hooked _IpiSetFwEntry
  [4844361015] HWLibs: hooked _ipi_gc_hw_init
  [4844384619] HWLibs: hooked _gc_hw_init
  [4844448169] HWLibs: failed to resolve symbol _smu_cgs_free_memory (expected)
  [4844521967] H-SMU-1: smu_init_function_pointer_list hooked successfully
  [4845273355] AmdTtlServices::setFirmwareDirectory(this=0xffffff95a5356000, dir=0xffffff95a3820100) pre +0x570=0x0, pre +0x958=0x0
  [4845283033] AmdTtlServices::setFirmwareDirectory done: this=0xffffff95a5356000 post +0x570=0x0, post +0x958=0xffffff95a3820100
  [4845646284] (x2) SpecialAMDKey request 0x14 (wait: 1)
  [4845647717] X6000: [54:0:0] [Accel] --- Before TTL:initialize() - BootLoader Version=0x00280066, POST Code=0xffffffff, Ready=0xffffffff
  [4845649821] X6000: [54:0:0] [Accel] >>> Calling TTL::initialize()
  [4845651745] X6000:        nonlocalMemSizeLimitBytes   = 0x0000000040000000
  [4845652145] X6000:        pDoorbellBase               = 0xffffffdba48f6000
  [4845652466] X6000:        doorbellApertureSizeInBytes = 2097152 (2048 KB)
  [4845659199] AmdTtlServices::initialize(this=0xffffff95a5356000, input=0xffffffaa6ffafa08): [0]=0xfffffffffc11e690 [1]=0xfffffffffc11e6a8 [2]=0x0 [4]=0x40000000 [5]=0xffffffdba48f6000 [6]=0x200000
  [4845662485] AmdTtlServices state before (this=0xffffff95a5356000): 0x560=0x0 0x568=0x0 0x570=0x0 0x578=0x0 0x598=0x0 0x958=0xffffff95a3820100
  [4845664999] AmdTtlServices: -rbgpusync570 active; synced +0x570 from +0x958 (0xffffff95a3820100)
  [4845665921] HWLibs: [AMD INFO] TTL Interface: Boot mode Normal.
  [4845666913] HWLibs: initialize normal
  [4845802096] AmdTtlServices::initialize() entered (this=0xffffff95a5356000)
  [4845842823] _ttl_initialize entered: params=0xffffffaa6ffaf818, ctx_38=0xffffff95a5356038
  [4845846810] ttl_params[0x00..0x18]: 0xffffff95a5356050 0xffffff95a53563f0 0xffffff95a5356528 0xffffff95a5356540
  [4845852761] ttl_params[0x20..0x38]: 0x0000003600001681 0x0000000000000000 0xffffffd7af917000 0xffffffdba48f6000
  [4845857891] ttl_params[0x40..0x58]: 0x0000000000200000 0x0000000000000000 0x0000000000000000 0x0000000000000000
  [4845860686] ttl_params[0x60..0x78]: 0x0000000000000000 0x0000000010000000 0x0000000040000000 0x0000000000000000
  [4845863271] ttl_params[0x80..0x98]: 0x0000000a00000000 0x0000000000000000 0xffffff95a53565a0 0x0000000000000000
  [4845864834] ttl_params[0xA0]:      0x0000000000000000
  [4845867850] _ttlIsSupportedDevice(ptr=0xffffffaa6ffaf838): devId=0x1681, bus=54, attr=0x0000000A
  [4845869573] _ttlIsSupportedDevice original returned 1
  [4847629211] HWLibs: AMD TTL COS:  [54:0:0]: Write PCI Config offset 0x60 size 2 data 0x2930
  [4847633830] HWLibs: AMD TTL COS:  [54:0:1]: Write PCI Config offset 0x60 size 2 data 0x20
  [4848012990] _IpiGetDiscoveryInfo(instance=0xffffffd7b2390000, client_id=3) returned 0
  [4848024081] _IpiGetDiscoveryInfo(instance=0xffffffd7b2390000, client_id=5) returned 0xffffffd7b2b7eecc
  [4848035533] _IpiGetDiscoveryInfo(instance=0xffffffd7b2390000, client_id=6) returned 0xffffffd7b2b7fb30
  [4848045652] _IpiGetDiscoveryInfo(instance=0xffffffd7b2390000, client_id=8) returned 0xffffffd7b2b80794
  [4848053446] _IpiGetDiscoveryInfo(instance=0xffffffd7b2390000, client_id=7) returned 0xffffffd7b2b813f8
  [4848059478] _IpiGetDiscoveryInfo(client_id=7) -> 0xffffffd7b2b813f8 (inst=1, span=0x210 vs 0x235, major=10, minor=3, rev=3)
  [4848067903] _IpiGetDiscoveryInfo(instance=0xffffffd7b2390000, client_id=2) returned 0xffffffd7b2b8205c
  [4848074095] _IpiGetDiscoveryInfo(instance=0xffffffd7b2390000, client_id=14) returned 0xffffffd7b2b82cc0
  [4848079786] _IpiGetDiscoveryInfo(instance=0xffffffd7b2390000, client_id=9) returned 0xffffffd7b2b83924
  [4848085376] _IpiGetDiscoveryInfo(instance=0xffffffd7b2390000, client_id=15) returned 0xffffffd7b2b84588
  [4848093792] _IpiGetDiscoveryInfo(instance=0xffffffd7b2390000, client_id=10) returned 0xffffffd7b2b851ec
  [4848108690] _IpiExecuteSwipEntryPoint entered: instance=0xffffffd7b2390000, client_id=3, event_id=0, context=0
  [4848111164] _IpiExecuteSwipEntryPoint returned 1 (client_id=3, event_id=0)
  [4848114972] _IpiExecuteSwipEntryPoint entered: instance=0xffffffd7b2390000, client_id=5, event_id=0, context=0
  [4848117857] _IpiGetDiscoveryInfo(instance=0xffffffd7b2390000, client_id=5) returned 0xffffffd7b2b7eecc
  [4848149887] _IpiExecuteSwipEntryPoint returned 1 (client_id=5, event_id=0)
  [4848153724] _IpiExecuteSwipEntryPoint entered: instance=0xffffffd7b2390000, client_id=6, event_id=0, context=0
  [4848156199] _ipi_psp_sw_init entered: dev=0xffffffd7b2410000, fn_ptr@+0x8=0xffffff7fa5b31430
  [4848158734] _IpiGetDiscoveryInfo(instance=0xffffffd7b2390000, client_id=6) returned 0xffffffd7b2b7fb30
  [4848166528] _psp_sw_init entered: p1=0xffffffaa6ffaf3f8 (0x228, +8=0x1, +218=0xffffffd7b233d128, +220=0xffffffd7b233d000), p2=0xffffffaa6ffaf620 (0x10)
  [4848175525] _psp_cos_alloc_memory(ctx=0xffffffd7b233d000, size=0x7e00, align=1) -> ret=0, ptr=0xffffffd7b2b02008
  [4848179452] _psp_asic_type_init entered: buf=0xffffffd7b2b02008, disc=0xffffffaa6ffaf3fc (+8=0xd, +C=0x0, +10=0x3)
  [4848181476] _psp_asic_type_init returned 0 (asic_type@+0x7d44 = 19)
  [4848184131] _psp_init_pfn_ptr entered: buf=0xffffffd7b2b02008 (asic_type=19)
  [4848186415] _psp_init_pfn_ptr: -rbgpuspoofttl active -> spoofing asic_type 19 -> 8 for table binding
  [4848188159] _psp_init_pfn_ptr: restored asic_type to 19; ret=0
  [4848191114] _psp_init_pfn_ptr: pfn_ring_init=0xffffff7fa5b3df9c, pfn_ring_create=0xffffff7fa5b3e131, pfn_is_sos=0xffffff7fa5b3ee03
  [4848192517] _psp_init_pfn_ptr returned 0
  [4848197947] _psp_read_config exited: buf=0xffffffd7b2b02008, ret=0, flags@+0x28=0x0
  [4848199580] _psp_sw_init returned 0
  [4848201353] _ipi_psp_sw_init returned 2097153
  [4848203447] _IpiExecuteSwipEntryPoint returned 1 (client_id=6, event_id=0)
  [4848207385] _IpiExecuteSwipEntryPoint entered: instance=0xffffffd7b2390000, client_id=8, event_id=0, context=0
  [4848210751] _IpiGetDiscoveryInfo(instance=0xffffffd7b2390000, client_id=8) returned 0xffffffd7b2b80794
  [4848221010] hooked_smu_init_function_pointer_list called (ctx=0xffffffd7b2528008, ver=0xd, rev=0x3)
  [4848223214] smu_get_hw_version resolved to index 0x10
  [4848233774] H-SMU-1: registered SMU 13 slot table
  [4848244925] slot_internal_sw_init entered (ctx=0xffffffd7b2528008)
  [4848248582] [Check 1] ctx+0x44[0]=0x00016000 (expected 0x16000), window_limit=0x00080000
  [4848262007] [Check 2] smu_cgs_alloc_memory ret=0, handle=0xffff000, gpu=0xffffffd7b2534000, cpu=0xffffff9a6fa520a0 (gpu is CGS IOVirtualAddress)
  [4848264281] [Check 2] Carveout verification (0xF4_0000_0000..0xF4_FFFF_FFFF): FAIL
  [4848266656] rb_smu13 initialized with ALLOW_QUERY (queries only)
  [4848268199] [Check 5] slot_internal_sw_init succeeded
  [4848352146] _IpiSetFwEntry(instance=0xffffffd7b2390000, fw_id=0) returned 1
  [4848354681] _IpiExecuteSwipEntryPoint returned 1 (client_id=8, event_id=0)
  [4848366022] _IpiExecuteSwipEntryPoint entered: instance=0xffffffd7b2390000, client_id=7, event_id=0, context=0
  [4848369198] _ipi_gc_sw_init entered: dev=0xffffffd7b2417000, fn_ptr@+0x8=0xffffff7fa5b90f74
  [4848371953] _IpiGetDiscoveryInfo(instance=0xffffffd7b2390000, client_id=7) returned 0xffffffd7b2b813f8
  [4848374869] _IpiGetDiscoveryInfo(client_id=7) -> 0xffffffd7b2b813f8 (inst=1, span=0x210 vs 0x235, major=10, minor=3, rev=3)
  [4848379157] _gc_sw_init entered: p1=0xffffffaa6ffaf370 (sz=0x238, +C=0xa, +10=0x3, +14=0x3), p2=0xffffffaa6ffaef48 (sz=0x428)
  [4848392933] _gc_get_hw_version(slice=0xffffffaa6ffaf37c) returned 0x000A0303
  [4848396589] _gc_get_config entered: gc_buf=0xffffffd7b2535008, hw_version=0x000A0303
  [4848407229] _gc_get_config exited
  [4848410495] _gc_get_hw_config_tbl_info entered: gc_buf=0xffffffd7b2535008, hw_version=0x000A0303
  [4848412609] _gc_get_hw_config_tbl_info exited: table_size@+0xd38=0x64
  [4848419793] _gc_set_fw_entry_info entered: p1=0xffffffd7b2535008, p2=0x30000000a, rev=3, p4=0xffffffaa6ffaef48
  [4848421967] _gc_get_hw_version(slice=0xffffffaa6ffadef8) returned 0x000A0303
  [4848423810] _gc_set_fw_entry_info returned 2
  [4848425253] _gc_sw_init returned 2
  [4848426936] _ipi_gc_sw_init returned 0
  [4848429201] _IpiExecuteSwipEntryPoint returned 0 (client_id=7, event_id=0)
  [4848430763] HWLibs: AMD Error:
  [4848432376] HWLibs: [54:0:0] TTL Event source_id=2, event_id=0x900c0401, event_info:type= 3,hw_id=0 : swip_client_id= 7, swip_event_id=0 : (SW_IP_CLIENT_ID__GC,EVENT__SW_INIT)
  [4848441213] HWLibs: AMD TTL COS:
  [4848441624] HWLibs: ----------------------------------------------------------------
  [4848442746] HWLibs: AMD TTL COS: ASSERT FUNCTION: logSwipFailure
  [4848443958] HWLibs: AMD TTL COS: ASSERT FILE:     ../../../src/tls/ttl_tls.c
  [4848445160] HWLibs: AMD TTL COS: ASSERT LINE:     507
  [4848446343] HWLibs: AMD TTL COS: ASSERT REASON:   GC init/power-up failed
  [4848447835] HWLibs: AMD TTL COS: ----------------------------------------------------------------
  [4848451633] HWLibs: AMD TTL COS:
  [4848451973] HWLibs: ----------------------------------------------------------------
  [4848453045] HWLibs: AMD TTL COS: ASSERT FUNCTION: TlsExecuteIpEntrySeq
  [4848455059] HWLibs: AMD TTL COS: ASSERT FILE:     ../../../src/tls/ttl_tls.c
  [4848456692] HWLibs: AMD TTL COS: ASSERT LINE:     357
  [4848457754] HWLibs: AMD TTL COS: ASSERT REASON:   SWIP init/power-up failed
  [4848459077] HWLibs: AMD TTL COS: ----------------------------------------------------------------
  [4848471029] HWLibs: AMD TTL COS:
  [4848471410] HWLibs: ----------------------------------------------------------------
  [4848473163] HWLibs: AMD TTL COS: ASSERT FUNCTION: TlsSwInit
  [4848474315] HWLibs: AMD TTL COS: ASSERT FILE:     ../../../src/tls/ttl_tls.c
  [4848475487] HWLibs: AMD TTL COS: ASSERT LINE:     129
  [4848476519] HWLibs: AMD TTL COS: ASSERT REASON:   Failed to create TLS
  [4848477842] HWLibs: AMD TTL COS: ----------------------------------------------------------------
  [4848482420] _IpiExecuteSwipEntryPoint entered: instance=0xffffffd7b2390000, client_id=8, event_id=2, context=0
  [4848484023] slot_internal_sw_exit entered
  [4848566538] _IpiExecuteSwipEntryPoint returned 1 (client_id=8, event_id=2)
  [4848571527] _IpiExecuteSwipEntryPoint entered: instance=0xffffffd7b2390000, client_id=6, event_id=2, context=0
  [4848579132] _IpiExecuteSwipEntryPoint returned 1 (client_id=6, event_id=2)
  [4848583139] _IpiExecuteSwipEntryPoint entered: instance=0xffffffd7b2390000, client_id=5, event_id=2, context=0
  [4848639184] _IpiExecuteSwipEntryPoint returned 1 (client_id=5, event_id=2)
  [4848643111] _IpiExecuteSwipEntryPoint entered: instance=0xffffffd7b2390000, client_id=3, event_id=2, context=0
  [4848813270] _IpiExecuteSwipEntryPoint returned 1 (client_id=3, event_id=2)
  [4848897218] _ttl_initialize returned status = 0x00000004
  [4848898240] HWLibs: [54:0:0] AMD Error:
  [4848899342] HWLibs: ttl_initialize failed status = 0x00000004
  [4848902107] AmdTtlServices::initialize() returned 0xE00002BC (this=0xffffff95a5356000)
  [4848977258] AmdTtlServices::initialize(this=0xffffff95a5356000, input) returned 0xE00002BC
  [4848979813] X6000: [54:0:0] [Accel] [CRITICAL] <<< TTL::initialize() Failed! Error code: 0xe00002bc
  [4849191840] X6000: [54:0:0] [Accel] >>> Begin HW diagnostic logging...
  [4849252965] (x4) SpecialAMDKey request 0x14 (wait: 1)
  [4849310052] HWLibs: [54:0:0] AMD Error:
  [4849311284] HWLibs: NULL TTL Handle.
  [4849313899] HWLibs: [54:0:0] AMD Error:
  [4849316884] HWLibs: FW Attestation Info not supported
  [4849320631] SpecialAMDKey request 0x14 (wait: 1)
  [4851619801] X6000: -- [54:0:0] PSP Information --
  [4851620312] X6000: tOS Status             = 0x00115136
  [4851620602] X6000: tOS Version            = 0x00280066 (0.28.0.66)
  [4851620883] X6000: BootLoader Version     = 0x00280066 (0.28.0.66)
  [4851621133] X6000: Sec. Policy Version    = 0x0d18006e (13.24.0.110)
  [4851621344] X6000: BootLoader Ready       = 0xffffffff
  [4851621554] X6000: BootLoader POST Code   = 0xffffffff
  [4851621785] X6000: FW Validation Info     = 0xffffffff
  [4851622215] X6000: ----------------------------------------------
  [4851622426] X6000: Write Ptr              = 0x00000000
  [4851622626] X6000: Read Ptr               = 0x00000000
  [4851622857] X6000: ----------------------------------------------
  [4851623067] X6000: tOS: Program Counter   = 0x00000000
  [4851623448] X6000: tOS: Thread Info       = 0x00000000 (threadID 0x0, CPU mode 0x0, ProcessID 0x0, ProcessType 0x0, AbortType 0x0)
  [4851623678] X6000: tOS: Exception Address = 0x00000000
  [4851623879] X6000: tOS: DFSR              = 0x00000000
  [4851624089] X6000: tOS: DFAR              = 0x00000000
  [4851624339] X6000: tOS: Panic code        = 0x00000000
  [4851624560] X6000: tOS: TA UUID           = 0x00000000
  [4851624780] X6000: ---RSMU Timeout Event Log---
  [4851625021] X6000: RSMU Timeout Events Recorded: 2 (0x00000002)
  [4851625702] X6000: Event[000] ID=0x00000006(CLKA0) SlaveError=0x0005b388{Addr=0x5b388 ApertureID=0x0 Initiator=0x00=UNKNOWN ErrorOperation=R SecurityError=N} AwData=0xc0e06380 (Violation: IP_Reg_Access=N, VCPU_Mem_Access=N, WAFLError=N, Security=N)
  [4851626413] X6000: Event[001] ID=0x01000030(UNKNOWN) SlaveError=0x02d01ef0{Addr=0x01ef0 ApertureID=0x1 Initiator=0x0b=GC ErrorOperation=R SecurityError=N} AwData=0xc0011b80 (Violation: IP_Reg_Access=N, VCPU_Mem_Access=N, WAFLError=N, Security=N)
  [4851626674] X6000: ---SWIP Error Log---
  [4851627255] X6000: [SW_IP_CLIENT_ID__TTL]: {4848433919:[54:0:0] TTL Event source_id=2, event_id=0x900c0401, event_info:type= 3,hw_id=0 : swip_client_id= 7, swip_event_id=0 : (SW_IP_CLIENT_ID__GC,EVENT__SW_INIT)}
  [4851630962] X6000: [54:0:0] [Accel] <<< End HW diagnostic logging.
  [4851735708] HWLibs: [54:0:0] AMD Error:
  [4851737181] HWLibs: m_hTtl is NULL!
  [4851738053] HWLibs: [54:0:0] AMD Error:
  [4851739646] HWLibs: Failed hdcp_services terminate: result 0xe00002c2
  [4851740678] HWLibs: [54:0:0] AMD Error:
  [4851741589] HWLibs: TTL function pointer is NULL!
  [4851742391] HWLibs: AMD Error:
  [4851743393] HWLibs: queryEngineQueueCount() for AMD_SWIP_QUEUE__VIDEO_DECODE failed!
  [4851999823] X6000: ----------------------------------------
  [4852000194] X6000: --- DRIVER STATE TRACE---
  [4852000495] X6000: DeviceID                                : [54:0:0]
  [4852000765] X6000: ----------------------
  [4852001126] X6000: u64AllAccelStart                        : 0x00000000000020c5
  [4852001336] X6000: ----------------------
  [4852001587] X6000: accelStart_entry                        : 1
  [4852001837] X6000: accelStarted                            : 0
  [4852002098] X6000: accelStart_exit                         : 1
  [4852002328] X6000: configureDevice_entry                   : 0
  [4852002558] X6000: deviceConfigured                        : 0
  [4852002789] X6000: configureDevice_exit                    : 0
  [4852003019] X6000: initLinkToPeer                          : 1
  [4852003240] X6000: createHWHandler                         : 1
  [4852003470] X6000: createHWInterface                       : 0
  [4852003731] X6000: hardwareInit_entry                      : 0
  [4852003961] X6000: hardwareInit                            : 0
  [4852004211] X6000: hardwareInit_exit                       : 0
  [4852004452] X6000: setupInitHWCapabilities                 : 0
  [4852004682] X6000: initExternalInterfaces                  : 1
  [4852004903] X6000: hwRegistersInitialized                  : 0
  [4852005133] X6000: mapDoorbellMemory                       : 0
  [4852005364] X6000: initializeTtl                           : 0
  [4852005584] X6000: hwMemoryInitialized                     : 0
  [4852005865] X6000: hwGartInitialized                       : 0
  [4852006115] X6000: hwVmmInitialized                        : 0
  [4852006356] X6000: allocateHWEngines                       : 0
  [4852006586] X6000: initializeHWEngines                     : 0
  [4852006816] X6000: initHWDisplay                           : 0
  [4852007047] X6000: initHWInfo                              : 0
  [4852007267] X6000: ----------------------
  [4852007568] X6000: u64AllPowerUp                           : 0x0000000000000000
  [4852007778] X6000: ----------------------
  [4852008009] X6000: ttlPowerUp                              : 0
  [4852008239] X6000: accelPowerUpHW_entry                    : 0
  [4852008459] X6000: accelPowerUpHW                          : 0
  [4852008720] X6000: accelPowerUpHW_exit                     : 0
  [4852008960] X6000: setVirtualSpace                         : 0
  [4852009271] X6000: hardwarePowerUp_entry                   : 0
  [4852009501] X6000: hardwarePowerUp                         : 0
  [4852009752] X6000: hardwarePowerUp_exit                    : 0
  [4852010002] X6000: powerUpHWEngines                        : 0
  [4852010233] X6000: startHWEngines                          : 0
  [4852010453] X6000: bltMgrInitialized                       : 0
  [4852010684] X6000: atomicBltMgrInitialized                 : 0
  [4852010914] X6000: ----------------------------------------
RBGPUDiag:
  alloc_ret = 0x0
  cap_tbl_idx = 0x0
  cap_tbl_patched = 0x1
  carveout_ok = 0x0
  ctx44_0 = 0x16000
  dev_type_idx = 0x42
  dev_type_patched = 0x1
  dev_type_rb_id = 0x1681
  dev_type_rb_type = 0x6
  fb_mc_addr = 0xffffffd7b2534000
  gc_disc_inst = 0x1
  gc_disc_major = 0xa
  gc_disc_minor = 0x3
  gc_disc_rev = 0x3
  gc_disc_span = 0x210
  gc_get_cfg_calls = 0x1
  gc_get_ver_calls = 0x2
  gc_hw_version = 0xa0303
  gc_set_fw_calls = 0x1
  gc_set_fw_info_calls = 0x1
  gc_set_fw_info_ret = 0x2
  gc_set_fw_ret = 0x1
  gc_sw_init_calls = 0x1
  gc_sw_init_ret = 0x0
  gc_sw_sub_calls = 0x1
  gc_sw_sub_ret = 0x2
  gc_tbl_info_calls = 0x1
  gc_tbl_info_sz = 0x64
  guard_calls = 0x0
  guard_routed = 0x0
  ipi_disc_calls = 0xe
  ipi_exec_calls = 0x9
  ipi_exec_ret = 0x1
  mp1_hw_index = 0x10
  psp_alloc_calls = 0x1
  psp_alloc_ret = 0x0
  psp_asic_init_calls = 0x1
  psp_asic_ret = 0x0
  psp_asic_type = 0x13
  psp_pfn_calls = 0x1
  psp_pfn_ret = 0x0
  psp_read_cfg_calls = 0x1
  psp_sw_init_calls = 0x1
  psp_sw_init_ret = 0x200001
  psp_sw_init_sub_calls = 0x1
  psp_sw_init_sub_ret = 0x0
  req_0x14 = 0x7
  smu_hook_calls = 0x1
  smu_hook_routed = 0x1
  sw_init_ret = 0x0
  ttl_init_calls = 0x1
  ttl_init_inp_calls = 0x1
  ttl_init_inp_ret = 0xe00002bc
  ttl_init_inp_this = 0xffffff95a5356000
  ttl_init_ret = 0xe00002bc
  ttl_init_this = 0xffffff95a5356000
  ttl_inp_0 = 0xfffffffffc11e690
  ttl_inp_1 = 0xfffffffffc11e6a8
  ttl_inp_2 = 0x0
  ttl_inp_4 = 0x40000000
  ttl_inp_5 = 0xffffffdba48f6000
  ttl_inp_6 = 0x200000
  ttl_is_supp_calls = 0x1
  ttl_is_supp_ret = 0x1
  ttl_param_q0 = 0xffffff95a5356050
  ttl_param_q1 = 0xffffff95a53563f0
  ttl_param_q2 = 0xffffff95a5356528
  ttl_param_q3 = 0xffffff95a5356540
  ttl_param_q4 = 0x3600001681
  ttl_param_q5 = 0x0
  ttl_raw_init_arg1 = 0xffffffaa6ffaf818
  ttl_raw_init_arg2 = 0xffffff95a5356038
  ttl_raw_init_calls = 0x1
  ttl_raw_init_ret = 0x4
  ttl_set_fw_dir_calls = 0x1
  ttl_set_fw_dir_post_570 = 0x0
  ttl_set_fw_dir_post_958 = 0xffffff95a3820100
  ttl_set_fw_dir_ptr = 0xffffff95a3820100
  ttl_set_fw_dir_this = 0xffffff95a5356000
  ttl_sync_570_applied = 0x1
  ttl_this_570 = 0x0
  window_limit = 0x80000
```


===== FILE: src/plugin/Info.plist =====

```xml
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleDevelopmentRegion</key>
    <string>en</string>
    <key>CFBundleExecutable</key>
    <string>$(EXECUTABLE_NAME)</string>
    <key>CFBundleIdentifier</key>
    <string>$(PRODUCT_BUNDLE_IDENTIFIER)</string>
    <key>CFBundleInfoDictionaryVersion</key>
    <string>6.0</string>
    <key>CFBundleName</key>
    <string>$(PRODUCT_NAME)</string>
    <key>CFBundlePackageType</key>
    <string>KEXT</string>
    <key>CFBundleShortVersionString</key>
    <string>$(MODULE_VERSION)</string>
    <key>CFBundleSignature</key>
    <string>????</string>
    <key>CFBundleVersion</key>
    <string>$(MODULE_VERSION)</string>
    <key>IOKitPersonalities</key>
    <dict>
        <key>$(PRODUCT_NAME)</key>
        <dict>
            <key>CFBundleIdentifier</key>
            <string>$(PRODUCT_BUNDLE_IDENTIFIER)</string>
            <key>IOClass</key>
            <string>$(PRODUCT_NAME)</string>
            <key>IOMatchCategory</key>
            <string>$(PRODUCT_NAME)</string>
            <key>IOProviderClass</key>
            <string>IOResources</string>
            <key>IOResourceMatch</key>
            <string>IOKit</string>
        </dict>
        <key>RembrandtGPUPeer</key>
        <dict>
            <key>CFBundleIdentifier</key>
            <string>$(PRODUCT_BUNDLE_IDENTIFIER)</string>
            <key>IOClass</key>
            <string>RembrandtGPUPeer</string>
            <key>IOMatchCategory</key>
            <string>ATIFramebuffer</string>
            <key>IOPCIMatch</key>
            <string>0x16811002</string>
            <key>IOProbeScore</key>
            <integer>65000</integer>
            <key>IOProviderClass</key>
            <string>IOPCIDevice</string>
        </dict>
        <key>RembrandtGPU HWServices (0x1681)</key>
        <dict>
            <key>CFBundleIdentifier</key>
            <string>com.apple.kext.AMDRadeonX6000HWServices</string>
            <key>IOClass</key>
            <string>AMDRadeonX6000_AMDRadeonHWServicesNavi</string>
            <key>IOMatchCategory</key>
            <string>AMDRadeonX6000HWServices</string>
            <key>IOPCIMatch</key>
            <string>0x16811002</string>
            <key>IOPropertyMatch</key>
            <dict>
                <key>LoadHWServices</key>
                <true/>
                <key>RBGPULoad</key>
                <true/>
            </dict>
            <key>IOProviderClass</key>
            <string>IOPCIDevice</string>
        </dict>
        <key>RembrandtGPU Accelerator (0x1681)</key>
        <dict>
            <key>CFBundleIdentifier</key>
            <string>com.apple.kext.AMDRadeonX6000</string>
            <key>IOClass</key>
            <string>AMDRadeonX6000_AMDNavi21GraphicsAccelerator</string>
            <key>IOMatchCategory</key>
            <string>IOAccelerator</string>
            <key>IOPCIMatch</key>
            <string>0x16811002</string>
            <key>IOPropertyMatch</key>
            <dict>
                <key>LoadAccelerator</key>
                <true/>
                <key>RBGPULoad</key>
                <true/>
            </dict>
            <key>IOProviderClass</key>
            <string>IOPCIDevice</string>
        </dict>
    </dict>
    <key>OSBundleLibraries</key>
    <dict>
        <key>as.vit9696.Lilu</key>
        <string>1.4.7</string>
        <key>com.apple.iokit.IOPCIFamily</key>
        <string>2.9</string>
        <key>com.apple.kpi.bsd</key>
        <string>12.0.0</string>
        <key>com.apple.kpi.iokit</key>
        <string>12.0.0</string>
        <key>com.apple.kpi.libkern</key>
        <string>12.0.0</string>
        <key>com.apple.kpi.mach</key>
        <string>12.0.0</string>
        <key>com.apple.kpi.unsupported</key>
        <string>12.0.0</string>
    </dict>
    <key>OSBundleRequired</key>
    <string>Root</string>
</dict>
</plist>
```


===== FILE: src/plugin/kern_start.hpp =====

```cpp
#ifndef KERN_START_HPP
#define KERN_START_HPP

#include <Headers/kern_api.hpp>
#include <Headers/kern_patcher.hpp>

#include "RembrandtGPUPeer.hpp"

#define MODULE_SHORT "rbgpu"

class RGPU {
public:
    void init();
    void deinit();

    void processKext(KernelPatcher &patcher, size_t id, mach_vm_address_t address, size_t size);

private:
    bool noSmuHook{false};
    bool noTrap3Guard{false};
    bool noLogCapture{false};
    bool safeTtlMode{false};
    bool sync570Mode{false};
    bool spoofTtlMode{false};
    bool pspHwMode{false};

    void installKernelLogHooks(KernelPatcher &patcher);
    void processHWLibs(KernelPatcher &patcher, size_t id, mach_vm_address_t address, size_t size);
    void processX6000(KernelPatcher &patcher, size_t id, mach_vm_address_t address, size_t size);
    void processHWServices(KernelPatcher &patcher, size_t id, mach_vm_address_t address, size_t size);

    void routeTTL(KernelPatcher &patcher, size_t id, mach_vm_address_t address, size_t size);
    void routePSPBasics(KernelPatcher &patcher, size_t id, mach_vm_address_t address, size_t size);
    void routePSPRingTmr(KernelPatcher &patcher, size_t id, mach_vm_address_t address, size_t size);
    void routeGC(KernelPatcher &patcher, size_t id, mach_vm_address_t address, size_t size);

    bool patchDeviceTypeTable(KernelPatcher &patcher, size_t id);
    bool patchDeviceCapabilityTable(KernelPatcher &patcher, size_t id);
};

#endif // KERN_START_HPP

```


===== FILE: src/plugin/kern_start.cpp =====

```cpp
#include <Headers/plugin_start.hpp>
#include <Headers/kern_api.hpp>
#include <Headers/kern_util.hpp>
#include <Headers/kern_patcher.hpp>
#include <IOKit/IOLib.h>

#include "kern_start.hpp"
#include "rb_smu13.h"

#ifndef RB_HWID_MP1
#define RB_HWID_MP1 1
#endif

static RGPU rgpu;
uint64_t g_guard_calls = 0;

static const char *bootargOff[]   = { "-rbgpuoff" };
static const char *bootargDebug[] = { "-rbgpudbg" };
static const char *bootargBeta[]  = { "-rbgpubeta" };

static void pluginStart() {
    rgpu.init();
}

PluginConfiguration ADDPR(config) = {
    xStringify(PRODUCT_NAME),
    parseModuleVersion(xStringify(MODULE_VERSION)),
    LiluAPI::AllowNormal | LiluAPI::AllowInstallerRecovery,
    bootargOff,
    arrsize(bootargOff),
    bootargDebug,
    arrsize(bootargDebug),
    bootargBeta,
    arrsize(bootargBeta),
    KernelVersion::Monterey,
    KernelVersion::Sequoia,
    pluginStart
};

static const char *kextPathHWLibs = "/System/Library/Extensions/AMDRadeonX6000HWServices.kext/Contents/PlugIns/AMDRadeonX6800HWLibs.kext/Contents/MacOS/AMDRadeonX6800HWLibs";
static const char *kextPathX6000  = "/System/Library/Extensions/AMDRadeonX6000.kext/Contents/MacOS/AMDRadeonX6000";
static const char *kextPathHWSvc  = "/System/Library/Extensions/AMDRadeonX6000HWServices.kext/Contents/MacOS/AMDRadeonX6000HWServices";

enum KextListIndex {
    KextHWLibs = 0,
    KextX6000,
    KextHWSvc
};

static KernelPatcher::KextInfo kextList[] = {
    { "com.apple.kext.AMDRadeonX6800HWLibs",     &kextPathHWLibs, 1, {true}, {}, KernelPatcher::KextInfo::Unloaded },
    { "com.apple.kext.AMDRadeonX6000",            &kextPathX6000,  1, {true}, {}, KernelPatcher::KextInfo::Unloaded },
    { "com.apple.kext.AMDRadeonX6000HWServices", &kextPathHWSvc,  1, {true}, {}, KernelPatcher::KextInfo::Unloaded }
};

static mach_vm_address_t g_kext_addr_hwlibs = 0;
static size_t            g_kext_size_hwlibs = 0;

static mach_vm_address_t g_kext_addr_x6000  = 0;
static size_t            g_kext_size_x6000  = 0;

static mach_vm_address_t g_kext_addr_hwsvc  = 0;
static size_t            g_kext_size_hwsvc  = 0;

static struct rb_smu13 g_smu;
static struct rb_hw    g_hw;
static struct rb_disc *g_disc       = nullptr;
static void           *g_smu_ctx    = nullptr;
static void           *g_dma_handle = nullptr;
static uint64_t        g_dma_gpu    = 0;
static void           *g_dma_cpu    = nullptr;

typedef int      (*smu_cgs_alloc_memory_t)(void *ctx, uint32_t size, uint32_t align, uint32_t domain, void **handle, uint64_t *gpu_addr, void **cpu_addr);
typedef int      (*smu_cgs_free_memory_t)(void *ctx, void *handle);
typedef uint32_t (*smu_get_hw_version_t)(uint64_t ver, uint64_t rev);
typedef int      (*smu_read_config_space_t)(void *ctx);
typedef int      (*smu_init_function_pointer_list_t)(void *ctx, uint64_t ver, uint64_t rev);

typedef int      (*amdtll_init_input_t)(void *this_ptr, void *input);
typedef int      (*amdtll_initialize_t)(void *this_ptr);
typedef void     (*amdtll_set_fw_dir_t)(void *this_ptr, void *fw_dir);
typedef uint32_t (*ttl_initialize_fn_t)(void *init_params, void *ttl_context_38);
typedef uint8_t  (*ttl_is_supported_device_fn_t)(void *dev_info_20);
typedef int      (*ttl_hw_init_fn_t)(void *ctx);
typedef uint8_t  (*ipi_execute_swip_entry_point_fn_t)(void *instance, uint32_t swip_client_id, uint32_t event_id, void *context);
typedef void*    (*ipi_get_discovery_info_fn_t)(void *instance, uint32_t client_id);
typedef int      (*ipi_psp_sw_init_fn_t)(void *dev);
typedef int      (*ipi_psp_hw_init_fn_t)(void *dev);
typedef int      (*ipi_gc_sw_init_fn_t)(void *dev);
typedef int      (*ipi_gc_hw_init_fn_t)(void *dev);

typedef int      (*gc_sw_init_fn_t)(void *param1, void *param2);
typedef int      (*gc_hw_init_fn_t)(void *gc_buf, void *arg2, void *arg3);
typedef uint32_t (*gc_get_hw_version_fn_t)(void *version_slice);
typedef void     (*gc_get_config_fn_t)(void *gc_buf, uint32_t hw_version);
typedef void     (*gc_get_hw_config_tbl_info_fn_t)(void *gc_buf, uint32_t hw_version);
typedef int      (*gc_set_fw_entry_info_fn_t)(void *p1, void *p2, uint32_t rev, void *p4);
typedef int      (*gc_init_fcn_ptr_list_fn_t)(void *gc_buf, uint32_t hw_version);
typedef int      (*gc_get_doorbell_requirements_fn_t)(void *gc_buf, void *out_reqs);
typedef uint8_t  (*ttl_dev_get_doorbell_range_fn_t)(void *dev, uint32_t type, uint32_t idx, void *out_info);
typedef uint8_t  (*ipi_set_fw_entry_fn_t)(void *instance, void *fw_entry);

typedef int      (*psp_sw_init_fn_t)(void *param1, void *param2);
typedef int      (*psp_hw_init_fn_t)(void *psp_buf, void *arg2, void *arg3);
typedef int      (*psp_cos_alloc_memory_fn_t)(void *ctx, uint32_t size, uint32_t align, void **out_ptr);
typedef int      (*psp_asic_type_init_fn_t)(void *psp_buf, void *disc_entry);
typedef int      (*psp_init_pfn_ptr_fn_t)(void *psp_buf);
typedef int      (*psp_read_config_fn_t)(void *psp_buf);
typedef int      (*psp_allocate_private_mem_fn_t)(void *psp_buf);
typedef int      (*psp_ring_init_fn_t)(void *psp_buf, uint32_t ring_type);
typedef int      (*psp_ring_create_fn_t)(void *psp_buf, uint32_t ring_type);
typedef int      (*psp_bootloader_is_sos_running_fn_t)(void *psp_buf);
typedef int      (*psp_cmd_km_init_fn_t)(void *psp_buf);
typedef int      (*psp_tmr_init_fn_t)(void *psp_buf);
typedef int      (*psp_tmr_load_fn_t)(void *psp_buf);
typedef int      (*psp_dtm_init_fn_t)(void *psp_buf);
typedef int      (*psp_np_fw_load_fn_t)(void *psp_buf);

typedef void     (*kprintf_t)(const char *fmt, ...);
typedef void     (*iolog_t)(const char *fmt, ...);

static smu_cgs_alloc_memory_t           orig_smu_cgs_alloc_memory           = nullptr;
static smu_cgs_free_memory_t            orig_smu_cgs_free_memory            = nullptr;
static smu_get_hw_version_t             orig_smu_get_hw_version             = nullptr;
static smu_read_config_space_t          orig_smu_read_config_space          = nullptr;
static smu_init_function_pointer_list_t orig_smu_init_function_pointer_list = nullptr;

static amdtll_init_input_t              orig_amdtll_init_input              = nullptr;
static amdtll_initialize_t              orig_amdtll_initialize              = nullptr;
static amdtll_set_fw_dir_t              orig_amdtll_set_fw_dir              = nullptr;
static ttl_initialize_fn_t              orig_ttl_initialize                 = nullptr;
static ttl_is_supported_device_fn_t     orig_ttl_is_supported_device        = nullptr;
static ttl_hw_init_fn_t                 orig_ttl_hw_init                    = nullptr;
static ipi_execute_swip_entry_point_fn_t orig_ipi_execute_swip_entry_point = nullptr;
static ipi_get_discovery_info_fn_t      orig_ipi_get_discovery_info         = nullptr;
static ipi_psp_sw_init_fn_t             orig_ipi_psp_sw_init                = nullptr;
static ipi_psp_hw_init_fn_t             orig_ipi_psp_hw_init                = nullptr;
static ipi_gc_sw_init_fn_t              orig_ipi_gc_sw_init                 = nullptr;
static ipi_gc_hw_init_fn_t              orig_ipi_gc_hw_init                 = nullptr;

static gc_sw_init_fn_t                  orig_gc_sw_init                     = nullptr;
static gc_hw_init_fn_t                  orig_gc_hw_init                     = nullptr;
static gc_get_hw_version_fn_t           orig_gc_get_hw_version              = nullptr;
static gc_get_config_fn_t               orig_gc_get_config                  = nullptr;
static gc_get_hw_config_tbl_info_fn_t   orig_gc_get_hw_config_tbl_info      = nullptr;
static gc_set_fw_entry_info_fn_t        orig_gc_set_fw_entry_info           = nullptr;
static gc_init_fcn_ptr_list_fn_t        orig_gc_init_fcn_ptr_list           = nullptr;
static gc_get_doorbell_requirements_fn_t orig_gc_get_doorbell_requirements  = nullptr;
static ttl_dev_get_doorbell_range_fn_t  orig_ttl_dev_get_doorbell_range     = nullptr;
static ipi_set_fw_entry_fn_t            orig_ipi_set_fw_entry               = nullptr;

static psp_sw_init_fn_t                 orig_psp_sw_init                    = nullptr;
static psp_hw_init_fn_t                 orig_psp_hw_init                    = nullptr;
static psp_cos_alloc_memory_fn_t        orig_psp_cos_alloc_memory           = nullptr;
static psp_asic_type_init_fn_t          orig_psp_asic_type_init             = nullptr;
static psp_init_pfn_ptr_fn_t            orig_psp_init_pfn_ptr               = nullptr;
static psp_read_config_fn_t             orig_psp_read_config                = nullptr;
static psp_allocate_private_mem_fn_t    orig_psp_allocate_private_mem       = nullptr;
static psp_ring_init_fn_t               orig_psp_ring_init                  = nullptr;
static psp_ring_create_fn_t             orig_psp_ring_create                = nullptr;
static psp_bootloader_is_sos_running_fn_t orig_psp_bootloader_is_sos_running = nullptr;
static psp_cmd_km_init_fn_t             orig_psp_cmd_km_init                = nullptr;
static psp_tmr_init_fn_t                orig_psp_tmr_init                   = nullptr;
static psp_tmr_load_fn_t                orig_psp_tmr_load                   = nullptr;
static psp_dtm_init_fn_t                orig_psp_dtm_init                   = nullptr;
static psp_np_fw_load_fn_t              orig_psp_np_fw_load                 = nullptr;

static kprintf_t orig_kprintf = nullptr;
static iolog_t   orig_iolog   = nullptr;

static bool g_safe_ttl_mode  = false;
static bool g_sync_570_mode  = false;
static bool g_spoof_ttl_mode = false;
static bool g_psp_hw_mode    = false;

struct DeviceTypeEntry {
    uint32_t deviceId;
    uint32_t deviceType;
};

struct CapabilityEntry {
    uint8_t data[80];
};

static inline const char *getCallerTag(mach_vm_address_t ret_addr) {
    if (g_kext_addr_hwlibs && ret_addr >= g_kext_addr_hwlibs && ret_addr < g_kext_addr_hwlibs + g_kext_size_hwlibs) {
        return "HWLibs:";
    }
    if (g_kext_addr_x6000 && ret_addr >= g_kext_addr_x6000 && ret_addr < g_kext_addr_x6000 + g_kext_size_x6000) {
        return "X6000:";
    }
    if (g_kext_addr_hwsvc && ret_addr >= g_kext_addr_hwsvc && ret_addr < g_kext_addr_hwsvc + g_kext_size_hwsvc) {
        return "HWSvc:";
    }
    return nullptr;
}

static void wrap_kprintf(const char *fmt, ...) {
    void *ret_addr = __builtin_return_address(0);
    const char *tag = getCallerTag(reinterpret_cast<mach_vm_address_t>(ret_addr));
    va_list va;
    va_start(va, fmt);
    if (tag) {
        rbgpu_log_captured_va(tag, fmt, va, orig_kprintf);
    } else if (orig_kprintf) {
        char buf[256];
        vsnprintf(buf, sizeof(buf), fmt, va);
        orig_kprintf("%s", buf);
    }
    va_end(va);
}

static void wrap_iolog(const char *fmt, ...) {
    void *ret_addr = __builtin_return_address(0);
    const char *tag = getCallerTag(reinterpret_cast<mach_vm_address_t>(ret_addr));
    va_list va;
    va_start(va, fmt);
    if (tag) {
        rbgpu_log_captured_va(tag, fmt, va, orig_iolog);
    } else if (orig_iolog) {
        char buf[256];
        vsnprintf(buf, sizeof(buf), fmt, va);
        orig_iolog("%s", buf);
    }
    va_end(va);
}

static int smu13ErrToApple(int err) {
    switch (err) {
        case RB_SMU13_OK:             return 0;
        case RB_SMU13_E_INVAL:        return 1;
        case RB_SMU13_E_FW:
        case RB_SMU13_E_VERSION:
        case RB_SMU13_E_NODATA:       return 2;
        case RB_SMU13_E_BUSY:
        case RB_SMU13_E_TIMEOUT:
        case RB_SMU13_E_DEAD:         return 3;
        case RB_SMU13_E_NOT_ALLOWED:  return 4;
        default:                      return 2;
    }
}

static uint32_t smu13HwRreg(void *ctx, uint32_t byte_off) {
    if (!ctx) return 0;
    void *cgs = *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(ctx) + 0x08);
    if (!cgs) return 0;
    void *handle = *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(cgs) + 0x08);
    typedef uint32_t (*cgs_rreg_t)(void *h, uint32_t dword_idx, uint32_t flags);
    auto rreg_fn = *reinterpret_cast<cgs_rreg_t *>(reinterpret_cast<uintptr_t>(cgs) + 0x40);
    if (!rreg_fn) return 0;
    return rreg_fn(handle, byte_off / 4, 0);
}

static void smu13HwWreg(void *ctx, uint32_t byte_off, uint32_t v) {
    if (!ctx) return;
    void *cgs = *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(ctx) + 0x08);
    if (!cgs) return;
    void *handle = *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(cgs) + 0x08);
    typedef void (*cgs_wreg_t)(void *h, uint32_t dword_idx, uint32_t val, uint32_t flags);
    auto wreg_fn = *reinterpret_cast<cgs_wreg_t *>(reinterpret_cast<uintptr_t>(cgs) + 0x38);
    if (!wreg_fn) return;
    wreg_fn(handle, byte_off / 4, v, 0);
}

static void smu13HwDelayUs(void *ctx, uint32_t us) {
    if (ctx) {
        void *os_svc = *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(ctx) + 0x10);
        if (os_svc) {
            void *os_handle = *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(os_svc) + 0x08);
            typedef void (*cos_delay_t)(void *h, uint32_t us);
            auto delay_fn = *reinterpret_cast<cos_delay_t *>(reinterpret_cast<uintptr_t>(os_svc) + 0x80);
            if (delay_fn) {
                delay_fn(os_handle, us);
                return;
            }
        }
    }
    IODelay(us);
}

static int slot_internal_sw_init(void *ctx, ...) {
    rbgpu_log("slot_internal_sw_init entered (ctx=%p)", ctx);
    g_smu_ctx = ctx;

    uint32_t mp1_base     = *reinterpret_cast<uint32_t *>(reinterpret_cast<uintptr_t>(ctx) + 0x44);
    uint32_t window_limit = *reinterpret_cast<uint32_t *>(reinterpret_cast<uintptr_t>(ctx) + 0x27C);

    rbgpu_diag_set_num("ctx44_0", mp1_base);
    rbgpu_diag_set_num("window_limit", window_limit);
    rbgpu_log("[Check 1] ctx+0x44[0]=0x%08X (expected 0x16000), window_limit=0x%08X", mp1_base, window_limit);

    if (!orig_smu_cgs_alloc_memory) {
        rbgpu_log("smu_cgs_alloc_memory symbol unresolved");
        rbgpu_diag_set_num("sw_init_ret", 2);
        return 2;
    }

    int alloc_ret = orig_smu_cgs_alloc_memory(ctx, 0x1000, 0x1000, 1, &g_dma_handle, &g_dma_gpu, &g_dma_cpu);
    rbgpu_diag_set_num("alloc_ret", static_cast<uint64_t>(alloc_ret));
    rbgpu_diag_set_num("fb_mc_addr", g_dma_gpu);
    rbgpu_log("[Check 2] smu_cgs_alloc_memory ret=%d, handle=%p, gpu=0x%llx, cpu=%p (gpu is CGS IOVirtualAddress)",
              alloc_ret, g_dma_handle, g_dma_gpu, g_dma_cpu);

    if (alloc_ret != 0 || !g_dma_cpu) {
        rbgpu_log("Driver table memory allocation failed");
        rbgpu_diag_set_num("sw_init_ret", 2);
        return 2;
    }

    bool in_carveout = (g_dma_gpu >= 0xF400000000ULL && g_dma_gpu <= 0xF4FFFFFFFFULL);
    rbgpu_diag_set_num("carveout_ok", in_carveout ? 1 : 0);
    rbgpu_log("[Check 2] Carveout verification (0xF4_0000_0000..0xF4_FFFF_FFFF): %s", in_carveout ? "PASS" : "FAIL");

    memset(&g_hw, 0, sizeof(g_hw));
    g_hw.ctx      = ctx;
    g_hw.mmio_len = window_limit;
    g_hw.rreg     = smu13HwRreg;
    g_hw.wreg     = smu13HwWreg;
    g_hw.fb_len   = 0;
    g_hw.fb_read  = nullptr;
    g_hw.delay_us = smu13HwDelayUs;

    size_t disc_size = sizeof(struct rb_disc) + 2 * sizeof(struct rb_ip);
    if (!g_disc) {
        g_disc = static_cast<struct rb_disc *>(IOMallocZero(disc_size));
    } else {
        memset(g_disc, 0, disc_size);
    }

    if (!g_disc) {
        rbgpu_log("Failed to allocate memory for rb_disc");
        rbgpu_diag_set_num("sw_init_ret", 2);
        return 2;
    }

    g_disc->nips = 2;

    g_disc->ips[0].hw_id    = RB_HWID_MP1;
    g_disc->ips[0].die      = 0;
    g_disc->ips[0].inst     = 0;
    g_disc->ips[0].nbase    = 1;
    g_disc->ips[0].major    = 13;
    g_disc->ips[0].minor    = 0;
    g_disc->ips[0].revision = 3;
    g_disc->ips[0].base[0]  = mp1_base;

    g_disc->ips[1].hw_id    = 4; // SMUIO
    g_disc->ips[1].die      = 0;
    g_disc->ips[1].inst     = 0;
    g_disc->ips[1].nbase    = 1;
    g_disc->ips[1].base[0]  = 0x16800;

    struct rb_smu13_dma dma;
    memset(&dma, 0, sizeof(dma));
    dma.cpu  = g_dma_cpu;
    dma.mc   = g_dma_gpu;
    dma.size = 0x1000;

    int init_ret = rb_smu13_init(&g_smu, &g_hw, g_disc, &dma, 0);
    if (init_ret != RB_SMU13_OK) {
        rbgpu_log("rb_smu13_init failed: %s (%d)", rb_smu13_strerror(init_ret), init_ret);
        rbgpu_diag_set_num("init_err", static_cast<uint64_t>(init_ret));
        rbgpu_diag_set_num("last_smu_err", static_cast<uint64_t>(init_ret));
        rbgpu_diag_set_num("sw_init_ret", static_cast<uint64_t>(smu13ErrToApple(init_ret)));
        return smu13ErrToApple(init_ret);
    }

    rbgpu_log("rb_smu13 initialized with ALLOW_QUERY (queries only)");
    rbgpu_log("[Check 5] slot_internal_sw_init succeeded");
    rbgpu_diag_set_num("sw_init_ret", 0);
    return 0;
}

static int slot_internal_hw_init(void *ctx) {
    rbgpu_log("slot_internal_hw_init entered (ctx=%p)", ctx);

    int err = rb_smu13_bringup(&g_smu);
    rbgpu_diag_set_num("bringup_err", static_cast<uint64_t>(err));
    if (err != RB_SMU13_OK) {
        rbgpu_log("rb_smu13_bringup failed: %s (%d)", rb_smu13_strerror(err), err);
        rbgpu_diag_set_num("last_smu_err", static_cast<uint64_t>(err));
        return smu13ErrToApple(err);
    }

    rbgpu_log("[Check 3] rb_smu13_bringup succeeded");
    if (g_smu.have_clocks) {
        rbgpu_diag_set_num("gfx_min", g_smu.clocks.min_gfx_clk);
        rbgpu_diag_set_num("gfx_max", g_smu.clocks.max_gfx_clk);
        rbgpu_diag_set_num("num_soc_levels", g_smu.clocks.num_soc_levels);
        rbgpu_diag_set_num("num_df_pstates", g_smu.clocks.num_df_pstates);

        rbgpu_log("[Check 3] Clocks: min_gfx=%u MHz, max_gfx=%u MHz, num_soc_levels=%u, num_df_pstates=%u",
                  g_smu.clocks.min_gfx_clk, g_smu.clocks.max_gfx_clk,
                  g_smu.clocks.num_soc_levels, g_smu.clocks.num_df_pstates);

        char keyBuf[32];
        for (uint8_t i = 0; i < g_smu.clocks.num_soc_levels && i < RB_SMU13_NUM_DPM_LEVELS; i++) {
            snprintf(keyBuf, sizeof(keyBuf), "soc_clk_%u", i);
            rbgpu_diag_set_num(keyBuf, g_smu.clocks.soc_clocks[i]);
            rbgpu_log("[Check 4] SOC level %u: %u MHz", i, g_smu.clocks.soc_clocks[i]);
        }
        for (uint8_t i = 0; i < g_smu.clocks.num_df_pstates && i < RB_SMU13_NUM_DF_PSTATES; i++) {
            snprintf(keyBuf, sizeof(keyBuf), "df_pstate_%u_fclk", i);
            rbgpu_diag_set_num(keyBuf, g_smu.clocks.df_pstate[i].fclk);
            snprintf(keyBuf, sizeof(keyBuf), "df_pstate_%u_memclk", i);
            rbgpu_diag_set_num(keyBuf, g_smu.clocks.df_pstate[i].memclk);
            snprintf(keyBuf, sizeof(keyBuf), "df_pstate_%u_volt", i);
            rbgpu_diag_set_num(keyBuf, g_smu.clocks.df_pstate[i].voltage);

            rbgpu_log("[Check 4] DF P-state %u: fclk=%u MHz, memclk=%u MHz, volt=%u",
                      i, g_smu.clocks.df_pstate[i].fclk,
                      g_smu.clocks.df_pstate[i].memclk,
                      g_smu.clocks.df_pstate[i].voltage);
        }
    }
    return 0;
}

static int slot_internal_sw_exit(void *ctx) {
    rbgpu_log("slot_internal_sw_exit entered");
    if (g_dma_handle && orig_smu_cgs_free_memory) {
        orig_smu_cgs_free_memory(ctx, g_dma_handle);
        g_dma_handle = nullptr;
        g_dma_cpu = nullptr;
        g_dma_gpu = 0;
    }
    if (g_disc) {
        size_t disc_size = sizeof(struct rb_disc) + 2 * sizeof(struct rb_ip);
        IOFree(g_disc, disc_size);
        g_disc = nullptr;
    }
    g_smu_ctx = nullptr;
    return 0;
}

static int slot_internal_hw_exit(void *ctx) {
    rbgpu_log("slot_internal_hw_exit entered");
    int err = rb_smu13_prepare_unload(&g_smu);
    if (err != RB_SMU13_OK) {
        rbgpu_log("rb_smu13_prepare_unload failed: %s (%d)", rb_smu13_strerror(err), err);
        rbgpu_diag_set_num("unload_err", static_cast<uint64_t>(err));
        rbgpu_diag_set_num("last_smu_err", static_cast<uint64_t>(err));
    }
    return smu13ErrToApple(err);
}

struct smu_event_msg {
    uint32_t size;
    uint32_t event;
    uint64_t *param;
};

static int slot_notify_event(void *ctx, smu_event_msg *msg, ...) {
    if (!msg) {
        rbgpu_log("slot_notify_event received NULL message");
        return 1;
    }

    uint32_t event = msg->event;
    rbgpu_log("slot_notify_event: event %u", event);

    if (event > 11) {
        return 2;
    }

    if (event == 0 || event == 4 || event == 6 || event == 10) {
        int err = rb_smu13_bringup(&g_smu);
        if (err != RB_SMU13_OK) {
            rbgpu_log("slot_notify_event: re-bringup for event %u failed: %s (%d)",
                      event, rb_smu13_strerror(err), err);
            rbgpu_diag_set_num("last_smu_err", static_cast<uint64_t>(err));
            return smu13ErrToApple(err);
        }
        return 0;
    }

    return 0;
}

static int slot_gfx_off_control(void *ctx, uint32_t op) {
    rbgpu_log("slot_gfx_off_control: op %u", op);
    static bool s_gfxoff_initialized = false;

    if (op == 0) {
        if (!s_gfxoff_initialized) {
            int ret = rb_smu13_enable_gfxoff(&g_smu);
            if (ret == RB_SMU13_OK) {
                s_gfxoff_initialized = true;
            } else {
                rbgpu_log("rb_smu13_enable_gfxoff error: %s (%d)", rb_smu13_strerror(ret), ret);
                rbgpu_diag_set_num("gfxoff_init_err", static_cast<uint64_t>(ret));
                rbgpu_diag_set_num("last_smu_err", static_cast<uint64_t>(ret));
            }
        }
        int err = rb_smu13_gfx_off_control(&g_smu, 1);
        if (err != RB_SMU13_OK) {
            rbgpu_log("rb_smu13_gfx_off_control(1) error: %s (%d)", rb_smu13_strerror(err), err);
            rbgpu_diag_set_num("gfxoff_ctrl_err", static_cast<uint64_t>(err));
            rbgpu_diag_set_num("last_smu_err", static_cast<uint64_t>(err));
        }
        return smu13ErrToApple(err);
    } else if (op == 1) {
        int err = rb_smu13_gfx_off_control(&g_smu, 0);
        if (err != RB_SMU13_OK) {
            rbgpu_log("rb_smu13_gfx_off_control(0) error: %s (%d)", rb_smu13_strerror(err), err);
            rbgpu_diag_set_num("gfxoff_ctrl_err", static_cast<uint64_t>(err));
            rbgpu_diag_set_num("last_smu_err", static_cast<uint64_t>(err));
        }
        return smu13ErrToApple(err);
    }

    return 4;
}

static int slot_system_features_control(void *ctx, uint64_t *mask) {
    rbgpu_log("slot_system_features_control entered");
    return 0;
}

static int slot_power_feature_caps(void *ctx, uint64_t *caps) {
    rbgpu_log("slot_power_feature_caps entered");
    if (caps) {
        *caps = 0;
    }
    return 0;
}

static int slot_unload_smu(void *ctx, ...) {
    rbgpu_log("slot_unload_smu entered");
    int err = rb_smu13_prepare_unload(&g_smu);
    if (err != RB_SMU13_OK) {
        rbgpu_log("rb_smu13_prepare_unload error: %s (%d)", rb_smu13_strerror(err), err);
        rbgpu_diag_set_num("unload_err", static_cast<uint64_t>(err));
        rbgpu_diag_set_num("last_smu_err", static_cast<uint64_t>(err));
    }
    return smu13ErrToApple(err);
}

static int hooked_smu_init_function_pointer_list(void *ctx, uint64_t ver, uint64_t rev) {
    rbgpu_diag_inc("smu_hook_calls");
    rbgpu_log("hooked_smu_init_function_pointer_list called (ctx=%p, ver=0x%llx, rev=0x%llx)", ctx, ver, rev);

    uint32_t index = 0;
    if (orig_smu_get_hw_version) {
        index = orig_smu_get_hw_version(ver, rev);
    } else {
        uint32_t major = ver & 0xFF;
        uint32_t minor = (ver >> 32) & 0xFF;
        uint32_t r     = rev & 0xFF;
        if (major == 13 && minor == 0 && r == 3) {
            index = 0x10;
        }
    }

    rbgpu_diag_set_num("mp1_hw_index", index);
    rbgpu_log("smu_get_hw_version resolved to index 0x%02X", index);

    if (index != 0x10) {
        if (orig_smu_init_function_pointer_list) {
            return orig_smu_init_function_pointer_list(ctx, ver, rev);
        }
        return 2;
    }

    *reinterpret_cast<uint32_t *>(reinterpret_cast<uintptr_t>(ctx) + 0x278) = 0x10;
    memset(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(ctx) + 0x650), 0, 0x740 - 0x650);

    if (orig_smu_read_config_space) {
        orig_smu_read_config_space(ctx);
    }

    *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(ctx) + 0x650) = reinterpret_cast<void *>(slot_internal_sw_init);
    *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(ctx) + 0x658) = reinterpret_cast<void *>(slot_internal_hw_init);
    *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(ctx) + 0x660) = reinterpret_cast<void *>(slot_internal_sw_exit);
    *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(ctx) + 0x668) = reinterpret_cast<void *>(slot_internal_hw_exit);
    *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(ctx) + 0x680) = reinterpret_cast<void *>(slot_notify_event);
    *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(ctx) + 0x6E0) = reinterpret_cast<void *>(slot_gfx_off_control);
    *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(ctx) + 0x6E8) = reinterpret_cast<void *>(slot_system_features_control);
    *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(ctx) + 0x6F8) = reinterpret_cast<void *>(slot_power_feature_caps);
    *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(ctx) + 0x720) = reinterpret_cast<void *>(slot_unload_smu);

    rbgpu_log("H-SMU-1: registered SMU 13 slot table");
    return 0;
}

static uint8_t hooked_ttlIsSupportedDevice(void *dev_info_20) {
    rbgpu_diag_inc("ttl_is_supp_calls");
    auto dwords = reinterpret_cast<uint32_t *>(dev_info_20);
    uint32_t dev_id = dwords ? dwords[0] : 0;
    uint32_t bus_no = dwords ? dwords[1] : 0;
    uint32_t attr   = dwords ? dwords[0x64 / 4] : 0;

    rbgpu_log("_ttlIsSupportedDevice(ptr=%p): devId=0x%04X, bus=%u, attr=0x%08X", dev_info_20, dev_id, bus_no, attr);

    uint8_t ret = 0;
    if (orig_ttl_is_supported_device) {
        ret = orig_ttl_is_supported_device(dev_info_20);
    }
    rbgpu_log("_ttlIsSupportedDevice original returned %u", ret);

    if (g_spoof_ttl_mode && ret == 0 && dev_id == 0x1681) {
        ret = 1;
        rbgpu_log("_ttlIsSupportedDevice: -rbgpuspoofttl active -> overriding return to 1 (supported)");
    }

    rbgpu_diag_set_num("ttl_is_supp_ret", ret);
    return ret;
}

static int hooked_ttl_hw_init(void *ctx) {
    rbgpu_diag_inc("ttl_hw_init_calls");
    rbgpu_log("_ttl_hw_init entered: ctx=%p (pspHwMode: %s)", ctx, g_psp_hw_mode ? "ENABLED" : "DISABLED");

    if (!g_psp_hw_mode) {
        rbgpu_log("_ttl_hw_init: DRY RUN active (-rbgpupsphw not set) -> aborting before hardware writes, returning 0");
        rbgpu_diag_set_num("ttl_hw_init_ret", 0);
        return 0;
    }

    int ret = 0;
    if (orig_ttl_hw_init) {
        ret = orig_ttl_hw_init(ctx);
    }
    rbgpu_log("_ttl_hw_init returned %d", ret);
    rbgpu_diag_set_num("ttl_hw_init_ret", ret);
    return ret;
}

static uint8_t hooked_ipi_execute_swip_entry_point(void *instance, uint32_t swip_client_id, uint32_t event_id, void *context) {
    rbgpu_diag_inc("ipi_exec_calls");
    rbgpu_log("_IpiExecuteSwipEntryPoint entered: instance=%p, client_id=%u, event_id=%u, context=%p",
              instance, swip_client_id, event_id, context);

    uint8_t ret = 0;
    if (orig_ipi_execute_swip_entry_point) {
        ret = orig_ipi_execute_swip_entry_point(instance, swip_client_id, event_id, context);
    }

    rbgpu_log("_IpiExecuteSwipEntryPoint returned %u (client_id=%u, event_id=%u)", ret, swip_client_id, event_id);
    rbgpu_diag_set_num("ipi_exec_ret", ret);
    return ret;
}

static void *hooked_IpiGetDiscoveryInfo(void *instance, uint32_t client_id) {
    rbgpu_diag_inc("ipi_disc_calls");
    void *ret = nullptr;
    if (orig_ipi_get_discovery_info) {
        ret = orig_ipi_get_discovery_info(instance, client_id);
    }
    rbgpu_log("_IpiGetDiscoveryInfo(instance=%p, client_id=%u) returned %p", instance, client_id, ret);
    if (client_id == 7 && ret) {
        uint32_t num_inst = *reinterpret_cast<uint32_t *>(ret);
        uint64_t span = static_cast<uint64_t>(num_inst) * 0x210;
        uint32_t major = *reinterpret_cast<uint32_t *>(reinterpret_cast<uintptr_t>(ret) + 0xC);
        uint32_t minor = *reinterpret_cast<uint32_t *>(reinterpret_cast<uintptr_t>(ret) + 0x10);
        uint32_t rev   = *reinterpret_cast<uint32_t *>(reinterpret_cast<uintptr_t>(ret) + 0x14);
        rbgpu_log("_IpiGetDiscoveryInfo(client_id=7) -> %p (inst=%u, span=0x%llx vs 0x235, major=%u, minor=%u, rev=%u)",
                  ret, num_inst, span, major, minor, rev);
        rbgpu_diag_set_num("gc_disc_inst", num_inst);
        rbgpu_diag_set_num("gc_disc_span", span);
        rbgpu_diag_set_num("gc_disc_major", major);
        rbgpu_diag_set_num("gc_disc_minor", minor);
        rbgpu_diag_set_num("gc_disc_rev", rev);
    }
    return ret;
}

static int hooked_ipi_psp_sw_init(void *dev) {
    rbgpu_diag_inc("psp_sw_init_calls");
    uint64_t *dev_ptr = reinterpret_cast<uint64_t *>(dev);
    void *fn_ptr = dev_ptr ? reinterpret_cast<void *>(dev_ptr[1]) : nullptr;

    rbgpu_log("_ipi_psp_sw_init entered: dev=%p, fn_ptr@+0x8=%p", dev, fn_ptr);

    int ret = 0;
    if (orig_ipi_psp_sw_init) {
        ret = orig_ipi_psp_sw_init(dev);
    }

    rbgpu_log("_ipi_psp_sw_init returned %d", ret);
    rbgpu_diag_set_num("psp_sw_init_ret", ret);
    return ret;
}

static int hooked_ipi_psp_hw_init(void *dev) {
    rbgpu_diag_inc("psp_hw_init_calls");
    rbgpu_log("_ipi_psp_hw_init entered: dev=%p", dev);

    int ret = 0;
    if (orig_ipi_psp_hw_init) {
        ret = orig_ipi_psp_hw_init(dev);
    }

    rbgpu_log("_ipi_psp_hw_init returned %d", ret);
    rbgpu_diag_set_num("psp_hw_init_ret", ret);
    return ret;
}

static int hooked_psp_sw_init(void *param1, void *param2) {
    rbgpu_diag_inc("psp_sw_init_sub_calls");
    uint32_t p1_0 = param1 ? *reinterpret_cast<uint32_t *>(param1) : 0;
    uint32_t p1_8 = param1 ? *reinterpret_cast<uint32_t *>(reinterpret_cast<uintptr_t>(param1) + 0x8) : 0;
    uint64_t p1_218 = param1 ? *reinterpret_cast<uint64_t *>(reinterpret_cast<uintptr_t>(param1) + 0x218) : 0;
    uint64_t p1_220 = param1 ? *reinterpret_cast<uint64_t *>(reinterpret_cast<uintptr_t>(param1) + 0x220) : 0;
    uint32_t p2_0 = param2 ? *reinterpret_cast<uint32_t *>(param2) : 0;

    rbgpu_log("_psp_sw_init entered: p1=%p (0x%x, +8=0x%x, +218=%p, +220=%p), p2=%p (0x%x)",
              param1, p1_0, p1_8, reinterpret_cast<void *>(p1_218), reinterpret_cast<void *>(p1_220), param2, p2_0);

    int ret = 1;
    if (orig_psp_sw_init) {
        ret = orig_psp_sw_init(param1, param2);
    }

    rbgpu_log("_psp_sw_init returned %d", ret);
    rbgpu_diag_set_num("psp_sw_init_sub_ret", ret);
    return ret;
}

static int hooked_psp_hw_init(void *psp_buf, void *arg2, void *arg3) {
    rbgpu_diag_inc("psp_hw_sub_calls");
    uint32_t a2_18 = arg2 ? *reinterpret_cast<uint32_t *>(reinterpret_cast<uintptr_t>(arg2) + 0x18) : 0;
    uint8_t  a2_1c = arg2 ? *reinterpret_cast<uint8_t *>(reinterpret_cast<uintptr_t>(arg2) + 0x1c) : 0;
    uint8_t  a2_1d = arg2 ? *reinterpret_cast<uint8_t *>(reinterpret_cast<uintptr_t>(arg2) + 0x1d) : 0;
    uint8_t  a2_38 = arg2 ? *reinterpret_cast<uint8_t *>(reinterpret_cast<uintptr_t>(arg2) + 0x38) : 0;
    uint8_t  a2_39 = arg2 ? *reinterpret_cast<uint8_t *>(reinterpret_cast<uintptr_t>(arg2) + 0x39) : 0;

    rbgpu_log("_psp_hw_init entered: psp_buf=%p, arg2=%p (+0x18=0x%x, +0x1c=0x%02x, +0x1d=0x%02x, +0x38=0x%02x, +0x39=0x%02x), arg3=%p",
              psp_buf, arg2, a2_18, a2_1c, a2_1d, a2_38, a2_39, arg3);
    rbgpu_diag_set_num("psp_hw_a2_18", a2_18);

    int ret = 1;
    if (orig_psp_hw_init) {
        ret = orig_psp_hw_init(psp_buf, arg2, arg3);
    }

    rbgpu_log("_psp_hw_init returned %d", ret);
    rbgpu_diag_set_num("psp_hw_sub_ret", ret);
    return ret;
}

static int hooked_psp_cos_alloc_memory(void *ctx, uint32_t size, uint32_t align, void **out_ptr) {
    rbgpu_diag_inc("psp_alloc_calls");
    int ret = 1;
    if (orig_psp_cos_alloc_memory) {
        ret = orig_psp_cos_alloc_memory(ctx, size, align, out_ptr);
    }
    void *allocated = (out_ptr && ret == 0) ? *out_ptr : nullptr;
    rbgpu_log("_psp_cos_alloc_memory(ctx=%p, size=0x%x, align=%u) -> ret=%d, ptr=%p",
              ctx, size, align, ret, allocated);
    rbgpu_diag_set_num("psp_alloc_ret", ret);
    return ret;
}

static int hooked_psp_asic_type_init(void *psp_buf, void *disc_entry) {
    rbgpu_diag_inc("psp_asic_init_calls");
    uint32_t d_8  = disc_entry ? *reinterpret_cast<uint32_t *>(reinterpret_cast<uintptr_t>(disc_entry) + 0x8) : 0;
    uint32_t d_C  = disc_entry ? *reinterpret_cast<uint32_t *>(reinterpret_cast<uintptr_t>(disc_entry) + 0xC) : 0;
    uint32_t d_10 = disc_entry ? *reinterpret_cast<uint32_t *>(reinterpret_cast<uintptr_t>(disc_entry) + 0x10) : 0;

    rbgpu_log("_psp_asic_type_init entered: buf=%p, disc=%p (+8=0x%x, +C=0x%x, +10=0x%x)",
              psp_buf, disc_entry, d_8, d_C, d_10);

    int ret = 1;
    if (orig_psp_asic_type_init) {
        ret = orig_psp_asic_type_init(psp_buf, disc_entry);
    }

    uint32_t asic_type = psp_buf ? *reinterpret_cast<uint32_t *>(reinterpret_cast<uintptr_t>(psp_buf) + 0x7d44) : 0;
    rbgpu_log("_psp_asic_type_init returned %d (asic_type@+0x7d44 = %u)", ret, asic_type);
    rbgpu_diag_set_num("psp_asic_ret", ret);
    rbgpu_diag_set_num("psp_asic_type", asic_type);
    return ret;
}

static int hooked_psp_init_pfn_ptr(void *psp_buf) {
    rbgpu_diag_inc("psp_pfn_calls");
    uint32_t *asic_type_ptr = psp_buf ? reinterpret_cast<uint32_t *>(reinterpret_cast<uintptr_t>(psp_buf) + 0x7d44) : nullptr;
    uint32_t orig_asic = asic_type_ptr ? *asic_type_ptr : 0;
    rbgpu_log("_psp_init_pfn_ptr entered: buf=%p (asic_type=%u)", psp_buf, orig_asic);

    if (g_spoof_ttl_mode && asic_type_ptr && orig_asic == 19) {
        *asic_type_ptr = 8;
        rbgpu_log("_psp_init_pfn_ptr: -rbgpuspoofttl active -> spoofing asic_type 19 -> 8 for table binding");
    }

    int ret = 1;
    if (orig_psp_init_pfn_ptr) {
        ret = orig_psp_init_pfn_ptr(psp_buf);
    }

    if (g_spoof_ttl_mode && asic_type_ptr && orig_asic == 19) {
        *asic_type_ptr = 19;
        rbgpu_log("_psp_init_pfn_ptr: restored asic_type to 19; ret=%d", ret);
    }

    uint64_t *pfn_ring_init   = psp_buf ? reinterpret_cast<uint64_t *>(reinterpret_cast<uintptr_t>(psp_buf) + 0x7d50) : nullptr;
    uint64_t *pfn_ring_create = psp_buf ? reinterpret_cast<uint64_t *>(reinterpret_cast<uintptr_t>(psp_buf) + 0x7d60) : nullptr;
    uint64_t *pfn_is_sos      = psp_buf ? reinterpret_cast<uint64_t *>(reinterpret_cast<uintptr_t>(psp_buf) + 0x7de8) : nullptr;
    rbgpu_log("_psp_init_pfn_ptr: pfn_ring_init=%p, pfn_ring_create=%p, pfn_is_sos=%p",
              pfn_ring_init ? reinterpret_cast<void *>(*pfn_ring_init) : nullptr,
              pfn_ring_create ? reinterpret_cast<void *>(*pfn_ring_create) : nullptr,
              pfn_is_sos ? reinterpret_cast<void *>(*pfn_is_sos) : nullptr);

    rbgpu_log("_psp_init_pfn_ptr returned %d", ret);
    rbgpu_diag_set_num("psp_pfn_ret", ret);
    return ret;
}

static int hooked_psp_read_config(void *psp_buf) {
    rbgpu_diag_inc("psp_read_cfg_calls");
    int ret = 0;
    if (orig_psp_read_config) {
        ret = orig_psp_read_config(psp_buf);
    }
    uint64_t flags = psp_buf ? *reinterpret_cast<uint64_t *>(reinterpret_cast<uintptr_t>(psp_buf) + 0x28) : 0;
    rbgpu_log("_psp_read_config exited: buf=%p, ret=%d, flags@+0x28=0x%llx", psp_buf, ret, flags);
    return ret;
}

static int hooked_psp_allocate_private_mem(void *psp_buf) {
    rbgpu_diag_inc("psp_alloc_priv_calls");
    rbgpu_log("_psp_allocate_private_mem entered: psp_buf=%p", psp_buf);
    int ret = 1;
    if (orig_psp_allocate_private_mem) {
        ret = orig_psp_allocate_private_mem(psp_buf);
    }
    rbgpu_log("_psp_allocate_private_mem returned %d", ret);
    rbgpu_diag_set_num("psp_alloc_priv_ret", ret);
    return ret;
}

static int hooked_psp_ring_init(void *psp_buf, uint32_t ring_type) {
    rbgpu_diag_inc("psp_ring_init_calls");
    rbgpu_log("_psp_ring_init entered: psp_buf=%p, ring_type=%u", psp_buf, ring_type);
    int ret = 1;
    if (orig_psp_ring_init) {
        ret = orig_psp_ring_init(psp_buf, ring_type);
    }
    rbgpu_log("_psp_ring_init returned %d (ring_type=%u)", ret, ring_type);
    rbgpu_diag_set_num("psp_ring_init_ret", ret);
    return ret;
}

static int hooked_psp_ring_create(void *psp_buf, uint32_t ring_type) {
    rbgpu_diag_inc("psp_ring_create_calls");
    rbgpu_log("_psp_ring_create entered: psp_buf=%p, ring_type=%u", psp_buf, ring_type);
    int ret = 1;
    if (orig_psp_ring_create) {
        ret = orig_psp_ring_create(psp_buf, ring_type);
    }
    rbgpu_log("_psp_ring_create returned %d (ring_type=%u)", ret, ring_type);
    rbgpu_diag_set_num("psp_ring_create_ret", ret);
    return ret;
}

static int hooked_psp_bootloader_is_sos_running(void *psp_buf) {
    rbgpu_diag_inc("psp_is_sos_calls");
    rbgpu_log("_psp_bootloader_is_sos_running entered: psp_buf=%p", psp_buf);
    int ret = 1;
    if (orig_psp_bootloader_is_sos_running) {
        ret = orig_psp_bootloader_is_sos_running(psp_buf);
    }
    rbgpu_log("_psp_bootloader_is_sos_running returned %d", ret);
    rbgpu_diag_set_num("psp_is_sos_ret", ret);
    return ret;
}

static int hooked_psp_cmd_km_init(void *psp_buf) {
    rbgpu_diag_inc("psp_km_init_calls");
    rbgpu_log("_psp_cmd_km_init entered: psp_buf=%p", psp_buf);
    int ret = 1;
    if (orig_psp_cmd_km_init) {
        ret = orig_psp_cmd_km_init(psp_buf);
    }
    rbgpu_log("_psp_cmd_km_init returned %d", ret);
    rbgpu_diag_set_num("psp_km_init_ret", ret);
    return ret;
}

static int hooked_psp_tmr_init(void *psp_buf) {
    rbgpu_diag_inc("psp_tmr_init_calls");
    rbgpu_log("_psp_tmr_init entered: psp_buf=%p", psp_buf);
    int ret = 1;
    if (orig_psp_tmr_init) {
        ret = orig_psp_tmr_init(psp_buf);
    }
    rbgpu_log("_psp_tmr_init returned %d", ret);
    rbgpu_diag_set_num("psp_tmr_init_ret", ret);
    return ret;
}

static int hooked_psp_tmr_load(void *psp_buf) {
    rbgpu_diag_inc("psp_tmr_load_calls");
    rbgpu_log("_psp_tmr_load entered: psp_buf=%p", psp_buf);
    int ret = 1;
    if (orig_psp_tmr_load) {
        ret = orig_psp_tmr_load(psp_buf);
    }
    rbgpu_log("_psp_tmr_load returned %d", ret);
    rbgpu_diag_set_num("psp_tmr_load_ret", ret);
    return ret;
}

static int hooked_psp_dtm_init(void *psp_buf) {
    rbgpu_diag_inc("psp_dtm_init_calls");
    rbgpu_log("_psp_dtm_init entered: psp_buf=%p", psp_buf);
    int ret = 1;
    if (orig_psp_dtm_init) {
        ret = orig_psp_dtm_init(psp_buf);
    }
    rbgpu_log("_psp_dtm_init returned %d", ret);
    rbgpu_diag_set_num("psp_dtm_init_ret", ret);
    return ret;
}

static int hooked_psp_np_fw_load(void *psp_buf) {
    rbgpu_diag_inc("psp_np_fw_load_calls");
    rbgpu_log("_psp_np_fw_load entered: psp_buf=%p", psp_buf);
    int ret = 1;
    if (orig_psp_np_fw_load) {
        ret = orig_psp_np_fw_load(psp_buf);
    }
    rbgpu_log("_psp_np_fw_load returned %d", ret);
    rbgpu_diag_set_num("psp_np_fw_load_ret", ret);
    return ret;
}

static int hooked_ipi_gc_sw_init(void *dev) {
    rbgpu_diag_inc("gc_sw_init_calls");
    uint64_t *dev_ptr = reinterpret_cast<uint64_t *>(dev);
    void *fn_ptr = dev_ptr ? reinterpret_cast<void *>(dev_ptr[1]) : nullptr;

    rbgpu_log("_ipi_gc_sw_init entered: dev=%p, fn_ptr@+0x8=%p", dev, fn_ptr);

    int ret = 0;
    if (orig_ipi_gc_sw_init) {
        ret = orig_ipi_gc_sw_init(dev);
    }

    rbgpu_log("_ipi_gc_sw_init returned %d", ret);
    rbgpu_diag_set_num("gc_sw_init_ret", ret);
    return ret;
}

static int hooked_ipi_gc_hw_init(void *dev) {
    rbgpu_diag_inc("gc_hw_init_calls");
    rbgpu_log("_ipi_gc_hw_init entered: dev=%p", dev);

    int ret = 0;
    if (orig_ipi_gc_hw_init) {
        ret = orig_ipi_gc_hw_init(dev);
    }

    rbgpu_log("_ipi_gc_hw_init returned %d", ret);
    rbgpu_diag_set_num("gc_hw_init_ret", ret);
    return ret;
}

static int hooked_gc_sw_init(void *param1, void *param2) {
    rbgpu_diag_inc("gc_sw_sub_calls");
    uint32_t p1_sz = param1 ? *reinterpret_cast<uint32_t *>(param1) : 0;
    uint32_t p2_sz = param2 ? *reinterpret_cast<uint32_t *>(param2) : 0;
    uint32_t d_8  = param1 ? *reinterpret_cast<uint32_t *>(reinterpret_cast<uintptr_t>(param1) + 0xC) : 0;
    uint32_t d_C  = param1 ? *reinterpret_cast<uint32_t *>(reinterpret_cast<uintptr_t>(param1) + 0x10) : 0;
    uint32_t d_10 = param1 ? *reinterpret_cast<uint32_t *>(reinterpret_cast<uintptr_t>(param1) + 0x14) : 0;

    rbgpu_log("_gc_sw_init entered: p1=%p (sz=0x%x, +C=0x%x, +10=0x%x, +14=0x%x), p2=%p (sz=0x%x)",
              param1, p1_sz, d_8, d_C, d_10, param2, p2_sz);

    int ret = 1;
    if (orig_gc_sw_init) {
        ret = orig_gc_sw_init(param1, param2);
    }

    rbgpu_log("_gc_sw_init returned %d", ret);
    rbgpu_diag_set_num("gc_sw_sub_ret", ret);
    return ret;
}

static int hooked_gc_hw_init(void *gc_buf, void *arg2, void *arg3) {
    rbgpu_diag_inc("gc_hw_sub_calls");
    rbgpu_log("_gc_hw_init entered: gc_buf=%p, arg2=%p, arg3=%p", gc_buf, arg2, arg3);

    int ret = 1;
    if (orig_gc_hw_init) {
        ret = orig_gc_hw_init(gc_buf, arg2, arg3);
    }

    rbgpu_log("_gc_hw_init returned %d", ret);
    rbgpu_diag_set_num("gc_hw_sub_ret", ret);
    return ret;
}

static uint32_t hooked_gc_get_hw_version(void *version_slice) {
    rbgpu_diag_inc("gc_get_ver_calls");
    uint32_t ver = 0;
    if (orig_gc_get_hw_version) {
        ver = orig_gc_get_hw_version(version_slice);
    }
    rbgpu_log("_gc_get_hw_version(slice=%p) returned 0x%08X", version_slice, ver);
    rbgpu_diag_set_num("gc_hw_version", ver);
    return ver;
}

static void hooked_gc_get_config(void *gc_buf, uint32_t hw_version) {
    rbgpu_diag_inc("gc_get_cfg_calls");
    rbgpu_log("_gc_get_config entered: gc_buf=%p, hw_version=0x%08X", gc_buf, hw_version);
    if (orig_gc_get_config) {
        orig_gc_get_config(gc_buf, hw_version);
    }
    rbgpu_log("_gc_get_config exited");
}

static void hooked_gc_get_hw_config_tbl_info(void *gc_buf, uint32_t hw_version) {
    rbgpu_diag_inc("gc_tbl_info_calls");
    rbgpu_log("_gc_get_hw_config_tbl_info entered: gc_buf=%p, hw_version=0x%08X", gc_buf, hw_version);
    if (orig_gc_get_hw_config_tbl_info) {
        orig_gc_get_hw_config_tbl_info(gc_buf, hw_version);
    }
    uint32_t sz = gc_buf ? *reinterpret_cast<uint32_t *>(reinterpret_cast<uintptr_t>(gc_buf) + 0xd38) : 0;
    rbgpu_log("_gc_get_hw_config_tbl_info exited: table_size@+0xd38=0x%x", sz);
    rbgpu_diag_set_num("gc_tbl_info_sz", sz);
}

static int hooked_gc_set_fw_entry_info(void *p1, void *p2, uint32_t rev, void *p4) {
    rbgpu_diag_inc("gc_set_fw_info_calls");
    rbgpu_log("_gc_set_fw_entry_info entered: p1=%p, p2=%p, rev=%u, p4=%p", p1, p2, rev, p4);
    int ret = 1;
    if (orig_gc_set_fw_entry_info) {
        ret = orig_gc_set_fw_entry_info(p1, p2, rev, p4);
    }
    rbgpu_log("_gc_set_fw_entry_info returned %d", ret);
    rbgpu_diag_set_num("gc_set_fw_info_ret", ret);
    return ret;
}

static int hooked_gc_init_fcn_ptr_list(void *gc_buf, uint32_t hw_version) {
    rbgpu_diag_inc("gc_fcn_ptr_calls");
    rbgpu_log("_gc_init_fcn_ptr_list entered: gc_buf=%p, hw_version=0x%08X", gc_buf, hw_version);

    uint32_t version_to_pass = hw_version;
    if (g_spoof_ttl_mode && hw_version == 0xA0303) {
        version_to_pass = 0xA0300;
        rbgpu_log("_gc_init_fcn_ptr_list: -rbgpuspoofttl active -> spoofing hw_version 0x%08X -> 0x%08X",
                  hw_version, version_to_pass);
    }

    int ret = 2;
    if (orig_gc_init_fcn_ptr_list) {
        ret = orig_gc_init_fcn_ptr_list(gc_buf, version_to_pass);
    }
    rbgpu_log("_gc_init_fcn_ptr_list returned %d (passed ver: 0x%08X)", ret, version_to_pass);
    rbgpu_diag_set_num("gc_fcn_ptr_ret", ret);
    return ret;
}

static int hooked_gc_get_doorbell_requirements(void *gc_buf, void *out_reqs) {
    rbgpu_diag_inc("gc_doorbell_req_calls");
    rbgpu_log("_gc_get_doorbell_requirements entered: gc_buf=%p, out_reqs=%p", gc_buf, out_reqs);
    int ret = 1;
    if (orig_gc_get_doorbell_requirements) {
        ret = orig_gc_get_doorbell_requirements(gc_buf, out_reqs);
    }
    rbgpu_log("_gc_get_doorbell_requirements returned %d", ret);
    rbgpu_diag_set_num("gc_doorbell_req_ret", ret);
    return ret;
}

static uint8_t hooked_ttlDevGetDoorbellRange(void *dev, uint32_t type, uint32_t idx, void *out_info) {
    rbgpu_diag_inc("gc_doorbell_calls");
    uint8_t ret = 0;
    if (orig_ttl_dev_get_doorbell_range) {
        ret = orig_ttl_dev_get_doorbell_range(dev, type, idx, out_info);
    }
    rbgpu_log("_ttlDevGetDoorbellRange(dev=%p, type=%u, idx=%u, out=%p) returned %u",
              dev, type, idx, out_info, ret);
    rbgpu_diag_set_num("gc_doorbell_ret", ret);
    return ret;
}

static uint8_t hooked_IpiSetFwEntry(void *instance, void *fw_entry) {
    rbgpu_diag_inc("gc_set_fw_calls");
    uint32_t fw_id = fw_entry ? *reinterpret_cast<uint32_t *>(fw_entry) : 0;
    uint8_t ret = 0;
    if (orig_ipi_set_fw_entry) {
        ret = orig_ipi_set_fw_entry(instance, fw_entry);
    }
    rbgpu_log("_IpiSetFwEntry(instance=%p, fw_id=%u) returned %u", instance, fw_id, ret);
    rbgpu_diag_set_num("gc_set_fw_ret", ret);
    return ret;
}

static uint32_t hooked_ttl_initialize(void *arg1, void *arg2) {
    rbgpu_diag_inc("ttl_raw_init_calls");
    rbgpu_diag_set_num("ttl_raw_init_arg1", reinterpret_cast<uint64_t>(arg1));
    rbgpu_diag_set_num("ttl_raw_init_arg2", reinterpret_cast<uint64_t>(arg2));

    auto params = reinterpret_cast<const uint64_t *>(arg1);
    rbgpu_log("_ttl_initialize entered: params=%p, ctx_38=%p", arg1, arg2);

    if (params) {
        rbgpu_diag_set_num("ttl_param_q0", params[0]);
        rbgpu_diag_set_num("ttl_param_q1", params[1]);
        rbgpu_diag_set_num("ttl_param_q2", params[2]);
        rbgpu_diag_set_num("ttl_param_q3", params[3]);
        rbgpu_diag_set_num("ttl_param_q4", params[4]);
        rbgpu_diag_set_num("ttl_param_q5", params[5]);

        rbgpu_log("ttl_params[0x00..0x18]: 0x%016llx 0x%016llx 0x%016llx 0x%016llx",
                  params[0], params[1], params[2], params[3]);
        rbgpu_log("ttl_params[0x20..0x38]: 0x%016llx 0x%016llx 0x%016llx 0x%016llx",
                  params[4], params[5], params[6], params[7]);
        rbgpu_log("ttl_params[0x40..0x58]: 0x%016llx 0x%016llx 0x%016llx 0x%016llx",
                  params[8], params[9], params[10], params[11]);
        rbgpu_log("ttl_params[0x60..0x78]: 0x%016llx 0x%016llx 0x%016llx 0x%016llx",
                  params[12], params[13], params[14], params[15]);
        rbgpu_log("ttl_params[0x80..0x98]: 0x%016llx 0x%016llx 0x%016llx 0x%016llx",
                  params[16], params[17], params[18], params[19]);
        rbgpu_log("ttl_params[0xA0]:      0x%016llx", params[20]);
    }

    uint32_t ret = 1;
    if (orig_ttl_initialize) {
        ret = orig_ttl_initialize(arg1, arg2);
    }

    rbgpu_log("_ttl_initialize returned status = 0x%08x", ret);
    rbgpu_diag_set_num("ttl_raw_init_ret", ret);
    return ret;
}

static int hooked_AmdTtlServices_initialize(void *this_ptr) {
    rbgpu_diag_inc("ttl_init_calls");
    rbgpu_diag_set_num("ttl_init_this", reinterpret_cast<uint64_t>(this_ptr));
    rbgpu_log("AmdTtlServices::initialize() entered (this=%p)", this_ptr);
    int ret = 0;
    if (orig_amdtll_initialize) {
        ret = orig_amdtll_initialize(this_ptr);
    }
    rbgpu_log("AmdTtlServices::initialize() returned 0x%08X (this=%p)", ret, this_ptr);
    rbgpu_diag_set_num("ttl_init_ret", static_cast<uint32_t>(ret));
    return ret;
}

static void hooked_AmdTtlServices_setFirmwareDirectory(void *this_ptr, void *fw_dir) {
    rbgpu_diag_inc("ttl_set_fw_dir_calls");
    rbgpu_diag_set_num("ttl_set_fw_dir_this", reinterpret_cast<uint64_t>(this_ptr));
    rbgpu_diag_set_num("ttl_set_fw_dir_ptr", reinterpret_cast<uint64_t>(fw_dir));

    uint64_t pre_570 = this_ptr ? *reinterpret_cast<uint64_t *>(reinterpret_cast<uintptr_t>(this_ptr) + 0x570) : 0;
    uint64_t pre_958 = this_ptr ? *reinterpret_cast<uint64_t *>(reinterpret_cast<uintptr_t>(this_ptr) + 0x958) : 0;
    rbgpu_log("AmdTtlServices::setFirmwareDirectory(this=%p, dir=%p) pre +0x570=0x%llx, pre +0x958=0x%llx",
              this_ptr, fw_dir, pre_570, pre_958);

    if (orig_amdtll_set_fw_dir) {
        orig_amdtll_set_fw_dir(this_ptr, fw_dir);
    }

    uint64_t post_570 = this_ptr ? *reinterpret_cast<uint64_t *>(reinterpret_cast<uintptr_t>(this_ptr) + 0x570) : 0;
    uint64_t post_958 = this_ptr ? *reinterpret_cast<uint64_t *>(reinterpret_cast<uintptr_t>(this_ptr) + 0x958) : 0;
    rbgpu_diag_set_num("ttl_set_fw_dir_post_570", post_570);
    rbgpu_diag_set_num("ttl_set_fw_dir_post_958", post_958);
    rbgpu_log("AmdTtlServices::setFirmwareDirectory done: this=%p post +0x570=0x%llx, post +0x958=0x%llx",
              this_ptr, post_570, post_958);
}

static int hooked_AmdTtlServices_initialize_input(void *this_ptr, void *input) {
    rbgpu_diag_inc("ttl_init_inp_calls");
    rbgpu_diag_set_num("ttl_init_inp_this", reinterpret_cast<uint64_t>(this_ptr));
    auto inp = reinterpret_cast<uint64_t *>(input);

    uint64_t f560 = this_ptr ? *reinterpret_cast<uint64_t *>(reinterpret_cast<uintptr_t>(this_ptr) + 0x560) : 0;
    uint64_t f568 = this_ptr ? *reinterpret_cast<uint64_t *>(reinterpret_cast<uintptr_t>(this_ptr) + 0x568) : 0;
    uint64_t f570 = this_ptr ? *reinterpret_cast<uint64_t *>(reinterpret_cast<uintptr_t>(this_ptr) + 0x570) : 0;
    uint32_t f578 = this_ptr ? *reinterpret_cast<uint32_t *>(reinterpret_cast<uintptr_t>(this_ptr) + 0x578) : 0;
    uint8_t  f598 = this_ptr ? *reinterpret_cast<uint8_t *>(reinterpret_cast<uintptr_t>(this_ptr) + 0x598) : 0;
    uint64_t f958 = this_ptr ? *reinterpret_cast<uint64_t *>(reinterpret_cast<uintptr_t>(this_ptr) + 0x958) : 0;

    rbgpu_diag_set_num("ttl_inp_0", inp ? inp[0] : 0);
    rbgpu_diag_set_num("ttl_inp_1", inp ? inp[1] : 0);
    rbgpu_diag_set_num("ttl_inp_2", inp ? inp[2] : 0);
    rbgpu_diag_set_num("ttl_inp_4", inp ? inp[4] : 0);
    rbgpu_diag_set_num("ttl_inp_5", inp ? inp[5] : 0);
    rbgpu_diag_set_num("ttl_inp_6", inp ? inp[6] : 0);
    rbgpu_diag_set_num("ttl_this_570", f570);

    rbgpu_log("AmdTtlServices::initialize(this=%p, input=%p): [0]=0x%llx [1]=0x%llx [2]=0x%llx [4]=0x%llx [5]=0x%llx [6]=0x%llx",
              this_ptr, input,
              inp ? inp[0] : 0, inp ? inp[1] : 0, inp ? inp[2] : 0,
              inp ? inp[4] : 0, inp ? inp[5] : 0, inp ? inp[6] : 0);

    rbgpu_log("AmdTtlServices state before (this=%p): 0x560=0x%llx 0x568=0x%llx 0x570=0x%llx 0x578=0x%x 0x598=0x%x 0x958=0x%llx",
              this_ptr, f560, f568, f570, f578, f598, f958);

    if (g_sync_570_mode && f570 == 0 && f958 != 0) {
        *reinterpret_cast<uint64_t *>(reinterpret_cast<uintptr_t>(this_ptr) + 0x570) = f958;
        f570 = f958;
        rbgpu_diag_set_num("ttl_sync_570_applied", 1);
        rbgpu_log("AmdTtlServices: -rbgpusync570 active; synced +0x570 from +0x958 (0x%llx)", f958);
    }

    if (g_safe_ttl_mode && f570 == 0) {
        rbgpu_log("AmdTtlServices: -rbgpusafettl active; applying SafeBoot bypass to pass 1265fd3e");
        *reinterpret_cast<uint32_t *>(reinterpret_cast<uintptr_t>(this_ptr) + 0x578) = 1;
    }

    int ret = 0;
    if (orig_amdtll_init_input) {
        ret = orig_amdtll_init_input(this_ptr, input);
    }

    rbgpu_log("AmdTtlServices::initialize(this=%p, input) returned 0x%08X", this_ptr, ret);
    rbgpu_diag_set_num("ttl_init_inp_ret", static_cast<uint32_t>(ret));
    return ret;
}

static bool patchDeviceTypeTableAt(mach_vm_address_t addr, size_t max_entries = 83) {
    auto table = reinterpret_cast<DeviceTypeEntry *>(addr);

    for (size_t i = 0; i < max_entries; i++) {
        if (table[i].deviceId == 0x1681) {
            rbgpu_diag_set_num("dev_type_patched", 1);
            rbgpu_diag_set_num("dev_type_idx", i);
            rbgpu_diag_set_num("dev_type_rb_id", table[i].deviceId);
            rbgpu_diag_set_num("dev_type_rb_type", table[i].deviceType);
            rbgpu_log("HWLibs: __ZL15deviceTypeTable already contains 0x1681 at index %zu", i);
            return true;
        }
    }

    size_t target_idx = 66;
    if (target_idx < max_entries && table[target_idx].deviceId == 0x005D && table[target_idx].deviceType == 6) {
        uint32_t old_id = table[target_idx].deviceId;

        MachInfo::setKernelWriting(true, KernelPatcher::kernelWriteLock);
        table[target_idx].deviceId   = 0x1681;
        table[target_idx].deviceType = 6;
        MachInfo::setKernelWriting(false, KernelPatcher::kernelWriteLock);

        uint32_t rb_id   = table[target_idx].deviceId;
        uint32_t rb_type = table[target_idx].deviceType;
        rbgpu_diag_set_num("dev_type_patched", 1);
        rbgpu_diag_set_num("dev_type_idx", target_idx);
        rbgpu_diag_set_num("dev_type_rb_id", rb_id);
        rbgpu_diag_set_num("dev_type_rb_type", rb_type);

        if (rb_id != 0x1681) {
            rbgpu_log("HWLibs: [CRITICAL] write verification FAILED at index %zu! readback: 0x%04X != 0x1681", target_idx, rb_id);
            return false;
        }

        rbgpu_log("HWLibs: patched __ZL15deviceTypeTable at index %zu: 0x%04X -> 0x1681 (type 6, verified readback: 0x%04X, type %u)",
                  target_idx, old_id, rb_id, rb_type);
        return true;
    }

    for (size_t i = 0; i < max_entries; i++) {
        if (table[i].deviceType == 6 &&
            table[i].deviceId != 0x73A0 &&
            table[i].deviceId != 0x73A2 &&
            table[i].deviceId != 0x73A3 &&
            table[i].deviceId != 0x73AB &&
            table[i].deviceId != 0x73AE &&
            table[i].deviceId != 0x73BF) {
            uint32_t old_id = table[i].deviceId;

            MachInfo::setKernelWriting(true, KernelPatcher::kernelWriteLock);
            table[i].deviceId   = 0x1681;
            table[i].deviceType = 6;
            MachInfo::setKernelWriting(false, KernelPatcher::kernelWriteLock);

            uint32_t rb_id   = table[i].deviceId;
            uint32_t rb_type = table[i].deviceType;
            rbgpu_diag_set_num("dev_type_patched", 1);
            rbgpu_diag_set_num("dev_type_idx", i);
            rbgpu_diag_set_num("dev_type_rb_id", rb_id);
            rbgpu_diag_set_num("dev_type_rb_type", rb_type);

            if (rb_id != 0x1681) {
                rbgpu_log("HWLibs: [CRITICAL] write verification FAILED at index %zu! readback: 0x%04X != 0x1681", i, rb_id);
                return false;
            }

            rbgpu_log("HWLibs: patched __ZL15deviceTypeTable at active index %zu: 0x%04X -> 0x1681 (type 6, verified readback: 0x%04X, type %u)",
                      i, old_id, rb_id, rb_type);
            return true;
        }
    }

    rbgpu_log("HWLibs: failed to find a replaceable slot in __ZL15deviceTypeTable");
    return false;
}

bool RGPU::patchDeviceTypeTable(KernelPatcher &patcher, size_t id) {
    rbgpu_diag_set_num("dev_type_patched", 0);
    rbgpu_diag_set_num("dev_type_rb_id", 0);
    rbgpu_diag_set_num("dev_type_rb_type", 0);

    mach_vm_address_t tbl_addr = patcher.solveSymbol(id, "__ZL15deviceTypeTable");
    if (!tbl_addr) {
        rbgpu_log("HWLibs: symbol __ZL15deviceTypeTable not resolved; skipping device type patch");
        return false;
    }

    rbgpu_log("HWLibs: resolved __ZL15deviceTypeTable symbol at %p", reinterpret_cast<void *>(tbl_addr));
    return patchDeviceTypeTableAt(tbl_addr);
}

static bool patchDeviceCapabilityTableAt(mach_vm_address_t addr, size_t max_entries = 92) {
    auto table = reinterpret_cast<CapabilityEntry *>(addr);

    for (size_t i = 0; i < max_entries; i++) {
        uint32_t *dev_id_ptr = reinterpret_cast<uint32_t *>(table[i].data + 0x10);
        if (*dev_id_ptr == 0x1681) {
            rbgpu_diag_set_num("cap_tbl_patched", 1);
            rbgpu_diag_set_num("cap_tbl_idx", i);
            rbgpu_log("HWLibs: _DeviceCapabilityTbl already contains 0x1681 at index %zu", i);
            return true;
        }
    }

    size_t template_idx = SIZE_MAX;
    for (size_t i = 0; i < max_entries; i++) {
        uint32_t *dev_id_ptr = reinterpret_cast<uint32_t *>(table[i].data + 0x10);
        if (*dev_id_ptr == 0x73A2) {
            template_idx = i;
            break;
        }
    }

    if (template_idx == SIZE_MAX) {
        rbgpu_log("HWLibs: template device 0x73A2 not found in _DeviceCapabilityTbl; aborting capability patch");
        return false;
    }

    size_t target_idx = SIZE_MAX;
    for (size_t i = 0; i < max_entries; i++) {
        uint32_t *dev_id_ptr = reinterpret_cast<uint32_t *>(table[i].data + 0x10);
        if (*dev_id_ptr == 0x7300) {
            target_idx = i;
            break;
        }
    }

    if (target_idx == SIZE_MAX) {
        rbgpu_log("HWLibs: target device 0x7300 not found in _DeviceCapabilityTbl; aborting capability patch");
        return false;
    }

    MachInfo::setKernelWriting(true, KernelPatcher::kernelWriteLock);
    memcpy(&table[target_idx], &table[template_idx], sizeof(CapabilityEntry));
    *reinterpret_cast<uint32_t *>(table[target_idx].data + 0x10) = 0x1681;
    *reinterpret_cast<uint64_t *>(table[target_idx].data + 0x18) = 0xDEADCAFEULL;
    *reinterpret_cast<uint64_t *>(table[target_idx].data + 0x20) = 0xDEADCAFEULL;
    MachInfo::setKernelWriting(false, KernelPatcher::kernelWriteLock);

    uint32_t rb_id = *reinterpret_cast<uint32_t *>(table[target_idx].data + 0x10);
    uint64_t rb_18 = *reinterpret_cast<uint64_t *>(table[target_idx].data + 0x18);
    uint64_t rb_20 = *reinterpret_cast<uint64_t *>(table[target_idx].data + 0x20);
    rbgpu_log("HWLibs: patched _DeviceCapabilityTbl at index %zu: cloned from index %zu, set deviceId=0x1681, +0x18=0x%llx, +0x20=0x%llx (readback id: 0x%04X)",
              target_idx, template_idx, rb_18, rb_20, rb_id);
    rbgpu_diag_set_num("cap_tbl_patched", rb_id == 0x1681 ? 1 : 0);
    rbgpu_diag_set_num("cap_tbl_idx", target_idx);

    return rb_id == 0x1681;
}

bool RGPU::patchDeviceCapabilityTable(KernelPatcher &patcher, size_t id) {
    rbgpu_diag_set_num("cap_tbl_patched", 0);
    rbgpu_diag_set_num("cap_tbl_idx", 0);

    if (!spoofTtlMode) {
        rbgpu_log("HWLibs: capability table patch skipped (-rbgpuspoofttl not set)");
        return false;
    }

    mach_vm_address_t tbl_addr = patcher.solveSymbol(id, "_DeviceCapabilityTbl");
    if (!tbl_addr) {
        rbgpu_log("HWLibs: symbol _DeviceCapabilityTbl not resolved; skipping capability table patch");
        return false;
    }

    rbgpu_log("HWLibs: resolved _DeviceCapabilityTbl symbol at %p", reinterpret_cast<void *>(tbl_addr));
    return patchDeviceCapabilityTableAt(tbl_addr);
}

void RGPU::installKernelLogHooks(KernelPatcher &patcher) {
    static bool s_kernel_hooks_installed = false;
    if (s_kernel_hooks_installed || noLogCapture) {
        return;
    }

    KernelPatcher::RouteRequest req_kprintf("_kprintf", wrap_kprintf, orig_kprintf);
    if (patcher.routeMultiple(KernelPatcher::KernelID, &req_kprintf, 1)) {
        rbgpu_log("Kernel: routed _kprintf successfully");
    } else {
        rbgpu_log("Kernel: failed to route _kprintf");
    }

    KernelPatcher::RouteRequest req_iolog("_IOLog", wrap_iolog, orig_iolog);
    if (patcher.routeMultiple(KernelPatcher::KernelID, &req_iolog, 1)) {
        rbgpu_log("Kernel: routed _IOLog successfully");
    } else {
        rbgpu_log("Kernel: failed to route _IOLog");
    }

    s_kernel_hooks_installed = true;
}

void RGPU::init() {
    noSmuHook    = checkKernelArgument("-rbgpunosmu");
    noTrap3Guard = checkKernelArgument("-rbgpunoguard");
    noLogCapture = checkKernelArgument("-rbgpunocapture");
    safeTtlMode  = checkKernelArgument("-rbgpusafettl");
    sync570Mode  = checkKernelArgument("-rbgpusync570");
    spoofTtlMode = checkKernelArgument("-rbgpuspoofttl");
    pspHwMode    = checkKernelArgument("-rbgpupsphw");
    g_safe_ttl_mode  = safeTtlMode;
    g_sync_570_mode  = sync570Mode;
    g_spoof_ttl_mode = spoofTtlMode;
    g_psp_hw_mode    = pspHwMode;

    rbgpu_log("loaded (Milestone 2; SMU Hook: %s, Trap 3 Guard: %s, Capture: %s, SafeTTL: %s, Sync570: %s, SpoofTTL: %s, PSPHW: %s)",
              noSmuHook ? "DISABLED" : "ENABLED",
              noTrap3Guard ? "DISABLED" : "ENABLED",
              noLogCapture ? "DISABLED" : "ENABLED",
              safeTtlMode ? "ENABLED" : "DISABLED",
              sync570Mode ? "ENABLED" : "DISABLED",
              spoofTtlMode ? "ENABLED" : "DISABLED",
              pspHwMode ? "ENABLED" : "DISABLED");

    lilu.onKextLoad(kextList, arrsize(kextList), [](void *user, KernelPatcher &patcher, size_t id, mach_vm_address_t address, size_t size) {
        static_cast<RGPU *>(user)->processKext(patcher, id, address, size);
    }, this);
}

void RGPU::deinit() {
}

void RGPU::processKext(KernelPatcher &patcher, size_t id, mach_vm_address_t address, size_t size) {
    installKernelLogHooks(patcher);

    if (kextList[KextHWLibs].loadIndex == id) {
        g_kext_addr_hwlibs = address;
        g_kext_size_hwlibs = size;
        rbgpu_log("HWLibs loaded at %p (size 0x%zx)", reinterpret_cast<void *>(address), size);
        processHWLibs(patcher, id, address, size);
    } else if (kextList[KextX6000].loadIndex == id) {
        g_kext_addr_x6000 = address;
        g_kext_size_x6000 = size;
        rbgpu_log("X6000 loaded at %p (size 0x%zx)", reinterpret_cast<void *>(address), size);
        processX6000(patcher, id, address, size);
    } else if (kextList[KextHWSvc].loadIndex == id) {
        g_kext_addr_hwsvc = address;
        g_kext_size_hwsvc = size;
        rbgpu_log("HWSvc loaded at %p (size 0x%zx)", reinterpret_cast<void *>(address), size);
        processHWServices(patcher, id, address, size);
    }
}

void RGPU::routeTTL(KernelPatcher &patcher, size_t id, mach_vm_address_t address, size_t size) {
    KernelPatcher::RouteRequest reqs[] = {
        KernelPatcher::RouteRequest(
            "__ZN14AmdTtlServices10initializeEP30_TtlLibraryInitializationInput",
            hooked_AmdTtlServices_initialize_input,
            orig_amdtll_init_input
        ),
        KernelPatcher::RouteRequest(
            "__ZN14AmdTtlServices10initializeEv",
            hooked_AmdTtlServices_initialize,
            orig_amdtll_initialize
        ),
        KernelPatcher::RouteRequest(
            "__ZN14AmdTtlServices20setFirmwareDirectoryEP20AMDFirmwareDirectory",
            hooked_AmdTtlServices_setFirmwareDirectory,
            orig_amdtll_set_fw_dir
        ),
        KernelPatcher::RouteRequest(
            "_ttl_initialize",
            hooked_ttl_initialize,
            orig_ttl_initialize
        ),
        KernelPatcher::RouteRequest(
            "_ttlIsSupportedDevice",
            hooked_ttlIsSupportedDevice,
            orig_ttl_is_supported_device
        ),
        KernelPatcher::RouteRequest(
            "_ttl_hw_init",
            hooked_ttl_hw_init,
            orig_ttl_hw_init
        ),
        KernelPatcher::RouteRequest(
            "_IpiExecuteSwipEntryPoint",
            hooked_ipi_execute_swip_entry_point,
            orig_ipi_execute_swip_entry_point
        ),
        KernelPatcher::RouteRequest(
            "_IpiGetDiscoveryInfo",
            hooked_IpiGetDiscoveryInfo,
            orig_ipi_get_discovery_info
        )
    };

    for (size_t i = 0; i < arrsize(reqs); i++) {
        if (patcher.routeMultiple(id, &reqs[i], 1, address, size)) {
            rbgpu_log("HWLibs: hooked %s", reqs[i].symbol);
        } else {
            rbgpu_log("HWLibs: failed to hook %s", reqs[i].symbol);
        }
    }
}

void RGPU::routePSPBasics(KernelPatcher &patcher, size_t id, mach_vm_address_t address, size_t size) {
    KernelPatcher::RouteRequest reqs[] = {
        KernelPatcher::RouteRequest(
            "_ipi_psp_sw_init",
            hooked_ipi_psp_sw_init,
            orig_ipi_psp_sw_init
        ),
        KernelPatcher::RouteRequest(
            "_ipi_psp_hw_init",
            hooked_ipi_psp_hw_init,
            orig_ipi_psp_hw_init
        ),
        KernelPatcher::RouteRequest(
            "_psp_sw_init",
            hooked_psp_sw_init,
            orig_psp_sw_init
        ),
        KernelPatcher::RouteRequest(
            "_psp_hw_init",
            hooked_psp_hw_init,
            orig_psp_hw_init
        ),
        KernelPatcher::RouteRequest(
            "_psp_cos_alloc_memory",
            hooked_psp_cos_alloc_memory,
            orig_psp_cos_alloc_memory
        ),
        KernelPatcher::RouteRequest(
            "_psp_asic_type_init",
            hooked_psp_asic_type_init,
            orig_psp_asic_type_init
        ),
        KernelPatcher::RouteRequest(
            "_psp_init_pfn_ptr",
            hooked_psp_init_pfn_ptr,
            orig_psp_init_pfn_ptr
        ),
        KernelPatcher::RouteRequest(
            "_psp_read_config",
            hooked_psp_read_config,
            orig_psp_read_config
        )
    };

    for (size_t i = 0; i < arrsize(reqs); i++) {
        if (patcher.routeMultiple(id, &reqs[i], 1, address, size)) {
            rbgpu_log("HWLibs: hooked %s", reqs[i].symbol);
        } else {
            rbgpu_log("HWLibs: failed to hook %s", reqs[i].symbol);
        }
    }
}

void RGPU::routePSPRingTmr(KernelPatcher &patcher, size_t id, mach_vm_address_t address, size_t size) {
    KernelPatcher::RouteRequest reqs[] = {
        KernelPatcher::RouteRequest(
            "_psp_allocate_private_mem",
            hooked_psp_allocate_private_mem,
            orig_psp_allocate_private_mem
        ),
        KernelPatcher::RouteRequest(
            "_psp_ring_init",
            hooked_psp_ring_init,
            orig_psp_ring_init
        ),
        KernelPatcher::RouteRequest(
            "_psp_ring_create",
            hooked_psp_ring_create,
            orig_psp_ring_create
        ),
        KernelPatcher::RouteRequest(
            "_psp_bootloader_is_sos_running",
            hooked_psp_bootloader_is_sos_running,
            orig_psp_bootloader_is_sos_running
        ),
        KernelPatcher::RouteRequest(
            "_psp_cmd_km_init",
            hooked_psp_cmd_km_init,
            orig_psp_cmd_km_init
        ),
        KernelPatcher::RouteRequest(
            "_psp_tmr_init",
            hooked_psp_tmr_init,
            orig_psp_tmr_init
        ),
        KernelPatcher::RouteRequest(
            "_psp_tmr_load",
            hooked_psp_tmr_load,
            orig_psp_tmr_load
        ),
        KernelPatcher::RouteRequest(
            "_psp_dtm_init",
            hooked_psp_dtm_init,
            orig_psp_dtm_init
        ),
        KernelPatcher::RouteRequest(
            "_psp_np_fw_load",
            hooked_psp_np_fw_load,
            orig_psp_np_fw_load
        )
    };

    for (size_t i = 0; i < arrsize(reqs); i++) {
        if (patcher.routeMultiple(id, &reqs[i], 1, address, size)) {
            rbgpu_log("HWLibs: hooked %s", reqs[i].symbol);
        } else {
            rbgpu_log("HWLibs: failed to hook %s", reqs[i].symbol);
        }
    }
}

void RGPU::routeGC(KernelPatcher &patcher, size_t id, mach_vm_address_t address, size_t size) {
    KernelPatcher::RouteRequest reqs[] = {
        KernelPatcher::RouteRequest(
            "_ipi_gc_sw_init",
            hooked_ipi_gc_sw_init,
            orig_ipi_gc_sw_init
        ),
        KernelPatcher::RouteRequest(
            "_gc_sw_init",
            hooked_gc_sw_init,
            orig_gc_sw_init
        ),
        KernelPatcher::RouteRequest(
            "_gc_get_hw_version",
            hooked_gc_get_hw_version,
            orig_gc_get_hw_version
        ),
        KernelPatcher::RouteRequest(
            "_gc_get_config",
            hooked_gc_get_config,
            orig_gc_get_config
        ),
        KernelPatcher::RouteRequest(
            "_gc_get_hw_config_tbl_info",
            hooked_gc_get_hw_config_tbl_info,
            orig_gc_get_hw_config_tbl_info
        ),
        KernelPatcher::RouteRequest(
            "_gc_set_fw_entry_info",
            hooked_gc_set_fw_entry_info,
            orig_gc_set_fw_entry_info
        ),
        KernelPatcher::RouteRequest(
            "_gc_init_fcn_ptr_list",
            hooked_gc_init_fcn_ptr_list,
            orig_gc_init_fcn_ptr_list
        ),
        KernelPatcher::RouteRequest(
            "_gc_get_doorbell_requirements",
            hooked_gc_get_doorbell_requirements,
            orig_gc_get_doorbell_requirements
        ),
        KernelPatcher::RouteRequest(
            "_ttlDevGetDoorbellRange",
            hooked_ttlDevGetDoorbellRange,
            orig_ttl_dev_get_doorbell_range
        ),
        KernelPatcher::RouteRequest(
            "_IpiSetFwEntry",
            hooked_IpiSetFwEntry,
            orig_ipi_set_fw_entry
        ),
        KernelPatcher::RouteRequest(
            "_ipi_gc_hw_init",
            hooked_ipi_gc_hw_init,
            orig_ipi_gc_hw_init
        ),
        KernelPatcher::RouteRequest(
            "_gc_hw_init",
            hooked_gc_hw_init,
            orig_gc_hw_init
        )
    };

    for (size_t i = 0; i < arrsize(reqs); i++) {
        if (patcher.routeMultiple(id, &reqs[i], 1, address, size)) {
            rbgpu_log("HWLibs: hooked %s", reqs[i].symbol);
        } else {
            rbgpu_log("HWLibs: failed to hook %s", reqs[i].symbol);
        }
    }
}

void RGPU::processHWLibs(KernelPatcher &patcher, size_t id, mach_vm_address_t address, size_t size) {
    rbgpu_log("Patching com.apple.kext.AMDRadeonX6800HWLibs");

    patchDeviceTypeTable(patcher, id);
    patchDeviceCapabilityTable(patcher, id);

    routeTTL(patcher, id, address, size);
    routePSPBasics(patcher, id, address, size);
    routePSPRingTmr(patcher, id, address, size);
    routeGC(patcher, id, address, size);

    if (!noSmuHook) {
        orig_smu_cgs_alloc_memory = reinterpret_cast<smu_cgs_alloc_memory_t>(patcher.solveSymbol(id, "_smu_cgs_alloc_memory"));
        if (!orig_smu_cgs_alloc_memory) {
            rbgpu_log("HWLibs: failed to resolve symbol _smu_cgs_alloc_memory");
        }

        orig_smu_cgs_free_memory = reinterpret_cast<smu_cgs_free_memory_t>(patcher.solveSymbol(id, "_smu_cgs_free_memory"));
        if (!orig_smu_cgs_free_memory) {
            rbgpu_log("HWLibs: failed to resolve symbol _smu_cgs_free_memory (expected)");
        }

        orig_smu_get_hw_version = reinterpret_cast<smu_get_hw_version_t>(patcher.solveSymbol(id, "_smu_get_hw_version"));
        if (!orig_smu_get_hw_version) {
            rbgpu_log("HWLibs: failed to resolve symbol _smu_get_hw_version");
        }

        orig_smu_read_config_space = reinterpret_cast<smu_read_config_space_t>(patcher.solveSymbol(id, "_smu_read_config_space"));
        if (!orig_smu_read_config_space) {
            rbgpu_log("HWLibs: failed to resolve symbol _smu_read_config_space");
        }

        mach_vm_address_t smu_init_sym = patcher.solveSymbol(id, "_smu_init_function_pointer_list");
        if (!smu_init_sym) {
            rbgpu_log("HWLibs: failed to resolve symbol _smu_init_function_pointer_list");
        }

        KernelPatcher::RouteRequest requests[] = {
            KernelPatcher::RouteRequest("_smu_init_function_pointer_list", hooked_smu_init_function_pointer_list, orig_smu_init_function_pointer_list)
        };

        if (patcher.routeMultiple(id, requests, address, size)) {
            rbgpu_diag_set_num("smu_hook_routed", 1);
            rbgpu_log("H-SMU-1: smu_init_function_pointer_list hooked successfully");
        } else {
            rbgpu_diag_set_num("smu_hook_routed", 0);
            rbgpu_log("H-SMU-1: failed to hook smu_init_function_pointer_list");
        }
    }
}

void RGPU::processX6000(KernelPatcher &patcher, size_t id, mach_vm_address_t address, size_t size) {
    rbgpu_log("Patching com.apple.kext.AMDRadeonX6000 (Trap 3 Failure Guard)");

    if (noTrap3Guard) {
        return;
    }

    mach_vm_address_t start_sym = patcher.solveSymbol(id, "__ZN37AMDRadeonX6000_AMDGraphicsAccelerator5startEP9IOService");
    if (!start_sym) {
        rbgpu_log("X6000: failed to resolve symbol __ZN37AMDRadeonX6000_AMDGraphicsAccelerator5startEP9IOService");
    }

    mach_vm_address_t stop_sym  = patcher.solveSymbol(id, "__ZN37AMDRadeonX6000_AMDGraphicsAccelerator4stopEP9IOService");
    if (!stop_sym) {
        rbgpu_log("X6000: failed to resolve symbol __ZN37AMDRadeonX6000_AMDGraphicsAccelerator4stopEP9IOService");
    }

    if (!start_sym || !stop_sym) {
        rbgpu_log("Trap 3 Guard: required start/stop symbols not resolved");
        rbgpu_diag_set_num("guard_routed", 0);
        return;
    }

    auto start_bytes = reinterpret_cast<uint8_t *>(start_sym);
    bool patched = false;

    for (size_t i = 0; i < 0x2000; i++) {
        if (start_bytes[i] == 0xE8) {
            int32_t rel = *reinterpret_cast<int32_t *>(&start_bytes[i + 1]);
            mach_vm_address_t call_dest = reinterpret_cast<mach_vm_address_t>(&start_bytes[i + 5]) + rel;
            if (call_dest == stop_sym) {
                uint8_t *post_stop = &start_bytes[i + 5];
                rbgpu_log("Trap 3 Guard: located call stop() at %p (offset +0x%zx)", &start_bytes[i], i);

                uint8_t *pop_seq = nullptr;
                for (size_t j = 0; j < 64; j++) {
                    if (post_stop[j] == 0xC3) {
                        size_t k = j;
                        while (k > 0 && ((post_stop[k - 1] >= 0x58 && post_stop[k - 1] <= 0x5F) ||
                                         post_stop[k - 1] == 0x41)) {
                            k--;
                        }
                        pop_seq = &post_stop[k];
                        break;
                    }
                }

                if (pop_seq && (pop_seq - post_stop) >= 17) {
                    MachInfo::setKernelWriting(true, KernelPatcher::kernelWriteLock);

                    post_stop[0] = 0x48;
                    post_stop[1] = 0xB8;
                    *reinterpret_cast<uint64_t *>(&post_stop[2]) = reinterpret_cast<uint64_t>(&g_guard_calls);

                    post_stop[10] = 0x48;
                    post_stop[11] = 0xFF;
                    post_stop[12] = 0x00;

                    post_stop[13] = 0x31;
                    post_stop[14] = 0xC0;

                    int32_t jmp_offset = static_cast<int32_t>(pop_seq - (post_stop + 17));
                    if (jmp_offset >= -128 && jmp_offset <= 127) {
                        post_stop[15] = 0xEB;
                        post_stop[16] = static_cast<uint8_t>(jmp_offset);
                        memset(&post_stop[17], 0x90, pop_seq - &post_stop[17]);
                    } else {
                        post_stop[15] = 0xE9;
                        int32_t rel32 = static_cast<int32_t>(pop_seq - (post_stop + 20));
                        *reinterpret_cast<int32_t *>(&post_stop[16]) = rel32;
                        memset(&post_stop[20], 0x90, pop_seq - &post_stop[20]);
                    }

                    MachInfo::setKernelWriting(false, KernelPatcher::kernelWriteLock);
                    rbgpu_log("Trap 3 Guard: successfully patched start() failure path (jumped to %p)", pop_seq);
                    rbgpu_diag_set_num("guard_routed", 1);
                    patched = true;
                    break;
                }
            }
        }
    }

    if (!patched) {
        rbgpu_diag_set_num("guard_routed", 0);
        rbgpu_log("Trap 3 Guard: failed to locate or patch stop() call site in start()");
    }
}

void RGPU::processHWServices(KernelPatcher &patcher, size_t id, mach_vm_address_t address, size_t size) {
    rbgpu_log("com.apple.kext.AMDRadeonX6000HWServices matched and monitored");
}

```


===== FILE: src/plugin/RembrandtGPUPeer.hpp =====

```cpp
#ifndef REMBRANDTGPU_PEER_HPP
#define REMBRANDTGPU_PEER_HPP

#include <IOKit/IOService.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <kern/thread_call.h>
#include <stdarg.h>

void rbgpu_log(const char *fmt, ...) __printflike(1, 2);
void rbgpu_log_captured_va(const char *tag, const char *fmt, va_list va, void (*orig_fn)(const char *, ...));
void rbgpu_diag_set_num(const char *key, uint64_t val);
void rbgpu_diag_set_str(const char *key, const char *val);
void rbgpu_diag_inc(const char *key);

class RembrandtGPUPeer : public IOService {
    OSDeclareDefaultStructors(RembrandtGPUPeer)

private:
    IOPCIDevice     *pciDevice{nullptr};
    IOService       *accelerator{nullptr};
    const OSSymbol  *symSpecialAMDKey{nullptr};
    thread_call_t    postRegisterCall{nullptr};
    thread_call_t    refreshDiagCall{nullptr};

    static void postRegisterAction(thread_call_param_t param0, thread_call_param_t param1);
    static void refreshDiagAction(thread_call_param_t param0, thread_call_param_t param1);

    void handlePostRegister();
    void performRefreshDiag();

public:
    bool init(OSDictionary *dictionary = nullptr) override;
    void free() override;
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;

    IOReturn callPlatformFunction(const OSSymbol *functionName,
                                  bool waitForFunction,
                                  void *p1, void *p2,
                                  void *p3, void *p4) override;

    void scheduleRefreshDiag();
};

#endif // REMBRANDTGPU_PEER_HPP

```


===== FILE: src/plugin/RembrandtGPUPeer.cpp =====

```cpp
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <Headers/kern_util.hpp>
#include <mach/mach_time.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSBoolean.h>

#include "RembrandtGPUPeer.hpp"
#include "vbios_data.h"

#define super IOService

OSDefineMetaClassAndStructors(RembrandtGPUPeer, IOService)

#define RBGPU_RING_SIZE 256
#define RBGPU_LINE_MAX  384
#define RBGPU_MAX_DIAGS 96
#define RBGPU_BUF_SIZE  4096

struct LogEntry {
    uint64_t time;
    uint32_t count;
    char     line[RBGPU_LINE_MAX];
};

enum DiagType {
    DIAG_TYPE_NUM,
    DIAG_TYPE_STR
};

struct DiagEntry {
    char     key[32];
    DiagType type;
    uint64_t numVal;
    char     strVal[64];
};

static IOLock           *g_diagLock = nullptr;
static RembrandtGPUPeer *g_peer     = nullptr;

static LogEntry  g_logRing[RBGPU_RING_SIZE];
static size_t    g_logHead  = 0;
static size_t    g_logCount = 0;

static DiagEntry g_diags[RBGPU_MAX_DIAGS];
static size_t    g_diagCount = 0;

static LogEntry  s_snapLogs[RBGPU_RING_SIZE];
static DiagEntry s_snapDiags[RBGPU_MAX_DIAGS];
static size_t    s_snapHead      = 0;
static size_t    s_snapLogCount  = 0;
static size_t    s_snapDiagCount = 0;

static char      s_log_buf[RBGPU_BUF_SIZE];
static char      s_capture_buf[RBGPU_BUF_SIZE];
static char      s_line_scratch[RBGPU_LINE_MAX];

extern uint64_t g_guard_calls;

static void initDiagLock() {
    if (!g_diagLock) {
        IOLock *lock = IOLockAlloc();
        if (!OSCompareAndSwapPtr(nullptr, lock, reinterpret_cast<void * volatile *>(&g_diagLock))) {
            IOLockFree(lock);
        }
    }
}

static void rbgpu_diag_set_num_locked(const char *key, uint64_t val) {
    for (size_t i = 0; i < g_diagCount; i++) {
        if (strncmp(g_diags[i].key, key, sizeof(g_diags[i].key)) == 0) {
            g_diags[i].type   = DIAG_TYPE_NUM;
            g_diags[i].numVal = val;
            return;
        }
    }
    if (g_diagCount < RBGPU_MAX_DIAGS) {
        strncpy(g_diags[g_diagCount].key, key, sizeof(g_diags[g_diagCount].key) - 1);
        g_diags[g_diagCount].key[sizeof(g_diags[g_diagCount].key) - 1] = '\0';
        g_diags[g_diagCount].type   = DIAG_TYPE_NUM;
        g_diags[g_diagCount].numVal = val;
        g_diagCount++;
    }
}

static void rbgpu_log_append_locked(const char *line) {
    uint64_t now = mach_absolute_time();

    if (g_logCount > 0) {
        size_t lastIdx = (g_logHead - 1 + RBGPU_RING_SIZE) % RBGPU_RING_SIZE;
        if (strncmp(g_logRing[lastIdx].line, line, sizeof(g_logRing[lastIdx].line)) == 0) {
            g_logRing[lastIdx].count++;
            g_logRing[lastIdx].time = now;
            return;
        }
    }

    g_logRing[g_logHead].time  = now;
    g_logRing[g_logHead].count = 1;
    strncpy(g_logRing[g_logHead].line, line, sizeof(g_logRing[g_logHead].line) - 1);
    g_logRing[g_logHead].line[sizeof(g_logRing[g_logHead].line) - 1] = '\0';
    g_logHead = (g_logHead + 1) % RBGPU_RING_SIZE;
    if (g_logCount < RBGPU_RING_SIZE) {
        g_logCount++;
    }
}

static void rbgpu_process_multiline_locked(const char *tag, char *buffer) {
    char *cursor = buffer;
    while (*cursor) {
        char *eol = strchr(cursor, '\n');
        if (eol) {
            *eol = '\0';
        }

        size_t len = strnlen(cursor, RBGPU_LINE_MAX);
        while (len > 0 && (cursor[len - 1] == '\r' || cursor[len - 1] == ' ')) {
            cursor[len - 1] = '\0';
            len--;
        }

        if (len > 0) {
            if (tag && tag[0] != '\0') {
                snprintf(s_line_scratch, sizeof(s_line_scratch), "%s %s", tag, cursor);
                rbgpu_log_append_locked(s_line_scratch);
            } else {
                rbgpu_log_append_locked(cursor);
            }
        }

        if (!eol) {
            break;
        }
        cursor = eol + 1;
    }
}

void rbgpu_log(const char *fmt, ...) {
    initDiagLock();
    if (g_diagLock) {
        IOLockLock(g_diagLock);
        va_list args;
        va_start(args, fmt);
        vsnprintf(s_log_buf, sizeof(s_log_buf), fmt, args);
        va_end(args);

        IOLog("RembrandtGPU: %s\n", s_log_buf);
        rbgpu_process_multiline_locked(nullptr, s_log_buf);
        IOLockUnlock(g_diagLock);
    }

    if (g_peer) {
        g_peer->scheduleRefreshDiag();
    }
}

void rbgpu_log_captured_va(const char *tag, const char *fmt, va_list va, void (*orig_fn)(const char *, ...)) {
    initDiagLock();
    if (g_diagLock) {
        IOLockLock(g_diagLock);
        vsnprintf(s_capture_buf, sizeof(s_capture_buf), fmt, va);
        rbgpu_process_multiline_locked(tag, s_capture_buf);
        IOLockUnlock(g_diagLock);
    }

    if (orig_fn) {
        orig_fn("%s\n", s_capture_buf);
    }

    if (g_peer) {
        g_peer->scheduleRefreshDiag();
    }
}

void rbgpu_diag_set_num(const char *key, uint64_t val) {
    initDiagLock();
    if (g_diagLock) {
        IOLockLock(g_diagLock);
        rbgpu_diag_set_num_locked(key, val);
        IOLockUnlock(g_diagLock);
    }
    if (g_peer) {
        g_peer->scheduleRefreshDiag();
    }
}

void rbgpu_diag_set_str(const char *key, const char *val) {
    initDiagLock();
    if (g_diagLock) {
        IOLockLock(g_diagLock);
        for (size_t i = 0; i < g_diagCount; i++) {
            if (strncmp(g_diags[i].key, key, sizeof(g_diags[i].key)) == 0) {
                g_diags[i].type = DIAG_TYPE_STR;
                strncpy(g_diags[i].strVal, val, sizeof(g_diags[i].strVal) - 1);
                g_diags[i].strVal[sizeof(g_diags[i].strVal) - 1] = '\0';
                IOLockUnlock(g_diagLock);
                if (g_peer) g_peer->scheduleRefreshDiag();
                return;
            }
        }
        if (g_diagCount < RBGPU_MAX_DIAGS) {
            strncpy(g_diags[g_diagCount].key, key, sizeof(g_diags[g_diagCount].key) - 1);
            g_diags[g_diagCount].key[sizeof(g_diags[g_diagCount].key) - 1] = '\0';
            g_diags[g_diagCount].type = DIAG_TYPE_STR;
            strncpy(g_diags[g_diagCount].strVal, val, sizeof(g_diags[g_diagCount].strVal) - 1);
            g_diags[g_diagCount].strVal[sizeof(g_diags[g_diagCount].strVal) - 1] = '\0';
            g_diagCount++;
        }
        IOLockUnlock(g_diagLock);
    }
    if (g_peer) {
        g_peer->scheduleRefreshDiag();
    }
}

void rbgpu_diag_inc(const char *key) {
    initDiagLock();
    if (g_diagLock) {
        IOLockLock(g_diagLock);
        for (size_t i = 0; i < g_diagCount; i++) {
            if (strncmp(g_diags[i].key, key, sizeof(g_diags[i].key)) == 0) {
                if (g_diags[i].type == DIAG_TYPE_NUM) {
                    g_diags[i].numVal++;
                } else {
                    g_diags[i].type   = DIAG_TYPE_NUM;
                    g_diags[i].numVal = 1;
                }
                IOLockUnlock(g_diagLock);
                if (g_peer) g_peer->scheduleRefreshDiag();
                return;
            }
        }
        if (g_diagCount < RBGPU_MAX_DIAGS) {
            strncpy(g_diags[g_diagCount].key, key, sizeof(g_diags[g_diagCount].key) - 1);
            g_diags[g_diagCount].key[sizeof(g_diags[g_diagCount].key) - 1] = '\0';
            g_diags[g_diagCount].type   = DIAG_TYPE_NUM;
            g_diags[g_diagCount].numVal = 1;
            g_diagCount++;
        }
        IOLockUnlock(g_diagLock);
    }
    if (g_peer) {
        g_peer->scheduleRefreshDiag();
    }
}

bool RembrandtGPUPeer::init(OSDictionary *dictionary) {
    if (!super::init(dictionary)) {
        return false;
    }

    symSpecialAMDKey = OSSymbol::withCString("SpecialAMDKey");
    if (!symSpecialAMDKey) {
        return false;
    }

    postRegisterCall = thread_call_allocate(postRegisterAction, this);
    refreshDiagCall  = thread_call_allocate(refreshDiagAction, this);

    return postRegisterCall != nullptr && refreshDiagCall != nullptr;
}

void RembrandtGPUPeer::free() {
    if (g_peer == this) {
        g_peer = nullptr;
    }
    if (postRegisterCall) {
        thread_call_cancel(postRegisterCall);
        thread_call_free(postRegisterCall);
        postRegisterCall = nullptr;
    }
    if (refreshDiagCall) {
        thread_call_cancel(refreshDiagCall);
        thread_call_free(refreshDiagCall);
        refreshDiagCall = nullptr;
    }
    OSSafeReleaseNULL(symSpecialAMDKey);
    OSSafeReleaseNULL(accelerator);
    super::free();
}

bool RembrandtGPUPeer::start(IOService *provider) {
    if (checkKernelArgument("-rbgpuoff")) {
        rbgpu_log("Peer: disabled by -rbgpuoff; aborting start");
        return false;
    }

    if (!super::start(provider)) {
        return false;
    }

    pciDevice = OSDynamicCast(IOPCIDevice, provider);
    if (!pciDevice) {
        rbgpu_log("Peer: provider is not an IOPCIDevice");
        return false;
    }

    g_peer = this;

    auto name = OSString::withCString("ATY,Belknap");
    if (name) {
        pciDevice->setProperty("@0,name", name);
        name->release();
    }

    auto vbios = OSData::withBytes(vbios_data, vbios_data_size);
    if (vbios) {
        pciDevice->setProperty("ATY,bin_image", vbios);
        vbios->release();
    }

    pciDevice->setProperty("RBGPULoad", kOSBooleanTrue);
    pciDevice->setProperty("LoadHWServices", kOSBooleanTrue);
    pciDevice->setProperty("LoadAccelerator", kOSBooleanTrue);

    registerService();
    pciDevice->registerService();

    rbgpu_log("Peer: attached to 0x16811002; properties set and service registered");
    scheduleRefreshDiag();
    return true;
}

void RembrandtGPUPeer::stop(IOService *provider) {
    if (g_peer == this) {
        g_peer = nullptr;
    }
    if (postRegisterCall) {
        thread_call_cancel(postRegisterCall);
    }
    if (refreshDiagCall) {
        thread_call_cancel(refreshDiagCall);
    }
    if (accelerator) {
        accelerator->release();
        accelerator = nullptr;
    }
    super::stop(provider);
}

void RembrandtGPUPeer::scheduleRefreshDiag() {
    if (refreshDiagCall) {
        thread_call_enter(refreshDiagCall);
    }
}

void RembrandtGPUPeer::refreshDiagAction(thread_call_param_t param0, thread_call_param_t param1) {
    auto self = static_cast<RembrandtGPUPeer *>(param0);
    if (self) {
        self->performRefreshDiag();
    }
}

void RembrandtGPUPeer::performRefreshDiag() {
    if (!g_diagLock) {
        return;
    }

    IOLockLock(g_diagLock);
    rbgpu_diag_set_num_locked("guard_calls", g_guard_calls);

    s_snapHead      = g_logHead;
    s_snapLogCount  = g_logCount;
    memcpy(s_snapLogs, g_logRing, sizeof(s_snapLogs));

    s_snapDiagCount = g_diagCount;
    memcpy(s_snapDiags, g_diags, sizeof(s_snapDiags));
    IOLockUnlock(g_diagLock);

    OSArray *logArray = OSArray::withCapacity(static_cast<unsigned int>(s_snapLogCount));
    if (logArray) {
        char lineBuf[RBGPU_LINE_MAX + 32];
        for (size_t i = 0; i < s_snapLogCount; i++) {
            size_t idx = (s_snapHead - s_snapLogCount + i + RBGPU_RING_SIZE) % RBGPU_RING_SIZE;
            if (s_snapLogs[idx].count > 1) {
                snprintf(lineBuf, sizeof(lineBuf), "[%llu] (x%u) %s",
                         s_snapLogs[idx].time, s_snapLogs[idx].count, s_snapLogs[idx].line);
            } else {
                snprintf(lineBuf, sizeof(lineBuf), "[%llu] %s",
                         s_snapLogs[idx].time, s_snapLogs[idx].line);
            }
            OSString *osLine = OSString::withCString(lineBuf);
            if (osLine) {
                logArray->setObject(osLine);
                osLine->release();
            }
        }
        setProperty("RBGPULog", logArray);
        logArray->release();
    }

    OSDictionary *diagDict = OSDictionary::withCapacity(static_cast<unsigned int>(s_snapDiagCount));
    if (diagDict) {
        for (size_t i = 0; i < s_snapDiagCount; i++) {
            if (s_snapDiags[i].type == DIAG_TYPE_NUM) {
                OSNumber *num = OSNumber::withNumber(s_snapDiags[i].numVal, 64);
                if (num) {
                    diagDict->setObject(s_snapDiags[i].key, num);
                    num->release();
                }
            } else {
                OSString *str = OSString::withCString(s_snapDiags[i].strVal);
                if (str) {
                    diagDict->setObject(s_snapDiags[i].key, str);
                    str->release();
                }
            }
        }
        setProperty("RBGPUDiag", diagDict);
        diagDict->release();
    }
}

void RembrandtGPUPeer::postRegisterAction(thread_call_param_t param0, thread_call_param_t param1) {
    auto self = static_cast<RembrandtGPUPeer *>(param0);
    if (self) {
        self->handlePostRegister();
    }
}

void RembrandtGPUPeer::handlePostRegister() {
    if (!accelerator || !symSpecialAMDKey) {
        rbgpu_log("handlePostRegister: missing accelerator or symbol reference");
        return;
    }

    uint32_t one = 1;

    rbgpu_log("handlePostRegister: sending Request 0x06 (PowerPlay Ready)");
    uint32_t reqPowerPlayReady = 0x06;
    IOReturn ret06 = accelerator->callPlatformFunction(symSpecialAMDKey, true, &reqPowerPlayReady, &one, nullptr, nullptr);
    rbgpu_log("handlePostRegister: Request 0x06 returned 0x%08X", ret06);
    rbgpu_diag_set_num("ret_0x06", static_cast<uint64_t>(ret06));

    rbgpu_log("handlePostRegister: sending Request 0x1B (Controller Enabled)");
    uint32_t reqControllerEnabled = 0x1B;
    IOReturn ret1B = accelerator->callPlatformFunction(symSpecialAMDKey, true, &reqControllerEnabled, &one, nullptr, nullptr);
    rbgpu_log("handlePostRegister: Request 0x1B returned 0x%08X", ret1B);
    rbgpu_diag_set_num("ret_0x1B", static_cast<uint64_t>(ret1B));
}

IOReturn RembrandtGPUPeer::callPlatformFunction(const OSSymbol *functionName,
                                                bool waitForFunction,
                                                void *p1, void *p2,
                                                void *p3, void *p4) {
    if (functionName && symSpecialAMDKey && functionName->isEqualTo(symSpecialAMDKey)) {
        if (!p1) {
            rbgpu_log("SpecialAMDKey received NULL p1");
            return kIOReturnBadArgument;
        }

        uint32_t req = *reinterpret_cast<uint32_t *>(p1);
        char reqKey[16];
        snprintf(reqKey, sizeof(reqKey), "req_0x%02X", req);
        rbgpu_diag_inc(reqKey);

        rbgpu_log("SpecialAMDKey request 0x%02X (wait: %d)", req, waitForFunction);

        switch (req) {
            case 0x00:
                if (p2) {
                    OSSafeReleaseNULL(accelerator);
                    accelerator = OSDynamicCast(IOService, reinterpret_cast<OSObject *>(p2));
                    if (accelerator) {
                        accelerator->retain();
                        rbgpu_log("Registered accelerator instance: %s", accelerator->getName());
                        if (postRegisterCall) {
                            thread_call_enter(postRegisterCall);
                        }
                    }
                }
                return kIOReturnSuccess;

            case 0x0B:
                if (accelerator) {
                    rbgpu_log("Dropping registered accelerator: %s", accelerator->getName());
                    accelerator->release();
                    accelerator = nullptr;
                }
                return kIOReturnSuccess;

            case 0x14:
                if (p2) {
                    *reinterpret_cast<uint32_t *>(p2) = 1;
                    return kIOReturnSuccess;
                }
                return kIOReturnBadArgument;

            case 0x17:
                rbgpu_log("ASIC Reset State requested (unsupported)");
                return kIOReturnUnsupported;

            case 0x1A:
                rbgpu_log("Get DCC Capabilities requested (unsupported)");
                return kIOReturnUnsupported;

            default:
                rbgpu_log("Unhandled SpecialAMDKey request 0x%02X", req);
                return kIOReturnUnsupported;
        }
    }

    return super::callPlatformFunction(functionName, waitForFunction, p1, p2, p3, p4);
}

```


===== FILE: src/plugin/kmod_info.c =====

```cpp
/* SPDX-License-Identifier: MIT */
/* Module descriptor, as Xcode generates it from MODULE_NAME / MODULE_START /
 * MODULE_STOP: libkmod's _start/_stop call _realmain/_antimain, which are
 * Lilu's plugin entry points from plugin_start.cpp. */
#include <mach/mach_types.h>
#include <mach/kmod.h>

extern kern_return_t _start(kmod_info_t *ki, void *data);
extern kern_return_t _stop(kmod_info_t *ki, void *data);
extern kern_return_t RembrandtGPU_kern_start(kmod_info_t *ki, void *data);
extern kern_return_t RembrandtGPU_kern_stop(kmod_info_t *ki, void *data);

KMOD_EXPLICIT_DECL(com.rembrandtgpu.plugin, PLUGIN_VERSION, _start, _stop)
__private_extern__ kmod_start_func_t *_realmain = RembrandtGPU_kern_start;
__private_extern__ kmod_stop_func_t *_antimain = RembrandtGPU_kern_stop;
__private_extern__ int _kext_apple_cc = __APPLE_CC__;
```


===== FILE: src/common/rb_discovery.h =====

```cpp
/* SPDX-License-Identifier: MIT */
/*
 * AMD IP discovery table parser.
 *
 * The table is a binary the firmware leaves at the top of the GPU's memory
 * carve-out; it lists every IP block on the chip (GC, MP0, DCN, ...) with its
 * version and the MMIO base addresses its registers sit at. Layout and checks
 * follow Linux: docs/linux-ref/discovery.h and amdgpu_discovery.c
 * (amdgpu_discovery_init, _table_check, _reg_base_init).
 *
 * Freestanding C: no libc, no allocation, no kernel types. The same file is
 * compiled into the kext and into the host tools (tools/rbdump, the hosttest).
 * Every read is bounds-checked against the buffer length; a malformed table
 * yields an error code, never an out-of-bounds read.
 */
#ifndef RB_DISCOVERY_H
#define RB_DISCOVERY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RB_DISC_BINARY_SIGNATURE   0x28211407u
#define RB_DISC_TABLE_SIGNATURE    0x53445049u  /* "IPDS" */
#define RB_DISC_GC_TABLE_ID        0x4347u      /* "GC" */
#define RB_DISC_HARVEST_SIGNATURE  0x56524148u  /* "HARV" */
#define RB_DISC_VCN_TABLE_ID       0x004E4356u
#define RB_DISC_MALL_TABLE_ID      0x4C4C414Du

/* amdgpu_discovery.h */
#define RB_DISC_TMR_SIZE    (10u << 10)
#define RB_DISC_TMR_OFFSET  (64u << 10)

#define RB_DISC_MAX_IPS     128
#define RB_DISC_MAX_BASES   8
#define RB_DISC_MAX_HARVEST 32

/* Hardware IDs used by this project (docs/linux-ref/soc15_hw_ip.h). */
#define RB_HWID_MP1    1
#define RB_HWID_GC     11
#define RB_HWID_MMHUB  34
#define RB_HWID_NBIF   108
#define RB_HWID_MP0    255
#define RB_HWID_DMU    271

enum {
    RB_DISC_OK = 0,
    RB_DISC_E_ARG = -1,          /* NULL buffer or output */
    RB_DISC_E_SHORT = -2,        /* buffer smaller than the binary header */
    RB_DISC_E_SIGNATURE = -3,    /* binary signature is not 0x28211407 */
    RB_DISC_E_VERSION = -4,      /* binary header version not 0, 1 or 2 */
    RB_DISC_E_SIZE = -5,         /* binary_size larger than the buffer or too small */
    RB_DISC_E_NO_IP_TABLE = -6,  /* IP discovery table offset is 0 or out of bounds */
    RB_DISC_E_IP_SIGNATURE = -7, /* IP discovery table signature is not "IPDS" */
    RB_DISC_E_DIE = -8,          /* bad die count, die offset or die id */
    RB_DISC_E_IP_BOUNDS = -9,    /* an IP entry runs past the end of the binary */
};

/* Bits in rb_disc.check_flags: checks Linux makes that failed. Linux refuses
 * the whole table when any of them is set; this parser records them and keeps
 * going while the structure stays in bounds, so a probe still reports what it
 * could read. */
#define RB_DISC_F_BINARY_CHECKSUM  (1u << 0)
#define RB_DISC_F_IP_CHECKSUM      (1u << 1)
#define RB_DISC_F_GC_SIGNATURE     (1u << 2)
#define RB_DISC_F_GC_CHECKSUM      (1u << 3)
#define RB_DISC_F_HARVEST_SIG      (1u << 4)
#define RB_DISC_F_HARVEST_CHECKSUM (1u << 5)
#define RB_DISC_F_VCN_SIGNATURE    (1u << 6)
#define RB_DISC_F_VCN_CHECKSUM     (1u << 7)
#define RB_DISC_F_MALL_SIGNATURE   (1u << 8)
#define RB_DISC_F_TABLE_BOUNDS     (1u << 9)   /* a table offset/size runs past the binary */
#define RB_DISC_F_IPS_TRUNCATED    (1u << 10)  /* more than RB_DISC_MAX_IPS entries */
#define RB_DISC_F_BASES_TRUNCATED  (1u << 11)  /* an entry had more than RB_DISC_MAX_BASES */
#define RB_DISC_F_IP_INVALID       (1u << 12)  /* entry Linux skips (instance >= 48 or hw_id >= 300) */

struct rb_ip {
    uint16_t hw_id;
    uint16_t die;
    uint8_t  inst;
    uint8_t  nbase;      /* as stored in the table */
    uint8_t  major, minor, revision;
    uint8_t  sub_revision, variant;  /* table version >= 3, else 0 */
    uint8_t  harvest;                /* table version < 3, else 0 */
    uint32_t base[RB_DISC_MAX_BASES];  /* dword offsets into the MMIO BAR */
};

/* The fields common to gc_info v1.0 - v1.3, in table order. */
enum {
    RB_GC_NUM_SE, RB_GC_NUM_WGP0_PER_SA, RB_GC_NUM_WGP1_PER_SA, RB_GC_NUM_RB_PER_SE,
    RB_GC_NUM_GL2C, RB_GC_NUM_GPRS, RB_GC_NUM_MAX_GS_THDS, RB_GC_GS_TABLE_DEPTH,
    RB_GC_GSPRIM_BUFF_DEPTH, RB_GC_PARAMETER_CACHE_DEPTH, RB_GC_DOUBLE_OFFCHIP_LDS_BUFFER,
    RB_GC_WAVE_SIZE, RB_GC_MAX_WAVES_PER_SIMD, RB_GC_MAX_SCRATCH_SLOTS_PER_CU, RB_GC_LDS_SIZE,
    RB_GC_NUM_SC_PER_SE, RB_GC_NUM_SA_PER_SE, RB_GC_NUM_PACKER_PER_SC, RB_GC_NUM_GL2A,
    RB_GC_V1_FIELDS
};

struct rb_gc_info {
    uint8_t  present;   /* 0 none, 1 v1.x parsed, 2 other version (fields zero) */
    uint16_t version_major, version_minor;
    uint32_t v1[RB_GC_V1_FIELDS];
};

struct rb_disc {
    uint16_t bin_version_major, bin_version_minor;
    uint16_t bin_size;
    uint16_t ip_table_version;
    uint8_t  base_addr_64_bit;
    uint16_t num_dies;
    uint32_t check_flags;
    uint32_t nips;
    struct rb_ip ips[RB_DISC_MAX_IPS];
    struct rb_gc_info gc;
    uint32_t nharvest;
    struct { uint16_t hw_id; uint8_t inst; } harvest[RB_DISC_MAX_HARVEST];
};

/* Parse `len` bytes at `bin` into `out` (zeroed first). Returns RB_DISC_OK or
 * a negative RB_DISC_E_* code; on RB_DISC_OK, out->check_flags may still list
 * failed checks. */
int rb_disc_parse(const uint8_t *bin, uint32_t len, struct rb_disc *out);

const char *rb_disc_strerror(int err);
const char *rb_hwid_name(uint16_t hw_id);   /* "GC", "MP0", ...; "?" if unknown */

/* First entry with this hw_id and instance, or NULL. */
const struct rb_ip *rb_disc_find(const struct rb_disc *d, uint16_t hw_id, uint8_t inst);

/* Absolute dword offset of register `reg` (a Linux regFOO value) in base
 * segment `base_idx` (regFOO_BASE_IDX) of IP hw_id/inst, as Linux's
 * SOC15_REG_OFFSET computes it. Returns 0 on success, -1 if the IP or the base
 * segment is missing. */
int rb_disc_reg(const struct rb_disc *d, uint16_t hw_id, uint8_t inst,
                uint8_t base_idx, uint32_t reg, uint32_t *out_dw);

#ifdef __cplusplus
}
#endif

#endif /* RB_DISCOVERY_H */
```


===== FILE: src/common/rb_probe.h =====

```cpp
/* SPDX-License-Identifier: MIT */
/*
 * Read-only probe of a Rembrandt-class AMD APU, with no driver attached.
 *
 * Hardware access goes through struct rb_hw so that the same code runs in the
 * kext (real MMIO) and in tools/hosttest (a simulated chip). Freestanding C,
 * no allocation: the caller owns struct rb_probe_result (~40 KB; allocate it,
 * never put it on the kernel stack).
 *
 * The only register writes this code can make are to MM_INDEX / MM_INDEX_HI,
 * to read GPU memory through MM_DATA the way amdgpu_device_mm_access does.
 * Their original values are restored afterwards. Any other write is refused
 * by rb_probe.c itself, before it reaches rb_hw.
 */
#ifndef RB_PROBE_H
#define RB_PROBE_H

#include <stdint.h>
#include "rb_discovery.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Probe levels. 1 is the default: blocks that are powered whenever the
 * machine is on (PSP/SMU mailboxes) or that the boot display already uses
 * (memory hub). 2 adds the display engine, which is lit (the boot display
 * scans out through it) and whose OTG state Linux reads at init. 3 adds the
 * graphics core, which may be power-gated with no driver loaded. */
#define RB_LEVEL_SAFE 1
#define RB_LEVEL_DC   2
#define RB_LEVEL_GC   3
#define RB_LEVEL_MAX  RB_LEVEL_GC

struct rb_hw {
    void *ctx;
    uint32_t mmio_len;   /* bytes mapped of the register BAR (BAR5) */
    uint32_t (*rreg)(void *ctx, uint32_t byte_off);
    void     (*wreg)(void *ctx, uint32_t byte_off, uint32_t value);
    /* Optional framebuffer BAR (BAR0) read, for the cross-check only:
     * copy `size` bytes at `byte_off` into dst; 0 on success. NULL: skipped.
     * fb_len is the BAR's length. */
    uint64_t fb_len;
    int (*fb_read)(void *ctx, uint64_t byte_off, uint8_t *dst, uint32_t size);
    /* Busy-wait `us` microseconds. Used by rb_smu.c's polls; the probe
     * itself never waits. */
    void (*delay_us)(void *ctx, uint32_t us);
};

enum rb_reg_status {
    RB_REG_OK = 0,
    RB_REG_NO_IP,         /* the IP or its base segment is not in the table */
    RB_REG_VERSION,       /* the IP version differs from the one the offset is for */
    RB_REG_OUT_OF_BAR,    /* offset past the mapped register BAR */
    RB_REG_LEVEL,         /* above the requested probe level: not read */
};

struct rb_reg_sample {
    const char *name;
    uint32_t dw;          /* absolute dword offset (when resolved) */
    uint32_t value;
    uint8_t  status;      /* enum rb_reg_status */
};

#define RB_PROBE_BIN_MAX  (16u << 10)
#define RB_PROBE_MAX_REGS 64

enum rb_disc_source {
    RB_SRC_NONE = 0,
    RB_SRC_DEFAULT,       /* (MEMSIZE << 20) - 64 KB held the signature */
    RB_SRC_SCRATCH,       /* DRIVER_SCRATCH_0/1/2 gave offset and size */
    RB_SRC_DEFAULT_NOSIG, /* no signature anywhere; default offset read anyway, as Linux does */
};

struct rb_probe_result {
    int level;
    const char *fail;     /* first fatal problem, or NULL */

    uint32_t memsize_mb;  /* RCC_CONFIG_MEMSIZE: carve-out size in MB */
    uint32_t scratch[3];  /* DRIVER_SCRATCH_0..2 */
    uint32_t c2pmsg33;    /* MP0_SMN_C2PMSG_33 at its fixed offset */
    uint32_t ipd_version; /* IP_DISCOVERY_VERSION */
    uint32_t mm_index_saved, mm_index_hi_saved;

    uint8_t  disc_source; /* enum rb_disc_source */
    uint64_t disc_offset; /* offset in GPU memory */
    uint32_t disc_size;   /* bytes read into disc_bin */
    uint8_t  bar0_checked;   /* 1 if disc_offset was inside BAR0 (past its first
                              * 16 MB, the boot framebuffer) and compared */
    uint8_t  bar0_matches;   /* 1 if BAR0 at disc_offset held the same bytes */
    uint8_t  disc_bin[RB_PROBE_BIN_MAX];
    uint8_t  bar0_bin[RB_PROBE_BIN_MAX];   /* scratch for the cross-check */

    int parse_err;        /* rb_disc_parse result; meaningful if disc_size != 0 */
    struct rb_disc disc;

    uint32_t nregs;
    struct rb_reg_sample regs[RB_PROBE_MAX_REGS];

    uint32_t index_writes;     /* MM_INDEX / MM_INDEX_HI writes made */
    uint32_t refused_writes;   /* writes refused by the policy (must stay 0) */
};

/* Run the probe at `level` (RB_LEVEL_SAFE .. RB_LEVEL_MAX). Fills `r`
 * (zeroed first). Returns 0 if the discovery table was read and parsed,
 * -1 otherwise (r->fail says why; whatever was read is still in r). */
int rb_probe_run(const struct rb_hw *hw, int level, struct rb_probe_result *r);

#ifdef __cplusplus
}
#endif

#endif /* RB_PROBE_H */
```


===== FILE: src/common/rb_regs.h =====

```cpp
/* SPDX-License-Identifier: MIT */
/*
 * Register offsets the probe reads. Values are Linux's regFOO / mmFOO dword
 * offsets and their _BASE_IDX, from torvalds/linux at the commit in
 * docs/linux-ref/UPSTREAM.md:
 *
 *   fixed   amdgpu_discovery.c ("consistent across all the SOCs")
 *   MP0/MP1 include/asic_reg/mp/mp_13_0_2_offset.h (used by psp_v13_0.c and
 *           yellow_carp_ppt.c for Rembrandt's PSP/SMU 13.0.1/13.0.3/13.0.8)
 *   MMHUB   include/asic_reg/mmhub/mmhub_2_3_0_offset.h (gmc_v10_0.c drives
 *           MMHUB 2.3.0, 2.4.0 and 2.4.1 with mmhub_v2_3.c, which uses it)
 *   GC      include/asic_reg/gc/gc_10_3_0_offset.h (Linux uses it for GC 10.3.3)
 *   DCN     include/asic_reg/dcn/dcn_3_1_2_offset.h (DCN 3.1.2 and 3.1.3 are
 *           both DCN_VERSION_3_1 in Linux; dcn31_resource.c uses this header)
 *
 * Every register here is only READ. The single exception is the
 * MM_INDEX / MM_INDEX_HI pair, which selects what MM_DATA reads; see
 * rb_probe.c.
 */
#ifndef RB_REGS_H
#define RB_REGS_H

#include <stdint.h>
#include "rb_discovery.h"
#include "rb_probe.h"

/* Fixed dword offsets into the MMIO BAR. */
#define RB_mmMM_INDEX            0x0
#define RB_mmMM_DATA             0x1
#define RB_mmMM_INDEX_HI         0x6
#define RB_mmDRIVER_SCRATCH_0    0x94
#define RB_mmDRIVER_SCRATCH_1    0x95
#define RB_mmDRIVER_SCRATCH_2    0x96
#define RB_mmRCC_CONFIG_MEMSIZE  0xde3
#define RB_mmMP0_SMN_C2PMSG_33   0x16061
#define RB_mmIP_DISCOVERY_VERSION 0x16A00

struct rb_reg_def {
    const char *name;
    uint16_t hw_id;
    uint8_t  base_idx;
    uint8_t  level;
    uint8_t  want_major;   /* IP major version the offset is valid for */
    uint8_t  minor_lo, minor_hi;   /* ... and its minor range (Linux's grouping) */
    uint32_t reg;
};

static const struct rb_reg_def rb_reg_defs[] = {
    /* PSP (MP0). C2PMSG_35 bit 31: bootloader ready. C2PMSG_81 != 0: secure
     * OS running (psp_v13_0_is_sos_alive). C2PMSG_33 bit 31: IFWI init done. */
    { "MP0_C2PMSG_33",  RB_HWID_MP0, 0, RB_LEVEL_SAFE, 13, 0, 0xff, 0x0061 },
    { "MP0_C2PMSG_35",  RB_HWID_MP0, 0, RB_LEVEL_SAFE, 13, 0, 0xff, 0x0063 },
    { "MP0_C2PMSG_81",  RB_HWID_MP0, 0, RB_LEVEL_SAFE, 13, 0, 0xff, 0x0091 },
    /* SMU (MP1) mailbox: 66 message, 82 argument, 90 response. */
    { "MP1_C2PMSG_66",  RB_HWID_MP1, 0, RB_LEVEL_SAFE, 13, 0, 0xff, 0x0282 },
    { "MP1_C2PMSG_82",  RB_HWID_MP1, 0, RB_LEVEL_SAFE, 13, 0, 0xff, 0x0292 },
    { "MP1_C2PMSG_90",  RB_HWID_MP1, 0, RB_LEVEL_SAFE, 13, 0, 0xff, 0x029a },
    /* Where the carve-out sits: FB_LOCATION in 16 MB units of GPU address
     * space, FB_OFFSET its system-physical address in 16 MB units. */
    { "MMMC_VM_FB_OFFSET",        RB_HWID_MMHUB, 1, RB_LEVEL_SAFE, 2, 3, 4, 0x08d7 },
    { "MMMC_VM_FB_LOCATION_BASE", RB_HWID_MMHUB, 1, RB_LEVEL_SAFE, 2, 3, 4, 0x08ec },
    { "MMMC_VM_FB_LOCATION_TOP",  RB_HWID_MMHUB, 1, RB_LEVEL_SAFE, 2, 3, 4, 0x08ed },
    { "MMMC_VM_AGP_BASE",         RB_HWID_MMHUB, 1, RB_LEVEL_SAFE, 2, 3, 4, 0x08f0 },

    /* Level 3: graphics core. */
    { "GRBM_STATUS",              RB_HWID_GC, 0, RB_LEVEL_GC, 10, 3, 3, 0x0da4 },
    { "GRBM_STATUS2",             RB_HWID_GC, 0, RB_LEVEL_GC, 10, 3, 3, 0x0da2 },
    { "CP_STAT",                  RB_HWID_GC, 0, RB_LEVEL_GC, 10, 3, 3, 0x0f40 },
    { "CP_ME_CNTL",               RB_HWID_GC, 0, RB_LEVEL_GC, 10, 3, 3, 0x0f56 },
    { "GCMC_VM_FB_OFFSET",        RB_HWID_GC, 0, RB_LEVEL_GC, 10, 3, 3, 0x16e7 },
    { "GCMC_VM_FB_LOCATION_BASE", RB_HWID_GC, 0, RB_LEVEL_GC, 10, 3, 3, 0x16fc },
    { "GCMC_VM_FB_LOCATION_TOP",  RB_HWID_GC, 0, RB_LEVEL_GC, 10, 3, 3, 0x16fd },
    { "RLC_CNTL",                 RB_HWID_GC, 1, RB_LEVEL_GC, 10, 3, 3, 0x4c00 },
    { "RLC_STAT",                 RB_HWID_GC, 1, RB_LEVEL_GC, 10, 3, 3, 0x4c04 },

    /* Level 2: display engine (DCN 3.1.x), which pipe scans out what. */
    { "DMCUB_CNTL",       RB_HWID_DMU, 2, RB_LEVEL_DC, 3, 1, 1, 0x01f6 },
    { "DMCUB_SCRATCH0",   RB_HWID_DMU, 2, RB_LEVEL_DC, 3, 1, 1, 0x01e3 },
    { "OTG0_CONTROL",     RB_HWID_DMU, 2, RB_LEVEL_DC, 3, 1, 1, 0x1b41 },
    { "OTG0_H_TOTAL",     RB_HWID_DMU, 2, RB_LEVEL_DC, 3, 1, 1, 0x1b2a },
    { "OTG0_V_TOTAL",     RB_HWID_DMU, 2, RB_LEVEL_DC, 3, 1, 1, 0x1b2f },
    { "OTG1_CONTROL",     RB_HWID_DMU, 2, RB_LEVEL_DC, 3, 1, 1, 0x1bc1 },
    { "OTG1_H_TOTAL",     RB_HWID_DMU, 2, RB_LEVEL_DC, 3, 1, 1, 0x1baa },
    { "OTG1_V_TOTAL",     RB_HWID_DMU, 2, RB_LEVEL_DC, 3, 1, 1, 0x1baf },
    { "OTG2_CONTROL",     RB_HWID_DMU, 2, RB_LEVEL_DC, 3, 1, 1, 0x1c41 },
    { "OTG2_H_TOTAL",     RB_HWID_DMU, 2, RB_LEVEL_DC, 3, 1, 1, 0x1c2a },
    { "OTG2_V_TOTAL",     RB_HWID_DMU, 2, RB_LEVEL_DC, 3, 1, 1, 0x1c2f },
    { "OTG3_CONTROL",     RB_HWID_DMU, 2, RB_LEVEL_DC, 3, 1, 1, 0x1cc1 },
    { "OTG3_H_TOTAL",     RB_HWID_DMU, 2, RB_LEVEL_DC, 3, 1, 1, 0x1caa },
    { "OTG3_V_TOTAL",     RB_HWID_DMU, 2, RB_LEVEL_DC, 3, 1, 1, 0x1caf },
    { "HUBP0_SURFACE_CONFIG",  RB_HWID_DMU, 2, RB_LEVEL_DC, 3, 1, 1, 0x05e5 },
    { "HUBP0_PRIMARY_ADDR",    RB_HWID_DMU, 2, RB_LEVEL_DC, 3, 1, 1, 0x060a },
    { "HUBP0_PRIMARY_ADDR_HI", RB_HWID_DMU, 2, RB_LEVEL_DC, 3, 1, 1, 0x060b },
    { "HUBP1_SURFACE_CONFIG",  RB_HWID_DMU, 2, RB_LEVEL_DC, 3, 1, 1, 0x06c1 },
    { "HUBP1_PRIMARY_ADDR",    RB_HWID_DMU, 2, RB_LEVEL_DC, 3, 1, 1, 0x06e6 },
    { "HUBP1_PRIMARY_ADDR_HI", RB_HWID_DMU, 2, RB_LEVEL_DC, 3, 1, 1, 0x06e7 },
    { "HUBP2_SURFACE_CONFIG",  RB_HWID_DMU, 2, RB_LEVEL_DC, 3, 1, 1, 0x079d },
    { "HUBP2_PRIMARY_ADDR",    RB_HWID_DMU, 2, RB_LEVEL_DC, 3, 1, 1, 0x07c2 },
    { "HUBP2_PRIMARY_ADDR_HI", RB_HWID_DMU, 2, RB_LEVEL_DC, 3, 1, 1, 0x07c3 },
    { "HUBP3_SURFACE_CONFIG",  RB_HWID_DMU, 2, RB_LEVEL_DC, 3, 1, 1, 0x0879 },
    { "HUBP3_PRIMARY_ADDR",    RB_HWID_DMU, 2, RB_LEVEL_DC, 3, 1, 1, 0x089e },
    { "HUBP3_PRIMARY_ADDR_HI", RB_HWID_DMU, 2, RB_LEVEL_DC, 3, 1, 1, 0x089f },
};

#define RB_NREG_DEFS (sizeof(rb_reg_defs) / sizeof(rb_reg_defs[0]))

#endif /* RB_REGS_H */
```


===== FILE: src/common/rb_smu.h =====

```cpp
/* SPDX-License-Identifier: MIT */
/*
 * Read-only queries to the SMU (MP1 power-management firmware) of a
 * Rembrandt-class APU, through the driver mailbox Linux uses:
 * MP1_C2PMSG_66 message, _82 argument, _90 response (smu_v13_0_init_msg_ctl),
 * with smu_msg_v1_send_msg's protocol (pm/swsmu/smu_cmn.c):
 *
 *   pre-poll   wait for response != 0 (the previous message finished)
 *   send       response = 0, argument = arg, message = index
 *   post-poll  wait for response != 0; 1 = OK, then read the output from
 *              the argument register
 *
 * Stricter than Linux:
 *   - only the messages in rb_smu.c's allowlist, which all just report state
 *     (version, interface version, enabled-feature mask);
 *   - nothing is sent unless the response register reads exactly 1 (OK, idle);
 *   - the first message that does not come back OK stops the sequence;
 *   - the only registers written are those three mailbox registers, resolved
 *     from the discovery table for an MP1 of major version 13; any other write
 *     is refused before it reaches rb_hw.
 *
 * Message numbers: pm/swsmu/inc/pmfw_if/smu_v13_0_1_ppsmc.h (yellow_carp_ppt.c
 * uses it for MP1 13.0.1, 13.0.3 and 13.0.8).
 */
#ifndef RB_SMU_H
#define RB_SMU_H

#include <stdint.h>
#include "rb_discovery.h"
#include "rb_probe.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RB_SMU_MSG_GetSmuVersion          0x02
#define RB_SMU_MSG_GetDriverIfVersion     0x03
#define RB_SMU_MSG_GetEnabledSmuFeatures  0x12

/* PPSMC_Result_* */
#define RB_SMU_RESP_NONE          0x00
#define RB_SMU_RESP_OK            0x01
#define RB_SMU_RESP_FAILED        0xFF
#define RB_SMU_RESP_UNKNOWN_CMD   0xFE
#define RB_SMU_RESP_REJ_PREREQ    0xFD
#define RB_SMU_RESP_REJ_BUSY      0xFC

/* Linux: adev->usec_timeout (100000) * 20, polled at 1 us. */
#define RB_SMU_TIMEOUT_US 2000000u

enum rb_smu_xfer_status {
    RB_SMU_X_OK = 0,
    RB_SMU_X_PRE_BUSY,      /* pre-poll: response stayed 0 */
    RB_SMU_X_TIMEOUT,       /* sent; no response within the timeout */
    RB_SMU_X_FW_ERROR,      /* sent; response was a failure code */
};

struct rb_smu_xfer {
    uint8_t  msg;
    uint32_t arg;
    uint8_t  status;        /* enum rb_smu_xfer_status */
    uint32_t resp;          /* response register after the post-poll */
    uint32_t out;           /* argument register after an OK response */
    uint32_t polls;         /* 1 us polls the post-poll took */
};

#define RB_SMU_MAX_XFERS 4

struct rb_smu_result {
    const char *fail;       /* why the sequence stopped, or NULL */
    uint32_t mp1_major, mp1_minor, mp1_rev;
    uint32_t dw_msg, dw_arg, dw_resp;
    uint32_t initial_msg, initial_arg, initial_resp;

    uint32_t nxfers;
    struct rb_smu_xfer x[RB_SMU_MAX_XFERS];

    uint8_t  have_version, have_if_version, have_features;
    uint32_t smu_version;   /* program << 24 | major << 16 | minor << 8 | debug */
    uint32_t if_version;
    uint64_t features;      /* FEATURE_*_BIT of smu_v13_0_1_pmfw.h */

    uint32_t writes;
    uint32_t refused_writes;   /* must stay 0 */
};

/* Run the query sequence. `d` is a parsed discovery table (rb_probe_run's).
 * hw->delay_us must be set. timeout_us 0 means RB_SMU_TIMEOUT_US. Returns 0
 * if every query came back OK, -1 otherwise (r->fail says why). */
int rb_smu_query(const struct rb_hw *hw, const struct rb_disc *d,
                 uint32_t timeout_us, struct rb_smu_result *r);

#ifdef __cplusplus
}
#endif

#endif /* RB_SMU_H */
```


===== FILE: src/common/rb_smu13.h =====

```cpp
/* SPDX-License-Identifier: MIT */
/*
 * SMU 13 (MP1 13.0.1 / 13.0.3 / 13.0.8, "Yellow Carp" = Rembrandt) power
 * management, ported from Linux pm/swsmu/smu13/yellow_carp_ppt.c and the
 * smu_v13_0.c / smu_cmn.c helpers it uses. This is the layer Apple's SMU
 * function list (smu_11_0_7_create_function_pointer_list's 27 slots, see
 * docs/APPLE-STACK.md B3) is to be built on.
 *
 * Freestanding C, no allocation; hardware through struct rb_hw (rb_probe.h)
 * like the probe, so tools/hosttest runs it against a simulated SMU.
 *
 * Transport: the same mailbox and protocol as rb_smu.c (C2PMSG_66 message,
 * _82 argument, _90 response; smu_msg_v1_send_msg). The only registers
 * written are those three; SMUIO_GFX_MISC_CNTL is read for the GFXOFF status.
 *
 * Policy: messages are grouped (RB_SMU13_ALLOW_*). rb_smu13_init() allows
 * only RB_SMU13_ALLOW_QUERY, the messages the probe already sends on this
 * machine. Every other group changes the chip's state and has to be allowed
 * by the caller with rb_smu13_allow() before any of its messages is sent; a
 * message outside the allowed groups returns RB_SMU13_E_NOT_ALLOWED without
 * touching the mailbox. A timeout makes the context dead: nothing more is sent
 * until rb_smu13_init() again.
 *
 * Table transfers (DPM clocks, metrics, watermarks) go through a DMA buffer
 * the caller owns (struct rb_smu13_dma): the SMU reads and writes it at its
 * GPU (MC) address, the CPU at `cpu`.
 */
#ifndef RB_SMU13_H
#define RB_SMU13_H

#include <stdint.h>
#include "rb_discovery.h"
#include "rb_probe.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------- messages (ppsmc.h) --- */
/* smu_v13_0_1_ppsmc.h; only those yellow_carp_message_map maps. */
#define RB_SMU13_MSG_TestMessage               0x01
#define RB_SMU13_MSG_GetSmuVersion             0x02
#define RB_SMU13_MSG_GetDriverIfVersion        0x03
#define RB_SMU13_MSG_EnableGfxOff              0x04
#define RB_SMU13_MSG_PowerDownVcn              0x06
#define RB_SMU13_MSG_PowerUpVcn                0x07
#define RB_SMU13_MSG_SetHardMinVcn             0x08
#define RB_SMU13_MSG_PrepareMp1ForUnload       0x0C
#define RB_SMU13_MSG_SetDriverDramAddrHigh     0x0D
#define RB_SMU13_MSG_SetDriverDramAddrLow      0x0E
#define RB_SMU13_MSG_TransferTableSmu2Dram     0x0F
#define RB_SMU13_MSG_TransferTableDram2Smu     0x10
#define RB_SMU13_MSG_GfxDeviceDriverReset      0x11
#define RB_SMU13_MSG_GetEnabledSmuFeatures     0x12
#define RB_SMU13_MSG_SetHardMinSocclkByFreq    0x13
#define RB_SMU13_MSG_SetSoftMinVcn             0x15
#define RB_SMU13_MSG_GetGfxclkFrequency        0x17
#define RB_SMU13_MSG_GetFclkFrequency          0x18
#define RB_SMU13_MSG_AllowGfxOff               0x19
#define RB_SMU13_MSG_DisallowGfxOff            0x1A
#define RB_SMU13_MSG_SetSoftMaxGfxClk          0x1B
#define RB_SMU13_MSG_SetHardMinGfxClk          0x1C
#define RB_SMU13_MSG_SetSoftMaxSocclkByFreq    0x1D
#define RB_SMU13_MSG_SetSoftMaxFclkByFreq      0x1E
#define RB_SMU13_MSG_SetSoftMaxVcn             0x1F
#define RB_SMU13_MSG_SetPowerLimitPercentage   0x20
#define RB_SMU13_MSG_PowerDownJpeg             0x21
#define RB_SMU13_MSG_PowerUpJpeg               0x22
#define RB_SMU13_MSG_SetHardMinFclkByFreq      0x23
#define RB_SMU13_MSG_SetSoftMinSocclkByFreq    0x24
#define RB_SMU13_MSG_COUNT                     0x29

/* Mode argument of GfxDeviceDriverReset (SMU_RESET_MODE_2). */
#define RB_SMU13_RESET_MODE_2 2

/* ------------------------------------------------------- message groups --- */
#define RB_SMU13_ALLOW_QUERY    0x01u  /* TestMessage, versions, feature mask, Get*Frequency */
#define RB_SMU13_ALLOW_TABLES   0x02u  /* driver table address, table transfers */
#define RB_SMU13_ALLOW_GFXOFF   0x04u  /* EnableGfxOff, Allow/DisallowGfxOff */
#define RB_SMU13_ALLOW_CLOCKS   0x08u  /* hard/soft min/max of GFX, SOC, FCLK, VCN; power limit */
#define RB_SMU13_ALLOW_MMPOWER  0x10u  /* VCN and JPEG power up/down */
#define RB_SMU13_ALLOW_UNLOAD   0x20u  /* PrepareMp1ForUnload */
#define RB_SMU13_ALLOW_RESET    0x40u  /* GfxDeviceDriverReset */

/* --------------------------------------------------------------- errors --- */
#define RB_SMU13_OK               0
#define RB_SMU13_E_INVAL         -1   /* bad argument or context */
#define RB_SMU13_E_NOT_ALLOWED   -2   /* message group not allowed: nothing sent */
#define RB_SMU13_E_BUSY          -3   /* mailbox busy or in an unknown state: nothing sent */
#define RB_SMU13_E_TIMEOUT       -4   /* sent, no answer; the context is now dead */
#define RB_SMU13_E_FW            -5   /* the SMU answered with an error (last_resp) */
#define RB_SMU13_E_VERSION       -6   /* not an MP1 13.0.1/3/8, or interface != 4 */
#define RB_SMU13_E_NODATA        -7   /* needs a table that is not loaded */
#define RB_SMU13_E_DEAD          -8   /* an earlier timeout: nothing sent */

/* Response codes (PPSMC_Result_*). */
#define RB_SMU13_RESP_NONE         0x00
#define RB_SMU13_RESP_OK           0x01
#define RB_SMU13_RESP_FAILED       0xFF
#define RB_SMU13_RESP_UNKNOWN_CMD  0xFE
#define RB_SMU13_RESP_REJ_PREREQ   0xFD
#define RB_SMU13_RESP_REJ_BUSY     0xFC

/* SMU13_YELLOW_CARP_DRIVER_IF_VERSION: the tables below match this. */
#define RB_SMU13_DRIVER_IF_VERSION 4

/* --------------------------------------- tables (driver_if_yellow_carp.h) --- */
/* Same layouts as Linux's DpmClocks_t, SmuMetrics_t, Watermarks_t;
 * tools/hosttest checks every field offset against the Linux header. */

#define RB_SMU13_TABLE_WATERMARKS  1
#define RB_SMU13_TABLE_DPMCLOCKS   4
#define RB_SMU13_TABLE_SMU_METRICS 7

#define RB_SMU13_NUM_DPM_LEVELS    8
#define RB_SMU13_NUM_DF_PSTATES    4
#define RB_SMU13_NUM_WM_RANGES     4

struct rb_smu13_df_pstate {
    uint32_t fclk, memclk, voltage;
    uint8_t  wck_ratio, spare[3];
};

struct rb_smu13_dpm_clocks {            /* DpmClocks_t; MHz, mV with 2 fraction bits */
    uint32_t dcf_clocks[RB_SMU13_NUM_DPM_LEVELS];
    uint32_t disp_clocks[RB_SMU13_NUM_DPM_LEVELS];
    uint32_t dpp_clocks[RB_SMU13_NUM_DPM_LEVELS];
    uint32_t soc_clocks[RB_SMU13_NUM_DPM_LEVELS];
    uint32_t vclocks[RB_SMU13_NUM_DPM_LEVELS];
    uint32_t dclocks[RB_SMU13_NUM_DPM_LEVELS];
    uint32_t soc_voltage[RB_SMU13_NUM_DPM_LEVELS];
    struct rb_smu13_df_pstate df_pstate[RB_SMU13_NUM_DF_PSTATES];
    uint8_t  num_dcf_levels, num_disp_levels, num_soc_levels, num_vcn_levels, num_df_pstates;
    uint8_t  spare[3];
    uint32_t min_gfx_clk, max_gfx_clk;
};

struct rb_smu13_metrics {               /* SmuMetrics_t */
    uint16_t gfxclk, socclk, vclk, dclk, memclk, spare;        /* MHz */
    uint16_t gfx_activity, uvd_activity;                       /* centi-percent */
    uint16_t voltage[2], current[2], power[2];                 /* mV, mA, mW: VDD, SOC */
    uint16_t core_freq[8], core_power[8], core_temp[8];
    uint16_t l3_freq, l3_temp;
    uint16_t gfx_temp, soc_temp;                               /* centi-degrees C */
    uint16_t throttler_status;
    uint16_t current_socket_power;                             /* mW */
    uint16_t stapm_opn_limit, stapm_current_limit;             /* W */
    uint32_t apu_power, dgpu_power;                            /* mW */
    uint16_t vdd_tdc, soc_tdc, vdd_edc, soc_edc;               /* mA */
    uint16_t infra_cpu_max_freq, infra_gfx_max_freq;           /* MHz */
    uint16_t skin_temp, device_state;
};

struct rb_smu13_wm_row {                /* WatermarkRowGeneric_t */
    uint16_t min_clock, max_clock, min_mclk, max_mclk;
    uint8_t  wm_setting, wm_type, padding[2];
};

struct rb_smu13_watermarks {            /* Watermarks_t; row 0 SOCCLK, 1 DCFCLK */
    struct rb_smu13_wm_row row[2][RB_SMU13_NUM_WM_RANGES];
    uint32_t mmhub_padding[7];
};

/* One watermark set as DC hands it over (pp_smu_wm_set_range). */
struct rb_smu13_wm_set {
    uint8_t  wm_inst;
    uint16_t min_fill_mhz, max_fill_mhz, min_drain_mhz, max_drain_mhz;
};

/* --------------------------------------------------- feature bits (pmfw) --- */
#define RB_SMU13_FEATURE_CCLK_DPM     0
#define RB_SMU13_FEATURE_VCN_DPM     11
#define RB_SMU13_FEATURE_FCLK_DPM    13
#define RB_SMU13_FEATURE_SOCCLK_DPM  14
#define RB_SMU13_FEATURE_MP0CLK_DPM  15
#define RB_SMU13_FEATURE_LCLK_DPM    16
#define RB_SMU13_FEATURE_SHUBCLK_DPM 17
#define RB_SMU13_FEATURE_DCFCLK_DPM  18
#define RB_SMU13_FEATURE_GFX_DPM     19
/* yellow_carp_dpm_features: all set = "DPM is running". */
#define RB_SMU13_DPM_FEATURES \
    ((1ull << RB_SMU13_FEATURE_CCLK_DPM) | (1ull << RB_SMU13_FEATURE_VCN_DPM) | \
     (1ull << RB_SMU13_FEATURE_FCLK_DPM) | (1ull << RB_SMU13_FEATURE_SOCCLK_DPM) | \
     (1ull << RB_SMU13_FEATURE_MP0CLK_DPM) | (1ull << RB_SMU13_FEATURE_LCLK_DPM) | \
     (1ull << RB_SMU13_FEATURE_SHUBCLK_DPM) | (1ull << RB_SMU13_FEATURE_DCFCLK_DPM) | \
     (1ull << RB_SMU13_FEATURE_GFX_DPM))

/* ---------------------------------------------------------------- types --- */
enum rb_smu13_clk {
    RB_SMU13_CLK_GFX,       /* SMU_GFXCLK / SMU_SCLK */
    RB_SMU13_CLK_SOC,
    RB_SMU13_CLK_FCLK,
    RB_SMU13_CLK_MCLK,      /* SMU_MCLK / SMU_UCLK */
    RB_SMU13_CLK_VCLK,
    RB_SMU13_CLK_DCLK,
};

enum rb_smu13_level {       /* amd_dpm_forced_level */
    RB_SMU13_LEVEL_AUTO,
    RB_SMU13_LEVEL_LOW,
    RB_SMU13_LEVEL_HIGH,
    RB_SMU13_LEVEL_MANUAL,
    RB_SMU13_LEVEL_PROFILE_STANDARD,
    RB_SMU13_LEVEL_PROFILE_MIN_SCLK,
    RB_SMU13_LEVEL_PROFILE_MIN_MCLK,
    RB_SMU13_LEVEL_PROFILE_PEAK,
    RB_SMU13_LEVEL_PROFILE_EXIT,
};

enum rb_smu13_sensor {      /* amd_pp_sensors yellow_carp_read_sensor answers */
    RB_SMU13_SENSOR_GPU_LOAD,       /* percent */
    RB_SMU13_SENSOR_VCN_LOAD,       /* percent */
    RB_SMU13_SENSOR_INPUT_POWER,    /* CurrentSocketPower, mW */
    RB_SMU13_SENSOR_EDGE_TEMP,      /* millidegrees C */
    RB_SMU13_SENSOR_HOTSPOT_TEMP,   /* millidegrees C */
    RB_SMU13_SENSOR_MCLK,           /* 10 kHz units (MHz * 100) */
    RB_SMU13_SENSOR_SCLK,           /* 10 kHz units */
    RB_SMU13_SENSOR_VDDGFX,         /* mV */
    RB_SMU13_SENSOR_VDDNB,          /* mV */
    RB_SMU13_SENSOR_SS_APU_SHARE,   /* percent */
    RB_SMU13_SENSOR_SS_DGPU_SHARE,  /* percent */
};

/* The driver table: DMA memory the SMU can reach. size must hold the largest
 * table (rb_smu13_table_size_max()). flush/invalidate: HDP flush before the
 * SMU reads it and invalidate after it wrote (amdgpu_hdp_flush/_invalidate);
 * may be NULL when the memory is coherent. */
struct rb_smu13_dma {
    void    *cpu;
    uint64_t mc;
    uint32_t size;
    void (*flush)(void *ctx);
    void (*invalidate)(void *ctx);
};

struct rb_smu13 {
    const struct rb_hw *hw;
    uint32_t timeout_us;
    uint32_t allowed;              /* RB_SMU13_ALLOW_* */
    uint32_t mp1_major, mp1_minor, mp1_rev;
    uint32_t dw_msg, dw_arg, dw_resp;
    uint32_t dw_gfx_misc_cntl;     /* SMUIO_GFX_MISC_CNTL, 0 if no SMUIO */
    int dead;

    struct rb_smu13_dma dma;
    int table_location_set;

    /* State read from the SMU. */
    uint32_t smu_version, if_version;
    uint64_t features;
    int have_versions, have_features;
    struct rb_smu13_dpm_clocks clocks;
    int have_clocks;

    /* yellow_carp_set_fine_grain_gfx_freq_parameters / od_edit. */
    uint32_t gfx_default_hard_min, gfx_default_soft_max;
    uint32_t gfx_actual_hard_min, gfx_actual_soft_max;

    /* Accounting. */
    uint32_t nmsgs, writes, refused_writes;
    uint8_t  last_msg;
    uint32_t last_arg, last_resp, last_polls;
};

/* ----------------------------------------------------------- operations --- */
/* Each returns RB_SMU13_OK or a negative RB_SMU13_E_*. Linux function in []. */

/* Bind to the chip: MP1 13.0.1/3/8 from the discovery table, mailbox and
 * SMUIO registers, `dma` as the driver table (may be NULL: no table
 * operations). Sends nothing. Allows RB_SMU13_ALLOW_QUERY only.
 * timeout_us 0: 2 s (Linux's). */
int rb_smu13_init(struct rb_smu13 *s, const struct rb_hw *hw, const struct rb_disc *d,
                  const struct rb_smu13_dma *dma, uint32_t timeout_us);
void rb_smu13_allow(struct rb_smu13 *s, uint32_t groups);
uint32_t rb_smu13_msg_group(uint8_t msg);   /* RB_SMU13_ALLOW_* of a message, 0 if unknown */
uint32_t rb_smu13_table_size_max(void);

/* One message, any group the context allows. out may be NULL. */
int rb_smu13_send(struct rb_smu13 *s, uint8_t msg, uint32_t arg, uint32_t *out);

/* [smu_cmn_check_fw_version] versions; E_VERSION if the interface is not 4. */
int rb_smu13_check_fw_version(struct rb_smu13 *s);
/* [smu_cmn_get_enabled_mask] */
int rb_smu13_get_enabled_mask(struct rb_smu13 *s, uint64_t *mask);
/* [yellow_carp_is_dpm_running] 1 running, 0 not, <0 error. */
int rb_smu13_is_dpm_running(struct rb_smu13 *s);
/* [smu_v13_0_set_driver_table_location] */
int rb_smu13_set_driver_table_location(struct rb_smu13 *s);
/* [smu_cmn_update_table] table_id RB_SMU13_TABLE_*; drv2smu 1 = write. */
int rb_smu13_update_table(struct rb_smu13 *s, uint32_t table_id, uint16_t arg,
                          void *data, uint32_t size, int drv2smu);
/* [yellow_carp_set_default_dpm_tables + set_fine_grain_gfx_freq_parameters] */
int rb_smu13_set_default_dpm_table(struct rb_smu13 *s);
/* [smu_cmn_get_metrics_table] always fresh (no cache). */
int rb_smu13_get_metrics(struct rb_smu13 *s, struct rb_smu13_metrics *m);
/* [yellow_carp_read_sensor] */
int rb_smu13_read_sensor(struct rb_smu13 *s, enum rb_smu13_sensor sensor, uint32_t *value);
/* [yellow_carp_set_watermarks_table] writes the table on every call. */
int rb_smu13_set_watermarks(struct rb_smu13 *s,
                            const struct rb_smu13_wm_set *reader, uint32_t nreader,
                            const struct rb_smu13_wm_set *writer, uint32_t nwriter);

/* [yellow_carp_post_smu_init] EnableGfxOff. */
int rb_smu13_enable_gfxoff(struct rb_smu13 *s);
/* [smu_v13_0_gfx_off_control] AllowGfxOff / DisallowGfxOff. */
int rb_smu13_gfx_off_control(struct rb_smu13 *s, int enable);
/* [yellow_carp_get_gfxoff_status] 0 in GFXOFF, 1 leaving, 2 not in GFXOFF,
 * 3 entering. */
int rb_smu13_get_gfxoff_status(struct rb_smu13 *s, uint32_t *status);
/* [yellow_carp_dpm_set_vcn_enable / _jpeg_enable] */
int rb_smu13_set_vcn_power(struct rb_smu13 *s, int on);
int rb_smu13_set_jpeg_power(struct rb_smu13 *s, int on);
/* [yellow_carp_system_features_control(false)] PrepareMp1ForUnload. */
int rb_smu13_prepare_unload(struct rb_smu13 *s);
/* [yellow_carp_mode2_reset] */
int rb_smu13_mode2_reset(struct rb_smu13 *s);

/* Clocks, all in MHz. Need rb_smu13_set_default_dpm_table() and the feature
 * mask (rb_smu13_get_enabled_mask()) first. */
/* [yellow_carp_get_current_clk_freq] */
int rb_smu13_get_current_clk(struct rb_smu13 *s, enum rb_smu13_clk clk, uint32_t *mhz);
/* [yellow_carp_get_dpm_level_count] */
int rb_smu13_get_dpm_level_count(struct rb_smu13 *s, enum rb_smu13_clk clk, uint32_t *count);
/* [yellow_carp_get_dpm_freq_by_index] */
int rb_smu13_get_dpm_freq_by_index(struct rb_smu13 *s, enum rb_smu13_clk clk,
                                   uint32_t level, uint32_t *mhz);
/* [yellow_carp_get_dpm_ultimate_freq] min/max may be NULL. E_NODATA when the
 * clock's DPM is off (Linux then uses VBIOS boot values, not available here). */
int rb_smu13_get_dpm_ultimate_freq(struct rb_smu13 *s, enum rb_smu13_clk clk,
                                   uint32_t *min, uint32_t *max);
/* [yellow_carp_set_soft_freq_limited_range] */
int rb_smu13_set_soft_freq_range(struct rb_smu13 *s, enum rb_smu13_clk clk,
                                 uint32_t min, uint32_t max);
/* [yellow_carp_force_clk_levels] SOC, FCLK, VCLK, DCLK. */
int rb_smu13_force_clk_levels(struct rb_smu13 *s, enum rb_smu13_clk clk, uint32_t mask);
/* [yellow_carp_set_performance_level] */
int rb_smu13_set_performance_level(struct rb_smu13 *s, enum rb_smu13_level level);

/* The bring-up Linux does in smu_hw_init / smu_smc_hw_setup for this ASIC,
 * read-and-configure part only: versions (interface must be 4), feature
 * mask, driver table location, DPM clock table. Needs ALLOW_TABLES. Does not
 * touch GFXOFF, clocks or VCN/JPEG power: those are separate calls. */
int rb_smu13_bringup(struct rb_smu13 *s);

const char *rb_smu13_strerror(int err);

#ifdef __cplusplus
}
#endif

#endif /* RB_SMU13_H */
```


===== FILE: src/common/rb_discovery.c =====

```cpp
/* SPDX-License-Identifier: MIT */
/* AMD IP discovery table parser. See rb_discovery.h. */
#include "rb_discovery.h"

/* Little-endian loads that never read past `len`. The caller checks the range
 * first with in_bounds(); these only assemble bytes (no unaligned access). */
static uint16_t ld16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t ld32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int in_bounds(uint32_t off, uint32_t size, uint32_t len)
{
    return off <= len && size <= len - off;
}

static uint16_t byte_sum(const uint8_t *p, uint32_t n)
{
    uint16_t s = 0;
    for (uint32_t i = 0; i < n; i++)
        s = (uint16_t)(s + p[i]);
    return s;
}

static void zero(void *p, uint32_t n)
{
    volatile uint8_t *b = (volatile uint8_t *)p;
    for (uint32_t i = 0; i < n; i++)
        b[i] = 0;
}

/* binary_header (v0/v1): signature u32, major u16, minor u16, checksum u16,
 * size u16, then 6 x table_info{offset u16, checksum u16, size u16, pad u16}.
 * binary_header_v2: the same 12 bytes, then num_tables u16, pad u16, then the
 * table_info list. */
#define BHDR_FIXED        12u
#define TABLE_INFO_SIZE   8u
enum { T_IP = 0, T_GC, T_HARVEST, T_VCN, T_MALL, T_NPS, T_TOTAL };

struct tinfo { uint16_t offset, checksum; };

static int table_info(const uint8_t *bin, uint32_t len, uint16_t major, unsigned id, struct tinfo *ti)
{
    uint32_t list, count;
    if (major == 2) {
        if (!in_bounds(BHDR_FIXED, 4, len))
            return -1;
        count = ld16(bin + BHDR_FIXED);
        list = BHDR_FIXED + 4;
    } else {
        count = T_TOTAL;
        list = BHDR_FIXED;
    }
    if (id >= count || !in_bounds(list + id * TABLE_INFO_SIZE, TABLE_INFO_SIZE, len))
        return -1;
    ti->offset = ld16(bin + list + id * TABLE_INFO_SIZE);
    ti->checksum = ld16(bin + list + id * TABLE_INFO_SIZE + 2);
    return 0;
}

/* One signed table, checked as amdgpu_discovery_table_check does: a zero
 * offset means "absent" and passes. size_off/size_w locate the size field
 * inside the table header (0 width: fixed `fixed_size`). */
static void check_table(const uint8_t *bin, uint32_t len, uint16_t major, unsigned id,
                        uint32_t want_sig, uint32_t size_off, uint32_t size_w,
                        uint32_t fixed_size, int check_sum,
                        uint32_t f_sig, uint32_t f_sum, uint32_t *flags)
{
    struct tinfo ti;
    uint32_t size;
    if (table_info(bin, len, major, id, &ti) || ti.offset == 0)
        return;
    if (!in_bounds(ti.offset, size_off + (size_w ? size_w : 4), len)) {
        *flags |= RB_DISC_F_TABLE_BOUNDS;
        return;
    }
    if (ld32(bin + ti.offset) != want_sig) {
        *flags |= f_sig;
        return;
    }
    if (!check_sum)
        return;
    /* Linux reads the GC size as le16 of a u32 field, the IP table size as
     * le16, VCN as le32; mirror that. */
    if (size_w == 2)
        size = ld16(bin + ti.offset + size_off);
    else if (size_w == 4)
        size = ld32(bin + ti.offset + size_off);
    else
        size = fixed_size;
    if (!in_bounds(ti.offset, size, len)) {
        *flags |= RB_DISC_F_TABLE_BOUNDS;
        return;
    }
    if (byte_sum(bin + ti.offset, size) != ti.checksum)
        *flags |= f_sum;
}

/* ip_discovery_header: signature u32 @0, version u16 @4, size u16 @6,
 * id u32 @8, num_dies u16 @12, die_info[16]{die_id u16, die_offset u16} @14,
 * then a u16 whose bit 0 is base_addr_64_bit (version 4). 80 bytes. */
#define IHDR_SIZE      80u
#define IHDR_DIE_INFO  14u
#define IHDR_FLAGS     78u
#define IP_ENTRY_FIXED 8u   /* hw_id u16, inst u8, nbase u8, major, minor, rev, (sub_rev|variant<<4) */

static int parse_ips(const uint8_t *bin, uint32_t len, uint32_t ihdr_off, struct rb_disc *d)
{
    uint32_t i, j, k;
    d->ip_table_version = ld16(bin + ihdr_off + 4);
    d->num_dies = ld16(bin + ihdr_off + 12);
    d->base_addr_64_bit = (uint8_t)(bin[ihdr_off + IHDR_FLAGS] & 1);
    if (d->ip_table_version < 4)
        d->base_addr_64_bit = 0;   /* the byte is padding before v4 */
    if (d->num_dies == 0 || d->num_dies > 16)
        return RB_DISC_E_DIE;

    for (i = 0; i < d->num_dies; i++) {
        uint32_t die_off = ld16(bin + ihdr_off + IHDR_DIE_INFO + i * 4 + 2);
        uint32_t ip_off, num_ips;
        if (!in_bounds(die_off, 4, len) || ld16(bin + die_off) != i)
            return RB_DISC_E_DIE;
        num_ips = ld16(bin + die_off + 2);
        ip_off = die_off + 4;
        for (j = 0; j < num_ips; j++) {
            uint32_t nbase, stride, entry_size;
            uint16_t hw_id;
            uint8_t inst;
            if (!in_bounds(ip_off, IP_ENTRY_FIXED, len))
                return RB_DISC_E_IP_BOUNDS;
            hw_id = ld16(bin + ip_off);
            inst = bin[ip_off + 2];
            nbase = bin[ip_off + 3];
            stride = d->base_addr_64_bit ? 8u : 4u;
            entry_size = IP_ENTRY_FIXED + nbase * stride;
            if (!in_bounds(ip_off, entry_size, len))
                return RB_DISC_E_IP_BOUNDS;

            /* amdgpu_discovery_validate_ip: HWIP_MAX_INSTANCE 48, HW_ID_MAX
             * 300 (amdgpu_ip.h). Linux skips these entries; so does this
             * parser, flagging it. */
            if (inst >= 48 || hw_id >= 300) {
                d->check_flags |= RB_DISC_F_IP_INVALID;
            } else if (d->nips >= RB_DISC_MAX_IPS) {
                d->check_flags |= RB_DISC_F_IPS_TRUNCATED;
            } else {
                struct rb_ip *ip = &d->ips[d->nips++];
                uint8_t b7 = bin[ip_off + 7];
                ip->hw_id = hw_id;
                ip->die = (uint16_t)i;
                ip->inst = inst;
                ip->nbase = (uint8_t)nbase;
                ip->major = bin[ip_off + 4];
                ip->minor = bin[ip_off + 5];
                ip->revision = bin[ip_off + 6];
                if (d->ip_table_version >= 3) {
                    ip->sub_revision = b7 & 0xf;
                    ip->variant = b7 >> 4;
                } else {
                    ip->harvest = b7 & 0xf;
                }
                if (nbase > RB_DISC_MAX_BASES)
                    d->check_flags |= RB_DISC_F_BASES_TRUNCATED;
                for (k = 0; k < nbase && k < RB_DISC_MAX_BASES; k++) {
                    const uint8_t *p = bin + ip_off + IP_ENTRY_FIXED + k * stride;
                    /* 64-bit: Linux keeps the low 32 bits, top two cleared. */
                    ip->base[k] = d->base_addr_64_bit ? (ld32(p) & 0x3FFFFFFFu) : ld32(p);
                }
            }
            ip_off += entry_size;
        }
    }
    return RB_DISC_OK;
}

static void parse_gc(const uint8_t *bin, uint32_t len, uint16_t major, struct rb_disc *d)
{
    struct tinfo ti;
    uint32_t o;
    if (table_info(bin, len, major, T_GC, &ti) || ti.offset == 0)
        return;
    o = ti.offset;
    /* gpu_info_header: table_id u32, major u16, minor u16, size u32 (12 B),
     * then the u32 fields shared by v1.0 - v1.3. */
    if (!in_bounds(o, 12, len) || ld32(bin + o) != RB_DISC_GC_TABLE_ID)
        return;
    d->gc.version_major = ld16(bin + o + 4);
    d->gc.version_minor = ld16(bin + o + 6);
    if (d->gc.version_major != 1) {
        d->gc.present = 2;
        return;
    }
    if (!in_bounds(o, 12 + RB_GC_V1_FIELDS * 4, len))
        return;
    d->gc.present = 1;
    for (uint32_t i = 0; i < RB_GC_V1_FIELDS; i++)
        d->gc.v1[i] = ld32(bin + o + 12 + i * 4);
}

static void parse_harvest(const uint8_t *bin, uint32_t len, uint16_t major, struct rb_disc *d)
{
    struct tinfo ti;
    if (table_info(bin, len, major, T_HARVEST, &ti) || ti.offset == 0)
        return;
    /* harvest_table: header{signature u32, version u32}, list[32]{hw_id u16, inst u8, pad u8}. */
    if (!in_bounds(ti.offset, 8 + RB_DISC_MAX_HARVEST * 4, len) ||
        ld32(bin + ti.offset) != RB_DISC_HARVEST_SIGNATURE)
        return;
    for (uint32_t i = 0; i < RB_DISC_MAX_HARVEST; i++) {
        const uint8_t *e = bin + ti.offset + 8 + i * 4;
        uint16_t hw_id = ld16(e);
        if (hw_id == 0)
            break;   /* Linux stops at the first empty slot */
        d->harvest[d->nharvest].hw_id = hw_id;
        d->harvest[d->nharvest].inst = e[2];
        d->nharvest++;
    }
}

int rb_disc_parse(const uint8_t *bin, uint32_t len, struct rb_disc *d)
{
    struct tinfo ti;
    uint16_t major;
    int r;

    if (!bin || !d)
        return RB_DISC_E_ARG;
    zero(d, sizeof(*d));
    if (len < BHDR_FIXED)
        return RB_DISC_E_SHORT;
    if (ld32(bin) != RB_DISC_BINARY_SIGNATURE)
        return RB_DISC_E_SIGNATURE;

    major = ld16(bin + 4);
    d->bin_version_major = major;
    d->bin_version_minor = ld16(bin + 6);
    d->bin_size = ld16(bin + 10);
    if (major > 2)
        return RB_DISC_E_VERSION;
    if (d->bin_size < BHDR_FIXED || d->bin_size > len)
        return RB_DISC_E_SIZE;
    len = d->bin_size;   /* nothing past binary_size belongs to the table */

    /* Byte sum of everything after the checksum field, up to binary_size. */
    if (byte_sum(bin + 10, len - 10) != ld16(bin + 8))
        d->check_flags |= RB_DISC_F_BINARY_CHECKSUM;

    if (table_info(bin, len, major, T_IP, &ti) || ti.offset == 0 ||
        !in_bounds(ti.offset, IHDR_SIZE, len))
        return RB_DISC_E_NO_IP_TABLE;
    if (ld32(bin + ti.offset) != RB_DISC_TABLE_SIGNATURE)
        return RB_DISC_E_IP_SIGNATURE;

    check_table(bin, len, major, T_IP, RB_DISC_TABLE_SIGNATURE, 6, 2, 0, 1,
                RB_DISC_F_TABLE_BOUNDS, RB_DISC_F_IP_CHECKSUM, &d->check_flags);
    check_table(bin, len, major, T_GC, RB_DISC_GC_TABLE_ID, 8, 2, 0, 1,
                RB_DISC_F_GC_SIGNATURE, RB_DISC_F_GC_CHECKSUM, &d->check_flags);
    check_table(bin, len, major, T_HARVEST, RB_DISC_HARVEST_SIGNATURE, 0, 0,
                8 + RB_DISC_MAX_HARVEST * 4, 1,
                RB_DISC_F_HARVEST_SIG, RB_DISC_F_HARVEST_CHECKSUM, &d->check_flags);
    check_table(bin, len, major, T_VCN, RB_DISC_VCN_TABLE_ID, 8, 4, 0, 1,
                RB_DISC_F_VCN_SIGNATURE, RB_DISC_F_VCN_CHECKSUM, &d->check_flags);
    check_table(bin, len, major, T_MALL, RB_DISC_MALL_TABLE_ID, 8, 4, 0, 0,
                RB_DISC_F_MALL_SIGNATURE, 0, &d->check_flags);

    r = parse_ips(bin, len, ti.offset, d);
    if (r != RB_DISC_OK)
        return r;
    parse_gc(bin, len, major, d);
    parse_harvest(bin, len, major, d);
    return RB_DISC_OK;
}

const struct rb_ip *rb_disc_find(const struct rb_disc *d, uint16_t hw_id, uint8_t inst)
{
    for (uint32_t i = 0; d && i < d->nips; i++)
        if (d->ips[i].hw_id == hw_id && d->ips[i].inst == inst)
            return &d->ips[i];
    return 0;
}

int rb_disc_reg(const struct rb_disc *d, uint16_t hw_id, uint8_t inst,
                uint8_t base_idx, uint32_t reg, uint32_t *out_dw)
{
    const struct rb_ip *ip = rb_disc_find(d, hw_id, inst);
    if (!ip || base_idx >= ip->nbase || base_idx >= RB_DISC_MAX_BASES || !out_dw)
        return -1;
    *out_dw = ip->base[base_idx] + reg;
    return 0;
}

const char *rb_disc_strerror(int err)
{
    switch (err) {
    case RB_DISC_OK:             return "ok";
    case RB_DISC_E_ARG:          return "bad argument";
    case RB_DISC_E_SHORT:        return "buffer shorter than the binary header";
    case RB_DISC_E_SIGNATURE:    return "bad binary signature";
    case RB_DISC_E_VERSION:      return "unknown binary header version";
    case RB_DISC_E_SIZE:         return "binary_size out of range";
    case RB_DISC_E_NO_IP_TABLE:  return "no IP discovery table";
    case RB_DISC_E_IP_SIGNATURE: return "bad IP discovery table signature";
    case RB_DISC_E_DIE:          return "bad die list";
    case RB_DISC_E_IP_BOUNDS:    return "IP entry out of bounds";
    default:                     return "unknown error";
    }
}

/* soc15_hw_ip.h; the IDs a Rembrandt-class APU or this project cares about. */
const char *rb_hwid_name(uint16_t id)
{
    switch (id) {
    case 1:   return "MP1";      case 2:   return "MP2";
    case 3:   return "THM";      case 4:   return "SMUIO";
    case 5:   return "FUSE";     case 6:   return "CLKA";
    case 10:  return "PWR";      case 11:  return "GC";
    case 12:  return "UVD/VCN";  case 13:  return "AUDIO_AZ";
    case 14:  return "ACP";      case 15:  return "DCI";
    case 16:  return "DCO";      case 17:  return "XDMA";
    case 18:  return "DCEAZ";    case 19:  return "SDPMUX";
    case 20:  return "NTB";      case 21:  return "VPE";
    case 22:  return "UMSCH";    case 24:  return "IOHC";
    case 28:  return "L2IMU";    case 32:  return "VCE";
    case 34:  return "MMHUB";    case 35:  return "ATHUB";
    case 36:  return "DBGU_NBIO"; case 37: return "DFX";
    case 38:  return "DBGU0";    case 39:  return "DBGU1";
    case 40:  return "OSSSYS";   case 41:  return "HDP";
    case 42:  return "SDMA0";    case 43:  return "SDMA1";
    case 44:  return "ISP";      case 45:  return "DBGU_IO";
    case 46:  return "DF";       case 47:  return "CLKB";
    case 48:  return "FCH";      case 49:  return "DFX_DAP";
    case 66:  return "WAFLC";    case 67:  return "FCH_USB_PD";
    case 68:  return "SDMA2";    case 69:  return "SDMA3";
    case 70:  return "PCIE";     case 80:  return "PCS";
    case 89:  return "DDCL";     case 90:  return "SST";
    case 91:  return "LSDMA";    case 100: return "IOAGR";
    case 108: return "NBIF";     case 124: return "IOAPIC";
    case 128: return "SYSTEMHUB"; case 144: return "NTBCCP";
    case 150: return "UMC";      case 168: return "SATA";
    case 170: return "USB";      case 176: return "CCXSEC";
    case 200: return "XGMI";     case 216: return "XGBE";
    case 255: return "MP0";      case 271: return "DMU";
    case 272: return "DIO";      case 274: return "DAZ";
    case 294: return "ATU";      case 295: return "AIGC";
    default:  return "?";
    }
}
```


===== FILE: src/common/rb_probe.c =====

```cpp
/* SPDX-License-Identifier: MIT */
/* Read-only probe of a Rembrandt-class AMD APU. See rb_probe.h. */
#include "rb_probe.h"
#include "rb_regs.h"

static void zero(void *p, uint32_t n)
{
    volatile uint8_t *b = (volatile uint8_t *)p;
    for (uint32_t i = 0; i < n; i++)
        b[i] = 0;
}

/* All register traffic goes through these two. */
static int rreg(const struct rb_hw *hw, uint32_t dw, uint32_t *out)
{
    if (dw >= hw->mmio_len / 4)
        return -1;
    *out = hw->rreg(hw->ctx, dw * 4);
    return 0;
}

/* The write policy: only the MMIO index pair, nothing else, ever. */
static void wreg_index(const struct rb_hw *hw, struct rb_probe_result *r, uint32_t dw, uint32_t v)
{
    if (dw != RB_mmMM_INDEX && dw != RB_mmMM_INDEX_HI) {
        r->refused_writes++;
        return;
    }
    r->index_writes++;
    hw->wreg(hw->ctx, dw * 4, v);
}

/* Read `size` bytes (multiple of 4) of GPU memory at `pos` through
 * MM_INDEX/MM_INDEX_HI/MM_DATA, as amdgpu_device_mm_access does: bit 31 of
 * MM_INDEX selects memory, MM_INDEX_HI holds pos >> 31. */
static void mm_read(const struct rb_hw *hw, struct rb_probe_result *r,
                    uint64_t pos, uint8_t *dst, uint32_t size)
{
    uint32_t hi = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < size; i += 4) {
        uint64_t p = pos + i;
        uint32_t v = 0, tmp = (uint32_t)(p >> 31);
        wreg_index(hw, r, RB_mmMM_INDEX, ((uint32_t)p) | 0x80000000u);
        if (tmp != hi) {
            wreg_index(hw, r, RB_mmMM_INDEX_HI, tmp);
            hi = tmp;
        }
        (void)rreg(hw, RB_mmMM_DATA, &v);
        dst[i] = (uint8_t)v;
        dst[i + 1] = (uint8_t)(v >> 8);
        dst[i + 2] = (uint8_t)(v >> 16);
        dst[i + 3] = (uint8_t)(v >> 24);
    }
}

static uint32_t mm_read32(const struct rb_hw *hw, struct rb_probe_result *r, uint64_t pos)
{
    uint8_t b[4];
    mm_read(hw, r, pos, b, 4);
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

/* Is BAR0 a straight window onto GPU memory from offset 0? Compare the bytes
 * just read through MM_DATA with BAR0 at the same offset. Only reads BAR0, and
 * never its first 16 MB, which the boot framebuffer driver has mapped. */
#define RB_BAR0_KEEP_OUT (16u << 20)

static void bar0_compare(const struct rb_hw *hw, struct rb_probe_result *r)
{
    if (!hw->fb_read || r->disc_offset < RB_BAR0_KEEP_OUT ||
        r->disc_offset > hw->fb_len || r->disc_size > hw->fb_len - r->disc_offset)
        return;
    if (hw->fb_read(hw->ctx, r->disc_offset, r->bar0_bin, r->disc_size))
        return;
    r->bar0_checked = 1;
    r->bar0_matches = 1;
    for (uint32_t i = 0; i < r->disc_size; i++)
        if (r->bar0_bin[i] != r->disc_bin[i]) {
            r->bar0_matches = 0;
            return;
        }
}

static void sample_regs(const struct rb_hw *hw, struct rb_probe_result *r)
{
    for (uint32_t i = 0; i < RB_NREG_DEFS && r->nregs < RB_PROBE_MAX_REGS; i++) {
        const struct rb_reg_def *def = &rb_reg_defs[i];
        struct rb_reg_sample *s = &r->regs[r->nregs++];
        const struct rb_ip *ip = rb_disc_find(&r->disc, def->hw_id, 0);
        s->name = def->name;
        if (def->level > r->level) {
            s->status = RB_REG_LEVEL;
            continue;
        }
        if (!ip || rb_disc_reg(&r->disc, def->hw_id, 0, def->base_idx, def->reg, &s->dw)) {
            s->status = RB_REG_NO_IP;
            continue;
        }
        if (ip->major != def->want_major || ip->minor < def->minor_lo || ip->minor > def->minor_hi) {
            s->status = RB_REG_VERSION;
            continue;
        }
        s->status = rreg(hw, s->dw, &s->value) ? RB_REG_OUT_OF_BAR : RB_REG_OK;
    }
}

int rb_probe_run(const struct rb_hw *hw, int level, struct rb_probe_result *r)
{
    uint32_t sig;
    uint64_t def_off = 0;

    if (!r)
        return -1;
    zero(r, sizeof(*r));
    r->level = level;
    if (!hw || !hw->rreg || !hw->wreg) {
        r->fail = "no hardware access";
        return -1;
    }
    /* Everything below reads fixed offsets up to IP_DISCOVERY_VERSION. */
    if (hw->mmio_len / 4 <= RB_mmIP_DISCOVERY_VERSION) {
        r->fail = "register BAR too small";
        return -1;
    }

    (void)rreg(hw, RB_mmRCC_CONFIG_MEMSIZE, &r->memsize_mb);
    (void)rreg(hw, RB_mmDRIVER_SCRATCH_0, &r->scratch[0]);
    (void)rreg(hw, RB_mmDRIVER_SCRATCH_1, &r->scratch[1]);
    (void)rreg(hw, RB_mmDRIVER_SCRATCH_2, &r->scratch[2]);
    (void)rreg(hw, RB_mmMP0_SMN_C2PMSG_33, &r->c2pmsg33);
    (void)rreg(hw, RB_mmIP_DISCOVERY_VERSION, &r->ipd_version);

    if (r->memsize_mb == 0xFFFFFFFFu) {
        r->fail = "registers read all-ones: device not responding";
        return -1;
    }
    if (r->memsize_mb == 0) {
        /* Linux then treats the table as living in system memory (scratch
         * registers or ACPI give the address) and reads it with memremap.
         * MM_DATA would read the wrong thing: stop. */
        r->fail = "carve-out size is 0: table in system memory, not supported";
        return -1;
    }

    (void)rreg(hw, RB_mmMM_INDEX, &r->mm_index_saved);
    (void)rreg(hw, RB_mmMM_INDEX_HI, &r->mm_index_hi_saved);

    /* amdgpu_discovery_get_tmr_info, bare-metal path. */
    r->disc_size = RB_DISC_TMR_SIZE;
    if (r->memsize_mb > (RB_DISC_TMR_OFFSET >> 20)) {
        def_off = ((uint64_t)r->memsize_mb << 20) - RB_DISC_TMR_OFFSET;
        sig = mm_read32(hw, r, def_off);
        if (sig == RB_DISC_BINARY_SIGNATURE) {
            r->disc_source = RB_SRC_DEFAULT;
            r->disc_offset = def_off;
        }
    }
    if (r->disc_source == RB_SRC_NONE) {
        if (r->scratch[2]) {
            r->disc_source = RB_SRC_SCRATCH;
            r->disc_offset = ((uint64_t)r->scratch[1] << 32) | r->scratch[0];
            r->disc_size = r->scratch[2];
        } else {
            r->disc_source = RB_SRC_DEFAULT_NOSIG;
            r->disc_offset = def_off;
        }
    }
    if (r->disc_size > RB_PROBE_BIN_MAX)
        r->disc_size = RB_PROBE_BIN_MAX;
    r->disc_size &= ~3u;
    /* Linux trusts the scratch pointer; this probe only reads inside the
     * carve-out it was told about. */
    if (r->disc_offset > ((uint64_t)r->memsize_mb << 20) ||
        r->disc_size > ((uint64_t)r->memsize_mb << 20) - r->disc_offset) {
        r->fail = "table location lies outside the carve-out: not read";
        r->disc_size = 0;
    }

    if (r->disc_size)
        mm_read(hw, r, r->disc_offset, r->disc_bin, r->disc_size);

    wreg_index(hw, r, RB_mmMM_INDEX_HI, r->mm_index_hi_saved);
    wreg_index(hw, r, RB_mmMM_INDEX, r->mm_index_saved);

    if (!r->disc_size) {
        if (!r->fail)
            r->fail = "table size is 0";
        return -1;
    }
    bar0_compare(hw, r);

    r->parse_err = rb_disc_parse(r->disc_bin, r->disc_size, &r->disc);
    if (r->parse_err != RB_DISC_OK) {
        r->fail = rb_disc_strerror(r->parse_err);
        return -1;
    }
    sample_regs(hw, r);
    return 0;
}
```


===== FILE: src/common/rb_smu.c =====

```cpp
/* SPDX-License-Identifier: MIT */
/* Read-only SMU queries. See rb_smu.h. */
#include "rb_smu.h"

/* MP1_SMN_C2PMSG_66 / _82 / _90, base index 0 (mp_13_0_2_offset.h). */
#define MP1_C2PMSG_66 0x0282
#define MP1_C2PMSG_82 0x0292
#define MP1_C2PMSG_90 0x029a

static void zero(void *p, uint32_t n)
{
    volatile uint8_t *b = (volatile uint8_t *)p;
    for (uint32_t i = 0; i < n; i++)
        b[i] = 0;
}

static uint32_t rd(const struct rb_hw *hw, uint32_t dw)
{
    return hw->rreg(hw->ctx, dw * 4);
}

/* The write policy: the three mailbox registers, nothing else. */
static void wr(const struct rb_hw *hw, struct rb_smu_result *r, uint32_t dw, uint32_t v)
{
    if (dw != r->dw_msg && dw != r->dw_arg && dw != r->dw_resp) {
        r->refused_writes++;
        return;
    }
    r->writes++;
    hw->wreg(hw->ctx, dw * 4, v);
}

/* __smu_msg_v1_poll_stat: until the response is non-zero or time runs out. */
static uint32_t poll_resp(const struct rb_hw *hw, const struct rb_smu_result *r,
                          uint32_t timeout_us, uint32_t *polls)
{
    uint32_t v = 0, n;
    for (n = 0; n < timeout_us; n++) {
        v = rd(hw, r->dw_resp);
        if (v != RB_SMU_RESP_NONE)
            break;
        hw->delay_us(hw->ctx, 1);
    }
    if (polls)
        *polls = n;
    return v;
}

static int known_resp(uint32_t v)
{
    return v == RB_SMU_RESP_OK || v == RB_SMU_RESP_FAILED || v == RB_SMU_RESP_UNKNOWN_CMD ||
           v == RB_SMU_RESP_REJ_PREREQ || v == RB_SMU_RESP_REJ_BUSY;
}

/* One message; returns 0 if it came back OK. */
static int xfer(const struct rb_hw *hw, struct rb_smu_result *r, uint32_t timeout_us,
                uint8_t msg, uint32_t arg, uint32_t *out)
{
    struct rb_smu_xfer *x = &r->x[r->nxfers++];
    uint32_t pre;
    x->msg = msg;
    x->arg = arg;

    pre = poll_resp(hw, r, timeout_us, 0);
    if (pre == RB_SMU_RESP_NONE || !known_resp(pre)) {
        x->status = RB_SMU_X_PRE_BUSY;
        x->resp = pre;
        r->fail = "SMU mailbox busy or in an unknown state before sending";
        return -1;
    }

    wr(hw, r, r->dw_resp, 0);
    wr(hw, r, r->dw_arg, arg);
    wr(hw, r, r->dw_msg, msg);

    x->resp = poll_resp(hw, r, timeout_us, &x->polls);
    if (x->resp == RB_SMU_RESP_NONE) {
        x->status = RB_SMU_X_TIMEOUT;
        r->fail = "SMU did not answer within the timeout";
        return -1;
    }
    if (x->resp != RB_SMU_RESP_OK) {
        x->status = RB_SMU_X_FW_ERROR;
        r->fail = "SMU answered with an error";
        return -1;
    }
    x->out = rd(hw, r->dw_arg);
    *out = x->out;
    return 0;
}

int rb_smu_query(const struct rb_hw *hw, const struct rb_disc *d,
                 uint32_t timeout_us, struct rb_smu_result *r)
{
    const struct rb_ip *mp1;
    uint32_t lo = 0, hi = 0;

    if (!r)
        return -1;
    zero(r, sizeof(*r));
    if (!hw || !hw->rreg || !hw->wreg || !hw->delay_us || !d) {
        r->fail = "no hardware access";
        return -1;
    }
    if (!timeout_us)
        timeout_us = RB_SMU_TIMEOUT_US;

    mp1 = rb_disc_find(d, RB_HWID_MP1, 0);
    if (!mp1 || mp1->nbase < 1) {
        r->fail = "no MP1 in the discovery table";
        return -1;
    }
    r->mp1_major = mp1->major;
    r->mp1_minor = mp1->minor;
    r->mp1_rev = mp1->revision;
    if (mp1->major != 13) {
        r->fail = "MP1 is not version 13: mailbox layout unknown";
        return -1;
    }
    r->dw_msg = mp1->base[0] + MP1_C2PMSG_66;
    r->dw_arg = mp1->base[0] + MP1_C2PMSG_82;
    r->dw_resp = mp1->base[0] + MP1_C2PMSG_90;
    if (r->dw_msg >= hw->mmio_len / 4 || r->dw_arg >= hw->mmio_len / 4 ||
        r->dw_resp >= hw->mmio_len / 4) {
        r->fail = "SMU mailbox outside the register BAR";
        return -1;
    }

    r->initial_msg = rd(hw, r->dw_msg);
    r->initial_arg = rd(hw, r->dw_arg);
    r->initial_resp = rd(hw, r->dw_resp);
    if (r->initial_resp != RB_SMU_RESP_OK) {
        r->fail = "SMU mailbox not idle (response is not 1): nothing sent";
        return -1;
    }

    if (xfer(hw, r, timeout_us, RB_SMU_MSG_GetSmuVersion, 0, &r->smu_version))
        return -1;
    r->have_version = 1;
    if (xfer(hw, r, timeout_us, RB_SMU_MSG_GetDriverIfVersion, 0, &r->if_version))
        return -1;
    r->have_if_version = 1;
    /* smu_cmn_get_enabled_mask: argument 0 low word, 1 high word. */
    if (xfer(hw, r, timeout_us, RB_SMU_MSG_GetEnabledSmuFeatures, 0, &lo))
        return -1;
    if (xfer(hw, r, timeout_us, RB_SMU_MSG_GetEnabledSmuFeatures, 1, &hi))
        return -1;
    r->features = ((uint64_t)hi << 32) | lo;
    r->have_features = 1;
    return 0;
}
```


===== FILE: src/common/rb_smu13.c =====

```cpp
/* SPDX-License-Identifier: MIT */
/* SMU 13 power management for Rembrandt. See rb_smu13.h. */
#include "rb_smu13.h"

/* MP1_SMN_C2PMSG_66 / _82 / _90, base index 0 (mp_13_0_2_offset.h). */
#define MP1_C2PMSG_66 0x0282
#define MP1_C2PMSG_82 0x0292
#define MP1_C2PMSG_90 0x029a

/* yellow_carp_ppt.c: regSMUIO_GFX_MISC_CNTL, base index 0. */
#define RB_HWID_SMUIO 4                                   /* soc15_hw_ip.h */
#define SMUIO_GFX_MISC_CNTL 0x00c5
#define SMUIO_GFX_MISC_CNTL__PWR_GFXOFF_STATUS_MASK  0x6u
#define SMUIO_GFX_MISC_CNTL__PWR_GFXOFF_STATUS_SHIFT 1

#define SMU_13_VCLK_SHIFT 16                              /* smu_v13_0.h */
#define TEMP_UNITS_PER_CENTIGRADE 1000                    /* SMU_TEMPERATURE_UNITS_PER_CENTIGRADES */

/* Linux: adev->usec_timeout (100000) * 20, polled at 1 us. */
#define DEFAULT_TIMEOUT_US 2000000u

#define Q RB_SMU13_ALLOW_QUERY
#define T RB_SMU13_ALLOW_TABLES
#define G RB_SMU13_ALLOW_GFXOFF
#define C RB_SMU13_ALLOW_CLOCKS
#define P RB_SMU13_ALLOW_MMPOWER
static const uint8_t msg_group[RB_SMU13_MSG_COUNT] = {
    [RB_SMU13_MSG_TestMessage]             = Q,
    [RB_SMU13_MSG_GetSmuVersion]           = Q,
    [RB_SMU13_MSG_GetDriverIfVersion]      = Q,
    [RB_SMU13_MSG_GetEnabledSmuFeatures]   = Q,
    [RB_SMU13_MSG_GetGfxclkFrequency]      = Q,
    [RB_SMU13_MSG_GetFclkFrequency]        = Q,
    [RB_SMU13_MSG_SetDriverDramAddrHigh]   = T,
    [RB_SMU13_MSG_SetDriverDramAddrLow]    = T,
    [RB_SMU13_MSG_TransferTableSmu2Dram]   = T,
    [RB_SMU13_MSG_TransferTableDram2Smu]   = T,
    [RB_SMU13_MSG_EnableGfxOff]            = G,
    [RB_SMU13_MSG_AllowGfxOff]             = G,
    [RB_SMU13_MSG_DisallowGfxOff]          = G,
    [RB_SMU13_MSG_SetHardMinVcn]           = C,
    [RB_SMU13_MSG_SetHardMinSocclkByFreq]  = C,
    [RB_SMU13_MSG_SetSoftMinVcn]           = C,
    [RB_SMU13_MSG_SetSoftMaxGfxClk]        = C,
    [RB_SMU13_MSG_SetHardMinGfxClk]        = C,
    [RB_SMU13_MSG_SetSoftMaxSocclkByFreq]  = C,
    [RB_SMU13_MSG_SetSoftMaxFclkByFreq]    = C,
    [RB_SMU13_MSG_SetSoftMaxVcn]           = C,
    [RB_SMU13_MSG_SetPowerLimitPercentage] = C,
    [RB_SMU13_MSG_SetHardMinFclkByFreq]    = C,
    [RB_SMU13_MSG_SetSoftMinSocclkByFreq]  = C,
    [RB_SMU13_MSG_PowerDownVcn]            = P,
    [RB_SMU13_MSG_PowerUpVcn]              = P,
    [RB_SMU13_MSG_PowerDownJpeg]           = P,
    [RB_SMU13_MSG_PowerUpJpeg]             = P,
    [RB_SMU13_MSG_PrepareMp1ForUnload]     = RB_SMU13_ALLOW_UNLOAD,
    [RB_SMU13_MSG_GfxDeviceDriverReset]    = RB_SMU13_ALLOW_RESET,
};
#undef Q
#undef T
#undef G
#undef C
#undef P

uint32_t rb_smu13_msg_group(uint8_t msg)
{
    return msg < RB_SMU13_MSG_COUNT ? msg_group[msg] : 0;
}

static uint32_t table_size(uint32_t id)
{
    switch (id) {
    case RB_SMU13_TABLE_WATERMARKS:  return sizeof(struct rb_smu13_watermarks);
    case RB_SMU13_TABLE_DPMCLOCKS:   return sizeof(struct rb_smu13_dpm_clocks);
    case RB_SMU13_TABLE_SMU_METRICS: return sizeof(struct rb_smu13_metrics);
    default:                         return 0;
    }
}

uint32_t rb_smu13_table_size_max(void)
{
    uint32_t a = table_size(RB_SMU13_TABLE_WATERMARKS), b = table_size(RB_SMU13_TABLE_DPMCLOCKS),
             c = table_size(RB_SMU13_TABLE_SMU_METRICS), m = a;
    if (b > m) m = b;
    if (c > m) m = c;
    return m;
}

static void zero(void *p, uint32_t n)
{
    volatile uint8_t *b = (volatile uint8_t *)p;
    for (uint32_t i = 0; i < n; i++)
        b[i] = 0;
}

static void copy(void *dst, const void *src, uint32_t n)
{
    volatile uint8_t *d = (volatile uint8_t *)dst;
    const volatile uint8_t *s = (const volatile uint8_t *)src;
    for (uint32_t i = 0; i < n; i++)
        d[i] = s[i];
}

static uint32_t rd(const struct rb_smu13 *s, uint32_t dw)
{
    return s->hw->rreg(s->hw->ctx, dw * 4);
}

/* The write policy: the three mailbox registers, nothing else. */
static void wr(struct rb_smu13 *s, uint32_t dw, uint32_t v)
{
    if (dw != s->dw_msg && dw != s->dw_arg && dw != s->dw_resp) {
        s->refused_writes++;
        return;
    }
    s->writes++;
    s->hw->wreg(s->hw->ctx, dw * 4, v);
}

/* __smu_msg_v1_poll_stat */
static uint32_t poll_resp(const struct rb_smu13 *s, uint32_t *polls)
{
    uint32_t v = 0, n;
    for (n = 0; n < s->timeout_us; n++) {
        v = rd(s, s->dw_resp);
        if (v != RB_SMU13_RESP_NONE)
            break;
        s->hw->delay_us(s->hw->ctx, 1);
    }
    if (polls)
        *polls = n;
    return v;
}

static int known_resp(uint32_t v)
{
    return v == RB_SMU13_RESP_OK || v == RB_SMU13_RESP_FAILED || v == RB_SMU13_RESP_UNKNOWN_CMD ||
           v == RB_SMU13_RESP_REJ_PREREQ || v == RB_SMU13_RESP_REJ_BUSY;
}

int rb_smu13_init(struct rb_smu13 *s, const struct rb_hw *hw, const struct rb_disc *d,
                  const struct rb_smu13_dma *dma, uint32_t timeout_us)
{
    const struct rb_ip *mp1;
    uint32_t dw;

    if (!s)
        return RB_SMU13_E_INVAL;
    zero(s, sizeof(*s));
    s->dead = 1;                                   /* until everything checks out */
    if (!hw || !hw->rreg || !hw->wreg || !hw->delay_us || !d)
        return RB_SMU13_E_INVAL;
    s->hw = hw;
    s->timeout_us = timeout_us ? timeout_us : DEFAULT_TIMEOUT_US;

    mp1 = rb_disc_find(d, RB_HWID_MP1, 0);
    if (!mp1 || mp1->nbase < 1)
        return RB_SMU13_E_VERSION;
    s->mp1_major = mp1->major;
    s->mp1_minor = mp1->minor;
    s->mp1_rev = mp1->revision;
    /* smu_set_funcs: yellow_carp_set_ppt_funcs for 13.0.1, 13.0.3, 13.0.8. */
    if (mp1->major != 13 || mp1->minor != 0 ||
        (mp1->revision != 1 && mp1->revision != 3 && mp1->revision != 8))
        return RB_SMU13_E_VERSION;

    s->dw_msg = mp1->base[0] + MP1_C2PMSG_66;
    s->dw_arg = mp1->base[0] + MP1_C2PMSG_82;
    s->dw_resp = mp1->base[0] + MP1_C2PMSG_90;
    if (s->dw_msg >= hw->mmio_len / 4 || s->dw_arg >= hw->mmio_len / 4 ||
        s->dw_resp >= hw->mmio_len / 4)
        return RB_SMU13_E_INVAL;
    if (rb_disc_reg(d, RB_HWID_SMUIO, 0, 0, SMUIO_GFX_MISC_CNTL, &dw) == 0 && dw < hw->mmio_len / 4)
        s->dw_gfx_misc_cntl = dw;

    if (dma) {
        if (!dma->cpu || !dma->mc || dma->size < rb_smu13_table_size_max())
            return RB_SMU13_E_INVAL;
        copy(&s->dma, dma, sizeof(s->dma));
    }
    s->allowed = RB_SMU13_ALLOW_QUERY;
    s->dead = 0;
    return RB_SMU13_OK;
}

void rb_smu13_allow(struct rb_smu13 *s, uint32_t groups)
{
    if (s)
        s->allowed |= groups;
}

int rb_smu13_send(struct rb_smu13 *s, uint8_t msg, uint32_t arg, uint32_t *out)
{
    uint32_t pre, group;

    if (!s || !s->hw)
        return RB_SMU13_E_INVAL;
    if (s->dead)
        return RB_SMU13_E_DEAD;
    group = rb_smu13_msg_group(msg);
    if (!group)
        return RB_SMU13_E_INVAL;
    if (!(s->allowed & group))
        return RB_SMU13_E_NOT_ALLOWED;

    s->last_msg = msg;
    s->last_arg = arg;
    s->last_polls = 0;

    pre = poll_resp(s, 0);
    s->last_resp = pre;
    if (pre == RB_SMU13_RESP_NONE) {
        s->dead = 1;                  /* the previous message never finished */
        return RB_SMU13_E_BUSY;
    }
    if (!known_resp(pre))
        return RB_SMU13_E_BUSY;

    wr(s, s->dw_resp, 0);
    wr(s, s->dw_arg, arg);
    wr(s, s->dw_msg, msg);
    s->nmsgs++;

    s->last_resp = poll_resp(s, &s->last_polls);
    if (s->last_resp == RB_SMU13_RESP_NONE) {
        s->dead = 1;
        return RB_SMU13_E_TIMEOUT;
    }
    if (s->last_resp != RB_SMU13_RESP_OK)
        return RB_SMU13_E_FW;
    if (out)
        *out = rd(s, s->dw_arg);
    return RB_SMU13_OK;
}

/* ------------------------------------------------------ versions, features */

int rb_smu13_check_fw_version(struct rb_smu13 *s)
{
    uint32_t ifv = 0, ver = 0;
    int e = rb_smu13_send(s, RB_SMU13_MSG_GetDriverIfVersion, 0, &ifv);
    if (e)
        return e;
    e = rb_smu13_send(s, RB_SMU13_MSG_GetSmuVersion, 0, &ver);
    if (e)
        return e;
    s->if_version = ifv;
    s->smu_version = ver;
    s->have_versions = 1;
    /* Linux only logs a mismatch; the table layouts here depend on it. */
    return ifv == RB_SMU13_DRIVER_IF_VERSION ? RB_SMU13_OK : RB_SMU13_E_VERSION;
}

int rb_smu13_get_enabled_mask(struct rb_smu13 *s, uint64_t *mask)
{
    uint32_t lo = 0, hi = 0;
    int e = rb_smu13_send(s, RB_SMU13_MSG_GetEnabledSmuFeatures, 0, &lo);
    if (e)
        return e;
    e = rb_smu13_send(s, RB_SMU13_MSG_GetEnabledSmuFeatures, 1, &hi);
    if (e)
        return e;
    s->features = ((uint64_t)hi << 32) | lo;
    s->have_features = 1;
    if (mask)
        *mask = s->features;
    return RB_SMU13_OK;
}

int rb_smu13_is_dpm_running(struct rb_smu13 *s)
{
    uint64_t m;
    int e = rb_smu13_get_enabled_mask(s, &m);
    if (e)
        return e;
    return (m & RB_SMU13_DPM_FEATURES) == RB_SMU13_DPM_FEATURES;
}

/* --------------------------------------------------------------- tables */

int rb_smu13_set_driver_table_location(struct rb_smu13 *s)
{
    int e;
    if (!s || !s->dma.cpu)
        return RB_SMU13_E_INVAL;
    e = rb_smu13_send(s, RB_SMU13_MSG_SetDriverDramAddrHigh, (uint32_t)(s->dma.mc >> 32), 0);
    if (!e)
        e = rb_smu13_send(s, RB_SMU13_MSG_SetDriverDramAddrLow, (uint32_t)s->dma.mc, 0);
    s->table_location_set = e == RB_SMU13_OK;
    return e;
}

int rb_smu13_update_table(struct rb_smu13 *s, uint32_t table_id, uint16_t arg,
                          void *data, uint32_t size, int drv2smu)
{
    uint32_t want = table_size(table_id), param;
    int e;

    if (!s || !data || !want || size != want)
        return RB_SMU13_E_INVAL;
    if (!s->dma.cpu || want > s->dma.size)
        return RB_SMU13_E_INVAL;
    if (!s->table_location_set)
        return RB_SMU13_E_NODATA;
    if (s->dead)
        return RB_SMU13_E_DEAD;
    if (!(s->allowed & RB_SMU13_ALLOW_TABLES))
        return RB_SMU13_E_NOT_ALLOWED;

    if (drv2smu) {
        copy(s->dma.cpu, data, want);
        if (s->dma.flush)
            s->dma.flush(s->hw->ctx);
    }
    param = ((uint32_t)arg << 16) | (table_id & 0xFFFF);
    e = rb_smu13_send(s, drv2smu ? RB_SMU13_MSG_TransferTableDram2Smu : RB_SMU13_MSG_TransferTableSmu2Dram,
                      param, 0);
    if (e)
        return e;
    if (!drv2smu) {
        if (s->dma.invalidate)
            s->dma.invalidate(s->hw->ctx);
        copy(data, s->dma.cpu, want);
    }
    return RB_SMU13_OK;
}

int rb_smu13_set_default_dpm_table(struct rb_smu13 *s)
{
    const struct rb_smu13_dpm_clocks *c;
    int e;

    if (!s)
        return RB_SMU13_E_INVAL;
    /* Read in place (no 300-byte stack copy); have_clocks gates every use. */
    s->have_clocks = 0;
    e = rb_smu13_update_table(s, RB_SMU13_TABLE_DPMCLOCKS, 0, &s->clocks, sizeof(s->clocks), 0);
    if (e)
        return e;
    c = &s->clocks;
    /* Linux indexes the arrays with these counts unchecked; refuse a table
     * whose counts would run past them. */
    if (c->num_dcf_levels > RB_SMU13_NUM_DPM_LEVELS || c->num_disp_levels > RB_SMU13_NUM_DPM_LEVELS ||
        c->num_soc_levels > RB_SMU13_NUM_DPM_LEVELS || c->num_vcn_levels > RB_SMU13_NUM_DPM_LEVELS ||
        c->num_df_pstates > RB_SMU13_NUM_DF_PSTATES)
        return RB_SMU13_E_NODATA;
    s->have_clocks = 1;
    /* yellow_carp_set_fine_grain_gfx_freq_parameters */
    s->gfx_default_hard_min = c->min_gfx_clk;
    s->gfx_default_soft_max = c->max_gfx_clk;
    s->gfx_actual_hard_min = 0;
    s->gfx_actual_soft_max = 0;
    return RB_SMU13_OK;
}

int rb_smu13_get_metrics(struct rb_smu13 *s, struct rb_smu13_metrics *m)
{
    return rb_smu13_update_table(s, RB_SMU13_TABLE_SMU_METRICS, 0, m, sizeof(*m), 0);
}

/* yellow_carp_get_ss_power_percent, with Linux's 16-bit intermediates. */
static void ss_power_percent(const struct rb_smu13_metrics *m, uint32_t *apu, uint32_t *dgpu)
{
    uint32_t apu_boost = 0, dgpu_boost = 0;
    uint16_t apu_limit, dgpu_limit = 0, apu_power, dgpu_power;

    apu_power = (uint16_t)(m->apu_power / 1000);
    apu_limit = m->stapm_opn_limit;
    if (apu_power > apu_limit && apu_limit != 0)
        apu_boost = ((uint32_t)(apu_power - apu_limit) * 100) / apu_limit;
    apu_boost = apu_boost > 100 ? 100 : apu_boost;

    dgpu_power = (uint16_t)(m->dgpu_power / 1000);
    if (m->stapm_current_limit > m->stapm_opn_limit)
        dgpu_limit = (uint16_t)(m->stapm_current_limit - m->stapm_opn_limit);
    if (dgpu_power > dgpu_limit && dgpu_limit != 0)
        dgpu_boost = ((uint32_t)(dgpu_power - dgpu_limit) * 100) / dgpu_limit;
    dgpu_boost = dgpu_boost > 100 ? 100 : dgpu_boost;

    if (dgpu_boost >= apu_boost)
        apu_boost = 0;
    else
        dgpu_boost = 0;
    *apu = apu_boost;
    *dgpu = dgpu_boost;
}

int rb_smu13_read_sensor(struct rb_smu13 *s, enum rb_smu13_sensor sensor, uint32_t *value)
{
    struct rb_smu13_metrics m;
    uint32_t apu, dgpu;
    int e;

    if (!value)
        return RB_SMU13_E_INVAL;
    e = rb_smu13_get_metrics(s, &m);
    if (e)
        return e;
    switch (sensor) {
    case RB_SMU13_SENSOR_GPU_LOAD:      *value = m.gfx_activity / 100u; break;
    case RB_SMU13_SENSOR_VCN_LOAD:      *value = m.uvd_activity / 100u; break;
    case RB_SMU13_SENSOR_INPUT_POWER:   *value = m.current_socket_power; break;
    case RB_SMU13_SENSOR_EDGE_TEMP:     *value = m.gfx_temp / 100u * TEMP_UNITS_PER_CENTIGRADE; break;
    case RB_SMU13_SENSOR_HOTSPOT_TEMP:  *value = m.soc_temp / 100u * TEMP_UNITS_PER_CENTIGRADE; break;
    case RB_SMU13_SENSOR_MCLK:          *value = (uint32_t)m.memclk * 100u; break;
    case RB_SMU13_SENSOR_SCLK:          *value = (uint32_t)m.gfxclk * 100u; break;
    case RB_SMU13_SENSOR_VDDGFX:        *value = m.voltage[0]; break;
    case RB_SMU13_SENSOR_VDDNB:         *value = m.voltage[1]; break;
    case RB_SMU13_SENSOR_SS_APU_SHARE:  ss_power_percent(&m, &apu, &dgpu); *value = apu; break;
    case RB_SMU13_SENSOR_SS_DGPU_SHARE: ss_power_percent(&m, &apu, &dgpu); *value = dgpu; break;
    default:                            return RB_SMU13_E_INVAL;
    }
    return RB_SMU13_OK;
}

int rb_smu13_set_watermarks(struct rb_smu13 *s,
                            const struct rb_smu13_wm_set *reader, uint32_t nreader,
                            const struct rb_smu13_wm_set *writer, uint32_t nwriter)
{
    struct rb_smu13_watermarks t;
    enum { WM_SOCCLK = 0, WM_DCFCLK = 1 };

    if (!s || nreader > RB_SMU13_NUM_WM_RANGES || nwriter > RB_SMU13_NUM_WM_RANGES ||
        (nreader && !reader) || (nwriter && !writer))
        return RB_SMU13_E_INVAL;
    zero(&t, sizeof(t));
    for (uint32_t i = 0; i < nreader; i++) {
        t.row[WM_DCFCLK][i].min_clock = reader[i].min_drain_mhz;
        t.row[WM_DCFCLK][i].max_clock = reader[i].max_drain_mhz;
        t.row[WM_DCFCLK][i].min_mclk = reader[i].min_fill_mhz;
        t.row[WM_DCFCLK][i].max_mclk = reader[i].max_fill_mhz;
        t.row[WM_DCFCLK][i].wm_setting = reader[i].wm_inst;
    }
    for (uint32_t i = 0; i < nwriter; i++) {
        t.row[WM_SOCCLK][i].min_clock = writer[i].min_fill_mhz;
        t.row[WM_SOCCLK][i].max_clock = writer[i].max_fill_mhz;
        t.row[WM_SOCCLK][i].min_mclk = writer[i].min_drain_mhz;
        t.row[WM_SOCCLK][i].max_mclk = writer[i].max_drain_mhz;
        t.row[WM_SOCCLK][i].wm_setting = writer[i].wm_inst;
    }
    return rb_smu13_update_table(s, RB_SMU13_TABLE_WATERMARKS, 0, &t, sizeof(t), 1);
}

/* ------------------------------------------------------- power control */

int rb_smu13_enable_gfxoff(struct rb_smu13 *s)
{
    return rb_smu13_send(s, RB_SMU13_MSG_EnableGfxOff, 0, 0);
}

int rb_smu13_gfx_off_control(struct rb_smu13 *s, int enable)
{
    return rb_smu13_send(s, enable ? RB_SMU13_MSG_AllowGfxOff : RB_SMU13_MSG_DisallowGfxOff, 0, 0);
}

int rb_smu13_get_gfxoff_status(struct rb_smu13 *s, uint32_t *status)
{
    if (!s || !s->hw || !status || !s->dw_gfx_misc_cntl)
        return RB_SMU13_E_INVAL;
    *status = (rd(s, s->dw_gfx_misc_cntl) & SMUIO_GFX_MISC_CNTL__PWR_GFXOFF_STATUS_MASK) >>
              SMUIO_GFX_MISC_CNTL__PWR_GFXOFF_STATUS_SHIFT;
    return RB_SMU13_OK;
}

int rb_smu13_set_vcn_power(struct rb_smu13 *s, int on)
{
    return rb_smu13_send(s, on ? RB_SMU13_MSG_PowerUpVcn : RB_SMU13_MSG_PowerDownVcn, 0, 0);
}

int rb_smu13_set_jpeg_power(struct rb_smu13 *s, int on)
{
    return rb_smu13_send(s, on ? RB_SMU13_MSG_PowerUpJpeg : RB_SMU13_MSG_PowerDownJpeg, 0, 0);
}

int rb_smu13_prepare_unload(struct rb_smu13 *s)
{
    return rb_smu13_send(s, RB_SMU13_MSG_PrepareMp1ForUnload, 0, 0);
}

int rb_smu13_mode2_reset(struct rb_smu13 *s)
{
    return rb_smu13_send(s, RB_SMU13_MSG_GfxDeviceDriverReset, RB_SMU13_RESET_MODE_2, 0);
}

/* --------------------------------------------------------------- clocks */

/* yellow_carp_clk_dpm_is_enabled: 1/0, or <0 without a feature mask. */
static int clk_dpm_enabled(const struct rb_smu13 *s, enum rb_smu13_clk clk)
{
    uint32_t bit;
    if (!s->have_features)
        return RB_SMU13_E_NODATA;
    switch (clk) {
    case RB_SMU13_CLK_MCLK:
    case RB_SMU13_CLK_FCLK: bit = RB_SMU13_FEATURE_FCLK_DPM; break;
    case RB_SMU13_CLK_GFX:  bit = RB_SMU13_FEATURE_GFX_DPM; break;
    case RB_SMU13_CLK_SOC:  bit = RB_SMU13_FEATURE_SOCCLK_DPM; break;
    case RB_SMU13_CLK_VCLK:
    case RB_SMU13_CLK_DCLK: bit = RB_SMU13_FEATURE_VCN_DPM; break;
    default:                return 1;
    }
    return (int)((s->features >> bit) & 1);
}

int rb_smu13_get_dpm_level_count(struct rb_smu13 *s, enum rb_smu13_clk clk, uint32_t *count)
{
    if (!s || !count)
        return RB_SMU13_E_INVAL;
    if (!s->have_clocks)
        return RB_SMU13_E_NODATA;
    switch (clk) {
    case RB_SMU13_CLK_SOC:  *count = s->clocks.num_soc_levels; break;
    case RB_SMU13_CLK_VCLK:
    case RB_SMU13_CLK_DCLK: *count = s->clocks.num_vcn_levels; break;
    case RB_SMU13_CLK_MCLK:
    case RB_SMU13_CLK_FCLK: *count = s->clocks.num_df_pstates; break;
    default:                return RB_SMU13_E_INVAL;
    }
    return RB_SMU13_OK;
}

int rb_smu13_get_dpm_freq_by_index(struct rb_smu13 *s, enum rb_smu13_clk clk,
                                   uint32_t level, uint32_t *mhz)
{
    const struct rb_smu13_dpm_clocks *c;
    if (!s || !mhz)
        return RB_SMU13_E_INVAL;
    if (!s->have_clocks)
        return RB_SMU13_E_NODATA;
    c = &s->clocks;
    switch (clk) {
    case RB_SMU13_CLK_SOC:
        if (level >= c->num_soc_levels) return RB_SMU13_E_INVAL;
        *mhz = c->soc_clocks[level];
        break;
    case RB_SMU13_CLK_VCLK:
        if (level >= c->num_vcn_levels) return RB_SMU13_E_INVAL;
        *mhz = c->vclocks[level];
        break;
    case RB_SMU13_CLK_DCLK:
        if (level >= c->num_vcn_levels) return RB_SMU13_E_INVAL;
        *mhz = c->dclocks[level];
        break;
    case RB_SMU13_CLK_MCLK:
        if (level >= c->num_df_pstates) return RB_SMU13_E_INVAL;
        *mhz = c->df_pstate[level].memclk;
        break;
    case RB_SMU13_CLK_FCLK:
        if (level >= c->num_df_pstates) return RB_SMU13_E_INVAL;
        *mhz = c->df_pstate[level].fclk;
        break;
    default:
        return RB_SMU13_E_INVAL;
    }
    return RB_SMU13_OK;
}

int rb_smu13_get_current_clk(struct rb_smu13 *s, enum rb_smu13_clk clk, uint32_t *mhz)
{
    struct rb_smu13_metrics m;
    int e;
    if (!mhz)
        return RB_SMU13_E_INVAL;
    switch (clk) {
    case RB_SMU13_CLK_FCLK: return rb_smu13_send(s, RB_SMU13_MSG_GetFclkFrequency, 0, mhz);
    case RB_SMU13_CLK_GFX:  return rb_smu13_send(s, RB_SMU13_MSG_GetGfxclkFrequency, 0, mhz);
    case RB_SMU13_CLK_SOC:
    case RB_SMU13_CLK_VCLK:
    case RB_SMU13_CLK_DCLK:
    case RB_SMU13_CLK_MCLK:
        break;
    default:
        return RB_SMU13_E_INVAL;
    }
    e = rb_smu13_get_metrics(s, &m);
    if (e)
        return e;
    *mhz = clk == RB_SMU13_CLK_SOC ? m.socclk : clk == RB_SMU13_CLK_VCLK ? m.vclk :
           clk == RB_SMU13_CLK_DCLK ? m.dclk : m.memclk;
    return RB_SMU13_OK;
}

int rb_smu13_get_dpm_ultimate_freq(struct rb_smu13 *s, enum rb_smu13_clk clk,
                                   uint32_t *min, uint32_t *max)
{
    const struct rb_smu13_dpm_clocks *c;
    uint32_t max_level = 0, min_level = 0;
    int en, e;

    if (!s)
        return RB_SMU13_E_INVAL;
    en = clk_dpm_enabled(s, clk);
    if (en < 0)
        return en;
    if (!en)
        return RB_SMU13_E_NODATA;    /* Linux: VBIOS boot value */
    if (!s->have_clocks)
        return RB_SMU13_E_NODATA;
    c = &s->clocks;

    if (max) {
        switch (clk) {
        case RB_SMU13_CLK_GFX:  *max = c->max_gfx_clk; break;
        case RB_SMU13_CLK_MCLK:
        case RB_SMU13_CLK_FCLK: max_level = 0; break;
        case RB_SMU13_CLK_SOC:  max_level = (uint32_t)c->num_soc_levels - 1; break;
        case RB_SMU13_CLK_VCLK:
        case RB_SMU13_CLK_DCLK: max_level = (uint32_t)c->num_vcn_levels - 1; break;
        default:                return RB_SMU13_E_INVAL;
        }
        if (clk != RB_SMU13_CLK_GFX) {
            e = rb_smu13_get_dpm_freq_by_index(s, clk, max_level, max);
            if (e)
                return e;
        }
    }
    if (min) {
        switch (clk) {
        case RB_SMU13_CLK_GFX:  *min = c->min_gfx_clk; break;
        case RB_SMU13_CLK_MCLK:
        case RB_SMU13_CLK_FCLK: min_level = (uint32_t)c->num_df_pstates - 1; break;
        case RB_SMU13_CLK_SOC:
        case RB_SMU13_CLK_VCLK:
        case RB_SMU13_CLK_DCLK: min_level = 0; break;
        default:                return RB_SMU13_E_INVAL;
        }
        if (clk != RB_SMU13_CLK_GFX) {
            e = rb_smu13_get_dpm_freq_by_index(s, clk, min_level, min);
            if (e)
                return e;
        }
    }
    return RB_SMU13_OK;
}

int rb_smu13_set_soft_freq_range(struct rb_smu13 *s, enum rb_smu13_clk clk,
                                 uint32_t min, uint32_t max)
{
    uint8_t msg_min, msg_max;
    int en, e;

    if (!s)
        return RB_SMU13_E_INVAL;
    en = clk_dpm_enabled(s, clk);
    if (en < 0)
        return en;
    if (!en)
        return RB_SMU13_E_INVAL;
    switch (clk) {
    case RB_SMU13_CLK_GFX:
        msg_min = RB_SMU13_MSG_SetHardMinGfxClk;
        msg_max = RB_SMU13_MSG_SetSoftMaxGfxClk;
        break;
    case RB_SMU13_CLK_FCLK:
        msg_min = RB_SMU13_MSG_SetHardMinFclkByFreq;
        msg_max = RB_SMU13_MSG_SetSoftMaxFclkByFreq;
        break;
    case RB_SMU13_CLK_SOC:
        msg_min = RB_SMU13_MSG_SetHardMinSocclkByFreq;
        msg_max = RB_SMU13_MSG_SetSoftMaxSocclkByFreq;
        break;
    case RB_SMU13_CLK_VCLK:
    case RB_SMU13_CLK_DCLK:
        msg_min = RB_SMU13_MSG_SetHardMinVcn;
        msg_max = RB_SMU13_MSG_SetSoftMaxVcn;
        break;
    default:
        return RB_SMU13_E_INVAL;
    }
    /* As Linux: only VCLK is shifted into the upper half. */
    if (clk == RB_SMU13_CLK_VCLK) {
        min <<= SMU_13_VCLK_SHIFT;
        max <<= SMU_13_VCLK_SHIFT;
    }
    e = rb_smu13_send(s, msg_min, min, 0);
    if (e)
        return e;
    return rb_smu13_send(s, msg_max, max, 0);
}

static uint32_t lowest_bit(uint32_t m)  { uint32_t i = 0; while (!(m & 1)) { m >>= 1; i++; } return i; }
static uint32_t highest_bit(uint32_t m) { uint32_t i = 0; while (m >>= 1) i++; return i; }

int rb_smu13_force_clk_levels(struct rb_smu13 *s, enum rb_smu13_clk clk, uint32_t mask)
{
    uint32_t lo = mask ? lowest_bit(mask) : 0, hi = mask ? highest_bit(mask) : 0;
    uint32_t fmin = 0, fmax = 0;
    int e;

    switch (clk) {
    case RB_SMU13_CLK_SOC:
    case RB_SMU13_CLK_FCLK:
    case RB_SMU13_CLK_VCLK:
    case RB_SMU13_CLK_DCLK:
        break;
    default:
        return RB_SMU13_E_INVAL;
    }
    e = rb_smu13_get_dpm_freq_by_index(s, clk, lo, &fmin);
    if (e)
        return e;
    e = rb_smu13_get_dpm_freq_by_index(s, clk, hi, &fmax);
    if (e)
        return e;
    return rb_smu13_set_soft_freq_range(s, clk, fmin, fmax);
}

/* yellow_carp_get_umd_pstate_clk_default */
static uint32_t umd_pstate_clk(const struct rb_smu13 *s, enum rb_smu13_clk clk)
{
    int v8 = s->mp1_rev == 8;            /* 13.0.8; else 13.0.1 / 13.0.3 */
    switch (clk) {
    case RB_SMU13_CLK_GFX:  return v8 ? 533 : 700;
    case RB_SMU13_CLK_SOC:  return v8 ? 533 : 678;
    case RB_SMU13_CLK_FCLK: return v8 ? 800 : 1800;
    default:                return 0;
    }
}

/* yellow_carp_get_dpm_profile_freq; ultimate-freq errors ignored as in Linux. */
static void profile_freq(struct rb_smu13 *s, enum rb_smu13_level level, enum rb_smu13_clk clk,
                         uint32_t *min, uint32_t *max)
{
    uint32_t limit = umd_pstate_clk(s, clk);
    switch (clk) {
    case RB_SMU13_CLK_GFX:
        if (level == RB_SMU13_LEVEL_PROFILE_PEAK)
            rb_smu13_get_dpm_ultimate_freq(s, clk, 0, &limit);
        else if (level == RB_SMU13_LEVEL_PROFILE_MIN_SCLK)
            rb_smu13_get_dpm_ultimate_freq(s, clk, &limit, 0);
        break;
    case RB_SMU13_CLK_SOC:
        if (level == RB_SMU13_LEVEL_PROFILE_PEAK)
            rb_smu13_get_dpm_ultimate_freq(s, clk, 0, &limit);
        break;
    case RB_SMU13_CLK_FCLK:
        if (level == RB_SMU13_LEVEL_PROFILE_PEAK)
            rb_smu13_get_dpm_ultimate_freq(s, clk, 0, &limit);
        else if (level == RB_SMU13_LEVEL_PROFILE_MIN_MCLK)
            rb_smu13_get_dpm_ultimate_freq(s, clk, &limit, 0);
        break;
    case RB_SMU13_CLK_VCLK:
    case RB_SMU13_CLK_DCLK:
        rb_smu13_get_dpm_ultimate_freq(s, clk, 0, &limit);
        break;
    default:
        break;
    }
    *min = *max = limit;
}

int rb_smu13_set_performance_level(struct rb_smu13 *s, enum rb_smu13_level level)
{
    static const enum rb_smu13_clk clks[5] = {
        RB_SMU13_CLK_GFX, RB_SMU13_CLK_FCLK, RB_SMU13_CLK_SOC, RB_SMU13_CLK_VCLK, RB_SMU13_CLK_DCLK,
    };
    uint32_t mn[5], mx[5];
    int e;

    if (!s)
        return RB_SMU13_E_INVAL;
    zero(mn, sizeof(mn));
    zero(mx, sizeof(mx));
    for (int i = 0; i < 5; i++) {
        switch (level) {
        case RB_SMU13_LEVEL_HIGH:
            rb_smu13_get_dpm_ultimate_freq(s, clks[i], 0, &mx[i]);
            mn[i] = mx[i];
            break;
        case RB_SMU13_LEVEL_LOW:
            rb_smu13_get_dpm_ultimate_freq(s, clks[i], &mn[i], 0);
            mx[i] = mn[i];
            break;
        case RB_SMU13_LEVEL_AUTO:
            rb_smu13_get_dpm_ultimate_freq(s, clks[i], &mn[i], &mx[i]);
            break;
        case RB_SMU13_LEVEL_PROFILE_STANDARD:
        case RB_SMU13_LEVEL_PROFILE_MIN_SCLK:
        case RB_SMU13_LEVEL_PROFILE_MIN_MCLK:
        case RB_SMU13_LEVEL_PROFILE_PEAK:
            profile_freq(s, level, clks[i], &mn[i], &mx[i]);
            break;
        case RB_SMU13_LEVEL_MANUAL:
        case RB_SMU13_LEVEL_PROFILE_EXIT:
            return RB_SMU13_OK;
        default:
            return RB_SMU13_E_INVAL;
        }
    }
    for (int i = 0; i < 5; i++) {
        if (!mn[i] || !mx[i])
            continue;
        e = rb_smu13_set_soft_freq_range(s, clks[i], mn[i], mx[i]);
        if (e)
            return e;
        if (clks[i] == RB_SMU13_CLK_GFX) {
            s->gfx_actual_hard_min = mn[i];
            s->gfx_actual_soft_max = mx[i];
        }
    }
    return RB_SMU13_OK;
}

/* -------------------------------------------------------------- bring-up */

int rb_smu13_bringup(struct rb_smu13 *s)
{
    int e = rb_smu13_check_fw_version(s);
    if (e)
        return e;
    e = rb_smu13_get_enabled_mask(s, 0);
    if (e)
        return e;
    e = rb_smu13_set_driver_table_location(s);
    if (e)
        return e;
    return rb_smu13_set_default_dpm_table(s);
}

const char *rb_smu13_strerror(int err)
{
    switch (err) {
    case RB_SMU13_OK:            return "ok";
    case RB_SMU13_E_INVAL:       return "invalid argument";
    case RB_SMU13_E_NOT_ALLOWED: return "message group not allowed (nothing sent)";
    case RB_SMU13_E_BUSY:        return "mailbox busy (nothing sent)";
    case RB_SMU13_E_TIMEOUT:     return "no answer from the SMU";
    case RB_SMU13_E_FW:          return "SMU answered with an error";
    case RB_SMU13_E_VERSION:     return "unsupported SMU version";
    case RB_SMU13_E_NODATA:      return "required table not loaded";
    case RB_SMU13_E_DEAD:        return "SMU context dead after a timeout (nothing sent)";
    default:                     return "unknown error";
    }
}
```


===== FILE: tools/plugin_check.sh =====

```bash
#!/bin/sh
# After a reboot: did the RembrandtGPU plugin load, and what did it say?
#
#   tools/plugin_check.sh          # no sudo needed
#   sudo tools/plugin_check.sh     # also searches the kernel message buffer
#
# Read-only. Saves what it found to logs/<time>-plugin/.
set -u

root="$(cd "$(dirname "$0")/.." && pwd)"
out="$root/logs/$(date +%Y%m%d-%H%M%S)-plugin"
mkdir -p "$out"

args="$(nvram boot-args 2>/dev/null | cut -f2-)"
echo "boot-args: $args"
case " $args " in *" -rbgpuoff "*) off=1; echo "  -rbgpuoff is set: the plugin should report itself disabled" ;; *) off=0 ;; esac

echo
echo "--- loaded kexts"
kmutil showloaded --list-only 2>/dev/null | grep -E 'as.vit9696.Lilu|com.rembrandtgpu' | tee "$out/kexts.txt"
grep -q com.rembrandtgpu.plugin "$out/kexts.txt" || echo "  com.rembrandtgpu.plugin is not loaded"
# Is the loaded plugin the one in build/out? (kmutil prints the binary's UUID)
built="$(dwarfdump --uuid "$root/build/out/RembrandtGPU.kext/Contents/MacOS/RembrandtGPU" 2>/dev/null | awk '{print $2}')"
if [ -n "$built" ] && grep -q com.rembrandtgpu.plugin "$out/kexts.txt"; then
    grep com.rembrandtgpu.plugin "$out/kexts.txt" | grep -q "$built" \
        && echo "  loaded plugin = build/out (UUID $built)" \
        || echo "  NOTE: loaded plugin is NOT the current build/out (UUID $built): reinstall with sudo tools/efi.sh install"
fi

echo
echo "--- plugin messages this boot"
/usr/bin/log show --last boot --style compact --predicate 'eventMessage CONTAINS "RembrandtGPU"' 2>/dev/null \
    | grep RembrandtGPU | grep -v "log run noninteractively" > "$out/log.txt"
if [ "$(id -u)" = 0 ]; then
    /sbin/dmesg | grep RembrandtGPU >> "$out/log.txt"
fi
cat "$out/log.txt"

echo
echo "--- registry"
# Lilu's plugin_start.cpp: the RembrandtGPU IOService only survives start()
# when Lilu accepted the plugin and pluginStart() ran (startSuccess). A loaded
# kext without the instance stopped itself (-rbgpuoff, or Lilu refused it).
ioreg -w0 -p IOService -c RembrandtGPU -r -l | grep -E '\+-o RembrandtGPU|CFBundleIdentifier"' \
    | tee "$out/ioreg.txt"
loaded=0; grep -q com.rembrandtgpu.plugin "$out/kexts.txt" && loaded=1
running=0; grep -q '+-o RembrandtGPU' "$out/ioreg.txt" && running=1

echo
echo "--- verdict"
if [ $running = 1 ]; then
    [ $off = 1 ] && echo "WRONG: plugin ran although -rbgpuoff is set" \
                 || echo "OK: plugin loaded and ran (milestone 0)"
elif [ $loaded = 1 ]; then
    if [ $off = 1 ]; then
        echo "OK: -rbgpuoff works, the plugin stopped itself"
    else
        echo "plugin loaded but did not run: Lilu refused it."
        grep -o 'should not continue [0-9]*\|failed to call parent [0-9]*' "$out/log.txt" | head -1
        echo "(Lilu errors: 4 = macOS version out of range, 5 = disabled, 6 = too late)"
    fi
else
    echo "plugin not loaded: OpenCore did not inject it. Run: sudo tools/efi.sh status"
fi
echo "(early-boot plugin log lines are usually not kept; the registry is the check)"

echo
echo "--- milestone 1: peer, GPU properties, HWServices"
ioreg -w0 -p IOService -r -c IOPCIDevice -l 2>/dev/null > "$out/pci.txt"
# The 680M's IOPCIDevice (IGPU@0) and what is attached to it.
ioreg -w0 -p IOService -n IGPU@0 -r -d1 -l 2>/dev/null > "$out/igpu.txt"
ioreg -w0 -p IOService -n IGPU@0 -r -d2 2>/dev/null | grep -- "+-o" | sed 's/<class \([^,]*\),.*/(\1)/' | tee "$out/igpu-children.txt"
for k in '"@0,name"' '"RBGPULoad"' '"LoadHWServices"' '"LoadAccelerator"'; do
    v="$(grep -m1 "$k" "$out/igpu.txt" | sed 's/^[ |]*//')"
    echo "  ${v:-$k: not set}"
done
binsz="$(grep -m1 '"ATY,bin_image"' "$out/igpu.txt" | sed 's/.*<\(.*\)>.*/\1/' | tr -d '\n' | wc -c)"
[ "$binsz" -gt 0 ] && echo "  ATY,bin_image: $((binsz / 2)) bytes" || echo "  ATY,bin_image: not set"
peer=0; grep -q "RembrandtGPUPeer" "$out/igpu-children.txt" && peer=1
hws=0; grep -q "HWServices" "$out/igpu-children.txt" && hws=1
echo "  peer attached: $peer   HWServices attached: $hws"
kmutil showloaded --list-only 2>/dev/null | grep -iE "AMDRadeonX6000|AMDRadeonX6800|AMDSupport" | tee "$out/amd-kexts.txt"

/usr/bin/log show --last boot --style compact --predicate \
    'eventMessage CONTAINS "RembrandtGPUPeer" OR eventMessage CONTAINS[c] "AMDRadeon" OR eventMessage CONTAINS "HWLibs" OR eventMessage CONTAINS "HWServices"' \
    2>/dev/null | grep -v -e "log run noninteractively" -e "^Timestamp" > "$out/amd-log.txt"
[ "$(id -u)" = 0 ] && /sbin/dmesg | grep -iE "RembrandtGPUPeer|AMDRadeon|HWLibs|HWServices|firmware device|Unsupported hw" >> "$out/amd-log.txt"
echo "  $(wc -l < "$out/amd-log.txt" | tr -d ' ') log lines from the peer and Apple's AMD kexts (first 40):"
head -40 "$out/amd-log.txt" | sed 's/^/    /'
echo
echo "--- plugin diagnostics (RBGPULog / RBGPUDiag on the peer)"
ioreg -w0 -p IOService -c RembrandtGPUPeer -r -l -a 2>/dev/null > "$out/peer.plist"
python3 - "$out/peer.plist" <<'PY'
import plistlib, sys
try:
    tree = plistlib.load(open(sys.argv[1], "rb"))
except Exception:
    print("  peer not found"); sys.exit(0)
node = tree[0] if isinstance(tree, list) and tree else tree
diag, log = node.get("RBGPUDiag"), node.get("RBGPULog")
if diag is None and log is None:
    print("  (the peer publishes no RBGPUDiag / RBGPULog)")
for k, v in sorted((diag or {}).items()):
    # ioreg -a writes 64-bit numbers as signed integers; show them as unsigned.
    print(f"  {k} = {v & 0xffffffffffffffff:#x}" if isinstance(v, int) else f"  {k} = {v}")
for line in (log or []):
    print("  | " + str(line).replace("\n", "\n  |     "))
PY
echo "--- logged pointers inside AMD kexts (tools/re/ptrsym.py)"
python3 "$root/tools/re/ptrsym.py" "$out/peer.plist" 2>&1
ls -t /Library/Logs/DiagnosticReports/*.panic 2>/dev/null | head -1 | sed 's/^/  newest panic report: /'

[ -n "${SUDO_USER:-}" ] && chown -R "$SUDO_USER" "$out"
echo
echo "saved to ${out#$root/}"
```


===== FILE: tools/re/ptrsym.py =====

```python
#!/usr/bin/env python3
"""Name the code and data addresses in a boot's RBGPULog: every pointer that
falls inside an AMD kext the plugin saw load ("HWLibs loaded at A (size S)",
"X6000 loaded at ...", "HWSvc loaded at ...") is mapped back to the extracted
kext in build/re/ and printed as symbol+offset, with its file address.

    python3 tools/re/ptrsym.py logs/<dir>/peer.plist

The kexts are loaded contiguously from __TEXT, so file address =
__TEXT.vmaddr + (pointer - load address). Pointers outside every loaded kext
(heap, stack, MMIO) are skipped.
"""
import os
import plistlib
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from machoread import Macho  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
KEXTS = {
    "HWLibs": "AMDRadeonX6800HWLibs",
    "X6000": "AMDRadeonX6000",
    "HWSvc": "AMDRadeonX6000HWServices",
}
LOADED = re.compile(r"\b(HWLibs|X6000|HWSvc) loaded at (0x[0-9a-fA-F]+) \(size (0x[0-9a-fA-F]+)\)")
POINTER = re.compile(r"0x[0-9a-fA-F]{12,16}\b")


def log_lines(path):
    tree = plistlib.load(open(path, "rb"))
    node = tree[0] if isinstance(tree, list) and tree else tree
    return [str(x) for x in (node.get("RBGPULog") or [])]


def main():
    lines = log_lines(sys.argv[1])
    ranges = []
    for line in lines:
        m = LOADED.search(line)
        if m:
            macho = Macho(os.path.join(ROOT, "build", "re", KEXTS[m.group(1)]))
            text = next(s for s in macho.segs if s[0] == "__TEXT")
            ranges.append((int(m.group(2), 16), int(m.group(3), 16), KEXTS[m.group(1)], macho, text[1]))
    if not ranges:
        print("  (no 'loaded at' lines in the log)")
        return 0
    seen = {load for load, *_ in ranges}
    named = 0
    for line in lines:
        for tok in POINTER.findall(line):
            p = int(tok, 16)
            if p in seen:
                continue
            for load, size, kext, macho, base in ranges:
                if load <= p < load + size:
                    seen.add(p)
                    named += 1
                    va = base + (p - load)
                    n = macho.name_at(va)
                    name = f"{n[0]}+{n[1]:#x}" if n and n[1] else (n[0] if n else "?")
                    print(f"  {tok} = {kext} {va:#x} {name}")
    if not named:
        print("  (no logged pointer falls inside a loaded AMD kext)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
```
