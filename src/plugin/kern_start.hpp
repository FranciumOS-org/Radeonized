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

    void installKernelLogHooks(KernelPatcher &patcher);
    void processHWLibs(KernelPatcher &patcher, size_t id, mach_vm_address_t address, size_t size);
    void processX6000(KernelPatcher &patcher, size_t id, mach_vm_address_t address, size_t size);
    void processHWServices(KernelPatcher &patcher, size_t id, mach_vm_address_t address, size_t size);

    bool patchDeviceTypeTable(KernelPatcher &patcher, size_t id);
};

#endif // KERN_START_HPP

