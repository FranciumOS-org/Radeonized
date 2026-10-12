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
  beyond queries and the user's decision per docs/RULES.md, so fixing the check
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
decision under the docs/RULES.md rule.

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
