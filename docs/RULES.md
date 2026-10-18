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
  nothing. A capture is the armed boot's only if its `rbgpu-diag` reads
  `armed=1` **live**, not in `-prev`. [2gw]
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
```

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
- The 2am/2an/2ao blanket firmware-ready bypasses still owe the explanation
  2cy demanded.
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

**Distrust `GRBM_STATUS`.** It reads `a0003028` in all five samples
across two boots, including where `CP_STAT = 0` and the CP is un-halted
yet it still claims `CP_BUSY` + `GUI_ACTIVE`. Either it is not read live
through the accessor or it means something else. Nothing in 2hp rests on
it and 2hl's use of it is weaker than it looked. Owed.

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

**Next, and it is cheap:** raise the four-fault NVRAM cap and see
whether a `status 0x40953` fault EVER carries a nonzero VA. If none
does across many faults, VA 0 is real and the timestamp hypothesis gets
its test. Owed at the same time: the `vmfault_hooked` / `vmfaults_seen`
/ `vmfault_calls` / `vmfault_reports` counters were **truncated out of
`rbgpu-diag`** (it caps at `mapq_ca`), so this boot cannot say how many
faults there were or whether `fillFaultAccelInfo` ran at all — missing
looks the same as zero (2eh, 2ek, 2fb). The counts belong in the
`rbgpu-fault` note itself.
