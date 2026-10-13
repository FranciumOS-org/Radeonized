# RembrandtGPU — notes for Claude

> **New session / new account: read `docs/HANDOFF.md` FIRST.**
> It is the current *state* — what is settled, what is open, the pending
> experiment, the procedure, and the eight withdrawn conclusions this project
> has produced by reading a value without first establishing what makes it
> meaningful. The rest of this file is the chronological *evidence journal*:
> authoritative for what was measured, in order, and not to be trimmed.
> `docs/HANDOFF-FULL.md` is a stale dump from 2026-10-05 — ignore it.

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
  `sudo tools/unload.sh`. Claude never runs these and never asks for the password.
- Never install into the EFI or /Library/Extensions.
- The Lilu plugin (src/plugin, `make plugin`) boots from the EFI. The user runs
  `sudo tools/efi.sh install|off|on|uninstall|status|restore` (saves
  config.plist to logs/efi-backups/ first; edits via tools/efi_config.py, tested
  by tools/efi_config_test.py) and `tools/plugin_check.sh` after a reboot.
  Claude never runs efi.sh. `efi.sh sysreport-on` + reboot + `efi.sh vbios`
  pulls the 680M VBIOS out of the ACPI VFCT table (tools/vfct.py, tested by
  tools/vfct_test.py) into firmware/ (gitignored: AMD/ASUS firmware).
- Before any load, commit so a hang loses nothing. The user allows Claude to
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

## 2ag-2bb (2026-10-05/06): past the GC firmware wall, TTL init completes

The yellow_carp firmware substitution worked. With `-rbgpusync570
-rbgpuspoofttl -rbgpupsphw`, in order: the GC fw version check patched
(`gc_vercheck_patched 1`, handling the 0x3D encoding), 7 firmware entries
swapped to the 680M's own images (`yc_swapped 7`), Navi21 TAP_DELAYS skipped
because the TOS rejects them (`tap_skipped 5`), and three waits bypassed that
never complete on the PSP-load path: `_gc_fw_autoload_is_completed` (2am),
`_gc_check_register_equal_ext` (2an, RLC safe mode), and
`_sdma_5_2_1_fw_autoload_is_completed` (2ao). SMU bringup stays skipped while
the DMA address is unverified (2ak; `carveout_ok` is still 0).

`_ttl_hw_init` then ran for the first time and **TTL init completed**. The
accelerator's own state trace goes far past boot 3's `0x20c5`:

```
u64AllAccelStart 0x00fffffd
hardwareInit 1 · initializeTtl 1 · hwMemoryInitialized 1 · hwGartInitialized 1
hwVmmInitialized 1 · allocateHWEngines 1 · initializeHWEngines 1
initHWDisplay 1 · initHWInfo 1
u64AllPowerUp 0x0 — ttlPowerUp 0, accelPowerUpHW 0, everything after 0
```

GART and VMM are up; nothing has powered up. Logs: logs/20261006-*-plugin,
logs/20261011-*-plugin (174116, 184452, 192548, 204341 are dry runs without
`-rbgpupsphw`, ending at the old `_ttl_initialize` status 4; the 96-key ones
are hw boots that ended in a panic, so their return values were never written).

## 2bn-2br, 2cb-2cn (2026-10-06/11): allocateMemoryResources, then powerUp

- Display pipe reserve: `AMDHWMemory::reserve` (originator 0x1e) fails because
  the allocator is empty — `free=0`, no reservations — because `powerUpHW` has
  never run (2bq). Releasing `reserveNDRVSpace`'s boot-framebuffer slot and
  retrying (2bp) did not help; request 0x06 succeeds and 0x1B is unsupported
  but the VRAM pool stays empty (2br). docs/SPEC-DISPLAY-RESERVE.md has the
  chain.
- `AMDHardware::allocateMemoryResources` fails: engine[9], channel[0] passes
  `vtable[0x2c0]`, channels [1] and [2] return 0 (2cb). `AMDVCN2EncChannel::
  allocateMemoryResources` stubbed to true, VCN firmware being absent (2cc).
- 2cf-2cl moved to the PM4 engine: `AMDGFX10PM4Engine::powerUp` runs, `doStart`
  runs with arg false, the real `initComputeMQD` succeeds, and
  `AMDGFX10KIQHWChannel::startKIQ` is called and returns 0.
- 2cm replaced the deliberate reserve-hook panic with a log line; 2cn hooks
  `AMDGraphicsAccelerator::start` to fail cleanly when `powerUpHW` fails. Not
  yet confirmed on hardware (the EFI partition filled up).
- A second panic path remains: `IOAccelDisplayMachine::display_mode_did_change`
  (Kernel-2026-10-11-215906.panic) — the only Apple panic in that series; the
  other twelve Oct 11 panics are the plugin's own `rbgpu reserve fail`.

## Correction (2026-10-12, docs/SPEC-PM4-DOSTART.md): 2cl is backwards

Verified against the installed X6000 7.0.25 and IOAcceleratorFamily2 485.2:

- `powerUp` (0xbea1182) is a 14-byte tail jump to its own vtable[0x148], a
  thunk that calls `doStart(false)`. powerUp has no body and no recovery
  path: doStart's return value is powerUp's, verbatim.
- `startKIQ` returns a **status, 0 = success**, not a bool. In doStart,
  `testl %eax,%eax; je` continues on 0 and aborts on nonzero. `kiqres=0` was
  success. 2cl forces the return to 1 and so sends doStart down the one
  branch that always fails; it cannot ever produce a passing boot and must be
  reverted before the next boot means anything. (The 2ci comment in
  kern_start.cpp had already derived 0 = success.)
- doStart **does** have a return-true path (0xbea150e -> 0xbea1280 with
  r14 = 1); 0xbea1280 is a shared epilogue returning `r14 & 1`, not the false
  path. The remaining gates after startKIQ are
  `submitSetResourcesPacket`, `initGraphicsMQD` (never yet exercised),
  `submitMapQueuesPacket` per ring (`+0x348` = 5), a ring-state check that
  needs `CP_RB0_RPTR`/`CP_RB0_WPTR`/`CP_RB0_WPTR_HI` and channel+0x54 all
  zero, `waitForActiveValue`, and `(engine+0xc0)->vtable[0x328]`.
- HAZARD: doStart calls `waitForActiveValue(0, false, 1, 0)`, and with that
  4th argument 0 its 1000 x 100 us timeout and its delay are both dead code.
  It spins on `CP_GFX_HQD_ACTIVE` with no bound unless the register reads
  0xffffffff. Removing 2cl exposes this on the next boot: force the 4th
  argument to 1 first (that restores Apple's own 100 ms bound, it is not a
  bypass).
- The display panic has a supported escape hatch: `displayModeDidChange`
  returning false only panics when bit 0x20 of accelerator `+0xc91` is clear,
  and the one site that sets it (`IOGraphicsAccelerator2::start` 0x14e4e165)
  does so when `getProperty("IOPCITunnelled", gIOServicePlane, recursive|
  parents)` finds the property — Apple's eGPU flag. Publishing
  `IOPCITunnelled` on the 680M's IOPCIDevice sets it with no code patching.
  It changes other behaviour too (Removable, PM, surface eviction), so it
  belongs behind its own boot-arg, for survival during bring-up only.
- GC register names for this path come from docs/linux-ref/gc_10_3_0_offset.h
  (added 2026-10-12); Apple's encoded ids are `(dword offset << 32) | 11`,
  11 = GC_HWID.

## 2co-2cq (2026-10-12): fixes implemented, not yet booted

In src/plugin (built, hosttest passes; no boot yet):

- 2co: `startKIQ` hook is observe-only; 2cl's forced `1` removed.
- 2cp: `waitForActiveValue`'s third parameter forced true, restoring Apple's
  1000 x 100 us bound. Without this, removing 2cl would expose an unbounded
  poll of `CP_GFX_HQD_ACTIVE` — a wedged CPU with no panic and no log.
- 2cq: observe-only hooks on `submitSetResourcesPacket`, `initGraphicsMQD`
  (never exercised before) and `submitMapQueuesPacket`.
- `-rbgpunodisppanic` (new, off by default): the peer publishes
  `IOPCITunnelled`, so `display_mode_did_change` returns false instead of
  panicking. Changes other behaviour too (Removable, PM, surface eviction);
  for bring-up survival only.

New diag keys: `wfav_calls`, `wfav_bounded_forced`, `wfav_ret`,
`setres_calls`, `setres_ret`, `gfxmqd_calls`, `gfxmqd_ret`, `mapq_calls`,
`mapq_fails`. The 2cm survive line carries them as well.

Next boot: `-rbgpusync570 -rbgpuspoofttl -rbgpupsphw -rbgpunodisppanic`.
What it answers: which gate after `startKIQ` fails, and whether
`CP_GFX_HQD_ACTIVE` ever goes active. Open question in
docs/SPEC-PM4-DOSTART.md section 7: nothing yet tests whether the PSP
accepted and started the yellow_carp images.

## 2cr (2026-10-12): no panic, but no desktop either

First `-rbgpunodisppanic` boot: **no panic** (the IOPCITunnelled escape hatch
works), but the desktop never appeared — the screen stayed on the verbose
console. Expected consequence: `displayModeDidChange` now returns false
instead of panicking, so the display machine never completes the mode change
and WindowServer never puts up the UI. The boot's RBGPULog was lost (it only
lives in that boot's IORegistry), and the user recovered the machine with
`-rbgpuoff`.

- 2cr: fixed an ordering bug in 2cn. It checked `g_rb_2cn_poweruphw_failed`
  *before* calling the original `start()`, but `powerUpHW` is called from
  inside `start()`, so the flag was always still 0 there and the guard only
  applied to a second `start()` that never comes. That is why 2cn was never
  observed to do anything. The check now runs after the original returns.
- Hypothesis, not yet tested: failing `start()` may still not restore the
  desktop, because 2be patched the accelerator's `stop()` to RET (the trap-3
  guard), so a refused match never tears down the IOAcceleratorFamily2
  registration. If 2cr alone does not bring the desktop back, that teardown
  is the next thing to look at — and the trap that made 2be necessary has to
  be understood rather than patched away.
- tools/dump_peer.sh (new): dumps RBGPULog/RBGPUDiag with nothing but `ioreg`
  and `sh`, so the log can be collected from single-user mode (Cmd+S at the
  OpenCore picker) on a boot that never reaches a desktop. plugin_check.sh
  needs python3, `log show` and dwarfdump, none of them reliable there.

## 2ct (2026-10-12): headless accelerator — a desktop with the accelerator loaded

2cs was built but never installed (the user's last `efi.sh install` predates
it), so every boot after 2cr still had the accelerator always-on and no way
back to a desktop except editing config.plist from Windows. Install state has
to be checked against the built UUID before drawing conclusions from a boot.

Single-user mode is NOT a usable route on macOS 15 here: `mount -uw /` fails
on the sealed system volume and /Users is a separate Data volume that may not
be mounted, so tools/dump_peer.sh could not write anything. Deliberate
diagnostic panics are also off the table by the user's decision (2026-10-12).

2ct makes the accelerator adopt **no display pipe**, so an armed boot reaches
a normal desktop and `tools/plugin_check.sh` works as usual:

- `IOAccelDisplayMachine::found_framebuffer` (family 0x14e21208) is hooked to
  return false. That is Apple's own path when the pipe count has reached
  `accel+0xcb0` (= 1, set by `IOGraphicsAccelerator2::initializeConfigStructure`
  0x14e4d7bf): it logs and returns false at 0x14e21287 without creating a pipe.
- With zero pipes, `set_display_mode_and_vram`'s pipe loop — its only false
  return — does nothing, so `displayModeDidChange` returns true,
  `display_mode_did_change` takes its success path (0x14e2153f: count 0 ->
  0x14e2160c -> true), and `IOFramebuffer::open` completes.
- The accelerator still loads and still runs powerUp/doStart, so every gate
  diagnostic is unaffected. It just drives no display, which is the honest
  state while powerUpHW fails.
- Do NOT instead fail the pipe constructor or return NULL from the factory:
  at 0x14e2125d the result is stored into the pipe array with no NULL check
  and the count is incremented regardless (0x14e21263-0x14e2127d), so a NULL
  pipe would crash on the next pipe walk.
- `-rbgpudisplaypipe` restores Apple's behaviour. New diag keys
  `found_fb_routed`, `found_fb_calls`, `found_fb_refused`.
- IOAcceleratorFamily2 is now in kextList (it loads as a dependency of
  AMDRadeonX6000, so after this plugin, and onKextLoad fires for it).

## 2cu (2026-10-12): diagnostics survive the boot in NVRAM

2ct was installed (UUID 2801660E) and the boot still showed no UI. That boot
was **armed**, not unarmed as it looked: `rbgpu-arm` had been set during the
single-user attempt while 2cs was not yet installed, so nothing consumed it,
and 2ct's one-shot consume is why `nvram rbgpu-arm` now reads empty. So the
accelerator did load and 2ct did not deliver a desktop. Whether the
`found_framebuffer` hook even installed is unknown, because the boot's log
died with it — which is the real problem, not the UI.

2cu writes a compact summary to NVRAM on every diag refresh, flushed with
`IONVRAM-FORCESYNCNOW-PROPERTY` so a power-cycle does not lose it. Read it
from the next boot's desktop:

```
nvram rbgpu-diag
```

Values are hex, space-separated `k=v`. Keys: armed, found_fb_routed,
found_fb_calls, found_fb_refused, ttl_raw_init_ret, ttl_hw_init_ret,
gc_sw_init_ret, pw_stage, pw_ret, setres_ret, gfxmqd_ret, mapq_calls,
mapq_fails, wfav_calls, wfav_bounded_forced, wfav_ret. Last write wins, so a
boot that dies later still leaves what it reached.

Open question for the next armed boot: `found_fb_routed`. If it is 0 or
absent, the 2ct hook never installed — the likely cause being that
IOAcceleratorFamily2 was already loaded when the plugin registered its
kextList, in which case onKextLoad never fires for it and the hook has to go
in another way. If it is 1 with `found_fb_refused` >= 1 and there is still no
desktop, then zero display pipes is not sufficient and the assumption behind
2ct is wrong.

Lesson recorded: never interpret a boot without checking BOTH the installed
UUID against build/out AND whether `rbgpu-arm` was still set from an earlier
attempt. Two boots were wasted on each of those.

## 2cv (2026-10-12): the UI breakage was IOPCITunnelled, not the accelerator

The NVRAM summary from the no-UI boot read `armed=0` and nothing else: that
boot had **no accelerator at all** (LoadAccelerator withheld), and still
reached no desktop. So the accelerator was never what broke the UI, and
2cr/2ct were aimed at the wrong thing.

Milestone 1 ran peer + HWServices + HWLibs with no accelerator and the screen
was unchanged. The difference now is `-rbgpunodisppanic`, which makes the peer
publish **`IOPCITunnelled`** on IGPU@0 — Apple's "this GPU is behind
Thunderbolt" flag. Declaring the GPU that owns the built-in panel to be a
hot-pluggable external device is why WindowServer never drives the internal
display. The side effect warned about when 2co added it ("driving the built-in
panel") was the whole effect.

- `-rbgpunodisppanic` must stay OUT of boot-args. It is not needed once 2ct
  holds: with zero display pipes `display_mode_did_change` returns true on its
  own, so there is no false return to turn into a panic.
- The first 2cu lesson stands and gained a third item: before interpreting a
  boot, check the installed UUID against build/out, whether `rbgpu-arm` was
  left set, AND which plugin boot-args are actually in config.plist.
- `nvram rbgpu-diag` worked exactly as intended on its first try: one line
  from a dead boot replaced four blind reboots. Keep it as the primary way to
  read a boot that does not reach a desktop.

EFI hazard (2026-10-12): Spotlight indexed the mounted EFI partition and
corrupted it; the user restored from logs/efi-backups via Windows. tools/efi.sh
now creates `.metadata_never_index` at the EFI root and runs `mdutil -i off`
/ `mdutil -X` on it whenever it mounts, best-effort. Keep the backups.

### 2cv confirmed on hardware (2026-10-12)

Baseline boot with UUID 03147907, boot-args `-rbgpusync570 -rbgpuspoofttl
-rbgpupsphw -nvoff -wegnoegpu` and **no** `-rbgpunodisppanic`, `-rbgpuoff` or
`-s`: desktop came up normally with the kext live. On IGPU@0:
RembrandtGPUPeer, AMDSupport, IONDRVFramebuffer, HWServicesNavi; no
accelerator (`armed=0`); no IOPCITunnelled property. So the milestone 1
configuration is restored with everything except the accelerator running, and
IOPCITunnelled is confirmed as the sole cause of the lost UI.

## 2cw (2026-10-12): the armed boot HUNG in engine[4], and TTL init returned 0

A photo of the stuck console (the boot recovered on its own restart, as 2cs
intends) shows the real state, which NVRAM had already lost:

```
AmdTtlServices::initialize() returned 0x00000000
AmdTtlServices::initialize(this, input) returned 0x00000000
2bm/2cb: AMDHardware::allocateMemoryResources entered
2cb: step1 result=0xffffff90692db680 (OK)
2cb: engine[0] allocateMemoryResources -> 1 (OK)
2cb: engine[1] -> 1 (OK)   engine[2] -> 1 (OK)   engine[3] -> 1 (OK)
2cb: engine[4] = 0xffffff90692f5300 calling allocateMemoryResources...
   <console stops here>
```

- **TTL init returned 0** — the first explicit success, both the inner and the
  outer `AmdTtlServices::initialize`.
- The boot did not fail, it **hung** inside engine[4]'s
  `allocateMemoryResources`. Engines 0-3 now pass; on 2026-10-11 the failure
  was engine[9] and engines 0-3 never got this far.
- `-rbgpunodisppanic` was NOT set this boot, so the stuck console is the hang,
  not the IOPCITunnelled problem of 2cv. Those are two separate faults and
  both were real.
- One-shot arming worked exactly as designed: a plain restart came up on the
  desktop with no rescue.

Three fixes, none of them a guess:

- 2cw rotates the NVRAM summary instead of clobbering it. The recovery boot
  overwrote rbgpu-diag with its own `armed=0` before the armed boot's numbers
  were read, which is why the hang left no trace. Now the first write of each
  boot moves the stored value to `rbgpu-diag-prev`, so the last two boots are
  both readable (`nvram rbgpu-diag`, `nvram rbgpu-diag-prev`).
- Request 0x14 (Gpu Status) is polled continuously and flooded both the
  console and the 512-line ring, pushing out everything else. Now only the
  first three and every 64th are logged.
- The 2cb loop names each engine from its OSObject metaclass, so the next boot
  says what engine[4] actually is rather than just its index.

Next boot answers: which class engine[4] is, and therefore whether it is an IP
whose resources cannot be allocated on this chip (VCN encode was already
stubbed in 2cc for exactly that reason).

## 2cx (2026-10-12): the GPU never executes the first packet — located exactly

Second armed-boot photo, with the 2cw build. Everything up to the first real
GPU command now passes, several of them for the first time ever:

```
2cb: all steps passed -> true
2bm allocateMemoryResources returned 1          <- was failing at engine[9]
Registered accelerator instance: AMDRadeonX6000_AMDNavi21GraphicsAccelerator
2cn: accelerator start returned 1               <- start() SUCCEEDED
2ct: found_framebuffer(...) REFUSED -> adopts no display pipe   <- 2ct works
2br: powerUpHW entered
2bs: AMDHardware::powerUpHWEngines entered
2bu: AMDGFX10PM4Engine::powerUp entered -> 2ce: doStart entered (arg=0)
2cg: original initComputeMQD returned 1
2co: startKIQ returned 0 (0 = SUCCESS, doStart continues)
   <nothing further; only request 0x14 polls, up to [#1536]>
```

So the engine[4] "hang" of the previous boot was **log-induced slowness**, not
a stall: throttling request 0x14 let allocateMemoryResources run straight
through. And 2co's correction is confirmed on hardware — startKIQ returns 0
and doStart continues past it.

Where it actually stops, from the disassembly:

```
submitSetResourcesPacket (0xbec7404)   builds the SET_RESOURCES packet
  -> tail call submitKIQFrame (0xbe9572e)
       commitBlock, ring doorbell (vtable[0x158])
       -> waitForHwStamp (0xbe85536)
            timeout 0x4C4B40 us (5 s), poll 0x64, re-armed while
            channel vtable[0x2a0] says keep waiting (0xbe855c0-0xbe855f0)
```

`waitForHwStamp` is the first point where this driver asks the GPU to EXECUTE
something and waits for the answer. The stamp never arrives, so the command
processor is not consuming the KIQ packet. That is SPEC-PM4-DOSTART section 7
answered: **the CP/MEC microcode is not running**, so the PSP either did not
load the yellow_carp images or did not start them. Every bypass in 2am/2an/2ao
was masking exactly this.

Lesson about the instrumentation itself: all the 2cp/2cq hooks logged only
AFTER the original returned, so a hang produced no line at all and the stall
had to be found by disassembly. 2cx adds ENTER logging to each of them and
hooks `waitForHwStamp` itself (observe only, no override), logging its return
and elapsed ticks. New diag keys `stamp_calls`, `stamp_ret`, `stamp_ticks`.

Next real question, and it is no longer about Apple's driver: did the PSP
accept and start the CP/MEC firmware? That is PSP-side evidence (ring
response status per load), plus reading CP_STAT / CP_ME_CNTL / RLC_GPM_STAT
at this point. Note the boot is not wedged forever - waitForHwStamp is
bounded, so it should eventually fail and, with 2ct, let the desktop come up.

## 2cy (2026-10-12): fail fast instead of waiting forever

A third photo, later in the same situation: request 0x14 at **#24640** and
still climbing, with HID drivers, IOConsoleUsers and the login window coming
up behind it. The system is alive; only the accelerator thread is stuck.

That settles the shape of `waitForHwStamp`: the 5 s timeout is **per poll**,
and the loop at 0xbe855c0-0xbe855f0 re-arms it for as long as the channel's
`vtable[0x2a0]` says to keep waiting. With a command processor that never
consumes the packet, the wait is unbounded in practice — the earlier note
that it "should eventually fail and let the desktop come up" was wrong, and
only a power cycle ends such a boot.

2cy therefore returns false from `waitForHwStamp` immediately instead of
calling the original. The stamp cannot arrive while the CP microcode is not
running, so waiting costs a power cycle and buys nothing. Returning false
takes Apple's own failure path (submitKIQFrame logs and returns false,
doStart exits via vtable[0x150], powerUp and powerUpHW fail), and with 2ct
refusing the display pipe the boot then COMPLETES to a desktop where
`tools/plugin_check.sh` reads the whole log. Armed boots stop costing a power
cycle.

`-rbgpustampwait` restores the real wait, for the boot where the CP finally
runs and the stamp is expected to arrive. That flag is the test for "is the
command processor alive now".

### 2cy confirmed on hardware (2026-10-12)

The armed boot now completes. Full chain from the console:

```
2co: startKIQ returned 0 (0 = SUCCESS, doStart continues)
2cx: submitSetResourcesPacket ENTER (ch=.. spec=..)
2cx: waitForHwStamp ENTER (ch=.. stamp=1)
2cy: skipping the wait (-rbgpustampwait not set) -> false
2cx: waitForHwStamp -> 0 (TIMEOUT - the CP never consumed the packet)
2cq: submitSetResourcesPacket -> 0 (FAIL -> doStart exits via vtable[0x150])
2ce: doStart returned 0
2bu: AMDGFX10PM4Engine::powerUp -> 0 ; 2bv: *** ENGINE 0 FAILED *** (fail_idx=0)
2bt: AMDHardware::powerUp -> 0 ; AMDGFX10Hardware::powerUp -> 0
2br: Navi21Hardware::powerUp -> 0 ; powerUpHW returned 0x0
2cn: powerUpHW failed, will fail start() cleanly (survive mode)
handlePostRegister: Request 0x06 returned 0x00000000
handlePostRegister: Request 0x1B returned 0xE00002C7
[rtw89] scan finished: 92 network(s) heard
```

The boot carries on into Wi-Fi scanning: no hang, no power cycle. 2cy, 2cn
and 2ct all fire in the intended order, and the engine-0 failure is now a
clean, logged, survivable outcome instead of a wedged machine.

Also new, for later: the accelerator sends the peer requests **0x12** and
**0x1D**, both currently "Unhandled", and request 0x1B still answers
0xE00002C7. Neither blocks the current path.

The remaining question is unchanged and is now the only one that matters:
the CP never consumes the KIQ packet, so the command processor is not
running. Next evidence to gather is PSP-side (ring response status per
firmware load) plus CP_STAT / CP_ME_CNTL / RLC_GPM_STAT reads, and the
2am/2an/2ao bypasses — which force "firmware ready" latches to report
success — should be made per-caller and logged rather than blanket-true,
since they have been masking exactly this since 2ag.

## 2cz-2da (2026-10-12): read a displayless boot, and ask the PSP directly

The armed boot never reaches a desktop. The console text continues past the
GPU failure into Wi-Fi scanning, and then WindowServer starts and blanks the
panel completely — no desktop, and no text either, so even a photograph stops
working and `plugin_check.sh` cannot run. (2cv's IOPCITunnelled fault is
fixed and separate; this is WindowServer driving a framebuffer it cannot
actually mode-set once an accelerator is present.)

Two ways to read such a boot:

- **SSH (preferred).** The system stays alive — rtw89 reported "scan
  finished: 92 network(s) heard" — so with Remote Login enabled the machine
  is reachable at 10.0.0.69 and `tools/plugin_check.sh` can be run from a
  phone. It writes logs/<time>-plugin/ to disk, so nothing has to be read on
  the phone; just reboot afterwards and read the files.
- **NVRAM (fallback, 2cz).** The peer now also writes the last 8 log lines as
  `rbgpu-log` alongside `rbgpu-diag`. The 2cw rotation never created
  `rbgpu-diag-prev` at all, so it now copies the text into a fresh OSString
  and records a `rot` key (1 created, 2 setProperty did not take, 0 nothing
  to rotate) rather than failing silently again.

2da hooks `_ipi_psp_load_fw` (HWLibs 0x1264289b), the single place where each
firmware image is handed to the PSP:

```
edi = *(uint32 *)p                 TTL firmware id
_TtlFwIdToCgsFwId(ttl_id)   -> cgs id
_ipiPspCgsFwToPspFw(cgs_id) -> psp id; 0x37 = no mapping -> assert, return 4
otherwise build the command and submit it to the PSP
```

Logging the TTL id and the return per call says exactly which images the PSP
accepted and which it refused — the question SPEC-PM4-DOSTART section 7 asks
and nothing has yet answered. Observe only. Diag keys `fw_calls`, `fw_fails`,
`fw_failmask` (bit per TTL firmware id). The symbol is a local text symbol
defined in HWLibs, not an import, so routing it by HWLibs' id is safe under
the milestone-2c Lilu rule.

## 2db (2026-10-12): `_ipi_psp_load_fw` is NEVER called — GC uses backdoor autoload

First full capture of an armed boot, over SSH with no display
(logs/20261012-165018-plugin, UUID 81AD2BE6 = build/out). The whole chain is
in the log, ending exactly as the photographs showed:

```
2ce: doStart entered (arg=0)
2co: startKIQ returned 0 (0 = SUCCESS, doStart continues)
2cx: submitSetResourcesPacket ENTER
2cx: waitForHwStamp ENTER (stamp=1)
2cy: skipping the wait -> false
2cx: waitForHwStamp -> 0 (TIMEOUT - the CP never consumed the packet) (5831 ticks)
2cq: submitSetResourcesPacket -> 0 ; 2ce: doStart returned 0
2bv: *** ENGINE 0 FAILED *** ; 2br: powerUpHW returned 0x0
2cn: powerUpHW failed, will fail start() cleanly (survive mode)
X6000: [54:0:0]: Controller is enabled, finish initialization
2ct: found_framebuffer REFUSED -> accelerator adopts no display pipe
```

`u64AllPowerUp` is now 0xba (accelPowerUpHW_entry/exit, setVirtualSpace,
hardwarePowerUp_entry/exit all 1), against 0x0 all week.

**The finding:** `HWLibs: hooked _ipi_psp_load_fw` is in the log and `2da:`
appears **zero** times. The hook installed and the function was never called,
so no firmware image is ever handed to the PSP one at a time.

That is not the same as "no firmware was loaded". HWLibs has a second path
for GC — backdoor autoload — and the symbols say so:

```
_gc_allocate_fw_backdoor_autoload_buffer   126aa32f
_gc_copy_backdoor_autoload_fw_10_1         126ad312
_gc_fw_backdoor_autoload_config_10_1       126acbf8
_gc_fw_backdoor_autoload_validate_10_1     126ace63
_gc_fw_autoload_is_completed               126ac55a   <- 2am forces this to 1
```

The driver fills a buffer with every image, points the hardware at it, and
the PSP loads the lot; `_gc_fw_autoload_is_completed` polls for that to
finish. 2am patches that poll to report "done" unconditionally, which says
nothing about whether the buffer was ever filled. 2db hooks the four
preparation steps (observe only) to find out. If they never run, nothing was
ever loaded, the command processor cannot be executing, and the unconsumed
KIQ packet is fully explained — with 2am having reported success for a load
that never happened.

Also fixed: `RBGPU_MAX_DIAGS` was 96 and the capture contained exactly 96
entries, so every new key (`fw_calls`, `stamp_calls`, `setres_ret`, ...) was
silently dropped and read as absent. Raised to 192. Absent has to mean
absent. The log, not the diag table, is what proved 2da never fired.

`_TtlRtsLoadFirmware` (0x12630130) is the only caller of `_ipi_psp_load_fw`,
installed by `_TtlSetRts` (0x1262fad8) at RTS slot +0x358; nothing in HWLibs
calls that slot, so the per-image path is driven from outside HWLibs.

## 2dc (2026-10-12): everything initialises — the GFX block is never powered up

Second SSH capture (logs/20261012-165744-plugin, UUID 2F57DF20 = build/out,
147 diag entries so absence is now real). Two of my earlier conclusions were
wrong and are corrected here:

- **`_ipi_psp_load_fw` never being called is not a fault.** The real load path
  is `_psp_np_fw_load`, which ran for **111 ms** (12029802247 -> 12141249718)
  and returned 0. Firmware *is* loaded.
- **The backdoor-autoload chain never running is not a fault either.** Those
  symbols are the `_10_1` (Navi 1x) variants; GC 10.3 does not use them. All
  four 2db hooks installed and none was called, which is expected.

And the return-value convention, which had been read as failure:

```
_gc_hw_init returned 0                  inner HWLibs status, 0 = success
_ipi_gc_hw_init returned 0xE188A001     IPI wrapper, bool in AL -> 1 = success
_ipi_psp_hw_init returned 0x00280001    AL = 1 = success
_ttl_hw_init returned 0xE1882001        AL = 1 = success
```

The upper bits are leftover register content, not an error code; the hooks
print the whole EAX. This matches the older data, where `_gc_sw_init returned
2` (status 2 = failure) paired with `_ipi_gc_sw_init -> 0` (bool false). So
**PSP hw init, GC hw init and TTL hw init all SUCCEEDED this boot**, with the
yellow_carp firmware loaded.

So nothing in Apple's init is failing any more. The one deliberate gap is
ours:

```
slot_internal_hw_init entered
2as DMA VERIFY: carve-out range 0xf400000000..0xf4ffffffff
2as: SMU DMA addr NOT carve-out verified -> SKIPPING rb_smu13_bringup
```

`rb_smu13_bringup` never runs, so the SMU is never given its driver table and
never asked for anything. On this APU the SMU owns GFX power and clocks
(GFX_DPM, GFXOFF, DS_GFXCLK are all in the enabled-feature mask from the
third probe), so with no SMU interaction the GFX block stays gated: the CP
cannot execute the KIQ packet no matter how correctly the ring is set up.
That is a complete explanation of `waitForHwStamp` timing out, and it is the
last thing standing.

Why the gate trips, and it is not what the comment assumed: the plugin calls

```c
smu_cgs_alloc_memory(ctx, 0x1000, 0x1000, 1, &g_dma_handle, &g_dma_gpu, &g_dma_cpu)
    -> handle = 0x0FFFF000
       gpu    = 0xffffffe0e19f4000
       cpu    = 0xffffff950d0d3580
```

and verifies `g_dma_gpu`, which holds a kernel VA. `_smu_cgs_alloc_memory`
(0x126661aa) takes seven arguments and fills three out-pointers from one
result struct: out1 <- [-0x50], out2 <- [-0x48], out3 <- [-0x30]. Of the three
observed values only **out1 = 0x0FFFF000** is not a kernel pointer: it is page
aligned and sits just under 0x10000000, which is the aperture size the TTL
input block reports at +0x68. So out1 is very likely the GPU MC address and
the plugin has been verifying the wrong field since 2ak.

Note also that the carve-out range in the check may itself be the wrong test:
an SMU driver table on an APU normally lives in GTT with a GART MC address,
not in the 0xF4_xx carve-out, so "must be inside the carve-out" would reject a
perfectly valid address.

This is a decision for the user, not an inference to act on: letting
`rb_smu13_bringup` run sends SetDriverDramAddrHigh/Low and TransferTable, and
the SMU then DMAs into whatever address it was given. Confirming which field
is the MC address must come first.

### 2dd (2026-10-12): confirm which output is the MC address, before trusting it

User's decision: prove the field rather than infer it. `slot_internal_sw_init`
now makes a SECOND 4 KB allocation and logs both result triples plus the
deltas. An address moves by about the allocation size between two
allocations; an opaque handle does not. Allocation only — no SMU message is
sent and nothing else touches hardware, and an armed boot already makes one
such allocation that is never freed (`_smu_cgs_free_memory` does not exist in
HWLibs).

Diag keys `a1_handle`, `a2_handle`, `a1_gpu`, `a2_gpu`, `a_dhandle`, `a_dgpu`,
`a2_ret`. If `a_dhandle` is near 0x1000 and `a_dgpu` is not, out1 is the GPU
MC address and the 2ak/2as gate has been verifying the wrong field all along.

## 2de (2026-10-12): the three allocation outputs were rotated — and `dma.cpu` pointed at a handle

The 2dd confirm boot (logs/20261012-171153-plugin, UUID DCB971D1) answered
more than it was asked. Two 4 KB allocations:

| field | first | second | delta | what it is |
|---|---|---|---|---|
| out1 "handle" | 0x0FFFF000 | 0x0FFFE000 | **-0x1000** | GPU (MC) address, allocated downward from the 0x10000000 aperture |
| out2 "gpu" | 0xffffffe0e18c8000 | 0xffffffe0e18c9000 | **+0x1000** | CPU mapping of the 4 KB buffer |
| out3 "cpu" | 0xffffff950b89c520 | 0xffffff950b89c550 | **+0x30** | a 48-byte descriptor — the handle |

Two separate 4 KB allocations cannot both have "CPU pointers" 48 bytes apart,
so out3 is not a buffer. All three names were rotated by one position.

**The part that matters for safety:** `dma.cpu = g_dma_cpu` was out3, and
`rb_smu13.c:311` does `copy(s->dma.cpu, data, want)` — it memcpy's the whole
driver table through that pointer. Had `rb_smu13_bringup` ever run, it would
have written hundreds of bytes over a 48-byte kernel object. The 2ak/2as gate
was added to stop the SMU DMA-ing to a bogus address; what it actually
prevented was the driver corrupting the kernel heap itself. Confirming the
field before enabling, rather than trusting the inference, is what caught it.

2de therefore:

- renames the globals to `g_dma_mc` / `g_dma_cpu` / `g_dma_handle` by what the
  deltas prove, and fixes the typedef's parameter names to match, so the
  ordering cannot quietly rot again;
- sets `dma.mc = g_dma_mc` (out1) and `dma.cpu = g_dma_cpu` (out2);
- replaces the carve-out test with a GART test. The old one could never pass:
  an APU driver table lives in the GART aperture, not the 0xF4_xx carve-out.
  The new check is nonzero, page aligned, below the 0x10000000 aperture the
  TTL input reports at +0x68;
- gates the bringup itself behind **`-rbgpusmubringup`**, off by default.
  A correct address is not permission to talk to the SMU:
  `rb_smu13_bringup` sends SetDriverDramAddrHigh/Low and TransferTable, after
  which the SMU DMA-writes its table to that address. That is a message group
  beyond queries and the user's decision per CLAUDE.md, so fixing the check
  must not silently start sending them.

Next boot (no `-rbgpusmubringup`) should log `2as: MC address verified
(0x0FFFF000) but -rbgpusmubringup not set`, with `smu_dma_mc_ok 1` and
`smu_bringup_armed 0` — proving the address path is right with nothing sent.
Turning the SMU on is then a separate, deliberate step.

### 2de confirmed on hardware (2026-10-12)

logs/20261012-172708-plugin, UUID 770C9F8A = build/out:

```
[Check 2] smu_cgs_alloc_memory ret=0, mc=0xffff000, cpu=0xffffffe0de4dc000, handle=0xffffff8ba1ff09a0
2dd: deltas mc=-4096 cpu=4096 handle=96 (expect -0x1000, +0x1000, small)
2as DMA VERIFY: sane=1 ... mc_is_kernel_va=0 (would be the old wrong field)
2as: MC address verified (0xffff000) but -rbgpusmubringup not set -> SKIPPING (no SMU message sent)
```

`mc_sane 1`, `smu_dma_mc_ok 1`, `smu_bringup_armed 0`. The remap holds on a
second boot with different addresses, `dma.cpu` now points at the real 4 KB
buffer, and the whole address path is verified with nothing sent to the SMU.

Note for the next step: the plugin never calls `rb_smu13_allow()`, so only
`RB_SMU13_ALLOW_QUERY` is permitted. With `-rbgpusmubringup` alone,
`rb_smu13_bringup` would pass its two query steps and then have
`set_driver_table_location` refused by the policy — safe, but a no-op. Testing
the hypothesis needs `RB_SMU13_ALLOW_TABLES` as well, which is the user's
decision under the CLAUDE.md rule.

What `rb_smu13_bringup` actually sends, in order:

1. `check_fw_version` — QUERY, already allowed
2. `get_enabled_mask` — QUERY, already allowed
3. `set_driver_table_location` — **SetDriverDramAddrHigh(0)** and
   **SetDriverDramAddrLow(0x0FFFF000)**: tells the SMU where the driver table
   lives. TABLES group.
4. `set_default_dpm_table` -> `update_table(DPMCLOCKS)` — a **TransferTable**,
   after which the SMU DMA-writes roughly 300 bytes of clock table into that
   address and the plugin reads it back through `dma.cpu`. TABLES group.

Blast radius, now that the address is verified: 0x0FFFF000 is a GPU address
inside the GART aperture, pointing at a buffer allocated through Apple's own
CGS allocator, so the SMU writes into our own 4 KB page. The residual risk is
that the GPU address space is not mapped as assumed, in which case the write
lands elsewhere in GPU-visible memory — bounded by the GART aperture, not host
kernel memory, and no longer the kernel-heap corruption 2de found.

Necessary but possibly not sufficient: `rb_smu13_bringup` does not touch
GFXOFF. If the GFX block is gated by GFXOFF rather than by a missing driver
table, a second step (`RB_SMU13_ALLOW_GFXOFF` plus a DisallowGfxOff call)
would be needed on top.

## 2df (2026-10-12): TABLES allowed — the first SMU write beyond queries

User's decision, asked and given on 2026-10-12: allow `RB_SMU13_ALLOW_TABLES`
so `rb_smu13_bringup` can actually run. Chosen over probing GFX registers
first (a read of a gated block can stall the SMN bus — that is what the boot
6/7 RSMU timeouts were) and over allowing GFXOFF at the same time (nothing
calls it yet, so it would be permission without a user).

`rb_smu13_allow(&g_smu, RB_SMU13_ALLOW_TABLES)` is called in exactly one
place, under `-rbgpusmubringup`. In order, the chip sees:

```
check_fw_version, get_enabled_mask    QUERY, reads only
SetDriverDramAddrHigh(0)              record the driver table address, high
SetDriverDramAddrLow(0x0FFFF000)      record the driver table address, low
TransferTable(DPMCLOCKS)              the SMU DMA-WRITES ~300 bytes of clock
                                      table there; the plugin reads it back
```

The address is the one 2dd/2de verified: a GART address pointing at a 4 KB
page from Apple's own CGS allocator, so the SMU writes into our own buffer.
The remaining uncertainty is whether the GPU address space is mapped as
assumed; if not the write lands elsewhere in GPU-visible memory, bounded by
the GART aperture rather than host kernel memory.

**The step verifies itself.** The clock values logged afterwards
(`min_gfx`, `max_gfx`, the SOC levels, the DF p-states) can only be plausible
680M numbers if the SMU really wrote where we expect. Zeros or nonsense mean
the mapping is wrong — learned in one boot, without damage. New diag keys
`smu_nmsgs`, `smu_writes`, `smu_refused` record the message accounting.

Necessary but possibly not sufficient: bringup does not touch GFXOFF. If the
GFX block is gated by GFXOFF rather than by a missing driver table, a second
step (`RB_SMU13_ALLOW_GFXOFF` plus a DisallowGfxOff call) follows.

## 2dg (2026-10-12): the 2df boot PANICKED inside configureDevice

The 2df boot (SMU TABLES allowed, `-rbgpusmubringup`) panicked. There is no
`.panic` file: `debug=0x100` halts on the panic screen instead of rebooting,
so nothing was written to /Library/Logs/DiagnosticReports (the newest kernel
panic there is 2026-10-11). A photograph of the stuck console is the only
record, and it shows only the bottom of the backtrace:

```
IOAcceleratorFamily2 : __ZN22IOGraphicsAccelerator25startEP9IOService + 0x2dc
AMDRadeonX6000       : __ZN37AMDRadeonX6000_AMDGraphicsAccelerator5startEP9IOService + 0x149
rembrandtgpu.plugin  : __ZL22hooked_accel_start_2cnPvS_ + 0x37
mach_kernel          : IOService::startCandidate / probeCandidates / doServiceMatch
```

**The frame is identifiable even though the panic string is not.** The return
address `IOGraphicsAccelerator2::start+0x2dc` is 0x14e4de4e in the extracted
kext, and the instruction before it is

```
14e4de3f  movq (%r14),%rcx          ; r14 = the accelerator
14e4de42  movq %r14,%rdi
14e4de45  movq %rax,%rsi            ; rax = this->0x368, the IOPCIDevice
14e4de48  callq *0x9c0(%rcx)        ; vtable slot 0x9c0
```

Slot 0x9c0 is pure virtual in `__ZTV22IOGraphicsAccelerator2` and resolves in
`__ZTV43AMDRadeonX6000_AMDNavi21GraphicsAccelerator` (dump offset +0x9d0, the
address point being symbol+0x10) to

```
AMDRadeonX6000_AMDGraphicsAccelerator::configureDevice(IOPCIDevice *)
```

So **the panic is inside `configureDevice` or deeper**, in a frame the photo
cut off. `configureDevice` (0xbe3c938) calls `initLinkToPeer` and
`createHWInterface(IOPCIDevice *)` — which is where TTL init runs, and
therefore where `slot_internal_hw_init` and 2df's `rb_smu13_bringup` run.
That is exactly the code 2df newly enabled, and it is reached before
`powerUpHW` (which the 2cx log shows running *after* `start()` returns, not
inside it — the 2cr comment saying otherwise is wrong). The SMU bringup is
the prime suspect, but nothing yet proves the fault address, so this is a
located frame, not a diagnosis.

**The NVRAM summary does not belong to that boot.** `rbgpu-diag-prev` reads

```
armed=1 found_fb_routed=1 found_fb_calls=2 found_fb_refused=2
ttl_raw_init_ret=0 ttl_hw_init_ret=ffffffffde370001 gc_sw_init_ret=1 setres_ret=0
```

and `setres_ret` is only set during `powerUpHW`, after `start()` returns — a
point the panic boot never reached. So this is an earlier (2cy/2db-era) armed
boot, not the 2df one. Do not read it as the panic boot's record.

Every SMU key from 2df (`smu_nmsgs`, `smu_writes`, `smu_refused`, `gfx_min`,
`gfx_max`, ...) is absent from NVRAM anyway, because they were never added to
`kRbgpuNvramKeys`: **2df's entire purpose was to learn those numbers and no
boot could report them.**

Two fixes, both in src/plugin/RembrandtGPUPeer.cpp:

- The 2df SMU keys are now in `kRbgpuNvramKeys`, so the next armed boot
  reports what the SMU step did even if it dies.
- `rbgpuConsumeArming` flushes the `rbgpu-arm` removal with
  `IONVRAM-FORCESYNCNOW-PROPERTY` immediately, instead of relying on the later
  `saveDiagToNvram` flush. An armed boot can panic at any point after the
  consume, and the disarm has to be durable before the accelerator is let near
  the hardware. (Whether the 2df boot's removal really failed to reach flash
  is not proven; `rbgpu-arm` reads 1 today, but the machine has since been
  rescued with `-rbgpuoff`, under which the peer never runs and never consumes
  it, so it may have been re-armed by hand. The flush is correct either way.)

Two things to change before the next armed boot:

- `rbgpu-arm` is currently **1**. Any boot without `-rbgpuoff` arms the
  accelerator.
- `debug=0x100` should come out of boot-args. It is why this panic left a
  photograph instead of a report. Without it macOS reboots on panic and writes
  the full panic log, and with the durable disarm above that reboot comes up
  unarmed instead of looping.

### Next boot, decided 2026-10-12

Repeat 2df with the SMU bringup still on, but let the machine panic properly:
drop `debug=0x100` (= DB_LOG_PI_SCRN, which prints the panic to the screen and
stays there) so macOS reboots and writes a real `.panic` file with the frames
above `configureDevice` and the panic string. `keepsyms=1` stays, so the report
is symbolicated.

Order matters: **install the 2dg build first**. The durable `rbgpu-arm` flush
is what makes the panic reboot come up unarmed instead of looping, and it only
exists in this build.

```
sudo tools/efi.sh install                     # 2dg build, UUID 1687501B
sudo tools/efi.sh bootarg remove debug=0x100
sudo tools/efi.sh on                          # drop -rbgpuoff
sudo tools/efi.sh status                      # check boot-args and the UUID
```

`rbgpu-arm` is already 1, so the next boot is armed. Expect: panic, automatic
reboot, desktop on the second boot.

Read it there, **before any further reboot** — the 2cw rotation only keeps two
boots, and the recovery boot pushes the panic boot's summary into
`rbgpu-diag-prev`:

```
ls -lt /Library/Logs/DiagnosticReports/*.panic | head
nvram rbgpu-diag-prev          # the panic boot's numbers
tools/plugin_check.sh
```

Known gap: the diag summary reaches NVRAM through an async thread_call, so if
the panic follows an SMU message within microseconds the SMU keys may still be
missing. The `.panic` report is the primary artifact; the NVRAM summary is the
bonus.

## 2dh (2026-10-12): the SMU hung on TransferTable, and the teardown panicked

The 2dg boot (UUID 1687501B, `debug=0x100` removed, `-rbgpusmubringup` on)
panicked again and **still wrote no `.panic` file**: the panic starts, prints
its backtrace, and never reaches "Panic in memory stackshot succeeded", so
nothing is committed to NVRAM for the next boot to write out. Dropping
`debug=0x100` does not help with a half-panic. The user screen-recorded it
instead (IMG_5480.MOV, 30 fps); frames pulled with AVFoundation are the
record. Key frames: logs/20261012-video-panic/ (gitignored).

### What the SMU did — the first answer from the hardware

```
RembrandtGPU: 2df: SMU accounting after bringup: msgs=7 writes=21 refused=0
              last_msg=0x0f last_resp=0x0
RembrandtGPU: rb_smu13_bringup failed: no answer from the SMU (-4)
RembrandtGPU: slot_internal_hw_exit entered
RembrandtGPU: rb_smu13_prepare_unload failed: SMU context dead after a timeout (-8)
RembrandtGPU: slot_internal_sw_exit entered
```

7 messages, 21 register writes (3 per message), **0 refused**. `last_msg`
0x0F is `TransferTableSmu2Dram`, so every earlier message was answered and
the transfer was not (`last_resp=0`: response register still 0, not 1/OK and
not an error code). `rb_smu13_bringup`'s own sequence accounts for six
(GetSmuVersion, GetDriverIfVersion, GetEnabledSmuFeatures,
SetDriverDramAddrHigh, SetDriverDramAddrLow, TransferTableSmu2Dram); which
message is the seventh is unresolved and worth confirming from the log.

The address messages only record a value; the transfer is the first time the
SMU tries to *use* it. It accepted the address and hung on the DMA.

**Why, and this reverses 2de's reasoning.** Linux allocates every SMU table
for this exact chip in VRAM, not GTT — docs/linux-ref/yellow_carp_ppt.c:159-164:

```c
SMU_TABLE_INIT(tables, SMU_TABLE_WATERMARKS, sizeof(Watermarks_t),
               PAGE_SIZE, AMDGPU_GEM_DOMAIN_VRAM);
SMU_TABLE_INIT(tables, SMU_TABLE_DPMCLOCKS,  sizeof(DpmClocks_t),
               PAGE_SIZE, AMDGPU_GEM_DOMAIN_VRAM);
SMU_TABLE_INIT(tables, SMU_TABLE_SMU_METRICS, sizeof(SmuMetrics_t),
               PAGE_SIZE, AMDGPU_GEM_DOMAIN_VRAM);
```

So the driver table belongs in the **carve-out** (MC 0xF4_0000_0000-
0xF4_FFFF_FFFF), and 2de's claim that "an APU driver table lives in the GART
aperture, not the 0xF4_xx carve-out" is wrong. 2ak/2as were right about the
domain and wrong only about which *field* to test; 2de fixed the field and
then replaced a correct domain test with an incorrect one.

The address we handed the SMU was `0x0FFFF000`, which sits just under
`0x10000000` — the value the TTL input block reports at +0x68, and the size
the 2dd deltas showed the allocator counting down from. **Hypothesis, not yet
proven:** Apple's `smu_cgs_alloc_memory` returns an *offset into the local
frame buffer*, not an absolute MC address, so the true MC address is
`FB_LOCATION_BASE + offset` = `0xF4_0FFF_F000`. The SMU was pointed at
`0x0FFFF000`, which it cannot reach, so the transfer never completed. Worth
noting the type argument already asks for FB: the plugin passes smu memory
type 1, and `_convert_smu_memory_type_to_cgs` (0x126660c0) maps 1 -> cgs 0 =
`CGS_GPU_MEM_TYPE__VISIBLE_FB`.

### Why it panicked — our slot table meets Apple's teardown

The panic is NOT in the bringup. It is in the failure path, which had never
run before. Faulting frame, from the video:

```
AMDRadeonX6800HWLibs : __ZN14AmdTtlServices22cosReleaseMemoryHandleEPvS0_ + 0x14
... -> _ttl_initialize -> AmdTtlServices::initialize(_TtlLibraryInitializationInput*)
... -> AMDGraphicsAccelerator::configureDevice -> ::start + 0x149
```

`cosReleaseMemoryHandle` (0x1265d9c0), with +0x14 the faulting instruction:

```
1265d9c4  testq %rdi,%rdi ; je  -> assert, return 2     cos context non-null
1265d9c9  testq %rsi,%rsi ; je  -> assert, return 2     handle non-null
1265d9ce  movq  (%rsi),%rax                             load the handle's table
1265d9d4  callq *0x28(%rax)      <-- +0x14, THE FAULT
```

Both null guards passed, so the handle was non-NULL and its contents were
garbage. It comes from `_smu_internal_sw_exit` (0x1266813e), which runs
immediately after our slot:

```
12668148  movq 0x660(%rdi),%rax   ; our slot_internal_sw_exit
12668157  callq *%rax             ; "slot_internal_sw_exit entered" in the log
12668161  movq 0x340(%rbx),%rsi   ; NO null check
1266816b  callq _smu_cos_release_mem_handle   -> cosReleaseMemoryHandle -> PANIC
12668170  movq 0x348(%rbx),%rsi   ; this one IS null-checked
```

`ctx+0x340` is written in exactly one place, `_smu_get_fw_constants`
(0x12666091), from a caller-provided struct. `_smu_sw_init` zeroes `ctx+0x348`
explicitly but never `ctx+0x340`, so Apple's code assumes its own
`_smu_internal_sw_init` path always leaves a releasable handle there — and we
replaced that path with our slot table.

**Fix (hook layer, not yet written):** `slot_internal_sw_exit` should zero
`ctx+0x340` before returning. Apple's unconditional release then passes NULL,
`cosReleaseMemoryHandle` takes its own `testq %rsi,%rsi` guard, logs its
assert and returns 2 — Apple's own path, not a bypass. It leaks one handle on
a failed init that ends the driver anyway; a leak beats a panic during
bring-up.

**Correction:** `_smu_cgs_free_memory` really does not exist, but
**`_smu_cgs_release_memory` (0x1266633a) does**, as does
`_smu_cos_release_mem_handle` (0x12665b12). `slot_internal_sw_exit`'s
`orig_smu_cgs_free_memory` is therefore always NULL and it has never freed the
two 4 KB allocations from 2dd.

### Next step is the user's call

Two separate things, in this order:

1. **Stop the panic** (zero `ctx+0x340` in `slot_internal_sw_exit`). Without
   it every SMU failure costs a panic and a power cycle, so no further SMU
   experiment is readable.
2. **Fix the address**, then retry the transfer. The experiment that settles
   the offset-vs-MC question without sending anything: log
   `MMMC_VM_FB_LOCATION` alongside the allocator's value and check whether
   `base + 0x0FFFF000` lands inside the carve-out. Changing the address the
   SMU is given is a new hardware write and the user's decision.

## 2di (2026-10-12): the handle fix, and an absolute MC address

Claude now writes the hook layer too (the Gemini split ended 2026-10-12), so
both 2dh findings are implemented rather than specified.

**The panic fix.** `slot_internal_sw_exit` zeroes `ctx+0x340` before
returning, because Apple's `_smu_internal_sw_exit` releases that handle
immediately afterwards with no null check (0x12668161). With NULL,
`cosReleaseMemoryHandle` takes its own `testq %rsi,%rsi` guard, logs its
assert and returns 2 — Apple's own path. One handle leaks on an init that is
failing anyway. The old value is logged as `sw_exit_h340` so we can see what
it actually was.

**The address.** `smu_cgs_alloc_memory`'s out1 is an offset into the local
frame buffer, not an address the SMU can reach — the 2dg boot sent
`SetDriverDramAddrHigh(0)` / `Low(0x0FFFF000)` and the SMU hung on the
transfer. `slot_internal_sw_init` now reads the carve-out out of MMHUB
instead of assuming it:

```
MMHUB base_idx 1 = 0x1a000 (discovery, probe 1); offsets from rb_regs.h
  MMMC_VM_FB_LOCATION_BASE  dword 0x1a8ec  byte 0x6a3b0
  MMMC_VM_FB_LOCATION_TOP   dword 0x1a8ed  byte 0x6a3b4
  MMMC_VM_FB_OFFSET         dword 0x1a8d7  byte 0x6a35c
```

`window_limit` is 0x80000 (512 KB, logged every boot), so these are inside the
mapped window; the code checks that anyway and reads nothing if they are not.
The registers hold 16 MB units, so `fb_base = (reg & 0xFFFFFF) << 24`, and
the address handed to the SMU becomes `fb_base + out1` — expected
`0xF4_0FFF_F000`, with a high word of 0xF4 instead of the 0 that failed.

`dma.mc` is zero unless the whole 4 KB lies inside the range MMHUB reported,
so an unreadable or implausible FB_LOCATION stops the bringup rather than
guessing. The bringup itself still needs `-rbgpusmubringup`.

New diag keys, all in NVRAM: `fb_base_reg`, `fb_top_reg`, `fb_off_reg`,
`fb_regs_sane`, `fb_regs_oow`, `mc_abs_hi`, `mc_abs_lo`, `mc_abs_ok`,
`sw_exit_h340`, `sw_exit_h340_cleared`.

Build: UUID CBA2E0AA, hosttest passes.

What the next armed boot decides:

- `TransferTableSmu2Dram` either completes or times out again. If it
  completes, the clock values print and the offset theory is proven; if it
  times out with `last_msg=0x0f` again, the address is still not what the SMU
  wants and the write landed inside the carve-out past the GOP framebuffer,
  not in host memory.
- Either way the boot should no longer panic, because the teardown path is
  fixed. `sw_exit_h340_cleared=1` with no panic is the proof.

### 2di confirmed on hardware (2026-10-12): the SMU link works

logs/20261012-195941-plugin, armed boot, no panic, desktop reached:

```
2di: MMHUB FB_LOCATION_BASE=0x0000F400 TOP=0x0000F4FF FB_OFFSET=0x00000780
2di: alloc offset 0xffff000 + fb_base 0xf400000000 = MC 0xf40ffff000; inside carve-out=1
2df: SMU accounting after bringup: msgs=7 writes=21 refused=0 last_msg=0x0f last_resp=0x1
[Check 3] rb_smu13_bringup succeeded
[Check 4] SOC levels: 400 600 720 800 900 1028 1028 1200 MHz
[Check 4] DF P-state 0: fclk=1800 memclk=2400 volt=3639 ; 1: fclk=500 memclk=1000 volt=2600
```

`last_resp` is 1 where 2dh had 0, `bringup_err=0`, and the SOC/DF values are
real Rembrandt numbers, so the FB-offset -> MC translation is proven, not
inferred. First two-way SMU transaction in the project: the SMU DMA-wrote its
table into our page at an address derived from MMHUB registers.

Two things the boot did NOT settle:

- **The 2di panic fix is unproven.** `sw_exit_h340` is absent: the bringup
  succeeded, so the teardown path never ran. Correct by reading, untested on
  hardware.
- **`gfx_min` and `gfx_max` are both 0** while every neighbouring field
  decoded correctly, and `stamp_calls=1 stamp_ret=0` - the CP still does not
  consume the KIQ packet. Linux reads max/min sclk from those same two fields
  (yellow_carp_ppt.c:895,925), so this is not a parsing error: the SMU is
  reporting that the GFX block has no clock range.

So 2dc's hypothesis (no SMU -> GFX gated -> CP dead) was necessary but not
sufficient, exactly as 2de/2df warned. The SMU is up and the GFX block is
still not.

## 2dj (2026-10-12): ask the chip why the GFX clock range is 0

Two causes fit `gfx_min=gfx_max=0`: GFX is powered but parked in GFXOFF, or
GFX DPM was never started. They need different fixes, so settle it before
asking for a new message group.

`rb_smu13_get_gfxoff_status` (already in rb_smu13.c) is a plain read of
SMUIO_GFX_MISC_CNTL - no message, no mailbox write, so no policy group and
nothing new sent to the chip. SMUIO is alive, having just carried seven
messages; this is not one of the gated-GC reads behind the boot 6/7 RSMU
timeouts, and no GC register is touched.

Logged as `[Check 6]` after the clock dump. Diag keys `gfxoff_status`
(0 in GFXOFF, 1 leaving, 2 not in GFXOFF, 3 entering) and `gfxoff_read_err`,
both in NVRAM so a boot that dies still reports them.

Build: UUID C9731AE2, hosttest passes. Same boot-args as 2di, still armed.

What it decides:

- **status 0 (in GFXOFF)**: GFX is parked. The lever is `DisallowGfxOff`,
  which needs `RB_SMU13_ALLOW_GFXOFF` - the user's decision.
- **status 2 (not in GFXOFF)**: GFX is powered and the clock range is 0
  because GFX DPM was never started. GFXOFF is then the wrong lever and
  `SetHardMinGfxClk` / `SetSoftMaxGfxClk` (a different group again) is the
  question.
- **read error**: rb_smu13_init never located SMUIO, and the question stays
  open.

### 2dj answered (2026-10-12): NOT in GFXOFF — that door is closed

logs/20261012-201650-plugin, UUID C9731AE2, armed, no panic:

```
[Check 6] GFXOFF status = 2: NOT in GFXOFF
gfxoff_read_err=0   gfx_min=0   gfx_max=0   stamp_ret=0
```

The read succeeded, so this is definitive: GFX is **not parked**.
`RB_SMU13_ALLOW_GFXOFF` + `DisallowGfxOff` would have been a permission spent
on nothing. One boot, one closed door, no new message sent.

The zeros were checked for the boring explanation and survive it: our
`struct rb_smu13_dpm_clocks` is 304 bytes with `min_gfx_clk` at +296 and
`max_gfx_clk` at +300, field-for-field identical to Linux's `DpmClocks_t`
(smu13_driver_if_yellow_carp.h:120-140, all NUM_* = 8 except NUM_DF_PSTATE 4),
and `rb_smu13_update_table` copies `table_size()` = `sizeof` = 304 bytes. Both
fields are inside the copy, so the zeros are real data from the SMU, not a
short read.

## 2dk (2026-10-12): the GC register dump was reading the wrong addresses

The same boot ran two GC register readers a few ms apart and they disagreed:

```
2ba CORRECT: RLC_CNTL=0x00000001 RLC_STAT=0x00000025 BOOTLOAD=0x00000001
2ap DIAG [post-gc-hw-init]: RLC_CNTL=0x00000000 RLC_STAT=0x00000000 CP_STAT=0xffffffff
2ap DIAG [post-gc-hw-init]: RLC_STAT=0x00000000 DIFFERS from Raphael 0x25 - investigate!
```

`rbgpu_dump_gc_regs` computed `0xa000 + 0x4c00` as a **byte** address. The
gc_10_3_0_offset.h offsets are DWORD indices and the segment bases are DWORD
too, so the byte address is `(seg + offset) * 4`. The 2ba block inside
`hooked_gc_hw_init` already had this right.

So **RLC_STAT really is 0x25** - the Raphael reference value, RLC firmware
present in instruction RAM - and the "DIFFERS ... investigate!" line was our
own bug. `CP_STAT=0xffffffff` was an undecoded address, not a register value.
Two of the project's own alarms were false, and the firmware-present evidence
that justifies the 2am bypass was there all along.

Consequence worth stating plainly: **no boot has ever validly read CP_STAT.**
The CP's actual state is still unmeasured.

2dk rewrites the dump:

- `(seg + dword offset) * 4` throughout. Seg1 = 0xa000 is proven on hardware
  (it is what yields RLC_STAT 0x25 and RLC_CNTL 1).
- Seg0 is **not** proven - the plugin's synthetic discovery table carries only
  MP0/MP1, so there is no GC base to read - so CP_STAT is read at BOTH
  candidates, AMD's `gc_10_3_0` convention 0x1260 and the old assumption 0,
  and the values say which decodes. 0xffffffff at both means seg0 is still
  unknown and the boot measured nothing, which the log says in those words
  rather than implying a result.
- Adds RLC_GPM_STAT, CP_ME_CNTL and CP_MEC_CNTL, and decodes the halt bits
  (ME/PFP/CE from CP_ME_CNTL 28/26/24, MEC1/MEC2 from CP_MEC_CNTL 30/28).
- Called additionally from the `submitSetResourcesPacket` ENTER hook, so for
  the first time the CP is read **while the KIQ packet is in flight**.

All reads. No GC register is written. Risk is bounded: an undecoded address
returns 0xffffffff, which this boot already demonstrated harmlessly, and
GFXOFF status 2 says the block is powered.

Build: UUID 4524E5FA, hosttest passes. Same boot-args, still armed.

What it decides: whether the CP microcode is halted (ME/PFP/CE/MEC halt bits
set) or running but not fetching. Those are different problems, and nothing so
far distinguishes them.

### 2dk on hardware (2026-10-12): CP not halted, CP_STAT 0 — but the base is unproven

logs/20261012-202627-plugin, UUID 4524E5FA, armed, no panic. Identical at both
read points, `post-gc-hw-init` and the new `pre-kiq-submit`:

```
RLC_CNTL=0x00000001  RLC_STAT=0x00000025  RLC_GPM_STAT=0x00b40017
seg0=0x1260 -> CP_STAT=0x00000000 CP_ME_CNTL=0x00000000 CP_MEC_CNTL=0x00000000
seg0=0x0000 -> CP_STAT=0x00000000
ME_HALT=0 PFP_HALT=0 CE_HALT=0 MEC1_HALT=0 MEC2_HALT=0
```

Taken at face value that is a CP which is powered, **not halted**, and
completely idle even at the instant the packet is submitted and the doorbell
rung - a different failure from "the microcode is not running", and it would
point at the ring/doorbell/MQD rather than at the firmware.

**But the base is not proven, and the 2dk log line overclaimed it.** The test
was `!= 0xffffffff`, and CP_STAT read `0x00000000` at BOTH candidate bases, so
it cannot tell a real zero from a dead address that reads 0. The
"seg0=0x1260 decodes" line is not evidence. Until the base is settled,
CP_STAT=0 and the halt bits prove nothing.

RLC_GPM_STAT=0x00b40017 is new and nonzero, which at least confirms seg1 keeps
decoding real data.

## 2dl (2026-10-12): settle the base, and ask whether the MEC ever fetched

Registers that cannot be zero when the block is the right one, all BASE_IDX 0,
all reads:

```
CP_RB0_CNTL  0x1de1   ring control - programmed by doStart, and even the reset
                      value carries RB_BUFSZ, so 0 means this is not the CP
CP_RB0_BASE  0x1de0   ring base address
CP_RB0_RPTR  0x0f60   the ring-state check doStart makes after startKIQ
CP_RB0_WPTR  0x1df4
GRBM_STATUS  0x0da4
```

CP_RB0_CNTL is read at both candidate bases and only one may be live; the log
says CONFIRMED, CONFIRMED-the-other-way, UNKNOWN, or ambiguous in those words,
and when it is UNKNOWN it says explicitly that CP_STAT and the halt bits prove
nothing rather than letting them be read as a result.

The one that answers the real question:

```
CP_MEC_ME1_HEADER_DUMP  0x0e2e   the header of the last packet the MEC FETCHED
```

Nonzero at `pre-kiq-submit` means the command processor really did read from
the ring, which moves the fault from "the CP never sees the work" to "the CP
saw it and did not finish it". Zero means it never fetched anything, and the
problem is upstream of the CP - doorbell, MQD, or a ring the CP cannot reach,
the same class of bug as the SMU address in 2dh/2di.

Build: UUID CB818072, hosttest passes. Same boot-args, still armed. All reads,
no GC register written.

### 2dl on hardware (2026-10-12): seg0 proven — and the header-dump claim is wrong

logs/20261012-203308-plugin, UUID CB818072, armed, no panic.

**The base is settled.** `CP_RB0_CNTL = 0x00a00000` at seg0 = 0x1260 and
`0x00000000` at the old base 0, so **seg0 = 0x1260** and the CP readings from
2dk are real data:

```
CP_STAT=0x00000000  ME_HALT=0 PFP_HALT=0 CE_HALT=0 MEC1_HALT=0 MEC2_HALT=0
GRBM_STATUS=0xa0003028
```

The CP is powered, **not halted**, and idle - including at `pre-kiq-submit`,
with the packet supposedly in flight.

**But two of the new values are fill patterns, and 2dl's own conclusion was
wrong:**

```
CP_RB0_BASE         = 0xfedcbaef
MEC_ME1_HEADER_DUMP = 0xdef0def0   at post-gc-hw-init
MEC_ME1_HEADER_DUMP = 0xdef1def1   at pre-kiq-submit
```

`0xfedcbaef` is not a ring base and `0xdef0def0` is not a PM4 header. The
header moving `def0 -> def1`, incrementing by exactly one in BOTH halves
between two reads, is a FIFO walking a pre-filled pattern buffer. So
`2dl DIAG: MEC fetched a packet: YES` is **not supported** - it was the same
overclaiming as 2dk's "seg0 decodes", one layer down. Three times now a
nonzero-or-not test has been treated as a result; the lesson is to make the
code state the pattern it sees, not a verdict the value cannot carry.

CP_RB0_BASE being a poison value is probably not itself a fault: doStart runs
with arg 0, the KIQ is a compute queue driven by MQD + doorbell, and
CP_RB0_RPTR/WPTR both read 0 exactly as doStart's own ring-state check wants.

## 2dm (2026-10-12): characterise the pattern, and ask whether the queue is live

Two read-only additions, no verdict where the data cannot support one.

**Is the header dump a pattern?** `CP_MEC_ME1_HEADER_DUMP` pops the next entry
on each read, so it is now read four times in a row plus ME2 once. The log
says which of three things it saw - stepping by 0x00010001 (a pre-filled
pattern, proving nothing about fetching), constant, or varying irregularly
(possibly real headers, PM4 type 3 having bits 31:30 = 11) - and says in those
words that a stepping pattern says nothing.

**Is the KIQ queue live?** This is the direct question and nothing has asked
it:

```
CP_HQD_ACTIVE               0x1fab   bit 0 = this compute HQD is active
CP_GFX_HQD_ACTIVE           0x1e80   the one doStart polls in waitForActiveValue
CP_HQD_PQ_BASE              0x1fb1   was the queue ever pointed at a ring
CP_HQD_PQ_DOORBELL_CONTROL  0x1fb8   bit 30 = doorbell enabled
```

If the CP is unhalted and idle while a packet is supposedly in flight, the
fault has to be that the queue was never mapped or the doorbell never routed -
and these four say so directly.

Build: UUID CD998921, hosttest passes. Same boot-args, still armed. All reads.

### 2dm on hardware (2026-10-12): the KIQ queue was never programmed

logs/20261012-204121-plugin, UUID CD998921, armed, no panic.

**The header dump is settled - it is a pattern.** Reading it four times in a
row at each point gives `def1 def2 def3 def4` then `def5 def6 def7 def0`: an
eight-entry circular buffer pre-filled with def0..def7, popping one per read.
2dl's "MEC fetched a packet: YES" is conclusively withdrawn. (The second
point printed "varies irregularly" only because the step test met the
def7 -> def0 wrap; the data itself is unambiguous. The classifier, not the
chip, was wrong.)

**The finding, at both read points including `pre-kiq-submit`:**

```
CP_HQD_ACTIVE=0x00000000  CP_GFX_HQD_ACTIVE=0x00000000
CP_HQD_PQ_BASE=0x00000000 CP_HQD_PQ_DOORBELL_CONTROL=0x00000000
```

No ring base, no doorbell, not active. The CP is unhalted and idle because
there is nothing for it to service. That explains `waitForHwStamp` completely
and moves the fault off the firmware entirely.

## 2dn (2026-10-12): startKIQ writes no register — it asks the layer below

`startKIQ` (0xbec7688) disassembled: it writes **no hardware register at all**.
It builds a 0x38-byte descriptor on the stack from the channel's own fields
(+0x84 & ~3, 0x80, +0x48, +0xb8, +0xd0, the two u64 args, 0x200), then

```
rdi = this+0x20 ; callq *0x2c0(vtable)        -> a service object
callq *0x120(that vtable) with (8, 0, descriptor, outbuf)
```

and compares three dwords of the `AMD_PM4_QUEUE_SPEC` (arg c) against the
first three dwords the callee wrote into outbuf: equal -> return 0, different
-> 0xE00002BC.

So startKIQ **asks the TTL/HWLibs layer to create the KIQ queue** (command 8)
and only checks that the me/pipe/queue it got back is the one it asked for. It
returned 0 on this chip, so the layer below reported a queue it never
programmed. That is the bug, and it is in the same place as every other one
this week: Apple's Navi 21 code path running against GC 10.3.3.

**Caveat that has to be settled first.** The `CP_HQD_*` block is a WINDOW:
`GRBM_GFX_CNTL` (dword 0x0dc2, BASE_IDX 0) selects which ME / PIPE / QUEUE /
VMID those registers refer to. An all-zero read only means "never mapped" if
the selector happens to point at the KIQ's own slot. 2dn therefore logs:

- the `AMD_PM4_QUEUE_SPEC`'s first three dwords from inside the existing
  startKIQ hook - the me/pipe/queue the KIQ is supposed to occupy;
- `GRBM_GFX_CNTL` in the register dump, beside that spec;
- a line saying in those words that a zero selector plus a zero window proves
  nothing if the KIQ lives at a nonzero me/pipe/queue.

The dump is also called from a new third point, `post-startKIQ`, so the window
is read immediately after the layer below claims the queue exists.

Build: UUID 824205E1, hosttest passes. Same boot-args, still armed. All reads.

### 2dn on hardware (2026-10-12): we have been reading the WRONG HQD slot

logs/20261012-204631-plugin, UUID 824205E1, armed, no panic.

```
2dn: startKIQ QUEUE_SPEC[0..2] = 0x00000002 0x00000001 0x00000000
     (me=2 pipe=1 queue=0); ring args a=0xf40fb28000 b=0xf40fb28800
2dn DIAG [post-startKIQ]: GRBM_GFX_CNTL=0x00000000
```

The KIQ lives at **ME 2 / pipe 1 / queue 0**, and the `CP_HQD_*` window has
been pointed at **0 / 0 / 0** for every reading taken so far. So 2dm's
"the KIQ queue was never programmed" is **withdrawn**: those all-zero values
were the wrong slot and prove nothing. The 2dn caveat was written for exactly
this case and it fired.

Also learned, and good news: the ring arguments are `0xf40fb28000` and
`0xf40fb28800` - real carve-out MC addresses. The ring itself is allocated
where it should be.

Four claims in a row (2dk "seg0 decodes", 2dl "MEC fetched", 2dm "never
programmed", and now this) have come from reading a value without first
proving the thing that makes the value meaningful. The practice that keeps
catching it is cheap and should stay: say in the code what would have to be
true for the reading to mean anything, and log when it is not.

## 2do (2026-10-12): point the window at the KIQ's own slot

Reaching ME 2 / pipe 1 / queue 0 needs `GRBM_GFX_CNTL` written - the
**plugin's first write to a GC register**. It is an index register: it selects
which ME/PIPE/QUEUE/VMID instance the windowed `CP_HQD_*` registers address
and changes no GPU behaviour by itself. amdgpu writes it around every HQD
access (`nv_grbm_select`). The old value is saved, restored immediately after,
and read back; nothing else runs concurrently, this being inside the
accelerator's single-threaded start path.

**The field layout is not trusted.** The encoding
`pipe | me<<2 | vmid<<4 | queue<<8` comes from amdgpu and is not independently
verified against this chip, so the probe does not depend on it. It reads
CP_HQD_ACTIVE / PQ_BASE / PQ_BASE_HI / PQ_RPTR / PQ_DOORBELL_CONTROL /
PQ_CONTROL / VMID at the spec's slot and, failing a hit, sweeps me 0..2 x
pipe 0..3. The right slot identifies itself: `CP_HQD_PQ_BASE` holds the ring
base >> 8, and startKIQ was handed ring MC `0xf40fb28000`, so the slot that
reads `0xf40fb280` is the KIQ's - which proves the slot and the field layout
together.

If nothing in the sweep matches, the log says in those words that this does
**not** prove the queue is missing, because a wrong field layout and a missing
queue look identical from here.

Build: UUID 54D5EF33, hosttest passes. Same boot-args, still armed.

### 2do on hardware (2026-10-12): the KIQ queue IS live — 2dm withdrawn

logs/20261012-211900-plugin, UUID 54D5EF33, armed, no panic. At the KIQ's own
slot, me=2 pipe=1 queue=0, and nowhere else in the sweep:

```
ACTIVE=0x00000001  PQ_BASE=0xffbfe800/HI=0  RPTR=0x00000000
DOORBELL=0x40000000  CTRL=0xc030860d  VMID=0
```

**The queue is programmed, active, and its doorbell is enabled** (bit 30).
2dm's "the KIQ queue was never programmed" is **definitively withdrawn**: the
all-zero readings really were nothing but the wrong window. me=1/* and
me=2 pipe=0/2 read CTRL=0x00308509 with ACTIVE=0, so those are genuinely empty
slots and the selector is working; me=2 pipe=3 aliases onto the same slot as
pipe=1.

**`GRBM_GFX_CNTL` reads back 0 even when the write took.** The windowed values
differ per selector, which proves the write works, so the readback is not a
failure signal. Noted in the code.

**The PQ_BASE mismatch is not evidence.** The probe expected `ring MC >> 8` =
0xf40fb280 and got 0xffbfe800, and the "no slot matches" line fired. That test
rested on an unverified assumption: a compute HQD's `CP_HQD_PQ_BASE` holds a
**GPUVM virtual address** translated through the queue's VMID, while startKIQ
was handed a physical MC address. There is no reason those would be the same
number, so the mismatch says nothing about the ring. Fifth time an assumption
underneath a reading needed checking before the reading meant anything.

## 2dp (2026-10-12): RPTR vs WPTR, and does the queue predate startKIQ

Two questions left, both cheap.

**Has anything actually been submitted?** `CP_HQD_PQ_RPTR` reads 0 - the CP has
consumed nothing - but WPTR was never read, and WPTR is what the driver's
doorbell write advances. The pair is the discriminator:

```
WPTR == RPTR == 0   nothing was ever submitted; the doorbell write is not
                    reaching the queue
WPTR != RPTR        work IS queued and the CP is not consuming it - the
                    failure is in execution, not submission
WPTR == RPTR != 0   the CP consumed everything, and the stamp is a different
                    problem
```

`CP_HQD_PQ_WPTR_LO` (0x1fdf) is added, and the doorbell is decoded into
enable (bit 30) and offset (bits 27:2) - the 2do value 0x40000000 is enable
set with offset 0.

**Is this queue ours?** me=2 pipe=1 queue=0 is now a measured fact rather than
a guess, so the probe runs at every dump point including `post-gc-hw-init`,
which is before startKIQ. If the queue is already ACTIVE there, it predates
startKIQ and belongs to the firmware; if it appears only afterwards, startKIQ
really did create it.

The me 0..2 x pipe 0..3 sweep is dropped: it only reprinted the same slot
through aliased selectors and cost twelve log lines per point.

Build: UUID E34337E3, hosttest passes. Same boot-args, still armed.

### 2dp on hardware (2026-10-12): startKIQ creates the queue — and my read point was wrong

logs/20261012-212642-plugin, UUID E34337E3, armed, no panic.

**Settled: the queue does NOT predate startKIQ.** Same slot, me=2 pipe=1
queue=0, at two points:

```
post-gc-hw-init  ACTIVE=0  PQ_BASE=0  DOORBELL=0  CTRL=0x00308509
post-startKIQ    ACTIVE=1  PQ_BASE=0xffbfe800  DOORBELL=0x40000000  CTRL=0xc030860d
```

`0x00308509` is exactly what the empty me=1/* slots read in 2do, so before
startKIQ this slot is genuinely empty and afterwards it is configured.
**startKIQ really does create the KIQ queue**, which means the TTL/HWLibs
layer underneath it works on this chip. Doorbell enable is set with offset 0,
and offset 0 is what amdgpu uses for the KIQ, so that is not a fault either.

**Not settled, and the fault is mine: the measurement was in the wrong place.**
`pre-kiq-submit` is `submitSetResourcesPacket`'s **ENTRY** hook - it runs
before the packet is written and before the doorbell is rung - so
`WPTR = RPTR = 0` there is exactly what should be seen and discriminates
nothing. The 2dp line "nothing submitted yet (both 0)" is accurate and useless.

## 2dq (2026-10-12): measure after the doorbell, not before it

`waitForHwStamp` is called from `submitKIQFrame` **after** commitBlock and
after the doorbell write (vtable[0x158]), so the 2cy hook is the first moment
at which the work genuinely is submitted. The register dump now runs there too,
tagged `at-stamp-wait`.

That is the discriminator this whole line of boots has been circling:

```
WPTR still 0   the doorbell / WPTR write never reached the queue - the fault
               is in submission, and the CP is idle because nothing arrived
WPTR advanced  work IS queued and the CP is not consuming it - the fault is
               in execution, with a live queue and a real ring behind it
```

Everything else about the queue is now measured and healthy: created by
startKIQ, ACTIVE, doorbell enabled at the offset amdgpu uses, ring allocated
at a real carve-out MC address, CP unhalted, RLC firmware present.

Build: UUID 53BA2078, hosttest passes. Same boot-args, still armed. All reads
except the GRBM_GFX_CNTL selector, which is saved and restored.

### 2dq on hardware (2026-10-12): THE CP EXECUTES THE PACKET

logs/20261012-213243-plugin, UUID 53BA2078, armed, no panic. At
`at-stamp-wait`, which is inside `waitForHwStamp` and therefore after
commitBlock and after the doorbell:

```
me=2 pipe=1 queue=0: ACTIVE=1 RPTR=0x00000020 WPTR=0x00000020
                     DOORBELL=0x40000000 CTRL=0xc030860d
CP_STAT=0x88000000      (0x00000000 at post-gc-hw-init, post-startKIQ and
                         pre-kiq-submit on this same boot)
```

**WPTR advanced to 0x20 and RPTR caught up to it.** The driver wrote the
SET_RESOURCES packet and rang the doorbell; `CP_HQD_PQ_RPTR` is written by
**hardware**, so the command processor fetched and consumed it. CP_STAT going
from 0 to busy at exactly this point says the same thing a second way.

**So the premise of 2cx through 2dp is wrong.** "The CP never consumes the KIQ
packet" / "the CP/MEC microcode is not running" is **withdrawn**. The command
processor runs, the queue is live, the doorbell is routed, the ring is real.

Why the project believed otherwise for fifteen milestones: **2cy returns false
from `waitForHwStamp` without ever calling the original.** Every
"waitForHwStamp -> 0 (TIMEOUT - the CP never consumed the packet)" line since
2cy is our own hook reporting its own skip, and the log text asserted a cause
it never measured. The stamp has not timed out; nobody has waited for it.

2cy itself recorded the exit: "`-rbgpustampwait` restores the real wait, for
the boot where the CP finally runs and the stamp is expected to arrive. That
flag is the test for 'is the command processor alive now'." It is alive.

Sixth and sharpest instance of the same lesson: a diagnostic that substitutes
its own value must never narrate that value as a hardware result.

## 2dr (2026-10-12): let the wait actually run

`-rbgpustampwait` is now the expected path rather than the daring one. The hook
calls the original, logs what it returned in those terms, and dumps the queue
again as `post-stamp-wait`.

```
real wait -> 1   the stamp arrived; doStart continues past
                 submitSetResourcesPacket for the first time in the project
real wait -> 0   the CP consumed the packet and the stamp still did not land,
                 so the fault is in the stamp/fence path, not in execution
```

Residual risk, unchanged from 2cy and worth stating: the wait is unbounded in
practice (5 s per poll, re-armed while the channel says keep waiting), so if
the stamp never arrives the boot wedges and costs a power cycle. The evidence
that it will arrive is RPTR = WPTR with CP_STAT busy. One-shot arming means
the recovery boot comes up unarmed either way.

Build: UUID C68112B7, hosttest passes.

**Next boot adds one boot-arg:**

```
sudo tools/efi.sh bootarg add -rbgpustampwait
sudo tools/efi.sh install
sudo nvram rbgpu-arm=1 && sudo reboot
```

### 2dr on hardware (2026-10-12): the wait ran, and it is NOT the GPU

logs/20261012-213653-plugin, UUID C68112B7, armed, no panic, no wedge.

```
2dr: the REAL wait returned 0 - the stamp did not arrive even though the CP
     consumed the packet, so the fault is in the stamp path, not execution
2cx: waitForHwStamp -> 0  (5000220287 ticks)
```

The wait really ran and gave up after **one** 5 s poll rather than re-arming
forever, so the feared wedge did not happen and the boot completed. Combined
with 2dq (RPTR = WPTR = 0x20, CP_STAT busy): **the command processor executes
the packet and the completion stamp still never arrives.**

### What waitForHwStamp actually polls (0xbe85536, disassembled)

```
waitForHwStamp(this, stamp):
  if (!isEnabled())                      return false       vtable[0x150]
  if (stamp - this->0x84 <= 0)           return true        already completed
  loop { pollWait(5 s, 100)
         checkForTimestampUpdate()                          vtable[0x1a8]
         if (this->0x84 - stamp >= 0) break
         if (chan->vtable[0x2a0]()) continue }
  return (stamp - this->0x84) <= 0
```

It polls **`channel+0x84`**, not the GPU. The refresh is
`AMDHWChannel::checkForTimestampUpdate` (0xbe851d6):

```
  if (this->0x80 == this->0x84) return 0;      <-- EARLY EXIT, reads nothing
  vtable[0x2d0](this); timestampUpdated(this)

timestampUpdated (0xbe85214):
  if (this->0xe0 == -1) return;                <-- no writeback configured
  ... reads through this->0xc0 / this->0xe0 and writes this->0x84
```

So **`+0x80` is the submitted stamp and `+0x84` the completed one**. If the
submit path never incremented `+0x80`, the refresh short-circuits and `+0x84`
can never move, however well the GPU executed - which is exactly the shape of
what 2dq measured.

## 2ds (2026-10-12): read the channel's own bookkeeping

The waitForHwStamp hook now logs `+0x80`, `+0x84`, `+0xc0` and `+0xe0` before
the wait and `+0x80`/`+0x84` after it, with a line naming which of three
situations the numbers show:

```
+0x80 == +0x84      nothing was marked submitted; checkForTimestampUpdate takes
                    its early exit and never reads the fence. Bookkeeping, not
                    the GPU.
+0xe0 == -1         no writeback address configured on the channel, so
                    timestampUpdated returns before reading anything.
otherwise           a frame is outstanding and a writeback address is set, so
                    the fence memory itself is not being updated.
```

All plain reads of the channel object. New diag keys `stamp_submitted`,
`stamp_completed`, `stamp_wb_c0`, `stamp_wb_e0`.

Also corrected: the 2cx log line still printed "TIMEOUT - the CP never
consumed the packet" on every failed wait. That text is now false and was the
sentence the project read off for fifteen milestones; it now says the stamp
was not reached and that this is **not** evidence the CP is dead.

Build: UUID 3C0AAE81, hosttest passes. Same boot-args, `-rbgpustampwait` stays.

### 2ds on hardware (2026-10-12): the bookkeeping is CORRECT — it is the fence

logs/20261012-214334-plugin, UUID 3C0AAE81, armed, no panic. One line survived
and it is the one that matters:

```
2ds: channel stamps AFTER the wait: submitted(+0x80)=1 completed(+0x84)=0 (asked-for=1)
```

**`+0x80 != +0x84`**, so `checkForTimestampUpdate` does NOT take its early
exit: the frame is properly marked submitted and the refresh really does run.
The driver's own bookkeeping is right. That leaves the third branch of the 2ds
classifier - `timestampUpdated` runs and the **fence writeback is what never
gets written**, or `+0xe0` is -1 and there is no writeback address at all.

Which of those two it is was not answered, because of two instrumentation
failures in the same boot:

- **The 512-line ring wrapped during the 5 s wait.** The request-0x14 poll
  keeps logging throughout, so by the time the log is read the `waitForHwStamp
  ENTER` line, the whole `at-stamp-wait` register dump and the 2ds BEFORE line
  are all gone. The ring's oldest entry is timestamped 13918091552 while the
  wait began near 12509590203 - about 500 lines evicted inside one wait.
- **The diag table hit 320... no, 192, exactly the cap, again.** 2db raised it
  from 96 to 192 and wrote "absent has to mean absent"; this boot filled 192
  and silently dropped `stamp_wb_c0` / `stamp_wb_e0`, the two values the whole
  step existed to capture. Same bug, same silence, one milestone family later.

## 2dt (2026-10-12): make the instrumentation survive its own experiment

Three fixes, none of them about the GPU.

- `RBGPU_MAX_DIAGS` 192 -> 320, and a **full table now says so**: the first
  dropped key logs `*** DIAG TABLE FULL at N entries - 'key' and every later
  key is being DROPPED. Absent no longer means absent.` A silent cap has now
  cost two debugging rounds; it should never cost a third quietly.
- The values read before the wait (`+0x80`, `+0x84`, `+0xc0`, `+0xe0`) are
  held in file statics and **reprinted in the AFTER line**, which is logged
  once the wait returns and therefore survives the ring wrap.
- That AFTER line now names the conclusion the numbers support: `+0xe0` of -1
  means `timestampUpdated` returns without reading any writeback, i.e. no
  fence address is configured; anything else means a frame is outstanding, an
  address is set, and the fence memory itself is not being written.

Build: UUID 41D6ABAE, hosttest passes. Same boot-args, `-rbgpustampwait` stays.

### 2dt on hardware (2026-10-12): the fence is never read — `+0xe0` is -1

logs/20261012-215649-plugin, UUID 41D6ABAE, armed, no panic. 212 diag entries,
so the cap is no longer in the way, and the AFTER line survived the ring wrap
exactly as 2dt intended:

```
2ds: channel stamps AFTER the wait: submitted(+0x80)=1 completed(+0x84)=0
     (asked-for=1) ; BEFORE sub=1 cmp=0 ;
     writeback +0xc0=0xffffffa965fcc040 +0xe0=0xffffffffffffffff
     <<< +0xe0 is -1: timestampUpdated returns WITHOUT reading any writeback
```

`timestampUpdated` (0xbe85214) opens with `if (this->0xe0 == -1) return;`, so
**that branch is taken every time**. The frame is correctly marked submitted,
the CP really executed it (2dq), `+0xc0` holds a real kernel VA - and the
completed counter has no path to be updated at all.

### What `+0xe0` actually is (AMDHWChannel::init, 0xbe848cc)

Not an address. `init` writes -1 at 0xbe84974 and the **channel index** at
0xbe84ade, and only after three gates all pass:

```
be84a9d  callq *0x330(this->0x20)   AMDHardware::registerChannel(this)
         false -> jumps away, +0xe0 stays -1
be84aad  callq *0x1c8(this)         a KIQ-channel method
         false -> jumps away, +0xe0 stays -1
be84ad1  AMDSWScheduler::addHWChannel(sched, this, this->0xc)
         false -> jumps away, +0xe0 stays -1
be84ade  this->0xe0 = (int)this->0xc
```

`timestampUpdated` then calls `this->0x20`'s `vtable[0x3b0]` to get the
scheduler and passes it `+0xe0` as an index alongside `+0xc0` and `&this->0x84`.
So **`+0xe0` is the channel's slot in the `AMDSWScheduler`, and -1 means this
channel was never registered with it.** The fence lives in the scheduler; with
no slot there is nothing to look up.

That is the same shape as the SMU bug of 2dh/2di: a per-channel registration
Apple's Navi 21 path performs that ours never completes.

## 2du (2026-10-12): name the gate that refuses

Two of the three gates are named symbols defined in X6000, so both are hooked
observe-only and the log says which one returns false:

```
__ZN26AMDRadeonX6000_AMDHardware15registerChannelEP28AMDRadeonX6000_IAMDHWChannel   0xbea9a04
__ZN29AMDRadeonX6000_AMDSWScheduler12addHWChannelEP28AMDRadeonX6000_IAMDHWChannelm  0xbe93f98
```

Each logs its arguments and return, and says in the failing case that the
consequence is `channel+0xe0` staying -1 and the fence never being read. Diag
keys `regchan_calls`, `regchan_fails`, `addchan_calls`, `addchan_fails`.

The middle gate, `this->vtable[0x1c8]`, resolves inside `startKIQ`'s address
range rather than to its own symbol, so it is an unnamed local function; if
both named gates pass and `+0xe0` is still -1, that one is the answer by
elimination and will need its own hook by address.

Build: UUID 9EE6ADFB, hosttest passes. Same boot-args, `-rbgpustampwait` stays.

### 2du on hardware (2026-10-12): both named gates PASS, every time

logs/20261012-220552-plugin, UUID 9EE6ADFB, armed, no panic.

```
regchan_calls = 25   regchan_fails = ABSENT (zero)
addchan_calls = 23   addchan_fails = ABSENT (zero)
stamp_wb_e0   = -1   (unchanged)
```

Neither `AMDHardware::registerChannel` nor `AMDSWScheduler::addHWChannel` ever
returns false, for any of the two dozen channels they see - and the KIQ
channel's `+0xe0` is still -1. So the simple story ("one of the named gates
refuses") is wrong.

**The 25-vs-23 gap is the lead.** Two channels reached `registerChannel` and
never reached `addHWChannel`, which is exactly what the middle gate,
`this->vtable[0x1c8]`, refusing would produce - it sits between the two.

The 2du log lines themselves were lost to the ring wrap again (channel init
happens early and the 5 s wait evicts ~500 lines), so only the counters
survived. Having both a counter and a line was what saved this boot.

## 2dv (2026-10-12): ask by pointer, not by count

Counting cannot say whether the KIQ is one of the two that fell out. Both
hooks now record the channel **pointers** they saw succeed (a small deduped
array, 48 max), and the `waitForHwStamp` hook - which holds the KIQ channel -
reports membership:

```
kiq_in_regchan   was this exact channel registered
kiq_in_addchan   was this exact channel added to the scheduler
regchan_seen_n   distinct channels that passed registerChannel
addchan_seen_n   distinct channels that passed addHWChannel
```

and names the conclusion:

- **in reg, not in add** - the middle gate `vtable[0x1c8]` is what refuses for
  the KIQ, and that is why `+0xe0` is -1. It then needs its own hook by
  address, being an unnamed local.
- **in neither** - the KIQ never reached `registerChannel` at all, so
  `AMDHWChannel::init` is failing earlier than any of the three gates.
- **in both** - both gates passed for this very channel, so `+0xe0` should have
  been set; either something clears it afterwards, or the object
  `waitForHwStamp` receives is not the one `init` ran on. All three are
  different problems and the log says which.

Diag keys, all in NVRAM so the ring wrap cannot hide them again.

Build: UUID 6369CA9E, hosttest passes. Same boot-args, `-rbgpustampwait` stays.

### 2dv answered (2026-10-12): reg-without-add — and it is BY DESIGN

logs/20261012-220950-plugin, UUID 6369CA9E, armed, no panic:

```
kiq_in_regchan=1  kiq_in_addchan=0  regchan_seen_n=25  addchan_seen_n=23
stamp_wb_e0=-1
```

The KIQ channel is one of the two that passed `registerChannel` and never
reached `addHWChannel`, which 2dv predicted would name `vtable[0x1c8]` as the
gate that refuses. It does name it - and the gate turns out to be a
**compile-time constant**:

```
KIQ vtable[0x140] (dump +0x150) = 0xbec77a4   movb $0x1,%al ; retq      always TRUE
KIQ vtable[0x1c8] (dump +0x1d8) = 0xbec77c6   return !vtable[0x140]()   always FALSE
```

`AMDHWChannel::init` then takes `je 0xbe84b06` with `r14b` already 1, so it
**succeeds** while deliberately skipping `AMDSWScheduler::addHWChannel` and the
`+0xe0` write. Nothing refused anything: for a KIQ channel `+0xe0 == -1` is
Apple's design.

## Correction (2026-10-12): 2ds/2dt read `timestampUpdated` backwards

The whole 2ds -> 2dv line rests on one misread branch. `timestampUpdated`
(0xbe85214):

```
be8521d  cmpq $-0x1,0xe0(%rdi) ; je 0xbe8525a
         ... scheduler path: AMDSWScheduler::timestampUpdated(sched,
             this->0xe0 as an INDEX, this->0xc0, &this->0x84)
be8525a  movq 0xc0(%rbx),%rax      ; the writeback VA
be85261  movl (%rax),%edx          ; READ THE FENCE
be85263  movl %edx,0x84(%rbx)      ; completed = fence value
be85269  tail call this->0x38 vtable[0x230](this->0xc)
```

`+0xe0 == -1` is the **direct** path - read the fence dword straight into
`+0x84` - not "no writeback configured". 2dt's line "`+0xe0` is -1:
timestampUpdated returns WITHOUT reading any writeback" is **withdrawn**; it
reads the fence on every poll. 2du and 2dv were hooking gates for a fault that
does not exist, and 2du's "`+0xe0` is a scheduler index, not an address" was
right about what the field is and wrong about what -1 means.

Seventh instance of the project's recurring failure, and the first where the
misreading was a jump direction rather than an unverified assumption.

### Where the fence actually comes from (`AMDHWChannel::init`, 0xbe848cc)

`init`'s other KIQ constant, `vtable[0x148]` (dump +0x158 = 0xbec77ac,
`xorl %eax,%eax`), is false too, so the KIQ takes the allocated-fence branch:

```
be849b8  callq *0x148(%rax)        KIQ: always false
be849c0  je 0xbe849cb              -> taken
be849c2  (not taken) leaq 0x84(%rbx),%rax     +0xc0 = &this->0x84, no real fence
be849cb  rdi = this->0x20                     the HW interface
be849d2  callq *0x348(%rax) -> +0xac           the fence slot INDEX
be849e7  callq *0x358(%rcx)(idx) -> +0xb0      that slot's CPU VA
be849f4  movq %rax,0xc0(%rbx)                  +0xc0 = +0xb0
be849fb  movl $0x0,0x80(%rbx)
```

So the KIQ polls a genuine hardware fence slot. The GPU is told some MC
address for that slot and writes the completion value there; the CPU reads the
slot's kernel VA at `+0xc0`. 2ds measured `+0x80 = 1`, `+0x84 = 0` and
`+0xc0 = 0xffffffa965fcc040`, so the refresh runs, the dword is read every
poll, and it is not the stamp. Same shape as the SMU bug of 2dh/2di: a correct
CPU pointer paired with an address the hardware cannot reach.

## 2dw (2026-10-12): read the fence, and scan its page

All plain reads. The hook now logs, before and after the real wait:

- `+0xac` (fence slot index), `+0xb0`, and whether `+0xc0 == &this->0x84` -
  which of the two `init` branches this channel took;
- the fence dword at `+0xc0` itself, which is the value `timestampUpdated`
  copies into `+0x84`;
- a scan of the single 4 KB page containing `+0xc0` for the asked-for stamp.
  `timestampUpdated` dereferences that page every poll, so it is mapped.

```
value at +0xc0 == stamp     the GPU wrote the fence and the CPU-side pickup is
                            what fails
stamp elsewhere in the page the fence works and the slot address given to the
                            hardware does not match the one the CPU polls -
                            the 2dh/2di shape
nowhere in the page         the GPU wrote the fence somewhere else entirely,
                            or never wrote it
```

The log says in those words that a single hit on a small integer can be
coincidence and is a lead rather than a result - the lesson of 2dk/2dl/2dm.

Build: UUID 8CBC5B64, hosttest passes. Same boot-args, `-rbgpustampwait` stays.

### 2dw on hardware (2026-10-12): a real fence slot, and the GPU never writes it

logs/20261012-222241-plugin, UUID 8CBC5B64, armed, no panic. 226 diag entries.

```
fence_is_self=0  fence_slot_idx=1  fence_slot_b0=0xffffffc9d0cd9040
fence_val_before=0  fence_val_after=0
fence_c0_off=0x40  fence_scan_hits=13  fence_scan_off=0x1c
```

`fence_is_self=0` confirms the KIQ took the allocated-fence branch at
0xbe849cb: a genuine hardware writeback slot, index 1, VA
0xffffffc9d0cd9040. The dword there is 0 before the wait and 0 after it, so
**the GPU never writes the completion value where the CPU polls**.

**The page scan is noise and is not a finding.** 13 dwords in the page equal
1, so the "value at a different offset" branch the classifier printed cannot
be read as evidence - it is exactly the coincidence the 2dw log line warned
about. The accessors then explained the hit outright (below): `+0x1c` lies
inside frame 0, another channel's 64-byte writeback frame.

### The writeback frame accessors (`AMDHardware`, 0xbea9a94-0xbea9ae5)

```
allocateChannelWriteBackFrame()      return hw->0x20650++            0,1,2,...
getChannelWriteBackFrameCPUPtr(i)    return hw->0x510 + (i << 6)     64-byte frames
getChannelWriteBackFrameGPUAddr(i)   return hw->0x518->vt[0x128]() + (i << 6)
```

and they are the three vtable slots `AMDHWChannel::init` uses - call offsets
0x348 (allocate), 0x350 (GPU addr), 0x358 (CPU ptr), i.e. dump offsets +0x358,
+0x360, +0x368 of `__ZTV32AMDRadeonX6000_AMDNavi21Hardware`.

Slot index 1 at page offset 0x40 is exactly `hw->0x510 + (1 << 6)`, so the CPU
side is confirmed arithmetically, not assumed. `init` only ever takes the CPU
pointer; the GPU is told the matching address through `vtable[0x350]`, whose
value no boot has read.

## 2dx (2026-10-12): read the GPU side of the fence

One value decides this: `hw->0x518->vtable[0x128]()`, the writeback buffer's
GPU base. `getChannelWriteBackFrameGPUAddr` is a `T` symbol in X6000, so it is
resolved by symbol (safe under the milestone-2c Lilu rule) and **called
read-only** - not routed - from the `waitForHwStamp` hook, for frame 0 and for
the KIQ's own slot. The hook also reads `hw->0x510` and `hw->0x20650` and
checks the CPU arithmetic it already verified by hand.

The reference is on the same chip: startKIQ was handed ring MC
`0xf40fb28000`, and 2di read the carve-out base as `0xF4_0000_0000`. So:

```
high word 0xF4    the address is in the carve-out the ring lives in, so it
                  looks right and the fence not landing is something else
high word 0        an FB offset, not an absolute MC address - the GPU is
                  writing outside the page the CPU polls, which is the
                  2dh/2di bug again in the writeback allocator
anything else      a third address space, needing its own reading
```

Diag keys `wb_gpu_frame0`, `wb_gpu_slot`, `wb_cpu_base`, `wb_next_idx`,
`wb_cpu_arith_ok`, `wb_gpu_hi`, `wb_gpu_carveout`, all in NVRAM.

Build: UUID A8DD3601, hosttest passes. Same boot-args, `-rbgpustampwait` stays.

### 2dx on hardware (2026-10-12): the GPU address is RIGHT — and it is a third space

logs/20261012-222823-plugin, UUID A8DD3601, armed, no panic. 233 diag entries.

```
wb_cpu_base=0xffffffa9c64f9000   wb_cpu_arith_ok=1
wb_gpu_frame0=0xffbfdc0000   wb_gpu_slot=0xffbfdc0040   wb_gpu_hi=0xff
wb_next_idx=0x11                 17 frames handed out
```

The 2dx classifier's third branch fired: the high word is `0xFF`, neither the
carve-out `0xF4` nor zero. **But that is not a wrong address space - it is the
right one.** 2do measured `CP_HQD_PQ_BASE = 0xffbfe800`, i.e. ring base
`0xFF_BFE8_0000`, in the same `0xFF_BF..` region and only 0xC0000 away. That
is Apple's GPUVM space; the CP demonstrably fetches the ring from it (2dq), so
the mapping works for reads and the writeback frame sits right beside the ring.

So the FB-offset hypothesis of 2dh/2di **does not apply here** and the
writeback address is structurally correct. `wb_cpu_arith_ok=1` confirms the
CPU side too.

### The fence really is requested — WRITE_DATA, and it uses `+0xc8`

`submitKIQFrame` (0xbe9572e) emits no fence of its own:

```
callq *0x140(ring)        must be true
incl 0x80(%rbx)           submitted++  (the 1 that 2ds measured)
commitBlock(ring, 0x20)   commit the 32-dword block
callq *0x158(ring)        the doorbell
waitForHwStamp(this, +0x80)
```

The fence is inside the block. `submitSetResourcesPacket` (0xbec7404) builds
SET_RESOURCES in the first half (`frame[0] = 0xC006A000`: PM4 type 3, opcode
0xA0, 8 dwords), then at 0xbec7463 calls `vtable[0x328]` with `frame + 0x40` =
`AMDGFX10KIQHWChannel::writeDataCmdPacket` (0xbec7642):

```
p[0] = 0xC0033700      PM4 type 3, opcode 0x37 = WRITE_DATA, 5 dwords
p[1] = 0x00100500      DST_SEL 5 (memory), WR_CONFIRM
p[2] = this->0xc8 & ~3 destination LOW
p[3] = this->0xcc      destination HIGH
p[4] = vtable[0x1b0]() data = getCommandSubmitTimestamp()
```

**The destination is `+0xc8`, not `+0xc0`**, and the two are assigned in
different functions for the same frame index:

```
init                    0xbe849f4  +0xc0 = vtable[0x358](idx)    CPU pointer
allocateMemoryResources 0xbe84f05  +0xc8 = vtable[0x350](+0xac)  GPU address
```

`allocateMemoryResources` is the function 2cb found failing engine by engine
and 2cc had to stub for VCN encode, so one of the pair can be set while the
other is not.

## 2dy (2026-10-12): read the fence packet the driver actually built

`writeDataCmdPacket` is hooked observe-only: the original runs first and the
five dwords are read back out of the ring block, so this is the packet the GPU
was handed, not a reconstruction. The `waitForHwStamp` hook also reads
`+0xc8` and compares it with `getChannelWriteBackFrameGPUAddr(slot)`.

```
dst == 0                     the GPU is told to write the fence to address 0,
                             so it can never reach the polled page -
                             allocateMemoryResources never set +0xc8
dst == wb_gpu_slot           the packet targets the right frame and the fence
                             failing is further down
dst in 0xFF_BF.., different  the packet targets another frame than the CPU polls
```

Diag keys `fencepkt_calls`, `fencepkt_hdr`, `fencepkt_ctl`, `fencepkt_dst`,
`fencepkt_data`, `chan_c8`, `chan_c8_matches_wb`, all in NVRAM.

Build: UUID 6DE72A13, hosttest passes. Same boot-args, `-rbgpustampwait` stays.

### 2dy on hardware (2026-10-12): the fence packet is CORRECT in every field

logs/20261012-223450-plugin, UUID 6DE72A13, armed, no panic. 240 diag entries.

```
fencepkt_hdr=0xc0033700   WRITE_DATA, 5 dwords
fencepkt_ctl=0x00100500   DST_SEL 5 (memory), WR_CONFIRM
fencepkt_dst=0xffbfdc0040 == wb_gpu_slot == chan_c8
fencepkt_data=0x1         == the asked-for stamp
chan_c8=0xffbfdc0040  chan_c8_matches_wb=1
fence_val_before=0  fence_val_after=0  wb_cpu_base=0xffffffe0de560000
```

So `allocateMemoryResources` **did** set `+0xc8`, and it equals
`getChannelWriteBackFrameGPUAddr(1)` exactly. The GPU is told to write the
value 1 to the very frame whose CPU VA (`0xffffffe0de560040`) the driver
polls, with write-confirm on. The CP consumed the block (2dq) and the dword
never changes. Nothing in the submission is wrong.

(The `2dy` log line was evicted by the ring wrap; the diag keys carried it,
which is why they exist.)

### The frame layout, and why it is not the suspect

`getKIQFrame` (0xbe957a0) fills all 32 dwords from `vtable[0x1f0]`, which for
the KIQ is the constant `0xFFFF1000` (0xbec77f8) - PM4 type 3, opcode 0x10
(NOP), count field 0x3FFF, AMD's standard ring-padding dword. SET_RESOURCES
then overwrites dwords 0-7 and `writeDataCmdPacket` dwords 16-20, leaving the
NOP filler between and after them.

RPTR advancing to 0x20 = 32 dwords = the whole committed block is consistent
with the CP walking all of it. **This is Apple's shipping KIQ bring-up and it
works on Navi 21**, so the layout, the padding and the writeback mechanism are
sound in principle; inventing a PM4 parse-drift theory for a path that
demonstrably works elsewhere would be the eighth instance of the project's
recurring error, not a finding.

## 2dz (2026-10-12): did the fence write fault in GPUVM?

What IS chip-specific: the destination `0xFFBFDC0040` is a GPUVM **virtual**
address translated through the queue's VMID. The CP fetches the ring out of
the same `0xFF_BF..` region, but fetch and write need not be mapped alike, and
a write that misses the page tables leaves RPTR advanced with the value never
stored - exactly what is measured. All reads, all BASE_IDX 0 (seg0 = 0x1260,
proven in 2dl), from docs/linux-ref/gc_10_3_0_offset.h:

```
GCVM_L2_PROTECTION_FAULT_CNTL     0x15c4
GCVM_L2_PROTECTION_FAULT_STATUS   0x15c8
GCVM_L2_PROTECTION_FAULT_ADDR_LO32/HI32  0x15c9 / 0x15ca
CP_CPC_STATUS 0x0e24   CP_CPF_STATUS 0x0e27
CPC_UTCL1_STATUS 0x1f55   CPF_UTCL1_STATUS 0x1f56   GRBM_STATUS2 0x0da2
```

The UTCL1 status registers are the CP's own address-translation units, so they
report a translation failure even if L2 fault reporting is off.

**What would have to be true for the reading to mean anything**, stated in the
code and in the log: a zero STATUS only means "no fault" if fault reporting is
switched on. `FAULT_CNTL == 0` means no fault class is reported at all, and the
log then says in those words that this does **not** mean there was no fault.
When a fault IS latched, the address register's units are not independently
verified, so both the raw value and the 4 KB-page reading are printed and
compared against the fence destination, with neither asserted.

Diag keys `vmf_cntl`, `vmf_status`, `vmf_addr_lo`, `vmf_addr_hi`,
`cp_cpc_status`, `cp_cpf_status`, `cpc_utcl1_status`, `cpf_utcl1_status`,
`diag_grbm_status2`, in NVRAM. Read at every dump point, including
`post-stamp-wait` after the full 5 s wait.

Build: UUID 0EEDEBBB, hosttest passes. Same boot-args, `-rbgpustampwait` stays.

### 2dz on hardware (2026-10-12): a GPUVM fault IS latched — at the RING, not the fence

logs/20261012-224335-plugin, UUID 0EEDEBBB, armed, no panic. 249 diag entries.

```
vmf_cntl=0x00001ffc   reporting IS on, so the latch is meaningful
vmf_status=0x00000881  MORE_FAULTS=1 WALKER_ERROR=0 PERMISSION_FAULTS=0x8
                       MAPPING_ERROR=0 RW=0 (a READ)
vmf_addr_hi=0  vmf_addr_lo=0x0ffbfe80
CP_CPC_STATUS=0xa0000000  CP_CPF_STATUS=0xa8008001
CPC_UTCL1=0x00000000      CPF_UTCL1=0x000b0002
GRBM_STATUS2=0x34108008
```

The `FAULT_CNTL != 0` precondition 2dz insisted on is satisfied, so this is a
real latched fault and not an unreported-silence artefact.

**The address identifies the ring, and 2dz's own classifier missed it** because
it compared only against the fence destination:

```
fault page 0x0FFBFE80 << 12        = 0xFFBFE80000
CP_HQD_PQ_BASE 0xffbfe800 << 8     = 0xFFBFE80000   <- exact 40-bit match
fence dest                          = 0xFFBFDC0040  (0xC0000 lower, a different page)
```

Two independently-read registers agreeing to 40 bits under their own standard
conventions is not the small-integer coincidence that 2dw's page scan was.
`RW=0` makes it a **read**; `MAPPING_ERROR=0` with `PERMISSION_FAULTS=0x8` says
the mapping exists and the access was refused. `CPF_UTCL1_STATUS` nonzero while
`CPC_UTCL1_STATUS` is zero points at the **fetcher's** translation unit, not the
compute side - a second, independent signal for the same place.

**This puts 2dq's conclusion in question, not its measurement.** WPTR
advancing, RPTR catching up and CP_STAT going busy are all still true; but a
ring fetch that faults can advance RPTR having discarded the data, so "the CP
executed the packet" may have been "the CP fetched, faulted and discarded".
2dq is not withdrawn - its reading stands and its interpretation is now open.

## 2ea (2026-10-12): date the latch, and compare against both candidates

The fault registers **latch and stay until cleared**, and `MORE_FAULTS=1` says
more than one occurred. A fault read at `post-stamp-wait` could therefore have
been raised at any earlier moment, including before `startKIQ` existed, by
something that is not ours. Until that is excluded this is a lead, not a
finding - the same discipline that caught 2dk through 2dv.

So the status and address are now recorded **per dump point** in their own diag
keys (`vmf_st_gc`, `vmf_st_kiq`, `vmf_st_pre`, `vmf_st_wait`, `vmf_st_post` and
the matching `vmf_ad_*`), which the ring wrap cannot hide and which the single
last-write `vmf_status` key could never express.

```
latched at post-gc-hw-init    pre-existing, raised before any KIQ work - ours
                              is not the cause and the trail restarts
first latched at at-stamp-wait the KIQ's own fetch is what GPUVM refused
```

The comparison now names which address it matched - the ring base
(`CP_HQD_PQ_BASE << 8`, captured from the HQD probe), the fence page, or
neither - instead of only testing the fence.

Build: UUID 749D33AD, hosttest passes. Same boot-args, `-rbgpustampwait` stays.

### 2ea on hardware (2026-10-12): the fault is OURS — raised by the submission

logs/20261012-225909-plugin, UUID 749D33AD, armed, no panic. 259 diag entries.

```
point   vmf_st_*     vmf_ad_*      -> byte
gc      0x00000000   0             -            before startKIQ
kiq     0x00000000   0             -
pre     0x00000000   0             -
wait    0x000009ba   0x0ffbfe80    0xffbfe80000  <- appears HERE
post    0x000009ba   0x0ffbfe80    0xffbfe80000
```

STATUS is **zero at all three points before the doorbell** and nonzero from
`at-stamp-wait` onwards, at the ring base again. So the fault is raised by the
KIQ's own submission and does **not** predate `startKIQ`: the pre-existing-fault
escape 2ea was written to test is closed, and the ring-fault lead is real.

**But the 2ea log line printed "nothing latched at this point" while showing a
nonzero status** - a bug in the classifier, not in the chip. It tested
`vm_status & 1`, assuming bit 0 was a validity bit. Bit 0 is **MORE_FAULTS**
("further faults beyond the first"). 2dz's `0x881` printed the fault banner
only because MORE_FAULTS happened to be set that boot; this boot's genuine
fault has MORE_FAULTS=0 and the code called it nothing. **A nonzero STATUS is
the latch.** Ninth instance of this project's recurring error - and the first
inside the code written to prevent it. Fixed in 2eb (`vm_status != 0`).

Decoded properly, both boots at the same address, same client, same direction:

| field | 0x881 (2dz) | 0x9ba (2ea) |
|---|---|---|
| MORE_FAULTS | 1 | 0 |
| WALKER_ERROR | 0 | **5** |
| PERMISSION_FAULTS | 0x8 | 0xB |
| MAPPING_ERROR | 0 | **1** |
| CID | 4 | 4 |
| RW | 0 (read) | 0 (read) |

`MAPPING_ERROR=1` with `WALKER_ERROR=5` says the page walk found **no valid
mapping** for the ring address. `CPF_UTCL1_STATUS` is nonzero both boots
(0xb0002, 0xb01) while `CPC_UTCL1_STATUS` is 0: the fetcher's translation unit.

So the chain is coherent end to end: the MQD points the CP at GPUVM
`0xFF_BFE8_0000`, that address does not translate, the fetch faults, RPTR
advances having discarded the data, the WRITE_DATA inside the frame is never
executed, the fence never lands, `waitForHwStamp` fails, `powerUpHW` fails.

## 2eb (2026-10-12): is the ring address inside the VMID's mapped range?

This picks up the thread 2do left hanging. `CP_HQD_PQ_BASE << 8` =
`0xFF_BFE8_0000` is not the ring MC address `startKIQ` was handed
(`0xf40fb28000`), and 2do correctly said there was no reason those should
match - a compute HQD holds a GPUVM VA. What is new is that the GPUVM VA
**does not translate to anything**, so the question is whether the page tables
are meant to cover it. The queue runs at VMID 0 (measured in 2do), so
`GCVM_CONTEXT0_*` is its context:

```
GCVM_L2_CNTL                           0x15bc
GCVM_CONTEXT0_CNTL                     0x15fc
GCVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_LO32/HI32   0x1667 / 0x1668
GCVM_CONTEXT0_PAGE_TABLE_START_ADDR_LO32/HI32  0x1687 / 0x1688
GCVM_CONTEXT0_PAGE_TABLE_END_ADDR_LO32/HI32    0x16a7 / 0x16a8
```

All reads, BASE_IDX 0. START/END are page numbers in amdgpu's usage, so both
readings are printed and the range test accepts either - neither is asserted,
the same rule the fault address gets.

```
ring VA outside [START, END]   the MQD was given a GPUVM address this VMID's
                               page tables do not cover at all
ring VA inside                 the aperture is right and the fault is in the
                               page-table contents, not the range
```

Diag keys `vm_l2_cntl`, `vm_c0_cntl`, `vm_c0_ptb`, `vm_c0_start`, `vm_c0_end`,
`vm_ring_in_range`, in NVRAM.

Build: UUID 3857364D, hosttest passes. Same boot-args, `-rbgpustampwait` stays.

## 2eb/2ed answered (2026-10-12): the range is right, the GC hub is right

logs/20261012-232037-plugin, armed, no panic, 274 diag entries, `d=0`.

```
2eb: GCVM_L2_CNTL=0x000c0603 CONTEXT0_CNTL=0x01555481 PT_BASE=0xfdfa001
     START=0xffbfa00 END=0xffffe00 (pages -> 0xffbfa00000..0xffffe00000)
     ring VA = 0xffbfe80000 -> INSIDE the mapped range
2ed: GCMC FB_LOCATION base=0x0000f400 top=0x0000f4ff FB_OFFSET=0x00000780
     AGP bot=0x00ffffff top=0x00000000 SYS lo=0x003d0000 hi=0x003d3fff
     all_zero=0 fb_agrees_with_mmhub=1
```

The ~1 GB CONTEXT0 window matches the 0x40000000 the TTL input reports at
+0x68. The GC hub's system aperture is amdgpu's own formula applied to the
real carve-out — `min(fb,agp) >> 18` = `0xF400000000 >> 18` = 0x3d0000 and
`0xF4FFFFFFFF >> 18` = 0x3d3fff — and AGP bot 0xffffff / top 0 is amdgpu's
"AGP disabled" encoding. Two independently-derived numbers agreeing to the
bit, so both the hub setup and the register decode are sound.

**So the aperture is right and the fault is in the page-table contents.**

### The vram_base_offset reading, and why it did NOT survive

`GCVM_CONTEXT0_PAGE_TABLE_BASE_ADDR` = `0x0FDFA001`: high word 0, bit 0
(AMDGPU_PTE_VALID) set, bit 1 (AMDGPU_PTE_SYSTEM) clear — a page directory
declared to be in VRAM. Linux says what belongs there (docs/linux-ref/
UPSTREAM.md, fetched 2026-10-12):

```
gfxhub_v2_1_init_gart_aperture_regs -> setup_vm_pt_regs(amdgpu_gmc_pd_addr(gart.bo))
amdgpu_gmc_pd_addr -> gmc_v10_0_get_vm_pde: VRAM -> amdgpu_gmc_vram_mc2pa(addr)
amdgpu_gmc_vram_mc2pa(mc) = mc - vram_start + vram_base_offset
vram_base_offset = gfxhub_v2_1_get_mc_fb_offset() = GCMC_VM_FB_OFFSET << 24
```

We read `GCMC_VM_FB_OFFSET = 0x780`, so `vram_base_offset` should be
`0x7_8000_0000`. Apple's value is exactly that computation with
`vram_base_offset = 0` — correct for a Navi 21 dGPU, short by 0x7_8000_0000
here. The same shape as the SMU address bug of 2dh/2di.

**The register was deliberately NOT patched on that reading.** If Apple's VM
layer uses offset 0 throughout, the PTEs are low by the same amount and
making the walk succeed would aim GPU reads *and writes* at host DRAM near
0x0FDF_A000 — worse than the current clean fault. 2ee read both candidate
pages instead.

### 2ee on hardware (2026-10-12): NEITHER page is a page directory

logs/20261012-233404-plugin, armed, no panic, 286 diag entries. The 2ee log
lines were evicted by the ring wrap; the diag keys carried the result, which
is why they exist.

```
pdbA phys=0x0fdfa000   nonzero=512/512 valid=251 low=23 high=228
                       [0]=0x4f8d48fe8949f389
pdbB phys=0x78fdfa000  nonzero=0 valid=0
```

`0x...8949f389` little-endian is `89 f3 49 89 fe 48 8d 4f` =
`mov %esi,%ebx ; mov %rdi,%r14 ; lea ...` — **x86-64 machine code**. Candidate
A is live macOS kernel memory, and 251/512 "valid" bits is a coin flip on
noise, not PDEs. Candidate B is 512 zero qwords.

So the `vram_base_offset` reading is **not supported**, and the page
directory's location is unknown. Worth stating plainly: the GPU's page-table
walker is currently pointed at live kernel memory, and the MAPPING_ERROR
fault is what has been protecting the machine.

## 2ef/2eg (2026-10-12): prove the instrument, then ask the driver

Two additions, both read-only.

**2ef, the control.** pdbB's zeros only mean "no directory here" if reading
carve-out physical memory works at all, and nothing has ever tested that —
every carve-out read in this project went through BAR0 in the probe kext, not
through `IOMemoryDescriptor` on the system-physical alias. Probe 2 supplies
the control: the carve-out is at system-physical `0x7_8000_0000` and HUBP0
scans the GOP framebuffer from carve-out offset 0, which is the console this
machine is displaying and so cannot be a page of zeros.

```
framebuffer page nonzero   carve-out reads work; pdbB's zeros are real
framebuffer page zero      the instrument does not read carve-out memory and
                           the whole 2ee result proves nothing
```

**2eg, the direct question.** Guessing transformations of a register is how
2ee went wrong; `AMDHWVMM::allocVMBlock(level, GLKMemoryElement *)`
(0xbe90eea) is where every page-table block actually comes from:

```
be90efb  movq 0x78(%rdi),%rdi          the allocator
be90f11  movl 0xac0(%r15,level*32),%r14d
be90f30  callq <allocator>(elem, r14, 0x1000, 1, 1)
be90f38  cmpq $0x0,0x20(%rbx)          the result lands at elem+0x20
```

Hooked observe-only: the original runs first and the element is read back, so
the log carries the block's real addresses with `level` saying which tier it
is. The root directory is the one the hub register should be pointing at, and
comparing it with `0x0FDFA001` says what transformation Apple applied.

Build: UUID 9C57527B, hosttest passes. Same boot-args, still armed.

### 2ef/2eg on hardware (2026-10-12): the control FAILED — 2ee is withdrawn

logs/20261012-234107-plugin, armed, no panic, 292 diag entries.

```
ctl_fb_phys=0x780000000  ctl_fb_mapped=1  ctl_fb_nonzero=0  ctl_fb_e0=0
pdbA_mapped=1  pdbA_nonzero=0        (was 512/512 nonzero the previous boot)
pdbB_mapped=1  pdbB_nonzero=0
vmm_alloc_calls=ABSENT
```

**The GOP framebuffer page reads as 512 zero qwords** while the console is
displaying on it, and in ARGB8888 even a black pixel is 0xFF000000, not zero.
So `IOMemoryDescriptor::withPhysicalAddress` + `createMappingInTask` **maps
carve-out memory without reading it** — `mapped=1` and every qword zero.

Therefore **2ee's "candidate B holds no page directory" is withdrawn**: B's
zeros were the instrument, not the chip. The `vram_base_offset` reading is
back to *unresolved*, neither supported nor refuted. (pdbA reading real kernel
code one boot and zeros the next is consistent with it being ordinary RAM
whose contents differ per boot; it is not evidence either way.)

This is the control doing exactly its job, and it is worth noting that the
conclusion it killed was mine and was already written into CLAUDE.md as a
finding. Reading physical memory is not a usable instrument in this plugin.

**2eg never fired.** `AMDHWVMM::allocVMBlock` was hooked and never called,
which on reflection is expected: `AMDHWVMM::createVMContext(AMDAccelTask *)`
builds **per-task** VM contexts, and an armed boot has no GPU clients at all.
AMDHWVMM is not what builds the GART page table for VMID 0.

## 2eh (2026-10-12): ask the TTL layer for the GART table's address

HWLibs names the function outright:

```
__ZN14AmdTtlServices19getGartTableAddressEPPvPy   0x126617f0
  12661800  movq %rdi,%rsi              rsi = this
  1266183f  movq 0x38(%rsi),%rdi        the TTL handle
  12661848  leaq -0x40(%rbp),%rsi       a 5-qword out block
  1266184c  callq _ttl_get_gart_primary_page_table_info   0x1262f80f
```

`_ttl_get_gart_primary_page_table_info` is a local text symbol defined in
HWLibs, so resolving it by HWLibs' id is safe under the milestone-2c Lilu
rule. It is **resolved only, never routed**: the plugin calls it read-only
with the TTL handle the `initialize()` hook already captures
(`this->0x38` — the same value boot 13 saw passed to `_ttl_initialize`),
rather than hoping Apple calls it. It reports an address; it programs nothing.

What the five returned qwords decide, stated before they are read. The hub
register `GCVM_CONTEXT0_PAGE_TABLE_BASE_ADDR` reads 0x0FDFA001, i.e. page
0x0FDFA000 plus the VALID bit:

```
a qword == 0x0FDFA000      the register holds exactly what TTL reports, and
                           the question becomes what that number means
a qword == 0xF4_0FDF_A000  TTL has the carve-out MC address and the register
                           was written without the MC base
a qword == 0x7_8FDF_A000   TTL has the system-physical address and the
                           register was written without vram_base_offset -
                           the 2dh/2di shape a third time
none of these              TTL and the hub disagree about which table is in
                           use, and neither reading stands
```

Diag keys `gart_info_resolved`, `gart_info_st`, `gart_info_0..4`,
`gart_match_idx`, all in the NVRAM hot list. The physical-page scan and its
control stay in, now that the control is known to report honestly.

Build: UUID 917A6DC7, hosttest passes. Same boot-args, still armed.

### 2eh on hardware (2026-10-12): asked too early — the TTL handle was still 0

logs/20261012-235221-plugin, armed, no panic, 299 diag entries.

```
gart_info_resolved=1    the symbol resolved
gart_info_st=0xffffffff the plugin's own "handle was NULL" sentinel
gart_info_0..4 = 0
```

So nothing was asked of TTL at all. `g_rb_ttl_this` was captured, but
`*(this + 0x38)` read 0: the one shot was spent at `post-gc-hw-init`, which
runs **inside** `_ttl_initialize`, and `_ttl_initialize(params, &this->0x38)`
only stores the handle into that slot when it finishes. The value cannot exist
yet at the point it was read.

Tenth instance of the recurring error and a repeat of 2dp exactly: the
measurement was taken before the thing it measures exists. The sentinel is
what kept it from being read as "TTL has no GART table".

## 2ei (2026-10-12): retry until the handle exists

One-line shape change: `g_rb_2eh_done` is set **only when the call actually
runs**, so a dump point that finds `this->0x38` still 0 logs that it is
retrying and leaves the question open instead of burning the shot. The later
dump points — `post-startKIQ`, `pre-kiq-submit`, `at-stamp-wait`,
`post-stamp-wait` — all run well after TTL init completes.

`gart_info_early` counts how many points were too early, so the next boot says
whether the retry was needed rather than leaving it to inference.

Build: UUID 3DA57E40, hosttest passes. Same boot-args, still armed.

### 2ei answered (2026-10-13): TTL names the page table, in three address spaces

logs/20261012-235843-plugin, armed, no panic, 301 diag entries.

```
gart_info_early=1       one dump point WAS too early - the 2ei retry was needed
gart_info_st=0          the call succeeded
gart_match_idx=2
gart_info_0 = 0x100201000
gart_info_1 = 0x100000000008
gart_info_2 = 0xf40fdfa000          <- carve-out MC
gart_info_3 = 0x88fdfa000           <- BAR0 aperture
gart_info_4 = 0xffffffe4e017c000    <- kernel VA
```

Three independently-derived encodings of the **same page**, and every one
carries the offset `0x0FDF_A000`, which is exactly what the hub register
holds:

| value | base (probe 2) | offset |
|---|---|---|
| 0xF4_0FDF_A000 | carve-out 0xF4_0000_0000 | 0x0FDF_A000 |
| 0x8_8FDF_A000 | BAR0 0x8_8000_0000 | 0x0FDF_A000 |
| `GCVM_CONTEXT0_PAGE_TABLE_BASE_ADDR` = 0x0FDFA001 | — | 0x0FDF_A000 |

So the GART page directory is at carve-out MC `0xF4_0FDF_A000`, and the hub
register holds that page **with the base stripped** — `amdgpu_gmc_vram_mc2pa`
computed with `vram_base_offset = 0`. That is correct for a Navi 21 dGPU,
where `GCMC_VM_FB_OFFSET` is 0 and the register legitimately ends up holding
an FB-relative offset, and short by `0x7_8000_0000` on this APU, where
FB_OFFSET reads 0x780. **The vram_base_offset reading is now supported by
measurement rather than inferred**, and three numbers agreeing on one offset
is not the small-integer coincidence 2dw's page scan was.

`gart_info_4` also solves the instrument problem 2ef exposed: it is a kernel
VA for the page-table page, so its contents can be read through a pointer TTL
itself supplied, with no `IOMemoryDescriptor` in the way.

## 2ej (2026-10-13): read the directory's own entries before touching PT_BASE

The register is now a known-wrong value with a known-right replacement, and
patching it is still **not** safe on its own. If Apple's VM layer uses
`vram_base_offset = 0` throughout, the PDEs carry the same error, and making
the walk succeed would aim GPU reads *and writes* at host DRAM near
`0x0FDF_A000` — strictly worse than the current clean MAPPING_ERROR fault,
which is what has been protecting the machine.

So read the directory at `gart_info_4` and classify where its entries point.
A GFX10 PDE is 8 bytes, bit 0 = valid, bit 1 = `AMDGPU_PTE_SYSTEM` (a bus
address, counted separately rather than lumped in with the VRAM entries):

```
entries at 0x7_8xxx_xxxx    only PT_BASE is wrong; the table already uses
                            system-physical addresses and patching the
                            register is the whole fix
entries LOW (< 0x1000_0000) the PTEs carry the same zero offset; the bug is
                            systemic and patching PT_BASE alone is unsafe
entries at 0xF4_xxxx_xxxx   the table holds raw MC addresses, a third
                            convention again
valid = 0                   the directory was never populated and none of the
                            above applies
```

Reads only — a dereference of a kernel pointer TTL just returned. Diag keys
`pde_valid`, `pde_sys`, `pde_low`, `pde_sysphys`, `pde_carveout`, `pde_other`,
`pde_e0`, `pde_e1`, `pde_no_va`, all in the NVRAM hot list.

Build: UUID 57A1E1A6, hosttest passes. Same boot-args, still armed.

### 2ej on hardware (2026-10-13): read the WRONG PART of the table

logs/20261013-000530-plugin, armed, no panic, 310 diag entries.

```
pde_no_va=1   pde_valid=0   pde_sys=0   pde_low=0   pde_sysphys=0
gart_info_2=0xf40fdfa000   gart_info_4=0xffffffdbcc659000
```

`pde_no_va=1` is stale: the early retry pass runs the block with `out[]` all
zero, sets the sentinel, and nothing clears it on the successful pass.
`pde_valid=0` really was written by the live read, so the scan did run.

**But the scan was aimed at the wrong offset, and the reading is void.** Two
things 2ej got wrong:

- `GCVM_CONTEXT0_CNTL = 0x01555481` has **PAGE_TABLE_DEPTH (bits 2:1) = 0**,
  so what PT_BASE points at is a **flat PTE array**, not a page directory —
  one 8-byte entry per 4 KB page of the aperture.
- The aperture starts at `START << 12` = `0xFF_BFA0_0000`, and the ring sits
  at `0xFF_BFE8_0000`, so the ring's entry is at index
  `(ring - start) >> 12` = **1152**, byte offset **0x2400**. The 512 entries
  2ej read cover `0xFF_BFA0_0000..0xFF_BFC0_0000` and do **not** include it.

So "no valid entries" is a statement about an empty low aperture — entirely
unremarkable — and says nothing about the ring's own mapping. Eleventh
instance of the recurring error, caught by checking the arithmetic before
writing it up rather than after.

## 2ek (2026-10-13): read the ring's own PTE

Index 1152, byte offset 0x2400, and the read is bounded by **two
independently-derived sizes** rather than one guess: `out[0]`'s low 32 bits
(`0x201000`) and `(END - START) * 8` from the aperture registers
(`0x202000`). The entry is read only when it fits inside the smaller.

```
entry 0 / not valid     the ring page is genuinely unmapped: the fault is the
                        page table's CONTENTS, and fixing PT_BASE alone would
                        not help
valid, addr 0x7_8xx     the table is right and ONLY PT_BASE is wrong -
                        patching the register is the whole fix
valid, addr LOW         the PTEs carry the same zero vram_base_offset; the bug
                        is systemic and patching PT_BASE alone is unsafe
needs more than limit   not read, and the log says it says nothing
```

The whole-table count is kept too, now bounded by the same limit and labelled
as a flat PTE table, and `pde_no_va` is cleared on the successful pass so the
sentinel cannot go stale again.

Diag keys `ring_pte`, `ring_pte_valid`, `ring_pte_addr`, `ring_pte_idx`,
`ring_pte_oob`, `pte_entries`, plus the existing `pde_*` counts.

Build: UUID 1CD15946, hosttest passes. Same boot-args, still armed.

### 2ek on hardware (2026-10-13): the table IS populated — and all of it is SYSTEM

logs/20261013-002405-plugin, armed, no panic, 309 diag entries.

```
pte_entries=0x40200 (262656 scanned)   pde_valid=0x5e8 (1512)
pde_sys=0x5e8 (1512)                   pde_low=0  pde_sysphys=0
pde_carveout=0  pde_other=0            pde_no_va=0
ring_pte_idx=ABSENT
```

**The flat PTE table is real and populated**: 1512 valid entries out of
262656, and **every one of them has `AMDGPU_PTE_SYSTEM` set**. They are bus
addresses of system pages, which is exactly what a GART does. 1512 pages is
~6 MB mapped into a 1 GB aperture — a normal sparse GART. 2ej's "no valid
entries" was reading the empty first 2 MB, as 2ek predicted.

So `pde_low / pde_sysphys / pde_carveout` are all 0 only because SYSTEM
entries are counted separately and `continue` before the address
classification — not because nothing points anywhere.

**But the ring's own PTE was still not read.** `ring_pte_idx` is set
unconditionally inside `if (ring_va >= start_va)`, so `ring_va` was 0: the
2eh/2ek block runs **earlier in the dump function than
`rbgpu_probe_hqd_slot`**, which is what sets `g_rb_2ea_pq_base`. At the first
dump point where the TTL handle exists (`post-startKIQ`), that global still
held the previous point's value, and `post-gc-hw-init` had measured
`PQ_BASE = 0`. The guard skipped the read without a word.

Twelfth instance of measuring before the thing it needs exists, and the third
of exactly this ordering shape — 2dp (before the doorbell), 2eh (before the
TTL handle), now this (before the HQD probe).

## 2el (2026-10-13): read the ring PTE after the HQD probe

The 2eh block now only **caches** the table's kernel VA and the agreed size
limit; the ring-PTE read moved to the end of the dump function, after
`rbgpu_probe_hqd_slot` has run, with **its own latch** set only when the read
actually happens — so a dump point that is still too early retries at the
next one instead of burning the shot. Same correction shape as 2ei.

```
not valid        the ring page is genuinely unmapped: the fault is the table's
                 CONTENTS and PT_BASE alone would not help
valid            the table is fine and only PT_BASE is wrong
```

A `SYSTEM` bit on the ring's entry is **expected**, not a fault — every valid
entry in this table has it — so `ring_pte_sys` is recorded and the log says so
rather than leaving it to be misread as the thirteenth instance.

Diag keys `ring_pte`, `ring_pte_valid`, `ring_pte_sys`, `ring_pte_addr`,
`ring_pte_idx`, `ring_pte_oob`.

Build: UUID 4E2BAA6F, hosttest passes. Same boot-args, still armed.

### 2el answered (2026-10-13): the ring's PTE is VALID — the table is not the fault

logs/20261013-003209-plugin, armed, no panic, 314 diag entries.

```
ring_pte_idx   = 0x480 (1152)      exactly the computed index
ring_pte       = 0x30006badce077
ring_pte_valid = 1
ring_pte_sys   = 1
ring_pte_addr  = 0x6badce000
```

Low bits `0x77` = VALID | SYSTEM | SNOOPED | EXECUTE | READ | WRITE, pointing
at bus address `0x6_badce000` — real host DRAM about 27 GB in, plausible on a
30-34 GiB machine. **The ring page is mapped and the table's contents are
right.** Of the four outcomes 2ek named, this is "valid: the table is fine and
only PT_BASE is wrong".

## 2em (2026-10-13): point the hub register at where the table really is

The first **behavioural** write to a GC register this project has made.
`GRBM_GFX_CNTL` (2do) is an index register, saved and restored, that changes
nothing by itself; this one changes how every GPUVM translation in VMID 0
resolves and is left in place. It is off unless **`-rbgpuptbase`** is in
boot-args.

What is measured rather than assumed, and re-checked every boot before the
write is allowed:

- TTL reports the flat PTE table at carve-out MC `0xF4_0FDF_A000`, and
  `GCVM_CONTEXT0_PAGE_TABLE_BASE_ADDR` holds exactly that page with the
  carve-out base stripped (2ei: three address spaces, one offset
  `0x0FDF_A000`). The code recomputes that relationship and **refuses** if it
  does not hold.
- Linux wants `mc2pa(mc) = mc - vram_start + vram_base_offset`, with
  `vram_base_offset = GCMC_VM_FB_OFFSET << 24` (UPSTREAM.md). FB_OFFSET reads
  0x780, so the register is short by `0x7_8000_0000` — correct for a Navi 21
  dGPU where FB_OFFSET is 0.
- The ring's own PTE is VALID (2el), so the contents are not the fault.
- **Every** valid entry in the table is `AMDGPU_PTE_SYSTEM` (2ek: 1512 of
  1512). A SYSTEM entry holds a bus address and never goes through `mc2pa`,
  so no PTE here can carry the same zero-offset error. **That is what makes
  the patch safe**: the danger that blocked it through 2ej-2el — PTEs low by
  `0x7_8000_0000`, aiming GPU writes at the wrong host DRAM — cannot occur in
  a table with no VRAM entries.

```
PT_BASE 0x0FDFA001 -> 0x78FDFA001   (LO32 0x8fdfa001, HI32 0x7)
```

Residual risk, stated plainly: if the walker wants the BAR0 alias
(`0x8_8FDF_A000`, which TTL also reported) rather than the DRAM alias
(`0x7_8FDF_A000`), the write points it at another wrong page and the same
MAPPING_ERROR comes back — no worse than now. No TLB invalidate is issued, so
a stale negative translation is a second candidate if the fault persists.

The test is clean because `vmf_st_*` is recorded per dump point and is 0 at
every point before the doorbell on an unpatched boot:

```
vmf_st_wait = 0 and stamp_ret = 1   the fault is gone and the fence arrived -
                                    this was the whole fix
vmf_st_wait nonzero at the ring     the walker wants a different alias, or the
                                    TLB is stale
ptb_rel_holds = 0                   the measured relationship did not hold and
                                    nothing was written
```

Diag keys `ptb_written`, `ptb_rel_holds`, `ptb_cur`, `ptb_want`, `ptb_back`,
with `vmf_st_wait`, `vmf_st_post` and `stamp_ret` promoted into the hot list.

Build: UUID EF933186, hosttest passes. **Needs one new boot-arg**, and it is
the user's call whether to arm it.

### 2em answered on hardware (2026-10-13): THE FAULT IS GONE AND THE STAMP ARRIVED

The armed boot wedged and `tools/plugin_check.sh` never returned, so the
IORegistry log died with it. NVRAM carried the result out, which is exactly
what 2cu exists for:

```
rbgpu-diag-prev: d=107 armed=1
  ptb_written=1 ptb_rel_holds=1
  ptb_cur=fdfa001 ptb_want=78fdfa001 ptb_back=78fdfa001
  vmf_st_wait=0 vmf_st_post=0 stamp_ret=1
  ring_pte=30006b631d077 ring_pte_valid=1 ring_pte_sys=1 ring_pte_idx=480
```

That is 2em's own success branch, written before the reading was taken:
**`vmf_st_wait = 0` and `stamp_ret = 1`.** The register took the value
(`ptb_back == ptb_want`), the GPUVM `MAPPING_ERROR` that every boot since 2dz
latched at the ring base is **gone**, and `waitForHwStamp` returned TRUE - the
GPU executed the SET_RESOURCES packet and wrote the completion fence to the
writeback slot the CPU polls.

So the chain 2dz-2el reconstructed was right end to end, and the whole fault
was one register holding `amdgpu_gmc_vram_mc2pa` computed with
`vram_base_offset = 0`: correct for the Navi 21 dGPU Apple's driver was
written for, short by `0x7_8000_0000` on this APU. `doStart` continued past
`submitSetResourcesPacket` for the first time in this project.

**Then the boot wedged**, and it is worth being precise that it wedged AFTER
succeeding, not instead of succeeding. Symptoms, all one cause: `ioreg`
(plugin_check) never returned because it blocks on the IOService lock the
wedged `start()` thread holds; Ctrl-C could not help because the process was
in a kernel wait; `sudo reboot` could not finish for the same reason. No
panic. The console photograph showed `post-startKIQ` as its last line while
NVRAM proved the boot had reached `at-stamp-wait` and passed it - **console
output stops updating long before the hang, so it does not locate one.**

## 2en (2026-10-13): bound the wait, and leave a breadcrumb through a wedge

Two instrumentation faults, both mine, neither about the GPU.

**The wait had no bound.** `doStart`'s remaining gates are `initGraphicsMQD`
(never once exercised) and five `submitMapQueuesPacket` calls, and every one
goes through `submitKIQFrame` -> `waitForHwStamp` again. Apple's
`waitForHwStamp` re-arms its 5 s poll for as long as the channel's
`vtable[0x2a0]` says to keep waiting (disassembled in 2cy), so the first wait
succeeding bounds none of the later ones.

The real wait is now bounded at 2 s. This is **not** 2cy's skip - that
substituted its own value and then narrated it as a hardware result, which
cost fifteen milestones. The poll is Apple's own, reproduced from the
disassembly already recorded here: its enabled check (`vtable[0x150]`), its
refresh (`vtable[0x1a8]` `checkForTimestampUpdate`, the only thing that moves
`channel+0x84`), its completion test `(int32)(stamp - channel+0x84) <= 0`,
and its 100 us interval. The one difference is a cap in place of the re-arm,
so a wait that would have completed still completes and one that would have
spun forever returns false into Apple's own failure path. If either vtable
slot reads NULL the code says in those words that it waited for nothing and
the false is an instrument failure, not a GPU result.

`-rbgpustampraw` restores Apple's unbounded wait. It is what wedged the
machine; it is not a default.

**A wedge left no trace.** Every `rbgpu_diag_set_num` schedules the NVRAM
refresh on its own `thread_call` - which is precisely why `ptb_written` and
`stamp_ret` reached NVRAM from a thread that then wedged. So a single number
updated at each gate survives any hang: `gate_stage` is set at the entry and
exit of `submitSetResourcesPacket` (40/45), `initGraphicsMQD` (50/55), each
`submitMapQueuesPacket` (60+ring / 70+ring) and each `waitForHwStamp`
(100+call / 150+call on success). The last value in NVRAM names the gate the
boot died in.

The NVRAM summary dropped **107** keys that boot (`d=107`), including
`gfxmqd_*` and `mapq_*` - the keys that would have named the gate. The hot
list is rewritten for the new frontier: `gate_stage` first, then the gates
after `submitSetResourcesPacket`, with the 2em keys kept because they are the
precondition (if `ptb_written` is 0 the rest means nothing). 2ei/2ek/2el are
answered and move down into the main list.

Build: UUID 2CD55966, hosttest passes. Boot-args unchanged - `-rbgpuptbase`
stays, `-rbgpustampwait` stays and is now bounded.

What the next armed boot decides: `gate_stage` names where the boot stops,
and the bounded wait means it should not stop at all - it should fail a gate
cleanly and come up to a desktop, as every boot since 2cy has.

### 2en on hardware (2026-10-13): it wedged again, and the breadcrumb did not survive

Armed boot, wedged, power-cycled. `rbgpu-diag-prev`:

```
d=34 armed=1 vmf_st_kiq=0 found_fb_routed=1 found_fb_calls=1 found_fb_refused=1
ttl_raw_init_ret=0 ttl_hw_init_ret=ffffffffcb588001 gc_sw_init_ret=1
smu_dma_mc_ok=1 smu_bringup_armed=1 bringup_err=0 smu_nmsgs=7 smu_writes=15
gfx_min=0 gfx_max=0 gfxoff_status=2 diag_rlc_stat=25 diag_rlc_gpm=b40017
diag_cp_stat=0 diag_rb0_cntl=a00000 diag_mec_hdr=def5def5 kiq_spec0=2
```

The key order is the 2en hot list, so this IS the 2en build. What it says:

- `kiq_spec0=2` and `vmf_st_kiq=0`: `startKIQ` ran and the `post-startKIQ`
  register dump got as far as the per-point fault record.
- `vmf_st_pre`, `stamp_calls`, `setres_calls`, `ptb_written` and
  **`gate_stage` are all absent**, so `submitSetResourcesPacket` was never
  entered and the 2em patch never fired this boot.
- `d=34` against the 2em boot's `d=107`: the summary is a snapshot of a table
  holding ~70 fewer keys. The refresh stopped much earlier than last time.

**So 2en's breadcrumb failed at exactly the job it was added for.**
`gate_stage` goes through `rbgpu_diag_set_num`, which only *schedules*
`performRefreshDiag` on a `thread_call`. That thread kept running through the
2em wedge, which is why `ptb_written` and `stamp_ret` survived then - and it
did not this time. A breadcrumb that can be starved by the hang it is meant
to locate is not a breadcrumb, and reading "`gate_stage` absent" as "the gate
was never entered" would be the fourteenth instance of this project's
recurring error: the absence is equally well explained by the refresh never
running again.

What IS supported: the boot stopped somewhere **inside
`rbgpu_dump_gc_regs("post-startKIQ")`**, at or after the per-point fault
record (which is written after the 2eh TTL call, so that call completed) and
before the function returned. The 2em boot ran this same path to completion,
so the stop is not deterministic at a fixed instruction - which is itself
worth knowing and was not knowable before.

## 2eo (2026-10-13): a breadcrumb that does not need the wedged machine's help

`rbgpu_gate_nvram(stage, what)` writes one NVRAM variable, `rbgpu-gate`, **on
the calling thread, synchronously**, flushed with
`IONVRAM-FORCESYNCNOW-PROPERTY`. No diag table, no `thread_call`, nothing that
another thread has to come along and finish. It overwrites, so what survives a
power cycle is the last point the boot actually reached:

```
nvram rbgpu-gate
```

The 2en gates (40/45 setres, 50/55 initGraphicsMQD, 60+ring / 70+ring
mapQueues, 100+call / 150+call waitForHwStamp) now go through it as well as
through the diag table, and `rbgpu_dump_gc_regs` is marked at nine points of
its own, each carrying the dump tag: `1 enter`, `2 after-2dz-fault-regs`,
`4 after-2ed-gc-hub`, `5 after-2ee-phys-scan`, `6 after-2ea-fault-per-point`,
`7 after-2do-hqd-probe`, `8 after-2el-ring-pte`, `9 dump-COMPLETE`.

Cost is one flash write per mark, a few dozen per armed boot. A wedge that
costs a power cycle and teaches nothing costs more.

Build: UUID EDB85AA6, hosttest passes. Boot-args unchanged.

What the next armed boot decides: `nvram rbgpu-gate` names the last block the
boot finished. `6 dump:post-startKIQ:...` points at the HQD probe (the only
code that writes `GRBM_GFX_CNTL`); `7` at the 2el ring-PTE read; `8` at the
2em register write; a `4x`/`1xx` value means the dump completed and the stop
is in a doStart gate after all.

### 2eo answered on hardware (2026-10-13): the stop is inside `initGraphicsMQD`

The synchronous breadcrumb worked on its first boot, and the boot also left a
full log (logs/20261013-170451-plugin), so the two can be checked against each
other:

```
nvram rbgpu-gate -> "50 initGraphicsMQD ENTER - never once exercised before 2em"
log.txt last line -> "2cx: initGraphicsMQD ENTER (pm4=0xffffff9085dfdc00)"
```

They agree exactly. The 2em result reproduced, and the chain is now the
longest it has ever been:

```
2em [post-startKIQ]: PT_BASE 0xfdfa001 -> 0x78fdfa001 ... WRITING
2em [post-startKIQ]: read back 0x78fdfa001 -> TOOK
2en: gate 40 - submitSetResourcesPacket ENTER
2dy: WRITE_DATA fence packet #1 ... dst=0xffbfdc0040 data=1
2en: gate 101 - waitForHwStamp ENTER
2dr: the REAL wait returned 1 - THE STAMP ARRIVED
2en: gate 151 - waitForHwStamp RETURNED TRUE
2en: gate 45 - submitSetResourcesPacket OK - doStart goes on to initGraphicsMQD
2cq: submitSetResourcesPacket(spec=...) -> 1 (OK)
2en: gate 50 - initGraphicsMQD ENTER
   <nothing further>
```

`bwait_iters=1`: the bounded wait was satisfied on its **first** poll, and
`fence_val_before=1` - the GPU had already written the fence before the wait
began. So the 2en cap costs nothing on a working path, and
**`submitSetResourcesPacket` returned 1 for the first time in this project.**

Also worth recording: `gate_stage=65` in the diag summary is hex 0x65 = 101
decimal = `waitForHwStamp ENTER`, while the synchronous `rbgpu-gate` reached
50. The diag-table breadcrumb lagged the real one by three gates on the very
boot that compared them, which is 2eo's premise demonstrated rather than
argued.

## 2ep (2026-10-13): split initGraphicsMQD, because it contains no loop

`initGraphicsMQD` (0xbea2542) disassembled: it has no loop of its own, so it
is blocking inside something it calls. Its outbound calls are

```
bea255d  (this->0x18)->vtable[0x2d8]()        the memory object
bea257d  r14->vtable[0x148]()                 the same pair initComputeMQD
bea25b3  r14->vtable[0x210](...)              uses, which 2cd walks already
bea2603  (this->0x18)->vtable[0x2c0]()        -> the SERVICE object
bea2626  svc->vtable[0x128](9, 0)             destroy, only if this->0x310
bea26a1  svc->vtable[0x120](9, 0, spec, out)  CREATE THE GRAPHICS QUEUE
bea2713  (this->0x30)->vtable[0x148](0xf702000042) then [0x120](..., 1)
bea2740  (this->0x30)->vtable[0x148](0xe802000042) then [0x120](..., 0)
```

`bea26a1` is the same slot and the same shape as `startKIQ`'s call, which uses
**command 8** and works; this is **command 9**, and it is the first time
anything has asked the layer below for a *graphics* queue. The last two pairs
are register read-modify-writes, and a read of a gated block can stall the SMN
bus - which is what the boot 6/7 RSMU timeouts were, and would explain a hang
that takes the whole machine rather than one thread.

The standing suspicion this meets: the SMU has reported `gfx_min = gfx_max = 0`
since 2di - GFX DPM never started - and this is the first code to touch the
graphics ring. The compute path (KIQ) now works end to end; the graphics one
may have no clock at all.

So the hook walks Apple's own object chain first, with a synchronous
breadcrumb between each step, and records the service function's address:
`51` reading `pm4+0x18`, `52` calling `vtable[0x2c0]`, `53` service object
obtained, `54` about to call the original. All pointer reads plus the one
virtual call Apple is about to make anyway.

Build: UUID 3BA80FD7, hosttest passes. Boot-args unchanged.

What the next armed boot decides, from `nvram rbgpu-gate`:

```
51/52/53   the fault is in our own walk, not Apple's call
54         the preconditions are sound and the hang is inside Apple's
           command-9 queue create or the GC register pair after it, and
           gfxmqd_svc_fn names the function to disassemble
55/51x     initGraphicsMQD completed and the stop moved on to mapQueues
```

### 2ep answered on hardware (2026-10-13): past the service object, and the suspect is MINE

Armed boot, wedged, power-cycled; the plugin_check that followed ran in the
recovery boot and captured an empty log, so `rbgpu-gate` is the whole record:

```
nvram rbgpu-gate -> "53 initGraphicsMQD: service object obtained"
```

Stages 51, 52 and 53 all passed, so Apple's object chain is sound:
`pm4+0x18` read, `vtable[0x2c0]` called, a service object returned. Gate 54
was not written.

**Between 53 and 54 there is no Apple code at all** - two vtable-slot reads,
two diag sets and one log line. That makes the breadcrumb itself a suspect,
and there is a concrete mechanism rather than a shrug:
`rbgpu_gate_nvram` called `IORegistryEntry::fromPath("/options", gIODTPlane)`
on **every** mark, which walks the device-tree plane under the registry lock -
dozens of times, from inside `IOService::start`, the one thread that must not
contend for the registry. That is a deadlock shape, and it matches the symptom
that a wedge takes `ioreg` and `reboot` with it instead of stalling one
thread. The 2eo boot (nine marks per dump point, the system still alive enough
to run plugin_check over SSH) against the 2ep boot (four more marks, hard
wedge) is consistent with it - two boots are a correlation, not a
demonstration, and it is written here as a suspicion to be removed.

## 2eq (2026-10-13): take the registry lookup out of the breadcrumb

Three changes, all to the instrument.

- `/options` is resolved **once**, lazily, and the entry retained for the
  boot; later marks only `setProperty` on an entry already in hand. The
  registry-plane walk is gone from the hot path.
- The nine per-block dump marks are **retired**. The 2eo boot showed the
  register dump runs to completion at every point (gate 40 follows it), so
  they were answering a settled question at the cost of 45 synchronous NVRAM
  writes a boot.
- The 53 -> 54 span gets its own marks - `56` dereferencing the service
  vtable, `57` vtable read, `58` slots read, `59` slots recorded - so the next
  boot names the instruction rather than the span.

Also fixed in passing: the first version of this change stripped the
`options->release()` from `saveDiagToNvram` instead of from
`rbgpu_gate_nvram`, which would have leaked an IORegistryEntry on every diag
refresh. Caught by reading back which function the edit landed in.

Build: UUID D7713190, hosttest passes. Boot-args unchanged.

What the next armed boot decides, from `nvram rbgpu-gate`:

```
54 or beyond   the span is clear and the fromPath lookup WAS the problem;
               the hang then moves to Apple's command-9 queue create
56/57/58/59    the span is real after all and the mark names the read
53 again       neither - the stop is in rbgpu_gate_nvram itself, and the
               synchronous NVRAM write has to go
```
