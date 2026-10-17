# RembrandtGPU — handoff state (2026-10-17, after 2gv)

**Read this first.** `docs/RULES.md` is now short — the project rules, the rules
that cost boots to learn, and the chip facts — and is auto-loaded every
session. `docs/JOURNAL.md` is the chronological evidence journal (~8500
lines): authoritative for *what was measured*, in order, including every
withdrawn reading, and must not be trimmed; grep it by milestone tag rather
than reading it whole. This file is the *current state*: what is settled,
what is open, what to do next, the procedure, and the mistakes this project
keeps making.

`docs/HANDOFF-FULL.md` is a stale file-dump from 2026-10-05. Ignore it.

---

## 1. One-paragraph state

Apple's Navi 21 stack (AMDRadeonX6000 + AMDRadeonX6800HWLibs +
HWServicesNavi) runs on the Radeon 680M. PSP, GC and TTL init all succeed with
the 680M's own yellow_carp firmware; the SMU link works two-way; the
accelerator's `start()` returns 1; the KIQ is created, active and doorbelled;
GPUVM translates; the GPU executes PM4 packets and writes completion fences
the CPU reads. **`AMDGFX10PM4Engine::doStart` returns 1 and every engine
powers up.**

**`powerUpHW` SUCCEEDS**, now on eight consecutive boots (`ret=1 post=1111
one=3`). `AMDHardware::startHWEngines` returns TRUE once exactly one SDMA
engine asks the layer for queues (2gi, `-rbgpusdmaone`): Apple's Navi 21 path
builds four SDMA engines while this chip has one instance, so engines [1..3]
take Apple's own trivial-TRUE path and engine[0] alone asks.

**The frontier is a GPU-restart loop driven by ONE outstanding stamp — and
it is the only stamp the driver has ever issued.** Request 0x1B reaches
`AMDAccelEventMachine::finishAllStamps`, which walks 63 slots; exactly one
has work outstanding, **slot 25**, `submitted=1 completed=0 fence=0`. The
same index Apple's own `GPURestartDeferred stampIdx=25` names from a
different source. `accel->0xa0`, the driver-wide `incrementStamp` count,
reads **1** (2gs). **Slot 25 is the `"VMPT"` channel** — it names itself
(2gt) — so the one stamp this driver has ever issued is a page-table
update. **It holds a real `AMDSDMAChannel` on hw engine index 2, and that
engine has zero queues** (2gu: `h=AMDSDMAChannel e2=1 e2q=0`): nothing
refuses the submit, nothing can complete it. Engine 2 is the first engine
`-rbgpusdmaone` switches off, while the one working SDMA sits at index 1
(2gv, §2).

```
finishStamp(25) -> waitForStamp -> kIOReturnTimeout after ~10 s
  -> handleFinishChannelRestart
       IOAccelDisplayMachine::waitForStampTimeout -> FALSE (pipe count is 0)
       hardwareErrorEvent: GPURestartDeferred stampIdx=25
       AMDAccelChannel::resetHardwareAndReplay -> request 0x17 -> our peer:
                                                   Unsupported
  -> retry, UNCONDITIONALLY, forever   (W and R climb 1:1 across 3 samples)
```

The GPU is **not** hung: the KIQ completes stamps 69, 70, 71 with real fences
in the same window. Request 0x14 reached **#1,506,624** — the register poll
inside `waitForStamp`, each read reaching our peer — and macOS spawns a
`DumpGPURestart` process, so userspace sees the restart too.

Nothing about display is started. The accelerator deliberately adopts **no
display pipe** (2ct), so armed boots can reach a desktop.

---


## 2. The pending experiment — send VMPT to the SDMA engine that exists

Build **`CF1786D5`** (2gv). **Boot-arg to ADD this once:**

```
sudo tools/efi.sh bootarg add -rbgpuvmpteng1
```

### What is settled

Slot 25 is the **`"VMPT"`** channel — it names itself (2gt) — so the one
stamp this driver has ever issued is a page-table update. Boot 16 (2gu)
then measured what is behind it:

```
25:s1/c0/f0[AccelChannel "VMPT" h=AMDSDMAChannel]
   out=1 tot=1 nch=63 hwn=25 nz=0 e2=1 e2q=0
```

**The channel exists and its engine has zero queues.** Nothing refuses the
submit; nothing can complete it. That was row three of the table written
before the boot.

The index bases differ, and that is the finding. The SDMA walk reads
`hw + 0x3b8 + i*8` while `getHWChannel` indexes `hw + 0x3b0 + engine*8`, so
**2gi's `engine[i]` is hw array index `i + 1`**:

```
hw[0] = Navi21PM4Engine        GFX
hw[1] = SDMAEngine_GC_10_3_0   2gi's engine[0] - LEFT ALONE, keeps queues
hw[2] = SDMAEngine_GC_10_3_0   2gi's engine[1] - zeroed        <- VMPT
hw[3] = SDMAEngine_GC_10_3_0   2gi's engine[2] - zeroed
hw[4] = SDMAEngine_GC_10_3_0   2gi's engine[3] - zeroed
```

VMPT asks for the **first** engine `-rbgpusdmaone` switches off, while the
one working SDMA sits at index 1. `one=3` says all three writes took, and
2gi only counts an engine whose count was non-zero first — so hw[2] had
queues and 2gi removed them. 2gi's own comment predicted exactly this:
*"if anything later submits to engines [1..3], it finds no queues."*

Apple's arithmetic makes hw[2] unavoidable:
`ENGINE_TYPE = ((caps->0x92 * 2 + 1) & idx) + 1`, with `prio=0` forcing
`idx = 1`, and `x*2+1` always odd — so the answer is 2 whatever the caps
byte holds.

### The experiment

Hook `AMDHardware::getHWChannel(engine, ring)` (0xbea9994) and send
**engine 2 to engine 1**. Milestone 2c checked (external *defined* symbol,
not an import); rule 9 checked (`55 48 89 e5 89 f0 …`, no relative branch
in the patch window). The route happens every boot; the **substitution** is
gated on `-rbgpuvmpteng1`, so a boot without the arg still reports the
remap OFF rather than leaving it ambiguous whether the route took. Every
remap is logged as a substitution — a stamp that completes after this
proves the remap works, **not** that engine 2 exists (2cy).

```
nvram rbgpu-boot   must be 17 — a NEW sequence, or every note is STALE
nvram rbgpu-1b     ... fas:E(n=63|... "VMPT" h=H] out=O tot=T nch=63
                       hwn=25 nz=Z e2=1 e2q=0 e1q=Q)
RBGPULog           one "2gv: ... SUBSTITUTED to engine=1" line
```

**Written before the boot:**

| reading | means |
|---|---|
| `e1q=0` | the remap target has no queues either — **nothing** about this boot may be read as being about engine 1, and the whole approach is wrong |
| no `2gv:` substitution line | the route or the boot-arg did not take; the rest of the note says nothing about the hypothesis |
| `out=0`, `nz` non-zero, no restart loop | VMPT completed — the frontier moves, and 2gi's blanket zeroing becomes the thing to fix properly |
| `out=1` still, `h=AMDSDMAChannel` | VMPT has a backed channel and still never completes; the fault is in the submit or the doorbell, not the engine |

**Still open and not guessed: who calls `incrementStamp(25)`.** The
slot-0x1d8 shortlist was checked and does not hold. With `tot=1` it is a
single call somewhere in startup.


## 3. Verified on hardware — do NOT re-derive these

- **The chip.** GC 10.3.3, DMU 3.1.3, MMHUB/ATHUB 2.4.0, MP0/MP1 13.0.3,
  SDMA 5.2.3 (**one instance**), VCN 3.1.1. 1 SE x 2 SA x 3 WGP = 12 CUs.
- **Carve-out.** MC `0xF4_0000_0000`-`0xF4_FFFF_FFFF`, system-physical
  `0x7_8000_0000` (`GCMC_VM_FB_OFFSET` = 0x780), BAR0 256 MB at
  `0x8_8000_0000`.
- **seg0 = 0x1260** for GC `BASE_IDX 0`, agreed four independent ways; seg1 =
  0xa000. Byte address is `(seg + dword_offset) * 4`.
- **The SMU link works.** `rb_smu13_bringup` succeeds with real Rembrandt SOC
  and DF values; the MC address is `FB_LOCATION_BASE + alloc_offset`.
- **`powerUpHW` SUCCEEDS**, four consecutive boots (`ret=1 post=1111 one=3`),
  with `-rbgpusdmaone` letting exactly one SDMA engine ask for queues.
- **Request 0x1B passes its `isDeviceValid()` gate** and
  `startGlobalPerfCounters` returns; `hardwareDidStart` does not (2gn).
- **The accessor slots.** On `pm4+0x30` / `hw->0x368`: `[0x148]` resolves an
  encoded register id to a dword offset, `[0x118]` READS that offset,
  `[0x120]` WRITES it. Apple's encoded ids are `(dword offset << 32) | 11`.
- **`hw->0x380` is an `AMDGFX10VMM`** and `hw->0x20820` an
  `AMDGFX10UserQueueManager`, both from `init`'s own allocations (2go).
- **Request 0x1B's `hardwareDidStart` completes** (2go), including
  `AMDHWVMM::postInit`'s two VM-fault interrupt registrations and the single
  GC register read (`C = 0x3b`).
- **The HIQ ring works.** `AMDGFX10HIQHWChannel::submitSetResourcesPacket` ->
  `submitFrame(true)` -> doorbell -> `waitForHwStamp` returned **1**: the GPU
  executed the HIQ's SET_RESOURCES packet and wrote its fence (2go).
- **`accel->0x380` is an `AMDAccelEventMachine`** (via
  `IOGraphicsAccelerator2::createEventMachine`), and call offset 0x158 on it
  is `finishAllStamps` (2gp).
- **The GPU executes and signals normally at 0x1B time** — the KIQ completed
  stamps 70/71/72 with fences landing where the CPU polls (2gp), while the
  event machine was declaring a hang.
- **Request numbers are Apple's `_eAMDAccelIOFBRequestType`**, from the symbol
  `AMDGraphicsAccelerator::sendRequestToController` (2gp).
- **`powerUpHW` succeeds**, now five consecutive boots.
- **`accel->0x1a78[]` is the channel array**, bounded by `accel->0x1e78` —
  Apple's own `eventTimeout` lookup, and the same array `waitForStamp` bounds
  against (2gr).
- **Exactly one stamp slot is outstanding at 0x1B time**: slot 25,
  `submitted=1 completed=0 fence=0` (2gq).
- **GFX is NOT in GFXOFF** (status 2), and `gfx_min = gfx_max = 0` is real
  data from the SMU: **GFX DPM was never started.** Standing, untouched.
- **PT_BASE (2em) was the GPUVM fix.** The hub register held
  `amdgpu_gmc_vram_mc2pa` computed with `vram_base_offset = 0`; writing
  `0x78FDFA001` removed the MAPPING_ERROR and the fence began landing.
  Requires `-rbgpuptbase`.
- **The whole `doStart` chain completes**: setres + stamp, `initGraphicsMQD`
  (~230 us), mapQueues ring 0 + stamp, `initComputeMQD(5)`, mapQueues ring 5
  + stamp, the ring-state reads, `waitForActiveValue` with
  `CP_GFX_HQD_ACTIVE = 1`, `vtable[0x328]`. **No unexercised step remains.**
- **The powerUp chain runs on the peer's post-register thread inside request
  0x06**, from `thread_call_delayed_timer` — NOT `IOService::start`. Confirmed
  three ways including Apple's own panic frames.
- **`powerUpHW` SUCCEEDS (2gi), reproduced on three boots** (b=6, b=7, b=8):
  `startHWEngines` returns TRUE with `ret=1 post=1111 allstarted one=3`.
- **Request 0x1B does not return, and it SPINS (2gk).** Sent with powerUpHW
  succeeding; the machine stays alive (log sequence advancing ~4000/20 s, all
  three samplers firing, `pci.txt` complete at 401 KB where 2fo's was 0) and
  request 0x14 runs at ~5600/s against 18/s on the boot with 0x1B held. Our
  0x14 handler answers a **constant 1** (RembrandtGPUPeer.cpp:1553).
- **0x1B is at `0xbe417d0` and contains no loop (2gl)** — so the poll is in a
  callee. See §2 for the disassembly and the candidate.
- **One SDMA engine asking is sufficient (2gi).** `startHWEngines` returns
  TRUE and **`powerUpHW` succeeds** with engines [1..3] on Apple's
  trivial-TRUE path and engine[0] alone asking. Reproduced on two consecutive
  boots (`b=6`, `b=7`), both `ret=1 post=1111 allstarted one=3`.
- **The SDMA 2x2 is closed (2gh), but its two conditions are NOT separable.**
  engine[1] STARTS asking first for instance 0; it is refused asking second
  for instance 0 (2gd) or first for instance 1 (2gf). Both the instance index
  and the ordinal are necessary — but this chip has one SDMA instance, so
  "second asker" and "duplicate create on instance 0" are the SAME event in
  every cell measured, and 2gi removes the second asker entirely rather than
  discriminating them. Open, and it does not block anything.
- **The display-pipe reserve now PASSES (2gi).** Seven `2bn reserve` calls,
  all seven returning 1, at real carve-out offsets. 2bq's reading — the
  allocator was empty because `powerUpHW` had never run — is confirmed by the
  cause being removed. docs/SPEC-DISPLAY-RESERVE.md's chain is unblocked.
- **Apple's trivial-TRUE path works** (`start()` with `+0x48 == 0` →
  `0xbea4b49`): used on engine[0] in 2ge and again in the 2gh boot.
- **Request 0x1B hangs** when powerUpHW has touched the hardware, so it is not
  sent unless powerUpHW succeeded (`-rbgpuforce1b` overrides).

### Key addresses (AMDRadeonX6000 unless noted)

```
0xbea1190  AMDGFX10PM4Engine::doStart
0xbea2542  initGraphicsMQD            (region G gated on pm4+0x10, measured 0)
0xbea8ec9  powerUpHWEngines   walks hw+0x3B0, 11 entries, vtable[0x138]
0xbea8fea  startHWEngines     walks hw+0x3B8, hw->0xc2*2+2, vtable[0x148]
0xbea4a6a  AMDGFX10SDMAEngine::start      <- THE FRONTIER
0xbeca1f0  Navi21Hardware::allocateHWEngines
0xbeca1d9  Navi21Hardware::setupAndInitializeHWCapabilities  (+0xc2 = 1)
0xbed26ad  Navi23Hardware::      "       "                   (+0xc2 = 0)
0xbec7688  startKIQ           (asks the service layer, command 8 - works)
0xbe85536  waitForHwStamp
0xbe848cc  AMDHWChannel::init
```

Engine array layout, confirmed from the constructor and from `amd-log.txt`:

```
0x3B0[0]      AMDNavi21PM4Engine
0x3B0[1..4]   AMDSDMAEngine_GC_10_3_0   == 0x3B8[0..3]
0x3B0[8..9]   AMDVCN3HWEngine
```

### Apple's SDMA `start()`, disassembled (0xbea4a6a)

```
bea4a6a  if (this->0x10 != 0)   return FALSE        "started" flag
bea4a87  svc = (this->0x18)->vtable[0x2c0]()
bea4a94  if (this->0x48 == 0)   -> bea4b49          zero queues: trivial TRUE
loop qi < this->0x48:
  bea4aa7  movq 0x50(%rbx,%r12,8), %r13   ch = INLINE ARRAY inside the engine
  bea4ad4  desc = { 0x40000, ch->0x48, ch->0xd0 }
  bea4b0c  svc->vtable[0x120](this->0x1a0[qi], this->0x1a4[qi], &desc, &out)
  bea4b14  if (ret != 0)        -> bea4b5d          unmap, return FALSE
bea4b49  this->0x10 = 1 ; vtable[0x278]() ; return TRUE
```

Measured pairs: `queue[0] = (0x7, instance)`, `queue[1] = (0xa, instance)`.
`+0x50` is an **inline array of channel pointers inside the engine**, not a
pointer that is then indexed — misreading that caused the 2gg panic.

---

## 4. Open and unexplained

- **Why engine[1]'s queue create is refused.** The 2gh boot decides whether
  it is the instance index or the engine object.
- **There is no `hw->0xc2` that fits this chip.** `0xc2*2+2` is always even
  and >= 2, so Apple's Navi 21 path cannot walk exactly one SDMA engine. If
  the instance index turns out to be the discriminator, the real fix is making
  `allocateHWEngines` build one engine, which is a separate change.
- **`gfx_min = gfx_max = 0`** since 2di — GFX DPM never started. It has not
  bitten any measured path yet (the RB0 writes and the HQD poll all work), but
  it is unexplained.
- **A panic can lose an NVRAM note** that the 2gd nonce would otherwise make
  readable (2gg: `rbgpu-boot` stayed at 3 although stage 1 ran 3 s earlier).
  A note's write can also fail to survive a power cycle (2gc, measured by
  comparing in-boot `nvram.txt` against the post-cycle read). Cause unknown.
- **`rbgpu-diag` stopped reporting the frontier** after 2fa rate-limited the
  refresh. Values that matter must ride an NVRAM note, not the diag table.
- **The published `RBGPULog` and `log.txt` can disagree** within one capture
  (2fx). Noted, not chased.

### Debts — bypasses that have never been justified

- **2am / 2an / 2ao**: `_gc_fw_autoload_is_completed`,
  `_gc_check_register_equal_ext` (RLC safe mode) and
  `_sdma_5_2_1_fw_autoload_is_completed` are all forced true, blanket. The
  SDMA one is directly relevant to the current frontier.
- **2cc**: `AMDVCN2EncChannel::allocateMemoryResources` stubbed to true.
- **2be**: the accelerator's `stop()` patched to RET (a trap-3 guard whose
  cause was never understood).
- **2ct**: the display pipe is refused outright.
- **2cp**: `waitForActiveValue`'s `bounded` parameter forced true. This one is
  a restoration of Apple's own 100 ms cap, not a bypass, and 2fi measured the
  poll exiting on its first iteration anyway.

### Not started at all

Display, VCN, GFX DPM, any GPU client. No boot has reached a desktop with the
accelerator powered since 2em.

---

## 5. The recurring error — read this before concluding anything

**Twenty-one recorded instances.** The shape is always the same: *a value is
read without first establishing what would make it meaningful*, or *absence is
read as a result*, or *a table is built from n=1 rows*.

Withdrawn conclusions, each once written up as a finding:

```
2dk  "seg0 = 0x1260 decodes"        tested != 0xffffffff; both bases read 0
2dl  "the MEC fetched a packet"     0xdef0def0 was a fill pattern
2dm  "the KIQ queue was never programmed"   wrong HQD window (GRBM_GFX_CNTL)
2do  "the PQ_BASE mismatch is a fault"      GPUVM VA vs physical MC address
2dp  "nothing was submitted"        measured before the doorbell
2dt  "+0xe0 == -1 means no writeback"       the jump was read backwards
2ee  "neither page is a page directory"     the instrument never read memory
2eh  "TTL has no GART table"        asked before the handle existed
2ej  "the page table has no valid entries"  read the wrong 2 MB of it
2ek  "the ring PTE is unreadable"   read before the HQD probe ran
2ev  the seven-boot span table      seven single samples read as causal
2es  "the refresh thread blocked"   it was looping, not blocked
2eu  "the hang is in initGraphicsMQD"       it was our own register dump
2dq  "the CP executed the packet"   it fetched, faulted and discarded
2fo  "the gate trail was the wedge" one boot read as a rule; 2fs refuted it
2fe  "three mapQueues calls remain" 0x348 is a loop BOUND, not a count
2gc  "a constant command 7"         read pair[0] only; arg1 is 0x7 then 0xa
2fu  third boot's `s=1` read as evidence    overwrite var, no nonce
```

### The rules this produced, in force

1. **Say in the code what would have to be true for a reading to mean
   anything, and log it when it is not.**
2. **Absence is not a result.** Every path out of a probe writes a record
   naming which path it took.
3. **A measurement placed behind a varying gate inherits the variance** (2fd).
   Put probes where the code always reaches.
4. **A diagnostic that substitutes its own value must never narrate that value
   as a hardware result** (2cy cost fifteen milestones).
5. **An overwrite NVRAM variable is this boot's only if `b=` is new** (2gd).
6. **One variable per boot.**
7. **The instrument is a suspect.** Two panics and ~17 boots in the 2e/2f
   family were the measuring apparatus, not the chip.
8. **Never route a symbol unless `nm -m build/re/<kext>` shows it DEFINED
   there** (milestone 2c: `solveSymbol` returns a bare KASLR slide for
   imports).
9. **Before routing a symbol, disassemble its first 16 bytes and refuse if a
   rel8/rel32 branch begins within the first 5** (2gb: Lilu copies displaced
   instructions byte-for-byte into a trampoline, so a short branch keeps its
   original displacement and lands in Lilu's own buffer).
10. **A NULL check is not a pointer check.** Validate derived pointers as
    kernel VAs (`>= 0xffffff8000000000`) before dereferencing (2gh).

---

## 6. Procedure — who runs what

**the assistant never runs `efi.sh`, `load.sh` or `unload.sh`, and never asks for the
password.** the assistant may commit at any time without asking; **pushing needs
asking**. Loading or unloading a kext needs asking. Deliberate diagnostic
panics are off the table (user's decision, 2026-10-12). Adding an SMU message
group is the user's decision.

### Build

```
make plugin          # the Lilu plugin. `make kext tools` does NOT rebuild it.
make hosttest        # must pass before any load
dwarfdump --uuid build/out/RembrandtGPU.kext/Contents/MacOS/RembrandtGPU
```

**Always check the UUID moved.** Two boots have been lost to a stale build.

### Arm a boot

```
sudo tools/efi.sh install && sudo tools/efi.sh status
sudo tools/efi.sh on && sudo nvram rbgpu-arm=1 && sudo reboot
```

Arming is one-shot: the peer consumes `rbgpu-arm` and flushes the removal
immediately, so a panic reboot comes up unarmed. **After a panic, check
`nvram rbgpu-arm` anyway** — 2gg found it still at 1.

### Read a boot — in the ARMED boot, before any reboot

```
tools/plugin_check.sh
```

Every readable boot in the 2g family was readable because this ran in the
armed boot. It writes `logs/<timestamp>-plugin/` with `nvram.txt`, `log.txt`,
`pci.txt`, `igpu.txt`, `peer.plist`. A 0-byte `pci.txt` means the IGPU
registry subtree was locked — the hard-wedge fingerprint.

### Before interpreting ANY boot

```
kmutil showloaded | grep rembrandt      vs  dwarfdump --uuid build/out/...
nvram rbgpu-boot                        a NEW sequence, or the notes are stale
nvram rbgpu-diag | head -c 60           armed=1 means this boot was armed
nvram boot-args                         which flags were ACTUALLY set
ls -lt /Library/Logs/DiagnosticReports/*.panic | head
grep -o 'com.rembrandtgpu.plugin(1.0)\[[0-9A-F-]*\]' <the .panic>
                                        WHICH BUILD panicked - not your memory
                                        of it. The 2gg panic was read as a
                                        2gh panic for a day, and the EFI
                                        backups (boot-args per operation) are
                                        what settled it.
```

### Rescue

`-rbgpuoff` in boot-args: the kext still loads, no peer instance exists.

### NVRAM channels

```
rbgpu-boot        per-boot sequence counter; every note is prefixed b=<n>
rbgpu-prog        stage note, overwritten (s=1 powerUpHW ENTER ... s=6)
rbgpu-sdma        the SDMA engine map, written once at stage 4 (2gf)
rbgpu-prog2       the +15/+35/+65 s samplers (2fy), stage + log sequence
rbgpu-gate        the gate trail - OFF while -rbgpunogates is set
rbgpu-diag/-prev  the diag summary; stops reporting the frontier (see §4)
rbgpu-arm         one-shot arming
```

---

## 7. Where to go next

1. **Build the four observe-only hooks** (§2) and spend one boot. The answer
   is which of `startGlobalPerfCounters`, `hardwareDidStart`,
   `(this->0x380)->vt[0x158]` or `enableInterrupts` is entered and does not
   return.
2. **If it is `enableInterrupts`**, the question becomes an interrupt storm —
   the GPU has been able to reach host memory and raise interrupts only since
   2em, which 2fk already flagged as the one new asynchronous thing on this
   chip.
3. **If it is `startGlobalPerfCounters` or `hardwareDidStart`**, it is a
   register wait on the GFX block, and `gfx_min = gfx_max = 0` — GFX DPM never
   started, standing since 2di — becomes the leading explanation. Acting on it
   needs an SMU message group beyond TABLES, which is **the user's decision**
   under the docs/RULES.md rule; say what it changes on the chip before asking.
4. **Standing, unchanged:** the 2am / 2an / 2ao blanket bypasses (the SDMA one
   is adjacent to a path that now genuinely starts), 2cc (VCN encode stubbed),
   2be (`stop()` patched to RET, cause never understood), 2ct (the display
   pipe refused outright). The display path is untouched and 2bq's reserve
   chain is now unblocked (§3), so it is the frontier after 0x1B.
