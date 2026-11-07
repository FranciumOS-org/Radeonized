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
#include "rb_dmub.h"
#include "rb_pg.h"
#include "rb_dig.h"
#include "rb_enc.h"
#include "rb_digbe.h"
#include "rb_dpaltq.h"
#include "rb_frametime.h"
#include "rb_gpuname.h"
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
    /* 2zm: the live tunable channel. `IORegistryEntrySetCFProperties` from
     * userspace lands here, which is how an A/B can be run INSIDE one boot -
     * rule 44's preferred form, and the one this project could never reach,
     * because every dial it has ever had was a boot-arg. Six boots went into
     * three dose points for the retire tick for exactly that reason.
     *
     * Deliberately narrow: it accepts `RBFBPubMs` and nothing else, bounded by
     * rb_pub_ms_ok() so the boot-arg path and this one cannot disagree
     * (rule 38), and it changes the rate of an INSTRUMENT - no register is
     * written and no hardware state moves, which is why this is not in the
     * class of things that need the user's decision. Every other key is
     * refused and counted, so a typo is a reading rather than silence
     * (rule 14). */
    virtual IOReturn setProperties(OSObject *props) APPLE_KEXT_OVERRIDE;
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
    /* 2zk: the hot-path publish throttle. pubMs is `rbgpufbpubms=<0..10000>`
     * in milliseconds; 0 restores the pre-2zk behaviour, which is the A side
     * of the A/B rule 44 asks for. pubHotCalls counts what the frame path
     * wanted and pubHotDone what it got, so the ratio says how much was
     * actually elided rather than how much we intended to (rule 14). */
    uint32_t pubMs;
    uint64_t pubIntervalAbs;
    uint64_t pubNextAbs;
    uint64_t pubHotCalls;
    uint64_t pubHotDone;
    /* 2zm: the live setter's own record. Accepted and refused are separate so
     * "nobody asked" and "asked for something out of range" cannot read
     * alike, and pubMsLast keeps what was refused (rule 19's form: a refusal
     * path must publish the value it refused). */
    uint64_t pubSetsOk;
    uint64_t pubSetsRefused;
    uint64_t pubSetsOther;
    long     pubMsLast;
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

    /* 2CA: the chip's own name and carve-out size, fetched once from the
     * plugin. gpuNameLookups makes it one attempt whether or not it worked,
     * so a missing plugin is not retried every vblank; gpuNameDerived says
     * whether the `model` property on this node is the chip's answer or
     * start()'s literal. */
    uint32_t        gpuNameLookups{0};
    uint32_t        gpuNameDerived{0};
    bool               hubpDeferPend;  /* 2xm: -rbgpufbflippend */
    /* 2B1: SURFACE_TRIPLE_BUFFER_ENABLE, the second pending flip slot.
     * hubpTripleOrig is the word read BEFORE anything is written, so the
     * restore is a snapshot rather than a reconstruction (2y5's rule about
     * reading the restore state instead of inferring it). */
    bool               hubpTripleArmed;   /* the dial's current intent     */
    uint32_t           hubpTripleOrig;    /* FLIP_CONTROL2 before we wrote */
    bool               hubpTripleHaveOrig;
    uint64_t           hubpTripleWrites;
    uint64_t           hubpTripleRefused; /* rb_hubp_triple_may_write said no */
    uint32_t           hubpTripleRefusedVal;
    uint32_t           hubpTripleReadback;
    uint64_t           hubpDeferrals;  /* 2xm: writes skipped because of it */
    /* 2B3: the five HPD blocks, read-only, out of the aperture HUBP already
     * maps. hpdSetupMask is the sample taken at setup (which cable state that
     * was is a fact about the boot, not about the hardware - rule 33), and
     * hpdSinkMask is resampled on every periodic publish so a hotplug AFTER
     * boot is visible without a reboot. hpdSinkEver ORs every mask ever seen,
     * because a cable plugged and pulled between two publishes would
     * otherwise leave nothing behind (rule 31: what is published periodically
     * is stale by design). hpdEnMask is the CONTROL, published beside the
     * sense so "no sink" cannot be confused with "the block is off". */
    uint32_t           hpdSinkMask;
    uint32_t           hpdSinkEver;
    uint32_t           hpdEnMask;
    uint32_t           hpdSetupMask;
    uint32_t           hpdSetupEnMask;
    uint64_t           hpdChanges;     /* transitions of hpdSinkMask         */
    /* 2B8: the five DP AUX engines, read-only, and the LIVE OTG words.
     * otgLive exists because 2B5 found that both existing OTG readings
     * (the plugin's and hubpOtgCtl1..3) are one-shots taken at setup, so
     * nothing could say whether a second pipe lit with the cable attached. */
    uint32_t           auxControl[5];
    uint32_t           auxDphy[5];
    uint32_t           auxStatus[5];
    uint32_t           auxArb[5];
    uint32_t           auxEnMask;
    uint32_t           auxMatchMask;   /* vs instance 0 - see rb_aux.h      */
    uint32_t           otgLive[4];
    /* 2BA: the DPCD read. auxDpcdRefuse is a CODE and not a bool, because
     * "the DMCUB had the engine" and "no engine is bound to that HPD" are
     * different facts that a single failure flag would merge (rule 14):
     * 1 unarmed, 2 no map, 3 no engine for that HPD, 4 bad request,
     * 5 engine dark, 6 DMCUB owns it, 7 stale DONE never fell,
     * 8 no DONE after GO, 9 DONE with a zero reply count. */
    bool               auxDpcdArmed;
    uint64_t           auxDpcdAttempts;
    uint64_t           auxDpcdOks;
    uint32_t           auxDpcdRefuse;
    uint32_t           auxDpcdInst;
    uint32_t           auxDpcdArbSeen;
    uint32_t           auxDpcdStatus;
    uint32_t           auxDpcdReplyCode;
    uint32_t           auxDpcdBytes;
    uint8_t            auxDpcdData[16];
    bool               auxDpcdSuspect;
    uint32_t           auxDpcdHpd;     /* which HPD block to target, def 2 */
    /* 2BC: the EDID. auxEdidRefuse shares 1-9 with the DPCD codes and adds
     * 10 (the offset write never ACKed) and 11 (a chunk read failed), and
     * auxEdidBytes says how far it got, because a partial block is a
     * different finding from none. */
    uint64_t           auxEdidAttempts;
    uint64_t           auxEdidOks;
    uint32_t           auxEdidRefuse;
    uint32_t           auxEdidInst;
    uint32_t           auxEdidBytes;
    uint32_t           auxEdidDefers;
    bool               auxEdidHeaderOk;
    bool               auxEdidSumOk;
    uint8_t            auxEdidData[128];
    /* 2C0: the five RDPCSTX PHY blocks (DP alt mode, lane power, ref clock).
     * Opt-in, read-only, resampled every periodic publish so a cable event
     * needs no reboot. dpaltEverNotDisabled ORs every not-disabled mask seen,
     * because the IDENTIFICATION is a difference across a cable event (2B4's
     * method) and a periodic publish is stale by design (rule 31). */
    uint32_t           dpaltWord[5];   /* RB_DPALT_COUNT, last raw words     */
    uint32_t           dpaltActiveMask;
    uint32_t           dpaltNotDisabledMask;
    uint32_t           dpaltEverNotDisabled;
    uint64_t           dpaltChanges;   /* transitions of the not-disabled mask */
    int                dpaltSweep;     /* enum rb_dpalt_sweep: the control   */
    bool               dpaltSampled;   /* a sample has been taken: see below */
    bool               dpaltArmed;     /* -rbgpufbdpalt                      */
    bool               dpaltReady;     /* the aperture covered the registers */
    uint32_t           hpdStatus[5];   /* RB_HPD_COUNT, last raw words       */
    uint32_t           hpdControl[5];
    bool               hpdReady;       /* the aperture covered the registers */
    /* 2B5: what the BOOT set up, captured once and never written again.
     * RBFBAmtrNotReadyArmed used to publish amtrNotReady itself, so the key
     * whose name claimed provenance carried the live value and read No after
     * a runtime holdset 0 on a boot that armed the hold perfectly well -
     * which is the fact rule 34 wants before an A/B is interpreted. */
    bool               amtrNotReadySetup;
    bool               amtrNotReadySetupHave;
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
    /* 2zl: DMCUB inbox 1, read-only. The question is only whether anything
     * else produces on the ring, so the FIRST values are kept beside the
     * current ones and the move counts are separate - "never moved" and "we
     * never looked" must not read alike (rule 14). */
    uint32_t           dmubInboxBase;
    uint32_t           dmubInboxSize;
    uint32_t           dmubWptr0;
    uint32_t           dmubRptr0;
    uint32_t           dmubWptr;
    uint32_t           dmubRptr;
    uint64_t           dmubReads;
    uint64_t           dmubWptrMoves;
    uint64_t           dmubRptrMoves;
    /* 2zn: producing on that ring. Separately armed (-rbgpufbdmubbl) and,
     * even armed, never sent except on an explicit userspace trigger - see
     * RBAMDFramebufferDMUB.cpp for why the slider is deliberately not wired
     * to it. The locate and the scan are read-only and unconditional: they
     * are the control for the address derivation and they cost nothing. */
    bool               dmubBlArmed;
    uint32_t           dmubLocRefuse;  /* rb_dmub.h's RB_DMUB_LOC_* */
    uint32_t           dmubInbox[2];   /* INBOX1 BASE_ADDRESS, SIZE */
    uint32_t           dmubFb[2];      /* FB_LOCATION_BASE, FB_OFFSET */
    /* 2zn/boot 272: ALL SEVEN region-3 windows, base/top/off_lo/off_hi each,
     * raw with the enable bits intact. The table is what refuted the design
     * - CW4 is not the mailbox on this firmware - so it is published whole
     * rather than reduced to the window the locate picked. */
    uint32_t           dmubWinRegs[7 * 4];
    /* ...and what each window's memory LOOKED like: refuse, run, bad, abm.
     * The search's whole working, so a boot where no window carries a
     * signature says which ones were even candidates and why the rest were
     * not (rule 14). */
    uint32_t           dmubWinScan[7 * 4];
    int                dmubWin;        /* the mailbox window, -1 if none */
    uint32_t           dmubLayout;     /* how many of the six CW bases match */
    uint32_t           dmubSigOk;      /* an OBSERVATION, never the selector */
    uint32_t           dmubAddrKind;   /* RB_DMUB_ADDR_MC or _PA, measured */
    uint64_t           dmubMem;        /* the address the window holds */
    uint32_t           dmubCap;        /* the ring's capacity, from the register */
    uint64_t           dmubRingPA;     /* derived system-physical address */
    uint64_t           dmubRingOff;    /* ...and its offset inside the carve-out */
    uint32_t           dmubRoute;      /* 0 none, 1 BAR0 sub-range, 2 physical */
    IOMemoryDescriptor *dmubRingMD;
    IOMemoryMap       *dmubRingMap;
    volatile uint8_t  *dmubRing;
    uint32_t           dmubScanSlots;  /* 64-byte slots read at bring-up */
    uint32_t           dmubScanPlaus;  /* ...of those, plausible headers */
    uint32_t           dmubScanAbm;    /* ...of those, type == DMUB_CMD__ABM */
    uint32_t           dmubScanBytes;
    uint64_t           dmubSendsOk;
    uint64_t           dmubSendsRefused;
    uint32_t           dmubSendRefuse; /* the last refusal's code */
    uint32_t           dmubSendWptr;   /* the WPTR we last wrote */
    uint64_t           dmubAcks;       /* sends where RPTR reached that WPTR */
    uint64_t           dmubAckSpins;   /* ...and sends where it never did */
    uint64_t           dmubBlSends;
    uint64_t           dmubPipeSends;
    long               dmubBlLast;     /* -1 until one is sent */
    /* 2zt: the SET_BACKLIGHT control type, settable so one boot can try all
     * three. Defaults to RB_DMUB_BL_CONTROL_PWM, which is what every send
     * before 2zt carried, so an unset boot reproduces boot 274/277 exactly.
     * aux_inst is only non-zero for an AUX type. */
    uint32_t           dmubBlType;
    uint32_t           dmubBlAuxInst;
    uint32_t           dmubBlTypeSets;   /* how many times it was changed */
    /* The byte-level identification (2zq). dmubPush copies the command it
     * was handed into dmubReplySent and, after the ack, copies the same 64
     * bytes back out of the ring into dmubReplyGot; the verdict is
     * rb_dmub.h's, never computed here. Both buffers are published whole
     * because a verdict without the bytes it was derived from is exactly
     * what rule 26 keeps paying for. */
    uint32_t           dmubCapsSends;
    uint8_t            dmubReplySent[RB_DMUB_CMD_SIZE];
    uint8_t            dmubReplyGot[RB_DMUB_CMD_SIZE];
    uint32_t           dmubReplyVerdict;   /* RB_DMUB_REPLY_* */
    uint32_t           dmubReplySlot;      /* the ring offset we wrote */
    uint32_t           dmubConfirms;       /* how many came back CONFIRMED */
    /* GPINT (2zr): the one test on this surface with no address in it. */
    uint32_t           dmubGpintSends, dmubGpintRefuse;
    uint32_t           dmubGpintSent, dmubGpintBack, dmubGpintAcked;
    uint32_t           dmubGpintSpins;

    /* 2DC: DOMAIN_CONTROL, the one remaining test of whether this firmware
     * EXECUTES an inbox command (2DB retired every other instrument), and
     * the DOMAIN power-gating words that are its readback.
     *
     * The whole experiment is ONE setProperties call - sample, send, sample -
     * because four sessions in a row were lost to state that drifted between
     * the arm and the action: the wrong physical port (2D8), the wrong
     * firmware MUX mode (2D9), the wrong ring window (2DA), and that window
     * again after a reboot reset it (2DA addendum 2). A handover that needs
     * two commands in the right order with nothing rebooting in between is a
     * handover that will be run wrong. */
    bool               dmubDomArmed;     /* -rbgpufbdmubdom */
    uint32_t           dmubDomSends;
    uint32_t           dmubDomRefuse;    /* the last refusal's code */
    uint32_t           dmubDomInst;      /* the instance last asked for */
    uint32_t           dmubDomGate;      /* ...and whether it was a gate */
    uint32_t           dmubDomMoved;     /* rb_pg_moved_mask's bitmask */
    uint32_t           dmubDomRan;       /* experiments that reached a send */
    /* 2DD: every ENTRY to the RBFBDmubDomain handler, refusals included, so a
     * reader can wait on something monotonic instead of on the value it is
     * about to interpret - RBGPUDiag-style snapshots and the 500 ms publish
     * cannot distinguish "not published yet" from "the write was lost"
     * (2C5). */
    uint32_t           dmubDomCalls;

    /* 2E2: the DIG1 transmitter command for the Type-C port - the first real
     * bring-up step for the external display, and the sixth instrument for
     * "does this firmware execute a VBIOS command". Same shape as the DOMAIN
     * experiment above and for the same reason: one call that samples,
     * sends and samples, so the arm and the action cannot come apart.
     * The target is PHY2 (2E1's ROM-derived UNIPHY_C), which drives nothing,
     * and PHY0 - the live panel - is refused by name. */
    bool               digArmed;         /* -rbgpufbdig */
    bool               feArmed;          /* -rbgpufbfe (2EC) */
    bool               dtoArmed;         /* -rbgpufbdto (2EF) */
    bool               dtoReady;         /* the aperture covered BASE_IDX 1 */
    /* 2EF: the four pipes' register groups, read at BASE_IDX 1 - and pipe 0's
     * four offsets read at BASE_IDX 2 as well, which is the alias control.
     * Both published raw whichever way the verdict falls (2B0/2B4/2B9). */
    uint32_t           dtoWord[4][4];
    uint32_t           dtoAlias[4];
    /* What PHASE[0] was compared against: `modeClkHz`, the live raster times
     * the measured refresh, formed by rb_drr.h's one definition of that
     * product. Published so a boot can be read without re-deriving it. */
    uint32_t           dtoExpectedHz;
    uint32_t           dtoRefSrc;        /* RB_DPDTO_REF_SRC_*               */
    uint32_t           dtoHTotal;        /* OTG0's live raster, read here    */
    uint32_t           dtoVTotal;
    uint32_t           dtoRefreshMhz;
    uint32_t           dtoRefHz;         /* MODULO[0], iff the base is good  */
    int                dtoBaseVerdict;
    int                dtoPipeVerdict;

    /* 2F1: the write. `dtoProgCalls` is the monotonic counter a tool polls
     * past its pre-set value rather than sleeping on a throttled snapshot
     * (rule 31).
     *
     * 2F2 SPLIT THESE IN TWO, because 2F1's single set cost the only reading
     * of the write that mattered. The `dtoCall*` fields are what the MOST
     * RECENT call asked for and saw; the `dtoProg*`/`dtoRate*` fields are the
     * record of the last call that actually WROTE. 2F1 reset the second set
     * at entry and then returned through the refusal, so a second invocation
     * in the same boot - the one the already-live guard is there to refuse -
     * republished the successful run as `verdict -1 NOT RUN` with
     * `before == after`. Only the monotonic `dtoProgRan` survived to say a
     * write had happened at all. A refusal now touches nothing but
     * `dtoProgCalls`, `dtoProgRefuse` and the `dtoCall*` set (rule 16: the
     * instrument is a suspect, and this one erased its own result). */
    uint32_t           dtoCallInst;      /* the pipe this call asked for     */
    uint32_t           dtoCallPixHz;
    uint32_t           dtoCallSrcSel;
    uint32_t           dtoCallRefHz;     /* pipe 0's MODULO, as sampled now  */
    uint32_t           dtoCallLive;      /* the target's live word, now      */
    uint32_t           dtoProgInst;
    uint32_t           dtoProgPixHz;
    uint32_t           dtoProgRefHz;     /* MODULO copied from pipe 0        */
    uint32_t           dtoProgSrcSel;    /* the one field Linux and the      */
                                         /* firmware disagree on (2F0)       */
    uint32_t           dtoProgRefuse;    /* RB_DPDTO_W_*                     */
    uint32_t           dtoProgApplied;
    uint32_t           dtoProgChanged;
    uint32_t           dtoProgClean;
    uint32_t           dtoRateBefore;
    uint32_t           dtoRateAfter;
    uint32_t           dtoPhaseAfter;    /* raw, and NOT the verdict (2F0)   */
    uint32_t           dtoModuloAfter;
    uint32_t           dtoProgCalls;     /* monotonic, including refusals    */
    uint32_t           dtoProgRan;       /* writes that actually happened    */
    int                dtoProgVerdict;   /* RB_DPDTO_PROG_*                  */
    uint32_t           digFe;            /* the FE one-hot the dial asked */

    /* 2EC: DIG_FE_SOURCE_SELECT, the FIRST write this project makes to the
     * DIG back end. `feClean` is the check that the RMW disturbed no other
     * field, and `feChanged` is the one carrying evidence of the store -
     * `feApplied` alone has no control (2E6). */
    uint32_t           feInst;
    uint32_t           feMask;
    uint32_t           feRefuse;
    uint32_t           feRan;
    uint32_t           feApplied;
    uint32_t           feChanged;
    uint32_t           feClean;
    uint32_t           feBefore;
    uint32_t           feAfter;
    bool               dpaltqArmed;      /* -rbgpufbaltq (2EA) */
    uint32_t           digCalls;         /* every handler entry, monotonic */
    uint32_t           digSends;         /* pushes that reached the ring   */
    uint32_t           digRan;           /* experiments that sampled after */
    uint32_t           digRefuse;        /* the last refusal's code        */
    uint32_t           digPhy;           /* the phyid last asked for       */
    uint32_t           digBefore;        /* target PHY_CNTL6 before        */
    uint32_t           digAfter;         /* ...and after                   */
    uint32_t           digRefWord;       /* the CONTROL PHY's word         */
    uint32_t           digVerdict;       /* rb_dig_verdict()               */

    /* 2E5: enc1_configure_encoder's two writes, which precede the DIG1
     * transmitter command in dcn10_link_encoder_enable_dp_output and which
     * 2E2 deliberately did not reproduce. `digEnc` is the A/B dial, default
     * on: 2E4 IS the A side, but having it reachable in the SAME boot is
     * what 2B7 cost a milestone to learn. */
    bool               digEnc;           /* configure before sending       */
    uint32_t           encInst;          /* link encoder last configured   */
    uint32_t           encRefuse;        /* rb_enc_refuse()                */
    uint32_t           encRan;           /* configures that wrote          */
    uint32_t           encCfgBefore;     /* DP_CONFIG before               */
    uint32_t           encCfgAfter;      /* ...and read BACK after         */
    uint32_t           encScramBefore;   /* DP_DPHY_SCRAM_CNTL before      */
    uint32_t           encScramAfter;    /* ...and read BACK after         */
    uint32_t           encApplied;       /* rb_enc_applied() on the reads  */
    /* 2E6: `encApplied` with before == after == target cannot tell a landed
     * write from a dropped one, so the change bit is published beside it. */
    uint32_t           encChanged;       /* rb_enc_changed() on the reads   */
    uint32_t           encSurveyCfg[RB_ENC_COUNT];    /* DP_CONFIG, all 5   */
    uint32_t           encSurveyScram[RB_ENC_COUNT];  /* SCRAM_CNTL, all 5  */
    uint32_t           encSurveyMask;    /* rb_enc_configured_mask()        */
    bool               encSurveyDone;    /* latched - only REAL readings do */
    /* 2E7: DIG_BE_CNTL on all five, read-only. The one named missing step
     * (DIG_FE_SOURCE_SELECT) asked with a read instead of a write. */
    uint32_t           digbeWord[RB_DIGBE_COUNT];
    uint32_t           digbeEnWord[RB_DIGBE_COUNT];
    uint32_t           digbeMask;        /* rb_digbe_connected_mask()       */
    uint32_t           digbeEnMask;      /* rb_digbe_enabled_mask()         */
    bool               digbeDone;        /* latched - only REAL readings do */
    /* 2E8: the same two blocks re-read AFTER a send, so the transmitter
     * command's effect on the DIG back end is a before/after inside ONE boot.
     * 2E4 judged that command on RDPCSTX_PHY_CNTL6 alone and DIG_ENABLE -
     * Linux's own "is this transmitter up" test - was never sampled. */
    uint32_t           digbePost[RB_DIGBE_COUNT];
    uint32_t           digbeEnPost[RB_DIGBE_COUNT];
    /* 2EA: the chip's own answer to "is DP granted on this PHY", swept over
     * all five. Read-only - every field but phy_id is an output. The ROM's
     * DP_IS_USB_C pattern (2E1) is the control and lives in rb_dpaltq.h. */
    struct rb_dpaltq_reply dpaltqReply[RB_DPALTQ_PHY_COUNT];
    uint32_t           dpaltqRefuse[RB_DPALTQ_PHY_COUNT];
    uint32_t           dpaltqVerdict;
    uint32_t           dpaltqUsbMask;
    uint32_t           dpaltqFill;
    uint32_t           dpaltqRetCleared; /* bitmask: firmware cleared RET   */
    uint64_t           dpaltqCalls;      /* monotonic, bumped before any    */
    uint32_t           dpaltqSends;
    uint32_t           digbeDiffMask;    /* rb_digbe_diff_mask() on CNTL    */
    uint32_t           digbeEnDiffMask;  /* ... and on EN_CNTL              */
    bool               digbePostDone;
    uint32_t           pgStatus[RB_PG_DOMAIN_COUNT];
    uint32_t           pgConfig[RB_PG_DOMAIN_COUNT];
    uint32_t           pgBefore[RB_PG_DOMAIN_COUNT];
    uint32_t           pgAfter[RB_PG_DOMAIN_COUNT];
    uint32_t           pgSamples;        /* so a zeroed sample is not a read */
    uint32_t           dmubScanBad;      /* implausible slots, published apart */
    int                dmubScanWin;      /* WHICH window the scan describes */
    uint32_t           dmubScratch0, dmubScratch7, dmubDataout;
    uint32_t           dmubScratch0Pre, dmubScratch7Pre;
    /* 2zs: every candidate window, mapped and selectable. INBOX1_BASE_ADDRESS
     * reads 0x80000000, which no region-3 window covers, so CW4 cannot be the
     * mailbox on this firmware however well the CODE windows match Linux. */
    struct RBDmubCand {
        IOMemoryDescriptor *md;
        IOMemoryMap        *map;
        volatile uint8_t   *mem;
        uint64_t            pa;
        uint64_t            memAddr;
        uint32_t            span;
        uint32_t            route;
        uint32_t            refuse;
        uint32_t            kind;
    };
    RBDmubCand         dmubCand[RB_DMUB_WINSEL_COUNT];
    uint32_t           dmubWinSel;
    uint32_t           dmubSelSets;      /* 2E3: monotonic select count */
    uint32_t           dmubInboxInR3;
    uint32_t           dmubRegRegs[9];   /* r4/r5/r6: off_lo, off_hi, top */
    uint32_t           dmubRegScan[9];   /* r4/r5/r6: run, bad, abm */
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
    /* 2A0: the frame-time budget. Four accumulators that tile the wall clock
     * between one transaction and the next - see src/common/rb_frametime.h
     * for why a DECOMPOSITION is the next step and not an eighth dial.
     *
     * ftOn is a RUNTIME dial (`{"RBFBFtOn": 0|1}`), not a boot-arg, because
     * 2zm's lesson is that a dial you can move inside one boot costs one boot
     * instead of four - and the instrument must have a control, since it adds
     * two mach_absolute_time() reads per transaction and rule 16 says the
     * instrument is a suspect. ftReset zeroes the accumulators so a window is
     * EXACT: cumulative sums over a boot that spent minutes idle describe the
     * idle desktop, not the frame path.
     *
     * ftPrevExit is the previous setAttribute's exit stamp, in mach units; 0
     * means "no previous one", which is the first transaction and after every
     * reset, and those produce no gap sample rather than a gap measured from
     * the epoch. */
    bool               ftOn;
    uint64_t           ftPrevExit;
    uint64_t           ftResets;
    struct rb_ft_acc   ftWork;      /* inside setAttribute('amtr')       */
    struct rb_ft_acc   ftGap;       /* between transactions              */
    struct rb_ft_acc   ftGet;       /* inside getAttribute('amtr')       */
    struct rb_ft_acc   ftVbl;       /* inside deliverVBL()               */
    /* 2A1: how fast Apple comes back, split by what we told it.
     *
     * 2A1 measured `upstream` at 99.5 % of the interval and the one piece of
     * it we own is the 2xo kIOReturnNotReady hold - but the WAIT happens in
     * Apple's isTransactionComplete loop (2x6), not in our call, so the
     * budget cannot attribute it. These two accumulate the same quantity -
     * the gap from one 'amtr' GET's exit to the next one's entry - separated
     * only by whether we HELD the previous one.
     *
     * ftRetry is the treatment, ftPoll the control, and they are the same
     * measurement on the same clock in the same window, which is what makes
     * the comparison mean anything (rule 45: one dial per observation). If
     * they are alike the hold costs nothing and upstream is Apple's own
     * pacing; if ftRetry is a vblank and ftPoll is microseconds, the hold is
     * the ceiling and it is ours. */
    uint64_t           ftPrevGetExit;
    bool               ftPrevGetHeld;
    struct rb_ft_acc   ftRetry;     /* previous GET was HELD              */
    struct rb_ft_acc   ftPoll;      /* previous GET answered ready        */
    /* 2A7: how much of a hold is the LATCH and how much is Apple's phase.
     *
     * The flip is vsync'd (RB_HUBP_FLIP_TYPE 0), so SURFACE_FLIP_PENDING
     * cannot clear before the next vblank - the wait is real hardware time,
     * not something a cleverer answer can skip. What MIGHT be recoverable is
     * the part after it: Apple asks again every ~4.4 ms (2A2) while the latch
     * resolves on a ~6.95 ms boundary, so an ask can miss the boundary and
     * cost another whole round trip.
     *
     * ftLatchWait is the wall time from the FIRST held GET of a run to the
     * GET that finally finds the flip latched - everything we cost Apple for
     * one flip. ftHoldAsks is how many GETs that run took. Together they
     * separate the two: if the wait is ~1 vblank and the asks are ~1, we are
     * paying the hardware and there is nothing to win; if the wait is ~2
     * vblanks and the asks are ~2, the second ask arrived after the latch was
     * already done and the phase is the cost.
     *
     * This measures; it changes nothing about the hold (2A6: the hold stays,
     * because the flicker is real). */
    uint64_t           ftHoldStart;  /* stamp of the run's first held GET  */
    uint64_t           ftHoldAsks;   /* held GETs in the current run       */
    struct rb_ft_acc   ftLatchWait;  /* run start -> the GET that saw clear */
    struct rb_ft_acc   ftHoldRunAsks;/* asks per run, in "ns" (count*1)    */

    /* 2A9: the same bracket, but regime-independent - and a HISTOGRAM.
     *
     * ftLatchWait above only exists when we hold, so it says nothing about
     * the configuration the flicker actually happens in. A PENDING run is
     * the same physical interval observed without reference to our answer:
     * from the first GET that sees SURFACE_FLIP_PENDING set to the first
     * that sees it clear. With the hold on it should reproduce ftLatchWait
     * (a free cross-check between a new instrument and an old one); with the
     * hold off it is the only thing that can measure the exposure window at
     * all, because hubpHoldNotReady() used to return before reading the bit.
     *
     * The histogram is the point, not the mean: see rb_frametime.h for why a
     * mean of 1.00 vblanks cannot distinguish "every flip needs the whole
     * period" from "most are nearly done and a tail is exposed", and only
     * the second has a middle setting in it. */
    uint64_t           ftPendStart;   /* first GET of the run that saw set  */
    uint64_t           ftPendAsks;    /* GETs in the current pending run    */
    struct rb_ft_acc   ftPendWait;    /* run start -> the GET that saw clear */
    struct rb_ft_acc   ftPendRunAsks; /* asks per run, as a count           */
    struct rb_ft_hist  ftPendHist;    /* that wait, in quarters of a vblank */
    uint32_t           hubpTiling;    /* DCSURF_TILING_CONFIG, read-only */
    /* 2A9: the word hubpFlipPending() last read. A SEPARATE member from
     * hubpFlipCtl, which is 2xj's one-shot setup snapshot published as
     * RBFBHubpFlipCtl - reusing it would have overwritten a published
     * setup fact with frame-path noise ~300 times a second, and nothing
     * downstream would have said so. */
    uint32_t           hubpPendCtl;
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
    /* 2A9: split the OBSERVATION from the DECISION. hubpFlipPending() reads
     * SURFACE_FLIP_PENDING and nothing else; hubpHoldNotReady() takes that
     * answer and decides. They were one function, which meant the bit was
     * only ever read when the hold was armed - so the instrument was blind
     * in exactly the configuration under investigation. */
    bool hubpFlipPending(void);
    /* 2B1: returns true iff the readback confirms the bit moved (rule 20 -
     * a setter that reports success is not evidence the field changed). */
    bool hubpTripleApply(bool enable);
    bool hubpHoldNotReady(bool pending);
    /* 2B3: resample the HPD blocks. Read-only; safe to call from the publish
     * path because it touches no state the frame path reads. */
    void hpdSample(void);
    /* 2B8: read the AUX engines and the live OTG words. Read-only, same
     * mapping, same arming as hpdSample(). */
    void auxSample(void);
    /* 2C0: the RDPCSTX PHY survey. Same segment and same arming shape as
     * hpdSample(), but gated on dpaltArmed as well, because this is a PHY
     * block this project has never read and Linux's own comment warns the
     * access can hang when DMCUB is not running. */
    void dpaltSample(void);
    /* 2EF: pipe 0's live DP pixel-clock DTO, read-only - and the check that
     * BASE_IDX 1 is a real address before anything is written there. Same
     * mapping as hpdSample(); gated on dtoArmed. */
    void dtoSample(void);

    /* 2F1: the three writes of `dcn31_program_pix_clk`'s DP branch on one
     * dark pipe. Refuses unless THIS boot's dtoSample() confirmed BASE_IDX 1,
     * and refuses pipe 0 by name. Returns an RB_DPDTO_W_* code. */
    uint32_t dtoProgram(uint32_t inst, uint32_t pixHz, int srcSel);
    void dtoPublish(void);
    /* 2BA: a native AUX read of the sink's DPCD on the engine bound to `hpd`.
     * Opt-in (-rbgpufbaux) and triggered only from setProperties; every poll
     * bounded; refuses rather than racing when the DMCUB owns the engine. */
    bool auxDpcdRead(uint32_t hpd, uint32_t address, uint32_t length);
    /* 2BC: one AUX transaction of any of the five actions, and one 128-byte
     * EDID block over I2C-over-AUX (the MOT protocol). */
    bool auxTransact(unsigned inst, uint32_t action, uint32_t address,
                     const uint8_t *wdata, uint32_t wlen,
                     uint8_t *rbuf, uint32_t rlen,
                     uint32_t *replyOut, uint32_t *gotOut);
    unsigned auxEngineFor(uint32_t hpd);
    bool auxEdidRead(uint32_t hpd, uint32_t block);
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
    /* 2zn: the DMCUB inbox. dmubInit/dmubSetup/dmubTeardown bracket the
     * mapping, dmubScan is the read-only control, and dmubPush is the only
     * function in the project that writes to the ring - every send goes
     * through it. See RBAMDFramebufferDMUB.cpp. */
    void     dmubInit(void);
    bool     dmubSetup(void);
    void     dmubTeardown(void);
    void     dmubScan(void);
    uint32_t dmubMapWin(uint64_t pa, uint64_t fbOff, uint32_t span,
                        IOMemoryDescriptor **mdOut, IOMemoryMap **mapOut);
    void     dmubScanRun(volatile uint8_t *mem, uint32_t span,
                         uint32_t *run, uint32_t *bad, uint32_t *abm);
    uint32_t dmubPush(const uint8_t *cmd);
    uint32_t dmubSendBacklight(uint32_t level);
    uint32_t dmubSendPipe(void);
    uint32_t dmubSendCaps(void);
    uint32_t dmubGpint(uint32_t code);
    /* 2DC. pgSample is read-only and runs UNARMED, so a boot can read the
     * baseline without arming anything (2BA's shape: the survey is free, the
     * transaction is opt-in). dmubDomainExperiment is the armed half and is
     * the whole test in one call. */
    void     pgSample(uint32_t *status, uint32_t *config);
    uint32_t dmubSendDomain(uint32_t inst, uint32_t gate);
    uint32_t dmubDomainExperiment(uint32_t inst, uint32_t gate);
    /* 2E2. Reads the target's and the control's RDPCSTX_PHY_CNTL6 either
     * side of one DIG1 transmitter ENABLE. Armed half only; the PHY survey
     * it judges by is dpaltSample(), which runs unarmed. */
    uint32_t dmubDigExperiment(uint32_t phyid);
    void     encSurvey(void);
    void     digbeSurvey(void);
    void     digbeResample(void);
    uint32_t feConnect(uint32_t inst, uint32_t fe);
    uint32_t dmubDpAltQuery(uint32_t fill);
    uint32_t encConfigure(uint32_t inst, uint32_t lanes);
    bool dmubSelect(uint32_t n);
    void     dmubPublish(void);
    /* 2x3: follow the compositor's swap chain at vblank rate instead of only
     * when a transaction tells us. See RBAMDFramebufferHUBP.cpp. */
    void hubpPoll(void);
    void gpuNameSetup(void);
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
    /* 2A0: convert a mach-unit delta to ns and file it, bounded. Inline-cheap
     * and called from the frame path, so it does nothing at all when ftOn is
     * false - the control side of the instrument costs one bool test. */
    void ftFile(struct rb_ft_acc *acc, uint64_t t0, uint64_t t1, bool bound);
    /* 2A9: ftFile plus a histogram of the same sample, so the conversion to
     * nanoseconds still happens exactly once (rule 41). */
    void ftFileHist(struct rb_ft_acc *acc, struct rb_ft_hist *hist,
                    uint64_t t0, uint64_t t1);
    void publishFrameTime(void);
    /* 2zk: the throttled form, for the call sites that sit in the frame path.
     * publishVBLState() is ~110 setProperty calls - an OSObject allocation and
     * a registry-dictionary insert each - and boot 270 measured it running
     * 227 times a second, because setInterruptState publishes on every
     * enabled-state change and Apple's compositor arms and disarms the
     * vblank twice per transaction (VBLEnables +2269 in 20 s, publishes
     * +4547 = 2x). That is ~25 000 registry insertions a second on Apple's
     * own thread, inside the call that gates every frame. Rule 16: the
     * instrument is a suspect, and this one has never been examined while
     * six models of the ~73 sets/s ceiling have died. The periodic publish
     * every 288 vblanks already carries every field this one does, so the
     * hot-path call is throttled rather than needed. */
    void publishVBLStateHot(void);
};

#endif /* RBAMDFRAMEBUFFER_HPP */
