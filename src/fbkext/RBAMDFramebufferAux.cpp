/* SPDX-License-Identifier: MIT */
/* 2BA: a native AUX read of the Type-C sink's DPCD.
 *
 * THE FIRST WRITE THIS PROJECT MAKES TO A DISPLAY BLOCK OUTSIDE THE PANEL'S
 * OWN PIPE, and it is the user's decision (2026-11-02). What bounds it:
 *
 *   - It runs on the engine bound to an HPD block the caller names, defaulting
 *     to HPD2, which 2B4 established is the Type-C port. The PANEL's engine
 *     (AUX0, HPD_SEL 0) is never the target unless someone asks for HPD 0, and
 *     the arming is opt-in besides.
 *   - The arbitration is REFUSED, not raced: Linux's acquire_engine() declines
 *     when AUX_ARB_CONTROL says the DMCUB owns the engine, and so does
 *     rb_aux_may_acquire(). 2B8 declined to write at all because "the DMCUB
 *     also arbitrates"; the answer is that the hardware publishes the owner.
 *   - It is a native READ. No DPCD write, no I2C-over-AUX, no link training.
 *     Reading DPCD cannot change the sink's state.
 *   - `-rbgpufbaux` arms it and NOTHING IS SENT until a userspace trigger
 *     names an address, the same shape as 2zn's DMUB route. An armed boot
 *     nobody pokes performs no transaction.
 *   - Every poll is BOUNDED, and this runs from setProperties on a userspace
 *     thread - never the frame path, so rule 31's prohibition on blocking
 *     Apple's thread is not engaged.
 *
 * The sequence is dce_aux.c's submit_channel_request() and
 * read_channel_reply(), transcribed. Two of its steps are SKIPPED and the
 * reason is in the register list rather than in a judgement:
 * AUX_COMMON_REG_LIST0(id) - which dcn31_resource.c uses verbatim - contains
 * seven registers and AUXN_IMPCAL is not one of them, so Linux's own
 * `if (REG(AUXN_IMPCAL))` is false on this chip and the calibration block
 * does not run. And acquire_engine()'s AUX_EN enable path is guarded by
 * `if (field == 0)`; 2B9 measured AUX_EN already set on both live engines, so
 * it is skipped too - and if it is ever clear we REFUSE rather than enabling
 * it, because enabling a dark engine is a different decision from using a
 * configured one.
 */

#include "RBAMDFramebuffer.hpp"
#include "rb_aux.h"
#include "rb_dpcd.h"
#include <IOKit/IOLib.h>

static const uint32_t kRbDmuSeg2Aux = 0x34c0;

/* Bounded waits. The engine answers in microseconds when it answers at all;
 * these are backstops, and reaching one is published rather than silent. */
#define RB_AUX_POLL_US      10
#define RB_AUX_POLL_TRIES   600      /* 6 ms */

bool RBAMDFramebuffer::auxDpcdRead(uint32_t hpd, uint32_t address,
                                   uint32_t length)
{
    auxDpcdAttempts++;
    auxDpcdRefuse = 0;
    auxDpcdReplyCode = 0xff;
    auxDpcdBytes = 0;
    auxDpcdSuspect = false;
    for (unsigned i = 0; i < RB_DPCD_MAX_READ; i++) { auxDpcdData[i] = 0; }

    if (!auxDpcdArmed)               { auxDpcdRefuse = 1; return false; }
    if (!hpdReady || !hubpRegs)       { auxDpcdRefuse = 2; return false; }

    /* Which engine? Read from the hardware's own selector (2B9), never
     * assumed. A fresh sample first, because an engine can have been
     * reconfigured since the last publish. */
    auxSample();
    const unsigned inst = rb_aux_for_hpd(auxControl, RB_AUX_COUNT, hpd);
    if (inst >= RB_AUX_COUNT)         { auxDpcdRefuse = 3; return false; }
    auxDpcdInst = inst;

    uint8_t req[4];
    const uint32_t reqLen = rb_dpcd_read_request(address, length,
                                                 req, sizeof(req));
    if (reqLen == 0)                  { auxDpcdRefuse = 4; return false; }

    const unsigned dwArb  = kRbDmuSeg2Aux + rb_aux_dw(inst, RB_AUX_REG_ARB_CONTROL);
    const unsigned dwCtl  = kRbDmuSeg2Aux + rb_aux_dw(inst, RB_AUX_REG_CONTROL);
    const unsigned dwSwc  = kRbDmuSeg2Aux + rb_aux_dw(inst, RB_AUX_REG_SW_CONTROL);
    const unsigned dwSwd  = kRbDmuSeg2Aux + rb_aux_dw(inst, RB_AUX_REG_SW_DATA);
    const unsigned dwStat = kRbDmuSeg2Aux + rb_aux_dw(inst, RB_AUX_REG_SW_STATUS);
    const unsigned dwInt  = kRbDmuSeg2Aux + rb_aux_dw(inst, RB_AUX_REG_INT_CONTROL);

    /* A dark engine is a refusal, not something to bring up here. */
    if (!rb_aux_enabled(hubpRegs[dwCtl])) { auxDpcdRefuse = 5; return false; }

    /* THE REFUSAL THAT MAKES THIS SAFE. Published with the value refused
     * (rule 19) so "the DMCUB had it" is distinguishable from every other
     * way this can decline. */
    const uint32_t arb0 = hubpRegs[dwArb];
    auxDpcdArbSeen = arb0;
    if (!rb_aux_may_acquire(arb0))     { auxDpcdRefuse = 6; return false; }

    hubpRegs[dwArb] = rb_aux_arb_acquire_word(arb0);

    bool ok = false;
    do {
        /* Acknowledge any stale DONE, then wait for it to fall. */
        hubpRegs[dwInt] = RB_AUX_INT_SW_DONE_ACK;
        unsigned tries = RB_AUX_POLL_TRIES;
        while ((hubpRegs[dwStat] & RB_AUX_SW_DONE) && tries--) {
            IODelay(RB_AUX_POLL_US);
        }
        if (hubpRegs[dwStat] & RB_AUX_SW_DONE) { auxDpcdRefuse = 7; break; }

        hubpRegs[dwSwc] = rb_aux_swc_request_word(reqLen);
        /* The first word carries AUTOINCREMENT_DISABLE so index 0 is
         * addressed; the rest clear it so the engine walks the buffer. */
        hubpRegs[dwSwd] = rb_aux_swd_write_word(0, req[0], 1);
        for (uint32_t i = 1; i < reqLen; i++) {
            hubpRegs[dwSwd] = rb_aux_swd_write_word(0, req[i], 0);
        }

        hubpRegs[dwSwc] = rb_aux_swc_request_word(reqLen) | RB_AUX_SWC_GO;

        tries = RB_AUX_POLL_TRIES;
        while (!(hubpRegs[dwStat] & RB_AUX_SW_DONE) && tries--) {
            IODelay(RB_AUX_POLL_US);
        }
        const uint32_t st = hubpRegs[dwStat];
        auxDpcdStatus = st;
        if (!(st & RB_AUX_SW_DONE)) { auxDpcdRefuse = 8; break; }

        const uint32_t replied = rb_aux_reply_count(st);
        if (replied == 0) { auxDpcdRefuse = 9; break; }

        hubpRegs[dwSwd] = rb_aux_swd_read_setup_word();
        const uint8_t first = rb_aux_swd_byte(hubpRegs[dwSwd]);
        auxDpcdReplyCode = rb_dpcd_reply_code(first);

        uint32_t n = rb_dpcd_payload_bytes(replied);
        if (n > RB_DPCD_MAX_READ) { n = RB_DPCD_MAX_READ; }
        for (uint32_t i = 0; i < n; i++) {
            auxDpcdData[i] = rb_aux_swd_byte(hubpRegs[dwSwd]);
        }
        auxDpcdBytes = n;
        auxDpcdSuspect = rb_dpcd_reply_suspect_norepeat(auxDpcdData, n)
                             ? true : false;
        ok = (auxDpcdReplyCode == RB_DPCD_REPLY_ACK) && n > 0;
    } while (0);

    /* Release unconditionally, on every path out of the block above - an
     * engine left with our REQ bit set is one the DMCUB cannot take back. */
    hubpRegs[dwArb] = rb_aux_arb_release_word(hubpRegs[dwArb]);

    if (ok) { auxDpcdOks++; }
    return ok;
}
