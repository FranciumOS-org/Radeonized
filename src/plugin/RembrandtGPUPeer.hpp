#ifndef REMBRANDTGPU_PEER_HPP
#define REMBRANDTGPU_PEER_HPP

#include <IOKit/IOService.h>
#include <IOKit/pci/IOPCIDevice.h>

class RembrandtGPUPeer : public IOService {
    OSDeclareDefaultStructors(RembrandtGPUPeer)

private:
    IOPCIDevice     *pciDevice{nullptr};
    IOService       *accelerator{nullptr};
    const OSSymbol  *symSpecialAMDKey{nullptr};

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
