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
};

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
}

/* The vblank offers to retire. Returns 1 if it should go ahead - which is the
 * only thing that moves the driver to VBL, so an unarmed build keeps the timer
 * and the offer is counted rather than silently dropped (rule 14). */
static inline int rb_retire_vbl_offer(struct rb_retire *r, uint64_t now)
{
    if (!r) { return 0; }
    if (!r->armed) { r->vblDeclined++; return 0; }
    if (r->src != RB_RETIRE_SRC_VBL) {
        r->src = RB_RETIRE_SRC_VBL;
        r->handoffs++;
    }
    r->lastVbl = now;
    r->vblRetires++;
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

#endif /* RB_RETIRE_H */
