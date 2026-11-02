/* rb_frametime.h - where the time in a frame period actually goes (2A0).
 *
 * WHY THIS EXISTS, AND WHY IT IS NOT AN EIGHTH MODEL OF THE ~73 CEILING.
 *
 * Seven dials have been moved at native and all seven returned 70-75 sets/s:
 * the retire tick down (2xn) and up (2zc), the timer beat (2zd), the latch
 * (2zg), the workload (2zb), the HUBP backlog (2zb) and the frame-path
 * publish cost (2zz). Every one of those experiments had the same shape -
 * change something, read `sets/s`, compare - and that shape cannot say where
 * the time goes, only whether a guess moved it. This header is the other
 * shape: a DECOMPOSITION of the wall clock between one transaction and the
 * next into named pieces that must sum back to the interval.
 *
 * It is built on a re-reading of a number already in hand rather than on a
 * new hypothesis. `fed` in tools/paired.sh is AmtrSets/s divided by vbl/s, so
 * `fed ~49 %` does not mean "half of every frame period is unaccounted for" -
 * it means **the compositor hands us a transaction only on every second
 * vblank**. And boot 271 at 60 Hz read `fed` 97-99 %, so it feeds every
 * vblank there. Those two facts together say the ceiling is an absolute rate
 * near 73/s rather than a fraction of the refresh, which is a statement about
 * a PERIOD - ~13.7 ms per transaction - and a period is a thing you measure
 * by timing it, not by guessing what sets it.
 *
 * THE BUDGET. Per transaction, the wall clock is tiled by exactly two pieces:
 *
 *   work = exit(setAttribute('amtr')) - entry(setAttribute('amtr'))
 *   gap  = entry(setAttribute('amtr')) - exit(previous setAttribute)
 *
 * so mean(work) + mean(gap) IS the mean inter-transaction interval, whose
 * reciprocal is `sets/s` - a quantity tools/paired.sh already measures a
 * completely different way. That is the instrument's self-check (rule 26: an
 * instrument must be able to say when its own premise has failed), and it is
 * the reason the sums are kept with their counts instead of as a running
 * mean.
 *
 * Inside `gap` we own two things and Apple owns the rest: the 'amtr' GETs
 * (including the 2xo/2xn kIOReturnNotReady holds, which are deliberately
 * BLOCKING - Apple's isTransactionComplete waits on them) and our own vblank
 * handler. Those are accumulated separately, so the remainder
 *
 *   upstream = gap - get - vbl
 *
 * is time in which Apple's accelerator had the frame and did not call us. If
 * `upstream` dominates the interval, the ceiling is upstream of this kext and
 * no dial of ours can move it - which would explain all seven dead models at
 * once rather than adding an eighth.
 *
 * THE PLAUSIBILITY BOUND IS NOT OPTIONAL. The desktop is idle most of the
 * time, and an idle second is a one-second `gap`. One of those swamps a mean
 * built from 1500 samples at 13.7 ms, so rb_ft_add() DISCARDS any sample
 * above a caller-supplied bound and counts the discards - an accumulator that
 * silently averaged them in would report the idle desktop as the frame cost
 * (rule 14: absence and nonsense must both say something).
 *
 * Freestanding C: no kernel headers, no floating point, so the hosttest runs
 * the same arithmetic the kext runs.
 */
#ifndef RB_FRAMETIME_H
#define RB_FRAMETIME_H

#include <stdint.h>

/* One accumulator. All times in NANOSECONDS - the caller converts, because
 * mach_absolute_time() units are not nanoseconds on every machine and a
 * conversion done once at the boundary cannot drift between members. */
struct rb_ft_acc {
    uint64_t sum_ns;
    uint64_t n;
    uint64_t max_ns;
    uint64_t dropped;      /* samples above the bound - see the header note */
};

static inline void rb_ft_reset(struct rb_ft_acc *a)
{
    if (!a) { return; }
    a->sum_ns = 0;
    a->n = 0;
    a->max_ns = 0;
    a->dropped = 0;
}

/* Add one sample. bound_ns == 0 means "no bound"; otherwise a sample strictly
 * greater than the bound is discarded and counted. Saturates rather than
 * wrapping: a sum that wrapped would read as a small, plausible mean, which
 * is the worst failure an instrument can have (rule 17's shape). */
static inline void rb_ft_add(struct rb_ft_acc *a, uint64_t ns, uint64_t bound_ns)
{
    if (!a) { return; }
    if (bound_ns != 0 && ns > bound_ns) {
        a->dropped++;
        return;
    }
    if (a->sum_ns > UINT64_MAX - ns) {
        a->dropped++;                      /* would wrap: refuse the sample */
        return;
    }
    a->sum_ns += ns;
    a->n++;
    if (ns > a->max_ns) { a->max_ns = ns; }
}

static inline uint64_t rb_ft_mean_ns(const struct rb_ft_acc *a)
{
    if (!a || a->n == 0) { return 0; }
    return a->sum_ns / a->n;
}

/* The self-check: mean(work) + mean(gap) is the mean interval between
 * transactions, so this must agree with 1e9 / sets_per_s measured any other
 * way. Returns 0 when either side has no samples, because a half-populated
 * budget is not a budget. */
static inline uint64_t rb_ft_interval_ns(const struct rb_ft_acc *work,
                                         const struct rb_ft_acc *gap)
{
    if (!work || !gap || work->n == 0 || gap->n == 0) { return 0; }
    return rb_ft_mean_ns(work) + rb_ft_mean_ns(gap);
}

/* Transactions per second from an interval, in MILLI-units (73.21/s reads as
 * 73210) so there is no floating point in the kext and no rounding decision
 * hidden in a divide. */
static inline uint64_t rb_ft_rate_mhz(uint64_t interval_ns)
{
    if (interval_ns == 0) { return 0; }
    return 1000000000000ull / interval_ns;
}

/* The part of the interval in which Apple had the frame and did not call us:
 * gap minus the two things inside it that are ours. Clamped at 0 - a negative
 * remainder means the pieces were measured on different windows, and
 * returning 0 with the caller publishing all four pieces lets the reader see
 * that for themselves rather than reading a wrapped huge number as a result.
 */
static inline uint64_t rb_ft_upstream_ns(uint64_t gap_mean_ns,
                                         uint64_t ours_mean_ns)
{
    return gap_mean_ns > ours_mean_ns ? gap_mean_ns - ours_mean_ns : 0;
}

/* Share of the interval, in per-mille, so "48.7 %" reads as 487. */
static inline uint64_t rb_ft_share_pm(uint64_t part_ns, uint64_t interval_ns)
{
    if (interval_ns == 0) { return 0; }
    if (part_ns > UINT64_MAX / 1000ull) { return 1000; }
    const uint64_t pm = (part_ns * 1000ull) / interval_ns;
    return pm > 1000 ? 1000 : pm;
}

/* The bound for a `gap` sample: a generous multiple of the frame period, so
 * it scales with the mode instead of being a constant that is right at 144 Hz
 * and wrong at 48. 8x is deliberately loose - at fed 50 % a real gap is
 * already 2x - because the cost of discarding a real sample is a biased mean
 * and the cost of keeping an idle one is a meaningless mean, and only the
 * second is invisible. */
static inline uint64_t rb_ft_gap_bound_ns(uint64_t vbl_period_ns)
{
    if (vbl_period_ns == 0) { return 0; }
    if (vbl_period_ns > UINT64_MAX / 8ull) { return 0; }
    return vbl_period_ns * 8ull;
}

/* 2A2: what our kIOReturnNotReady hold costs, from measurements only.
 *
 * 2A1 printed "the hold does NOT delay Apple measurably" off a ratio under
 * 2.0x, and that threshold was invented. Boot 284 read retry 4.384 ms against
 * poll 2.771 ms - a ratio of 1.58x, which the threshold called nothing - and
 * the hold in fact owned ~21 % of the interval, because the ratio is the wrong
 * statistic: what matters is the per-hold PENALTY times HOW MANY holds there
 * are per transaction, and 1.58x of a 2.77 ms base applied 1.7 times is
 * 2.75 ms of a 12.99 ms interval.
 *
 * THE PREMISE IS CHECKABLE, AND IS CHECKED. This arithmetic is only valid if
 * the GET gaps tile the interval - if the GETs are the only thing happening
 * between transactions. rb_ft_gets_tile_pm() returns how well they do, as a
 * fraction of the interval in per-mille, and boot 284 gave 1001: the weighted
 * mean gap (3.513 ms) times the GETs per transaction (3.7) reproduced the
 * 12.993 ms interval to 0.1 %. Below ~900 or above ~1100 the gaps do NOT
 * account for the interval and the penalty cannot be attributed this way -
 * the caller must say so rather than print a share (rule 14: a premise that
 * failed has to be audible).
 *
 * A withdrawn version of this arithmetic exists in 2A1 and the difference is
 * the whole point: there, one vblank per hold was INVENTED. Here the penalty
 * is the difference between two accumulators measured on the same clock in the
 * same window, and the tiling check says they describe the interval.
 */
static inline uint64_t rb_ft_hold_penalty_ns(uint64_t retry_mean_ns,
                                             uint64_t poll_mean_ns)
{
    return retry_mean_ns > poll_mean_ns ? retry_mean_ns - poll_mean_ns : 0;
}

/* Holds per transaction, in per-mille (1.70 reads as 1700), so the share
 * below needs no floating point. */
static inline uint64_t rb_ft_holds_per_txn_pm(uint64_t retry_n, uint64_t txn_n)
{
    if (txn_n == 0) { return 0; }
    if (retry_n > UINT64_MAX / 1000ull) { return 0; }
    return (retry_n * 1000ull) / txn_n;
}

/* The share of the interval the hold owns: penalty x holds-per-transaction,
 * over the interval, in per-mille. 0 when any input is missing, because a
 * share computed from a half-populated window is worse than no share. */
static inline uint64_t rb_ft_hold_share_pm(uint64_t penalty_ns,
                                           uint64_t holds_pm,
                                           uint64_t interval_ns)
{
    if (interval_ns == 0 || penalty_ns == 0 || holds_pm == 0) { return 0; }
    if (penalty_ns > UINT64_MAX / holds_pm) { return 1000; }
    const uint64_t owned_ns = (penalty_ns * holds_pm) / 1000ull;
    return rb_ft_share_pm(owned_ns, interval_ns);
}

/* Do the GET gaps account for the interval? Weighted mean gap times GETs per
 * transaction, over the interval, in per-mille. ~1000 means yes. This is the
 * premise of rb_ft_hold_share_pm() and must be read before it. */
static inline uint64_t rb_ft_gets_tile_pm(uint64_t retry_mean_ns,
                                          uint64_t retry_n,
                                          uint64_t poll_mean_ns,
                                          uint64_t poll_n,
                                          uint64_t txn_n,
                                          uint64_t interval_ns)
{
    const uint64_t n = retry_n + poll_n;
    if (n == 0 || txn_n == 0 || interval_ns == 0) { return 0; }
    /* Sum of all gaps, which is already "the time the GETs account for". No
     * need for the mean and the count separately - and summing avoids a
     * rounding step that at 3.5 ms x 5665 would be visible. */
    const uint64_t total_ns = retry_mean_ns * retry_n + poll_mean_ns * poll_n;
    const uint64_t per_txn = total_ns / txn_n;
    /* NOT rb_ft_share_pm(), which clamps at 1000: a tiling of 1300 is a
     * REFUSAL - the gaps overlap or the counts come from different windows -
     * and clamping it to "100 %" would read as the premise holding. */
    if (per_txn > UINT64_MAX / 1000ull) { return 0; }
    return (per_txn * 1000ull) / interval_ns;
}

#endif /* RB_FRAMETIME_H */
