/* SPDX-License-Identifier: MIT */
/* 2F3: write one byte of the sink's DPCD over DP AUX.
 *
 * THE FIRST WRITE THIS PROJECT MAKES TO THE SINK rather than to this chip.
 * 2BD's EDID read writes an offset byte over AUX, but that goes to the
 * monitor's DDC EEPROM, not to its DP receiver. These addresses change the
 * receiver's own state, and link training is nothing but a sequence of them.
 *
 * WHAT BOUNDS IT, three ways, each a refusal with its own code:
 *   - THE PANEL IS REFUSED BY NAME, three times independently: here by HPD,
 *     in the kext by HPD, and in the kext again by AUX ENGINE. A DPCD write
 *     on the link the desktop is running on is the way to lose it.
 *   - THE ADDRESS IS ALLOWLISTED, not blocklisted. Only the link
 *     configuration block is writable; the capability and status blocks that
 *     training READS are refused, as is everything else in the 20-bit space.
 *     Enforced here, in rb_dpcd_write_refuse() and again in the encoder.
 *   - NOTHING IN THE LIST ACTS ALONE. A rate and a lane count do nothing
 *     until a training pattern is set, and this sink is displaying nothing in
 *     any case: no boot of this machine has ever trained this link. 2F2
 *     measured that - the firmware's GOP lights no external display on this
 *     chassis on either GPU, and the sink's LED never leaves standby.
 *
 * HOW TO READ THE RESULT. The verdict comes from TWO things, neither of which
 * is the value we wrote:
 *   - the sink's own ACK, a reply we do not compose;
 *   - a read-AFTER against a read-BEFORE, so `changed` says the receiver
 *     STORED the byte rather than merely answering the transaction.
 * They can disagree. A sink may ACK a write to a field it ignores, and that
 * is a reportable outcome (ACK_IGNORED), not a failure. `applied` is
 * published beside `changed` because a byte already holding the target value
 * makes a readback agree for free - rule 20.
 *
 * Needs `-rbgpufbaux` in boot-args, and root.
 */
#import <Foundation/Foundation.h>
#import <IOKit/IOKitLib.h>

static long rdnum(io_service_t fb, const char *name)
{
    CFStringRef key = CFStringCreateWithCString(NULL, name,
                                                kCFStringEncodingUTF8);
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
    CFRelease(key);
    return out;
}

static const char *wrefusal(long c)
{
    switch (c) {
    case 0:  return "none - the transaction ran";
    case 20: return "NOT ARMED - add -rbgpufbaux to boot-args";
    case 21: return "no register mapping (check RBFBHubpRefuse)";
    case 22: return "THAT IS THE PANEL'S HPD - refused by name";
    case 23: return "THAT IS THE PANEL'S AUX ENGINE - refused by name";
    case 24: return "no AUX engine is bound to that HPD block";
    case 25: return "ADDRESS NOT IN THE ALLOWLIST";
    case 26: return "the value is not a byte";
    default: return "unknown";
    }
}

static const char *wverdict(long v)
{
    switch (v) {
    case -1: return "NOT RUN";
    case 0:  return "NO ANSWER - the sink did not ACK";
    case 1:  return "ACK + STORED - it answered and the byte reads back";
    case 2:  return "ACK + IGNORED - it answered and the byte did not change";
    case 3:  return "ACK + BLIND - it answered, the read-back failed";
    default: return "unknown";
    }
}

/* The allowlist, mirrored from rb_dpcd.h so this tool refuses first and names
 * the register. The kext refuses independently; this is the outer check. */
static const char *addrName(unsigned long a)
{
    switch (a) {
    case 0x100: return "DP_LINK_BW_SET";
    case 0x101: return "DP_LANE_COUNT_SET";
    case 0x102: return "DP_TRAINING_PATTERN_SET";
    case 0x103: return "DP_TRAINING_LANE0_SET";
    case 0x104: return "DP_TRAINING_LANE1_SET";
    case 0x105: return "DP_TRAINING_LANE2_SET";
    case 0x106: return "DP_TRAINING_LANE3_SET";
    case 0x107: return "DP_DOWNSPREAD_CTRL";
    case 0x108: return "DP_MAIN_LINK_CHANNEL_CODING_SET";
    case 0x600: return "DP_SET_POWER";
    default:    return NULL;
    }
}

static void usage(void)
{
    printf("usage: dpcdwrite <address> <value> [hpd]      (hpd default 2)\n"
           "\n"
           "The ONLY writable addresses, drm_dp.h's names:\n"
           "  0x100 DP_LINK_BW_SET          0x06 RBR, 0x0a HBR, 0x14 HBR2\n"
           "  0x101 DP_LANE_COUNT_SET       lanes in the low 4 bits,\n"
           "                                0x80 = ENHANCED_FRAME_EN\n"
           "  0x102 DP_TRAINING_PATTERN_SET 0 off, 1 TPS1, 2 TPS2\n"
           "  0x103..0x106 DP_TRAINING_LANEn_SET   swing/pre-emphasis\n"
           "  0x107 DP_DOWNSPREAD_CTRL      0x10 = SPREAD_AMP_0_5\n"
           "  0x108 DP_MAIN_LINK_CHANNEL_CODING_SET  0x01 = ANSI 8b/10b\n"
           "  0x600 DP_SET_POWER            1 = D0 (on), 2 = D3\n"
           "\n"
           "This sink is DP 1.2 / HBR2 / 2 lanes (2BB), so the first two\n"
           "writes of training here are:\n"
           "  dpcdwrite 0x100 0x14      HBR2\n"
           "  dpcdwrite 0x101 0x82      2 lanes, enhanced framing\n"
           "Neither does anything until 0x102 sets a training pattern.\n");
}

int main(int argc, char **argv)
{
    if (argc < 3) { usage(); return 2; }

    const unsigned long addr = strtoul(argv[1], NULL, 0);
    const unsigned long val  = strtoul(argv[2], NULL, 0);
    const unsigned long hpd  = (argc >= 4) ? strtoul(argv[3], NULL, 0) : 2;

    /* The outer of three panel checks. Done before the service is even
     * opened, so a typo cannot reach the kext at all. */
    if (hpd == 0) {
        printf("REFUSING to pass HPD0 from this tool.\n"
               "HPD0 is the internal eDP PANEL this desktop is running on.\n"
               "A DPCD write on that link is how the display is lost. The\n"
               "kext refuses it by HPD and again by AUX engine; this is the\n"
               "outer of the three checks.\n");
        return 2;
    }
    const char *an = addrName(addr);
    if (!an) {
        printf("REFUSING address 0x%05lx - not in the allowlist.\n\n", addr);
        usage();
        return 2;
    }
    if (val > 0xff) {
        printf("REFUSING value 0x%lx - AUX_SW_DATA carries one byte.\n", val);
        return 2;
    }

    io_service_t fb = IOServiceGetMatchingService(
        kIOMainPortDefault, IOServiceMatching("RBAMDFramebuffer"));
    if (!fb) {
        fprintf(stderr, "no RBAMDFramebuffer - is the framebuffer armed?\n");
        return 1;
    }

    const long armed = rdnum(fb, "RBFBAuxDpcdArmed");
    printf("DPCD write is %s", armed == 1 ? "ARMED" : "NOT ARMED");
    if (armed != 1) { printf("   (add -rbgpufbaux to boot-args)"); }
    printf("\n");
    printf("writing %s (0x%05lx) = 0x%02lx on the engine bound to HPD%lu\n",
           an, addr, val, hpd);

    const long before = rdnum(fb, "RBFBAuxWCalls");

    /* All three keys in ONE setProperties call: the kext reads the HPD and
     * the address before the VALUE that triggers the transaction, so both
     * dials are in place before the write they govern (rule 34). */
    NSDictionary *go = @{ @"RBFBAuxWHpd"   : @(hpd),
                          @"RBFBAuxWAddr"  : @(addr),
                          @"RBFBAuxWValue" : @(val) };
    kern_return_t kr = IORegistryEntrySetCFProperties(fb,
                                                      (__bridge CFTypeRef)go);
    printf("-> 0x%x%s\n", kr,
           kr == KERN_SUCCESS ? "  (the push returned OK)" : "");

    /* Rule 31: poll a MONOTONIC counter past its pre-read value. Sleeping and
     * comparing two reads of the dialled value cannot tell "not published
     * yet" from "the write was lost", and that mistake has been made five
     * times in this project. */
    long calls = before;
    for (int spins = 0; spins < 60; spins++) {
        calls = rdnum(fb, "RBFBAuxWCalls");
        if (calls > before) { break; }
        usleep(100 * 1000);
    }
    if (calls <= before) {
        printf("\nRBFBAuxWCalls never moved past %ld in 6 s.\n"
               "That is NOT a refusal. Either this framebuffer predates 2F3\n"
               "(the key would be ABSENT and read as -1), or the set never\n"
               "reached the handler. Check the UUID inside the collection\n"
               "first (rule 15):\n"
               "  kmutil inspect -B /Library/KernelCollections/"
               "AuxiliaryKernelExtensions.kc \\\n"
               "    --show-kext-uuids | grep -i rembrandt\n", before);
        IOObjectRelease(fb);
        return 3;
    }

    const long ref     = rdnum(fb, "RBFBAuxWRefuse");
    const long inst    = rdnum(fb, "RBFBAuxWInst");
    const long reply   = rdnum(fb, "RBFBAuxWReplyCode");
    const long acked   = rdnum(fb, "RBFBAuxWAcked");
    const long bOk     = rdnum(fb, "RBFBAuxWBeforeOk");
    const long aOk     = rdnum(fb, "RBFBAuxWAfterOk");
    const long bVal    = rdnum(fb, "RBFBAuxWBefore");
    const long aVal    = rdnum(fb, "RBFBAuxWAfter");
    const long applied = rdnum(fb, "RBFBAuxWApplied");
    const long changed = rdnum(fb, "RBFBAuxWChanged");
    const long verd    = rdnum(fb, "RBFBAuxWVerdict");
    const long oks     = rdnum(fb, "RBFBAuxWOks");

    printf("\nresult (calls %ld, acked-writes %ld)\n", calls, oks);
    printf("  engine     AUX%ld   (from AUX_HPD_SEL, not assumed - 2B9)\n",
           inst);
    printf("  refusal    %ld - %s\n", ref, wrefusal(ref));
    if (ref != 0) {
        printf("\nNothing was written to the sink. The refusal is the"
               " finding.\n");
        IOObjectRelease(fb);
        return 4;
    }

    const char *rn = reply == 0 ? "ACK" : reply == 1 ? "NACK"
                   : reply == 2 ? "DEFER" : "no reply";
    printf("  reply      %ld (%s)   acked %ld\n", reply, rn, acked);
    /* The Ok flags are read FIRST and printed with the value, because a
     * refused read publishes 0xffffffff and that is NOT the byte 0xff - a
     * missing value that passes a field test is rule 14's trap. */
    if (bOk == 1) { printf("  before     0x%02lx\n", bVal & 0xff); }
    else          { printf("  before     (the read-before FAILED)\n"); }
    if (aOk == 1) { printf("  after      0x%02lx\n", aVal & 0xff); }
    else          { printf("  after      (the read-after FAILED)\n"); }
    printf("  applied %ld  changed %ld\n", applied, changed);
    printf("  VERDICT    %ld  %s\n", verd, wverdict(verd));

    if (verd == 1 && changed == 1) {
        printf("\n  -> THE SINK ACCEPTED A WRITE AND STORED IT. The ACK is\n"
               "     the sink's own reply, not a value we composed, and\n"
               "     `changed 1` says the byte differs from the read BEFORE -\n"
               "     so this is not rule 20's agreeing readback.\n"
               "     This is the mechanism link training is made of. It is\n"
               "     NOT a trained link: no training pattern on the wire, no\n"
               "     CR or EQ loop, no symbol lock. Those are the next steps.\n");
    } else if (verd == 1 && changed != 1) {
        printf("\n  -> ACK, and the byte reads back as what we wrote - but it\n"
               "     ALREADY held that value, so `applied` carries no\n"
               "     evidence of our store (rule 20, 2E6's shape). The ACK is\n"
               "     still the sink answering. Write a DIFFERENT value to the\n"
               "     same address to get a `changed` reading.\n");
    } else if (verd == 2) {
        printf("\n  -> The sink ACKED and did NOT store the byte. That is a\n"
               "     real outcome, not a failed transaction: the receiver\n"
               "     answered the AUX request and ignored the field. Worth\n"
               "     checking the address against drm_dp.h before concluding\n"
               "     anything about the channel.\n");
    } else if (verd == 3) {
        printf("\n  -> ACK, but the read-AFTER failed, so whether it stored\n"
               "     the byte is unknown. Do not record this as a successful\n"
               "     write. Re-run; a DEFER on the read half is routine.\n");
    } else if (verd == 0) {
        printf("\n  -> No ACK. The reply code above says whether the sink\n"
               "     NACKed (it is declining this address - the channel\n"
               "     works) or said nothing at all (refusal 8/9 territory on\n"
               "     the read side). A NACK and silence are different\n"
               "     findings and must not be merged.\n");
    }

    IOObjectRelease(fb);
    return 0;
}
