#ifndef REMBRANDTGPU_PEER_HPP
#define REMBRANDTGPU_PEER_HPP

#include <IOKit/IOService.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <kern/thread_call.h>
#include <stdarg.h>

void rbgpu_log(const char *fmt, ...) __printflike(1, 2);
void rbgpu_log_captured_va(const char *tag, const char *fmt, va_list va, void (*orig_fn)(const char *, ...));
void rbgpu_diag_set_num(const char *key, uint64_t val);
// 2eo: synchronous NVRAM breadcrumb, written on the calling thread.
void rbgpu_gate_nvram(uint32_t stage, const char *what);
void rbgpu_nvram_quiesce(void);
// 2fq: ONE synchronous NVRAM variable, written once where it is called. Not a
// trail: the gate trail's twenty-four flash commits inside IOService::start
// were the 2fo wedge, and this is a single write that carries a whole answer.
void rbgpu_nvram_note(const char *var, const char *text);
// 2qo: a one-shot NVRAM gate for an experiment that can cost the desktop.
// True iff `var` was set and not a literal "0"; the variable is removed and
// FLUSHED TO FLASH before returning, so a boot that ends in a black screen is
// recovered by a plain power cycle - no boot-arg edit and no second OS.
// `why` is written on every path; see the definition for the codes.
// 2xw: the default-on gate, the durable-disarm key read and the boot
// sentinel. `rbgpu_arg` is what every feature check goes through now;
// `rbgpu_nvram_armed` replaces the one-shot consumers for the three arms that
// used to need one; `rbgpu_sentinel_begin` must run BEFORE anything is armed
// and returns true on a recovery boot.
bool rbgpu_arg(const char *tok);
bool rbgpu_stock(void);
/* 2xz: 1 if the measured Apple-object offsets in this tree were measured on
 * the OS this boot is running, or -rbgpuosforce says to use them anyway.
 * Folded into rbgpu_stock(), so nothing has to ask it to be safe - it is here
 * for the paths that must distinguish "the user asked for stock" from "the
 * kext refused to guess". */
bool rbgpu_os_measured(void);
bool rbgpu_nvram_armed(const char *var, uint32_t *why);

// 2xx: has the generation key been written? See `rb_defaults.h` - before it
// exists, a "0" in `rbgpu-tick` or `rbgpu-fb` is our own spent one-shot.
bool rbgpu_gen_present(void);
bool rbgpu_sentinel_begin(void);

bool rbgpu_nvram_consume_oneshot(const char *var, uint32_t *why);
// 2ey: an independent 250 ms NVRAM heartbeat (`nvram rbgpu-beat`), so the
// gate trail can be told apart from a machine that died under it.
void rbgpu_heartbeat_start(void);
// 2fw: two delayed samples of the progress stage, so one boot says whether
// the chain is stopped or crawling without the user running anything.
void rbgpu_progwatch_start(void);
// 2w3: three delayed READS of the three bits that make
// IOAccelDisplayPipeUserClient2::transactionEnd answer kIOReturnNotReady -
// accel->0xc78 bit 1, pipe->0x282 and pipe->0x298. `-rbgpuaccelprobe`.
void rbgpu_accelprobe_start(void);
/* 2wx: the per-window advance()/submit rate timeline -> rbgpu-rate. */
void rbgpu_rate_start_2wx(void);
/* 2xg: the scheduler tick - a periodic AMDSWScheduler::checkTimestamps() at
 * the panel's rate, to test whether display-transaction retirement is the gate
 * on the frame cadence. `-rbgpuschedtick` + a `rbgpu-tick` one-shot. */
void rbgpu_schedtick_start_2xg(void);
extern volatile uint32_t g_rb_prog_stage;
extern volatile uint32_t g_rb_step;   // 2fz: lock-free, log-free position marker
#define RB_STEP(n) do { g_rb_step = (n); } while (0)
void rbgpu_diag_set_str(const char *key, const char *val);
void rbgpu_diag_inc(const char *key);

// 2bq: peer progress, readable from hooks (a panic loses RBGPULog/RBGPUDiag).
// Bits: 1 Register (0x00) received, 2 post-Register thread ran,
// 4 request 0x06 returned, 8 request 0x1B returned.
// 2fr: 1 only when AMDAccelerator::powerUpHW returned nonzero.
extern volatile int g_rb_poweruphw_ok;
extern volatile uint32_t g_rb_peer_stage;
extern volatile uint32_t g_rb_peer_ret06;
extern volatile uint32_t g_rb_peer_ret1B;

// 2ow: the vblank pump. Defined in kern_start.cpp, started from
// RembrandtGPUPeer::start() - ordinary IOService matching context, where a
// workloop is routine. 2ov created it inside the routed
// getInformationForDisplayMode instead, during the framebuffer's own early
// mode enumeration, and boot 135 reported vblp_solved 1 with vblp_armed 0.
// Declared here because this header is the one both files already include.
bool rbgpu_vblp_start(OSObject *owner);

class RembrandtGPUPeer : public IOService {
    OSDeclareDefaultStructors(RembrandtGPUPeer)

private:
    IOPCIDevice     *pciDevice{nullptr};
    IOService       *accelerator{nullptr};
    const OSSymbol  *symSpecialAMDKey{nullptr};
    thread_call_t    postRegisterCall{nullptr};
    thread_call_t    refreshDiagCall{nullptr};

    static void postRegisterAction(thread_call_param_t param0, thread_call_param_t param1);
    static void refreshDiagAction(thread_call_param_t param0, thread_call_param_t param1);

    /* 2CB: the Metal name, published on `pciDevice` (IGPU@0) because that is
     * the accelerator's single IOService parent and the only node
     * AMDRadeonX6000MTLDriver's one-level lookup can see. `atyNameDone`
     * makes it one successful publish; `atyNameTries` counts the attempts,
     * so "the aperture was never up" is distinguishable from "it was never
     * called" - 2C2's drain bug spent a milestone in that gap. */
    uint32_t         atyNameDone{0};
    uint32_t         atyNameTries{0};
    void publishAtyName2CB();

    void handlePostRegister();
    void performRefreshDiag();
    // 2cu: persist a compact summary into NVRAM (`nvram rbgpu-diag`), so a
    // boot that never reaches a desktop still yields its numbers.
    void saveDiagToNvram();

public:
    bool init(OSDictionary *dictionary = nullptr) override;
    void free() override;
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;

    // 2C2: the retire-drain runtime dial. A dial and not only a boot-arg
    // because rule 44 wants the A/B INTERLEAVED inside one boot.
    IOReturn setProperties(OSObject *properties) override;

    IOReturn callPlatformFunction(const OSSymbol *functionName,
                                  bool waitForFunction,
                                  void *p1, void *p2,
                                  void *p3, void *p4) override;

    void scheduleRefreshDiag();
};

#endif // REMBRANDTGPU_PEER_HPP

