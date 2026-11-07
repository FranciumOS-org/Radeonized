/* otgprog.m - start a SECOND RASTER on the pipe whose DP DTO is running.
 *
 * optc1_program_timing plus optc31_enable_crtc, sixteen writes, built and
 * gated entirely by rb_otgprog.h so this tool decides nothing the hosttest
 * cannot. It reads, it sends one setProperties call, it reads again, and it
 * prints what moved.
 *
 * THE MODE IS REQUIRED IN FULL except for the two sync polarities. A raster
 * is eight numbers and a default for any of them is a timing nobody chose on
 * a pipe whose clock is live. The one preset is `1080p60`, whose numbers are
 * 2BD's own reading of this sink's EDID detailed timing and which multiply
 * out to exactly the 148 500 000 Hz boot 461 programmed into the DTO.
 *
 * WHAT A `STILL` VERDICT MEANS HERE, said before anyone reads one: the
 * in-call frame count is sampled microseconds after the enable, and 1080p60
 * is one frame every 16 700 us. So STILL in the run record is the EXPECTED
 * reading and is NOT a failure. The real verdict is `RBFBOtgR<n>Frames`
 * moving between two periodic publishes, which this tool reads for you after
 * the call.
 */
#import <Foundation/Foundation.h>
#import <IOKit/IOKitLib.h>
#include "rb_otgprog.h"

/* 2FA: `found` is not optional here. Rule 14 at the tool boundary, in its own
 * documented form: a missing ioreg key read as -1 is 0xffffffff, which passes
 * most field tests. The first run of this tool proved it - against a live
 * kext that does not publish these keys yet, every pipe printed MASTER_EN,
 * because -1 & 1 is 1. An absent key and a zero register must not print the
 * same way. */
static long rdnum2(io_service_t fb, const char *name, int *found)
{
    CFStringRef key = CFStringCreateWithCString(NULL, name,
                                                kCFStringEncodingUTF8);
    CFTypeRef v = IORegistryEntryCreateCFProperty(fb, key,
                                                  kCFAllocatorDefault, 0);
    long out = -1;
    const int have = (v != NULL);
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
    if (found) { *found = have; }
    return out;
}

/* The absent case collapses to -1 for the many places that only print it. */
static long rdnum(io_service_t fb, const char *name)
{
    return rdnum2(fb, name, NULL);
}

static const char *refusal(long c)
{
    switch (c) {
    case RB_OTGPROG_W_OK:        return "none - the writes ran";
    case RB_OTGPROG_W_UNARMED:
        return "not armed - add -rbgpufbraster to boot-args";
    case RB_OTGPROG_W_NOREGS:
        return "no register window, or no sample this boot";
    case RB_OTGPROG_W_PANEL:
        return "pipe 0 is the PANEL's - refused by name";
    case RB_OTGPROG_W_INST:      return "no such pipe";
    case RB_OTGPROG_W_RUNNING:
        return "that pipe is ALREADY SCANNING - nothing here turns an OTG "
               "off, so this needs a reboot";
    case RB_OTGPROG_W_NOCLOCK:
        return "that pipe's DP DTO is not running - run `dmubbl dto` first";
    case RB_OTGPROG_W_MODE:
        return "the mode itself was refused (incomplete, or it does not "
               "close)";
    case RB_OTGPROG_W_CLOCKMISS:
        return "the mode does not multiply out to that pipe's DTO clock";
    case RB_OTGPROG_W_NOPANEL:
        return "the panel's own OTG0 is not scanning, so the kext is NOT "
               "addressing the OTG block - nothing read from it means "
               "anything, and no write will land where it is aimed";
    default:                     return "unknown";
    }
}

static const char *verdict(long v)
{
    switch (v) {
    case RB_OTGPROG_V_NOT_RUN:     return "NOT RUN - nothing was programmed";
    case RB_OTGPROG_V_SCANNING:    return "SCANNING - the frame count moved";
    case RB_OTGPROG_V_STILL:
        return "STILL in-call - EXPECTED, see the frame counts below";
    case RB_OTGPROG_V_NOT_ENABLED: return "NOT ENABLED - MASTER_EN did not "
                                          "read back";
    case RB_OTGPROG_V_REFUSED:     return "REFUSED";
    default:                       return "unknown";
    }
}

static void survey(io_service_t fb)
{
    printf("\n  all four timing generators, live\n");
    printf("    pipe  OTG_CONTROL  FRAME_COUNT\n");
    for (int i = 0; i < (int)RB_OTGPROG_COUNT; i++) {
        char k[32];
        int haveC = 0, haveF = 0;
        snprintf(k, sizeof(k), "RBFBOtgR%dCtl", i);
        long c = rdnum2(fb, k, &haveC);
        snprintf(k, sizeof(k), "RBFBOtgR%dFrames", i);
        long f = rdnum2(fb, k, &haveF);
        if (!haveC || !haveF) {
            printf("    OTG%d  (key absent - the LOADED kext does not "
                   "publish it)%s\n", i,
                   i == 0 ? "  <- the PANEL" : "");
            continue;
        }
        printf("    OTG%d  0x%08lx   %-10ld %s%s\n", i, c, f,
               (c & RB_OTGPROG_M_MASTER_EN) ? "MASTER_EN " : "",
               i == 0 ? " <- the PANEL, the positive control" : "");
    }
    printf("    (0x300 is a dark pipe. The panel's 0x00011201 with its\n"
           "     counter advancing is what a scanning OTG looks like, so a\n"
           "     0 beside it is a real 0 and not an instrument that is off.)\n");

    /* 2FA, boot 462: this is the gate's own input, and the first armed run
     * failed exactly here - all four generators read 0x00000000 because the
     * kext indexed its BAR5 map with the raw BASE_IDX 2 dword and never
     * added DMU segment 2's 0x34c0. Printed as a verdict rather than left
     * for a reader to notice in the table above (rule 45). */
    {
        int havePanel = 0;
        const long ok = rdnum2(fb, "RBFBOtgRPanelOk", &havePanel);
        if (!havePanel) {
            printf("\n  PANEL CONTROL: the kext does not publish it.\n");
        } else if (ok == 1) {
            printf("\n  PANEL CONTROL OK: OTG0 reads as a scanning "
                   "generator,\n  so this instrument is addressing the OTG "
                   "block.\n");
        } else {
            printf("\n  *** PANEL CONTROL FAILED ***\n"
                   "  OTG0 does not read as scanning. The panel IS lit, so\n"
                   "  this is the instrument, not the hardware: the kext is\n"
                   "  not addressing the OTG block. Every number above is a\n"
                   "  reading of the wrong address. `go` will REFUSE 48.\n");
        }
    }
}

int main(int argc, char **argv)
{
    unsigned long pipe = 1, refresh = 60;
    struct rb_otgprog_mode m;
    int go = 0, havePreset = 0;

    memset(&m, 0, sizeof(m));
    m.h_sync_positive = 1;
    m.v_sync_positive = 1;

    for (int a = 1; a < argc; a++) {
        if (strcmp(argv[a], "go") == 0) { go = 1; }
        else if (strcmp(argv[a], "1080p60") == 0) {
            /* 2BD read these off this sink's own EDID, first detailed timing
             * descriptor, and they multiply out to exactly 148.5 MHz. */
            m.h_addressable = 1920; m.h_front_porch = 88;
            m.h_sync_width  = 44;   m.h_total       = 2200;
            m.v_addressable = 1080; m.v_front_porch = 4;
            m.v_sync_width  = 5;    m.v_total       = 1125;
            refresh = 60; havePreset = 1;
        }
        else if (strncmp(argv[a], "pipe=", 5) == 0)
            pipe = strtoul(argv[a] + 5, NULL, 0);
        else if (strncmp(argv[a], "refresh=", 8) == 0)
            refresh = strtoul(argv[a] + 8, NULL, 0);
        else if (strncmp(argv[a], "hactive=", 8) == 0)
            m.h_addressable = (uint32_t)strtoul(argv[a] + 8, NULL, 0);
        else if (strncmp(argv[a], "hfront=", 7) == 0)
            m.h_front_porch = (uint32_t)strtoul(argv[a] + 7, NULL, 0);
        else if (strncmp(argv[a], "hsync=", 6) == 0)
            m.h_sync_width = (uint32_t)strtoul(argv[a] + 6, NULL, 0);
        else if (strncmp(argv[a], "htotal=", 7) == 0)
            m.h_total = (uint32_t)strtoul(argv[a] + 7, NULL, 0);
        else if (strncmp(argv[a], "vactive=", 8) == 0)
            m.v_addressable = (uint32_t)strtoul(argv[a] + 8, NULL, 0);
        else if (strncmp(argv[a], "vfront=", 7) == 0)
            m.v_front_porch = (uint32_t)strtoul(argv[a] + 7, NULL, 0);
        else if (strncmp(argv[a], "vsync=", 6) == 0)
            m.v_sync_width = (uint32_t)strtoul(argv[a] + 6, NULL, 0);
        else if (strncmp(argv[a], "vtotal=", 7) == 0)
            m.v_total = (uint32_t)strtoul(argv[a] + 7, NULL, 0);
        else if (strcmp(argv[a], "hsyncneg") == 0) m.h_sync_positive = 0;
        else if (strcmp(argv[a], "vsyncneg") == 0) m.v_sync_positive = 0;
        else {
            printf("usage: otgprog [go] [1080p60] [pipe=N] [refresh=HZ]\n"
                   "       [hactive= hfront= hsync= htotal=]\n"
                   "       [vactive= vfront= vsync= vtotal=]\n"
                   "       [hsyncneg] [vsyncneg]\n"
                   "\n"
                   "  With no `go` this prints the four generators and sends\n"
                   "  nothing. `1080p60` is 2BD's reading of this sink's own\n"
                   "  EDID: 1920x1080, h 88/44/2200, v 4/5/1125, which is\n"
                   "  exactly 148.5 MHz - the clock `dmubbl dto` programs.\n"
                   "  pipe defaults to 1, the first that is NOT the panel's;\n"
                   "  pipe 0 is refused here and in the kext.\n"
                   "  Sync polarities default POSITIVE, which is what that\n"
                   "  EDID says and what 1080p60 is in every standard.\n");
            return 2;
        }
    }

    if (pipe == 0) {
        printf("\nREFUSING to pass pipe 0 from this tool.\n"
               "Pipe 0 is the eDP PANEL this desktop is running on -\n"
               "OTG0_CONTROL 0x00011201 with OTG_MASTER_EN set, and 2F0\n"
               "measured its DP DTO driving it. Rewriting its raster takes\n"
               "the desktop down. The kext refuses it by name as well; this\n"
               "is the outer of the two checks.\n");
        return 2;
    }

    io_service_t fb = IOServiceGetMatchingService(
        kIOMainPortDefault, IOServiceMatching("RBAMDFramebuffer"));
    if (!fb) {
        fprintf(stderr, "no RBAMDFramebuffer - is the framebuffer armed?\n");
        return 1;
    }

    {
        int haveArmed = 0;
        const long armed = rdnum2(fb, "RBFBOtgRArmed", &haveArmed);
        if (!haveArmed) {
            printf("THE LOADED KEXT DOES NOT PUBLISH RBFBOtgR* AT ALL.\n"
                   "This build is newer than the one that is loaded - check\n"
                   "the UUID inside the collection, not in /Library/\n"
                   "Extensions (rule 15):\n"
                   "  kmutil inspect -B /Library/KernelCollections/"
                   "AuxiliaryKernelExtensions.kc \\\n"
                   "    --show-kext-uuids | grep -i rembrandt\n"
                   "Nothing below is a reading of this machine.\n");
        } else {
            printf("second raster is %s   sampled %s   aperture %s\n",
                   armed == 1 ? "ARMED" : "NOT ARMED",
                   rdnum(fb, "RBFBOtgRReady") == 1 ? "yes" : "no",
                   rdnum(fb, "RBFBOtgRAperture") == 1 ? "ok" : "NO");
        }
    }
    survey(fb);

    if (!go) {
        printf("\nread-only. add `go` to program a raster, and name a mode:\n"
               "  otgprog go 1080p60\n");
        IOObjectRelease(fb);
        return 0;
    }

    if (!havePreset && (!m.h_total || !m.v_total)) {
        printf("\nREFUSING: no mode. A raster is eight numbers and this tool\n"
               "defaults none of them - a default raster is a timing nobody\n"
               "chose, on a pipe whose clock is live. Use `1080p60`, or give\n"
               "all eight with refresh=.\n");
        IOObjectRelease(fb);
        return 2;
    }

    const long callsBefore = rdnum(fb, "RBFBOtgRProgCalls");

    /* Every key in ONE setProperties call: the kext reads the whole mode,
     * then the trigger, so the mode is complete before the write it governs
     * (rule 34). A multi-call handover is one that gets run with half the
     * mode from a previous invocation. */
    NSDictionary *d = @{
        @"RBFBOtgRPipe":      @(pipe),
        @"RBFBOtgRRefreshHz": @(refresh),
        @"RBFBOtgRHActive":   @(m.h_addressable),
        @"RBFBOtgRHFront":    @(m.h_front_porch),
        @"RBFBOtgRHSync":     @(m.h_sync_width),
        @"RBFBOtgRHTotal":    @(m.h_total),
        @"RBFBOtgRVActive":   @(m.v_addressable),
        @"RBFBOtgRVFront":    @(m.v_front_porch),
        @"RBFBOtgRVSync":     @(m.v_sync_width),
        @"RBFBOtgRVTotal":    @(m.v_total),
        @"RBFBOtgRHSyncPos":  @(m.h_sync_positive),
        @"RBFBOtgRVSyncPos":  @(m.v_sync_positive),
        @"RBFBOtgRGo":        @(1),
    };
    const kern_return_t kr =
        IORegistryEntrySetCFProperties(fb, (__bridge CFTypeRef)d);
    printf("\nprogramming pipe %lu: %ux%u, h %u/%u/%u, v %u/%u/%u, "
           "%lu Hz -> 0x%x\n",
           pipe, m.h_addressable, m.v_addressable,
           m.h_front_porch, m.h_sync_width, m.h_total,
           m.v_front_porch, m.v_sync_width, m.v_total, refresh, kr);

    /* Poll the MONOTONIC counter past the value read before the call, never
     * sleep and compare two snapshots of a dialled value (rule 31). */
    for (int i = 0; i < 400; i++) {
        if (rdnum(fb, "RBFBOtgRProgCalls") != callsBefore) break;
        usleep(10000);
    }

    const long rc = rdnum(fb, "RBFBOtgRCallRefuse");
    printf("\nTHIS CALL  (calls %ld)\n", rdnum(fb, "RBFBOtgRProgCalls"));
    printf("  refusal      %ld  %s\n", rc, refusal(rc));
    printf("  asked        pipe %ld  %ld Hz refresh  mode clock %ld Hz\n",
           rdnum(fb, "RBFBOtgRCallInst"), rdnum(fb, "RBFBOtgRCallRefreshHz"),
           rdnum(fb, "RBFBOtgRCallModeHz"));
    printf("  that pipe's DP DTO PHASE  %ld Hz\n",
           rdnum(fb, "RBFBOtgRCallDtoHz"));
    printf("  live OTG_CONTROL          0x%08lx\n",
           rdnum(fb, "RBFBOtgRCallLive"));

    printf("\nTHE LAST CALL THAT WROTE  (ran %ld)\n",
           rdnum(fb, "RBFBOtgRProgRan"));
    if (rdnum(fb, "RBFBOtgRProgRan") == 0) {
        printf("  none in this boot - nothing below is a reading.\n");
    } else {
        printf("  pipe         %ld,  %ld writes of 16\n",
               rdnum(fb, "RBFBOtgRProgInst"), rdnum(fb, "RBFBOtgRProgWrites"));
        printf("  raster       H_TOTAL %ld  V_TOTAL %ld  v front porch %ld\n",
               rdnum(fb, "RBFBOtgRProgHTotal"), rdnum(fb, "RBFBOtgRProgVTotal"),
               rdnum(fb, "RBFBOtgRProgVFront"));
        printf("               ^ the front porch is AFTER Linux's floor of 1\n"
               "                 (apply_front_porch_workaround), so it may\n"
               "                 differ from what was asked\n");
        printf("  OTG_CONTROL  0x%08lx -> 0x%08lx\n",
               rdnum(fb, "RBFBOtgRBefore"), rdnum(fb, "RBFBOtgRAfter"));
        printf("  readbacks    H_TOTAL 0x%08lx  V_TOTAL 0x%08lx\n",
               rdnum(fb, "RBFBOtgRHTotalAfter"),
               rdnum(fb, "RBFBOtgRVTotalAfter"));
        printf("               ^ raw only. These are our own numbers coming\n"
               "                 back, which is rule 20's defect and not a\n"
               "                 verdict.\n");
        printf("  frame count  %ld -> %ld  (in-call, microseconds apart)\n",
               rdnum(fb, "RBFBOtgRFramesBefore"),
               rdnum(fb, "RBFBOtgRFramesAfter"));
        printf("  VERDICT      %ld  %s\n", rdnum(fb, "RBFBOtgRProgVerdict"),
               verdict(rdnum(fb, "RBFBOtgRProgVerdict")));
    }

    /* The verdict that actually counts: the counter across two PERIODIC
     * publishes, which are far enough apart to resolve a 16.7 ms frame. */
    if (rc == RB_OTGPROG_W_OK) {
        char k[32];
        snprintf(k, sizeof(k), "RBFBOtgR%ldFrames",
                 rdnum(fb, "RBFBOtgRProgInst"));
        const long f0 = rdnum(fb, k);
        printf("\nTHE REAL VERDICT - %s across two periodic samples\n", k);
        printf("  now          %ld\n", f0);
        long f1 = f0;
        for (int i = 0; i < 300 && f1 == f0; i++) { usleep(10000); f1 = rdnum(fb, k); }
        printf("  then         %ld\n", f1);
        if (f1 != f0) {
            printf("  -> THE RASTER IS SCANNING. That counter is the\n"
                   "     hardware's own and we never write it.\n"
                   "     NOT established by this: a picture. No stream is\n"
                   "     mapped to this OTG (enc31_map_stream_to_link), no\n"
                   "     surface is attached, and the link is not trained.\n"
                   "     A dark monitor is still the EXPECTED reading.\n");
        } else {
            printf("  -> IT DID NOT MOVE in 3 s, which is ~180 frames at\n"
                   "     60 Hz, so this is not the in-call sampling problem.\n"
                   "     Read OTG_CONTROL above: MASTER_EN set with a still\n"
                   "     counter means the generator is enabled and not\n"
                   "     advancing, which points at the pixel clock feeding\n"
                   "     it rather than at these sixteen writes.\n");
        }
    }

    survey(fb);
    IOObjectRelease(fb);
    return rc == RB_OTGPROG_W_OK ? 0 : 3;
}
