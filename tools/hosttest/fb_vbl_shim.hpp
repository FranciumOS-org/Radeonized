// Host substitutes for IOKit scheduling; production VBL methods are unchanged.
#pragma once
#include <cstdint>
#include <map>
#include <string>

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
    bool vblRunning=false;
    uint64_t vblFires=0;
    uint32_t vblEnables=0;
    std::map<std::string,uint64_t> properties;
    bool setProperty(const char *key,bool v) { properties[key]=v;return true; }
    bool setProperty(const char *key,uint64_t v,unsigned) { properties[key]=v;return true; }
    IOReturn registerForInterruptType(IOSelect,IOFBInterruptProc,OSObject *,void *,void **);
    IOReturn unregisterInterrupt(void *);
    IOReturn setInterruptState(void *,UInt32);
    void armVBL();
    void deliverVBL();
    void publishVBLState();
    bool anyVBLEnabled();
    static void vblFired(thread_call_param_t,thread_call_param_t);
};
