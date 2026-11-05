/* rb_retire.h - WHO DRIVES FRAME RETIREMENT, and the handover between them.
 *
 * 2zc measured the retire tick as a two-sided cost: 2000 us gives 51.33
 * sets/s, 6944 gives 74.6, 13888 gives 49.2, so 6944 us - exactly the native
 * panel period - is an INTERIOR optimum. A cost that rises in BOTH directions
 * from a point is not a rate limit, because a rate limit is one-sided; it is
 * two periodic timers beating against each other, the retire thread_call
 * against the VBL thread_call. Five models of that ceiling have now died, so
 * the fix deliberately does not tune the period: it DELETES the second timer
 * by retiring from the vblank that is already firing.
 *
 * WHY THIS IS A STATE MACHINE AND NOT AN `if`. The two drivers live in
 * different kexts and different kernel collections - the retire target is
 * resolved by the plugin in BootKernelExtensions.kc, the vblank belongs to
 * RBAMDFramebuffer in the auxiliary collection - so neither can link against
 * the other and NEITHER CAN SEE WHETHER THE OTHER IS ALIVE. The framebuffer
 * may be absent, may decline the panel (rule 38), may have its vblank
 * disabled by a mode switch, or may simply be an older build with no handover
 * at all. 2xe is what happens if retirement stops: the desktop goes to one
 * frame per ten seconds. So the timer is never removed, only DEMOTED to a
 * watchdog, and it takes the job back the moment the vblank goes quiet.
 *
 * THE INVARIANT THE HOSTTEST ASSERTS: retirement never stops, and never
 * happens twice for one tick. For any interleaving of vblanks and timer ticks,
 * if the vblank is quiet for longer than `quiet` the timer retires again, and
 * while the vblank is live the timer retires nothing.
 *
 * All times are mach_absolute_time() units. Subtraction is unsigned and
 * compared as a difference so a clock that fails to advance cannot be read as
 * a huge gap.
 */
#ifndef RB_RETIRE_H
#define RB_RETIRE_H

#include <stdint.h>

#define RB_RETIRE_SRC_TIMER 0u
#define RB_RETIRE_SRC_VBL   1u

struct rb_retire {
    uint32_t armed;        /* the handover is permitted at all (boot-arg) */
    uint32_t src;          /* who is driving RIGHT NOW */
    uint64_t lastVbl;      /* abs time of the last vblank-driven retire */
    uint64_t vblRetires;   /* retires driven by the vblank */
    uint64_t timerRetires; /* retires driven by the timer */
    uint64_t vblDeclined;  /* vblank offers refused because not armed */
    uint64_t handoffs;     /* timer -> vblank transitions */
    uint64_t takebacks;    /* vblank -> timer, the watchdog firing */
    /* 2CF: sub-vblank retire occasions. See the block comment below. */
    uint32_t sub;          /* occasions per vblank period; 1 = off (pre-2CF) */
    uint32_t subRun;       /* interior fires since the last vblank */
    uint64_t subPeriodNs;  /* the vblank period the framebuffer last reported */
    uint64_t subRetires;   /* interior retires, cumulative */
    uint64_t subSkipped;   /* interior fires that found the run already full */
    uint64_t subLate;      /* interior arms whose slot was already past */
};

/* 2zf: IS THIS MODE ONE THE VBLANK CAN CARRY?
 *
 * Boot 267 measured the handover as a +24 % gain at native refresh and a
 * REGRESSION below it: 90.4 sets/s at 143.85 Hz against a 73.1 baseline, but
 * 59.45 at 119.97 (was ~74.5) and 30.04 at 60.01 (was ~52), the last two
 * pinned at exactly 50 % of their vblank rate.
 *
 * The cause is not new, it is 2zc's own curve. Retirement used to run at a
 * FIXED 6944 us - 144/s - whatever the display was doing; driving it from the
 * vblank ties it to the display rate instead, so a 60 Hz mode runs it at
 * 0.42x the measured optimum, and 2zc measured that halving the rate costs
 * about a third. The retire wants a RATE, and only the native vblank supplies
 * it.
 *
 * So the vblank takes the job only where it can do it at least as often as
 * the timer would. `native_ns` is the framebuffer's own native period, which
 * it already knows, so this needs no value carried across the collection
 * boundary. Slack is one part in sixteen: the period is derived per boot from
 * a spread-spectrum clock (2y6) and must not fail an exact compare against
 * itself.
 */
static inline int rb_retire_vbl_suits(uint64_t period_ns, uint64_t native_ns)
{
    if (!period_ns || !native_ns) { return 0; }
    return period_ns <= native_ns + (native_ns >> 4);
}

static inline void rb_retire_reset(struct rb_retire *r, uint32_t armed)
{
    if (!r) { return; }
    r->armed        = armed ? 1u : 0u;
    r->src          = RB_RETIRE_SRC_TIMER;
    r->lastVbl      = 0u;
    r->vblRetires   = 0u;
    r->timerRetires = 0u;
    r->vblDeclined  = 0u;
    r->handoffs     = 0u;
    r->takebacks    = 0u;
    r->sub          = 1u;
    r->subRun       = 0u;
    r->subPeriodNs  = 0u;
    r->subRetires   = 0u;
    r->subSkipped   = 0u;
    r->subLate      = 0u;
}

/* The vblank offers to retire. Returns 1 if it should go ahead - which is the
 * only thing that moves the driver to VBL, so an unarmed build keeps the timer
 * and the offer is counted rather than silently dropped (rule 14). */
static inline int rb_retire_vbl_offer(struct rb_retire *r, uint64_t now,
                                      uint64_t vbl_period_ns)
{
    if (!r) { return 0; }
    if (!r->armed) { r->vblDeclined++; return 0; }
    if (r->src != RB_RETIRE_SRC_VBL) {
        r->src = RB_RETIRE_SRC_VBL;
        r->handoffs++;
    }
    r->lastVbl = now;
    r->vblRetires++;
    /* 2CF: the framebuffer's own live period, carried on every offer rather
     * than cached at setup, because the mode changes at runtime and without a
     * reboot (2z1) and because `RBFBVBLPeriodNs` is re-derived per mode from a
     * spread-spectrum clock (2y6) - so there is ONE rate per mode and no
     * second copy of it here to drift. A zero means the framebuffer did not
     * say, and then the interior slots cannot be placed at all. */
    r->subPeriodNs = vbl_period_ns;
    r->subRun      = 0u;
    return 1;
}

/* The timer tick asks whether it should retire. In TIMER mode it always does.
 * In VBL mode it does so only when the vblank has been quiet for `quiet`, and
 * taking the job back is a state change, not a one-off - otherwise a vblank
 * that stopped for good would be rediscovered as "quiet" on every tick and
 * the handoff counter would climb forever. */
static inline int rb_retire_timer_should(struct rb_retire *r, uint64_t now,
                                         uint64_t quiet)
{
    if (!r) { return 0; }
    if (r->src == RB_RETIRE_SRC_VBL) {
        /* Unsigned, and only ever a forward difference: if `now` has not
         * advanced past lastVbl the gap is 0, never 2^64 - something. */
        const uint64_t gap = (now > r->lastVbl) ? (now - r->lastVbl) : 0u;
        if (gap < quiet) { return 0; }
        r->src = RB_RETIRE_SRC_TIMER;
        r->takebacks++;
    }
    r->timerRetires++;
    return 1;
}

/* What period the timer should re-arm at. Demoted to a watchdog it must still
 * fire often enough to catch a dead vblank inside one `quiet` window, and
 * rarely enough that it is not the thing it was demoted for being. */
static inline uint32_t rb_retire_timer_period_us(const struct rb_retire *r,
                                                 uint32_t fast_us,
                                                 uint32_t watch_us)
{
    if (!r) { return fast_us; }
    return (r->src == RB_RETIRE_SRC_VBL) ? watch_us : fast_us;
}


/* 2C2: HOW MANY TIMES TO POLL THE RETIREMENT PER OFFER.
 *
 * Boot 409 measured `metal_probe --queues N` starting each queue's first
 * command buffer exactly ONE vblank after the previous one: 13.3-14.5 ms for
 * queue 0 (n=12, tight) and then +6.9 to +7.2 ms per queue, with the absolute
 * slots landing at the same times for N = 2, 4 and 8. `RBFBVBLPeriodNs` is
 * 6952507, so the spacing IS the vblank period to three figures, and
 * `stamp_calls` 42337 against `stamp_completed` 42314 says about one stamp
 * retires per call. We call `checkTimestamps()` once per vblank, so N queues
 * with pending work need N vblanks and nothing runs concurrently.
 *
 * That is invisible to a single stream - one buffer every 6.95 ms is the
 * 144/s the compositor wants, which is why the desktop is smooth and 2A1
 * measured our own cost at 44 microseconds - and it is exactly what an app
 * needing several units of GPU work BEFORE it can draw pays for: the cost is
 * per queue, serialised.
 *
 * So poll more than once per offer. Deliberately NOT progress-detected:
 * 2xe read the event source as signalled by `checkTimestamps` rather than
 * advanced by it, so `advance()` runs on the event source's own thread and a
 * counter delta across our call would not be a synchronous progress signal -
 * inferring otherwise is rule 35 exactly. An unconditional bounded loop needs
 * no such inference, and the instrument is EXTERNAL: metal_probe's ladder,
 * which is userspace, instant and reproduced twelve times.
 *
 * 1 is today's behaviour and the A side. The bound exists because this runs
 * on the framebuffer's vblank thread_call (rule 31): at 144 Hz, 8 is 1152
 * calls a second where today's is 144, and 2zc measured that making the
 * retirement run MORE often can cost a third of the frame rate when it
 * contends - which is why this is a dial with an interleaved A/B and not a
 * default.
 */
#define RB_RETIRE_DRAIN_MAX 8u

static inline uint32_t rb_retire_drain_clamp(uint32_t want)
{
    if (want == 0u) { return 1u; }
    if (want > RB_RETIRE_DRAIN_MAX) { return RB_RETIRE_DRAIN_MAX; }
    return want;
}


/* 2C4: THE RETIRE-TICK PERIOD AS A RUNTIME DIAL, AND WHY IT IS THE LEVER.
 *
 * 2C2 read the queue ladder on boot 409/410 at the panel's native 143.8 Hz
 * and concluded "one queue released per VBLANK", because `RBFBVBLPeriodNs`
 * 6952507 matched the ~6.95 ms step to three figures. Boot 413 shows that
 * identification could not have been made there: that boot runs the 60 Hz
 * mode, so the vblank period is 16669443 ns, the timer is driving
 * (`vblret_src` 0 - `rb_retire_vbl_suits()` hands sub-native modes back),
 * `tick_us` is 6944 - and `metal_probe --queues 8` spread 56.009 ms,
 * indistinguishable from boot 410's 55.295-56.308 ms at native.
 *
 * Had the vblank been the quantum, 60 Hz would have cost 2.4x as much:
 * ~117 ms for eight queues against the ~56 measured. It is therefore NOT
 * the vblank. What is equal across the two boots is the RETIRE-OCCASION
 * rate: ~144/s, supplied by the vblank at native and by this timer at
 * 60 Hz, which are deliberately the same number (RB_2XG_DEFAULT_US 6944 was
 * tuned to the measured 143.83 Hz in 2xe). 2C2 measured a real lattice and
 * attached it to the one of two equal periods that happens not to be ours.
 *
 * The consequence is that there IS a dial, where 2C2's reading said there
 * was none: when the timer drives, the ladder's step is this value. The
 * prediction, stated before the boot that tests it - halving `tick_us` to
 * 3472 at 60 Hz halves the eight-queue spread to ~28 ms. If it does, the
 * concurrency latency the user feels as "Settings icons reload every time"
 * is buyable, and 2zc's dose-response says the price is frame rate (2000 us
 * cost a third of it). If it does not, the tick merely correlates and the
 * pacing is elsewhere - which is a different answer, not a null one.
 *
 * It is a DIAL and not a boot-arg because rule 44 is now "interleaved inside
 * one boot or two boots per side", boot 292 having drifted 15 % inside
 * itself - and because `rbgpu_2xg_arm_next()` recomputes the period from
 * this value on every single arm, so a write here needs no reschedule, no
 * teardown and no lock: the next arm picks it up. The worst a race costs is
 * one tick at the old period.
 *
 * The bounds are the boot-arg's own (2xm's RB_2XG_MIN_US/MAX_US), defined
 * here so one definition serves the parse, the dial and the hosttest rather
 * than three that can drift - which is what `rb_drr.h` was built to avoid.
 *
 * NOTE ON WHERE IT BITES: while the vblank drives retirement,
 * `rb_retire_timer_period_us()` above runs this timer at the WATCHDOG period
 * instead, so the dial moves nothing at native with `-rbgpuretirevbl`. The
 * experiment belongs in a timer-driven configuration - the 60 Hz mode boot
 * 413 is already in - and `tickset` must publish `vblret_src` beside the
 * value so a null result cannot be the dial being inert (rule 39).
 */
#define RB_TICK_US_MIN      500u
#define RB_TICK_US_MAX  1000000u

static inline uint32_t rb_tick_us_clamp(uint32_t want)
{
    if (want < RB_TICK_US_MIN) { return RB_TICK_US_MIN; }
    if (want > RB_TICK_US_MAX) { return RB_TICK_US_MAX; }
    return want;
}

/* Rule 14 as a code rather than a bool: the setter must be able to say WHICH
 * way a value was out of range, because "too fast" and "too slow" are
 * different mistakes and a single refusal merges them. */
#define RB_TICK_OK        0u
#define RB_TICK_TOO_FAST  1u
#define RB_TICK_TOO_SLOW  2u

static inline uint32_t rb_tick_us_verdict(uint32_t want)
{
    if (want < RB_TICK_US_MIN) { return RB_TICK_TOO_FAST; }
    if (want > RB_TICK_US_MAX) { return RB_TICK_TOO_SLOW; }
    return RB_TICK_OK;
}

/* 2CF: SUB-VBLANK RETIRE OCCASIONS - THE SAME LEVER AT NATIVE REFRESH.
 *
 * WHAT IS ESTABLISHED, and this rests on five measured boots rather than a
 * model. 2C5 halved the retire tick and the eight-queue ladder halved with it
 * (1.995x for a 2.000x tick change); 2C7 fitted five ticks to
 * `spread = 8.036 x tick - 0.20 ms` with residuals inside +-1.0 %; 2C8 held
 * `spread/tick` at 7.996 +- 0.037 over SEVEN ticks across a 6x range. So the
 * quantum that paces concurrent queues is the RETIRE OCCASION and its period
 * is the whole of it. 2C8 named the threshold: at `tick = vbl/8` the
 * eight-queue spread is exactly one frame, which is where multi-queue work
 * stops costing extra frames to first paint.
 *
 * WHY THAT LEVER IS UNREACHABLE AT NATIVE TODAY. With `-rbgpuretirevbl` the
 * vblank drives retirement (`vblret_src` 1) and `rb_retire_timer_period_us()`
 * demotes this timer to a 50 ms watchdog, so `tickset` is inert - it refuses
 * rather than measuring, which is rule 39 enforced. One occasion per vblank
 * is 6.95 ms at 143.8 Hz, so eight queues span ~56 ms however the tick is
 * set. The choice has therefore been: native refresh and a coarse quantum, or
 * a sub-native mode where the timer drives and the quantum is buyable.
 *
 * AND WHY THE OBVIOUS FIXES ARE BOTH DEAD. Polling harder inside one occasion
 * is MEASURED DEAD - 2C2 ran `checkTimestamps()` up to 8 times per offer and
 * six interleaved runs gave 55.295-56.308 ms with `vblret_extra` 50897
 * proving the polls ran. Occasions need to be spread in TIME, not stacked.
 * And simply running both drivers at once is 2zc's configuration, which cost
 * a third of the frame rate: two independent periodic timers BEAT, and 2zg
 * then measured the beat as the noise source behind every variance surprise
 * from 2zb onward.
 *
 * SO THE OCCASIONS ARE PHASE-LOCKED BY CONSTRUCTION. `sub` is how many
 * occasions one vblank period carries: 1 is today's behaviour exactly, 4 puts
 * three interior occasions between consecutive vblanks. The interior deadlines
 * are computed from `lastVbl`, the timestamp of the last vblank-driven retire,
 * NEVER walked forward from the previous arm - so there are not two periods
 * that can drift apart, there is one period subdivided, re-anchored every
 * frame by the display itself. That is the difference from 2zc, and it is a
 * property of the arithmetic rather than a tuning.
 *
 * WHAT IS DELIBERATELY NOT CLAIMED. The law was fitted with the TIMER driving
 * in a sub-native mode. Whether an occasion supplied this way is worth the
 * same as one from a free-running timer is UNMEASURED, and there is a named
 * reason it might not be: an interior occasion runs on a thread_call the
 * display did not wake, at a phase inside the frame that the timer-driven
 * regime never held fixed. `sub` is therefore a runtime dial, default 1, so
 * the A/B is interleaved inside one boot - rule 44, which 2B5-2B7 paid for.
 *
 * The frame-rate side is the risk and the hosttest cannot see it: these are
 * extra `checkTimestamps()` calls in the regime 2zg measured as the quiet one,
 * so read `tools/paired.sh` and `ftbudget` as well as the ladder, and 2zc's
 * two-sided curve is the shape to watch for.
 */
#define RB_RETIRE_SUB_MIN 1u
#define RB_RETIRE_SUB_MAX 8u

static inline uint32_t rb_retire_sub_clamp(uint32_t want)
{
    if (want < RB_RETIRE_SUB_MIN) { return RB_RETIRE_SUB_MIN; }
    if (want > RB_RETIRE_SUB_MAX) { return RB_RETIRE_SUB_MAX; }
    return want;
}

/* 1 when interior occasions should be placed at all.
 *
 * Every term is a refusal someone has paid for. `armed`/`src` keep this inert
 * in the timer-driven regime, where `tickset` already owns the quantum and two
 * mechanisms for one job is how 2zc happened. `sub > 1` is the dial. And a
 * zero period means the framebuffer did not report one, which must read as
 * "cannot place them" and never as "place them at zero" - a zero interval
 * thread_call is a spin (rule 16: the instrument is a suspect, and so is the
 * dial). */
static inline int rb_retire_sub_active(const struct rb_retire *r)
{
    if (!r) { return 0; }
    if (!r->armed) { return 0; }
    if (r->src != RB_RETIRE_SRC_VBL) { return 0; }
    if (r->sub < 2u || r->sub > RB_RETIRE_SUB_MAX) { return 0; }
    if (!r->subPeriodNs) { return 0; }
    return 1;
}

/* The interior spacing in nanoseconds, or 0 when there is none to place.
 * Derived from the period the framebuffer reported, so a mode switch moves it
 * on the next vblank with nothing to invalidate. */
static inline uint64_t rb_retire_sub_period_ns(const struct rb_retire *r)
{
    if (!rb_retire_sub_active(r)) { return 0u; }
    return r->subPeriodNs / (uint64_t)r->sub;
}

/* Should THIS timer fire be taken as an interior retire occasion?
 *
 * Called only after `rb_retire_timer_should()` has declined, so the vblank is
 * driving and this fire is not the watchdog taking the job back. A run holds
 * `sub - 1` interior occasions: the vblank itself is the first of the `sub`,
 * and counting it again would retire twice at the same phase.
 *
 * The full-run case is COUNTED rather than silently dropped (rule 14): it is
 * what a vblank that stopped arriving looks like from in here, and it must be
 * distinguishable from an interior occasion that never ran at all. */
static inline int rb_retire_sub_should(struct rb_retire *r)
{
    if (!rb_retire_sub_active(r)) { return 0; }
    if (r->subRun + 1u >= r->sub) { r->subSkipped++; return 0; }
    r->subRun++;
    r->subRetires++;
    return 1;
}

/* The absolute deadline of the next interior occasion, or 0 if there is none
 * to arm - which includes the case where the slot is ALREADY PAST.
 *
 * `sub_period_abs` is `rb_retire_sub_period_ns()` converted by the caller,
 * because this header is freestanding and knows nothing of mach timebase.
 *
 * Returning 0 for a slot in the past is the point of this function rather than
 * an edge case. Arming a thread_call into the past fires a catch-up burst -
 * the drift defect 2xe already fixed once in this same timer - and here the
 * anchor is a timestamp taken on another thread, so a late fire is ordinary
 * rather than exceptional. The caller falls back to the watchdog arm and the
 * next vblank re-anchors everything. */
static inline uint64_t rb_retire_sub_deadline(const struct rb_retire *r,
                                              uint64_t sub_period_abs,
                                              uint64_t now)
{
    uint64_t d;

    if (!rb_retire_sub_active(r) || !sub_period_abs) { return 0u; }
    if (r->subRun + 1u >= r->sub) { return 0u; }      /* run is full */
    d = r->lastVbl + sub_period_abs * (uint64_t)(r->subRun + 1u);
    if (d <= now) { return 0u; }
    return d;
}

#endif /* RB_RETIRE_H */
