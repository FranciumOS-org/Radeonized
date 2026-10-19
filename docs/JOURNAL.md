# RembrandtGPU — the evidence journal

Chronological, authoritative for **what was measured, in order**, including
every reading that was later withdrawn. **Not to be trimmed**: the
withdrawals are the point — they are what has kept this project from
repeating its twenty-three recorded instances of reading a value without
first establishing what makes it meaningful.

Split out of CLAUDE.md on 2026-10-17 because CLAUDE.md is auto-loaded into
every session's context and this is ~8500 lines of it. Nothing was deleted;
the content below is byte-for-byte what CLAUDE.md held, and the durable
rules it taught are summarised in CLAUDE.md's "Rules that cost boots".

Read this when you need the history of a specific milestone (grep for its
tag, e.g. `2em`, `2gi`). `docs/HANDOFF.md` is the current state and is what
to read first.

---

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

## 2er (2026-10-13): why plugin_check.sh never finishes, and the fix

Located rather than assumed. Both 2026-10-13 wedge runs leave the same
fingerprint in `logs/<time>-plugin/`:

```
ioreg.txt   159 bytes   written
pci.txt       0 bytes   created, never filled
igpu.txt    absent      the next command never ran
```

So the script stops at one line, every time:

```
ioreg -w0 -p IOService -r -c IOPCIDevice -l > "$out/pci.txt"
```

That walk recurses **IGPU@0's whole subtree and serialises every property**,
so it is the first command that touches the branch holding the accelerator
still inside `start()`. The earlier `-c RembrandtGPU` lookup finishes because
our plugin's own IOService lives under IOResources, nowhere near it - which is
why the script always got as far as the registry verdict and no further.

And the reason Ctrl-C did nothing: a blocked `ioreg` is in an uninterruptible
kernel wait. It cannot be interrupted or killed, which is the same reason
`sudo reboot` could not finish.

Two changes to tools/plugin_check.sh:

- `rb_ioreg <seconds> <outfile> <args...>` runs each ioreg in the background,
  waits a bounded time for a completion stamp, and moves on without it. It
  does **not** try to kill the orphan - that cannot work - and says so. The
  script now always completes and writes everything it did get.
- It captures the NVRAM breadcrumbs **first**: `rbgpu-gate`, `rbgpu-diag`,
  `rbgpu-diag-prev`, `rbgpu-arm`, into `nvram.txt`. That is the primary
  evidence on a wedged boot and the script had never collected it.

Verified on a healthy boot: completes in 12 s, reports the breadcrumbs, the
loaded-UUID check and the full milestone-1 registry as before.

### 2eq answered on hardware (2026-10-13): the breadcrumb itself is the hazard

```
nvram rbgpu-gate -> "51 initGraphicsMQD: reading pm4+0x18"
```

**Earlier than the 2ep boot's 53**, in the same straight-line code, one build
later. That span is three pointer reads and two diag sets - deterministic
code, which cannot stop at different instructions on different boots unless
something non-deterministic is in it. The only such thing is the synchronous
NVRAM write.

Caching `/options` (2eq's main change) removed the registry-plane walk and did
**not** help, which rules that mechanism out and leaves the flash commit
itself: `setProperty` + `IONVRAM-FORCESYNCNOW-PROPERTY` takes the NVRAM lock
and runs a device transaction, and 2ep/2eq do that from inside
`IOService::start` several times inside one hook. Both boots died *at a gate
write*, just at different gates.

Note what this does NOT say: the write count is not the variable. 2eq removed
45 dump marks and added 4 inner ones - fewer writes in total - and stopped
sooner. So this is about doing the commit in that context at all, not about
how many.

**The control was already run.** The 2eo boot had the outer gates and **no**
marks inside this hook: it reached gate 50, did not wedge hard, and left the
machine alive enough to run plugin_check over SSH for a 43 KB log. So the
handful of outer gates are survivable and the inner ones are not.

## 2es (2026-10-13): mark without writing flash

`rbgpu_mark_2es(stage, what)` sets a diag key and logs - no NVRAM, no commit.
Marks `51`, `52`, `53`, `56`, `57`, `58`, `59` now use it. Gates `50`
(initGraphicsMQD ENTER) and `54` (about to call the original) stay on NVRAM:
two writes bracketing our walk, which is the 2eo configuration plus one.

That gives both records on the next boot. If it behaves like 2eo - no hard
wedge, system alive - `tools/plugin_check.sh` captures the `2es: mark N` lines
with the pointer values, and `rbgpu-gate` says whether Apple's call was
reached. If it wedges anyway, `rbgpu-gate` alone still distinguishes the two
cases.

Build: UUID A6474756, hosttest passes. Boot-args unchanged.

```
54        our walk is clear and the hang is inside Apple's command-9 queue
          create or the GC register pair after it - the first time that would
          be established rather than suspected
50        the hang is in our walk after all, and the log's last 2es mark names
          the step
```

### 2es answered on hardware (2026-10-13): the NVRAM theory is REFUTED, and the log does not locate the thread

logs/20261013-173533-plugin, armed, wedged, power-cycled - but read completely
for the first time, because 2er's bounded ioreg let plugin_check finish. 460
log lines (ring size 512, so nothing wrapped) plus nvram.txt.

```
nvram rbgpu-gate -> "50 initGraphicsMQD ENTER"
log.txt last line -> "2es: mark 51 - initGraphicsMQD: reading pm4+0x18"
rbgpu-diag       -> d=76 armed=1 gate_stage=65 ptb_written=1 ptb_back=78fdfa001
                    vmf_st_wait=0 stamp_calls=1 bwait_iters=1
                    fence_val_before=1 fence_val_after=1
```

The 2em/2eo result reproduced a third time: PT_BASE took, no GPUVM fault, the
fence was already written before the wait began, `submitSetResourcesPacket`
returned 1.

**Withdrawal 1: the synchronous NVRAM write is not the hazard.** 2es removed
every NVRAM write from the 51-59 span and the boot stopped in the *same span*,
at the *same mark* as 2eq. A cause that is removed while the effect stays is
not the cause. The 2eq conclusion ("what is left is the flash commit itself")
is withdrawn.

**Withdrawal 2: the log's last line does not locate the thread.** `RBGPULog`
only reaches the registry through `performRefreshDiag`'s
`setProperty("RBGPULog", ...)`, so the published ring ends at the last
*refresh*, not at the thread's stopping point. This is exactly the lesson 2em
recorded for the console photograph - "console output stops updating long
before the hang, so it does not locate one" - and I read the log as if it were
exempt. Fifteenth instance of the recurring error.

**What the two records say when read together, and it is a real finding.**
`performRefreshDiag` publishes the log first and calls `saveDiagToNvram` last:

- the published log contains mark 51, so a refresh pass ran *after* mark 51;
- that same pass never finished its flash commit - the summary still reads
  `gate_stage=0x65` = 101 = `waitForHwStamp ENTER`, three gates stale, where
  the thread had already passed gates 151, 45 and 50.

So the refresh thread published the log and then **blocked inside the NVRAM
transaction**, while the start() thread went on to need that same controller at
gate 54. Two writers, one NVRAM lock, and the one that must not block is the
one holding up `IOService::start`. Gate 50's write succeeded because it came
first; gate 54's never appeared.

That also explains the shape of 2ep (died after gate 53's write) and 2eq (died
after gate 51's write) without needing the flash commit to be intrinsically
unsafe: every one of those boots died *at a gate write*, with a refresh pass
in flight. And 2eo, which had no inner marks at all, wrote gate 50 and then
hung with the system still alive enough for SSH - the one boot where the
start() thread was plausibly inside Apple's original rather than in an NVRAM
wait.

Note what is still NOT established: whether Apple's command-9 queue create was
ever reached. Gate 54 is missing, and a missing synchronous write is equally
well explained by the write blocking as by the code never getting there. That
is the question 2et exists to answer.

## 2et (2026-10-13): give the breadcrumb the NVRAM controller to itself

`rbgpu_nvram_quiesce()` sets a flag that makes `saveDiagToNvram` return
immediately. `rbgpu_gate_2en` calls it at the first gate of the chain
(stage >= 40), so from `submitSetResourcesPacket` ENTER onward the refresh
thread stops flushing flash entirely and the synchronous `rbgpu-gate` writes
are the only NVRAM traffic in the boot.

What is kept and what is given up, deliberately:

- `RBGPULog` and `RBGPUDiag` keep refreshing into the registry, so a boot that
  stays readable still yields the full log and the whole diag table.
- `rbgpu-diag` stops advancing at gate 40. Everything that decides the current
  frontier is already in it by then (`ptb_written`, `ptb_back`, `vmf_st_*`,
  the SMU keys), and the stamp keys are proven three boots running. The gate
  breadcrumb replaces them for everything after.
- `saveDiagToNvram` still does its own `IORegistryEntry::fromPath` walk per
  call, which 2eq removed from the gate path; quiescing it retires that too
  for the rest of the boot.

Build: UUID A5A1F416, hosttest passes. Boot-args unchanged.

What the next armed boot decides, from `nvram rbgpu-gate`:

```
54        our walk is clear and the hang is inside Apple's command-9 queue
          create or the GC register pair after it - established, finally,
          rather than suspected
55 / 51   initGraphicsMQD RETURNED and the stop moved on to mapQueues
50        the write was never the blocker and the stop really is in our walk;
          the log's last 2es mark then bounds it, with the caveat above that
          the log lags the thread
```

### 2et answered on hardware (2026-10-13): gate 54 is genuinely not reached

logs/20261013-174345-plugin, armed, wedged. `plugin_check` ran **in the armed
boot** this time and `pci.txt` completed at 401408 bytes, where every previous
wedge left it at 0 - so the IGPU registry subtree was not locked and the
machine was in better shape than the 2es wedge.

```
nvram rbgpu-gate -> "50 initGraphicsMQD ENTER"
log.txt last line -> "2es: mark 52 - initGraphicsMQD: calling vtable[0x2c0]"
rbgpu-diag       -> d=27 armed=1, and NO gate_stage / ptb_* / stamp_* at all
```

**The quiesce fired.** The summary froze far earlier than the 2es boot's
(which carried `gate_stage=65`, `ptb_written`, `stamp_calls`, `bwait_iters`):
its last completed flush predates `post-startKIQ`, and nothing after gate 40
reached flash. That is 2et doing exactly what it was written to do.

**So the missing gate 54 can no longer be blamed on a blocked flash write,
and that settles it: the thread does not reach gate 54.** The stop is between
gate 50 and gate 54 - inside the 51-59 walk. Two boots now with the NVRAM
explanation removed and gate 54 still absent.

The log also reached **mark 52**, one further than 2es's mark 51, which means
`pm4+0x18` was read, `memobj` was non-NULL, its vtable was dereferenced and
slot 0x2c0 was read. The only substantive thing left in the span is the call
itself: `fn_2c0(memobj)` - **Apple's own `vtable[0x2c0]`**, which Apple makes
for itself at 0xbea2603 a moment later. Per the 2es withdrawal the log lags
the thread, so mark 52 is an upper bound and not a location; the 2ep boot
wrote gate 53, so 0x2c0 has returned on at least one boot.

Open instrumentation question, recorded rather than chased: the published
`RBGPULog` held only **61** lines (11 KB) against the 2es boot's 460 (63 KB),
starting at 14.4 s with everything before gate 101 absent - which is why the
`2et:` quiesce line and gate 40 are not in it. The ring is 512 and the same
script read 460 lines one boot earlier, so this is the publish side, not the
script.

## 2eu (2026-10-13): take the walk out, so the two gates have nothing between them

The walk is now **off** unless `-rbgpumqdwalk` is set. By default
`initGraphicsMQD`'s hook is gate 50, gate 54, the original - nothing else at
all between the two NVRAM writes.

That makes the next boot decisive in a way no previous one has been:

```
54 written   the span was ours, and the hang is inside Apple's own
             initGraphicsMQD - the command-9 graphics-queue create at
             0xbea26a1 or the GC register read-modify-writes at 0xbea2713 /
             0xbea2740. Established, not suspected.
50 only      two synchronous NVRAM writes with nothing between them still
             lose the second, so the gate mechanism itself is the problem and
             the breadcrumb needs a channel that is not NVRAM.
```

Worth stating the reframe this makes available either way: our marks were
never plausibly *causing* a hang in Apple's object chain - at most they reach
Apple's own `vtable[0x2c0]` a few microseconds before Apple does. If the walk
hangs there, so would the original.

Build: UUID E1EA8BF0, hosttest passes. Boot-args unchanged.

### 2eu answered on hardware (2026-10-13): GATE 54 — the hang is in APPLE'S code

logs/20261013-180057-plugin, armed, wedged, power-cycled. With the walk gone
there is nothing between the two gates but the call to the original, and the
second gate landed:

```
nvram rbgpu-gate -> "54 initGraphicsMQD: calling the ORIGINAL - anything
                     after this is Apple's comma"
log.txt last line -> "2en: gate 54 - initGraphicsMQD: calling the ORIGINAL"
```

Both records agree and the log is complete (65916 bytes, 2er's bounded ioreg
again). So, after four boots of chasing my own instrument:

- **the gate mechanism works** - two synchronous NVRAM writes inside
  `IOService::start`, back to back, both landed;
- **the thread reaches Apple's `initGraphicsMQD` and does not come back.**

That is the first time in this milestone family that the hang is established
to be in Apple's code rather than suspected. `pci.txt` is 0 bytes again
(against the 2et boot's 401 KB), so the wedge is the hard kind - the start()
thread is sitting in Apple's function holding the IOService lock.

**And one correction to 2eu's own reframe.** I wrote that our marks "at most
reach Apple's own `vtable[0x2c0]` a few microseconds before Apple does". The
disassembly below shows Apple's FIRST call on `pm4+0x18` is `vtable[0x2d8]`,
and `vtable[0x2c0]` is reached only inside a conditional branch. If that
branch is not taken, the walk was calling something Apple never calls - a
real behavioural difference, not a head start. The reframe was not supported.

### Apple's initGraphicsMQD, disassembled end to end (0xbea2542-0xbea275b)

```
A  bea255d  (pm4+0x18)->vtable[0x2d8]()      -> memobj; NULL -> return false
B  bea257d  memobj->vtable[0x148]()          -> bool;  false -> return false
C  bea258b  pm4+0x250                         NULL -> return false
D  bea25b3  memobj->vtable[0x210](pm4+0x250, 0x1000, &slot, 0x100)
E  bea25c8  slot->vtable[0x118]()            -> ptr;   NULL -> return false
F  bea25ea  bzero(ptr, 0x1000)               (stub resolved with kc_stubs.py)
G  bea25f2  IF pm4+0x10 == 1:
     bea2603  (pm4+0x18)->vtable[0x2c0]()    -> service object
     bea2626  IF pm4+0x310 == 1: svc->vtable[0x128](9, 0)      destroy
     bea26a1  svc->vtable[0x120](9, 0, spec, out)  CREATE THE GRAPHICS QUEUE
H  bea26f1  memobj->vtable[0x218](&out2)                        ALWAYS
I  bea2713  (pm4+0x30)->vtable[0x148](0xf702000042) -> v
   bea2726  (pm4+0x30)->vtable[0x120](v, 1)                     ALWAYS
J  bea2740  (pm4+0x30)->vtable[0x148](0xe802000042) -> v
   bea2750  (pm4+0x30)->vtable[0x120](v, 0)                     ALWAYS
```

The function has no loop, so it is blocking inside one of these. Three things
worth stating that were not known before:

- **Region G runs only when `pm4+0x10 == 1`.** The command-9 graphics-queue
  create - the thing this whole milestone family is about - is behind one byte.
- There is a **4 KB `bzero`** at F, into a pointer that came from
  `slot->vtable[0x118]()`. If that is a GPU-visible or uncached mapping, a
  4096-byte store loop through it is a candidate for a stall in its own right.
- The pairs at I and J are `vtable[0x148](encoded_id)` returning a value that
  is passed straight to `vtable[0x120](value, flag)` - a read then a write,
  on `pm4+0x30`, with flag 1 then 0. The ids `0xF702000042` / `0xE802000042`
  are NOT the `(dword offset << 32) | 11` form recorded earlier in this file
  (low word here is 0x02000042), so the register they name is **not yet
  decoded** and nothing should be inferred from the raw value.

## 2ev (2026-10-13): read the byte that decides a third of it

Four plain field reads before calling the original - no virtual calls, so
nothing Apple is not about to do itself - and the two deciding bytes go into
gate 54's own NVRAM text so a wedge carries them out:

```
pm4+0x10    1 -> region G runs; 0 -> the create is SKIPPED
pm4+0x310   1 -> the destroy at 0xbea2626 runs first
pm4+0x250   the buffer handed to vtable[0x210]
pm4+0x30    the register-access object used by I and J
```

Diag keys `gfxmqd_f10`, `gfxmqd_f310`, `gfxmqd_f250`, `gfxmqd_f30`, promoted
into the NVRAM hot list right behind `gate_stage`.

What the next armed boot decides, from `nvram rbgpu-gate`:

```
f10=1   the command-9 create is live, and the next build splits G from H-J
        by instrumenting the service object's own vtable[0x120]
f10=0   the create never runs, so the hang is in A-F (the allocation chain
        and the 4 KB bzero) or in the GC register pairs H-J - and the
        standing gfx_min = gfx_max = 0 suspicion becomes the leading
        explanation rather than a background one
```

Build: UUID 527576AC, hosttest passes. Boot-args unchanged.

### 2ev answered on hardware (2026-10-13): it went BACK to gate 50

No `plugin_check` capture from the armed boot, so the records are NVRAM read
from the recovery boot that followed. Checked before interpreting, per the
2cu/2cv rule:

```
kmutil showloaded -> com.rembrandtgpu.plugin 527576AC   == build/out (2ev)
rbgpu-diag        -> d=0 armed=0                        this is the recovery boot
rbgpu-diag-prev   -> d=27 armed=1 ttl_hw_init_ret=ffffffff97f08001
                     (the 2eu boot read ...b8a33001, so this IS a new boot)
nvram rbgpu-gate  -> "50 initGraphicsMQD ENTER"
```

`rbgpu-gate` is a single overwrite variable and an unarmed boot never writes
it, so the 50 belongs to the armed 2ev boot. **2eu reached 54 and 2ev did
not**, with the only difference being what 2ev put between them: five field
reads, four `rbgpu_diag_set_num` calls, one `rbgpu_log`, one `snprintf`.

So the field reads never happened and the question 2ev asked is unanswered.
What it did answer is a different and more useful thing — the seven-boot
table, with 2eu as the control:

| build | between gate 50 and gate 54 | gate reached |
|---|---|---|
| 2eo | nothing (no inner marks) | 50 |
| 2ep | 4 NVRAM marks | 53 |
| 2eq | 4 NVRAM marks, `/options` cached | 51 |
| 2es | 7 log+diag marks, no NVRAM | 50 |
| 2et | same, plus the NVRAM quiesce | 50 |
| **2eu** | **nothing at all** | **54** |
| 2ev | 4 diag sets + 1 log + 1 snprintf | 50 |

**Whatever this hook does between the two gates decides how far the thread
gets, and only the empty span reaches Apple's code.** That is not the NVRAM
flash commit (2es/2et removed it and nothing improved) and not the registry
walk (2eq removed it and nothing improved); it is the diag/log machinery
itself being exercised from inside this particular hook. The mechanism is
still unidentified and is NOT claimed here - what is established is that the
span must stay empty for the experiment to run at all.

Plain loads from a valid object cannot block, so the suspect is
`rbgpu_diag_set_num` / `rbgpu_log`: both take `g_diagLock` and schedule
`performRefreshDiag`, which does `setProperty` on the peer from another
thread while this one is inside `IOService::start`.

## 2ew (2026-10-13): keep the span empty, read the fields somewhere safe

Two changes, no new hardware access.

- The initGraphicsMQD hook goes back to the 2eu shape: gate 50, gate 54, the
  original, nothing in between. That is the only configuration that has ever
  reached Apple's code.
- 2ev's field reads move into the **doStart** hook, whose `this` IS the same
  PM4 engine and whose logging has been safe in every boot since 2ce.

What the reads bound, stated rather than assumed:

- `pm4+0x10` - nothing in this chain writes it, so its value at doStart entry
  is its value at the branch (0xbea25f2). This is the one that matters.
- `pm4+0x310` - initGraphicsMQD writes it itself (cleared 0xbea2634, set
  0xbea26dd), but this is the FIRST call, so the value read here is what the
  branch at 0xbea260c tests.
- `pm4+0x250` / `pm4+0x30` - read before `initComputeMQD` runs, so a lower
  bound on what 0xbea258b sees, not a proof of it.

Build: UUID 1E39C00A, hosttest passes. Boot-args unchanged.

What the next armed boot decides, from the log plus `nvram rbgpu-gate`:

```
gate 54 again + f10=1   the create is live; the next step splits region G
                        from H-J, and it must do so WITHOUT touching the
                        50->54 span
gate 54 again + f10=0   region G never runs, so the hang is in A-F (the
                        allocation chain and the 4 KB bzero) or in the GC
                        register pairs - and gfx_min = gfx_max = 0 becomes
                        the leading explanation
gate 50 again           the doStart reads are ALSO enough to cost the thread
                        its progress, which would make the hook layer itself,
                        not its placement, the thing to fix
```

### 2ew answered on hardware (2026-10-13): f10 = 0 — THE COMMAND-9 CREATE NEVER RUNS

Read from the recovery boot that followed; build checked first, per the
2cu/2cv rule.

```
kmutil showloaded -> com.rembrandtgpu.plugin 1E39C00A   == build/out (2ew)
rbgpu-diag        -> d=0 armed=0                        the recovery boot
rbgpu-diag-prev   -> d=36 armed=1 ttl_hw_init_ret=ffffffffc6ef3001  (new boot)
     gfxmqd_f10=0  gfxmqd_f310=0
     gfxmqd_f250=f40fb27000  gfxmqd_f30=ffffffd39ba10000
nvram rbgpu-gate  -> "151 waitForHwStamp RETURNED TRUE"
```

**The reads landed and the answer is f10 = 0.** Moving them to the doStart
hook worked exactly as intended, and the question 2ev could not answer is
settled:

- **`pm4+0x10 = 0`, so region G is SKIPPED.** Apple's command-9
  graphics-queue create at `0xbea26a1` **never runs on this chip**. The whole
  line of suspicion this milestone family is named for is closed.
- `pm4+0x310 = 0`, so the destroy at `0xbea2626` does not run either - which
  follows, since it is nested inside G.
- `pm4+0x250 = 0xF4_0FB2_7000` - a real carve-out MC address, 0x1000 below the
  ring that `startKIQ` was handed (`0xf40fb28000`). The buffer region D passes
  to `vtable[0x210]` is allocated where it should be.
- `pm4+0x30 = 0xffffffd39ba10000` - a kernel object, the register accessor
  regions I and J use.

So the hang is in **A-F** (the allocation chain and the 4 KB `bzero` at
`0xbea25ea`) or in **H-J** (`vtable[0x218]`, then the two GC register
read-modify-writes). And the standing suspicion is now the leading
explanation rather than a background one: `gfx_min = gfx_max = 0` since 2di,
GFX DPM never started, and I/J are the first GC register accesses this path
makes.

**Withdrawal: the 2ev seven-boot table was over-read.** 2ev concluded that
"whatever this hook does between the two gates decides how far the thread
gets, and only the empty span reaches Apple's code". 2ew ran with the span
empty - the exact configuration that gave 54 - and the gate came back **151**,
which is *before* gates 45, 50 and 54. The prediction failed.

So the stopping points across seven boots (53, 51, 50, 50, **54**, 50, 151)
are seven single samples of something that varies, and I read their ordering
as causal. 2eu's gate 54 was one sample, not a control. Sixteenth instance of
the recurring error, and the first where I built a table out of n=1 rows.

What all seven boots DO agree on: the thread gets into this region and the
machine wedges hard. Nothing in the seven distinguishes where.

## 2ex (2026-10-13): make one boot say whether the thread stopped or the record did

The overwrite form of `rbgpu-gate` cannot tell "the thread stopped at 151"
from "45, 50 and 54 were written and the tail was lost". Every conclusion
drawn from a single gate value has that ambiguity under it, which is how the
2ev table happened.

`rbgpu_gate_nvram` now keeps a bounded **trail** and rewrites the whole string
on every gate, so the variable reads e.g.

```
40 101 151 | 151 waitForHwStamp RETURNED TRUE      the thread stopped there
40 101 151 45 50 54 | 54 calling the ORIGINAL      it reached Apple's code
```

Cap is 192 bytes with `CAP` appended rather than silent truncation; the chain
writes about twenty gates. Same one flash write per gate as before - no new
mechanism, just a record that does not throw away its own history.

Build: UUID 41E6681D, hosttest passes. Boot-args unchanged.

What the next armed boot decides:

```
trail ends at 151      the thread really does stop right after the first
                       stamp wait, and the 2eu gate-54 boot is the outlier to
                       explain - not the rule
trail reaches 54       the overwrite form was losing the tail all along, every
                       single-gate reading in 2eo-2ew is suspect, and the
                       hang is in Apple's A-F or H-J as f10=0 now implies
trail reaches 55 / 6x  initGraphicsMQD RETURNED and the frontier has moved on
                       to submitMapQueuesPacket
```

### 2ex answered on hardware (2026-10-13): the trail ends at 151

Read from the recovery boot that followed; build and arming checked first, per
the 2cu/2cv rule.

```
kmutil showloaded -> com.rembrandtgpu.plugin 41E6681D   == build/out (2ex)
rbgpu-diag        -> d=0 armed=0                        the recovery boot
rbgpu-diag-prev   -> d=27 armed=1 ttl_hw_init_ret=ffffffffddbee001
                     (2ew read ...c6ef3001, so this IS a new boot)
nvram rbgpu-gate  -> "40 101 151 | 151 waitForHwStamp RETURNED TRUE"
```

**The trail mechanism works** - three gates accumulated in one variable, so the
2ex ambiguity is gone for this boot: no gate after 151 committed. And gate 151
is written *after* `rbgpu_dump_gc_regs("post-stamp-wait")`, so that entire
register dump ran to completion, which no previous reading could establish.

What the span between gate 151 and gate 45 actually contains: one
`rbgpu_diag_set_num("stamp_ticks")`, one `rbgpu_log`, the return through
Apple's `submitKIQFrame` epilogue (`submitSetResourcesPacket` tail-calls it),
two more diag sets, then gate 45.

**And the 2eu boot reached gate 54**, which is past 45 and past 50 - the same
straight-line span, traversed successfully there. So the stopping point is
**not located by code position**, and that is the first thing in this
milestone family to be established across two trustworthy records rather than
inferred from an ordering.

A deadlock was checked for and is NOT there: `performRefreshDiag` releases
`g_diagLock` before its two `setProperty` calls (RembrandtGPUPeer.cpp:912),
so the lock-inversion shape - start() thread waiting on `g_diagLock` while the
refresh thread waits on the registry lock that `IOService::start` holds -
cannot occur. Written down because it was about to be written up as a finding.

## 2ey (2026-10-13): the control every gate reading has assumed

Every reading from 2eo to 2ex rests on one untested premise: **that the trail
stops where the THREAD stops.** An asynchronous death that merely catches the
thread wherever it happens to be produces exactly the same record - and
produces a *varying* stopping point in deterministic code, which is what has
now been measured twice. The two possibilities need completely different
fixes and are indistinguishable from a single thread's breadcrumb.

This matters more than any new question about Apple's code, because since 2em
there IS something asynchronous on this chip for the first time: PT_BASE
points GPUVM at the real GART, whose 1512 valid PTEs are all
`AMDGPU_PTE_SYSTEM` host bus addresses, and the CP is live and executing. The
GPU can now reach host memory and raise interrupts, neither of which it could
do before 2em - and 2em is the boot where the wedging started.

`rbgpu_heartbeat_start()` arms a 250 ms `thread_call` that writes
`rbgpu-beat` with its own counter plus the last gate it saw. It is armed at
gate 40, beside the 2et quiesce, so it costs nothing on a boot that never
reaches the chain.

The gate path keeps absolute priority: it claims `g_rbgpuNvramBusy` across its
write without ever waiting for it, and the heartbeat is the side that yields.
But a heartbeat that goes silent whenever the flag is held could not tell "the
gate path is wedged inside its own flash write" from "the machine is dead", so
after three consecutive skips (~750 ms) it writes anyway and records
`forced`.

```
nvram rbgpu-beat
```

Three outcomes, all distinguishable in one variable:

```
n large, forced=0     the machine is ALIVE and the start() thread alone is
                      stuck; the trail does locate it, and the span between
                      gate 151 and gate 45 is where to look
n large, forced>=1    the thread is stuck INSIDE the gate's own NVRAM write -
                      the 2eq/2es suspicion back, with direct evidence this
                      time instead of the inference that 2es refuted
absent / n small      the whole machine stopped within ~250 ms of the trail's
                      last gate. The stop is not a thread blocking in Apple's
                      code at all, every single-gate and trail reading from
                      2eo to 2ex is measuring where the thread happened to be,
                      and the next question is what the GPU is doing
                      asynchronously now that GPUVM resolves
```

`n` also dates the stop: the beat carries `mach_absolute_time()`, so the gap
between the last beat and the gate write is readable.

Build: UUID 76617375, hosttest passes. Boot-args unchanged.

### 2ey answered on hardware (2026-10-13): THE MACHINE IS ALIVE — the trail does locate the thread

logs/20261013-184415-plugin. `plugin_check` ran **in the armed boot** this
time (`rbgpu-diag` reads `armed=1`, `rbgpu-diag-prev` is the previous recovery
boot), 42 KB of log, and the build and arming were checked first:

```
kmutil showloaded -> com.rembrandtgpu.plugin 76617375   == build/out (2ey)
ttl_hw_init_ret   -> 565a0001   (2ex read ddbee001, so a new boot)
nvram rbgpu-gate  -> "40 101 151 | 151 waitForHwStamp RETURNED TRUE"
nvram rbgpu-beat  -> "n=16 gate=151 skips=0 forced=0 t=18649635959"
log.txt last line -> "[15.393382] 2en: gate 151 - waitForHwStamp RETURNED TRUE"
pci.txt           -> 0 bytes
```

**Branch 1 of the 2ey table, decisively.** The heartbeat ran 16 ticks - about
4 s - after the trail's last gate, the last beat at 18.65 s against gate 151's
15.39 s, and the machine then stayed up long enough for the user to run
`plugin_check` in that same boot. So:

- **the whole-machine death is excluded.** The asynchronous-fault alternative
  that 2ey existed to test - the GPU now being able to reach host memory and
  raise interrupts since 2em - is not what is happening. The start() thread
  alone is stuck, and `pci.txt` at 0 bytes says why `ioreg` blocks: that
  thread holds the IOService lock over the IGPU subtree.
- **the trail does locate the thread**, which every reading from 2eo to 2ex
  had assumed and none had tested.
- **`skips=0 forced=0`**: the heartbeat never once found the gate path holding
  the NVRAM flag. So the thread is **not** stuck inside the gate's own flash
  write - the 2eq/2es suspicion, refuted this time by direct measurement
  rather than by removing it and seeing no change.

So the stop is in the span between gate 151's commit and gate 45's commit, and
that span is now trustworthy. The published log agrees independently: its last
line is gate 151, with 3.26 s of proven liveness after it. (That agreement is
corroboration, not proof - per the 2es withdrawal the published log ends at
the last *refresh*, and a refresh pass blocked at 15.39 s would end it there
whatever the thread did.)

Two instrumentation notes from the same capture:

- The 2ey log line exceeded `RBGPU_LINE_MAX` (384) and its published entry
  **swallowed the entry after it** - gate 40's line is glued onto the tail of
  the truncated heartbeat line, and the same happened to gate 101 behind the
  2dy fence line. Long log lines cost the next line. Shortened in 2ez.
- `make kext tools` does **not** rebuild the plugin; only `make plugin` does.
  A first build of 2ez reported the unchanged 2ey UUID. Always check the UUID
  moved, which is the 2cw lesson recurring in a new place.

## 2ez (2026-10-13): empty the one span that is now known to hold the thread

The span from gate 151 to gate 45 holds, in order: one
`rbgpu_diag_set_num("stamp_ticks")`, one `rbgpu_log`, the return through
Apple's `submitKIQFrame` epilogue, and two more diag sets. Apple's part is a
handful of instructions - `waitForHwStamp` is the last call `submitKIQFrame`
makes, and `submitSetResourcesPacket` tail-calls it, so the return comes
straight back to our hook. Everything else in the span is ours.

Emptying the span is the single move that has ever shifted this frontier:
2eu did exactly this for 50->54 and got into Apple's code for the first time.

- In the `waitForHwStamp` hook, the diag sets and the log line move **before**
  gate 151, so the only thing after that gate is `return ret`.
- In the `submitSetResourcesPacket` hook, gate 45 becomes the **first**
  statement after the original returns, ahead of its diag sets and log.

That leaves Apple's epilogue and one call as the entire span.

```
trail reaches 45      the span was ours, and the frontier moves on to
                      initGraphicsMQD with the stop no longer in our tail code
trail still ends 151  the stop is inside Apple's own submitKIQFrame epilogue,
                      with nothing of ours left in the span to blame - which
                      would be a hard fact and a new place to disassemble
```

Build: UUID 91991334, hosttest passes. Boot-args unchanged.

### 2ez answered on hardware (2026-10-13): the thread is not stuck — it is CRAWLING

logs/20261013-190821-plugin, `plugin_check` ran in the armed boot again
(`rbgpu-diag` armed=1, `ttl_hw_init_ret=a8ef2001`, a new boot), build checked:

```
kmutil showloaded -> com.rembrandtgpu.plugin 91991334   == build/out (2ez)
nvram rbgpu-gate  -> "40 101 | 101 waitForHwStamp ENTER"
nvram rbgpu-beat  -> "n=27 gate=101 skips=0 forced=0 t=21884893126"
log.txt           -> 52 lines, last is the post-stamp-wait dump's final line
```

The trail went **backwards** - 2ey reached 151, 2ez only 101 - and emptying
the span did not help. Taken as another stopping point that would be the
eighth sample of something varying. It is not: the timestamps say what is
actually happening.

**The measurement.** Gaps between consecutive RBGPULog lines, same code, same
dump function, within and across boots:

```
2ey boot, last 8 gaps:   0.007 0.007 0.009 0.012 0.009 0.009 0.007 0.037 ms
2ez boot, last 8 gaps:   4.766 4.154 5.197 5.748 5.786 5.856 3.999 7.325 ms
2ez boot, at-stamp-wait: 9.434 9.417 8.964 10.129 9.340 9.229 9.858 10.476 ms
2ez boot, the 25 lines between those two runs:  0.002 - 0.048 ms
```

**Within one boot the same logging code runs at 3 us and at 10 ms**, swinging
back and forth. That is not a hang and not a code position - it is
contention, and it is ours.

**The mechanism, from the sizes.** Every `rbgpu_log` call runs
`scheduleRefreshDiag`, and `performRefreshDiag` memcpy's **235 KB** under
`g_diagLock` (512 x 400-byte `LogEntry` = 200 KB, plus 320 x 112-byte
`DiagEntry` = 35 KB) and then builds up to 512 `OSString`s, an `OSArray` and a
320-entry `OSDictionary` and `setProperty`s both. With log lines arriving back
to back the refresh runs continuously, and the start() thread blocks on
`g_diagLock` for the snapshot every time it logs.

**A thread throttled to milliseconds per log line is indistinguishable from a
wedge.** It holds the IOService lock the whole time - which is exactly why
`ioreg` blocks and `pci.txt` is 0 bytes - and with hundreds of log lines still
to come in the doStart chain it would crawl for minutes. It also stops
wherever it happens to be when the machine is power-cycled, which is a
complete explanation for the one thing all eight boots agree on and nothing
distinguished: **the stopping point varies because there is no stopping
point.**

What this retires, and these were all real observations read the wrong way:

- the 2ev seven-boot table (already withdrawn) and its successor readings;
- "2eu reached 54 and 2ev did not" as a statement about the span's contents -
  it is a statement about how much logging each build did before the
  observation;
- 2es's "the refresh thread published the log and then blocked inside the
  NVRAM transaction" - it was not blocked, it was looping;
- the beat going quiet at n=16 / n=27 while the machine lived: the
  `thread_call` pool is saturated by back-to-back refresh passes, so the
  beat's re-arm is starved. Starvation, not death - and 2ey's branch 1
  ("the machine is ALIVE") stands, which is what made this readable.

Seventeenth instance of the recurring error, and the largest: eight boots of
this milestone family were measuring the instrument.

## 2fa (2026-10-13): stop the publisher from throttling the thread it observes

Two changes, both in `RembrandtGPUPeer.cpp`, neither touching the GPU.

- `scheduleRefreshDiag` rate-limits to **at most one refresh per 250 ms**.
  The publisher exists to be read after the fact; nothing needs it sooner.
- The snapshot copies only the diag entries in use (`g_diagCount` x 112)
  instead of a flat 35 KB, shortening the `g_diagLock` hold.

The prediction is quantitative, which no step in this family has been able to
make before, and the next boot's own log checks it:

```
inter-line gaps back to single-digit us   the contention was the whole of it,
                                          and the trail should now run far
                                          past 151 into the mapQueues gates
gaps still in milliseconds                the rate limit is not where the cost
                                          is, and the next place to look is
                                          the two setProperty calls rather
                                          than the snapshot
```

Worth stating what this does NOT settle: whether anything in Apple's
`initGraphicsMQD` is actually wrong on this chip. 2ew's `f10 = 0` stands -
the command-9 create never runs - but every claim about *where* the thread
stops inside that function, from 2eo onward, is now withdrawn. The frontier
is wherever the next boot gets to with the instrument out of the way.

Build: UUID 8CE83E37, hosttest passes. Boot-args unchanged.

### 2fa answered on hardware (2026-10-13): the prediction held, and the trail advanced

logs/20261013-191504-plugin, `plugin_check` in the armed boot
(`armed=1`, `ttl_hw_init_ret=d0c85001`, a new boot), build checked:

```
kmutil showloaded -> com.rembrandtgpu.plugin 8CE83E37   == build/out (2fa)
nvram rbgpu-gate  -> "40 101 151 45 | 45 submitSetResourcesPacket OK"
nvram rbgpu-beat  -> "n=28 gate=45 skips=0 forced=0 t=21640918143"
log.txt           -> 148 lines
```

**The quantitative prediction was right.** Inter-line gaps across the whole
boot:

```
min 0.002 ms   median 0.004 ms   p90 0.028 ms
last 10 gaps: 0.004 0.003 0.004 0.007 0.004 0.003 0.004 0.003 0.011 0.007 ms
```

against the 2ez boot's 4-10 ms in the same dump function. The contention was
the whole of it, and 2ez's "the thread is crawling" is confirmed by the fix
working rather than by the inference alone.

**And the trail moved**: `40 101 151 45`, where 2ez got `40 101`. Gate 151 and
gate 45 both landed, so 2ez's reorder was also right - that span was ours.

The thread's last log line is at **14.928 s** and the beat's last write at
**21.641 s**: 6.7 s of proven liveness after it, with the `2cq:` line (which
2ez moved to *after* gate 45) absent. So the stop is between gate 45's commit
and gate 50's commit, and this time the instrument is measured to be out of
the way.

### What is actually in that span: Apple programs the GRAPHICS ring

`doStart` (0xbea1190) between the two calls, disassembled:

```
bea12a4  callq submitSetResourcesPacket            <- gate 45 is here
bea12cb  r15 = this->0x30                          the register accessor
bea12df  [0x148](0x1DE1..0B CP_RB0_CNTL)   -> bea12f2 [0x128](v, 0x80000000)
bea130c  [0x148](0x1DE2..0B CP_RB_RPTR_WR) -> bea131c [0x120](v, ..)
bea132f  [0x148](0x1DF4..0B CP_RB0_WPTR)   -> bea133f [0x120](v, ..)
bea1359  [0x148](0x1DF5..0B CP_RB0_WPTR_HI)-> bea1369 [0x120](v, ..)
bea1383  [0x148](0x1DE1..0B CP_RB0_CNTL)   -> bea1396 [0x130](v, ..)
bea13ad  callq initGraphicsMQD                     <- gate 50 is here
```

Five register read-modify-write pairs, and three things fall out:

- **The ids decode.** They are exactly the `(dword offset << 32) | 11` form
  this file recorded for Apple's encoded register ids, 11 = GC_HWID, and the
  dword offsets are `mmCP_RB0_CNTL` 0x1de1, `mmCP_RB_RPTR_WR` 0x1de2,
  `mmCP_RB0_WPTR` 0x1df4, `mmCP_RB0_WPTR_HI` 0x1df5 from
  docs/linux-ref/gc_10_3_0_offset.h. (The `0xF702000042` / `0xE802000042` ids
  inside initGraphicsMQD are still NOT this form and stay undecoded.)
- **The object is `this->0x30`** - the same accessor 2ew measured at
  `0xffffffd39ba10000`, and the same one initGraphicsMQD's regions I and J
  use. So the suspect 2ew named is reached *earlier* than 2ew thought.
- **These are the GRAPHICS ring (RB0) registers.** The KIQ is a compute queue
  and works end to end; this is the first thing in this path to touch RB0,
  and `gfx_min = gfx_max = 0` has stood since 2di.

**What is NOT claimed:** that the GFX block is dead and reads stall. 2dl read
`CP_RB0_CNTL` straight through MMIO on this chip and got `0x00a00000`, so the
register is readable, and that measurement contradicts the easy story. What
has never been attempted is a **write**, which is what `0x120` / `0x128` /
`0x130` do.

## 2fb (2026-10-13): make the read, and bracket it

One virtual call, read-only, that Apple makes unconditionally a few
microseconds later on the same object with the same id - `vtable[0x148]` is
the register *read*. It runs in the `submitSetResourcesPacket` hook right
after gate 45, so the chip is in the state Apple's own call will see;
reading it at doStart entry instead would be the 2dp/2eh/2ek mistake of
measuring before the state exists. The accessor pointer is stashed at doStart
entry, where reading `pm4+0x30` has been safe since 2ce.

No new hardware write. Gates 46 and 47 bracket the call.

```
trail ...45 46 47   the accessor READ works, so the stop is in one of Apple's
                    WRITES (0x120/0x128/0x130) - the first write to the
                    graphics ring this project has ever caused
trail ...45 46      the read itself does not return, and the frontier is a
                    GC register read through Apple's accessor even though the
                    same register reads fine through direct MMIO (2dl)
trail ...45 only    our two diag sets and one log line still cost the thread,
                    despite 4 us measured - the hook layer, not its placement
```

Build: UUID 0BDB68D1, hosttest passes. Boot-args unchanged.

### 2fb answered on hardware (2026-10-13): NOTHING — the probe can exit without a gate

No `plugin_check` capture (the armed boot left no new log dir), so NVRAM is
the whole record. Build and arming checked first:

```
kmutil showloaded -> com.rembrandtgpu.plugin 0BDB68D1   == build/out (2fb)
rbgpu-diag        -> armed=0                            the recovery boot
rbgpu-diag-prev   -> d=23 armed=1                       the armed 2fb boot
nvram rbgpu-gate  -> "40 101 151 45 | 45 submitSetResourcesPacket OK"
nvram rbgpu-beat  -> "n=13 gate=45 skips=0 forced=0 t=18711491703"
```

**The trail is identical to 2fa and gate 46 is absent, and that absence is
uninterpretable** - because `rbgpu_probe_regread_2fb` returns silently when
the accessor, its vtable or `vtable[0x148]` reads NULL. So "no gate 46" is
equally well explained by:

- the thread stopping in the two diag sets and one log line before the probe,
  which 2fa measured at 4 us each; or
- the probe reaching its own null check and skipping without a word.

That is the ambiguity-of-absence this project has already hit at 2eh, 2ek and
2en, built in fresh by me one step after writing that the instrument was
finally trustworthy. The boot is spent and it answered nothing.

**A second, smaller regression, and it is 2fa's.** The prev summary carries
`d=23` with **no `ttl_hw_init_ret` and no `gfxmqd_*`**, where every boot
before 2fa had them. Rate-limiting the refresh to 250 ms means far fewer
flushes, so the summary now freezes earlier in the boot. The gate trail is
synchronous and unaffected, but anything that only lives in `rbgpu-diag` is
now less likely to survive. Values that matter must ride the trail.

So the `g_rb_2fb_regobj` stash could not be confirmed either: it is set inside
`if (pm4)` in the doStart hook (kern_start.cpp:3004), which the chain does
reach, but the key that would prove it was dropped.

## 2fc (2026-10-13): no silent exits, and put the value in the trail

Three changes to the probe, all instrument:

- **Gate 46 is written FIRST**, before any null check, and carries the
  accessor pointer in its text. The trail is synchronous and reliable where
  the diag summary no longer is.
- Each null check gets its own **gate 48** naming which pointer was NULL,
  instead of returning silently.
- The call itself is bracketed by **gate 49** (about to call) and **gate 47**
  (returned, with the value in the text).

Every path out of the probe now writes a gate, so the next trail is a
complete decision tree:

```
...45 46 49 47   the accessor READ works and returned a value; the stop is in
                 one of Apple's WRITES (0x120/0x128/0x130) - the first write
                 to the graphics ring this project would have caused
...45 46 49      the read itself does not return, even though 2dl read the
                 same register fine through direct MMIO
...45 46 48      the probe skipped, and the gate says which pointer was NULL
...45 only       the thread stops in the two diag sets and one log line
                 before the probe - the hook layer itself, at 4 us a call
```

Build: UUID 720E1371, hosttest passes. Boot-args unchanged.

### 2fc answered on hardware (2026-10-13): the trail endpoint still varies

logs/20261013-193242-plugin, `plugin_check` ran in the armed boot
(`armed=1`, `ttl_hw_init_ret=7dc79001`, a new boot), build checked:

```
kmutil showloaded -> com.rembrandtgpu.plugin 720E1371   == build/out (2fc)
nvram rbgpu-gate  -> "40 101 | 101 waitForHwStamp ENTER"
nvram rbgpu-beat  -> "n=17 gate=101 skips=0 forced=0 t=19380922540"
log.txt           -> 278 lines, last is the post-stamp-wait dump's final line
```

**The trail went backwards again, and 2fc changed nothing in that span.** Its
only edit is inside the probe, which runs *after* gate 45. So:

```
build   relevant code in the 45->151 span   trail reached
2fa     unchanged                            45
2fb     unchanged                            45
2fc     unchanged                            101
```

Three boots, identical code, three different endpoints. **The gate trail's
endpoint still varies**, which is 2ez's finding re-confirmed on a build where
the logging contention was supposed to be gone - and it means the endpoint
cannot be used to locate the thread even now.

Two things were checked before writing that, because the obvious story was
tempting and wrong:

- **The logging is slow in this boot, but not where it matters.** 157 of 277
  gaps exceed 1 ms, all in the channel-init phase from 13.5 s
  (`createEngine`, `registerChannel`, `addHWChannel`), with the gaps growing
  monotonically - 4.4, 5.9, 5.1, 6.1, 6.3, 6.5, 6.8, 7.0 ms. The
  post-stamp-wait tail runs at 3-10 us, exactly as 2fa measured.
- **It is not an O(n) cost in our log path.** `rbgpu_log_append_locked` is
  O(1) (it dedupes against the previous entry only) and
  `rbgpu_diag_set_num_locked` is an O(g_diagCount) scan of 32-byte keys -
  microseconds at 300 keys, not milliseconds. And one refresh per 250 ms
  cannot charge 5 ms to each of thirty lines in a 200 ms window. The gaps are
  between *our hook lines*, so they include Apple's own per-channel work.
- **Total elapsed logging is comparable**: 2fa 148 lines over 1861 ms, 2fc
  278 lines over 1640 ms. The thread is not slower in 2fc; the time sits in
  different places.

I was one step from blaming the 2fa rate limit, or an O(n) scan, on a single
boot each. Both were checked and neither holds.

## 2fd (2026-10-13): put the probe where the code always reaches

2fb and 2fc each spent a boot and answered nothing, for the same structural
reason: the probe sat behind gate 45, and whether gate 45 is reached varies
between boots. A measurement placed behind a varying gate inherits the
variance.

`rbgpu_probe_regread_2fb()` now runs from the **doStart hook**, immediately
after the accessor is stashed - the one site in this chain that has executed
in every boot since 2ce, twenty milestones ago.

2fb's objection to reading here was that the chip state would be wrong. That
applies to the WRITES; for a plain READ it does not, and the project's own
data says so: **2dl read `CP_RB0_CNTL` straight through MMIO at
`post-gc-hw-init`** - earlier than doStart - and got `0x00a00000`. The
register is readable before doStart, so a read through Apple's accessor there
is sound.

What the next trail says, and it no longer depends on reaching gate 45:

```
46 49 47 early in the trail   the accessor READ works and returned a value;
                              the stop is in one of Apple's WRITES
                              (0x120/0x128/0x130) at 0xbea12f2 onward
46 49 (then 40 ...)           the read itself does not return
46 48                         the probe skipped, and the gate names the NULL
no 46 at all                  the doStart hook's field-read block did not run,
                              which would be new and worth its own look
```

Build: UUID DF573C32, hosttest passes. Boot-args unchanged.

### 2fd answered on hardware (2026-10-13): the accessor read works — and it is NOT a read

logs/20261013-194149-plugin, `plugin_check` ran in the armed boot
(`armed=1`, `ttl_hw_init_ret=dcf5f001`, a new boot), build checked:

```
kmutil showloaded -> com.rembrandtgpu.plugin DF573C32   == build/out (2fd)
nvram rbgpu-gate  -> "46 49 47 40 101 | 101 waitForHwStamp ENTER"
nvram rbgpu-beat  -> "n=16 gate=101 skips=0 forced=0 t=19096530433"
log.txt           -> 462 lines, last is the post-stamp-wait dump's final line
```

Moving the probe to doStart entry worked: gates **46 49 47** land first in the
trail, before gate 40, so the measurement no longer depends on reaching a
varying gate. 2fd's first decision branch fired - the call returned a value.

**And the value settles something bigger than the question asked.**

```
2fb: CP_RB0_CNTL via Apple's accessor = 0x00003041
```

That is not a register's contents:

```
seg0 (proven in 2dl)            0x1260
mmCP_RB0_CNTL (gc_10_3_0)       0x1DE1
                                ------
                                0x3041   exact
```

So **`vtable[0x148]` resolves an encoded register id to a dword offset** - it
does not read the register. 2dl read the same register's contents through
direct MMIO and got `0x00a00000`; the two numbers were never measuring the
same thing. A 14-bit sum matching exactly, from two independent sources
(Apple's own accessor, and gc_10_3_0_offset.h plus the seg0 proven in 2dl), is
not the small-integer coincidence 2dw's page scan was.

Two consequences:

- **seg0 = 0x1260 is confirmed a second, independent way.** Apple's accessor
  computes the same base this project measured in 2dl.
- **The 2fa reading of the doStart span is corrected.** Those five
  `[0x148]` -> `[0x12x]` pairs are not read-modify-writes: `[0x148]` resolves
  the id and `[0x120]`/`[0x128]`/`[0x130]` take the resolved offset. So the
  span contains **no GC register read at all** - it is five writes, and the
  project has still never caused a write to the graphics ring.

A reading of what the three write slots are, consistent with amdgpu's
`gfx_v10_0_cp_gfx_resume` and **not** verified by disassembling them: set
`RB_RPTR_WR_ENA` (bit 31, the `0x80000000` at 0xbea12f2) in CP_RB0_CNTL,
write CP_RB_RPTR_WR / CP_RB0_WPTR / CP_RB0_WPTR_HI, then clear the bit again -
so `[0x128]` = set bits, `[0x120]` = write, `[0x130]` = clear bits. Recorded
as a reading, not a finding.

**The endpoint still varies, and the beat says the thread really does stop.**
The log shows the stamp arriving (`the REAL wait returned 1`) and the whole
`post-stamp-wait` dump completing at 15.338954; gate 151 is written after that
dump and never committed. `rbgpu_gate_nvram` sets `g_rbgpuLastGate` before its
write, and the last beat - 3.8 s later, `skips=0 forced=0` - still reads
`gate=101`. So the thread did not reach gate 151's call, while 2fa went
through that whole span and reached 45.

```
build   trail endpoint
2fa     45
2fb     45
2fc     101
2fd     101
```

## 2fe (2026-10-13): take the only hardware access out of the varying span

With the 2fa logging contention measured gone (this boot's tail gaps are
0.002-0.008 ms), the span from gate 101 to gate 45 contains exactly one thing
that touches the chip: `rbgpu_dump_gc_regs`, at `at-stamp-wait` and
`post-stamp-wait`. It reads ~40 GC registers and writes `GRBM_GFX_CNTL`. **A
GC/SMN read that stalls is a documented non-deterministic hazard on this
chip** - it is what the boot 6/7 RSMU timeout events were - which makes it the
only remaining candidate for an endpoint that varies on identical code.

So the three dump points after `post-startKIQ` are retired unless
**`-rbgpuregdump`** is set, and each logs that it skipped rather than going
silent. Every question they were added for is answered and recorded:

```
at-stamp-wait    the GPUVM fault that used to latch here is gone (2em)
post-stamp-wait  the queue is live, RPTR=WPTR=0x20 (2dq/2do)
pre-kiq-submit   the fault latch is clean before the doorbell (2ea)
```

`post-gc-hw-init` and `post-startKIQ` **stay**, because the 2em PT_BASE write
lives inside the dump at `post-startKIQ` and the whole frontier depends on it.

Build: UUID 477D5F0B, hosttest passes. Boot-args unchanged.

What the next armed boot decides, from `nvram rbgpu-gate`:

```
46 49 47 40 101 151 45 50 54 ...   the register dump was the stall; the
                                   frontier moves to Apple's initGraphicsMQD
                                   with nothing of ours left in the span
endpoint still varies              the dump is not it either, and what is left
                                   in the span is Apple's own code plus our
                                   diag/log calls at 4 us each
```

### 2fe answered on hardware (2026-10-13): THE DUMP WAS THE STALL — initGraphicsMQD RETURNS

No log dir from the armed boot, so NVRAM is the whole record. Build and arming
checked first, per the 2cu/2cv rule:

```
kmutil showloaded -> com.rembrandtgpu.plugin 477D5F0B   == build/out (2fe)
rbgpu-diag        -> d=0 armed=0                        the recovery boot
rbgpu-diag-prev   -> d=27 armed=1 ttl_hw_init_ret=dcead001
                     (2fd read dcf5f001, so this IS a new boot)
nvram rbgpu-gate  -> "46 49 47 40 101 151 45 50 54 55 60 102 152 70 65 103 153 75
                      | 75 submitMapQueuesPacket OK"
nvram rbgpu-beat  -> "n=14 gate=75 skips=0 forced=0 t=18085392836"
```

Decoded against the numbering (ENTER `60+ring`, OK `70+ring`, FAIL `65+ring`):

```
46 49 47        the accessor probe at doStart entry
40 101 151 45   submitSetResourcesPacket + stamp #1 -> OK
50 54 55        initGraphicsMQD ENTER -> the ORIGINAL -> **RETURNED OK**
60 102 152 70   submitMapQueuesPacket ring 0 + stamp #2 -> OK
65 103 153 75   submitMapQueuesPacket ring 5 + stamp #3 -> OK
```

**2fe's first decision branch, and it is the largest single advance in this
milestone family.** Three things that eight boots failed to establish:

- **`initGraphicsMQD` RETURNS** - gate 55, never once seen before. So 2eu's
  "the hang is established to be in Apple's `initGraphicsMQD`" was the
  **register dump**, not Apple's code. The 4 KB `bzero` at 0xbea25ea, the
  allocation chain A-F and the regions H-J are all cleared in one boot.
- **Apple's five RB0 writes completed.** `0x120`/`0x128`/`0x130` on
  `pm4+0x30` at 0xbea12f2-0xbea1396 are the first writes to the **graphics**
  ring in this project, and they did not stall. The standing
  `gfx_min = gfx_max = 0` suspicion (2di onward) did **not** bite - GFX DPM
  never starting does not stop the ring registers being programmed.
- **Two `submitMapQueuesPacket` calls succeeded, each with a real GPU stamp.**
  Stamp waits #2 and #3 both returned TRUE, so the KIQ is executing
  MAP_QUEUES packets and writing fences for them, not just SET_RESOURCES.

What is NOT claimed: that one boot settles the variance. This is a single
sample, and reading one as a rule is this project's recurring error. What
makes it supported rather than inferred is that the jump is qualitative
(45, 45, 101, 101 -> eighteen gates) and the mechanism was named in a written
prediction *before* the boot: the register dump was the only hardware access
left in the varying span, and GC/SMN reads are a documented non-deterministic
stall on this chip.

`doStart`'s `+0x348` is 5, so **three mapQueues calls remain**, then the
ring-state check, `waitForActiveValue` (bounded by 2cp) and
`(engine+0xc0)->vtable[0x328]`.

**A defect in my own numbering, found while decoding.** The fail band
`65+ring` collides with the enter band `60+ring` for ring >= 5 - and ring 5
is real, measured in this very trail - so `65` means both "ring 0 FAILED" and
"ring 5 ENTER". The ordering disambiguates it here (ring 0 had already
committed 70, and a FAIL cannot follow an OK within one call), but a record
should not need that.

## 2ff (2026-10-13): the gate last, and bands that cannot collide

- In the mapQueues hook the `rbgpu_log` moves **before** the exit gate, so the
  gate is the last statement before `return ret` and the span from it to the
  next ENTER gate holds nothing of ours. That is the move that landed gate 151
  (2ez) and gate 54 (2eu), and the 2fe trail stopped immediately after gate 75
  with that one log line the only thing of ours left in the span.
- The same rule applied to the `initGraphicsMQD` hook's gate 55.
- Bands are now disjoint: enter `60+(ring&7)` = 60..67, OK `70+(ring&7)` =
  70..77, fail `80+(ring&7)` = 80..87, clear of the `100+call` / `150+call`
  stamp bands. The 2fe trail's own values (65 enter, 75 OK for ring 5) are
  unchanged, so it stays readable against the new encoding.

The two diag sets stay *before* the gate: the thread traversed them this boot
(gate 152 -> gate 75), so they are measured survivable, and moving them after
the gate would put them into the span that matters.

Build: UUID 8B7407F6, hosttest passes. Boot-args unchanged, `-rbgpuregdump`
stays off.

What the next armed boot decides, from `nvram rbgpu-gate`:

```
three more 6x/1xx/7x triples   all five mapQueues done, and the frontier is
                               the ring-state check / waitForActiveValue /
                               vtable[0x328] at the end of doStart
stops after a 7x again         the stop is inside Apple's next mapQueues call,
                               with nothing of ours in the span - a hard fact
                               and a new place to disassemble
an 8x value                    a mapQueues call FAILED rather than hung, which
                               is a clean doStart failure and a desktop
```

### 2ff answered on hardware (2026-10-13): the trail went backwards again

Recovery boot; build checked first (`kmutil showloaded` 8B7407F6 == build/out),
`rbgpu-diag` d=0 armed=0.

```
nvram rbgpu-gate -> "46 49 47 40 101 151 45 50 54 55 60 102 152
                     | 152 waitForHwStamp RETURNED TRUE"
nvram rbgpu-beat -> "n=17 gate=152 skips=0 forced=0 t=18751827333"
```

Thirteen gates against 2fe's eighteen: stamp #2 returned TRUE and gate 70
never committed, so this boot did not finish even the FIRST mapQueues call.

And 2ff's own reasoning was wrong on its own terms. Moving the log before the
exit gate put **more** of our code into the `152 -> 70` span, not less - the
same total, redistributed. The 2eu/2ez precedent it was reasoning from is
itself now suspect: 2ez explained the variance as logging contention, 2fa
measured that contention gone, and the variance stayed.

### The constant nobody had looked at: the heartbeat does not survive its boot

`rbgpu_beat_action` re-arms itself with `thread_call_enter_delayed` **every
250 ms, unconditionally**, for as long as `g_rbgpuBeatOn`. So in the boots
where `plugin_check` ran *in the armed boot* and the machine demonstrably
lived for minutes - 2ey, 2fa, 2fc, 2fd - `n` should read in the hundreds.

```
2ez n=27   2fa n=28   2fb n=13   2fc n=17   2fd n=16   2fe n=14   2ff n=17
```

**It reads 13-28 in every boot**, across trail endpoints from 101 to 75. The
beat stops a few seconds after gate 40 while the machine goes on living, and
its loop contains exactly one thing that can block: its own `setProperty` +
`IONVRAM-FORCESYNCNOW-PROPERTY`.

So **synchronous NVRAM flash commits stop working partway through the boot** -
and the gate trail uses that same mechanism. That is a candidate cause for the
varying endpoint which is **not** the thread: the trail may stop where the
WRITES stop, not where the thread stops.

What this does and does not touch:

- 2ey's branch 1 **stands**: the machine is alive and the start() thread holds
  the IOService lock (`pci.txt` 0 bytes). The thread is genuinely stuck or
  slow. What is now open is whether the trail's ENDPOINT locates it.
- `skips=0 forced=0` does not help here. It says the beat never found the gate
  path holding the flag; it says nothing about whether either write landed.
- It would also explain 2fe without the register dump being a stall at all:
  from gate 40 the quiesce means the only NVRAM writers are the gates and the
  beat, so anything that changes their relative throughput changes how much
  trail gets recorded. 2fe's finding that `initGraphicsMQD` RETURNS is **not**
  affected - gates 55, 70 and 75 committed, and a committed gate is positive
  evidence whatever happens to later ones.

Stated as a hypothesis, not a finding. A seven-row table of one boot each is
exactly the shape that produced the withdrawn 2ev table and the 2ez
correction; the per-boot totals (gates + beats = 17..32) are suggestive and
not tight, and 2fb's 4+13 breaks any clean budget.

## 2fg (2026-10-13): take the other writer out, and make the gate say if it landed

- The heartbeat is **off unless `-rbgpubeat`** is set. Its question is
  answered, it costs four flash commits a second, and from gate 40 it is the
  only NVRAM writer besides the gate trail. Removing it roughly doubles what
  is left for the gates and tests the hypothesis directly.
- `rbgpu_gate_nvram` now counts its writes and any the registry refuses, and
  carries both in the text: `[w=N f=M]`.

```
trail runs much further        writes were the constraint, and every endpoint
                              from 2fa onward was the record running out
same endpoints, w climbing,   the writes are landing and the thread really is
f=0                           what stops - so the stop is in Apple's next
                              mapQueues and that is a place to disassemble
f nonzero                     the registry refused a write, which names the
                              failure outright
```

Also fixed: the 2fe section left a code fence unterminated in this file.

Build: UUID 9ACF72A0, hosttest passes. Boot-args unchanged (`-rbgpubeat` is
new and deliberately NOT added; `-rbgpuregdump` stays off).

### 2fg answered on hardware (2026-10-13): the write-budget hypothesis is REFUTED

`plugin_check` ran in the armed boot (`rbgpu-diag` armed=1,
`ttl_hw_init_ret=c1b6b001`, a new boot), though `log.txt` came back empty.
Build checked: 9ACF72A0 == build/out.

```
nvram rbgpu-gate -> "46 49 47 40 101 151 45 50 54 55 60 102 152 70 65 103 153 75
                     | 75 submitMapQueuesPacket OK [w=18 f=0]"
nvram rbgpu-beat -> stale from the 2ff boot (the beat is off, as intended)
```

**Eighteen writes, zero refused, and the trail stops at exactly the gate 2fe
stopped at.** Removing the heartbeat took ~17 competing flash commits out of
the boot and bought nothing, so the hypothesis that the trail stops where the
WRITES stop does not hold. 2fg's second branch: the writes land and the thread
is what stops.

And **gate 75 is the first reproducible endpoint this milestone family has
had**: 2fe 75, 2ff 152, 2fg 75. Two of three boots agree, where the previous
twelve agreed on nothing.

### What is actually after gate 75 (doStart 0xbea1416-0xbea14f7, disassembled)

```
bea1416  testb $1,%r14b ; je bea14cd       skip it all if any mapQueues failed
bea1420  (this->0x18)->vtable[0x3b8]()      -> this->0x340
bea143b  [0x148](0xF60..0B   CP_RB0_RPTR)    -> bea1456 [0x118](off)
bea1469  [0x148](0x1DF5..0B CP_RB0_WPTR_HI) -> bea1481 [0x118](off)
bea1495  [0x148](0x1DF4..0B CP_RB0_WPTR)    -> bea14ae [0x118](off)
bea14c5  orl 0x54(%rcx),%r13d                channel+0x54
bea14d3  waitForActiveValue(0, 1, 0)
bea14f7  (this->0xc0)->vtable[0x328]()
```

That is exactly the ring-state check the 2cl correction predicted from the
branch structure - `CP_RB0_RPTR` / `CP_RB0_WPTR` / `CP_RB0_WPTR_HI` and
`channel+0x54` all zero - now read instruction by instruction.

**The accessor's slots are settled: `0x148` resolves an id to a dword offset,
`0x118` READS that offset, `0x120` WRITES it.** `[0x118]` takes only the
offset and its return value is used (`movl %eax,%r13d`, `orl %r15d,%eax`);
`[0x120]` takes offset + value (the GRBM_GFX_CNTL selector write inside
waitForActiveValue at 0xbea15bf proves the shape). So 2fd's "the span contains
no GC register read at all" was right about the five RB0 pairs and the read
slot it could not name is 0x118 - and these three are **the first GC register
READS Apple makes in this path**, the same class of access 2fe measured
stalling non-deterministically.

### waitForActiveValue is an unbounded spin, and its escape cannot happen here

`waitForActiveValue(_eAMD_HW_RING_TYPE ring, bool b1, bool b2)`, called from
doStart with `(0, 1, 0)`. With `ring == 0` it selects
`0x1E80..0B` = **CP_GFX_HQD_ACTIVE** (0xbea1589) and polls:

```
bea15c5 loop:
  [0x148](0xDC2..0B GRBM_GFX_CNTL) -> [0x120](off, selector)   write the window
  [0x148](CP_GFX_HQD_ACTIVE)       -> [0x118](off) -> eax      READ
  if (eax == -1)            -> return false                    the only escape
  r14b = (uint8)eax ^ b1
  al = r14b & b2            ; b2 = 0  ->  al = 0
  if (al != 1)              -> bea1611   ALWAYS: skips the 0x3e7 counter
                                         AND the IODelay(0x64)
bea1611:
  if (r14b & 1)             -> loop      spin while active == 0
```

**With `b2 = 0` the 1000-iteration cap and the 100 us delay are both dead
code**, so this is a tight, delay-free, unbounded spin whose only exit is the
register reading `0xFFFFFFFF`. And nothing can end it on this chip: 2ew
measured `pm4+0x10 = 0`, so Apple's command-9 graphics-queue create at
0xbea26a1 never runs, so the graphics HQD is never made active (2dm/2do read
`CP_GFX_HQD_ACTIVE = 0` at every point).

That matches every symptom this family has produced - the thread never
returns, the machine stays alive, `ioreg` blocks on the IGPU subtree,
`pci.txt` is 0 bytes, no later gate is ever written.

**But 2cp should already prevent it.** `hooked_pm4_waitForActiveValue_2cp`
forces the 4th C++ parameter (`bounded`) true, and that parameter IS `ecx` =
`b2` = `-0x34(%rbp)` - the one that gates the counter. So either 2cp is not
reached, or the stop is earlier.

## 2fh (2026-10-13): one bit, to split the span after gate 75

Two gates in the existing `waitForActiveValue` hook - **90** at entry (carrying
`ring`, `expected` and the `bounded` value, since the diag table is quiesced
from gate 40 and cannot report `wfav_*` any more) and **95** after the
original returns, placed last before the `return` per the 2ff rule. Bands
90/95 are clear of 60..67 / 70..77 / 80..87 / 100+ / 150+.

Nothing else changes. In particular **no new hardware access**: knowing that
`[0x118]` is the read slot makes a real `CP_RB0_CNTL` read through Apple's
accessor possible at last, but adding one in the same boot would put a second
variable into a question that is finally down to one bit.

```
...75 90 95       the ring-state reads completed AND waitForActiveValue
                  returned; the stop moved to vtable[0x328] at 0xbea14f7
...75 90          the thread is inside waitForActiveValue - and gate 90's text
                  says whether 2cp forced bounded=1, so it says in the same
                  breath whether the cap was restored and still hung, or 2cp
                  never fired and this is the unbounded spin
...75 only        the stop is in Apple's three GC register READS at
                  0xbea143b-0xbea14ae, with nothing of ours in the span - a
                  hard fact, and the same class of access 2fe showed stalling
```

Build: UUID 3A13C496, hosttest passes. Boot-args unchanged.

### 2fh answered on hardware (2026-10-13): inside waitForActiveValue, WITH the cap on

Recovery boot; build checked first (3A13C496 == build/out), `rbgpu-diag`
d=0 armed=0. logs/20261013-201615-plugin.

```
nvram rbgpu-gate -> "46 49 47 40 101 151 45 50 54 55 60 102 152 70 65 103 153 75 90
                     | 90 waitForActiveValue ENTER ring=0 exp=1 bnd=1 [w=19 f=0]"
```

Gate 90 landed, gate 95 did not, and three things are settled at once:

- **Apple's three GC register READS completed.** The span from gate 75 to
  gate 90 is `vtable[0x3b8]` plus the ring-state check - `CP_RB0_RPTR`,
  `CP_RB0_WPTR_HI`, `CP_RB0_WPTR` resolved with `[0x148]` and read with
  `[0x118]` - so reads through Apple's own accessor work on this chip. The
  2fg suspicion that those three were the stall is **closed**.
- **2cp fired.** `bnd=1` in gate 90's own text, so `bounded` was forced true
  and both the `cmpl $0x3e7` counter (0xbea15fe) and the delay are live. The
  delay resolves with kc_stubs.py to **`_IODelay(0x64)`**, a 100 us
  busy-wait, so the capped poll owes an answer in about 100 ms.
- **And it still does not return.** A bounded 1000 x 100 us loop cannot
  explain that. What can is a single stalled access inside it - and each
  iteration makes two, a `GRBM_GFX_CNTL` **write** through `[0x120]` and a
  `CP_GFX_HQD_ACTIVE` **read** through `[0x118]`, so 1000 iterations is 2000
  GC register accesses.

Also worth recording: `[w=19 f=0]`, so the write accounting 2fg added holds
for a nineteenth consecutive landing with nothing refused. The trail is not
the thing that is failing.

## 2fi (2026-10-13): run Apple's poll ourselves, with gates inside it

Nothing can be instrumented inside Apple's function, so the poll is
reproduced in the hook - instruction for instruction from the disassembly
already recorded here, with only the iteration count changed:

```
resolve GRBM_GFX_CNTL   [0x148](0xDC2..0B)      -> gate 91, with the offset
write selector 0        [0x120](off, 0)         -> gate 92
resolve CP_GFX_HQD_ACTIVE [0x148](0x1E80..0B)   -> gate 93, with the offset
read it                 [0x118](off)            -> gate 94, with the VALUE
loop while (v ^ expected) & 1, max 64, exit on 0xffffffff
                                                -> gate 96, iters + last + ret
```

**This is the 2en pattern, not 2cy's.** 2cy substituted its own value and then
narrated it as a hardware result, which cost fifteen milestones. This one
reproduces Apple's own loop and only shortens the count, and when the ring
does not go active it returns false into **Apple's own failure path** - which
is the honest outcome here, because 2ew's `f10 = 0` means the command-9
graphics-queue create never runs, so `CP_GFX_HQD_ACTIVE` cannot go active and
false is what Apple's capped loop would itself return after 100 ms. The log
says in those words that the false is ours and not a hardware verdict.

Every exit writes a gate: **98** if a slot of `pm4+0x30` is NULL (and says the
false is an instrument failure, not a result), **99** if `-rbgpuwfavraw` or a
non-zero ring sends it to Apple's original instead.

`-rbgpuwfavraw` restores Apple's version, mirroring `-rbgpustampraw`.

```
...90 91 92 93 94 96 95   the whole poll runs; the stop moved past
                          waitForActiveValue to vtable[0x328] at 0xbea14f7,
                          and gate 96 says what the HQD actually read
...90 91 92               the GRBM_GFX_CNTL WRITE is where it stalls - the
                          first time a GC register write would be named
...90 91 92 93 94 (stop)  the first HQD read returned and the loop stalls on
                          a later iteration, so it is cumulative, not one
                          access
...90 91 (stop)           the resolve is fine and the write never returns
...90 98 / ...90 99       neither - the gate names which
```

A boot that reaches gate 96 with `ret=0` should then fail `doStart` cleanly
and come up to a desktop, which no boot has done since 2em.

Build: UUID 4E5792D6, hosttest passes. Boot-args unchanged.

### 2fi answered on hardware (2026-10-13): THE GRAPHICS HQD IS ACTIVE — and my own prediction was refuted

logs/20261013-202502-plugin, `plugin_check` ran **in the armed boot**
(`rbgpu-diag` armed=1, `ttl_hw_init_ret=c553a001`, a new boot), 486 log lines,
build checked first per the 2cu/2cv rule:

```
kmutil showloaded -> com.rembrandtgpu.plugin 4E5792D6   == build/out (2fi)
nvram rbgpu-gate  -> "46 49 47 40 101 151 45 50 54 55 60 102 152 70 65 103 153
                      75 90 91 92 93 94 96
                      | 96 poll done iters=1 last=0x00000001 dead=0 ret=1 [w=24 f=0]"
log.txt last line -> the 2fi poll line, at 15.347
pci.txt           -> 0 bytes
```

**The whole poll ran, and the value is the finding:**

```
gate 91  GRBM_GFX_CNTL      resolved to dword 0x2022
gate 92  selector 0 WRITTEN
gate 93  CP_GFX_HQD_ACTIVE  resolved to dword 0x30e0
gate 94  first read = 0x00000001
gate 96  iters=1 last=0x00000001 dead=0 ret=1
```

- **`CP_GFX_HQD_ACTIVE` reads 1.** The graphics HQD is **active**. `expected`
  is 1, so Apple's own loop would exit on its first poll and return true: the
  unbounded-spin hazard that 2cp has been guarding since 2026-10-12 does not
  bite on this chip in this state.
- **2fi's own prediction is REFUTED, and it was mine.** I wrote that a false
  was the expected outcome "because 2ew's `f10 = 0` means the command-9
  graphics-queue create never runs, so `CP_GFX_HQD_ACTIVE` cannot go active".
  The chip says it went active anyway. `f10 = 0` stands as a measurement; the
  inference drawn from it does not. What makes a graphics queue active here is
  the **MAP_QUEUES** packets - five `submitMapQueuesPacket` calls, two of them
  confirmed in this very trail with real GPU stamps (gates 70 and 75) - not
  Apple's command-9 create. The log line now says so in those words.
- **Both resolutions confirm seg0 a third and fourth time.** `0x1260 + 0x0DC2
  = 0x2022` and `0x1260 + 0x1E80 = 0x30E0`, exact, against
  docs/linux-ref/gc_10_3_0_offset.h. Four independent agreements on seg0 now
  (2dl MMIO, 2fd CP_RB0_CNTL, and these two).
- **Gate 92 is the first GC register WRITE this project has gated end to end,
  and it returned.** A write through Apple's `[0x120]` to `GRBM_GFX_CNTL`
  completed, and the read through `[0x118]` that followed it completed too.
  So neither slot is the stall, and `[w=24 f=0]` is a twenty-fourth
  consecutive landing with nothing refused.

**Where it stops: gate 96 committed, gate 95 did not**, and the span between
them was entirely ours - three `rbgpu_diag_set_num` calls, one `rbgpu_log`,
one `snprintf`. The 2fi log line is the log's last entry and the `2cp:` line
after it never appears, which is consistent with the thread stopping in those
diag sets; per the 2es withdrawal the published log ends at the last refresh,
so that is corroboration and not a location.

## 2fj (2026-10-13): empty the 96 -> 95 span, and gate doStart's own exit

- **Gate 95 is now the first statement after the poll.** The three `wfav_*`
  diag sets and the `2cp:` log line are deleted outright: `rbgpu_nvram_quiesce()`
  fires at gate 40, so nothing written to the diag table after it can reach
  NVRAM, and gate 95's text already carries the return value. Emptying a span
  is the only move that has ever advanced this frontier - 2eu at 50->54,
  2ez at 151->45, 2fe at the register dump.
- **Gates 30 / 31 at `doStart`'s return**, so the trail states whether the
  whole chain completed instead of leaving it to be inferred from the last
  inner gate. The band is clear of 40/45, 46-49, 50/54/55, 60-67/70-77/80-87,
  90-96/98/99 and 100+/150+.

What is left after gate 95 is Apple's `(engine+0xc0)->vtable[0x328]` at
0xbea14f7 - **the last call in `doStart`**.

Build: UUID 1FEC6BCA, hosttest passes. Boot-args unchanged.

```
...96 95 30     doStart RETURNED TRUE. powerUp succeeds, powerUpHWEngines
                goes on to the next engine, and the 2cn survive guard is no
                longer the path - the first time this project would have a
                PM4 engine report success
...96 95 31     doStart returned FALSE at vtable[0x328], which is a clean
                failure and a desktop, and names that call as the next thing
                to disassemble
...96 95        the stop is inside vtable[0x328] itself, with nothing of ours
                in the span - a hard fact and a new place to look
...96 only      the span was not the diag sets after all, and what remains
                between the poll and gate 95 is one assignment
```

### 2fj answered on hardware (2026-10-13): no — the trail went backwards to 152

Recovery boot; build and arming checked first per the 2cu/2cv rule.

```
kmutil showloaded -> com.rembrandtgpu.plugin 1FEC6BCA   == build/out (2fj)
rbgpu-diag        -> d=0 armed=0                        the recovery boot
rbgpu-diag-prev   -> d=27 armed=1 ttl_hw_init_ret=cd22c001
                     (2fi read c553a001, so this IS a new boot)
nvram rbgpu-gate  -> "46 49 47 40 101 151 45 50 54 55 60 102 152
                      | 152 waitForHwStamp RETURNED TRUE [w=13 f=0]"
```

Thirteen gates against 2fi's twenty-four, and **the trail is byte-identical to
2ff's**: stamp #2 returned TRUE and gate 70 never committed, so this boot did
not finish even the first `submitMapQueuesPacket`.

**2fj changed nothing in that span.** Its only edits are in the
`waitForActiveValue` hook (which runs after gate 75) and at `doStart`'s
return. So this is 2fc again: a build whose relevant code is unchanged,
landing on a different endpoint.

```
build   edits in the 152 -> 70 span   trail endpoint   gates
2fe     -                             75               18
2ff     log moved before the gate     152              13
2fg     -                             75               18
2fh     -                             90               19
2fi     -                             96               24
2fj     -                             152              13
```

Six boots, four of them with no change at all in that span, four different
endpoints. **The endpoint still varies**, and every structural explanation
offered for it has now been refuted on hardware: logging contention (2ez,
fixed in 2fa, variance stayed), the NVRAM write budget (2ff, refuted by 2fg
at `w=18 f=0`), and the register dump (2fe, which helped once and then did
not).

## 2fk (2026-10-13): time the trail, because nothing ever has

The one quantity never measured in this family is **how long a gate costs**.
Each one does a `setProperty` plus `IONVRAM-FORCESYNCNOW-PROPERTY` - a real
flash commit inside `IOService::start` - and no boot has ever clocked one.

2ez found exactly this shape once already, in the log rather than the trail:
the same code running at 3 us and at 10 ms per line within one boot, which is
indistinguishable from a wedge and stops wherever the power cycle lands. That
diagnosis is what made 2fa work, and the possibility that the gate trail is
now the slow instrument has never been tested. It also fits what nothing else
explains: the heartbeat dying at `n=13..28` in every boot while the machine
demonstrably lives for minutes (2ff), since the beat competes for the same
NVRAM controller.

`rbgpu_gate_nvram` now carries two numbers in its tail text:

```
t   ms since the FIRST gate of the boot
wx  the worst single gate-to-gate gap in ms
```

No new mechanism and no new hardware access - two statics and one
`mach_absolute_time()` per gate.

Build: UUID 7C93413F, hosttest passes. Boot-args unchanged.

```
t tens of ms at the last gate      the thread genuinely stops, the endpoint
                                   DOES locate it, and the next question is
                                   what is in that span
t already seconds, wx in the       the gates are the crawl: the endpoint is
hundreds                           just where the power cycle landed, every
                                   endpoint reading from 2fa onward is void,
                                   and the breadcrumb needs a cheaper channel
```

One thing only the user can supply, and it matters as much as the numbers:
**how long each armed boot sat before it was power-cycled.** If the two
13-gate boots were cut short and the 24-gate boot was left longer, that is
the second branch without needing another build.

### 2fk answered on hardware (2026-10-14): REFUTED by its own number — a gate costs 6 us

logs/20261014-101920-plugin, `plugin_check` ran **in the armed boot**
(`ttl_hw_init_ret=cfda4001`, a new boot), 58 KB of log, `pci.txt` 0 bytes.
Build checked: 7C93413F == build/out.

```
nvram rbgpu-gate -> "46 49 47 40 101 151 45 50 54 55 60 102 152
                     | 152 waitForHwStamp RETURNED TRUE [w=13 f=0 t=380 wx=367]"
```

**The instrument verifies against the log to the millisecond** - gate 46 at
13.968946, gate 152 at 14.349315, difference 380.4 ms, and the trail says
`t=380`. So the timing is trustworthy, and per-gate it says:

```
gate    abs s        gap
  46  13.968946
  49  13.975077    6.131 ms     <- BEFORE the quiesce
  47  13.981186    6.109 ms     <- BEFORE the quiesce
  40  14.348836  367.650 ms     <- wx: initComputeMQD + startKIQ + the
                                   post-startKIQ dump + the 2em PT_BASE write
 101  14.348851    0.015 ms
 151  14.348951    0.100 ms
  45  14.348957    0.006 ms
  50  14.348982    0.025 ms
  54  14.348990    0.008 ms
  55  14.349213    0.223 ms     <- Apple's initGraphicsMQD, timed at last
  60  14.349219    0.006 ms
 102  14.349226    0.007 ms
 152  14.349315    0.089 ms
```

- **A gate costs ~6 US after `rbgpu_nvram_quiesce()` and ~6 MS before it.** The
  three gates ahead of gate 40 are 6 ms apart; the nine after it, Apple's own
  work included, span **479 us in total**. A thousandfold difference with the
  quiesce as the only variable. So 2fk's hypothesis - the gates are the crawl -
  is **refuted by the very number it added**, and `wx=367` is not a gate cost
  at all but the real work between the accessor probe and the first gate of the
  chain.
- **Apple's `initGraphicsMQD` takes 223 us** (gate 54 -> 55). First time it has
  been timed, and 2fe's "it RETURNS" now has a duration beside it.
- **`t` is 380 ms, not seconds**, so branch one: the thread genuinely stops and
  the endpoint does locate it.

**And that reframes the variance into something new.** The whole frontier -
gate 40 to gate 152 - is **479 us wide**, traversed at 6 us a gate. The
endpoints this family has collected (152 at 13 gates, 96 at 24 gates) are a
few hundred **microseconds** of thread progress apart. Deterministic code
cannot stop at positions that vary inside a 500 us window; something
asynchronous arriving at an unpredictable moment can, and the endpoint then
records where the thread happened to be. That is consistent with 2ey's
branch 1 rather than against it - the machine is alive, one thread is stuck -
and it is the first explanation of the variance that the measurements support
rather than contradict.

Since 2em there is exactly one asynchronous thing on this chip for the first
time: the GPU reaches host memory and the fences land (`fence_val_after=2`
this boot, and the wait returned 1). An interrupt or the SW scheduler touching
the same objects is a candidate; **it is named here as the leading reading,
not a finding** - nothing yet measures it.

## 2fl (2026-10-14): dense gates, because they are now measured to be cheap

The reversal of 2ff and 2fj is a measurement, not a change of mind. Those
steps emptied spans on the belief that our code in them cost the thread its
progress; 2fk measures that cost at **6 us a gate**, so emptying the
152 -> 70 span was saving microseconds out of 89.

So the span gets gates at every boundary it has, which no span in this family
has ever had:

```
152  waitForHwStamp RETURNED TRUE      (existing)
 32  mapQueues: the ORIGINAL returned  <- new
 33  the two diag sets returned        <- new
 70  submitMapQueuesPacket OK          (existing, one log line before it)
```

Three more gates, ~18 us added to an 89 us span. Band 32/33 is clear of
30/31 (doStart), 40/45, 46-49, 50/54/55, 60-67/70-77/80-87, 90-96/98/99 and
100+/150+.

Also corrected: the 2dw fence line still printed "the failure is in the
CPU-side pickup, not in the GPU" whenever the fence HAD landed. That was true
while the fence never arrived and has been false since 2em, where the wait
succeeds on its first poll. It now says the landed fence is the working path.

Build: UUID 5CC58C07, hosttest passes. Boot-args unchanged.

```
...152 32 33 70 ...   the span is clear and the frontier moves on; with
                      t per gate, two such boots say whether the stop is a
                      code position at all
...152 32 33          the stop is the one log line left in the span
...152 32             the two diag sets
...152 (stop)         Apple's own submitKIQFrame epilogue, with nothing of
                      ours in the span - and 2fi traversed that same epilogue
                      twice, so it would be a race rather than a position
```

`t` on the last gate is now the discriminator that matters more than the
endpoint: if boots that stop at different gates all report `t` near 380 ms,
the stop is a wall-clock moment - a watchdog or an asynchronous arrival - and
not a place in the code at all.

### 2fl answered on hardware (2026-10-14): gate 102 — and `t` is NOT a fixed moment

No log dir from the armed boot, so NVRAM is the whole record. Build checked:
5CC58C07 == build/out (2fl), `rbgpu-diag` d=0 armed=0 (the recovery boot).

```
nvram rbgpu-gate -> "46 49 47 40 101 151 45 50 54 55 60 102
                     | 102 waitForHwStamp ENTER [w=12 f=0 t=171 wx=171]"
```

Twelve gates, one EARLIER than 2fj/2ff: the second `waitForHwStamp` was
entered and gate 152 never committed. So 2fl's new gates 32 and 33 never had
a chance - they sit after 152 - and the question they were added for is still
open.

**`t = wx = 171`**, so one gap is the entire elapsed time and every other gate
rounds to zero. That one gap is 47 -> 40 again, as in 2fk; it was 367 ms there
and 171 ms here, so even Apple's own work in that span (initComputeMQD,
startKIQ, the post-startKIQ dump, the 2em PT_BASE write) varies more than
twofold boot to boot. Everything from gate 40 onward is under a millisecond,
confirming 2fk's 479 us a second time.

**And `t` answers the question 2fk posed.** 2fk predicted that if boots
stopping at different gates all reported `t` near 380 ms, the stop would be a
wall-clock moment - a watchdog or a timeout - rather than a code position.
They do not: 380 ms at gate 152, 171 ms at gate 102. **The watchdog reading is
refuted.**

### The commit-lag hypothesis is refuted too, by a record already in hand

Before writing a seventh explanation I checked the one that the 6 us gate cost
makes tempting: a flash commit cannot take 6 us, so `IONVRAM-FORCESYNCNOW-
PROPERTY` may not commit synchronously, and the flash copy could lag the
thread by an unknown number of gates. That would make every endpoint since 2eo
a measurement of the commit, not the thread - and it would also explain why
`w` never contradicts the trail, since **`w` rides inside the very string
whose commit is in question**, so the two can only ever agree. On that reading
2fg's refutation of the write-budget hypothesis was circular.

It does not hold, and the 2fk boot already said so:

```
in the ARMED boot   logs/20261014-101920-plugin/nvram.txt
                    "... 60 102 152 | 152 ... [w=13 f=0 t=380 wx=367]"
after the power cycle, read directly
                    "... 60 102 152 | 152 ... [w=13 f=0 t=380 wx=367]"
```

**Byte-identical.** The in-boot read returns the in-memory value, so if writes
were being lost to the commit the two would differ. They do not. The trail is
honest about what the thread wrote.

### Where this leaves the variance, stated without a seventh guess

Six structural explanations have now been offered for the varying endpoint and
every one has been refuted on hardware:

```
logging contention        2ez -> fixed in 2fa, variance stayed
NVRAM write budget        2ff -> refuted by 2fg (w=18 f=0)
the register dump         2fe -> helped once, then did not
the gates being the crawl 2fk -> refuted by its own timing (6 us)
a wall-clock watchdog     2fk -> refuted by this boot (t 380 vs 171)
NVRAM commit lag          here -> refuted by the 2fk in-boot read
```

I am not proposing a seventh. What is settled and worth keeping separate from
the variance:

- the chain runs **end to end in 479 us** when it runs: setres + stamp,
  initGraphicsMQD (223 us), mapQueues with real GPU stamps, the ring-state
  reads, and the HQD poll;
- **`CP_GFX_HQD_ACTIVE` reads 1** (2fi) - the graphics queue is live;
- all three accessor slots work - `[0x148]` resolve, `[0x118]` read,
  `[0x120]` write - with seg0 = 0x1260 agreed four independent ways;
- the trail is a trustworthy, 6 us, honest record of what the thread wrote.

## 2fm (2026-10-14): ask whether the thread stops at all — no build required

Every reading in this family, including 2ey's, assumes the thread has *stopped*
where the trail ends. 2ey established the machine is alive and `pci.txt` at 0
bytes shows the start() thread holds the IOService lock; it did **not**
establish that the thread is no longer making progress. A thread advancing at
one gate per minute produces exactly the same record as a stopped one, and
2ez already caught this project mistaking slowness for a hang once.

The test needs no new build and no new code. The trail is in memory and the
machine is alive, so **read it twice from the armed boot, a minute apart**:

```
nvram rbgpu-gate ; sleep 60 ; nvram rbgpu-gate
```

```
the two reads DIFFER     the thread is still advancing and there is no
                         stopping point at all - the "endpoint" is just where
                         the clock was when it was read, every endpoint in
                         2eo-2fl is void for locating anything, and the real
                         question is what costs minutes per gate in code
                         measured at 6 us
the two reads MATCH      the thread genuinely stops, for the first time
                         established rather than assumed, and the endpoint is
                         a real location - so the 102 -> 152 span (our own
                         bounded wait, 2en) is where to look
```

2fl stays installed; its gates 32/33 are unanswered and will answer themselves
on any boot that gets past 152. Boot-args unchanged, build 5CC58C07.

### 2fl/2fm answered on hardware (2026-10-14): GATE 32 — Apple's mapQueues RETURNS

logs/20261014-130311-plugin, `plugin_check` ran **in the armed boot**
(`rbgpu-diag` armed=1, `ttl_hw_init_ret=b6c05001`, a new boot). Build checked:
5CC58C07 == build/out (2fl).

```
nvram rbgpu-gate -> "46 49 47 40 101 151 45 50 54 55 60 102 152 32
                     | 32 mapQueues: the ORIGINAL returned [w=14 f=0 t=369 wx=356]"
```

**Gate 32 landed - the new one - so `submitMapQueuesPacket`'s ORIGINAL returns
on this chip.** Fourteen gates, the most since 2fi, and the dense-gate
reversal paid for itself on its first boot: that return was previously buried
inside the 152 -> 70 span and could only be inferred from the boots that
reached gate 70.

Per-gate, against the log's own timestamps (`t=369` vs 369.3 ms measured, so
the instrument agrees a second time):

```
  46  14.055488
  49  14.058491    3.003 ms
  47  14.068117    9.626 ms
  40  14.424259  356.142 ms   <- wx, the same real-work span as 2fk
 101  14.424273    0.014 ms
 151  14.424374    0.101 ms
  45  14.424380    0.006 ms
  50  14.424405    0.025 ms
  54  14.424413    0.008 ms
  55  14.424646    0.233 ms   <- Apple's initGraphicsMQD again (2fk: 0.223)
  60  14.424652    0.006 ms
 102  14.424660    0.008 ms
 152  14.424751    0.091 ms
  32  14.424756    0.005 ms   <- Apple's epilogue + our return: 5 US
```

- **gate 40 -> gate 32 is 497 us**, a third measurement of the ~480 us
  frontier, and `initGraphicsMQD` reproduces at 233 us against 223 us.
- **Apple's `submitKIQFrame` epilogue and the return into our hook cost 5 us.**
  2fl's branch "the stop is inside Apple's own epilogue" is closed.
- The in-boot `nvram.txt` is byte-identical to the post-power-cycle read
  again, so the commit-lag refutation holds on a second boot.

**And the stop is now bounded to two `rbgpu_diag_set_num` calls** - gate 32 to
gate 33 contains nothing else. That span was read rather than guessed:
`performRefreshDiag` (RembrandtGPUPeer.cpp:1096) releases `g_diagLock` before
building any OSString and before both `setProperty` calls, holding it only
across a ~220 KB memcpy - tens of microseconds, once per 250 ms since 2fa. The
diag table is not full (no `DIAG TABLE FULL` line, `d=27`). So nothing in
those two calls can cost what a stop would cost, and 2fe/2fg traversed this
very span to reach gates 70 and 75.

**Which is the pattern the whole family now shows, stated plainly:** the
endpoint is always at a boundary of our code, our code is measured at 4-6 us,
and the frontier is 500 us wide. Those three cannot all describe a thread
that stops somewhere.

## 2fn (2026-10-14): the 2fm test is still owed, and nothing should be built until it is run

2fm asked for two reads of `rbgpu-gate` a minute apart from the armed boot.
That was not captured this time - the terminal held nothing and `plugin_check`
records the trail once - so **the question it exists to settle is still
open**, and it is prior to every other question here. A thread advancing one
gate a minute leaves exactly the record above; so does a stopped one.

No build. 2fl stays installed (gate 33 is unanswered and will answer itself
on any boot that passes it), boot-args unchanged, UUID 5CC58C07. In the armed
boot:

```
nvram rbgpu-gate ; sleep 60 ; nvram rbgpu-gate
```

```
DIFFER   there is no stopping point - every endpoint from 2eo to 2fm is void
         for locating anything, and the question becomes what costs minutes
         per gate in code measured at 6 us
MATCH    the thread genuinely stops, established rather than assumed for the
         first time, and the 32 -> 33 span is a real location - which would
         then be a hard fact about two IOLock round-trips and the next thing
         to disassemble is the lock, not the GPU
```

Writing a seventh structural hypothesis before that answer arrives would be
the same mistake six times over; the six already refuted are listed under
2fl.

## 2fo (2026-10-14): the user asked whether the restarts are doing anything — the accounting, and the switch

Asked directly, and the honest answer is mostly no. Twenty-four armed boots
since 2em:

```
~7   produced a fact about the CHIP
     2eo  submitSetResourcesPacket returns 1
     2ew  f10 = 0, the command-9 create never runs
     2fe  initGraphicsMQD RETURNS; five RB0 writes land; mapQueues succeeds
          with real GPU stamps
     2fh  Apple's three GC register reads complete, 2cp's cap is on
     2fi  CP_GFX_HQD_ACTIVE = 1, and a GC register WRITE through Apple's
          accessor completes; seg0 agreed a third and fourth way
     2fk  the chain is ~500 us wide; initGraphicsMQD is 223 us
     2fl  Apple's submitMapQueuesPacket original returns

~17  produced a fact about MY OWN INSTRUMENT
     six structural hypotheses, each raised and then refuted on hardware
     (logging contention, the write budget, the register dump, the gates
     being the crawl, a wall-clock watchdog, NVRAM commit lag)
```

Two out of three restarts went into the measuring apparatus, and the frontier
has not moved in the sense that matters: `doStart` has never completed and no
boot has reached a desktop with the accelerator powered since 2em.

**What that table makes obvious in hindsight.** Every measurement inside the
chain says it WORKS. There is no failure anywhere in 500 us - setres returns
1, initGraphicsMQD returns, mapQueues returns with real stamps, the
ring-state reads complete, the HQD is active, and 2fi ran the poll through to
`ret=1`. There is only a thread that does not come back.

And the one thing present in all 24 of those boots and absent from the twenty
milestones before 2eo is **~24 synchronous NVRAM device transactions inside
`IOService::start`**. The heartbeat has been saying the controller stops
answering partway through every one of them (`n=13..28` while the machine
demonstrably lives for minutes, 2ff). **The trail has never been removed and
the chain has never been run without it.**

`-rbgpunogates` does that. The gate path returns after one diag set; the 2et
quiesce is reached only from inside it, so with the gates off the diag summary
and `RBGPULog` keep publishing exactly as they did for 2ce-2en - the
configuration that stayed readable for twenty milestones. No GPU behaviour
changes; `-rbgpuptbase`, `-rbgpustampwait` and the rest are untouched.

Build: UUID 1AC17A45, hosttest passes. **One new boot-arg**, and the boot that
uses it is the first since 2em with no synchronous flash write in the chain.

```
a DESKTOP, or rbgpu-diag carrying mapq_calls=5 / pw_ret / the doStart outcome
        the gate trail was the wedge, and seven boots of "where does the
        thread stop" were measuring a stop that our own breadcrumb caused
wedges the same way, nothing of ours writing flash
        the trail is exonerated, the stop is real, and the 32 -> 33 span
        stands as a genuine location
```

This supersedes 2fn's hold: the two-read test is cheaper to run on a boot that
also answers this one, so run both.

### 2fo answered on hardware (2026-10-14): THE GATE TRAIL WAS THE WEDGE — doStart RETURNED 1

logs/20261014-131620-plugin. Build checked: 1AC17A45 == build/out (2fo),
`ttl_hw_init_ret=c3af8001` so a new boot. **`nvram rbgpu-gate` is unchanged
from the 2fl boot** (same 14 gates, same `t=369 wx=356`), which is the switch
confirming itself: with `-rbgpunogates` the gate path writes no flash at all.

And the chain ran to the end:

```
[14.984808] 2bv: engine[0] = 0xffffff95f52c2800 calling powerUp...
[14.991579] 2bu: AMDGFX10PM4Engine::powerUp entered
[15.954263] 2cq: submitSetResourcesPacket -> 1 (OK)
[16.268503] 2cq: submitMapQueuesPacket(ring=0, ...) -> 1 (OK)
[16.702248] 2cq: submitMapQueuesPacket(ring=5, ...) -> 1 (OK)
[16.713816] 2cp: waitForActiveValue(ring=0, expected=1, bounded=0) -> forcing bounded=1
[16.966970] 2ce: doStart returned 1                       <<<<
[16.977029] 2bu: AMDGFX10PM4Engine::powerUp -> 1          <<<<
[16.987527] 2bv: engine[0] powerUp -> 1                   <<<<
             engine[1] -> 1   [2] -> 1   [3] -> 1   [4] -> 1
             [5][6][7] NULL   [8] -> 1   [9] -> 1   [10] NULL
[17.154513] 2bv: all engines powered up OK                <<<<
[17.163855] 2bs: AMDHardware::startHWEngines entered
[17.173930] 2bs: AMDHardware::startHWEngines -> 0         <<<< the new frontier
[17.183125] 2bt: AMDHardware::powerUp -> 0
[17.211544] 2br: powerUpHW returned 0x0
[17.220497] 2cn: powerUpHW failed, will fail start() cleanly (survive mode)
[17.229666] handlePostRegister: Request 0x06 returned 0x00000000
```

**`doStart` RETURNED 1.** `AMDGFX10PM4Engine::powerUp` returned 1. Every
engine in the array powered up. None of that has ever happened in this
project, and the thread did not wedge - it ran on through `powerUpHW`'s
failure path into `handlePostRegister`, the 2cy shape that completes a boot.

**So 2fo's first branch fired and the accounting in it was right.** The gate
trail was the wedge. The seven "where does the thread stop" boots from 2fh to
2fm were measuring a stop that our own breadcrumb caused, and the variance
that six hypotheses failed to explain had no cause to find: with the
breadcrumb gone the chain simply completes. The one thing never removed was
the one thing responsible.

Worth stating precisely what is and is not withdrawn:

- **Withdrawn:** every endpoint reading from 2eo to 2fm as a statement about
  where the thread stops, and with them the 32 -> 33 span of 2fn. There was
  no stopping point to locate.
- **Not withdrawn:** every *committed* gate, because a gate that landed is
  positive evidence whatever happened after it. 2fe's `initGraphicsMQD`
  RETURNS, 2fi's `CP_GFX_HQD_ACTIVE = 1`, 2fh's GC reads, 2fl's mapQueues
  return, 2fk's timings - all still stand, and this boot reproduces the whole
  chain independently with no gates at all.
- Also note the logging ran at ~10 ms a line in this boot (17.135, 17.145,
  17.154 ...), the 2ez contention shape, which is why the chain took 2 s here
  against the 500 us 2fk measured. It was slow and it still finished.

### The new frontier: `AMDHardware::startHWEngines` (0xbea8fea, disassembled)

```
bea8ff8  count = hw->0xc2 * 2 + 2
loop:
  if (i == count)        -> r12w = 0x200, SUCCESS
  e = hw->0x3b8[i]       the SAME engine array 2bv walks
  if (!e)                -> r12 = 0, FAIL            (0xbea9068)
  if (!e->vtable[0x148]()) -> os_log, r12 = 0, FAIL  (0xbea9030)
  i++ ; continue
```

So it is the 2bv shape one function later, over the same array, through
`vtable[0x148]` instead of `powerUp` - and Apple names the failing index in
an `__os_log_internal` call we do not capture. It returned 0 in a single log
line's worth of time, with no inner logging of ours to slow it.

Note it did **not** re-enter `doStart`: the hook logged exactly one
`doStart returned`, so whatever `vtable[0x148]` is for these engines in this
context, it is not the `powerUp` thunk path running a second time. Stated as
an observation; the slot is not yet resolved per class.

## 2fp (2026-10-14): name the engine that refuses

`rbgpu_walk_starthw_2fp` runs from the existing `startHWEngines` ENTER hook
and walks the array Apple is about to walk - reading `hw->0xc2`, the count,
each `hw->0x3b8[i]` and each engine's `vtable[0x148]` pointer. Reads only;
the original still makes every call, and nothing touches hardware. The count
is refused if it exceeds 32 rather than walking a garbage length.

```
engine[i] = NULL           startHWEngines fails at the NULL check (0xbea9068)
                           and the index names the missing engine outright
all non-NULL               the FALSE comes from one of the vtable[0x148]
                           calls, and the logged slot pointers say which
                           function to disassemble
```

Gates stay OFF: `-rbgpunogates` is now the configuration that works and the
boot-args keep it. Build: UUID 88399259, hosttest passes.

### 2fp answered nothing (2026-10-14): the boot ran and left no record

Build and arming checked first, per the 2cu/2cv rule:

```
kmutil showloaded -> com.rembrandtgpu.plugin 88399259   == build/out (2fp)
logs/20261014-132756-plugin/nvram.txt
   rbgpu-diag  = d=27 armed=1 ttl_hw_init_ret=ffffffffdca2e001
                 (2fo read c3af8001, so this IS a new boot)
   rbgpu-gate  = unchanged from the 2fl boot - the gates are still off
log.txt      = 0 bytes
ioreg-raw.txt = ABSENT                 (the 2fo boot's was 484 bytes + .done)
```

So the armed 2fp boot happened and produced **no evidence at all**.
`log show --last boot` returned nothing and `plugin_check` never reached even
its first bounded ioreg, so the capture is equally consistent with the boot
wedging and with the script being interrupted - and the log cannot be
recovered afterwards: the persistent store has nothing for 13:20-13:40 and
the machine rebooted at 13:28:23, so `--last boot` now covers a different
boot. No `.panic` was written (the newest is 2026-10-11), so it did not panic.

**And the fallback channel does not work either, which is the part worth
keeping.** The 2fo and 2fp summaries are *byte-identical* apart from
`ttl_hw_init_ret`, and both end at `regchan_calls=19` with `d=27`. 2fo is the
boot that ran the entire chain and reached `handlePostRegister`; its summary
stops hundreds of log lines before `powerUp` all the same. So `rbgpu-diag`
has not been reporting the frontier since the gates went off, whatever the
boot does, and reading a key's absence from it as a fact would be the
eighteenth instance of the recurring error.

Not claimed: that 2fp's walk caused the loss. It reads the array 2bv already
walks and adds ~11 log lines; nothing in it writes flash, and a bad pointer
would have panicked rather than gone quiet.

## 2fq (2026-10-14): give the answer a channel that survives a dead boot

The question is unchanged - which engine `startHWEngines` refuses - and the
only thing wrong with 2fp was that its answer lived in a log.

`rbgpu_nvram_note(var, text)` writes **one** NVRAM variable, once, on the
calling thread, with the same FORCESYNCNOW commit the gate trail used. The
distinction from the trail is the whole point of 2fo's accounting: what
wedged those boots was ~24 synchronous flash commits threaded through
`IOService::start`, not one. Gates 50, 54, 55, 70 and 75 each landed on boots
that went on running, and 2fk measured a gate at ~6 us.

Two writes per boot, both to `rbgpu-starthw`:

```
after the walk   c2=N n=M map=..... allok | NULL@i | NOFN@i
after the call   the same string plus ret=0/1
```

The map is one character per engine - `.` non-NULL with a non-NULL
`vtable[0x148]`, `f` non-NULL with a NULL slot, `-` NULL - so one variable
carries the whole array rather than an index whose meaning has to be
remembered. The second write is what proves the first is this boot's and not
a stale read.

The NVRAM hot list is also rewritten for this frontier. Everything in it was
`gate_stage`, `ptb_*`, `stamp_*`, `gfxmqd_*` and `mapq_*` - all belonging to
questions 2fo answered - and per the 2ec rule they move down into the main
list behind `starthw_c2`, `starthw_count`, `starthw_null_idx`,
`starthw_badfn_idx`, `pw_stage`, `pw_ret`. Given the paragraph above, those
keys are a hope; `rbgpu-starthw` is the record. `tools/plugin_check.sh`
captures it first.

Build: UUID EA5A3668, hosttest passes. Boot-args unchanged - `-rbgpunogates`
stays, it is the configuration that works.

```
map has a '-'    startHWEngines fails at the NULL check (0xbea9068) and the
                 position names the engine; 2bv powered that same index up,
                 so something between powerUp and here removed it
map all '.'      the FALSE comes from one of the vtable[0x148] CALLS, and
                 the next step is to resolve that slot per engine class and
                 disassemble it
map has an 'f'   Apple calls through a NULL slot unconditionally, which is a
                 fault and not a FALSE - a different failure from both
nothing written  the boot does not reach startHWEngines, which would itself
                 be new: 2fo reached it
```

## 2fr (2026-10-14): the 2fo boot stopped at request 0x1B, not at startHWEngines

The user ran `plugin_check` in the 2fo boot and it "got stuck at Verdict
Milestone 1 like always", then power-cycled. That single sentence, checked
against the 2fo log, settles two things the previous entry got wrong.

**The 2fo boot did NOT reach a desktop.** The `doStart RETURNED 1` entry read
`handlePostRegister` as "the 2cy shape that completes a boot" and said the
desktop was suggestive but unconfirmed. It did not happen. `pci.txt` is 0
bytes in that capture, exactly as in every wedge, so the IGPU registry
subtree was locked while the log was taken - and the log's true last line is
not what that entry quoted:

```
[17.220497] 2cn: powerUpHW failed, will fail start() cleanly (survive mode)
[17.229666] handlePostRegister: Request 0x06 returned 0x00000000
[17.238625] handlePostRegister: sending Request 0x1B (Controller Enabled)
   <nothing further, and the machine sat there for minutes>
```

The 2cy boot logged `Request 0x1B returned 0xE00002C7` and carried on into
Wi-Fi scanning. **This boot sent 0x1B and never got an answer.**

**And the timestamps say what that thread is actually doing, which the
journal had structurally wrong.** Request 0x06 is SENT at 14.944 and RETURNS
at 17.229, 2.3 s later, while `2cn: accelerator start returned 1` is logged
at 14.958 in between:

```
14.938  Registered accelerator instance
14.944  handlePostRegister: sending Request 0x06        <- the peer's thread
14.958  2cn: accelerator start returned 1               <- start() is DONE
...     powerUp, doStart, the whole chain               <- inside 0x06
17.229  handlePostRegister: Request 0x06 returned 0     <- 2.3 s later
17.238  handlePostRegister: sending Request 0x1B        <- and it hangs
```

So the entire powerUp chain runs **on the peer's own post-register thread,
inside request 0x06**, not on the accelerator's `start()` thread. That also
retires the 2cn "survive mode" line as anything but a log message: `start()`
had already returned 1 twenty log lines earlier, so there is no `start()`
left to fail. (2cx had noted powerUpHW runs after `start()` returns; what is
new is which thread makes the call and from where.)

Why 0x1B hangs now when 2cy answered it at once: in 2cy powerUpHW failed at
the very first gate with the hardware untouched. Now the engines are powered,
the queues are mapped and the rings are programmed, so "Controller Enabled"
reaches code with real state to act on.

**The change.** Request 0x1B is not sent when powerUpHW returned 0.
`g_rb_poweruphw_ok` is set from powerUpHW's own return value, so this is the
honest state and not a bypass: the controller is not enabled, and claiming
otherwise would be one more blanket-true latch of the kind 2am/2an/2ao
already owe an explanation for. `-rbgpuforce1b` sends it anyway, for the boot
where powerUpHW finally succeeds.

`rbgpu-post` records which of the two happened, through the 2fq one-shot
note, so a boot that still wedges says whether 0x1B was sent:

```
0x06 done ret=0; 0x1B SKIPPED (powerUpHW failed)   the skip took effect
0x06 done; 0x1B SENT - no return means it hangs here
0x1B RETURNED 0x........                           it answered after all
```

Build: UUID 0E64D1C2, hosttest passes. Boot-args unchanged.

What the next armed boot decides, and it is two answers in one boot because
2fq made the first one independent of the log:

```
nvram rbgpu-starthw    the engine map: which entry startHWEngines refuses,
                       or that it refuses none of them and the FALSE comes
                       from one of the vtable[0x148] calls
a DESKTOP              0x1B was the wedge, plugin_check runs normally, and
                       the full log comes out of the same boot - which would
                       be the first readable complete armed boot since 2em
wedges anyway, with    the stop is somewhere else again, and rbgpu-post plus
rbgpu-post = SKIPPED   rbgpu-starthw say how far it got with no log at all
```

### 2fr answered nothing either (2026-10-14): the variables were never created

Build and arming checked first, per the 2cu/2cv rule:

```
kmutil showloaded -> com.rembrandtgpu.plugin 0E64D1C2   == build/out (2fr)
rbgpu-diag        -> d=0 armed=0                        the recovery boot
rbgpu-diag-prev   -> d=26 armed=1 ttl_hw_init_ret=ffffffffa0e93001
                     (2fp read dca2e001, so this IS a new boot)
nvram rbgpu-starthw -> absent
nvram rbgpu-post    -> absent
nvram -p | grep rbgpu -> 6 variables, and NEITHER new one is among them
no new logs/ dir, no .panic; shutdown stall at 13:39, this boot began 13:41
```

**The two variables were never created at all**, so the writes did not merely
land late or get lost - `rbgpu_nvram_note` was never called, or it does not
work. Those are the only two readings and **nothing in this boot
distinguishes them**:

```
the thread never reached either call site - startHWEngines ENTER, or
  handlePostRegister's 0x1B decision
rbgpu_nvram_note does not work: it creates a NEW NVRAM variable, which is
  not what the gate trail does (that overwrites rbgpu-gate, created boots ago)
  and has never been exercised
```

And the reason it is ambiguous is mine. All four of 2fq/2fr's call sites sit
**past** the frontier whose endpoint has varied in every boot from 2fa to
2fm. 2fd wrote the rule after 2fb and 2fc each spent a boot on it - *a
measurement placed behind a varying gate inherits the variance* - and 2fq put
both of its writes there anyway. Fourth time in this project (2eh, 2ek, 2en,
2fb) that absence has been built in as the answer.

Also worth noting against the diag summary: `regchan_calls=1` here against
`19` in both 2fo and 2fp. Same keys, same order, a boot that froze even
earlier. That channel is not reporting the frontier, which 2fq already said.

## 2fs (2026-10-14): one stage-numbered variable, first written where the code always reaches

`rbgpu-prog`, overwritten with a stage number at each milestone, replacing
`rbgpu-starthw` and `rbgpu-post`:

```
s=1  doStart ENTER                      <- the always-reached site
s=2  doStart RETURNED 0/1
s=3  the startHWEngines engine map
s=4  the map plus startHWEngines' ret
s=5  0x1B SKIPPED (powerUpHW failed) | 0x1B SENT
s=6  0x1B RETURNED 0x........
```

**Stage 1 is the whole point.** doStart ENTER has executed in every boot since
2ce, twenty milestones ago, and it is the same site 2fd moved the accessor
probe to for exactly this reason. So the next boot cannot come back ambiguous:

```
rbgpu-prog absent        rbgpu_nvram_note does not work - settled in one boot
                         rather than inferred, and the channel has to change
s=1 only                 the mechanism works AND the thread did not get past
                         doStart, which would be a regression against 2fo and
                         the first direct evidence of the varying stop with
                         no gate trail in the boot at all
s=3 / s=4                the engine map, which is the question 2fp and 2fq
                         were both built to answer
s=5 / s=6                the 0x1B skip took effect and the boot got past the
                         point the 2fo boot sat at for minutes
```

Commit budget, stated because it is the thing 2fo blamed for the wedge:
**one** flash commit inside the ~500 us chain region (stage 1, guarded by a
`static bool` so it fires once however many engines call doStart), and the
rest only after the chain has returned. 2fo's accounting was about ~24
commits threaded *through* the chain.

Build: UUID 03CF6E1A, hosttest passes. Boot-args unchanged - `-rbgpunogates`
stays.

### 2fs answered on hardware (2026-10-14): the channel WORKS — and 2fo does not reproduce

logs/20261014-135212-plugin, `plugin_check` ran **in the armed boot**
(`rbgpu-diag` armed=1, `ttl_hw_init_ret=df06a001`, a new boot). Build checked:
03CF6E1A == build/out (2fs).

```
nvram -p | grep rbgpu -> rbgpu-prog NOW EXISTS
nvram rbgpu-prog      -> "s=1 doStart ENTER"
log.txt               -> 352 lines, 53 KB
ioreg-raw.txt         -> 484 bytes + .done   (the first bounded ioreg completed)
pci.txt               -> absent              (stuck at the IGPU walk, as in every wedge)
```

**The mechanism is settled.** `rbgpu-prog` was created and read back, so
`rbgpu_nvram_note` works - and that resolves 2fr's ambiguity in the other
direction: **neither of 2fq/2fr's call sites was reached**, which is why
those variables never appeared. One of the two readings is now closed.

**But `s=1 only` does NOT mean the thread stopped at doStart**, and the log
says so outright:

```
[12.928153] 2br: powerUpHW entered
[13.010800] 2fs: prog s=1 doStart ENTER
[13.464847] 2cq: initGraphicsMQD -> 1 (OK)
[13.464850] 2cx: submitMapQueuesPacket ENTER (ring=0)
[13.464907] 2en: BOUNDED wait satisfied after 1 polls: +0x84 reached 2
[13.464929] 2cx: waitForHwStamp -> 1 (the GPU executed it)
```

The chain ran **454 ms past stage 1**, through `submitSetResourcesPacket`,
`initGraphicsMQD -> 1` and into `submitMapQueuesPacket` ring 0 with stamp #2
arriving - and `s=2`, written at `doStart`'s return, never landed. Stage 1 is
at doStart ENTER and stage 2 at its return, so the thread is somewhere between
mapQueues ring 0 and that return: **the same region the gate trail used to
stop in.**

**So 2fo's "the gate trail was the wedge" does not reproduce, and that
conclusion has to come down to size.** It rested on one boot. This boot has
ONE synchronous flash commit instead of twenty-four, no gate trail at all, and
inter-line gaps of **2-6 us** across the whole tail - no logging contention,
no trail, and the chain still does not finish. 2fo remains the only boot in
which it did.

Nineteenth instance of the recurring error and the same shape as the withdrawn
2ev table: a qualitative jump in a single sample read as a rule. What 2fo
still establishes, because committed evidence survives: `doStart` CAN return
1, `AMDGFX10PM4Engine::powerUp` CAN return 1, every engine CAN power up, and
`startHWEngines` returns 0 when reached. What it does not establish is that
removing the trail is sufficient.

## 2ft (2026-10-14): zero commits inside the region, with the record still outside it

One variable moved, nothing else. Stage 1 goes from `doStart` ENTER to
**`powerUpHW` ENTER** - which runs ~80 ms earlier (12.928 against 13.011 in
this very boot) and has executed in every boot since 2br - so the ~500 us
chain region now contains **no flash commit at all**, exactly like 2fo, while
`rbgpu-prog` still proves the channel from outside it.

```
s=1 then s=2/s=3/s=4   the chain completes with nothing of ours committing
                       inside it - two boots agreeing, and the engine map
                       arrives in the same breath
s=1 only, again        no commit, no trail and no logging contention in the
                       region, and it still stops: the stop is real, and for
                       the first time there is nothing of ours left in there
                       to blame
```

That second branch is the one worth stating plainly in advance, because six
structural explanations are already refuted and this would remove the seventh
candidate (our own one commit) without proposing an eighth. The region would
then be Apple's code and the GPU alone: three more `submitMapQueuesPacket`
calls with their stamps, the ring-state reads, the HQD poll, and
`vtable[0x328]`.

Build: UUID 55F4C6A3, hosttest passes. Boot-args unchanged - `-rbgpunogates`
stays.

### 2ft answered on hardware (2026-10-14): the chain completed, and startHWEngines was ENTERED

logs/20261014-163218-plugin, `plugin_check` in the armed boot
(`armed=1`, `ttl_hw_init_ret=df06a001`, a new boot). Build checked:
55F4C6A3 == build/out (2ft).

```
nvram rbgpu-prog -> "s=2 doStart RETURNED 1"
log.txt          -> 352 lines, ends at the 2fp walk's count line
ioreg-raw.txt    -> 484 bytes + .done ; ioreg.txt 159 bytes ; pci.txt 0 bytes
```

```
[12.718615] 2br: powerUpHW entered
[12.718665] 2fs: prog s=1 powerUpHW ENTER - before the chain
[14.797252] 2ce: doStart returned 1
[14.797271] 2fs: prog s=2 doStart RETURNED 1
[14.797275] 2bv: engine[0] powerUp -> 1   [1] -> 1  [2] -> 1  [3] -> 1  [4] -> 1
[14.797296]      [8] -> 1   [9] -> 1
[14.797301] 2bv: all engines powered up OK
[14.797303] 2bs: AMDHardware::startHWEngines entered (hw=0xffffff80454f7000)
[14.797306] 2fp: startHWEngines will walk 4 entries (hw+0xc2 = 1)
```

**`doStart` returned 1 and stage 2 committed**, so with zero flash commits
inside the ~500 us region the chain completed - 2fo's result reproduced, which
2fs (one commit at doStart ENTER) did not manage. Two boots for "no commits in
the region" against one for "one commit", so the correlation is now worth
something without being a demonstration.

And the count line is the finding: **`hw->0xc2 = 1`, so count = 4.**

## 2fu (2026-10-14): startHWEngines walks the SDMA engines — resolved with no boot

Two things I wrote are wrong, and the disassembly corrects both.

**The arrays are eight bytes apart.** `powerUpHWEngines` reads `hw+0x3B0`, 11
entries, `vtable[0x138]` (0xbea8ec9-0xbea8ef5 - so 2bv's manual loop is
faithful). `startHWEngines` reads `hw+0x3B8`, count `hw->0xc2 * 2 + 2`. The
2fp comment "the SAME engine array 2bv walks" was off by one pointer:

```
0x3B0[0]      AMDRadeonX6000_AMDNavi21PM4Engine
0x3B0[1..4]   AMDRadeonX6000_AMDSDMAEngine_GC_10_3_0   == 0x3B8[0..3]
0x3B0[8..9]   AMDRadeonX6000_AMDVCN3HWEngine
```

`hw->0xc2 = 1` gives count 4, which is **exactly** the number of SDMA engines
present, and 0x3B8 is **exactly** where they begin. Two independently-derived
numbers agreeing, so `startHWEngines` walks the four SDMA engines and the PM4
engine is not in its window. All four are non-NULL (2bv powered them up), so
**the NULL check at 0xbea9068 cannot fire** and the FALSE comes from a
`vtable[0x148]` call - 2fq's "map all '.'" branch, settled without the walk's
own per-engine lines ever reaching the log.

**And that resolves the 2fo anomaly instead of leaving it.** Slot 0x148 is
`start()`, and for the PM4 class it is `0xbed2342` = `xorl %esi,%esi ; jmp
doStart` - a `doStart(false)` thunk. Had the PM4 been in range, doStart would
have run twice; the 2fo log has exactly **one** `doStart entered` with
`startHWEngines -> 0` ten ms later. It is not in range, so there was never a
second call to explain. For the SDMA class, slot 0x148 is
`AMDRadeonX6000_AMDGFX10SDMAEngine::start()` (0xbea4a6a).

### Apple's SDMA start(), disassembled end to end (0xbea4a6a)

```
bea4a6a  if (this->0x10 != 0)   return FALSE        one byte, no work at all
bea4a87  svc = (this->0x18)->vtable[0x2c0]()
bea4a94  if (this->0x48 == 0)   -> bea4b49          zero queues: trivial TRUE
loop i < this->0x48:
  bea4aa7  ch   = this->0x50[i]
  bea4ad4  desc = { 0x40000, ch->0x48, ch->0xd0 }
  bea4b0c  svc->vtable[0x120](this->0x1a0[i], this->0x1a4[i], &desc, &out)
  bea4b14  if (ret != 0)        -> bea4b5d          unmap, return FALSE
bea4b49  this->0x10 = 1 ; vtable[0x278]() ; return TRUE
```

`0x10` is a **"started" flag**: set on success, and a second `start()` refuses
at the first instruction. The tempting explanation was therefore that
`powerUp` had already set it, making the FALSE a double-call artefact.

**Refuted statically, before it cost a boot:** the SDMA `vtable[0x138]`
(powerUp) is `0xbed1fac` = `movb $0x1,%al ; retq` - a constant TRUE that
touches nothing. That is also why 2bv logged engines 1-4 powering up **3 us
apart**. So `0x10` is 0 on entry and the body really runs.

**What is left is `svc->vtable[0x120](cmd, ...)` returning nonzero** - the
same service-object slot `startKIQ` uses with command 8, which works on this
chip. This is the first time SDMA has asked that layer for a queue here, and
the command ids come from `this->0x1a0[i]` / `0x1a4[i]`.

Worth noting what now becomes relevant: `_sdma_5_2_1_fw_autoload_is_completed`
has been forced true since 2ao (one of the three blanket bypasses 2cy said
were masking something), and this is the first code path that depends on the
SDMA block actually being up.

### The instrument

`AMDGFX10SDMAEngine::start` is hooked observe-only - `nm -m` shows it external
and defined in X6000, so routing it by X6000's id is safe under the
milestone-2c rule. Per call it logs `this`, the `0x10` flag, the queue count
`0x48` and the two command ids, all read **before** the original so they are
what Apple's own gates test, then the return.

All four calls also accumulate into one compact string that rides out on the
single stage-4 note at `startHWEngines`' exit, so a boot with no log still
answers:

```
s=4 ... ret=0 sdma[4]{#0:f10=0,q=N,r=1 #1:... #2:... #3:...}
```

```
one r=0 with f10=0      that engine's queue create refused - the index names
                        it and the next step is the command id it sent
one r=0 with f10=1      it was already started, which the static reading says
                        cannot happen on the first call, and would mean
                        start() runs twice
all r=1 and ret=0       startHWEngines returns 0 for a reason outside the
                        loop, and 0xbea906b onward is where to look
q=0 everywhere          every engine takes the trivial-TRUE path and the
                        FALSE is not in the queue create at all
```

Stage 1 stays at `powerUpHW` ENTER, so the chain region keeps zero commits;
stages 3 and 4 are both after it.

Build: UUID 83F2470A, hosttest passes. Boot-args unchanged - `-rbgpunogates`
stays.

### 2fu answered on hardware (2026-10-14): two boots, neither finished the chain

Two armed boots, both captured in-boot, build checked (83F2470A == build/out):

```
logs/20261014-164916-plugin   ttl_hw_init_ret=ca584001   218 log lines
logs/20261014-165258-plugin   ttl_hw_init_ret=bec27001   186 log lines
both: rbgpu-prog = "s=1 powerUpHW ENTER"   pci.txt 0 bytes   no 2fu: lines
both logs end at: 2cx: waitForHwStamp -> 1 (the GPU executed it)
```

Different `ttl_hw_init_ret`, so two separate boots and **not** the 2fm
two-read test - that question is still unrun. Both reached
`initGraphicsMQD -> 1` and the ring-0 stamp, neither reached `doStart`'s
return, and the SDMA hook never got a chance to fire.

**So 2ft's correlation is already weakened by its own successor.** Zero flash
commits inside the region now stands at **two completions out of four boots**
(2fo, 2ft complete; 2fu's two do not), against one boot for the
one-commit configuration. That is not a rule, and 2ft said it was "worth
something without being a demonstration" - this is the demonstration failing.
The variance 2fo claimed to explain is **not explained**.

## 2fv (2026-10-14): the mapQueues loop runs TWICE, not six times

Found offline, chasing a mismatch rather than assuming either side. The 2fo
log carries exactly two mapQueues calls, `ring=0` and `ring=5`, while the loop
I read increments the ring by one - which cannot produce 0 then 5. The loop
top is eight bytes earlier than where I started reading:

```
bea139f  cmpl $0x5, %r13d
bea13a3  je   bea13b4        ring == 5 -> initComputeMQD(5)
bea13a5  testl %r13d,%r13d
bea13a8  jne  bea140a        ring 1,2,3,4 -> SKIP STRAIGHT TO THE INCREMENT
bea13aa  initGraphicsMQD()   ring == 0 only
bea13c1  testb %al,%al ; je -> fail
bea13cc..bea1402  submitMapQueuesPacket(ring = r13, ...)
bea140a  incl %r13d ; cmpl 0x348(%rbx) ; jbe loop
```

**Rings 1-4 do nothing at all** - the `jne` jumps past both the MQD init and
the submit. Only ring 0 (graphics) and ring 5 (compute) have work, which is
exactly the two calls the chip shows. The disassembly and the hardware now
agree, where before they silently did not.

**Correction: "+0x348 = 5, so five submitMapQueuesPacket calls" (2cl, repeated
in 2fe as "three mapQueues calls remain") is wrong.** `0x348` is the loop
BOUND, not a count; the body is gated to two ring values. There are **two**
mapQueues calls and **both already succeed** on this chip.

Consequence worth stating plainly: **`doStart` has no unexercised step left.**
setres + stamp, initGraphicsMQD, mapQueues ring 0 + stamp, initComputeMQD(5),
mapQueues ring 5 + stamp, the ring-state reads, `waitForActiveValue` (HQD
active, 2fi), `vtable[0x328]` - every one has been observed to complete, and
2fo and 2ft both ran the whole thing to `doStart returned 1`.

So the wedging boots stop in a narrow, named window: **after ring 0's stamp
returns and before ring 5's mapQueues completes** - `initComputeMQD(5)` and
one GPU submit. 2fo and 2ft traversed it; 2fs and both 2fu boots did not.

### No build. The test that is owed, and is free

2fm asked for two reads of the breadcrumb a minute apart from inside the
armed boot, and it has now gone unrun for four milestones while six
structural hypotheses were raised and refuted. It is prior to all of them:
**a thread advancing one step a minute leaves the same record as a stopped
one**, and 2ez already caught this project mistaking slowness for a hang.

`rbgpu-prog` makes it a one-liner, and it needs no new build - 2fu stays
installed, boot-args unchanged, UUID 83F2470A. In the **armed** boot, once
`plugin_check` has shown the wedge:

```
nvram rbgpu-prog ; sleep 60 ; nvram rbgpu-prog
```

```
DIFFER   there is no stopping point - the chain is crawling, every endpoint
         reading is void, and the question becomes what costs a minute per
         step in code measured at microseconds
MATCH    the thread genuinely stops, established for the first time, and the
         window above (initComputeMQD(5) + one submit) is a real location
```

Writing a seventh structural hypothesis before that answer arrives would be
the same mistake six times over.

### 2fu, third boot (2026-10-14): s=1 again, and the test is still unrun

```
kmutil showloaded -> 83F2470A == build/out (2fu)
rbgpu-diag-prev   -> armed=1 ttl_hw_init_ret=ffffffffb5583001  (a new boot)
nvram rbgpu-prog  -> "s=1 powerUpHW ENTER"
no new logs/ dir  -> no plugin_check capture this time
```

So "zero flash commits inside the region" now stands at **two completions out
of five boots** (2fo, 2ft complete; 2fs with one commit and 2fu's three
without). Whatever decides it, our commit count is not it.

## 2fw (2026-10-14): the plugin runs the 2fm test itself

2fm asked for two reads of the breadcrumb a minute apart from inside the
armed boot. It has been owed for **five milestones** and has never once been
run, and the reason is structural rather than anybody's forgetfulness: it
asks the user to type two commands into a machine that is wedging, while
`plugin_check` is blocked on `ioreg` and the obvious reflex is to power-cycle.

It is also still prior to every other question here. **A chain advancing one
step a minute leaves exactly the record a stopped one leaves**, and 2ez
already caught this project reading slowness as a hang once. Six structural
explanations for the variance have been refuted; a seventh should not be
written before this is settled.

So the plugin runs it. Two one-shot `thread_call`s, at **+30 s** and
**+90 s** from `powerUpHW` ENTER, each appending the stage the chain has
reached to `rbgpu-prog2`:

```
a=N@...ms          the stage at +30 s
a=N@...ms b=M@...  and at +90 s
```

Both samples land in one variable, so a power cycle still yields the
comparison, and **no user action is involved at all** - no SSH, no typing
into a wedged console, no second capture.

```
b > a    the chain is ADVANCING - there is no stopping point, and every
         endpoint reading in this project is void for locating anything.
         The question becomes what costs a minute per step in code measured
         at microseconds.
b == a   it is genuinely STOPPED, established rather than assumed for the
         first time, and the 2fv window (initComputeMQD(5) + one GPU submit)
         is a real location.
absent   the timer never fired - which is its own answer, and the one 2ez
         already saw when the heartbeat died at n=13..28 with the machine
         demonstrably alive: the thread_call pool is starved.
```

Cost: two flash commits, both long after the ~500 us chain region, which is
the budget 2fs/2ft established. **Nothing is written inside the region** -
stage 1 stays at `powerUpHW` ENTER and the timer is armed beside it.

`tools/plugin_check.sh` captures `rbgpu-prog2` first, but the point is that
it does not need to: `nvram rbgpu-prog2` from the recovery boot is the whole
answer.

Build: UUID 64AEBDD8, hosttest passes. Boot-args unchanged - `-rbgpunogates`
stays.

### 2fw answered on hardware (2026-10-14): the timer FIRES, to the millisecond

Two armed boots, both with `plugin_check` run in the boot, build checked
(64AEBDD8 == build/out):

```
logs/20261014-172734   ttl_hw_init_ret=c3789001   261 lines  pci.txt 0
    rbgpu-prog  = s=1 powerUpHW ENTER
    rbgpu-prog2 = a=1@43832ms          armed at 13826 ms -> +30006 ms
    log ends 14.293, at ring-0's stamp returning

logs/20261014-172925   ttl_hw_init_ret=d107d001   290 lines  pci.txt 393216
    rbgpu-prog  = s=2 doStart RETURNED 1
    rbgpu-prog2 = a=2@43136ms          armed at 13137 ms -> +29999 ms
    log ends 15.2135, at "2fp: startHWEngines will walk 4 entries"
```

**The sampler works and the `thread_call` pool is not starved.** Both samples
fired within 6 ms of their 30 s deadline, from the same pool whose starvation
2ez inferred when the heartbeat died at `n=13..28`. That inference does not
hold for this configuration - what killed the beat was its own four-per-second
flash commits, not the pool.

**What one sample each already says, and it is the first timing of the stall.**
Boot B reached stage 2 at **15.2 s** and was still at stage 2 at **43.1 s**:
28 seconds inside `startHWEngines`' four-SDMA walk, which 2fk-era timings put
at microseconds. Boot A was still at stage 1 at **43.8 s**, 30 s after
`powerUpHW` ENTER at 13.8 s, with its log ending at ring-0's stamp. So the two
boots differ by a whole stage at the same wall-clock moment on identical code:
**the variance is in time taken, not in a position where execution stops.**

**What it does NOT say** - and this is the question 2fm has owed for six
milestones - is whether either thread ever moves again. One sample cannot;
`b` never landed in either boot, because +90 s is past when a wedged machine
gets power-cycled. 2fw asked for the wrong cadence.

**Boot B is also the most complete armed capture this project has taken:**
`pci.txt` 393216 bytes, `igpu.txt`, `peer.plist`, and the accelerator present
on IGPU@0 beside the peer, AMDSupport, IONDRVFramebuffer and HWServicesNavi.
Every previous wedge left `pci.txt` at 0. So the IGPU registry subtree was not
locked - consistent with 2fr, where the chain runs on the peer's post-register
thread inside request 0x06 and not inside `IOService::start` at all.

And `amd-log.txt` confirms 2fu's static reading of the engine array with no
boot spent on it: engine[0] `AMDNavi21PM4Engine`, engines [1..4]
`AMDSDMAEngine_GC_10_3_0`, engines [8][9] `AMDVCN3HWEngine` - exactly the
layout that makes `hw+0x3B8` with count 4 the four SDMA engines.

## 2fx (2026-10-14): three samples, and a counter finer than the stage

Two faults in 2fw, both mine, both about the sampler rather than the chip.

**The cadence outlived the boot.** +30 s and +90 s; the first landed twice and
the second never. Now **+15 s, +35 s, +65 s**, so a boot power-cycled at ~45 s
still yields two points and the comparison survives.

**The stage is too coarse to answer the question.** There are six stages, and
the entire `startHWEngines` walk lives between stage 2 and stage 3 - so boot
B's "stage 2 at 15 s, stage 2 at 43 s" is equally consistent with a thread
stopped and a thread crawling through four SDMA `start()` calls. Each sample
now also carries **`g_rbgpuLogSeq`**, a monotonic count bumped on every log
append *and* every dedupe hit, which never saturates at the ring size the way
`g_logCount` does.

```
a=s2/L290@15000ms b=s2/L290@35000ms   stopped - nothing logged in 20 s, and
                                      the 2fv window is a real location
a=s2/L290@15000ms b=s2/L461@35000ms   CRAWLING - the stage has not moved but
                                      the thread is still running code, every
                                      endpoint reading in this project is void
                                      for locating anything, and the question
                                      becomes what costs seconds per step
```

Cost is unchanged in the place that matters: three flash commits, all long
after the ~500 us chain region, which still contains **no** commit of ours -
stage 1 stays at `powerUpHW` ENTER and the timer is armed beside it.

Build: UUID 3B9D0163, hosttest passes. Boot-args unchanged - `-rbgpunogates`
stays.

### 2fx answered on hardware (2026-10-14): the SECOND sample never fires

logs/20261014-200821-plugin, `plugin_check` in the armed boot, build checked
(3B9D0163 == build/out), `rbgpu-diag` armed=1 `d=1`:

```
rbgpu-prog  = s=2 doStart RETURNED 1          <- the FOURTH chain completion
rbgpu-prog2 = a=s2/L748@27786ms               <- one sample, again
log.txt     = 139 lines, ends at "2fp: startHWEngines will walk 4 entries"
pci.txt     = 389120 bytes                    <- healthy, as in the 2fw boot
```

The sample carries `L748`, so the log-sequence counter works; the stage is 2,
so `doStart` had already returned by 27.8 s. **And the `b` sample at +35 s did
not land** - 20 seconds after the one that did.

**That is three boots now, and it is the finding.** 2fw armed +30/+90 and lost
the second; 2fx armed +15/+35/+65 and lost the second and third. Every FIRST
shot landed within 6 ms of its deadline; no later shot has ever landed, at a
60 s gap or a 20 s gap alike. The interval is not the variable.

The one structural difference between the shot that works and the shots that
do not: **the later ones are armed by `thread_call_enter_delayed` called from
inside that same `thread_call`'s own handler.** Re-entering a thread_call from
its own callback is exactly where that call is already marked running, and the
completion path can clear the deadline the handler just set. So this is a
mechanism, not a guess, and three boots at two intervals are its evidence.

Worth recording separately: the published `RBGPULog` and `log.txt` disagree in
this capture - the plist holds 394 entries ending at 12.72 s while log.txt
holds 139 ending at 14.75 s. Two snapshots of one ring cannot both be right,
so one of plugin_check's two reads is partial. Noted, not chased; it changes
no conclusion here because `rbgpu-prog2` is NVRAM and neither is involved.

Also reproducible across all four completing boots (2ft, 2fw-B, 2fx): the log
ends at the `2fp` count line, with the per-engine lines never published.

## 2fy (2026-10-14): three timers, none of which re-arms itself

`rbgpu_progwatch_start` allocates **three separate `thread_call`s** and arms
all three from the calling thread at +15 / +35 / +65 s, each carrying its own
phase index as its parameter. The handler samples, writes and returns -
nothing re-arms anything, so the failure mode that cost 2fw and 2fx their
second samples cannot occur. The shared text buffer gets an `IOSimpleLock`,
since three independent timers can now land close together.

The log line says so in those words: with this shape, a missing sample means
**the timer did not fire**, which is a different and much stronger statement
than 2fx could make.

```
a=s2/L748  b=s2/L748    STOPPED - nothing logged in 20 s, and the 2fv window
                        (initComputeMQD(5) + one GPU submit) is a real location
a=s2/L748  b=s2/L961    CRAWLING - the stage has not moved but the thread is
                        still running code, and every endpoint reading in this
                        project is void for locating anything
a only, again           the timers themselves do not survive the boot, which
                        is then its own finding and needs a channel that is
                        not a thread_call
```

Build: UUID 3D6F354F, hosttest passes. Boot-args unchanged - `-rbgpunogates`
stays. The chain region still contains no flash commit of ours.

### 2fy ANSWERED on hardware (2026-10-14): THE THREAD GENUINELY STOPS

logs/20261014-201620-plugin, armed, build checked (3D6F354F == build/out),
`ttl_hw_init_ret=a1bb4001` so a new boot. **All three timers fired, on the
dot** - armed at 14535 ms, samples at +15000 / +35000 / +65000:

```
rbgpu-prog2 = a=s1/L671@29535ms  b=s1/L671@49535ms  c=s1/L671@79535ms
rbgpu-prog  = s=1 powerUpHW ENTER
log.txt     = 254 lines, last at 14.985418
```

**Identical stage AND identical log sequence at 29.5 s, 49.5 s and 79.5 s.**
Fifty seconds, and `g_rbgpuLogSeq` - which is bumped by *every* log append and
every dedupe hit - did not move by one.

**So the thread is STOPPED, not crawling.** That is the question 2fm asked on
2026-10-14 and that went unrun for six milestones while six structural
hypotheses were raised and refuted. It is now established by measurement
rather than assumed, and the alternative 2ez taught the project to fear -
slowness mistaken for a hang - is excluded here: a crawling thread logs.

Three more things the same variable settles:

- **The machine is alive at 79.5 s** and the `thread_call` pool is healthy -
  three independent timers fired to the millisecond while the chain thread was
  frozen. 2ez's pool-starvation reading does not apply to this configuration.
- **2fy's fix worked.** Three separate timers, none re-arming from its own
  handler, and all three landed where 2fw and 2fx lost every later shot. The
  2fx diagnosis (re-entering a `thread_call` from its own callback) is
  confirmed by the fix rather than by the inference alone.
- This boot stopped at **stage 1**, with the log's last line
  `2cx: waitForHwStamp -> 1 (the GPU executed it) (22292 ticks)` following
  `submitMapQueuesPacket ENTER (ring=0)` sixteen lines earlier. That is the
  2fv window exactly: after ring 0's stamp returns, before ring 5's mapQueues
  completes.

So the "endpoint" readings from 2eo onward were not meaningless after all -
they were recording a real stop. What stays withdrawn is the *location*, since
the endpoint varies; what is new is that there IS one to find.

### The latent deadlock this made worth reading for — found, and NOT the cause

With "stopped forever, no panic, no log" established as the shape, the hook
layer was read for anything that can block permanently. There is one:

```
rbgpu_diag_set_num()            IOLockLock(g_diagLock)
  rbgpu_diag_set_num_locked()     ... table full ...
    rbgpu_log()                   IOLockLock(g_diagLock)   <-- SAME THREAD
```

`g_diagLock` is an **`IOLock`**, which is not recursive: a thread blocking on
a lock it already holds blocks **forever**, silently. And the trigger fits the
variance - it is behind `static bool s_warned`, so it can fire at most once
per boot, and only when the diag table first reaches `RBGPU_MAX_DIAGS`.

**It is not what stopped this boot, and that was checked rather than assumed:**
the boot's own `RBGPUDiag` holds **246 of 320** entries, and the string
`DIAG TABLE FULL` appears in **no** log this project has ever captured. So
this is a live bug found by reading, with the table at 246 and climbing, fixed
in 2fz before it can fire - not a diagnosis of the frontier.

A second candidate was **refuted statically, before costing a boot**:
`rbgpu_log` calls `IOLog` while holding `g_diagLock`, and this plugin *routes*
`_IOLog` (milestone 2c). But `wrap_iolog` tags by `__builtin_return_address`
against the HWLibs / X6000 / HWSvc ranges only, so our own call gets
`tag == nullptr` and goes straight to `orig_iolog`. No recursion.

## 2fz (2026-10-14): a breadcrumb that takes no lock at all

Here is what every instrument in this family has had in common, and it took
"the thread is stopped" to make it worth saying: **every breadcrumb ever
placed inside the chain went through `rbgpu_log` or `rbgpu_diag_set_num`,
and both take `g_diagLock`** - or through the gate trail, which wrote flash.
Not one of them could be placed at a point in the chain without adding a lock
acquisition to that point.

`RB_STEP(n)` is a bare `volatile uint32_t` store. No lock, no log, no NVRAM,
no call. It cannot be what stops the thread, and the 2fy timers - now measured
to fire on the dot while the chain is frozen - read it from outside and carry
it in each sample as `S<n>`.

Markers, chosen to bracket exactly the span this boot died in:

```
10/11/12   waitForHwStamp: original returned / diag sets done / log done
20+ring    mapQueues: about to call Apple's original
30+ring    mapQueues: Apple's original RETURNED
40+ring    mapQueues: the two diag sets returned
50         initGraphicsMQD returned
90         doStart returned
```

```
a=s1/S12  b=s1/S12   the stop is in Apple's submitKIQFrame / mapQueues
                     epilogue, with nothing of ours in the span at all
a=s1/S30  b=s1/S30   the stop is in OUR two diag sets - which take g_diagLock,
                     and the refresh thread is the only other taker
a=s1/S4x / S50 / S90 it got further than the log did, and the log is what
                     stops rather than the thread
```

Also in 2fz: the table-full self-deadlock above is fixed. The warning is
raised through a `g_diagFullPending` flag and logged **after** the lock is
released, so no path can re-enter `g_diagLock` on its own thread.

Build: UUID E16B0804, hosttest passes. Boot-args unchanged - `-rbgpunogates`
stays. The chain region still contains no flash commit of ours, and now no
lock of ours either at the marked points.

## 2ga (2026-10-14): the 2fz boot PANICKED — twice, identically, and it is located

Two `.panic` files, the first real symbolicated reports this project has got
since 2026-10-11, at **20:24** and **20:33**, both with the 2fz build
(E16B0804) loaded. The user recovered with `-rbgpuoff`. Different KASLR
slides, identical backtrace, so this is **reproducible**:

```
panic(cpu N): Kernel trap, type = 14 = page fault
  error code 0x3        present | WRITE
  RIP = Lilu+0x1ae22 (boot A) / Lilu+0x1ae4a (boot B)
  CR2 = RIP - 11, also inside Lilu ; RAX = CR2 ; RBX=2 RDX=3 both boots

  Lilu   : Crypto::hash + 0xc12            <- the FAULTING frame
  X6000  : AMDHardware::startHWEngines + 0x3e
  plugin : hooked_hw_startHWEngines + 0x384
  X6000  : AMDHardware::powerUp + 0x76
  X6000  : AMDGFX10Hardware::powerUp + 0x1b
  X6000  : AMDNavi21Hardware::powerUp + 0x29
  X6000  : AMDGraphicsAccelerator::powerUpHW + 0x252
  X6000  : AMDGraphicsAccelerator::callPlatformFunction + 0x21f
  plugin : RembrandtGPUPeer::handlePostRegister + 0x81
  kernel : thread_call_delayed_timer
```

**The good half, and it is large.** `doStart` returned 1, every engine
powered up, `AMDHardware::powerUp` ran, and the thread reached
`startHWEngines` — **past the 2fv window** that every stopped boot since 2fs
died inside. Uptime at panic was 16.26 s, which is why `rbgpu-prog2` shows no
sample: the first 2fy timer fires at ~29 s. `rbgpu-prog` reads
`s=1 powerUpHW ENTER`, written by these boots and textually identical to the
2fy boot's, so there is no contradiction there.

It also confirms 2fr a third way, from Apple's own frames:
`handlePostRegister` runs on a **`thread_call_delayed_timer`** thread, with
`callPlatformFunction` -> `powerUpHW` inside it. Not `IOService::start`.

**Where it faults.** `startHWEngines+0x3e` is the `e->vtable[0x148]()` call
site (the FAIL branch is at +0x46 in the 2fo disassembly). So Apple's call
through an SDMA engine's `vtable[0x148]` **lands inside Lilu's `__TEXT`** and
page-faults on a *write* to Lilu's own text 11 bytes behind the instruction
pointer. `Crypto::hash` is simply the nearest preceding exported symbol; the
real address is in Lilu's patcher region.

**The milestone-2c rule is NOT what went wrong, and that was checked before
writing it down:**

```
$ nm -m build/re/AMDRadeonX6000 | grep GFX10SDMAEngine5startEv
000000000bea4a6a (__TEXT,__text) external __ZN33AMDRadeonX6000_AMDGFX10SDMAEngine5startEv
```

External and defined in X6000, so `solveSymbol` cannot have returned a bare
slide. 2fu's claim that the route was legitimate under that rule holds.

**Why Lilu's own code faults is NOT established and no guess is recorded.**
The one thing this project added that intercepts `vtable[0x148]` is 2fu's
route of that exact function, so 2ga puts it behind **`-rbgpusdmahook`**, off
by default — the 2eu/2fo move of removing the single suspect:

```
panic GONE     the route is the cause, established rather than suspected,
               and the question becomes why routing THIS function breaks
               where twenty others did not
panic ANYWAY   the route is exonerated and the fault is in Apple's own
               vtable[0x148] call with nothing of ours in it
```

Build: UUID 5C7753C4, hosttest passes. Boot-args unchanged; `-rbgpusdmahook`
is new and deliberately NOT added.

### 2ga answered on hardware (2026-10-15): NO PANIC, and the chain ran to the END

logs/20261015-003756-plugin, `plugin_check` ran **in the armed boot**
(`rbgpu-diag` armed=1, `ttl_hw_init_ret=ffffffffe135d001`, a new boot), build
checked first per the 2cu/2cv rule: 5C7753C4 == build/out (2ga). `pci.txt` is
**565485 bytes** and `peer.plist` 86 KB - the most complete armed capture this
project has ever taken.

```
newest .panic -> Kernel-2026-10-14-203313   (the 2fz boots; nothing new)
rbgpu-prog    -> s=5 0x06 done ret=0; 0x1B SKIPPED (powerUpHW failed)
rbgpu-prog2   -> a=s4/S90/L820@28793ms b=s4/S90/L821@48793ms c=s4/S90/L822@78793ms
```

**2ga's first branch: the panic is GONE.** With `-rbgpusdmahook` absent the
2fu route of `AMDGFX10SDMAEngine::start` is not installed, and the two
identical page faults at `startHWEngines+0x3e` did not recur. So **that route
was the cause**, established rather than suspected.

And with the route out of the way the whole chain ran, further than any boot
in this project:

```
[13.792] handlePostRegister: sending Request 0x06 (PowerPlay Ready)
[13.792] 2fs: prog s=1 powerUpHW ENTER
[16.214] 2ce: doStart returned 1
[16.232] 2bu: AMDGFX10PM4Engine::powerUp -> 1
[16.241] engine[0] -> 1  [1] -> 1  [2] -> 1  [3] -> 1  [4] -> 1  [8] -> 1  [9] -> 1
[16.405] 2bv: all engines powered up OK
[16.414] 2bs: AMDHardware::startHWEngines entered
[16.424] 2fp: startHWEngines will walk 4 entries (hw+0xc2 = 1)
[16.444] 2fp: engine[0..3] all non-NULL, vtable[0x148] = 0xffffff7fa4d8da6a (all four)
[16.493] 2fs: prog s=3 c2=1 n=4 map=.... allok
[16.502] 2bs: AMDHardware::startHWEngines -> 0
[16.511] 2fs: prog s=4 ... ret=0 sdma[0]{}
[16.543] 2br: powerUpHW returned 0x0
[16.559] handlePostRegister: Request 0x06 returned 0x00000000
[16.575] handlePostRegister: powerUpHW FAILED, so Request 0x1B is NOT sent
```

- **`hw->0xc2 = 1`, count 4, all four entries live, and all four share one
  `vtable[0x148]`** - which confirms 2fu's static reading from the chip: the
  window holds the four SDMA engines and the slot is their `start()`.
- The NULL check at 0xbea9068 cannot fire, so the FALSE comes from one of the
  four **calls**. 2fq's "map all '.'" branch.
- **2fr's 0x1B skip worked.** The request the 2fo boot sat on for minutes was
  not sent, and the thread returned instead of wedging.
- **No stall anywhere.** All three 2fy samplers fired on the dot and read
  `S90` (doStart returned) with `L820 -> 821 -> 822`: the log counter advancing
  by one per 20 s, which is the request-0x14 poll's dedupe bump on an idle,
  living machine. Fifty seconds of proven liveness with the chain complete.

One cosmetic defect of my own, found in the same record: the samples read
`s=4` while `rbgpu-prog` reads `s=5`, because the 0x1B notes in
RembrandtGPUPeer.cpp call `rbgpu_nvram_note` without setting
`g_rb_prog_stage`, which only the kern_start helper updates. Harmless, and
fixed in 2gb - a record that disagrees with itself is a trap for a later
reader.

## 2gb (2026-10-15): why routing THAT function panics, and the answer without it

2ga asked, if the panic went away, "why does routing THIS function break where
twenty others did not". It is in the first six bytes, and no boot is needed:

```
bea4a6a: 80 7f 10 00   cmpb $0x0, 0x10(%rdi)
bea4a6e: 74 03         je   0xbea4a73          <- rel8, displacement +3
bea4a70: 31 c0         xorl %eax, %eax
bea4a72: c3            retq
bea4a73: 55            pushq %rbp              <- the branch target
```

An x86-64 route needs **at least 5 bytes** at the entry for its jump, so the
patched window covers that `je` at +4, and Lilu copies the displaced
instructions byte-for-byte into a trampoline so `orig` can be called. `74 03`
is a **short conditional branch whose displacement is relative to its own
address**: copied to a trampoline elsewhere it still reads "+3", so it no
longer lands on `0xbea4a73` but three bytes further into Lilu's own trampoline
buffer, and whatever is there is executed.

That accounts for **every** field of the two panic reports, which is what
makes it a finding rather than a seventh hypothesis:

| observed | explained by |
|---|---|
| RIP inside Lilu's `__TEXT` (`Crypto::hash+0xc12`, the nearest preceding symbol) | execution inside Lilu's trampoline region |
| error 0x3 = present \| **WRITE**, CR2 = RIP - 11, also in Lilu | garbage bytes executed as a store |
| caller `startHWEngines+0x3e`, the `vtable[0x148]` call site | the very first entry into the routed function |
| identical at two different KASLR slides | a wrong displacement is slide-independent |
| `nm -m` shows the symbol external and defined in X6000 | the milestone-2c rule was NOT violated, as 2ga already checked |

**And it says why the other routes were fine.** Every one of them begins with
straight-line setup (`pushq %rbp; movq %rsp,%rbp; ...`). This is the first
function routed whose prologue carries a relative branch inside the patch
window. General rule, worth keeping beside the milestone-2c rule: **before
routing a symbol, disassemble its first 16 bytes and refuse if a rel8/rel32
branch begins within the first 5.**

### The answer without the route: Apple's own started-flag

The route stays off. The question it was for - which of the four SDMA engines
refuses - is answered by reading Apple's own bookkeeping, because `start()`
sets `this->0x10 = 1` **only** on its success path (0xbea4b49) and
`startHWEngines` breaks out of its walk at the **first** FALSE. So after the
original returns, the flags name the failing index exactly: the engines before
it read 1, it and everything after it read 0.

Plain loads from pointers the 2fp walk already reads. Nothing patched, nothing
routed, no trampoline that can be wrong.

- In the ENTER walk, each engine's `+0x10` (started), `+0x48` (queue count),
  `+0x50` (channel array) and `+0x1a0`/`+0x1a4` (the command ids) are logged
  and the first two go into `sdma<i>_q` / `sdma<i>_cmd`. A `+0x48` of 0 means
  that engine takes the trivial-TRUE path and **cannot** be the one that
  refuses, which the log says in those words.
- A new post pass after the original builds `pre=0000 post=1100 fail@2` and
  rides out on the existing stage-4 note, so a boot that loses its log still
  answers.

```
fail@N          engine[N] is the one Apple's walk refused at, and its own
                queue count and command id are in the same record
allstarted      every start() returned TRUE and startHWEngines' FALSE comes
                from OUTSIDE the loop (0xbea906b onward) - a different
                question, and one nothing has looked at
q=0 everywhere  every engine takes the trivial-TRUE path, so the FALSE is not
                in the queue create at all
```

Also fixed: `g_rb_prog_stage` is now set at the stage-5 and stage-6 notes.

Build: UUID E119D29C, hosttest passes. Boot-args unchanged - `-rbgpunogates`
stays, `-rbgpusdmahook` stays OUT and is now proven rather than suspected.

### 2gb answered on hardware (2026-10-15): `fail@1` — engine[1] is where Apple's walk breaks

logs/20261015-005032-plugin, `plugin_check` ran **in the armed boot**
(`rbgpu-diag` armed=1, `ttl_hw_init_ret=ffffffffc46d3001`, a new boot), build
checked first per the 2cu/2cv rule: E119D29C == build/out (2gb). No panic
(newest is still 2026-10-14 20:33). `pci.txt` 567801 bytes, `peer.plist`
87 KB - a second complete armed capture in a row.

```
rbgpu-prog  -> s=5 0x06 done ret=0; 0x1B SKIPPED (powerUpHW failed)
rbgpu-prog2 -> a=s5/S90/L834@28999ms b=s5/S90/L835@49000ms
```

```
[16.204] 2bs: AMDHardware::startHWEngines entered
[16.214] 2fp: startHWEngines will walk 4 entries (hw+0xc2 = 1)
[16.244] 2gb: engine[0] started(+0x10)=0 queues(+0x48)=2 chans=0xffffff9042087400 cmd=0x7/0x0
[16.283] 2gb: engine[1] started(+0x10)=0 queues(+0x48)=2 chans=0xffffff9040c72c00 cmd=0x7/0x1
[16.321] 2gb: engine[2] started(+0x10)=0 queues(+0x48)=2 chans=0xffffff9042087a00 cmd=0x7/0x2
[16.357] 2gb: engine[3] started(+0x10)=0 queues(+0x48)=2 chans=0xffffff9042087e00 cmd=0x7/0x3
[16.401] 2bs: AMDHardware::startHWEngines -> 0
[16.417] 2gb: pre=0000 post=1000 fail@1
```

**The started-flag read works and it names the engine.** Three things in one
boot, none of them needing the route that panicked:

- **`pre=0000`**: all four flags are 0 before the walk, so no engine was
  pre-started. That confirms **on hardware** what 2fu refuted statically - the
  SDMA `powerUp` (`vtable[0x138]` = `movb $0x1,%al ; retq`) really does touch
  nothing, and the FALSE is not a double-call artefact.
- **`post=1000 fail@1`**: engine[0]'s `start()` returned TRUE and set its own
  `+0x10`; engine[1]'s returned FALSE and Apple's walk broke there. So **one
  SDMA engine starts and the second does not.**
- **`queues(+0x48) = 2` on every engine**, so none takes the trivial-TRUE path
  at 0xbea4b49 - the loop really runs, and the FALSE is a real refusal from
  `svc->vtable[0x120]`.

`sdma[0]{}` in the stage-4 note is the 2fu per-call record, empty because that
route is off by design.

## 2gc (2026-10-15): the chip has ONE SDMA instance and Apple asks for four

The discriminator is the **instance index**, and three independent things say
so without a single boot spent:

**1. This machine's own IP discovery table says one.** From the first probe,
2026-10-03 (logs/20261003-184349/probe.plist):

```
SDMA0(42)  inst 0  v5.2.3.0
PCIE(70)   inst 0  /  PCIE(70)   inst 1
SMUIO(4)   inst 0  /  SMUIO(4)   inst 1
UMC(150)   inst 0  /  UMC(150)   inst 1
```

The parser does enumerate multiple instances when they exist - three IPs here
have two each. **SDMA has exactly one.**

**2. The count is a per-ASIC constant, and the second data point matches real
silicon.** `hw->0xc2` is written by one instruction, and Navi 23 is the
control:

```
Navi21Hardware::setupAndInitializeHWCapabilities  0xbeca1d9
    movw $0x101, 0xc1(%rbx)     -> +0xc2 = 1 -> count 0xc2*2+2 = 4
Navi23Hardware::setupAndInitializeHWCapabilities  0xbed26ad
    movw $0x100, 0xc2(%rbx)     -> +0xc2 = 0 -> count = 2
```

Navi 21 (6800/6900) has 4 SDMA instances; Navi 23 (6600) has 2. The formula
reproduces both. And the **allocation** side computes the same thing from the
other direction - `Navi21Hardware::allocateHWEngines` (0xbeca1f0):

```
beca1ff  new Navi21PM4Engine              -> hw->0x3B0
beca224  new SDMAEngine_GC_10_3_0         -> hw->0x3B8     UNCONDITIONAL
beca249  new SDMAEngine_GC_10_3_0                          UNCONDITIONAL
beca269  cmpb $0x1, 0xc2(%rbx) ; jne 0xbeca2b4
beca277  new SDMAEngine_GC_10_3_0         only when 0xc2 == 1
beca298  new SDMAEngine_GC_10_3_0         only when 0xc2 == 1
beca2b4  new VCN3HWEngine x2
```

Two unconditional plus two gated - exactly `0xc2*2+2`, from the constructor
rather than the walk. Also confirms 2fw's `amd-log.txt` layout reading
(`0x3B0[0]` PM4, `0x3B0[1..4]` = `0x3B8[0..3]` SDMA, `0x3B0[8..9]` VCN) from
the code that builds it.

**3. The failing argument IS the instance**, read from the call site rather
than guessed:

```
bea4aee  movl 0x1a0(%rbx,%r12,8), %esi     arg1
bea4af6  movl 0x1a4(%rbx,%r12,8), %edx     arg2
bea4b0c  callq *0x120(%rax)
bea4b12  testl %eax,%eax ; jne -> FAIL
```

So `engine+0x1a0` is an array of `{u32,u32}` pairs, **stride 8, indexed by the
queue index** - 2gb's single read happened to be pair[0], and is now read as
such. Measured pair[0] across the four engines: `(7,0) (7,1) (7,2) (7,3)` - a
constant command 7 and the engine index. engine[1] asks the layer below for
SDMA **instance 1**.

**And a structural fact worth stating plainly: there is no count that fits
this chip.** `0xc2*2+2` is always even and at least 2, so Apple's Navi 21 path
cannot walk exactly one SDMA engine. Setting `+0xc2 = 0` would halve the
problem and still fail at engine[1]. The count is not the lever.

### The test: one variable

For every engine after the first, rewrite arg2 to 0 so each queue create asks
for instance 0. A field write to Apple's own objects, not a code patch - the
same class as 2di's `ctx+0x340` zero - and **off unless `-rbgpusdmainst0`**.
Every queue's pair is logged before and after, with the read-back.

**What it is NOT: a fix.** Four engine objects would each hold queues on SDMA
instance 0, which is only tolerable because an armed boot has no GPU clients
and nothing submits SDMA work. If it works, the real fix is to make Apple
build one SDMA engine, and that is a separate question.

**0x1B is held back for this boot even if `powerUpHW` now succeeds.** Without
that, the one boot answering the SDMA question would also be the first ever to
send 0x1B with the hardware fully brought up - two variables at once, which is
what 2fb/2fc and the withdrawn 2ev table were each made of. Sending it becomes
a deliberate second step with `-rbgpuforce1b`.

```
post=1111 allstarted   the instance index was the whole discriminator,
                       established on hardware, and startHWEngines returns
                       TRUE for the first time in this project
post=1000 fail@1 again the instance is NOT it, the refusal is about the engine
                       object or its channels, and the three supports above
                       owe an explanation
a partial flip         the per-engine flags say exactly how far it got
```

Build: UUID 4C4BA282, hosttest passes. **One new boot-arg**, `-rbgpusdmainst0`,
and it is the user's call whether to arm it; `-rbgpunogates` stays,
`-rbgpusdmahook` stays out.

### 2gc answered NOTHING on hardware (2026-10-15): the note channel loses writes

The armed boot ran — build and arming checked first, per the 2cu/2cv rule —
and its answer was not captured.

```
kmutil showloaded -> 4C4BA282 == build/out (2gc)
boot-args          -> ... -rbgpunogates -rbgpusdmainst0     (the arg IS set)
rbgpu-diag         -> d=0 armed=0                           the recovery boot
rbgpu-diag-prev    -> d=70 armed=1 ttl_hw_init_ret=ffffffffcd567001
                      (2gb read c46d3001, so this IS a new boot)
no new logs/ dir   -> no plugin_check capture from the armed boot
no .panic          -> newest is still 2026-10-14 20:33
rbgpu-prog         -> "s=5 0x06 done ret=0; 0x1B SKIPPED (powerUpHW failed)"
rbgpu-prog2        -> "a=s1/L671@29535ms b=s1/L671@49535ms c=s1/L671@79535ms"
```

**`rbgpu-prog2` is the 2fy boot's value — two boots stale — and that is a
measurement, not a suspicion.** Both of the last two boots captured their own
prog2 in the armed boot, and both reverted after the power cycle:

```
2ga in-boot nvram.txt   a=s4/S90/L820@28793ms b=...@48793 c=...@78793
2gb in-boot nvram.txt   a=s5/S90/L834@28999ms b=...@49000
live read now           a=s1/L671@29535ms b=...@49535 c=...@79535   (2fy)
```

So **a note's write can fail to survive the power cycle.** The cause is NOT
established and no guess is recorded here.

**Which makes `rbgpu-prog` uninterpretable this boot, and that is the whole
result.** It reads the 2gc build's *failure* text, which this boot could have
written (the rewrite did not help) or 2gb could have left behind (identical
text). Nothing in the record distinguishes them. Worth noting the two
variables are not stale from the same boot — prog2 is 2fy's while prog is
2ga-or-later — so a single stale-image explanation does not cover it, and the
honest position is that prog's provenance is simply unknown.

`starthw_c2` is also absent from the prev summary where 2gb's had it, which
would put the freeze before the walk; but that channel has not reported the
frontier since 2fa, so it is not evidence.

**Twentieth instance of the recurring error, and it is retroactive.** The 2fu
third boot read `s=1 powerUpHW ENTER` with no capture and was written up as
this-boot evidence — while the boot before it had written the same text. That
reading is now withdrawn too. An overwrite variable read after a power cycle
is this boot's value only if something establishes it was rewritten this boot,
and nothing ever did.

## 2gd (2026-10-15): a note that says which boot wrote it

Every note now carries a per-boot sequence number. The first note of a boot
reads the counter out of `rbgpu-boot`, increments it, writes it back, and all
notes are prefixed `b=<n>`:

```
rbgpu-boot  = 38
rbgpu-prog  = b=38 s=4 c2=1 n=4 map=.... allok ret=1 pre=0000 post=1111 ...
```

A read whose `b=` equals the one recorded last turn is **stale, full stop** -
no inference, no ambiguity, and it costs one extra NVRAM property per boot.
`tools/plugin_check.sh` captures `rbgpu-boot` first.

This does not fix the lost commit; it makes a lost commit detectable, which is
what the last three boots needed and did not have. It also retires the
"absent/identical means X" reading for good: from here, a note is evidence
only when its `b=` is new.

Nothing else changes - the 2gc instance rewrite stays exactly as built, still
behind `-rbgpusdmainst0`, and the question it asks is unchanged and unanswered.

Build: UUID 866EF957, hosttest passes. Boot-args unchanged - `-rbgpunogates`
and `-rbgpusdmainst0` both stay.

**And the procedure matters more than the build this time.** Both readable
boots in this family (2ga, 2gb) were readable because `plugin_check` ran **in
the armed boot**, which yields the full log and a 560 KB `pci.txt`. The three
unreadable ones had no capture. So: run it in the armed boot, before any
reboot.

### 2gc/2gd answered on hardware (2026-10-15): the instance index is NOT the discriminator

logs/20261015-011214-plugin, `plugin_check` ran **in the armed boot**
(`rbgpu-diag` armed=1, `ttl_hw_init_ret=ffffffffb5a64001`, a new boot), build
checked: 866EF957 == build/out (2gd). No panic. 64 KB log, `pci.txt` 565795.

**2gd's nonce worked on its first boot and it is what makes this readable:**

```
rbgpu-boot  = 1
rbgpu-prog  = b=1 s=5 0x06 done ret=0; 0x1B SKIPPED (powerUpHW failed)
rbgpu-prog2 = b=1 a=s5/S90/L849@29577ms b=s5/S90/L850@49576ms
```

`b=1` is a sequence never recorded before, so both notes are **this** boot's
with no inference - exactly the ambiguity that made the 2gc boot a write-off.

**The rewrite landed and nothing moved:**

```
2gc: engine[1] queue[0] arg2 0x1 -> 0x0 (read back 0x0)
2gc: engine[1] queue[1] arg2 0x1 -> 0x0 (read back 0x0)
     ... the same for engine[2] and engine[3], six patches, all read back
2bs: AMDHardware::startHWEngines -> 0
2gb: pre=0000 post=1000 fail@1          inst0=6
```

So 2gc's second branch fired: **the instance index is not what engine[1] is
refused for**, and the three supports it rested on owe an explanation. The
chip still has one SDMA instance and Apple still builds four engines - those
measurements stand - but that is not the discriminator.

**And the same boot establishes something stronger than the question asked.**
Reading every queue pair rather than just the first:

```
engine[0]  queue[0] = (0x7, 0x0)   queue[1] = (0xa, 0x0)
engine[1]  queue[0] = (0x7, 0x1)   queue[1] = (0xa, 0x1)   -> both arg2 -> 0
engine[2]  queue[0] = (0x7, 0x2)   queue[1] = (0xa, 0x2)   -> both arg2 -> 0
engine[3]  queue[0] = (0x7, 0x3)   queue[1] = (0xa, 0x3)   -> both arg2 -> 0
```

- **`arg1` is not a constant.** It is 0x7 for queue[0] and 0xa for queue[1] on
  every engine, so 2gc's "a constant command 7 and the engine index" was an
  artefact of reading pair[0] alone. Corrected in the log line, which now says
  `pair[0]=` rather than `cmd=`.
- **With the rewrite on, engine[1] sent `(0x7,0)` and `(0xa,0)` - byte-identical
  to engine[0]'s, which succeeded - and still returned FALSE.** Identical
  arguments, one succeeds, the next fails. **The refusal cannot be about the
  arguments at all.**

**The confound 2gc created and did not name.** It warned that four engines
holding queues on instance 0 was a *safety* matter; it is also a confound,
because a queue-create layer refusing the second request for a queue that
already exists is expected for reasons that have nothing to do with
instances. So this boot does not cleanly refute the instance theory either -
it refutes "arg2 is what decides", which is a different and better-supported
statement.

## 2ge (2026-10-15): make engine[1] the FIRST to ask

`start()` has a trivial-TRUE path when `this->0x48 == 0` (0xbea4a94 ->
0xbea4b49): it sets the started flag, calls `vtable[0x278]` and returns TRUE
**without asking the layer for anything**. So writing 0 into engine[0]'s queue
count makes **engine[1] the first engine in the boot to create a queue**, with
its own untouched `(0x7, 1)` and `(0xa, 1)`.

That separates the two readings the duplicate conflated:

```
post=1100 or further   engine[1] succeeds once it asks FIRST, so the failure
                       is ORDINAL - the layer allows one SDMA engine's queues
                       and refuses the next whatever instance it names, and
                       instance 1 is fine
post=1000 fail@1 again it fails even asking first with instance 1, so the
                       instance really is absent and 2gc's supports stand -
                       the duplicate was masking a genuine refusal
post=0000 fail@0       the skip broke engine[0], which Apple's own
                       trivial-TRUE path should make impossible and would say
                       the field read is wrong
```

Off unless **`-rbgpusdma0skip`**, and `-rbgpusdmainst0` must come **out** -
one variable. A field write to Apple's own object like 2gc's and 2di's;
engine[0] contributing no SDMA queues is tolerable only because an armed boot
has no GPU clients and nothing submits SDMA work. The old count is logged and
rides out on the stage-4 note as `skip0=N`.

0x1B stays held: the hold now triggers on either SDMA boot-arg, so a boot that
finally passes `powerUpHW` does not also become the first to send it.

Build: UUID 2D8B1F7C, hosttest passes.

### 2ge answered HALF on hardware (2026-10-15): startHWEngines still returns 0

Build and arming checked first, per the 2cu/2cv rule. No `plugin_check`
capture from the armed boot (newest log dir is still the 2gd boot's), so the
notes are the whole record — and this time the nonce makes one of them count.

```
kmutil showloaded -> 2D8B1F7C == build/out (2ge)
boot-args          -> ... -rbgpunogates -rbgpusdma0skip   (and NO -rbgpusdmainst0)
rbgpu-boot         -> 2
rbgpu-prog         -> b=2 s=5 0x06 done ret=0; 0x1B SKIPPED (powerUpHW failed)
rbgpu-prog2        -> a=s1/L671@29535ms ...   (no b= prefix at all, so pre-2gd)
no .panic          -> newest is still 2026-10-14 20:33
```

**`b=2` is a sequence never recorded before, so `rbgpu-prog` IS this boot's**,
and the boot-arg swap took. What it establishes, with no inference:

- **`powerUpHW` failed, so `startHWEngines` returned 0 again.** Had the skip
  made the walk succeed, 2gc's hold would have written
  `s=5 powerUpHW OK; 0x1B HELD` instead. **The engine[0] skip did not make
  the walk pass.**

**What it does NOT establish, and the fault is mine.** 2ge's decision table
needs the `post=` flags, and both of its branches — `post=1100` (engine[1]
succeeded once asking first, engine[2] then failed) and `post=1000 fail@1`
(engine[1] failed even asking first) — produce `ret=0` and therefore the exact
same stage-5 text. The map lived in the stage-4 note, and **stage 5 always
follows and overwrites it**, because `rbgpu-prog` is a single overwrite slot
carrying both the progress counter and the answer. One variable, two purposes:
the 2fq mistake in a new place, and it cost this boot's answer.

Not claimed: that `rbgpu-prog2`'s staleness is another lost write. It carries
no `b=` at all, so it predates 2gd — and the samplers fire at ~29/49/79 s of
uptime, so a boot rebooted before 29 s simply never writes it. With no in-boot
capture the two are indistinguishable, unlike the 2gc case where the in-boot
`nvram.txt` held the new value and the post-cycle read did not.

## 2gf (2026-10-15): the engine map gets its own variable

One line. The stage-4 string now also goes to **`rbgpu-sdma`**, which no later
stage overwrites, so the map survives the stage-5 note that always follows it:

```
rbgpu-boot = 3
rbgpu-prog = b=3 s=5 ...                        the progress counter
rbgpu-sdma = b=3 c2=1 n=4 map=.... allok ret=0 pre=0000 post=.... skip0=2
```

`tools/plugin_check.sh` captures it between `rbgpu-prog` and `rbgpu-prog2`.

Nothing else changes: the 2ge skip stays exactly as built behind
`-rbgpusdma0skip`, the 2gc rewrite stays behind `-rbgpusdmainst0` (still out),
and the question 2ge asked is unchanged and now answerable from a boot with no
log at all.

```
post=1100 or further   engine[1] succeeds once it asks FIRST - the failure is
                       ORDINAL and instance 1 is fine
post=1000 fail@1       it fails even asking first, so the instance really is
                       absent and 2gc's three supports stand
post=0000 fail@0       the skip broke engine[0], which Apple's own
                       trivial-TRUE path should make impossible
skip0 absent           the skip never ran and the boot says so, rather than
                       leaving it to be inferred
```

Build: UUID 8BBC5A21, hosttest passes. Boot-args unchanged from the 2ge boot —
`-rbgpunogates` and `-rbgpusdma0skip` stay, `-rbgpusdmainst0` stays out.

### 2gf answered on hardware (2026-10-15): the failure is NOT ordinal

logs/20261015-012845-plugin, `plugin_check` ran **in the armed boot**
(`rbgpu-diag` armed=1, 75 KB log, `pci.txt` 567699), build checked first per
the 2cu/2cv rule: 8BBC5A21 == build/out (2gf). No panic (newest is still
2026-10-14 20:33).

```
rbgpu-boot = 3                                   a sequence never recorded before
rbgpu-prog = b=3 s=5 0x06 done ret=0; 0x1B SKIPPED (powerUpHW failed)
rbgpu-sdma = b=3 c2=1 n=4 map=.... allok ret=0 pre=0000 post=1000 fail@1
             inst0=0 sdma[0]{} skip0=2
```

**2gf's one-line fix worked**: the engine map survived the stage-5 note that
destroyed it last boot, and `b=3` makes it this boot's with no inference. The
log confirms the same string independently.

```
[15.543045] 2gc: SDMA instance rewrite OFF (-rbgpusdmainst0 not set)
[15.543046] 2ge: engine[0] queues(+0x48) 2 -> 0 (read back 0)
[15.543058] 2gb: engine[1] started=0 queues=2 pair[0]=(0x7,0x1)
[15.543060] 2gc: engine[1] queue[0] pair = (0x7, 0x1)  queue[1] = (0xa, 0x1)
[15.543114] 2bs: AMDHardware::startHWEngines -> 0
[15.543118] 2gb: pre=0000 post=1000 fail@1
```

`skip0=2` says the skip ran, and `post=1000` is engine[0] taking Apple's own
trivial-TRUE path (0xbea4b49) and setting its flag without asking the layer
for anything. So **engine[1] was the first engine in the boot to create a
queue, asked with its own untouched `(0x7,1)` / `(0xa,1)`, and was still
refused.** 2ge's second branch: the refusal is **not ordinal** — it is not
"the layer allows one SDMA engine and refuses the next".

### But the 2x2 is incomplete, and the missing cell is the whole question

Three cells are now measured and **every single-variable change away from the
working one fails**:

| arg2 (instance) | order | engine | result |
|---|---|---|---|
| 0 | first | engine[0] | **TRUE** (every boot) |
| 0 | second | engine[1] (2gd) | FALSE |
| 1 | first | engine[1] (2gf) | FALSE |
| **0** | **first** | **engine[1]** | **never measured** |

That last cell is the one that separates the two surviving explanations, and
nothing in 2gd or 2gf can stand in for it:

```
engine[1] asking FIRST for instance 0 SUCCEEDS
    the discriminator really is the instance index, and 2gd's failure was
    exactly the duplicate confound 2gc feared - a layer refusing a second
    queue on an instance that already has one. The fix is then to make Apple
    build ONE SDMA engine, which is a separate question with a known shape.
it still FAILS
    the request is byte-identical to engine[0]'s working one AND in the same
    ordinal position, so neither the arguments nor the order can be what
    decides. What is left is the engine OBJECT - its channel array at +0x50,
    whose ch->0x48 and ch->0xd0 go into the descriptor at 0xbea4ad4.
```

## 2gg (2026-10-15): the missing cell costs no code, and the fail branch is pre-answered

**The experiment is boot-args only.** Both switches already exist and are
independent in the code - `rbgpu_sdma0_skip_2ge` returns unless `idx == 0`
(kern_start.cpp:4943) and `rbgpu_sdma_inst0_2gc` returns unless `idx != 0`
(:4963) - so setting both makes engine[0] ask for nothing and engine[1] ask
first for instance 0. The 0x1B hold fires on either arg, so a boot that
finally passes `powerUpHW` still does not become the first to send 0x1B.

The one code change is **read-only logging in a block that already runs
identically**, so it adds nothing to the outcome being measured (the `post=`
flags). Apple builds the queue descriptor from the channel, not from the
arguments (0xbea4aa7-0xbea4ad4: `ch = this->0x50[qi]`, `desc = { 0x40000,
ch->0x48, ch->0xd0 }`), and those two fields are the only part of the request
the pairs do not carry. Logging them per queue means that if the missing cell
still fails, the next question is already half-answered rather than costing
another boot. A NULL channel says so in those words instead of printing a
zero that could be read as a measurement.

Build: UUID 0E9563CF, hosttest passes. **One boot-arg goes back in**, and it
is the user's call: `-rbgpusdmainst0` alongside `-rbgpusdma0skip`.
`-rbgpunogates` stays, `-rbgpusdmahook` stays out.

```
post=1100 or further   instance 1 was the whole discriminator; the real fix is
                       making Apple build one SDMA engine
post=1000 fail@1       a byte-identical request in the same ordinal position
                       is still refused, so it is the engine object, and the
                       logged ch+0x48 / ch+0xd0 name where to look next
post=0000 fail@0       the skip broke engine[0], which Apple's own
                       trivial-TRUE path should make impossible
skip0 / inst0 absent   a switch did not run and the boot says which
```

### 2gg PANICKED on hardware (2026-10-15): my own descriptor read, and the instruction I misread

Kernel-2026-10-15-103535.panic, build `0E9563CF` = build/out (2gg), uptime
16.87 s. The faulting frame is **ours**:

```
Kernel trap, type = 14 = page fault
CR2 = 0x0000000000000049   Error code 0x0   (a READ of a non-present page)
RBX = 1   RAX = 1
plugin : hooked_hw_startHWEngines + 0x2d8
X6000  : AMDHardware::powerUp + 0x76 -> GFX10 -> Navi21 -> powerUpHW
plugin : RembrandtGPUPeer::handlePostRegister + 0x81
kernel : thread_call_delayed_timer
```

**The cause is one misread instruction, and it is the read 2gg added.** Apple's
channel fetch is

```
bea4aa7: 4e 8b 6c e3 50    movq 0x50(%rbx,%r12,8), %r13
```

with `%rbx` = the engine and `%r12` = the queue index — so the channel
pointers are an **inline array inside the engine object**,
`ch = *(void **)(engine + 0x50 + qi*8)`. 2gg's comment wrote that as
`this->0x50[qi]` and the code implemented it as
`(*(void **)(ep + 0x50))[qi]`: it took the pointer AT +0x50, which is
channel[0] itself, and then indexed **into channel[0]'s own fields**. For
`qi=1` that reads `channel[0]+8`, which held **1**, and `1 + 0x48` is exactly
the `CR2 = 0x49` the report names. Every field of the panic is accounted for.

So the walk's long-standing `chans(+0x50)=0xffffff90...` label was itself
misleading — that value is **channel[0]'s pointer**, not an array base — and
the label is what the mistake was built on.

**Two things this also says, neither of them good.** The panic wrote no note
at all: `rbgpu-boot` still reads **3**, the 2gf boot's, although stage 1
(`powerUpHW` ENTER) runs ~3 s before the fault — so a panic can lose a
commit that the 2gd nonce would otherwise have made readable. And
`rbgpu-arm` read **1** afterwards with no `-rbgpuoff` in boot-args, so the
machine was left armed for a repeat.

**And the 2gg question is unanswered.** The walk runs in the `startHWEngines`
ENTER hook, before Apple's original and before the 2gc/2ge field writes for
that engine, so engine[0]'s own loop faulted: nothing was patched, no
`post=` flags exist, and the chip's state was not changed by the attempt.

## 2gh (2026-10-15): the read corrected, guarded, and taken off the default path

Three changes, in the order they matter:

- **The arithmetic matches the instruction**: `ch = *(void **)(ep + 0x50 +
  qi*8)`, read off `bea4aa7` rather than off my own prose.
- **A NULL check is not a pointer check.** The value that faulted was `1`, so
  the guard is now "is this a kernel VA" — this chip's are all
  `0xffffff........`, so anything below `0xffffff8000000000` is logged as
  **not dereferenced** and as saying nothing, rather than being read.
- **The whole descriptor read is behind `-rbgpusdmadesc`, off by default.**
  2gg's own reasoning for putting it in the default path was that it "adds
  nothing to the outcome being measured". That was true of its effect on the
  `post=` flags and false of its effect on the boot, which is the thing that
  actually matters, and it is the second time in this family that an
  instrument killed the boot it was riding (2fz/2ga was the first). The
  boot-args 2x2 is the question; it now runs with nothing new in the path.

So the next armed boot is **exactly the 2gg experiment with the new read
switched off**: `-rbgpusdma0skip` + `-rbgpusdmainst0` together, engine[0]
asking for nothing and engine[1] asking FIRST for instance 0.

Build: UUID D27034BD, hosttest passes. Boot-args unchanged from the 2gg plan —
`-rbgpunogates`, `-rbgpusdma0skip`, `-rbgpusdmainst0`; `-rbgpusdmadesc` is new
and deliberately NOT added, `-rbgpusdmahook` stays out.

```
post=1100 or further   instance 1 was the whole discriminator; the real fix is
                       making Apple build one SDMA engine
post=1000 fail@1       a byte-identical request in the same ordinal position
                       is still refused, so it is the engine object - and THEN
                       the descriptor read is worth its own boot, with
                       -rbgpusdmadesc, now that it is correct and guarded
post=0000 fail@0       the skip broke engine[0], which Apple's own
                       trivial-TRUE path should make impossible
skip0 / inst0 absent   a switch did not run and the boot says which
```

### 2gh answered on hardware (2026-10-16): `post=1100` — engine[1] STARTED

Read from the recovery boot that followed; no `plugin_check` capture, so the
NVRAM notes are the whole record — and the 2gd nonce is what makes them
count. Build and arming checked first, per the 2cu/2cv rule:

```
kmutil showloaded -> D27034BD == build/out (2gh)
boot-args         -> ... -rbgpunogates -rbgpusdma0skip -rbgpusdmainst0
                     (no -rbgpuoff: the boot WAS armed)
rbgpu-boot        -> 4        a sequence never recorded before
rbgpu-sdma        -> b=4 c2=1 n=4 map=.... allok ret=0 pre=0000 post=1100
                     fail@2 inst0=6 sdma[0]{} skip0=2
rbgpu-prog        -> b=4 s=5 0x06 done ret=0; 0x1B SKIPPED (powerUpHW failed)
rbgpu-prog2       -> b=4 a=s5/S90/L852@27688ms
no new .panic     -> newest is still 2026-10-15 10:35 (which is 2gg's)
```

**The bit order was checked against the code that writes it before the value
was read**, because reading a map backwards is exactly the 2dt shape:
`rbgpu_poststarthw_2gb` emits `post` left-to-right in index order, one digit
per engine's own `+0x10` started-flag. So `post=1100 fail@2` is engine[0] and
**engine[1]** both started, engine[2] the first with its flag still 0.

`skip0=2` says engine[0]'s queue count was zeroed and it took Apple's
trivial-TRUE path; `inst0=6` says six arg2 rewrites landed (engines 1-3, two
queues each). So **engine[1] was the first engine in the boot to ask the
layer for a queue, asked for instance 0, and SUCCEEDED.**

**And one note of mine from the 2gg panic is corrected by the record rather
than by argument.** The 10:35 panic report carries plugin UUID `0E9563CF` =
2gg, and the EFI backups show 2gh installed at 10:40 — five minutes *after*
it. `-rbgpuoff` went into boot-args in the same window and stayed through
every later install, so **2gh had never run armed until now**: the panic that
prompted the rescue was 2gg's, which is the one 2gh was written to fix.

### The 2x2 is closed, and the two conditions are NOT separable

| engine | instance | asks | result |
|---|---|---|---|
| [0] | 0 | first | **TRUE** (every boot) |
| [1] | 0 | second (2gd) | FALSE |
| [1] | 1 | first (2gf) | FALSE |
| **[1]** | **0** | **first** (2gh) | **TRUE** |
| [2] | 0 | second (2gh) | FALSE |

Cell 4 against cell 3 — same engine, same order, instance differs — makes the
**instance index** matter. Cell 4 against cell 2 — same engine, same
instance, order differs — makes the **ordinal** matter. Both are necessary,
and engine[2] reproducing cell 2's shape in the same boot is a fifth,
internally-consistent point.

**What is NOT claimed: that these are two independent conditions.** This chip
has one SDMA instance, so "the second engine to ask" and "a duplicate create
on instance 0" are the **same event** in every cell measured, and nothing
here separates them. No experiment that leaves the engine count at four can:
there is no second instance to ask for. Recorded as open rather than
resolved — the 2gc confound was also named in advance and still cost a boot
to clear — and it does not block the fix, because both readings imply the
same one.

So HANDOFF §7's `post=1100` branch is the live one: **make exactly one SDMA
engine ask.** `hw->0xc2*2+2` is always even and >= 2, so the count cannot
express 1 and the constant is not the lever.

## 2gi (2026-10-16): exactly one SDMA engine asks

Engines [1..3] get a zero queue count, so Apple's own `start()` takes its
trivial-TRUE path at `0xbea4b49` — sets the started flag, calls
`vtable[0x278]`, returns TRUE without asking the layer for anything. That
path is proven on hardware twice: 2ge used it on engine[0], and the 2gh
boot's `post=1100` has engine[0] passing through it with `skip0=2`.

**engine[0] is left completely alone.** It natively asks for instance 0 (the
pairs are `(0x7, idx)` / `(0xa, idx)`, measured in 2gd) in the first ordinal
position — the one combination measured TRUE on every boot. So 2gi needs
**neither** `-rbgpusdmainst0` **nor** `-rbgpusdma0skip`, and both must come
out: one variable.

**The counts are not restored afterwards.** Those three engine objects really
do have no queues on this chip — the instance behind them does not exist — so
zero is the honest state, and restoring it would be a latch reporting
hardware that is not there, which is the debt 2am/2an/2ao already owe. It is
tolerable for the same reason 2ge's was: an armed boot has no GPU clients and
nothing submits SDMA work.

Two instrument details, both rule 2:

- A write that does not read back zero is logged as *failed*, and that
  engine's later `post=` digit is said to mean nothing — otherwise a switch
  that did not take and a layer that accepted all four look identical.
- **`one=N`** rides out on `rbgpu-sdma`, so `post=1111` is only readable as
  the one-engine answer when N is 3.

And `-rbgpusdmaone` joins the **0x1B hold**, which is the point of the hold
rather than a formality: this boot is expected to be the first on which
`powerUpHW` succeeds, so without it the same boot would also be the first
ever to send request 0x1B with the hardware fully brought up — and 2fo
measured that call never returning. `-rbgpuforce1b` is the deliberate second
step.

Build: UUID B8D742D4, hosttest passes. **Boot-args change**: add
`-rbgpusdmaone`, remove `-rbgpusdma0skip` and `-rbgpusdmainst0`.

```
post=1111 allstarted one=3   startHWEngines returns TRUE and powerUpHW
                             SUCCEEDS for the first time in this project; the
                             next step is 0x1B, held by design
post=1111 one<3              a switch did not take, and the note says so
                             rather than letting this read as the answer
post=1000 fail@1             engine[0] alone is not enough either, so the
                             layer refuses something that is not the instance
                             and not the ordinal - and the engine OBJECT is
                             back, with -rbgpusdmadesc the next boot
allstarted but ret=0         every start() returned TRUE and startHWEngines'
                             FALSE comes from OUTSIDE the loop (0xbea906b
                             onward), which nothing has yet looked at
```

### 2gi answered on hardware (2026-10-16): `powerUpHW` SUCCEEDS — and it reproduced

**Two consecutive armed boots, both complete, both identical.** The capture is
boot `b=6` (logs/20261016-020353-plugin, `plugin_check` run **in the armed
boot**: 41 KB log, `pci.txt` 611143 bytes, the healthiest armed capture this
project has taken); the live notes read afterwards are boot `b=7`. Build and
args checked first per the 2cu/2cv rule:

```
kmutil showloaded -> B8D742D4 == build/out (2gi)
boot-args         -> ... -rbgpunogates -rbgpusdmaone
                     (-rbgpusdma0skip and -rbgpusdmainst0 both OUT: one variable)
b=6 (in-boot)  rbgpu-sdma = c2=1 n=4 map=.... allok ret=1 pre=0000
                            post=1111 allstarted inst0=0 sdma[0]{} one=3
               rbgpu-prog = s=5 powerUpHW OK; 0x1B HELD by an SDMA arg
b=7 (live)     byte-identical in both variables
no new .panic  -> newest is still 2026-10-15 10:35 (2gg's)
```

**2gi's first branch, with its own precondition satisfied.** `one=3` says
exactly three engines were zeroed and read back zero, which is what makes
`post=1111` readable as the one-engine answer rather than as a switch that
did not take — the distinction 2gi wrote into the note in advance. From the
log, independently:

```
[16.747] 2bs: AMDHardware::startHWEngines entered
[16.756] 2fp: startHWEngines will walk 4 entries (hw+0xc2 = 1)
[16.784] 2gb: engine[0] started=0 queues=2 pair[0]=(0x7,0x0)   <- LEFT ALONE
[16.870] 2gi: engine[1] queues(+0x48) 2 -> 0 (read back 0)
[16.934] 2gi: engine[2] queues(+0x48) 2 -> 0 (read back 0)
[16.992] 2gi: engine[3] queues(+0x48) 2 -> 0 (read back 0)
[17.033] 2bs: AMDHardware::startHWEngines -> 1                 <<<< TRUE
[17.038] 2gb: pre=0000 post=1111 allstarted
[17.152] 2br: powerUpHW returned 0x5d97bc01                    <<<< AL=1 = OK
[17.157] handlePostRegister: Request 0x06 returned 0x00000000
[17.169] handlePostRegister: Request 0x1B is NOT sent - powerUpHW_ok=1, hold=1
```

So, for the first time in this project:

- **`AMDHardware::startHWEngines` returns TRUE.** Letting exactly one SDMA
  engine ask the layer for queues is sufficient, and Apple's own trivial-TRUE
  path at `0xbea4b49` carries the other three.
- **`powerUpHW` SUCCEEDS.** `0x5d97bc01` is the 2dc convention — upper bits
  are leftover register content, `AL = 1` is the bool — and the peer's own
  `powerUpHW_ok=1` in the 0x1B line confirms it from the other side.
- **The 2gc/2ge 0x1B hold did its job on the boot it was written for.** This
  is exactly the boot it was built to protect: the first on which `powerUpHW`
  succeeded would otherwise also have been the first ever to send request 0x1B
  with the hardware fully brought up. It was held, and the thread returned.

**And a side effect that answers 2bq without a boot of its own.** The display
pipe reserve now PASSES — seven `2bn reserve` calls, all seven returning 1,
at real carve-out offsets (`0xf4fb000000 size=0x4400000` and
`0xf40fb27000 size=0x3000`):

```
2bq  "AMDHWMemory::reserve fails because the allocator is empty - free=0,
      no reservations - because powerUpHW has never run"
```

That reading was right, and it is confirmed by the cause being removed rather
than by argument. docs/SPEC-DISPLAY-RESERVE.md's chain is unblocked.

Still standing, unchanged: `gfx_min = gfx_max = 0` (GFX DPM never started),
and `2ct` still refuses the display pipe by design, so the desktop stays on
IONDRVFramebuffer.

**What is NOT claimed.** The 2gh entry recorded that "the second engine to ask"
and "a duplicate create on instance 0" are the same event in every cell
measured and that nothing separates them. This boot does not separate them
either — it was never going to, since it removes the second asker entirely.
Both readings predicted this result; it discriminates neither, and that stays
open.

## 2gj (2026-10-16): two instrument defects the successful boot exposed

Neither touches behaviour, and both are the kind that make a later record
unreadable.

- **`RBGPU_MAX_DIAGS` 320 -> 448.** The 2dt warning fired for the first time
  ever, **20 times in one boot**, so keys really were being dropped and
  "absent" had stopped meaning absent. Third raise (96 in 2db, 192 in 2dt,
  320, now 448) — and the warning 2dt added is what caught it unprompted,
  which is the first time that guard has paid for itself.
- **The `allstarted` log line contradicted the boot it described.** It printed
  "startHWEngines' FALSE comes from outside the loop" on a boot where
  `startHWEngines` returned TRUE, because that branch was unreachable with
  `ret=1` when the message was written. `rbgpu_poststarthw_2gb` now takes the
  walk's return and the two cases say what they are. A record that disagrees
  with itself is the 2ga trap.

Build: UUID BE087B5D, hosttest passes. Boot-args unchanged.

## The next step is request 0x1B, and it is the user's call

`powerUpHW` succeeding makes `-rbgpuforce1b` the deliberate second step the
hold was always deferring. What is known about it, and it is not encouraging:

- 2cy sent 0x1B with the hardware **untouched** (powerUpHW failed at the first
  gate) and it answered `0xE00002C7` at once.
- 2fo sent it with the engines powered, the queues mapped and the rings
  programmed, and **it never returned** — the machine sat for minutes and had
  to be power-cycled. That is what 2fr's skip was built from.

So this is the first boot that would send it in the state 2fo hung in, with
one difference: `startHWEngines` now succeeds, so the controller really is
enabled rather than half-way. Whether that is the difference that makes 0x1B
answer is exactly the open question, and a hang costs a power cycle.

`rbgpu-prog` stage 6 is already wired to record the return, so a boot that
hangs says `s=5 ... 0x1B SENT` and one that answers says `s=6 0x1B RETURNED
0x........`, from NVRAM, with the `b=` nonce making it this boot's.

## 2gk (2026-10-16): 0x1B still does not return — but it SPINS, it does not block

logs/20261016-021917-plugin, `plugin_check` ran **in the armed boot**, build
and args checked first per the 2cu/2cv rule:

```
kmutil showloaded -> BE087B5D == build/out (2gj)
boot-args         -> ... -rbgpunogates -rbgpusdmaone -rbgpuforce1b
rbgpu-boot        -> 8        a sequence never recorded before
rbgpu-sdma        -> b=8 c2=1 n=4 map=.... allok ret=1 pre=0000
                     post=1111 allstarted inst0=0 sdma[0]{} one=3
rbgpu-prog        -> b=8 s=5 0x1B SENT - no s=6 means it hangs here
rbgpu-prog2       -> b=8 a=s5/S12/L3112@30138ms
                          b=s5/S12/L7133@50138ms
                          c=s5/S12/L12389@80139ms
no new .panic     -> newest is still 2026-10-15 10:35 (2gg's)
```

**The 2gi result reproduced a third time** (`ret=1 post=1111 one=3`), so
`powerUpHW` succeeding is now solid across three consecutive boots.

**And the hypothesis the hold was deferring is REFUTED.** The 0x1B entry
predicted that `startHWEngines` now succeeding might be the difference that
makes request 0x1B answer, since 2cy got `0xE00002C7` at once with the
hardware untouched and 2fo got nothing with it half-way. It is not: `s=5` with
no `s=6` means 0x1B was sent and did not return, in the one state it had never
been sent in.

### But it is not 2fo's wedge, and that is the new part

```
pci.txt           -> 401408 bytes   (2fo's was 0: the IGPU subtree was locked)
rbgpu-prog2       -> L3112 -> L7133 -> L12389 across 30/50/80 s
log.txt           -> 1004 lines, ALL from 122.8 s onward, 934 of them 0x14
```

The log-sequence counter advances by ~4000 per 20 s and all three samplers
fire, so **the machine is alive and code is running** — the opposite of 2fy's
`S12/L671` frozen triple. 2fo's "it never returned" was right about 0x1B and
wrong to call the machine wedged; with `pci.txt` completing, nothing holds the
IOService lock this time.

### The control, run before claiming anything

Request 0x14 (Gpu Status) is a peer request Apple's accelerator makes of us.
Its rate across the last three boots, where the only variable is the boot-arg:

| boot | 0x1B | last 0x14 | rate |
|---|---|---|---|
| 2gf (b=3) | powerUpHW failed, not sent | #64 at 15.0 s | 4/s |
| 2gi (b=6) | **HELD** | #320 at 17.1 s | 18/s |
| **2gk (b=8)** | **SENT** | **#738688 at 132.5 s** | **~5600/s** |

A 300x change against the 2gi boot, which is the same build family with
`powerUpHW` equally successful and `-rbgpuforce1b` the only difference. So
**request 0x1B's path is what polls GPU status, and it spins rather than
blocking** — established by the control, not inferred from the one boot.

And our answer to it is a **constant**
(`RembrandtGPUPeer.cpp:1553`, unchanged since milestone 2b):

```c
case 0x14:
    if (p2) { *(uint32_t *)p2 = 1; return kIOReturnSuccess; }
```

**Reading, NOT established:** Apple's controller-enabled path polls GPU status
through the peer and our invariant `1` never satisfies its exit condition.
What the record does **not** carry: whether the poll is on the same thread as
0x1B (the log has no thread ids), and what value or state that loop actually
wants — 0x1B's handler has never been disassembled. Writing the mechanism up
as a finding on a rate correlation alone would be the 2gc shape; it is a lead
with a strong control under it.

### Consequence for the instrument

The 512-line ring is now useless on an 0x1B boot: at 5600 requests/s the
every-64th throttle 2cw added still writes ~87 lines/s, so the whole chain —
`startHWEngines`, the SDMA lines, the 0x1B send itself — is evicted within
six seconds. That is why `grep 0x1B log.txt` finds nothing while
`rbgpu-sdma` and `rbgpu-prog` carry the answer. The NVRAM notes are the
record on this frontier, exactly as 2gd/2gf built them to be.

Next step is a **reading job, not a boot**: disassemble what 0x1B does in
`callPlatformFunction`'s dispatch, which HANDOFF §7 already named as the
branch to take if 0x1B hung. No build, no boot-arg, no hardware.

## 2gl (2026-10-16): request 0x1B disassembled — no boot spent

`AMDGraphicsAccelerator::callPlatformFunction` (0xbe4154e) dispatches on
`(*(uint32 *)p1) - 1` through a jump table at **0xbe41ad4**, bounded at 0x1b
so requests 0x01-0x1C are live and everything else takes the
`0xE00002C7` default set at 0xbe415a7.

**The table decode is cross-checked, not assumed.** It gives request 0x06 ->
`0xbe41717`, and the 2ga panic frame reads `callPlatformFunction+0x21f` =
`0xbe4176d`, which is the `callq powerUpHW` inside that case. An independent
record landing inside the case the table predicts is what makes the rest of
this readable.

```
req    target       what
0x06   0xbe41717    powerUpHW            <- works, returns 0
0x1B   0xbe417d0    Controller Enabled   <- does not return
0x1C   0xbe4163c    the 0x1B sibling (power-manager request 0xB, not 0xA)
0x02, 0x0B-0x0F, 0x12-0x18, 0x1A  ->  0xbe41aa8, the 0xE00002C7 default
```

### 0x06 against 0x1B, side by side

Both open with the **same** four steps - a lock triple on `this+0x90` /
`this->0x88`, a call into IOAcceleratorFamily2 at `0x14e551a6`, and
`vtable[0x850]` with the same string - and then diverge completely:

```
0x06 (0xbe41717)                      0x1B (0xbe417d0)
  r14 = 0          success up front     r14 = 0xE00002D8   a BAIL value
  callq powerUpHW                       if (this->0x1f40)
  this->0x1eb8 = 1                        sendRequestToPowerManager(
  if (this->0x1a70)                           this->0x1ea0, 0xA, 0)
    (0x1a70)->vtable[0x130](1)          <formatted call>(str, this->0x1e80+4)
  vtable[0x858](str, 0)                 hw = this->0x1a38
  callq 0x14e551b8   (family)           if (!hw)            -> 0xE00002D8
  <stub>(this->0x88)                    if (!hw->vt[0x2a0]()) -> 0xE00002D8
  vtable[0xae8]()                       hw->vt[0x4c8]()
  jmp -> return 0                       hw->vt[0x130]()
                                        (this->0x380)->vt[0x158]()
                                        hw->vt[0x218]()
```

Four things this settles:

- **0x1B's case contains no loop.** It is straight-line, so the ~5600/s
  request-0x14 poll 2gk measured is inside one of these callees, not in the
  dispatch.
- **0x06 never touches `this->0x1a38`** (the `AMDHardware`); 0x1B makes four
  calls on it. So the two requests are not variations on one path — 0x1B is
  the first and only request that drives the hardware object from this
  dispatch.
- **0x06 sets its return to 0 before doing any work**, which is why it
  answered `0x00000000` on every boot that reached it even while `powerUpHW`
  failed. Its success is not evidence about anything downstream.
- **`hw->vtable[0x2a0]()` is a gate that has never been exercised.** The same
  slot appears at 0xbe416c4, but that is inside request **0x1C**'s case, and
  our peer has only ever sent 0x06 and 0x1B — so no boot has run it. (I was
  one step from reading 0x06's success as validating that gate. It is in a
  different case.)

### The leading candidate, named as a candidate

`sendRequestToPowerManager(this->0x1ea0, 0xA, 0)` is the first substantive
thing 0x1B does that 0x06 does not, and it is a **named symbol**
(`__ZN37AMDRadeonX6000_AMDGraphicsAccelerator25sendRequestToPowerManagerE26_eAMD_PWR_MGR_REQUEST_TYPEbPvS1_S1_`),
so it can be hooked observe-only for the cost of one boot.

Why it is the leading reading: a power manager that asks the GPU for status
until it reaches a requested state is exactly the 6 kHz shape, and
**`gfx_min = gfx_max = 0` has stood since 2di** - the SMU reports no GFX clock
range, so a wait for a clock state could never be satisfied. That would make
the oldest unexplained debt in this project the cause of the newest frontier.

**NOT established, and three things have to hold first:** that the poll is on
this thread at all (the log carries no thread ids); that `this->0x1f40` is
nonzero so the call even runs; and that it is this call rather than one of the
five `hw` vtable slots, none of which is named yet.

### Correction (same day): there was NO tooling debt — it was my invocation

The paragraph this section originally carried claimed `dumptable.py` and
`vtslots.py` "could not reach" the vtable, the strings and the stubs, and
recorded that as a finding. **It is withdrawn.** The truth, checked rather
than assumed one step later:

```
dumptable.py ... __ZTV32AMDRadeonX6000_AMDNavi21Hardware 0x600
  ValueError: invalid literal for int() with base 10: '0x600'
```

`int(sys.argv[3])` parses base 10, so passing a hex size raised before the
tool ever looked at the file. Without the size argument it works perfectly —
the vtable is at `0xbfbc100`, inside `__DATA` (vmaddr 0xbf90000, filesize
0x36000), and `Macho.off()` maps it correctly. The strings are in `__cstring`
(0xbf3c521 + 0xc6de), which the extracted file also carries; my reader for
them was mis-parsing `objdump -s` output, not hitting a missing section.

**Twenty-second instance of the recurring error**, and the first where the
thing I blamed was my own tooling rather than the chip. The shape is identical
to all the others: a failure was read as evidence about the world without
establishing what would make it mean that. One line of `int(x, 0)` in
`dumptable.py` is the only real defect, now fixed so the trap cannot recur.

## 2gm (2026-10-16): request 0x1B named end to end, and the 0x14 flood explained

With the tools working, every call in 0x1B's case resolves:

```
0x1B (0xbe417d0)
  lock triple (this+0x90, this->0x88) ; family call 0x14e551a6 ; vtable[0x850]
  if (this->0x1f40)  sendRequestToPowerManager(this->0x1ea0, 0xA, 0)
  log("%s: Controller is enabled, finish initialization", this->0x1e80 + 4)
  hw = this->0x1a38 ; r14 = 0xE00002D8
  if (!hw)                            -> return 0xE00002D8
  if (!hw->isDeviceValid())           -> return 0xE00002D8      [0x2a0]
  hw->startGlobalPerfCounters()                                 [0x4c8]
  hw->hardwareDidStart()                                        [0x130]
  (this->0x380)->vtable[0x158]()
  hw->enableInterrupts()                                        [0x218]
```

**That log string dates the case.** `"Controller is enabled, finish
initialization"` is in the **2db** log (2026-10-12), when `powerUpHW` failed
and 0x1B answered `0xE00002C7` at once. So 0x1B's case used to run start to
finish and now does not — the difference is that the hardware is actually
powered up, so the four calls after the gate do real work instead of finding
nothing to do.

### What issues request 0x14, and why its rate is a poll meter

`AMDHardware::isDeviceValid()` (0xbea98ae) **is** the request-0x14 sender:

```
bea98ce  rdi = this->0x18         the peer
bea98d5  esi = 0x14               the request
bea98da  edx = 1                  waitForFunction = TRUE
bea98e5  callq *0x2a8(vtable)     callPlatformFunction
```

Our own log line reads `SpecialAMDKey request 0x14 (wait: 1)`, and `wait: 1`
is that `edx = 1` — an independent confirmation from two directions.

It is **one call per invocation, not a loop**, so 2gk's ~5600/s is 5600
*invocations* per second. And the 56 call sites through vtable slot 0x2a0
include the two that matter:

```
AMDHWRegisters::read(uint)          0xbe8d266   <- EVERY register read
AMDHardware::MCIL_WaitFor(..)       0xbeab526/0xbeab54f   a polling primitive
AMDHWChannel::waitForHwStamp        0xbe855e8   (already known to poll)
AMDGFX10PM4Engine::isIdle           0xbea17f2
```

**So the 0x14 flood is a register-poll loop, and the counter is a free poll
meter** — ~5600 register reads a second, each one validating the device
through the peer first. That reframes 2gk: the rate was never about GPU status
as such.

So the loop is inside one of 0x1B's four post-gate calls, and
`startGlobalPerfCounters` / `hardwareDidStart` / `enableInterrupts` are each
plausible places to wait on a register. **Not established:** which one. 2gl's
`sendRequestToPowerManager` candidate is **weakened** — it runs before the
logging call, and that log line is absent from the 2gk capture only because
the ring had wrapped, so it cannot be used either way.

### The next boot, now precisely aimed

Hook the four observe-only and log ENTER and RETURN for each; the one entered
without returning is the answer, and the three before it returning is positive
evidence either way. All four are external and defined in X6000, so routing
is legitimate under the milestone-2c rule — **and rule 9 applies: disassemble
each one's first 16 bytes and refuse any whose prologue carries a rel8/rel32
branch inside the 5-byte patch window.** `isDeviceValid` itself is an example
of why: it opens `cmpb $0x1, 0x30d(%rdi) ; jne` — a rel32 branch at +7, which
is outside the window, but the check is not optional.

## 2gn (2026-10-16): which of 0x1B's four post-gate calls does not return

Three observe-only hooks, and the fourth call localised by elimination.

**The rule-9 check, done before any code was written.** Each target's first
16 bytes disassembled; the patch window is the first 5:

```
startGlobalPerfCounters 0xbeae2e2  55 / 48 89 e5 / 41 56 / 48 89 fb ...
hardwareDidStart        0xbeacf08  55 / 48 89 e5 / 41 56 / 53 / 48 83 ec 10
                                   (its rip-relative leaq is at +0xe)
enableInterrupts        0xbea9558  55 / 48 89 e5 / 41 56 / 53 / cmpb ...
                                   (its `je` is at +0x14)
```

All three open `pushq %rbp ; movq %rsp,%rbp ; pushq %r14`, straight-line well
past the window, so no relative branch is displaced into Lilu's trampoline —
the fault 2gb diagnosed. And `nm -m` shows all three **external and defined
in `__TEXT`** of X6000, so the milestone-2c rule holds too.

Apple ignores every one of the three return values (no test follows any of the
three call sites at 0xbe41889 / 0xbe4189d / 0xbe418c5), so they are routed
through a `void (*)(void *)` thunk. Nothing is substituted.

**The fourth call is not hooked, because it has no symbol.**
`(this->0x380)->vtable[0x158]()` sits between `hardwareDidStart` and
`enableInterrupts`; `this->0x380` is loaded in eight `AMDGraphicsAccelerator`
methods and stored in none of them, so a base class sets it. It is **not** an
`AMDSWScheduler` — that vtable is 0x148 bytes, so slot 0x158 is past its end.
(That was the obvious guess, since `swTimestampCallback` loads the same field;
it was checked rather than assumed.) So `hds:R` present with `ei:E` absent
names it with no fourth symbol needed.

**The record is `rbgpu-1b`**, its own NVRAM variable that no later stage
overwrites — the 2gf lesson — holding a progressive trail:

```
perf:E perf:R hds:E hds:R ei:E ei:R     every call returned; 0x1B's hang is
                                        after enableInterrupts, in the
                                        epilogue at 0xbe418cb onward
perf:E perf:R hds:E hds:R ei:E          enableInterrupts does not return -
                                        an interrupt path, newly reachable
                                        since 2em
perf:E perf:R hds:E hds:R               the UNNAMED (this->0x380)->vt[0x158]
perf:E perf:R hds:E                     hardwareDidStart
perf:E                                  startGlobalPerfCounters
absent                                  the gate refused: isDeviceValid()
                                        returned false and 0x1B returned
                                        0xE00002D8 - which `rbgpu-prog` s=6
                                        would then confirm
```

It is written **only while 0x1B is in flight** (`g_rb_2gn_in1b`, set by the
peer across that one `callPlatformFunction`). All three functions are also
called from `powerUpHW`'s own path, so an ungated note would be overwritten
by calls that are not the question — and `perf_calls` / `hds_calls` /
`ei_calls` still count every invocation either way.

Six flash commits at most, all of them **after** the ~500 us chain region that
2fo and 2ft showed has to stay clear; 0x1B is sent once 0x06 has returned
(2fr's timeline), so this is the stage-5/6 region where notes are already
proven safe.

Build: UUID CD191ED5, hosttest passes. **Boot-args unchanged** —
`-rbgpuforce1b` is already in, and it is what sends 0x1B.

Expect the boot to hang again and cost a power cycle; `rbgpu-1b` is read from
the recovery boot, with `rbgpu-boot` proving the nonce is new.

### 2gn answered on hardware (2026-10-16): `hardwareDidStart` is where 0x1B hangs

No `plugin_check` capture from the armed boot, so NVRAM is the whole record -
and the 2gd nonce is what makes it count. Build and args checked first, per
the 2cu/2cv rule:

```
kmutil showloaded -> CD191ED5 == build/out (2gn)
boot-args         -> ... -rbgpunogates -rbgpusdmaone -rbgpuforce1b -rbgpustampwait
rbgpu-boot        -> 9        a sequence never recorded before
rbgpu-1b          -> b=9 perf:E perf:R hds:E
rbgpu-prog        -> b=9 s=5 0x1B SENT - no s=6 means it hangs here
rbgpu-sdma        -> b=9 c2=1 n=4 map=.... allok ret=1 pre=0000 post=1111
                     allstarted inst0=0 sdma[0]{} one=3
rbgpu-prog2       -> b=4, two boots stale: the samplers fire at ~29/49/79 s
                     and this boot was cut short, so nothing is missing
no new .panic     -> newest is still 2026-10-15 10:35 (2gg's)
```

Against the decode table 2gn wrote before the boot:

- **`isDeviceValid()` PASSED.** The gate at 0xbe41858 let 0x1B through, which
  no boot had ever established - the alternative branch was an immediate
  `0xE00002D8`, and `s=5` with no `s=6` rules it out from the other side too.
- **`startGlobalPerfCounters` entered and RETURNED.**
- **`hardwareDidStart` entered and did not return**, so the unnamed
  `(accel->0x380)->vt[0x158]` and `enableInterrupts` are never reached and are
  both cleared.
- **`powerUpHW` succeeded a fourth consecutive time** (`ret=1 post=1111
  one=3`). The 2gi result is solid.

One caveat, stated because the trail is a single overwrite slot: `rbgpu-1b`
accumulates in one string, so a *lost* final commit (which 2gc proved can
happen) looks exactly like a mark that was never written. `hds:E` being last
is therefore "hds:R did not commit", and whether that is because it never ran
is settled by the next boot's marks rather than asserted here. `rbgpu-prog`'s
missing `s=6` and 2gk's 100-second request-0x14 flood both corroborate that
the thread genuinely does not come back.

## 2go (2026-10-16): hardwareDidStart's chain, every member of it named — no boot spent

`AMDGFX10Hardware::hardwareDidStart` (0xbeacf08) is 40 instructions with **no
loop**, and so is the base-class `AMDRTHardware::hardwareDidStart` (0xbe97920)
it calls first. So the stop is in a callee, and all of them resolve:

```
hardwareDidStart (GFX10, 0xbeacf08)
  A  AMDRTHardware::hardwareDidStart(this)        non-virtual, via
                                  __ZTV28...AMDRTHardware +0x140 (= vt[0x130],
                                  the slot Apple dispatched on: consistent)
     A1  peer->callPlatformFunction(0x12, wait=1, .., &out, 0)
         our peer answers kIOReturnUnsupported, so `this->0x206c0 = 0` and the
         `vtable[0x3e8]` branch at 0xbe9797c is NOT taken
     A2  (hw->0x380)->vt[0x120]()                 first time only
  B  peer->callPlatformFunction(0x1D, wait=1, .., &out, 0)
     also Unsupported here, so it is a bool store into `this->0x2084c`
  C  (hw->0x368)->vt[0x118]((hw->0x20838)->0x64)  ONE register read
  D  (hw->0x20820)->vt[0x160](0, C, 0, hw->0x20844)
```

**The two unresolved fields, resolved from the code that fills them:**

- `hw->0x380 = this->vtable[0x5f0]()` in `AMDHardware::init` (0xbea8012), and
  Navi21Hardware's dump +0x600 is `GFX10Hardware::allocateAMDHWVMM`, whose
  body `new`s an **`AMDGFX10VMM`** (0xbead25a). Call offset 0x120 -> dump
  +0x130 = **`AMDHWVMM::postInit`** (0xbe90746). Disassembled: it registers
  two `vmFaultInterruptCallback` handlers for interrupt sources `0xFF0000E9`
  and `0xFF0000EA` and tail-jumps out. **No loop, no register access.**
- `hw->0x20820 = new AMDGFX10UserQueueManager` in `GFX10Hardware::init`
  (0xbeace13). Call offset 0x160 -> dump +0x170 =
  **`setSchedulerResources(ushort, uint, uint, uint)`** (0xbec86a6), whose
  four-argument signature matches the call site's `(0, C, 0, hw->0x20844)`
  exactly.

**And `setSchedulerResources` is the finding.** It stores five fields and
**tail-jumps** into `AMDGFX10HIQHWChannel::submitSetResourcesPacket`
(0xbec7b6c) on `this->0x4e8` - the **HIQ**, the user-queue scheduler's own
channel - which is the KIQ's path with a different ring:

```
getHIQFrame      getRingBlock(0x20) + 32 filler dwords from vt[0x1f0]
                 (0xbe6c39e is three instructions; commitBlock is four -
                  neither can block, so the frame fetch is not a wait)
frame[0] = 0xC006A000    SET_RESOURCES, PM4 type 3 opcode 0xA0, 8 dwords
vt[0x328](frame + 0x40)  the HIQ's writeDataCmdPacket
submitFrame(true) (0xbec7c18):
    vt[0x140] must be true
    incl +0x80 ; commitBlock(0x20) ; doorbell vt[0x158]
    tail jump -> AMDHWChannel::waitForHwStamp (0xbe85536)
```

So **the HIQ submits a SET_RESOURCES packet and waits for a GPU fence, exactly
as the KIQ does** - and 2gm established that `AMDHWRegisters::read()` and
`AMDHardware::MCIL_WaitFor()` both reach our peer as request 0x14. A poll in
here is the ~5600/s flood 2gk measured.

**What does NOT fit that story, which is why this is a split and not a
diagnosis.** `-rbgpustampwait` is in boot-args and `-rbgpustampraw` is not, so
our 2en bounded wait caps at **2000 ms** and returns false into Apple's own
failure path. A 2 s cap cannot produce a flood that ran for 100+ seconds in
the 2gk boot. Either the stop is before `waitForHwStamp`, or the bounded wait
is not reached on this channel. Only marks can say which, and asserting the
HIQ stamp wait as the cause on the strength of the shape alone would be the
2gc shape again.

### The instrument

Five observe-only routes, every one of them a named symbol, plus `stw:` marks
in the `waitForHwStamp` hook that already exists. All checked before a line
was written:

```
nm -m   all five external and defined in X6000's __TEXT (milestone-2c rule)
rule 9  first 16 bytes of each are pushes and movs; the first relative branch
        is at +0x15, +0x12, +0x45, +0x12 and +0x1c - every one far outside the
        5-byte patch window, so no displaced rel8 can land in Lilu's
        trampoline (the fault 2gb diagnosed and 2ga measured)
```

Apple ignores the first three return values; the two HIQ ones are propagated,
and `hiq:R=` / `hsf:R=` carry the value so a false that is OURS (a NULL orig)
cannot be read as a hardware verdict. The marks are gated on `g_rb_2gn_in1b`
exactly as 2gn's are, so `powerUpHW`'s own calls to these functions write
nothing. `setSchedulerResources`' mark carries its four arguments, so `C` -
the one register read in the chain - rides out in the trail.

`req_2go` lives in its own `noinline` function: five more `RouteRequest`s on
`processX6000`'s frame broke `-Wframe-larger-than` at 1176 bytes, and `static`
locals need a guard variable the kernel does not support.

Build: UUID 71E5F5F6, hosttest passes. Boot-args unchanged.

```
rt:E vmm:E vmm:R rt:R ... ssr:E(..) hiq:E hsf:E(1) stw:E (stop)
        the HIQ's stamp wait does not return even bounded, which would mean
        our own 2en cap is not reached on this channel - and that is a bug in
        the instrument, not the chip
... hsf:E(1) (stop), no stw:E
        the stop is in commitBlock or the doorbell vt[0x158] - the first
        doorbell ring on the HIQ ring this project has ever caused
... ssr:E(..) (stop), no hiq:E
        setSchedulerResources' own five field stores, or the tail dispatch
rt:R then nothing
        request 0x1D into OUR OWN peer, or the single register read C - and
        0x1D reaching our `default:` means our own handler is in the path
rt:E then nothing, no vmm:E
        request 0x12 into our peer, same shape one step earlier
vmm:E then nothing
        AMDHWVMM::postInit - an interrupt registration, and the two sources
        it registers are VM FAULT handlers, newly meaningful since 2em
hds:R lands
        the 2gn boot's trail had simply lost its last commit, and the stop is
        the unnamed (accel->0x380)->vt[0x158] after all
```

### 2go answered on hardware (2026-10-16): `hardwareDidStart` RETURNS — the stop is the unnamed call

No `plugin_check` capture, so NVRAM is the whole record; the 2gd nonce makes
it count. Build and args checked first per the 2cu/2cv rule:

```
kmutil showloaded -> 71E5F5F6 == build/out (2go)
rbgpu-boot        -> 10       a sequence never recorded before
rbgpu-1b          -> b=10 perf:E perf:R hds:E rt:E vmm:E vmm:R rt:R
                     ssr:E(0,0x3b,0,0xc) hiq:E hsf:E(1) stw:E stw:R=1
                     hsf:R=1 hiq:R=1 ssr:R hds:R
rbgpu-prog        -> b=10 s=5 0x1B SENT - no s=6 means it hangs here
rbgpu-sdma        -> b=10 ... ret=1 post=1111 allstarted one=3
rbgpu-prog2       -> b=4, stale: the samplers fire at ~29/49/79 s and this
                     boot was cut short before the first one
no new .panic     -> newest is still 2026-10-15 10:35 (2gg's)
```

**Sixteen marks, every one of them paired, and the trail reads inside-out:**

- `rt:E ... vmm:E vmm:R ... rt:R` - the base `AMDRTHardware::hardwareDidStart`
  and `AMDHWVMM::postInit` both returned, so the two `vmFaultInterruptCallback`
  registrations for sources `0xFF0000E9`/`0xFF0000EA` completed, and request
  0x12 into our own peer returned (its `Unsupported` is the honest answer).
- `ssr:E(0,0x3b,0,0xc)` - `setSchedulerResources`, carrying its four
  arguments. **`C = 0x3b`**: the one GC register read in the chain, through
  `(hw->0x368)->vt[0x118]((hw->0x20838)->0x64)`. The `0xc` is `hw->0x20844`,
  which `0xbeae52a` sets with `movl $0xc` - so the argument block decodes
  against the code that fills it, and the single register read completed.
- `hiq:E hsf:E(1) stw:E **stw:R=1** hsf:R=1 hiq:R=1` - **the HIQ's stamp wait
  returned TRUE.** The GPU executed the HIQ's SET_RESOURCES packet and wrote
  its completion fence. That ring had never run; `submitFrame(true)` committed
  the block, rang the doorbell and got its fence back, and
  `submitSetResourcesPacket` returned 1.
- `ssr:R hds:R` - and `hardwareDidStart` RETURNED.

**So 2go's leading candidate is refuted by its own instrument.** The HIQ
SET_RESOURCES submit was named as the lead and the stamp wait as the suspect;
both work. The 2 s cap on our 2en bounded wait never even came into it -
`stw:R=1` says the fence arrived.

**`ei:E` is absent**, so of 2gn's decode table this is the "`... hds:R`"
branch: the stop is the one call between `hardwareDidStart` and
`enableInterrupts` - the unnamed `(accel->0x380)->vtable[0x158]()` at
`0xbe418b1`.

And it retires the 2gn caveat in the direction that matters: `hds:R` lands
here, so whether the 2gn boot lost that commit or genuinely stopped inside
`hardwareDidStart` no longer gates anything. Both boots sent 0x1B and neither
returned; this one says where.

## 2gp (2026-10-16): the unnamed call named, and the loop found — no boot spent

Every link resolves from the code that fills it:

```
accel->0x380 = this->vtable[0x880]()     IOGraphicsAccelerator2::start 0x14e4e4be
  vtable dump +0x890 -> IOGraphicsAccelerator2::createEventMachine 0x14e51c2a
    -> vtable[0x888] = AMDGraphicsAccelerator::newEventMachine 0xbe3c4ca
       -> new AMDRadeonX6000_AMDAccelEventMachine (0xd58 bytes)
call offset 0x158 -> dump +0x168 = AMDAccelEventMachine::finishAllStamps
```

(The earlier store to `accel+0x380` at `0x14e4dc74` is in the same function's
zeroing block, `rax = 0`, so it is not the one that matters - checked before
reading the other.)

**2gn's guess was right to be refused.** It said `accel->0x380` is not an
`AMDSWScheduler` because that vtable is only 0x148 bytes. It is the
**event machine**, and the slot is `finishAllStamps` - a name that fits a
5600/s poll exactly.

### The chain under it, and the loop

`AMDAccelEventMachine::finishAllStamps` (0xbe4b2c2) is a tracing wrapper: a
kdebug pair around a **non-virtual** call through the pointer at `0xbf90278`
-> `__ZTV24IOAccelEventMachineFast2`, dump +0x168. Its sibling `finishStamp`
uses +0x160 on the same pointer, and the derived vtable has those two at the
same dump offsets - which is what proves these are dump offsets applied to the
vtable *symbol*, not call offsets on an address point. So the body is

```
IOAccelEventMachineFast2::finishAllStamps  0x14e43a1e
    for (i = 0; i < this->0x30; i++)  sum += this->vt[0x150](i)
        BOUNDED, and vt[0x150] -> dump +0x160 = finishStamp(int)

IOAccelEventMachineFast2::finishStamp(i)   0x14e438c6
    submitted = this->[0xfc + i*24] ; completed = this->[0xf8 + i*24]
    if (submitted - completed <= 0)        return 0
    completed = *(this->0x28[i])           read the channel's fence
    if (submitted - completed <= 0)        return 0
 14e43975:  r = this->vt[0x238](i, submitted, &completed)
            if (r != 0) { handleFinishChannelRestart(r, i, 1); goto 14e43975 }
            ... kdebug, accumulate elapsed, return
```

**The retry at `0x14e439a0` is unconditional and carries no counter and no
delay.** Any nonzero return spins forever, and nothing inside the retry
re-reads the fence - so a wait that errors can never become satisfied.

`vt[0x238]` -> dump +0x248 = **`AMDAccelEventMachine::waitForStamp(int, uint,
uint *)`** (0xbe4b588), and its first two bails return nonzero before touching
hardware at all:

```
this->0xd38 == 0                                        -> 0xE0000001
idx < 0, idx >= hw->0x1e78, or hw->0x1a78[idx] == NULL   -> 0xE00002C2
```

So the mechanism is complete and internally consistent with everything 2gk
measured: a stamp wait that errors immediately, a channel restart that touches
registers, every register read going through `isDeviceValid` to our peer as
request 0x14 (2gm), and a retry with nothing to break it. ~5600 of those a
second for 100+ seconds, with the machine alive and `pci.txt` completing.

**NOT established, and it is exactly what the boot decides:** which return
code, and therefore which bail. Both are plausible and they need different
fixes - `0xd38` being NULL is a missing event-source setup, an invalid index
is a channel/stamp-slot mismatch. Asserting one from the shape would be the
2gc mistake.

### The instrument, and why it is counters rather than marks

Three observe-only routes. All checked before a line was written: `nm -m`
shows each **external and defined in its OWN kext's `__TEXT`**, so routing by
that kext's id is legitimate under the milestone-2c rule (and
`handleFinishChannelRestart` is routed under IOAcceleratorFamily2, in kextList
since 2ct); rule 9 is clean on all three - pushes and movs for the first 16
bytes, the first relative branch far outside the 5-byte patch window.

```
AMDAccelEventMachine::finishAllStamps     marks fas:E(n=this->0x30) / fas:R=
AMDAccelEventMachine::waitForStamp        COUNTERS ONLY
IOAccelEventMachine2::handleFinishChannelRestart   a counter
```

`waitForStamp` runs thousands of times a second, so a log line or an NVRAM
write per call would be the 2ez logging contention and the 2fo flash-commit
wedge at once. It gets plain `volatile uint32_t` increments - first return,
last return, call count - and **the +15/+35/+65 s prog2 samplers carry them
out**, a channel measured to fire while this very thread spins (2gk: all three
landed with `L` advancing by ~4000 per 20 s).

`g_rbgpuProgText` 192 -> 384 bytes, because three samples now carry the
counters and would have been truncated.

Build: UUID F49A2AEA, hosttest passes. Boot-args unchanged.

```
nvram rbgpu-prog2   W=<waitForStamp calls> r0=<first ret> rN=<last ret>
                    R=<restarts> N=<stamp count>
nvram rbgpu-1b      ... hds:E ... fas:E(n=..) and whether fas:R lands
```

| reading | means |
|---|---|
| `r0=rN=0xe0000001`, R climbing | `evm->0xd38` is NULL - the event machine has no wait source, and the retry is infinite. The oldest candidate fix is whatever sets that field. |
| `r0=rN=0xe00002c2`, R climbing | the index or `hw->0x1a78[idx]` is bad - a channel/stamp-slot mismatch, and `N` says how many slots the loop walks |
| `r0` nonzero, `rN=0` | it eventually succeeds and the spin is elsewhere, so `fas:R` should land and the stop moves past `finishAllStamps` |
| `W` large, `R=0` | the retry is NOT the loop - `waitForStamp` itself polls internally, and its deeper path (0xbe4b602 onward) is the next read |
| `W=0` | `finishAllStamps` never reaches the wait, so the stop is in the bounded loop or the fence read, and `N` bounds it |
| `fas:E` absent | the stop is before `finishAllStamps` after all, which would contradict this boot's `hds:R` |

### 2gp answered on hardware (2026-10-16): kIOReturnTimeout, and it is a GPU-RESTART LOOP

logs/20261016-161920-plugin, `plugin_check` run **in the armed boot** at
~221 s of uptime - the longest an armed boot has ever been left up, which is
what made the three samplers all land. Build and args checked first per the
2cu/2cv rule: F49A2AEA == build/out (2gp), `rbgpu-boot` = **11**, a sequence
never recorded before. No new `.panic`. `pci.txt` 405504 bytes.

```
rbgpu-prog  = b=11 s=5 0x1B SENT - no s=6 means it hangs here
rbgpu-sdma  = b=11 ... ret=1 post=1111 allstarted one=3        (a FIFTH time)
rbgpu-prog2 = b=11
  a=s5/S12/L2981@30820ms   W=1 r0=0xe00002d6 rN=0xe00002d6 R=1 N=63
  b=s5/S12/L6984@50820ms   W=3 r0=0xe00002d6 rN=0xe00002d6 R=3 N=63
  c=s5/S12/L12178@80821ms  W=5 r0=0xe00002d6 rN=0xe00002d6 R=5 N=63
```

**The retry loop 2gp read out of the disassembly is confirmed running.** `W`
and `R` climb in lockstep, 1:1 - every `waitForStamp` is followed by exactly
one `handleFinishChannelRestart` - which is `finishStamp`'s unconditional
retry at `0x14e439a0` and nothing else. `N = 63` stamp slots.

**And the return code is NEITHER bail I named.** `0xE00002D6` is
`kIOReturnTimeout` (IOReturn.h:124). So `waitForStamp` does not refuse on
`evm->0xd38` or on an index check - it **genuinely waits and times out**, and
both branches of 2gp's table are wrong about the mechanism even though its
"R climbing = infinite retry" shape held. The rate says the same: W rises by
2 per 20 s, so an iteration costs ~10 s - a timeout period, not an argument
check - and 2gk's ~5600/s request-0x14 flood is the register polling *inside*
one wait.

### What the log carries, and it is bigger than the counters

The ring holds only ~9 s at this request rate (212.2 s to 221.1 s), but three
lines in it are decisive, two of them **Apple's own**:

```
2gn: ... ssr:R hds:R fas:E(n=63) stw:E stw:R=1 stw:E stw:R=1 ... (many)
void IOAccelEventMachine2::hardwareErrorEvent(): GPURestartDeferred stampIdx=25 type=2
RembrandtGPU: ASIC Reset State requested (unsupported)
2cx: waitForHwStamp ENTER (ch=0xffffff94ff368c80 stamp=70) ... 71 ... 72
2dr: the REAL wait returned 1 - THE STAMP ARRIVED
2dw: the fence dword at +0xc0 AFTER the wait ... the GPU wrote the fence
     where the CPU polls - this is the working path
```

- **`fas:E(n=63)` landed**, so `finishAllStamps` is entered - 2go's
  by-elimination naming of `(accel->0x380)->vt[0x158]` is confirmed by the
  hook firing, independently of the trail.
- **Apple raises a GPU RESTART.** `hardwareErrorEvent: GPURestartDeferred
  stampIdx=25 type=2` is IOAcceleratorFamily2's own `os_log`, captured through
  the milestone-2c route. So `handleFinishChannelRestart` is not a quiet
  bookkeeping call - it is Apple's hang-recovery path, and it names the
  offending slot: **stamp index 25 of 63**.
- **The restart asks OUR peer to reset the ASIC and we refuse.** Request
  **0x17** is sent by `AMDAccelChannel::resetHardwareAndReplay` (0xbe45549)
  through `AMDGraphicsAccelerator::sendRequestToController(
  _eAMDAccelIOFBRequestType, bool, ...)` - which also confirms, from Apple's
  own symbol, that these request numbers are its `_eAMDAccelIOFBRequestType`
  enum. Our handler answers `kIOReturnUnsupported` and never writes `p2`, so
  Apple reads an ASIC-reset state of 0 (the caller zeroes it at 0xbe4553f).
- **The KIQ is alive throughout.** Stamps **70, 71, 72** enter
  `waitForHwStamp` and the real wait returns 1 each time, with the fence
  landing where the CPU polls. So the GPU is executing and signalling
  normally while the event machine declares it hung.

**The chain, every link now named:**

```
0x1B -> hardwareDidStart (returns, 2go) -> finishAllStamps (63 slots)
  -> finishStamp(i)  for the slot with work outstanding
     -> waitForStamp -> kIOReturnTimeout after ~10 s, ~5600 register
        polls/s inside it (each one reaching our peer as request 0x14, 2gm)
     -> handleFinishChannelRestart
        -> hardwareErrorEvent: GPURestartDeferred stampIdx=25 type=2
        -> AMDAccelChannel::resetHardwareAndReplay
           -> request 0x17 (ASIC Reset State) -> OUR PEER -> Unsupported
     -> retry, unconditionally, forever
```

**What is NOT established**, and it is the thing that matters: *why stamp slot
25 has work outstanding at all.* At 0x1B time this project has submitted no
GPU work through the event machine, so a nonzero `submitted` there is either
work Apple itself issued and the GPU dropped, or bookkeeping that was never
initialised - and those need completely different fixes. Answering 0x17
differently is the tempting move and would be treating the symptom; it is
also a behavioural change to our peer and the user's call, so it is recorded
as an option rather than taken.

## 2gq (2026-10-16): read the stamp table before Apple walks it

`finishStamp(i)` takes its decision from three fields and nothing else, read
instruction by instruction at 0x14e438da-0x14e43912:

```
submitted = *(uint32 *)(evm + 0xfc + i*24)
completed = *(uint32 *)(evm + 0xf8 + i*24)
fence     = **(uint32 **)(evm->0x28 + i*8)
```

and it waits only while `submitted - completed > 0` survives re-reading the
fence. So the outstanding slots are exactly the ones that can reach
`waitForStamp`, and dumping them before Apple's walk says which they are,
what they are waiting for, and what the fence actually reads - the three
numbers that separate "real work the GPU dropped" from "bookkeeping that was
never initialised".

The walk runs in the existing `finishAllStamps` hook, is bounded at 64 slots,
refuses a count above that rather than walking a garbage length, and checks
every fence pointer against the kernel-VA range before dereferencing it - the
2gh rule, written after the 2gg panic read `1 + 0x48`. A pointer that fails
prints `?` rather than a value that could be mistaken for a measurement. The
list rides out on `fas:E(n=63|25:s3/c1/f1,... out=N)`.

Two instrument defects the same boot exposed, both fixed here:

- **The `stw:` marks became the flood.** Once the restart loop starts, the
  KIQ resubmits continuously and every `waitForHwStamp` added a pair to the
  trail - overflowing `RBGPU_LINE_MAX` so the published line swallowed the
  one after it (the 2ey defect, back in a new place) and heading for the
  `rbgpu-1b` CAP. They are suppressed from `fas:E` onward and **counted**
  instead, with the count riding out in prog2 as `T=`.
- `g_rb_2gn_trail` 320 -> 512 bytes for the stamp list, and
  `tools/plugin_check.sh` now captures **`rbgpu-1b`**, which it never did -
  this boot's copy had to be read live, and the live read was stale.

**On that staleness, because it is a measurement and not a nuisance.** The
in-boot `nvram.txt` holds `rbgpu-prog = b=11 s=5` while the live read after
the power cycle holds `b=11 s=1` - the same boot's nonce with an *earlier*
stage. So a note's later commits can be lost across a power cycle while an
earlier one survives, which is the 2gc loss with the nonce now proving it is
the same boot rather than leaving it ambiguous. `rbgpu-sdma`, `rbgpu-prog2`
and `rbgpu-1b` all read stale live and all read `b=11` in the capture. **Run
`plugin_check` in the armed boot; the live read is not a substitute.**

Build: UUID DEEF00AF, hosttest passes. Boot-args unchanged.

```
out=0                 nothing is outstanding and finishAllStamps should not
                      wait at all - which would contradict W climbing, so it
                      would mean the table changes between our read and
                      Apple's
one slot, f == sub    the fence already holds the asked-for value and the
                      CPU-side pickup is what fails - the 2dw shape
one slot, f == cmp    the GPU never wrote that fence: real outstanding work
                      the chip dropped, and the channel behind slot 25 is the
                      next thing to name
one slot, f=?         the fence pointer is not a kernel VA, so the table was
                      never initialised for that slot - bookkeeping, and the
                      fix is upstream of the GPU entirely
many slots            the event machine thinks a lot is in flight, which at
                      0x1B time would itself be the finding
```

### 2gq answered on hardware (2026-10-16): ONE slot outstanding, and it is slot 25

logs/20261016-202748-plugin, `plugin_check` run **in the armed boot**, build
and nonce checked first per the 2cu/2cv rule: DEEF00AF == build/out (2gq),
`rbgpu-boot` = **12**, a sequence never recorded before. No new `.panic`.

```
rbgpu-1b    = b=12 perf:E perf:R hds:E rt:E vmm:E vmm:R rt:R ssr:E(0,0x3b,0,0xc)
              hiq:E hsf:E(1) stw:E stw:R=1 hsf:R=1 hiq:R=1 ssr:R hds:R
              fas:E(n=63|25:s1/c0/f0 out=1)
rbgpu-prog2 = b=12  a=W=1 r0=rN=0xe00002d6 R=1 N=63 T=4
                    b=W=3 ...                R=3 N=63 T=12
                    c=W=6 ...                R=6 N=63 T=22
rbgpu-sdma  = b=12 ... ret=1 post=1111 allstarted one=3     (a SIXTH time)
```

**`out=1`.** Of 63 stamp slots exactly one has work outstanding, and it is
**slot 25** — the same index Apple's own `GPURestartDeferred stampIdx=25`
named in the previous boot, from a completely different source. Two
independent records agreeing on one number.

```
slot 25:  submitted = 1   completed = 0   fence = 0
```

So of 2gq's four branches this is **`f == completed`: the GPU never wrote
that fence.** The pointer dereferenced cleanly (it printed `0`, not the `?`
the kernel-VA guard emits), so the slot is initialised and the fence memory
is readable — this is one real submit that never completed, not uninitialised
bookkeeping. `submitted = 1` is the *first* stamp on that slot.

The 2gq suppression worked too: `T=4 -> 12 -> 22` counts the KIQ's
resubmissions (about four per retry iteration) without the trail overflowing,
and the stamp list fitted in the same string.

## 2gr (2026-10-16): name the channel behind slot 25 — no boot spent

Apple's own index-to-channel lookup is `AMDAccelEventMachine::eventTimeout`
(0xbe4b92e), which `restart_channel` calls through `vt[0x220]`:

```
accel = evm->0x10
if (idx < 0 || idx >= accel->0x1e78)   -> NULL
ch = accel->0x1a78[idx]                   0xbe4b991
```

**And that corrects 2gp's reading of `waitForStamp`'s second bail.** I wrote
it as `hw->0x1e78` / `hw->0x1a78`; it is the **accelerator's** array, the same
one `eventTimeout` uses. The correction also makes the 2gq result sharper:
`waitForStamp` returned `kIOReturnTimeout`, not `0xE00002C2`, so that bail did
*not* fire — which already proves index 25 is inside the bound and
`accel->0x1a78[25]` is non-NULL. The channel exists.

### What else the restart path turns out to consult

`handleFinishChannelRestart` (0x14e25edc) begins:

```
IOAccelDisplayMachine::waitForStampTimeout(stampIdx)    via accel->0x378
    if (TRUE)  return   <- no restart at all
```

and that function (0x14e220aa) is

```
if (this->0x108 == 0)  return FALSE          <- 0x108 is the PIPE COUNT
for (p = 0; p < this->0x108; p++)
    if (this->0x88[p]->waitForStampTimeout(i)) return TRUE
return FALSE
```

**2ct makes that count zero**, so Apple's own defer-the-restart escape can
never fire on this machine. Worth recording as an interaction between our
instrument and the frontier — but **not** as a fix: a pipe saying "I am
waiting on that stamp" only defers the restart, and the stamp still would not
complete. It changes who spins, not whether.

It then ends at `IOAccelEventMachine2::restart_channel` (0x14e25fe8), whose
`this->0x94 == 2` early path is `checkChannelProgress` (dump +0x228) — so the
`type=2` in Apple's log line and the `type=4` this path sets are different
entries into the same machinery.

### The instrument

The 2gq slot dump now also carries, for every outstanding slot, the channel
`accel->0x1a78[i]` with its **class name** (every channel is an OSObject, the
2cw idiom) and its own `AMDHWChannel` bookkeeping — the fields 2ds/2dw/2dy
established: `+0x80` submitted, `+0x84` completed, `+0xc8` the GPU address the
WRITE_DATA fence packet targets.

Reads only, both pointers checked against the kernel-VA range first (the 2gh
rule), and the class name is trimmed to its distinctive tail because every one
of them starts `AMDRadeonX6000_AMD`.

Build: UUID 6F1F006B, hosttest passes. Boot-args unchanged.

```
fas:E(n=63|25:s1/c0/f0[<class> subN cmpM c8=0x...] out=1)
```

| reading | means |
|---|---|
| `c8` a real `0xFF_BF..` GPUVM address, `sub=1 cmp=0` | the channel genuinely submitted one frame and the GPU never wrote its fence — the same shape 2dy measured on the KIQ before 2em fixed PT_BASE, now on a different channel, and the next question is that channel's ring and doorbell |
| `c8 = 0` | `allocateMemoryResources` never gave this channel a fence GPU address, so the WRITE_DATA had nowhere to land — a per-channel setup gap, and 2dx/2dy name the two slots that fill it |
| `sub = 0` while the event machine says submitted=1 | the event machine's stamp was recorded without the channel ever submitting — bookkeeping, and `writeStampCommand` (0x14e43477) is where to look |
| the class is a **display** or **VCN** channel | the outstanding work is not the GFX path at all, and the standing `2ct` zero-pipe choice becomes directly relevant |
| the class is the KIQ or a GFX ring | it is the path already proven to work, which would make slot 25's single stamp the odd one out and worth comparing against the stamps 70-72 that do complete |

### 2gr answered on hardware (2026-10-16): the channel is an `AMDAccelChannel` — and the fields I read with it are VOID

logs/20261016-204728-plugin, `plugin_check` run **in the armed boot** at ~156 s
of uptime. Build and nonce checked first per the 2cu/2cv rule: 6F1F006B ==
build/out (2gr), `rbgpu-boot` = **13**, a sequence never recorded before. No
new `.panic`.

```
rbgpu-1b = b=13 perf:E perf:R hds:E rt:E vmm:E vmm:R rt:R ssr:E(0,0x3b,0,0xc)
           hiq:E hsf:E(1) stw:E stw:R=1 hsf:R=1 hiq:R=1 ssr:R hds:R
           fas:E(n=63|25:s1/c0/f0[AccelChannel sub3461091728 cmp4294967246 c8=0] out=1)
rbgpu-sdma = b=13 ... ret=1 post=1111 allstarted one=3        (a SEVENTH time)
rbgpu-prog2 = b=13 W=1->3->6  r0=rN=0xe00002d6  R=1->3->6  N=63  T=3->10->22
```

**The class landed and it is a third thing.** Neither branch of 2gr's table
fired: the channel behind stamp slot 25 is **`AMDRadeonX6000_AMDAccelChannel`**
(the trim strips the shared `AMDRadeonX6000_AMD` prefix), which is the
accelerator's per-client software channel — the same class whose
`resetHardwareAndReplay` (0xbe45549) 2gp found sending request 0x17. So the
restart loop's victim and the restart's agent are one class, which is
coherent, and the pointer yielding a clean metaclass name proves it is a live
OSObject rather than a stale entry.

**And the three numbers beside it mean nothing.** Checked after the fact, from
the vtable extents:

```
__ZTV30AMDRadeonX6000_AMDAccelChannel  0xbf92998, 0x278 bytes
__ZTV15IOAccelChannel2                 0x14e7f088, 0x1b8 bytes   its base
__ZTV28AMDRadeonX6000_IAMDHWChannel    0xbfa8a80  }  a different
__ZTV35AMDRadeonX6000_AMDGFX10KIQHWChannel 0xbfbb5b8 }  hierarchy entirely
```

`+0x80` submitted, `+0x84` completed and `+0xc8` the fence GPU address are
**`AMDHWChannel`'s** layout, established in 2ds/2dw/2dy for the KIQ. An
`AMDAccelChannel` is an `IOAccelChannel2` subclass and has no such fields, so
`sub=3461091728` (0xCE4C1190), `cmp=4294967246` (-50) and `c8=0` are three
unrelated bytes of a different object. **Twenty-third instance of the
recurring error**, and its particular shape is worth naming: the step named
the class and read its fields *in the same breath*, so the fields were read
before the thing that gives them meaning was known. 2gs removes them.

### The stamp table resolved end to end, from Apple's own code — no boot spent

```
getStampOffset(i)            0x14e25ae0   return i * 4
setStampBaseAddress(base)    0x14e25aea
    evm->0x20 = base ; n = evm->0x30
    evm->0x28 = kalloc(n * 8)
    for (i = 0; i < n; i++) evm->0x28[i] = base + vt[0x138](i)
incrementStamp(i)            0x14e43416
    evm[0xfc + i*24]++  ;  evm->0x10->0xa0++        a SOFTWARE counter
writeStampCommand(i, q, rec) 0x14e4346c
    reads evm[0xfc + i*24], tail-jumps evm->vt[0x2a0] to emit it
```

Three things fall out, and two of them close open questions:

- **A "stamp slot" is not per-channel.** The fences are one flat block of `n`
  dwords at `evm->0x20`, slot `i` at `base + i*4`; `evm->0x28` is just an
  array of pointers into it. So slot 25's fence reading 0 means dword 25 of a
  shared block was never written — the slot is initialised and readable, and
  nothing wrote it.
- **`submitted` is a pure software counter.** `incrementStamp` bumps it with
  no hardware involved at all, and the emit is a separate call. So
  `s1/c0/f0` is exactly: `incrementStamp(25)` ran **once**, and no stamp value
  ever landed in dword 25.
- **The vtable convention is confirmed a second way.** `finishStamp` is at
  dump +0x160 and `finishAllStamps` at +0x168, i.e. call offsets 0x150 and
  0x158 — independently re-confirming 2gp's `vt[0x150]` and 2go's
  by-elimination naming of `(accel->0x380)->vt[0x158]`.

Also in this capture, both new: macOS spawned a **`DumpGPURestart[604]`**
process, so userspace sees the restart and not only our log; and request 0x14
reached **#1,107,904** — over a million register polls through the peer, two
request-0x17 ASIC-reset refusals 6 s apart matching `R` climbing.

**Not found: who calls `incrementStamp(25)`.** The slot-0x1d8 shortlist was
checked and does not hold — `signalStamp`'s call at 0x14e26283 is on
`evm->0x78`, a different object, and the base `IOAccelEventMachine2` vtable
has every slot from +0x1d0 to +0x210 pointing at one pure-virtual stub
(0x975800), so the base cannot name them either. Recorded as open rather than
guessed.

## 2gs (2026-10-16): read what Apple's code defines, not what another class has

The per-slot channel fields are gone; the class name stays, because that is
what the lookup actually establishes. In their place, three numbers each read
off an instruction rather than off an assumed layout:

```
accel->0xa0      the total incrementStamp count for the WHOLE driver
                 (0x14e4345a). If it is 1, slot 25's single submit is the
                 only stamp this driver has ever issued - which would make it
                 Apple's own one-off and not a leftover from a client.
accel->0x1e78    the channel-array bound eventTimeout and waitForStamp both
                 test, so it says whether 25 is a registered channel or
                 merely in range of an array that is mostly empty.
nz               how many of the n stamp dwords at evm->0x20 are nonzero -
                 i.e. whether the GPU has EVER written a stamp into that
                 block, on any slot.
```

`nz` is the one that splits the frontier, and it is free:

| reading | means |
|---|---|
| `nz=0` | the GPU has never written **any** stamp — the stamp block's GPU address is wrong or nothing was ever emitted, and this is one bug for all 63 slots rather than something about slot 25. The same shape as 2dh/2di and 2em: a correct CPU pointer paired with an address the hardware cannot reach. |
| `nz>0` with slot 25 still 0 | other slots complete and 25 alone does not, so the block is reachable and the question really is that one submit |
| `tot=1` | slot 25's submit is the only stamp the driver ever issued |
| `tot>1` with `out=1` | others were issued and completed, which would contradict `nz=0` and say the block is fine |
| `nch` small, 25 near it | the channel array is barely populated and index 25 being valid is worth its own look |

Reads only, same bounds and the same kernel-VA guard (the 2gh rule). Nothing
new touches hardware and no new NVRAM write: it rides the existing `fas:E`
mark on `rbgpu-1b`.

Build: UUID CD40814D, hosttest passes. Boot-args unchanged.

### 2gs answered on hardware (2026-10-16): `tot=1` — ONE stamp in the whole boot

logs/20261016-212858-plugin, `plugin_check` run **in the armed boot** at
~204 s of uptime. Build and nonce checked first per the 2cu/2cv rule:
CD40814D == build/out (2gs), `rbgpu-boot` = **14**, a sequence never recorded
before. No new `.panic`.

```
rbgpu-1b = b=14 ... ssr:R hds:R
           fas:E(n=63|25:s1/c0/f0[AccelChannel] out=1 tot=1 nch=63 nz=0)
rbgpu-sdma = b=14 ... ret=1 post=1111 allstarted one=3     (an EIGHTH time)
rbgpu-prog2 = b=14 W=1->3->6  r0=rN=0xe00002d6  R=1->3->6  N=63  T=4->10->22
```

**`tot=1`.** `accel->0xa0` is bumped by every `incrementStamp` in the whole
driver (0x14e4345a), and across the entire boot it reads **one**. So slot
25's single submit is not one of many — it is the *only* stamp this driver
has ever issued, through powerUpHW, every engine, and 0x1B. The event
machine's stamp table was touched exactly once.

**`nch=63`**, equal to the stamp count, so the channel array is fully
populated rather than mostly empty and index 25 is a real entry.

**And `nz=0` turns out to carry no information, which is my error in the
table rather than in the chip.** With `tot=1` at most one of the 63 dwords
could ever have been nonzero, so `nz=0` is exactly `f=0` restated. 2gs called
it "the one that splits the frontier, and it is free" and gave it a row
reading "the GPU has never written **any** stamp — one bug for all 63 slots";
that reading needs `tot>1` to mean anything, and the table listed `tot` as a
separate row instead of as the precondition it is. The branch did not fire;
it was never able to.

The KIQ meanwhile completes stamps **69, 70, 71** in the same window with
real fences (`+0x80`/`+0x84` advancing in step, the fence dword matching), so
the GPU is executing and signalling normally. Those are `AMDHWChannel` stamps
— a different mechanism entirely, which is why they do not appear in `tot`.

### Where slot 25 comes from, read out of Apple's own constructor — no boot

`AMDGraphicsAccelerator::createAccelChannels(bool)` (0xbe3b478) is what fills
`accel->0x1a78[]` and writes `accel->0x1e78` (0xbe3b995, the only writer).
Four `AMDAccelChannel::create` sites, in this order:

```
0xbe3b522   one per HARDWARE channel, i = 0 .. hw->vt[0x340]()-1
            name from the hw channel's own vt[0x200]
0xbe3b547   accel->0x1e7c = that count
0xbe3b57d   "VMPT"            at index == the count
0xbe3b774   a third group
0xbe3b940   "DisplayPipe%d"   last
```

**So the array is hardware channels first, then VMPT, then the rest** — and
2dv has measured **25 distinct hardware channels** on this chip, every boot,
including this one (`distinct reg=25 add=23`). If `hw->vt[0x340]()` is that
same 25, index 25 is **VMPT**, the VM page-table channel.

**Named as a candidate, not a finding.** Two independent counts agreeing at
25 is suggestive and is exactly the shape that produced the withdrawn 2dw
page scan: `registerChannel`'s distinct count and `vt[0x340]`'s return are
not established to be the same number. `accel->0x1e7c` is that number
directly and 2gt reads it.

**And the channel names itself, which settles it outright.**
`AMDAccelChannel::init` (0xbe443dd) does `strlcpy(this + 0x188, name, 0x10)`,
so every one of the 63 carries its own string in a 16-byte buffer. Also read
off the same function: `+0xe8` the IAMDHWChannel, `+0x1a4` the SS_PRIORITY,
`+0x198` initialised to -1, `+0x1a0` = `hwch->vt[0x1c8]()` — the same slot
2dv found is a compile-time constant for a KIQ.

## 2gt (2026-10-16): let the channel say its own name

Two reads, both off an instruction rather than an inference:

```
ch+0x188   the 16-byte name buffer, printed only if every byte is printable
           ASCII - anything else means this is not that buffer and the log
           says "?" rather than a value that could be mistaken for one
accel+0x1e7c   the hardware-channel count, so the VMPT boundary is a
               measurement instead of 2dv's count standing in for it
```

The class name stays beside it, since the two answer different questions.
Reads only, same bounds, same kernel-VA guard (the 2gh rule), no new NVRAM
write — it rides the existing `fas:E` mark.

Build: UUID E30F88B8, hosttest passes. Boot-args unchanged.

```
fas:E(n=63|25:s1/c0/f0[AccelChannel "<name>"] out=1 tot=1 nch=63 hwn=N nz=0)
```

| reading | means |
|---|---|
| `"VMPT"` with `hwn=25` | slot 25 is the VM page-table channel, and the one stamp this driver ever issued was a page-table update that never completed. The 2em PT_BASE write and `AMDHWVMM::postInit`'s two VM-fault interrupt registrations are both in that territory, so this would tie the frontier to the oldest live patch in the project. |
| a hardware channel's name, `hwn>25` | slot 25 is an ordinary ring channel and the name says which engine |
| `"DisplayPipe0"` | the outstanding work is the display path, and 2ct's deliberate zero-pipe choice stops being a side note |
| `"?"` | `+0x188` is not the name buffer and the offset reading is wrong — the class name still stands |
| `hwn != 25` | the VMPT-at-25 arithmetic does not hold and the name is the only evidence |

## 2gu — slot 25 is VMPT, and VMPT asks for the second SDMA engine

**Boot 15 (E30F88B8, 2gt) answered it.** `b=15` is new on `rbgpu-boot`,
`rbgpu-prog`, `rbgpu-sdma` and `rbgpu-1b`:

```
fas:E(n=63|25:s1/c0/f0[AccelChannel "VMPT"] out=1 tot=1 nch=63 hwn=25 nz=0)
```

The nonce check needed care this time. The capture was read in a LATER,
UNARMED boot: the live log says `armed = 0x0`, "rbgpu-arm not set", and
`rbgpu-diag` = `d=0 armed=0` with the armed boot's `d=72 armed=1` rotated
into `rbgpu-diag-prev`. That is consistent and not a stale read, because
`rbgpu_boot_seq` (RembrandtGPUPeer.cpp) bumps the counter lazily on the
**first note of a boot**, and the diag writer uses `setProperty` directly
with no `b=` prefix. An unarmed boot therefore rotates diag and writes no
note, leaving `rbgpu-boot` at 15. The b=15 notes are boot 15's, the 2gt
armed run. (This is the 2gd rule working as intended rather than against
us, but it is worth recording that "unarmed boot since" is a state the
nonce handles correctly only because of that laziness.)

### The channel names itself

`AMDAccelChannel::init` (0xbe443dd) does `strlcpy(this + 0x188, name, 0x10)`,
so the 2gt read is the string `createAccelChannels` passed in: **"VMPT"**.
Three independent agreements that slot 25 is the VMPT channel:

- the name buffer read straight off the object,
- `hwn=25` — `accel->0x1e7c` is the hardware-channel count, and
  `createAccelChannels` (0xbe3b547) stores it and then creates VMPT at
  exactly that index, so index == count,
- 2dv's measurement of 25 distinct hardware channels.

This replaces the 2gs candidate ("slot 25 **may** be VMPT, held as a
candidate, not a finding") with a measurement.

### What VMPT submits through, resolved from Apple's code with no boot

`createAccelChannels` at 0xbe3b54e:

```
movq  0x1a38(%r14), %rdi        ; accel->0x1a38 = the HW interface
movl  $0x2, %esi                ; _eAMD_CHANNEL_TYPE = 2
xorl  %edx, %edx                ; SS_PRIORITY = 0
xorl  %ecx, %ecx                ; idx = 0
callq *0x320(%rax)
leaq  "VMPT", %rcx
callq AMDAccelChannel::create(accel, thatChannel, index, "VMPT", 0)
```

Call offset 0x320 is dump offset **0x330** (2gp's `−0x10` rule), which in
`__ZTV32AMDRadeonX6000_AMDNavi21Hardware` is
`AMDGFX10Hardware::getHWChannel(_eAMD_CHANNEL_TYPE, SS_PRIORITY, unsigned)`
— three arguments, matching the call site exactly. That is an independent
confirmation of the −0x10 rule itself.

The chain:

1. `GFX10Hardware::getHWChannel` (0xbeaff68): type 1 goes to `this->0x3b0`
   (engine 0, GFX). **Anything else** tail-jumps through the
   `AMDRTHardware` vtable symbol at dump offset 0x330 — a non-virtual call
   through a vtable symbol, so dump offsets apply directly (2gp) —
   i.e. `RTHardware::getHWChannel`.
2. `RTHardware::getHWChannel` (0xbe973fa), for type 2:
   ```
   callq  *0x158(%rax)          ; caps struct
   movb   0x92(%rax), %r12b
   ... prio == 0 -> ebx = 1
   movzbl 0x8f(%rax), %edx      ; RING_TYPE = caps->0x8f
   leal   0x1(,%rax,2), %esi    ; caps->0x92 * 2 + 1   (always odd)
   andl   %ebx, %esi            ; & 1                  (always 1)
   incl   %esi                  ; ENGINE_TYPE = 2      (unconditionally)
   jmpq   *AMDHardware_vt[0x328] ; getHWChannel(engine, ring)
   ```
   The caps byte cannot change the engine: `x*2+1` is odd, so `&1` is 1 and
   the result is 2 whatever `caps->0x92` holds.
3. `AMDHardware::getHWChannel(engine, ring)` (0xbea9994):
   ```
   movq 0x3b0(%rdi,%rax,8), %rdi    ; hw->[0x3b0 + engine*8]
   testq %rdi, %rdi
   je    -> return NULL
   jmpq  *engine->vt[0x1a8]         ; (ring)
   ```
4. `Navi21Hardware::allocateHWEngines` (0xbeca1ff) fills that array:
   ```
   hw+0x3b0 [0] = AMDRadeonX6000_AMDNavi21PM4Engine          (GFX)
   hw+0x3b8 [1] = AMDRadeonX6000_AMDSDMAEngine_GC_10_3_0
   hw+0x3c0 [2] = AMDRadeonX6000_AMDSDMAEngine_GC_10_3_0
   hw+0x3c8 [3] = AMDRadeonX6000_AMDSDMAEngine_GC_10_3_0
   hw+0x3d0 [4] = AMDRadeonX6000_AMDSDMAEngine_GC_10_3_0
   ```
   (Navi23 at 0xbed2794 has the same shape with `_GC_10_3_4` and fewer.)
   Four SDMA engines — the `hw->0xc2 * 2 + 2 = 4` of 2gc/2gi, now with the
   objects named.

**So VMPT is backed by engine index 2: the second SDMA engine.** This chip
has one SDMA instance, and `-rbgpusdmaone` (2gi) zeroes the queue count of
engines [1..3] — `one=3` in this boot's `rbgpu-sdma` says all three took
the write.

### The debt 2gi wrote down in advance

2gi's own comment, when that bypass was added:

> "It is tolerable for the same reason 2ge's was: an armed boot has no GPU
> clients and nothing submits SDMA work. **If anything later submits to
> engines [1..3], it finds no queues** rather than a queue that silently is
> not backed."

The premise is false. The accelerator submits VMPT itself during startup —
a page-table operation, not client work — and VMPT is engine 2, one of the
three. `tot=1` says that submit is the only stamp the driver has ever
issued in the whole boot, and `nz=0` says no fence was ever written for it.

### Why none of the readings so far could settle it, and the one that can

`AMDAccelChannel::init` stores the hardware channel and then **branches on
it being NULL** (0xbe44380, 0xbe4438e):

```
movq  %rdx, 0xe8(%rdi)     ; this->0xe8 = hwChannel
testq %rdx, %rdx
je    0xbe443cd
  movl $0x190, %r12d / $0x400, %r15d    ; backed:   0x190 * 0x400
0xbe443cd:
  movl $0x90,  %r12d / $0x1,   %r15d    ; unbacked: 0x90 * 1
  movb $0x1, -0x29(%rbp)
```

A NULL hardware channel is a **supported path**: `init` returns TRUE,
`create` returns the object, and the array entry is named, in range and
non-NULL. Every reading taken so far — `s1/c0/f0`, `nch=63`, `hwn=25`, the
"VMPT" name — is equally true of a channel with `0xe8 == 0`. That is why
none of them could decide this, and saying so here rather than treating the
engine-2 chain as proof is the point.

**2gu's instrument** (build 62631971) adds two defined reads:

- `h=` — slot 25's `ch->0xe8`: `0` for no hardware channel, else the class
  name, `BAD` for a non-kernel pointer (absence is not a result).
- `e2=` / `e2q=` — `hw->0x3c0` read exactly as `AMDHardware::getHWChannel`
  reads it, and its queue count at `+0x48`, the field 2gi writes zero to.

What each outcome would mean, stated before the boot:

| `h=` | `e2q=` | reading |
|---|---|---|
| `0` | `0` | 2gi's bypass denied VMPT its channel; the stamp can never complete |
| `0` | non-0 | engine 2 has queues and VMPT still got nothing — 2gi exonerated, different fault |
| class | `0` | VMPT has a channel on a queueless engine — the "silently not backed" case 2gi hoped to avoid |
| class | non-0 | the channel and engine are both fine and the fault is downstream |

Only the first row would make 2gi the cause. The table is written down now
so the boot cannot be read into whichever row is convenient.

**Still open:** who calls `incrementStamp(25)`. `tot=1` says it is a single
call somewhere in startup; the 2gs shortlist did not hold and nothing has
replaced it.

## 2gv — VMPT holds a real SDMA channel on an engine with zero queues

**Boot 16 (62631971, 2gu).** `b=16` new on all four note channels; read in
the following unarmed boot, which is sound for the reason 2gu set out (the
nonce bumps on the first *note*, and an unarmed boot writes none).

```
fas:E(n=63|25:s1/c0/f0[AccelChannel "VMPT" h=MAChannel]
      out=1 tot=1 nch=63 hwn=25 nz=0 e2=1 e2q=0)
```

This is **row three** of the table 2gu wrote down before the boot:

| `h=` | `e2q=` | means |
|---|---|---|
| class | `0` | VMPT has a channel on a queueless engine — the "silently not backed" case 2gi hoped to avoid |

So: the channel exists, which is why nothing ever refuses the submit, and
the engine behind it has no queues, which is why nothing can ever complete
it. `tot=1` and `nz=0` from the same note bound it — one stamp ever issued,
no fence ever written.

**2gi is implicated, but not in the form 2gu's first row described.** The
bypass did not deny VMPT a channel; it left VMPT holding one whose engine
it had emptied.

### A mangled class name, caught here rather than later

`h=MAChannel` is not a class. The short-name loop walked to the *last* `'D'`
followed by an uppercase letter, so it ate into `"SDMAChannel"` itself. The
real class is **`AMDRadeonX6000_AMDSDMAChannel`**. Replaced with
`rbgpu_shortclass_2gv`, which strips only the fixed `"AMDRadeonX6000_"`
prefix and cannot over-trim. Recorded because a mangled name is a reading
that cannot be checked afterwards, which is the same failure mode as the
recurring error even though nothing was concluded from it.

### The index bases differ, and that is the finding

`rbgpu_walk_starthw_2fp` walks `hw + 0x3b8 + i*8`, while
`AMDHardware::getHWChannel` indexes `hw + 0x3b0 + engine*8`. So **2gi's
`engine[i]` is hw array index `i + 1`**:

```
hw[0] = Navi21PM4Engine        GFX
hw[1] = SDMAEngine_GC_10_3_0   2gi's engine[0] - LEFT ALONE, keeps queues
hw[2] = SDMAEngine_GC_10_3_0   2gi's engine[1] - zeroed        <- VMPT
hw[3] = SDMAEngine_GC_10_3_0   2gi's engine[2] - zeroed
hw[4] = SDMAEngine_GC_10_3_0   2gi's engine[3] - zeroed
```

VMPT's engine 2 is the **first** engine 2gi switches off, and the one
working SDMA sits at index 1. `one=3` says all three writes took, and
`rbgpu_sdma_one_2gi` only counts an engine whose count was non-zero
beforehand — so hw[2] *had* queues and 2gi removed them.

Apple's arithmetic makes hw[2] unavoidable: `RTHardware::getHWChannel`
computes `ENGINE_TYPE = ((caps->0x92 * 2 + 1) & idx) + 1`, and the VMPT
call site passes `prio=0`, which forces `idx = 1`. `x*2+1` is odd, so `&1`
is 1 and the answer is 2 whatever the caps byte holds. On a part with two
or more SDMA instances that is correct; here it names the engine that was
switched off.

### 2gv's experiment — build CF1786D5

Hook `AMDHardware::getHWChannel(engine, ring)` (0xbea9994) and send
**engine 2 to engine 1**, the single SDMA instance this chip has.

- Milestone 2c: `nm -m build/re/AMDRadeonX6000` shows the symbol as
  external **defined** at 0xbea9994, not an import.
- Rule 9: prologue `55 48 89 e5 89 f0 48 8b …` — `pushq %rbp`,
  `movq %rsp,%rbp`, `movl %esi,%eax`. No rel8 or rel32 branch begins inside
  the 5-byte patch window.
- Routed unconditionally; the **substitution** is gated on
  `-rbgpuvmpteng1`, so a boot without the arg still says the remap is OFF
  rather than leaving it ambiguous whether the route took.
- Every remap is logged as a substitution (2cy): a stamp that completes
  after this proves the remap works, **not** that engine 2 exists.
- `e1q` added to the note — engine 1's queue count, read the same way — so
  the remap target's state is measured rather than inferred from `one=3`.

Expected if the hypothesis holds: `e1q` non-zero, one `2gv:` substitution
logged, and slot 25 either completing (`nz` non-zero, `out=0`, the restart
loop gone) or failing somewhere new. If `e1q` reads 0 the remap target is
wrong and nothing about the result may be read as being about engine 1.

**Still open:** who calls `incrementStamp(25)`. Unchanged; `tot=1` still
says it is one call somewhere in startup.

## 2gw — the remap works: one stamp, Success, zero restarts

**Boot 17 (CF1786D5, 2gv, `-rbgpuvmpteng1` IN).** Captured over SSH **in the
armed boot** — the 2gq procedure, followed properly for the first time in
this stretch, and the only reason this milestone is readable at all.
`logs/20261017-032220-plugin/`.

```
rbgpu-prog  = b=17 s=6 0x1B RETURNED 0xE00002C7
rbgpu-1b    = b=17 ... hds:R fas:E(n=65|n=65 REFUSED(>64)) fas:R=0x26b4 ei:E ei:R
rbgpu-prog2 = b=17 a=s6/S12/L875@31694ms W=1 r0=0 rN=0 R=0 N=65 T=0
                   b=s6/S12/L876@51694ms W=1 r0=0 rN=0 R=0 N=65 T=0
                   c=s6/S12/L877@81694ms W=1 r0=0 rN=0 R=0 N=65 T=0
rbgpu-diag  = d=72 armed=1 ... (the full key set)
```

`rbgpu-diag` reading `armed=1` *in the live variable* (not `-prev`) is what
proves this capture is the armed boot's own.

### The frontier is broken

From `rbgpu_prog_action`: `W` is `waitForStamp`'s **call count**, `r0`/`rN`
its first and last return, `R` the **restart count**.

| | 2gu (boot 16) | 2gv (boot 17) |
|---|---|---|
| `waitForStamp` calls | 1 → 3 → 6, climbing | **1, flat across all three samples** |
| its return | `0xE00002D6` `kIOReturnTimeout` | **`0` `kIOReturnSuccess`** |
| restarts | 1 → 3 → 6, climbing | **0** |
| request 0x14 | reached #1,506,624 | — |

Both codes resolved from the SDK's own `IOReturn.h`, not from memory:
`iokit_common_err(0x2d6)` is `kIOReturnTimeout` and `(0x2c7)` is
`kIOReturnUnsupported`.

**The VMPT stamp completed.** `waitForStamp` ran once and the GPU wrote the
fence. `finishAllStamps` RETURNED (`fas:R=0x26b4`) instead of retrying
forever, `enableInterrupts` ran and returned (`ei:E ei:R`), and request 0x1B
returned. The GPU-restart loop that has been the frontier since 2gn is gone.

The log tail shows the machine **quiet**, not spinning: `logseq` advances
875 → 876 → 877 across the 31 s, 51 s and 81 s samples — one line per
sample, which is the sampler's own.

The accelerator then got FURTHER than any previous boot, reaching
framebuffer adoption, where 2ct refuses it by design:

```
2ct: found_framebuffer(fb=0xffffff8ba0777300) REFUSED -> accelerator adopts
     no display pipe, desktop stays on IONDRVFramebuffer
```

### The remap itself

17 substitutions, every one logged as a substitution (2cy), all from
`getHWChannel(engine=2, ring=0)`, returning two distinct real channels from
engine 1 (`0xffffff9539527000` and `0xffffff953952a800`). So engine 2 is
asked for a channel **seventeen times**, not once — VMPT is one caller among
several, and all of them were being handed an engine with no queues.

### WITHDRAWN: "2gv hangs the machine well before the frontier"

I read the *following, unarmed* boot's NVRAM, saw `rbgpu-prog = b=17 s=1
powerUpHW ENTER`, `rbgpu-sdma`/`rbgpu-1b`/`rbgpu-prog2` still at `b=16`, and
`rbgpu-diag-prev = d=27` with no `starthw_*` keys, and concluded the build
hung inside `powerUpHW`. **Every part of that is wrong.** The armed boot
reached `s=6`, wrote all four notes at `b=17`, and left a full `d=72` diag.

What I actually had was a power cycle eating the later commits — the exact
failure 2gq is written about, which I quoted in the same breath before
reasoning past it. I then took `last reboot` showing no clean shutdown
before 03:22 as corroboration, but a user rebooting a machine whose screen
has gone black looks identical to a hang, so it corroborated nothing.

**Twenty-fourth instance of the recurring error**, and the first caught by
the user rather than by the practice: the question "did you overwrite the
armed boot?" is what surfaced the SSH capture. Two concrete rules come out
of it:

- A stale note is not weak evidence of an early stop; it is **no evidence
  about where the boot got to**, and must not be combined with other stale
  notes into a story. Only `rbgpu-diag` reading `armed=1` *live* identifies
  a capture as the armed boot's.
- `plugin_check.sh` only reads NVRAM — there is no write in it — so running
  it in a later boot destroys nothing. The armed boot's capture survives in
  `logs/<timestamp>-plugin/` and should be looked for **before** reading
  live variables.

### The guard that cost the table

`fas:E(n=65|n=65 REFUSED(>64))`. The stamp count went 63 → **65** and the
dump's own bound refused to walk it, so the one boot that finally broke the
loop is the one whose stamp table went unread. 64 was an arbitrary round
number, not a fact about the structure. Raised to 256, still refusing out
loud. Why the table grew by two is **not guessed**.

### Where it stands now

Request 0x1B returns **`kIOReturnUnsupported`**. That is a new and much
later frontier: every stage of the chain returned, so the Unsupported comes
from the tail of Apple's handling, not from a stage that hung. The obvious
candidate is our own peer's `default:` answering some request Apple makes
late in 0x1B — the same shape as the 0x17 refusal 2gq recorded — but which
request is **not established** and must not be guessed.

Open, unchanged: who calls `incrementStamp(25)`; and now, why the screen
went black while SSH stayed up, on a boot where 2ct still refused the
display pipe.

## 2gy — 0x12 and 0x1D are the two requests we refuse inside 0x1B

**Boot 18 (63983BA1, 2gx).** Captured **in the armed boot**
(`logs/20261017-183233-plugin/`, `rbgpu-diag = d=72 armed=1` live — the
2gw rule applied and satisfied this time).

```
rbgpu-prog = b=18 s=6 0x1B RETURNED 0xE00002C7
rbgpu-1b   = b=18 ... fas:E(n=65|25:s1/c0/f0[AMDAccelChannel "VMPT"
             h=AMDGFX10SDMAChannel] out=1 tot=1 nch=65 hwn=25 nz=0
             e2=1 e2q=0 e1q=2) fas:R=0x2ecf ei:E ei:R q[12!,1D!|14x1]
```

The 2gx trail reads **`q[12!,1D!|14x1]`** and the log has the whole chain
in order, in one boot:

```
[6128026444] handlePostRegister: sending Request 0x1B (Controller Enabled)
[6128119588] SpecialAMDKey request 0x12 (wait: 1)
[6128121843] Unhandled SpecialAMDKey request 0x12
[6128142712] SpecialAMDKey request 0x1D (wait: 1)
[6128144315] Unhandled SpecialAMDKey request 0x1D
[6128889602] handlePostRegister: Request 0x1B returned 0xE00002C7
```

Inside the 0x1B window Apple asks the peer for **exactly two** things,
0x12 at +93 µs and 0x1D at +116 µs, we refuse both with
`kIOReturnUnsupported`, and 0x1B returns **that same code** at +863 µs.
Nothing else is asked of us in the window (`14x1` is one poll).

That is row one of the table 2gx wrote down before the boot: the trail
ends on a refused request. **It is still one boot and still correlation** —
Apple does something for ~745 µs after the 0x1D refusal before returning,
and nothing yet shows the return value is ours rather than its own.

0x12 and 0x1D are not new. They were seen "Unhandled" as far back as the
2cy-era boot, and 2gn localised where they arrive (0x12 around `rt:E`,
0x1D after `rt:R`). What is new is that they are the **only** two in the
window and that 0x1B's code equals the one we hand them.

### Confirmations that came with it

- **`e1q=2`** — engine 1 has two queues. The 2gv remap target is real, and
  the remap is no longer resting on `one=3` arithmetic.
- **`h=AMDGFX10SDMAChannel`** — the class prints correctly now; 2gw's
  `MAChannel` was the over-trimming bug, as recorded.
- **`n` and `nch` are both 65** while **`hwn` is still 25**, so the two
  channels the table gained since 2gu are past VMPT, not hardware ones.
  Why it grew is still not guessed.
- Slot 25 reads `out=1` at `fas:E` — that is *entry*, before the wait, and
  is consistent with 2gw: the stamp is outstanding on entry, the wait
  succeeds, `fas` returns (`0x2ecf` here, `0x26b4` in 2gw).

### A boot lost to a race, worth recording

The first attempt did not arm. The peer logged
`Peer: /options not found; accelerator NOT armed` at **0.52 s**, where the
2gw boot's peer ran at 1.9 s: `IORegistryEntry::fromPath("/options", ...)`
is tried **once**, and that boot it lost the race with the registry. The
failure is silent in the worst way — it looks exactly like "the user forgot
to arm", and `rbgpu-arm` is left set, so the next reboot armed with no
reinstall. A bounded retry around that lookup is owed.

### Why 0x12/0x1D are not simply answered yet

Answering means deciding what to write through `p2`, and nothing so far
says whether `p2` is an out-parameter at all. Inventing a value and then
reading the consequence as a hardware result is the 2cy failure exactly,
and the debt 2am/2an/2ao still owe.

**2gz (build F9623A20)** therefore looks before it answers: for 0x12 and
0x1D it logs `p2`, `p3`, `p4` and the first two dwords behind each that is
a kernel pointer, saying so explicitly when one is not read. Observe-only,
no boot-arg — each request arrives once per boot, so there is no storm.

Written before the boot:

| reading | means |
|---|---|
| `p2` a kernel pointer holding plausible input | Apple is asking a question; the answer must be computed, not invented |
| `p2` a kernel pointer holding zeros | likely a pure out-parameter, and the next step is what value it wants |
| `p2` null or not a pointer | the request takes no data and `kIOReturnSuccess` alone may be the whole answer |

**Open:** who calls `incrementStamp(25)`; why the stamp table grew by two;
the `/options` race; and whether 0x1B's `Unsupported` is ours or Apple's.

## 2gz / 2ha — 0x1B does not fail: 0xE00002C7 is its SUCCESS path

**Boot 19 (F9623A20, 2gz).** Captured **in the armed boot**
(`logs/20261017-191725-plugin/`, `rbgpu-diag = d=72 armed=1` live).

### What 2gz measured

```
2gz: req 0x12 args p2=0xfffffff2f514fd38 p3=0xfffffff2f514fd3c p4=0
2gz:   *p2 = 0x00000000 0x00000000
2gz:   *p3 = 0x00000000 0xDE118000
2gz:   p4 not a kernel pointer - NOT read
2gz: req 0x1D args p2=0xfffffff2f514fd88 p3=0xfffffff2f514fd8c p4=0
     (identical shape)
```

`p3` is exactly `p2 + 4`: two adjacent dwords on the caller's stack, both
zeroed, `p4` null. The same shape as request 0x14, where the peer writes
its answer into `p2`. Row two of 2gy's table — they look like
out-parameters. **That turned out not to be the question worth answering.**

### WITHDRAWN: "the new frontier is 0x1B returning kIOReturnUnsupported"

Recorded in 2gw and repeated in 2gy and in `docs/HANDOFF.md`. It is wrong.

`AMDGraphicsAccelerator::callPlatformFunction` (0xbe4154e) preloads its
return before dispatching:

```
movl  (%r14), %eax           ; req = *p1
movl  $0xe00002c7, %r14d     ; DEFAULT return = kIOReturnUnsupported
decl  %eax
cmpl  $0x1b, %eax
ja    0xbe41aa8              ; out of range -> return the default
leaq  0x515(%rip), %rcx      ; jump table at 0xbe41ad4, index req-1
jmpq  *%rax
```

and the common exit at 0xbe41aa8 is `movl %r14d, %eax; ...; retq`.

The jump table (decoded from the binary, index `req-1`, base 0xbe41ad4)
gives 0x1B a real case at **0xbe417d0**, while 0x02, 0x0B-0x0F, 0x12-0x18
and 0x1A all land on the default.

Inside the 0x1B case, after printing
`"%s: Controller is enabled, finish initialization\n"`:

```
0xbe41863  movl  $0xe00002d8, %r14d   ; kIOReturnNotReady - the FAILURE value
0xbe4186c  je    0xbe418d5            ; hw interface NULL -> NotReady
0xbe41871  callq *0x2a0(%rax)         ; isDeviceValid
0xbe4187d  je    0xbe418d5            ; FALSE -> NotReady
0xbe41889  callq *0x4c8(%rax)         ; startGlobalPerfCounters
0xbe4189d  callq *0x130(%rax)         ; hardwareDidStart
0xbe418b1  callq *0x158(%rax)         ; (accel->0x380)->finishAllStamps
0xbe418c5  callq *0x218(%rax)         ; enableInterrupts
0xbe418cf  movl  $0xe00002c7, %r14d   ; <-- the COMPLETED path sets this
0xbe418d5  ... cleanup ... jmp 0xbe41aa8
```

**Both bail paths return 0xE00002D8. Only the path that executes all five
hardware calls restores 0xE00002C7.** So the code we have been reading as
a failure is the one value that *proves* the whole body ran. Apple reuses
the preloaded constant as "done, nothing further"; whatever the intent,
the control flow is not ambiguous.

Every slot resolves, and they reproduce the 2gn trail exactly:

| call | method | trail mark |
|---|---|---|
| `hw->vt[0x2a0]` | `isDeviceValid` | — |
| `hw->vt[0x4c8]` | `startGlobalPerfCounters` | `perf:E perf:R` |
| `hw->vt[0x130]` | `hardwareDidStart` | `hds:E … hds:R` |
| `(accel->0x380)->vt[0x158]` | `finishAllStamps` | `fas:E … fas:R` |
| `hw->vt[0x218]` | `enableInterrupts` | `ei:E ei:R` |

Two independent sources — the live trail and the disassembly — give the
same five calls in the same order, all returning. This also names 2gn's
"unnamed `(accel->0x380)->vt[0x158]`" as `finishAllStamps`, agreeing with
2gp.

**Request 0x1B completes.** Since 2gw broke the restart loop, the
accelerator has been finishing its initialisation every boot.

### What that costs, and what it saves

The error: 2gw took a numeric return code and assigned it a meaning —
"Unsupported, therefore a failure" — without reading the code that
produces it. The constant's *name* was treated as its *role*. The peer's
own `default:` returning the same value made the story fit, and 2gy then
built a second milestone on top of it. **Twenty-fifth instance of the
recurring error.** The rule it adds:

- A return code is not a result until the code that sets it has been
  read. A named constant tells you what the name means, not what the
  function meant by it — and an error-shaped value on a success path is
  exactly the case where the name misleads.

What it saves: 0x12 and 0x1D are **incidental**. They arrive in the first
~120 µs of the 0x1B case, before the hardware sequence, we refuse both,
and Apple runs the entire path anyway. Answering them is **not** required
for 0x1B to complete, and the 2gz follow-up that was about to be built is
unnecessary. They remain unexplained, but they are not blocking anything.

### Where this actually leaves the project

Nothing in the 0x1B path is now known to fail. The accelerator initialises,
the GPU executes and fences, the restart loop is gone. What has never
happened is the accelerator driving anything: **2ct refuses the display
pipe by design** (`found_fb_calls=2 found_fb_refused=2`), so the desktop
stays on IONDRVFramebuffer and no boot has run with the accelerator
adopting the panel. That refusal, not 0x1B, is the next thing in the way —
and lifting it is a decision with a real cost (it is what keeps the UI
alive), so it is the user's call, not a step to take unasked.

Still open: who calls `incrementStamp(25)`; why the stamp table grew
63 → 65; the `/options` race; the one black-screen boot; and what 0x12 and
0x1D are for.

## 2hb — correction: armed boots have NEVER had a display

Stated by the user on 2026-10-17, from direct observation across the
whole project:

> every boot from when we made from like 2c... went black with only log
> lines... or no display... ive been doing SSH since... no boot had a
> display in armed at all

This corrects several things written in 2gw, 2gy and 2ha:

- **"The screen went black while SSH stayed up" is not new behaviour.**
  2gw recorded it as a new, unexplained symptom of that boot. It has been
  the case for every armed boot since roughly milestone 2c.
- **It is not intermittent.** Shortly before this correction I said boots
  17 and 20 went black while 18 and 19 were fine. That was inferred from
  gaps in `last reboot` — a reboot with no preceding shutdown record — and
  it was wrong. Boots 18 and 19 were read in the *unarmed* boot that
  followed; the user's "the screen didn't stay black this time" referred
  to that unarmed boot, not to the armed one. Every armed boot is black.
  Same error shape as 2gw: reading reboot bookkeeping as if it reported
  what the user saw, when only the user could say.
- **2ct's stated rationale is not achieved.** Its comment says the pipe is
  refused "so armed boots can reach a desktop", and `found_fb_refused=2`
  confirms the refusal happens — but the desktop does not come up anyway.
  Whatever kills the panel is a *separate* cause from display-pipe
  adoption, and it is unidentified.

What is NOT in doubt: the machine stays healthy in an armed boot. SSH
works, `plugin_check.sh` runs, `system_profiler` runs, the 0x1B chain
completes. Only the panel is dead.

### Boot 20 (F9623A20, armed)

`b=20 s=6 0x1B RETURNED 0xE00002C7` — the 2ha success value, so the whole
0x1B body ran again. No capture was saved before the power cycle, so only
the NVRAM notes survive.

In that boot the user ran `system_profiler SPDisplaysDataType` over SSH
and reported **`Metal: Supported`**. The control is good: the same command
in the *unarmed* boot prints the plain IONDRVFramebuffer entry (`VRAM
(Total): 7 MB`, device 0x1681) with **no `Metal:` line at all**, so the
line is attributable to our accelerator and not to the firmware
framebuffer.

**It is still a reported line, not a measurement I have seen, and
`Metal: Supported` does not establish that any GPU work executes.** The
armed registry (2gz capture) shows the accelerator published in the
`IOAccelerator` category with `MetalPluginName = AMDRadeonX6000MTLDriver`,
`MetalPluginClassName = AMDMTLGFX10Device`, `IOGLBundleName =
AMDRadeonX6000GLDriver`, `IOAccelRevision = 2`, `dpm = 1`, plus a live
`AMDAccelSharedUserClient` whose `API` property reads `Metal` and whose
`IOUserClientCreator` is `pid 162`. A client attaching is not a kernel
dispatching, and neither is a capability string.

### What the next armed boot must do differently

These boots end in a power cycle and take their logs with them, so the
procedure changes: **write every result to a file under `logs/` as it is
produced**, rather than reading it on screen. Milestone 2hc builds that.

## 2hd — the first Metal device, and newCommandQueue is where it stops

**Boot 22 (F9623A20, armed).** Five captures, all taken **in the armed
boot** — each `state.txt` records `d=72 armed=1` live, the 2gw rule
satisfied by construction this time because `metal_check.sh` writes it
first. `logs/20261017-2001*-metal/` and `logs/20261017-200249-metal/`.

### Metal enumerates the 680M

Four enumerate-only runs, identical, `exit=0`:

```
MTLCopyAllDevices: 1 device(s)

device: AMD Radeon HD GFX10 Family Unknown Prototype
  registryID      = 0x10000054f
  lowPower        = 0
  removable       = 0
  headless        = 0
  unifiedMemory   = 0
  maxThreadsPerTG = 1024
  recommendedMax  = 4096 MB
```

Against the control measured in the unarmed boot: **`MTLCopyAllDevices: 0
device(s)`**, and `system_profiler` printing the plain IONDRVFramebuffer
entry with no `Metal:` line at all. So the device is attributable to this
project's accelerator and nothing else, and 2hb's reported
`Metal: Supported` is confirmed by a second, independent route.

The name is Apple's generic GFX10 fallback — "Unknown Prototype" — which
is what an unrecognised device id yields; it is not a sign of anything
being wrong. `maxThreadsPerTG = 1024` and a 4 GB recommended working set
are plausible and come from the driver, not from us.

### Where it stops

The `--dispatch` run ends mid-step:

```
  dispatch: creating queue
```

and nothing after it — no `dispatch: compiling shader`, and no
`metal_probe exit=` line, which `metal_check.sh` appends only after the
probe returns. That print is `fflush`ed immediately before
`[dev newCommandQueue]` and the next print happens immediately after it,
so the probe **never returned from `newCommandQueue`**. First real GPU
operation a client asks for, and it does not come back.

No panic report was written (newest is 2026-10-15), so this is a hang, not
a crash.

**Not established:** whether the whole machine wedged or only the process
blocked. The user power-cycled, and `last reboot` cannot tell those apart
— which is exactly the 2hb/2gw error, so it is left open rather than
inferred.

### Why this is a plausible place to stop

A command queue is the first thing that needs a *client-facing* ring, as
opposed to the KIQ this project has been driving. Everything proven so far
runs through SDMA and the KIQ: `powerUpHW` (2gi), the VMPT stamp on SDMA
engine 1 (2gv/2gw), the PM4 fences. The GFX engine has never been shown to
execute anything, and the standing 2di debt — `gfx_min = gfx_max = 0`, GFX
DPM never started — has sat unexplained since 2026-10-12 precisely because
nothing had ever asked the GFX pipe to do work. A command queue asks.

That is a hypothesis, not a finding. What it predicts and what would
distinguish it is for the next milestone to write down before measuring.

### The procedure worked

Five captures survived a hang that took the machine down, because
`metal_check.sh` writes each step to `logs/` and `sync`s before the next.
The enumerate/dispatch split is what makes the stopping point legible: the
four clean runs establish that everything up to queue creation works, so
the one truncated file locates the failure to a single call. Had this been
one combined probe, the result would have been "it hung somewhere".

**Open:** whether the hang is machine-wide; what `newCommandQueue` is
waiting on; who calls `incrementStamp(25)`; why the stamp table grew
63 → 65; the `/options` race; why armed boots have no display; what 0x12
and 0x1D are for.

## 2he — the hang is process-local, so it can be sampled

Asked rather than inferred, because 2gw and 2hb were both cases of
inferring exactly this kind of thing from reboot bookkeeping. The user:

> Only that command hung; SSH still worked and I could run other things.

So 2hd's open question is settled: **the whole machine does not wedge.**
`newCommandQueue` blocks one client thread in the kernel while the rest of
the system stays responsive.

That is a better position than 2hd assumed, in three ways:

- **The GPU is not hard-hung.** A wedged GPU would take the KIQ and the
  rest of the stack with it; nothing else was disturbed.
- **No power cycle is needed to study it.** The boot survives, so a hung
  probe can be left in place and examined while it hangs.
- **The blocked thread has a stack, and the stack names the call.** A
  thread sitting in the kernel still shows its userspace frames down to
  the trap, which identifies the exact selector into the accelerator
  rather than leaving "somewhere in newCommandQueue".

### What changed in the instrument

`metal_check.sh --dispatch` now runs the probe in the background with a
bounded 25 s wait, and if it is still alive:

- `sample <pid> 3` into `sample.txt` — userspace stacks, no root needed.
- `spindump <pid> 3` into `spindump.txt` for kernel stacks **when run as
  root**; when not root it writes a line saying so rather than leaving an
  empty file, because an empty file and "spindump found nothing" must not
  look alike.
- Leaves the probe running and prints the pid, so the state is still there
  to poke at.

Everything is still `sync`ed step by step, since the earlier reason for
that (a boot that may not survive) has not changed, only become less
likely.

### Next, in order

1. An armed boot, `sudo ./tools/metal_check.sh --dispatch`, for the
   userspace **and** kernel stacks of the blocked thread. Zero kext
   changes; the stack should name the IOConnect selector and the kernel
   function it waits in.
2. Only then instrument the kext, with the stack saying where — rather
   than hooking `AMDAccelCommandQueue::initSubmitChannels` on the strength
   of 2hd's GFX-ring hypothesis, which remains a hypothesis.

Doing (1) first is the point: it is free, it is non-invasive, and it tells
us where to put the hook that (2) would otherwise have to guess.

## 2hf — the hang is `fEnabled`, and 2ct is why it is never set

**Boot 23 (F9623A20, armed).** `logs/20261018-001306-metal/`, `state.txt`
recording `d=72 armed=1` live. The 2hd hang reproduced exactly
(`dispatch: creating queue`, nothing after), 2he's sampler fired, and the
user ran it under `sudo`, so both stacks are present.

### The userspace stack names the call

```
newCommandQueue
 → amdMtl_Device_LazyInit → GFX10_MtlDevice amdMtl_LazyInit
  → amdMtl_InitUBMInterface → UBMCreate
   → BltMgrContainer::Create → BltMgr::Init → Gfx10BltMgr::HwlInit
    → Gfx10ShaderVidMemMgr::CpuLoadInitialShaders
     → ShaderVidMemMgr::AllocVidMemForShaders → BltMgrBase::AllocVidMem
      → amdMtlAllocateBuffer → MTLIOAccelBuffer → MTLIOAccelResource
       → IOAccelResourceCreate → IOConnectCallMethod → mach_msg2_trap
```

`newCommandQueue` lazily builds the blit manager, which loads its initial
shaders, which needs video memory. The first buffer allocation a Metal
client ever makes goes into the kernel and does not come back.

### The kernel stack names the reason

`spindump` gave unsymbolicated frames; resolved against
`build/re/IOAcceleratorFamily2` (`__TEXT` vmaddr 0x14e13000, offsets as
spindump prints them):

```
+187908  IOAccelSharedUserClient2::externalMethod                  +0x82
+187401  IOAccelSharedUserClient2::s_new_resource                  +0xa7
+179561  IOAccelSharedUserClient2::new_resource                    +0x1fd
+271009  IOGraphicsAccelerator2::acceleratorWaitEnabled            +0x8f
+271091  waiting_for_fEnabled(_IOLock*, IOGraphicsAccelerator2*)   +0xb
         ... kernel sleep primitives
```

`acceleratorWaitEnabled` (0x14e55212):

```
movl  0xc78(%rdi), %eax     ; flags word at accel+0xc78
testb $0x2, %al             ; bit 0x2 == enabled
jne   0x14e552c0            ; set -> return at once
...
callq waiting_for_fEnabled  ; 0x14e5529c; +0x8f is the return address here
```

The `+0x8f` frame is 0x14e552a1, the instruction after that call, so this
is the plain `waiting_for_fEnabled` path — not the
`_after_poweredOffByAGC` or `_after_displayModeChange` variants.

### What sets the bit, and who calls it

A scan of every write to `0xc78` in IOAcceleratorFamily2 gives exactly one
setter of bit 0x2:

```
IOGraphicsAccelerator2::enableAccelerator()   0x14e5558e  orb $0x2, 0xc78(%rbx)
IOGraphicsAccelerator2::disableAccelerator()  0x14e555ba  andb $-0x3, 0xc78(%rbx)
```

and `enableAccelerator` has exactly **two** callers in the kext:

```
IOAccelDisplayMachine::display_mode_did_change(unsigned int)
IOAccelLegacyDisplayMachine::display_mode_did_change(unsigned int)
```

**The accelerator is enabled by a display mode change, and nothing else.**

### The chain, end to end

```
2ct refuses found_framebuffer  (by design, found_fb_refused=2)
 → the accelerator adopts no display pipe
  → no IOAccelDisplayMachine ever sees a mode change
   → enableAccelerator() is never called
    → fEnabled (bit 0x2 at accel+0xc78) is never set
     → acceleratorWaitEnabled() sleeps in waiting_for_fEnabled
      → IOAccelSharedUserClient2::new_resource never returns
       → Metal's first buffer allocation hangs
        → newCommandQueue hangs
```

Every link is read out of Apple's code; none is inferred from a return
code or a timing coincidence. It also retroactively explains 2gq's
`IOAccelDisplayMachine::waitForStampTimeout -> FALSE (pipe count 0)` — the
same absent display machine, seen from the other side.

### 2hd's hypothesis is withdrawn

2hd guessed the stop would be a client-facing **GFX ring**, tied to the
2di debt (`gfx_min = gfx_max = 0`). It is not. Nothing in the stack
touches a ring, an engine or a queue; the thread never reaches hardware at
all. It was labelled a hypothesis and not acted on, which is the only
reason it cost nothing — had 2hd hooked
`AMDAccelCommandQueue::initSubmitChannels` on the strength of it, the hook
would have been in a function the thread never enters.

The 2di debt is untouched by this and remains open on its own terms.

### What this changes about 2ct

2ct has been carried as a *standing debt* and, in the 2ha handoff, as "the
next thing in the way" with the caveat that lifting it "gains less than
hoped". That caveat was wrong. 2ct is not merely in the way of the panel;
it is the identified cause of the Metal hang.

Two directions, and they are genuinely different:

1. **Lift 2ct** — let the accelerator adopt the display pipe, so a real
   mode change enables it. Also the riskiest: Apple's Navi 21 display code
   is DCN 3.0 and this chip is DCN 3.1.2, and nothing here has ever
   initialised DCN.
2. **Enable without the panel** — call `enableAccelerator()` once after
   start, leaving 2ct refusing. If resource creation then proceeds, Metal
   works headlessly and DCN is never touched. This is a substitution and
   owes 2cy's discipline: it must be logged as one, and a stamp completing
   afterwards proves the flag gated it, **not** that a display exists.

(2) is the smaller, more informative step and does not foreclose (1).
`enableAccelerator` is an external **defined** symbol in
IOAcceleratorFamily2 (milestone 2c satisfied by the right kext id), so it
can be called rather than faked — preferable to writing bit 0x2 ourselves,
since Apple's function may do more than set the flag.

**Open:** what else `enableAccelerator` does besides the `orb`; whether a
headless-enabled accelerator can actually allocate; why armed boots have
no display; the `/options` race; `incrementStamp(25)`; the 63 → 65 table.

### Addendum — what `enableAccelerator` actually does, and the lock

```
__ZN22IOGraphicsAccelerator217enableAcceleratorEv:   0x14e55570
  testb $0x8, 0xc92(%rdi)
  jne   0x14e5558e
  movq  0x380(%rbx), %rdi
  callq IOAccelEventMachine2::startHardwareProgressTimerUnlocked()
  orb   $0x2, 0xc78(%rbx)
  retq
```

Two facts that matter for option (2):

- **It is not display-specific.** It starts the event machine's hardware
  progress timer (`accel->0x380` is the AMDAccelEventMachine, per 2gp) and
  sets the bit. Nothing in it touches a pipe, a panel or DCN. That is what
  makes enabling headlessly plausible rather than merely convenient.
- **`...Unlocked` means a lock is expected to be held already.** Both real
  callers are inside `display_mode_did_change`, which holds the
  accelerator lock; `acceleratorWaitEnabled` sleeps on `accel->0x88`, so
  that is the lock in question. Calling `enableAccelerator` from a hook
  without holding `accel->0x88` would be calling an unlocked-variant
  function with no lock — the shape that produced the 2fz/2ga and 2gg
  panics. **Any attempt must take `accel->0x88` first**, and must say in
  the log whether it got it.

So option (2) is not a one-line call. It is: take `accel->0x88`, call
`enableAccelerator()`, release, and log all three steps — with the
substitution named as one (2cy), since a Metal allocation succeeding
afterwards proves the flag gated it and nothing more.

## 2hg — enable the accelerator without a display pipe (build 23B6A724)

The experiment 2hf pointed at, option (2): leave 2ct refusing and set the
gate directly, so Metal can allocate without ever touching DCN.

**Solved, not routed** (`processAccelFamily`, under IOAcceleratorFamily2's
own kext id per milestone 2c — the symbol is external and defined there,
not in X6000):

```
__ZN22IOGraphicsAccelerator217enableAcceleratorEv  ->  g_rb_enableAccelerator_2hg
```

Apple's function is called rather than the bit being written, because it
does more than the `orb`: it starts the event machine's hardware progress
timer first.

**Where:** in the peer's `handlePostRegister`, immediately after request
0x1B returns. That is outside Apple's call stack, on our own thread, at
the latest point in the sequence 2ha mapped — so there is no reentrancy
into a path that is mid-flight.

**The lock is taken.** `enableAccelerator` tail-calls
`startHardwareProgressTimerUnlocked`, and `Unlocked` means the caller
holds the lock; both real callers run inside `display_mode_did_change`,
which does. `acceleratorWaitEnabled` sleeps on `accel->0x88`, so that is
the lock. The hook reads it, and **refuses to call** if it is not a kernel
pointer — calling an unlocked-variant function with no lock is the shape
that produced the 2fz/2ga and 2gg panics.

**Every path says something** (absence is not a result): symbol unsolved,
no accelerator, implausible lock, and already-enabled each log a distinct
line and set `ea_called=0`, so a boot that changes nothing cannot be read
as "enabling did not help".

**It is a substitution and is logged as one** (2cy). The log line says in
words that this is the plugin setting the gate, not a display. If a Metal
allocation succeeds afterwards it proves the FLAG was the gate — nothing
more, and nothing downstream may be narrated as if a display exists.

Off unless `-rbgpuforceenable`. New note `rbgpu-ea` carries
`ea:<before>-><after>` for the flags word; `plugin_check.sh` now reads it.

### Written before the boot

| reading | means |
|---|---|
| `ea_solved=0` | the symbol never resolved; the boot-arg did nothing and the Metal result is unrelated to this hook |
| `ea_called=0` with "already enabled" | something else set bit 0x2 first — and that would itself be news, since 2hf says only `display_mode_did_change` can |
| `ea_called=0` with "lock is not a kernel pointer" | `+0x88` is not what 2hf read it as; the offset is wrong and nothing was called |
| `ea:XXXXXXX0->XXXXXXX2`, Metal still hangs | the flag was not the only gate; the stack from the next `--dispatch` says where it moved to |
| `ea:...->...2`, Metal allocates | **the flag was the gate.** Headless Metal, with 2ct still refusing and DCN untouched |

The last row is the one worth having: it would mean compute works without
the display problem being solved at all.

## 2hh — the flag WAS the gate: a display, a login, and the real frontier

Boot of 2026-10-18 00:41, armed, build `23B6A724`, first boot ever with
`-rbgpuforceenable`. **The user had a display.** Lock screen drawn,
password accepted, the session-loading indicator appeared — then it
froze. The machine panicked ~2m43s in, before SSH could be reached.

This is the **first armed boot with a panel since roughly 2c** (2hb),
and it did not come from touching DCN, the display pipe or 2ct. It came
from setting one bit.

### What the boot recorded

`rbgpu-ea` = `b=24 ea:00000000->00000002`. The bottom row of 2hg's
pre-written table, exactly: `enableAccelerator()` solved, the lock at
`accel+0x88` was a plausible kernel pointer, the call was made under it,
and `accel+0xc78` went `0x00000000 -> 0x00000002`. **`fEnabled` is
settable with no display pipe.**

The rest of the trail is 2ha's unchanged: `rbgpu-prog` `s=6 0x1B
RETURNED 0xE00002C7` (the success value), `rbgpu-diag-prev` `d=73
armed=1`, `fas:R=0x2104c`, `ei:E ei:R`. Boot-args in the spin report's
own header, untruncated, confirm `-rbgpuvmpteng1 -rbgpuforceenable`.

`rbgpu-prog2` is the one that moved: `W=2 ... T=12` @21s, `W=6 ... T=34`
@41s, `W=11 ... T=56` @71s, `R=0`, `N=65` throughout. Waits and timeouts
climbing linearly against a flat stamp table and **zero restarts at
those sample points** — a client issuing stamps that never retire.

### Why no armed boot ever had a display — answered

2hf found WindowServer's first `IOAccelResourceCreate` asleep in
`waiting_for_fEnabled` forever. That is not merely "Metal hangs": it is
**WindowServer's first allocation**, so WindowServer never drew
anything. The black screen of every armed boot since 2c was the same
`fEnabled` gate, not a DCN or panel fault. With the bit set,
WindowServer allocated, composited and drew the lock screen on
IONDRVFramebuffer.

2ct's rationale is therefore *half* restored: refusing the display pipe
does leave the desktop usable — it was the unset gate, downstream of the
refusal, that killed it. The rationale was right about the mechanism it
intended and wrong about the outcome, for a reason two milestones away.

### The new frontier, fully symbolicated

`watchdogd` panicked on `no successful checkins from WindowServer
(2 induced crashes) in 120 seconds`. Two
`userspace_watchdog_timeout.spin` reports were saved (00:43:19 and
00:43:58) and are in `logs/20261018-004318-WSHANG-ARMED-2hg/`.

Seventeen of WindowServer's eighteen threads — the main thread and every
`com.apple.root.default-qos` worker — are in the same place:

```
QuartzCore (CA render tree) -> IOSurface.framework -> IOKit
  iokit_user_client_trap
  IOSurfaceRootUserClient::lock_surface      +0x89
  IOSurface::lockSurface                     +0xb1
  IOSurface::synchronize_device_caches       +0x8a
  lck_mtx_sleep -> thread_block -> asleep
```

`+0x8a` is the return address of the `IOLockSleep` inside

```c
while (surface->0x210 != 0) {        // device-cache work in flight
    surface->0x214++;                // waiter count
    IOLockSleep(surface->0x50, surface, THREAD_UNINT);
    surface->0x214--;
}
```

so those seventeen are merely queued. **One** thread holds it, at
`IOSurface + 34448` = `synchronize_device_caches +0x13a`, the return
address after `IOSurfaceDeviceCache::wait()` in the busy section:

```c
surface->0x210++;  IOLockUnlock(surface->0x50);
cache->pageOff();  cache->wait(flag);  cache->accelRelease();
IOLockLock(surface->0x50);  surface->0x210--;  IOLockWakeup(...);
```

Below `wait()`, in the first spin:

```
AMDRadeonX6000_AMDAccelEventMachine::finishEventUnlocked  +0x56
IOAccelEventMachineFast2::finishEventUnlocked             +0xe7
[0x0]
AMDRadeonX6000_AMDAccelEventMachine::waitForStamp         +0x230
  7/12 samples:
    AMDRadeonX6000_AMDAccelChannel::waitForTimestamp      +0x121
    AMDRadeonX6000_IAMDHWInterface::pollWait              +0x71
    thread_block -> machine_switch_context
  1/12 sample:
    AMDRadeonX6000_AMDAccelChannel::waitForTimestamp      +0x240
    AMDRadeonX6000_AMDGFX10PM4Engine::verifyThreadsActive +0x2c
    AMDRadeonX6000_AMDGFX10PM4Engine::QueryQueueStatus    +0x163
```

and in the second spin, 40 s later, the same thread has moved on to

```
IOAccelEventMachine2::hardwareErrorEvent  +0x1b4
IOAccelEventMachine2::restart_channel     +0x73 / +0xeb
IOAccelFIFOChannel2::restart              +0x322
AMDRadeonX6000_AMDAccelChannel::resetHardwareAndReplay +0x212
```

**So: `waitForStamp` times out on a real client stamp, and the event
machine enters the GPU-restart path.** This is 2gq's restart loop again
— but this time on the client-facing GFX/PM4 path, not the VMPT/SDMA one
`-rbgpuvmpteng1` fixed, and issued by WindowServer rather than by the
driver's own bring-up.

### What the frontier now is, stated narrowly

A surface the compositor asks to sync needs a device-cache operation;
that operation is submitted and its stamp never retires; `waitForStamp`
times out; the channel restarts; it never recovers; every other
WindowServer thread piles up behind `surface+0x210`; at 120 s the
userspace watchdog panics the machine.

`QueryQueueStatus` and `verifyThreadsActive` on `AMDGFX10PM4Engine` put
this on the **PM4/GFX ring** — hw engine `[0]`, untouched by
`-rbgpusdmaone` and by `-rbgpuvmpteng1`. That is the first time this
project has had a client submit to the GFX ring at all, and it is the
first evidence that bears on the **2di debt** (`gfx_min = gfx_max = 0`
since 2026-10-12, GFX DPM never started). 2di is no longer "never
explained; not yet shown to matter" — a GFX ring that does not retire
stamps is exactly what a GPU with no GFX clock range would look like.
That is a hypothesis, labelled as one, and nothing is to be built on it
until a measurement supports it.

### Operational consequence

`-rbgpuforceenable` makes the machine **self-panic in under three
minutes**, with no SSH window in practice. It is not safe to leave armed
unattended. It stays a deliberate, supervised experiment, not a
permanent boot-arg, until the stamp that hangs is understood.

The clean way to use it again is to pair it with something that makes
the boot survive the watchdog — or to accept the panic and read the
spin reports, which is what produced this milestone and cost nothing
but a reboot.

## 2hj — one stuck thread: the compositor was accelerated, and the GPU retires nothing

Boot of 2026-10-18 ~01:03, armed, build `23B6A724`, boot-args
`-rbgpuforceenable wdt=-1`. `state.txt` records `rbgpu-diag` = `d=73
armed=1` **live** and `rbgpu-boot = 25`, so this capture is the armed
boot's by the 2gw rule. Captured under `sudo` from SSH:
`logs/20261018-010521-metal/`.

`wdt=-1` did what reading `IOWatchdog::start` said it would: bit 0 of the
`wdt` boot-arg takes the "watchdog disabled by boot-arg" path and zeroes
the monitoring config at `+0x89/+0xa8/+0xac`. **No panic.** The machine
froze and stayed up, SSH stayed usable, and for the first time the
frontier could be measured live instead of reconstructed from a crash
report.

### The gate is open — confirmed, not assumed

2hf found WindowServer's `IOAccelSharedUserClient2::new_resource` asleep
at **+0x1fd** in `acceleratorWaitEnabled` -> `waiting_for_fEnabled`. In
this boot the same function is at **+0x1cc**, and the spindump annotates
it `blocked by turnstile waiting for kernel_task [0] thread 0x9a7` — an
ordinary mutex with a named holder, not the `fEnabled` sleep. `rbgpu-ea`
reads `b=25 ea:00000000->00000002`.

So 2hh's reading holds on a second boot and from a different angle: the
bit is set, the gate is open, and what blocks now blocks for a different
reason.

### The compositor was running on the GPU

The user reported the lock screen's password field rendering **grey and
translucent** rather than the flat black of an unarmed boot. The
WindowServer spindump says why, in its own frames:

```
_XHWCaptureDesktop
WS::Capture::create_iosurface_for_window_list
CaptureSurfaceMetal::Populate
MetalBacking::CreateCompositeDestination
MetalIOSurfaceBacking::PrepareForUse
-[GFXAAMD_MtlDevice newTextureWithDescriptor:iosurface:plane:]   <- AMDRadeonX6000MTLDriver
-[GFXAAMD_MtlTexture initIOSurfaceWithDevice:...]
-[MTLIOAccelTexture initWithDevice:...]
IOAccelResourceCreate
```

**WindowServer is compositing through Metal on `AMDRadeonX6000MTLDriver`
— this GPU's own driver.** That is the measurement; the translucency is
what that looks like on screen. Stated separately on purpose: the stack
is the evidence, the colour is the user's observation of its effect, and
neither is being used to prove the other.

### Metal reached the actual dispatch

`metal.txt`, in order, every line written before the step it names:

```
MTLCopyAllDevices: 1 device(s)
device: AMD Radeon HD GFX10 Family Unknown Prototype
  dispatch: creating queue        <- returned
  dispatch: compiling shader      <- returned
  dispatch: encoding 4096 threads <- returned
  dispatch: committing - IF THE GPU HANGS, IT HANGS HERE
metal_probe STILL RUNNING after 25s
```

**`newCommandQueue` succeeds.** The 2hd frontier and the 2hf cause are
both gone. The probe's main thread sits in
`-[_MTLCommandBuffer waitUntilCompleted]`, and the work was genuinely
submitted — its `com.Metal.CommandQueueDispatch` thread is down in the
kernel:

```
IOAccelCommandQueue::s_submit_command_buffers          +0xfc
IOAccelCommandQueue::submit_command_buffers            +0x314
IOAccelCommandQueue::submit_command_buffer             +0x102
IOAccelCommandQueue::process_command_buffer            +0x402
AMDAccelCommandQueue::processCommandBuffer             +0x62
IOAccelCommandQueue::processCommandBuffer              +0x565
IOAccelCommandQueue::processAndSubmitCoalescedSegments +0x6a
AMDAccelCommandQueue::endCoalescedSegment              +0xe
AMDAccelCommandQueue::submitCommands                   +0x93
AMDAccelChannel::submitCommands                        +0x1d
IOAccelFIFOChannel2::submitCommands                    +0x8f
AMDAccelChannel::waitForRingBufferSpace                +0x163   <- the ring is FULL
AMDUserQueue::prepareQueue                             +0x9f
AMDAccelEventMachine::finishEvent                      +0x56
IOAccelEventMachineFast2::finishEvent                  +0xee
IOAccelEventMachine2::handleFinishChannelRestart       +0xf7
IOAccelEventMachine2::restart_channel                  +0xeb
IOAccelFIFOChannel2::restart                           +0x322
AMDAccelChannel::resetHardwareAndReplay                +0x212
AMDAccelChannel::waitForTimestamp                      +0x121
IAMDHWInterface::pollWait                              +0x71
thread_block                                            (291/301 samples)
```

The remaining 4 samples catch it in the poll itself —
`waitForTimestamp +0x151` -> `AMDSWScheduler::checkTimestamps` ->
`AMDHWChannel::checkForTimestampUpdate` -> `AMDSWScheduler::timestampUpdated`
— so this is a live poll for a timestamp, not a dead sleep. The GPU is
being asked, repeatedly, and never answers.

### One cause, three victims

The turnstile named the holder, and it is a dedicated kernel thread:

```
Thread 0x9a7  "AMDRadeonX6000_AMDAccelEventMachine"  priority 81
  IOAccelEventMachine2::hardwareErrorEvent  +0x1b4
  IOAccelEventMachine2::restart_channel     +0xeb
  IOAccelFIFOChannel2::restart              +0x322
  AMDAccelChannel::resetHardwareAndReplay   +0x212
  AMDAccelChannel::waitForTimestamp         +0x121
  IAMDHWInterface::pollWait                 +0x71
  thread_block                               (286/300 samples)
```

The same five frames as the Metal submit thread, and the same five frames
as 2hh's WindowServer surface-sync holder. So:

- the **event-machine thread** is stuck in `resetHardwareAndReplay`
  waiting for a timestamp,
- **WindowServer** is stuck on the lock that thread holds,
- the **Metal probe** is stuck behind the same restart, reached through a
  full ring.

Three separate hangs across two boots, one root cause: **the GPU does not
retire timestamps on the client channel, and the channel-restart path
cannot recover because it waits on the same timestamp.**

This is a far better reproducer than 2hh's. A 4096-thread compute kernel
writing `out[i] = i*3+7` involves no compositor, no surface, no display —
and reaches the identical five frames.

### What this does and does not say about 2di

`AMDGFX10PM4Engine::QueryQueueStatus` and `verifyThreadsActive` appeared
in 2hh under the same `waitForTimestamp`; here the submit arrives via
`AMDUserQueue::prepareQueue`. Both are the client ring. 2di
(`gfx_min = gfx_max = 0`, GFX DPM never started) remains the leading
suspect and remains **unmeasured**. Nothing here establishes it. The next
step is to read the ring's rptr/wptr and the GFX clock state at the
moment of the wait, not to assume the clock is why.

Also unexplained, and not to be folded into the above: this boot's
`rbgpu-prog2` reads `W=1 T=0` @21s and `W=1 T=8` @41s, against the 2hh
boot's `W=2/6/11, T=12/34/56`. Two boots is not a table (2fs).

### The instrument earned its keep

`system_profiler SPDisplaysDataType` **blocked** and was killed at 30 s by
the `bounded()` helper added in 2hi, leaving a file that says so in
words. Without that bound the capture would have stalled there and never
reached the Metal probe — the whole result above would have been lost to
a hang in a diagnostic.

## 2hl — the CP is clocked, fetched 689 dwords, and stopped: 2di is not the cause

Boot of 2026-10-18 ~01:47, armed, build `01ADE600`, boot-args
`-rbgpuforceenable wdt=-1 -rbgpuringprobe`. `state.txt` has `rbgpu-diag`
= `d=73 armed=1` live and `rbgpu-boot = 26`; capture in
`logs/20261018-014904-metal/`. No panic; the box froze and stayed
reachable, as 2hj established it would.

**The 2hk probe ran and returned real values, both times.** The in-boot
capture holds the +100 s sample and live NVRAM holds the +160 s one, so
for once we have the pair rather than an overwrite:

```
p=0 (+100s)  cntl=00a00e10 rptr=000002b1 wptr=00000380 wptr_hi=00000000
             cp_stat=90038600 grbm=a0003028
p=1 (+160s)  cntl=00a00e10 rptr=000002b1 wptr=00000380 wptr_hi=00000000
             cp_stat=90038600 grbm=a0003028
```

**Byte for byte identical across sixty seconds.**

### None of the three predicted outcomes

2hk was written expecting one of: rptr frozen at zero with wptr moved;
both advancing; or wptr never moving. What came back is a fourth:
**rptr is frozen at 0x2b1, which is neither zero nor equal to wptr.**

- `rptr = 0x2b1` = 689 dwords. The CP **fetched and consumed 689 dwords
  of this ring** and then stopped.
- `wptr = 0x380` = 896 dwords. **207 dwords sit unconsumed.**
- Both are frozen for a minute, so this is a stall, not slow progress.

### What the status registers say

Decoded against `docs/linux-ref/gc_10_3_0_sh_mask.h`, fetched for this
(see UPSTREAM.md — the commit could not be resolved from this
environment, the bytes are pinned by sha256 instead):

```
GRBM_STATUS = 0xa0003028
    CP_BUSY                = 1
    GUI_ACTIVE             = 1
    DB_CLEAN               = 1
    CB_CLEAN               = 1
    ME0PIPE0_CMDFIFO_AVAIL = 8
    (bit 0x20 is set and the header does not name it)

CP_STAT = 0x90038600
    CP_BUSY            = 1
    PFP_BUSY           = 1
    ME_BUSY            = 1
    MEQ_BUSY           = 1
    ROQ_RING_BUSY      = 1
    ROQ_INDIRECT1_BUSY = 1
    ROQ_CE_RING_BUSY   = 1

CP_RB0_CNTL = 0x00a00e10
    RB_BUFSZ = 16, RB_BLKSZ = 14, MIN_AVAILSZ = 2, MIN_IB_AVAILSZ = 2
```

The CP's **front end** is the busy part: the read-order queues hold data
(`ROQ_RING_BUSY`, `ROQ_INDIRECT1_BUSY`, `ROQ_CE_RING_BUSY`), and the
prefetch parser and micro-engine are both busy (`PFP_BUSY`, `ME_BUSY`,
`MEQ_BUSY`). Meanwhile `DB_CLEAN` and `CB_CLEAN` are both set: the depth
and colour blocks have nothing in flight. **The work never reached the
shader cores.** The CP is holding fetched packets it cannot retire.

### 2di is not the cause — stated carefully

`gfx_min = gfx_max = 0` has been the leading suspect since 2hh, on the
reasoning that a GFX ring with no clock range would accept packets and
never retire them. **That reasoning is now contradicted by measurement.**

A CP with no clock does not advance `rptr` to 0x2b1, does not raise
`GUI_ACTIVE`, and does not report six separate front-end blocks busy.
The GFX block is clocked and executing. Whatever `gfx_min = gfx_max = 0`
means, it is not "the GFX engine is switched off".

What this does **not** say: it does not say the clock is correct, that
DPM is healthy, or that 2di is harmless. It says 2di is not the
mechanism of this hang. The debt stands; its promotion to prime suspect
(CLAUDE.md, after 2hh) is **withdrawn**.

This is the measurement doing its job. The hypothesis was explicitly
labelled, never built on, and is now dropped on evidence rather than
carried forward because it was plausible.

### What the frontier now is

The CP consumed 689 dwords of the GFX ring and stalled with 207
remaining, front-end queues full, nothing downstream active. That is the
shape of a command processor waiting on something that never arrives —
a `WAIT_REG_MEM` whose condition never becomes true, a fence or
semaphore, or a fetch that never returns because its address does not
translate.

The last of those is worth naming because this project has history with
it: GPUVM. The 2em `PT_BASE` fix made translation work for the ranges
tested, and the KIQ executes PM4 and writes fences — but the KIQ is a
different ring with a different VMID, and nothing has established that
the **client** ring's addresses translate.

### What the Metal dispatch did and did not do

The probe hung at commit again, as in 2hj. Its timing matters: the
`+100 s` sample fell **before** the dispatch was submitted and the
`+160 s` one **after**, and the two are identical. So the Metal
submission **did not touch RB0 at all** — its compute work goes to an
MQD-based user queue (`AMDUserQueue::prepareQueue` in 2hj's stack), not
to the kernel GFX ring.

**RB0 was already stalled before any Metal client ran.** This is the
driver's own work, wedged early, and it is what the event-machine thread
has been polling for all along. One correction to how 2hj was framed:
"the ring is full" from `waitForRingBufferSpace` is a *consequence* two
levels removed, not the client's own ring filling up.

`rbgpu-prog2` agrees: `W=1` throughout — **one** `waitForStamp` call,
with timeouts climbing `T=0 -> 6 -> 34` across the three samples. One
wait, never satisfied.

### Next measurement, and what it must not assume

Read, at the stall:

1. **The ring contents around `rptr`** — the actual dwords at 0x2b1.
   The PM4 opcode there names the packet the CP stopped on, and that is
   a single decisive fact rather than another inference.
2. **`GCVM_L2_PROTECTION_FAULT_STATUS` and the companion address
   registers.** A VM fault stalls a CP in exactly this shape, and these
   registers say outright whether one happened, with the faulting
   address and VMID.
3. **`CP_ME_CNTL`** to confirm nothing is halted, and `GRBM_STATUS2` for
   the blocks `GRBM_STATUS` does not cover.

(2) is the cheapest and the highest-value: it is a handful of register
reads through the accessor already proven to work here, and a nonzero
fault status would name the mechanism outright. It is a question that
can come back "no fault", which is what makes it worth asking.

### The probe itself

No stall. The six reads went through the accessor cleanly at both
samples, which also answers the 2fe worry for this access pattern: a
fixed six-register read at a single point is not what stalled in 2fe.
That is evidence about the instrument, not a licence to reinstate
`-rbgpuregdump`, which swept.

One defect found: `rbgpu-ring` is a single NVRAM variable, so `p=1`
overwrote `p=0`. The pair survived only because `metal_check.sh`
happened to capture `state.txt` between the two samples. That was luck.
The note must accumulate both samples in one string, the way
`rbgpu-prog2` does — owed.

## 2hn — a GPUVM page-table walk is failing, and the CP is halted

Boot of 2026-10-18 ~02:19, armed, build `888D121C` (matches `build/out`),
boot-args `-rbgpuforceenable wdt=-1 -rbgpuringprobe`. `state.txt` has
`rbgpu-diag` = `d=73 armed=1` live and `rbgpu-boot = 27`; capture in
`logs/20261018-022144-metal/`. The in-boot note:

```
rbgpu-ring = b=27 p=0 vmf=40953 vmalo=0 vmahi=0 vmfc=1ffc
             r=2b1 w=380 cps=90038600 grbm=a0003028
             g2=54200008 me=15000000 rbb=ffbfde00 rbbhi=0
```

(Live NVRAM still reads the `b=26` note — the `p=1` commit was lost across
the power cycle while the `p=0` one survived. Exactly 2gq. The in-boot
capture is the evidence; the live variable is not.)

### The fault register is not zero

Decoded against `docs/linux-ref/gc_10_3_0_sh_mask.h`:

```
GCVM_L2_PROTECTION_FAULT_STATUS = 0x00040953
    MORE_FAULTS       = 1
    WALKER_ERROR      = 1
    PERMISSION_FAULTS = 5
    MAPPING_ERROR     = 1
    CID               = 4
    RW                = 1       (a WRITE)
```

**`WALKER_ERROR` + `MAPPING_ERROR`: the page-table walker could not
resolve an address.** On a write, from client 4, and `MORE_FAULTS` says
it happened more than once.

`GCVM_L2_PROTECTION_FAULT_CNTL = 0x1ffc` — every fault class is enabled,
so this is not a chip configured to ignore faults; it is a chip reporting
one.

### And the CP is halted

```
CP_ME_CNTL = 0x15000000
    CE_HALT = 1   PFP_HALT = 1   ME_HALT = 1
```

All three command-processor micro-engines are **halted**. That is why
`rptr` is frozen at 0x2b1 with 207 dwords pending, and it reconciles the
2hl reading: the front-end queues still hold fetched data
(`ROQ_*_BUSY`, `PFP_BUSY`, `ME_BUSY` in `CP_STAT`) because the engines
were stopped mid-stream rather than drained.

**The halt is almost certainly the driver's doing, not the chip's.**
`resetHardwareAndReplay` is exactly where the stuck thread sits (2hj),
and halting PFP/ME/CE is what a channel reset does first. So the
ordering that fits every reading so far is:

1. a GPUVM write faults — the walker cannot map the address
2. the work never completes, so the timestamp is never written
3. `waitForTimestamp` times out (`rbgpu-prog2`: `T=0 -> 8 -> 36`)
4. the event machine calls `resetHardwareAndReplay`, which **halts the CP**
5. the replay waits for the same timestamp, which still never arrives
6. everything queues behind it — WindowServer on the mutex, Metal on the
   restart

That is a chain, and steps 4-6 are measured. Step 1 is measured as *a
fault having happened*; that it is **this** fault causing **this** hang
is inference, and the next section says how to close it.

```
GRBM_STATUS2 = 0x54200008    CPF_BUSY, CPG_BUSY, RLC_BUSY, SDMA_BUSY
CP_RB0_BASE  = 0xffbfde00 -> ring VA 0xFF_BFDE_0000
```

The ring base confirms the GFX ring lives in Apple's GPUVM space
(`0xFF_BF..`, CLAUDE.md), reached through the same translation that is
faulting.

### What this does NOT establish, stated plainly

**The fault status register is latched and carries no timestamp.** It
says at least one fault occurred since the last clear; it does not say
when, or that it was caused by the work the CP died on. `MORE_FAULTS`
makes it certain there was more than one, and nothing distinguishes an
early bring-up fault from a fault at the frontier.

This is precisely the recurring error this project keeps paying for:
reading a value without establishing what makes it meaningful. The
reading is real and it is the best lead this project has had on this
hang, but "a fault is latched" is not yet "the fault is the cause".

**`vmalo = 0` and `vmahi = 0`.** Either the faulting page genuinely is
VA 0 — consistent with `MAPPING_ERROR`, and the shape of an
uninitialised pointer in a descriptor — or the address did not latch
into those registers. Both are worth resolving and neither is assumed
here.

### The next measurement, and it is read-only

Time-bound the fault **without writing anything**: read
`GCVM_L2_PROTECTION_FAULT_STATUS` at an **early** point, before any
client work — the existing `+15 s` prog sample is already there — and
again at `+100 s` and `+160 s`.

- clean early, faulted later -> the fault happens in the window where
  the hang develops, and the chain above is established rather than
  inferred
- already faulted at `+15 s` -> there is an earlier GPUVM problem this
  project has been walking past since bring-up, and that is a different
  and larger finding
- either way the question is answered by three reads of one register

Clearing the status to re-arm it would be stronger, but that is a
register **write**, and this probe has been read-only by design (2cy).
The early-read version answers the same question and costs nothing, so
it goes first.

Also owed, unchanged from 2hl: `vmalo`/`vmahi` reading zero needs
explaining before the address is used for anything.

### The Metal probe

Hung at commit again, identically to 2hj and 2hl. Three boots, same
place. `rbgpu-prog2`: `W=1` throughout — one `waitForStamp` call,
timeouts climbing `T=0 -> 8 -> 36`.

## 2hp — the faulting client is CPF, the faults are ongoing, and the GPU recovers while the software does not

Boot of 2026-10-18 ~02:30, armed, build `8EB5C8DF` (matches `build/out`),
`rbgpu-diag` = `d=73 armed=1` live, `rbgpu-boot = 28`. Capture in
`logs/20261018-023219-metal/`; the third sample came from live NVRAM,
which this time carried all three.

```
p=0 (+12s)   vmf=3009bb vmalo=400300 vmahi=0 vmfc=1ffc
             r=b1  w=280 cps=94078600 grbm=a0003028 g2=54204008 me=15000000
             rbb=ffbfde00 rbbhi=0
p=1 (+100s)  vmf=40953  vmalo=0      vmahi=0
             r=2b1 w=380 cps=90038600 grbm=a0003028 g2=54204008 me=15000000
p=2 (+160s)  vmf=40953  vmalo=0      vmahi=0
             r=180 w=180 cps=0       grbm=a0003028 g2=14200008 me=0
```

### 1. The faults are ongoing, not one latched event — 2hn's caveat retires

2hn could not say *when* the fault happened, because the status register
latches. **The two reads return different values**, which settles it:
something clears the register between them.

Linux says what. `gmc_v10_0_process_interrupt` reads
`vm_l2_pro_fault_status` and then does
`WREG32_P(hub->vm_l2_pro_fault_cntl, 1, ~1)` — the driver clears and
re-arms after every fault it handles. So Apple's driver is **taking VM
fault interrupts and clearing them**, and each of our samples caught a
*different, more recent* fault.

The faults are **recurring throughout the boot**, not a single historical
event. That is stronger than 2hn's reading and it is measured, not
inferred.

It also explains `vmalo` going to 0: `gfxhub_v2_1_get_invalidate_req`
sets `CLEAR_PROTECTION_FAULT_STATUS_ADDR` in the invalidate request, so a
TLB invalidation clears the address register. The `0x400300` captured at
+12 s is a real latched value; the later zeros mean an invalidation ran
in between, not that the address is zero.

**What is still not established:** how to turn `vmalo` into an address.
Linux takes the faulting address from the *interrupt vector*
(`entry->src_data[0] << 12`), never from this register. `0x400300 << 12`
= `0x4_0030_0000` is the obvious reading and it is **not** sourced. Not
used for anything here.

### 2. The faulting client is the CP's fetcher

`CID = 4` in both samples. `gfxhub_client_ids[]` in `gfxhub_v2_1.c`:

```
[0] CB/DB  [1] Reserved  [2] GE1  [3] GE2  [4] CPF  [5] CPC  [6] CPG  [7] RLC ...
```

**CID 4 = CPF**, the command processor's *fetcher* — the block that
fetches command buffers and indirect buffers. The client that cannot
translate is the one reading the command stream.

And `GRBM_STATUS2` has **`CPF_BUSY` set in all three samples**, including
`p=2` where everything else has gone idle. The fetcher is the one thing
that never comes back.

### 3. Two different faults, not one repeated

```
p=0  WALKER_ERROR=5  PERMISSION_FAULTS=0xb  MAPPING_ERROR=1  CID=4  VMID=3  (a READ)
p=1  WALKER_ERROR=1  PERMISSION_FAULTS=5    MAPPING_ERROR=1  CID=4  VMID=0  (RW=1, a WRITE)
```

Different VMIDs, different directions, different error codes. **VMID 3**
at +12 s is a client context, not the kernel's VMID 0. So the problem is
not confined to one context or one access.

### 4. The early sample is already faulted — and that is the answer to 2ho

2ho asked: clean at +12 s and faulted later, or faulted already? **Faulted
already**, and with the CP already halted (`me=15000000`) twelve seconds
in.

By 2ho's own pre-written table that is the second branch: *an earlier
GPUVM problem that bring-up has been walking past.* The frontier is
therefore **not** something that begins when a Metal client submits. It
is present before any client, and the client hang is downstream of it.

This also revises 2hl. The ring is not simply frozen: between +12 s and
+100 s `rptr` advanced `0xb1 -> 0x2b1` and `wptr` `0x280 -> 0x380`. Work
*was* progressing, in bursts, while faulting. 2hl's two identical samples
caught a window where it happened to be stopped.

### 5. The GPU recovers; the software does not

`p=2` is the most surprising reading of the night:

```
me = 0           CE_HALT, PFP_HALT, ME_HALT all CLEARED - the CP is running again
cps = 0          CP_STAT entirely idle
r = w = 0x180    rptr == wptr: the ring is EMPTY and the pointers were REWOUND
g2: CPG_BUSY and RLC_RQ_PENDING gone (CPF_BUSY remains)
```

Between +100 s and +160 s the reset **completed**. The CP was un-halted,
the ring reset to an empty state, and the pending work discarded rather
than replayed — `rptr == wptr` at an offset lower than either previous
sample.

**And the machine was still hung.** Metal still blocked in
`waitUntilCompleted`, WindowServer still blocked, the event-machine
thread still polling. So the shape of the bug is now:

> the hardware faults, the driver resets it, the reset succeeds and
> throws the work away — and the timestamp that the waiting thread needs
> was part of the work that was thrown away, so it never arrives and
> nothing ever wakes.

That is a different problem from "the GPU is wedged". The GPU is fine at
+160 s. The software is waiting for a completion that can no longer
happen.

### 6. One reading to distrust

`GRBM_STATUS` reads **`a0003028` in all three samples, and in both
samples of the previous boot.** Five identical reads across two boots,
including at `p=2` where `CP_STAT` is 0 and the CP is un-halted — yet
`GRBM_STATUS` still claims `CP_BUSY` and `GUI_ACTIVE`.

That constancy is suspicious. Either this register is not being read
live through the accessor, or it means something other than the obvious.
**Nothing in this milestone rests on `GRBM_STATUS`**, and 2hl's use of it
should be treated as weaker than it looked. Resolving it is owed.

### Next

The question has moved. It is no longer "why does the GPU not retire the
timestamp" but **"why does CPF fail to translate, from the very start of
the boot"**.

Cheapest next reads, all through the proven accessor and still
read-only: the GCVM context registers for the faulting contexts —
`GCVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_LO32/HI32`,
`GCVM_CONTEXT0_PAGE_TABLE_START/END_ADDR`, and the VMID 3 equivalents —
compared against what 2em's `PT_BASE` fix actually wrote. 2em made
translation work for what it tested; the ring base at
`0xFF_BFDE_0000` and a CPF fetch are not necessarily inside that.

## 2hr — the client contexts' page-directory base registers have no VALID bit

Boot of 2026-10-18 ~02:47, armed, build `2066830F` (matches `build/out`),
`rbgpu-diag` = `d=73 armed=1` live, `rbgpu-boot = 29`. Capture in
`logs/20261018-024701-metal/`.

```
rbgpu-vm = b=29
  p=0  c0:cn=1555481,b=78fdfa001,s=ffbfa00,e=ffffe00
       c1:cn=3b,     b=fb00b000, s=400000, e=23fffff
       c3:cn=3b,     b=fb015000, s=400000, e=23fffff
  p=1  c0:cn=1555481,b=78fdfa001  c1:cn=3b,b=fb00b000  c3:cn=3b,b=fb015000
```

Unchanged between +12 s and +100 s, so these are not a transient
mid-programming state.

### The windows are fine, and the contexts are enabled

`GCVM_CONTEXT*_PAGE_TABLE_START/END_ADDR` hold a `LOGICAL_PAGE_NUMBER`
(the sh_mask header names the field), so the windows are:

```
c0   VA 0xFF_BFA0_0000 .. 0xFF_FFE0_0000
c1   VA 0x4_0000_0000  .. 0x23_FFFF_F000
c3   VA 0x4_0000_0000  .. 0x23_FFFF_F000
```

- The **GFX ring at `0xFF_BFDE_0000`** (2hn) is inside c0's window. ✓
- The **+12 s faulting page `0x400300`** = VA `0x4_0030_0000` is inside
  c3's window, 3 MB past its start. ✓

So two hypotheses from 2hq's pre-written list are **dead**: context 3 was
not left unprogrammed, and this is not a range fault. `ENABLE_CONTEXT` is
set in all three.

### The anomaly: c1 and c3 have no VALID bit

```
c0  PDE = 0x7_8FDF_A001      bit 0 SET      <- 2em's patched value
c1  PDE = 0x0_FB00B000       bit 0 CLEAR
c3  PDE = 0x0_FB015000       bit 0 CLEAR    <- the context that faulted
```

The register field is literally named `PAGE_DIRECTORY_ENTRY_LO32`, so it
carries a PDE, flags and all. Two independent sources say bit 0 must be
set:

- **Linux.** `amdgpu_gmc_pd_addr()` (amdgpu_gmc.c) for Vega10+ does
  `flags = AMDGPU_PTE_VALID; ...; pd_addr |= flags`, and
  `AMDGPU_PTE_VALID` is `(1ULL << 0)` (amdgpu_vm.h). Every page-directory
  base Linux writes to this register has bit 0 set, at every depth.
- **Apple's own code**, which matters more here.
  `AMDRadeonX6000_AMDGFX10VMM::getPDEValue` (0xbe9b9de) ends:

```
    movabsq $0xffffffffffc0, %rdi
    andq    %rdi, %rdx            ; address, 64-byte aligned
    ...
    shlq    $0x38, %rdi           ; bit 56 when level > depth
    orq     %rdx, %rdi
    leaq    (%rcx,%rdi), %rax     ; + the block-size field at bit 59
    incq    %rax                  ; <<<< +1: the VALID bit
    retq
```

**Every PDE Apple builds has bit 0 set.** A base register ending in
`000` is therefore anomalous by Apple's own convention, not only by
Linux's.

`AMDGCHub_10_3_0::fillVMRegisters` (0xbed0cae) builds the offset table
for all sixteen contexts with exactly the strides the header gives
(`0x1667 + 2N`, `0x1687 + 2N`, `0x16a7 + 2N`, `0x15fc + N`), so Apple
addresses context 3 and the offsets this probe used are Apple's own.

### Also different: the table depth

```
c0    PAGE_TABLE_DEPTH = 0            flat - matches 2em's flat PTE array
c1/c3 PAGE_TABLE_DEPTH = 1, BLOCK_SIZE = 7
```

So the client contexts expect a two-level walk. `WALKER_ERROR` is a
walker complaint, and a depth-1 walk beginning at a directory entry the
hardware does not consider valid is a coherent way to produce one.

### What is NOT established

This is a lead, not a cause, and the alternatives have not been excluded:

1. **The hardware may ignore bit 0 in this register.** Linux and Apple
   both set it, which is good evidence it is load-bearing, but neither
   proves the walker checks it.
2. **These values may not come from `getPDEValue` at all.** Something
   wrote two distinct values (`0xfb00b000`, `0xfb015000`), but whether
   that was Apple's VM code, firmware, or a leftover is not established.
   The write path was not found: `programAndInvalidateVM` (0xbe9b7a2) is
   the invalidate side, and the base-register write was not located in
   this pass.
3. **The addresses themselves may be wrong**, independently of the flag.
   `0xFB00B000` and `0xFB015000` are ~4.2 GB, plausible system DRAM and
   *not* shaped like the stripped carve-out offset 2em corrected for
   context 0 (that was `0x0FDF_A000`, inside a 512 MB carve-out). So this
   is probably **not** a second instance of the 2em bug — but "probably
   not" is not "not".

Setting the bit and seeing whether the fault clears is the obvious
experiment, and 2em is the precedent for a single targeted write behind
its own boot-arg. It is **not** the next step, because contexts 1-15 are
client contexts that the driver reprograms per VM bind, so a one-shot
write from a timer would likely be overwritten before it is tested.

### The better next step: Apple already decodes these faults

`AMDRadeonX6000_AMDGFX10VMM` has `getVMFaultInfo`,
`getVmFaultMemoryClientName`, `writeVmFaultProtectionString` and
`clearVMFault`, and the kext carries this format string:

```
%s: %s VM_FAULT %u %s VA: %#llx, pages: %u, op: %s, vmid: %u, pasid: %u,
memClient: %#x, Task %d %s, VA status: %s, Closest good VA: %#llx
```

Apple records the faulting **VA** directly — ending the `vmalo`
interpretation debt outright — plus the op, the vmid, the pasid, the
memory client and a **closest good VA**, which would say immediately
whether the fault is just past the end of a real mapping.

It is not reaching us: no `VM_FAULT` text in the captured `ioreg.txt` of
any armed boot, and nothing in the unified log for the boot window. So
it is gated behind a log level or lands in a buffer we are not reading.

Hooking `getVMFaultInfo` and logging the `VM_FAULT_INFO` it fills gives
Apple's own per-fault record, at the moment of each fault, instead of our
sampled snapshots. Rule 9 (2gb) applies: disassemble the first 16 bytes
and refuse if a rel8/rel32 branch starts within the first 5, before
routing anything.

## 2ht — Apple's own fault records: a CPF read at 0x4_0030_0000, a CPF write to VA 0, and two phantoms

Boot of 2026-10-18 ~03:01, armed, build `A187A307` (matches `build/out`),
`rbgpu-diag` = `d=73 armed=1` live, `rbgpu-boot = 30`. Capture in
`logs/20261018-030337-metal/`. The 2hs hooks took and Apple's own
`VM_FAULT_INFO` records came out:

```
#1  hub 0  VA 0x4_0030_0000    vmid 3   READ   cid 4    status 0x3009bb
#2  hub 1  VA 0xFFFFFFFFF000   vmid 15  WRITE  cid 511  status 0xffffffff
#3  hub 0  VA 0                vmid 0   WRITE  cid 4    status 0x40953
#4  hub 1  VA 0xFFFFFFFFF000   vmid 15  WRITE  cid 511  status 0xffffffff
```

### #1 confirms the static derivation exactly

2hs computed VA `0x4_0030_0000` from `(HI32 & 0xf) << 44 | (LO32 << 12)`
by reading Apple's disassembly, before this boot ran. Apple's own code
produced the identical value. VMID 3, a **read**, client **CPF** — the
command processor's fetcher — three megabytes into context 3's window,
the context whose page-directory base has no VALID bit (2hr).

The `vmalo` interpretation debt from 2hn/2hp is **closed**: the shift is
Apple's own, and the number agrees.

### #2 and #4 are not faults. MMHUB reads back all-ones

`status = 0xffffffff` is the classic dead-register read. Every decoded
field is simply the all-ones bits: `vmid 15` (0xf), `cid 511` (0x1ff),
`VA 0xFFFFFFFFF000` (`0xfffffff << 12 | 0xf << 44`). Nothing faulted.

**Apple cannot tell.** `getVMFaultInfo` fills its struct whenever
`(addr != 0 || status != 0)`, and all-ones satisfies that. So on this
chip Apple reports a **phantom MMHUB fault on every poll of hub 1**, with
garbage in every field, and feeds it to the same fault-handling path as
a real one.

That is a new defect, independent of the hang, and it means any reasoning
that treats Apple's hub-1 fault reports as real is reasoning about
nothing. It also raises a question not yet answered: whether this phantom
stream is itself provoking resets.

Why hub 1's register reads all-ones is not established here. The GFXHUB
(hub 0) registers read fine through the same accessor.

### #3 is the one that matters: a CPF WRITE to VA 0

`hub 0, VA 0, vmid 0, WRITE, cid 4 (CPF), status 0x40953` — the same
status 2hn and 2hp caught at +100 s, now with Apple's own decode
attached.

A **write** by the command processor to **virtual address zero** is the
shape of an uninitialised pointer in a descriptor. And the most
interesting candidate pointer in this story is the **end-of-pipe
timestamp writeback address**: if the CP is told to write its completion
timestamp to VA 0, the write faults, the timestamp never lands, and
`waitForTimestamp` polls forever — which is precisely the hang of 2hj,
2hl and 2hp.

**That is a hypothesis and is labelled as one.** It is not established
that CPF's write is the timestamp write, and it is not established that
VA 0 is real rather than an artifact:

> `gfxhub_v2_1_get_invalidate_req` sets
> `CLEAR_PROTECTION_FAULT_STATUS_ADDR`, so a TLB invalidation between the
> fault and the read zeroes the address register (2hp). Apple reads the
> same registers this probe does, so Apple's `VA 0` is equally
> susceptible. "The address was zero" and "the address was cleared before
> anyone looked" are not distinguished by this reading.

The discriminator is cheap: capture **more** faults and see whether a
`status 0x40953` fault ever carries a nonzero VA. If none ever does
across many faults, VA 0 is real. The current cap of four, chosen to
avoid the 2fo wedge, is what prevents that today.

### The rest of the boot is unchanged

```
ring p=0  vmf=3009bb vmalo=400300 r=b1  w=280 cps=90078600 me=15000000
ring p=1  vmf=40953  vmalo=0      r=2b1 w=380 cps=90038600 me=15000000
```

Identical to 2hp and 2hr boot-for-boot, which is itself worth having:
**four consecutive armed boots now reproduce the same two faults in the
same order at the same addresses.** This is deterministic, not a race.
Metal hung at commit for the fourth boot running.

### Instrument defect found

The `vmfault_hooked`, `vmfaults_seen`, `vmfault_calls` and
`vmfault_reports` diag keys **did not survive** — `rbgpu-diag` is length
capped and truncates at `mapq_ca`, well before them. So this boot cannot
say how many times `getVMFaultInfo` was called, how many faults there
were in total, or whether `fillFaultAccelInfo` ran at all.

That is exactly the ambiguity-of-absence this project keeps paying for
(2eh, 2ek, 2fb): the counters are missing, and missing looks the same as
zero. The counts belong **in the `rbgpu-fault` note itself**, not in the
shared diag table. Owed, and it blocks the "is Apple's reporting path
even running" question 2hs was partly built to answer.

## 2hv — Apple's fault reporting runs; VA 0 held across three faults, which is not enough

Boot of 2026-10-18 ~14:26, armed, build `A77DD014` (matches `build/out`),
`rbgpu-diag` = `d=73 armed=1` live, `rbgpu-boot = 31`. Capture in
`logs/20261018-142827-metal/`. Fifth consecutive armed boot; ring, VM
contexts and Metal all identical to 2hp/2hr/2ht.

```
rbgpu-fault = b=31 hk=1 c=8 f=8 r=4 o=0
  s3009bb   h0  n=1  va=400300000     mx=400300000     v3  R  c4
  sffffffff h1  n=4  va=fffffffff000  mx=fffffffff000  v15 W  c511
  s40953    h0  n=3  va=0             mx=0             v0  W  c4
```

### What this settles

**The instrument is honest now.** `hk=1` proves the hooks took, and the
counters survived in the note instead of being truncated out of
`rbgpu-diag` as in 2ht. A boot with no faults and a boot where the hooks
failed can no longer look the same.

**Apple's fault reporting path RUNS.** `r=4` —
`AMDHWVMM::fillFaultAccelInfo` was called four times. Apple is not merely
latching faults in hardware: it is assembling four complete fault
reports, with VA, op, vmid, pasid, memClient and closest-good-VA, and
handing each to `kprintf`, where nothing can read them (2hs). Apple's own
driver considers this a real, reportable GPU fault condition, every boot.

**Every single poll found a fault.** `c=8 f=8` — eight calls to
`getVMFaultInfo`, eight faults. Apple never once looked and found the
chip clean.

**The hub-1 phantoms dominate.** Four of the eight are the all-ones
dead-register reads 2ht identified. Half of what Apple's fault path
processes on this chip is noise from a register that does not decode.

### What this does NOT settle, and I said beforehand what would

2hu was built to answer one question: does a `status 0x40953` fault ever
carry a nonzero VA? The answer here is **no — across three faults.**

**Three is not enough, and 2hu's own text set the bar at "hundreds".**
Writing `mx=0` down as confirmation would be the recurring error again:
a reading that only means something at a sample size this boot did not
reach. The honest statement is that `s40953` was seen three times, every
time with VA 0, and that this is consistent with both live hypotheses —
a genuine null target, and an address register cleared by
`CLEAR_PROTECTION_FAULT_STATUS_ADDR` before anyone read it (2hp).

Worse, the sample size cannot be raised this way. **Apple polls
`getVMFaultInfo` only eight times in two and a half minutes**, so more
waiting buys almost nothing, and the five-boot record (`o=0`, same
signatures, same order) says the behaviour is deterministic rather than
sampled thinly from a varied population.

So the end-of-pipe-timestamp hypothesis from 2ht is **neither supported
nor refuted** by this boot. It stands exactly where it did.

### The discriminator that does not depend on the fault registers at all

Stop asking the hardware what address faulted and ask the **command
stream what address the CP was told to write**. The EOP timestamp target
is a field in the PM4 `RELEASE_MEM` packet sitting in the ring, and we
now know everything needed to find it:

- ring VA `0xFF_BFDE_0000` (`CP_RB0_BASE << 8`, 2hn)
- `rptr` 0xb1 early, 0x2b1 at the hang; `wptr` 0x280 / 0x380 (2hl, 2hp)
- context 0's window covers it and its PDE is valid (2hr)

Decoding the dwords around `rptr` names the packet the CP stopped on
**and** shows its writeback address directly. If that field is zero, the
hypothesis is established outright and the fault-register ambiguity
never needs resolving. If it is a sane address, the hypothesis dies and
the VA 0 really was a cleared register.

That is the mapping step deliberately deferred in 2hm ("a register read
may settle it first, and bundling a new memory mapping into the same
boot would mean two new things at once"). The register reads have now
been run to their limit, and they did not settle it.

The risk is real and new: this needs the plugin to map GPU-visible
memory, which it has never done. The ring is at a GPUVM address, so the
CPU-side route is the carve-out alias — the same address-space
translation 2ei/2em worked out — and getting it wrong means reading the
wrong page and believing it. Whatever it maps must be checked against
something known before any dword of it is interpreted: a PM4 packet
stream has a recognisable `0xC0` type-3 header pattern, and a buffer
that does not look like PM4 is a failed mapping, not a finding.

## 2hx — the ring maps, and the timestamp-target hypothesis is REFUTED

Boot of 2026-10-18 ~14:44, armed, build `1035EE1A` (matches `build/out`),
`rbgpu-diag` = `d=73 armed=1` live, `rbgpu-boot = 32`, boot-args plus
`-rbgpuringdump`. Capture in `logs/20261018-144648-metal/`.

```
rbgpu-pm4 = b=32 pa=6c8911000 t3=799 r=2b1 w=380
            | rm@1e  a=ffbfdc0000 s=3
            | rm@8e  a=ffbfdc0000 s=4
            | rm@10e a=ffbfdc0000 s=5
```

### The mapping is real

`t3=799` — **799 PM4 type-3 headers in 1024 dwords.** The gate 2hw was
built around did not just pass, it passed overwhelmingly; nothing but a
command ring looks like that. So the route works: ring VA -> CONTEXT0's
flat PTE table -> PTE -> physical page `0x6_C891_1000` -> one page
mapped read-only. The bus-vs-physical worry is answered for this path.

**This is the first time this project has read GPU-visible memory.** It
is worth saying that the thing which made it safe was the refusal
branch, not the success: had the PTE held a bus address that was not the
CPU physical address, `t3` would have come back near zero and the dump
would have said "failed mapping" instead of inventing a finding.

### The hypothesis is dead

Three `RELEASE_MEM` packets, at ring dwords `0x1e`, `0x8e` and `0x10e`,
every one with

```
dw3/dw4  ADDR = 0xFF_BFDC_0000
dw5/dw6  seq  = 3, 4, 5
```

**The end-of-pipe timestamp writeback address is `0xFF_BFDC_0000`, not
zero.** 2ht's hypothesis — "the CP is told to write its timestamp to VA
0, so the write faults and the timestamp never lands" — is **refuted**.
It was labelled a hypothesis in 2ht, 2hu and 2hv and never built on, and
it is now dropped on direct evidence from the command stream.

By elimination this also resolves 2hp/2hv's ambiguity the other way: the
`VA 0` in `status 0x40953` is the **cleared-register artifact**, not a
null target. `CLEAR_PROTECTION_FAULT_STATUS_ADDR` had wiped the address
before anyone read it, exactly as the alternative predicted. (Strictly
this is inference from the refutation, not an independent measurement of
the clear — but no reading now supports a genuine null.)

`0xFF_BFDC_0000` sits inside CONTEXT0's window
(`0xFF_BFA0_0000 .. 0xFF_FFE0_0000`, 2hr) and 128 KB below the ring
itself at `0xFF_BFDE_0000`. Its page index into the flat table is
`0xFFBFDC0 - 0xFFBFA00 = 0x3C0` = **960**, against the ring's 992.

### What the CP actually did

All three fence packets sit **before** `rptr = 0x2b1` (689), so the CP
fetched past all of them. It was handed three completion fences for
sequence numbers 3, 4 and 5, aimed at a sane in-window address — and the
driver still waits forever.

That splits cleanly into two remaining possibilities, and they are
distinguished by reading one page:

1. **the writes faulted** — `0xFF_BFDC_0000`'s PTE is bad, so the
   timestamp never reached memory. This is now the natural reading of
   the CPF *write* fault under VMID 0 (2ht #3), and it would make the
   fault address `0xFF_BFDC_0000` rather than 0.
2. **the writes landed and the driver reads elsewhere** — the memory at
   `0xFF_BFDC_0000` contains 5, and `waitForTimestamp` is polling a
   different address.

### Next, and it reuses everything already built

Read PTE[960] for the fence buffer (the 2el machinery, already in the
plugin), and **map `0xFF_BFDC_0000` and dump its first dwords**.

- contents hold `5` -> the fences landed; the bug is on the driver's
  read side, and this becomes a question about where `waitForTimestamp`
  looks rather than about the GPU at all
- contents hold `0` or junk, PTE[960] invalid -> the write faulted, and
  the VM fault finally has its real address

One page, the same proven route, the same sanity discipline — though the
gate has to change, because a fence buffer is not full of PM4 headers.
What makes a fence buffer recognisable is far weaker evidence than
`t3=799`, so the dump must report the raw dwords and the PTE and let the
reading be argued from those, not assert a verdict in-kernel.

### Two incidental findings

**The RBGPULog had rotated past the dump.** The raw dwords around `rptr`
were logged and are gone: the buffer held roughly **994,000 entries of
`SpecialAMDKey request 0x14`**, one every ~7 ms, which flushed
everything else out. 2es said the published log ends at the last refresh
and cannot locate a hang; this is the same defect costing a
deliberately-captured measurement. Anything that must survive goes to
NVRAM, which is why the packet addresses did survive.

**The driver polls request 0x14 about 994,000 times.** Not investigated
here and not obviously part of the hang, but a driver issuing a million
of anything while wedged is worth a note.

## 2hz — the fence writes LAND. The GPU completes six, then stops

Boot of 2026-10-18 ~14:55, armed, build `EB823959` (matches `build/out`),
`rbgpu-diag` = `d=73 armed=1` live, `rbgpu-boot = 33`. Capture in
`logs/20261018-145747-metal/`.

```
rbgpu-pm4   = pa=6c7c05000 t3=799 r=2b1 w=380
              | rm@1e a=ffbfdc0000 s=3 | rm@8e a=ffbfdc0000 s=4
              | rm@10e a=ffbfdc0000 s=5
rbgpu-fence = fv=ffbfdc0000 idx=3c0 pte=30006c835d077 v=1 s=1
              pa=6c835d000 rpa=6c7c05000 seq=8 o=0
              d: 6 0 280 0 380 0 2 1 0 0 0 0   f=0 nz=1
```

### The fence page is mapped, writeable, and has real data in it

```
PTE[960] = 0x0003_0006_C835_D077
    VALID=1  SYSTEM=1  SNOOPED=1  EXECUTABLE=1  READABLE=1  WRITEABLE=1
    phys = 0x6_C835_D000
```

Valid **and writeable**, exactly what a fence target needs. The ring page
this boot is `0x6_C7C0_5000` from the same table, and that one is
independently corroborated by `t3=799`. So possibility 1 of 2hx — "the
writes faulted because PTE[960] is bad" — is **dead**.

Read as 64-bit values, the page holds:

```
+0x00 = 0x6
+0x08 = 0x280
+0x10 = 0x380
+0x18 = 0x1_0000_0002
```

**`+0x00` is 6.** The RELEASE_MEM packets carry sequence numbers 3, 4,
5 … up to **8** (`seq=8`; the note caps the listing at three packets, so
there are at least six in the page). The fence buffer contains **6**.

**The GPU's timestamp writes LAND.** The address is right, the page is
mapped writeable, and the hardware wrote a real sequence number into it.
The entire fence mechanism works.

### A third outcome, which was not in the table

2hx wrote down two possibilities: the writes faulted, or the writes
landed and `waitForTimestamp` reads the wrong place. **Neither is what
happened.**

The writes land — *up to 6* — and then stop, while 7 and 8 were
submitted and never completed. So this is not a broken fence path and
not a driver reading the wrong address. **The GPU executes the command
stream, signals six fences correctly, and then stops partway.**

That is exactly 2hl's measurement seen from the other side: `rptr` frozen
at 0x2b1 with 207 dwords pending. Now we know the CP got *through* six
fence packets before it stopped, and that everything it did up to that
point worked.

`+0x08 = 0x280` and `+0x10 = 0x380` are worth noting without being
over-read: `0x380` is exactly this boot's live `wptr`, and `0x280` was
the `wptr` at the +12 s sample. The page evidently also holds ring
pointer shadows. Not interpreted further here — the fence value is the
finding and the rest is context.

### What this does to the suspect list

The fence machinery is exonerated. What remains is: **what stops the CP
mid-stream after fence 6?**

And there is exactly one unexplained fault left pointing at it — 2ht's
**#1: a CPF READ at VA `0x4_0030_0000` under VMID 3**. The other GFXHUB
fault (`status 0x40953`, a VMID 0 write) now looks like a plausible
consequence of the reset path rather than a cause, and the hub-1 pair
are the all-ones phantoms (2ht).

**That puts context 3 back at the centre, on much firmer ground than in
2hr.** Context 3 is the VMID the surviving fault names; its window
covers the faulting address; and its page-directory base is the one that
lacks the `VALID` bit that both Linux and Apple's own `getPDEValue` set
unconditionally. In 2hr that was a suggestive anomaly among several
candidates. With the fence path exonerated and the other faults
explained away, it is now the only live explanation on the table.

It is still not proven. The three caveats of 2hr stand unchanged: the
hardware may ignore bit 0 in that register, the values may not come from
`getPDEValue`, and the addresses may be wrong independently of the flag.
What has changed is that nothing else is competing with it any more.

### Instrument note

The 2hy dump carried no strong sanity gate by design, and said so. In
the event it did not need one: the PTE came back valid with sensible
permission bits, its physical address sits beside the ring's from the
same table, and the page contains a value inside the exact range of
sequence numbers the ring's own packets carry. Three independent things
agreeing is what makes this readable — not the dwords on their own.

## 2ib (2026-10-18) — `nowd`: RB0 never programs a GCVM context base

Sixth consecutive armed boot, capture `logs/20261018-151147-metal/`.
Verified before reading: `rbgpu-diag` reads `armed=1` **live** (not in
`-prev`), and the loaded kext UUID `5E02FBCC-27AB-347E-A97C-46289A9E7655`
matches `build/out`. Boot-args carry `-rbgpuforceenable wdt=-1
-rbgpuringprobe -rbgpuringdump`.

```
rbgpu-pm4 = b=34 pa=6c1e7a000 t3=525 r=b1 w=280 | nowd
            | rm@1e a=ffbfdc0000 s=2 | rm@c5 a=ffbfdc0000 s=3 | rm@145 a=ffbfdc0000 s=4
```

`t3=525` type-3 headers in 1024 dwords, so the route (ring VA -> CONTEXT0
flat PTE -> physical page) is sound again and this is a real command ring
page. **No `WRITE_DATA` packet targets any register in `0x28C7..0x28E6`**,
the GCVM context 0..15 `PAGE_TABLE_BASE_ADDR_LO32/HI32` range. Third row of
2ia's pre-written table.

2ia's code logs the `nowd` case as **not** distinguishing "programmed
elsewhere in the ring" from "never programmed", because one page cannot
tell those apart in general. **In this boot the numbers close that gap.**
`wptr = 0x280` = 640 dwords and the dump covers 1024, so the entire written
extent of RB0 since boot lies inside the page that was read; `wptr` has
never exceeded `0x380` in any boot of this series, so nothing has wrapped.
Therefore **RB0 has emitted no VM flush for any context this boot.**

Caveat, owed: `CP_RB0_CNTL` was not read, so the ring's size is inferred
from the pointers rather than measured. One register; fold it into the next
ring probe.

Why it matters. In Linux the only writer of contexts 1-15's page-directory
base is `gmc_v10_0_emit_flush_gpu_tlb`, emitted on **whatever ring the job
runs on** (`amdgpu_ring_emit_wreg`). RB0 is not emitting it. So either
Apple's client work runs on a different ring -- the compute MQD user queue,
which 2hl already argued Metal uses, since RB0 was stalled before any Metal
client existed -- or something programs those registers by a path 2ia's
static search did not cover.

2ia's static search did miss one shape. It looked for a displacement into
the VMM object. Apple's register accessor takes an **encoded id**, and slot
`[0x120]` is the WRITE entry (2fd, 2fg); a write whose id comes from a
runtime table never looks like a struct field access and no displacement
search can find it. That is the hook the user's request asked for, and it
is now specific: hook the accessor's write path and report every write
whose resolved dword offset falls in `0x1667..0x1686`.

Two further readings from this boot, recorded and **not** interpreted:

```
rbgpu-ring  p=0 and p=1 both: r=b1 w=280 cps=90038600 me=15000000
rbgpu-fence fv=ffbfdc0018 idx=3c0 pte=30006c3ca8077 v=1 s=1
            pa=6c3ca8000 rpa=6c1e7a000 seq=5 o=6
            d: 0 0 0 0 0 0 0 0 0 0 31 0  f=0 nz=1
```

The CP is halted (`CP_ME_CNTL = 0x15000000`) in **both** samples this boot;
2hp's +160 s recovery is not visible here (the accumulating note carries two
samples, so this is as likely a note-capacity limit as a behavioural
difference -- do not read a difference into it).

The fence target moved to `0xFF_BFDC_0018` this boot, six dwords into the
page, and **nothing landed there** (`f=0`; the twelve dwords from that
offset are zero except `0x31` eleven dwords on, which is some other slot and
is not interpreted). This does **not** contradict 2hz, where `+0x00` held 6:
this boot stalled at `rptr = 0xb1`, before the first RELEASE_MEM at `@0x1e`
could retire, where 2hz's boot reached `0x2b1` and had passed three of them.
An earlier stop, not a different mechanism. 2hz's finding -- that the fence
writes land when the CP reaches them -- stands.

The fault signature is unchanged for the sixth boot running: `hk=1 c=8 f=8
r=4 o=0`, with `s3009bb h0 va=400300000 v3 R c4`, the `sffffffff h1`
phantom, and `s40953 h0 va=0 v0 W c4`. The GCVM context registers are also
unchanged: c0 `b=78fdfa001` (bit 0 set), c1 `b=fb00b000` and c3
`b=fb015000` (bit 0 clear). Deterministic.


## 2ic (2026-10-18) — the accessor is NAMED, and it never writes the base

Static half of the user's request, done properly at last.

`AMDRadeonX6000_AMDHWRegisters` is the register accessor this project has
been calling blind since 2fd. Its vtable
(`__ZTV29AMDRadeonX6000_AMDHWRegisters`, 0xbfab6a0) decodes under the 2gp
convention -- a CALL offset is the DUMP offset minus 0x10 -- to exactly the
slots five earlier milestones inferred from disassembly alone:

```
call [0x118] = dump +0x128 = AMDHWRegisters::read(uint)          2fd: "0x118 READS"
call [0x120] = dump +0x130 = AMDHWRegisters::write(uint, uint)   2fg: "0x120 WRITES"
call [0x128] = dump +0x138 = readOrModifyWrite(uint, uint)       the (v, 0x80000000) call
call [0x130] = dump +0x140 = readAndModifyWrite(uint, uint)
call [0x148] = dump +0x158 = socRegOffset(SOCRegister)           2fd: "0x148 resolves"
```

Five independent confirmations of a convention that was inferred, not read.
`[0x128]`'s name even explains the one call site that passed `0x80000000` as
its second argument: it ORs a bit in.

`write` is the single choke point for GC MMIO. Its body ends
`movl %ebx,(%rax,%rcx)` with rax = this+0x38 (the aperture) and rcx = reg*4,
and **both** read-modify-write entries tail-jump through the virtual
`[0x120]` slot (`movq 0x120(%rax),%rax` ... `jmpq *%rax`) rather than
storing themselves. So one hook sees every write, RMW included.
`IAMDHWRegisters`' own vtable points every one of those slots at a single
pure-virtual stub (0x975800), so `AMDHWRegisters` is the only
implementation and the class coverage is complete.

Milestone-2c rule: `nm -m` shows
`__ZN29AMDRadeonX6000_AMDHWRegisters5writeEjj` **defined** at 0xbe8d28a.
Rule 9 (2gb): it opens `55 48 89 e5 41 57 41 56 53` and its first branch is
a `jne` at +0x11, outside the five-byte window. Both checked, not assumed.

### The boot

Seventh consecutive armed boot, `logs/20261018-160133-metal/`. Verified
first: `armed=1` **live**, loaded UUID `7DDCC43B-AE8E-347A-BC09-6A3464627948`
= `build/out`.

```
rbgpu-ctxw = hk=1 calls=34657 hits=0 | none
```

The hook took, the accessor performed **34,657** writes, and **not one** was
to 0x28C7..0x28E6, the context 0..15 `PAGE_TABLE_BASE_ADDR` registers.

So all three paths this project could name are now closed by measurement:

| path | result |
|---|---|
| MMIO via a displacement into the VMM object | 2ia: nothing in the kext touches the region |
| the GFX ring, as a PM4 `WRITE_DATA` | 2ib: `nowd`, across the ring's whole written extent |
| the register accessor, by encoded id | **2ic: 0 of 34,657** |

And the disassembly of `AMDGFX10VMM::programAndInvalidateVM` (0xbe9b7a2),
Apple's `emit_flush_gpu_tlb`, shows the flush path really does go through
the hooked function: it takes the accessor from `(this->0x10)->vt[0x2d0]`,
has `vt[0x220]` fill a nine-entry `{reg, val}` array on its stack, then
loops over the nonzero entries with `callq *0x120(%rax)`.

### What this boot does NOT establish, named before it is built on

**`hits=0` has no positive control.** 34,657 calls proves the hook ran. It
proves nothing about whether the hook covers the code that programs GCVM
contexts, because the only range watched is the one range nothing wrote.
Reading `hits=0` as "nothing writes the base" without that control would be
the recurring error in its purest form, and it is not read that way here.

### Two real changes in this boot, not instrument noise

`rbgpu-fault` has only **two** signatures, not three: `c=6 f=6 r=3`, with
`s3009bb h0 va=400300000 v3 R c4` (n=3) and the `sffffffff h1` phantom
(n=3). **The `s40953` VMID-0 WRITE to VA 0 did not happen at all this
boot.** That is direct support for 2hx/2hz's inference that it was a
consequence of the reset path rather than a primary fault -- it is the first
boot of seven where it is absent. The CPF read at `0x4_0030_0000` under
VMID 3 is now the **only** real fault in the boot.

`rbgpu-fence`: `fv=ffbfdc0000 ... seq=5 d: 1 0 80 0 280 0 0 1 0 0 0 0`.
The fence page holds **1** where 2hz's boot held 6, and the ring's packets
carry seq 1/2/3 where 2hx's carried 3/4/5. So the GPU completed **one**
fence this boot against six in 2hz. Progress before the stall varies boot
to boot; the stall does not. (`+0x08`/`+0x10` hold `0x80`/`0x280`, the ring
pointer shadows, `0x280` being this boot's live `wptr`. Noted, not
interpreted, as in 2hz.)

The ring and context readings are otherwise identical to every boot since
2hl: `r=b1 w=280 cps=90038600 me=15000000` in both samples, c0
`b=78fdfa001`, c1 `b=fb00b000`, c3 `b=fb015000`.

## 2id (2026-10-18) — the positive control, and Linux's answer

Linux settles what Apple's init is *supposed* to do, and it is decisive.
`gfxhub_v2_1_setup_vmid_config` loops `i = 0..14` and writes
`GCVM_CONTEXT1_CNTL`, `PAGE_TABLE_START_ADDR_LO32/HI32` and
`PAGE_TABLE_END_ADDR_LO32/HI32` -- and **never** `PAGE_TABLE_BASE_ADDR`.
The base for contexts 1..15 is written in exactly one place upstream:
`gmc_v10_0_emit_flush_gpu_tlb`, per job, on a ring. So there is no
init-time writer of the base in the upstream design either, and 2ic's
negative is what upstream predicts.

That reframes the question. 2hr read c1 and c3 with `cn=0x3b`,
`s=400000`, `e=23fffff` -- CNTL, START and END all carry
programmed-looking values. Those registers sit just outside 2ic's filter:

```
CNTL  = 0x1260 + 0x15FC + n    = 0x285C + n
BASE  = 0x1260 + 0x1667 + 2n   = 0x28C7 + 2n     <- all 2ic watched
START = 0x1260 + 0x1687 + 2n   = 0x28E7 + 2n
END   = 0x1260 + 0x16A7 + 2n   = 0x2907 + 2n
```

2id widens the same hook to `0x285C..0x2926` and reports a 16-bit mask per
group -- which *contexts* were written -- because that is what fits in an
NVRAM note. It also hooks `programAndInvalidateVM` and counts calls.
Rule 9 on that one: it opens `55 48 89 e5 41 57 41 56 41 55 41 54 53`,
pushes and a movq, first branch 0x9a bytes in. Checked.

The note gains `cm= sm= em= bm=` plus per-group counts and `pivm=<calls>/<hooked>`.

What a reading would mean, written down before the boot:

| reading | means |
|---|---|
| `cm`/`sm`/`em` set, `bm=0000` | the hook DOES reach the code that programs GCVM contexts, and that code writes CNTL/START/END and never the base -- exactly Linux. c1/c3's bases were then never written by anything, and 2hr read whatever the registers held, not a value any code chose. |
| all four masks `0000` | the hook does not reach that code and 2ic's `hits=0` is worth nothing |
| `bm` has a bit set | something does write a base through the accessor and 2ic's filter or counting was wrong |
| `pivm=0` with `bm=0000` | the VM flush never ran. Then nothing has even tried to program a base, and the missing VALID bit is not a wrong value but an absent write. |
| `pivm>0` with `bm=0000` | the flush ran and its request array contains no base register -- Apple's flush invalidates without programming, and the base must come from somewhere else again |

The last two rows are the point. 2hr's "the value is missing bit 0" and
"the register was never written" look identical from a register read, and
`pivm` is the first thing that separates them.


## 2id (2026-10-18) — the positive control lands, and the flush does not program

Eighth consecutive armed boot, `logs/20261018-161037-metal/`. `armed=1`
**live**, loaded UUID `76A82E0A-AD25-39EA-8F35-F5AAB741B776` = `build/out`.

```
rbgpu-ctxw = hk=1 calls=35016 hits=0
             | cm=fffe sm=0000 em=0000 bm=0000 nc=15 ns=0 ne=0 mid=504
             | pivm=336/1 h=0003 | none
```

**`cm=fffe`: the positive control.** Contexts **1 through 15**, fifteen
writes, context 0's bit clear. That is precisely Linux's
`gfxhub_v2_1_setup_vmid_config` loop - `i = 0..14` over `CONTEXT1_CNTL + i`,
which is contexts 1..15 and never context 0. So the hook demonstrably
reaches the code that programs GCVM contexts, and 2ic's `hits=0` is a real
negative rather than an untested range. The control 2ic lacked is supplied,
and it did not have to be argued for - the mask matched upstream's loop
bounds exactly.

**`pivm=336`, hubs 0 and 1.** `programAndInvalidateVM` ran **336 times** and
wrote **no page-table base, ever**. That is the fifth row of 2id's table:
Apple's flush *invalidates* without *programming*. The 504 `mid` writes
land between `CONTEXT15_CNTL` and the base block, which is the invalidation
request/ack/range registers - the flush's own traffic, exactly where 336
flushes should put it. So the two possibilities 2hr could not separate are
separated: this is **not** a flush writing a wrong value. It is a base that
nothing writes.

**The result that was not predicted: `sm=0000 em=0000`.** 2hr reads c1 and
c3 with `s=400000` and `e=23fffff` - programmed-looking START/END values,
identical across both contexts, the shape of a loop that writes all fifteen
alike. Linux writes START and END in the same loop as CNTL. **Apple's X6000
writes CNTL and nothing else.** So for contexts 1..15 this driver sets the
enable word and leaves START, END and BASE holding values it never wrote.

`initializeTlbRegs` (0xbeae6c6) does write through `[0x120]`, so it was
inside the 35,016 and whatever it wrote was seen. The negative is not a
coverage gap in that direction.

Everything else reproduces: `rbgpu-fault` back to three signatures
(`c=8 f=8 r=4`, the `s40953` row present again after its absence in 2ic),
`rbgpu-pm4` `t3=799 r=2b1 w=380` with RELEASE_MEM seq 3/4/5, `rbgpu-fence`
holding **6** with `seq=8` - the 2hx/2hz "far" variant of the stall, where
2ic's boot was the "near" one (`r=b1`, one fence). Progress before the
stall varies; the stall does not. Contexts unchanged: c0 `b=78fdfa001`,
c1 `b=fb00b000`, c3 `b=fb015000`.

## 2ie (2026-10-18) — the other write path, and why the callback matters

`AMDRadeonX6800HWLibs` is Apple's port of the very Linux file this is being
compared against, and it says so in its symbol names:
`_vm_10_1_setup_vmid_config`, `_vm_10_1_initialize_page_table_registers`,
`_vm_10_1_enable_virtual_domain`, `_vm_10_1_initialize_tlb_registers`, and
its own `_vm_10_1_write_register`. The chain:

```
_vm_10_1_write_register(dev, hubmask, reg_id, index, value)
  -> _vm_10_1_get_register_offset(dev, 1, reg_id, index)   = offset
  -> _gvm_write_register((dev->0), offset, value, 0xB)     0xB = GC_HWID
  -> _gvm_cgs_write_register(ctx, offset, value, hwid)
```

`_vm_10_1_enable_virtual_domain` calls, in order, `setup_fragment_size`,
`enable_tlb`, `setup_vmid_config(dev, hub, vmid, ..., 1)` and
`program_default_page_addr` - and no base write among them, which is
consistent with 2id.

Rule 9 (2gb) on both candidates, checked: `_gvm_write_register` is a
five-byte thunk `55 48 89 e5 5d e9 ...` whose `jmp rel32` begins at byte
**5**, at the very edge of the window - so it is NOT the hook site.
`_gvm_cgs_write_register` (0x126779ed) opens `55 48 89 e5 4c 8b 87 68 0c`,
pushes and movqs with no branch in the first five bytes, and it is where
the resolved offset arrives. Both are local symbols (`nm` prints `t`)
**defined** in HWLibs, so the milestone-2c rule is satisfied.

Its body is the reason this probe exists:

```
movq 0xc68(%rdi), %r8 ; movq 0x38(%r8), %rax ; jmpq *%rax
```

**HWLibs does not touch MMIO itself.** It tail-jumps to a callback the host
driver registered. So HWLibs' writes may already be arriving at X6000's
`AMDHWRegisters::write` - the hooked function - and 2id's `cm=fffe` may BE
`setup_vmid_config` seen from the other end. That would be a tidy story and
it is not assumed; it is measured, by comparing the two hooks' masks.

| reading | means |
|---|---|
| gvm `cm=fffe` too | one path, two views. 2id already covered HWLibs, and **nothing anywhere** programs START, END or BASE for contexts 1..15 |
| gvm `cm=0000`, calls large | two separate paths; HWLibs writes plenty but not the context block, and 2hr's START/END values come from neither |
| gvm `bm` nonzero | HWLibs DOES write the base, on a path X6000's accessor never sees, and the value is in this note |
| gvm `calls=0` | HWLibs wrote no register this boot; the silence is HWLibs, not the chip |

One thing is deliberately not assumed. X6000's accessor returns a
**segment-included** offset (2fe: 0x3041 for CP_RB0_CNTL, raw 0x1DE1, GC
seg0 0x1260), but HWLibs passes a hwid *alongside* the offset, which is how
a design that applies the segment downstream looks. So the offset here may
be raw. **Both framings are counted** and the note says which matched;
exactly one should. Guessing would be the recurring error on a register
offset, and this project has paid for that before.

Note `rbgpu-gvmw`, gated on `-rbgpuringprobe` (already in the boot-args),
flushed from the same thread_call as 2ic/2id. No config.plist change.


## 2ie result (2026-10-18) — two paths, and NOTHING writes the base

Ninth consecutive armed boot, `logs/20261018-165514-metal/`. `armed=1`
**live**, loaded UUID `8FC92114-46EC-3CDD-A569-15B2BE64C308` = `build/out`.

```
rbgpu-gvmw = hk=1 calls=462 gc=323
  | raw cm=0000 bm=0000 sm=0000 em=0000 n=0/0/0/0 m=0 bf=0 bl=0
  | seg cm=ffff bm=0001 sm=0001 em=0001 n=17/2/2/2 m=63 bf=fdfa001 bl=0
rbgpu-ctxw = hk=1 calls=35027 hits=0 | cm=fffe sm=0000 em=0000 bm=0000
             nc=15 ns=0 ne=0 mid=504 | pivm=336/1 h=0003 | none
```

**The framing question is answered and it was worth asking.** The `seg`
view matched everything and the `raw` view matched nothing at all, so
HWLibs passes segment-included dword offsets too and the hwid is not a
segment selector. Exactly one framing matched, which is what made the
reading safe; had both matched, neither would have been.

**The two paths are SEPARATE.** HWLibs' `cm=ffff` includes context 0 and
counts 17 writes; X6000's `cm=fffe` excludes context 0 and counts 15. Had
HWLibs' writes flowed through the callback into `AMDHWRegisters::write`,
X6000's mask would carry bit 0 and its count would be the larger. It does
not. So the callback at `ctx->0xc68+0x38` is **not** X6000's accessor, the
two hooks cover different traffic, and 2ie's first table row is excluded.

Coverage of HWLibs is complete, checked rather than assumed: all ten call
sites into `_gvm_cgs_write_register` are hooked by hooking it, and
`_gvm_write_register_virt` - the one sibling entry point - funnels into the
same function at six separate sites. Only a direct MMIO store inside
HWLibs would be invisible, and none is known.

**`bm=0001`: HWLibs writes a page-directory base for context 0 and no
other.** `bf=0xFDFA001`, and that value is the probe's own corroboration -
bit 0 **set**, and missing the carve-out offset, which is exactly the bug
2em found and patched (`rbgpu-diag` still reads `ptb_written=1
ptb_back=78fdfa001`, our corrected value written over it). So this probe
is reading real HWLibs output, and **Apple's code sets VALID wherever it
writes a base**, as `getPDEValue`'s closing `incq %rax` says it must.

Both boots together:

| path | CNTL | BASE | START | END |
|---|---|---|---|---|
| X6000 `AMDHWRegisters::write` | ctx 1-15 | - | - | - |
| HWLibs `_gvm_cgs_write_register` | ctx 0-15 | **ctx 0** | ctx 0 | ctx 0 |

**No path writes BASE, START or END for contexts 1 through 15.**

### This closes 2hr

2hr found contexts 1 and 3 holding page-directory bases with bit 0 clear
and listed three caveats: the hardware might ignore bit 0, the values might
not come from `getPDEValue`, and the addresses might be wrong independently
of the flag. The third is now moot and the second is answered in the
strongest way: **those values come from nowhere.** Nothing wrote them.

So the missing VALID bit was never a wrong value. It is an **absent
write** - c1/c3's registers hold whatever they held, and reading them as
"a base Apple computed badly" was the wrong frame for four milestones.
2hr's own reading stands; its implied cause does not.

Combined with 2id, the shape of the defect is now specific: in Linux the
base for contexts 1..15 is written per job by
`amdgpu_vm_flush -> ring->emit_vm_flush`, at the moment a VMID is bound to
a VM. Apple's `programAndInvalidateVM` ran 336 times and invalidated
without programming (2id), RB0 carries no such packet (2ib), neither
register path writes it (2ic/2id/2ie) - **and the chip then faults on a CPF
read under VMID 3**, which is a VMID whose translation base was never
established.

Boot otherwise reproduces the "far" variant exactly: `t3=799 r=2b1 w=380`,
RELEASE_MEM seq 3/4/5, fence page holding 6 with `seq=8`, three fault
signatures, contexts unchanged.

## 2if (2026-10-18) — the VMID handout and the value Apple computes

Two hooks, both rule-9 checked and both defined in AMDRadeonX6000:

```
AMDHWVMM::assignVMID(IAMDHWVMContext*, uint &vmid, AMDAccelChannel*,
                     IOAccelEvent**, bool*)        0xbe91376
  prologue 55 48 89 e5 41 57 41 56 41 55 41 54 53   pushes and a movq
AMDGFX10VMM::getPDEValue(eAMD_VMPT_LEVEL, uint64)  0xbe9b9de
  prologue 55 48 89 e5 ff c6 8b 87 34               the ff c6 at byte 4 is
                                                    `incl %esi`, not a branch;
                                                    first branch is a jne at +0xe
```

`getPDEValue` is the function 2hr wanted and could not justify hooking
until the write path was understood. It is the one place Apple computes a
page-directory entry and its tail is `incq %rax`, so it ORs VALID
unconditionally; `nv` counts any return with bit 0 clear and should be 0.
`assignVMID`'s out-parameter is read **after** the original runs, the only
moment it holds the assigned VMID.

What a reading would mean, before the boot:

| reading | means |
|---|---|
| `vmids` has bit 3, and a good L0/L1 value | Apple COMPUTED a base for the faulting context and never wrote it. The write is the gap, and performing it is the fix. |
| `vmids` has bit 3, no getPDEValue for it | Apple never computes a base for that context either; the gap is further upstream than the write |
| `vmids` bit 3 clear | the chip faulted under a VMID this driver never bound - a different bug, and the CPF read at 0x4_0030_0000 needs another explanation |
| `av=0` | no VMID was assigned this boot; the silence is the hook, not the driver |
| `nv` nonzero | the `incq %rax` reading of getPDEValue's tail is wrong, and 2hr's Apple-convention argument falls with it |

Note `rbgpu-vmid`, gated on `-rbgpuringprobe`, flushed from the same
thread_call. No config.plist change.


## 2if result (2026-10-18) — VMID 3 IS bound, and Apple computes no base for it

Tenth consecutive armed boot, `logs/20261018-171436-metal/`. `armed=1`
**live**, loaded UUID `0980B50B-A3AF-3A19-B090-252F6CC62702` = `build/out`.

```
rbgpu-vmid = hk=1 av=41 fail=41 vmids=000e | pde nv=0 o=0
             | L0 c3   f=2000000000000001 l=2000000000000001
             | L1 c111 f=0100000000000001 l=0100000000000001
```

### `fail=41` is a MISLABEL, caught before it was built on

The counter increments on `r != 0`, named `fail` on the assumption that a
nonzero return means failure. That assumption was never checked, and
CLAUDE.md names this exact mistake: *a return code is not a result until
the code that sets it has been read* (2ha, the 25th instance).

So it was read. `AMDHWVMM::assignVMID` sets its return value in **one**
place in the entire function:

```
be914e3:  movb $0x1, %al
be914f3:  retq
```

One writer, and it writes **1**. The function returns true. So `r != 0` is
**success**, and the correct reading is `success=41`: **all 41 VMID
assignments succeeded.** Nothing failed. Had this gone into a conclusion
unchecked it would have been the 26th instance; it is instead the first one
caught by the rule rather than by a later boot.

### What the note actually says

**`vmids=000e`** - bits 1, 2 and 3. **VMID 3 was handed out**, successfully,
to a real VM context. So the chip is not faulting under a VMID the driver
never bound; 2if's third table row is excluded and the main line survives.

**`nv=0`** - not one of the 114 `getPDEValue` returns had bit 0 clear. The
`incq %rax` reading of its tail is now **measured**, not inferred from
disassembly, and 2hr's "every PDE Apple builds has VALID set" is confirmed
on hardware.

**The PDE values carry no address.** `L1` was called 111 times and `L0`
three times, and every call at a given level returned the identical value:
`0x0100000000000001` and `0x2000000000000001`. Bit 0 set, a
block-fragment-size field, and an **address field of zero**. These are
page-table *content* entries, not a page-directory base for a context.

So this is row 2 of 2if's table: **VMID 3 is assigned, and `getPDEValue`
never produces a base for it.** Apple has no code that computes a
page-directory base for contexts 1..15 - which is exactly consistent with
2ic/2id/2ie finding nothing that writes one. The gap is not a write that
was skipped; there is no value, and no write, anywhere in the path.

Everything else reproduces for the tenth time, byte for byte: `ctxw` and
`gvmw` identical to the previous boot (`calls=35021` against 35027, the
only difference), three fault signatures, `t3=799 r=2b1 w=380`, fence
holding 6 with `seq=8`, c1 `b=fb00b000`, c3 `b=fb015000`.


## 2ig (2026-10-18) — the first write to a GCVM context register

Everything that leads here is measured, not argued. Nothing writes BASE,
START or END for contexts 1..15: not X6000's accessor (2ic/2id, 0 of
~35,000 writes, with cm=fffe as the positive control), not HWLibs (2ie,
context 0 only), not RB0 (2ib, `nowd` across the ring's whole written
extent). Apple's flush ran **336 times** and invalidated without
programming (2id). `getPDEValue` returns page-table content entries with a
zero address field and no base for any context (2if). And yet VMID 3 is
bound - successfully, 41 assignments deep - and the chip faults on a CPF
read at `0x4_0030_0000` under exactly that VMID, identically, in ten
consecutive boots.

So the register holds a leftover. 2ig asks whether giving it a real root
directory stops the fault. User approved the write (2026-10-18), choosing
the self-gating one-boot form over an extra observe-only boot.

### The strict match — the value is not guessed

Apple's own allocator hands out the root directories.
`allocVMBlock(level 0)` is called three times, once per VM context, which
agrees with 2if's three level-0 `getPDEValue` calls and three VMIDs handed
out; level 1 is the page-table tier (111 calls). 2ig records each level-0
block's `elem+0x20` and `elem+0x28` - the fields `allocVMBlock` itself
tests for success (2eg). The context object bound to VMID 3, captured from
`assignVMID`'s own argument, is then scanned for a qword **equal** to one of
those roots.

A match means the object really holds that root at an offset Apple chose.
No match means the probe **refuses and only dumps**, which is why the boot
pays for itself either way: the dump is what a value-finding pass would
start from. The refusal is logged as *the probe could not identify a value
it was willing to write* - not as evidence about the register.

### What it does to the chip

Two writes at the +100 s sample, after the hang is long established (the
+12 s sample has shown it in every boot):

```
GCVM_CONTEXT3_PAGE_TABLE_BASE_ADDR_LO32  dword 0x28CD = root | 1
GCVM_CONTEXT3_PAGE_TABLE_BASE_ADDR_HI32  dword 0x28CE = root >> 32
```

Bit 0 is `AMDGPU_PTE_VALID`. Linux ORs it unconditionally
(`amdgpu_gmc_pd_addr`, Vega10+); Apple's `getPDEValue` ends `incq %rax`;
and 2if **measured** `nv=0` across 114 calls. So setting it follows both
conventions rather than inventing one. Nothing else is touched - no
invalidation, no other context - and the previous values are read back and
committed to NVRAM **before** the write, so a hang inside the write still
leaves the before-state and the intended value on record.

It cannot un-hang this boot, and is not expected to: by +100 s the CP has
been halted for ninety seconds and the work owing the timestamp was thrown
away by the reset (2hp). What it can show is whether the write **sticks**,
whether the CPF fault stops recurring, and whether `CP_ME_CNTL` clears -
read at +160 s by the same instruments that read +12 s. If those say yes,
the next boot does this at bind time, where it can prevent the hang.

Gated on its own boot-arg `-rbgpuptwrite` (added with
`efi.sh bootarg add`), so no ordinary `-rbgpuringprobe` boot ever writes a
register by accident and it can be removed without a rebuild. Without the
arg the probe does the whole identification and writes nothing - a dry run
whose note records exactly what it would have written.

| `rbgpu-ptw` reading | means |
|---|---|
| `GO ... back=<new>` and the +160 s fault gone | the base was the fault's cause and the write is the fix to generalise |
| `GO ... back=<new>` and the fault unchanged | the base takes the value and is not, by itself, enough |
| `GO ... back=<old>` | the register refused the write - 2hr's first caveat, the hardware ignoring this field, answered directly |
| `REFUSED nomatch` + dump | no root in the scanned window; the dump feeds the next pass |
| `REFUSED no ctx3` | `assignVMID` never bound a context to VMID 3 **in this boot** |
| `OFF` | the arg is missing; the dry run still names the value |


## 2ig result (2026-10-18) — the register was never a leftover. It was TRUNCATED.

Eleventh consecutive armed boot, `logs/20261018-172635-metal/`, `armed=1`
live at capture, UUID `376E6F1C-A373-3B75-955D-28AF641A7234` = `build/out`,
and `-rbgpuptwrite` present in the live `kern.bootargs`.

```
rbgpu-ptw = b=39 arm=1 ctx3=ffffff8b6de37980 nroot=3
            r0=f4fb000000/fff r1=f4fb00b000/fff r2=f4fb015000/fff
            m@b8=f4fb015000 o=28cd/28ce
            was=fb015000/0 new=fb015001/f4 back=fb015001/f4
```

The strict match fired. Apple's allocator handed out **three** level-0
roots - `0xF4_FB00_0000`, `0xF4_FB00_B000`, `0xF4_FB01_5000` - all MC
addresses in the carve-out (`0xF4_0000_0000`, a chip fact since 2dl), one
per VM context, agreeing with 2if's three level-0 `getPDEValue` calls and
three VMIDs handed out. The VMID-3 context holds `0xF4_FB01_5000` at
`ctx+0xb8`.

### The finding, and a withdrawal

Context 3's register held **`0xFB015000` with HI32 = 0**. That is the low
32 bits of *that very context's own root directory*, with the carve-out
high bits `0xF4` missing and bit 0 clear. Context 1 is the same shape:
register `0xFB00B000`, root `0xF4_FB00_B000`.

**So 2ie's inference is WITHDRAWN.** I wrote that nothing writes these
registers and they therefore hold leftovers. The measurements behind that
(2ic/2id/2ie) stand and are not in question - but the inference does not.
A leftover cannot coincidentally equal the low half of its own context's
root, and certainly not twice. **Something writes them on a fourth path
none of the three hooks sees**, and finding it is now owed.

And the shape of the corruption is one this project has already paid for:
**it is 2em's bug again** - an MC address losing its carve-out high bits,
`0xF4_FB01_5000` written as `0x0_FB01_5000`. 2em fixed exactly this for
CONTEXT0. Contexts 1..15 have the same defect and nothing has ever fixed
it there.

So the four-milestone frame of "the missing VALID bit" was aimed at the
smaller half of the problem. Bit 0 is missing, yes - but so are the top
eight bits of the address, and a page-directory base pointing at
`0x0_FB01_5000` instead of `0xF4_FB01_5000` would fault regardless of what
bit 0 said. `WALKER_ERROR` is a walker complaint, and this is what the
walker was given.

### What the write settled

`back = fb015001/f4`: the register **accepted both** the VALID bit and
`HI32 = 0xF4`. **2hr's first caveat is dead** - the hardware does not
ignore bit 0 in this field, and it does not ignore the high dword either.

### After the write

The in-boot capture ran at +115 s, before the +160 s sample, so phase 2
exists only in NVRAM. Its nonce `b=39` matches `state.txt`'s, which is what
makes it this boot's (2gd); `armed=0` live only says a later boot has since
started.

```
p=2 vmf=40953 vmalo=0 vmahi=0 r=2b1 w=380 cps=94038600 grbm=a0003028
    g2=74200008 me=15000000
rbgpu-fault  c=14 f=14 r=7
  s3009bb   h0 n=2 va=400280000 v3 R c4
  sffffffff h1 n=7
  s40953    h0 n=5 v0 W c4
```

In the **six polls after the write, the VMID-3 CPF fault did not recur
once**, while the hub-1 phantom went 4 -> 7 and `s40953` went 2 -> 5. That
is a real signal and a **weak** one: n=2 over fourteen polls against n=1-3
over eight in earlier boots is nowhere near the bar 2hv set, and calling it
confirmation would be the recurring error. It is recorded as suggestive,
nothing more.

The CP is still halted (`me=15000000`), `rptr`/`wptr` are unmoved and
`0x40953` still latches - all expected. A write at +100 s cannot revive
work the reset discarded at +160 s in 2hp's boot; the hang was ninety
seconds old before the probe touched anything. This boot was never going
to recover and was not built to.

**The test that matters is the same write at bind time**, before any work
is submitted - and 2ig has established everything that test needs: where
the value lives (`ctx+0xb8`, matched against Apple's own allocator), that
the register takes it, and that the correction is the carve-out high bits
as well as bit 0.


## 2ih (2026-10-18) — the same write, at the only moment it can matter

2ig established everything this needs: where the value lives (`ctx+0xb8`,
matched against Apple's own allocator rather than assumed), that the
register accepts both the VALID bit and the carve-out high dword, and that
the defect is **truncation** - `0xF4_FB01_5000` held as `0x0_FB01_5000` -
which is 2em's bug on contexts 1..15.

2ih writes the correction immediately after `assignVMID` binds a context,
before any work is submitted under that VMID. Per bind:

```
GCVM_CONTEXT<vmid>_PAGE_TABLE_BASE_ADDR_LO32 = (root & 0xFFFFFFFF) | 1
GCVM_CONTEXT<vmid>_PAGE_TABLE_BASE_ADDR_HI32 = root >> 32
dword = 0x1667 + 2*vmid raw  ->  0x28C7 + 2*vmid through socRegOffset
```

**VMID 0 is never touched.** 2em already owns context 0 and it reads
`0x7_8FDF_A001`, correct and valid. Only contexts 1..15, and only ones
whose `ctx+0xb8` equals a level-0 root `allocVMBlock` actually handed out.
`0xb8` is a candidate *offset*; the match is what turns it into a value, so
a context laid out differently is refused and counted, never misread.

Gated on `-rbgpuptfix`, deliberately a different arg from 2ig's
`-rbgpuptwrite` so the two experiments can never be confused in a capture.
Without the arg the hook identifies, counts and records every value and
writes nothing.

Risk, stated plainly: this writes a live GPU register during normal
operation on every bind, where 2ig wrote twice at a moment already known to
be dead. A wrong value could fault differently rather than less. The strict
match bounds that - the only values written are addresses Apple's own
allocator produced for that very context - and every write's before, after
and read-back is recorded so a worse outcome is legible rather than
mysterious.

| `rbgpu-ptfix` reading | means |
|---|---|
| `wrote>0 badback=0` and the VMID-3 fault gone at +12 s | the truncation was the cause and the correction is the fix |
| `wrote>0` and the fault unchanged | the base is right and something else also breaks the walk |
| `wrote>0 badback>0` | a register refused the value, which 2ig's single write did not see |
| `wrote=0 nomatch=binds` | `ctx+0xb8` is not the root in general; 2ig matched once and that offset does not generalise. Nothing written. |
| `noobj>0` | the accessor was not available at bind time; the write could not be attempted and the silence is that |

The decisive reading is the **+12 s** sample, which in every one of eleven
boots has already shown `vmf=3009bb`, VMID 3, CPF, `WALKER_ERROR`. If that
sample comes back clean, the eleven-boot signature is broken.


## 2ih result (2026-10-18) — the write lands perfectly and changes nothing

Twelfth consecutive armed boot, `logs/20261018-173507-metal/`, `armed=1`
live, UUID `42B30D91-0F24-3110-A548-700916CA7D4E` = `build/out`,
`-rbgpuptfix` present and `-rbgpuptwrite` removed.

```
rbgpu-ptfix = b=40 arm=1 binds=41 wrote=41 nomatch=0 noobj=0 badback=0
              vmids=000e | first root=f4fb000000 was=0/0
              new=fb000001/f4 back=fb000001/f4
rbgpu-vm    = c1:cn=3b,b=f4fb00b001  c3:cn=3b,b=f4fb015001   (both samples)
```

Mechanically flawless: 41 binds, 41 writes, no refusals, no bad read-backs,
and the corrected values **persist** through +100 s - nothing overwrote
them. The instrument did exactly what it was built to do.

And `+12 s` is byte-identical to the eleven boots before it: `vmf=3009bb`,
VMID 3, CPF, `WALKER_ERROR`, VA `0x4_0030_0000`, `me=15000000`. Metal still
hangs at commit. Row 2 of 2ih's table.

### `was=0/0` - the mystery writer runs INSIDE assignVMID

The first bind found the register **zero**, not `0xFB000000`. So the
truncated value is not there before binding: it is written at or during the
bind, on a path none of 2ic/2id/2ie's hooks sees, and 2ih's write - which
runs after the original returns - lands last and sticks. The fourth writer
is still unidentified but now has a time window, and the fact that our
later write is never clobbered means the mechanism of 2ih is sound.

### Why it changed nothing: the wrong address space

2em established what this register wants and verified it on context 0:

```
pa = (mc - fb_base) + vram_base_offset
     fb_base          = GCMC_VM_FB_LOCATION_BASE << 24 = 0xF4_0000_0000
     vram_base_offset = GCMC_VM_FB_OFFSET        << 24 = 0x7_8000_0000
```

TTL's table is at MC `0xF4_0FDF_A000`; `0x0FDF_A000 + 0x7_8000_0000` =
`0x7_8FDF_A000`, which is what c0 holds and what translates. **2ih wrote an
MC address into a register that wants a physical one.** The read-back
verified, the walker still had nonsense, and `badback=0` was never evidence
about correctness - only about the register accepting bits.

The truncated values say the same thing from the other side, and this is
the part worth keeping. The three roots are MC `0xF4_FB00_0000`,
`0xF4_FB00_B000`, `0xF4_FB01_5000`; the registers held `0xFB000000`,
`0xFB00B000`, `0xFB015000` - **exactly `root - fb_base`**, with
`vram_base_offset` omitted. So this was never a generic truncation of high
bits. It is **2em's defect**, the same missing `vram_base_offset`, on
contexts 1..15 where nothing has ever corrected it. 2ig's "the carve-out
high bits were dropped" was the right observation with the wrong arithmetic
behind it, and the correction it implied was therefore wrong too.

## 2ii (2026-10-18) — 2em's arithmetic, applied per context, self-checking

Same hook, same bind-time moment, same strict root match. Only the value
changes:

```
want = (root - fb_base) + vram_base_offset
GCVM_CONTEXT<vmid>_PAGE_TABLE_BASE_ADDR = want | 1
```

For context 3: `0xFB01_5000 + 0x7_8000_0000` = **`0x8_7B01_5001`**, where
2ih wrote `0xF4_FB01_5001`.

The write is now **self-checking**, which is the part that makes it worth a
boot. If the register already holds a nonzero value it must equal
`root - fb_base`; when it does, the root, the transform and the defect all
agree *before* anything is written. When it does not, the write is
**refused** and counted (`relbad`) rather than guessed - and the note
records the value it refused on. `relok` counts the confirmations. Refused
also if `FB_LOCATION_BASE` or `FB_OFFSET` have not been read yet
(`nofb`), since the transform is not derivable without them.

| `rbgpu-ptfix` reading | means |
|---|---|
| `relok>0 wrote>0` and the +12 s fault gone | 2em's defect on contexts 1..15 was the cause, and this is the fix |
| `relok>0 wrote>0`, fault unchanged | the base is now in the right space with the right value and the walk still fails - the page tables it points at become the next question |
| `relbad>0` | the `root - fb_base` relationship does not hold; the arithmetic is wrong and the note names the value |
| `nofb=binds` | the FB registers were not read before the first bind; ordering problem, not a chip result |

2em's own residual risk applies unchanged and is worth repeating: if the
walker wants the BAR0 alias (`0x8_8000_0000` + offset) rather than the DRAM
alias, this points at another wrong page and the same `MAPPING_ERROR`
returns - no worse than now. And no TLB invalidation is issued, so a stale
negative translation remains a second candidate if the fault persists.


## 2ii result (2026-10-18) — the transform is confirmed and the address does not exist

Thirteenth consecutive armed boot, `logs/20261018-174223-metal/`, UUID
`BB031957-5027-38F8-A309-F6AEF09B9482` = `build/out`.

```
rbgpu-ptfix = b=41 arm=1 binds=41 wrote=6 nomatch=0 noobj=0 badback=0
              nofb=0 relok=3 relbad=35 vmids=000e
rbgpu-vm    = c1:b=87b00b001  c3:b=87b015001     (both samples)
```

**`relok=3` is a real result.** Three binds found the register already
holding exactly `root - fb_base`. So whoever programs it performs precisely
that subtraction and omits `vram_base_offset` - 2em's defect on contexts
1..15, established rather than inferred.

**`relbad=35` is a bug in my own check**, not a fact about the chip: after
the first corrected write the register holds `0x8_7B00_0001`, which the
check compares against `fb_off` and refuses. It refused its own correct
value. Fixed in 2ij by treating "already holds what we would write" as a
distinct, counted outcome.

**And the fault is identical for the thirteenth time**: `vmf=3009bb`, VMID
3, CPF, `WALKER_ERROR`, VA `0x4_0030_0000`, `me=15000000`, Metal hanging at
commit.

### Why: the address written does not exist

A free host check settled it. `hw.memsize` is **32 GB**. The carve-out's
system-physical base is `0x7_8000_0000` = **30 GB**, so the aperture is at
most **2 GB**. But `fb_off` for these roots is `0xFB01_5000` = **4.2 GB** -
more than twice the entire carve-out - and 2ii's target
`0x8_7B01_5000` is **~33 GB, past the end of installed RAM.**

Context 0 is the contrast that proves the rule: its `fb_off` is
`0x0FDF_A000` = 250 MB, inside the aperture, target `0x7_8FDF_A000` = 30.2
GB, real memory, and it translates.

So `mc2pa` applies to c0's table and **not** to these roots: they are not
VRAM addresses. That agrees with 2hz, which found the page tables carrying
`AMDGPU_PTE_SYSTEM`, and with 2em's own note that *a SYSTEM entry holds a
bus address and never goes through mc2pa*. 2ii put a non-existent address
in the register, the register accepted it (`badback=0`, which was never
evidence about correctness), and the walker had nonsense of a new kind.

`relok=3` survives this and sharpens it: the driver really does subtract
`fb_base` from these roots, which now reads as **the driver applying a VRAM
transform to a non-VRAM address**. That would be the defect one level above
the register, and it is the first explanation in this series that accounts
for both the truncated value and the fact that correcting it the obvious
way cannot work.

### The check that should have run before the write

`fb_off` must lie inside the FB aperture. I had every number needed to see
that and did not look - the same shape of error as reading a value without
establishing what makes it meaningful, committed on a write rather than a
read. 2ij adds the gate, measured from the chip:

## 2ij (2026-10-18) — refuse what is outside the aperture

```
fb_top = GCMC_VM_FB_LOCATION_TOP (dword 0x16FD) << 24 | 0xFFFFFF
refuse unless fb_base < fb_top and root <= fb_top
```

Read from the chip rather than inferred from the host's `hw.memsize`, so
the gate holds whatever the firmware configured. Three counters added:
`out` (root outside the aperture - the write refused), `alr` (register
already holds the value we would write - idempotent, replacing 2ii's 35
false refusals), and `fbtop` (the register as read, so the aperture extent
is in the record).

| `rbgpu-ptfix` reading | means |
|---|---|
| `out=binds wrote=0` | these page tables are **not** in the carve-out. The register wants their bus address and the value must come from somewhere other than this subtraction - which is then the next thing to find. |
| `out=0 relok>0 wrote>0`, fault gone | the aperture gate was the only thing wrong and the correction stands |
| `out=0 relok>0 wrote>0`, fault unchanged | the base is right in the right space and the walk still fails; the page tables it points at become the question |
| `alr=binds` | nothing needed changing this boot |

The expected reading is `out=binds wrote=0`, i.e. the probe refusing
everything it wrote last boot. That is the point: a boot that writes
nothing and says why is worth more than one that writes an address past the
end of RAM.


## 2ij result (2026-10-18) — the gate was right and the geometry is impossible

Fourteenth consecutive armed boot, `logs/20261018-202320-metal/`, UUID
`B2A7FD10-9BAF-347E-92CA-0C6A292C1534` = `build/out`.

```
rbgpu-ptfix = b=42 arm=1 binds=41 wrote=6 nomatch=0 noobj=0 badback=0
              nofb=0 relok=3 relbad=0 out=0 alr=35 fbtop=f4ff vmids=000e
```

The idempotency fix worked: `relbad` 35 -> **0**, `alr=35`. The check no
longer refuses its own correct value.

**`out=0`, and that is the result.** `fbtop=0xF4FF` makes the FB aperture
MC `0xF4_0000_0000 .. 0xF4_FFFF_FFFF` - a full **4 GB** - so the roots at
`0xF4_FB01_5000`, 3.92 GB in, are inside it. The gate passed them
correctly. My host-side reasoning last milestone said the aperture was 2 GB
and it was wrong; the chip says 4 GB.

(Also correcting my own arithmetic: `0xFB01_5000` is 3.92 GB, not the
4.2 GB I wrote. The conclusion that 2ii's target lies past installed RAM is
unchanged - `0x8_7B01_5000` is ~33.9 GB against 32 GB of RAM - but the
figure was wrong and is corrected here rather than left in the record.)

So the geometry is contradictory, and the contradiction is the finding:

| | |
|---|---|
| FB aperture (MC) | 4 GB, `0xF4_0000_0000`-`0xF4_FFFF_FFFF` |
| carve-out system-physical base | `0x7_8000_0000` = 30 GB |
| installed RAM | 32 GB |
| aperture's top 2 GB maps to | 32-34 GB, which is nothing |
| the roots sit at | ~33.9 GB, in that overhang |

**The FB aperture is twice the memory behind it, and the page-table roots
are in the unbacked half.** Either `vram_base_offset = FB_OFFSET << 24` is
not the right base on an APU, or these roots are reached through a
different aperture entirely.

And the fault is identical for the fourteenth time: `vmf=3009bb`, VMID 3,
CPF, `WALKER_ERROR`, VA `0x4_0030_0000`, `me=15000000`.

## 2ik (2026-10-18) — stop writing, read the aperture map

Three writes have now been attempted at this register, each assuming a
transform: 2ih the raw MC address, 2ii `mc2pa`, 2ij the same behind a gate
that turned out to permit it. The gate was correct and the answer is still
wrong, which means the assumption underneath all three is the thing to
test, not the next variant of it.

What has never been read is where the **other** apertures are. 2ik reads
nine registers at the +12 s sample, offsets from `gc_10_3_0_offset.h`:

```
0x16FC FB_LOCATION_BASE   0x16FD FB_LOCATION_TOP
0x16FE AGP_TOP            0x16FF AGP_BOT          0x1700 AGP_BASE
0x1701 SYSTEM_APERTURE_LOW_ADDR    0x1702 SYSTEM_APERTURE_HIGH_ADDR
0x16E8/0x16E9 SYSTEM_APERTURE_DEFAULT_ADDR LSB/MSB
```

In Linux an MC address inside `[AGP_BOT, AGP_TOP]` is **system memory
reached without mc2pa**, and the system-aperture bounds say which MC
addresses the hub resolves to host DRAM directly. The roots are MC
`0xF4_FB00_0000`, `0xF4_FB00_B000`, `0xF4_FB01_5000`.

| `rbgpu-aper` reading | means |
|---|---|
| `agpb..agpt` covers the roots | they are SYSTEM memory, reached without `mc2pa`, and all three transforms tried were the wrong *family* of answer - one explanation for three boots |
| only `fbb..fbt` covers them | the FB aperture really is 4 GB over 2 GB of backing, and the question becomes what `vram_base_offset` should be on an APU |
| the roots are in neither | the MC addresses are not hub-resolvable at all and `ctx+0xb8` is not what 2ig took it for |

**This boot writes nothing.** The bind-time write is held back
(`unres=` counts it), the register is left exactly as Apple programmed it,
and the baseline that 2ih and 2ii perturbed is restored. Guessing a third
transform before reading the map would be the same error a fourth time.

