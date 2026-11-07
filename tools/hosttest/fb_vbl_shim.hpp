// Host substitutes for IOKit scheduling; production VBL methods are unchanged.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
extern "C" {
#include "rb_drr.h"
#include "rb_hubp.h"
#include "rb_retire.h"
#include "rb_dmub.h"
#include "rb_frametime.h"
#include "rb_dpdto.h"
#include "rb_otgprog.h"
#include "rb_aux.h"
#include "rb_dpcd.h"
#include "rb_dptx.h"
}
#include <map>
#include <string>
#include <vector>

using UInt32=uint32_t;
using UInt64=uint64_t;
using IOSelect=uint32_t;
using IOReturn=int32_t;
using thread_call_param_t=void *;
constexpr IOSelect kIOFBVBLInterruptType=0x76626c20;
constexpr IOReturn kIOReturnSuccess=0, kIOReturnUnsupported=-1;
constexpr IOReturn kIOReturnNoResources=-2, kIOReturnBadArgument=-3;
struct OSObject {};
using IOFBInterruptProc=void (*)(OSObject *,void *);
struct FakeCall { unsigned queued=0, enters=0, cancels=0; uint64_t deadline=0; };
using thread_call_t=FakeCall *;
void clock_interval_to_deadline(uint32_t,unsigned,uint64_t *);
/* 2xe: armVBL() now advances a lattice instead of adding a delay to `now`,
 * so the host needs the same three clock primitives the kext uses. The fake
 * clock runs in nanoseconds, so absolute units and nanoseconds are 1:1. */
void clock_get_uptime(uint64_t *);
void nanoseconds_to_absolutetime(uint64_t,uint64_t *);
void absolutetime_to_nanoseconds(uint64_t,uint64_t *);
// 2y7: the per-mode VBL accounting timestamps its baselines.
uint64_t mach_absolute_time(void);
void thread_call_enter_delayed(thread_call_t,uint64_t);
void thread_call_cancel(thread_call_t);
bool thread_call_enter(thread_call_t);
#define LOG(...) ((void)0)

// Only the non-virtual VBL methods are linked into the host executable. These
// fields have the same types as the real class; no kernel layout is simulated.
class RBAMDFramebuffer : public OSObject {
public:
    struct Interrupt {
        IOSelect type;
        IOFBInterruptProc proc;
        OSObject *target;
        void *ref;
        bool enabled;
        bool used;
        uint64_t fires;
    };
    static const unsigned kMaxInterrupts=4;
    static constexpr uint32_t kRefreshRate1616=0x008fd53f;
    static constexpr uint64_t kVBLPeriodNsFast=1000000000ULL*65536ULL/kRefreshRate1616;
    // 2y6: the period is a runtime value now, so the host tests exercise the
    // same retune path the mode switch uses rather than a constant.
    uint64_t vblPeriodNs=kVBLPeriodNsFast;
    uint64_t vblRetunes=0;
    uint64_t vblFiresAtRetune=0, vblNsAtRetune=0;
    uint32_t vblEnablesAtRetune=0;
    void vblRetune(uint64_t periodNs);
    // 2y6: publishVBLState reports the mode table too; the host build needs
    // the members to exist, with the same defaults start() gives them.
    bool modeArmed=false;
    uint32_t modeRefuse=20, modeLastAsked=0, modeClkHz=0, modeBaseMhz=0;
    // 2z0: the derived mode list.
    struct rb_drr_mode modeList[RB_DRR_MAX_MODES]{};
    unsigned modeCount=0;
    int modeIndexOf(int id) const {
        const int i=id-100;
        if(i<0) return -1;
        if(!modeArmed) return i==0?0:-1;
        return ((unsigned)i<modeCount)?i:-1;
    }
    uint32_t modeMinRb=0, modeMaxRb=0, modeCtlRb=0;
    uint32_t modeHTotal0=0, modeVTotal0=0;
    int32_t modeCur=100;
    uint64_t modeSwitches=0, modeApplies=0;
    unsigned getDisplayModeCount() const { return modeArmed?2u:1u; }
    Interrupt interrupts[kMaxInterrupts]{};
    thread_call_t vblCall=nullptr;
    thread_call_t vblKickCall=nullptr;
    bool vblRunning=false;
    uint64_t vblFires=0;
    uint32_t vblEnables=0;
    /* 2zk: the hot-path publish throttle. Mirrored here because the hosttest
     * compiles RBAMDFramebufferVBL.cpp against THIS class. */
    uint32_t pubMs=500;
    uint64_t pubIntervalAbs=0;
    uint64_t pubNextAbs=0;
    uint64_t pubHotCalls=0;
    uint64_t pubHotDone=0;
    uint64_t pubSetsOk=0, pubSetsRefused=0, pubSetsOther=0;   /* 2zm */
    long     pubMsLast=-1;
    uint64_t vblNextDeadline=0;
    uint32_t vblResyncs=0;
    // 2wc: the 'amtr' counters publishVBLState also reports.
    static const unsigned kAmtrSnapBytes=0x80;
    bool amtrArmed=false;
    bool amtrSnapped=false;
    uint64_t amtrGets=0;
    uint64_t amtrSets=0;
    bool selfDisarmed=false;
    unsigned disarmWrites=0;
    // 2wp: the host substitute counts the NVRAM consume instead of doing it.
    void disarmFB(){ selfDisarmed=true; disarmWrites++; }
    uint64_t amtrPeak=0;
    uint64_t amtrBucket=0;
    uint64_t amtrNoFlip=0;
    uint64_t amtrPtr=0;
    uint64_t amtrRefused=0;
    uint8_t amtrSnap[kAmtrSnapBytes]{};
    static const unsigned kAmtrFlipBytes=0x220;
    bool amtrFlipSnapped=false;
    uint64_t amtrFlipPtr=0;
    uint64_t amtrFlipRefused=0;
    uint8_t amtrFlipSnap[kAmtrFlipBytes]{};
    bool retireArmed=false;
    uint64_t retireEnables=0;
    uint64_t retireKicks=0;
    uint64_t retireDelivered=0;
    bool hubpArmed=false;
    bool hubpAll=false;
    bool hubpReady=false;
    uint32_t hubpRefuse=0;
    uint32_t hubpOtgCtl=0;
    uint32_t hubpAddrCfg=0;
    uint32_t hubpFlipCtl=0;   /* 2xj */
    uint32_t hubpFlipCtl2=0;  /* 2xj */
    uint64_t hubpPendChecks=0; /* 2xk */
    uint64_t hubpPendHits=0;   /* 2xk */
    struct rb_hubp_latch hubpLatch{};  /* 2za */
    /* 2zd: mirrored because RBAMDFramebufferVBL.cpp compiles into BOTH the
     * kext and this host test. Five milestones have now paid this tax; the
     * host has no IOKit, so the peer is a void* that is always null and
     * vblRetOffer() takes its early return. */
    void *vblRetPeer{nullptr};
    const void *vblRetSym{nullptr};
    uint32_t vblRetArmed{0};
    uint32_t vblRetLookups{0};
    uint64_t vblRetCalls{0};
    uint64_t vblRetDrove{0};
    uint64_t vblRetRefused{0};
    uint64_t vblRetModeSkips{0};  /* 2zf */
    void vblRetSetup(void) {}
    void vblRetOffer(void) {}
    /* 2CA: the name lookup needs an IOService registry, so it is stubbed -
     * but COUNTED, so the hosttest can pin that registerForInterruptType
     * reaches it exactly once per registration rather than per vblank. A
     * stub that records nothing would make the wiring untestable, which is
     * the shape rule 14 keeps catching. */
    uint32_t gpuNameLookups{0};
    uint32_t gpuNameDerived{0};
    uint32_t gpuNameSetupCalls{0};
    void gpuNameSetup(void) { gpuNameSetupCalls++; }
    bool     hubpDeferPend=false; /* 2xm */
    uint64_t hubpDeferrals=0;     /* 2xm */
    uint32_t hubpOtgCtl1=0;       /* 2xp */
    uint32_t hubpOtgCtl2=0;       /* 2xp */
    uint32_t hubpOtgCtl3=0;       /* 2xp */
    bool     hubpOtgAny=false;    /* 2xp */
    bool     amtrNotReady=false;  /* 2xn */
    uint64_t amtrHeld=0;          /* 2xn */
    uint64_t amtrHoldExpired=0;   /* 2xn */
    /* 2xs: mirrored for the backlight publish. The hosttest compiles
     * RBAMDFramebufferVBL.cpp against THIS class, so every newly published
     * field has to exist here too - the fifth time this file has been the
     * thing that broke the build. Note the declarations here carry an
     * initializer, so a regex anchored on `bool blReady;` will not match
     * them; that is exactly how the previous attempt silently did nothing. */
    bool     blArmed=false;       /* 2xs */
    bool     blReady=false;       /* 2xs */
    uint32_t blRefuse=0;          /* 2xs */
    uint32_t blOtgCtl=0;          /* 2xs */
    uint32_t blCntl=0;            /* 2xs */
    uint32_t blCntl2=0;           /* 2xs */
    uint32_t blPeriod=0;          /* 2xs */
    uint32_t blLock=0;            /* 2xs */
    uint32_t blLevel0=0;          /* 2xs */
    uint32_t blFloor=0;           /* 2xs */
    uint32_t blFloorRefused=0;    /* 2xs */
    uint32_t blLastLevel=0;       /* 2xs */
    uint32_t blLastCount=0;       /* 2xs */
    uint32_t blReadback=0;        /* 2xs */
    uint32_t blLiveCntl=0;        /* 2zi */
    uint32_t blLiveLock=0;        /* 2zi */
    uint32_t blLiveCount=0;       /* 2zi */
    uint64_t blLiveReads=0;       /* 2zi */
    uint64_t blLiveDiffs=0;       /* 2zi */
    uint32_t dmubInboxBase=0, dmubInboxSize=0;   /* 2zl */
    uint32_t dmubWptr0=0, dmubRptr0=0, dmubWptr=0, dmubRptr=0;
    uint64_t dmubReads=0, dmubWptrMoves=0, dmubRptrMoves=0;
    /* 2zi: the real one reads MMIO; here it only has to exist and be
     * side-effect free, like the other register samplers this shim stubs. */
    void blSampleLive(void) {}
    /* 2zn: the DMUB inbox. The real dmubPublish() is ~20 setProperty calls
     * in RBAMDFramebufferDMUB.cpp, which this build does not compile; here
     * it only has to exist, and it bumps setPropertyCalls by nothing so the
     * 2zk throttle test still counts only publishVBLState's own publishes.
     * Mirrored members below for the same reason every other block is
     * mirrored - this file is the fifth-and-counting cause of a broken
     * build, and an unmirrored member is a compile error, not a surprise. */
    void dmubPublish(void) {}
    bool     dmubBlArmed=false;
    uint32_t dmubLocRefuse=0;
    uint32_t dmubInbox[2]={0,0};
    uint32_t dmubFb[2]={0,0};
    uint32_t dmubWinRegs[7*4]={0};
    uint32_t dmubWinScan[7*4]={0};
    int      dmubWin=-1;
    uint32_t dmubLayout=0, dmubSigOk=0;
    uint32_t dmubAddrKind=0, dmubCap=0;
    uint64_t dmubMem=0;
    uint64_t dmubRingPA=0, dmubRingOff=0;
    uint32_t dmubRoute=0;
    uint32_t dmubScanSlots=0, dmubScanPlaus=0, dmubScanAbm=0, dmubScanBytes=0;
    uint64_t dmubSendsOk=0, dmubSendsRefused=0;
    uint32_t dmubSendRefuse=0, dmubSendWptr=0;
    uint64_t dmubAcks=0, dmubAckSpins=0, dmubBlSends=0, dmubPipeSends=0;
    long     dmubBlLast=-1;
    uint32_t dmubBlType=RB_DMUB_BL_CONTROL_PWM, dmubBlAuxInst=0, dmubBlTypeSets=0;
    uint32_t dmubCapsSends=0, dmubReplyVerdict=0, dmubReplySlot=0;
    uint32_t dmubConfirms=0;
    uint32_t dmubGpintSends=0, dmubGpintRefuse=0, dmubGpintSent=0;
    uint32_t dmubGpintBack=0, dmubGpintAcked=0, dmubGpintSpins=0;
    uint32_t dmubScratch0=0, dmubScratch7=0, dmubDataout=0;
    uint32_t dmubScratch0Pre=0, dmubScratch7Pre=0;
    uint32_t dmubWinSel=0, dmubInboxInR3=0;
    uint32_t dmubRegRegs[9]={0}, dmubRegScan[9]={0};
    uint8_t  dmubReplySent[RB_DMUB_CMD_SIZE]={0};
    uint8_t  dmubReplyGot[RB_DMUB_CMD_SIZE]={0};
    uint64_t blSets=0;            /* 2xs */
    uint64_t blGets=0;            /* 2xs */
    uint64_t blPendTimeouts=0;    /* 2xs */
    uint64_t blParmAdds=0;        /* 2xs */
    bool     blBuiltIn=false;     /* 2xy */
    uint64_t blFlagsGets=0;       /* 2xy */
    uint64_t blSenseGets=0;       /* 2xy */
    bool     blParmArmed=false;   /* 2y0 */
    uint64_t blParmSetDisplays=0; /* 2y0 */
    uint64_t blParmUpdates=0;     /* 2y0 */
    uint64_t blParmPublished=0;   /* 2y0 */
    uint64_t blParmPublishFails=0;/* 2y0 */
    uint64_t blParmNoDict=0;      /* 2y0 */
    bool     drrArmed=false;      /* 2y5 */
    bool     drrDone=false;       /* 2y5 */
    bool     drrReverted=false;   /* 2y5 */
    uint32_t drrRefuse=0;         /* 2y5 */
    uint32_t drrLines=0;          /* 2y5 */
    uint32_t drrSecs=0;           /* 2y5 */
    uint32_t drrDelay=0;          /* 2y5 */
    uint64_t drrRuns=0;           /* 2y5 */
    uint32_t drrMhzBefore=0;      /* 2y5 */
    uint32_t drrMhzDuring=0;      /* 2y5 */
    uint32_t drrMhzAfter=0;       /* 2y5 */
    uint32_t drrMinRb=0;          /* 2y5 */
    uint32_t drrMaxRb=0;          /* 2y5 */
    uint32_t drrCtlRb=0;          /* 2y5 */
    uint32_t drrCtl0=0;           /* 2y5 */
    uint32_t drrVTotalLive=0;     /* 2y5 */
    uint32_t drrStep=0;           /* 2y5 */
    uint32_t drrMin0=0;           /* 2y5 */
    uint32_t drrMax0=0;           /* 2y5 */
    uint32_t otgHTotal=0;         /* 2y4 */
    uint32_t otgVTotal=0;         /* 2y4 */
    uint32_t otgVTotalMin=0;      /* 2y4 */
    uint32_t otgVTotalMax=0;      /* 2y4 */
    uint32_t otgVTotalMid=0;      /* 2y4 */
    uint32_t otgVTotalCntl=0;     /* 2y4 */
    uint32_t otgVBlankSE=0;       /* 2y4 */
    uint32_t otgFrameCount0=0;    /* 2y4 */
    uint64_t blParmSets=0;        /* 2y0 */
    uint64_t blParmSetsPerc=0;    /* 2y2 */
    uint64_t blParmSetsLin=0;     /* 2y2 */
    uint64_t blParmSetsUsable=0;  /* 2y2 */
    uint64_t blParmSetFails=0;    /* 2y0 */
    uint32_t blParmLastAsked=0;   /* 2y0 */
    uint32_t hubpTiling=0;
    /* 2B1 */
    bool hubpTripleArmed=false, hubpTripleHaveOrig=false;
    uint32_t hubpTripleOrig=0, hubpTripleRefusedVal=0, hubpTripleReadback=0;
    /* 2B3: mirrored because RBAMDFramebufferVBL.cpp compiles into the
     * hosttest - the same reason 2B1's triple members are here. */
    uint32_t hpdSinkMask=0, hpdSinkEver=0, hpdEnMask=0;
    uint32_t hpdSetupMask=0, hpdSetupEnMask=0;
    uint64_t hpdChanges=0;
    uint32_t hpdStatus[5]={0,0,0,0,0};
    uint32_t hpdControl[5]={0,0,0,0,0};
    bool hpdReady=false;
    void hpdSample(void) {}
    /* 2B8, mirrored for the same reason as the rest. */
    uint32_t auxControl[5]={0,0,0,0,0}, auxDphy[5]={0,0,0,0,0};
    uint32_t auxStatus[5]={0,0,0,0,0}, auxArb[5]={0,0,0,0,0};
    uint32_t auxEnMask=0, auxMatchMask=0;
    uint32_t otgLive[4]={0,0,0,0};
    void auxSample(void) {}
    /* 2C0: the RDPCSTX PHY survey. Mirrored here because
     * RBAMDFramebufferVBL.cpp compiles into the hosttest and publishes
     * these - the 2B1 lesson, which cost three separate build breaks in one
     * session. dpaltSample() lives in RBAMDFramebufferHUBP.cpp, which does
     * not, so it is a stub exactly as hpdSample() and auxSample() are. */
    uint32_t dpaltWord[5]={0,0,0,0,0};
    uint32_t dpaltActiveMask=0, dpaltNotDisabledMask=0, dpaltEverNotDisabled=0;
    uint64_t dpaltChanges=0;
    int dpaltSweep=0;
    bool dpaltSampled=false, dpaltArmed=false, dpaltReady=false;
    void dpaltSample(void) {}
    /* 2EF: the DP DTO survey, stubbed for the same reason - dtoSample() lives
     * in RBAMDFramebufferHUBP.cpp, which this executable does not compile.
     * The FIELDS still have to exist here, because the publish path under
     * test reads them, and the verdicts start at their NOT_SAMPLED codes
     * rather than at zero: zero is RB_DPDTO_BASE_CONFIRMED, and a shim that
     * initialised them to "confirmed" would be a default this project has
     * been bitten by before (rule 14 - an enum 0 is a value). */
    uint32_t dtoWord[4][4]={{0,0,0,0},{0,0,0,0},{0,0,0,0},{0,0,0,0}};
    uint32_t dtoAlias[4]={0,0,0,0};
    uint32_t dtoExpectedHz=0, dtoRefHz=0;
    uint32_t dtoRefSrc=RB_DPDTO_REF_SRC_NONE;
    uint32_t dtoHTotal=0, dtoVTotal=0, dtoRefreshMhz=0;
    int dtoBaseVerdict=RB_DPDTO_BASE_NOT_SAMPLED;
    int dtoPipeVerdict=RB_DPDTO_PIPE_NOT_SAMPLED;
    bool dtoArmed=false, dtoReady=false;
    void dtoSample(void) {}
    /* 2F1: the write side. dtoPublish() and dtoProgram() both live in
     * RBAMDFramebufferHUBP.cpp, so both are stubbed - but the fields are
     * here because the setProperties path under test stores into them before
     * it ever reaches the write, and `dtoProgInst` starts at the FIRST FREE
     * pipe rather than 0, which is the panel's and would make a bare
     * RBFBDtoPixHz target it. */
    uint32_t dtoCallInst = RB_DPDTO_FIRST_FREE_PIPE;
    uint32_t dtoCallPixHz = 0;
    uint32_t dtoCallSrcSel = 1u;
    uint32_t dtoCallRefHz = 0;
    uint32_t dtoCallLive = 0;
    uint32_t dtoProgInst=RB_DPDTO_FIRST_FREE_PIPE;
    uint32_t dtoProgPixHz=0, dtoProgRefHz=0, dtoProgSrcSel=1;
    uint32_t dtoProgRefuse=RB_DPDTO_W_OK;
    uint32_t dtoProgApplied=0, dtoProgChanged=0, dtoProgClean=0;
    uint32_t dtoRateBefore=0, dtoRateAfter=0;
    uint32_t dtoPhaseAfter=0, dtoModuloAfter=0;
    uint32_t dtoProgCalls=0, dtoProgRan=0;
    int dtoProgVerdict=RB_DPDTO_PROG_NOT_RUN;
    void dtoPublish(void) {}
    uint32_t dtoProgram(uint32_t, uint32_t, int) { return RB_DPDTO_W_OK; }
    /* 2FA: the second raster, stubbed for the same reason - otgSample(),
     * otgPublish() and otgProgram() live in RBAMDFramebufferHUBP.cpp, which
     * is NOT part of the hosttest, but VBL.cpp calls all three. The verdict
     * starts at NOT_RUN and NOT at 0, which is SCANNING. */
    bool otgArmed=false, otgApertureOk=false, otgReady=false;
    uint32_t otgControl[RB_OTGPROG_COUNT]={0};
    uint32_t otgFrames[RB_OTGPROG_COUNT]={0};
    uint32_t otgCallInst=1, otgCallRefreshHz=0, otgCallModeHz=0;
    uint32_t otgCallDtoHz=0, otgCallLive=0;
    uint32_t otgCallRefuse=RB_OTGPROG_W_OK;
    uint32_t otgProgInst=1, otgProgModeHz=0, otgProgWrites=0;
    uint32_t otgProgHTotal=0, otgProgVTotal=0, otgProgVFrontPorch=0;
    uint32_t otgBefore=0, otgAfter=0;
    uint32_t otgFramesBefore=0, otgFramesAfter=0;
    uint32_t otgHTotalAfter=0, otgVTotalAfter=0;
    uint32_t otgProgCalls=0, otgProgRan=0;
    int otgProgVerdict=RB_OTGPROG_V_NOT_RUN;
    void otgSample(void) {}
    void otgPublish(void) {}
    uint32_t otgProgram(uint32_t, uint32_t, const struct rb_otgprog_mode *)
    { return RB_OTGPROG_W_OK; }
    /* 2BA, mirrored - the hosttest compiles RBAMDFramebufferVBL.cpp. */
    bool auxDpcdArmed=false, auxDpcdSuspect=false;
    uint64_t auxDpcdAttempts=0, auxDpcdOks=0;
    uint32_t auxDpcdRefuse=0, auxDpcdInst=0, auxDpcdArbSeen=0;
    uint32_t auxDpcdStatus=0, auxDpcdReplyCode=0, auxDpcdBytes=0;
    uint32_t auxDpcdHpd=2;
    /* 2F3: the DPCD write's state and publisher. */
    uint64_t auxDpcdWCalls=0, auxDpcdWOks=0;
    uint32_t auxDpcdWRefuse=RB_DPCD_W_OK, auxDpcdWHpd=2;
    uint32_t auxDpcdWInst=RB_AUX_COUNT, auxDpcdWAddr=0, auxDpcdWValue=0;
    uint32_t auxDpcdWBefore=0xffffffffu, auxDpcdWBeforeOk=0;
    uint32_t auxDpcdWAfter=0xffffffffu, auxDpcdWAfterOk=0;
    uint32_t auxDpcdWReplyCode=0xff, auxDpcdWAcked=0;
    uint32_t auxDpcdWApplied=0, auxDpcdWChanged=0;
    int auxDpcdWVerdict=RB_DPCD_W_V_NOT_RUN;
    void auxWPublish(void) {}
    /* 2F4: the DP transmitter. */
    bool dptxArmed=false, dptxReady=false;
    uint32_t dptxWord[RB_DPTX_COUNT][RB_DPTX_REG_COUNT]={{0}};
    uint64_t dptxCalls=0, dptxRan=0;
    uint32_t dptxRefuse=RB_DPTX_W_OK, dptxInst=2;
    uint32_t dptxTps=RB_DPTX_TPS1, dptxLanes=2;
    uint32_t dptxTpBefore=0, dptxTpAfter=0;
    uint32_t dptxLcBefore=0, dptxLcAfter=0;
    uint32_t dptxDcBefore=0, dptxDcAfter=0;
    uint32_t dptxPcBefore=0, dptxPcAfter=0;
    uint32_t dptxApplied=0, dptxClean=0, dptxMoved=0, dptxObservable=0;
    uint32_t auxDpcdDefers=0, auxDpcdDeferVerdict=0, auxDpcdWDefers=0;
    uint32_t dptxSinkBefore=0xffffffffu, dptxSinkBeforeOk=0;
    uint32_t dptxSinkAfter=0xffffffffu, dptxSinkAfterOk=0;
    uint32_t dptxDpcdRefuse=0, dptxDpcdSet=0;
    int dptxVerdict=RB_DPTX_V_NOT_RUN;
    void dptxSample(void) {}
    void dptxPublish(void) {}
    uint32_t dptxTrain(uint32_t, uint32_t, uint32_t, uint32_t)
    { return RB_DPTX_W_OK; }
    uint32_t auxDpcdWrite(uint32_t, uint32_t, uint32_t)
    { return RB_DPCD_W_OK; }
    unsigned char auxDpcdData[16]={0};
    bool auxDpcdRead(uint32_t, uint32_t, uint32_t) { return false; }
    /* 2BC, mirrored. */
    uint64_t auxEdidAttempts=0, auxEdidOks=0;
    uint32_t auxEdidRefuse=0, auxEdidInst=0, auxEdidBytes=0, auxEdidDefers=0;
    bool auxEdidHeaderOk=false, auxEdidSumOk=false;
    unsigned char auxEdidData[128]={0};
    bool auxEdidRead(uint32_t, uint32_t) { return false; }
    bool amtrNotReadySetup=false, amtrNotReadySetupHave=false;
    uint64_t hubpTripleWrites=0, hubpTripleRefused=0;
    bool hubpTripleApply(bool enable);
    uint32_t hubpPitch=0;
    uint32_t hubpConfig=0;
    uint64_t hubpOrig=0;
    uint64_t hubpWrites=0;
    uint64_t hubpLast=0;
    uint64_t hubpReadback=0;
    uint64_t hubpRejects=0;
    uint64_t hubpRejectAddr=0;
    uint32_t hubpRejectWhy=0;
    // 2t0: the host substitute counts NVRAM writes instead of performing
    // them, which is what makes "bounded by construction" an assertion
    // rather than a claim in a comment.
    uint32_t crumbStage=0;
    unsigned crumbWrites=0;
    uint32_t lastCrumb=0;
    void crumb(uint32_t stage){
        if(stage>crumbStage) crumbStage=stage;
        lastCrumb=crumbStage; crumbWrites++;
    }
    std::map<std::string,uint64_t> properties;
    std::map<std::string,std::string> strings;   // 2z0
    /* 2zk: count every setProperty so the throttle test can assert that
     * publishVBLState was SKIPPED rather than that the throttle's own
     * counter was not bumped - 2u9's tautology, avoided. */
    uint64_t setPropertyCalls=0;
    bool setProperty(const char *key,bool v) { setPropertyCalls++;properties[key]=v;return true; }
    // 2z0: the real IORegistryEntry has a (const char*, const char*) overload;
    // without it here, setProperty(key, buf) binds to the BOOL one and
    // publishes 1 for every string. -Wpointer-bool-conversion caught it.
    bool setProperty(const char *key,const char *v) { setPropertyCalls++;strings[key]=v;return true; }
    bool setProperty(const char *key,uint64_t v,unsigned) { setPropertyCalls++;properties[key]=v;return true; }
    std::map<std::string,std::vector<uint8_t>> blobs;
    bool setProperty(const char *key,const void *b,unsigned n) {
        const uint8_t *p=(const uint8_t *)b; blobs[key].assign(p,p+n); return true; }
    IOReturn registerForInterruptType(IOSelect,IOFBInterruptProc,OSObject *,void *,void **);
    IOReturn unregisterInterrupt(void *);
    IOReturn setInterruptState(void *,UInt32);
    void armVBL();
    void kickVBL();
    /* 2A0: the frame-time budget. Real members and the real functions, not a
     * stub, so the hosttest runs the arithmetic the kext runs - including
     * through the fake mach_absolute_time(), which is what lets a test assert
     * that mean(work)+mean(gap) reconstructs an interval it chose itself. */
    bool ftOn=true;
    uint64_t ftPrevExit=0;
    uint64_t ftResets=0;
    struct rb_ft_acc ftWork{}, ftGap{}, ftGet{}, ftVbl{};
    uint64_t ftPrevGetExit=0;
    bool ftPrevGetHeld=false;
    struct rb_ft_acc ftRetry{}, ftPoll{};
    uint64_t ftHoldStart=0, ftHoldAsks=0;
    struct rb_ft_acc ftLatchWait{}, ftHoldRunAsks{};
    /* 2A9 */
    struct rb_ft_acc ftPendWait{}, ftPendRunAsks{};
    struct rb_ft_hist ftPendHist{};
    uint64_t ftPendStart=0, ftPendAsks=0;
    uint32_t hubpPendCtl=0;
    void ftFileHist(struct rb_ft_acc *acc, struct rb_ft_hist *hist,
                    uint64_t t0, uint64_t t1);
    void ftFile(struct rb_ft_acc *acc,uint64_t t0,uint64_t t1,bool bound);
    void publishFrameTime();
    void deliverVBL();
    void publishVBLState();
    void publishVBLStateHot();   /* 2zk, defined in RBAMDFramebufferVBL.cpp */
    bool anyVBLEnabled();
    uint64_t foreignFires() const;
    uint64_t pollCalls=0;
    bool pollArmed=false;
    uint64_t pollReads=0, pollChanges=0, pollFirstSeen=0;
    void hubpPoll() { pollCalls++; }
    static void vblFired(thread_call_param_t,thread_call_param_t);
    static void vblKicked(thread_call_param_t,thread_call_param_t);
};
