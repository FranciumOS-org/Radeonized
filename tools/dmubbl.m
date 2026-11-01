/* SPDX-License-Identifier: MIT */
/* 2zn: read the DMCUB inbox findings, and send one ABM command.
 *
 * WHY A TOOL AND NOT THE SLIDER. 2zl identified the real brightness route -
 * on DCN 3.1 the level goes to the DMCUB as an ABM command, never to
 * BL_PWM_CNTL from the CPU - and boot 271 measured the ring free. What is
 * NOT known is whether the command this project builds is the one firmware
 * 4.0.15.47 accepts, and whether ABM is bound to the pipe. Wiring that to
 * the slider would make the panel's behaviour a function of two writes at
 * once, which is how 2zj's gap survived four milestones (rule 45). So each
 * command is sent by name, one at a time, with the whole picture printed
 * before and after.
 *
 * ONE BOOT DOES ALL OF IT:
 *   dmubbl                 - read-only: the locate, the scan control, counters
 *   dmubbl 65536           - SET_BACKLIGHT at full scale
 *   dmubbl 8192            - SET_BACKLIGHT at 12.5 %
 *   dmubbl pipe            - SET_PIPE (bind ABM to OTG0) - only if the level
 *                            alone does nothing
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

static long num(io_service_t fb, const char *key)
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
        } else if (CFGetTypeID(v) == CFBooleanGetTypeID()) {
            out = CFBooleanGetValue((CFBooleanRef)v) ? 1 : 0;
        }
        CFRelease(v);
    }
    return out;
}

static const char *locWhy(long c)
{
    switch (c) {
    case 0: return "OK";
    case 1: return "no BAR5 map (is -rbgpufbbacklight on?)";
    case 2: return "INBOX1_BASE_ADDRESS is not CW4's base";
    case 3: return "INBOX1_SIZE is not 8192";
    case 4: return "CW4 window cannot hold the ring";
    case 5: return "CW4_OFFSET pair is zero - no mailbox placed";
    case 6: return "the derived address is not inside the carve-out";
    case 7: return "the ring is not 64-byte aligned";
    case 8: return "located but NOT mappable";
    default: return "unknown";
    }
}

static const char *sendWhy(long c)
{
    switch (c) {
    case 0: return "sent";
    case 1: return "not armed - boot with -rbgpufbdmubbl";
    case 2: return "no ring mapping";
    case 3: return "the locate refused";
    case 4: return "WPTR or RPTR is not a legal ring position";
    case 5: return "the ring is full";
    case 6: return "the level was out of range";
    default: return "unknown";
    }
}

/* The scan IS the control for the address derivation (rule 26). slots ==
 * plausible and non-zero means the 37 commands the firmware wrote at
 * bring-up are being read, which a wrong address does not produce. */
static void dump(io_service_t fb)
{
    long slots = num(fb, "RBFBDmubScanSlots");
    long plaus = num(fb, "RBFBDmubScanPlaus");
    long loc   = num(fb, "RBFBDmubLocRefuse");

    printf("armed            %ld   (-rbgpufbdmubbl)\n",
           num(fb, "RBFBDmubBlArmed"));
    printf("locate           %ld  %s\n", loc, locWhy(loc));
    printf("  CW4 base/top   0x%lx .. 0x%lx\n",
           num(fb, "RBFBDmubCw4Base"), num(fb, "RBFBDmubCw4Top"));
    printf("  CW4 offset     0x%lx_%08lx\n",
           num(fb, "RBFBDmubCw4OffHi"), num(fb, "RBFBDmubCw4OffLo"));
    printf("  FB loc/offset  0x%lx / 0x%lx\n",
           num(fb, "RBFBDmubFbLocation"), num(fb, "RBFBDmubFbOffset"));
    printf("  ring pa        0x%lx  (carve-out + 0x%lx)\n",
           num(fb, "RBFBDmubRingPA"), num(fb, "RBFBDmubRingFbOff"));
    printf("  map route      %ld   (1 = BAR0 sub-range, 2 = physical)\n",
           num(fb, "RBFBDmubRoute"));
    printf("scan             %ld slots, %ld plausible, %ld ABM, %ld bytes\n",
           slots, plaus, num(fb, "RBFBDmubScanAbm"),
           num(fb, "RBFBDmubScanBytes"));
    if (slots > 0 && slots == plaus) {
        printf("  -> CONTROL PASSES: every slot the firmware wrote reads as a\n"
               "     dmub_cmd_header, which a wrong address does not do.\n");
    } else if (slots > 0) {
        printf("  -> CONTROL FAILS: %ld of %ld slots are not headers. Do NOT\n"
               "     send anything; the derivation is wrong.\n",
               slots - plaus, slots);
    } else {
        printf("  -> nothing scanned. The ring was not mapped or the pointers\n"
               "     were not legal positions - read the locate line.\n");
    }
    printf("sends ok/refused %ld / %ld   last refusal %ld %s\n",
           num(fb, "RBFBDmubSendsOk"), num(fb, "RBFBDmubSendsRefused"),
           num(fb, "RBFBDmubSendRefuse"),
           sendWhy(num(fb, "RBFBDmubSendRefuse")));
    printf("  backlight/pipe %ld / %ld   last level %ld\n",
           num(fb, "RBFBDmubBlSends"), num(fb, "RBFBDmubPipeSends"),
           num(fb, "RBFBDmubBlLast"));
    printf("  acks/timeouts  %ld / %ld   last WPTR 0x%lx\n",
           num(fb, "RBFBDmubAcks"), num(fb, "RBFBDmubAckTimeouts"),
           num(fb, "RBFBDmubSendWptr"));
    printf("  -> an ack is RPTR reaching our WPTR: the firmware CONSUMED the\n"
           "     command. That is not the panel moving - the panel is the\n"
           "     deliverable and your eyes are its only instrument.\n");
}

int main(int argc, char **argv)
{
    io_service_t fb = findFB();
    if (!fb) {
        fprintf(stderr, "no RBAMDFramebuffer - is the framebuffer armed?\n");
        return 1;
    }

    dump(fb);

    if (argc < 2) {
        printf("\nread-only. to send one command:\n"
               "  dmubbl <0..65536>   SET_BACKLIGHT, u16.16 (65536 = full)\n"
               "  dmubbl pipe         SET_PIPE, bind ABM to OTG0\n");
        IOObjectRelease(fb);
        return 0;
    }

    NSDictionary *d;
    if (strcmp(argv[1], "pipe") == 0) {
        d = @{ @"RBFBDmubPipe": @(1) };
        printf("\nsending SET_PIPE (bind ABM to OTG0)...\n");
    } else {
        long want = atol(argv[1]);
        d = @{ @"RBFBDmubBl": @(want) };
        printf("\nsending SET_BACKLIGHT %ld (%.1f %%)...\n",
               want, (double)want * 100.0 / 65536.0);
    }

    kern_return_t kr = IORegistryEntrySetCFProperties(
        fb, (__bridge CFDictionaryRef)d);
    printf("-> 0x%x%s\n", kr, kr == KERN_SUCCESS ? " (the push returned OK)" : "");
    printf("\nthe kext logged the refusal code and the WPTR it wrote. Rule 31:\n"
           "the counters above are published periodically, so re-run with no\n"
           "argument in a second or two rather than reading an unchanged\n"
           "number as a failure.\n");

    IOObjectRelease(fb);
    return kr == KERN_SUCCESS ? 0 : 1;
}
