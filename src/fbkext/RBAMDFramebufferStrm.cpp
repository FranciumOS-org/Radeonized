/* SPDX-License-Identifier: MIT */
/* 305: THE STREAM WRITE on DIG2 - the first write this project makes to the
 * DIO stream encoder, and the first thing on this line that could put a
 * picture on the wire rather than a clock.
 *
 * rb_strprog.h carries the write list, every refusal and the citations; this
 * file is the order of operations, the delays, and the two moments that make
 * the result mean anything.
 *
 * THE LICENCE. Nothing is written unless THIS MOMENT'S sample says the MSA
 * derivation reproduces the live panel bit for bit. strmProgram() re-samples
 * before it builds rather than trusting the periodic publish, because
 * RBGPUDiag-style snapshots are stale by design and a licence read off a
 * stale snapshot is not a licence (rule 31). Boot 474 is where that check
 * first passed, with PARAM3's sync and polarity inputs read rather than
 * assumed.
 *
 * THE VERDICT IS BIT 16, NOT BIT 0. DP_VID_STREAM_CNTL bit 0 ENABLE is what
 * a driver writes and bit 16 STATUS is what the hardware answers; upstream
 * REG_WAITs on the second. Taking the first as the answer is rule 20's
 * defect, and this line has lacked that witness since 2EB. Both are
 * published, never collapsed into one.
 *
 * THE STREAM IS NOT THE PICTURE. There is no surface on pipe 1 yet, so a
 * STATUS that comes up means the encoder is carrying a raster whose pixels
 * are whatever the unprogrammed HUBP feeds it. That is a real intermediate
 * result and must be read as one - not as success, and not as failure.
 */
#include "RBAMDFramebuffer.hpp"
#include "rb_strenc.h"
#include "rb_strprog.h"
#include "rb_dpdto.h"
#include <IOKit/IOLib.h>

static const uint32_t kRbDmuSeg2Strm = 0x34c0;

/* How long to wait for STATUS to drop after ENABLE 0. Upstream's REG_WAIT is
 * 10 us x 5000 = 50 ms; this is the same budget, bounded, and a timeout is a
 * REFUSAL WITH ITS OWN CODE rather than a silent continue - carrying on past
 * a stuck FIFO is how the next three writes land on a block that is still
 * draining. */
static const uint32_t kStrmWaitUs    = 10u;
static const uint32_t kStrmWaitTries = 5000u;

uint32_t RBAMDFramebuffer::strmProgram(uint32_t inst, uint32_t tgInst,
                                       uint32_t pixKhz, uint32_t linkRate,
                                       uint32_t phase)
{
    struct rb_strprog_plan plan;
    struct rb_strenc_msa msa;
    const struct rb_strprog_write *w = plan.w;
    uint32_t n = 0, i;
    int rc;

    strmCalls++;

    /* The run record is reset either way, so a refusal cannot be read as the
     * previous run's result (rule 14, and 2F2's lesson about a selector set
     * shared with a run record). */
    strmRefuse      = 0;
    strmWrites      = 0;
    strmApplied     = 0;
    strmChangedMask = 0;
    strmRan         = 0;
    strmVerdict     = RB_STRPROG_V_NOT_RUN;
    strmCntlBefore  = 0;
    strmCntlAfter   = 0;
    strmStatusOff   = 0;
    strmMVid        = 0;
    strmPhaseUsed   = phase;
    for (i = 0; i < RB_STRPROG_MAX_WRITES; i++) {
        strmWas[i] = 0; strmWant[i] = 0; strmGot[i] = 0;
    }
    strmInstUsed    = inst;
    strmOtgUsed     = tgInst;
    strmPixKhzUsed  = pixKhz;
    strmLinkUsed    = linkRate;

    if (!strmArmed)   { strmRefuse = RB_STRPROG_W_NOT_ARMED; return strmRefuse; }
    if (!hubpRegs)    { strmRefuse = RB_STRPROG_W_NO_REGS;   return strmRefuse; }
    if (!strencReady) { strmRefuse = RB_STRPROG_W_APERTURE;  return strmRefuse; }

    /* Re-sample NOW. The licence below is only worth what the sample behind
     * it is worth, and the published one is periodic. */
    strencSample();

    /* We derive only pipes 0 and 1. A tg_inst we have no derivation for gets
     * its own refusal rather than being silently paired with pipe 1's words -
     * substituting a value and then writing it is the recurring error in its
     * most expensive form. */
    if (tgInst != 1u) { strmRefuse = RB_STRPROG_W_NO_DERIVATION; return strmRefuse; }

    msa.param1 = strencDerived[1][0];
    msa.param2 = strencDerived[1][1];
    msa.param3 = strencDerived[1][2];
    msa.param4 = strencDerived[1][3];

    strmMVid = rb_strenc_m_vid(pixKhz, linkRate);

    /* The builder holds the licence check itself: strencMsaMismatch is pipe
     * 0's derivation against the live panel, and strencDeriveRc[1] is whether
     * the target pipe had a raster to derive from at all. Two different
     * questions, two different refusals. */
    rc = rb_strprog_build(inst, tgInst, &msa, strencMsaMismatch,
                          strencDeriveRc[1], strmMVid,
                          (enum rb_strprog_phase)phase, &plan);
    if (rc != RB_STRPROG_OK) {
        strmRefuse = (uint32_t)rc;
        return strmRefuse;
    }
    n = plan.n;
    strmWrites = n;

    /* The instance's own CNTL before anything moves - a captured value needs
     * a captured control in the same boot (rule 19), and here in the same
     * call. */
    strmCntlBefore = hubpRegs[kRbDmuSeg2Strm + RB_STRENC_DW_VID_STREAM_CNTL +
                              inst * RB_STRENC_INST_STRIDE];

    for (i = 0; i < n; i++) {
        const uint32_t addr = kRbDmuSeg2Strm + w[i].dw;
        const uint32_t was  = hubpRegs[addr];
        uint32_t val;

        if (w[i].whole) {
            /* Linux's REG_SET: the register is written, every other field
             * zeroed. Not cosmetic on a register holding live state, which
             * is why it is carried per write rather than assumed. */
            val = w[i].value;
        } else {
            val = (was & ~w[i].mask) | (w[i].value & w[i].mask);
        }

        hubpRegs[addr] = val;
        strmApplied++;

        /* `changed` beside `applied`: a readback agreeing with the target is
         * not evidence of a write when the target was already present
         * (rule 20). DIG2's DIG_SOURCE_SELECT is expected to be one of
         * these - it reads 0, and 0 is also the reset value, so whether the
         * write moved anything is exactly the thing that cannot be assumed. */
        if (hubpRegs[addr] != was) { strmChangedMask |= (1u << i); }

        /* THE READBACK ITSELF, per write, not just whether it differed.
         * Boot 478 inferred "DP_VID_M refused the store" from a zero bit in
         * the changed mask, which cannot tell a dropped store from a store of
         * the value already there. The word the register held immediately
         * after the store is the reading; keep it. */
        if (i < RB_STRPROG_MAX_WRITES) {
            strmWas[i]  = was;
            strmWant[i] = val;
            strmGot[i]  = hubpRegs[addr];
        }

        if (w[i].delay_us) { IODelay(w[i].delay_us); }

        /* After ENABLE 0, upstream WAITS for STATUS to clear before touching
         * the FIFO. Bounded, and a timeout refuses. */
        /* The wait point comes from the PLAN, not from a constant naming a
         * position in the list: the two phases put it in different places,
         * and a hardcoded index is the kind that drifts silently when the
         * list is reordered (which is exactly what the split just did). */
        if (plan.wait_off_idx >= 0 && i == (uint32_t)plan.wait_off_idx) {
            uint32_t t;
            for (t = 0; t < kStrmWaitTries; t++) {
                const uint32_t c = hubpRegs[kRbDmuSeg2Strm + w[i].dw];
                if (!rb_strenc_status(c)) { break; }
                IODelay(kStrmWaitUs);
            }
            strmStatusOff = (t < kStrmWaitTries) ? 1u : 0u;
            if (!strmStatusOff) {
                strmRefuse = RB_STRPROG_W_STATUS_STUCK;
                strmRan    = 1u;
                strmCntlAfter = hubpRegs[kRbDmuSeg2Strm + w[i].dw];
                return strmRefuse;
            }
        }
    }

    strmRan = 1u;

    /* THE ANSWER. Read the whole word back and let rb_strprog_verdict()
     * split ENABLE from STATUS; four MSA readbacks beside it, so "the write
     * landed" and "the hardware accepted it" stay separable. */
    strmCntlAfter = hubpRegs[kRbDmuSeg2Strm + RB_STRENC_DW_VID_STREAM_CNTL +
                             inst * RB_STRENC_INST_STRIDE];
    strmVerdict = rb_strprog_verdict(strmCntlAfter);

    for (i = 0; i < 4; i++) {
        static const uint32_t dw[4] = {
            RB_STRENC_DW_MSA_PARAM1, RB_STRENC_DW_MSA_PARAM2,
            RB_STRENC_DW_MSA_PARAM3, RB_STRENC_DW_MSA_PARAM4
        };
        strmMsaAfter[i] = hubpRegs[kRbDmuSeg2Strm + dw[i] +
                                   inst * RB_STRENC_INST_STRIDE];
    }
    strmSteerAfter = hubpRegs[kRbDmuSeg2Strm + RB_STRENC_DW_STEER_FIFO +
                              inst * RB_STRENC_INST_STRIDE];
    strmFeAfter    = hubpRegs[kRbDmuSeg2Strm + RB_STRENC_DW_DIG_FE_CNTL +
                              inst * RB_STRENC_INST_STRIDE];

    strmRefuse = RB_STRPROG_W_OK;
    return strmRefuse;
}

void RBAMDFramebuffer::strmPublish(void)
{
    char key[40];
    unsigned i;

    setProperty("RBFBStrmArmed",   (uint64_t)(strmArmed ? 1 : 0), 32);
    setProperty("RBFBStrmCalls",   (uint64_t)strmCalls, 32);
    setProperty("RBFBStrmRefuse",  (uint64_t)strmRefuse, 32);
    setProperty("RBFBStrmRan",     (uint64_t)strmRan, 32);
    setProperty("RBFBStrmWrites",  (uint64_t)strmWrites, 32);
    setProperty("RBFBStrmApplied", (uint64_t)strmApplied, 32);
    setProperty("RBFBStrmChanged", (uint64_t)strmChangedMask, 32);
    setProperty("RBFBStrmPhase",   (uint64_t)strmPhaseUsed, 32);
    setProperty("RBFBStrmInst",    (uint64_t)strmInstUsed, 32);
    setProperty("RBFBStrmOtg",     (uint64_t)strmOtgUsed, 32);
    setProperty("RBFBStrmPixKhz",  (uint64_t)strmPixKhzUsed, 32);
    setProperty("RBFBStrmLinkRate",(uint64_t)strmLinkUsed, 32);
    setProperty("RBFBStrmMVid",    (uint64_t)strmMVid, 32);
    /* The two halves, never one. */
    setProperty("RBFBStrmCntlBefore", (uint64_t)strmCntlBefore, 64);
    setProperty("RBFBStrmCntlAfter",  (uint64_t)strmCntlAfter, 64);
    setProperty("RBFBStrmStatusOff",  (uint64_t)strmStatusOff, 32);
    setProperty("RBFBStrmVerdict",    (uint64_t)strmVerdict, 32);
    setProperty("RBFBStrmSteerAfter", (uint64_t)strmSteerAfter, 64);
    setProperty("RBFBStrmFeAfter",    (uint64_t)strmFeAfter, 64);

    /* Per write: what the register held, what we asked, what it held
     * immediately afterwards. A store that is dropped and a store of a value
     * already present look identical in the changed mask alone. */
    for (i = 0; i < strmWrites && i < RB_STRPROG_MAX_WRITES; i++) {
        snprintf(key, sizeof(key), "RBFBStrmW%uWas", i);
        setProperty(key, (uint64_t)strmWas[i], 64);
        snprintf(key, sizeof(key), "RBFBStrmW%uWant", i);
        setProperty(key, (uint64_t)strmWant[i], 64);
        snprintf(key, sizeof(key), "RBFBStrmW%uGot", i);
        setProperty(key, (uint64_t)strmGot[i], 64);
    }
    for (i = 0; i < 4; i++) {
        snprintf(key, sizeof(key), "RBFBStrmMsaAfter%u", i + 1u);
        setProperty(key, (uint64_t)strmMsaAfter[i], 64);
    }
}
