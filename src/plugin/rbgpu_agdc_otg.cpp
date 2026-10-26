// 2ry: early, read-only boot timing; independent of NDRV and AMD startup.
#ifdef RBGPU_OTG_HOSTTEST
#include "otg_kernel_shim.hpp"
#else
#include "rbgpu_agdc.hpp"
#include "RembrandtGPUPeer.hpp"
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOLib.h>
#include <kern/clock.h>
#endif
#include "rb_otg.h"

// ---------------------------------------------------------------------------
// 2rm: OTG0's live timing, read-only, as the 0x921 fallback.
//
// Boot 159 measured the problem this solves: IONDRVFramebuffer ANSWERS
// getTimingInfoForDisplayMode and what it answers is flags = 0, i.e.
// kIODetailedTimingValid clear - no blanking, no sync, no pixel clock - and
// there is no IODisplayEDID anywhere in the registry. The firmware did bring
// DCN 3.1.2 up, so the panel's actual timing is in OTG0's registers.
//
// Everything here is a READ. No DCN register is written, which is what makes
// this different in kind from the OTG/HUBP writes CLAUDE.md reserves to the
// user: a wrong read reports a wrong number, it cannot blank the panel.
//
// Addressing: DMU segment 2 is 0x34c0 on this chip, parsed by this project's
// own rb_disc_parse from this machine's real discovery table
// (tests/data/rembrandt-680m-tufa15.bin -> DMU 3.1.3, nbase 5,
// base[2] = 0x34c0), so OTG0_CONTROL lands at dword 0x5001. The plugin's
// g_disc is a two-IP stub (MP1 + SMUIO) with no DMU entry, so the segment is
// a constant here - and because a constant is an assumption, it is CHECKED
// against 2oq's measured four-register signature before any value is
// believed: OTG0 with OTG_MASTER_EN set and OTG1-3 reading 0x300. If that
// does not hold we are not addressing what we think we are, and the sampler
// refuses rather than converting whatever it read.
// ---------------------------------------------------------------------------
static const uint32_t kRbDmuSeg2 = 0x34c0;   // measured; checked below

/* dcn_3_1_2_offset.h, the same dwords rb_regs.h already lists. */
static const uint32_t kRbOtgStride      = 0x80;   // OTG1_H_TOTAL - OTG0_H_TOTAL
static const uint32_t kRbOtg0Control    = 0x1b41;
static const uint32_t kRbOtg0FrameCount = 0x1b4c;
static const uint32_t kRbOtg0HTotal     = 0x1b2a;
static const uint32_t kRbOtg0HBlank     = 0x1b2b;
static const uint32_t kRbOtg0HSyncA     = 0x1b2c;
static const uint32_t kRbOtg0HSyncCntl  = 0x1b2d;
static const uint32_t kRbOtg0VTotal     = 0x1b2f;
static const uint32_t kRbOtg0VBlank     = 0x1b36;
static const uint32_t kRbOtg0VSyncA     = 0x1b37;
static const uint32_t kRbOtg0VSyncCntl  = 0x1b38;

#define RB_OTG_MASTER_EN  0x1u
#define RB_OTG_OFF_VALUE  0x300u   // 2oq measured OTG1-3 at exactly this

namespace {
class OtgMap {
public:
    IOMemoryMap *map;
    explicit OtgMap(IOMemoryMap *m) : map(m) {}
    ~OtgMap() { if (map) map->release(); }
};
uint32_t rbgpuOtgRead(const volatile uint32_t *regs, uint32_t dw) {
    return regs[kRbDmuSeg2 + dw];
}
}

// Fills `out` from OTG0 and returns true only if every precondition held.
// Rule 14: every exit publishes a key, so a boot always says which step
// stopped it instead of leaving the keys absent.
bool rbgpu_agdc_sample_otg(IOPCIDevice *device, struct rb_otg_timing *out) {
    if (!out) { rbgpu_diag_set_num("otg_err", 1); return false; }
    // 2ry: callable before IONDRVSupport and AMD's SMU sw_init. Neither a
    // late source-binding callback nor g_hw is a prerequisite for firmware
    // scanout. Never enable PCI decoding or bus mastering here.
    if (!device || device->configRead16(kIOPCIConfigVendorID) != 0x1002 ||
        device->configRead16(kIOPCIConfigDeviceID) != 0x1681 ||
        device->configRead8(kIOPCIConfigRevisionID) != 0x0a) {
        rbgpu_diag_set_num("otg_err", 6); return false;
    }
    if (!(device->configRead16(kIOPCIConfigCommand) & kIOPCICommandMemorySpace)) {
        rbgpu_diag_set_num("otg_err", 7); return false;
    }
    OtgMap mapped(device->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress5,
                                      kIOMapInhibitCache | kIOMapReadOnly));
    if (!mapped.map) { rbgpu_diag_set_num("otg_err", 8); return false; }
    const uint64_t length = mapped.map->getLength();
    rbgpu_diag_set_num("otg_bar_len", length);
    const uint64_t lastByte =
        static_cast<uint64_t>(kRbDmuSeg2 + kRbOtg0Control + 3*kRbOtgStride)*4 + 4;
    const auto address = mapped.map->getVirtualAddress();
    if (!address || (address & 3) || length < lastByte) {
        rbgpu_diag_set_num("otg_err", 9); return false;
    }
    const volatile uint32_t *regs = reinterpret_cast<const volatile uint32_t *>(address);
    rbgpu_diag_set_num("otg_map_ro", 1);

    const uint32_t ctl0 = rbgpuOtgRead(regs, kRbOtg0Control);
    rbgpu_diag_set_num("otg_ctl0", ctl0);
    uint32_t off_ok = 0;
    for (unsigned i = 1; i < 4; i++) {
        uint32_t c = rbgpuOtgRead(regs, kRbOtg0Control + i * kRbOtgStride);
        rbgpu_diag_set_num(i == 1 ? "otg_ctl1" : i == 2 ? "otg_ctl2" : "otg_ctl3", c);
        if (c == RB_OTG_OFF_VALUE) off_ok++;
    }
    rbgpu_diag_set_num("otg_off_ok", off_ok);
    if (!(ctl0 & RB_OTG_MASTER_EN) || off_ok != 3) {
        rbgpu_diag_set_num("otg_err", 3);   // not the segment we think it is
        return false;
    }

    out->h_total           = rbgpuOtgRead(regs, kRbOtg0HTotal);
    out->h_blank_start_end = rbgpuOtgRead(regs, kRbOtg0HBlank);
    out->h_sync_a          = rbgpuOtgRead(regs, kRbOtg0HSyncA);
    out->h_sync_a_cntl     = rbgpuOtgRead(regs, kRbOtg0HSyncCntl);
    out->v_total           = rbgpuOtgRead(regs, kRbOtg0VTotal);
    out->v_blank_start_end = rbgpuOtgRead(regs, kRbOtg0VBlank);
    out->v_sync_a          = rbgpuOtgRead(regs, kRbOtg0VSyncA);
    out->v_sync_a_cntl     = rbgpuOtgRead(regs, kRbOtg0VSyncCntl);

    // The pixel clock is in no register this project reads, so the refresh is
    // MEASURED from OTG0's read-only frame counter and multiplied by the
    // totals - the method RembrandtProbe.cpp already uses (2os: 10 frames in
    // 69 524 857 ns). Synthesising a clock from CVT/GTF would be rule 17.
    // BOOT 160 MEASURED THIS WRONG AND THE FIX IS THE METHOD, NOT THE WINDOW.
    // The first cut read the counter, slept a fixed 100 ms and divided: it got
    // 15 frames in 100 977 001 ns = 148.549 Hz where this panel scans at
    // 143.833, because an integer counter sampled over a fixed window can only
    // ever resolve +/- one frame - the panel really delivered 14.52 frames and
    // 15 is a 3.3 % over-read, which went straight into pixelClock (353.2 MHz
    // published against a true 341.99). Lengthening the window only shrinks
    // that error linearly and costs delay on a publish path that boot 160
    // showed is ALREADY too late.
    // So both endpoints are now latched on a counter EDGE. The frame count is
    // then exact by construction and the only error left is the difference
    // between the two polling delays, not a whole frame: at 100 us polling
    // over 16 frames (~111 ms) that is ~0.1 %, a ~30x improvement for the same
    // elapsed time. otg_frames and otg_ns are published so the arithmetic can
    // be re-checked from any capture rather than trusted.
    const uint32_t kOtgFrames  = 16;
    const uint32_t kOtgPollUs  = 100;
    const uint32_t kOtgMaxPoll = 4000; // 400 ms requested delay; bounded polls
    uint32_t polls = 0;
    uint32_t prev = rbgpuOtgRead(regs, kRbOtg0FrameCount);
    while (rbgpuOtgRead(regs, kRbOtg0FrameCount) == prev) {   // edge: start of a frame
        if (++polls >= kOtgMaxPoll) {
            rbgpu_diag_set_num("otg_err", 4);  // enabled but never advancing
            return false;
        }
        IODelay(kOtgPollUs);
    }
    const uint64_t t0 = mach_absolute_time();
    uint32_t frames = 0;
    prev = rbgpuOtgRead(regs, kRbOtg0FrameCount);
    uint64_t t1 = t0;
    while (frames < kOtgFrames) {
        if (++polls >= kOtgMaxPoll) {
            rbgpu_diag_set_num("otg_frames", frames);
            rbgpu_diag_set_num("otg_err", 4);
            return false;
        }
        IODelay(kOtgPollUs);
        const uint32_t now = rbgpuOtgRead(regs, kRbOtg0FrameCount);
        if (now != prev) {
            frames += now - prev;        // the counter may step by more than 1
            prev = now;
            t1 = mach_absolute_time();   // latched ON the edge, not after a sleep
        }
    }
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(t1 - t0, &ns);
    rbgpu_diag_set_num("otg_frames", frames);
    rbgpu_diag_set_num("otg_ns", ns);
    rbgpu_diag_set_num("otg_polls", polls);
    if (!frames || !ns) {
        rbgpu_diag_set_num("otg_err", 4);   // scanning out but not advancing
        return false;
    }
    // millihertz, so 143.833 Hz is 143833 and nothing rounds to an integer Hz.
    const uint64_t mhz = (uint64_t)frames * 1000000000000ull / ns;
    if (mhz < 20000 || mhz > 400000) {
        rbgpu_diag_set_num("otg_mhz_bad", mhz);
        rbgpu_diag_set_num("otg_err", 5);   // outside any panel we can believe
        return false;
    }
    if (rbgpuOtgRead(regs, kRbOtg0Control) != ctl0 ||
        rbgpuOtgRead(regs, kRbOtg0HTotal) != out->h_total ||
        rbgpuOtgRead(regs, kRbOtg0HBlank) != out->h_blank_start_end ||
        rbgpuOtgRead(regs, kRbOtg0HSyncA) != out->h_sync_a ||
        rbgpuOtgRead(regs, kRbOtg0HSyncCntl) != out->h_sync_a_cntl ||
        rbgpuOtgRead(regs, kRbOtg0VTotal) != out->v_total ||
        rbgpuOtgRead(regs, kRbOtg0VBlank) != out->v_blank_start_end ||
        rbgpuOtgRead(regs, kRbOtg0VSyncA) != out->v_sync_a ||
        rbgpuOtgRead(regs, kRbOtg0VSyncCntl) != out->v_sync_a_cntl) {
        rbgpu_diag_set_num("otg_err", 10); return false;
    }
    out->refresh_mhz = (uint32_t)mhz;
    rbgpu_diag_set_num("otg_err", 0);
    return true;
}
