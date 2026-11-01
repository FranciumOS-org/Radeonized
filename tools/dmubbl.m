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

/* rb_dmub.h's RB_DMUB_LOC_*. RENUMBERED after boot 272 - a code printed by
 * the old build does not mean what this table says. */
/* A 32-bit published value, masked.
 *
 * RULE 16, PAID FOR TWICE IN ONE MILESTONE. The kext publishes with
 * setProperty(..., 32), and a 32-bit OSNumber whose bit 31 is set comes back
 * through CFNumberGetValue as a NEGATIVE long - so DMCUB_INBOX1_BASE_ADDRESS
 * of 0x80000000 read as -2147483648. Boot 272 printed it as 0xffffffff8400ffff
 * and I read that as the register; boot 273's version guard then saw the
 * negative value, concluded the kext was too old, and SUPPRESSED A
 * SUCCESSFUL LOCATE. Mask here, and never test a reading's sign to decide
 * whether a key exists. */
static uint32_t u32(io_service_t fb, const char *key)
{
    return (uint32_t)(num(fb, key) & 0xffffffffLL);
}

/* Does the key exist at all? The only honest version check. */
static int have(io_service_t fb, const char *key)
{
    CFTypeRef v = IORegistryEntryCreateCFProperty(
        fb, (__bridge CFStringRef)[NSString stringWithUTF8String:key],
        kCFAllocatorDefault, 0);
    if (!v) { return 0; }
    CFRelease(v);
    return 1;
}

static const char *locWhy(long c)
{
    switch (c) {
    case 0: return "OK";
    case 1: return "no BAR5 map (is -rbgpufbbacklight on?)";
    case 2: return "INBOX1_BASE/SIZE cannot be a ring";
    case 3: return "no enabled window carries a command signature";
    case 4: return "that window's offset pair is zero";
    case 5: return "FB_LOCATION/FB_OFFSET give no ranges to classify against";
    case 6: return "the address is neither MC nor system-physical";
    case 7: return "the ring is not 64-byte aligned";
    case 8: return "resolved but NOT mappable";
    default: return "unknown";
    }
}

static const char *kindWhy(long k)
{
    switch (k) {
    case 1: return "MC (mc2pa applied)";
    case 2: return "already system-physical";
    default: return "none";
    }
}

/* A CFData property's raw bytes, up to n. */
static int rawbytes(io_service_t fb, const char *key, uint8_t *out, int n)
{
    CFTypeRef v = IORegistryEntryCreateCFProperty(
        fb, (__bridge CFStringRef)[NSString stringWithUTF8String:key],
        kCFAllocatorDefault, 0);
    int got = 0;
    if (v) {
        if (CFGetTypeID(v) == CFDataGetTypeID()) {
            const CFIndex len = CFDataGetLength((CFDataRef)v);
            const uint8_t *p = CFDataGetBytePtr((CFDataRef)v);
            for (; got < n && (CFIndex)got < len; got++) { out[got] = p[got]; }
        }
        CFRelease(v);
    }
    return got;
}

static const char *replyWhy(long v)
{
    switch (v) {
    case 1: return "ZERO       - the slot reads all zeros";
    case 2: return "UNCHANGED  - our bytes, untouched";
    case 3: return "CONFIRMED  - the firmware cleared ret_status";
    case 4: return "CHANGED    - differs, but not that way";
    default: return "NONE       - nothing sent yet";
    }
}

/* The reply block: the two buffers and what the difference means. This is
 * the one reading on this surface that is not an inference from a pointer. */
static void dumpReply(io_service_t fb)
{
    uint8_t sent[64] = {0}, got[64] = {0};
    const int ns = rawbytes(fb, "RBFBDmubReplySent", sent, 64);
    const int ng = rawbytes(fb, "RBFBDmubReplyGot", got, 64);
    const long v = num(fb, "RBFBDmubReplyVerdict");
    int i;

    if (!have(fb, "RBFBDmubReplyVerdict")) { return; }

    printf("  caps sends     %ld   confirms %ld\n",
           num(fb, "RBFBDmubCapsSends"), num(fb, "RBFBDmubConfirms"));
    printf("  reply slot     0x%x   verdict %ld %s\n",
           u32(fb, "RBFBDmubReplySlot"), v, replyWhy(v));
    if (ns < 64 || ng < 64) {
        printf("  (buffers absent - the kext did not publish 64 bytes)\n");
        return;
    }
    printf("    sent");
    for (i = 0; i < 20; i++) { printf(" %02x", sent[i]); }
    printf("\n    got ");
    for (i = 0; i < 20; i++) { printf(" %02x", got[i]); }
    printf("\n           ");
    for (i = 0; i < 20; i++) { printf(" %s", sent[i] == got[i] ? "  " : "^^"); }
    printf("\n");

    if (v == 3) {
        printf("  -> THE MAILBOX IS IDENTIFIED AT THE BYTE LEVEL. byte 2 is\n"
               "     ret_status; we sent it SET and it came back CLEAR, with\n"
               "     type and sub_type intact. dc_dmub_srv_query_caps_cmd is\n"
               "     the code that does that, and dmub_rb_get_return_data\n"
               "     reads the reply at rptr - 64, which is this slot. Only\n"
               "     the DMCUB clears that bit, so CW4 is its inbox and our\n"
               "     stores land in the memory it reads.\n");
        printf("     feature caps: psr %u  mclk_sw_ver %u  subvp_psr %u\n"
               "                   gecc %u  replay %u  ABM_AUX_BL %u  lsdma %u\n",
               got[4], got[5], got[10], got[11], got[12], got[16], got[17]);
        printf("     -> abm_aux_backlight_support %u: %s\n", got[16],
               got[16] ? "this firmware drives the backlight over AUX, so a "
                         "PWM-typed SET_BACKLIGHT is the wrong control type"
                       : "NOT over AUX, so DMU_BACKLIGHT_CONTROL_PWM is the "
                         "right type and the level should take");
    } else if (v == 2) {
        printf("  -> our bytes are in that memory and nothing replied. For an\n"
               "     ABM command that is EXPECTED (dmub_abm_lcd.c never sets\n"
               "     ret_status), so it says the write stuck and nothing about\n"
               "     the address. Send `dmubbl caps` for the discriminator.\n");
    } else if (v == 1) {
        printf("  -> THE SLOT READS ZERO. Our store did not land in the memory\n"
               "     this mapping reads, so every ack so far was the DMCUB\n"
               "     consuming someone else's bytes - boot 273's failure mode\n"
               "     again, and CW4 is retired as the mailbox despite six\n"
               "     exact layout matches.\n");
    } else if (v == 4) {
        printf("  -> something writes this memory and not the way dc_dmub_srv.c\n"
               "     describes. Read the two rows above rather than the verdict.\n");
    }
}

/* A CFData property as an array of 28 uint32s: seven windows, four each. */
static int words(io_service_t fb, const char *key, uint32_t *out, int n)
{
    CFTypeRef v = IORegistryEntryCreateCFProperty(
        fb, (__bridge CFStringRef)[NSString stringWithUTF8String:key],
        kCFAllocatorDefault, 0);
    int got = 0;
    if (v) {
        if (CFGetTypeID(v) == CFDataGetTypeID()) {
            const CFIndex len = CFDataGetLength((CFDataRef)v);
            const uint8_t *p = CFDataGetBytePtr((CFDataRef)v);
            for (; got < n && (CFIndex)((got + 1) * 4) <= len; got++) {
                memcpy(&out[got], p + got * 4, 4);
            }
        }
        CFRelease(v);
    }
    return got;
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

/* The SEARCH is the control for the whole derivation (rule 26). 2zn named a
 * window and boot 272 refuted it; the correction then found 2zn's
 * DMUB_CW4_BASE had been copied out of the register it was meant to be
 * checked against. So nothing is named now: every enabled window is
 * resolved, mapped and read, and the one carrying the firmware's own 37
 * bring-up commands is the mailbox. */
static void dump(io_service_t fb)
{
    long loc = num(fb, "RBFBDmubLocRefuse");
    long win = num(fb, "RBFBDmubWin");
    uint32_t wr[28] = {0}, ws[28] = {0};
    int nwr = words(fb, "RBFBDmubWindows", wr, 28);
    int nws = words(fb, "RBFBDmubWinScan", ws, 28);

    /* Rule 15, as the FIRST thing printed - and by PRESENCE, not by sign.
     * The previous version tested `num(...) < 0`, which is true for any
     * 32-bit value with bit 31 set, so on boot 273 it declared the kext too
     * old and hid a locate that had in fact succeeded. The instrument is a
     * suspect (rule 16). */
    if (!have(fb, "RBFBDmubLayout") || nwr < 28) {
        printf("the running kext does not publish this tool's keys - it is\n"
               "OLDER than this build (rule 15). Nothing below is a reading;\n"
               "install the kext this tool was built with and reboot.\n");
        return;
    }

    printf("armed            %ld   (-rbgpufbdmubbl)\n",
           num(fb, "RBFBDmubBlArmed"));
    printf("locate           %ld  %s\n", loc, locWhy(loc));
    printf("  layout         %u of %u CW bases match dmub_srv.c%s\n",
           u32(fb, "RBFBDmubLayout"), u32(fb, "RBFBDmubLayoutOf"),
           u32(fb, "RBFBDmubLayout") == u32(fb, "RBFBDmubLayoutOf")
               ? "  <- the identification" : "  <- INCOMPLETE");
    printf("  INBOX1 base    0x%08x   size %u   (unexplained, unused)\n",
           u32(fb, "RBFBDmubInbox1Base"), u32(fb, "RBFBDmubInbox1Size"));
    printf("  FB loc/offset  0x%x / 0x%x\n",
           u32(fb, "RBFBDmubFbLocation"), u32(fb, "RBFBDmubFbOffset"));

    /* The whole window table, raw. This is what refuted 2zn, and a search
     * that printed only its own winner would have had nothing to say. */
    if (nwr >= 28) {
        printf("  region-3 windows (base / top / offset, * = enabled):\n");
        for (int i = 0; i < 7; i++) {
            const uint32_t b = wr[i * 4 + 0], t = wr[i * 4 + 1];
            const uint32_t ol = wr[i * 4 + 2], oh = wr[i * 4 + 3];
            printf("    CW%d %c 0x%08x .. 0x%08x   off 0x%x_%08x",
                   i, (t & 0x80000000u) ? '*' : ' ', b & 0x1fffffffu,
                   t & 0x1fffffffu, oh, ol);
            if (nws >= 28) {
                printf("   [resolve %u, run %u, bad %u, abm %u]",
                       ws[i * 4 + 0], ws[i * 4 + 1], ws[i * 4 + 2],
                       ws[i * 4 + 3]);
            }
            printf("\n");
        }
        printf("    (addresses are 29-bit fields, so 0x64000000 is written "
               "as 0x04000000)\n");
    }

    if (win >= 0) {
        printf("mailbox          CW%ld\n", win);
        printf("  address kind   %ld  %s\n", num(fb, "RBFBDmubAddrKind"),
               kindWhy(num(fb, "RBFBDmubAddrKind")));
        printf("  window mem     0x%llx\n",
               (unsigned long long)num(fb, "RBFBDmubMem"));
        printf("  ring pa        0x%llx  (carve-out + 0x%llx)\n",
               (unsigned long long)num(fb, "RBFBDmubRingPA"),
               (unsigned long long)num(fb, "RBFBDmubRingFbOff"));
        printf("  capacity       %u bytes\n", (uint32_t)num(fb, "RBFBDmubCap"));
        printf("  map route      %ld   (1 = BAR0 sub-range, 2 = physical)\n",
               num(fb, "RBFBDmubRoute"));
        printf("  live WPTR/RPTR 0x%x / 0x%x\n",
               u32(fb, "RBFBDmubWptr"), u32(fb, "RBFBDmubRptr"));
        printf("  signature      %ld of %ld slots, %ld ABM  -> %s\n",
               num(fb, "RBFBDmubScanPlaus"), num(fb, "RBFBDmubScanSlots"),
               num(fb, "RBFBDmubScanAbm"),
               u32(fb, "RBFBDmubSigOk") ? "present" : "ABSENT");
        if (!u32(fb, "RBFBDmubSigOk")) {
            printf("  -> NO byte-level confirmation. Boot 273 read this ring as\n"
                   "     ALL ZEROS while the registers said WPTR == RPTR ==\n"
                   "     2368, so \"37 commands sit at bring-up\" was an\n"
                   "     inference from the POINTER and never from memory. The\n"
                   "     address rests on the layout agreement alone, which is\n"
                   "     six exact matches - strong, but not the same thing.\n");
        }
    } else {
        printf("mailbox          NONE FOUND - nothing is mapped and nothing\n"
               "                 will be sent. Read the per-window [resolve,\n"
               "                 run, bad] columns above: resolve 0 means the\n"
               "                 window was a candidate and its memory did not\n"
               "                 look like a ring.\n");
    }

    printf("sends ok/refused %ld / %ld   last refusal %ld %s\n",
           num(fb, "RBFBDmubSendsOk"), num(fb, "RBFBDmubSendsRefused"),
           num(fb, "RBFBDmubSendRefuse"),
           sendWhy(num(fb, "RBFBDmubSendRefuse")));
    printf("  backlight/pipe %ld / %ld   last level %ld\n",
           num(fb, "RBFBDmubBlSends"), num(fb, "RBFBDmubPipeSends"),
           num(fb, "RBFBDmubBlLast"));
    printf("  acks/timeouts  %ld / %ld   last WPTR 0x%x\n",
           num(fb, "RBFBDmubAcks"), num(fb, "RBFBDmubAckTimeouts"),
           u32(fb, "RBFBDmubSendWptr"));
    printf("  -> an ack is RPTR reaching our WPTR, i.e. the DMCUB consumed\n"
           "     whatever was at the REAL ring's WPTR. On boot 273 that was a\n"
           "     zeroed slot - DMUB_CMD__NULL - because the command had gone\n"
           "     into the wrong window, so the ack said nothing about it. An\n"
           "     ack is necessary and nowhere near sufficient; the panel is\n"
           "     the deliverable and your eyes are its only instrument.\n");
    dumpReply(fb);
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
               "  dmubbl pipe         SET_PIPE, bind ABM to OTG0\n"
               "  dmubbl caps         QUERY_FEATURE_CAPS - READ-ONLY on the\n"
               "                      chip, and the only command whose reply\n"
               "                      the firmware writes back into our own\n"
               "                      slot. Send this FIRST: it is what turns\n"
               "                      an ack into a byte-level identification.\n");
        IOObjectRelease(fb);
        return 0;
    }

    /* Say what the address rests on, immediately before acting on it. Boot
     * 273 wrote into the trace buffer because the selector was wrong, and
     * there is still no byte-level confirmation of the right one. */
    if (!u32(fb, "RBFBDmubSigOk")) {
        printf("\nNOTE: the mailbox is identified by the LAYOUT (six exact CW\n"
               "matches) and not by its contents, which read empty. That is\n"
               "the best evidence available and it is not proof.\n");
    }

    NSDictionary *d;
    if (strcmp(argv[1], "caps") == 0) {
        d = @{ @"RBFBDmubCaps": @(1) };
        printf("\nsending QUERY_FEATURE_CAPS (read-only, ret_status set)...\n");
    } else if (strcmp(argv[1], "pipe") == 0) {
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
