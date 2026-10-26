// 2ra: per-GPU AGDC bridge for IOPresentment. OFF unless -rbgpuagdc AND
// rbgpu-agdc one-shot. No IOFramebuffer takeover and no DCN register writes.
// Runtime superclass/vtables avoid a system-KC static dependency (2qy).
#include "rbgpu_agdc.hpp"
#include "rbgpu_agdc_lookup.hpp"
#include "RembrandtGPUPeer.hpp"
#include "rb_agdc.h"
#include "rb_otg.h"
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
// 2rd: the boot framebuffer's own timing, for attribute 0x921. Set from
// processNDRV once IONDRVSupport is up; NULL until then, and the bridge
// refuses 0x921 rather than inventing a reply while it is NULL.
RbNdrvCurrentMode ndrvCurrentMode;
RbNdrvTimingInfo  ndrvTimingInfo;
// 2rh: the timing is sampled ONCE, on our own thread_call, before the service
// is started - never from the vendor callback. See sampleTiming().
uint8_t timingCache[RB_AGDC_TIMING_SIZE];
bool    timingValid;
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

// IOTimingInformation: appleTimingID, flags, then the detailed union. The
// V2 body therefore begins at +0x08 and is RB_AGDC_TIMING_SIZE bytes.
// kIODetailedTimingValid is quoted as a local constant rather than included,
// so this adds no import and the 2oz trap stays shut (2q4's rule).
constexpr uint32_t kRbDetailedTimingValid=0x80000000u;
constexpr size_t   kRbTimingDetailOff=0x08;
constexpr size_t   kRbTimingInfoSize=kRbTimingDetailOff+RB_AGDC_TIMING_SIZE;

// 2rh: SAMPLE the current mode's V2 timing into timingCache. Runs on our own
// thread_call, once, BEFORE service->start() publishes the bridge - never from
// the vendor callback.
//
// 2rh proposed a lock-order inversion here and 2ri MEASURED IT FALSE. Neither
// getter takes a lock: getCurrentDisplayMode is 34 bytes, two loads from
// this->0x1e8/0x1ec and two stores, with no call at all; and
// getTimingInfoForDisplayMode goes through _doStatus (0x15001610), which takes
// no lock, never blocks, and on the un-gated path merely logs Apple's own
// "%s::_doStatus(%d) not gated\n" via OSReportWithBacktrace and then performs
// the call anyway - EVERY path falls through to it. So this code could not
// have deadlocked, and the frozen panel was NOT caused by it: boot 156 froze
// the same way with agdc_started=1, before any of this existed.
//
// Sampling once off the callback is still right, and for a measured reason:
// the vendor callback runs on WindowServer's thread, which is not the
// IOGraphics gate, so calling from there earned an OSReportWithBacktrace on
// EVERY 0x921/0x711 query. Once, on our own thread, costs one.
//
// A failure here leaves timingValid false, and 0x921/0x711 then REFUSE rather
// than zero-fill (2rd: IOPresentment validates no timing field, so a zero
// reply would pass and poison its capability cache).
// 2rn: boot 160 showed the bridge publishing AFTER WindowServer had already
// asked IOPresentment for the AGDC, so "when" is now part of the evidence.
// mach_absolute_time() is ticks since boot, so this is uptime in ms and can be
// compared directly against the log's own timestamps.
static uint64_t rbgpuAgdcUptimeMs() {
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    return ns / 1000000ull;
}

RbOtgSample otgSample=nullptr;

// 2ro: OTG0 is a COMPLETE, independent timing source, so it must be reachable
// from EVERY way the NDRV can fail - not only from the one case boot 159 hit.
// The first cut had four early returns (agdc_tim_err 6, 7, 1, 2) that bypassed
// it entirely and put the fallback on the fifth path alone, the one where the
// NDRV answers and its flags lack kIODetailedTimingValid. That also blocked
// publishing before the framebuffer exists, which takes path 7.
//
// Returns the agdc_tim_err code; 0 means the cache is filled.
uint32_t sampleTimingNdrv() {
    if (!ndrvCurrentMode || !ndrvTimingInfo) return 6;  // getters unresolved
    if (!framebuffer)                        return 7;  // no framebuffer (yet)
    uint8_t scratch[kRbTimingInfoSize];
    int32_t mode=0, depth=0;
    if (ndrvCurrentMode(framebuffer,&mode,&depth)!=kIOReturnSuccess) return 1;
    rbgpu_diag_set_num("agdc_tim_mode",static_cast<uint64_t>(mode));
    for (size_t i=0;i<kRbTimingInfoSize;i++) scratch[i]=0;
    if (ndrvTimingInfo(framebuffer,mode,scratch)!=kIOReturnSuccess) return 2;
    uint32_t flags=0;
    for (unsigned i=0;i<4;i++) flags|=static_cast<uint32_t>(scratch[4+i])<<(8*i);
    rbgpu_diag_set_num("agdc_tim_flags",flags);
    // Rule 13/14: the struct is only meaningful when the driver says the
    // detailed timing is filled in. Refuse rather than ship whatever was there.
    if (!(flags & kRbDetailedTimingValid)) return 3;    // boot 159/160's case
    const uint8_t *v2=scratch+kRbTimingDetailOff;
    // Field offsets are Apple's own, named in rb_agdc.h from the CFNumber keys
    // IOPresentment publishes them under; all five are reported so a boot says
    // what timing was handed over and, on a refusal, exactly what was refused.
    const uint32_t hactive=rb_agdc_get32(v2+RB_AGDC_V2_HACTIVE);
    const uint32_t vactive=rb_agdc_get32(v2+RB_AGDC_V2_VACTIVE);
    const uint32_t hblank =rb_agdc_get32(v2+RB_AGDC_V2_HBLANKING);
    const uint32_t vblank =rb_agdc_get32(v2+RB_AGDC_V2_VBLANKING);
    const uint64_t clock  =rb_agdc_get64(v2+RB_AGDC_V2_PIXELCLOCK);
    rbgpu_diag_set_num("agdc_tim_h",hactive);
    rbgpu_diag_set_num("agdc_tim_v",vactive);
    rbgpu_diag_set_num("agdc_tim_hblank",hblank);
    rbgpu_diag_set_num("agdc_tim_vblank",vblank);
    rbgpu_diag_set_num("agdc_tim_clk",clock);
    if (!hactive || !vactive) return 4;
    // 2rg: __gatherCapabilities computes refresh = pixelClock / (hTotal*vTotal)
    // and publishes it as RefreshTime/Min/MaxRefreshTime. A zero clock passes
    // its > 0.0f guard and yields 0.0 Hz, so rb_agdc_link_config refuses it;
    // report the millihertz this boot's timing would produce either way, so the
    // capture says whether the number Apple will advertise is the panel's.
    const uint64_t total=static_cast<uint64_t>(hactive+hblank)*(vactive+vblank);
    rbgpu_diag_set_num("agdc_tim_mhz",total?(clock*1000ull)/total:0ull);
    if (!clock) return 5;
    for (size_t i=0;i<RB_AGDC_TIMING_SIZE;i++) timingCache[i]=v2[i];
    timingValid=true;
    rbgpu_diag_set_num("agdc_tim_cached",1);   // 1 = NDRV, 2 = OTG0
    return 0;
}

// OTG0's live registers, read-only. True only if the cache is filled.
bool sampleTimingOtg() {
    if (!otgSample) { rbgpu_diag_set_num("agdc_otg_src",0); return false; }
    struct rb_otg_timing otg;
    for (size_t i=0;i<sizeof otg;i++) reinterpret_cast<uint8_t *>(&otg)[i]=0;
    if (!otgSample(&otg)) { rbgpu_diag_set_num("agdc_otg_src",1); return false; }
    rbgpu_diag_set_num("agdc_otg_src",2);
    // Publish the raw registers before converting: a refusal below must say
    // what it refused (rule 19), and these words are the whole input, so a
    // capture of them is a capture of the hardware.
    rbgpu_diag_set_num("agdc_otg_htot",otg.h_total);
    rbgpu_diag_set_num("agdc_otg_hbse",otg.h_blank_start_end);
    rbgpu_diag_set_num("agdc_otg_hsa", otg.h_sync_a);
    rbgpu_diag_set_num("agdc_otg_vtot",otg.v_total);
    rbgpu_diag_set_num("agdc_otg_vbse",otg.v_blank_start_end);
    rbgpu_diag_set_num("agdc_otg_vsa", otg.v_sync_a);
    rbgpu_diag_set_num("agdc_otg_mhz", otg.refresh_mhz);
    uint8_t otg_v2[RB_V2_SIZE];
    int orc=rb_otg_to_v2(&otg,otg_v2,sizeof otg_v2);
    rbgpu_diag_set_num("agdc_otg_ret",static_cast<uint64_t>(-orc));
    if (orc!=RB_OTG_OK) return false;      // refuses, never zero-fills
    for (size_t i=0;i<RB_AGDC_TIMING_SIZE;i++) timingCache[i]=otg_v2[i];
    timingValid=true;
    rbgpu_diag_set_num("agdc_tim_h",rb_agdc_get32(otg_v2+RB_AGDC_V2_HACTIVE));
    rbgpu_diag_set_num("agdc_tim_v",rb_agdc_get32(otg_v2+RB_AGDC_V2_VACTIVE));
    rbgpu_diag_set_num("agdc_tim_clk",rb_agdc_get64(otg_v2+RB_AGDC_V2_PIXELCLOCK));
    rbgpu_diag_set_num("agdc_tim_mhz",otg.refresh_mhz);
    rbgpu_diag_set_num("agdc_tim_cached",2);   // 2 = from OTG0, 1 = NDRV
    return true;
}

void sampleTiming() {
    // Rule 14: every exit publishes. agdc_tim_err was seeded to 9 before any of
    // this, so the key exists even if we are never called at all.
    // The NDRV is tried first because it is the display's own advertised
    // timing; OTG0 then gets a turn after ANY of its failures, and
    // agdc_tim_ndrv_err keeps the NDRV's own verdict visible either way so a
    // boot still says why the first source was not used.
    const uint32_t ndrv=sampleTimingNdrv();
    rbgpu_diag_set_num("agdc_tim_ndrv_err",ndrv);
    if (ndrv==0) { rbgpu_diag_set_num("agdc_tim_err",0); return; }
    rbgpu_diag_set_num("agdc_tim_err",ndrv);
    if (sampleTimingOtg()) rbgpu_diag_set_num("agdc_tim_err",0);
}

IOReturn vendor(void *,unsigned attr,unsigned long *,unsigned long inSize,
                unsigned long *out,unsigned long *outSize,void *) {
    rbgpu_diag_set_num("agdc_vendor_attr",attr);
    rbgpu_diag_set_num("agdc_vendor_in",inSize);
    rbgpu_diag_set_num("agdc_vendor_out",outSize ? *outSize : 0);
    if (attr==1) rbgpu_diag_inc("agdc_vendor_calls");
    if (attr==0x980) rbgpu_diag_inc("agdc_cap_calls");
    if (attr==0x921) rbgpu_diag_inc("agdc_link_calls");
    if (attr==0x711) {
        rbgpu_diag_inc("agdc_extcap_calls");
        // 2rf: IOPresentment passes ONE buffer as both input and output, so
        // the type it asked for should be readable here, exactly as Apple's
        // own handler reads it. Publish it: if this is always 0 the kernel
        // handed us a separate zeroed output copy and every type would be
        // refused for that reason and not because it is unimplemented.
        // 2ri, rule 19: a NULL check is not a pointer check. The type lives at
        // +0x04, so reading it needs EIGHT bytes of output buffer, and until
        // now this read only tested `out` - an out-of-bounds kernel read for
        // any caller passing a shorter one.
        if (out && outSize && *outSize >= 8)
            rbgpu_diag_set_num("agdc_extcap_type",
                               rb_agdc_get32(
                                   reinterpret_cast<const uint8_t *>(out)+4));
        else
            rbgpu_diag_set_num("agdc_extcap_type_short",outSize ? *outSize : 0);
    }
    // 2rh: the CACHE, never a call. This runs on WindowServer's thread inside
    // a user-client external method; calling into IOFramebuffer from here is
    // what is believed to have wedged boot 157 three times.
    const void *v2=((attr==0x921 || attr==0x711) && timingValid)
                   ? static_cast<const void *>(timingCache) : nullptr;
    // KERNEL pointers, not registry IDs. Apple's filtered 0x980 handler
    // still converts these before copying anything to userspace.
    const int result=rb_agdc_reply(attr,inSize,out,outSize,
        reinterpret_cast<uint64_t>(pci),reinterpret_cast<uint64_t>(framebuffer),v2);
    if (attr==0x921) rbgpu_diag_set_num("agdc_link_result",
                                        static_cast<uint64_t>(result));
    if (attr==0x711) {
        rbgpu_diag_set_num("agdc_extcap_result",static_cast<uint64_t>(result));
        if (result==0) rbgpu_diag_inc("agdc_extcap_answered");
    }
    if (result==RB_AGDC_UNSUPPORTED) {
        rbgpu_diag_set_num("agdc_vendor_unsupported",attr);
        return kIOReturnUnsupported;
    }
    return result==0 ? kIOReturnSuccess : kIOReturnBadArgument;
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
    const bool mine=self==service && service;
    const auto capacity=mine && outSize ? *outSize : 0;
    const IOReturn result=mine && attr==1 && (api==0 || api==2 || api==3)
        ? vendor(self,attr,static_cast<unsigned long *>(in),inSize,
                      static_cast<unsigned long *>(out),outSize,nullptr)
        : originalFilter(self,api,attr,in,inSize,out,outSize,state);
    if (mine) {
        // Record AFTER Apple's filter as well: a request can fail there
        // without ever reaching our vendor callback. No buffers/pointers logged.
        rbgpu_diag_set_num("agdc_last_ret",static_cast<uint32_t>(result));
        if (attr==0x921) {
            rbgpu_diag_inc("agdc_logical_calls");
            rbgpu_diag_set_num("agdc_logical_in",inSize);
            rbgpu_diag_set_num("agdc_logical_out",capacity);
            rbgpu_diag_set_num("agdc_logical_ret",static_cast<uint32_t>(result));
        }
        if (result!=kIOReturnSuccess) {
            rbgpu_diag_inc("agdc_failed_calls");
            rbgpu_diag_set_num("agdc_failed_attr",attr);
            rbgpu_diag_set_num("agdc_failed_ret",static_cast<uint32_t>(result));
        }
    }
    return result;
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
    if (finished || !pci) return;   // 2ro: pci is the requirement, not the fb
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
    // 2rk: AppleGraphicsDevicePolicy's personality is
    // IOProviderClass = AppleGraphicsDeviceControl, so it probes EVERY AGDC -
    // ours included - and its probe (0x13e0989a) matches with score 1000
    // unless the provider carries NeedsPolicyDriver as an OSBoolean whose
    // getValue() is FALSE. An ABSENT property therefore RECRUITS the policy
    // driver. Apple's own nested AGDC sets it false for exactly this reason
    // (0x13e0e800: setProperty(name,false) between attach() and start()).
    // We are a single-GPU APU with no gmux, so we opt out by default;
    // -rbgpuagdcpolicy leaves the property absent for an A/B test.
    const bool allowPolicy=checkKernelArgument("-rbgpuagdcpolicy");
    if (!allowPolicy && !service->setProperty("NeedsPolicyDriver",false)) {
        auto failed=service; service=nullptr; failed->release();
        refuse(16,"NeedsPolicyDriver publish failed"); return;
    }
    rbgpu_diag_set_num("agdc_policy_optout",allowPolicy ? 0u : 1u);
    if (!service->setProperty("IOUserClientClass","AppleGraphicsDeviceControlClient") ||
        !service->setProperty("RBGPUAGDCBridge",true) || !service->attach(pci)) {
        auto failed=service; service=nullptr; failed->release();
        refuse(14,"properties/attach failed"); return;
    }
    rbgpu_diag_set_num("agdc_pci_id",pci->getRegistryEntryID());
    if (framebuffer)
        rbgpu_diag_set_num("agdc_fb_id",framebuffer->getRegistryEntryID());
    // 2rh: sample the timing HERE - on this thread_call, with nothing of ours
    // registered yet and no Apple lock held by us - so the vendor callback
    // never has to call into IOFramebuffer. Must precede start(), because
    // AGDC::start registers the service and WindowServer can query at once.
    rbgpu_diag_set_num("agdc_t_presample_ms",rbgpuAgdcUptimeMs());
    sampleTiming();
    rbgpu_diag_set_num("agdc_t_sampled_ms",rbgpuAgdcUptimeMs());
    rbgpu_diag_set_num("agdc_start_enter",1);
    rbgpu_diag_set_num("agdc_stage",60);
    bool started=service->start(pci); // AGDC::start registers the service itself
    rbgpu_diag_set_num("agdc_t_started_ms",rbgpuAgdcUptimeMs());
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
    // 2rh, rule 14: seed the verdict so an ABSENT agdc_tim_err is impossible.
    // 9 means sampleTiming() was never reached at all, which is exactly what
    // boot 157 could not distinguish from a full diag table.
    if (checkKernelArgument("-rbgpuagdc")) rbgpu_diag_set_num("agdc_tim_err",9);
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

void rbgpu_agdc_otg_source(RbOtgSample fn) {
    if (!checkKernelArgument("-rbgpuagdc")) return;
    otgSample=fn;
    rbgpu_diag_set_num("agdc_otg_bound",fn?1u:0u);
}

void rbgpu_agdc_timing_source(RbNdrvCurrentMode cur,RbNdrvTimingInfo info) {
    if (!checkKernelArgument("-rbgpuagdc")) return;
    ndrvCurrentMode=cur; ndrvTimingInfo=info;
    rbgpu_diag_set_num("agdc_tim_src",(cur?1u:0u)|(info?2u:0u)); // 3 required
}

// 2ro: the ONE thing the bridge needed the framebuffer for was its provider,
// and the peer already has it. Allocating the thread_call here - and entering
// it - is what used to chain the whole publish behind enableController.
void rbgpu_agdc_pci(IOService *pciDevice) {
    if (!checkKernelArgument("-rbgpuagdc") || pci) return;
    auto dev=OSDynamicCast(IOPCIDevice,pciDevice);
    if (!dev || dev->configRead16(kIOPCIConfigVendorID)!=0x1002 ||
        dev->configRead16(kIOPCIConfigDeviceID)!=0x1681) {
        refuse(6,"PCI identity"); return;
    }
    pci=dev; pci->retain();
    rbgpu_diag_set_num("agdc_pci_seen",1);
    if (!work) {
        work=thread_call_allocate(run,nullptr);
        if (!work) { refuse(7,"worker allocation"); return; }
    }
    thread_call_enter(work);
}

// Still called from the enableController hook, but no longer the trigger: it
// only supplies the framebuffer, which is now optional. If the publish has
// already happened the timing came from OTG0 and this changes nothing; if it
// has not, run() gets another chance with the NDRV source available too.
void rbgpu_agdc_framebuffer(IOService *fb) {
    if (!checkKernelArgument("-rbgpuagdc") || framebuffer) return;
    if (reinterpret_cast<uintptr_t>(fb)<0xffffff8000000000ULL ||
        !fb->getMetaClass() || !fb->getMetaClass()->getClassName() ||
        strcmp(fb->getMetaClass()->getClassName(),"IONDRVFramebuffer")) {
        refuse(5,"unexpected framebuffer"); return;
    }
    framebuffer=fb; framebuffer->retain();
    rbgpu_diag_set_num("agdc_fb_seen",1);
    // The peer normally gets here first; this covers the case where it did not
    // (and keeps the old ordering working) without being the only path.
    if (!pci) rbgpu_agdc_pci(fb->getProvider());
    else if (work && !finished) thread_call_enter(work);
}
