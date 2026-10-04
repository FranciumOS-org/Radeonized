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
