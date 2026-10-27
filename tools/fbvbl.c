/* SPDX-License-Identifier: MIT */
/* Read the EXISTING IOFramebuffer shared VBL counter. No cursor creation,
 * target suspension, kernel-memory probe, or write to the mapped memory. */
#include <IOKit/IOKitLib.h>
#include <IOKit/graphics/IOGraphicsTypes.h>
#include <IOKit/graphics/IOFramebufferShared.h>
#include <mach/mach_time.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    unsigned seconds = 2;
    if (argc > 2) {
        fprintf(stderr, "usage: fbvbl [seconds: 1..10]\n");
        return 2;
    }
    if (argc == 2) {
        char *end = NULL;
        long value = strtol(argv[1], &end, 10);
        if (!*argv[1] || *end || value < 1 || value > 10) {
            fprintf(stderr, "seconds must be 1..10\n");
            return 2;
        }
        seconds = (unsigned)value;
    }
    io_iterator_t iterator = IO_OBJECT_NULL;
    kern_return_t result = IOServiceGetMatchingServices(kIOMainPortDefault,
                                    IOServiceMatching("IOFramebuffer"), &iterator);
    if (result) {
        fprintf(stderr, "IOFramebuffer match failed: %#x\n", result);
        return 1;
    }
    unsigned measured = 0;
    io_service_t framebuffer;
    while ((framebuffer = IOIteratorNext(iterator))) {
        io_name_t name = "unknown";
        uint64_t registryID = 0;
        IORegistryEntryGetName(framebuffer, name);
        IORegistryEntryGetRegistryEntryID(framebuffer, &registryID);
        printf("%s registry=%#llx\n", name, registryID);
        io_connect_t connection = IO_OBJECT_NULL;
        result = IOServiceOpen(framebuffer, mach_task_self(),
                              kIOFBSharedConnectType, &connection);
        IOObjectRelease(framebuffer);
        if (result) {
            printf("  read-only shared connection failed: %#x; no measurement\n", result);
            continue;
        }
        mach_vm_address_t address = 0;
        mach_vm_size_t size = 0;
        result = IOConnectMapMemory64(connection, kIOFBCursorMemory,
                      mach_task_self(), &address, &size, kIOMapAnywhere | kIOMapReadOnly);
        if (result) {
            printf("  existing shared memory unavailable: %#x; no measurement\n", result);
            IOServiceClose(connection);
            continue;
        }
        if (size >= offsetof(StdFBShmem_t, vblCount) + sizeof(uint64_t)) {
            const volatile StdFBShmem_t *shared = (const volatile StdFBShmem_t *)address;
            // Layout pinned to the public SDK: version 3, as measured on this
            // machine. Refuse an unknown layout before interpreting a counter.
            printf("  version=%d structSize=%d mappingBytes=%llu countOffset=%zu\n",
                   shared->version, shared->structSize, size, offsetof(StdFBShmem_t, vblCount));
            if (shared->version == 3 && shared->structSize > 0 &&
                (uint64_t)shared->structSize <= size &&
                (size_t)shared->structSize >= offsetof(StdFBShmem_t, vblCount) + sizeof(uint64_t)) {
                const uint64_t start = shared->vblCount;
                const uint64_t startTime = mach_absolute_time();
                sleep(seconds);
                const uint64_t finish = shared->vblCount;
                const uint64_t elapsed = mach_absolute_time() - startTime;
                mach_timebase_info_data_t timebase;
                mach_timebase_info(&timebase);
                const double ns = (double)elapsed * timebase.numer / timebase.denom;
                if (finish >= start && ns > 0) {
                    printf("  vblCount %llu -> %llu; delta=%llu over %.3f s = %.3f Hz\n",
                           start, finish, finish-start, ns/1e9, (finish-start)*1e9/ns);
                    measured++;
                } else {
                    puts("  counter reset or invalid sample interval; no rate reported");
                }
            } else {
                puts("  unknown shared-memory layout; no measurement");
            }
        } else {
            printf("  mapping too short: %llu bytes; no measurement\n", size);
        }
        IOConnectUnmapMemory64(connection, kIOFBCursorMemory, mach_task_self(), address);
        IOServiceClose(connection);
    }
    IOObjectRelease(iterator);
    return measured ? 0 : 1;
}
