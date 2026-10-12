# RembrandtGPU — notes for Claude

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
