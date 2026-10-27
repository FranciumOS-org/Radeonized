/* SPDX-License-Identifier: MIT */
/* RBAMDFramebuffer - an IOFramebuffer subclass for the firmware-lit panel.
 *
 * Why this exists, and why the class is named the way it is (2ql, 2sl):
 * CoreDisplay's `__GetSystemGPUVendors_block_invoke` iterates every
 * `IOFramebuffer` service, calls `IOObjectCopyClass` on it, and `CFStringFind`s
 * `Intel` / `AMD` / `NVDA` / `AppleParavirt` in the CLASS NAME. A name matching
 * none of them earns bit 0x01, which the `supported` mask can never contain -
 * so one unclassifiable framebuffer makes `UseIOPresentment()` return 0 for
 * every possible value of every preference, and `Display::GetNextSurface` takes
 * the legacy path whose surface the accelerator refuses to create (2qk). The
 * desktop then composites on the CPU (2pd). `make useiop` prints that gate
 * live: `IONDRVFramebuffer name=.Display_boot -> bit 0x01`.
 *
 * `present` is the OR over ALL framebuffers, so adding an AMD-named one beside
 * `.Display_boot` would give 0x05 and keep the gate shut. This personality
 * therefore DISPLACES `IONDRVFramebuffer`: same provider (`IOPCIDevice`), same
 * `IONameMatch` (`display`), same `IOMatchCategory` (`IOFramebuffer`), and a
 * higher `IOProbeScore` than its 20000. One driver wins per category.
 *
 * It writes NOTHING to the display engine. The firmware already ran the modeset
 * (`OTG0_CONTROL 0x00011201`, DMCUB 4.0.15.47) and the GOP framebuffer is BAR0
 * offset 0 (2pr); every number below was read off this machine's live
 * `IONDRVFramebuffer` or measured by the plugin's OTG sampler. That is also the
 * shape of the one working comparable driver (2sj): subclass IOFramebuffer,
 * take over the lit mode, only ever READ the OTG registers.
 *
 * Arming requires BOTH -rbgpuownfb and the NVRAM key rbgpu-fb. Removal of a
 * one-shot is not proven durable (2sq/2sr); removing the boot-arg from the
 * OpenCore config is the durable disarm. Never unload a live framebuffer.
 */
#ifndef RBAMDFRAMEBUFFER_HPP
#define RBAMDFRAMEBUFFER_HPP

#include <IOKit/graphics/IOFramebuffer.h>

class IOPCIDevice;

class RBAMDFramebuffer : public IOFramebuffer {
    OSDeclareDefaultStructors(RBAMDFramebuffer)

public:
    /* IOService */
    IOService *probe(IOService *provider, SInt32 *score) APPLE_KEXT_OVERRIDE;
    bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
    void stop(IOService *provider) APPLE_KEXT_OVERRIDE;

    /* IOFramebuffer: memory */
    IODeviceMemory *getApertureRange(IOPixelAperture aperture) APPLE_KEXT_OVERRIDE;
    IODeviceMemory *getVRAMRange(void) APPLE_KEXT_OVERRIDE;

    /* IOFramebuffer: modes */
    const char *getPixelFormats(void) APPLE_KEXT_OVERRIDE;
    IOItemCount getDisplayModeCount(void) APPLE_KEXT_OVERRIDE;
    IOReturn getDisplayModes(IODisplayModeID *allDisplayModes) APPLE_KEXT_OVERRIDE;
    IOReturn getInformationForDisplayMode(IODisplayModeID displayMode,
                                          IODisplayModeInformation *info) APPLE_KEXT_OVERRIDE;
    UInt64 getPixelFormatsForDisplayMode(IODisplayModeID displayMode,
                                         IOIndex depth) APPLE_KEXT_OVERRIDE;
    IOReturn getPixelInformation(IODisplayModeID displayMode, IOIndex depth,
                                 IOPixelAperture aperture,
                                 IOPixelInformation *info) APPLE_KEXT_OVERRIDE;
    IOReturn getCurrentDisplayMode(IODisplayModeID *displayMode,
                                   IOIndex *depth) APPLE_KEXT_OVERRIDE;
    IOReturn setDisplayMode(IODisplayModeID displayMode, IOIndex depth) APPLE_KEXT_OVERRIDE;
    IOReturn getStartupDisplayMode(IODisplayModeID *displayMode,
                                   IOIndex *depth) APPLE_KEXT_OVERRIDE;
    IOReturn setStartupDisplayMode(IODisplayModeID displayMode, IOIndex depth) APPLE_KEXT_OVERRIDE;

    /* IOFramebuffer: controller and connection */
    IOReturn enableController(void) APPLE_KEXT_OVERRIDE;
    bool isConsoleDevice(void) APPLE_KEXT_OVERRIDE;
    IOItemCount getConnectionCount(void) APPLE_KEXT_OVERRIDE;
    IOReturn getAttributeForConnection(IOIndex connectIndex, IOSelect attribute,
                                       uintptr_t *value) APPLE_KEXT_OVERRIDE;
    IOReturn setAttributeForConnection(IOIndex connectIndex, IOSelect attribute,
                                       uintptr_t value) APPLE_KEXT_OVERRIDE;
    IOReturn getAttribute(IOSelect attribute, uintptr_t *value) APPLE_KEXT_OVERRIDE;
    IOReturn setAttribute(IOSelect attribute, uintptr_t value) APPLE_KEXT_OVERRIDE;

    /* IOFramebuffer: the vblank, which is what drains the display pipe (2s4) */
    IOReturn registerForInterruptType(IOSelect interruptType, IOFBInterruptProc proc,
                                      OSObject *target, void *ref,
                                      void **interruptRef) APPLE_KEXT_OVERRIDE;
    IOReturn unregisterInterrupt(void *interruptRef) APPLE_KEXT_OVERRIDE;
    IOReturn setInterruptState(void *interruptRef, UInt32 state) APPLE_KEXT_OVERRIDE;

private:
    /* One registered interrupt handler per type. IOFramebuffer asks for at most
     * a handful; four is what the comparable driver carries (2sj). */
    struct Interrupt {
        IOSelect type;
        IOFBInterruptProc proc;
        OSObject *target;
        void *ref;
        bool enabled;
        bool used;
        uint64_t fires;
    };
    static const unsigned kMaxInterrupts = 4;
    static constexpr UInt32 kRefreshRate1616 = 0x008fd53f; /* measured 143.83 Hz */
    static constexpr uint64_t kVBLPeriodNs =
        1000000000ULL * 65536ULL / kRefreshRate1616;

    IOPCIDevice *pci;
    IODeviceMemory *aperture;
    Interrupt interrupts[kMaxInterrupts];
    thread_call_t vblCall;
    bool vblRunning;
    /* 2su: setInterruptState was silent, so enabled-and-never-fired,
     * disabled, and never-registered all looked identical. These make the
     * enable transition and the delivery observable (rule 14). */
    uint64_t vblFires;
    uint32_t vblEnables;
    /* 2t0: crash breadcrumbs. Rule 10 - a boot that dies leaves NVRAM only,
     * and rule 31's registry properties vanish with the machine. */
    IORegistryEntry *nvram;
    bool crumbOn;
    uint32_t crumbStage;
    char crumbBoot[12];   /* rbgpu-boot, the nonce (rule 11) */

    /* True iff this instance won the `IOFramebuffer` match category, i.e.
     * displaced IONDRVFramebuffer. Gates the console claim and the connection
     * count; see the comment on the definition. */
    bool ownsPanel(void);

    static void vblFired(thread_call_param_t self, thread_call_param_t);
    void crumb(uint32_t stage);
    void deliverVBL(void);
    void armVBL(void);
    bool anyVBLEnabled(void);
    void publishVBLState(void);
};

#endif /* RBAMDFRAMEBUFFER_HPP */
