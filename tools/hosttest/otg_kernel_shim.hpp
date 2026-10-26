// Minimal IOKit surface for executing the production OTG sampler on the host.
#pragma once
#include <cstdint>
#include <cstddef>

constexpr unsigned kIOPCIConfigVendorID=0, kIOPCIConfigDeviceID=2;
constexpr unsigned kIOPCIConfigCommand=4, kIOPCIConfigRevisionID=8;
constexpr unsigned kIOPCIConfigBaseAddress5=0x24;
constexpr unsigned kIOPCICommandMemorySpace=2;
constexpr unsigned kIOMapInhibitCache=0x100, kIOMapReadOnly=0x1000;

struct IOMemoryMap {
    uintptr_t address=0;
    uint64_t length=0;
    unsigned releases=0;
    uintptr_t getVirtualAddress() const { return address; }
    uint64_t getLength() const { return length; }
    void release() { releases++; }
};
struct IOPCIDevice {
    unsigned vendor=0x1002, device=0x1681, revision=0x0a, command=2;
    unsigned maps=0, reg=0, options=0;
    IOMemoryMap *mapping=nullptr;
    unsigned configRead16(unsigned r) const {
        return r==kIOPCIConfigVendorID ? vendor :
               r==kIOPCIConfigDeviceID ? device : command;
    }
    unsigned configRead8(unsigned) const { return revision; }
    IOMemoryMap *mapDeviceMemoryWithRegister(unsigned r, unsigned o) {
        maps++; reg=r; options=o; return mapping;
    }
};
void rbgpu_diag_set_num(const char *, uint64_t);
void IODelay(unsigned);
uint64_t mach_absolute_time();
void absolutetime_to_nanoseconds(uint64_t, uint64_t *);
struct rb_otg_timing;
bool rbgpu_agdc_sample_otg(IOPCIDevice *, rb_otg_timing *);
