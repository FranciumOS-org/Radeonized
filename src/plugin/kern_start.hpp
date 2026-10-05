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

    void processKext(KernelPatcher &patcher, size_t id, mach_vm_address_t slide, size_t size);

private:
    bool noSmuHook{false};
    bool noTrap3Guard{false};

    void processHWLibs(KernelPatcher &patcher, size_t id, mach_vm_address_t slide, size_t size);
    void processX6000(KernelPatcher &patcher, size_t id, mach_vm_address_t slide, size_t size);
};

#endif // KERN_START_HPP
