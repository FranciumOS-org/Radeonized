/* SPDX-License-Identifier: MIT */
/* RembrandtProbe: see RembrandtProbe.hpp. */
#include "RembrandtProbe.hpp"

#include <IOKit/IOLib.h>
#include <IOKit/IOSubMemoryDescriptor.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <pexpert/pexpert.h>

extern "C" {
#include "rb_probe.h"
}

#define LOG(fmt, ...) IOLog("RembrandtProbe: " fmt "\n", ##__VA_ARGS__)

OSDefineMetaClassAndStructors(RembrandtProbe, IOService)

namespace {

struct HwCtx {
    volatile uint32_t *mmio;
    IOPCIDevice *pci;
};

uint32_t k_rreg(void *ctx, uint32_t off)
{
    return static_cast<HwCtx *>(ctx)->mmio[off / 4];
}

void k_wreg(void *ctx, uint32_t off, uint32_t value)
{
    /* rb_probe.c only lets MM_INDEX / MM_INDEX_HI through to here. */
    static_cast<HwCtx *>(ctx)->mmio[off / 4] = value;
}

/* Map just the pages holding [off, off + size) of BAR0, uncached and
 * read-only, copy them out with 32-bit loads, unmap. rb_probe.c never asks
 * for the first 16 MB, which the boot framebuffer driver has mapped. */
int k_fb_read(void *ctx, uint64_t off, uint8_t *dst, uint32_t size)
{
    HwCtx *c = static_cast<HwCtx *>(ctx);
    IODeviceMemory *bar0 = c->pci->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
    if (!bar0 || (off & 3) || (size & 3))
        return -1;
    uint64_t start = trunc_page_64(off), end = round_page_64(off + size);
    if (end > bar0->getLength())
        return -1;

    IOSubMemoryDescriptor *sub = IOSubMemoryDescriptor::withSubRange(bar0, start, end - start, kIODirectionIn);
    if (!sub)
        return -1;
    IOMemoryMap *map = sub->map(kIOMapInhibitCache | kIOMapReadOnly);
    if (!map) {
        sub->release();
        return -1;
    }
    const volatile uint32_t *p = reinterpret_cast<const volatile uint32_t *>(map->getVirtualAddress() + (off - start));
    for (uint32_t i = 0; i < size / 4; i++) {
        uint32_t v = p[i];
        dst[i * 4] = static_cast<uint8_t>(v);
        dst[i * 4 + 1] = static_cast<uint8_t>(v >> 8);
        dst[i * 4 + 2] = static_cast<uint8_t>(v >> 16);
        dst[i * 4 + 3] = static_cast<uint8_t>(v >> 24);
    }
    map->release();
    sub->release();
    return 0;
}

void set_num(OSDictionary *d, const char *key, uint64_t v, unsigned bits)
{
    if (OSNumber *n = OSNumber::withNumber(v, bits)) {
        d->setObject(key, n);
        n->release();
    }
}

void set_str(OSDictionary *d, const char *key, const char *s)
{
    if (OSString *o = OSString::withCString(s)) {
        d->setObject(key, o);
        o->release();
    }
}

const char *source_name(uint8_t s)
{
    switch (s) {
    case RB_SRC_DEFAULT:       return "default offset (MEMSIZE - 64 KB)";
    case RB_SRC_SCRATCH:       return "DRIVER_SCRATCH registers";
    case RB_SRC_DEFAULT_NOSIG: return "default offset, no signature found";
    default:                   return "none";
    }
}

const char *status_name(uint8_t s)
{
    switch (s) {
    case RB_REG_NO_IP:      return "skipped: IP not in table";
    case RB_REG_VERSION:    return "skipped: IP version differs";
    case RB_REG_OUT_OF_BAR: return "skipped: outside register BAR";
    case RB_REG_LEVEL:      return "not read at this level";
    default:                return "?";
    }
}

} // namespace

bool RembrandtProbe::start(IOService *provider)
{
    if (!IOService::start(provider))
        return false;

    IOPCIDevice *pci = OSDynamicCast(IOPCIDevice, provider);
    if (!pci)
        return false;

    UInt16 vendor = pci->configRead16(kIOPCIConfigVendorID);
    UInt16 device = pci->configRead16(kIOPCIConfigDeviceID);
    UInt8 rev = pci->configRead8(kIOPCIConfigRevisionID);
    UInt16 cmd = pci->configRead16(kIOPCIConfigCommand);
    LOG("attached to %04x:%04x rev %02x, PCI command %#06x", vendor, device, rev, cmd);

    if (vendor != 0x1002 || device != 0x1681) {
        LOG("not a Rembrandt 680M; leaving it alone");
        return false;
    }
    if (!(cmd & kIOPCICommandMemorySpace)) {
        /* The probe never turns decoding on: that is the owning driver's job. */
        LOG("memory decoding is off; leaving it alone");
        return false;
    }

    uint32_t level = RB_LEVEL_SAFE;
    if (OSNumber *n = OSDynamicCast(OSNumber, getProperty("RBProbeLevel")))
        level = n->unsigned32BitValue();
    PE_parse_boot_argn("rbprobe_level", &level, sizeof(level));
    if (level < RB_LEVEL_SAFE || level > RB_LEVEL_MAX)
        level = RB_LEVEL_SAFE;

    IOMemoryMap *mmio = pci->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress5, kIOMapInhibitCache);
    if (!mmio) {
        LOG("cannot map the register BAR (BAR5)");
        return false;
    }
    IODeviceMemory *bar0 = pci->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
    LOG("register BAR %#llx + %#llx, framebuffer BAR %#llx + %#llx, level %u",
        (unsigned long long)mmio->getPhysicalAddress(), (unsigned long long)mmio->getLength(),
        bar0 ? (unsigned long long)bar0->getPhysicalAddress() : 0ull,
        bar0 ? (unsigned long long)bar0->getLength() : 0ull, level);

    auto *r = static_cast<rb_probe_result *>(IOMalloc(sizeof(rb_probe_result)));
    if (!r) {
        mmio->release();
        return false;
    }
    bzero(r, sizeof(*r));

    HwCtx ctx = { reinterpret_cast<volatile uint32_t *>(mmio->getVirtualAddress()), pci };
    rb_hw hw = {};
    hw.ctx = &ctx;
    hw.mmio_len = static_cast<uint32_t>(mmio->getLength());
    hw.rreg = k_rreg;
    hw.wreg = k_wreg;
    hw.fb_len = bar0 ? bar0->getLength() : 0;
    hw.fb_read = bar0 ? k_fb_read : nullptr;

    int rc = rb_probe_run(&hw, static_cast<int>(level), r);
    mmio->release();

    LOG("MEMSIZE %u MB, scratch %#x %#x %#x, C2PMSG_33 %#x, IP_DISCOVERY_VERSION %#x",
        r->memsize_mb, r->scratch[0], r->scratch[1], r->scratch[2], r->c2pmsg33, r->ipd_version);
    LOG("discovery table: %s, offset %#llx, %u bytes; BAR0 cross-check %s",
        source_name(r->disc_source), (unsigned long long)r->disc_offset, r->disc_size,
        !r->bar0_checked ? "not done" : r->bar0_matches ? "matches" : "DIFFERS");
    if (rc) {
        LOG("probe stopped: %s", r->fail ? r->fail : "?");
    } else {
        LOG("%u IPs, table v%u, check flags %#x", r->disc.nips, r->disc.ip_table_version, r->disc.check_flags);
        for (uint32_t i = 0; i < r->disc.nips; i++) {
            const rb_ip *ip = &r->disc.ips[i];
            LOG("  %-9s inst %u  v%u.%u.%u  base0 %#x", rb_hwid_name(ip->hw_id), ip->inst,
                ip->major, ip->minor, ip->revision, ip->nbase ? ip->base[0] : 0);
        }
        for (uint32_t i = 0; i < r->nregs; i++)
            if (r->regs[i].status == RB_REG_OK)
                LOG("  %-26s [%#07x] = %#010x", r->regs[i].name, r->regs[i].dw, r->regs[i].value);
    }
    LOG("index writes %u, refused writes %u", r->index_writes, r->refused_writes);

    publish(r, pci);
    IOFree(r, sizeof(*r));
    /* Stay attached so the results remain in the registry until unload. */
    return true;
}

void RembrandtProbe::publish(const rb_probe_result *r, IOPCIDevice *pci)
{
    OSDictionary *d = OSDictionary::withCapacity(32);
    if (!d)
        return;

    set_num(d, "PCIVendor", pci->configRead16(kIOPCIConfigVendorID), 16);
    set_num(d, "PCIDevice", pci->configRead16(kIOPCIConfigDeviceID), 16);
    set_num(d, "PCIRevision", pci->configRead8(kIOPCIConfigRevisionID), 8);
    set_num(d, "Level", static_cast<uint64_t>(r->level), 32);
    set_str(d, "Result", r->fail ? r->fail : "ok");
    set_num(d, "MemsizeMB", r->memsize_mb, 32);
    set_num(d, "DriverScratch0", r->scratch[0], 32);
    set_num(d, "DriverScratch1", r->scratch[1], 32);
    set_num(d, "DriverScratch2", r->scratch[2], 32);
    set_num(d, "MP0_C2PMSG_33_fixed", r->c2pmsg33, 32);
    set_num(d, "IPDiscoveryVersion", r->ipd_version, 32);
    set_str(d, "DiscoverySource", source_name(r->disc_source));
    set_num(d, "DiscoveryOffset", r->disc_offset, 64);
    set_num(d, "DiscoverySize", r->disc_size, 32);
    set_str(d, "BAR0CrossCheck", !r->bar0_checked ? "not done" : r->bar0_matches ? "matches" : "differs");
    set_num(d, "ParseError", static_cast<uint32_t>(r->parse_err), 32);
    set_num(d, "CheckFlags", r->disc.check_flags, 32);
    set_num(d, "IndexWrites", r->index_writes, 32);
    set_num(d, "RefusedWrites", r->refused_writes, 32);

    if (r->disc_size) {
        if (OSData *bin = OSData::withBytes(r->disc_bin, r->disc_size)) {
            d->setObject("DiscoveryBinary", bin);
            bin->release();
        }
    }

    if (OSArray *ips = OSArray::withCapacity(r->disc.nips ? r->disc.nips : 1)) {
        for (uint32_t i = 0; i < r->disc.nips; i++) {
            const rb_ip *ip = &r->disc.ips[i];
            char line[160];
            size_t n = static_cast<size_t>(snprintf(line, sizeof(line), "%s(%u) inst %u v%u.%u.%u.%u bases",
                                                    rb_hwid_name(ip->hw_id), ip->hw_id, ip->inst,
                                                    ip->major, ip->minor, ip->revision, ip->sub_revision));
            for (uint32_t k = 0; k < ip->nbase && k < RB_DISC_MAX_BASES && n < sizeof(line); k++)
                n += static_cast<size_t>(snprintf(line + n, sizeof(line) - n, " %#x", ip->base[k]));
            if (OSString *s = OSString::withCString(line)) {
                ips->setObject(s);
                s->release();
            }
        }
        d->setObject("IPs", ips);
        ips->release();
    }

    if (OSDictionary *regs = OSDictionary::withCapacity(r->nregs ? r->nregs : 1)) {
        for (uint32_t i = 0; i < r->nregs; i++) {
            const rb_reg_sample *s = &r->regs[i];
            char v[64];
            if (s->status == RB_REG_OK)
                snprintf(v, sizeof(v), "0x%08x @ dword 0x%x", s->value, s->dw);
            else
                snprintf(v, sizeof(v), "%s", status_name(s->status));
            set_str(regs, s->name, v);
        }
        d->setObject("Registers", regs);
        regs->release();
    }

    setProperty("RBProbe", d);
    d->release();
}

void RembrandtProbe::stop(IOService *provider)
{
    LOG("stopping");
    IOService::stop(provider);
}
