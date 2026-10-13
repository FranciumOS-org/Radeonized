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
// 2ey: an independent 250 ms NVRAM heartbeat (`nvram rbgpu-beat`), so the
// gate trail can be told apart from a machine that died under it.
void rbgpu_heartbeat_start(void);
void rbgpu_diag_set_str(const char *key, const char *val);
void rbgpu_diag_inc(const char *key);

// 2bq: peer progress, readable from hooks (a panic loses RBGPULog/RBGPUDiag).
// Bits: 1 Register (0x00) received, 2 post-Register thread ran,
// 4 request 0x06 returned, 8 request 0x1B returned.
extern volatile uint32_t g_rb_peer_stage;
extern volatile uint32_t g_rb_peer_ret06;
extern volatile uint32_t g_rb_peer_ret1B;

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

    IOReturn callPlatformFunction(const OSSymbol *functionName,
                                  bool waitForFunction,
                                  void *p1, void *p2,
                                  void *p3, void *p4) override;

    void scheduleRefreshDiag();
};

#endif // REMBRANDTGPU_PEER_HPP

