# RembrandtGPU — notes for Claude

> **New session: read `docs/HANDOFF.md` FIRST.** It is the current *state* —
> what works, what is open, the frontier, the verified facts and the
> procedure. `docs/JOURNAL.md` is the chronological evidence journal
> (~17 000 lines): authoritative for what was measured, in order, including
> every withdrawn reading, and **not to be trimmed**. Grep it by milestone tag
> (`2ks`, `2lg`) for the history of one step; never read it whole.
> `docs/HANDOFF-FULL.md` is a stale dump from 2026-10-05 — ignore it.

Goal: graphics acceleration for the Radeon 680M (Rembrandt, `1002:1681` rev
0a, GC 10.3.3 / DCN 3.1.2 / PSP+SMU 13.0.x / SDMA 5.2.3) on macOS. Plan:
`docs/PLAN.md`. Machine: ASUS TUF A15 FA507NU (Ryzen 7 7735HS), macOS 15.8.1
(24H32), OpenCore. Boot-args include `-wegnoegpu`; csr-active-config 0xA03.

## Status in three lines

**An armed boot is a usable, accelerated Mac** — the CP halt is fixed
(`-rbgpuvramadj`, 2ks/2ku/2kw), zero VM faults, Metal compute bit-exact.
**The frontier is readable text, and it is NOT a layout problem** (2ln, and
confirmed from the register state in 2mw: boot 115 reports every client
resource LINEAR, `m0=5553`, with the text still scrambled). Every layout lever
moves the widget/chrome class and leaves text untouched.
**The CPU-upload line of work is FINISHED and did not reach the symptom** - do
not re-run it. 2mn found a real defect with a law that predicted 16/16
geometries, `tools/iosurftex.m` fixes it, and 2mw proved it cannot be the cause:
Claude issues **zero** `replaceRegion` calls and all 101 of its texture
descriptors are `MTLStorageModePrivate`, which cannot take a CPU upload.
What is still unmeasured, and where to look next: **MSAA resolve** (20 of those
101 are `type=4`), texture-to-texture blit, mipmapped sampling, sRGB.
See `docs/PROBLEM-TEXT.md`.
**The display half has never started** — Apple's controller matches Navi 21
PCI IDs, ours is `0x16811002`, and spoofing `device-id` kills the accelerator.

Details, evidence and next steps: `docs/HANDOFF.md`.

## Layout

- `src/common/` — freestanding C shared by the kext and host tools: the IP
  discovery parser (`rb_discovery.c`), the read-only probe (`rb_probe.c`), the
  register list (`rb_regs.h`, every offset cites its Linux header), the SMU
  queries (`rb_smu.c`, `rb_smu13.c`). Message groups (`RB_SMU13_ALLOW_*`)
  start at queries only; allowing any other group on hardware is the user's
  decision. Tested in `tools/hosttest/smu13_test.c` against a simulated SMU
  whose tables are Linux's own structs.
- `src/kext/` — `RembrandtProbe`, an IOService in its own IOMatchCategory
  beside the boot framebuffer driver. Runs the probe once in `start()`,
  publishes `RBProbe` properties, unmaps everything.
- `src/plugin/` — the Lilu plugin (`kern_start.cpp` holds the hooks and
  probes, `RembrandtGPUPeer.cpp` the IOService peer). Boots from the EFI.
- `tools/hosttest/` — the probe against a simulated chip + parser fuzzing,
  ASan/UBSan. `make kext` runs it first; never load a kext whose hosttest
  fails. Add a case whenever `src/common` changes, and check a new test fails
  when the code it guards is broken.
- `docs/linux-ref/` — unmodified Linux files the code follows
  (`UPSTREAM.md` records each commit; fetch from torvalds/linux).
- `tools/re/` — static analysis of Apple's AMD kexts: `kc_extract.py` (kext
  out of a kernel collection as a standalone Mach-O), `machoread.py` (chained
  pointers, strings, symbols), `dumptable.py`, `kc_stubs.py` (kernel calls
  through branch stubs → names; `annotate` for objdump output), `vtslots.py`
  (IOService-family vtable offsets → methods), `strxref.py` (functions using a
  C string), `switches.py` (jump tables: case → handler), `ptrsym.py` (names
  every `RBGPULog` pointer inside a loaded AMD kext; run by
  `plugin_check.sh`). Output to `build/re/`. Findings:
  `docs/APPLE-STACK.md`.

## Workflow

- Build: `make kext tools` (CommandLineTools only, no Xcode); `make plugin`
  for the Lilu plugin — **`make kext tools` does NOT rebuild it**. The kext
  step resolves every import with `kmutil libraries`.
- **Always check the UUID moved** after a build
  (`dwarfdump --uuid build/out/...`). Two boots were lost to a stale build.
- The probe's hardware rules: no writes except `MM_INDEX`/`MM_INDEX_HI`
  (enforced in `rb_probe.c`, asserted by the hosttest), never enable PCI
  decoding or bus mastering, never map BAR0's first 16 MB (the boot
  framebuffer's), read only inside the carve-out. Levels 2 (DCN) and 3 (+ GC)
  are opt-in: `RBProbeLevel` in `src/kext/Info.plist`, or `rbprobe_level=N`.
- The SMU queries' hardware rules (`rb_smu.c`; opt-in `make kext SMU=1`,
  plist `RBSMUQuery`, boot-arg `rbprobe_smu=1`): writes only to MP1
  C2PMSG_66/82/90 of an MP1 13.x, only allowlisted query messages
  (GetSmuVersion, GetDriverIfVersion, GetEnabledSmuFeatures), nothing sent
  unless the response register reads 1, stop at the first non-OK answer.
  Linux's protocol (`smu_msg_v1_send_msg`).
- Register gates follow Linux's **version grouping**, not the exact version:
  this chip reports MMHUB 2.4.0 (Linux: `mmhub_v2_3` offsets) and DMU 3.1.3
  (Linux: `DCN_VERSION_3_1`, `dcn_3_1_2` offsets). Check the grouping before
  adding a gate.
- Results land in `logs/<timestamp>/`.
  `tests/data/rembrandt-680m-tufa15.bin` is this machine's real IP discovery
  table (2026-10-03); the hosttest parses it and runs the full probe on it.

## Hard rules

- **Always ask the user before loading or unloading a kext.** A bad load can
  hang or panic the machine and ends the session.
- The user runs `sudo tools/load.sh`, `sudo tools/unload.sh`,
  `sudo tools/efi.sh …` and `tools/plugin_check.sh`.
  **Claude never runs these and never asks for the password.**
- **Never install into the EFI or `/Library/Extensions`.**
- Before any load, **commit** so a hang loses nothing. The user allows Claude
  to commit at any time without asking (2026-10-04); **pushing still needs
  asking**.
- **Deliberate diagnostic panics are off the table** (user's decision,
  2026-10-12).
- **Adding an SMU message group is the user's decision** — say what it changes
  on the chip.
- **NootedRed and NootRX are under the ChefKiss Open Knowledge License v1,
  whose §2 and §3 forbid AI reading, summarizing, analyzing or deriving output
  from the work. Do not read or analyze them.** Findings from those projects
  must come from the user in their own words.
- `efi.sh` saves `config.plist` to `logs/efi-backups/` first; it edits via
  `tools/efi_config.py` (tested by `tools/efi_config_test.py`).
  `efi.sh sysreport-on` + reboot + `efi.sh vbios` pulls the 680M VBIOS out of
  the ACPI VFCT table (`tools/vfct.py`) into `firmware/` (gitignored).

## Rules that cost boots to learn

Each was paid for on hardware. `docs/JOURNAL.md` has the evidence under the
tag in brackets; `docs/HANDOFF.md` §7 states them with their reasoning.

**Routing Apple's code**

1. Never route a name unless `nm -m build/re/<kext>` shows it **defined**
   there. `solveSymbol` does not skip imports — it returns the bare slide and
   `routeMultiple` writes a jump there. [2c]
2. Disassemble a symbol's first 16 bytes and **refuse if a rel8/rel32 branch
   begins within the first 5**. Lilu copies displaced instructions
   byte-for-byte into a trampoline, so a short branch keeps its original
   displacement and lands in Lilu's own buffer. [2gb]
3. **Grep the mangled symbol across the whole plugin before adding a route.**
   A second `RouteRequest` for an already-routed function **panics the machine
   outright**. A new transform goes **inside the existing hook**. [2kn]
4. **`RouteRequest::from` is NOT written back** by Lilu 1.7.3, although
   `kern_patcher.hpp` documents it as such. Call `solveSymbol` yourself. A doc
   comment is not a measurement. [2kp]
5. **Cross-check every hand-written mangled name against `nm`.** Three length
   prefixes were wrong in one change; each would have made `solveSymbol`
   return 0 and silently unscope the fix. [2kr]
6. **When a fix moves upstream, REMOVE the downstream patch it replaces, in
   the same change.** "Redundant" and "inert" are different things — 2ku and
   2ki composed and the halt came back. [2kv]
7. vtable dumps: **call offset = dump offset − 0x10**. A *non-virtual* call
   through a vtable **symbol** uses dump offsets directly. [2gp]

**Reading a boot**

8. **Read `rbgpu-boot` FIRST.** If it has not increased since the last
   capture, the boot was never armed and *nothing* in NVRAM is this boot's.
   `rbgpu-diag` carries no nonce, so "armed=1 reads live" certifies a **stale**
   capture. [2iz]
9. **`rbgpu-arm` is one-shot** — the peer consumes and deletes it. Every armed
   boot needs it set again; an install that only replaces the kext and edits
   boot-args does **not** arm. [2iz]
10. **Run `sudo tools/plugin_check.sh` in the armed boot, over SSH, before
    rebooting.** It saves the capture to `logs/<timestamp>-plugin/` on disk,
    which survives a power cycle; live NVRAM afterwards does not. Three
    experiments were spent blaming the code for this. **Look in
    `logs/<timestamp>-plugin/` before reading live NVRAM.** [2jg, 2gq, 2gw]
11. **A note is evidence only when its `b=` nonce is new.** [2gd]
12. **A stale note is no evidence about where a boot got to** — not weak
    evidence of an early stop. A user rebooting a black screen looks exactly
    like a hang. [2gw]
13. **A return code is not a result until the code that sets it has been
    read.** A named constant tells you what the NAME means, not what the
    function meant by it. Request 0x1B's *success* path returns
    `kIOReturnUnsupported`. [2ha]
14. **Absence is not a result.** A silent `return` inside an instrument, a
    full diag table and a missing NVRAM write all look identical to "the code
    never got there". Every path out of a probe must say something.
    [2eh, 2ek, 2fb]
15. Before interpreting anything, check the loaded UUID against `build/out`,
    whether `rbgpu-arm` was left set, and which boot-args are actually in
    `config.plist`. [2cu, 2cv]

**Instrumenting**

16. **The instrument is a suspect.** It has cost a wedge (2fo), a crawl
    mistaken for a hang (2ez) and two panics (2fz/2ga, 2gg). When a
    measurement and a suspect are both new, **remove the suspect** rather than
    reasoning about it.
17. **Never substitute a value and then narrate it as a hardware result.**
    [2cy, which cost fifteen milestones]
18. **Do not build a table from n=1 rows.** [2ev, 2fs]
19. **A NULL check is not a pointer check.** Validate derived pointers as
    kernel VAs (`>= 0xffffff8000000000`) before dereferencing. [2gh]
20. **A registration that reports success is not evidence that a patch
    applied.** A Lilu `BinaryModPatch` with `section == 0`
    (`SectionDisabled`) is skipped and its file never read, while
    `onProcLoad` still returns `NoError` - so the hook logged `ARMED` and
    patched nothing. Log the enabling field itself and refuse when it is
    disabled. [2lq]
21. **Never put `DYLD_INSERT_LIBRARIES` in a launchd environment, and test a
    recovery procedure in the situation it exists for.** `launchctl setenv` is
    system-wide, not per-app, and dyld **kills** any process that cannot load
    an inserted dylib - including every sandboxed app, which cannot read one
    from a home directory. Every newly launched app died; the machine needed a
    reboot. The documented undo, `launchctl unsetenv` over SSH, addresses a
    different domain than the Aqua session and did nothing - the working form
    is `sudo launchctl asuser $(id -u) launchctl unsetenv DYLD_INSERT_LIBRARIES`.
    Inject per process, where the blast radius is one process started on
    purpose. [2mq]
22. **A search is an instrument too.** A grep that returns nothing is not
    evidence of absence until it has been checked against a case known to
    be present. `objdump` separates mnemonic and operands with a **tab**, so
    a space-based pattern silently matched nothing and produced a confident
    wrong negative. [2lw]

**The recurring error, stated once**

Thirty recorded instances of reading a value without first establishing
what makes it meaningful. The practice that keeps catching it: **say in the
code what would have to be true for the reading to mean anything, and log it
when it is not.** The 2kx/2la/2lc trio adds a variant worth naming — **read
the evidence already in hand before designing the next experiment**: all three
tested a global layout parameter that the user's photograph had already
excluded.

## Chip facts established on hardware

- **GC seg0 = 0x1260** (agreed four independent ways), seg1 = 0xa000. Apple's
  encoded register ids are `(dword offset << 32) | 11`, 11 = GC_HWID. Offsets
  from `docs/linux-ref/gc_10_3_0_offset.h`; byte address is
  `(seg + dword) * 4`.
- The register accessor's slots on `pm4+0x30` / `hw->0x368`: `[0x148]`
  resolves an id to a dword offset, `[0x118]` **reads** it, `[0x120]`
  **writes** it. [2fd, 2fg]
- Carve-out MC `0xF4_0000_0000`, system-physical `0x7_8000_0000`
  (`FB_OFFSET` 0x780), BAR0 `0x8_8000_0000`. Apple's GPUVM space is
  `0xFF_BF..`. **`mc2pa(mc) = mc - vram_start + vram_base_offset`** — the one
  missing term that was the whole CP halt.
- **One** SDMA instance; Apple's Navi 21 path builds four engines
  (`hw->0xc2 * 2 + 2`). [2gc, 2gi]
- The hw engine array is `hw + 0x3b0 + engine*8`: `[0]` PM4/GFX, `[1..4]` the
  four SDMA engines. The SDMA *walk* bases at `hw + 0x3b8`, so `engine[i]` is
  hw index `i + 1`. [2gu, 2gv]
- `GB_ADDR_CONFIG` is dword `0x13de`, BASE_IDX 0; this chip reads **0x242**
  (4 pipes, 4 packers, 1 SE). Fields per `gc_10_3_0_sh_mask.h`.
- `AddrSwizzleMode`: 0 = LINEAR, 9 = 64KB_S, 22 = 4KB_D_X, 25 = 64KB_S_X,
  27 = 64KB_R_X.
- **`getPDEValue` returns flags only** — the address is ORed in by its caller.
  Never hook it to correct an address. [2ko]

## Boot-args

The authoritative list, with the reason for each, is **`docs/HANDOFF.md` §9**.
The two that bite hardest:

- **Never arm `-rbgpuforceenable` without `wdt=-1`.** Without it the frozen
  boot panics at 120 s with no usable SSH window. [2hh]
- **Must stay OUT:** `-rbgpufbmatch` (spoofs `device-id`, **kills the
  accelerator** — this plugin works by making Apple *accept* `0x1681`),
  `-rbgpupdefix` (composes with the upstream fix, halt returns),
  `-rbgpunodisppanic`, `-rbgpusdmahook`, `-rbgpustampraw`, `-rbgpuregdump`,
  `debug=0x100`.
- **`-rbgpulinear` is now a SUSPECT in the text defect (2mm), not neutral.**
  It forces client surfaces to LINEAR while the userspace driver lays them out
  in 64 KB tiles; every geometry whose tiled footprint exceeds its pixel bytes
  renders wrong and every geometry where the two agree renders correctly. Test
  pending: remove it and the height sweep must go clean. The note below is the
  pre-2mm reading and its ~80 % widget win still stands.
- **`-rbgpulinear` belongs IN and is currently OUT.** Boot 104 removed it and
  the widget/Dock/chrome artifacts came straight back on a healthy chip, so
  its ~80 % is real and not an artefact of boot 98's broken state. Restore:
  `sudo tools/efi.sh bootarg add -rbgpulinear && sudo nvram rbgpu-arm=1`
  It does **not** affect the text in either direction (2ln).
- **`-rbgpumtllinear`** (2lm) is built but OUT: a Lilu user patch forcing
  `DetermineSwizzleMode` to LINEAR in **Terminal only**. Not armed; 2ll's
  result must be read first.

## Rescue

`-rbgpuoff` in boot-args: the kext still loads, no peer instance exists.
