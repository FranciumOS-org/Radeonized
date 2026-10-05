#include <IOKit/IOLib.h>
#include <Headers/kern_util.hpp>

#include "RembrandtGPUPeer.hpp"
#include "vbios_data.h"

#define super IOService
#define Log(fmt, ...) IOLog("RembrandtGPUPeer: " fmt "\n", ##__VA_ARGS__)

OSDefineMetaClassAndStructors(RembrandtGPUPeer, IOService)

bool RembrandtGPUPeer::init(OSDictionary *dictionary) {
    if (!super::init(dictionary)) {
        return false;
    }

    symSpecialAMDKey = OSSymbol::withCString("SpecialAMDKey");
    if (!symSpecialAMDKey) {
        return false;
    }

    postRegisterCall = thread_call_allocate(postRegisterAction, this);
    return postRegisterCall != nullptr;
}

void RembrandtGPUPeer::free() {
    if (postRegisterCall) {
        thread_call_cancel(postRegisterCall);
        thread_call_free(postRegisterCall);
        postRegisterCall = nullptr;
    }
    OSSafeReleaseNULL(symSpecialAMDKey);
    OSSafeReleaseNULL(accelerator);
    super::free();
}

bool RembrandtGPUPeer::start(IOService *provider) {
    if (checkKernelArgument("-rbgpuoff")) {
        Log("Disabled by -rbgpuoff; aborting start");
        return false;
    }

    if (!super::start(provider)) {
        return false;
    }

    pciDevice = OSDynamicCast(IOPCIDevice, provider);
    if (!pciDevice) {
        Log("Provider is not an IOPCIDevice");
        return false;
    }

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
    pciDevice->setProperty("LoadAccelerator", kOSBooleanTrue);

    registerService();
    pciDevice->registerService();

    Log("Peer attached to 0x16811002; properties set and service registered");
    return true;
}

void RembrandtGPUPeer::stop(IOService *provider) {
    if (postRegisterCall) {
        thread_call_cancel(postRegisterCall);
    }
    if (accelerator) {
        accelerator->release();
        accelerator = nullptr;
    }
    super::stop(provider);
}

void RembrandtGPUPeer::postRegisterAction(thread_call_param_t param0, thread_call_param_t param1) {
    auto self = static_cast<RembrandtGPUPeer *>(param0);
    if (self) {
        self->handlePostRegister();
    }
}

void RembrandtGPUPeer::handlePostRegister() {
    if (!accelerator || !symSpecialAMDKey) {
        Log("handlePostRegister: missing accelerator or symbol reference");
        return;
    }

    uint32_t one = 1;

    Log("handlePostRegister: sending Request 0x06 (PowerPlay Ready)");
    uint32_t reqPowerPlayReady = 0x06;
    IOReturn ret = accelerator->callPlatformFunction(symSpecialAMDKey, true, &reqPowerPlayReady, &one, nullptr, nullptr);
    Log("handlePostRegister: Request 0x06 returned 0x%08X", ret);

    Log("handlePostRegister: sending Request 0x1B (Controller Enabled)");
    uint32_t reqControllerEnabled = 0x1B;
    ret = accelerator->callPlatformFunction(symSpecialAMDKey, true, &reqControllerEnabled, &one, nullptr, nullptr);
    Log("handlePostRegister: Request 0x1B returned 0x%08X", ret);
}

IOReturn RembrandtGPUPeer::callPlatformFunction(const OSSymbol *functionName,
                                                bool waitForFunction,
                                                void *p1, void *p2,
                                                void *p3, void *p4) {
    if (functionName && symSpecialAMDKey && functionName->isEqualTo(symSpecialAMDKey)) {
        if (!p1) {
            Log("SpecialAMDKey received NULL p1");
            return kIOReturnBadArgument;
        }

        uint32_t req = *reinterpret_cast<uint32_t *>(p1);
        Log("SpecialAMDKey request 0x%02X (wait: %d)", req, waitForFunction);

        switch (req) {
            case 0x00: // Register
                if (p2) {
                    OSSafeReleaseNULL(accelerator);
                    accelerator = OSDynamicCast(IOService, reinterpret_cast<OSObject *>(p2));
                    if (accelerator) {
                        accelerator->retain();
                        Log("Registered accelerator instance: %s", accelerator->getName());
                        if (postRegisterCall) {
                            thread_call_enter(postRegisterCall);
                        }
                    }
                }
                return kIOReturnSuccess;

            case 0x0B: // Request Drop Registration
                if (accelerator) {
                    Log("Dropping registered accelerator: %s", accelerator->getName());
                    accelerator->release();
                    accelerator = nullptr;
                }
                return kIOReturnSuccess;

            case 0x14: // Gpu Status
                if (p2) {
                    *reinterpret_cast<uint32_t *>(p2) = 1;
                    return kIOReturnSuccess;
                }
                return kIOReturnBadArgument;

            case 0x17: // ASIC Reset State
                Log("ASIC Reset State requested (unsupported)");
                return kIOReturnUnsupported;

            case 0x1A: // Get DCC Capabilities
                Log("Get DCC Capabilities requested (unsupported)");
                return kIOReturnUnsupported;

            default:
                Log("Unhandled SpecialAMDKey request 0x%02X", req);
                return kIOReturnUnsupported;
        }
    }

    return super::callPlatformFunction(functionName, waitForFunction, p1, p2, p3, p4);
}
