# RembrandtGPU — handoff state (2026-10-12, after 2ea)

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
active and doorbelled with its ring in the carve-out. `powerUpHW` still fails
at exactly one point: the KIQ's completion fence never lands, so
`waitForHwStamp` returns false, `doStart` aborts, engine 0 fails. The latest
boot found a **latched GPUVM read/permission fault whose address matches the
ring base exactly**, which is the strongest lead so far and is *not yet
confirmed to be ours* — that is what the pending build measures.

Nothing about display is started. The accelerator deliberately adopts **no
display pipe** (2ct) so armed boots reach a desktop.

---

## 2. The pending experiment — 2ea, build UUID `749D33AD`

Built, hosttest passes, committed (`98148a5`). An install was run at 22:54 on
2026-10-12, so this is probably already on the EFI — **verify with
`sudo tools/efi.sh status` against `dwarfdump --uuid
build/out/RembrandtGPU.kext/Contents/MacOS/RembrandtGPU` before interpreting
any boot.**

The question: the GPUVM fault registers **latch until cleared**, and
`MORE_FAULTS=1` says more than one fault occurred, so the fault seen at
`post-stamp-wait` could have been raised before `startKIQ` ever existed, by
something that is not ours. 2ea records status and address **per dump point**:

| diag keys | meaning |
|---|---|
| `vmf_st_gc` / `vmf_ad_gc` | at `post-gc-hw-init`, **before** startKIQ |
| `vmf_st_kiq` / `vmf_ad_kiq` | at `post-startKIQ` |
| `vmf_st_pre` / `vmf_ad_pre` | at `pre-kiq-submit` |
| `vmf_st_wait` / `vmf_ad_wait` | at `at-stamp-wait`, after the doorbell |
| `vmf_st_post` / `vmf_ad_post` | at `post-stamp-wait`, after the 5 s wait |

Outcomes:

- **Already latched at `vmf_st_gc`** (bit 0 set) → pre-existing, not caused by
  the KIQ. The ring-fault lead collapses and the trail restarts. Do not keep
  arguing for it.
- **First latched at `vmf_st_wait` or `_post`** → the KIQ's own ring fetch is
  what GPUVM refused. That is the finding, and the next step is the page
  tables / VMID for that ring, not more register archaeology.

---

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

Channel layout: `+0x0c` index, `+0x20` hardware interface, `+0x80` submitted,
`+0x84` completed, `+0xac` fence slot index, `+0xb0`/`+0xc0` fence CPU VA,
`+0xc8`/`+0xcc` fence **GPU** address (set in `allocateMemoryResources`, not
`init`), `+0xe0` scheduler slot (-1 = direct fence path).

---

## 4. Open and unexplained

1. **The pending 2ea question** (§2) — is the ring fault ours?
2. **2dq's interpretation is open.** Its *measurement* stands (WPTR/RPTR/
   CP_STAT). But a ring fetch that faults can advance RPTR having discarded
   the data, so "the CP executed the packet" may be "fetched, faulted,
   discarded". Do not cite 2dq as proof the CP executes PM4 until the fault
   is dated.
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

**Eight times** a conclusion has been drawn from a value without first
establishing what makes that value meaningful, and had to be withdrawn:
2dk ("seg0 decodes" — 0 at both bases), 2dl ("MEC fetched" — a fill pattern),
2dm ("queue never programmed" — wrong HQD window), 2do (PQ_BASE expected to
equal the MC address), 2dp (measured before the doorbell), 2dq (narrated its
own hook's substituted value as a hardware result), 2ds–2dv (read a jump
direction backwards and then hooked gates for a fault that did not exist),
2dw (13 small-integer hits read as a located write).

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

After the boot, over SSH (10.0.0.69) or at the desktop:

```
tools/plugin_check.sh             # writes logs/<timestamp>-plugin/
nvram rbgpu-diag                  # this boot
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

## 7. If the ring-fault lead collapses

If 2ea shows the fault was already latched before `startKIQ`, stop doing
register archaeology on this path. The cheaper next lever is **source
comparison**: read how amdgpu sets up the KIQ's VMID, page tables and ring
mapping for yellow_carp (`gfx_v10_0.c`, `amdgpu_gart.c`, `gmc_v10_0.c`) and
compare against what Apple's path actually leaves configured. The SMU address
bug (2dh/2di) was solved exactly that way — by reading
`yellow_carp_ppt.c:159-164` and noticing Linux puts the tables in VRAM.
