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
  register list (`rb_regs.h`, every offset cites its Linux header) and the SMU
  queries (`rb_smu.c`).
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
  (functions that use a C string). Output
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
  Claude never runs efi.sh.
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
