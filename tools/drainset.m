/* SPDX-License-Identifier: MIT */
/* 2C2: how many times one vblank polls the GPU retirement.
 *
 * WHAT THIS IS FOR. Boot 409 ran `metal_probe --queues N` twelve times and
 * every run gave the same ladder: queue 0's first command buffer starts
 * 13.3-14.5 ms after commit, and each queue after it starts +6.9 to +7.2 ms
 * later, with the absolute slots landing at the same times whether you ask
 * for 2, 4 or 8 queues. `RBFBVBLPeriodNs` is 6952507, so the spacing IS the
 * vblank period, and `stamp_calls` 42337 against `stamp_completed` 42314 says
 * about one stamp retires per call. We poll `checkTimestamps()` once per
 * vblank, so N queues with pending work need N vblanks and nothing overlaps.
 *
 * That is invisible to one steady stream - 144 retires a second is what the
 * compositor wants - and it is exactly what an app pays when it needs several
 * units of GPU work finished BEFORE it can draw its first frame.
 *
 * HOW TO READ IT. The instrument is metal_probe, not this tool and not a
 * counter:
 *
 *     drainset 1 && metal_probe --queues 8      # the A side, today
 *     drainset 8 && metal_probe --queues 8      # the B side
 *
 * and ALTERNATE them several times rather than running a block of each: boot
 * 292 drifted ~15 % inside itself, which is the whole reason rule 44 now says
 * an A/B inside one boot is only valid interleaved.
 *
 * If the +6.95 ms ladder collapses, the serialisation was ours and this is
 * the fix. If the ladder is unchanged with `vblret_extra` climbing, then the
 * extra polls happened and did nothing - `checkTimestamps()` already drains
 * what it can, the one-per-vblank is downstream in Apple's `advance()`, and
 * the next step is a different function. Either way one boot answers it.
 *
 * WHAT MIGHT GO WRONG. This runs on the framebuffer's vblank thread_call
 * (rule 31): 8 is 1152 calls a second where today is 144. 2zc measured that
 * running retirement MORE often can cost a third of the frame rate when two
 * drivers contend, so check the frame rate as well as the ladder -
 * `tools/paired.sh` - and do not leave it high because the ladder looked
 * better. Nothing here writes to the chip; `drainset 1` and a reboot both
 * restore today's behaviour.
 *
 * Needs root.
 */
#import <Foundation/Foundation.h>
#import <IOKit/IOKitLib.h>

int main(int argc, char **argv)
{
    io_service_t peer = IOServiceGetMatchingService(
        kIOMainPortDefault, IOServiceMatching("RembrandtGPUPeer"));
    if (!peer) {
        fprintf(stderr, "no RembrandtGPUPeer - is the accelerator armed?\n");
        return 1;
    }

    /* The dial's live value comes from the diag table, which is the only
     * place the plugin publishes it - read it rather than trusting a return
     * code (rule 13). */
    CFTypeRef d = IORegistryEntryCreateCFProperty(peer, CFSTR("RBGPUDiag"),
                                                  kCFAllocatorDefault, 0);
    long live = -1, extra = -1, sets = -1;   /* 2C4: drain_sets */
    if (d && CFGetTypeID(d) == CFDictionaryGetTypeID()) {
        NSDictionary *nd = (__bridge NSDictionary *)d;
        NSNumber *v = nd[@"vblret_drain"];
        NSNumber *e = nd[@"vblret_extra"];
        NSNumber *k = nd[@"drain_sets"];
        if (v) { live  = [v longValue]; }
        if (e) { extra = [e longValue]; }
        if (k) { sets  = [k longValue]; }
    }
    if (d) { CFRelease(d); }

    printf("retire drain is %ld poll%s per vblank", live, live == 1 ? "" : "s");
    if (extra >= 0) { printf("   (extra polls so far: %ld)", extra); }
    printf("\n");

    if (argc < 2) {
        printf("\nto change it: drainset <1..8>\n"
               "  1 = today's behaviour and the A side.\n"
               "  measure with:  build/out/metal_probe --queues 8\n"
               "  ALTERNATE the sides, do not run a block of each "
               "(rule 44).\n");
        IOObjectRelease(peer);
        return 0;
    }

    const long want = atol(argv[1]);
    if (want < 1 || want > 8) {
        fprintf(stderr, "drainset takes 1..8.\n");
        IOObjectRelease(peer);
        return 2;
    }

    NSDictionary *set = @{ @"RBGPURetireDrain": @(want) };
    kern_return_t kr = IORegistryEntrySetCFProperties(
        peer, (__bridge CFDictionaryRef)set);
    printf("set RBGPURetireDrain(%ld) -> 0x%x\n", want, kr);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr,
                "the write did not take. 0xe00002c2 is a bad value (the kext "
                "REFUSES\nrather than clamping, so an A/B can never silently "
                "run the A side twice);\n0xe00002c7 means the peer does not "
                "know this key - wrong plugin build.\n");
        IOObjectRelease(peer);
        return 1;
    }

    /* 2C4 FIXES THIS, AND IT IS WHY 2C2's A/B DEGENERATED - AT LEAST IN PART.
     *
     * The comment here used to say "the peer publishes this key immediately
     * on the set, so no publish-interval wait is needed". That is false.
     * `RBGPUDiag` is a memcpy'd SNAPSHOT published by `performRefreshDiag()`,
     * and `scheduleRefreshDiag()` permits at most one refresh per 250 ms and
     * DROPS a request inside that window instead of deferring it (the
     * throttle is 2ez's fix for a crawl indistinguishable from a wedge, so it
     * stays). A 200 ms sleep reads INSIDE that window and returns the
     * previous value.
     *
     * Boot 414 demonstrated it on the tick dial: four of seven sets reported
     * MISMATCH while `tick_sets` proved all seven writes had landed. So poll
     * the monotonic counter until it passes its pre-set value - that proves
     * the snapshot is FRESH as well as proving the call arrived, which a
     * stale read cannot distinguish.
     *
     * NOTE, and deliberately not overstated: this does NOT establish that it
     * is the whole of 2C2's drain bug. `vblret_drain` has a SECOND writer -
     * `rbgpu_retire_publish_2zd()` republishes it from the live global
     * several times a second - so a stale read of that key should not have
     * persisted the way boot 410's did. The drain bug stays open and
     * `drain_recv`/`drain_want`/`drain_sets` are still what settles it. */
    long now = -1, nset = sets;
    for (int i = 0; i < 30 && (sets < 0 || nset <= sets); i++) {  /* ~3 s */
        usleep(100000);
        d = IORegistryEntryCreateCFProperty(peer, CFSTR("RBGPUDiag"),
                                            kCFAllocatorDefault, 0);
        if (d && CFGetTypeID(d) == CFDictionaryGetTypeID()) {
            NSNumber *k = ((__bridge NSDictionary *)d)[@"drain_sets"];
            NSNumber *v = ((__bridge NSDictionary *)d)[@"vblret_drain"];
            if (k) { nset = [k longValue]; }
            if (v) { now  = [v longValue]; }
        }
        if (d) { CFRelease(d); }
        if (sets < 0 && now == want) { break; }  /* pre-2C3 plugin: no counter */
    }

    printf("vblret_drain now reads %ld   (drain_sets %ld -> %ld)\n",
           now, sets, nset);
    if (now != want) {
        fprintf(stderr,
                "MISMATCH: the setter succeeded and a FRESH snapshot still\n"
                "reports %ld. `drain_sets` %ld -> %ld says whether the call\n"
                "arrived. This is no longer the stale-snapshot race (2C4) -\n"
                "do not run an A/B against it.\n", now, sets, nset);
        IOObjectRelease(peer);
        return 3;
    }

    IOObjectRelease(peer);
    return 0;
}
