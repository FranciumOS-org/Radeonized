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

    options->removeProperty("rbgpu-fb");
    if (auto sync = OSString::withCString("now")) {
        options->setProperty("IONVRAM-FORCESYNCNOW-PROPERTY", sync);
        sync->release();
    }
    /* This read-back proves the IN-MEMORY state only. 2sq measured the key
     * coming back after a reboot having read as absent here, and the kernel
     * path above explains how. So it is logged as what it is, and the boot-arg
     * is what the disarm actually rests on. */
    bool cleared = options->getProperty("rbgpu-fb") == nullptr;
    LOG("armed (-rbgpuownfb + rbgpu-fb); taking the panel for THIS BOOT. "
        "one-shot removal %s IN MEMORY - this does NOT prove it reached flash "
        "(2sq); the durable disarm is `sudo tools/efi.sh bootarg remove -rbgpuownfb`",
        cleared ? "accepted" : "FAILED, so the key is definitely still set");
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
    char buf[96];
    /* 2t0 addendum: the nonce comes FIRST because it decides whether anything
     * after it is this boot's. rbgpu-fbcrumb persists in NVRAM, so without it
     * a boot that died before stage 1 leaves the PREVIOUS boot's crumb reading
     * exactly like this one's - rule 11, and rule 8's whole point. Compare b=
     * against `nvram rbgpu-boot` before believing a single other field. */
    snprintf(buf, sizeof(buf), "b=%s s=%u f=%llu e=%u r=%u",
             crumbBoot, crumbStage, (unsigned long long)vblFires, vblEnables,
             vblRunning ? 1u : 0u);
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
    default:
        return IOFramebuffer::getAttribute(attribute, value);
    }
}

IOReturn RBAMDFramebuffer::setAttribute(IOSelect attribute, uintptr_t value)
{
    return IOFramebuffer::setAttribute(attribute, value);
}
