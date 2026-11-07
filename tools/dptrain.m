/* SPDX-License-Identifier: MIT */
/* 2F4: put a DP training pattern on the wire, and ask the SINK whether it
 * arrived.
 *
 * WHAT IT DOES ON THE CHIP. The four writes of
 * dcn10_link_encoder_set_dp_phy_pattern_training_pattern(), in Linux's order,
 * on one transmitter: the pattern (a whole-register write), then
 * TRAINING_COMPLETE off, PHY bypass off and PRBS off as read-modify-writes.
 * Then the DPCD side of the same pattern to the sink, and a read of its lane
 * status.
 *
 * THE TWO PATTERN ENCODINGS ARE NOT THE SAME NUMBER. The hardware register
 * takes 0..3 for TPS1..TPS4; DPCD 0x102 takes 1, 2, 3, 7. This tool and the
 * kext both speak in TPSn and rb_dptx.h converts, because setting a different
 * pattern on each side of the link looks exactly like a sink that cannot
 * achieve clock recovery - a wrong answer that would be believed.
 *
 * THE VERDICT IS THE SINK'S OWN LANE STATUS (DPCD 0x202), read BEFORE as well
 * as after. Our own registers can only say the stores landed, which is rule
 * 20's defect if taken as the answer; and "CR not done" is the state the sink
 * is already in, so only a CHANGE in that byte is evidence the pattern reached
 * it.
 *
 * WHAT A FAILURE HERE DOES NOT MEAN. CR_DONE is NOT expected yet: the
 * transmitter has not been enabled at the link rate, which is the atom
 * transmitter command over DMUB and a separate step. A PATTERN_SET verdict is
 * the EXPECTED reading, and it is still the first time this project has put a
 * training pattern on a wire.
 *
 * Needs `-rbgpufbtrain` in boot-args (which arms the AUX half itself), and
 * root.
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

static const char *refusal(long c)
{
    switch (c) {
    case 0:  return "none - the writes ran";
    case 60: return "NOT ARMED - add -rbgpufbtrain to boot-args";
    case 61: return "no register mapping (check RBFBHubpRefuse)";
    case 62: return "THAT IS THE PANEL'S TRANSMITTER - refused by name";
    case 63: return "instance out of range (0..4)";
    case 64: return "THE BLOCK IS NOT IDENTIFIED - the panel's own "
                    "DP_LINK_CNTL does not read as a trained link";
    case 65: return "the pattern must be TPS1..TPS4";
    case 66: return "a VIDEO STREAM is running on that transmitter";
    default: return "unknown";
    }
}

static const char *verdictName(long v)
{
    switch (v) {
    case -1: return "NOT RUN";
    case 0:  return "NO STORE - our own registers did not take";
    case 1:  return "PATTERN SET - our side only; the sink's status did not "
                    "move";
    case 2:  return "SINK MOVED - the sink's lane status CHANGED";
    case 3:  return "CR DONE - clock recovery on every lane";
    case 4:  return "NO-OP - the writes took, and NOT ONE BIT MOVED";
    case 5:  return "SINK BLIND - our side took; the sink could not be read";
    default: return "unknown";
    }
}

/* The code the DPCD 0x102 write itself returned - 2F3's write refusals and
 * verdicts, which 2F4 captured into a field and never published. */
static const char *dpcdSetName(long v)
{
    switch (v) {
    case 0:  return "OK - the write ran";
    case 20: return "NOT ARMED";
    case 21: return "NO REGS";
    case 22: return "PANEL HPD - refused by name";
    case 23: return "PANEL AUX ENGINE - refused by name";
    case 24: return "NO SUCH AUX ENGINE";
    case 25: return "ADDRESS NOT ALLOWLISTED";
    case 26: return "VALUE OUT OF RANGE";
    default: return "unknown";
    }
}

static void laneLine(const char *tag, long ok, long byte, long lanes)
{
    if (ok != 1) {
        printf("  %-14s (the read FAILED)\n", tag);
        return;
    }
    printf("  %-14s 0x%02lx  ", tag, byte & 0xff);
    for (long i = 0; i < lanes && i < 2; i++) {
        const long st = (i & 1) ? ((byte >> 4) & 0x7) : (byte & 0x7);
        printf("lane%ld[%s%s%s] ", i,
               st & 1 ? "CR " : "", st & 2 ? "EQ " : "",
               st & 4 ? "LOCK" : "");
    }
    printf("\n");
}

int main(int argc, char **argv)
{
    unsigned long inst = 2, tps = 1, hpd = 2, lanes = 2;
    int haveGo = 0;

    for (int a = 1; a < argc; a++) {
        if (strncmp(argv[a], "inst=", 5) == 0) {
            inst = strtoul(argv[a] + 5, NULL, 0); haveGo = 1;
        } else if (strncmp(argv[a], "tps=", 4) == 0) {
            tps = strtoul(argv[a] + 4, NULL, 0); haveGo = 1;
        } else if (strncmp(argv[a], "hpd=", 4) == 0) {
            hpd = strtoul(argv[a] + 4, NULL, 0); haveGo = 1;
        } else if (strncmp(argv[a], "lanes=", 6) == 0) {
            lanes = strtoul(argv[a] + 6, NULL, 0); haveGo = 1;
        } else if (strcmp(argv[a], "go") == 0) {
            haveGo = 1;
        } else {
            printf("usage: dptrain go [inst=N] [tps=1..4] [hpd=N] "
                   "[lanes=1..2]\n"
                   "  inst  default 2 - DIG2, the Type-C port (2B4/2B9/2E8)\n"
                   "        instance 0 is the PANEL and is refused by name\n"
                   "  tps   default 1 - TPS1, the clock-recovery pattern.\n"
                   "        TPSn, NOT the register's 0-based encoding and\n"
                   "        NOT DPCD 0x102's (which is 1,2,3,7).\n"
                   "  hpd   default 2 - picks the AUX engine for the sink\n"
                   "        half, from AUX_HPD_SEL rather than assumed\n"
                   "  lanes default 2 - this SINK's lane count (2BB read it\n"
                   "        from its DPCD). It decides what CR_DONE means.\n"
                   "\n"
                   "With no arguments this prints the block read-only.\n");
            return 2;
        }
    }

    io_service_t fb = IOServiceGetMatchingService(
        kIOMainPortDefault, IOServiceMatching("RBAMDFramebuffer"));
    if (!fb) {
        fprintf(stderr, "no RBAMDFramebuffer - is the framebuffer armed?\n");
        return 1;
    }

    const long armed = rdnum(fb, "RBFBDpTxArmed");
    const long ready = rdnum(fb, "RBFBDpTxReady");
    const long refOk = rdnum(fb, "RBFBDpTxRefTrained");

    printf("DP transmitter block is %s", armed == 1 ? "ARMED" : "NOT ARMED");
    if (armed != 1) { printf("   (add -rbgpufbtrain to boot-args)"); }
    printf("   sampled %s%s\n", ready == 1 ? "yes" : "NO",
           ready < 0 ? "  (the key is ABSENT - framebuffer predates 2F4)"
                     : "");

    /* THE IDENTIFICATION, first, because nothing below means anything without
     * it: the panel's own transmitter must read as a trained link, since the
     * firmware trained it and it is scanning out right now. */
    printf("\nthe block's identification (rule 47, asked of a write)\n");
    printf("  panel DP0 reads as a TRAINED link: %s\n",
           refOk == 1 ? "YES - the block is what we think it is"
         : refOk < 0 ? "UNKNOWN - the key is ABSENT, not a negative reading"
                     : "NO - the write will be REFUSED (code 64)");

    printf("\nall five transmitters, live\n");
    printf("  inst  LINK_CNTL   VID       DPHY_CNTL  TP_SEL    PRBS\n");
    for (int i = 0; i < 5; i++) {
        char k[28];
        long lc, vid, dc, tp, pc;
        snprintf(k, sizeof(k), "RBFBDpTx%dLink", i);  lc  = rdnum(fb, k);
        snprintf(k, sizeof(k), "RBFBDpTx%dVid", i);   vid = rdnum(fb, k);
        snprintf(k, sizeof(k), "RBFBDpTx%dDphy", i);  dc  = rdnum(fb, k);
        snprintf(k, sizeof(k), "RBFBDpTx%dTpSel", i); tp  = rdnum(fb, k);
        snprintf(k, sizeof(k), "RBFBDpTx%dPrbs", i);  pc  = rdnum(fb, k);
        /* Rule 14 at the tool boundary: an ABSENT key reads -1, and
         * printing that as 0x00000000 would be indistinguishable from a real
         * zero word - which is the trap 2F3's own publisher was written to
         * avoid and which this table walked straight into. */
        if (lc < 0 || vid < 0 || dc < 0 || tp < 0 || pc < 0) {
            printf("  DP%-3d (keys ABSENT - this framebuffer predates 2F4; "
                   "nothing here is a reading)\n", i);
            continue;
        }
        printf("  DP%-3d 0x%08lx  0x%06lx  0x%08lx 0x%06lx  0x%06lx %s%s%s%s\n",
               i, lc, vid, dc, tp, pc,
               (lc & 0x10) ? " TRAINED" : "",
               (lc & 0x100) ? " LINKUP" : "",
               (vid & 1) ? " STREAM" : "",
               (dc & 0x10000) ? " BYPASS" : "");
    }
    printf("  (TRAINED = TRAINING_COMPLETE, a bit a driver WRITES.\n"
           "   LINKUP = DP_LINK_STATUS, the hardware's own - the one worth\n"
           "   reading as a verdict. DP0 is the panel's.)\n");

    if (!haveGo) {
        printf("\nRead-only. `dptrain go` defaults to TPS1, which boot 458\n"
               "measured as a NO-OP on this block: TPS1 encodes as hardware\n"
               "pattern 0 and a dark TP_SEL already reads 0. Use\n"
               "`dptrain go tps=2` for a write that CAN move the register -\n"
               "note DP0, a live trained link, reads TP_SEL 0x2 itself, so\n"
               "this register does hold non-zero values in practice.\n"
               "`dptrain` with any bad argument prints the usage.\n");
        IOObjectRelease(fb);
        return 0;
    }

    if (inst == 0) {
        printf("\nREFUSING to pass instance 0 from this tool.\n"
               "DP0/DIG0 is the internal eDP PANEL this desktop is running\n"
               "on, and a training pattern REPLACES the video stream on a\n"
               "transmitter. The kext refuses it by name as well; this is\n"
               "the outer of the two checks.\n");
        IOObjectRelease(fb);
        return 2;
    }
    if (tps < 1 || tps > 4) {
        printf("\nREFUSING tps=%lu - the patterns are TPS1..TPS4.\n", tps);
        IOObjectRelease(fb);
        return 2;
    }
    if (lanes < 1 || lanes > 2) {
        printf("\nREFUSING lanes=%lu - one DPCD status byte covers two"
               " lanes.\n", lanes);
        IOObjectRelease(fb);
        return 2;
    }

    const long before = rdnum(fb, "RBFBDpTxCalls");

    /* All four keys in ONE setProperties call: the kext reads the instance,
     * the pattern and the lane count before the RBFBDpTxGo that triggers the
     * writes, so every dial is in place before the write it governs (rule 34,
     * and 2DA's two-call handover). */
    NSDictionary *go = @{ @"RBFBDpTxInst"  : @(inst),
                          @"RBFBDpTxTps"   : @(tps),
                          @"RBFBDpTxLanes" : @(lanes),
                          @"RBFBDpTxGo"    : @(hpd) };
    kern_return_t kr = IORegistryEntrySetCFProperties(fb,
                                                      (__bridge CFTypeRef)go);
    printf("\nTPS%lu on DP%lu, sink over HPD%lu, %lu lane%s -> 0x%x%s\n",
           tps, inst, hpd, lanes, lanes == 1 ? "" : "s", kr,
           kr == KERN_SUCCESS ? "  (the push returned OK)" : "");

    /* Rule 31: poll a MONOTONIC counter past its pre-read value. */
    long calls = before;
    for (int spins = 0; spins < 60; spins++) {
        calls = rdnum(fb, "RBFBDpTxCalls");
        if (calls > before) { break; }
        usleep(100 * 1000);
    }
    if (calls <= before) {
        printf("\nRBFBDpTxCalls never moved past %ld in 6 s.\n"
               "That is NOT a refusal. Either this framebuffer predates 2F4\n"
               "(the key would be ABSENT and read as -1), or the set never\n"
               "reached the handler. Check the UUID inside the collection\n"
               "first (rule 15).\n", before);
        IOObjectRelease(fb);
        return 3;
    }

    const long ref   = rdnum(fb, "RBFBDpTxRefuse");
    const long ran   = rdnum(fb, "RBFBDpTxRan");
    printf("\nresult (calls %ld, writes that ran %ld)\n", calls, ran);
    printf("  refusal      %ld - %s\n", ref, refusal(ref));
    if (ref != 0) {
        printf("\nNothing was written. The refusal is the finding.\n");
        if (ref == 64) {
            printf("Read RBFBDpTx0Link: the panel's transmitter must show\n"
                   "both TRAINING_COMPLETE (0x10) and DP_LINK_STATUS (0x100)\n"
                   "for this block to be identified at all.\n");
        }
        IOObjectRelease(fb);
        return 4;
    }

    const long tpB = rdnum(fb, "RBFBDpTxTpBefore");
    const long tpA = rdnum(fb, "RBFBDpTxTpAfter");
    const long lcB = rdnum(fb, "RBFBDpTxLcBefore");
    const long lcA = rdnum(fb, "RBFBDpTxLcAfter");
    const long dcB = rdnum(fb, "RBFBDpTxDcBefore");
    const long dcA = rdnum(fb, "RBFBDpTxDcAfter");
    const long pcB = rdnum(fb, "RBFBDpTxPcBefore");
    const long pcA = rdnum(fb, "RBFBDpTxPcAfter");
    const long app = rdnum(fb, "RBFBDpTxApplied");
    const long cln = rdnum(fb, "RBFBDpTxClean");
    const long sbOk = rdnum(fb, "RBFBDpTxSinkBeforeOk");
    const long saOk = rdnum(fb, "RBFBDpTxSinkAfterOk");
    const long sb   = rdnum(fb, "RBFBDpTxSinkBefore");
    const long sa   = rdnum(fb, "RBFBDpTxSinkAfter");
    const long dset = rdnum(fb, "RBFBDpTxDpcdRefuse");
    const long mov  = rdnum(fb, "RBFBDpTxMoved");
    const long obs  = rdnum(fb, "RBFBDpTxObservable");
    const long dpat = rdnum(fb, "RBFBDpTxDpcdSet");
    const long verd = rdnum(fb, "RBFBDpTxVerdict");

    printf("\n  our four registers, in Linux's order\n");
    printf("    TP_SEL      0x%06lx -> 0x%06lx   %s (a WHOLE-register write)\n",
           tpB, tpA, tpB == tpA ? "unchanged" : "CHANGED  ");
    printf("    LINK_CNTL   0x%08lx -> 0x%08lx %s (TRAINING_COMPLETE off)\n",
           lcB, lcA, lcB == lcA ? "unchanged" : "CHANGED  ");
    printf("    DPHY_CNTL   0x%08lx -> 0x%08lx %s (BYPASS off)\n",
           dcB, dcA, dcB == dcA ? "unchanged" : "CHANGED  ");
    printf("    PRBS_CNTL   0x%06lx -> 0x%06lx   %s (PRBS_EN off)\n",
           pcB, pcA, pcB == pcA ? "unchanged" : "CHANGED  ");
    printf("    applied %ld  clean %ld  moved %ld  observable %ld\n",
           app, cln, mov, obs);
    printf("      ^ `clean` is the check that the three read-modify-writes\n"
           "        disturbed no neighbouring field. 2EC paid for that one.\n"
           "        `moved` is whether ANY bit changed, and `observable` is\n"
           "        whether the requested pattern COULD have moved TP_SEL at\n"
           "        all. Boot 458 had applied 1 and clean 1 honestly with\n"
           "        moved 0 - every target state was already present on a\n"
           "        dark transmitter, which is rule 20's own defect.\n");

    printf("\n  THE SINK'S DPCD 0x102, the pattern's other half\n");
    printf("    write code   %ld - %s\n", dpat, dpcdSetName(dpat));
    printf("    the ACK and whether the sink STORED it are in the AUX write\n"
           "    record, which is a separate reading:\n"
           "      ioreg -rl -c RBAMDFramebuffer | grep RBFBAuxW\n"
           "    Verdict 1 = ACK_STORED, 2 = ACK_IGNORED (the sink answered\n"
           "    and kept its old byte), 3 = ACK_BLIND, 0 = no answer.\n");

    printf("\n  THE SINK'S OWN LANE STATUS (DPCD 0x202) - we never write it\n");
    laneLine("before", sbOk, sb, (long)lanes);
    laneLine("after", saOk, sa, (long)lanes);
    if (dset != 0) {
        printf("  (the sink-side read refused with AUX code %ld)\n", dset);
    }
    printf("  VERDICT      %ld  %s\n", verd, verdictName(verd));

    if (verd == 3) {
        printf("\n  -> CLOCK RECOVERY. Every lane reports CR_DONE, from a\n"
               "     register this project does not write, after a pattern\n"
               "     this project put on the wire. That is more than was\n"
               "     expected at this step - the transmitter has NOT been\n"
               "     enabled at the link rate yet - so read it twice and\n"
               "     re-run before believing it (rule 44).\n");
    } else if (verd == 2) {
        printf("\n  -> THE SINK'S STATUS MOVED. Not clock recovery, but the\n"
               "     receiver changed state after our pattern, which is\n"
               "     evidence something reached it. The byte above says\n"
               "     which bits. Re-run to check it repeats.\n");
    } else if (verd == 1) {
        printf("\n  -> PATTERN SET, and this is the EXPECTED reading. Our\n"
               "     four registers took the writes cleanly; the sink's lane\n"
               "     status did not move, which is what should happen while\n"
               "     the transmitter is not yet driving at the link rate.\n"
               "     The missing step is the atom transmitter ENABLE over\n"
               "     DMUB (2E9 proved that path executes) carrying the rate\n"
               "     the sink was just told to expect.\n"
               "     A dark monitor is still the expected reading.\n");
    } else if (verd == 5) {
        printf("\n  -> SINK BLIND. Our four registers took, and the sink's\n"
               "     lane status could NOT be read afterwards, so this run\n"
               "     says nothing about the sink either way. The before-read\n"
               "     succeeded, so the AUX engine was working at the start of\n"
               "     the call - which makes the DPCD 0x102 write and the\n"
               "     100 us wait the only things that happened in between.\n"
               "     Check whether the sink is still answering at all with\n"
               "     `dpcdread 0x202 1`, then `dpcdread 0 1`. If 0x000 reads\n"
               "     and 0x202 does not, the receiver is awake and the\n"
               "     status register is the problem; if neither reads, the\n"
               "     AUX channel is.\n");
    } else if (verd == 4) {
        printf("\n  -> NO-OP. The writes took and NOT ONE BIT MOVED, so this\n"
               "     run put nothing new on the wire and nothing about the\n"
               "     sink may be attributed to it. With `observable 0` the\n"
               "     cause is the pattern itself: TPS1 encodes as hardware\n"
               "     pattern 0 and a dark TP_SEL already reads 0, so TPS1 is\n"
               "     invisible in this register by construction - and the\n"
               "     three read-modify-writes each clear a bit a dark\n"
               "     instance does not have set. Run `dptrain go tps=2`,\n"
               "     which encodes as 1 and CAN move TP_SEL. That is the\n"
               "     write that tests whether this block answers us at all.\n");
    } else if (verd == 0) {
        printf("\n  -> OUR OWN REGISTERS DID NOT TAKE. `applied 0` means a\n"
               "     field did not read back; `clean 0` means one of the\n"
               "     read-modify-writes disturbed a neighbour, which makes\n"
               "     the whole reading suspect and the address the first\n"
               "     suspect. Do NOT interpret the sink bytes off this run.\n");
    }

    IOObjectRelease(fb);
    return 0;
}
