/* SPDX-License-Identifier: MIT */
/* 2A2: turn the 2xo kIOReturnNotReady hold on or off while the desktop runs.
 *
 * The hold answers kIOReturnNotReady to getAttribute('amtr') while our flip is
 * still unlatched, which makes Apple's isTransactionComplete wait (2x6). 2A1
 * measured it owning ~21 % of the transaction interval, matching the 27 % 2xo
 * recorded when it was introduced - so it is a TRADE and this is a dial, not a
 * removal: with it off, 2xo's whole-frame flicker is expected back.
 *
 * A property and not a boot-arg because an A/B then costs nothing instead of
 * one reboot per side (2zm), and because the recovery from the flicker is a
 * command rather than a power cycle.
 *
 * Needs root. Writing it also zeroes the frame-time window, so a budget is
 * never a blend of both sides.
 */
#import <Foundation/Foundation.h>
#import <IOKit/IOKitLib.h>

int main(int argc, char **argv)
{
    io_service_t fb = IOServiceGetMatchingService(
        kIOMainPortDefault, IOServiceMatching("RBAMDFramebuffer"));
    if (!fb) {
        fprintf(stderr, "no RBAMDFramebuffer - is the framebuffer armed?\n");
        return 1;
    }

    CFTypeRef cur = IORegistryEntryCreateCFProperty(
        fb, CFSTR("RBFBAmtrNotReady"), kCFAllocatorDefault, 0);
    int on = -1;
    if (cur) {
        if (CFGetTypeID(cur) == CFBooleanGetTypeID()) {
            on = CFBooleanGetValue((CFBooleanRef)cur) ? 1 : 0;
        }
        CFRelease(cur);
    }
    printf("2xo hold is currently %s\n",
           on == 1 ? "ON" : on == 0 ? "OFF" : "unreadable");

    if (argc < 2) {
        printf("to change it: holdset <0|1>   (0 = off, expect 2xo's "
               "flicker)\n");
        IOObjectRelease(fb);
        return 0;
    }

    const long want = atol(argv[1]);
    if (want != 0 && want != 1) {
        fprintf(stderr, "holdset takes 0 or 1.\n");
        IOObjectRelease(fb);
        return 2;
    }
    NSDictionary *d = @{ @"RBFBNotReady": @(want) };
    kern_return_t kr = IORegistryEntrySetCFProperties(
        fb, (__bridge CFDictionaryRef)d);
    printf("set RBFBNotReady(%ld) -> 0x%x%s\n", want, kr,
           kr == KERN_SUCCESS ? " (OK, frame-time window zeroed)" : "");
    if (kr == KERN_SUCCESS && want == 0) {
        printf("the hold is OFF: 2xo's whole-frame flicker is expected, and\n"
               "whether it appears is part of the measurement. `holdset 1`\n"
               "puts it back with no reboot.\n");
    }
    IOObjectRelease(fb);
    return kr == KERN_SUCCESS ? 0 : 1;
}
