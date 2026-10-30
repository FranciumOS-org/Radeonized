# HANDOFF — next step

**Written 2026-10-29, after 2xt/2xu, for a session starting cold on a new
account. Everything needed to continue is in this file. Read `docs/RULES.md` for
the standing rules, then this.**

---

## THE SITUATION IN ONE PARAGRAPH

The user upgraded the machine to **macOS 26.7.2 (25G332, Darwin 25.6.0)** and
the **swizzle artifacts came back** — scrambled text and images, the the assistant app
unreadable. **Nothing is corrupted and nothing was half-written.** The swizzle
fix has two halves; the kernel half still works, and the userspace half stopped
being applied, which is the 2r6 fail-safe behaving exactly as designed.
**macOS 26 broke that half in TWO independent ways. The first is fixed. The
second is diagnosed down to one function and is the frontier.**

## WHAT THE USER IS WAITING FOR

A readable desktop. They are low on usage budget and switching accounts. They
have been told that `sudo tools/efi.sh bootarg add -rbgpuoff` gives them a
correct, readable, software-composited desktop (no acceleration) as an interim.
**Check whether they took that**; if `-rbgpuoff` is in boot-args, the driver is
inert and nothing about the GPU in that boot means anything (rule 27: it also
stops `init()` running, so such a boot is not a control for the init path).

---

## 2xt — FIXED, AND PROVEN. Do not re-open this.

`AMDRadeonX6000MTLDriver` moved and was rebuilt:

| | macOS 15.8.1 | macOS 26.7.2 |
|---|---|---|
| `__TEXT` | `0x7FFB08BF3000` | **`0x7FFB110D4000`** |
| size | `0x5029AE` | `0x4E8570` |

Both old patch sites now hold unrelated code, so `rbgpu_mtlk_place()` compared,
mismatched and wrote nothing.

**Re-derived** in `src/plugin/rbgpu_mtlcave.h` (regenerate with
`python3 tools/gen_cavepatch.py > src/plugin/rbgpu_mtlcave.h`):

    function  0x7FFB1121622D   amdMtl_HWL_DetermineSwizzleMode
    cave      0x7FFB1147D710   __const, 163 bytes used of 176 reserved
    __TEXT    0x7FFB110D4000

Confirmed **three independent ways**: the 30-byte prologue is unique in the new
`__TEXT`; the offset within `__TEXT` barely moved (`0x142A11` -> `0x14222D` in
a 5 MB image); and **LC_SYMTAB lands exactly on the address** with that name.
The cave sits in a 273-byte zero run at `__const+0x2F50`, the same structural
slot as the old cave (`__const+0x2F00`, also 273 bytes), and its 48-byte anchor
is byte-identical to the old one.

**A latent trap was closed in the generator.** `gen_cavepatch.py` overrode
`cp.CAVE_VM` but never `cp.FN_VM`, while `build_cave()`'s jump back to FN+30
and `detour()`'s rel32 both read `cachepatch.py`'s OWN `FN_VM`. They were equal
for the whole macOS 15 era, so it worked by coincidence. **Mutation-tested**:
without `cp.FN_VM` the trampoline jumps to `0x7FFB08D35A2F` instead of
`0x7FFB1121624B` — **132.9 MB off, into an unrelated dylib, executing garbage
in every process that touches Metal**. None of the generator's assertions would
have caught it; they only check find-pattern uniqueness and the prologue.

Every displacement was then hand-verified independently of the generator:
detour -> CAVE exactly, trampoline -> FN+30 exactly, internal call ->
trampoline, mode table NOX entries (21->5 22->6 23->6 25->9 26->10 27->10),
163 bytes inside 176 reserved with the tail still zero.

---

## 2xu — THE FRONTIER. The addresses are right; the READ fails.

Boot 226 ran the re-derived plugin **`FCEA5698`** (confirmed loaded against
`build/out`, rule 15) and **the artifacts are still there**.

**The addresses are NOT the problem. Three proofs, none needing a boot:**

* `mtlk_place` = **0x7FFB2339B6E0** = exactly `(CAVE_VM - ANCHOR) + slide` for
  the new header. The kext's arithmetic agrees with the header to the byte.
* `mtlk_slide` = **0x11F1E000**, and dyld independently reports the live
  shared-cache slide as **0x11F1E000** (CoreDisplay live `0x7FF817266000`
  against unslid `0x7FF805348000`).
* **That address reads `kRbMtlCaveFind` byte for byte from a live userspace
  process**, in this boot. Reproduce with:

      env -u DYLD_INSERT_LIBRARIES python3 -c \
        "import ctypes; print(ctypes.string_at(0x7FFB2339B6E0,16).hex())"
      # expect fc000000780000000000000000000000

**THE BLOCKER:** `mtlk_match` = **0xFFFF** and `mtlk_probe_read` = **1 =
KERN_INVALID_ADDRESS**. `rbgpu_mtlk_place` writes `0xFFFF` on exactly one
condition — `g_mtlkVmMapReadUser` returned non-`KERN_SUCCESS`. **The bytes are
never compared because the read never produces any.**

**It failed that way EVERY time, and that is provable without another boot:**
`mtlk_cavehits` **1280**, `mtlk_failed` **1280**, `mtlk_patched` **0**, and
**`mtlk_moved` is ABSENT**. `g_mtlkMoved` increments only on a *successful*
read whose bytes mismatched, and it is set on every call, not only reported
ones. So **not one of the 1280 attempts ever completed a read**.

**Corollary that cost a wrong instruction to the user:** `mtlk_moved_verdict`
being absent is **NOT** evidence the addresses are right. That verdict is only
reachable through a byte comparison this boot never reached. Do not use it to
check an address re-derivation.

**Clean differential, from 45 macOS 15 captures already on disk:**

| | macOS 15 (45 captures) | macOS 26, boot 226 |
|---|---|---|
| `mtlk_probe_read` | **0** = KERN_SUCCESS | **1** = KERN_INVALID_ADDRESS |
| `mtlk_match` | **1** = pattern found | **0xFFFF** = read failed |
| `mtlk_patched` / `cavehits` | **763 / 768** | **0 / 1280** |
| `mtlk_moved` | 1–2 stragglers | **absent** |

Same hook, same four symbols, same mechanism. A **second, independent** macOS
26 change stacked on the driver move.

### Already eliminated — do NOT spend a boot re-testing these

* **Not the symbols.** `mtlk_syms` = **15** (all four resolved), and `nm -m`
  confirms `_get_task_map`, `_vm_map_read_user`, `_vm_map_write_user`,
  `_get_bsdtask_info` are real `__TEXT,__text` definitions on macOS 26, not
  stubs.
* **Not a bad map.** `_vm_map_read_user` (`0xffffff800034c8a0`) was
  disassembled. Its not-current-map path **zone-validates** the `vm_map_t`
  against `_zone_info` and requires zone index `0xf`; a map that fails branches
  to `0xffffff800034cb27`, which calls into the **panic** path, not a return.
  We did not panic, so the map was valid. The only remaining producer of the
  observed `1` is the `copyin` (`0xffffff800034cab8` fast path /
  `0xffffff800034ca02` slow path) faulting — `setne r15b`, `r15` -> `eax`.
* **Not "the shared region is not entered yet".** Call-site scan of the macOS
  26 kernel (`E8 rel32` over `__TEXT,__text`):

      _vm_shared_region_enter    1 site  0xFFFFFF8000346B51  in _vm_map_exec
      _task_set_main_thread_qos  2 sites 0xFFFFFF80007B231A  in _posix_spawn
                                         0xFFFFFF80007B6FB6  in ___mac_execve

  `vm_map_exec` runs early in exec, `task_set_main_thread_qos` late. The naive
  ordering story is refuted by the call graph.
* **Not on-demand subcaches.** A fresh process that never loads
  `Metal.framework` still reads `0x7FFB2339B6E0` successfully.
* **`_thread_bootstrap_return` is NOT routable — rule 2 refuses it.** It is
  **five bytes**: a bare `callq _dtrace_thread_bootstrap` rel32 at offset 0,
  falling straight into `_thread_exception_return`. Lilu copies displaced
  instructions byte for byte, so routing it relocates a rel32 into Lilu's
  buffer. It also never returns.

**What IS established:** the patch runs from
`wrap_rbMtlkTaskSetMainThreadQos` (`_task_set_main_thread_qos`), and at that
point the new task's map does not have the cache readable — whatever the
mechanism. Say it that narrowly; the mechanism is not established.

---

## THE NEXT STEP — needs NO new kernel route

The plugin **already** hooks
`AMDRadeonX6000_AMDHWAlignManager2::getPreferredSwizzleMode2`
(`hooked_swizmode_2la`, installed near `src/plugin/kern_start.cpp:14586`,
flag `g_rb_2la_hk_swz`). Boot 224 read `rbgpu-surf` = `swz hk=1 calls=1466`, so
it is live and firing.

That hook runs **in the Metal client's own process context**, long after exec,
where:

* the shared cache is demonstrably readable, and
* `current_map()` **IS** the target map — which takes `_vm_map_read_user`'s
  **fast `copyin` path** (`0xffffff800034caad`), not the map-switch path that
  is failing.

**Plan:** retry the cave + detour placement from that hook, once per task
(keep a small set of recently-patched task pointers or pids). It inherits 2om's
compare-before-write fail-safe unchanged, so a wrong address still writes
nothing. Use `current_task()` / `get_task_map(current_task())`.

**SETTLE THIS FIRST, statically, before writing code:** does
`getPreferredSwizzleMode2` fire **before or after** the client's first
`amdMtl_HWL_DetermineSwizzleMode` call? If after, the patch lands from the
second texture onward and early surfaces in each process stay wrong — in which
case the correct point is the client's **user-client open**, not the swizzle
query. (The plugin has other client-context hooks to choose from, e.g. the 2le
route on `AMDAccelResourceAddr2::initialize`.)

**Rule 31 applies**: `IOLog` is invisible here — publish with
`rbgpu_diag_set_num` and read it back from the registry, not the log. Give the
retry its **own** counters (`mtlk2_*`) so it is distinguishable from the exec
path's 1280 failures.

---

## THE BUILD IS BROKEN IN A WAY THAT LIES TO YOU — already fixed, know about it

On macOS 26.7.2 with **CommandLineTools 16.4**, the **ASan runtime is broken**:

    printf '#include <stdio.h>\nint main(void){printf("ok\\n");return 0;}\n' > /tmp/t.c
    xcrun clang -fsanitize=address /tmp/t.c -o /tmp/t && /tmp/t   # SIGILL, exit 132
    xcrun clang                    /tmp/t.c -o /tmp/t && /tmp/t   # "ok"

This made `make plugin` and `sudo tools/efi.sh install` die with a bare
**`Illegal instruction: 4`**, which looks exactly like the hosttest failing —
when in fact **every assertion passes**. Rule 16, in its purest form.

The Makefile now **probes** the runtime and degrades **loudly**:

* `SAN=auto` (default) — detects the breakage, prints three `***` lines on
  every build, runs the tests unsanitized, and they must still all pass
* `SAN=1` — forces sanitizers on; **still dies** on this toolchain. Use this to
  re-check after a CLT update
* `SAN=0` — forces off

Cached in `build/.san_ok`, keyed by the compiler version string, so a CLT
update re-probes by itself. `make clean` drops it.
**The real fix is updating CommandLineTools. Never commit a change whose only
passing run was unsanitized.**

---

## STATE OF THE TREE

Committed on `main`, nothing uncommitted:

    49e8898  2xu: boot 226 says the addresses are right and the READ is what fails
    bdde929  2xt: probe the sanitizer runtime instead of assuming it works
    8e4d142  2xt: macOS 26 moved the Metal driver, the patcher failed safe, re-derived
    60bd35c  2xq: the GOP does not light HDMI, measured on boot 221

Built artifacts and their UUIDs:

    plugin  build/out/RembrandtGPU.kext      FCEA5698  (installed in the EFI, boot 226)
    fbkext  build/out/RBAMDFramebuffer.kext  7F312381  (NOT installed; see below)

**The framebuffer kext still links on macOS 26** — `make fbkext` reports
`LINK 81 symbols from the system collection, 293 from boot`, so
`IOGraphicsFamily` still resolves out of `SystemKernelExtensions.kc`. The
framebuffer half of the project survived the OS update.

**`7F312381` contains the UNBOOTED 2xs backlight work** (see below). The kext
currently loaded in boot 226 is `C7278266`, the pre-backlight build. Do not
install `7F312381` as part of the artifact fix — it is a separate milestone.

---

## 2xs — BACKLIGHT, BUILT AND TESTED, NEVER BOOTED

Finished and committed but **never armed on hardware**. All of it is behind
`-rbgpufbbacklight`, which is **not** in boot-args.

* `src/common/rb_backlight.h` — all decision logic, pure/freestanding
* `src/fbkext/RBAMDFramebufferBacklight.cpp` — mechanism only
* `tools/hosttest/backlight_test.c` — passes, including **the control**:
  Linux's `dce_get_16_bit_backlight_from_pwm` applied to 2os's measured
  `BL_PWM_CNTL 0xc00056ea` / `BL_PWM_PERIOD_CNTL 0x000c0fa0` reproduces
  **22784** exactly, the number 2os derived by hand three milestones earlier
* Mutation-tested four ways (M1–M4), all failing with real assertion failures
* The 18 `bl*` fields were mirrored into `tools/hosttest/fb_vbl_shim.hpp` —
  **the fifth time that file broke the build after a new published field.** The
  previous attempt failed because its regex was anchored on `bool blReady;`
  while the real declarations carry `=false` initializers

Still outstanding for 2xs if it is picked up: add `-rbgpufbbacklight` and
`rbgpufbblfloor=` to docs/RULES.md's boot-arg list, record the three new
`docs/linux-ref/` files (`dce_panel_cntl.c`, `dce_panel_cntl.h`,
`dcn_3_1_2_sh_mask.h`, torvalds/linux v6.12) in `docs/linux-ref/UPSTREAM.md`,
and append a 2xs milestone to the journal. **Arm check first**: read
`RBFBBlArmed` / `RBFBBlReady` / `RBFBBlRefuse` before anything else.

A **follow-on, not started**: making `AppleBacklightDisplay` match (add
`kIOConnectionBuiltIn` 0x800 to `kConnectionFlags` at
`src/fbkext/RBAMDFramebuffer.cpp:898`, and override `getAppleSense` to return
display type 2/14/20) is what brings the real `"brightness"` parameter, the
System Settings slider and the brightness keys. Riskier — it changes which
`IODisplay` subclass matches — so it needs its own boot-arg and its own boot.

---

## EVERY APPLE ADDRESS IN THE REPO IS NOW A macOS 15 ADDRESS

This is the big one for any future static work. `docs/RULES.md` and
`docs/JOURNAL.md` are full of dyld-cache addresses measured on macOS 15.8.1.
**They are all wrong now.** Measured so far:

    AMDRadeonX6000MTLDriver __TEXT  0x7FFB08BF3000 -> 0x7FFB110D4000
    CoreDisplay             __TEXT  0x7FF80404C000 -> 0x7FF805348000

Consequences:

* **`RBGPU_IOP_SITE_VM` (`0x7ff80412ce87`, `kern_start.cpp:7136`) is stale.**
  It fails safe the same way (`iop_match` = **0xFFFF**, `iop_patched` = 0), and
  2t7 measured gate 1 as won **unpatched**, so it is **not load-bearing** — but
  do not read anything into its numbers.
* `tools/re/dscsym.sh` hardcodes macOS 15 unslid bases (`SkyLight
  0x7FF807B42000`, `CoreDisplay 0x7FF804_04C000`) and will mis-symbolise
  everything until updated.
* Re-derive before any further static read of CoreDisplay, SkyLight or
  IOPresentment.

The cache is at
`/System/Volumes/Preboot/Cryptexes/OS/System/Library/dyld/`, now **7 files**,
with `dyld_shared_cache_x86_64h.map` giving each image's `__TEXT`.
`tools/re/dyldcache.py` reads it (note the API: `Cache.images()` returns
`(loadAddress, textSize, path)` triples, and `symtab(cache, Image)` returns
`(addr, name, ntype)`).

---

## USEFUL ONE-LINERS FOR THIS PHASE

    # which boot, and is it armed (rule 8 / rule 33 — ALWAYS do this first)
    nvram rbgpu-boot; sysctl -n kern.boottime
    kextstat | grep -i rembrandt          # UUID must match build/out

    # the FULL diag table (the `rbgpu-diag` NVRAM note is a TRUNCATED hot-key
    # list — mtlk_* is NOT in it, so absence there proves nothing, rule 26)
    ioreg -rl -c RembrandtGPUPeer | grep -o '"RBGPUDiag" = {.*}'

    # find the driver in the new cache
    grep -A2 'AMDRadeonX6000MTLDriver$' \
      /System/Volumes/Preboot/Cryptexes/OS/System/Library/dyld/dyld_shared_cache_x86_64h.map

    # the live shared-cache slide, from dyld itself
    env -u DYLD_INSERT_LIBRARIES python3 -c "import ctypes;l=ctypes.CDLL(None);
    l._dyld_get_image_vmaddr_slide.restype=ctypes.c_long;print(hex(l._dyld_get_image_vmaddr_slide(0)))"

**This session runs inside the the assistant app with `DYLD_INSERT_LIBRARIES` set —
launch anything with `env -u DYLD_INSERT_LIBRARIES` or it starts under whatever
patch this the assistant is running (2ns).**

---

## WHAT THE USER RUNS, NOT the assistant

`sudo tools/efi.sh install`, `sudo tools/efi.sh bootarg add|remove <arg>`,
`sudo tools/load.sh`, `sudo tools/unload.sh`, `sudo tools/fbload.sh`,
`tools/plugin_check.sh`. **the assistant never runs these and never asks for the
password.** the assistant never installs into the EFI or `/Library/Extensions`.
**Never run `kmutil unload` on the framebuffer kext — it PANICS.** To remove
it, reboot. Ask before any kext load. Commit freely; **ask before pushing**.
