# RembrandtGPU — notes for Claude

> **THIS FILE IS A REFERENCE CARD AND MUST STAY ONE.** It has ballooned into
> milestone narrative **three times** (last: 221 KB / ~55 k tokens, loaded
> every turn, trimmed 2026-11-06). Every time, everything in it was already in
> the journal under its tag. **Put findings in the journal, not here.** Nothing
> is deleted, only moved; git history holds every earlier version.
>
> - `docs/HANDOFF-NEXT.md` — current state and what to do now. **Read next.**
> - `docs/JOURNAL.md` (~61 000 lines) — the chronological evidence record,
>   authoritative for what was measured, in order, including every withdrawn
>   reading. **Never trimmed. Grep it by milestone tag** (`2xw`, `2zw`, `2E9`);
>   never read it whole.
> - `docs/EXTERNAL-DISPLAY.md` — the one open goal, in full.
> - `docs/PLAN.md`, `docs/APPLE-STACK.md`, `docs/UPSTREAM.md`.

Goal: graphics acceleration for the Radeon 680M (Rembrandt, `1002:1681` rev 0a,
GC 10.3.3 / DCN 3.1.2 / PSP+SMU 13.0.x / SDMA 5.2.3) on macOS Ventura and up
(`src/common/rb_osver.h` holds what is *measured* per OS, which is not the same
as what loads). Machine: ASUS TUF A15 FA507NU (Ryzen 7 7735HS), **macOS 26.7.2
(25G332, Darwin 25.6.0)**, OpenCore, csr-active-config **0xA43**.

**IT WORKS.** An armed boot is an accelerated Mac: Metal bit-exact,
`AMD Radeon 680M` / 4 GB / Metal 3 in System Profiler, `RBAMDFramebuffer`
owning the panel, Apple's `AMDRadeonX6000` compositing for it, ~91-140 fps,
brightness, a derived refresh ladder, USB on real interrupts. Arming is the
**default** (`src/common/rb_defaults.h`); the boot sentinel (`rbgpu-safe`) makes
a boot that dies inside 60 s come up stock.

**THE CHASSIS FIRMWARE IS PART OF THE CONFIGURATION (rule 47).** Must be in
**iGPU-only / Eco mode** (ASUS setup, set from Windows). In hybrid mode the eDP
backlight is wired to the dGPU and our registers drive nothing — six milestones
went into that gap. HDMI-A is dGPU-wired, so HDMI and brightness are mutually
exclusive on this chassis. Type-C DP alt mode *does* reach the 680M in Eco mode.
`./build/out/blset` reports the mode first (a `vendor-id` `0x10de` on the bus
means hybrid).

---

## Layout

- `src/common/` — freestanding C shared by kext, plugin and hosttest. Pure
  logic lives here so the hosttest decides everything the kext decides:
  `rb_discovery.c`, `rb_probe.c` (read-only), `rb_regs.h` (every offset cites
  its Linux header), `rb_smu*.c`, `rb_otg.h`, `rb_agdc.h`, `rb_hubp.h`,
  `rb_backlight.h`, `rb_hpd.h`, `rb_aux.h`, `rb_dpcd.h`, `rb_dig.h`,
  `rb_digbe.h`, `rb_dio.h`, `rb_dpalt.h`, `rb_dpdto.h`, `rb_dptx.h`, `rb_enc.h`, `rb_drr.h`, `rb_frametime.h`,
  `rb_gpuname.h`, `rb_osver.h`, **`rb_defaults.h`** (the default-on tokens).
- `src/kext/` — `RembrandtProbe`, read-only, publishes `RBProbe*`, unmaps.
- `src/fbkext/` — `RBAMDFramebuffer`, the `IOFramebuffer` for the
  firmware-lit panel. `make fbkext`; **installed by the user.**
- `src/plugin/` — the Lilu plugin: `kern_start.cpp`, `RembrandtGPUPeer.cpp`
  (IOService peer, diag table, NVRAM keys, default-on gate, sentinel),
  `rbgpu_agdc*.cpp`, `rbgpu_mtlcave.h`. Boots from the OpenCore EFI.
- `tools/hosttest/` — probe against a simulated chip + parser fuzzing.
  `make kext` runs it first; **never load a kext whose hosttest fails.** Add a
  case whenever `src/common` changes, and **check the new case fails when the
  code it guards is broken** — one break per guard (rule 48).
- `tools/re/` — static analysis. Kexts: `kc_extract.py`, `machoread.py`,
  `dumptable.py`, `kc_stubs.py`, `vtslots.py`, `strxref.py`, `switches.py`,
  `ptrsym.py`, `vcall.py`, `fldstore.py`. dyld cache: `dsc.py`,
  `dyldcache.py`, **`dscbase.sh`** (derives the live base — build first),
  `dscinv.sh`, `dscsym.sh`, `dscdis.sh`, `ripxref.py`. Also
  `vbios_dispobj.py` (our own `firmware/680m-vbios.rom`),
  `xhci_intr0_derive.py`. Output to `build/re/`.
- `docs/linux-ref/` — unmodified Linux files the code follows;
  `UPSTREAM.md` records each commit.

## Workflow

- `make kext tools`, `make fbkext`, **`make plugin`** (NOT rebuilt by
  `make kext tools`), **`make dmubbl`** (its own target — `make tools` does not
  build it; 2E9 lost a reading to this). Rule 15 applies to tools too.
- **Always check the UUID moved** (`dwarfdump --uuid build/out/...`).
- For `src/fbkext`, check the UUID **inside the collection**, not in
  `/Library/Extensions` — different facts, three boots went to the gap:
  `kmutil inspect -B /Library/KernelCollections/AuxiliaryKernelExtensions.kc
  --show-kext-uuids | grep rembrandt`
- **Installing the framebuffer is ONE command: `sudo tools/fbinstall.sh`.**
  It stages, builds the aux collection to a scratch path, refuses unless that
  collection carries the built UUID, backs up the live one, writes it, and
  verifies the UUID inside the live collection.
- **`kmutil install --update-all` cannot work here** — it rebuilds the boot and
  system collections and dies on a missing KDK/kernel variant before reaching
  the auxiliary one. Build only the aux collection, and
  **`--allow-missing-kdk` is required on `create` too** or it writes nothing
  and **exits 0**.
- After any plugin change: `kmutil libraries -p build/out/RembrandtGPU.kext`
  must resolve with **0 from `SystemKernelExtensions.kc`** (the 2oz trap), and
  `grep -c IOGraphicsFamily src/plugin/Info.plist` must be **0**.
  `src/fbkext` is the opposite — it *requires* IOGraphicsFamily from the system
  KC, and `make fbkext` fails if absent.
- Probe rules: no writes except `MM_INDEX`/`MM_INDEX_HI`, never enable PCI
  decoding or bus mastering, never map BAR0's first 16 MB, read only inside the
  carve-out. SMU (`SMU=1`): MP1 C2PMSG_66/82/90 only, allowlisted queries only,
  nothing sent unless the response register reads 1, stop at the first non-OK.
- Register gates follow Linux's **version grouping**, not the exact version:
  this chip is MMHUB 2.4.0 (`mmhub_v2_3`) and DMU 3.1.3 (`DCN_VERSION_3_1`,
  `dcn_3_1_2`, and **DPCS 4.2.0** — `RDPCS` appears nowhere in
  `dcn_3_1_2_offset.h`).
- Captures land in `logs/<timestamp>-plugin/`.
- **`make clean` deletes `build/re/`** and every extracted Apple binary; their
  absence looks exactly like a finding. Re-extract before trusting an empty
  static search.
- **ASan is broken on CLT 16.4 + macOS 26** (a two-line printf SIGILLs). The
  Makefile probes and degrades loudly. Never commit a change whose only passing
  run was unsanitized.

## Hard rules

- **Always ask before loading or unloading a kext.** A bad load ends the
  session.
- The user runs `sudo tools/load.sh`, `unload.sh`, `fbload.sh`,
  `sudo tools/efi.sh …`, `tools/plugin_check.sh`. **Claude never runs these and
  never asks for the password.**
- **Never install into the EFI or `/Library/Extensions`.** Claude builds and
  hands over the commands.
- **Never `kmutil unload` the framebuffer kext — it PANICS** (2sq). Reboot.
- Before any load, **commit**. Claude may commit and push at any time without
  asking (2026-10-04, 2026-10-30).
- **Deliberate diagnostic panics are off the table** (2026-10-12).
- **Adding an SMU message group is the user's decision** — say what it changes.
- **NootedRed / NootRX are under the ChefKiss OKL v1, whose §2-§3 forbid AI
  reading, summarizing or analyzing the work. Do not read them.** Findings from
  those projects must come from the user in their own words. (Same care for
  NullMoth's `nvidia-macos-driver`: source-available, non-commercial — read the
  LICENCE before the code.)
- **This session runs with `DYLD_INSERT_LIBRARIES` set and every shell inherits
  it.** Launch anything with `env -u DYLD_INSERT_LIBRARIES` (2ns).

---

## Rules that cost boots to learn

Each was paid for on hardware; the bracketed tag is the journal's evidence.
**Keep the numbers** — they are cited throughout the journal.

**Routing Apple's code**

1. Never route a name unless `nm -m` shows it **defined** there. [2c]
2. Refuse a target whose first five bytes hold a rel8/rel32 branch — Lilu
   copies displaced instructions byte for byte. [2gb, 2xu]
3. Grep the mangled symbol first: a second `RouteRequest` for a routed
   function **panics**. New transforms go inside the existing hook. [2kn]
4. `RouteRequest::from` is **not** written back by Lilu 1.7.3 though the header
   says so. Call `solveSymbol` yourself. [2kp]
5. Cross-check hand-written mangled names against `nm` — a wrong length prefix
   silently unscopes the fix. [2kr, 2s2]
6. When a fix moves upstream, **remove the downstream patch in the same
   change**. "Redundant" and "inert" differ. [2kv]
7. vtable dumps: **call offset = dump offset − 0x10**; a non-virtual call
   through a vtable *symbol* uses dump offsets directly. [2gp]

**Reading a boot**

8. **Read `rbgpu-boot` first.** No increase = the boot was never armed and
   nothing in NVRAM is this boot's. Never label a milestone with a guessed boot
   number. [2iz, 2wl, 2B5]
9. `rbgpu-*=0` is a **durable disarm** (no one-shots since 2xw); the sentinel is
   the recovery. When you invert what a key MEANS, ask what value your own
   earlier builds left in it. [2iz, 2xw, 2xx]
10. **Capture in the armed boot, over SSH, before rebooting.** What must survive
    an uncapturable boot has to be in NVRAM and **periodic**. [2jg, 2s5, 2wl]
11. A note is evidence only when its `b=` nonce is new. [2gd]
12. A stale note says nothing about where a boot got to. A user rebooting a
    black screen looks exactly like a hang. [2gw]
13. **A return code is not a result until the code that sets it has been read**
    — request 0x1B's *success* path returns `kIOReturnUnsupported`, and
    `CGCompleteDisplayConfiguration` returns success over a mode change that
    never reached the kext. On a bus, absence of a return value is not absence
    of an effect. [2ha, 2s2, 2z1, 2D9]
14. **Absence is not a result.** A silent return, a full diag table and a
    missing write look identical. Every path out must say something; refusals
    are **codes**, not bools; check `diag_used` vs `diag_max`. Applies at the
    **tool** boundary too — a missing ioreg key read as `-1` is `0xffffffff`,
    which passes most field tests. [2eh, 2ds, 2qw, 2s6, 2w7, 2CD, 2E6]
15. Check the loaded UUID, keys and boot-args **for every artifact the boot
    loads** — plugin from the EFI, framebuffer from `/Library/Extensions`, and
    **the userspace tool too**. 2xx cost nine boots because the plugin was
    reinstalled every time and the framebuffer never was. [2cu, 2w6, 2xx, 2E9]

**Instrumenting**

16. **The instrument is a suspect.** It has cost a wedge, a crawl mistaken for a
    hang, two panics, three destroyed captures and a build that looked like a
    failing test. When a measurement and a suspect are both new, remove the
    suspect rather than reasoning about it. [2fo, 2ez, 2rh, 2xt]
17. **Never substitute a value and then narrate it as a hardware result**, and
    never turn two numbers into a mechanism. A counter that moves in a
    **non-interleaved** comparison has not been shown to move because of the
    change. [2cy (fifteen milestones), 2A6, 2B7]
18. Do not build a table from n=1 rows; two numbers agreeing to one figure is
    not an identification. Applies to the **candidates** as well as the
    readings. [2ev, 2wt, 2C5]
19. A NULL check is not a pointer check. Floors: `0xffffff8000000000` kalloc'd,
    **`0xffffff7f80000000`** kext symbol. A refusal must publish the value it
    refused; a captured pointer needs a captured control in the same boot.
    [2gh, 2qu, 2s6]
20. A registration that reports success is not evidence a patch applied; log
    the enabling field. **And a readback agreeing with the target is not
    evidence of a write when the target was already present** — publish
    `changed` beside `applied`. [2lq, 2E6]
21. **Never put `DYLD_INSERT_LIBRARIES` in a launchd environment**, and test a
    recovery procedure in the situation it exists for. [2mq]
22. **Lilu's user patcher does not run on this OS** (`isUserDisabled` includes
    `>= BigSur`). Check a subsystem is *enabled* before reading how it behaves.
    [2nn, 2of]
23. `pgrep` cannot see the process a script runs inside; use
    `ps -axo pid=,command=` with a prefix match. [2nu]
24. The dyld shared cache is inside a grafted, Apple-signed cryptex and cannot
    be modified. [2ny]
25. **Read the machine, not the manual.** [2ny]
26. **A search is an instrument too** — a grep returning nothing proves nothing
    until checked against a known-present case. Paid for in: `objdump`'s tab
    separator; `strings -a` under-reporting; `otool -tV` resolving names;
    `ioreg -c X -k Y` not filtering (use `ioreg -rl -c X`); exit code read as
    verdict; **`timeout` does not exist on macOS**; a constant the assembler
    built by arithmetic; **a key name that is a prefix of another's**
    (`rbgpu-diag` vs `rbgpu-diag-prev`); an **unanchored** match where the
    VALUE contains the key (`spdisplays_metal3` printing as
    `spdisplays_metal`); and alternation ordered so a longer token can never
    win. Anchor the match or read the value. **The NVRAM diag table prints
    every number as hex with no `0x`.** [2v8, 2wc, 2wg, 2v9, 2zc, 2CD]
27. **No vnode work at plugin init** — the root fs is not mounted. `-rbgpuoff`
    stops `init()` entirely, so such a boot is not a control for it. [2oe]
28. Attribute log lines by timestamp **and** pid; the reliable separator is the
    `Darwin Kernel Version` banner. `log` is a zsh builtin — `/usr/bin/log`.
    [2q0, 2s1]
29. **Derive a kernel slide from a pointer you solved yourself, never from
    `kextstat`** — off by `0x15a00000` in one boot. [2s6, 2s7]
30. Boot-arg names: the kernel's parse is **exact**, our shell greps are not.
    Test a whole token (`tr ' \t' '\n\n' | grep -qx`) and **never name an arg
    so it prefixes an existing one** — it has bitten three times
    (`-rbgpufbdpaltq`, `-rbgpufbdigfe`). `rb_defaults.h`'s hosttest asserts it
    for derived negatives; opt-in tokens are **not** covered and need the
    manual scan. [2ss, 2xw, 2EA, 2EC]
31. **`IOLog` is not an instrument** — publish with `setProperty` and read
    `ioreg -rl -c <class>`. **What you then read is published PERIODICALLY, so
    it is stale by design.** A counter that has not moved YET is not one that
    did not move. **A dial's verification must poll something MONOTONIC**, never
    sleep and compare two snapshots of the dialled value — `RBGPUDiag` is a
    250 ms-throttled snapshot that **drops** refreshes. This has cost four
    false MISMATCHes, one aborted run and one refused send, each time in the
    *check* written to protect a dial the rule had already been applied to.
    [2sv, 2xm, 2xs, 2C5, 2D1, 2E3]
32. Read a `char*[]` table's **length** table too, and check the slot count
    against the bound. An enum 0 is a value; an unwritten field looks the same.
    [2v8]
33. **Establish which boot it is before reading anything** (`kern.boottime`,
    `rbgpu-boot`). **An index is not a count.** A sticky status register says
    what happened, not what is true now. [2v9, 2u4, 2wd, 2B9]
34. **An experiment's arm is part of the experiment**: say which keys and args
    it depends on and verify each first. A dial's **state at the start** is part
    of the arm, and a script that restores a default invalidates the next
    command typed after it. Arm a feature's dependencies **inside the
    invocation** rather than documenting them. [2w0, 2B6, 2E2]
35. **When a static chain ends at our own code, instrument our end first.**
    [2w1]
36. A log capture has a **level**, a **predicate** and a **window**, and all
    three silently drop the answer. Match the **sender**, not a function name.
    [2u5, 2w2, 2w4]
37. **A feature must stay disarmable from the machine it runs on.**
    `removeProperty` reaches no sync; `setProperty` does. [2sq, 2wj]
38. **Two halves that are separately armed must check each other** — the panel
    taken with no accelerator under it freezes on the last frame. A check in
    `armedOnce()`, not a comment. [2wp, 2xw]
39. **A measurement taken in a configuration that does not exhibit the defect
    does not locate the defect.** Say which configuration a reading came from
    and whether it has the symptom. **And when a dial turns a feature off, check
    whether it also turns the MEASUREMENT off** — an A/B whose B side cannot be
    instrumented is not an A/B. **In reverse: when a configuration is later
    found invalid, go back through the readings it invalidated** — three lines
    were closed on nulls measured in the wrong configuration. [2wy, 2x0, 2A9,
    2zu, 2D6, 2EB]
40. **Boot-arg VALUES come back as a binary int, not ASCII**:
    `int v = 0; PE_parse_boot_argn(name, &v, sizeof(v))`. [2xm]
41. **A constant is only a constant on the OS it was measured on**, and an
    instrument that re-measures something every run can still subtract a stale
    base. Derive the base too (`tools/re/dscbase.sh`); for anything that cannot
    be derived at use time, record **per OS** whether it was measured there and
    refuse to use it elsewhere (`rb_osver.h`). [2xz]
42. **A collection you did not allocate is not yours to write to.**
    `getProperty` returns a borrowed pointer the registry owns and IOKit
    enforces that with a **panic**. Copy it (`OSDictionary::withDictionary`),
    mutate the copy, `setProperty` the copy. Ask of every scalar and collection:
    **who owns this?** [2y1, 2A9]
43. **A value you read back is not a value that SURVIVES.** `nvram boot-args`
    accepts a write, reads back correctly, and is **overwritten from OpenCore's
    config.plist next boot** — four boots lost. Use
    **`sudo tools/efi.sh bootarg add|remove ARG …`**, and verify an arm **after
    the boot, from the running system**. Ask: **who else writes this, and
    when?** [2z7]
44. **A measurement needs enough samples to resolve the difference it claims,
    counted in BOOTS, not rows.** Boot-to-boot variance exceeds within-boot
    variance: one boot's six consistent samples did not survive the next boot.
    **A configuration claim needs ≥2 boots per side, or an A/B inside one
    boot — and that A/B is only valid INTERLEAVED at single-window
    granularity**, since a boot has been measured drifting ~15 % inside itself.
    The variance itself can be the finding. [2ze, 2zg, 2B7]
45. **A chain of verified links is not a verified chain.** When the deliverable
    is something a **human observes** — a panel, a frame, a lag — that
    observation is a measurement and needs its own instrument, **control and
    repeat**. A benchmark is not a workload, and "a rare event did not occur"
    is not "it cannot". [2zj, 2A6, 2D4]
46. **Check the version grouping of the METHOD, not just the offsets.** A
    register can be real, writable, readable-back and have a different owner —
    ask which Linux file the *function* comes from and whether this chip's `dc`
    would call it (`dce_*` vs `dcn31_*`). Discharge it in the resource file, by
    source, not by reasoning. **And checking that a delegate exists is not
    checking that it is REACHED.** [2zl, 2B8, 2DC, 2E5]
47. **Before concluding a register is the wrong control surface, establish the
    output is WIRED to this chip right now.** A write that lands, reads back
    bit-identical and is contested by nobody is the **expected** reading of a
    working block with nothing attached. Ask it of any output. On a laptop with
    switchable graphics the answer changes with no code change, so **the
    firmware setup mode is part of the configuration** — and so is **the power
    state of the port**: this chassis' Type-C subsystem can die in a way that
    survives a reboot and a reinstall and is cleared only by a 30-second
    power-key hold. **Asked of a WRITE:** establish the live word decodes as the
    register the conclusion assumes, because a write to a *readable wrong
    address* does something. [2zw, 2D8, 2DD, 2EC]
48. **Run a break-it check per guard, and a break that fires NOTHING is a
    RESULT.** The same defensive check is load-bearing in one decoder and inert
    in another; only the hardware says which. A test that passes **for the
    wrong reason** is not coverage, and a conjunction can be correct while one
    term carries no information. **A statistic must be derived from what the
    consumer DOES with it**, not from whichever summary is easiest. And the
    `-Werror` trap, **five times**: a break that leaves a function unused kills
    the build, and `make hosttest` then runs the previous binary with **zero
    FAIL lines** — grep `error:` as well as `FAIL`. [2BC, 2E7, 2E8, 2B0, 2A1]

**The recurring error, stated once.** Thirty-odd recorded instances of reading
a value without first establishing what makes it meaningful. The practice that
keeps catching it: **say in the code what would have to be true for the reading
to mean anything, and log it when it is not.** Three variants: **read the
evidence already in hand before designing the next experiment**; **find the
consumer before writing to a field**; and **before calling a limit physics,
check the hardware is configured the way the conclusion assumes** — and when a
fact looks unobtainable, **read the register definition before designing an
experiment to infer it** (the HPD→AUX mapping and the second flip slot were both
documented in files already in the tree).

---

## Chip facts established on hardware

- **GC seg0 = 0x1260** (four ways), seg1 = 0xa000. Apple's encoded register ids
  are `(dword offset << 32) | 11`, 11 = GC_HWID; byte address is
  `(seg + dword) * 4`. Offsets from `gc_10_3_0_offset.h`. **DMU segment 2 =
  0x34c0**, and **DMU BASE_IDX 1 = 0xc0** (2EF, off this machine's own
  `tests/data/rembrandt-680m-tufa15.bin`, with `base[2]` in the same array as
  the control; `build/out/rbdump` prints it). BAR5 is 512 KB.
- Accessor slots on `pm4+0x30` / `hw->0x368`: `[0x148]` id → dword offset,
  `[0x118]` **read**, `[0x120]` **write**. [2fd, 2fg]
- Carve-out MC `0xF4_0000_0000`, system-physical `0x7_8000_0000`
  (`FB_OFFSET` 0x780), BAR0 `0x8_8000_0000`; **the GOP framebuffer is BAR0
  offset 0**. Apple's GPUVM space is `0xFF_BF..`.
  **`mc2pa(mc) = mc - vram_start + vram_base_offset`** — the term that was the
  whole CP halt.
- **One** SDMA instance (Apple's Navi 21 path builds four). Engine array is
  `hw + 0x3b0 + engine*8` (`[0]` PM4/GFX, `[1..4]` SDMA); the SDMA walk bases
  at `hw + 0x3b8`.
- `GB_ADDR_CONFIG` dword `0x13de` BASE_IDX 0 = **0x242** (4 pipes, 4 packers,
  1 SE). Geometry 1 SE × 2 SA × 3 WGP = **12 CU**; harvest mask is **per
  shader array** and `CC_GC_SHADER_ARRAY_CONFIG` reads 0xFFF80001 (narrowed: 0).
- `AddrSwizzleMode`: 0 LINEAR, 9 64KB_S, 22 4KB_D_X, 25 64KB_S_X, 27 64KB_R_X.
  The `_X` family is the swizzle defect; **why is still not established.**
- **`getPDEValue` returns flags only** — the address is ORed in by its caller.
  Never hook it to correct an address. [2ko]
- The panel: 1920x1080, `H_TOTAL` 2106, `V_TOTAL` 1129, **143.83 Hz** native —
  **but the DRR ladder moves it, and 2FB misconverted a measurement by taking
  143.83 as true at the moment of the reading when the panel was at 61.40**;
  pixel
  clock ~342 011 761 Hz (derived; the DP PLL has never been located).
  `V_BLANK_START_END` 1311820 (start 1100, end 20) is why V_TOTAL can change
  without touching the active raster. OTG0 runs (`OTG0_CONTROL 0x00011201`),
  OTG1-3 at 0x300 = off, DMCUB **4.0.15.47**. Measure refresh by latching both
  endpoints on an `OTG0_FRAME_COUNT` edge, never over a fixed window.
- A Transfer TRB's dword 2 is
  `length[16:0] | TD-size[21:17] | interrupter[31:22]` — the USB fix's basis.
- `IODetailedTimingInformationV2` is 0xa0 bytes, `pixelClock` at 0x28;
  `AGDCLinkConfig_t` is 0xb0 and embeds it at **+0x04**.
- `IOFramebuffer` vtable call offsets: `0x9b0` `getVRAMRange`, `0xad0`
  `registerForInterruptType`, `0xae0` `setInterruptState`.
- **Every Apple address in a comment here or in the journal is a macOS 15
  address.** Re-derive with **`tools/re/dscbase.sh <Lib>`** before any static
  read. The only hardcoded Apple address left in live code is
  `RBGPU_IOP_SITE_VM`, stale and failing safe.

### The external display path (Type-C DP alt mode)

**Full record: `docs/EXTERNAL-DISPLAY.md`.** The card keeps only the shape.

Port = **HPD2 = AUX2 = UNIPHY_C/phyid 2 = DIG2**, four independent instruments.
Sink **MSI MP2412C**, DP 1.2, HBR2, 2 lanes, native 100 Hz — **reached through
an ACTIVE USB-C-to-HDMI ADAPTER (310e)**, which is what answers our AUX: DPCD
`0x005` `0x1d` has `DWN_STRM_PORT_PRESENT` **1** with type **DVI/HDMI** and
`FORMAT_CONVERSION`, `0x080` says port type **3 = HDMI** at **340 MHz TMDS**,
and `0x500` carries a **branch device** OUI `00-10-fa` / `"cHDMIb"` while the
sink block `0x400` is all zero. So **"try a USB-C-to-HDMI dongle" is the
configuration every boot of this line already ran in**, 2FB included. It does
not touch the blocker (a held PHY reset is upstream of any conversion behind
it). **The one cheap untested axis is the SINK** — n=1 across ~30 boots; a
plain USB-C-to-DP cable into the monitor's own DP input is one boot and no
code, with **no mechanism proposed** and not a candidate. **Done:** DP alt
mode granted; EDID over I2C-over-AUX; the **pixel clock running** on pipe 1's
DP DTO (boot 455, `DP_DTO*_PHASE`/`MODULO`/`PIXEL_RATE_CNTL` at **BASE_IDX 1 =
0xc0**, whose offsets collide with the DOMAIN power-gating block at BASE_IDX 2);
the sink taking a native DPCD write (boot 457, `0x100` `0x06 -> 0x14`); the DP
**transmitter block answering our writes** (boot 459, `TP_SEL 0 -> 1` on DP2,
with a four-instance in-boot control). **It is a clock, NOT a picture.**
**2F9 ran the whole chain in one boot (461) and the sink saw nothing.** The
atom transmitter ENABLE at the link rate **already existed** — `dmubbl dig`
carries `RB_DIG_TYPEC_SYMCLK` 54000 = HBR2 and has executed since 2E2; an
earlier handoff of mine called it the blocker without reading the tool. In one
boot, each step with its in-boot control: `dmubbl dig fe` moved DIG2's
`DIG_BE_CNTL` `0x00010000 -> 0x20000000` and `DIG_BE_EN_CNTL` `0x100 -> 0x101`
(DIG0/1/3/4 byte-identical); `dmubbl dto 148500` took `PIXEL_RATE_CNTL`
`0x8000c000 -> 0x8000d090` with `DPDTO_ENABLE_STATUS` 0 -> 1; `dptrain go
tps=2` moved `TP_SEL 0 -> 1`. **DPCD `0x202` read `0x00` before and after** —
no clock recovery. **`RDPCSTX2_PHY_CNTL6` never moved** (`0x00063333`, PHY0's
`0x00194444` as control), and re-sending the ENABLE *after* the clock was
running did not move it either, so **order is not the missing thing**. That
register is the suspect as much as the PHY: it is the legacy fallback Linux
reads only when DMCUB is absent and it already disagreed with `QUERY_DP_ALT` on
all five PHYs, so `dig` needs a verdict register that is not this one.
**2FB-2FE settled the raster and the clock, and neither was the blocker.**
The second raster scans (`OTG1_CONTROL 0x300 -> 0x00011201`, boot 464) and the
8.27x rate it ran at was **`PIPE_DTO_SRC_SEL`**: boot 468 at `srcsel=0`
measured 59.98 Hz against 60 asked, ratio **0.9997**, against 8.2726 (boot
464) and 8.1293 (boot 467) at `srcsel=1`. The default is now **0** and the
2 x 600 MHz candidate is **dead as a mechanism**. `rb_dpdto.h`'s argument for
Linux's 1 stands on source grounds and is not withdrawn; the dial keeps both.
With the clock right, boot 468 re-ran the whole chain — `dig fe` moved
`FE_SEL 0 -> 0x4`, `dptrain tps=2` moved `TP_SEL 0 -> 1` — and **DPCD `0x202`
still read `0x00` before and after**.

**THE BLOCKER IS NOT NAMED, and `0x00063333` IS RETIRED AS A MISREAD (300,
301).** It was read off `RDPCSTX2_PHY_CNTL6` for five boots (459, 461, 464,
468, 469) while `dmubbl dpaltq` had printed, since 2EA, that **Linux reads
`RDPCSPIPE_PHY_CNTL6`, not RDPCSTX's, for UNIPHY_C on `YELLOW_CARP_B0`** — the
recurring error's "read the evidence already in hand" form, its most expensive
instance. `src/common/rb_dpcspipe.h` reads the right block: **two** real
instances (`0x2d73`, `0x2e4b`, BASE_IDX 2; upstream's comment is
`//[Note] Hack. RDPCSPIPE only has 2 instances.`, indexes 2/3/4 alias them and
`rb_dpcspipe_dw()` **refuses** them). **Boot 471:** RDPCSPIPE0 `0x00000000`
NOT DISABLED, RDPCSPIPE1 `0x00060000` DISABLED+acked, sweep **DIFFERS** (the
control: two registers, not one address twice), **AGREES** with
`QUERY_DP_ALT` — and non-vacuous, since the other instance disagrees. Same
sample: RDPCSTX0 `0x00194444`, RDPCSTX1 `0x00023333` (settling), RDPCSTX2-4
`0x00063333`. So RDPCSTX2 contradicts **both** the ROM-attested instrument and
the register Linux reads, with a control each side. Which instance is UNIPHY_C
is **not** established (three USB-C PHYs, two registers — rule 18); the
agreement stays an **OR**. **LANE POWER IS UNTOUCHED** — this register defines
only DPALT_DP4 / DISABLE / DISABLE_ACK, **no MPLL, no ref clock, no PSTATE**,
and conflating those two questions is what made `0x00063333` look like one
verdict. `tools/dpcspipelook.sh`. `QUERY_DP_ALT` is ROM-attested on three
boots (2E1's mask `0x1c`; boots 449, 470, 471) and of the three USB-C PHYs
**only PHY2 reads `alt_disable` 0**. **Boot 469 also killed `hpdsel`** (in-boot
A/B, Sent 3 vs Sent 2, both acked, PHY2 unmoved) **and power gating**
(`RBFBPgCandidate` -1, `dmubbl dom` refusal 24). `rb_dpalt.h` has **no setter**
and Linux marks that write path "avoid if possible".

**THE CABLE MUST BE PLUGGED IN AT BOOT (2FB).** Plugged in afterwards, every
AUX read returns **refusal 3** on engine "AUX5" (= `RB_AUX_COUNT`, none):
AUX1-4 read `AUX_EN` **clear** and `HPD2_STATUS` reads `0xff000013` against a
healthy `0xff0ff013`, the missing `0x000ff000` being a programmed delay field.
We never write `AUX_EN` — the firmware's connect path does, at boot. Cable in
at boot: engine **AUX2**, 128 bytes, refusal 0, `RBFBAuxEn` mask **5**. Rule 47
with a new clause: **so is WHEN the port came up.** (`RBFBAuxEn` and
`RBFBHpdSink` are **masks**, not indexes — rule 33's shape, cost two misreads.)

**307: THE PHY IS HELD IN RESET, AND THE ATOM ENABLE DOES NOT RELEASE IT.**
`src/common/rb_dpcsphy.h` + `tools/phylook.sh` read the RDPCSTX PHY with the
live panel as the positive control in the same sample. **PHY0: `PHY_RESET`
0, `TX_CLOCK_ON` + `SRAMCLK_CLOCK_ON` set, `TX_FIFO_EN` set. PHY2:
`PHY_RESET` 1, no clock, no FIFO, `SRAM_INIT_DONE` SET** — so the block is
powered with its firmware loaded and simply never released, and PHY2 is
**byte-identical to PHY3, which nobody has touched**. Then, **in the same
boot with a four-instance control**, the atom transmitter ENABLE (phyid 2,
action 1, digmode 0, lanenum 2, symclk 54000) was sent twice, `refusal 0`,
`ran 2` — and PHY2's three words did not change one bit. **But `ran` is a
MAILBOX ACK:** `transmitter_control_v1_7` calls `transmitter_control_dmcub`
and **`return BP_RESULT_OK` unconditionally**, so *Linux cannot tell whether
the table ran either* (rule 13 upstream). "The ENABLE has executed since 2E2"
was only ever "DMCUB consumed the slot".
**Four candidates died from source with no boots:** DCN31 NULLs **all** of
`dpcs_initialize_phy` / `dpcs_configure_phypll` / `dpcs_configure_dpcs` /
`dpcs_enable_dpcs` / `set_phy_source`, so **there is no driver-side PHY
bringup to be missing**; `dcn20`'s own sequence runs only under
`debug.avoid_vbios_exec_table` (default false, and needs those NULLs), so
**the VBIOS table is the only mechanism that exists**; every payload field is
now verified against the source (54000 = 20 x 27000 / 10; `digfe_sel` 0
unconditionally by the helper; `connobj_id` 0 on a `cntl = {0}`) and
**`pll_id` is not in the v1.7 payload at all**, killing "we omit the clock
source"; and `set_dio_phy_mux` is called **only by the HPO path**, so 2FC's
mux is not the gap. **No DP-alt acquire exists in the driver** — ownership is
the firmware's.
**Boot 478's DIG1/DIG3 conclusion is WITHDRAWN**: `DP_VID_N`'s `0x2000` was
already there going in on DIG2 too, so "they retain no stores" was a misread
and there is **no per-instance power discrimination**. What stands is uniform:
`DP_VID_M`/`DP_VID_N` stores do not stick on any instance tried, which sits
downstream of a PHY in reset.

**308: THE SRAM-LOAD HANDSHAKE IS THE ONE LIVE CANDIDATE, AND IT WAS IN OUR
OWN HEADER.** Decoding boot 479's `PHY_CNTL0` words per FIELD instead of as
hex — the first time — against `dpcs_4_2_0_sh_mask.h`'s sixteen fields: the
live panel (`0xb0100004`) has **`SRAM_EXT_LD_DONE` 1, `SRAM_BYPASS` 1,
`REF_RANGE` 0** with `PHY_RESET` 0; PHY2 (`0x10100605`) has **both SRAM bits
0, `REF_RANGE` 3** with `PHY_RESET` 1. **`SRAM_INIT_DONE` is set on BOTH**, so
307's "the firmware is loaded" was the term carrying no information.
`SRAM_BYPASS` was **not in `rb_dpcsphy.h` at all**. Candidate, **not** a
mechanism: n=1 per side, no caller, and **the two SRAM bits move together in
that sample, so they are ONE observation** (the break-it sweep found that —
third pair this file's own data cannot separate). **`SRAM_EXT_LD_DONE` had
been in the ANSWERED set on nothing but its name** — no vendored `.c` names
it — so there is now a third **`RB_DPCSPHY_U_*` UNATTRIBUTED** set, pairwise
disjoint per register, and **no setter**. `rb_dpcsphy_sram_diff()` compares
against the panel and refuses **-2** when the subject *is* the control.
**308b: THE ROM IS ELIMINATED.** `firmware/680m-vbios.rom`'s master command
table (`0xa480`, rev 2.1, 81 entries) has slot 39 `dig1transmittercontrol` =
**`0x0000`**, with every display-encoder entry 0 — so the firmware servicing
our command is **DMCUB's image** (4.0.15.47, CW0/CW1), not the ROM. `dmubbl`
had printed this since 2E2 and a handoff of mine said to read the ROM anyway.
**And no sibling project has the answer:** **nullarc** (780M, Phoenix, DCN
3.1.4 — the nearest chip to ours, MIT/GPL-2.0 + PolyForm-NC) lists external
displays under *"Not yet"* and has **no DPCS material**; **Cobalt64** (MIT,
RDNA 4) reads the whole RDPCSTX block — ~35 registers against our five — but
writes no reset release; **UnifiedRed** (BSD-3-Clause) is DCE 11 era.

**309 / BOOT 481: 308'S CANDIDATE IS DEAD, AND THE SURVEY CANNOT SEE ITS OWN
SUBJECT.** `EXT_LD_DONE` and `SRAM_BYPASS` are set on the live panel and
**clear on ALL FOUR dark instances**, so they separate live from dark and say
**nothing about PHY2** — the generic "never brought up" signature. `REF_RANGE`
does not even do that (dark PHY4 reads the panel's 0) and `SRAM_INIT_DONE` is
set on four of five. **The candidate died because the dark instances were in
the same sample** — the one part of 308 that worked as designed. The licence
gate is confirmed on hardware: **verdict 5 NO_LICENCE on every instance**,
panel included.
**The real finding: PHY2 and PHY3 are BYTE-IDENTICAL across all five
registers** (`0x008f00f0`, `0x001019f0`, `0`, `0x03100000`, `0x10100605`)
while ROM-attested `QUERY_DP_ALT` — re-run this boot, mask `0x1c` matching
2E1 again — says **PHY2 `alt_disable` 0** against **PHY3 `alt_disable` 1**. So
the state that makes PHY2 drivable is **in none of the registers we read**,
five of ~35 in the block. RDPCSPIPE also reproduced boot 471 exactly (rule
44's second sample, free).
**309 added four registers named for the question**, read-only, no setter, all
45 offsets script-verified: `RDPCSTX_DPALT_CONTROL_REG` `0x2956`
(`ALLOW_DRIVER_ACCESS` / `DRIVER_ACCESS_BLOCKED`, "who owns this" asked of the
hardware), `RDPCSTX_DMCU_DPALT_DIS_BLOCK_REG` `0x293c` (`FORCE_TX_CLK_DIS`),
`RDPCSTX_DMCU_DPALT_PHY_CNTL6` `0x2955` (a **third** source on
`DPALT_DISABLE`), `RDPCSTX_CNTL2` `0x2939` (`PHY_ENC_TYPE_SEL`). **All
UNATTRIBUTED because their writer is DMCU** — neither party the
ANSWERED/DRIVEN split separates. **308's lesson is structural now:** every
instance publishes **two** diffs, against the panel and against
**`RB_DPCSPHY_DARK_USBC_INST` = PHY3**; only a difference holding against
**both** is PHY2's own, and the hosttest encodes 308's dead candidate as a
failing case. Ten break-it guards; the one that fired nothing was **invalid**
(a cross-register mask, which the per-register disjointness check correctly
permits). Framebuffer UUID **16CB216D**.

**309b / BOOT 482: THE OWNERSHIP CANDIDATE IS DEAD, AND THE SURVEY IS DEAD AS
A METHOD.** `RDPCSTX_DPALT_CONTROL_REG` reads **`0x00000000` on all five
instances, the live working panel included** — `ALLOW_DRIVER_ACCESS` 0 and
`DRIVER_ACCESS_BLOCKED` 0 everywhere. **The panel drives a 1080p link right
now with ALLOW clear**, so the field cannot mean what its name says on this
part, and the positive control is what establishes it. `DIS_BLOCK` is 0 on all
five too, so `FORCE_TX_CLK_DIS` is **not** why PHY2 has no `TX_CLOCK_ON`.
**`DMCU_DPALT_PHY_CNTL6` retires `0x00063333` a SECOND way:** PHY2 reads it
(PSTATE 3 x4, `MPLL_EN` 0, `DPALT_DISABLE`+ACK) agreeing with 301's retired
RDPCSTX read and contradicting `QUERY_DP_ALT` — **but PHY2, PHY3 and PHY4 read
it IDENTICALLY** while the query separates them, so **a register that reads
the same on a granted port and a disabled port is not per-port grant state**
(every field `_RESERVED` upstream). PSTATE 3 is **USB-C-ness, not
working-ness** — dark non-USB-C PHY1 reads 0. `CNTL2`'s `CR_CONVERT_FIFO_EMPTY`
splits USB-C from not, again; `ENC_TYPE_SEL` is 0 on all five and 2FC already
established DIG **is** zero, so no finding was available. **The extended A/B:
the atom ENABLE moves NONE of the nine** (307 could only see three), four
instances as control.
**Nothing in nine registers distinguishes PHY2 from PHY3** — `AccessDiffDark`
**0**, the only non-zero diff against the panel and shared by PHY1 and PHY4.
**Three surveys of this block have now failed the same way** (five registers,
then nine); the only instrument that separates PHY2 from PHY3 is still
ROM-attested `QUERY_DP_ALT`. **Reading more of this file is not the next
step.** Two places are left: the PHY's **internal** registers behind
`RDPCS_TX_CR_ADDR`/`CR_DATA` (Cobalt64 reads `CR_ADDR` and deliberately does
**not** read `CR_DATA`, suspecting the read is itself a trigger — a hazard,
not a free read), and **DMCUB's image**, which services both the command and
the query.


**310 / 310d: THE COMMAND EXISTS, NOBODY HAD SENT IT, AND BOOT 485 SENT IT —
THE FIRMWARE DOES NOT ANSWER.**
**`DMUB_CMD__VBIOS_TRANSMITTER_SET_PHY_FSM = 29`**, same VBIOS family as the
atom ENABLE (1, moves nothing) and `QUERY_DP_ALT` (26, the only instrument
that tells PHY2 from PHY3). *"Control PHY FSM"*. `enum dmub_phy_fsm_state`
names **1 RESET** (307's measured PHY2), **2 RESET_RELEASED**, **3
SRAM_LOAD_DONE** (308's milestone), **10 TX_EN** (the live panel) — three
milestones surveyed this machine's *output* register by register while the
machine itself had a command. Payload is **"passed in/out" with a `uint32_t
status`**, the execution evidence command 1 structurally cannot give: its
`transmitter_control_v1_7` returns `BP_RESULT_OK` **unconditionally**, so
**`ran` is a MAILBOX ACK and Linux cannot tell either**. No caller anywhere,
upstream included.
**`src/common/rb_phyfsm.h` + `dmubbl fsm` + `RBAMDFramebufferPhyFsm.cpp`.**
Payload **24** (query_dp_alt declares 4, the transmitter 60 — one union, three
sizes), header `0x18001d80`. **Units: `symclk_10khz` 54000 on command 1 vs
`symclk_100Hz` 5400000 here — a different FIELD NAME and a factor of 100**;
310's own entry said "same name, factor of ten", wrong twice, and
`rb_phyfsm_symclk_from_10khz()` now has a test pinning it. Target states are
an **allowlist** (2,3,4,5,9,10): RESET and POWER_DOWN undo the aim and
**TX_EN_TEST_MODE is refused because 2F8 showed a pattern degrades this sink
until a reboot**. `status` prefilled `0xFFFFFFFF` so NO_REPLY has its own
code; `mode` 0 = DP is also what an unwritten byte looks like, so **no
default**. Refusals **70..79**. **`dmubbl fsm` and `fsm <state>` return before
`findFB()` — only `fsm go <state>` sends**, deliberately unlike `dmubbl
dig`/`dto`, which write with no argument. The verdict is **two things, never
one number**: did DMCUB ANSWER and did the PHY MOVE (nine registers, four
instances as control), with `rb_phyfsm_verdict()` checking the **controls
before any success** (`V_CONTAMINATED`, not `V_RELEASED`). 30 break-it guards;
**the `-Werror` trap fired for the seventh and eighth time**, one break was
caught by **ASan while my classifier reported FIRED NOTHING**, and one fired
nothing because **MY COMMENT WAS WRONG** (`ctlMoved` implies `anyMoved`).
305's arm-ordering trap fired on the first draft, and `rb_phyfsm_verdict()`
sat in the tree with a **type error and no caller** — `-Wall` caught what the
hosttest could not, because **a function with no caller has no coverage**.

**BOOT 485: FOUR SENDS, STATES 2 / 3 / 10 / 2, `refusal 0` EACH — `decode 77
NO_REPLY` EVERY TIME, SUBJECT MASK 0, CONTROL MASK 0.** The fixed printer says
more than the status field: **`reply slot 0xc80 verdict 2 UNCHANGED - our
bytes, untouched`**, so DMCUB wrote nothing into the **whole 64-byte slot**.
**And the negative is bracketed in the same boot**: the ring **drained**
(`Rptr == Wptr == 2432`, 1 ack — DMCUB consumed it), and `dmubbl dpaltq`
returned `is_usb` mask `0x1c` with `alt_disable` 0 on PHY2 alone, **ROM MATCH**
against 2E1 — so a sibling command on the same ring in the same boot **does**
reply, and the reply is the chip's. **DMCUB 4.0.15.47 does not act on
sub-type 29.** Not separated: unimplemented vs implemented-and-rejects-phyid-2
with no status either way; both are "does not act on it". `phylook` after all
four: PHY2 **byte-identical to where 307 found it** and still to dark PHY3.
DPCD unchanged — `0x202` `0x00`, `0x204` `0x80`, `0x205` `0x00`.
**The state axis is not the variable** — state is cumulative, so 2 -> 3 -> 10
was itself a ladder and it moved nothing. **310's candidate is dead, and it
retires 307's open question**: the execution evidence has now been asked for
with a command that can give it, and this firmware answers neither.
Instrument defect recorded: `fsmReport()` printed
`status 0xffffffffffffffff` because `num()` returns a signed long and the
`%08lx` sites did not mask. **The kext's compare was on the 32-bit value and
`decode 77` was sound** — a *display* defect, but one a journal reader cannot
tell from a measurement defect, which is the whole of rule 16. Fixed with the
`& 0xffffffffL` the `dig`/`dto` reporters already use. Also:
`RBFBFsmArmed`/`RBFBFsmCalls` are absent from ioreg until the first
`RBFBFsmGo` — a **publish gap, not a wrong kext**.

**And the CR window is NOT A READ.** `RDPCS_TX_CR_ADDR` (0x2934 + n*0xd8) /
`CR_DATA` (+1) are plain 16-bit value fields — no busy, no trigger, no status —
with a second name for the same address (`DPCSSYS_CR<n>_*`) and **no vendored
`.c` user**. Reading `CR_DATA` returns whatever register the **firmware's own
latched address** selects, and choosing one means **writing CR_ADDR** and
clobbering that latch mid-sequence. Rule 47's write clause in a new form: the
live word decodes not as a register but as somebody else's cursor.
**Deprioritised.** Full RDPCSTX2 list is in the journal (~41 per instance;
`SCRATCH` 0x2ae7 and `DEBUG_CONFIG` 0x2aed are the only ones worth a line
later).

**Still not done:** a **stream** on DIG2, a surface, and a second
`IOFramebuffer` (`getDisplayModeCount()` has returned 1 since 2sl).
**303 read the block; 304 derived what to write and gave the derivation a
control.** Addresses **confirmed three ways in boot 473**: `BASE CONFIRMED`
(MSA = OTG **+1** on both axes — `dcn10_optc.c:191` vs
`dcn10_stream_encoder.c:419`, the convention that refused 472),
`MSA_PARAM4` = `0x07800438` = **1920x1080** active (the stronger check, DRR
cannot move it), and `MSA_PARAM2` VSTART **20** = the V blank end. Panel
instance **RUNNING**, masks all `0x01`, sweep DIFFERS, **DIG2's MSA registers
all blank** — the positive identification of what is missing.

**Two per-instance differences on DIG2, four-instance control** —
`DP_STEER_FIFO_RESET` **set** on DIG2 and clear on all four others (a **held
reset**; `dp_unblank` pulses it 1->0), and `DP_VID_STREAM_DIS_DEFER` **0** on
DIG2 against **2** everywhere else, so 2 is the reset value and DIG2's 0 was
written by the firmware at boot. **Both reproduced in boots 472 and 473**
(rule 44's two boots per side). **`DIS_DEFER` IS RETIRED AS A CANDIDATE on
source grounds**: `dcn20_stream_encoder.c:541` says *"this register has no
effect on enable transition"* and upstream stopped programming it — so it is
real, firmware-written, and **not** the blocker, and the write set must not
touch it. The held FIFO reset survives as the only live candidate of the two,
and the unblank's own pulse covers it by construction.

**304: every MSA field is derivable from six OTG registers, and the panel is
the control.** Inverting `optc1_program_timing` collapses the arithmetic:
**`MSA_HSTART` = `OTG_H_BLANK_END`**, **`MSA_VSTART` = `OTG_V_BLANK_END`**,
sync widths = `OTG_*_SYNC_A_END`, and the **polarity bit transfers unchanged**
because optc and MSA both store the negation of the same flag; totals are the
register **+1**, `HWIDTH`/`VHEIGHT` are blank `START - END`.
`rb_strenc_derive_msa()` writes its output **only on OK**, has nine refusal
codes, and `rb_strenc_msa_mismatch()` returns a **mask of which word
disagrees** (`0x10` = could not derive, above the four-bit range).
`strencSample()` derives **both pipes** and compares **pipe 0's against DIG0's
four live words bit for bit in the same sample** — `RBFBStrencMsaMismatch`,
0 = agree. That is **PARAM3's only real check**: the hosttest's sync inputs
were back-derived from its own expected output, which is circular, so the
boot is where the sync and polarity fields are first read rather than assumed.
Pipe 1's derivation has **no control** and is published rather than hidden.
Fourteen break-it checks, all caught by a failing test.

**The M/N formula is validated against hardware**: `n_vid` is always `0x8000`
and `m_vid = N * pix_khz / (link_rate * 27000)`; at the panel's ~342 MHz over
**HBR** that gives 41508 against a live `0xa2a4` = 41636 (0.3%, the residual
being the DRR drift). **`DP_VID_M` is hardware-generated on the panel**
(`DP_VID_M_N_GEN_EN` set) and moved `0xa2a6` -> `0xa2a4` between boots 472 and
473, so it is a value to **prime**, never to copy. For the sink at 148500 kHz
over HBR2: `m_vid` **9011**. `tools/strenclook.sh`.

**305 RAN THE WRITE IN BOOT 477: 19/19 APPLIED, AND STATUS READ 0.** Every
mechanical part worked — MSA read back exactly, **`DP_STEER_FIFO` 1 -> 0**
(DIG2's held reset from three boots, cleared by the unblank's own pulse),
**`DIG_FE_CNTL` -> 1** (`DIG_SOURCE_SELECT` = OTG1, never written before 305),
`CNTL 0x00100000 -> 0x00100001`, m_vid 9011. **But bit 16 STATUS read 0 —
VERDICT 2 NO_STATUS**, the first time this line has asked the hardware for a
stream and been declined. In-sample control: DIG0 ENABLE 1 / STATUS **1**,
DIG2 ENABLE 1 / STATUS **0**, DIG1/3/4 both 0. **"Read too early" is dead**
(OTG1 2849 -> 2969 frames = exactly 60.0 Hz, so ~2900 vblanks at STATUS 0).
**The BE-enable dependency is eliminated**: `dmubbl dig fe noenc` moved DIG2's
`DIG_BE_EN_CNTL` `0x100 -> 0x101` with a four-instance control; STATUS
unchanged (`DIG_BE_CNTL` was already `0x20000000`, firmware-set at boot).
Pipe 1's derived MSA verified independently against CEA-861 1080p60 — HTOTAL
2200, VTOTAL 1125, HSTART 192, VSTART 41, HSYNC 44, VSYNC 5, 1920x1080, all
exact, and **the polarity differs from the panel's**, which is why the field
is known to be read rather than carried over.

**THE WRITE LIST LOCKED OUT ITS OWN NEXT STEP.** `dptrain go tps=2` refused
**66 `RB_DPTX_W_STREAM_LIVE`**. 305 collapsed `dp_set_stream_attribute` and
`dp_unblank` into one list, and **they are two functions precisely because
link training happens between them** — `enable_link_dp` trains, then
unblanks. The card had "FE connect -> configure -> transmitter ENABLE" and
never that **the stream enable comes AFTER training**; it was in
`enable_link_dp` all along. This does **not** say a trained link gives
STATUS 1 — it says the experiment could not reach the state where that is
asked.

**305b SPLIT IT AND BOOT 478 PROVED THE SPLIT — AND THE BLOCKER IS
UNTOUCHED.** `rb_strprog_build()` takes a **phase** and `rb_strprog_plan`
**returns** the wait points (the `WAIT_*_IDX` constants are gone — they differ
per phase). **ATTRIBUTE** (`strencprog go`, 10 writes) ends with
**`DP_VID_STREAM_ENABLE` 0** — not upstream's, and the recovery from 477.
**UNBLANK** (`strencprog unblank`, 10 writes): ENABLE 0 + wait, M/N primed
inside GEN_EN 0/1, `DIG_START` pulse, FIFO pulse ending at 0, ENABLE 1 + the
STATUS read. **Order: `go` -> `dptrain go tps=2` -> `unblank`**, phase
defaulting to ATTRIBUTE everywhere; the load-bearing guard is that ATTRIBUTE
**cannot** enable the stream. Twelve break-it checks, eleven caught by a
failing test and none by `-Werror`; the twelfth **fired nothing** first time
(the break was inert — 304's `| 1u` again) and the sweep also caught the check
reading `w[idx]` before bounding `idx`.
**Boot 478: `dptrain` RAN, no refusal 66** — the ordering defect is fixed, the
whole chain executed (DTO 0 -> 1; OTG1 scanning; configure 10/10 changed
`0xf7` with ENABLE left 0; `TP_SEL 0 -> 1`; unblank 10/10 changed `0x374`,
held FIFO cleared, `CNTL -> 0x00100001`) and **STATUS stayed 0**. But
**DPCD `0x202` read `0x00` before and after**, so the boot asked whether an
UNTRAINED link answers STATUS 1 — already answered in 477. **The split was
real and was not the blocker.** The sink now states the blocker itself:
**`0x205` = `0x00`, `RECEIVE_PORT_0_STATUS` CLEAR — no sync on the main
link** — while **`0x204` bit 7 `LINK_STATUS_UPDATED` is SET**, so it noticed
something. **NEW AND UNEXPLAINED: `DP_VID_M` on DIG2 WILL NOT TAKE A STORE**
(reads 0 after 9011) while `DP_VID_N` in the same loop, instance and mask
shape takes `0x8000`; the held-FIFO hypothesis is **dead** (in-boot A/B, both
states). **The DIG1/DIG3 control FAILED and that is the finding**: they retain
no stores at all (`0x8000` reads back `0x2000`, the ENABLE bit does not
survive to the next read), so they do not isolate the variable (rule 39) — but
with DIG0 live that is the first **per-instance power discrimination** here.
The kext now publishes **`RBFBStrmW<n>Was`/`Want`/`Got`** per write so a
dropped store is read directly, not inferred from a changed mask. Framebuffer
UUID **27BF9539**, not installed.

**306 KILLED FOUR CANDIDATES WITH ZERO BOOTS AND BUILT THE PHY'S OWN STATUS
READ.** Spent the live boot before asking for another.
**`enc1_configure_encoder` is NOT the gap** — 2E5 did it (`rb_enc.h`,
`-rbgpufbfe`) and boot 478's published survey shows **DIG2 already carries
`DP_CONFIG` 1 = the sink's 2 lanes** (panel 3 = 4 lanes, dark instances 0) with
a `DP_DPHY_SCRAM_CNTL` `0x01020010` byte-identical to the panel's, `EncRan` 0
so they are the firmware's. **The DPIA branch is dead on source grounds**
(`DISPLAY_ENDPOINT_USB4_DPIA` is USB4 tunnelling; plain DP alt mode takes the
atom table we already run). **THE RDPCSPIPE INSTANCE IS SETTLED BY
ELIMINATION**: `dcn31_resource.c:404` is `link_regs(id, phyid)` with
`DPCS_DCN31_REG_LIST(**id**)`, DIG2 is id 2, the header aliases index 2 onto
`0x2d73` = **instance 0**, and the other direction would make UNIPHY_C read
DISABLED+acked and contradict ROM-attested `QUERY_DP_ALT`. **DP alt is not
disabled on our PHY, from two sources.** (My own arm scan lied first: a
six-token grep said `-rbgpufbfe` absent when seventeen are present — rule 26.)

**`src/common/rb_dpcsphy.h` — the PHY status, READ-ONLY, no setter**, sharing
`-rbgpufbdpalt` with **no new token**. Five registers x five instances,
BASE_IDX 2, **stride 0xd8** derived from two header offsets, all 25 offsets
script-verified against `dpcs_4_2_0_offset.h`: `RDPCSTX_CNTL` `0x2930`,
`CLOCK_CNTL` `0x2931`, `INTERRUPT_CONTROL` `0x2932`, `RDPCS_TX_SRAM_CNTL`
`0x2936`, `PHY_CNTL0` `0x2940`. **The file's subject is WHO ANSWERS EACH BIT**
— **ANSWERED** masks are the readings, **DRIVEN** masks say only that software
wrote them, and the hosttest proves the sets disjoint **at runtime**. Witnesses:
`PHY_CNTL0` `0x02000000` **REF_CLKDET_RESULT**, `0x10000000`
**SRAM_INIT_DONE**, `CLOCK_CNTL`'s three **CLOCK_ON** bits, `INTERRUPT`'s
sticky `0xf0` **TX FIFO_ERROR**. The clkdet result carries its own licence
(`rb_dpcsphy_clkdet_valid()` — a 0 with the detector off means nothing).
`tools/phylook.sh`. **Twelve break-it guards, all caught by a failing test,
none by `-Werror` — after three rounds that each found a defect in MY OWN
checks**: breaking the verdict's CLOCK_ON term **fired nothing** (it carried
no information, so those bits are now published raw); emptying one mask
**fired nothing** (the check tested a group's OR, the same defect one level
up); and the ANSWERED/DRIVEN overlap check **was never coverage** —
constant-folded, so `-Werror` killed the build instead of the test failing,
**the `-Werror` trap for the sixth time**. Also new:
**`RBFBStrmW<n>Was`/`Want`/`Got` per write**, so boot 478's `DP_VID_M` finding
is read directly rather than inferred. Framebuffer UUID **99122618**.

**302 built the stream encoder READ** — `src/common/rb_strenc.h`, 14 registers
x 5 instances, BASE_IDX 2, stride 0x100. **The read is the experiment: DIG0 is
the panel's live working DP stream**, so every field has a known-good value in
the same sample as DIG2's and the three never brought up are the negative
control. **The witness this line has lacked since 2EB:**
`DP_VID_STREAM_CNTL` bit 0 `ENABLE` (what a driver writes) vs bit 16 `STATUS`
(what the **hardware** answers, and what upstream `REG_WAIT`s on) — published
as two masks, never one. Four files for one operation
(`dcn31_resource.c:1323` construct DCN30 over a **DCN10** register struct;
`.dp_set_stream_attribute`/`.dp_unblank` are **DCN20's**;
`.dig_connect_to_otg` **DCN10's**), all vendored.
**And `setup_dio_stream_encoder` is NOT the gap:** its `enable_stream` /
`map_stream_to_link` / `set_input_mode` / `enable_fifo` are all **absent**
from `dcn30_str_enc_funcs`, so it reduces to `connect_dig_be_to_fe`, done in
2EC.
**Order:** FE connect -> configure encoder -> transmitter ENABLE; the atom
transmitter command carries no front end **by design**.
**2FC corrected the name:** `enc31_map_stream_to_link` is the **HPO DP**
(128b/132b) encoder's and our 8b/10b link never goes through it. This chip's
DIO stream encoder is `dcn30_dio_stream_encoder_construct` over DCN10's
register struct (`dcn31_resource.c:1323`, `:361`) — DCN30 code on DCN10
registers, 2FA's two-file split again. Our connect was
`dcn10_link_encoder_connect_dig_be_to_fe`, **done in 2EC**.
**2FC also built the mux read 459/461/464 never took:** `DIO_LINK{A..F}_CNTL`
(0x1f04..0x1f09, BASE_IDX 2, stride 1) selects which KIND of encoder drives
each link. `ENCODER_TYPE_DIG` is **zero**, so a zero word cannot be told from
unprogrammed, and the only finding available is a **non-zero** `ENC_TYPE_SEL`
on LINKC. `tools/diolook.sh`; read-only, no setter, and the caller that
programs it is **not** identified.

---

## Boot-args and dials

**The right boot-args are none of ours.** `src/common/rb_defaults.h` is the list
and the hosttest checks it — **read that file, not a copy of it.**
`sudo tools/efi.sh defaults` removes the redundant ones in one edit, keeps every
disarm and everything not ours (**`wdt=-1` above all** — Apple's arg, needed by
the default-on `-rbgpuforceenable`, and without it a frozen boot panics at 120 s
with no SSH window).

**Rescue, in order of bluntness:**
- `sudo nvram rbgpu-arm=0` — accelerator off next boot (and the framebuffer
  declines the panel with it, rule 38). Also `rbgpu-fb`, `rbgpu-agdc`,
  `rbgpu-iop`, `rbgpu-tick`, each `=0`. Re-arm with `sudo nvram -d <key>`.
- `-rbgpuno<feature>` — one feature off, derived from its own name.
- **`-rbgpustock`** — every default off, plugin otherwise alive.
- **`-rbgpuoff`** — the plugin loads and does nothing. The documented rescue.
- The **boot sentinel** needs no command: a boot that does not survive 60 s
  makes the next one stock.

**Opt-in, each for a named reason** (details in the journal under the tag):
`-rbgpuosforce` (arms the default table on an unmeasured OS),
`-rbgpufbmodes` (a second display mode; mutually exclusive with
`-rbgpufbdrr`), `-rbgpufbdpalt` (the RDPCSTX PHY read — read-only, but Linux
marks the path "avoid if possible", hang conditioned on DMCUB not running,
which is measured false; **2FC's `DIO_LINK*_CNTL` read shares this arm, and
`-rbgpufbdig` / `-rbgpufbfe`, with no new token**), `-rbgpufbdmubbl` / `-rbgpufbdmubdom` /
`-rbgpufbdig` / `-rbgpufbaltq` / `-rbgpufbfe` (the DMUB and DIG experiments —
each arms its own dependencies; **PHY0 / DIG0 / front end DIGA are the panel's
and are refused by name**. **`dmubbl dig` WRITES WITH NO ARGUMENT**, like
`dmubbl dto`: phyid defaults to **PHY2**, so a bare run typed to read the
usage performs the atom transmitter ENABLE — 307 did exactly that, `sends 2
ran 2`. This card claimed a "names a target first" gate for `dig` until 307;
there is none, and the two verbs that write unprompted are now both named
here), `-rbgpufbaux` (native AUX DPCD read **and** write; one token, because
`-rbgpufbauxw` would have it as a prefix — rule 30. Nothing is written until
`dpcdwrite` names an **address**, which has no writable default),
`-rbgpufbdto` (the DP DTO read **and** write, boot 455; opt-in because the
offsets collide with the DOMAIN block at this kext's base. **`dmubbl dto`
WRITES WITH NO ARGUMENT** — `khz` defaults to 148500, 2BD's preferred timing
(`tools/dmubbl.m:1791`), so unlike `dmubbl dig`/`dto`'s siblings there is no
"names a target first" gate here; this card claimed one until boot 475, where
a bare `dmubbl dto` run to read the usage programmed pipe 1. Pipe 0 is
refused by name in both halves, and
the write is declined unless THIS boot's sample reproduced the panel's pixel
clock at BASE_IDX 1), `-rbgpufbtrain` (the DP training pattern on the wire;
arms `-rbgpufbaux` itself, since its verdict comes from the sink. Nothing is
written until `dptrain go`; the panel's transmitter is refused by name and the
write is declined unless the panel's own DP0 reads as a trained link.
**`tps=1` is a no-op on a dark instance** and **`tps=3`/`tps=4`
DEGRADE the sink, costing a reboot — the tool refuses them. Use `tps=2`),
`-rbgpufbstrm` (**THE STREAM WRITE on DIG2**, 305 — two phases,
`strencprog go` then **train** then `strencprog unblank`; arms `-rbgpufbdpalt`
itself for the read the licence depends on, and that arm is applied AFTER
`dpaltArmed`'s own assignment because setting it earlier is setting it before
the line that overwrites it. Nothing is written until `strencprog go`; DIG0
and OTG0 are refused **by name** in both halves, and the write is declined
unless THIS call's own re-sample shows the MSA derivation reproducing the live
panel and the target pipe holding a raster. The phase defaults to the
configure half, which leaves the stream disabled so `dptrain` can run —
boot 477's refusal 66),
`-rbgpufbfsm` (**THE SET_PHY_FSM SEND on PHY2**, 310c, **run in boot 485 and
the firmware does not answer — 310d**; arms `-rbgpufbdpalt`
itself for the before/after survey, and that arm is applied AFTER
`dpaltArmed`'s own assignment, 305's trap. Nothing is written until
`RBFBFsmGo` names a **state**, which has **no default**; PHY0 is refused by
name twice and the send is declined unless this call's own re-sample shows the
live panel reading as a brought-up PHY. **`dmubbl fsm` and `fsm <state>` are
BUILD-ONLY and return before `findFB()` — only `dmubbl fsm go <state>`
sends**, deliberately unlike `dmubbl dig`/`dto`),
`-rbgpufbraster` (the second raster — OTG timing on pipe 1, 2FA. Nothing is
written until `otgprog ... go` names a mode, pipe 0 is refused by name in both
halves, and the write is declined unless this boot's own sample shows the pixel
clock running on that pipe — refusal 45 names it rather than arming
`-rbgpufbdto` silently. **NOT `-rbgpufbotg`**: `-rbgpufbotgany` exists and
would have it as a prefix, rule 30), `-rbgpuagdcearly` (**wedges the desktop**), `-rbgpufbmatch` (**kills the
accelerator**), `-rbgpumcreg` (failure mode is a **hang** — user's decision),
`-rbgpupdefix`, `-rbgpufbvramrange`, `-rbgpumcnew`, `-rbgpunodisppanic`,
`-rbgpusdmahook`, `-rbgpustampraw`, `-rbgpuregdump`, `-rbgpubeat`.

**Runtime dials** (`setProperties`, no boot needed):
`{"RBFBPubMs": N}` → `build/out/pubms`;
`{"RBFBFtOn": 0|1}` → `ftbudget` (frame-time budget; the write zeroes the
window); `{"RBFBNotReady": 0|1}` → `holdset` (**the 2xo hold — worth ~36 % of
the frame rate and traded against whole-frame flicker. Default ON. Do not flip
the default**); `{"RBFBTriple": 0|1}` → `tripleset` (the second pending flip
slot — **measured INERT interleaved in both regimes; stays off**);
`{"RBGPUTickUs": N}` → `tickset` (retire tick, 500..1000000 µs; **inert and
refused while the vblank drives**); `{"RBGPURetireSub": 1..8}` → `subret`
(retire occasions per vblank period — **the win: first paint 13.9 → 3.5 ms,
rate +94 % interleaved with the hold ON, and the user's own lag report
reproduced and controlled**).

**Tunables** (rule 40 — pass as numbers): `rbgpufbpubms=<0..10000>` (default
500), `rbgpuschedtickus=<500..1000000>` (**derived since 2D5**; 6944 is a
measured two-sided optimum at native — read 2zc before changing it),
`rbgpuretiresub=<1..8>` (**derived since 2D5**: `ceil(period / 2084000)`
clamped 1..8 → 4 at native, 8 at 60 Hz, both measured defaults out of one
constant), `rbgpuvblhz=<millihertz>`, `rbgpufbvrammb=`, `rbgpufbvramrangemb=`,
`rbgpufbhubpmax=`, `rbgpufbatymb=`, `rbgpufbblfloor=`, `rbgpumtlkscope=` /
`rbgpumtlkexclude=` (exclude wins), `rbprobe_level=`, `rbprobe_smu=1`.

Diag counters live in `ioreg -rl -c RembrandtGPUPeer`'s `RBGPUDiag`
(**decimal**; the NVRAM `rbgpu-diag` copy is hex and caps near 520 bytes, so
absence from one is not absence from the other).

---

## Closed — do not re-open without new evidence

Page flipping (`getDisplayPipeTransactionFlip` never called once); forcing
`UseIOPresentment` (72 call sites, selects the whole IOPresentment world); the
AGDC capability surface (every type eliminated by measurement); the plane-count
line (both directions); `System Gamma - Unknown` (never ours to answer); tiling
writes (the swizzle already agreed); `-rbgpuagdcearly` (wedges); **the
frame-rate ceiling** (eight models died; the answer was our own
`kIOReturnNotReady` hold, the flip is vsync-latched at 1.00 vblanks with a
distribution bimodal at the boundary, so there is no partial hold — and the real
lever turned out to be **retire occasions per vblank**, not the hold); **the
HPD→PHY register** (there is none — it comes from the VBIOS display-object
table, which we have on disk); **polling `checkTimestamps()` harder**
(`rbgpuretiredrain`, measured dead); **efficiency/power** (moved to
`~/Developer/Econized`).

**311 - WEBGL IS NOT MISSING; MSAA COLOUR SURFACE SIZING RETURNS 0.** Browser
pages are white because `getContext('webgl')` defaults to `antialias: true`.
Isolated to that one attribute (interleaved, five pairs, two sizes): `true` ->
default framebuffer `INCOMPLETE_ATTACHMENT`, every op **1286**; `false` ->
correct throughout. Below the browser, Metal claims
`supportsTextureSampleCount` 2/4/8 and returns **nil for all three**. The
matrix splits it: multisampled **colour** fails in four formats, two sample
counts, two sizes, array and not; multisampled **depth/stencil** (D32, D16,
D32S8, S8) all allocate. The failing call is
**`heapTextureSizeAndAlignWithDescriptor:`** - **size 0, align `0xD7739C01`**
against the control's 16384/4096 and MSAA depth's 65536/65536, i.e. an
**unwritten output struct** (rule 14 inside Apple's stack). **FMASK is a
CANDIDATE, not a mechanism** - the split is FMASK-versus-HTILE shaped, but no
FMASK path has been read and no configuration here lacks the symptom (rule
39). `tools/msaa/`; `dscinv.sh` does not accept
`AMDRadeonX6000MTLDriver` yet. Metal is otherwise unaffected - everything
measured on this chip runs single-sampled.

**Open:** the rest of the external-display chain — `docs/EXTERNAL-DISPLAY.md`
for the state, `docs/HANDOFF-NEXT.md` for the next step. **THE BLOCKER IS
NAMED: PHY2 IS HELD IN RESET** (307 — `PHY_RESET` 1 against the live panel's
0, no clock, no FIFO, firmware loaded), and **the atom transmitter ENABLE does
not release it**, in an in-boot A/B with a four-instance control. Everything
downstream — no clock recovery, DPCD `0x202` `0x00`, `0x205` bit 0 clear,
`DP_VID_M` refusing a store — follows from that. The open question is no
longer a register on our side: the driver has **no** PHY bringup on DCN31, the
VBIOS table is the only mechanism, our payload is verified field by field, and
`ran` is only a mailbox ack. **310 found the one command into the FSM
(`SET_PHY_FSM` = 29) and 310d SENT IT in boot 485 — the firmware does not
answer.** Four sends, states 2 / 3 / 10 / 2, `refusal 0` each: the reply slot
came back **bit-for-bit as we left it**, PHY2 moved **nothing**, all four
controls held still — while the ring **drained** and `QUERY_DP_ALT` on the
same ring in the same boot returned a **ROM MATCH**. So **DMCUB 4.0.15.47
does not act on sub-type 29**, and the execution evidence this line needed has
now been asked for with the one command that could give it. **Every candidate
on our side of the mailbox is dead**: 308's SRAM handshake, 309's ownership
registers, 309b's survey as a method (nine registers cannot tell PHY2 from
PHY3 while `QUERY_DP_ALT` can), and now the FSM command itself. The CR window
is **not a read** (310) and is deprioritised. **What is left is not a register
and not a command we can name**: DMCUB's own image (4.0.15.47, CW0/CW1 — it
services both the command that replies and the one that does not, and it is
the only party that has ever moved this PHY), or evidence from a machine where
this PHY *has* been brought up. Both are a different kind of work from
anything 300-310 did, and neither has a boot spent on it.
The things never attempted are a stream on a **trained** DIG2 link, a
**surface** on pipe 1 and a second `IOFramebuffer`. 2F8 added a cost: `tps=3`
**degrades the sink until a reboot**, which is what 2F7 spent a milestone
diagnosing. Also open: the **wedge** (work submitted that
never retires — channel-52 GFX, 3.5 s, the "random lag"; `stamp_ticks` 125666
retires the "nothing polls it" family); `powermetrics` has **never been run**
against the default 576 interior thread_calls/s, the one real argument against
the 2D5 default; Ventura and Sonoma claim **nothing** in `rb_osver.h`.
