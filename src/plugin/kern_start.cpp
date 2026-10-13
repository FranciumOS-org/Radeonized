#include <Headers/plugin_start.hpp>
#include <Headers/kern_api.hpp>
#include <Headers/kern_util.hpp>
#include <Headers/kern_patcher.hpp>
#include <IOKit/IOLib.h>

#include "kern_start.hpp"
#include "rb_smu13.h"

#ifndef RB_HWID_MP1
#define RB_HWID_MP1 1
#endif

static RGPU rgpu;
uint64_t g_guard_calls = 0;

static const char *bootargOff[]   = { "-rbgpuoff" };
static const char *bootargDebug[] = { "-rbgpudbg" };
static const char *bootargBeta[]  = { "-rbgpubeta" };

static void pluginStart() {
    rgpu.init();
}

PluginConfiguration ADDPR(config) = {
    xStringify(PRODUCT_NAME),
    parseModuleVersion(xStringify(MODULE_VERSION)),
    LiluAPI::AllowNormal | LiluAPI::AllowInstallerRecovery,
    bootargOff,
    arrsize(bootargOff),
    bootargDebug,
    arrsize(bootargDebug),
    bootargBeta,
    arrsize(bootargBeta),
    KernelVersion::Monterey,
    KernelVersion::Sequoia,
    pluginStart
};

static const char *kextPathHWLibs = "/System/Library/Extensions/AMDRadeonX6000HWServices.kext/Contents/PlugIns/AMDRadeonX6800HWLibs.kext/Contents/MacOS/AMDRadeonX6800HWLibs";
static const char *kextPathX6000  = "/System/Library/Extensions/AMDRadeonX6000.kext/Contents/MacOS/AMDRadeonX6000";
static const char *kextPathHWSvc  = "/System/Library/Extensions/AMDRadeonX6000HWServices.kext/Contents/MacOS/AMDRadeonX6000HWServices";
static const char *kextPathAccelFamily =
    "/System/Library/Extensions/IOAcceleratorFamily2.kext/Contents/MacOS/IOAcceleratorFamily2";

enum KextListIndex {
    KextHWLibs = 0,
    KextX6000,
    KextHWSvc,
    KextAccelFamily
};

static KernelPatcher::KextInfo kextList[] = {
    { "com.apple.kext.AMDRadeonX6800HWLibs",     &kextPathHWLibs, 1, {true}, {}, KernelPatcher::KextInfo::Unloaded },
    { "com.apple.kext.AMDRadeonX6000",            &kextPathX6000,  1, {true}, {}, KernelPatcher::KextInfo::Unloaded },
    { "com.apple.kext.AMDRadeonX6000HWServices", &kextPathHWSvc,  1, {true}, {}, KernelPatcher::KextInfo::Unloaded },
    // 2ct: for the headless-accelerator hook. IOAcceleratorFamily2 loads as a
    // dependency of AMDRadeonX6000, so it loads after this plugin and
    // onKextLoad fires for it.
    { "com.apple.iokit.IOAcceleratorFamily2", &kextPathAccelFamily, 1, {true}, {},
      KernelPatcher::KextInfo::Unloaded }
};

static mach_vm_address_t g_kext_addr_hwlibs = 0;
static size_t            g_kext_size_hwlibs = 0;

static mach_vm_address_t g_kext_addr_x6000  = 0;
static size_t            g_kext_size_x6000  = 0;

static mach_vm_address_t g_kext_addr_hwsvc  = 0;
static size_t            g_kext_size_hwsvc  = 0;

static mach_vm_address_t g_kext_addr_accelfam = 0;
static size_t            g_kext_size_accelfam = 0;

static struct rb_smu13 g_smu;
static struct rb_hw    g_hw;
static struct rb_disc *g_disc       = nullptr;
static void           *g_smu_ctx    = nullptr;
// 2de (2026-10-12): named from the 2dd evidence, not from the parameter
// order. Two 4 KB allocations moved these by -0x1000, +0x1000 and +0x30
// respectively, so out1 is a GPU address allocated downward from the 256 MB
// aperture, out2 is the CPU mapping of the buffer, and out3 - 48 bytes apart
// between two 4 KB allocations - can only be a small descriptor, i.e. the
// handle. The old names had all three rotated by one position.
static uint64_t        g_dma_mc     = 0;        // out1: offset into the local FB
static void           *g_dma_cpu    = nullptr;  // out2: CPU address of the buffer
static void           *g_dma_handle = nullptr;  // out3: opaque handle, for free()

// 2di: out1 is an OFFSET into the local frame buffer, not an address the SMU
// can use. The absolute MC address is MMMC_VM_FB_LOCATION_BASE + out1; this
// holds it, and 0 means the FB registers did not read sanely (then nothing
// is sent). See the 2di block in slot_internal_sw_init.
static uint64_t        g_dma_mc_abs = 0;
static uint64_t        g_fb_base    = 0;        // carve-out MC base  (from MMHUB)
static uint64_t        g_fb_end     = 0;        // carve-out MC last byte

// 2de: parameter names from the 2dd deltas, not from a guess at the order.
// out1 is the GPU (MC) address, out2 the CPU mapping, out3 the handle.
typedef int      (*smu_cgs_alloc_memory_t)(void *ctx, uint32_t size, uint32_t align, uint32_t domain, uint64_t *mc_addr, void **cpu_addr, void **handle);
typedef int      (*smu_cgs_free_memory_t)(void *ctx, void *handle);
typedef uint32_t (*smu_get_hw_version_t)(uint64_t ver, uint64_t rev);
typedef int      (*smu_read_config_space_t)(void *ctx);
typedef int      (*smu_init_function_pointer_list_t)(void *ctx, uint64_t ver, uint64_t rev);

typedef int      (*amdtll_init_input_t)(void *this_ptr, void *input);
typedef int      (*amdtll_initialize_t)(void *this_ptr);
typedef void     (*amdtll_set_fw_dir_t)(void *this_ptr, void *fw_dir);
typedef uint32_t (*ttl_initialize_fn_t)(void *init_params, void *ttl_context_38);
typedef uint8_t  (*ttl_is_supported_device_fn_t)(void *dev_info_20);
typedef int      (*ttl_hw_init_fn_t)(void *ctx);
typedef uint8_t  (*ipi_execute_swip_entry_point_fn_t)(void *instance, uint32_t swip_client_id, uint32_t event_id, void *context);
typedef void*    (*ipi_get_discovery_info_fn_t)(void *instance, uint32_t client_id);
typedef int      (*ipi_psp_sw_init_fn_t)(void *dev);
typedef int      (*ipi_psp_hw_init_fn_t)(void *dev);
typedef int      (*ipi_gc_sw_init_fn_t)(void *dev);
typedef int      (*ipi_gc_hw_init_fn_t)(void *dev);

typedef int      (*gc_sw_init_fn_t)(void *param1, void *param2);
typedef int      (*gc_hw_init_fn_t)(void *gc_buf, void *arg2, void *arg3);
typedef uint32_t (*gc_get_hw_version_fn_t)(void *version_slice);
typedef void     (*gc_get_config_fn_t)(void *gc_buf, uint32_t hw_version);
typedef void     (*gc_get_hw_config_tbl_info_fn_t)(void *gc_buf, uint32_t hw_version);
typedef int      (*gc_set_fw_entry_info_fn_t)(void *p1, void *p2, uint32_t rev, void *p4);
typedef int      (*gc_init_fcn_ptr_list_fn_t)(void *gc_buf, uint32_t hw_version);
typedef int      (*gc_get_doorbell_requirements_fn_t)(void *gc_buf, void *out_reqs);
typedef uint8_t  (*ttl_dev_get_doorbell_range_fn_t)(void *dev, uint32_t type, uint32_t idx, void *out_info);
typedef uint8_t  (*ipi_set_fw_entry_fn_t)(void *instance, void *fw_entry);

typedef int      (*psp_sw_init_fn_t)(void *param1, void *param2);
typedef int      (*psp_hw_init_fn_t)(void *psp_buf, void *arg2, void *arg3);
typedef int      (*psp_cos_alloc_memory_fn_t)(void *ctx, uint32_t size, uint32_t align, void **out_ptr);
typedef int      (*psp_asic_type_init_fn_t)(void *psp_buf, void *disc_entry);
typedef int      (*psp_init_pfn_ptr_fn_t)(void *psp_buf);
typedef int      (*psp_read_config_fn_t)(void *psp_buf);
typedef int      (*psp_allocate_private_mem_fn_t)(void *psp_buf);
typedef int      (*psp_ring_init_fn_t)(void *psp_buf, uint32_t ring_type);
typedef int      (*psp_ring_create_fn_t)(void *psp_buf, uint32_t ring_type);
typedef int      (*psp_bootloader_is_sos_running_fn_t)(void *psp_buf);
typedef int      (*psp_cmd_km_init_fn_t)(void *psp_buf);
typedef int      (*psp_tmr_init_fn_t)(void *psp_buf);
typedef int      (*psp_tmr_load_fn_t)(void *psp_buf);
typedef int      (*psp_dtm_init_fn_t)(void *psp_buf);
typedef int      (*psp_np_fw_load_fn_t)(void *psp_buf);

typedef void     (*kprintf_t)(const char *fmt, ...);
typedef void     (*iolog_t)(const char *fmt, ...);

// 2bg: submitFrame/createEngine diagnostics for pHandle=0 investigation
typedef uint32_t (*amdtll_submit_frame_fn_t)(void *this_ptr, uint32_t queue_type, uint32_t index, void *param);
typedef int      (*amdtll_create_engine_fn_t)(void *this_ptr, uint32_t queue_type, uint32_t index, void *input);
// 2bh: buildCommand diagnostics
typedef uint32_t (*amdtll_build_command_fn_t)(void *this_ptr, uint32_t queue_type, uint32_t index, void *info, void *param);
// 2bm: allocateMemoryResources - the vtable[0x120] that returns false
typedef uint8_t (*amdrt_allocate_memory_resources_fn_t)(void *this_ptr);

static smu_cgs_alloc_memory_t           orig_smu_cgs_alloc_memory           = nullptr;
static smu_cgs_free_memory_t            orig_smu_cgs_free_memory            = nullptr;
static smu_get_hw_version_t             orig_smu_get_hw_version             = nullptr;
static smu_read_config_space_t          orig_smu_read_config_space          = nullptr;
static smu_init_function_pointer_list_t orig_smu_init_function_pointer_list = nullptr;

static amdtll_init_input_t              orig_amdtll_init_input              = nullptr;
static amdtll_initialize_t              orig_amdtll_initialize              = nullptr;
static amdtll_set_fw_dir_t              orig_amdtll_set_fw_dir              = nullptr;
static ttl_initialize_fn_t              orig_ttl_initialize                 = nullptr;
static ttl_is_supported_device_fn_t     orig_ttl_is_supported_device        = nullptr;
static ttl_hw_init_fn_t                 orig_ttl_hw_init                    = nullptr;
static ipi_execute_swip_entry_point_fn_t orig_ipi_execute_swip_entry_point = nullptr;
static ipi_get_discovery_info_fn_t      orig_ipi_get_discovery_info         = nullptr;
static ipi_psp_sw_init_fn_t             orig_ipi_psp_sw_init                = nullptr;
static ipi_psp_hw_init_fn_t             orig_ipi_psp_hw_init                = nullptr;
static ipi_gc_sw_init_fn_t              orig_ipi_gc_sw_init                 = nullptr;
static ipi_gc_hw_init_fn_t              orig_ipi_gc_hw_init                 = nullptr;

static gc_sw_init_fn_t                  orig_gc_sw_init                     = nullptr;
static gc_hw_init_fn_t                  orig_gc_hw_init                     = nullptr;
static gc_get_hw_version_fn_t           orig_gc_get_hw_version              = nullptr;
static gc_get_config_fn_t               orig_gc_get_config                  = nullptr;
static gc_get_hw_config_tbl_info_fn_t   orig_gc_get_hw_config_tbl_info      = nullptr;
static gc_set_fw_entry_info_fn_t        orig_gc_set_fw_entry_info           = nullptr;
static gc_init_fcn_ptr_list_fn_t        orig_gc_init_fcn_ptr_list           = nullptr;
static gc_get_doorbell_requirements_fn_t orig_gc_get_doorbell_requirements  = nullptr;
static ttl_dev_get_doorbell_range_fn_t  orig_ttl_dev_get_doorbell_range     = nullptr;
static ipi_set_fw_entry_fn_t            orig_ipi_set_fw_entry               = nullptr;

static psp_sw_init_fn_t                 orig_psp_sw_init                    = nullptr;
static psp_hw_init_fn_t                 orig_psp_hw_init                    = nullptr;
static psp_cos_alloc_memory_fn_t        orig_psp_cos_alloc_memory           = nullptr;
static psp_asic_type_init_fn_t          orig_psp_asic_type_init             = nullptr;
static psp_init_pfn_ptr_fn_t            orig_psp_init_pfn_ptr               = nullptr;
static psp_read_config_fn_t             orig_psp_read_config                = nullptr;
static psp_allocate_private_mem_fn_t    orig_psp_allocate_private_mem       = nullptr;
static psp_ring_init_fn_t               orig_psp_ring_init                  = nullptr;
static psp_ring_create_fn_t             orig_psp_ring_create                = nullptr;
static psp_bootloader_is_sos_running_fn_t orig_psp_bootloader_is_sos_running = nullptr;
static psp_cmd_km_init_fn_t             orig_psp_cmd_km_init                = nullptr;
static psp_tmr_init_fn_t                orig_psp_tmr_init                   = nullptr;
static psp_tmr_load_fn_t                orig_psp_tmr_load                   = nullptr;
static psp_dtm_init_fn_t                orig_psp_dtm_init                   = nullptr;
static psp_np_fw_load_fn_t              orig_psp_np_fw_load                 = nullptr;

static kprintf_t orig_kprintf = nullptr;
static iolog_t   orig_iolog   = nullptr;

// 2bg: orig pointers for submitFrame/createEngine
static amdtll_submit_frame_fn_t  orig_amdtll_submit_frame  = nullptr;
static amdtll_create_engine_fn_t orig_amdtll_create_engine = nullptr;
// 2bh: orig pointer for buildCommand
static amdtll_build_command_fn_t orig_amdtll_build_command = nullptr;
// 2bm: orig pointer for allocateMemoryResources
static amdrt_allocate_memory_resources_fn_t orig_amdrt_allocate_memory_resources = nullptr;

static bool g_safe_ttl_mode  = false;
static bool g_sync_570_mode  = false;
static bool g_spoof_ttl_mode = false;
static bool g_psp_hw_mode    = false;

struct DeviceTypeEntry {
    uint32_t deviceId;
    uint32_t deviceType;
};

struct CapabilityEntry {
    uint8_t data[80];
};

static inline const char *getCallerTag(mach_vm_address_t ret_addr) {
    if (g_kext_addr_hwlibs && ret_addr >= g_kext_addr_hwlibs && ret_addr < g_kext_addr_hwlibs + g_kext_size_hwlibs) {
        return "HWLibs:";
    }
    if (g_kext_addr_x6000 && ret_addr >= g_kext_addr_x6000 && ret_addr < g_kext_addr_x6000 + g_kext_size_x6000) {
        return "X6000:";
    }
    if (g_kext_addr_hwsvc && ret_addr >= g_kext_addr_hwsvc && ret_addr < g_kext_addr_hwsvc + g_kext_size_hwsvc) {
        return "HWSvc:";
    }
    return nullptr;
}

static void wrap_kprintf(const char *fmt, ...) {
    void *ret_addr = __builtin_return_address(0);
    const char *tag = getCallerTag(reinterpret_cast<mach_vm_address_t>(ret_addr));
    va_list va;
    va_start(va, fmt);
    if (tag) {
        rbgpu_log_captured_va(tag, fmt, va, orig_kprintf);
    } else if (orig_kprintf) {
        char buf[256];
        vsnprintf(buf, sizeof(buf), fmt, va);
        orig_kprintf("%s", buf);
    }
    va_end(va);
}

static void wrap_iolog(const char *fmt, ...) {
    void *ret_addr = __builtin_return_address(0);
    const char *tag = getCallerTag(reinterpret_cast<mach_vm_address_t>(ret_addr));
    va_list va;
    va_start(va, fmt);
    if (tag) {
        rbgpu_log_captured_va(tag, fmt, va, orig_iolog);
    } else if (orig_iolog) {
        char buf[256];
        vsnprintf(buf, sizeof(buf), fmt, va);
        orig_iolog("%s", buf);
    }
    va_end(va);
}

static int smu13ErrToApple(int err) {
    switch (err) {
        case RB_SMU13_OK:             return 0;
        case RB_SMU13_E_INVAL:        return 1;
        case RB_SMU13_E_FW:
        case RB_SMU13_E_VERSION:
        case RB_SMU13_E_NODATA:       return 2;
        case RB_SMU13_E_BUSY:
        case RB_SMU13_E_TIMEOUT:
        case RB_SMU13_E_DEAD:         return 3;
        case RB_SMU13_E_NOT_ALLOWED:  return 4;
        default:                      return 2;
    }
}

static uint32_t smu13HwRreg(void *ctx, uint32_t byte_off) {
    if (!ctx) return 0;
    void *cgs = *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(ctx) + 0x08);
    if (!cgs) return 0;
    void *handle = *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(cgs) + 0x08);
    typedef uint32_t (*cgs_rreg_t)(void *h, uint32_t dword_idx, uint32_t flags);
    auto rreg_fn = *reinterpret_cast<cgs_rreg_t *>(reinterpret_cast<uintptr_t>(cgs) + 0x40);
    if (!rreg_fn) return 0;
    return rreg_fn(handle, byte_off / 4, 0);
}

static void smu13HwWreg(void *ctx, uint32_t byte_off, uint32_t v) {
    if (!ctx) return;
    void *cgs = *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(ctx) + 0x08);
    if (!cgs) return;
    void *handle = *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(cgs) + 0x08);
    typedef void (*cgs_wreg_t)(void *h, uint32_t dword_idx, uint32_t val, uint32_t flags);
    auto wreg_fn = *reinterpret_cast<cgs_wreg_t *>(reinterpret_cast<uintptr_t>(cgs) + 0x38);
    if (!wreg_fn) return;
    wreg_fn(handle, byte_off / 4, v, 0);
}

static void smu13HwDelayUs(void *ctx, uint32_t us) {
    if (ctx) {
        void *os_svc = *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(ctx) + 0x10);
        if (os_svc) {
            void *os_handle = *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(os_svc) + 0x08);
            typedef void (*cos_delay_t)(void *h, uint32_t us);
            auto delay_fn = *reinterpret_cast<cos_delay_t *>(reinterpret_cast<uintptr_t>(os_svc) + 0x80);
            if (delay_fn) {
                delay_fn(os_handle, us);
                return;
            }
        }
    }
    IODelay(us);
}

static int slot_internal_sw_init(void *ctx, ...) {
    rbgpu_log("slot_internal_sw_init entered (ctx=%p)", ctx);
    g_smu_ctx = ctx;

    uint32_t mp1_base     = *reinterpret_cast<uint32_t *>(reinterpret_cast<uintptr_t>(ctx) + 0x44);
    uint32_t window_limit = *reinterpret_cast<uint32_t *>(reinterpret_cast<uintptr_t>(ctx) + 0x27C);

    rbgpu_diag_set_num("ctx44_0", mp1_base);
    rbgpu_diag_set_num("window_limit", window_limit);
    rbgpu_log("[Check 1] ctx+0x44[0]=0x%08X (expected 0x16000), window_limit=0x%08X", mp1_base, window_limit);

    if (!orig_smu_cgs_alloc_memory) {
        rbgpu_log("smu_cgs_alloc_memory symbol unresolved");
        rbgpu_diag_set_num("sw_init_ret", 2);
        return 2;
    }

    int alloc_ret = orig_smu_cgs_alloc_memory(ctx, 0x1000, 0x1000, 1, &g_dma_mc, &g_dma_cpu, &g_dma_handle);
    rbgpu_diag_set_num("alloc_ret", static_cast<uint64_t>(alloc_ret));
    rbgpu_diag_set_num("fb_mc_addr", g_dma_mc);
    rbgpu_log("[Check 2] smu_cgs_alloc_memory ret=%d, mc=0x%llx, cpu=%p, handle=%p",
              alloc_ret, g_dma_mc, g_dma_cpu, g_dma_handle);

    if (alloc_ret != 0 || !g_dma_cpu) {
        rbgpu_log("Driver table memory allocation failed");
        rbgpu_diag_set_num("sw_init_ret", 2);
        return 2;
    }

    // 2dd (2026-10-12): WHICH of the three outputs is the GPU MC address?
    //
    // This matters because slot_internal_hw_init verified the wrong field, which
    // holds a kernel VA (0xffffffe0........), so the check has failed since
    // 2ak and rb_smu13_bringup has never run - which is why the SMU is never
    // given its driver table, the GFX block stays gated, and the CP never
    // executes the KIQ packet.
    //
    // Of the three values only the first is not a kernel pointer:
    //     handle = 0x0FFFF000   page aligned, just under the 0x10000000
    //                           aperture the TTL input reports at +0x68
    //     gpu    = 0xffffffe0e19f4000
    //     cpu    = 0xffffff950d0d3580
    // so out1 is very likely the MC address. "Very likely" is not enough to
    // hand to the SMU, which will DMA to whatever it is told, so this proves
    // it instead: allocate a SECOND buffer and compare. An address moves by
    // roughly the allocation size between two allocations; an opaque handle
    // does not behave that way. Allocation only - no SMU message is sent and
    // nothing else touches hardware.
    //
    // This runs only on an armed boot, where a 4 KB allocation is already
    // made and never freed (_smu_cgs_free_memory does not exist in HWLibs).
    {
        uint64_t mc2 = 0;
        void *cpu2 = nullptr, *h2 = nullptr;
        int r2 = orig_smu_cgs_alloc_memory(ctx, 0x1000, 0x1000, 1, &mc2, &cpu2, &h2);
        rbgpu_log("2dd: second alloc ret=%d, mc=0x%llx, cpu=%p, handle=%p", r2, mc2, cpu2, h2);
        rbgpu_diag_set_num("a2_ret", static_cast<uint64_t>(r2));
        rbgpu_diag_set_num("a1_mc", g_dma_mc);
        rbgpu_diag_set_num("a2_mc", mc2);
        if (r2 == 0) {
            int64_t dmc = static_cast<int64_t>(mc2) - static_cast<int64_t>(g_dma_mc);
            int64_t dcpu = static_cast<int64_t>(reinterpret_cast<uint64_t>(cpu2)) -
                           static_cast<int64_t>(reinterpret_cast<uint64_t>(g_dma_cpu));
            int64_t dh = static_cast<int64_t>(reinterpret_cast<uint64_t>(h2)) -
                         static_cast<int64_t>(reinterpret_cast<uint64_t>(g_dma_handle));
            rbgpu_log("2dd: deltas mc=%lld cpu=%lld handle=%lld (expect -0x1000, +0x1000, small)",
                      dmc, dcpu, dh);
            rbgpu_diag_set_num("a_dmc", static_cast<uint64_t>(dmc));
            rbgpu_diag_set_num("a_dcpu", static_cast<uint64_t>(dcpu));
            rbgpu_diag_set_num("a_dhandle", static_cast<uint64_t>(dh));
        }
    }

    // 2di (2026-10-12): out1 is an FB OFFSET, and the SMU needs an absolute
    // MC address. This is what the 2dg boot got wrong.
    //
    // The 2df boot sent SetDriverDramAddrHigh(0) / Low(0x0FFFF000) and the
    // SMU answered both, then never answered TransferTableSmu2Dram
    // (last_msg=0x0f, last_resp=0). The address messages only record a value;
    // the transfer is the first time the SMU uses it.
    //
    // Linux allocates every SMU table for this exact chip in VRAM, not GTT -
    // docs/linux-ref/yellow_carp_ppt.c:159-164 uses AMDGPU_GEM_DOMAIN_VRAM
    // for Watermarks, DpmClocks and SmuMetrics - so the table belongs in the
    // carve-out, and 2de's "an APU driver table lives in the GART aperture"
    // was wrong. The allocator agrees: the plugin asks for smu memory type 1,
    // and _convert_smu_memory_type_to_cgs (0x126660c0) maps 1 -> cgs 0 =
    // CGS_GPU_MEM_TYPE__VISIBLE_FB. What comes back, 0x0FFFF000 counting down
    // from the 0x10000000 the TTL input reports at +0x68, is an offset into
    // that local frame buffer.
    //
    // So read the carve-out's real position out of MMHUB rather than assume
    // it. MMHUB base_idx 1 = 0x1a000 (discovery table, probe 1), and the
    // offsets are Linux's mmhub_2_3_0_offset.h as recorded in rb_regs.h.
    // These are plain reads of a block that is up and scanning the display;
    // they are not the gated-GC reads that caused the boot 6/7 RSMU timeouts.
    {
        const uint32_t kMmhubBase = 0x1a000;          // dwords
        const uint32_t kFbLocBase = kMmhubBase + 0x08ec;
        const uint32_t kFbLocTop  = kMmhubBase + 0x08ed;
        const uint32_t kFbOffset  = kMmhubBase + 0x08d7;

        // These sit at byte 0x6a3b0 and up, well past where the MP1 mailbox
        // lives. Read nothing outside the window Apple mapped: a read past
        // the end of the BAR mapping is not a diagnostic, it is a fault.
        uint32_t highest   = (kFbLocTop * 4) + 4;
        bool     in_window = (window_limit == 0) || (highest <= window_limit);
        bool     regs_sane = false;

        g_fb_base = 0;
        g_fb_end  = 0;
        rbgpu_diag_set_num("fb_regs_oow", in_window ? 0 : 1);

        if (!in_window) {
            rbgpu_log("2di: MMHUB FB_LOCATION at byte 0x%x is outside the MMIO window "
                      "(limit 0x%08X) -> not read, carve-out stays unknown",
                      kFbLocBase * 4, window_limit);
        } else {
            uint32_t base_reg = smu13HwRreg(ctx, kFbLocBase * 4);
            uint32_t top_reg  = smu13HwRreg(ctx, kFbLocTop * 4);
            uint32_t off_reg  = smu13HwRreg(ctx, kFbOffset * 4);

            rbgpu_diag_set_num("fb_base_reg", base_reg);
            rbgpu_diag_set_num("fb_top_reg", top_reg);
            rbgpu_diag_set_num("fb_off_reg", off_reg);
            rbgpu_log("2di: MMHUB FB_LOCATION_BASE=0x%08X TOP=0x%08X FB_OFFSET=0x%08X",
                      base_reg, top_reg, off_reg);

            // The registers hold 16 MB units in bits [23:0]: probe 1 read
            // 0xf400/0xf4ff, i.e. 0xF4_0000_0000..0xF4_FFFF_FFFF.
            regs_sane = base_reg != 0 && base_reg != 0xFFFFFFFF &&
                        top_reg != 0xFFFFFFFF &&
                        (top_reg & 0x00FFFFFF) >= (base_reg & 0x00FFFFFF);
            if (regs_sane) {
                g_fb_base = static_cast<uint64_t>(base_reg & 0x00FFFFFF) << 24;
                g_fb_end  = (static_cast<uint64_t>(top_reg & 0x00FFFFFF) << 24) | 0xFFFFFFULL;
            }
        }

        rbgpu_log("2di: carve-out MC range 0x%llx..0x%llx (regs_sane=%d in_window=%d)",
                  g_fb_base, g_fb_end, regs_sane ? 1 : 0, in_window ? 1 : 0);
        rbgpu_diag_set_num("fb_regs_sane", regs_sane ? 1 : 0);
    }

    // The candidate absolute address, and the test that decides whether the
    // SMU is allowed to see it: the whole 4 KB buffer must lie inside the
    // carve-out that MMHUB just reported.
    uint64_t mc_abs = (g_fb_base != 0) ? (g_fb_base + g_dma_mc) : 0;
    bool mc_ok = (g_fb_base != 0) &&
                 (g_dma_mc != 0) && ((g_dma_mc & 0xFFF) == 0) &&
                 (mc_abs >= g_fb_base) &&
                 (mc_abs + 0x1000 - 1 <= g_fb_end);
    g_dma_mc_abs = mc_ok ? mc_abs : 0;

    rbgpu_log("2di: alloc offset 0x%llx + fb_base 0x%llx = MC 0x%llx; inside carve-out=%d",
              g_dma_mc, g_fb_base, mc_abs, mc_ok ? 1 : 0);
    rbgpu_diag_set_num("mc_abs_hi", static_cast<uint32_t>(mc_abs >> 32));
    rbgpu_diag_set_num("mc_abs_lo", static_cast<uint32_t>(mc_abs));
    rbgpu_diag_set_num("mc_abs_ok", mc_ok ? 1 : 0);
    rbgpu_diag_set_num("mc_sane", mc_ok ? 1 : 0);
    rbgpu_diag_set_num("carveout_ok", mc_ok ? 1 : 0);
    rbgpu_log("[Check 2] Carve-out verification (from MMHUB, not assumed): %s",
              mc_ok ? "PASS" : "FAIL");

    memset(&g_hw, 0, sizeof(g_hw));
    g_hw.ctx      = ctx;
    g_hw.mmio_len = window_limit;
    g_hw.rreg     = smu13HwRreg;
    g_hw.wreg     = smu13HwWreg;
    g_hw.fb_len   = 0;
    g_hw.fb_read  = nullptr;
    g_hw.delay_us = smu13HwDelayUs;

    size_t disc_size = sizeof(struct rb_disc) + 2 * sizeof(struct rb_ip);
    if (!g_disc) {
        g_disc = static_cast<struct rb_disc *>(IOMallocZero(disc_size));
    } else {
        memset(g_disc, 0, disc_size);
    }

    if (!g_disc) {
        rbgpu_log("Failed to allocate memory for rb_disc");
        rbgpu_diag_set_num("sw_init_ret", 2);
        return 2;
    }

    g_disc->nips = 2;

    g_disc->ips[0].hw_id    = RB_HWID_MP1;
    g_disc->ips[0].die      = 0;
    g_disc->ips[0].inst     = 0;
    g_disc->ips[0].nbase    = 1;
    g_disc->ips[0].major    = 13;
    g_disc->ips[0].minor    = 0;
    g_disc->ips[0].revision = 3;
    g_disc->ips[0].base[0]  = mp1_base;

    g_disc->ips[1].hw_id    = 4; // SMUIO
    g_disc->ips[1].die      = 0;
    g_disc->ips[1].inst     = 0;
    g_disc->ips[1].nbase    = 1;
    g_disc->ips[1].base[0]  = 0x16800;

    struct rb_smu13_dma dma;
    memset(&dma, 0, sizeof(dma));
    dma.cpu  = g_dma_cpu;      // out2, the real buffer mapping
    dma.mc   = g_dma_mc_abs;   // 2di: fb_base + out1, or 0 if unverified
    dma.size = 0x1000;

    int init_ret = rb_smu13_init(&g_smu, &g_hw, g_disc, &dma, 0);
    if (init_ret != RB_SMU13_OK) {
        rbgpu_log("rb_smu13_init failed: %s (%d)", rb_smu13_strerror(init_ret), init_ret);
        rbgpu_diag_set_num("init_err", static_cast<uint64_t>(init_ret));
        rbgpu_diag_set_num("last_smu_err", static_cast<uint64_t>(init_ret));
        rbgpu_diag_set_num("sw_init_ret", static_cast<uint64_t>(smu13ErrToApple(init_ret)));
        return smu13ErrToApple(init_ret);
    }

    rbgpu_log("rb_smu13 initialized with ALLOW_QUERY (queries only)");
    rbgpu_log("[Check 5] slot_internal_sw_init succeeded");
    rbgpu_diag_set_num("sw_init_ret", 0);
    return 0;
}

static int slot_internal_hw_init(void *ctx) {
    rbgpu_log("slot_internal_hw_init entered (ctx=%p)", ctx);

    // 2ak: SAFETY. g_smu.dma.mc is a CGS IOVirtualAddress that FAILS carve-out
    // verification (it is a kernel VA, not a GPU-visible address). Letting
    // rb_smu13_bringup run would send it to the SMU via SetDriverDramAddrHigh/
    // Low, making the SMU DMA to a bogus address. Skip real bringup until the
    // DMA address is fixed; report success so _ttl_hw_init can still proceed
    // to the PSP/GC phases.
    //
    // 2as: DMA VERIFICATION ONLY (per the assistant's safety rules). Log full DMA
    // struct details and carve-out analysis. DO NOT send anything to SMU.
    // This boot is for address verification alone.
    uint64_t smu_mc = g_smu.dma.mc;
    void *smu_cpu = g_smu.dma.cpu;
    uint32_t smu_size = g_smu.dma.size;

    // 2de: the field being checked used to be the wrong one - see 2dd - and
    // `dma.cpu` pointed at a 48-byte handle rather than the 4 KB buffer, so
    // rb_smu13_bringup would have memcpy'd the driver table over a small
    // kernel object (rb_smu13.c:311). That gate, written for a different
    // reason, is what kept it from happening.
    //
    // 2de also replaced the carve-out test with a GART test, and that was
    // wrong: Linux puts these tables in VRAM (yellow_carp_ppt.c:159-164), so
    // the carve-out was the right domain all along. 2di restores it, reading
    // the range from MMHUB instead of hardcoding it.
    // 2di: the test is now the carve-out range MMHUB itself reported in
    // sw_init, applied to the absolute address. dma.mc is 0 unless the whole
    // 4 KB buffer was shown to lie inside it, so an unreadable or implausible
    // FB_LOCATION stops the bringup instead of guessing.
    bool smu_mc_ok = (smu_mc != 0) && ((smu_mc & 0xFFF) == 0) &&
                     (g_fb_base != 0) && (smu_mc >= g_fb_base) &&
                     (smu_mc + 0x1000 - 1 <= g_fb_end);
    rbgpu_log("2as DMA VERIFY: mc=0x%llx cpu=%p size=0x%x", smu_mc, smu_cpu, smu_size);
    rbgpu_log("2as DMA VERIFY: carve-out 0x%llx..0x%llx sane=%d (nonzero, page aligned, inside)",
              g_fb_base, g_fb_end, smu_mc_ok ? 1 : 0);
    rbgpu_log("2as DMA VERIFY: mc_is_kernel_va=%d (would be the old wrong field)",
              (smu_mc >= 0xFFFFFF0000000000ULL) ? 1 : 0);
    rbgpu_log("2as DMA VERIFY: size_nonzero=%d size_4k_aligned=%d", (smu_size != 0) ? 1 : 0, (smu_size % 0x1000 == 0) ? 1 : 0);
    rbgpu_diag_set_num("smu_dma_mc_ok", smu_mc_ok ? 1 : 0);
    rbgpu_diag_set_num("dma_mc_hi", (uint32_t)(smu_mc >> 32));
    rbgpu_diag_set_num("dma_mc_lo", (uint32_t)smu_mc);
    rbgpu_diag_set_num("dma_size", smu_size);
    if (!smu_mc_ok) {
        rbgpu_log("2as: SMU DMA addr still not sane -> SKIPPING rb_smu13_bringup");
        return 0;
    }

    // 2de: the address being right is NOT permission to talk to the SMU.
    // rb_smu13_bringup sends SetDriverDramAddrHigh/Low and TransferTable,
    // after which the SMU DMA-writes its table into that address - a message
    // group beyond the queries the project allows by default, and the user's
    // decision per docs/RULES.md. Fixing the check must not silently start
    // sending them, so bringup needs its own opt-in.
    if (!checkKernelArgument("-rbgpusmubringup")) {
        rbgpu_log("2as: MC address verified (0x%llx) but -rbgpusmubringup not set "
                  "-> SKIPPING rb_smu13_bringup (no SMU message sent)", smu_mc);
        rbgpu_diag_set_num("smu_bringup_armed", 0);
        return 0;
    }
    rbgpu_diag_set_num("smu_bringup_armed", 1);

    // 2df (2026-10-12): the TABLES group, allowed by the user's decision.
    //
    // rb_smu13_init() permits RB_SMU13_ALLOW_QUERY only, so until now
    // rb_smu13_bringup could not have sent a table message even if it ran:
    // set_driver_table_location would have been refused by the policy. This
    // is the one place that widens it, and only under -rbgpusmubringup.
    //
    // What the chip then does, in order:
    //   check_fw_version, get_enabled_mask        QUERY, reads only
    //   SetDriverDramAddrHigh(mc >> 32)           record table address, high
    //   SetDriverDramAddrLow(mc & 0xffffffff)     record table address, low
    //   TransferTable(DPMCLOCKS)                  the SMU DMA-WRITES about
    //       300 bytes of clock table into that address; the plugin then reads
    //       it back through dma.cpu.
    //
    // 2di: the address is now absolute - fb_base + the allocator's offset -
    // and is only non-zero when the whole 4 KB lies inside the carve-out
    // MMHUB reported this boot. The high word is therefore 0xF4 rather than
    // the 0 the 2dg boot sent, which is the difference that boot was missing.
    // The page is Apple's own CGS FB allocation, so the SMU writes into our
    // own buffer; if the offset theory is wrong the write still lands inside
    // the carve-out, past the GOP framebuffer, not in host memory.
    //
    // This step verifies itself: the clock values logged below are only
    // plausible 680M numbers if the SMU really wrote where we expect. Zeros
    // or nonsense mean the mapping is wrong, learned without damage.
    rb_smu13_allow(&g_smu, RB_SMU13_ALLOW_TABLES);
    rbgpu_log("2df: RB_SMU13_ALLOW_TABLES granted (user decision 2026-10-12); "
              "running rb_smu13_bringup with mc=0x%llx cpu=%p", smu_mc, smu_cpu);

    int err = rb_smu13_bringup(&g_smu);
    rbgpu_diag_set_num("smu_nmsgs", g_smu.nmsgs);
    rbgpu_diag_set_num("smu_writes", g_smu.writes);
    rbgpu_diag_set_num("smu_refused", g_smu.refused_writes);
    rbgpu_log("2df: SMU accounting after bringup: msgs=%u writes=%u refused=%u "
              "last_msg=0x%02x last_resp=0x%x",
              g_smu.nmsgs, g_smu.writes, g_smu.refused_writes,
              g_smu.last_msg, g_smu.last_resp);
    rbgpu_diag_set_num("bringup_err", static_cast<uint64_t>(err));
    if (err != RB_SMU13_OK) {
        rbgpu_log("rb_smu13_bringup failed: %s (%d)", rb_smu13_strerror(err), err);
        rbgpu_diag_set_num("last_smu_err", static_cast<uint64_t>(err));
        return smu13ErrToApple(err);
    }

    rbgpu_log("[Check 3] rb_smu13_bringup succeeded");
    if (g_smu.have_clocks) {
        rbgpu_diag_set_num("gfx_min", g_smu.clocks.min_gfx_clk);
        rbgpu_diag_set_num("gfx_max", g_smu.clocks.max_gfx_clk);
        rbgpu_diag_set_num("num_soc_levels", g_smu.clocks.num_soc_levels);
        rbgpu_diag_set_num("num_df_pstates", g_smu.clocks.num_df_pstates);

        rbgpu_log("[Check 3] Clocks: min_gfx=%u MHz, max_gfx=%u MHz, num_soc_levels=%u, num_df_pstates=%u",
                  g_smu.clocks.min_gfx_clk, g_smu.clocks.max_gfx_clk,
                  g_smu.clocks.num_soc_levels, g_smu.clocks.num_df_pstates);

        char keyBuf[32];
        for (uint8_t i = 0; i < g_smu.clocks.num_soc_levels && i < RB_SMU13_NUM_DPM_LEVELS; i++) {
            snprintf(keyBuf, sizeof(keyBuf), "soc_clk_%u", i);
            rbgpu_diag_set_num(keyBuf, g_smu.clocks.soc_clocks[i]);
            rbgpu_log("[Check 4] SOC level %u: %u MHz", i, g_smu.clocks.soc_clocks[i]);
        }
        for (uint8_t i = 0; i < g_smu.clocks.num_df_pstates && i < RB_SMU13_NUM_DF_PSTATES; i++) {
            snprintf(keyBuf, sizeof(keyBuf), "df_pstate_%u_fclk", i);
            rbgpu_diag_set_num(keyBuf, g_smu.clocks.df_pstate[i].fclk);
            snprintf(keyBuf, sizeof(keyBuf), "df_pstate_%u_memclk", i);
            rbgpu_diag_set_num(keyBuf, g_smu.clocks.df_pstate[i].memclk);
            snprintf(keyBuf, sizeof(keyBuf), "df_pstate_%u_volt", i);
            rbgpu_diag_set_num(keyBuf, g_smu.clocks.df_pstate[i].voltage);

            rbgpu_log("[Check 4] DF P-state %u: fclk=%u MHz, memclk=%u MHz, volt=%u",
                      i, g_smu.clocks.df_pstate[i].fclk,
                      g_smu.clocks.df_pstate[i].memclk,
                      g_smu.clocks.df_pstate[i].voltage);
        }
    }

    // 2dj (2026-10-12): ask the chip whether GFX is parked in GFXOFF.
    //
    // The 2di boot brought the SMU up for real - the SOC and DF tables came
    // back as plausible 680M numbers - but MinGfxClk and MaxGfxClk both read
    // 0, and the CP still never consumed the KIQ packet (stamp_ret 0). Linux
    // reads max/min sclk from those same two fields
    // (yellow_carp_ppt.c:895,925), so the zeros are not a parsing error: the
    // SMU is reporting that the GFX block has no clock range.
    //
    // Two different causes fit that: GFX is powered but parked in GFXOFF, or
    // GFX DPM was never started at all. They need different fixes, so settle
    // it before asking for any new message group.
    //
    // rb_smu13_get_gfxoff_status is a plain read of SMUIO_GFX_MISC_CNTL - no
    // message, no mailbox write, so no policy group and nothing new sent to
    // the chip. SMUIO is alive (the SMU just answered seven messages through
    // it); this is NOT one of the gated-GC reads that caused the boot 6/7
    // RSMU timeouts, and no GC register is touched here.
    //
    //   0 = in GFXOFF, 1 = leaving GFXOFF, 2 = not in GFXOFF, 3 = entering
    //   (yellow_carp_get_gfxoff_status)
    {
        uint32_t gfxoff = 0xFFFFFFFFu;
        int gret = rb_smu13_get_gfxoff_status(&g_smu, &gfxoff);
        rbgpu_diag_set_num("gfxoff_read_err", static_cast<uint64_t>(gret));
        if (gret == RB_SMU13_OK) {
            rbgpu_diag_set_num("gfxoff_status", gfxoff);
            const char *what = (gfxoff == 0) ? "IN GFXOFF (GFX is parked - DisallowGfxOff would be the lever)"
                             : (gfxoff == 1) ? "LEAVING GFXOFF"
                             : (gfxoff == 2) ? "NOT in GFXOFF (GFX is powered; the zero clock range means "
                                               "GFX DPM was never started, not that it is parked)"
                             : (gfxoff == 3) ? "ENTERING GFXOFF"
                                             : "unknown encoding";
            rbgpu_log("[Check 6] GFXOFF status = %u: %s", gfxoff, what);
        } else {
            // dw_gfx_misc_cntl is 0 when rb_smu13_init found no SMUIO block.
            rbgpu_log("[Check 6] GFXOFF status unavailable: %s (%d) - SMUIO register "
                      "not located, so this boot cannot say why the clock range is 0",
                      rb_smu13_strerror(gret), gret);
        }
    }
    return 0;
}

static int slot_internal_sw_exit(void *ctx) {
    rbgpu_log("slot_internal_sw_exit entered");

    // 2di (2026-10-12): THE PANIC FIX. Apple's _smu_internal_sw_exit
    // (0x1266813e) calls this slot through ctx+0x660 and then, with NO null
    // check, releases the memory handle at ctx+0x340:
    //
    //     12668148  movq 0x660(%rdi),%rax   ; this function
    //     12668157  callq *%rax
    //     12668161  movq 0x340(%rbx),%rsi   ; no test, no branch
    //     1266816b  callq _smu_cos_release_mem_handle
    //     12668170  movq 0x348(%rbx),%rsi   ; this one IS null-checked
    //
    // That reaches cosReleaseMemoryHandle (0x1265d9c0), which checks both
    // arguments for NULL and then dereferences the handle:
    //
    //     1265d9c9  testq %rsi,%rsi ; je -> assert, return 2
    //     1265d9ce  movq  (%rsi),%rax
    //     1265d9d4  callq *0x28(%rax)       <-- the 2dg panic, +0x14
    //
    // ctx+0x340 has exactly one writer, _smu_get_fw_constants (0x12666091),
    // and _smu_sw_init zeroes ctx+0x348 but never ctx+0x340: Apple assumes
    // its own _smu_internal_sw_init path always leaves a releasable handle
    // there. We replaced that path with this slot table, so the handle is
    // whatever the context happened to hold - non-NULL and not a handle.
    //
    // Zeroing it takes Apple's own guard: the release gets NULL,
    // cosReleaseMemoryHandle logs its assert and returns 2. It leaks one
    // handle on an init that is failing anyway, which beats a panic.
    //
    // This had never fired before because the SMU teardown had never run:
    // every earlier boot either skipped bringup or never reached it.
    if (ctx) {
        auto p340 = reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(ctx) + 0x340);
        rbgpu_log("2di: ctx+0x340 = %p -> zeroing (Apple releases it unconditionally)", *p340);
        rbgpu_diag_set_num("sw_exit_h340", reinterpret_cast<uint64_t>(*p340));
        *p340 = nullptr;
        rbgpu_diag_set_num("sw_exit_h340_cleared", 1);
    }
    if (g_dma_handle && orig_smu_cgs_free_memory) {
        orig_smu_cgs_free_memory(ctx, g_dma_handle);
        g_dma_handle = nullptr;
        g_dma_cpu = nullptr;
        g_dma_mc = 0;
    }
    if (g_disc) {
        size_t disc_size = sizeof(struct rb_disc) + 2 * sizeof(struct rb_ip);
        IOFree(g_disc, disc_size);
        g_disc = nullptr;
    }
    g_smu_ctx = nullptr;
    return 0;
}

static int slot_internal_hw_exit(void *ctx) {
    rbgpu_log("slot_internal_hw_exit entered");
    int err = rb_smu13_prepare_unload(&g_smu);
    if (err != RB_SMU13_OK) {
        rbgpu_log("rb_smu13_prepare_unload failed: %s (%d)", rb_smu13_strerror(err), err);
        rbgpu_diag_set_num("unload_err", static_cast<uint64_t>(err));
        rbgpu_diag_set_num("last_smu_err", static_cast<uint64_t>(err));
    }
    return smu13ErrToApple(err);
}

struct smu_event_msg {
    uint32_t size;
    uint32_t event;
    uint64_t *param;
};

static int slot_notify_event(void *ctx, smu_event_msg *msg, ...) {
    if (!msg) {
        rbgpu_log("slot_notify_event received NULL message");
        return 1;
    }

    uint32_t event = msg->event;
    rbgpu_log("slot_notify_event: event %u", event);

    if (event > 11) {
        return 2;
    }

    if (event == 0 || event == 4 || event == 6 || event == 10) {
        // 2ak: same safety guard as slot_internal_hw_init: never re-bringup
        // with an unverified SMU DMA address.
        uint64_t smu_mc_ev = g_smu.dma.mc;
        if (smu_mc_ev < 0xF400000000ULL || smu_mc_ev > 0xF4FFFFFFFFULL) {
            rbgpu_log("slot_notify_event: skipping re-bringup for event %u (SMU DMA addr 0x%llx unverified)", event, smu_mc_ev);
            return 0;
        }
        int err = rb_smu13_bringup(&g_smu);
        if (err != RB_SMU13_OK) {
            rbgpu_log("slot_notify_event: re-bringup for event %u failed: %s (%d)",
                      event, rb_smu13_strerror(err), err);
            rbgpu_diag_set_num("last_smu_err", static_cast<uint64_t>(err));
            return smu13ErrToApple(err);
        }
        return 0;
    }

    return 0;
}

static int slot_gfx_off_control(void *ctx, uint32_t op) {
    rbgpu_log("slot_gfx_off_control: op %u", op);
    static bool s_gfxoff_initialized = false;

    if (op == 0) {
        if (!s_gfxoff_initialized) {
            int ret = rb_smu13_enable_gfxoff(&g_smu);
            if (ret == RB_SMU13_OK) {
                s_gfxoff_initialized = true;
            } else {
                rbgpu_log("rb_smu13_enable_gfxoff error: %s (%d)", rb_smu13_strerror(ret), ret);
                rbgpu_diag_set_num("gfxoff_init_err", static_cast<uint64_t>(ret));
                rbgpu_diag_set_num("last_smu_err", static_cast<uint64_t>(ret));
            }
        }
        int err = rb_smu13_gfx_off_control(&g_smu, 1);
        if (err != RB_SMU13_OK) {
            rbgpu_log("rb_smu13_gfx_off_control(1) error: %s (%d)", rb_smu13_strerror(err), err);
            rbgpu_diag_set_num("gfxoff_ctrl_err", static_cast<uint64_t>(err));
            rbgpu_diag_set_num("last_smu_err", static_cast<uint64_t>(err));
        }
        return smu13ErrToApple(err);
    } else if (op == 1) {
        int err = rb_smu13_gfx_off_control(&g_smu, 0);
        if (err != RB_SMU13_OK) {
            rbgpu_log("rb_smu13_gfx_off_control(0) error: %s (%d)", rb_smu13_strerror(err), err);
            rbgpu_diag_set_num("gfxoff_ctrl_err", static_cast<uint64_t>(err));
            rbgpu_diag_set_num("last_smu_err", static_cast<uint64_t>(err));
        }
        return smu13ErrToApple(err);
    }

    return 4;
}

static int slot_system_features_control(void *ctx, uint64_t *mask) {
    rbgpu_log("slot_system_features_control entered");
    return 0;
}

static int slot_power_feature_caps(void *ctx, uint64_t *caps) {
    rbgpu_log("slot_power_feature_caps entered");
    if (caps) {
        *caps = 0;
    }
    return 0;
}

static int slot_unload_smu(void *ctx, ...) {
    rbgpu_log("slot_unload_smu entered");
    int err = rb_smu13_prepare_unload(&g_smu);
    if (err != RB_SMU13_OK) {
        rbgpu_log("rb_smu13_prepare_unload error: %s (%d)", rb_smu13_strerror(err), err);
        rbgpu_diag_set_num("unload_err", static_cast<uint64_t>(err));
        rbgpu_diag_set_num("last_smu_err", static_cast<uint64_t>(err));
    }
    return smu13ErrToApple(err);
}

static int hooked_smu_init_function_pointer_list(void *ctx, uint64_t ver, uint64_t rev) {
    rbgpu_diag_inc("smu_hook_calls");
    rbgpu_log("hooked_smu_init_function_pointer_list called (ctx=%p, ver=0x%llx, rev=0x%llx)", ctx, ver, rev);

    uint32_t index = 0;
    if (orig_smu_get_hw_version) {
        index = orig_smu_get_hw_version(ver, rev);
    } else {
        uint32_t major = ver & 0xFF;
        uint32_t minor = (ver >> 32) & 0xFF;
        uint32_t r     = rev & 0xFF;
        if (major == 13 && minor == 0 && r == 3) {
            index = 0x10;
        }
    }

    rbgpu_diag_set_num("mp1_hw_index", index);
    rbgpu_log("smu_get_hw_version resolved to index 0x%02X", index);

    if (index != 0x10) {
        if (orig_smu_init_function_pointer_list) {
            return orig_smu_init_function_pointer_list(ctx, ver, rev);
        }
        return 2;
    }

    *reinterpret_cast<uint32_t *>(reinterpret_cast<uintptr_t>(ctx) + 0x278) = 0x10;
    memset(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(ctx) + 0x650), 0, 0x740 - 0x650);

    if (orig_smu_read_config_space) {
        orig_smu_read_config_space(ctx);
    }

    *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(ctx) + 0x650) = reinterpret_cast<void *>(slot_internal_sw_init);
    *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(ctx) + 0x658) = reinterpret_cast<void *>(slot_internal_hw_init);
    *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(ctx) + 0x660) = reinterpret_cast<void *>(slot_internal_sw_exit);
    *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(ctx) + 0x668) = reinterpret_cast<void *>(slot_internal_hw_exit);
    *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(ctx) + 0x680) = reinterpret_cast<void *>(slot_notify_event);
    *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(ctx) + 0x6E0) = reinterpret_cast<void *>(slot_gfx_off_control);
    *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(ctx) + 0x6E8) = reinterpret_cast<void *>(slot_system_features_control);
    *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(ctx) + 0x6F8) = reinterpret_cast<void *>(slot_power_feature_caps);
    *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(ctx) + 0x720) = reinterpret_cast<void *>(slot_unload_smu);

    rbgpu_log("H-SMU-1: registered SMU 13 slot table");
    return 0;
}

static uint8_t hooked_ttlIsSupportedDevice(void *dev_info_20) {
    rbgpu_diag_inc("ttl_is_supp_calls");
    auto dwords = reinterpret_cast<uint32_t *>(dev_info_20);
    uint32_t dev_id = dwords ? dwords[0] : 0;
    uint32_t bus_no = dwords ? dwords[1] : 0;
    uint32_t attr   = dwords ? dwords[0x64 / 4] : 0;

    rbgpu_log("_ttlIsSupportedDevice(ptr=%p): devId=0x%04X, bus=%u, attr=0x%08X", dev_info_20, dev_id, bus_no, attr);

    uint8_t ret = 0;
    if (orig_ttl_is_supported_device) {
        ret = orig_ttl_is_supported_device(dev_info_20);
    }
    rbgpu_log("_ttlIsSupportedDevice original returned %u", ret);

    if (g_spoof_ttl_mode && ret == 0 && dev_id == 0x1681) {
        ret = 1;
        rbgpu_log("_ttlIsSupportedDevice: -rbgpuspoofttl active -> overriding return to 1 (supported)");
    }

    rbgpu_diag_set_num("ttl_is_supp_ret", ret);
    return ret;
}

static int hooked_ttl_hw_init(void *ctx) {
    rbgpu_diag_inc("ttl_hw_init_calls");
    rbgpu_log("_ttl_hw_init entered: ctx=%p (pspHwMode: %s)", ctx, g_psp_hw_mode ? "ENABLED" : "DISABLED");

    if (!g_psp_hw_mode) {
        rbgpu_log("_ttl_hw_init: DRY RUN active (-rbgpupsphw not set) -> aborting before hardware writes, returning 0");
        rbgpu_diag_set_num("ttl_hw_init_ret", 0);
        return 0;
    }

    int ret = 0;
    if (orig_ttl_hw_init) {
        ret = orig_ttl_hw_init(ctx);
    }
    rbgpu_log("_ttl_hw_init returned %d", ret);
    rbgpu_diag_set_num("ttl_hw_init_ret", ret);
    return ret;
}

static uint8_t hooked_ipi_execute_swip_entry_point(void *instance, uint32_t swip_client_id, uint32_t event_id, void *context) {
    rbgpu_diag_inc("ipi_exec_calls");
    rbgpu_log("_IpiExecuteSwipEntryPoint entered: instance=%p, client_id=%u, event_id=%u, context=%p",
              instance, swip_client_id, event_id, context);

    uint8_t ret = 0;
    if (orig_ipi_execute_swip_entry_point) {
        ret = orig_ipi_execute_swip_entry_point(instance, swip_client_id, event_id, context);
    }

    rbgpu_log("_IpiExecuteSwipEntryPoint returned %u (client_id=%u, event_id=%u)", ret, swip_client_id, event_id);
    rbgpu_diag_set_num("ipi_exec_ret", ret);
    return ret;
}

static void *hooked_IpiGetDiscoveryInfo(void *instance, uint32_t client_id) {
    rbgpu_diag_inc("ipi_disc_calls");
    void *ret = nullptr;
    if (orig_ipi_get_discovery_info) {
        ret = orig_ipi_get_discovery_info(instance, client_id);
    }
    rbgpu_log("_IpiGetDiscoveryInfo(instance=%p, client_id=%u) returned %p", instance, client_id, ret);
    if (client_id == 7 && ret) {
        uint32_t num_inst = *reinterpret_cast<uint32_t *>(ret);
        uint64_t span = static_cast<uint64_t>(num_inst) * 0x210;
        uint32_t major = *reinterpret_cast<uint32_t *>(reinterpret_cast<uintptr_t>(ret) + 0xC);
        uint32_t minor = *reinterpret_cast<uint32_t *>(reinterpret_cast<uintptr_t>(ret) + 0x10);
        uint32_t rev   = *reinterpret_cast<uint32_t *>(reinterpret_cast<uintptr_t>(ret) + 0x14);
        rbgpu_log("_IpiGetDiscoveryInfo(client_id=7) -> %p (inst=%u, span=0x%llx vs 0x235, major=%u, minor=%u, rev=%u)",
                  ret, num_inst, span, major, minor, rev);
        rbgpu_diag_set_num("gc_disc_inst", num_inst);
        rbgpu_diag_set_num("gc_disc_span", span);
        rbgpu_diag_set_num("gc_disc_major", major);
        rbgpu_diag_set_num("gc_disc_minor", minor);
        rbgpu_diag_set_num("gc_disc_rev", rev);
    }
    return ret;
}

static int hooked_ipi_psp_sw_init(void *dev) {
    rbgpu_diag_inc("psp_sw_init_calls");
    uint64_t *dev_ptr = reinterpret_cast<uint64_t *>(dev);
    void *fn_ptr = dev_ptr ? reinterpret_cast<void *>(dev_ptr[1]) : nullptr;

    rbgpu_log("_ipi_psp_sw_init entered: dev=%p, fn_ptr@+0x8=%p", dev, fn_ptr);

    int ret = 0;
    if (orig_ipi_psp_sw_init) {
        ret = orig_ipi_psp_sw_init(dev);
    }

    rbgpu_log("_ipi_psp_sw_init returned %d", ret);
    rbgpu_diag_set_num("psp_sw_init_ret", ret);
    return ret;
}

static int hooked_ipi_psp_hw_init(void *dev) {
    rbgpu_diag_inc("psp_hw_init_calls");
    rbgpu_log("_ipi_psp_hw_init entered: dev=%p", dev);

    int ret = 0;
    if (orig_ipi_psp_hw_init) {
        ret = orig_ipi_psp_hw_init(dev);
    }

    rbgpu_log("_ipi_psp_hw_init returned %d", ret);
    rbgpu_diag_set_num("psp_hw_init_ret", ret);
    return ret;
}

static int hooked_psp_sw_init(void *param1, void *param2) {
    rbgpu_diag_inc("psp_sw_init_sub_calls");
    uint32_t p1_0 = param1 ? *reinterpret_cast<uint32_t *>(param1) : 0;
    uint32_t p1_8 = param1 ? *reinterpret_cast<uint32_t *>(reinterpret_cast<uintptr_t>(param1) + 0x8) : 0;
    uint64_t p1_218 = param1 ? *reinterpret_cast<uint64_t *>(reinterpret_cast<uintptr_t>(param1) + 0x218) : 0;
    uint64_t p1_220 = param1 ? *reinterpret_cast<uint64_t *>(reinterpret_cast<uintptr_t>(param1) + 0x220) : 0;
    uint32_t p2_0 = param2 ? *reinterpret_cast<uint32_t *>(param2) : 0;

    rbgpu_log("_psp_sw_init entered: p1=%p (0x%x, +8=0x%x, +218=%p, +220=%p), p2=%p (0x%x)",
              param1, p1_0, p1_8, reinterpret_cast<void *>(p1_218), reinterpret_cast<void *>(p1_220), param2, p2_0);

    int ret = 1;
    if (orig_psp_sw_init) {
        ret = orig_psp_sw_init(param1, param2);
    }

    rbgpu_log("_psp_sw_init returned %d", ret);
    rbgpu_diag_set_num("psp_sw_init_sub_ret", ret);
    return ret;
}

static int hooked_psp_hw_init(void *psp_buf, void *arg2, void *arg3) {
    rbgpu_diag_inc("psp_hw_sub_calls");
    uint32_t a2_18 = arg2 ? *reinterpret_cast<uint32_t *>(reinterpret_cast<uintptr_t>(arg2) + 0x18) : 0;
    uint8_t  a2_1c = arg2 ? *reinterpret_cast<uint8_t *>(reinterpret_cast<uintptr_t>(arg2) + 0x1c) : 0;
    uint8_t  a2_1d = arg2 ? *reinterpret_cast<uint8_t *>(reinterpret_cast<uintptr_t>(arg2) + 0x1d) : 0;
    uint8_t  a2_38 = arg2 ? *reinterpret_cast<uint8_t *>(reinterpret_cast<uintptr_t>(arg2) + 0x38) : 0;
    uint8_t  a2_39 = arg2 ? *reinterpret_cast<uint8_t *>(reinterpret_cast<uintptr_t>(arg2) + 0x39) : 0;

    rbgpu_log("_psp_hw_init entered: psp_buf=%p, arg2=%p (+0x18=0x%x, +0x1c=0x%02x, +0x1d=0x%02x, +0x38=0x%02x, +0x39=0x%02x), arg3=%p",
              psp_buf, arg2, a2_18, a2_1c, a2_1d, a2_38, a2_39, arg3);
    rbgpu_diag_set_num("psp_hw_a2_18", a2_18);

    int ret = 1;
    if (orig_psp_hw_init) {
        ret = orig_psp_hw_init(psp_buf, arg2, arg3);
    }

    rbgpu_log("_psp_hw_init returned %d", ret);
    rbgpu_diag_set_num("psp_hw_sub_ret", ret);
    return ret;
}

static int hooked_psp_cos_alloc_memory(void *ctx, uint32_t size, uint32_t align, void **out_ptr) {
    rbgpu_diag_inc("psp_alloc_calls");
    int ret = 1;
    if (orig_psp_cos_alloc_memory) {
        ret = orig_psp_cos_alloc_memory(ctx, size, align, out_ptr);
    }
    void *allocated = (out_ptr && ret == 0) ? *out_ptr : nullptr;
    rbgpu_log("_psp_cos_alloc_memory(ctx=%p, size=0x%x, align=%u) -> ret=%d, ptr=%p",
              ctx, size, align, ret, allocated);
    rbgpu_diag_set_num("psp_alloc_ret", ret);
    return ret;
}

static int hooked_psp_asic_type_init(void *psp_buf, void *disc_entry) {
    rbgpu_diag_inc("psp_asic_init_calls");
    uint32_t d_8  = disc_entry ? *reinterpret_cast<uint32_t *>(reinterpret_cast<uintptr_t>(disc_entry) + 0x8) : 0;
    uint32_t d_C  = disc_entry ? *reinterpret_cast<uint32_t *>(reinterpret_cast<uintptr_t>(disc_entry) + 0xC) : 0;
    uint32_t d_10 = disc_entry ? *reinterpret_cast<uint32_t *>(reinterpret_cast<uintptr_t>(disc_entry) + 0x10) : 0;

    rbgpu_log("_psp_asic_type_init entered: buf=%p, disc=%p (+8=0x%x, +C=0x%x, +10=0x%x)",
              psp_buf, disc_entry, d_8, d_C, d_10);

    int ret = 1;
    if (orig_psp_asic_type_init) {
        ret = orig_psp_asic_type_init(psp_buf, disc_entry);
    }

    uint32_t asic_type = psp_buf ? *reinterpret_cast<uint32_t *>(reinterpret_cast<uintptr_t>(psp_buf) + 0x7d44) : 0;
    rbgpu_log("_psp_asic_type_init returned %d (asic_type@+0x7d44 = %u)", ret, asic_type);
    rbgpu_diag_set_num("psp_asic_ret", ret);
    rbgpu_diag_set_num("psp_asic_type", asic_type);
    return ret;
}

static int hooked_psp_init_pfn_ptr(void *psp_buf) {
    rbgpu_diag_inc("psp_pfn_calls");
    uint32_t *asic_type_ptr = psp_buf ? reinterpret_cast<uint32_t *>(reinterpret_cast<uintptr_t>(psp_buf) + 0x7d44) : nullptr;
    uint32_t orig_asic = asic_type_ptr ? *asic_type_ptr : 0;
    rbgpu_log("_psp_init_pfn_ptr entered: buf=%p (asic_type=%u)", psp_buf, orig_asic);

    if (g_spoof_ttl_mode && asic_type_ptr && orig_asic == 19) {
        *asic_type_ptr = 8;
        rbgpu_log("_psp_init_pfn_ptr: -rbgpuspoofttl active -> spoofing asic_type 19 -> 8 for table binding");
    }

    int ret = 1;
    if (orig_psp_init_pfn_ptr) {
        ret = orig_psp_init_pfn_ptr(psp_buf);
    }

    if (g_spoof_ttl_mode && asic_type_ptr && orig_asic == 19) {
        *asic_type_ptr = 19;
        rbgpu_log("_psp_init_pfn_ptr: restored asic_type to 19; ret=%d", ret);
    }

    uint64_t *pfn_ring_init   = psp_buf ? reinterpret_cast<uint64_t *>(reinterpret_cast<uintptr_t>(psp_buf) + 0x7d50) : nullptr;
    uint64_t *pfn_ring_create = psp_buf ? reinterpret_cast<uint64_t *>(reinterpret_cast<uintptr_t>(psp_buf) + 0x7d60) : nullptr;
    uint64_t *pfn_is_sos      = psp_buf ? reinterpret_cast<uint64_t *>(reinterpret_cast<uintptr_t>(psp_buf) + 0x7de8) : nullptr;
    rbgpu_log("_psp_init_pfn_ptr: pfn_ring_init=%p, pfn_ring_create=%p, pfn_is_sos=%p",
              pfn_ring_init ? reinterpret_cast<void *>(*pfn_ring_init) : nullptr,
              pfn_ring_create ? reinterpret_cast<void *>(*pfn_ring_create) : nullptr,
              pfn_is_sos ? reinterpret_cast<void *>(*pfn_is_sos) : nullptr);

    rbgpu_log("_psp_init_pfn_ptr returned %d", ret);
    rbgpu_diag_set_num("psp_pfn_ret", ret);
    return ret;
}

static int hooked_psp_read_config(void *psp_buf) {
    rbgpu_diag_inc("psp_read_cfg_calls");
    int ret = 0;
    if (orig_psp_read_config) {
        ret = orig_psp_read_config(psp_buf);
    }
    uint64_t flags = psp_buf ? *reinterpret_cast<uint64_t *>(reinterpret_cast<uintptr_t>(psp_buf) + 0x28) : 0;
    rbgpu_log("_psp_read_config exited: buf=%p, ret=%d, flags@+0x28=0x%llx", psp_buf, ret, flags);
    return ret;
}

static int hooked_psp_allocate_private_mem(void *psp_buf) {
    rbgpu_diag_inc("psp_alloc_priv_calls");
    rbgpu_log("_psp_allocate_private_mem entered: psp_buf=%p", psp_buf);
    int ret = 1;
    if (orig_psp_allocate_private_mem) {
        ret = orig_psp_allocate_private_mem(psp_buf);
    }
    rbgpu_log("_psp_allocate_private_mem returned %d", ret);
    rbgpu_diag_set_num("psp_alloc_priv_ret", ret);
    return ret;
}

static int hooked_psp_ring_init(void *psp_buf, uint32_t ring_type) {
    rbgpu_diag_inc("psp_ring_init_calls");
    rbgpu_log("_psp_ring_init entered: psp_buf=%p, ring_type=%u", psp_buf, ring_type);
    int ret = 1;
    if (orig_psp_ring_init) {
        ret = orig_psp_ring_init(psp_buf, ring_type);
    }
    rbgpu_log("_psp_ring_init returned %d (ring_type=%u)", ret, ring_type);
    rbgpu_diag_set_num("psp_ring_init_ret", ret);
    return ret;
}

static int hooked_psp_ring_create(void *psp_buf, uint32_t ring_type) {
    rbgpu_diag_inc("psp_ring_create_calls");
    rbgpu_log("_psp_ring_create entered: psp_buf=%p, ring_type=%u", psp_buf, ring_type);
    int ret = 1;
    if (orig_psp_ring_create) {
        ret = orig_psp_ring_create(psp_buf, ring_type);
    }
    rbgpu_log("_psp_ring_create returned %d (ring_type=%u)", ret, ring_type);
    rbgpu_diag_set_num("psp_ring_create_ret", ret);
    return ret;
}

static int hooked_psp_bootloader_is_sos_running(void *psp_buf) {
    rbgpu_diag_inc("psp_is_sos_calls");
    rbgpu_log("_psp_bootloader_is_sos_running entered: psp_buf=%p", psp_buf);
    int ret = 1;
    if (orig_psp_bootloader_is_sos_running) {
        ret = orig_psp_bootloader_is_sos_running(psp_buf);
    }
    rbgpu_log("_psp_bootloader_is_sos_running returned %d", ret);
    rbgpu_diag_set_num("psp_is_sos_ret", ret);
    return ret;
}

static int hooked_psp_cmd_km_init(void *psp_buf) {
    rbgpu_diag_inc("psp_km_init_calls");
    rbgpu_log("_psp_cmd_km_init entered: psp_buf=%p", psp_buf);
    int ret = 1;
    if (orig_psp_cmd_km_init) {
        ret = orig_psp_cmd_km_init(psp_buf);
    }
    rbgpu_log("_psp_cmd_km_init returned %d", ret);
    rbgpu_diag_set_num("psp_km_init_ret", ret);
    return ret;
}

static int hooked_psp_tmr_init(void *psp_buf) {
    rbgpu_diag_inc("psp_tmr_init_calls");
    rbgpu_log("_psp_tmr_init entered: psp_buf=%p", psp_buf);
    int ret = 1;
    if (orig_psp_tmr_init) {
        ret = orig_psp_tmr_init(psp_buf);
    }
    rbgpu_log("_psp_tmr_init returned %d", ret);
    rbgpu_diag_set_num("psp_tmr_init_ret", ret);
    return ret;
}

static int hooked_psp_tmr_load(void *psp_buf) {
    rbgpu_diag_inc("psp_tmr_load_calls");
    rbgpu_log("_psp_tmr_load entered: psp_buf=%p", psp_buf);
    int ret = 1;
    if (orig_psp_tmr_load) {
        ret = orig_psp_tmr_load(psp_buf);
    }
    rbgpu_log("_psp_tmr_load returned %d", ret);
    rbgpu_diag_set_num("psp_tmr_load_ret", ret);
    return ret;
}

static int hooked_psp_dtm_init(void *psp_buf) {
    rbgpu_diag_inc("psp_dtm_init_calls");
    rbgpu_log("_psp_dtm_init entered: psp_buf=%p", psp_buf);
    int ret = 1;
    if (orig_psp_dtm_init) {
        ret = orig_psp_dtm_init(psp_buf);
    }
    rbgpu_log("_psp_dtm_init returned %d", ret);
    rbgpu_diag_set_num("psp_dtm_init_ret", ret);
    return ret;
}

static int hooked_psp_np_fw_load(void *psp_buf) {
    rbgpu_diag_inc("psp_np_fw_load_calls");
    rbgpu_log("_psp_np_fw_load entered: psp_buf=%p", psp_buf);
    int ret = 1;
    if (orig_psp_np_fw_load) {
        ret = orig_psp_np_fw_load(psp_buf);
    }
    rbgpu_log("_psp_np_fw_load returned %d", ret);
    rbgpu_diag_set_num("psp_np_fw_load_ret", ret);
    return ret;
}

static int hooked_ipi_gc_sw_init(void *dev) {
    rbgpu_diag_inc("gc_sw_init_calls");
    uint64_t *dev_ptr = reinterpret_cast<uint64_t *>(dev);
    void *fn_ptr = dev_ptr ? reinterpret_cast<void *>(dev_ptr[1]) : nullptr;

    rbgpu_log("_ipi_gc_sw_init entered: dev=%p, fn_ptr@+0x8=%p", dev, fn_ptr);

    int ret = 0;
    if (orig_ipi_gc_sw_init) {
        ret = orig_ipi_gc_sw_init(dev);
    }

    rbgpu_log("_ipi_gc_sw_init returned %d", ret);
    rbgpu_diag_set_num("gc_sw_init_ret", ret);
    return ret;
}

static int hooked_ipi_gc_hw_init(void *dev) {
    rbgpu_diag_inc("gc_hw_init_calls");
    rbgpu_log("_ipi_gc_hw_init entered: dev=%p", dev);

    int ret = 0;
    if (orig_ipi_gc_hw_init) {
        ret = orig_ipi_gc_hw_init(dev);
    }

    rbgpu_log("_ipi_gc_hw_init returned %d", ret);
    rbgpu_diag_set_num("gc_hw_init_ret", ret);
    return ret;
}

static int hooked_gc_sw_init(void *param1, void *param2) {
    rbgpu_diag_inc("gc_sw_sub_calls");
    uint32_t p1_sz = param1 ? *reinterpret_cast<uint32_t *>(param1) : 0;
    uint32_t p2_sz = param2 ? *reinterpret_cast<uint32_t *>(param2) : 0;
    uint32_t d_8  = param1 ? *reinterpret_cast<uint32_t *>(reinterpret_cast<uintptr_t>(param1) + 0xC) : 0;
    uint32_t d_C  = param1 ? *reinterpret_cast<uint32_t *>(reinterpret_cast<uintptr_t>(param1) + 0x10) : 0;
    uint32_t d_10 = param1 ? *reinterpret_cast<uint32_t *>(reinterpret_cast<uintptr_t>(param1) + 0x14) : 0;

    rbgpu_log("_gc_sw_init entered: p1=%p (sz=0x%x, +C=0x%x, +10=0x%x, +14=0x%x), p2=%p (sz=0x%x)",
              param1, p1_sz, d_8, d_C, d_10, param2, p2_sz);

    int ret = 1;
    if (orig_gc_sw_init) {
        ret = orig_gc_sw_init(param1, param2);
    }

    rbgpu_log("_gc_sw_init returned %d", ret);
    rbgpu_diag_set_num("gc_sw_sub_ret", ret);
    return ret;
}

// 2ap: Diagnostic register dump. Reads GC engine status registers via the
// already-mapped MMIO (g_hw.rreg) to verify whether the hardware is ACTUALLY
// ready, vs our 2am/2an/2ao bypasses which assume it. Raphael-iGPU reference:
// RLC_STAT==0x25 means firmware IS in instruction RAM but the BOOTLOAD_COMPLETE
// latch never set (bypass justified). Any other value means our assumption may
// be wrong. File-static, no hpp change.
// 2dn: the me/pipe/queue the KIQ asked for, captured in the startKIQ hook.
static uint32_t g_rb_kiq_spec0 = 0xFFFFFFFFu;
static uint32_t g_rb_kiq_spec1 = 0xFFFFFFFFu;
static uint32_t g_rb_kiq_spec2 = 0xFFFFFFFFu;
static uint64_t g_rb_kiq_ring_mc = 0;   // startKIQ's first u64 arg: the ring MC address

// 2dt: carried across the 5 s wait, because the log ring wraps during it.
static uint64_t g_rb_2dt_wb_c0 = 0;
static uint64_t g_rb_2dt_wb_e0 = 0;
static uint32_t g_rb_2dt_sub_before = 0;
static uint32_t g_rb_2dt_cmp_before = 0;

// 2do (2026-10-12): read the CP_HQD_* window at the slot the KIQ actually uses.
//
// The 2dn boot settled it: startKIQ's AMD_PM4_QUEUE_SPEC is
// me=2 pipe=1 queue=0, and GRBM_GFX_CNTL reads 0, so every CP_HQD_* value
// recorded so far was the window pointed at me=0 pipe=0 queue=0 - the WRONG
// slot. The all-zero readings in 2dm prove nothing, exactly as the 2dn
// caveat said they would not.
//
// Reaching the right slot needs GRBM_GFX_CNTL written, which is this plugin's
// FIRST write to a GC register. It is an index register: it selects which
// ME / PIPE / QUEUE / VMID instance the windowed registers address and changes
// no GPU behaviour by itself. amdgpu does exactly this around every HQD access
// (nv_grbm_select). The value is saved before and restored after, and nothing
// else runs concurrently here - this is inside the accelerator's single-
// threaded start path.
//
// The field layout is from amdgpu's nv_grbm_select and is NOT independently
// verified here, so the probe does not rely on it: it sweeps a small set of
// slots and recognises the right one by CP_HQD_PQ_BASE matching the ring
// address startKIQ was given (a = 0xf40fb28000 on the 2dn boot, and
// CP_HQD_PQ_BASE holds the ring base >> 8). A match proves both the slot and
// the field layout at once; no match across the sweep means no queue in any of
// them was pointed at our ring.
static uint32_t rbgpu_grbm_sel(uint32_t me, uint32_t pipe, uint32_t queue, uint32_t vmid) {
    return (pipe & 3u) | ((me & 3u) << 2) | ((vmid & 0xFu) << 4) | ((queue & 7u) << 8);
}

static bool rbgpu_probe_hqd_slot(const char *tag, uint32_t me, uint32_t pipe,
                                 uint32_t queue, uint32_t expect_pq_base) {
    const uint32_t kSeg0 = 0x1260;
    const uint32_t dwGRBM_GFX_CNTL = 0x0dc2;
    const uint32_t dwACTIVE   = 0x1fab;
    const uint32_t dwVMID     = 0x1fac;
    const uint32_t dwPQ_BASE  = 0x1fb1;
    const uint32_t dwPQ_BASEH = 0x1fb2;
    const uint32_t dwPQ_RPTR  = 0x1fb3;
    const uint32_t dwPQ_DBELL = 0x1fb8;
    const uint32_t dwPQ_CTRL  = 0x1fba;

    const uint32_t dwPQ_WPTR = 0x1fdf;   // CP_HQD_PQ_WPTR_LO

    uint32_t sel = rbgpu_grbm_sel(me, pipe, queue, 0);
    g_hw.wreg(g_hw.ctx, (kSeg0 + dwGRBM_GFX_CNTL) * 4, sel);
    // GRBM_GFX_CNTL reads back 0 on this chip even when the write took - the
    // 2do boot proved the selector works anyway, because the windowed values
    // differ per slot. Do not read the readback as "the write failed".
    uint32_t sel_rb = g_hw.rreg(g_hw.ctx, (kSeg0 + dwGRBM_GFX_CNTL) * 4);

    uint32_t act   = g_hw.rreg(g_hw.ctx, (kSeg0 + dwACTIVE)   * 4);
    uint32_t vmid  = g_hw.rreg(g_hw.ctx, (kSeg0 + dwVMID)     * 4);
    uint32_t base  = g_hw.rreg(g_hw.ctx, (kSeg0 + dwPQ_BASE)  * 4);
    uint32_t baseh = g_hw.rreg(g_hw.ctx, (kSeg0 + dwPQ_BASEH) * 4);
    uint32_t rptr  = g_hw.rreg(g_hw.ctx, (kSeg0 + dwPQ_RPTR)  * 4);
    uint32_t wptr  = g_hw.rreg(g_hw.ctx, (kSeg0 + dwPQ_WPTR)  * 4);
    uint32_t dbell = g_hw.rreg(g_hw.ctx, (kSeg0 + dwPQ_DBELL) * 4);
    uint32_t ctrl  = g_hw.rreg(g_hw.ctx, (kSeg0 + dwPQ_CTRL)  * 4);

    bool hit = (expect_pq_base != 0) && (base == expect_pq_base);
    bool any = (act | base | baseh | dbell | ctrl) != 0;

    if (any || hit) {
        rbgpu_log("2do [%s] me=%u pipe=%u queue=%u (sel=0x%08x rb=0x%08x): ACTIVE=0x%08x "
                  "PQ_BASE=0x%08x/HI=0x%08x RPTR=0x%08x WPTR=0x%08x DOORBELL=0x%08x "
                  "CTRL=0x%08x VMID=0x%08x%s",
                  tag, me, pipe, queue, sel, sel_rb, act, base, baseh, rptr, wptr, dbell, ctrl,
                  vmid, hit ? "  <<< PQ_BASE equals the ring MC >> 8" : "");
        // 2dp: the discriminator. WPTR is what the driver's doorbell write
        // advances; RPTR is what the command processor has consumed.
        if (act & 1) {
            rbgpu_log("2dp [%s] me=%u pipe=%u queue=%u ACTIVE: doorbell_en(bit30)=%u "
                      "doorbell_offset(27:2)=0x%x  RPTR=0x%08x WPTR=0x%08x -> %s",
                      tag, me, pipe, queue, (dbell >> 30) & 1, (dbell >> 2) & 0x3FFFFFFu,
                      rptr, wptr,
                      (wptr == 0 && rptr == 0)
                          ? "nothing submitted yet (both 0)"
                          : (wptr != rptr
                                 ? "WORK IS QUEUED AND THE CP HAS NOT CONSUMED IT"
                                 : "the CP has consumed everything submitted"));
        }
    }
    return hit;
}

static void rbgpu_dump_gc_regs(const char *tag) {
    if (!g_hw.ctx || !g_hw.rreg) {
        rbgpu_log("2ap DIAG [%s]: no HW ctx/rreg, skipping", tag);
        return;
    }

    // 2dk (2026-10-12): THE ADDRESSING WAS WRONG, and it produced a false alarm.
    //
    // This function used to read `0xa000 + 0x4c00` as a BYTE address. The GC
    // offsets in gc_10_3_0_offset.h are DWORD indices and the segment bases
    // are DWORD too, so the byte address is (seg + offset) * 4. The 2ba block
    // in hooked_gc_hw_init already had this right and read, on the same boot,
    //
    //     2ba CORRECT: RLC_CNTL=0x00000001 RLC_STAT=0x00000025 BOOTLOAD=0x00000001
    //
    // while this function read 0 / 0 / 0 at the same moment and then logged
    // "RLC_STAT DIFFERS from Raphael 0x25 - investigate!". RLC_STAT really is
    // 0x25 - the Raphael reference value, firmware present in instruction RAM.
    // The alarm was our own bug. Likewise CP_STAT read 0xffffffff here, which
    // is an undecoded address, not a register value.
    //
    // Segment bases, in dwords:
    //   BASE_IDX 1 = 0xa000  - proven on hardware: 0xa000+0x4c04 gives RLC_STAT
    //                          0x25 and 0xa000+0x4c00 gives RLC_CNTL 1, both
    //                          sane and both matching the reference.
    //   BASE_IDX 0 = unproven. The plugin's synthetic discovery table carries
    //                          only MP0/MP1, so there is no GC base to read.
    //                          Rather than hardcode one, read CP_STAT at both
    //                          candidates and let the values say which decodes:
    //                          0xffffffff is an undecoded address, anything
    //                          else is a register. AMD's convention for
    //                          gc_10_3_0 is SEG0 = 0x1260; 0 is what this
    //                          function assumed before.
    //
    // Everything here is a READ. No GC register is written.
    const uint32_t kSeg1 = 0xa000;          // dwords, proven
    const uint32_t kSeg0Conv = 0x1260;      // dwords, AMD convention, unproven
    const uint32_t kSeg0Old  = 0x0000;      // dwords, what this code assumed

    // gc_10_3_0_offset.h dword indices.
    const uint32_t dwRLC_CNTL     = 0x4c00; // BASE_IDX 1
    const uint32_t dwRLC_STAT     = 0x4c04; // BASE_IDX 1
    const uint32_t dwRLC_GPM_STAT = 0x4e6e; // BASE_IDX 1
    const uint32_t dwCP_STAT      = 0x0f40; // BASE_IDX 0
    const uint32_t dwCP_MEC_CNTL  = 0x0f55; // BASE_IDX 0
    const uint32_t dwCP_ME_CNTL   = 0x0f56; // BASE_IDX 0

    uint32_t rlc_cntl = g_hw.rreg(g_hw.ctx, (kSeg1 + dwRLC_CNTL)     * 4);
    uint32_t rlc_stat = g_hw.rreg(g_hw.ctx, (kSeg1 + dwRLC_STAT)     * 4);
    uint32_t rlc_gpm  = g_hw.rreg(g_hw.ctx, (kSeg1 + dwRLC_GPM_STAT) * 4);

    uint32_t cp_stat_a = g_hw.rreg(g_hw.ctx, (kSeg0Conv + dwCP_STAT)     * 4);
    uint32_t cp_me_a   = g_hw.rreg(g_hw.ctx, (kSeg0Conv + dwCP_ME_CNTL)  * 4);
    uint32_t cp_mec_a  = g_hw.rreg(g_hw.ctx, (kSeg0Conv + dwCP_MEC_CNTL) * 4);
    uint32_t cp_stat_b = g_hw.rreg(g_hw.ctx, (kSeg0Old  + dwCP_STAT)     * 4);

    rbgpu_log("2dk DIAG [%s]: RLC_CNTL=0x%08x RLC_STAT=0x%08x RLC_GPM_STAT=0x%08x (seg1 0xa000, proven)",
              tag, rlc_cntl, rlc_stat, rlc_gpm);
    rbgpu_log("2dk DIAG [%s]: seg0=0x1260 -> CP_STAT=0x%08x CP_ME_CNTL=0x%08x CP_MEC_CNTL=0x%08x",
              tag, cp_stat_a, cp_me_a, cp_mec_a);
    rbgpu_log("2dk DIAG [%s]: seg0=0x0000 -> CP_STAT=0x%08x (the old assumption)",
              tag, cp_stat_b);

    rbgpu_diag_set_num("diag_rlc_cntl", rlc_cntl);
    rbgpu_diag_set_num("diag_rlc_stat", rlc_stat);
    rbgpu_diag_set_num("diag_rlc_gpm", rlc_gpm);
    rbgpu_diag_set_num("diag_cp_stat", cp_stat_a);
    rbgpu_diag_set_num("diag_cp_me_cntl", cp_me_a);
    rbgpu_diag_set_num("diag_cp_mec_cntl", cp_mec_a);
    rbgpu_diag_set_num("diag_cp_stat_seg0", cp_stat_b);

    // What the numbers mean, so a photograph of the console is already an
    // answer and nothing has to be looked up afterwards.
    if (rlc_stat == 0x25) {
        rbgpu_log("2dk DIAG [%s]: RLC_STAT=0x25 matches the Raphael reference "
                  "(RLC firmware present in instruction RAM)", tag);
    } else {
        rbgpu_log("2dk DIAG [%s]: RLC_STAT=0x%08x is NOT the reference 0x25", tag, rlc_stat);
    }
    // 2dl (2026-10-12): which seg0 base is real, settled by registers that
    // CANNOT be zero when the block is the right one.
    //
    // The 2dk boot read CP_STAT = 0x00000000 at BOTH candidate bases, so
    // "not 0xffffffff" was not enough to tell a real zero from a dead address
    // that happens to read 0 - the 2dk log line claiming "seg0=0x1260 decodes"
    // was overclaiming on exactly that. These registers discriminate:
    //
    //   CP_RB0_CNTL  - ring control. The driver has programmed the ring by
    //                  this point and even the reset value carries RB_BUFSZ,
    //                  so zero means we are not looking at the CP.
    //   CP_RB0_BASE  - ring base address, programmed by doStart.
    //
    // And one that answers the actual question rather than the addressing one:
    //
    //   CP_MEC_ME1_HEADER_DUMP - the header of the last packet the MEC
    //                  FETCHED. Nonzero means the command processor really did
    //                  read from the ring, which would move the fault from
    //                  "the CP never sees the work" to "the CP saw it and did
    //                  not complete it". Zero at the KIQ submit means it never
    //                  fetched anything.
    //
    // CP_RB0_RPTR / WPTR are in the ring-state check doStart makes after
    // startKIQ, so they are worth having next to the rest.
    const uint32_t dwCP_RB0_CNTL   = 0x1de1; // BASE_IDX 0
    const uint32_t dwCP_RB0_BASE   = 0x1de0; // BASE_IDX 0
    const uint32_t dwCP_RB0_RPTR   = 0x0f60; // BASE_IDX 0
    const uint32_t dwCP_RB0_WPTR   = 0x1df4; // BASE_IDX 0
    const uint32_t dwCP_MEC_HDR    = 0x0e2e; // BASE_IDX 0, ME1 header dump
    const uint32_t dwGRBM_STATUS   = 0x0da4; // BASE_IDX 0

    uint32_t rb_cntl_a = g_hw.rreg(g_hw.ctx, (kSeg0Conv + dwCP_RB0_CNTL) * 4);
    uint32_t rb_cntl_b = g_hw.rreg(g_hw.ctx, (kSeg0Old  + dwCP_RB0_CNTL) * 4);
    uint32_t rb_base   = g_hw.rreg(g_hw.ctx, (kSeg0Conv + dwCP_RB0_BASE) * 4);
    uint32_t rb_rptr   = g_hw.rreg(g_hw.ctx, (kSeg0Conv + dwCP_RB0_RPTR) * 4);
    uint32_t rb_wptr   = g_hw.rreg(g_hw.ctx, (kSeg0Conv + dwCP_RB0_WPTR) * 4);
    uint32_t mec_hdr   = g_hw.rreg(g_hw.ctx, (kSeg0Conv + dwCP_MEC_HDR)  * 4);
    uint32_t grbm      = g_hw.rreg(g_hw.ctx, (kSeg0Conv + dwGRBM_STATUS) * 4);

    rbgpu_log("2dl DIAG [%s]: seg0 test - CP_RB0_CNTL @0x1260=0x%08x @0x0000=0x%08x",
              tag, rb_cntl_a, rb_cntl_b);
    rbgpu_log("2dl DIAG [%s]: @0x1260 CP_RB0_BASE=0x%08x RPTR=0x%08x WPTR=0x%08x "
              "MEC_ME1_HEADER_DUMP=0x%08x GRBM_STATUS=0x%08x",
              tag, rb_base, rb_rptr, rb_wptr, mec_hdr, grbm);

    rbgpu_diag_set_num("diag_rb0_cntl", rb_cntl_a);
    rbgpu_diag_set_num("diag_rb0_cntl_s0", rb_cntl_b);
    rbgpu_diag_set_num("diag_rb0_base", rb_base);
    rbgpu_diag_set_num("diag_rb0_rptr", rb_rptr);
    rbgpu_diag_set_num("diag_rb0_wptr", rb_wptr);
    rbgpu_diag_set_num("diag_mec_hdr", mec_hdr);
    rbgpu_diag_set_num("diag_grbm_status", grbm);

    bool a_live = (rb_cntl_a != 0 && rb_cntl_a != 0xFFFFFFFF);
    bool b_live = (rb_cntl_b != 0 && rb_cntl_b != 0xFFFFFFFF);
    if (a_live && !b_live) {
        rbgpu_log("2dl DIAG [%s]: seg0 = 0x1260 CONFIRMED (CP_RB0_CNTL nonzero there "
                  "and not at 0) - the CP_STAT/halt-bit readings above are real", tag);
    } else if (b_live && !a_live) {
        rbgpu_log("2dl DIAG [%s]: seg0 = 0x0000 CONFIRMED - the 0x1260 readings above "
                  "are NOT the CP and must be discarded", tag);
    } else if (!a_live && !b_live) {
        rbgpu_log("2dl DIAG [%s]: CP_RB0_CNTL is 0/0xffffffff at BOTH bases - seg0 is "
                  "still UNKNOWN, so CP_STAT=0 and the halt bits prove NOTHING. Do not "
                  "read them as 'the CP is idle and unhalted'.", tag);
    } else {
        rbgpu_log("2dl DIAG [%s]: CP_RB0_CNTL is live at BOTH bases - ambiguous, "
                  "seg0 not settled", tag);
    }

    if (a_live) {
        // CP_ME_CNTL bits 28/26/24 halt PFP/CE/ME; CP_MEC_CNTL bits 30/28 halt MEC1/2.
        rbgpu_log("2dk DIAG [%s]: CP_STAT=0x%08x; ME_HALT=%u PFP_HALT=%u CE_HALT=%u "
                  "MEC1_HALT=%u MEC2_HALT=%u",
                  tag, cp_stat_a,
                  (cp_me_a  >> 28) & 1, (cp_me_a  >> 26) & 1, (cp_me_a >> 24) & 1,
                  (cp_mec_a >> 30) & 1, (cp_mec_a >> 28) & 1);
    }

    // 2dm (2026-10-12): do NOT read "header dump nonzero" as "the MEC fetched a
    // packet". The 2dl boot got 0xdef0def0 at post-gc-hw-init and 0xdef1def1 at
    // pre-kiq-submit, and CP_RB0_BASE read 0xfedcbaef. Those are fill patterns,
    // not data, and the header marching from def0 to def1 - incrementing by one
    // in BOTH halves - is a FIFO walking a pre-filled buffer, not a record of
    // real packets. A PM4 type-3 header does not step in sequence. The 2dl
    // "MEC fetched a packet: YES" line was overclaiming, the same way 2dk's
    // "seg0 decodes" did.
    //
    // CP_MEC_ME1_HEADER_DUMP pops the next entry on each read, so reading it
    // several times in a row characterises it: def2, def3, def4... is a
    // pre-filled pattern and proves nothing about fetching; varying or
    // repeating values would be real.
    {
        const uint32_t dwCP_MEC_HDR2 = 0x0e2f;  // ME2 header dump
        uint32_t h[4];
        for (int i = 0; i < 4; i++)
            h[i] = g_hw.rreg(g_hw.ctx, (kSeg0Conv + dwCP_MEC_HDR) * 4);
        uint32_t h2 = g_hw.rreg(g_hw.ctx, (kSeg0Conv + dwCP_MEC_HDR2) * 4);
        rbgpu_log("2dm DIAG [%s]: ME1_HEADER_DUMP x4 = 0x%08x 0x%08x 0x%08x 0x%08x ; "
                  "ME2 = 0x%08x", tag, h[0], h[1], h[2], h[3], h2);
        rbgpu_diag_set_num("diag_mec_hdr1", h[0]);
        rbgpu_diag_set_num("diag_mec_hdr4", h[3]);
        rbgpu_diag_set_num("diag_mec_hdr_me2", h2);

        bool stepping = (h[1] == h[0] + 0x00010001u) && (h[2] == h[1] + 0x00010001u);
        if (stepping) {
            rbgpu_log("2dm DIAG [%s]: the header dump STEPS by 0x00010001 per read - "
                      "a pre-filled pattern buffer, NOT fetched packet headers. It says "
                      "nothing about whether the CP read the ring.", tag);
        } else if (h[0] == h[1] && h[1] == h[2]) {
            rbgpu_log("2dm DIAG [%s]: the header dump is constant across reads (0x%08x) - "
                      "not a popping FIFO here", tag, h[0]);
        } else {
            rbgpu_log("2dm DIAG [%s]: the header dump varies irregularly - these may be "
                      "real packet headers (PM4 type 3 has bits 31:30 = 11)", tag);
        }
    }

    // The direct question, and the one doStart itself asks: is the queue live?
    // The KIQ is a compute queue, so CP_HQD_ACTIVE bit 0 is "this HQD is
    // active"; CP_GFX_HQD_ACTIVE is the one doStart polls in waitForActiveValue.
    // CP_HQD_PQ_BASE and the doorbell control say whether the queue was ever
    // pointed at a ring and a doorbell at all - which is where the fault must
    // be if the CP is unhalted and idle while a packet is supposedly in flight.
    {
        const uint32_t dwCP_HQD_ACTIVE     = 0x1fab;
        const uint32_t dwCP_GFX_HQD_ACTIVE = 0x1e80;
        const uint32_t dwCP_HQD_PQ_BASE    = 0x1fb1;
        const uint32_t dwCP_HQD_PQ_DOORBELL= 0x1fb8;

        uint32_t hqd_active     = g_hw.rreg(g_hw.ctx, (kSeg0Conv + dwCP_HQD_ACTIVE)     * 4);
        uint32_t gfx_hqd_active = g_hw.rreg(g_hw.ctx, (kSeg0Conv + dwCP_GFX_HQD_ACTIVE) * 4);
        uint32_t hqd_pq_base    = g_hw.rreg(g_hw.ctx, (kSeg0Conv + dwCP_HQD_PQ_BASE)    * 4);
        uint32_t hqd_doorbell   = g_hw.rreg(g_hw.ctx, (kSeg0Conv + dwCP_HQD_PQ_DOORBELL)* 4);

        rbgpu_log("2dm DIAG [%s]: CP_HQD_ACTIVE=0x%08x CP_GFX_HQD_ACTIVE=0x%08x "
                  "CP_HQD_PQ_BASE=0x%08x CP_HQD_PQ_DOORBELL_CONTROL=0x%08x",
                  tag, hqd_active, gfx_hqd_active, hqd_pq_base, hqd_doorbell);
        rbgpu_diag_set_num("diag_hqd_active", hqd_active);
        rbgpu_diag_set_num("diag_gfx_hqd_active", gfx_hqd_active);
        rbgpu_diag_set_num("diag_hqd_pq_base", hqd_pq_base);
        rbgpu_diag_set_num("diag_hqd_doorbell", hqd_doorbell);
        rbgpu_log("2dm DIAG [%s]: KIQ queue active (CP_HQD_ACTIVE bit 0) = %u; "
                  "doorbell enabled (bit 30) = %u",
                  tag, hqd_active & 1, (hqd_doorbell >> 30) & 1);

        // 2dn: the CP_HQD_* block is a WINDOW. GRBM_GFX_CNTL selects which
        // ME / PIPE / QUEUE / VMID those registers refer to, so an all-zero
        // read only means "the KIQ was never mapped" if the selector is
        // pointing at the KIQ's own slot. Report the selector and the spec
        // side by side instead of assuming they agree.
        const uint32_t dwGRBM_GFX_CNTL = 0x0dc2;  // BASE_IDX 0
        uint32_t gfx_cntl = g_hw.rreg(g_hw.ctx, (kSeg0Conv + dwGRBM_GFX_CNTL) * 4);
        rbgpu_diag_set_num("diag_grbm_gfx_cntl", gfx_cntl);
        rbgpu_log("2dn DIAG [%s]: GRBM_GFX_CNTL=0x%08x (the ME/PIPE/QUEUE/VMID "
                  "selector the CP_HQD_* window follows); startKIQ spec = "
                  "0x%08x 0x%08x 0x%08x",
                  tag, gfx_cntl, g_rb_kiq_spec0, g_rb_kiq_spec1, g_rb_kiq_spec2);
        if (gfx_cntl == 0 && (hqd_active | hqd_pq_base | hqd_doorbell) == 0) {
            rbgpu_log("2dn DIAG [%s]: selector is 0 and the window is all zero. If the "
                      "KIQ lives at a nonzero me/pipe/queue this read is of the WRONG "
                      "slot and proves nothing; if it lives at 0/0/0 the queue really "
                      "was never programmed.", tag);
        }

        // 2do: now look where the KIQ actually lives. Only once startKIQ has
        // run and told us the slot; before that there is nothing to aim at.
        // 2dp: me=2 pipe=1 queue=0 is now a measured fact (the 2do boot found
        // ACTIVE=1, DOORBELL enabled and CTRL=0xc030860d only there), so probe
        // it at EVERY point - including post-gc-hw-init, before startKIQ runs.
        // That answers the one thing 2do could not: whether the queue is
        // something startKIQ created, or was already there from the firmware.
        if (g_rb_kiq_spec0 == 0xFFFFFFFFu && g_hw.wreg) {
            rbgpu_log("2dp [%s]: startKIQ has not run yet; probing the known KIQ slot "
                      "me=2 pipe=1 queue=0 anyway, to see whether the queue predates it",
                      tag);
            rbgpu_probe_hqd_slot(tag, 2, 1, 0, 0);
            g_hw.wreg(g_hw.ctx, (kSeg0Conv + dwGRBM_GFX_CNTL) * 4, gfx_cntl);
        } else if (g_rb_kiq_spec0 != 0xFFFFFFFFu && g_hw.wreg) {
            uint32_t want_base = (uint32_t)(g_rb_kiq_ring_mc >> 8);
            rbgpu_log("2do [%s]: probing the HQD window. KIQ slot from the spec is "
                      "me=%u pipe=%u queue=%u; ring MC 0x%llx so CP_HQD_PQ_BASE should "
                      "read 0x%08x. Saving GRBM_GFX_CNTL=0x%08x and restoring it after.",
                      tag, g_rb_kiq_spec0, g_rb_kiq_spec1, g_rb_kiq_spec2,
                      g_rb_kiq_ring_mc, want_base, gfx_cntl);

            bool found = rbgpu_probe_hqd_slot(tag, g_rb_kiq_spec0, g_rb_kiq_spec1,
                                              g_rb_kiq_spec2, want_base);
            rbgpu_diag_set_num("hqd_slot_found", found ? 1 : 0);
            if (!found) {
                // 2dp: the 2do sweep found the slot alive but PQ_BASE =
                // 0xffbfe800, not the ring MC >> 8. That mismatch is NOT
                // evidence of a wrong ring: a compute HQD's PQ_BASE holds a
                // GPUVM virtual address translated through the queue's VMID,
                // and startKIQ was handed a physical MC address. They are not
                // the same number and there is no reason they should be. The
                // sweep is dropped - it only reprinted the same slot through
                // aliased selectors - and the question is now RPTR vs WPTR.
                rbgpu_log("2dp [%s]: PQ_BASE != ring MC >> 8 (0x%08x), which is EXPECTED: "
                          "a compute HQD holds a GPUVM VA, not the physical address "
                          "startKIQ was given. Not evidence of a wrong ring.",
                          tag, want_base);
            }

            // Restore whatever the selector held before we touched it.
            g_hw.wreg(g_hw.ctx, (kSeg0Conv + dwGRBM_GFX_CNTL) * 4, gfx_cntl);
            uint32_t back = g_hw.rreg(g_hw.ctx, (kSeg0Conv + dwGRBM_GFX_CNTL) * 4);
            rbgpu_log("2do [%s]: GRBM_GFX_CNTL restored to 0x%08x (read back 0x%08x)",
                      tag, gfx_cntl, back);
        }
    }
}

static int hooked_gc_hw_init(void *gc_buf, void *arg2, void *arg3) {
    rbgpu_diag_inc("gc_hw_sub_calls");
    rbgpu_log("_gc_hw_init entered: gc_buf=%p, arg2=%p, arg3=%p", gc_buf, arg2, arg3);

    // 2ba: CORRECT offsets (2ay/2az were wrong).
    // gc_buf+0xae8=0xec04 is the FULL DWORD (already includes 0xa000 base).
    // gc_buf+0xaec=0xee8d is the FULL DWORD.
    // BYTE = DWORD * 4. Do NOT add 0xa000 again.
    if (gc_buf && g_hw.ctx && g_hw.rreg) {
        uint32_t off_ae8 = *(uint32_t*)((uint8_t*)gc_buf + 0xae8);  // 0xec04
        uint32_t off_aec = *(uint32_t*)((uint8_t*)gc_buf + 0xaec);  // 0xee8d
        uint8_t flag_2d6 = *(uint8_t*)((uint8_t*)gc_buf + 0x2d6);
        uint8_t flag_2d9 = *(uint8_t*)((uint8_t*)gc_buf + 0x2d9);

        // Correct BYTE addresses (DWORD * 4, no extra base)
        uint32_t rlc_stat_byte = off_ae8 * 4;      // 0xec04 * 4 = 0x3b010
        uint32_t bootload_byte = off_aec * 4;      // 0xee8d * 4 = 0x3ba34
        uint32_t rlc_cntl_byte = (0xa000 + 0x4c00) * 4;  // 0x3b000 (offset needs base)

        uint32_t rlc_stat = g_hw.rreg(g_hw.ctx, rlc_stat_byte);
        uint32_t bootload = g_hw.rreg(g_hw.ctx, bootload_byte);
        uint32_t rlc_cntl = g_hw.rreg(g_hw.ctx, rlc_cntl_byte);

        rbgpu_log("2ba CORRECT: RLC_CNTL=0x%08x RLC_STAT=0x%08x BOOTLOAD=0x%08x flags=%x/%x",
                  rlc_cntl, rlc_stat, bootload, flag_2d6, flag_2d9);
        rbgpu_log("2ba ADDR: stat_byte=0x%x bootload_byte=0x%x cntl_byte=0x%x",
                  rlc_stat_byte, bootload_byte, rlc_cntl_byte);
    }

    int ret = 1;
    if (orig_gc_hw_init) {
        ret = orig_gc_hw_init(gc_buf, arg2, arg3);
    }

    rbgpu_log("_gc_hw_init returned %d", ret);
    rbgpu_diag_set_num("gc_hw_sub_ret", ret);
    // 2au: 2ar post-init RLC enable REMOVED (see above). Diagnostic read only.
    // 2ap: dump HW state after GC init (with bypasses active) to verify reality
    if (g_psp_hw_mode)
        rbgpu_dump_gc_regs("post-gc-hw-init");
    return ret;
}

static uint32_t hooked_gc_get_hw_version(void *version_slice) {
    rbgpu_diag_inc("gc_get_ver_calls");
    uint32_t ver = 0;
    if (orig_gc_get_hw_version) {
        ver = orig_gc_get_hw_version(version_slice);
    }
    rbgpu_log("_gc_get_hw_version(slice=%p) returned 0x%08X", version_slice, ver);
    rbgpu_diag_set_num("gc_hw_version", ver);
    return ver;
}

static void hooked_gc_get_config(void *gc_buf, uint32_t hw_version) {
    rbgpu_diag_inc("gc_get_cfg_calls");
    rbgpu_log("_gc_get_config entered: gc_buf=%p, hw_version=0x%08X", gc_buf, hw_version);
    if (orig_gc_get_config) {
        orig_gc_get_config(gc_buf, hw_version);
    }
    rbgpu_log("_gc_get_config exited");
}

static void hooked_gc_get_hw_config_tbl_info(void *gc_buf, uint32_t hw_version) {
    rbgpu_diag_inc("gc_tbl_info_calls");
    rbgpu_log("_gc_get_hw_config_tbl_info entered: gc_buf=%p, hw_version=0x%08X", gc_buf, hw_version);
    if (orig_gc_get_hw_config_tbl_info) {
        orig_gc_get_hw_config_tbl_info(gc_buf, hw_version);
    }
    uint32_t sz = gc_buf ? *reinterpret_cast<uint32_t *>(reinterpret_cast<uintptr_t>(gc_buf) + 0xd38) : 0;
    rbgpu_log("_gc_get_hw_config_tbl_info exited: table_size@+0xd38=0x%x", sz);
    rbgpu_diag_set_num("gc_tbl_info_sz", sz);
}

static int hooked_gc_set_fw_entry_info(void *p1, void *p2, uint32_t rev, void *p4) {
    rbgpu_diag_inc("gc_set_fw_info_calls");
    rbgpu_log("_gc_set_fw_entry_info entered: p1=%p, p2=%p, rev=%u, p4=%p", p1, p2, rev, p4);
    // 2af: read-only dump of the p4 block (first 8 dwords). p4 is an
    // Apple-owned pointer; this only reads, to learn the entry format the
    // yellow_carp firmware-replacement step will need to fabricate.
    if (p4) {
        uint32_t *d = reinterpret_cast<uint32_t *>(p4);
        rbgpu_log("_gc_set_fw_entry_info p4 dwords: %08x %08x %08x %08x %08x %08x %08x %08x",
                  d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7]);
    }
    int ret = 1;
    if (orig_gc_set_fw_entry_info) {
        ret = orig_gc_set_fw_entry_info(p1, p2, rev, p4);
    }
    rbgpu_log("_gc_set_fw_entry_info returned %d", ret);
    rbgpu_diag_set_num("gc_set_fw_info_ret", ret);
    // 2ag: post-call dump. If the version-check patch let _gc_10_3_get_fw_constants
    // run, the p4 buffer now holds the firmware entries it registered: this is
    // the layout the yellow_carp substitution step must replicate.
    if (p4) {
        uint32_t *d = reinterpret_cast<uint32_t *>(p4);
        rbgpu_log("_gc_set_fw_entry_info p4 post dwords: %08x %08x %08x %08x %08x %08x %08x %08x",
                  d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7]);
    }
    return ret;
}

static int hooked_gc_init_fcn_ptr_list(void *gc_buf, uint32_t hw_version) {
    rbgpu_diag_inc("gc_fcn_ptr_calls");
    rbgpu_log("_gc_init_fcn_ptr_list entered: gc_buf=%p, hw_version=0x%08X", gc_buf, hw_version);

    uint32_t version_to_pass = hw_version;
    if (g_spoof_ttl_mode && hw_version == 0xA0303) {
        version_to_pass = 0xA0300;
        rbgpu_log("_gc_init_fcn_ptr_list: -rbgpuspoofttl active -> spoofing hw_version 0x%08X -> 0x%08X",
                  hw_version, version_to_pass);
    }

    int ret = 2;
    if (orig_gc_init_fcn_ptr_list) {
        ret = orig_gc_init_fcn_ptr_list(gc_buf, version_to_pass);
    }
    rbgpu_log("_gc_init_fcn_ptr_list returned %d (passed ver: 0x%08X)", ret, version_to_pass);
    rbgpu_diag_set_num("gc_fcn_ptr_ret", ret);
    return ret;
}

static int hooked_gc_get_doorbell_requirements(void *gc_buf, void *out_reqs) {
    rbgpu_diag_inc("gc_doorbell_req_calls");
    rbgpu_log("_gc_get_doorbell_requirements entered: gc_buf=%p, out_reqs=%p", gc_buf, out_reqs);
    int ret = 1;
    if (orig_gc_get_doorbell_requirements) {
        ret = orig_gc_get_doorbell_requirements(gc_buf, out_reqs);
    }
    rbgpu_log("_gc_get_doorbell_requirements returned %d", ret);
    rbgpu_diag_set_num("gc_doorbell_req_ret", ret);
    return ret;
}

static uint8_t hooked_ttlDevGetDoorbellRange(void *dev, uint32_t type, uint32_t idx, void *out_info) {
    rbgpu_diag_inc("gc_doorbell_calls");
    uint8_t ret = 0;
    if (orig_ttl_dev_get_doorbell_range) {
        ret = orig_ttl_dev_get_doorbell_range(dev, type, idx, out_info);
    }
    rbgpu_log("_ttlDevGetDoorbellRange(dev=%p, type=%u, idx=%u, out=%p) returned %u",
              dev, type, idx, out_info, ret);
    rbgpu_diag_set_num("gc_doorbell_ret", ret);
    return ret;
}

static uint8_t hooked_IpiSetFwEntry(void *instance, void *fw_entry) {
    rbgpu_diag_inc("gc_set_fw_calls");
    uint8_t ret = 0;
    bool skip_orig = false; // 2al: set for entries we deliberately do not register
    // 2af: read-only dump of the firmware entry descriptor (first 8 dwords),
    // before and after the original call, to learn its layout for the
    // yellow_carp firmware-replacement step. Nothing here is modified.
    if (fw_entry) {
        uint32_t *d = reinterpret_cast<uint32_t *>(fw_entry);
        rbgpu_log("_IpiSetFwEntry pre : instance=%p entry=%p id=%u dwords: %08x %08x %08x %08x %08x %08x %08x %08x",
                  instance, fw_entry, d[0],
                  d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7]);
        // 2ai: identify the blob. data_ptr (d[2..3]) points at the firmware
        // image: dump its first 16 bytes for the amdgpu_ucode.h header decode
        // (docs/linux-ref). aux_ptr (d[6..7]) may hold version/name info.
        // Read-only; the entry itself is untouched.
        uint64_t data_ptr = static_cast<uint64_t>(d[2]) | (static_cast<uint64_t>(d[3]) << 32);
        if (data_ptr) {
            uint8_t *b = reinterpret_cast<uint8_t *>(data_ptr);
            rbgpu_log("_IpiSetFwEntry data hdr: %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x",
                      b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
                      b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
        }
        uint64_t aux_ptr = static_cast<uint64_t>(d[6]) | (static_cast<uint64_t>(d[7]) << 32);
        if (aux_ptr) {
            uint32_t *a = reinterpret_cast<uint32_t *>(aux_ptr);
            rbgpu_log("_IpiSetFwEntry aux: %08x %08x %08x %08x %08x %08x %08x %08x",
                      a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]);
        }
        // 2aj: yellow_carp substitution. Apple's Navi21 images would fail the
        // Rembrandt PSP's per-ASIC authentication, so swap the entries we have
        // exact yellow_carp counterparts for (boot-31 symbol map). The swap is
        // observable in dry-run logs but only consumed if -rbgpupsphw is set.
        // d[2..3] = data_ptr, d[4] = size. Images are headerless (see
        // fw_yellow_carp.cpp): data_ptr -> image bytes, size -> image size.
        extern const uint8_t yc_ce_img[];
        extern const uint8_t yc_pfp_img[];
        extern const uint8_t yc_me_img[];
        extern const uint8_t yc_mec_img[];
        extern const uint8_t yc_rlc_img[];
        extern const uint8_t yc_sdma_img[];
        extern const uint8_t yc_toc_img[];
        const uint8_t *yc_img = nullptr;
        uint32_t yc_size = 0;
        switch (d[0]) {
            case 1:  yc_img = yc_ce_img;   yc_size = 0x40380; break; // CE
            case 2:  yc_img = yc_pfp_img;  yc_size = 0x40400; break; // PFP
            case 3:  yc_img = yc_me_img;   yc_size = 0x40400; break; // ME
            case 6:  yc_img = yc_mec_img;  yc_size = 0x41680; break; // MEC
            case 9:  yc_img = yc_rlc_img;  yc_size = 0x06200; break; // RLC
            case 14: yc_img = yc_sdma_img; yc_size = 0x08400; break; // SDMA0
            case 36: yc_img = yc_toc_img;  yc_size = 0x00600; break; // PSP TOC
            default: break;
        }
        if (yc_img) {
            d[2] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(yc_img));
            d[3] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(yc_img) >> 32);
            d[4] = yc_size;
            rbgpu_log("_IpiSetFwEntry YC-SWAP id=%u -> img=%p size=0x%x", d[0], yc_img, yc_size);
            rbgpu_diag_inc("yc_swapped");
        }
        // 2al: skip Navi21 TAP_DELAYS (ids 31-35: GLOBAL, SE0-SE3). Boot 35
        // (first -rbgpupsphw boot) proved the PSP itself initializes and loads
        // its own firmware on this PSP 13, but _psp_np_fw_load then died on
        // "SE1 Tap Delays": cmd 0x6 rejected by the TOS with 0xffff0006 x4.
        // These are Navi21 dGPU PHY tuning blobs; the Rembrandt TOS rejects
        // them and linux-firmware ships no yellow_carp equivalents. Do not
        // register them: report success without calling the original, so the
        // PSP never attempts the load and np_fw_load can proceed to the next
        // firmware.
        if (d[0] >= 31 && d[0] <= 35) {
            rbgpu_log("_IpiSetFwEntry TAP-SKIP id=%u (not registered)", d[0]);
            rbgpu_diag_inc("tap_skipped");
            skip_orig = true;
            ret = 1;
        }
    } else {
        rbgpu_log("_IpiSetFwEntry pre : instance=%p entry=NULL", instance);
    }
    if (orig_ipi_set_fw_entry && !skip_orig) {
        ret = orig_ipi_set_fw_entry(instance, fw_entry);
    }
    if (fw_entry) {
        uint32_t *d = reinterpret_cast<uint32_t *>(fw_entry);
        rbgpu_log("_IpiSetFwEntry post: id=%u dwords: %08x %08x %08x %08x %08x %08x %08x %08x ret=%u",
                  d[0], d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7], ret);
    } else {
        rbgpu_log("_IpiSetFwEntry post: ret=%u", ret);
    }
    rbgpu_diag_set_num("gc_set_fw_ret", ret);
    return ret;
}

static uint32_t hooked_ttl_initialize(void *arg1, void *arg2) {
    rbgpu_diag_inc("ttl_raw_init_calls");
    rbgpu_diag_set_num("ttl_raw_init_arg1", reinterpret_cast<uint64_t>(arg1));
    rbgpu_diag_set_num("ttl_raw_init_arg2", reinterpret_cast<uint64_t>(arg2));

    auto params = reinterpret_cast<const uint64_t *>(arg1);
    rbgpu_log("_ttl_initialize entered: params=%p, ctx_38=%p", arg1, arg2);

    if (params) {
        rbgpu_diag_set_num("ttl_param_q0", params[0]);
        rbgpu_diag_set_num("ttl_param_q1", params[1]);
        rbgpu_diag_set_num("ttl_param_q2", params[2]);
        rbgpu_diag_set_num("ttl_param_q3", params[3]);
        rbgpu_diag_set_num("ttl_param_q4", params[4]);
        rbgpu_diag_set_num("ttl_param_q5", params[5]);

        rbgpu_log("ttl_params[0x00..0x18]: 0x%016llx 0x%016llx 0x%016llx 0x%016llx",
                  params[0], params[1], params[2], params[3]);
        rbgpu_log("ttl_params[0x20..0x38]: 0x%016llx 0x%016llx 0x%016llx 0x%016llx",
                  params[4], params[5], params[6], params[7]);
        rbgpu_log("ttl_params[0x40..0x58]: 0x%016llx 0x%016llx 0x%016llx 0x%016llx",
                  params[8], params[9], params[10], params[11]);
        rbgpu_log("ttl_params[0x60..0x78]: 0x%016llx 0x%016llx 0x%016llx 0x%016llx",
                  params[12], params[13], params[14], params[15]);
        rbgpu_log("ttl_params[0x80..0x98]: 0x%016llx 0x%016llx 0x%016llx 0x%016llx",
                  params[16], params[17], params[18], params[19]);
        rbgpu_log("ttl_params[0xA0]:      0x%016llx", params[20]);
    }

    uint32_t ret = 1;
    if (orig_ttl_initialize) {
        ret = orig_ttl_initialize(arg1, arg2);
    }

    rbgpu_log("_ttl_initialize returned status = 0x%08x", ret);
    rbgpu_diag_set_num("ttl_raw_init_ret", ret);
    // 2ap SAFETY: This is a DIAGNOSTIC build. If ttl_initialize succeeded, we
    // have the register dumps we need. Force failure (same code as boot 36's
    // GC-timeout path, which is known-safe) so Apple's X6000 start() takes the
    // graceful failure path instead of proceeding into the code that panicked
    // on boot 39. The panic investigation needs the diag data, not another panic.
    // 2bf: DISABLED - trap-3 guard (2be) now protects against the boot-39 panic.
    // Let ttl_initialize return naturally so start() can proceed.
    if (false && g_psp_hw_mode && ret == 0) {
        rbgpu_log("2ap SAFETY: ttl_initialize succeeded, FORCING 0xe00002bc to avoid boot-39 panic");
        rbgpu_diag_set_num("diag_forced_fail", 1);
        return 0xe00002bc;
    }
    return ret;
}

static int hooked_AmdTtlServices_initialize(void *this_ptr) {
    rbgpu_diag_inc("ttl_init_calls");
    rbgpu_diag_set_num("ttl_init_this", reinterpret_cast<uint64_t>(this_ptr));
    rbgpu_log("AmdTtlServices::initialize() entered (this=%p)", this_ptr);
    int ret = 0;
    if (orig_amdtll_initialize) {
        ret = orig_amdtll_initialize(this_ptr);
    }
    rbgpu_log("AmdTtlServices::initialize() returned 0x%08X (this=%p)", ret, this_ptr);
    rbgpu_diag_set_num("ttl_init_ret", static_cast<uint32_t>(ret));
    // 2aq SAFETY (same as 2ap): diagnostic build. If init succeeded, force
    // failure so X6000::start() takes the graceful path, not the panic path.
    // 2ap only guarded _ttl_initialize; X6000 may use this C++ method instead.
    // 2bf: DISABLED - trap-3 guard (2be) now protects against the boot-39 panic.
    if (false && g_psp_hw_mode && ret == 0) {
        rbgpu_log("2aq SAFETY: AmdTtlServices::initialize succeeded, FORCING 0xe00002bc");
        rbgpu_diag_set_num("diag_forced_fail_cpp", 1);
        return 0xe00002bc;
    }
    return ret;
}

static void hooked_AmdTtlServices_setFirmwareDirectory(void *this_ptr, void *fw_dir) {
    rbgpu_diag_inc("ttl_set_fw_dir_calls");
    rbgpu_diag_set_num("ttl_set_fw_dir_this", reinterpret_cast<uint64_t>(this_ptr));
    rbgpu_diag_set_num("ttl_set_fw_dir_ptr", reinterpret_cast<uint64_t>(fw_dir));

    uint64_t pre_570 = this_ptr ? *reinterpret_cast<uint64_t *>(reinterpret_cast<uintptr_t>(this_ptr) + 0x570) : 0;
    uint64_t pre_958 = this_ptr ? *reinterpret_cast<uint64_t *>(reinterpret_cast<uintptr_t>(this_ptr) + 0x958) : 0;
    rbgpu_log("AmdTtlServices::setFirmwareDirectory(this=%p, dir=%p) pre +0x570=0x%llx, pre +0x958=0x%llx",
              this_ptr, fw_dir, pre_570, pre_958);

    if (orig_amdtll_set_fw_dir) {
        orig_amdtll_set_fw_dir(this_ptr, fw_dir);
    }

    uint64_t post_570 = this_ptr ? *reinterpret_cast<uint64_t *>(reinterpret_cast<uintptr_t>(this_ptr) + 0x570) : 0;
    uint64_t post_958 = this_ptr ? *reinterpret_cast<uint64_t *>(reinterpret_cast<uintptr_t>(this_ptr) + 0x958) : 0;
    rbgpu_diag_set_num("ttl_set_fw_dir_post_570", post_570);
    rbgpu_diag_set_num("ttl_set_fw_dir_post_958", post_958);
    rbgpu_log("AmdTtlServices::setFirmwareDirectory done: this=%p post +0x570=0x%llx, post +0x958=0x%llx",
              this_ptr, post_570, post_958);
}

// 2bg: Hook submitFrame to log (queueType, index) when pHandle is NULL.
// This identifies which queues are missing engines.
static uint32_t hooked_AmdTtlServices_submitFrame(void *this_ptr, uint32_t queue_type, uint32_t index, void *param) {
    rbgpu_log("2bg submitFrame: this=%p queueType=%u index=%u param=%p", this_ptr, queue_type, index, param);
    uint32_t ret = 0xe00002bc;
    if (orig_amdtll_submit_frame) {
        ret = orig_amdtll_submit_frame(this_ptr, queue_type, index, param);
    }
    rbgpu_log("2bg submitFrame returned 0x%x (queueType=%u index=%u)", ret, queue_type, index);
    return ret;
}

// 2bg: Hook createEngine to log when engines are created.
// 2bi REVERTED (2bj): The (0,1) creation panicked in _TtlRtsCreateEngine (NULL call).
// Do NOT create extra engines - driver's internal state doesn't support it.
static int hooked_AmdTtlServices_createEngine(void *this_ptr, uint32_t queue_type, uint32_t index, void *input) {
    rbgpu_log("2bg createEngine: this=%p queueType=%u index=%u input=%p", this_ptr, queue_type, index, input);
    int ret = -1;
    if (orig_amdtll_create_engine) {
        ret = orig_amdtll_create_engine(this_ptr, queue_type, index, input);
    }
    rbgpu_log("2bg createEngine returned %d (queueType=%u index=%u)", ret, queue_type, index);
    return ret;
}

// 2bh: Hook buildCommand to log (queueType, index) for pHandle failures.
// 2bl: Redirect (0,1) -> (0,0). The driver only created (0,0); (0,1) has no engine.
//      Trying to create (0,1) panics in _TtlRtsCreateEngine (NULL function pointer).
//      Redirecting the 5 failing commands to the existing (0,0) engine is safe.
static uint32_t hooked_AmdTtlServices_buildCommand(void *this_ptr, uint32_t queue_type, uint32_t index, void *info, void *param) {
    uint32_t eff_queue_type = queue_type;
    uint32_t eff_index = index;
    // 2bl: Redirect the missing (0,1) to the existing (0,0)
    if (queue_type == 0 && index == 1) {
        eff_index = 0;
        rbgpu_log("2bl: redirecting buildCommand (0,1) -> (0,0)");
    }
    rbgpu_log("2bh buildCommand: this=%p queueType=%u index=%u (eff %u,%u) info=%p param=%p",
              this_ptr, queue_type, index, eff_queue_type, eff_index, info, param);
    uint32_t ret = 0xe00002bc;
    if (orig_amdtll_build_command) {
        ret = orig_amdtll_build_command(this_ptr, eff_queue_type, eff_index, info, param);
    }
    rbgpu_log("2bh buildCommand returned 0x%x (queueType=%u index=%u)", ret, queue_type, index);
    return ret;
}

// 2bm: Hook AMDRTHardware::allocateMemoryResources (vtable[0x120]).
// This returns false, causing "Failed to init HW Acelerator resources".
// 2bz: REMOVED the force-true hack (per the assistant option 2). Now we diagnose
// WHY it fails legitimately instead of bypassing it.
static uint8_t hooked_AMDRTHardware_allocateMemoryResources(void *this_ptr) {
    rbgpu_log("2bm allocateMemoryResources: this=%p entered", this_ptr);
    uint8_t ret = 0;
    if (orig_amdrt_allocate_memory_resources) {
        ret = orig_amdrt_allocate_memory_resources(this_ptr);
    }
    rbgpu_log("2bm allocateMemoryResources returned %u (0=fail, 1=success)", ret);
    // 2bz: No longer forcing success. Let it fail naturally so we can diagnose.
    return ret;
}

// 2cb: REPLACE AMDHardware::allocateMemoryResources with a fully instrumented
// version. The 2bz boot proved it returns false with 7 engines present, but
// didn't identify WHICH step fails. This replicates the original's logic
// step-by-step with logging (from disassembly of 0xbea8de0):
//   Step 1: obj(hw+0x18)->vtable[0x208](obj, hw+0x510, ((hw+0x530)&0xff)<<12+0x1000, 3, 0, 3)
//           -> stored at hw+0x518; if NULL, return false.
//   Step 2: for i in 0..10: eng = hw+0x3b0[i]; if eng: if (!eng->vtable[0x120](eng)) return false.
//   Return true if all pass.
// Globals for panic-embedded diagnostics (log is lost on failure path).
static volatile int g_rb_alloc_fail_step = -1;  // 0 = all OK, 1 = vtable[0x208] NULL, 2 = engine N failed
static volatile int g_rb_alloc_fail_engine = -1;
static volatile void *g_rb_alloc_fail_engine_ptr = nullptr;
// Kept for RouteRequest's signature; 2cb replaces the original and never calls it.
static bool (*orig_hw_allocateMemoryResources)(void *) = nullptr;

// 2ce: Trace the powerUp -> doStart -> initComputeMQD call chain.
// 2cd showed initComputeMQD is never called, so the failure is earlier.
// The powerUp half lives in hooked_pm4_powerUp (2bu): one route per function.
// 2cf: Call-chain flags stashed for the panic line (RBGPULog is lost on panic).
static volatile int g_rb_2cf_powerup_called = 0;
static volatile int g_rb_2cf_dostart_called = 0;
static volatile int g_rb_2cf_initmqd_called = 0;
static volatile int g_rb_2cf_initmqd_result = -1;
// 2ch: Track startKIQ (called by doStart after initComputeMQD).
static volatile int g_rb_2ch_startkiq_called = 0;
static volatile int g_rb_2ch_startkiq_result = -1;
// 2cm survive mode: globals removed (unused).

static bool (*orig_pm4_doStart_2ce)(void *, bool) = nullptr;

static bool hooked_pm4_doStart_2ce(void *pm4, bool arg) {
    rbgpu_log("2ce: AMDGFX10PM4Engine::doStart entered (pm4=%p arg=%d)", pm4, arg ? 1 : 0);
    g_rb_2cf_dostart_called = 1;
    // Call original - our initComputeMQD hook (2cd) will log if it's reached
    bool ret = orig_pm4_doStart_2ce ? orig_pm4_doStart_2ce(pm4, arg) : false;
    rbgpu_log("2ce: doStart returned %d", ret ? 1 : 0);
    return ret;
}

// 2cd: REPLACE AMDGFX10PM4Engine::initComputeMQD with a fully instrumented
// step-by-step version. doStart() calls initComputeMQD(4) first; if it returns
// false, engine 0's powerUp fails. This identifies which of the 5 failure
// branches triggers.
// 2ch: Hook AMDGFX10KIQHWChannel::startKIQ to see if doStart fails there.
static int (*orig_kiq_startKIQ_2ch)(void *, uint64_t, uint64_t, void *, void *) = nullptr;

// 2ci: REPLACE startKIQ with instrumented version. The original:
//   obj = (this+0x20)->vtable[0x2c0]()
//   ret = obj->vtable[0x120](obj, 8, 0, &s1, &s2)
//   if ret != 0: return ret
//   else: compare queue spec; return 0 (match) or 0xe00002bc (mismatch)
// 2ch showed it returns 0, meaning vtable[0x120] returned 0 AND spec matched.
//
// 2co (2026-10-12): 2cl's forcing of the return to 1 is REVERTED. startKIQ
// returns a STATUS, not a bool, and 0 is success. doStart (X6000 7.0.25
// 0xbea126d) does:
//     testl %eax, %eax
//     je    0xbea1296        ; eax == 0  -> continue (the long path)
//     callq *0x138(%r14)     ; eax != 0  -> abort helper
//     xorl  %r14d, %r14d     ; -> return false
// So the 2ch/2ck boots were already reporting success, and forcing 1 put
// doStart on the one branch that always fails. Observe only, never override.
// docs/SPEC-PM4-DOSTART.md section 2.
static int hooked_kiq_startKIQ_2co(void *this_ptr, uint64_t a, uint64_t b, void *c, void *d) {
    g_rb_2ch_startkiq_called = 1;
    if (!this_ptr) {
        rbgpu_log("2co: startKIQ called with NULL this -> returning 0 (success) unchanged");
        g_rb_2ch_startkiq_result = 0;
        return 0;
    }
    // 2dn: startKIQ writes NO register itself. Disassembled at 0xbec7688, it
    // builds a 0x38-byte descriptor on the stack from the channel's own fields
    // (+0x84 & ~3, 0x80, +0x48, +0xb8, +0xd0, the two u64 args, 0x200), then
    //
    //   rdi = this+0x20 ; callq *0x2c0(vtable)      -> some service object
    //   callq *0x120(that object's vtable) with (8, 0, descriptor, outbuf)
    //
    // and finally compares three dwords of the AMD_PM4_QUEUE_SPEC (arg c)
    // against the first three dwords the callee wrote into outbuf. Equal ->
    // return 0; different -> 0xE00002BC. So startKIQ ASKS the TTL/HWLibs layer
    // to create the KIQ queue (command 8) and only checks that the me/pipe/
    // queue it got back is the one it asked for.
    //
    // The 2dm boot shows that call returning success while CP_HQD_PQ_BASE and
    // CP_HQD_ACTIVE are 0, so the layer below reported a queue it never
    // programmed. Log the spec so we know which me/pipe/queue the KIQ is
    // supposed to occupy - the HQD registers are indexed by GRBM_GFX_CNTL, so
    // without that triple an all-zero HQD read cannot be told apart from
    // "we read a different queue slot".
    if (c) {
        const uint32_t *spec = reinterpret_cast<const uint32_t *>(c);
        g_rb_kiq_spec0 = spec[0];
        g_rb_kiq_spec1 = spec[1];
        g_rb_kiq_spec2 = spec[2];
        g_rb_kiq_ring_mc = a;
        rbgpu_log("2dn: startKIQ QUEUE_SPEC[0..2] = 0x%08x 0x%08x 0x%08x "
                  "(the me/pipe/queue the KIQ should occupy); ring args a=0x%llx b=0x%llx",
                  spec[0], spec[1], spec[2], a, b);
        rbgpu_diag_set_num("kiq_spec0", spec[0]);
        rbgpu_diag_set_num("kiq_spec1", spec[1]);
        rbgpu_diag_set_num("kiq_spec2", spec[2]);
    }

    int orig_ret = orig_kiq_startKIQ_2ch ? orig_kiq_startKIQ_2ch(this_ptr, a, b, c, d) : 0;
    g_rb_2ch_startkiq_result = orig_ret;
    rbgpu_log("2co: startKIQ returned %d (%s)", orig_ret,
              orig_ret == 0 ? "0 = SUCCESS, doStart continues" : "nonzero = failure");
    rbgpu_log("2dn: startKIQ claims the queue exists; the HQD registers below say "
              "whether anything was actually programmed");
    rbgpu_dump_gc_regs("post-startKIQ");
    return orig_ret;
}

// 2cp: AMDGFX10PM4Engine::waitForActiveValue(ring, bool expected, bool bounded).
// doStart calls it as (0, true, false). The third parameter is the one that
// arms the timeout: with it false, the counter at 0xbea15fb and the 100 us
// delay at 0xbea1607 are unreachable, and the poll of CP_GFX_HQD_ACTIVE
// (GC dword offset 0x1e80) only ends on 1 or 0xffffffff. Force it true so
// Apple's own 1000 x 100 us bound applies and an inactive ring returns false
// instead of wedging the CPU. docs/SPEC-PM4-DOSTART.md section 4.
static bool (*orig_pm4_waitForActiveValue_2cp)(void *, uint32_t, bool, bool) = nullptr;
static volatile int g_rb_2cp_wfav_calls = 0;
static volatile int g_rb_2cp_wfav_ret = -1;
static volatile int g_rb_2cp_wfav_bounded = 0;

static bool hooked_pm4_waitForActiveValue_2cp(void *pm4, uint32_t ring, bool expected,
                                              bool bounded) {
    g_rb_2cp_wfav_calls++;
    if (!bounded) {
        g_rb_2cp_wfav_bounded = 1;
        rbgpu_log("2cp: waitForActiveValue(ring=%u, expected=%d, bounded=0) -> forcing "
                  "bounded=1 (unbounded poll of CP_GFX_HQD_ACTIVE would hang)",
                  ring, expected ? 1 : 0);
        bounded = true;
    } else {
        rbgpu_log("2cp: waitForActiveValue(ring=%u, expected=%d, bounded=1) unchanged",
                  ring, expected ? 1 : 0);
    }
    rbgpu_log("2cx: waitForActiveValue ENTER (ring=%u) - polls CP_GFX_HQD_ACTIVE", ring);
    bool ret = orig_pm4_waitForActiveValue_2cp
                   ? orig_pm4_waitForActiveValue_2cp(pm4, ring, expected, bounded)
                   : false;
    g_rb_2cp_wfav_ret = ret ? 1 : 0;
    rbgpu_diag_set_num("wfav_calls", static_cast<uint64_t>(g_rb_2cp_wfav_calls));
    rbgpu_diag_set_num("wfav_bounded_forced", static_cast<uint64_t>(g_rb_2cp_wfav_bounded));
    rbgpu_diag_set_num("wfav_ret", static_cast<uint64_t>(g_rb_2cp_wfav_ret));
    rbgpu_log("2cp: waitForActiveValue(ring=%u) -> %d %s", ring, ret ? 1 : 0,
              ret ? "(ring ACTIVE)" : "(timed out or register read 0xffffffff)");
    return ret;
}

// 2cx (2026-10-12): observe AMDHWChannel::waitForHwStamp, the first point at
// which this driver asks the GPU to EXECUTE something and waits for the
// answer. submitKIQFrame (0xbe9572e) commits the KIQ frame and then calls
// this; it polls with a 0x4C4B40 us (5 s) timeout and re-arms while the
// channel's vtable[0x2a0] still says to keep waiting (0xbe855c0-0xbe855f0).
//
// The 2026-10-12 armed boot stopped exactly here: startKIQ returned 0, then
// nothing. A stamp that never arrives means the command processor is not
// executing the packet - i.e. the CP/MEC microcode the PSP was asked to load
// is not running. Observe only: no override, no skip. The point is to see the
// return value and the time it took.
static bool (*orig_hwchannel_waitForHwStamp_2cx)(void *, uint32_t) = nullptr;
static volatile int g_rb_2cx_stamp_calls = 0;
static volatile int g_rb_2cx_stamp_ret = -1;
static volatile uint64_t g_rb_2cx_stamp_us = 0;

static bool hooked_hwchannel_waitForHwStamp_2cx(void *ch, uint32_t stamp) {
    g_rb_2cx_stamp_calls++;
    rbgpu_diag_set_num("stamp_calls", static_cast<uint64_t>(g_rb_2cx_stamp_calls));
    rbgpu_log("2cx: waitForHwStamp ENTER (ch=%p stamp=%u) - waiting for the GPU to "
              "execute; 5 s timeout per poll", ch, stamp);

    // 2cy (2026-10-12): do NOT call the original by default.
    //
    // waitForHwStamp's 0x4C4B40 us (5 s) timeout is per poll, and the loop at
    // 0xbe855c0-0xbe855f0 re-arms it for as long as the channel's
    // vtable[0x2a0] says to keep waiting. With a command processor that never
    // consumes the packet that is an unbounded wait: the 2026-10-12 armed
    // boot was still polling request 0x14 at #24640 with the login window up
    // behind it, and only a power cycle ended it.
    //
    // The stamp cannot arrive while the CP microcode is not running, so
    // waiting is pure cost. Returning false immediately takes Apple's own
    // failure path - submitKIQFrame logs its error and returns false,
    // doStart exits through vtable[0x150] and returns false, powerUp and
    // powerUpHW fail - and with 2ct refusing the display pipe the boot then
    // COMPLETES to a desktop, where plugin_check.sh can read the whole log.
    // That turns every armed boot from a power cycle into a usable run.
    //
    // -rbgpustampwait restores the real wait, for the boot where the CP
    // finally does run and the stamp is expected to arrive.
    // 2dq (2026-10-12): read the queue HERE, not at the submit hook's entry.
    //
    // The 2dp boot answered one question and exposed a measurement in the
    // wrong place. What it settled: the KIQ queue does NOT predate startKIQ -
    // at post-gc-hw-init the slot reads ACTIVE=0 CTRL=0x00308509 (the same
    // empty value as me=1/*), and at post-startKIQ it reads ACTIVE=1
    // CTRL=0xc030860d with the doorbell enabled. startKIQ really does create
    // the queue, so the TTL layer below it works.
    //
    // What it did NOT settle: `pre-kiq-submit` is submitSetResourcesPacket's
    // ENTRY hook, which runs BEFORE the packet is written and before the
    // doorbell is rung. WPTR = RPTR = 0 there is expected and says nothing.
    //
    // waitForHwStamp is called from submitKIQFrame AFTER commitBlock and after
    // the doorbell (vtable[0x158]), so this is the first moment at which the
    // work really is submitted. WPTR here is the discriminator:
    //   WPTR still 0  -> the doorbell/WPTR write never reached the queue
    //   WPTR advanced -> work is queued and the CP is not consuming it
    rbgpu_dump_gc_regs("at-stamp-wait");

    // 2ds (2026-10-12): the channel's own stamp bookkeeping, which is what
    // waitForHwStamp actually polls - not the GPU.
    //
    //   waitForHwStamp(this, stamp)  (0xbe85536)
    //     if (stamp - this->0x84 <= 0) return true        already completed
    //     loop { pollWait(5 s); checkForTimestampUpdate(); ... }
    //     return (stamp - this->0x84) <= 0
    //
    //   checkForTimestampUpdate (0xbe851d6)
    //     if (this->0x80 == this->0x84) return 0;   <-- EARLY EXIT, reads nothing
    //     vtable[0x2d0](this); timestampUpdated(this)
    //
    //   timestampUpdated (0xbe85214)
    //     if (this->0xe0 == -1) return;             <-- no writeback configured
    //     ... reads through this->0xc0 / this->0xe0 and writes this->0x84
    //
    // So +0x80 is the SUBMITTED stamp and +0x84 the COMPLETED one. If +0x80
    // was never incremented, the refresh short-circuits and +0x84 can never
    // move, no matter what the command processor did - and 2dq proved the CP
    // did execute the packet. These are plain reads of the channel object.
    if (ch) {
        auto u32at = [&](uint32_t off) {
            return *reinterpret_cast<volatile uint32_t *>(
                reinterpret_cast<uintptr_t>(ch) + off);
        };
        auto u64at = [&](uint32_t off) {
            return *reinterpret_cast<volatile uint64_t *>(
                reinterpret_cast<uintptr_t>(ch) + off);
        };
        uint32_t submitted = u32at(0x80);
        uint32_t completed = u32at(0x84);
        uint64_t wb_c0     = u64at(0xc0);
        uint64_t wb_e0     = u64at(0xe0);
        // 2dt: the 5 s wait lets the request-0x14 poll flood evict ~500 lines,
        // so anything logged BEFORE the wait is gone by the time the log is
        // read. Keep the values and reprint them in the one line that survives.
        g_rb_2dt_wb_c0 = wb_c0;
        g_rb_2dt_wb_e0 = wb_e0;
        g_rb_2dt_sub_before = submitted;
        g_rb_2dt_cmp_before = completed;
        rbgpu_log("2ds: channel stamps BEFORE the wait: submitted(+0x80)=%u "
                  "completed(+0x84)=%u asked-for=%u ; writeback +0xc0=0x%llx +0xe0=0x%llx",
                  submitted, completed, stamp, wb_c0, wb_e0);
        rbgpu_diag_set_num("stamp_submitted", submitted);
        rbgpu_diag_set_num("stamp_completed", completed);
        rbgpu_diag_set_num("stamp_wb_c0", wb_c0);
        rbgpu_diag_set_num("stamp_wb_e0", wb_e0);
        if (submitted == completed) {
            rbgpu_log("2ds: +0x80 == +0x84, so checkForTimestampUpdate takes its EARLY "
                      "EXIT and never reads the fence at all. Nothing marked the frame "
                      "submitted, so the wait can only ever time out - this is "
                      "bookkeeping, not the GPU.");
        } else if (wb_e0 == 0xFFFFFFFFFFFFFFFFull) {
            rbgpu_log("2ds: +0xe0 is -1, so timestampUpdated returns without reading a "
                      "writeback location: no fence address is configured on this channel.");
        } else {
            rbgpu_log("2ds: a frame IS outstanding and a writeback address is set, so the "
                      "fence memory itself is what is not being updated.");
        }
    }

    uint64_t t0 = mach_absolute_time();
    bool ret;
    if (checkKernelArgument("-rbgpustampwait")) {
        // 2dr: this is now the expected path, not the daring one. The 2dq boot
        // read RPTR = WPTR = 0x20 at this very point with CP_STAT = 0x88000000,
        // so the CP fetched and executed the SET_RESOURCES packet. RPTR is
        // written by hardware, so that is the command processor moving, not the
        // driver. The stamp may well already be there; 2cy has simply never let
        // anyone wait for it.
        ret = orig_hwchannel_waitForHwStamp_2cx
                  ? orig_hwchannel_waitForHwStamp_2cx(ch, stamp)
                  : false;
        rbgpu_log("2dr: the REAL wait returned %d%s", ret ? 1 : 0,
                  ret ? " - THE STAMP ARRIVED; doStart continues past "
                        "submitSetResourcesPacket for the first time"
                      : " - the stamp did not arrive even though the CP consumed "
                        "the packet, so the fault is in the stamp path, not execution");
        if (ch) {
            uint32_t sub = *reinterpret_cast<volatile uint32_t *>(
                reinterpret_cast<uintptr_t>(ch) + 0x80);
            uint32_t cmp = *reinterpret_cast<volatile uint32_t *>(
                reinterpret_cast<uintptr_t>(ch) + 0x84);
            rbgpu_log("2ds: channel stamps AFTER the wait: submitted(+0x80)=%u "
                      "completed(+0x84)=%u (asked-for=%u) ; BEFORE sub=%u cmp=%u ; "
                      "writeback +0xc0=0x%llx +0xe0=0x%llx%s",
                      sub, cmp, stamp, g_rb_2dt_sub_before, g_rb_2dt_cmp_before,
                      g_rb_2dt_wb_c0, g_rb_2dt_wb_e0,
                      (g_rb_2dt_wb_e0 == 0xFFFFFFFFFFFFFFFFull)
                          ? "  <<< +0xe0 is -1: timestampUpdated returns WITHOUT reading "
                            "any writeback, so no fence address is configured"
                          : "  <<< a writeback address IS set and a frame IS outstanding, "
                            "so the fence memory itself is not being written");
        }
        rbgpu_dump_gc_regs("post-stamp-wait");
    } else {
        rbgpu_log("2cy: skipping the wait (-rbgpustampwait not set) -> false, so the "
                  "boot fails fast instead of polling forever");
        ret = false;
    }
    uint64_t dt = mach_absolute_time() - t0;

    g_rb_2cx_stamp_ret = ret ? 1 : 0;
    g_rb_2cx_stamp_us = dt;
    rbgpu_diag_set_num("stamp_ret", static_cast<uint64_t>(g_rb_2cx_stamp_ret));
    rbgpu_diag_set_num("stamp_ticks", dt);
    rbgpu_log("2cx: waitForHwStamp -> %d %s (%llu ticks)", ret ? 1 : 0,
              ret ? "(the GPU executed it)"
                  : "(the stamp was not reached - NOT evidence the CP is dead; 2dq "
                    "measured RPTR=WPTR with CP_STAT busy, so it does execute)",
              dt);
    return ret;
}

// 2cq: the remaining doStart gates after startKIQ, observe-only. Nothing here
// overrides a return value; the point is to name the exact exit.
// docs/SPEC-PM4-DOSTART.md section 2 (the table) and section 6.
static bool (*orig_kiq_submitSetResources_2cq)(void *, const void *) = nullptr;
static bool (*orig_pm4_initGraphicsMQD_2cq)(void *) = nullptr;
static bool (*orig_kiq_submitMapQueues_2cq)(void *, uint32_t, uint32_t, uint64_t,
                                            const void *, uint64_t) = nullptr;
static volatile int g_rb_2cq_setres_calls = 0;
static volatile int g_rb_2cq_setres_ret = -1;
static volatile int g_rb_2cq_gfxmqd_calls = 0;
static volatile int g_rb_2cq_gfxmqd_ret = -1;
static volatile int g_rb_2cq_mapq_calls = 0;
static volatile int g_rb_2cq_mapq_fails = 0;

static bool hooked_kiq_submitSetResources_2cq(void *ch, const void *spec) {
    g_rb_2cq_setres_calls++;
    // 2cx: log on ENTRY. The 2026-10-12 armed boot stopped here with no line
    // at all, because every hook logged only after the original returned - a
    // hang therefore produced nothing. submitSetResourcesPacket tail-calls
    // submitKIQFrame -> waitForHwStamp, which waits for the GPU to execute
    // the packet, so "entered with no return" is the signal that matters.
    rbgpu_log("2cx: submitSetResourcesPacket ENTER (ch=%p spec=%p) - waits for the "
              "GPU to execute the KIQ packet", ch, spec);

    // 2dk: the CP's own state at the one moment it matters. Everything before
    // this point reports success - PSP, GC and TTL hw_init all return success,
    // the yellow_carp firmware is loaded, RLC_STAT is the reference 0x25, the
    // SMU is up and says GFX is not in GFXOFF - and yet the stamp never
    // arrives. Read-only, and the only reading taken while the packet is
    // actually in flight.
    rbgpu_dump_gc_regs("pre-kiq-submit");

    bool ret = orig_kiq_submitSetResources_2cq
                   ? orig_kiq_submitSetResources_2cq(ch, spec)
                   : false;
    g_rb_2cq_setres_ret = ret ? 1 : 0;
    rbgpu_diag_set_num("setres_calls", static_cast<uint64_t>(g_rb_2cq_setres_calls));
    rbgpu_diag_set_num("setres_ret", static_cast<uint64_t>(g_rb_2cq_setres_ret));
    rbgpu_log("2cq: submitSetResourcesPacket(spec=%p) -> %d %s", spec, ret ? 1 : 0,
              ret ? "(OK)" : "(FAIL -> doStart exits via vtable[0x150], returns false)");
    return ret;
}

static bool hooked_pm4_initGraphicsMQD_2cq(void *pm4) {
    g_rb_2cq_gfxmqd_calls++;
    rbgpu_log("2cx: initGraphicsMQD ENTER (pm4=%p)", pm4);
    bool ret = orig_pm4_initGraphicsMQD_2cq ? orig_pm4_initGraphicsMQD_2cq(pm4) : false;
    g_rb_2cq_gfxmqd_ret = ret ? 1 : 0;
    rbgpu_diag_set_num("gfxmqd_calls", static_cast<uint64_t>(g_rb_2cq_gfxmqd_calls));
    rbgpu_diag_set_num("gfxmqd_ret", static_cast<uint64_t>(g_rb_2cq_gfxmqd_ret));
    rbgpu_log("2cq: initGraphicsMQD -> %d %s", ret ? 1 : 0,
              ret ? "(OK)" : "(FAIL -> doStart returns false)");
    return ret;
}

static bool hooked_kiq_submitMapQueues_2cq(void *ch, uint32_t ring, uint32_t a, uint64_t b,
                                           const void *spec, uint64_t c) {
    g_rb_2cq_mapq_calls++;
    rbgpu_log("2cx: submitMapQueuesPacket ENTER (ring=%u)", ring);
    bool ret = orig_kiq_submitMapQueues_2cq
                   ? orig_kiq_submitMapQueues_2cq(ch, ring, a, b, spec, c)
                   : false;
    if (!ret) {
        g_rb_2cq_mapq_fails++;
    }
    rbgpu_diag_set_num("mapq_calls", static_cast<uint64_t>(g_rb_2cq_mapq_calls));
    rbgpu_diag_set_num("mapq_fails", static_cast<uint64_t>(g_rb_2cq_mapq_fails));
    rbgpu_log("2cq: submitMapQueuesPacket(ring=%u, a=0x%x, b=0x%llx, spec=%p, c=0x%llx) -> %d %s",
              ring, a, b, spec, c, ret ? 1 : 0, ret ? "(OK)" : "(FAIL)");
    return ret;
}

static bool (*orig_pm4_initComputeMQD_2cd)(void *, uint32_t) = nullptr;

static bool hooked_pm4_initComputeMQD_2cd(void *pm4, uint32_t ring_type) {
    rbgpu_log("2cd: initComputeMQD entered (pm4=%p ring_type=%u)", pm4, ring_type);
    g_rb_2cf_initmqd_called = 1;
    if (!pm4) {
        rbgpu_log("2cd: pm4 is NULL -> false");
        g_rb_2cf_initmqd_result = 0;
        return false;
    }
    // rdi = this+0x50[ring_type], cast to AMDRTRing (we skip the cast, just log)
    void *ring_obj = *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(pm4) + 0x50 + ring_type * 8);
    rbgpu_log("2cd: ring_obj (pm4+0x50[%u]) = %p", ring_type, ring_obj);
    // r14 = (this+0x18)->vtable[0x2d8]()
    void *mem_obj = *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(pm4) + 0x18);
    rbgpu_log("2cd: mem_obj (pm4+0x18) = %p", mem_obj);
    void *r14 = nullptr;
    if (mem_obj) {
        void **mem_vtable = *reinterpret_cast<void ***>(mem_obj);
        if (mem_vtable) {
            auto fn_2d8 = reinterpret_cast<void *(*)(void *)>(mem_vtable[0x2d8 / 8]);
            rbgpu_log("2cd: calling mem->vtable[0x2d8] (%p)...", reinterpret_cast<void *>(fn_2d8));
            r14 = fn_2d8 ? fn_2d8(mem_obj) : nullptr;
            rbgpu_log("2cd: mem->vtable[0x2d8]() = %p %s", r14, r14 ? "(OK)" : "(NULL -> FAIL1)");
        } else {
            rbgpu_log("2cd: mem_obj has NULL vtable -> FAIL1");
        }
    } else {
        rbgpu_log("2cd: mem_obj is NULL -> FAIL1");
    }
    if (!r14) {
        rbgpu_log("2cd: FAIL1 - r14 is NULL");
        g_rb_2cf_initmqd_result = 0;
        return false;
    }
    // r14->vtable[0x148]()
    {
        void **r14_vtable = *reinterpret_cast<void ***>(r14);
        auto fn_148 = r14_vtable ? reinterpret_cast<bool (*)(void *)>(r14_vtable[0x148 / 8]) : nullptr;
        rbgpu_log("2cd: calling r14->vtable[0x148] (%p)...", reinterpret_cast<void *>(fn_148));
        bool ok = fn_148 ? fn_148(r14) : false;
        rbgpu_log("2cd: r14->vtable[0x148]() -> %d %s", ok ? 1 : 0, ok ? "(OK)" : "(FAIL2)");
        if (!ok) {
            rbgpu_log("2cd: FAIL2 - vtable[0x148] returned false");
            g_rb_2cf_initmqd_result = 0;
        return false;
        }
    }
    // r12 = this+0x250[ring_type*32]
    void *r12 = *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(pm4) + 0x250 + ring_type * 32);
    rbgpu_log("2cd: r12 (pm4+0x250[%u*32]) = %p %s", ring_type, r12, r12 ? "(OK)" : "(NULL -> FAIL3)");
    if (!r12) {
        rbgpu_log("2cd: FAIL3 - r12 is NULL");
        g_rb_2cf_initmqd_result = 0;
        return false;
    }
    // r14->vtable[0x210](r12, 0x1000, &local, 0x100)
    void *local = nullptr;
    {
        void **r14_vtable = *reinterpret_cast<void ***>(r14);
        auto fn_210 = r14_vtable ? reinterpret_cast<bool (*)(void *, void *, uint32_t, void **, uint32_t)>(r14_vtable[0x210 / 8]) : nullptr;
        rbgpu_log("2cd: calling r14->vtable[0x210] (%p)...", reinterpret_cast<void *>(fn_210));
        bool ok = fn_210 ? fn_210(r14, r12, 0x1000, &local, 0x100) : false;
        rbgpu_log("2cd: r14->vtable[0x210]() -> %d local=%p %s", ok ? 1 : 0, local, ok ? "(OK)" : "(FAIL4)");
        if (!ok) {
            rbgpu_log("2cd: FAIL4 - vtable[0x210] returned false");
            g_rb_2cf_initmqd_result = 0;
        return false;
        }
    }
    // rax = local->vtable[0x118]()
    void *mqd = nullptr;
    if (local) {
        void **local_vtable = *reinterpret_cast<void ***>(local);
        auto fn_118 = local_vtable ? reinterpret_cast<void *(*)(void *)>(local_vtable[0x118 / 8]) : nullptr;
        rbgpu_log("2cd: calling local->vtable[0x118] (%p)...", reinterpret_cast<void *>(fn_118));
        mqd = fn_118 ? fn_118(local) : nullptr;
        rbgpu_log("2cd: local->vtable[0x118]() = %p %s", mqd, mqd ? "(OK)" : "(NULL -> FAIL5)");
    } else {
        rbgpu_log("2cd: local is NULL -> FAIL5");
    }
    if (!mqd) {
        rbgpu_log("2cd: FAIL5 - mqd is NULL");
        g_rb_2cf_initmqd_result = 0;
        return false;
    }
    rbgpu_log("2cd: all checks passed, calling ORIGINAL initComputeMQD (2cg)");
    g_rb_2cf_initmqd_result = 1;
    // 2cg: Call the original to do the real work. If doStart still fails,
    // the failure is in doStart's later steps (vtable[0x130], startKIQ),
    // not in initComputeMQD.
    bool orig_ret = orig_pm4_initComputeMQD_2cd ? orig_pm4_initComputeMQD_2cd(pm4, ring_type) : false;
    rbgpu_log("2cg: original initComputeMQD returned %d", orig_ret ? 1 : 0);
    g_rb_2cf_initmqd_result = orig_ret ? 1 : 0;
    return orig_ret;
}

// 2cc: Stub AMDVCN2EncChannel::allocateMemoryResources to return true.
// The VCN 3.1 firmware (ativvaxy_vcn3_1.dat) is missing for 0x1681, so the VCN
// encode channels fail their allocation. VCN is video encode/decode, not needed
// for display/Metal bring-up. This lets the rest of the driver initialize.
static bool hooked_vcn2enc_alloc_2cc(void *ch) {
    rbgpu_log("2cc: AMDVCN2EncChannel::allocateMemoryResources(%p) stubbed -> true (VCN fw missing)", ch);
    return true;
}
static bool (*orig_vcn2enc_alloc_2cc)(void *) = nullptr;

static bool hooked_hw_allocateMemoryResources_2cb(void *hw) {
    rbgpu_log("2cb: AMDHardware::allocateMemoryResources entered (hw=%p)", hw);
    if (!hw) {
        rbgpu_log("2cb: hw is NULL -> false");
        return false;
    }
    void *obj = *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(hw) + 0x18);
    uint64_t arg_rsi = *reinterpret_cast<uint64_t *>(reinterpret_cast<uintptr_t>(hw) + 0x510);
    uint8_t b530 = *reinterpret_cast<uint8_t *>(reinterpret_cast<uintptr_t>(hw) + 0x530);
    uint64_t arg_rdx = (static_cast<uint64_t>(b530) << 12) + 0x1000;
    rbgpu_log("2cb: step1 obj=%p rsi=0x%llx rdx=0x%llx", obj, arg_rsi, arg_rdx);
    void *mem_result = nullptr;
    if (obj) {
        void **obj_vtable = *reinterpret_cast<void ***>(obj);
        if (obj_vtable) {
            auto alloc_fn = reinterpret_cast<void *(*)(void *, uint64_t, uint64_t, uint32_t, uint32_t, uint32_t)>(obj_vtable[0x208 / 8]);
            rbgpu_log("2cb: step1 alloc_fn=%p", reinterpret_cast<void *>(alloc_fn));
            if (alloc_fn) {
                mem_result = alloc_fn(obj, arg_rsi, arg_rdx, 3, 0, 3);
            } else {
                rbgpu_log("2cb: step1 alloc_fn is NULL!");
            }
        } else {
            rbgpu_log("2cb: step1 obj has NULL vtable!");
        }
    } else {
        rbgpu_log("2cb: step1 obj (hw+0x18) is NULL!");
    }
    *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(hw) + 0x518) = mem_result;
    rbgpu_log("2cb: step1 result=%p %s", mem_result, mem_result ? "(OK)" : "(NULL -> FAIL)");
    if (!mem_result) {
        g_rb_alloc_fail_step = 1;
        return false;
    }
    for (int i = 0; i < 11; i++) {
        void *eng = *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(hw) + 0x3b0 + i * 8);
        if (!eng) {
            rbgpu_log("2cb: engine[%d] = NULL (skipped)", i);
            continue;
        }
        void **vtable = *reinterpret_cast<void ***>(eng);
        if (!vtable) {
            rbgpu_log("2cb: engine[%d] = %p has NULL vtable -> FAIL", i, eng);
            g_rb_alloc_fail_step = 2;
            g_rb_alloc_fail_engine = i;
            g_rb_alloc_fail_engine_ptr = eng;
            return false;
        }
        auto engAlloc = reinterpret_cast<bool (*)(void *)>(vtable[0x120 / 8]);
        // 2cw: name the engine. The 2026-10-12 armed boot HUNG inside
        // engine[4]'s allocateMemoryResources (engines 0-3 returned 1), and
        // the index alone does not say which IP that is. Every engine is an
        // OSObject, so its metaclass gives the class name directly.
        const char *engName = "?";
        if (auto obj = OSDynamicCast(OSObject, reinterpret_cast<OSObject *>(eng))) {
            if (auto meta = obj->getMetaClass()) {
                engName = meta->getClassName();
            }
        }
        rbgpu_log("2cb: engine[%d] = %p (%s) calling allocateMemoryResources...",
                  i, eng, engName);
        bool eng_ret = engAlloc ? engAlloc(eng) : false;
        rbgpu_log("2cb: engine[%d] allocateMemoryResources -> %d %s", i, eng_ret ? 1 : 0, eng_ret ? "(OK)" : "(FAIL)");
        if (!eng_ret) {
            g_rb_alloc_fail_step = 2;
            g_rb_alloc_fail_engine = i;
            g_rb_alloc_fail_engine_ptr = eng;
            // 2cb: Drill into the failing engine's channels.
            // AMDHWEngine::allocateMemoryResources loops count=eng+0x48 channels
            // at eng+0xc0[j], calling each channel->vtable[0x2c0]().
            uint32_t ch_count = *reinterpret_cast<uint32_t *>(reinterpret_cast<uintptr_t>(eng) + 0x48);
            rbgpu_log("2cb: engine[%d] FAILED - drilling into %u channels", i, ch_count);
            for (uint32_t j = 0; j < ch_count; j++) {
                void *ch = *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(eng) + 0xc0 + j * 8);
                if (!ch) {
                    rbgpu_log("2cb:   channel[%u] = NULL (skipped)", j);
                    continue;
                }
                void **ch_vtable = *reinterpret_cast<void ***>(ch);
                if (!ch_vtable) {
                    rbgpu_log("2cb:   channel[%u] = %p has NULL vtable", j, ch);
                    continue;
                }
                auto chAlloc = reinterpret_cast<bool (*)(void *)>(ch_vtable[0x2c0 / 8]);
                rbgpu_log("2cb:   channel[%u] = %p (fn=%p) calling vtable[0x2c0]...", j, ch,
                          reinterpret_cast<void *>(ch_vtable[0x2c0 / 8]));
                bool ch_ret = chAlloc ? chAlloc(ch) : false;
                rbgpu_log("2cb:   channel[%u] vtable[0x2c0] -> %d %s", j, ch_ret ? 1 : 0,
                          ch_ret ? "(OK)" : "(FAIL)");
            }
            return false;
        }
    }
    rbgpu_log("2cb: all steps passed -> true");
    g_rb_alloc_fail_step = 0;
    return true;
}

// 2bn: Hook AMDHWMemory::reserve to diagnose the display_mode_did_change failure.
// Per the assistant's analysis, set_display_mode_and_vram calls mem->reserve() for each
// display pipe, and it fails, causing the panic. Log the parameters to see why.
static bool (*orig_amdhw_reserve)(void *this_ptr, void *elem, uint64_t offset, uint64_t size, bool flag, int originator) = nullptr;
static int reserve_log_count = 0;

// 2br: powerUpHW progress (the accelerator's VRAM pool is filled there).
// Bits: 1 powerUpHW entered, 2 powerUpHW returned, 4 Navi21Hardware::powerUp
// entered, 8 it returned true, 0x10 enableAllocations entered, 0x20 it
// returned true. 2bs adds: 0x40 powerUpHWEngines entered, 0x80 it returned
// true, 0x100 startHWEngines entered, 0x200 it returned true.
// 2bt adds: 0x400 AMDHardware::powerUp entered, 0x800 it returned true,
// 0x1000 AMDGFX10Hardware::powerUp entered, 0x2000 it returned true.
// Read by the reserve panic.
static volatile uint32_t g_rb_pw_stage = 0;
static volatile uint32_t g_rb_pw_ret = 0xFFFFFFFF;
static uint32_t (*orig_accel_powerUpHW)(void *) = nullptr;
// 2cn: Survive mode - flag set when powerUpHW fails, used to fail start() cleanly.
static volatile int g_rb_2cn_poweruphw_failed = 0;
static bool (*orig_navi21_powerUp)(void *) = nullptr;
static bool (*orig_hwmem_enableAllocations)(void *) = nullptr;

// 2cn: Hook AMDGraphicsAccelerator::start to fail cleanly when powerUpHW failed.
static bool (*orig_accel_start_2cn)(void *, void *) = nullptr;
static bool hooked_accel_start_2cn(void *accel, void *provider) {
    rbgpu_log("2cn: accelerator start entered (accel=%p)", accel);

    // 2cr (2026-10-12): the flag check used to be HERE, before the original
    // call. powerUpHW is called from inside start(), so the flag is always
    // still 0 at this point and the guard only ever applied to a second
    // start() that never comes. That is why 2cn was never observed to do
    // anything. Check it after the original returns instead.
    bool ret = orig_accel_start_2cn ? orig_accel_start_2cn(accel, provider) : false;
    rbgpu_log("2cn: accelerator start returned %d", ret ? 1 : 0);

    if (g_rb_2cn_poweruphw_failed && ret) {
        // powerUpHW failed during start() yet start() reported success. Refuse
        // the match so IOKit detaches the accelerator and the screen stays
        // with IONDRVFramebuffer, instead of leaving a half-started
        // accelerator registered with IOAcceleratorFamily2.
        rbgpu_log("2cr: powerUpHW failed during start() -> returning false so the "
                  "accelerator detaches (survive mode)");
        return false;
    }
    return ret;
}

static uint32_t hooked_accel_powerUpHW(void *accel) {
    g_rb_pw_stage |= 1;
    rbgpu_log("2br: powerUpHW entered (accel=%p)", accel);
    uint32_t ret = orig_accel_powerUpHW ? orig_accel_powerUpHW(accel) : 0;
    g_rb_pw_ret = ret;
    g_rb_pw_stage |= 2;
    rbgpu_log("2br: powerUpHW returned 0x%x", ret);
    if (ret == 0) {
        rbgpu_log("2cn: powerUpHW failed, will fail start() cleanly (survive mode)");
        g_rb_2cn_poweruphw_failed = 1;
    }
    return ret;
}

static bool hooked_navi21_powerUp(void *hw) {
    g_rb_pw_stage |= 4;
    bool ret = orig_navi21_powerUp ? orig_navi21_powerUp(hw) : false;
    if (ret) g_rb_pw_stage |= 8;
    rbgpu_log("2br: Navi21Hardware::powerUp -> %d", ret ? 1 : 0);
    return ret;
}

static bool hooked_hwmem_enableAllocations(void *mem) {
    g_rb_pw_stage |= 0x10;
    bool ret = orig_hwmem_enableAllocations ? orig_hwmem_enableAllocations(mem) : false;
    if (ret) g_rb_pw_stage |= 0x20;
    rbgpu_log("2br: AMDHWMemory::enableAllocations -> %d", ret ? 1 : 0);
    return ret;
}

// 2bs: Drill into AMDHardware::powerUp to find which engine fails.
// Per the assistant's handoff, Navi21Hardware::powerUp returns false because
// powerUpHWEngines() or startHWEngines() fails. Hook both, on both the
// base AMDHardware and the GFX10 variant, to see which returns false.
// New pw_stage bits: 0x40 powerUpHWEngines entered, 0x80 it returned true,
// 0x100 startHWEngines entered, 0x200 it returned true.
static bool (*orig_hw_powerUpHWEngines)(void *) = nullptr;
static bool (*orig_hw_startHWEngines)(void *) = nullptr;
static bool (*orig_gfx10_powerUpHWEngines)(void *) = nullptr;
static bool (*orig_gfx10_startHWEngines)(void *) = nullptr;

// 2bw: Global to carry the failing engine info from powerUpHWEngines
// to the reserve panic (log lines are lost in the crash).
static volatile int g_rb_fail_engine_idx = -2;  // -2 = not run, -1 = all OK
static volatile void *g_rb_fail_engine_ptr = nullptr;
// 2bx: Additional engine 0 diagnostics for single-boot max info.
static volatile void *g_rb_fail_engine_vtable = nullptr;
static volatile void *g_rb_fail_engine_powerup_fn = nullptr;
static volatile uint64_t g_rb_fail_engine_d0 = 0;
static volatile uint64_t g_rb_fail_engine_d8 = 0;
static volatile uint64_t g_rb_fail_engine_d10 = 0;
static volatile uint64_t g_rb_fail_engine_d18 = 0;
// 2by: Capture the vtable[0x148] target (the real powerUp implementation
// that AMDGFX10PM4Engine::powerUp tail-calls to).
static volatile void *g_rb_fail_engine_real_pu = nullptr;
static bool hooked_hw_powerUpHWEngines(void *hw) {
    g_rb_pw_stage |= 0x40;
    rbgpu_log("2bs: AMDHardware::powerUpHWEngines entered (hw=%p)", hw);
    // 2bv: Replace the original with a fully instrumented manual loop.
    // The original iterates 11 engines at hw+0x3b0, calling each engine's
    // vtable[0x138]() (powerUp), returning false on first failure.
    // We do the same but log each engine's index, pointer, and result,
    // so the panic/log shows EXACTLY which engine fails.
    // We do NOT call the original - this replaces it for diagnosis.
    bool all_ok = true;
    int fail_idx = -1;
    void *fail_ptr = nullptr;
    if (hw) {
        for (int i = 0; i < 11; i++) {
            void *eng = *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(hw) + 0x3b0 + i * 8);
            if (!eng) {
                rbgpu_log("2bv: engine[%d] = NULL (skipped)", i);
                continue;
            }
            void **vtable = *reinterpret_cast<void ***>(eng);
            if (!vtable) {
                rbgpu_log("2bv: engine[%d] = %p has NULL vtable!", i, eng);
                all_ok = false;
                fail_idx = i;
                fail_ptr = eng;
                break;
            }
            auto engPowerUp = reinterpret_cast<bool (*)(void *)>(vtable[0x138 / 8]);
            rbgpu_log("2bv: engine[%d] = %p (vtable=%p) calling powerUp...", i, eng, vtable);
            bool ret = engPowerUp ? engPowerUp(eng) : false;
            rbgpu_log("2bv: engine[%d] powerUp -> %d", i, ret ? 1 : 0);
            if (!ret) {
                rbgpu_log("2bv: *** ENGINE %d FAILED ***", i);
                all_ok = false;
                fail_idx = i;
                fail_ptr = eng;
                // 2bx: Capture max diagnostics for the panic.
                g_rb_fail_engine_vtable = vtable;
                g_rb_fail_engine_powerup_fn = reinterpret_cast<void *>(vtable[0x138 / 8]);
                // 2by: Capture vtable[0x148] - the real target that PM4Engine::powerUp tail-calls.
                g_rb_fail_engine_real_pu = reinterpret_cast<void *>(vtable[0x148 / 8]);
                // First 4 qwords of the engine object (state clues).
                g_rb_fail_engine_d0 = *reinterpret_cast<uint64_t *>(eng);
                g_rb_fail_engine_d8 = *reinterpret_cast<uint64_t *>(reinterpret_cast<uintptr_t>(eng) + 8);
                g_rb_fail_engine_d10 = *reinterpret_cast<uint64_t *>(reinterpret_cast<uintptr_t>(eng) + 0x10);
                g_rb_fail_engine_d18 = *reinterpret_cast<uint64_t *>(reinterpret_cast<uintptr_t>(eng) + 0x18);
                break;
            }
        }
    } else {
        rbgpu_log("2bv: hw is NULL!");
        all_ok = false;
    }
    // 2bw: Stash the result for the reserve panic.
    g_rb_fail_engine_idx = all_ok ? -1 : fail_idx;
    g_rb_fail_engine_ptr = fail_ptr;
    if (all_ok) {
        g_rb_pw_stage |= 0x80;
        rbgpu_log("2bv: all engines powered up OK");
    } else {
        rbgpu_log("2bv: powerUpHWEngines returning false (fail_idx=%d)", fail_idx);
    }
    // Update pw_stage bit 0x80 on success (matches original semantics)
    return all_ok;
}

static bool hooked_hw_startHWEngines(void *hw) {
    g_rb_pw_stage |= 0x100;
    rbgpu_log("2bs: AMDHardware::startHWEngines entered (hw=%p)", hw);
    bool ret = orig_hw_startHWEngines ? orig_hw_startHWEngines(hw) : false;
    if (ret) g_rb_pw_stage |= 0x200;
    rbgpu_log("2bs: AMDHardware::startHWEngines -> %d", ret ? 1 : 0);
    return ret;
}

static bool hooked_gfx10_powerUpHWEngines(void *hw) {
    g_rb_pw_stage |= 0x40;
    rbgpu_log("2bs: AMDGFX10Hardware::powerUpHWEngines entered (hw=%p)", hw);
    bool ret = orig_gfx10_powerUpHWEngines ? orig_gfx10_powerUpHWEngines(hw) : false;
    if (ret) g_rb_pw_stage |= 0x80;
    rbgpu_log("2bs: AMDGFX10Hardware::powerUpHWEngines -> %d", ret ? 1 : 0);
    return ret;
}

static bool hooked_gfx10_startHWEngines(void *hw) {
    g_rb_pw_stage |= 0x100;
    rbgpu_log("2bs: AMDGFX10Hardware::startHWEngines entered (hw=%p)", hw);
    bool ret = orig_gfx10_startHWEngines ? orig_gfx10_startHWEngines(hw) : false;
    if (ret) g_rb_pw_stage |= 0x200;
    rbgpu_log("2bs: AMDGFX10Hardware::startHWEngines -> %d", ret ? 1 : 0);
    return ret;
}

// 2bt: Also hook AMDHardware::powerUp and AMDGFX10Hardware::powerUp to trace
// the call chain from Navi21Hardware::powerUp down. New pw bits:
// 0x400 AMDHardware::powerUp entered, 0x800 it returned true,
// 0x1000 AMDGFX10Hardware::powerUp entered, 0x2000 it returned true.
static bool (*orig_hw_powerUp)(void *) = nullptr;
static bool (*orig_gfx10_powerUp)(void *) = nullptr;

static bool hooked_hw_powerUp(void *hw) {
    g_rb_pw_stage |= 0x400;
    rbgpu_log("2bt: AMDHardware::powerUp entered (hw=%p)", hw);
    bool ret = orig_hw_powerUp ? orig_hw_powerUp(hw) : false;
    if (ret) g_rb_pw_stage |= 0x800;
    rbgpu_log("2bt: AMDHardware::powerUp -> %d", ret ? 1 : 0);
    return ret;
}

static bool hooked_gfx10_powerUp(void *hw) {
    g_rb_pw_stage |= 0x1000;
    rbgpu_log("2bt: AMDGFX10Hardware::powerUp entered (hw=%p)", hw);
    bool ret = orig_gfx10_powerUp ? orig_gfx10_powerUp(hw) : false;
    if (ret) g_rb_pw_stage |= 0x2000;
    rbgpu_log("2bt: AMDGFX10Hardware::powerUp -> %d", ret ? 1 : 0);
    return ret;
}

// 2bu: Hook AMDGFX10PM4Engine::powerUp - the only engine with its own powerUp
// symbol. If this returns false, we've found our culprit.
static bool (*orig_pm4_powerUp)(void *) = nullptr;

static bool hooked_pm4_powerUp(void *eng) {
    rbgpu_log("2bu: AMDGFX10PM4Engine::powerUp entered (eng=%p)", eng);
    // 2ce (merged): log the vtable slot only. Calling it would re-enter this
    // hook, and a second route on powerUp panics Lilu (short jump multiroute).
    // 2cf: stash in globals for the panic line.
    if (eng) {
        void **vtable = *reinterpret_cast<void ***>(eng);
        rbgpu_log("2ce: powerUp vtable[0x148] target = %p", vtable ? vtable[0x148 / 8] : nullptr);
        g_rb_2cf_powerup_called = 1;
    }
    bool ret = orig_pm4_powerUp ? orig_pm4_powerUp(eng) : false;
    rbgpu_log("2bu: AMDGFX10PM4Engine::powerUp -> %d", ret ? 1 : 0);
    return ret;
}

// 2bp: successful reservations seen so far, so a failure can name what
// already holds its range (reserveNDRVSpace's boot-framebuffer slot uses
// originator 0 at the start of VRAM).
struct RbReserveRec { void *mem; void *elem; uint64_t off; uint64_t size; int orig; };
static RbReserveRec g_reserve_recs[8];
static int g_reserve_rec_count = 0;

static bool hooked_AMDHWMemory_reserve(void *this_ptr, void *elem, uint64_t offset, uint64_t size, bool flag, int originator) {
    // Log this+0x50 (base) and this+0x68 (allocator)
    uint64_t base = this_ptr ? *reinterpret_cast<uint64_t *>(reinterpret_cast<uintptr_t>(this_ptr) + 0x50) : 0;
    uint64_t allocator = this_ptr ? *reinterpret_cast<uint64_t *>(reinterpret_cast<uintptr_t>(this_ptr) + 0x68) : 0;

    bool ret = false;
    if (orig_amdhw_reserve) {
        ret = orig_amdhw_reserve(this_ptr, elem, offset, size, flag, originator);
    }

    // Log first few calls (both successes and failures) to see what ranges are accepted
    if (reserve_log_count < 10) {
        rbgpu_log("2bn reserve: this=%p elem=%p offset=0x%llx size=0x%llx flag=%d orig=%d -> %d (base=0x%llx alloc=%p)",
                  this_ptr, elem, offset, size, flag ? 1 : 0, originator, ret ? 1 : 0, base, reinterpret_cast<void *>(allocator));
        reserve_log_count++;
    } else if (!ret) {
        // Always log failures
        rbgpu_log("2bn reserve FAILED: this=%p offset=0x%llx size=0x%llx (base=0x%llx alloc=%p)",
                  this_ptr, offset, size, base, reinterpret_cast<void *>(allocator));
    }

    if (ret && g_reserve_rec_count < 8) {
        g_reserve_recs[g_reserve_rec_count++] = { this_ptr, elem, offset, size, originator };
    }

    // 2bp: set_display_mode_and_vram (originator 0x1e) reserves the boot
    // framebuffer at the start of VRAM for the display pipe, after freeing only
    // the second reserveNDRVSpace slot (+0x1ee0). The first slot (+0x1ed0,
    // originator 0, same start) still holds the range, so the allocator
    // refuses. Release that slot through the same AMDHWMemory and retry once.
    if (!ret && originator == 0x1e && g_psp_hw_mode && this_ptr && orig_amdhw_reserve) {
        for (int i = 0; i < g_reserve_rec_count; i++) {
            RbReserveRec &r = g_reserve_recs[i];
            if (r.mem != this_ptr || r.orig != 0 || r.off != offset || !r.elem ||
                *reinterpret_cast<void **>(r.elem) == nullptr) {
                continue;
            }
            auto vt = *reinterpret_cast<void ***>(this_ptr);
            auto safeFree = reinterpret_cast<void (*)(void *, void *)>(vt[0x1a0 / 8]);
            rbgpu_log("2bp: releasing NDRV reservation elem=%p off=0x%llx size=0x%llx for the display pipe", r.elem, r.off, r.size);
            safeFree(this_ptr, r.elem);
            r.elem = nullptr;
            ret = orig_amdhw_reserve(this_ptr, elem, offset, size, flag, originator);
            rbgpu_log("2bp: display pipe reserve retry -> %d", ret ? 1 : 0);
            rbgpu_diag_set_num("ndrv_release_retry", ret ? 1 : 0);
            break;
        }
    }

    // 2bo: still failing: panic with the numbers (the RBGPULog is lost in the
    // display_mode_did_change panic that would follow) and the first
    // successful reservations, to show what holds the range.
    if (!ret && originator == 0x1e) {
        uint64_t rr[4][3] = {};
        for (int i = 0; i < 4 && i < g_reserve_rec_count; i++) {
            rr[i][0] = g_reserve_recs[i].off;
            rr[i][1] = g_reserve_recs[i].size;
            rr[i][2] = static_cast<uint64_t>(g_reserve_recs[i].orig);
        }
        // 2cm SURVIVE MODE: Log instead of panic. Let macOS reach desktop.
        rbgpu_log("2cm SURVIVE: reserve fail: peer stage=0x%x r06=0x%x r1B=0x%x pw=0x%x pwret=0x%x engfail=%d engptr=%p | 2cf: pu=%d ds=%d mqd=%d mqdres=%d | 2ch: kiq=%d kiqres=%d (0 = SUCCESS) | 2cq: setres=%d/%d gfxmqd=%d/%d mapq=%d fail=%d | 2cp: wfav=%d forced=%d ret=%d | off=0x%llx size=0x%llx",
              g_rb_peer_stage, g_rb_peer_ret06, g_rb_peer_ret1B, g_rb_pw_stage, g_rb_pw_ret,
              g_rb_fail_engine_idx, g_rb_fail_engine_ptr,
              g_rb_2cf_powerup_called, g_rb_2cf_dostart_called, g_rb_2cf_initmqd_called, g_rb_2cf_initmqd_result,
              g_rb_2ch_startkiq_called, g_rb_2ch_startkiq_result,
              g_rb_2cq_setres_calls, g_rb_2cq_setres_ret,
              g_rb_2cq_gfxmqd_calls, g_rb_2cq_gfxmqd_ret,
              g_rb_2cq_mapq_calls, g_rb_2cq_mapq_fails,
              g_rb_2cp_wfav_calls, g_rb_2cp_wfav_bounded, g_rb_2cp_wfav_ret,
              offset, size);
    }

    return ret;
}

static int hooked_AmdTtlServices_initialize_input(void *this_ptr, void *input) {
    rbgpu_diag_inc("ttl_init_inp_calls");
    rbgpu_diag_set_num("ttl_init_inp_this", reinterpret_cast<uint64_t>(this_ptr));
    auto inp = reinterpret_cast<uint64_t *>(input);

    uint64_t f560 = this_ptr ? *reinterpret_cast<uint64_t *>(reinterpret_cast<uintptr_t>(this_ptr) + 0x560) : 0;
    uint64_t f568 = this_ptr ? *reinterpret_cast<uint64_t *>(reinterpret_cast<uintptr_t>(this_ptr) + 0x568) : 0;
    uint64_t f570 = this_ptr ? *reinterpret_cast<uint64_t *>(reinterpret_cast<uintptr_t>(this_ptr) + 0x570) : 0;
    uint32_t f578 = this_ptr ? *reinterpret_cast<uint32_t *>(reinterpret_cast<uintptr_t>(this_ptr) + 0x578) : 0;
    uint8_t  f598 = this_ptr ? *reinterpret_cast<uint8_t *>(reinterpret_cast<uintptr_t>(this_ptr) + 0x598) : 0;
    uint64_t f958 = this_ptr ? *reinterpret_cast<uint64_t *>(reinterpret_cast<uintptr_t>(this_ptr) + 0x958) : 0;

    rbgpu_diag_set_num("ttl_inp_0", inp ? inp[0] : 0);
    rbgpu_diag_set_num("ttl_inp_1", inp ? inp[1] : 0);
    rbgpu_diag_set_num("ttl_inp_2", inp ? inp[2] : 0);
    rbgpu_diag_set_num("ttl_inp_4", inp ? inp[4] : 0);
    rbgpu_diag_set_num("ttl_inp_5", inp ? inp[5] : 0);
    rbgpu_diag_set_num("ttl_inp_6", inp ? inp[6] : 0);
    rbgpu_diag_set_num("ttl_this_570", f570);

    rbgpu_log("AmdTtlServices::initialize(this=%p, input=%p): [0]=0x%llx [1]=0x%llx [2]=0x%llx [4]=0x%llx [5]=0x%llx [6]=0x%llx",
              this_ptr, input,
              inp ? inp[0] : 0, inp ? inp[1] : 0, inp ? inp[2] : 0,
              inp ? inp[4] : 0, inp ? inp[5] : 0, inp ? inp[6] : 0);

    rbgpu_log("AmdTtlServices state before (this=%p): 0x560=0x%llx 0x568=0x%llx 0x570=0x%llx 0x578=0x%x 0x598=0x%x 0x958=0x%llx",
              this_ptr, f560, f568, f570, f578, f598, f958);

    if (g_sync_570_mode && f570 == 0 && f958 != 0) {
        *reinterpret_cast<uint64_t *>(reinterpret_cast<uintptr_t>(this_ptr) + 0x570) = f958;
        f570 = f958;
        rbgpu_diag_set_num("ttl_sync_570_applied", 1);
        rbgpu_log("AmdTtlServices: -rbgpusync570 active; synced +0x570 from +0x958 (0x%llx)", f958);
    }

    if (g_safe_ttl_mode && f570 == 0) {
        rbgpu_log("AmdTtlServices: -rbgpusafettl active; applying SafeBoot bypass to pass 1265fd3e");
        *reinterpret_cast<uint32_t *>(reinterpret_cast<uintptr_t>(this_ptr) + 0x578) = 1;
    }

    int ret = 0;
    if (orig_amdtll_init_input) {
        ret = orig_amdtll_init_input(this_ptr, input);
    }

    rbgpu_log("AmdTtlServices::initialize(this=%p, input) returned 0x%08X", this_ptr, ret);
    rbgpu_diag_set_num("ttl_init_inp_ret", static_cast<uint32_t>(ret));
    return ret;
}

static bool patchDeviceTypeTableAt(mach_vm_address_t addr, size_t max_entries = 83) {
    auto table = reinterpret_cast<DeviceTypeEntry *>(addr);

    for (size_t i = 0; i < max_entries; i++) {
        if (table[i].deviceId == 0x1681) {
            rbgpu_diag_set_num("dev_type_patched", 1);
            rbgpu_diag_set_num("dev_type_idx", i);
            rbgpu_diag_set_num("dev_type_rb_id", table[i].deviceId);
            rbgpu_diag_set_num("dev_type_rb_type", table[i].deviceType);
            rbgpu_log("HWLibs: __ZL15deviceTypeTable already contains 0x1681 at index %zu", i);
            return true;
        }
    }

    size_t target_idx = 66;
    if (target_idx < max_entries && table[target_idx].deviceId == 0x005D && table[target_idx].deviceType == 6) {
        uint32_t old_id = table[target_idx].deviceId;

        MachInfo::setKernelWriting(true, KernelPatcher::kernelWriteLock);
        table[target_idx].deviceId   = 0x1681;
        table[target_idx].deviceType = 6;
        MachInfo::setKernelWriting(false, KernelPatcher::kernelWriteLock);

        uint32_t rb_id   = table[target_idx].deviceId;
        uint32_t rb_type = table[target_idx].deviceType;
        rbgpu_diag_set_num("dev_type_patched", 1);
        rbgpu_diag_set_num("dev_type_idx", target_idx);
        rbgpu_diag_set_num("dev_type_rb_id", rb_id);
        rbgpu_diag_set_num("dev_type_rb_type", rb_type);

        if (rb_id != 0x1681) {
            rbgpu_log("HWLibs: [CRITICAL] write verification FAILED at index %zu! readback: 0x%04X != 0x1681", target_idx, rb_id);
            return false;
        }

        rbgpu_log("HWLibs: patched __ZL15deviceTypeTable at index %zu: 0x%04X -> 0x1681 (type 6, verified readback: 0x%04X, type %u)",
                  target_idx, old_id, rb_id, rb_type);
        return true;
    }

    for (size_t i = 0; i < max_entries; i++) {
        if (table[i].deviceType == 6 &&
            table[i].deviceId != 0x73A0 &&
            table[i].deviceId != 0x73A2 &&
            table[i].deviceId != 0x73A3 &&
            table[i].deviceId != 0x73AB &&
            table[i].deviceId != 0x73AE &&
            table[i].deviceId != 0x73BF) {
            uint32_t old_id = table[i].deviceId;

            MachInfo::setKernelWriting(true, KernelPatcher::kernelWriteLock);
            table[i].deviceId   = 0x1681;
            table[i].deviceType = 6;
            MachInfo::setKernelWriting(false, KernelPatcher::kernelWriteLock);

            uint32_t rb_id   = table[i].deviceId;
            uint32_t rb_type = table[i].deviceType;
            rbgpu_diag_set_num("dev_type_patched", 1);
            rbgpu_diag_set_num("dev_type_idx", i);
            rbgpu_diag_set_num("dev_type_rb_id", rb_id);
            rbgpu_diag_set_num("dev_type_rb_type", rb_type);

            if (rb_id != 0x1681) {
                rbgpu_log("HWLibs: [CRITICAL] write verification FAILED at index %zu! readback: 0x%04X != 0x1681", i, rb_id);
                return false;
            }

            rbgpu_log("HWLibs: patched __ZL15deviceTypeTable at active index %zu: 0x%04X -> 0x1681 (type 6, verified readback: 0x%04X, type %u)",
                      i, old_id, rb_id, rb_type);
            return true;
        }
    }

    rbgpu_log("HWLibs: failed to find a replaceable slot in __ZL15deviceTypeTable");
    return false;
}

bool RGPU::patchDeviceTypeTable(KernelPatcher &patcher, size_t id) {
    rbgpu_diag_set_num("dev_type_patched", 0);
    rbgpu_diag_set_num("dev_type_rb_id", 0);
    rbgpu_diag_set_num("dev_type_rb_type", 0);

    mach_vm_address_t tbl_addr = patcher.solveSymbol(id, "__ZL15deviceTypeTable");
    if (!tbl_addr) {
        rbgpu_log("HWLibs: symbol __ZL15deviceTypeTable not resolved; skipping device type patch");
        return false;
    }

    rbgpu_log("HWLibs: resolved __ZL15deviceTypeTable symbol at %p", reinterpret_cast<void *>(tbl_addr));
    return patchDeviceTypeTableAt(tbl_addr);
}

static bool patchDeviceCapabilityTableAt(mach_vm_address_t addr, size_t max_entries = 92) {
    auto table = reinterpret_cast<CapabilityEntry *>(addr);

    for (size_t i = 0; i < max_entries; i++) {
        uint32_t *dev_id_ptr = reinterpret_cast<uint32_t *>(table[i].data + 0x10);
        if (*dev_id_ptr == 0x1681) {
            rbgpu_diag_set_num("cap_tbl_patched", 1);
            rbgpu_diag_set_num("cap_tbl_idx", i);
            rbgpu_log("HWLibs: _DeviceCapabilityTbl already contains 0x1681 at index %zu", i);
            return true;
        }
    }

    size_t template_idx = SIZE_MAX;
    for (size_t i = 0; i < max_entries; i++) {
        uint32_t *dev_id_ptr = reinterpret_cast<uint32_t *>(table[i].data + 0x10);
        if (*dev_id_ptr == 0x73A2) {
            template_idx = i;
            break;
        }
    }

    if (template_idx == SIZE_MAX) {
        rbgpu_log("HWLibs: template device 0x73A2 not found in _DeviceCapabilityTbl; aborting capability patch");
        return false;
    }

    size_t target_idx = SIZE_MAX;
    for (size_t i = 0; i < max_entries; i++) {
        uint32_t *dev_id_ptr = reinterpret_cast<uint32_t *>(table[i].data + 0x10);
        if (*dev_id_ptr == 0x7300) {
            target_idx = i;
            break;
        }
    }

    if (target_idx == SIZE_MAX) {
        rbgpu_log("HWLibs: target device 0x7300 not found in _DeviceCapabilityTbl; aborting capability patch");
        return false;
    }

    MachInfo::setKernelWriting(true, KernelPatcher::kernelWriteLock);
    memcpy(&table[target_idx], &table[template_idx], sizeof(CapabilityEntry));
    *reinterpret_cast<uint32_t *>(table[target_idx].data + 0x10) = 0x1681;
    *reinterpret_cast<uint64_t *>(table[target_idx].data + 0x18) = 0xDEADCAFEULL;
    *reinterpret_cast<uint64_t *>(table[target_idx].data + 0x20) = 0xDEADCAFEULL;
    MachInfo::setKernelWriting(false, KernelPatcher::kernelWriteLock);

    uint32_t rb_id = *reinterpret_cast<uint32_t *>(table[target_idx].data + 0x10);
    uint64_t rb_18 = *reinterpret_cast<uint64_t *>(table[target_idx].data + 0x18);
    uint64_t rb_20 = *reinterpret_cast<uint64_t *>(table[target_idx].data + 0x20);
    rbgpu_log("HWLibs: patched _DeviceCapabilityTbl at index %zu: cloned from index %zu, set deviceId=0x1681, +0x18=0x%llx, +0x20=0x%llx (readback id: 0x%04X)",
              target_idx, template_idx, rb_18, rb_20, rb_id);
    rbgpu_diag_set_num("cap_tbl_patched", rb_id == 0x1681 ? 1 : 0);
    rbgpu_diag_set_num("cap_tbl_idx", target_idx);

    return rb_id == 0x1681;
}

bool RGPU::patchDeviceCapabilityTable(KernelPatcher &patcher, size_t id) {
    rbgpu_diag_set_num("cap_tbl_patched", 0);
    rbgpu_diag_set_num("cap_tbl_idx", 0);

    if (!spoofTtlMode) {
        rbgpu_log("HWLibs: capability table patch skipped (-rbgpuspoofttl not set)");
        return false;
    }

    mach_vm_address_t tbl_addr = patcher.solveSymbol(id, "_DeviceCapabilityTbl");
    if (!tbl_addr) {
        rbgpu_log("HWLibs: symbol _DeviceCapabilityTbl not resolved; skipping capability table patch");
        return false;
    }

    rbgpu_log("HWLibs: resolved _DeviceCapabilityTbl symbol at %p", reinterpret_cast<void *>(tbl_addr));
    return patchDeviceCapabilityTableAt(tbl_addr);
}

// 2ag: _gc_set_fw_entry_info bails at `cmpl $0xa0300` (file 0x126aa1ce;
// fn file 0x126aa16b, so +0x63) because the chip is GC 10.3.3. Patch the
// imm32 to 0xa0303 so the Navi 21 firmware-lookup path runs and we can see
// what _gc_10_3_get_fw_constants fills into the p4 buffer. Gated behind
// -rbgpuspoofttl like the other version spoofs. Verify-before-write.
// 2ah: boot 29 showed the encoding is `3D id` (cmpl $imm32, %eax), not
// `81 /7 id`: the immediate sits at +1, not +2. Handle both forms.
static bool patchGcFwVersionCheckAt(mach_vm_address_t fn_addr) {
    const uintptr_t cmpl_site = fn_addr + 0x63;

    uint8_t op = *reinterpret_cast<uint8_t *>(cmpl_site);

    uintptr_t imm_at = 0;
    if (op == 0x3D) {
        // cmpl $imm32, %eax : opcode(1) + imm32(4)
        imm_at = cmpl_site + 1;
    } else if (op == 0x81) {
        // cmpl $imm32, r/m32 : opcode(1) + modrm(1) + imm32(4), /7 in modrm
        uint8_t modrm = *reinterpret_cast<uint8_t *>(cmpl_site + 1);
        if ((modrm & 0x38) != 0x38) {
            rbgpu_log("HWLibs: GC fw version check: 0x81 with unexpected modrm 0x%02X, NOT patching",
                      modrm);
            return false;
        }
        imm_at = cmpl_site + 2;
    } else {
        rbgpu_log("HWLibs: GC fw version check: unexpected opcode 0x%02X at %p, NOT patching",
                  op, reinterpret_cast<void *>(cmpl_site));
        return false;
    }

    uint32_t imm = *reinterpret_cast<uint32_t *>(imm_at);

    rbgpu_diag_set_num("gc_vercheck_patched", 0);
    if (imm == 0x000A0303) {
        rbgpu_log("HWLibs: GC fw version check already patched (imm=0x%08X)", imm);
        rbgpu_diag_set_num("gc_vercheck_patched", 1);
        return true;
    }
    if (imm != 0x000A0300) {
        rbgpu_log("HWLibs: GC fw version check: unexpected imm 0x%08X at %p, NOT patching",
                  imm, reinterpret_cast<void *>(imm_at));
        return false;
    }

    MachInfo::setKernelWriting(true, KernelPatcher::kernelWriteLock);
    *reinterpret_cast<uint32_t *>(imm_at) = 0x000A0303;
    MachInfo::setKernelWriting(false, KernelPatcher::kernelWriteLock);

    uint32_t rb = *reinterpret_cast<uint32_t *>(imm_at);
    if (rb != 0x000A0303) {
        rbgpu_log("HWLibs: [CRITICAL] GC fw version check patch FAILED readback: 0x%08X", rb);
        return false;
    }
    rbgpu_diag_set_num("gc_vercheck_patched", 1);
    rbgpu_log("HWLibs: patched GC fw version check imm 0x000A0300 -> 0x000A0303 at %p",
              reinterpret_cast<void *>(imm_at));
    return true;
}

bool RGPU::patchGcFwVersionCheck(KernelPatcher &patcher, size_t id) {
    if (!spoofTtlMode) {
        rbgpu_log("HWLibs: GC fw version check patch skipped (-rbgpuspoofttl not set)");
        return false;
    }

    mach_vm_address_t fn_addr = patcher.solveSymbol(id, "_gc_set_fw_entry_info");
    if (!fn_addr) {
        rbgpu_log("HWLibs: symbol _gc_set_fw_entry_info not resolved; skipping GC fw version check patch");
        return false;
    }

    rbgpu_log("HWLibs: resolved _gc_set_fw_entry_info at %p", reinterpret_cast<void *>(fn_addr));
    return patchGcFwVersionCheckAt(fn_addr);
}

// 2am: _gc_fw_autoload_is_completed waits for BOOTLOAD_COMPLETE in
// RLC_RLCS_BOOTLOAD_STATUS, a latch set only by the RLC's backdoor-autoload
// bootloader. When the PSP places the firmware itself (our path: boot 36
// proved _psp_np_fw_load returns 0), nothing sets the latch during the wait,
// so it times out after 50ms even though the microcode is present. An
// independent Raphael-iGPU bring-up measured exactly this: RLC_STAT==0x25,
// BOOTLOAD_COMPLETE clear at both offsets, yet instruction RAM holds real
// microcode in all five engines. Patch the function entry to return 1
// immediately (mov eax,1; ret). Gated behind -rbgpupsphw; verify-before-write.
// File-static (no hpp change needed).
static bool patchGcAutoloadCheckAt(mach_vm_address_t fn_addr) {
    uint8_t b0 = *reinterpret_cast<uint8_t *>(fn_addr);
    if (b0 == 0xC3 || b0 == 0xCC) {
        rbgpu_log("HWLibs: GC autoload check already patched or stub (b0=0x%02X), skipping", b0);
        rbgpu_diag_set_num("gc_autoload_patched", 1);
        return true;
    }
    // mov eax, 1; ret
    static const uint8_t patch[6] = {0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3};
    MachInfo::setKernelWriting(true, KernelPatcher::kernelWriteLock);
    for (int i = 0; i < 6; i++)
        *(reinterpret_cast<uint8_t *>(fn_addr) + i) = patch[i];
    MachInfo::setKernelWriting(false, KernelPatcher::kernelWriteLock);
    for (int i = 0; i < 6; i++) {
        uint8_t rb = *(reinterpret_cast<uint8_t *>(fn_addr) + i);
        if (rb != patch[i]) {
            rbgpu_log("HWLibs: [CRITICAL] GC autoload check patch FAILED readback at +%d: 0x%02X", i, rb);
            return false;
        }
    }
    rbgpu_diag_set_num("gc_autoload_patched", 1);
    rbgpu_log("HWLibs: patched _gc_fw_autoload_is_completed entry to return 1 at %p",
              reinterpret_cast<void *>(fn_addr));
    return true;
}

static bool patchGcAutoloadCheck(KernelPatcher &patcher, size_t id) {
    if (!g_psp_hw_mode) {
        rbgpu_log("HWLibs: GC autoload check patch skipped (-rbgpupsphw not set)");
        return false;
    }
    mach_vm_address_t fn_addr = patcher.solveSymbol(id, "_gc_fw_autoload_is_completed");
    if (!fn_addr) {
        rbgpu_log("HWLibs: symbol _gc_fw_autoload_is_completed not resolved; skipping patch");
        return false;
    }
    rbgpu_log("HWLibs: resolved _gc_fw_autoload_is_completed at %p", reinterpret_cast<void *>(fn_addr));
    return patchGcAutoloadCheckAt(fn_addr);
}

// 2an: _gc_hw_init calls _gc_check_register_equal_ext (a "wait until register
// == value" helper used by cosWaitForFunc) several times; boot 37 shows two
// 500ms timeouts killing the init. The Raphael-iGPU bring-up identified the
// same pattern in _gc_enter_rlc_safe_mode_10_3: it writes RLC_SAFE_MODE and
// waits 500ms for the RLC to clear CMD, which never happens on the PSP-load
// path, while upstream Linux just proceeds after its timeout. Patch the helper
// entry to return 1 immediately ("condition met"). Gated behind -rbgpupsphw;
// verify-before-write. File-static (no hpp change needed).
static bool patchGcRegCheckAt(mach_vm_address_t fn_addr) {
    uint8_t b0 = *reinterpret_cast<uint8_t *>(fn_addr);
    if (b0 == 0xC3 || b0 == 0xCC) {
        rbgpu_log("HWLibs: GC reg-check already patched or stub (b0=0x%02X), skipping", b0);
        rbgpu_diag_set_num("gc_regcheck_patched", 1);
        return true;
    }
    // mov eax, 1; ret
    static const uint8_t patch[6] = {0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3};
    MachInfo::setKernelWriting(true, KernelPatcher::kernelWriteLock);
    for (int i = 0; i < 6; i++)
        *(reinterpret_cast<uint8_t *>(fn_addr) + i) = patch[i];
    MachInfo::setKernelWriting(false, KernelPatcher::kernelWriteLock);
    for (int i = 0; i < 6; i++) {
        uint8_t rb = *(reinterpret_cast<uint8_t *>(fn_addr) + i);
        if (rb != patch[i]) {
            rbgpu_log("HWLibs: [CRITICAL] GC reg-check patch FAILED readback at +%d: 0x%02X", i, rb);
            return false;
        }
    }
    rbgpu_diag_set_num("gc_regcheck_patched", 1);
    rbgpu_log("HWLibs: patched _gc_check_register_equal_ext entry to return 1 at %p",
              reinterpret_cast<void *>(fn_addr));
    return true;
}

static bool patchGcRegCheck(KernelPatcher &patcher, size_t id) {
    if (!g_psp_hw_mode) {
        rbgpu_log("HWLibs: GC reg-check patch skipped (-rbgpupsphw not set)");
        return false;
    }
    mach_vm_address_t fn_addr = patcher.solveSymbol(id, "_gc_check_register_equal_ext");
    if (!fn_addr) {
        rbgpu_log("HWLibs: symbol _gc_check_register_equal_ext not resolved; skipping patch");
        return false;
    }
    rbgpu_log("HWLibs: resolved _gc_check_register_equal_ext at %p", reinterpret_cast<void *>(fn_addr));
    return patchGcRegCheckAt(fn_addr);
}

// 2ao: _sdma_5_2_1_fw_autoload_is_completed is the SDMA twin of the GC autoload
// wait: it reads the same BOOTLOAD_COMPLETE latch through the SDMA client, and
// upstream's sdma_v5_2_start has no such wait at all on the PSP load path.
// Boot 38 proved GC hw_init now passes (2am+2an worked); SDMA hw_init then
// timed out here after 50ms. Patch the entry to return 1 immediately.
// Gated behind -rbgpupsphw; verify-before-write. File-static.
static bool patchSdmaAutoloadCheckAt(mach_vm_address_t fn_addr) {
    uint8_t b0 = *reinterpret_cast<uint8_t *>(fn_addr);
    if (b0 == 0xC3 || b0 == 0xCC) {
        rbgpu_log("HWLibs: SDMA autoload check already patched or stub (b0=0x%02X), skipping", b0);
        rbgpu_diag_set_num("sdma_autoload_patched", 1);
        return true;
    }
    // mov eax, 1; ret
    static const uint8_t patch[6] = {0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3};
    MachInfo::setKernelWriting(true, KernelPatcher::kernelWriteLock);
    for (int i = 0; i < 6; i++)
        *(reinterpret_cast<uint8_t *>(fn_addr) + i) = patch[i];
    MachInfo::setKernelWriting(false, KernelPatcher::kernelWriteLock);
    for (int i = 0; i < 6; i++) {
        uint8_t rb = *(reinterpret_cast<uint8_t *>(fn_addr) + i);
        if (rb != patch[i]) {
            rbgpu_log("HWLibs: [CRITICAL] SDMA autoload check patch FAILED readback at +%d: 0x%02X", i, rb);
            return false;
        }
    }
    rbgpu_diag_set_num("sdma_autoload_patched", 1);
    rbgpu_log("HWLibs: patched _sdma_5_2_1_fw_autoload_is_completed entry to return 1 at %p",
              reinterpret_cast<void *>(fn_addr));
    return true;
}

static bool patchSdmaAutoloadCheck(KernelPatcher &patcher, size_t id) {
    if (!g_psp_hw_mode) {
        rbgpu_log("HWLibs: SDMA autoload check patch skipped (-rbgpupsphw not set)");
        return false;
    }
    mach_vm_address_t fn_addr = patcher.solveSymbol(id, "_sdma_5_2_1_fw_autoload_is_completed");
    if (!fn_addr) {
        rbgpu_log("HWLibs: symbol _sdma_5_2_1_fw_autoload_is_completed not resolved; skipping patch");
        return false;
    }
    rbgpu_log("HWLibs: resolved _sdma_5_2_1_fw_autoload_is_completed at %p", reinterpret_cast<void *>(fn_addr));
    return patchSdmaAutoloadCheckAt(fn_addr);
}

void RGPU::installKernelLogHooks(KernelPatcher &patcher) {
    static bool s_kernel_hooks_installed = false;
    if (s_kernel_hooks_installed || noLogCapture) {
        return;
    }

    KernelPatcher::RouteRequest req_kprintf("_kprintf", wrap_kprintf, orig_kprintf);
    if (patcher.routeMultiple(KernelPatcher::KernelID, &req_kprintf, 1)) {
        rbgpu_log("Kernel: routed _kprintf successfully");
    } else {
        rbgpu_log("Kernel: failed to route _kprintf");
    }

    KernelPatcher::RouteRequest req_iolog("_IOLog", wrap_iolog, orig_iolog);
    if (patcher.routeMultiple(KernelPatcher::KernelID, &req_iolog, 1)) {
        rbgpu_log("Kernel: routed _IOLog successfully");
    } else {
        rbgpu_log("Kernel: failed to route _IOLog");
    }

    s_kernel_hooks_installed = true;
}

void RGPU::init() {
    noSmuHook    = checkKernelArgument("-rbgpunosmu");
    noTrap3Guard = checkKernelArgument("-rbgpunoguard");
    noLogCapture = checkKernelArgument("-rbgpunocapture");
    safeTtlMode  = checkKernelArgument("-rbgpusafettl");
    sync570Mode  = checkKernelArgument("-rbgpusync570");
    spoofTtlMode = checkKernelArgument("-rbgpuspoofttl");
    pspHwMode    = checkKernelArgument("-rbgpupsphw");
    g_safe_ttl_mode  = safeTtlMode;
    g_sync_570_mode  = sync570Mode;
    g_spoof_ttl_mode = spoofTtlMode;
    g_psp_hw_mode    = pspHwMode;

    rbgpu_log("loaded (Milestone 2; SMU Hook: %s, Trap 3 Guard: %s, Capture: %s, SafeTTL: %s, Sync570: %s, SpoofTTL: %s, PSPHW: %s)",
              noSmuHook ? "DISABLED" : "ENABLED",
              noTrap3Guard ? "DISABLED" : "ENABLED",
              noLogCapture ? "DISABLED" : "ENABLED",
              safeTtlMode ? "ENABLED" : "DISABLED",
              sync570Mode ? "ENABLED" : "DISABLED",
              spoofTtlMode ? "ENABLED" : "DISABLED",
              pspHwMode ? "ENABLED" : "DISABLED");

    lilu.onKextLoad(kextList, arrsize(kextList), [](void *user, KernelPatcher &patcher, size_t id, mach_vm_address_t address, size_t size) {
        static_cast<RGPU *>(user)->processKext(patcher, id, address, size);
    }, this);
}

void RGPU::deinit() {
}

void RGPU::processKext(KernelPatcher &patcher, size_t id, mach_vm_address_t address, size_t size) {
    installKernelLogHooks(patcher);

    if (kextList[KextHWLibs].loadIndex == id) {
        g_kext_addr_hwlibs = address;
        g_kext_size_hwlibs = size;
        rbgpu_log("HWLibs loaded at %p (size 0x%zx)", reinterpret_cast<void *>(address), size);
        processHWLibs(patcher, id, address, size);
    } else if (kextList[KextX6000].loadIndex == id) {
        g_kext_addr_x6000 = address;
        g_kext_size_x6000 = size;
        rbgpu_log("X6000 loaded at %p (size 0x%zx)", reinterpret_cast<void *>(address), size);
        processX6000(patcher, id, address, size);
    } else if (kextList[KextAccelFamily].loadIndex == id) {
        g_kext_addr_accelfam = address;
        g_kext_size_accelfam = size;
        rbgpu_log("AccelFamily loaded at %p (size 0x%zx)", reinterpret_cast<void *>(address), size);
        processAccelFamily(patcher, id, address, size);
    } else if (kextList[KextHWSvc].loadIndex == id) {
        g_kext_addr_hwsvc = address;
        g_kext_size_hwsvc = size;
        rbgpu_log("HWSvc loaded at %p (size 0x%zx)", reinterpret_cast<void *>(address), size);
        processHWServices(patcher, id, address, size);
    }
}

// 2da (2026-10-12): per-firmware PSP load results.
//
// The whole bring-up now turns on one question: did the PSP accept and start
// the yellow_carp images? `yc_swapped 7` only proves HWLibs' descriptors were
// swapped, not that the PSP validated or ran anything, and the KIQ packet
// going unconsumed (2cx) says the command processor is not running.
//
// _ipi_psp_load_fw (HWLibs 0x1264289b) is where each image goes to the PSP:
//     edi = *(uint32 *)p                      TTL firmware id
//     _TtlFwIdToCgsFwId(ttl_id)        -> cgs id
//     _ipiPspCgsFwToPspFw(cgs_id)      -> psp id; == 0x37 means "no mapping",
//                                         which logs an assert and returns 4
//     otherwise it builds the command and submits it to the PSP
// So the TTL id and the return value, per call, say exactly which firmware
// the PSP took and which it refused. Observe only.
static int (*orig_ipi_psp_load_fw_2da)(void *, void *) = nullptr;
static volatile int g_rb_2da_fw_calls = 0;
static volatile int g_rb_2da_fw_fails = 0;
static volatile uint32_t g_rb_2da_fw_failmask = 0;

static int hooked_ipi_psp_load_fw_2da(void *dev, void *p) {
    uint32_t ttl_fw_id = p ? *reinterpret_cast<uint32_t *>(p) : 0xffffffff;
    g_rb_2da_fw_calls++;
    rbgpu_log("2da: _ipi_psp_load_fw ENTER (dev=%p ttl_fw_id=%u)", dev, ttl_fw_id);

    int ret = orig_ipi_psp_load_fw_2da ? orig_ipi_psp_load_fw_2da(dev, p) : 4;

    if (ret != 0) {
        g_rb_2da_fw_fails++;
        if (ttl_fw_id < 32) {
            g_rb_2da_fw_failmask |= (1u << ttl_fw_id);
        }
    }
    rbgpu_diag_set_num("fw_calls", static_cast<uint64_t>(g_rb_2da_fw_calls));
    rbgpu_diag_set_num("fw_fails", static_cast<uint64_t>(g_rb_2da_fw_fails));
    rbgpu_diag_set_num("fw_failmask", static_cast<uint64_t>(g_rb_2da_fw_failmask));
    rbgpu_log("2da: _ipi_psp_load_fw(ttl_fw_id=%u) -> %d %s", ttl_fw_id, ret,
              ret == 0 ? "(PSP ACCEPTED)" : "(REFUSED)");
    return ret;
}

// 2db (2026-10-12): did the GC firmware autoload buffer ever get prepared?
//
// The 16:50 capture showed _ipi_psp_load_fw hooked and NEVER called, so no
// image is handed to the PSP one at a time. HWLibs has a second path for
// GC: "backdoor autoload", where the driver allocates a buffer, copies every
// firmware image into it, configures the registers that point the hardware
// at it, and then the PSP loads the lot. _gc_fw_autoload_is_completed polls
// for that to finish — and 2am patches it to report "done" unconditionally,
// which says nothing about whether the buffer was ever filled.
//
// If these four never run, nothing was ever loaded and the command processor
// could not possibly be executing: that would explain the unconsumed KIQ
// packet completely, and would mean 2am has been reporting success for a
// load that never happened. Observe only.
static uint64_t (*orig_gc_alloc_autoload_2db)(void *, void *) = nullptr;
static uint64_t (*orig_gc_copy_autoload_2db)(void *, void *) = nullptr;
static uint64_t (*orig_gc_config_autoload_2db)(void *, void *) = nullptr;
static uint64_t (*orig_gc_validate_autoload_2db)(void *, void *) = nullptr;

static uint64_t hooked_gc_alloc_autoload_2db(void *a, void *b) {
    rbgpu_log("2db: _gc_allocate_fw_backdoor_autoload_buffer ENTER (a=%p b=%p)", a, b);
    uint64_t r = orig_gc_alloc_autoload_2db ? orig_gc_alloc_autoload_2db(a, b) : 1;
    rbgpu_diag_set_num("al_alloc_ret", r);
    rbgpu_diag_inc("al_alloc_calls");
    rbgpu_log("2db: _gc_allocate_fw_backdoor_autoload_buffer -> 0x%llx", r);
    return r;
}

static uint64_t hooked_gc_copy_autoload_2db(void *a, void *b) {
    rbgpu_log("2db: _gc_copy_backdoor_autoload_fw ENTER (a=%p b=%p)", a, b);
    uint64_t r = orig_gc_copy_autoload_2db ? orig_gc_copy_autoload_2db(a, b) : 1;
    rbgpu_diag_set_num("al_copy_ret", r);
    rbgpu_diag_inc("al_copy_calls");
    rbgpu_log("2db: _gc_copy_backdoor_autoload_fw -> 0x%llx", r);
    return r;
}

static uint64_t hooked_gc_config_autoload_2db(void *a, void *b) {
    rbgpu_log("2db: _gc_fw_backdoor_autoload_config ENTER (a=%p b=%p)", a, b);
    uint64_t r = orig_gc_config_autoload_2db ? orig_gc_config_autoload_2db(a, b) : 1;
    rbgpu_diag_set_num("al_config_ret", r);
    rbgpu_diag_inc("al_config_calls");
    rbgpu_log("2db: _gc_fw_backdoor_autoload_config -> 0x%llx", r);
    return r;
}

static uint64_t hooked_gc_validate_autoload_2db(void *a, void *b) {
    rbgpu_log("2db: _gc_fw_backdoor_autoload_validate ENTER (a=%p b=%p)", a, b);
    uint64_t r = orig_gc_validate_autoload_2db ? orig_gc_validate_autoload_2db(a, b) : 1;
    rbgpu_diag_set_num("al_valid_ret", r);
    rbgpu_diag_inc("al_valid_calls");
    rbgpu_log("2db: _gc_fw_backdoor_autoload_validate -> 0x%llx", r);
    return r;
}

void RGPU::routeTTL(KernelPatcher &patcher, size_t id, mach_vm_address_t address, size_t size) {
    KernelPatcher::RouteRequest reqs[] = {
        KernelPatcher::RouteRequest(
            "__ZN14AmdTtlServices10initializeEP30_TtlLibraryInitializationInput",
            hooked_AmdTtlServices_initialize_input,
            orig_amdtll_init_input
        ),
        KernelPatcher::RouteRequest(
            "__ZN14AmdTtlServices10initializeEv",
            hooked_AmdTtlServices_initialize,
            orig_amdtll_initialize
        ),
        KernelPatcher::RouteRequest(
            "__ZN14AmdTtlServices20setFirmwareDirectoryEP20AMDFirmwareDirectory",
            hooked_AmdTtlServices_setFirmwareDirectory,
            orig_amdtll_set_fw_dir
        ),
        KernelPatcher::RouteRequest(
            "__ZN14AmdTtlServices11submitFrameE19AMD_SWIP_QUEUE_TYPEjPv",
            hooked_AmdTtlServices_submitFrame,
            orig_amdtll_submit_frame
        ),
        // 2da: per-firmware PSP load results - the open question.
        KernelPatcher::RouteRequest(
            "_ipi_psp_load_fw",
            hooked_ipi_psp_load_fw_2da,
            orig_ipi_psp_load_fw_2da
        ),
        // 2db: the GC backdoor-autoload chain, which is the path GC actually
        // uses and which 2am reports as "completed" without checking.
        KernelPatcher::RouteRequest(
            "_gc_allocate_fw_backdoor_autoload_buffer",
            hooked_gc_alloc_autoload_2db,
            orig_gc_alloc_autoload_2db
        ),
        KernelPatcher::RouteRequest(
            "_gc_copy_backdoor_autoload_fw_10_1",
            hooked_gc_copy_autoload_2db,
            orig_gc_copy_autoload_2db
        ),
        KernelPatcher::RouteRequest(
            "_gc_fw_backdoor_autoload_config_10_1",
            hooked_gc_config_autoload_2db,
            orig_gc_config_autoload_2db
        ),
        KernelPatcher::RouteRequest(
            "_gc_fw_backdoor_autoload_validate_10_1",
            hooked_gc_validate_autoload_2db,
            orig_gc_validate_autoload_2db
        ),
        KernelPatcher::RouteRequest(
            "__ZN14AmdTtlServices12createEngineE19AMD_SWIP_QUEUE_TYPEjP21_TtlCreateEngineInput",
            hooked_AmdTtlServices_createEngine,
            orig_amdtll_create_engine
        ),
        KernelPatcher::RouteRequest(
            "__ZN14AmdTtlServices12buildCommandE19AMD_SWIP_QUEUE_TYPEjP20_TtlBuildCommandInfoPv",
            hooked_AmdTtlServices_buildCommand,
            orig_amdtll_build_command
        ),
        KernelPatcher::RouteRequest(
            "_ttl_initialize",
            hooked_ttl_initialize,
            orig_ttl_initialize
        ),
        KernelPatcher::RouteRequest(
            "_ttlIsSupportedDevice",
            hooked_ttlIsSupportedDevice,
            orig_ttl_is_supported_device
        ),
        KernelPatcher::RouteRequest(
            "_ttl_hw_init",
            hooked_ttl_hw_init,
            orig_ttl_hw_init
        ),
        KernelPatcher::RouteRequest(
            "_IpiExecuteSwipEntryPoint",
            hooked_ipi_execute_swip_entry_point,
            orig_ipi_execute_swip_entry_point
        ),
        KernelPatcher::RouteRequest(
            "_IpiGetDiscoveryInfo",
            hooked_IpiGetDiscoveryInfo,
            orig_ipi_get_discovery_info
        )
    };

    for (size_t i = 0; i < arrsize(reqs); i++) {
        if (patcher.routeMultiple(id, &reqs[i], 1, address, size)) {
            rbgpu_log("HWLibs: hooked %s", reqs[i].symbol);
        } else {
            rbgpu_log("HWLibs: failed to hook %s", reqs[i].symbol);
        }
    }
}

void RGPU::routePSPBasics(KernelPatcher &patcher, size_t id, mach_vm_address_t address, size_t size) {
    KernelPatcher::RouteRequest reqs[] = {
        KernelPatcher::RouteRequest(
            "_ipi_psp_sw_init",
            hooked_ipi_psp_sw_init,
            orig_ipi_psp_sw_init
        ),
        KernelPatcher::RouteRequest(
            "_ipi_psp_hw_init",
            hooked_ipi_psp_hw_init,
            orig_ipi_psp_hw_init
        ),
        KernelPatcher::RouteRequest(
            "_psp_sw_init",
            hooked_psp_sw_init,
            orig_psp_sw_init
        ),
        KernelPatcher::RouteRequest(
            "_psp_hw_init",
            hooked_psp_hw_init,
            orig_psp_hw_init
        ),
        KernelPatcher::RouteRequest(
            "_psp_cos_alloc_memory",
            hooked_psp_cos_alloc_memory,
            orig_psp_cos_alloc_memory
        ),
        KernelPatcher::RouteRequest(
            "_psp_asic_type_init",
            hooked_psp_asic_type_init,
            orig_psp_asic_type_init
        ),
        KernelPatcher::RouteRequest(
            "_psp_init_pfn_ptr",
            hooked_psp_init_pfn_ptr,
            orig_psp_init_pfn_ptr
        ),
        KernelPatcher::RouteRequest(
            "_psp_read_config",
            hooked_psp_read_config,
            orig_psp_read_config
        )
    };

    for (size_t i = 0; i < arrsize(reqs); i++) {
        if (patcher.routeMultiple(id, &reqs[i], 1, address, size)) {
            rbgpu_log("HWLibs: hooked %s", reqs[i].symbol);
        } else {
            rbgpu_log("HWLibs: failed to hook %s", reqs[i].symbol);
        }
    }
}

void RGPU::routePSPRingTmr(KernelPatcher &patcher, size_t id, mach_vm_address_t address, size_t size) {
    KernelPatcher::RouteRequest reqs[] = {
        KernelPatcher::RouteRequest(
            "_psp_allocate_private_mem",
            hooked_psp_allocate_private_mem,
            orig_psp_allocate_private_mem
        ),
        KernelPatcher::RouteRequest(
            "_psp_ring_init",
            hooked_psp_ring_init,
            orig_psp_ring_init
        ),
        KernelPatcher::RouteRequest(
            "_psp_ring_create",
            hooked_psp_ring_create,
            orig_psp_ring_create
        ),
        KernelPatcher::RouteRequest(
            "_psp_bootloader_is_sos_running",
            hooked_psp_bootloader_is_sos_running,
            orig_psp_bootloader_is_sos_running
        ),
        KernelPatcher::RouteRequest(
            "_psp_cmd_km_init",
            hooked_psp_cmd_km_init,
            orig_psp_cmd_km_init
        ),
        KernelPatcher::RouteRequest(
            "_psp_tmr_init",
            hooked_psp_tmr_init,
            orig_psp_tmr_init
        ),
        KernelPatcher::RouteRequest(
            "_psp_tmr_load",
            hooked_psp_tmr_load,
            orig_psp_tmr_load
        ),
        KernelPatcher::RouteRequest(
            "_psp_dtm_init",
            hooked_psp_dtm_init,
            orig_psp_dtm_init
        ),
        KernelPatcher::RouteRequest(
            "_psp_np_fw_load",
            hooked_psp_np_fw_load,
            orig_psp_np_fw_load
        )
    };

    for (size_t i = 0; i < arrsize(reqs); i++) {
        if (patcher.routeMultiple(id, &reqs[i], 1, address, size)) {
            rbgpu_log("HWLibs: hooked %s", reqs[i].symbol);
        } else {
            rbgpu_log("HWLibs: failed to hook %s", reqs[i].symbol);
        }
    }
}

void RGPU::routeGC(KernelPatcher &patcher, size_t id, mach_vm_address_t address, size_t size) {
    KernelPatcher::RouteRequest reqs[] = {
        KernelPatcher::RouteRequest(
            "_ipi_gc_sw_init",
            hooked_ipi_gc_sw_init,
            orig_ipi_gc_sw_init
        ),
        KernelPatcher::RouteRequest(
            "_gc_sw_init",
            hooked_gc_sw_init,
            orig_gc_sw_init
        ),
        KernelPatcher::RouteRequest(
            "_gc_get_hw_version",
            hooked_gc_get_hw_version,
            orig_gc_get_hw_version
        ),
        KernelPatcher::RouteRequest(
            "_gc_get_config",
            hooked_gc_get_config,
            orig_gc_get_config
        ),
        KernelPatcher::RouteRequest(
            "_gc_get_hw_config_tbl_info",
            hooked_gc_get_hw_config_tbl_info,
            orig_gc_get_hw_config_tbl_info
        ),
        KernelPatcher::RouteRequest(
            "_gc_set_fw_entry_info",
            hooked_gc_set_fw_entry_info,
            orig_gc_set_fw_entry_info
        ),
        KernelPatcher::RouteRequest(
            "_gc_init_fcn_ptr_list",
            hooked_gc_init_fcn_ptr_list,
            orig_gc_init_fcn_ptr_list
        ),
        KernelPatcher::RouteRequest(
            "_gc_get_doorbell_requirements",
            hooked_gc_get_doorbell_requirements,
            orig_gc_get_doorbell_requirements
        ),
        KernelPatcher::RouteRequest(
            "_ttlDevGetDoorbellRange",
            hooked_ttlDevGetDoorbellRange,
            orig_ttl_dev_get_doorbell_range
        ),
        KernelPatcher::RouteRequest(
            "_IpiSetFwEntry",
            hooked_IpiSetFwEntry,
            orig_ipi_set_fw_entry
        ),
        KernelPatcher::RouteRequest(
            "_ipi_gc_hw_init",
            hooked_ipi_gc_hw_init,
            orig_ipi_gc_hw_init
        ),
        KernelPatcher::RouteRequest(
            "_gc_hw_init",
            hooked_gc_hw_init,
            orig_gc_hw_init
        )
    };

    for (size_t i = 0; i < arrsize(reqs); i++) {
        if (patcher.routeMultiple(id, &reqs[i], 1, address, size)) {
            rbgpu_log("HWLibs: hooked %s", reqs[i].symbol);
        } else {
            rbgpu_log("HWLibs: failed to hook %s", reqs[i].symbol);
        }
    }
}

void RGPU::processHWLibs(KernelPatcher &patcher, size_t id, mach_vm_address_t address, size_t size) {
    rbgpu_log("Patching com.apple.kext.AMDRadeonX6800HWLibs");

    patchDeviceTypeTable(patcher, id);
    patchDeviceCapabilityTable(patcher, id);
    patchGcFwVersionCheck(patcher, id);
    // 2bb: RE-ENABLED the gc_autoload bypass. 2ba PROVED the hardware is healthy:
    // RLC_STAT=0x25 (matches Raphael healthy), BOOTLOAD=0x1 (bit 0 set).
    // But bit 31 (BOOTLOAD_COMPLETE) is never set during HW_INIT on PSP path.
    // Raphael confirmed: latch sets AFTER HW_INIT, not during. Apple's check
    // is incompatible with PSP firmware loading. Bypass is necessary, not cheating.
    patchGcAutoloadCheck(patcher, id);
    patchGcRegCheck(patcher, id);
    patchSdmaAutoloadCheck(patcher, id);

    routeTTL(patcher, id, address, size);
    routePSPBasics(patcher, id, address, size);
    routePSPRingTmr(patcher, id, address, size);
    routeGC(patcher, id, address, size);

    if (!noSmuHook) {
        orig_smu_cgs_alloc_memory = reinterpret_cast<smu_cgs_alloc_memory_t>(patcher.solveSymbol(id, "_smu_cgs_alloc_memory"));
        if (!orig_smu_cgs_alloc_memory) {
            rbgpu_log("HWLibs: failed to resolve symbol _smu_cgs_alloc_memory");
        }

        orig_smu_cgs_free_memory = reinterpret_cast<smu_cgs_free_memory_t>(patcher.solveSymbol(id, "_smu_cgs_free_memory"));
        if (!orig_smu_cgs_free_memory) {
            rbgpu_log("HWLibs: failed to resolve symbol _smu_cgs_free_memory (expected)");
        }

        orig_smu_get_hw_version = reinterpret_cast<smu_get_hw_version_t>(patcher.solveSymbol(id, "_smu_get_hw_version"));
        if (!orig_smu_get_hw_version) {
            rbgpu_log("HWLibs: failed to resolve symbol _smu_get_hw_version");
        }

        orig_smu_read_config_space = reinterpret_cast<smu_read_config_space_t>(patcher.solveSymbol(id, "_smu_read_config_space"));
        if (!orig_smu_read_config_space) {
            rbgpu_log("HWLibs: failed to resolve symbol _smu_read_config_space");
        }

        mach_vm_address_t smu_init_sym = patcher.solveSymbol(id, "_smu_init_function_pointer_list");
        if (!smu_init_sym) {
            rbgpu_log("HWLibs: failed to resolve symbol _smu_init_function_pointer_list");
        }

        KernelPatcher::RouteRequest requests[] = {
            KernelPatcher::RouteRequest("_smu_init_function_pointer_list", hooked_smu_init_function_pointer_list, orig_smu_init_function_pointer_list)
        };

        if (patcher.routeMultiple(id, requests, address, size)) {
            rbgpu_diag_set_num("smu_hook_routed", 1);
            rbgpu_log("H-SMU-1: smu_init_function_pointer_list hooked successfully");
        } else {
            rbgpu_diag_set_num("smu_hook_routed", 0);
            rbgpu_log("H-SMU-1: failed to hook smu_init_function_pointer_list");
        }
    }
}

void RGPU::processX6000(KernelPatcher &patcher, size_t id, mach_vm_address_t address, size_t size) {
    rbgpu_log("Patching com.apple.kext.AMDRadeonX6000 (Trap 3 Failure Guard)");

    if (noTrap3Guard) {
        return;
    }

    mach_vm_address_t start_sym = patcher.solveSymbol(id, "__ZN37AMDRadeonX6000_AMDGraphicsAccelerator5startEP9IOService");
    if (!start_sym) {
        rbgpu_log("X6000: failed to resolve symbol __ZN37AMDRadeonX6000_AMDGraphicsAccelerator5startEP9IOService");
    }

    mach_vm_address_t stop_sym  = patcher.solveSymbol(id, "__ZN37AMDRadeonX6000_AMDGraphicsAccelerator4stopEP9IOService");
    if (!stop_sym) {
        rbgpu_log("X6000: failed to resolve symbol __ZN37AMDRadeonX6000_AMDGraphicsAccelerator4stopEP9IOService");
    }

    if (!start_sym || !stop_sym) {
        rbgpu_log("Trap 3 Guard: required start/stop symbols not resolved");
        rbgpu_diag_set_num("guard_routed", 0);
        return;
    }

    auto start_bytes = reinterpret_cast<uint8_t *>(start_sym);
    bool patched = false;

    // 2be: the assistant's analysis - boot-39 panic is on start()'s FAILURE path.
    // start() calls stop() on failure; stop() clears this+0x1ea0 to 0;
    // the shared exit then dereferences NULL+0x18 and panics.
    // 2bd proved the hardcoded offset (0x6b2) doesn't match the loaded kext
    // (ASLR slide; bytes at 0x6b2 were 48 8b 03, not a CALL).
    // New approach: patch stop() entry to RET (0xC3). If stop() never runs,
    // +0x1ea0 is never cleared, the exit can't panic, and start() fails
    // cleanly with the state trace surviving.
    {
        uint8_t *stop_bytes = reinterpret_cast<uint8_t *>(stop_sym);
        rbgpu_log("Trap 3 Guard 2be: stop_sym=%p, first byte=0x%02x", stop_bytes, stop_bytes[0]);
        MachInfo::setKernelWriting(true, KernelPatcher::kernelWriteLock);
        stop_bytes[0] = 0xC3;  // RET
        MachInfo::setKernelWriting(false, KernelPatcher::kernelWriteLock);
        rbgpu_log("Trap 3 Guard 2be: patched stop() entry to RET");
        rbgpu_diag_set_num("guard_routed", 1);
        patched = true;
    }

    // Fallback: try the old direct-call search if the targeted patch didn't work
    for (size_t i = 0; i < 0x2000; i++) {
        if (patched) break;  // Skip if targeted patch already succeeded
        if (start_bytes[i] == 0xE8) {
            int32_t rel = *reinterpret_cast<int32_t *>(&start_bytes[i + 1]);
            mach_vm_address_t call_dest = reinterpret_cast<mach_vm_address_t>(&start_bytes[i + 5]) + rel;
            if (call_dest == stop_sym) {
                uint8_t *post_stop = &start_bytes[i + 5];
                rbgpu_log("Trap 3 Guard: located call stop() at %p (offset +0x%zx)", &start_bytes[i], i);

                uint8_t *pop_seq = nullptr;
                for (size_t j = 0; j < 64; j++) {
                    if (post_stop[j] == 0xC3) {
                        size_t k = j;
                        while (k > 0 && ((post_stop[k - 1] >= 0x58 && post_stop[k - 1] <= 0x5F) ||
                                         post_stop[k - 1] == 0x41)) {
                            k--;
                        }
                        pop_seq = &post_stop[k];
                        break;
                    }
                }

                if (pop_seq && (pop_seq - post_stop) >= 17) {
                    MachInfo::setKernelWriting(true, KernelPatcher::kernelWriteLock);

                    post_stop[0] = 0x48;
                    post_stop[1] = 0xB8;
                    *reinterpret_cast<uint64_t *>(&post_stop[2]) = reinterpret_cast<uint64_t>(&g_guard_calls);

                    post_stop[10] = 0x48;
                    post_stop[11] = 0xFF;
                    post_stop[12] = 0x00;

                    post_stop[13] = 0x31;
                    post_stop[14] = 0xC0;

                    int32_t jmp_offset = static_cast<int32_t>(pop_seq - (post_stop + 17));
                    if (jmp_offset >= -128 && jmp_offset <= 127) {
                        post_stop[15] = 0xEB;
                        post_stop[16] = static_cast<uint8_t>(jmp_offset);
                        memset(&post_stop[17], 0x90, pop_seq - &post_stop[17]);
                    } else {
                        post_stop[15] = 0xE9;
                        int32_t rel32 = static_cast<int32_t>(pop_seq - (post_stop + 20));
                        *reinterpret_cast<int32_t *>(&post_stop[16]) = rel32;
                        memset(&post_stop[20], 0x90, pop_seq - &post_stop[20]);
                    }

                    MachInfo::setKernelWriting(false, KernelPatcher::kernelWriteLock);
                    rbgpu_log("Trap 3 Guard: successfully patched start() failure path (jumped to %p)", pop_seq);
                    rbgpu_diag_set_num("guard_routed", 1);
                    patched = true;
                    break;
                }
            }
        }
    }

    if (!patched) {
        rbgpu_diag_set_num("guard_routed", 0);
        rbgpu_log("Trap 3 Guard: failed to locate or patch stop() call site in start()");
    }

    // 2bm: Hook AMDRTHardware::allocateMemoryResources (vtable[0x120]).
    // This returns false, causing "Failed to init HW Acelerator resources".
    KernelPatcher::RouteRequest req_alloc(
        "__ZN28AMDRadeonX6000_AMDRTHardware23allocateMemoryResourcesEv",
        hooked_AMDRTHardware_allocateMemoryResources,
        orig_amdrt_allocate_memory_resources
    );
    if (patcher.routeMultiple(id, &req_alloc, 1, address, size)) {
        rbgpu_log("X6000: hooked AMDRTHardware::allocateMemoryResources");
    } else {
        rbgpu_log("X6000: failed to hook allocateMemoryResources");
    }

    // 2bn: Hook AMDHWMemory::reserve to diagnose display_mode_did_change failure.
    // Per the assistant's analysis, set_display_mode_and_vram calls reserve() for each
    // display pipe and it fails. Log-only, no behavior change.
    KernelPatcher::RouteRequest req_reserve(
        "__ZN26AMDRadeonX6000_AMDHWMemory7reserveEP16AMDMemoryElementyyb15_eOP_ORIGINATOR",
        hooked_AMDHWMemory_reserve,
        orig_amdhw_reserve
    );
    if (patcher.routeMultiple(id, &req_reserve, 1, address, size)) {
        rbgpu_log("X6000: hooked AMDHWMemory::reserve (2bn diagnostic)");
    } else {
        rbgpu_log("X6000: failed to hook AMDHWMemory::reserve");
    }

    // 2br: powerUpHW and the two calls before reserveNDRVSpace that can fail.
    KernelPatcher::RouteRequest req_pw[] = {
        {"__ZN37AMDRadeonX6000_AMDGraphicsAccelerator9powerUpHWEv", hooked_accel_powerUpHW, orig_accel_powerUpHW},
        {"__ZN32AMDRadeonX6000_AMDNavi21Hardware7powerUpEv", hooked_navi21_powerUp, orig_navi21_powerUp},
        {"__ZN26AMDRadeonX6000_AMDHWMemory17enableAllocationsEv", hooked_hwmem_enableAllocations, orig_hwmem_enableAllocations},
        // 2cn: Survive mode - fail start cleanly when powerUpHW fails.
        {"__ZN37AMDRadeonX6000_AMDGraphicsAccelerator5startEP9IOService", hooked_accel_start_2cn, orig_accel_start_2cn},
    };
    if (patcher.routeMultiple(id, req_pw, arrsize(req_pw), address, size)) {
        rbgpu_log("X6000: hooked powerUpHW, Navi21Hardware::powerUp, enableAllocations (2br)");
    } else {
        rbgpu_log("X6000: failed to hook the powerUpHW chain (2br)");
    }

    // 2bs: Hook powerUpHWEngines/startHWEngines to find which engine fails.
    // Try both AMDHardware (base) and AMDGFX10Hardware (GFX10-specific).
    // Done individually so a missing symbol doesn't break the others.
    KernelPatcher::RouteRequest req_pue1(
        "__ZN26AMDRadeonX6000_AMDHardware16powerUpHWEnginesEv",
        hooked_hw_powerUpHWEngines,
        orig_hw_powerUpHWEngines
    );
    if (patcher.routeMultiple(id, &req_pue1, 1, address, size)) {
        rbgpu_log("X6000: hooked AMDHardware::powerUpHWEngines (2bs)");
    } else {
        rbgpu_log("X6000: AMDHardware::powerUpHWEngines not found (2bs)");
    }

    KernelPatcher::RouteRequest req_she1(
        "__ZN26AMDRadeonX6000_AMDHardware14startHWEnginesEv",
        hooked_hw_startHWEngines,
        orig_hw_startHWEngines
    );
    if (patcher.routeMultiple(id, &req_she1, 1, address, size)) {
        rbgpu_log("X6000: hooked AMDHardware::startHWEngines (2bs)");
    } else {
        rbgpu_log("X6000: AMDHardware::startHWEngines not found (2bs)");
    }

    KernelPatcher::RouteRequest req_pue2(
        "__ZN31AMDRadeonX6000_AMDGFX10Hardware16powerUpHWEnginesEv",
        hooked_gfx10_powerUpHWEngines,
        orig_gfx10_powerUpHWEngines
    );
    if (patcher.routeMultiple(id, &req_pue2, 1, address, size)) {
        rbgpu_log("X6000: hooked AMDGFX10Hardware::powerUpHWEngines (2bs)");
    } else {
        rbgpu_log("X6000: AMDGFX10Hardware::powerUpHWEngines not found (2bs)");
    }

    KernelPatcher::RouteRequest req_she2(
        "__ZN31AMDRadeonX6000_AMDGFX10Hardware14startHWEnginesEv",
        hooked_gfx10_startHWEngines,
        orig_gfx10_startHWEngines
    );
    if (patcher.routeMultiple(id, &req_she2, 1, address, size)) {
        rbgpu_log("X6000: hooked AMDGFX10Hardware::startHWEngines (2bs)");
    } else {
        rbgpu_log("X6000: AMDGFX10Hardware::startHWEngines not found (2bs)");
    }

    // 2bt: Hook AMDHardware::powerUp and AMDGFX10Hardware::powerUp to trace
    // the call chain down from Navi21Hardware::powerUp.
    KernelPatcher::RouteRequest req_pu1(
        "__ZN26AMDRadeonX6000_AMDHardware7powerUpEv",
        hooked_hw_powerUp,
        orig_hw_powerUp
    );
    if (patcher.routeMultiple(id, &req_pu1, 1, address, size)) {
        rbgpu_log("X6000: hooked AMDHardware::powerUp (2bt)");
    } else {
        rbgpu_log("X6000: AMDHardware::powerUp not found (2bt)");
    }

    KernelPatcher::RouteRequest req_pu2(
        "__ZN31AMDRadeonX6000_AMDGFX10Hardware7powerUpEv",
        hooked_gfx10_powerUp,
        orig_gfx10_powerUp
    );
    if (patcher.routeMultiple(id, &req_pu2, 1, address, size)) {
        rbgpu_log("X6000: hooked AMDGFX10Hardware::powerUp (2bt)");
    } else {
        rbgpu_log("X6000: AMDGFX10Hardware::powerUp not found (2bt)");
    }

    // 2bu: Hook AMDGFX10PM4Engine::powerUp - the graphics/compute engine.
    KernelPatcher::RouteRequest req_pm4(
        "__ZN32AMDRadeonX6000_AMDGFX10PM4Engine7powerUpEv",
        hooked_pm4_powerUp,
        orig_pm4_powerUp
    );
    if (patcher.routeMultiple(id, &req_pm4, 1, address, size)) {
        rbgpu_log("X6000: hooked AMDGFX10PM4Engine::powerUp (2bu)");
    } else {
        rbgpu_log("X6000: AMDGFX10PM4Engine::powerUp not found (2bu)");
    }

    // 2cb: Replace AMDHardware::allocateMemoryResources with a fully instrumented
    // step-by-step version + channel drill-down (per the assistant option 2).
    KernelPatcher::RouteRequest req_hw_alloc(
        "__ZN26AMDRadeonX6000_AMDHardware23allocateMemoryResourcesEv",
        hooked_hw_allocateMemoryResources_2cb,
        orig_hw_allocateMemoryResources
    );
    if (patcher.routeMultiple(id, &req_hw_alloc, 1, address, size)) {
        rbgpu_log("X6000: hooked AMDHardware::allocateMemoryResources (2cb)");
    } else {
        rbgpu_log("X6000: AMDHardware::allocateMemoryResources not found (2bz)");
    }
    // 2cc: Stub the VCN encode channel allocation (VCN fw missing).
    KernelPatcher::RouteRequest req_vcn_alloc(
        "__ZN32AMDRadeonX6000_AMDVCN2EncChannel23allocateMemoryResourcesEv",
        hooked_vcn2enc_alloc_2cc,
        orig_vcn2enc_alloc_2cc
    );
    if (patcher.routeMultiple(id, &req_vcn_alloc, 1, address, size)) {
        rbgpu_log("X6000: stubbed AMDVCN2EncChannel::allocateMemoryResources (2cc)");
    } else {
        rbgpu_log("X6000: FAILED to stub AMDVCN2EncChannel::allocateMemoryResources (2cc)");
    }
    // 2cd: Instrument initComputeMQD to find why engine 0's powerUp fails.
    KernelPatcher::RouteRequest req_mqd(
        "__ZN32AMDRadeonX6000_AMDGFX10PM4Engine14initComputeMQDE18_eAMD_HW_RING_TYPE",
        hooked_pm4_initComputeMQD_2cd,
        orig_pm4_initComputeMQD_2cd
    );
    if (patcher.routeMultiple(id, &req_mqd, 1, address, size)) {
        rbgpu_log("X6000: hooked AMDGFX10PM4Engine::initComputeMQD (2cd)");
    } else {
        rbgpu_log("X6000: FAILED to hook initComputeMQD (2cd)");
    }
    // 2ce: powerUp is already routed by 2bu above (its logging is merged there);
    // a second routeMultiple on the same function panics Lilu.
    KernelPatcher::RouteRequest req_ds(
        "__ZN32AMDRadeonX6000_AMDGFX10PM4Engine7doStartEb",
        hooked_pm4_doStart_2ce,
        orig_pm4_doStart_2ce
    );
    if (patcher.routeMultiple(id, &req_ds, 1, address, size)) {
        rbgpu_log("X6000: hooked AMDGFX10PM4Engine::doStart (2ce)");
    } else {
        rbgpu_log("X6000: FAILED to hook PM4 doStart (2ce)");
    }
    // 2ch: Hook startKIQ (doStart calls it after initComputeMQD).
    KernelPatcher::RouteRequest req_kiq(
        "__ZN35AMDRadeonX6000_AMDGFX10KIQHWChannel8startKIQEyyPK18AMD_PM4_QUEUE_SPECPj",
        hooked_kiq_startKIQ_2co,
        orig_kiq_startKIQ_2ch
    );
    if (patcher.routeMultiple(id, &req_kiq, 1, address, size)) {
        rbgpu_log("X6000: hooked AMDGFX10KIQHWChannel::startKIQ (2co observe-only)");
    } else {
        rbgpu_log("X6000: FAILED to hook startKIQ (2ch)");
    }

    // 2cp: bound waitForActiveValue. doStart calls it as (ring 0, false, 1, 0),
    // and with that 4th argument 0 its own timeout counter and its 100 us
    // delay are both dead code (X6000 0xbea15f4-0xbea1615): it re-reads
    // CP_GFX_HQD_ACTIVE as fast as it can and only ever leaves the loop when
    // the register reads 1 or 0xffffffff. A GFX ring that stays inactive while
    // the register is readable as 0 is an unbounded spin with no panic and no
    // log. Forcing the 4th argument to 1 restores Apple's own 1000 x 100 us
    // bound, so a dead ring becomes a logged false instead of a hung CPU.
    // This is a bounded wait, not a bypass. docs/SPEC-PM4-DOSTART.md section 4.
    KernelPatcher::RouteRequest req_wfav(
        "__ZN32AMDRadeonX6000_AMDGFX10PM4Engine18waitForActiveValueE18_eAMD_HW_RING_TYPEbb",
        hooked_pm4_waitForActiveValue_2cp,
        orig_pm4_waitForActiveValue_2cp
    );
    if (patcher.routeMultiple(id, &req_wfav, 1, address, size)) {
        rbgpu_log("X6000: hooked AMDGFX10PM4Engine::waitForActiveValue (2cp bounded)");
    } else {
        rbgpu_log("X6000: FAILED to hook waitForActiveValue (2cp) - HANG RISK on the "
                  "doStart late path");
    }

    // 2cq: log the remaining doStart gates after startKIQ. All read-only.
    KernelPatcher::RouteRequest reqs_late[] = {
        KernelPatcher::RouteRequest(
            "__ZN35AMDRadeonX6000_AMDGFX10KIQHWChannel24submitSetResourcesPacketEPK18AMD_PM4_QUEUE_SPEC",
            hooked_kiq_submitSetResources_2cq,
            orig_kiq_submitSetResources_2cq
        ),
        KernelPatcher::RouteRequest(
            "__ZN32AMDRadeonX6000_AMDGFX10PM4Engine15initGraphicsMQDEv",
            hooked_pm4_initGraphicsMQD_2cq,
            orig_pm4_initGraphicsMQD_2cq
        ),
        KernelPatcher::RouteRequest(
            "__ZN35AMDRadeonX6000_AMDGFX10KIQHWChannel21submitMapQueuesPacketE18_eAMD_HW_RING_TYPEjyPK18AMD_PM4_QUEUE_SPECy",
            hooked_kiq_submitMapQueues_2cq,
            orig_kiq_submitMapQueues_2cq
        ),
        // 2cx: the actual wait for the GPU to execute a packet.
        KernelPatcher::RouteRequest(
            "__ZN27AMDRadeonX6000_AMDHWChannel14waitForHwStampEj",
            hooked_hwchannel_waitForHwStamp_2cx,
            orig_hwchannel_waitForHwStamp_2cx
        ),
    };
    for (size_t i = 0; i < arrsize(reqs_late); i++) {
        if (patcher.routeMultiple(id, &reqs_late[i], 1, address, size)) {
            rbgpu_log("X6000: hooked doStart late gate %zu (2cq)", i);
        } else {
            rbgpu_log("X6000: FAILED to hook doStart late gate %zu (2cq)", i);
        }
    }
}

// 2ct (2026-10-12): headless accelerator - adopt no display pipe.
//
// The problem this solves: on an armed boot the accelerator loads, powerUpHW
// fails, and then WindowServer never gets a desktop. The chain is
//   IOFramebuffer::open
//     -> IOAccelDisplayMachine::display_mode_did_change      (family 0x14e2148e)
//        -> AMDAccelDisplayMachine::displayModeDidChange     (X6000 0xbe48428)
//           -> set_display_mode_and_vram                     (X6000 0xbe48604)
// and set_display_mode_and_vram returns false in exactly one place: the loop
// over adopted display pipes, where it tries to reserve the boot
// framebuffer's range in a VRAM allocator that is empty because powerUpHW
// never ran. False there used to panic; with -rbgpunodisppanic it just fails
// the framebuffer open, which is why the screen stays on the boot console.
//
// With ZERO adopted pipes that loop has nothing to fail on, so
// set_display_mode_and_vram returns true, display_mode_did_change takes its
// success path (family 0x14e2153f: pipe count 0 -> 0x14e2160c -> return
// true), IOFramebuffer::open completes, and the desktop comes up on
// IONDRVFramebuffer as it does on an unarmed boot. The accelerator still
// loads and still runs powerUp/doStart, so the gate diagnostics are
// unaffected - it simply drives no display, which is the honest state here.
//
// How: IOAccelDisplayMachine::found_framebuffer(IOFramebuffer*) is what
// adopts a framebuffer as pipe. Apple's own code already refuses when the
// pipe count has reached accel+0xcb0 (1, from
// IOGraphicsAccelerator2::initializeConfigStructure 0x14e4d7bf): it logs and
// returns false at 0x14e21287 without creating a pipe. Returning false from
// this hook takes exactly that supported path.
//
// Note on what NOT to do: the factory result at 0x14e2125d is stored into the
// pipe array with no NULL check and the count is incremented regardless
// (0x14e21263-0x14e2127d), so failing the pipe constructor or returning NULL
// from the factory would leave a NULL pipe behind and crash on the next pipe
// walk. The refusal has to happen in found_framebuffer, before any of that.
//
// -rbgpudisplaypipe restores Apple's behaviour (adopt the framebuffer), for a
// boot where we want the old path back.
static bool (*orig_accel_found_framebuffer_2ct)(void *, void *) = nullptr;
static volatile int g_rb_2ct_found_fb_calls = 0;
static volatile int g_rb_2ct_found_fb_refused = 0;

static bool hooked_accel_found_framebuffer_2ct(void *machine, void *framebuffer) {
    g_rb_2ct_found_fb_calls++;
    if (checkKernelArgument("-rbgpudisplaypipe")) {
        bool ret = orig_accel_found_framebuffer_2ct
                       ? orig_accel_found_framebuffer_2ct(machine, framebuffer)
                       : false;
        rbgpu_log("2ct: found_framebuffer(fb=%p) passed through (-rbgpudisplaypipe) -> %d",
                  framebuffer, ret ? 1 : 0);
        return ret;
    }
    g_rb_2ct_found_fb_refused++;
    rbgpu_diag_set_num("found_fb_calls", static_cast<uint64_t>(g_rb_2ct_found_fb_calls));
    rbgpu_diag_set_num("found_fb_refused", static_cast<uint64_t>(g_rb_2ct_found_fb_refused));
    rbgpu_log("2ct: found_framebuffer(fb=%p) REFUSED -> accelerator adopts no display "
              "pipe, desktop stays on IONDRVFramebuffer", framebuffer);
    return false;
}

void RGPU::processAccelFamily(KernelPatcher &patcher, size_t id, mach_vm_address_t address,
                              size_t size) {
    KernelPatcher::RouteRequest req(
        "__ZN21IOAccelDisplayMachine17found_framebufferEP13IOFramebuffer",
        hooked_accel_found_framebuffer_2ct,
        orig_accel_found_framebuffer_2ct
    );
    if (patcher.routeMultiple(id, &req, 1, address, size)) {
        rbgpu_log("AccelFamily: hooked IOAccelDisplayMachine::found_framebuffer (2ct headless)");
        rbgpu_diag_set_num("found_fb_routed", 1);
    } else {
        rbgpu_log("AccelFamily: FAILED to hook found_framebuffer (2ct) - an armed boot will "
                  "not reach a desktop");
        rbgpu_diag_set_num("found_fb_routed", 0);
    }
}

void RGPU::processHWServices(KernelPatcher &patcher, size_t id, mach_vm_address_t address, size_t size) {
    rbgpu_log("com.apple.kext.AMDRadeonX6000HWServices matched and monitored");
}

