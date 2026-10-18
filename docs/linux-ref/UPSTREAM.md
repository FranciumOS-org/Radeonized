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
