/* SPDX-License-Identifier: MIT */
/* 2y0: the full-name `brightness` parameter, which is the only thing between
 * 2xs/2xy and the System Settings slider.
 *
 * WHAT BOOT 250 ESTABLISHED, AND WHY THIS IS A THIRD PIECE. 2xs made the
 * panel's duty cycle writable and published `'brig'`; boot 249 proved the
 * write round-trips and the user confirmed the panel physically responds. 2xy
 * made `AppleBacklightDisplay` match the connection; boot 250 proved it, with
 * `IOProbeScore` 3000, `IODisplayConnectFlags <00080000>`, `AppleSense 0`,
 * `AppleDisplayType 2` and our own `RBFBBlFlagsGets`/`RBFBBlSenseGets` both 2.
 *
 * And the slider still did not appear, nor did the brightness keys respond,
 * because the parameter is named `"brig"` and every client looks for
 * `kIODisplayBrightnessKey`, which is the string `"brightness"`.
 * `IOFramebufferParameterHandler` builds each key from the 4-BYTE FourCC we
 * return in the `'parm'` list, so four characters is all a framebuffer can
 * ever name. 2xy's commit guessed that `IOBacklightDisplay` would publish the
 * full name once it matched. It does not - boot 250 settled that - and this
 * file is what does.
 *
 * HOW IT GETS FOUND, read out of IOGraphicsFamily with no boot spent:
 *
 *   IODisplay::searchParameterHandlers(this, entry)      0x14ce7d66
 *       iter = entry->getChildIterator(gIOServicePlane)
 *       for each child: if OSDynamicCast(IODisplayParameterHandler, child)
 *                           this->addParameterHandler(child)
 *
 *   and its FIRST caller passes the FRAMEBUFFER itself (0x14ce8ba4), before
 *   the IOBacklightDisplay-only branch that also searches the children of a
 *   service named "backlight" (0x14ce8c1c, via nameMatching). We have no ACPI
 *   backlight device, but we do not need one: the unconditional call searches
 *   our own framebuffer's children, and a child of our framebuffer is
 *   something this kext creates for itself.
 *
 *   IODisplay::addParameterHandler(this, h)              0x14ce7e04
 *       arr = OSDynamicCast(OSArray, this->fParameterHandler)
 *       if (arr && arr->getNextIndexOfObject(h, 0) != -1) return true
 *       if (!h->setDisplay(this)) return false            <- vtable 0x850
 *       if (!arr) { arr = OSArray::withCapacity(2); this->fParameterHandler = arr; }
 *       arr->setObject(h)
 *
 *   The handler list is an OSArray, so adding ours does NOT displace
 *   `IOFramebufferParameterHandler`: `'brig'`, `'rgsc'`, `'ggsc'`, `'bgsc'`
 *   and `'vblm'` keep working and `"brightness"` appears beside them.
 *
 *   IODisplay::doUpdate(this)                            0x14cea4be
 *       walks the same array and calls each handler's doUpdate (vtable 0x868)
 *
 * WHY THE SERVICE PLANE and not the device tree: the plane in
 * searchParameterHandlers is one of two imported globals and the GOT slot does
 * not name itself, so it is settled from the other end. Apple's own handler
 * for this category is `AppleIntelPanelA`, whose personality is
 * `IOProviderClass = IOACPIPlatformDevice`, `IOMatchCategory =
 * IODisplayParameters` - it is its provider's SERVICE-plane child, and the
 * entry searched for it is that provider. So the plane is `gIOServicePlane`
 * and `attach()` + `registerService()` is the whole requirement. Rule 14
 * rather than trust: `RBFBBlParmSetDisplays` counts the calls we actually
 * receive, so a boot where this reasoning is wrong says 0 instead of being
 * silent, and the next step is the device tree rather than a guess.
 *
 * ORDER: this is created before `IOFramebuffer::start()`, so the child exists
 * before any `IODisplayConnect` or `IODisplay` can be made. IOKit matching is
 * asynchronous, so registering later would be a race against the display's
 * own driver.
 *
 * WHAT IT DOES NOT DO. It publishes `"brightness"` ONLY. Apple also defines
 * `"linear-brightness"` and `"usable-linear-brightness"`, whose scale and
 * meaning differ (linear against perceptual), and publishing both at once
 * would change two things in one boot and could produce a slider whose
 * position disagrees with the panel. If the keys still do not respond with
 * `"brightness"` present, `linear-brightness` is the next single variable.
 */
#include "RBAMDFramebuffer.hpp"
#include "RBAMDBacklightParams.hpp"
#include "rb_backlight.h"
#include <IOKit/graphics/IODisplay.h>
#include <IOKit/IOLib.h>

/* Rule 31 says IOLog is not an instrument and every FACT here is published
 * with setProperty instead. These lines are for the setup path only, where a
 * refusal has to be attributable before any counter exists to carry it. */
#define LOG(fmt, ...) IOLog("RBAMDFramebuffer: " fmt "\n", ##__VA_ARGS__)

OSDefineMetaClassAndStructors(RBAMDBacklightParams, IODisplayParameterHandler)

bool RBAMDBacklightParams::attachTo(RBAMDFramebuffer *owner)
{
    fb = owner;
    return fb != nullptr;
}

/* Called from IODisplay::addParameterHandler through vtable slot 0x850, and a
 * false return means we are NOT added to the array at all - so this answers
 * true for anything it can still do something with, and records the call
 * either way. */
bool RBAMDBacklightParams::setDisplay(IODisplay *disp)
{
    if (!fb) {
        return false;
    }
    fb->blParmSetDisplays++;
    display = disp;
    /* Publishing here as well as in doUpdate(): the contract read out of
     * IOGraphicsFamily guarantees doUpdate() is called for every handler in
     * the array, but it does not guarantee WHEN, and a parameter that appears
     * one vblank late looks exactly like one that never appeared. */
    (void)publish();
    return true;
}

/* vtable 0x868. Idempotent by construction - addParameter on a key already
 * present is a no-op in IODisplay and setParameter just overwrites the
 * value - so being called repeatedly costs nothing. */
bool RBAMDBacklightParams::doUpdate(void)
{
    if (fb) {
        fb->blParmUpdates++;
    }
    return publish();
}

bool RBAMDBacklightParams::publish(void)
{
    if (!fb || !display) {
        return false;
    }
    /* The dictionary IODisplay keeps its parameters in. It is the display's
     * own property, not ours, and it is the same one
     * IOFramebufferParameterHandler has already filled with 'brig' and the
     * three gamma scales - which is why this appends rather than replaces. */
    OSDictionary *params =
        OSDynamicCast(OSDictionary, display->getProperty(gIODisplayParametersKey));
    if (!params) {
        fb->blParmNoDict++;
        return false;
    }
    const uint32_t floor = fb->blFloor;
    const uint32_t level = fb->blCurrentLevel();

    /* The SAME bounds 2xs answers for 'brig', deliberately: two parameters
     * naming one register must agree about its range or a client that reads
     * one and writes the other moves the panel somewhere neither asked for.
     * The minimum is the floor and not zero, so no client can ask for a dark
     * panel (rule 12 - a state we cannot diagnose from). */
    bool ok = IODisplay::addParameter(params, gIODisplayBrightnessKey,
                                      static_cast<SInt32>(floor),
                                      static_cast<SInt32>(RB_BL_LEVEL_FULL));
    if (ok) {
        ok = IODisplay::setParameter(params, gIODisplayBrightnessKey,
                                     static_cast<SInt32>(level));
    }
    if (ok) {
        fb->blParmPublished++;
    } else {
        fb->blParmPublishFails++;
    }
    return ok;
}

/* The set path. IODisplay walks every handler in the array, so returning
 * false for a name that is not ours is correct and lets the framebuffer's own
 * handler take 'brig', 'rgsc' and the rest. */
bool RBAMDBacklightParams::doIntegerSet(OSDictionary *params,
                                        const OSSymbol *paramName,
                                        UInt32 value)
{
    if (!fb || !paramName) {
        return false;
    }
    /* Pointer comparison is enough and is what Apple's own handlers do -
     * OSSymbols are interned, so the global IS the symbol for this string -
     * but the string compare is kept as the fallback because a symbol
     * created before the global was initialised would not be the same
     * object, and a silent no-match here is indistinguishable from "the UI
     * never asked" (rule 14). */
    const bool mine = (paramName == gIODisplayBrightnessKey) ||
                      (strcmp(paramName->getCStringNoCopy(),
                              kIODisplayBrightnessKey) == 0);
    if (!mine) {
        return false;
    }

    fb->blParmSets++;
    fb->blParmLastAsked = value;
    if (!fb->blSetLevel(value)) {
        fb->blParmSetFails++;
        return false;
    }
    /* Reflect what the panel actually took, not what was asked: blSetLevel
     * clamps to the floor, and a dictionary that disagrees with the register
     * is how a slider ends up fighting the hardware. */
    if (params) {
        IODisplay::setParameter(params, paramName,
                                static_cast<SInt32>(fb->blLastLevel));
    }
    return true;
}

bool RBAMDBacklightParams::doDataSet(const OSSymbol *, OSData *)
{
    /* Nothing here takes a data-typed parameter. Declining is the truthful
     * answer and lets another handler have it. */
    return false;
}

/* -- the framebuffer's side -------------------------------------------- */

bool RBAMDFramebuffer::blParamsSetup(void)
{
    blParams = nullptr;
    blParmSetDisplays = 0;
    blParmUpdates = 0;
    blParmPublished = 0;
    blParmPublishFails = 0;
    blParmNoDict = 0;
    blParmSets = 0;
    blParmSetFails = 0;
    blParmLastAsked = 0;

    if (!blParmArmed) {
        return false;
    }
    /* Requires 2xs: with no register access there is nothing to publish a
     * range for, and a parameter whose writes cannot land is worse than an
     * absent one - the slider would move and the panel would not. Rule 38's
     * question, answered: this half depends on blReady and on nothing else. */
    if (!blReady) {
        LOG("2y0: -rbgpufbblparm is armed but the backlight is not ready "
            "(RBFBBlRefuse %u), so the \"brightness\" parameter is NOT "
            "published - a parameter whose writes cannot reach the register "
            "is worse than none.", blRefuse);
        return false;
    }

    blParams = new RBAMDBacklightParams;
    if (!blParams) {
        LOG("2y0: could not allocate the parameter handler");
        return false;
    }
    if (!blParams->init() || !blParams->attachTo(this)) {
        LOG("2y0: parameter handler init failed");
        blParams->release();
        blParams = nullptr;
        return false;
    }
    /* attach() makes it our SERVICE-plane child, which is exactly where
     * IODisplay::searchParameterHandlers looks (0x14ce7d66, called with the
     * framebuffer at 0x14ce8ba4). registerService() is what lets IOKit see
     * it; it needs no personality and matches nothing, because nothing
     * matches ON it - IODisplay finds it by walking our children. */
    if (!blParams->attach(this)) {
        LOG("2y0: parameter handler could not attach to the framebuffer");
        blParams->release();
        blParams = nullptr;
        return false;
    }
    blParams->registerService();
    LOG("2y0: \"brightness\" parameter handler attached as a child of the "
        "framebuffer; IODisplay should find it when it walks our children. "
        "RBFBBlParmSetDisplays says whether it did.");
    return true;
}

void RBAMDFramebuffer::blParamsTeardown(void)
{
    if (blParams) {
        blParams->detach(this);
        blParams->release();
        blParams = nullptr;
    }
}
