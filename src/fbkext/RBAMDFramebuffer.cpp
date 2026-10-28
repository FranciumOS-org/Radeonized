/* SPDX-License-Identifier: MIT */
/* RBAMDFramebuffer: see RBAMDFramebuffer.hpp. */
#include "RBAMDFramebuffer.hpp"

#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSString.h>
#include <kern/thread_call.h>
#include <pexpert/pexpert.h>

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
const IODisplayModeID kModeID       = 100;
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
 * CLAUDE.md rule 9 already states the answer, learned on other features and
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
bool armedOnce(void)
{
    char flag[16];
    if (!PE_parse_boot_argn("-rbgpuownfb", flag, sizeof(flag))) {
        LOG("-rbgpuownfb is not in boot-args; NOT arming whatever NVRAM says "
            "(rule 9: the feature needs the arg AND the key, so a key that "
            "failed to clear is inert). IONDRVFramebuffer keeps the panel.");
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
        LOG("/options not found after %u tries; NOT arming, so IONDRVFramebuffer "
            "matches as usual. rbgpu-fb is untouched - a plain reboot retries.",
            tries);
        return false;
    }

    bool armed = false;
    if (OSObject *val = options->getProperty("rbgpu-fb")) {
        /* `nvram rbgpu-fb=1` stores an OSData, so check the first byte rather
         * than expecting a string. Any value counts except a literal "0". */
        armed = true;
        if (auto data = OSDynamicCast(OSData, val)) {
            if (data->getLength() >= 1 &&
                *static_cast<const char *>(data->getBytesNoCopy()) == '0') {
                armed = false;
            }
        } else if (auto str = OSDynamicCast(OSString, val)) {
            if (str->isEqualTo("0")) {
                armed = false;
            }
        }
    }

    if (!armed) {
        LOG("-rbgpuownfb present but rbgpu-fb not set; staying out of the way "
            "(the desktop keeps IONDRVFramebuffer). Arm with: "
            "sudo nvram rbgpu-fb=1");
        options->release();
        return false;
    }

    /* 2wj: CONSUME BY OVERWRITE, NOT BY REMOVAL. This is the fix for 2sq, and
     * the measurement that makes it work has been in this file since 2sr:
     * `removeProperty` tail-jumps to `removePropertyWithGUIDAndName`, which
     * calls NO sync, while `setProperty` reaches
     * `setPropertyWithGUIDAndName` and DOES - which is why `rbgpu-fbcrumb`
     * has persisted reliably all along while this key kept surviving its own
     * deletion. So write a literal "0" instead of deleting, and the arming
     * check twenty lines above already treats "0" as NOT armed, for both
     * OSData and OSString.
     *
     * Boot 196 is what forced this: with the key never consuming, the
     * framebuffer took over on every power cycle, and at the 0.09 Hz
     * transaction rate 2wi measured there is no usable desktop from which to
     * edit boot-args - the user had to boot WINDOWS to disarm. A one-shot
     * that cannot consume itself is not a one-shot, and the feature could not
     * disarm itself on the machine it runs on.
     *
     * The boot-arg remains the durable disarm (rule 9, and it is still what
     * the log line says); this makes the one-shot do its own job as well. */
    if (auto zero = OSString::withCString("0")) {
        options->setProperty("rbgpu-fb", zero);
        zero->release();
    }
    if (auto sync = OSString::withCString("now")) {
        options->setProperty("IONVRAM-FORCESYNCNOW-PROPERTY", sync);
        sync->release();
    }
    /* Still an IN-MEMORY read-back: it proves the overwrite took in the
     * registry, not that flash accepted it. The difference from 2sq is that
     * the path taken is now the one measured to sync, so this reading is
     * worth something it was not worth before. */
    bool cleared = false;
    if (OSObject *v = options->getProperty("rbgpu-fb")) {
        if (auto d = OSDynamicCast(OSData, v)) {
            cleared = d->getLength() >= 1 &&
                      *static_cast<const char *>(d->getBytesNoCopy()) == '0';
        } else if (auto t = OSDynamicCast(OSString, v)) {
            cleared = t->isEqualTo("0");
        }
    }
    LOG("armed (-rbgpuownfb + rbgpu-fb); taking the panel for THIS BOOT. "
        "one-shot overwrite to \"0\" %s IN MEMORY via the path 2sr measured as "
        "syncing (2wj); the durable disarm is still "
        "`sudo tools/efi.sh bootarg remove -rbgpuownfb`",
        cleared ? "accepted" : "FAILED, so the key is definitely still armed");
    options->release();
    return true;
}

} /* namespace */

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

    /* 2wc: 'amtr'. Armed with -rbgpufbamtr; the flag is published
     * UNCONDITIONALLY a few lines below, because rule 15's cheapest
     * independent check that the new binary is the one running is a key the
     * new code publishes whatever the boot-args say. */
    amtrArmed = false;
    amtrSnapped = false;
    amtrGets = 0;
    amtrSets = 0;
    amtrPtr = 0;
    amtrRefused = 0;
    bzero(amtrSnap, sizeof(amtrSnap));
    amtrFlipSnapped = false;
    amtrFlipPtr = 0;
    amtrFlipRefused = 0;
    bzero(amtrFlipSnap, sizeof(amtrFlipSnap));
    {
        char amtrflag[16];
        amtrArmed = PE_parse_boot_argn("-rbgpufbamtr", amtrflag, sizeof(amtrflag));
    }

    /* 2wh: the HUBP write. Armed with -rbgpufbhubp, and it needs
     * -rbgpufbamtr too, because the address it writes arrives only through
     * setAttribute('amtr'). Rule 30: both names are whole tokens and neither
     * prefixes the other, so PE_parse_boot_argn cannot confuse them.
     * rbgpufbhubpmax=<N> bounds the writes for a deliberately conservative
     * first boot; 0 or absent means unlimited, which is what a live display
     * needs. */
    hubpArmed = false;
    hubpMax = 0;
    hubpRejectAddr = 0;
    hubpRejectWhy = 0;
    {
        char hubpflag[16];
        hubpArmed = PE_parse_boot_argn("-rbgpufbhubp", hubpflag, sizeof(hubpflag))
                    && amtrArmed;
        int maxn = 0;
        if (PE_parse_boot_argn("rbgpufbhubpmax", &maxn, sizeof(maxn)) &&
            maxn > 0 && maxn <= 1000000) {
            hubpMax = (uint64_t)maxn;
        }
    }
    /* 2wi: -rbgpufbretire. Deliberately NOT named -rbgpufbvbl-something:
     * rule 30 warns that a shell grep for an existing arg matches a longer
     * one, and `-rbgpufbvbl` is already in use. */
    retireArmed = false;
    retireEnables = 0;
    {
        char retflag[16];
        retireArmed = PE_parse_boot_argn("-rbgpufbretire", retflag, sizeof(retflag));
    }

    hubpSetup();

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
        char atyflag[16];
        bool atyOn = PE_parse_boot_argn("-rbgpufbaty", atyflag, sizeof(atyflag));
        setProperty("RBFBAtyArmed", atyOn);
        setProperty("RBFBAmtrArmed", amtrArmed);
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
void RBAMDFramebuffer::crumb(uint32_t stage)
{
    if (!crumbOn || !nvram) {
        return;
    }
    if (stage > crumbStage) {
        crumbStage = stage;
    }
    char buf[160];
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
    snprintf(buf, sizeof(buf), "b=%s s=%u f=%llu e=%u r=%u h=%u w=%llu k=%u t=%llu",
             crumbBoot, crumbStage, (unsigned long long)vblFires, vblEnables,
             vblRunning ? 1u : 0u,
             hubpReady ? 0u : (hubpArmed ? (hubpRefuse ? hubpRefuse : 0xffu) : 0xfeu),
             (unsigned long long)hubpWrites,
             (hubpWrites && hubpReadback == hubpLast) ? 1u : 0u,
             (unsigned long long)amtrSets);
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
    vblRunning = false;
    crumb(8);   /* stop() entered - a clean teardown, not a crash */
    crumbOn = false;            /* no writes once the object is going away */
    OSSafeReleaseNULL(nvram);
    OSSafeReleaseNULL(aperture);
    hubpTeardown();
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

IOItemCount RBAMDFramebuffer::getDisplayModeCount(void)
{
    return 1;
}

IOReturn RBAMDFramebuffer::getDisplayModes(IODisplayModeID *allDisplayModes)
{
    if (!allDisplayModes) {
        return kIOReturnBadArgument;
    }
    allDisplayModes[0] = kModeID;
    return kIOReturnSuccess;
}

IOReturn RBAMDFramebuffer::getInformationForDisplayMode(IODisplayModeID displayMode,
                                                        IODisplayModeInformation *info)
{
    if (displayMode != kModeID || !info) {
        return kIOReturnBadArgument;
    }
    bzero(info, sizeof(*info));
    info->nominalWidth  = kWidth;
    info->nominalHeight = kHeight;
    info->refreshRate   = kRefreshRate1616;
    info->maxDepthIndex = 0;
    info->flags         = kDisplayModeValidFlag | kDisplayModeSafeFlag |
                          kDisplayModeDefaultFlag;
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
    if (displayMode != kModeID || depth != 0 || aperture_ != kIOFBSystemAperture || !info) {
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
        *displayMode = kModeID;
    }
    if (depth) {
        *depth = 0;
    }
    return kIOReturnSuccess;
}

IOReturn RBAMDFramebuffer::setDisplayMode(IODisplayModeID displayMode, IOIndex depth)
{
    /* Accept the one mode the firmware already set and write NOTHING to the
     * display engine. Anything else is refused rather than silently ignored. */
    if (displayMode != kModeID || depth != 0) {
        LOG("setDisplayMode(%d, %d) refused; only mode %d depth 0 exists",
            displayMode, (int)depth, kModeID);
        return kIOReturnUnsupported;
    }
    return kIOReturnSuccess;
}

IOReturn RBAMDFramebuffer::getStartupDisplayMode(IODisplayModeID *displayMode, IOIndex *depth)
{
    return getCurrentDisplayMode(displayMode, depth);
}

IOReturn RBAMDFramebuffer::setStartupDisplayMode(IODisplayModeID displayMode, IOIndex depth)
{
    return setDisplayMode(displayMode, depth);
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
        if (value) {
            *value = 0;
        }
        return kIOReturnSuccess;
    case kConnectionSupportsAppleSense:
    case kConnectionSupportsHLDDCSense:
        /* We have no DDC path to the internal panel, so decline rather than
         * claim a sense we cannot perform. */
        return kIOReturnUnsupported;
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
    case kRBAttrAMDTransaction:
        if (!amtrArmed) {
            break;
        }
        amtrGets++;
        /* AMDFramebuffer never writes *out on this selector, and the one
         * consumer (getDisplayInfo) presets its local and ignores it. */
        return kIOReturnSuccess;
    default:
        break;
    }
    return IOFramebuffer::getAttribute(attribute, value);
}

IOReturn RBAMDFramebuffer::setAttribute(IOSelect attribute, uintptr_t value)
{
    if (attribute == kRBAttrAMDTransaction && amtrArmed) {
        amtrSets++;
        if (value == 0) {
            /* AMDFramebuffer's own answer to a null transaction. */
            return kIOReturnBadArgument;
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
        if (retireArmed) {
            for (unsigned i = 0; i < kMaxInterrupts; i++) {
                if (interrupts[i].used && interrupts[i].target != this &&
                    !interrupts[i].enabled) {
                    interrupts[i].enabled = true;
                    retireEnables++;
                }
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
            if ((flags & 0x1) && ps >= 0xffffff8000000000ULL) {
                hubpProgram(((const uint64_t *)ps)[1]);           /* flip +0x08 */
            }
        }
        return kIOReturnSuccess;
    }
    return IOFramebuffer::setAttribute(attribute, value);
}
