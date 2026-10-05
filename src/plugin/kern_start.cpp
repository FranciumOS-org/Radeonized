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

static kprintf_t orig_kprintf = nullptr;
static iolog_t   orig_iolog   = nullptr;

static bool g_safe_ttl_mode  = false;
static bool g_sync_570_mode  = false;
static bool g_spoof_ttl_mode = false;

struct DeviceTypeEntry {
    uint32_t deviceId;
    uint32_t deviceType;
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
    rbgpu_log("[Check 2] smu_cgs_alloc_memory ret=%d, handle=%p, gpu=0x%llx, cpu=%p",
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

    rb_smu13_allow(&g_smu, RB_SMU13_ALLOW_TABLES);
    rbgpu_log("rb_smu13 initialized with ALLOW_QUERY and ALLOW_TABLES");
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
        rbgpu_log("ttl_params[0xA0]:      0x%016llx",
                  params[20]);
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
    if (patchDeviceTypeTableAt(tbl_addr)) {
        return true;
    }

    return false;
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
    g_safe_ttl_mode  = safeTtlMode;
    g_sync_570_mode  = sync570Mode;
    g_spoof_ttl_mode = spoofTtlMode;

    rbgpu_diag_set_num("smu_hook_routed", 0);
    rbgpu_diag_set_num("guard_routed", 0);
    rbgpu_diag_set_num("smu_hook_calls", 0);
    rbgpu_diag_set_num("guard_calls", 0);
    rbgpu_diag_set_num("dev_type_patched", 0);
    rbgpu_diag_set_num("dev_type_rb_id", 0);
    rbgpu_diag_set_num("dev_type_rb_type", 0);
    rbgpu_diag_set_num("ttl_init_routed", 0);
    rbgpu_diag_set_num("ttl_init_calls", 0);
    rbgpu_diag_set_num("ttl_init_this", 0);
    rbgpu_diag_set_num("ttl_init_ret", 0);
    rbgpu_diag_set_num("ttl_init_inp_routed", 0);
    rbgpu_diag_set_num("ttl_init_inp_calls", 0);
    rbgpu_diag_set_num("ttl_init_inp_this", 0);
    rbgpu_diag_set_num("ttl_init_inp_ret", 0);
    rbgpu_diag_set_num("ttl_set_fw_dir_routed", 0);
    rbgpu_diag_set_num("ttl_set_fw_dir_calls", 0);
    rbgpu_diag_set_num("ttl_set_fw_dir_this", 0);
    rbgpu_diag_set_num("ttl_set_fw_dir_ptr", 0);
    rbgpu_diag_set_num("ttl_set_fw_dir_post_570", 0);
    rbgpu_diag_set_num("ttl_set_fw_dir_post_958", 0);
    rbgpu_diag_set_num("ttl_sync_570_applied", 0);
    rbgpu_diag_set_num("ttl_raw_init_routed", 0);
    rbgpu_diag_set_num("ttl_raw_init_calls", 0);
    rbgpu_diag_set_num("ttl_raw_init_ret", 0);
    rbgpu_diag_set_num("ttl_raw_init_arg1", 0);
    rbgpu_diag_set_num("ttl_raw_init_arg2", 0);
    rbgpu_diag_set_num("ttl_is_supp_routed", 0);
    rbgpu_diag_set_num("ttl_is_supp_calls", 0);
    rbgpu_diag_set_num("ttl_is_supp_ret", 0);
    rbgpu_diag_set_num("ttl_param_q0", 0);
    rbgpu_diag_set_num("ttl_param_q1", 0);
    rbgpu_diag_set_num("ttl_param_q2", 0);
    rbgpu_diag_set_num("ttl_param_q3", 0);
    rbgpu_diag_set_num("ttl_param_q4", 0);
    rbgpu_diag_set_num("ttl_param_q5", 0);

    rbgpu_log("loaded (Milestone 2; SMU Hook: %s, Trap 3 Guard: %s, Capture: %s, SafeTTL: %s, Sync570: %s, SpoofTTL: %s)",
              noSmuHook ? "DISABLED" : "ENABLED",
              noTrap3Guard ? "DISABLED" : "ENABLED",
              noLogCapture ? "DISABLED" : "ENABLED",
              safeTtlMode ? "ENABLED" : "DISABLED",
              sync570Mode ? "ENABLED" : "DISABLED",
              spoofTtlMode ? "ENABLED" : "DISABLED");

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

void RGPU::processHWLibs(KernelPatcher &patcher, size_t id, mach_vm_address_t address, size_t size) {
    rbgpu_log("Patching com.apple.kext.AMDRadeonX6800HWLibs");

    patchDeviceTypeTable(patcher, id);

    KernelPatcher::RouteRequest ttlReqs[] = {
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
        )
    };

    if (patcher.routeMultiple(id, &ttlReqs[0], 1, address, size)) {
        rbgpu_diag_set_num("ttl_init_inp_routed", 1);
        rbgpu_log("HWLibs: hooked __ZN14AmdTtlServices10initializeEP30_TtlLibraryInitializationInput");
    } else {
        rbgpu_diag_set_num("ttl_init_inp_routed", 0);
        rbgpu_log("HWLibs: failed to hook __ZN14AmdTtlServices10initializeEP30_TtlLibraryInitializationInput");
    }

    if (patcher.routeMultiple(id, &ttlReqs[1], 1, address, size)) {
        rbgpu_diag_set_num("ttl_init_routed", 1);
        rbgpu_log("HWLibs: hooked __ZN14AmdTtlServices10initializeEv");
    } else {
        rbgpu_diag_set_num("ttl_init_routed", 0);
        rbgpu_log("HWLibs: failed to hook __ZN14AmdTtlServices10initializeEv");
    }

    if (patcher.routeMultiple(id, &ttlReqs[2], 1, address, size)) {
        rbgpu_diag_set_num("ttl_set_fw_dir_routed", 1);
        rbgpu_log("HWLibs: hooked __ZN14AmdTtlServices20setFirmwareDirectoryEP20AMDFirmwareDirectory");
    } else {
        rbgpu_diag_set_num("ttl_set_fw_dir_routed", 0);
        rbgpu_log("HWLibs: failed to hook __ZN14AmdTtlServices20setFirmwareDirectoryEP20AMDFirmwareDirectory");
    }

    if (patcher.routeMultiple(id, &ttlReqs[3], 1, address, size)) {
        rbgpu_diag_set_num("ttl_raw_init_routed", 1);
        rbgpu_log("HWLibs: hooked _ttl_initialize");
    } else {
        rbgpu_diag_set_num("ttl_raw_init_routed", 0);
        rbgpu_log("HWLibs: failed to hook _ttl_initialize");
    }

    if (patcher.routeMultiple(id, &ttlReqs[4], 1, address, size)) {
        rbgpu_diag_set_num("ttl_is_supp_routed", 1);
        rbgpu_log("HWLibs: hooked _ttlIsSupportedDevice");
    } else {
        rbgpu_diag_set_num("ttl_is_supp_routed", 0);
        rbgpu_log("HWLibs: failed to hook _ttlIsSupportedDevice");
    }

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

