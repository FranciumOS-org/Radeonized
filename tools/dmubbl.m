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

static int words(io_service_t fb, const char *key, uint32_t *out, int n);

/* Regions 4/5/6 and the candidate table (2zs). */
static void dumpRegions(io_service_t fb)
{
    uint32_t rr[9] = {0}, rs[9] = {0}, ck[10] = {0};
    const uint32_t inbox = u32(fb, "RBFBDmubInbox1Base");
    int i;

    if (!have(fb, "RBFBDmubInboxInR3")) { return; }
    words(fb, "RBFBDmubRegions", rr, 9);
    words(fb, "RBFBDmubRegScan", rs, 9);
    words(fb, "RBFBDmubCandOk", ck, 10);

    printf("the inbox base the FIRMWARE wrote: 0x%08x\n", inbox);
    if (!num(fb, "RBFBDmubInboxInR3")) {
        printf("  -> OUTSIDE region 3 (CW0..CW6 span 0x60000000..0x66ffffff).\n"
               "     dmub_dcn31_setup_mailbox writes inbox1->base UNTRANSLATED,\n"
               "     and Linux sets it to cw4.region.base = 0x64000000. This\n"
               "     firmware wrote something else, so CW4 is NOT the mailbox -\n"
               "     and boot 275's unanswered caps query was a correct reading\n"
               "     of a wrong address. Regions 4/5/6 are the other apertures:\n"
               "     fixed base in hardware, only OFFSET and TOP programmable.\n");
    } else {
        printf("  -> inside region 3, so a CW window can be the mailbox.\n");
    }
    printf("  apertures (offset / top, * = enabled):\n");
    for (i = 0; i < 3; i++) {
        const uint32_t lo = rr[i * 3 + 0], hi = rr[i * 3 + 1];
        const uint32_t tp = rr[i * 3 + 2];
        printf("    REGION%d %c 0x%x_%08x  top 0x%08x   [resolve %u, run %u, "
               "bad %u, abm %u]\n", 4 + i, (tp & 0x80000000u) ? '*' : ' ',
               hi, lo, tp, ck[7 + i], rs[i * 3 + 0], rs[i * 3 + 1],
               rs[i * 3 + 2]);
    }
    printf("  selector       %ld   (0..6 = CW0..CW6, 7/8/9 = REGION4/5/6)\n",
           num(fb, "RBFBDmubWinSel"));
    printf("  candidates OK  ");
    for (i = 0; i < 10; i++) {
        if (ck[i] == 0) { printf("%d ", i); }
    }
    printf("\n  -> `dmubbl win <n>` points the push at one of those, then\n"
           "     `dmubbl caps` says whether the firmware answers THERE. The\n"
           "     caps query works; it only ever needed the right address.\n");
}

/* The GPINT block. No address appears anywhere in it, which is why it exists:
 * boot 275's ring result is consistent with an unimplemented command, a wrong
 * base and a DMCUB that is not running commands at all, and every one of
 * those produces an advancing RPTR. */
static void dumpGpint(io_service_t fb)
{
    const uint32_t sent = u32(fb, "RBFBDmubGpintSent");
    const uint32_t back = u32(fb, "RBFBDmubGpintBack");
    const uint32_t s7p  = u32(fb, "RBFBDmubScratch7Pre");
    const uint32_t s7   = u32(fb, "RBFBDmubScratch7");
    const uint32_t s0   = u32(fb, "RBFBDmubScratch0");
    const long acked = num(fb, "RBFBDmubGpintAcked");
    const long sends = num(fb, "RBFBDmubGpintSends");

    if (!have(fb, "RBFBDmubGpintSends")) { return; }

    printf("GPINT (no address involved)\n");
    printf("  sends/refuse   %ld / %ld\n", sends,
           num(fb, "RBFBDmubGpintRefuse"));
    if (sends == 0) {
        printf("  -> not sent yet. `dmubbl gpint` writes ONE register,\n"
               "     DMCUB_GPINT_DATAIN1, with DMUB_GPINT__GET_FW_VERSION and\n"
               "     reads DMCUB_SCRATCH7. STOP_FW is refused in src/common,\n"
               "     exhaustively over codes 2..144, because it would halt the\n"
               "     firmware that is driving this panel.\n");
        return;
    }
    printf("  sent / back    0x%08x / 0x%08x   acked %ld\n", sent, back, acked);
    printf("  SCRATCH7       0x%08x -> 0x%08x   (pre -> post)\n", s7p, s7);
    printf("  SCRATCH0       0x%08x   DATAOUT 0x%08x\n", s0,
           u32(fb, "RBFBDmubDataout"));
    printf("  version bytes  %u.%u.%u.%u from SCRATCH0,  %u.%u.%u.%u "
           "from SCRATCH7\n",
           (s0 >> 24) & 0xff, (s0 >> 16) & 0xff, (s0 >> 8) & 0xff, s0 & 0xff,
           (s7 >> 24) & 0xff, (s7 >> 16) & 0xff, (s7 >> 8) & 0xff, s7 & 0xff);

    if (acked) {
        printf("  -> THE DMCUB IS ALIVE AND ANSWERING US. It cleared the status\n"
               "     field of a register we wrote, with no memory, no window and\n"
               "     no mc2pa anywhere in the path. So the ring is the problem,\n"
               "     not the microcontroller: either command 6 is not implemented\n"
               "     by 4.0.15.47 or it reads a base other than CW4's offset.\n");
        if (s7 != s7p) {
            printf("     SCRATCH7 MOVED, so that value is a reply and not a\n"
                   "     leftover. Check it against 4.0.15.47, which a level-2\n"
                   "     probe read from SCRATCH0 - a DIFFERENT register.\n");
        } else {
            printf("     SCRATCH7 did NOT move, so whatever it holds was already\n"
                   "     there and is not this query's answer (rule 26's control).\n");
        }
    } else {
        printf("  -> NO ACK. The status field we set is still set, so the DMCUB\n"
               "     did not take a register write that involves no addressing\n"
               "     at all. That retires the ring as the question: the inbox\n"
               "     RPTR advancing is then not the firmware executing our\n"
               "     commands, and 2zq's four acks meant something else.\n");
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
        /* 2zt: NAME THE WINDOW THE SELECTOR'S OWN WAY. Slots 0..6 are the
         * region-3 CW windows; 7, 8 and 9 are REGION4/5/6, which are not CW
         * windows at all and printing them as "CW7" was a reading hazard of
         * exactly the kind rule 33 is about - 2zs identified REGION4 as the
         * mailbox and the tool kept calling it CW7. */
        static const char *const wn[] = { "CW0", "CW1", "CW2", "CW3", "CW4",
                                          "CW5", "CW6", "REGION4", "REGION5",
                                          "REGION6" };
        printf("mailbox          %s\n",
               (win >= 0 && win < 10) ? wn[win] : "?");
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
    {
        const long t = num(fb, "RBFBDmubBlType");
        printf("  control type   %ld (%s), aux_inst %ld, changed %ld time(s)\n",
               t, t == 0 ? "PWM" : (t == 1 ? "VESA_AUX"
                                            : (t == 2 ? "AMD_AUX" : "?")),
               num(fb, "RBFBDmubBlAuxInst"), num(fb, "RBFBDmubBlTypeSets"));
        if (t == 0) {
            printf("  -> PWM is the type boot 270 already FALSIFIED on its "
                   "own:\n"
                   "     BL_PWM_CNTL held our duty cycle for 40 s with "
                   "RBFBBlLiveDiffs\n"
                   "     pinned at 25 (nothing else writing it) and the panel "
                   "unmoved.\n"
                   "     `dmubbl type 1` then a level is the untried case.\n");
        }
    }
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
    dumpGpint(fb);
    dumpRegions(fb);
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
               "                      an ack into a byte-level identification.\n"
               "  dmubbl gpint        GET_FW_VERSION over GPINT - ONE register\n"
               "                      write, no address anywhere. Answers\n"
               "                      whether the DMCUB talks to us at all.\n"
               "  dmubbl dom [inst]   DOMAIN_CONTROL power-up, the 2DC\n"
               "                      execution test. ONE call: the kext\n"
               "                      samples the four DOMAIN_PG_STATUS\n"
               "                      words, sends, and samples again.\n"
               "                      RBFBDmubDomMoved non-zero means the\n"
               "                      firmware EXECUTED it, which no other\n"
               "                      instrument here can show. Needs\n"
               "                      -rbgpufbdmubdom.\n"
               "  dmubbl win <0..9>   point the push at a window: 0..6 are\n"
               "                      CW0..CW6, 7/8/9 are REGION4/5/6. Then\n"
               "                      `dmubbl caps` tests THAT address.\n");
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
    if (strcmp(argv[1], "win") == 0) {
        long n = argc > 2 ? atol(argv[2]) : -1;
        if (n < 0 || n > 9) {
            fprintf(stderr, "dmubbl win <0..9>\n");
            IOObjectRelease(fb);
            return 1;
        }
        d = @{ @"RBFBDmubSel": @(n) };
        printf("\nselecting window %ld...\n", n);
    } else if (strcmp(argv[1], "gpint") == 0) {
        d = @{ @"RBFBDmubGpint": @(1) };
        printf("\nsending GPINT GET_FW_VERSION (one register, no address)...\n");
    } else if (strcmp(argv[1], "caps") == 0) {
        d = @{ @"RBFBDmubCaps": @(1) };
        printf("\nsending QUERY_FEATURE_CAPS (read-only, ret_status set)...\n");
    } else if (strcmp(argv[1], "type") == 0) {
        /* 2zt: the control type for every SUBSEQUENT send. Sends nothing
         * itself, so a type change and a level change are never the same
         * observation. `dmubbl type 1` then `dmubbl 65536`. */
        if (argc < 3) {
            printf("usage: dmubbl type <0=PWM|1=VESA_AUX|2=AMD_AUX> "
                   "[aux_inst]\n");
            IOObjectRelease(fb);
            return 2;
        }
        long t = atol(argv[2]);
        long ai = (argc > 3) ? atol(argv[3]) : 0;
        if (t < 0 || t > 2 || ai < 0 || ai > 255) {
            printf("refusing type %ld aux_inst %ld - type is 0..2, "
                   "aux_inst 0..255\n", t, ai);
            IOObjectRelease(fb);
            return 2;
        }
        d = @{ @"RBFBDmubBlType": @(t), @"RBFBDmubBlAuxInst": @(ai) };
        printf("\nsetting backlight_control_type = %ld (%s), aux_inst %ld "
               "- NO COMMAND IS SENT\n", t,
               t == 0 ? "PWM" : (t == 1 ? "VESA_AUX" : "AMD_AUX"), ai);
    } else if (strcmp(argv[1], "pipe") == 0) {
        d = @{ @"RBFBDmubPipe": @(1) };
        printf("\nsending SET_PIPE (bind ABM to OTG0)...\n");
    } else if (strcmp(argv[1], "dom") == 0) {
        /* 2DC: the DOMAIN_CONTROL experiment, which is ONE setProperties
         * call - the kext samples the four DOMAIN_PG_STATUS words, sends the
         * command and samples them again. Nothing here needs a window
         * selected first, nothing needs the previous command to have landed,
         * and no reboot can split it: four sessions in a row were lost to a
         * two-step handover where the first step had quietly reverted. */
        long inst = (argc > 2) ? atol(argv[2]) : -1;
        long gate = (argc > 3) ? atol(argv[3]) : 0;

        if (gate != 0 && gate != 1) {
            printf("usage: dmubbl dom [inst [0=power up|1=power gate]]\n"
                   "       inst omitted or negative = let the kext pick the\n"
                   "       one domain whose state differs from DOMAIN0's\n");
            IOObjectRelease(fb);
            return 2;
        }
        if (gate == 1) {
            printf("\nREFUSING to pass power_gate=1 from this tool.\n"
                   "Powering a display domain DOWN is the one direction that\n"
                   "can take something away; the test only needs a power-UP,\n"
                   "and the kext refuses DOMAIN0 either way. Edit the kext if\n"
                   "a gate is ever genuinely wanted.\n");
            IOObjectRelease(fb);
            return 2;
        }
        d = @{ @"RBFBDmubDomain": @((int32_t)inst),
               @"RBFBDmubDomainGate": @(0) };
        if (inst < 0) {
            printf("\nsending DOMAIN_CONTROL power-up to the kext's own\n"
                   "candidate (the first domain unlike DOMAIN0)...\n");
        } else {
            printf("\nsending DOMAIN_CONTROL power-up to DOMAIN%ld...\n", inst);
        }
        printf("this is the FIRST DMUB_CMD__VBIOS command this project has\n"
               "ever sent. The verdict is RBFBDmubDomMoved - a bitmask of\n"
               "which DOMAIN_PG_STATUS words changed across the send. Any\n"
               "non-zero value means the firmware EXECUTED an inbox command,\n"
               "which no other instrument here can show (2DB).\n");
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
