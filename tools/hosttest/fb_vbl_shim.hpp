// Host substitutes for IOKit scheduling; production VBL methods are unchanged.
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

using UInt32=uint32_t;
using UInt64=uint64_t;
using IOSelect=uint32_t;
using IOReturn=int32_t;
using thread_call_param_t=void *;
constexpr IOSelect kIOFBVBLInterruptType=0x76626c20;
constexpr IOReturn kIOReturnSuccess=0, kIOReturnUnsupported=-1;
constexpr IOReturn kIOReturnNoResources=-2, kIOReturnBadArgument=-3;
struct OSObject {};
using IOFBInterruptProc=void (*)(OSObject *,void *);
struct FakeCall { unsigned queued=0, enters=0, cancels=0; uint64_t deadline=0; };
using thread_call_t=FakeCall *;
void clock_interval_to_deadline(uint32_t,unsigned,uint64_t *);
void thread_call_enter_delayed(thread_call_t,uint64_t);
void thread_call_cancel(thread_call_t);
bool thread_call_enter(thread_call_t);
#define LOG(...) ((void)0)

// Only the non-virtual VBL methods are linked into the host executable. These
// fields have the same types as the real class; no kernel layout is simulated.
class RBAMDFramebuffer : public OSObject {
public:
    struct Interrupt {
        IOSelect type;
        IOFBInterruptProc proc;
        OSObject *target;
        void *ref;
        bool enabled;
        bool used;
        uint64_t fires;
    };
    static const unsigned kMaxInterrupts=4;
    static constexpr uint32_t kRefreshRate1616=0x008fd53f;
    static constexpr uint64_t kVBLPeriodNs=1000000000ULL*65536ULL/kRefreshRate1616;
    Interrupt interrupts[kMaxInterrupts]{};
    thread_call_t vblCall=nullptr;
    thread_call_t vblKickCall=nullptr;
    bool vblRunning=false;
    uint64_t vblFires=0;
    uint32_t vblEnables=0;
    // 2wc: the 'amtr' counters publishVBLState also reports.
    static const unsigned kAmtrSnapBytes=0x80;
    bool amtrArmed=false;
    bool amtrSnapped=false;
    uint64_t amtrGets=0;
    uint64_t amtrSets=0;
    uint64_t amtrPeak=0;
    uint64_t amtrBucket=0;
    uint64_t amtrNoFlip=0;
    uint64_t amtrPtr=0;
    uint64_t amtrRefused=0;
    uint8_t amtrSnap[kAmtrSnapBytes]{};
    static const unsigned kAmtrFlipBytes=0x220;
    bool amtrFlipSnapped=false;
    uint64_t amtrFlipPtr=0;
    uint64_t amtrFlipRefused=0;
    uint8_t amtrFlipSnap[kAmtrFlipBytes]{};
    bool retireArmed=false;
    uint64_t retireEnables=0;
    uint64_t retireKicks=0;
    uint64_t retireDelivered=0;
    bool hubpArmed=false;
    bool hubpReady=false;
    uint32_t hubpRefuse=0;
    uint32_t hubpOtgCtl=0;
    uint32_t hubpAddrCfg=0;
    uint32_t hubpTiling=0;
    uint32_t hubpPitch=0;
    uint32_t hubpConfig=0;
    uint64_t hubpOrig=0;
    uint64_t hubpWrites=0;
    uint64_t hubpLast=0;
    uint64_t hubpReadback=0;
    uint64_t hubpRejects=0;
    uint64_t hubpRejectAddr=0;
    uint32_t hubpRejectWhy=0;
    // 2t0: the host substitute counts NVRAM writes instead of performing
    // them, which is what makes "bounded by construction" an assertion
    // rather than a claim in a comment.
    uint32_t crumbStage=0;
    unsigned crumbWrites=0;
    uint32_t lastCrumb=0;
    void crumb(uint32_t stage){
        if(stage>crumbStage) crumbStage=stage;
        lastCrumb=crumbStage; crumbWrites++;
    }
    std::map<std::string,uint64_t> properties;
    bool setProperty(const char *key,bool v) { properties[key]=v;return true; }
    bool setProperty(const char *key,uint64_t v,unsigned) { properties[key]=v;return true; }
    std::map<std::string,std::vector<uint8_t>> blobs;
    bool setProperty(const char *key,const void *b,unsigned n) {
        const uint8_t *p=(const uint8_t *)b; blobs[key].assign(p,p+n); return true; }
    IOReturn registerForInterruptType(IOSelect,IOFBInterruptProc,OSObject *,void *,void **);
    IOReturn unregisterInterrupt(void *);
    IOReturn setInterruptState(void *,UInt32);
    void armVBL();
    void kickVBL();
    void deliverVBL();
    void publishVBLState();
    bool anyVBLEnabled();
    static void vblFired(thread_call_param_t,thread_call_param_t);
    static void vblKicked(thread_call_param_t,thread_call_param_t);
};
