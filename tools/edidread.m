/* SPDX-License-Identifier: MIT */
/* 2BC: read the sink's EDID over I2C-over-AUX and parse it.
 *
 * WHAT IT DOES ON THE CHIP. One I2C transaction on the engine bound to an HPD
 * block (default HPD2, the Type-C port): an I2C WRITE with MOT carrying the
 * byte offset, then eight 16-byte READs, the last without MOT so the sink's
 * DDC bus is released. DEFER is retried, bounded - a sink defers routinely
 * while its own DDC bus is busy and a single-shot read would report a working
 * monitor as silent.
 *
 * WHAT IT NEEDS. `-rbgpufbaux` in boot-args, the same arming as dpcdread.
 *
 * NOTHING IS PARSED UNTIL BOTH CHECKS PASS. The kext validates the fixed
 * eight-byte header AND the 128-byte checksum, and they are independent: a
 * partial chunked read can satisfy the header and fail the sum. If either
 * fails this prints the bytes and stops, because a plausible-looking mode
 * parsed out of a half-read block is the kind of result that costs
 * milestones.
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

static const char *refusal(long c)
{
    switch (c) {
    case 0:  return "none";
    case 1:  return "NOT ARMED - add -rbgpufbaux to boot-args";
    case 2:  return "no BAR5 map";
    case 3:  return "no AUX engine is bound to that HPD block";
    case 4:  return "the request was malformed";
    case 5:  return "that engine has AUX_EN clear";
    case 6:  return "THE DMCUB OWNS THE ENGINE - declined, not raced";
    case 7:  return "a stale AUX_SW_DONE never fell";
    case 8:  return "no AUX_SW_DONE after GO - the sink did not answer";
    case 9:  return "DONE with a zero reply byte count";
    case 10: return "THE OFFSET WRITE NEVER ACKED - I2C-over-AUX did not start";
    case 11: return "a chunk read failed partway (see bytes read)";
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

    unsigned long blk = 0, hpd = 2;
    if (argc >= 2) { blk = strtoul(argv[1], NULL, 0); }
    if (argc >= 3) { hpd = strtoul(argv[2], NULL, 0); }

    const long armed = rdnum(fb, CFSTR("RBFBAuxDpcdArmed"));
    printf("EDID read is %s", armed == 1 ? "ARMED" : "NOT ARMED");
    if (armed != 1) { printf("  (add -rbgpufbaux to boot-args)"); }
    printf("\n");

    NSDictionary *sel = @{ @"RBFBAuxHpd" : @(hpd) };
    IORegistryEntrySetCFProperties(fb, (__bridge CFTypeRef)sel);
    NSDictionary *go = @{ @"RBFBEdidRead" : @(blk) };
    kern_return_t kr = IORegistryEntrySetCFProperties(fb,
                                                      (__bridge CFTypeRef)go);
    printf("read EDID block %lu on the engine bound to HPD%lu -> 0x%x\n",
           blk, hpd, kr);

    sleep(1);   /* rule 31: the verdict is published periodically */
    const long ref = rdnum(fb, CFSTR("RBFBEdidRefuse"));
    const long n   = rdnum(fb, CFSTR("RBFBEdidBytes"));
    const long def = rdnum(fb, CFSTR("RBFBEdidDefers"));
    const long hok = rdnum(fb, CFSTR("RBFBEdidHeaderOk"));
    const long sok = rdnum(fb, CFSTR("RBFBEdidSumOk"));
    const long ins = rdnum(fb, CFSTR("RBFBEdidInst"));

    printf("  engine AUX%ld   bytes %ld   defers %ld\n", ins, n, def);
    printf("  refusal %ld - %s\n", ref, refusal(ref));
    printf("  header %s   checksum %s\n",
           hok == 1 ? "OK" : "BAD", sok == 1 ? "OK" : "BAD");

    if (ref != 0 && n <= 0) {
        printf("\nNo bytes. The refusal above is the finding.\n");
        IOObjectRelease(fb);
        return 3;
    }

    unsigned char e[128] = {0};
    long have = 0;
    CFDataRef d = (CFDataRef)IORegistryEntryCreateCFProperty(
        fb, CFSTR("RBFBEdid"), kCFAllocatorDefault, 0);
    if (d && CFGetTypeID(d) == CFDataGetTypeID()) {
        have = CFDataGetLength(d);
        if (have > 128) { have = 128; }
        CFDataGetBytes(d, CFRangeMake(0, have), e);
    }
    if (d) { CFRelease(d); }

    if (have <= 0) {
        printf("\nRBFBEdid carried no bytes.\n");
        IOObjectRelease(fb);
        return 4;
    }

    printf("\n");
    for (long i = 0; i < have; i += 16) {
        printf("  %02lx:", i);
        for (long j = i; j < i + 16 && j < have; j++) { printf(" %02x", e[j]); }
        printf("\n");
    }

    if (hok != 1 || sok != 1) {
        printf("\nNOT PARSED. The header and the checksum are independent and\n"
               "one of them failed, so nothing below the bytes is\n"
               "trustworthy. A half-read block can satisfy the header and\n"
               "fail the sum, which is exactly what a chunked read can do.\n");
        IOObjectRelease(fb);
        return 5;
    }

    /* The three-letter manufacturer ID is five bits per letter, big-endian
     * across bytes 8-9, 'A' = 1. */
    const unsigned mid = ((unsigned)e[8] << 8) | e[9];
    const char mfg[4] = { (char)('A' + ((mid >> 10) & 0x1f) - 1),
                          (char)('A' + ((mid >> 5) & 0x1f) - 1),
                          (char)('A' + (mid & 0x1f) - 1), 0 };
    printf("\n  manufacturer     %s\n", mfg);
    printf("  product code     0x%04x\n", e[10] | (e[11] << 8));
    printf("  EDID version     %u.%u\n", e[18], e[19]);

    /* The monitor's name lives in a descriptor tagged 0xFC. */
    for (unsigned off = 0x36; off + 18 <= 126; off += 18) {
        if (e[off] == 0 && e[off+1] == 0 && e[off+3] == 0xfc) {
            char name[14] = {0};
            for (unsigned i = 0; i < 13; i++) {
                const unsigned char c = e[off + 5 + i];
                name[i] = (c == 0x0a || c < 0x20) ? 0 : (char)c;
            }
            printf("  name             %s\n", name);
        }
    }

    /* EVERY descriptor, not just the first. 2BD: the first version parsed
     * only 0x36 and reported "60 Hz" for a monitor whose 100 Hz mode sits in
     * the SECOND slot - a true statement about the preferred timing that read
     * as the whole answer. Four slots, and the range-limits descriptor too,
     * because it bounds what the others claim.
     *
     * Every refresh here is DERIVED from the timing totals rather than read,
     * so a misparsed field shows up as an absurd rate instead of a plausible
     * one - which is how the hand-transcription error behind this fix
     * announced itself (0.017 Hz).
     */
    double bestHz = 0.0; unsigned bestW = 0, bestH = 0; double bestGbps = 0.0;
    printf("\n  TIMING DESCRIPTORS\n");
    static const unsigned slots[4] = { 0x36, 0x48, 0x5a, 0x6c };
    for (unsigned s = 0; s < 4; s++) {
        const unsigned char *d = &e[slots[s]];
        const unsigned pclk10k = d[0] | (d[1] << 8);
        if (pclk10k == 0) {
            /* A zero pixel clock means this slot is a non-timing descriptor,
             * and byte 3 is its tag. */
            if (d[3] == 0xfd) {
                printf("    0x%02x  RANGE LIMITS: V %u-%u Hz, H %u-%u kHz,"
                       " max pixel clock %u MHz\n",
                       slots[s], d[5], d[6], d[7], d[8], d[9] * 10);
            } else if (d[3] == 0xfc) {
                printf("    0x%02x  monitor name\n", slots[s]);
            } else if (d[3] == 0xff || d[3] == 0xfe) {
                printf("    0x%02x  text descriptor (tag 0x%02x)\n",
                       slots[s], d[3]);
            } else if (d[3] != 0x10) {   /* 0x10 is the "unused" tag */
                printf("    0x%02x  descriptor tag 0x%02x\n", slots[s], d[3]);
            }
            continue;
        }
        const unsigned hact = d[2] | ((d[4] & 0xf0) << 4);
        const unsigned hbl  = d[3] | ((d[4] & 0x0f) << 8);
        const unsigned vact = d[5] | ((d[7] & 0xf0) << 4);
        const unsigned vbl  = d[6] | ((d[7] & 0x0f) << 8);
        const unsigned long clk = (unsigned long)pclk10k * 10000ul;
        const unsigned long htot = hact + hbl, vtot = vact + vbl;
        if (!htot || !vtot) {
            printf("    0x%02x  a timing with a zero total - not usable\n",
                   slots[s]);
            continue;
        }
        const double hz = (double)clk / (double)(htot * vtot);
        const double gbps = (double)clk * 24.0 / 1e9;
        printf("    0x%02x  %u x %u  @ %.2f Hz   clk %lu Hz   "
               "Htot %lu Vtot %lu   %.2f Gbps%s\n",
               slots[s], hact, vact, hz, clk, htot, vtot, gbps,
               s == 0 ? "   <- PREFERRED" : "");
        if (hz > bestHz) {
            bestHz = hz; bestW = hact; bestH = vact; bestGbps = gbps;
        }
    }

    if (bestHz > 0.0) {
        printf("\n  FASTEST MODE THE SINK DESCRIBES: %u x %u @ %.2f Hz\n",
               bestW, bestH, bestHz);
        /* Against 2BB's MEASURED link, not a datasheet number: 2 lanes at
         * HBR2 is 10.8 Gbps raw and ~8.64 after 8b/10b. */
        printf("    needs %.2f Gbps at 24 bpp; 2BB measured this link at\n"
               "    2 lanes x HBR2 = ~8.64 Gbps usable, so it %s\n",
               bestGbps, bestGbps <= 8.64 ? "FITS" : "DOES NOT FIT");
        if (bestGbps > 8.64) {
            printf("    It would need more lanes, a lower rate, or fewer\n"
                   "    bits per pixel.\n");
        }
    }

    printf("\nTHE SINK'S EDID IS READ. Still not done: link training, a\n"
           "second OTG and HUBP, a DIG encoder, and a second IOFramebuffer\n"
           "so macOS attaches a display. None has been attempted.\n");
    IOObjectRelease(fb);
    return 0;
}
