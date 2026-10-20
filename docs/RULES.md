# RembrandtGPU — notes for the assistant

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
- **Grep the mangled symbol across the whole plugin before adding a route.**
  A second `RouteRequest` for a function this plugin already routed panics
  the machine outright: Lilu's `readChain` finds the first route's 5-byte
  `E9` jmp in the prologue and refuses to build a second trampoline over it
  — `patcher: previous plugin had short jump type on a multiroute function,
  this is not allowed`. 2km added a `getPDEValue` hook that 2if had routed
  since boot 71 and boot 85 panicked at kext load, before any measurement.
  A new transform on an already-hooked function goes **inside the existing
  hook**, not beside it. [2kn]
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
- **Look in `logs/<timestamp>-plugin/` before reading live NVRAM.** The
  armed boot's capture may already be saved there (ssh in while the screen
  is black). `plugin_check.sh` only READS nvram, so a later run destroys
  nothing. [2gw]
- **In an armed boot, run `sudo tools/plugin_check.sh` over SSH BEFORE
  rebooting.** It saves the whole capture to `logs/<timestamp>-plugin/`
  on disk, which survives the power cycle; live NVRAM read after a
  reboot does not. Boots 57, 58 and 59 each wrote `rbgpu-prog` (early)
  and lost every ring-probe note (late) - the 2gq pattern - and three
  experiments were spent blaming the code for it. The user cannot run
  the assistant from an armed boot, so the capture has to be a file. [2jg]
- **Read `rbgpu-boot` FIRST, before anything else in NVRAM.** If it has not
  increased since the last capture, the boot was never armed and *nothing*
  in NVRAM is this boot's. 2gw's old test - "`rbgpu-diag` reads `armed=1`
  live" - **certifies a stale capture**: `rbgpu-diag` carries no `b=`
  nonce, so an unarmed boot leaves the previous armed boot's `armed=1`
  sitting there, live. Twenty minutes of 2ix's numbers were read as 2iy's.
  [2iz]
- **`rbgpu-arm` is one-shot** - the peer consumes it and deletes it. Every
  armed boot needs `sudo nvram rbgpu-arm=1` set again, and an install that
  only replaces the kext and edits boot-args does **not** arm. [2iz]
- **A return code is not a result until the code that sets it has been
  read.** A named constant tells you what the NAME means, not what the
  function meant by it. Request 0x1B's *success* path returns
  `kIOReturnUnsupported`; both its failure paths return
  `kIOReturnNotReady`. Two milestones were built on the opposite reading.
  [2ha, the 25th instance]
- A stale note is **no evidence about where a boot got to** — not weak
  evidence of an early stop. Never assemble several stale notes into a
  story; a user rebooting a black screen looks exactly like a hang. [2gw,
  the 24th instance]
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
- The hw engine array is `hw + 0x3b0 + engine*8`: `[0]` PM4/GFX, `[1..4]`
  the four SDMA engines. The SDMA *walk* bases at `hw + 0x3b8`, so **2gi's
  `engine[i]` is hw index `i + 1`** — 2gi keeps hw[1] and empties hw[2..4].
  [2gu, 2gv]

## Boot-args — what must stay in and out

```
-rbgpusync570 -rbgpuspoofttl -rbgpupsphw     the TTL/PSP path
-rbgpusmubringup -rbgpuptbase                the SMU link and the GART fix
-rbgpustampwait                              the bounded (2en) real wait
-rbgpunogates                                the configuration that works
-rbgpusdmaone                                what makes powerUpHW succeed
-rbgpuforce1b                                sends request 0x1B
-rbgpurlcsub                                 what makes the RLC autoload COMPLETE
-rbgpunocgcg                                 CGCG off - takes, but NOT a fix (2jv)
-rbgpumewatch                                the timed CP/RLC/GFXOFF sample banks
```

`-rbgpunocgcg` is IN but is **NOT a fix** (2jv). It writes
`RLC_CGCG_CGLS_CTRL = 0` and `RLC_PG_CNTL = 0`; the write **takes**
(`refused=0`, `0x363f -> 0`, held to 11535 ms), so RLC registers are
host-writable unlike `CP_ME_CNTL`. It stops the RLC's context-idle
handler running at all (`RLC_GPM_STAT` `0x140016` at both samples) and
**the CP still halts at 8835 ms**. Kept IN only to hold boot 68's
configuration constant. CGCG as the re-halt mechanism is REFUTED.

**GFXOFF is live again, and 2ix's exclusion of it was unsound.**
`0x15000000` is `CE/PFP/ME_HALT` with every `*_PIPE*_RESET` bit **clear**
- the CP's power-on state, and what `gfx_v10_0_cp_gfx_enable(false)`
writes. No software here writes it (boot 65: all four writes wrote 0). A
powered-down GFX block presents its defaults and drops writes, which
accounts in one mechanism for the halt value, the refused writes, 2jl's
`CE_STEP`, `RLC_STAT = 0`, `srst = 0`, the deterministic ~8.8 s across
three boots, and 2hp's +160 s recovery. 2ix excluded GFXOFF on
`gfxoff_status = 2`, but that is read in the **late diag**, after the
+160 s recovery, never at 8.8 s - the recurring error, 26th instance.
2jv samples it in the 50 ms window. [2jv]

`-rbgpurlcsub` is IN and proven (2jq): it supplies the five RLC
sub-images 2aj never did - `srlist_cntl`/`gpm`/`srm` and the **LX6
iram/dram**, ids 26/27/28/29/30, carved out of `yellow_carp_rlc.bin`.
Without it the RLC boots Navi 21's LX6 firmware and
`BOOTLOAD_COMPLETE` never sets; with it the latch sets and the CP is
un-halted. Never remove it without expecting `complete=0` back.

`-rbgpuforceenable` + `wdt=-1` are the current experiment pair, both IN
and both proven (2hh, 2hj).

`-rbgpuforceenable` calls `IOGraphicsAccelerator2::enableAccelerator()`
under `accel->0x88` after 0x1B returns; `accel+0xc78` goes `0 -> 2` and
**the machine gets an accelerated display**: lock screen drawn, password
accepted, and WindowServer compositing through Metal on
`AMDRadeonX6000MTLDriver` (2hj proves the last part from the stack, not
from the screen). The boot then freezes on the 2hj hang.

`wdt=-1` disables Apple's userspace watchdog — read, not assumed:
`IOWatchdog::start` tests bit 0 of `wdt`, logs "watchdog disabled by
boot-arg" and zeroes `+0x89/+0xa8/+0xac`. It changes nothing on the chip.
Without it the frozen boot **panics at 120 s** with no usable SSH window
(2hh); with it the box freezes and stays reachable, which is what made
2hj measurable at all. **Never arm `-rbgpuforceenable` without `wdt=-1`.**

`-rbgpuvmpteng1` is now IN and proven (2gw): it sends the engine-2
`getHWChannel` requests to engine 1, the one SDMA instance this chip has.
It is what broke the GPU-restart loop.

**Must stay OUT**, each proven:

- `-rbgpunodisppanic` — publishes `IOPCITunnelled` and costs the UI. [2cv]
- `-rbgpusdmahook` — routes a function whose prologue breaks rule 9. [2gb]
- `-rbgpustampraw` — Apple's unbounded wait; it wedged the machine. [2en]
- `-rbgpuregdump` — the GC register dump stalls non-deterministically. [2fe]
- `debug=0x100` — halts on the panic screen instead of writing a report.

## Standing debts

- The peer's `/options` lookup is tried **once**
  (`IORegistryEntry::fromPath`). On a boot where the peer runs early
  (0.52 s vs the usual 1.9 s) it loses the race, logs `/options not
  found`, and silently does not arm — indistinguishable from the user
  forgetting. `rbgpu-arm` stays set, so a plain reboot recovers with no
  reinstall. A bounded retry is owed. [2gy]

- `gfx_min = gfx_max = 0` since **2di** (2026-10-12): the SMU reports no GFX
  clock range, GFX DPM never started. Still unexplained. **Its promotion to
  prime suspect after 2hh is WITHDRAWN** — 2hl measured the CP clocked and
  executing (rptr advanced to 0x2b1, `GUI_ACTIVE`, six front-end blocks
  busy), which a switched-off GFX engine cannot do. 2di is not the
  mechanism of this hang. Whether it matters for anything else is still
  open.
- **2am is now a MISDIAGNOSIS, and it is the root cause (2ix).** 2it
  verified on this chip what 2am assumed: the CP microcode IS present
  (pfp/me/ce/mec each read 64/64 nonzero dwords of instruction RAM), so
  that half of the debt is PAID. But 2am's *reasoning* - "the PSP places
  the firmware, so nothing sets BOOTLOAD_COMPLETE and the wait times out
  spuriously" - is wrong for this part. Linux's `gfx_v10_0_rlc_resume`
  waits on that latch and **fails bring-up without it** whenever
  `load_type == PSP && psp.autoload_supported`, which is Rembrandt's path:
  on a PSP+autoload part the autoload is *precisely* what sets the latch.
  The timeout was a true negative and we patched it away. `rbgpu-bl` reads
  `iram=1 complete=0` in every boot. **2jh's refutation is NARROWER than
  2jh claimed (2jo):** stopping the RLC outright - `RLC_CNTL` 1 -> 0,
  confirmed, clean restore - does not release `CP_ME_CNTL`, which rules
  out a *running* RLC actively holding it. It does **not** rule out a
  latch set at reset and cleared only by the autoload completing, which
  an RLC stop cannot clear either. **So 2ix's conclusion stands - the
  halt is released by the autoload and by nothing the host can do - and
  only its mechanism was wrong.** 2jl fits it exactly: `CE_STEP`, an
  ordinary R/W debug bit, will not stick, so the register is read-only
  to the host rather than being fought over.
  Also refuted separately (2je): Apple's 10_3 path has **no backdoor
  fallback** - told the latch is clear, bring-up stops before the SMU
  rather than taking another route, so the 2am patch is load-bearing.
- **PAID (2jq, 2026-10-19): `BOOTLOAD_COMPLETE = 1`.** `rbgpu-bl` reads
  `bl=c0000001 iram=1 complete=1` in both phases of boot 63, after
  twelve boots of `complete=0`, and `diag_cp_me_cntl` / `diag_cp_mec_cntl`
  both read **0** where they read `15000000` / `50000000`. The cause was
  **2jp**: Apple's `_gc_10_3_get_fw_constants` asks for 21 firmware
  entries and 2aj supplied 7, so the RLC had been booting Navi 21's
  `rlc_lx6_iram`/`rlc_lx6_dram` - the firmware for the processor inside
  the RLC that runs the autoload - plus Navi 21's three save/restore
  lists. yellow_carp's own are in `yellow_carp_rlc.bin` (v2.2, ip 10.3)
  as sub-images; `-rbgpurlcsub` supplies them as ids 26/27/28/29/30.
  **2ix's conclusion is confirmed and the debt below is closed:** the
  halt is cleared by the autoload and by nothing the host can do.
  **New frontier: something re-halts the CP after bring-up** - `me` back
  to `15000000` by +12 s and +100 s with `h=0`, so not Apple's 10_3 halt
  path, and the resets (`reset=6 @15241ms`) come *after* it. Boot 64
  confirms the latch holds at all three phases. **`p2`'s `me=0 cps=0
  r==w==0x180` is the KNOWN post-reset state from 2hp, not a fix.**
  (2jq's "`ttl_hw_init_ret` now fails" is **WITHDRAWN** by 2jr: it was
  already nonzero in boot 62, before `-rbgpurlcsub`, and varies per boot
  like a handle. The `0` it was compared against came from a stale
  boot-55 diag - the 2iz trap.)
- **(CLOSED by 2jq) `CP_ME_CNTL` is READ-ONLY to the host, and the autoload is the only
  thing that clears it.** 2jl: `CE_STEP` (bit 25), an ordinary R/W debug
  bit, **will not stick**, and eight tight-loop reads after writing 0
  never showed one changed bit. So "the CP re-halts itself" is dead too.
  Excluded with positive controls: power gating (`pwr=1 clk=1 pipe=1`),
  soft reset (`srst=0`), RLC safe mode (2iv), a running RLC (2jh),
  GRBM misrouting (2jn: `cntl=0`, `idx=0`, SE0 exists), **GFXOFF**
  (`gfxoff_status=2` is Linux's "NOT in GFXOFF",
  `yellow_carp_ppt.c:627`), **RLCG** (SRIOV-only: `amdgpu_rlc_wreg` is a
  plain `amdgpu_device_wreg`), and **Apple's write path** (0xbe8d28a
  disassembled: direct store below the window, `MM_INDEX`/`MM_DATA`
  above, no filter - the writes reach the register). [2jl, 2jn, 2jo]
- 2an and 2ao still owe the explanation 2cy demanded. 2an is *not*
  vindicated: 2iv found `RLC_SAFE_MODE` stuck with CMD set and Linux's own
  exit sequence unable to clear it across 1000 polls.
- 2ct refuses the display pipe **by design**. 2hf traced the Metal hang to
  the `fEnabled` gate that the refusal leaves unset; **2hh closes it**:
  that same unset gate is why every armed boot was black, because
  WindowServer's own first `IOAccelResourceCreate` slept on it and
  WindowServer therefore never drew. Setting the bit directly
  (`-rbgpuforceenable`) produced a lock screen and an accepted login. So
  the black panel was never a DCN or display fault and is no longer a
  separate problem. 2ct itself is unchanged and still refuses the pipe.
  [2hf, 2hh]
- 2gh: "the second engine to ask" and "a duplicate create on instance 0" are
  the same event on this chip and nothing separates them.

## Where the work stands

`docs/HANDOFF.md` is authoritative and current. In one paragraph: Apple's
Navi 21 stack runs on the 680M with the chip's own yellow_carp firmware;
PSP, GC, TTL and the SMU all initialise; GPUVM translates since the 2em
PT_BASE fix; the KIQ executes PM4 and writes fences; **`powerUpHW`
succeeds** (2gi, eight consecutive boots). The frontier is request 0x1B
spinning in a GPU-restart loop over **one** outstanding stamp — slot 25,
the only stamp the driver has ever issued. That stamp was slot 25, the
**"VMPT"** channel, holding a real `AMDSDMAChannel` on hw engine **2** — an
engine `-rbgpusdmaone` had emptied of queues (2gt, 2gu).
**`-rbgpuvmpteng1` fixed it** (2gw): engine-2 channel requests go to engine
1, the one SDMA instance this chip has. `waitForStamp` now runs **once** and
returns **Success**, restarts are **0**, `finishAllStamps` and
`enableInterrupts` both return, and the accelerator reaches framebuffer
adoption. **Request 0x1B then COMPLETES** (2ha): its `0xE00002C7` is the value its
*success* path sets at 0xbe418cf, after `isDeviceValid`,
`startGlobalPerfCounters`, `hardwareDidStart`, `finishAllStamps` and
`enableInterrupts` have all run — both failure paths return
`0xE00002D8` instead. The live 2gn trail and the disassembly agree call
for call. The 0x12/0x1D refusals are incidental: Apple runs the whole
path despite them. Nothing in the 0x1B path is now known to fail.

**Metal enumerates the GPU** (2hd): in an armed boot
`MTLCopyAllDevices` returns **1 device** — "AMD Radeon HD GFX10 Family
Unknown Prototype", 1024 threads/TG, 4 GB working set — against **0
devices** unarmed. The frontier is now **`newCommandQueue`**, the first
operation a Metal client asks for: the probe never returns from it, with
no panic. **The hang is process-local** (2he): SSH stays up, so the blocked thread
was sampled — and **2hf identified the cause outright**. The stacks give
`newCommandQueue` → the blit manager's initial shaders → the first buffer
allocation → `IOAccelResourceCreate` → in the kernel,
`IOAccelSharedUserClient2::new_resource` →
`IOGraphicsAccelerator2::acceleratorWaitEnabled` → asleep in
`waiting_for_fEnabled`. `fEnabled` is **bit 0x2 at accel+0xc78**; its only
setter is `IOGraphicsAccelerator2::enableAccelerator()`, whose only two
callers are `IOAccelDisplayMachine::display_mode_did_change` and the
Legacy variant. **2ct refuses the display pipe, so no mode change ever
happens, so the accelerator is never enabled, so every resource creation
blocks forever.** 2hd's GFX-ring guess is withdrawn at this point in the
story — that thread never reached hardware. **2hh then set the bit and
the gate opened** (see below); only after it opens does a client reach
the GFX ring, where the guess turns out to have been aimed at the right
engine for the wrong reason.

**An armed boot reached the login screen** (2hh, 2026-10-18). Until then
no armed boot had shown a panel since roughly 2c — verbose log lines and
then nothing, all work over SSH (user, 2026-10-17). The cause was the
`fEnabled` gate, not the panel: `-rbgpuforceenable` set it and the lock
screen drew, the password was accepted, session load began. **Then the
machine froze and the userspace watchdog panicked it at 120 s.** So a
black armed boot is still the norm without that boot-arg, and is still
never to be called new, intermittent or a warning sign.

**Metal dispatches for real** (2hj, with `wdt=-1` so the boot survives).
`newCommandQueue` **succeeds** — the 2hd frontier and the 2hf cause are
both gone. The probe creates a queue, compiles a shader at runtime,
encodes 4096 threads and commits; it then hangs in `waitUntilCompleted`.
And WindowServer composites through Metal on `AMDRadeonX6000MTLDriver`
(`CaptureSurfaceMetal::Populate` -> `GFXAAMD_MtlDevice
newTextureWithDescriptor:iosurface:`), which is what the user saw as a
grey translucent password field instead of a black one.

**The frontier is one stuck kernel thread** (2hj). A thread named
`AMDRadeonX6000_AMDAccelEventMachine` sits in
`hardwareErrorEvent` -> `restart_channel` -> `IOAccelFIFOChannel2::restart`
-> `AMDAccelChannel::resetHardwareAndReplay` -> `waitForTimestamp` ->
`IAMDHWInterface::pollWait`, polling for a timestamp that never arrives.
WindowServer blocks on the mutex it holds (the spindump names the
turnstile); the Metal submit blocks behind the same restart, reached via
`waitForRingBufferSpace` — the ring is full. **Three hangs across two
boots, one cause: the GPU retires no timestamps on the client channel,
and the restart path cannot recover because it waits on the same
timestamp.** It is the restart loop of 2gq again, on the client ring —
untouched by `-rbgpusdmaone` and `-rbgpuvmpteng1`, both of which act on
SDMA. Stacks: `logs/20261018-010521-metal/` (2hj, live) and
`logs/20261018-004318-WSHANG-ARMED-2hg/` (2hh, from the panic).

The reproducer is now a 4096-thread compute kernel with no compositor,
no surface and no display in the path.

**The CP is clocked and stalled mid-stream** (2hl, the 2hk ring probe).
Two samples sixty seconds apart, byte for byte identical:
`rptr=000002b1 wptr=00000380 cp_stat=90038600 grbm=a0003028`. So the CP
**consumed 689 dwords of the GFX ring and stopped with 207 left**.
`GRBM_STATUS` says `CP_BUSY` + `GUI_ACTIVE` with `DB_CLEAN` and
`CB_CLEAN`; `CP_STAT` says `PFP_BUSY`, `ME_BUSY`, `MEQ_BUSY` and all
three `ROQ_*_BUSY`. The command processor's front end is holding fetched
packets it cannot retire, and **nothing reached the shader cores**.

Two consequences. **2di is out as the cause** (see Standing debts).
And RB0 was already stalled **before** any Metal client ran — the
`+100 s` sample predates the dispatch and matches the `+160 s` one — so
the wedged work is the driver's own, and Metal's compute goes to an
MQD user queue, not RB0. 2hj's "the ring is full" is a consequence two
levels removed, not the client ring filling.

**A GPUVM page-table walk is failing, and the CP is halted** (2hn).
`GCVM_L2_PROTECTION_FAULT_STATUS = 0x40953`: `WALKER_ERROR`,
`MAPPING_ERROR`, `MORE_FAULTS`, `PERMISSION_FAULTS=5`, `CID=4`, `RW=1`
(a write). Fault reporting is fully enabled (`CNTL = 0x1ffc`), so this
is a chip reporting a fault, not one configured to ignore them. And
`CP_ME_CNTL = 0x15000000` — `CE_HALT`, `PFP_HALT`, `ME_HALT` all set:
the command processor is **halted**, which is why `rptr` is frozen with
the front-end queues still full. The halt is almost certainly
`resetHardwareAndReplay`'s own doing (2hj's stuck thread), not the
chip's. `CP_RB0_BASE = 0xffbfde00` puts the GFX ring at VA
`0xFF_BFDE_0000`, inside Apple's GPUVM space — reached through the
translation that is faulting.

**2hp settles when, and names the client.** The two fault reads return
**different** values, so the register is being cleared and re-armed —
Linux's `gmc_v10_0_process_interrupt` does exactly that
(`WREG32_P(vm_l2_pro_fault_cntl, 1, ~1)`) after each fault. So Apple is
taking VM fault interrupts and **the faults are ongoing**, not one
historical latch. 2hn's caveat is retired.

**`CID = 4` is `CPF`** (`gfxhub_client_ids[]`, gfxhub_v2_1.c) — the
command processor's *fetcher*, the block that reads the command stream.
`CPF_BUSY` is set in every sample of every boot, including ones where
everything else went idle.

**The +12 s sample is already faulted**, with the CP already halted. So
this is not something a Metal client triggers: it is present before any
client, and every client hang is downstream. Two *different* faults were
caught — `VMID=3` on a read at +12 s, `VMID=0` on a write at +100 s —
so it is not one context or one access.

**The GPU recovers and the software does not.** At +160 s `me=0`
(CP un-halted), `cps=0` (CP idle), `r == w == 0x180` (ring empty,
pointers rewound): the reset completed and **threw the pending work
away**. The timestamp the waiting thread needs was part of that work, so
it can never arrive. The machine was still hung. The bug is no longer
"the GPU is wedged" — at +160 s the GPU is fine.

Revises 2hl: the ring is not simply frozen. `rptr` advanced
`0xb1 -> 0x2b1` between +12 s and +100 s. 2hl caught a stopped window.

**`GRBM_STATUS` - DEBT CLOSED (2jr).** 2hp said to distrust it: it read
`a0003028` in every sample of every boot, including ones with
`CP_STAT = 0` and the CP un-halted, so it was unclear whether it was
read live at all. **Boot 64 reads `3028` at +160 s** (and `g2`
`200008`), the first non-`a0003028` value this project has seen. It was
live all along; the chip had simply never reached a clean idle before
the RLC autoload started completing. 2hl's use of it is sound after all.

Owed also: `vmalo = 0x400300` at +12 s is a real latched value (the
later zeros are TLB invalidations, which set
`CLEAR_PROTECTION_FAULT_STATUS_ADDR`), but **Linux never converts this
register to an address** — it takes the address from the interrupt
vector. `0x400300 << 12` is the obvious reading and is unsourced. Do not
use it until it is.

**The client contexts' page-directory bases have no VALID bit** (2hr).
The GCVM context registers, read at +12 s and unchanged at +100 s:

```
c0  cn=1555481  PDE=0x7_8FDF_A001  window VA 0xFF_BFA0_0000..0xFF_FFE0_0000
c1  cn=3b       PDE=0x0_FB00B000   window VA 0x4_0000_0000 ..0x23_FFFF_F000
c3  cn=3b       PDE=0x0_FB015000   window VA 0x4_0000_0000 ..0x23_FFFF_F000
```

Two 2hq hypotheses are **dead**: context 3 is programmed and enabled, and
the faulting page (`0x400300` -> VA `0x4_0030_0000`) is *inside* its
window, so it is not a range fault. The GFX ring at `0xFF_BFDE_0000` is
inside c0's window too.

What is left is **bit 0**. The field is `PAGE_DIRECTORY_ENTRY_LO32`, a
PDE with flags. c0 (2em's patched value) ends in 1; **c1 and c3 end in
0**. Linux always ORs `AMDGPU_PTE_VALID` (bit 0) into this register
(`amdgpu_gmc_pd_addr`, Vega10+, every depth) — and so does Apple:
`AMDGFX10VMM::getPDEValue` (0xbe9b9de) ends `incq %rax`. **Every PDE
Apple builds has bit 0 set**, so these two registers are anomalous by
Apple's own convention. c0 is also `PAGE_TABLE_DEPTH=0` (flat, as 2em
found) while c1/c3 are depth 1 — and `WALKER_ERROR` is a walker
complaint.

**Not yet a cause.** Unexcluded: the hardware may ignore bit 0 here;
these values may not come from `getPDEValue` at all (the write path was
not located); the addresses may be wrong independently of the flag
(though `0xFB0..` is not shaped like the stripped carve-out offset 2em
fixed, so this is probably not a second 2em). Setting the bit is the
obvious experiment but contexts 1-15 are reprogrammed per VM bind, so a
one-shot timer write would be overwritten before it is tested.

**Apple's own fault records** (2ht, the 2hs hooks). Four, in order,
reproducing identically across four consecutive armed boots — this is
deterministic, not a race:

```
#1  hub 0  VA 0x4_0030_0000    vmid 3   READ   cid 4 (CPF)  st 0x3009bb
#2  hub 1  VA 0xFFFFFFFFF000   vmid 15  WRITE  cid 511      st 0xffffffff
#3  hub 0  VA 0                vmid 0   WRITE  cid 4 (CPF)  st 0x40953
#4  hub 1  VA 0xFFFFFFFFF000   vmid 15  WRITE  cid 511      st 0xffffffff
```

**#1 closes the `vmalo` debt.** 2hs derived `VA = (HI32 & 0xf) << 44 |
(LO32 << 12)` from Apple's disassembly *before* the boot; Apple's own
code produced the identical value. CPF reads VA `0x4_0030_0000`, 3 MB
into context 3's window — the context whose PDE has no VALID bit (2hr).

**#2/#4 are phantoms, and that is a new defect.** `status = 0xffffffff`
is a dead-register read; `vmid 15`, `cid 511`, `VA 0xFFFFFFFFF000` are
just the all-ones bits decoded. **MMHUB's fault status register reads
all-ones on this chip**, and since `getVMFaultInfo` fills its struct
whenever `(addr != 0 || status != 0)`, Apple reports a phantom hub-1
fault on every poll and feeds it to the real fault path. Never treat an
Apple hub-1 fault report on this chip as real. Whether that phantom
stream provokes resets is open.

**#3 is the lead: a CPF WRITE to VA 0**, vmid 0, the same `0x40953` the
samples caught at +100 s. An uninitialised pointer shape — and the
candidate that would explain everything is the **end-of-pipe timestamp
writeback address**: told to write the completion timestamp to VA 0, the
write faults, the timestamp never lands, `waitForTimestamp` polls
forever. **Hypothesis, not established** — neither that CPF's write is
the timestamp write, nor that VA 0 is real rather than an artifact of
`CLEAR_PROTECTION_FAULT_STATUS_ADDR` zeroing the address register before
anyone read it (2hp). Apple reads the same registers, so Apple's zero is
equally susceptible.

**2hv ran that test and it came back short.** Fifth consecutive armed
boot, everything else identical:

```
hk=1 c=8 f=8 r=4 o=0
  s3009bb   h0 n=1 va=400300000    mx=400300000    v3 R  c4
  sffffffff h1 n=4 va=fffffffff000 mx=fffffffff000 v15W  c511
  s40953    h0 n=3 va=0            mx=0            v0 W  c4
```

Settled: the hooks took (`hk=1`) and the counters now survive in the
note. **Apple's fault reporting path RUNS** — `r=4`,
`fillFaultAccelInfo` assembled four complete reports and kprintf'd them
where nothing can read them. **Every poll found a fault** (`c=8 f=8`);
Apple never once saw the chip clean. Half of what it processes is the
hub-1 phantom.

**Not settled: whether VA 0 is real.** `s40953` was seen three times,
VA 0 every time — but 2hu set the bar at *hundreds*, and three is not
it. Calling `mx=0` confirmation would be the recurring error. It is
consistent with BOTH live hypotheses (genuine null target, or
`CLEAR_PROTECTION_FAULT_STATUS_ADDR` wiping the register first), and
the sample size **cannot be raised this way** — Apple polls
`getVMFaultInfo` only eight times in two and a half minutes. The
end-of-pipe-timestamp hypothesis is neither supported nor refuted.

**2hx did that, and the hypothesis is REFUTED.** The ring maps and the
dump is unambiguous:

```
rbgpu-pm4 = pa=6c8911000 t3=799 r=2b1 w=380
            | rm@1e a=ffbfdc0000 s=3 | rm@8e a=ffbfdc0000 s=4
            | rm@10e a=ffbfdc0000 s=5
```

`t3=799` type-3 headers in 1024 dwords — nothing but a command ring
looks like that, so the route (ring VA -> CONTEXT0 flat PTE table ->
PTE -> physical page `0x6_C891_1000`) is sound and the bus-vs-physical
worry is answered. **First GPU-visible memory this project has read.**

**The end-of-pipe timestamp address is `0xFF_BFDC_0000`, not zero.**
Three `RELEASE_MEM` packets, seq 3/4/5, all aimed there. 2ht's
timestamp-target hypothesis is dropped on direct evidence. By
elimination the `VA 0` of `status 0x40953` was the
`CLEAR_PROTECTION_FAULT_STATUS_ADDR` artifact after all, not a null
target — inference from the refutation, not an independent measurement
of the clear, but nothing now supports a genuine null.

`0xFF_BFDC_0000` is inside CONTEXT0's window and 128 KB below the ring;
its flat-table page index is **960** (the ring's is 992). All three
fence packets sit **before** `rptr = 0x2b1`, so the CP fetched past all
of them and the driver still waits.

**2hz: the fence writes LAND, and neither possibility was right.**

```
rbgpu-fence = fv=ffbfdc0000 idx=3c0 pte=30006c835d077 v=1 s=1
              pa=6c835d000 rpa=6c7c05000 seq=8 o=0
              d: 6 0 280 0 380 0 2 1 0 0 0 0
```

`PTE[960] = 0x3_0006_C835_D077` — **VALID, SYSTEM, SNOOPED, READABLE,
WRITEABLE**. The fence page is mapped and writeable, so "the writes
faulted" is dead. And the page holds **6** at `+0x00`, inside the exact
range of sequence numbers the ring's RELEASE_MEM packets carry (3, 4,
5 … up to `seq=8`). **The GPU's timestamp writes land. The fence
mechanism works end to end.**

But only *up to 6*: fences 7 and 8 were submitted and never completed.
So it is not a broken fence path and not a driver reading the wrong
address — **the GPU executes the stream, signals six fences correctly,
then stops partway**, which is 2hl's frozen `rptr` seen from the other
side. (`+0x08`/`+0x10` hold `0x280`/`0x380`, the ring pointer shadows —
`0x380` is the live `wptr`. Noted, not interpreted.)

**So context 3 is back at the centre, on much firmer ground.** The
fence machinery is exonerated; the `0x40953` VMID-0 write now looks
like a consequence of the reset path; the hub-1 pair are phantoms. The
one unexplained fault left is **2ht #1: a CPF READ at VA
`0x4_0030_0000` under VMID 3** — whose window covers that address and
whose page-directory base is the one missing the `VALID` bit that both
Linux and Apple's own `getPDEValue` set unconditionally (2hr).

Still not proven, and 2hr's three caveats stand unchanged: the hardware
may ignore bit 0 there, the values may not come from `getPDEValue`, and
the addresses may be wrong independently of the flag. What changed is
that **nothing else is competing with it any more**.

**THE HALT IS A CONSEQUENCE (2kh, 2026-10-20).** Boot 81's halt-sample
sweep closes the question the whole 2j* series was chasing.
`CP_IB1_BASE = 0x4_0030_0000` and `GCVM_L2_PROTECTION_FAULT_STATUS` /
`_ADDR_LO32` give VA `0x4_0030_0000` **in the same sample** - the
faulting address IS the CP's IB1 base. `CID = 4` = CPF (the command
processor's *fetcher*), READ, VMID 3. All three instruction pointers read
identically at the halt and 50 ms later: **PARKED**, not looping.
`CP_ME_HEADER_DUMP = 0xc0033700` = type 3, 5 dwords, opcode `0x37`
`WRITE_DATA`.

So nothing "writes the halt". **The CP points IB1 at a VA it cannot
translate, the fetch faults at CPF, and the engines park** -
`CP_ME_CNTL = 0x15000000` is the result. Every elimination in the 2j*
series was hunting an agent that does not exist; the measurements stand,
the framing was wrong.

`int=0` is NOT evidence of no error: `CP_INT_CNTL_RING0 = 0x3c0000`
enables only `CMP_BUSY`, `CNTX_BUSY`, `CNTX_EMPTY` and `GFX_IDLE` -
`PRIV_INSTR`, `PRIV_REG`, `OPCODE_ERROR` and `RESERVED_BIT_ERROR` are all
**disabled**.

**The fix is therefore not a CP register.** VMID 3's page-directory base
has to be correct **before the first IB fetch under it**, which is earlier
than VM bind time. Either correct it where Apple programs the context, or
find why a fetch happens before any bind. [2kh]

Also settled by 2kh: the ring's own `INDIRECT_BUFFER` is **VMID 0**
(control dword `0x800003b8`), so 2kc's carve-out `mc2pa` was correct, and
the chain walk reached the declared size exactly (`chain=14 walked=952`),
making `noME-in-IB` a real negative **for that buffer**. But
`CP_IB1_BASE` names a different address in a different space, so the IB
that killed the CP was never in the ring page being scanned.

Open, flagged, not interpreted: `rbgpu-vmid` reads `av=41 fail=41` -
every `assignVMID` call reports failure while `vmids=000e` says 1, 2 and
3 were handed out. `fail` is the hook's own criterion and has not been
read against the code that sets it (2ha).

**THE FRONTIER MOVED (2ix, 2026-10-18).** The GPUVM line is closed and
the CP line replaced it. Two results, in order:

1. **The page-directory base defect is real and the correction works, but
   "the fault is gone" is WITHDRAWN (2kh).** Apple's value for contexts 1-15
   omits `vram_base_offset`, so the register points at unmapped memory
   (`0xFB01_5000` reads all-ones; `mc2pa` of it, `0x8_7B01_5000`, holds a
   genuine depth-1 directory with one valid PDE carrying Apple's own L0
   template). Writing the corrected value at bind time does take.
   **But it lands too late.** Boot 81 reads `vmf=3009bb va=400300` at the
   halt sample with `-rbgpuptfix` in, and `rbgpu-vm` shows `c3:b=fb015000`
   - Apple's uncorrected value - at p=0 (+12 s) and `c3:b=87b015001` at
   p=1. The halt is at **8.88 s, before both**. The claim "it stays gone"
   survived four milestones only because every sample was taken at +12 s or
   later, and +12 s is already after the CP has fetched. The
   fence/ring/GPUVM machinery is still sound. [2kh]
2. **It changed nothing about the hang, and 2ix found why.**
   `CP_ME_CNTL = 0x15000000` (CE/PFP/ME halted) and **every write to it is
   refused** - from `g_hw`, from Apple's accessor, and from Apple's own
   driver code, which writes 0 three times a boot. Eliminated at the
   moment of the halt: GFX power-gating (`pwr=1 clk=1 pipe=1`), soft reset
   (`srst=0`), RLCG (statically - gfx10 has an empty RLCG range), RLC safe
   mode (forced to 0, write still refused), and the instrument itself
   (`SCRATCH_REG0` took a write in the same function). What remains is
   `BOOTLOAD_COMPLETE = 0`: **the RLC never finished the bootload
   handshake and so never handed the CP to the host.**

**Next:** establish whether Apple's 10_3 autoload path is wired. HWLibs
ships `_gc_10_3_rlc_toc` and `_gc_parse_rlc_toc_10_3`, but
`_gc_fw_backdoor_autoload_config_10_1` / `_copy_` / `_validate_` exist
only as `_10_1`, and the PSP bootloader path is `_11_0` (Navi21). Zero
direct callers proves nothing - HWLibs dispatches through
`*_init_function_pointer_list`, so the tables must be walked.

**Older, now closed:** find what writes
`GCVM_CONTEXT3_PAGE_TABLE_BASE_ADDR`. 2hr
could not locate the write path; that is now the question worth a boot.
Options: hook `getPDEValue` to see what Apple computes versus what
lands in the register, or find the register write statically. Setting
the bit by hand remains the obvious experiment and remains awkward —
contexts 1-15 are reprogrammed per VM bind, so a one-shot timer write
would be overwritten before it could be tested; it needs a hook on the
write, not a timer.
