/* SPDX-License-Identifier: MIT */
/* Module descriptor. An IOKit kext has no module entry points of its own:
 * libkmod's _start/_stop see NULL here and return, and IOKit drives the C++
 * class from the personality in Info.plist. */
#include <mach/mach_types.h>
#include <mach/kmod.h>

extern kern_return_t _start(kmod_info_t *ki, void *data);
extern kern_return_t _stop(kmod_info_t *ki, void *data);

KMOD_EXPLICIT_DECL(com.rembrandtgpu.probe, "0.1.0", _start, _stop)
__private_extern__ kmod_start_func_t *_realmain = 0;
__private_extern__ kmod_stop_func_t *_antimain = 0;
__private_extern__ int _kext_apple_cc = __APPLE_CC__;
