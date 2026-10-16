# RembrandtGPU — handoff state (2026-10-16, after 2gi)

**Read this before CLAUDE.md.** CLAUDE.md is the chronological evidence
journal (~4000 lines); it is authoritative for *what was measured*, in order,
and must not be trimmed. This file is the *current state*: what is settled,
what is open, what to do next, the procedure, and the mistakes this project
keeps making.

`docs/HANDOFF-FULL.md` is a stale file-dump from 2026-10-05. Ignore it.

---

## 1. One-paragraph state

Apple's Navi 21 stack (AMDRadeonX6000 + AMDRadeonX6800HWLibs +
HWServicesNavi) runs on the Radeon 680M. PSP, GC and TTL init all succeed with
the 680M's own yellow_carp firmware; the SMU link works two-way with real
clock tables; the accelerator's `start()` returns 1; the KIQ queue is created,
active and doorbelled; GPUVM translates; the GPU executes PM4 packets and
writes completion fences the CPU reads. **`AMDGFX10PM4Engine::doStart` returns
1 and every engine in the array powers up.**

`powerUpHW` now fails at exactly one place: **`AMDHardware::startHWEngines`
returns 0** because Apple's Navi 21 path builds and walks **four** SDMA
engines (`hw->0xc2*2+2`) while this chip's IP discovery table lists **one**
SDMA instance. The 2gh boot closed the 2x2 and settled the shape: **exactly
one engine may ask the layer for queues** — engine[1] starts when it asks
first for instance 0, and whichever engine asks second is refused. 2gi makes
engines [1..3] take Apple's own trivial-TRUE path so only engine[0] asks.
That is the whole current frontier.

Nothing about display is started. The accelerator deliberately adopts **no
display pipe** (2ct), so armed boots can reach a desktop.

---

## 2. The pending experiment — build UUID `B8D742D4` (2gi)

**Boot-args only. No code question is open in it.** The 2gh boot closed the
2x2; 2gi applies the fix it implies.

Engines [1..3] get a zero queue count, so Apple's own `start()` takes its
trivial-TRUE path at `0xbea4b49` — sets the started flag, calls
`vtable[0x278]`, returns TRUE **without asking the layer for anything**. That
path is proven on hardware twice (2ge on engine[0]; the 2gh boot's
`post=1100` with `skip0=2`). **engine[0] is left completely alone**: it
natively asks for instance 0 in the first ordinal position, the one
combination measured TRUE on every boot.

The counts are **not** restored. Those engines really have no queues on this
chip, so zero is the honest state; restoring it would be a latch reporting
hardware that is not there.

### Install and arm

```
sudo tools/efi.sh bootarg remove -rbgpusdma0skip
sudo tools/efi.sh bootarg remove -rbgpusdmainst0
sudo tools/efi.sh bootarg add -rbgpusdmaone
sudo tools/efi.sh install && sudo tools/efi.sh status     # expect B8D742D4
sudo tools/efi.sh on && sudo nvram rbgpu-arm=1 && sudo reboot
```

Both old switches must come out: one variable. 2gi needs neither.

Then **in the armed boot, before any reboot**:

```
tools/plugin_check.sh
```

### Reading it

```
nvram rbgpu-boot    a NEW sequence number, or every note below is STALE
nvram rbgpu-sdma    b=<n> ... ret=? pre=0000 post=???? ... one=?
nvram rbgpu-prog    b=<n> s=5 ...
```

```
post=1111 allstarted one=3   startHWEngines returns TRUE and powerUpHW
                             SUCCEEDS for the first time in this project.
                             Next step is request 0x1B, held by design.
post=1111 one<3              a switch did not take, and the note says so
                             rather than letting this read as the answer
post=1000 fail@1             engine[0] alone is not enough either, so the
                             refusal is neither the instance nor the ordinal
                             and the engine OBJECT is back: spend the next
                             boot on -rbgpusdmadesc
allstarted but ret=0         every start() returned TRUE and the FALSE comes
                             from OUTSIDE the loop (0xbea906b onward), which
                             nothing has yet looked at
```

`-rbgpusdmaone` also joins the **0x1B hold**, which is the point of the hold
rather than a formality: this boot is expected to be the first on which
`powerUpHW` succeeds, and 2fo measured request 0x1B never returning once the
hardware has real state. `-rbgpuforce1b` is the deliberate second step.

### Current boot-args (OpenCore config.plist)

```
keepsyms=1 -vi2c-force-polling -lilubetaall msgbuf=1048576 -rbgpusync570
-rbgpuspoofttl amfi_get_out_of_my_way=0x1 amfi=0x80 alcid=66 -v -rbgpupsphw
-rbgpusmubringup -rbgpustampwait -rbgpuptbase -nvoff -wegnoegpu
-rbgpunogates -broadtekdbg        (+ -rbgpusdmaone for this experiment)
```

**`-rbgpunodisppanic` must stay OUT** — it publishes `IOPCITunnelled` and that
alone costs the UI (2cv). **`-rbgpusdmahook` must stay OUT** — proven to panic
(2gb). `-rbgpusdmadesc` is off by default and is the *next* boot's tool, not
this one's (2gh).

**`-broadtekdbg` and `BroadTek.kext` are new to the GPU boots** — the EFI
backups show BroadTek was NOT enabled during any boot up to and including
2gg. It is a separate kext for another project, so it is a background
variable rather than part of this experiment; if a boot behaves oddly in a
way the SDMA question does not explain, rule it out first.

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
- **The SDMA 2x2 is closed (2gh).** Exactly one engine may ask the layer for
  queues. engine[1] STARTS when it asks first for instance 0 (`post=1100`);
  it is refused asking second for instance 0 (2gd) or first for instance 1
  (2gf), and engine[2] asking second for instance 0 is refused in the same
  boot. Both the instance index and the ordinal are necessary — but this chip
  has one SDMA instance, so "second asker" and "duplicate create on instance
  0" are the SAME event in every cell and **nothing measured separates them**.
  Open, and it does not block the fix: both readings imply one engine.
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

**Claude never runs `efi.sh`, `load.sh` or `unload.sh`, and never asks for the
password.** Claude may commit at any time without asking; **pushing needs
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

## 7. Where to go after the 2gi boot

- **If `post=1111 one=3`**: `startHWEngines` returns TRUE and **`powerUpHW`
  succeeds for the first time in this project**. The next deliberate step is
  `-rbgpuforce1b` — request 0x1B with the hardware fully brought up has never
  been sent, and 2fo measured it never returning when powerUpHW had touched
  the hardware. Expect it to need its own boot and possibly its own fix.
  After that: the display path, which is untouched (2ct refuses the pipe).
- **If `post=1000 fail@1`**: engine[0] alone is not enough, so the refusal is
  neither the instance nor the ordinal. Spend one boot with `-rbgpusdmadesc`
  (already built, off by default) to get `ch->0x48` and `ch->0xd0` per queue
  and compare engine[0]'s with engine[1]'s — those two fields are the only
  part of the request the pair arguments do not carry.
- **If `allstarted` but `ret=0`**: every `start()` returned TRUE and
  `startHWEngines`' FALSE comes from outside the loop, at `0xbea906b` onward.
  Nothing has looked there; it needs disassembling before a boot is spent.
- **Standing, further out**: `gfx_min = gfx_max = 0` (GFX DPM never started),
  the 2am/2an/2ao blanket bypasses — the SDMA one is directly adjacent to
  this frontier — and the fact that `hw->0xc2*2+2` cannot express 1, so if a
  real fix is ever wanted rather than a zeroed queue count, it is a patch to
  `allocateHWEngines` or to the walk.
