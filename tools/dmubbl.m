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

/* 2DD: the dom refusal codes, decoded. Boot 436 printed "-> 0xe00002e2" and
 * that is the IOReturn of the setProperties call, which says the handler
 * refused and nothing about WHY (rule 13). */
static const char *domWhy(long c)
{
    switch (c) {
    case 0:  return "no refusal - the command was sent";
    case 20: return "not armed (-rbgpufbdmubdom missing from boot-args)";
    case 21: return "instance outside the 6-bit payload field";
    case 22: return "no register mapping (hubpSetup refused this boot)";
    case 23: return "DOMAIN0 named by hand, or an instance past DOMAIN3";
    case 24: return "no domain differs from DOMAIN0 - nothing to power up "
                    "(boot 437: all seven domains in one state)";
    case 25: return "the DOMAIN block read as eight ZERO words - the sample "
                    "is unreadable, so this is an INSTRUMENT failure and not "
                    "a reading of the chip";
    default: return "unknown";
    }
}

/* Wait on something MONOTONIC, never on the value being interpreted: the
 * properties publish every 500 ms, so a read straight after the set can
 * return the previous verdict and look exactly like a lost write (2C5). */
/* 2DE: the DOMAIN NUMBER each slot carries, in the kext's table order. */
#define DOM_SLOTS 7
static const unsigned domNums[DOM_SLOTS] = { 0, 1, 2, 3, 16, 17, 18 };

static void domRow(const char *label, const uint32_t *w, int haveIt)
{
    int i;

    printf("  %-14s", label);
    if (!haveIt) {
        printf("not sampled - the refusal came before the snapshot\n");
        return;
    }
    for (i = 0; i < DOM_SLOTS; i++) { printf(" %08x", w[i]); }
    printf("\n");
}

/* 2E2: the DIG1 transmitter refusal codes. Numbered above the DOMAIN ones
 * (20-25) on purpose, so a code read out of the wrong key cannot be decoded
 * against the wrong table and come back plausible. */
/* The one-hot names, from dcn10_link_encoder.c:53-60. Printed rather than the
 * bare number because reading 0x4 as "front end 4" instead of DIGC is 2E1's
 * hpdsel trap with the arrow reversed. */
static const char *feName(long m)
{
    switch (m) {
    case 0x00: return "INVALID - no front end";
    case 0x01: return "DIGA (the panel's; refused)";
    case 0x02: return "DIGB";
    case 0x04: return "DIGC";
    case 0x08: return "DIGD";
    case 0x10: return "DIGE";
    default:   return "not a one-hot front end";
    }
}

/* 2EC: DIG_FE_SOURCE_SELECT's refusals, 60-69, numbered clear of rb_dig's
 * 30-37, rb_enc's 40-45 and rb_dpaltq's 50-55. Three separate codes for a bad
 * front-end mask, because 0, two bits and a sixth front end are three
 * different mistakes (rule 14). */
static const char *feWhy(long c)
{
    switch (c) {
    case 0:  return "no refusal - the connection was written";
    case 60: return "not armed (-rbgpufbfe missing from boot-args)";
    case 61: return "no writable BAR5 window, or the aperture does not cover "
                    "the DIG block";
    case 62: return "back end 0 - the PANEL's (2E8 read FE_SEL 0x1 DIGA on "
                    "DIG0). Refused by name";
    case 63: return "back-end instance outside 0..4";
    case 64: return "front end 0 - DCN10_DIG_FE_SOURCE_SELECT_INVALID, i.e. "
                    "no front end named. This is the DEFAULT, so an armed "
                    "boot that names none writes nothing";
    case 65: return "the front-end value is not one-hot. The field is a MASK "
                    "(DIGA 0x1, DIGB 0x2, DIGC 0x4, DIGD 0x8, DIGE 0x10), "
                    "never an index";
    case 66: return "a front end above DIGE - this part has five "
                    "(num_stream_encoder = 5)";
    case 67: return "front end DIGA - the PANEL's own stream encoder, and "
                    "this field ACCUMULATES rather than replaces, so ORing "
                    "it elsewhere would put the live desktop's encoder on a "
                    "second back end. Refused by name";
    case 68: return "the live word does not decode to a DIG_MODE this part "
                    "writes, so that dword is NOT DIG_BE_CNTL - refused "
                    "BEFORE the store (2DD read a plausible wrong address; "
                    "this would have written one)";
    case 69: return "the dword is on the dcn302/dcn312 deny list";
    default: return "unknown";
    }
}

static const char *digWhy(long c)
{
    switch (c) {
    case 0:  return "no refusal - the command was sent";
    /* 1..5 are dmubPush's own, shared with every other send here. */
    case 1:  return "the DMUB half is not armed (-rbgpufbdmubbl missing)";
    case 2:  return "no ring mapping or no register window";
    case 3:  return "the ring address was never identified this boot";
    case 4:  return "WPTR or RPTR outside the ring";
    case 5:  return "the ring is full";
    case 30: return "not armed (-rbgpufbdig missing from boot-args)";
    case 31: return "no RDPCSTX mapping - the product cannot be read, so a "
                    "send would be unjudgeable";
    case 32: return "that phyid is PHY0, the eDP PANEL (2E1's display path 0 "
                    "on UNIPHY_A, and 2C1's only lit PHY). Refused by name";
    case 33: return "phyid outside 0..5, or outside the five RDPCSTX "
                    "instances this chip has";
    case 34: return "action is not ENABLE or DISABLE";
    case 35: return "lanenum is not 1, 2 or 4";
    case 36: return "hpdsel outside 1..6 (0 is 'HPD not assigned')";
    case 37: return "the CONTROL PHY (PHY0, the panel) does not read LIT. "
                    "2C1 measured it at 0x00194444; if it is not answering "
                    "that way now, a difference read from this block is "
                    "uninterpretable - this is an INSTRUMENT refusal";
    default: return "unknown";
    }
}

static const char *digVerdictName(long v)
{
    switch (v) {
    case 0:  return "STILL - byte-identical across the send";
    case 1:  return "MOVED - the word changed but the PHY is not lit";
    case 2:  return "LIT - the PHY came up";
    default: return "unknown";
    }
}

/* The same monotonic-counter wait as domVerdict, for the same reason (2C5):
 * the properties publish every 500 ms, so reading the verdict straight after
 * the set can return the previous one and look exactly like a lost write. */
/* 2E5: rb_enc.h's refusal codes, 40-45. Numbered clear of rb_dig's 30-37 so
 * a code read against the wrong table cannot decode plausibly. */
/* 2E6: the five-encoder survey. Shared by `dmubbl dig` and the read-only
 * `dmubbl dig survey`, so the reading is the same text either way. */
static void encSurveyReport(io_service_t fb)
{
    const long encMask = num(fb, "RBFBEncSurveyMask");
    const long done    = num(fb, "RBFBEncSurveyDone");

    /* RULE 15 AT THE TOOL BOUNDARY. `num()` returns -1 for an ABSENT key,
     * which is 0xffffffff in the low word - and before this check the five
     * words printed as ffffffff and every instance decoded as "configured",
     * which is the opposite of the truth on a kext that simply does not
     * publish them. The kext refuses an all-ones word now too; this is the
     * outer of the two checks, and it names the real cause. */
    if (num(fb, "RBFBEncSurveyMask") < 0 || num(fb, "RBFBEncCfg0") < 0) {
        printf("\n  survey of all five link encoders\n"
               "    NOT PUBLISHED by the loaded framebuffer - this reading "
               "needs the\n    2E6 binary. `kextstat | grep rembrandt` "
               "against the UUID the build\n    printed (rule 15); an absent "
               "key is not an absent configuration.\n");
        return;
    }

    /* 2E6's survey: which of the five link encoders look configured. The
     * hypothesis is 0x05 - instances 0 and 2 - which is the pair 2B4's HPD
     * sink mask and 2B9's AUX_EN mask independently name. */
    printf("\n  survey of all five link encoders (read-only, latched before "
           "any send)\n");
    for (int i = 0; i < 5; i++) {
        char kc[32], ks[32];
        snprintf(kc, sizeof(kc), "RBFBEncCfg%d", i);
        snprintf(ks, sizeof(ks), "RBFBEncScram%d", i);
        const long c = num(fb, kc), sc = num(fb, ks);
        printf("    DP%d  CONFIG %08lx  SCRAM %08lx   %s\n", i,
               c & 0xffffffffL, sc & 0xffffffffL,
               (encMask & (1L << i)) ? "configured" : "-");
    }
    printf("    mask %#lx  ", encMask);
    if (encMask == 0x05L) {
        printf("-> 0 and 2 ONLY, which is 2B4's HPD sink mask and 2B9's\n"
               "               AUX_EN mask: the FIRMWARE configured the "
               "Type-C encoder.\n");
    } else if (encMask == 0x1fL) {
        printf("-> all five alike, so 0x1/0x0101FF10 is the block's RESET\n"
               "               state and says nothing about the firmware.\n");
    } else if (encMask == 0L) {
        printf("-> none configured (check RBFBEncSurveyDone - an unready\n"
               "               aperture reads as this).\n");
    } else {
        printf("-> neither 0x05 nor 0x1f; read the words above.\n");
    }
    /* 2E7: DIG_BE_CNTL beside it - the one named missing step, asked with a
     * read. DIG_FE_SOURCE_SELECT is a ONE-HOT mask (0 = INVALID), never an
     * index. */
    if (num(fb, "RBFBDigBeMask") < 0 || num(fb, "RBFBDigBe0") < 0) {
        /* Rule 14: absence says something. Silence here would read as "the
         * block was not surveyed" when the cause is an older binary. */
        printf("\n  DIG_BE_CNTL - NOT PUBLISHED by the loaded framebuffer.\n"
               "    This reading needs the 2E7 binary; check kextstat against "
               "the\n    UUID the build printed (rule 15).\n");
    } else {
        const long bem = num(fb, "RBFBDigBeMask");
        printf("\n  DIG_BE_CNTL - is the DIG front end connected? (2E1's "
               "missing step)\n");
        for (int i = 0; i < 5; i++) {
            char k[32];
            snprintf(k, sizeof(k), "RBFBDigBe%d", i);
            const long w = num(fb, k);
            const unsigned long sel = ((unsigned long)w & 0x7f00UL) >> 8;
            const unsigned long md  = ((unsigned long)w & 0x70000UL) >> 16;
            const char *fe = sel == 0 ? "INVALID (unconnected)"
                           : sel == 1 ? "DIGA" : sel == 2 ? "DIGB"
                           : sel == 4 ? "DIGC" : sel == 8 ? "DIGD"
                           : "not one-hot - NOT a front end";
            const char *mo = md == 0 ? "DP/eDP SST" : md == 1 ? "LVDS"
                           : md == 2 ? "TMDS-DVI" : md == 3 ? "TMDS-HDMI"
                           : md == 5 ? "DP MST" : "not a mode Linux writes";
            const unsigned long hs = ((unsigned long)w & 0x70000000UL) >> 28;
            printf("    DIG%d  %08lx  FE_SEL %#lx %-28s MODE %lu %-11s "
                   "HPD_SEL %lu\n",
                   i, (unsigned long)w & 0xffffffffUL, sel, fe, md, mo, hs);
        }
        printf("    mask %#lx  ", bem);
        if (bem & 0x04L) {
            printf("-> INSTANCE 2 IS CONNECTED. The firmware made the\n"
                   "               FE-to-BE connection as well as configuring "
                   "the encoder\n               (2E7), so 2E1's missing step "
                   "is NOT missing and the only\n               remaining "
                   "candidate is the pixel clock (VBIOS sub-type 2).\n");
        } else {
            printf("-> instance 2 is NOT connected, so 2E1's missing step is\n"
                   "               real and DIG_BE_CNTL.DIG_FE_SOURCE_SELECT "
                   "is a write\n               still to make - one of the two "
                   "remaining candidates.\n");
        }

        /* 2E8: HPD_SELECT is 0-based (dcn10_link_encoder.c:563 writes
         * hpd_source, HPD_SOURCEID1 = 0), so it is 2E1's atom hpdsel MINUS
         * one. 0 is ambiguous between "HPD instance 0" and "never written",
         * so it is reported and not judged. */
        {
            const long w2 = num(fb, "RBFBDigBe2");
            const unsigned long hs2 = ((unsigned long)w2 & 0x70000000UL) >> 28;
            if (w2 >= 0 && hs2 != 0) {
                printf("    HPD_SEL %lu on DIG2 = DCN HPD instance %lu "
                       "(atom hpdsel %lu, 2E1's +1).\n"
                       "               The firmware associated this back end "
                       "with the port 2B4,\n               2B9 and 2E7 "
                       "already name - a FOURTH agreeing instrument.\n",
                       hs2, hs2, hs2 + 1);
            }
        }

        /* DIG_BE_EN_CNTL - the driver never writes it (dcn10_is_dig_enabled()
         * is its only reference in all of docs/linux-ref/), so it is the
         * firmware's own "is this transmitter up" bit and a SECOND product of
         * the DIG1_TRANSMITTER_CONTROL command 2E4 judged on the PHY alone. */
        if (num(fb, "RBFBDigBeEn0") < 0) {
            printf("\n  DIG_BE_EN_CNTL - NOT PUBLISHED by the loaded "
                   "framebuffer.\n    This reading needs the 2E8 binary "
                   "(rule 15).\n");
        } else {
            printf("\n  DIG_BE_EN_CNTL - Linux's own is-this-DIG-enabled "
                   "test\n");
            for (int i = 0; i < 5; i++) {
                char k[32];
                snprintf(k, sizeof(k), "RBFBDigBeEn%d", i);
                const long w = num(fb, k);
                const unsigned long u = (unsigned long)w & 0xffffffffUL;
                printf("    DIG%d  %08lx   DIG_ENABLE %d   SYMCLK_BE_ON %d\n",
                       i, u, (u & 0x1UL) ? 1 : 0, (u & 0x100UL) ? 1 : 0);
            }
            printf("    mask %#lx", num(fb, "RBFBDigBeEnMask"));
            if (num(fb, "RBFBDigBeEnMask") == 0x05L) {
                printf("  -> 0 and 2, the pair every other instrument "
                       "names.\n");
            } else {
                printf("\n");
            }

            /* The before/after. postDone is what separates "sent and nothing
             * moved" from "nothing was sent" - both publish a 0 diff. */
            if (num(fb, "RBFBDigBePostDone") <= 0) {
                printf("    no send on this boot yet, so no before/after "
                       "(RBFBDigSends %ld).\n", num(fb, "RBFBDigSends"));
            } else {
                const long d  = num(fb, "RBFBDigBeDiff");
                const long de = num(fb, "RBFBDigBeEnDiff");
                printf("    after the send: DIG_BE_CNTL diff %#lx, "
                       "EN_CNTL diff %#lx\n", d, de);
                if (d == 0 && de == 0) {
                    printf("               -> the DIG back end did not move "
                           "either, so the\n               command's effect "
                           "is absent in BOTH of its products, not\n"
                           "               just the PHY (2E4 read only the "
                           "PHY).\n");
                } else {
                    printf("               -> THE BACK END MOVED. The "
                           "firmware DOES execute this command\n"
                           "               with a visible register "
                           "product.\n");
                    /* Name what moved. The instances that did NOT move are
                     * the control, so they are printed too - a diff on the
                     * targeted instance alone is the whole attribution. */
                    for (int i = 0; i < 5; i++) {
                        char ka[40], kb[40], kc[40], kd[40];
                        snprintf(ka, sizeof(ka), "RBFBDigBe%d", i);
                        snprintf(kb, sizeof(kb), "RBFBDigBePost%d", i);
                        snprintf(kc, sizeof(kc), "RBFBDigBeEn%d", i);
                        snprintf(kd, sizeof(kd), "RBFBDigBeEnPost%d", i);
                        const unsigned long a = (unsigned long)num(fb, ka) & 0xffffffffUL;
                        const unsigned long b = (unsigned long)num(fb, kb) & 0xffffffffUL;
                        const unsigned long c = (unsigned long)num(fb, kc) & 0xffffffffUL;
                        const unsigned long e = (unsigned long)num(fb, kd) & 0xffffffffUL;
                        const int moved = (a != b) || (c != e);
                        printf("                  DIG%d  CNTL %08lx -> %08lx   "
                               "EN %08lx -> %08lx  %s\n",
                               i, a, b, c, e,
                               moved ? "<== MOVED" : "(control)");
                    }
                    printf("               A diff naming only the instance "
                           "the command targeted is\n               the "
                           "attribution; the other four are the control.\n");
                }
            }
        }
    }

    if (!done) {
        printf("    RBFBEncSurveyDone is NO - the aperture was not ready, so\n"
               "    these words are not a reading. -rbgpufbdig or "
               "-rbgpufbdpalt\n    arms the mapping.\n");
    }
}

static const char *encWhy(long c)
{
    switch (c) {
    case 0:  return "applied, or not attempted";
    case 40: return "the encoder half was OFF for this run (the 2E4 arm)";
    case 41: return "no writable aperture - blRegs or the PHY block";
    case 42: return "instance 0 is the PANEL's link encoder, refused by name";
    case 43: return "instance outside link_enc_regs[0..4]";
    case 44: return "lane count not 1, 2 or 4";
    case 45: return "the dword is on the dcn302/dcn312 danger list";
    default: return "unknown code";
    }
}

static void digVerdictPrint(io_service_t fb, long callsBefore)
{
    long calls = callsBefore;
    int spins;

    for (spins = 0; spins < 60; spins++) {
        calls = num(fb, "RBFBDigCalls");
        if (calls > callsBefore) { break; }
        usleep(100 * 1000);
    }
    if (calls <= callsBefore) {
        printf("\nthe kext never recorded the call (RBFBDigCalls still %ld "
               "after 6 s).\nThat is NOT a refusal - either this framebuffer "
               "predates 2E2 or the\nset never reached the handler. Check the "
               "loaded UUID first (rule 15).\n", callsBefore);
        return;
    }

    const long refuse  = num(fb, "RBFBDigRefuse");
    const long ran     = num(fb, "RBFBDigRan");
    const long sends   = num(fb, "RBFBDigSends");
    const long phy     = num(fb, "RBFBDigPhy");
    const long verdict = num(fb, "RBFBDigVerdict");
    const long before  = num(fb, "RBFBDigBefore");
    const long after   = num(fb, "RBFBDigAfter");
    const long ref     = num(fb, "RBFBDigRefWord");

    printf("\nverdict (read back after the call the kext counted)\n");
    printf("  refusal        %ld  %s\n", refuse, digWhy(refuse));
    printf("  target         PHY%ld   sends %ld   ran %ld\n", phy, sends, ran);
    printf("  control  PHY0  %08lx  %s\n", ref & 0xffffffffL,
           ref == 0 ? "NOT READ" : "(2C1 measured 0x00194444 on this chip)");
    if (ran == 0) {
        printf("  before / after not sampled - the refusal came before the "
               "snapshot\n");
        return;
    }
    printf("  before   PHY%ld  %08lx\n", phy, before & 0xffffffffL);
    printf("  after    PHY%ld  %08lx\n", phy, after & 0xffffffffL);
    printf("  verdict        %ld  %s\n", verdict, digVerdictName(verdict));

    /* 2EC. Printed before the encoder block because it runs before it on
     * the chip: link_hwss_dio.c does the FE connect inside
     * setup_dio_stream_encoder and link_dpms.c:2527 says in its own words
     * that this is "before the link training", which is where
     * enc1_configure_encoder and the transmitter command both live. */
    const long feWant  = num(fb, "RBFBDigFeWant");
    const long feRef   = num(fb, "RBFBDigFeRefuse");
    const long feRan   = num(fb, "RBFBDigFeRan");
    const long feInst  = num(fb, "RBFBDigFeInst");
    const long feAppl  = num(fb, "RBFBDigFeApplied");
    const long feChg   = num(fb, "RBFBDigFeChanged");
    const long feCln   = num(fb, "RBFBDigFeClean");
    const long feB     = num(fb, "RBFBDigFeBefore");
    const long feA     = num(fb, "RBFBDigFeAfter");

    printf("\nfront end -> back end (DIG_BE_CNTL.DIG_FE_SOURCE_SELECT on "
           "DIG%ld)\n", feInst);
    if (feRef == -1 || feWant == -1) {
        printf("  the RBFBDigFe* keys are ABSENT from this framebuffer, so "
               "it predates\n  2EC. That is rule 15, not a reading - check "
               "the loaded UUID.\n");
    } else {
        printf("  asked for      0x%lx  %s\n", feWant, feName(feWant));
        printf("  refusal        %ld  %s\n", feRef, feWhy(feRef));
        printf("  DIG_BE_CNTL    %08lx -> %08lx   FE_SEL %#lx -> %#lx\n",
               feB & 0xffffffffL, feA & 0xffffffffL,
               (feB >> 8) & 0x7fL, (feA >> 8) & 0x7fL);
        /* 2ED. The three derived bits are only COMPUTED on the write path, so
         * on a refusal they are their initialisers and interpreting them is
         * rule 14 at the tool boundary - the A side of 2EC printed
         * "SOMETHING ELSE IN THE WORD CHANGED" over a word that had not been
         * touched, which is an alarming false statement about the address.
         * Guard on `ran`, which is the only key that says a write happened. */
        if (feRan == 0) {
            printf("  applied/changed/clean   NOT COMPUTED - nothing was "
                   "written (ran 0),\n                          so these "
                   "three keys are their initialisers and\n"
                   "                          mean nothing. Read the refusal "
                   "above.\n");
        } else {
            printf("  applied        %ld  %s\n", feAppl,
                   feAppl ? "the front end's bit READS BACK set"
                          : "the readback does NOT carry it");
            printf("  changed        %ld  %s\n", feChg,
                   feChg ? "the word MOVED - evidence of the store itself"
                         : "the word did not move");
            printf("  clean          %ld  %s\n", feCln,
                   feCln ? "every bit outside FE_SEL survived (DIG_MODE, "
                           "HPD_SELECT)"
                         : "SOMETHING ELSE IN THE WORD CHANGED - the RMW or "
                           "the address is wrong");
        }
    }

    /* 2E5. Printed BEFORE the interpretation, because which explanation a
     * STILL verdict leaves alive depends entirely on whether these two
     * writes took - and 2B0 is what deriving a verdict from the wrong
     * summary costs when the raw is not beside it. */
    const long encOn    = num(fb, "RBFBDigEnc");
    const long encRef   = num(fb, "RBFBEncRefuse");
    const long encAppl  = num(fb, "RBFBEncApplied");
    const long encRan   = num(fb, "RBFBEncRan");
    const long cfgB     = num(fb, "RBFBEncCfgBefore");
    const long cfgA     = num(fb, "RBFBEncCfgAfter");
    const long scrB     = num(fb, "RBFBEncScramBefore");
    const long scrA     = num(fb, "RBFBEncScramAfter");
    const long encChg   = num(fb, "RBFBEncChanged");

    printf("\nencoder (enc1_configure_encoder on link encoder %ld)\n", phy);
    printf("  configured     %s   ran %ld\n", encOn ? "yes" : "NO", encRan);
    printf("  refusal        %ld  %s\n", encRef, encWhy(encRef));
    printf("  DP_CONFIG      %08lx -> %08lx   (DP_UDI_LANES, want 1 for "
           "2 lanes)\n", cfgB & 0xffffffffL, cfgA & 0xffffffffL);
    printf("  SCRAM_CNTL     %08lx -> %08lx   (bit 4 ADVANCE set, the other "
           "three fields kept)\n", scrB & 0xffffffffL, scrA & 0xffffffffL);
    /* 2ED, same guard as the FE block above: both derived bits are computed
     * on the write path only, so on a refusal they are initialisers. */
    if (encRan == 0) {
        printf("  applied/changed NOT COMPUTED - the configure did not run, "
               "so both keys\n                  are initialisers. Read the "
               "refusal above.\n");
    } else {
        printf("  applied        %ld  %s\n", encAppl,
               encAppl ? "the READBACK agrees with the target"
                       : "the readback does NOT agree");
        printf("  changed        %ld  %s\n", encChg,
               encChg ? "our write MOVED a register, so the write path works"
                      : "NEITHER register moved - the encoder was ALREADY in "
                        "the target\n                     state, so `applied` "
                        "says nothing about our write (2E6)");
    }

    encSurveyReport(fb);

    if (verdict == 2) {
        printf("\n  -> THE FIRMWARE EXECUTES DMUB_CMD__VBIOS COMMANDS, and "
               "the external\n     display's transmitter is UP. 2DB spent "
               "four instruments on the\n     first half of that and 2DC-2DF "
               "never got to send anything. Next:\n     "
               "DIG_BE_CNTL.DIG_FE_SOURCE_SELECT, then link training.\n");
    } else if (verdict == 1) {
        printf("\n  -> the firmware ACTED - that word is not ours to write "
               "and it moved.\n     So VBIOS commands execute. The PHY is "
               "not lit by the conjunction\n     rb_dig.h uses, which points "
               "at the encoder setup this experiment\n     deliberately "
               "omits (DP_CONFIG / DP_DPHY_SCRAM_CNTL). Read the word.\n");
    } else {
        if (encAppl && !encChg) {
            printf("\n  -> sent, and THE ENCODER WAS ALREADY IN THE TARGET "
                   "STATE - neither write\n     moved a bit. So 2E2's live "
                   "alternative was never a DIFFERENCE: the\n     "
                   "encoder was configured on boot 443 too, and 2E4's STILL "
                   "already\n     carried it. The caveat is discharged by "
                   "measurement rather than by\n     change - and our write "
                   "path is UNVERIFIED by this reading (2E6).\n\n     Two "
                   "steps named in advance are still not reproduced and are "
                   "now\n     the whole list: DIG_BE_CNTL.DIG_FE_SOURCE_SELECT"
                   " (which\n     dig_encoder_sel_to_atom returns 0 for, and "
                   "2E1 recorded as a MISSING\n     STEP, not a free field) "
                   "and the pixel clock (VBIOS sub-type 2).\n     A "
                   "transmitter with no clock and no front end may have "
                   "nothing to\n     light, so \"the firmware discards VBIOS "
                   "commands\" is NOT yet the\n     reading - it is one of "
                   "two live candidates, and the cheaper to refute.\n");
        } else if (encAppl) {
            printf("\n  -> sent, THE ENCODER IS CONFIGURED AND VERIFIED BY "
                   "READBACK, and the PHY\n     still did not move. 2E2's "
                   "live alternative is now ELIMINATED, so the\n     "
                   "remaining reading is that this firmware accepts a "
                   "DMUB_CMD__VBIOS\n     command, advances RPTR over it "
                   "(2E4) and DISCARDS it.\n\n     Before concluding that, "
                   "two things are NOT reproduced and are named\n     here "
                   "rather than in a journal: DIG_BE_CNTL.DIG_FE_SOURCE_SELECT "
                   "(the\n     DIG FE-to-BE connection, which "
                   "dig_encoder_sel_to_atom returns 0 for\n     and which "
                   "2E1 recorded as a MISSING STEP, not a free field), and "
                   "the\n     pixel clock (VBIOS sub-type 2). A transmitter "
                   "enabled with no clock\n     and no front-end attached "
                   "may have nothing to light.\n");
        } else {
            printf("\n  -> sent and NOTHING moved, AND the encoder configure "
                   "did not take -\n     so this run cannot separate the two. "
                   "Read the refusal above; a\n     code 40 means the "
                   "encoder half was off, which is 2E4's arm and\n     "
                   "reproduces 2E4 rather than advancing on it.\n");
        }
    }

    /* 2EC: what the FE outcome does to the list above. Said here rather than
     * folded into the three paragraphs, because those record what was named
     * in advance and that record should not move. */
    if (feRan > 0 && feRef == 0 && feAppl && feChg && feCln) {
        printf("\n  -> AND THE FRONT END IS NOW CONNECTED (FE_SEL %#lx -> "
               "%#lx on DIG%ld,\n     the word moved and nothing else in it "
               "did). So of the two steps 2E6\n     named as not reproduced, "
               "ONE IS LEFT: the pixel clock, VBIOS\n     sub-type 2 - "
               "another inbox command of the kind 2E9 proved executes\n     "
               "and 2EB proved answers. A transmitter with a front end and "
               "no clock\n     still has nothing to light, so a STILL "
               "verdict here is STILL not\n     \"the firmware discards "
               "VBIOS commands\".\n",
               (feB >> 8) & 0x7fL, (feA >> 8) & 0x7fL, feInst);
    } else if (feRan > 0 && feRef == 0 && !feCln) {
        printf("\n  -> THE WRITE WAS NOT CLEAN: a bit outside FE_SEL moved. "
               "Treat the whole\n     reading as suspect and the address as "
               "the first suspect - do NOT\n     interpret the PHY verdict "
               "off this run.\n");
    } else if (feRan > 0 && feRef == 0 && !feChg) {
        printf("\n  -> the FE write landed on a word that was ALREADY "
               "connected, so\n     `applied` carries no evidence of our "
               "store (2E6's shape). That\n     contradicts 2E8's FE_SEL 0 "
               "on DIG2 and is worth reading twice.\n");
    }
}

/* ===================== 2F1: the DP pixel-clock DTO write ================= */

static const char *dtoWhy(long c)
{
    switch (c) {
    case 0:  return "OK - the three writes ran";
    case 40: return "not armed (-rbgpufbdto absent)";
    case 41: return "no register mapping";
    case 42: return "THIS BOOT did not confirm BASE_IDX 1 - read RBFBDtoBase";
    case 43: return "pipe 0 is the PANEL, refused by name";
    case 44: return "pipe out of range (0..3)";
    case 45: return "that pipe's DP DTO is already enabled";
    case 46: return "pipe 0's MODULO is not usable as a reference";
    case 47: return "the asked clock is outside 25 MHz .. the live reference";
    default: return "unknown";
    }
}

static const char *dtoVerdictName(long v)
{
    switch (v) {
    case -1: return "NOT RUN";
    case 0:  return "STILL - the hardware's status bit did not follow";
    case 1:  return "STARTED - status went 0 -> 1: the DTO makes a clock";
    case 2:  return "WAS ON - already set, so no transition to attribute";
    default: return "unknown";
    }
}

static void dtoVerdictPrint(io_service_t fb, long callsBefore)
{
    long calls = callsBefore;
    int spins;

    /* Rule 31, and the fifth recorded instance of getting this wrong: poll a
     * MONOTONIC counter past its pre-set value rather than sleeping and
     * comparing two reads of the dialled value, which cannot tell "not
     * published yet" from "the write was lost". */
    for (spins = 0; spins < 60; spins++) {
        calls = num(fb, "RBFBDtoProgCalls");
        if (calls > callsBefore) { break; }
        usleep(100 * 1000);
    }
    if (calls <= callsBefore) {
        printf("\nRBFBDtoProgCalls never moved past %ld in 6 s.\n"
               "That is NOT a refusal. Either this framebuffer predates 2F1 "
               "(the key\nwould then be ABSENT and read as -1), or the set "
               "never reached the\nhandler. Check the loaded UUID inside the "
               "collection first (rule 15):\n  kmutil inspect -B "
               "/Library/KernelCollections/AuxiliaryKernelExtensions.kc \\\n"
               "    --show-kext-uuids | grep -i rembrandt\n", callsBefore);
        return;
    }

    const long refuse  = num(fb, "RBFBDtoProgRefuse");
    const long ran     = num(fb, "RBFBDtoProgRan");
    const long inst    = num(fb, "RBFBDtoProgInst");
    const long pix     = num(fb, "RBFBDtoProgPixHz");
    const long ref     = num(fb, "RBFBDtoProgRefHz");
    const long srcsel  = num(fb, "RBFBDtoProgSrcSel");
    const long applied = num(fb, "RBFBDtoProgApplied");
    const long changed = num(fb, "RBFBDtoProgChanged");
    const long clean   = num(fb, "RBFBDtoProgClean");
    const long before  = num(fb, "RBFBDtoRateBefore");
    const long after   = num(fb, "RBFBDtoRateAfter");
    const long phase   = num(fb, "RBFBDtoPhaseAfter");
    const long modulo  = num(fb, "RBFBDtoModuloAfter");
    const long verdict = num(fb, "RBFBDtoProgVerdict");
    const long base    = num(fb, "RBFBDtoBase");

    printf("\nverdict (read back after the call the kext counted)\n");
    printf("  base verdict   %ld  %s\n", base,
           base == 0 ? "CONFIRMED this boot" : "NOT confirmed this boot");
    printf("  refusal        %ld  %s\n", refuse, dtoWhy(refuse));
    printf("  pipe           %ld   ran %ld   srcsel %ld\n", inst, ran, srcsel);
    printf("  asked clock    %ld Hz  (%.6f MHz)\n", pix, (double)pix / 1e6);
    printf("  reference      %ld Hz  (%.6f MHz) - COPIED from pipe 0\n",
           ref, (double)ref / 1e6);
    printf("  PIXEL_RATE_CNTL 0x%08lx -> 0x%08lx\n", before, after);
    printf("  applied %ld  changed %ld  clean %ld\n", applied, changed, clean);
    printf("  PHASE/MODULO readback  %ld / %ld\n", phase, modulo);
    printf("                 ^ raw only. 2F0 measured PHASE as the clock the\n"
           "                   firmware ASKED for, not the achieved one, so\n"
           "                   our own number coming back here says nothing\n"
           "                   (rule 20) and is known in advance to agree.\n");
    printf("  VERDICT        %ld  %s\n", verdict, dtoVerdictName(verdict));

    if (refuse == 0 && verdict == 1) {
        printf("\n  -> DPDTO_ENABLE_STATUS went 0 -> 1 on a pipe that was "
               "dark.\n     That bit is the hardware's answer and not one we "
               "wrote, and 2F0\n     attested both its states on this chip "
               "beforehand. The pixel\n     clock for the external display's "
               "pipe exists.\n");
        printf("     NOT established by this: a picture. There is no OTG "
               "timing on\n     this pipe, no trained link, no stream mapped "
               "to the encoder and\n     no surface - all four are later "
               "steps, and a dark monitor is\n     the EXPECTED reading.\n");
    } else if (refuse == 0 && verdict == 0) {
        printf("\n  -> the three writes ran, the enable read back, and the "
               "hardware's\n     own status bit did NOT follow. That is a "
               "real negative about this\n     block and not a failed write - "
               "`changed 1` says the word moved.\n     Candidates, none "
               "measured: the DTO needs something else enabled\n     "
               "(DISPCLK to the pipe, OTG_PIXEL_RATE_SOURCE), or SRC_SEL is "
               "the\n     wrong way round - try the other side with `srcsel "
               "0` in THIS boot,\n     interleaved (rule 44).\n");
    } else if (refuse == 42) {
        printf("\n  -> nothing was written, and that is the gate working. The "
               "write is\n     at BASE_IDX 1, whose offsets collide with the "
               "DOMAIN power gating\n     of the pipe scanning the panel, so "
               "it is refused unless THIS boot's\n     own sample reproduced "
               "the panel's pixel clock at that base.\n     Read RBFBDtoBase, "
               "RBFBDtoExpectedHz and RBFBDto0Phase.\n");
    }
}

static void domVerdict(io_service_t fb, long callsBefore)
{
    uint32_t before[DOM_SLOTS] = {0}, after[DOM_SLOTS] = {0};
    uint32_t st[DOM_SLOTS] = {0}, cf[DOM_SLOTS] = {0};
    long calls = callsBefore;
    int spins;
    int i;

    for (spins = 0; spins < 60; spins++) {
        calls = num(fb, "RBFBDmubDomCalls");
        if (calls > callsBefore) { break; }
        usleep(100 * 1000);
    }
    if (calls <= callsBefore) {
        printf("\nthe kext never recorded the call (RBFBDmubDomCalls still "
               "%ld after 6 s).\n"
               "That is NOT a refusal - either this framebuffer predates 2DD "
               "or the\nset never reached the handler. Check the loaded UUID "
               "first (rule 15).\n", callsBefore);
        return;
    }

    const long refuse = num(fb, "RBFBDmubDomRefuse");
    const long moved  = num(fb, "RBFBDmubDomMoved");
    const long ran    = num(fb, "RBFBDmubDomRan");
    const long inst   = num(fb, "RBFBDmubDomInst");
    const long cand = num(fb, "RBFBPgCandidate");
    words(fb, "RBFBPgBefore", before, DOM_SLOTS);
    words(fb, "RBFBPgAfter",  after,  DOM_SLOTS);
    words(fb, "RBFBPgStatus", st,     DOM_SLOTS);
    words(fb, "RBFBPgConfig", cf,     DOM_SLOTS);

    printf("\nverdict (read back after the call the kext counted)\n");
    printf("  refusal        %ld  %s\n", refuse, domWhy(refuse));
    printf("  domain         %ld   ran %ld   moved 0x%lx\n", inst, ran, moved);
    printf("  candidate      ");
    if (cand < 0) {
        printf("none - every domain is in DOMAIN0's state\n");
    } else {
        printf("DOMAIN%ld\n", cand);
    }
    printf("  %-14s", "domain");
    for (i = 0; i < DOM_SLOTS; i++) { printf(" %8u", domNums[i]); }
    printf("   <- DOMAIN NUMBERS, not slots\n");
    domRow("live STATUS", st, 1);
    domRow("live CONFIG", cf, 1);
    /* ran 0 means the experiment refused before sampling, and four zero
     * words there would read exactly like a chip answer (2DD's lesson about
     * this very field). */
    domRow("before STATUS", before, ran > 0);
    domRow("after  STATUS", after,  ran > 0);

    if (refuse == 0 && moved != 0) {
        printf("  -> THE FIRMWARE EXECUTED AN INBOX COMMAND. A DOMAIN_PG_STATUS\n"
               "     word changed across one DMUB_CMD__VBIOS send, which no "
               "other\n     instrument in this tool can show (2DB spent all "
               "four). The whole\n     VBIOS bring-up path for the external "
               "display is available.\n");
    } else if (refuse == 0) {
        printf("  -> sent, acked, and NO domain moved. On its own that reads "
               "as\n     'this firmware accepts and discards VBIOS commands' "
               "- but the\n     before/after words above are the evidence for "
               "that, not this line.\n");
    } else {
        printf("  -> REFUSED, so nothing was sent and this is not a reading "
               "about\n     the firmware at all.\n");
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

/* 2DA's trap, closed inside the invocation rather than documented - and
 * SHARED, because 2EA needs the identical precondition. Returns 0 when the
 * push is pointed at REGION4 and non-zero when it refuses; a caller that
 * ignores it would be the exact mistake this exists to prevent. */
/* ---- 2EA: the QUERY_DP_ALT sweep ----
 *
 * The verdict is computed from the RAW per-phy keys, never from the kext's
 * own summary: 2B0 is what a summary that answers a different question than
 * the consumer asks costs, and this tool is the consumer. */
static const char *dpaltqWhy(long c)
{
    switch (c) {
    case 0:  return "OK";
    case 50: return "the experiment is not armed (-rbgpufbdpaltq)";
    case 51: return "phy out of range";
    case 52: return "the inbox push itself refused";
    case 53: return "NO REPLY - the OUT bytes came back as we wrote them";
    case 54: return "a reply byte that is neither 0 nor 1";
    case 55: return "the reply names a DIFFERENT phy";
    default: return "?";
    }
}

static void dpaltqReport(io_service_t fb, long callsBefore)
{
    long calls = -1;
    int i;

    if (num(fb, "RBFBDpAltQCalls") < 0) {
        printf("\nRBFBDpAltQCalls is ABSENT. This framebuffer predates 2EA -\n"
               "rule 15: check the loaded UUID against the one the build\n"
               "printed. Nothing is reported rather than decoding keys that\n"
               "are not there (2E6's tool read -1 as all-ones and said the\n"
               "opposite of the truth).\n");
        return;
    }
    for (int t = 0; t < 60; t++) {
        if (num(fb, "RBFBDpAltQCalls") > callsBefore) {
            calls = num(fb, "RBFBDpAltQCalls");
            break;
        }
        usleep(100000);
    }
    if (calls < 0) {
        printf("\nRBFBDpAltQCalls never moved past %ld in 6 s - the call did\n"
               "not arrive. Polled, not slept on: a snapshot read would be\n"
               "rule 31 for the fourth time.\n", callsBefore);
        return;
    }

    printf("\nQUERY_DP_ALT sweep (fill 0x%02lx, %ld sends)\n",
           num(fb, "RBFBDpAltQFill"), num(fb, "RBFBDpAltQSends"));
    printf("  phy  is_usb  alt_disable  is_dp4  echo  refusal\n");

    unsigned usb = 0, answered = 0;
    for (i = 0; i < 5; i++) {
        char k[40];
        snprintf(k, sizeof(k), "RBFBDpAltQRefuse%d", i);
        const long why = num(fb, k);
        snprintf(k, sizeof(k), "RBFBDpAltQUsb%d", i);
        const long u = num(fb, k);
        snprintf(k, sizeof(k), "RBFBDpAltQDis%d", i);
        const long dd = num(fb, k);
        snprintf(k, sizeof(k), "RBFBDpAltQDp4%d", i);
        const long f = num(fb, k);
        snprintf(k, sizeof(k), "RBFBDpAltQEcho%d", i);
        const long e = num(fb, k);

        if (why == 0) {
            answered |= (1u << i);
            if (u) { usb |= (1u << i); }
            printf("   %d     %ld         %ld          %ld      %ld   %s%s\n",
                   i, u, dd, f, e, dpaltqWhy(why),
                   i == 2 ? "   <== the Type-C port (2E1)" : "");
        } else {
            printf("   %d     -         -          -      -   %ld: %s\n",
                   i, why, dpaltqWhy(why));
        }
    }

    printf("\nis_usb mask 0x%02x over the %u phys that answered; the ROM says "
           "0x1C\n(DP_IS_USB_C on display paths 2/3/4 - 2E1, an independent "
           "source).\n", usb, __builtin_popcount(answered));

    if (answered == 0) {
        printf("\nNOTHING ANSWERED. That is 2zr's reading reproduced at the\n"
               "RIGHT address this time, which makes it a real result rather\n"
               "than 2C0's inference: REGION4 executes commands (2E9) and\n"
               "still returns no data. Then this route is closed and the\n"
               "DPALT_DISABLE question stays with RDPCSTX2_PHY_CNTL6.\n");
        return;
    }
    if (usb == 0 || usb == answered) {
        printf("\nUNIFORM is_usb across the phys that answered - 2BF's own\n"
               "control, and it refuses: the panel's PHY is known NOT to be\n"
               "USB-C, so a uniform answer carries no information about\n"
               "whether the reply is the firmware's or our own bytes.\n");
        return;
    }
    if (answered == 0x1f && usb == 0x1C) {
        printf("\nROM MATCH. The reply pattern equals 2E1's VBIOS read on all\n"
               "five paths, from a source this command did not derive from -\n"
               "so the reply path WORKS and the answers below are the chip's.\n");
    } else {
        printf("\nAnswered and non-uniform, but NOT the ROM's pattern. The\n"
               "reply path produced something; what it produced is not\n"
               "verified against an independent source, so read the bytes\n"
               "and not the verdict.\n");
    }

    {
        char k[40];
        snprintf(k, sizeof(k), "RBFBDpAltQDis%d", 2);
        const long dis = num(fb, k);
        snprintf(k, sizeof(k), "RBFBDpAltQUsb%d", 2);
        const long u2 = num(fb, k);
        snprintf(k, sizeof(k), "RBFBDpAltQRefuse%d", 2);
        if (num(fb, k) != 0 || !u2) {
            printf("\nPHY2 did not answer as a USB-C phy, so the question\n"
                   "this sweep exists for is unanswered.\n");
        } else if (dis) {
            printf("\n==> DP ALT MODE IS NOT GRANTED on PHY2. That AGREES with\n"
                   "RDPCSTX2_PHY_CNTL6's 0x00063333 (2C1), so that register was\n"
                   "right all along and the external display is a PD / firmware\n"
                   "matter on this chassis - rule 47 for the third time (2zw's\n"
                   "MUX, 2D8's dead Type-C, this). No write of ours can light a\n"
                   "PHY the PD controller has not handed DP lanes to.\n"
                   "NOTE the tension this does NOT resolve: 2BB read a full DPCD\n"
                   "and 2BD a full EDID over AUX2, which on Type-C needs alt\n"
                   "mode negotiated. Two instruments still disagree.\n");
        } else {
            printf("\n==> DP ALT MODE IS GRANTED on PHY2. Then\n"
                   "RDPCSTX2_PHY_CNTL6 is the WRONG REGISTER for this part -\n"
                   "Linux reads RDPCSPIPE_PHY_CNTL6 for UNIPHY_C on\n"
                   "YELLOW_CARP_B0 - and the bring-up continues: the FE\n"
                   "connection (DIG_FE_SOURCE_SELECT, ours to write per 2E8)\n"
                   "and the pixel clock (VBIOS sub-type 2).\n");
        }
    }
}

static int ensureRegion4(io_service_t fb)
{
    /* 2DA's trap, closed inside the invocation rather than documented.
     *
     * The push goes to whatever window RBFBDmubSel last named, the
     * default is CW4, and this project's own rb_dmub.h rules CW4 out -
     * the firmware wrote its INBOX1 base as 0x80000000, which is inside
     * REGION4's top of 0x8000ffff and outside region 3 entirely. 2DA
     * lost one session to that default and a SECOND to a reboot resetting
     * the selector after it had been set by hand. So this does not
     * document the prerequisite, it performs it: select REGION4, read the
     * selector back, and refuse to send if it is not 7. An experiment
     * whose precondition is a previous command is one that gets run
     * without it (rule 34, made structural - the same reason the kext
     * side is a single call). */
    {
        const long selBefore = num(fb, "RBFBDmubWinSel");
        const long setsBefore = num(fb, "RBFBDmubSelSets");
        if (selBefore != 7) {
            printf("\nselector is %ld; REGION4 is the identified mailbox "
                   "(2DA), selecting 7\nfirst so the command does not go "
                   "into CW4 - which rb_dmub.h itself\nrules out.\n",
                   selBefore);
            kern_return_t ksel = IORegistryEntrySetCFProperties(
                fb, (__bridge CFDictionaryRef)@{ @"RBFBDmubSel": @(7) });
            if (ksel != KERN_SUCCESS) {
                printf("selecting REGION4 failed (0x%x) - refusing to "
                       "send into an unknown window.\n", ksel);
                IOObjectRelease(fb);
                return 2;
            }
            /* Read it BACK rather than trusting the return code: a
             * kern_return_t says the setter ran, not that the state took
             * (rule 13, and 2A3 is what trusting it costs).
             *
             * 2E3: but POLL, and poll the monotonic counter first. Every
             * DMUB key is published on a 500 ms throttle, so the first
             * version of this read the stale 4 back immediately after a
             * successful set and refused its own send - rule 31, third
             * instance. RBFBDmubSelSets counts accepted selects and only
             * ever rises, so waiting for it past its pre-set value
             * separates "not published yet" from "the write was lost",
             * which the dialled value alone cannot. */
            long selNow = -1;
            for (int i = 0; i < 60; i++) {
                if (num(fb, "RBFBDmubSelSets") > setsBefore) {
                    selNow = num(fb, "RBFBDmubWinSel");
                    break;
                }
                usleep(100000);
            }
            if (selNow < 0) {
                printf("RBFBDmubSelSets never moved past %ld in 6 s - the "
                       "select did not\narrive (or this framebuffer "
                       "predates 2E3). Refusing to send.\n", setsBefore);
                IOObjectRelease(fb);
                return 2;
            }
            if (selNow != 7) {
                printf("selector reads %ld after asking for 7 - refusing "
                       "to send.\n", selNow);
                IOObjectRelease(fb);
                return 2;
            }
            printf("selector now 7 (REGION4).\n");
        }
    }
    return 0;
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
               "  dmubbl dpaltq [zero]  QUERY_DP_ALT over phy 0..4 - the\n"
               "                      chip's own answer to whether DP alt\n"
               "                      mode is granted on the Type-C PHY,\n"
               "                      which every bring-up write depends on.\n"
               "                      READ-ONLY on the chip. Needs\n"
               "                      -rbgpufbdpaltq. `zero` is the\n"
               "                      Linux-exact fill control.\n"
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
        /* A DOMAIN NUMBER, not a slot: 0..3 are the pipes and 16..18 the
         * other blocks, which is what the command's `inst` field means
         * (dmub_cmd.h:3150). 4..15 do not exist and the kext refuses them. */
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
    } else if (strcmp(argv[1], "dpaltq") == 0) {
        /* 2EA: five QUERY_DP_ALT commands in ONE setProperties call - the
         * kext sweeps phy 0..4 and decodes each reply out of the slot
         * dmubPush already reads back. Read-only on the chip: every field
         * but phy_id is an output.
         *
         * The ARGUMENT is the fill byte, not a phy: `zero` sends Linux's
         * byte-exact memset(0) and anything else (the default) sends the
         * 0xFF sentinel. The sentinel is the instrument - under a zero fill
         * an unanswered slot and a genuine all-zero reply are the same four
         * bytes - and the zero run is the control that keeps the deviation
         * testable rather than merely argued. */
        if (ensureRegion4(fb) != 0) { IOObjectRelease(fb); return 2; }

        long fill = 1;              /* non-zero = the 0xFF sentinel */
        if (argc > 2 && strcmp(argv[2], "zero") == 0) { fill = 0; }
        else if (argc > 2) {
            printf("usage: dmubbl dpaltq [zero]\n"
                   "       default = the 0xFF sentinel fill (the instrument)\n"
                   "       zero     = Linux's byte-exact memset(0) control\n");
            IOObjectRelease(fb);
            return 2;
        }

        d = @{ @"RBFBDpAltQ": @(fill) };
        printf("\nsweeping QUERY_DP_ALT (sub-type 26) over phy 0..4, fill %s...\n",
               fill ? "0xFF (sentinel)" : "0x00 (Linux-exact control)");
        printf("read-only: phy_id is the only input and the command changes no\n"
               "hardware state. 2C0 refused to build this because 2zr measured\n"
               "no inbox reply - but 2zr's reading was taken at CW4, 2DA found\n"
               "the ring is REGION4 and 2E9 proved a command sent there\n"
               "EXECUTES. So the reply path has never been tried at the right\n"
               "address. What it decides: whether DP alt mode is granted on\n"
               "PHY2 at all, which every downstream write depends on.\n");
    } else if (strcmp(argv[1], "dig") == 0) {
        /* 2E2: the DIG1 transmitter ENABLE for the Type-C port. ONE
         * setProperties call - the kext reads the control PHY, reads the
         * target PHY, sends, and reads the target again - for the reason the
         * `dom` path records: a two-step handover is one that gets run with
         * the first step quietly reverted.
         *
         * The argument is a phyid, and omitting it means PHY2, which 2E1
         * derived from this board's own VBIOS display-object table as the
         * Type-C port (UNIPHY_C) and which 2B4's HPD instance 2, 2B9's
         * AUX_HPD_SEL 2 and 2C1's dark 0x00063333 all corroborate. There is
         * no reason to pass one by hand. */
        long phy = (argc > 2) ? atol(argv[2]) : -1;
        /* 2E5: the A side, reachable in the SAME boot. 2B7 is why - an A/B
         * across boots cannot tell a dial from a drift, and 2B6 died on
         * exactly that. `noenc` reproduces 2E4's arm byte for byte. */
        int wantEnc = 1;
        /* 2EC: the FE connection, OFF unless asked. 0 is the documented
         * INVALID and is what the kext refuses, so this is both the dial and
         * the A side; -1 asks for the derived front end (DIGC) so the
         * ordinary invocation names no magic number. An explicit one-hot is
         * accepted for a deliberate A/B against another front end. */
        long wantFe = 0;
        for (int a = 2; a < argc; a++) {
            if (strcmp(argv[a], "noenc") == 0) { wantEnc = 0; }
            else if (strcmp(argv[a], "fe") == 0) { wantFe = -1; }
            else if (strncmp(argv[a], "fe=", 3) == 0) {
                wantFe = strtol(argv[a] + 3, NULL, 0);
            }
        }
        if (argc > 2 && (strcmp(argv[2], "noenc") == 0
                         || strcmp(argv[2], "fe") == 0
                         || strncmp(argv[2], "fe=", 3) == 0)) { phy = -1; }
        /* 2E6: report the five-encoder survey and SEND NOTHING. The survey
         * is the independent finding (did the firmware configure the Type-C
         * encoder, or is 0x1/0x0101FF10 a reset default?) and it should not
         * need the experiment armed to be read. It is latched pristine at the
         * first periodic publish, so a send would not spoil it either - this
         * just makes the reading free. */
        if (argc > 2 && strcmp(argv[2], "survey") == 0) {
            encSurveyReport(fb);
            IOObjectRelease(fb);
            return 0;
        }

        if (phy == 0) {
            printf("\nREFUSING to pass phyid 0 from this tool.\n"
                   "PHY0 is the eDP PANEL this desktop is running on - 2E1's\n"
                   "display path 0 on UNIPHY_A, and the only lit PHY 2C1\n"
                   "measured. The kext refuses it by name as well; this is\n"
                   "the outer of the two checks.\n");
            IOObjectRelease(fb);
            return 2;
        }
        if (phy > 5) {
            printf("usage: dmubbl dig [phyid] [fe|fe=MASK] [noenc]\n"
                   "       phyid omitted = PHY2, the Type-C port (2E1)\n"
                   "       fe  = also connect front end DIGC (2EC)\n"
                   "       the payload documents 0=UNIPHYA .. 5=UNIPHYF\n");
            IOObjectRelease(fb);
            return 2;
        }

        if (ensureRegion4(fb) != 0) { IOObjectRelease(fb); return 2; }

        /* Both keys in ONE setProperties call: VBL.cpp handles RBFBDigEnc
         * first, so the dial is in place before the send it governs - a
         * two-call handover is what rule 34 and 2DA warn about, and here it
         * is avoidable entirely. */
        /* All three keys in ONE setProperties call: VBL.cpp handles
         * RBFBDigFe and RBFBDigEnc before RBFBDigPhy, so both dials are in
         * place before the send they govern (rule 34). */
        d = @{ @"RBFBDigFe": @((int32_t)wantFe),
               @"RBFBDigEnc": @(wantEnc),
               @"RBFBDigPhy": @((int32_t)phy) };
        printf("\nencoder half: %s\n", wantEnc
               ? "ON - enc1_configure_encoder's two writes run first (2E5)"
               : "OFF - reproducing 2E4's arm exactly (the A side)");
        if (wantFe == 0) {
            printf("FE connect:   OFF - no front end named, so nothing is "
                   "written to\n              DIG_BE_CNTL. This is 2E6's arm "
                   "and the A side; pass `fe`\n              to connect "
                   "DIGC, which 2E8 measured ABSENT on DIG2.\n");
        } else if (wantFe < 0) {
            printf("FE connect:   ON - DIGC (0x4) into the target back end's "
                   "FE_SEL,\n              an OR inside an RMW, with "
                   "DIG_MODE and HPD_SELECT kept.\n              2E8 read "
                   "FE_SEL 0 there, so a real write MUST move the word.\n");
            printf("              NOTE: there is no disconnect path, so the "
                   "A side (`dig 2`\n              with no `fe`) must be run "
                   "FIRST in a boot. Once the FE is\n              "
                   "connected it stays connected until reboot.\n");
        } else {
            printf("FE connect:   ON - front end 0x%lx (%s)\n",
                   wantFe, feName(wantFe));
        }
        if (phy < 0) {
            printf("\nsending DIG1_TRANSMITTER_CONTROL ENABLE to PHY2, the\n"
                   "Type-C port (TRANSMITTER_UNIPHY_C)...\n");
        } else {
            printf("\nsending DIG1_TRANSMITTER_CONTROL ENABLE to PHY%ld...\n",
                   phy);
        }
        printf("payload v1_7, which this board's ROM decides and not a guess:\n"
               "  its atom command table has NO dig1transmittercontrol entry,\n"
               "  so crev is 0 and transmitter_control_fallback is hardcoded\n"
               "  to v1_7 over DMUB (2E2). phyid 2, action 1, digmode 0,\n"
               "  lanenum 2, hpdsel 3 (NOT 2 - the payload comment is 1-based),\n"
               "  symclk_10khz 54000.\n"
               "The verdict is RBFBDigBefore vs RBFBDigAfter - PHY2's own\n"
               "RDPCSTX_PHY_CNTL6 either side of the send, with PHY0 as the\n"
               "measured control. This is both the first bring-up step for the\n"
               "external display AND the test 2DB-2DF never got to run.\n");
    } else if (strcmp(argv[1], "dto") == 0) {
        /* 2F1: the three writes of `dcn31_program_pix_clk`'s DP branch on one
         * dark pipe. A REGISTER write, not a DMUB command - it lives in this
         * tool because the dial plumbing and the refusal decoding are here,
         * and it needs REGION4 for nothing.
         *
         * The clock is an argument in kHz and DEFAULTS TO 2BD's preferred
         * timing, with its provenance printed: 148.5 MHz is the first
         * detailed timing descriptor of the EDID this project read off this
         * sink over I2C-over-AUX on boot 295, which is the mode `dc` would
         * pick and the one the monitor is guaranteed to accept. Its 100 Hz
         * mode (228000) is the other measured option and both fit the
         * ~8.64 Gbps link 2BB measured. The handoff named 228000; the
         * preferred timing is chosen instead, deliberately and for that
         * reason - nothing downstream of the clock exists yet, so the value
         * cannot be observably wrong either way, and the one with fewer
         * assumptions is the preferred one. */
        long khz = 148500;
        long pipe = -1;
        long srcsel = -1;
        for (int a = 2; a < argc; a++) {
            if (strncmp(argv[a], "pipe=", 5) == 0) {
                pipe = strtol(argv[a] + 5, NULL, 0);
            } else if (strncmp(argv[a], "srcsel=", 7) == 0) {
                srcsel = strtol(argv[a] + 7, NULL, 0);
            } else if (strcmp(argv[a], "help") == 0) {
                printf("usage: dmubbl dto [kHz] [pipe=N] [srcsel=0|1]\n"
                       "       kHz omitted = 148500, 2BD's preferred timing\n"
                       "       (its 100 Hz mode is 228000)\n"
                       "       pipe omitted = 1, the first pipe that is NOT "
                       "the panel's\n"
                       "       srcsel omitted = 1, which is Linux's value "
                       "(the firmware\n"
                       "       left pipe 0's at 0 - the A side, interleavable "
                       "in one boot)\n");
                IOObjectRelease(fb);
                return 2;
            } else {
                khz = strtol(argv[a], NULL, 0);
            }
        }

        if (pipe == 0) {
            printf("\nREFUSING to pass pipe 0 from this tool.\n"
                   "Pipe 0 is the eDP PANEL this desktop is running on - "
                   "OTG0_CONTROL\n0x00011201 with OTG1-3 at 0x300 (2oq), and "
                   "2F0 measured its DTO\ndriving it. The kext refuses it by "
                   "name as well; this is the outer\nof the two checks.\n");
            IOObjectRelease(fb);
            return 2;
        }
        if (pipe > 3) {
            printf("usage: dmubbl dto [kHz] [pipe=1..3] [srcsel=0|1]  "
                   "(`dto help`)\n");
            IOObjectRelease(fb);
            return 2;
        }

        /* All three keys in ONE setProperties call: VBL.cpp reads the pipe and
         * SRC_SEL before the clock that triggers the write, so both dials are
         * in place before the write they govern. A two-call handover is what
         * rule 34 and 2DA warn about and it is avoidable here. */
        NSMutableDictionary *m = [NSMutableDictionary dictionary];
        if (pipe >= 0) { m[@"RBFBDtoPipe1"] = @((uint32_t)pipe); }
        if (srcsel >= 0) { m[@"RBFBDtoSrcSel"] = @((uint32_t)(srcsel ? 1 : 0)); }
        m[@"RBFBDtoPixHz"] = @((uint32_t)(khz * 1000));
        d = m;

        printf("\nprogramming the DP pixel-clock DTO on pipe %ld:\n",
               pipe < 0 ? 1 : pipe);
        printf("  PHASE  = %ld Hz  (%.6f MHz)%s\n", khz * 1000,
               (double)khz / 1000.0,
               khz == 148500 ? "  <- 2BD's preferred timing, off this sink's "
                               "own EDID" : "");
        printf("  MODULO = pipe 0's own live value, COPIED, not written down.\n"
               "           2F0 read 600 000 000 there. PHASE/MODULO is a "
               "RATIO, so\n           reusing the firmware's reference makes "
               "this pipe as accurate\n           as the panel's and the TRUE "
               "reference never has to be known -\n           which is also "
               "what Linux does (one dp_dto_ref_khz per pipe).\n");
        printf("  PIXEL_RATE_CNTL: DP_DTO_ENABLE 1, PIPE_DTO_SRC_SEL %ld%s\n",
               srcsel < 0 ? 1L : (srcsel ? 1L : 0L),
               srcsel < 0 ? "  <- Linux's value; the firmware left pipe 0's "
                            "at 0" : "");
        printf("\nin Linux's order (PHASE, MODULO, then the enable), as an "
               "RMW that\nkeeps DTBCLK_DTO_DIV and the error bits - which is "
               "what REG_UPDATE_2\ncompiles to (2EE quoted the sequence).\n");
        printf("\nTHE VERDICT IS DPDTO_ENABLE_STATUS, a bit we do NOT write, "
               "whose two\nstates 2F0 attested on this chip before anything "
               "was programmed. PHASE\nis NOT the verdict: 2F0 measured it as "
               "the clock the firmware ASKED\nfor, so reading our own number "
               "back there would be rule 20's defect.\n");
        printf("\nThe write is refused unless THIS boot confirmed BASE_IDX 1 "
               "- at the\nother candidate base these offsets are the DOMAIN "
               "power gating of the\npipe scanning the panel.\n");
    } else {
        long want = atol(argv[1]);
        d = @{ @"RBFBDmubBl": @(want) };
        printf("\nsending SET_BACKLIGHT %ld (%.1f %%)...\n",
               want, (double)want * 100.0 / 65536.0);
    }

    const int isDom = (strcmp(argv[1], "dom") == 0);
    const long domCallsBefore = isDom ? num(fb, "RBFBDmubDomCalls") : 0;
    const int isDig = (strcmp(argv[1], "dig") == 0);
    const long digCallsBefore = isDig ? num(fb, "RBFBDigCalls") : 0;
    const int isDpAltQ = (strcmp(argv[1], "dpaltq") == 0);
    const long dpaltqCallsBefore = isDpAltQ ? num(fb, "RBFBDpAltQCalls") : 0;
    const int isDto = (strcmp(argv[1], "dto") == 0);
    const long dtoCallsBefore = isDto ? num(fb, "RBFBDtoProgCalls") : 0;

    kern_return_t kr = IORegistryEntrySetCFProperties(
        fb, (__bridge CFDictionaryRef)d);
    printf("-> 0x%x%s\n", kr, kr == KERN_SUCCESS ? " (the push returned OK)" : "");
    if (isDig) {
        digVerdictPrint(fb, digCallsBefore);
    } else if (isDpAltQ) {
        dpaltqReport(fb, dpaltqCallsBefore);
    } else if (isDto) {
        dtoVerdictPrint(fb, dtoCallsBefore);
    } else if (isDom) {
        /* The IOReturn is a yes/no over every refusal path at once, so the
         * tool reads the code itself rather than making the user decode an
         * errno-shaped number. */
        domVerdict(fb, domCallsBefore);
    } else {
        printf("\nthe kext logged the refusal code and the WPTR it wrote. Rule 31:\n"
               "the counters above are published periodically, so re-run with no\n"
               "argument in a second or two rather than reading an unchanged\n"
               "number as a failure.\n");
    }

    IOObjectRelease(fb);
    return kr == KERN_SUCCESS ? 0 : 1;
}
