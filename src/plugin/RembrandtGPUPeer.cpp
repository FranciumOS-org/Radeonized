
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <Headers/kern_util.hpp>
#include <mach/mach_time.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSBoolean.h>

#include "RembrandtGPUPeer.hpp"
#include "vbios_data.h"

#define super IOService

OSDefineMetaClassAndStructors(RembrandtGPUPeer, IOService)

#define RBGPU_RING_SIZE 512
#define RBGPU_LINE_MAX  384
// 2db: was 96, and the 2026-10-12 16:50 capture had exactly 96 entries - the
// table was full, so every new key (fw_calls, stamp_calls, setres_ret, ...)
// was silently dropped and read as "absent". Absent must mean absent, not
// "the table filled up", or the next conclusion drawn from it will be wrong.
#define RBGPU_MAX_DIAGS 320
#define RBGPU_BUF_SIZE  4096

struct LogEntry {
    uint64_t time;
    uint32_t count;
    char     line[RBGPU_LINE_MAX];
};

enum DiagType {
    DIAG_TYPE_NUM,
    DIAG_TYPE_STR
};

struct DiagEntry {
    char     key[32];
    DiagType type;
    uint64_t numVal;
    char     strVal[64];
};

static IOLock           *g_diagLock = nullptr;
static RembrandtGPUPeer *g_peer     = nullptr;

static LogEntry  g_logRing[RBGPU_RING_SIZE];
static size_t    g_logHead  = 0;
static size_t    g_logCount = 0;

static DiagEntry g_diags[RBGPU_MAX_DIAGS];
static size_t    g_diagCount = 0;

static LogEntry  s_snapLogs[RBGPU_RING_SIZE];
static DiagEntry s_snapDiags[RBGPU_MAX_DIAGS];
static size_t    s_snapHead      = 0;
static size_t    s_snapLogCount  = 0;
static size_t    s_snapDiagCount = 0;

static char      s_log_buf[RBGPU_BUF_SIZE];
static char      s_capture_buf[RBGPU_BUF_SIZE];
static char      s_line_scratch[RBGPU_LINE_MAX];

extern uint64_t g_guard_calls;

static void initDiagLock() {
    if (!g_diagLock) {
        IOLock *lock = IOLockAlloc();
        if (!OSCompareAndSwapPtr(nullptr, lock, reinterpret_cast<void * volatile *>(&g_diagLock))) {
            IOLockFree(lock);
        }
    }
}

static void rbgpu_diag_set_num_locked(const char *key, uint64_t val) {
    for (size_t i = 0; i < g_diagCount; i++) {
        if (strncmp(g_diags[i].key, key, sizeof(g_diags[i].key)) == 0) {
            g_diags[i].type   = DIAG_TYPE_NUM;
            g_diags[i].numVal = val;
            return;
        }
    }
    if (g_diagCount < RBGPU_MAX_DIAGS) {
        strncpy(g_diags[g_diagCount].key, key, sizeof(g_diags[g_diagCount].key) - 1);
        g_diags[g_diagCount].key[sizeof(g_diags[g_diagCount].key) - 1] = '\0';
        g_diags[g_diagCount].type   = DIAG_TYPE_NUM;
        g_diags[g_diagCount].numVal = val;
        g_diagCount++;
        return;
    }
    // 2dt: a full table used to drop new keys in silence, so "absent" became a
    // lie. 2db hit this at 96 and raised it to 192 with the note "absent has to
    // mean absent"; the 2ds boot filled 192 exactly and lost stamp_wb_e0 the
    // same way. Say so once, loudly, instead of letting the next reader infer
    // that a key was never set.
    static bool s_warned = false;
    if (!s_warned) {
        s_warned = true;
        rbgpu_log("*** DIAG TABLE FULL at %d entries - '%s' and every later key is "
                  "being DROPPED. Absent no longer means absent; raise "
                  "RBGPU_MAX_DIAGS.", RBGPU_MAX_DIAGS, key);
    }
}

static void rbgpu_log_append_locked(const char *line) {
    uint64_t now = mach_absolute_time();

    if (g_logCount > 0) {
        size_t lastIdx = (g_logHead - 1 + RBGPU_RING_SIZE) % RBGPU_RING_SIZE;
        if (strncmp(g_logRing[lastIdx].line, line, sizeof(g_logRing[lastIdx].line)) == 0) {
            g_logRing[lastIdx].count++;
            g_logRing[lastIdx].time = now;
            return;
        }
    }

    g_logRing[g_logHead].time  = now;
    g_logRing[g_logHead].count = 1;
    strncpy(g_logRing[g_logHead].line, line, sizeof(g_logRing[g_logHead].line) - 1);
    g_logRing[g_logHead].line[sizeof(g_logRing[g_logHead].line) - 1] = '\0';
    g_logHead = (g_logHead + 1) % RBGPU_RING_SIZE;
    if (g_logCount < RBGPU_RING_SIZE) {
        g_logCount++;
    }
}

static void rbgpu_process_multiline_locked(const char *tag, char *buffer) {
    char *cursor = buffer;
    while (*cursor) {
        char *eol = strchr(cursor, '\n');
        if (eol) {
            *eol = '\0';
        }

        size_t len = strnlen(cursor, RBGPU_LINE_MAX);
        while (len > 0 && (cursor[len - 1] == '\r' || cursor[len - 1] == ' ')) {
            cursor[len - 1] = '\0';
            len--;
        }

        if (len > 0) {
            if (tag && tag[0] != '\0') {
                snprintf(s_line_scratch, sizeof(s_line_scratch), "%s %s", tag, cursor);
                rbgpu_log_append_locked(s_line_scratch);
            } else {
                rbgpu_log_append_locked(cursor);
            }
        }

        if (!eol) {
            break;
        }
        cursor = eol + 1;
    }
}

void rbgpu_log(const char *fmt, ...) {
    initDiagLock();
    if (g_diagLock) {
        IOLockLock(g_diagLock);
        va_list args;
        va_start(args, fmt);
        vsnprintf(s_log_buf, sizeof(s_log_buf), fmt, args);
        va_end(args);

        IOLog("RembrandtGPU: %s\n", s_log_buf);
        rbgpu_process_multiline_locked(nullptr, s_log_buf);
        IOLockUnlock(g_diagLock);
    }

    if (g_peer) {
        g_peer->scheduleRefreshDiag();
    }
}

void rbgpu_log_captured_va(const char *tag, const char *fmt, va_list va, void (*orig_fn)(const char *, ...)) {
    initDiagLock();
    if (g_diagLock) {
        IOLockLock(g_diagLock);
        vsnprintf(s_capture_buf, sizeof(s_capture_buf), fmt, va);
        rbgpu_process_multiline_locked(tag, s_capture_buf);
        IOLockUnlock(g_diagLock);
    }

    if (orig_fn) {
        orig_fn("%s\n", s_capture_buf);
    }

    if (g_peer) {
        g_peer->scheduleRefreshDiag();
    }
}

void rbgpu_diag_set_num(const char *key, uint64_t val) {
    initDiagLock();
    if (g_diagLock) {
        IOLockLock(g_diagLock);
        rbgpu_diag_set_num_locked(key, val);
        IOLockUnlock(g_diagLock);
    }
    if (g_peer) {
        g_peer->scheduleRefreshDiag();
    }
}

void rbgpu_diag_set_str(const char *key, const char *val) {
    initDiagLock();
    if (g_diagLock) {
        IOLockLock(g_diagLock);
        for (size_t i = 0; i < g_diagCount; i++) {
            if (strncmp(g_diags[i].key, key, sizeof(g_diags[i].key)) == 0) {
                g_diags[i].type = DIAG_TYPE_STR;
                strncpy(g_diags[i].strVal, val, sizeof(g_diags[i].strVal) - 1);
                g_diags[i].strVal[sizeof(g_diags[i].strVal) - 1] = '\0';
                IOLockUnlock(g_diagLock);
                if (g_peer) g_peer->scheduleRefreshDiag();
                return;
            }
        }
        if (g_diagCount < RBGPU_MAX_DIAGS) {
            strncpy(g_diags[g_diagCount].key, key, sizeof(g_diags[g_diagCount].key) - 1);
            g_diags[g_diagCount].key[sizeof(g_diags[g_diagCount].key) - 1] = '\0';
            g_diags[g_diagCount].type = DIAG_TYPE_STR;
            strncpy(g_diags[g_diagCount].strVal, val, sizeof(g_diags[g_diagCount].strVal) - 1);
            g_diags[g_diagCount].strVal[sizeof(g_diags[g_diagCount].strVal) - 1] = '\0';
            g_diagCount++;
        }
        IOLockUnlock(g_diagLock);
    }
    if (g_peer) {
        g_peer->scheduleRefreshDiag();
    }
}

void rbgpu_diag_inc(const char *key) {
    initDiagLock();
    if (g_diagLock) {
        IOLockLock(g_diagLock);
        for (size_t i = 0; i < g_diagCount; i++) {
            if (strncmp(g_diags[i].key, key, sizeof(g_diags[i].key)) == 0) {
                if (g_diags[i].type == DIAG_TYPE_NUM) {
                    g_diags[i].numVal++;
                } else {
                    g_diags[i].type   = DIAG_TYPE_NUM;
                    g_diags[i].numVal = 1;
                }
                IOLockUnlock(g_diagLock);
                if (g_peer) g_peer->scheduleRefreshDiag();
                return;
            }
        }
        if (g_diagCount < RBGPU_MAX_DIAGS) {
            strncpy(g_diags[g_diagCount].key, key, sizeof(g_diags[g_diagCount].key) - 1);
            g_diags[g_diagCount].key[sizeof(g_diags[g_diagCount].key) - 1] = '\0';
            g_diags[g_diagCount].type   = DIAG_TYPE_NUM;
            g_diags[g_diagCount].numVal = 1;
            g_diagCount++;
        }
        IOLockUnlock(g_diagLock);
    }
    if (g_peer) {
        g_peer->scheduleRefreshDiag();
    }
}

bool RembrandtGPUPeer::init(OSDictionary *dictionary) {
    if (!super::init(dictionary)) {
        return false;
    }

    symSpecialAMDKey = OSSymbol::withCString("SpecialAMDKey");
    if (!symSpecialAMDKey) {
        return false;
    }

    postRegisterCall = thread_call_allocate(postRegisterAction, this);
    refreshDiagCall  = thread_call_allocate(refreshDiagAction, this);

    return postRegisterCall != nullptr && refreshDiagCall != nullptr;
}

void RembrandtGPUPeer::free() {
    if (g_peer == this) {
        g_peer = nullptr;
    }
    if (postRegisterCall) {
        thread_call_cancel(postRegisterCall);
        thread_call_free(postRegisterCall);
        postRegisterCall = nullptr;
    }
    if (refreshDiagCall) {
        thread_call_cancel(refreshDiagCall);
        thread_call_free(refreshDiagCall);
        refreshDiagCall = nullptr;
    }
    OSSafeReleaseNULL(symSpecialAMDKey);
    OSSafeReleaseNULL(accelerator);
    super::free();
}

// One-shot arming for the accelerator (2cs, 2026-10-12).
//
// Why: an armed boot runs the accelerator, and when powerUpHW fails the
// display machine stays registered with IOAcceleratorFamily2 and WindowServer
// never puts up a desktop (2cr proved that a refused start() does not undo
// it). Recovering by editing config.plist from another OS to add -rbgpuoff is
// not a workable loop. So the accelerator is gated on an NVRAM variable that
// this function consumes: a boot that breaks the desktop leaves the variable
// cleared, and the next restart comes up normally with no intervention.
//
//   arm:     sudo nvram rbgpu-arm=1    (then reboot)
//   disarm:  sudo nvram -d rbgpu-arm   (also works in single-user mode)
//
// Unarmed, the peer still publishes RBGPULoad and LoadHWServices, so
// HWServices and HWLibs load and their diagnostics are collected; only
// LoadAccelerator is withheld, which is what milestone 1 booted with (screen
// unchanged). -rbgpuarm forces armed without NVRAM, for a boot where the
// desktop is not wanted anyway.
//
// Caveat: if this setup uses OpenCore's emulated NVRAM, a variable cleared
// here may not survive a forced restart, in which case the next boot is armed
// again. Recover from single-user mode (Cmd+S) with `nvram -d rbgpu-arm` -
// no other OS needed. The log line below reports whether the clear took.
static bool rbgpuConsumeArming() {
    if (checkKernelArgument("-rbgpuarm")) {
        rbgpu_log("Peer: -rbgpuarm set; accelerator ARMED by boot-arg (NVRAM untouched)");
        return true;
    }

    IORegistryEntry *options = IORegistryEntry::fromPath("/options", gIODTPlane);
    if (!options) {
        rbgpu_log("Peer: /options not found; accelerator NOT armed");
        return false;
    }

    OSObject *val = options->getProperty("rbgpu-arm");
    bool armed = false;
    if (val) {
        // Any value counts except a literal "0": `nvram rbgpu-arm=1` stores
        // an OSData, so check the first byte rather than expecting a string.
        armed = true;
        if (auto data = OSDynamicCast(OSData, val)) {
            if (data->getLength() >= 1 &&
                *static_cast<const char *>(data->getBytesNoCopy()) == '0') {
                armed = false;
            }
        } else if (auto str = OSDynamicCast(OSString, val)) {
            if (str->isEqualTo("0")) {
                armed = false;
            }
        }
    }

    if (!armed) {
        rbgpu_log("Peer: rbgpu-arm not set; accelerator NOT armed (desktop stays on "
                  "IONDRVFramebuffer). Arm it with: sudo nvram rbgpu-arm=1");
        options->release();
        return false;
    }

    // Consume it before doing anything that can break this boot, and FLUSH
    // the removal to flash immediately.
    //
    // 2dg: removeProperty alone only changes IODTNVRAM's in-memory copy. The
    // flush used to happen later, in saveDiagToNvram's thread_call. The 2df
    // boot panicked inside the accelerator's start() before that ran, so the
    // removal never reached flash, the next boot was armed again, and it
    // panicked again - the boot loop of 2026-10-12 that cost a Windows trip.
    // An armed boot can panic at any point after this, so the disarm has to
    // be durable before the accelerator is ever let near the hardware.
    options->removeProperty("rbgpu-arm");
    if (auto sync = OSString::withCString("now")) {
        options->setProperty("IONVRAM-FORCESYNCNOW-PROPERTY", sync);
        sync->release();
    }
    bool cleared = options->getProperty("rbgpu-arm") == nullptr;
    rbgpu_log("Peer: rbgpu-arm set; accelerator ARMED for this boot only; "
              "one-shot clear %s",
              cleared ? "OK (next boot comes up normally)"
                      : "FAILED - still set, the next boot will be armed too; "
                        "disarm with `nvram -d rbgpu-arm` (single-user: Cmd+S)");
    options->release();
    return true;
}

bool RembrandtGPUPeer::start(IOService *provider) {
    if (checkKernelArgument("-rbgpuoff")) {
        rbgpu_log("Peer: disabled by -rbgpuoff; aborting start");
        return false;
    }

    if (!super::start(provider)) {
        return false;
    }

    pciDevice = OSDynamicCast(IOPCIDevice, provider);
    if (!pciDevice) {
        rbgpu_log("Peer: provider is not an IOPCIDevice");
        return false;
    }

    g_peer = this;

    auto name = OSString::withCString("ATY,Belknap");
    if (name) {
        pciDevice->setProperty("@0,name", name);
        name->release();
    }

    auto vbios = OSData::withBytes(vbios_data, vbios_data_size);
    if (vbios) {
        pciDevice->setProperty("ATY,bin_image", vbios);
        vbios->release();
    }

    pciDevice->setProperty("RBGPULoad", kOSBooleanTrue);
    pciDevice->setProperty("LoadHWServices", kOSBooleanTrue);

    // 2cs: LoadAccelerator only on an armed boot. Without it Apple's
    // accelerator personality does not match at all, so nothing can claim the
    // display and the machine always reaches a desktop.
    const bool armed = rbgpuConsumeArming();
    if (armed) {
        pciDevice->setProperty("LoadAccelerator", kOSBooleanTrue);
    }
    rbgpu_diag_set_num("armed", armed ? 1 : 0);

    // -rbgpunodisppanic: survive a failed display mode change instead of
    // panicking. IOAccelDisplayMachine::display_mode_did_change panics when
    // AMDAccelDisplayMachine::displayModeDidChange returns false, UNLESS bit
    // 0x20 of the accelerator's +0xc91 is set (IOAcceleratorFamily2 485.2,
    // 0x14e215c0). The one site that sets it is IOGraphicsAccelerator2::start
    // (0x14e4e165), when
    //   getProperty("IOPCITunnelled", gIOServicePlane,
    //               kIORegistryIterateRecursively|kIORegistryIterateParents)
    // finds the property: Apple's eGPU/Thunderbolt flag, since a hot-pluggable
    // GPU must not panic the kernel on a failed mode change. The lookup walks
    // the registry parents, so setting it here on the IOPCIDevice is enough;
    // no code patching. See docs/SPEC-PM4-DOSTART.md section 5.
    //
    // This is for surviving bring-up, not a permanent answer: IOPCITunnelled
    // also makes that start() read "Removable", and other code (IOPCIFamily,
    // AMDSupport, WindowServer) treats a tunnelled GPU differently for power
    // management, surface eviction and driving the built-in panel. Hence its
    // own boot-arg, off by default.
    if (checkKernelArgument("-rbgpunodisppanic")) {
        pciDevice->setProperty("IOPCITunnelled", kOSBooleanTrue);
        rbgpu_log("Peer: -rbgpunodisppanic -> IOPCITunnelled set (display_mode_did_change "
                  "returns false instead of panicking)");
    }

    registerService();
    pciDevice->registerService();

    rbgpu_log("Peer: attached to 0x16811002; properties set and service registered");
    scheduleRefreshDiag();
    return true;
}

void RembrandtGPUPeer::stop(IOService *provider) {
    if (g_peer == this) {
        g_peer = nullptr;
    }
    if (postRegisterCall) {
        thread_call_cancel(postRegisterCall);
    }
    if (refreshDiagCall) {
        thread_call_cancel(refreshDiagCall);
    }
    if (accelerator) {
        accelerator->release();
        accelerator = nullptr;
    }
    super::stop(provider);
}

void RembrandtGPUPeer::scheduleRefreshDiag() {
    if (refreshDiagCall) {
        thread_call_enter(refreshDiagCall);
    }
}

void RembrandtGPUPeer::refreshDiagAction(thread_call_param_t param0, thread_call_param_t param1) {
    auto self = static_cast<RembrandtGPUPeer *>(param0);
    if (self) {
        self->performRefreshDiag();
    }
}

// 2cu (2026-10-12): persist a compact diagnostic summary into NVRAM.
//
// Why: RBGPUDiag and RBGPULog live only in the running boot's IORegistry. On
// a boot that never reaches a desktop they are unreadable and then lost, and
// every such boot has cost a rescue without producing a single number.
// Single-user mode cannot write files on macOS 15 here (sealed system volume,
// Data volume unmounted) and deliberate panics are off the table, so NVRAM is
// what is left: it survives the boot, and `nvram rbgpu-diag` reads it from
// the next boot's desktop.
//
// Written on every diag refresh, so the last write wins and a boot that dies
// later still leaves the numbers from however far it got. Keys are the short
// ones that decide the next step; the full log still goes to the registry for
// boots that do reach a desktop.
//
// IONVRAM-FORCESYNCNOW-PROPERTY asks IODTNVRAM to flush immediately, so the
// value is committed before a power-cycle rather than sitting in the
// in-memory copy. Without it a forced restart can lose the write.
// 2ec (2026-10-12): THE HOT LIST, emitted first.
//
// The summary is a fixed 512 bytes and every milestone appended its new keys
// to the END of kRbgpuNvramKeys - which is exactly where truncation eats
// them. The 2eb boot's summary stopped mid-key at "mc", before every one of
// `vm_l2_cntl`, `vm_c0_cntl`, `vm_c0_ptb`, `vm_c0_start`, `vm_c0_end`,
// `vm_ring_in_range`: the six values that boot existed to produce. Same
// failure as the diag cap of 2db (96) and 2ds (192), in the other buffer.
//
// The buffer is NOT being enlarged. It is flushed with FORCESYNCNOW on every
// refresh, three variables are written per boot, and this project has already
// had its EFI corrupted once; spending more NVRAM to fix an ordering bug would
// be the wrong trade. Put the keys that decide the next step first instead.
//
// Rule: when a milestone starts, its keys go HERE; when it is answered, they
// move down into the main list.
static const char *const kRbgpuNvramHotKeys[] = {
    "armed",
    // 2ee: which candidate physical page holds the real page directory, and
    // do its entries carry the same zero vram_base_offset? This is the live
    // question; everything it needs for context comes with it.
    "pdbA_valid", "pdbA_nonzero", "pdbA_low", "pdbA_high", "pdbA_e0",
    "pdbB_valid", "pdbB_nonzero", "pdbB_low", "pdbB_high", "pdbB_e0",
    "pdbB_phys", "pdbA_mapped", "pdbB_mapped", "vm_c0_ptb", "gc_fb_off",
    // 2ef: the control - does reading carve-out physical memory work at all?
    "ctl_fb_mapped", "ctl_fb_nonzero", "ctl_fb_e0",
    // 2eg: where Apple's own VM allocator put each page-table block.
    "vmm_alloc_calls", "vmm_l1_lvl", "vmm_l1_e20", "vmm_l1_e28",
    "vmm_l2_lvl", "vmm_l2_e20", "vmm_l3_lvl", "vmm_l3_e20",
    // the fault that sent us here, at the point it appears
    "vmf_st_wait", "vmf_ad_wait",
    // 2eb/2ed are ANSWERED (ring VA in range; GC hub programmed and agreeing
    // with MMHUB) and have moved down into the main list, per the rule above.
};

static bool rbgpuIsHotKey(const char *key) {
    for (size_t i = 0; i < sizeof(kRbgpuNvramHotKeys) / sizeof(kRbgpuNvramHotKeys[0]); i++) {
        if (strcmp(kRbgpuNvramHotKeys[i], key) == 0) {
            return true;
        }
    }
    return false;
}

static const char *const kRbgpuNvramKeys[] = {
    "armed", "found_fb_routed", "found_fb_calls", "found_fb_refused",
    "ttl_raw_init_ret", "ttl_hw_init_ret", "gc_sw_init_ret",
    "pw_stage", "pw_ret",
    "setres_ret", "gfxmqd_ret", "mapq_calls", "mapq_fails",
    "wfav_calls", "wfav_bounded_forced", "wfav_ret",
    // 2dg: the SMU bringup's own numbers. 2df's whole purpose was to learn
    // these and they were not in this list, so the panic boot taught us
    // nothing about whether the SMU step worked.
    "smu_dma_mc_ok", "smu_bringup_armed", "bringup_err",
    "smu_nmsgs", "smu_writes", "smu_refused", "last_smu_err",
    "gfx_min", "gfx_max",
    // 2dj: why the GFX clock range is 0 - parked in GFXOFF, or never started.
    "gfxoff_status", "gfxoff_read_err",
    // 2dk: the CP/RLC state, read with the corrected addressing.
    "diag_rlc_stat", "diag_rlc_gpm", "diag_cp_stat",
    "diag_cp_me_cntl", "diag_cp_mec_cntl", "diag_cp_stat_seg0",
    // 2dl: seg0 proof + whether the MEC ever fetched a packet.
    "diag_rb0_cntl", "diag_rb0_cntl_s0", "diag_rb0_rptr", "diag_rb0_wptr",
    "diag_mec_hdr", "diag_grbm_status",
    // 2dm: is the KIQ queue live, and is the header dump a pattern?
    "diag_hqd_active", "diag_gfx_hqd_active", "diag_hqd_pq_base",
    "diag_hqd_doorbell", "diag_mec_hdr4",
    // 2dn: the HQD window selector and the queue the KIQ asked for.
    "diag_grbm_gfx_cntl", "kiq_spec0", "kiq_spec1", "kiq_spec2",
    "hqd_slot_found",
    // 2ds: the channel's own submitted/completed stamp counters.
    "stamp_submitted", "stamp_completed", "stamp_wb_e0",
    // 2du: which gate leaves channel+0xe0 at -1.
    "regchan_calls", "regchan_fails", "addchan_calls", "addchan_fails",
    // 2dv: is the KIQ channel itself among the registered / added ones.
    "kiq_in_regchan", "kiq_in_addchan", "regchan_seen_n", "addchan_seen_n",
    // 2dw: the KIQ's real hardware fence slot, and whether the GPU wrote it.
    "fence_slot_idx", "fence_slot_b0", "fence_is_self", "fence_val_before",
    "fence_val_after", "fence_scan_hits", "fence_scan_off", "fence_c0_off",
    // 2dx: the GPU-side address of the same writeback frame.
    "wb_gpu_frame0", "wb_gpu_slot", "wb_cpu_base", "wb_next_idx",
    "wb_cpu_arith_ok", "wb_gpu_hi", "wb_gpu_carveout",
    // 2dy: the fence WRITE_DATA packet and the field it targets.
    "fencepkt_calls", "fencepkt_hdr", "fencepkt_ctl", "fencepkt_dst",
    "fencepkt_data", "chan_c8", "chan_c8_matches_wb",
    // 2dz: did the fence write fault in GPUVM?
    "vmf_cntl", "vmf_status", "vmf_addr_lo", "vmf_addr_hi",
    "cp_cpc_status", "cp_cpf_status", "cpc_utcl1_status", "cpf_utcl1_status",
    // 2ea: the fault status per dump point, to date the latch.
    "vmf_st_gc", "vmf_st_kiq", "vmf_st_pre", "vmf_st_wait", "vmf_st_post",
    "vmf_ad_gc", "vmf_ad_kiq", "vmf_ad_pre", "vmf_ad_wait", "vmf_ad_post",
    // 2eb: the VMID 0 page-table context the ring must live in.
    "vm_l2_cntl", "vm_c0_cntl", "vm_c0_ptb", "vm_c0_start", "vm_c0_end",
    "vm_ring_in_range",
    "stamp_calls", "stamp_ret",
    // 2di: the carve-out read from MMHUB, the absolute MC address derived
    // from it, and whether the sw_exit handle fix fired. These decide the
    // next step, so they have to survive a boot that dies.
    "fb_base_reg", "fb_top_reg", "fb_regs_sane",
    "mc_abs_hi", "mc_abs_lo", "mc_abs_ok",
    "sw_exit_h340", "sw_exit_h340_cleared",
};

void RembrandtGPUPeer::saveDiagToNvram() {
    IORegistryEntry *options = IORegistryEntry::fromPath("/options", gIODTPlane);
    if (!options) {
        return;
    }

    // Short "k=v" pairs, only for keys that exist this boot. NVRAM values are
    // size-limited, so this stays well under 512 bytes.
    // Static, not stack: the kernel caps a frame at 1024 bytes and these
    // three buffers together are twice that. saveDiagToNvram only ever runs
    // on the single diag-refresh thread_call, so sharing them is safe.
    // 2ec: body is built hot-keys-first, and anything that does not fit is
    // COUNTED rather than silently dropped. `d=N` goes at the front, so a
    // truncated summary says so instead of just ending mid-key.
    static char body[512];
    static char buf[512];
    size_t len = 0;
    unsigned dropped = 0;
    body[0] = '\0';

    // Reserve room for the "d=NNN " prefix so the marker can never be the
    // thing that gets cut.
    const size_t kBodyLimit = sizeof(body) - 8;

    for (size_t pass = 0; pass < 2; pass++) {
        const char *const *list = pass == 0 ? kRbgpuNvramHotKeys : kRbgpuNvramKeys;
        size_t count = pass == 0
            ? sizeof(kRbgpuNvramHotKeys) / sizeof(kRbgpuNvramHotKeys[0])
            : sizeof(kRbgpuNvramKeys) / sizeof(kRbgpuNvramKeys[0]);

        for (size_t k = 0; k < count; k++) {
            const char *key = list[k];
            // The hot keys were already emitted in pass 0.
            if (pass == 1 && rbgpuIsHotKey(key)) {
                continue;
            }
            for (size_t i = 0; i < s_snapDiagCount; i++) {
                if (strncmp(s_snapDiags[i].key, key, sizeof(s_snapDiags[i].key)) != 0) {
                    continue;
                }
                if (s_snapDiags[i].type != DIAG_TYPE_NUM) {
                    break;
                }
                int n = snprintf(body + len, kBodyLimit - len, "%s%s=%llx",
                                 len ? " " : "", key, s_snapDiags[i].numVal);
                if (n > 0 && static_cast<size_t>(n) < kBodyLimit - len) {
                    len += static_cast<size_t>(n);
                } else {
                    body[len] = '\0';
                    dropped++;
                }
                break;
            }
        }
    }

    if (len == 0) {
        options->release();
        return;
    }

    snprintf(buf, sizeof(buf), "d=%u %s", dropped, body);

    // Say it out loud, once, the way 2dt made the diag cap say it. A silent
    // cap has now cost three debugging rounds across two buffers.
    static bool s_truncWarned = false;
    if (dropped != 0 && !s_truncWarned) {
        s_truncWarned = true;
        rbgpu_log("*** NVRAM SUMMARY FULL: %u keys did not fit in %zu bytes and were "
                  "DROPPED. The hot list survived; everything else may not. "
                  "Read RBGPUDiag from the registry (tools/plugin_check.sh) for "
                  "the full table.", dropped, sizeof(body));
    }

    // 2cw: rotate, do not clobber. The 2026-10-12 armed boot hung, and the
    // recovery boot that followed overwrote rbgpu-diag with its own
    // "armed=0" before the armed boot's numbers were ever read. On the first
    // write of each boot, move whatever is already stored to rbgpu-diag-prev,
    // so the last two boots are always readable:
    //     nvram rbgpu-diag        this boot
    //     nvram rbgpu-diag-prev   the boot before it
    // 2cz: rotate with a FRESH copy, and report whether it took. The 2cw
    // version passed the retained original object straight back to
    // setProperty under a new name and no `rbgpu-diag-prev` ever appeared in
    // /options, so something in that path fails silently. Copying the text
    // into a new OSString removes the only suspicious part; the `rot` diag
    // key then says on the next boot whether the variable was really created,
    // instead of leaving it to be guessed again.
    static bool s_rotated = false;
    if (!s_rotated) {
        s_rotated = true;
        static char oldbuf[sizeof(buf)];
        oldbuf[0] = '\0';
        if (OSObject *old = options->getProperty("rbgpu-diag")) {
            if (auto str = OSDynamicCast(OSString, old)) {
                strlcpy(oldbuf, str->getCStringNoCopy(), sizeof(oldbuf));
            } else if (auto dat = OSDynamicCast(OSData, old)) {
                unsigned int n = dat->getLength();
                if (n >= sizeof(oldbuf)) n = sizeof(oldbuf) - 1;
                memcpy(oldbuf, dat->getBytesNoCopy(), n);
                oldbuf[n] = '\0';
            }
        }
        if (oldbuf[0]) {
            if (auto prev = OSString::withCString(oldbuf)) {
                options->setProperty("rbgpu-diag-prev", prev);
                prev->release();
            }
            rbgpu_diag_set_num("rot", options->getProperty("rbgpu-diag-prev") ? 1 : 2);
        } else {
            rbgpu_diag_set_num("rot", 0);  // nothing stored to rotate
        }
    }

    if (auto val = OSString::withCString(buf)) {
        options->setProperty("rbgpu-diag", val);
        val->release();
    }

    // 2cz: and a tail of the log itself. An armed boot does not reach a
    // desktop - WindowServer blanks the panel once it starts, so even the
    // console text is gone and tools/plugin_check.sh cannot run - which has
    // meant reading results off photographs. The diag numbers alone do not
    // say which call failed; these lines do. Kept small on purpose: NVRAM
    // space is shared and limited, so this is the last few lines only.
    {
        static char log[1024];
        size_t llen = 0;
        log[0] = '\0';
        size_t want = 8;
        size_t have = s_snapLogCount < want ? s_snapLogCount : want;
        for (size_t i = s_snapLogCount - have; i < s_snapLogCount; i++) {
            size_t idx = (s_snapHead + RBGPU_RING_SIZE - s_snapLogCount + i) % RBGPU_RING_SIZE;
            int n = snprintf(log + llen, sizeof(log) - llen, "%s%s",
                             llen ? " | " : "", s_snapLogs[idx].line);
            if (n > 0 && static_cast<size_t>(n) < sizeof(log) - llen) {
                llen += static_cast<size_t>(n);
            } else {
                break;
            }
        }
        if (llen) {
            if (auto lv = OSString::withCString(log)) {
                options->setProperty("rbgpu-log", lv);
                lv->release();
            }
        }
    }

    // Flush now: a boot with no desktop ends in a power-cycle.
    if (auto sync = OSString::withCString("now")) {
        options->setProperty("IONVRAM-FORCESYNCNOW-PROPERTY", sync);
        sync->release();
    }
    options->release();
}

void RembrandtGPUPeer::performRefreshDiag() {
    if (!g_diagLock) {
        return;
    }

    IOLockLock(g_diagLock);
    rbgpu_diag_set_num_locked("guard_calls", g_guard_calls);

    s_snapHead      = g_logHead;
    s_snapLogCount  = g_logCount;
    memcpy(s_snapLogs, g_logRing, sizeof(s_snapLogs));

    s_snapDiagCount = g_diagCount;
    memcpy(s_snapDiags, g_diags, sizeof(s_snapDiags));
    IOLockUnlock(g_diagLock);

    OSArray *logArray = OSArray::withCapacity(static_cast<unsigned int>(s_snapLogCount));
    if (logArray) {
        char lineBuf[RBGPU_LINE_MAX + 32];
        for (size_t i = 0; i < s_snapLogCount; i++) {
            size_t idx = (s_snapHead - s_snapLogCount + i + RBGPU_RING_SIZE) % RBGPU_RING_SIZE;
            if (s_snapLogs[idx].count > 1) {
                snprintf(lineBuf, sizeof(lineBuf), "[%llu] (x%u) %s",
                         s_snapLogs[idx].time, s_snapLogs[idx].count, s_snapLogs[idx].line);
            } else {
                snprintf(lineBuf, sizeof(lineBuf), "[%llu] %s",
                         s_snapLogs[idx].time, s_snapLogs[idx].line);
            }
            OSString *osLine = OSString::withCString(lineBuf);
            if (osLine) {
                logArray->setObject(osLine);
                osLine->release();
            }
        }
        setProperty("RBGPULog", logArray);
        logArray->release();
    }

    OSDictionary *diagDict = OSDictionary::withCapacity(static_cast<unsigned int>(s_snapDiagCount));
    if (diagDict) {
        for (size_t i = 0; i < s_snapDiagCount; i++) {
            if (s_snapDiags[i].type == DIAG_TYPE_NUM) {
                OSNumber *num = OSNumber::withNumber(s_snapDiags[i].numVal, 64);
                if (num) {
                    diagDict->setObject(s_snapDiags[i].key, num);
                    num->release();
                }
            } else {
                OSString *str = OSString::withCString(s_snapDiags[i].strVal);
                if (str) {
                    diagDict->setObject(s_snapDiags[i].key, str);
                    str->release();
                }
            }
        }
        setProperty("RBGPUDiag", diagDict);
        diagDict->release();
    }

    // 2cu: and into NVRAM, so a boot with no desktop still yields its numbers.
    saveDiagToNvram();
}

void RembrandtGPUPeer::postRegisterAction(thread_call_param_t param0, thread_call_param_t param1) {
    auto self = static_cast<RembrandtGPUPeer *>(param0);
    if (self) {
        self->handlePostRegister();
    }
}

volatile uint32_t g_rb_peer_stage = 0;
volatile uint32_t g_rb_peer_ret06 = 0xFFFFFFFF;
volatile uint32_t g_rb_peer_ret1B = 0xFFFFFFFF;

void RembrandtGPUPeer::handlePostRegister() {
    g_rb_peer_stage |= 2;
    if (!accelerator || !symSpecialAMDKey) {
        rbgpu_log("handlePostRegister: missing accelerator or symbol reference");
        return;
    }

    uint32_t one = 1;

    rbgpu_log("handlePostRegister: sending Request 0x06 (PowerPlay Ready)");
    uint32_t reqPowerPlayReady = 0x06;
    IOReturn ret06 = accelerator->callPlatformFunction(symSpecialAMDKey, true, &reqPowerPlayReady, &one, nullptr, nullptr);
    rbgpu_log("handlePostRegister: Request 0x06 returned 0x%08X", ret06);
    rbgpu_diag_set_num("ret_0x06", static_cast<uint64_t>(ret06));
    g_rb_peer_ret06 = static_cast<uint32_t>(ret06);
    g_rb_peer_stage |= 4;

    rbgpu_log("handlePostRegister: sending Request 0x1B (Controller Enabled)");
    uint32_t reqControllerEnabled = 0x1B;
    IOReturn ret1B = accelerator->callPlatformFunction(symSpecialAMDKey, true, &reqControllerEnabled, &one, nullptr, nullptr);
    rbgpu_log("handlePostRegister: Request 0x1B returned 0x%08X", ret1B);
    rbgpu_diag_set_num("ret_0x1B", static_cast<uint64_t>(ret1B));
    g_rb_peer_ret1B = static_cast<uint32_t>(ret1B);
    g_rb_peer_stage |= 8;
}

IOReturn RembrandtGPUPeer::callPlatformFunction(const OSSymbol *functionName,
                                                bool waitForFunction,
                                                void *p1, void *p2,
                                                void *p3, void *p4) {
    if (functionName && symSpecialAMDKey && functionName->isEqualTo(symSpecialAMDKey)) {
        if (!p1) {
            rbgpu_log("SpecialAMDKey received NULL p1");
            return kIOReturnBadArgument;
        }

        uint32_t req = *reinterpret_cast<uint32_t *>(p1);
        char reqKey[16];
        snprintf(reqKey, sizeof(reqKey), "req_0x%02X", req);
        rbgpu_diag_inc(reqKey);

        // 2cw: request 0x14 (Gpu Status) is polled continuously by the
        // accelerator - the 2026-10-12 armed boot filled both the console and
        // the 512-line RBGPULog ring with it, pushing out the lines that
        // matter. Log the first few and then only every 64th.
        if (req != 0x14) {
            rbgpu_log("SpecialAMDKey request 0x%02X (wait: %d)", req, waitForFunction);
        } else {
            static uint32_t s_req14_seen = 0;
            s_req14_seen++;
            if (s_req14_seen <= 3 || (s_req14_seen % 64) == 0) {
                rbgpu_log("SpecialAMDKey request 0x14 (wait: %d) [#%u]",
                          waitForFunction, s_req14_seen);
            }
        }

        switch (req) {
            case 0x00:
                if (p2) {
                    OSSafeReleaseNULL(accelerator);
                    accelerator = OSDynamicCast(IOService, reinterpret_cast<OSObject *>(p2));
                    if (accelerator) {
                        accelerator->retain();
                        rbgpu_log("Registered accelerator instance: %s", accelerator->getName());
                        g_rb_peer_stage |= 1;
                        if (postRegisterCall) {
                            thread_call_enter(postRegisterCall);
                        }
                    }
                }
                return kIOReturnSuccess;

            case 0x0B:
                if (accelerator) {
                    rbgpu_log("Dropping registered accelerator: %s", accelerator->getName());
                    accelerator->release();
                    accelerator = nullptr;
                }
                return kIOReturnSuccess;

            case 0x14:
                if (p2) {
                    *reinterpret_cast<uint32_t *>(p2) = 1;
                    return kIOReturnSuccess;
                }
                return kIOReturnBadArgument;

            case 0x17:
                rbgpu_log("ASIC Reset State requested (unsupported)");
                return kIOReturnUnsupported;

            case 0x1A:
                rbgpu_log("Get DCC Capabilities requested (unsupported)");
                return kIOReturnUnsupported;

            default:
                rbgpu_log("Unhandled SpecialAMDKey request 0x%02X", req);
                return kIOReturnUnsupported;
        }
    }

    return super::callPlatformFunction(functionName, waitForFunction, p1, p2, p3, p4);
}

