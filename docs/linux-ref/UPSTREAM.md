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
