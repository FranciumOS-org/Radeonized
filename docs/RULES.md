# RembrandtGPU — notes for the assistant

> **Read `docs/HANDOFF-NEXT.md` next** — current state and what to do now.
> `docs/JOURNAL.md` (~48 000 lines) is the chronological evidence record:
> authoritative for what was measured, in order, including every withdrawn
> reading, and **never to be trimmed**. Grep it by milestone tag (`2ks`, `2v8`,
> `2xw`); never read it whole.
>
> **This file is a reference card and must stay one.** It was 2 800 lines of
> milestone narrative twice over; both times everything in it was already in
> the journal under its tag. Put findings in the journal. Nothing is ever
> deleted here, only moved — and git history holds every earlier version,
> including the removed `docs/HANDOFF.md` and `docs/HANDOFF-FULL.md`.

Goal: graphics acceleration for the Radeon 680M (Rembrandt, `1002:1681` rev 0a,
GC 10.3.3 / DCN 3.1.2 / PSP+SMU 13.0.x / SDMA 5.2.3) on macOS **Ventura and
up** (2xz: `src/common/rb_osver.h` says what that claims per version, which is
not the same thing for every version). Plan: `docs/PLAN.md`. Machine: ASUS TUF A15 FA507NU (Ryzen 7 7735HS), **macOS 26.7.2
(25G332, Darwin 25.6.0)**, OpenCore. csr-active-config **0xA43** (SIP reports
Filesystem Protections disabled, yet the dyld cryptex path still refuses writes
with EROFS because its files carry the `restricted` flag, enforced
independently of the CSR bits).

## Where this stands

**It works.** An armed boot is an accelerated Mac: Metal compute bit-exact,
System Profiler reads `AMD Radeon 680M` / 4 GB, our own `RBAMDFramebuffer`
owns the panel, Apple's `AMDRadeonX6000` composites for it, SkyLight logs
`Metal compositor activated`, and the desktop runs at ~82 transactions/s with
no flicker and no tearing. USB runs on real interrupts. The remaining asks are
small and named below.

**A "0" our own old builds wrote is not a disarm (2xx).** Two of the five
keys were consumed by *overwrite* before 2xw — the way a one-shot was spent
was to write a literal `"0"` into it — which is exactly the value 2xw's reader
treats as the user's durable disarm. So the first 2xw boot came up with the
scheduler tick off and the panel declined, and the desktop went back to one
frame per 10 s: boot 236's own record reads `tick_armed=0 tick_oneshot=3`. The
value cannot be told from a hand-typed `=0`, so they are told apart by
generation: `rbgpu-gen` is written once, before it exists a `"0"` in
`rbgpu-tick` or `rbgpu-fb` is ignored, after it exists every `"0"` is obeyed.
`rb_key_disarms()` in `rb_defaults.h` is the rule and both kexts ask it, and
`rbgpu_nvram_consume_oneshot_ow` is **deleted** so nothing can write that
value again. The other three keys were consumed by removal and were never
ambiguous. Rule 37, asked the other way round: **what does a stale value our
own earlier build wrote look like to the new reader?**

**Arming is now the default (2xw).** Empty boot-args give the configuration
boot 231 ran: `src/common/rb_defaults.h` holds those 45 tokens and everything
in it is on. The five NVRAM one-shots are gone in that direction —
`nvram rbgpu-arm=0` / `-agdc` / `-iop` / `-tick` / `-fb` are durable disarms
the plugin never writes. The safety net is the **boot sentinel**: `rbgpu-safe`
is set before anything is armed and cleared from a thread_call 60 s later, so a
boot that panics, hangs or is power-cycled inside that minute leaves it set,
and the next boot comes up stock and clears it. One bad boot costs one stock
boot. `-rbgpunosentinel` opts out; a deliberate restart inside the minute also
costs one stock boot.

**Which macOS, and what has been measured on it (2xz).** "Supports Ventura and
up" is two claims and `src/common/rb_osver.h` keeps them apart, because
conflating them is how a wrong address gets written to a chip. **(1)** The kext
LOADS from Darwin 22 up: everything done by symbol name is version-independent
and rule 1 already makes each of those fail safe. **(2)** A version-dependent
artifact has been MEASURED on a given OS - per OS, per artifact, recorded as a
bit, and never inferable from a version number. Sequoia claims the object
offsets (derived there) and a cave row; Tahoe claims both plus the USB table -
the offsets not because they were re-derived but because **boot 247 is an
accelerated desktop running them**. Ventura and Sonoma claim **nothing**, which
is the honest state. On an OS with no `RB_OSD_KEXTOFF` claim `rbgpu_stock()` is
true, so the whole default-on table comes up off and the boot says so with the
Darwin number in it; `-rbgpuosforce` arms it for the boot that goes and
measures. Both kexts read `version_major` and ask the same function (rule 38).
`make osrange` fails the build if the declared Lilu range drifts from the
matrix - it was Monterey..Sequoia on a Tahoe machine, loading only because
`-lilubetaall` is in boot-args. Every boot publishes `os_major`, `os_derived`,
`os_forced`, `os_measured`, and `rbgpu_nvram_armed` why **9** is "no measured
offsets for this OS", distinct from 5's "the user asked for stock".

**The cave is a table, and the generator derives it (2xz).**
`src/plugin/mtlcave/darwin<N>.h` holds one row per OS build;
`rbgpu_mtlcave.h` is the table and the lookup. A row says where to LOOK - what
authorises a write is still the byte-pattern compare in every process, so a row
right for the major and wrong for the point release writes nothing and 2r6's
verdict fires. `make mtlcave` derives this machine's row off its own cache
(symbol, prologue uniqueness, a unique zero-run anchor; the mangled length
prefix computed, rule 5) and names the two edits left by hand. It reproduced
2xt's by-hand derivation byte for byte. `make cavecheck` reads the row for its
own kernel and said CAVE+DETOUR PRESENT on the live boot 247 desktop.

**How the display half got here**, each step measured and in the journal:
`RBAMDFramebuffer` subclasses `IOFramebuffer` and displaces
`IONDRVFramebuffer` (2sl/2sm/2st) — its class name carrying "AMD" is what
opens CoreDisplay's IOPresentment vendor gate (2ql/2t7). `ATY,fb_offset` and
`ATY,fb_size` open `initFramebufferResource` (2w9/2wa). Answering the `'amtr'`
framebuffer attribute is the gate on every display-pipe transaction (2wc/2wd),
and `setAttribute('amtr')` hands us the `_AMD_PIPE_FLIP` carrying the
compositor's surface address (2wf/2wg). Writing that address to HUBP0's
primary-surface pair is what puts the composited desktop on the panel
(2wh/2wi). `-rbgpuschedtick` ticking Apple's `AMDSWScheduler::checkTimestamps()`
at the panel's 6944 µs is what makes frames retire (2xe/2xi) — 2 transactions
in 30 s became 3375. Answering `kIOReturnNotReady` while our flip is unlatched
is what fixed the flicker, at 27 % of the frame rate (2xo).

**The swizzle fix has two halves and both are needed**: the kernel linear force
and the kernel-side userspace patcher (2on–2op). macOS 26 broke the userspace
half twice — the driver moved and was rebuilt (2xt, re-derived in
`src/plugin/rbgpu_mtlcave.h`), and the exec-time hook cannot read the target's
map at all (2xu: `mtlk_probe_read` 1 = KERN_INVALID_ADDRESS, 0 of 1536
patched). The fix is to place the patch from the client's own process instead,
at `IOGraphicsAccelerator2::newUserClient`, and boot 231 measured it working:
`mtlk2_uc_hk 1`, `mtlk2_uc 196`, **`mtlk2_patched 43`**, zero failures. Both
`mtlk_syms` meanings matter: **63 = 0x3F is "all resolved" now**, not 15.

**USB: real interrupts, not a 10 ms poll (2xv).** The five xHCI controllers
expose one MSI vector each, so only interrupter 0 can interrupt;
`AppleUSBXHCI` targets interrupters 1 and 2 for transfer events. Nine OpenCore
patches zero the Interrupter Target field. `tools/xhci_intr0.py` holds the
table and refuses to install one that does not match the running kernel;
`tools/re/xhci_intr0_derive.py` re-derives it after an OS update.

**Open, small, and each one named:**
- **Brightness.** Both halves are built and neither has been booted.
  `-rbgpufbbacklight` writes the PWM duty cycle (2xs); `-rbgpufbbuiltin`
  answers `kConnectionFlags` with `kIOConnectionBuiltIn` and `getAppleSense`
  with `kPanelTFTConnect`, the two things `IOBacklightDisplay::probe` requires
  before `AppleBacklightDisplay` can match the connection and System Settings
  can draw the slider (2xy). Separately armed on purpose: the match reaches the
  display-prefs key, mirroring and the wrangler's dimming, the write only
  reaches a duty cycle. `RBFBBlBuiltIn` / `RBFBBlFlagsGets` / `RBFBBlSenseGets`
  tell "the probe never asked" from "it asked and still did not match"; the
  outcome is `ioreg -rl -c IODisplayConnect`.
- **The last ~22 % of the frame rate.** 82/s against the panel's 143.83; not
  established whether that is demand-limited or still gated (2xi/2xo).
- **The wedge**, an independent second defect: work submitted that never
  retires, on GFX/SDMA/compute user queues. `metal_probe --queues 4` reproduces
  it in userspace with no kext and no reboot (2wt). Not the display blocker
  (2x0, rule 39).
- **HDMI is the full project** and the panel is wired to the dGPU, so it is
  unreachable rather than merely unbuilt (2xq/2xr).
- **Every Apple address in a COMMENT here and in the journal is a macOS 15
  address.** `AMDRadeonX6000MTLDriver __TEXT` 0x7FFB08BF3000 →
  **0x7FFB110D4000**; CoreDisplay 0x7FF80404C000 → **0x7FF805348000**;
  SkyLight 0x7FF807B42000 → **0x7FF8091F1000**. Re-derive before any static
  read: **`tools/re/dscbase.sh <Lib>`** reads the live unslid `__TEXT` base and
  size out of `dyld_info -segments`, and dscsym.sh / dscinv.sh take their bases
  from it instead of a table (2xz - the table was stale and both tools reported
  confidently anyway). In LIVE code the only hardcoded Apple address left is
  `RBGPU_IOP_SITE_VM`, stale and failing safe (2t7 won that gate unpatched, so
  it is not load-bearing); everything else is a symbol lookup or a cave-table
  row.
- **ASan is broken on CLT 16.4 + macOS 26**: a two-line `printf` program
  SIGILLs. The Makefile probes it and degrades loudly (`SAN=auto`); every
  assertion passes unsanitized. Never commit a change whose only passing run
  was unsanitized. Rule 16.

**Closed — do not re-open:** page flipping without new evidence (2pf: with
`-rbgpuflipprobe` routed, `getDisplayPipeTransactionFlip` was never called
once); forcing `UseIOPresentment` (2ql: 72 call sites, it selects the whole
IOPresentment world); the AGDC capability surface, every type eliminated by
measurement (2sd/2v6/2w1); the plane-count line (2u3/2v9 both directions);
`System Gamma - Unknown`, which was never ours to answer (2v8); tiling writes
(2wi: the swizzle already agreed); `-rbgpuagdcearly`, which wedges (2rz).

## Layout

- `src/common/` — freestanding C shared by the kext, the plugin and the host
  tests: IP discovery (`rb_discovery.c`), read-only probe (`rb_probe.c`),
  registers (`rb_regs.h`, every offset cites its Linux header), SMU
  (`rb_smu.c`, `rb_smu13.c`), OTG timing (`rb_otg.h`), AGDC (`rb_agdc.h`),
  HUBP decisions (`rb_hubp.h`), backlight (`rb_backlight.h`), **the default-on
  token table (`rb_defaults.h`)**. Pure logic lives here so the hosttest can
  decide everything the kext decides.
- `src/kext/` — `RembrandtProbe`, read-only, publishes `RBProbe*` and unmaps.
- `src/fbkext/` — `RBAMDFramebuffer`, the `IOFramebuffer` for the firmware-lit
  panel: delivers `'vbl '` from a `thread_call` at the measured refresh,
  answers `'amtr'`, writes HUBP0's surface address per flip. `make fbkext`;
  **installed by the user** into the auxiliary collection.
- `src/plugin/` — the Lilu plugin: `kern_start.cpp` (hooks, probes, the
  swizzle patcher), `RembrandtGPUPeer.cpp` (the IOService peer, the diag
  table, the NVRAM hot keys, the default-on gate and the sentinel),
  `rbgpu_agdc.cpp` (the AGDC bridge), `rbgpu_agdc_otg.cpp` (the read-only BAR5
  OTG sampler). Boots from the OpenCore EFI.
- `tools/hosttest/` — the probe against a simulated chip plus parser fuzzing.
  `make kext` runs it first; never load a kext whose hosttest fails. Add a case
  whenever `src/common` changes, and check the new case fails when the code it
  guards is broken.
- `tools/re/` — static analysis. Kexts: `kc_extract.py`, `machoread.py`,
  `dumptable.py` (the instrument that works on a KC vtable), `kc_stubs.py`,
  `vtslots.py`, `strxref.py`, `switches.py`, `ptrsym.py`, `vcall.py`,
  `fldstore.py`. dyld cache: `dsc.py`, `dyldcache.py`, `dscinv.sh` (build this
  first — an `atos` grid names every function in seconds), `dscsym.sh`,
  `dscdis.sh`, `ripxref.py`. Output to `build/re/`, findings in
  `docs/APPLE-STACK.md`.
- `tools/xhci_intr0.py`, `tools/re/xhci_intr0_derive.py` — the USB
  interrupter-0 patches (2xv), tested by `tools/xhci_intr0_test.py`.
- `tools/earlybench.sh` — a `RunAtLoad` daemon that benches the GPU in the
  +0..+20 s window no terminal reaches. ONE queue, never `--queues` (a known
  wedge trigger). The user installs it.
- `docs/linux-ref/` — unmodified Linux files the code follows; `UPSTREAM.md`
  records each commit.

## Workflow

- `make kext tools`, `make fbkext`, and **`make plugin` for the Lilu plugin —
  `make kext tools` does NOT rebuild it.** CommandLineTools only, no Xcode.
- **Always check the UUID moved** (`dwarfdump --uuid build/out/...`). Two boots
  were lost to a stale build.
- After any plugin change: `kmutil libraries -p build/out/RembrandtGPU.kext`
  must resolve everything with **0 from `SystemKernelExtensions.kc`** (the 2oz
  trap) and `grep -c IOGraphicsFamily src/plugin/Info.plist` must be **0**.
  The plugin is injected into `BootKernelExtensions.kc`, where a system-KC
  dependency cannot bind. **`src/fbkext` is the opposite** — loaded from the
  auxiliary collection, it *requires* `IOGraphicsFamily` out of
  `SystemKernelExtensions.kc`, and `make fbkext` fails if it is absent (2sl).
- Probe rules: no writes except `MM_INDEX`/`MM_INDEX_HI` (enforced in
  `rb_probe.c`, asserted by the hosttest), never enable PCI decoding or bus
  mastering, never map BAR0's first 16 MB, read only inside the carve-out.
- SMU rules (`make kext SMU=1`): writes only to MP1 C2PMSG_66/82/90 of an MP1
  13.x, only allowlisted query messages, nothing sent unless the response
  register reads 1, stop at the first non-OK answer.
- Register gates follow Linux's **version grouping**, not the exact version:
  this chip reports MMHUB 2.4.0 (`mmhub_v2_3`) and DMU 3.1.3
  (`DCN_VERSION_3_1`, `dcn_3_1_2`). Check the grouping before adding one.
- Captures land in `logs/<timestamp>-plugin/`.

## Hard rules

- **Always ask before loading or unloading a kext.** A bad load can hang or
  panic the machine and ends the session.
- The user runs `sudo tools/load.sh`, `sudo tools/unload.sh`,
  `sudo tools/fbload.sh`, `sudo tools/efi.sh …` and `tools/plugin_check.sh`.
  **the assistant never runs these and never asks for the password.**
- **Never install into the EFI or `/Library/Extensions`.** the assistant builds and
  hands over the commands.
- **Never run `kmutil unload` on the framebuffer kext — it PANICS** (2sq):
  `IOFramebuffer::stop+0x48` reads `*(this->0x1c8)`, NULL on an instance that
  never completed configuration. To remove it, reboot.
- Before any load, **commit** so a hang loses nothing. the assistant may commit at any
  time without asking (2026-10-04), and **pushing no longer needs asking
  either** (user, 2026-10-30: "also push whenever u need").
- **Deliberate diagnostic panics are off the table** (user's decision,
  2026-10-12).
- **Adding an SMU message group is the user's decision** — say what it changes
  on the chip.
- **NootedRed and NootRX are under the ChefKiss Open Knowledge License v1,
  whose §2 and §3 forbid AI reading, summarizing, analyzing or deriving output
  from the work. Do not read or analyze them.** Findings from those projects
  must come from the user in their own words.
- **Note for any session in the the assistant app:** it runs with
  `DYLD_INSERT_LIBRARIES` set and every shell inherits it. Launch anything with
  `env -u DYLD_INSERT_LIBRARIES` or it starts under whatever patch this the assistant
  is running (2ns).

## Rules that cost boots to learn

Each was paid for on hardware; the tag in brackets is the journal's evidence.
The numbering is cited all over the journal — **keep the numbers**.

**Routing Apple's code**

1. Never route a name unless `nm -m` shows it **defined** there; `solveSymbol`
   returns the bare slide for an import and `routeMultiple` writes a jump
   there. [2c]
2. Refuse a target whose first five bytes contain a rel8/rel32 branch — Lilu
   copies displaced instructions byte for byte. [2gb, 2xu]
3. Grep the mangled symbol across the plugin first: a second `RouteRequest`
   for a routed function **panics**. A new transform goes inside the existing
   hook. [2kn]
4. **`RouteRequest::from` is NOT written back** by Lilu 1.7.3 though the header
   says so. Call `solveSymbol` yourself. [2kp]
5. Cross-check hand-written mangled names against `nm` — a wrong length prefix
   silently unscopes the fix. [2kr, 2s2]
6. When a fix moves upstream, **remove the downstream patch in the same
   change**. "Redundant" and "inert" differ. [2kv]
7. vtable dumps: **call offset = dump offset − 0x10**; a non-virtual call
   through a vtable *symbol* uses dump offsets directly. [2gp]

**Reading a boot**

8. **Read `rbgpu-boot` first.** No increase means the boot was never armed and
   nothing in NVRAM is this boot's. Boot numbers are the nonce or nothing.
   [2iz, 2wl]
9. One-shots are one-shot — **and since 2xw there are none left to arm**;
   `rbgpu-*=0` is a durable disarm and the sentinel is the recovery. The
   surviving lesson: a feature needs a boot-arg *and* a key only when a stuck
   key must be harmless. And when you invert what a key MEANS, ask what values
   your own earlier builds left in it — `rbgpu-tick` and `rbgpu-fb` held the
   `"0"` the old consume-by-overwrite path wrote, and the inversion read it as
   the user's disarm. [2iz, 2qy, 2xw, 2xx]
10. **Capture in the armed boot, over SSH, before rebooting.** What must
    survive an uncapturable boot has to be in NVRAM and **periodic**, not a set
    of fixed marks. [2jg, 2gw, 2s5, 2wl]
11. A note is evidence only when its `b=` nonce is new. [2gd]
12. A stale note says nothing about where a boot got to. A user rebooting a
    black screen looks exactly like a hang. [2gw]
13. **A return code is not a result until the code that sets it has been
    read** — request 0x1B's *success* path returns `kIOReturnUnsupported`.
    [2ha, 2s2]
14. **Absence is not a result.** A silent return, a full diag table and a
    missing write look identical. Every path out of a probe must say
    something; check `diag_used` against `diag_max` first; a last-wins key
    cannot express a per-entry fact; and the absence of a log means nothing
    until **every** exit has been enumerated. [2eh, 2ds, 2qw, 2s6, 2w7]
15. Check the loaded UUID, the keys and the boot-args **for every artifact the
    boot loads** — the plugin from the EFI, the framebuffer from
    `/Library/Extensions`. A rebuild refreshes neither. A key the new code
    publishes *unconditionally* is the cheapest proof the new binary is
    running. **2xx is the expensive case: nine boots, a WindowServer crash
    loop and a nine-stage boot-arg bisect, all because the plugin was
    reinstalled every time and the framebuffer never was.** Every "cleared"
    result in that bisect was taken against a two-milestone-old second binary.
    `kextstat | grep rembrandt` against the UUIDs the build printed is the
    first reading of any boot, not an afterthought. [2cu, 2w6, 2wa, 2xx]

**Instrumenting**

16. **The instrument is a suspect.** It has cost a wedge, a crawl mistaken for
    a hang, two panics, three destroyed captures and a build that looked like a
    failing test (ASan). When a measurement and a suspect are both new, remove
    the suspect rather than reasoning about it. [2fo, 2ez, 2rh, 2xt]
17. **Never substitute a value and then narrate it as a hardware result.**
    [2cy, which cost fifteen milestones]
18. Do not build a table from n=1 rows, and two numbers agreeing to one figure
    is not an identification. [2ev, 2wt]
19. A NULL check is not a pointer check, and use the right floor —
    `0xffffff8000000000` for a kalloc'd object, **`0xffffff7f80000000`** for a
    kext symbol. A refusal path must publish the value it refused, and a
    captured pointer needs a captured control pointer in the same boot. [2gh,
    2qu, 2s6]
20. A registration that reports success is not evidence that a patch applied.
    Log the enabling field itself. [2lq]
21. **Never put `DYLD_INSERT_LIBRARIES` in a launchd environment**, and test a
    recovery procedure in the situation it exists for. Inject per process.
    [2mq]
22. **Lilu's user patcher does not run on this OS at all** —
    `isUserDisabled = … || getKernelVersion() >= BigSur`. Check a subsystem is
    *enabled* before reading how it behaves. Verify a patch from the target
    process's own memory. [2nn, 2of]
23. `pgrep` cannot see the process a script runs inside, or its ancestors; use
    `ps -axo pid=,command=` with a prefix match. [2nu]
24. The dyld shared cache is inside a grafted, Apple-signed cryptex and cannot
    be modified. [2ny]
25. **Read the machine, not the manual.** Four Recovery trips died on things
    asserted from general macOS knowledge. [2ny]
26. **A search is an instrument too** — a grep returning nothing proves nothing
    until checked against a known-present case. Paid for in: `objdump`'s tab
    separator; `strings -a` under-reporting on a 373 MB KC; `otool -tV`
    resolving names so a numeric search misses; a tail `jmp` invisible to a
    `callq *0x…(` grep; `ioreg -c X -k Y` not filtering (use `ioreg -rl -c X`);
    a tool's exit code read as its verdict, and the mirror — a loud error over
    a write that landed; **`timeout` does not exist on macOS**; a word the
    producing code never contains (`Clean`, five boots); a constant the
    assembler built by **arithmetic**; a `sort -u` read as a count; and **a key
    name that is a prefix of another key's name** (`rbgpu-diag` matching
    `rbgpu-diag-prev`). Anchor the match or read the value, never the file; and
    xref a string's **start**. [2v8, 2wc, 2wg, 2v9]
27. **No vnode work at plugin init** — the root filesystem is not mounted yet.
    `-rbgpuoff` stops `init()` entirely, so such a boot is not a control for
    it. [2oe]
28. Attribute every log line to a boot by timestamp **and** pid; pids are
    reused, so the reliable separator is the `Darwin Kernel Version` banner.
    `log` is a zsh builtin — use `/usr/bin/log`. [2q0, 2s1]
29. **Derive a kernel slide from a pointer you solved yourself, never from
    `kextstat`** — off by `0x15a00000` in one boot. [2s6, 2s7]
30. Boot-arg names: the kernel's parse is **exact** (`token_len ==
    strlen(name)`), our shell greps are not. Test a whole token
    (`tr ' \t' '\n\n' | grep -qx`), and do not name an arg so it prefixes an
    existing one — `rb_defaults.h`'s hosttest now asserts that for the derived
    negatives. [2ss, 2xw]
31. **`IOLog` is not an instrument** — in `src/fbkext` or in the plugin.
    32 554 kernel-sender lines reached one boot's log and not one was ours.
    `setProperty` it and read `ioreg -rl -c <class>`, or publish a diag key.
    [2sv, 2xm]
32. Read a `char*[]` table's **length** table too, and check the slot count
    against the bound in the code. An enum value of 0 is a value; an unwritten
    field is an absence that looks the same. [2v8]
33. **Before reading anything from the live machine, establish which boot it
    is** — `kern.boottime` against the capture's, `rbgpu-boot` against the last
    nonce. And **an index is not a count**: name the object a number belongs to
    before retiring a line with it. [2v9, 2u4, 2wd]
34. An arm instruction must list every one-shot the experiment needs and the
    reader must check each before interpreting anything. 2xw removed the
    one-shots; what remains is to **say which keys and args the experiment
    depends on** and verify them first. [2w0]
35. **When a static chain ends at our own code, instrument our end first.**
    2v9 disassembled five layers correctly and attached them to the one link
    that was fine. [2w1]
36. A log capture has a **level**, a **predicate** and a **window**, and all
    three silently drop the answer: `log show` omits INFO without `--info`,
    INFO does not survive a reboot, FAULT does, and matching one function name
    instead of the **sender** hid 213 lines including the wedge. [2u5, 2w2,
    2w4]
37. **A feature must stay disarmable from the machine it runs on.**
    `removeProperty` reaches no sync; `setProperty` does. Consume by
    **overwrite** on a path measured to persist, and ask what recovery looks
    like when a feature works *and* when it half-works. [2sq, 2wj]
38. **Two halves that are separately armed must check each other.** The
    framebuffer writes HUBP from `setAttribute('amtr')` and nowhere else, so
    the panel taken with no accelerator underneath freezes on the last frame —
    guaranteed. Since 2xw this is a check in `armedOnce()`, not a comment.
    [2wp, 2xw]
39. **A measurement of a defect taken in a configuration that does not exhibit
    the defect does not locate the defect.** Say which configuration a reading
    comes from, and whether that configuration has the symptom, before
    promoting it to a frontier. An instrument that needs its own load must
    publish the load beside the result. [2wy, 2x0]
40. **Boot-arg VALUES come back as a binary int, not ASCII.** The correct idiom
    is `int v = 0; PE_parse_boot_argn(name, &v, sizeof(v))`
    (`rbgpu_boot_argn_u32()`). A default that reads correctly is not evidence
    that the tunable works. [2xm]

41. **A constant is only a constant on the OS it was measured on - and an
    instrument that re-measures something every run can still be subtracting a
    stale base.** `dscsym.sh` measured the shared-cache slide on every single
    invocation, said so in its own header comment, and then subtracted a macOS
    15 SkyLight base from it, so `atos` answered for an address 0x16AF000 away
    from the one asked about and never once said anything was wrong. The fix is
    structural, not a corrected number: derive the base too
    (`tools/re/dscbase.sh`), and for anything that cannot be derived at use
    time, record PER OS whether it was measured there and refuse to use it
    where it was not (`src/common/rb_osver.h`). Ask of every written-down
    address: **which OS was this measured on, and is that the one running?**
    [2xz]

**The recurring error, stated once.** Thirty-odd recorded instances of reading
a value without first establishing what makes it meaningful. The practice that
keeps catching it: **say in the code what would have to be true for the reading
to mean anything, and log it when it is not.** Two variants: **read the
evidence already in hand before designing the next experiment** (2kx/2la/2lc
tested something a photograph had already excluded; 2s5 found the answer in
nine captures nobody had read together), and **find the consumer before writing
to a field** (2q9/2qa, which saved a boot twice over in 2s3).

## Chip facts established on hardware

- **GC seg0 = 0x1260** (four independent ways), seg1 = 0xa000. Apple's encoded
  register ids are `(dword offset << 32) | 11`, 11 = GC_HWID; byte address is
  `(seg + dword) * 4`. Offsets from `docs/linux-ref/gc_10_3_0_offset.h`.
- Accessor slots on `pm4+0x30` / `hw->0x368`: `[0x148]` id → dword offset,
  `[0x118]` **read**, `[0x120]` **write**. [2fd, 2fg]
- Carve-out MC `0xF4_0000_0000`, system-physical `0x7_8000_0000`
  (`FB_OFFSET` 0x780), BAR0 `0x8_8000_0000`, and **the GOP framebuffer is BAR0
  offset 0** (2pr). Apple's GPUVM space is `0xFF_BF..`.
  **`mc2pa(mc) = mc - vram_start + vram_base_offset`** — the missing term that
  was the whole CP halt.
- **One** SDMA instance; Apple's Navi 21 path builds four (`hw->0xc2 * 2 + 2`).
  The engine array is `hw + 0x3b0 + engine*8` (`[0]` PM4/GFX, `[1..4]` SDMA);
  the SDMA walk bases at `hw + 0x3b8`. [2gc, 2gi, 2gu]
- `GB_ADDR_CONFIG` is dword `0x13de` BASE_IDX 0, reads **0x242** (4 pipes,
  4 packers, 1 SE).
- `AddrSwizzleMode`: 0 LINEAR, 9 64KB_S, 22 4KB_D_X, 25 64KB_S_X, 27 64KB_R_X.
  The `_X` family is the swizzle defect; **why is still not established** —
  2nr blamed Navi 21's pipe count and 2ns measured that false.
- **`getPDEValue` returns flags only** — the address is ORed in by its caller.
  Never hook it to correct an address. [2ko]
- The panel: 1920x1080, `H_TOTAL` 2106, `V_TOTAL` 1129, pixel clock
  **341 987 984 Hz**, **143.83 Hz** measured. OTG0 is already running
  (`OTG0_CONTROL = 0x00011201`, OTG1-3 at 0x300 = off, DMCUB 4.0.15.47), DMU
  segment 2 = **0x34c0**, BAR5 is 512 KB. Measure the refresh by latching both
  endpoints on an `OTG0_FRAME_COUNT` edge, never over a fixed window (2rn).
- A Transfer TRB's dword 2 (`+0x8`) is
  `length[16:0] | TD-size[21:17] | interrupter[31:22]` — the USB fix's whole
  basis (2xv).
- `IODetailedTimingInformationV2` is 0xa0 bytes, `pixelClock` at 0x28;
  `AGDCLinkConfig_t` is 0xb0 and embeds it at **+0x04** (2rd/2rg).
- `IOFramebuffer` vtable call offsets: `0x9b0` `getVRAMRange`, `0xad0`
  `registerForInterruptType`, `0xae0` `setInterruptState` (2pq, 2s5).

## Boot-args

**The right boot-args are none of ours.** `src/common/rb_defaults.h` is the
list and the hosttest checks it; read that file, not a copy of it here.
**`sudo tools/efi.sh defaults`** takes the redundant ones out of the EFI in one
edit - exactly that table, matched by name - keeps every disarm and everything
not ours (`wdt=-1` above all), reports any `rbgpu` token it kept, and then
prints which NVRAM keys would disarm a half.

**Rescue, in order of bluntness:**
- `sudo nvram rbgpu-arm=0` — accelerator off next boot (and the framebuffer
  declines the panel with it, rule 38). Also `rbgpu-fb`, `rbgpu-agdc`,
  `rbgpu-iop`, `rbgpu-tick`, each `=0`. Re-arm with `sudo nvram -d <key>`.
- `-rbgpunoschedtick`, `-rbgpunofbhubp`, … — one feature off, derived from its
  own name: `-rbgpuno` + the rest, or for the two already-negative tokens
  (`-rbgpunocgcg`, `-rbgpunogates`) the positive form.
- **`-rbgpustock`** — every default off, the plugin otherwise alive.
- **`-rbgpuoff`** — the plugin loads and does nothing, and the framebuffer
  declines the panel. The documented rescue.
- The **boot sentinel** needs no command: a boot that does not survive 60 s
  makes the next one stock automatically.

**Still opt-in, and why:** `-rbgpufbbacklight` (never booted, 2xs),
`-rbgpufbbuiltin` (never booted, 2xy - changes which `IODisplay` subclass
matches the connection), `-rbgpuosforce` (2xz - arms the default-on table on an
OS none of the measured offsets came from; a safety gate's override is never
default-on),
`-rbgpuagdcearly` (**wedges the desktop**, 2rz), `-rbgpufbmatch` (spoofs
`device-id` and **kills the accelerator**), `-rbgpupdefix` (composes with the
upstream fix and the halt returns), `-rbgpufbvramrange` (hypothesis dead,
2pr), `-rbgpumcnew`/`-rbgpumcreg` (`mcreg`'s failure mode is a **hang**, so it
is the user's decision), `-rbgpunodisppanic`, `-rbgpusdmahook`,
`-rbgpustampraw`, `-rbgpuregdump`, `-rbgpubeat`. **`-rbgpuforceenable` is now
default-on and still wants `wdt=-1` in boot-args** — it is Apple's arg, we
cannot set it, and without it a frozen boot panics at 120 s with no SSH window
(2hh). Keep `wdt=-1`.

**Tunables:** `rbgpuschedtickus=<500..1000000>` (6944 is the better value on
the evidence, 2xn), `rbgpuvblhz=<millihertz>` (20000-400000),
`rbgpufbvrammb=<256..16384>`, `rbgpufbvramrangemb=<8..256>`,
`rbgpufbhubpmax=<N>` (0 = unlimited), `rbgpufbatymb=<1..3072>`,
`rbgpufbblfloor=`, `rbgpumtlkscope=` / `rbgpumtlkexclude=` (exclude wins),
`rbprobe_level=N`, `rbprobe_smu=1`. Rule 40: pass them as numbers, read them
with `rbgpu_boot_argn_u32()`.
