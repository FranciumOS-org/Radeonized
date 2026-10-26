#include "otg_kernel_shim.hpp"
#include "rb_otg.h"
#include "rb_agdc.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>

static uint32_t regs[0x6000];
static uint64_t nowUs;
static uint32_t initialFrame;
static unsigned periodUs, mutateAt;
static std::map<std::string,uint64_t> diag;
constexpr unsigned base=0x34c0, frame=base+0x1b4c;

void rbgpu_diag_set_num(const char *key,uint64_t v) { diag[key]=v; }
uint64_t mach_absolute_time() { return nowUs*1000; }
void absolutetime_to_nanoseconds(uint64_t ticks,uint64_t *ns) { *ns=ticks; }
void IODelay(unsigned us) {
    nowUs+=us;
    if (periodUs) regs[frame]=initialFrame+uint32_t(nowUs/periodUs);
    if (mutateAt && nowUs>=mutateAt) regs[base+0x1b2a]=2200;
}

static void reset(IOPCIDevice &pci,IOMemoryMap &mapping) {
    memset(regs,0,sizeof regs);
    diag.clear(); nowUs=0; initialFrame=0; periodUs=6952; mutateAt=0;
    regs[base+0x1b41]=0x11201;
    for (unsigned i=1;i<4;i++) regs[base+0x1b41+i*0x80]=0x300;
    regs[base+0x1b2a]=2106; regs[base+0x1b2b]=0x7f07ff;
    regs[base+0x1b2c]=0x3c0000;
    regs[base+0x1b2f]=1129; regs[base+0x1b36]=0x14044c;
    regs[base+0x1b37]=0x50000; regs[base+0x1b38]=1;
    mapping=IOMemoryMap{};
    mapping.address=reinterpret_cast<uintptr_t>(regs);
    mapping.length=sizeof regs;
    pci=IOPCIDevice{}; pci.mapping=&mapping;
}

int main() {
    IOPCIDevice pci; IOMemoryMap mapping; rb_otg_timing timing{};
    reset(pci,mapping);
    // No NDRV, source-binding callback or AMD g_hw exists in this executable.
    assert(rbgpu_agdc_sample_otg(&pci,&timing));
    assert(diag.at("otg_err")==0 && diag.at("otg_frames")==16);
    assert(timing.refresh_mhz>143000 && timing.refresh_mhz<145000);
    assert(mapping.releases==1 && pci.maps==1);
    assert(pci.reg==kIOPCIConfigBaseAddress5);
    assert(pci.options==(kIOMapReadOnly|kIOMapInhibitCache));
    uint8_t v2[RB_V2_SIZE], link[RB_AGDC_LINK_SIZE]={};
    assert(rb_otg_to_v2(&timing,v2,sizeof v2)==0);
    size_t len=sizeof link;
    assert(rb_agdc_reply(0x921,sizeof link,link,&len,1,1,v2)==0);
    assert(rb_agdc_get32(link+RB_AGDC_TIMING_OFF+RB_V2_HACTIVE)==1920);
    assert(rb_agdc_get32(link+RB_AGDC_TIMING_OFF+RB_V2_VACTIVE)==1080);

    reset(pci,mapping); initialFrame=UINT32_MAX-8; regs[frame]=initialFrame;
    assert(rbgpu_agdc_sample_otg(&pci,&timing));
    assert(diag.at("otg_frames")==16 && mapping.releases==1);

    reset(pci,mapping); periodUs=0;
    assert(!rbgpu_agdc_sample_otg(&pci,&timing));
    assert(diag.at("otg_err")==4 && nowUs<400000 && mapping.releases==1);
    reset(pci,mapping); mutateAt=50000;
    assert(!rbgpu_agdc_sample_otg(&pci,&timing));
    assert(diag.at("otg_err")==10 && mapping.releases==1);
    reset(pci,mapping); periodUs=1000;
    assert(!rbgpu_agdc_sample_otg(&pci,&timing));
    assert(diag.at("otg_err")==5 && mapping.releases==1);

    // Refuse identity/decoding failures before mapping any device memory.
    for (unsigned which=0;which<4;which++) {
        reset(pci,mapping);
        if (which==0) pci.vendor=0xffff;
        if (which==1) pci.device=0xffff;
        if (which==2) pci.revision=0xff;
        if (which==3) pci.command=0;
        assert(!rbgpu_agdc_sample_otg(&pci,&timing));
        assert(diag.at("otg_err")== (which==3 ? 7 : 6));
        assert(pci.maps==0 && mapping.releases==0);
    }
    reset(pci,mapping); pci.mapping=nullptr;
    assert(!rbgpu_agdc_sample_otg(&pci,&timing) && diag.at("otg_err")==8);
    // An inaccessible address with a short aperture must never be read.
    reset(pci,mapping); mapping.address=4; mapping.length=0x14004;
    assert(!rbgpu_agdc_sample_otg(&pci,&timing));
    assert(diag.at("otg_err")==9 && mapping.releases==1);
    reset(pci,mapping); mapping.address=0;
    assert(!rbgpu_agdc_sample_otg(&pci,&timing));
    assert(diag.at("otg_err")==9 && mapping.releases==1);
    reset(pci,mapping); regs[base+0x1b41+0x80]=0x301;
    assert(!rbgpu_agdc_sample_otg(&pci,&timing));
    assert(diag.at("otg_err")==3 && mapping.releases==1);
    reset(pci,mapping);
    assert(!rbgpu_agdc_sample_otg(&pci,nullptr) && diag.at("otg_err")==1);
    assert(!rbgpu_agdc_sample_otg(nullptr,&timing) && diag.at("otg_err")==6);
    puts("OTG kernel sampler: early timing, edge latch, refusal and unmap tests passed");
}
