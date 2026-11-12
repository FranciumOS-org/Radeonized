
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
#include "rb_defaults.h"
#include "rb_osver.h"
#include "rb_retire.h"   /* 2C2: RB_RETIRE_DRAIN_MAX for the dial's bound */
#include "rb_gpuname.h"  /* 2CA: RB_GPUNAME_MAX for the handover buffer */

// 2xx: the one-time migration off the pre-2xw consume-by-overwrite values.
// Defined with the rest of the arming gate, far below; called from start()
// before anything reads an NVRAM key for a decision.
static uint32_t rbgpu_gen_migrate(void);
#if RB_VBIOS_BAKED
#include "vbios_data.h"
#endif

/* 3fw: Linux's check_atom_bios() floor, and Apple's cosGetVbiosRomImage
 * check: an option ROM at least 0x49 bytes long starting 0x55 0xAA. Enough
 * to refuse a truncated or mistyped plist value; the ATOM table pointer is
 * validated by the consumer, not here. */
/* 3fxc: defined with the sentinel's thread_call machinery below. */
static void rbgpu_hwlibs_gate_arm(IOService *pciDevice);

static bool rbgpu_vbios_plausible(OSData *d) {
    if (!d || d->getLength() < 0x49) {
        return false;
    }
    auto b = static_cast<const uint8_t *>(d->getBytesNoCopy());
    return b != nullptr && b[0] == 0x55 && b[1] == 0xAA;
}

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
/* 2zd: the vblank-driven retire. rbgpu_retire_from_vbl_2zd() is what the
 * framebuffer reaches through callPlatformFunction below; the publisher is
 * called from the periodic refresh because the vblank must not take a lock. */
extern bool rbgpu_retire_from_vbl_2zd(uint64_t vbl_period_ns);
extern void rbgpu_retire_publish_2zd(void);
/* 2CF: occasions per vblank period, defined beside the arm that uses it. */
extern void     rbgpu_retire_sub_set_2CF(uint32_t want);
extern uint32_t rbgpu_retire_sub_get_2CF(void);
extern uint64_t rbgpu_retire_sub_sets_2CF(void);
/* 2C2: the retire-drain dial, defined in kern_start.cpp beside the loop that
 * reads it. */
static uint64_t g_rbgpu_drain_sets = 0;   /* 2C3 */
extern void rbgpu_retire_drain_set_2C2(uint32_t want);
extern uint32_t rbgpu_retire_drain_get_2C2(void);
/* 2C4: the retire-tick dial. */
extern void rbgpu_tick_us_set_2C4(uint32_t want);
extern uint32_t rbgpu_tick_us_get_2C4(void);
extern uint64_t rbgpu_tick_sets_get_2C4(void);
/* 2CA: the chip's own name and carve-out size, read from the shader-array
 * config and RCC_CONFIG_MEMSIZE in kern_start.cpp (which owns the aperture).
 * The framebuffer cannot do these reads - it runs no discovery and its
 * `ATY,fb_size` is a boot-arg - so they cross the collection boundary here. */
extern int rbgpu_gpuname_get_2CA(char *name, uint32_t namelen, uint32_t *mb,
                                 uint32_t *cu, uint32_t *why);
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

        /* 3br: the brand in the log is Radeonized. Nothing in tools/ greps
         * this prefix (checked), so it is display only; the bundle id, the
         * class names, the -rbgpu* boot-args and the rbgpu-* NVRAM keys are
         * deliberately UNCHANGED, because the EFI config.plist, this
         * machine's NVRAM and 60 000 lines of journal all cite them. */
        IOLog("Radeonized: %s\n", s_log_buf);
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

    // 2xw: ARMED BY DEFAULT, and the key is now only a disarm.
    //
    // Everything below this point is the one-shot machinery and is kept,
    // commented, as the record of what it was for - but it no longer decides
    // anything. The sentinel has already run in start(), so a boot that did
    // not survive the last one is stock here and `rbgpu_nvram_armed` answers
    // false for it; `nvram rbgpu-arm=0` is the durable disarm; and
    // `-rbgpustock` or `-rbgpuoff` turns it off from boot-args, which is the
    // one disarm that works even when NVRAM cannot be read.
    uint32_t why = 0;
    bool armed = rbgpu_nvram_armed("rbgpu-arm", &why);
    rbgpu_diag_set_num("arm_why", why);
    rbgpu_log("Peer: accelerator %s (arm_why %u: 0 default, 1 /options "
              "unreachable, 3 rbgpu-arm=0, 5 stock, 6 sentinel latch)",
              armed ? "ARMED by default (2xw)" : "NOT armed", why);
    return armed;
}

// Kept for the record: the one-shot form this replaced (2cs/2dg/2in). It is no
// longer called. Nothing in it was wrong - it was right for a feature that
// could cost the desktop, and the desktop has been stable since boot 215.
static bool rbgpuConsumeArmingOneShot() __attribute__((unused));
static bool rbgpuConsumeArmingOneShot() {

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

// 3rec-b: is this boot the installer or recoveryOS?
//
// 3rec declared `LiluAPI::AllowNormal` and that was the WRONG LEVER, measured
// in the Recovery boot the user photographed: the plugin logged
// "2xw: sentinel cleared after 60000 ms" there, so `RembrandtGPUPeer::start`
// ran with AllowNormal in force. Lilu's run mode gates Lilu's own callbacks -
// the patcher, the kext hooks - and nothing else. Our IOKitPersonalities
// match `IOResources`/`IOKit`, which exists in every boot there is, so IOKit
// starts this service whatever Lilu decided. The declaration is still right
// and stays; it was simply never the thing that was loading us.
//
// So ask the question ourselves, at `start`, where IOKit is up and no vnode
// work is needed (rule 27 is about init, but the cheapest test is also the
// one that needs no filesystem).
//
// Two independent signals, OR'd, each with its normal-boot control MEASURED
// on this machine today rather than assumed:
//
//   1. `/chosen` carries one of `boot-ramdmg-extents`, `container-dmg`,
//      `root-dmg`, `auth-root-dmg`. These are the four names Lilu 1.7.3's own
//      binary carries for this decision (read out of the EFI's Lilu with
//      `strings`), so we are using the same discriminator Lilu does.
//      CONTROL: on this normal boot, `ioreg -p IODeviceTree -n chosen -rl`
//      shows NONE of the four. An installer or recovery root IS a disk image,
//      which is why the property exists at all.
//
//   2. `/chosen`'s `boot-file` names a recovery or installer path. CONTROL:
//      this normal boot reads
//      `boot\System\Library\KernelCollections\BootKernelExtensions.kc`.
//      Lilu's binary carries `/com.apple.recovery.boot/prelinkedkernel` and
//      `/macOS Install Data/Locked Files/Boot Files/prelinkedkernel` as the
//      corresponding paths, which is where the two needles come from.
//
// Rule 14: every path out publishes a code, so a boot that comes up stock can
// be told from one that came up stock for the wrong reason. `RBGPURecovery`
// is 0 normal, or the signal number that fired.
//
// Failing SAFE here means answering "not recovery": an unreadable `/chosen`
// is the normal-boot answer, because a desktop that silently refuses to
// accelerate is the failure this project cannot debug from inside.
static uint32_t g_rbRecovery = 0;

static bool rbgpu_recovery_boot(void) {
    static int cached = -1;
    if (cached >= 0) {
        return cached == 1;
    }

    IORegistryEntry *chosen = IORegistryEntry::fromPath("/chosen", gIODTPlane);
    if (!chosen) {
        cached = 0;
        g_rbRecovery = 0;
        return false;
    }

    static const char *kDmgKeys[] = {
        "boot-ramdmg-extents", "container-dmg", "root-dmg", "auth-root-dmg"
    };
    for (size_t i = 0; i < arrsize(kDmgKeys); i++) {
        if (chosen->getProperty(kDmgKeys[i]) != nullptr) {
            g_rbRecovery = 1;
            rbgpu_log("3rec-b: /chosen carries `%s`: installer or recovery "
                      "boot; the plugin stands down", kDmgKeys[i]);
            break;
        }
    }

    if (g_rbRecovery == 0) {
        OSData *bf = OSDynamicCast(OSData, chosen->getProperty("boot-file"));
        if (bf != nullptr) {
            unsigned int len = bf->getLength();
            const char *p = static_cast<const char *>(bf->getBytesNoCopy());
            // The property is a NUL-terminated path in a sized blob; bound
            // every read by the blob, never by the NUL (rule 14 at the tool
            // boundary - a missing terminator must not walk off the end).
            if (p != nullptr && len > 0 && len < 1024) {
                static const char *kNeedles[] = {
                    "com.apple.recovery.boot", "macOS Install Data"
                };
                for (size_t n = 0; n < arrsize(kNeedles); n++) {
                    size_t nl = strlen(kNeedles[n]);
                    if (nl > len) {
                        continue;
                    }
                    for (unsigned int off = 0; off + nl <= len; off++) {
                        if (memcmp(p + off, kNeedles[n], nl) == 0) {
                            g_rbRecovery = 2;
                            rbgpu_log("3rec-b: /chosen boot-file names `%s`: "
                                      "installer or recovery boot; the plugin "
                                      "stands down", kNeedles[n]);
                            break;
                        }
                    }
                    if (g_rbRecovery != 0) {
                        break;
                    }
                }
            }
        }
    }

    chosen->release();
    cached = g_rbRecovery != 0 ? 1 : 0;
    return cached == 1;
}

bool RembrandtGPUPeer::start(IOService *provider) {
    if (checkKernelArgument("-rbgpuoff")) {
        rbgpu_log("Peer: disabled by -rbgpuoff; aborting start");
        return false;
    }

    // 3rec-b: Recovery and the installer come up stock. `rbgpu-arm=0` did NOT
    // fix the hang and `-rbgpuoff` DID, which puts it in this plugin before
    // any feature gate, and `-rbgpuoff` returns false from exactly here - so
    // this is the one line that reproduces the measured good boot.
    if (rbgpu_recovery_boot()) {
        rbgpu_diag_set_num("recovery", g_rbRecovery);
        rbgpu_log("Peer: installer/recovery boot (signal %u); aborting start "
                  "so the environment comes up stock", g_rbRecovery);
        return false;
    }

    // 2xw: the sentinel runs before anything is armed, and a recovery boot
    // makes everything consulted from here on stock. The hooks installed in
    // processKernel ran before this point and are NOT covered - they are the
    // same code every boot since 2ks has run, and the latch exists for the
    // display chain, which is all downstream of here.
    // 2xx: correct the two keys a pre-2xw boot left holding "0" and write
    // the generation key, before anything reads a key for a decision.
    rbgpu_gen_migrate();

    rbgpu_sentinel_begin();

    // 2xw: publish the verdict for the OTHER kext to read.
    //
    // `RBAMDFramebuffer` has to answer the same question at probe time - is
    // the accelerator going to be armed? - because rule 38 says the panel
    // taken without one freezes. It can read the same NVRAM keys, but not the
    // sentinel latch: the latch is "1" both when the PREVIOUS boot failed and
    // while THIS boot is still proving itself, and which of the two it is
    // depends on whether the framebuffer probed before this point.
    //
    // 2wg measured the order - the framebuffer's `RBFBCrumbBoot` lags
    // `rbgpu-boot` by exactly one in two separate boots, so it reads the
    // counter before this function increments it and therefore probes first.
    // That is a measurement of two boots, not a guarantee, so instead of
    // relying on it this property makes the order irrelevant: if the
    // framebuffer finds it, the peer has already decided and its answer is
    // authoritative; if it does not, the peer has not run yet and a latch can
    // only be the previous boot's.
    setProperty("RBGPUStock", rbgpu_stock());

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

    /* 3fw: the board's VBIOS, which Apple's stack reads as `ATY,bin_image`.
     * A release build bakes none in. The installer extracts the image from
     * the machine's own ACPI VFCT table and writes it into this kext's
     * Info.plist, so it arrives here as this service's own property and the
     * only copy that ever moves is the one already on the owner's board.
     * A build with RB_VBIOS_BAKED still carries the development image, and
     * the bundle property wins over it so one can be swapped for the other
     * with no rebuild. */
    OSData *vbios = OSDynamicCast(OSData, getProperty("ATY,bin_image"));
    if (vbios && rbgpu_vbios_plausible(vbios)) {
        pciDevice->setProperty("ATY,bin_image", vbios);
        rbgpu_log("Peer: ATY,bin_image from the bundle, %u bytes",
                  vbios->getLength());
        rbgpu_diag_set_num("vbios_src", 1);
    } else {
        if (vbios) {
            rbgpu_log("Peer: the bundle's ATY,bin_image is not an option ROM "
                      "(%u bytes); ignoring it", vbios->getLength());
        }
        /* 3vb-c: before falling back to a baked image, ask the machine for its
         * own. AppleACPIPlatformExpert::getACPITableData("VFCT", 0) is a
         * dictionary lookup whose miss is a NULL, so this costs nothing when
         * the firmware does not publish the table, and when it does the
         * per-machine install step and its reboot both disappear. The OSData
         * comes back owned by US (the probe copies the dictionary's bytes),
         * so it is released here. Priority is deliberate: an image the
         * installer put in the bundle still WINS over this one, because that
         * is the override a user reaches for when the firmware's copy is
         * wrong. */
        bool acpi_took = false;
        {
            extern OSData *rbgpu_acpi_vbios_3vb(int *rc_out);
            int arc = -1;
            OSData *acpi = rbgpu_acpi_vbios_3vb(&arc);
            if (acpi) {
                if (rbgpu_vbios_plausible(acpi)) {
                    pciDevice->setProperty("ATY,bin_image", acpi);
                    rbgpu_log("Peer: ATY,bin_image from the ACPI VFCT table, "
                              "%u bytes", acpi->getLength());
                    rbgpu_diag_set_num("vbios_src", 3);
                    acpi->release();
                    acpi_took = true;
                }
                /* It walked and parsed but our own plausibility check says no.
                 * That is a DIFFERENT finding from "no table" and gets its own
                 * number rather than falling silently through (rule 14). */
                else {
                    rbgpu_log("Peer: the ACPI VFCT image (%u bytes) failed the "
                              "plausibility check; ignoring it",
                              acpi->getLength());
                    rbgpu_diag_set_num("acpivb_implausible", acpi->getLength());
                    acpi->release();
                }
            }
        }
        /* If the machine's own image was published, nothing below runs - and
         * in particular no baked development ROM is substituted for it. */
        if (!acpi_took) {
#if RB_VBIOS_BAKED
        auto baked = OSData::withBytes(vbios_data, vbios_data_size);
        if (baked) {
            pciDevice->setProperty("ATY,bin_image", baked);
            baked->release();
            rbgpu_log("Peer: ATY,bin_image from the build, %zu bytes",
                      vbios_data_size);
            rbgpu_diag_set_num("vbios_src", 2);
        }
#else
        /* 2eh: absence is not a result. Say it, so a boot with no VBIOS is
         * never mistaken for one where the image was rejected downstream. */
        rbgpu_log("Peer: NO ATY,bin_image published - this build bakes none "
                  "in, the bundle carries none, and the ACPI VFCT route "
                  "refused (see `acpivbios` for which way). Run the "
                  "installer's VBIOS step, or `sudo tools/efi.sh vbios`.");
        rbgpu_diag_set_num("vbios_src", 0);
#endif
        }
    }

    /* 3fxc: RBGPULoad and LoadHWServices go out immediately again.
     *
     * 3fxb withheld them until our HWLibs patches landed and boot 541 proved
     * that circular: HWLibs' only loader is IOKit matching on HWServicesNavi,
     * which is precisely what these two properties start, so the gate waited
     * 20 000 ms for something it was itself preventing. `hwlibs_gate_ok 0`,
     * `hwlibs_patched` absent, HWLibs not in `kextstat`, HWServicesNavi
     * instances 0.
     *
     * It also showed a second defect worth keeping in mind anywhere a property
     * is published late: setProperty on a node whose matching has already
     * settled triggers NOTHING. The pair was on the PCI node and HWServices
     * still never started. A late publish needs registerService(), which is
     * what 3fxc's own gate does below.
     *
     * The ordering lever is LoadAccelerator, not these. See 2cs further down. */
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
        /* 3fxc: published through the HWLibs gate rather than here.
         *
         * This property matches our own accelerator personality, which IOKit
         * starts - a second instance, independent of the one HWServices
         * spawns internally inside its own start(). An accelerated boot has
         * had TWO instances since 2cs, so this path is not new; what is new is
         * that it no longer races. HWServices' internal accelerator enters
         * start() 65-68 ms before our HWLibs patches apply (boots 536, 539,
         * 540, all verified armed) and dies at createHWInterface with an
         * unpatched deviceTypeTable. Ours can simply wait for the patch,
         * because nothing HWLibs needs is waiting on it.
         *
         * The gate publishes and then calls registerService(), which boot 541
         * showed is required: a property set after matching has settled starts
         * nothing by itself. */
        rbgpu_hwlibs_gate_arm(pciDevice);
    }
    rbgpu_diag_set_num("armed", armed ? 1 : 0);

    // 2w5: the accelerator-gate probe is armed HERE, and the reason is a lost
    // boot. 2w3 put the call beside `rbgpu_heartbeat_start()` in
    // kern_start.cpp because a grep found that name - and never read what
    // gated it: the heartbeat sits inside `if (stage >= 40)` AND inside
    // `if (checkKernelArgument("-rbgpubeat"))`, and `-rbgpubeat` has not been
    // in boot-args since 2fg answered its question. So boot 186 ran with
    // `-rbgpuaccelprobe` set and build D8B1115C loaded and produced not one
    // `ax*` key, including the unconditional `ax_say`. Rule 16, on the
    // instrument's own call site: a probe needs a call site that runs
    // whenever ITS arg is set, not one borrowed from a neighbour.
    //
    // This line runs in the peer's start() on every boot, armed or not, and
    // the probe's own `-rbgpuaccelprobe` check is the only gate it needs.
    rbgpu_accelprobe_start();
    rbgpu_rate_start_2wx();
    /* 2xg: same reasoning as 2w5's fix above - its own arg is its only gate,
     * and this line runs in the peer's start() on every boot. The one-shot is
     * consumed inside, BEFORE the first tick is armed (rule 37), so a panic
     * costs exactly one power cycle. */
    rbgpu_schedtick_start_2xg();

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

/* 2D1: A DIAL'S WRITE MUST PUBLISH, AND `scheduleRefreshDiag()` CANNOT
 * PROMISE THAT.
 *
 * That function DROPS a request inside the 250 ms window - 2ez's fix for a
 * refresh that ran back to back under `g_diagLock` and was indistinguishable
 * from a wedge, and it stays. But nothing re-arms a dropped request, and the
 * refresh is the ONLY thing that calls `rbgpu_retire_publish_2zd()`, whose own
 * key writes land inside their own window and are dropped in turn. So on a
 * quiet desktop the chain TERMINATES: no log line, no refresh, and
 * `RBGPUDiag` can be arbitrarily stale.
 *
 * Boot 426 is what that costs. `subret 4` landed - `vblret_subrecv` 4,
 * `vblret_sub` 4, the subdivision running at 3 interior retires per frame -
 * and the tool's own 3 s poll of `vblret_subsets` saw a frozen snapshot,
 * reported "the set did not arrive", exited 3, and the `&&` took
 * `paired.sh` with it. Boot 424 had probes logging throughout and the same
 * poll passed, which is why 2C5's "poll the set-counter" prescription looked
 * strictly better than a sleep: it is the SAME premise with a loop around it,
 * and the premise - that something refreshes the snapshot - was never
 * established.
 *
 * So this defers to the end of the current window instead of dropping, and
 * CLAIMS that slot so a second dial write defers to the one after. The
 * throttle's guarantee is unchanged - never more than one refresh per 250 ms -
 * and there is no steady-state cost, because only a userspace dial write calls
 * this. A log line still uses `scheduleRefreshDiag()` and is still dropped;
 * that is the case 2ez is about.
 *
 * NOT CLAIMED, but now a live candidate: this is the shape of 2C2's
 * undiagnosed drain bug, where `drainset 1` returned SUCCESS three times and
 * `vblret_drain` kept reading 8. That was excluded because the key "has a
 * second writer, several times a second from the live global" - and
 * `rbgpu_retire_publish_2zd()` has exactly ONE call site, inside the refresh.
 * The dismissal rested on a false premise about its frequency. Settling it
 * means re-running that A/B on a build with this fix, not reasoning further.
 */
void RembrandtGPUPeer::refreshDiagSoon() {
    if (!refreshDiagCall) {
        return;
    }
    uint64_t now = mach_absolute_time();
    uint64_t interval = 0;
    nanoseconds_to_absolutetime(250ull * 1000ull * 1000ull, &interval);
    if (g_lastRefreshAt && (now - g_lastRefreshAt) < interval) {
        g_lastRefreshAt += interval;        /* claim the next slot */
        thread_call_enter_delayed(refreshDiagCall, g_lastRefreshAt);
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
    // 2xg/2xh: the scheduler tick's arm check, ahead of everything, because a
    // boot carrying it can PANIC and because boot 213 proved these three are
    // what the reading turns on. `tick_armed` 1 means it is ticking;
    // `tick_oneshot` says why not (0 consumed, 1 /options, 2 absent,
    // 3 literal "0", 4 the overwrite would not take); and `tick_solved`
    // separates "the route callback has not run yet" from "solveSymbol
    // failed", which `tw=7` in rbgpu-sched cannot do on its own.
    "tick_armed", "tick_oneshot", "tick_solved", "tick_us",
    // 2w3: the three bits behind kIOReturnNotReady, at the very top because
    // every boot that carries them wedges and the note truncates at 512.
    // `ax<i>_gate` is the whole verdict in one digit (0 all open, 1 accel not
    // enabled, 2 WSAA defer, 3 pipe not active); the sentences are further
    // down and are allowed to fall off the end.
    "ax0_gate", "ax0_enabled", "ax0_active", "ax0_defer",
    "ax0_vram", "ax0_vmap", "ax0_fbs", "ax0_n",
    // 2w8: the branch selectors inside AMD's initFramebufferResource. 2w7
    // answered the three-gate question, so those keys move down and these
    // take the top of the note: `fi340`/`fi348` are what configureDisplay
    // must fill and `r380` is the value the hook returns.
    "ax0_fi340", "ax0_fi348", "ax0_fi34c", "ax0_b35d",
    "ax0_r380", "ax0_b35e", "ax0_b35f",
    "ax0_w", "ax0_h", "ax0_bpp", "ax0_fmt", "ax0_hw330",
    "ax2_gate", "ax2_enabled", "ax2_active", "ax2_defer",
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
    // 2v9: THE ONE KEY THE WHOLE DISPLAY-PIPE CHAIN HANGS ON, so it goes
    // ahead of everything else in this block. 2v9 read the chain to its end:
    // IOPTransaction::Submit fails at its first step and logs NOTHING iff
    // IOPresentment's plane count is 0; that count is
    // CFArrayGetCount(caps["DeviceArray"]); DeviceArray is NULL iff
    // CreatePlaneDictionary is, which happens iff caps->0x434 != 1; and
    // 0x434 is set by one instruction, only when the type-0x10 (Scaler)
    // 0x711 query returns IOReturn 0, a non-zero out-length and a non-zero
    // count. agdc_extcap_answered cannot settle that - it is a last-wins
    // total and cannot say WHICH types were answered (rule 14).
    //   bits  7..0  our return      (0 = success)
    //   bits 15..8  the count we published at +0x08  (must be >= 1)
    //   bits 23..16 the framebuffer index we were handed at +0x00, which
    //               rb_agdc_extcap refuses unless it is 0
    //   bits 47..32 the out-length, which Apple fails the gather on if 0
    // So 0x196c_00_01_00 is the shape that makes the chain work; anything
    // with a non-zero low byte, a zero count byte, or a non-zero index byte
    // names the defect outright. ABSENT means the scaler was never asked,
    // which agdc_extcap_calls distinguishes.
    "agdc_ec_scaler",
    // The other four cost nothing to answer and nothing downstream depends
    // on them, so they sit after the gamma provenance.

    // 2v0: the gamma reply's own shape, read back out of the buffer we wrote,
    // so a wedged boot's NVRAM identifies the build without kmutil. 2 entries
    // and elems2 = 0x200 is the two-entry build; absent means the one-entry
    // build (or a gamma query that never arrived).
    "agdc_gamma_entries", "agdc_gamma_elems1", "agdc_gamma_elems2",
    "agdc_ec_csc", "agdc_ec_gamma", "agdc_ec_degamma", "agdc_ec_cursor",
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
                /* 2zc: "0x" IS NOT DECORATION. Every diag number is printed
                 * in hex, and a value whose digits happen to all be 0-9
                 * reads as a perfectly plausible decimal: boot 265 published
                 * tick_us=3640 for a tick armed at 13888 us (0x3640), which
                 * looked exactly like a refused or clamped parse, and 2z4's
                 * "pg_ticks=24" was really 36 ticks. The base has to be in
                 * the value, because the reader cannot tell from the digits
                 * (rule 26 - an instrument must say what it means). */
                int n = snprintf(body + len, kBodyLimit - len, "%s%s=0x%llx",
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

// 2xw: THE CONFIGURATION IS THE DEFAULT NOW.
//
// Boots 215-231 ran the same forty-five `-rbgpu*` tokens and five NVRAM
// one-shots, and the machine is a usable accelerated Mac. Re-typing that set
// before every boot is no longer a safety measure, it is just a way to lose a
// boot to a token nobody noticed was missing (rule 15's shape, applied to
// boot-args). So `src/common/rb_defaults.h` holds that exact set and
// everything in it is ON with nothing in boot-args.
//
// What replaces the one-shots as the safety net is the BOOT SENTINEL below.
// What is deliberately NOT replaced: `-rbgpuoff` still stops the plugin dead,
// and it is still the documented rescue.
static int g_rbStock      = -1;   // -1 not decided, 1 stock, 0 armed
static int g_rbSafeLatch  = -1;   // 1 the latch was set when the peer looked
static int g_rbGenPresent = -1;   // -1 not looked, 1 RB_GEN_KEY exists
static thread_call_t g_rbSentinelCall = nullptr;

/* 3fxc: the HWLibs gate. LoadAccelerator matches our own accelerator
 * personality, so withholding it delays OUR accelerator instance without
 * delaying HWServices - and therefore without delaying the IOKit matching
 * that is the only thing which loads HWLibs. 3fxb withheld RBGPULoad instead
 * and boot 541 showed that waits on something the gate itself prevents. */
#define RB_HWLIBS_GATE_MS        20000u   /* total budget before giving up */
#define RB_HWLIBS_GATE_STEP_MS      25u   /* poll interval */

extern int rbgpu_hwlibs_patched(void);    /* set on processHWLibs' last line */

static thread_call_t g_rbHwlibsGateCall = nullptr;
static IOService    *g_rbHwlibsGateDev  = nullptr;
static uint32_t      g_rbHwlibsGateMs   = 0;
static int           g_rbHwlibsGateDone  = 0;

/* 3rgr-c (2026-11-12): the gate has to survive a boot that does not.
 *
 * Four armed boots have stalled with the log store stopping between 5.3 s and
 * 10.2 s, and the gate's own window - `hwlibs_gate_ms` measured 5700, 5900,
 * 5975 and 10875 across good boots - lies inside that span. That is a
 * coincidence of windows and nothing more: the gate withholds OUR accelerator
 * personality only, not HWServices (the 3fxc comment above, and boot 541),
 * so no mechanism connects it to a system-wide stall. It is named here
 * because it cannot currently be RULED OUT either, and that is the defect
 * being fixed: `rbgpu-diag-prev` is capped near 520 bytes and the three
 * hwlibs_gate keys are past the cap, so a stalled boot leaves no record of
 * whether the gate ever closed. The question "was the gate still polling when
 * the machine stopped?" has been unanswerable for four boots.
 *
 * So the gate marks the synchronous trail, which is written on the calling
 * thread and flushed immediately and therefore survives a wedge (2eo-2fg).
 * Seven marks at most, against a 192-byte trail that currently holds
 * fourteen, so the cap is not at risk:
 *
 *   600  armed, LoadAccelerator withheld
 *   601  still polling, once per 5 s (at most four)
 *   602  published because HWLibs was patched
 *   603  published because the 20 s budget ran out
 *   604  could not publish - no PCI device (rule 14's third case)
 *
 * A stalled boot whose trail ends at 600 or 601 puts the gate open at the
 * moment the machine stopped; one that reaches 602 and stalls anyway
 * eliminates the gate, which is the more likely outcome and is worth the same
 * one boot. Either way the next stall answers it instead of adding a fifth
 * unreadable data point. Off with -rbgpunogates, like every other gate. */
#define RB_HWLIBS_GATE_MARK_MS   5000u    /* trail mark every 5 s of polling */

static uint32_t g_rbHwlibsGateMarked = 0; /* ms at the last 601 mark */

static void rbgpu_hwlibs_gate_mark(uint32_t stage, const char *what) {
    static int nogate = -1;
    if (nogate < 0) {
        nogate = rbgpu_arg("-rbgpunogates") ? 1 : 0;
    }
    if (nogate) {
        return;
    }
    rbgpu_gate_nvram(stage, what);
}

/* Publishes LoadAccelerator, exactly once, and says which of the two reasons
 * got us here. Both reasons publish: the gate orders the boot, it never
 * cancels it - a boot with no accelerator at all would be worse than an
 * unordered one. */
static void rbgpu_hwlibs_gate_publish(const char *why, uint32_t ok) {
    if (g_rbHwlibsGateDone) {
        return;
    }
    g_rbHwlibsGateDone = 1;

    IOService *dev = g_rbHwlibsGateDev;
    if (!dev) {
        /* Rule 14: a gate that cannot publish must not look like one that
         * published and was ignored downstream. */
        rbgpu_diag_set_num("hwlibs_gate_ok", 2);
        rbgpu_hwlibs_gate_mark(604, "hwlibs gate: no PCI device, nothing published");
        rbgpu_log("3fxc: the gate has NO PCI device - LoadAccelerator was "
                  "never published and no accelerator will start this boot");
        return;
    }

    dev->setProperty("LoadAccelerator", kOSBooleanTrue);

    /* Boot 541: a property set after the node's matching has settled starts
     * nothing. Ask for the re-match explicitly. Skipped when the gate never
     * had to wait, because matching has not settled yet at that point and the
     * original 2cs behaviour is exactly right. */
    uint32_t reg = 0;
    if (g_rbHwlibsGateMs > 0) {
        dev->registerService();
        reg = 1;
    }

    rbgpu_diag_set_num("hwlibs_gate_ms", g_rbHwlibsGateMs);
    rbgpu_diag_set_num("hwlibs_gate_ok", ok);
    rbgpu_diag_set_num("hwlibs_gate_regsvc", reg);
    /* 3rgr-c: the trail's last word on the gate. The ms is in the text, so
     * the next boot reads both how it closed and when, without the diag
     * table the cap truncates. */
    {
        static char gm[64];
        snprintf(gm, sizeof(gm), "hwlibs gate closed ok=%u ms=%u regsvc=%u",
                 ok, g_rbHwlibsGateMs, reg);
        rbgpu_hwlibs_gate_mark(ok ? 602 : 603, gm);
    }
    rbgpu_log("3fxc: published LoadAccelerator after %u ms because %s "
              "(registerService %u)", g_rbHwlibsGateMs, why, reg);
    if (!ok) {
        rbgpu_log("3fxc: THE GATE TIMED OUT - HWLibs was not patched in %u ms, "
                  "so our accelerator starts in the same unordered state as "
                  "Apple's and may lose the same race.", RB_HWLIBS_GATE_MS);
    }
}

static void rbgpu_hwlibs_gate_action(thread_call_param_t, thread_call_param_t) {
    if (rbgpu_hwlibs_patched()) {
        rbgpu_hwlibs_gate_publish("HWLibs is patched", 1);
        return;
    }
    if (g_rbHwlibsGateMs >= RB_HWLIBS_GATE_MS) {
        rbgpu_hwlibs_gate_publish("the gate's budget ran out", 0);
        return;
    }
    g_rbHwlibsGateMs += RB_HWLIBS_GATE_STEP_MS;
    /* 3rgr-c: one mark per 5 s, so a trail ending here says the gate was
     * still open when the machine stopped, and says roughly when. */
    if (g_rbHwlibsGateMs - g_rbHwlibsGateMarked >= RB_HWLIBS_GATE_MARK_MS) {
        g_rbHwlibsGateMarked = g_rbHwlibsGateMs;
        static char pm[64];
        snprintf(pm, sizeof(pm), "hwlibs gate still polling at %u ms",
                 g_rbHwlibsGateMs);
        rbgpu_hwlibs_gate_mark(601, pm);
    }
    uint64_t deadline = 0;
    clock_interval_to_deadline(RB_HWLIBS_GATE_STEP_MS, kMillisecondScale, &deadline);
    thread_call_enter_delayed(g_rbHwlibsGateCall, deadline);
}

/* 3rgr-d (2026-11-12): the edge, not the level.
 *
 * The user asked whether there is a time at which Apple's stack is "100%
 * ready or timed out" that we could simply wait out. There is not, and this
 * machine's own numbers say so: `hwlibs_gate_ms` has read 5700, 5900, 5975
 * and 10875 across four good boots, a spread of nearly 2x, so any constant
 * chosen from them is either short on the 10875 boot or a tax on the other
 * three. A fixed wait is also strictly the current gate MINUS its early exit,
 * because the gate already means "until ready, up to 20 s".
 *
 * The better answer was already in the tree. `g_rb_hwlibs_patched` is set on
 * the last line of processHWLibs, which is an EDGE at a known instant; the
 * 25 ms poll is only how the gate observed it. So processHWLibs calls this
 * instead, and the gate fires on the edge.
 *
 * What this is worth, stated honestly: at most one poll interval, 25 ms. It
 * does NOT shorten the 5700 ms, because that time is HWLibs genuinely not
 * being loaded and patched yet, not the gate failing to notice. The gate has
 * never been a guess at a duration - it is an event wait with a backstop, and
 * that is the answer to "why is this driver time-based": mostly it is not.
 * The real guesses are the two /options retry loops, bounded at 1 s and 500
 * ms. This change is correctness, not a stall fix, and nothing here claims to
 * address 3rgr-b's stall.
 *
 * The poll is DELIBERATELY left in place as the backstop. If this kick never
 * arrives - processHWLibs not reached, the call not yet allocated, Lilu's
 * patcher context declining to enqueue - the gate closes exactly as it does
 * today, so the worst case is the behaviour we already measured. Safe to call
 * at any time and any number of times: before the gate is armed it is a nop,
 * after it has closed rbgpu_hwlibs_gate_publish's own guard discards it, and
 * a double fire publishes once. */
void rbgpu_hwlibs_gate_kick(void) {
    if (g_rbHwlibsGateDone || !g_rbHwlibsGateCall) {
        return;
    }
    rbgpu_diag_set_num("hwlibs_gate_kick", 1);
    /* Cancel the pending delayed poll first: re-entering a call that is
     * already queued with a deadline is the footgun 2fy recorded, and the
     * cancel makes the enqueue unambiguous. A cancel that loses the race
     * against the poll firing is harmless - both paths end in the same
     * guarded publish. */
    thread_call_cancel(g_rbHwlibsGateCall);
    thread_call_enter(g_rbHwlibsGateCall);
}

static void rbgpu_hwlibs_gate_arm(IOService *pciDevice) {
    g_rbHwlibsGateDev = pciDevice;

    /* Already patched - a machine where HWLibs got there first. Publish on
     * this thread, so that boot is byte-identical to 2cs's behaviour, costs no
     * latency, needs no registerService, and reads `hwlibs_gate_ms` 0. */
    if (rbgpu_hwlibs_patched()) {
        rbgpu_hwlibs_gate_publish("HWLibs was already patched", 1);
        return;
    }

    g_rbHwlibsGateCall = thread_call_allocate(rbgpu_hwlibs_gate_action, nullptr);
    if (!g_rbHwlibsGateCall) {
        /* No timer, so no gate. Publish immediately rather than never: an
         * unordered boot is what we have today, a boot with no accelerator at
         * all is a regression. */
        rbgpu_diag_set_num("hwlibs_gate_alloc", 0);
        rbgpu_hwlibs_gate_publish("thread_call_allocate failed, so there is no "
                                  "gate to wait on", 0);
        return;
    }
    rbgpu_diag_set_num("hwlibs_gate_alloc", 1);
    rbgpu_hwlibs_gate_mark(600, "hwlibs gate armed, LoadAccelerator withheld");
    rbgpu_log("3fxc: gate armed - LoadAccelerator is withheld until HWLibs is "
              "patched, or %u ms, whichever comes first", RB_HWLIBS_GATE_MS);

    uint64_t deadline = 0;
    clock_interval_to_deadline(RB_HWLIBS_GATE_STEP_MS, kMillisecondScale, &deadline);
    thread_call_enter_delayed(g_rbHwlibsGateCall, deadline);
}

// 2xx: /options, cached, for the generation key and the sentinel.
static IORegistryEntry *rbgpu_options(void) {
    if (!g_rbgpuOptions) {
        g_rbgpuOptions = IORegistryEntry::fromPath("/options", gIODTPlane);
    }
    return g_rbgpuOptions;
}

// 2xx: does RB_GEN_KEY exist? Its PRESENCE is the whole signal - the value is
// there only so a human reading `nvram -p` can see which generation wrote it.
bool rbgpu_gen_present(void) {
    if (g_rbGenPresent < 0) {
        IORegistryEntry *options = rbgpu_options();
        if (!options) {
            // Unreadable /options cannot hold a disarm either (why 1 above),
            // so the answer that changes nothing is "not present".
            return false;
        }
        g_rbGenPresent = options->getProperty(RB_GEN_KEY) != nullptr ? 1 : 0;
    }
    return g_rbGenPresent == 1;
}

// 2xx: the one-time migration off the pre-2xw consume-by-overwrite values.
//
// Two keys hold a literal "0" that our own older builds wrote as the way of
// spending a one-shot, and 2xw's reader treats that as the user's disarm. The
// generation rule already ignores those, so this is what makes the rule stop
// being needed: rewrite the stale values, then write the generation key.
//
// The ORDER is the whole safety property and it is the 2dg order again: the
// keys are corrected FIRST and RB_GEN_KEY is written LAST, so a boot that
// dies in the middle leaves the generation key absent and the next boot still
// ignores the stale zeros. Nothing here can disarm anything - every write
// moves a key from "0" to "1", which is the armed direction - so it is safe
// to run on a stock boot too, and it does, because a stock boot is exactly
// the boot that most needs the next one to come up clean.
//
// A disarm the user types AFTER this runs is obeyed: the key will read "0"
// with RB_GEN_KEY present, and rb_key_disarms says yes.
static uint32_t rbgpu_gen_migrate(void) {
    IORegistryEntry *options = rbgpu_options();
    uint32_t fixed = 0;

    if (!options) {
        rbgpu_diag_set_num("gen", 3);   // /options unreachable
        return 0;
    }
    if (options->getProperty(RB_GEN_KEY)) {
        g_rbGenPresent = 1;
        rbgpu_diag_set_num("gen", 1);   // already migrated, nothing to do
        return 0;
    }

    for (int i = 0; i < RB_LEGACY_ZERO_COUNT; i++) {
        const char *var = rb_legacy_zero_keys[i];
        bool zero = false;
        if (OSObject *val = options->getProperty(var)) {
            if (auto data = OSDynamicCast(OSData, val)) {
                zero = data->getLength() >= 1 &&
                       *static_cast<const char *>(data->getBytesNoCopy()) == '0';
            } else if (auto str = OSDynamicCast(OSString, val)) {
                zero = str->isEqualTo("0");
            }
        }
        if (!zero) {
            continue;
        }
        if (auto one = OSString::withCString("1")) {
            options->setProperty(var, one);
            one->release();
            fixed++;
            rbgpu_log("2xx: `%s` held the \"0\" a pre-2xw boot wrote when it "
                      "spent the one-shot; rewriting it to \"1\" so it stops "
                      "reading as a disarm.", var);
        }
    }

    if (auto gen = OSString::withCString(RB_GEN_CURRENT)) {
        options->setProperty(RB_GEN_KEY, gen);
        gen->release();
    }
    if (auto sync = OSString::withCString("now")) {
        options->setProperty("IONVRAM-FORCESYNCNOW-PROPERTY", sync);
        sync->release();
    }
    // Read the generation key back: a migration that did not land has to say
    // so, because the rule above keeps working and the next boot repeats this
    // (rule 14 - the write and the silent failure must not look the same).
    bool landed = options->getProperty(RB_GEN_KEY) != nullptr;
    g_rbGenPresent = landed ? 1 : 0;
    rbgpu_diag_set_num("gen", landed ? 2 : 0);
    rbgpu_diag_set_num("gen_fixed", fixed);
    rbgpu_log("2xx: migration %s, %u stale key(s) rewritten",
              landed ? "written" : "FAILED to read back", fixed);
    return fixed;
}

// 2xz: WHICH macOS, AND WHETHER ANYTHING HERE WAS MEASURED ON IT.
//
// `version_major` is the Darwin major - the kernel's own exported integer,
// which is also what Lilu's getKernelVersion() returns. Both kexts read THAT
// and not their own notion of the version, because rule 38 is only satisfied
// if the two halves answer this identically: the plugin gets the declaration
// from Lilu's kern_util.hpp, which already declares it, and the framebuffer -
// which has no Lilu - gets it from <libkern/version.h>. Same symbol.
//
// `rb_osver.h` holds the matrix and the reasoning. The short version: this
// kext LOADS from Ventura up, because everything it does by symbol name is
// version-independent and rule 1 already makes each of those fail safe on its
// own. But the offsets into Apple's driver objects - pm4+0x30, hw->0x368,
// hw+0x3b0, the [0x148]/[0x118]/[0x120] accessors - were MEASURED on Sequoia
// and re-measured in use on Tahoe, and they cannot be inferred for an OS
// nobody has booted this on. Writing a ring pointer through an offset that
// moved is not a failed experiment, it is an unbounded write into another
// driver's object.
//
// So on an OS with no RB_OSD_KEXTOFF claim the whole default-on table comes up
// OFF - the same state `-rbgpustock` produces, which is a live, usable macOS
// with our framebuffer declining the panel - and the boot says exactly that.
// `-rbgpuosforce` overrides it for the boot that goes and measures. That boot
// is how a row earns its bit; nothing earns it from this file.
static int g_rbOsMeasured = -1;   // -1 not decided, 1 measured here, 0 not
static int g_rbOsForced   = 0;

bool rbgpu_os_measured(void) {
    if (g_rbOsMeasured < 0) {
        const int major = version_major;
        const bool ok   = rb_os_derived(major, RB_OSD_KEXTOFF) != 0;
        g_rbOsForced    = (!ok && checkKernelArgument("-rbgpuosforce")) ? 1 : 0;
        g_rbOsMeasured  = (ok || g_rbOsForced) ? 1 : 0;

        const struct rb_os_row *row = rb_os_row_for(major);
        rbgpu_log("2xz: Darwin %d (%s)%s%s", major, rb_os_name(major),
                  row ? " - " : "", row ? row->note : "");
        if (ok) {
            rbgpu_log("2xz: this OS claims the measured object offsets; the "
                      "default-on table applies normally.");
        } else if (g_rbOsForced) {
            rbgpu_log("2xz: nothing in this tree was measured on Darwin %d, "
                      "and -rbgpuosforce is in boot-args, so the default-on "
                      "table applies ANYWAY. Every offset into an Apple "
                      "driver object is now a guess - read every counter "
                      "before trusting any of them, and expect a stock boot "
                      "from the sentinel if this does not survive 60 s.",
                      major);
        } else {
            rbgpu_log("2xz: nothing in this tree was measured on Darwin %d, so "
                      "coming up STOCK rather than writing offsets measured on "
                      "another OS. The plugin is alive and every probe still "
                      "reports; add -rbgpuosforce to arm it and measure.",
                      major);
        }
        rbgpu_diag_set_num("os_major", static_cast<uint32_t>(major));
        rbgpu_diag_set_num("os_derived", rb_os_derived_mask(major));
        rbgpu_diag_set_num("os_forced", static_cast<uint32_t>(g_rbOsForced));
        rbgpu_diag_set_num("os_measured", static_cast<uint32_t>(g_rbOsMeasured));
    }
    return g_rbOsMeasured != 0;
}

// Stock means "behave as though none of our tokens were in boot-args". It is
// set by `-rbgpustock`, by `-rbgpuoff`, by the sentinel finding its latch, and
// since 2xz by running on an OS none of the measured offsets came from.
bool rbgpu_stock(void) {
    if (g_rbStock < 0) {
        g_rbStock = (checkKernelArgument("-rbgpustock") ||
                     checkKernelArgument("-rbgpuoff") ||
                     !rbgpu_os_measured()) ? 1 : 0;
    }
    return g_rbStock != 0;
}

// The one gate every feature goes through.
//
// A token in the default table is on unless its negative is in boot-args; a
// token that is not in the table keeps the old meaning exactly, so an
// experiment stays opt-in. A positive token still in boot-args is harmless
// and redundant, which is what lets this build be installed without editing
// boot-args first.
bool rbgpu_arg(const char *tok) {
    if (!rb_default_on(tok)) {
        return checkKernelArgument(tok);
    }
    if (rbgpu_stock()) {
        return false;
    }
    char off[64];
    if (!rb_off_token(tok, off, sizeof(off))) {
        // Cannot happen for a table entry - the hosttest asserts every one of
        // them derives a negative - but if it ever did, the feature stays on
        // and says so rather than silently flipping.
        rbgpu_log("2xw: no negative token derivable for `%s`; leaving it ON", tok);
        return true;
    }
    return !checkKernelArgument(off);
}

// Armed-by-default, with the NVRAM key kept as a DURABLE DISARM.
//
// The one-shots were armed by the key's presence and consumed on use, so a
// boot that broke the desktop could not repeat. That direction is now
// inverted: the feature is armed by default and `nvram <var>=0` turns it off
// until the user says otherwise. The key is read, never written, so nothing
// here can produce the 2wj failure where a key would not clear.
//
// `why`, on every path (rule 14):
//   0 armed by default            1 /options unreachable, armed anyway
//   3 the key reads "0" - disarmed by hand
//   5 stock (`-rbgpustock` / `-rbgpuoff`)
//   6 the boot sentinel's latch was set, so the previous boot did not survive
//   8 the key reads "0" but RB_GEN_KEY is absent, so that "0" is our own
//     pre-2xw consume-by-overwrite artifact and not a disarm (2xx)
//   9 this OS has no measured offsets and -rbgpuosforce is absent (2xz).
//     Distinct from 5 on purpose: "the user asked for stock" and "the kext
//     refused to guess at another OS's offsets" are different facts and a
//     single code for both is rule 14's failure, not a tidier table.
bool rbgpu_nvram_armed(const char *var, uint32_t *why) {
    auto out = [why](uint32_t v) { if (why) *why = v; };
    if (rbgpu_stock()) {
        if (g_rbSafeLatch == 1) {
            out(6);
        } else if (!rbgpu_os_measured()) {
            out(9);
        } else {
            out(5);
        }
        return false;
    }
    IORegistryEntry *options = g_rbgpuOptions;
    if (!options) {
        options = IORegistryEntry::fromPath("/options", gIODTPlane);
        if (!options) {
            // Default-on means an unreadable /options cannot hide a disarm, so
            // the only disarm left this boot is the boot-arg. Said out loud
            // because it is the opposite of what the one-shot form did.
            out(1);
            return true;
        }
        g_rbgpuOptions = options;
    }
    bool zero = false;
    if (OSObject *val = options->getProperty(var)) {
        if (auto data = OSDynamicCast(OSData, val)) {
            zero = data->getLength() >= 1 &&
                   *static_cast<const char *>(data->getBytesNoCopy()) == '0';
        } else if (auto str = OSDynamicCast(OSString, val)) {
            zero = str->isEqualTo("0");
        }
    }
    if (zero) {
        // 2xx: our own pre-2xw builds wrote this exact value when they spent
        // a one-shot by overwrite, so it only counts as a disarm once the
        // generation key exists. See the note in rb_defaults.h.
        if (rb_key_disarms(var, 1, rbgpu_gen_present() ? 1 : 0)) {
            out(3);
            return false;
        }
        rbgpu_log("2xx: `%s` reads \"0\" but " RB_GEN_KEY " is absent, so that "
                  "is a spent pre-2xw one-shot and not a disarm; arming. "
                  "`sudo nvram %s=0` after this boot disarms it for real.",
                  var, var);
        out(8);
        return true;
    }
    out(0);
    return true;
}

// THE BOOT SENTINEL - what the one-shots were actually for.
//
// A one-shot made a bad boot un-repeatable: the key was consumed before the
// dangerous work, so a hang or a panic left it cleared. Arming by default
// gives that up, and 2wj is the record of what it costs when there is no way
// back - boot 196 had to be disarmed from Windows.
//
// So the key is written the other way round. `rbgpu-safe` is set to "1" here,
// before anything is armed, and overwritten with "0" from a thread_call
// kRbSentinelMs later. A boot that reaches that call was alive long enough to
// be driven; a boot that panics, hangs or is power-cycled before it leaves the
// latch set, and the NEXT boot sees it, comes up stock, and clears it - so one
// bad boot costs exactly one stock boot and never a trip to another OS.
//
// Both writes go through setProperty, the only path measured to sync (2sr),
// and the stock boot clears the latch BEFORE arming anything, in the same
// order 2dg established for the arming one-shot.
//
// The window is a trade and it is stated rather than tuned: a deliberate
// restart inside kRbSentinelMs looks exactly like a boot that did not survive
// (rule 12 in its original form - a user rebooting a black screen and a user
// rebooting a working machine are indistinguishable from NVRAM). It costs one
// stock boot. `-rbgpunosentinel` turns the whole mechanism off for a session
// of rapid reboots, and then a bad boot is the user's problem again.
static const uint32_t kRbSentinelMs = 60000;

/* 3fwd: the latch is cleared on a SUCCESS SIGNAL as well as on the timer.
 *
 * The timer alone cannot tell "the user rebooted on purpose" from "the machine
 * died", so every reboot inside 60 s spent the NEXT boot stock. On
 * 2026-11-11 five boots were read and three were stock ones nobody had asked
 * for; two of those were then counted as evidence that a build was broken.
 * A tester would hit exactly that and report an unaccelerated desktop.
 *
 * So the accelerator attaching clears it immediately - about 7 s into a good
 * boot rather than 60 - and after that a deliberate reboot costs nothing. The
 * timer STAYS as the backstop, because a boot that never accelerates on
 * purpose (`rbgpu-arm=0`, `-rbgpustock`, a recovery boot) would otherwise
 * never clear the latch and would make every following boot stock: the exact
 * trap, moved rather than removed. Whichever fires first wins; the other is a
 * no-op, and `sentinel_cleared_by` says which it was.
 */
static uint32_t g_rbSentinelCleared = 0;

static void rbgpu_sentinel_clear_now(const char *why, uint32_t by);

/* 3rgr: THE ATTACH IS NOT SURVIVAL, and 2026-11-12 paid for the difference.
 *
 * 3fwd cleared the latch the instant the accelerator attached, on the argument
 * that attaching is what the boot exists to do. Two cold boots then wedged
 * HARD at +5.7 s and +5.3 s - measured from the log store, which stops dead at
 * those timestamps - and boot 556's own timeline puts the accelerator's
 * publish at +5.72 s. So the clear fired, synced, and the machine died a
 * fraction of a second later. The latch was gone, the next boot came up armed,
 * and it wedged identically: the user got two dead boots where the sentinel
 * exists to allow exactly one. `rbgpu-safe` reading 0 after two hangs is the
 * evidence, and it is also what made "were those boots even armed?" unanswerable
 * for an afternoon.
 *
 * The attach signal is kept - it is what makes a deliberate reboot cheap, and
 * five unreadable boots on 2026-11-11 are the reason it exists - but it now
 * SCHEDULES the clear instead of performing it. A boot has to still be running
 * kRbSentinelAttachGraceMs after the accelerator attached before it counts as
 * survived, which is the weakest claim that covers the measured failure and
 * still clears at ~21 s rather than 60.
 *
 * The 60 s timer stays as the backstop for a boot that never accelerates on
 * purpose. Whichever fires first wins; `sentinel_cleared_by` says which.
 */
static const uint32_t kRbSentinelAttachGraceMs = 15000;
static thread_call_t  g_rbSentinelAttachCall   = nullptr;
static uint32_t       g_rbSentinelAttachSched  = 0;

static void rbgpu_sentinel_attach_fire(thread_call_param_t, thread_call_param_t) {
    rbgpu_sentinel_clear_now("the accelerator attached and the boot was still "
                             "alive the whole grace period afterwards", 2);
}

void rbgpu_sentinel_accelerator_ok(void) {
    if (g_rbSentinelCleared || g_rbSentinelAttachSched) {
        return;   /* already cleared, or the grace period is already running */
    }
    if (!g_rbSentinelAttachCall) {
        g_rbSentinelAttachCall =
            thread_call_allocate(rbgpu_sentinel_attach_fire, nullptr);
    }
    if (!g_rbSentinelAttachCall) {
        /* No timer. Clearing now is 3fwd's behaviour and is what this change
         * exists to avoid, but the alternative is leaving the clear to the 60 s
         * backstop, which is strictly safer - so take that instead and say so.
         * Rule 14: this must not read as a normal attach clear. */
        rbgpu_diag_set_num("sentinel_attach_sched", 0);
        rbgpu_log("3rgr: thread_call_allocate FAILED for the attach grace, so "
                  "the latch is left to the %u ms backstop instead of clearing "
                  "here", kRbSentinelMs);
        return;
    }
    g_rbSentinelAttachSched = 1;
    rbgpu_diag_set_num("sentinel_attach_sched", 1);
    rbgpu_diag_set_num("sentinel_attach_grace_ms", kRbSentinelAttachGraceMs);
    uint64_t deadline = 0;
    clock_interval_to_deadline(kRbSentinelAttachGraceMs, kMillisecondScale,
                               &deadline);
    thread_call_enter_delayed(g_rbSentinelAttachCall, deadline);
    rbgpu_log("3rgr: the accelerator attached; the latch clears in %u ms if "
              "this boot is still alive then, not now",
              kRbSentinelAttachGraceMs);
}

static void rbgpu_sentinel_clear(thread_call_param_t, thread_call_param_t) {
    rbgpu_sentinel_clear_now("the 60000 ms timer expired", 1);
}

static void rbgpu_sentinel_clear_now(const char *why, uint32_t by) {
    if (g_rbSentinelCleared) {
        return;   /* whichever fired first already did it */
    }
    IORegistryEntry *options = g_rbgpuOptions;
    if (!options) {
        options = IORegistryEntry::fromPath("/options", gIODTPlane);
        if (!options) {
            rbgpu_diag_set_num("sentinel_cleared", 2);
            return;
        }
        g_rbgpuOptions = options;
    }
    if (auto zero = OSString::withCString("0")) {
        options->setProperty("rbgpu-safe", zero);
        zero->release();
    }
    if (auto sync = OSString::withCString("now")) {
        options->setProperty("IONVRAM-FORCESYNCNOW-PROPERTY", sync);
        sync->release();
    }
    bool ok = false;
    if (OSObject *v = options->getProperty("rbgpu-safe")) {
        if (auto d = OSDynamicCast(OSData, v)) {
            ok = d->getLength() >= 1 &&
                 *static_cast<const char *>(d->getBytesNoCopy()) == '0';
        } else if (auto t = OSDynamicCast(OSString, v)) {
            ok = t->isEqualTo("0");
        }
    }
    rbgpu_diag_set_num("sentinel_cleared", ok ? 1 : 0);
    if (ok) {
        /* Only a VERIFIED clear is attributed. A failed read-back leaves the
         * key at 0 so the other path still tries (2eh: say which, or the two
         * mechanisms are indistinguishable in the record). */
        g_rbSentinelCleared = 1;
        rbgpu_diag_set_num("sentinel_cleared_by", by);
    }
    rbgpu_log("2xw/3fwd: sentinel cleared because %s: %s", why,
              ok ? "this boot counts as survived, and rebooting now costs "
                   "nothing" :
                   "the read-back FAILED, so the next boot may come up stock");
}

// Returns true if this boot is a recovery boot (the latch was set).
bool rbgpu_sentinel_begin(void) {
    if (checkKernelArgument("-rbgpunosentinel")) {
        rbgpu_diag_set_num("sentinel", 2);
        g_rbSafeLatch = 0;
        return false;
    }
    IORegistryEntry *options = g_rbgpuOptions;
    if (!options) {
        options = IORegistryEntry::fromPath("/options", gIODTPlane);
        if (!options) {
            // No latch readable and none writable: this boot is armed and
            // unprotected. Publish it - an absent key and an unreachable
            // /options must not read the same (rule 14).
            rbgpu_diag_set_num("sentinel", 3);
            g_rbSafeLatch = 0;
            return false;
        }
        g_rbgpuOptions = options;
    }

    bool latched = false;
    if (OSObject *val = options->getProperty("rbgpu-safe")) {
        latched = true;
        if (auto data = OSDynamicCast(OSData, val)) {
            if (data->getLength() >= 1 &&
                *static_cast<const char *>(data->getBytesNoCopy()) == '0') {
                latched = false;
            }
        } else if (auto str = OSDynamicCast(OSString, val)) {
            if (str->isEqualTo("0")) {
                latched = false;
            }
        }
    }
    g_rbSafeLatch = latched ? 1 : 0;

    if (latched) {
        // Clear first, then run stock: the order 2dg paid for.
        if (auto zero = OSString::withCString("0")) {
            options->setProperty("rbgpu-safe", zero);
            zero->release();
        }
        if (auto sync = OSString::withCString("now")) {
            options->setProperty("IONVRAM-FORCESYNCNOW-PROPERTY", sync);
            sync->release();
        }
        g_rbStock = 1;
        rbgpu_diag_set_num("sentinel", 4);
        rbgpu_diag_set_num("safe_latch", 1);
        rbgpu_log("2xw: rbgpu-safe was still set, so the previous boot did not "
                  "survive its first %u ms. Coming up STOCK this boot and the "
                  "latch is cleared, so the next boot is armed again.",
                  kRbSentinelMs);
        return true;
    }

    if (auto one = OSString::withCString("1")) {
        options->setProperty("rbgpu-safe", one);
        one->release();
    }
    if (auto sync = OSString::withCString("now")) {
        options->setProperty("IONVRAM-FORCESYNCNOW-PROPERTY", sync);
        sync->release();
    }
    if (!g_rbSentinelCall) {
        g_rbSentinelCall = thread_call_allocate(rbgpu_sentinel_clear, nullptr);
    }
    if (g_rbSentinelCall) {
        uint64_t deadline = 0;
        clock_interval_to_deadline(kRbSentinelMs, kMillisecondScale, &deadline);
        thread_call_enter_delayed(g_rbSentinelCall, deadline);
        rbgpu_diag_set_num("sentinel", 1);
    } else {
        // The latch is set and nothing will clear it: that would make EVERY
        // following boot a stock boot. Clear it again rather than leave the
        // machine in that state, and say so.
        if (auto zero = OSString::withCString("0")) {
            options->setProperty("rbgpu-safe", zero);
            zero->release();
        }
        rbgpu_diag_set_num("sentinel", 0);
        rbgpu_log("2xw: thread_call_allocate FAILED, so nothing would clear the "
                  "sentinel; latch removed again and this boot runs unprotected");
    }
    return false;
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

// 2xg: THE SAME GATE, CONSUMED BY OVERWRITE INSTEAD OF BY REMOVAL.
//
// `rbgpu_nvram_consume_oneshot` above removes the variable and then reads it
// back as absent - and 2sq measured that exact sequence SURVIVING A REBOOT for
// `rbgpu-fb`, because `IODTNVRAMVariables::removeProperty` tail-jumps to
// `removePropertyWithGUIDAndName`, which calls no sync at all. Its read-back
// therefore proves the in-memory registry and nothing about flash, and it
// reports `why = 0` ("consumed; clear OK") on a key that is still armed. Six
// boots re-armed the framebuffer takeover that way and boot 196 had to be
// disarmed from Windows (rule 37).
//
// 2wj fixed it in `src/fbkext/RBAMDFramebuffer.cpp` by writing a literal "0"
// instead of deleting: `setProperty` reaches `setPropertyWithGUIDAndName`,
// which DOES call `IODTNVRAM::syncInternal`, and that is the single referrer
// of `IONVRAM-FORCESYNCNOW-PROPERTY` in the whole kernel. This is the same
// fix for the plugin side, and it exists as a separate function rather than a
// change to the one above because that one is load-bearing for four shipped
// one-shots and this is the first caller whose failure mode is a PANIC.
//
// 2xx: `rbgpu_nvram_consume_oneshot_ow` lived here. It spent a one-shot by
// OVERWRITING the key with a literal "0", which is precisely the value
// 2xw's reader treats as the user's durable disarm - so every machine that
// ran an armed pre-2xw boot came up with the scheduler tick off and the
// panel declined. It has no callers left and is deleted rather than left
// unused, so nothing in this plugin can write that value again (rule 6:
// "redundant" and "inert" differ). Its behaviour is in git history at
// 288a87b. `rbgpu_nvram_consume_oneshot` stays - it consumes by REMOVAL,
// so it cannot leave a value behind, and `rbgpu-mcreg` still uses it.

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

    /* 2zd: BEFORE the lock, because rbgpu_diag_set_num() takes it and
     * IOLock is not recursive - publishing from inside here would deadlock
     * the refresh thread_call on its own lock. */
    rbgpu_retire_publish_2zd();
    /* 2CB: and retry the Metal name here, for the same reason and with the
     * same lock discipline. `handlePostRegister` can run before the register
     * aperture is up, and the derivation deliberately does not latch that
     * case - so this is what eventually publishes it. One successful publish
     * and `atyNameDone` stops the retries; it is not a periodic write. */
    publishAtyName2CB();

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

/* ===================================================================== 2CB
 * PUBLISH THE METAL NAME WHERE THE METAL DRIVER ACTUALLY LOOKS.
 *
 * AMDRadeonX6000MTLDriver builds `[MTLDevice name]` as
 * `"AMD " + ATY,FamilyName + " " + ATY,DeviceName`, with the two properties
 * read by `IORegistryEntryGetParentEntry(accel, "IOService", &parent)` then
 * `IORegistryEntryCreateCFProperty(parent, key, ...)` - decoded byte-exactly
 * from the two stubs and their GOT slots against IOKit's export table. With
 * neither property present it falls back to "Radeon HD" and "GFX10 Family
 * Unknown Prototype", which is precisely the name this machine reports.
 *
 * `GetParentEntry`, not `SearchCFProperty`: ONE level, not recursive. The
 * accelerator's single IOService parent is our `pciDevice` (IGPU@0) - the
 * MTLDevice's registryID is the accelerator node and its parent is that
 * IOPCIDevice - so this is the only node where the properties are visible.
 * Our framebuffer and this peer are SIBLINGS of the accelerator and would be
 * invisible.
 *
 * NOT A NEW WRITE SURFACE: start() already publishes `@0,name`,
 * `ATY,bin_image` and `RBGPULoad` on this same node by this same call, so
 * the mechanism is the established one and only the two keys are new.
 *
 * OSString DELIBERATELY: the driver hands the property straight back where a
 * CFString is expected (its fallbacks are CFString literals), so an OSData
 * would arrive as a CFData and be formatted as something else entirely.
 * `setProperty(const char *, const char *)` makes an OSString.
 *
 * AND IT IS WHAT MAKES SYSTEM INFORMATION SAY "Metal: 3": SPDisplaysReporter
 * pairs an MTLDevice to a GPU dictionary by name, and its first test is
 * `[sppci_model caseInsensitiveCompare:[dev name]] == NSOrderedSame`. The
 * split reconstructs our own `model` through Apple's format, so the match is
 * exact; on failure the reporter writes none of its six per-device keys,
 * which is why the Metal family line is missing today.
 * ===================================================================== */
void RembrandtGPUPeer::publishAtyName2CB() {
    char     name[RB_GPUNAME_MAX];
    char     family[RB_GPUNAME_MAX], device[RB_GPUNAME_MAX];
    uint32_t mb = 0, cu = 0, why = 0;

    if (atyNameDone) {
        return;
    }
    /* DEFAULT-ON since 2CE, disarmed by `-rbgpunomtlname` - the negative is
     * derived by `rb_off_token()` and honoured by `rbgpu_arg()` because the
     * token is in `rb_default_on_tokens`, so this call site did not change
     * when the default did.
     *
     * It was opt-in for 2CB-2CD because of a consumer we have NOT read: the
     * strings `ATY,FamilyName` and `ATY,DeviceName` also appear in
     * `AMDRadeonX6000Framebuffer`, so Apple's own kexts read them too (2q9/
     * 2qa: find the consumer before writing to a field - here there are at
     * least two and only one is read). That is still true; what changed is
     * that boots 417 and 418 both ran an accelerated desktop with the
     * properties published and nothing anywhere behaved differently, which is
     * the two measured boots rule 44 asks for. The unread consumer is a
     * reason to keep watching, not a reason to keep the chip nameless. */
    if (!rbgpu_arg("-rbgpumtlname")) {
        atyNameDone = 1;
        return;
    }
    atyNameTries++;
    rbgpu_diag_set_num("aty_tries", atyNameTries);

    /* Establish the node is what the conclusion assumes before writing to
     * it (rule 47 asked of a write): the lookup is one level up from the
     * accelerator, and if our provider is not that IOPCIDevice then these
     * properties land somewhere the driver will never read and the boot
     * would report a success that changed nothing. */
    if (!pciDevice) {
        rbgpu_diag_set_num("aty_why", 1);
        return;
    }

    name[0] = '\0';
    if (!rbgpu_gpuname_get_2CA(name, RB_GPUNAME_MAX, &mb, &cu, &why) ||
        !name[0] || !cu) {
        /* 2CA refused - most likely the register aperture is not up yet, in
         * which case it does NOT latch and the next call retries. */
        rbgpu_diag_set_num("aty_why", 2);
        rbgpu_diag_set_num("aty_gpuname_why", why);
        return;
    }

    if (!rb_gpu_aty_split(name, family, sizeof(family),
                          device, sizeof(device))) {
        /* A model Apple's format cannot reassemble byte-identically. Publish
         * NOTHING: a Metal name that disagrees with `model` is the defect
         * this is fixing, not a partial improvement. */
        rbgpu_diag_set_num("aty_why", 3);
        atyNameDone = 1;
        rbgpu_log("2CB: '%s' cannot round-trip through Apple's "
                  "\"AMD \" + family + \" \" + device - publishing nothing",
                  name);
        return;
    }

    pciDevice->setProperty("ATY,FamilyName", family);
    pciDevice->setProperty("ATY,DeviceName", device);

    /* The readback decides, not the store (rules 13/20). An OSString that is
     * not there, or not a string, is a failure however the setter returned -
     * and this is the node another kext owns, so a refusal is possible. */
    {
        OSString *f = OSDynamicCast(OSString, pciDevice->getProperty("ATY,FamilyName"));
        OSString *d = OSDynamicCast(OSString, pciDevice->getProperty("ATY,DeviceName"));
        const bool ok = f && d && f->isEqualTo(family) && d->isEqualTo(device);
        rbgpu_diag_set_num("aty_ok", ok ? 1 : 0);
        rbgpu_diag_set_num("aty_why", ok ? 0 : 4);
        if (!ok) {
            rbgpu_log("2CB: the ATY name properties did NOT read back - "
                      "family %s, device %s",
                      f ? "present" : "absent", d ? "present" : "absent");
            return;
        }
    }
    atyNameDone = 1;
    rbgpu_log("2CB: published ATY,FamilyName='%s' ATY,DeviceName='%s' on %s "
              "-> Metal should name it 'AMD %s %s'",
              family, device, pciDevice->getName(), family, device);
}

void RembrandtGPUPeer::handlePostRegister() {
    g_rb_peer_stage |= 2;
    /* 2CB: the accelerator exists by now, so its parent chain is final and
     * the register aperture is usually up. Retried from the periodic publish
     * if it is not (the derivation does not latch that case). */
    publishAtyName2CB();
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
    if (force1b < 0) { force1b = rbgpu_arg("-rbgpuforce1b") ? 1 : 0; }
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
                  rbgpu_arg("-rbgpusdmaone")) ? 1 : 0;
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
    if (rbgpu_arg("-rbgpuforceenable")) {
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

/* 2C2: the one runtime dial this peer takes. Boot 409 measured each Metal
 * command queue's first buffer starting exactly one vblank after the
 * previous one, because `checkTimestamps()` is polled once per vblank and
 * retires about one stamp per call - so N queues serialise over N vblanks.
 * This sets how many times one offer polls.
 *
 * Returns kIOReturnBadArgument on a value outside 1..RB_RETIRE_DRAIN_MAX
 * rather than clamping silently: a dial that quietly substitutes a value is
 * rule 17, and an A/B whose B side was clamped to A is worse than one that
 * refused. The caller verifies from `vblret_drain` in the diag table, never
 * from this return code (rule 13).
 */
IOReturn RembrandtGPUPeer::setProperties(OSObject *properties) {
    OSDictionary *dict = OSDynamicCast(OSDictionary, properties);
    if (!dict) { return kIOReturnBadArgument; }

    /* 2C4: more than one dial now, so an unrecognised dictionary must still
     * be distinguishable from a recognised one that refused. Each key is
     * handled independently and `handled` is what separates them; returning
     * on the first missing key (which is what this did while there was only
     * one) would make a second dial silently unreachable. */
    unsigned handled = 0;
    IOReturn rc = kIOReturnSuccess;

    OSNumber *t = OSDynamicCast(OSNumber, dict->getObject("RBGPUTickUs"));
    if (t) {
        handled++;
        const uint32_t twant = t->unsigned32BitValue();
        rbgpu_diag_set_num("tick_recv", t->unsigned64BitValue());
        rbgpu_diag_set_num("tick_want", twant);
        const uint32_t v = rb_tick_us_verdict(twant);
        if (v != RB_TICK_OK) {
            /* Rule 14 as a code: 1 is too fast, 2 is too slow. A single
             * "refused" would merge a typo'd 50 with a typo'd 50000000. */
            rbgpu_diag_set_num("tick_refused", twant);
            rbgpu_diag_set_num("tick_refuse_why", v);
            rc = kIOReturnBadArgument;
        } else {
            rbgpu_tick_us_set_2C4(twant);
            rbgpu_diag_set_num("tick_us",   rbgpu_tick_us_get_2C4());
            rbgpu_diag_set_num("tick_sets", rbgpu_tick_sets_get_2C4());
        }
    }

    /* 2CF: occasions per vblank period, 1..8. Refused rather than clamped,
     * for the reason 2C4 states: a clamp lets an A/B silently run the A side
     * twice while both calls report success. */
    OSNumber *sn = OSDynamicCast(OSNumber, dict->getObject("RBGPURetireSub"));
    if (sn) {
        const uint32_t swant = sn->unsigned32BitValue();
        handled++;
        rbgpu_diag_set_num("vblret_subrecv", swant);
        if (swant < RB_RETIRE_SUB_MIN || swant > RB_RETIRE_SUB_MAX) {
            rbgpu_diag_set_num("vblret_subrefuse", swant);
            rc = kIOReturnBadArgument;
        } else {
            rbgpu_retire_sub_set_2CF(swant);
            rbgpu_diag_set_num("vblret_sub",     rbgpu_retire_sub_get_2CF());
            rbgpu_diag_set_num("vblret_subsets", rbgpu_retire_sub_sets_2CF());
        }
    }

    OSNumber *n = OSDynamicCast(OSNumber, dict->getObject("RBGPURetireDrain"));
    if (!n) {
        /* 2D1: a dial that landed must PUBLISH, or its verification reads a
         * snapshot that nothing will refresh (boot 426). Only when something
         * was actually handled, so a stray setProperties cannot drive the
         * refresh. */
        if (handled) { refreshDiagSoon(); }
        return handled ? rc : kIOReturnUnsupported;
    }
    handled++;

    /* 2C3: SAY WHAT ARRIVED, NOT JUST WHAT WAS DECIDED.
     *
     * On boot 410 this setter took `8` and then refused to go back to `1`:
     * `drainset 1` returned KERN_SUCCESS three times and `vblret_drain` kept
     * reading 8, with `drain_refused` absent so the bounds check was not it.
     * Three writers of the global exist and none writes 8, the code is
     * symmetric in 1 and 8, and reading it harder could not distinguish
     * "setProperties never ran" from "it ran and the write was lost" from
     * "the registry snapshot is stale" - which is rule 14 exactly, and the
     * reason the A/B degenerated to one sample a side.
     *
     * So publish the raw value received and a monotonic count of accepted
     * sets. `drain_recv` differing from `drain_want` names a decode problem;
     * `drain_sets` not advancing names a call that never arrived; both moving
     * with `vblret_drain` stale names the publish path. One of the three, and
     * no inference needed. */
    const uint32_t want = n->unsigned32BitValue();
    rbgpu_diag_set_num("drain_recv", n->unsigned64BitValue());
    rbgpu_diag_set_num("drain_want", want);
    if (want < 1 || want > RB_RETIRE_DRAIN_MAX) {
        rbgpu_diag_set_num("drain_refused", want);
        refreshDiagSoon();   /* 2D1: a refusal must be readable too (rule 19) */
        return kIOReturnBadArgument;
    }
    rbgpu_retire_drain_set_2C2(want);
    g_rbgpu_drain_sets++;
    rbgpu_diag_set_num("drain_sets", g_rbgpu_drain_sets);
    /* 2D1: THIS COMMENT USED TO SAY "publish immediately as well as
     * periodically", AND NEITHER HALF WAS TRUE. `rbgpu_diag_set_num()` writes
     * the in-kernel table and schedules a refresh that the 250 ms throttle
     * then DROPS, and the refresh is the only caller of
     * `rbgpu_retire_publish_2zd()`, so there is no "periodically" either -
     * on a quiet desktop nothing refreshes `RBGPUDiag` at all. 2C5 corrected
     * this claim in `drainset`'s comment and left it standing here, in the
     * code it was a claim about.
     *
     * `refreshDiagSoon()` is what makes it true: it defers to the end of the
     * throttle window instead of dropping. 2C2's drain bug had exactly this
     * symptom and was excluded on the strength of the sentence above. */
    rbgpu_diag_set_num("vblret_drain", rbgpu_retire_drain_get_2C2());
    refreshDiagSoon();
    return rc;
}

IOReturn RembrandtGPUPeer::callPlatformFunction(const OSSymbol *functionName,
                                                bool waitForFunction,
                                                void *p1, void *p2,
                                                void *p3, void *p4) {
    /* 2zd: THE CROSS-COLLECTION HANDOVER. RBAMDFramebuffer lives in the
     * auxiliary kernel collection and this plugin in BootKernelExtensions.kc,
     * so neither can link against the other; a virtual call by name is the
     * only way across. Checked FIRST and answered with nothing allocated,
     * nothing logged and nothing locked, because this runs on the
     * framebuffer's vblank thread_call at up to 144 Hz (rule 31).
     *
     * p1, when non-NULL, receives 1 if this offer actually drove a retire, so
     * the framebuffer can count refusals on its own side rather than assuming
     * the call did something (rule 13 - a return code is not a result). */
    if (functionName && functionName->isEqualTo("RBGPURetireNow")) {
        /* 2CF: p2, when non-NULL, carries the framebuffer's LIVE vblank
         * period in nanoseconds - `RBFBVBLPeriodNs`, re-derived per mode from
         * the panel's own clock (2y6). Passed on every offer rather than
         * cached at setup, because the mode changes at runtime without a
         * reboot (2z1) and because one rate per mode is the invariant 2y6
         * bought; a second copy here is a second thing to drift.
         *
         * A NULL or a zero means "the framebuffer did not say", and then
         * `rb_retire_sub_active()` refuses to place interior occasions at
         * all - which is pre-2CF behaviour, so an older framebuffer paired
         * with this plugin is simply the A side (rule 38: the two halves are
         * separately installed and must each be safe alone). */
        const uint64_t period_ns =
            p2 ? *reinterpret_cast<const uint64_t *>(p2) : 0ull;
        const bool did = rbgpu_retire_from_vbl_2zd(period_ns);
        if (p1) { *reinterpret_cast<uint32_t *>(p1) = did ? 1u : 0u; }
        return did ? kIOReturnSuccess : kIOReturnNotReady;
    }

    /* 2CA: THE NAME AND MEMORY SIZE HANDOVER. Called ONCE from the
     * framebuffer's setup path, never from the frame path - the reads are of
     * strap and efuse registers that do not change while the machine runs,
     * and the plugin caches them, so this is cheap even though it is not on
     * a hot path.
     *
     * p1 is a char buffer of at least RB_GPUNAME_MAX, p2 a uint32_t for the
     * carve-out in MB, p3 a uint32_t for the CU count and p4 a uint32_t for
     * the refusal code. Every out-parameter is optional and each is answered
     * independently, because the name and the size refuse for different
     * reasons: an unreadable harvest mask must not cost a correct 4096 MB. */
    if (functionName && functionName->isEqualTo("RBGPUGpuName")) {
        const int any = rbgpu_gpuname_get_2CA(reinterpret_cast<char *>(p1),
                                              RB_GPUNAME_MAX,
                                              reinterpret_cast<uint32_t *>(p2),
                                              reinterpret_cast<uint32_t *>(p3),
                                              reinterpret_cast<uint32_t *>(p4));
        return any ? kIOReturnSuccess : kIOReturnNotFound;
    }

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


// ---------------------------------------------------------------------------
// 2w3: THE THREE BITS THAT DECIDE `transactionEnd`.
//
// Boot 185 named the failure at last: `IOPRESENTMENT_METHOD_FAILURE:
// IOReturn: 0x2d8` = kIOReturnNotReady, 118 times, from
// `IOAccelDisplayPipeUserClient2::transactionEnd` (IOAcceleratorFamily2
// 0x14e64620). Exactly three conditions produce that code there:
//
//     (accel->0xc78 & 0x2) == 0   the accelerator-enabled bit
//     pipe->0x282 != 0            the WSAA defer flag
//     pipe->0x298 == 0            IOAccelDisplayPipe::isActive() false
//
// All three live in Apple's own kernel objects at offsets taken from the
// accessors rather than guessed: `getFramebufferCount` reads machine+0x108,
// `getDisplayPipe` reads machine+0x88+8*i, `isActive` reads pipe+0x298, and
// `find_vram_descriptor` reaches the machine through accel+0x378.
//
// This is a READ. No hooks, no writes, no vtable work: find the accelerator
// by class, check its OSMetaClass size is large enough to contain the
// offsets we are about to touch (rule 19 - and refuse, loudly, when it is
// not), then publish the bytes. One armed boot says which gate is shut.
//
// Rule 14: one key per sample, never a last-wins key, because the whole
// question is whether the bit is clear the WHOLE time or only early.
// ---------------------------------------------------------------------------

#define RB_AX_FLAGS_OFF     0xc78u   // IOGraphicsAccelerator2 flags; bit 1 = enabled
#define RB_AX_VRAMDESC_OFF  0x180u   // getVRAMRange() result, or 0 if its map failed
#define RB_AX_VRAMMAP_OFF   0x188u
#define RB_AX_MACHINE_OFF   0x378u   // IOAccelDisplayMachine *
#define RB_AX_MIN_SIZE      0xca0u
#define RB_DM_COUNT_OFF     0x108u   // getFramebufferCount()
#define RB_DM_PIPES_OFF     0x088u   // getDisplayPipe(i)
#define RB_DM_ACCEL_OFF     0x128u
#define RB_DM_MIN_SIZE      0x140u
#define RB_DP_ACTIVE_OFF    0x298u   // isActive()
#define RB_DP_DEFER_OFF     0x282u   // wsaa defer
#define RB_DP_280_OFF       0x280u
#define RB_DP_299_OFF       0x299u
#define RB_DP_MIN_SIZE      0x2a0u

static thread_call_t g_rbgpuAxCalls[3];
static const uint32_t kRbgpuAxDelaysMs[3] = { 25000, 45000, 75000 };
static volatile uint32_t g_rbgpuAxSeq;

// A kext symbol floor is the WRONG yardstick for a kalloc'd object (rule 19);
// these are OSObjects, so the kernel's own base is the right one.
static bool rbgpu_ax_plausible(const void *p) {
    return reinterpret_cast<uintptr_t>(p) >= 0xffffff8000000000ULL;
}

static void rbgpu_ax_action(thread_call_param_t p0, thread_call_param_t) {
    const unsigned idx = static_cast<unsigned>(reinterpret_cast<uintptr_t>(p0));
    char k[48];
    #define RB_AXKEY(suffix) (snprintf(k, sizeof(k), "ax%u_" suffix, idx), k)

    // The base class first, because it is what the offsets were read from;
    // the concrete name as a fallback, in case the base is not matchable by
    // name on this OS. A match that only ever worked one way would make a
    // zero read exactly like "the service is absent" (rule 26).
    static const char *const kClasses[2] = {
        "IOGraphicsAccelerator2",
        "AMDRadeonX6000_AMDNavi21GraphicsAccelerator",
    };
    IOService *accel = nullptr;
    uint32_t n = 0, via = 0;
    for (unsigned c = 0; c < 2 && !accel; c++) {
        OSDictionary *m = IOService::serviceMatching(kClasses[c]);
        if (!m) {
            continue;
        }
        OSIterator *it = IOService::getMatchingServices(m);
        m->release();
        if (!it) {
            continue;
        }
        accel = OSDynamicCast(IOService, it->getNextObject());
        if (accel) {
            n = 1;
            while (it->getNextObject()) n++;
            via = c + 1;
        }
        it->release();
    }
    rbgpu_diag_set_num(RB_AXKEY("n"), n);
    rbgpu_diag_set_num(RB_AXKEY("via"), via);
    if (!accel) {
        // Rule 14: a silent return and an absent key look identical.
        rbgpu_diag_set_str(RB_AXKEY("say"), "no IOGraphicsAccelerator2 yet");
        return;
    }

    const OSMetaClass *mc = accel->getMetaClass();
    const unsigned sz = mc ? static_cast<unsigned>(mc->getClassSize()) : 0;
    rbgpu_diag_set_num(RB_AXKEY("size"), sz);
    if (sz < RB_AX_MIN_SIZE) {
        // The offsets came from a disassembly of THIS machine's kext; a class
        // too small to hold them means the read would be off the end, so
        // refuse and say what was refused rather than publishing garbage.
        rbgpu_diag_set_str(RB_AXKEY("say"), "accel too small - REFUSED");
        return;
    }
    const uint8_t *a = reinterpret_cast<const uint8_t *>(accel);
    const uint32_t flags = *reinterpret_cast<const uint32_t *>(a + RB_AX_FLAGS_OFF);
    const void *vram = *reinterpret_cast<void *const *>(a + RB_AX_VRAMDESC_OFF);
    const void *vmap = *reinterpret_cast<void *const *>(a + RB_AX_VRAMMAP_OFF);
    IOService *dm = *reinterpret_cast<IOService *const *>(a + RB_AX_MACHINE_OFF);

    rbgpu_diag_set_num(RB_AXKEY("flags"), flags);
    // The one bit the whole chain turns on, published on its own so it cannot
    // be lost in a word nobody decodes.
    rbgpu_diag_set_num(RB_AXKEY("enabled"), (flags & 0x2u) ? 1u : 0u);
    rbgpu_diag_set_num(RB_AXKEY("vram"), vram ? 1u : 0u);
    rbgpu_diag_set_num(RB_AXKEY("vmap"), vmap ? 1u : 0u);

    if (!rbgpu_ax_plausible(dm)) {
        rbgpu_diag_set_str(RB_AXKEY("say"), "no display machine");
        return;
    }
    const OSMetaClass *dmc = dm->getMetaClass();
    const unsigned dsz = dmc ? static_cast<unsigned>(dmc->getClassSize()) : 0;
    rbgpu_diag_set_num(RB_AXKEY("dmsize"), dsz);
    if (dsz < RB_DM_MIN_SIZE) {
        rbgpu_diag_set_str(RB_AXKEY("say"), "machine too small - REFUSED");
        return;
    }
    const uint8_t *d = reinterpret_cast<const uint8_t *>(dm);
    const uint32_t fbs = *reinterpret_cast<const uint32_t *>(d + RB_DM_COUNT_OFF);
    rbgpu_diag_set_num(RB_AXKEY("fbs"), fbs);
    if (fbs == 0 || fbs > 8) {
        rbgpu_diag_set_str(RB_AXKEY("say"), "framebuffer count out of range");
        return;
    }
    IOService *pipe = *reinterpret_cast<IOService *const *>(d + RB_DM_PIPES_OFF);
    if (!rbgpu_ax_plausible(pipe)) {
        rbgpu_diag_set_str(RB_AXKEY("say"), "pipe 0 absent");
        return;
    }
    const OSMetaClass *pmc = pipe->getMetaClass();
    const unsigned psz = pmc ? static_cast<unsigned>(pmc->getClassSize()) : 0;
    rbgpu_diag_set_num(RB_AXKEY("dpsize"), psz);
    if (psz < RB_DP_MIN_SIZE) {
        rbgpu_diag_set_str(RB_AXKEY("say"), "pipe too small - REFUSED");
        return;
    }
    const uint8_t *q = reinterpret_cast<const uint8_t *>(pipe);
    rbgpu_diag_set_num(RB_AXKEY("active"), q[RB_DP_ACTIVE_OFF]);
    rbgpu_diag_set_num(RB_AXKEY("defer"), q[RB_DP_DEFER_OFF]);
    rbgpu_diag_set_num(RB_AXKEY("b280"), q[RB_DP_280_OFF]);
    rbgpu_diag_set_num(RB_AXKEY("b299"), q[RB_DP_299_OFF]);

    // 2w8: boot 188 measured `active 0`, so `init_framebuffer_resource`
    // (0x14e5d7ca) returned false - and the two exits that can do that
    // without logging are `getCurrentDisplayMode` (ours returns success) and
    // `pipe->vt[0x858](0, resource)`, which on this machine is
    // `AMDRadeonX6000_AMDAccelDisplayPipe::initFramebufferResource`
    // (0xbe49356). These are the fields that decide which of ITS seven silent
    // returns fired, plus the fields the BASE fills just before the call, as
    // positive progress markers.
    //
    // The base writes, from our getPixelInformation: 0x290 activeWidth,
    // 0x294 activeHeight (16-bit), 0x296 bytes-per-pixel (bitsPerPixel >> 3),
    // 0x28c the pixel-format enum (4 for 32bpp/8-bit components), and 0x284
    // is the framebuffer index. If those read 1920 / 1080 / 4 / 4 then
    // getPixelInformation succeeded and the failure IS the vendor hook.
    //
    // AMD's hook then reads: 0x98 getFramebuffer(), 0xe0 the resource,
    // 0x328/0x330 the HW objects (0x330 = accel->0x1a38->vt[0x2f8]()),
    // 0x340 + 0x348 the _FRAMEBUFFER_INFO that
    // `AMDRadeonX6000_AMDHWDisplay::configureDisplay(fbIndex, index, info,
    // resource)` (vt[0x138], 0xbe87bd4) must fill, the 0x35e / 0x35f branch
    // selectors, and 0x380 - the value it RETURNS, so a zero there is a
    // silent failure all by itself.
    //
    // Every offset is inside the measured class size (boot 188: 15216), and
    // the RB_DP_MIN_SIZE guard above already covers them.
    rbgpu_diag_set_num(RB_AXKEY("fbidx"),
                       *reinterpret_cast<const uint32_t *>(q + 0x284));
    rbgpu_diag_set_num(RB_AXKEY("fmt"),
                       *reinterpret_cast<const uint32_t *>(q + 0x28c));
    rbgpu_diag_set_num(RB_AXKEY("w"),
                       *reinterpret_cast<const uint32_t *>(q + 0x290));
    rbgpu_diag_set_num(RB_AXKEY("h"),
                       *reinterpret_cast<const uint16_t *>(q + 0x294));
    rbgpu_diag_set_num(RB_AXKEY("bpp"),
                       *reinterpret_cast<const uint16_t *>(q + 0x296));
    rbgpu_diag_set_num(RB_AXKEY("pfb"),
                       *reinterpret_cast<void *const *>(q + 0x98) ? 1u : 0u);
    rbgpu_diag_set_num(RB_AXKEY("pres"),
                       *reinterpret_cast<void *const *>(q + 0xe0) ? 1u : 0u);
    rbgpu_diag_set_num(RB_AXKEY("hw328"),
                       *reinterpret_cast<void *const *>(q + 0x328) ? 1u : 0u);
    rbgpu_diag_set_num(RB_AXKEY("hw330"),
                       *reinterpret_cast<void *const *>(q + 0x330) ? 1u : 0u);
    // 0x340 is an address and 0x348 a size: publish them whole, because
    // "non-zero" would not tell a plausible value from a garbage one.
    rbgpu_diag_set_num(RB_AXKEY("fi340"),
                       *reinterpret_cast<const uint64_t *>(q + 0x340));
    rbgpu_diag_set_num(RB_AXKEY("fi348"),
                       *reinterpret_cast<const uint32_t *>(q + 0x348));
    rbgpu_diag_set_num(RB_AXKEY("fi34c"),
                       *reinterpret_cast<const uint32_t *>(q + 0x34c));
    /* 2w9: 0x35d is AMDHWDisplay::isDisplayEnabled's answer, which
     * set_display_mode_and_vram (0xbe48604) stores and then requires to be 1.
     * It is TRUE iff the 64-bit ATY,fb_size property is non-zero - so this one
     * byte separates "we published the property" from "we did not". */
    rbgpu_diag_set_num(RB_AXKEY("b35d"), q[0x35d]);
    rbgpu_diag_set_num(RB_AXKEY("b35e"), q[0x35e]);
    rbgpu_diag_set_num(RB_AXKEY("b35f"), q[0x35f]);
    rbgpu_diag_set_num(RB_AXKEY("r380"),
                       *reinterpret_cast<void *const *>(q + 0x380) ? 1u : 0u);

    // The verdict, in the form the next step needs: which of the three gates
    // would make transactionEnd answer kIOReturnNotReady right now.
    // A number as well as a sentence: the NVRAM note is 512 bytes and strings
    // are what pushed the 2s8 keys past the cut.  0 all three open,
    // 1 accel not enabled, 2 WSAA defer, 3 pipe not active.
    const char *verdict = "ALL THREE OPEN - NotReady is elsewhere";
    uint32_t gate = 0;
    if (!(flags & 0x2u))           { gate = 1; verdict = "accel NOT enabled (0xc78 bit1)"; }
    else if (q[RB_DP_DEFER_OFF])   { gate = 2; verdict = "WSAA defer (pipe 0x282)"; }
    else if (!q[RB_DP_ACTIVE_OFF]) { gate = 3; verdict = "pipe NOT active (0x298)"; }
    rbgpu_diag_set_num(RB_AXKEY("gate"), gate);
    rbgpu_diag_set_str(RB_AXKEY("say"), verdict);
    #undef RB_AXKEY
}

void rbgpu_accelprobe_start(void) {
    if (!rbgpu_arg("-rbgpuaccelprobe")) {
        return;
    }
    if (g_rbgpuAxSeq) {
        return;             // single entry; the arg is read from two places
    }
    g_rbgpuAxSeq = 1;
    for (unsigned i = 0; i < 3; i++) {
        g_rbgpuAxCalls[i] = thread_call_allocate(
            rbgpu_ax_action, reinterpret_cast<thread_call_param_t>(
                static_cast<uintptr_t>(i)));
        if (!g_rbgpuAxCalls[i]) {
            rbgpu_diag_set_str("ax_say", "thread_call_allocate FAILED");
            continue;
        }
        uint64_t deadline = 0;
        clock_interval_to_deadline(kRbgpuAxDelaysMs[i], kMillisecondScale,
                                   &deadline);
        thread_call_enter_delayed(g_rbgpuAxCalls[i], deadline);
    }
    rbgpu_diag_set_str("ax_say", "ARMED: 25/45/75 s reads of the three gates");
}
