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
const UInt32 kRefreshRate1616       = 0x008fd53f;   /* 143.83 Hz */
const UInt32 kBytesPerRow           = 7680;
const UInt32 kApertureBytes         = 8294400;      /* IOFBMemorySize, live */

/* The vblank period that follows from the measured refresh: 1 / 143.908 Hz.
 * Stated in nanoseconds because that is what the delay takes, and derived
 * rather than typed so the two cannot drift. */
const uint64_t kVBLPeriodNs         = 1000000000ULL * 65536ULL / kRefreshRate1616;

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
 * removed with `tools/efi.sh bootarg remove -rbgpufb`, which edits a file and
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
    if (!PE_parse_boot_argn("-rbgpufb", flag, sizeof(flag))) {
        LOG("-rbgpufb is not in boot-args; NOT arming whatever NVRAM says "
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
        LOG("-rbgpufb present but rbgpu-fb not set; staying out of the way "
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
    LOG("armed (-rbgpufb + rbgpu-fb); taking the panel for THIS BOOT. "
        "one-shot removal %s IN MEMORY - this does NOT prove it reached flash "
        "(2sq); the durable disarm is `sudo tools/efi.sh bootarg remove -rbgpufb`",
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
    /* Refuse unless armed, and consume the one-shot before returning. A boot
     * that goes wrong therefore costs exactly one power cycle. */
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

    vblCall = thread_call_allocate(vblFired, this);
    if (!vblCall) {
        LOG("thread_call_allocate failed; the vblank will not be delivered");
    }

    if (!IOFramebuffer::start(provider)) {
        LOG("IOFramebuffer::start refused");
        return false;
    }

    /* The labels System Profiler reads. Values measured, not invented: the
     * 4096 MB is what the plugin publishes from the real carve-out size. */
    setProperty("VRAM,totalMB", 4096ULL, 32);
    setProperty("model", "AMD Radeon 680M");
    LOG("started; %s", ownsPanel()
        ? "OWNS THE PANEL (displaced IONDRVFramebuffer): console yes, 1 connection"
        : "attached BESIDE the boot framebuffer: console no, 0 connections - "
          "this configuration measures start(), it does not drive the display");
    return true;
}

void RBAMDFramebuffer::stop(IOService *provider)
{
    if (vblCall) {
        thread_call_cancel(vblCall);
        thread_call_free(vblCall);
        vblCall = nullptr;
    }
    vblRunning = false;
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

/* -- the vblank ---------------------------------------------------------- */

/* Why a thread_call rather than a real interrupt: the display engine's own
 * vblank IRQ is wired to the AMD driver's interrupt controller, which we do not
 * own, and 2s4 measured that what the display pipe actually needs is a 'vbl '
 * callback at the panel's rate - not a particular source for it. The comparable
 * working driver drives its VBL handlers from a thread_call for the same reason
 * (2sj). The rate is the measured refresh, so the pacing is right even though
 * the phase is not locked to scanout. */

bool RBAMDFramebuffer::anyVBLEnabled(void)
{
    for (unsigned i = 0; i < kMaxInterrupts; i++) {
        if (interrupts[i].used && interrupts[i].enabled &&
            interrupts[i].type == kIOFBVBLInterruptType) {
            return true;
        }
    }
    return false;
}

void RBAMDFramebuffer::armVBL(void)
{
    if (!vblCall || !anyVBLEnabled()) {
        vblRunning = false;
        return;
    }
    vblRunning = true;
    uint64_t deadline = 0;
    clock_interval_to_deadline((uint32_t)kVBLPeriodNs, 1, &deadline);
    thread_call_enter_delayed(vblCall, deadline);
}

void RBAMDFramebuffer::deliverVBL(void)
{
    for (unsigned i = 0; i < kMaxInterrupts; i++) {
        if (interrupts[i].used && interrupts[i].enabled &&
            interrupts[i].type == kIOFBVBLInterruptType && interrupts[i].proc) {
            interrupts[i].proc(interrupts[i].target, interrupts[i].ref);
        }
    }
    armVBL();
}

void RBAMDFramebuffer::vblFired(thread_call_param_t self, thread_call_param_t)
{
    static_cast<RBAMDFramebuffer *>(self)->deliverVBL();
}

IOReturn RBAMDFramebuffer::registerForInterruptType(IOSelect interruptType,
                                                    IOFBInterruptProc proc,
                                                    OSObject *target, void *ref,
                                                    void **interruptRef)
{
    if (interruptType != kIOFBVBLInterruptType) {
        /* Decline every other type explicitly. A silent success here would be
         * a registration that reports success without a patch applying, which
         * rule 20 exists to forbid. */
        return kIOReturnUnsupported;
    }
    for (unsigned i = 0; i < kMaxInterrupts; i++) {
        if (interrupts[i].used) {
            continue;
        }
        interrupts[i].type    = interruptType;
        interrupts[i].proc    = proc;
        interrupts[i].target  = target;
        interrupts[i].ref     = ref;
        interrupts[i].enabled = false;   /* IOFramebuffer enables it explicitly */
        interrupts[i].used    = true;
        if (interruptRef) {
            *interruptRef = &interrupts[i];
        }
        LOG("registered 'vbl ' handler %u (period %llu ns)", i, kVBLPeriodNs);
        return kIOReturnSuccess;
    }
    LOG("no free interrupt slot for 'vbl ' (%u in use)", kMaxInterrupts);
    return kIOReturnNoResources;
}

IOReturn RBAMDFramebuffer::unregisterInterrupt(void *interruptRef)
{
    for (unsigned i = 0; i < kMaxInterrupts; i++) {
        if (interruptRef == &interrupts[i] && interrupts[i].used) {
            bzero(&interrupts[i], sizeof(interrupts[i]));
            if (!anyVBLEnabled() && vblCall) {
                thread_call_cancel(vblCall);
                vblRunning = false;
            }
            return kIOReturnSuccess;
        }
    }
    return kIOReturnBadArgument;
}

IOReturn RBAMDFramebuffer::setInterruptState(void *interruptRef, UInt32 state)
{
    for (unsigned i = 0; i < kMaxInterrupts; i++) {
        if (interruptRef == &interrupts[i] && interrupts[i].used) {
            interrupts[i].enabled = (state != 0);
            if (interrupts[i].enabled && !vblRunning) {
                armVBL();
            } else if (!anyVBLEnabled() && vblCall) {
                thread_call_cancel(vblCall);
                vblRunning = false;
            }
            return kIOReturnSuccess;
        }
    }
    return kIOReturnBadArgument;
}
