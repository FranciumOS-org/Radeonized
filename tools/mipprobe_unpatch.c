// In-process A/B for the swizzle detour: put the driver's original prologue
// back in THIS process only. The kext places the detour once per pid, so a
// removal after the device is up stays removed.
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach-o/dyld.h>
#include <string.h>
#include <stdio.h>
#include <sys/sysctl.h>
#include "rbgpu_mtlcave.h"
#include "mipprobe_unpatch.h"

static intptr_t g_slide;

static const struct rb_mtlcave_row *row_and_fn(uint8_t **fn)
{
    char rel[64]; size_t n = sizeof(rel); int maj = 0;
    if (sysctlbyname("kern.osrelease", rel, &n, NULL, 0) != 0) return NULL;
    for (const char *c = rel; *c >= '0' && *c <= '9'; c++) maj = maj * 10 + (*c - '0');
    const struct rb_mtlcave_row *row = rbgpu_mtlcave_row(maj);
    if (!row) return NULL;
    for (uint32_t i = 0; i < _dyld_image_count(); i++) {
        const char *nm = _dyld_get_image_name(i);
        if (nm && strstr(nm, "AMDRadeonX6000MTLDriver")) {
            g_slide = _dyld_get_image_vmaddr_slide(i);
            *fn = (uint8_t *)(row->fn_vm + g_slide);
            return row;
        }
    }
    return NULL;
}

int rb_detour_state(void)
{
    uint8_t *fn = NULL;
    const struct rb_mtlcave_row *row = row_and_fn(&fn);
    if (!row) return -2;
    if (memcmp(fn, row->fn_repl, row->fn_len) == 0) return 1;
    if (memcmp(fn, row->fn_find, row->fn_len) == 0) return 0;
    return -1;
}

int rb_detour_remove(void)
{
    uint8_t *fn = NULL;
    const struct rb_mtlcave_row *row = row_and_fn(&fn);
    if (!row) return -2;
    mach_vm_address_t pg = (mach_vm_address_t)fn & ~0xfffULL;
    mach_vm_size_t len = (((mach_vm_address_t)fn + row->fn_len + 0xfff) & ~0xfffULL) - pg;
    kern_return_t kr = mach_vm_protect(mach_task_self(), pg, len, FALSE,
                                       VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY);
    if (kr != KERN_SUCCESS) return kr;
    memcpy(fn, row->fn_find, row->fn_len);
    kr = mach_vm_protect(mach_task_self(), pg, len, FALSE, VM_PROT_READ | VM_PROT_EXECUTE);
    return kr;
}

// 3mip: the cave half. The kext wrote ITS cave into this process when the
// user client opened; the row compiled into this tool may carry a newer one.
// rb_cave_state() says which is in memory, so a run can never claim a policy
// it did not execute, and rb_cave_install() puts this build's bytes in - the
// same bytes, at the same two places, that the kext will write after the next
// boot. It is a dry run of the plugin, in the boot before the plugin exists.
static int poke(uint8_t *at, const uint8_t *bytes, size_t n)
{
    mach_vm_address_t pg = (mach_vm_address_t)at & ~0xfffULL;
    mach_vm_size_t len = (((mach_vm_address_t)at + n + 0xfff) & ~0xfffULL) - pg;
    kern_return_t kr = mach_vm_protect(mach_task_self(), pg, len, FALSE,
                                       VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY);
    if (kr != KERN_SUCCESS) return kr;
    memcpy(at, bytes, n);
    return mach_vm_protect(mach_task_self(), pg, len, FALSE, VM_PROT_READ | VM_PROT_EXECUTE);
}

int rb_cave_state(void)
{
    uint8_t *fn = NULL;
    const struct rb_mtlcave_row *row = row_and_fn(&fn);
    if (!row) return -2;
    const uint8_t *cave = (const uint8_t *)(row->cave_vm + g_slide);
    return memcmp(cave, row->cave_repl + row->cave_anchor, row->cave_used) == 0;
}

int rb_cave_install(void)
{
    uint8_t *fn = NULL;
    const struct rb_mtlcave_row *row = row_and_fn(&fn);
    if (!row) return -2;
    uint8_t *cave = (uint8_t *)(row->cave_vm + g_slide);
    // The anchor is the only thing that says this zero run is the right one,
    // and the kext checks it before every write. So does this.
    if (memcmp(cave - row->cave_anchor, row->cave_find, row->cave_anchor) != 0) return -3;
    int kr = poke(fn, row->fn_find, row->fn_len);          // nothing may enter a half-written cave
    if (kr) return kr;
    kr = poke(cave, row->cave_repl + row->cave_anchor, row->cave_reserved);
    if (kr) return kr;
    return poke(fn, row->fn_repl, row->fn_len);
}
