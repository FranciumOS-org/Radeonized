# Radeon 680M (Rembrandt) on macOS 15.8.1 — the CP halt

Hardware: ASUS TUF A15 FA507NU, Ryzen 7 7735HS, Radeon 680M `1002:1681` rev 0a.
GC 10.3.3 / DCN 3.1.2 / PSP+SMU 13.0.x / SDMA 5.2.3, one SDMA instance.
macOS 15.8.1 (24H32), OpenCore, `csr-active-config 0xA03`.

---

## 1. How the driver works

**Spoofed Navi 21 personality driving Apple's own shipping kexts, patched at
runtime. No custom accelerator and no custom Metal driver.**

A Lilu kernel plugin (`RembrandtGPU.kext`, loaded from the EFI right after
Lilu) hooks four Apple kexts:

| kext | role |
|---|---|
| `AMDRadeonX6000` | the Navi 21 accelerator (`IOGraphicsAccelerator2` subclass) |
| `AMDRadeonX6800HWLibs` | per-IP firmware/HW library, dispatches through `*_init_function_pointer_list` tables |
| `AMDRadeonX6000HWServicesNavi` | the HW services layer |
| `IOAcceleratorFamily2` | the generic accel family |

Patching is via Lilu's `KernelPatcher::RouteRequest` / `routeMultiple` /
`solveSymbol`. WindowServer composites through Apple's own
`AMDRadeonX6000MTLDriver` — confirmed from a live stack
(`CaptureSurfaceMetal::Populate` → `GFXAAMD_MtlDevice
newTextureWithDescriptor:iosurface:`), not from the screen.

The 680M itself is not claimed by AMD's display driver: boot-args carry
`-wegnoegpu`, and `IONDRVFramebuffer` + `AMDSupport` stay attached to
`IGPU@0` on the firmware framebuffer. Our plugin sits beside that.

Two hard-won routing rules, both paid for with kernel panics:

- Never route a symbol unless `nm -m` shows it **defined** in that kext.
  `solveSymbol` does not skip imports — it returns the bare slide and
  `routeMultiple` writes a jump there.
- Refuse any symbol whose first 5 bytes contain a rel8/rel32 branch. Lilu
  copies displaced instructions into a trampoline byte-for-byte, so a short
  branch keeps its original displacement and lands inside Lilu's buffer.

Current experiment boot-args (each proven on hardware, not guessed):

```
-rbgpusync570 -rbgpuspoofttl -rbgpupsphw    TTL/PSP path
-rbgpusmubringup -rbgpuptbase               SMU link + GART PT_BASE fix
-rbgpustampwait                             bounded stamp wait
-rbgpusdmaone -rbgpuvmpteng1                one SDMA instance, engine-2 → 1
-rbgpuforce1b -rbgpuforceenable  wdt=-1     drives 0x1B, sets fEnabled
-rbgpurlcsub                                the RLC sub-images (see §2)
-rbgpunocgcg                                CGCG off (takes; does not fix)
```

`-rbgpuforceenable` sets `fEnabled` (bit `0x2` at `accel+0xc78`) directly,
because `IOAccelDisplayMachine::display_mode_did_change` never fires — we
refuse the display pipe by design. Without it every resource creation sleeps
forever in `IOGraphicsAccelerator2::acceleratorWaitEnabled` and the screen is
black. With it the lock screen draws and a login is accepted.

---

## 2. Firmware blobs and how they are loaded

The chip's own Linux `yellow_carp_*` firmware, substituted into Apple's
HWLibs by hooking its `_IpiSetFwEntry` path and swapping the payload by entry
id. Apple's `_gc_10_3_get_fw_constants` (0x126b4254) asks for **21** entries.

**CP microcode** — PFP / ME / CE / MEC. Verified *on the chip*, not assumed:
each reads 64/64 nonzero dwords back out of its instruction RAM.
`yellow_carp_mec2.bin` is on disk and **not** currently supplied; whether GC
10.3 wants a MEC2 entry is unverified.

**RLC** — this was the one that mattered. `yellow_carp_rlc.bin` is a
`rlc_firmware_header_v2_2` (ip 10.3). We parse the header and carve six
payloads:

| payload | offset | size | entry id |
|---|---|---|---|
| main ucode | `0x100` | `0x6200` | (already supplied) |
| `srlist_cntl` | `0x6300` | `0x250` | 26 |
| `srlist_gpm` | `0x6550` | `0x600` | 27 |
| `srlist_srm` | `0x6b50` | `0x4880` | 28 |
| `rlc_iram` | `0xb3d0` | `0x10200` | 29 |
| `rlc_dram` | `0x1b5d0` | `0x10200` | 30 |

`0x1b5d0 + 0x10200 = 0x2b7d0`, the exact file length — the carve is
self-checking. Three of the ids were pinned by exact size match against our
own `_IpiSetFwEntry` log.

**This is what fixed `BOOTLOAD_COMPLETE`.** We had been supplying 7 of 21
entries, so the RLC was booting **Navi 21's** `rlc_lx6_iram`/`rlc_lx6_dram` —
the firmware for the processor *inside* the RLC that runs the autoload — plus
Navi 21's three save/restore lists. `RLC_RLCS_BOOTLOAD_STATUS` read
`complete=0` for twelve consecutive boots. With the yellow_carp sub-images it
reads `bl=c0000001 iram=1 complete=1`, and `CP_ME_CNTL` / `CP_MEC_CNTL` both
read **0** at bring-up where they had read `15000000` / `50000000`.

**SMU** — SMU 13.0.x. `yellow_carp_ppt.c` ported to freestanding C
(`src/common/rb_smu13.c`), tested against a simulated SMU whose tables are
Linux's own structs. Protocol is `smu_msg_v1_send_msg` over MP1
`C2PMSG_66/82/90`. Live: 7 messages, 15 writes, 0 refused, `bringup_err=0`.
Note `gfx_min = gfx_max = 0` — the SMU reports no GFX clock range and GFX DPM
never started. Still unexplained, but **not** the cause of this hang: the CP
has been measured clocked and executing.

**PSP** — `-rbgpupsphw`. This part is PSP + autoload
(`load_type == PSP && psp.autoload_supported`), so the PSP places the
firmware and the autoload is what sets `BOOTLOAD_COMPLETE`. Apple's 10_3 path
has **no backdoor fallback**: told the latch is clear, bring-up stops before
the SMU rather than taking another route.

**VBIOS** pulled from the ACPI VFCT table. Firmware is gitignored.

---

## 3. GFXOFF and power gating

All measured at the moment of the halt, in a 50 ms sampling window — not read
late and not assumed.

**GFXOFF: not engaged.** `SMUIO_GFX_MISC_CNTL`'s `PWR_GFXOFF_STATUS` reads
**2** = "Not in GFXOFF" (`yellow_carp_ppt.c:625-628`) at **both** samples
straddling the halt, in five consecutive boots. We do not actively disable it.
An earlier exclusion of GFXOFF was withdrawn as unsound because it rested on a
read taken ~160 s in; this one is at the halt.

**Power gating: off.** `RLC_PG_CNTL = 0` in every boot. `RLC_GPM_STAT` shows
`GFX_POWER_STATUS`, `GFX_CLOCK_STATUS` and `GFX_PIPELINE_POWER_STATUS` all
set at the halt.

**Clock gating: on by Apple's default, and we now turn it off.**
`_gc_set_clock_gating_feature_flag_10_3` (0x126b4231) is nine instructions:

```
dev+0xd48 = (dev+0x2dc & dev+0x2e0) | (0x7cf & ~dev+0x2e0)
```

`0x7cf` includes `AMD_CG_SUPPORT_GFX_CGCG` (bit 1) and `_CGLS` (bit 2), and
HWLibs ships `_gc_update_coarse_grain_clock_gating_10_1` with **no `_10_3`
variant** — the same gap it has for the autoload config. Live,
`RLC_CGCG_CGLS_CTRL` reads `0x363f`: `CGCG_EN`, `CGLS_EN`, idle threshold 54.

We write it (and `RLC_PG_CNTL`) to 0 on 24 ticks across 2.0–11.2 s, as Linux's
non-PSP `gfx_v10_0_rlc_resume` branch does. **The write takes** (`refused=0`,
`0x363f → 0`, held to 11.5 s) and it measurably changes the RLC's behaviour —
`RLC_GPM_STAT`'s `CNTX_IDLE_BEING_PROCESSED` stops setting entirely. **The CP
still halts at the same time.** CGCG is refuted.

**RLC save/restore machine:** `RLC_SRM_CNTL = 3` (`SRM_ENABLE` +
`AUTO_INCR_ADDR`). Writing 0 moves the halt ~1.5 s *earlier* but does not stop
it. (n=1, and "we destabilised something else" is not excluded.)

Note: our own `-rbgpunogates` only disables an instrument of ours. It has
never touched chip gating — the name misled us for a while.

---

## 4. The CP halt — what triggers it is the open question

**We have not found the writer. This is the frontier.** Everything below is
measured.

`CP_ME_CNTL = 0x15000000` — bits 24/26/28 = `CE_HALT | PFP_HALT | ME_HALT`,
and **every `*_PIPE*_RESET` bit (16–21) is clear.** That is the CP's power-on
value, and exactly what Linux's `gfx_v10_0_cp_gfx_enable(adev, false)` writes.

**Timing.** The CP comes up un-halted and is re-halted at **8.63–8.87 s**,
reproducible across a dozen boots (8827, 8835, 8839, 8865, 8830, 8688, 8634 ms
…). Deterministic, not a race.

**The transition, at 50 ms resolution, is one sample:**

```
OK   @8715ms  cps=0          grbm=3028      me=0          rbb=ffbfe000 rbc=a00e10
HALT @8765ms  cps=90038600   grbm=a0003028  me=15000000   rbb=ffbfe000 rbc=a00e10
```

- `CP_STAT 0 → 0x90038600` — `CP_BUSY`, `PFP_BUSY`, `ME_BUSY`, `MEQ_BUSY`,
  `ROQ_RING_BUSY`, `ROQ_INDIRECT1_BUSY`, `ROQ_CE_RING_BUSY`.
- `GRBM_STATUS 0x3028 → 0xa0003028` — gains `GUI_ACTIVE` and `CP_BUSY`.
  `0xa0003028` is the value every later sample of every boot reads;
  `0x3028` is clean idle. This 50 ms is the moment the machine wedges.
- `CP_STAT = 0` beforehand means even `CPF_BUSY` was clear — the chip reaches
  a genuinely clean state 50 ms before it wedges.

**The CP block stays alive across the halt.** `CP_RB0_BASE` (`ffbfe000`) and
`CP_RB0_CNTL` (`a00e10`) keep Apple's programmed values, in three independent
boots. So the halt value is **real and written**, not a dead block reading back
its defaults. (We tested the opposite hypothesis explicitly; it is refuted.)

**No host path wrote it.** Apple's `_gc_halt_micro_engines_10_3` (0x126b3616)
and `_gc_unhalt_micro_engines_10_3` (0x126b36df) are hooked and counted: the
halt path never runs. Every write to `CP_ME_CNTL` from any host path — our
`g_hw`, Apple's register accessor, and Apple's own driver code — writes **0**.
Apple's write path was disassembled (0xbe8d28a: direct store below the window,
`MM_INDEX`/`MM_DATA` above, no filter), so writes do reach the register.

**`CP_ME_CNTL` is specifically arbitrated away from the host**, while its
neighbours in the same block are not: every write to it is refused, and
`CE_STEP` (an ordinary R/W debug bit) will not stick, yet `CP_RB0_*` hold
their values throughout.

**Eliminated, each with a positive control at the halt:** GFX power gating ·
soft reset (`GRBM_SOFT_RESET = 0`) · RLC safe mode · a *running* RLC
(`RLC_CNTL` 1→0, clean restore, halt not released) · GRBM misrouting
(`GRBM_GFX_CNTL = 0`, `GRBM_GFX_INDEX` idx 0, SE0 present) · RLCG (SRIOV-only;
`amdgpu_rlc_wreg` is a plain `amdgpu_device_wreg` and gfx10's RLCG range is
empty) · Apple's write path · **CGCG** · **GFXOFF** · **the SRM**.

**Remaining candidates: the CP executing a PM4 packet, or the RLC.**

For the first: we map the GFX ring through its PTE and dump it.
`CP_RB0_BASE = 0xffbfe000` is page-aligned and `wptr = 0x380` (896 dwords), so
the whole submitted stream fits in the 1024-dword page we read — and it
contains **no** `WRITE_DATA` / `SET_CONFIG_REG` / `SET_UCONFIG_REG` targeting
dword `0x0F56`. But there are 3–4 `INDIRECT_BUFFER` packets, so that is not
conclusive. The first IB is at MC `0xF4_0FBE_9000` (carve-out range), which
maps to physical `0x7_8FBE_9000` via `amdgpu_gmc_vram_mc2pa` with
`vram_start = FB_LOCATION_BASE << 24` and
`vram_base_offset = FB_OFFSET << 24` — same address in every boot. It maps
cleanly, but its contents are too sparse to validate as a command buffer
(declares 952 dwords; 183 nonzero; 59 type-3 headers, ~the density random data
gives), so **no conclusion is drawn from it yet**. A packet chain-walk
validator is the next step.

If the IBs come back genuinely clean, the remaining candidate is the RLC's own
firmware — which an `RLC_CNTL` stop cannot rule out, because a latch set at
reset and cleared only by the autoload completing would survive it.

---

## 5. The log, from lock screen to hang

Two caveats first, because the log will disappoint:

- Our published log buffer ends at the last *refresh*, not at the stopping
  point, so its last line does not locate the hang.
- The refresh loop stops at ~73 s in every boot, armed or not. The actual
  evidence channel for this work is a set of NVRAM notes with per-boot nonces,
  captured over SSH from inside the armed boot.

What the live log does show is that **the KIQ works** while RB0 is halted —
stamps advance and fences land:

```
2cx: waitForHwStamp ENTER (ch=0xffffff86ca0b1700 stamp=112) - 5 s timeout per poll
2ds: channel stamps BEFORE the wait: submitted(+0x80)=112 completed(+0x84)=111
2dy: channel+0xc8 (what the WRITE_DATA packet targets) = 0xffbfde0040 ;
     getChannelWriteBackFrameGPUAddr(1) = 0xffbfde0040 -> they MATCH
2en: BOUNDED wait satisfied after 1 polls (~100 us, cap 2000 ms): +0x84 reached 112
2dr: the REAL wait returned 1 - THE STAMP ARRIVED
2dw: the fence dword at +0xc0 AFTER the wait = 0x00000070 -> the GPU wrote the
     fence where the CPU polls - this is the working path
2cx: waitForHwStamp -> 1 (the GPU executed it) (45245 ticks)
```

…then the rest of the buffer is one line, 1392 times:

```
RembrandtGPU: SpecialAMDKey request 0x14 (wait: 1) [#1341504]
RembrandtGPU: SpecialAMDKey request 0x14 (wait: 1) [#1341568]
...
```

That is the spin. The structure behind it, from a live spindump of the hung
boot (`logs/20261018-010521-metal/`) and from a panic report
(`logs/20261018-004318-WSHANG-ARMED-2hg/`):

```
thread AMDRadeonX6000_AMDAccelEventMachine
  hardwareErrorEvent
    restart_channel
      IOAccelFIFOChannel2::restart
        AMDAccelChannel::resetHardwareAndReplay
          waitForTimestamp
            IAMDHWInterface::pollWait        <- polls forever
```

WindowServer blocks on the mutex that thread holds (the spindump names the
turnstile). A Metal submit blocks behind the same restart via
`waitForRingBufferSpace`. Three hangs across two boots, one cause: **the GPU
retires no timestamps on the client channel, and the restart path cannot
recover because it waits on the same timestamp.**

The reproducer no longer needs the compositor: a 4096-thread compute kernel
with no surface and no display in the path hangs in `waitUntilCompleted`.

Register snapshots at +12 s / +100 s / +160 s, current configuration:

```
p=0  r=b1  w=280  cps=94038600  grbm=a0003028  me=15000000  rbb=ffbfe000
p=1  r=b1  w=280  cps=90018600  grbm=a0003028  me=15000000
p=2  r=231 w=300  cps=90018600  grbm=a0003028  me=15000000
```

Also worth flagging for anyone reading GCVM faults on this part: **MMHUB's
fault status register reads all-ones**, and Apple's `getVMFaultInfo` fills its
struct whenever `(addr != 0 || status != 0)`, so Apple reports a **phantom
hub-1 fault on every poll** (`vmid 15`, `cid 511`, `VA 0xFFFFFFFFF000`) and
feeds it to the real fault path. Half of what Apple's fault handler processes
on this chip is that phantom.

---

## The question

Something on the chip writes `CP_ME_CNTL = 0x15000000` about 8.7 s into every
boot, on a live CP block, with no host write and with the autoload latch
staying set. The whole host side is eliminated with positive controls. Is
there a known GC 10.3 mechanism — RLC firmware behaviour, a PSP action, a CP
microcode fail-safe on a fetch fault — that halts CE/PFP/ME this way and that
a host cannot clear?
