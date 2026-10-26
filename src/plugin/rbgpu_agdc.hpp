#ifndef RBGPU_AGDC_HPP
#define RBGPU_AGDC_HPP
#include <Headers/kern_patcher.hpp>
#include <IOKit/IOService.h>
void rbgpu_agdc_load(KernelPatcher &, size_t, mach_vm_address_t, size_t);
void rbgpu_agdc_framebuffer(IOService *);

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
#endif
