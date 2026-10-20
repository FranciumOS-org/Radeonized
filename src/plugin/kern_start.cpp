#include <Headers/plugin_start.hpp>
#include <Headers/kern_api.hpp>
#include <Headers/kern_util.hpp>
#include <Headers/kern_patcher.hpp>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>

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

// 2jp: the RLC sub-image substitution is opt-in, and resolved LAZILY at the
// first _IpiSetFwEntry rather than from an init hook, because the entries
// are registered before anything of ours would get a chance to run an
// initialiser. Absence is not a result (2eh): it logs either way, so a boot
// where the arg is off cannot be mistaken for one where the swap failed.
static int g_rb_2jp_on = -1;
static bool rbgpu_2jp_enabled(void) {
    if (g_rb_2jp_on < 0) {
        g_rb_2jp_on = checkKernelArgument("-rbgpurlcsub") ? 1 : 0;
        rbgpu_log("2jp: RLC sub-image substitution %s - ids 26/27/28/29/30, "
                  "the srlist cntl/gpm/srm lists and the LX6 iram/dram that "
                  "2aj left as Apple's Navi21 images",
                  g_rb_2jp_on ? "ON" : "OFF (-rbgpurlcsub not set)");
        rbgpu_diag_set_num("rlcsub_on", static_cast<uint32_t>(g_rb_2jp_on));
    }
    return g_rb_2jp_on == 1;
}
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

// 2dw: the fence slot itself, carried across the wait for the same reason.
static uint32_t g_rb_2dw_slot_idx   = 0;
static uint64_t g_rb_2dw_slot_b0    = 0;
static uint32_t g_rb_2dw_fence_bef  = 0;
static uint64_t g_rb_2dt_wb_c8      = 0;  // 2dz: the fence destination
static uint32_t g_rb_2ea_pq_base    = 0;  // 2ea: CP_HQD_PQ_BASE (ring VA >> 8)
static uint64_t g_rb_2eb_pt_base    = 0;  // 2eb: GCVM_CONTEXT0_PAGE_TABLE_BASE
static uint64_t g_rb_2eb_start_pg   = 0;  // 2ek: CONTEXT0 PAGE_TABLE_START (pages)
static uint64_t g_rb_2eb_end_pg     = 0;  // 2ek: CONTEXT0 PAGE_TABLE_END (pages)
static uint32_t g_rb_2ed_fb_off     = 0;  // 2ed: GCMC_VM_FB_OFFSET (16 MB units)
static uint32_t g_rb_2ed_fb_base    = 0;  // 2em: GCMC_VM_FB_LOCATION_BASE (16 MB units)
static int      g_rb_2em_done       = 0;  // 2em: PT_BASE patch, latched on success
static int      g_rb_2ee_pdb_done   = 0;  // 2ee: map the candidates once per boot
static uint64_t g_rb_gart_va        = 0;  // 2el: kernel VA of the flat PTE table
static uint64_t g_rb_gart_mc        = 0;  // 2em: carve-out MC of the same table
static uint64_t g_rb_gart_limit     = 0;  // 2el: bytes of it both sizes agree on
static int      g_rb_2ek_done       = 0;  // 2el: ring-PTE read, latched on success
static uint32_t g_rb_2ek_valid      = 0;  // 2el: valid entries the scan counted
static void    *g_rb_ttl_this       = nullptr;  // 2eh: the AmdTtlServices object
// 2eh: _ttl_get_gart_primary_page_table_info(ttl_handle, out[5]) - the
// authoritative source for where the GART primary page table lives.
static uint32_t (*g_rb_ttl_gart_info)(void *, uint64_t *) = nullptr;
static int      g_rb_2eh_done       = 0;

// 2dx: AMDHardware::getChannelWriteBackFrameGPUAddr(uint) - the MC address the
// GPU is told for a writeback frame, the counterpart of the CPU pointer the
// channel polls. Resolved by symbol, called read-only.
typedef uint64_t (*wb_gpuaddr_t)(void *hw, uint32_t idx);
static wb_gpuaddr_t orig_wb_gpuaddr_2dx = nullptr;

// 2dy: AMDGFX10KIQHWChannel::writeDataCmdPacket(uint *) - the WRITE_DATA
// packet that asks the GPU to write the completion fence. Observe only.
static void (*orig_kiq_writeDataCmd_2dy)(void *, uint32_t *) = nullptr;
static uint32_t g_rb_2dy_calls = 0;
static uint32_t g_rb_2dy_pkt[5] = {0, 0, 0, 0, 0};

static void hooked_kiq_writeDataCmd_2dy(void *ch, uint32_t *p) {
    if (orig_kiq_writeDataCmd_2dy)
        orig_kiq_writeDataCmd_2dy(ch, p);
    g_rb_2dy_calls++;
    if (!p)
        return;
    for (int i = 0; i < 5; i++)
        g_rb_2dy_pkt[i] = p[i];
    // The packet Apple just built, read back from the ring block itself.
    //   p[0] 0xC0033700  PM4 type 3, opcode 0x37 = WRITE_DATA, 5 dwords
    //   p[1] 0x00100500  DST_SEL 5 (memory), WR_CONFIRM
    //   p[2] destination low  = channel+0xc8 & ~3
    //   p[3] destination high = channel+0xcc
    //   p[4] data             = getCommandSubmitTimestamp()
    uint64_t dst = (static_cast<uint64_t>(p[3]) << 32) | p[2];
    uint64_t c8 = 0;
    if (ch)
        c8 = *reinterpret_cast<volatile uint64_t *>(
            reinterpret_cast<uintptr_t>(ch) + 0xc8);
    rbgpu_diag_set_num("fencepkt_calls", g_rb_2dy_calls);
    rbgpu_diag_set_num("fencepkt_hdr", p[0]);
    rbgpu_diag_set_num("fencepkt_ctl", p[1]);
    rbgpu_diag_set_num("fencepkt_dst", dst);
    rbgpu_diag_set_num("fencepkt_data", p[4]);
    rbgpu_diag_set_num("chan_c8", c8);
    rbgpu_log("2dy: WRITE_DATA fence packet #%u on ch=%p: hdr=0x%08x ctl=0x%08x "
              "dst=0x%llx data=%u ; channel+0xc8=0x%llx -> %s",
              g_rb_2dy_calls, ch, p[0], p[1], dst, p[4], c8,
              (dst == 0)
                  ? "*** DESTINATION IS ZERO: the GPU is told to write the "
                    "fence to address 0, so it can never reach the page the "
                    "CPU polls. +0xc8 is set in allocateMemoryResources, not "
                    "in init, so that is where to look"
              : ((dst >> 32) == 0xFF)
                  ? "destination is in the 0xFF_BF.. GPUVM region, the same one "
                    "the ring base lives in, so the address looks right"
                  : "destination is in neither the 0xFF_BF.. GPUVM region nor "
                    "zero, so it needs its own reading");
}
static int      g_rb_2dw_is_self    = 0;

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
    if (base != 0 && base != 0xFFFFFFFF)
        g_rb_2ea_pq_base = base;   // 2ea: the GPUVM ring base, >> 8
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

// 2eo: the same synchronous breadcrumb, inside the register dump. The 2en
// boot's NVRAM shows vmf_st_kiq set (the post-startKIQ dump had started) and
// vmf_st_pre never set (submitSetResourcesPacket was never entered), so the
// boot stopped somewhere INSIDE this function - and nothing says where. Each
// mark overwrites `rbgpu-gate`, so the surviving value names the last block
// this function finished.
// 2eq: the per-block dump marks are retired. The 2eo boot showed the dump
// runs to completion at every point (gate 40 follows it), so nine marks per
// dump point were answering a settled question at the cost of 45 synchronous
// NVRAM writes a boot. rbgpu_gate_nvram is still there if a dump stalls again.

static void rbgpu_dump_gc_regs(const char *tag);

// 2fe (2026-10-13): the three dump points AFTER post-startKIQ are retired by
// default. Every question they were added for is answered: the GPUVM fault is
// gone at at-stamp-wait (2em), the queue is live with RPTR=WPTR=0x20 at
// post-stamp-wait (2dq/2do), and the fault latch is clean before the doorbell
// (2ea). What they still do is read ~40 GC registers and write GRBM_GFX_CNTL
// inside the one span the trail has been stopping in - and a GC/SMN read that
// stalls is a documented non-deterministic hazard on this chip (the boot 6/7
// RSMU timeouts). This is the only hardware access in that span, so it is the
// only candidate left for an endpoint that varies between 45 and 101 on
// identical code. -rbgpuregdump puts them back.
static void rbgpu_dump_gc_regs_late(const char *tag) {
    static int checked = 0, on = 0;
    if (!checked) { checked = 1; on = checkKernelArgument("-rbgpuregdump") ? 1 : 0; }
    if (!on) {
        rbgpu_log("2fe: [%s] register dump SKIPPED (-rbgpuregdump not set). This is "
                  "the only hardware access in the span the trail stops in; the "
                  "questions it answered are recorded and it reads nothing new.",
                  tag);
        return;
    }
    rbgpu_dump_gc_regs(tag);
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

    // 2dz (2026-10-12): did the fence WRITE_DATA fault in GPUVM?
    //
    // 2dy proved the packet is correct in every field: WRITE_DATA (0xC0033700),
    // DST_SEL 5 memory with WR_CONFIRM (0x00100500), destination
    // 0xFFBFDC0040 = exactly the writeback frame whose CPU VA the driver polls,
    // data = the asked-for stamp. The CP consumed the block (2dq: RPTR = WPTR =
    // 0x20 with CP_STAT busy) and the dword never changed.
    //
    // This is Apple's shipping KIQ code and it works on Navi 21, so the packet
    // layout, the 0xFFFF1000 NOP padding and the writeback mechanism are sound
    // in principle. What is chip-specific is that the destination is a GPUVM
    // VIRTUAL address translated through the queue's VMID. The CP reads the
    // ring out of the same 0xFF_BF.. region, but a WRITE that misses the page
    // tables leaves RPTR advanced and the value never stored - which is
    // precisely what has been measured.
    //
    // All reads, all BASE_IDX 0 (seg0 = 0x1260, proven in 2dl).
    const uint32_t dwVM_FAULT_CNTL    = 0x15c4;
    const uint32_t dwVM_FAULT_STATUS  = 0x15c8;
    const uint32_t dwVM_FAULT_ADDR_LO = 0x15c9;
    const uint32_t dwVM_FAULT_ADDR_HI = 0x15ca;
    const uint32_t dwCP_CPC_STATUS    = 0x0e24;
    const uint32_t dwCP_CPF_STATUS    = 0x0e27;
    const uint32_t dwCPC_UTCL1_STATUS = 0x1f55;
    const uint32_t dwCPF_UTCL1_STATUS = 0x1f56;
    const uint32_t dwGRBM_STATUS2     = 0x0da2;

    uint32_t vm_cntl   = g_hw.rreg(g_hw.ctx, (kSeg0Conv + dwVM_FAULT_CNTL)    * 4);
    uint32_t vm_status = g_hw.rreg(g_hw.ctx, (kSeg0Conv + dwVM_FAULT_STATUS)  * 4);
    uint32_t vm_lo     = g_hw.rreg(g_hw.ctx, (kSeg0Conv + dwVM_FAULT_ADDR_LO) * 4);
    uint32_t vm_hi     = g_hw.rreg(g_hw.ctx, (kSeg0Conv + dwVM_FAULT_ADDR_HI) * 4);
    uint32_t cpc_stat  = g_hw.rreg(g_hw.ctx, (kSeg0Conv + dwCP_CPC_STATUS)    * 4);
    uint32_t cpf_stat  = g_hw.rreg(g_hw.ctx, (kSeg0Conv + dwCP_CPF_STATUS)    * 4);
    uint32_t cpc_utcl1 = g_hw.rreg(g_hw.ctx, (kSeg0Conv + dwCPC_UTCL1_STATUS) * 4);
    uint32_t cpf_utcl1 = g_hw.rreg(g_hw.ctx, (kSeg0Conv + dwCPF_UTCL1_STATUS) * 4);
    uint32_t grbm2     = g_hw.rreg(g_hw.ctx, (kSeg0Conv + dwGRBM_STATUS2)     * 4);

    rbgpu_diag_set_num("vmf_cntl", vm_cntl);
    rbgpu_diag_set_num("vmf_status", vm_status);
    rbgpu_diag_set_num("vmf_addr_lo", vm_lo);
    rbgpu_diag_set_num("vmf_addr_hi", vm_hi);
    rbgpu_diag_set_num("cp_cpc_status", cpc_stat);
    rbgpu_diag_set_num("cp_cpf_status", cpf_stat);
    rbgpu_diag_set_num("cpc_utcl1_status", cpc_utcl1);
    rbgpu_diag_set_num("cpf_utcl1_status", cpf_utcl1);
    rbgpu_diag_set_num("diag_grbm_status2", grbm2);

    rbgpu_log("2dz DIAG [%s]: GCVM_L2_PROTECTION_FAULT CNTL=0x%08x STATUS=0x%08x "
              "ADDR_HI=0x%08x LO=0x%08x", tag, vm_cntl, vm_status, vm_hi, vm_lo);
    rbgpu_log("2dz DIAG [%s]: CP_CPC_STATUS=0x%08x CP_CPF_STATUS=0x%08x "
              "CPC_UTCL1=0x%08x CPF_UTCL1=0x%08x GRBM_STATUS2=0x%08x",
              tag, cpc_stat, cpf_stat, cpc_utcl1, cpf_utcl1, grbm2);

    // What would have to be true for a zero STATUS to mean anything: fault
    // reporting has to be switched on. GCVM_L2_PROTECTION_FAULT_CNTL bit 0 is
    // ENABLE_INTERRUPTS in amdgpu's layout, and the register being entirely
    // zero means no class of fault is being reported at all - in which case a
    // zero STATUS says nothing whatever. Say so rather than letting it read
    // as "no fault".
    // 2eb (2026-10-12): is the ring address inside the VMID's mapped range?
    //
    // 2ea dated the fault: STATUS is 0 at post-gc-hw-init, post-startKIQ and
    // pre-kiq-submit, and becomes 0x9ba at at-stamp-wait - so the KIQ's own
    // submission raises it, and the "pre-existing fault" escape is closed.
    // Decoded, 0x9ba is WALKER_ERROR=5, PERMISSION_FAULTS=0xB,
    // MAPPING_ERROR=1, CID=4, RW=0: a READ whose page walk found NO VALID
    // MAPPING, at 0xFFBFE80000 = CP_HQD_PQ_BASE << 8, the ring base.
    //
    // 2do left exactly this thread hanging: PQ_BASE's GPUVM address
    // (0xFFBFE80000) is not the ring MC address startKIQ was handed
    // (0xf40fb28000), and there was no reason those should match. Now the
    // GPUVM address turns out not to translate to anything, so the question
    // is whether the page tables are supposed to cover it at all. The queue
    // runs at VMID 0 (measured in 2do), so GCVM_CONTEXT0_* is its context.
    //
    // All reads, BASE_IDX 0, from docs/linux-ref/gc_10_3_0_offset.h. If the
    // ring address lies outside [START, END] the answer is immediate; if it
    // lies inside, the range is right and the fault is in the page-table
    // contents rather than the aperture.
    {
        const uint32_t dwL2_CNTL   = 0x15bc;
        const uint32_t dwC0_CNTL   = 0x15fc;
        const uint32_t dwC0_PT_LO  = 0x1667, dwC0_PT_HI  = 0x1668;
        const uint32_t dwC0_ST_LO  = 0x1687, dwC0_ST_HI  = 0x1688;
        const uint32_t dwC0_EN_LO  = 0x16a7, dwC0_EN_HI  = 0x16a8;

        auto rd = [&](uint32_t dw) {
            return g_hw.rreg(g_hw.ctx, (kSeg0Conv + dw) * 4);
        };
        uint32_t l2c = rd(dwL2_CNTL), c0c = rd(dwC0_CNTL);
        uint64_t ptb = (static_cast<uint64_t>(rd(dwC0_PT_HI)) << 32) | rd(dwC0_PT_LO);
        uint64_t st  = (static_cast<uint64_t>(rd(dwC0_ST_HI)) << 32) | rd(dwC0_ST_LO);
        uint64_t en  = (static_cast<uint64_t>(rd(dwC0_EN_HI)) << 32) | rd(dwC0_EN_LO);
        uint64_t ring = static_cast<uint64_t>(g_rb_2ea_pq_base) << 8;

        rbgpu_diag_set_num("vm_l2_cntl", l2c);
        rbgpu_diag_set_num("vm_c0_cntl", c0c);
        rbgpu_diag_set_num("vm_c0_ptb", ptb);
        g_rb_2eb_pt_base = ptb;   // 2ee reads the page it points at
        rbgpu_diag_set_num("vm_c0_start", st);
        g_rb_2eb_start_pg = st;   // 2ek: the flat PTE table is indexed from here
        g_rb_2eb_end_pg   = en;
        rbgpu_diag_set_num("vm_c0_end", en);

        // START/END are page numbers in amdgpu's usage, so both readings are
        // printed and neither is asserted - the same rule as the fault addr.
        rbgpu_log("2eb DIAG [%s]: GCVM_L2_CNTL=0x%08x CONTEXT0_CNTL=0x%08x "
                  "PT_BASE=0x%llx START=0x%llx END=0x%llx "
                  "(as pages: 0x%llx..0x%llx) ; ring VA = 0x%llx",
                  tag, l2c, c0c, ptb, st, en, st << 12, en << 12, ring);
        if (ring != 0) {
            bool in_raw   = (ring >= st && ring <= en);
            bool in_pages = (ring >= (st << 12) && ring <= (en << 12));
            rbgpu_diag_set_num("vm_ring_in_range", (in_raw || in_pages) ? 1 : 0);
            rbgpu_log("2eb DIAG [%s]: ring VA 0x%llx vs CONTEXT0 range -> %s",
                      tag, ring,
                      (in_raw || in_pages)
                          ? "INSIDE the mapped range, so the aperture is right "
                            "and the fault is in the page-table contents, not "
                            "the range"
                          : "OUTSIDE the mapped range under either reading: the "
                            "ring GPUVM address the MQD was given is not covered "
                            "by this VMID's page tables at all");
        }
    }

    // 2ed (2026-10-12): did Apple program the GC hub's apertures at all?
    //
    // The fault decodes as MAPPING_ERROR=1 / WALKER_ERROR=5, so the walk RAN
    // and found no valid PTE - which means the walker had a page-table base
    // to start from. What it does not say is whether the surrounding GFXHUB
    // aperture setup is there. GC and MMHUB are separate VM hubs on GFX10
    // (Linux: gfxhub_v2_1 and mmhub_v2_3, programmed independently), and 2di
    // has already read MMHUB's copy as a healthy FB_LOCATION 0xF400/0xF4FF -
    // that hub is driving the display, so it proves nothing about this one.
    //
    // Reading the GC-side copies costs one more dword each in the same window
    // and says whether the GFXHUB was set up at all:
    //
    //   all zero       the GC hub's apertures were never programmed; Apple's
    //                  Navi 21 path set up MMHUB and left GFXHUB alone, and
    //                  the unmapped ring is a symptom of that, not the cause
    //   FB matches     the GC hub agrees with MMHUB about where the carve-out
    //                  is, so the aperture setup is not the problem and the
    //                  fault really is in the page-table contents
    //
    // Neither reading is asserted in the log. All reads, BASE_IDX 0, offsets
    // from docs/linux-ref/gc_10_3_0_offset.h (3638-3690).
    {
        auto rd = [&](uint32_t dw) {
            return g_hw.rreg(g_hw.ctx, (kSeg0Conv + dw) * 4);
        };
        uint32_t fb_base = rd(0x16fc);   // GCMC_VM_FB_LOCATION_BASE
        uint32_t fb_top  = rd(0x16fd);   // GCMC_VM_FB_LOCATION_TOP
        uint32_t fb_off  = rd(0x16e7);   // GCMC_VM_FB_OFFSET
        uint32_t agp_bot = rd(0x16ff);   // GCMC_VM_AGP_BOT
        uint32_t agp_top = rd(0x16fe);   // GCMC_VM_AGP_TOP
        uint32_t sys_lo  = rd(0x1701);   // GCMC_VM_SYSTEM_APERTURE_LOW_ADDR
        uint32_t sys_hi  = rd(0x1702);   // GCMC_VM_SYSTEM_APERTURE_HIGH_ADDR

        rbgpu_diag_set_num("gc_fb_base", fb_base);
        g_rb_2ed_fb_base = fb_base;   // 2em
        rbgpu_diag_set_num("gc_fb_top", fb_top);
        rbgpu_diag_set_num("gc_fb_off", fb_off);
        g_rb_2ed_fb_off = fb_off;   // 2ee: vram_base_offset = fb_off << 24
        rbgpu_diag_set_num("gc_agp_bot", agp_bot);
        rbgpu_diag_set_num("gc_agp_top", agp_top);
        rbgpu_diag_set_num("gc_sys_lo", sys_lo);
        rbgpu_diag_set_num("gc_sys_hi", sys_hi);

        bool all_zero = (fb_base | fb_top | fb_off | agp_bot | agp_top |
                         sys_lo | sys_hi) == 0;
        bool fb_agrees = (fb_base == 0xf400) && (fb_top == 0xf4ff);
        rbgpu_diag_set_num("gc_hub_all_zero", all_zero ? 1 : 0);
        rbgpu_diag_set_num("gc_hub_fb_agrees", fb_agrees ? 1 : 0);

        rbgpu_log("2ed DIAG [%s]: GCMC FB_LOCATION base=0x%08x top=0x%08x "
                  "FB_OFFSET=0x%08x AGP bot=0x%08x top=0x%08x SYS lo=0x%08x hi=0x%08x",
                  tag, fb_base, fb_top, fb_off, agp_bot, agp_top, sys_lo, sys_hi);
        rbgpu_log("2ed DIAG [%s]: GC hub all_zero=%d fb_agrees_with_mmhub=%d "
                  "(MMHUB read 0xf400/0xf4ff in 2di; the two hubs are programmed "
                  "independently, so MMHUB being healthy says nothing here)",
                  tag, all_zero ? 1 : 0, fb_agrees ? 1 : 0);
    }

    // 2ef helper: map one physical page read-only and characterise it.
    // Returns false if the mapping failed, in which case the counts are
    // meaningless and the caller must say so.
    auto scan_phys = [](uint64_t phys, uint32_t *valid, uint32_t *nonzero,
                        uint32_t *low, uint32_t *high, uint64_t *first4) -> bool {
        *valid = *nonzero = *low = *high = 0;
        first4[0] = first4[1] = first4[2] = first4[3] = 0;
        auto md = IOMemoryDescriptor::withPhysicalAddress(phys, 0x1000, kIODirectionIn);
        if (!md) return false;
        IOMemoryMap *map = md->createMappingInTask(kernel_task, 0,
                                                   kIOMapAnywhere | kIOMapInhibitCache);
        if (!map) { md->release(); return false; }
        auto p = reinterpret_cast<volatile uint64_t *>(map->getVirtualAddress());
        for (uint32_t i = 0; i < 512; i++) {
            uint64_t e = p[i];
            if (i < 4) first4[i] = e;
            if (e != 0) (*nonzero)++;
            if (e & 1) {
                (*valid)++;
                uint64_t a = e & 0x0000FFFFFFFFF000ULL;
                if (a < 0x10000000ULL) (*low)++; else (*high)++;
            }
        }
        map->release();
        md->release();
        return true;
    };

    // 2ee (2026-10-12): WHERE is the page directory really, and are the PTEs
    // wrong the same way?
    //
    // 2eb/2ed settled that the ring VA is inside VMID 0's range and that the
    // GC hub's apertures are correct, so the fault is in the page-table
    // CONTENTS. The contents start at GCVM_CONTEXT0_PAGE_TABLE_BASE_ADDR,
    // which this boot read as 0x0FDFA001 with the high word 0.
    //
    // Linux says what that register should hold (fetched 2026-10-12):
    //   gfxhub_v2_1_init_gart_aperture_regs -> gfxhub_v2_1_setup_vm_pt_regs
    //     writes lower/upper_32_bits(amdgpu_gmc_pd_addr(gart.bo))
    //   amdgpu_gmc_pd_addr -> amdgpu_gmc_get_pde_for_bo -> gmc_v10_0_get_vm_pde:
    //     for VRAM (no AMDGPU_PTE_SYSTEM) *addr = amdgpu_gmc_vram_mc2pa(*addr)
    //   amdgpu_gmc_vram_mc2pa(mc) = mc - vram_start + vram_base_offset
    //   vram_base_offset = gfxhub_v2_1_get_mc_fb_offset()
    //                    = GCMC_VM_FB_OFFSET << 24
    //
    // We read GCMC_VM_FB_OFFSET = 0x780 this boot (2ed), so vram_base_offset
    // should be 0x7_8000_0000 - the carve-out's system-physical base, which
    // probe 2 measured independently through MMHUB. Apple's value has the low
    // bit (AMDGPU_PTE_VALID) set, bit 1 (AMDGPU_PTE_SYSTEM) CLEAR - so it is
    // declaring a page directory in VRAM - yet carries no 0x7_8 high bits.
    // That is mc2pa computed with vram_base_offset = 0, which is correct for
    // a Navi 21 dGPU (FB_OFFSET 0) and wrong by exactly 0x7_8000_0000 here.
    //
    // DO NOT patch the register on the strength of that. If Apple's whole VM
    // layer uses offset 0, the PTEs are low by the same amount, and making
    // the walk succeed would point GPU reads AND WRITES at host DRAM around
    // 0x0FDF_A000 instead of at the carve-out. That is the one outcome worse
    // than the current clean fault.
    //
    // So read both candidate pages first. This is read-only: map each
    // physical page in, read the first entries, unmap. A GFX10 PDE is 8
    // bytes with bit 0 = valid.
    //
    //   candidate A = what the GPU is told          (0x0FDFA000)
    //   candidate B = A + (GCMC_VM_FB_OFFSET << 24) (0x7_8FDFA000)
    //
    // What each outcome means, stated before the reading is taken:
    //   B holds valid entries, A does not   the directory really is in the
    //       carve-out and the register is short by vram_base_offset
    //   entries in B point LOW (< 0x1000_0000)   the PTEs carry the same zero
    //       offset, so the bug is systemic and fixing PT_BASE alone would be
    //       dangerous, not sufficient
    //   entries in B point into 0x7_8.. or the carve-out   only the register
    //       is wrong and patching it is the whole fix
    //   neither looks like a directory    this reading proves nothing and the
    //       page-directory location is still unknown
    if (g_rb_2ee_pdb_done == 0 && g_rb_2ed_fb_off != 0) {
        g_rb_2ee_pdb_done = 1;
        uint64_t pdb_raw = g_rb_2eb_pt_base & ~0xFFFULL;
        uint64_t cand[2] = {
            pdb_raw,
            pdb_raw + (static_cast<uint64_t>(g_rb_2ed_fb_off) << 24),
        };
        for (int c = 0; c < 2; c++) {
            uint32_t valid, nonzero, low, high;
            uint64_t first[4];
            bool mapped = scan_phys(cand[c], &valid, &nonzero, &low, &high, first);

            rbgpu_log("2ee DIAG [%s]: PDB candidate %c phys=0x%llx -> %s"
                      " valid=%u nonzero=%u low(<0x10000000)=%u high=%u"
                      " [0]=0x%llx [1]=0x%llx [2]=0x%llx [3]=0x%llx",
                      tag, c == 0 ? 'A' : 'B', cand[c],
                      mapped ? "mapped" : "MAP FAILED (reading proves nothing)",
                      valid, nonzero, low, high,
                      first[0], first[1], first[2], first[3]);

            const char *pfx = c == 0 ? "pdbA" : "pdbB";
            char key[32];
            snprintf(key, sizeof(key), "%s_valid", pfx);   rbgpu_diag_set_num(key, valid);
            snprintf(key, sizeof(key), "%s_nonzero", pfx); rbgpu_diag_set_num(key, nonzero);
            snprintf(key, sizeof(key), "%s_low", pfx);     rbgpu_diag_set_num(key, low);
            snprintf(key, sizeof(key), "%s_high", pfx);    rbgpu_diag_set_num(key, high);
            snprintf(key, sizeof(key), "%s_e0", pfx);      rbgpu_diag_set_num(key, first[0]);
            snprintf(key, sizeof(key), "%s_phys", pfx);    rbgpu_diag_set_num(key, cand[c]);
            snprintf(key, sizeof(key), "%s_mapped", pfx);  rbgpu_diag_set_num(key, mapped ? 1 : 0);
        }

        // 2ef (2026-10-12): THE CONTROL. The 2ee boot read candidate B as 512
        // zero qwords, and that only means "no page directory here" if reading
        // carve-out physical memory works at all. Nothing has ever tested
        // that: every carve-out read this project has done went through BAR0
        // in the probe kext, not through IOMemoryDescriptor on the
        // system-physical alias.
        //
        // Probe 2 measured the control: MMMC_VM_FB_OFFSET 0x780 puts the
        // carve-out at system-physical 0x7_8000_0000, and HUBP0 scans the GOP
        // framebuffer from carve-out offset 0 - which is the console this
        // machine is displaying, so it cannot be a page of zeros.
        //
        //   framebuffer page nonzero   carve-out reads work, and candidate B
        //                              really holds no page directory
        //   framebuffer page zero      the instrument does not read carve-out
        //                              memory and the 2ee result proves
        //                              NOTHING either way
        {
            uint64_t fb_phys = static_cast<uint64_t>(g_rb_2ed_fb_off) << 24;
            uint32_t valid, nonzero, low, high;
            uint64_t first[4];
            bool mapped = scan_phys(fb_phys, &valid, &nonzero, &low, &high, first);
            rbgpu_diag_set_num("ctl_fb_phys", fb_phys);
            rbgpu_diag_set_num("ctl_fb_mapped", mapped ? 1 : 0);
            rbgpu_diag_set_num("ctl_fb_nonzero", nonzero);
            rbgpu_diag_set_num("ctl_fb_e0", first[0]);
            rbgpu_log("2ef DIAG [%s]: CONTROL framebuffer page phys=0x%llx -> %s "
                      "nonzero=%u/512 [0]=0x%llx [1]=0x%llx -- %s",
                      tag, fb_phys, mapped ? "mapped" : "MAP FAILED",
                      nonzero, first[0], first[1],
                      (mapped && nonzero > 0)
                          ? "carve-out reads WORK, so pdbB's zeros are real"
                          : "carve-out reads do NOT work here; the 2ee result "
                            "proves nothing and must not be used");
        }

        rbgpu_log("2ee DIAG [%s]: a page of zeros or of noise is NOT a page "
                  "directory; only 'B valid and A not' supports the "
                  "vram_base_offset reading, and where B's entries POINT "
                  "decides whether the PTEs carry the same error.", tag);
    }

    // 2eh (2026-10-12): ASK THE TTL LAYER where the GART page table is.
    //
    // 2ef's control came back decisive and negative: the GOP framebuffer page
    // at system-physical 0x7_8000_0000 read as 512 zero qwords while the
    // console was displaying on it, and in ARGB8888 even black is 0xFF000000.
    // So IOMemoryDescriptor maps carve-out memory and does not READ it, and
    // 2ee's "candidate B is zeros" proved nothing. Reading physical memory is
    // not a usable instrument here, and the vram_base_offset question is open,
    // not closed.
    //
    // 2eg was meant to answer it by asking AMDHWVMM::allocVMBlock, and that
    // hook never fired (`vmm_alloc_calls` absent). AMDHWVMM builds per-task VM
    // contexts via createVMContext(AMDAccelTask *), and an armed boot has no
    // GPU clients, so there is nothing for it to do. The GART page table for
    // VMID 0 comes from the TTL layer instead, and HWLibs names the function:
    //
    //   __ZN14AmdTtlServices19getGartTableAddressEPPvPy  0x126617f0
    //     12661800  movq %rdi,%rsi              rsi = this
    //     1266183f  movq 0x38(%rsi),%rdi        the TTL handle
    //     12661848  leaq -0x40(%rbp),%rsi       a 5-qword out block
    //     1266184c  callq _ttl_get_gart_primary_page_table_info
    //
    // Rather than hope Apple calls it, call it ourselves with the TTL handle
    // the initialize() hook already captured. Read-only: it reports an
    // address, it does not program anything.
    //
    // What the answer decides, stated before it is read: one of the returned
    // qwords should correspond to GCVM_CONTEXT0_PAGE_TABLE_BASE_ADDR, which
    // reads 0x0FDFA001 (0x0FDFA000 plus the VALID bit). If a qword equals
    //   0x0FDFA000             the register holds exactly what TTL reports,
    //                          and the question moves to what that number is
    //   0xF4_0FDF_A000         TTL has the carve-out MC address and the
    //                          register was written without the MC base
    //   0x7_8FDF_A000          TTL has the system-physical address and the
    //                          register was written without vram_base_offset,
    //                          which is the 2dh/2di shape a third time
    //   none of these          the register and TTL disagree about which
    //                          table is in use, and neither reading stands
    //
    // 2ei (2026-10-12): RETRY until the handle exists. The first 2eh boot
    // returned the 0xFFFFFFFF sentinel - `this->0x38` read 0 - because the
    // one shot was spent at `post-gc-hw-init`, which runs INSIDE
    // `_ttl_initialize`, and `_ttl_initialize(params, &this->0x38)` only
    // stores the handle into that slot when it finishes. Same mistake as 2dp:
    // the measurement was taken before the thing it measures exists. The
    // later dump points (post-startKIQ, pre-kiq-submit, at-stamp-wait,
    // post-stamp-wait) all run well after TTL init completes, so the done
    // flag is now set only when the call actually ran.
    if (g_rb_2eh_done == 0 && g_rb_ttl_gart_info && g_rb_ttl_this) {
        void *handle = *reinterpret_cast<void **>(
            reinterpret_cast<uintptr_t>(g_rb_ttl_this) + 0x38);
        uint64_t out[5] = {0, 0, 0, 0, 0};
        uint32_t st = 0xFFFFFFFF;
        if (handle) {
            g_rb_2eh_done = 1;          // only now is the reading meaningful
            st = g_rb_ttl_gart_info(handle, out);
        } else {
            rbgpu_log("2eh DIAG [%s]: TTL handle (this->0x38) is still 0 here - "
                      "TTL init has not stored it yet; retrying at the next "
                      "dump point rather than recording a sentinel as a result.",
                      tag);
            rbgpu_diag_inc("gart_info_early");
        }
        rbgpu_diag_set_num("gart_info_st", st);
        rbgpu_diag_set_num("gart_info_0", out[0]);
        rbgpu_diag_set_num("gart_info_1", out[1]);
        rbgpu_diag_set_num("gart_info_2", out[2]);
        rbgpu_diag_set_num("gart_info_3", out[3]);
        rbgpu_diag_set_num("gart_info_4", out[4]);
        rbgpu_log("2eh DIAG [%s]: _ttl_get_gart_primary_page_table_info(handle=%p) "
                  "-> status %u ; out = 0x%llx 0x%llx 0x%llx 0x%llx 0x%llx",
                  tag, handle, st, out[0], out[1], out[2], out[3], out[4]);

        uint64_t reg = g_rb_2eb_pt_base & ~0xFFFULL;
        uint64_t mc  = reg + (static_cast<uint64_t>(g_rb_2ed_fb_off) << 24);
        for (int i = 0; i < 5; i++) {
            uint64_t v = out[i] & ~0xFFFULL;
            if (v == 0) continue;
            const char *what = (v == reg)  ? "EQUALS the hub register value"
                             : (v == mc)   ? "equals reg + (FB_OFFSET<<24): the "
                                             "register is missing vram_base_offset"
                             : (v == (reg + 0xF400000000ULL))
                                           ? "equals reg + carve-out MC base"
                                           : nullptr;
            if (what) {
                rbgpu_log("2eh DIAG [%s]: out[%d]=0x%llx %s", tag, i, out[i], what);
                rbgpu_diag_set_num("gart_match_idx", static_cast<uint64_t>(i));
            }
        }
        // 2ek (2026-10-13): read the RING's OWN PTE, not the start of the table.
        //
        // 2ej read the first 512 qwords at out[4] and found none valid, and
        // that proves nothing, because of two things it got wrong:
        //
        //   GCVM_CONTEXT0_CNTL = 0x01555481 has PAGE_TABLE_DEPTH (bits 2:1)
        //   = 0, so what PT_BASE points at is a FLAT PTE ARRAY, not a page
        //   directory - one 8-byte entry per 4 KB page of the aperture.
        //
        //   The aperture starts at START<<12 = 0xFF_BFA0_0000 and the ring
        //   sits at 0xFF_BFE8_0000, so the ring's entry is at index
        //   (ring - start) >> 12 = 1152, byte offset 0x2400. The first 512
        //   entries cover 0xFF_BFA0_0000..0xFF_BFC0_0000 and do NOT include
        //   it. "No valid entries" was a statement about empty low aperture,
        //   not about the ring.
        //
        // The read at +0x2400 is bounded by TWO independently-derived sizes
        // rather than one guess: out[0]'s low 32 bits (0x201000) and
        // (END - START) * 8 from the aperture registers (0x202000). The entry
        // is only read when it fits inside the smaller of the two.
        if (out[4] >= 0xFFFFFF0000000000ULL) {
            rbgpu_diag_set_num("pde_no_va", 0);   // clear the early-pass sentinel

            uint64_t size_a   = out[0] & 0xFFFFFFFFULL;
            uint64_t size_b   = (g_rb_2eb_end_pg - g_rb_2eb_start_pg) * 8ULL;
            uint64_t limit    = size_a < size_b ? size_a : size_b;

            auto pte = reinterpret_cast<volatile uint64_t *>(out[4]);

            // Count across as much of the table as both sizes allow.
            uint64_t nent = limit / 8;
            if (nent > 0x40400ULL) nent = 0x40400ULL;
            uint32_t valid = 0, sys = 0, lo = 0, mid = 0, fb = 0, other = 0;
            for (uint64_t i = 0; i < nent; i++) {
                uint64_t e = pte[i];
                if (!(e & 1)) continue;
                valid++;
                if (e & 2) { sys++; continue; }
                uint64_t a = e & 0x0000FFFFFFFFF000ULL;
                if (a < 0x10000000ULL)                              lo++;
                else if (a >= 0x780000000ULL && a < 0x800000000ULL) mid++;
                else if (a >= 0xF400000000ULL && a < 0xF500000000ULL) fb++;
                else                                                other++;
            }
            rbgpu_diag_set_num("pte_entries", nent);
            rbgpu_diag_set_num("pde_valid", valid);
            g_rb_2ek_valid = valid;
            rbgpu_diag_set_num("pde_sys", sys);
            rbgpu_diag_set_num("pde_low", lo);
            rbgpu_diag_set_num("pde_sysphys", mid);
            rbgpu_diag_set_num("pde_carveout", fb);
            rbgpu_diag_set_num("pde_other", other);

            rbgpu_log("2ek DIAG [%s]: FLAT PTE table (CONTEXT0 depth 0) at kernel VA "
                      "0x%llx, size_a=0x%llx size_b=0x%llx -> scanned %llu entries: "
                      "valid=%u sys=%u low=%u sysphys(0x7_8..)=%u carveout(0xF4_..)=%u other=%u",
                      tag, out[4], size_a, size_b, nent,
                      valid, sys, lo, mid, fb, other);

            // 2el: the ring's own PTE cannot be read here. This block runs
            // BEFORE the HQD probe in the same dump, so `g_rb_2ea_pq_base`
            // still holds the previous dump point's value - 0 at
            // post-gc-hw-init - and the ring VA is not yet known. The 2ek
            // read was silently skipped by its `ring_va >= start_va` guard
            // for exactly that reason. Cache what the read needs and do it
            // at the end of the dump, after the HQD probe has run.
            g_rb_gart_va    = out[4];
            g_rb_gart_limit = limit;
            g_rb_gart_mc    = out[2];   // 2em: the carve-out MC of the table
        } else {
            rbgpu_log("2ek DIAG [%s]: out[4]=0x%llx is not a kernel VA - nothing read.",
                      tag, out[4]);
            rbgpu_diag_set_num("pde_no_va", 1);
        }

        rbgpu_log("2eh DIAG [%s]: hub register PT_BASE=0x%llx (page 0x%llx); "
                  "reg+FB_OFFSET<<24 = 0x%llx. A qword matching none of these "
                  "means TTL and the hub disagree and neither reading stands.",
                  tag, g_rb_2eb_pt_base, reg, mc);
    }

    // 2ea (2026-10-12): WHEN did the fault latch, and is it the ring or the
    // fence? The 2dz boot found STATUS=0x881 with CNTL=0x1ffc (reporting on,
    // so the latch is meaningful) and fault page 0x0FFBFE80. Under the
    // standard conventions that is byte 0xFFBFE80000, and CP_HQD_PQ_BASE =
    // 0xffbfe800 << 8 is the same 0xFFBFE80000 - an exact 40-bit match with
    // the RING base, not the fence (0xFFBFDC0040, 0xC0000 lower). 2dz only
    // compared against the fence, so it printed "a different access" and
    // missed the identification.
    //
    // The fault registers LATCH and stay until cleared, and MORE_FAULTS=1
    // says more than one occurred, so a fault seen at post-stamp-wait could
    // have been raised at any earlier moment - including before startKIQ, by
    // something that is not ours at all. Recording the status per dump point
    // is what distinguishes "the KIQ fetch faulted" from "a fault was already
    // latched before the KIQ existed". Without that, this is a lead, not a
    // finding.
    {
        const char *shrt = "x";
        if (tag) {
            if (!strncmp(tag, "post-gc", 7))        shrt = "gc";
            else if (!strncmp(tag, "post-startKIQ", 13)) shrt = "kiq";
            else if (!strncmp(tag, "pre-kiq", 7))   shrt = "pre";
            else if (!strncmp(tag, "at-stamp", 8))  shrt = "wait";
            else if (!strncmp(tag, "post-stamp", 10)) shrt = "post";
        }
        char kst[48], kad[48];
        snprintf(kst, sizeof(kst), "vmf_st_%s", shrt);
        snprintf(kad, sizeof(kad), "vmf_ad_%s", shrt);
        rbgpu_diag_set_num(kst, vm_status);
        rbgpu_diag_set_num(kad, (static_cast<uint64_t>(vm_hi) << 32) | vm_lo);

        uint64_t fpage = (static_cast<uint64_t>(vm_hi) << 32) | vm_lo;
        uint64_t fbyte = fpage << 12;
        uint64_t ring  = static_cast<uint64_t>(g_rb_2ea_pq_base) << 8;
        rbgpu_log("2ea DIAG [%s]: fault latched=%u status=0x%08x page=0x%llx "
                  "-> byte 0x%llx ; ring base (PQ_BASE 0x%08x << 8) = 0x%llx ; "
                  "fence dest = 0x%llx -> %s",
                  tag, vm_status & 1u, vm_status, fpage, fbyte,
                  g_rb_2ea_pq_base, ring, g_rb_2dt_wb_c8,
                  (vm_status == 0)
                      ? "nothing latched at this point"
                  : (ring != 0 && fbyte == ring)
                      ? "THE FAULT IS AT THE RING BASE - the CP's fetch of the "
                        "ring is what GPUVM refused, so the packets may never "
                        "have been read validly at all"
                  : ((fbyte & ~0xFFFull) == (g_rb_2dt_wb_c8 & ~0xFFFull))
                      ? "THE FAULT IS AT THE FENCE PAGE - the write is what "
                        "GPUVM refused"
                      : "the fault is at neither the ring nor the fence page");
    }

    // 2eb: a nonzero STATUS is the latch. Bit 0 is MORE_FAULTS ("further
    // faults beyond the first"), NOT a validity bit - 2dz/2ea tested
    // `status & 1` and so called the 2ea boot's genuine 0x9ba fault "nothing
    // latched", while 2dz's 0x881 printed the banner only because
    // MORE_FAULTS happened to be set that boot. Ninth instance of this
    // project's recurring error, in the code written to prevent it.
    if (vm_status != 0) {
        // The address register's units are not independently verified here, so
        // both readings are printed and neither is asserted.
        uint64_t raw  = (static_cast<uint64_t>(vm_hi) << 32) | vm_lo;
        uint64_t pages = raw << 12;
        rbgpu_log("2dz DIAG [%s]: *** A GPUVM PROTECTION FAULT IS LATCHED *** "
                  "STATUS=0x%08x (MORE_FAULTS=%u WALKER_ERROR=%u "
                  "PERMISSION_FAULTS=0x%x MAPPING_ERROR=%u RW=%u) ; fault addr "
                  "raw=0x%llx, or 0x%llx if the register counts 4 KB pages ; the "
                  "fence destination was 0x%llx -> %s",
                  tag, vm_status, vm_status & 1u, (vm_status >> 1) & 7u,
                  (vm_status >> 4) & 0xFu, (vm_status >> 8) & 1u,
                  (vm_status >> 18) & 1u, raw, pages, g_rb_2dt_wb_c8,
                  ((raw == (g_rb_2dt_wb_c8 >> 12)) || (pages == (g_rb_2dt_wb_c8 & ~0xFFFull)))
                      ? "THE FAULT IS AT THE FENCE DESTINATION: the GPUVM "
                        "mapping covers the ring for fetch but not this frame "
                        "for write"
                      : "the fault address does not match the fence destination "
                        "under either reading, so it is a different access");
    } else if (vm_cntl == 0) {
        rbgpu_log("2dz DIAG [%s]: STATUS=0 but FAULT_CNTL=0, so no fault class is "
                  "being reported - this does NOT mean there was no fault",
                  tag);
    } else {
        rbgpu_log("2dz DIAG [%s]: no GPUVM protection fault latched, with "
                  "FAULT_CNTL=0x%08x so reporting is at least partly on - the "
                  "write was not refused by the page tables",
                  tag, vm_cntl);
    }

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

    // 2el (2026-10-13): the ring's own PTE, read AFTER the HQD probe.
    //
    // 2ek put this read inside the 2eh block, which runs earlier in this same
    // function than `rbgpu_probe_hqd_slot` - so `g_rb_2ea_pq_base` still held
    // the previous dump point's value (0, at post-gc-hw-init), the guard
    // `ring_va >= start_va` was false, and the whole read was skipped without
    // a word. `ring_pte_idx` came back ABSENT. Twelfth instance of taking a
    // measurement before the thing it needs exists, and the third of exactly
    // this ordering shape (2dp, 2eh, this).
    //
    // Its own latch, set only when the read actually happens, so a dump point
    // that is still too early retries at the next one.
    if (g_rb_2ek_done == 0 && g_rb_gart_va != 0 && g_rb_2ea_pq_base != 0) {
        uint64_t start_va = g_rb_2eb_start_pg << 12;
        uint64_t ring_va  = static_cast<uint64_t>(g_rb_2ea_pq_base) << 8;
        if (ring_va >= start_va) {
            uint64_t idx  = (ring_va - start_va) >> 12;
            uint64_t need = (idx + 1) * 8;
            rbgpu_diag_set_num("ring_pte_idx", idx);
            if (need <= g_rb_gart_limit) {
                g_rb_2ek_done = 1;
                auto pte = reinterpret_cast<volatile uint64_t *>(g_rb_gart_va);
                uint64_t e = pte[idx];
                uint64_t a = e & 0x0000FFFFFFFFF000ULL;
                rbgpu_diag_set_num("ring_pte", e);
                rbgpu_diag_set_num("ring_pte_valid", (e & 1) ? 1 : 0);
                rbgpu_diag_set_num("ring_pte_sys", (e >> 1) & 1);
                rbgpu_diag_set_num("ring_pte_addr", a);
                rbgpu_log("2el DIAG [%s]: ring VA 0x%llx -> PTE[%llu] (byte 0x%llx) "
                          "= 0x%llx  valid=%llu system=%llu addr=0x%llx",
                          tag, ring_va, idx, idx * 8, e, (e & 1), (e >> 1) & 1, a);
                rbgpu_log("2el DIAG [%s]: not valid means the ring page is genuinely "
                          "unmapped and the fault is the table's CONTENTS, so PT_BASE "
                          "alone would not help; valid means the table is fine and "
                          "only PT_BASE is wrong. The 2ek scan found %u valid entries "
                          "and ALL of them AMDGPU_PTE_SYSTEM, so this table maps "
                          "system pages - a SYSTEM bit here is expected, not a fault.",
                          tag, g_rb_2ek_valid);
            } else {
                rbgpu_diag_set_num("ring_pte_oob", 1);
                rbgpu_log("2el DIAG [%s]: ring PTE needs 0x%llx bytes but the table is "
                          "bounded at 0x%llx - NOT read, and this says nothing about "
                          "the ring's mapping.", tag, need, g_rb_gart_limit);
            }
        } else {
            rbgpu_log("2el DIAG [%s]: ring VA 0x%llx is below the aperture start "
                      "0x%llx - not read.", tag, ring_va, start_va);
        }
    }

    // 2em (2026-10-13): point the hub register at where the table REALLY is.
    //
    // This is the plugin's first BEHAVIOURAL write to a GC register -
    // GRBM_GFX_CNTL (2do) is an index register that is saved and restored and
    // changes nothing by itself. This one changes how every GPUVM translation
    // in VMID 0 resolves, and it is left in place. Off unless
    // `-rbgpuptbase` is in boot-args.
    //
    // What is measured, not assumed, before it is allowed to fire:
    //
    //   * TTL reports the flat PTE table at carve-out MC 0xF4_0FDF_A000, and
    //     `GCVM_CONTEXT0_PAGE_TABLE_BASE_ADDR` holds exactly that page with
    //     the carve-out base stripped (2ei: three address spaces, one offset).
    //     The code re-checks that relationship every boot and refuses if it
    //     does not hold.
    //   * Linux: the register wants mc2pa(mc) = mc - vram_start +
    //     vram_base_offset, with vram_base_offset = GCMC_VM_FB_OFFSET << 24
    //     (UPSTREAM.md). FB_OFFSET reads 0x780 here, so the value is short by
    //     0x7_8000_0000 - right for a Navi 21 dGPU where FB_OFFSET is 0.
    //   * The ring's own PTE at index 1152 is VALID (2el), so the table's
    //     contents are not the fault.
    //   * EVERY valid entry in the table is AMDGPU_PTE_SYSTEM (2ek: 1512 of
    //     1512). A SYSTEM entry holds a bus address and never goes through
    //     mc2pa, so no PTE here can carry the same zero-offset error. That is
    //     what makes this safe: the danger that blocked the patch through
    //     2ej-2el - PTEs low by 0x7_8000_0000, aiming GPU writes at the wrong
    //     host DRAM - cannot occur in a table with no VRAM entries.
    //
    // Residual risk, stated plainly: if the walker wants the BAR0 alias
    // (0x8_8FDF_A000) rather than the DRAM alias (0x7_8FDF_A000), the write
    // points it at another wrong page and the same MAPPING_ERROR comes back -
    // no worse than now. No TLB invalidate is issued, so a stale negative
    // translation is a second candidate if the fault persists.
    if (g_rb_2em_done == 0 && checkKernelArgument("-rbgpuptbase") &&
        g_rb_gart_mc != 0 && g_rb_2ed_fb_base != 0 && g_rb_2ed_fb_off != 0) {
        const uint32_t dwPTB_LO = 0x1667, dwPTB_HI = 0x1668;
        uint64_t fb_base = static_cast<uint64_t>(g_rb_2ed_fb_base) << 24;
        uint64_t vram_base_offset = static_cast<uint64_t>(g_rb_2ed_fb_off) << 24;

        uint32_t lo = g_hw.rreg(g_hw.ctx, (kSeg0Conv + dwPTB_LO) * 4);
        uint32_t hi = g_hw.rreg(g_hw.ctx, (kSeg0Conv + dwPTB_HI) * 4);
        uint64_t cur = (static_cast<uint64_t>(hi) << 32) | lo;
        uint64_t flags = cur & 0xFFF;
        uint64_t fb_off = (g_rb_gart_mc >= fb_base) ? (g_rb_gart_mc - fb_base) : 0;

        bool rel_holds = (fb_off != 0) && ((cur & ~0xFFFULL) == fb_off);
        rbgpu_diag_set_num("ptb_cur", cur);
        rbgpu_diag_set_num("ptb_rel_holds", rel_holds ? 1 : 0);

        if (!rel_holds) {
            rbgpu_log("2em [%s]: REFUSING to patch. PT_BASE=0x%llx but TTL's table "
                      "MC 0x%llx is FB offset 0x%llx - the stripped-offset "
                      "relationship 2ei measured does NOT hold this boot, so the "
                      "replacement value is not derivable.",
                      tag, cur, g_rb_gart_mc, fb_off);
        } else {
            uint64_t want = fb_off + vram_base_offset + flags;
            g_rb_2em_done = 1;
            rbgpu_log("2em [%s]: PT_BASE 0x%llx -> 0x%llx (FB offset 0x%llx + "
                      "vram_base_offset 0x%llx + flags 0x%llx). WRITING.",
                      tag, cur, want, fb_off, vram_base_offset, flags);
            g_hw.wreg(g_hw.ctx, (kSeg0Conv + dwPTB_HI) * 4, static_cast<uint32_t>(want >> 32));
            g_hw.wreg(g_hw.ctx, (kSeg0Conv + dwPTB_LO) * 4, static_cast<uint32_t>(want));

            uint32_t rlo = g_hw.rreg(g_hw.ctx, (kSeg0Conv + dwPTB_LO) * 4);
            uint32_t rhi = g_hw.rreg(g_hw.ctx, (kSeg0Conv + dwPTB_HI) * 4);
            uint64_t back = (static_cast<uint64_t>(rhi) << 32) | rlo;
            rbgpu_diag_set_num("ptb_want", want);
            rbgpu_diag_set_num("ptb_back", back);
            rbgpu_diag_set_num("ptb_written", (back == want) ? 1 : 0);
            rbgpu_log("2em [%s]: read back 0x%llx -> %s. If the submission now "
                      "raises no fault (vmf_st_wait 0) and stamp_ret is 1, this "
                      "was the whole fix; if the same MAPPING_ERROR returns, the "
                      "walker wants a different alias or the TLB is stale.",
                      tag, back, (back == want) ? "TOOK" : "DID NOT TAKE");
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
        // 2jp: the five RLC sub-images 2aj never supplied. Apple's
        // _gc_10_3_get_fw_constants registers rlc_srlist_cntl/gpm/srm and
        // rlc_lx6_iram/dram as separate firmware entries (switch indices
        // 0,1,2,0xd,0xe); 2aj swapped only the main ucode, so the RLC has
        // been booting with NAVI 21 save/restore lists and, worse, a Navi21
        // LX6 image - the firmware for the processor INSIDE the RLC that
        // runs the autoload. That is a mechanical candidate for
        // BOOTLOAD_COMPLETE never setting while the RLC otherwise looks
        // alive (RLC_STAT 0x25, f32=1).
        //
        // Identified from our own _IpiSetFwEntry log against the v2.2
        // header of yellow_carp_rlc.bin, with THREE exact size matches:
        //   id 26 = 0x250   == srlist_cntl  0x250
        //   id 27 = 0x600   == srlist_gpm   0x600
        //   id 29 = 0x10200 == rlc_iram     0x10200
        // and two same-role size mismatches, where ours is the bigger:
        //   id 28  Apple 0x47a0  ours (srm)  0x4880
        //   id 30  Apple 0x4200  ours (dram) 0x10200
        //
        // The mismatches are the risk in this step and are not hidden: 2aj
        // writes ptr AND size together, so the PSP is told the real size,
        // but whether it accepts a larger image for these two entries is
        // unknown. -rbgpurlcsub gates all five so a bad outcome is one
        // boot-arg away from reverted.
        extern const uint8_t yc_rlc_srlist_cntl_img[];
        extern const uint8_t yc_rlc_srlist_gpm_img[];
        extern const uint8_t yc_rlc_srlist_srm_img[];
        extern const uint8_t yc_rlc_iram_img[];
        extern const uint8_t yc_rlc_dram_img[];
        extern const uint32_t yc_rlc_srlist_cntl_img_size;
        extern const uint32_t yc_rlc_srlist_gpm_img_size;
        extern const uint32_t yc_rlc_srlist_srm_img_size;
        extern const uint32_t yc_rlc_iram_img_size;
        extern const uint32_t yc_rlc_dram_img_size;

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
        // 2jp: the RLC sub-images, opt-in.
        if (!yc_img && rbgpu_2jp_enabled()) {
            switch (d[0]) {
                case 26: yc_img = yc_rlc_srlist_cntl_img;
                         yc_size = yc_rlc_srlist_cntl_img_size; break;
                case 27: yc_img = yc_rlc_srlist_gpm_img;
                         yc_size = yc_rlc_srlist_gpm_img_size;  break;
                case 28: yc_img = yc_rlc_srlist_srm_img;
                         yc_size = yc_rlc_srlist_srm_img_size;  break;
                case 29: yc_img = yc_rlc_iram_img;
                         yc_size = yc_rlc_iram_img_size;        break;
                case 30: yc_img = yc_rlc_dram_img;
                         yc_size = yc_rlc_dram_img_size;        break;
                default: break;
            }
            if (yc_img) { rbgpu_diag_inc("yc_rlc_swapped"); }
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

// 2fb: the register accessor (pm4+0x30), stashed at doStart entry.
static void *g_rb_2fb_regobj = nullptr;
static void rbgpu_probe_regread_2fb(void);

// 2fj: the gate helper is defined further down; declare it for this hook.
static void rbgpu_gate_2en(uint32_t stage, const char *what);

// 2fs (2026-10-14): ONE stage-numbered NVRAM variable, `rbgpu-prog`.
//
// 2fq put its two writes at startHWEngines and at the 0x1B decision, both of
// them PAST the frontier whose endpoint has varied in every boot of this
// family. The 2fr boot then wrote neither, and `nvram -p` shows the variables
// were never created at all - which is equally well explained by
//
//   the thread never reaching either site, or
//   rbgpu_nvram_note not working (creating a NEW variable, as opposed to
//     overwriting rbgpu-gate, has never been exercised)
//
// and nothing distinguishes them. That is the ambiguity of absence this
// project has already paid for at 2eh, 2ek, 2en and 2fb, and 2fd named the
// fix: put the measurement where the code ALWAYS reaches.
//
// So stage 1 is written at doStart ENTER - the one site in this chain that
// has executed in every boot since 2ce, twenty milestones ago, and the same
// site 2fd moved the accessor probe to for exactly this reason. If
// `rbgpu-prog` is absent after the next boot, the mechanism is broken and
// that is settled without a second boot; if it reads `s=1`, the mechanism
// works and the thread did not get past doStart.
//
// Cost: one flash commit inside the ~500 us chain region (stage 1), and the
// rest only after the chain has finished. 2fo's accounting blamed ~24
// commits threaded THROUGH the chain for the wedge; this is one.
static void rbgpu_prog_2fs(uint32_t stage, const char *what) {
    char buf[200];
    snprintf(buf, sizeof(buf), "s=%u %s", stage, what ? what : "");
    rbgpu_nvram_note("rbgpu-prog", buf);
    g_rb_prog_stage = stage;          // 2fw: what the timer samples
    rbgpu_log("2fs: prog s=%u %s", stage, what ? what : "");
}

static bool hooked_pm4_doStart_2ce(void *pm4, bool arg) {
    rbgpu_log("2ce: AMDGFX10PM4Engine::doStart entered (pm4=%p arg=%d)", pm4, arg ? 1 : 0);

    // 2ew: the fields that decide which region of Apple's initGraphicsMQD
    // can run, read HERE rather than inside that hook - 2ev proved reads and
    // logs in that span cost the thread its progress. doStart's `this` IS the
    // PM4 engine initGraphicsMQD is later called on, and this hook's logging
    // has been safe in every boot since 2ce.
    //
    // What these bound and what they do not: `pm4+0x10` is a configuration
    // byte nothing in this chain writes, so its value here is its value at
    // 0xbea25f2. `pm4+0x310` IS written by initGraphicsMQD itself (cleared at
    // 0xbea2634, set at 0xbea26dd), but this is the FIRST call, so the value
    // here is the one the branch at 0xbea260c tests. `pm4+0x250` and
    // `pm4+0x30` are read before initComputeMQD runs, so they are a lower
    // bound on what 0xbea258b sees, not a proof of it.
    if (pm4) {
        uint8_t f10  = *reinterpret_cast<volatile uint8_t *>(
            reinterpret_cast<uintptr_t>(pm4) + 0x10);
        uint8_t f310 = *reinterpret_cast<volatile uint8_t *>(
            reinterpret_cast<uintptr_t>(pm4) + 0x310);
        void *f250 = *reinterpret_cast<void **>(
            reinterpret_cast<uintptr_t>(pm4) + 0x250);
        void *f18  = *reinterpret_cast<void **>(
            reinterpret_cast<uintptr_t>(pm4) + 0x18);
        void *f30  = *reinterpret_cast<void **>(
            reinterpret_cast<uintptr_t>(pm4) + 0x30);
        rbgpu_diag_set_num("gfxmqd_f10", f10);
        rbgpu_diag_set_num("gfxmqd_f310", f310);
        rbgpu_diag_set_num("gfxmqd_f250", reinterpret_cast<uint64_t>(f250));
        rbgpu_diag_set_num("gfxmqd_f30", reinterpret_cast<uint64_t>(f30));
        // 2fb: stash the register accessor so the setres hook can use it at
        // the moment Apple does, rather than here at doStart entry. Measuring
        // before the state exists is the 2dp/2eh/2ek mistake.
        g_rb_2fb_regobj = f30;
        // 2fd (2026-10-13): the probe runs HERE, not after gate 45.
        //
        // Three boots (2fa, 2fb, 2fc) ran the gate-45 span with no relevant
        // code change between them and the trail ended at 45, 45, 101. So the
        // trail's endpoint still varies boot to boot, exactly as 2ez found,
        // and a probe placed behind a varying gate inherits that variance -
        // 2fb and 2fc both spent a boot learning nothing for that reason.
        //
        // doStart entry is the one site in this chain that has executed in
        // every boot since 2ce. 2fb's objection to reading here was that the
        // chip state would be wrong, but that applies to the WRITES; 2dl read
        // CP_RB0_CNTL straight through MMIO at post-gc-hw-init - earlier than
        // this - and got 0x00a00000, so the register is readable before
        // doStart and a READ here is sound.
        rbgpu_probe_regread_2fb();
        rbgpu_log("2ew: initGraphicsMQD branch fields at doStart entry: "
                  "pm4+0x10=%u (1 = the command-9 graphics-queue create at "
                  "0xbea26a1 RUNS; 0 = region G is SKIPPED and the hang is in "
                  "A-F or the GC register pairs H-J) pm4+0x310=%u (1 = the "
                  "destroy at 0xbea2626 runs first) pm4+0x250=%p pm4+0x18=%p "
                  "pm4+0x30=%p", f10, f310, f250, f18, f30);
    }
    g_rb_2cf_dostart_called = 1;
    // Call original - our initComputeMQD hook (2cd) will log if it's reached
    bool ret = orig_pm4_doStart_2ce ? orig_pm4_doStart_2ce(pm4, arg) : false;
    RB_STEP(90);   // 2fz: Apple's doStart RETURNED
    rbgpu_log("2ce: doStart returned %d", ret ? 1 : 0);
    // 2fj: doStart's own exit, so the trail says whether the whole chain
    // completed rather than leaving it to be inferred from the last inner
    // gate. Band 30 is clear of 40/45, 46-49, 50/54/55, 60-67/70-77/80-87,
    // 90-96/98/99 and 100+/150+.
    rbgpu_gate_2en(ret ? 30 : 31,
                   ret ? "doStart RETURNED TRUE - powerUp succeeds"
                       : "doStart RETURNED FALSE - clean failure, desktop");
    // 2fs stage 2: once, on the FIRST doStart to return. The chain is behind
    // us here, so this commit is not inside the 500 us region.
    {
        static bool once = false;
        if (!once) {
            once = true;
            rbgpu_prog_2fs(2, ret ? "doStart RETURNED 1" : "doStart RETURNED 0");
        }
    }
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
// 2du (2026-10-12): which gate leaves channel+0xe0 at -1.
//
// The 2dt boot read, at the KIQ channel, submitted(+0x80)=1 completed(+0x84)=0
// with +0xc0 = 0xffffffa965fcc040 (a real kernel VA) and **+0xe0 = -1**.
// `timestampUpdated` (0xbe85214) begins `if (this->0xe0 == -1) return;`, so
// that branch is taken every time and the fence is never read. The frame is
// correctly marked submitted and the CP really executed it (2dq); the
// completed counter simply has no path to be updated.
//
// +0xe0 is NOT an address - `AMDHWChannel::init` (0xbe848cc) sets it to -1 at
// 0xbe84974 and to `(int)this->0xc`, the channel index, at 0xbe84ade, and only
// after three gates all pass:
//
//   be84a9d  callq *0x330(this->0x20)    AMDHardware::registerChannel(this)
//            false -> jump away, +0xe0 stays -1
//   be84aad  callq *0x1c8(this)          a KIQ-channel method
//            false -> jump away, +0xe0 stays -1
//   be84ad1  AMDSWScheduler::addHWChannel(sched, this, this->0xc)
//            false -> jump away, +0xe0 stays -1
//   be84ade  this->0xe0 = (int)this->0xc
//
// So the index is the channel's slot in the AMDSWScheduler, and -1 means this
// channel was never registered with it. Both outer gates are named symbols
// defined in X6000, so hook them observe-only and the next boot names which
// one refuses.
static bool (*orig_hw_registerChannel_2du)(void *, void *) = nullptr;
static bool (*orig_sched_addHWChannel_2du)(void *, void *, uint64_t) = nullptr;
// 2gn: request 0x1B's three named post-gate calls. Apple ignores every
// return value (no test follows any of the three call sites), so void.
static void *(*orig_hw_getHWChannel_2gv)(void *, uint32_t, uint32_t) = nullptr;
static void (*orig_hw_startGlobalPerfCounters_2gn)(void *) = nullptr;
static void (*orig_hw_hardwareDidStart_2gn)(void *) = nullptr;
static void (*orig_hw_enableInterrupts_2gn)(void *) = nullptr;
// 2go: hardwareDidStart's own chain, every member of it a named symbol.
// Apple ignores the first three returns; the two HIQ ones are propagated.
static void (*orig_rt_hardwareDidStart_2go)(void *) = nullptr;
static void (*orig_vmm_postInit_2go)(void *) = nullptr;
static void (*orig_uqm_setSchedulerResources_2go)(void *, uint16_t, uint32_t,
                                                  uint32_t, uint32_t) = nullptr;
static bool (*orig_hiq_submitSetResourcesPacket_2go)(void *, uint16_t, uint32_t) = nullptr;
static bool (*orig_hiq_submitFrame_2go)(void *, bool) = nullptr;
// 2gp: the event machine's stamp wait, which the 2go trail localised by
// elimination. waitForStamp runs at thousands of calls a second, so it gets
// counters and NOT a log line or an NVRAM write (the 2ez contention).
static uint32_t (*orig_evm_finishAllStamps_2gp)(void *) = nullptr;
static uint32_t (*orig_evm_waitForStamp_2gp)(void *, int, uint32_t, uint32_t *) = nullptr;
static void (*orig_evm_handleFinishChannelRestart_2gp)(void *, int, int, uint64_t) = nullptr;

// 2dv (2026-10-12): counting is not enough - track WHICH channel.
//
// The 2du boot: regchan_calls=25, addchan_calls=23, and NEITHER ever returned
// false. So both named gates pass for every channel they see, and yet the KIQ
// channel's +0xe0 is still -1. The 25-vs-23 gap means two channels got
// registerChannel and never reached addHWChannel - which is precisely the
// shape of the middle gate, `this->vtable[0x1c8]`, refusing for them.
//
// Counters cannot say whether the KIQ is one of those two, and the log lines
// were lost to the ring wrap, so record the channel POINTERS and answer by
// membership at waitForHwStamp, where the KIQ channel is in hand.
#define RB_2DV_MAX_CHAN 48
static void    *g_rb_2dv_reg[RB_2DV_MAX_CHAN];
static uint32_t g_rb_2dv_reg_n = 0;
static void    *g_rb_2dv_add[RB_2DV_MAX_CHAN];
static uint32_t g_rb_2dv_add_n = 0;

static void rb_2dv_note(void **arr, uint32_t *n, void *p) {
    if (!p || *n >= RB_2DV_MAX_CHAN) return;
    for (uint32_t i = 0; i < *n; i++)
        if (arr[i] == p) return;
    arr[(*n)++] = p;
}
static bool rb_2dv_seen(void *const *arr, uint32_t n, void *p) {
    for (uint32_t i = 0; i < n; i++)
        if (arr[i] == p) return true;
    return false;
}

// 2eg (2026-10-12): ask Apple's own VM allocator where the page tables are.
//
// 2ee found that NEITHER candidate physical page holds a page directory:
// 0x0FDFA000 is live macOS kernel memory (its first qword decodes as x86
// machine code and 251/512 "valid" bits is a coin flip on noise), and
// 0x78FDFA000 is 512 zero qwords. So guessing transformations of
// GCVM_CONTEXT0_PAGE_TABLE_BASE_ADDR is the wrong approach - ask the driver.
//
// AMDHWVMM::allocVMBlock(level, GLKMemoryElement *elem) is where every page
// table block comes from (0xbe90eea):
//
//   be90efb  movq 0x78(%rdi),%rdi          the allocator
//   be90f11  movl 0xac0(%r15,level*32),%r14d
//   be90f30  callq <allocator>(elem, r14, 0x1000, 1, 1)
//   be90f38  cmpq $0x0,0x20(%rbx)          the result lands at elem+0x20
//
// So elem+0x20 onwards carries the block's addresses, and `level` says which
// tier it is - the root directory is the one the hub register should point
// at. Observe only: the original runs first and the element is read back.
static bool (*orig_vmm_allocVMBlock_2eg)(void *, uint32_t, void *) = nullptr;
static uint32_t g_rb_2eg_calls = 0;

// 2ig (2026-10-18): remember the ROOT blocks, not just the first eight calls.
// 2if counted getPDEValue at level 0 three times and level 1 one hundred and
// eleven times, and three VMIDs were handed out - so level 0 is the root
// directory, one per VM context, and level 1 is the page-table tier. These
// are the only addresses a context page-directory base could legitimately
// hold, which is what makes them a STRICT match criterion rather than a
// guess at a struct field.
#define RB_2IG_ROOTS 8
struct rb_2ig_root { void *elem; uint64_t a20, a28; };
static struct rb_2ig_root g_rb_2ig_root[RB_2IG_ROOTS];
static uint32_t g_rb_2ig_nroot = 0;

static bool hooked_vmm_allocVMBlock_2eg(void *vmm, uint32_t level, void *elem) {
    bool ret = orig_vmm_allocVMBlock_2eg ? orig_vmm_allocVMBlock_2eg(vmm, level, elem) : false;
    g_rb_2eg_calls++;
    rbgpu_diag_set_num("vmm_alloc_calls", g_rb_2eg_calls);

    if (elem && level == 0 && g_rb_2ig_nroot < RB_2IG_ROOTS) {
        auto r = reinterpret_cast<volatile uint64_t *>(elem);
        g_rb_2ig_root[g_rb_2ig_nroot].elem = elem;
        g_rb_2ig_root[g_rb_2ig_nroot].a20  = r[4];
        g_rb_2ig_root[g_rb_2ig_nroot].a28  = r[5];
        g_rb_2ig_nroot++;
    }

    if (elem && g_rb_2eg_calls <= 8) {
        auto q = reinterpret_cast<volatile uint64_t *>(elem);
        rbgpu_log("2eg: allocVMBlock(level=%u) -> %d elem=%p "
                  "[0]=0x%llx [1]=0x%llx [2]=0x%llx [3]=0x%llx "
                  "[4]=0x%llx [5]=0x%llx [6]=0x%llx [7]=0x%llx",
                  level, ret ? 1 : 0, elem,
                  q[0], q[1], q[2], q[3], q[4], q[5], q[6], q[7]);
        char key[32];
        snprintf(key, sizeof(key), "vmm_l%u_lvl", g_rb_2eg_calls);
        rbgpu_diag_set_num(key, level);
        // +0x20 is the field allocVMBlock itself tests for success.
        snprintf(key, sizeof(key), "vmm_l%u_e20", g_rb_2eg_calls);
        rbgpu_diag_set_num(key, q[4]);
        snprintf(key, sizeof(key), "vmm_l%u_e28", g_rb_2eg_calls);
        rbgpu_diag_set_num(key, q[5]);
    }
    return ret;
}

static bool hooked_hw_registerChannel_2du(void *hw, void *chan) {
    bool ret = orig_hw_registerChannel_2du ? orig_hw_registerChannel_2du(hw, chan) : false;
    rbgpu_log("2du: AMDHardware::registerChannel(hw=%p chan=%p) -> %d%s",
              hw, chan, ret ? 1 : 0,
              ret ? "" : "  <<< FAILS -> AMDHWChannel::init skips the scheduler and "
                         "leaves channel+0xe0 = -1, so no fence is ever read");
    rbgpu_diag_inc("regchan_calls");
    if (!ret) rbgpu_diag_inc("regchan_fails");
    else rb_2dv_note(g_rb_2dv_reg, &g_rb_2dv_reg_n, chan);
    return ret;
}

static bool hooked_sched_addHWChannel_2du(void *sched, void *chan, uint64_t idx) {
    bool ret = orig_sched_addHWChannel_2du ? orig_sched_addHWChannel_2du(sched, chan, idx) : false;
    rbgpu_log("2du: AMDSWScheduler::addHWChannel(sched=%p chan=%p idx=%llu) -> %d%s",
              sched, chan, idx, ret ? 1 : 0,
              ret ? "  (so channel+0xe0 becomes the index and the fence CAN be read)"
                  : "  <<< FAILS -> channel+0xe0 stays -1");
    rbgpu_diag_inc("addchan_calls");
    if (!ret) rbgpu_diag_inc("addchan_fails");
    else rb_2dv_note(g_rb_2dv_add, &g_rb_2dv_add_n, chan);
    return ret;
}

// 2gn (2026-10-16): which of request 0x1B's four post-gate calls does not
// return.
//
// 0x1B's case (0xbe417d0, named end to end in 2gm) is straight-line, so
// 2gk's ~5600/s request-0x14 rate is a loop inside a callee - and 0x14 is
// sent by `AMDHardware::isDeviceValid()`, which `AMDHWRegisters::read()` and
// `AMDHardware::MCIL_WaitFor()` both call, so that rate is a REGISTER-POLL
// METER. After the gate, 0x1B does exactly four things:
//
//   hw->startGlobalPerfCounters()            [0x4c8]
//   hw->hardwareDidStart()                   [0x130]
//   (this->0x380)->vtable[0x158]()           <- class UNRESOLVED
//   hw->enableInterrupts()                   [0x218]
//
// Three are named symbols; the third is not. `this->0x380` is loaded in eight
// AMDGraphicsAccelerator methods and stored in none of them, so a base class
// sets it, and it is **not** an AMDSWScheduler - that vtable is 0x148 bytes,
// so slot 0x158 is past its end (checked, because `swTimestampCallback`
// loading the same field made it the obvious guess).
//
// So the unnamed call is localised **by elimination** rather than hooked:
// it sits between `hardwareDidStart` and `enableInterrupts`, so
// `hds:R` present with `ei:E` absent names it with no fourth symbol.
//
// Rule 9 was applied to all three before writing this. Each opens
// `pushq %rbp ; movq %rsp,%rbp ; pushq %r14`, straight-line well past the
// 5-byte patch window:
//
//   startGlobalPerfCounters 0xbeae2e2   55 / 48 89 e5 / 41 56 ...
//   hardwareDidStart        0xbeacf08   55 / 48 89 e5 / 41 56 ...   (its
//       rip-relative leaq is at +0xe, outside the window)
//   enableInterrupts        0xbea9558   55 / 48 89 e5 / 41 56 / 53 / cmpb,
//       and its `je` is at +0x14 - far outside
//
// Apple ignores all three return values (no test follows any of the three
// call sites), so they are called through a void thunk and nothing is
// substituted: observe only.
//
// The record is its own NVRAM variable, `rbgpu-1b`, which no later stage
// overwrites - the 2gf lesson. It is written only while 0x1B is in flight,
// because all three functions are also called from powerUpHW's own path and
// an ungated note would be overwritten by calls that are not the question.
// Six commits at most, and all of them AFTER the ~500 us chain region that
// 2fo/2ft showed must stay clear.
volatile int g_rb_2gn_in1b = 0;
static char g_rb_2gn_trail[512];   // 2gq: room for the stamp table too
static uint32_t g_rb_2gn_n = 0;

// 2gx: which request our peer is asked for inside the 0x1B window, and
// which ones it refuses.
//
// 2gw left 0x1B returning kIOReturnUnsupported (0xE00002C7) AFTER every
// stage of the chain returned, so the refusal comes from the tail of
// Apple's handling. Two possibilities that the return code alone cannot
// separate: Apple's own 0x1B handler ends in Unsupported, or Apple asks our
// peer something late and propagates OUR refusal back out. The peer's
// `default:`, 0x17 and 0x1A all return exactly 0xE00002C7, so the code we
// see is consistent with both and settles neither.
//
// This records the request numbers in order, with `!` on the ones we
// refused. It does NOT write NVRAM per request: `rbgpu_2gn_mark` commits on
// every call, and a request storm inside this window would be the 2ez
// logging contention and the 2fo wedge at once. Requests accumulate in a
// buffer and the whole trail is committed ONCE, after 0x1B returns.
//
// 0x14 is counted rather than listed - it is the register-poll meter and
// reached #1,506,624 in 2gq.
static char g_rb_2gx_reqs[112];
static uint32_t g_rb_2gx_n14 = 0;
static uint32_t g_rb_2gx_dropped = 0;

static void rbgpu_2gn_mark(const char *tag);

void rbgpu_2gx_req(uint32_t req, int refused) {
    if (!g_rb_2gn_in1b) { return; }
    if (req == 0x14) { g_rb_2gx_n14++; return; }
    size_t n = strlen(g_rb_2gx_reqs);
    // Leave room for the longest entry plus the terminator; when it is full
    // say so at flush rather than silently truncating (absence is not a
    // result).
    if (n + 6 >= sizeof(g_rb_2gx_reqs)) { g_rb_2gx_dropped++; return; }
    snprintf(g_rb_2gx_reqs + n, sizeof(g_rb_2gx_reqs) - n, "%s%02X%s",
             n ? "," : "", req, refused ? "!" : "");
}

void rbgpu_2gx_flush(void) {
    char t[160];
    snprintf(t, sizeof(t), "q[%s|14x%u%s]",
             g_rb_2gx_reqs[0] ? g_rb_2gx_reqs : "none",
             g_rb_2gx_n14,
             g_rb_2gx_dropped ? " +DROPPED" : "");
    rbgpu_2gn_mark(t);
}

static void rbgpu_2gn_mark(const char *tag) {
    if (!g_rb_2gn_in1b) { return; }
    size_t n = strlen(g_rb_2gn_trail);
    snprintf(g_rb_2gn_trail + n, sizeof(g_rb_2gn_trail) - n,
             "%s%s", n ? " " : "", tag);
    g_rb_2gn_n++;
    rbgpu_nvram_note("rbgpu-1b", g_rb_2gn_trail);
    rbgpu_log("2gn: %s", g_rb_2gn_trail);
}


// 2gv: VMPT asks for an SDMA engine this chip does not have, and gets a
// channel anyway.
//
// Measured (boot 16, build 62631971):
//   25:s1/c0/f0[AccelChannel "VMPT" h=AMDSDMAChannel]
//              out=1 tot=1 nch=63 hwn=25 nz=0 e2=1 e2q=0
//
// which is row three of the table 2gu wrote down BEFORE the boot: the
// channel exists, so nothing refuses the submit, and the engine behind it
// has zero queues, so nothing can complete it. `tot=1` says this is the
// only stamp the driver has ever issued and `nz=0` says no fence was ever
// written for it.
//
// The index bases differ and that is the whole point. The SDMA walk in
// rbgpu_walk_starthw_2fp reads `hw + 0x3b8 + i*8`, so 2gi's `engine[i]` is
// hw array index `i + 1`:
//
//   hw[0] = Navi21PM4Engine         GFX
//   hw[1] = SDMAEngine_GC_10_3_0    2gi's engine[0] - LEFT ALONE, has queues
//   hw[2] = SDMAEngine_GC_10_3_0    2gi's engine[1] - zeroed   <- VMPT
//   hw[3] = SDMAEngine_GC_10_3_0    2gi's engine[2] - zeroed
//   hw[4] = SDMAEngine_GC_10_3_0    2gi's engine[3] - zeroed
//
// `one=3` says all three really took the write, and each had a NON-zero
// count before it (rbgpu_sdma_one_2gi only counts that case). So hw[2] had
// queues, 2gi removed them to stop a second engine asking the layer for an
// instance that does not exist, and VMPT then asked hw[2] for a channel.
//
// Apple's own arithmetic makes hw[2] unavoidable. RTHardware::getHWChannel
// (0xbe973fa) computes `ENGINE_TYPE = ((caps->0x92 * 2 + 1) & idx) + 1`,
// and the VMPT call site passes prio=0, which forces `idx = 1`. Since
// `x * 2 + 1` is odd, `& 1` is 1 and the answer is 2 whatever the caps byte
// holds - on a part with two or more SDMA instances that is correct, and
// here it names the one engine that was switched off.
//
// What this hook does: in AMDHardware::getHWChannel(engine, ring)
// (0xbea9994), which is where `hw->[0x3b0 + engine*8]` is indexed, send
// engine 2 to engine 1 - the single SDMA instance this chip actually has.
// It is a SUBSTITUTION and it is logged as one every time, because 2cy's
// rule is that a substituted value must never be narrated as a hardware
// result: a stamp that completes after this proves the remap works, not
// that engine 2 exists.
//
// Rule 9 checked: 55 48 89 e5 89 f0 48 8b ... - pushq %rbp, movq %rsp,%rbp,
// movl %esi,%eax. No rel8 or rel32 branch begins inside the 5-byte patch
// window. Milestone 2c checked: `nm -m build/re/AMDRadeonX6000` shows
// __ZN26AMDRadeonX6000_AMDHardware12getHWChannelE20_eAMD_HW_ENGINE_TYPE18_eAMD_HW_RING_TYPE
// as an external DEFINED symbol at 0xbea9994, not an import.
//
// Off unless `-rbgpuvmpteng1`.
static int g_rb_2gv_on = -1;
static uint32_t g_rb_2gv_remaps = 0;

static void *hooked_hw_getHWChannel_2gv(void *hw, uint32_t engine, uint32_t ring) {
    if (g_rb_2gv_on < 0) {
        g_rb_2gv_on = checkKernelArgument("-rbgpuvmpteng1") ? 1 : 0;
        rbgpu_log("2gv: VMPT engine remap %s", g_rb_2gv_on ? "ON (2 -> 1)" : "OFF");
    }
    uint32_t use = engine;
    if (g_rb_2gv_on && engine == 2) {
        use = 1;
        g_rb_2gv_remaps++;
        rbgpu_diag_set_num("vmpt_remaps", g_rb_2gv_remaps);
        rbgpu_log("2gv: getHWChannel(engine=2, ring=%u) SUBSTITUTED to "
                  "engine=1 - this is the plugin's value, not the chip's",
                  ring);
    }
    void *ch = orig_hw_getHWChannel_2gv
                   ? orig_hw_getHWChannel_2gv(hw, use, ring)
                   : nullptr;
    if (g_rb_2gv_on && engine == 2) {
        // Absence is not a result: say what came back, so a later `h=` can
        // be tied to this call rather than assumed to follow from it.
        rbgpu_log("2gv: engine=1 returned channel %p", ch);
        rbgpu_diag_set_num("vmpt_ch_nonnull", ch ? 1 : 0);
    }
    return ch;
}

static void hooked_hw_startGlobalPerfCounters_2gn(void *hw) {
    rbgpu_diag_inc("perf_calls");
    rbgpu_2gn_mark("perf:E");
    if (orig_hw_startGlobalPerfCounters_2gn) { orig_hw_startGlobalPerfCounters_2gn(hw); }
    rbgpu_2gn_mark("perf:R");
}

static void hooked_hw_hardwareDidStart_2gn(void *hw) {
    rbgpu_diag_inc("hds_calls");
    rbgpu_2gn_mark("hds:E");
    if (orig_hw_hardwareDidStart_2gn) { orig_hw_hardwareDidStart_2gn(hw); }
    // If this RETURN lands and `ei:E` never does, the stop is the unnamed
    // (this->0x380)->vtable[0x158]() between them.
    rbgpu_2gn_mark("hds:R");
}

static void hooked_hw_enableInterrupts_2gn(void *hw) {
    rbgpu_diag_inc("ei_calls");
    rbgpu_2gn_mark("ei:E");
    if (orig_hw_enableInterrupts_2gn) { orig_hw_enableInterrupts_2gn(hw); }
    rbgpu_2gn_mark("ei:R");
}

// 2go (2026-10-16): the 2gn boot answered - `b=9 perf:E perf:R hds:E`, so
// AMDGFX10Hardware::hardwareDidStart is entered and does not return. It is
// 40 instructions with no loop (0xbeacf08), and so is the base-class
// hardwareDidStart it calls first (0xbe97920), so the stop is in a callee.
// Every one of them is now a named symbol, read off the disassembly:
//
//   hardwareDidStart (GFX10, 0xbeacf08)
//     A  AMDRTHardware::hardwareDidStart(this)       non-virtual, via the
//        A1  peer->callPlatformFunction(0x12, wait=1, .., &out, 0)
//            our peer answers kIOReturnUnsupported, so `this->0x206c0 = 0`
//            and the vtable[0x3e8] branch at 0xbe9797c is NOT taken
//        A2  (hw->0x380 = AMDGFX10VMM)->postInit()   first time only
//            two vmFaultInterruptCallback registrations (0xFF0000E9/EA).
//            No loop and no register access.
//     B  peer->callPlatformFunction(0x1D, wait=1, .., &out, 0)
//        also Unsupported here, so it is a bool store and nothing more
//     C  (hw->0x368)->vt[0x118]((hw->0x20838)->0x64)   ONE register read
//     D  (hw->0x20820 = AMDGFX10UserQueueManager)
//            ->setSchedulerResources(0, C, 0, hw->0x20844)
//          -> AMDGFX10HIQHWChannel::submitSetResourcesPacket   (tail jump)
//               getHIQFrame: getRingBlock(0x20) + 32 filler dwords, both
//                            trivial (0xbe6c39e has no loop at all)
//               SET_RESOURCES 0xC006A000, then vt[0x328] at frame+0x40
//            -> submitFrame(true): commitBlock(0x20), doorbell vt[0x158],
//            -> AMDHWChannel::waitForHwStamp        <- the SAME function our
//                                                      2en hook already routes
//
// So the HIQ - the user-queue scheduler's own channel - submits a
// SET_RESOURCES packet exactly as the KIQ does, and 2gm established that
// AMDHWRegisters::read() and MCIL_WaitFor() reach the peer as request 0x14.
// A poll in here is the ~5600/s flood 2gk measured.
//
// What does NOT fit that story, and is why this is a split and not a fix:
// -rbgpustampwait is set and -rbgpustampraw is not, so our 2en bounded wait
// caps at 2000 ms and returns false into Apple's own failure path. A 2 s cap
// cannot produce a flood that ran for 100+ seconds in the 2gk boot. Either
// the stop is before waitForHwStamp, or the bounded wait is not reached on
// this channel, and only marks can say which.
//
// All five targets checked before writing a line:
//   nm -m: all external and defined in X6000's __TEXT (milestone-2c rule)
//   rule 9: first 16 bytes of each are pushes and movs - the first relative
//           branch is at +0x15 / +0x12 / +0x45 / +0x12 / +0x1c, every one
//           far outside the 5-byte patch window, so no displaced rel8 can
//           land in Lilu's trampoline (the 2gb/2ga panic)
//
// Observe-only. The marks are gated on 0x1B being in flight exactly as 2gn's
// are, so powerUpHW's own calls to these functions write nothing, and all of
// them land in the stage-5/6 region where notes are proven - not the ~500 us
// chain region 2fo/2ft showed must stay clear.
static void hooked_rt_hardwareDidStart_2go(void *hw) {
    rbgpu_diag_inc("rt_hds_calls");
    rbgpu_2gn_mark("rt:E");
    if (orig_rt_hardwareDidStart_2go) { orig_rt_hardwareDidStart_2go(hw); }
    // rt:R present with ssr:E absent puts the stop in request 0x1D (our own
    // peer), the single register read C, or the dispatch to D.
    rbgpu_2gn_mark("rt:R");
}

static void hooked_vmm_postInit_2go(void *vmm) {
    rbgpu_diag_inc("vmm_postinit_calls");
    rbgpu_2gn_mark("vmm:E");
    if (orig_vmm_postInit_2go) { orig_vmm_postInit_2go(vmm); }
    rbgpu_2gn_mark("vmm:R");
}

static void hooked_uqm_setSchedulerResources_2go(void *uqm, uint16_t a,
                                                 uint32_t b, uint32_t c,
                                                 uint32_t d) {
    rbgpu_diag_inc("ssr_calls");
    char t[48];
    snprintf(t, sizeof(t), "ssr:E(%u,%#x,%u,%#x)", a, b, c, d);
    rbgpu_2gn_mark(t);
    if (orig_uqm_setSchedulerResources_2go) {
        orig_uqm_setSchedulerResources_2go(uqm, a, b, c, d);
    }
    rbgpu_2gn_mark("ssr:R");
}

static bool hooked_hiq_submitSetResourcesPacket_2go(void *ch, uint16_t a,
                                                    uint32_t b) {
    rbgpu_diag_inc("hiq_setres_calls");
    rbgpu_2gn_mark("hiq:E");
    bool ret = orig_hiq_submitSetResourcesPacket_2go
                   ? orig_hiq_submitSetResourcesPacket_2go(ch, a, b)
                   : false;
    // The false when orig is NULL is OURS, not a hardware verdict - and the
    // mark says so by carrying the value.
    rbgpu_2gn_mark(ret ? "hiq:R=1" : "hiq:R=0");
    return ret;
}

static bool hooked_hiq_submitFrame_2go(void *ch, bool submit) {
    rbgpu_diag_inc("hiq_submitframe_calls");
    rbgpu_2gn_mark(submit ? "hsf:E(1)" : "hsf:E(0)");
    bool ret = orig_hiq_submitFrame_2go ? orig_hiq_submitFrame_2go(ch, submit)
                                        : false;
    // hsf:E present with hsf:R absent means the stop is inside commitBlock,
    // the doorbell vt[0x158], or waitForHwStamp - and our own stamp hook's
    // `stw:` marks split those three.
    rbgpu_2gn_mark(ret ? "hsf:R=1" : "hsf:R=0");
    return ret;
}

// 2gp (2026-10-16): the 2go trail named the hang by elimination, and the
// chain under it is now read end to end.
//
// `rbgpu-1b` = `b=10 perf:E perf:R hds:E rt:E vmm:E vmm:R rt:R
//               ssr:E(0,0x3b,0,0xc) hiq:E hsf:E(1) stw:E stw:R=1 hsf:R=1
//               hiq:R=1 ssr:R hds:R` - every mark returned, `ei:E` absent.
// So hardwareDidStart RETURNS and the stop is the one call between it and
// enableInterrupts: `(accel->0x380)->vtable[0x158]()` at 0xbe418b1.
//
// That object and that slot resolve with no boot spent:
//
//   accel->0x380 = this->vtable[0x880]() in IOGraphicsAccelerator2::start
//                  (0x14e4e4be) -> IOGraphicsAccelerator2::createEventMachine
//                  -> vtable[0x888] = AMDGraphicsAccelerator::newEventMachine
//                  -> new AMDRadeonX6000_AMDAccelEventMachine (0xd58 bytes)
//   call offset 0x158 -> dump +0x168 = AMDAccelEventMachine::finishAllStamps
//
// AMDAccelEventMachine::finishAllStamps (0xbe4b2c2) is a tracing wrapper: a
// kdebug pair around a NON-virtual call through the pointer at 0xbf90278 ->
// __ZTV24IOAccelEventMachineFast2, dump +0x168 (its sibling finishStamp uses
// +0x160 the same way, which is what proves these are dump offsets and not
// address-point offsets). The body is
// IOAccelEventMachineFast2::finishAllStamps (0x14e43a1e):
//
//   for (i = 0; i < this->0x30; i++)  sum += this->vt[0x150](i)
//
// - a BOUNDED loop over the stamp slots, calling finishStamp(i). And
// IOAccelEventMachineFast2::finishStamp (0x14e438c6) is where the loop is:
//
//   submitted = this->[0xfc + i*24] ; completed = this->[0xf8 + i*24]
//   if (submitted - completed <= 0)        return 0
//   completed = *(this->0x28[i])           read the channel's fence
//   if (submitted - completed <= 0)        return 0
//   14e43975:  r = this->vt[0x238](i, submitted, &completed)
//              if (r != 0) { handleFinishChannelRestart(r, i, 1); goto 975; }
//
// **The retry at 0x14e439a0 is unconditional and has no counter**, so any
// nonzero return spins forever - and `vt[0x238]` (dump +0x248) is
// AMDAccelEventMachine::waitForStamp (0xbe4b588), whose first two bails are
// nonzero before touching anything:
//
//   this->0xd38 == 0                 -> 0xE0000001
//   idx < 0, idx >= hw->0x1e78, or hw->0x1a78[idx] == NULL  -> 0xE00002C2
//
// That is a complete mechanism for 2gk's ~5600/s request-0x14 flood with no
// progress: a wait that errors immediately, a channel restart that touches
// registers (each register read going through isDeviceValid -> request 0x14,
// 2gm), and a retry with nothing to break it.
//
// **NOT established, and this is what the boot decides:** which return code,
// and therefore which of the two bails. Asserting one from the shape would be
// the 2gc mistake; the counters name it.
//
// All three targets checked before a line was written: `nm -m` shows each
// external and defined in its OWN kext's __TEXT (so routing by that kext's id
// is legitimate under the milestone-2c rule, and handleFinishChannelRestart
// is routed under IOAcceleratorFamily2, already in kextList since 2ct), and
// rule 9 is clean on all three - pushes and movs for the first 16 bytes, the
// first relative branch far outside the 5-byte patch window.
//
// **The channel is the prog2 sampler, not a mark.** waitForStamp runs
// thousands of times a second, so a log line or an NVRAM write per call would
// be the 2ez contention and the 2fo wedge at once. It gets plain counters;
// the +15/+35/+65 s samplers carry them out, and those are measured to fire
// while this very thread spins (2gk: all three landed with L advancing).
volatile uint32_t g_rb_2gp_fas_n      = 0;   // this->0x30, the stamp count
volatile uint32_t g_rb_2gp_wfs_calls  = 0;
volatile uint32_t g_rb_2gp_wfs_first  = 0;
volatile uint32_t g_rb_2gp_wfs_last   = 0;
volatile uint32_t g_rb_2gp_restarts   = 0;
volatile uint32_t g_rb_2gq_in_fas     = 0;   // 2gq: suppress per-call stw: marks
volatile uint32_t g_rb_2gq_stw        = 0;   // ... and count them instead

// 2gq: the stamp table itself, read before Apple walks it.
//
// finishStamp(i) takes its decision from three fields and nothing else
// (0x14e438da-0x14e43912, read instruction by instruction):
//
//   submitted = *(uint32 *)(evm + 0xfc + i*24)
//   completed = *(uint32 *)(evm + 0xf8 + i*24)
//   fence     = **(uint32 **)(evm->0x28 + i*8)
//
// and it waits only while `submitted - completed > 0` survives re-reading the
// fence. So the slots with work outstanding are exactly the ones that can
// reach waitForStamp, and Apple's own `GPURestartDeferred stampIdx=25` names
// one of them. At 0x1B time nothing of ours has submitted GPU work through
// the event machine at all, so a nonzero `submitted` here is either real work
// Apple issued or bookkeeping that was never initialised - and those need
// different fixes.
//
// Read-only, bounded at 64 slots, and every fence pointer is checked for the
// kernel-VA range before it is dereferenced (the 2gh rule, after the 2gg
// panic read `1 + 0x48`). A pointer that fails is printed as `?` rather than
// as a value that could be mistaken for a measurement.
// 2gv: strip only the shared "AMDRadeonX6000_" prefix. The 2gu note printed
// `h=MAChannel`, because the old loop walked to the LAST 'D' followed by an
// uppercase letter and so ate into "SDMAChannel" itself. The real class was
// AMDRadeonX6000_AMDSDMAChannel, and a mangled class name is exactly the
// kind of reading that cannot be checked later. A fixed prefix cannot
// over-trim.
static const char *rbgpu_shortclass_2gv(const char *cn) {
    static const char pfx[] = "AMDRadeonX6000_";
    size_t n = sizeof(pfx) - 1;
    if (!strncmp(cn, pfx, n)) { return cn + n; }
    return cn;
}

static void rbgpu_dump_stamps_2gq(void *evm, char *out, size_t cap) {
    out[0] = 0;
    if (!evm) { snprintf(out, cap, "noevm"); return; }
    uintptr_t e = reinterpret_cast<uintptr_t>(evm);
    uint32_t n = *reinterpret_cast<volatile uint32_t *>(e + 0x30);
    // 2gw: the bound was 64 and the 2gv boot came back with n=65, so the one
    // boot that finally broke the restart loop is the one whose stamp table
    // went unread. 64 was an arbitrary round number, not a fact about the
    // structure; the guard's real job is to refuse a count that would make
    // this walk read past the allocation, and 256 does that just as well
    // while leaving room for the table to grow again. The refusal still
    // SAYS so rather than returning quietly (rule: absence is not a result).
    if (n > 256) { snprintf(out, cap, "n=%u REFUSED(>256)", n); return; }
    void **fences = *reinterpret_cast<void ***>(e + 0x28);
    uint32_t outstanding = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t cmp = *reinterpret_cast<volatile uint32_t *>(e + 0xf8 + i * 24);
        uint32_t sub = *reinterpret_cast<volatile uint32_t *>(e + 0xfc + i * 24);
        if (static_cast<int32_t>(sub - cmp) <= 0) { continue; }
        outstanding++;
        // The fence is what finishStamp re-reads before it decides to wait.
        char fv[24];
        snprintf(fv, sizeof(fv), "?");
        if (fences) {
            void *fp = fences[i];
            if (reinterpret_cast<uintptr_t>(fp) >= 0xffffff8000000000ULL) {
                snprintf(fv, sizeof(fv), "%u",
                         *reinterpret_cast<volatile uint32_t *>(fp));
            }
        }
        // 2gr/2gs: name the channel behind this slot.
        // `AMDAccelEventMachine::eventTimeout` (0xbe4b991) does the lookup
        // Apple itself uses, and it is the accelerator's array, not the
        // hardware's:
        //
        //   accel = evm->0x10
        //   if (idx < 0 || idx >= accel->0x1e78) -> NULL
        //   ch = accel->0x1a78[idx]
        //
        // which is the SAME array and bound `waitForStamp`'s second bail
        // tests - so a Timeout rather than 0xE00002C2 already proves this
        // index is in range and the pointer non-NULL (2gp).
        //
        // 2gs: the channel's OWN fields are NOT read any more. 2gr read
        // +0x80/+0x84/+0xc8 here, which are `AMDHWChannel`'s layout
        // (2ds/2dw/2dy) - and the class that came back is
        // AMDRadeonX6000_AMDAccelChannel, whose vtable (0xbf92998, 0x278
        // bytes) sits in the IOAccelChannel2 hierarchy and not in the
        // IAMDHWChannel one (0xbfa8a80+, ~0x3d0-0x440). Those three numbers
        // were a different class's offsets and meant nothing. Reading the
        // fields in the same step that named the class is what allowed it.
        const char *cn = "?";
        void *ch = nullptr;
        void *accel = *reinterpret_cast<void **>(e + 0x10);
        if (reinterpret_cast<uintptr_t>(accel) >= 0xffffff8000000000ULL) {
            uintptr_t a = reinterpret_cast<uintptr_t>(accel);
            uint32_t nch = *reinterpret_cast<volatile uint32_t *>(a + 0x1e78);
            if (i < nch) {
                ch = *reinterpret_cast<void **>(a + 0x1a78 + i * 8);
            }
        }
        // 2gt: the channel NAMES ITSELF. AMDAccelChannel::init (0xbe443dd)
        // does `strlcpy(this + 0x188, name, 0x10)`, so every one of these
        // carries the string createAccelChannels passed it - the hardware
        // channel's own `vt[0x200]` name, or the literals "VMPT" and
        // "DisplayPipe%d". That beats inferring the slot from array
        // arithmetic, which is what the 2gr field read did wrong.
        char nm[20];
        nm[0] = 0;
        if (reinterpret_cast<uintptr_t>(ch) >= 0xffffff8000000000ULL) {
            if (auto o = OSDynamicCast(OSObject, reinterpret_cast<OSObject *>(ch))) {
                if (auto m = o->getMetaClass()) { cn = m->getClassName(); }
            }
            const volatile char *p188 =
                reinterpret_cast<const volatile char *>(
                    reinterpret_cast<uintptr_t>(ch) + 0x188);
            size_t w = 0;
            for (size_t q = 0; q < 16 && w < sizeof(nm) - 1; q++) {
                char c = p188[q];
                if (!c) { break; }
                // Only printable ASCII: anything else means this is not the
                // 16-byte name buffer and the reading says nothing.
                if (c < 0x20 || c > 0x7e) { w = 0; break; }
                nm[w++] = c;
            }
            nm[w] = 0;
        }
        // 2gu: the one field that says whether this channel has hardware
        // behind it at all. AMDAccelChannel::init (0xbe44380) does
        // `this->0xe8 = hwChannel` and then BRANCHES ON IT BEING NULL
        // (0xbe4438e): a backed channel allocates 0x190 * 0x400, a NULL one
        // allocates 0x90 * 1 and sets the same flag the index mismatch sets.
        // So init SUCCEEDS with no hardware channel, create returns the
        // object, and the array entry looks exactly like a real one - named,
        // in range, non-NULL. Every reading taken so far (s1/c0/f0, nch=63,
        // hwn=25, the "VMPT" name) is equally true of a channel with
        // `0xe8 == 0`, which is why none of them could settle this.
        //
        // Why it is the question: createAccelChannels asks for VMPT with
        //   hw->vt[0x320](2, 0, 0) = GFX10Hardware::getHWChannel(
        //                              _eAMD_CHANNEL_TYPE=2, prio=0, idx=0)
        // which falls to RTHardware::getHWChannel (0xbe973fa), computes
        // ENGINE_TYPE = ((caps->0x92 * 2 + 1) & 1) + 1 = 2 unconditionally,
        // and ends in AMDHardware::getHWChannel(engine, ring) (0xbea9994):
        //   `rdi = hw->[0x3b0 + engine*8]; if (!rdi) return NULL;`
        // Navi21Hardware::allocateHWEngines (0xbeca1ff) fills that array
        // [0]=Navi21PM4Engine, [1..4]=SDMAEngine_GC_10_3_0. Engine 2 is
        // therefore the SECOND SDMA engine - one of the three that
        // `-rbgpusdmaone` (2gi) deliberately gives ZERO queues. 2gi's own
        // comment said it: "if anything later submits to engines [1..3], it
        // finds no queues". VMPT is that submitter, and it submits during
        // startup, not from a GPU client.
        char hv[20];
        snprintf(hv, sizeof(hv), "?");
        if (reinterpret_cast<uintptr_t>(ch) >= 0xffffff8000000000ULL) {
            void *hwc = *reinterpret_cast<void **>(
                reinterpret_cast<uintptr_t>(ch) + 0xe8);
            if (!hwc) {
                snprintf(hv, sizeof(hv), "0");
            } else if (reinterpret_cast<uintptr_t>(hwc) >= 0xffffff8000000000ULL) {
                const char *hn = "?";
                if (auto o = OSDynamicCast(OSObject, reinterpret_cast<OSObject *>(hwc))) {
                    if (auto m = o->getMetaClass()) { hn = m->getClassName(); }
                }
                snprintf(hv, sizeof(hv), "%s", rbgpu_shortclass_2gv(hn));
            } else {
                // Absence is not a result: a non-kernel pointer is neither
                // "no hardware channel" nor a usable one, and must not be
                // read as either.
                snprintf(hv, sizeof(hv), "BAD");
            }
        }
        // Only the tail of the class name fits, and it is the distinctive
        // part (AMDRadeonX6000_AMD... is shared by all of them).
        const char *shortn = rbgpu_shortclass_2gv(cn);
        size_t k = strlen(out);
        if (k + 80 < cap) {
            snprintf(out + k, cap - k, "%s%u:s%u/c%u/f%s[%s \"%s\" h=%s]",
                     k ? "," : "", i, sub, cmp, fv, shortn,
                     nm[0] ? nm : "?", hv);
        }
    }
    // 2gs: three numbers that are defined by Apple's own code rather than
    // by a guessed layout, and that bound the question the slot cannot.
    //
    //   accel->0xa0   incrementStamp(i) does `evm[0xfc+i*24]++` AND
    //                 `evm->0x10->0xa0++` (0x14e4345a), so this is the total
    //                 number of stamps the whole driver has ever submitted.
    //                 If it is 1, slot 25's single submit is the only one.
    //   accel->0x1e78 the channel-array bound eventTimeout and waitForStamp
    //                 both test, so it says whether 25 is a real registered
    //                 channel or merely in range.
    //   nz            setStampBaseAddress (0x14e25aea) fills evm->0x28[i]
    //                 with `base + getStampOffset(i)` and getStampOffset is
    //                 `i*4` (0x14e25ae0), so the fences are one flat block
    //                 of n dwords at evm->0x20. Counting the nonzero ones
    //                 says whether the GPU has EVER written a stamp there.
    uint32_t tot = 0, nch = 0, nz = 0, hwn = 0, e2q = 0, e1q = 0;
    int have_e2 = 3;
    void *accel = *reinterpret_cast<void **>(e + 0x10);
    if (reinterpret_cast<uintptr_t>(accel) >= 0xffffff8000000000ULL) {
        uintptr_t a = reinterpret_cast<uintptr_t>(accel);
        tot = *reinterpret_cast<volatile uint32_t *>(a + 0xa0);
        nch = *reinterpret_cast<volatile uint32_t *>(a + 0x1e78);
        // 2gt: createAccelChannels (0xbe3b547) stores the HARDWARE channel
        // count at accel+0x1e7c, then creates "VMPT" at exactly that index
        // and the DisplayPipe channels after it. So hwn says where the
        // boundary is, and whether slot 25 is a hardware channel at all.
        hwn = *reinterpret_cast<volatile uint32_t *>(a + 0x1e7c);
        // 2gu: engine[2] itself, read the same way AMDHardware::getHWChannel
        // reads it (hw->[0x3b0 + engine*8]), and its queue count at +0x48 -
        // the exact field 2gi writes zero to. `e2=` says the engine object
        // exists; `e2q=` says how many queues it has NOW. This is what makes
        // `h=` above mean something: h=0 with e2q=0 is 2gi's bypass denying
        // VMPT its channel, while h=0 with e2q!=0 would be a different
        // failure entirely and 2gi would be exonerated.
        void *hw = *reinterpret_cast<void **>(a + 0x1a38);
        if (reinterpret_cast<uintptr_t>(hw) >= 0xffffff8000000000ULL) {
            // 2gv: engine[1] too - the remap target. The SDMA walk's base
            // is `hw + 0x3b8 + i*8` (rbgpu_walk_starthw_2fp), so 2gi's
            // engine[0], the ONE it deliberately leaves alone, is hw array
            // index 1; its engines [1..3] are hw indices [2..4]. VMPT's
            // engine 2 is therefore the FIRST engine 2gi disables, and the
            // working SDMA sits at index 1. `e1q` is what makes that
            // sentence checkable instead of arithmetic.
            void *e1 = *reinterpret_cast<void **>(
                reinterpret_cast<uintptr_t>(hw) + 0x3b8);
            if (reinterpret_cast<uintptr_t>(e1) >= 0xffffff8000000000ULL) {
                e1q = *reinterpret_cast<volatile uint32_t *>(
                    reinterpret_cast<uintptr_t>(e1) + 0x48);
            }
            void *e2 = *reinterpret_cast<void **>(
                reinterpret_cast<uintptr_t>(hw) + 0x3c0);
            if (reinterpret_cast<uintptr_t>(e2) >= 0xffffff8000000000ULL) {
                have_e2 = 1;
                e2q = *reinterpret_cast<volatile uint32_t *>(
                    reinterpret_cast<uintptr_t>(e2) + 0x48);
            } else if (!e2) {
                have_e2 = 0;   // the array slot is NULL: getHWChannel returns NULL
            } else {
                have_e2 = 2;   // neither; says nothing, and must not be read as either
            }
        } else {
            have_e2 = 3;       // no hw interface to ask
        }
    }
    void *base = *reinterpret_cast<void **>(e + 0x20);
    if (reinterpret_cast<uintptr_t>(base) >= 0xffffff8000000000ULL) {
        volatile uint32_t *b = reinterpret_cast<volatile uint32_t *>(base);
        for (uint32_t i = 0; i < n; i++) { if (b[i]) { nz++; } }
    }
    size_t k = strlen(out);
    snprintf(out + k, cap - k,
             "%s out=%u tot=%u nch=%u hwn=%u nz=%u e2=%d e2q=%u e1q=%u",
             k ? "" : "none", outstanding, tot, nch, hwn, nz, have_e2, e2q,
             e1q);
}

static uint32_t hooked_evm_finishAllStamps_2gp(void *evm) {
    if (evm) {
        g_rb_2gp_fas_n = *reinterpret_cast<volatile uint32_t *>(
            reinterpret_cast<uintptr_t>(evm) + 0x30);
    }
    char st[256];   // 2gr: each slot now carries its channel
    rbgpu_dump_stamps_2gq(evm, st, sizeof(st));
    char t[200];
    snprintf(t, sizeof(t), "fas:E(n=%u|%s)", g_rb_2gp_fas_n, st);
    rbgpu_2gn_mark(t);
    // 2gq: from here the restart loop resubmits KIQ work forever, so the
    // per-call stw: marks stop being evidence and start being the thing that
    // overflows the trail. They are counted instead; prog2 carries the count.
    g_rb_2gq_in_fas = 1;
    uint32_t ret = orig_evm_finishAllStamps_2gp
                       ? orig_evm_finishAllStamps_2gp(evm)
                       : 0;
    // The 0 when orig is NULL is OURS; the mark carries the value either way.
    char r[40];
    snprintf(r, sizeof(r), "fas:R=%#x", ret);
    rbgpu_2gn_mark(r);
    return ret;
}

static uint32_t hooked_evm_waitForStamp_2gp(void *evm, int idx, uint32_t stamp,
                                            uint32_t *out) {
    uint32_t ret = orig_evm_waitForStamp_2gp
                       ? orig_evm_waitForStamp_2gp(evm, idx, stamp, out)
                       : 0;
    // No log, no NVRAM: this is called thousands of times a second.
    if (g_rb_2gp_wfs_calls == 0) { g_rb_2gp_wfs_first = ret; }
    g_rb_2gp_wfs_last = ret;
    g_rb_2gp_wfs_calls++;
    return ret;
}

static void hooked_evm_handleFinishChannelRestart_2gp(void *evm, int a, int b,
                                                      uint64_t c) {
    g_rb_2gp_restarts++;
    if (orig_evm_handleFinishChannelRestart_2gp) {
        orig_evm_handleFinishChannelRestart_2gp(evm, a, b, c);
    }
}

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

static void rbgpu_gate_2en(uint32_t stage, const char *what);

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
    // 2fh (2026-10-13): the one bit that splits the span after gate 75.
    //
    // doStart after the mapQueues loop (0xbea1416-0xbea14f7), disassembled:
    //   bea1420  (this->0x18)->vtable[0x3b8]()            -> this->0x340
    //   bea143b  [0x148](0xF60..0B CP_RB0_RPTR)   -> bea1456 [0x118](off)
    //   bea1469  [0x148](0x1DF5..0B CP_RB0_WPTR_HI)-> bea1481 [0x118](off)
    //   bea1495  [0x148](0x1DF4..0B CP_RB0_WPTR)  -> bea14ae [0x118](off)
    //   bea14c5  orl 0x54(%rcx)                            channel+0x54
    //   bea14d3  waitForActiveValue(0, 1, 0)
    // so **vtable[0x118] is the register READ** (0x148 resolves, 0x120 writes),
    // and those three are the first GC register READS Apple makes in this path -
    // the same class of access 2fe measured stalling non-deterministically.
    //
    // And waitForActiveValue itself is the other candidate. With b2 = 0:
    //   bea15f4  andb -0x34(%rbp), %al   ; b2 = 0 -> al = 0
    //   bea15f9  jne bea1611             ; ALWAYS taken: skips the 0x3e7 counter
    //                                    ; AND the IODelay(0x64)
    //   bea1611  testb $1,%r14b ; jne    ; spin while (active ^ b1) & 1
    // a tight delay-free unbounded spin on CP_GFX_HQD_ACTIVE whose only escape
    // is the register reading 0xffffffff - and 2ew's f10 = 0 means Apple's
    // command-9 graphics-queue create never runs here, so nothing ever makes
    // that HQD active. 2cp forces the 4th parameter, which IS b2/ecx, so the
    // bound should be restored; these gates say whether 2cp is even reached.
    //
    // Bands 90/95 are clear of 60..67 / 70..77 / 80..87 / 100+ / 150+.
    char wenter[96];
    snprintf(wenter, sizeof(wenter), "waitForActiveValue ENTER ring=%u exp=%d bnd=%d",
             ring, expected ? 1 : 0, bounded ? 1 : 0);
    rbgpu_gate_2en(90, wenter);

    // 2fi (2026-10-13): run Apple's own poll HERE, with gates inside it.
    //
    // The 2fh boot landed gate 90 with bnd=1 and never landed 95. So 2cp DID
    // force the cap (the counter at 0xbea15fe and the _IODelay(0x64) at
    // 0xbea160c are both live) and the loop STILL does not return - where a
    // capped 1000 x 100 us poll owes an answer in about 100 ms. What a
    // bounded loop cannot explain, a single stalled access inside it can, and
    // each iteration makes two: a GRBM_GFX_CNTL write and a
    // CP_GFX_HQD_ACTIVE read through pm4+0x30.
    //
    // This is the 2en pattern, not 2cy's. 2cy substituted its own value and
    // then narrated it as a hardware result. This reproduces Apple's loop
    // instruction for instruction from the disassembly above - resolve
    // (vt[0x148]), write the selector (vt[0x120]), read the HQD (vt[0x118]),
    // exit on 0xffffffff, test (active ^ expected) & 1 - and only replaces
    // the 1000-iteration count with 64, so the gates can say WHICH access
    // stalls. If it completes without the ring going active it returns false
    // into Apple's own failure path, which is the honest outcome: 2ew's
    // f10 = 0 means the graphics queue is never created, so the HQD cannot
    // go active and false is what Apple's own code would eventually return.
    //
    // -rbgpuwfavraw restores Apple's original, mirroring -rbgpustampraw.
    static int wfav_raw = -1;
    if (wfav_raw < 0) { wfav_raw = checkKernelArgument("-rbgpuwfavraw") ? 1 : 0; }

    bool ret = false;
    void *robj = g_rb_2fb_regobj;
    if (wfav_raw || !robj || ring != 0) {
        rbgpu_gate_2en(99, wfav_raw ? "-rbgpuwfavraw: calling Apple's ORIGINAL"
                                    : "no accessor or ring!=0: Apple's ORIGINAL");
        ret = orig_pm4_waitForActiveValue_2cp
                  ? orig_pm4_waitForActiveValue_2cp(pm4, ring, expected, bounded)
                  : false;
    } else {
        void **rvt = *reinterpret_cast<void ***>(robj);
        auto resolve = reinterpret_cast<uint32_t (*)(void *, uint64_t)>(
            rvt ? rvt[0x148 / sizeof(void *)] : nullptr);
        auto rd = reinterpret_cast<uint32_t (*)(void *, uint32_t)>(
            rvt ? rvt[0x118 / sizeof(void *)] : nullptr);
        auto wr = reinterpret_cast<void (*)(void *, uint32_t, uint32_t)>(
            rvt ? rvt[0x120 / sizeof(void *)] : nullptr);
        if (!resolve || !rd || !wr) {
            rbgpu_gate_2en(98, "a slot of pm4+0x30 is NULL - polled nothing, "
                               "the false below is an INSTRUMENT failure");
        } else {
            uint32_t sel_off = resolve(robj, 0xDC20000000Bull);   // GRBM_GFX_CNTL
            char g91[96];
            snprintf(g91, sizeof(g91), "GRBM_GFX_CNTL resolved to dword 0x%x", sel_off);
            rbgpu_gate_2en(91, g91);

            wr(robj, sel_off, 0);                                 // ring 0 selector
            rbgpu_gate_2en(92, "GRBM_GFX_CNTL selector 0 WRITTEN");

            uint32_t act_off = resolve(robj, 0x1E800000000Bull);  // CP_GFX_HQD_ACTIVE
            char g93[96];
            snprintf(g93, sizeof(g93), "CP_GFX_HQD_ACTIVE resolved to dword 0x%x", act_off);
            rbgpu_gate_2en(93, g93);

            uint32_t v = rd(robj, act_off);
            char g94[96];
            snprintf(g94, sizeof(g94), "first CP_GFX_HQD_ACTIVE read = 0x%08x", v);
            rbgpu_gate_2en(94, g94);

            int iters = 1;
            bool dead = (v == 0xFFFFFFFFu);
            while (!dead && ((v ^ (expected ? 1u : 0u)) & 1u) && iters < 64) {
                wr(robj, sel_off, 0);
                v = rd(robj, act_off);
                if (v == 0xFFFFFFFFu) { dead = true; break; }
                iters++;
            }
            ret = !dead && (((v ^ (expected ? 1u : 0u)) & 1u) == 0);
            char g96[96];
            snprintf(g96, sizeof(g96), "poll done iters=%d last=0x%08x dead=%d ret=%d",
                     iters, v, dead ? 1 : 0, ret ? 1 : 0);
            rbgpu_gate_2en(96, g96);
            rbgpu_log("2fi: our bounded poll of CP_GFX_HQD_ACTIVE ran %d iterations, "
                      "last value 0x%08x, returning %d. This is OUR value, not a "
                      "hardware verdict. 2fi predicted a false here because "
                      "2ew's f10=0 keeps Apple's command-9 create from running; "
                      "the chip REFUTED that - CP_GFX_HQD_ACTIVE read 1 on the "
                      "first poll, so MAP_QUEUES made the graphics HQD active "
                      "without the command-9 path, and Apple's own loop would "
                      "also exit at once.",
                      iters, v, ret ? 1 : 0);
        }
    }
    g_rb_2cp_wfav_ret = ret ? 1 : 0;
    // 2fj (2026-10-13): gate 95 is the FIRST statement after the poll, and the
    // three wfav_* diag sets and the 2cp log line that used to sit here are
    // GONE. The 2fi trail reached gate 96 and not gate 95, and the whole span
    // between them was ours: three rbgpu_diag_set_num calls, one rbgpu_log and
    // one snprintf. Emptying such a span is the only move that has ever
    // advanced this frontier (2eu at 50->54, 2ez at 151->45, 2fe at the
    // register dump).
    //
    // The diag sets were dead weight in any case: rbgpu_nvram_quiesce() fires
    // at gate 40, so nothing written to the diag table after it can reach
    // NVRAM, and gate 95's own text carries the return value.
    rbgpu_gate_2en(95, ret ? "waitForActiveValue RETURNED 1 - ring ACTIVE"
                           : "waitForActiveValue RETURNED 0");
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
// 2en (2026-10-13): the wedge that came AFTER the fix, and the two things
// that let it happen without leaving a trace.
//
// The 2em boot worked. Its NVRAM record: ptb_written=1, ptb_rel_holds=1,
// ptb_back == ptb_want == 0x78fdfa001, vmf_st_wait=0, vmf_st_post=0 and
// stamp_ret=1. The GPUVM MAPPING_ERROR that every boot since 2dz latched at
// the ring is GONE, and the GPU wrote the completion fence, so doStart
// continued past submitSetResourcesPacket for the first time in this
// project. Then the machine wedged: tools/plugin_check.sh never returned
// (ioreg blocks on the IOService lock the wedged start() thread holds),
// Ctrl-C could not help because the process was in a kernel wait, and
// `reboot` could not finish for the same reason. No panic, and the console
// had stopped updating long before, so the photograph showed post-startKIQ
// while NVRAM showed the boot had reached at-stamp-wait and succeeded.
//
// WHY it could wedge. doStart's remaining gates are initGraphicsMQD and five
// submitMapQueuesPacket calls, and every one of those goes through
// submitKIQFrame -> waitForHwStamp again. Apple's waitForHwStamp re-arms its
// 5 s poll for as long as the channel's vtable[0x2a0] says to keep waiting
// (disassembled in 2cy), so the FIRST wait succeeding does not bound the
// later ones. A later wait that never completes hangs the boot forever.
//
// So the real wait is now BOUNDED. This is not 2cy's skip, which substituted
// its own value and then narrated it as a hardware result - the poll below
// is Apple's own, reproduced from the disassembly recorded above: its
// enabled check (vtable[0x150]), its refresh (vtable[0x1a8]
// checkForTimestampUpdate, the only thing that moves channel+0x84), its
// completion test ((int32)(stamp - channel+0x84) <= 0) and its 100 us poll
// interval. The one difference is a cap in place of the re-arm. A wait that
// would have completed inside the cap still completes; one that would have
// spun forever returns false and takes Apple's own failure path, which 2cy
// already proved is survivable and logged.
//
// -rbgpustampraw restores Apple's unbounded wait. That is what wedged the
// machine on 2026-10-13; it is not a default.
static bool rbgpu_bounded_stamp_wait_2en(void *ch, uint32_t stamp, uint32_t cap_ms) {
    if (!ch) return false;
    void **vt = *reinterpret_cast<void ***>(ch);
    if (!vt) {
        rbgpu_log("2en: channel %p has a NULL vtable - cannot wait", ch);
        return false;
    }
    auto fn_enabled = reinterpret_cast<bool (*)(void *)>(vt[0x150 / 8]);
    auto fn_refresh = reinterpret_cast<void (*)(void *)>(vt[0x1a8 / 8]);
    if (!fn_enabled || !fn_refresh) {
        rbgpu_log("2en: vtable[0x150]=%p vtable[0x1a8]=%p - one is NULL, so the "
                  "bounded wait cannot run and returns false WITHOUT waiting. "
                  "That is an instrument failure, not a GPU result.",
                  reinterpret_cast<void *>(fn_enabled),
                  reinterpret_cast<void *>(fn_refresh));
        rbgpu_diag_set_num("bwait_vt_bad", 1);
        return false;
    }

    auto completed = [ch]() -> uint32_t {
        return *reinterpret_cast<volatile uint32_t *>(
            reinterpret_cast<uintptr_t>(ch) + 0x84);
    };

    if (!fn_enabled(ch)) {
        rbgpu_log("2en: isEnabled() is false, so Apple's own wait would return "
                  "false here too");
        rbgpu_diag_set_num("bwait_disabled", 1);
        return false;
    }
    if (static_cast<int32_t>(stamp - completed()) <= 0) {
        rbgpu_log("2en: stamp %u was already completed (+0x84=%u) before waiting",
                  stamp, completed());
        return true;
    }

    const uint32_t iters = cap_ms * 10;   // 100 us per iteration
    uint64_t t0 = mach_absolute_time();
    for (uint32_t i = 0; i < iters; i++) {
        fn_refresh(ch);
        if (static_cast<int32_t>(stamp - completed()) <= 0) {
            rbgpu_diag_set_num("bwait_iters", i + 1);
            rbgpu_log("2en: BOUNDED wait satisfied after %u polls (~%u us, cap %u ms): "
                      "+0x84 reached %u", i + 1, (i + 1) * 100, cap_ms, completed());
            (void)t0;
            return true;
        }
        IODelay(100);
    }
    rbgpu_diag_set_num("bwait_iters", iters);
    rbgpu_diag_set_num("bwait_timeout", 1);
    rbgpu_log("2en: BOUNDED wait gave up after %u ms (+0x80=%u +0x84=%u asked %u). "
              "Apple's wait would have re-armed here and hung the boot; this "
              "returns false and takes Apple's own failure path instead.",
              cap_ms,
              *reinterpret_cast<volatile uint32_t *>(reinterpret_cast<uintptr_t>(ch) + 0x80),
              completed(), stamp);
    return false;
}

// 2en: a breadcrumb that survives a wedge. Every diag set schedules the
// NVRAM refresh on its own thread_call, which is why ptb_written and
// stamp_ret reached NVRAM from a thread that then wedged - so a single
// number updated at each gate says exactly how far the boot got, even when
// the console has stopped and the IORegistry dies with the boot. The 2em
// wedge left no such number and cost a power cycle to learn nothing.
// 2es (2026-10-13): a mark that does NOT write NVRAM.
//
// The 2eq boot stopped at gate 51 where 2ep stopped at 53 - EARLIER, in
// straight-line code that had worked the boot before. That span is three
// pointer reads and two diag sets; deterministic code cannot stop at
// different instructions on different boots, so the non-deterministic thing
// in it is the only candidate: the synchronous NVRAM write itself. Caching
// /options (2eq) removed the registry walk and did not help, which rules that
// mechanism out and leaves the flash commit - a lock and a device transaction
// taken dozens of times from inside IOService::start.
//
// The control already exists: the 2eo boot had the outer gates and NO marks
// inside this hook, reached gate 50, and left the machine alive enough to run
// plugin_check over SSH with a 43 KB log. So the outer gates are survivable
// and the inner ones are not. These become log-only - the diag table and
// IOLog, both of which that surviving boot captured in full.
// 2eu: the initGraphicsMQD object walk, off unless -rbgpumqdwalk is set.
// Checked once, lazily, so the boot-arg lookup is not repeated per call.
static bool rbgpu_mqd_walk_2eu(void) {
    static int cached = -1;
    if (cached < 0) {
        cached = checkKernelArgument("-rbgpumqdwalk") ? 1 : 0;
    }
    return cached == 1;
}

static void rbgpu_mark_2es(uint32_t stage, const char *what) {
    rbgpu_diag_set_num("gfxmqd_mark", stage);
    rbgpu_log("2es: mark %u - %s", stage, what);
}

static void rbgpu_gate_2en(uint32_t stage, const char *what) {
    // 2fo (2026-10-14): the whole trail is OFF when -rbgpunogates is set, and
    // the question that switch asks is the one 24 boots never asked.
    //
    // The accounting since 2em: about 7 of those boots produced a fact about
    // the chip and about 17 went into chasing this instrument, through six
    // structural hypotheses that were each refuted on hardware. Meanwhile
    // every measurement inside the chain says it WORKS - setres returns 1,
    // initGraphicsMQD returns in 223 us, mapQueues returns with real GPU
    // stamps, the ring-state reads complete, CP_GFX_HQD_ACTIVE reads 1 and
    // 2fi ran the poll to ret=1. There is no failure anywhere in 500 us of
    // chain; there is only a thread that does not come back.
    //
    // And the one thing present in all 24 and absent from the twenty
    // milestones before 2eo is ~24 synchronous NVRAM device transactions
    // inside IOService::start. The heartbeat corroborates that the controller
    // stops answering partway through every boot (n=13..28 while the machine
    // lives for minutes, 2ff). The trail has never been removed and the chain
    // never run without it.
    //
    // With the gates off the quiesce below never fires either, so the diag
    // summary and RBGPULog keep publishing exactly as they did for 2ce-2en -
    // the configuration that was readable for twenty milestones. The boot
    // then either reaches a desktop, which no boot has since 2em, or wedges
    // identically with nothing of ours writing flash, which exonerates the
    // trail and makes the stop real.
    static int nogate = -1;
    if (nogate < 0) { nogate = checkKernelArgument("-rbgpunogates") ? 1 : 0; }
    if (nogate) {
        rbgpu_diag_set_num("gate_stage", stage);
        return;
    }

    // 2et: gate 40 is the first gate of the doStart chain. From here on the
    // refresh thread stops flushing the diag summary, so these synchronous
    // writes are the only NVRAM traffic. The 2es boot showed the two writers
    // contend: a refresh pass published the log with mark 51 in it and then
    // never finished its own flash commit, leaving the summary three gates
    // stale - which is the state the start() thread then walked into at
    // gate 54.
    if (stage >= 40) {
        rbgpu_nvram_quiesce();
        // 2fg (2026-10-13): the heartbeat is OFF unless -rbgpubeat is set.
        // Its question is ANSWERED: 2ey branch 1, the machine is alive and
        // the start() thread alone is stuck. What it still does is write
        // rbgpu-beat with a FORCESYNCNOW flash commit four times a second,
        // and from gate 40 the quiesce above means the gate trail and the
        // beat are the ONLY two NVRAM writers left in the boot.
        //
        // And the beat does not survive its own boot. It re-arms itself
        // unconditionally every 250 ms, so in the boots where plugin_check
        // ran in the armed boot and the machine demonstrably lived for
        // minutes (2ey, 2fa, 2fc, 2fd) n should read in the hundreds. It
        // reads 13-28, every time, across trail endpoints from 101 to 75.
        // The beat's loop contains exactly one thing that can block: its own
        // setProperty + IONVRAM-FORCESYNCNOW-PROPERTY. So synchronous flash
        // commits stop working partway through the boot, and the gate trail
        // uses the same mechanism.
        //
        // That is a candidate cause for the varying endpoint that is NOT the
        // thread: the trail may stop where the WRITES stop, not where the
        // thread stops. Taking the beat out roughly doubles what is left for
        // the gates and tests that directly.
        if (checkKernelArgument("-rbgpubeat")) {
            rbgpu_heartbeat_start();
        }
    }
    rbgpu_diag_set_num("gate_stage", stage);
    // 2eo: and synchronously, on this thread, because the diag path above
    // can be starved by the hang it is trying to locate.
    rbgpu_gate_nvram(stage, what);
    rbgpu_log("2en: gate %u - %s", stage, what);
}

static volatile int g_rb_2cx_stamp_calls = 0;
static volatile int g_rb_2cx_stamp_ret = -1;
static volatile uint64_t g_rb_2cx_stamp_us = 0;

static bool hooked_hwchannel_waitForHwStamp_2cx(void *ch, uint32_t stamp) {
    g_rb_2cx_stamp_calls++;
    rbgpu_diag_set_num("stamp_calls", static_cast<uint64_t>(g_rb_2cx_stamp_calls));
    // 2en: which wait we are in. The 2em boot succeeded on call 1 and the
    // wedge was on a later one, so the call index is what names it.
    rbgpu_diag_set_num("stamp_in_call", static_cast<uint64_t>(g_rb_2cx_stamp_calls));
    rbgpu_gate_2en(100 + g_rb_2cx_stamp_calls, "waitForHwStamp ENTER");
    // 2go: and in the 0x1B trail, so the HIQ's own wait is distinguishable
    // from commitBlock and the doorbell before it. Writes nothing unless
    // 0x1B is in flight.
    if (g_rb_2gq_in_fas) { g_rb_2gq_stw++; } else { rbgpu_2gn_mark("stw:E"); }
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
    rbgpu_dump_gc_regs_late("at-stamp-wait");

    // 2dw (2026-10-12): corrected reading of the stamp path, and the fence
    // itself. 2ds/2dt/2du/2dv read ONE BRANCH BACKWARDS and chased a non-bug
    // for four milestones. The real code (0xbe85214) is:
    //
    //   timestampUpdated(this)
    //     if (this->0xe0 == -1) {                    <-- the DIRECT path
    //         this->0x84 = *(uint32 *)this->0xc0;    reads the fence, always
    //         tail call this->0x38 vtable[0x230](this->0xc)
    //         return;
    //     }
    //     ... scheduler path: AMDSWScheduler::timestampUpdated(sched,
    //         this->0xe0 as an index, this->0xc0, &this->0x84)
    //
    // So +0xe0 is the channel's slot in the AMDSWScheduler and -1 means
    // "not scheduler-managed, poll my fence directly" - NOT "no writeback".
    // For a KIQ channel -1 is BY DESIGN: AMDHWChannel::init asks
    // this->vtable[0x1c8], which in AMDGFX10KIQHWChannel is the constant
    // thunk 0xbec77c6 (`return !vtable[0x140]()` where vtable[0x140] is
    // `movb $0x1,%al; retq`) - an unconditional false. init then takes
    // `je 0xbe84b06` with r14b already 1, so it SUCCEEDS while deliberately
    // skipping AMDSWScheduler::addHWChannel and the +0xe0 write. 2dv measured
    // exactly that (kiq_in_regchan 1, kiq_in_addchan 0) and the conclusion it
    // printed was wrong: nothing refused anything.
    //
    // And the KIQ does get a REAL hardware fence. init's other constant,
    // vtable[0x148] (= 0xbec77ac, `xorl %eax,%eax`), is also false, so init
    // takes the allocated-fence branch at 0xbe849cb:
    //
    //   +0xac = interface vtable[0x348]()        the fence slot INDEX
    //   +0xb0 = interface vtable[0x358](idx)     that slot's CPU VA
    //   +0xc0 = +0xb0                            what timestampUpdated reads
    //   +0x80 = 0
    //
    // (The branch at 0xbe849c2 instead sets +0xc0 = &this->0x84, a channel
    // with no real fence. Which one this channel took is measurable.)
    //
    // So the chain is: the GPU is told some MC address for that slot and
    // writes the completion value there; the CPU reads the slot's kernel VA.
    // 2ds measured +0x80 = 1 and +0x84 = 0, so the refresh DOES run and the
    // dword at +0xc0 is being read on every poll and is not the stamp. That
    // is the same shape as the SMU bug of 2dh/2di: a correct CPU pointer
    // paired with an address the hardware cannot reach.
    //
    // All plain reads. The page scan is clamped to the single 4 KB page that
    // contains +0xc0, which timestampUpdated itself dereferences every poll,
    // so it is mapped.
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
        uint32_t slot_idx  = u32at(0xac);
        uint64_t slot_b0   = u64at(0xb0);
        bool is_self = (wb_c0 == reinterpret_cast<uintptr_t>(ch) + 0x84);

        // 2dt: the 5 s wait lets the request-0x14 poll flood evict ~500 lines,
        // so anything logged BEFORE the wait is gone by the time the log is
        // read. Keep the values and reprint them in the one line that survives.
        g_rb_2dt_wb_c0 = wb_c0;
        g_rb_2dt_wb_e0 = wb_e0;
        g_rb_2dt_sub_before = submitted;
        g_rb_2dt_cmp_before = completed;
        g_rb_2dw_slot_idx = slot_idx;
        g_rb_2dw_slot_b0  = slot_b0;
        g_rb_2dw_is_self  = is_self ? 1 : 0;

        rbgpu_diag_set_num("stamp_submitted", submitted);
        rbgpu_diag_set_num("stamp_completed", completed);
        rbgpu_diag_set_num("stamp_wb_c0", wb_c0);
        rbgpu_diag_set_num("stamp_wb_e0", wb_e0);
        rbgpu_diag_set_num("fence_slot_idx", slot_idx);
        rbgpu_diag_set_num("fence_slot_b0", slot_b0);
        rbgpu_diag_set_num("fence_is_self", is_self ? 1 : 0);

        // 2dv's membership facts, with the conclusion corrected: a KIQ channel
        // being in registerChannel and not in addHWChannel is Apple's own
        // design, not a refusal. Kept because it is the fact that proves it.
        bool in_reg = rb_2dv_seen(g_rb_2dv_reg, g_rb_2dv_reg_n, ch);
        bool in_add = rb_2dv_seen(g_rb_2dv_add, g_rb_2dv_add_n, ch);
        rbgpu_diag_set_num("kiq_in_regchan", in_reg ? 1 : 0);
        rbgpu_diag_set_num("kiq_in_addchan", in_add ? 1 : 0);
        rbgpu_diag_set_num("regchan_seen_n", g_rb_2dv_reg_n);
        rbgpu_diag_set_num("addchan_seen_n", g_rb_2dv_add_n);
        rbgpu_log("2dv: THIS channel %p: registerChannel seen=%d addHWChannel seen=%d "
                  "(distinct reg=%u add=%u) - for a KIQ, reg-without-add is EXPECTED",
                  ch, in_reg, in_add, g_rb_2dv_reg_n, g_rb_2dv_add_n);

        rbgpu_log("2ds: channel stamps BEFORE the wait: submitted(+0x80)=%u "
                  "completed(+0x84)=%u asked-for=%u ; writeback +0xc0=0x%llx +0xe0=0x%llx",
                  submitted, completed, stamp, wb_c0, wb_e0);
        rbgpu_log("2dw: fence slot: index(+0xac)=%u va(+0xb0)=0x%llx ; +0xc0 %s "
                  "(%s) ; +0xe0=%s",
                  slot_idx, slot_b0,
                  is_self ? "== &this->0x84" : "is a separate location",
                  is_self ? "init took 0xbe849c2: NO real fence, the channel points at "
                            "its own completed counter"
                          : "init took 0xbe849cb: a real hardware fence slot from the "
                            "interface",
                  (wb_e0 == 0xFFFFFFFFFFFFFFFFull)
                      ? "-1, the DIRECT path, which is BY DESIGN for a KIQ "
                        "(vtable[0x1c8] is a constant false) and is NOT a fault"
                      : "a scheduler index");

        // 2dx (2026-10-12): the GPU side of the same writeback frame.
        //
        //   allocateChannelWriteBackFrame()      return hw->0x20650++
        //   getChannelWriteBackFrameCPUPtr(i)    return hw->0x510 + (i << 6)
        //   getChannelWriteBackFrameGPUAddr(i)   return hw->0x518->vt[0x128]()
        //                                               + (i << 6)
        //
        // Frames are 64 bytes apart, and the measured +0xc0 sits at page
        // offset 0x40 with slot index 1, which is exactly hw->0x510 + (1 << 6)
        // - so the CPU side is confirmed arithmetically. The GPU side is one
        // value nobody has read: hw->0x518 vtable[0x128](), the writeback
        // buffer's GPU base. If it is an FB offset rather than an absolute MC
        // address, the GPU writes the fence outside the window the CPU polls
        // and this is the 2dh/2di bug again, in a different allocator.
        //
        // startKIQ was handed ring MC 0xf40fb28000 on this same chip, so a
        // correct GPU address here is expected to be 0xF4-based too; that ring
        // address is the reference the log compares against.
        void *hwobj = *reinterpret_cast<void **>(
            reinterpret_cast<uintptr_t>(ch) + 0x20);
        if (orig_wb_gpuaddr_2dx && hwobj) {
            uint64_t g0 = orig_wb_gpuaddr_2dx(hwobj, 0);
            uint64_t g1 = orig_wb_gpuaddr_2dx(hwobj, slot_idx);
            uint64_t cpu_base = *reinterpret_cast<volatile uint64_t *>(
                reinterpret_cast<uintptr_t>(hwobj) + 0x510);
            uint32_t next_idx = *reinterpret_cast<volatile uint32_t *>(
                reinterpret_cast<uintptr_t>(hwobj) + 0x20650);
            rbgpu_diag_set_num("wb_gpu_frame0", g0);
            rbgpu_diag_set_num("wb_gpu_slot", g1);
            rbgpu_diag_set_num("wb_cpu_base", cpu_base);
            rbgpu_diag_set_num("wb_next_idx", next_idx);
            bool cpu_ok = (wb_c0 == cpu_base + (static_cast<uint64_t>(slot_idx) << 6));
            rbgpu_diag_set_num("wb_cpu_arith_ok", cpu_ok ? 1 : 0);
            rbgpu_log("2dx: writeback frames: hw=%p cpu_base(+0x510)=0x%llx "
                      "next_idx(+0x20650)=%u ; CPU ptr for slot %u = 0x%llx, "
                      "measured +0xc0 = 0x%llx -> arithmetic %s",
                      hwobj, cpu_base, next_idx, slot_idx,
                      cpu_base + (static_cast<uint64_t>(slot_idx) << 6), wb_c0,
                      cpu_ok ? "MATCHES" : "DOES NOT match");
            // 0xF4 is the carve-out base MMHUB reported in 2di. An address
            // without it cannot be what the ring addresses use.
            bool g1_carveout = ((g1 >> 32) == 0xF4);
            bool g1_zero_hi  = ((g1 >> 32) == 0);
            rbgpu_diag_set_num("wb_gpu_hi", g1 >> 32);
            rbgpu_diag_set_num("wb_gpu_carveout", g1_carveout ? 1 : 0);
            // 2dy: the field the fence packet actually uses. init sets +0xc0
            // (CPU) from vtable[0x358]; allocateMemoryResources sets +0xc8
            // (GPU) from vtable[0x350] at 0xbe84f05. They are assigned in
            // different functions, so one can be set while the other is not.
            uint64_t c8 = *reinterpret_cast<volatile uint64_t *>(
                reinterpret_cast<uintptr_t>(ch) + 0xc8);
            g_rb_2dt_wb_c8 = c8;
            rbgpu_diag_set_num("chan_c8", c8);
            rbgpu_diag_set_num("chan_c8_matches_wb", (c8 == g1) ? 1 : 0);
            rbgpu_log("2dy: channel+0xc8 (what the WRITE_DATA packet targets) = "
                      "0x%llx ; getChannelWriteBackFrameGPUAddr(%u) = 0x%llx -> %s",
                      c8, slot_idx, g1,
                      (c8 == g1)
                          ? "they MATCH, so the packet targets the right frame "
                            "and the fence failing is further down"
                      : (c8 == 0)
                          ? "+0xc8 is ZERO: allocateMemoryResources never set it "
                            "for this channel, so the fence packet writes to "
                            "address 0"
                          : "they DIFFER: the packet targets a different frame "
                            "than the one the CPU polls");
            rbgpu_log("2dx: GPU addr for frame 0 = 0x%llx, for slot %u = 0x%llx "
                      "(delta 0x%llx, expected 0x%x) ; ring MC from startKIQ = "
                      "0x%llx -> %s",
                      g0, slot_idx, g1, g1 - g0,
                      static_cast<uint32_t>(slot_idx) << 6, g_rb_kiq_ring_mc,
                      g1_carveout
                          ? "high word is 0xF4, the same carve-out the ring "
                            "lives in, so the address LOOKS right and the fence "
                            "not landing is something else"
                      : g1_zero_hi
                          ? "high word is ZERO while the ring is 0xF4-based: "
                            "this is an FB offset, not an absolute MC address, "
                            "and the GPU is writing the fence outside the page "
                            "the CPU polls - the 2dh/2di bug in the writeback "
                            "allocator"
                          : "high word is neither 0xF4 nor 0, so it is some "
                            "third address space and needs its own reading");
        } else {
            rbgpu_log("2dx: no getChannelWriteBackFrameGPUAddr (resolved=%d hw=%p), "
                      "so the GPU side of the fence was NOT measured this boot",
                      orig_wb_gpuaddr_2dx != nullptr, hwobj);
        }

        // The value timestampUpdated reads on every poll.
        if (wb_c0 && !is_self) {
            uint32_t fv = *reinterpret_cast<volatile uint32_t *>(wb_c0);
            g_rb_2dw_fence_bef = fv;
            rbgpu_diag_set_num("fence_val_before", fv);
            rbgpu_log("2dw: the fence dword at +0xc0 BEFORE the wait = 0x%08x "
                      "(asked-for stamp = %u)", fv, stamp);
        }

        if (submitted == completed) {
            rbgpu_log("2ds: +0x80 == +0x84, so checkForTimestampUpdate takes its EARLY "
                      "EXIT and never reads the fence at all. Nothing marked the frame "
                      "submitted, so the wait can only ever time out - this is "
                      "bookkeeping, not the GPU.");
        } else {
            rbgpu_log("2ds: a frame IS outstanding (+0x80 != +0x84), so "
                      "checkForTimestampUpdate runs and timestampUpdated reads the "
                      "fence on every poll. If +0x84 still does not move, the fence "
                      "memory is what is not being written.");
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
        // 2en: bounded by default. -rbgpustampraw is Apple's unbounded wait,
        // which wedged the machine on 2026-10-13 on a LATER call than the one
        // that succeeded.
        bool raw = checkKernelArgument("-rbgpustampraw");
        rbgpu_diag_set_num("bwait_raw", raw ? 1 : 0);
        if (raw) {
            rbgpu_log("2en: -rbgpustampraw: calling Apple's UNBOUNDED wait. If the "
                      "stamp does not arrive this boot hangs with no panic and no "
                      "log, and only a power cycle ends it.");
            ret = orig_hwchannel_waitForHwStamp_2cx
                      ? orig_hwchannel_waitForHwStamp_2cx(ch, stamp)
                      : false;
        } else {
            ret = rbgpu_bounded_stamp_wait_2en(ch, stamp, 2000);
        }
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
                          ? "  (+0xe0 -1 = the direct fence path, by design for a KIQ)"
                          : "  (+0xe0 = a scheduler index)");

            // 2dw: did the GPU write the completion value ANYWHERE in the fence
            // page? If the stamp turns up at a different offset, the fence
            // mechanism works and only the slot address handed to the hardware
            // is wrong - the 2dh/2di shape. Clamped to the one page that
            // timestampUpdated dereferences every poll, so it is mapped.
            if (g_rb_2dt_wb_c0 && !g_rb_2dw_is_self) {
                uint32_t fv = *reinterpret_cast<volatile uint32_t *>(g_rb_2dt_wb_c0);
                rbgpu_diag_set_num("fence_val_after", fv);
                rbgpu_log("2dw: the fence dword at +0xc0 AFTER the wait = 0x%08x "
                          "(BEFORE 0x%08x, asked-for %u) -> %s",
                          fv, g_rb_2dw_fence_bef, stamp,
                          // 2fl: this text asserted a CPU-side failure. Since
                          // 2em the wait SUCCEEDS on its first poll, so a
                          // landed fence is the working path, not a fault.
                          (fv >= stamp)
                              ? "the GPU wrote the fence where the CPU polls - "
                                "this is the working path"
                              : "the GPU never wrote this location");

                uintptr_t page = g_rb_2dt_wb_c0 & ~static_cast<uintptr_t>(0xFFF);
                uint32_t hits = 0, first_off = 0, first_val = 0;
                for (uint32_t o = 0; o < 0x1000; o += 4) {
                    uint32_t v = *reinterpret_cast<volatile uint32_t *>(page + o);
                    if (v == stamp) {
                        if (!hits) { first_off = o; first_val = v; }
                        hits++;
                    }
                }
                uint32_t c0_off = static_cast<uint32_t>(g_rb_2dt_wb_c0 - page);
                rbgpu_diag_set_num("fence_scan_hits", hits);
                rbgpu_diag_set_num("fence_scan_off", first_off);
                rbgpu_diag_set_num("fence_c0_off", c0_off);
                rbgpu_log("2dw: fence page 0x%llx scan for the value %u: %u hit(s), "
                          "first at +0x%03x (val 0x%08x); +0xc0 itself is at +0x%03x "
                          "-> %s",
                          static_cast<uint64_t>(page), stamp, hits, first_off,
                          first_val, c0_off,
                          (hits == 0)
                              ? "the value is nowhere in this page, so the GPU wrote "
                                "the fence somewhere else entirely (or never wrote it); "
                                "a single hit elsewhere would have meant a wrong slot "
                                "address, and this does not show that"
                              : (first_off == c0_off)
                              ? "it IS at +0xc0, so the read path is what is broken"
                              : "the value is in this page but at a DIFFERENT offset "
                                "than +0xc0: the fence works and the slot address given "
                                "to the hardware does not match the one the CPU polls - "
                                "the 2dh/2di shape. Note a small integer like this can "
                                "also occur by chance, so treat one hit as a lead, not "
                                "a result.");
            }
        }
        rbgpu_dump_gc_regs_late("post-stamp-wait");
    } else {
        rbgpu_log("2cy: skipping the wait (-rbgpustampwait not set) -> false, so the "
                  "boot fails fast instead of polling forever");
        ret = false;
    }
    uint64_t dt = mach_absolute_time() - t0;

    g_rb_2cx_stamp_ret = ret ? 1 : 0;
    g_rb_2cx_stamp_us = dt;
    // 2ez (2026-10-13): everything that is not the gate goes BEFORE it, so the
    // only thing between gate 151 and the `return` is the return. 2ey proved
    // the machine stays alive while this thread stops, so the trail locates
    // the thread - and the trail stops here. Emptying the span is the one move
    // that has ever shifted this frontier (2eu did it for 50->54).
    RB_STEP(10);   // 2fz: the original returned
    rbgpu_diag_set_num("stamp_ret", static_cast<uint64_t>(g_rb_2cx_stamp_ret));
    rbgpu_diag_set_num("stamp_ticks", dt);
    RB_STEP(11);   // the two diag sets returned
    rbgpu_log("2cx: waitForHwStamp -> %d %s (%llu ticks)", ret ? 1 : 0,
              ret ? "(the GPU executed it)"
                  : "(the stamp was not reached - NOT evidence the CP is dead; 2dq "
                    "measured RPTR=WPTR with CP_STAT busy, so it does execute)",
              dt);
    rbgpu_gate_2en(ret ? (150 + g_rb_2cx_stamp_calls) : (120 + g_rb_2cx_stamp_calls),
                   ret ? "waitForHwStamp RETURNED TRUE" : "waitForHwStamp returned false");
    RB_STEP(12);   // the log line returned; next is Apple's submitKIQFrame epilogue
    if (!g_rb_2gq_in_fas) {
        rbgpu_2gn_mark(ret ? "stw:R=1" : "stw:R=0");   // 2go, 0x1B-gated
    }
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

// 2fb (2026-10-13): the five register read-modify-writes Apple's doStart makes
// between submitSetResourcesPacket and initGraphicsMQD, disassembled:
//
//   bea12df  [0x148](0x1DE1..0B = CP_RB0_CNTL)   -> bea12f2 [0x128](v, 0x80000000)
//   bea130c  [0x148](0x1DE2..0B = CP_RB_RPTR_WR) -> bea131c [0x120](v, ..)
//   bea132f  [0x148](0x1DF4..0B = CP_RB0_WPTR)   -> bea133f [0x120](v, ..)
//   bea1359  [0x148](0x1DF5..0B = CP_RB0_WPTR_HI)-> bea1369 [0x120](v, ..)
//   bea1383  [0x148](0x1DE1..0B = CP_RB0_CNTL)   -> bea1396 [0x130](v, ..)
//
// The object is `this->0x30` - the same accessor 2ew measured at
// 0xffffffd39ba10000 and the same one initGraphicsMQD's regions I and J use.
// The ids decode in the form docs/RULES.md records, (dword offset << 32) | 11 with
// 11 = GC_HWID, and the names come from docs/linux-ref/gc_10_3_0_offset.h.
//
// These are the GRAPHICS ring registers. The KIQ is compute and works end to
// end; this is the first time anything in this path touches RB0. Note what is
// NOT claimed: 2dl read CP_RB0_CNTL directly through MMIO and got 0x00a00000,
// so the register is readable and "the GFX block is dead so reads stall" is
// contradicted by our own measurement. What has never been attempted is a
// WRITE, which is what 0x120/0x128/0x130 do.
//
// So the probe is the READ only, through Apple's own accessor, bracketed by
// gates, microseconds before Apple makes the identical call. No new write.

#define RB_2FB_CP_RB0_CNTL 0x1DE10000000Bull

static void rbgpu_probe_regread_2fb(void) {
    // 2fc (2026-10-13): EVERY exit path writes a gate. The 2fb version
    // returned silently when the accessor was missing, so "gate 46 absent"
    // could not tell "the thread stopped before the probe" from "the probe
    // skipped itself" - the ambiguity-of-absence this project has hit at
    // 2eh, 2ek and 2en, built in fresh.
    //
    // Gate 46 is written FIRST, before any check, and carries the accessor
    // pointer: the trail is synchronous and reliable, while the diag summary
    // now freezes earlier (a consequence of 2fa's rate limit) and no longer
    // reports gfxmqd_f30.
    void *obj = g_rb_2fb_regobj;
    char what[96];
    snprintf(what, sizeof(what), "setres tail done; accessor=%p", obj);
    rbgpu_gate_2en(46, what);

    if (!obj) {
        rbgpu_gate_2en(48, "probe SKIPPED: no accessor stashed at doStart");
        return;
    }
    void **vt = *reinterpret_cast<void ***>(obj);
    if (!vt) {
        rbgpu_gate_2en(48, "probe SKIPPED: accessor vtable NULL");
        return;
    }
    auto fn = reinterpret_cast<uint32_t (*)(void *, uint64_t)>(
        vt[0x148 / sizeof(void *)]);
    if (!fn) {
        rbgpu_gate_2en(48, "probe SKIPPED: vtable[0x148] NULL");
        return;
    }
    rbgpu_gate_2en(49, "calling accessor vtable[0x148] on the CP_RB0_CNTL id");
    uint32_t v = fn(obj, RB_2FB_CP_RB0_CNTL);
    char done[96];
    snprintf(done, sizeof(done), "vtable[0x148] RETURNED 0x%08x (0x3041 = seg0+off)", v);
    rbgpu_diag_set_num("rb0_cntl_via_accessor", v);
    rbgpu_gate_2en(47, done);
    rbgpu_log("2fe: accessor vtable[0x148](CP_RB0_CNTL id) = 0x%08x. seg0 0x1260 + "
              "mmCP_RB0_CNTL 0x1de1 = 0x3041 EXACTLY, so this slot RESOLVES an "
              "encoded id to a dword offset and does NOT read the register (2dl read "
              "the contents, 0x00a00000, through MMIO). It confirms seg0 = 0x1260 a "
              "second, independent way: Apple's accessor computes the same base.", v);
}

static bool hooked_kiq_submitSetResources_2cq(void *ch, const void *spec) {
    g_rb_2cq_setres_calls++;
    // 2cx: log on ENTRY. The 2026-10-12 armed boot stopped here with no line
    // at all, because every hook logged only after the original returned - a
    // hang therefore produced nothing. submitSetResourcesPacket tail-calls
    // submitKIQFrame -> waitForHwStamp, which waits for the GPU to execute
    // the packet, so "entered with no return" is the signal that matters.
    rbgpu_gate_2en(40, "submitSetResourcesPacket ENTER");
    rbgpu_log("2cx: submitSetResourcesPacket ENTER (ch=%p spec=%p) - waits for the "
              "GPU to execute the KIQ packet", ch, spec);

    // 2dk: the CP's own state at the one moment it matters. Everything before
    // this point reports success - PSP, GC and TTL hw_init all return success,
    // the yellow_carp firmware is loaded, RLC_STAT is the reference 0x25, the
    // SMU is up and says GFX is not in GFXOFF - and yet the stamp never
    // arrives. Read-only, and the only reading taken while the packet is
    // actually in flight.
    rbgpu_dump_gc_regs_late("pre-kiq-submit");

    bool ret = orig_kiq_submitSetResources_2cq
                   ? orig_kiq_submitSetResources_2cq(ch, spec)
                   : false;
    // 2ez: the gate is the FIRST thing after the original returns. Apple's
    // submitKIQFrame epilogue plus this one call is then the entire span from
    // gate 151, so a trail that still ends at 151 puts the stop in Apple's
    // few instructions with nothing of ours left to blame.
    rbgpu_gate_2en(ret ? 45 : 41, ret ? "submitSetResourcesPacket OK - doStart goes on "
                                        "to initGraphicsMQD"
                                      : "submitSetResourcesPacket FAILED");
    g_rb_2cq_setres_ret = ret ? 1 : 0;
    rbgpu_diag_set_num("setres_calls", static_cast<uint64_t>(g_rb_2cq_setres_calls));
    rbgpu_diag_set_num("setres_ret", static_cast<uint64_t>(g_rb_2cq_setres_ret));
    rbgpu_log("2cq: submitSetResourcesPacket(spec=%p) -> %d %s", spec, ret ? 1 : 0,
              ret ? "(OK)" : "(FAIL -> doStart exits via vtable[0x150], returns false)");
    return ret;
}

static bool hooked_pm4_initGraphicsMQD_2cq(void *pm4) {
    g_rb_2cq_gfxmqd_calls++;
    rbgpu_gate_2en(50, "initGraphicsMQD ENTER - never once exercised before 2em");
    rbgpu_log("2cx: initGraphicsMQD ENTER (pm4=%p)", pm4);

    // 2ep (2026-10-13): split this function, because the boot stops INSIDE it.
    //
    // The 2eo boot got here and no further: `nvram rbgpu-gate` read
    // "50 initGraphicsMQD ENTER" and the captured log's last line is the same
    // hook. Everything before it now passes - PT_BASE patched, no GPUVM
    // fault, the stamp arrived on the bounded wait's FIRST poll, and
    // submitSetResourcesPacket returned 1 for the first time in this project.
    //
    // initGraphicsMQD (0xbea2542) contains no loop of its own, so it is
    // blocking inside something it calls. Disassembled, its outbound calls
    // are:
    //
    //   bea255d  (this->0x18)->vtable[0x2d8]()        the memory object
    //   bea257d  r14->vtable[0x148]()                 same pair initComputeMQD
    //   bea25b3  r14->vtable[0x210](...)              uses (2cd walks these)
    //   bea2603  (this->0x18)->vtable[0x2c0]()        -> the SERVICE object
    //   bea2626  svc->vtable[0x128](9, 0)             destroy, if 0x310 set
    //   bea26a1  svc->vtable[0x120](9, 0, spec, out)  CREATE THE GFX QUEUE
    //   bea2713  (this->0x30)->vtable[0x148](0xf702000042) then [0x120](...,1)
    //   bea2740  (this->0x30)->vtable[0x148](0xe802000042) then [0x120](...,0)
    //
    // bea26a1 is the same slot and the same shape as startKIQ's call, which
    // uses command 8 and works; this one is command 9 and is the first thing
    // that has ever asked the layer below for a GRAPHICS queue. The last pair
    // are register read-modify-writes, and a read of a gated block can stall
    // the SMN bus - which is what the boot 6/7 RSMU timeouts were, and would
    // explain a hang that takes the whole machine rather than one thread.
    //
    // Note the standing suspicion this meets: the SMU has reported
    // gfx_min = gfx_max = 0 since 2di - GFX DPM never started - and this is
    // the first code to touch the graphics ring. The compute path (KIQ) is
    // working; the graphics one may have no clock at all.
    //
    // So walk Apple's own object chain FIRST, with a synchronous breadcrumb
    // between each step, and record the service function's address. All reads
    // of pointers plus one virtual call Apple is about to make anyway
    // (vtable[0x2c0], which only returns the service object). If the boot
    // stops at 51/52/53 the fault is in our walk; if it reaches 54 the
    // preconditions are sound and the hang is inside Apple's command-9 call
    // or the register pair after it - and `gfxmqd_svc_fn` names the function
    // to disassemble.
    //
    // 2eu (2026-10-13): the walk is OFF by default now. With the NVRAM
    // controller quiesced (2et) the missing gate 54 can no longer be blamed
    // on a blocked flash write, so the stop really is between gate 50 and
    // gate 54 - and the only substantive thing in that span is Apple's own
    // vtable[0x2c0] call, which Apple makes itself at 0xbea2603 a moment
    // later. With the walk gone there is NOTHING between the two gates but
    // the original call, so gate 54 appearing or not is decisive:
    //
    //   54 written  the span was ours; the hang is inside Apple's
    //               initGraphicsMQD - command-9 queue create or the GC
    //               register pair at the end
    //   50 only     two NVRAM writes with nothing between them still lose
    //               the second, and the gate mechanism itself is the problem
    //
    // -rbgpumqdwalk puts the walk back for a boot that wants the pointers.
    if (pm4 && rbgpu_mqd_walk_2eu()) {
        rbgpu_mark_2es(51, "initGraphicsMQD: reading pm4+0x18");
        void *memobj = *reinterpret_cast<void **>(
            reinterpret_cast<uintptr_t>(pm4) + 0x18);
        rbgpu_diag_set_num("gfxmqd_memobj", reinterpret_cast<uint64_t>(memobj));

        if (memobj) {
            void **mvt = *reinterpret_cast<void ***>(memobj);
            auto fn_2c0 = mvt ? reinterpret_cast<void *(*)(void *)>(mvt[0x2c0 / 8])
                              : nullptr;
            rbgpu_diag_set_num("gfxmqd_fn2c0", reinterpret_cast<uint64_t>(
                reinterpret_cast<void *>(fn_2c0)));
            rbgpu_mark_2es(52, "initGraphicsMQD: calling vtable[0x2c0] for the "
                               "service object");
            void *svc = fn_2c0 ? fn_2c0(memobj) : nullptr;
            rbgpu_diag_set_num("gfxmqd_svc", reinterpret_cast<uint64_t>(svc));
            rbgpu_mark_2es(53, "initGraphicsMQD: service object obtained");

            if (svc) {
                // 2eq: the 2ep boot stopped between 53 and 54, a span holding
                // only these few reads. Mark each one, so the next boot says
                // which - or says the span is clear and the breadcrumb's own
                // registry lookup (now cached) was the problem.
                rbgpu_mark_2es(56, "initGraphicsMQD: dereferencing the service vtable");
                void **svt = *reinterpret_cast<void ***>(svc);
                rbgpu_mark_2es(57, "initGraphicsMQD: service vtable read");
                void *fn120 = svt ? svt[0x120 / 8] : nullptr;
                void *fn128 = svt ? svt[0x128 / 8] : nullptr;
                rbgpu_mark_2es(58, "initGraphicsMQD: service slots read");
                rbgpu_diag_set_num("gfxmqd_svc_fn",
                                   reinterpret_cast<uint64_t>(fn120));
                rbgpu_diag_set_num("gfxmqd_svc_fn128",
                                   reinterpret_cast<uint64_t>(fn128));
                rbgpu_mark_2es(59, "initGraphicsMQD: service slots recorded");
                rbgpu_log("2ep: initGraphicsMQD service object %p: "
                          "vtable[0x120]=%p (command 9 = create the GRAPHICS "
                          "queue, the same slot startKIQ uses with command 8) "
                          "vtable[0x128]=%p ; pm4+0x310=%u (nonzero means the "
                          "destroy call at 0xbea2626 runs first)",
                          svc, fn120, fn128,
                          *reinterpret_cast<volatile uint8_t *>(
                              reinterpret_cast<uintptr_t>(pm4) + 0x310));
            } else {
                rbgpu_log("2ep: vtable[0x2c0] returned NULL - Apple's own code "
                          "would then call through a null service object, so "
                          "this is a precondition failure, not a hang");
            }
        }
    }
    // 2ew (2026-10-13): NOTHING between gate 50 and gate 54. The 2ev boot
    // put four field reads, four diag sets and one log line here and the
    // breadcrumb went back to 50, where 2eu - with this span empty - reached
    // 54 and got into Apple's code. Seven boots now agree that whatever this
    // hook does between the two gates decides how far the thread gets, so the
    // span stays empty and the field reads moved to the doStart hook, where
    // logging has been safe since 2ce.
    rbgpu_gate_2en(54, "initGraphicsMQD: calling the ORIGINAL - anything after "
                       "this is Apple's command-9 queue create or the GC "
                       "register pair at the end");
    bool ret = orig_pm4_initGraphicsMQD_2cq ? orig_pm4_initGraphicsMQD_2cq(pm4) : false;
    g_rb_2cq_gfxmqd_ret = ret ? 1 : 0;
    rbgpu_diag_set_num("gfxmqd_calls", static_cast<uint64_t>(g_rb_2cq_gfxmqd_calls));
    rbgpu_diag_set_num("gfxmqd_ret", static_cast<uint64_t>(g_rb_2cq_gfxmqd_ret));
    RB_STEP(50);   // 2fz: initGraphicsMQD returned
    rbgpu_log("2cq: initGraphicsMQD -> %d %s", ret ? 1 : 0,
              ret ? "(OK)" : "(FAIL -> doStart returns false)");
    // 2ff: gate last before the return, same rule as the mapQueues hook.
    rbgpu_gate_2en(ret ? 55 : 51, ret ? "initGraphicsMQD OK" : "initGraphicsMQD FAILED");
    return ret;
}

static bool hooked_kiq_submitMapQueues_2cq(void *ch, uint32_t ring, uint32_t a, uint64_t b,
                                           const void *spec, uint64_t c) {
    g_rb_2cq_mapq_calls++;
    rbgpu_gate_2en(60 + (ring & 0x7), "submitMapQueuesPacket ENTER - this one also "
                                      "waits for a stamp");
    rbgpu_log("2cx: submitMapQueuesPacket ENTER (ring=%u)", ring);
    RB_STEP(20 + (ring & 0x7));   // 2fz: about to call Apple's original
    bool ret = orig_kiq_submitMapQueues_2cq
                   ? orig_kiq_submitMapQueues_2cq(ch, ring, a, b, spec, c)
                   : false;
    // 2fl (2026-10-14): DENSE gates, the opposite of 2ff/2fj - and the reversal
    // is a measurement, not a change of mind. 2fk timed the trail against the
    // log: after rbgpu_nvram_quiesce() fires at gate 40 a gate costs ~6 US
    // (gates 40..152 are 479 us apart in total, Apple's own work included),
    // against ~6 MS for the three before the quiesce. So emptying this span was
    // saving microseconds, and three more gates here cost ~18 us in a span
    // measured at 89 us.
    //
    // 32 = Apple's original returned (so the stop was NOT in submitKIQFrame's
    //      epilogue), 33 = the two diag sets returned, then 70+ring as before
    //      with only the log line between. Every boundary in this span now has
    //      a gate, which no span in this family has ever had.
    RB_STEP(30 + (ring & 0x7));   // 2fz: Apple's original RETURNED
    rbgpu_gate_2en(32, "mapQueues: the ORIGINAL returned");
    if (!ret) {
        g_rb_2cq_mapq_fails++;
    }
    rbgpu_diag_set_num("mapq_calls", static_cast<uint64_t>(g_rb_2cq_mapq_calls));
    rbgpu_diag_set_num("mapq_fails", static_cast<uint64_t>(g_rb_2cq_mapq_fails));
    RB_STEP(40 + (ring & 0x7));   // 2fz: the two diag sets returned
    rbgpu_gate_2en(33, "mapQueues: the two diag sets returned");
    rbgpu_log("2cq: submitMapQueuesPacket(ring=%u, a=0x%x, b=0x%llx, spec=%p, c=0x%llx) -> %d %s",
              ring, a, b, spec, c, ret ? 1 : 0, ret ? "(OK)" : "(FAIL)");
    // 2ff: the gate is the LAST statement before the return, so the span between
    // this gate and the next ENTER gate holds nothing of ours - the move that
    // landed gate 151 (2ez) and gate 54 (2eu). The 2fe trail stopped right after
    // this gate with three mapQueues calls still to come, and the only thing left
    // in that span was this log line.
    // The fail band was 65+ring, which COLLIDES with the 60+ring enter band for
    // ring >= 5 - and ring 5 is real, measured in the 2fe trail. 80+ring instead,
    // ring masked to 3 bits, so enter 60..67 / OK 70..77 / fail 80..87 are
    // disjoint and disjoint from the 100+/150+ stamp bands.
    rbgpu_gate_2en(ret ? (70 + (ring & 0x7)) : (80 + (ring & 0x7)),
                   ret ? "submitMapQueuesPacket OK" : "submitMapQueuesPacket FAILED");
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
// 2fr (2026-10-14): the peer reads this before it sends request 0x1B. It is
// set from powerUpHW's own return, so "the controller is enabled" is only
// claimed when the hardware actually powered up.
volatile int g_rb_poweruphw_ok = 0;
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
    // 2ft (2026-10-14): stage 1 moved HERE from doStart ENTER, so the ~500 us
    // chain region contains ZERO flash commits while the record still exists.
    //
    // The 2fs boot settled the mechanism - `rbgpu-prog` read back `s=1`, so
    // rbgpu_nvram_note works and 2fr's missing variables mean neither of its
    // call sites was reached. But the chain did NOT finish: its log runs 454
    // ms past s=1, through initGraphicsMQD -> 1 and into mapQueues ring 0,
    // with 2-6 us between every line and no gate trail in the boot at all,
    // and `s=2` (doStart's return) never landed.
    //
    // So 2fo's "the gate trail was the wedge" does not reproduce. That was
    // one boot. This one has ONE synchronous commit instead of twenty-four
    // and still stops in the same region.
    //
    // powerUpHW ENTER runs ~80 ms before doStart and has executed in every
    // boot since 2br, so stage 1 keeps proving the channel from outside the
    // region under test. If the chain now completes, two boots agree that
    // commits inside it matter; if it stops again, nothing of ours is in
    // that region and the stop is real.
    rbgpu_prog_2fs(1, "powerUpHW ENTER - before the chain");
    rbgpu_progwatch_start();          // 2fw: +30 s / +90 s stage samples
    uint32_t ret = orig_accel_powerUpHW ? orig_accel_powerUpHW(accel) : 0;
    g_rb_pw_ret = ret;
    g_rb_pw_stage |= 2;
    rbgpu_log("2br: powerUpHW returned 0x%x", ret);
    if (ret == 0) {
        rbgpu_log("2cn: powerUpHW failed, will fail start() cleanly (survive mode)");
        g_rb_2cn_poweruphw_failed = 1;
    } else {
        g_rb_poweruphw_ok = 1;
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

// 2fp (2026-10-14): walk the loop startHWEngines runs, naming the engine that
// refuses. Disassembled at 0xbea8fea:
//
//   count = hw->0xc2 * 2 + 2
//   for (i = 0; i < count; i++) {
//       e = hw->0x3b8[i];            NOT the array 2bv walks - see 2fu
//       if (!e) -> return false
//       if (!e->vtable[0x148]()) -> os_log, return false
//   }
//   return true
//
// So it is the 2bv shape one function later, on the same array, through
// vtable[0x148] instead of powerUp - and Apple names the failing index in an
// os_log we do not capture. This reads the array and the slot pointer only;
// the original still makes every call. No hardware access.
// 2fq (2026-10-14): the walk's answer rides ONE NVRAM write, not the log.
//
// The 2fp boot ran (ttl_hw_init_ret moved) and produced no evidence at all:
// `log show --last boot` came back empty and plugin_check never got as far
// as its first bounded ioreg, so the capture is also consistent with that
// boot wedging. rbgpu-diag is no fallback - the 2fo and 2fp summaries are
// byte-identical and both stop at `regchan_calls=19`, hundreds of log lines
// before powerUp. So the one question this step asks needs a channel that
// survives a boot with no log and no desktop, and NVRAM is the only one this
// project has that does.
//
// The map is one character per engine, written as a single variable:
//
//   .  the entry is non-NULL and its vtable[0x148] slot is non-NULL
//   f  the entry is non-NULL and vtable[0x148] reads NULL - Apple would call
//      through a null slot, which is a fault, not a FALSE return
//   -  the entry is NULL: startHWEngines fails at 0xbea9068 right there, and
//      the position in the map names the engine outright
static char g_rb_2fq_map[96];

// 2gb (2026-10-15): read the SDMA engines' started-flags, not their code.
//
// Apple's start() (0xbea4a6a) sets `this->0x10 = 1` only on its success path
// at 0xbea4b49, and `startHWEngines` breaks out of its walk on the FIRST
// engine that returns FALSE. So after the original returns, the flags name the
// failing index exactly: the engines before it read 1, it and everything after
// it read 0. That is Apple's own bookkeeping, not an inference about it.
//
// This replaces 2fu's route of start() itself, which is what panicked (see the
// routing block at the bottom of this file). Plain loads from objects whose
// pointers we already walk - nothing is written, nothing is routed, and no
// trampoline exists to be wrong.
// 2gc (2026-10-15): the chip has ONE SDMA instance and Apple asks for four.
//
// 2gb measured `pre=0000 post=1000 fail@1`: engine[0]'s start() returned TRUE
// and engine[1]'s returned FALSE, so Apple's walk broke at the second SDMA
// engine. Three independent things say the discriminator is the INSTANCE
// INDEX, and none of them needed a boot:
//
//  1. This machine's own IP discovery table (probe 1, 2026-10-03) lists
//     exactly `SDMA0(42) inst 0` - while PCIE, SMUIO and UMC each enumerate
//     inst 0 AND inst 1, so the parser does report multiples when they exist.
//     ONE SDMA instance.
//  2. The count is a per-ASIC capability constant, and the second data point
//     agrees with real silicon:
//       Navi21Hardware::setupAndInitializeHWCapabilities  0xbeca1d9
//           movw $0x101, 0xc1(%rbx)      -> +0xc2 = 1 -> count 4   (Navi 21: 4 SDMA)
//       Navi23Hardware::setupAndInitializeHWCapabilities  0xbed26ad
//           movw $0x100, 0xc2(%rbx)      -> +0xc2 = 0 -> count 2   (Navi 23: 2 SDMA)
//     And `Navi21Hardware::allocateHWEngines` (0xbeca1f0) builds two SDMA
//     engines UNCONDITIONALLY and two more only behind `cmpb $0x1,0xc2`
//     (0xbeca269) - the allocation side computing the same 0xc2*2+2 from the
//     other direction.
//  3. The failing argument IS the instance. From the call site, read rather
//     than guessed: the pairs at engine+0x1a0 are an array of {u32,u32} with
//     stride 8 indexed by queue, and pair[0] measured (7, 0) (7, 1) (7, 2)
//     (7, 3) across engines 0..3 - a constant command 7 and the engine index.
//
// Worth stating plainly: `0xc2*2+2` is always even and at least 2, so Apple's
// Navi 21 path CANNOT walk exactly one SDMA engine. Setting +0xc2 = 0 would
// halve the problem and still fail at engine[1]. There is no count that fits
// this chip.
//
// So this tests the instance theory with ONE variable: for every engine after
// the first, rewrite arg2 to 0 so each queue create asks for instance 0. It
// is a field write to Apple's own objects, not a code patch - the same class
// as 2di's `ctx+0x340` zero - and it is OFF unless `-rbgpusdmainst0`.
//
// What it is NOT: a fix. Four engine objects would each hold queues on SDMA
// instance 0, which is only safe because an armed boot has no GPU clients and
// nothing submits SDMA work. If it works, the real fix is to make Apple build
// one SDMA engine, and that is a separate question.
//
//   post=1111 allstarted   the instance index was the whole discriminator,
//                          established on hardware, and startHWEngines
//                          returns TRUE for the first time
//   post=1000 fail@1 again the instance is NOT it, and the refusal is about
//                          the engine object or its channels - a different
//                          question, and the three supports above would owe
//                          an explanation
//   a partial flip         the per-engine flags say exactly how far it got
static uint32_t g_rb_2gc_patched = 0;
static int      g_rb_2gc_on = -1;

// 2ge (2026-10-15): make engine[1] the FIRST to ask, by giving engine[0] no
// queues.
//
// 2gc's rewrite landed - six arg2 fields set to 0, every one read back - and
// `post=1000 fail@1` did not move. So **the instance index is not the
// discriminator**, and 2gc's three supports owe an explanation.
//
// What the same boot established instead, and it is stronger than the
// question 2gc asked: with the rewrite on, engine[1]'s two queue creates sent
// `(0x7, 0)` and `(0xa, 0)` - **byte-identical to engine[0]'s**, which
// succeeded - and engine[1] still returned FALSE. Identical arguments, one
// succeeds, the next fails. The refusal therefore cannot be about the
// arguments at all; it is about the engine object, its channels, or state the
// layer below holds once a queue exists.
//
// The duplicate is also a confound 2gc created and did not name as one: it
// warned that four engines would each hold queues on instance 0 as a *safety*
// matter, when it also makes a refusal of the second request expected for a
// reason that has nothing to do with instances.
//
// So test the ordinal directly. `start()` takes a trivial-TRUE path when
// `this->0x48 == 0` (0xbea4a94 -> 0xbea4b49): it sets the started flag, calls
// vtable[0x278] and returns TRUE **without asking the layer for anything**.
// Writing 0 into engine[0]'s queue count therefore makes engine[1] the first
// engine in the boot to create a queue, with its own untouched `(0x7, 1)` and
// `(0xa, 1)`.
//
//   post=1100 or further   engine[1] succeeds once it asks FIRST, so the
//                          failure is ordinal - the layer allows one SDMA
//                          engine's queues and refuses the next, whatever
//                          instance it names. Instance 1 is fine.
//   post=1000 fail@1 again engine[1] fails even asking first with instance 1,
//                          so the instance really is absent and the 2gc
//                          supports stand after all - the duplicate was
//                          masking a genuine refusal
//   post=0000 fail@0       the skip itself broke engine[0], which should be
//                          impossible on Apple's own trivial-TRUE path and
//                          would say the field read is wrong
//
// Off unless `-rbgpusdma0skip`. A field write to Apple's own object, like
// 2gc's and 2di's; engine[0] contributing no SDMA queues is tolerable only
// because an armed boot has no GPU clients and nothing submits SDMA work.
static int g_rb_2gh_on = -1;
static int g_rb_2ge_on = -1;
static uint32_t g_rb_2ge_saved_q = 0xffffffffu;

static void rbgpu_sdma0_skip_2ge(uint32_t idx, uintptr_t ep) {
    if (g_rb_2ge_on < 0) {
        g_rb_2ge_on = checkKernelArgument("-rbgpusdma0skip") ? 1 : 0;
    }
    if (!g_rb_2ge_on || idx != 0) { return; }
    volatile uint32_t *q = reinterpret_cast<volatile uint32_t *>(ep + 0x48);
    g_rb_2ge_saved_q = *q;
    *q = 0;
    rbgpu_log("2ge: engine[0] queues(+0x48) %u -> %u (read back %u) - it will "
              "take Apple's trivial-TRUE path and ask the layer for NOTHING, "
              "so engine[1] is the first to create a queue",
              g_rb_2ge_saved_q, 0u, *q);
    rbgpu_diag_set_num("sdma0_skip_was", g_rb_2ge_saved_q);
}

static void rbgpu_sdma_inst0_2gc(uint32_t idx, uintptr_t ep, uint32_t queues) {
    if (g_rb_2gc_on < 0) {
        g_rb_2gc_on = checkKernelArgument("-rbgpusdmainst0") ? 1 : 0;
        if (!g_rb_2gc_on) {
            rbgpu_log("2gc: SDMA instance rewrite OFF (-rbgpusdmainst0 not "
                      "set); engine[1..3] will ask for instances 1..3, which "
                      "this chip's discovery table says do not exist");
        }
    }
    if (!g_rb_2gc_on || idx == 0 || queues == 0 || queues > 8) { return; }
    for (uint32_t qi = 0; qi < queues; qi++) {
        volatile uint32_t *a2 =
            reinterpret_cast<volatile uint32_t *>(ep + 0x1a4 + qi * 8);
        uint32_t was = *a2;
        if (was == 0) { continue; }
        *a2 = 0;
        uint32_t back = *a2;
        g_rb_2gc_patched++;
        rbgpu_log("2gc: engine[%u] queue[%u] arg2 0x%x -> 0x%x (read back "
                  "0x%x) - asking for SDMA instance 0", idx, qi, was, 0u, back);
    }
}

// 2gi (2026-10-16): let exactly ONE SDMA engine ask the layer for queues.
//
// The 2gh boot closed the 2x2 and the answer is `post=1100 fail@2`: engine[1]
// STARTED. Five cells are now measured, and the bit order was checked against
// `rbgpu_poststarthw_2gb` before any of this was read - `post` is written
// left-to-right in index order, one digit per engine's own +0x10 flag:
//
//   engine   instance   asks     result
//   [0]      0          first    TRUE   (every boot)
//   [1]      0          second   FALSE  (2gd)
//   [1]      1          first    FALSE  (2gf)
//   [1]      0          first    TRUE   (2gh)   <- the cell that was missing
//   [2]      0          second   FALSE  (2gh)
//
// Cell 4 against cell 3 (same engine, same order, instance differs) makes the
// instance index matter. Cell 4 against cell 2 (same engine, same instance,
// order differs) makes the ordinal matter. Both are necessary.
//
// What is NOT claimed: that these are two separate conditions. This chip has
// ONE SDMA instance, so "the second engine to ask" and "a duplicate create on
// instance 0" are the SAME EVENT in every cell measured, and nothing here
// separates them. They are not separable by any experiment that leaves the
// engine count at four, and they imply the same fix, so the distinction is
// recorded as open rather than resolved - cf. the 2gc confound, which was
// named in advance and still cost a boot to clear.
//
// The fix follows from either reading: exactly one engine may ask. Mechanism
// is Apple's own trivial-TRUE path at 0xbea4b49 - `start()` with
// `this->0x48 == 0` sets the started flag, calls vtable[0x278] and returns
// TRUE without asking the layer for anything. That path is now proven on
// hardware twice: 2ge used it on engine[0], and the 2gh boot's `post=1100`
// has engine[0] passing through it with `skip0=2`.
//
// So engines [1..3] get a zero queue count and engine[0] is left completely
// alone - it natively asks for instance 0 (the pairs are `(0x7, idx)` and
// `(0xa, idx)`, measured in 2gd) and in the first ordinal position, which is
// the one combination that has returned TRUE on every boot. 2gi therefore
// needs NEITHER `-rbgpusdmainst0` NOR `-rbgpusdma0skip`: both must come out,
// so the boot carries one variable.
//
// The counts are NOT restored afterwards. Those three engine objects really
// do have no queues on this chip - the instance behind them does not exist -
// so zero is the honest state and restoring it would be a latch reporting
// hardware that is not there, which is the debt 2am/2an/2ao already owe. It
// is tolerable for the same reason 2ge's was: an armed boot has no GPU
// clients and nothing submits SDMA work. If anything later submits to
// engines [1..3], it finds no queues rather than a queue that silently is
// not backed.
//
// Off unless `-rbgpusdmaone`.
static int g_rb_2gi_on = -1;
static uint32_t g_rb_2gi_zeroed = 0;

static void rbgpu_sdma_one_2gi(uint32_t idx, uintptr_t ep) {
    if (g_rb_2gi_on < 0) {
        g_rb_2gi_on = checkKernelArgument("-rbgpusdmaone") ? 1 : 0;
        if (!g_rb_2gi_on) {
            rbgpu_log("2gi: one-SDMA-engine mode OFF (-rbgpusdmaone not set); "
                      "all four engines will ask the layer and engine[1] is "
                      "expected to be refused for instance 1");
        }
    }
    // engine[0] is left untouched on purpose: instance 0, asking first, is
    // the only combination measured TRUE on every boot.
    if (!g_rb_2gi_on || idx == 0) { return; }
    volatile uint32_t *q = reinterpret_cast<volatile uint32_t *>(ep + 0x48);
    uint32_t was = *q;
    if (was == 0) {
        rbgpu_log("2gi: engine[%u] already has ZERO queues - nothing written, "
                  "and it takes the trivial-TRUE path on its own", idx);
        return;
    }
    *q = 0;
    uint32_t back = *q;
    if (back != 0) {
        // Absence is not a result (rule 2): say the write did not take rather
        // than letting a later `post=` digit be read as the layer's answer.
        rbgpu_log("2gi: engine[%u] queues(+0x48) %u -> 0 FAILED, reads back "
                  "%u - this engine will still ask the layer, so its "
                  "started-flag says nothing about the one-engine theory",
                  idx, was, back);
        return;
    }
    g_rb_2gi_zeroed++;
    rbgpu_log("2gi: engine[%u] queues(+0x48) %u -> 0 (read back %u) - it will "
              "take Apple's trivial-TRUE path (0xbea4b49) and ask the layer "
              "for NOTHING, so engine[0] is the only engine that asks",
              idx, was, back);
    char key[24];
    snprintf(key, sizeof(key), "sdma%u_zeroed_was", idx);
    rbgpu_diag_set_num(key, was);
}

static uint8_t g_rb_2gb_pre[32];
static uint8_t g_rb_2gb_post[32];
static uint32_t g_rb_2gb_count = 0;
static char g_rb_2gb_flags[96];

static void rbgpu_walk_starthw_2fp(void *hw) {
    int n = 0;
    if (!hw) {
        snprintf(g_rb_2fq_map, sizeof(g_rb_2fq_map), "hw=NULL");
        rbgpu_prog_2fs(3, g_rb_2fq_map);
        return;
    }
    uint8_t c2 = *reinterpret_cast<volatile uint8_t *>(
        reinterpret_cast<uintptr_t>(hw) + 0xc2);
    uint32_t count = static_cast<uint32_t>(c2) * 2u + 2u;
    rbgpu_log("2fp: startHWEngines will walk %u entries (hw+0xc2 = %u)",
              count, c2);
    rbgpu_diag_set_num("starthw_c2", c2);
    rbgpu_diag_set_num("starthw_count", count);
    if (count > 32) {
        // Not walked rather than walked blindly: past the real array the
        // entries are whatever follows it in the object, and dereferencing
        // one of those as a vtable pointer is a fault, not a measurement.
        n = snprintf(g_rb_2fq_map, sizeof(g_rb_2fq_map),
                     "c2=%u n=%u IMPLAUSIBLE - not walked", c2, count);
        rbgpu_log("2fp: count %u is implausible - not walking", count);
        rbgpu_prog_2fs(3, g_rb_2fq_map);
        return;
    }

    n = snprintf(g_rb_2fq_map, sizeof(g_rb_2fq_map), "c2=%u n=%u map=", c2, count);
    uint32_t first_null = 0xffffffffu;
    uint32_t first_badfn = 0xffffffffu;
    for (uint32_t i = 0; i < count; i++) {
        void *e = *reinterpret_cast<void **>(
            reinterpret_cast<uintptr_t>(hw) + 0x3b8 + i * 8);
        char c;
        if (!e) {
            c = '-';
            if (first_null == 0xffffffffu) { first_null = i; }
            rbgpu_log("2fp: engine[%u] = NULL -> startHWEngines returns FALSE "
                      "here (0xbea9068), and this is the whole failure", i);
        } else {
            void **vt = *reinterpret_cast<void ***>(e);
            void *fn = vt ? vt[0x148 / sizeof(void *)] : nullptr;
            c = fn ? '.' : 'f';
            if (!fn && first_badfn == 0xffffffffu) { first_badfn = i; }
            rbgpu_log("2fp: engine[%u] = %p vtable[0x148] = %p", i, e, fn);
            // 2gb: Apple's own gate fields, read here instead of from inside a
            // hook on start() - because routing start() is what panicked (see
            // the 2gb block below). These are the exact fields the
            // disassembly at 0xbea4a6a tests, read before Apple's call, so
            // they are what its gates will see.
            uintptr_t ep = reinterpret_cast<uintptr_t>(e);
            g_rb_2gb_pre[i] = *reinterpret_cast<volatile uint8_t *>(ep + 0x10);
            uint32_t q    = *reinterpret_cast<volatile uint32_t *>(ep + 0x48);
            void *chans   = *reinterpret_cast<void **>(ep + 0x50);
            // 2gc: the pairs are an ARRAY, stride 8, indexed by the QUEUE
            // index - proven from the call site rather than assumed:
            //   bea4aee  movl 0x1a0(%rbx,%r12,8), %esi    arg1
            //   bea4af6  movl 0x1a4(%rbx,%r12,8), %edx    arg2
            // so 2gb's single read happened to be pair[0] and is now read as
            // such, with every queue's pair logged instead of just the first.
            uint32_t cmd  = *reinterpret_cast<volatile uint32_t *>(ep + 0x1a0);
            uint32_t cmd2 = *reinterpret_cast<volatile uint32_t *>(ep + 0x1a4);
            // 2ge: labelled "pair[0]" rather than "cmd", because reading all
            // the pairs showed arg1 is NOT a constant 7: queue[0] sends 0x7
            // and queue[1] sends 0xa on every engine. 2gc's "a constant
            // command 7 and the engine index" came from reading pair[0]
            // alone and is corrected here.
            rbgpu_log("2gb: engine[%u] started(+0x10)=%u queues(+0x48)=%u "
                      "chans(+0x50)=%p pair[0]=(0x%x,0x%x)", i,
                      g_rb_2gb_pre[i], q, chans, cmd, cmd2);
            if (q <= 8) {
                for (uint32_t qi = 0; qi < q; qi++) {
                    uint32_t a1 = *reinterpret_cast<volatile uint32_t *>(
                        ep + 0x1a0 + qi * 8);
                    uint32_t a2 = *reinterpret_cast<volatile uint32_t *>(
                        ep + 0x1a4 + qi * 8);
                    // 2gh: the DESCRIPTOR read, corrected and gated.
                    //
                    // 2gg had this wrong and it PANICKED (page fault, CR2 =
                    // 0x49, inside this function). Apple's instruction is
                    //   bea4aa7  movq 0x50(%rbx,%r12,8), %r13
                    // so the channel pointers are an INLINE ARRAY inside the
                    // engine - `ch = *(void **)(engine + 0x50 + qi*8)` - not
                    // a pointer at +0x50 that is then indexed. 2gg read
                    // `(*(void **)(ep+0x50))[qi]`, which dereferences INTO
                    // channel[0]'s own fields and used whatever it found
                    // there as a pointer; +0x48 of the 1 it read is the 0x49
                    // the fault reports. So the `chans` the walk logs is
                    // channel[0]'s pointer, not an array base, and that
                    // label was the whole mistake.
                    //
                    // Two guards, because a NULL check was not enough: the
                    // pointer must be a kernel VA (this chip's are all
                    // 0xffffff........), and the whole read is behind
                    // -rbgpusdmadesc so it cannot cost the boot-args
                    // experiment a second boot. Reads only.
                    if (g_rb_2gh_on < 0) {
                        g_rb_2gh_on =
                            checkKernelArgument("-rbgpusdmadesc") ? 1 : 0;
                    }
                    if (g_rb_2gh_on) {
                        void *ch = *reinterpret_cast<void **>(
                            ep + 0x50 + qi * 8);
                        uintptr_t cp = reinterpret_cast<uintptr_t>(ch);
                        if (cp >= 0xffffff8000000000ull) {
                            uint64_t c48 = *reinterpret_cast<
                                volatile uint64_t *>(cp + 0x48);
                            uint64_t cd0 = *reinterpret_cast<
                                volatile uint64_t *>(cp + 0xd0);
                            rbgpu_log("2gh: engine[%u] queue[%u] pair = "
                                      "(0x%x, 0x%x) ch=%p desc={0x40000, "
                                      "ch+0x48=0x%llx, ch+0xd0=0x%llx}",
                                      i, qi, a1, a2, ch,
                                      static_cast<unsigned long long>(c48),
                                      static_cast<unsigned long long>(cd0));
                        } else {
                            rbgpu_log("2gh: engine[%u] queue[%u] pair = "
                                      "(0x%x, 0x%x) ch=%p is NOT a kernel "
                                      "pointer - NOT dereferenced, and the "
                                      "descriptor fields say NOTHING",
                                      i, qi, a1, a2, ch);
                        }
                    } else {
                        rbgpu_log("2gc: engine[%u] queue[%u] pair = "
                                  "(0x%x, 0x%x)", i, qi, a1, a2);
                    }
                }
            }
            rbgpu_sdma_inst0_2gc(i, ep, q);
            rbgpu_sdma0_skip_2ge(i, ep);
            rbgpu_sdma_one_2gi(i, ep);
            if (i < 4) {
                char key[24];
                snprintf(key, sizeof(key), "sdma%u_q", i);
                rbgpu_diag_set_num(key, q);
                snprintf(key, sizeof(key), "sdma%u_cmd", i);
                rbgpu_diag_set_num(key, cmd);
            }
            if (q == 0) {
                rbgpu_log("2gb: engine[%u] has ZERO queues, so Apple's start() "
                          "takes its trivial-TRUE path (0xbea4b49) and sets "
                          "+0x10 - it cannot be the engine that refuses", i);
            }
        }
        if (n > 0 && static_cast<size_t>(n) + 2 < sizeof(g_rb_2fq_map)) {
            g_rb_2fq_map[n++] = c;
            g_rb_2fq_map[n] = '\0';
        }
    }
    // 2fp's own decision table, recorded where it is read rather than left to
    // be remembered: a '-' means the NULL check at 0xbea9068 is the failure
    // and the index names the engine; an all-'.' map means the FALSE comes
    // from one of the vtable[0x148] CALLS, which is a different question and
    // needs the slot disassembled rather than the array read.
    if (first_null != 0xffffffffu) {
        rbgpu_diag_set_num("starthw_null_idx", first_null);
        snprintf(g_rb_2fq_map + n, sizeof(g_rb_2fq_map) - n,
                 " NULL@%u", first_null);
    } else if (first_badfn != 0xffffffffu) {
        rbgpu_diag_set_num("starthw_badfn_idx", first_badfn);
        snprintf(g_rb_2fq_map + n, sizeof(g_rb_2fq_map) - n,
                 " NOFN@%u", first_badfn);
        rbgpu_log("2fp: engine[%u] has a NULL vtable[0x148] - Apple calls "
                  "through it unconditionally", first_badfn);
    } else {
        snprintf(g_rb_2fq_map + n, sizeof(g_rb_2fq_map) - n, " allok");
        rbgpu_log("2fp: all %u entries are non-NULL with a non-NULL "
                  "vtable[0x148], so a FALSE return comes from one of the "
                  "CALLS and not from the array", count);
    }
    g_rb_2gb_count = count;
    rbgpu_prog_2fs(3, g_rb_2fq_map);
}

// 2gb: the post pass. Called after Apple's startHWEngines returns, reading the
// same +0x10 flag the success path sets. Builds "pre=0000 post=1100 fail@2".
static void rbgpu_poststarthw_2gb(void *hw, bool walk_ret) {
    g_rb_2gb_flags[0] = '\0';
    if (!hw || g_rb_2gb_count == 0 || g_rb_2gb_count > 32) { return; }
    uint32_t fail = 0xffffffffu;
    for (uint32_t i = 0; i < g_rb_2gb_count; i++) {
        void *e = *reinterpret_cast<void **>(
            reinterpret_cast<uintptr_t>(hw) + 0x3b8 + i * 8);
        g_rb_2gb_post[i] = e ? *reinterpret_cast<volatile uint8_t *>(
            reinterpret_cast<uintptr_t>(e) + 0x10) : 0xff;
        if (g_rb_2gb_post[i] == 0 && fail == 0xffffffffu) { fail = i; }
    }
    // Cursor arithmetic kept in size_t and always re-clamped against the
    // buffer, rather than relying on 4+32+6+32+11 fitting in 96: a snprintf
    // return that exceeds the buffer would make `sizeof - n` underflow and
    // the next call write past the end.
    const size_t cap = sizeof(g_rb_2gb_flags);
    size_t n = 0;
    auto put = [&](const char *lit) {
        if (n >= cap - 1) { return; }
        int r = snprintf(g_rb_2gb_flags + n, cap - n, "%s", lit);
        if (r > 0) { n += static_cast<size_t>(r); }
        if (n > cap - 1) { n = cap - 1; }
    };
    put("pre=");
    for (uint32_t i = 0; i < g_rb_2gb_count && n + 1 < cap; i++) {
        g_rb_2gb_flags[n++] = static_cast<char>('0' + (g_rb_2gb_pre[i] & 0xf));
        g_rb_2gb_flags[n] = '\0';
    }
    put(" post=");
    for (uint32_t i = 0; i < g_rb_2gb_count && n + 1 < cap; i++) {
        g_rb_2gb_flags[n++] = static_cast<char>('0' + (g_rb_2gb_post[i] & 0xf));
        g_rb_2gb_flags[n] = '\0';
    }
    if (fail != 0xffffffffu) {
        if (n + 1 < cap) {
            snprintf(g_rb_2gb_flags + n, cap - n, " fail@%u", fail);
        }
        rbgpu_diag_set_num("sdma_fail_idx", fail);
        rbgpu_log("2gb: %s - engine[%u] is the FIRST whose started-flag is "
                  "still 0, so it is the one Apple's walk refused at",
                  g_rb_2gb_flags, fail);
    } else {
        put(" allstarted");
        // 2gi: this branch is now reachable with startHWEngines returning
        // TRUE, which it never was when the message was written. Saying
        // "startHWEngines' FALSE comes from outside the loop" on a boot that
        // SUCCEEDED is a record that contradicts itself - the 2ga trap - so
        // the two cases say what they are.
        if (walk_ret) {
            rbgpu_log("2gb: %s - EVERY engine's started-flag is set AND "
                      "startHWEngines returned TRUE: the walk passed",
                      g_rb_2gb_flags);
        } else {
            rbgpu_log("2gb: %s - EVERY engine's started-flag is set, so every "
                      "start() returned TRUE and startHWEngines' FALSE comes "
                      "from outside the loop (0xbea906b onward)",
                      g_rb_2gb_flags);
        }
    }
}

// 2fu (2026-10-14): AMDGFX10SDMAEngine::start is what startHWEngines calls.
//
// Resolved statically, no boot spent, and it corrects two things I wrote.
//
// `powerUpHWEngines` reads hw+0x3B0, 11 entries, vtable[0x138] (verified at
// 0xbea8ec9-0xbea8ef5, so 2bv's manual loop is faithful). `startHWEngines`
// reads hw+0x3B8 - EIGHT BYTES LATER - with count = hw->0xc2 * 2 + 2. So the
// 2fp comment "the SAME engine array 2bv walks" was wrong by one pointer, and
// the two windows are:
//
//   0x3B0[0]      AMDRadeonX6000_AMDNavi21PM4Engine
//   0x3B0[1..4]   AMDRadeonX6000_AMDSDMAEngine_GC_10_3_0   = 0x3B8[0..3]
//   0x3B0[8..9]   AMDRadeonX6000_AMDVCN3HWEngine
//
// The 2ft boot measured hw->0xc2 = 1, so count = 4 - exactly the number of
// SDMA engines present, and 0x3B8 is exactly where they start. Two
// independently-derived numbers agreeing, so `startHWEngines` walks the four
// SDMA engines and the PM4 engine is NOT in its window.
//
// That resolves the 2fo anomaly rather than leaving it: the PM4's
// vtable[0x148] IS a `doStart(false)` thunk (0xbed2342), so had the PM4 been
// in range, doStart would have run a second time - and the 2fo log has
// exactly ONE "doStart entered" with startHWEngines returning 0 ten ms later.
// It is not in range, so there is no second call to explain.
//
// Apple's own start(), disassembled (0xbea4a6a):
//
//   if (this->0x10 != 0)            return FALSE      one byte, no work
//   svc = (this->0x18)->vtable[0x2c0]()
//   if (this->0x48 == 0)            -> set 0x10 = 1, return TRUE
//   for (i = 0; i < this->0x48; i++) {
//       ch   = this->0x50[i]
//       desc = { 0x40000, ch->0x48, ch->0xd0 }
//       if (svc->vtable[0x120](this->0x1a0[i], this->0x1a4[i], &desc, &out))
//                                   -> unmap, return FALSE
//   }
//   this->0x10 = 1 ; vtable[0x278] ; return TRUE
//
// `0x10` is a "started" flag - set on success, and a second start() refuses at
// the first instruction. The tempting explanation was that powerUp had already
// set it, making the FALSE a double-call artefact. **Refuted statically:** the
// SDMA vtable[0x138] is 0xbed1fac = `movb $0x1,%al ; retq`, a constant TRUE
// that touches nothing, which is also why 2bv logged engines 1-4 powering up
// 3 us apart. So 0x10 is 0 on entry and the body really runs.
//
// What is left is `svc->vtable[0x120](cmd, ...)` returning nonzero - the SAME
// service slot `startKIQ` uses with command 8, which works. This is the first
// time SDMA has asked that layer for a queue on this chip.
//
// Observe only: the original runs and its return is reported. The fields are
// read before the call, so they are what Apple's own gates will test.
static bool (*orig_sdma_start)(void *) = nullptr;
static char g_rb_2fu_rec[160];
static uint32_t g_rb_2fu_calls = 0;

static bool hooked_sdma_start_2fu(void *eng) {
    uint8_t  f10 = 0;
    uint32_t f48 = 0;
    uint32_t cmd = 0, cmd2 = 0;
    if (eng) {
        uintptr_t e = reinterpret_cast<uintptr_t>(eng);
        f10 = *reinterpret_cast<volatile uint8_t *>(e + 0x10);
        f48 = *reinterpret_cast<volatile uint32_t *>(e + 0x48);
        cmd  = *reinterpret_cast<volatile uint32_t *>(e + 0x1a0);
        cmd2 = *reinterpret_cast<volatile uint32_t *>(e + 0x1a4);
    }
    rbgpu_log("2fu: SDMAEngine::start #%u eng=%p f10=%u queues(+0x48)=%u "
              "cmd[0]=0x%x/0x%x", g_rb_2fu_calls, eng, f10, f48, cmd, cmd2);

    bool ret = orig_sdma_start ? orig_sdma_start(eng) : false;

    rbgpu_log("2fu: SDMAEngine::start #%u -> %d %s", g_rb_2fu_calls, ret ? 1 : 0,
              ret ? "" : (f10 ? "(refused at the 0x10 started-flag)"
                             : "(a queue create returned nonzero)"));
    // One compact record for all four calls, carried out of a dead boot by the
    // single stage-4 note at startHWEngines' exit.
    int n = static_cast<int>(strlen(g_rb_2fu_rec));
    if (n >= 0 && n + 24 < static_cast<int>(sizeof(g_rb_2fu_rec))) {
        snprintf(g_rb_2fu_rec + n, sizeof(g_rb_2fu_rec) - n,
                 "%s#%u:f10=%u,q=%u,r=%d", n ? " " : "", g_rb_2fu_calls,
                 f10, f48, ret ? 1 : 0);
    }
    g_rb_2fu_calls++;
    return ret;
}

static bool hooked_hw_startHWEngines(void *hw) {
    g_rb_pw_stage |= 0x100;
    rbgpu_log("2bs: AMDHardware::startHWEngines entered (hw=%p)", hw);
    rbgpu_walk_starthw_2fp(hw);
    bool ret = orig_hw_startHWEngines ? orig_hw_startHWEngines(hw) : false;
    if (ret) g_rb_pw_stage |= 0x200;
    rbgpu_log("2bs: AMDHardware::startHWEngines -> %d", ret ? 1 : 0);
    rbgpu_poststarthw_2gb(hw, ret);
    rbgpu_diag_set_num("sdma_inst0_patched", g_rb_2gc_patched);
    // 2fq: the second and last write. Two per boot, against the trail's
    // twenty-four - and this one proves the first was not a lucky read of a
    // stale variable, because it carries the return from the same call.
    {
        char buf[300];
        snprintf(buf, sizeof(buf), "%s ret=%d %s inst0=%u sdma[%u]{%s}",
                 g_rb_2fq_map, ret ? 1 : 0, g_rb_2gb_flags, g_rb_2gc_patched,
                 g_rb_2fu_calls, g_rb_2fu_rec);
        if (g_rb_2ge_saved_q != 0xffffffffu) {
            size_t n2 = strlen(buf);
            snprintf(buf + n2, sizeof(buf) - n2, " skip0=%u", g_rb_2ge_saved_q);
        }
        // 2gi: `one=N` says how many of engines [1..3] really were zeroed, so
        // a `post=1111` is only readable as the one-engine answer when N is 3.
        // Without it, a switch that did not run and a layer that accepted all
        // four would look identical in the note - rule 2.
        if (g_rb_2gi_on > 0) {
            size_t n2 = strlen(buf);
            snprintf(buf + n2, sizeof(buf) - n2, " one=%u", g_rb_2gi_zeroed);
        }
        rbgpu_prog_2fs(4, buf);
        // 2gf: the SAME string to its own variable. `rbgpu-prog` is a single
        // overwrite slot tracking progress, so the stage-5 note (the 0x1B
        // decision, which always follows) destroys the engine map the stage-4
        // note carried - and the 2ge boot was lost to exactly that: `b=2`
        // proved the note was this boot's and said only "powerUpHW failed",
        // which both of 2ge's branches produce. One variable carrying two
        // purposes, which is the 2fq mistake in a new place.
        rbgpu_nvram_note("rbgpu-sdma", buf);
    }
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
    g_rb_ttl_this = this_ptr;   // 2eh: needed to ask for the GART table address

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
    // 2jc: -rbgpuautoloadtruth stops the 2am lie.
    //
    // 2am patched this function to `return 1` on the reasoning that the PSP
    // places the firmware so the latch is never set during the wait and the
    // timeout is spurious. 2ix showed the reasoning is wrong for this part:
    // on a PSP+autoload chip the autoload is PRECISELY what sets the latch,
    // Linux's gfx_v10_0_rlc_resume fails bring-up without it, and 2jb has
    // now watched the consequence from the other end - Apple calls the
    // correct un-halt and the RLC refuses it, forever.
    //
    // So the patch is a true negative we silenced. With this arg Apple is
    // told the truth and we find out what Apple does about it: a different
    // error path, a backdoor fallback, or the same bring-up with one more
    // 50 ms wait. It costs one boot and no chip state of ours.
    if (checkKernelArgument("-rbgpuautoloadtruth")) {
        rbgpu_log("2jc: GC autoload check NOT patched (-rbgpuautoloadtruth) - "
                  "Apple will see BOOTLOAD_COMPLETE=0 as it really is");
        rbgpu_diag_set_num("gc_autoload_patched", 0);
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

// 2ie: defined below, beside the rest of the context-write instrumentation.
static void rbgpu_route_gvmwrite_2ie(KernelPatcher &patcher, size_t id,
                                     mach_vm_address_t address, size_t size);
// 2iy: likewise.
static void rbgpu_route_microengine_2iy(KernelPatcher &patcher, size_t id,
                                        mach_vm_address_t address, size_t size);
// 2jc (2026-10-19): is the 10_3 autoload path wired at all?
//
// 2jb settled that the host cannot un-halt the CP: Apple calls the correct
// _gc_unhalt_micro_engines_10_3 once at 236 ms, through its own ext2 write
// path, and CP_ME_CNTL does not move for 160 s. The only lever left is to
// make the autoload actually complete, so the question is which parts of
// the 10_3 autoload machinery Apple runs.
//
// The _10_1 hooks have existed for milestones and have never logged a call;
// but ours is the _10_3 branch and those were never hooked. Three symbols,
// all DEFINED in HWLibs (nm prints `t`), all passing rule 9 - each opens
// `55 48 89 e5` with the next instruction a push or a mov:
//
//   _gc_parse_rlc_toc_10_3                           0x126b4678
//   _gc_fw_backdoor_loading_disable_10_3             0x126b4732
//   _gc_fw_initialize_backdoor_loading_register_set_10_3  0x126b4746
//
// Read statically first, because it changes what the counts will mean:
//
//   _disable_10_3 is NOT a stub. It stores 1 to dev+0x930 and dev+0x948 -
//   two flags that say "backdoor loading is off". If Apple calls it, Apple
//   has deliberately chosen the PSP path and the backdoor is closed by
//   decision, not by omission.
//
//   _initialize_backdoor_loading_register_set_10_3 writes RLC_CNTL (0x4c00,
//   seg1) with 1 through dev+0xc0. That is real backdoor register setup and
//   it is the function that would have to run for a host-side load.
//
// So the counts separate two very different worlds: Apple deciding against
// the backdoor, versus Apple never considering it.
//
// 2ja's lesson is applied from the start: these hooks touch NO hardware.
// Three counter increments, nothing else, no register read anywhere near
// Apple's init thread.
static uint32_t g_rb_2jc_toc = 0, g_rb_2jc_dis = 0, g_rb_2jc_reg = 0;
static int      g_rb_2jc_hooked = 0;

static void (*orig_toc_2jc)(void *) = nullptr;
static void (*orig_dis_2jc)(void *) = nullptr;
static void (*orig_reg_2jc)(void *) = nullptr;

static void hooked_toc_2jc(void *d) {
    g_rb_2jc_toc++;
    if (orig_toc_2jc) orig_toc_2jc(d);
}
static void hooked_dis_2jc(void *d) {
    g_rb_2jc_dis++;
    if (orig_dis_2jc) orig_dis_2jc(d);
}
static void hooked_reg_2jc(void *d) {
    g_rb_2jc_reg++;
    if (orig_reg_2jc) orig_reg_2jc(d);
}

void rbgpu_autoload_flush_2jc(void) {
    char note[192];
    snprintf(note, sizeof(note),
             "hk=%d toc=%u dis=%u reg=%u",
             g_rb_2jc_hooked, g_rb_2jc_toc, g_rb_2jc_dis, g_rb_2jc_reg);
    rbgpu_nvram_note("rbgpu-ald", note);
    rbgpu_log("2jc: %s", note);
    rbgpu_log("2jc: toc=0 means Apple never even parses the RLC TOC for our "
              "version. dis>0 with reg=0 means Apple deliberately closed the "
              "backdoor and committed to the PSP. reg>0 means the backdoor "
              "register set runs and a host-side load is already half wired.");
}

static void rbgpu_route_autoload_2jc(KernelPatcher &patcher, size_t id,
                                     mach_vm_address_t address, size_t size);

void RGPU::processHWLibs(KernelPatcher &patcher, size_t id, mach_vm_address_t address, size_t size) {
    rbgpu_log("Patching com.apple.kext.AMDRadeonX6800HWLibs");
    rbgpu_route_gvmwrite_2ie(patcher, id, address, size);
    rbgpu_route_microengine_2iy(patcher, id, address, size);
    rbgpu_route_autoload_2jc(patcher, id, address, size);

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

    // 2eh: the GART primary page table's own address, straight from TTL.
    // A local text symbol defined in HWLibs (nm shows it as 't'), so
    // resolving it by HWLibs' id is safe under the milestone-2c Lilu rule.
    // Resolved only - never routed; we call it read-only.
    g_rb_ttl_gart_info = reinterpret_cast<uint32_t (*)(void *, uint64_t *)>(
        patcher.solveSymbol(id, "_ttl_get_gart_primary_page_table_info"));
    rbgpu_diag_set_num("gart_info_resolved", g_rb_ttl_gart_info ? 1 : 0);
    rbgpu_log("HWLibs: _ttl_get_gart_primary_page_table_info %s",
              g_rb_ttl_gart_info ? "resolved (2eh)" : "NOT RESOLVED (2eh cannot run)");

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

// 2go: its own function, so the five RouteRequests do not sit on
// processX6000's frame (-Wframe-larger-than is 1024 bytes there).
static __attribute__((noinline)) void rbgpu_route_2go(KernelPatcher &patcher, size_t id,
                            mach_vm_address_t address, size_t size) {
    // 2go: split hardwareDidStart's chain. Every one of these is a named
    // symbol read off the disassembly, external and defined in X6000's
    // __TEXT, and rule-9 clean (first relative branch at +0x15 / +0x12 /
    // +0x45 / +0x12 / +0x1c, all far outside the 5-byte patch window).
    KernelPatcher::RouteRequest req_2go[] = {
        KernelPatcher::RouteRequest(
            "__ZN28AMDRadeonX6000_AMDRTHardware16hardwareDidStartEv",
            hooked_rt_hardwareDidStart_2go, orig_rt_hardwareDidStart_2go),
        KernelPatcher::RouteRequest(
            "__ZN23AMDRadeonX6000_AMDHWVMM8postInitEv",
            hooked_vmm_postInit_2go, orig_vmm_postInit_2go),
        KernelPatcher::RouteRequest(
            "__ZN39AMDRadeonX6000_AMDGFX10UserQueueManager21setSchedulerResourcesEtjjj",
            hooked_uqm_setSchedulerResources_2go, orig_uqm_setSchedulerResources_2go),
        KernelPatcher::RouteRequest(
            "__ZN35AMDRadeonX6000_AMDGFX10HIQHWChannel24submitSetResourcesPacketEtj",
            hooked_hiq_submitSetResourcesPacket_2go,
            orig_hiq_submitSetResourcesPacket_2go),
        KernelPatcher::RouteRequest(
            "__ZN35AMDRadeonX6000_AMDGFX10HIQHWChannel11submitFrameEb",
            hooked_hiq_submitFrame_2go, orig_hiq_submitFrame_2go),
        // 2gp: the event machine's stamp wait, where the 2go trail ends.
        KernelPatcher::RouteRequest(
            "__ZN35AMDRadeonX6000_AMDAccelEventMachine15finishAllStampsEv",
            hooked_evm_finishAllStamps_2gp, orig_evm_finishAllStamps_2gp),
        KernelPatcher::RouteRequest(
            "__ZN35AMDRadeonX6000_AMDAccelEventMachine12waitForStampEijPj",
            hooked_evm_waitForStamp_2gp, orig_evm_waitForStamp_2gp),
    };
    if (patcher.routeMultiple(id, req_2go, 7, address, size)) {
        rbgpu_log("X6000: hooked RTHardware::hardwareDidStart + AMDHWVMM::postInit "
                  "+ setSchedulerResources + HIQ submitSetResourcesPacket + "
                  "HIQ submitFrame + finishAllStamps + waitForStamp (2go/2gp)");
    } else {
        rbgpu_log("X6000: FAILED to hook the 2go hardwareDidStart chain");
    }
}

// ---------------------------------------------------------------------------
// 2hs (2026-10-18): Apple's OWN per-fault record, which it has been printing
// into a void all along.
//
// 2hr left the faulting VA as an interpretation debt. Reading
// `AMDGFX10VMM::getVMFaultInfo` (0xbe9b1e2) settles it statically - Apple
// reconstructs the address exactly as Linux does:
//
//     r13 = HI32 & 0xf ; shlq $0x2c   -> << 44
//     r12 = LO32       ; shlq $0xc    -> << 12
//     orq %r13, %r12                  -> the faulting VA
//
// and then fills a caller-provided struct, each field cross-checked against
// the shifts in gc_10_3_0_sh_mask.h:
//
//     VM_FAULT_INFO +0x00  u64  faulting VA
//                   +0x08  u32  (status >> 20) & 0xf     VMID
//                   +0x0c  u8   (status >> 18) & 1       RW
//                   +0x10  u32  (status >> 9)  & 0x1ff   CID
//                   +0x14  u32  raw GCVM_L2_PROTECTION_FAULT_STATUS
//
// It fills the struct ONLY when (addr != 0 || status != 0), so an untouched
// struct means "Apple saw no fault", which is a different answer from "the
// hook did not run" - this code keeps them apart by pre-poisoning the struct
// and saying which happened.
//
// Why hook it at all, when the probe already samples the same three
// registers: the samples are snapshots at +12/+100/+160 s, and this is
// per-fault and per-call. It answers whether Apple NOTICES the faults, how
// often, and at which addresses - none of which a snapshot can say.
//
// And `AMDHWVMM::fillFaultAccelInfo` (0xbe91c6a) is the one that assembles
// the full report, including "Closest good VA", and hands it to **kprintf**
// (resolved through build/re/stubs.json). kprintf does not reach the unified
// log or the IO registry, which is exactly why no armed boot has ever shown
// a VM fault message despite the faults being real. Here it is only
// COUNTED - its data is in locals this hook cannot reach - but a nonzero
// count proves Apple's fault reporting path runs, and a zero count with
// faults latched proves it does not.
//
// Both targets pass rule 9 (2gb): first 16 bytes of each are frame setup and
// pushes, no rel8/rel32 branch anywhere near the first 5. Both are shown
// **defined** in (__TEXT,__text) of AMDRadeonX6000 by `nm -m`, so routing by
// this kext's id is safe (milestone 2c). Observe-only: both call the
// original and change nothing.
// 2ki: see the route site for the full derivation.
static uint64_t rbgpu_2iq_ms(void);   // defined with the 2iq restart hooks
static void (*orig_prepareVMInvalidateRequest_2ki)(void *, void *, const void *,
                                                   bool) = nullptr;
static int      g_rb_2ki_hooked = 0;
static int      g_rb_2ki_on     = -1;
static uint32_t g_rb_2ki_calls  = 0;
static uint32_t g_rb_2ki_fixed  = 0;
static uint32_t g_rb_2ki_already = 0;   // bit 0 already set: left alone
static uint32_t g_rb_2ki_zero   = 0;    // zero PD address: left alone
static uint32_t g_rb_2ki_above  = 0;    // already >= vram_start: left alone
static uint64_t g_rb_2ki_first_was = 0;
static uint64_t g_rb_2ki_first_now = 0;
static uint32_t g_rb_2ki_first_vmid = 0xFFFFFFFFu;
static uint32_t g_rb_2ki_first_ms  = 0;
// 2kj (2026-10-20): per-VMID, because boot 82's note could not answer the one
// question that matters. It read `calls=353 fixed=3 zero=334 above=16` with
// `first vmid=2` - so the site runs, the rewrite works and nothing breaks, but
// whether **VMID 3** ever comes through with a base was unrecorded. VMID 3 is
// the one the CP faults on (`ib1 = 0x4_0030_0000`, `vmf = 3009bb`).
//
// `zero=334` is explained and is not a bug: the PD-programming block in
// prepareVMInvalidateRequest sits behind `cmpb $0x1, 0x24(%rdx); jne`, so
// Apple writes a base only when `info->0x24 == 1`. The other calls are
// invalidate-only and carry no base, and programAndInvalidateVM zeroes the
// request struct before the call - hence a zero read. The guard was right to
// skip them.
#define RB_2KJ_VMIDS 16u
struct rb_2kj_s { uint32_t fixed, zero, above, valid; uint64_t was, now; };
static struct rb_2kj_s g_rb_2kj[RB_2KJ_VMIDS];
static uint32_t g_rb_2kj_oob = 0;   // vmid outside 0..15, if that ever happens

static void hooked_prepareVMInvalidateRequest_2ki(void *vmm, void *req,
                                                  const void *info, bool flag) {
    if (orig_prepareVMInvalidateRequest_2ki) {
        orig_prepareVMInvalidateRequest_2ki(vmm, req, info, flag);
    }
    if (g_rb_2ki_on < 0) {
        g_rb_2ki_on = checkKernelArgument("-rbgpupdefix") ? 1 : 0;
        rbgpu_log("2ki: PD-address correction %s",
                  g_rb_2ki_on ? "ARMED (-rbgpupdefix)" : "OFF (observe only)");
    }
    if (!req) { return; }
    g_rb_2ki_calls++;

    auto *r = reinterpret_cast<volatile uint32_t *>(req);
    // request+0x04 = PD address LO32, +0x0c = HI32 (from the disassembly).
    uint64_t was = (static_cast<uint64_t>(r[3]) << 32) | r[1];
    uint32_t vmid = info ?
        reinterpret_cast<const uint32_t *>(info)[1] : 0xFFFFFFFFu;

    struct rb_2kj_s *v = (vmid < RB_2KJ_VMIDS) ? &g_rb_2kj[vmid] : nullptr;
    if (!v) { g_rb_2kj_oob++; }
    if (was == 0)   { g_rb_2ki_zero++;    if (v) v->zero++;  return; }
    if (was & 1)    { g_rb_2ki_already++; if (v) v->valid++; return; }
    if (g_rb_2ed_fb_off == 0)     { return; }   // no offset known: do nothing
    uint64_t vstart = static_cast<uint64_t>(g_rb_2ed_fb_base) << 24;
    uint64_t vbo    = static_cast<uint64_t>(g_rb_2ed_fb_off)  << 24;
    if (vstart && was >= vstart) { g_rb_2ki_above++; if (v) v->above++; return; }

    uint64_t now = (was + vbo) | 1ull;
    if (g_rb_2ki_first_vmid == 0xFFFFFFFFu) {
        g_rb_2ki_first_was  = was;
        g_rb_2ki_first_now  = now;
        g_rb_2ki_first_vmid = vmid;
        g_rb_2ki_first_ms   = static_cast<uint32_t>(rbgpu_2iq_ms());
        rbgpu_log("2ki: vmid %u PD %llx -> %llx (+vram_base_offset %llx, "
                  "VALID) %s", vmid, was, now, vbo,
                  g_rb_2ki_on ? "WRITTEN" : "not written, observe only");
    }
    if (g_rb_2ki_on != 1) { return; }
    r[1] = static_cast<uint32_t>(now);
    r[3] = static_cast<uint32_t>(now >> 32);
    g_rb_2ki_fixed++;
    if (v) { v->fixed++; if (v->was == 0) { v->was = was; v->now = now; } }
}

void rbgpu_pdefix_flush_2ki(void) {
    char n[608];
    if (!g_rb_2ki_hooked) {
        rbgpu_nvram_note("rbgpu-pde",
                         "hk=0 the prepareVMInvalidateRequest hook did NOT "
                         "take - silence is the hook, not the driver");
        return;
    }
    snprintf(n, sizeof(n),
             "hk=1 on=%d calls=%u fixed=%u | left: valid=%u zero=%u above=%u | "
             "first vmid=%u @%ums %llx -> %llx",
             g_rb_2ki_on, g_rb_2ki_calls, g_rb_2ki_fixed,
             g_rb_2ki_already, g_rb_2ki_zero, g_rb_2ki_above,
             g_rb_2ki_first_vmid, g_rb_2ki_first_ms,
             g_rb_2ki_first_was, g_rb_2ki_first_now);
    size_t at = strlen(n);
    for (unsigned i = 0; i < RB_2KJ_VMIDS && at < sizeof(n) - 40; i++) {
        const struct rb_2kj_s *v = &g_rb_2kj[i];
        if (!v->fixed && !v->zero && !v->above && !v->valid) continue;
        int w2 = snprintf(n + at, sizeof(n) - at,
                          " | v%u f=%u z=%u a=%u V=%u", i, v->fixed, v->zero,
                          v->above, v->valid);
        if (w2 > 0) at += static_cast<size_t>(w2);
    }
    // VMID 3 is the one the CP faults on, so it gets its values spelled out.
    if (g_rb_2kj[3].was && at < sizeof(n) - 48) {
        snprintf(n + at, sizeof(n) - at, " | v3 %llx->%llx",
                 g_rb_2kj[3].was, g_rb_2kj[3].now);
    } else if (at < sizeof(n) - 24) {
        snprintf(n + at, sizeof(n) - at, " | v3 NEVER CARRIED A BASE");
    }
    rbgpu_nvram_note("rbgpu-pde", n);
    rbgpu_log("2ki: %s", n);
    rbgpu_log("2kj: `v3 NEVER CARRIED A BASE` means Apple does not program "
              "context 3's page-table base through this path at all, which "
              "matches 2hr failing to find any write path - and then the "
              "writer is somewhere else and this hook can never fix it. "
              "`v3 f>0` means we DID correct it and it was still too late or "
              "still wrong.");
    rbgpu_log("2ki: calls=0 means the request is never prepared and this is "
              "the wrong site. fixed=0 with calls>0 means every value already "
              "passed a guard - read `left:` to see which. The test is "
              "whether rbgpu-cph still reports vmf=3009bb at the halt and "
              "whether the halt happens at all.");
}

// 2ki: routed from its own function - folding it into processX6000 pushed
// that frame past the 1024-byte -Wframe-larger-than limit.
__attribute__((noinline))
static void rbgpu_route_pdefix_2ki(KernelPatcher &patcher, size_t id,
                                   mach_vm_address_t address, size_t size) {
    KernelPatcher::RouteRequest pvir(
        "__ZN26AMDRadeonX6000_AMDGFX10VMM26prepareVMInvalidateRequestEP25AMD_VM_INVALIDATE_REQUESTPK22AMD_VM_INVALIDATE_INFOb",
        hooked_prepareVMInvalidateRequest_2ki,
        orig_prepareVMInvalidateRequest_2ki);
    if (patcher.routeMultiple(id, &pvir, 1, address, size)) {
        rbgpu_log("2ki: hooked AMDGFX10VMM::prepareVMInvalidateRequest - the "
                  "page-directory address is corrected in the request, before "
                  "the CP's WRITE_DATA packet is built");
        g_rb_2ki_hooked = 1;
    } else {
        rbgpu_log("2ki: FAILED to hook prepareVMInvalidateRequest - hk=0 in "
                  "rbgpu-pde means the hook, not a request that never ran");
        g_rb_2ki_hooked = 0;
        patcher.clearError();
    }

}

static void (*orig_getVMFaultInfo_2hs)(void *, uint32_t, void *) = nullptr;
static void (*orig_fillFaultAccelInfo_2hs)(void *) = nullptr;
static uint32_t g_rb_2hs_calls = 0;
static uint32_t g_rb_2hs_faults = 0;
static uint32_t g_rb_2hs_fill = 0;
static int      g_rb_2hs_hooked = 0;
static char     g_rb_2hs_note[384];

// 2hu (2026-10-18): aggregate by fault SIGNATURE instead of logging the
// first four.
//
// 2ht asked one question it could not answer: does a `status 0x40953` fault
// EVER carry a nonzero VA? If it never does, the CPF write really is aimed
// at virtual address zero and the end-of-pipe-timestamp hypothesis gets its
// test; if some do, the zero was `CLEAR_PROTECTION_FAULT_STATUS_ADDR` wiping
// the address register before anyone looked (2hp), and the hypothesis dies.
//
// Raising the four-fault cap is the obvious answer and the wrong one: the
// faults recur all boot (2hp), so a commit per fault is the 2fo wedge. What
// the question actually needs is, per distinct (hub, status): how many, the
// first VA, and **the largest VA ever seen**. `mx=0` after hundreds of
// faults is the finding; one nonzero `mx` kills the hypothesis outright.
// That is bounded storage and bounded writes.
//
// The hub-1 all-ones phantoms (2ht) are counted like anything else rather
// than filtered out. Dropping them silently would make "Apple polls hub 1
// and gets garbage" invisible, and that is its own defect.
//
// The counters live IN this note. 2ht put them in `rbgpu-diag`, which is
// length-capped and truncated them away, leaving no way to tell "no faults"
// from "the hook never ran" - the ambiguity of absence again (2eh, 2ek, 2fb).
#define RB_2HU_SIGS 5
#define RB_2HU_MAX_COMMITS 16

struct rb_2hu_sig {
    uint32_t status;
    uint32_t hub;
    uint32_t count;
    uint64_t first_va;
    uint64_t max_va;     // the whole point: stays 0 only if every VA was 0
    uint32_t vmid;
    uint32_t cid;
    uint8_t  rw;
    uint8_t  used;
};
static struct rb_2hu_sig g_rb_2hu[RB_2HU_SIGS];
static uint32_t g_rb_2hu_other = 0;      // faults past the signature table
static uint32_t g_rb_2hu_commits = 0;

extern uint32_t g_rb_2im_f_at_write;   // 2im: defined with the 2il probe

static void rbgpu_fault_note_2hu(void) {
    size_t n = 0;
    int w = snprintf(g_rb_2hs_note, sizeof(g_rb_2hs_note),
                     "hk=%d c=%u f=%u r=%u o=%u fw=%d",
                     g_rb_2hs_hooked, g_rb_2hs_calls, g_rb_2hs_faults,
                     g_rb_2hs_fill, g_rb_2hu_other,
                     (g_rb_2im_f_at_write == 0xFFFFFFFFu)
                         ? -1
                         : static_cast<int>(g_rb_2hs_faults - g_rb_2im_f_at_write));
    n = (w > 0) ? static_cast<size_t>(w) : 0;
    for (unsigned i = 0; i < RB_2HU_SIGS; i++) {
        if (!g_rb_2hu[i].used || n >= sizeof(g_rb_2hs_note) - 72) {
            continue;
        }
        w = snprintf(g_rb_2hs_note + n, sizeof(g_rb_2hs_note) - n,
                     " | s%x h%u n=%u va=%llx mx=%llx v%u%s c%u",
                     g_rb_2hu[i].status, g_rb_2hu[i].hub, g_rb_2hu[i].count,
                     g_rb_2hu[i].first_va, g_rb_2hu[i].max_va,
                     g_rb_2hu[i].vmid, g_rb_2hu[i].rw ? "W" : "R",
                     g_rb_2hu[i].cid);
        if (w > 0) {
            n += static_cast<size_t>(w);
            if (n >= sizeof(g_rb_2hs_note)) {
                n = sizeof(g_rb_2hs_note) - 1;
            }
        }
    }
    rbgpu_nvram_note("rbgpu-fault", g_rb_2hs_note);
    g_rb_2hu_commits++;
}

// Called from the ring probe at +12/+100/+160 s, so the note is committed at
// known times even if no signature changed - "the hook ran and saw nothing"
// must be writable, not just inferable.
void rbgpu_fault_flush_2hu(void) {
    if (!g_rb_2hs_hooked) {
        rbgpu_nvram_note("rbgpu-fault", "hk=0 the VM fault hooks did NOT take "
                                        "this boot - silence here is the hook, "
                                        "not the chip");
        return;
    }
    rbgpu_fault_note_2hu();
}

// ---------------------------------------------------------------------------
// 2ic (2026-10-18): who writes GCVM_CONTEXT*_PAGE_TABLE_BASE_ADDR.
//
// 2ia asked this statically and found nothing: no displacement into the VMM
// object touches the descriptor region, so no struct-field search can find
// the write. 2ib then ruled out the other candidate - RB0 emitted no
// WRITE_DATA to 0x28C7..0x28E6 at all, and since wptr (0x280 dwords) lies
// inside the single 1024-dword page that was dumped, and nothing has
// wrapped, that is the whole written extent of the ring this boot.
//
// What both searches miss is the same thing: Apple's register writes go
// through an accessor that takes an ENCODED ID, so the dword offset is a
// runtime value and the write looks like nothing in particular.
//
// The accessor is now NAMED. AMDRadeonX6000_AMDHWRegisters's vtable
// (__ZTV29AMDRadeonX6000_AMDHWRegisters at 0xbfab6a0) decodes, under the 2gp
// convention that a CALL offset is the DUMP offset minus 0x10, to exactly
// the slots five earlier milestones inferred from disassembly:
//
//     call [0x118] = dump +0x128 = AMDHWRegisters::read(uint)         (2fd)
//     call [0x120] = dump +0x130 = AMDHWRegisters::write(uint, uint)  (2fg)
//     call [0x128] = dump +0x138 = readOrModifyWrite(uint, uint)
//     call [0x130] = dump +0x140 = readAndModifyWrite(uint, uint)
//     call [0x148] = dump +0x158 = socRegOffset(SOCRegister)          (2fd)
//
// `write` is the single choke point for GC MMIO: its body ends
// `movl %ebx,(%rax,%rcx)` with rax = this+0x38 (the aperture) and
// rcx = reg*4, and BOTH read-modify-write entries tail-jump through the
// virtual [0x120] slot (`movq 0x120(%rax),%rax` ... `jmpq *%rax`) rather
// than storing themselves. So one hook sees every write, RMW included.
//
// Milestone-2c rule: `nm -m build/re/AMDRadeonX6000` shows
// __ZN29AMDRadeonX6000_AMDHWRegisters5writeEjj DEFINED at 0xbe8d28a, not
// imported. Rule 9 (2gb): its first bytes are `55 48 89 e5 41 57 41 56 53`
// - pushes and a mov; the first branch is a `jne` at +0x11, well outside
// the five-byte patch window. Checked, not assumed.
//
// The filter. socRegOffset returns a SEGMENT-INCLUDED dword offset (2fe
// measured 0x3041 for CP_RB0_CNTL, whose raw offset is 0x1DE1: GC seg0
// 0x1260 + 0x1DE1). The context page-directory base registers are
// seg0 0x1260 + 0x1667 + 2*vmid, so VMID n's LO32 is 0x28C7 + 2n and its
// HI32 is 0x28C8 + 2n. The faulting context 3 is 0x28CD / 0x28CE.
//
// Cost. This is the hottest function in the driver, so the hook does one
// range compare and, inside the range, one array store. No logging and no
// NVRAM from the hook itself - 2ez's crawl was caused by logging in a hot
// path, not by a branch. The note is built and committed from the ring
// probe's thread_call, the 2hu pattern.
//
// What a reading would mean, written down before the boot (the recurring
// error, stated 25 times in docs/RULES.md):
//
//   a write to 0x28CD with bit 0 SET      the register lost the bit after
//                                          the write, or 2hr misread it
//   a write to 0x28CD with bit 0 CLEAR    the VALUE is wrong at its source,
//                                          and the bug is in whatever built
//                                          it - the first thing this project
//                                          could actually fix
//   writes to 0x28C7 but none to 0x28CD   context 0 is programmed this way
//                                          and contexts 1-15 are not, which
//                                          would mean c1/c3's bases were
//                                          never written at all and 2hr read
//                                          reset values
//   n=0 with calls>0                      the hook ran, the accessor ran,
//                                          and NOTHING writes these registers
//                                          through it - the write is on some
//                                          third path (firmware, PSP, or a
//                                          ring this project has not read)
//   calls=0                               the hook took but the accessor was
//                                          never called; the silence is the
//                                          instrument, not the chip
//
// Gated on -rbgpuringprobe, which is already in the boot-args and already
// drives the thread_call that flushes this note. No config.plist change.
// ---------------------------------------------------------------------------

// 2ir: CP_ME_CNTL, watched on BOTH write paths.
#define RB_2IR_ME_CNTL 0x21B6u        // GC seg0 0x1260 + dword 0x0F56
#define RB_2IR_MAX 6
static uint32_t g_rb_2ir_val[RB_2IR_MAX], g_rb_2ir_h_val[RB_2IR_MAX];
static uint64_t g_rb_2ir_ms[RB_2IR_MAX],  g_rb_2ir_h_ms[RB_2IR_MAX];
static uint32_t g_rb_2ir_n = 0, g_rb_2ir_h_n = 0;
static uint32_t g_rb_2ir_over = 0, g_rb_2ir_h_over = 0;
static uint32_t g_rb_2ir_last = 0xDEADu;
static uint64_t rbgpu_2iq_ms(void);   // 2iq, defined with the restart hooks

#define RB_2IC_FIRST 0x28C7u          // CONTEXT0_PAGE_TABLE_BASE_ADDR_LO32
#define RB_2IC_LAST  0x28E6u          // CONTEXT15_..._HI32
#define RB_2IC_N     (RB_2IC_LAST - RB_2IC_FIRST + 1u)

struct rb_2ic_slot {
    uint32_t count;
    uint32_t first;
    uint32_t last;
};
static struct rb_2ic_slot g_rb_2ic[RB_2IC_N];
static uint64_t g_rb_2ic_calls  = 0;    // every write the accessor performed
static uint32_t g_rb_2ic_hits   = 0;    // writes inside the BASE range
static int      g_rb_2ic_hooked = 0;
static int      g_rb_2ic_armed  = 0;

// 2id (2026-10-18): the positive control 2ic lacked.
//
// 2ic measured calls=34657 hits=0 and that is a real negative only if the
// hook would have CAUGHT a write to those registers had one happened. 34657
// calls proves the hook ran; it proves nothing about whether it covers the
// code that programs GCVM contexts, because the only range it watched is the
// one register nothing wrote. That is the recurring error in its purest
// form - a reading with nothing established to make it meaningful.
//
// The neighbours fix it. Seg-included dword offsets, GC seg0 = 0x1260 and
// raw offsets from gc_10_3_0_offset.h via the 2hq macros:
//
//     CNTL  = 0x1260 + 0x15FC + n    = 0x285C + n       (contexts 0..15)
//     BASE  = 0x1260 + 0x1667 + 2n   = 0x28C7 + 2n      (2ic's range)
//     START = 0x1260 + 0x1687 + 2n   = 0x28E7 + 2n
//     END   = 0x1260 + 0x16A7 + 2n   = 0x2907 + 2n
//
// 2hr read contexts 1 and 3 with cn=0x3b, s=400000, e=23fffff - CNTL,
// START and END all carry programmed-looking values, so SOMETHING writes
// them. If this boot sees those writes through the same hook and still sees
// no BASE write, the negative is airtight: the hook reaches the code that
// programs GCVM contexts and that code does not write the base.
//
// And that is exactly Linux's behaviour. gfxhub_v2_1_setup_vmid_config
// writes CONTEXT1_CNTL, PAGE_TABLE_START_ADDR and PAGE_TABLE_END_ADDR for
// contexts 0..14 and NEVER writes PAGE_TABLE_BASE_ADDR; the base for
// contexts 1..15 is written only by gmc_v10_0_emit_flush_gpu_tlb, per job,
// on the ring. So the expected shape of this reading is masks full for
// CNTL/START/END and empty for BASE - which would mean c1 and c3's bases
// were never written at all this boot and 2hr read whatever the registers
// held, not a value any code chose.
//
// A bitmask per group says which CONTEXTS were touched in 32 characters,
// which is what fits in an NVRAM note.
#define RB_2ID_CNTL_FIRST  0x285Cu
#define RB_2ID_CNTL_LAST   0x286Bu
#define RB_2ID_START_FIRST 0x28E7u
#define RB_2ID_START_LAST  0x2906u
#define RB_2ID_END_FIRST   0x2907u
#define RB_2ID_END_LAST    0x2926u

static uint16_t g_rb_2id_cm = 0;        // contexts whose CNTL was written
static uint16_t g_rb_2id_bm = 0;        // contexts whose BASE was written
static uint16_t g_rb_2id_sm = 0;        // contexts whose START was written
static uint16_t g_rb_2id_em = 0;        // contexts whose END was written
static uint32_t g_rb_2id_c_cntl = 0;
static uint32_t g_rb_2id_c_start = 0;
static uint32_t g_rb_2id_c_end = 0;
static uint32_t g_rb_2id_c_mid = 0;     // 0x286C..0x28C6, the unnamed gap

// The VM flush itself. programAndInvalidateVM (0xbe9b7a2) is Apple's
// emit_flush_gpu_tlb: it takes the accessor from (this->0x10)->vt[0x2d0],
// has vt[0x220] fill a nine-entry {reg, val} array on its stack, then loops
// `callq *0x120(%rax)` over the nonzero entries - AMDHWRegisters::write, the
// function already hooked. So if this ran and wrote no base, the base is not
// in its request array; if it never ran, hits=0 says nothing at all. One
// counter separates those, and it is the difference between a result and a
// coincidence.
static uint32_t g_rb_2id_pivm = 0;
static uint16_t g_rb_2id_pivm_hubs = 0;   // bit n set = hub n seen
static int      g_rb_2id_pivm_hooked = 0;

static void (*orig_hwreg_write_2ic)(void *, uint32_t, uint32_t) = nullptr;

static void hooked_hwreg_write_2ic(void *self, uint32_t reg, uint32_t val) {
    g_rb_2ic_calls++;
    // 2ir (2026-10-18): who halts the command processor.
    //
    // 2iq killed 2hn's guess. setGPURestart is NEVER called, and the first
    // resetHardwareAndReplay lands at 15015 ms after the X6000 hooks were
    // routed - strictly after the +12 s sample that already read
    // CP_ME_CNTL = 0x15000000. The two baselines differ (t0 is X6000 load,
    // the ring phases count from the peer's earlier start), which only makes
    // the conclusion stronger: the CP is halted BEFORE any reset. The
    // restart loop that has dominated this diagnosis since 2hj is a
    // consequence.
    //
    // In Linux those three bits are set by
    // gfx_v10_0_cp_gfx_enable(adev, false) during GC bring-up, and cleared
    // by cp_gfx_enable(adev, true) when the GFX ring is brought up. If Apple
    // performs the disable and never the enable, RB0 cannot execute - which
    // explains the frozen rptr of 2hl better than any fault did.
    //
    // CP_ME_CNTL is dword 0x0F56 raw, GC seg0 0x1260, so 0x21B6
    // seg-included. Every write to it is recorded with its value and the
    // milliseconds since the hooks were routed, so the ORDER of a disable
    // and any later enable is visible rather than inferred.
    if (reg == RB_2IR_ME_CNTL) {
        if (g_rb_2ir_n < RB_2IR_MAX) {
            g_rb_2ir_val[g_rb_2ir_n] = val;
            g_rb_2ir_ms[g_rb_2ir_n] = rbgpu_2iq_ms();
            g_rb_2ir_n++;
        } else {
            g_rb_2ir_over++;
        }
        g_rb_2ir_last = val;
    }
    if (reg >= RB_2ID_CNTL_FIRST && reg <= RB_2ID_END_LAST) {
        if (reg <= RB_2ID_CNTL_LAST) {
            g_rb_2id_cm = static_cast<uint16_t>(
                g_rb_2id_cm | (1u << (reg - RB_2ID_CNTL_FIRST)));
            g_rb_2id_c_cntl++;
        } else if (reg < RB_2IC_FIRST) {
            g_rb_2id_c_mid++;
        } else if (reg <= RB_2IC_LAST) {
            struct rb_2ic_slot *sl = &g_rb_2ic[reg - RB_2IC_FIRST];
            if (sl->count == 0) sl->first = val;
            sl->last = val;
            sl->count++;
            g_rb_2id_bm = static_cast<uint16_t>(
                g_rb_2id_bm | (1u << ((reg - RB_2IC_FIRST) >> 1)));
            g_rb_2ic_hits++;
        } else if (reg <= RB_2ID_START_LAST) {
            g_rb_2id_sm = static_cast<uint16_t>(
                g_rb_2id_sm | (1u << ((reg - RB_2ID_START_FIRST) >> 1)));
            g_rb_2id_c_start++;
        } else {
            g_rb_2id_em = static_cast<uint16_t>(
                g_rb_2id_em | (1u << ((reg - RB_2ID_END_FIRST) >> 1)));
            g_rb_2id_c_end++;
        }
    }
    if (orig_hwreg_write_2ic) {
        orig_hwreg_write_2ic(self, reg, val);
    }
}

// Committed from the ring probe's thread_call, never from the hook.
void rbgpu_ctxwrite_flush_2ic(void) {
    if (!g_rb_2ic_armed) {
        rbgpu_nvram_note("rbgpu-ctxw",
                         "off - -rbgpuringprobe was not set, so the accessor "
                         "was never hooked. Silence here is the boot-arg.");
        return;
    }
    if (!g_rb_2ic_hooked) {
        rbgpu_nvram_note("rbgpu-ctxw",
                         "hk=0 the AMDHWRegisters::write hook did NOT take "
                         "this boot - silence here is the hook, not the chip");
        return;
    }

    char note[352];
    size_t n = 0;
    int w = snprintf(note, sizeof(note),
                     "hk=1 calls=%llu hits=%u | cm=%04x sm=%04x em=%04x "
                     "bm=%04x nc=%u ns=%u ne=%u mid=%u | pivm=%u/%u h=%04x",
                     g_rb_2ic_calls, g_rb_2ic_hits,
                     g_rb_2id_cm, g_rb_2id_sm, g_rb_2id_em, g_rb_2id_bm,
                     g_rb_2id_c_cntl, g_rb_2id_c_start, g_rb_2id_c_end,
                     g_rb_2id_c_mid,
                     g_rb_2id_pivm, g_rb_2id_pivm_hooked, g_rb_2id_pivm_hubs);
    n = (w > 0) ? static_cast<size_t>(w) : 0;

    unsigned shown = 0, skipped = 0;
    for (unsigned i = 0; i < RB_2IC_N; i++) {
        if (g_rb_2ic[i].count == 0) continue;
        if (n >= sizeof(note) - 48) { skipped++; continue; }
        unsigned reg = RB_2IC_FIRST + i;
        w = snprintf(note + n, sizeof(note) - n, " | %x c%u f%x l%x",
                     reg, g_rb_2ic[i].count, g_rb_2ic[i].first,
                     g_rb_2ic[i].last);
        if (w > 0) {
            n += static_cast<size_t>(w);
            if (n >= sizeof(note)) { n = sizeof(note) - 1; break; }
        }
        shown++;
    }
    if (shown == 0 && n < sizeof(note) - 16) {
        snprintf(note + n, sizeof(note) - n, " | none");
    } else if (skipped && n < sizeof(note) - 12) {
        snprintf(note + n, sizeof(note) - n, " +%u", skipped);
    }

    rbgpu_nvram_note("rbgpu-ctxw", note);
    rbgpu_log("2ic: %s", note);
    rbgpu_log("2id: cm/sm/em are the POSITIVE CONTROL. A context bit set in "
              "cm, sm or em and clear in bm means this hook does reach the "
              "code that programs GCVM contexts and that code writes CNTL, "
              "START and END but never the page-directory BASE - which is "
              "what Linux's gfxhub_v2_1_setup_vmid_config does, and would "
              "mean c1/c3's bases were never written by anything. All four "
              "masks 0000 means the hook does NOT reach that code and 2ic's "
              "hits=0 is worth nothing. pivm=%u is the VM flush, the only "
              "thing that writes a base in Linux: pivm=0 with bm=0000 is a "
              "flush that never ran, not a flush that wrote the wrong value.",
              g_rb_2id_pivm);
    if (g_rb_2ic_hits == 0) {
        rbgpu_log("2ic: the accessor performed %llu writes this boot and NONE "
                  "of them was to 0x%x..0x%x. If calls is large this is a "
                  "real negative - the page-directory bases are not written "
                  "through AMDHWRegisters at all - and the write is on a path "
                  "this project has not read. If calls is 0 the hook took but "
                  "the accessor never ran, and this says nothing about the "
                  "chip.", g_rb_2ic_calls, RB_2IC_FIRST, RB_2IC_LAST);
    }
}

// ---------------------------------------------------------------------------
// 2ie (2026-10-18): the OTHER register-write path.
//
// 2id's masks are a real positive control - cm=fffe, contexts 1..15, bit 0
// clear, which is exactly Linux's setup_vmid_config loop (i = 0..14 over
// CONTEXT1_CNTL + i). So the hook reaches code that programs GCVM contexts,
// and sm=em=bm=0000 says that code wrote no START, END or BASE. But 2hr
// reads c1/c3 with s=400000 and e=23fffff, values nothing chose.
//
// AMDRadeonX6800HWLibs is the second path, and it is Apple's port of the
// very Linux file this is being compared against: it defines
// _vm_10_1_setup_vmid_config, _vm_10_1_initialize_page_table_registers,
// _vm_10_1_enable_virtual_domain and its own _vm_10_1_write_register, by
// those names.
//
//     _vm_10_1_write_register(dev, hubmask, reg_id, index, value)
//       -> _vm_10_1_get_register_offset(dev, 1, reg_id, index)   = offset
//       -> _gvm_write_register((dev->0), offset, value, 0xB)     0xB = GC_HWID
//       -> _gvm_cgs_write_register(ctx, offset, value, hwid)
//
// _gvm_write_register is a five-byte thunk (`55 48 89 e5 5d e9 ...`) whose
// `jmp rel32` begins at byte 5 - right at the edge of rule 9's window, so it
// is NOT the hook site. Its target is, and the target is where the resolved
// offset arrives: _gvm_cgs_write_register at 0x126779ed opens
// `55 48 89 e5 4c 8b 87 68 0c` - pushes, a movq, then a seven-byte movq,
// with no branch in the first five bytes. Rule 9 checked, not assumed, on
// both. Both are local symbols (`nm` prints `t`), defined in HWLibs, so the
// milestone-2c rule is satisfied - they are not imports.
//
// What that function's body shows matters more than the hook:
//
//     movq 0xc68(%rdi), %r8 ; movq 0x38(%r8), %rax ; jmpq *%rax
//
// HWLibs does not touch MMIO itself. It tail-jumps to a callback the HOST
// driver registered. So HWLibs' writes may already be arriving at X6000's
// AMDHWRegisters::write - the function 2ic/2id hooked - and 2id's cm=fffe
// may BE setup_vmid_config seen from the other end.
//
// That is the ambiguity this probe removes, and it is removed by comparing
// the two hooks' masks rather than by reasoning about the callback:
//
//   gvm cm=fffe as well           one path, two views. 2id already covered
//                                  HWLibs, and NOTHING programs START, END
//                                  or BASE for contexts 1..15 anywhere.
//   gvm cm=0000, gvm calls large  two separate paths; HWLibs writes plenty
//                                  but not the context block, and the
//                                  START/END values come from neither
//   gvm bm nonzero                HWLibs DOES write the base, on a path
//                                  X6000's accessor never sees, and the
//                                  value it writes is in this note
//   gvm calls=0                   HWLibs never wrote a register this boot;
//                                  the silence is HWLibs, not the chip
//
// One more thing is deliberately not assumed. X6000's accessor returns a
// SEGMENT-INCLUDED dword offset (2fe: 0x3041 for CP_RB0_CNTL, raw 0x1DE1,
// GC seg0 0x1260). HWLibs passes a hwid alongside the offset, which is
// exactly how a design that applies the segment DOWNSTREAM looks, so the
// offset here may be raw. Both framings are therefore counted, and the note
// says which one matched. Guessing it would be the recurring error on a
// register offset, which this project has paid for before.
// ---------------------------------------------------------------------------

#define RB_2IE_RAW_BASE 0x15FCu       // CONTEXT0_CNTL, raw
#define RB_2IE_SEG_BASE 0x285Cu       // the same, GC seg0 0x1260 added

struct rb_2ie_view {
    uint16_t cm, bm, sm, em;
    uint32_t n_cntl, n_base, n_start, n_end, n_mid;
    uint32_t base_first, base_last;
};
static struct rb_2ie_view g_rb_2ie_raw;   // offsets read as raw
static struct rb_2ie_view g_rb_2ie_seg;   // offsets read as seg-included
static uint64_t g_rb_2ie_calls = 0;
static uint64_t g_rb_2ie_gc    = 0;       // calls whose hwid was GC (0xB)
static int      g_rb_2ie_hooked = 0;

// Classify one offset under one framing. `b` is that framing's CONTEXT0_CNTL.
static void rbgpu_2ie_class(struct rb_2ie_view *v, uint32_t b, uint32_t off,
                            uint32_t val) {
    if (off < b || off > b + 0xCA) return;         // 0x15FC..0x16C6 span
    uint32_t d = off - b;
    if (d <= 0xF) {                                 // CNTL, one per context
        v->cm = static_cast<uint16_t>(v->cm | (1u << d));
        v->n_cntl++;
    } else if (d < 0x6B) {                          // the invalidation block
        v->n_mid++;
    } else if (d <= 0x8A) {                         // BASE, two per context
        if (v->n_base == 0) v->base_first = val;
        v->base_last = val;
        v->bm = static_cast<uint16_t>(v->bm | (1u << ((d - 0x6B) >> 1)));
        v->n_base++;
    } else if (d <= 0xAA) {                         // START
        v->sm = static_cast<uint16_t>(v->sm | (1u << ((d - 0x8B) >> 1)));
        v->n_start++;
    } else {                                        // END
        v->em = static_cast<uint16_t>(v->em | (1u << ((d - 0xAB) >> 1)));
        v->n_end++;
    }
}

static void (*orig_gvm_cgs_write_2ie)(void *, uint32_t, uint32_t, uint32_t) = nullptr;

static void hooked_gvm_cgs_write_2ie(void *ctx, uint32_t off, uint32_t val,
                                     uint32_t hwid) {
    g_rb_2ie_calls++;
    if (hwid == 0xB && off == RB_2IR_ME_CNTL) {
        // 2ir: the same register, the other path. 2ie proved these are two
        // separate write paths, so watching only one would leave the obvious
        // hole - HWLibs is where the Linux-derived GC bring-up lives.
        if (g_rb_2ir_h_n < RB_2IR_MAX) {
            g_rb_2ir_h_val[g_rb_2ir_h_n] = val;
            g_rb_2ir_h_ms[g_rb_2ir_h_n] = rbgpu_2iq_ms();
            g_rb_2ir_h_n++;
        } else {
            g_rb_2ir_h_over++;
        }
    }
    if (hwid == 0xB) {
        g_rb_2ie_gc++;
        rbgpu_2ie_class(&g_rb_2ie_raw, RB_2IE_RAW_BASE, off, val);
        rbgpu_2ie_class(&g_rb_2ie_seg, RB_2IE_SEG_BASE, off, val);
    }
    if (orig_gvm_cgs_write_2ie) {
        orig_gvm_cgs_write_2ie(ctx, off, val, hwid);
    }
}

void rbgpu_gvmwrite_flush_2ie(void) {
    if (!g_rb_2ie_hooked) {
        rbgpu_nvram_note("rbgpu-gvmw",
                         "hk=0 the HWLibs write hook did NOT take this boot - "
                         "silence here is the hook, not HWLibs");
        return;
    }
    char note[352];
    snprintf(note, sizeof(note),
             "hk=1 calls=%llu gc=%llu | raw cm=%04x bm=%04x sm=%04x em=%04x "
             "n=%u/%u/%u/%u m=%u bf=%x bl=%x | seg cm=%04x bm=%04x sm=%04x "
             "em=%04x n=%u/%u/%u/%u m=%u bf=%x bl=%x",
             g_rb_2ie_calls, g_rb_2ie_gc,
             g_rb_2ie_raw.cm, g_rb_2ie_raw.bm, g_rb_2ie_raw.sm, g_rb_2ie_raw.em,
             g_rb_2ie_raw.n_cntl, g_rb_2ie_raw.n_base, g_rb_2ie_raw.n_start,
             g_rb_2ie_raw.n_end, g_rb_2ie_raw.n_mid,
             g_rb_2ie_raw.base_first, g_rb_2ie_raw.base_last,
             g_rb_2ie_seg.cm, g_rb_2ie_seg.bm, g_rb_2ie_seg.sm, g_rb_2ie_seg.em,
             g_rb_2ie_seg.n_cntl, g_rb_2ie_seg.n_base, g_rb_2ie_seg.n_start,
             g_rb_2ie_seg.n_end, g_rb_2ie_seg.n_mid,
             g_rb_2ie_seg.base_first, g_rb_2ie_seg.base_last);
    rbgpu_nvram_note("rbgpu-gvmw", note);
    rbgpu_log("2ie: %s", note);
    rbgpu_log("2ie: compare cm here with 2id's cm=fffe. The SAME mask from "
              "both hooks means one path seen twice, so 2id already covered "
              "HWLibs and nothing anywhere programs the context base. A large "
              "calls with cm=0000 in both framings means two separate paths "
              "and the START/END values 2hr read come from neither. Exactly "
              "one framing should match at all; if both do the offsets are "
              "ambiguous and neither reading is safe.");
}

// ---------------------------------------------------------------------------
// 2iy (2026-10-18): is Apple's own un-halt ever CALLED?
//
// 2ix found the root cause - BOOTLOAD_COMPLETE never sets, so the RLC
// never releases the CP - and walking HWLibs' dispatch found the function
// that is supposed to do the releasing. _gc_init_fcn_ptr_list has two
// branches, and the 10_3 one (ours) assigns:
//
//   _gc_halt_micro_engines_10_3      0x126b3616
//   _gc_unhalt_micro_engines_10_3    0x126b36df
//   _gc_parse_rlc_toc_10_3, _gc_fw_backdoor_loading_disable_10_3,
//   _gc_fw_initialize_backdoor_loading_register_set_10_3,
//   _gc_enter/exit_rlc_safe_mode_10_3
//
// and, unlike the 10_1 branch, assigns NO _gc_fw_backdoor_autoload_config,
// NO _validate, NO _check_ucode_loaded and NO _load_ucode. The 10_3 path
// has no firmware-loading machinery at all: it delegates entirely to the
// PSP. Which is exactly consistent with an autoload handshake nobody
// drives.
//
// _gc_unhalt_micro_engines_10_3 is precisely right for this chip:
//
//   read  CP_ME_CNTL  (0xf56), AND 0xEAFFFFFF  -> clears bits 24/26/28,
//                                                 CE_HALT/PFP_HALT/ME_HALT
//   write CP_ME_CNTL
//   read  CP_MEC_CNTL (0xf55), AND 0xBFFFFFFF  -> clears bit 30, MEC_ME1_HALT
//   write CP_MEC_CNTL
//
// The mask is exactly ~0x15000000, the value we have been reading for
// twelve boots. Apple has the correct un-halt for our exact version.
//
// AND IT WRITES THROUGH A PATH I HAVE NEVER WATCHED.
// _gc_cgs_write_register_ext2(dev, off, val, hwid=0xB, flag=1) tail-jumps
// through a host callback at ctx->0x8 -> +0x120, where
// _gvm_cgs_write_register - the one 2ie hooked - uses +0xc68 -> +0x38. A
// third write path. So 2ie's "hwl [0@239]" was some other write entirely,
// and **there is no evidence _gc_unhalt_micro_engines_10_3 has ever run.**
//
// That trailing flag of 1 also matters: it may select an indirect or
// RLC-assisted write. If CP_ME_CNTL needs that mechanism, then 2iw's
// refusals were the wrong MECHANISM rather than an owned register, and
// 2iu's "RLCG eliminated" - which I drew from Linux having an empty RLCG
// range - would not apply to Apple's own implementation of it.
//
// 2ja (2026-10-19) - THE 2iy HOOKS STOPPED THE BOOT, AND THE INSTRUMENT
// WAS THE FAULT FOR THE THIRD TIME.
//
// The first armed boot with these hooks: PSP came all the way up
// (_psp_np_fw_load 0, _psp_hw_init 0), the hooks installed, and then
// rbgpu-boot never incremented - so rbgpu_prog_2fs(1) at powerUpHW ENTER
// never ran, and that call has fired in every boot since 2br. The
// accelerator never reached powerUpHW. Boot 55, the same build minus
// these hooks, got all the way to request 0x1B.
//
// The hooks read CP_ME_CNTL through g_hw.rreg from inside HWLibs' GC
// init, on Apple's own start thread. That is the 2fe failure exactly -
// GC register reads from this path stall non-deterministically, which is
// why -rbgpuregdump is permanently OUT - and docs/RULES.md's standing rule
// says to remove a new suspect rather than reason about it.
//
// So the hooks now touch NOTHING: two counters and one clock read. The
// before/after values are gone from the hook and the single CP_ME_CNTL
// read moved to the flush, which runs in the ring probe where GC reads
// are proven. This costs only the `->` transition across the call; the
// uh=0 / uh>0 decision - which is the whole question - is unaffected.

// So: hook both, observe only, before changing anything.
// Rule 9 (2gb) checked on both: unhalt opens
// `55 48 89 e5 41 57 41 56 41 54 53`, halt opens
// `55 48 89 e5 41 56 53 50`, pushes and a movq, no branch in the first
// five bytes. Both are DEFINED in HWLibs (`nm` prints `t`).
//
// Each hook reads CP_ME_CNTL before and after the original runs, so the
// question is answered in one line: called or not, and effective or not.
//
//   uh=0                 Apple NEVER calls its own un-halt. That is the
//                        missing step, and the fix is to call it.
//   uh>0, me unchanged    it runs and the write is refused even through
//                        Apple's own ext2 path - the RLC-ownership story
//                        stands and the handshake is the only way out
//   uh>0, me -> 0, later 15000000
//                        it works and something re-halts afterwards, and
//                        the halt hook says what
//   h>0                  something deliberately halts the CP, and when
// ---------------------------------------------------------------------------

static uint32_t g_rb_2iy_uh = 0, g_rb_2iy_h = 0;
static uint64_t g_rb_2iy_uh_ms = 0, g_rb_2iy_h_ms = 0;
static int      g_rb_2iy_hooked = 0;

static void (*orig_unhalt_me_2iy)(void *) = nullptr;
static void (*orig_halt_me_2iy)(void *) = nullptr;

// 2ja: the hooks TOUCH NOTHING. See the 2ja note above - the 2iy version
// read CP_ME_CNTL through g_hw.rreg from inside HWLibs' GC init and the
// accelerator never reached powerUpHW. A pure counter cannot do that: no
// MMIO, no lock, no allocation, nothing but two adds and a clock read.
//
// rbgpu_2iq_ms() is absolutetime_to_nanoseconds on mach_absolute_time(),
// which 2iq already runs from inside Apple's restart path. It is the one
// thing here that is not a plain store, and it is proven in that position.
static void hooked_unhalt_me_2iy(void *dev) {
    if (g_rb_2iy_uh == 0) {
        g_rb_2iy_uh_ms = rbgpu_2iq_ms();
    }
    g_rb_2iy_uh++;
    if (orig_unhalt_me_2iy) orig_unhalt_me_2iy(dev);
}

static void hooked_halt_me_2iy(void *dev) {
    if (g_rb_2iy_h == 0) {
        g_rb_2iy_h_ms = rbgpu_2iq_ms();
    }
    g_rb_2iy_h++;
    if (orig_halt_me_2iy) orig_halt_me_2iy(dev);
}

void rbgpu_microengine_flush_2iy(void) {
    // The ONE register read, here rather than in the hook. This runs from
    // the ring probe, which has read GC registers through g_hw in every
    // boot since 2hk without ever stopping one.
    uint32_t me = g_hw.rreg ? g_hw.rreg(g_hw.ctx, (0x1260u + 0x0F56u) * 4)
                            : 0xDEADu;
    char note[352];
    snprintf(note, sizeof(note),
             "hk=%d uh=%u @%llums | h=%u @%llums | me=%x",
             g_rb_2iy_hooked, g_rb_2iy_uh, g_rb_2iy_uh_ms,
             g_rb_2iy_h, g_rb_2iy_h_ms, me);
    rbgpu_nvram_note("rbgpu-ume", note);
    rbgpu_log("2iy: %s", note);
    rbgpu_log("2iy: uh=0 means Apple never calls its own un-halt and THAT is "
              "the missing step. uh>0 with me still 15000000 means even "
              "Apple's ext2 write path is refused and the RLC really does "
              "own the register. uh>0 with me=0 means it works. h counts "
              "and dates the deliberate halts.");
}

static void rbgpu_route_microengine_2iy(KernelPatcher &patcher, size_t id,
                                        mach_vm_address_t address, size_t size) {
    if (!checkKernelArgument("-rbgpuringprobe")) return;
    KernelPatcher::RouteRequest reqs[] = {
        KernelPatcher::RouteRequest("_gc_unhalt_micro_engines_10_3",
                                    hooked_unhalt_me_2iy, orig_unhalt_me_2iy),
        KernelPatcher::RouteRequest("_gc_halt_micro_engines_10_3",
                                    hooked_halt_me_2iy, orig_halt_me_2iy),
    };
    if (patcher.routeMultiple(id, reqs, 2, address, size)) {
        rbgpu_log("2iy: hooked _gc_(un)halt_micro_engines_10_3 (observe only)");
        g_rb_2iy_hooked = 1;
    } else {
        rbgpu_log("2iy: FAILED to hook the micro-engine halt pair - rbgpu-ume "
                  "will say hk=0, so uh=0 would be the hook and not Apple");
        g_rb_2iy_hooked = 0;
        patcher.clearError();
    }
}

static void rbgpu_route_autoload_2jc(KernelPatcher &patcher, size_t id,
                                     mach_vm_address_t address, size_t size) {
    // 2jd: behind its OWN arg, and OUT by default. Two consecutive armed
    // boots with these three routes (57, 58) wrote not one ring-probe note,
    // while boot 56 - the identical build minus them - wrote all three
    // phases. The mechanism is NOT identified: all three pass rule 9, and
    // _gc_fw_backdoor_loading_disable_10_3's 20 bytes have instruction
    // boundaries at 12 and 18, so Lilu's jump cannot spill into the next
    // function. Unexplained, reproducible, and therefore out.
    if (!checkKernelArgument("-rbgpualdprobe")) {
        rbgpu_log("2jc: autoload trio NOT hooked (-rbgpualdprobe not set) - "
                  "2jd took them out after two blank boots");
        return;
    }
    if (!checkKernelArgument("-rbgpuringprobe")) return;
    KernelPatcher::RouteRequest reqs[] = {
        KernelPatcher::RouteRequest("_gc_parse_rlc_toc_10_3",
                                    hooked_toc_2jc, orig_toc_2jc),
        KernelPatcher::RouteRequest("_gc_fw_backdoor_loading_disable_10_3",
                                    hooked_dis_2jc, orig_dis_2jc),
        KernelPatcher::RouteRequest(
            "_gc_fw_initialize_backdoor_loading_register_set_10_3",
            hooked_reg_2jc, orig_reg_2jc),
    };
    if (patcher.routeMultiple(id, reqs, 3, address, size)) {
        rbgpu_log("2jc: hooked the 10_3 autoload trio (observe only)");
        g_rb_2jc_hooked = 1;
    } else {
        rbgpu_log("2jc: FAILED to hook the 10_3 autoload trio - rbgpu-ald "
                  "will say hk=0, so three zeros would be the hooks and not "
                  "Apple");
        g_rb_2jc_hooked = 0;
        patcher.clearError();
    }
}

static void rbgpu_route_gvmwrite_2ie(KernelPatcher &patcher, size_t id,
                                     mach_vm_address_t address, size_t size) {
    if (!checkKernelArgument("-rbgpuringprobe")) {
        rbgpu_log("2ie: -rbgpuringprobe not set, _gvm_cgs_write_register left "
                  "alone");
        return;
    }
    KernelPatcher::RouteRequest req("_gvm_cgs_write_register",
                                    hooked_gvm_cgs_write_2ie,
                                    orig_gvm_cgs_write_2ie);
    if (patcher.routeMultiple(id, &req, 1, address, size)) {
        rbgpu_log("2ie: hooked _gvm_cgs_write_register (observe only) - every "
                  "HWLibs register write, with the offset already resolved");
        g_rb_2ie_hooked = 1;
    } else {
        rbgpu_log("2ie: FAILED to hook _gvm_cgs_write_register - rbgpu-gvmw "
                  "will say hk=0 so an empty note is not mistaken for HWLibs "
                  "writing nothing");
        g_rb_2ie_hooked = 0;
        patcher.clearError();
    }
}

// ---------------------------------------------------------------------------
// 2if (2026-10-18): the VMID handout, and the PD value Apple computes.
//
// 2ie closed the question 2hr opened. Two register-write paths exist and
// both are now instrumented, and between them:
//
//               CNTL        BASE      START     END
//   X6000       ctx 1-15    -         -         -
//   HWLibs      ctx 0-15    ctx 0     ctx 0     ctx 0
//
// Nothing writes BASE, START or END for contexts 1..15. So c1 and c3's
// page-directory bases are not values anyone computed and got wrong -
// nothing ever wrote them, and 2hr read leftovers. The missing VALID bit is
// an ABSENT WRITE, not a bad value. HWLibs' own context-0 write proves the
// convention holds where it does write: bf=0xFDFA001, bit 0 set (and
// missing the carve-out offset, which is 2em's bug seen from its source).
//
// In Linux the base for contexts 1..15 is programmed per job, by
// amdgpu_vm_flush -> ring->emit_vm_flush, at the moment a VMID is handed to
// a VM. Apple's programAndInvalidateVM invalidates without programming
// (2id: pivm=336, bm=0000), so the handout is the next place to look.
//
//   AMDHWVMM::assignVMID(IAMDHWVMContext*, uint &vmid, AMDAccelChannel*,
//                        IOAccelEvent**, bool*)      0xbe91376
//   AMDGFX10VMM::getPDEValue(eAMD_VMPT_LEVEL, uint64) 0xbe9b9de
//
// Rule 9 (2gb), checked on both: assignVMID opens
// `55 48 89 e5 41 57 41 56 41 55 41 54 53` - pushes and a movq. getPDEValue
// opens `55 48 89 e5 ff c6 8b 87 34` - the `ff c6` at byte 4 is `incl %esi`,
// not a branch, and the first branch is a `jne` 0xe bytes in. Neither has a
// rel8 or rel32 branch beginning in the first five bytes. Both are DEFINED
// in AMDRadeonX6000 per `nm -m`, so the milestone-2c rule holds.
//
// getPDEValue is the function 2hr wanted and could not justify hooking
// until now. It is the one place Apple computes a page-directory entry, and
// its tail is `incq %rax` - it ORs the VALID bit unconditionally. So:
//
//   assignVMID hands out 3 and getPDEValue produced a good value
//                 -> the value exists and the WRITE is missing. That is a
//                    gap in Apple's path on this chip, and the fix is to
//                    perform the write Apple never performs.
//   assignVMID hands out 3 and getPDEValue was never called for it
//                 -> Apple never computes a base for that context either,
//                    and the gap is further upstream than the write
//   assignVMID never hands out 3
//                 -> VMID 3 was never assigned, and the CPF read at
//                    0x4_0030_0000 under VMID 3 (2ht #1) is the chip using
//                    a VMID the driver never bound - a different bug
//   av=0          -> no VMID was ever assigned this boot; the hook is the
//                    silence, not the driver
//
// Observe only. getPDEValue is called per page-table level per bind, so it
// records a per-level digest rather than every call, and never logs from
// inside. assignVMID's out-parameter is read AFTER the original runs, which
// is the only moment it holds the assigned VMID.
// ---------------------------------------------------------------------------

#define RB_2IF_LVLS 4

static void *g_rb_2ig_ctx[16];              // 2ig: VMID -> the bound context
static uint32_t g_rb_2if_av = 0;            // assignVMID calls
static uint16_t g_rb_2if_vmids = 0;         // bit n = VMID n was handed out
static uint32_t g_rb_2if_av_fail = 0;       // calls that returned nonzero
static uint32_t g_rb_2if_pde[RB_2IF_LVLS];  // getPDEValue calls, per level
static uint64_t g_rb_2if_pde_first[RB_2IF_LVLS];
static uint64_t g_rb_2if_pde_last[RB_2IF_LVLS];
static uint32_t g_rb_2if_pde_other = 0;     // levels past the table
static uint32_t g_rb_2if_pde_novalid = 0;   // returns with bit 0 CLEAR
static int      g_rb_2if_hooked = 0;

static uint32_t (*orig_assignVMID_2if)(void *, void *, uint32_t *, void *,
                                       void *, void *) = nullptr;
static uint64_t (*orig_getPDEValue_2if)(void *, uint32_t, uint64_t) = nullptr;

// ---------------------------------------------------------------------------
// 2ih (2026-10-18): correct the page-directory base AT BIND TIME.
//
// 2ig found the defect and it is not the one four milestones were aimed at.
// Context 3's register held 0xFB015000 with HI32 = 0, and that is the LOW
// 32 BITS of that context's own root directory, 0xF4_FB01_5000 - the
// carve-out high bits dropped, and bit 0 along with them. Context 1 is the
// same shape. **This is 2em's bug**, which stripped exactly these high bits
// from CONTEXT0's base, reappearing on contexts 1..15 where nothing has
// ever fixed it.
//
// So the base is not absent and is not a leftover; it is TRUNCATED. A
// walker handed 0x0_FB01_5000 instead of 0xF4_FB01_5000 faults whatever
// bit 0 says, which is what WALKER_ERROR has been reporting for eleven
// boots.
//
// 2ig also proved the register accepts the correction: writing
// 0xFB015001 / 0xF4 read back 0xFB015001 / 0xF4 unchanged. What it could
// not do is help, because by +100 s the CP had been halted for ninety
// seconds and the work owing the timestamp was long discarded (2hp). This
// is the same write at the only moment it can matter - immediately after
// assignVMID binds the context, before any work is submitted under it.
//
// THE VALUE IS STILL NOT GUESSED. Same strict match as 2ig: ctx+0xb8 must
// equal one of the level-0 roots Apple's own allocVMBlock handed out
// (recorded from elem+0x20 / elem+0x28, the fields allocVMBlock itself
// tests). No match, no write, and the refusal is counted and reported.
// ctx+0xb8 is used as the candidate OFFSET only; the match is what makes it
// a value, so a context whose layout differs is refused rather than
// misread.
//
// WHAT IT DOES TO THE CHIP, per bind:
//
//     GCVM_CONTEXT<vmid>_PAGE_TABLE_BASE_ADDR_LO32 = (root & 0xFFFFFFFF) | 1
//     GCVM_CONTEXT<vmid>_PAGE_TABLE_BASE_ADDR_HI32 = root >> 32
//
// dword = 0x1667 + 2*vmid raw, which socRegOffset turns into 0x28C7 + 2*vmid.
// VMID 0 is NEVER touched: 2em already owns context 0 and c0 reads
// 0x7_8FDF_A001, correct and valid. Only contexts 1..15 are corrected, and
// only ones whose root matched.
//
// Gated on -rbgpuptfix, its own boot-arg, separate from 2ig's
// -rbgpuptwrite so the two experiments can never be confused in a capture.
// Without it the hook identifies and counts and writes nothing, which is a
// dry run whose note still names every value.
//
// Risk, stated plainly: this writes a live GPU register during normal
// operation, on every bind, where 2ig wrote twice at a known-dead moment.
// If the value were wrong it could fault differently rather than less. The
// strict match is what bounds that - the only values written are addresses
// Apple's own allocator produced for that very context - and the before and
// after of every write is recorded so a worse outcome is legible rather
// than mysterious.
// ---------------------------------------------------------------------------

#define RB_2IH_CTX_ROOT_OFF 0xb8u      // where 2ig found the root, matched not assumed

static uint32_t g_rb_2ih_calls   = 0;   // binds seen for VMID 1..15
static uint32_t g_rb_2ih_wrote   = 0;
static uint32_t g_rb_2ih_nomatch = 0;
static uint32_t g_rb_2ih_noobj   = 0;
static uint32_t g_rb_2ih_badback = 0;   // read-back did not equal what we wrote
static uint32_t g_rb_2ih_nofb    = 0;   // FB_LOCATION_BASE / FB_OFFSET not read yet
static uint32_t g_rb_2ih_relok   = 0;   // 2em relationship held on a nonzero register
static uint32_t g_rb_2ih_relbad  = 0;   // it did not, and the write was refused
static uint32_t g_rb_2ih_outside = 0;   // 2ij: root outside the FB aperture
static uint32_t g_rb_2ih_already = 0;   // 2ij: already holds what we would write
static uint32_t g_rb_2ih_fbtop   = 0;   // 2ij: GCMC_VM_FB_LOCATION_TOP as read
static uint32_t g_rb_2ih_unresolved = 0;  // 2ik: write held back, aperture unknown
static uint16_t g_rb_2ih_mask    = 0;   // VMIDs corrected
static uint64_t g_rb_2ih_first_root = 0;
static uint32_t g_rb_2ih_first_was_lo = 0, g_rb_2ih_first_was_hi = 0;
static uint32_t g_rb_2ih_first_new_lo = 0, g_rb_2ih_first_new_hi = 0;
static uint32_t g_rb_2ih_first_bk_lo = 0, g_rb_2ih_first_bk_hi = 0;
static int      g_rb_2ih_armed   = -1;

static void rbgpu_ptfix_2ih(void *ctx, uint32_t vmid) {
    if (vmid == 0 || vmid > 15 || !ctx) {
        return;                      // 2em owns context 0; leave it alone
    }
    if (g_rb_2ih_armed < 0) {
        g_rb_2ih_armed = checkKernelArgument("-rbgpuptfix") ? 1 : 0;
    }
    g_rb_2ih_calls++;

    void *obj = g_rb_2fb_regobj;
    if (!obj) { g_rb_2ih_noobj++; return; }
    void **rvt = *reinterpret_cast<void ***>(obj);
    auto resolve = reinterpret_cast<uint32_t (*)(void *, uint64_t)>(
        rvt ? rvt[0x148 / sizeof(void *)] : nullptr);
    auto rd = reinterpret_cast<uint32_t (*)(void *, uint32_t)>(
        rvt ? rvt[0x118 / sizeof(void *)] : nullptr);
    auto wr = reinterpret_cast<void (*)(void *, uint32_t, uint32_t)>(
        rvt ? rvt[0x120 / sizeof(void *)] : nullptr);
    if (!resolve || !rd || !wr) { g_rb_2ih_noobj++; return; }

    uint64_t root = *reinterpret_cast<volatile uint64_t *>(
        reinterpret_cast<uintptr_t>(ctx) + RB_2IH_CTX_ROOT_OFF);

    int matched = 0;
    for (unsigned i = 0; i < g_rb_2ig_nroot; i++) {
        if (root != 0 &&
            (root == g_rb_2ig_root[i].a20 || root == g_rb_2ig_root[i].a28)) {
            matched = 1;
            break;
        }
    }
    if (!matched) { g_rb_2ih_nomatch++; return; }

    // 2ii (2026-10-18): the MC address is NOT what this register wants.
    //
    // 2ih wrote `root | 1` and it took - badback=0, and c1/c3 read back
    // 0xF4_FB00_B001 / 0xF4_FB01_5001 and stayed - and the walker faulted
    // exactly as before. The reason is address space. 2em established what
    // this register wants, and verified it on context 0:
    //
    //     pa = (mc - fb_base) + vram_base_offset
    //          fb_base          = GCMC_VM_FB_LOCATION_BASE << 24
    //          vram_base_offset = GCMC_VM_FB_OFFSET        << 24  (0x780 here)
    //
    // Context 0 is the proof: TTL's table sits at MC 0xF4_0FDF_A000, and
    // 0x0FDF_A000 + 0x7_8000_0000 = 0x7_8FDF_A000, which is what c0 holds
    // and what translates. An MC address in this register is simply wrong,
    // and 2ih put one there.
    //
    // The truncated values 2ig found say the same thing from the other side.
    // The three roots are MC 0xF4_FB00_0000, 0xF4_FB00_B000, 0xF4_FB01_5000,
    // and the registers held 0xFB000000, 0xFB00B000, 0xFB015000 - exactly
    // `root - fb_base`, with vram_base_offset omitted. So this was never a
    // generic truncation: it is **2em's defect**, the same missing
    // vram_base_offset, on contexts 1..15. 2ig's "the carve-out high bits
    // were dropped" was the right observation with the wrong arithmetic
    // behind it.
    //
    // That makes the write self-checking. If the register already holds a
    // nonzero value it must equal `root - fb_base`; when it does, the root,
    // the transform and the defect all agree before anything is written, and
    // when it does not the write is refused rather than guessed.
    if (g_rb_2ed_fb_base == 0 || g_rb_2ed_fb_off == 0) {
        g_rb_2ih_nofb++;
        return;
    }
    uint64_t fb_base = static_cast<uint64_t>(g_rb_2ed_fb_base) << 24;
    uint64_t vram_base_offset = static_cast<uint64_t>(g_rb_2ed_fb_off) << 24;
    if (root < fb_base) { g_rb_2ih_nomatch++; return; }
    uint64_t fb_off = root - fb_base;
    uint64_t want = fb_off + vram_base_offset;

    uint64_t id_lo = ((0x1667ull + 2ull * vmid) << 32) | 0xB;
    uint64_t id_hi = ((0x1668ull + 2ull * vmid) << 32) | 0xB;
    uint32_t off_lo = resolve(obj, id_lo);
    uint32_t off_hi = resolve(obj, id_hi);
    uint32_t was_lo = rd(obj, off_lo);
    uint32_t was_hi = rd(obj, off_hi);
    uint32_t new_lo = static_cast<uint32_t>(want & 0xFFFFFFFFu) | 1u;
    uint32_t new_hi = static_cast<uint32_t>(want >> 32);

    // 2ij (2026-10-18): REFUSE a value outside the FB aperture.
    //
    // 2ii wrote 0x8_7B01_5001 and the register took it and the walker still
    // faulted - because that address does not exist. The machine has 32 GB;
    // the carve-out starts at 30 GB (system-physical 0x7_8000_0000), so the
    // aperture is at most 2 GB. fb_off for these roots is 0xFB01_5000 =
    // 4.2 GB, more than twice the whole carve-out, and the result lands at
    // ~33 GB, past the end of installed RAM.
    //
    // Context 0 is the contrast that proves the rule: its fb_off is
    // 0x0FDF_A000 = 250 MB, inside the aperture, target 0x7_8FDF_A000, and
    // it translates. So mc2pa applies to c0's table and NOT to these roots:
    // they are not VRAM addresses. That fits 2hz finding the page tables
    // with AMDGPU_PTE_SYSTEM set, and 2em's own note that a SYSTEM entry
    // holds a bus address and never goes through mc2pa.
    //
    // relok=3 still stands - the driver really does subtract fb_base - but
    // it now reads as the driver applying a VRAM transform to a non-VRAM
    // address, which would be the defect one level above the register.
    //
    // So the gate: fb_off must be inside the aperture, measured from
    // GCMC_VM_FB_LOCATION_TOP rather than assumed. Writing past the end of
    // RAM is what 2ii did and it must not happen again.
    uint32_t fb_top_raw = rd(obj, resolve(obj, (0x16FDull << 32) | 0xB));
    g_rb_2ih_fbtop = fb_top_raw;

    // 2ik (2026-10-18): STOP WRITING until the aperture map is read.
    //
    // 2ij's gate passed everything (out=0) because fbtop=0xF4FF makes the FB
    // aperture a full 4 GB, MC 0xF4_0000_0000..0xF4_FFFF_FFFF, and the roots
    // at 0xF4_FB01_5000 sit 3.92 GB inside it. The gate was right and the
    // geometry is still contradictory:
    //
    //   FB aperture        4 GB of MC space
    //   carve-out base     system-physical 0x7_8000_0000 = 30 GB
    //   installed RAM      32 GB
    //   aperture top 2 GB  maps to 32..34 GB, which is nothing
    //   the roots          land at ~33.9 GB, in that overhang
    //
    // So the FB aperture is twice the memory behind it and the page-table
    // roots are in the unbacked half. Either vram_base_offset
    // (= GCMC_VM_FB_OFFSET << 24) is not the right base on an APU, or these
    // roots are reached through a DIFFERENT aperture - AGP or the system
    // aperture - whose registers this project has never read.
    //
    // Writing again before knowing which would be guessing a third time, so
    // this boot writes nothing and reads the map instead. The register is
    // left exactly as Apple programmed it, which also restores the baseline
    // that 2ih and 2ii perturbed.
    g_rb_2ih_unresolved++;
    return;
    uint64_t fb_top = (static_cast<uint64_t>(fb_top_raw) << 24) | 0xFFFFFFull;
    if (fb_top <= fb_base || root > fb_top) {
        g_rb_2ih_outside++;
        if (g_rb_2ih_outside == 1) {
            g_rb_2ih_first_root = root;
            g_rb_2ih_first_was_lo = was_lo; g_rb_2ih_first_was_hi = was_hi;
            g_rb_2ih_first_new_lo = static_cast<uint32_t>(want & 0xFFFFFFFFu) | 1u;
            g_rb_2ih_first_new_hi = static_cast<uint32_t>(want >> 32);
        }
        return;      // the root is not in the FB aperture; mc2pa is wrong for it
    }

    // The self-check: a nonzero register must already be `root - fb_base`,
    // OR already be the corrected value we would write (idempotent - 2ii
    // counted 35 refusals of its own correct writes, which was a bug in the
    // check and not a fact about the chip).
    uint64_t cur = (static_cast<uint64_t>(was_hi) << 32) | was_lo;
    if (cur != 0 && (cur & ~0xFFFULL) == (want & ~0xFFFULL)) {
        g_rb_2ih_already++;
        return;
    }
    if (cur != 0 && (cur & ~0xFFFULL) != fb_off) {
        g_rb_2ih_relbad++;
        if (g_rb_2ih_relbad == 1) {
            g_rb_2ih_first_root = root;
            g_rb_2ih_first_was_lo = was_lo; g_rb_2ih_first_was_hi = was_hi;
            g_rb_2ih_first_new_lo = new_lo; g_rb_2ih_first_new_hi = new_hi;
        }
        return;      // the 2em relationship does not hold; do not guess
    }
    if (cur != 0) g_rb_2ih_relok++;

    if (g_rb_2ih_wrote == 0) {
        g_rb_2ih_first_root   = root;
        g_rb_2ih_first_was_lo = was_lo;  g_rb_2ih_first_was_hi = was_hi;
        g_rb_2ih_first_new_lo = new_lo;  g_rb_2ih_first_new_hi = new_hi;
    }
    if (!g_rb_2ih_armed) {
        return;                      // dry run: identified, counted, not written
    }

    wr(obj, off_lo, new_lo);
    wr(obj, off_hi, new_hi);
    uint32_t bk_lo = rd(obj, off_lo);
    uint32_t bk_hi = rd(obj, off_hi);
    if (g_rb_2ih_wrote == 0) { g_rb_2ih_first_bk_lo = bk_lo; g_rb_2ih_first_bk_hi = bk_hi; }
    if (bk_lo != new_lo || bk_hi != new_hi) g_rb_2ih_badback++;
    g_rb_2ih_wrote++;
    g_rb_2ih_mask = static_cast<uint16_t>(g_rb_2ih_mask | (1u << vmid));
}

void rbgpu_ptfix_flush_2ih(void) {
    char note[352];
    snprintf(note, sizeof(note),
             "arm=%d binds=%u wrote=%u nomatch=%u noobj=%u badback=%u "
             "nofb=%u relok=%u relbad=%u out=%u alr=%u unres=%u fbtop=%x "
             "vmids=%04x | first root=%llx "
             "was=%x/%x new=%x/%x back=%x/%x",
             g_rb_2ih_armed, g_rb_2ih_calls, g_rb_2ih_wrote,
             g_rb_2ih_nomatch, g_rb_2ih_noobj, g_rb_2ih_badback,
             g_rb_2ih_nofb, g_rb_2ih_relok, g_rb_2ih_relbad,
             g_rb_2ih_outside, g_rb_2ih_already, g_rb_2ih_unresolved,
             g_rb_2ih_fbtop,
             g_rb_2ih_mask, g_rb_2ih_first_root,
             g_rb_2ih_first_was_lo, g_rb_2ih_first_was_hi,
             g_rb_2ih_first_new_lo, g_rb_2ih_first_new_hi,
             g_rb_2ih_first_bk_lo, g_rb_2ih_first_bk_hi);
    rbgpu_nvram_note("rbgpu-ptfix", note);
    rbgpu_log("2ih: %s", note);
    rbgpu_log("2ij: out=%u is the gate 2ii lacked - a root outside the FB "
              "aperture, where mc2pa does not apply. out=binds with wrote=0 "
              "means these page tables are NOT in the carve-out, so the "
              "register wants their bus address and the value must come from "
              "somewhere other than this subtraction. fbtop=0x%x is "
              "GCMC_VM_FB_LOCATION_TOP as read, so the aperture extent is "
              "measured here rather than inferred from hw.memsize on the "
              "host.", g_rb_2ih_outside, g_rb_2ih_fbtop);
    rbgpu_log("2ii: relok counts binds where the register already held "
              "`root - fb_base`, which is 2em's defect confirmed per context "
              "before the write. relbad counts binds where it held something "
              "else and the write was REFUSED rather than guessed - a nonzero "
              "relbad means the arithmetic is wrong somewhere and the note's "
              "first was= shows for which value.");
    rbgpu_log("2ih: wrote=0 with binds>0 and nomatch=binds means ctx+0xb8 is "
              "not the root for these contexts and NOTHING was written - the "
              "offset 2ig matched once is not general, and the boot says so "
              "rather than writing a wrong value. badback>0 means a register "
              "refused the value, which 2ig's single write did not see.");
}

static uint32_t hooked_assignVMID_2if(void *vmm, void *ctx, uint32_t *vmid,
                                      void *chan, void *ev, void *flag) {
    uint32_t r = orig_assignVMID_2if
                     ? orig_assignVMID_2if(vmm, ctx, vmid, chan, ev, flag)
                     : 0;
    g_rb_2if_av++;
    if (r != 0) g_rb_2if_av_fail++;
    // Read AFTER the original: this is the only moment it holds the answer.
    if (vmid && *vmid < 16) {
        g_rb_2if_vmids = static_cast<uint16_t>(g_rb_2if_vmids | (1u << *vmid));
        g_rb_2ig_ctx[*vmid] = ctx;      // 2ig: which object owns this VMID
        rbgpu_ptfix_2ih(ctx, *vmid);    // 2ih: the write, at the right moment
    }
    return r;
}

static uint64_t hooked_getPDEValue_2if(void *vmm, uint32_t level, uint64_t a) {
    uint64_t v = orig_getPDEValue_2if ? orig_getPDEValue_2if(vmm, level, a) : 0;
    if (level < RB_2IF_LVLS) {
        if (g_rb_2if_pde[level] == 0) g_rb_2if_pde_first[level] = v;
        g_rb_2if_pde_last[level] = v;
        g_rb_2if_pde[level]++;
    } else {
        g_rb_2if_pde_other++;
    }
    if ((v & 1) == 0) g_rb_2if_pde_novalid++;
    return v;
}

void rbgpu_vmid_flush_2if(void) {
    if (!g_rb_2if_hooked) {
        rbgpu_nvram_note("rbgpu-vmid",
                         "hk=0 the assignVMID/getPDEValue hooks did NOT take "
                         "this boot - silence here is the hook, not the driver");
        return;
    }
    char note[352];
    size_t n = 0;
    int w = snprintf(note, sizeof(note),
                     "hk=1 av=%u fail=%u vmids=%04x | pde nv=%u o=%u",
                     g_rb_2if_av, g_rb_2if_av_fail, g_rb_2if_vmids,
                     g_rb_2if_pde_novalid, g_rb_2if_pde_other);
    n = (w > 0) ? static_cast<size_t>(w) : 0;
    for (unsigned i = 0; i < RB_2IF_LVLS; i++) {
        if (g_rb_2if_pde[i] == 0) continue;
        if (n >= sizeof(note) - 56) break;
        w = snprintf(note + n, sizeof(note) - n, " | L%u c%u f%llx l%llx",
                     i, g_rb_2if_pde[i], g_rb_2if_pde_first[i],
                     g_rb_2if_pde_last[i]);
        if (w > 0) {
            n += static_cast<size_t>(w);
            if (n >= sizeof(note)) { n = sizeof(note) - 1; break; }
        }
    }
    rbgpu_nvram_note("rbgpu-vmid", note);
    rbgpu_log("2if: %s", note);
    rbgpu_log("2if: vmids is the question. A bit set for 3 with a good L0/L1 "
              "value means Apple COMPUTED a page-directory base for the "
              "faulting context and never wrote it - the write is the gap, "
              "and performing it is the fix. vmids with bit 3 clear means the "
              "chip faulted under a VMID this driver never bound, which is a "
              "different bug entirely. nv counts returns with the VALID bit "
              "clear: getPDEValue ends `incq %%rax`, so nv should be 0, and "
              "anything else means that reading of the tail is wrong.");
}

static void rbgpu_route_vmid_2if(KernelPatcher &patcher, size_t id,
                                 mach_vm_address_t address, size_t size) {
    if (!checkKernelArgument("-rbgpuringprobe")) {
        rbgpu_log("2if: -rbgpuringprobe not set, assignVMID/getPDEValue left "
                  "alone");
        return;
    }
    KernelPatcher::RouteRequest reqs[] = {
        KernelPatcher::RouteRequest(
            "__ZN23AMDRadeonX6000_AMDHWVMM10assignVMIDEP30AMDRadeonX6000_IAMDHWVMContextRjP30AMDRadeonX6000_AMDAccelChannelPP12IOAccelEventPb",
            hooked_assignVMID_2if, orig_assignVMID_2if),
        KernelPatcher::RouteRequest(
            "__ZN26AMDRadeonX6000_AMDGFX10VMM11getPDEValueE15eAMD_VMPT_LEVELy",
            hooked_getPDEValue_2if, orig_getPDEValue_2if),
    };
    if (patcher.routeMultiple(id, reqs, 2, address, size)) {
        rbgpu_log("2if: hooked AMDHWVMM::assignVMID + AMDGFX10VMM::getPDEValue "
                  "(observe only)");
        g_rb_2if_hooked = 1;
    } else {
        rbgpu_log("2if: FAILED to hook the VMID handout - rbgpu-vmid will say "
                  "hk=0 so an empty note is not mistaken for a driver that "
                  "never assigned a VMID");
        g_rb_2if_hooked = 0;
        patcher.clearError();
    }
}

// ---------------------------------------------------------------------------
// 2iq (2026-10-18): WHO halts the CP, and when.
//
// Two boots now confirm the page-directory correction: s3009bb - CPF under
// VMID 3 - is gone and stays gone, and contexts 1 and 3 both hold corrected
// bases whose candidate tests passed independently. The GPUVM story for
// contexts 1..15 is closed.
//
// What is left is the thing that was there before any of it:
// CP_ME_CNTL = 0x15000000 at the +12 s sample. CE_HALT, PFP_HALT and
// ME_HALT, all set, before any Metal client exists and now with the VMID-3
// fault gone. 2hn guessed the halt is resetHardwareAndReplay's own doing
// rather than the chip's, and that guess has never been tested. It is now
// the only thing between here and a submission that retires.
//
// Two hooks, both DEFINED in AMDRadeonX6000 per `nm -m` and both rule-9
// checked (2gb):
//
//   AMDAccelChannel::resetHardwareAndReplay()   0xbe45288
//     55 48 89 e5 41 57 41 56 41 55 41 54 53   pushes and a movq
//   AMDAccelCPContext::setGPURestart()          0xbe64864
//     55 48 89 e5 c6 87 33 01 00 00 01 5d c3
//
// setGPURestart is the better of the two and the reason this probe is cheap:
// it is three instructions, `movb $0x1, 0x133(%rdi)`, so it REQUESTS a
// restart rather than performing one. Hooking it catches the trigger, and
// the trigger is what has never been seen - every reading so far has been
// of the consequence.
//
// Timing matters as much as counting. A baseline is taken when the hooks
// are routed, and the first call of each is recorded as milliseconds after
// that. If the first setGPURestart lands before the +12 s sample, the CP was
// halted by software before any client ran, and the question becomes what
// that first restart was reacting to. If neither fires at all and the CP is
// still halted, the halt is NOT the reset path and 2hn's guess dies.
//
// Observe only. Both originals run.
// ---------------------------------------------------------------------------

static uint64_t g_rb_2iq_t0 = 0;
static uint32_t g_rb_2iq_reset = 0, g_rb_2iq_setr = 0;
static uint64_t g_rb_2iq_first_reset_ms = 0, g_rb_2iq_first_setr_ms = 0;
static int      g_rb_2iq_hooked = 0;

static uint64_t rbgpu_2iq_ms(void) {
    if (g_rb_2iq_t0 == 0) return 0;
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time() - g_rb_2iq_t0, &ns);
    return ns / 1000000ull;
}

static void (*orig_resetHWAndReplay_2iq)(void *) = nullptr;
static void (*orig_setGPURestart_2iq)(void *) = nullptr;

static void hooked_resetHWAndReplay_2iq(void *ch) {
    if (g_rb_2iq_reset == 0) g_rb_2iq_first_reset_ms = rbgpu_2iq_ms();
    g_rb_2iq_reset++;
    if (orig_resetHWAndReplay_2iq) orig_resetHWAndReplay_2iq(ch);
}

static void hooked_setGPURestart_2iq(void *cp) {
    if (g_rb_2iq_setr == 0) g_rb_2iq_first_setr_ms = rbgpu_2iq_ms();
    g_rb_2iq_setr++;
    if (orig_setGPURestart_2iq) orig_setGPURestart_2iq(cp);
}

void rbgpu_mecntl_flush_2ir(void) {
    char note[352];
    size_t n = 0;
    int w = snprintf(note, sizeof(note), "x6k n=%u o=%u last=%x",
                     g_rb_2ir_n, g_rb_2ir_over, g_rb_2ir_last);
    n = (w > 0) ? static_cast<size_t>(w) : 0;
    for (uint32_t i = 0; i < g_rb_2ir_n && n < sizeof(note) - 32; i++) {
        w = snprintf(note + n, sizeof(note) - n, " [%x@%llu]",
                     g_rb_2ir_val[i], g_rb_2ir_ms[i]);
        if (w > 0) { n += static_cast<size_t>(w); if (n >= sizeof(note)) { n = sizeof(note) - 1; break; } }
    }
    if (n < sizeof(note) - 24) {
        w = snprintf(note + n, sizeof(note) - n, " | hwl n=%u o=%u",
                     g_rb_2ir_h_n, g_rb_2ir_h_over);
        if (w > 0) n += static_cast<size_t>(w);
    }
    for (uint32_t i = 0; i < g_rb_2ir_h_n && n < sizeof(note) - 32; i++) {
        w = snprintf(note + n, sizeof(note) - n, " [%x@%llu]",
                     g_rb_2ir_h_val[i], g_rb_2ir_h_ms[i]);
        if (w > 0) { n += static_cast<size_t>(w); if (n >= sizeof(note)) { n = sizeof(note) - 1; break; } }
    }
    rbgpu_nvram_note("rbgpu-me", note);
    rbgpu_log("2ir: %s", note);
    rbgpu_log("2ir: a write of 0x15000000 and no later write of 0 is the "
              "answer - Apple halts the CP during bring-up and never "
              "un-halts it, so RB0 cannot execute and every stalled rptr "
              "since 2hl follows. Writes on NEITHER path with the register "
              "still reading 0x15000000 means firmware or the PSP left it "
              "that way and no driver code is responsible.");
}

void rbgpu_restart_flush_2iq(void) {
    char note[352];
    snprintf(note, sizeof(note),
             "hk=%d setr=%u @%llums | reset=%u @%llums",
             g_rb_2iq_hooked, g_rb_2iq_setr, g_rb_2iq_first_setr_ms,
             g_rb_2iq_reset, g_rb_2iq_first_reset_ms);
    rbgpu_nvram_note("rbgpu-rst", note);
    rbgpu_log("2iq: %s", note);
    rbgpu_log("2iq: setr=0 reset=0 with the CP still halted means the halt is "
              "NOT the reset path and 2hn's guess is dead - which would send "
              "this back to what programs CP_ME_CNTL directly. A first "
              "setGPURestart before 12000 ms means software halted the CP "
              "before any client ran, and the next question is what that "
              "first restart was reacting to.");
}

static void rbgpu_route_restart_2iq(KernelPatcher &patcher, size_t id,
                                    mach_vm_address_t address, size_t size) {
    if (!checkKernelArgument("-rbgpuringprobe")) return;
    g_rb_2iq_t0 = mach_absolute_time();
    KernelPatcher::RouteRequest reqs[] = {
        KernelPatcher::RouteRequest(
            "__ZN30AMDRadeonX6000_AMDAccelChannel22resetHardwareAndReplayEv",
            hooked_resetHWAndReplay_2iq, orig_resetHWAndReplay_2iq),
        KernelPatcher::RouteRequest(
            "__ZN32AMDRadeonX6000_AMDAccelCPContext13setGPURestartEv",
            hooked_setGPURestart_2iq, orig_setGPURestart_2iq),
    };
    if (patcher.routeMultiple(id, reqs, 2, address, size)) {
        rbgpu_log("2iq: hooked resetHardwareAndReplay + setGPURestart "
                  "(observe only)");
        g_rb_2iq_hooked = 1;
    } else {
        rbgpu_log("2iq: FAILED to hook the restart path - rbgpu-rst will say "
                  "hk=0, so zero counts are the hook and not the driver");
        g_rb_2iq_hooked = 0;
        patcher.clearError();
    }
}

static void (*orig_programAndInvalidateVM_2id)(void *, const void *) = nullptr;

static void hooked_programAndInvalidateVM_2id(void *vmm, const void *info) {
    g_rb_2id_pivm++;
    if (info) {
        uint32_t hub = *reinterpret_cast<const uint32_t *>(info);
        if (hub < 16) {
            g_rb_2id_pivm_hubs = static_cast<uint16_t>(
                g_rb_2id_pivm_hubs | (1u << hub));
        }
    }
    if (orig_programAndInvalidateVM_2id) {
        orig_programAndInvalidateVM_2id(vmm, info);
    }
}

static void rbgpu_route_hwregwrite_2ic(KernelPatcher &patcher, size_t id,
                                       mach_vm_address_t address, size_t size) {
    if (!checkKernelArgument("-rbgpuringprobe")) {
        rbgpu_log("2ic: -rbgpuringprobe not set, AMDHWRegisters::write left "
                  "alone");
        return;
    }
    g_rb_2ic_armed = 1;
    KernelPatcher::RouteRequest req(
        "__ZN29AMDRadeonX6000_AMDHWRegisters5writeEjj",
        hooked_hwreg_write_2ic, orig_hwreg_write_2ic);
    if (patcher.routeMultiple(id, &req, 1, address, size)) {
        rbgpu_log("2ic: hooked AMDHWRegisters::write (observe only) - every GC "
                  "register write, read-modify-writes included, now passes "
                  "through a range compare on 0x%x..0x%x",
                  RB_2IC_FIRST, RB_2IC_LAST);
        g_rb_2ic_hooked = 1;
    } else {
        rbgpu_log("2ic: FAILED to hook AMDHWRegisters::write - rbgpu-ctxw will "
                  "say hk=0 so the empty note is not mistaken for a chip that "
                  "never wrote the registers");
        g_rb_2ic_hooked = 0;
        patcher.clearError();
    }

    // 2ki (2026-10-20): correct the page-directory address IN the invalidate
    // request, which is the last place it is touched before it becomes packets.
    //
    // 2kh located the halt: CP_IB1_BASE = 0x4_0030_0000 and the GCHUB fault
    // address are the SAME number in the same sample, CID=4 (CPF) READ under
    // VMID 3. The CP fetches an IB through a page directory it cannot
    // translate and parks all three engines - CP_ME_CNTL = 0x15000000 is the
    // result, not the act.
    //
    // 2ix's fix was right and lands too late: it writes at VM *bind* time,
    // and rbgpu-vm reads c3 = fb015000 (Apple's) at +12 s against 87b015001
    // at +100 s, while the halt is at 8.88 s - before both.
    //
    // Why a host register write cannot win that race, and this is the part
    // 2hr/2ia already found independently: on gfx10 the context page-table
    // base for VMIDs 1-15 is **not written by host MMIO at all**. Linux's
    // `gmc_v10_0_emit_flush_gpu_tlb` emits it with `amdgpu_ring_emit_wreg` -
    // the CP writes it from the ring. 2hr could not find an MMIO write path
    // because there is none. And 2kh's `CP_ME_HEADER_DUMP = 0xc0033700` is a
    // 5-dword WRITE_DATA, the exact shape of `gfx_v10_0_write_data_to_reg`.
    //
    // So the value must be corrected BEFORE the packet is built.
    // `prepareVMInvalidateRequest` (0xbe9b4b4) is where it is assembled, and
    // the disassembly shows the defect plainly:
    //
    //     be9b505: movq 0x18(%r14), %rcx     ; info->0x18 = the PD address
    //     be9b509: movl %ecx, 0x4(%rbx)      ; LO32 -> request+0x04
    //     be9b50c: shrq $0x20, %rcx
    //     be9b510: movl %ecx, 0xc(%rbx)      ; HI32 -> request+0x0c
    //
    // Copied through verbatim: no `vram_base_offset`, no VALID bit - while
    // the START/END addresses immediately below ARE processed (>>12, >>44).
    // On Navi 21 `vram_base_offset` is 0 so the omission is invisible; on an
    // APU with FB_OFFSET 0x780 it points at unmapped memory.
    //
    // The correction is Linux's own: `amdgpu_gmc_pd_addr` ORs
    // `AMDGPU_PTE_VALID` for Vega10+ at every depth, and
    // `amdgpu_gmc_vram_mc2pa` adds `vram_base_offset`. Apple's own
    // `getPDEValue` (0xbe9b9de) ends `incq %rax`, so every PDE Apple builds
    // has bit 0 set - these registers are anomalous by Apple's own convention.
    //
    // GUARDED, because a blind add would corrupt a correct value (2cy):
    // the rewrite happens only when the value is nonzero, has bit 0 CLEAR,
    // and is BELOW vram_start. Apple's `0xfb015000` satisfies all three;
    // an already-correct `0x87b015001` satisfies none and is left alone.
    // Every decision is logged, including the ones that decline.
    //
    // Behind -rbgpupdefix. It writes a value the GPU will use, so it is
    // opt-in.
    rbgpu_route_pdefix_2ki(patcher, id, address, size);

    // 2id: the VM flush. Rule 9 (2gb): programAndInvalidateVM at 0xbe9b7a2
    // opens `55 48 89 e5 41 57 41 56 41 55 41 54 53` - pushes, a movq and
    // nothing else; the first branch is 0x9a bytes in. Checked, not assumed.
    KernelPatcher::RouteRequest pivm(
        "__ZN26AMDRadeonX6000_AMDGFX10VMM22programAndInvalidateVMEPK22AMD_VM_INVALIDATE_INFO",
        hooked_programAndInvalidateVM_2id, orig_programAndInvalidateVM_2id);
    if (patcher.routeMultiple(id, &pivm, 1, address, size)) {
        rbgpu_log("2id: hooked AMDGFX10VMM::programAndInvalidateVM (observe "
                  "only) - counts the VM flush, which is the only thing that "
                  "writes a context page-directory base in Linux");
        g_rb_2id_pivm_hooked = 1;
    } else {
        rbgpu_log("2id: FAILED to hook programAndInvalidateVM - pivm=0 in the "
                  "note will mean the hook, not a flush that never ran");
        g_rb_2id_pivm_hooked = 0;
        patcher.clearError();
    }
}

static void hooked_getVMFaultInfo_2hs(void *vmm, uint32_t hub, void *info) {
    g_rb_2hs_calls++;

    // Poison first: Apple only writes the struct when it has a fault, so an
    // unchanged struct must be distinguishable from one it filled with
    // zeroes. Absence is not a result (2eh, 2ek, 2fb).
    const uint64_t kPoison = 0xDEADBEEFDEADBEEFull;
    if (info) {
        *reinterpret_cast<uint64_t *>(info) = kPoison;
        *reinterpret_cast<uint32_t *>(reinterpret_cast<uintptr_t>(info) + 0x14) = 0xDEADBEEF;
    }

    if (orig_getVMFaultInfo_2hs) {
        orig_getVMFaultInfo_2hs(vmm, hub, info);
    }
    if (!info) {
        return;
    }

    uintptr_t p = reinterpret_cast<uintptr_t>(info);
    uint64_t va  = *reinterpret_cast<uint64_t *>(p);
    uint32_t st  = *reinterpret_cast<uint32_t *>(p + 0x14);
    if (va == kPoison && st == 0xDEADBEEF) {
        return;   // Apple looked and found nothing; the call is counted above
    }

    g_rb_2hs_faults++;
    uint32_t vmid = *reinterpret_cast<uint32_t *>(p + 0x08);
    uint8_t  rw   = *reinterpret_cast<uint8_t  *>(p + 0x0c);
    uint32_t cid  = *reinterpret_cast<uint32_t *>(p + 0x10);

    // Find or add the (hub, status) signature.
    int slot = -1;
    for (unsigned i = 0; i < RB_2HU_SIGS; i++) {
        if (g_rb_2hu[i].used && g_rb_2hu[i].status == st && g_rb_2hu[i].hub == hub) {
            slot = static_cast<int>(i);
            break;
        }
    }
    bool is_new = false, first_nonzero = false;
    if (slot < 0) {
        for (unsigned i = 0; i < RB_2HU_SIGS; i++) {
            if (!g_rb_2hu[i].used) {
                slot = static_cast<int>(i);
                g_rb_2hu[i].used = 1;
                g_rb_2hu[i].status = st;
                g_rb_2hu[i].hub = hub;
                g_rb_2hu[i].first_va = va;
                g_rb_2hu[i].vmid = vmid;
                g_rb_2hu[i].cid = cid;
                g_rb_2hu[i].rw = rw;
                is_new = true;
                break;
            }
        }
    }
    if (slot < 0) {
        g_rb_2hu_other++;     // table full: counted, never silently dropped
        return;
    }
    g_rb_2hu[slot].count++;
    if (va > g_rb_2hu[slot].max_va) {
        first_nonzero = (g_rb_2hu[slot].max_va == 0 && va != 0);
        g_rb_2hu[slot].max_va = va;
    }

    if (is_new) {
        rbgpu_log("2hu: NEW fault signature hub=%u status=0x%08x VA=0x%llx "
                  "vmid=%u %s cid=%u (cid 4 = CPF; status 0xffffffff on hub 1 "
                  "is a dead-register read, not a fault - 2ht)",
                  hub, st, va, vmid, rw ? "WRITE" : "READ", cid);
    }
    if (first_nonzero) {
        rbgpu_log("2hu: signature 0x%08x hub %u produced its FIRST NONZERO VA "
                  "0x%llx after %u faults - the zero was the address register "
                  "being cleared, not a null target",
                  st, hub, va, g_rb_2hu[slot].count);
    }

    // Commit only on a change that matters, and never more than
    // RB_2HU_MAX_COMMITS times: the faults recur all boot and a write per
    // fault is the 2fo wedge.
    if ((is_new || first_nonzero) && g_rb_2hu_commits < RB_2HU_MAX_COMMITS) {
        rbgpu_fault_note_2hu();
    }
}

static void hooked_fillFaultAccelInfo_2hs(void *vmm) {
    g_rb_2hs_fill++;
    // The record it builds goes to kprintf, which we cannot read. Counting it
    // still answers a real question: whether Apple's fault REPORTING path
    // runs at all, as opposed to the fault merely being latched in hardware.
    if (g_rb_2hs_fill <= 3) {
        rbgpu_log("2hs: fillFaultAccelInfo #%u - Apple IS assembling a fault "
                  "report (it goes to kprintf, which reaches neither the "
                  "unified log nor ioreg, which is why no armed boot has ever "
                  "shown one)", g_rb_2hs_fill);
    }
    if (orig_fillFaultAccelInfo_2hs) {
        orig_fillFaultAccelInfo_2hs(vmm);
    }
}

// Its own function: processX6000's frame is already near the 1024-byte limit
// and an inline RouteRequest pushed it over once (2gv).
static void rbgpu_route_vmfault_2hs(KernelPatcher &patcher, size_t id,
                                    mach_vm_address_t address, size_t size) {
    KernelPatcher::RouteRequest reqs[] = {
        KernelPatcher::RouteRequest(
            "__ZN26AMDRadeonX6000_AMDGFX10VMM14getVMFaultInfoE16eAMD_VM_HUB_TYPEPN23AMDRadeonX6000_AMDHWVMM13VM_FAULT_INFOE",
            hooked_getVMFaultInfo_2hs, orig_getVMFaultInfo_2hs),
        KernelPatcher::RouteRequest(
            "__ZN23AMDRadeonX6000_AMDHWVMM18fillFaultAccelInfoEv",
            hooked_fillFaultAccelInfo_2hs, orig_fillFaultAccelInfo_2hs),
    };
    if (patcher.routeMultiple(id, reqs, 2, address, size)) {
        rbgpu_log("2hs: hooked getVMFaultInfo + fillFaultAccelInfo (observe only)");
        g_rb_2hs_hooked = 1;
    } else {
        rbgpu_log("2hs: FAILED to hook the VM fault path - rbgpu-fault will "
                  "stay empty this boot for that reason, NOT because there "
                  "were no faults");
        g_rb_2hs_hooked = 0;
        patcher.clearError();
    }
}

// 2gv: routed unconditionally; the substitution itself is gated on
// `-rbgpuvmpteng1` inside the hook, so a boot without the arg still logs
// that the remap is OFF rather than leaving it ambiguous whether the route
// took.
static void rbgpu_route_getHWChannel_2gv(KernelPatcher &patcher, size_t id,
                                         mach_vm_address_t address,
                                         size_t size) {
    KernelPatcher::RouteRequest req(
        "__ZN26AMDRadeonX6000_AMDHardware12getHWChannelE20_eAMD_HW_ENGINE_TYPE18_eAMD_HW_RING_TYPE",
        hooked_hw_getHWChannel_2gv, orig_hw_getHWChannel_2gv);
    if (patcher.routeMultiple(id, &req, 1, address, size)) {
        rbgpu_log("2gv: hooked AMDHardware::getHWChannel(engine, ring)");
    } else {
        rbgpu_log("2gv: FAILED to hook AMDHardware::getHWChannel - the VMPT "
                  "engine remap CANNOT happen this boot, whatever boot-args "
                  "say");
        patcher.clearError();
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

    // 2fu: AMDGFX10SDMAEngine::start - what startHWEngines actually calls on
    // each of the four SDMA engines.
    //
    // 2ga (2026-10-14): OFF unless -rbgpusdmahook. Two armed boots panicked
    // identically, at different KASLR slides, with this backtrace:
    //
    //   Lilu  (Crypto::hash + 0xc12 / +0xc3a, i.e. Lilu+0x1ae22 / +0x1ae4a)
    //   X6000 AMDHardware::startHWEngines + 0x3e     <- the vtable[0x148] CALL
    //   plugin hooked_hw_startHWEngines + 0x384
    //
    // page fault, error code 0x3 (present | WRITE), with CR2 = RIP - 11 and
    // both inside Lilu's own __TEXT. So Apple's call through the SDMA engine's
    // vtable[0x148] lands in Lilu and faults writing to Lilu's own text.
    //
    // The milestone-2c rule is NOT what went wrong: `nm -m build/re/
    // AMDRadeonX6000` shows __ZN33AMDRadeonX6000_AMDGFX10SDMAEngine5startEv
    // as "external __TEXT,__text" at 0xbea4a6a, so the symbol really is
    // defined here and solveSymbol cannot have returned a bare slide. That
    // was checked before writing any of this down.
    //
    // 2gb (2026-10-15) ANSWERED, and the mechanism is identified. The armed
    // boot with this route OFF did not panic - it ran the whole chain to
    // handlePostRegister. So **this route is the cause**, and the reason is in
    // the function's own first six bytes:
    //
    //   bea4a6a: 80 7f 10 00   cmpb $0x0, 0x10(%rdi)
    //   bea4a6e: 74 03         je   0xbea4a73          <- RELATIVE, rel8 = +3
    //   bea4a70: 31 c0         xorl %eax, %eax
    //   bea4a72: c3            retq
    //   bea4a73: 55            pushq %rbp              <- the branch target
    //
    // An x86-64 route needs at least 5 bytes at the entry for its jump, so the
    // patched window covers the `je` at +4, and Lilu copies the displaced
    // instructions byte-for-byte into a trampoline so `orig` can be called.
    // `74 03` is a SHORT conditional branch whose displacement is relative to
    // the instruction's own address: copied to a trampoline at a different
    // address it still reads "+3", so it no longer lands on 0xbea4a73 but 3
    // bytes further into Lilu's own trampoline buffer, and whatever is there
    // is executed. That accounts for every field of the panic - RIP inside
    // Lilu's __TEXT, a WRITE fault at RIP-11 also inside Lilu, the caller
    // being startHWEngines+0x3e (the very first entry into the routed
    // function), and identical behaviour at two different KASLR slides, since
    // a wrong displacement is slide-independent.
    //
    // **And it says why twenty other routes in this project worked.** Every
    // one of them begins with straight-line setup (`pushq %rbp; movq
    // %rsp,%rbp; ...`). This is the first function routed whose prologue
    // contains a relative branch inside the patch window, and it is a general
    // rule worth keeping: before routing a symbol, disassemble its first 16
    // bytes and refuse if a rel8/rel32 branch starts within the first 5.
    //
    // The route therefore stays OFF, and the question it was added for - which
    // SDMA engine refuses - is answered WITHOUT it, by reading Apple's own
    // started-flag after the walk (rbgpu_poststarthw_2gb above). Plain loads,
    // no patching, no trampoline that can be wrong.
    if (checkKernelArgument("-rbgpusdmahook")) {
        KernelPatcher::RouteRequest req_sdma(
            "__ZN33AMDRadeonX6000_AMDGFX10SDMAEngine5startEv",
            hooked_sdma_start_2fu,
            orig_sdma_start
        );
        if (patcher.routeMultiple(id, &req_sdma, 1, address, size)) {
            rbgpu_log("X6000: hooked AMDGFX10SDMAEngine::start (2fu, -rbgpusdmahook)");
        } else {
            rbgpu_log("X6000: AMDGFX10SDMAEngine::start not found (2fu)");
        }
    } else {
        rbgpu_log("2gb: SDMA start route SKIPPED - PROVEN to panic (its "
                  "prologue has a rel8 `je` inside the patch window, so "
                  "Lilu's trampoline copy branches into Lilu's own text). "
                  "The started-flag read in 2gb answers the question instead.");
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

    // 2du: the two named gates in AMDHWChannel::init that decide whether
    // channel+0xe0 gets the scheduler index or stays -1. Observe only.
    KernelPatcher::RouteRequest req_2du[] = {
        KernelPatcher::RouteRequest(
            "__ZN26AMDRadeonX6000_AMDHardware15registerChannelEP28AMDRadeonX6000_IAMDHWChannel",
            hooked_hw_registerChannel_2du, orig_hw_registerChannel_2du),
        KernelPatcher::RouteRequest(
            "__ZN29AMDRadeonX6000_AMDSWScheduler12addHWChannelEP28AMDRadeonX6000_IAMDHWChannelm",
            hooked_sched_addHWChannel_2du, orig_sched_addHWChannel_2du),
    };
    if (patcher.routeMultiple(id, req_2du, 2, address, size)) {
        rbgpu_log("X6000: hooked registerChannel + addHWChannel (2du observe-only)");
    } else {
        rbgpu_log("X6000: FAILED to hook the 2du channel-registration gates");
    }

    // 2gn: the three named calls request 0x1B makes after its isDeviceValid
    // gate. Observe only; the unnamed (this->0x380)->vtable[0x158] between
    // hardwareDidStart and enableInterrupts is localised by elimination.
    // Rule 9 checked on all three: each opens pushq %rbp / movq %rsp,%rbp /
    // pushq %r14, with no relative branch inside the 5-byte patch window.
    // 2gv: the engine index VMPT resolves to. In its own function so it
    // does not grow processX6000's stack frame past the 1024-byte limit.
    rbgpu_route_getHWChannel_2gv(patcher, id, address, size);
    rbgpu_route_vmfault_2hs(patcher, id, address, size);
    rbgpu_route_hwregwrite_2ic(patcher, id, address, size);
    rbgpu_route_vmid_2if(patcher, id, address, size);
    rbgpu_route_restart_2iq(patcher, id, address, size);

    KernelPatcher::RouteRequest req_2gn[] = {
        KernelPatcher::RouteRequest(
            "__ZN31AMDRadeonX6000_AMDGFX10Hardware23startGlobalPerfCountersEv",
            hooked_hw_startGlobalPerfCounters_2gn, orig_hw_startGlobalPerfCounters_2gn),
        KernelPatcher::RouteRequest(
            "__ZN31AMDRadeonX6000_AMDGFX10Hardware16hardwareDidStartEv",
            hooked_hw_hardwareDidStart_2gn, orig_hw_hardwareDidStart_2gn),
        KernelPatcher::RouteRequest(
            "__ZN26AMDRadeonX6000_AMDHardware16enableInterruptsEv",
            hooked_hw_enableInterrupts_2gn, orig_hw_enableInterrupts_2gn),
    };
    if (patcher.routeMultiple(id, req_2gn, 3, address, size)) {
        rbgpu_log("X6000: hooked startGlobalPerfCounters + hardwareDidStart + "
                  "enableInterrupts (2gn observe-only)");
    } else {
        rbgpu_log("X6000: FAILED to hook the 2gn 0x1B post-gate calls");
    }

    rbgpu_route_2go(patcher, id, address, size);

    // 2eg: where Apple's VM allocator puts each page-table block.
    KernelPatcher::RouteRequest req_2eg(
        "__ZN23AMDRadeonX6000_AMDHWVMM12allocVMBlockE15eAMD_VMPT_LEVELP16GLKMemoryElement",
        hooked_vmm_allocVMBlock_2eg, orig_vmm_allocVMBlock_2eg);
    if (patcher.routeMultiple(id, &req_2eg, 1, address, size)) {
        rbgpu_log("X6000: hooked AMDHWVMM::allocVMBlock (2eg observe-only)");
    } else {
        rbgpu_log("X6000: FAILED to hook AMDHWVMM::allocVMBlock");
    }

    // 2dy: the WRITE_DATA packet that carries the completion fence. Observe
    // only - the original runs first and the built packet is read back.
    KernelPatcher::RouteRequest req_2dy(
        "__ZN35AMDRadeonX6000_AMDGFX10KIQHWChannel18writeDataCmdPacketEPj",
        hooked_kiq_writeDataCmd_2dy, orig_kiq_writeDataCmd_2dy);
    if (patcher.routeMultiple(id, &req_2dy, 1, address, size)) {
        rbgpu_log("X6000: hooked KIQ writeDataCmdPacket (2dy observe-only)");
    } else {
        rbgpu_log("X6000: FAILED to hook writeDataCmdPacket (2dy)");
    }

    // 2dx: resolve the GPU-address counterpart of the writeback frame the KIQ
    // channel polls. Defined in X6000 (nm shows it as a T symbol), so looking
    // it up by this kext's id is safe under the milestone-2c Lilu rule. Not
    // routed - only called, read-only, from the waitForHwStamp hook.
    orig_wb_gpuaddr_2dx = reinterpret_cast<wb_gpuaddr_t>(patcher.solveSymbol(
        id, "__ZNK26AMDRadeonX6000_AMDHardware31getChannelWriteBackFrameGPUAddrEj"));
    if (!orig_wb_gpuaddr_2dx) {
        patcher.clearError();
        rbgpu_log("X6000: 2dx could NOT resolve getChannelWriteBackFrameGPUAddr");
    } else {
        rbgpu_log("X6000: 2dx resolved getChannelWriteBackFrameGPUAddr at 0x%llx",
                  reinterpret_cast<uint64_t>(orig_wb_gpuaddr_2dx));
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

// 2hg: the address of IOGraphicsAccelerator2::enableAccelerator, solved
// out of IOAcceleratorFamily2 so the peer can CALL it rather than set the
// flag itself.
//
// 2hf established the whole chain: Metal's first buffer allocation enters
// IOAccelSharedUserClient2::new_resource, which calls
// acceleratorWaitEnabled, which tests bit 0x2 of accel+0xc78 and sleeps in
// waiting_for_fEnabled when it is clear. The ONLY setter of that bit is
// enableAccelerator (0x14e55570, `orb $0x2, 0xc78`), and its only two
// callers are IOAccelDisplayMachine::display_mode_did_change and the
// Legacy variant. 2ct refuses the display pipe, so neither ever runs.
//
// Calling Apple's function rather than writing the bit, because it also
// starts the event machine's hardware progress timer:
//
//   testb $0x8, 0xc92(%rdi)
//   jne   skip
//     (accel->0x380)->startHardwareProgressTimerUnlocked()
//   skip:
//   orb   $0x2, 0xc78(%rbx)
//
// Milestone 2c: external and DEFINED in IOAcceleratorFamily2, so it is
// solved under THIS kext's id, not X6000's.
void (*g_rb_enableAccelerator_2hg)(void *) = nullptr;

void RGPU::processAccelFamily(KernelPatcher &patcher, size_t id, mach_vm_address_t address,
                              size_t size) {
    // Solved, not routed: we want to call it, not intercept it.
    auto ea = patcher.solveSymbol(id, "__ZN22IOGraphicsAccelerator217enableAcceleratorEv",
                                  address, size);
    if (ea) {
        g_rb_enableAccelerator_2hg = reinterpret_cast<void (*)(void *)>(ea);
        rbgpu_log("2hg: enableAccelerator solved at %p", reinterpret_cast<void *>(ea));
        rbgpu_diag_set_num("ea_solved", 1);
    } else {
        // Absence is not a result: say it, so a boot with -rbgpuforceenable
        // that changes nothing is not read as "enabling did not help".
        patcher.clearError();
        rbgpu_log("2hg: FAILED to solve enableAccelerator - -rbgpuforceenable "
                  "CANNOT do anything this boot, whatever the boot-args say");
        rbgpu_diag_set_num("ea_solved", 0);
    }

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

    // 2gp: count the channel restarts that finishStamp's unconditional retry
    // at 0x14e439a0 drives. External and defined in this kext's __TEXT, and
    // rule-9 clean (pushes and movs through +0xe).
    KernelPatcher::RouteRequest req_2gp(
        "__ZN20IOAccelEventMachine226handleFinishChannelRestartEiij",
        hooked_evm_handleFinishChannelRestart_2gp,
        orig_evm_handleFinishChannelRestart_2gp
    );
    if (patcher.routeMultiple(id, &req_2gp, 1, address, size)) {
        rbgpu_log("AccelFamily: hooked handleFinishChannelRestart (2gp counter)");
    } else {
        rbgpu_log("AccelFamily: FAILED to hook handleFinishChannelRestart (2gp)");
    }
}

void RGPU::processHWServices(KernelPatcher &patcher, size_t id, mach_vm_address_t address, size_t size) {
    rbgpu_log("com.apple.kext.AMDRadeonX6000HWServices matched and monitored");
}


// ---------------------------------------------------------------------------
// 2hk (2026-10-18): the ring's own pointers, read while the wait is happening.
//
// 2hj left one question that decides the whole next direction. A kernel thread
// named AMDRadeonX6000_AMDAccelEventMachine sits in resetHardwareAndReplay ->
// waitForTimestamp -> pollWait, polling for a timestamp that never arrives;
// WindowServer blocks on its mutex and the Metal submit blocks behind the same
// restart, reaching it through waitForRingBufferSpace. So the GPU is handed
// work and nothing retires. Three things could be true, and they send the work
// in three different directions:
//
//   wptr advanced, rptr frozen   the GPU is not FETCHING. The ring is set up
//                                and the doorbell rang, but the CP is not
//                                consuming. 2di (gfx_min = gfx_max = 0) becomes
//                                a real lead rather than a guess.
//   both advanced, no timestamp  the GPU EXECUTED and the completion path is
//                                broken - fence address, writeback, or
//                                interrupts. 2di is eliminated.
//   wptr never advanced          the driver never rang the doorbell at all. The
//                                bug is upstream of the hardware entirely.
//
// Nothing in this project has read those pointers. Until one of the three is
// picked by a measurement, any work on 2di is a guess with a name on it.
//
// How this is done, and why it is not -rbgpuregdump (which is permanently OUT,
// 2fe: the GC register dump stalled non-deterministically):
//
//   - SIX registers at ONE point, a fixed list, no sweep and no loop.
//   - Through the accessor this project already uses and has measured working:
//     vtable[0x148] resolves an encoded id to a dword offset, vtable[0x118]
//     reads it (2fd, 2fg; resolved twice in 2fi). Offsets agree TWO ways -
//     Linux's gc_10_3_0_offset.h and Apple's own disassembly at 0xbea143b,
//     where doStart reads exactly CP_RB0_RPTR, CP_RB0_WPTR_HI and CP_RB0_WPTR
//     through the same slots.
//   - Behind its own boot-arg, off by default.
//   - On its own thread_call, armed AFTER all three prog2 samples have fired,
//     so if a read does stall (2fe's failure) it cannot cost us those.
//   - An ENTER note is committed to NVRAM BEFORE the first read. A stall and a
//     zero must not look alike (2eh, 2ek, 2fb): if `rbgpu-ring` reads
//     "ENTER..." and nothing else, the read is what stalled, and that is
//     itself the answer about this accessor.
//   - Every refusal path says which check failed. No silent return.
//
// This reads only. It writes no register, rings no doorbell and touches no
// queue - unlike 2fi's waitForActiveValue reproduction, which had to write a
// selector. Read-only is what keeps it clear of the 2cy rule: nothing here is
// substituted, so nothing here can be narrated as a hardware result that is
// not one.
#define RB_2HK_CP_RB0_RPTR    0x0F600000000Bull
#define RB_2HK_CP_RB0_WPTR    0x1DF40000000Bull
#define RB_2HK_CP_RB0_WPTR_HI 0x1DF50000000Bull
#define RB_2HK_CP_STAT        0x0F400000000Bull
#define RB_2HK_GRBM_STATUS    0x0DA40000000Bull
// 2hm: the registers 2hl's reading made worth asking for. Every BASE_IDX is
// 0 in gc_10_3_0_offset.h, the same segment as CP_RB0_CNTL, whose resolution
// through this accessor is proven (2fe, and seg0 = 0x1260 agreed four ways).
#define RB_2HM_GRBM_STATUS2   0x0DA20000000Bull
#define RB_2HM_CP_ME_CNTL     0x0F560000000Bull
#define RB_2HM_VMFAULT_STATUS 0x15C80000000Bull
#define RB_2HM_VMFAULT_LO32   0x15C90000000Bull
#define RB_2HM_VMFAULT_HI32   0x15CA0000000Bull
#define RB_2HM_VMFAULT_CNTL   0x15C40000000Bull
#define RB_2HM_CP_RB0_BASE    0x1DE00000000Bull
#define RB_2HM_CP_RB0_BASE_HI 0x1E510000000Bull

static int g_rb_2hk_on = -1;

// ---------------------------------------------------------------------------
// 2hq (2026-10-18): the GCVM context registers, for the contexts that faulted.
//
// 2hp found the faulting client is CPF and caught two faults: VMID 3 on a
// read at +12 s, VMID 0 on a write at +100 s. **2em only ever patched
// CONTEXT0** - its `dwPTB_LO = 0x1667` is GCVM_CONTEXT0_PAGE_TABLE_BASE_ADDR,
// VMID 0. Nothing in this project has ever looked at context 3, let alone
// written it.
//
// 2em also wrote down, a week before the evidence arrived, exactly what to
// check if this happened: "if the same MAPPING_ERROR returns, the walker
// wants a different alias or the TLB is stale", and "No TLB invalidate is
// issued, so a stale negative translation is a second candidate". The
// MAPPING_ERROR did return. This reads the registers that say which.
//
// Offsets from docs/linux-ref/gc_10_3_0_offset.h, each one checked in the
// header rather than extrapolated from a stride, and every BASE_IDX is 0 -
// the same segment as CP_RB0_CNTL, whose resolution here is proven.
//
//   CNTL                  0x15fc + N          (stride 1)
//   PAGE_TABLE_BASE_ADDR  0x1667 + 2N (LO), +1 (HI)
//   PAGE_TABLE_START_ADDR 0x1687 + 2N (LO), +1 (HI)
//   PAGE_TABLE_END_ADDR   0x16a7 + 2N (LO), +1 (HI)
//
// Read-only, like the rest of this probe. What it can say:
//
//   context 3 BASE = 0        it was never programmed, and a CPF fetch under
//                             VMID 3 cannot translate. That would be the
//                             whole answer.
//   context 3 BASE set, but   the ring at 0xFF_BFDE_0000 is outside the
//     START/END excludes      window the context covers - a range fault,
//     the ring                which is what PERMISSION_FAULTS would mean.
//   context 3 looks right     the table CONTENTS or a stale TLB, and 2em's
//                             second candidate is the one left standing.
//
// Its own note (rbgpu-vm) rather than rbgpu-ring: twenty-one values do not
// fit beside the ring data, and a note that silently truncates is worse than
// one that is explicit (2ho).
#define RB_2HQ_CTX_CNTL(n)  ((0x15FCull + (n)) << 32 | 0xB)
#define RB_2HQ_CTX_BASE(n)  ((0x1667ull + 2 * (n)) << 32 | 0xB)
#define RB_2HQ_CTX_START(n) ((0x1687ull + 2 * (n)) << 32 | 0xB)
#define RB_2HQ_CTX_END(n)   ((0x16A7ull + 2 * (n)) << 32 | 0xB)

static char g_rb_2hq_vm[320];

// ---------------------------------------------------------------------------
// 2ig (2026-10-18): WRITE context 3's page-directory base. The first write
// this project has ever made to a GCVM context register.
//
// Everything that led here is a measurement, not an argument. Nothing writes
// BASE, START or END for contexts 1..15 - not X6000's accessor (2ic/2id:
// 0 of ~35,000), not HWLibs (2ie: context 0 only), not RB0 (2ib: nowd), and
// Apple's own flush ran 336 times and invalidated without programming (2id).
// getPDEValue produces page-table CONTENT entries with a zero address field
// and no base for any context (2if). Yet VMID 3 is bound, successfully, 41
// assignments deep (2if), and the chip faults on a CPF read at
// 0x4_0030_0000 under exactly that VMID, in ten consecutive boots.
//
// So the register holds a leftover, and the question is whether giving it a
// real root directory stops the fault.
//
// THE STRICT MATCH. The value is not guessed and not computed here. Apple's
// own allocator hands out the root directories - allocVMBlock(level 0),
// three of them, one per VM context - and 2ig records each one's addresses
// from elem+0x20 and elem+0x28, the fields allocVMBlock itself tests. The
// context object bound to VMID 3 is then scanned for a qword EQUAL to one of
// those recorded roots. A match means the object genuinely holds that root,
// at an offset Apple chose, and that is the value written. No match means
// the probe REFUSES and only dumps, which is why this boot is worth having
// either way - the dump is what the next one would need.
//
// WHAT IT DOES TO THE CHIP. Two register writes at the +100 s sample, after
// the hang is already established (the +12 s sample has always shown it):
//
//     GCVM_CONTEXT3_PAGE_TABLE_BASE_ADDR_LO32  (dword 0x28CD) = root | 1
//     GCVM_CONTEXT3_PAGE_TABLE_BASE_ADDR_HI32  (dword 0x28CE) = root >> 32
//
// Bit 0 is AMDGPU_PTE_VALID. Linux ORs it unconditionally (amdgpu_gmc_pd_addr,
// Vega10+), Apple's getPDEValue ends `incq %rax`, and 2if measured nv=0 over
// 114 calls - so setting it follows both conventions rather than inventing
// one. Nothing else is touched. No invalidation is issued, no other context
// is written, and the original values are read back and recorded first so
// the change is reversible in evidence even though it is not undone.
//
// It cannot un-hang this boot: by +100 s the CP has been halted for ninety
// seconds and the work that owed the timestamp was discarded (2hp). What it
// can show is whether the write STICKS, whether the CPF fault stops
// recurring, and whether CP_ME_CNTL clears - all of which the +160 s sample
// reads with the same instruments that read the +12 s one. If those say yes,
// the next boot does this at bind time, where it can prevent the hang.
//
// Gated on its own boot-arg, -rbgpuptwrite, so no ordinary -rbgpuringprobe
// boot ever writes a register by accident, and so it can be removed with
// `efi.sh bootarg remove` without a rebuild.
// ---------------------------------------------------------------------------

#define RB_2IG_CTX3_LO 0x166Dull        // 0x1667 + 2*3, raw; seg0 makes 0x28CD
#define RB_2IG_CTX3_HI 0x166Eull
#define RB_2IG_SCAN    0x300u           // bytes of the context object to scan

static int g_rb_2ig_done = 0;

static void rbgpu_ptwrite_2ig(void *obj,
                              uint32_t (*resolve)(void *, uint64_t),
                              uint32_t (*rd)(void *, uint32_t),
                              void (*wr)(void *, uint32_t, uint32_t),
                              unsigned phase) {
    if (phase != 1 || g_rb_2ig_done) {
        return;
    }
    g_rb_2ig_done = 1;

    int armed = checkKernelArgument("-rbgpuptwrite") ? 1 : 0;
    void *ctx = g_rb_2ig_ctx[3];

    char note[352];
    size_t n = 0;
    int w = snprintf(note, sizeof(note), "arm=%d ctx3=%llx nroot=%u",
                     armed, reinterpret_cast<uint64_t>(ctx), g_rb_2ig_nroot);
    n = (w > 0) ? static_cast<size_t>(w) : 0;

    // The roots, so a refusal still says what was being looked for.
    for (unsigned i = 0; i < g_rb_2ig_nroot && i < 3; i++) {
        if (n >= sizeof(note) - 48) break;
        w = snprintf(note + n, sizeof(note) - n, " r%u=%llx/%llx", i,
                     g_rb_2ig_root[i].a20, g_rb_2ig_root[i].a28);
        if (w > 0) { n += static_cast<size_t>(w); if (n >= sizeof(note)) { n = sizeof(note) - 1; break; } }
    }

    if (!ctx || g_rb_2ig_nroot == 0) {
        snprintf(note + n, sizeof(note) - n,
                 " REFUSED no %s", ctx ? "root" : "ctx3");
        rbgpu_nvram_note("rbgpu-ptw", note);
        rbgpu_log("2ig: REFUSED - %s. Nothing was written. This is not "
                  "evidence about the register; it is the probe saying it "
                  "could not identify a value it was willing to write.",
                  ctx ? "allocVMBlock recorded no level-0 root"
                      : "assignVMID never bound a context to VMID 3");
        return;
    }

    // The strict match: a qword in the context object equal to a root.
    auto q = reinterpret_cast<volatile uint64_t *>(ctx);
    uint64_t root = 0;
    unsigned moff = 0;
    int found = 0;
    for (unsigned o = 0; o < RB_2IG_SCAN / 8 && !found; o++) {
        uint64_t v = q[o];
        if (v == 0) continue;
        for (unsigned i = 0; i < g_rb_2ig_nroot; i++) {
            if (v == g_rb_2ig_root[i].a20 || v == g_rb_2ig_root[i].a28) {
                root = v; moff = o * 8; found = 1; break;
            }
        }
    }

    // The dump goes to the log either way; the log is captured in-boot.
    rbgpu_log("2ig: ctx3=%p first qwords: %llx %llx %llx %llx %llx %llx "
              "%llx %llx %llx %llx %llx %llx",
              ctx, q[0], q[1], q[2], q[3], q[4], q[5],
              q[6], q[7], q[8], q[9], q[10], q[11]);

    if (!found) {
        if (n < sizeof(note) - 24) {
            snprintf(note + n, sizeof(note) - n, " REFUSED nomatch");
        }
        rbgpu_nvram_note("rbgpu-ptw", note);
        rbgpu_log("2ig: REFUSED - no qword in the first 0x%x bytes of the "
                  "VMID-3 context equals any recorded level-0 root. Nothing "
                  "was written. The dump above is what a value-finding pass "
                  "would start from; it does NOT mean the context holds no "
                  "root, only that none is in the scanned window in a form "
                  "that matches allocVMBlock's own fields.", RB_2IG_SCAN);
        return;
    }

    uint32_t off_lo = resolve(obj, (RB_2IG_CTX3_LO << 32) | 0xB);
    uint32_t off_hi = resolve(obj, (RB_2IG_CTX3_HI << 32) | 0xB);
    uint32_t was_lo = rd(obj, off_lo);
    uint32_t was_hi = rd(obj, off_hi);
    uint32_t new_lo = static_cast<uint32_t>(root & 0xFFFFFFFFu) | 1u;
    uint32_t new_hi = static_cast<uint32_t>(root >> 32);

    if (n < sizeof(note) - 96) {
        w = snprintf(note + n, sizeof(note) - n,
                     " m@%x=%llx o=%x/%x was=%x/%x new=%x/%x",
                     moff, root, off_lo, off_hi, was_lo, was_hi,
                     new_lo, new_hi);
        if (w > 0) { n += static_cast<size_t>(w); if (n >= sizeof(note)) n = sizeof(note) - 1; }
    }

    if (!armed) {
        if (n < sizeof(note) - 12) snprintf(note + n, sizeof(note) - n, " OFF");
        rbgpu_nvram_note("rbgpu-ptw", note);
        rbgpu_log("2ig: -rbgpuptwrite is NOT set. A value was identified and "
                  "NOTHING was written. The note records what would have been "
                  "written, which is the whole dry run.");
        return;
    }

    // Commit the pre-write note FIRST, so a hang inside the write still
    // leaves the before-state and the intended value on record.
    if (n < sizeof(note) - 12) snprintf(note + n, sizeof(note) - n, " GO");
    rbgpu_nvram_note("rbgpu-ptw", note);

    wr(obj, off_lo, new_lo);
    wr(obj, off_hi, new_hi);
    uint32_t back_lo = rd(obj, off_lo);
    uint32_t back_hi = rd(obj, off_hi);

    if (n < sizeof(note) - 32) {
        snprintf(note + n, sizeof(note) - n, " back=%x/%x", back_lo, back_hi);
    }
    rbgpu_nvram_note("rbgpu-ptw", note);
    rbgpu_log("2ig: WROTE CONTEXT3 base lo=0x%08x hi=0x%08x (was 0x%08x/"
              "0x%08x), read back 0x%08x/0x%08x. back != new means the "
              "register did not take the value, which is itself the answer "
              "to 2hr's first caveat. The +160 s sample reads the fault "
              "status, CP_ME_CNTL and all three contexts with the same "
              "instruments that read +12 s.",
              new_lo, new_hi, was_lo, was_hi, back_lo, back_hi);
}

// ---------------------------------------------------------------------------
// 2ik (2026-10-18): read the aperture map before guessing a transform again.
//
// Three writes have now been attempted at this register and each assumed a
// transform: 2ih the raw MC address, 2ii mc2pa with vram_base_offset, 2ij
// the same with a gate that turned out to permit it. The gate was correct
// and the geometry is still impossible - a 4 GB FB aperture over 2 GB of
// backing, with the roots in the overhang.
//
// What has never been read is where the OTHER apertures are. Offsets from
// gc_10_3_0_offset.h, GC seg0 0x1260 applied by socRegOffset:
//
//   0x16FC  GCMC_VM_FB_LOCATION_BASE        << 24
//   0x16FD  GCMC_VM_FB_LOCATION_TOP         << 24 | 0xFFFFFF
//   0x16FE  GCMC_VM_AGP_TOP                 << 24 | 0xFFFFFF
//   0x16FF  GCMC_VM_AGP_BOT                 << 24
//   0x1700  GCMC_VM_AGP_BASE
//   0x1701  GCMC_VM_SYSTEM_APERTURE_LOW_ADDR   << 18
//   0x1702  GCMC_VM_SYSTEM_APERTURE_HIGH_ADDR  << 18
//   0x16E8/0x16E9 GCMC_VM_SYSTEM_APERTURE_DEFAULT_ADDR LSB/MSB
//
// In Linux, an MC address inside [AGP_BOT, AGP_TOP] is SYSTEM memory reached
// without mc2pa, and the system aperture bounds say which MC addresses the
// hub resolves to host DRAM directly. If 0xF4_FB01_5000 falls in the AGP
// window, the register wants its bus address and every transform tried so
// far was the wrong family of answer - which would explain all three boots
// at once.
//
// Observe only. The write above is held back this boot.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// 2il (2026-10-18): let the PAGE decide which address is the page directory.
//
// Three transforms have been written and each was a guess dressed as an
// argument. 2ik's aperture map removes one family of answer - AGP is
// DISABLED (AGP_BOT 0xFFFFFF<<24 sits above AGP_TOP 0, which is how
// amdgpu_gmc_agp_location switches the window off), and the system aperture
// equals the FB aperture (sysl<<18 = 0xF4_0000_0000, sysh<<18 =
// 0xF4_FFFC_0000, programmed from vram_start/vram_end). So the roots are in
// the VRAM range and mc2pa is the right FAMILY. The contradiction is back
// on the arithmetic:
//
//   mc2pa(0xF4_FB01_5000) = 0xFB01_5000 + 0x7_8000_0000 = ~33.9 GB   no RAM
//   Apple's own value      = 0xFB01_5000                = 3.92 GB    real
//
// And that is the experiment never run. 2hr's original proposal - leave
// Apple's value alone and set ONLY bit 0 - was never tested, because 2ig
// reframed the problem as truncation and the next three boots chased the
// reframing. The MC address was tested, mc2pa was tested, the obvious one
// was not.
//
// So stop choosing. Map each candidate page and ask which one LOOKS like a
// page directory. This is 2ee's method, which already distinguished live
// kernel memory from a real table for context 0, applied to four addresses:
//
//   a  fb_off                 0x0_FB01_5000   Apple's own value
//   b  fb_off + FB_OFFSET<<24 0x8_7B01_5000   mc2pa (2ii wrote this)
//   c  the raw MC address     0xF4_FB01_5000  (2ih wrote this)
//   d  fb_off + BAR0 base     0x9_7B01_5000   the aperture alias 2em named
//                                             as its residual risk
//
// THE DISCRIMINATOR, written down before the boot. A depth-1 page directory
// for a sparse VM is mostly zero with a few valid entries, and every
// nonzero entry has bit 0 set. Noise does not look like that - 2ee measured
// 251 of 512 "valid" bits on live kernel memory, a coin flip. So per
// candidate: nz = nonzero qwords, v = qwords with bit 0 set. A page
// qualifies only if nz > 0 AND v == nz AND nz <= 64. All-ones (unmapped)
// gives nz = 512, v = 512 and fails the nz cap. Live code gives v < nz.
//
// The write happens only if EXACTLY ONE candidate qualifies. Two qualifying
// is ambiguous and is refused; zero means none of these is the directory
// and the counts say what each page looked like, which is the next lead.
// ---------------------------------------------------------------------------

struct rb_2il_cand { uint64_t pa; uint32_t nz, v; uint64_t first; uint8_t mapped; };

// 2ip: one note for all three contexts. Each append replaces the whole
// variable, so a hang partway still leaves every context handled so far on
// record rather than losing the lot.
static char g_rb_2ip_note[768];
static size_t g_rb_2ip_mark = 0;    // where the current context's entry starts

// 2iq: REPLACE the current context's entry rather than appending a second
// copy. 2ip called this once before the write and once after, so every
// context appeared twice and the buffer filled before v3 could land -
// context 2 was never visible. `commit` closes the entry and moves the mark.
static void rbgpu_2ip_append(const char *one, bool commit) {
    size_t have = g_rb_2ip_mark;
    if (have > sizeof(g_rb_2ip_note) - 80) return;
    snprintf(g_rb_2ip_note + have, sizeof(g_rb_2ip_note) - have,
             "%s%s", have ? " || " : "", one);
    if (commit) g_rb_2ip_mark = strlen(g_rb_2ip_note);
    rbgpu_nvram_note("rbgpu-pdc", g_rb_2ip_note);
}

__attribute__((noinline))
static void rbgpu_walk_2im(uint64_t pd_pa, uint64_t vbo) {
    // 2im (2026-10-18): the walk. The directory has ONE valid entry and 2il
    // recorded its VALUE but not its INDEX, which is what says whether it
    // covers the faulting VA at all. Dump the nonzero (index, value) pairs,
    // then follow the first one to its page table and dump that too. The
    // geometry (depth 1, PAGE_TABLE_BLOCK_SIZE from cn=0x3b) is then derived
    // from the data rather than computed from a field decode I have not
    // verified on this chip.
    {
        IOMemoryDescriptor *md = IOMemoryDescriptor::withPhysicalAddress(
            static_cast<IOPhysicalAddress>(pd_pa), 0x1000, kIODirectionIn);
        IOMemoryMap *mp = md ? md->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
        if (mp) {
            auto q = reinterpret_cast<volatile uint64_t *>(mp->getVirtualAddress());
            char w2[352];
            size_t m = 0;
            int x = snprintf(w2, sizeof(w2), "pd=%llx", pd_pa);
            m = (x > 0) ? static_cast<size_t>(x) : 0;
            uint64_t pte_pa = 0;
            unsigned shown = 0;
            for (unsigned k = 0; k < 512 && shown < 6; k++) {
                if (q[k] == 0) continue;
                if (pte_pa == 0) pte_pa = (q[k] & 0x0000FFFFFFFFF000ull) + vbo;
                if (m < sizeof(w2) - 40) {
                    x = snprintf(w2 + m, sizeof(w2) - m, " [%u]=%llx", k, q[k]);
                    if (x > 0) { m += static_cast<size_t>(x); if (m >= sizeof(w2)) { m = sizeof(w2) - 1; break; } }
                }
                shown++;
            }
            mp->release();
            if (md) md->release();

            // Follow it. The PTE page's address goes through the same mc2pa
            // that 2il just PROVED for the directory, so this is the first
            // step of this walk that is not a guess.
            if (pte_pa) {
                IOMemoryDescriptor *m2 = IOMemoryDescriptor::withPhysicalAddress(
                    static_cast<IOPhysicalAddress>(pte_pa), 0x1000, kIODirectionIn);
                IOMemoryMap *p2 = m2 ? m2->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
                if (p2) {
                    auto t = reinterpret_cast<volatile uint64_t *>(p2->getVirtualAddress());
                    uint32_t nz = 0, v = 0; uint64_t f1 = 0; unsigned i1 = 0;
                    for (unsigned k = 0; k < 512; k++) {
                        if (t[k] == 0) continue;
                        if (nz == 0) { f1 = t[k]; i1 = k; }
                        nz++; if (t[k] & 1) v++;
                    }
                    if (m < sizeof(w2) - 64) {
                        x = snprintf(w2 + m, sizeof(w2) - m,
                                     " | pt=%llx nz=%u v=%u [%u]=%llx",
                                     pte_pa, nz, v, i1, f1);
                        if (x > 0) { m += static_cast<size_t>(x); if (m >= sizeof(w2)) m = sizeof(w2) - 1; }
                    }
                    p2->release();
                } else if (m < sizeof(w2) - 24) {
                    snprintf(w2 + m, sizeof(w2) - m, " | pt=%llx UNMAPPED", pte_pa);
                }
                if (m2) m2->release();

                // 2io (2026-10-18): the entry that actually covers the fault.
                //
                // cn=0x3b decodes to depth 1 with PAGE_TABLE_BLOCK_SIZE field
                // 7, i.e. block size 16, so PDE[0] spans 2^16 pages - far more
                // than the 768 needed to reach the faulting VA, and the
                // directory DOES cover 0x4_0030_0000. But a 2^16-entry block
                // is 128 pages of PTEs and 2im read only the first. The
                // faulting VA is page 0x300 = 768 above this context's window
                // start 0x4_0000_0000, so its entry is at byte 768*8 = 0x1800
                // into the block: the SECOND PTE page, index 256.
                //
                // That one entry is the whole question. If it is valid and
                // points somewhere real, translation should succeed and the
                // fault means something other than a missing mapping.
                const uint64_t kFaultPage = 0x300;
                uint64_t e_pa = pte_pa + ((kFaultPage * 8) & ~0xFFFull);
                unsigned e_ix = static_cast<unsigned>(kFaultPage % 512);
                IOMemoryDescriptor *m3 = IOMemoryDescriptor::withPhysicalAddress(
                    static_cast<IOPhysicalAddress>(e_pa), 0x1000, kIODirectionIn);
                IOMemoryMap *p3 = m3 ? m3->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
                if (p3) {
                    auto u = reinterpret_cast<volatile uint64_t *>(p3->getVirtualAddress());
                    uint64_t ent = u[e_ix];
                    if (m < sizeof(w2) - 56) {
                        snprintf(w2 + m, sizeof(w2) - m, " | F pa=%llx[%u]=%llx %s",
                                 e_pa, e_ix, ent,
                                 (ent == 0) ? "ZERO" : ((ent & 1) ? "VALID" : "NOTVALID"));
                    }
                    p3->release();
                } else if (m < sizeof(w2) - 24) {
                    snprintf(w2 + m, sizeof(w2) - m, " | F pa=%llx UNMAPPED", e_pa);
                }
                if (m3) m3->release();
            }
            rbgpu_nvram_note("rbgpu-walk", w2);
            rbgpu_log("2im: %s", w2);
            rbgpu_log("2im: the faulting VA is 0x4_0030_0000, which is page "
                      "0x300 above this context's window start 0x4_0000_0000. "
                      "If the only valid PDE's index cannot reach page 0x300, "
                      "the directory does not map the address the CPF reads "
                      "and the base being right changes nothing - which would "
                      "move the question from the base to who populates the "
                      "table.");
        } else if (md) {
            md->release();
        }
    }

}



static int g_rb_2il_done = 0;
uint32_t g_rb_2im_f_at_write = 0xFFFFFFFFu;   // 2im: fault count when 2il wrote
static uint32_t g_rb_2io_st_after_clear = 0xFFFFFFFFu;  // 2io: status right after the clear

__attribute__((noinline))
static void rbgpu_pdcandidates_2il(void *obj,
                                   uint32_t (*resolve)(void *, uint64_t),
                                   uint32_t (*rd)(void *, uint32_t),
                                   void (*wr)(void *, uint32_t, uint32_t),
                                   unsigned phase) {
    if (phase != 0 || g_rb_2il_done) {
        return;
    }
    g_rb_2il_done = 1;

    // 2ip (2026-10-18): every bound VMID, not just the one that faulted.
    //
    // 2io is the first boot in seventeen where the fault signature changed:
    // s3009bb - CPF reading 0x4_0030_0000 under VMID 3 - is GONE, and one
    // poll found the chip clean (c=8 f=7). It was not the clear masking it:
    // the seven polls after the clear still reported the phantom and
    // s40953, so the path was live and a VMID-3 fault would have re-latched.
    // **The base correction works.** It worked in 2il and 2im too and was
    // invisible because the latched status kept being re-read, which is
    // exactly what fw= could not escape.
    //
    // But 2il only ever corrected CONTEXT 3, because that is where the fault
    // happened to be. rbgpu-vmid reports vmids=000e - VMIDs 1, 2 AND 3 are
    // all bound to real contexts - and rbgpu-vm shows c1 still holding
    // Apple's broken 0xFB00_B000. Two of the three bound contexts still
    // point their walker at unmapped memory.
    //
    // So do all of them. Each gets its OWN candidate check - its own
    // register read, its own four pages mapped, its own single-qualifier
    // rule - because "the transform that worked for context 3" is a reason
    // to test the others the same way, not a reason to skip testing them.
    for (unsigned vm = 1; vm <= 3; vm++) {
    uint32_t off_lo = resolve(obj, ((0x1667ull + 2ull * vm) << 32) | 0xB);
    uint32_t off_hi = resolve(obj, ((0x1668ull + 2ull * vm) << 32) | 0xB);
    uint32_t cur_lo = rd(obj, off_lo);
    uint32_t cur_hi = rd(obj, off_hi);
    uint64_t fb_off = ((static_cast<uint64_t>(cur_hi) << 32) | cur_lo) & ~0xFFFULL;

    if (fb_off == 0 || g_rb_2ed_fb_off == 0) {
        rbgpu_log("2ip: context %u base reads 0 or FB_OFFSET unknown - no "
                  "candidate could be formed, nothing written for it", vm);
        continue;
    }
    uint64_t vbo = static_cast<uint64_t>(g_rb_2ed_fb_off) << 24;
    uint64_t fbb = static_cast<uint64_t>(g_rb_2ed_fb_base) << 24;

    struct rb_2il_cand c[4];
    c[0].pa = fb_off;                 // Apple's own value
    c[1].pa = fb_off + vbo;           // mc2pa
    c[2].pa = fb_off + fbb;           // the raw MC address
    c[3].pa = fb_off + 0x880000000ull; // the BAR0 alias 2em flagged

    for (unsigned i = 0; i < 4; i++) {
        c[i].nz = 0; c[i].v = 0; c[i].first = 0; c[i].mapped = 0;
        IOMemoryDescriptor *md = IOMemoryDescriptor::withPhysicalAddress(
            static_cast<IOPhysicalAddress>(c[i].pa), 0x1000, kIODirectionIn);
        if (!md) continue;
        IOMemoryMap *mp = md->map(kIOMapAnywhere | kIOMapInhibitCache);
        if (!mp) { md->release(); continue; }
        auto q = reinterpret_cast<volatile uint64_t *>(mp->getVirtualAddress());
        c[i].mapped = 1;
        for (unsigned k = 0; k < 512; k++) {
            uint64_t e = q[k];
            if (e == 0) continue;
            if (c[i].nz == 0) c[i].first = e;
            c[i].nz++;
            if (e & 1) c[i].v++;
        }
        mp->release();
        md->release();
    }

    char note[352];
    size_t n = 0;
    int w = snprintf(note, sizeof(note), "v%u fbo=%llx", vm, fb_off);
    n = (w > 0) ? static_cast<size_t>(w) : 0;
    int qual = -1, nqual = 0;
    for (unsigned i = 0; i < 4; i++) {
        int ok = c[i].mapped && c[i].nz > 0 && c[i].v == c[i].nz && c[i].nz <= 64;
        if (ok) { qual = static_cast<int>(i); nqual++; }
        if (n >= sizeof(note) - 56) continue;
        w = snprintf(note + n, sizeof(note) - n, " | %c pa=%llx m=%u nz=%u v=%u f=%llx%s",
                     'a' + i, c[i].pa, c[i].mapped, c[i].nz, c[i].v,
                     c[i].first, ok ? " OK" : "");
        if (w > 0) { n += static_cast<size_t>(w); if (n >= sizeof(note)) { n = sizeof(note) - 1; break; } }
    }

    if (nqual != 1) {
        if (n < sizeof(note) - 16) {
            snprintf(note + n, sizeof(note) - n, " | nq=%d NOWRITE", nqual);
        }
        rbgpu_2ip_append(note, true);
        rbgpu_log("2il: %s", note);
        rbgpu_log("2il: %d candidates qualified, so NOTHING was written. "
                  "Zero means none of these four is context 3's page "
                  "directory and the counts above are the next lead; more "
                  "than one means the discriminator cannot separate them and "
                  "choosing would be a guess.", nqual);
        continue;
    }

    rbgpu_walk_2im(c[qual].pa, vbo);

    uint64_t winner = c[qual].pa;
    uint32_t new_lo = static_cast<uint32_t>(winner & 0xFFFFFFFFu) | 1u;
    uint32_t new_hi = static_cast<uint32_t>(winner >> 32);
    if (n < sizeof(note) - 40) {
        snprintf(note + n, sizeof(note) - n, " | W %c %x/%x", 'a' + qual,
                 new_lo, new_hi);
        n = strlen(note);
    }
    if (g_rb_2im_f_at_write == 0xFFFFFFFFu) {
        g_rb_2im_f_at_write = g_rb_2hs_faults;   // 2im: countable post-write
    }
    rbgpu_2ip_append(note, false);           // commit BEFORE writing

    wr(obj, off_lo, new_lo);
    wr(obj, off_hi, new_hi);
    uint32_t b_lo = rd(obj, off_lo), b_hi = rd(obj, off_hi);
    if (n < sizeof(note) - 24) {
        snprintf(note + n, sizeof(note) - n, " bk=%x/%x clr=%x", b_lo, b_hi,
                 g_rb_2io_st_after_clear);
    }
    rbgpu_2ip_append(note, true);
    // 2io: CLEAR the fault status after correcting the base.
    //
    // fw=8 last boot looked like "every fault happened after the write", but
    // the status register LATCHES, so a fault from +5 s read at +20 s counts
    // as post-write too. fw cannot distinguish a new fault from an old latch,
    // and reading it as if it could would be the recurring error.
    //
    // Linux clears it the same way after each fault
    // (gmc_v10_0_process_interrupt: WREG32_P(vm_l2_pro_fault_cntl, 1, ~1)),
    // so setting bit 0 of GCVM_L2_PROTECTION_FAULT_CNTL is Apple's and
    // Linux's own mechanism, not an invention. After this, anything the
    // +100 s sample reads is a fault raised AFTER the base was corrected.
    {
        uint32_t coff = resolve(obj, (0x15C4ull << 32) | 0xB);
        uint32_t cval = rd(obj, coff);
        wr(obj, coff, cval | 1u);
        uint32_t soff = resolve(obj, (0x15C8ull << 32) | 0xB);
        g_rb_2io_st_after_clear = rd(obj, soff);
        rbgpu_log("2io: cleared GCVM_L2_PROTECTION_FAULT_CNTL (0x%08x -> "
                  "0x%08x); status immediately after reads 0x%08x. The +100 s "
                  "sample now answers a question fw= could not: whether the "
                  "VMID-3 fault RE-ARMS with a correct page-directory base.",
                  cval, cval | 1u, g_rb_2io_st_after_clear);
    }

    rbgpu_log("2il: candidate %c at 0x%llx is the only page shaped like a "
              "page directory (nz=%u, all valid). Wrote 0x%08x/0x%08x, read "
              "back 0x%08x/0x%08x. This is +12 s, before the Metal client, "
              "so unlike 2ig it is early enough to matter.",
              'a' + qual, winner, c[qual].nz, new_lo, new_hi, b_lo, b_hi);
    }   // 2ip: next bound VMID
}

__attribute__((noinline))
static void rbgpu_aperture_probe_2ik(void *obj,
                                     uint32_t (*resolve)(void *, uint64_t),
                                     uint32_t (*rd)(void *, uint32_t),
                                     unsigned phase) {
    if (phase != 0) {
        return;
    }
    static const struct { uint64_t dw; const char *n; } kR[] = {
        { 0x16FC, "fbb" }, { 0x16FD, "fbt" },
        { 0x16FE, "agpt" }, { 0x16FF, "agpb" }, { 0x1700, "agpbase" },
        { 0x1701, "sysl" }, { 0x1702, "sysh" },
        { 0x16E8, "dfl" },  { 0x16E9, "dfm" },
    };
    char note[352];
    size_t n = 0;
    for (unsigned i = 0; i < sizeof(kR) / sizeof(kR[0]); i++) {
        uint32_t v = rd(obj, resolve(obj, (kR[i].dw << 32) | 0xB));
        if (n >= sizeof(note) - 24) break;
        int w = snprintf(note + n, sizeof(note) - n, "%s%s=%x",
                         n ? " " : "", kR[i].n, v);
        if (w > 0) { n += static_cast<size_t>(w); if (n >= sizeof(note)) { n = sizeof(note) - 1; break; } }
    }
    rbgpu_nvram_note("rbgpu-aper", note);
    rbgpu_log("2ik: %s", note);
    rbgpu_log("2ik: the roots are MC 0xF4_FB00_0000 / 0xF4_FB00_B000 / "
              "0xF4_FB01_5000. If agpb..agpt covers them they are SYSTEM "
              "memory reached without mc2pa and every transform tried so far "
              "is the wrong family of answer. If only fbb..fbt covers them, "
              "the FB aperture really is 4 GB over 2 GB of backing and the "
              "question becomes what vram_base_offset should be on an APU.");
}

static char g_rb_2is_note[352];   // 2is: the two read paths, side by side
static char g_rb_2iu_note[512];   // 2iu: power, reset and the write-readback
static char g_rb_2ix_note[256];   // 2ix: the RLC bootload latch

static void rbgpu_vmctx_probe_2hq(void *obj,
                                  uint32_t (*resolve)(void *, uint64_t),
                                  uint32_t (*rd)(void *, uint32_t),
                                  unsigned phase) {
    // Phase 0 gets the full dump; phase 1 gets a digest, so the note stays
    // inside its budget while still showing whether anything was reprogrammed
    // between bring-up and the hang. Phase 2 adds nothing: by then the reset
    // has run and the contexts are whatever it left.
    if (phase > 1) {
        return;
    }
    static const unsigned kCtx[] = { 0, 1, 3 };   // VMID 0, the 1..15 template, and the VMID 3 that faulted

    char buf[288];
    size_t n = 0;
    int w = snprintf(buf, sizeof(buf), "%sp=%u", g_rb_2hq_vm[0] ? " | " : "", phase);
    n = (w > 0) ? static_cast<size_t>(w) : 0;

    for (unsigned i = 0; i < sizeof(kCtx) / sizeof(kCtx[0]); i++) {
        unsigned c = kCtx[i];
        uint32_t cntl = rd(obj, resolve(obj, RB_2HQ_CTX_CNTL(c)));
        uint64_t base = (static_cast<uint64_t>(rd(obj, resolve(obj, RB_2HQ_CTX_BASE(c) + (1ull << 32)))) << 32)
                      | rd(obj, resolve(obj, RB_2HQ_CTX_BASE(c)));
        uint64_t st = 0, en = 0;
        if (phase == 0) {
            st = (static_cast<uint64_t>(rd(obj, resolve(obj, RB_2HQ_CTX_START(c) + (1ull << 32)))) << 32)
               | rd(obj, resolve(obj, RB_2HQ_CTX_START(c)));
            en = (static_cast<uint64_t>(rd(obj, resolve(obj, RB_2HQ_CTX_END(c) + (1ull << 32)))) << 32)
               | rd(obj, resolve(obj, RB_2HQ_CTX_END(c)));
            rbgpu_log("2hq: ctx%u cntl=0x%08x base=0x%llx start=0x%llx end=0x%llx",
                      c, cntl, base, st, en);
        } else {
            rbgpu_log("2hq: ctx%u cntl=0x%08x base=0x%llx", c, cntl, base);
        }

        if (n >= sizeof(buf) - 72) {
            break;
        }
        if (phase == 0) {
            w = snprintf(buf + n, sizeof(buf) - n, " c%u:cn=%x,b=%llx,s=%llx,e=%llx",
                         c, cntl, base, st, en);
        } else {
            w = snprintf(buf + n, sizeof(buf) - n, " c%u:cn=%x,b=%llx", c, cntl, base);
        }
        if (w > 0) {
            n += static_cast<size_t>(w);
            if (n >= sizeof(buf)) {
                n = sizeof(buf) - 1;
            }
        }
    }

    size_t have = strlen(g_rb_2hq_vm);
    snprintf(g_rb_2hq_vm + have, sizeof(g_rb_2hq_vm) - have, "%s", buf);
    rbgpu_nvram_note("rbgpu-vm", g_rb_2hq_vm);
    rbgpu_log("2hq: %s", buf);
    if (phase == 0) {
        rbgpu_log("2hq: the GFX ring is at VA 0xFF_BFDE_0000 (CP_RB0_BASE << 8, "
                  "2hn). A context whose base is 0, or whose start/end window "
                  "excludes that VA, cannot translate a CPF fetch of it. 2em "
                  "patched CONTEXT0 ONLY - context 3 has never been touched by "
                  "this project.");
    }
}

// 2hm: both samples accumulate here and the WHOLE buffer is written every
// time. 2hl got its pair only because metal_check.sh happened to snapshot
// state.txt between the two writes - p=1 had already overwritten p=0 in
// NVRAM. That was luck, and a measurement whose two halves depend on luck is
// not a measurement. Same shape as rbgpu-prog2, which has carried three
// samples in one variable since 2fy.
static char g_rb_2hm_ring[352];

// ---------------------------------------------------------------------------
// 2hw (2026-10-18): read the ring's PM4 and find what address the CP was
// told to write its timestamp to.
//
// 2hv ran the fault-register line of attack to its limit. `status 0x40953`
// carried VA 0 three times, but Apple polls getVMFaultInfo only eight times
// in two and a half minutes, so the sample can never reach the size 2hu said
// it needed, and "the address is zero" stays indistinguishable from "an
// invalidate cleared the register first" (2hp).
//
// So stop asking the hardware what faulted and ask the command stream what
// the CP was ASKED to do. The end-of-pipe timestamp target is a field in the
// PM4 RELEASE_MEM packet, confirmed against Linux's own emitter
// (gfx_v10_0_ring_emit_fence, docs/linux-ref/gfx_v10_0.c):
//
//     dw0  PACKET3(PACKET3_RELEASE_MEM=0x49, 6)   (3<<30)|(op<<8)|(n<<16)
//     dw1  event/GCR control
//     dw2  DATA_SEL / INT_SEL / DST_SEL
//     dw3  lower_32_bits(addr)      <<<< the timestamp writeback address
//     dw4  upper_32_bits(addr)
//     dw5  lower_32_bits(seq)
//     dw6  upper_32_bits(seq)
//     dw7  ctx_id
//
// Zero in dw3/dw4 establishes 2ht's hypothesis outright; a sane address
// kills it. Either way the fault-register ambiguity stops mattering.
//
// Only ONE page is needed. CP_RB0_CNTL has RB_BUFSZ=16 so the ring is large,
// but rptr (0xb1 -> 0x2b1) and wptr (0x280 -> 0x380) are both dword indices
// inside the first 4 KB (dwords 0..1023). Mapping one page covers the whole
// region between them.
//
// **This is the first time the plugin maps GPU-visible memory, and that is
// the risk.** The route is: ring VA -> index into CONTEXT0's flat PTE table
// (already mapped at g_rb_gart_va by 2ek/2el, Apple's own pointer) -> the
// PTE -> a physical page. Every valid entry in that table is
// AMDGPU_PTE_SYSTEM (2ek: 1512 of 1512), so the address in it is a
// bus/DMA address; on this machine with no IOMMU translation in play that
// equals the CPU physical address, but **it is not guaranteed to**.
//
// That is exactly why the sanity gate is not optional. A wrong mapping
// returns a page of something, and a page of something looks like data.
// Before any dword is interpreted this counts PM4 type-3 headers
// ((dw >> 30) == 3) across the page; a ring buffer is full of them. Below
// the threshold the dump REFUSES and says the mapping is unconvincing -
// a failed mapping is a failed mapping, not a finding (2cy).
//
// Read-only, one page, unmapped immediately. Off unless -rbgpuringdump.
#define RB_2HW_PM4_MIN 8

static int g_rb_2hw_on   = -1;
static int g_rb_2hw_done = 0;

// 2hy (2026-10-18): the fence buffer the RELEASE_MEM packets point at.
//
// 2hx refuted the timestamp-target hypothesis: the address is
// 0xFF_BFDC_0000, not zero, and the CP fetched past all three fence packets
// (seq 3, 4, 5) while the driver waited forever. Two things can still be
// true, and one page separates them:
//
//   the page holds 5         the writes LANDED. waitForTimestamp is polling
//                            somewhere else, and this is a driver-side bug,
//                            not a GPU one.
//   the page holds 0 / junk  the writes FAULTED, and the CPF write fault
//   or PTE[idx] invalid      under VMID 0 is really at this address, with
//                            the register cleared before anyone read it.
//
// The address is NOT hardcoded. It comes from dw3/dw4 of a RELEASE_MEM
// packet decoded out of the ring in the same pass, so if Apple ever aims the
// fence somewhere else this follows it instead of reading a stale constant.
//
// **There is no strong sanity gate here, and that is stated rather than
// papered over.** 2hw could demand 799 PM4 headers because a ring is full of
// them; a fence buffer is a handful of counters and mostly zero, which is
// exactly what a wrong mapping also looks like. So this reports the PTE, the
// physical address and the raw dwords and leaves the verdict to be argued
// from them. The only corroboration available is indirect: this page comes
// from the same flat table as the ring page, whose mapping 2hx verified, so
// both physical addresses are printed for comparison. A reading built on
// this page alone, without the PTE agreeing, is not a finding (2cy).
static uint64_t g_rb_2hy_fence_va = 0;
static uint64_t g_rb_2hy_seq      = 0;

static void rbgpu_fencedump_2hy(uint64_t ring_pa) {
    if (g_rb_2hy_fence_va == 0) {
        rbgpu_nvram_note("rbgpu-fence", "SKIP: no RELEASE_MEM address found in the ring");
        rbgpu_log("2hy: SKIPPED - no fence address was decoded, so nothing was "
                  "read. This says nothing about the fence buffer.");
        return;
    }
    uint64_t fva = g_rb_2hy_fence_va;
    uint64_t start_va = g_rb_2eb_start_pg << 12;
    if (fva < start_va) {
        char b[112];
        snprintf(b, sizeof(b), "SKIP: fence va %llx below aperture %llx", fva, start_va);
        rbgpu_nvram_note("rbgpu-fence", b);
        rbgpu_log("2hy: SKIPPED - %s", b);
        return;
    }
    uint64_t idx = (fva - start_va) >> 12;
    if ((idx + 1) * 8 > g_rb_gart_limit) {
        rbgpu_nvram_note("rbgpu-fence", "SKIP: fence PTE index past the table");
        rbgpu_log("2hy: SKIPPED - PTE[%llu] is past the %llu-byte table",
                  idx, g_rb_gart_limit);
        return;
    }

    uint64_t e  = reinterpret_cast<volatile uint64_t *>(g_rb_gart_va)[idx];
    uint64_t pa = e & 0x0000FFFFFFFFF000ULL;
    uint32_t valid = (e & 1) ? 1 : 0;
    uint32_t sys   = (e >> 1) & 1;

    rbgpu_log("2hy: fence VA 0x%llx -> PTE[%llu] = 0x%llx valid=%u sys=%u "
              "phys=0x%llx (the RING's page was phys 0x%llx from the same "
              "table - compare them)",
              fva, idx, e, valid, sys, pa, ring_pa);

    // An invalid PTE is the whole answer and needs no mapping.
    if (!valid || pa == 0) {
        char b[160];
        snprintf(b, sizeof(b), "fv=%llx idx=%llx pte=%llx INVALID - the fence page "
                               "is UNMAPPED, so the timestamp write could not land",
                 fva, idx, e);
        rbgpu_nvram_note("rbgpu-fence", b);
        rbgpu_log("2hy: PTE[%llu] = 0x%llx is NOT VALID. The page the CP was told "
                  "to write its timestamp to is unmapped, which is a complete "
                  "explanation of the hang and matches the CPF WRITE fault of "
                  "2ht #3 - and means that fault's real address is 0x%llx.",
                  idx, e, fva);
        return;
    }

    IOMemoryDescriptor *md =
        IOMemoryDescriptor::withPhysicalAddress(static_cast<IOPhysicalAddress>(pa),
                                                4096, kIODirectionIn);
    if (!md) {
        rbgpu_nvram_note("rbgpu-fence", "SKIP: withPhysicalAddress NULL");
        rbgpu_log("2hy: SKIPPED - could not describe phys 0x%llx", pa);
        return;
    }
    IOMemoryMap *mp = md->map(kIOMapAnywhere | kIOMapInhibitCache);
    if (!mp) {
        md->release();
        rbgpu_nvram_note("rbgpu-fence", "SKIP: map() NULL");
        rbgpu_log("2hy: SKIPPED - could not map phys 0x%llx", pa);
        return;
    }
    auto dw = reinterpret_cast<volatile uint32_t *>(mp->getVirtualAddress());

    // The fence lives at the START of the page only if the VA is page
    // aligned; it may be anywhere within it. Report the offset so the dump
    // can be read correctly either way.
    uint32_t off_dw = static_cast<uint32_t>((fva & 0xFFF) >> 2);

    char note[352];
    size_t n = 0;
    int w = snprintf(note, sizeof(note),
                     "fv=%llx idx=%llx pte=%llx v=%u s=%u pa=%llx rpa=%llx seq=%llu o=%u d:",
                     fva, idx, e, valid, sys, pa, ring_pa, g_rb_2hy_seq, off_dw);
    n = (w > 0) ? static_cast<size_t>(w) : 0;

    int found = 0, anynz = 0;
    for (unsigned i = 0; i < 12; i++) {
        uint32_t v = dw[off_dw + i];
        if (v) anynz = 1;
        if (g_rb_2hy_seq != 0 && v == static_cast<uint32_t>(g_rb_2hy_seq)) found = 1;
        if (n < sizeof(note) - 12) {
            w = snprintf(note + n, sizeof(note) - n, " %x", v);
            if (w > 0) {
                n += static_cast<size_t>(w);
                if (n >= sizeof(note)) n = sizeof(note) - 1;
            }
        }
    }
    if (n < sizeof(note) - 10) {
        snprintf(note + n, sizeof(note) - n, " f=%d nz=%d", found, anynz);
    }
    rbgpu_nvram_note("rbgpu-fence", note);
    rbgpu_log("2hy: %s", note);
    rbgpu_log("2hy: a dword equal to seq %llu means the GPU's timestamp write "
              "LANDED and waitForTimestamp is reading the wrong place - a "
              "driver-side bug. All zero, with the PTE valid, means the write "
              "never arrived. Neither reading is safe unless the PTE above "
              "agrees, because this page has no PM4-style signature to check "
              "the mapping against (2hw had 799 headers; this has none).",
              g_rb_2hy_seq);

    mp->release();
    md->release();
}

// ---------------------------------------------------------------------------
// 2kc (2026-10-19): read the indirect buffer and look for the halt there.
//
// Boot 78: `ib=4@f40fbe9000`. Four INDIRECT_BUFFER packets, so 2jz's `noME`
// stays weak exactly as pre-registered - the CP is delegated off-page and
// the write to CP_ME_CNTL can be inside an IB.
//
// The address is the useful part. `0xF4_0FBE_9000` is in the **carve-out MC**
// range (`0xF4_0000_0000`), not GPUVM space like the ring at `0xFF_BF..`, so
// it needs no page-table walk: Linux's own `amdgpu_gmc_vram_mc2pa` applies,
//     pa = mc - vram_start + vram_base_offset
// with `vram_start = GCMC_VM_FB_LOCATION_BASE << 24` and
// `vram_base_offset = GCMC_VM_FB_OFFSET << 24`, both already read on this
// chip (2ed/2ee) and both used for the context bases in 2ip.
//
// Every value is derived: the MC address comes from the packet the CP is
// actually executing, and the two bases from the registers. Nothing is
// hardcoded - 2cy.
static uint64_t g_rb_2kc_ib_mc = 0;
static uint32_t g_rb_2kc_ib_sz = 0;
static uint32_t g_rb_2kc_ib_vmid = 0;   // 2ke
static uint32_t g_rb_2kc_ib_ctl  = 0;   // 2ke
static uint64_t g_rb_2kf_ib1_va  = 0;   // 2kf: CP_IB1_BASE at the halt
static unsigned g_rb_2kg_phase   = 9;   // 2kg: which phase wrote rbgpu-pm4

static void rbgpu_ibdump_2kc(void) {
    char note[288];
    if (g_rb_2kc_ib_mc == 0) {
        rbgpu_nvram_note("rbgpu-ib", "SKIP: no INDIRECT_BUFFER seen in the ring page");
        return;
    }
    if (g_rb_2ed_fb_base == 0 || g_rb_2ed_fb_off == 0) {
        snprintf(note, sizeof(note),
                 "SKIP mc=%llx: FB_LOCATION_BASE or FB_OFFSET unknown, no "
                 "conversion possible", g_rb_2kc_ib_mc);
        rbgpu_nvram_note("rbgpu-ib", note);
        return;
    }
    uint64_t vstart = static_cast<uint64_t>(g_rb_2ed_fb_base) << 24;
    uint64_t vbo    = static_cast<uint64_t>(g_rb_2ed_fb_off)  << 24;
    // Refuse rather than convert an address that is not in the carve-out:
    // mc2pa is only valid for VRAM, and a wrong conversion would map some
    // unrelated page and be read as a finding (2cy).
    if (g_rb_2kc_ib_mc < vstart) {
        snprintf(note, sizeof(note),
                 "REFUSED mc=%llx below vram_start=%llx - not a carve-out "
                 "address, mc2pa does not apply", g_rb_2kc_ib_mc, vstart);
        rbgpu_nvram_note("rbgpu-ib", note);
        return;
    }
    uint64_t pa = g_rb_2kc_ib_mc - vstart + vbo;
    auto md = IOMemoryDescriptor::withPhysicalAddress(pa & ~0xFFFULL, 0x1000,
                                                      kIODirectionIn);
    if (!md) {
        snprintf(note, sizeof(note), "SKIP mc=%llx pa=%llx: withPhysicalAddress "
                 "returned NULL", g_rb_2kc_ib_mc, pa);
        rbgpu_nvram_note("rbgpu-ib", note);
        return;
    }
    IOMemoryMap *mp = md->createMappingInTask(kernel_task, 0,
                                              kIOMapAnywhere | kIOMapInhibitCache);
    if (!mp) {
        md->release();
        snprintf(note, sizeof(note), "SKIP mc=%llx pa=%llx: map() returned NULL",
                 g_rb_2kc_ib_mc, pa);
        rbgpu_nvram_note("rbgpu-ib", note);
        return;
    }
    auto dw = reinterpret_cast<volatile uint32_t *>(mp->getVirtualAddress());
    unsigned t3 = 0, nz = 0, nme = 0;
    uint32_t meval = 0, meat = 0;
    for (unsigned i = 0; i < 1024; i++) {
        uint32_t h = dw[i];
        if (h) nz++;
        if ((h >> 30) == 3) t3++;
    }
    // 2kd (2026-10-19): CHAIN-WALK, because 2kc's t3 count was too weak a
    // validity test to carry the conclusion.
    //
    // Boot 80: `sz=952 t3=59 nz=183`. The ring gives t3=799 of 1024, far above
    // the 25% you get from random data, which is why 2hx called that mapping
    // unambiguous. Here 59 type-3 among 183 nonzero is ~32% - essentially the
    // random baseline - and the packet declares 952 dwords while only 183 are
    // nonzero. So 2kc's own table ("t3 high -> it IS a command buffer") was
    // NOT satisfied, and `noME-in-IB` could not be read as a clean negative.
    //
    // A real command buffer chains: each type-3 header's COUNT field gives the
    // body length, so header + 2 + count lands exactly on the next header, all
    // the way to the declared size. Garbage breaks within a packet or two.
    // Walking the chain also reads register writes only where a packet really
    // is, instead of mistaking data for a header - which is what the brute
    // scan above does.
    unsigned chain = 0, walked = 0;
    {
        unsigned i = 0;
        unsigned lim = (g_rb_2kc_ib_sz && g_rb_2kc_ib_sz < 1024)
                           ? g_rb_2kc_ib_sz : 1024u;
        while (i < lim) {
            uint32_t h = dw[i];
            uint32_t type = h >> 30;
            unsigned len;
            if (type == 3)       { len = ((h >> 16) & 0x3FFFu) + 2u; }
            else if (type == 2)  { len = 1u; }        // NOP filler
            else break;                               // not a packet: stop
            if (len == 0 || i + len > lim) break;
            if (type == 3) {
                uint32_t op = (h >> 8) & 0xFFu, reg = 0xFFFFFFFFu, val = 0;
                if      (op == 0x37 && len >= 5) { reg = dw[i + 2];           val = dw[i + 4]; }
                else if (op == 0x68 && len >= 3) { reg = dw[i + 1] + 0x2000u; val = dw[i + 2]; }
                else if (op == 0x79 && len >= 3) { reg = dw[i + 1] + 0xc000u; val = dw[i + 2]; }
                if (reg == 0x0F56u) {
                    nme++;
                    if (nme == 1) { meval = val; meat = i; }
                    rbgpu_log("2kd: chained packet @%03x op=%02x writes "
                              "CP_ME_CNTL = 0x%08x %s", i, op, val,
                              (val & 0x15000000u) ? "<<<< THIS IS THE HALT" : "");
                }
                if (chain < 3) {
                    rbgpu_log("2kd: chain[%u] @%03x hdr=%08x op=%02x len=%u",
                              chain, i, h, op, len);
                }
                chain++;
            }
            i += len;
            walked = i;
        }
    }
    snprintf(note, sizeof(note),
             "mc=%llx pa=%llx sz=%u vmid=%u ctl=%x t3=%u nz=%u chain=%u "
             "walked=%u | %s",
             g_rb_2kc_ib_mc, pa, g_rb_2kc_ib_sz, g_rb_2kc_ib_vmid,
             g_rb_2kc_ib_ctl, t3, nz, chain, walked,
             nme ? "" : "noME-in-IB");
    if (nme) {
        size_t at = strlen(note);
        snprintf(note + at, sizeof(note) - at, "ME@%x=%x n=%u", meat, meval, nme);
    }
    rbgpu_nvram_note("rbgpu-ib", note);
    rbgpu_log("2kc: %s", note);
    rbgpu_log("2kd: `walked` reaching sz is the validity test, not t3. A real "
              "command buffer chains exactly to its declared length; a wrong "
              "page breaks in the first packet or two. walked << sz means the "
              "mapping or the address is wrong and noME-in-IB means NOTHING "
              "(2eh). This is also ONE page and ONE of the IBs.");
    mp->release();
    md->release();
}

static void rbgpu_ringdump_2hw(uint64_t ring_va, uint32_t rptr, uint32_t wptr) {
    if (g_rb_2hw_on < 0) {
        g_rb_2hw_on = checkKernelArgument("-rbgpuringdump") ? 1 : 0;
        rbgpu_log("2hw: ring PM4 dump %s", g_rb_2hw_on ? "ON" : "OFF (-rbgpuringdump)");
    }
    if (!g_rb_2hw_on || g_rb_2hw_done) {
        return;
    }

    // Each refusal names its own check and lands in NVRAM. Silence here must
    // never be mistaken for "the ring looked fine".
    if (!g_rb_gart_va || !g_rb_2eb_start_pg) {
        rbgpu_nvram_note("rbgpu-pm4", "SKIP: no PTE table (gart_va/start unset)");
        rbgpu_log("2hw: SKIPPED - gart_va=0x%llx start_pg=0x%llx; the 2ek/2el "
                  "table is not available, nothing was read",
                  g_rb_gart_va, g_rb_2eb_start_pg);
        return;
    }
    uint64_t start_va = g_rb_2eb_start_pg << 12;
    if (ring_va < start_va) {
        char b[96];
        snprintf(b, sizeof(b), "SKIP: ring va %llx below aperture %llx", ring_va, start_va);
        rbgpu_nvram_note("rbgpu-pm4", b);
        rbgpu_log("2hw: SKIPPED - %s", b);
        return;
    }
    uint64_t idx = (ring_va - start_va) >> 12;
    if ((idx + 1) * 8 > g_rb_gart_limit) {
        rbgpu_nvram_note("rbgpu-pm4", "SKIP: ring PTE index past the table");
        rbgpu_log("2hw: SKIPPED - PTE[%llu] is past the %llu-byte table",
                  idx, g_rb_gart_limit);
        return;
    }

    uint64_t e  = reinterpret_cast<volatile uint64_t *>(g_rb_gart_va)[idx];
    uint64_t pa = e & 0x0000FFFFFFFFF000ULL;
    if (!(e & 1) || pa == 0) {
        char b[96];
        snprintf(b, sizeof(b), "SKIP: ring PTE[%llu]=%llx not valid", idx, e);
        rbgpu_nvram_note("rbgpu-pm4", b);
        rbgpu_log("2hw: SKIPPED - ring PTE[%llu] = 0x%llx, valid=%d. The ring "
                  "page is unmapped, which is itself a finding and a different "
                  "one from a bad dump.", idx, e, (e & 1) ? 1 : 0);
        return;
    }
    g_rb_2hw_done = 1;   // one attempt per boot, whatever happens below

    rbgpu_log("2hw: ring VA 0x%llx -> PTE[%llu] = 0x%llx -> phys 0x%llx "
              "(sys=%llu). Mapping one page, read-only.",
              ring_va, idx, e, pa, (e >> 1) & 1);

    IOMemoryDescriptor *md =
        IOMemoryDescriptor::withPhysicalAddress(static_cast<IOPhysicalAddress>(pa),
                                                4096, kIODirectionIn);
    if (!md) {
        rbgpu_nvram_note("rbgpu-pm4", "SKIP: withPhysicalAddress returned NULL");
        rbgpu_log("2hw: SKIPPED - could not describe phys 0x%llx", pa);
        return;
    }
    IOMemoryMap *mp = md->map(kIOMapAnywhere | kIOMapInhibitCache);
    if (!mp) {
        md->release();
        rbgpu_nvram_note("rbgpu-pm4", "SKIP: map() returned NULL");
        rbgpu_log("2hw: SKIPPED - could not map phys 0x%llx", pa);
        return;
    }
    auto dw = reinterpret_cast<volatile uint32_t *>(mp->getVirtualAddress());

    // THE GATE. Count type-3 headers before believing any of this is a ring.
    uint32_t t3 = 0, nonzero = 0;
    for (unsigned i = 0; i < 1024; i++) {
        uint32_t v = dw[i];
        if (v) nonzero++;
        if ((v >> 30) == 3) t3++;
    }
    if (t3 < RB_2HW_PM4_MIN) {
        char b[160];
        snprintf(b, sizeof(b), "REFUSED pa=%llx t3=%u nz=%u (not PM4)", pa, t3, nonzero);
        rbgpu_nvram_note("rbgpu-pm4", b);
        rbgpu_log("2hw: REFUSING to interpret. phys 0x%llx has only %u type-3 "
                  "headers in 1024 dwords (%u nonzero). A ring buffer is full "
                  "of them, so this mapping is not the ring - most likely the "
                  "PTE holds a bus address that is not the CPU physical "
                  "address. This is a FAILED MAPPING, not a finding about the "
                  "ring.", pa, t3, nonzero);
        mp->release();
        md->release();
        return;
    }

    rbgpu_log("2hw: mapping looks like PM4 (%u type-3 headers, %u nonzero of "
              "1024 dwords). rptr=0x%x wptr=0x%x", t3, nonzero, rptr, wptr);

    // Walk the packets and find every RELEASE_MEM. Its dw3/dw4 is the answer.
    char note[448];
    size_t n = 0;
    int w = snprintf(note, sizeof(note), "ph=%u pa=%llx t3=%u r=%x w=%x",
                     g_rb_2kg_phase, pa, t3, rptr, wptr);
    n = (w > 0) ? static_cast<size_t>(w) : 0;

    // 2ia: the register WRITES in the stream, which is how the page-directory
    // base is programmed at all.
    //
    // 2hr could not find anything in the kext that writes
    // GCVM_CONTEXT*_PAGE_TABLE_BASE_ADDR by MMIO, and 2ia's static search
    // found none either - because there is none. Linux's
    // gmc_v10_0_emit_flush_gpu_tlb programs it with
    // `amdgpu_ring_emit_wreg(ring, hub->ctx0_ptb_addr_lo32 + ctx_addr_distance*vmid, ...)`:
    // **the ring writes it, with a PM4 packet.** Apple has no MMIO path for
    // the same reason.
    //
    // gfx_v10_0_write_data_to_reg gives the layout:
    //     dw0  PACKET3(PACKET3_WRITE_DATA=0x37, 3)
    //     dw1  ENGINE_SEL | DST_SEL(0) | WR_CONFIRM
    //     dw2  reg      (absolute dword offset)
    //     dw3  0
    //     dw4  val
    //
    // The context bases live at seg0 + 0x1667 + 2*vmid = 0x28C7 + 2*vmid,
    // so VMID 0 is 0x28C7 and the VMID 3 that faults is **0x28CD**. Scanning
    // for WRITE_DATA into 0x28C7..0x28E6 shows the value the CP was actually
    // told to put there - including whether bit 0, the VALID flag Apple's own
    // getPDEValue always sets, was present in the packet.
    //
    // Three outcomes, all useful:
    //   a write to 0x28CD with bit 0 SET    the packet was right and the
    //                                       register lost it, or we misread
    //                                       the register in 2hr
    //   a write to 0x28CD with bit 0 CLEAR  the packet itself is wrong, and
    //                                       the bug is upstream in whatever
    //                                       built the value
    //   no write to 0x28CD at all           context 3 was NEVER programmed by
    //                                       this ring; the register holds
    //                                       whatever it held before
    unsigned nwd = 0;
    for (unsigned i = 0; i + 4 < 1024; i++) {
        uint32_t h = dw[i];
        if ((h >> 30) != 3 || ((h >> 8) & 0xFF) != 0x37) {   // PACKET3_WRITE_DATA
            continue;
        }
        uint32_t reg = dw[i + 2];
        if (reg < 0x28C7 || reg > 0x28E6) {
            continue;
        }
        uint32_t val = dw[i + 4];
        unsigned vmid = (reg - 0x28C7) / 2;
        int is_hi = ((reg - 0x28C7) & 1) != 0;
        nwd++;
        rbgpu_log("2ia: WRITE_DATA @%03x reg 0x%x = 0x%08x  -> CONTEXT%u "
                  "PAGE_TABLE_BASE_ADDR_%s%s", i, reg, val, vmid,
                  is_hi ? "HI32" : "LO32",
                  (!is_hi && !(val & 1)) ? "  <<<< VALID BIT NOT SET IN THE PACKET" : "");
        if (n < sizeof(note) - 32) {
            w = snprintf(note + n, sizeof(note) - n, " | wd%x=%x", reg, val);
            if (w > 0) {
                n += static_cast<size_t>(w);
                if (n >= sizeof(note)) n = sizeof(note) - 1;
            }
        }
    }
    if (nwd == 0) {
        rbgpu_log("2ia: NO WRITE_DATA to any GCVM context base register in this "
                  "page. Either the programming happens elsewhere in the ring, "
                  "or context 3 is never programmed by this ring at all - this "
                  "page alone cannot tell those apart.");
        if (n < sizeof(note) - 12) {
            w = snprintf(note + n, sizeof(note) - n, " | nowd");
            if (w > 0) n += static_cast<size_t>(w);
        }
    }

    // 2jz (2026-10-19): does the ring itself write CP_ME_CNTL?
    //
    // Boot 74 settled 2jy's discriminator: `rbb ffbfe000->ffbfe000` and
    // `rbc a00e10->a00e10` ACROSS the halt. The CP block is alive on the
    // register bus and keeps Apple's programmed values, so `0x15000000` is a
    // real value something WROTE - hypothesis (b), a hung block reading back
    // its defaults, is refuted.
    //
    // 2ix counted every host write path (g_hw, Apple's accessor, Apple's own
    // driver code) and every one wrote **0**. So the writer is on the chip,
    // and the CP writing it ITSELF from the command stream fits exactly: the
    // value is real, a write caused it, and no host counter can see it.
    //
    // Three packet forms can reach a GC register, and all three are scanned:
    //   WRITE_DATA      (0x37) dst_sel=0, dw2 = absolute dword offset
    //   SET_CONFIG_REG  (0x68) dw1 = offset - 0x2000 (PACKET3_SET_CONFIG_REG_START)
    //   SET_UCONFIG_REG (0x79) dw1 = offset - 0xc000 (PACKET3_SET_UCONFIG_REG_START)
    // CP_ME_CNTL is dword 0x0F56, which falls in SET_CONFIG_REG's range.
    //
    // Read-only, on a page already mapped and already validated as PM4 by the
    // t3 check above - no new risk over 2hx.
    //
    // NOT a result either way on its own: this is ONE page of the ring
    // (1024 dwords) and the halt could be written from an indirect buffer or
    // from further along. "No packet here" does not mean "no packet" (2eh).
    {
        const uint32_t ME_CNTL_DW = 0x0F56u;
        unsigned nme = 0;
        for (unsigned i = 0; i + 4 < 1024; i++) {
            uint32_t h = dw[i];
            if ((h >> 30) != 3) continue;
            uint32_t op = (h >> 8) & 0xFF;
            uint32_t reg = 0xFFFFFFFFu, val = 0;
            const char *form = nullptr;
            if (op == 0x37) {                       // WRITE_DATA
                reg = dw[i + 2]; val = dw[i + 4]; form = "WRITE_DATA";
            } else if (op == 0x68) {                // SET_CONFIG_REG
                reg = dw[i + 1] + 0x2000u; val = dw[i + 2]; form = "SET_CONFIG_REG";
            } else if (op == 0x79) {                // SET_UCONFIG_REG
                reg = dw[i + 1] + 0xc000u; val = dw[i + 2]; form = "SET_UCONFIG_REG";
            } else {
                continue;
            }
            if (reg != ME_CNTL_DW) continue;
            nme++;
            rbgpu_log("2jz: %s @%03x writes CP_ME_CNTL (dword 0x%x) = 0x%08x "
                      "%s", form, i, reg, val,
                      (val & 0x15000000u) ? "<<<< THIS IS THE HALT" : "");
            if (n < sizeof(note) - 40) {
                w = snprintf(note + n, sizeof(note) - n, " | ME@%x=%x", i, val);
                if (w > 0) {
                    n += static_cast<size_t>(w);
                    if (n >= sizeof(note)) n = sizeof(note) - 1;
                }
            }
        }
        // 2kb (2026-10-19): how much of the stream is NOT in this page?
        //
        // Boot 77 returned `noME`, and 2jz pre-registered that as weak. It is
        // stronger than that allowed for: CP_RB0_BASE reads 0xffbfe000, so
        // the ring base is page-aligned, and wptr = 0x380 = 896 dwords, so
        // the whole submitted stream fits inside the 1024 dwords scanned.
        // The one real gap left is INDIRECT_BUFFER (0x3f), which sends the CP
        // somewhere this page does not cover.
        //
        //   ib=0  -> nothing is delegated, the scan saw the whole stream, and
        //            `noME` becomes strong: the CP does not write its own
        //            halt, leaving the RLC (2jo never excluded it)
        //   ib>0  -> the addresses say where to look next, and `noME` stays
        //            weak exactly as pre-registered
        unsigned nib = 0;
        uint64_t ib0 = 0;
        for (unsigned i = 0; i + 3 < 1024; i++) {
            uint32_t h = dw[i];
            if ((h >> 30) != 3) continue;
            if (((h >> 8) & 0xFF) != 0x3fu) continue;      // INDIRECT_BUFFER
            uint64_t a2 = (static_cast<uint64_t>(dw[i + 2]) << 32) | dw[i + 1];
            if (nib == 0) {
                ib0 = a2;
                g_rb_2kc_ib_mc = a2;                      // 2kc, never hardcoded
                g_rb_2kc_ib_sz = dw[i + 3] & 0xFFFFFu;
                // 2ke: dw3 bits 27:24 are VMID (gfx_v10_0_ring_emit_ib_gfx
                // ORs `vmid << 24` into the control dword). 2kc read only the
                // size field and assumed the address was a carve-out MC
                // address. If VMID != 0 it is that context's GPU VA instead,
                // mc2pa maps an unrelated page, and 2kc's sparse content is
                // explained. The address shape argues MC (0xF40.. matches
                // FB_LOCATION_BASE, and startKIQ's ring MC has the same
                // prefix), but shape is not the field.
                g_rb_2kc_ib_vmid = (dw[i + 3] >> 24) & 0xFu;
                g_rb_2kc_ib_ctl  = dw[i + 3];
            }
            nib++;
            if (nib <= 2) {
                rbgpu_log("2kb: INDIRECT_BUFFER @%03x addr=0x%llx size=%u", i,
                          a2, dw[i + 3] & 0xFFFFFu);
            }
        }
        rbgpu_log("2kb: %u INDIRECT_BUFFER packets in the page. 0 means the "
                  "scan saw the whole stream and `noME` is strong.", nib);
        // 2kg: the raw dwords, because a decode is an interpretation and the
        // reviewer asked for hex. Window 0x90..0xc0 brackets rptr=0xb1, and
        // every IB packet's four dwords go out verbatim so the VMID and
        // length can be read independently of our decode.
        for (unsigned q = 0x90; q < 0xc0; q += 8) {
            rbgpu_log("2kg: RB0[%03x] %08x %08x %08x %08x %08x %08x %08x %08x",
                      q, dw[q], dw[q+1], dw[q+2], dw[q+3],
                      dw[q+4], dw[q+5], dw[q+6], dw[q+7]);
        }
        {
            unsigned seen = 0;
            for (unsigned i = 0; i + 3 < 1024 && seen < 4; i++) {
                uint32_t h = dw[i];
                if ((h >> 30) != 3 || ((h >> 8) & 0xFF) != 0x3fu) continue;
                seen++;
                rbgpu_log("2kg: IB[%u] @%03x raw %08x %08x %08x %08x "
                          "(vmid=%u len=%u)", seen, i, h, dw[i+1], dw[i+2],
                          dw[i+3], (dw[i+3] >> 24) & 0xFu, dw[i+3] & 0xFFFFFu);
            }
        }
        if (n < sizeof(note) - 40) {
            w = snprintf(note + n, sizeof(note) - n, " | ib=%u@%llx", nib, ib0);
            if (w > 0) {
                n += static_cast<size_t>(w);
                if (n >= sizeof(note)) n = sizeof(note) - 1;
            }
        }
        if (nme == 0) {
            rbgpu_log("2jz: no packet in THIS page writes CP_ME_CNTL. Weight "
                      "that against the ib= count above: with ib=0 the page is "
                      "the whole stream.");
            if (n < sizeof(note) - 12) {
                w = snprintf(note + n, sizeof(note) - n, " | noME");
                if (w > 0) n += static_cast<size_t>(w);
            }
        }
    }

    unsigned found = 0;
    for (unsigned i = 0; i < 1024; i++) {
        uint32_t h = dw[i];
        if ((h >> 30) != 3) {
            continue;
        }
        uint32_t op = (h >> 8) & 0xFF;
        if (op != 0x49 || i + 7 >= 1024) {       // PACKET3_RELEASE_MEM
            continue;
        }
        uint64_t addr = (static_cast<uint64_t>(dw[i + 4]) << 32) | dw[i + 3];
        uint64_t seq  = (static_cast<uint64_t>(dw[i + 6]) << 32) | dw[i + 5];
        found++;
        rbgpu_log("2hw: RELEASE_MEM at dword 0x%x: hdr=%08x c1=%08x c2=%08x "
                  "ADDR=0x%llx seq=0x%llx ctx=%08x %s",
                  i, h, dw[i + 1], dw[i + 2], addr, seq, dw[i + 7],
                  addr ? "" : "<<<< THE TIMESTAMP TARGET IS ZERO");
        if (addr != 0) {
            g_rb_2hy_fence_va = addr;      // derived, never hardcoded
            if (seq > g_rb_2hy_seq) g_rb_2hy_seq = seq;
        }
        if (found <= 3 && n < sizeof(note) - 48) {
            w = snprintf(note + n, sizeof(note) - n, " | rm@%x a=%llx s=%llx",
                         i, addr, seq);
            if (w > 0) {
                n += static_cast<size_t>(w);
                if (n >= sizeof(note)) n = sizeof(note) - 1;
            }
        }
    }
    if (!found && n < sizeof(note) - 24) {
        snprintf(note + n, sizeof(note) - n, " | NO RELEASE_MEM in page");
        rbgpu_log("2hw: no RELEASE_MEM packet in the first page. The timestamp "
                  "packet is elsewhere in the ring or inside an indirect "
                  "buffer, so this says NOTHING about its address.");
    }
    rbgpu_nvram_note("rbgpu-pm4", note);
    rbgpu_ibdump_2kc();      // 2kc: the IB the ring just told us about

    // The dwords the CP actually stopped on, logged raw so the decode can be
    // redone offline against nvd.h without another boot.
    unsigned from = (rptr >= 4) ? (rptr - 4) : 0;
    for (unsigned i = from; i < from + 24 && i < 1024; i += 4) {
        rbgpu_log("2hw: ring[%03x] %08x %08x %08x %08x", i,
                  dw[i], dw[i + 1], dw[i + 2], dw[i + 3]);
    }

    mp->release();
    md->release();

    // 2hy: now the fence buffer those packets point at. After the ring page
    // is unmapped and rbgpu-pm4 is committed, so a failure here cannot cost
    // the result that is already in hand.
    rbgpu_fencedump_2hy(pa);
}

// ---------------------------------------------------------------------------
// 2js (2026-10-19): WHEN does the CP get re-halted, and what moves with it?
//
// 2jq/2jr: the autoload now completes, HWLibs writes CP_ME_CNTL = 0 at
// ~341 ms and it TAKES (diag reads 0). By the +12 s ring probe it is back
// to 0x15000000 and writes are refused again. That window has never been
// sampled.
//
// And `rbgpu-me` already rules out the obvious: FOUR writes to CP_ME_CNTL
// were caught this boot - hwl [0@311], x6k [0@12393] [0@43605] [0@92315] -
// and every one wrote **0**. Nothing writes the halt value through Apple's
// accessor or through the gvm path. So the re-halt is either a write on a
// path we do not watch, or the hardware re-asserting it.
//
// Twenty-four samples, 500 ms apart, covering 0.5 s .. 12 s. Each reads
// CP_ME_CNTL and the four things most likely to move with it:
//
//   CP_STAT          is the CP still executing when it halts?
//   RLC_STAT         boot 63/64 show this going 0x25 -> 0 after autoload
//   RLC_RLCS_BOOTLOAD_STATUS   does the latch drop back?
//   GRBM_SOFT_RESET  is the GFX block being reset under us?
//
// The note reports the LAST clean sample and the FIRST halted one, so the
// transition is bracketed to 500 ms, plus the co-values at that instant.
//
// Discipline: NO logging inside the sampler - 2ez turned logging contention
// into a hang that looked like a stall. Plain stores only. And the timers
// are armed ONCE, all of them, from the calling thread (2fy: re-arming from
// inside a handler failed in three consecutive boots). Absence is a result
// here too: if the register never goes non-zero in 24 samples the note says
// so, which would mean the re-halt happens after +12 s and the ring probe's
// p0 catches it rather than this.
#define RB_2JS_N 24u
struct rb_2js_s { uint32_t ms, me, cps, rlcs, bl, srst; };
static struct rb_2js_s g_rb_2js[RB_2JS_N];
static uint32_t        g_rb_2js_got = 0;
static int             g_rb_2js_on  = -1;

void rbgpu_mewatch_sample_2js(unsigned idx) {
    if (g_rb_2js_on < 0) {
        g_rb_2js_on = checkKernelArgument("-rbgpumewatch") ? 1 : 0;
    }
    if (g_rb_2js_on != 1 || idx >= RB_2JS_N || !g_hw.rreg) {
        return;
    }
    struct rb_2js_s *e = &g_rb_2js[idx];
    e->ms   = static_cast<uint32_t>(rbgpu_2iq_ms());
    e->me   = g_hw.rreg(g_hw.ctx, (0x1260u + 0x0F56u) * 4);
    e->cps  = g_hw.rreg(g_hw.ctx, (0x1260u + 0x0F40u) * 4);
    e->rlcs = g_hw.rreg(g_hw.ctx, (0xa000u + 0x4C04u) * 4);
    e->bl   = g_hw.rreg(g_hw.ctx, (0xa000u + 0x4E8Du) * 4);
    e->srst = g_hw.rreg(g_hw.ctx, (0x1260u + 0x0DA8u) * 4);
    if (idx + 1 > g_rb_2js_got) { g_rb_2js_got = idx + 1; }
}

void rbgpu_mewatch_flush_2js(void) {
    if (g_rb_2js_on != 1) {
        rbgpu_nvram_note("rbgpu-mew", "off - -rbgpumewatch not set");
        return;
    }
    // last sample with me == 0, and the first with me != 0
    int last_ok = -1, first_bad = -1;
    for (unsigned i = 0; i < g_rb_2js_got; i++) {
        if (g_rb_2js[i].ms == 0) continue;            // never fired
        if (g_rb_2js[i].me == 0) { last_ok = static_cast<int>(i); }
        else if (first_bad < 0)  { first_bad = static_cast<int>(i); }
    }
    char n[352];
    if (first_bad < 0) {
        snprintf(n, sizeof(n),
                 "n=%u NEVER HALTED in 0.5-12s | last ok i=%d @%ums "
                 "cps=%x rlcs=%x bl=%x",
                 g_rb_2js_got, last_ok,
                 last_ok >= 0 ? g_rb_2js[last_ok].ms : 0,
                 last_ok >= 0 ? g_rb_2js[last_ok].cps : 0,
                 last_ok >= 0 ? g_rb_2js[last_ok].rlcs : 0,
                 last_ok >= 0 ? g_rb_2js[last_ok].bl : 0);
    } else {
        const struct rb_2js_s *a = (last_ok >= 0) ? &g_rb_2js[last_ok] : nullptr;
        const struct rb_2js_s *b = &g_rb_2js[first_bad];
        snprintf(n, sizeof(n),
                 "n=%u | OK i=%d @%ums me=%x cps=%x rlcs=%x bl=%x srst=%x "
                 "|| HALT i=%d @%ums me=%x cps=%x rlcs=%x bl=%x srst=%x",
                 g_rb_2js_got,
                 last_ok, a ? a->ms : 0, a ? a->me : 0, a ? a->cps : 0,
                 a ? a->rlcs : 0, a ? a->bl : 0, a ? a->srst : 0,
                 first_bad, b->ms, b->me, b->cps, b->rlcs, b->bl, b->srst);
    }
    rbgpu_nvram_note("rbgpu-mew", n);
    rbgpu_log("2js: %s", n);
    rbgpu_log("2js: bl dropping from c0000001 at the same sample means the "
              "autoload latch is being cleared and the halt follows it. "
              "srst non-zero means the GFX block is being soft-reset. "
              "cps still busy at the halt means it stopped mid-stream. "
              "NEVER HALTED means the re-halt is later than +12 s.");
}

// ---------------------------------------------------------------------------
// 2jt (2026-10-19): the RLC's own registers across the 500 ms the halt lands in.
//
// 2js bracketed it on boot 65:
//
//   OK   i=15 @8328ms me=0        cps=0        rlcs=25 bl=c0000001 srst=0
//   HALT i=16 @8829ms me=15000000 cps=94038600 rlcs=0  bl=c0000001 srst=0
//
// Three things move together - the CP halts, the CP had just gone BUSY, and
// **RLC_STAT drops 0x25 -> 0**. Two do not: `srst=0` so it is not a soft
// reset, and `bl` stays `c0000001` so the autoload latch is not being
// cleared. Whatever halts the CP, the RLC stops in the same 500 ms.
//
// Which way round that runs is what this measures, at 50 ms instead of
// 500 ms across 7.5 s .. 9.5 s, with the registers that say what the RLC
// was doing:
//
//   RLC_STAT          0x4c04   the transition itself
//   RLC_GPM_STAT      0x4e6e   what the GPM is executing
//   RLC_SAFE_MODE     0x4ca0   2iv found this stuck with CMD set
//   RLC_CGCG_CGLS_CTRL 0x4c49  clock gating - Linux's NON-PSP path writes 0
//   RLC_PG_CNTL       0x4c43   power gating - likewise
//   RLC_SRM_CNTL      0x4c80   the save/restore machine
//
// All seg1. The last two matter because `gfx_v10_0_rlc_resume`'s non-PSP
// branch explicitly disables CG and PG before starting the RLC, and the
// PSP+autoload branch - ours - never does. If CGCG or PG is enabled and the
// RLC gates GFX off under us, that is a one-register fix. If instead the
// GPM stops at a fault, it is not.
//
// Same discipline as 2js: no logging in the sampler, all timers armed once
// from the calling thread (2fy), and an explicit "no transition" note so
// absence reads as a result (2eh). Rides -rbgpumewatch - no new arg.
// 2jx (2026-10-19): widened 40 -> 60 and the window 7.5-9.5 s -> 6.5-9.5 s.
// Boot 70 moved the halt to 7345 ms with -rbgpunosrm and the fixed 7.5 s
// start missed it entirely (NO TRANSITION), so cps/grbm were never captured
// at 50 ms. 6.5-9.5 s covers both the 7.35 s and the 8.84 s halt.
#define RB_2JT_N 60u
// 2jv (2026-10-19) adds `gfxoff`: SMUIO_GFX_MISC_CNTL's PWR_GFXOFF_STATUS,
// 0=GFXOFF 1=transition out 2=NOT in GFXOFF 3=transition into GFXOFF
// (yellow_carp_ppt.c:625-628). Read as a raw dword through the offset
// rb_smu13_init already resolved, so no SMU protocol runs in the sampler.
// 2jw adds `cps` (CP_STAT 0x0F40 seg0) and `grbm` (GRBM_STATUS 0x0DA4 seg0):
// the 500 ms bank shows CP_STAT 0 -> 0x90038600, so the CP went BUSY and then
// halted. Whether a fault state appears BEFORE the halt or only with it is
// what 50 ms resolution can answer and 500 ms cannot.
// 2jy (2026-10-19) adds `rbb` (CP_RB0_BASE, dword 0x1DE0 seg0) and `rbc`
// (CP_RB0_CNTL, 0x1DE1): a discriminator between two hypotheses this project
// has conflated throughout, mine included.
//
// Boot 71 put the whole transition inside one 50 ms sample: CP_STAT
// 0 -> 0x90038600, GRBM_STATUS 0x3028 -> 0xa0003028 (GUI_ACTIVE + CP_BUSY)
// and CP_ME_CNTL -> 0x15000000, all together, from a CP that was COMPLETELY
// idle (cps=0, so even CPF_BUSY clear - the first genuinely clean "before"
// this project has captured).
//
// So either (a) something sets the halt bits on a live CP block, or (b) the
// CP block hangs and the register simply READS BACK its power-on value. (b)
// explains the refused writes and 2jl's CE_STEP without any agent at all,
// and nothing measured so far separates the two.
//
// CP_RB0_BASE separates them. It reads 0xffbfe000 at p0 - a value Apple
// PROGRAMMED, not a default. At the halt:
//   rbb still 0xffbfe000  -> the block answers reads correctly, so
//                            CP_ME_CNTL's value is real and something set it
//   rbb 0 or garbage      -> the whole block reverted or stopped answering,
//                            and the halt bits are a SYMPTOM, not a cause
// 2ke (2026-10-20): the registers that name the packet the CP stopped on.
//
// All offsets from gc_10_3_0_offset.h, all verified present:
//   CP_INT_STATUS_RING0 0x1e0d seg0  PRIV_INSTR(22) PRIV_REG(23)
//                                    OPCODE_ERROR(24) RESERVED_BIT_ERROR(27)
//                                    CP_ECC_ERROR(14)
//   CP_ME_HEADER_DUMP   0x0f41 seg0  last header the ME processed
//   CP_PFP_HEADER_DUMP  0x0f42 seg0  last header the PFP processed
//   CP_IB1_BASE_LO/HI   0x20cc/0x20cd seg1
//   CP_IB1_BUFSZ        0x20ce seg1  dwords REMAINING, so base+(size-bufsz)
//                                    locates the exact packet
//   GCVM_L2_PROTECTION_FAULT_STATUS  already read, but only from +12 s on
//
// CP_ME_HEADER_DUMP sits ONE DWORD from CP_STAT (0x0f40), which this sampler
// has been reading since 2jw. The register that names the packet was next to
// the one we were already looking at.
//
// Why these: an external review's hypothesis is that the CP halts itself on
// something it fetched - in the one transition sample CP_STAT gains
// ROQ_INDIRECT1_BUSY, so new work had arrived and the CP halted while
// fetching it. If a privileged-register write arrives in an unprivileged IB,
// PRIV_REG is exactly what the CP raises. That is testable and we had never
// read the register.
struct rb_2jt_s { uint32_t ms, me, rlcs, gpm, safe, cgcg, pg, srm, gfxoff,
                           cps, grbm, rbb, rbc,
                           cpint, mehdr, pfphdr, ib1lo, ib1sz, vmf; };
static struct rb_2jt_s g_rb_2jt[RB_2JT_N];
static uint32_t        g_rb_2jt_got = 0;

// ---------------------------------------------------------------------------
// 2kf (2026-10-20): the full CP fault sweep, latched at the FIRST halt sample.
//
// A reviewer asked for ~25 registers "from the first halt sample of one boot,
// before any replay". Every name they gave exists in gc_10_3_0_offset.h -
// nothing had to be skipped - and all are listed with their dword and segment
// in the journal under 2kf.
//
// It is NOT added to every sample. 2jv is the reason: ONE extra MMIO read per
// sample, 60 samples, destabilised the boot after ~73 s and cost five
// experiments. So the sweep fires **once**, on the first sample where `me`
// goes nonzero, and a second short sweep fires on the sample after that -
// which is exactly what item 6 wants, instruction pointers from two samples
// to show whether they are parked.
#define RB_2KF_SEG0 0x1260u
#define RB_2KF_SEG1 0xa000u
struct rb_2kf_s {
    uint32_t ms, cpint, cpintc, mehdr, pfphdr, cehdr;
    uint32_t meip, pfpip, ceip;
    uint32_t ib1lo, ib1hi, ib1sz, ib1cmd;
    uint32_t ib2lo, ib2hi, ib2sz, ib2cmd;
    uint32_t ceib1lo, ceib1hi, ceib1sz;
    uint32_t vmf, vmalo, vmahi;
};
static struct rb_2kf_s g_rb_2kf[2];
static int             g_rb_2kf_got = 0;

static void rbgpu_cpsweep_2kf(unsigned which) {
    if (which > 1 || !g_hw.rreg) return;
    struct rb_2kf_s *e = &g_rb_2kf[which];
    auto r0 = [](uint32_t d) { return g_hw.rreg(g_hw.ctx, (RB_2KF_SEG0 + d) * 4); };
    auto r1 = [](uint32_t d) { return g_hw.rreg(g_hw.ctx, (RB_2KF_SEG1 + d) * 4); };
    e->ms     = static_cast<uint32_t>(rbgpu_2iq_ms());
    e->meip   = r0(0x0F46);  e->pfpip = r0(0x0F45);  e->ceip = r0(0x0F47);
    if (which == 1) { return; }   // 2nd sample: pointers only, keep it cheap
    e->cpint  = r0(0x1E0D);  e->cpintc = r0(0x1E0A);
    e->mehdr  = r0(0x0F41);  e->pfphdr = r0(0x0F42);  e->cehdr = r0(0x0F44);
    e->ib1lo  = r1(0x20CC);  e->ib1hi = r1(0x20CD);
    e->ib1sz  = r1(0x20CE);  e->ib1cmd = r1(0x20C0);
    e->ib2lo  = r1(0x20CF);  e->ib2hi = r1(0x20D0);
    e->ib2sz  = r1(0x20D1);  e->ib2cmd = r1(0x20C1);
    e->ceib1lo = r1(0x20C6); e->ceib1hi = r1(0x20C7); e->ceib1sz = r1(0x20C8);
    e->vmf    = r0(0x15C8);  e->vmalo = r0(0x15C9);   e->vmahi = r0(0x15CA);
    // Hand the IB1 base to the deferred dump. Derived from the register the
    // CP itself is using, never hardcoded (2cy).
    g_rb_2kf_ib1_va = ((static_cast<uint64_t>(e->ib1hi) << 32) | e->ib1lo);
}

void rbgpu_cpsweep_flush_2kf(void) {
    if (g_rb_2kf_got == 0) {
        rbgpu_nvram_note("rbgpu-cph", "NO HALT SAMPLE - the sweep never latched");
        return;
    }
    const struct rb_2kf_s *a = &g_rb_2kf[0];
    const struct rb_2kf_s *b = &g_rb_2kf[1];
    char n[608];
    int w = snprintf(n, sizeof(n),
        "halt@%ums int=%x intc=%x | hdr me=%x pfp=%x ce=%x | ip me=%x pfp=%x "
        "ce=%x | ib1=%x%08x sz=%x cmd=%x | ib2=%x%08x sz=%x cmd=%x | "
        "ceib1=%x%08x sz=%x | vmf=%x va=%x%08x",
        a->ms, a->cpint, a->cpintc, a->mehdr, a->pfphdr, a->cehdr,
        a->meip, a->pfpip, a->ceip,
        a->ib1hi, a->ib1lo, a->ib1sz, a->ib1cmd,
        a->ib2hi, a->ib2lo, a->ib2sz, a->ib2cmd,
        a->ceib1hi, a->ceib1lo, a->ceib1sz,
        a->vmf, a->vmahi, a->vmalo);
    if (w > 0 && g_rb_2kf_got > 1 && static_cast<size_t>(w) < sizeof(n)) {
        snprintf(n + w, sizeof(n) - static_cast<size_t>(w),
                 " || +50ms@%ums ip me=%x pfp=%x ce=%x %s",
                 b->ms, b->meip, b->pfpip, b->ceip,
                 (b->meip == a->meip && b->pfpip == a->pfpip) ? "PARKED" : "MOVING");
    }
    rbgpu_nvram_note("rbgpu-cph", n);
    rbgpu_log("2kf: %s", n);
    rbgpu_log("2kf: int bit 23 PRIV_REG / 22 PRIV_INSTR / 24 OPCODE_ERROR / "
              "27 RESERVED_BIT_ERROR / 14 CP_ECC. PRIV_REG set would mean an "
              "unprivileged packet tried a privileged register - the "
              "reviewer's hypothesis, confirmed. intc=0 means those "
              "interrupts are MASKED and int reading 0 proves nothing (2eh). "
              "ib1 base + (cmd_bufsz - bufsz) locates the packet. PARKED "
              "instruction pointers mean the engines stopped, MOVING means "
              "they are still running something.");
}

void rbgpu_rlcwatch_sample_2jt(unsigned idx) {
    if (g_rb_2js_on != 1 || idx >= RB_2JT_N || !g_hw.rreg) {
        return;
    }
    const uint32_t S1 = 0xa000u;
    struct rb_2jt_s *e = &g_rb_2jt[idx];
    e->ms   = static_cast<uint32_t>(rbgpu_2iq_ms());
    e->me   = g_hw.rreg(g_hw.ctx, (0x1260u + 0x0F56u) * 4);
    e->rlcs = g_hw.rreg(g_hw.ctx, (S1 + 0x4C04u) * 4);
    e->gpm  = g_hw.rreg(g_hw.ctx, (S1 + 0x4E6Eu) * 4);
    e->safe = g_hw.rreg(g_hw.ctx, (S1 + 0x4CA0u) * 4);
    e->cgcg = g_hw.rreg(g_hw.ctx, (S1 + 0x4C49u) * 4);
    e->pg   = g_hw.rreg(g_hw.ctx, (S1 + 0x4C43u) * 4);
    e->srm  = g_hw.rreg(g_hw.ctx, (S1 + 0x4C80u) * 4);
    // 2ka (2026-10-19): the GFXOFF read is REMOVED, and it was the fault.
    //
    // `rbgpu-ring` carried p=0|p=1|p=2 in boots 66 and 68 and has carried
    // **p=0 only** in boots 69, 70, 71, 74 and 76 - a clean break at the 2jv
    // build, the one that added an SMUIO_GFX_MISC_CNTL read to this sampler
    // 40 (then 60) times from a thread_call. -rbgpunocgcg entered at boot 68,
    // which still had all three phases, so the CGCG writes are exonerated.
    // `d=73` is constant in every boot including 66 and 68, so it is the
    // refresh loop's own stopping point and NOT the capture time - it was not
    // the explanation.
    //
    // So the instrument was killing the +100 s and +160 s ring phases, which
    // is why 2jz's `rbgpu-pm4` read `b=67` twice. -rbgpuregdump is already
    // permanently OUT for this exact failure mode (2fe). Removed rather than
    // reasoned about, per the standing rule.
    //
    // Nothing is lost: gfxoff read **2** at both samples straddling the halt
    // in all five of those boots, so GFXOFF is excluded and the measurement
    // is complete. 0xFFFFFFFF now means "not read" rather than "unresolved".
    e->gfxoff = 0xFFFFFFFFu;
    e->cps  = g_hw.rreg(g_hw.ctx, (0x1260u + 0x0F40u) * 4);
    e->grbm = g_hw.rreg(g_hw.ctx, (0x1260u + 0x0DA4u) * 4);
    e->rbb  = g_hw.rreg(g_hw.ctx, (0x1260u + 0x1DE0u) * 4);   // CP_RB0_BASE
    e->rbc  = g_hw.rreg(g_hw.ctx, (0x1260u + 0x1DE1u) * 4);   // CP_RB0_CNTL
    // 2ke
    e->cpint  = g_hw.rreg(g_hw.ctx, (0x1260u + 0x1E0Du) * 4);  // CP_INT_STATUS_RING0
    e->mehdr  = g_hw.rreg(g_hw.ctx, (0x1260u + 0x0F41u) * 4);  // CP_ME_HEADER_DUMP
    e->pfphdr = g_hw.rreg(g_hw.ctx, (0x1260u + 0x0F42u) * 4);  // CP_PFP_HEADER_DUMP
    e->ib1lo  = g_hw.rreg(g_hw.ctx, (S1 + 0x20CCu) * 4);       // CP_IB1_BASE_LO
    e->ib1sz  = g_hw.rreg(g_hw.ctx, (S1 + 0x20CEu) * 4);       // CP_IB1_BUFSZ
    e->vmf    = g_hw.rreg(g_hw.ctx, (0x1260u + 0x15C8u) * 4);  // GCVM_L2_PROTECTION_FAULT_STATUS, seg0 (BASE_IDX 0)
    // 2kf: one-shot sweep on the first halted sample, and pointers again on
    // the next one. Costs nothing on the other 58 samples.
    if (e->me != 0 && g_rb_2kf_got == 0) {
        rbgpu_cpsweep_2kf(0);
        g_rb_2kf_got = 1;
    } else if (g_rb_2kf_got == 1) {
        rbgpu_cpsweep_2kf(1);
        g_rb_2kf_got = 2;
    }
    if (idx + 1 > g_rb_2jt_got) { g_rb_2jt_got = idx + 1; }
}

void rbgpu_rlcwatch_flush_2jt(void) {
    if (g_rb_2js_on != 1) {
        return;
    }
    int last_ok = -1, first_bad = -1;
    for (unsigned i = 0; i < g_rb_2jt_got; i++) {
        if (g_rb_2jt[i].ms == 0) continue;
        if (g_rb_2jt[i].me == 0) { last_ok = static_cast<int>(i); }
        else if (first_bad < 0)  { first_bad = static_cast<int>(i); }
    }
    char n[640];
    if (first_bad < 0 || last_ok < 0) {
        // Still say what the RLC looked like - a window with no transition
        // is a result about WHERE the halt is, not a dead probe.
        const struct rb_2jt_s *z = (g_rb_2jt_got > 0) ? &g_rb_2jt[0] : nullptr;
        snprintf(n, sizeof(n),
                 "n=%u NO TRANSITION in 6.5-9.5s (ok=%d bad=%d) | first "
                 "me=%x rlcs=%x gpm=%x safe=%x cgcg=%x pg=%x srm=%x "
                 "gfxoff=%x",
                 g_rb_2jt_got, last_ok, first_bad,
                 z ? z->me : 0, z ? z->rlcs : 0, z ? z->gpm : 0,
                 z ? z->safe : 0, z ? z->cgcg : 0, z ? z->pg : 0,
                 z ? z->srm : 0, z ? z->gfxoff : 0xFFFFFFFFu);
    } else {
        const struct rb_2jt_s *a = &g_rb_2jt[last_ok];
        const struct rb_2jt_s *b = &g_rb_2jt[first_bad];
        snprintf(n, sizeof(n),
                 "OK @%ums rlcs=%x gpm=%x safe=%x cgcg=%x pg=%x srm=%x "
                 "gfxoff=%x || HALT @%ums me=%x rlcs=%x gpm=%x safe=%x "
                 "cgcg=%x pg=%x srm=%x gfxoff=%x",
                 a->ms, a->rlcs, a->gpm, a->safe, a->cgcg, a->pg, a->srm,
                 a->gfxoff,
                 b->ms, b->me, b->rlcs, b->gpm, b->safe, b->cgcg, b->pg,
                 b->srm, b->gfxoff);
        size_t at = strlen(n);
        if (at < sizeof(n)) {
            at += static_cast<size_t>(snprintf(n + at, sizeof(n) - at,
                     " | cps %x->%x grbm %x->%x rbb %x->%x rbc %x->%x",
                     a->cps, b->cps, a->grbm, b->grbm,
                     a->rbb, b->rbb, a->rbc, b->rbc));
            if (at < sizeof(n)) {
                snprintf(n + at, sizeof(n) - at,
                         " | int %x->%x meh %x->%x pfph %x->%x ib1 %x/%x->%x/%x "
                         "vmf %x->%x",
                         a->cpint, b->cpint, a->mehdr, b->mehdr,
                         a->pfphdr, b->pfphdr,
                         a->ib1lo, a->ib1sz, b->ib1lo, b->ib1sz,
                         a->vmf, b->vmf);
            }
        }
    }
    rbgpu_nvram_note("rbgpu-mew2", n);
    rbgpu_cpsweep_flush_2kf();
    rbgpu_log("2jt: %s", n);
    rbgpu_log("2jy: rbb unchanged across the halt means the CP block answers "
              "reads, so CP_ME_CNTL's 0x15000000 is a real value something "
              "SET. rbb going to 0 or garbage means the block reverted or "
              "stopped answering and the halt bits are a SYMPTOM - which "
              "would explain the refused writes and 2jl's CE_STEP with no "
              "agent at all.");
    rbgpu_log("2jv: gfxoff=0 or 3 at the halt is GFXOFF, and 2ix's exclusion "
              "of it was unsound on timing - gfxoff_status=2 was read in the "
              "LATE diag, after the +160s recovery, never at the halt. "
              "gfxoff=2 at both samples excludes it properly for the first "
              "time. gfxoff=ffffffff means no SMUIO dword resolved.");
    rbgpu_log("2jt: cgcg or pg non-zero at the halt points at the RLC gating "
              "GFX under us, which Linux's non-PSP path prevents by writing "
              "both to 0 - and that would be a one-register fix. gpm "
              "changing at the same sample points at the GPM instead. safe "
              "non-zero revives 2iv.");
}


// ---------------------------------------------------------------------------
// 2ju (2026-10-19): turn coarse-grain clock gating off, and measure it.
//
// 2jt bracketed the re-halt to 49 ms on boot 66 and exactly one bit moves
// with it:
//
//   OK   @8678ms rlcs=25 gpm=140037 safe=0 cgcg=363f pg=0 srm=3
//   HALT @8727ms rlcs=25 gpm=140017 safe=0 cgcg=363f pg=0 srm=3 me=15000000
//
// RLC_GPM_STAT 0x140037 -> 0x140017 clears bit 5,
// CNTX_IDLE_BEING_PROCESSED: at 8678 ms the GPM is running its context-idle
// handler and 49 ms later the handler has finished and CE/PFP/ME are halted.
// `rlcs` is still 0x25 AT the halt, so 2js's RLC_STAT 0x25 -> 0 is later and
// downstream - the RLC did not stop and take the CP with it. `pg=0`,
// `safe=0` and (2js) `srst=0` rule out power gating, safe mode and a soft
// reset.
//
// RLC_CGCG_CGLS_CTRL = 0x363f decodes to CGCG_EN=1, CGLS_EN=1,
// CGLS_REP_COMPANSAT_DELAY=15, CGCG_GFX_IDLE_THRESHOLD=54. Halting the CP is
// how CGCG entry gates the GFX clock, so the configuration and the observed
// handler agree.
//
// It is Apple's default, not a leftover: `_gc_set_clock_gating_feature_flag_10_3`
// (0x126b4231) computes
//     dev+0xd48 = (dev+0x2dc & dev+0x2e0) | (0x7cf & ~dev+0x2e0)
// and 0x7cf has bit 1 (AMD_CG_SUPPORT_GFX_CGCG) and bit 2 (CGLS) set.
// HWLibs ships `_gc_update_coarse_grain_clock_gating_10_1` and **no _10_3**,
// the same gap 2ix found for the autoload config - so nothing on the 10_3
// path ever turns it back off.
//
// NOTE a wrong premise in our own comments, corrected here: `-rbgpunogates`
// turns off the 2fo gate TRAIL, an instrument of ours. It has never touched
// the chip's clock gating. Boot 66 proves it - `cgcg=0x363f` with
// -rbgpunogates in boot-args.
//
// So: write RLC_CGCG_CGLS_CTRL = 0 and RLC_PG_CNTL = 0, which is exactly
// what `gfx_v10_0_rlc_resume`'s non-PSP branch does before starting the RLC.
// Written directly, with no RLC_SAFE_MODE bracket, for two reasons: Linux's
// own non-PSP branch writes them bare, and 2iv found RLC_SAFE_MODE stuck
// with CMD set and Linux's exit sequence unable to clear it across 1000
// polls, so the bracket is the less trustworthy half.
//
// This is a fix AND a measurement, because three things can go wrong and the
// note has to separate them (2eh - absence is not a result):
//
//   1. the write is refused, like CP_ME_CNTL is (2jl)    -> `post` != 0
//   2. it sticks but something restores it               -> a later `pre` != 0
//   3. it sticks and holds and the CP still halts        -> gating was not
//                                                           the mechanism
//
// so every tick records the value BEFORE our write, the value read back
// AFTER it, and CP_ME_CNTL. 24 ticks, 400 ms apart, 2.0 s .. 11.2 s - the
// 8.7 s halt with margin either side, and the idle threshold of 54 is short
// enough that one write at bring-up could not be assumed to hold.
//
// Behind -rbgpunocgcg. Writes to a live GPU, so it is opt-in.
#define RB_2JU_N 24u
struct rb_2ju_s { uint32_t ms, pre, post, me, srm_pre, srm_post; };
static int g_rb_2jw_on = -1;   // 2jw: -rbgpunosrm
static struct rb_2ju_s g_rb_2ju[RB_2JU_N];
static uint32_t        g_rb_2ju_got = 0;
static int             g_rb_2ju_on  = -1;

static bool rbgpu_2ju_enabled(void) {
    if (g_rb_2ju_on < 0) {
        g_rb_2ju_on = checkKernelArgument("-rbgpunocgcg") ? 1 : 0;
        rbgpu_log("2ju: CGCG/PG disable %s",
                  g_rb_2ju_on ? "ARMED (-rbgpunocgcg)" : "off");
        rbgpu_diag_set_num("nocgcg_on", static_cast<uint32_t>(g_rb_2ju_on));
    }
    return g_rb_2ju_on == 1;
}

void rbgpu_nocgcg_tick_2ju(unsigned idx) {
    if (!rbgpu_2ju_enabled() || idx >= RB_2JU_N || !g_hw.rreg || !g_hw.wreg) {
        return;
    }
    const uint32_t S1   = 0xa000u;
    const uint32_t CGCG = (S1 + 0x4C49u) * 4;   // RLC_CGCG_CGLS_CTRL
    const uint32_t PGC  = (S1 + 0x4C43u) * 4;   // RLC_PG_CNTL
    const uint32_t SRMC = (S1 + 0x4C80u) * 4;   // RLC_SRM_CNTL
    struct rb_2ju_s *e = &g_rb_2ju[idx];
    e->ms   = static_cast<uint32_t>(rbgpu_2iq_ms());
    e->pre  = g_hw.rreg(g_hw.ctx, CGCG);
    g_hw.wreg(g_hw.ctx, CGCG, 0);
    g_hw.wreg(g_hw.ctx, PGC,  0);
    e->post = g_hw.rreg(g_hw.ctx, CGCG);
    e->me   = g_hw.rreg(g_hw.ctx, (0x1260u + 0x0F56u) * 4);
    // 2jw (2026-10-19): the save/restore machine, behind its own arg.
    //
    // Every host-visible agent is now excluded with a positive control -
    // power gating, soft reset, RLC safe mode, a running RLC, GRBM routing,
    // RLCG, Apple's write path, CGCG (2jv) and GFXOFF (boot 69: gfxoff=2 at
    // BOTH samples straddling the halt). The halt value is the CP's power-on
    // state with every *_PIPE*_RESET bit clear, and no host path wrote it.
    //
    // One agent is left that can write a GC register without passing through
    // any host path: the RLC's SRM, which replays a saved register list.
    // `srm = 3` (SRM_ENABLE + AUTO_INCR_ADDR) in every sample of every boot,
    // and those lists are exactly what -rbgpurlcsub started supplying as ids
    // 26/27/28. If CP_ME_CNTL is in a restored list carrying its reset value,
    // the RLC writes 0x15000000 back and no counter of ours ever sees it.
    //
    // Honest about what this cannot settle: the re-halt is only OBSERVABLE
    // since 2jq un-halted the CP, so "-rbgpurlcsub introduced it" is not
    // testable by removing rlcsub - that puts complete=0 back and the CP is
    // halted from the start for a different reason. This tests the SRM
    // itself, not which list it came from.
    if (g_rb_2jw_on < 0) {
        g_rb_2jw_on = checkKernelArgument("-rbgpunosrm") ? 1 : 0;
        rbgpu_diag_set_num("nosrm_on", static_cast<uint32_t>(g_rb_2jw_on));
    }
    if (g_rb_2jw_on == 1) {
        e->srm_pre = g_hw.rreg(g_hw.ctx, SRMC);
        g_hw.wreg(g_hw.ctx, SRMC, 0);
        e->srm_post = g_hw.rreg(g_hw.ctx, SRMC);
    } else {
        e->srm_pre  = g_hw.rreg(g_hw.ctx, SRMC);
        e->srm_post = e->srm_pre;
    }
    if (idx + 1 > g_rb_2ju_got) { g_rb_2ju_got = idx + 1; }
}

void rbgpu_nocgcg_flush_2ju(void) {
    if (g_rb_2ju_on != 1) {
        return;
    }
    // What each of the three failure modes looks like, counted separately.
    unsigned refused = 0;    // post != 0: the write did not take
    unsigned restored = 0;   // pre != 0 on a tick after one that took
    int      first_restore = -1;  // 2jv: boot 68 reported restored=1 and the
                                  // note could not say WHERE - fixed here
    int      first_halt = -1;
    int      last_tick  = -1;
    bool     took_once  = false;
    for (unsigned i = 0; i < g_rb_2ju_got; i++) {
        if (g_rb_2ju[i].ms == 0) continue;
        last_tick = static_cast<int>(i);
        if (g_rb_2ju[i].post != 0) { refused++; }
        else                       { took_once = true; }
        // 2jw fix: `took_once` is set in THIS iteration when the write
        // takes, so the old test counted tick 0's own pre-write 0x363f as a
        // restore. Boots 68 and 69 both reported restored=1 and both meant
        // ZERO restores - `restore1 @2339ms pre=363f` is the first tick.
        if (took_once && i > 0 && g_rb_2ju[i].pre != 0) {
            restored++;
            if (first_restore < 0) { first_restore = static_cast<int>(i); }
        }
        if (g_rb_2ju[i].me != 0 && first_halt < 0) {
            first_halt = static_cast<int>(i);
        }
    }
    char n[416];
    if (last_tick < 0) {
        snprintf(n, sizeof(n), "n=0 NO TICKS RAN - timers never fired");
    } else {
        const struct rb_2ju_s *f = &g_rb_2ju[0];
        const struct rb_2ju_s *l = &g_rb_2ju[last_tick];
        int n1 = snprintf(n, sizeof(n),
                          "n=%u refused=%u restored=%u | first @%ums "
                          "pre=%x post=%x me=%x | last @%ums pre=%x post=%x "
                          "me=%x | ",
                          g_rb_2ju_got, refused, restored,
                          f->ms, f->pre, f->post, f->me,
                          l->ms, l->pre, l->post, l->me);
        if (n1 < 0) { n1 = 0; }
        if (first_halt < 0) {
            n1 += snprintf(n + n1, sizeof(n) - static_cast<size_t>(n1),
                           "NO HALT in 2.0-11.2s");
        } else {
            const struct rb_2ju_s *h = &g_rb_2ju[first_halt];
            n1 += snprintf(n + n1, sizeof(n) - static_cast<size_t>(n1),
                           "HALT @%ums me=%x pre=%x post=%x",
                           h->ms, h->me, h->pre, h->post);
        }
        if (n1 > 0 && static_cast<size_t>(n1) < sizeof(n)) {
            const struct rb_2ju_s *h =
                (first_halt >= 0) ? &g_rb_2ju[first_halt] : &g_rb_2ju[last_tick];
            n1 += snprintf(n + n1, sizeof(n) - static_cast<size_t>(n1),
                           " | srm %x->%x", h->srm_pre, h->srm_post);
        }
        if (first_restore >= 0 && n1 > 0 &&
            static_cast<size_t>(n1) < sizeof(n)) {
            const struct rb_2ju_s *r = &g_rb_2ju[first_restore];
            snprintf(n + n1, sizeof(n) - static_cast<size_t>(n1),
                     " | restore1 @%ums pre=%x", r->ms, r->pre);
        }
    }
    rbgpu_nvram_note("rbgpu-cgcg", n);
    rbgpu_log("2ju: %s", n);
    rbgpu_log("2ju: refused>0 means RLC_CGCG_CGLS_CTRL is host-read-only like "
              "CP_ME_CNTL (2jl) and this lever does not exist. restored>0 "
              "means the write takes but something puts 0x363f back - the "
              "save/restore machine is live (srm=3), so that is the one to "
              "expect. NO HALT with refused=0 restored=0 is the fix. A HALT "
              "with refused=0 restored=0 refutes CGCG as the mechanism and "
              "sends 2jt's gpm bit 5 back to being a coincidence.");
}
void rbgpu_ring_probe_2hk(unsigned phase) {
    if (g_rb_2hk_on < 0) {
        g_rb_2hk_on = checkKernelArgument("-rbgpuringprobe") ? 1 : 0;
        rbgpu_log("2hk: ring probe %s", g_rb_2hk_on ? "ON" : "OFF (-rbgpuringprobe)");
    }
    if (!g_rb_2hk_on) {
        return;
    }

    // Every refusal below is distinct and lands in NVRAM, because "the probe
    // read nothing" and "the probe never ran" have looked identical three
    // times in this project.
    void *obj = g_rb_2fb_regobj;
    if (!obj) {
        rbgpu_nvram_note("rbgpu-ring", "SKIP: no accessor stashed at doStart");
        rbgpu_log("2hk: SKIPPED - g_rb_2fb_regobj is NULL, nothing was read");
        return;
    }
    void **vt = *reinterpret_cast<void ***>(obj);
    if (!vt) {
        rbgpu_nvram_note("rbgpu-ring", "SKIP: accessor vtable NULL");
        rbgpu_log("2hk: SKIPPED - accessor vtable NULL, nothing was read");
        return;
    }
    auto resolve = reinterpret_cast<uint32_t (*)(void *, uint64_t)>(
        vt[0x148 / sizeof(void *)]);
    auto rd = reinterpret_cast<uint32_t (*)(void *, uint32_t)>(
        vt[0x118 / sizeof(void *)]);
    if (!resolve || !rd) {
        rbgpu_nvram_note("rbgpu-ring", "SKIP: vtable[0x148] or [0x118] NULL");
        rbgpu_log("2hk: SKIPPED - resolve=%p read=%p, nothing was read",
                  reinterpret_cast<void *>(resolve), reinterpret_cast<void *>(rd));
        return;
    }

    // Committed BEFORE the first read, and it carries whatever earlier phases
    // already recorded. If this is all that survives, the read stalled - which
    // is a result, not an absence.
    {
        char enter[400];
        snprintf(enter, sizeof(enter), "%s%sENTER p=%u (reading; a lone ENTER = STALL)",
                 g_rb_2hm_ring, g_rb_2hm_ring[0] ? " " : "", phase);
        rbgpu_nvram_note("rbgpu-ring", enter);
    }
    rbgpu_log("2hk: ENTER phase=%u - about to resolve and read the register set. "
              "If rbgpu-ring still ends in ENTER after this boot, the accessor "
              "stalled and THAT is the finding (2fe's failure, reproduced).",
              phase);

    // 2hm: the order matters only in that the VM fault registers come FIRST.
    // 2hl showed the CP front end holding packets it cannot retire with
    // nothing downstream active, which is the shape of a fetch that never
    // returns. If a fault is latched, these name it outright - with the
    // faulting address - and no further inference is needed. Reading them
    // does not clear them; the fault status is cleared by a write, which this
    // probe never does.
    //
    // 2ho: `once` marks the registers that cannot change during a boot - the
    // fault-enable mask and the ring's base address. They are read and logged
    // on phase 0 only. Three samples of everything would not fit the note,
    // and a note that silently truncates is worse than one that is explicit
    // about what it carries.
    struct { const char *name; uint64_t id; bool once; } regs[] = {
        { "vmf",     RB_2HM_VMFAULT_STATUS, false },
        { "vmalo",   RB_2HM_VMFAULT_LO32,   false },
        { "vmahi",   RB_2HM_VMFAULT_HI32,   false },
        { "vmfc",    RB_2HM_VMFAULT_CNTL,   true  },
        { "r",       RB_2HK_CP_RB0_RPTR,    false },
        { "w",       RB_2HK_CP_RB0_WPTR,    false },
        { "cps",     RB_2HK_CP_STAT,        false },
        { "grbm",    RB_2HK_GRBM_STATUS,    false },
        { "g2",      RB_2HM_GRBM_STATUS2,   false },
        { "me",      RB_2HM_CP_ME_CNTL,     false },
        { "rbb",     RB_2HM_CP_RB0_BASE,    true  },
        { "rbbhi",   RB_2HM_CP_RB0_BASE_HI, true  },
    };

    char one[288];
    size_t n = 0;
    int w = snprintf(one, sizeof(one), "p=%u", phase);
    n = (w > 0) ? static_cast<size_t>(w) : 0;
    for (unsigned i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
        if (regs[i].once && phase != 0) {
            continue;
        }
        uint32_t off = resolve(obj, regs[i].id);
        uint32_t val = rd(obj, off);
        rbgpu_log("2hk: %s dword 0x%x = 0x%08x", regs[i].name, off, val);
        if (n >= sizeof(one) - 24) {
            break;
        }
        w = snprintf(one + n, sizeof(one) - n, " %s=%x", regs[i].name, val);
        if (w > 0) {
            n += static_cast<size_t>(w);
            if (n >= sizeof(one)) {
                n = sizeof(one) - 1;
            }
        }
    }

    size_t have = strlen(g_rb_2hm_ring);
    snprintf(g_rb_2hm_ring + have, sizeof(g_rb_2hm_ring) - have,
             "%s%s", have ? " | " : "", one);
    rbgpu_nvram_note("rbgpu-ring", g_rb_2hm_ring);
    rbgpu_log("2hk: %s", one);

    // 2is (2026-10-18): read CP_ME_CNTL and CP_STAT BOTH WAYS, same instant.
    //
    // 2ir found that nobody writes 0x15000000. All three writes to
    // CP_ME_CNTL are ZERO - HWLibs un-halts at 239 ms, X6000 writes zero
    // again at 43.6 s and 88.6 s - and the ring probe still reads
    // 0x15000000. Worse, the SAME register reads 0 in the diag probe
    // (diag_cp_me_cntl=0, diag_cp_stat=0) in the same boot.
    //
    // Two explanations and nothing yet separating them:
    //
    //   TIMING   the diag probe runs early, during bring-up, shortly after
    //            HWLibs' 239 ms enable. 0 would be correct then, and the
    //            halt would be something that happens later - and since no
    //            driver write sets it, that something is firmware or the
    //            hardware.
    //   THE READ the accessor path is returning a value that is not this
    //            register. docs/RULES.md already distrusts GRBM_STATUS for
    //            exactly this - it reads a0003028 in every sample of every
    //            boot, including ones where CP_STAT says the CP is idle.
    //
    // The difference between the two paths is the mechanism, not the
    // offset: the diag probe uses g_hw.rreg (the plugin's own BAR0 mapping)
    // at (0x1260 + dword) * 4, the ring probe uses socRegOffset + [0x118].
    // So read both, here, microseconds apart, on the same register.
    //
    // Disagreement means the accessor read is not to be trusted, and with
    // it goes "the CP is halted" - the claim 2hn, 2hp and everything since
    // rests on. Agreement means the halt is real, arrived after 239 ms, and
    // no driver code did it.
    if (g_hw.rreg) {
        uint32_t d_me  = g_hw.rreg(g_hw.ctx, (0x1260u + 0x0F56u) * 4);
        uint32_t d_cps = g_hw.rreg(g_hw.ctx, (0x1260u + 0x0F40u) * 4);
        uint32_t d_grb = g_hw.rreg(g_hw.ctx, (0x1260u + 0x0DA4u) * 4);
        uint32_t a_me  = rd(obj, resolve(obj, RB_2HM_CP_ME_CNTL));
        uint32_t a_cps = rd(obj, resolve(obj, RB_2HK_CP_STAT));
        uint32_t a_grb = rd(obj, resolve(obj, RB_2HK_GRBM_STATUS));
        char b2[160];
        snprintf(b2, sizeof(b2),
                 "p%u me d=%x a=%x %s| cps d=%x a=%x %s| grbm d=%x a=%x %s",
                 phase, d_me, a_me, (d_me == a_me) ? "OK " : "DIFF ",
                 d_cps, a_cps, (d_cps == a_cps) ? "OK " : "DIFF ",
                 d_grb, a_grb, (d_grb == a_grb) ? "OK " : "DIFF ");
        size_t hv = strlen(g_rb_2is_note);
        snprintf(g_rb_2is_note + hv, sizeof(g_rb_2is_note) - hv,
                 "%s%s", hv ? " | " : "", b2);
        rbgpu_nvram_note("rbgpu-rd2", g_rb_2is_note);
        rbgpu_log("2is: %s", b2);
    } else if (phase == 0) {
        rbgpu_nvram_note("rbgpu-rd2", "g_hw.rreg is NULL - the direct path is "
                                      "unavailable, so nothing was compared");
    }

    // 2it (2026-10-18): IS THE CP MICROCODE ACTUALLY THERE?
    //
    // 2is settled the read: both paths agree exactly on CP_ME_CNTL, CP_STAT
    // and GRBM_STATUS at both phases, so the accessor is sound and **the CP
    // really is halted**. The earlier diag_cp_me_cntl=0 was timing - that
    // probe runs just after HWLibs' 246 ms enable, where 0 was correct.
    //
    // Which leaves a timeline that points at one thing:
    //
    //     246 ms    HWLibs writes CP_ME_CNTL = 0   (enable)
    //    <12000 ms  the halt appears, with NO driver write
    //    12000 ms   reads 0x15000000
    //    17321 ms   X6000 writes CP_ME_CNTL = 0    (enable again)
    //   100000 ms   STILL reads 0x15000000
    //
    // The driver writes zero and the bits do not clear - twice. On gfx10
    // there is a standard reason for exactly that: CE_HALT, PFP_HALT and
    // ME_HALT cannot be cleared until the CP microcode is loaded and valid.
    // gfx_v10_0_cp_gfx_enable(adev, true) only takes effect after the ucode
    // is in place; on this part the PSP loads it.
    //
    // And that lands on the oldest debt in this project. 2am patches
    // _gc_fw_autoload_is_completed to return 1 unconditionally, reasoning
    // that the PSP placed the firmware so the RLC's autoload latch never
    // sets. An independent Raphael-iGPU bring-up found real microcode in
    // instruction RAM - but **that was never verified on THIS chip**, which
    // is precisely the explanation 2cy demanded and docs/RULES.md still lists as
    // owed. A halted CP whose halt bits will not clear is what an unloaded
    // CP looks like.
    //
    // So read the instruction RAM. BASE_IDX 1 for these, i.e. seg1 0xa000,
    // which the project already proves with the RLC reads, so this uses
    // g_hw directly rather than the accessor (whose encoded ids are seg0).
    //
    //   0x5814 CP_PFP_UCODE_ADDR   0x5815 CP_PFP_UCODE_DATA
    //   0x5816 CP_ME_RAM_RADDR     0x5817 CP_ME_RAM_DATA
    //   0x5818 CP_CE_UCODE_ADDR    0x5819 CP_CE_UCODE_DATA
    //   0x581a CP_MEC_ME1_UCODE_ADDR 0x581b CP_MEC_ME1_UCODE_DATA
    //
    // WHAT THIS DOES TO THE CHIP: writes 0 to each engine's UCODE_ADDR - a
    // read POINTER into instruction RAM, the same class of register as
    // GRBM_GFX_CNTL (2do), which this project already treats as benign
    // because it selects rather than acts - then reads DATA 64 times, which
    // auto-increments. Nothing is written to the RAM itself and no engine
    // state changes. It is safe here precisely because the CP is halted,
    // which is the premise under test. Gated on -rbgpucpfw, its own arg.
    //
    //   nz near 64 with varied values  the microcode IS loaded, 2am's
    //                                  assumption holds, and the halt needs
    //                                  another explanation
    //   nz = 0, or all-ones            the RAM is EMPTY. The PSP did not
    //                                  load the CP microcode, 2am has been
    //                                  reporting success for firmware that
    //                                  was never placed, and the halted CP
    //                                  follows directly - as does every
    //                                  stalled rptr since 2hl.
    if (phase == 0 && g_hw.rreg && g_hw.wreg &&
        checkKernelArgument("-rbgpucpfw")) {
        static const struct { uint32_t a, d; const char *n; } kE[] = {
            { 0x5814, 0x5815, "pfp" }, { 0x5816, 0x5817, "me" },
            { 0x5818, 0x5819, "ce" },  { 0x581a, 0x581b, "mec" },
        };
        char fw[352];
        size_t fn = 0;
        for (unsigned e = 0; e < 4; e++) {
            g_hw.wreg(g_hw.ctx, (0xa000u + kE[e].a) * 4, 0);
            uint32_t nz = 0, ones = 0, first = 0;
            for (unsigned k = 0; k < 64; k++) {
                uint32_t v = g_hw.rreg(g_hw.ctx, (0xa000u + kE[e].d) * 4);
                if (k == 0) first = v;
                if (v == 0xFFFFFFFFu) ones++;
                else if (v) nz++;
            }
            if (fn >= sizeof(fw) - 56) break;
            int fwr = snprintf(fw + fn, sizeof(fw) - fn,
                               "%s%s nz=%u f1=%u d0=%x", fn ? " | " : "",
                               kE[e].n, nz, ones, first);
            if (fwr > 0) { fn += static_cast<size_t>(fwr); if (fn >= sizeof(fw)) { fn = sizeof(fw) - 1; break; } }
        }
        rbgpu_nvram_note("rbgpu-cpfw", fw);
        rbgpu_log("2it: %s", fw);
        rbgpu_log("2it: nz near 64 with varied values means the CP microcode "
                  "IS loaded and the halt needs another explanation. nz=0 or "
                  "all-ones means the RAM is EMPTY - the PSP never loaded it, "
                  "2am has been reporting success for firmware that was never "
                  "placed, and the halted CP follows directly.");
    } else if (phase == 0 && !checkKernelArgument("-rbgpucpfw")) {
        rbgpu_nvram_note("rbgpu-cpfw", "off - -rbgpucpfw not set, the "
                                       "instruction RAM was not read");
    }

    // 2iu (2026-10-18): WHY the halt bits will not clear. Four candidates,
    // one sample, at the moment the CP is actually halted.
    //
    // 2it closed the firmware question: pfp/me/ce/mec all read 64 of 64
    // nonzero dwords of instruction RAM with distinct plausible first words,
    // so the CP microcode IS loaded and 2am's bypass is vindicated - the
    // oldest debt in docs/RULES.md, verified on this chip at last. The halt is
    // not an unloaded CP.
    //
    // The decodes are also now checked against gc_10_3_0_sh_mask.h rather
    // than carried from memory, which they had been for ten milestones:
    //   CP_ME_CNTL bits 0x18/0x1a/0x1c = CE_HALT/PFP_HALT/ME_HALT, so
    //     0x15000000 is exactly those three. Confirmed.
    //   GRBM_STATUS 0xa0003028 = CMDFIFO_AVAIL 0x28, DB_CLEAN, CB_CLEAN,
    //     CP_BUSY, GUI_ACTIVE. Confirmed - the graphics pipe claims
    //     outstanding work while its ME is halted.
    //   RLC_GPM_STAT 0xb40017 = RLC_BUSY, GFX_POWER_STATUS,
    //     GFX_CLOCK_STATUS, GFX_PIPELINE_POWER_STATUS, CMP_power_status,
    //     GFX_CLOCK_STATUS_3D, MGCG/FGCG overrides.
    //
    // That last one is the point. It says GFX is powered and clocked - but
    // it comes from the EARLY diag pass, the same pass that read
    // CP_ME_CNTL = 0 before the halt appeared. Every "GFX is healthy"
    // reading this project owns is from bring-up, and every "CP is halted"
    // reading is from later. Nobody has looked at the power state at the
    // moment the CP is halted, and if GFX were power-gated then the halt
    // bits would read at their reset default, writes would be dropped, the
    // ring would never advance and the status registers would be stale -
    // which is every symptom at once, and would also explain 2di's
    // gfx_min = gfx_max = 0, unexplained since 2026-10-12.
    //
    // Four discriminators, all reads but one:
    //
    //   RLC_GPM_STAT bit 1 clear   GFX is POWER-GATED. One cause for the
    //                              halt, the stale GRBM_STATUS and 2di.
    //   GRBM_SOFT_RESET nonzero    the CP is held in reset, so writes to
    //                              its control register do nothing
    //   CP_MEC_CNTL                the compute pipe's state beside the
    //                              graphics one - Apple drives clients
    //                              through MEC/KIQ, so a running MEC with a
    //                              halted ME may be normal and the halt a
    //                              red herring
    //   write-readback             write 0 to CP_ME_CNTL and read it back
    //                              IMMEDIATELY. Back as 0 means something
    //                              re-asserts it later; back as 0x15000000
    //                              means the write is refused right now.
    //
    // The one write is 0 to CP_ME_CNTL - byte for byte what HWLibs writes at
    // 246 ms and X6000 writes twice more. It asks the chip nothing the
    // driver is not already asking it.
    if (g_hw.rreg && g_hw.wreg) {
        uint32_t rlc_cnt = g_hw.rreg(g_hw.ctx, (0xa000u + 0x4C00u) * 4);
        uint32_t rlc_st  = g_hw.rreg(g_hw.ctx, (0xa000u + 0x4C04u) * 4);
        uint32_t rlc_gpm = g_hw.rreg(g_hw.ctx, (0xa000u + 0x4E6Eu) * 4);
        uint32_t rlc_sfm = g_hw.rreg(g_hw.ctx, (0xa000u + 0x4CA0u) * 4);
        uint32_t soft_rs = g_hw.rreg(g_hw.ctx, (0x1260u + 0x0DA8u) * 4);
        uint32_t mec_cnt = g_hw.rreg(g_hw.ctx, (0x1260u + 0x0F55u) * 4);

        uint32_t me_before = g_hw.rreg(g_hw.ctx, (0x1260u + 0x0F56u) * 4);
        g_hw.wreg(g_hw.ctx, (0x1260u + 0x0F56u) * 4, 0);
        uint32_t me_now = g_hw.rreg(g_hw.ctx, (0x1260u + 0x0F56u) * 4);

        char u[224];
        snprintf(u, sizeof(u),
                 "p%u gpm=%x pwr=%u clk=%u pipe=%u | rlc c=%x s=%x sfm=%x | "
                 "srst=%x mec=%x | me %x->%x %s",
                 phase, rlc_gpm, (rlc_gpm >> 1) & 1, (rlc_gpm >> 2) & 1,
                 (rlc_gpm >> 4) & 1, rlc_cnt, rlc_st, rlc_sfm,
                 soft_rs, mec_cnt, me_before, me_now,
                 (me_now == 0) ? "TOOK" : "REFUSED");
        size_t uh = strlen(g_rb_2iu_note);
        snprintf(g_rb_2iu_note + uh, sizeof(g_rb_2iu_note) - uh,
                 "%s%s", uh ? " | " : "", u);
        rbgpu_nvram_note("rbgpu-pwr", g_rb_2iu_note);
        rbgpu_log("2iu: %s", u);
        rbgpu_log("2iu: pwr=0 means the GFX block is power-gated and one "
                  "cause explains the halt, the stale GRBM_STATUS and 2di "
                  "together. srst nonzero means the CP is held in reset. "
                  "me TOOK means the write lands and something re-asserts it "
                  "later; me REFUSED means it is being dropped right now, "
                  "which with pwr=1 and srst=0 would leave RLC safe mode - "
                  "2an's unverified bypass - as the remaining candidate.");
    }

    // 2iv (2026-10-18): try to UN-HALT the CP. First real fix attempt at the
    // thing that has blocked every boot since 2hl.
    //
    // 2iu narrowed it to one hard fact and eliminated three candidates at
    // the moment of the halt: GFX is powered and clocked (pwr=1 clk=1
    // pipe=1), GRBM_SOFT_RESET is 0, the MEC is NOT halted (mec=0), and the
    // write to CP_ME_CNTL is **REFUSED** - 0 written, 0x15000000 read back
    // microseconds later. Not re-asserted later: dropped now.
    //
    // RLCG is eliminated statically, without a boot:
    // gfx_v10_0_is_rlcg_access_range calls check_rlcg_range with
    // entries = NULL and arr_size = 0, which returns false for every
    // register. No gfx10 GC register uses the RLCG indirect path, so that
    // is not why the write vanishes.
    //
    // What is left is the RLC. It is running (RLC_CNTL = RLC_ENABLE_F32),
    // busy (RLC_STAT 0x25 = RLC_BUSY | RLC_GPM_BUSY | RLC_THREAD_0_BUSY) -
    // and RLC_SAFE_MODE reads 1, i.e. CMD set with MESSAGE and RESPONSE
    // clear: **an unacknowledged safe-mode command**. In safe mode the RLC
    // quiesces the graphics pipe and owns its registers, which is what a
    // silently dropped CP_ME_CNTL write looks like from outside.
    //
    // And that is 2an's debt coming due. 2am's bypass was vindicated by
    // 2it; 2an patched _gc_check_register_equal_ext precisely because
    // _gc_enter_rlc_safe_mode_10_3 was timing out waiting on a register,
    // and nothing has ever verified what state that left the RLC in. This
    // reading says: pending, forever.
    //
    // THE ATTEMPT, in Linux's own sequence
    // (amdgpu_gfx_rlc_exit_safe_mode -> WREG32(RLC_SAFE_MODE, CMD_MASK)):
    //
    //   1. write RLC_SAFE_MODE = 1 (CMD set, MESSAGE clear) - the exit
    //   2. poll CMD for up to 1000 reads, bounded, no sleeping
    //   3. write CP_ME_CNTL = 0 and read back
    //   4. if still refused, force RLC_SAFE_MODE = 0 and try once more
    //   5. report rptr/wptr after, so a CP that starts consuming is visible
    //      in the same note
    //
    // Every value written is one Linux writes at the same register. Nothing
    // is invented.
    //
    // WRONG PREMISE, corrected 2ju (2026-10-19): this said "-rbgpunogates is
    // already in boot-args so clock gating is off". It is not.
    // -rbgpunogates turns off the 2fo gate TRAIL, an instrument of ours, and
    // has never touched the chip. Boot 66 read RLC_CGCG_CGLS_CTRL = 0x363f -
    // CGCG_EN and CGLS_EN both set - with -rbgpunogates in boot-args. So the
    // stated reason this was a reasonable register to poke did not hold.
    //
    // Gated on -rbgpurlcexit. Phase 0 only, before any client.
    if (phase == 0 && g_hw.rreg && g_hw.wreg &&
        checkKernelArgument("-rbgpurlcexit")) {
        const uint32_t kSFM = (0xa000u + 0x4CA0u) * 4;
        const uint32_t kME  = (0x1260u + 0x0F56u) * 4;
        char v[352];
        size_t vn = 0;

        uint32_t sfm0 = g_hw.rreg(g_hw.ctx, kSFM);
        g_hw.wreg(g_hw.ctx, kSFM, 1u);              // Linux's exit: CMD only
        uint32_t spins = 0, sfm1 = 0;
        for (; spins < 1000; spins++) {
            sfm1 = g_hw.rreg(g_hw.ctx, kSFM);
            if ((sfm1 & 1u) == 0) break;
        }
        g_hw.wreg(g_hw.ctx, kME, 0);
        uint32_t me1 = g_hw.rreg(g_hw.ctx, kME);

        int w1 = snprintf(v, sizeof(v), "sfm %x->%x sp=%u | me1=%x %s",
                          sfm0, sfm1, spins, me1,
                          (me1 == 0) ? "TOOK" : "still");
        vn = (w1 > 0) ? static_cast<size_t>(w1) : 0;

        uint32_t me2 = me1;
        if (me1 != 0) {
            g_hw.wreg(g_hw.ctx, kSFM, 0);           // force the CMD bit down
            uint32_t sfm2 = g_hw.rreg(g_hw.ctx, kSFM);
            g_hw.wreg(g_hw.ctx, kME, 0);
            me2 = g_hw.rreg(g_hw.ctx, kME);
            if (vn < sizeof(v) - 48) {
                int w2 = snprintf(v + vn, sizeof(v) - vn,
                                  " | force sfm=%x me2=%x %s", sfm2, me2,
                                  (me2 == 0) ? "TOOK" : "REFUSED");
                if (w2 > 0) vn += static_cast<size_t>(w2);
            }
        }

        uint32_t rp = rd(obj, resolve(obj, RB_2HK_CP_RB0_RPTR));
        uint32_t wp = rd(obj, resolve(obj, RB_2HK_CP_RB0_WPTR));
        uint32_t cs = rd(obj, resolve(obj, RB_2HK_CP_STAT));
        if (vn < sizeof(v) - 40) {
            snprintf(v + vn, sizeof(v) - vn, " | r=%x w=%x cps=%x", rp, wp, cs);
        }
        rbgpu_nvram_note("rbgpu-rlcx", v);
        rbgpu_log("2iv: %s", v);
        rbgpu_log("2iv: me1=0 or me2=0 means the CP accepted the un-halt once "
                  "the RLC released it - the blocker since 2hl, and a fix to "
                  "generalise into the bring-up path. Both still 0x15000000 "
                  "means safe mode is NOT what refuses the write, and with "
                  "power, reset and RLCG all eliminated the refusal has no "
                  "candidate left and needs a different kind of search. "
                  "r advancing past 0x%x in the same note would be the CP "
                  "consuming the ring for the first time.", RB_2HW_PM4_MIN);
    } else if (phase == 0 && !checkKernelArgument("-rbgpurlcexit")) {
        rbgpu_nvram_note("rbgpu-rlcx", "off - -rbgpurlcexit not set, the RLC "
                                       "was not touched");
    }

    // 2iw (2026-10-18): the POSITIVE CONTROL the write probe never had.
    //
    // 2iv eliminated safe mode: Linux's exit never cleared CMD across 1000
    // polls, and forcing RLC_SAFE_MODE = 0 DID take - yet CP_ME_CNTL still
    // refused. Which looked like "that one register is protected", until
    // the obvious confound: RLC_SAFE_MODE is seg1 and CP_ME_CNTL is seg0,
    // and every CP_ME_CNTL write this project has tried went through
    // g_hw.wreg, while the context-base writes that demonstrably TOOK at
    // the same instant went through the ACCESSOR - Apple's own path.
    //
    // So I have been comparing a refused write on one path against
    // successful writes on another and drawing conclusions about the
    // register. "seg1 writes work, seg0 g_hw writes do not" fits the data
    // exactly as well as "CP_ME_CNTL is protected" - and if it is the
    // former, 2iu's and 2iv's REFUSED is an INSTRUMENT ARTIFACT, not a fact
    // about the chip. That is 2id's missing positive control again, in a
    // new place: a write probe needs one as much as a hook does.
    //
    // The matrix, one sample, three cells:
    //
    //   A  g_hw -> SCRATCH_REG0 (seg1 0x2040), write 0xA5A5A5A5
    //        Does the g_hw write mechanism work at all right now?
    //        SCRATCH_REG0 exists to be scribbled on; nothing reads it.
    //   B  accessor -> CP_ME_CNTL, write 0, read back BOTH ways
    //        Apple's own path on the target register. Never tried.
    //   C  g_hw -> CP_ME_CNTL, write 0, read back
    //        The cell 2iu/2iv measured, repeated beside the others so the
    //        comparison is within one sample rather than across boots.
    //
    //   A fails                 g_hw writes are broken here and 2iu/2iv's
    //                           conclusion is withdrawn entirely
    //   A takes, B takes        the CP CAN be un-halted - the refusals were
    //                           the path, not the register, and the fix is
    //                           to use the accessor
    //   A takes, B and C refuse CP_ME_CNTL really is protected, by
    //                           something still unidentified, and the
    //                           elimination list stands
    //
    // SCRATCH_REG0 is restored to its original value immediately. The
    // CP_ME_CNTL writes are 0, which is what HWLibs and X6000 both write.
    if (phase == 0 && g_hw.rreg && g_hw.wreg) {
        const uint32_t kSCR = (0xa000u + 0x2040u) * 4;
        const uint32_t kMEb = (0x1260u + 0x0F56u) * 4;
        uint32_t scr_was = g_hw.rreg(g_hw.ctx, kSCR);
        g_hw.wreg(g_hw.ctx, kSCR, 0xA5A5A5A5u);
        uint32_t scr_now = g_hw.rreg(g_hw.ctx, kSCR);
        g_hw.wreg(g_hw.ctx, kSCR, scr_was);          // put it back

        void **wvt = *reinterpret_cast<void ***>(obj);
        auto wracc = reinterpret_cast<void (*)(void *, uint32_t, uint32_t)>(
            wvt ? wvt[0x120 / sizeof(void *)] : nullptr);
        uint32_t me_off = resolve(obj, RB_2HM_CP_ME_CNTL);
        uint32_t b_before = rd(obj, me_off);
        if (wracc) wracc(obj, me_off, 0);             // ACCESSOR write
        uint32_t b_acc = rd(obj, me_off);
        uint32_t b_dir = g_hw.rreg(g_hw.ctx, kMEb);

        g_hw.wreg(g_hw.ctx, kMEb, 0);                 // g_hw write
        uint32_t c_dir = g_hw.rreg(g_hw.ctx, kMEb);

        char x[320];
        snprintf(x, sizeof(x),
                 "A scr %x->%x %s | B acc me %x->%x (dir %x) %s | C dir %x %s",
                 scr_was, scr_now,
                 (scr_now == 0xA5A5A5A5u) ? "TOOK" : "FAILED",
                 b_before, b_acc, b_dir, (b_acc == 0) ? "TOOK" : "REFUSED",
                 c_dir, (c_dir == 0) ? "TOOK" : "REFUSED");
        rbgpu_nvram_note("rbgpu-wtest", x);
        rbgpu_log("2iw: %s", x);
        rbgpu_log("2iw: A FAILED means the g_hw write mechanism is broken "
                  "here and 2iu/2iv's REFUSED is withdrawn as an instrument "
                  "artifact. A TOOK with B TOOK means the CP can be "
                  "un-halted through Apple's own accessor and the refusals "
                  "were the PATH, not the register. A TOOK with B and C both "
                  "REFUSED means CP_ME_CNTL is genuinely protected and the "
                  "elimination list stands.");
    }

    // 2jn (2026-10-19): IS THE GRBM ROUTING THE WRITES SOMEWHERE ELSE?
    //
    // 2jl closed the "CP re-halts itself" idea: CE_STEP - an ordinary R/W
    // debug bit - would not stick either, and across eight tight-loop reads
    // no bit of CP_ME_CNTL ever moved. A register that reads a constant and
    // accepts nothing is not a protected register. It is a register nobody
    // is actually addressing.
    //
    // GRBM_GFX_CNTL (seg0 0x0dc2) carries MEID/PIPEID/QUEUEID/VMID and
    // ROUTES CP register access. GRBM_GFX_INDEX (seg1 0x2200) carries
    // SE_INDEX/SA_INDEX/INSTANCE_INDEX and three broadcast bits. Linux
    // brackets EVERY MQD programming with nv_grbm_select(me,pipe,queue,0)
    // ... nv_grbm_select(0,0,0,0) - gfx_v10_0.c lines 3832/3867,
    // 5214/5219, 6865/6876, 7164/7166 - because leaving it set misroutes
    // everything after it. And gfx_v10_0_select_se_sh builds
    // 0xE0000000 (SE | INSTANCE | SA broadcast) as the "all" value.
    //
    // We KNOW Apple programmed MQDs this boot: rbgpu-gate records
    // "32 mapQueues: the ORIGINAL returned".
    //
    // And the Navi21 angle, which is this project's recurring bug shape:
    // **Navi 21 has 4 shader engines. Rembrandt has 1.** If Apple's code
    // walks SE 0..3 and leaves SE_INDEX at 1, 2 or 3, every SE-indexed GC
    // register is addressed on a shader engine that does not exist here -
    // reads return a constant, writes go nowhere. That is 2gi/2gu's
    // four-SDMA-engines bug in a different block.
    //
    // It explains every observation at once: CP_ME_CNTL constant and
    // unwritable; CE_STEP refused; GCVM seg0 writes landing (GFXHUB is not
    // SE-indexed); SCRATCH_REG0 seg1 taking (not SE-indexed); APPLE'S OWN
    // un-halt refused (same misrouted path); the RLC stop changing nothing;
    // and GRBM_STATUS reading a0003028 in every sample of every boot, which
    // has been an open debt since 2hp.
    //
    // READ half: unconditional, read-only, two registers.
    // WRITE half: only under -rbgpugrbmfix. Sets GRBM_GFX_INDEX to
    // 0xE0000000 and GRBM_GFX_CNTL to 0 - Linux's own "select everything,
    // select nothing special" defaults - re-reads CP_ME_CNTL, writes 0 to
    // it, and reads back. The GRBM registers are then restored to whatever
    // Apple had, because changing what Apple's later code sees is not part
    // of this question.
    //
    // CP_ME_CNTL is deliberately NOT restored if the write lands: an
    // un-halted CP is the entire point. Say so plainly rather than hiding
    // a chip-state change in a restore.
    if (phase == 0 && g_hw.rreg && g_hw.wreg) {
        const uint32_t kGCNTL = (0x1260u + 0x0dc2u) * 4;   // seg0
        const uint32_t kGIDX  = (0xa000u + 0x2200u) * 4;   // seg1
        const uint32_t kMEb   = (0x1260u + 0x0F56u) * 4;   // seg0

        uint32_t gc = g_hw.rreg(g_hw.ctx, kGCNTL);
        uint32_t gi = g_hw.rreg(g_hw.ctx, kGIDX);

        char g[352];
        int n = snprintf(g, sizeof(g),
                 "cntl=%x me=%u pipe=%u q=%u vm=%u | idx=%x se=%u sa=%u inst=%u "
                 "bcast se=%u sa=%u inst=%u | %s",
                 gc, (gc >> 2) & 3u, gc & 3u, (gc >> 8) & 7u, (gc >> 4) & 0xfu,
                 gi, (gi >> 16) & 0xffu, (gi >> 8) & 0xffu, gi & 0xffu,
                 (gi >> 31) & 1u, (gi >> 29) & 1u, (gi >> 30) & 1u,
                 (gc == 0 && gi == 0xE0000000u)
                     ? "DEFAULT - routing is not the problem"
                     : "NOT DEFAULT - CP access is being misrouted");

        if (checkKernelArgument("-rbgpugrbmfix")) {
            uint32_t me_before = g_hw.rreg(g_hw.ctx, kMEb);
            g_hw.wreg(g_hw.ctx, kGIDX, 0xE0000000u);
            g_hw.wreg(g_hw.ctx, kGCNTL, 0);
            uint32_t gi2 = g_hw.rreg(g_hw.ctx, kGIDX);
            uint32_t gc2 = g_hw.rreg(g_hw.ctx, kGCNTL);
            uint32_t me_sel = g_hw.rreg(g_hw.ctx, kMEb);
            g_hw.wreg(g_hw.ctx, kMEb, 0);
            uint32_t me_after = g_hw.rreg(g_hw.ctx, kMEb);
            g_hw.wreg(g_hw.ctx, kGIDX, gi);                // restore Apple's
            g_hw.wreg(g_hw.ctx, kGCNTL, gc);
            snprintf(g + n, sizeof(g) - static_cast<size_t>(n),
                     " || FIX idx->%x cntl->%x | me %x sel=%x wrote0=%x %s",
                     gi2, gc2, me_before, me_sel, me_after,
                     (me_after == 0) ? "*** TOOK ***" : "still refused");
        }

        rbgpu_nvram_note("rbgpu-grbm", g);
        rbgpu_log("2jn: %s", g);
        rbgpu_log("2jn: cntl!=0 or idx!=E0000000 means Apple left the GRBM "
                  "selecting a specific ME/pipe/queue or a specific shader "
                  "engine, and every CP register access since has gone "
                  "there. On a 1-SE part addressed as a 4-SE part that is "
                  "nowhere. me sel differing from me before would show the "
                  "misrouting directly. TOOK means the CP is un-halted and "
                  "it was never protected at all.");
    }

    // 2jm (2026-10-19): what is actually AT the bootload address?
    //
    // 2jk found RLC_HYP_BOOTLOAD_ADDR already programmed - 0x8_7F80_0000,
    // size 0x6000, a real address 8 MB below the top of the carve-out
    // (0x7_8000_0000 .. 0x8_8000_0000), with COLD_BOOT_EXIT set. So
    // something DID point the RLC at an autoload bundle and the machinery
    // is half-configured rather than absent. `complete=0` is therefore NOT
    // "nobody pointed it anywhere".
    //
    // The next question is whether the buffer it points at holds anything.
    // The RLC fetches its own G ucode from there; if the page is empty or
    // holds something that is not microcode, the RLC has nothing to boot
    // from and the latch can never set - and that WOULD be a complete
    // explanation with a clear fix.
    //
    // Read-only. Maps the page named by the registers - never a hardcoded
    // address - with the same IOMemoryDescriptor::withPhysicalAddress +
    // kIOMapInhibitCache route 2hx and 2hy already use to read GPU-visible
    // memory successfully. Dumps the first eight dwords, counts nonzero
    // over the page, and compares the opening bytes against our own
    // yc_rlc_img so "is this OUR blob" is answered rather than guessed.
    //
    //   nz=0            the RLC is pointed at an empty buffer - nothing to
    //                   boot, and the latch cannot set
    //   nz>0, ymatch=1  our yellow_carp RLC image is in place and the
    //                   buffer is NOT the problem
    //   nz>0, ymatch=0  something else is there - whose, and why, becomes
    //                   the question
    if (phase == 0 && g_hw.rreg) {
        const uint32_t kS1 = 0xa000u;
        uint32_t blo = g_hw.rreg(g_hw.ctx, (kS1 + 0x5b5du) * 4);
        uint32_t bhi = g_hw.rreg(g_hw.ctx, (kS1 + 0x5b5eu) * 4);
        uint32_t bsz = g_hw.rreg(g_hw.ctx, (kS1 + 0x5b5cu) * 4);
        uint64_t bpa = (static_cast<uint64_t>(bhi) << 32) | blo;

        char n[288];
        if (bpa == 0) {
            snprintf(n, sizeof(n), "bootload addr is 0 - nothing to map");
        } else {
            IOMemoryDescriptor *md = IOMemoryDescriptor::withPhysicalAddress(
                static_cast<IOPhysicalAddress>(bpa), 4096, kIODirectionIn);
            IOMemoryMap *mp = md ? md->map(kIOMapAnywhere | kIOMapInhibitCache)
                                 : nullptr;
            if (!mp) {
                snprintf(n, sizeof(n), "pa=%llx sz=%x MAP FAILED (md=%d)",
                         bpa, bsz, md ? 1 : 0);
                if (md) md->release();
            } else {
                auto dw = reinterpret_cast<volatile uint32_t *>(
                    mp->getVirtualAddress());
                uint32_t d[8];
                for (int i = 0; i < 8; i++) d[i] = dw[i];
                unsigned nz = 0;
                for (int i = 0; i < 1024; i++) if (dw[i]) nz++;

                extern const uint8_t yc_rlc_img[];
                auto yw = reinterpret_cast<const uint32_t *>(yc_rlc_img);
                int ymatch = 1;
                for (int i = 0; i < 8; i++) if (d[i] != yw[i]) { ymatch = 0; break; }

                snprintf(n, sizeof(n),
                         "pa=%llx sz=%x nz=%u/1024 ymatch=%d | "
                         "%08x %08x %08x %08x %08x %08x %08x %08x",
                         bpa, bsz, nz, ymatch,
                         d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7]);
                mp->release();
                md->release();
            }
        }
        rbgpu_nvram_note("rbgpu-blbuf", n);
        rbgpu_log("2jm: %s", n);
        rbgpu_log("2jm: nz=0 means the RLC is pointed at an empty buffer and "
                  "the latch CANNOT set. ymatch=1 means our yellow_carp RLC "
                  "image is in place and the buffer is not the problem. "
                  "ymatch=0 with nz>0 means something else is there.");
    }

    // 2jk (2026-10-19): the four RLC_HYP registers nobody has read.
    //
    // Straight from Linux's own gfx_v10_0.c, which this project may use
    // freely (GPL, docs/linux-ref, UPSTREAM.md). Two things in it change
    // the picture, and both are READ-ONLY to check.
    //
    // 1. gfx_v10_0_wait_for_rlc_autoload_complete waits on TWO conditions:
    //
    //        cp_status == 0  AND  BOOTLOAD_COMPLETE == 1
    //
    //    Every boot of ours reads CP_STAT at 0x90078600 / 0x94038600.
    //    Not zero. So the wait's FIRST half fails too, and "the latch
    //    never sets" may be the second symptom rather than the cause.
    //
    // 2. gfx_v10_0_rlc_backdoor_autoload_enable - the AMDGPU_FW_LOAD_
    //    RLC_BACKDOOR_AUTO path, a third load type neither 2am nor 2ix
    //    considered - programs the RLC to fetch the bundle itself:
    //
    //        RLC_HYP_BOOTLOAD_ADDR_LO/HI = autoload buffer + RLC_G offset
    //        RLC_HYP_BOOTLOAD_SIZE       = RLC_G size
    //
    //    and REFUSES to proceed unless
    //
    //        RLC_HYP_RESET_VECTOR & (COLD_BOOT_EXIT | VDDGFX_EXIT)
    //        RLC_CNTL.RLC_ENABLE_F32 == 0   ("RLC ROM should halt itself")
    //
    //    Boot 60 read RLC_CNTL = 1. Our RLC is RUNNING, which is the
    //    PSP-autoload state, not the backdoor state.
    //
    // So: if BOOTLOAD_ADDR reads 0, nothing ever pointed the RLC at an
    // autoload bundle and `complete=0` needs no further explanation. If
    // it is non-zero, something did, and the address is a lead of its own.
    // And RESET_VECTOR says whether the backdoor path is even available
    // on this part before a single byte is written.
    //
    // Offsets, all seg1, from docs/linux-ref/gc_10_3_0_offset.h:
    //   RLC_HYP_RESET_VECTOR     0x5b54   bit0 COLD_BOOT_EXIT
    //   RLC_HYP_BOOTLOAD_SIZE    0x5b5c   bit1 VDDGFX_EXIT
    //   RLC_HYP_BOOTLOAD_ADDR_LO 0x5b5d
    //   RLC_HYP_BOOTLOAD_ADDR_HI 0x5b5e
    //
    // READ-ONLY. No arg, no gate, no chip-state change - it costs four
    // register reads in a function that already reads a dozen, and it
    // rides every armed boot from here.
    if (phase == 0 && g_hw.rreg) {
        const uint32_t kS1 = 0xa000u;
        uint32_t rv  = g_hw.rreg(g_hw.ctx, (kS1 + 0x5b54u) * 4);
        uint32_t bsz = g_hw.rreg(g_hw.ctx, (kS1 + 0x5b5cu) * 4);
        uint32_t blo = g_hw.rreg(g_hw.ctx, (kS1 + 0x5b5du) * 4);
        uint32_t bhi = g_hw.rreg(g_hw.ctx, (kS1 + 0x5b5eu) * 4);
        uint32_t cnt = g_hw.rreg(g_hw.ctx, (kS1 + 0x4c00u) * 4);
        uint32_t cps = g_hw.rreg(g_hw.ctx, (0x1260u + 0x0f40u) * 4);

        char h[288];
        snprintf(h, sizeof(h),
                 "rv=%x cold=%u vddgfx=%u | bootload %llx sz=%x %s | "
                 "rlc_cntl=%x f32=%u | cp_stat=%x %s",
                 rv, rv & 1u, (rv >> 1) & 1u,
                 ((unsigned long long)bhi << 32) | blo, bsz,
                 (blo || bhi) ? "SET" : "ZERO - nothing pointed the RLC anywhere",
                 cnt, cnt & 1u,
                 cps, (cps == 0) ? "idle (Linux's wait would pass this half)"
                                 : "NOT ZERO - Linux's wait fails here too");
        rbgpu_nvram_note("rbgpu-hyp", h);
        rbgpu_log("2jk: %s", h);
        rbgpu_log("2jk: bootload ZERO means no autoload bundle was ever "
                  "pointed at and complete=0 is explained. cold/vddgfx "
                  "both 0 means Linux's backdoor path would refuse this "
                  "part outright. f32=1 means the RLC is running, which "
                  "is the PSP-autoload state and NOT what the backdoor "
                  "path expects.");
    }

    // 2ji (2026-10-19): is CP_ME_CNTL write-PROTECTED, or does the CP
    // RE-HALT itself?
    //
    // 2jh killed the owner. Stopping the RLC - RLC_CNTL 1 -> 0, confirmed,
    // clean restore - did not release the register. Every candidate with a
    // positive control behind it is excluded, including the seg0 write path
    // (rbgpu-ctxw and rbgpu-pdc show seg0 GCVM writes landing with matching
    // readback in the same boot and the same function).
    //
    // What is left splits in two, and the two are indistinguishable from
    // everything measured so far:
    //
    //   PROTECTED    the write never reaches the register
    //   RE-HALTS     the write lands and hardware re-asserts CE/PFP/ME
    //                before any readback sees it. A CP with no valid
    //                firmware entry point would do precisely this, and it
    //                fits 2it - instruction RAM holds nonzero dwords, but
    //                nothing ever established a usable entry point.
    //
    // Two cells, and cell B does not depend on catching a race:
    //
    //   A  write 0, then read back EIGHT times in a tight loop. If any
    //      sample reads 0, or anything other than the entry value, the
    //      write reached the register and the question is answered
    //      outright - and the sample index dates the re-assert.
    //   B  write `was | CE_STEP`. Bit 25 is an ordinary R/W single-step
    //      debug bit sitting beside the halt bits in the same register,
    //      and nothing steps a halted engine. If bit 25 STICKS while the
    //      halt bits do not, the register accepts writes and the halt
    //      bits specifically are being forced - which is RE-HALTS. If
    //      bit 25 does not stick either, the whole register is
    //      PROTECTED.
    //
    // B is the one that settles it, because it needs no timing luck.
    //
    // Chip-state cost: one write of a step bit to a halted engine, and a
    // write of 0 to the halt bits that Apple and HWLibs both perform
    // three times a boot anyway. The entry value is restored and the
    // restore is read back, so a failed restore cannot pass as a clean
    // one. Behind -rbgpumehold, OUT by default.
    if (phase == 0 && g_hw.rreg && g_hw.wreg &&
        checkKernelArgument("-rbgpumehold")) {
        const uint32_t kMEb = (0x1260u + 0x0F56u) * 4;
        const uint32_t kSTEP = 0x02000000u;          // CE_STEP, bit 25
        uint32_t was = g_hw.rreg(g_hw.ctx, kMEb);

        uint32_t a[8];
        g_hw.wreg(g_hw.ctx, kMEb, 0);
        for (int i = 0; i < 8; i++) a[i] = g_hw.rreg(g_hw.ctx, kMEb);
        int moved = -1;
        for (int i = 0; i < 8; i++) if (a[i] != was) { moved = i; break; }

        g_hw.wreg(g_hw.ctx, kMEb, was | kSTEP);
        uint32_t b = g_hw.rreg(g_hw.ctx, kMEb);

        g_hw.wreg(g_hw.ctx, kMEb, was);
        uint32_t back = g_hw.rreg(g_hw.ctx, kMEb);

        char m[320];
        snprintf(m, sizeof(m),
                 "was=%x | A %x %x %x %x %x %x %x %x moved=%d | "
                 "B step %x %s | restore %x %s",
                 was, a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], moved,
                 b, (b & kSTEP) ? "STUCK" : "did not stick",
                 back, (back == was) ? "OK" : "FAILED");
        rbgpu_nvram_note("rbgpu-mehold", m);
        rbgpu_log("2ji: %s", m);
        rbgpu_log("2ji: B STUCK means the register ACCEPTS writes and only "
                  "the halt bits are forced - the CP re-halts itself, and "
                  "the fix is Linux's order: load ucode through "
                  "CP_*_UCODE_ADDR/DATA, THEN un-halt. B not stuck with "
                  "A never moving means the whole register is protected by "
                  "something still unnamed. Any A sample differing from was "
                  "answers it outright and dates the re-assert.");
    }

    // 2jf (2026-10-19): STOP THE RLC, THEN WRITE. The deferred experiment,
    // now due.
    //
    // Deferred before 2iy on the grounds that the un-halt might never run.
    // 2jb settled that: it runs once at 236 ms, through Apple's own ext2
    // write path, and CP_ME_CNTL does not move. So the question is no
    // longer "does anyone try" but "who is holding the register", and this
    // is the test that names them.
    //
    // Linux's direct-load path does not merely enter safe mode - it STOPS
    // the RLC. gfx_v10_0_rlc_stop writes RLC_CNTL = 0, clearing
    // RLC_ENABLE_F32, and only then does cp_gfx_enable touch CP_ME_CNTL.
    // 2iv forced RLC_SAFE_MODE to 0 and the write was still refused, but
    // safe mode is a handshake with a RUNNING RLC; this switches the F32
    // engine off entirely. Different thing, never tried.
    //
    //   me 15000000 -> 0   the RLC owned it. The link 2ix INFERRED from
    //                      Linux is then MEASURED on this chip, and the
    //                      road is a full host-side CP load.
    //   me unchanged       the RLC is not the gate. 2ix's root cause is
    //                      well-supported and wrong about the mechanism,
    //                      which redirects everything.
    //
    // Honest about the risk: this switches off the RLC's F32 engine on a
    // live GPU. The GPU retires no work as it is and DCN is a separate
    // block - but it is a chip-state
    // change, so it is behind its own arg and OUT by default. RLC_CNTL is
    // restored before the function returns, and the restore is read back
    // so a failed restore cannot be mistaken for a clean one.
    //
    // seg1 g_hw writes are PROVEN to work here: 2iw's control cell A wrote
    // SCRATCH_REG0 (seg1 0x2040) and it took. RLC_CNTL is seg1 too, so the
    // stop itself is on a path with a positive control behind it.
    if (phase == 0 && g_hw.rreg && g_hw.wreg &&
        checkKernelArgument("-rbgpurlcstop")) {
        const uint32_t kRLCC = (0xa000u + 0x4c00u) * 4;
        const uint32_t kMEb  = (0x1260u + 0x0F56u) * 4;

        uint32_t rlc_was = g_hw.rreg(g_hw.ctx, kRLCC);
        uint32_t me_was  = g_hw.rreg(g_hw.ctx, kMEb);

        g_hw.wreg(g_hw.ctx, kRLCC, 0);
        uint32_t rlc_off = g_hw.rreg(g_hw.ctx, kRLCC);

        void **wvt = *reinterpret_cast<void ***>(obj);
        auto wracc = reinterpret_cast<void (*)(void *, uint32_t, uint32_t)>(
            wvt ? wvt[0x120 / sizeof(void *)] : nullptr);
        uint32_t me_off = resolve(obj, RB_2HM_CP_ME_CNTL);

        if (wracc) wracc(obj, me_off, 0);
        uint32_t me_acc = g_hw.rreg(g_hw.ctx, kMEb);
        g_hw.wreg(g_hw.ctx, kMEb, 0);
        uint32_t me_dir = g_hw.rreg(g_hw.ctx, kMEb);

        g_hw.wreg(g_hw.ctx, kRLCC, rlc_was);          // restore
        uint32_t rlc_back = g_hw.rreg(g_hw.ctx, kRLCC);
        uint32_t me_after = g_hw.rreg(g_hw.ctx, kMEb);

        char r[320];
        snprintf(r, sizeof(r),
                 "rlc %x->%x %s | me %x acc=%x dir=%x %s | "
                 "restore %x %s | me_after %x",
                 rlc_was, rlc_off, (rlc_off == 0) ? "STOPPED" : "REFUSED",
                 me_was, me_acc, me_dir,
                 (me_acc == 0 || me_dir == 0) ? "TOOK" : "STILL REFUSED",
                 rlc_back, (rlc_back == rlc_was) ? "OK" : "FAILED",
                 me_after);
        rbgpu_nvram_note("rbgpu-rstop", r);
        rbgpu_log("2jf: %s", r);
        rbgpu_log("2jf: me TOOK with the RLC stopped means the RLC was "
                  "holding CE/PFP/ME and 2ix's inferred link is measured. "
                  "STILL REFUSED means the RLC is not the gate and the "
                  "root cause needs rethinking. rlc REFUSED means the stop "
                  "itself did not land and the cell says nothing.");
    }

    // 2ix (2026-10-18): did the RLC autoload handshake ever COMPLETE?
    //
    // 2iw closed the confound and vindicated the instrument:
    //   A  g_hw -> SCRATCH_REG0        0 -> 0xa5a5a5a5   TOOK
    //   B  accessor -> CP_ME_CNTL      refused, confirmed on both reads
    //   C  g_hw -> CP_ME_CNTL          refused
    // and the seg0 control comes free from 2il/2ip, whose accessor writes
    // to the GCVM context bases at 0x28C7 took with verified read-back in
    // these same boots. Same path, same segment, two registers: one
    // accepts, one refuses. So CP_ME_CNTL genuinely refuses writes from
    // EVERYBODY, Apple included, and 2iu/2iv's eliminations stand -
    // power, soft reset, RLCG and safe mode are all out.
    //
    // Which leaves the one mechanism that owns those bits on an APU.
    // The CP here is not started by the host: the PSP places the microcode
    // and then the RLC runs a bootload handshake, and **the RLC** releases
    // CE/PFP/ME. Until that handshake completes the RLC owns the halt bits
    // and host writes are ignored - by anyone, which is exactly what
    // 2iw measured.
    //
    // 2am patched away the wait for BOOTLOAD_COMPLETE in
    // RLC_RLCS_BOOTLOAD_STATUS because it was timing out. 2it proved the
    // microcode IS present, so 2am was right that the firmware arrives -
    // but "the firmware arrived" and "the handshake that releases the CP
    // finished" are different claims, and only the first was ever checked.
    // We told the driver the second was true as well.
    //
    // That is also a textbook Navi21-versus-Rembrandt difference: a dGPU
    // does not start its CP this way, and Apple's PSP path is Navi21's.
    //
    //   0x4e8d RLC_RLCS_BOOTLOAD_STATUS   seg1, BASE_IDX 1
    //     bit 0  RLC_RLCG_IRAM_LOADED
    //     bit 31 BOOTLOAD_COMPLETE
    //
    //   iram=1 complete=0  the PSP loaded the instruction RAM and the
    //                      bootload NEVER completed. The RLC still owns the
    //                      CP, which is why no one can un-halt it, and 2am
    //                      has been reporting a handshake that never
    //                      happened. Root cause.
    //   complete=1         the handshake finished and the RLC released the
    //                      CP, so the refusal is something else again and
    //                      this line is dead too
    //   iram=0 complete=0  the RLC never even saw the microcode the CP
    //                      instruction RAM demonstrably contains, which
    //                      would mean the PSP bypassed the RLC entirely
    if (g_hw.rreg) {
        uint32_t bl = g_hw.rreg(g_hw.ctx, (0xa000u + 0x4E8Du) * 4);
        char y[96];
        snprintf(y, sizeof(y), "p%u bl=%x iram=%u complete=%u",
                 phase, bl, bl & 1u, (bl >> 31) & 1u);
        size_t yh = strlen(g_rb_2ix_note);
        snprintf(g_rb_2ix_note + yh, sizeof(g_rb_2ix_note) - yh,
                 "%s%s", yh ? " | " : "", y);
        rbgpu_nvram_note("rbgpu-bl", g_rb_2ix_note);
        rbgpu_log("2ix: %s", y);
        rbgpu_log("2ix: complete=0 means the RLC never finished the bootload "
                  "handshake, still owns CE/PFP/ME, and ignores every write "
                  "to CP_ME_CNTL including Apple's - which is every symptom "
                  "since 2hl from one cause, and makes 2am's bypass the "
                  "place the truth was lost rather than the place it was "
                  "found.");
    }

    // 2hq: the context registers, after the ring note is safely committed so
    // that a stall in the new reads cannot cost us the old ones.
    rbgpu_vmctx_probe_2hq(obj, resolve, rd, phase);
    rbgpu_aperture_probe_2ik(obj, resolve, rd, phase);
    {
        void **rv3 = *reinterpret_cast<void ***>(obj);
        auto wr3 = reinterpret_cast<void (*)(void *, uint32_t, uint32_t)>(
            rv3 ? rv3[0x120 / sizeof(void *)] : nullptr);
        if (wr3) rbgpu_pdcandidates_2il(obj, resolve, rd, wr3, phase);
    }

    // 2ig: the write, after the contexts have been read at this phase so the
    // before-state is on record from the same sample.
    {
        void **rvt2 = *reinterpret_cast<void ***>(obj);
        auto wrfn = reinterpret_cast<void (*)(void *, uint32_t, uint32_t)>(
            rvt2 ? rvt2[0x120 / sizeof(void *)] : nullptr);
        if (wrfn) {
            rbgpu_ptwrite_2ig(obj, resolve, rd, wrfn, phase);
        } else if (phase == 1) {
            rbgpu_nvram_note("rbgpu-ptw", "REFUSED the accessor write slot is "
                                          "NULL - nothing was written");
        }
    }

    // 2hu: commit the fault tally at a known time, whatever happened. A boot
    // with no faults and a boot where the hooks never took must not look the
    // same in NVRAM.
    rbgpu_fault_flush_2hu();

    // 2ic: who wrote the context page-directory bases, same discipline.
    rbgpu_ctxwrite_flush_2ic();
    rbgpu_gvmwrite_flush_2ie();
    rbgpu_pdefix_flush_2ki();
    rbgpu_microengine_flush_2iy();
    rbgpu_autoload_flush_2jc();
    rbgpu_vmid_flush_2if();
    rbgpu_ptfix_flush_2ih();
    rbgpu_restart_flush_2iq();
    rbgpu_mecntl_flush_2ir();

    // 2hw: the PM4 itself, at the phase where the hang is established. Last,
    // after every other note is committed, because it is the newest and
    // riskiest thing here (2fo: when a measurement and a suspect are both
    // new, the suspect goes last).
    // 2kg (2026-10-20): ALSO at phase 0.
    //
    // Conceded to the reviewer: this dump ran only at phase 1 = +100 s,
    // roughly 91 s after the 8.7 s halt, so the ring it scanned was never
    // established to be the ring the CP was executing - which makes 2jz's
    // `noME` weaker than even 2kb allowed. Phase 0 is +12 s: still after the
    // halt, but 88 s closer, and before the later phases where rptr was seen
    // to move. The note records which phase wrote it so the two can never be
    // confused.
    if (phase == 0 || phase == 1) {
        g_rb_2kg_phase = phase;
        uint64_t rb_lo = rd(obj, resolve(obj, RB_2HM_CP_RB0_BASE));
        uint64_t rb_hi = rd(obj, resolve(obj, RB_2HM_CP_RB0_BASE_HI));
        uint64_t ring_va = ((rb_hi << 32) | rb_lo) << 8;
        uint32_t rp = rd(obj, resolve(obj, RB_2HK_CP_RB0_RPTR));
        uint32_t wp = rd(obj, resolve(obj, RB_2HK_CP_RB0_WPTR));
        rbgpu_ringdump_2hw(ring_va, rp, wp);
    }

    // The comparison the probe exists for, stated as a question because a
    // single sample decides nothing (2fs: no tables from n=1 rows).
    rbgpu_log("2hk: vmf NONZERO = a VM protection fault is latched and vmalo/vmahi "
              "name the address - that is the mechanism, no further inference "
              "needed. vmf ZERO = no fault was ever latched, and the CP is "
              "stalled on something inside the packet stream instead, which "
              "the dwords at rptr would name. r and w ACROSS the two samples "
              "say whether anything moved at all.");
}
