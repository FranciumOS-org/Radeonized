#ifndef REMBRANDTGPU_PEER_HPP
#define REMBRANDTGPU_PEER_HPP

#include <IOKit/IOService.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <kern/thread_call.h>

// Global logger and diagnostic functions accessible from all compilation units
void rbgpu_log(const char *fmt, ...) __printflike(1, 2);
void rbgpu_diag_set_num(const char *key, uint64_t val);
void rbgpu_diag_set_str(const char *key, const char *val);
void rbgpu_diag_inc(const char *key);

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
