
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
// 2qw: and it happened AGAIN, at 448. Boot 153 read exactly 448 of 448 and
// lost `mcp_super_size`, `mcp_verdict` and `mcp_say` - the three keys that
// carried a probe's whole conclusion, while every component it was computed
// from survived. Raised to 768, and this time the table REPORTS ITS OWN
// OCCUPANCY: `diag_max` and `diag_used` are seeded into slots 0 and 1 on the
// first insert, so they can never themselves be the keys that get dropped.
// A capture whose `diag_used` equals `diag_max` is a capture in which absence
// means nothing - readable from the data, not only from a log line nobody
// captured.
#define RBGPU_MAX_DIAGS 768
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
extern void (*g_rb_enableAccelerator_2hg)(void *);
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

// 2qw: slots 0 and 1, reserved. Seeded before any real key so that the two
// numbers which say whether the table is trustworthy are the two that can
// never be evicted by it filling up.
#define RBGPU_DIAG_SLOT_MAX  0
#define RBGPU_DIAG_SLOT_USED 1

static void rbgpu_diag_reserve_locked(void) {
    if (g_diagCount != 0) {
        return;
    }
    strncpy(g_diags[0].key, "diag_max", sizeof(g_diags[0].key) - 1);
    g_diags[0].type = DIAG_TYPE_NUM;
    g_diags[0].numVal = RBGPU_MAX_DIAGS;
    strncpy(g_diags[1].key, "diag_used", sizeof(g_diags[1].key) - 1);
    g_diags[1].type = DIAG_TYPE_NUM;
    g_diags[1].numVal = 2;
    g_diagCount = 2;
}

static void rbgpu_diag_set_num_locked(const char *key, uint64_t val) {
    rbgpu_diag_reserve_locked();
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
        g_diags[RBGPU_DIAG_SLOT_USED].numVal = g_diagCount;
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
        rbgpu_diag_reserve_locked();                               // 2qw
        if (g_diagCount < RBGPU_MAX_DIAGS) {
            strncpy(g_diags[g_diagCount].key, key, sizeof(g_diags[g_diagCount].key) - 1);
            g_diags[g_diagCount].key[sizeof(g_diags[g_diagCount].key) - 1] = '\0';
            g_diags[g_diagCount].type = DIAG_TYPE_STR;
            strncpy(g_diags[g_diagCount].strVal, val, sizeof(g_diags[g_diagCount].strVal) - 1);
            g_diags[g_diagCount].strVal[sizeof(g_diags[g_diagCount].strVal) - 1] = '\0';
            g_diagCount++;
            g_diags[RBGPU_DIAG_SLOT_USED].numVal = g_diagCount;   // 2qw
        } else {
            g_diagFullPending = true;                              // 2qw
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
        rbgpu_diag_reserve_locked();                               // 2qw
        if (g_diagCount < RBGPU_MAX_DIAGS) {
            strncpy(g_diags[g_diagCount].key, key, sizeof(g_diags[g_diagCount].key) - 1);
            g_diags[g_diagCount].key[sizeof(g_diags[g_diagCount].key) - 1] = '\0';
            g_diags[g_diagCount].type   = DIAG_TYPE_NUM;
            g_diags[g_diagCount].numVal = 1;
            g_diagCount++;
            g_diags[RBGPU_DIAG_SLOT_USED].numVal = g_diagCount;   // 2qw
        } else {
            g_diagFullPending = true;                              // 2qw
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

    // 2in (2026-10-18): a BOUNDED RETRY, the 2gy debt paid.
    //
    // This lookup was tried exactly once. On a boot where the peer runs
    // early it loses the race against /options appearing in the IODT plane,
    // logs "not found", and silently does not arm - which looks exactly like
    // the user forgetting to set the flag. It cost a boot tonight: the
    // machine came up with a working unaccelerated desktop, `armed=0` live,
    // and `rbgpu-arm` still 1 because it was never consumed.
    //
    // Ten tries, 100 ms apart, so at most one second. IOSleep is safe here:
    // this runs on the peer's own start() thread, not an interrupt context,
    // and a second is nothing against the 1.9 s the peer usually starts at.
    // The count is logged either way, so a boot that armed on the seventh
    // try is distinguishable from one that armed immediately - absence is
    // not a result, and neither is a success whose cost is invisible.
    IORegistryEntry *options = nullptr;
    unsigned tries = 0;
    for (; tries < 10; tries++) {
        options = IORegistryEntry::fromPath("/options", gIODTPlane);
        if (options) {
            break;
        }
        IOSleep(100);
    }
    if (!options) {
        rbgpu_log("Peer: /options not found after %u tries over %u ms; "
                  "accelerator NOT armed. rbgpu-arm is untouched, so a plain "
                  "reboot retries - no reinstall needed.", tries, tries * 100);
        return false;
    }
    if (tries > 0) {
        rbgpu_log("Peer: /options appeared on try %u (%u ms of waiting) - the "
                  "2gy race is real and the retry caught it.",
                  tries + 1, tries * 100);
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

    // 2ph: the DAL dry run's personality is gated on this property, which is
    // published only with -rbgpudal. Without it Apple's framebuffer kext is
    // never even loaded, so the boot is identical to one without this code.
    if (checkKernelArgument("-rbgpudal")) {
        pciDevice->setProperty("LoadDisplayController", kOSBooleanTrue);
        rbgpu_log("2ph: published LoadDisplayController - Apple's DAL will be "
                  "loaded, with every register write recorded and dropped");
    }
    // 2pi: the Wrangler, separately. Boot 140 showed the CONTROLLER attaches
    // and start()s clean on this chip with ZERO register writes, so start() is
    // construction only and something else has to ask DAL to touch hardware.
    // The Wrangler is Apple's IOFramebuffer for this GPU and is the obvious
    // asker. Its own arg so it stays independently attributable from the
    // controller (rule 16), and its own match category in the plist so it
    // cannot contest IOFramebuffer and cannot take the display.
    // 2pk: the PROMOTED Wrangler, which takes IOFramebuffer for real. Its
    // start() hook declines after letting DAL run, so IONDRVFramebuffer keeps
    // the panel - see the comment on hooked_wr_start_2pk.
    if (checkKernelArgument("-rbgpudalfbreal")) {
        pciDevice->setProperty("LoadDisplayWranglerFB", kOSBooleanTrue);
        rbgpu_log("2pk: published LoadDisplayWranglerFB - Apple's framebuffer "
                  "will contest IOFramebuffer, run DAL with every write "
                  "dropped, then decline so the panel stays on IONDRV");
    }
    if (checkKernelArgument("-rbgpudalfb")) {
        pciDevice->setProperty("LoadDisplayWrangler", kOSBooleanTrue);
        rbgpu_log("2pi: published LoadDisplayWrangler - Apple's framebuffer "
                  "will start in a PRIVATE match category, so IONDRVFramebuffer "
                  "keeps the panel and every DAL write is still dropped");
    }

    // 2cs: LoadAccelerator only on an armed boot. Without it Apple's
    // accelerator personality does not match at all, so nothing can claim the
    // display and the machine always reaches a desktop.
    const bool armed = rbgpuConsumeArming();
    if (armed) {
        pciDevice->setProperty("LoadAccelerator", kOSBooleanTrue);
    }
    rbgpu_diag_set_num("armed", armed ? 1 : 0);

    // 2li (2026-10-20): THE DISPLAY HALF WAS NEVER ASKED TO START.
    //
    // There is no AMD framebuffer because the chain that builds one never
    // begins. `AMDRadeonX6000Framebuffer.kext` carries the per-generation
    // controllers, and `AMDRadeonNavi21Controller` matches on real Navi 21
    // PCI ids:
    //
    //   IOPCIMatch 0x73A01002 0x73A21002 0x73A31002 0x73AB1002
    //              0x73AE1002 0x73AF1002 0x73BF1002
    //
    // This GPU is `0x16811002` and matches none of them, so the controller
    // never matches, no `AtiFbStub` is published, `AMDFramebuffer` (which
    // matches `IOProviderClass = AtiFbStub`, `IONameMatch = display`) has no
    // provider, and DCN is never programmed. That - not a DCN fault - is why
    // the panel is still on `IONDRVFramebuffer` and why System Profiler
    // reports the boot framebuffer's 7 MB (2kx).
    //
    // Confirmed live: `AMDSupport` is matched and active with **no
    // children** and `busy 0 (1 ms)`, no `AtiFbStub` instance exists, and
    // `kextstat` shows no framebuffer kext loaded at all.
    //
    // So the display half needs the same treatment the accelerator half
    // already gets: make Apple's personality match. IOPCIFamily honours a
    // `device-id` property injected on the device node, which is the
    // established technique for exactly this.
    //
    // OPT-IN and reversible, because it changes what matches on this node -
    // potentially including things that work today. `-rbgpufbmatch`.
    if (checkKernelArgument("-rbgpufbmatch")) {
        // 0x73A0, little-endian, the first id AMDRadeonNavi21Controller
        // accepts. Four bytes, the width IOPCIFamily reads.
        static const uint8_t kNavi21Id[4] = { 0xA0, 0x73, 0x00, 0x00 };
        if (auto did = OSData::withBytes(kNavi21Id, sizeof(kNavi21Id))) {
            pciDevice->setProperty("device-id", did);
            did->release();
            rbgpu_log("2li: device-id spoofed to 0x73A0 so "
                      "AMDRadeonNavi21Controller can match - the display "
                      "chain is AtiFbStub -> AMDFramebuffer -> DCN");
            rbgpu_diag_set_num("fbmatch", 1);
        } else {
            rbgpu_log("2li: FAILED to allocate the device-id override");
            rbgpu_diag_set_num("fbmatch", 0);
        }
    } else {
        rbgpu_log("2li: -rbgpufbmatch not set, device-id left at 0x1681 and "
                  "no AMD framebuffer will be built");
        rbgpu_diag_set_num("fbmatch", 2);
    }

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

    // 2ow: start the vblank pump HERE, not inside the routed framebuffer call.
    // 2ov created its workloop inside `getInformationForDisplayMode`, during
    // the framebuffer's own early mode enumeration, and boot 135 reported
    // `vblp_solved 1` with `vblp_armed 0` - the creation failed there. This is
    // ordinary IOService matching context, where a workloop is routine. The
    // pump idles until 2ou's hook hands over the framebuffer pointer and the
    // shmem at fb+0x90 exists, and it re-checks the shmem every tick rather
    // than trusting one early sample.
    rbgpu_vblp_start(this);   // 2ox: the owner - IOEventSource::init refuses NULL

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
    // 2qw: first, because they say whether every OTHER absence in the capture
    // means anything. diag_used == diag_max means keys were dropped.
    "diag_max", "diag_used",
    // 2s8: THE NOTE IS 512 BYTES AND THESE WERE PAST THE CUT. 2s5 and 2s6
    // both declared these keys "NVRAM-hot so a wedged boot reports them", and
    // boot 167 measured that FALSE: `fb_vbl` appears ZERO times in its
    // nvram.txt, because the note truncates mid-`mcp_mc_off` and this block
    // sat ~74 entries down the list. Being in the hot LIST is not being in the
    // NOTE - rule 20 (a registration that reports success is not evidence it
    // applied) on the project's own instrument, and it would have wasted the
    // one boot these keys exist for.
    //
    // So the list's own 2fq convention applies: the frontier moved and the
    // ordering did not follow. These come first now, ahead of the agdc, iop
    // and mcp blocks, whose questions are answered (2qy, 2s0) and which were
    // consuming the whole note.
    //
    // `fb_vbl_ot_armed` is the line that would move the display half: it is
    // set only from enableVBLInterrupt, whose caller chain starts at
    // IOAccelDisplayPipe::transaction_queue_gated, so it means a transaction
    // was queued. `fb_vbl_hv_lo`/`_hi` must equal `fb_vbl_sym_lo`/`_hi` or
    // nothing else in the block means anything (2s7's gate).
    // Only the cheap verdicts come first. The four pointer keys
    // (`fb_vbl_hv_lo`/`_hi`, `fb_vbl_sym_lo`/`_hi`) cost ~100 of the 512
    // bytes between them and their gate is ALREADY settled on this build by
    // boot 167, so they stay further down: a wedged boot needs the verdicts
    // and the proof the early trigger ran, not a re-measurement of the gate.
    "fb_vbl_ot_armed", "fb_vbl_ot_state", "fb_vbl_ot_n", "fb_vbl_ot",
    // 2s9: boot 168's verdict keys. agdc_resample says which exit the
    // re-sample took (1 ran, 2 already valid, 3 no framebuffer, 4 entered,
    // 5 no thread_call) and agdc_resample_valid is the enabling field itself
    // - whether a timing is now cached - so a boot cannot report "ran" and
    // leave 0x921 still refused without saying so. agdc_tim_cached names the
    // source (1 NDRV, 2 OTG0) and its ABSENCE is what boot 168 read.
    "agdc_resample", "agdc_resample_valid", "agdc_tim_cached",
    // 2sc: whether the boot asked for the four new capability types
    // (agdc_extcap_all), how many queries were refused by our own gate
    // (agdc_extcap_gated_n) and how many types were ANSWERED. On boot 169 the
    // answered count was 1 of 16; with the arg it should be 5.
    "agdc_extcap_all", "agdc_extcap_answered", "agdc_extcap_gated_n",
    "fb_vbl_hv_state", "fb_vbl_state", "fb_vbl_ot_cls", "fb_vbl_hv_cls",
    // and the keys that say the early trigger actually ran, so an absent
    // `fb_vbl_ot_armed` can be told from "the bridge never published".
    "agdc_early", "agdc_started", "agdc_stage", "agdc_note_fired",
    "iop_verify",
    // 2ra: current framebuffer frontier. These precede the old probes so a
    // failed login boot still distinguishes refusal, registration and start.
    "agdc_on", "agdc_error", "agdc_solved", "agdc_stage", "agdc_oneshot", "agdc_started",
    // 2rz: which trigger ran. 0 = late (framebuffer), the boot-163 shape that
    // keeps a desktop; 1 = the early matched notification, which boot 164
    // measured as winning the gate and wedging the machine.
    "agdc_early",
    // 2ry: boot 164 reached 0x921 with no timing; the summary omitted WHY.
    // Keep the sampler's verdict ahead of query counters and solved probes.
    "agdc_tim_err", "agdc_tim_cached", "agdc_tim_ndrv_err", "otg_err",
    "agdc_link_result", "agdc_extcap_answered",
    "agdc_user_vendor", "agdc_user_cap",
    "agdc_last_attr", "agdc_last_ret", "agdc_failed_attr", "agdc_failed_ret",
    "agdc_logical_calls", "agdc_logical_ret",
    // 2qo: THE frontier, and the first change here that is EXPECTED to be able
    // to end in a login screen that never appears - so these must survive a
    // boot with no usable screen, which is the whole reason this list exists.
    // Read them in this order:
    //   iop_oneshot  0 armed+consumed, 1 /options unreachable, 2 not armed,
    //                3 literal "0", 4 armed but the clear did not take (the
    //                NEXT boot is armed too - clear it by hand).
    //   iop_saw_ws   absent means WindowServer never execed while the hook was
    //                live, so nothing was attempted and nothing is proven.
    //   iop_match    1 the five bytes were the call, 2 already patched,
    //                0 neither (an OS update moved the site - failing safe).
    //   iop_verify   1 is the only line that proves the write landed, read
    //                back from WindowServer's own memory (rule 20).
    "iop_on", "iop_saw_ws", "iop_ready", "iop_oneshot",
    "iop_match", "iop_patched",
    // 2qt: read-only, but it runs during IOGraphicsFamily's load, long before
    // a desktop exists, so a boot that never shows one still reports it.
    // `mcp_verdict` 15 is the only value that means anything: name, size,
    // super and vtable all agreeing with the static read of
    // IOFramebuffer::MetaClass::MetaClass().
    // 2qu: the two ADDRESSES are hot too. Boot 151 refused both symbols on a
    // bad range check and recorded the rejected address as 0, so the capture
    // could not say what had been refused. The address and the range it is
    // judged against now travel together.
    "mcp_on", "mcp_mc", "mcp_vt", "mcp_acc", "mcp_size", "mcp_verdict",
    "mcp_mc_addr", "mcp_vt_addr", "mcp_kext_base",
    "mcp_mc_off", "mcp_vt_off", "mcp_deferred",
    // 2qx: stage 2. `-rbgpumcreg` can leave the machine with a next kext
    // load that hangs forever on _sStalledClassesLock, so every one of
    // these has to survive a boot that never finishes coming up.
    "mcn_on", "mcr_on", "mcn_ready", "mcn_built", "mcn_verdict",
    "mcr_oneshot", "mcr_ran", "mcr_pre", "mcr_check", "mcr_post",
    "mcr_name_ok",
    // 2q4: THE frontier. This boot claims kDisplayModeAcceleratorBackedFlag,
    // and it is the first display change in this project that can plausibly
    // blank the panel rather than merely report a wrong number - so these have
    // to survive a boot with no usable screen (rule 14, rule 10).
    // `fbaccel_flags0` is the one the premise rests on: 2q4 measured the
    // mode's original flags as 0xf, and if the capture says otherwise the
    // machine was not in the state that was measured. `fbaccel_had` non-zero
    // means the flag was ALREADY set and 2q4's premise is simply wrong.
    // 2s5: THE FRONTIER, so it comes first. These keys answer a question only
    // a boot that WEDGES can answer, and such a boot leaves nothing but NVRAM
    // (rule 10) - boot 164's capture is NVRAM-only for exactly that reason,
    // and `fb_vbl_state` was not hot, so the one key that would have
    // separated "a transaction was queued and we starved the VBL" from "no
    // transaction was ever queued" was invisible in the only boot where it
    // mattered. `fb_vbl_ot_armed` is the line that would move the display
    // half: it is set only from enableVBLInterrupt, whose caller chain starts
    // at IOAccelDisplayPipe::transaction_queue_gated. `fb_vbl_ot` says a
    // second 'vbl ' registrant exists at all; `fb_vbl_cls` says how it was
    // told apart (bit 0 = by target, bit 1 = by proc - a 0 or a 3 with both
    // registrants present means the classifier, not the stack, is the
    // suspect).
    // 2s6: the per-entry classification and the control pointer. Boot 166's
    // `fb_vbl_cls 0` was the pipe's own (correct) value read as a fault, and
    // its `fb_vbl_ot_lo`/`_hi` had no known-good pointer beside it to check
    // the capture path against. `fb_vbl_sym_*` is that pointer.
    "fb_vbl_hv_lo", "fb_vbl_hv_hi", "fb_vbl_sym_lo", "fb_vbl_sym_hi",
    "fb_vbl_ot_lo", "fb_vbl_ot_hi", "fb_vbl_cls",
    "fbaccel_on", "fbaccel_set", "fbaccel_had", "fbaccel_flags0",
    // 2pq: answered (2pr) - kept only because the keys cost nothing.
    // 2pq: the frontier, so it comes first. These seven decide whether the
    // boot answered its question, and they have to survive a boot with no
    // usable screen (rule 14, rule 10). `fbvr_orig_len` is the number the
    // whole hypothesis rests on - if it is not 8294400 the machine was not in
    // the state 2pp measured and nothing else here means anything.
    "fbvr_routed", "fbvr_verify", "fbvr_hit", "fbvr_big",
    "fbvr_orig_len", "fbvr_fail", "fbvr_calls",
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
    // 2pl: the DAL promotion's numbers. Boot 143 panicked and boot 144 may
    // come up with no usable screen, so these have to survive in NVRAM or the
    // boot teaches nothing (rule 14). Numeric only - the full trace lives in
    // dal_tr0..15 in RBGPUDiag and needs a readable boot; the 512-byte NVRAM
    // budget cannot hold it.
    "dal_writes", "dal_r_hw", "dal_tr_n", "dalfb_start_ret",
    "dalfb_grouted", "dalfb_attr_blocked",
    // 2po: these two were PROMISED to the user as persisting and were not in
    // this list, so boot 145 could not say how many writes got through. Added.
    "dal_passed", "dal_denied", "dal_pass",
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
// 2hk: two more, for the ring probe. Separate calls, armed AFTER all three
// prog2 samples are due, so that if a GC register read stalls the way 2fe's
// dump did, it takes only itself down and not the progress samples.
static thread_call_t     g_rbgpuRingCalls[3] = { nullptr, nullptr, nullptr };
extern void rbgpu_ring_probe_2hk(unsigned phase);

// 2js: twenty-four samplers, 500 ms apart, bracketing the CP re-halt.
// All armed ONCE from rbgpu_progwatch_start - 2fy established that
// re-arming a thread_call from inside its own handler does not fire.
#define RB_2JS_CALLS 24
static thread_call_t     g_rbgpuMeCalls[RB_2JS_CALLS] = { nullptr };
extern void rbgpu_mewatch_sample_2js(unsigned idx);
extern void rbgpu_mewatch_flush_2js(void);
// 2jt: the fine bank - 2jx widened it to 60 samples, 50 ms apart,
// 6.5 s .. 9.5 s, after boot 70's halt at 7345 ms fell outside 7.5 s.
#define RB_2JT_CALLS 60
static thread_call_t     g_rbgpuRlcCalls[RB_2JT_CALLS] = { nullptr };
extern void rbgpu_rlcwatch_sample_2jt(unsigned idx);
extern void rbgpu_rlcwatch_flush_2jt(void);
// 2ju: the CGCG/PG disable - 24 ticks, 400 ms apart, 2.0 s .. 11.2 s. It
// writes, so it is behind -rbgpunocgcg; see 2ju in kern_start.cpp.
#define RB_2JU_CALLS 24
static thread_call_t     g_rbgpuCgcgCalls[RB_2JU_CALLS] = { nullptr };
extern void rbgpu_nocgcg_tick_2ju(unsigned idx);
extern void rbgpu_nocgcg_flush_2ju(void);

static void rbgpu_nocgcg_action(thread_call_param_t p0, thread_call_param_t) {
    unsigned i = static_cast<unsigned>(reinterpret_cast<uintptr_t>(p0));
    rbgpu_nocgcg_tick_2ju(i);
    if (i == RB_2JU_CALLS - 1) {
        rbgpu_nocgcg_flush_2ju();
    }
}

static void rbgpu_rlcwatch_action(thread_call_param_t p0, thread_call_param_t) {
    unsigned i = static_cast<unsigned>(reinterpret_cast<uintptr_t>(p0));
    rbgpu_rlcwatch_sample_2jt(i);
    if (i == RB_2JT_CALLS - 1) {
        rbgpu_rlcwatch_flush_2jt();
    }
}

static void rbgpu_mewatch_action(thread_call_param_t p0, thread_call_param_t) {
    unsigned i = static_cast<unsigned>(reinterpret_cast<uintptr_t>(p0));
    rbgpu_mewatch_sample_2js(i);
    if (i == RB_2JS_CALLS - 1) {
        rbgpu_mewatch_flush_2js();
    }
}

static void rbgpu_ring_action(thread_call_param_t p0, thread_call_param_t) {
    rbgpu_ring_probe_2hk(static_cast<unsigned>(reinterpret_cast<uintptr_t>(p0)));
}
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

    // 2hk: the ring probe. 2ho adds the EARLY sample, and it is the one the
    // next question turns on.
    //
    // 2hn read GCVM_L2_PROTECTION_FAULT_STATUS = 0x40953 - WALKER_ERROR,
    // MAPPING_ERROR, MORE_FAULTS - but that register is LATCHED and carries
    // no timestamp. It says a fault happened, not when, and not that it is
    // the fault the CP died on. Reading it once and calling it the cause
    // would be the recurring error for the twenty-sixth time.
    //
    // +12 s answers it read-only. rbgpu-prog2 puts the first waitForTimestamp
    // timeout AFTER +21 s (T=0 at the +15 s sample, T=8 at +35 s), so +12 s
    // is before the hang develops:
    //
    //   clean at +12 s, faulted at +100 s -> the fault happens in the window
    //       where the hang develops, and 2hn's chain is established
    //   already faulted at +12 s          -> an earlier GPUVM problem that
    //       bring-up has been walking past, which is a bigger finding
    //
    // +100 s and +160 s stay as they were: by +100 s the hang is long
    // underway, and +160 s leaves room to start a Metal dispatch over SSH
    // between the two.
    //
    // The probe itself is off unless -rbgpuringprobe; these timers are armed
    // either way so that a boot WITHOUT the arg still logs "ring probe OFF"
    // and cannot be mistaken for a timer that failed to fire.
    static const uint32_t kRingMs[3] = { 12000, 100000, 160000 };
    unsigned rarmed = 0;
    for (unsigned i = 0; i < 3; i++) {
        g_rbgpuRingCalls[i] = thread_call_allocate(
            rbgpu_ring_action, reinterpret_cast<thread_call_param_t>(
                                   static_cast<uintptr_t>(i)));
        if (!g_rbgpuRingCalls[i]) {
            continue;
        }
        uint64_t rd = 0;
        clock_interval_to_deadline(kRingMs[i], kMillisecondScale, &rd);
        thread_call_enter_delayed(g_rbgpuRingCalls[i], rd);
        rarmed++;
    }
    rbgpu_log("2ho: ring probe armed, %u of 3 timers (+12 s, +100 s, +160 s "
              "-> rbgpu-ring). The +12 s sample is the one that time-bounds "
              "the latched VM fault 2hn found.", rarmed);

    // 2js: 0.5 s .. 12 s in 500 ms steps. Read-only; the sampler itself
    // checks -rbgpumewatch and returns immediately when it is off, so the
    // timers cost a function call each on a normal boot.
    unsigned marmed = 0;
    for (unsigned i = 0; i < RB_2JS_CALLS; i++) {
        g_rbgpuMeCalls[i] = thread_call_allocate(
            rbgpu_mewatch_action, reinterpret_cast<thread_call_param_t>(
                                      static_cast<uintptr_t>(i)));
        if (!g_rbgpuMeCalls[i]) {
            continue;
        }
        uint64_t md = 0;
        clock_interval_to_deadline((i + 1) * 500, kMillisecondScale, &md);
        thread_call_enter_delayed(g_rbgpuMeCalls[i], md);
        marmed++;
    }
    rbgpu_log("2js: CP re-halt watch armed, %u of %u timers (0.5 s .. 12 s, "
              "500 ms apart -> rbgpu-mew). Brackets the window between the "
              "un-halt at ~341 ms and the +12 s ring probe.",
              marmed, RB_2JS_CALLS);

    // 2jt: 6.5 s .. 9.5 s in 50 ms steps (2jx widened it) - covers boot 70's
    // 7345 ms halt as well as the 8827/8835/8839 ms baseline.
    unsigned rarmed2 = 0;
    for (unsigned i = 0; i < RB_2JT_CALLS; i++) {
        g_rbgpuRlcCalls[i] = thread_call_allocate(
            rbgpu_rlcwatch_action, reinterpret_cast<thread_call_param_t>(
                                       static_cast<uintptr_t>(i)));
        if (!g_rbgpuRlcCalls[i]) {
            continue;
        }
        uint64_t rd2 = 0;
        clock_interval_to_deadline(6500 + i * 50, kMillisecondScale, &rd2);
        thread_call_enter_delayed(g_rbgpuRlcCalls[i], rd2);
        rarmed2++;
    }
    rbgpu_log("2jt: RLC watch armed, %u of %u timers (6.5 s .. 9.5 s, 50 ms "
              "apart -> rbgpu-mew2).", rarmed2, RB_2JT_CALLS);

    // 2ju: 2.0 s .. 11.2 s in 400 ms steps. Every tick writes
    // RLC_CGCG_CGLS_CTRL = 0 and RLC_PG_CNTL = 0 and reads both back, so a
    // value that gets restored between ticks shows up as `restored>0` rather
    // than as a silent failure. Armed unconditionally here; the tick itself
    // returns at once unless -rbgpunocgcg is set, so no write happens without
    // the arg. All armed once from this thread (2fy).
    unsigned carmed = 0;
    for (unsigned i = 0; i < RB_2JU_CALLS; i++) {
        g_rbgpuCgcgCalls[i] = thread_call_allocate(
            rbgpu_nocgcg_action, reinterpret_cast<thread_call_param_t>(
                                     static_cast<uintptr_t>(i)));
        if (!g_rbgpuCgcgCalls[i]) {
            continue;
        }
        uint64_t cd = 0;
        clock_interval_to_deadline(2000 + i * 400, kMillisecondScale, &cd);
        thread_call_enter_delayed(g_rbgpuCgcgCalls[i], cd);
        carmed++;
    }
    rbgpu_log("2ju: CGCG disable armed, %u of %u timers (2.0 s .. 11.2 s, "
              "400 ms apart -> rbgpu-cgcg). Writes only with -rbgpunocgcg.",
              carmed, RB_2JU_CALLS);
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

// 2qo: a one-shot NVRAM gate for an experiment that can cost the desktop.
//
// Same contract as `rbgpu-arm` (2cs/2dg), for the same reason and with the
// same hard-won detail: `removeProperty` alone only edits IODTNVRAM's
// in-memory copy, so the removal is flushed with IONVRAM-FORCESYNCNOW BEFORE
// the caller does anything that can break the boot. The 2026-10-12 boot loop
// was paid for by getting that order wrong.
//
// `why` is set on every path, because "it did not arm" and "it armed and the
// patch did nothing" must be distinguishable from NVRAM alone (rule 14):
//   0 armed and consumed    1 /options unreachable
//   2 variable absent       3 variable present but a literal "0"
//   4 armed, but the clear did not take - the next boot is armed too
bool rbgpu_nvram_consume_oneshot(const char *var, uint32_t *why) {
    auto out = [why](uint32_t v) { if (why) *why = v; };
    IORegistryEntry *options = g_rbgpuOptions;
    if (!options) {
        options = IORegistryEntry::fromPath("/options", gIODTPlane);
        if (!options) {
            out(1);
            return false;
        }
        g_rbgpuOptions = options;   // retained by fromPath; kept for the boot
    }

    OSObject *val = options->getProperty(var);
    if (!val) {
        out(2);
        return false;
    }
    // `nvram var=1` stores an OSData, so check the first byte rather than
    // expecting a string - the 2cs lesson, unchanged.
    if (auto data = OSDynamicCast(OSData, val)) {
        if (data->getLength() >= 1 &&
            *static_cast<const char *>(data->getBytesNoCopy()) == '0') {
            out(3);
            return false;
        }
    } else if (auto str = OSDynamicCast(OSString, val)) {
        if (str->isEqualTo("0")) {
            out(3);
            return false;
        }
    }

    // 2qy: boot 154 measured this FAILING for `rbgpu-mcreg` while the
    // byte-identical `rbgpu-arm` path cleared in the same boot, and the
    // variable survived to live NVRAM (`nvram rbgpu-mcreg` still read 1
    // afterwards). The cause is NOT established. What is established is that
    // the old code then `return true`d anyway, so the caller ran its risky
    // operation with `why = 4` recorded and **no rescue in force** - had that
    // boot hung, the next one would have hung too, which is the exact failure
    // the one-shot exists to prevent.
    //
    // So: retry, and then FAIL CLOSED. A gate that cannot arm its own rescue
    // must refuse, not proceed and report. The retry is three rounds because
    // one forced sync may be coalesced with an earlier one in the same boot -
    // a hypothesis, not a measurement, and it is written here as such.
    bool cleared = false;
    for (int attempt = 0; attempt < 3 && !cleared; attempt++) {
        options->removeProperty(var);
        if (auto sync = OSString::withCString("now")) {
            options->setProperty("IONVRAM-FORCESYNCNOW-PROPERTY", sync);
            sync->release();
        }
        cleared = options->getProperty(var) == nullptr;
    }
    rbgpu_log("Peer: one-shot `%s` %s", var,
              cleared ? "consumed; clear OK (next boot comes up normally)"
                      : "clear FAILED after 3 attempts - REFUSING to run, "
                        "because the next boot would be armed too. Clear it "
                        "by hand: sudo nvram -d <var>");
    out(cleared ? 0 : 4);
    // Fail closed: `why = 4` now means "armed, but we did NOT run".
    return cleared;
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

    // 2hg: enable the accelerator WITHOUT a display pipe.
    //
    // 2hf: Metal's first buffer allocation sleeps in
    // IOGraphicsAccelerator2::acceleratorWaitEnabled because bit 0x2 of
    // accel+0xc78 is clear, and the only thing that sets it is
    // enableAccelerator(), whose only callers are the display machines'
    // display_mode_did_change. 2ct refuses the pipe, so it never runs and
    // every IOAccelResourceCreate blocks forever.
    //
    // THIS IS A SUBSTITUTION (2cy). If a Metal allocation succeeds after
    // this, that proves the FLAG was the gate - it does not prove a
    // display exists, and nothing downstream may be narrated as if one
    // does. Every step below says what it did, so a boot that changes
    // nothing cannot be mistaken for a boot where enabling did not help.
    //
    // The lock is not optional. enableAccelerator tail-calls
    // IOAccelEventMachine2::startHardwareProgressTimerUnlocked, and the
    // `Unlocked` suffix means the caller holds the lock; both real callers
    // run inside display_mode_did_change, which does. acceleratorWaitEnabled
    // sleeps on accel->0x88, so that is the lock. Calling an unlocked-variant
    // function with no lock held is the shape that produced the 2fz/2ga and
    // 2gg panics, so if the lock pointer is not plausible we do NOT call.
    if (checkKernelArgument("-rbgpuforceenable")) {
        if (!g_rb_enableAccelerator_2hg) {
            rbgpu_log("2hg: -rbgpuforceenable set but enableAccelerator was "
                      "never solved - nothing done");
            rbgpu_diag_set_num("ea_called", 0);
        } else if (!accelerator) {
            rbgpu_log("2hg: -rbgpuforceenable set but no accelerator "
                      "registered - nothing done");
            rbgpu_diag_set_num("ea_called", 0);
        } else {
            uintptr_t a = reinterpret_cast<uintptr_t>(accelerator);
            volatile uint32_t *flags = reinterpret_cast<volatile uint32_t *>(a + 0xc78);
            uint32_t before = *flags;
            IOLock *l = *reinterpret_cast<IOLock **>(a + 0x88);
            rbgpu_log("2hg: accel=%p flags(+0xc78)=0x%08X enabled=%d lock(+0x88)=%p",
                      accelerator, before, (before & 2) ? 1 : 0, l);
            rbgpu_diag_set_num("ea_flags_before", before);
            if (reinterpret_cast<uintptr_t>(l) < 0xffffff8000000000ULL) {
                rbgpu_log("2hg: lock at +0x88 is not a kernel pointer - REFUSING "
                          "to call enableAccelerator unlocked (2fz/2ga/2gg)");
                rbgpu_diag_set_num("ea_called", 0);
            } else if (before & 2) {
                rbgpu_log("2hg: already enabled - nothing to do, and any Metal "
                          "result this boot says nothing about this hook");
                rbgpu_diag_set_num("ea_called", 0);
            } else {
                IOLockLock(l);
                g_rb_enableAccelerator_2hg(accelerator);
                IOLockUnlock(l);
                uint32_t after = *flags;
                rbgpu_log("2hg: enableAccelerator CALLED under the lock - "
                          "flags 0x%08X -> 0x%08X, enabled=%d (SUBSTITUTED: this "
                          "is the plugin setting the gate, not a display)",
                          before, after, (after & 2) ? 1 : 0);
                rbgpu_diag_set_num("ea_called", 1);
                rbgpu_diag_set_num("ea_flags_after", after);
                char buf[48];
                snprintf(buf, sizeof(buf), "ea:%08X->%08X", before, after);
                rbgpu_nvram_note("rbgpu-ea", buf);
            }
        }
    }
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
            // 2ls: every 64th still meant a line every ~1.3 s at ~50
            // polls/s, which wrapped the kernel buffer in about two minutes
            // and cost three captures the plugin's own boot-time lines. The
            // exact count is in rbgpu-diag as req_0x14, which does not wrap.
            if (s_req14_seen <= 3 || (s_req14_seen % 4096) == 0) {
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

