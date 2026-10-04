/* SPDX-License-Identifier: MIT */
/* Module descriptor, as Xcode generates it from MODULE_NAME / MODULE_START /
 * MODULE_STOP: libkmod's _start/_stop call _realmain/_antimain, which are
 * Lilu's plugin entry points from plugin_start.cpp. */
#include <mach/mach_types.h>
#include <mach/kmod.h>

extern kern_return_t _start(kmod_info_t *ki, void *data);
extern kern_return_t _stop(kmod_info_t *ki, void *data);
extern kern_return_t RembrandtGPU_kern_start(kmod_info_t *ki, void *data);
extern kern_return_t RembrandtGPU_kern_stop(kmod_info_t *ki, void *data);

KMOD_EXPLICIT_DECL(com.rembrandtgpu.plugin, PLUGIN_VERSION, _start, _stop)
__private_extern__ kmod_start_func_t *_realmain = RembrandtGPU_kern_start;
__private_extern__ kmod_stop_func_t *_antimain = RembrandtGPU_kern_stop;
__private_extern__ int _kext_apple_cc = __APPLE_CC__;
