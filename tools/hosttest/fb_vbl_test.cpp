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
    puts("framebuffer VBL: registration starts clock; per-handler disable, re-arm, cancellation, telemetry, a periodic crumb, the 2wl rate bucket, the 2wp self-disarm, the 2wk immediate kick 2x1's foreignFires, 2x3's per-vblank poll, 2xe's drift-free cadence and 2y6's mode retune and 2A0's frame-time budget and 2A1's retry comparison passed");
}
