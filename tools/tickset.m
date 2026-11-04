/* SPDX-License-Identifier: MIT */
/* 2C4: the retire-tick period as a runtime dial - the lever 2C2 missed.
 *
 * WHAT 2C2 GOT WRONG. Boot 409/410 measured `metal_probe --queues N`
 * releasing one queue per ~6.95 ms and identified that step as the VBLANK
 * period, because `RBFBVBLPeriodNs` read 6952507 on those boots and matched
 * it to three figures. That identification could not have been made there:
 * the retire tick is 6944 us, tuned in 2xe to the panel's measured 143.83 Hz,
 * so the vblank period and the tick period are 0.12 % apart and no reading at
 * native can tell them apart. Rule 18 - two numbers agreeing is not an
 * identification - applied to the two candidates rather than to one.
 *
 * BOOT 413 SEPARATES THEM. It runs the 60 Hz mode, so the vblank period is
 * 16669443 ns while the tick is still 6944 us, and retirement is TIMER-driven
 * (`vblret_src` 0: `rb_retire_vbl_suits()` hands sub-native modes back,
 * since the vblank cannot supply 144 retires/s at 60 Hz). `--queues 8` spread
 * 56.009 ms there - against 55.295-56.308 ms at native on boot 410. Had the
 * vblank been the quantum, 60 Hz would have cost ~117 ms. It did not move.
 *
 * So the quantum is the RETIRE OCCASION, whoever supplies it, and when the
 * timer supplies it the period is this dial.
 *
 * HOW TO READ IT. The instrument is metal_probe; this tool only moves the
 * dial and proves the move landed.
 *
 *     tickset 6944 && metal_probe --queues 8      # A: today
 *     tickset 3472 && metal_probe --queues 8      # B: half the period
 *
 * and ALTERNATE, several times each - boot 292 drifted ~15 % inside itself,
 * which is why rule 44 only accepts an interleaved single-boot A/B.
 *
 * SAID BEFORE THE READING. If the quantum is the retire occasion, B halves
 * the eight-queue spread to ~28 ms. If the spread does not move, the tick
 * only correlated and the pacing is elsewhere - a different answer, not a
 * null one, and it would point back at the submit side that 2C3's `adv_calls`
 * reading is built to decide.
 *
 * WHY THE TOOL PRINTS `vblret_src` FIRST, AND REFUSES. While the VBLANK
 * drives retirement, `rb_retire_timer_period_us()` runs this timer at the
 * 50 ms WATCHDOG period instead, so the dial is INERT and an A/B would
 * measure nothing while looking exactly like a null result. That is rule 39 -
 * a reading taken in a configuration that cannot exhibit the effect does not
 * locate anything - and it is the shape 2A9's blind instrument had. So a
 * `vblret_src` of 1 is a REFUSAL here, with the reason named, not a warning
 * printed under a number.
 *
 * WHAT MIGHT GO WRONG. 2zc's dose-response is the cost side: 2000 us gave
 * 51.33 sets/s and 13888 gave 49.2 against 74.6 at 6944, so both directions
 * cost about a third of the frame rate and 6944 is an interior optimum FOR
 * FRAME RATE. This experiment is about a different quantity - concurrency
 * latency - so expect the frame rate to get worse while the ladder gets
 * better, read `tools/paired.sh` as well as the ladder, and do not leave the
 * tick fast because the ladder looked good. Nothing here writes to the chip;
 * `tickset 6944` and a reboot both restore today's behaviour.
 *
 * Needs root.
 */
#import <Foundation/Foundation.h>
#import <IOKit/IOKitLib.h>

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

    /* Read everything first, from the table, never from a return code
     * (rule 13). `tick_sets` is how a call that never arrived is told from
     * one that arrived and was lost - 2C2's drain bug spent a milestone in
     * exactly that ambiguity. */
    const long live  = diagNum(peer, @"tick_us",   -1);
    const long sets  = diagNum(peer, @"tick_sets", -1);
    const long src   = diagNum(peer, @"vblret_src", -1);
    const long armed = diagNum(peer, @"tick_armed", -1);

    printf("retire tick is %ld us", live);
    if (live > 0) { printf("  (%ld.%02ld Hz)", 1000000 / live,
                           (100000000 / live) % 100); }
    if (sets >= 0) { printf("   sets so far: %ld", sets); }
    printf("\n");
    printf("driver: %s\n",
           src == 1 ? "the VBLANK (vblret_src 1)" :
           src == 0 ? "this TIMER (vblret_src 0)" : "UNKNOWN (vblret_src absent)");

    if (live < 0) {
        fprintf(stderr,
                "\n`tick_us` is absent from the diag table. Before 2C4 it was\n"
                "published once from start() and this tool would have read the\n"
                "boot value forever (2A3); its absence here means the running\n"
                "plugin predates 2C4. Check the loaded UUID (rule 15).\n");
        IOObjectRelease(peer);
        return 1;
    }
    if (armed == 0) {
        fprintf(stderr,
                "\n`tick_armed` is 0 - the scheduler tick is not running at\n"
                "all, so this dial controls nothing. Nothing to A/B.\n");
        IOObjectRelease(peer);
        return 4;
    }

    if (argc < 2) {
        printf("\nto change it: tickset <500..1000000>\n"
               "  6944 = today's behaviour and the A side (2zc's optimum).\n"
               "  3472 = half, the B side of the 2C4 experiment.\n"
               "  measure with:  build/out/metal_probe --queues 8\n"
               "  ALTERNATE the sides, do not run a block of each "
               "(rule 44).\n");
        IOObjectRelease(peer);
        return 0;
    }

    const long want = atol(argv[1]);
    if (want < 500 || want > 1000000) {
        fprintf(stderr, "tickset takes 500..1000000 us.\n");
        IOObjectRelease(peer);
        return 2;
    }

    /* RULE 39, ENFORCED RATHER THAN DOCUMENTED. With the vblank driving, the
     * timer runs at the watchdog period and this value is never used, so the
     * A/B would compare two identical configurations and report a null. The
     * hosttest pins both branches of rb_retire_timer_period_us() so this is
     * not a guess about the kext's behaviour. */
    if (src == 1) {
        fprintf(stderr,
                "\nREFUSED: the VBLANK is driving retirement, so this timer is\n"
                "running at the 50 ms watchdog period and the tick value is\n"
                "unused. An A/B here measures nothing and looks exactly like a\n"
                "null result (rule 39).\n\n"
                "Run it in a timer-driven configuration instead: a sub-native\n"
                "display mode hands retirement back to the timer (the 60 Hz\n"
                "mode does - `vblret_src` reads 0 there), or boot with\n"
                "-rbgpunoretirevbl to drive natively from the timer.\n"
                "Check `vblret_src` reads 0 before relying on any reading.\n");
        IOObjectRelease(peer);
        return 5;
    }

    NSDictionary *set = @{ @"RBGPUTickUs": @(want) };
    kern_return_t kr = IORegistryEntrySetCFProperties(
        peer, (__bridge CFDictionaryRef)set);
    printf("set RBGPUTickUs(%ld) -> 0x%x\n", want, kr);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr,
                "the write did not take. 0xe00002c2 is a bad value (the kext\n"
                "REFUSES rather than clamping, so an A/B can never silently\n"
                "run the A side twice; `tick_refuse_why` says 1 too fast,\n"
                "2 too slow); 0xe00002c7 means the peer does not know this\n"
                "key - wrong plugin build, check the UUID.\n");
        IOObjectRelease(peer);
        return 1;
    }

    /* Verify from the registry, not from kr (2A3) - but WAIT FOR A FRESH
     * SNAPSHOT, which is what the first version of this tool got wrong.
     *
     * `RBGPUDiag` is not the live table: `performRefreshDiag()` publishes a
     * memcpy'd SNAPSHOT, and `scheduleRefreshDiag()` allows at most one
     * refresh per 250 ms and **drops** a request inside that window rather
     * than deferring it (2ez - the unthrottled version held the IOService
     * lock continuously and was indistinguishable from a wedge, so the
     * throttle stays). A single 200 ms sleep therefore reads INSIDE the
     * throttle window and returns the previous value - which on boot 414
     * made four of seven sets report MISMATCH while `tick_sets` proved all
     * seven writes had landed.
     *
     * So poll the monotonic counter until it passes the value read before
     * the set. That is strictly better than a longer sleep: it proves the
     * snapshot is FRESH as well as proving the call arrived, and the two are
     * what a stale read cannot distinguish. */
    long now = -1, nset = sets;
    for (int i = 0; i < 30 && nset <= sets; i++) {   /* up to ~3 s */
        usleep(100000);
        nset = diagNum(peer, @"tick_sets", -1);
    }
    now = diagNum(peer, @"tick_us", -1);
    if (nset <= sets) {
        fprintf(stderr,
                "`tick_sets` did not advance past %ld in 3 s. The set did not\n"
                "arrive, or the diag snapshot is not refreshing at all.\n", sets);
        IOObjectRelease(peer);
        return 3;
    }
    printf("tick_us now reads %ld us   (sets %ld)\n", now, nset);
    if (now != want) {
        fprintf(stderr,
                "MISMATCH: the setter succeeded, `tick_sets` advanced %ld -> %ld\n"
                "so the call DID arrive, and the fresh snapshot still reports\n"
                "%ld us. That is a real disagreement, not the stale-snapshot\n"
                "race this tool used to report. Do not run an A/B against it.\n",
                sets, nset, now);
        IOObjectRelease(peer);
        return 3;
    }

    IOObjectRelease(peer);
    return 0;
}
