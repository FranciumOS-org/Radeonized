// 2ra: per-GPU AGDC bridge for IOPresentment. OFF unless -rbgpuagdc AND
// rbgpu-agdc one-shot. No IOFramebuffer takeover and no DCN register writes.
// Runtime superclass/vtables avoid a system-KC static dependency (2qy).
#include "rbgpu_agdc.hpp"
#include "rbgpu_agdc_lookup.hpp"
#include "RembrandtGPUPeer.hpp"
#include "rb_agdc.h"
#include <Headers/kern_util.hpp>
#include <IOKit/IOLib.h>
#include <libkern/c++/OSSymbol.h>
#include <libkern/c++/OSDictionary.h>
#include <kern/clock.h>

namespace {
constexpr size_t objectSize=0x110, tableBytes=0x8b8, metaTableBytes=0xe8;
constexpr uintptr_t kextFloor=0xffffff7f80000000ULL;
const char className[]="RembrandtGPUAGDC";
alignas(16) uint64_t metaStorage[16];
alignas(16) uintptr_t table[tableBytes/8], metaTable[metaTableBytes/8];
const OSMetaClass *superMeta;
IOService *service, *framebuffer;
IOPCIDevice *pci;
thread_call_t work;
bool armed, finished;
unsigned retries;
using Ctor=void (*)(void *,const OSMetaClass *);
using Dtor=void (*)(void *);
using McCtor=void (*)(void *,const char *,const OSMetaClass *,unsigned);
using Pre=void *(*)(const char *);
using Post=int (*)(void *);
using Filter=IOReturn (*)(void *,unsigned,unsigned,void *,unsigned long,
                         void *,unsigned long *,void *);
Ctor ctor;
Dtor dtor;
McCtor mcCtor;
Pre pre;
Post post;
Filter originalFilter;

void refuse(unsigned code,const char *reason) {
    rbgpu_diag_set_num("agdc_error",code);
    rbgpu_diag_set_str("agdc_say",reason);
    rbgpu_log("2ra: AGDC refusal %u: %s",code,reason);
}

const OSMetaClass *ourMeta() {
    return reinterpret_cast<const OSMetaClass *>(metaStorage);
}
const OSMetaClass *getMeta(const void *) { return ourMeta(); }
void destroy(void *p) { dtor(p); }
void deleteObject(void *p) { destroy(p); IOFree(p,objectSize); }
OSObject *allocate(const void *) {
    // This experimental class has one pinned instance, no personality.
    if (service) return nullptr;
    void *p=IOMallocZero(objectSize);
    if (!p) return nullptr;
    ctor(p,ourMeta());
    *static_cast<uintptr_t **>(p)=table+2;
    ourMeta()->instanceConstructed(); // compiler-generated constructors do this
    return reinterpret_cast<OSObject *>(p);
}

IOReturn vendor(void *,unsigned attr,unsigned long *,unsigned long inSize,
                unsigned long *out,unsigned long *outSize,void *) {
    if (inSize || !outSize) return kIOReturnBadArgument;
    if (attr==1) {
        rbgpu_diag_inc("agdc_vendor_calls");
        return rb_agdc_vendor(out,*outSize)==0 ? kIOReturnSuccess : kIOReturnBadArgument;
    }
    if (attr==0x980) {
        rbgpu_diag_inc("agdc_cap_calls");
        // KERNEL pointers, not registry IDs. Keep Apple's filtered 0x980
        // handler: it converts these before copying anything to userspace.
        return rb_agdc_capability(out,*outSize,reinterpret_cast<uint64_t>(pci),
                   reinterpret_cast<uint64_t>(framebuffer))==0
                   ? kIOReturnSuccess : kIOReturnBadArgument;
    }
    rbgpu_diag_set_num("agdc_vendor_unsupported",attr);
    return kIOReturnUnsupported;
}

IOReturn filter(void *self,unsigned api,unsigned attr,void *in,
                unsigned long inSize,void *out,unsigned long *outSize,void *state) {
    // Attribute 1 normally enters the global AGDCPlugin table. Only this
    // service answers it locally. ALL other services/attributes use Apple.
    // newUserClient and authorizeTask remain Apple's unchanged code.
    if (self==service && service) {
        rbgpu_diag_set_num("agdc_last_attr",attr);
        rbgpu_diag_set_num("agdc_last_api",api);
        if (api==3 && attr==1) rbgpu_diag_inc("agdc_user_vendor");
        if (api==3 && attr==0x980) rbgpu_diag_inc("agdc_user_cap");
    }
    if (self==service && service && attr==1 && (api==0 || api==2 || api==3))
        return vendor(self,attr,static_cast<unsigned long *>(in),inSize,
                      static_cast<unsigned long *>(out),outSize,nullptr);
    return originalFilter(self,api,attr,in,inSize,out,outSize,state);
}

uintptr_t solve(KernelPatcher &p,size_t id,const char *name,
                mach_vm_address_t base=0,size_t size=0,const char *key=nullptr) {
    uintptr_t raw=0;
    auto a=rb_agdc_lookup(p,id,name,static_cast<uintptr_t>(base),size,&raw);
    if (key) rbgpu_diag_set_num(key,raw);
    if (p.getError()!=KernelPatcher::Error::NoError || a<kextFloor) {
        p.clearError(); rbgpu_log("2rb: unresolved/refused %s (raw %p, base %p, size %#lx)",
            name,reinterpret_cast<void *>(raw),reinterpret_cast<void *>(base),size);
        return 0;
    }
    return a;
}

void run(thread_call_param_t,thread_call_param_t) {
    if (finished || !framebuffer) return;
    if (!armed) { rbgpu_diag_set_num("agdc_wait_load",1); return; }
    // Lookup certifies registration; a BSS metaclass at kext-load time does
    // not yet contain an OSSymbol at +0x18 (2qv/2qx).
    const OSSymbol *name=OSSymbol::withCStringNoCopy("AppleGraphicsDeviceControl");
    const OSMetaClass *registered=name ? OSMetaClass::getMetaClassWithName(name) : nullptr;
    if (name) name->release();
    auto matching=IOService::serviceMatching("AppleGPUWrangler");
    IOService *wrangler=matching ? IOService::copyMatchingService(matching) : nullptr;
    if (matching) matching->release();
    const bool ready=registered && registered==superMeta && wrangler;
    if (wrangler) wrangler->release();
    if (!ready) {
        rbgpu_diag_set_num("agdc_wait",++retries);
        if (retries<100) {
            uint64_t deadline; clock_interval_to_deadline(100,kMillisecondScale,&deadline);
            thread_call_enter_delayed(work,deadline);
        } else {
            finished=true; refuse(8,"superclass/wrangler not ready");
        }
        return;
    }
    finished=true;
    if (superMeta->getClassSize()!=objectSize ||
        superMeta->getSuperClass()!=&IOService::gMetaClass) {
        refuse(9,"superclass layout changed"); return;
    }

    // All preparation is finished before the stalled-class lock. postModLoad
    // MUST run even on a NULL preModLoad handle; no diagnostics in between.
    rbgpu_diag_set_num("agdc_stage",40);
    void *handle=pre("com.rembrandtgpu.plugin");
    mcCtor(metaStorage,className,superMeta,objectSize);
    const int result=post(handle);
    rbgpu_diag_set_num("agdc_stage",41);
    rbgpu_diag_set_num("agdc_mc_post",static_cast<uint32_t>(result));
    if (result || metaStorage[3]==reinterpret_cast<uint64_t>(className)) {
        refuse(10,"metaclass registration failed"); return;
    }
    metaStorage[0]=reinterpret_cast<uintptr_t>(metaTable+2);
    const char *actual=ourMeta()->getClassName();
    if (!actual || strcmp(actual,className)) {
        refuse(11,"metaclass name mismatch"); return;
    }
    rbgpu_diag_set_num("agdc_mc_ok",1);
    service=reinterpret_cast<IOService *>(allocate(nullptr));
    if (!service) { refuse(12,"allocation failed"); return; }
    if (!service->init()) {
        auto failed=service; service=nullptr; failed->release();
        refuse(13,"init failed"); return;
    }
    service->setName(className);
    if (!service->setProperty("IOUserClientClass","AppleGraphicsDeviceControlClient") ||
        !service->setProperty("RBGPUAGDCBridge",true) || !service->attach(pci)) {
        auto failed=service; service=nullptr; failed->release();
        refuse(14,"properties/attach failed"); return;
    }
    rbgpu_diag_set_num("agdc_pci_id",pci->getRegistryEntryID());
    rbgpu_diag_set_num("agdc_fb_id",framebuffer->getRegistryEntryID());
    rbgpu_diag_set_num("agdc_start_enter",1);
    rbgpu_diag_set_num("agdc_stage",60);
    bool started=service->start(pci); // AGDC::start registers the service itself
    rbgpu_diag_set_num("agdc_stage",70);
    rbgpu_diag_set_num("agdc_started",started ? 1 : 0);
    // Retain the initial allocation for the boot, EVEN on start failure.
    // start() may have registered with another kext; freeing it repeated 2pk's panic.
    rbgpu_diag_set_str("agdc_say",started ? "PUBLISHED: verify IOPresentment in WindowServer"
                                        : "START FAILED: instance pinned; inspect log");
    if (!started) refuse(15,"start failed; instance pinned");
    rbgpu_log("2ra: per-GPU AGDC start=%d, registry id=0x%llx; no DCN writes",
              started,service->getRegistryEntryID());
}
} // namespace

void rbgpu_agdc_load(KernelPatcher &p,size_t id,mach_vm_address_t base,size_t size) {
    rbgpu_diag_set_num("agdc_on",checkKernelArgument("-rbgpuagdc") ? 1 : 0);
    if (!checkKernelArgument("-rbgpuagdc")) return;
    rbgpu_diag_set_num("agdc_error",0);
    rbgpu_diag_set_num("agdc_base",base);
    rbgpu_diag_set_num("agdc_size",size);
    auto vt=solve(p,id,"__ZTV26AppleGraphicsDeviceControl",base,size,"agdc_vt");
    auto mvt=solve(p,id,"__ZTVN26AppleGraphicsDeviceControl9MetaClassE",base,size,"agdc_mvt");
    superMeta=reinterpret_cast<const OSMetaClass *>(solve(p,id,"__ZN26AppleGraphicsDeviceControl10gMetaClassE",base,size,"agdc_mc"));
    ctor=reinterpret_cast<Ctor>(solve(p,id,"__ZN26AppleGraphicsDeviceControlC2EPK11OSMetaClass",base,size,"agdc_ctor"));
    dtor=reinterpret_cast<Dtor>(solve(p,id,"__ZN26AppleGraphicsDeviceControlD2Ev",base,size,"agdc_dtor"));
    mcCtor=reinterpret_cast<McCtor>(solve(p,KernelPatcher::KernelID,"__ZN11OSMetaClassC2EPKcPKS_j",0,0,"agdc_mcc"));
    pre=reinterpret_cast<Pre>(solve(p,KernelPatcher::KernelID,"__ZN11OSMetaClass10preModLoadEPKc",0,0,"agdc_pre"));
    post=reinterpret_cast<Post>(solve(p,KernelPatcher::KernelID,"__ZN11OSMetaClass11postModLoadEPv",0,0,"agdc_post"));
    auto pure=solve(p,KernelPatcher::KernelID,"___cxa_pure_virtual",0,0,"agdc_pure");
    auto fn=solve(p,id,"__ZN26AppleGraphicsDeviceControl26filtered_doDeviceAttributeENS_19agdc_filtered_api_tEjPmmS1_S1_P17AGDCClientState_t",base,size,"agdc_filter");
    const unsigned solved=(vt?1u:0u)|(mvt?2u:0u)|(superMeta?4u:0u)|(ctor?8u:0u)|
        (dtor?16u:0u)|(mcCtor?32u:0u)|(pre?64u:0u)|(post?128u:0u)|(pure?256u:0u)|(fn?512u:0u);
    rbgpu_diag_set_num("agdc_solved",solved); // 0x3ff required; kernel inputs are bits 5..8
    if (!vt || !mvt || !superMeta || !ctor || !dtor || !mcCtor || !pre || !post || !pure || !fn ||
        vt<base || size<tableBytes || vt-base>size-tableBytes ||
        mvt<base || size<metaTableBytes || mvt-base>size-metaTableBytes) {
        refuse(1,"required symbols/ranges"); return;
    }
    memcpy(table,reinterpret_cast<void *>(vt),tableBytes);
    memcpy(metaTable,reinterpret_cast<void *>(mvt),metaTableBytes);
    struct Site { size_t slot; const char *name; } sites[]={
        {0x00,"__ZN26AppleGraphicsDeviceControlD1Ev"},
        {0x08,"__ZN26AppleGraphicsDeviceControlD0Ev"},
        {0x38,"__ZNK26AppleGraphicsDeviceControl12getMetaClassEv"},
        {0x5c0,"__ZN26AppleGraphicsDeviceControl5startEP9IOService"},
        {0x770,"__ZN26AppleGraphicsDeviceControl13newUserClientEP4taskPvjP12OSDictionaryPP12IOUserClient"},
        {0x858,"__ZN26AppleGraphicsDeviceControl24vendor_doDeviceAttributeEjPmmS0_S0_P17AGDCClientState_t"},
    };
    bool layout=table[0]==0 && table[1]==0 && table[tableBytes/8-1]==0 &&
                metaTable[metaTableBytes/8-1]==0 && table[2+0x850/8]==pure;
    for (auto &s:sites) {
        auto expected=solve(p,id,s.name,base,size);
        if (!expected || table[2+s.slot/8]!=expected)
            rbgpu_diag_set_num("agdc_bad_slot",s.slot);
        layout=layout && expected && table[2+s.slot/8]==expected;
    }
    auto alloc=solve(p,id,"__ZNK26AppleGraphicsDeviceControl9MetaClass5allocEv",base,size);
    layout=layout && alloc && metaTable[2+0x88/8]==alloc;
    for (size_t i=2;i<tableBytes/8-1;i++)
        if (table[i]<kextFloor || (table[i]==pure && i!=2+0x850/8)) layout=false;
    const uint8_t prologue[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56};
    if (!layout || memcmp(reinterpret_cast<void *>(fn),prologue,sizeof prologue)) {
        refuse(2,"vtable/prologue mismatch"); return;
    }
    table[2]=reinterpret_cast<uintptr_t>(destroy);
    table[3]=reinterpret_cast<uintptr_t>(deleteObject);
    table[2+0x38/8]=reinterpret_cast<uintptr_t>(getMeta);
    table[2+0x850/8]=reinterpret_cast<uintptr_t>(vendor);
    metaTable[2+0x88/8]=reinterpret_cast<uintptr_t>(allocate);
    rbgpu_diag_set_num("agdc_layout_ok",1);
    rbgpu_diag_set_num("agdc_stage",10);
    uint32_t why=0;
    if (!rbgpu_nvram_consume_oneshot("rbgpu-agdc",&why)) {
        rbgpu_diag_set_num("agdc_oneshot",why);
        refuse(3,"UNARMED: no class or service created"); return;
    }
    rbgpu_diag_set_num("agdc_oneshot",why);
    rbgpu_diag_set_num("agdc_stage",20);
    // Defined in AGDC; first displaced instructions contain no branch.
    // Grepped across the whole plugin: no existing route (rules 1/2/3/5).
    KernelPatcher::RouteRequest route(
        "__ZN26AppleGraphicsDeviceControl26filtered_doDeviceAttributeENS_19agdc_filtered_api_tEjPmmS1_S1_P17AGDCClientState_t",
        filter,originalFilter);
    if (!p.routeMultiple(id,&route,1,base,size)) {
        p.clearError(); refuse(4,"filtered route failed"); return;
    }
    armed=true;
    rbgpu_diag_set_num("agdc_stage",30);
    rbgpu_diag_set_num("agdc_filter_routed",1);
    rbgpu_diag_set_str("agdc_say","ARMED: waiting for boot framebuffer");
    if (work) thread_call_enter(work); // framebuffer may have arrived first
}

void rbgpu_agdc_framebuffer(IOService *fb) {
    if (!checkKernelArgument("-rbgpuagdc") || framebuffer) return;
    if (reinterpret_cast<uintptr_t>(fb)<0xffffff8000000000ULL ||
        !fb->getMetaClass() || !fb->getMetaClass()->getClassName() ||
        strcmp(fb->getMetaClass()->getClassName(),"IONDRVFramebuffer")) {
        refuse(5,"unexpected framebuffer"); return;
    }
    pci=OSDynamicCast(IOPCIDevice,fb->getProvider());
    if (!pci || pci->configRead16(kIOPCIConfigVendorID)!=0x1002 ||
        pci->configRead16(kIOPCIConfigDeviceID)!=0x1681) {
        refuse(6,"framebuffer PCI identity"); return;
    }
    // Called after enableController. The worker waits for AGDC construction
    // outside the framebuffer's gate; no wait/registration inside its hook.
    framebuffer=fb; framebuffer->retain(); pci->retain();
    work=thread_call_allocate(run,nullptr);
    if (!work) { refuse(7,"worker allocation"); return; }
    thread_call_enter(work);
    rbgpu_diag_set_num("agdc_fb_seen",1);
}
