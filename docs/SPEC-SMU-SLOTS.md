# B3 in detail: SMU 13 behind Apple's SMU slots

What the hook layer has to do so AMDRadeonX6800HWLibs runs its power
management on the 680M's MP1 13.0.3 through `src/common/rb_smu13.{c,h}`.
Everything here was read from macOS 15.8.1's AMDRadeonX6800HWLibs (kexts
7.0.25) with tools/re (`switches.py` decodes the command tables). Addresses are
file addresses of the extracted kext (`build/re/AMDRadeonX6800HWLibs`); resolve
by symbol name, never by address. All names below are in the kext's symbol
table (local `t` symbols).

This item is only reached once HWServices loads X6800HWLibs for 0x1681 and
discovery passes (APPLE-STACK.md A1-A3, B1, B2). It is not the first hook.

## 1. How HWLibs reaches the SMU

```
ipi_smu_create_context            IP block 8; IpiSetSwipEntryPoints(8, {sw_init, sw_exit, hw_init, hw_exit})
  smu_sw_init(ctx, ...)           allocates the SMU context (below), then
    smu_init_function_pointer_list(ctx, ver, rev)     <-- fails today: "Unsupported hw version!"
    smu_internal_sw_init          -> slot +0x650, then smu_get_fw_constants -> slot +0x738
  smu_hw_init(ctx, ?, iface)      -> slot +0x658; then fills `iface` with the dispatchers
  smu_hw_exit / smu_sw_exit       -> slots +0x668 / +0x660
ipi_smu_notify_event              -> iface event_notify -> slot +0x680
ipiSmuPerformFullAsicReset        -> iface full_asic_reset -> slot +0x678
```

Outside the SMU code, HWLibs calls only those two interface entries (events
and full reset). The other dispatchers (`smu_dpm_control`,
`smu_power_control`, `smu_gfx_off_control`, ...) are published in the
interface table but no HWLibs code calls them. Whether the accelerator kext
calls them is open (section 8).

### Why it fails

`smu_get_hw_version(ver, rev)` packs `major<<16 | minor<<8 | rev`, where
`major = ver & 0xff`, `minor = (ver >> 32) & 0xff`, `rev = rev & 0xff`, and maps
the result to an index:

| MP1 | index | MP1 | index |
|---|---|---|---|
| 9.0.0 | 0x00 | 11.0.13 | 0x0A |
| 9.0.1 | 0x01 | 11.5.0 | 0x0B |
| 10.0.0 | 0x02 | 12.0.0 | 0x0C |
| 11.0.0 | 0x03 | 12.0.1 | 0x0D |
| 11.0.2 | 0x04 | 13.0.0 | 0x0E |
| 11.0.5 | 0x05 | 13.0.1 | 0x0F |
| 11.0.7 | 0x06 | **13.0.3 (680M)** | **0x10** |
| 11.0.9 | 0x07 | 13.0.4 | 0x11 |
| 11.0.11 | 0x08 | 13.0.5 | 0x12 |
| 11.0.12 | 0x09 | anything else | 0x14 |

`smu_init_function_pointer_list` stores the index at ctx+0x278, zeroes the
slots (ctx+0x650, 0xF0 bytes), calls `smu_read_config_space(ctx)`, and then:
- if `ctx+0x288 & 1` is clear, it accepts only indexes 6, 8, 9 and 10 (mask
  0x740) and calls `smu_11_0_7_create_function_pointer_list` for them;
  anything else calls `smu_assertion` ("Unsupported hw version!") and
  returns 2;
- if `ctx+0x288 & 1` is set, it builds the list only when bit 1 is also set
  and the index is 6; otherwise it returns 0 with every slot empty;
- in both cases, if `ctx+0x2e8 == 1`, it then calls `smu_update_function_pointers`.

## 2. The hook (H-SMU-1)

Replace `smu_init_function_pointer_list(ctx, ver, rev)`:

1. Compute the index with Apple's own `smu_get_hw_version(ver, rev)` (pure, no
   side effects).
2. Not 0x10: call the original unchanged and return its result. Every other
   ASIC keeps Apple's path.
3. 0x10: do what the original does on success, with our slots:
   - store 0x10 at ctx+0x278;
   - zero ctx+0x650 .. ctx+0x740;
   - call `smu_read_config_space(ctx)`;
   - fill the slots of section 5 (leave the rest zero);
   - return 0.

   Do not call the original first: its failure path runs `smu_assertion`,
   which hands the message to an OS-services callback (ctx+0x10 -> +0xF8)
   whose behaviour was not traced. Skip `smu_update_function_pointers` too:
   ctx+0x2e8 == 1 is an emulation mode, which a 0x1681 never uses.

The `-rbgpuoff` boot argument must leave this hook uninstalled.

## 3. The SMU context (what the slots receive as `ctx`)

| Offset | Meaning | Used for |
|---|---|---|
| +0x008 | register services (CGS), see below | registers, GPU memory |
| +0x010 | OS services, see below | delay, system memory, logging |
| +0x044 | u32 array: register base (dword) per base index | `smu_cgs_*_register` adds `[idx]` |
| +0x278 | u32 MP1 version index (0x10 for us) | set by the hook |
| +0x27C | u32 register-window limit (bytes) | bounds check |
| +0x280 | u8: bounds check enabled | |
| +0x288 | u16 flags (bit 0, bit 1: see section 1) | |
| +0x2C0 | u32 SMU 11 response timeout | not needed |
| +0x360 | SMU 11 table context (pptable, FB allocations) | **not ours, do not use** |
| +0x650 .. +0x738 | the 27 slots | section 5 |

Our own state (an `struct rb_smu13` plus the driver-table handle) has no field
in this context. Keep it in the plugin, keyed by `ctx` (one GPU: a single
static is enough). Do not reuse +0x360: common code
(`smu_query_feature_enabled`, `smu_svc_*`, the dispatchers) reads it.

### Register services (`*(ctx+0x8)`)

| Offset | Function | Signature as `smu_cgs_*` calls it |
|---|---|---|
| +0x08 | handle | first argument of every call below |
| +0x10 | allocate GPU memory | `(handle, req*, out*)`; `smu_cgs_alloc_memory` wraps it |
| +0x38 | write register | `(handle, dword_index, value, flags)` |
| +0x40 | read register | `u32 (handle, dword_index, flags)` |
| +0xB8 / +0xC0 | write / read beyond the window (indirect) | not needed |

Apple's own SMU 11 code goes through the wrappers
`smu_cgs_read_register(ctx, reg, base_idx, flags, 4)` and
`smu_cgs_write_register(ctx, reg, base_idx, value, 4, 0)`. They add
`ctx+0x44[base_idx]` to `reg` and pass the absolute dword index to +0x40 / +0x38.
`smu_11_0_7_send_message` uses base index 0 with 0x282 / 0x292 / 0x29A. These
are the same C2PMSG_66 / 82 / 90 registers that rb_smu13 uses, so
`ctx+0x44[0]` should be MP1's base (0x16000 on this machine).

### OS services (`*(ctx+0x10)`)

| Offset | Function |
|---|---|
| +0x08 | handle |
| +0x10 | allocate system memory (`smu_cos_alloc_memory`) |
| +0x80 | `delay_us(handle, us)` (`smu_cos_delay_in_us`) |
| +0xF8 | assertion / log callback (`smu_assertion`) |

## 4. Gluing rb_smu13 to the context

- `struct rb_hw` for `rb_smu13_init`:
  - `rreg(byte_off)`: call the CGS read (+0x40) with `byte_off / 4`.
  - `wreg(byte_off, v)`: call the CGS write (+0x38) with `byte_off / 4`.
  - `delay_us`: `smu_cos_delay_in_us`.
  - `mmio_len`: ctx+0x27C.
  - rb_smu13 only ever writes the three mailbox registers.
- `struct rb_disc` for `rb_smu13_init`: a two-entry table built by hand.
  - MP1, version 13.0.3, `base[0] = ctx+0x44[0]`.
  - SMUIO, `base[0] = 0x16800`, only for `rb_smu13_get_gfxoff_status`. The
    SMUIO base is not in the SMU context; leave the entry out and that one
    call returns `RB_SMU13_E_INVAL`.
- Driver table (`struct rb_smu13_dma`): allocate it once, in the sw_init slot,
  with `smu_cgs_alloc_memory(ctx, 0x1000, 0x1000, 1, &handle, &gpu_addr, &cpu_addr)`.
  This is the call, size class, alignment and domain (1) Apple uses for its
  own driver table (`smu_11_0_7_core_initialize_smc_tables`: size 0x684,
  align 0x1000, domain 1).
  - `mc = gpu_addr`, `cpu = cpu_addr`, `size = 0x1000`.
  - `flush` / `invalidate`: NULL for the first test (see section 7).
- Message groups: `rb_smu13_init` allows queries only. Which other groups to
  allow is a decision for the user (section 6).

## 5. Slots

Return codes the dispatchers and callers expect, from Apple's slots:
0 OK, 1 bad parameter, 2 failure, 3 timeout (`smu_11_0_7_send_message`), and 4
not supported. A dispatcher whose slot is NULL returns 4 itself.

rb_smu13 errors map to these as follows:

| rb_smu13 error | Apple code |
|---|---|
| `E_INVAL` | 1 |
| `E_FW`, `E_VERSION`, `E_NODATA` | 2 |
| `E_BUSY`, `E_TIMEOUT`, `E_DEAD` | 3 |
| `E_NOT_ALLOWED` | 4 |

| Slot | Apple name | Signature | Ours |
|---|---|---|---|
| +0x650 | internal_sw_init | `(ctx, ...)` | Allocate our state and the driver table (section 4); `rb_smu13_init`. Return 0, or 2 if allocation fails. |
| +0x658 | internal_hw_init | `(ctx)` | `rb_smu13_bringup`: versions (interface must be 4), feature mask, driver table address, DPM clock table. Linux's APU path; none of Apple's dGPU steps (VBIOS pptable, DC BTC, memory pool) apply. |
| +0x660 | internal_sw_exit | `(ctx)` | Free the driver table and our state. |
| +0x668 | internal_hw_exit | `(ctx)` | Apple: unload message, then table teardown. Ours: `rb_smu13_prepare_unload` (needs ALLOW_UNLOAD). Linux skips it when entering s0ix. |
| +0x678 | full_asic_reset | `(ctx, ...)` | Leave NULL (4). Linux's APU reset is mode 2 (`rb_smu13_mode2_reset`); wire it only after a reset is understood end to end. |
| +0x680 | notify_event | `(ctx, struct {u32 size=0x10; u32 event; u64 *param}*, ...)` | SMU events 0, 4, 6, 10: Apple runs `power_up` (re-init after resume); ours runs `rb_smu13_bringup` again (Linux re-runs hw setup on resume). Event 9: Apple switches DF C-states (no Yellow Carp equivalent): return 0. Event 11 (debug dump): return 0. Others: 0. Event > 11: 2. |
| +0x6E0 | gfx_off_control | `(ctx, u32 op)` | op 0 enable, 1 disable, else 4. Enable: `rb_smu13_gfx_off_control(1)`, preceded once by `rb_smu13_enable_gfxoff` (Linux's post_init). Disable: `rb_smu13_gfx_off_control(0)`. Needs ALLOW_GFXOFF. |
| +0x6E8 | system_features_control | `(ctx, u64 *mask)` | Apple enables DPM features. On Yellow Carp the firmware already runs them all (47 enabled on this machine) and Linux sends nothing on enable. Return 0. |
| +0x6F8 | power_feature_caps | `(ctx, u64 *caps)` | Apple ORs capability bits from SMU 11 feature bits. Start with `*caps = 0`, return 0, and map individual bits only once a caller is seen reading them. |
| +0x720 | unload_smu | `(ctx, ...)` | `rb_smu13_prepare_unload`. |
| +0x738 | get_ucode_consts | `(ctx, out[16])` | Leave NULL. It describes SMU firmware for the driver to load, and on an APU the firmware is already running. `smu_get_fw_constants` then reports "no firmware" (to verify, section 7). |
| +0x6B8 | dpm_control | `(ctx, u32 cmd, in*, out*)` | NULL first (section 8 lists Apple's commands and the rb_smu13 call each maps to). |
| +0x6C0 | power_control | `(ctx, u32 cmd, in*, out*)` | NULL first. |
| others | overdrive, thermal, fan, ips, azalia, ulv, fullscreen, soft_table, pm_log, push_to_stb, dummy_hw_init | | NULL (4). |

## 6. Decisions for the user before the first boot with this hook

Each RB_SMU13_ALLOW_* group changes what the hook may send. Minimum for
bring-up:

- **ALLOW_TABLES**: sets the driver table address and copies the DPM clock
  table into it. It changes only where the SMU writes table data, into memory
  the driver owns.
- **ALLOW_GFXOFF** (needed for slot +0x6E0): lets the SMU power the graphics
  core off when idle. Linux does this on every boot of this chip.
- **ALLOW_UNLOAD** (+0x668 / +0x720): tells the SMU the driver is going away.
  It runs only at shutdown or driver unload.

Leave CLOCKS, MMPOWER and RESET off until a caller needs them.

## 7. To check on the first boot (log each one)

1. `ctx+0x44[0]` is 0x16000 and `ctx+0x27C` covers it (the glue's MP1 base).
2. `smu_cgs_alloc_memory` returns 0, and `gpu_addr` lies in the carve-out
   (0xF4_0000_0000 - 0xF4_FFFF_FFFF).
3. `rb_smu13_bringup` returns 0, and the DPM table looks sane: GFX min/max
   in MHz, 1-8 SOC levels, 1-4 DF P-states.
4. The DPM table read back unchanged with NULL flush/invalidate. If it is all
   zero or stale, add HDP flush/invalidate (Linux: `amdgpu_hdp_flush` /
   `_invalidate`), whose Apple equivalent is not traced yet.
5. `smu_get_fw_constants` with slot +0x738 empty: sw_init still succeeds.
6. Which dispatchers return 4, and who called them, if anything shows up in
   the log.

## 8. Open: the control commands

Apple's command tables (`tools/re/switches.py`), for when a caller turns up:

`dpm_control` (+0x6B8):
- 0 force highest, 1 force lowest, 2 unforce: `rb_smu13_set_performance_level` HIGH / LOW / AUTO.
- 3 get clock limits (in, out), 0xA get all queried clock limits: `rb_smu13_get_dpm_ultimate_freq`.
- 4 set clock limit (in), 0xB set all clock limits: `rb_smu13_set_soft_freq_range`.
- 5 get current DPM (out): `rb_smu13_get_current_clk`.
- 6 set workload policy; 7/8 UCLK fast switching on/off; 0xC get DPM table: `rb_smu13_get_dpm_freq_by_index`.
- 0xD voltage limits; 0xE PCIe parameters (no PCIe link on an APU); 0x12 copy of 6 pptable dwords; 0x13 disable DPM prefetcher.
- 9, 0xF-0x11: assertion (invalid).

`power_control` (+0x6C0): 0 power source (AC/DC), 1/2/3 set / get / restore
power limit, 4 query feature enabled, 5 send message, 6/7 deep sleep on/off,
8/9 power gating on/off, 0xA power limit by DPM, 0xB/0xC send message with
parameter, 0xD/0xE graphics power optimisation off/on, 0xF SmartShift delta gain.

The structs behind `in` / `out` are not traced. Trace them only for commands
seen being called.
