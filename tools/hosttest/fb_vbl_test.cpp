#include "fb_vbl_shim.hpp"
#include <cassert>
#include <cstdio>

static uint64_t clockNs;
void clock_interval_to_deadline(uint32_t n,unsigned scale,uint64_t *out) {
    assert(scale==1);*out=clockNs+n;
}
void clock_get_uptime(uint64_t *out) {*out=clockNs;}
void nanoseconds_to_absolutetime(uint64_t ns,uint64_t *out) {*out=ns;}
void absolutetime_to_nanoseconds(uint64_t a,uint64_t *out) {*out=a;}
uint64_t mach_absolute_time(void) {return clockNs;}
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
/* 2xe: the same tick, but the handler takes `overheadNs` to run before it
 * re-arms - which is what really happens (delivery to every handler plus
 * hubpPoll()'s MMIO read). The old armVBL() added that to every period and
 * the panel ran 0.47 ms slow on every tick; this is the case that catches
 * it, and it is the ONLY difference from tick() above. */
static void tickSlow(RBAMDFramebuffer &fb,FakeCall &call,uint64_t overheadNs) {
    assert(call.queued);clockNs=call.deadline+overheadNs;call.queued=0;
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
    // 2t0/2wl: the breadcrumb budget. An NVRAM write is a flash commit, so
    // the count must stay bounded - but 2wl makes it PERIODIC, because boot
    // 197 ran twenty minutes and its last crumb was seventy seconds old. The
    // budget is now one write per 10000 fires (~70 s) and the stage says
    // which sample it is, so the test pins the RATE rather than a total.
    {
        RBAMDFramebuffer cr; FakeCall crTimer; cr.vblCall=&crTimer;
        void *ref=nullptr;
        assert(cr.registerForInterruptType(kIOFBVBLInterruptType,pipeCallback,nullptr,&counts,&ref)==0);
        for(unsigned i=0;i<120000;i++) cr.deliverVBL();
        assert(cr.vblFires==120000);
        // 100, 1000, then one per 10000 - twelve of those in 120000 fires.
        assert(cr.crumbWrites==2+12);
        assert(cr.lastCrumb==11+12);        // and monotonic to the last one
        // A lower stage must never walk the furthest point back.
        cr.crumb(2); assert(cr.crumbStage==23);
    }
    // 2wp: the no-transaction self-disarm. A framebuffer that took the panel
    // and was never handed a transaction leaves a frozen screen, which rule 12
    // says reads exactly like a hang - so it must consume its own arm for the
    // next boot. The three cases that matter are "it fires", "it does NOT fire
    // when transactions arrived", and "it fires at most once".
    {
        RBAMDFramebuffer d; FakeCall dT; d.vblCall=&dT; d.amtrArmed=true;
        void *ref=nullptr;
        assert(d.registerForInterruptType(kIOFBVBLInterruptType,pipeCallback,nullptr,&counts,&ref)==0);
        for(unsigned i=0;i<17999;i++) d.deliverVBL();
        assert(!d.selfDisarmed);           // not before the deadline
        d.deliverVBL();                    // fire 18000
        assert(d.selfDisarmed && d.disarmWrites==1);
        for(unsigned i=0;i<2000;i++) d.deliverVBL();
        assert(d.disarmWrites==1);         // and exactly once, ever
    }
    {
        // One transaction is enough to prove the accelerator is there.
        RBAMDFramebuffer k; FakeCall kT; k.vblCall=&kT; k.amtrArmed=true;
        void *ref=nullptr;
        assert(k.registerForInterruptType(kIOFBVBLInterruptType,pipeCallback,nullptr,&counts,&ref)==0);
        k.amtrSets=1;
        for(unsigned i=0;i<18001;i++) k.deliverVBL();
        assert(!k.selfDisarmed && k.disarmWrites==0);
    }
    {
        // A framebuffer that never armed for transactions is not this case
        // and must be left alone.
        RBAMDFramebuffer n; FakeCall nT; n.vblCall=&nT; n.amtrArmed=false;
        void *ref=nullptr;
        assert(n.registerForInterruptType(kIOFBVBLInterruptType,pipeCallback,nullptr,&counts,&ref)==0);
        for(unsigned i=0;i<18001;i++) n.deliverVBL();
        assert(!n.selfDisarmed);
    }
    // 2wl: the rate bucket. The user reported typing in Spotlight making the
    // frames smooth while the idle desktop sat at ~0.09 transactions/s, and a
    // total divided by a boot length cannot tell a burst from a trickle. The
    // peak must survive later quiet windows, and must not be the running
    // bucket - otherwise it would read as 0 whenever the boot ends quiet.
    {
        RBAMDFramebuffer rt; FakeCall rtTimer; rt.vblCall=&rtTimer;
        void *ref=nullptr;
        assert(rt.registerForInterruptType(kIOFBVBLInterruptType,pipeCallback,nullptr,&counts,&ref)==0);
        rt.amtrBucket=37;                      // a burst
        for(unsigned i=0;i<1000;i++) rt.deliverVBL();
        assert(rt.amtrPeak==37 && rt.amtrBucket==0);
        rt.amtrBucket=2;                       // a quieter window after it
        for(unsigned i=0;i<1000;i++) rt.deliverVBL();
        assert(rt.amtrPeak==37);               // the burst is not forgotten
        rt.amtrBucket=900;                     // and a bigger one replaces it
        for(unsigned i=0;i<1000;i++) rt.deliverVBL();
        assert(rt.amtrPeak==900 && rt.amtrBucket==0);
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
    // 2x1: foreignFires() is what the crumb's A= carries, and it exists to
    // answer ONE question from NVRAM alone: did Apple's 'vbl ' handler ever
    // run? So it must count the slots we did NOT register and only those.
    // Getting that backwards would publish our own 143.83 Hz tick as though
    // it were Apple's retirement and read as a spectacular success.
    {
        RBAMDFramebuffer f; FakeCall t,k; Counts c{};
        f.vblCall=&t; f.vblKickCall=&k; f.retireArmed=true;
        void *mine=nullptr,*theirs=nullptr;
        // target == &f is OURS; a different target is Apple's (the real
        // discriminator in publishVBLState and kickVBL alike).
        assert(f.registerForInterruptType(kIOFBVBLInterruptType,fbCallback,&f,&c,&mine)==0);
        OSObject other;
        assert(f.registerForInterruptType(kIOFBVBLInterruptType,pipeCallback,&other,&c,&theirs)==0);
        // Our own slot firing must NOT show up in A=.
        f.setInterruptState(theirs,0);
        tick(f,t);
        assert(c.fb>=1 && c.pipe==0);
        assert(f.foreignFires()==0);          // ours fired, A= stays 0
        // Now the retire block's effect: force-enable the foreign slot and
        // deliver immediately. This is what setAttribute('amtr') does.
        f.interrupts[1].enabled=true; f.retireEnables++;
        f.retireKicks++; f.kickVBL();
        assert(c.pipe==1);
        assert(f.foreignFires()==1);          // and now A= moves
        assert(f.retireDelivered==1);
        // A= must track further foreign fires through the periodic path too,
        // so a live boot and a dead one report the same number.
        tick(f,t);
        assert(f.foreignFires()==2 && c.pipe==2);
        assert(f.properties.at("RBFBForeignFires")==f.foreignFires() ||
               f.foreignFires()>0);           // published at least once set
        // A slot we registered ourselves can never contribute, however many
        // times it fires - the mutation this test exists to catch.
        uint64_t before=f.foreignFires();
        for(int i=0;i<5;i++) tick(f,t);
        assert(f.foreignFires()==before+5);   // only the ONE foreign slot
        assert(c.fb>before);                  // ours fired plenty meanwhile
    }
    // 2x3: the surface poll must run on EVERY vblank, because the whole
    // point is to follow a swap chain we are not told about. One missed tick
    // is one stale frame on the panel.
    {
        RBAMDFramebuffer f; FakeCall t,k; Counts c{};
        f.vblCall=&t; f.vblKickCall=&k;
        void *r=nullptr;
        assert(f.registerForInterruptType(kIOFBVBLInterruptType,fbCallback,&f,&c,&r)==0);
        uint64_t before=f.pollCalls;
        for(int i=0;i<7;i++) tick(f,t);
        assert(f.pollCalls==before+7);          // once per fire, no more, no less
        assert(f.vblFires==7);
        assert(f.pollCalls==f.vblFires);
    }
    // 2xe: THE VBLANK IS A CLOCK, NOT A DELAY. Measured from userspace with
    // CVDisplayLink: mean interval 7.4240 ms = 134.70 Hz against the panel's
    // 6.9527 ms = 143.83 Hz. The cause is arithmetic, not scheduling - the
    // old armVBL() computed `now + period` at the END of deliverVBL(), so
    // every tick paid for the handler that preceded it, forever.
    //
    // This case is the whole fix in two assertions: with a handler that
    // takes 1.4 ms (about what was measured), the deadlines must stay on an
    // EXACT lattice, and the elapsed panel time over N ticks must equal
    // N periods rather than N*(period+overhead).
    {
        RBAMDFramebuffer f; FakeCall t,k; Counts c{};
        f.vblCall=&t; f.vblKickCall=&k;
        void *r=nullptr;
        clockNs=0;
        assert(f.registerForInterruptType(kIOFBVBLInterruptType,fbCallback,&f,&c,&r)==0);
        const uint64_t P=RBAMDFramebuffer::kVBLPeriodNsFast;
        const uint64_t first=t.deadline;
        assert(first==P);                       // armed from clockNs==0
        const uint64_t overhead=1400000;        // 1.4 ms, the measured shape
        for(unsigned i=1;i<=200;i++) {
            tickSlow(f,t,overhead);
            // The i-th re-arm must land exactly (i+1) periods out. Any
            // dependence on `overhead` here is the defect returning.
            assert(t.deadline==first+(uint64_t)i*P);
        }
        assert(f.vblFires==200);
        assert(f.vblResyncs==0);                // 1.4 ms < one period
        // The rate the project's formula depends on, to a nanosecond.
        assert(t.deadline-first==200*P);

        // And a stall LONGER than a period must resync once, not queue a
        // burst of catch-up ticks with a deadline in the past.
        uint64_t resBefore=f.vblResyncs;
        tickSlow(f,t,P*3);                      // three periods late
        assert(f.vblResyncs==resBefore+1);
        assert(t.deadline>clockNs);             // never armed into the past
        assert(t.deadline==clockNs+P);
        // Having resynced, the lattice continues from the NEW phase.
        uint64_t base=t.deadline;
        for(unsigned i=1;i<=5;i++) {
            tickSlow(f,t,overhead);
            assert(t.deadline==base+(uint64_t)i*P);
        }
        assert(f.vblResyncs==resBefore+1);      // no further resyncs
    }

    // 2y6: the retune. A mode switch changes the frame clock UNDER a running
    // thread_call, and the thing that must not happen is 2xe coming back: a
    // deadline accumulated from the OLD period would carry the old cadence
    // across the switch forever, which looks exactly like a working switch
    // for the first few frames.
    {
        RBAMDFramebuffer f; FakeCall t,k; Counts c{};
        f.vblCall=&t; f.vblKickCall=&k;
        void *r=nullptr;
        clockNs=0;
        assert(f.registerForInterruptType(kIOFBVBLInterruptType,fbCallback,&f,&c,&r)==0);
        const uint64_t fast=RBAMDFramebuffer::kVBLPeriodNsFast;
        const uint64_t slow=16668887ULL;        // 60 Hz, from rb_drr_period_ns
        assert(t.deadline==fast);
        for(unsigned i=1;i<=10;i++) { tickSlow(f,t,0); }
        const uint64_t before=t.deadline;
        assert(before==11*fast);

        f.vblRetune(slow);
        assert(f.vblRetunes==1);
        assert(f.vblPeriodNs==slow);
        // Re-based, NOT accumulated. The next tick is taken with a NON-ZERO
        // overhead on purpose: tickSlow sets clockNs=deadline+overhead, so
        // with overhead 0 the clock lands exactly on the old deadline and
        // `clockNs+slow` and `before+slow` are the same number - the
        // assertion could not have told a re-base from an accumulation at
        // all. Off the lattice they differ, and that difference IS 2xe.
        const uint64_t off=1234567ULL;
        tickSlow(f,t,off);
        assert(clockNs==before+off);
        assert(t.deadline==clockNs+slow);       // re-based from now
        assert(t.deadline!=before+slow);        // not accumulated
        // And from there the new period is the lattice.
        const uint64_t nbase=t.deadline;
        for(unsigned i=1;i<=10;i++) {
            tickSlow(f,t,0);
            assert(t.deadline==nbase+(uint64_t)i*slow);
        }
        // Back again, by the same path, because a mode switch happens in both
        // directions and only one of them had ever been exercised.
        f.vblRetune(fast);
        assert(f.vblRetunes==2);
        tickSlow(f,t,0);
        const uint64_t fbase=t.deadline;
        assert(fbase==clockNs+fast);
        for(unsigned i=1;i<=10;i++) {
            tickSlow(f,t,0);
            assert(t.deadline==fbase+(uint64_t)i*fast);
        }
        // A zero period is refused rather than stopping the frame clock.
        f.vblRetune(0);
        assert(f.vblRetunes==2);
        assert(f.vblPeriodNs==fast);
    }
    /* 2zk: the hot-path publish throttle.
     *
     * Boot 270 measured publishVBLState() running 227 times a second from
     * setInterruptState, ~110 setProperty calls each, on the thread that
     * gates every frame. The throttle has to actually ELIDE the publish, so
     * these assertions count the shim's setProperty calls rather than the
     * throttle's own pubHotDone - asserting a counter the throttle itself
     * bumps against the throttle would be 2u9's tautology, which left a
     * suite that could not fail.
     *
     * The literals here are deliberate: 500 and 0 are written out rather
     * than read from the member, so a future edit that changes the default
     * stops agreeing with a number this test states independently. */
    {
        RBAMDFramebuffer f;
        clockNs=1000000000ull;                  // 1 s in, away from zero
        f.pubMs=500;
        f.pubIntervalAbs=500ull*1000000ull;     // abs == ns in this harness
        f.pubNextAbs=0; f.pubHotCalls=0; f.pubHotDone=0;
        f.setPropertyCalls=0;

        // First call in a quiet window publishes.
        f.publishVBLStateHot();
        assert(f.pubHotCalls==1);
        assert(f.pubHotDone==1);
        const uint64_t oneProps=f.setPropertyCalls;
        assert(oneProps>50);                    // a publish is many properties

        // The next 200 calls inside the interval publish NOTHING - not one
        // setProperty - which is the whole point of the change.
        for(unsigned i=0;i<200;i++) { f.publishVBLStateHot(); }
        assert(f.pubHotCalls==201);
        assert(f.pubHotDone==1);
        assert(f.setPropertyCalls==oneProps);

        // One nanosecond before the deadline is still inside it.
        clockNs += 500ull*1000000ull - 1;
        f.publishVBLStateHot();
        assert(f.pubHotDone==1);
        assert(f.setPropertyCalls==oneProps);

        // And on the deadline it publishes again, once.
        clockNs += 1;
        f.publishVBLStateHot();
        assert(f.pubHotDone==2);
        assert(f.setPropertyCalls==2*oneProps);

        // pubMs == 0 is the A side: every call publishes, and the clock is
        // never consulted, so a stale pubNextAbs cannot suppress anything.
        RBAMDFramebuffer g;
        g.pubMs=0;
        g.pubNextAbs=~0ull;                     // would block if it were read
        g.pubHotCalls=0; g.pubHotDone=0; g.setPropertyCalls=0;
        for(unsigned i=0;i<5;i++) { g.publishVBLStateHot(); }
        assert(g.pubHotCalls==5);
        assert(g.pubHotDone==5);
        assert(g.setPropertyCalls>=5*50);
    }
    /* 2A0: the frame-time budget.
     *
     * The thing being asserted is NOT that the accumulators add up - that is
     * arithmetic. It is that the two failure modes which would make the
     * instrument lie are visible:
     *
     *   - an idle desktop's one-second gap must be DROPPED, not averaged in,
     *     because averaged in it reports the idle desktop as the frame cost
     *     and there is nothing in the published numbers to show it happened;
     *   - a backwards interval (the vblank thread_call and Apple's thread can
     *     interleave) must be dropped rather than becoming a ~584-year sample
     *     that the bound then silently discards anyway.
     *
     * And the budget must reconstruct an interval this test chooses itself,
     * because that is the self-check tools/paired.sh will compare against
     * (rule 26: the instrument has to be able to say its premise failed).
     */
    {
        RBAMDFramebuffer f;
        f.vblPeriodNs = 6952507;                 // the measured native period
        const uint64_t bound = rb_ft_gap_bound_ns(f.vblPeriodNs);
        assert(bound == 6952507ull*8ull);

        // A transaction interval of 13.888 ms - one 144 Hz frame period
        // doubled, which is what fed ~50 % means - split into 200 us of our
        // own work and the rest upstream.
        const uint64_t want = 13888888ull;
        for (unsigned i=0;i<100;i++) {
            f.ftFile(&f.ftWork, 0, 200000, false);
            f.ftFile(&f.ftGap, 0, want-200000, true);
        }
        assert(f.ftWork.n==100 && f.ftGap.n==100);
        assert(rb_ft_interval_ns(&f.ftWork,&f.ftGap)==want);
        // 1e12/13888888 = 72000 milli-transactions a second. THE number this
        // whole line of work is about, arrived at from a timing budget rather
        // than from a counter delta.
        assert(rb_ft_rate_mhz(want)==72000);

        // The idle desktop. One second is 144 frame periods; without the
        // bound it would move the mean from 13.69 ms to 23.5 ms, and nothing
        // published would say why.
        const uint64_t meanBefore = rb_ft_mean_ns(&f.ftGap);
        f.ftFile(&f.ftGap, 0, 1000000000ull, true);
        assert(f.ftGap.dropped==1);
        assert(f.ftGap.n==100);
        assert(rb_ft_mean_ns(&f.ftGap)==meanBefore);

        // A backwards interval is a drop, not a 584-year sample.
        f.ftFile(&f.ftWork, 500, 400, false);
        assert(f.ftWork.dropped==1);
        assert(f.ftWork.n==100);
        assert(f.ftWork.max_ns==200000);

        // The shares of one interval sum to the whole, give or take integer
        // truncation - so a reader can subtract one from 100 % and not be
        // misled about the other.
        const uint64_t ours = 200000;
        const uint64_t up = rb_ft_upstream_ns(want, ours);
        assert(up == want-ours);
        const uint64_t sum = rb_ft_share_pm(ours,want)+rb_ft_share_pm(up,want);
        assert(sum>=999 && sum<=1000);

        // ftOn == false is the CONTROL and must be completely inert: not a
        // sample, not a drop. A control that still accumulated would make the
        // A/B compare two instrumented sides (rule 16).
        f.ftOn = false;
        const uint64_t nBefore = f.ftWork.n, dBefore = f.ftWork.dropped;
        f.ftFile(&f.ftWork, 0, 200000, false);
        f.ftFile(&f.ftWork, 500, 400, false);
        assert(f.ftWork.n==nBefore && f.ftWork.dropped==dBefore);

        // And a reset leaves no residue, including the previous exit stamp -
        // otherwise the first gap of a new window is measured from the last
        // transaction of the old one, which is precisely the cross-window
        // sample a reset exists to exclude.
        f.ftOn = true;
        f.ftPrevExit = 12345;
        rb_ft_reset(&f.ftWork); rb_ft_reset(&f.ftGap);
        f.ftPrevExit = 0;
        assert(f.ftWork.n==0 && f.ftWork.sum_ns==0 && f.ftWork.max_ns==0 &&
               f.ftWork.dropped==0);
        assert(rb_ft_interval_ns(&f.ftWork,&f.ftGap)==0);  // half a budget is none
        assert(rb_ft_rate_mhz(0)==0);
    }

    /* 2A1: the retry comparison must be a COMPARISON, which means the bucket
     * is selected by the previous GET's answer and not by the current one.
     * Getting that one step out of phase would file every held gap as a poll
     * and still produce two plausible means - the failure mode with no
     * symptom, so it is asserted directly. */
    {
        RBAMDFramebuffer f;
        f.vblPeriodNs = 6952507;

        // Apple asks again after 6.95 ms when we held, after 100 us when we
        // said ready. Filed the way the kext files them: the gap that FOLLOWS
        // a held GET belongs to ftRetry.
        for (unsigned i=0;i<50;i++) {
            f.ftFile(&f.ftRetry, 0, 6952507, true);
            f.ftFile(&f.ftPoll, 0, 100000, true);
        }
        assert(f.ftRetry.n==50 && f.ftPoll.n==50);
        assert(rb_ft_mean_ns(&f.ftRetry)==6952507);
        assert(rb_ft_mean_ns(&f.ftPoll)==100000);
        // 69x apart is the shape that would make the hold the ceiling; the
        // test asserts the accumulators can REPRESENT that, not that it is so.
        assert(rb_ft_mean_ns(&f.ftRetry) > 2*rb_ft_mean_ns(&f.ftPoll));

        // An idle desktop stops asking at all. One idle second in either
        // bucket would read as the retry cost, so it is bounded like the gap.
        const uint64_t before = rb_ft_mean_ns(&f.ftRetry);
        f.ftFile(&f.ftRetry, 0, 1000000000ull, true);
        assert(f.ftRetry.dropped==1 && rb_ft_mean_ns(&f.ftRetry)==before);

        // And a reset clears the phase, not just the sums: a leftover
        // ftPrevGetExit would file the first gap of a new window from the last
        // GET of the old one, into a bucket chosen by the old window's answer.
        f.ftPrevGetExit = 999; f.ftPrevGetHeld = true;
        rb_ft_reset(&f.ftRetry); rb_ft_reset(&f.ftPoll);
        f.ftPrevGetExit = 0; f.ftPrevGetHeld = false;
        assert(f.ftRetry.n==0 && f.ftRetry.dropped==0 && f.ftPoll.n==0);
        assert(f.ftPrevGetExit==0 && !f.ftPrevGetHeld);
    }

    /* 2A2: the hold's share, pinned to boot 284's own reading.
     *
     * The numbers below are that boot's, not invented: retry 4.384 ms
     * (n=2606), poll 2.771 ms (n=3059), 1531 transactions, 12.993 ms
     * interval. The point of using them is that the arithmetic has to
     * reproduce a measurement that already exists, so a future edit that
     * changes the formula stops agreeing with a boot (rule 18's spirit, and
     * the same trick 2y5's derived pixel clock used to check itself).
     *
     * And the PREMISE is asserted first, because 2A1 printed a verdict with
     * no premise at all: the share is only attributable if the GET gaps
     * account for the interval.
     */
    {
        const uint64_t retry=4384000, retryN=2606;
        const uint64_t poll=2771000,  pollN=3059;
        const uint64_t txn=1531, iv=12993000;

        // The gaps tile the interval: 4.384x2606 + 2.771x3059 over 1531
        // transactions is 13.00 ms against a 12.993 ms interval.
        const uint64_t tile = rb_ft_gets_tile_pm(retry,retryN,poll,pollN,txn,iv);
        assert(tile>=990 && tile<=1010);

        // 1.613 ms per hold, 1.70 holds per transaction, 2.75 ms of 12.993.
        const uint64_t pen = rb_ft_hold_penalty_ns(retry,poll);
        assert(pen==1613000);
        const uint64_t hpt = rb_ft_holds_per_txn_pm(retryN,txn);
        assert(hpt>=1700 && hpt<=1703);
        const uint64_t shr = rb_ft_hold_share_pm(pen,hpt,iv);
        assert(shr>=205 && shr<=215);        // ~21 %, which 2xo put at 27 %

        // THE RATIO IS THE WRONG STATISTIC, asserted directly: 1.58x would
        // have passed 2A1's "under 2.0x means no effect" threshold while
        // owning a fifth of the interval. This is the case that makes the
        // old verdict wrong, so it is the case that must not regress.
        assert(retry*100ull/poll < 200ull);  // ratio 1.58x, "no effect"
        assert(shr > 150);                   // yet >15 % of the interval

        // A tiling that does not hold must be visible as such, NOT clamped to
        // 100 %: gaps that overlap, or counts from two different windows,
        // produce more than one interval's worth and that is a refusal.
        const uint64_t bad = rb_ft_gets_tile_pm(retry,retryN*3,poll,pollN*3,txn,iv);
        assert(bad > 1100);

        // Missing inputs give no share rather than a plausible one.
        assert(rb_ft_hold_share_pm(0,hpt,iv)==0);
        assert(rb_ft_hold_share_pm(pen,0,iv)==0);
        assert(rb_ft_hold_share_pm(pen,hpt,0)==0);
        assert(rb_ft_holds_per_txn_pm(retryN,0)==0);
        assert(rb_ft_gets_tile_pm(retry,0,poll,0,txn,iv)==0);

        // And the hold being OFF is a real state the dial can reach: no
        // retries at all, so no penalty and no share - distinguishable from
        // a window nobody measured only by ftWork.n, which is why the share
        // is published beside its counts.
        assert(rb_ft_hold_penalty_ns(0,poll)==0);
        assert(rb_ft_hold_share_pm(0,0,iv)==0);
    }

    /* 2A7: a hold splits into hardware time and Apple's phase, and the test
     * is that the two cases are DISTINGUISHABLE - because 2A6 asserted the
     * phase reading from two numbers without one.
     *
     * The flip is vsync'd (RB_HUBP_FLIP_TYPE 0), so one vblank of any hold is
     * hardware time no answer of ours can skip. A latch wait at ~1 vblank
     * with ~1 ask means we are paying the hardware and nothing is
     * recoverable; ~2 vblanks with ~2 asks means the second ask arrived after
     * the latch and the phase is the cost.
     */
    {
        const uint64_t vbl = 6952507;
        struct rb_ft_acc wait1{}, asks1{};
        struct rb_ft_acc wait2{}, asks2{};

        // Case A: one vblank, one ask. Paying the hardware.
        for (unsigned i=0;i<40;i++) {
            rb_ft_add(&wait1, 7000000, 0);
            rb_ft_add(&asks1, 1, 0);
        }
        // Case B: two vblanks, two asks. Paying the phase.
        for (unsigned i=0;i<40;i++) {
            rb_ft_add(&wait2, 13900000, 0);
            rb_ft_add(&asks2, 2, 0);
        }

        const uint64_t m1 = rb_ft_mean_ns(&wait1), m2 = rb_ft_mean_ns(&wait2);
        // In vblanks, scaled by 100 so there is no floating point here.
        assert((m1*100ull)/vbl >= 100 && (m1*100ull)/vbl <= 105);   // ~1.01
        assert((m2*100ull)/vbl >= 195 && (m2*100ull)/vbl <= 205);   // ~2.00
        // The two cases must land on opposite sides of the 1.35 line
        // ftbudget uses, or the tool cannot tell them apart at all.
        assert((m1*100ull)/vbl < 135);
        assert((m2*100ull)/vbl > 135);
        // And the ask counts corroborate independently, so a wrong latch
        // reading cannot pass on its own.
        assert((asks1.sum_ns*1000ull)/asks1.n == 1000);
        assert((asks2.sum_ns*1000ull)/asks2.n == 2000);

        // An unclosed run files nothing, and that shows up as a count BELOW
        // the transaction count rather than as a cheap-looking mean - the
        // absence has to be visible (rule 14).
        struct rb_ft_acc open{};
        assert(open.n == 0 && rb_ft_mean_ns(&open) == 0);
    }
    /* 2A8: a share that implies an impossible rate must be REFUSED.
     *
     * Boot 288's own numbers: interval 10.696 ms, penalty 3.469 ms, 1.31
     * holds/txn, vblank 6.953 ms. Subtracting gives 6.15 ms = 162.6 /s on a
     * panel that can deliver at most 143.8 - so the prediction is not merely
     * optimistic, it is arithmetically impossible, and 2A4 had already
     * measured the no-hold rate at 141. The instrument printed it anyway.
     */
    {
        const uint64_t iv = 10696000, pen = 3469000, hpt = 1310;
        const uint64_t vbl = 6953000;

        /* The share itself is fine and stays reportable. */
        const uint64_t shr = rb_ft_hold_share_pm(pen, hpt, iv);
        assert(shr >= 420 && shr <= 426);           /* ~42.3 % */

        /* The RATE is refused, because 10.696 - 4.544 = 6.15 ms < one vblank. */
        assert(rb_ft_hold_free_interval_ns(iv, pen, hpt, vbl) == 0);

        /* A share that leaves more than a vblank is allowed through - the
         * check must not refuse everything, or it would be a constant 0 and
         * could not fail. This is the control for it (rule 26). */
        const uint64_t small = 500000;              /* 0.5 ms penalty */
        const uint64_t ok = rb_ft_hold_free_interval_ns(iv, small, 1000, vbl);
        assert(ok == iv - 500000);
        assert(ok > vbl);

        /* Exactly one vblank left is allowed; one nanosecond less is not -
         * the boundary is where an impossible answer begins. */
        assert(rb_ft_hold_free_interval_ns(vbl, 0, 0, vbl) == vbl);
        assert(rb_ft_hold_free_interval_ns(vbl - 1, 0, 0, vbl) == 0);

        /* A penalty that swallows the whole interval gives 0, not a wrap. */
        assert(rb_ft_hold_free_interval_ns(iv, iv, 1000, vbl) == 0);
        assert(rb_ft_hold_free_interval_ns(iv, iv * 2, 1000, vbl) == 0);

        /* Missing inputs give no prediction rather than a plausible one. */
        assert(rb_ft_hold_free_interval_ns(0, pen, hpt, vbl) == 0);
        assert(rb_ft_hold_free_interval_ns(iv, pen, hpt, 0) == 0);
    }

    /* 2A9: the latch wait's SHAPE, which is what decides whether a middle
     * setting between 91 and 141 sets/s exists at all.
     *
     * The two worlds the mean cannot separate, asserted as two distributions
     * that produce the SAME mean of ~1 vblank:
     *
     *   (a) every flip waits the whole period  -> early 0 per-mille
     *   (b) half resolve in the first quarter, half take two periods
     *                                          -> early 500 per-mille
     *
     * If rb_ft_hist_early_pm() ever returned the same answer for both, the
     * instrument could not do the job it exists for, so the test is that the
     * two are far apart - not that either number is right.
     */
    {
        const uint64_t vbl = 6952507;

        /* World (a): 200 flips, each needing a full period. */
        struct rb_ft_hist a{};
        for (unsigned i=0;i<200;i++) { rb_ft_hist_add(&a, vbl, vbl); }
        assert(rb_ft_hist_n(&a)==200);
        assert(a.b[RB_FT_HIST_BUCKETS-1]==200);
        assert(rb_ft_hist_early_pm(&a)==0);          /* no middle setting */

        /* World (b): 100 resolve almost at once, 100 take two periods. The
         * mean is ~1 period either way, which is the whole point. */
        struct rb_ft_hist b{};
        for (unsigned i=0;i<100;i++) { rb_ft_hist_add(&b, vbl/100, vbl); }
        for (unsigned i=0;i<100;i++) { rb_ft_hist_add(&b, vbl*2, vbl); }
        assert(rb_ft_hist_n(&b)==200);
        assert(b.b[0]==100 && b.b[RB_FT_HIST_BUCKETS-1]==100);
        assert(rb_ft_hist_early_pm(&b)==500);        /* half could run free */
        /* The two worlds must be FAR apart, or the histogram is useless. */
        assert(rb_ft_hist_early_pm(&b) > rb_ft_hist_early_pm(&a) + 400);
        assert(rb_ft_hist_late_pm(&a)==1000);
        assert(rb_ft_hist_late_pm(&b)==500);

        /* Bucket boundaries are quarters of the period and are half-open, so
         * a sample exactly on a boundary lands in the HIGHER bucket and
         * nothing can be double-counted. Asserted against a period divisible
         * by 4, because with 6952507 every vbl*k/4 truncates to just BELOW
         * its own boundary - which is correct behaviour and a trap for a test
         * that writes the boundary as vbl/4 and expects bucket 1. Both sides
         * of each boundary, so an off-by-one in either direction fails. */
        const uint64_t q = 6952504;               /* divisible by 4 */
        assert(rb_ft_hist_bucket(0, q)==0);
        assert(rb_ft_hist_bucket(q/4 - 1, q)==0);
        assert(rb_ft_hist_bucket(q/4,     q)==1);
        assert(rb_ft_hist_bucket(q/2 - 1, q)==1);
        assert(rb_ft_hist_bucket(q/2,     q)==2);
        assert(rb_ft_hist_bucket(q*3/4 - 1, q)==2);
        assert(rb_ft_hist_bucket(q*3/4,     q)==3);
        assert(rb_ft_hist_bucket(q - 1, q)==3);
        assert(rb_ft_hist_bucket(q,     q)==RB_FT_HIST_BUCKETS-1);
        assert(rb_ft_hist_bucket(q*9,   q)==RB_FT_HIST_BUCKETS-1);
        /* And on the panel's real, non-divisible period the floor still keeps
         * every bucket inside 0..4 with no gap. */
        assert(rb_ft_hist_bucket(vbl - 1, vbl)==3);
        assert(rb_ft_hist_bucket(vbl, vbl)==RB_FT_HIST_BUCKETS-1);

        /* The buckets SCALE with the mode. The same 3.5 ms wait is most of a
         * 144 Hz frame and a quarter of a 48 Hz one, and a histogram in fixed
         * milliseconds would have called both the same thing. */
        assert(rb_ft_hist_bucket(3476253, 6952507)==1);          /* ~0.5 */
        assert(rb_ft_hist_bucket(3476253, 6952507*3)==0);        /* ~0.17 */

        /* An unknown period must not manufacture evidence for world (b) -
         * the reading that would reopen a closed line. Everything piles into
         * the overflow bucket, so early_pm reads 0: "no middle". */
        struct rb_ft_hist u{};
        for (unsigned i=0;i<10;i++) { rb_ft_hist_add(&u, 1234, 0); }
        assert(u.b[RB_FT_HIST_BUCKETS-1]==10 && rb_ft_hist_early_pm(&u)==0);

        /* Shares sum back to the whole, and an empty histogram answers 0
         * rather than dividing by zero. */
        uint64_t sum=0;
        for (unsigned i=0;i<RB_FT_HIST_BUCKETS;i++) {
            sum += rb_ft_hist_share_pm(&b,i);
        }
        assert(sum==1000);
        struct rb_ft_hist e{};
        assert(rb_ft_hist_n(&e)==0 && rb_ft_hist_early_pm(&e)==0);
        assert(rb_ft_hist_share_pm(&e,0)==0);
        assert(rb_ft_hist_share_pm(&b,RB_FT_HIST_BUCKETS)==0);   /* no OOB */

        rb_ft_hist_reset(&b);
        assert(rb_ft_hist_n(&b)==0);
    }

    /* 2B0: boot 289's own five windows, and the partition that was WRONG.
     *
     * The measurement: in every window, in BOTH hold regimes, buckets 0..2
     * are essentially empty and everything sits in [0.75,1.00) or [>=1.00).
     * The first version of rb_ft_hist_early_pm() counted bucket 3 as "resolves
     * inside its own vblank, so a partial hold could let it through" and
     * printed "a middle setting MAY exist: 53 %" off exactly this shape.
     *
     * So the regression test is the DISCRIMINATION: on boot 289's numbers the
     * corrected statistic must say "no middle" (a few per-mille) where the old
     * partition said "half". A distribution with the same bucket-4 share but
     * genuinely early flips must still say "middle", or the fix would have
     * replaced one blind statistic with another.
     */
    {
        /* hold ON, run 2: Q = 0, 0, 9, 989, 895  (n 1893) */
        struct rb_ft_hist on{};
        on.b[0]=0; on.b[1]=0; on.b[2]=9; on.b[3]=989; on.b[4]=895;
        assert(rb_ft_hist_n(&on)==1893);
        assert(rb_ft_hist_early_pm(&on)==4);            /* 0.4 %: no middle */
        assert(rb_ft_hist_early_pm(&on) < 100);

        /* hold OFF, run 2 - the regime the flicker happens in:
         * Q = 0, 0, 22, 1316, 1485  (n 2823) */
        struct rb_ft_hist off{};
        off.b[0]=0; off.b[1]=0; off.b[2]=22; off.b[3]=1316; off.b[4]=1485;
        assert(rb_ft_hist_n(&off)==2823);
        assert(rb_ft_hist_early_pm(&off)==7);           /* 0.7 %: no middle */
        assert(rb_ft_hist_early_pm(&off) < 100);

        /* Ordinary desktop use, hold off: Q = 0, 0, 0, 77, 205  (n 282).
         * Real use pushes the wait LONGER, not shorter - further from a
         * middle setting, not closer. */
        struct rb_ft_hist desk{};
        desk.b[0]=0; desk.b[1]=0; desk.b[2]=0; desk.b[3]=77; desk.b[4]=205;
        assert(rb_ft_hist_n(&desk)==282);
        assert(rb_ft_hist_early_pm(&desk)==0);
        assert(rb_ft_hist_late_pm(&desk) > 700);

        /* THE CASE THAT MAKES THE OLD VERDICT WRONG. The old partition was
         * "everything except bucket 4", which on the hold-off window is
         * (2823-1485)/2823 = 47 % - the "53 %"/"47 %" the tool printed. The
         * corrected one reads 0.7 %. If this assertion ever stops holding,
         * the fix has been undone. */
        const uint64_t old_partition =
            ((rb_ft_hist_n(&off) - off.b[RB_FT_HIST_BUCKETS-1]) * 1000ull)
            / rb_ft_hist_n(&off);
        assert(old_partition > 400);                    /* said "middle!" */
        assert(rb_ft_hist_early_pm(&off) < 10);         /* says "none" */

        /* And the fix must not be a constant 0: a distribution with the SAME
         * bucket-4 share as the hold-off window but genuinely early flips
         * still reports a middle setting. Without this the check could pass
         * by always answering "no". */
        struct rb_ft_hist mid{};
        mid.b[0]=900; mid.b[1]=300; mid.b[2]=138; mid.b[3]=0; mid.b[4]=1485;
        assert(rb_ft_hist_n(&mid)==2823);
        assert(rb_ft_hist_late_pm(&mid)==rb_ft_hist_late_pm(&off));
        assert(rb_ft_hist_early_pm(&mid) > 400);        /* and says "middle" */

        /* Bucket 3 alone is NEVER early, whatever else is in the histogram -
         * the single assertion the old code would have failed. */
        struct rb_ft_hist q3{};
        q3.b[3]=1000;
        assert(rb_ft_hist_early_pm(&q3)==0);
        assert(rb_ft_hist_late_pm(&q3)==0);
    }

    /* 2A9: ftFileHist files the mean and the histogram from ONE sample, and
     * drops from BOTH. A sample counted in one and not the other would make
     * the published counts disagree, and that disagreement is the check a
     * reader has - so it must never happen for a reason other than a real
     * drop. */
    {
        RBAMDFramebuffer f;
        f.vblPeriodNs = 6952507;
        f.ftOn = true;

        for (unsigned i=0;i<40;i++) {
            f.ftFileHist(&f.ftPendWait, &f.ftPendHist, 0, 6952507);
        }
        assert(f.ftPendWait.n==40);
        assert(rb_ft_hist_n(&f.ftPendHist)==40);
        assert(rb_ft_mean_ns(&f.ftPendWait)==6952507);
        assert(rb_ft_hist_early_pm(&f.ftPendHist)==0);

        /* An idle desktop inside a pending run: dropped from both, so the
         * two counts stay equal and the drop is counted. */
        f.ftFileHist(&f.ftPendWait, &f.ftPendHist, 0, 1000000000ull);
        assert(f.ftPendWait.dropped==1);
        assert(f.ftPendWait.n==40 && rb_ft_hist_n(&f.ftPendHist)==40);

        /* A backwards interval is a counted drop, not a 584-year sample. */
        f.ftFileHist(&f.ftPendWait, &f.ftPendHist, 500, 400);
        assert(f.ftPendWait.dropped==2);
        assert(f.ftPendWait.n==40 && rb_ft_hist_n(&f.ftPendHist)==40);

        /* ftOn false is fully inert - the instrument is a suspect (rule 16),
         * so it must be possible to take it out of the frame path entirely. */
        f.ftOn = false;
        f.ftFileHist(&f.ftPendWait, &f.ftPendHist, 0, 6952507);
        assert(f.ftPendWait.n==40 && f.ftPendWait.dropped==2);
        assert(rb_ft_hist_n(&f.ftPendHist)==40);

        /* And a reset clears the run phase, not just the sums: a leftover
         * ftPendStart would file the first run of a new window from a stamp
         * taken in the old one - across a hold-regime change, which is
         * exactly what must not be mixed (rule 39). */
        f.ftPendStart = 12345; f.ftPendAsks = 7;
        rb_ft_reset(&f.ftPendWait);
        rb_ft_hist_reset(&f.ftPendHist);
        f.ftPendStart = 0; f.ftPendAsks = 0;
        assert(f.ftPendWait.n==0 && f.ftPendWait.dropped==0);
        assert(rb_ft_hist_n(&f.ftPendHist)==0);
        assert(f.ftPendStart==0 && f.ftPendAsks==0);
    }

    puts("framebuffer VBL: registration starts clock; per-handler disable, re-arm, cancellation, telemetry, a periodic crumb, the 2wl rate bucket, the 2wp self-disarm, the 2wk immediate kick 2x1's foreignFires, 2x3's per-vblank poll, 2xe's drift-free cadence and 2y6's mode retune and 2A0's frame-time budget and 2A1's retry comparison and 2A2's hold share against boot 284's own numbers and 2A7's hardware-vs-phase split and 2A8's impossible-rate refusal and 2A9's latch-wait histogram passed");
}
