/* SPDX-License-Identifier: MIT */
/*
 * RembrandtProbe: attaches to the Radeon 680M (1002:1681) next to whatever
 * driver already owns it (its own IOMatchCategory), runs the read-only probe
 * in src/common/rb_probe.c once from start(), publishes the results as
 * properties under the "RBProbe" key, and unmaps everything before returning.
 * Nothing stays mapped and nothing runs after start().
 *
 * Probe level (rb_probe.h): personality property RBProbeLevel (Info.plist, default 1), or
 * the boot-arg rbprobe_level=N, which wins.
 */
#ifndef REMBRANDT_PROBE_HPP
#define REMBRANDT_PROBE_HPP

#include <IOKit/IOService.h>

class IOPCIDevice;

class RembrandtProbe : public IOService {
    OSDeclareDefaultStructors(RembrandtProbe)

public:
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;

private:
    void publish(const struct rb_probe_result *r, IOPCIDevice *pci);
};

#endif
