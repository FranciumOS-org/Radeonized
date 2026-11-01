/* SPDX-License-Identifier: MIT */
/* RBAMDFramebuffer - an IOFramebuffer subclass for the firmware-lit panel.
 *
 * Why this exists, and why the class is named the way it is (2ql, 2sl):
 * CoreDisplay's `__GetSystemGPUVendors_block_invoke` iterates every
 * `IOFramebuffer` service, calls `IOObjectCopyClass` on it, and `CFStringFind`s
 * `Intel` / `AMD` / `NVDA` / `AppleParavirt` in the CLASS NAME. A name matching
 * none of them earns bit 0x01, which the `supported` mask can never contain -
 * so one unclassifiable framebuffer makes `UseIOPresentment()` return 0 for
 * every possible value of every preference, and `Display::GetNextSurface` takes
 * the legacy path whose surface the accelerator refuses to create (2qk). The
 * desktop then composites on the CPU (2pd). `make useiop` prints that gate
 * live: `IONDRVFramebuffer name=.Display_boot -> bit 0x01`.
 *
 * `present` is the OR over ALL framebuffers, so adding an AMD-named one beside
 * `.Display_boot` would give 0x05 and keep the gate shut. This personality
 * therefore DISPLACES `IONDRVFramebuffer`: same provider (`IOPCIDevice`), same
 * `IONameMatch` (`display`), same `IOMatchCategory` (`IOFramebuffer`), and a
 * higher `IOProbeScore` than its 20000. One driver wins per category.
 *
 * It writes NOTHING to the display engine. The firmware already ran the modeset
 * (`OTG0_CONTROL 0x00011201`, DMCUB 4.0.15.47) and the GOP framebuffer is BAR0
 * offset 0 (2pr); every number below was read off this machine's live
 * `IONDRVFramebuffer` or measured by the plugin's OTG sampler. That is also the
 * shape of the one working comparable driver (2sj): subclass IOFramebuffer,
 * take over the lit mode, only ever READ the OTG registers.
 *
 * Arming requires BOTH -rbgpuownfb and the NVRAM key rbgpu-fb. Removal of a
 * one-shot is not proven durable (2sq/2sr); removing the boot-arg from the
 * OpenCore config is the durable disarm. Never unload a live framebuffer.
 */
#ifndef RBAMDFRAMEBUFFER_HPP
#define RBAMDFRAMEBUFFER_HPP

/* 2z0: the derived mode list lives in the shared header so the hosttest
 * decides it (and so one definition of a mode exists, not two). */
extern "C" {
#include "rb_drr.h"
#include "rb_hubp.h"
#include "rb_retire.h"
}

#include <IOKit/graphics/IOFramebuffer.h>

class IOPCIDevice;
class RBAMDBacklightParams;

class RBAMDFramebuffer : public IOFramebuffer {
    OSDeclareDefaultStructors(RBAMDFramebuffer)

    /* 2y0: the parameter handler reaches blCurrentLevel(), blSetLevel(),
     * blFloor and blLastLevel, all of which are private and should stay that
     * way - they are this class's register arithmetic, not an API. The two
     * classes ship in one bundle and are meaningless apart, so friendship is
     * the honest expression of that rather than four new public accessors. */
    friend class RBAMDBacklightParams;

public:
    void drrRun(void);           /* 2y5: reached from the thread_call thunk */
    /* 2y5: argOn() lives in RBAMDFramebuffer.cpp's anonymous namespace, so it
     * is TU-local on purpose. This forwards to it rather than letting another
     * file keep its own copy of the boot-arg rule - two copies is how two
     * halves come to disagree about whether a feature is on. */
    static bool argEnabled(const char *tok);
    /* 2y6: ONE copy of the 2rn edge-latch sampler, defined in
     * RBAMDFramebufferDRR.cpp and used by the mode table too. Two copies is
     * how two halves come to disagree about what the panel is doing. */
    static uint32_t measureMhz(volatile uint32_t *regs);
    /* IOService */
    IOService *probe(IOService *provider, SInt32 *score) APPLE_KEXT_OVERRIDE;
    bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
    void stop(IOService *provider) APPLE_KEXT_OVERRIDE;

    /* IOFramebuffer: memory */
    IODeviceMemory *getApertureRange(IOPixelAperture aperture) APPLE_KEXT_OVERRIDE;
    IODeviceMemory *getVRAMRange(void) APPLE_KEXT_OVERRIDE;

    /* IOFramebuffer: modes */
    const char *getPixelFormats(void) APPLE_KEXT_OVERRIDE;
    IOItemCount getDisplayModeCount(void) APPLE_KEXT_OVERRIDE;
    IOReturn getDisplayModes(IODisplayModeID *allDisplayModes) APPLE_KEXT_OVERRIDE;
    IOReturn getInformationForDisplayMode(IODisplayModeID displayMode,
                                          IODisplayModeInformation *info) APPLE_KEXT_OVERRIDE;
    UInt64 getPixelFormatsForDisplayMode(IODisplayModeID displayMode,
                                         IOIndex depth) APPLE_KEXT_OVERRIDE;
    IOReturn getPixelInformation(IODisplayModeID displayMode, IOIndex depth,
                                 IOPixelAperture aperture,
                                 IOPixelInformation *info) APPLE_KEXT_OVERRIDE;
    IOReturn getCurrentDisplayMode(IODisplayModeID *displayMode,
                                   IOIndex *depth) APPLE_KEXT_OVERRIDE;
    IOReturn setDisplayMode(IODisplayModeID displayMode, IOIndex depth) APPLE_KEXT_OVERRIDE;
    IOReturn getStartupDisplayMode(IODisplayModeID *displayMode,
                                   IOIndex *depth) APPLE_KEXT_OVERRIDE;
    IOReturn setStartupDisplayMode(IODisplayModeID displayMode, IOIndex depth) APPLE_KEXT_OVERRIDE;

    /* IOFramebuffer: controller and connection */
    IOReturn enableController(void) APPLE_KEXT_OVERRIDE;
    bool isConsoleDevice(void) APPLE_KEXT_OVERRIDE;
    IOItemCount getConnectionCount(void) APPLE_KEXT_OVERRIDE;
    IOReturn getAttributeForConnection(IOIndex connectIndex, IOSelect attribute,
                                       uintptr_t *value) APPLE_KEXT_OVERRIDE;
    IOReturn setAttributeForConnection(IOIndex connectIndex, IOSelect attribute,
                                       uintptr_t value) APPLE_KEXT_OVERRIDE;
    /* 2xy: the two answers IOBacklightDisplay::probe needs. See the comment
     * on the definition in RBAMDFramebufferBacklight.cpp. */
    IOReturn getAppleSense(IOIndex connectIndex, UInt32 *senseType,
                           UInt32 *primary, UInt32 *extended,
                           UInt32 *displayType) APPLE_KEXT_OVERRIDE;
    IOReturn getAttribute(IOSelect attribute, uintptr_t *value) APPLE_KEXT_OVERRIDE;
    IOReturn setAttribute(IOSelect attribute, uintptr_t value) APPLE_KEXT_OVERRIDE;

    /* 2t3: gamma. IOFramebuffer::open() READS IOFBGammaCount/Width/HeaderSize
     * off the driver and leaves its own fields at zero when they are absent,
     * so publishing them is what tells IOGraphics gamma exists at all. These
     * two then receive the tables. */
    virtual IOReturn setGammaTable(UInt32 channelCount, UInt32 dataCount,
                                   UInt32 dataWidth, void *data) APPLE_KEXT_OVERRIDE;
    virtual IOReturn setGammaTable(UInt32 channelCount, UInt32 dataCount,
                                   UInt32 dataWidth, void *data,
                                   bool syncToVBL) APPLE_KEXT_OVERRIDE;

    /* IOFramebuffer: the vblank, which is what drains the display pipe (2s4) */
    IOReturn registerForInterruptType(IOSelect interruptType, IOFBInterruptProc proc,
                                      OSObject *target, void *ref,
                                      void **interruptRef) APPLE_KEXT_OVERRIDE;
    IOReturn unregisterInterrupt(void *interruptRef) APPLE_KEXT_OVERRIDE;
    IOReturn setInterruptState(void *interruptRef, UInt32 state) APPLE_KEXT_OVERRIDE;

private:
    /* One registered interrupt handler per type. IOFramebuffer asks for at most
     * a handful; four is what the comparable driver carries (2sj). */
    struct Interrupt {
        IOSelect type;
        IOFBInterruptProc proc;
        OSObject *target;
        void *ref;
        bool enabled;
        bool used;
        uint64_t fires;
    };
    static const unsigned kMaxInterrupts = 4;
    static constexpr UInt32 kRefreshRate1616 = 0x008fd53f; /* measured 143.83 Hz */
    static constexpr uint64_t kVBLPeriodNsFast =
        1000000000ULL * 65536ULL / kRefreshRate1616;

public:
    /* 2y6: CLASS constants, not file-scope ones. kModeID used to live in an
     * anonymous namespace in RBAMDFramebuffer.cpp, and 2y5 spent a build
     * discovering what that costs when a second file needs the same name -
     * a header declaration there created a SECOND global and every call site
     * went ambiguous. One definition, reachable from both files. */
    /* 2z0: ids are kModeIDBase + index into the DERIVED list. Index 0 is
     * always the panel's native timing, which is never written. 2y6's
     * kModeIDFast/kModeID60 were a two-entry table that would have claimed
     * 143.8 Hz on a 75 Hz panel; the list now comes from the raster. */
    static constexpr IODisplayModeID kModeIDBase = 100;
    static constexpr IODisplayModeID kModeIDFast = 100;  /* == native, index 0 */

private:

    IOPCIDevice *pci;
    IODeviceMemory *aperture;
    Interrupt interrupts[kMaxInterrupts];
    thread_call_t vblCall;
    bool vblRunning;
    /* 2su: setInterruptState was silent, so enabled-and-never-fired,
     * disabled, and never-registered all looked identical. These make the
     * enable transition and the delivery observable (rule 14). */
    uint64_t vblFires;
    uint32_t vblEnables;
    /* 2xe: THE VBLANK WAS NOT A CLOCK. armVBL() re-armed from `now`, at the
     * END of deliverVBL(), so the handler's own duration was added to every
     * single period. Measured from userspace with CVDisplayLink's CVTimeStamp
     * hostTime: mean interval 7.4240 ms = 134.70 Hz against the panel's
     * 6.9527 ms = 143.83 Hz, i.e. 0.47 ms long on every tick, and the rate
     * moved with system load across four runs (115.94 / 119.97 / 120.00 /
     * 133.72 Hz nominal). Three milestones' worth of "transactions/s =
     * sets / (fires / 143.83)" were therefore overstated by up to 17 %.
     * These two make the arm drift-free: the next deadline is one period
     * after the LAST DEADLINE, never after `now`, so handler duration cannot
     * accumulate - and when we really do fall a whole period behind we
     * resync instead of firing a catch-up burst, counting it so the gap is
     * visible rather than silent (rule 14). */
    uint64_t vblNextDeadline;
    uint32_t vblResyncs;
    /* 2t0: crash breadcrumbs. Rule 10 - a boot that dies leaves NVRAM only,
     * and rule 31's registry properties vanish with the machine. */
    IORegistryEntry *nvram;
    bool crumbOn;
    uint32_t crumbStage;
    uint64_t gammaCalls;
    char crumbBoot[12];   /* rbgpu-boot, the nonce (rule 11) */

    /* 2wc: the 'amtr' attribute, which is how Apple's accelerator talks to an
     * AMD framebuffer about display-pipe transactions. See the long note on
     * setAttribute() in the .cpp. Nothing here allocates: setAttribute runs on
     * the accelerator's own thread, so it only touches these fields and the
     * VBL thread_call publishes them (rule 31). */
    static const unsigned kAmtrSnapBytes = 0x80;
    bool     amtrArmed;
    bool     amtrSnapped;
    uint64_t amtrGets;
    uint64_t amtrSets;
    /* 2wl: the transaction RATE, not just the count. amtrBucket fills over
     * one 1000-fire window (~7 s at the measured 143.83 Hz) and amtrPeak
     * keeps the fullest window the boot ever saw, so a boot that cannot be
     * captured still says whether the chain EVER ran fast. The user reported
     * typing in Spotlight making frames smooth while the idle desktop sat at
     * ~0.09/s, and a total divided by a boot length cannot tell those apart.
     * amtrNoFlip counts transactions whose flag word has bit 0x1 CLEAR - the
     * ones 2wh deliberately does not write - so "we were handed a content
     * update and dropped it" is distinguishable from "we were never handed
     * one". */
    /* 2wp: the no-transaction self-disarm. The framebuffer arms off
     * -rbgpuownfb + rbgpu-fb, INDEPENDENTLY of whether the accelerator is
     * armed - and it can only ever show anything if the accelerator feeds it
     * transactions. Taking the panel without one is a guaranteed frozen
     * screen, which rule 12 says is indistinguishable from a hang. Boot 197's
     * successor did exactly that: fb armed, plugin did not, zero transactions,
     * and a healthy machine sat behind a frozen progress bar for ten minutes.
     * So if nothing has arrived by the deadline, consume rbgpu-fb for the
     * NEXT boot. It cannot rescue this one - the panel is already ours - but
     * it turns "power cycle until you give up" into exactly one bad boot. */
    bool     selfDisarmed;
    uint64_t amtrPeak;
    uint64_t amtrBucket;
    uint64_t amtrNoFlip;
    uint64_t amtrPtr;
    uint64_t amtrRefused;
    uint8_t  amtrSnap[kAmtrSnapBytes];

    /* 2wf: the plane state the transaction points at. txn->0x10 is a
     * _AMD_PIPE_FLIP, 0x220 bytes, and Apple's own code has ALREADY filled it
     * with the GPU addresses by the time setAttribute('amtr') is called:
     * executeTransaction calls AMDHWDisplay::getDisplayPipeTransactionFlip
     * (vt[0x198], dump 0x1a8) with &pipe->0x36b0 as the out-parameter, and
     * that function's own bzero(flip + 5, 0x21b) fixes the size at 0x220.
     * Three independent agreements on that number: the bzero, AMDFramebuffer's
     * memcpy(this + 0xf10, ps, 0x220) in handleDisplayPipeTransaction, and the
     * pipe's own field adjacency (0x36b0 + 0x220 == 0x38d0, the next pointer
     * the snapshot already carries).
     *
     * The addresses inside it are computed by a leaf helper at 0xbe88846:
     *     mem = res->0x88; base = mem->vt[0x158](0, 0);
     *     k = (uint8_t)res->0x14;
     *     off = (k == 0 || k == 0x80) ? res->0xf8 : (k == 0x82) ? res->0xfc : 0;
     *     return base + off;
     * so what lands here is a GPU virtual address, not a VRAM byte offset.
     * Which field holds it is NOT derived: three more vtable layers separate
     * the helper from the store, and rules 33 and 35 both say measure our end
     * rather than attach a correct chain one link off for the eighth time.
     * So dump the struct and read the offset out of it. */
    static const unsigned kAmtrFlipBytes = 0x220;
    /* 2x3: the surface poll. */
    bool     pollArmed;
    uint64_t pollReads;      /* vblanks on which we looked           */
    uint64_t pollChanges;    /* looks that saw a NEW surface address */
    uint64_t pollFirstSeen;  /* the first address the poll ever read  */
    bool     amtrFlipSnapped;
    uint64_t amtrFlipPtr;
    uint64_t amtrFlipRefused;
    uint8_t  amtrFlipSnap[kAmtrFlipBytes];

    /* 2wh: the first write to the display engine. The decision logic is
     * src/common/rb_hubp.h, proven by the hosttest; these are the mechanism's
     * state and its telemetry. hubpProgram() runs on the accelerator's thread
     * from setAttribute('amtr'), so it only does volatile stores - the map is
     * made once in start() and the VBL thread_call publishes (rule 31). */
    bool               hubpArmed;
    /* 2wm: -rbgpufbflipall. Drops the flag-word bit 0x1 gate so a content
     * update into an unchanged surface is written too. Its own boot-arg,
     * because it trades 2wf's evidence that txn->0x10 is fresh for the
     * chance that we have been refusing frames all along. */
    bool               hubpAll;
    bool               hubpReady;
    uint64_t           hubpMax;        /* 0 = unlimited; rbgpufbhubpmax= */
    IOMemoryMap       *hubpMap;
    volatile uint32_t *hubpRegs;
    unsigned           hubpIndex;
    uint32_t           hubpRefuse;     /* 0 = set up; else why we will not write */
    uint32_t           hubpPitch;      /* the pipe state we proved ourselves on */
    uint32_t           hubpConfig;
    uint32_t           hubpOtgCtl;
    uint32_t           hubpAddrCfg;   /* DCSURF_ADDR_CONFIG, read-only */
    uint32_t           hubpFlipCtl;   /* 2xj: DCSURF_FLIP_CONTROL, read-only */
    uint32_t           hubpFlipCtl2;  /* 2xj: DCSURF_FLIP_CONTROL2, read-only */
    uint64_t           hubpPendChecks; /* 2xk: flips where PENDING was read */
    uint64_t           hubpPendHits;   /* 2xk: ...and was already set */
    /* 2za: latch latency in vblanks - the state machine is in rb_hubp.h so
     * the hosttest decides it before the kext does. */
    struct rb_hubp_latch hubpLatch;

    /* 2zd: the vblank-driven frame retire. The plugin owns the retire target
     * and lives in a different kernel collection, so this is reached by name
     * through callPlatformFunction. Looked up ONCE, off the vblank. */
    IOService      *vblRetPeer{nullptr};
    const OSSymbol *vblRetSym{nullptr};
    uint32_t        vblRetArmed{0};
    uint32_t        vblRetLookups{0};
    uint64_t        vblRetCalls{0};
    uint64_t        vblRetDrove{0};
    uint64_t        vblRetRefused{0};
    uint64_t        vblRetModeSkips{0}; /* 2zf: offers the mode gate declined */
    bool               hubpDeferPend;  /* 2xm: -rbgpufbflippend */
    uint64_t           hubpDeferrals;  /* 2xm: writes skipped because of it */
    uint32_t           hubpOtgCtl1;    /* 2xp: OTG1-3, read even on refuse 5 */
    uint32_t           hubpOtgCtl2;
    uint32_t           hubpOtgCtl3;
    bool               hubpOtgAny;     /* 2xp: -rbgpufbotgany */
    /* 2xs: the backlight. Independently armed (-rbgpufbbacklight): it needs
     * only the panel, not the accelerator, so it shares neither the HUBP
     * arm nor the HUBP BAR5 map - the segment constant is re-checked by its
     * own gate rather than inherited. */
    bool               blArmed;
    bool               blReady;
    uint32_t           blRefuse;      /* 0 = set up; else why we will not write */
    IOMemoryMap       *blMap;
    volatile uint32_t *blRegs;
    uint32_t           blOtgCtl;      /* gate 1, published either way */
    uint32_t           blCntl;        /* the four PWRSEQ0 registers as found */
    uint32_t           blCntl2;
    uint32_t           blPeriod;
    uint32_t           blLock;
    uint32_t           blOrigCntl;    /* restored on stop() */
    uint32_t           blLevel0;      /* the level the firmware left, u16.16 */
    uint32_t           blFloor;       /* rbgpufbblfloor=, u16.16 */
    uint32_t           blFloorRefused; /* rule 40: a refused value is published */
    uint32_t           blLastLevel;   /* what we last asked for, clamped */
    uint32_t           blLastCount;   /* ...and the duty cycle it became */
    uint32_t           blReadback;    /* ...and what the register read back */
    /* 2zi: the same two registers re-read on the PERIODIC path, which is a
     * different fact from blReadback. blReadback is sampled microseconds
     * after our own store, so it answers "did the store reach the chip" and
     * nothing else; blCntl/blLock are a snapshot from start() and never move.
     * Neither can see a SECOND writer. On boot 269 the duty cycle round-trips
     * (69 % -> 17 %, read back bit-identical) and the panel does not change at
     * all, which leaves exactly two worlds: either something else owns
     * BL_PWM_CNTL between our writes - ABM in the DMCUB firmware is the
     * candidate, it is running 4.0.15.47 - or this panel's backlight does not
     * follow PWRSEQ0's PWM at all, which is 2or's own falsification test
     * reached by a route a zero period cannot detect. blLiveDiffs separates
     * them: it counts publishes where the live active-count disagrees with
     * blLastCount, so a climbing counter is a second writer and a pinned zero
     * is the wrong pin. Read-only, on the path that already does MMIO reads
     * (hubpPoll), published whether or not it moved (rule 14). */
    uint32_t           blLiveCntl;    /* BL_PWM_CNTL as it reads NOW */
    uint32_t           blLiveLock;    /* BL_PWM_GRP1_REG_LOCK as it reads NOW */
    uint32_t           blLiveCount;   /* ...its ACTIVE_CNT field alone */
    uint64_t           blLiveReads;   /* publishes that sampled it at all */
    uint64_t           blLiveDiffs;   /* ...of those, how many disagreed */
    uint64_t           blSets;
    uint64_t           blGets;
    uint64_t           blPendTimeouts;
    uint64_t           blParmAdds;    /* times we appended 'brig' to 'parm' */
    /* 2xy: -rbgpufbbuiltin, the System Settings slider. Separately armed from
     * -rbgpufbbacklight because it changes which IODisplay subclass matches
     * this connection, which is a different risk from writing a PWM duty
     * cycle: the slider needs both, a dark panel needs only the second. */
    bool               blBuiltIn;
    uint64_t           blFlagsGets;   /* kConnectionFlags answers given */
    uint64_t           blSenseGets;   /* getAppleSense answers given */
    /* 2y0: -rbgpufbblparm, the full-name "brightness" parameter. Separately
     * armed again, because it creates and registers a NEW IOKit service as a
     * child of this one - a different risk from answering two attributes
     * (2xy) or writing a duty cycle (2xs). Counters rather than a log, and
     * every one of them separates a distinct outcome (rule 14):
     *   SetDisplays 0 -> IODisplay never found us walking its framebuffer's
     *                    children, so the service plane is the wrong plane
     *                    and the device tree is next - NOT a publish failure
     *   NoDict        -> found, but the display had no IODisplayParameters
     *   PublishFails  -> found and had the dict, and addParameter refused
     *   Sets 0        -> published and nothing ever asked, i.e. the UI does
     *                    not use kIODisplayBrightnessKey either */
    bool               blParmArmed;
    RBAMDBacklightParams *blParams;
    uint64_t           blParmSetDisplays;
    uint64_t           blParmUpdates;
    uint64_t           blParmPublished;
    uint64_t           blParmPublishFails;
    uint64_t           blParmNoDict;
    /* 2y5: the refresh-rate experiment. Opt-in, runs once, reverts itself. */
    bool               drrArmed;
    bool               drrDone;
    bool               drrReverted;
    uint32_t           drrRefuse;
    uint32_t           drrLines;
    uint32_t           drrSecs;
    uint32_t           drrDelay;
    uint64_t           drrRuns;
    uint32_t           drrMhzBefore;
    uint32_t           drrMhzDuring;
    uint32_t           drrMhzAfter;
    uint32_t           drrMinRb;
    uint32_t           drrMaxRb;
    uint32_t           drrCtlRb;
    uint32_t           drrCtl0;
    uint32_t           drrVTotalLive;
    uint32_t           drrStep;
    uint32_t           drrMin0;
    uint32_t           drrMax0;
    thread_call_t      drrCall;

    /* 2y6: the mode table. Opt-in via -rbgpufbmodes; on any refusal the whole
     * thing collapses back to the single mode every boot since 2sl has been
     * measured against, rather than advertising one it cannot deliver.
     *   Refuse 20 -> not armed (the boot-arg is absent): one mode, as before
     *          21 -> no BAR5 map (rule 38, said at setup not at switch time)
     *          22 -> the refresh could not be measured, so there is no clock
     *          23 -> the clock or the derived rate came out zero
     *          24 -> 60 Hz is not reachable on this raster
     *          25 -> the timing write did not read back; it was put back
     *          26 -> a mode id that is not one of ours
     *          27 -> -rbgpufbdrr is also armed; they would fight (see below)
     *   and 0..5 are rb_drr.h's own, so a published number means one thing. */
    bool               modeArmed;
    uint32_t           modeRefuse;
    IODisplayModeID    modeCur;
    uint32_t           modeClkHz;      /* DERIVED from the measurement (2y6) */
    uint32_t           modeBaseMhz;    /* what the raster measured at setup */
    struct rb_drr_mode modeList[RB_DRR_MAX_MODES];
    unsigned           modeCount;
    uint64_t           modeSwitches;
    uint64_t           modeApplies;
    uint32_t           modeLastAsked;
    uint32_t           modeMinRb;
    uint32_t           modeMaxRb;
    uint32_t           modeCtlRb;
    uint32_t           modeCtl0;
    uint32_t           modeMin0;
    uint32_t           modeMax0;
    uint32_t           modeHTotal0;
    uint32_t           modeVTotal0;
    /* 2y6: the VBL period is no longer a constant. One aligned 64-bit store
     * on x86_64, read by the thread_call each time it re-arms, so the worst
     * case of a switch racing a tick is ONE tick armed at the old period -
     * not a torn value. vblRetune() also zeroes vblNextDeadline so the clock
     * re-bases from now instead of accumulating against the old period. */
    uint64_t           vblPeriodNs;
    uint64_t           vblRetunes;
    /* 2y7: Fires and UptimeNs are cumulative across modes, so neither can
     * give a per-mode rate - boot 256's -5.0 % at 60 Hz had to be computed
     * from animtest's window instead of from the kext. These latch at each
     * retune so "delivered since this mode began" is readable directly, and
     * EnablesAtRetune tests the live lead: the compositor toggled the vblank
     * interrupt ~44 times/s, and if an enable re-bases the clock that loses
     * fractional ticks continuously (vblResyncs was only 3, which rules out
     * the resync path and with it the coalescing guess). */
    uint64_t           vblFiresAtRetune;
    uint64_t           vblNsAtRetune;
    uint32_t           vblEnablesAtRetune;

    /* 2y4: OTG0's timing and DRR group as the firmware left it, read once in
     * blSetup(). The starting point for any refresh-rate change, and the
     * restore values if one is ever written. */
    uint32_t           otgHTotal;
    uint32_t           otgVTotal;
    uint32_t           otgVTotalMin;
    uint32_t           otgVTotalMax;
    uint32_t           otgVTotalMid;
    uint32_t           otgVTotalCntl;
    uint32_t           otgVBlankSE;
    uint32_t           otgFrameCount0;
    uint64_t           blParmSets;
    /* 2y2: per KEY, not one total. The whole question the boot answers is
     * WHICH name the brightness UI writes, and a single counter cannot say
     * (rule 14). CoreBrightness holds the strings "linear-brightness" and
     * "usable-linear-brightness" and never wrote "brightness" once across
     * boot 252, so these three counters are the experiment. */
    uint64_t           blParmSetsPerc;   /* "brightness"               */
    uint64_t           blParmSetsLin;    /* "linear-brightness"        */
    uint64_t           blParmSetsUsable; /* "usable-linear-brightness" */
    uint64_t           blParmSetFails;
    uint32_t           blParmLastAsked;
    bool               amtrNotReady;   /* 2xn: -rbgpufbnotready */
    uint64_t           hubpFlipDeadline; /* 2xn: abs time the hold expires */
    uint64_t           amtrHeld;       /* 2xn: NotReady answers given */
    uint64_t           amtrHoldExpired;/* 2xn: pending but past the deadline */
    uint32_t           hubpTiling;    /* DCSURF_TILING_CONFIG, read-only */
    uint64_t           hubpOrig;       /* the firmware's address: restore path */
    uint64_t           hubpWrites;
    uint64_t           hubpLast;
    uint64_t           hubpReadback;   /* the pair read back after write #1 */
    uint64_t           hubpRejects;
    uint64_t           hubpRejectAddr; /* the FIRST refused address, and why */
    uint32_t           hubpRejectWhy;

    /* 2wi: the transaction-retirement experiment. */
    bool          retireArmed;
    uint64_t      retireEnables;
    /* 2wk: the immediate-delivery path. Boot 195 (nonce) measured Apple
     * enabling its 'vbl ' slot 11 times with 4 transactions and its handler
     * firing 0 times in 7200 ticks - so the enable window is shorter than our
     * 7 ms period and the periodic tick never lands inside it. A SECOND
     * thread_call, fired from the enable transition itself, delivers on our
     * own thread microseconds later instead. Kept separate from vblCall so
     * the 143.83 Hz clock stays a clean time reference for the crumb's rate
     * formula, and so a kick can never cancel a pending periodic tick. */
    thread_call_t vblKickCall;
    uint64_t      retireKicks;      /* enable transitions we answered */
    uint64_t      retireDelivered;  /* kicks that found the slot still enabled */

    bool hubpSetup(void);
    void hubpTeardown(void);
    void hubpProgram(uint64_t mc);
    /* 2xn: is OUR flip still unlatched? Lives in RBAMDFramebufferHUBP.cpp
     * so the DMU segment constant stays in exactly one file. */
    bool hubpHoldNotReady(void);
    bool blSetup(void);
    void blTeardown(void);
    /* 2y0: lives in RBAMDBacklightParams.cpp beside the class it creates. */
    void otgTimingReset(void);   /* 2y4 */
    void drrReset(void);         /* 2y5 */
    bool drrSetup(void);
    void drrTeardown(void);
    bool modeKnown(IODisplayModeID id) const;
    IOReturn getStartupDisplayMode_fast(IODisplayModeID *displayMode, IOIndex *depth);
    void modeReset(void);        /* 2y6, RBAMDFramebufferModes.cpp */
    bool modeSetup(void);
    void modeTeardown(void);
    bool modeApply(IODisplayModeID want);
    UInt32 modeRate1616(IODisplayModeID id) const;
    int modeIndexOf(IODisplayModeID id) const;
    void vblRetune(uint64_t periodNs);
    bool blParamsSetup(void);
    void blParamsTeardown(void);
    uint32_t blCurrentLevel(void);
    bool blSetLevel(uint32_t level);
    /* 2zi: re-read BL_PWM_CNTL and the group lock on the periodic path, so a
     * second writer between our stores becomes visible. Read-only. */
    void blSampleLive(void);
    /* 2x3: follow the compositor's swap chain at vblank rate instead of only
     * when a transaction tells us. See RBAMDFramebufferHUBP.cpp. */
    void hubpPoll(void);
    void vblRetSetup(void);
    void vblRetOffer(void);

    /* True iff this instance won the `IOFramebuffer` match category, i.e.
     * displaced IONDRVFramebuffer. Gates the console claim and the connection
     * count; see the comment on the definition. */
    bool ownsPanel(void);

    static void vblFired(thread_call_param_t self, thread_call_param_t);
    static void vblKicked(thread_call_param_t self, thread_call_param_t);
    void kickVBL(void);
    void disarmFB(void);
    void crumb(uint32_t stage);
    void deliverVBL(void);
    void armVBL(void);
    bool anyVBLEnabled(void);
    /* 2x1: summed fires over every 'vbl ' slot we did NOT register - i.e.
     * Apple's. The per-slot facts stay in the registry (rule 14: a last-wins
     * key cannot express a per-entry fact), but the SUM answers the one
     * question the crumb must survive to answer: did Apple's handler ever
     * run? Zero with E= non-zero means we enabled its slot and it still
     * never ran. */
    uint64_t foreignFires(void) const;
    void publishVBLState(void);
};

#endif /* RBAMDFRAMEBUFFER_HPP */
