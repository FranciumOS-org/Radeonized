/* strencprog - 305: program the DP stream on a DIO stream encoder instance.
 *
 * Reads the licence, sends ONE setProperties call, reads the result. It
 * computes no verdict of its own: the kext decided, and recomputing a verdict
 * in the reader is 2C0's defect - two sources of truth that disagree only
 * when it matters.
 *
 * THE LICENCE IS PRINTED BEFORE THE WRITE IS OFFERED. Nothing is sent unless
 * RBFBStrencMsaMismatch is 0 in the sample this tool just read, because that
 * is the whole basis for believing the four MSA words are right (rule 47
 * asked of a write). The kext re-checks it at the moment of the write too -
 * this check is the courtesy, not the guard.
 *
 *   strencprog                 - read the licence and the state, write nothing
 *   strencprog go              - ATTRIBUTE: configure DIG2 from OTG1 at
 *                                148500 kHz HBR2, stream left DISABLED
 *   strencprog unblank         - UNBLANK: turn the stream on
 *   strencprog go inst=2 otg=1 pixkhz=148500 rate=20
 *
 * THE ORDER IS go -> dptrain go tps=2 -> unblank. Upstream splits
 * dp_set_stream_attribute from dp_unblank because link training happens
 * between them, and boot 477 paid for collapsing them: with the stream
 * enabled, dptrain refuses 66 STREAM_LIVE.
 *
 * DIG0 and OTG0 are the panel's and are refused BY NAME in the kext; this
 * tool refuses them again on the way in, because a refusal that only exists
 * one layer down is one layer from not existing.
 */
#import <Foundation/Foundation.h>
#import <IOKit/IOKitLib.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/sysctl.h>
#include "rb_strenc.h"
#include "rb_strprog.h"

static long rdnum2(io_service_t fb, const char *name, int *found)
{
    CFStringRef key = CFStringCreateWithCString(NULL, name,
                                                kCFStringEncodingUTF8);
    CFTypeRef v = IORegistryEntryCreateCFProperty(fb, key, NULL, 0);
    long out = -1;
    if (found) { *found = 0; }
    if (v) {
        if (CFGetTypeID(v) == CFNumberGetTypeID()) {
            long long t = 0;
            CFNumberGetValue((CFNumberRef)v, kCFNumberLongLongType, &t);
            out = (long)t;
            if (found) { *found = 1; }
        }
        CFRelease(v);
    }
    CFRelease(key);
    return out;
}

static long rdnum(io_service_t fb, const char *name)
{
    return rdnum2(fb, name, NULL);
}

static const char *refusal(long c)
{
    switch (c) {
    case 0:  return "OK";
    case 1:  return "E_ARGS - NULL or no room for the write list";
    case 2:  return "E_INST - instance out of range";
    case 3:  return "E_PANEL - DIG0 refused BY NAME (it is the panel's)";
    case 4:  return "E_MSA - the derivation does NOT reproduce the panel; "
                    "nothing may be written on it";
    case 5:  return "E_DERIVE - the target pipe has no raster to derive from "
                    "(program it with otgprog first, THIS boot)";
    case 6:  return "E_MVID - m_vid 0, never a valid M";
    case 7:  return "E_OTG0 - OTG0 refused BY NAME (it drives the panel)";
    case 8:  return "E_OTG - tg_inst outside the mux field";
    case 50: return "NOT_ARMED - boot with -rbgpufbstrm";
    case 51: return "NO_REGS - the register aperture is not mapped";
    case 52: return "APERTURE - the stream-encoder block is not covered; "
                    "arm -rbgpufbdpalt / -rbgpufbdig / -rbgpufbfe";
    case 53: return "NO_DERIVATION - no derivation exists for that pipe "
                    "(only pipes 0 and 1 are derived, and 0 is refused)";
    case 54: return "STATUS_STUCK - DP_VID_STREAM_STATUS never cleared after "
                    "ENABLE 0; the FIFO is still draining and the rest of the "
                    "sequence was NOT sent";
    default: return "unknown";
    }
}

static const char *verdictStr(long v)
{
    switch (v) {
    case 0: return "NOT_RUN";
    case 1: return "RUNNING - ENABLE 1 and STATUS 1: the hardware says it is "
                   "carrying a stream";
    case 2: return "NO_STATUS - ENABLE 1 but STATUS 0: we asked and the block "
                   "did not answer";
    case 3: return "NOT_ENABLED - the enable write did not even take";
    default: return "unknown";
    }
}

int main(int argc, const char **argv)
{
    @autoreleasepool {
        io_service_t fb = IOServiceGetMatchingService(kIOMainPortDefault,
                              IOServiceMatching("RBAMDFramebuffer"));
        if (!fb) {
            printf("RBAMDFramebuffer not found. Is the framebuffer loaded?\n");
            return 2;
        }

        int haveArmed = 0;
        const long armed = rdnum2(fb, "RBFBStrmArmed", &haveArmed);
        if (!haveArmed) {
            printf("THE LOADED KEXT DOES NOT PUBLISH RBFBStrm* AT ALL.\n"
                   "That is not a refusal - it is an older framebuffer. Check "
                   "the UUID INSIDE THE COLLECTION, not in /Library/Extensions"
                   " (rule 15):\n"
                   "  kmutil inspect -B /Library/KernelCollections/"
                   "AuxiliaryKernelExtensions.kc --show-kext-uuids | grep -i "
                   "rembrandt\n");
            IOObjectRelease(fb);
            return 3;
        }

        /* ---- the licence, printed first ---- */
        int haveMm = 0;
        const long mm = rdnum2(fb, "RBFBStrencMsaMismatch", &haveMm);
        const long p1rc = rdnum(fb, "RBFBStrencP1DeriveRc");

        /* THE ARM IS CROSS-CHECKED AGAINST THE KERNEL'S OWN BOOT-ARGS.
         * Boot 475 had `-rbgpufbstrm` in kern.bootargs and RBFBStrmArmed
         * still read 0, because hubpSetup() re-initialised the flag AFTER
         * start() had parsed it. "Armed" and "the kext thinks it is armed"
         * are two different facts, and a tool that reads only the second
         * cannot tell a missing token from a clobbered one (rule 34, and
         * rule 14's shape: two causes, one symptom). */
        int tokenInArgs = 0;
        {
            char buf[4096];
            size_t len = sizeof(buf);
            if (sysctlbyname("kern.bootargs", buf, &len, NULL, 0) == 0) {
                const char *needle = "-rbgpufbstrm";
                const size_t nl = strlen(needle);
                for (const char *p = buf; (p = strstr(p, needle)) != NULL; p++) {
                    const char before = (p == buf) ? ' ' : p[-1];
                    const char after  = p[nl];
                    /* whole token only - the kernel's parse is exact and our
                     * string search is not (rule 30) */
                    if ((before == ' ' || before == '\t') &&
                        (after == 0 || after == ' ' || after == '\t')) {
                        tokenInArgs = 1;
                        break;
                    }
                }
            }
        }

        printf("arm -rbgpufbstrm: %s  (kernel boot-args: %s)\n",
               armed ? "yes" : "NO", tokenInArgs ? "present" : "absent");
        if (tokenInArgs && !armed) {
            printf("\n  *** THE TOKEN IS IN THE KERNEL'S BOOT-ARGS BUT THE "
                   "KEXT READS NOT ARMED. ***\n"
                   "  That is not a missing boot-arg - it is the flag being "
                   "cleared after it was parsed,\n"
                   "  which is what happened in boot 475 (hubpSetup() ran "
                   "after start()). Do not re-add\n"
                   "  the boot-arg; find what writes the flag last.\n\n");
        }
        if (!tokenInArgs && armed) {
            printf("\n  *** THE KEXT READS ARMED WITH NO TOKEN IN BOOT-ARGS. "
                   "***\n  Stale publish, or a different arm path. Do not "
                   "write on this.\n\n");
        }
        printf("licence RBFBStrencMsaMismatch: ");
        if (!haveMm) {
            printf("ABSENT\n");
        } else if (mm == 0) {
            printf("0 - the MSA derivation reproduces the live panel bit for "
                   "bit\n");
        } else if (mm == 16) {
            printf("16 - the derivation was REFUSED, not disagreed with\n");
        } else {
            printf("%ld - DISAGREES with the panel (mask: 1 PARAM1, 2 PARAM2, "
                   "4 PARAM3, 8 PARAM4)\n", mm);
        }
        printf("target pipe 1 derivation: rc %ld%s\n", p1rc,
               p1rc == 0 ? "" : " - NO RASTER on pipe 1 in this boot; "
                                "program it with otgprog first");
        printf("\n");

        /* TWO PHASES, BECAUSE LINK TRAINING GOES BETWEEN THEM.
         *
         * `go` configures the encoder and leaves DP_VID_STREAM_ENABLE 0.
         * `unblank` turns the stream on. Upstream splits these for exactly
         * this reason - `enable_link_dp` trains the link between
         * dp_set_stream_attribute and dp_unblank - and boot 477 proved why
         * one list cannot do: with the stream enabled, dptrain refuses with
         * RB_DPTX_W_STREAM_LIVE (66) and the chain locks out its own next
         * step. */
        int wantGo = 0;
        unsigned phase = (unsigned)RB_STRPROG_PHASE_ATTRIBUTE;
        unsigned inst = 2, otg = 1, pixkhz = 148500, rate = 20;
        for (int i = 1; i < argc; i++) {
            if (!strcmp(argv[i], "go")) {
                wantGo = 1;
                phase = (unsigned)RB_STRPROG_PHASE_ATTRIBUTE;
                continue;
            }
            if (!strcmp(argv[i], "unblank")) {
                wantGo = 1;
                phase = (unsigned)RB_STRPROG_PHASE_UNBLANK;
                continue;
            }
            if (!strncmp(argv[i], "inst=", 5))   { inst   = (unsigned)strtoul(argv[i]+5, NULL, 0); continue; }
            if (!strncmp(argv[i], "otg=", 4))    { otg    = (unsigned)strtoul(argv[i]+4, NULL, 0); continue; }
            if (!strncmp(argv[i], "pixkhz=", 7)) { pixkhz = (unsigned)strtoul(argv[i]+7, NULL, 0); continue; }
            if (!strncmp(argv[i], "rate=", 5))   { rate   = (unsigned)strtoul(argv[i]+5, NULL, 0); continue; }
            printf("unknown argument '%s'\n", argv[i]);
            IOObjectRelease(fb);
            return 2;
        }

        if (!wantGo) {
            printf("Read-only. Nothing was written.\n\n");
            printf("To program the stream, once the licence above reads 0 and "
                   "pipe 1's rc is 0, IN THIS ORDER:\n"
                   "  strencprog go            # configure; stream left OFF\n"
                   "  dptrain go tps=2         # train the link\n"
                   "  strencprog unblank       # turn the stream on\n\n"
                   "`go` leaves DP_VID_STREAM_ENABLE 0 on purpose: with it "
                   "set, dptrain refuses 66 STREAM_LIVE (boot 477).\n");
            IOObjectRelease(fb);
            return 0;
        }

        /* The panel, refused again here. */
        if (inst == 0) {
            printf("REFUSED: instance 0 is DIG0, the panel's live working DP "
                   "stream and the control every experiment on this line is "
                   "measured against.\n");
            IOObjectRelease(fb);
            return 3;
        }
        if (otg == 0) {
            printf("REFUSED: OTG0 drives the panel. Connecting another front "
                   "end to it takes the desktop down.\n");
            IOObjectRelease(fb);
            return 3;
        }
        if (!armed) {
            printf("REFUSED: not armed. Boot with -rbgpufbstrm:\n"
                   "  sudo tools/efi.sh bootarg add -rbgpufbstrm\n");
            IOObjectRelease(fb);
            return 3;
        }
        if (!haveMm || mm != 0) {
            printf("REFUSED: the licence is not 0, so there is no basis for "
                   "believing the MSA words are right. Fix the derivation "
                   "before writing anything.\n");
            IOObjectRelease(fb);
            return 3;
        }
        if (p1rc != 0) {
            printf("REFUSED: pipe 1 has no raster in THIS boot, so there is "
                   "nothing to derive a stream from. The raster is not "
                   "persistent - program it in this boot:\n"
                   "  sudo build/out/otgprog ... go\n");
            IOObjectRelease(fb);
            return 3;
        }

        printf("writing: phase %u (%s), inst %u (DIG%u) from OTG%u, "
               "%u kHz, link rate %u\n", phase,
               phase == (unsigned)RB_STRPROG_PHASE_ATTRIBUTE
                   ? "ATTRIBUTE - configure, stream left OFF so the link can "
                     "be trained"
                   : "UNBLANK - turn the stream ON",
               inst, inst, otg, pixkhz, rate);
        if (phase == (unsigned)RB_STRPROG_PHASE_UNBLANK) {
            printf("  (this is the half that blocks dptrain: train FIRST)\n");
        }

        /* Every key in ONE call: the dials and the trigger together, so the
         * kext can never run with a dial from a previous invocation (rule
         * 34). */
        NSDictionary *d = @{
            @"RBFBStrmPhase":    @(phase),
            @"RBFBStrmInst":     @(inst),
            @"RBFBStrmOtg":      @(otg),
            @"RBFBStrmPixKhz":   @(pixkhz),
            @"RBFBStrmLinkRate": @(rate),
            @"RBFBStrmGo":       @(1),
        };
        const kern_return_t kr =
            IORegistryEntrySetCFProperties(fb, (__bridge CFTypeRef)d);

        const long rc = rdnum(fb, "RBFBStrmRefuse");
        printf("\nsetProperties kr 0x%x, RBFBStrmRefuse %ld (%s)\n",
               kr, rc, refusal(rc));
        printf("ran %ld, writes %ld, applied %ld, changed mask 0x%lx, "
               "m_vid %ld\n",
               rdnum(fb, "RBFBStrmRan"), rdnum(fb, "RBFBStrmWrites"),
               rdnum(fb, "RBFBStrmApplied"),
               (unsigned long)rdnum(fb, "RBFBStrmChanged"),
               rdnum(fb, "RBFBStrmMVid"));

        if (rdnum(fb, "RBFBStrmRan") != 1) {
            printf("\nNothing was written. The refusal above says why.\n");
            IOObjectRelease(fb);
            return rc == 0 ? 0 : 3;
        }

        const long cb = rdnum(fb, "RBFBStrmCntlBefore");
        const long ca = rdnum(fb, "RBFBStrmCntlAfter");
        const long vd = rdnum(fb, "RBFBStrmVerdict");
        printf("\nDP_VID_STREAM_CNTL 0x%08lx -> 0x%08lx\n",
               (unsigned long)cb, (unsigned long)ca);
        printf("  bit 0  ENABLE (what we wrote):      %ld\n", ca & 1);
        printf("  bit 16 STATUS (what the HW says):   %ld\n", (ca >> 16) & 1);
        printf("  STATUS cleared after ENABLE 0:      %ld\n",
               rdnum(fb, "RBFBStrmStatusOff"));
        printf("VERDICT %ld = %s\n", vd, verdictStr(vd));

        printf("\nMSA read back: %08lx %08lx %08lx %08lx\n",
               (unsigned long)rdnum(fb, "RBFBStrmMsaAfter1"),
               (unsigned long)rdnum(fb, "RBFBStrmMsaAfter2"),
               (unsigned long)rdnum(fb, "RBFBStrmMsaAfter3"),
               (unsigned long)rdnum(fb, "RBFBStrmMsaAfter4"));
        printf("DP_STEER_FIFO after: 0x%08lx  (bit 0 RESET must now be 0)\n",
               (unsigned long)rdnum(fb, "RBFBStrmSteerAfter"));
        printf("DIG_FE_CNTL after:   0x%08lx  (bits 2:0 DIG_SOURCE_SELECT "
               "must be %u)\n",
               (unsigned long)rdnum(fb, "RBFBStrmFeAfter"), otg);

        printf("\nHOW TO READ THIS\n\n"
               "A STREAM IS NOT A PICTURE. There is no surface on pipe 1, so "
               "even RUNNING means the encoder is carrying a raster whose\n"
               "pixels are whatever the unprogrammed HUBP feeds it. That is a "
               "real intermediate result - read it as one, not as\n"
               "success and not as failure.\n\n"
               "The thing to check next is the SINK, not these registers: "
               "DPCD 0x202 over AUX2. Our own registers can only say our\n"
               "stores landed, which is rule 20's defect if taken as the "
               "answer.\n\n"
               "Use dptrain with tps=2. tps=3 and tps=4 DEGRADE this sink "
               "and the recovery is a reboot (2F8).\n");

        IOObjectRelease(fb);
        return vd == 1 ? 0 : 1;
    }
}
