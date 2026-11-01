/* SPDX-License-Identifier: MIT */
/* 2zm: move RBFBPubMs while the frame clock keeps running.
 *
 * WHY. Rule 44 wants two boots per side OR an A/B inside one boot, and this
 * project has only ever been able to do the first, because every dial it owns
 * is a boot-arg. 2xn/2zb/2zc spent three boots on three points of one
 * dose-response curve; 2zg then spent two more finding that boot-to-boot
 * variance exceeds within-boot variance - the one error a single-boot A/B
 * cannot make. This makes 2zk's throttle switchable live, so six runs, a
 * write, and six more settle it on one boot.
 *
 * It writes ONE key on ONE service and nothing else reaches the chip: the
 * kext's setProperties accepts only RBFBPubMs, bounded by the same
 * rb_pub_ms_ok() the boot-arg path uses, and the value sets how often an
 * instrument publishes. No register is written.
 *
 * Needs root, because IORegistryEntrySetCFProperties on a driver does.
 */
#import <Foundation/Foundation.h>
#import <IOKit/IOKitLib.h>

static io_service_t findFB(void)
{
    return IOServiceGetMatchingService(kIOMainPortDefault,
                                       IOServiceMatching("RBAMDFramebuffer"));
}

static long readBack(io_service_t fb, const char *key)
{
    CFTypeRef v = IORegistryEntryCreateCFProperty(
        fb, (__bridge CFStringRef)[NSString stringWithUTF8String:key],
        kCFAllocatorDefault, 0);
    long out = -1;
    if (v) {
        if (CFGetTypeID(v) == CFNumberGetTypeID()) {
            long long t = 0;
            CFNumberGetValue((CFNumberRef)v, kCFNumberLongLongType, &t);
            out = (long)t;
        }
        CFRelease(v);
    }
    return out;
}

int main(int argc, char **argv)
{
    io_service_t fb = findFB();
    if (!fb) {
        fprintf(stderr, "no RBAMDFramebuffer - is the framebuffer armed?\n");
        return 1;
    }

    /* Rule 15: say which binary is answering before changing anything. */
    printf("RBFBPubMs        %ld ms\n", readBack(fb, "RBFBPubMs"));
    printf("  hot calls      %ld\n",    readBack(fb, "RBFBPubHotCalls"));
    printf("  hot published  %ld\n",    readBack(fb, "RBFBPubHotDone"));
    printf("  sets ok/ref/other  %ld / %ld / %ld   last asked %ld\n",
           readBack(fb, "RBFBPubSetsOk"),
           readBack(fb, "RBFBPubSetsRefused"),
           readBack(fb, "RBFBPubSetsOther"),
           readBack(fb, "RBFBPubMsLast"));

    if (argc < 2) {
        printf("\nread-only. to change it: pubms <0..10000>   (0 = unthrottled, the A side)\n");
        IOObjectRelease(fb);
        return 0;
    }

    long want = atol(argv[1]);
    NSDictionary *d = @{ @"RBFBPubMs": @(want) };
    kern_return_t kr = IORegistryEntrySetCFProperties(
        fb, (__bridge CFDictionaryRef)d);
    printf("\nset RBFBPubMs(%ld) -> 0x%x%s\n", want, kr,
           kr == KERN_SUCCESS ? " (OK)" : "");

    /* Rule 31: the counters are published periodically, so do NOT read them
     * back here and call an unchanged value a failure. RBFBPubMs itself is
     * set synchronously, but it reaches the registry on the next publish -
     * which, with the throttle just reset, is the next frame-path call. */
    if (kr == KERN_SUCCESS) {
        printf("re-run with no argument in a second or two to see it; a value\n"
               "that has not appeared YET has not failed (rule 31).\n");
    }
    IOObjectRelease(fb);
    return kr == KERN_SUCCESS ? 0 : 1;
}
