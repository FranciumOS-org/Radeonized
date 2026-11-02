/* SPDX-License-Identifier: MIT */
/* 2B1: turn DCN's SECOND PENDING FLIP SLOT on or off while the desktop runs.
 *
 * WHAT THIS IS FOR. 2A8 measured the 2xo hold at exactly one vblank of vsync
 * latch and 2B0 measured the latch distribution as bimodal with no early
 * tail, so neither a rate throttle nor a partial hold can exist - GIVEN ONE
 * PENDING SLOT. That premise has been read on every boot since 216 and never
 * questioned: DCSURF_FLIP_CONTROL2 reads 0x40, PENDING_MIN_TIME 64 with
 * SURFACE_TRIPLE_BUFFER_ENABLE CLEAR. This sets bit 10.
 *
 * WHAT IT MIGHT BUY. Two slots means programming an address no longer
 * discards an unlatched flip (boot 289 counted 81 and 26 of those per 20 s
 * window with the hold off, against 4 and 2 with it on), and there is one
 * more frame of pipeline between Apple accepting a transaction and that
 * surface reaching the panel - which is the slack 2xo's hazard needs, because
 * CoreDisplay releases the surface and signals readiness in ONE event and we
 * cannot separate them.
 *
 * WHAT IS NOT ESTABLISHED, said before you run it. Linux enables this only
 * behind its `enable_tri_buf` debug option and pairs a triple-buffered flip
 * with triplebuffer_lock/unlock on the timing generator instead of the normal
 * lock; we set the HUBP bit WITHOUT that protocol. The register write is
 * grouped right for DCN 3.1 (dcn31_hubp_funcs maps hubp2_enable_triplebuffer
 * verbatim - rule 46, UPSTREAM.md), the surrounding method is not fully
 * reproduced, and the failure mode is therefore unknown: a tear, a dropped
 * frame or a wedge. `tripleset 0` restores the firmware's own word, and a
 * reboot restores it unconditionally.
 *
 * A dial and not a boot-arg for 2zm's reason: the thing this is supposed to
 * buy is "hold OFF and no flicker", so it has to be A/B'd against holdset
 * inside one boot - four combinations, no reboots - and the only instrument
 * for the flicker is your own eyes (rule 45).
 *
 * Needs root. Writing it also zeroes the frame-time window.
 */
#import <Foundation/Foundation.h>
#import <IOKit/IOKitLib.h>

static long rdnum(io_service_t fb, CFStringRef key)
{
    CFTypeRef v = IORegistryEntryCreateCFProperty(fb, key,
                                                  kCFAllocatorDefault, 0);
    long out = -1;
    if (v) {
        if (CFGetTypeID(v) == CFNumberGetTypeID()) {
            long long t = 0;
            CFNumberGetValue((CFNumberRef)v, kCFNumberLongLongType, &t);
            out = (long)t;
        } else if (CFGetTypeID(v) == CFBooleanGetTypeID()) {
            out = CFBooleanGetValue((CFBooleanRef)v) ? 1 : 0;
        }
        CFRelease(v);
    }
    return out;
}

int main(int argc, char **argv)
{
    io_service_t fb = IOServiceGetMatchingService(
        kIOMainPortDefault, IOServiceMatching("RBAMDFramebuffer"));
    if (!fb) {
        fprintf(stderr, "no RBAMDFramebuffer - is the framebuffer armed?\n");
        return 1;
    }

    const long on   = rdnum(fb, CFSTR("RBFBHubpTriple"));
    const long live = rdnum(fb, CFSTR("RBFBHubpFlipCtl2"));
    const long orig = rdnum(fb, CFSTR("RBFBHubpTripleOrig"));
    const long hold = rdnum(fb, CFSTR("RBFBAmtrNotReady"));
    printf("second pending slot is %s   FLIP_CONTROL2 setup 0x%lx"
           "  (firmware word 0x%lx)\n",
           on == 1 ? "ON" : on == 0 ? "OFF" : "unreadable",
           live < 0 ? 0 : live, orig < 0 ? 0 : orig);
    printf("2xo hold is %s\n",
           hold == 1 ? "ON" : hold == 0 ? "OFF" : "unreadable");

    if (argc < 2) {
        printf("to change it: tripleset <0|1>\n"
               "  the combination worth trying is `holdset 0` + "
               "`tripleset 1`:\n"
               "  full rate with one more frame of pipeline. Watch the "
               "SCREEN,\n"
               "  with real window animations - animtest missed 2xo's "
               "flicker twice.\n");
        IOObjectRelease(fb);
        return 0;
    }

    const long want = atol(argv[1]);
    if (want != 0 && want != 1) {
        fprintf(stderr, "tripleset takes 0 or 1.\n");
        IOObjectRelease(fb);
        return 2;
    }

    NSDictionary *d = @{ @"RBFBTriple": @(want) };
    kern_return_t kr = IORegistryEntrySetCFProperties(
        fb, (__bridge CFDictionaryRef)d);
    printf("set RBFBTriple(%ld) -> 0x%x\n", want, kr);
    if (kr != KERN_SUCCESS) {
        /* The kext returns an IO error when the hardware readback says the
         * bit did not move, which is a different failure from a rejected
         * argument - say which, because "it did not work" is not a result
         * (rule 14). */
        fprintf(stderr,
                "the write did not take. 0xe00002be is 'HUBP not armed this "
                "boot';\n0xe00002bd means the bit did not move in the "
                "hardware readback.\nNothing was left half-applied - "
                "`tripleset 0` and a reboot both restore it.\n");
        IOObjectRelease(fb);
        return 1;
    }

    /* Verify from the REGISTRY after one publish interval, never from kr -
     * 2A3 cost an A/B phase to learn that kr says the setter ran, not that
     * the state is readable (rule 13/31). */
    sleep(1);
    const long now  = rdnum(fb, CFSTR("RBFBHubpTriple"));
    const long back = rdnum(fb, CFSTR("RBFBHubpTripleReadback"));
    const long ref  = rdnum(fb, CFSTR("RBFBHubpTripleRefused"));
    printf("RBFBHubpTriple now reads %s   hardware readback 0x%lx\n",
           now == 1 ? "ON" : now == 0 ? "OFF" : "unreadable",
           back < 0 ? 0 : back);
    if (ref > 0) {
        printf("NOTE: %ld refusal(s) - FLIP_CONTROL2 carried bits outside "
               "the three\ndocumented fields, so the RMW was declined "
               "(RBFBHubpTripleRefusedVal).\n", ref);
    }
    if (now != want) {
        fprintf(stderr,
                "MISMATCH: the setter succeeded and the registry reports "
                "%s.\nDo not run an A/B against this.\n",
                now == 1 ? "ON" : now == 0 ? "OFF" : "nothing");
        IOObjectRelease(fb);
        return 3;
    }
    if (want == 1) {
        printf("\nTwo pending slots now. Try `holdset 0` for full rate and "
               "WATCH THE\nSCREEN during real window animations. "
               "`tripleset 0` restores the\nfirmware's word; a reboot "
               "restores it unconditionally.\n");
    }

    IOObjectRelease(fb);
    return 0;
}
