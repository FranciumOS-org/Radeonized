#ifndef RBGPU_AGDC_HPP
#define RBGPU_AGDC_HPP
#include <Headers/kern_patcher.hpp>
#include <IOKit/IOService.h>
void rbgpu_agdc_load(KernelPatcher &, size_t, mach_vm_address_t, size_t);
void rbgpu_agdc_framebuffer(IOService *);
// 2ro: the publish trigger. Until now the bridge could not start before
// IONDRVFramebuffer::enableController, because that hook was the only caller
// of rbgpu_agdc_framebuffer() and that function allocated the thread_call -
// and boot 160 measured the bridge arriving AFTER WindowServer had already
// asked IOPresentment for the AGDC. Nothing about the bridge needs the
// framebuffer any more: since 2rm the timing comes from OTG0, and the one
// load-bearing use was fb->getProvider() for the IOPCIDevice, which
// RembrandtGPUPeer already holds in start(). So the peer hands the device over
// directly and the framebuffer becomes a later, optional refinement.
void rbgpu_agdc_pci(IOService *pciDevice);


// 2rd: the real boot-link timing for attribute 0x921. AGDCLinkConfig_t embeds
// the PUBLIC IODetailedTimingInformationV2, which is exactly what
// IONDRVFramebuffer::getTimingInfoForDisplayMode produces, so the honest
// source for every timing field is the boot framebuffer's own current mode -
// read-only, no DCN register access, nothing invented. Both symbols are
// DEFINED in IONDRVSupport (rule 1, checked with nm), and neither is routed
// anywhere, so this only resolves and CALLS them (rule 3 untouched).
// Without them the bridge refuses 0x921 instead of inventing a reply.
using RbNdrvCurrentMode = IOReturn (*)(IOService *, int32_t *, int32_t *);
using RbNdrvTimingInfo  = IOReturn (*)(IOService *, int32_t, void *);
void rbgpu_agdc_timing_source(RbNdrvCurrentMode, RbNdrvTimingInfo);

// 2rm: boot 159 measured that the NDRV has NO detailed timing to give - its
// one mode carries nominal geometry and a refresh rate, kIODetailedTimingValid
// is clear (agdc_tim_err 3) and there is no IODisplayEDID anywhere in the
// registry. The firmware did program DCN 3.1.2 and OTG0 is scanning, so the
// panel's real timing is in OTG0's registers. This is the fallback source:
// READS only, no DCN register is written, and a failed read refuses 0x921
// exactly as a failed NDRV read does.
struct rb_otg_timing;
using RbOtgSample = bool (*)(struct rb_otg_timing *);
void rbgpu_agdc_otg_source(RbOtgSample);
#endif
