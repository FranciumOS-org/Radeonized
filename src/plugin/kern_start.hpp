#ifndef KERN_START_HPP
#define KERN_START_HPP

#include <Headers/kern_api.hpp>
#include <Headers/kern_patcher.hpp>

#include "RembrandtGPUPeer.hpp"

#define MODULE_SHORT "rbgpu"

class RGPU {
public:
    void init();
    void deinit();

    void processKext(KernelPatcher &patcher, size_t id, mach_vm_address_t address, size_t size);

private:
    bool noSmuHook{false};
    bool noTrap3Guard{false};
    bool noLogCapture{false};
    bool safeTtlMode{false};
    bool sync570Mode{false};
    bool spoofTtlMode{false};
    bool pspHwMode{false};

    void installKernelLogHooks(KernelPatcher &patcher);
    void processHWLibs(KernelPatcher &patcher, size_t id, mach_vm_address_t address, size_t size);
    void processX6000(KernelPatcher &patcher, size_t id, mach_vm_address_t address, size_t size);
    void processHWServices(KernelPatcher &patcher, size_t id, mach_vm_address_t address, size_t size);
    // 2ct: IOAcceleratorFamily2, for the headless-accelerator hook.
    void processAccelFamily(KernelPatcher &patcher, size_t id, mach_vm_address_t address, size_t size);
    // 2ou: IONDRVSupport, for the refresh rate the boot framebuffer reports as 0.
    void processNDRV(KernelPatcher &patcher, size_t id, mach_vm_address_t address, size_t size);
    // 2ov: IOGraphicsFamily, to solve IOFramebuffer::handleVBL for the vblank pump.
    void processIOGraphics(KernelPatcher &patcher, size_t id, mach_vm_address_t address, size_t size);

    void routeTTL(KernelPatcher &patcher, size_t id, mach_vm_address_t address, size_t size);
    void routePSPBasics(KernelPatcher &patcher, size_t id, mach_vm_address_t address, size_t size);
    void routePSPRingTmr(KernelPatcher &patcher, size_t id, mach_vm_address_t address, size_t size);
    void routeGC(KernelPatcher &patcher, size_t id, mach_vm_address_t address, size_t size);

    bool patchDeviceTypeTable(KernelPatcher &patcher, size_t id);
    bool patchDeviceCapabilityTable(KernelPatcher &patcher, size_t id);
    bool patchGcFwVersionCheck(KernelPatcher &patcher, size_t id);
};

#endif // KERN_START_HPP

