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
            *fn = (uint8_t *)(row->fn_vm + _dyld_get_image_vmaddr_slide(i));
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
