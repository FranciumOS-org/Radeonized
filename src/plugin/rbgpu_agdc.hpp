#ifndef RBGPU_AGDC_HPP
#define RBGPU_AGDC_HPP
#include <Headers/kern_patcher.hpp>
#include <IOKit/IOService.h>
void rbgpu_agdc_load(KernelPatcher &, size_t, mach_vm_address_t, size_t);
void rbgpu_agdc_framebuffer(IOService *);
#endif
