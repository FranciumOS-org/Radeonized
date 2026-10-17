# RembrandtGPU — notes for Claude

> **New session: read `docs/HANDOFF.md` FIRST.** It is the current *state* —
> what is settled, what is open, the pending experiment and the procedure.
> `docs/JOURNAL.md` is the chronological evidence journal (~8500 lines):
> authoritative for what was measured, in order, including every withdrawn
> reading, and **not to be trimmed**. Grep it by milestone tag (`2em`, `2gi`)
> when you need the history of one step; do not read it whole.
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


## Rules that cost boots to learn

Each of these was paid for on hardware. `docs/JOURNAL.md` has the evidence
under the tag in brackets.

**Routing Apple's code**

- Never route a name by a kext's id unless `nm -m build/re/<kext>` shows it
  **defined** in that kext. `solveSymbol` does not skip imports; it returns
  the bare slide and `routeMultiple` writes a jump there. [milestone 2c]
- Before routing a symbol, disassemble its first 16 bytes and **refuse if a
  rel8/rel32 branch begins within the first 5**. Lilu copies the displaced
  instructions into a trampoline byte-for-byte, so a short branch keeps its
  original displacement and lands in Lilu's own buffer. Two identical page
  faults at two KASLR slides. [2gb, "rule 9"]
- vtable dumps: **call offset = dump offset − 0x10** (the address point is
  symbol + 0x10). A *non-virtual* call through a vtable **symbol** uses dump
  offsets directly. [2gp]

**Reading a boot**

- Before interpreting anything, check: the loaded UUID against `build/out`,
  whether `rbgpu-arm` was left set from an earlier attempt, and which
  boot-args are actually in config.plist. [2cu, 2cv]
- A note is evidence **only when its `b=` nonce is new**. An overwrite
  variable read after a power cycle is this boot's value only if something
  establishes it was rewritten this boot. [2gd, and 2gc which cost a boot]
- Run `tools/plugin_check.sh` **in the armed boot**. A note's later commits
  can be lost across a power cycle while an earlier one survives — the 2gp
  boot read `s=5` in-boot and `s=1` live, same nonce. [2gq]
- The published `RBGPULog` ends at the last *refresh*, not at the thread's
  stopping point, so its last line does not locate a hang. Nor does console
  output. [2es, 2em]
- Absence is not a result. A silent `return` inside an instrument, a full
  diag table, or a missing NVRAM write all look identical to "the code never
  got there". Every path out of a probe must say something. [2eh, 2ek, 2fb]

**Instrumenting**

- The instrument can be the fault. It has cost this project a wedge (the
  gate trail, 2fo), a crawl mistaken for a hang (logging contention, 2ez),
  and two panics (2fz/2ga, 2gg). When a measurement and a suspect are both
  new, remove the suspect rather than reasoning about it.
- Never substitute a value and then narrate it as a hardware result. [2cy,
  which cost fifteen milestones]
- Do not build a table from n=1 rows. [withdrawn 2ev table, 2fs]

**The recurring error, stated once**

Twenty-three recorded instances of reading a value without first
establishing what makes it meaningful. The practice that keeps catching it:
say in the code what would have to be true for the reading to mean
anything, and log it when it is not.

## Chip facts established on hardware

- GC seg0 = **0x1260** (agreed four independent ways: 2dl MMIO, 2fd, and two
  resolutions in 2fi). Apple's encoded register ids are
  `(dword offset << 32) | 11`, 11 = GC_HWID. Offsets from
  `docs/linux-ref/gc_10_3_0_offset.h`; byte address is `(seg + dword) * 4`.
- The register accessor's slots: `[0x148]` resolves an id to a dword offset,
  `[0x118]` READS it, `[0x120]` WRITES it. [2fd, 2fg]
- Carve-out MC `0xF4_0000_0000`, system-physical `0x7_8000_0000`
  (`FB_OFFSET` 0x780), BAR0 `0x8_8000_0000`. Apple's GPUVM space is
  `0xFF_BF..`.
- **One** SDMA instance; Apple's Navi 21 path builds four engines
  (`hw->0xc2 * 2 + 2`). [2gc, 2gi]

## Boot-args — what must stay in and out

```
-rbgpusync570 -rbgpuspoofttl -rbgpupsphw     the TTL/PSP path
-rbgpusmubringup -rbgpuptbase                the SMU link and the GART fix
-rbgpustampwait                              the bounded (2en) real wait
-rbgpunogates                                the configuration that works
-rbgpusdmaone                                what makes powerUpHW succeed
-rbgpuforce1b                                sends request 0x1B
```

**Must stay OUT**, each proven:

- `-rbgpunodisppanic` — publishes `IOPCITunnelled` and costs the UI. [2cv]
- `-rbgpusdmahook` — routes a function whose prologue breaks rule 9. [2gb]
- `-rbgpustampraw` — Apple's unbounded wait; it wedged the machine. [2en]
- `-rbgpuregdump` — the GC register dump stalls non-deterministically. [2fe]
- `debug=0x100` — halts on the panic screen instead of writing a report.

## Standing debts

- `gfx_min = gfx_max = 0` since **2di** (2026-10-12): the SMU reports no GFX
  clock range, GFX DPM never started. Never explained; not yet shown to
  matter.
- The 2am/2an/2ao blanket firmware-ready bypasses still owe the explanation
  2cy demanded.
- 2ct refuses the display pipe **by design**, so the desktop stays on
  IONDRVFramebuffer and no boot has ever run with the accelerator driving
  the panel.
- 2gh: "the second engine to ask" and "a duplicate create on instance 0" are
  the same event on this chip and nothing separates them.

## Where the work stands

`docs/HANDOFF.md` is authoritative and current. In one paragraph: Apple's
Navi 21 stack runs on the 680M with the chip's own yellow_carp firmware;
PSP, GC, TTL and the SMU all initialise; GPUVM translates since the 2em
PT_BASE fix; the KIQ executes PM4 and writes fences; **`powerUpHW`
succeeds** (2gi, eight consecutive boots). The frontier is request 0x1B
spinning in a GPU-restart loop over **one** outstanding stamp — slot 25, the
only stamp the driver has ever issued.
