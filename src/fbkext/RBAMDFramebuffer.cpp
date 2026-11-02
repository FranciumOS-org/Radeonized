/* SPDX-License-Identifier: MIT */
/* RBAMDFramebuffer: see RBAMDFramebuffer.hpp. */
#include "RBAMDFramebuffer.hpp"
#include "rb_backlight.h"
#include "rb_hubp.h"
#include "rb_defaults.h"
#include "rb_osver.h"
#include <libkern/version.h>

#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSString.h>
#include <kern/thread_call.h>
#include <pexpert/pexpert.h>
#include <mach/mach_time.h>

#define LOG(fmt, ...) IOLog("RBAMDFramebuffer: " fmt "\n", ##__VA_ARGS__)

OSDefineMetaClassAndStructors(RBAMDFramebuffer, IOFramebuffer)

namespace {

/* Every one of these was read off this machine, not chosen.
 *
 * `.Display_boot`'s live IOFBConfig carries a single mode, ID 100, with
 * DM = 80070000 38040000 3fd58f00 00000000, i.e. nominalWidth 0x780 = 1920,
 * nominalHeight 0x438 = 1080, refreshRate 0x008fd53f in 16.16 fixed = 143.83 Hz,
 * maxDepthIndex 0; and DF = 0x400003. The plugin's BAR5 OTG sampler measured
 * the same panel independently at 143.908 Hz by latching both endpoints on an
 * OTG0_FRAME_COUNT edge (2rn/2rm), which is the control on the 16.16 value.
 *
 * IOFBMemorySize reads 8294400 = 1920 * 1080 * 4, so the rows are NOT padded
 * and bytesPerRow is exactly 7680. The GOP framebuffer is BAR0 offset 0 (2pr).
 */
/* 2y6: the mode ids moved to CLASS constants in the header
 * (kModeIDFast = 100, kModeID60 = 101) because RBAMDFramebufferModes.cpp
 * needs them and this namespace is anonymous - the same trap 2y5 hit with
 * argOn. Nothing here redefines them. */
const UInt32 kWidth                 = 1920;
const UInt32 kHeight                = 1080;
const UInt32 kBytesPerRow           = 7680;
const UInt32 kApertureBytes         = 8294400;      /* IOFBMemorySize, live */

/* 2w9: the VRAM byte offset published as `ATY,fb_offset`. Non-zero because the
 * accelerator's gate rejects 0 (see the long note in start()). 16 MB is the
 * first offset that is both non-zero and clear of BAR0's first 16 MB, which
 * this project's own probe rules already treat as the boot framebuffer's. */
const UInt32 kAtyOffsetDefaultMB    = 16;

/* 2sr: THE BOOT-ARG IS THE REAL DISARM. The NVRAM one-shot is not.
 *
 * `rbgpu-fb` survived a reboot after being removed and read back as absent
 * (2sq), and the kernel says why that is possible:
 * `IODTNVRAMVariables::removeProperty` tail-jumps to
 * `IODTNVRAM::removePropertyWithGUIDAndName`, which calls **no** sync - the
 * removal is in memory until something else commits it. Only
 * `setPropertyWithGUIDAndName` calls `IODTNVRAM::syncInternal`, and it is the
 * single referrer of `IONVRAM-FORCESYNCNOW-PROPERTY` in the whole kernel. So
 * the read-back we perform proves the in-memory state and nothing about flash.
 *
 * docs/RULES.md rule 9 already states the answer, learned on other features and
 * not applied to this one: *the feature needs the arg AND the key, so without
 * the arg a stuck key is inert.* So arming requires BOTH, and the boot-arg -
 * removed with `tools/efi.sh bootarg remove -rbgpuownfb`, which edits a file and
 * is durable in a way an NVRAM one-shot demonstrably is not - is what makes a
 * stuck key harmless.
 *
 * The one-shot is kept for what it is good at: making a single armed boot
 * single, so a repeat needs a deliberate `nvram` command. It is no longer load
 * bearing for safety.
 *
 * Also mirrors RembrandtGPUPeer::armed()'s bounded retry for the /options race
 * (2in); shorter here because probe() runs after IODT matching has begun. */
/* 2xw: THE PANEL IS OURS BY DEFAULT, AND THE KEYS CHANGED DIRECTION.
 *
 * Until 2xw this needed `-rbgpuownfb` in boot-args AND `nvram rbgpu-fb=1`,
 * re-armed before every boot, because a framebuffer that takes the panel and
 * then fails leaves a screen that cannot be used to undo it - 2wj's boot 196
 * had to be disarmed from Windows. The configuration has been stable since
 * boot 215, so the default is now to take the panel, and the two keys that
 * used to arm it are read the other way round:
 *
 *   `-rbgpuoff` / `-rbgpustock` in boot-args  -> decline, always. RULE 38 is
 *       why this is not optional: the panel taken with no accelerator
 *       underneath freezes on the last frame drawn, so the boot-arg that
 *       disarms the accelerator must disarm this too, in the same boot.
 *   `nvram rbgpu-fb=0`                        -> decline. The user's durable
 *       disarm; we never write this key any more.
 *   `rbgpu-safe` still set                    -> decline. The plugin's boot
 *       sentinel (2xw) sets it before arming and clears it once a boot has
 *       survived a minute, so a boot that froze or panicked leaves it set and
 *       this one comes up stock. The plugin clears it, not us - one writer
 *       per key, so a stuck key can only ever mean "be careful".
 *
 * Everything on the refuse path says which of those it was (rule 14), because
 * "declined on purpose" and "never ran" look identical from the registry.
 *
 * Keeps 2in's bounded retry for the /options race. A difference worth stating:
 * if /options cannot be read at all, we now take the panel, where the one-shot
 * form declined - default-on means an unreadable NVRAM cannot hide a disarm,
 * and the boot-arg is the disarm that still works in that case. */
uint32_t armRefuseWhy = 0;   /* 0 armed, 2 -rbgpuoff/-rbgpustock, 3 rbgpu-fb=0,
                              * 4 /options unreachable (armed anyway),
                              * 5 -rbgpunoownfb, 6 the sentinel latch,
                              * 7 rbgpu-arm=0 (rule 38), 9 the peer ran first
                              * and said stock, 10 the peer ran first and said
                              * armed */

/* 2xz: the framebuffer's half of the OS gate. The plugin asks the identical
 * question in rbgpu_os_measured(), off the identical kernel global, because
 * rule 38 is only satisfied when both halves answer it the same way - and
 * this half can probe before the plugin has run at all, so it cannot ask the
 * plugin.
 *
 * What it gates here is narrower than in the plugin and that is deliberate.
 * This kext has no offsets into any Apple driver object: it is an
 * IOFramebuffer subclass, every register it touches is the APU's own and
 * carries a Linux header citation, and the one Apple structure it reads is
 * IODetailedTimingInformationV2 through the public header. So nothing here
 * NEEDS RB_OSD_KEXTOFF. It still honours it, for one reason: rule 38's own
 * case. The panel taken with no accelerator underneath freezes on the last
 * frame drawn, and on an OS where the plugin comes up stock there IS no
 * accelerator underneath. Declining the panel together is the whole point. */
static bool osMeasured(void)
{
    static int cached = -1;

    if (cached < 0) {
        char buf[16];
        const int major = version_major;
        const bool ok   = rb_os_derived(major, RB_OSD_KEXTOFF) != 0;

        cached = (ok || PE_parse_boot_argn("-rbgpuosforce", buf, sizeof(buf)))
                 ? 1 : 0;
        if (!ok) {
            LOG("2xz: Darwin %d (%s) - nothing in this tree was measured here; "
                "%s", major, rb_os_name(major),
                cached ? "-rbgpuosforce is in boot-args, so arming anyway"
                       : "declining the panel so it is not taken with no "
                         "accelerator underneath (rule 38)");
        }
    }
    return cached != 0;
}

bool argOn(const char *tok)
{
    char buf[16], off[64];

    if (!rb_default_on(tok)) {
        return PE_parse_boot_argn(tok, buf, sizeof(buf)) != 0;
    }
    if (PE_parse_boot_argn("-rbgpustock", buf, sizeof(buf)) ||
        PE_parse_boot_argn("-rbgpuoff", buf, sizeof(buf)) ||
        !osMeasured()) {
        return false;
    }
    if (!rb_off_token(tok, off, sizeof(off))) {
        return true;
    }
    return PE_parse_boot_argn(off, buf, sizeof(buf)) == 0;
}

bool keyReadsZero(IORegistryEntry *options, const char *var, bool *present);

/* 2xx: does a "0" in this key mean the user disarmed us, or is it the value
 * one of our own pre-2xw builds wrote when it spent a one-shot by overwrite?
 * `rb_key_disarms` in rb_defaults.h holds the rule, and the plugin asks it
 * the same question with the same inputs - rule 38 is only satisfied if both
 * halves answer identically, and the framebuffer may well probe BEFORE the
 * plugin's migration has run. */
bool keyDisarms(IORegistryEntry *options, const char *var)
{
    bool zero = keyReadsZero(options, var, nullptr);
    bool gen  = options->getProperty(RB_GEN_KEY) != nullptr;

    if (zero && !gen && !rb_key_disarms(var, 1, 0)) {
        LOG("%s reads \"0\" but " RB_GEN_KEY " is absent, so that is a spent "
            "pre-2xw one-shot and not a disarm (2xx); ignoring it.", var);
    }
    return rb_key_disarms(var, zero ? 1 : 0, gen ? 1 : 0) != 0;
}

bool keyReadsZero(IORegistryEntry *options, const char *var, bool *present)
{
    OSObject *val = options->getProperty(var);
    if (present) {
        *present = val != nullptr;
    }
    if (!val) {
        return false;
    }
    /* `nvram var=1` stores an OSData, so check the first byte rather than
     * expecting a string - the 2cs lesson, unchanged. */
    if (auto data = OSDynamicCast(OSData, val)) {
        return data->getLength() >= 1 &&
               *static_cast<const char *>(data->getBytesNoCopy()) == '0';
    }
    if (auto str = OSDynamicCast(OSString, val)) {
        return str->isEqualTo("0");
    }
    return false;
}

bool armedOnce(void)
{
    char flag[16];

    if (PE_parse_boot_argn("-rbgpuoff", flag, sizeof(flag)) ||
        PE_parse_boot_argn("-rbgpustock", flag, sizeof(flag))) {
        LOG("-rbgpuoff or -rbgpustock is in boot-args, so the accelerator is "
            "not armed either; declining the panel (rule 38 - the panel taken "
            "with no accelerator underneath freezes). IONDRVFramebuffer keeps "
            "it.");
        armRefuseWhy = 2;
        return false;
    }

    if (!argOn("-rbgpuownfb")) {
        LOG("-rbgpunoownfb is in boot-args; declining the panel, so "
            "IONDRVFramebuffer keeps it and the desktop composites in "
            "software (2wl measured that configuration as usable).");
        armRefuseWhy = 5;
        return false;
    }

    IORegistryEntry *options = nullptr;
    unsigned tries = 0;
    for (; tries < 5; tries++) {
        options = IORegistryEntry::fromPath("/options", gIODTPlane);
        if (options) {
            break;
        }
        IOSleep(100);
    }
    if (!options) {
        LOG("/options not found after %u tries; taking the panel anyway (2xw: "
            "armed by default, and an unreadable NVRAM cannot hold a disarm - "
            "`-rbgpuoff` in boot-args is the disarm that always works)", tries);
        armRefuseWhy = 4;
        return true;
    }

    /* RULE 38, as a check instead of a comment. The framebuffer writes HUBP
     * from `setAttribute('amtr')` and from nowhere else, so with the panel
     * taken and no accelerator underneath the screen freezes on whatever was
     * last drawn - guaranteed, not merely possible. It happened after boot
     * 197. The accelerator's own durable disarm is `nvram rbgpu-arm=0`, so
     * that key has to decline the panel too, in the same boot. */
    if (keyDisarms(options, "rbgpu-arm")) {
        LOG("rbgpu-arm reads \"0\", so the accelerator will not arm; declining "
            "the panel as well (rule 38 - the panel with no accelerator "
            "underneath freezes on the last frame).");
        armRefuseWhy = 7;
        options->release();
        return false;
    }
    if (keyDisarms(options, "rbgpu-fb")) {
        LOG("rbgpu-fb reads \"0\"; declining the panel. That is the durable "
            "disarm - re-arm with `sudo nvram -d rbgpu-fb`.");
        armRefuseWhy = 3;
        options->release();
        return false;
    }
    /* 2xw: if the peer has already run, its answer beats any key we could
     * read - see the long note at its setProperty("RBGPUStock"). This is the
     * only thing that makes the sentinel latch unambiguous for us: "1" means
     * "the previous boot did not survive" when we probe first, and "this boot
     * is still proving itself" when we probe second, and nothing in NVRAM
     * tells the two apart. */
    if (OSDictionary *match = IOService::serviceMatching("RembrandtGPUPeer")) {
        IOService *peer = IOService::copyMatchingService(match);
        match->release();
        if (peer) {
            OSBoolean *stock = OSDynamicCast(OSBoolean,
                                             peer->getProperty("RBGPUStock"));
            bool peerStock = stock != nullptr && stock->isTrue();
            peer->release();
            if (stock) {
                LOG("the peer has already run and says stock=%d; %s (rule 38 - "
                    "its answer decides, not the sentinel latch, which cannot "
                    "be told apart from this boot's own)",
                    peerStock ? 1 : 0,
                    peerStock ? "declining the panel" : "taking the panel");
                armRefuseWhy = peerStock ? 9 : 10;
                options->release();
                return !peerStock;
            }
        }
    }

    bool safePresent = false;
    bool safeZero = keyReadsZero(options, "rbgpu-safe", &safePresent);
    if (safePresent && !safeZero) {
        LOG("rbgpu-safe is still set, so the previous boot did not survive its "
            "first minute; declining the panel for this boot. The plugin "
            "clears the latch, so the next boot takes it again.");
        armRefuseWhy = 6;
        options->release();
        return false;
    }

    LOG("taking the panel (2xw: armed by default). Disarm for one boot with "
        "`sudo nvram rbgpu-fb=0`, or durably with `sudo tools/efi.sh bootarg "
        "add -rbgpuoff`.");
    armRefuseWhy = 0;
    options->release();
    return true;
}

} /* namespace */

/* 2y5: the one way out of this file's anonymous namespace. */
bool RBAMDFramebuffer::argEnabled(const char *tok)
{
    return argOn(tok);
}

/* Did we DISPLACE the boot framebuffer, or are we attached beside it?
 *
 * This is not a configuration flag, it is a fact about this instance, and it is
 * readable: only one driver wins a given (provider, IOMatchCategory) pair, so an
 * instance whose own IOMatchCategory is `IOFramebuffer` took that category from
 * IONDRVSupport's personality and therefore owns the panel and the console. Any
 * other category means IONDRVFramebuffer is still there and still the console.
 *
 * Two answers depend on it, and both were wrong before this existed:
 *
 *   isConsoleDevice()   - claiming the console while IONDRVFramebuffer holds it
 *                         invites IOFramebuffer to hand the kernel console to an
 *                         instance that is not driving it. On a framebuffer that
 *                         is the screen, that is a black screen with no SSH.
 *   getConnectionCount()- claiming a live connection beside the real framebuffer
 *                         conjures a second 1920x1080 display for WindowServer.
 *
 * In the beside configuration the truthful answers are "no" and "none", and the
 * whole point of that configuration is to exercise IOFramebuffer::start() while
 * the real framebuffer keeps the display. Answering otherwise would be claiming
 * hardware we are not driving - the shape of error rule 17 is about. */
bool RBAMDFramebuffer::ownsPanel(void)
{
    if (auto cat = OSDynamicCast(OSString, getProperty(gIOMatchCategoryKey))) {
        return cat->isEqualTo("IOFramebuffer");
    }
    /* No category property at all: assume we do NOT own the panel. The safe
     * direction for an unknown is the one that touches nothing. */
    return false;
}

IOService *RBAMDFramebuffer::probe(IOService *provider, SInt32 *score)
{
    if (!IOFramebuffer::probe(provider, score)) {
        return nullptr;
    }
    if (!OSDynamicCast(IOPCIDevice, provider)) {
        LOG("provider is not an IOPCIDevice; declining");
        return nullptr;
    }
    /* Refuse unless both gates are armed. The one-shot removal is checked
     * in memory; only the boot-arg is a durable disarm (2sq/2sr). */
    if (!armedOnce()) {
        return nullptr;
    }
    LOG("armed; claiming the IOFramebuffer match category on %s (score %d)",
        provider->getName(), score ? *score : 0);
    return this;
}

bool RBAMDFramebuffer::start(IOService *provider)
{
    pci = OSDynamicCast(IOPCIDevice, provider);
    if (!pci) {
        return false;
    }
    bzero(interrupts, sizeof(interrupts));
    vblRunning = false;
    vblFires = 0;
    vblEnables = 0;
    vblNextDeadline = 0;
    vblResyncs = 0;

    /* 2wc: 'amtr'. Armed with -rbgpufbamtr; the flag is published
     * UNCONDITIONALLY a few lines below, because rule 15's cheapest
     * independent check that the new binary is the one running is a key the
     * new code publishes whatever the boot-args say. */
    amtrArmed = false;
    amtrSnapped = false;
    amtrGets = 0;
    amtrSets = 0;
    selfDisarmed = false;
    amtrPeak = 0;
    amtrBucket = 0;
    amtrNoFlip = 0;
    amtrPtr = 0;
    amtrRefused = 0;
    bzero(amtrSnap, sizeof(amtrSnap));
    /* 2A0: the frame-time budget starts ON. It is two mach_absolute_time()
     * reads and four adds per transaction at ~73 transactions a second, and
     * the control for that claim is `{"RBFBFtOn": 0}` inside the same boot
     * (rule 16 wants the suspect removable, not absent). */
    ftOn = true;
    ftPrevExit = 0;
    ftResets = 0;
    rb_ft_reset(&ftWork);
    rb_ft_reset(&ftGap);
    rb_ft_reset(&ftGet);
    rb_ft_reset(&ftVbl);
    ftPrevGetExit = 0;
    ftPrevGetHeld = false;
    rb_ft_reset(&ftRetry);
    rb_ft_reset(&ftPoll);
    amtrFlipSnapped = false;
    amtrFlipPtr = 0;
    amtrFlipRefused = 0;
    bzero(amtrFlipSnap, sizeof(amtrFlipSnap));
    {
        amtrArmed = argOn("-rbgpufbamtr");
    }

    /* 2wh: the HUBP write. Armed with -rbgpufbhubp, and it needs
     * -rbgpufbamtr too, because the address it writes arrives only through
     * setAttribute('amtr'). Rule 30: both names are whole tokens and neither
     * prefixes the other, so PE_parse_boot_argn cannot confuse them.
     * rbgpufbhubpmax=<N> bounds the writes for a deliberately conservative
     * first boot; 0 or absent means unlimited, which is what a live display
     * needs. */
    hubpArmed = false;
    hubpAll = false;
    /* 2x3: -rbgpufbpoll. Rule 30 checked against every existing token: it
     * neither prefixes nor is prefixed by -rbgpufbaty / -rbgpufbamtr /
     * -rbgpufbhubp / -rbgpufbretire / -rbgpufbflipall / -rbgpufbvbl /
     * -rbgpufbvram / -rbgpufbaccel / -rbgpufbmatch / -rbgpufbnocrumb. */
    pollArmed = false;
    pollReads = 0;
    pollChanges = 0;
    pollFirstSeen = 0;
    {
        pollArmed = argOn("-rbgpufbpoll");
    }
    hubpMax = 0;
    hubpRejectAddr = 0;
    hubpRejectWhy = 0;
    {
        hubpArmed = argOn("-rbgpufbhubp")
                    && amtrArmed;
        /* 2wm: rule 30 - NOT -rbgpufbhubpall, because `-rbgpufbhubp` would
         * prefix it and a shell grep for the existing arg would then match
         * the new one. -rbgpufbflipall neither prefixes nor is prefixed by
         * any token already in use. */
        hubpAll = argOn("-rbgpufbflipall")
                  && hubpArmed;
        int maxn = 0;
        if (PE_parse_boot_argn("rbgpufbhubpmax", &maxn, sizeof(maxn)) &&
            maxn > 0 && maxn <= 1000000) {
            hubpMax = (uint64_t)maxn;
        }
        /* 2zk: rbgpufbpubms=<0..10000>, the hot-path publish throttle in
         * milliseconds. Default 500, which is already four times slower than
         * the 288-vblank periodic publish and so changes nothing anyone
         * reads. 0 restores the unthrottled pre-2zk behaviour and is the A
         * side of the comparison (rule 44). Rule 40: parsed as a binary int,
         * and a value outside the range leaves the default rather than being
         * clamped silently - RBFBPubMs publishes what took effect. */
        pubMs = 500;
        {
            int pms = -1;
            if (PE_parse_boot_argn("rbgpufbpubms", &pms, sizeof(pms)) &&
                pms >= 0 && pms <= 10000) {
                pubMs = (uint32_t)pms;
            }
        }
        pubIntervalAbs = 0;
        pubNextAbs = 0;
        pubHotCalls = 0;
        pubHotDone = 0;
        pubSetsOk = 0;
        pubSetsRefused = 0;
        pubSetsOther = 0;
        pubMsLast = -1;
        if (pubMs != 0) {
            nanoseconds_to_absolutetime((uint64_t)pubMs * 1000000ull,
                                        &pubIntervalAbs);
        }
        /* 2xm: -rbgpufbflippend defers a flip programmed over an armed,
         * unlatched one - the tearing mechanism boot 217 measured at 1.75 %
         * of flips. Rule 30: it neither prefixes nor is prefixed by
         * -rbgpufbflipall (they differ at the character after "flip"), and
         * PE_parse_boot_argn matches a whole token regardless. */
        hubpDeferPend = argOn("-rbgpufbflippend") && hubpArmed;
        /* 2xn: -rbgpufbnotready answers the 'amtr' GET the way Apple's own
         * AMDFramebuffer does - NotReady while a transaction is outstanding -
         * instead of claiming instant completion. Rule 30: NOT
         * `-rbgpufbamtrwait`, because `-rbgpufbamtr` would prefix it; nothing
         * in use prefixes `-rbgpufbnotready` or is prefixed by it
         * (`-rbgpufbnocrumb` shares only `-rbgpufbn`). */
        amtrNotReady = argOn("-rbgpufbnotready") && hubpArmed;
        /* 2xp: -rbgpufbotgany relaxes gate 1's OTG1-3 clause so 2or's cheap
         * HDMI test can be run WITHOUT freezing the laptop panel. Rule 30:
         * no token in use prefixes it or is prefixed by it. */
        hubpOtgAny = argOn("-rbgpufbotgany") && hubpArmed;
    }
    /* 2xs: -rbgpufbbacklight. Deliberately NOT gated on hubpArmed or
     * amtrArmed: the backlight needs only the panel, and coupling it to the
     * HUBP write would mean a boot that cannot test one cannot test the
     * other (rule 38 names what each half depends on - this half depends on
     * nothing but owning the panel). Rule 30 checked against every token in
     * use: -rbgpufbaty / -rbgpufbamtr / -rbgpufbhubp / -rbgpufbflipall /
     * -rbgpufbflippend / -rbgpufbnotready / -rbgpufbotgany / -rbgpufbretire
     * / -rbgpufbvbl / -rbgpufbvram / -rbgpufbaccel / -rbgpufbmatch /
     * -rbgpufbvramrange / -rbgpufbnocrumb / -rbgpufbpoll - none prefixes
     * -rbgpufbbacklight and it prefixes none of them (the longest shared
     * prefix with any of them is `-rbgpufb`). */
    dmubInit();
    dmubBlArmed = false;
    blArmed = false;
    blBuiltIn = false;
    blFloor = RB_BL_LEVEL_FLOOR;
    blFloorRefused = 0;
    {
        blArmed = argOn("-rbgpufbbacklight");
        /* 2xy: -rbgpufbbuiltin, the AppleBacklightDisplay match. Deliberately
         * NOT `&& blArmed`: arming the match with the write off is a useful
         * boot - it answers "does AppleBacklightDisplay match on this
         * machine" without one register write - and arming the write with the
         * match off is 2xs' own never-booted configuration. Rule 30 and the
         * full reasoning are on getAppleSense in
         * RBAMDFramebufferBacklight.cpp. */
        blBuiltIn = argOn("-rbgpufbbuiltin");
        /* 2y0: -rbgpufbblparm publishes the full-name "brightness" parameter
         * through our own IODisplayParameterHandler. Rule 30 against every
         * token in use: it shares `-rbgpufbb` with -rbgpufbbacklight and
         * -rbgpufbbuiltin and diverges at the next character (l against a
         * and u), so no two of the three prefix each other, and the derived
         * negatives -rbgpunofbblparm / -rbgpunofbbacklight /
         * -rbgpunofbbuiltin are distinct for the same reason. The hosttest
         * asserts it over the whole namespace. */
        blParmArmed = argOn("-rbgpufbblparm");
        /* 2zn: -rbgpufbdmubbl arms the DMCUB route - the one 2zl identified
         * as the real one. Opt-in and will stay opt-in: it is a new
         * hardware-write surface, a 64-byte command into memory the
         * microcontroller reads, and even armed nothing is sent until a
         * userspace trigger names a level (RBAMDFramebufferDMUB.cpp says
         * why the slider is not wired to it). Rule 30 against every token in
         * use: the only one sharing more than `-rbgpufb` is -rbgpufbdrr, and
         * the two diverge at the next character (m against r), so neither
         * prefixes the other and the derived negatives -rbgpunofbdmubbl /
         * -rbgpunofbdrr are distinct for the same reason. */
        dmubBlArmed = argOn("-rbgpufbdmubbl");
        /* rbgpufbblfloor=<N>, in Linux's u16.16 units where 0x10000 is full
         * scale. Rule 40: the value comes back as a BINARY INT of the
         * caller's width, so this is an int and not a char buffer - the bug
         * that made three tunables inert for twenty milestones. Refused
         * outside 1 %..50 %: a floor of 0 is the one state rule 12 says we
         * cannot diagnose from, and a floor above half the range would make
         * the control useless. */
        int floorn = 0;
        if (PE_parse_boot_argn("rbgpufbblfloor", &floorn, sizeof(floorn))) {
            if (floorn >= (int)(RB_BL_LEVEL_FULL / 100u) &&
                floorn <= (int)(RB_BL_LEVEL_FULL / 2u)) {
                blFloor = (uint32_t)floorn;
            } else {
                blFloorRefused = (uint32_t)floorn;
            }
        }
    }
    /* 2wi: -rbgpufbretire. Deliberately NOT named -rbgpufbvbl-something:
     * rule 30 warns that a shell grep for an existing arg matches a longer
     * one, and `-rbgpufbvbl` is already in use. */
    retireArmed = false;
    retireEnables = 0;
    retireKicks = 0;
    retireDelivered = 0;
    vblKickCall = nullptr;
    {
        retireArmed = argOn("-rbgpufbretire");
    }

    hubpSetup();
    drrReset();       /* 2y5: before anything can publish it */
    modeReset();      /* 2y6: and before getDisplayModeCount can be asked */
    vblPeriodNs = kVBLPeriodNsFast;
    vblRetunes = 0;
    otgTimingReset(); /* 2y4: before blSetup, which may not run at all */
    blSetup();        /* 2xs: independent of hubpSetup's result by design */
    /* 2y0: AFTER blSetup, because it refuses unless blReady, and BEFORE
     * IOFramebuffer::start() below, because that is what can create an
     * IODisplayConnect - and IOKit matching is asynchronous, so a handler
     * registered afterwards would be racing the display's own driver to
     * exist before IODisplay walks our children. */
    blParamsSetup();
    /* 2y6: BEFORE drrSetup, which declines if the mode table is armed, and
     * before IOFramebuffer::start() below, because that is what asks
     * getDisplayModeCount() - a second mode appearing afterwards would be a
     * mode list that changed under the compositor. */
    modeSetup();
    drrSetup();       /* 2y5: needs blSetup's BAR5 map, so it follows it */

    /* 2t0: the breadcrumb target. Opened once and retained, because crumb()
     * is also called from the VBL thread_call and fromPath there would be a
     * registry walk per milestone. A failure is reported, not fatal - rule 14,
     * and the kext must still work with no instrument. */
    crumbStage = 0;
    char nocrumb[16];
    crumbOn = !PE_parse_boot_argn("-rbgpufbnocrumb", nocrumb, sizeof(nocrumb));
    nvram = crumbOn ? IORegistryEntry::fromPath("/options", gIODTPlane) : nullptr;
    /* Snapshot the boot counter the plugin publishes. "?" is a reading too
     * (rule 14): it means the plugin did not run, so a crumb from this boot
     * cannot be told from an older one and must not be trusted. */
    crumbBoot[0] = '?';
    crumbBoot[1] = '\0';
    if (nvram) {
        if (OSObject *bv = nvram->getProperty("rbgpu-boot")) {
            const char *p = nullptr;
            unsigned n = 0;
            if (auto d = OSDynamicCast(OSData, bv)) {
                p = static_cast<const char *>(d->getBytesNoCopy());
                n = d->getLength();
            } else if (auto st = OSDynamicCast(OSString, bv)) {
                p = st->getCStringNoCopy();
                n = st->getLength();
            }
            if (p && n) {
                if (n > sizeof(crumbBoot) - 1) {
                    n = sizeof(crumbBoot) - 1;
                }
                unsigned w = 0;
                for (unsigned i = 0; i < n; i++) {
                    /* `nvram` stores this with a trailing NUL inside the data
                     * ("171%00" as printed), so stop at the first non-digit
                     * rather than copying embedded junk into the key. */
                    if (p[i] < '0' || p[i] > '9') {
                        break;
                    }
                    crumbBoot[w++] = p[i];
                }
                crumbBoot[w] = '\0';
                if (w == 0) {
                    crumbBoot[0] = '?';
                    crumbBoot[1] = '\0';
                }
            }
        }
    }
    setProperty("RBFBCrumbBoot", crumbBoot);
    setProperty("RBFBCrumbOn", crumbOn && nvram != nullptr);
    if (crumbOn && !nvram) {
        LOG("/options not found; crash breadcrumbs are OFF for this boot "
            "(nvram rbgpu-fbcrumb will be stale - do not read it as this boot's)");
    }
    crumb(1);   /* start() entered, provider is an IOPCIDevice */

    /* The firmware already lit the panel and the GOP framebuffer is BAR0
     * offset 0 (2pr), so the aperture is a sub-range of BAR0 rather than
     * anything we program. getDeviceMemoryWithRegister returns a reference
     * owned by the PCI device; withSubRange retains it for us. */
    IODeviceMemory *bar0 = pci->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
    if (!bar0) {
        LOG("no BAR0 device memory; refusing to start");
        return false;
    }
    aperture = IODeviceMemory::withSubRange(bar0, 0, kApertureBytes);
    if (!aperture) {
        LOG("could not make the %u-byte aperture sub-range of BAR0", kApertureBytes);
        return false;
    }
    LOG("aperture: BAR0 phys %#llx + 0, %u bytes, %ux%u stride %u",
        bar0->getPhysicalSegment(0, nullptr, 0), kApertureBytes,
        kWidth, kHeight, kBytesPerRow);
    crumb(2);   /* aperture sub-range made */

    vblCall = thread_call_allocate(vblFired, this);
    if (!vblCall) {
        LOG("thread_call_allocate failed; the vblank will not be delivered");
    }
    /* 2wk: a separate call for the immediate kick, so it can never cancel or
     * reschedule a pending periodic tick. A failure here is reported and not
     * fatal - the periodic path still runs (rule 14). */
    vblKickCall = thread_call_allocate(vblKicked, this);
    if (!vblKickCall) {
        LOG("kick thread_call_allocate failed; retirement will rely on the "
            "periodic tick, which boot 195 measured as always too late");
    }
    setProperty("RBFBVBLTimerAllocated", vblCall != nullptr);
    publishVBLState();  // zero is a reading; missing properties are not
    crumb(3);   /* thread_call allocated (or refused, see the property) */

    /* 2t3: these three are INPUTS. IOFramebuffer::open() calls
     * getProperty("IOFBGammaWidth"/"IOFBGammaCount"/"IOFBGammaHeaderSize") on
     * the driver, casts each to an OSNumber and stores it at private+0x1a0 /
     * +0x1a4 / +0x198; when the property is absent the safeMetaCast returns
     * NULL, the je is taken and the field stays ZERO (measured in
     * IOGraphicsFamily at 0x14fc6304-0x14fc63b4). So an absent key does not
     * mean "default gamma", it means "this framebuffer has no gamma", which is
     * what we have been telling IOGraphics - and matches 2sa's
     * `System Gamma - Unknown`, the one DisplayPipeState operation that never
     * resolved.
     *
     * The values are MEASURED, not chosen: they are what IONDRVFramebuffer
     * published on THIS machine (captured in logs/). Rule 17. */
    setProperty("IOFBGammaCount", 256ULL, 32);
    setProperty("IOFBGammaWidth", 8ULL, 32);
    setProperty("IOFBGammaHeaderSize", 12ULL, 32);
    gammaCalls = 0;
    setProperty("RBFBGammaCalls", 0ULL, 64);

    crumb(4);   /* about to enter IOFramebuffer::start - the superclass */
    if (!IOFramebuffer::start(provider)) {
        LOG("IOFramebuffer::start refused");
        crumb(5);   /* ...and it refused */
        return false;
    }
    crumb(6);   /* IOFramebuffer::start accepted */

    /* The labels System Profiler reads. Values measured, not invented: the
     * 4096 MB is what the plugin publishes from the real carve-out size. */
    setProperty("VRAM,totalMB", 4096ULL, 32);
    setProperty("model", "AMD Radeon 680M");

    /* 2w9: THE TWO PROPERTIES THE ACCELERATOR READS OFF THE FRAMEBUFFER.
     *
     * Boot 189 measured `pipe->0x340 == 0` as the single reason
     * `AMDRadeonX6000_AMDAccelDisplayPipe::initFramebufferResource`
     * (0xbe49356) returns 0 silently, which leaves `pipe->0x298` unset, which
     * makes `IOAccelDisplayPipe::isActive()` false, which is why
     * `IOAccelDisplayPipeUserClient2::transactionEnd` answered
     * kIOReturnNotReady 140 times. Read from the consumer, not guessed:
     *
     *   AMDRadeonX6000_AMDHWDisplay::getDisplayInfo (0xbe8700e)
     *       info+0x00 = *(uint64_t *)((OSData *)fb->getProperty
     *                                  ("ATY,fb_offset"))->getBytesNoCopy()
     *       info+0x0c = *(uint32_t *)((OSData *)fb->getProperty
     *                                  ("ATY,fb_size"))->getBytesNoCopy()
     *   AMDRadeonX6000_AMDHWDisplay::isDisplayEnabled (0xbe86f9c)
     *       returns `*(uint64_t *)ATY,fb_size != 0`, and
     *       set_display_mode_and_vram (0xbe48604) stores that in pipe->0x35d
     *       and then REQUIRES it to be 1.
     *
     * `info` IS `&pipe->0x340`, passed as the out-parameter of slot 0x130
     * (rule 7: dump 0x140 = getDisplayInfo) from set_display_mode_and_vram.
     * So both of these are hard gates and BOTH VALUES MUST BE NON-ZERO.
     *
     * The class is OSData, established from the use and not from the GOT: the
     * consumer calls vt[0x178] and then DEREFERENCES the result, which is
     * getBytesNoCopy() returning a pointer. An OSNumber would have handed back
     * the value itself. Hence setProperty(key, bytes, length) - the overload
     * AMDFramebuffer itself uses at 0x2a4478/0x2a4523 with ecx = 8.
     *
     * fb_offset is a BYTE OFFSET, not an address. initFramebufferResource adds
     * a base to it before reserving:
     *     addr = pipe->0x340 + hw->0x370->vt[0x1e0]()   (= mem->0x50)
     *     reserveFrameBuffer(pipe, addr, max(info+0x08, info+0x0c), resource)
     * where pipe->0x328 is accel->0x1a38 = AMDRadeonX6000_AMDNavi21Hardware.
     *
     * WHY THE OFFSET IS NOT ZERO, WHICH WOULD BE THE TRUTHFUL VALUE. Our
     * scanout IS at VRAM offset 0 - the GOP framebuffer is BAR0 offset 0 (2pr)
     * and the aperture is withSubRange(bar0, 0, kApertureBytes). But the gate
     * is `cmpq $0x0, (%r15); je fail`, so an offset of 0 is indistinguishable
     * from the property being absent, and AMD's own driver never puts the
     * scanout at offset 0. So this boot deliberately publishes a non-zero
     * offset while the display engine keeps scanning offset 0.
     *
     * THE CONSEQUENCE, STATED PLAINLY: if this works, the pipe goes ACTIVE and
     * the compositor renders at kAtyOffsetDefault while the panel still shows
     * whatever is at offset 0 - so the expected outcome is a stale or wrong
     * IMAGE with a working PIPE. SSH and the probe are the instruments, not the
     * screen. Pointing the scanout at the same offset means writing DCN
     * primary-surface registers, which this kext has never done and which is a
     * separate, narrower step once the chain is proven.
     *
     * Off unless `-rbgpufbaty` is in boot-args. Rule 30: the token is tested
     * whole by PE_parse_boot_argn, and `-rbgpufbaty` neither prefixes nor is
     * prefixed by -rbgpufbvbl / -rbgpufbvram / -rbgpufbvramrange /
     * -rbgpufbaccel / -rbgpufbmatch / -rbgpufbnocrumb. */
    {
        bool atyOn = argOn("-rbgpufbaty");
        setProperty("RBFBAtyArmed", atyOn);
        /* 2xw: why the panel was taken, published unconditionally beside the
         * other arm keys (rule 15). 0 armed by default, 4 /options was not
         * readable and we armed anyway. A DECLINE cannot publish anything -
         * there is no service - and is read the other way round: an
         * IONDRVFramebuffer in ioreg means we declined, and boot-args plus
         * `nvram rbgpu-fb rbgpu-safe` say which of the three reasons it was. */
        setProperty("RBFBArmWhy", (UInt64)armRefuseWhy, 32);
        setProperty("RBFBAmtrArmed", amtrArmed);
        /* 2A2: the hold is a runtime dial now, so its state must be readable
         * or an A/B cannot verify which side it is on (rule 20). */
        setProperty("RBFBAmtrNotReady", amtrNotReady);
        if (atyOn) {
            UInt32 mb = kAtyOffsetDefaultMB;
            int want = 0;
            if (PE_parse_boot_argn("rbgpufbatymb", &want, sizeof(want))) {
                if (want >= 1 && want <= 3072) {
                    mb = (UInt32)want;
                } else {
                    /* Rule 19: a refusal path publishes the value it refused. */
                    setProperty("RBFBAtyOffsetRefused", (UInt64)want, 32);
                    LOG("rbgpufbatymb=%d is outside 1-3072; keeping %u MB",
                        want, mb);
                }
            }
            UInt64 offset = (UInt64)mb << 20;
            UInt64 size   = kApertureBytes;

            setProperty("ATY,fb_offset", &offset, (unsigned int)sizeof(offset));
            setProperty("ATY,fb_size",   &size,   (unsigned int)sizeof(size));

            /* Rule 31: IOLog is not an instrument here - publish what we wrote
             * so `ioreg -rl -c RBAMDFramebuffer` can confirm it, and publish it
             * under our OWN names so the ATY keys stay exactly 8 raw bytes. */
            setProperty("RBFBAtyOffset", offset, 64);
            setProperty("RBFBAtySize", size, 64);
            setProperty("RBFBAtyOffsetMB", (UInt64)mb, 32);
            LOG("published ATY,fb_offset = 0x%llx (%u MB) and "
                "ATY,fb_size = %llu as 8-byte OSData",
                offset, mb, size);
        } else {
            LOG("-rbgpufbaty is not in boot-args; ATY,fb_offset and "
                "ATY,fb_size are NOT published, so initFramebufferResource "
                "will keep failing on pipe->0x340 == 0 (2w9)");
        }
    }
    LOG("started; %s", ownsPanel()
        ? "OWNS THE PANEL (displaced IONDRVFramebuffer): console yes, 1 connection"
        : "attached BESIDE the boot framebuffer: console no, 0 connections - "
          "this configuration measures start(), it does not drive the display");
    crumb(7);   /* start() returning true */
    return true;
}

/* -- gamma --------------------------------------------------------------- */

/* 2t3: we accept the table and program NOTHING, which is the honest behaviour
 * for this driver and needs saying plainly, because rule 20 exists to forbid
 * exactly the shape this has: a call that reports success without anything
 * being applied.
 *
 * Why accepting is still right: the panel is lit by firmware and this driver
 * deliberately writes nothing to the display engine (2sj/2sm), so the LUT in
 * DCN keeps whatever the firmware programmed - which is the state the desktop
 * already runs in today. Returning an error instead would leave IOGraphics
 * believing gamma is broken rather than simply fixed, and `IOBootFramebuffer`
 * - Apple's own software framebuffer - overrides this same 4-argument method.
 *
 * What makes the no-op auditable rather than a lie: every call is counted and
 * published, with its parameters, so `ioreg` says whether macOS ever actually
 * set a table and with what shape. If RBFBGammaCalls stays 0 then publishing
 * the three keys changed nothing downstream and this override is inert - which
 * is a result, not a silence (rule 14). */
IOReturn RBAMDFramebuffer::setGammaTable(UInt32 channelCount, UInt32 dataCount,
                                         UInt32 dataWidth, void *data)
{
    gammaCalls++;
    setProperty("RBFBGammaCalls", gammaCalls, 64);
    setProperty("RBFBGammaChannels", (UInt64)channelCount, 32);
    setProperty("RBFBGammaDataCount", (UInt64)dataCount, 32);
    setProperty("RBFBGammaDataWidth", (UInt64)dataWidth, 32);
    setProperty("RBFBGammaDataNull", data == nullptr);
    if (gammaCalls == 1) {
        LOG("setGammaTable #1: channels %u, entries %u, width %u, data %s - "
            "ACCEPTED AND NOT PROGRAMMED (firmware LUT retained; this driver "
            "writes nothing to the display engine)",
            channelCount, dataCount, dataWidth, data ? "present" : "NULL");
    }
    return kIOReturnSuccess;
}

IOReturn RBAMDFramebuffer::setGammaTable(UInt32 channelCount, UInt32 dataCount,
                                         UInt32 dataWidth, void *data,
                                         bool /*syncToVBL*/)
{
    /* Both arities are exported by IOGraphicsFamily
     * (__ZN13IOFramebuffer13setGammaTableEjjjPv and ...EjjjPvb) and which one
     * macOS calls here is NOT established, so both are overridden and the
     * five-argument form delegates. There is no hardware write to sync to. */
    return setGammaTable(channelCount, dataCount, dataWidth, data);
}

/* -- crash breadcrumbs --------------------------------------------------- */

/* 2t0: boot 171 panicked after the lock screen and wrote NO panic report, so
 * the furthest point reached was unrecoverable. Rule 10 states the general
 * case: a boot that dies leaves NVRAM only, and rule 31's registry properties
 * - the right instrument for a LIVE boot - are gone with the machine, which is
 * exactly the boot worth reading.
 *
 * One key, last-wins, which is correct HERE: what is wanted is the FURTHEST
 * stage reached, and crumbStage is monotonic so a later call cannot walk it
 * back. (Rule 14's "a last-wins key cannot express a per-entry fact" still
 * holds - that is why the per-slot VBL facts stay in the registry.)
 *
 * Bounded BY CONSTRUCTION, because every NVRAM write is a flash commit: stage
 * transitions only, plus four fixed VBL milestones at exact equality so each
 * fires once. NEVER per-fire - at the measured refresh that would be ~144
 * commits a second, and rule 16 says the instrument is the suspect. The first
 * milestone is 100, not 1, deliberately: ~0.7 s in, so no synchronous commit
 * lands inside display bring-up, where 2ft already found such commits suspect.
 * `-rbgpufbnocrumb` disables the mechanism outright.
 *
 * Read it after a crash with:  nvram rbgpu-fbcrumb */
/* 2wp: consume rbgpu-fb for the next boot, by OVERWRITE on the syncing path -
 * the same mechanism 2wj measured and for the same reason: removeProperty
 * reaches no sync, setProperty does. The arming check already treats a literal
 * "0" as not armed, for both OSData and OSString, so nothing else changes.
 *
 * This is deliberately NOT a rescue of the running boot. The panel is already
 * ours and cannot be handed back; what it buys is that the NEXT power cycle
 * comes up on IONDRVFramebuffer with a usable desktop instead of repeating
 * the frozen screen. Rule 37 asked what recovery looks like when a feature
 * half-works; this is that answer for the case where the accelerator is
 * missing underneath us. */
void RBAMDFramebuffer::disarmFB(void)
{
    if (selfDisarmed || !nvram) {
        return;
    }
    selfDisarmed = true;
    /* 2xw: this used to write `rbgpu-fb=0`, which is now the USER's durable
     * disarm - and a self-disarm that sets it would stay set until they
     * noticed and cleared it. So it sets the sentinel latch instead: the next
     * boot comes up stock, the plugin clears the latch, and the boot after
     * that is armed again. One bad boot costs one stock boot, and the key the
     * user owns is never written by us. */
    if (auto one = OSString::withCString("1")) {
        nvram->setProperty("rbgpu-safe", one);
        one->release();
    }
    if (auto sync = OSString::withCString("now")) {
        nvram->setProperty("IONVRAM-FORCESYNCNOW-PROPERTY", sync);
        sync->release();
    }
}

void RBAMDFramebuffer::crumb(uint32_t stage)
{
    if (!crumbOn || !nvram) {
        return;
    }
    if (stage > crumbStage) {
        crumbStage = stage;
    }
    char buf[256];
    /* 2t0 addendum: the nonce comes FIRST because it decides whether anything
     * after it is this boot's. rbgpu-fbcrumb persists in NVRAM, so without it
     * a boot that died before stage 1 leaves the PREVIOUS boot's crumb reading
     * exactly like this one's - rule 11, and rule 8's whole point. Compare b=
     * against `nvram rbgpu-boot` before believing a single other field. */
    /* 2wi: the HUBP fields ride along, because boot 195 proved the display
     * works and left NO capture - the box could not be driven well enough to
     * run plugin_check.sh, so every RBFBHubp* value was unmeasured. Rule 10:
     * a boot that cannot be captured leaves NVRAM only, so what must survive
     * has to be here. h= is the refusal code (0 = set up), w= the write
     * count, and k= 1 iff the pair read back exactly what we last wrote,
     * which is the whole content of "the store reached the chip". */
    /* 2wl: p= is the fullest 1000-fire (~7 s) window the boot ever saw and
     * n= the transactions whose flag word had bit 0x1 clear; K=/D= are the
     * retire kick and its delivery. Boot 197 ran for twenty minutes with the
     * retire build installed and left NOTHING readable, because the crumb was
     * written at four fixed fire counts and the last of them is 70 seconds in
     * - so this one is REWRITTEN every 10000 fires and s= carries which
     * sample it is. The last crumb in NVRAM is then the whole boot. */
    /* 2x1: E= and A= are the two numbers this experiment has owed for three
     * boots, and they were registry-only - which is unreadable in exactly the
     * boot that matters, because at the measured 0.09 fps there is no desktop
     * to run ioreg from (rule 10, and the reason 2wl made the crumb periodic).
     * E= is retireEnables, the force-enables WE performed; A= is the summed
     * fire count of every 'vbl ' slot we did NOT register, i.e. how often
     * Apple's own handler actually ran. Together with t= they decide it four
     * ways without a capture - see the handoff. */
    snprintf(buf, sizeof(buf),
             "b=%s s=%u f=%llu e=%u r=%u h=%u w=%llu k=%u t=%llu "
             "p=%llu n=%llu K=%llu D=%llu E=%llu A=%llu R=%llu G=%llu",
             crumbBoot, crumbStage, (unsigned long long)vblFires, vblEnables,
             vblRunning ? 1u : 0u,
             hubpReady ? 0u : (hubpArmed ? (hubpRefuse ? hubpRefuse : 0xffu) : 0xfeu),
             (unsigned long long)hubpWrites,
             (hubpWrites && hubpReadback == hubpLast) ? 1u : 0u,
             (unsigned long long)amtrSets,
             (unsigned long long)amtrPeak, (unsigned long long)amtrNoFlip,
             (unsigned long long)retireKicks,
             (unsigned long long)retireDelivered,
             (unsigned long long)retireEnables,
             (unsigned long long)foreignFires(),
             (unsigned long long)pollReads,
             (unsigned long long)pollChanges);
    if (auto str = OSString::withCString(buf)) {
        /* 2sr measured that setPropertyWithGUIDAndName is the ONLY path that
         * reaches IODTNVRAM::syncInternal, so a plain setProperty here DOES
         * sync - unlike removeProperty, which is exactly why rbgpu-fb keeps
         * surviving its own removal. */
        nvram->setProperty("rbgpu-fbcrumb", str);
        str->release();
    }
}

void RBAMDFramebuffer::stop(IOService *provider)
{
    if (vblCall) {
        thread_call_cancel(vblCall);
        thread_call_free(vblCall);
        vblCall = nullptr;
    }
    if (vblKickCall) {
        thread_call_cancel(vblKickCall);
        thread_call_free(vblKickCall);
        vblKickCall = nullptr;
    }
    vblRunning = false;
    crumb(8);   /* stop() entered - a clean teardown, not a crash */
    crumbOn = false;            /* no writes once the object is going away */
    OSSafeReleaseNULL(nvram);
    OSSafeReleaseNULL(aperture);
    hubpTeardown();
    /* 2y6: both of these put OTG timing back and both need blSetup's BAR5
     * map, so they run BEFORE blTeardown() unmaps it. drrTeardown() was
     * written in 2y5 and never called from here at all - it only mattered on
     * an unload, and this kext panics on `kmutil unload` (2sq) so stop() has
     * effectively never run. It is wired up now because "never reached" and
     * "correct" are different claims, and the mode table makes the same
     * teardown genuinely load-bearing: a kext that stopped while the panel
     * was at 60 Hz would leave the raster at a refresh nothing advertises. */
    modeTeardown();
    drrTeardown();
    blTeardown();
    IOFramebuffer::stop(provider);
}

/* -- memory ------------------------------------------------------------- */

IODeviceMemory *RBAMDFramebuffer::getApertureRange(IOPixelAperture aperture_)
{
    if (aperture_ != kIOFBSystemAperture || !aperture) {
        return nullptr;
    }
    aperture->retain();     /* IOFramebuffer consumes a reference */
    return aperture;
}

IODeviceMemory *RBAMDFramebuffer::getVRAMRange(void)
{
    /* The visible framebuffer, not the whole 256 MB BAR. 2pr measured that a
     * wider range buys nothing, and `-rbgpufbvramrange` is on the must-stay-out
     * list for costing a large kernel mapping for no effect. */
    if (!aperture) {
        return nullptr;
    }
    aperture->retain();
    return aperture;
}

/* -- modes -------------------------------------------------------------- */

const char *RBAMDFramebuffer::getPixelFormats(void)
{
    return IO32BitDirectPixels "\0\0";
}

/* 2y6: two modes when the table is armed, one otherwise. The "otherwise" is
 * the configuration every boot since 2sl has been read against, and a refusal
 * anywhere in modeSetup() lands here rather than advertising a mode we cannot
 * deliver. */
IOItemCount RBAMDFramebuffer::getDisplayModeCount(void)
{
    return modeArmed ? (IOItemCount)modeCount : 1;
}

IOReturn RBAMDFramebuffer::getDisplayModes(IODisplayModeID *allDisplayModes)
{
    if (!allDisplayModes) {
        return kIOReturnBadArgument;
    }
    const IOItemCount n = getDisplayModeCount();
    for (IOItemCount i = 0; i < n; i++) {
        allDisplayModes[i] = kModeIDBase + (IODisplayModeID)i;
    }
    return kIOReturnSuccess;
}

/* One place that decides whether an id is ours, so the five query methods
 * cannot disagree about it. */
bool RBAMDFramebuffer::modeKnown(IODisplayModeID id) const
{
    return modeIndexOf(id) >= 0;
}

IOReturn RBAMDFramebuffer::getInformationForDisplayMode(IODisplayModeID displayMode,
                                                        IODisplayModeInformation *info)
{
    if (!modeKnown(displayMode) || !info) {
        return kIOReturnBadArgument;
    }
    bzero(info, sizeof(*info));
    info->nominalWidth  = kWidth;
    info->nominalHeight = kHeight;
    /* The only field that differs between the two: same 1920x1080 active
     * raster, same depth, same everything else, because a longer V_TOTAL is a
     * longer vertical blank and nothing more (2y5). */
    info->refreshRate   = modeRate1616(displayMode);
    info->maxDepthIndex = 0;
    info->flags         = kDisplayModeValidFlag | kDisplayModeSafeFlag;
    /* 143.83 Hz stays the default and the safe mode. 60 Hz is offered, never
     * imposed: it is ours, it is three milestones younger, and a boot that
     * comes up in it would be a boot where nothing has been measured. */
    if (displayMode == kModeIDFast) {
        info->flags |= kDisplayModeDefaultFlag;
    }
    return kIOReturnSuccess;
}

UInt64 RBAMDFramebuffer::getPixelFormatsForDisplayMode(IODisplayModeID, IOIndex)
{
    /* Documented as returning 0 by every modern IOFramebuffer subclass; the
     * real answer comes from getPixelInformation. */
    return 0;
}

IOReturn RBAMDFramebuffer::getPixelInformation(IODisplayModeID displayMode, IOIndex depth,
                                               IOPixelAperture aperture_,
                                               IOPixelInformation *info)
{
    if (!modeKnown(displayMode) || depth != 0 || aperture_ != kIOFBSystemAperture || !info) {
        return kIOReturnBadArgument;
    }
    bzero(info, sizeof(*info));
    info->bytesPerRow      = kBytesPerRow;
    info->bitsPerPixel     = 32;
    info->pixelType        = kIORGBDirectPixels;
    info->componentCount   = 3;
    info->bitsPerComponent = 8;
    info->componentMasks[0] = 0x00ff0000;
    info->componentMasks[1] = 0x0000ff00;
    info->componentMasks[2] = 0x000000ff;
    info->activeWidth      = kWidth;
    info->activeHeight     = kHeight;
    strlcpy(info->pixelFormat, IO32BitDirectPixels, sizeof(info->pixelFormat));
    return kIOReturnSuccess;
}

IOReturn RBAMDFramebuffer::getCurrentDisplayMode(IODisplayModeID *displayMode, IOIndex *depth)
{
    if (displayMode) {
        *displayMode = modeArmed ? modeCur : kModeIDFast;
    }
    if (depth) {
        *depth = 0;
    }
    return kIOReturnSuccess;
}

IOReturn RBAMDFramebuffer::setDisplayMode(IODisplayModeID displayMode, IOIndex depth)
{
    if (depth != 0 || !modeKnown(displayMode)) {
        LOG("setDisplayMode(%d, %d) refused; depth 0 and modes %d..%d",
            displayMode, (int)depth, kModeIDBase,
            kModeIDBase + (modeArmed ? (int)modeCount : 1) - 1);
        return kIOReturnUnsupported;
    }

    /* Unarmed: accept the one mode the firmware already set and write NOTHING
     * to the display engine. This is the pre-2y6 behaviour verbatim. */
    if (!modeArmed) {
        return kIOReturnSuccess;
    }

    modeLastAsked = (uint32_t)displayMode;
    if (displayMode == modeCur) {
        /* Not a no-op worth skipping silently: the compositor asks for the
         * current mode on wake and on every display reconfigure, and counting
         * it separately from a real switch is what keeps modeSwitches
         * meaningful (rule 33 - an index is not a count). */
        modeApplies++;
        return kIOReturnSuccess;
    }

    if (!modeApply(displayMode)) {
        LOG("setDisplayMode(%d) failed, refuse %u; staying at %d",
            displayMode, modeRefuse, modeCur);
        return kIOReturnUnsupported;
    }
    return kIOReturnSuccess;
}

IOReturn RBAMDFramebuffer::getStartupDisplayMode(IODisplayModeID *displayMode, IOIndex *depth)
{
    /* The mode the machine BOOTS in, which is the firmware's 143.83 Hz and not
     * whatever is current - those are different questions and forwarding to
     * getCurrentDisplayMode answered the wrong one as soon as a second mode
     * existed. */
    return getStartupDisplayMode_fast(displayMode, depth);
}

IOReturn RBAMDFramebuffer::getStartupDisplayMode_fast(IODisplayModeID *displayMode,
                                                      IOIndex *depth)
{
    if (displayMode) { *displayMode = kModeIDFast; }
    if (depth)       { *depth = 0; }
    return kIOReturnSuccess;
}

IOReturn RBAMDFramebuffer::setStartupDisplayMode(IODisplayModeID displayMode, IOIndex depth)
{
    /* Accepted and NOT persisted. There is nowhere to persist it to - the
     * firmware lights the panel before this kext exists - and a startup mode
     * that silently did not take would be worse than one that was never
     * offered. The boot always comes up at 143.83 Hz; the compositor can
     * switch once the desktop is up. */
    if (depth != 0 || !modeKnown(displayMode)) {
        return kIOReturnUnsupported;
    }
    return kIOReturnSuccess;
}

/* -- controller and connection ------------------------------------------ */

IOReturn RBAMDFramebuffer::enableController(void)
{
    /* Nothing to enable: OTG0 is already running (OTG0_CONTROL 0x00011201,
     * DMCUB 4.0.15.47, measured) and we never program it. */
    return kIOReturnSuccess;
}

bool RBAMDFramebuffer::isConsoleDevice(void)
{
    /* True only when we displaced the boot framebuffer; then we own the GOP
     * framebuffer the kernel console is already drawing into. Attached beside
     * it, IONDRVFramebuffer is the console and saying otherwise could take the
     * screen. */
    return ownsPanel();
}

IOItemCount RBAMDFramebuffer::getConnectionCount(void)
{
    /* One connection - the internal panel - but only when we are the driver
     * for it. Beside the real framebuffer we report none, so no second display
     * is created for a panel we are not driving. */
    return ownsPanel() ? 1 : 0;
}

IOReturn RBAMDFramebuffer::getAttributeForConnection(IOIndex connectIndex,
                                                     IOSelect attribute,
                                                     uintptr_t *value)
{
    if (connectIndex != 0) {
        return kIOReturnBadArgument;
    }
    switch (attribute) {
    case kConnectionEnable:
    case kConnectionCheckEnable:
        if (value) {
            *value = 1;             /* the panel is lit */
        }
        return kIOReturnSuccess;
    case kConnectionFlags:
        /* 2xy: kIOConnectionBuiltIn is the first of the two things
         * IOBacklightDisplay::probe requires before AppleBacklightDisplay can
         * match this connection and System Settings can draw a brightness
         * slider. The reasoning, and why it is armed separately from the
         * backlight WRITE, is on getAppleSense in
         * RBAMDFramebufferBacklight.cpp. Counted, not just answered: a probe
         * that never asked and a probe that asked and rejected us are
         * otherwise indistinguishable (rule 14). */
        if (value) {
            *value = blBuiltIn ? (uintptr_t)kIOConnectionBuiltIn : 0;
        }
        if (blBuiltIn) {
            blFlagsGets++;
        }
        return kIOReturnSuccess;
    case kConnectionSupportsAppleSense:
        /* 2xy: this says whether getAppleSense is worth calling, and with the
         * match armed it is - we answer it for connection 0. With the match
         * disarmed we decline, which is 2xs' behaviour unchanged. */
        if (blBuiltIn) {
            if (value) {
                *value = 1;
            }
            return kIOReturnSuccess;
        }
        return kIOReturnUnsupported;
    case kConnectionSupportsHLDDCSense:
        /* We have no DDC path to the internal panel, so decline rather than
         * claim a sense we cannot perform. This one stays declined in every
         * configuration: there is no I2C to the eDP panel to answer with. */
        return kIOReturnUnsupported;

    /* 2xs: the backlight parameter. IOFramebufferParameterHandler::
     * displayModeChange (IOGraphicsFamily 0x14fc8ece) asks 'pcnt' for a
     * count, allocates count * 8 bytes, asks 'parm' to fill it with 8-byte
     * entries whose low dword is a FourCC, and then asks us for each FourCC
     * with a three-word buffer { value, min, max }. All three steps are
     * answered here; the base already answers the first two for its own four
     * parameters ('rgsc', 'ggsc', 'bgsc', 'vblm' - visible in our live
     * AppleDisplay's IODisplayParameters), so we APPEND rather than replace. */
    case kConnectionDisplayParameterCount: {
        const IOReturn r =
            IOFramebuffer::getAttributeForConnection(connectIndex, attribute, value);
        if (r == kIOReturnSuccess && value && blReady) {
            *value += 1;
        }
        return r;
    }
    case kConnectionDisplayParameters: {
        if (!value) {
            return IOFramebuffer::getAttributeForConnection(connectIndex,
                                                            attribute, value);
        }
        /* Ask the base how many entries IT will write, so our one goes after
         * the last of them. The caller sized the buffer from the count we
         * returned above, which is this number plus one, so index [n] is in
         * bounds exactly when the base agrees with itself between the two
         * calls - and if it does not, we append nothing rather than writing
         * past the end. */
        uintptr_t n = 0;
        const IOReturn rc =
            IOFramebuffer::getAttributeForConnection(connectIndex,
                                                     kConnectionDisplayParameterCount,
                                                     &n);
        const IOReturn r =
            IOFramebuffer::getAttributeForConnection(connectIndex, attribute, value);
        if (r == kIOReturnSuccess && rc == kIOReturnSuccess && blReady &&
            n > 0 && n < RB_BL_PARM_MAX) {
            value[n] = RB_BL_ATTR_BRIGHTNESS;
            blParmAdds++;
        }
        return r;
    }
    case RB_BL_ATTR_BRIGHTNESS:
        /* { value, min, max }, the layout displayModeChange reads at
         * 0x14fc92fa: -0xb8 is a lower bound it raises the value to and
         * -0xb0 an upper bound it lowers it to. The minimum is our floor,
         * not zero, so the UI cannot ask for a dark panel (rule 12). */
        if (!blReady) {
            return kIOReturnUnsupported;
        }
        if (value) {
            value[0] = blCurrentLevel();
            value[1] = blFloor;
            value[2] = RB_BL_LEVEL_FULL;
        }
        return kIOReturnSuccess;

    default:
        return IOFramebuffer::getAttributeForConnection(connectIndex, attribute, value);
    }
}

IOReturn RBAMDFramebuffer::setAttributeForConnection(IOIndex connectIndex,
                                                     IOSelect attribute,
                                                     uintptr_t value)
{
    if (connectIndex != 0) {
        return kIOReturnBadArgument;
    }
    switch (attribute) {
    case kConnectionPower:
    case kConnectionProbe:
        /* Acknowledged; the firmware's modeset is the only one there is. */
        return kIOReturnSuccess;
    /* 2xs: the brightness write. 'brig' is not a constant anybody chose -
     * IOFramebufferParameterHandler::doIntegerSet byte-swaps the first four
     * characters of the parameter NAME into the attribute it sends, and
     * kIODisplayBrightnessKey is the string "brightness". */
    case RB_BL_ATTR_BRIGHTNESS:
        if (!blReady) {
            return kIOReturnUnsupported;
        }
        return blSetLevel((uint32_t)value) ? kIOReturnSuccess
                                           : kIOReturnNotReady;
    default:
        return IOFramebuffer::setAttributeForConnection(connectIndex, attribute, value);
    }
}

/* 2wc: 'amtr' - the attribute Apple's accelerator uses to ask a framebuffer
 * whether it can take display-pipe transactions, and then to hand one over.
 * Read end to end, no boot:
 *
 *   IOAccelDisplayPipeTransaction2::set_transaction_args (0x14e5ea68) ends in
 *       r = pipe->vt[0x8a0](txn)           call 0x8a0 -> dump 0x8b0 (rule 7)
 *                                          = IOAccelDisplayPipe::validateTransaction
 *       if (r) { log "%s returning error 0x%x for transaction(%d)"; return r; }
 *   and that is the ONLY site in the function that can return an arbitrary
 *   code - the other two returns are the literals 0xe00002c2 and 0xe00002bd.
 *   The base validateTransaction is `xorl %eax,%eax; ret`; ours is
 *   AMDRadeonX6000_AMDAccelDisplayPipe::validateTransaction (0xbe49a9e), a
 *   kdebug-signposted wrapper (0x85ad2009/0x85ad200a) around
 *       AMDRadeonX6000_AMDAccelDisplayPipe::executeTransaction(txn, false)
 *   (0xbe49b54), whose FIRST test is
 *       ok = pipe->0x330 /-* the AMDDCNDisplay *-/ ->vt[0x190](getFramebufferIndex())
 *       if (!txn || !ok) return 0xe00002c2 + 5*(txn != NULL)
 *   and `0xe00002c2 + 5` is 0xe00002c7 = kIOReturnUnsupported. The assembler
 *   writes it as `leal (%rax,%rax,4), %r12d; addl $0xe00002c2, %r12d`, so a
 *   grep for the constant finds the five literal sites and MISSES this one -
 *   rule 26, and it is the sixth.
 *
 * Slot 0x190 (dump 0x1a0 in both __ZTV27AMDRadeonX6000_AMDHWDisplay and
 * __ZTV28AMDRadeonX6000_AMDDCNDisplay; 0x258 = isDisplayControlEnabled is the
 * control that fixes the table's alignment) is an unnamed accessor at
 * 0xbec02ce:  return ((uint8_t *)this->0x40)[idx * 0xd0 + 1] & 1;
 * i.e. bit 8 of the per-display dword at the head of a 0xd0-stride array that
 * AMDHWDisplay::init IOMallocs as `count * 0xd0`. Its SOLE writer is
 * AMDRadeonX6000_AMDHWDisplay::getDisplayInfo (0xbe8700e), the same function
 * 2w9 traced for ATY,fb_offset, forty bytes further on:
 *
 *   0xbe87165  r = fb->vt[0xa30]('amtr', &out)     call 0xa30 -> dump 0xa40
 *                                                 = IOFramebuffer::getAttribute
 *   0xbe8717e  if (r == 0 || r == kIOReturnNotReady) dstate |=  0x100
 *   0xbe8718b  else                                  dstate &= ~0x100
 *
 * ('wsaa' = 0x77736161 is the control: the line above does the same thing for
 * kIOWindowServerActiveAttribute and bit 0x80.) So the bit is ours, and ours
 * alone, and IOFramebuffer's base getAttribute answers an unknown selector
 * with kIOReturnUnsupported - which is exactly 122 refused transactions.
 *
 * Why this and not the gamma ceiling: with dirtyBits 0x3c1 and options 0x801
 * (boot 191's measured inputs) the other five kIOReturnUnsupported sites are
 * each gated on a dirty bit. Three are clear (0x4, 0x20, 0x400), one needs
 * dirtyBits == 0x100 exactly, and the live one - bit 0x80 - returns it only
 * when `getPipePostGammaTable()->getTableCount() > 0x100`. Boot 190's capture
 * counts the LLUT CoreDisplay actually submits: 71 dumps of 256 entries and 2
 * of 1024. A ceiling that 2 submissions in 73 cross cannot produce 122
 * failures out of 122; an unconditional first-line gate can, and does.
 *
 * What AMDFramebuffer does with the two selectors, so the shape of the answer
 * is copied rather than guessed:
 *
 *   getAttribute('amtr', out)            0x2a6faa
 *       if (!(ctrl->0x31332 >> 9 & 1)) return kIOReturnUnsupported;
 *       if (getOutstandingDisplayPipeTransactions()) return kIOReturnNotReady;
 *       return kIOReturnSuccess;         // *out is never written
 *   setAttribute('amtr', value)          0x2a65a4
 *       if (!value) return kIOReturnBadArgument;
 *       if (!tryAcquireIOPGuard()) return kIOReturnNotReady;
 *       r = handleDisplayPipeTransaction((_AMD_PIPE_TRANSACTION *)value);
 *       releaseIOPGuardNotify();
 *       return r;
 *
 * The `value` is `&pipe->0x410`, the transaction struct executeTransaction has
 * just finished filling: the flag word at +0x00 (it ORs 0x1, 0x2, 0x4, 0x8,
 * 0x10, 0x20, 0x40, 0x100 and 0x201 into it), the plane resource at +0x10, the
 * destination rect at +0x18/+0x20, the colour matrix at +0x30, the gamma
 * tables from +0x220 on. We do not program DCN yet, so this answers success
 * and SNAPSHOTS the first struct we are given - that snapshot is the whole
 * point of the boot, because it is the first time this project has seen a
 * cooked transaction, and the surface address in it is what the scanout
 * eventually has to be pointed at.
 *
 * Nothing in either handler allocates. They run on the accelerator's thread
 * with its locks held; the VBL thread_call publishes the counters (rule 31).
 */
static const IOSelect kRBAttrAMDTransaction = 0x616d7472; /* 'amtr' */

IOReturn RBAMDFramebuffer::getAttribute(IOSelect attribute, uintptr_t *value)
{
    switch (attribute) {
    case kIOHardwareCursorAttribute:
        /* No hardware cursor: IOFramebuffer then composites the cursor into
         * the framebuffer itself, which is what happens today. */
        if (value) {
            *value = 0;
        }
        return kIOReturnSuccess;
    case kRBAttrAMDTransaction: {
        /* Braced: 2A0 declares a local here, and a declaration with an
         * initialiser in an unbraced case is a jump across an initialisation
         * once another label follows it. */
        if (!amtrArmed) {
            break;
        }
        amtrGets++;
        /* 2A0: time the GET. This measures only our own decision, NOT the
         * wait it causes: hubpHoldNotReady() reads two words of MMIO and
         * returns, and what blocks is Apple's isTransactionComplete calling
         * back (2x6). So the hold's cost lands in `gap` and therefore in
         * RBFBFtUpstreamNs even though it is ours - read RBFBAmtrHeld against
         * RBFBAmtrGets to attribute that share, because a budget that labelled
         * it upstream without saying so would be exactly the kind of
         * confidently mislabelled number rule 17 is about. */
        const uint64_t ftG0 = ftOn ? mach_absolute_time() : 0;
        /* 2xn: answer the way Apple's AMDFramebuffer does. The decision is
         * rb_hubp_hold_notready() in src/common/rb_hubp.h, which the hosttest
         * runs first and which is BOUNDED by hubpFlipDeadline - answering
         * NotReady makes Apple's isTransactionComplete WAIT (2x6), so an
         * unbounded hold would wedge the display. Reads two words of MMIO and
         * nothing else: no allocation, no log, no block (rule 31). */
        {
            const bool hold = hubpHoldNotReady();
            if (ftOn) {
                const uint64_t ftG1 = mach_absolute_time();
                ftFile(&ftGet, ftG0, ftG1, true);
                /* 2A1: the gap SINCE THE LAST GET, filed into whichever
                 * accumulator the LAST GET's answer selects - so the bucket is
                 * chosen by what we told Apple, and the quantity measured is
                 * how long Apple then took to ask again. Bounded, because an
                 * idle desktop stops asking entirely and one idle second in
                 * either bucket would read as the retry cost. */
                if (ftPrevGetExit != 0) {
                    ftFile(ftPrevGetHeld ? &ftRetry : &ftPoll,
                           ftPrevGetExit, ftG0, true);
                }
                ftPrevGetExit = ftG1;
                ftPrevGetHeld = hold;
            }
            if (hold) {
                return kIOReturnNotReady;
            }
        }
        /* AMDFramebuffer never writes *out on this selector, and the one
         * consumer (getDisplayInfo) presets its local and ignores it. */
        return kIOReturnSuccess;
    }
    default:
        break;
    }
    return IOFramebuffer::getAttribute(attribute, value);
}

IOReturn RBAMDFramebuffer::setAttribute(IOSelect attribute, uintptr_t value)
{
    if (attribute == kRBAttrAMDTransaction && amtrArmed) {
        amtrSets++;
        /* 2A0: the transaction clock. t0 is taken before anything that can
         * return early, and the gap is filed from the PREVIOUS transaction's
         * exit, so mean(gap) + mean(work) reconstructs the interval between
         * transactions - the quantity whose reciprocal is the ~73 sets/s this
         * project has been unable to move seven times.
         *
         * The null transaction below returns without filing either piece. It
         * is not a frame, and including it would put a near-zero interval
         * into a mean of ~13.7 ms ones. */
        const uint64_t ftT0 = ftOn ? mach_absolute_time() : 0;
        if (value == 0) {
            /* AMDFramebuffer's own answer to a null transaction. */
            return kIOReturnBadArgument;
        }
        /* 2wl: the rate instrument, before anything that can return early.
         * One increment and one bit test - rule 31 still holds. */
        if (value >= 0xffffff8000000000ULL) {
            amtrBucket++;
            if ((((const uint64_t *)value)[0] & 0x1) == 0) {
                amtrNoFlip++;
            }
        }
        if (!amtrSnapped) {
            /* Rule 19: the right floor for a kalloc'd object is the KERNEL
             * base, not the kext one, and a refusal publishes what it refused.
             * This pointer is &pipe->0x410 inside an IOAccelDisplayPipe, whose
             * own fields run past 0x3b68, so 0x80 bytes from it are inside the
             * object by a wide margin. */
            if (value >= 0xffffff8000000000ULL) {
                memcpy(amtrSnap, (const void *)value, sizeof(amtrSnap));
                amtrPtr = (uint64_t)value;
                amtrSnapped = true;
            } else {
                amtrRefused = (uint64_t)value;
            }
        }
        /* 2wf: follow txn->0x10 to the _AMD_PIPE_FLIP and take the whole
         * 0x220 bytes. Separate flag from amtrSnapped so the two failures are
         * distinguishable: a header we could read with a plane state we could
         * not is a different fact from never being called. The flag word's bit
         * 0x1 is what makes txn->0x10 valid, and boot 193 measured it SET
         * (0x75), so a null here would itself be a result. */
        if (!amtrFlipSnapped && value >= 0xffffff8000000000ULL) {
            uint64_t ps = ((const uint64_t *)value)[2];   /* txn->0x10 */
            if (ps >= 0xffffff8000000000ULL) {
                memcpy(amtrFlipSnap, (const void *)ps, sizeof(amtrFlipSnap));
                amtrFlipPtr = ps;
                amtrFlipSnapped = true;
            } else {
                amtrFlipRefused = ps;
            }
        }
        /* 2wh: and the write. The address is _AMD_PIPE_FLIP +0x08/+0x0c, a
         * low/high pair in exactly the layout HUBP's register pair wants
         * (2wg). Taken fresh from EVERY transaction, not from the snapshot:
         * the snapshot is the first one only, and a swap chain moves the
         * surface each flip. hubpProgram() is a no-op unless armed and set
         * up, and it refuses anything outside the measured carve-out. */
        /* 2wi: transaction retirement. Boot 195 showed the display working at
         * the TRANSACTION rate - 1 frame per ~10.8 s, which boots 193/194 had
         * already measured as 0.093 sets/s before this code existed. The
         * prime suspect is that Apple's accelerator never learns a vblank
         * happened: it registers a 'vbl ' handler, disables its own slot
         * immediately, and 192/193/194 all read RBFBVBL1Fires 0 WITH
         * transactions in flight. 2s5 measured what should re-arm it
         * (transaction_queue_gated -> enableTransactionInterrupt ->
         * enableVBLInterrupt), and it is evidently not running here.
         *
         * So re-enable the slots we did not register, once a transaction has
         * actually arrived - which is exactly the condition Apple's own
         * enableTransactionInterrupt waits for. Field writes only: no
         * allocation, no logging, no blocking (rule 31). Counted so the
         * boot says whether it ever fired, and gated on its own boot-arg so
         * this is a separable experiment and not a silent change to 2wh. */
        /* 2x1: and DELIVER one immediately, instead of waiting for the next
         * periodic tick. The force-enable above has been armed for three
         * boots and never measured; the one race it leaves is real. Our tick
         * is 7 ms, so between transactions at the measured 0.09/s it fires
         * ~1500 times and would catch the window easily - UNLESS Apple
         * re-disables its slot inside those 7 ms, which is precisely what
         * every RBFBVBL1Fires reading of 0 is consistent with. Delivering on
         * our own thread_call closes it: the enable and the delivery are then
         * not separated by anything Apple can observe or undo.
         *
         * This is the same mechanism and hazard class as 2wk's kick on the
         * enable transition - thread_call_enter plus field writes, no
         * allocation, no logging, no blocking on the accelerator's thread
         * (rule 31) - and kickVBL re-checks `enabled` itself, so a window
         * that has already closed costs one scheduled call and is counted as
         * such. Kept under -rbgpufbretire rather than a third boot-arg: that
         * arg already means "make Apple learn a vblank happened", and adding
         * an arg per refinement multiplies boots without separating
         * hypotheses. */
        if (retireArmed) {
            bool forced = false;
            for (unsigned i = 0; i < kMaxInterrupts; i++) {
                if (interrupts[i].used && interrupts[i].target != this &&
                    !interrupts[i].enabled) {
                    interrupts[i].enabled = true;
                    retireEnables++;
                    forced = true;
                }
            }
            if (forced && vblKickCall) {
                retireKicks++;
                thread_call_enter(vblKickCall);
            }
        }
        if (hubpReady && value >= 0xffffff8000000000ULL) {
            /* Bit 0x1 of the flag word is what makes txn->0x10 a plane state
             * this transaction actually updated - 2wf read the consumer doing
             * exactly this test before it touches the pointer, and boot 193
             * measured the bit SET (flags 0x75). Without it we would write a
             * stale address on a transaction that only changed, say, the
             * colour matrix. */
            const uint64_t flags = ((const uint64_t *)value)[0];
            const uint64_t ps    = ((const uint64_t *)value)[2];  /* txn->0x10 */
            if (rb_hubp_should_write(flags, hubpAll ? 1 : 0) &&
                ps >= 0xffffff8000000000ULL) {
                hubpProgram(((const uint64_t *)ps)[1]);           /* flip +0x08 */
            }
        }
        /* 2A0: close the budget. The gap is filed first and BOUNDED, because
         * an idle desktop makes it arbitrarily large (rb_frametime.h explains
         * why that one has to be discarded rather than averaged); the work is
         * unbounded, since nothing about an idle desktop lengthens the inside
         * of this function. ftPrevExit is stamped last so the next gap starts
         * where this transaction really ended, past hubpProgram. */
        if (ftOn) {
            const uint64_t ftT1 = mach_absolute_time();
            if (ftPrevExit != 0) {
                ftFile(&ftGap, ftPrevExit, ftT0, true);
            }
            ftFile(&ftWork, ftT0, ftT1, false);
            ftPrevExit = ftT1;
        }
        return kIOReturnSuccess;
    }
    return IOFramebuffer::setAttribute(attribute, value);
}

/* 2zd: find the plugin's peer ONCE, off the vblank.
 *
 * copyMatchingService and NOT waitForMatchingService: the latter blocks until
 * a match appears, and a block inside IOFramebuffer::open would hang the boot
 * on exactly the configuration this lookup exists to tolerate - no plugin.
 * copyMatchingService asks once and returns null if nothing is published. The
 * plugin lives in BootKernelExtensions.kc and starts long before an
 * auxiliary-collection kext loads, so if it is going to be there at all it is
 * there now. Even so this runs off the vblank, because matching takes locks
 * and rule 31 forbids that on the vblank thread_call. A null peer is
 * not an error: it is the configuration where the plugin is absent, disarmed
 * or older than this build, and rule 38 says the two halves must check each
 * other rather than assume. The timer keeps retiring in that case and the
 * only cost is 2zc's beat, which is the behaviour of every boot before this
 * one.
 *
 * The OSSymbol is created here too, because creating it per vblank would be
 * an allocation at 144 Hz. */
void RBAMDFramebuffer::vblRetSetup(void)
{
    if (vblRetPeer || vblRetLookups) {
        return;
    }
    vblRetLookups++;
    if (!argOn("-rbgpuretirevbl")) {
        return;
    }
    OSDictionary *match = IOService::serviceMatching("RembrandtGPUPeer");
    if (!match) {
        return;
    }
    vblRetPeer = IOService::copyMatchingService(match);
    match->release();
    if (!vblRetPeer) {
        LOG("2zd: -rbgpuretirevbl is set but RembrandtGPUPeer was not found - "
            "the timer keeps the job");
        return;
    }
    vblRetSym = OSSymbol::withCString("RBGPURetireNow");
    if (!vblRetSym) {
        vblRetPeer->release();
        vblRetPeer = nullptr;
        return;
    }
    vblRetArmed = 1;
    LOG("2zd: retiring frames from the vblank");
}

/* 2zd: offer this vblank to the retire path. Runs on the vblank thread_call:
 * one virtual call by name, nothing allocated, nothing logged, nothing locked.
 *
 * The answer is READ rather than assumed (rule 13): the plugin returns
 * kIOReturnNotReady when the offer did not drive a retire - the handover is
 * disarmed on its side, the scheduler object is not sane yet, or the
 * consecutive-refusal stop has fired - and each case is counted here so that
 * "the vblank is driving" is a measurement and not a configuration. */
void RBAMDFramebuffer::vblRetOffer(void)
{
    if (!vblRetArmed || !vblRetPeer || !vblRetSym) {
        return;
    }
    /* 2zf: ONLY AT A MODE THE VBLANK CAN CARRY. Retirement used to run at a
     * fixed 6944 us whatever the display did; driving it from here ties it to
     * the display rate, and boot 267 measured that as +24 % at native and
     * -42 % at 60 Hz with `fed` pinned at exactly half. Below native the timer
     * keeps the job - which is every boot's behaviour before 2zd, so this is
     * the status quo and not a new risk. Checked per offer rather than per
     * setup because the mode changes at runtime and without a reboot (2z1),
     * and counted so a boot can say which side of the gate it sat on. */
    if (!rb_retire_vbl_suits(vblPeriodNs ? vblPeriodNs : kVBLPeriodNsFast,
                             kVBLPeriodNsFast)) {
        vblRetModeSkips++;
        return;
    }
    uint32_t drove = 0;
    vblRetCalls++;
    const IOReturn r = vblRetPeer->callPlatformFunction(vblRetSym, false,
                                                        &drove, nullptr,
                                                        nullptr, nullptr);
    if (r == kIOReturnSuccess && drove) {
        vblRetDrove++;
    } else {
        vblRetRefused++;
    }
}
