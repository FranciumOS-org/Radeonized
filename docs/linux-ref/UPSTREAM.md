# Linux reference files

Unmodified copies from torvalds/linux `master`, fetched 2026-10-03 at commit
`a74306e2e676f9775457366fc047a660fbf02f26`. MIT-licensed (AMD); see each file's
header. Used as the source of truth for the discovery-table layout and the
register offsets in `src/common/rb_regs.h`. Not compiled.

| File | Upstream path |
|---|---|
| discovery.h | drivers/gpu/drm/amd/include/discovery.h |
| amdgpu_discovery.h | drivers/gpu/drm/amd/amdgpu/amdgpu_discovery.h |
| amdgpu_discovery.c | drivers/gpu/drm/amd/amdgpu/amdgpu_discovery.c |
| soc15_hw_ip.h | drivers/gpu/drm/amd/include/soc15_hw_ip.h |
| mp_13_0_2_offset.h | drivers/gpu/drm/amd/include/asic_reg/mp/mp_13_0_2_offset.h |

The large GC 10.3.0, MMHUB 2.3.0 and DCN 3.1.2 offset headers are not copied
(1.5 MB each); the few registers taken from them are listed with their source in
`src/common/rb_regs.h`.

Added 2026-10-04, from master at `7704c4c5bb127673b4f0ead839919db573559e38`:

| File | Upstream path |
|---|---|
| smu_v13_0_1_ppsmc.h | drivers/gpu/drm/amd/pm/swsmu/inc/pmfw_if/smu_v13_0_1_ppsmc.h |
| smu_v13_0_1_pmfw.h | drivers/gpu/drm/amd/pm/swsmu/inc/pmfw_if/smu_v13_0_1_pmfw.h |

The mailbox protocol in `src/common/rb_smu.c` follows `smu_msg_v1_send_msg` in
drivers/gpu/drm/amd/pm/swsmu/smu_cmn.c and `smu_v13_0_init_msg_ctl` in
smu13/smu_v13_0.c; `SMU13_YELLOW_CARP_DRIVER_IF_VERSION` (4) is from
pm/swsmu/inc/pmfw_if/smu13_driver_if_yellow_carp.h.

Added 2026-10-05, from master at `7704c4c5bb127673b4f0ead839919db573559e38`
(the source `src/common/rb_smu13.c` ports):

| File | Upstream path |
|---|---|
| yellow_carp_ppt.c | drivers/gpu/drm/amd/pm/swsmu/smu13/yellow_carp_ppt.c |
| yellow_carp_ppt.h | drivers/gpu/drm/amd/pm/swsmu/smu13/yellow_carp_ppt.h |
| smu13_driver_if_yellow_carp.h | drivers/gpu/drm/amd/pm/swsmu/inc/pmfw_if/smu13_driver_if_yellow_carp.h |

`smu13_driver_if_yellow_carp.h` is the one exception to "not compiled": the
hosttest includes it to check rb_smu13's table layouts field by field.
The smu_v13_0.c / smu_cmn.c helpers rb_smu13 also follows
(`smu_v13_0_set_driver_table_location`, `smu_v13_0_gfx_off_control`,
`smu_cmn_update_table`, `smu_cmn_get_enabled_mask`, `smu_cmn_check_fw_version`)
are cited by name in rb_smu13.h, not copied (2400 and 1700 lines).

Added 2026-10-05, from master at `67f0943b394d920b6c142aad8c6af94340342ae7`:

| File | Upstream path |
|---|---|
| amdgpu_ucode.h | drivers/gpu/drm/amd/amdgpu/amdgpu_ucode.h |

Added 2026-10-12, from master at `5e213a985d103ecd348ed4e9ae71292d86be9a71`:

| File | Upstream path |
|---|---|
| gc_10_3_0_offset.h | drivers/gpu/drm/amd/include/asic_reg/gc/gc_10_3_0_offset.h |

Not compiled: used to name the GC registers that Apple's
`AMDGFX10PM4Engine::doStart` and `::waitForActiveValue` touch
(docs/SPEC-PM4-DOSTART.md). Offsets in it are dword offsets with a
`_BASE_IDX` of 0 for every register named in that spec, which is how
Apple's encoded register ids `(offset << 32) | 11` resolve.

## 2026-10-12 — consulted for 2ee (not vendored, read via raw.githubusercontent.com master)

- `drivers/gpu/drm/amd/amdgpu/gfxhub_v2_1.c`
  `gfxhub_v2_1_init_gart_aperture_regs` writes
  `lower/upper_32_bits(amdgpu_gmc_pd_addr(gart.bo))` to
  GCVM_CONTEXT0_PAGE_TABLE_BASE_ADDR via `setup_vm_pt_regs`, and
  START/END = `gart_start/gart_end >> 12`.
  `gfxhub_v2_1_init_system_aperture_regs` writes
  AGP_BOT/TOP = `agp_start/agp_end >> 24` and
  SYSTEM_APERTURE_LOW/HIGH = `min/max(fb,agp) >> 18`.
  `gfxhub_v2_1_get_mc_fb_offset` returns `GCMC_VM_FB_OFFSET << 24`.
- `drivers/gpu/drm/amd/amdgpu/amdgpu_gmc.c`
  `amdgpu_gmc_pd_addr` -> `amdgpu_gmc_get_pde_for_bo` (VRAM: `amdgpu_bo_gpu_offset`,
  an MC address) then `|= flags` (AMDGPU_PTE_VALID = bit 0).
  `amdgpu_gmc_vram_mc2pa(mc) = mc - vram_start + vram_base_offset`.
- `drivers/gpu/drm/amd/amdgpu/gmc_v10_0.c`
  `gmc_v10_0_get_vm_pde`: for VRAM (no AMDGPU_PTE_SYSTEM)
  `*addr = amdgpu_gmc_vram_mc2pa(*addr)`.
  `gmc_v10_0_vram_gtt_location`: `vm_manager.vram_base_offset =
  gfxhub.funcs->get_mc_fb_offset(adev)`.

Used to establish that GCVM_CONTEXT0_PAGE_TABLE_BASE_ADDR holds a
*system-physical* address (mc2pa of the page directory's MC address), and that
on this APU `vram_base_offset` must be `0x780 << 24 = 0x7_8000_0000`.

## gc_10_3_0_sh_mask.h

    drivers/gpu/drm/amd/include/asic_reg/gc/gc_10_3_0_sh_mask.h

Fetched 2026-10-18 from
`https://raw.githubusercontent.com/torvalds/linux/master/drivers/gpu/drm/amd/include/asic_reg/gc/gc_10_3_0_sh_mask.h`
sha256 `96352168616063a2715ee676e8290b99e9fd0596ad442434ab08687450ecbda5`, 49396 lines.

**The commit is NOT recorded, and that is a gap, not an omission.** The
GitHub API and `git ls-remote` are both unreachable from this environment;
only raw.githubusercontent resolved. The hash above pins the exact bytes
used, so the decode in 2hl can be rechecked against them, but "master on
2026-10-18" is weaker than this project's own rule asks for. Replace it
with the real commit when the network allows.

Used by 2hl to decode GRBM_STATUS, CP_STAT and CP_RB0_CNTL from the armed
boot's ring probe. The companion offsets header gc_10_3_0_offset.h, already
here, supplies the register addresses.

## gmc_v10_0.c, gfxhub_v2_1.c

    drivers/gpu/drm/amd/amdgpu/gmc_v10_0.c
    drivers/gpu/drm/amd/amdgpu/gfxhub_v2_1.c

Fetched 2026-10-18 from raw.githubusercontent.com/torvalds/linux/master/.
sha256 `69b5db518f67697d15cdac885a93718d6e1b93386522ca4e323145c2fbb2a344` and `94b1a294f030020ba2e71849a0762430e6d66839e3ddeee1129bb9862369fe86`. The commit is not recorded for the same reason
as gc_10_3_0_sh_mask.h above — the API and `git ls-remote` are unreachable
from here. Replace with the real commit when the network allows.

Used by 2hp for three things:

- `gfxhub_client_ids[]` in gfxhub_v2_1.c names the `CID` field of
  GCVM_L2_PROTECTION_FAULT_STATUS. **CID 4 = "CPF"**, the command
  processor's fetcher.
- `gmc_v10_0_process_interrupt` reads the status register and then does
  `WREG32_P(hub->vm_l2_pro_fault_cntl, 1, ~1)` — the driver **clears and
  re-arms** the fault status after each fault. That is why two reads a
  minute apart return different values: each catches a different, more
  recent fault.
- `gfxhub_v2_1_get_invalidate_req` sets
  `CLEAR_PROTECTION_FAULT_STATUS_ADDR` in the invalidate request, which
  is why GCVM_L2_PROTECTION_FAULT_ADDR_LO32 can read back 0 after a TLB
  invalidation.

Note what Linux does NOT do: it takes the faulting address from the
interrupt vector (`entry->src_data[0] << 12`), **not** from
GCVM_L2_PROTECTION_FAULT_ADDR_LO32. So this source does not establish how
to convert that register to an address, and 2hp does not claim one.

## amdgpu_gmc.c, amdgpu_gmc.h, amdgpu_vm.h

    drivers/gpu/drm/amd/amdgpu/amdgpu_gmc.{c,h}
    drivers/gpu/drm/amd/amdgpu/amdgpu_vm.h

Fetched 2026-10-18, same caveat as above: the commit could not be resolved
from this environment.

Used by 2hr for one fact: `amdgpu_gmc_pd_addr()` for Vega10+ does
`flags = AMDGPU_PTE_VALID; amdgpu_gmc_get_pde_for_bo(...); pd_addr |= flags`,
and `AMDGPU_PTE_VALID` is `(1ULL << 0)` — so every page-directory base
Linux writes to GCVM_CONTEXT*_PAGE_TABLE_BASE_ADDR has bit 0 set, at every
table depth. Apple's `AMDGFX10VMM::getPDEValue` agrees (it ends `incq %rax`).

## nvd.h, gfx_v10_0.c

    drivers/gpu/drm/amd/amdgpu/nvd.h
    drivers/gpu/drm/amd/amdgpu/gfx_v10_0.c

Fetched 2026-10-18; same commit caveat as the entries above.
sha256 `0a4de0b61f8be33d83e7dc1af4b96370898af36d211f296daf018e72820ea0eb` and `3b2013dcb603845c3785fe02fc9aeee848b391f10d462e9384a1e4f370a4fb1b`.

Used by 2hw for the PM4 format:

- `PACKET3(op, n) = (3 << 30) | ((op & 0xFF) << 8) | ((n & 0x3FFF) << 16)`,
  so a type-3 header is `(dw >> 30) == 3` and the opcode is `(dw >> 8) & 0xFF`.
- `PACKET3_RELEASE_MEM = 0x49` (also NOP 0x10, WAIT_REG_MEM 0x3C,
  INDIRECT_BUFFER 0x3F).
- `gfx_v10_0_ring_emit_fence` gives the RELEASE_MEM dword layout:
  dw0 header, dw1 event/GCR, dw2 DATA_SEL/INT_SEL/DST_SEL,
  **dw3 lower_32_bits(addr), dw4 upper_32_bits(addr)**, dw5/dw6 seq,
  dw7 ctx_id. dw3/dw4 is the end-of-pipe timestamp writeback address 2hw
  exists to read.

Checked 2026-10-23 (2oq), not copied, from master at
`734bef548a96e8774d6eef5dfa2026c6edc4e80b`:

| File | Upstream path |
|---|---|
| dcn_3_1_2_offset.h | drivers/gpu/drm/amd/include/asic_reg/dcn/dcn_3_1_2_offset.h |
| dcn_3_1_2_sh_mask.h | drivers/gpu/drm/amd/include/asic_reg/dcn/dcn_3_1_2_sh_mask.h |

Still not copied, per the note above: 1.5 MB and 6.5 MB. The offset header has
not changed upstream since commit `5dfcaecc7e1f7cd54134b093993aa3449e79afae`
(2023-05-18), so the dword offsets in `src/common/rb_regs.h` are stable.

The three DCN offsets this project already had were **verified** against it
rather than assumed - `regOTG0_OTG_H_TOTAL` 0x1b2a, `regOTG0_OTG_V_TOTAL`
0x1b2f and `regOTG0_OTG_CONTROL` 0x1b41, all `BASE_IDX 2`, matching
`rb_regs.h` exactly. 2oq then added, from the same header:

| rb_regs.h name | Linux name | dword |
|---|---|---|
| OTG0_STATUS | regOTG0_OTG_STATUS | 0x1b49 |
| OTG0_STATUS_POSITION | regOTG0_OTG_STATUS_POSITION | 0x1b4a |
| OTG0_FRAME_COUNT | regOTG0_OTG_STATUS_FRAME_COUNT | 0x1b4c |
| OTG0_H_BLANK_START_END | regOTG0_OTG_H_BLANK_START_END | 0x1b2b |
| OTG0_H_SYNC_A | regOTG0_OTG_H_SYNC_A | 0x1b2c |
| OTG0_H_SYNC_A_CNTL | regOTG0_OTG_H_SYNC_A_CNTL | 0x1b2d |
| OTG0_V_BLANK_START_END | regOTG0_OTG_V_BLANK_START_END | 0x1b36 |
| OTG0_V_SYNC_A | regOTG0_OTG_V_SYNC_A | 0x1b37 |
| OTG0_V_SYNC_A_CNTL | regOTG0_OTG_V_SYNC_A_CNTL | 0x1b38 |
| HUBP0_SURFACE_PITCH | regHUBPREQ0_DCSURF_SURFACE_PITCH | 0x0607 |
| HUBP0_SURFACE_CONTROL | regHUBPREQ0_DCSURF_SURFACE_CONTROL | 0x061a |

For the vblank step, when it comes: `regOTG0_OTG_VERTICAL_INTERRUPT0_CONTROL`
is 0x1b63 and `..._POSITION` 0x1b62, both `BASE_IDX 2`. `OTG_INTERRUPT_DEST`
does not exist under that name in 3.1.2 and still has to be found.

## DCN display headers (2pg, 2026-10-23)

Fetched from kernel.org (GitHub raw was rate-limiting), tag **v6.11**,
`drivers/gpu/drm/amd/include/asic_reg/dcn/`:

- `dcn_3_1_2_offset.h` — **this chip** (DMU 3.1.3 groups to `DCN_VERSION_3_1`,
  and `dcn31_resource.c` uses this header). Independently confirms
  `src/common/rb_regs.h`: `regOTG0_OTG_H_TOTAL 0x1b2a` BASE_IDX 2,
  `regOTG0_OTG_CONTROL 0x1b41`, `regHUBP0_DCSURF_SURFACE_CONFIG 0x5e5`.
- `dcn_3_0_2_offset.h` — the **newest DCN Apple actually compiles**
  (`dc/dcn302` appears in `AMDRadeonX6000Framebuffer`'s source paths), kept as
  the comparison baseline for 2pg.

Note the prefix change: the 3.0.x headers use `mm<REG>`, the 3.1.x headers use
`reg<REG>`. A parser that handles only one silently reports near-total
disagreement - that cost one wrong table in 2pg before it was caught (rule 26).

Added 2026-10-29 (2xk), from master at
`4dd16e31135ea8437a720222df34e8745f385d6d`. **Not copied** - the same decision
this file already records for the large offset headers - so the fields taken
are listed here with their source and are asserted by the hosttest:

| Source | Taken |
|---|---|
| drivers/gpu/drm/amd/include/asic_reg/dcn/dcn_3_1_2_sh_mask.h | `HUBPREQ0_DCSURF_FLIP_CONTROL` field masks: `SURFACE_UPDATE_LOCK` 0x1, `SURFACE_FLIP_TYPE` 0x2, `SURFACE_FLIP_PENDING` 0x100, `SURFACE_FLIP_PENDING_DELAY` 0x3ff00000; and `..._FLIP_CONTROL2`: `SURFACE_FLIP_PENDING_MIN_TIME` 0xff, `SURFACE_TRIPLE_BUFFER_ENABLE` 0x400 |
| drivers/gpu/drm/amd/display/dc/hubp/dcn10/dcn10_hubp.c | line 361, `REG_UPDATE(DCSURF_FLIP_CONTROL, SURFACE_FLIP_TYPE, flip_immediate)` - which is what establishes that 1 is immediate and 0 is vsync'd, rather than it being assumed |

They live in `src/common/rb_hubp.h` with the live boot-216 value `0x04100000`
as the control: it decodes with no leftover bits.

Added 2026-11-01, from master at `cbbddb6b08cc9900a87cccaa9b22bbe06aa4f50a`:

| File | Upstream path |
|---|---|
| dcn31_panel_cntl.c | drivers/gpu/drm/amd/display/dc/dcn31/dcn31_panel_cntl.c |
| link_edp_panel_control.c | drivers/gpu/drm/amd/display/dc/link/protocols/link_edp_panel_control.c |
| dcn21_hwseq.c | drivers/gpu/drm/amd/display/dc/hwss/dcn21/dcn21_hwseq.c |

These three are why 2zj's backlight route is wrong for this chip (2zk). The
local `dce_panel_cntl.c`, which 2xs transcribed, writes `BL_PWM_CNTL`
directly; `dcn31_panel_cntl.c` has **no `.set_backlight_level` at all** and
every member of `dcn31_link_panel_cntl_funcs` is a DMUB round trip, and
`dcn21_set_backlight_level` takes the register-writing `dce110` path only
`if (dc->dc->res_pool->dmcu)` - the old DMCU, which Rembrandt does not have -
and otherwise ends at `dmub_abm_set_backlight()`. `link_edp_panel_control.c`
is where the AUX variants live (`BACKLIGHT_CONTROL_VESA_AUX`,
`BACKLIGHT_CONTROL_AMD_AUX`), and they are reached only from
`edp_set_backlight_level_nits`, not from the ordinary PWM path - so eDP AUX
was the wrong first candidate.

### 2zn — the DMUB command ABI and the ring

`dmub_cmd.h`, `dmub_srv.c`, `dmub_dcn31.c`, `dce/dmub_abm_lcd.c`, all at
`cbbddb6b08cc9900a87cccaa9b22bbe06aa4f50a`, were read for 2zn but are **not**
vendored here: `dmub_cmd.h` alone is 8407 lines and only four structures and
seven constants from it are used. Every one of those is quoted with its line
number at its point of use in `src/common/rb_dmub.h` — the command header's
bit positions (dmub_cmd.h:2125-2134), `dmub_cmd_abm_set_backlight_data`
(:5785-5845), `dmub_cmd_abm_set_pipe_data` (:5716-5747), the ring geometry
(:1867-1877), the push and full/empty helpers (:8100-8200), the window layout
and the push-then-flush-then-WPTR sequence (dmub_srv.c:700-720, 880-892,
1037-1041), the CW4 translation (dmub_dcn31.c:56-80, 247-265) and what
`dmub_abm_set_backlight_level` actually fills in (dmub_abm_lcd.c:280-300).
Re-fetch from that commit before changing any of them; the line numbers are
the provenance.

2zq additionally read, and did not vendor, at the same commit
(cbbddb6b08cc9900a87cccaa9b22bbe06aa4f50a):

- `dmub/inc/dmub_cmd.h` - `DMUB_CMD__QUERY_FEATURE_CAPS = 6`;
  `struct dmub_feature_caps` (psr, fw_assisted_mclk_switch_ver, reserved[4],
  subvp_psr_support, gecc_enable, replay_supported, replay_reserved[3],
  abm_aux_backlight_support, lsdma_support_in_dmu - all uint8_t under
  `#pragma pack(push, 1)`, 14 bytes); `struct dmub_cmd_query_feature_caps_data`
  (one `feature_caps` member); `struct dmub_rb_cmd_query_feature_caps`
  (header + that); `dmub_cmd_header`'s `ret_status : 1` at bit 16, commented
  "1 if returned data, 0 otherwise"; and the ring inlines
  `dmub_rb_get_return_data` (`base_address + rptr - DMUB_RB_CMD_SIZE`, wrapping
  to `base_address + capacity - DMUB_RB_CMD_SIZE` when rptr is 0),
  `dmub_rb_push_front` (`base_address + wrpt`), `dmub_rb_front`
  (`base_address + rptr`), `dmub_rb_empty` (`wrpt == rptr`).
- `dc/dc_dmub_srv.c` - `dc_dmub_srv_query_caps_cmd`: memsets the command, sets
  type, sub_type 0, **`ret_status = 1`**, `payload_bytes =
  sizeof(struct dmub_cmd_query_feature_caps_data)`, sends with
  `DM_DMUB_WAIT_TYPE_WAIT_WITH_REPLY`, and copies the reply only if the send
  succeeded **and `ret_status == 0`**.
- `dmub/src/dmub_srv.c` - `dmub_srv_cmd_get_response` (register inbox only for
  `DMUB_CMD_INTERFACE_REG`, otherwise `dmub_rb_get_return_data` on
  `inbox1.rb`); `dmub_srv_wait_for_idle` (polls until `dmub_rb_empty` on
  inbox1); `default_inbox_type = DMUB_CMD_INTERFACE_FB` with the REG inbox
  reserved for DCN42/42B, DCN401, DCN60 - so DCN 3.1 replies through the ring.

2zr additionally read, and did not vendor, at the same commit:

- `dmub/inc/dmub_cmd.h` - `union dmub_gpint_data_register` (`param : 16`,
  `command_code : 12`, `status : 4`, plus `uint32_t all`);
  `enum dmub_gpint_command` with `DMUB_GPINT__INVALID_COMMAND = 0`,
  `DMUB_GPINT__GET_FW_VERSION = 1` ("Queries the firmware version"),
  `DMUB_GPINT__STOP_FW = 2` ("Halts the firmware"); and
  `#define DMUB_GPINT__STOP_FW_RESPONSE 0xDEADDEAD`.
- `dmub/src/dmub_dcn31.c` - `dmub_dcn31_set_gpint`
  (`REG_WRITE(DMCUB_GPINT_DATAIN1, reg.all)`), `dmub_dcn31_is_gpint_acked`
  (clears `status` in the local copy, reads `DMCUB_GPINT_DATAIN1`, compares
  the whole word), `dmub_dcn31_get_gpint_response`
  (`REG_READ(DMCUB_SCRATCH7)`), and `dmub_dcn31_get_gpint_dataout` (reads
  `DMCUB_GPINT_DATAOUT`, then clears it and pulses `DMCUB_GPINT_IH_INT_ACK`
  with `DMCUB_GPINT_IH_INT_EN` off - **deliberately not reproduced**, see 2zr).

Already vendored and used here: `dcn_3_1_2_offset.h`'s `regDMCUB_SCRATCH0`
0x01e3, `regDMCUB_SCRATCH7` 0x01ea, `regDMCUB_GPINT_DATAIN0` 0x01f7,
`regDMCUB_GPINT_DATAIN1` 0x01f8, `regDMCUB_GPINT_DATAOUT` 0x01f9, all
base_idx 2.

## 2B1 - SURFACE_TRIPLE_BUFFER_ENABLE

Not vendored as a file; read from upstream and recorded here because the
decision rests on it.

- `drivers/gpu/drm/amd/display/dc/hubp/dcn20/dcn20_hubp.c`,
  `hubp2_enable_triplebuffer(struct hubp *, bool enable)`:
  `REG_UPDATE(DCSURF_FLIP_CONTROL2, SURFACE_TRIPLE_BUFFER_ENABLE, enable ?
  DC_TRIPLEBUFFER_ENABLE : DC_TRIPLEBUFFER_DISABLE)`, guarded by a
  `REG_GET` so it only writes when the state differs. No lock, no sequencing.
- `drivers/gpu/drm/amd/display/dc/hubp/dcn31/dcn31_hubp.c`,
  `dcn31_hubp_funcs`: `.hubp_enable_tripleBuffer = hubp2_enable_triplebuffer`,
  `.hubp_is_triplebuffer_enabled = hubp2_is_triplebuffer_enabled`. **So DCN 3.1
  uses the DCN 2.0 writer unchanged** - the version grouping rule 46 asks for.
- `drivers/gpu/drm/amd/display/dc/hwss/dcn20/dcn20_hwseq.c`:
  `dcn20_program_triple_buffer()` is a thin forwarder;
  `dcn20_program_front_end_for_ctx()` calls it only when
  `dc->debug.enable_tri_buf` is set, and per-plane use is driven by
  `plane_state->triplebuffer_flips`, which also selects
  `triplebuffer_lock`/`triplebuffer_unlock` in `dcn20_tg_lock()` instead of the
  normal lock pair. **We set the HUBP bit without that lock protocol**, which
  is the part of the method this project does not reproduce.

## 2B8 — the DP AUX engine

- `dce/dce_aux.c`, `dce/dce_aux.h` — the AUX engine. The transaction sequence
  (`AUX_SW_CONTROL.AUX_SW_GO`, `AUX_SW_STATUS.AUX_SW_DONE`,
  `AUX_SW_REPLY_BYTE_COUNT`) and the arbitration (`AUX_ARB_CONTROL`). Fetched
  from kernel.org torvalds/linux plain/ on 2026-11-02. **No writer derived
  from this yet** — `rb_aux.h` is a read-only survey; this file is here for
  the sequence a transaction would have to follow.
- `resource/dcn31/dcn31_resource.c` — **the rule-46 check**:
  `dcn31_aux_engine_create()` → `dce110_aux_engine_construct()` with
  `aux_engine_regs[inst]` from `AUX_COMMON_REG_LIST0(id)`, five instances. So
  DCN 3.1 uses the DCE110 AUX engine **verbatim**, which is what makes the
  `dce_*` file the right one for this chip. Same relationship as
  `hubp2_enable_triplebuffer` in 2B1.
- `dc_ddc_types.h` — `I2CAUX_TRANSACTION_ACTION_DP_READ = 0x90` and
  `enum aux_transaction_reply` (ACK 0, NACK 1, DEFER 2), used by 2BA's
  `rb_dpcd.h`. Vendored so the constants are read from the tree rather than
  recalled. Fetched from kernel.org torvalds/linux plain/ on 2026-11-02.
- `include/drm/display/drm_dp.h` — the DPCD register map and its bit fields.
  Vendored for 2BB so every byte of boot 294's reply is decoded against the
  tree rather than from memory: `DP_MAX_LANE_COUNT_MASK` 0x1f,
  `DP_TPS3_SUPPORTED` bit 6, `DP_ENHANCED_FRAME_CAP` bit 7,
  `DP_LOCAL_EDID_PRESENT` bit 1 of 0x008, `DP_EDP_CONFIGURATION_CAP` 0x00d,
  `DP_EXTENDED_RECEIVER_CAP_FIELD_PRESENT` bit 7 of 0x00e, `DP_LINK_BW_5_4`
  0x14. Fetched from kernel.org torvalds/linux plain/ on 2026-11-02.

Added 2026-11-02, from master at `6628e7d4c6b25c8d18f1345e3a20d21829aae7e4`
(the source milestone 2BE reads; fetched from git.kernel.org, since
raw.githubusercontent.com was reset by the network in use):

| File | Upstream path |
|---|---|
| dmub_cmd.h | drivers/gpu/drm/amd/display/dmub/inc/dmub_cmd.h |
| command_table2.c | drivers/gpu/drm/amd/display/dc/bios/command_table2.c |
| dcn31_dio_link_encoder.c | drivers/gpu/drm/amd/display/dc/dio/dcn31/dcn31_dio_link_encoder.c |
| link_dp_training.c | drivers/gpu/drm/amd/display/dc/link/protocols/link_dp_training.c |
| link_dp_training_8b_10b.c | drivers/gpu/drm/amd/display/dc/link/protocols/link_dp_training_8b_10b.c |
| link_dp_training_dpia.c | drivers/gpu/drm/amd/display/dc/link/protocols/link_dp_training_dpia.c |
| link_dp_dpia.c | drivers/gpu/drm/amd/display/dc/link/protocols/link_dp_dpia.c |
| link_detection.c | drivers/gpu/drm/amd/display/dc/link/link_detection.c |
| dal_asic_id.h | drivers/gpu/drm/amd/display/include/dal_asic_id.h |

`dmub_cmd.h` is the authoritative DMUB command surface; 2BE reads the
`DMUB_CMD__VBIOS` sub-types out of it and `command_table2.c` for which BIOS
calls DCN 3.1 routes to DMCUB. The two `link_dp_training*` files are vendored
for the NEGATIVE result - they contain no DMUB reference, which is what
establishes that native DP training is driver-side. The DPIA pair is vendored
for contrast only: DPIA is USB4-tunnelled DP and needs a USB4 router, which
macOS does not expose on this machine (2B4).
