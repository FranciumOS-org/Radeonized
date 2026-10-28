#include "fb_vbl_shim.hpp"
#include <cassert>
#include <cstdio>

static uint64_t clockNs;
void clock_interval_to_deadline(uint32_t n,unsigned scale,uint64_t *out) {
    assert(scale==1);*out=clockNs+n;
}
void thread_call_enter_delayed(thread_call_t c,uint64_t deadline) {
    c->queued=1;c->deadline=deadline;c->enters++;
}
void thread_call_cancel(thread_call_t c) {c->queued=0;c->cancels++;}
/* 2wk: the immediate kick. Returns whether a call was ALREADY pending, which
 * is what the real thread_call_enter reports, so the test can assert that a
 * second enable while one kick is in flight does not queue a second. */
bool thread_call_enter(thread_call_t c) {
    bool was=c->queued; c->queued=1; c->enters++; return was;
}
struct Counts { unsigned fb=0, pipe=0; };
static void fbCallback(OSObject *target,void *ref) {
    assert(target);static_cast<Counts *>(ref)->fb++;
}
static void pipeCallback(OSObject *,void *ref) {static_cast<Counts *>(ref)->pipe++;}
static void tick(RBAMDFramebuffer &fb,FakeCall &call) {
    assert(call.queued);clockNs=call.deadline;call.queued=0;
    RBAMDFramebuffer::vblFired(&fb,nullptr);
}
int main() {
    RBAMDFramebuffer fb;FakeCall timer;fb.vblCall=&timer;Counts counts;
    void *own=nullptr, *pipe=nullptr;OSObject pipeTarget;
    fb.publishVBLState();
    assert(fb.properties.at("RBFBVBLFires")==0);
    assert(fb.properties.at("RBFBVBLRegistered")==0);
    // IOFramebuffer::open performs ONLY registration. That must start its
    // clock, with no fabricated setInterruptState(ref,1) from the test.
    assert(fb.registerForInterruptType(kIOFBVBLInterruptType,fbCallback,&fb,&counts,&own)==0);
    assert(own && timer.queued && fb.vblRunning);
    assert(timer.deadline>6950000 && timer.deadline<6960000);
    tick(fb,timer);
    assert(counts.fb==1 && counts.pipe==0);
    assert(fb.properties.at("RBFBVBL0Fires")==1);
    assert(fb.properties.at("RBFBVBL0Self")==1);
    // Pipe registers separately, then immediately disables its own interrupt.
    assert(fb.registerForInterruptType(kIOFBVBLInterruptType,pipeCallback,&pipeTarget,&counts,&pipe)==0);
    assert(pipe!=own);
    assert(fb.setInterruptState(pipe,0)==0);
    assert(fb.properties.at("RBFBVBLEnabled")==1);
    assert(fb.properties.at("RBFBVBL1Enabled")==0);
    for(unsigned i=0;i<3;i++) tick(fb,timer);
    assert(counts.fb==4 && counts.pipe==0 && timer.queued);
    assert(fb.setInterruptState(pipe,1)==0);
    tick(fb,timer);
    assert(counts.fb==5 && counts.pipe==1);
    // Turning off the FB must leave an explicitly enabled pipe working too.
    assert(fb.setInterruptState(own,0)==0);
    tick(fb,timer);
    assert(counts.fb==5 && counts.pipe==2 && fb.vblRunning);
    assert(fb.setInterruptState(pipe,0)==0);
    assert(!timer.queued && !fb.vblRunning);
    // A callback already dispatched at disable time must not deliver an IRQ.
    fb.deliverVBL();
    assert(counts.fb==5 && counts.pipe==2 && !timer.queued);
    assert(fb.setInterruptState(own,1)==0 && timer.queued);
    tick(fb,timer);assert(counts.fb==6 && counts.pipe==2);
    assert(fb.unregisterInterrupt(own)==0 && !timer.queued);
    assert(fb.properties.at("RBFBVBL0Used")==0);
    assert(fb.setInterruptState(own,1)==kIOReturnBadArgument);
    assert(fb.unregisterInterrupt(own)==kIOReturnBadArgument);
    assert(fb.unregisterInterrupt(pipe)==0);
    assert(fb.properties.at("RBFBVBLRegistered")==0);
    // Validate error paths and independent tokens at full capacity.
    void *refs[4]={};
    for(auto &ref:refs)
        assert(fb.registerForInterruptType(kIOFBVBLInterruptType,pipeCallback,nullptr,&counts,&ref)==0);
    for(unsigned i=0;i<4;i++) for(unsigned j=i+1;j<4;j++) assert(refs[i]!=refs[j]);
    void *extra=reinterpret_cast<void *>(1);
    assert(fb.registerForInterruptType(kIOFBVBLInterruptType,pipeCallback,nullptr,&counts,&extra)==kIOReturnNoResources && extra==nullptr);
    assert(fb.registerForInterruptType(0,pipeCallback,nullptr,&counts,&extra)==kIOReturnUnsupported);
    assert(fb.registerForInterruptType(kIOFBVBLInterruptType,nullptr,nullptr,nullptr,&extra)==kIOReturnBadArgument);
    assert(fb.registerForInterruptType(kIOFBVBLInterruptType,pipeCallback,nullptr,&counts,nullptr)==kIOReturnBadArgument);
    // Missing timer is observable and never presented as scheduled work.
    RBAMDFramebuffer noTimer;
    assert(noTimer.registerForInterruptType(kIOFBVBLInterruptType,pipeCallback,nullptr,&counts,&extra)==0);
    assert(!noTimer.vblRunning && noTimer.properties.at("RBFBVBLRunning")==0);
    // 2t0: the breadcrumb budget. An NVRAM write is a flash commit, so the
    // count must NOT scale with the fire count. Drive past every milestone
    // and assert the writes stay at exactly the four defined ones.
    {
        RBAMDFramebuffer cr; FakeCall crTimer; cr.vblCall=&crTimer;
        void *ref=nullptr;
        assert(cr.registerForInterruptType(kIOFBVBLInterruptType,pipeCallback,nullptr,&counts,&ref)==0);
        for(unsigned i=0;i<120000;i++) cr.deliverVBL();
        assert(cr.vblFires==120000);
        assert(cr.crumbWrites==4);          // 100, 1000, 10000, 100000 - no more
        assert(cr.lastCrumb==13);           // and monotonic to the last one
        // A lower stage must never walk the furthest point back.
        cr.crumb(2); assert(cr.crumbStage==13);
    }
    /* 2wk: the immediate kick. Boot 195 (nonce) measured Apple enabling its
     * slot 11 times with 4 transactions while its handler fired 0 times in
     * 7200 periodic ticks, so the window is shorter than the 7 ms period.
     * These assert the three things that reading depends on. */
    {
        RBAMDFramebuffer k; FakeCall kTimer, kKick;
        k.vblCall=&kTimer; k.vblKickCall=&kKick; k.retireArmed=true;
        Counts kc; void *mine=nullptr, *theirs=nullptr;
        // Slot 0 is ours (target == &k); slot 1 is "Apple's" (a foreign target).
        assert(k.registerForInterruptType(kIOFBVBLInterruptType,fbCallback,&k,&kc,&mine)==0);
        assert(k.registerForInterruptType(kIOFBVBLInterruptType,pipeCallback,nullptr,&kc,&theirs)==0);

        // 1. Our OWN slot's enable must NOT kick - otherwise every re-enable
        //    of the handler we registered would deliver a spurious vblank.
        uint64_t kicks0=k.retireKicks;
        k.setInterruptState(mine,0); k.setInterruptState(mine,1);
        assert(k.retireKicks==kicks0);

        // 2. A foreign slot's off->on transition kicks exactly once, and the
        //    kick delivers while the slot is still enabled.
        k.setInterruptState(theirs,0);
        unsigned before=kc.pipe;
        k.setInterruptState(theirs,1);
        assert(k.retireKicks==kicks0+1);
        k.kickVBL();
        assert(k.retireDelivered==1 && kc.pipe==before+1);

        // 3. A window that closed before the kick ran costs nothing, and the
        //    shortfall is visible: kicks - delivered is how often we were
        //    late, which is the measurement this experiment owes.
        k.setInterruptState(theirs,0);
        k.setInterruptState(theirs,1);
        assert(k.retireKicks==kicks0+2);
        k.setInterruptState(theirs,0);      // Apple gives up before we run
        unsigned held=kc.pipe;
        k.kickVBL();
        assert(k.retireDelivered==1 && kc.pipe==held);
        assert(k.retireKicks-k.retireDelivered==1);

        // 4. The kick must NOT move vblFires: that counter is the project's
        //    143.83 Hz time reference and the crumb divides by it.
        uint64_t fires=k.vblFires;
        k.setInterruptState(theirs,1); k.kickVBL();
        assert(k.vblFires==fires);

        // 5. Disarmed, nothing is kicked at all.
        RBAMDFramebuffer d; FakeCall dT,dK;
        d.vblCall=&dT; d.vblKickCall=&dK; d.retireArmed=false;
        void *dref=nullptr;
        assert(d.registerForInterruptType(kIOFBVBLInterruptType,pipeCallback,nullptr,&kc,&dref)==0);
        d.setInterruptState(dref,0); d.setInterruptState(dref,1);
        assert(d.retireKicks==0);
    }
    puts("framebuffer VBL: registration starts clock; per-handler disable, re-arm, cancellation, telemetry, a bounded 4-write crumb budget and the 2wk immediate kick passed");
}
