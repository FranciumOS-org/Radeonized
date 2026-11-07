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
  `rb_digbe.h`, `rb_dpalt.h`, `rb_dpdto.h`, `rb_enc.h`, `rb_drr.h`, `rb_frametime.h`,
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
- The panel: 1920x1080, `H_TOTAL` 2106, `V_TOTAL` 1129, **143.83 Hz**, pixel
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

### The external display path (Type-C DP alt mode), as measured

Port = **HPD instance 2** = **AUX engine 2** = **UNIPHY_C / phyid 2** = **DIG2**
(four independent instruments agree: HPD sink bit following the cable,
`AUX_CONTROL.AUX_HPD_SEL`, `DIG_BE_CNTL.DIG_HPD_SELECT`, and the VBIOS display
-object table in our own `firmware/680m-vbios.rom`). Sink: **MSI MP2412C**, DP
1.2, HBR2, **2 lanes**, EDID read over I2C-over-AUX, native **100 Hz** (fits the
~8.64 Gbps link). DP alt mode **is granted** (DMUB `QUERY_DP_ALT`, matching the
ROM bit for bit on all five phys; `RDPCSTX2_PHY_CNTL6` disagrees and is the
legacy fallback Linux only reads when DMCUB is absent). **The firmware executes
`DMUB_CMD__VBIOS` commands** (`DIG_BE_EN_CNTL.DIG_ENABLE` 0→1 on the targeted
instance alone, two boots) **and answers them** (the reply path works at
REGION4; `ret_status` stays SET, which Linux's own criterion would discard).
The firmware configured that encoder (`DP_CONFIG` = 2 lanes on DIG2, 4 on the
panel's DIG0, 0 on the three never brought up). Remaining: the **pixel clock**, which on a DP
link is **three register writes of ours** and not the VBIOS command (2EE read
the caller: `dcn31_program_pix_clk` programs `DP_DTO*_PHASE`/`MODULO`/
`PIXEL_RATE_CNTL` itself and reaches `set_pixel_clock` only for HDMI/DVI).
Those registers are **BASE_IDX 1 = 0xc0**, whose offsets collide with the
DOMAIN power-gating block at BASE_IDX 2 — and boots 452 and 453 both
**CONFIRMED** the base on the chip (2F0/2F1, so rule 44 is satisfied for it), with the alias control reading `DOMAIN0/1_PG_STATUS` as
`0x10000000` there. From that boot: **`dprefclk` = 600 MHz exactly**
(`MODULO[0]`, never located before); the panel's DTO does drive the panel
(enable and status both set); pipes 1-3 are dark and byte-identical. Two
cautions for the write: `PIPE0_DTO_SRC_SEL` is **0** where Linux writes 1 and
the panel works anyway, and **PHASE is the requested clock, not a readback**
(343.000000 MHz against a raster wanting 342.288, DRR ruled out) so it cannot
be the verdict register. **THE PIXEL CLOCK IS RUNNING** (2F1 built the writes, boot 455 ran them).
Pipe 1's `OTG1_PIXEL_RATE_CNTL` went `0x8000c000` → **`0x8000d090`**: our RMW
(`DP_DTO_ENABLE` + `PIPE_DTO_SRC_SEL`, `DTBCLK_DTO_DIV` 8 and
`DIO_FIFO_ERROR` 3 carried through) **plus exactly one bit we do not write,
`DPDTO_ENABLE_STATUS`**, whose two states 2F0 attested here first in two boots.
PHASE 148 500 000 (the sink's own preferred timing), MODULO **copied off
pipe 0** — PHASE/MODULO is a ratio, so the true reference never has to be
known. The in-boot control is better than the write path's readback: in the
same periodic sample pipes 2-3 still hold the dark word **byte-identical**
with PHASE/MODULO 0, and the panel's pipe 0 is untouched, so the change is
confined to the pipe the dial named. The panel kept scanning, which discharges
the DOMAIN-collision safety case by observation. **It is a clock, NOT a
picture** — no OTG timing on that pipe, no trained link, no stream mapped, no
surface. `SRC_SEL` is Linux's 1 where the firmware left pipe 0's at 0; what it
selects is still not established and the `srcsel=0` dial now needs a reboot
first, because nothing turns a live DTO off. A second `dmubbl dto` is refused
**45** by the already-live guard, which is also an independent reading of the
status bit. 2F2 split the published fields in two (`dtoCall*` = this call,
`dtoProg*` = the last call that WROTE) because 2F1's single set let that
refusal republish the successful write as `verdict -1 NOT RUN`.
Then link training over AUX2 (driver-side — zero `dmub`
references in `link_dp_training*.c`), `enc31_map_stream_to_link`, a second OTG
and HUBP, and a second `IOFramebuffer` (`getDisplayModeCount()` has returned 1
since 2sl). **Order matters:** FE connect → configure encoder → transmitter
ENABLE; the atom transmitter command carries no front end **by design**.

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
which is measured false), `-rbgpufbdmubbl` / `-rbgpufbdmubdom` /
`-rbgpufbdig` / `-rbgpufbaltq` / `-rbgpufbfe` (the DMUB and DIG experiments —
each arms its own dependencies and sends nothing until `build/out/dmubbl` names
a target; **PHY0 / DIG0 / front end DIGA are the panel's and are refused by
name**), `-rbgpufbaux` (native AUX DPCD read),
`-rbgpufbdto` (2EF's DP DTO read and 2F1's write, **which boot 455 ran** -
opt-in because the
offsets collide with the DOMAIN block at the base this kext holds. No second
token was added: `-rbgpufbdtoprog` would have this one as a prefix, the shape
rule 30 forbids. Even armed **nothing is written until `dmubbl dto` names a
clock**, pipe 0 is refused by name in both halves, and the write is declined
unless THIS boot's own sample reproduced the panel's pixel clock at BASE_IDX 1),
`-rbgpuagdcearly` (**wedges the desktop**), `-rbgpufbmatch` (**kills the
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

**Open:** the rest of the external-display chain above - the pixel clock is
DONE (boot 455, `DPDTO_ENABLE_STATUS` 0 → 1 on pipe 1), and link training over
AUX2 is next;
the **wedge** (work submitted that never retires — channel-52 GFX, 3.5 s, the
"random lag"; `stamp_ticks` 125666 retires the "nothing polls it" family);
`powermetrics` has **never been run** against the default 576 interior
thread_calls/s, which is the one real argument against the 2D5 default;
Ventura and Sonoma claim **nothing** in `rb_osver.h`.
