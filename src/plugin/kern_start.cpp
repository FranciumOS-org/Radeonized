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

enum KextListIndex {
    KextHWLibs = 0,
    KextX6000,
    KextHWSvc
};

static KernelPatcher::KextInfo kextList[] = {
    { "com.apple.kext.AMDRadeonX6800HWLibs",     &kextPathHWLibs, 1, {true}, {}, KernelPatcher::KextInfo::Unloaded },
    { "com.apple.kext.AMDRadeonX6000",            &kextPathX6000,  1, {true}, {}, KernelPatcher::KextInfo::Unloaded },
    { "com.apple.kext.AMDRadeonX6000HWServices", &kextPathHWSvc,  1, {true}, {}, KernelPatcher::KextInfo::Unloaded }
};

static mach_vm_address_t g_kext_addr_hwlibs = 0;
static size_t            g_kext_size_hwlibs = 0;

static mach_vm_address_t g_kext_addr_x6000  = 0;
static size_t            g_kext_size_x6000  = 0;

static mach_vm_address_t g_kext_addr_hwsvc  = 0;
static size_t            g_kext_size_hwsvc  = 0;

static struct rb_smu13 g_smu;
static struct rb_hw    g_hw;
static struct rb_disc *g_disc       = nullptr;
static void           *g_smu_ctx    = nullptr;
static void           *g_dma_handle = nullptr;
static uint64_t        g_dma_gpu    = 0;
static void           *g_dma_cpu    = nullptr;

typedef int      (*smu_cgs_alloc_memory_t)(void *ctx, uint32_t size, uint32_t align, uint32_t domain, void **handle, uint64_t *gpu_addr, void **cpu_addr);
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

    int alloc_ret = orig_smu_cgs_alloc_memory(ctx, 0x1000, 0x1000, 1, &g_dma_handle, &g_dma_gpu, &g_dma_cpu);
    rbgpu_diag_set_num("alloc_ret", static_cast<uint64_t>(alloc_ret));
    rbgpu_diag_set_num("fb_mc_addr", g_dma_gpu);
    rbgpu_log("[Check 2] smu_cgs_alloc_memory ret=%d, handle=%p, gpu=0x%llx, cpu=%p (gpu is CGS IOVirtualAddress)",
              alloc_ret, g_dma_handle, g_dma_gpu, g_dma_cpu);

    if (alloc_ret != 0 || !g_dma_cpu) {
        rbgpu_log("Driver table memory allocation failed");
        rbgpu_diag_set_num("sw_init_ret", 2);
        return 2;
    }

    bool in_carveout = (g_dma_gpu >= 0xF400000000ULL && g_dma_gpu <= 0xF4FFFFFFFFULL);
    rbgpu_diag_set_num("carveout_ok", in_carveout ? 1 : 0);
    rbgpu_log("[Check 2] Carveout verification (0xF4_0000_0000..0xF4_FFFF_FFFF): %s", in_carveout ? "PASS" : "FAIL");

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
    dma.cpu  = g_dma_cpu;
    dma.mc   = g_dma_gpu;
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

    int err = rb_smu13_bringup(&g_smu);
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
    return 0;
}

static int slot_internal_sw_exit(void *ctx) {
    rbgpu_log("slot_internal_sw_exit entered");
    if (g_dma_handle && orig_smu_cgs_free_memory) {
        orig_smu_cgs_free_memory(ctx, g_dma_handle);
        g_dma_handle = nullptr;
        g_dma_cpu = nullptr;
        g_dma_gpu = 0;
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

static int hooked_gc_hw_init(void *gc_buf, void *arg2, void *arg3) {
    rbgpu_diag_inc("gc_hw_sub_calls");
    rbgpu_log("_gc_hw_init entered: gc_buf=%p, arg2=%p, arg3=%p", gc_buf, arg2, arg3);

    int ret = 1;
    if (orig_gc_hw_init) {
        ret = orig_gc_hw_init(gc_buf, arg2, arg3);
    }

    rbgpu_log("_gc_hw_init returned %d", ret);
    rbgpu_diag_set_num("gc_hw_sub_ret", ret);
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
    // 2af: read-only dump of the firmware entry descriptor (first 8 dwords),
    // before and after the original call, to learn its layout for the
    // yellow_carp firmware-replacement step. Nothing here is modified.
    if (fw_entry) {
        uint32_t *d = reinterpret_cast<uint32_t *>(fw_entry);
        rbgpu_log("_IpiSetFwEntry pre : instance=%p entry=%p id=%u dwords: %08x %08x %08x %08x %08x %08x %08x %08x",
                  instance, fw_entry, d[0],
                  d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7]);
    } else {
        rbgpu_log("_IpiSetFwEntry pre : instance=%p entry=NULL", instance);
    }
    if (orig_ipi_set_fw_entry) {
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
    } else if (kextList[KextHWSvc].loadIndex == id) {
        g_kext_addr_hwsvc = address;
        g_kext_size_hwsvc = size;
        rbgpu_log("HWSvc loaded at %p (size 0x%zx)", reinterpret_cast<void *>(address), size);
        processHWServices(patcher, id, address, size);
    }
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

    for (size_t i = 0; i < 0x2000; i++) {
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
}

void RGPU::processHWServices(KernelPatcher &patcher, size_t id, mach_vm_address_t address, size_t size) {
    rbgpu_log("com.apple.kext.AMDRadeonX6000HWServices matched and monitored");
}

