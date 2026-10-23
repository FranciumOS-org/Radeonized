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
