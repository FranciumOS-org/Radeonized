// blset.m (2xs/2xy) - the ORACLE for "can we actually CHANGE the brightness?"
//
// WHY THIS EXISTS. Boot 249 proved the backlight MECHANISM: RBFBBlReady = Yes,
// RBFBBlRefuse = 0, one write, and RBFBBlReadback == RBFBBlLastCount == 22250,
// so the field was written and read back bit-identical. It did NOT prove the
// EFFECT. RBFBBlLastLevel came back equal to RBFBBlLevel0, meaning the only
// value ever written was the one the firmware had already put there - a no-op
// round trip. "The store reached the chip" and "the panel got brighter" are
// different claims and boot 249 only supports the first (rule 20 in its
// register form, and rule 14: a write that changes nothing and a write that
// is ignored look identical in a readback).
//
// The thing that normally moves it is the System Settings slider, which needs
// AppleBacklightDisplay to match the connection - that is 2xy, and 2xy needs
// its own boot. This asks for the same change the slider would, through the
// documented userspace entry point, so the effect can be settled with NO boot:
//
//   IODisplaySetFloatParameter(service, 0, CFSTR("brightness"), f)
//     -> IODisplay::setForKey -> IODisplay::doIntegerSet
//     -> IOFramebufferParameterHandler::doIntegerSet
//     -> fb->setAttributeForConnection(0, 'brig', value)
//
// which is exactly the chain 2xs read end to end. 'brig' is not a constant
// anybody chose: doIntegerSet byte-swaps the first four characters of the
// parameter NAME, and kIODisplayBrightnessKey is the string "brightness".
//
// Read-only with no argument. `blset <0.0..1.0>` performs the write, and the
// kext's own floor (RBFBBlFloor, 5120 of 65536 = 7.8 %) clamps it, so 0.0
// cannot produce a black panel - that bound is in the kext, not here, because
// a safety floor enforced by the caller is not a floor.
//
// build: make blset
#import <IOKit/IOKitLib.h>
#import <IOKit/graphics/IOGraphicsLib.h>
#import <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// 2xy: THE NAME IS NOT "brightness" AND THAT IS THE WHOLE POINT.
//
// ioreg shows our parameter in the AppleDisplay's IODisplayParameters as
// "brig" = {min 5120, max 65536, value 22784} - our floor, our scale, our
// value. IODisplayCopyParameters with kIODisplayBrightnessKey, which is the
// string "brightness", does not find it. Both are true and the difference is
// the key:
//
//   IOFramebufferParameterHandler builds that dictionary from the FourCCs we
//   returned in the 'parm' list, turning each 4-byte attribute into a
//   4-CHARACTER key. So our parameter is literally NAMED "brig".
//
//   The reverse direction is what 2xs read: doIntegerSet byte-swaps the first
//   four characters of the parameter NAME into the attribute. "brig" -> 'brig'
//   and "brightness" -> 'brig' too, because only the first four characters are
//   used. So the attribute is reachable under EITHER name...
//
//   ...but only if the name is a KEY THAT EXISTS in the dictionary a client
//   enumerates. A client asking for "brightness" by name finds nothing,
//   because the framebuffer-supplied entry is under "brig".
//
// So this prints every key it finds and tries both names, which is what turns
// "the slider is absent" from one fact into two separable ones (rule 14).
// 2zw/2zx: THE FIRMWARE GRAPHICS MODE IS PART OF THE CONFIGURATION, AND IT IS
// READABLE. Six milestones and ten boots went into a backlight register that
// was real, writable, uncontested and attached to NOTHING, because in hybrid
// (iGPU + dGPU) mode this chassis does not give the 680M's PWM block the
// panel's backlight. Switching the ASUS firmware to iGPU-only / Eco mode made
// the identical code work (boot 282). That is rule 47, and the whole cost of
// it was nobody asking which mode the machine was in.
//
// So ask, here, in the tool whose only job is brightness. The detector is the
// PCI bus and it is unambiguous: boot 282 in Eco mode has **zero** devices
// with vendor-id 0x10de - the firmware removes the dGPU from the bus
// altogether, it is not merely powered down. A present 0x10de therefore means
// hybrid mode, which is the configuration where the backlight write lands
// perfectly and the panel does not move.
//
// The match is ANCHORED on the vendor-id property (rule 26). A grep for
// "10de" over whole ioreg output returned 3 hits in a mode with no NVIDIA
// device on the bus at all, which is exactly the instrument error this
// project has paid for repeatedly.
//
// This is also the fallback for OTHER machines: the wiring differs per
// chassis, so the tool reports what it found rather than assuming this
// laptop's arrangement.
static int dgpuPresent(void)
{
    int found = 0;
    io_iterator_t it = IO_OBJECT_NULL;
    if (IOServiceGetMatchingServices(kIOMainPortDefault,
                                     IOServiceMatching("IOPCIDevice"),
                                     &it) != kIOReturnSuccess) {
        return -1;                      /* could not look, != "not there" */
    }
    io_service_t dev;
    while ((dev = IOIteratorNext(it))) {
        CFTypeRef v = IORegistryEntryCreateCFProperty(
            dev, CFSTR("vendor-id"), NULL, kNilOptions);
        if (v) {
            if (CFGetTypeID(v) == CFDataGetTypeID() &&
                CFDataGetLength((CFDataRef)v) >= 2) {
                const UInt8 *b = CFDataGetBytePtr((CFDataRef)v);
                /* little-endian: 0x10de reads as de 10 */
                if (b[0] == 0xde && b[1] == 0x10) { found = 1; }
            }
            CFRelease(v);
        }
        IOObjectRelease(dev);
    }
    IOObjectRelease(it);
    return found;
}

static void reportMode(void)
{
    const int d = dgpuPresent();
    if (d < 0) {
        printf("firmware graphics mode: UNKNOWN - could not enumerate "
               "IOPCIDevice\n");
        return;
    }
    if (d == 0) {
        printf("firmware graphics mode: iGPU-only / Eco "
               "(no 0x10de device on the PCI bus)\n"
               "  -> the backlight reaches the panel in this mode (2zw, "
               "boot 282).\n");
        return;
    }
    printf("firmware graphics mode: HYBRID - an NVIDIA device (0x10de) is on "
           "the PCI bus\n"
           "  *** THE BACKLIGHT WRITE WILL LAND AND THE PANEL WILL NOT "
           "MOVE. ***\n"
           "  This is not a bug in the driver and not a stale build. In this\n"
           "  mode the 680M's BL_PWM block does not drive the panel's\n"
           "  backlight, so every counter below will look perfect while\n"
           "  nothing changes - that cost six milestones and ten boots\n"
           "  (journal 2zj..2zw, rule 47).\n"
           "  Switch the ASUS firmware to iGPU-only / Eco mode.\n"
           "  Note the trade on this chassis: the HDMI port is wired to the\n"
           "  dGPU, and Eco mode takes the dGPU off the bus entirely - so it\n"
           "  is brightness OR HDMI, not both.\n");
}

static CFDictionaryRef findBl(CFDictionaryRef p, const char **which)
{
    static const char *const names[] = { kIODisplayBrightnessKey, "brig" };
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        CFStringRef k = CFStringCreateWithCString(NULL, names[i],
                                                  kCFStringEncodingUTF8);
        CFDictionaryRef d = k ? CFDictionaryGetValue(p, k) : NULL;
        if (k) CFRelease(k);
        if (d) {
            if (which) *which = names[i];
            return d;
        }
    }
    return NULL;
}

static void dumpParams(io_service_t disp)
{
    CFDictionaryRef p = NULL;
    if (IODisplayCopyParameters(disp, kNilOptions, &p) != kIOReturnSuccess || !p) {
        printf("  IODisplayCopyParameters failed - no parameters to read\n");
        return;
    }
    // Every key, so a missing one is a fact about this dictionary and not
    // about the one key we thought to ask for (rule 26).
    {
        CFIndex n = CFDictionaryGetCount(p);
        const void **keys = calloc((size_t)n, sizeof(void *));
        printf("  %ld parameter(s):", (long)n);
        if (keys) {
            CFDictionaryGetKeysAndValues(p, keys, NULL);
            for (CFIndex i = 0; i < n; i++) {
                char buf[64] = {0};
                CFStringGetCString((CFStringRef)keys[i], buf, sizeof(buf),
                                   kCFStringEncodingUTF8);
                printf(" %s", buf);
            }
            free(keys);
        }
        printf("\n");
    }
    const char *which = NULL;
    CFDictionaryRef b = findBl(p, &which);
    if (!b) {
        printf("  NO brightness parameter under either \"%s\" or \"brig\".\n",
               kIODisplayBrightnessKey);
        printf("  That is the 2xs arm failing, not 2xy: check RBFBBlArmed and\n"
               "  RBFBBlReady - the parameter is only appended when blReady.\n");
    } else {
        printf("  found it under the key \"%s\"\n", which);
        CFNumberRef v = CFDictionaryGetValue(b, CFSTR(kIODisplayValueKey));
        CFNumberRef mn = CFDictionaryGetValue(b, CFSTR(kIODisplayMinValueKey));
        CFNumberRef mx = CFDictionaryGetValue(b, CFSTR(kIODisplayMaxValueKey));
        long vv = 0, mnv = 0, mxv = 0;
        if (v)  CFNumberGetValue(v,  kCFNumberLongType, &vv);
        if (mn) CFNumberGetValue(mn, kCFNumberLongType, &mnv);
        if (mx) CFNumberGetValue(mx, kCFNumberLongType, &mxv);
        printf("  brightness: value %ld, min %ld, max %ld", vv, mnv, mxv);
        if (mxv > mnv) {
            printf("   (%.1f %% of range, floor is %.1f %% of full scale)",
                   100.0 * (double)(vv - mnv) / (double)(mxv - mnv),
                   100.0 * (double)mnv / (double)mxv);
        }
        printf("\n");
    }
    CFRelease(p);
}

int main(int argc, char **argv)
{
    io_iterator_t it = IO_OBJECT_NULL;
    if (IOServiceGetMatchingServices(kIOMainPortDefault,
                                     IOServiceMatching("IODisplayConnect"),
                                     &it) != kIOReturnSuccess) {
        printf("no IODisplayConnect at all\n");
        return 2;
    }

    reportMode();
    printf("\n");

    int n = 0;
    io_service_t conn;
    while ((conn = IOIteratorNext(it))) {
        io_iterator_t ci = IO_OBJECT_NULL;
        if (IORegistryEntryGetChildIterator(conn, kIOServicePlane, &ci) == kIOReturnSuccess) {
            io_service_t disp;
            while ((disp = IOIteratorNext(ci))) {
                io_name_t cls = {0};
                IOObjectGetClass(disp, cls);
                n++;
                printf("display %d: class %s\n", n, cls);
                // The class name IS the 2xy verdict: AppleDisplay means
                // IOBacklightDisplay::probe declined us, AppleBacklightDisplay
                // means it matched. Printed before the parameters so a reading
                // is always attributed to a configuration (rule 39).
                dumpParams(disp);

                if (argc > 1) {
                    float f = strtof(argv[1], NULL);
                    if (f < 0.0f || f > 1.0f) {
                        printf("  refusing %g - the parameter is 0.0..1.0\n", f);
                    } else {
                        // A SECOND ARGUMENT FORCES THE KEY (2zt). Until now
                        // this fell back to "brig" only when "brightness"
                        // FAILED - and 2y2 made "brightness" succeed, so the
                        // route boot 249 actually used has not been exercised
                        // since boot 249. The two keys reach DIFFERENT code:
                        // "brightness" is RBAMDBacklightParams.cpp, our own
                        // IODisplayParameterHandler added in 2y1; "brig" is
                        // IOFramebufferParameterHandler turning the 'parm'
                        // FourCC into a 4-character key and calling
                        // setAttributeForConnection. Same register at the end,
                        // not the same path to it - and the user's own
                        // recollection is that the panel moved on boot 249,
                        // before the slider was wired up at all.
                        const char *force = (argc > 2) ? argv[2] : NULL;
                        IOReturn r = kIOReturnUnsupported;
                        if (!force || strcmp(force, kIODisplayBrightnessKey) == 0) {
                            r = IODisplaySetFloatParameter(
                                disp, kNilOptions,
                                CFSTR(kIODisplayBrightnessKey), f);
                            printf("  set via \"%s\"(%g) -> 0x%x %s\n",
                                   kIODisplayBrightnessKey, f, r,
                                   r == kIOReturnSuccess ? "(OK)" : "(FAILED)");
                        }
                        if (force ? strcmp(force, "brig") == 0
                                  : r != kIOReturnSuccess) {
                            r = IODisplaySetFloatParameter(
                                disp, kNilOptions, CFSTR("brig"), f);
                            printf("  set via \"brig\"(%g)       -> 0x%x %s\n",
                                   f, r, r == kIOReturnSuccess ? "(OK)" : "(FAILED)");
                        }
                        if (force ? strcmp(force, "lin") == 0 : 0) {
                            r = IODisplaySetFloatParameter(
                                disp, kNilOptions,
                                CFSTR("linear-brightness"), f);
                            printf("  set via \"linear-brightness\"(%g) -> "
                                   "0x%x %s\n", f, r,
                                   r == kIOReturnSuccess ? "(OK)" : "(FAILED)");
                        }
                        if (r == kIOReturnSuccess) {
                            printf("  after:\n");
                            dumpParams(disp);
                            printf("  NOW CHECK THE KEXT, not this output: a\n"
                                   "  success here only says the call returned.\n"
                                   "    ioreg -rl -c RBAMDFramebuffer | grep -E "
                                   "'RBFBBlSets|RBFBBlLastLevel|RBFBBlLastCount|RBFBBlReadback'\n"
                                   "  RBFBBlSets must have INCREASED and\n"
                                   "  RBFBBlLastLevel must now differ from\n"
                                   "  RBFBBlLevel0 - otherwise the write never\n"
                                   "  reached setAttributeForConnection.\n");
                        }
                    }
                }
                IOObjectRelease(disp);
            }
            IOObjectRelease(ci);
        }
        IOObjectRelease(conn);
    }
    IOObjectRelease(it);

    if (n == 0) {
        printf("IODisplayConnect exists but has no IODisplay child\n");
        return 2;
    }
    if (argc == 1) {
        printf("\nread-only. to change it: blset <0.0..1.0>\n");
    }
    return 0;
}
