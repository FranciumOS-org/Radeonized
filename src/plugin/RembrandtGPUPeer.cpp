
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
#define RBGPU_MAX_DIAGS 96
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

    // Consume it before doing anything that can break this boot.
    options->removeProperty("rbgpu-arm");
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

        rbgpu_log("SpecialAMDKey request 0x%02X (wait: %d)", req, waitForFunction);

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

