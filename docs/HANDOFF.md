# RembrandtGPU — handoff state (2026-10-12, after 2ec)

**Read this before CLAUDE.md.** CLAUDE.md is the chronological evidence
journal (~2600 lines); it is authoritative for *what was measured* and should
not be trimmed. This file is the *current state*: what is settled, what is
open, what to do next, and the mistakes this project keeps making.

`docs/HANDOFF-FULL.md` is a stale file-dump from the boot-27 era (2026-10-05).
Ignore it.

---

## 1. One-paragraph state

Apple's Navi 21 stack (AMDRadeonX6000 + X6800HWLibs + HWServicesNavi) is
running on the 680M. PSP, GC and TTL init all succeed, the 680M's own
yellow_carp firmware is loaded, the SMU link works two-way with real clock
tables, the accelerator's `start()` returns 1, and the KIQ queue is created,
active and doorbelled. `powerUpHW` fails at exactly one point: the KIQ's
completion fence never lands, so `waitForHwStamp` returns false, `doStart`
aborts, engine 0 fails.

**The cause is now located and dated.** A GPUVM fault is raised by the KIQ's
own submission (STATUS is 0 before the doorbell, 0x9ba after) at
`0xFF_BFE8_0000` = `CP_HQD_PQ_BASE << 8` = the ring base, decoding to
MAPPING_ERROR=1 / WALKER_ERROR=5 / CID 4 / RW=0 — **a read whose page walk
found no valid mapping**. So: the MQD points the CP at a GPUVM address that
does not translate; the fetch faults; RPTR advances having discarded the data
(which is why 2dq's RPTR/CP_STAT reading looked like success); the WRITE_DATA
fence inside the frame never executes; the stamp never arrives. The open
question is no longer *what* fails but *why that address is unmapped* — see
§2 and §7.

Nothing about display is started. The accelerator deliberately adopts **no
display pipe** (2ct) so armed boots reach a desktop.

---

## 2. The pending experiment — 2eb, RE-RUN as build UUID `24155DB4`

**2eb already ran on hardware and its answer was LOST.** `rbgpu-arm` is
consumed and `rbgpu-diag-prev` is an armed boot with `smu_bringup_armed=1`, so
the boot happened — but `tools/plugin_check.sh` was not run before rebooting
out of it, and the NVRAM fallback truncated at 512 bytes, ending mid-key at
`mc`, before every single `vm_*` key. Nothing about the range test survives.
2ec fixes the truncation (§5, tenth instance); the experiment itself is
unchanged and simply needs one more armed boot.

Built, hosttest passes, committed. **Verify the installed UUID with
`sudo tools/efi.sh status` against `dwarfdump --uuid
build/out/RembrandtGPU.kext/Contents/MacOS/RembrandtGPU` before interpreting
any boot.**

2ea settled the timing question: the GPUVM fault STATUS is 0 at
`post-gc-hw-init`, `post-startKIQ` and `pre-kiq-submit`, and becomes 0x9ba at
`at-stamp-wait`. **The fault is raised by the KIQ's own submission**, at
`0xFF_BFE8_0000` = `CP_HQD_PQ_BASE << 8` = the ring base, decoding to
MAPPING_ERROR=1 / WALKER_ERROR=5 / CID 4 / RW=0 — a read whose page walk found
no valid mapping. The pre-existing-fault escape is closed; the lead is real.

2eb asks whether the page tables are meant to cover that address at all. The
queue runs at VMID 0, so it reads `GCVM_CONTEXT0_PAGE_TABLE_START/END_ADDR`
and `_BASE_ADDR` plus `GCVM_L2_CNTL` / `GCVM_CONTEXT0_CNTL`:

| outcome | meaning |
|---|---|
| `vm_ring_in_range = 0` | the MQD was given a GPUVM address this VMID's page tables do not cover — the aperture itself is wrong |
| `vm_ring_in_range = 1` | the range is right and the fault is in the page-table *contents* |

Either way the next lever is likely **source comparison** (§7), not more
register archaeology: how amdgpu maps the KIQ ring into GPUVM for yellow_carp
versus what Apple's `startKIQ` command-8 path leaves configured.

## 3. Verified on hardware — do NOT re-derive these

| fact | evidence |
|---|---|
| PSP asic_type 19 → 8 swap binds the `_11_0` table | 2aa |
| Firmware loads: `_psp_np_fw_load` 111 ms, returns 0 | 2dc |
| yellow_carp fw substitution works, 7 entries swapped | 2ag |
| GC sw_init / hw_init / TTL hw_init all succeed | 2dc |
| Return convention: inner HWLibs = status (0 ok); IPI wrapper = bool in AL, upper EAX is garbage | 2dc |
| `AmdTtlServices::initialize` returns 0 | 2cw |
| `allocateMemoryResources` passes all engines | 2cx |
| `accelerator->start()` returns 1 | 2cx |
| SMU two-way transfer works; MC = `FB_LOCATION_BASE + alloc offset` | 2di |
| GFX is **not** in GFXOFF (status 2) | 2dj |
| GC register maths: byte addr = `(seg + dword offset) * 4` | 2dk |
| **seg0 = 0x1260**, seg1 = 0xa000 | 2dl (CP_RB0_CNTL nonzero at one, zero at the other) |
| `CP_MEC_ME1_HEADER_DUMP` is an 8-entry buffer pre-filled `def0..def7` — proves nothing | 2dm |
| KIQ lives at **ME 2 / pipe 1 / queue 0** | 2dn (QUEUE_SPEC) |
| `CP_HQD_*` is a window selected by `GRBM_GFX_CNTL`; it reads back 0 even when the write took | 2do |
| startKIQ **creates** the queue (empty `CTRL=0x00308509` before, configured after) | 2dp |
| `startKIQ` writes no register — it asks TTL/HWLibs (command 8) | 2dn |
| WPTR advanced to 0x20, RPTR caught up, CP_STAT busy | 2dq (see §4 — interpretation open) |
| `+0x80` submitted / `+0x84` completed; bookkeeping is correct | 2ds |
| `+0xe0 == -1` is the **direct fence path**, and is BY DESIGN for a KIQ | 2dv correction |
| KIQ polls a real writeback frame, slot 1, `hw->0x510 + (1<<6)` | 2dw/2dx |
| Writeback GPU addr `0xffbfdc0040` is in the same GPUVM region as the ring — structurally correct | 2dx |
| Fence packet is correct in **every field** (WRITE_DATA 0xC0033700, DST_SEL 5, WR_CONFIRM, dst = `+0xc8` = the right frame, data = the stamp) | 2dy |
| Frame filler is `0xFFFF1000` = PM4 NOP count 0x3FFF, AMD's standard padding | 2dy |
| A GPUVM fault IS latched, `CNTL=0x1ffc` so reporting is on | 2dz |
| Fault addr `0x0FFBFE80 << 12` == `CP_HQD_PQ_BASE 0xffbfe800 << 8` == `0xFFBFE80000` = the ring base | 2dz/2ea |
| The fault is **raised by the submission**: STATUS 0 at gc/kiq/pre, 0x9ba at wait/post | 2ea |
| Fault decode: MAPPING_ERROR=1, WALKER_ERROR=5, CID 4, RW=0 (read, no valid mapping) | 2ea |
| `GCVM_L2_PROTECTION_FAULT_STATUS` bit 0 is **MORE_FAULTS**, not a validity bit — a nonzero STATUS is the latch | 2eb |
| `CPF_UTCL1_STATUS` nonzero both boots while `CPC_UTCL1_STATUS` is 0 — the fetcher's translation unit | 2dz/2ea |

### Key addresses (X6000 unless noted)

```
waitForHwStamp            0xbe85536     checkForTimestampUpdate  0xbe851d6
timestampUpdated          0xbe85214     AMDHWChannel::init       0xbe848cc
submitKIQFrame            0xbe9572e     getKIQFrame              0xbe957a0
submitSetResourcesPacket  0xbec7404     writeDataCmdPacket       0xbec7642
startKIQ                  0xbec7688     KIQ frame filler         0xbec77f8
registerChannel           0xbea9a04     addHWChannel             0xbe93f98
allocateChannelWriteBackFrame 0xbea9a94  ...GPUAddr 0xbea9aaa  ...CPUPtr 0xbea9ad2
HWLibs: _smu_internal_sw_exit 0x1266813e   cosReleaseMemoryHandle 0x1265d9c0
```

GC registers (all BASE_IDX 0, seg0 = 0x1260; byte addr = (seg + dword) * 4):

```
GCVM_L2_CNTL                      0x15bc    GCVM_CONTEXT0_CNTL     0x15fc
GCVM_L2_PROTECTION_FAULT_CNTL     0x15c4    ..._STATUS             0x15c8
..._ADDR_LO32 / _HI32             0x15c9 / 0x15ca
GCVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_LO32/HI32   0x1667 / 0x1668
GCVM_CONTEXT0_PAGE_TABLE_START_ADDR_LO32/HI32  0x1687 / 0x1688
GCVM_CONTEXT0_PAGE_TABLE_END_ADDR_LO32/HI32    0x16a7 / 0x16a8
CP_HQD_PQ_BASE 0x1fb1 (ring VA >> 8)   CP_HQD_PQ_RPTR/WPTR_LO 0x1fb4 / 0x1fdf
CP_HQD_ACTIVE 0x1fab   CP_HQD_PQ_DOORBELL_CONTROL 0x1fb8
GRBM_GFX_CNTL 0x0dc2 (ME/PIPE/QUEUE/VMID selector for the CP_HQD_* window)
CP_STAT 0x0f40  CP_CPC_STATUS 0x0e24  CP_CPF_STATUS 0x0e27
CPC_UTCL1_STATUS 0x1f55  CPF_UTCL1_STATUS 0x1f56
GRBM_STATUS 0x0da4  GRBM_STATUS2 0x0da2
```

The KIQ is at ME 2 / pipe 1 / queue 0, VMID 0. Ring MC handed to startKIQ:
`0xf40fb28000` (carve-out). Ring VA in the MQD: `0xFF_BFE8_0000`. Writeback
fence frame: GPU `0xffbfdc0040`, same GPUVM region, 0xC0000 lower.

Channel layout: `+0x0c` index, `+0x20` hardware interface, `+0x80` submitted,
`+0x84` completed, `+0xac` fence slot index, `+0xb0`/`+0xc0` fence CPU VA,
`+0xc8`/`+0xcc` fence **GPU** address (set in `allocateMemoryResources`, not
`init`), `+0xe0` scheduler slot (-1 = direct fence path).

---

## 4. Open and unexplained

1. **Why is `0xFF_BFE8_0000` unmapped?** That is the live question. 2eb (§2)
   asks whether it is even inside VMID 0's page-table range. Note the ring
   memory itself is fine — `startKIQ` was handed MC `0xf40fb28000` in the
   carve-out — so this is a mapping problem, not an allocation one.
2. **2dq's conclusion is withdrawn in substance.** Its *measurement* stands
   (WPTR advanced, RPTR caught up, CP_STAT busy), but a faulting fetch
   advances RPTR having discarded the data. **Do not cite 2dq as proof the CP
   executes PM4.** Whether the CP has ever successfully executed a packet on
   this chip is currently unproven either way.
3. **`gfx_min = gfx_max = 0`** from the SMU DPMCLOCKS table, while every
   neighbouring field decodes correctly and GFX is not in GFXOFF. Struct
   layout was checked against Linux and is right, so these are real zeros:
   GFX DPM was never started. **This is a separate wall from the fence** and
   may still bite after the fence is fixed.
4. Peer requests **0x12** and **0x1D** are "Unhandled"; **0x1B** answers
   0xE00002C7. None blocks the current path.
5. The 2di teardown panic fix (`ctx+0x340` zeroing) is correct by reading but
   **never exercised on hardware** — the bringup has succeeded every boot
   since.
6. `2am` / `2an` / `2ao` force three "firmware ready" latches to report
   success unconditionally. They are still blanket-true and should be made
   per-caller and logged.

### Not started at all

Display. DCN 3.1.2 vs Navi 21's DCN 3.0; `IOAccelDisplayMachine::
found_framebuffer` is hooked to refuse the pipe (2ct) so armed boots survive.
`-rbgpudisplaypipe` restores Apple's behaviour. This is a second project of
comparable size — do not imply an accelerated desktop is near.

**`-rbgpunodisppanic` must stay OUT of boot-args.** It publishes
`IOPCITunnelled`, which is what killed the UI in 2cr–2cv.

---

## 5. The recurring error — read this before concluding anything

**Ten times** a conclusion has been drawn from a value without first
establishing what makes that value meaningful, or an instrument has dropped
the very result it was built to capture. Withdrawn or lost:
2dk ("seg0 decodes" — 0 at both bases), 2dl ("MEC fetched" — a fill pattern),
2dm ("queue never programmed" — wrong HQD window), 2do (PQ_BASE expected to
equal the MC address), 2dp (measured before the doorbell), 2dq (narrated its
own hook's substituted value as a hardware result), 2ds–2dv (read a jump
direction backwards and then hooked gates for a fault that did not exist),
2dw (13 small-integer hits read as a located write), and 2dz/2ea (tested
`status & 1` as a fault-valid bit when bit 0 is MORE_FAULTS, so a genuine
0x9ba fault was logged as "nothing latched").

The ninth one happened **inside the code written to prevent the other eight**.
Writing a precondition into the log does not help if the precondition itself
is guessed.

The tenth is the same silent-cap bug for the third time, in the third buffer:
the diag table at 96 (2db), at 192 (2ds), and now the **512-byte NVRAM
summary**, which truncated the 2eb boot mid-key at `mc` and lost all six
`vm_*` values. Every milestone appends its new keys to the END of the list,
which is precisely where truncation eats them — so the summary systematically
drops the one experiment it was written for. 2ec emits a **hot list first**
and prefixes `d=<n>` with the number of keys that did not fit, so a truncated
summary says so. When a milestone starts, its keys go in the hot list; when
it is answered, they move down.

Corollary that is procedural, not technical: **NVRAM is the fallback, not the
record.** The full 320-entry table lives in the boot's own IORegistry and dies
with it. Run `tools/plugin_check.sh` while still in the armed boot.

Operational rules that actually catch it:

- State in the code what would have to be true for the reading to mean
  anything, and **log it when it is not**. This caught the last three.
- A diagnostic that substitutes its own return value must never narrate that
  value as a hardware result.
- `!= 0` and `!= 0xffffffff` are not evidence a register decoded.
- Prefer a comparison with a second, independently-derived number. The ring
  fault is credible because two registers agreed to **40 bits** under their
  own conventions; the 2dw page scan was not, because 13 dwords in a page
  equalled 1.
- When Apple's code demonstrably works on Navi 21, a theory that it is
  structurally broken needs very strong evidence. Prefer chip-specific
  explanations (addresses, page tables, firmware versions).
- Diag keys and the log must BOTH carry a result. The 512-line ring wraps
  during the 5 s stamp wait and evicts ~500 lines; counters in NVRAM are what
  survive.

---

## 6. Procedure — who runs what

**Claude never runs** `tools/load.sh`, `tools/unload.sh`, `tools/efi.sh`, and
never asks for the password. Claude may commit freely; **pushing needs
asking**. Deliberate diagnostic panics are off the table (user's decision,
2026-10-12).

Claude's loop: implement → `make plugin` (hosttest must pass) → commit → give
the user the build UUID and the commands below → read the resulting log.

The user runs:

```
sudo tools/efi.sh install
sudo tools/efi.sh status          # check the UUID matches build/out
sudo nvram rbgpu-arm=1 && sudo reboot
```

Arming is **one-shot**: the peer consumes `rbgpu-arm` and flushes the removal
immediately, so a panic or hang comes back up unarmed. A plain reboot after an
armed boot is safe.

**While still in the armed boot** — over SSH (10.0.0.69) or at its desktop,
BEFORE rebooting:

```
tools/plugin_check.sh             # writes logs/<timestamp>-plugin/
```

This is the step that matters. `RBGPUDiag` (320 entries) and `RBGPULog` live
only in that boot's IORegistry and die with it; the NVRAM summary is a
512-byte fallback that cannot hold them. Skipping it is what lost 2eb.

Then, from any later boot:

```
nvram rbgpu-diag                  # this boot (d=<n> = keys that did not fit)
nvram rbgpu-diag-prev             # the previous boot (rotation keeps two)
```

Current boot-args (from the 22:54 install's config.plist backup — verify, do
not assume):

```
keepsyms=1 -vi2c-force-polling -lilubetaall msgbuf=1048576 -rbgpusync570
-rbgpuspoofttl amfi_get_out_of_my_way=0x1 amfi=0x80 alcid=66 -v -rbgpupsphw
-nvoff -wegnoegpu -rbgpusmubringup -rbgpustampwait
```

Plugin flags in use: `-rbgpusync570`, `-rbgpuspoofttl`, `-rbgpupsphw`,
`-rbgpusmubringup`, `-rbgpustampwait`. Off-switch `-rbgpuoff` rescues a
machine. `-rbgpudisplaypipe` and `-rbgpunodisppanic` are not set and should
stay that way.

**Before interpreting ANY boot, check all three:** installed UUID vs
`build/out`; whether `rbgpu-arm` was left set from an earlier attempt; which
plugin boot-args are actually in config.plist. Boots have been wasted on each.

### Reading a log

`logs/<ts>-plugin/peer.plist` holds `RBGPULog` (512-entry ring) and
`RBGPUDiag` (cap 320, logs loudly when full — absent now really means absent).
`tools/re/ptrsym.py` names pointers inside loaded AMD kexts.

---

## 7. The next lever is probably source comparison

The ring-fault lead did **not** collapse — 2ea dated it to the submission. But
whichever way 2eb's range test goes, grinding more GC registers is unlikely to
explain *why* Apple's `startKIQ` command-8 path leaves the KIQ ring at a GPUVM
address that does not translate on this chip.

The cheaper lever is reading how amdgpu does it for yellow_carp and comparing:
`gfx_v10_0.c` (KIQ/MQD setup, `gfx_v10_0_kiq_init_queue`, `nv_grbm_select`),
`amdgpu_gart.c` and `gmc_v10_0.c` (GART aperture, VMID 0 context setup,
`gmc_v10_0_vm_init`), plus `amdgpu_ring.c` for where the ring is expected to
live. The question to answer from source: for GC 10.3 on an APU, is the KIQ
ring addressed through GART/GPUVM or physically, and what sets up the mapping
Apple is assuming exists.

Precedent: the SMU address bug (2dh/2di) was solved exactly this way, by
reading `yellow_carp_ppt.c:159-164` and noticing Linux puts the SMU tables in
VRAM — not by another register probe.

Fetch Linux sources from torvalds/linux and record the commit in
docs/linux-ref/UPSTREAM.md.
