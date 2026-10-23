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
 *
 * SMU queries (rb_smu.h), the only mailbox writes: personality property
 * RBSMUQuery (default false), or boot-arg rbprobe_smu=1. They run after a
 * successful probe, before the register BAR is unmapped.
 */
#ifndef REMBRANDT_PROBE_HPP
#define REMBRANDT_PROBE_HPP

#include <IOKit/IOService.h>
#include <stdint.h>

class IOPCIDevice;

/* 2oq: the refresh rate of a pipe that is already scanning out, timed from
 * OTG0's frame counter. Declared here only so publish() can take it; the
 * measurement itself is file-local in the .cpp, next to the register
 * accessors it uses. `ran` false means it was not attempted (level < 2, or
 * OTG0_CONTROL's OTG_MASTER_EN clear). */
struct RBVSync {
    uint8_t  ran;
    uint8_t  advanced;
    uint32_t frames;
    uint64_t ns;
    uint32_t millihertz;
    uint32_t first, last;
};

class RembrandtProbe : public IOService {
    OSDeclareDefaultStructors(RembrandtProbe)

public:
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;

private:
    void publish(const struct rb_probe_result *r, IOPCIDevice *pci, const struct rb_smu_result *sr,
                 const struct RBVSync *vs);
};

#endif
