/* SPDX-License-Identifier: MIT */
/* 2BA: read the sink's DPCD over DP AUX.
 *
 * WHAT IT DOES ON THE CHIP. One native AUX read on the engine bound to an HPD
 * block - by default HPD2, which 2B4 established is the Type-C port. A native
 * READ cannot change the sink's state, and the kext REFUSES rather than races
 * when AUX_ARB_CONTROL says the DMCUB owns the engine.
 *
 * WHAT IT NEEDS. `-rbgpufbaux` in boot-args. Without it the kext returns
 * kIOReturnNotReady and nothing is sent - an armed boot nobody pokes performs
 * no transaction at all.
 *
 * HOW TO READ THE RESULT. The reply CODE decides first (ACK/NACK/DEFER), then
 * the refusal code if there was no reply, and only then the bytes. DPCD 0x000
 * is the one block this tool interprets: revision, max link rate, max lane
 * count. A reply whose bytes are all identical is REPORTED AS SUSPECT rather
 * than decoded, because that is the signature of the one part of Linux's
 * sequence this project could not resolve (whether AUX_SW_DATA's index
 * advances on read while AUTOINCREMENT_DISABLE is set) - and a plausible
 * decode off a stuck index is exactly the kind of result that has cost this
 * project milestones.
 *
 * Needs root.
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

static const char *refusal(long code)
{
    switch (code) {
    case 0: return "none";
    case 1: return "NOT ARMED - add -rbgpufbaux to boot-args";
    case 2: return "no BAR5 map (check RBFBHubpRefuse)";
    case 3: return "NO AUX ENGINE IS BOUND TO THAT HPD BLOCK";
    case 4: return "the request was malformed (address or length)";
    case 5: return "that engine has AUX_EN clear - never brought up";
    case 6: return "THE DMCUB OWNS THE ENGINE - declined, not raced";
    case 7: return "a stale AUX_SW_DONE never fell";
    case 8: return "no AUX_SW_DONE after GO - the sink did not answer";
    case 9: return "DONE with a zero reply byte count";
    default: return "unknown";
    }
}

int main(int argc, char **argv)
{
    io_service_t fb = IOServiceGetMatchingService(
        kIOMainPortDefault, IOServiceMatching("RBAMDFramebuffer"));
    if (!fb) {
        fprintf(stderr, "no RBAMDFramebuffer - is the framebuffer armed?\n");
        return 1;
    }

    const long armed = rdnum(fb, CFSTR("RBFBAuxDpcdArmed"));
    const long hpdsink = rdnum(fb, CFSTR("RBFBHpdSink"));
    printf("DPCD read is %s", armed == 1 ? "ARMED" : "NOT ARMED");
    if (armed != 1) { printf("  (add -rbgpufbaux to boot-args)"); }
    printf("\n");
    printf("HPD sink mask 0x%02lx   (2B4: HPD2 is the Type-C port)\n",
           hpdsink < 0 ? 0 : hpdsink);

    unsigned long addr = 0x0;
    unsigned long len = 16;
    unsigned long hpd = 2;
    if (argc >= 2) { addr = strtoul(argv[1], NULL, 0); }
    if (argc >= 3) { len  = strtoul(argv[2], NULL, 0); }
    if (argc >= 4) { hpd  = strtoul(argv[3], NULL, 0); }

    if (argc < 2) {
        printf("\nusage: dpcdread [address [length [hpd]]]"
               "   (default 0x0 16 2)\n"
               "  0x000 is the receiver capability block: revision,\n"
               "  max link rate, max lane count - what link training needs.\n"
               "  A native READ only; it cannot change the sink.\n");
        IOObjectRelease(fb);
        return 0;
    }

    NSDictionary *props = @{ @"RBFBAuxHpd" : @(hpd) };
    kern_return_t kr = IORegistryEntrySetCFProperties(fb,
                                                      (__bridge CFTypeRef)props);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "selecting HPD%lu failed: 0x%x\n", hpd, kr);
        IOObjectRelease(fb);
        return 2;
    }

    NSDictionary *go = @{ @"RBFBAuxRead" : @(addr), @"RBFBAuxLen" : @(len) };
    kr = IORegistryEntrySetCFProperties(fb, (__bridge CFTypeRef)go);
    printf("\nread DPCD 0x%05lx, %lu bytes, on the engine bound to HPD%lu"
           " -> 0x%x\n", addr, len, hpd, kr);

    /* Rule 13/20: the verdict is the published reply, not the return code -
     * and rule 31: it is published periodically, so give it one interval. */
    sleep(1);
    const long inst = rdnum(fb, CFSTR("RBFBAuxDpcdInst"));
    const long ref  = rdnum(fb, CFSTR("RBFBAuxDpcdRefuse"));
    const long rep  = rdnum(fb, CFSTR("RBFBAuxDpcdReply"));
    const long n    = rdnum(fb, CFSTR("RBFBAuxDpcdBytes"));
    const long susp = rdnum(fb, CFSTR("RBFBAuxDpcdSuspect"));
    const long arb  = rdnum(fb, CFSTR("RBFBAuxDpcdArbSeen"));
    const long st   = rdnum(fb, CFSTR("RBFBAuxDpcdStatus"));
    const long att  = rdnum(fb, CFSTR("RBFBAuxDpcdAttempts"));
    const long oks  = rdnum(fb, CFSTR("RBFBAuxDpcdOks"));

    printf("  engine     AUX%ld   (chosen from AUX_HPD_SEL, not assumed)\n",
           inst);
    printf("  arb seen   0x%08lx   SW_STATUS 0x%08lx\n",
           arb < 0 ? 0 : arb, st < 0 ? 0 : st);
    printf("  attempts   %ld   acked-with-data %ld\n", att, oks);
    printf("  refusal    %ld - %s\n", ref, refusal(ref));

    if (ref != 0) {
        printf("\nNo transaction result. The refusal above is the finding.\n");
        IOObjectRelease(fb);
        return 3;
    }

    const char *rname = rep == 0 ? "ACK" : rep == 1 ? "NACK"
                      : rep == 2 ? "DEFER" : "unknown";
    printf("  reply      %ld (%s)   bytes %ld\n", rep, rname, n);

    if (rep != 0 || n <= 0) {
        printf("\nThe sink answered but did not ACK with data.\n"
               "A NACK or DEFER is still proof the channel works -\n"
               "it is the sink declining this address, not silence.\n");
        IOObjectRelease(fb);
        return 4;
    }

    printf("  bytes     ");
    unsigned char b[16] = {0};
    for (long i = 0; i < n && i < 16; i++) {
        char key[24];
        snprintf(key, sizeof(key), "RBFBAuxDpcd%02ld", i);
        CFStringRef k = CFStringCreateWithCString(NULL, key,
                                                  kCFStringEncodingUTF8);
        const long v = rdnum(fb, k);
        CFRelease(k);
        b[i] = (unsigned char)(v < 0 ? 0 : v);
        printf(" %02x", b[i]);
    }
    printf("\n");

    if (susp == 1) {
        printf("\nSUSPECT: every byte is identical. That is the signature of a\n"
               "stuck AUX_SW_DATA index - the one part of Linux's read\n"
               "sequence this project could not resolve - and NOT of a real\n"
               "DPCD block, whose revision, link rate and lane count differ.\n"
               "Nothing below is decoded, because a plausible decode off a\n"
               "stuck index is worse than no decode.\n");
        IOObjectRelease(fb);
        return 5;
    }

    if (addr == 0 && n >= 3) {
        const unsigned rev = b[0];
        const unsigned maj = rev >> 4, min = rev & 0xf;
        if (maj != 1 || min > 4) {
            printf("\nThe first byte is 0x%02x, which is not a DPCD revision\n"
                   "any sink reports (1.0 to 1.4). Treat the whole reply as\n"
                   "register noise rather than data.\n", rev);
            IOObjectRelease(fb);
            return 6;
        }
        printf("\n  DPCD revision    %u.%u\n", maj, min);
        printf("  max link rate    0x%02x", b[1]);
        switch (b[1]) {
        case 0x06: printf("  (1.62 Gbps/lane, RBR)\n"); break;
        case 0x0a: printf("  (2.7 Gbps/lane, HBR)\n"); break;
        case 0x14: printf("  (5.4 Gbps/lane, HBR2)\n"); break;
        case 0x1e: printf("  (8.1 Gbps/lane, HBR3)\n"); break;
        default:   printf("  (not one of the four standard rates)\n"); break;
        }
        printf("  max lane count   %u   (low 5 bits of 0x%02x)\n",
               b[2] & 0x1f, b[2]);
        printf("\nTHE AUX CHANNEL WORKS AND THE SINK IS TALKING TO US.\n"
               "That is link training's input. It is NOT a trained link,\n"
               "an EDID, a pipe or a display - each of those is its own\n"
               "step, and none of them has been attempted.\n");
    }

    IOObjectRelease(fb);
    return 0;
}
