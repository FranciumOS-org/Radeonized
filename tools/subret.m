/* SPDX-License-Identifier: MIT */
/* 2CF: sub-vblank retire occasions - 2C5-2C8's lever, at NATIVE refresh.
 *
 * WHAT IS ESTABLISHED. The quantum pacing concurrent Metal queues is the
 * retire occasion, and the eight-queue spread is 8x its period:
 * 2C5 halved the tick and the ladder halved (1.995x for 2.000x), 2C7 fitted
 * five ticks to `spread = 8.036 x tick - 0.20 ms` inside +-1.0 %, and 2C8
 * held `spread/tick` at 7.996 +- 0.037 over SEVEN ticks across a 6x range.
 * 2C8 named the threshold: at `tick = vbl/8` the eight-queue spread is exactly
 * one frame, which is where multi-queue work stops costing extra frames to
 * first paint.
 *
 * WHY `tickset` CANNOT REACH IT AT NATIVE. With the vblank driving
 * (`vblret_src` 1), `rb_retire_timer_period_us()` demotes that timer to a
 * 50 ms watchdog, so the tick value is unused and `tickset` refuses rather
 * than measuring (rule 39). One occasion per vblank is 6.95 ms at 143.8 Hz,
 * so eight queues span ~56 ms however the tick is set. That is the whole
 * reason the ladder has only ever been improved in a sub-native mode.
 *
 * WHAT THIS DIAL DOES. `sub` is how many retire occasions one vblank period
 * carries. 1 is today's behaviour exactly. 4 places three interior occasions
 * between consecutive vblanks, anchored on `lastVbl` - the display's own
 * timestamp - and re-anchored every frame, so there is ONE period subdivided
 * rather than two periods that can beat. That distinction is the design: 2zc
 * measured two independent timers costing a third of the frame rate, and 2zg
 * then identified the beat as the noise source behind every variance surprise
 * from 2zb onward.
 *
 * HOW TO READ IT, and the instrument is metal_probe, not this tool:
 *
 *     subret 1 && build/out/metal_probe --queues 8     # A: today
 *     subret 4 && build/out/metal_probe --queues 8     # B: four per frame
 *
 * and ALTERNATE, several times each. Boot 292 drifted ~15 % inside itself,
 * which is why rule 44 accepts only an INTERLEAVED single-boot A/B.
 *
 * SAID BEFORE THE READING, so it can be wrong. If an interior occasion is
 * worth the same as a timer-driven one, B divides the eight-queue spread by 4
 * - ~56 ms to ~14 ms. If the spread does not move, an occasion supplied this
 * way is NOT equivalent, and the reason would be worth having: it runs on a
 * thread_call the display did not wake, at a phase inside the frame that the
 * timer-driven regime never held fixed. That is a different answer, not a null
 * one.
 *
 * WHAT MIGHT GO WRONG, and it is the frame rate. These are extra
 * `checkTimestamps()` calls in the regime 2zg measured as the quiet one, so
 * read `tools/paired.sh` and `build/out/ftbudget` as well as the ladder. 2zc's
 * two-sided curve is the shape to watch for. Nothing here writes to the chip,
 * and `subret 1` or a reboot restores today's behaviour.
 *
 * Does not need root: boot 424 took the setProperties write as the console
 * user and `vblret_subsets` advanced. Checked rather than copied from
 * `tickset`'s banner.
 */
#import <Foundation/Foundation.h>
#import <IOKit/IOKitLib.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static long diagNum(io_service_t peer, NSString *key, long dflt)
{
    CFTypeRef d = IORegistryEntryCreateCFProperty(peer, CFSTR("RBGPUDiag"),
                                                  kCFAllocatorDefault, 0);
    long out = dflt;
    if (d && CFGetTypeID(d) == CFDictionaryGetTypeID()) {
        NSNumber *v = ((__bridge NSDictionary *)d)[key];
        if (v) { out = [v longValue]; }
    }
    if (d) { CFRelease(d); }
    return out;
}

int main(int argc, char **argv)
{
    io_service_t peer = IOServiceGetMatchingService(
        kIOMainPortDefault, IOServiceMatching("RembrandtGPUPeer"));
    if (!peer) {
        fprintf(stderr, "no RembrandtGPUPeer - is the accelerator armed?\n");
        return 1;
    }

    /* Read from the table, never from a return code (rule 13). `subsets` is
     * how a call that never arrived is told from one that arrived and was
     * lost - the ambiguity 2C2's drain bug spent a milestone in. */
    const long live   = diagNum(peer, @"vblret_sub",     -1);
    const long sets   = diagNum(peer, @"vblret_subsets", -1);
    const long src    = diagNum(peer, @"vblret_src",     -1);
    const long armed  = diagNum(peer, @"vblret_armed",   -1);
    const long subns  = diagNum(peer, @"vblret_subns",   -1);
    const long subret = diagNum(peer, @"vblret_subret",  -1);
    const long sublat = diagNum(peer, @"vblret_sublate", -1);
    const long subfull = diagNum(peer, @"vblret_subfull", -1);
    const long tick   = diagNum(peer, @"tick_us",        -1);

    if (live < 0) {
        fprintf(stderr,
                "`vblret_sub` is absent from the diag table, so the running\n"
                "plugin predates 2CF. Check the loaded UUID (rule 15) - a\n"
                "rebuild is not an install.\n");
        IOObjectRelease(peer);
        return 1;
    }

    printf("occasions per vblank period: %ld", live);
    if (subns > 0) {
        printf("   interior spacing %ld us", subns / 1000);
    }
    printf("\n");
    printf("driver: %s\n",
           src == 1 ? "the VBLANK (vblret_src 1) - this dial is LIVE" :
           src == 0 ? "the TIMER (vblret_src 0) - use tickset instead" :
                      "UNKNOWN (vblret_src absent)");
    if (sets >= 0)   { printf("sets so far: %ld\n", sets); }
    if (subret >= 0) {
        printf("interior retires: %ld   slots MISSED: %ld", subret,
               sublat < 0 ? 0 : sublat);
        /* 2D0: printed apart, because merged they read as a fault. A run
         * completing is the ordinary end of a frame's subdivision. */
        if (subfull >= 0) { printf("   runs completed: %ld", subfull); }
        printf("\n");
    }
    if (tick > 0)    { printf("(the timer's own period is %ld us, unused while "
                              "the vblank drives)\n", tick); }

    if (armed == 0) {
        fprintf(stderr,
                "\n`vblret_armed` is 0 - the vblank handover is not armed, so\n"
                "the vblank never drives and this dial controls nothing. It\n"
                "needs -rbgpuretirevbl.\n");
        IOObjectRelease(peer);
        return 4;
    }

    if (argc < 2) {
        printf("\nto change it: subret <1..8>\n"
               "  1 = today's behaviour and the A side.\n"
               "  4 = three interior occasions per frame, the B side.\n"
               "  8 = 2C8's vbl/8 threshold, where the eight-queue spread\n"
               "      is one frame.\n"
               "  measure with:  build/out/metal_probe --queues 8\n"
               "  and read the frame rate too:  sudo tools/paired.sh\n"
               "  ALTERNATE the sides, do not run a block of each "
               "(rule 44).\n");
        IOObjectRelease(peer);
        return 0;
    }

    const long want = atol(argv[1]);
    if (want < 1 || want > 8) {
        fprintf(stderr, "subret takes 1..8 occasions per vblank period.\n");
        IOObjectRelease(peer);
        return 2;
    }

    /* RULE 39, ENFORCED RATHER THAN DOCUMENTED - and this is the MIRROR of
     * `tickset`'s refusal, which is the point of having two tools. Interior
     * occasions are placed only while the VBLANK drives; below native the
     * vblank hands retirement back (`rb_retire_vbl_suits()`), the timer owns
     * the quantum, and `tickset` is the dial that moves it. Setting this one
     * there would report success and change nothing, which looks exactly like
     * a null result. */
    if (src == 0) {
        fprintf(stderr,
                "\nREFUSED: the TIMER is driving retirement, so interior\n"
                "occasions are not placed at all and this dial is inert. An\n"
                "A/B here measures nothing and looks exactly like a null\n"
                "result (rule 39).\n\n"
                "This happens in a sub-native display mode, where the vblank\n"
                "cannot supply the retire RATE and hands the job back (2zf).\n"
                "In that regime the quantum IS the timer period:\n\n"
                "    build/out/tickset 2084     # 2C8's vbl/8 at 60 Hz\n\n"
                "Switch to the native mode (build/out/modeset 100) to use\n"
                "this one, and check `vblret_src` reads 1 first.\n");
        IOObjectRelease(peer);
        return 5;
    }

    NSDictionary *set = @{ @"RBGPURetireSub": @(want) };
    kern_return_t kr = IORegistryEntrySetCFProperties(
        peer, (__bridge CFDictionaryRef)set);
    printf("\nset RBGPURetireSub(%ld) -> 0x%x\n", want, kr);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr,
                "the write did not take. 0xe00002c2 is a bad value (the kext\n"
                "REFUSES rather than clamping, so an A/B can never silently\n"
                "run the A side twice; `vblret_subrefuse` carries what it\n"
                "refused); 0xe00002c7 means the peer does not know this key -\n"
                "wrong plugin build, check the UUID.\n");
        IOObjectRelease(peer);
        return 1;
    }

    /* Verify from the registry, not from kr (2A3) - and WAIT FOR A FRESH
     * SNAPSHOT by polling the monotonic set-counter past its pre-set value.
     * `RBGPUDiag` is a memcpy'd snapshot on a 250 ms throttle that DROPS
     * rather than defers (2ez), so a fixed sleep reads inside the window and
     * returns the previous value - which on boot 414 made four of seven
     * `tickset` calls report MISMATCH while the writes had all landed (2C5).
     * Polling the counter proves the snapshot is FRESH as well as proving the
     * call arrived, and a stale read cannot distinguish those. */
    /* 2D1: POLLING IS NOT ENOUGH, AND THAT IS NOT A TOOL BUG - IT IS THE
     * SNAPSHOT NEVER REFRESHING.
     *
     * `RBGPUDiag` is republished only by `performRefreshDiag()`, which runs
     * only when something calls `scheduleRefreshDiag()` - and that drops a
     * request inside its 250 ms window without re-arming (2ez's throttle,
     * which stays). The refresh is also the ONLY caller of
     * `rbgpu_retire_publish_2zd()`, so its own key writes are dropped in turn
     * and the chain terminates. On a quiet desktop nothing refreshes at all.
     *
     * Boot 426: `subret 4` LANDED (`vblret_subrecv` 4, `vblret_sub` 4, the
     * subdivision running at 3 interior retires per frame) and this poll read
     * a frozen snapshot for 3 s, reported "the set did not arrive" and took
     * `paired.sh` down with it through the `&&`. Boot 424 passed only because
     * probes were logging. So 2C5's "poll the set-counter, strictly better
     * than a longer sleep" is the SAME premise with a loop around it.
     *
     * The kext fix is `refreshDiagSoon()`, which defers instead of dropping.
     * This is the USERSPACE half, so the tool works against a plugin that
     * predates it: re-issue the SAME value once the throttle window has
     * certainly passed. A second write more than 250 ms after the first is
     * not dropped, so it publishes - and re-setting a dial to the value it
     * already holds changes nothing on the chip, only the monotonic counter
     * this is waiting on. Harmless by construction, which is why it is safe
     * to do inside a verification. */
    long nset = sets, now = -1;
    int kicks = 0;
    for (int i = 0; i < 60 && nset <= sets; i++) {   /* up to ~6 s */
        usleep(100000);
        nset = diagNum(peer, @"vblret_subsets", -1);
        if (nset <= sets && (i % 4) == 3 && kicks < 10) {
            /* >=400 ms since the last write: past the 250 ms window. */
            (void)IORegistryEntrySetCFProperties(
                peer, (__bridge CFDictionaryRef)set);
            kicks++;
        }
    }
    if (nset <= sets) {
        fprintf(stderr,
                "`vblret_subsets` did not advance past %ld in 6 s, after %d\n"
                "re-issues of the same value to force a publish. The set did\n"
                "not arrive, or the diag snapshot is not refreshing at all.\n"
                "Check `vblret_subrecv` - if it reads %ld the call DID arrive\n"
                "and only the publish is stuck, so the dial is armed and a\n"
                "measurement against it is valid.\n", sets, kicks, want);
        IOObjectRelease(peer);
        return 3;
    }
    if (kicks) {
        printf("(took %d re-issue%s to force a publish - the diag snapshot\n"
               " only refreshes when something schedules it; 2D1)\n",
               kicks, kicks == 1 ? "" : "s");
    }
    now = diagNum(peer, @"vblret_sub", -1);
    printf("vblret_sub now reads %ld   (sets %ld)\n", now, nset);
    if (now != want) {
        fprintf(stderr,
                "MISMATCH: the setter succeeded, `vblret_subsets` advanced\n"
                "%ld -> %ld so the call DID arrive, and the fresh snapshot\n"
                "still reports %ld. That is a real disagreement, not the\n"
                "stale-snapshot race. Do not run an A/B against it.\n",
                sets, nset, now);
        IOObjectRelease(peer);
        return 3;
    }

    /* The EFFECT, not just the dial (rule 20): with sub > 1 the interior
     * occasions must actually be firing, and `subret` climbing is the only
     * thing that says so. A dial that reads 4 with no interior retires is the
     * 2A9 shape - the feature on and the mechanism inert. */
    if (want > 1) {
        /* 2D0: POLL FOR A CHANGE, NEVER SLEEP-THEN-COMPARE.
         *
         * The first version slept 600 ms and compared two reads of
         * `RBGPUDiag`. That dictionary is a memcpy'd SNAPSHOT on a 250 ms
         * throttle that DROPS requests rather than deferring them (2ez), and
         * nothing in a quiet moment schedules a refresh - so on boot 424 both
         * reads returned the SAME stale snapshot and this printed
         * "NOT climbing" while `vblret_subret` was in fact advancing at
         * 438/s, exactly 3 per vblank. It reported the mechanism dead and
         * aborted a run of the experiment it exists to protect.
         *
         * That is rule 31 - "a counter that has not moved YET is not a
         * counter that did not move" - and it is the same defect 2C5 fixed in
         * the DIAL check of this very file, reintroduced fifteen lines later
         * in the EFFECT check. The dial check was right because it polls a
         * monotonic counter until it passes a known value; so does this now,
         * and a timeout is reported as a timeout rather than as a verdict. */
        const long t0 = diagNum(peer, @"vblret_subret", -1);
        long t1 = t0;
        int ms = 0, ekicks = 0;
        for (int i = 0; i < 60 && t1 <= t0; i++) {   /* up to ~6 s */
            usleep(100000);
            ms += 100;
            t1 = diagNum(peer, @"vblret_subret", -1);
            /* 2D1 again, for the same reason: the interior retires are real
             * but unpublished until something schedules a refresh. */
            if (t1 <= t0 && (i % 4) == 3 && ekicks < 10) {
                (void)IORegistryEntrySetCFProperties(
                    peer, (__bridge CFDictionaryRef)set);
                ekicks++;
            }
        }
        if (t1 > t0) {
            printf("interior retires %ld -> %ld in %d ms   (climbing)\n",
                   t0, t1, ms);
        } else {
            fprintf(stderr,
                    "\nWARNING: the dial reads %ld and `vblret_subret` did not\n"
                    "advance in 4 s. Either the subdivision is inert or the\n"
                    "diag snapshot is not refreshing at all - and those are\n"
                    "NOT the same thing, so check both before believing it:\n"
                    "  `vblret_subns`  zero = the framebuffer reported no\n"
                    "                  vblank period (an older fbkext build);\n"
                    "                  nonzero = the subdivision is placed.\n"
                    "  `vblret_sublate` counts slots genuinely MISSED.\n"
                    "  `vblret_subfull` counts runs COMPLETING - about one per\n"
                    "                  vblank is correct and not a fault.\n"
                    "Do not read a ladder against a dial you could not\n"
                    "verify.\n", want);
            IOObjectRelease(peer);
            return 6;
        }
    }

    IOObjectRelease(peer);
    return 0;
}
