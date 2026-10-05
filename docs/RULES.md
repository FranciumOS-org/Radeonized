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
