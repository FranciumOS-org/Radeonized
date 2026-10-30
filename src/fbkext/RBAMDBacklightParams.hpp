/* SPDX-License-Identifier: MIT */
/* 2y0: the IODisplayParameterHandler that publishes the full-name
 * `brightness` parameter. The long reasoning - how IODisplay finds it, why
 * the service plane, and why a framebuffer cannot name a parameter longer
 * than four characters by itself - is the header comment on the .cpp. */
#ifndef RBAMDBACKLIGHTPARAMS_HPP
#define RBAMDBACKLIGHTPARAMS_HPP

#include <IOKit/graphics/IODisplay.h>

class RBAMDFramebuffer;

class RBAMDBacklightParams : public IODisplayParameterHandler {
    OSDeclareDefaultStructors(RBAMDBacklightParams)

public:
    /* Not a provider relationship - the framebuffer creates this, so it
     * hands over the back-pointer directly rather than going through
     * start(). Returns false only on a null owner. */
    bool attachTo(RBAMDFramebuffer *owner);

    /* The four the superclass makes pure virtual. Slots read out of
     * IOGraphicsFamily: setDisplay 0x850, doUpdate 0x868. */
    bool setDisplay(IODisplay *display) APPLE_KEXT_OVERRIDE;
    bool doIntegerSet(OSDictionary *params, const OSSymbol *paramName,
                      UInt32 value) APPLE_KEXT_OVERRIDE;
    bool doDataSet(const OSSymbol *paramName, OSData *value) APPLE_KEXT_OVERRIDE;
    bool doUpdate(void) APPLE_KEXT_OVERRIDE;

private:
    bool publish(void);

    RBAMDFramebuffer *fb      = nullptr;
    IODisplay        *display = nullptr;
};

#endif /* RBAMDBACKLIGHTPARAMS_HPP */
