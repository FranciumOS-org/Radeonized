
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <Headers/kern_util.hpp>
#include <mach/mach_time.h>
#include <kern/clock.h>
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
#define RBGPU_MAX_DIAGS 448
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
static volatile bool     g_diagFullPending = false;  // 2fz: logged outside the lock
static RembrandtGPUPeer *g_peer     = nullptr;

static LogEntry  g_logRing[RBGPU_RING_SIZE];
static size_t    g_logHead  = 0;
static size_t    g_logCount = 0;
// 2fx: a monotonic count of every log ACTIVITY (new line or dedupe bump). Unlike
// g_logCount it never saturates at the ring size, so two samples of it say
// whether the chain is advancing at all - finer than the 6-value stage number,
// where the whole startHWEngines walk lives between two stages.
static volatile uint64_t g_rbgpuLogSeq = 0;

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
extern volatile int g_rb_2gn_in1b;
extern void rbgpu_2gx_req(uint32_t req, int refused);
extern void rbgpu_2gx_flush(void);
// 2gp: the event machine's stamp-wait counters (kern_start.cpp).
extern volatile uint32_t g_rb_2gp_fas_n;
extern volatile uint32_t g_rb_2gp_wfs_calls;
extern volatile uint32_t g_rb_2gp_wfs_first;
extern volatile uint32_t g_rb_2gp_wfs_last;
extern volatile uint32_t g_rb_2gp_restarts;
extern volatile uint32_t g_rb_2gq_stw;   // 2gn: 0x1B post-gate record armed

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
    // 2fz: this used to call rbgpu_log from here, and every caller of this
    // function holds g_diagLock. rbgpu_log takes g_diagLock too, and an IOLock
    // is NOT recursive: the same thread blocking on a lock it already holds
    // blocks FOREVER, with no panic and no log line. That is a permanent hard
    // stop of exactly the shape this milestone family has been chasing - and
    // it fires only when the table first reaches the cap, once per boot, which
    // would make it look non-deterministic.
    //
    // It is NOT what stopped the 2fy boot: that boot's table held 246 of 320
    // and "DIAG TABLE FULL" has never appeared in any log this project has
    // captured. It is a live bug found by reading, with the table now at 246
    // and climbing, and it is fixed here rather than left to fire later.
    g_diagFullPending = true;
    if (false) {
        rbgpu_log("*** DIAG TABLE FULL at %d entries - '%s' and every later key is "
                  "being DROPPED. Absent no longer means absent; raise "
                  "RBGPU_MAX_DIAGS.", RBGPU_MAX_DIAGS, key);
    }
}

static void rbgpu_log_append_locked(const char *line) {
    uint64_t now = mach_absolute_time();

    g_rbgpuLogSeq++;   // 2fx: counts dedupe bumps too, so a repeating line still shows progress

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
    if (g_diagFullPending) {
        g_diagFullPending = false;
        rbgpu_log("*** DIAG TABLE FULL at %d entries - keys are being DROPPED. "
                  "Absent no longer means absent; raise RBGPU_MAX_DIAGS.",
                  RBGPU_MAX_DIAGS);
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

// 2fa (2026-10-13): rate-limit the publisher.
//
// Measured, not inferred: in the 2ez boot the gaps between consecutive
// RBGPULog lines oscillate between 3 us and 10 ms within one boot, on the
// same straight-line code, while the 2ey boot's tail ran at 7-37 us. Every
// rbgpu_log call scheduled performRefreshDiag, which memcpy's 235 KB
// (512 x 400-byte log entries + 320 x 112-byte diag entries) under
// g_diagLock and then builds up to 512 OSStrings, an OSArray and a 320-entry
// OSDictionary and setProperty's both - twice per pass. With log lines
// arriving back to back the refresh runs continuously, and the start() thread
// blocks on g_diagLock for the snapshot every time it logs.
//
// A thread throttled to milliseconds per log line, with hundreds of lines
// still to come in the doStart chain, is indistinguishable from a wedge: it
// holds the IOService lock the whole time (which is why ioreg blocks and
// pci.txt is 0 bytes) and it stops at a different place in every boot.
//
// At most one refresh per 250 ms. The publisher exists to be read after the
// fact, so nothing needs it sooner.
static uint64_t g_lastRefreshAt = 0;

void RembrandtGPUPeer::scheduleRefreshDiag() {
    if (!refreshDiagCall) {
        return;
    }
    uint64_t now = mach_absolute_time();
    uint64_t interval = 0;
    nanoseconds_to_absolutetime(250ull * 1000ull * 1000ull, &interval);
    if (g_lastRefreshAt && (now - g_lastRefreshAt) < interval) {
        return;
    }
    g_lastRefreshAt = now;
    thread_call_enter(refreshDiagCall);
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
    // 2fq (2026-10-14): the frontier moved and this list did not follow it.
    //
    // Everything that was here - gate_stage, the ptb_*, stamp_*, gfxmqd_* and
    // mapq_* keys - belongs to a question the 2fo boot ANSWERED by running
    // the whole chain with the gate trail off: doStart returned 1, every
    // engine powered up, and the stop is now AMDHardware::startHWEngines
    // returning 0. Per the rule above they move down into the main list and
    // the new frontier's keys come first.
    //
    // Note what the last two captures say about this channel, because it is
    // the reason 2fq also writes `rbgpu-starthw` directly: the 2fo and 2fp
    // summaries are byte-identical and both end at `regchan_calls=19`, which
    // is hundreds of log lines before powerUp. Whatever stops the summary
    // advancing, these keys are a hope and the one-shot note is the record.
    "starthw_c2", "starthw_count", "starthw_null_idx", "starthw_badfn_idx",
    "pw_stage", "pw_ret",
    // the preconditions: if these are not right the rest means nothing.
    "ptb_written", "ptb_back", "vmf_st_wait", "stamp_ret",
    "setres_ret", "gfxmqd_ret", "mapq_calls", "mapq_fails",
    "gate_stage",
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

// 2et (2026-10-13): once the doStart gate chain starts, the synchronous
// `rbgpu-gate` breadcrumb is the only NVRAM record that matters, and it must
// not have to queue behind this thread. The 2es boot proved they contend:
// a refresh pass published RBGPULog with mark 51 in it and then never
// finished its own saveDiagToNvram (the summary still reads gate_stage=0x65 =
// 101, three gates stale), so the refresh thread was sitting inside the NVRAM
// transaction while the start() thread went on to need it at gate 54.
static volatile bool g_rbgpuNvramQuiesced = false;

void rbgpu_nvram_quiesce(void) {
    if (!g_rbgpuNvramQuiesced) {
        g_rbgpuNvramQuiesced = true;
        rbgpu_log("2et: NVRAM quiesced - the diag summary stops being flushed "
                  "here, so the synchronous rbgpu-gate breadcrumb has the "
                  "NVRAM controller to itself for the rest of the gate chain. "
                  "Everything already in rbgpu-diag stands; nothing later "
                  "reaches it.");
    }
}

void RembrandtGPUPeer::saveDiagToNvram() {
    // 2et: the gate chain owns NVRAM from gate 40 onward. The registry
    // properties (RBGPULog / RBGPUDiag) keep refreshing; only the flash
    // commit stops.
    if (g_rbgpuNvramQuiesced) {
        return;
    }

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

// 2eo (2026-10-13): a breadcrumb that does NOT go through the diag table.
//
// 2en's `gate_stage` reached NVRAM through rbgpu_diag_set_num, which only
// schedules performRefreshDiag on a thread_call. That worked for 2em because
// the refresh thread kept running after the accelerator thread wedged. The
// 2en boot shows it is not dependable: its summary is a snapshot of a table
// holding ~34 fewer keys than the 2em boot's, so the refresh stopped early
// and the last writes - including every gate_stage - never reached flash. A
// breadcrumb that can be starved by the very hang it is meant to locate is
// not a breadcrumb.
//
// So this writes one NVRAM variable, `rbgpu-gate`, on the CALLING thread,
// synchronously, flushed immediately. It overwrites, so what survives is the
// last gate the boot actually entered. Cost is one flash write per gate, a
// few dozen per armed boot; a wedge that costs a power cycle and teaches
// nothing costs more.
//
//   nvram rbgpu-gate
// 2eq (2026-10-13): resolve /options ONCE and keep it.
//
// 2ep's boot stopped between gate 53 and gate 54 - a span that contains no
// Apple code at all, only two vtable-slot reads, two diag sets and a log
// line. That makes the breadcrumb itself a suspect, and there is a concrete
// mechanism: rbgpu_gate_nvram called IORegistryEntry::fromPath on every mark,
// which walks the device-tree plane under the registry lock. Doing that
// dozens of times from inside IOService::start - the one thread that must not
// contend for the registry - is a deadlock shape, and it matches the symptom
// that a wedge takes `ioreg` and `reboot` down with it rather than stalling
// one thread. The 2eo boot (nine marks per dump point, system still alive
// enough to run plugin_check over SSH) and the 2ep boot (four more marks,
// hard wedge) are consistent with that, though two boots are a correlation
// and not a demonstration.
//
// So the lookup happens once, lazily, and the entry is retained for the life
// of the boot; later marks only setProperty on an entry already in hand.
// This is stated as a suspicion being removed, not a diagnosis: if the next
// boot stops in the same place with the lookup gone, the breadcrumb is
// exonerated and the fault is in the two pointer reads.
static IORegistryEntry *g_rbgpuOptions = nullptr;

// 2ey (2026-10-13): the control that 2eo-2ex never had.
//
// Every gate reading so far assumes "the trail stops where the THREAD stops".
// Nothing has tested that. The 2ex trail ended at gate 151 while the 2eu boot
// reached gate 54, which is three gates further on through the same
// straight-line code - so the stopping point is not located by code position,
// and an asynchronous death that merely catches the thread wherever it happens
// to be would produce exactly that. Those two possibilities need different
// fixes and look identical from a single thread's breadcrumb.
//
// So a second thread writes `rbgpu-beat` every 250 ms, independently of the
// start() thread, carrying its own counter and the last gate it saw the gate
// path write. The gate path holds g_rbgpuNvramBusy across its own write and
// has absolute priority; the heartbeat skips rather than block it, because a
// heartbeat that can stall the thread it is measuring is worse than none.
//
// But a heartbeat that goes silent whenever the flag is held cannot tell
// "the gate path is wedged INSIDE its own flash write" from "the machine is
// dead", so after three consecutive skips (~750 ms) it writes anyway and says
// so. Three outcomes, all distinguishable in one variable:
//
//   beat advances, skips low   the machine is alive and the start() thread
//                              alone is stuck; the trail DOES locate it
//   beat advances, forced=1    the thread is stuck inside the gate's own
//                              NVRAM write - the 2eq/2es suspicion, with
//                              direct evidence this time instead of inference
//   beat absent / frozen       the whole machine stopped, and the gate the
//                              trail ends at is incidental
static volatile UInt32  g_rbgpuNvramBusy = 0;
static volatile uint32_t g_rbgpuLastGate = 0;
static thread_call_t     g_rbgpuBeatCall = nullptr;
static volatile bool     g_rbgpuBeatOn   = false;
static uint32_t          g_rbgpuBeatN    = 0;
static uint32_t          g_rbgpuBeatSkips = 0;
static uint32_t          g_rbgpuBeatForced = 0;

static void rbgpu_beat_action(thread_call_param_t, thread_call_param_t) {
    g_rbgpuBeatN++;

    bool took = OSCompareAndSwap(0, 1, &g_rbgpuNvramBusy);
    bool forced = false;
    if (!took) {
        g_rbgpuBeatSkips++;
        if (g_rbgpuBeatSkips >= 3) {
            // The gate path has held the flag for ~750 ms. Either it is wedged
            // inside its own flash write or it is writing far more often than
            // the chain should. Either way that IS the finding, so record it.
            forced = true;
            g_rbgpuBeatForced++;
        }
    } else {
        g_rbgpuBeatSkips = 0;
    }

    if (took || forced) {
        IORegistryEntry *options = g_rbgpuOptions;
        if (options) {
            char buf[160];
            snprintf(buf, sizeof(buf),
                     "n=%u gate=%u skips=%u forced=%u t=%llu",
                     g_rbgpuBeatN, g_rbgpuLastGate, g_rbgpuBeatSkips,
                     g_rbgpuBeatForced, mach_absolute_time());
            if (auto str = OSString::withCString(buf)) {
                options->setProperty("rbgpu-beat", str);
                str->release();
            }
            if (auto sync = OSString::withCString("now")) {
                options->setProperty("IONVRAM-FORCESYNCNOW-PROPERTY", sync);
                sync->release();
            }
        }
        if (took) {
            g_rbgpuNvramBusy = 0;
        }
    }

    if (g_rbgpuBeatOn && g_rbgpuBeatCall) {
        uint64_t deadline = 0;
        clock_interval_to_deadline(250, kMillisecondScale, &deadline);
        thread_call_enter_delayed(g_rbgpuBeatCall, deadline);
    }
}

void rbgpu_heartbeat_start(void) {
    if (g_rbgpuBeatOn) {
        return;
    }
    // Resolve /options here if the gate path has not already done it, so the
    // beat does not depend on which write happens first.
    if (!g_rbgpuOptions) {
        g_rbgpuOptions = IORegistryEntry::fromPath("/options", gIODTPlane);
    }
    g_rbgpuBeatCall = thread_call_allocate(rbgpu_beat_action, nullptr);
    if (!g_rbgpuBeatCall) {
        rbgpu_log("2ey: thread_call_allocate FAILED - no heartbeat this boot, so "
                  "alive-vs-dead stays unanswered and the gate trail keeps its "
                  "assumption");
        return;
    }
    g_rbgpuBeatOn = true;
    rbgpu_log("2ey: heartbeat armed (250 ms, rbgpu-beat)");
    uint64_t deadline = 0;
    clock_interval_to_deadline(250, kMillisecondScale, &deadline);
    thread_call_enter_delayed(g_rbgpuBeatCall, deadline);
}

// 2fq (2026-10-14): one NVRAM variable, one write, no trail.
//
// The 2fp boot answered nothing because its only channel was RBGPULog, and
// that boot's `log show` came back empty - the same loss that cost 2fb, 2fe
// and 2ev a boot each. The diag summary is no substitute: both the 2fo and
// the 2fp capture froze at the identical key (`regchan_calls=19`, d=27), so
// whatever rbgpu-diag reports, it stops reporting long before powerUp.
//
// What 2fo established is that TWENTY-FOUR synchronous flash commits inside
// IOService::start wedge the boot. It established nothing about one. Gates
// 50, 54, 55, 70 and 75 each landed on boots that went on running, and 2fk
// measured a gate at ~6 us, so a single write is not the hazard the trail
// was; the hazard was doing it two dozen times through the chain. This
// writes once, from one place, and says so in its own name.
// 2fw (2026-10-14): sample the progress stage twice, from a timer.
//
// The 2fm test - read the breadcrumb twice a minute apart from inside the
// armed boot - has been owed for five milestones and has never been run,
// because it needs the user to type two commands into a machine that is
// wedging. It is prior to every other question here: a chain advancing one
// step a minute leaves exactly the record a stopped one leaves, and 2ez
// already caught this project reading slowness as a hang once.
//
// So the plugin runs it itself. Two one-shot `thread_call`s, at +30 s and
// +90 s from `powerUpHW` ENTER, each writing the stage the chain has reached
// into `rbgpu-prog2`. Both samples land in one variable, so a power cycle
// still yields the comparison, and no user action is involved at all.
//
//   a=N          the stage at +30 s
//   a=N b=M      and at +90 s
//
//   b > a   the chain is ADVANCING - there is no stopping point, every
//           endpoint reading in this project is void for locating anything
//   b == a  it is genuinely stopped, established rather than assumed
//   absent  the timer never fired, which is its own answer: the thread_call
//           pool is starved, as 2ez found when the beat died at n=13..28
//
// Cost is two flash commits, both long after the ~500 us chain region, which
// is the budget 2fs/2ft established. Nothing is written inside the region.
// 2fy: THREE separate thread_calls, each armed once from rbgpu_progwatch_start.
// 2fw and 2fx both re-armed the single call from inside its own handler, and in
// three boots the first shot landed within 6 ms of its deadline while the second
// never fired - at a 60 s gap and at a 20 s gap alike. Nothing re-arms itself
// here, so that whole class of failure is gone.
static thread_call_t     g_rbgpuProgCalls[3] = { nullptr, nullptr, nullptr };
static volatile bool     g_rbgpuProgOn   = false;
static IOSimpleLock     *g_rbgpuProgLock = nullptr;
static char              g_rbgpuProgText[384];   // 2gp: three samples now carry the event-machine counters
volatile uint32_t        g_rb_prog_stage = 0;

// 2fz: a bare volatile store - no lock, no log, no NVRAM. Every breadcrumb
// this project has put inside the chain until now went through rbgpu_log or
// rbgpu_diag_set_num, and BOTH take g_diagLock; the gate trail wrote flash.
// A plain store takes nothing, so it cannot itself be what stops the thread,
// and the 2fy timers - which are measured to fire on the dot while the chain
// is frozen - read it from outside.
volatile uint32_t        g_rb_step = 0;

static void rbgpu_prog_action(thread_call_param_t p0, thread_call_param_t) {
    // Samples the stage AND the monotonic log sequence. The stage is coarse -
    // the entire startHWEngines walk sits between stage 2 and stage 3 - so the
    // sequence, bumped on every log append and dedupe hit, is what tells a
    // stopped thread from a crawling one.
    unsigned phase = static_cast<unsigned>(reinterpret_cast<uintptr_t>(p0));
    if (phase > 2) {
        return;
    }
    uint64_t now_ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time(), &now_ns);

    IOSimpleLockLock(g_rbgpuProgLock);
    size_t n = strlen(g_rbgpuProgText);
    // 2gp: the event-machine counters ride out here rather than on marks of
    // their own - waitForStamp runs thousands of times a second, so a write
    // per call would be the 2ez contention and the 2fo wedge at once. This
    // sampler is measured to fire while that thread spins (2gk).
    snprintf(g_rbgpuProgText + n, sizeof(g_rbgpuProgText) - n,
             "%s%c=s%u/S%u/L%llu@%llums W=%u r0=%#x rN=%#x R=%u N=%u T=%u",
             n ? " " : "",
             static_cast<char>('a' + phase), g_rb_prog_stage, g_rb_step,
             g_rbgpuLogSeq, now_ns / 1000000ull,
             g_rb_2gp_wfs_calls, g_rb_2gp_wfs_first, g_rb_2gp_wfs_last,
             g_rb_2gp_restarts, g_rb_2gp_fas_n, g_rb_2gq_stw);
    IOSimpleLockUnlock(g_rbgpuProgLock);

    rbgpu_nvram_note("rbgpu-prog2", g_rbgpuProgText);
    rbgpu_log("2fz: sample %c stage=%u step=%u logseq=%llu",
              static_cast<char>('a' + phase), g_rb_prog_stage, g_rb_step,
              g_rbgpuLogSeq);
}

void rbgpu_progwatch_start(void) {
    if (g_rbgpuProgOn) {
        return;
    }
    if (!g_rbgpuOptions) {
        g_rbgpuOptions = IORegistryEntry::fromPath("/options", gIODTPlane);
    }
    if (!g_rbgpuProgLock) {
        g_rbgpuProgLock = IOSimpleLockAlloc();
    }
    g_rbgpuProgOn = true;

    // +15, +35, +65 s. All three armed here, from the calling thread, because
    // arming from inside the handler is what failed three boots running.
    static const uint32_t kDelaysMs[3] = { 15000, 35000, 65000 };
    unsigned armed = 0;
    for (unsigned i = 0; i < 3; i++) {
        g_rbgpuProgCalls[i] = thread_call_allocate(
            rbgpu_prog_action, reinterpret_cast<thread_call_param_t>(
                                   static_cast<uintptr_t>(i)));
        if (!g_rbgpuProgCalls[i]) {
            continue;
        }
        uint64_t deadline = 0;
        clock_interval_to_deadline(kDelaysMs[i], kMillisecondScale, &deadline);
        thread_call_enter_delayed(g_rbgpuProgCalls[i], deadline);
        armed++;
    }
    rbgpu_log("2fy: progress watch armed, %u of 3 timers (+15 s, +35 s, +65 s "
              "-> rbgpu-prog2); a missing sample now means the timer did not "
              "fire, not that the handler failed to re-arm", armed);
}

// 2gd (2026-10-15): every note carries a per-boot sequence number.
//
// Why, and it is a defect in the channel rather than in the chip. An
// overwrite variable read after a power cycle is only this boot's value if
// something establishes that it was rewritten this boot, and nothing did.
// Two records prove the gap rather than suggest it:
//
//   the 2ga boot's in-boot capture had rbgpu-prog2 = a=s4/S90/L820@28793...
//   the 2gb boot's in-boot capture had rbgpu-prog2 = a=s5/S90/L834@28999...
//   the live read after each power cycle = a=s1/L671@29535 b=... c=...@79535
//
// which is the 2fy boot's value, two boots stale, twice. So a note's write
// can fail to survive the power cycle, and `rbgpu-prog` reading the same text
// as last time cannot be told from this boot writing the same text again.
// That is what made the 2gc boot unreadable, and it is the same shape as the
// 2fr ambiguity and the 2fu third boot (which read "s=1" and was taken as
// this-boot evidence when the previous boot had also written "s=1").
//
// The cause of the lost commit is NOT established and no guess is recorded.
// The nonce makes it detectable whatever the cause: the first note of each
// boot reads the counter out of `rbgpu-boot`, increments it and writes it
// back, and every note is prefixed `b=<n>`. A read whose `b=` equals the one
// recorded last turn is stale, full stop - no inference needed.
static uint32_t g_rbgpuBootSeq = 0;

static uint32_t rbgpu_boot_seq(IORegistryEntry *options) {
    if (g_rbgpuBootSeq != 0) {
        return g_rbgpuBootSeq;
    }
    uint32_t prev = 0;
    if (OSObject *o = options->getProperty("rbgpu-boot")) {
        char buf[24] = { 0 };
        if (OSString *os = OSDynamicCast(OSString, o)) {
            size_t n = strlen(os->getCStringNoCopy());
            if (n >= sizeof(buf)) { n = sizeof(buf) - 1; }
            memcpy(buf, os->getCStringNoCopy(), n);
        } else if (OSData *od = OSDynamicCast(OSData, o)) {
            unsigned len = od->getLength();
            if (len >= sizeof(buf)) { len = sizeof(buf) - 1; }
            memcpy(buf, od->getBytesNoCopy(), len);
        }
        for (const char *c = buf; *c; c++) {
            if (*c >= '0' && *c <= '9') {
                prev = prev * 10u + static_cast<uint32_t>(*c - '0');
                if (prev > 1000000u) { prev = 0; break; }   // garbage, restart
            } else if (prev != 0) {
                break;
            }
        }
    }
    g_rbgpuBootSeq = prev + 1;
    char out[24];
    snprintf(out, sizeof(out), "%u", g_rbgpuBootSeq);
    if (auto str = OSString::withCString(out)) {
        options->setProperty("rbgpu-boot", str);
        str->release();
    }
    return g_rbgpuBootSeq;
}

void rbgpu_nvram_note(const char *var, const char *text) {
    IORegistryEntry *options = g_rbgpuOptions;
    if (!options) {
        options = IORegistryEntry::fromPath("/options", gIODTPlane);
        if (!options) {
            return;
        }
        g_rbgpuOptions = options;   // retained by fromPath; kept for the boot
    }
    if (!var || !text) {
        return;
    }
    OSCompareAndSwap(0, 1, &g_rbgpuNvramBusy);
    uint32_t seq = rbgpu_boot_seq(options);
    char stamped[352];
    snprintf(stamped, sizeof(stamped), "b=%u %s", seq, text);
    if (auto str = OSString::withCString(stamped)) {
        options->setProperty(var, str);
        str->release();
    }
    if (auto sync = OSString::withCString("now")) {
        options->setProperty("IONVRAM-FORCESYNCNOW-PROPERTY", sync);
        sync->release();
    }
    g_rbgpuNvramBusy = 0;
}

void rbgpu_gate_nvram(uint32_t stage, const char *what) {
    IORegistryEntry *options = g_rbgpuOptions;
    if (!options) {
        options = IORegistryEntry::fromPath("/options", gIODTPlane);
        if (!options) {
            return;
        }
        g_rbgpuOptions = options;   // retained by fromPath; kept for the boot
    }
    // 2ex (2026-10-13): a TRAIL, not the last value. Seven boots stopped at
    // seven different gates (53, 51, 50, 50, 54, 50, 151) and the overwrite
    // form cannot tell "the thread stopped here" from "the later writes were
    // lost". Appending every gate and rewriting the whole string makes one
    // boot say which: a trail ending "40 101 151" means the thread stopped
    // there, while "40 101 151 45 50 54" means it got to Apple's code and the
    // overwrite form had simply been losing the tail.
    //
    // Bounded, and the cap is reported rather than silently truncating: the
    // doStart chain writes at most ~20 gates.
    static char trail[192];
    static size_t trail_len = 0;
    static bool  trail_full = false;
    char one[16];
    int n = snprintf(one, sizeof(one), "%u ", stage);
    if (n > 0 && trail_len + static_cast<size_t>(n) + 4 < sizeof(trail)) {
        memcpy(trail + trail_len, one, static_cast<size_t>(n));
        trail_len += static_cast<size_t>(n);
        trail[trail_len] = '\0';
    } else if (!trail_full) {
        trail_full = true;
        strlcat(trail, "CAP", sizeof(trail));
        trail_len = strlen(trail);
    }

    // 2fg: count the writes and any that the registry refuses, and carry both
    // in the text. If the trail stops while w keeps climbing and f stays 0,
    // the writes are landing and the thread really is the thing that stopped;
    // if the last trail the next boot shows is short with f nonzero, the
    // record ran out rather than the thread.
    static uint32_t gate_writes = 0;
    static uint32_t gate_wfails = 0;
    gate_writes++;

    // 2fk (2026-10-13): TIME the trail. Ten boots in this family have ended at
    // ten different gates with the code in the relevant span unchanged, and
    // every structural explanation offered so far has been refuted on
    // hardware: logging contention (2ez, fixed in 2fa, variance stayed), the
    // NVRAM write budget (2ff, refuted by 2fg at w=18 f=0), the register dump
    // (2fe, which helped once and then did not).
    //
    // The one thing never measured is how long a gate itself costs. 2ez found
    // exactly this shape once already - a thread running at 3 us per line and
    // at 10 ms per line within one boot, which is indistinguishable from a
    // wedge and stops wherever the power cycle lands. Each gate here does a
    // setProperty plus IONVRAM-FORCESYNCNOW-PROPERTY, a real flash commit,
    // and nothing has ever clocked one.
    //
    //   t  = ms since the FIRST gate of the boot
    //   wx = the worst single gate-to-gate gap in ms
    //
    // t small (tens of ms) at the last gate  -> the thread genuinely stops,
    //                                           and the endpoint locates it
    // t already seconds, wx in the hundreds  -> the gates are the crawl, the
    //                                           endpoint is just where the
    //                                           power cycle landed, and this
    //                                           whole family has been timing
    //                                           its own instrument again
    static uint64_t first_tick = 0;
    static uint64_t prev_tick  = 0;
    static uint64_t worst_ns   = 0;
    uint64_t now_tick = mach_absolute_time();
    uint64_t total_ns = 0;
    if (first_tick == 0) {
        first_tick = now_tick;
    } else {
        uint64_t d_ns = 0;
        absolutetime_to_nanoseconds(now_tick - prev_tick, &d_ns);
        if (d_ns > worst_ns) { worst_ns = d_ns; }
        absolutetime_to_nanoseconds(now_tick - first_tick, &total_ns);
    }
    prev_tick = now_tick;

    char buf[300];
    snprintf(buf, sizeof(buf), "%s| %u %s [w=%u f=%u t=%llu wx=%llu]", trail,
             stage, what ? what : "", gate_writes, gate_wfails,
             total_ns / 1000000ull, worst_ns / 1000000ull);

    // 2ey: hold the flag across the write so the heartbeat never lands in the
    // middle of it. This thread does NOT wait for the flag - it is the one
    // that must not block - it simply claims it; the heartbeat is the side
    // that yields.
    g_rbgpuLastGate = stage;
    OSCompareAndSwap(0, 1, &g_rbgpuNvramBusy);
    if (auto str = OSString::withCString(buf)) {
        if (!options->setProperty("rbgpu-gate", str)) {
            gate_wfails++;
        }
        str->release();
    } else {
        gate_wfails++;
    }
    if (auto sync = OSString::withCString("now")) {
        options->setProperty("IONVRAM-FORCESYNCNOW-PROPERTY", sync);
        sync->release();
    }
    g_rbgpuNvramBusy = 0;
    // NOT released: g_rbgpuOptions holds the one reference for the boot.
}

void RembrandtGPUPeer::performRefreshDiag() {
    if (!g_diagLock) {
        return;
    }

    IOLockLock(g_diagLock);
    rbgpu_diag_set_num_locked("guard_calls", g_guard_calls);

    // 2fa: copy only what is in use. The ring is indexed modulo its size, so
    // a partially-filled ring still needs the whole array; a full one needs it
    // all anyway. The diag table does not - g_diagCount entries of 112 bytes
    // instead of a flat 35 KB.
    s_snapHead      = g_logHead;
    s_snapLogCount  = g_logCount;
    memcpy(s_snapLogs, g_logRing,
           (g_logCount >= RBGPU_RING_SIZE) ? sizeof(s_snapLogs)
                                           : sizeof(s_snapLogs[0]) * RBGPU_RING_SIZE);

    s_snapDiagCount = g_diagCount;
    memcpy(s_snapDiags, g_diags, sizeof(s_snapDiags[0]) * g_diagCount);
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

    // 2fr (2026-10-14): request 0x1B is NOT sent when powerUpHW failed.
    //
    // The 2fo boot's log ends at "sending Request 0x1B" with no "returned"
    // line after it, where the 2cy boot logged both and then carried on into
    // Wi-Fi scanning. The whole shape of that boot says this is where it
    // stopped: `pci.txt` came back 0 bytes, so the IGPU registry subtree was
    // locked by this very thread, and the user power-cycled it minutes later.
    //
    // What that thread is doing is now clear from the timestamps, and it is
    // not what the journal assumed. Request 0x06 was SENT at 14.944 and
    // RETURNED at 17.229 - so the whole powerUp chain, doStart and all, runs
    // INSIDE request 0x06 on this thread, long after the accelerator's
    // start() returned 1 at 14.958. 0x06 then came back 0 and 0x1B never did.
    //
    // Why 0x1B behaves differently from 2cy, where it answered 0xE00002C7 at
    // once: in 2cy powerUpHW failed at the first gate and the hardware was
    // untouched. Now the engines are powered, the queues are mapped and the
    // rings are programmed, so "Controller Enabled" reaches code that has
    // real state to act on - and waits.
    //
    // Skipping it is the honest state, not a bypass. powerUpHW returned 0, so
    // the controller is NOT enabled, and telling the accelerator that it is
    // would be another blanket-true latch of the kind 2am/2an/2ao already
    // owe this project an explanation for. `-rbgpuforce1b` sends it anyway,
    // for the boot where powerUpHW finally succeeds and the answer is
    // expected to arrive.
    static int force1b = -1;
    if (force1b < 0) { force1b = checkKernelArgument("-rbgpuforce1b") ? 1 : 0; }
    // 2gc: hold 0x1B back while the SDMA instance rewrite is being tested,
    // even if powerUpHW now SUCCEEDS. Otherwise the one boot that answers the
    // SDMA question would also be the first boot ever to send 0x1B with the
    // hardware fully brought up - two variables at once, which is the mistake
    // 2fb/2fc and the withdrawn 2ev table were each made of. Sending it is
    // then a deliberate second step with `-rbgpuforce1b`.
    // 2gi: `-rbgpusdmaone` joins the hold, and it is the one that makes the
    // hold matter. That boot is expected to be the FIRST on which powerUpHW
    // succeeds, so without this the same boot would also be the first ever to
    // send 0x1B with the hardware fully brought up - and 2fo measured that
    // call never returning. One variable.
    static int hold1b = -1;
    if (hold1b < 0) {
        hold1b = (checkKernelArgument("-rbgpusdmainst0") ||
                  checkKernelArgument("-rbgpusdma0skip") ||
                  checkKernelArgument("-rbgpusdmaone")) ? 1 : 0;
    }
    if ((!g_rb_poweruphw_ok || hold1b) && !force1b) {
        rbgpu_log("handlePostRegister: Request 0x1B (Controller Enabled) is "
                  "NOT sent - powerUpHW_ok=%d, 2gc hold=%d. 2fo shows this "
                  "call does not return once the hardware has real state "
                  "(-rbgpuforce1b overrides)",
                  g_rb_poweruphw_ok, hold1b);
        rbgpu_diag_set_num("req1b_skipped", 1);
        rbgpu_diag_set_num("req1b_hold_2gc", static_cast<uint64_t>(hold1b));
        // 2gb: set the stage the 2fy samplers read, too. The 00:37 boot wrote
        // s=5 here at 16.58 s while all three samples still reported s=4,
        // because only the kern_start helper was updating g_rb_prog_stage.
        // Harmless, but a record that disagrees with itself is a trap.
        g_rb_prog_stage = 5;
        rbgpu_nvram_note("rbgpu-prog",
                         g_rb_poweruphw_ok
                             ? "s=5 powerUpHW OK; 0x1B HELD by an SDMA arg"
                             : "s=5 0x06 done ret=0; 0x1B SKIPPED (powerUpHW failed)");
        g_rb_peer_stage |= 0x10;
        return;
    }

    rbgpu_log("handlePostRegister: sending Request 0x1B (Controller Enabled)");
    uint32_t reqControllerEnabled = 0x1B;
    g_rb_prog_stage = 5;
    rbgpu_nvram_note("rbgpu-prog", "s=5 0x1B SENT - no s=6 means it hangs here");
    // 2gn: arm the 0x1B post-gate record only across this call. All three
    // hooked functions are also called from powerUpHW's own path, and an
    // ungated note would be overwritten by calls that are not the question.
    g_rb_2gn_in1b = 1;
    IOReturn ret1B = accelerator->callPlatformFunction(symSpecialAMDKey, true, &reqControllerEnabled, &one, nullptr, nullptr);
    // 2gx: commit the request trail while the window is still open (the
    // mark is gated on it), then close it. One NVRAM write, not one per
    // request.
    rbgpu_2gx_flush();
    g_rb_2gn_in1b = 0;
    rbgpu_log("handlePostRegister: Request 0x1B returned 0x%08X", ret1B);
    rbgpu_diag_set_num("ret_0x1B", static_cast<uint64_t>(ret1B));
    g_rb_peer_ret1B = static_cast<uint32_t>(ret1B);
    g_rb_peer_stage |= 8;
    {
        char buf[96];
        g_rb_prog_stage = 6;
        snprintf(buf, sizeof(buf), "s=6 0x1B RETURNED 0x%08X", ret1B);
        rbgpu_nvram_note("rbgpu-prog", buf);
    }
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
                rbgpu_2gx_req(req, 0);
                return kIOReturnSuccess;

            case 0x0B:
                if (accelerator) {
                    rbgpu_log("Dropping registered accelerator: %s", accelerator->getName());
                    accelerator->release();
                    accelerator = nullptr;
                }
                rbgpu_2gx_req(req, 0);
                return kIOReturnSuccess;

            case 0x14:
                rbgpu_2gx_req(req, 0);
                if (p2) {
                    *reinterpret_cast<uint32_t *>(p2) = 1;
                    return kIOReturnSuccess;
                }
                return kIOReturnBadArgument;

            case 0x17:
                rbgpu_log("ASIC Reset State requested (unsupported)");
                rbgpu_2gx_req(req, 1);
                return kIOReturnUnsupported;

            case 0x1A:
                rbgpu_log("Get DCC Capabilities requested (unsupported)");
                rbgpu_2gx_req(req, 1);
                return kIOReturnUnsupported;

            default:
                rbgpu_log("Unhandled SpecialAMDKey request 0x%02X", req);
                rbgpu_2gx_req(req, 1);
                // 2gz: 0x12 and 0x1D are the ONLY two requests Apple makes
                // inside the 0x1B window, we refuse both, and 0x1B then
                // returns the same kIOReturnUnsupported we gave them (2gy).
                // The obvious next move is to answer them - but answering
                // means deciding what to write through p2, and nothing so
                // far says whether p2 is an out-parameter at all. Writing a
                // value we invented and then reading the consequence as a
                // hardware result is exactly the 2cy failure and the debt
                // 2am/2an/2ao still owe.
                //
                // So look first. This prints the three argument pointers
                // and, for each that is a kernel pointer, the first two
                // dwords behind it. Observe-only, no boot-arg: 0x12 and
                // 0x1D arrive once each per boot (2gy timestamps), so there
                // is no storm to cause.
                if (req == 0x12 || req == 0x1D) {
                    uintptr_t a2 = reinterpret_cast<uintptr_t>(p2);
                    uintptr_t a3 = reinterpret_cast<uintptr_t>(p3);
                    uintptr_t a4 = reinterpret_cast<uintptr_t>(p4);
                    rbgpu_log("2gz: req 0x%02X args p2=%p p3=%p p4=%p",
                              req, p2, p3, p4);
                    const uintptr_t kmin = 0xffffff8000000000ULL;
                    // Each path SAYS something - a pointer that is not read
                    // must not look the same as one that read zero.
                    if (a2 >= kmin) {
                        volatile uint32_t *q = reinterpret_cast<volatile uint32_t *>(a2);
                        rbgpu_log("2gz:   *p2 = 0x%08X 0x%08X", q[0], q[1]);
                    } else {
                        rbgpu_log("2gz:   p2 not a kernel pointer - NOT read");
                    }
                    if (a3 >= kmin) {
                        volatile uint32_t *q = reinterpret_cast<volatile uint32_t *>(a3);
                        rbgpu_log("2gz:   *p3 = 0x%08X 0x%08X", q[0], q[1]);
                    } else {
                        rbgpu_log("2gz:   p3 not a kernel pointer - NOT read");
                    }
                    if (a4 >= kmin) {
                        volatile uint32_t *q = reinterpret_cast<volatile uint32_t *>(a4);
                        rbgpu_log("2gz:   *p4 = 0x%08X 0x%08X", q[0], q[1]);
                    } else {
                        rbgpu_log("2gz:   p4 not a kernel pointer - NOT read");
                    }
                }
                return kIOReturnUnsupported;
        }
    }

    return super::callPlatformFunction(functionName, waitForFunction, p1, p2, p3, p4);
}

