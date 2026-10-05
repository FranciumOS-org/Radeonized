#ifndef REMBRANDTGPU_PEER_HPP
#define REMBRANDTGPU_PEER_HPP

#include <IOKit/IOService.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <kern/thread_call.h>

class RembrandtGPUPeer : public IOService {
    OSDeclareDefaultStructors(RembrandtGPUPeer)

private:
    IOPCIDevice     *pciDevice{nullptr};
    IOService       *accelerator{nullptr};
    const OSSymbol  *symSpecialAMDKey{nullptr};
    thread_call_t    postRegisterCall{nullptr};

    static void postRegisterAction(thread_call_param_t param0, thread_call_param_t param1);
    void handlePostRegister();

public:
    bool init(OSDictionary *dictionary = nullptr) override;
    void free() override;
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;

    IOReturn callPlatformFunction(const OSSymbol *functionName,
                                  bool waitForFunction,
                                  void *p1, void *p2,
                                  void *p3, void *p4) override;
};

#endif // REMBRANDTGPU_PEER_HPP
