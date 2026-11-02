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

/* ONE AUX transaction, acquire to release. Everything above this comment's
 * bullet list applies to every caller: the engine comes from the hardware's
 * selector, the DMCUB refusal fires before anything is written, and the
 * release runs on every path out.
 *
 * `action` is dc_ddc_types.h's i2caux_transaction_action - a DP read, or one
 * of the four I2C forms. The request layout is identical for all of them
 * (dce_aux.c's submit_channel_request only branches on is_write), which is
 * why this is one function and not two.
 */
bool RBAMDFramebuffer::auxTransact(unsigned inst, uint32_t action,
                                   uint32_t address,
                                   const uint8_t *wdata, uint32_t wlen,
                                   uint8_t *rbuf, uint32_t rlen,
                                   uint32_t *replyOut, uint32_t *gotOut)
{
    if (replyOut) { *replyOut = 0xff; }
    if (gotOut)   { *gotOut = 0; }
    if (inst >= RB_AUX_COUNT || !hubpRegs) { auxDpcdRefuse = 2; return false; }

    const bool isWrite = (action == RB_I2C_ACTION_WRITE ||
                          action == RB_I2C_ACTION_WRITE_MOT);

    uint8_t req[4];
    const uint32_t hdr = rb_aux_request_bytes(action, address,
                                              isWrite ? wlen : rlen,
                                              req, sizeof(req));
    if (hdr == 0) { auxDpcdRefuse = 4; return false; }
    /* dce_aux.c: a write adds its payload to the byte count. */
    const uint32_t wrBytes = hdr + (isWrite ? wlen : 0u);

    const unsigned dwArb  = kRbDmuSeg2Aux + rb_aux_dw(inst, RB_AUX_REG_ARB_CONTROL);
    const unsigned dwCtl  = kRbDmuSeg2Aux + rb_aux_dw(inst, RB_AUX_REG_CONTROL);
    const unsigned dwSwc  = kRbDmuSeg2Aux + rb_aux_dw(inst, RB_AUX_REG_SW_CONTROL);
    const unsigned dwSwd  = kRbDmuSeg2Aux + rb_aux_dw(inst, RB_AUX_REG_SW_DATA);
    const unsigned dwStat = kRbDmuSeg2Aux + rb_aux_dw(inst, RB_AUX_REG_SW_STATUS);
    const unsigned dwInt  = kRbDmuSeg2Aux + rb_aux_dw(inst, RB_AUX_REG_INT_CONTROL);

    if (!rb_aux_enabled(hubpRegs[dwCtl])) { auxDpcdRefuse = 5; return false; }

    const uint32_t arb0 = hubpRegs[dwArb];
    auxDpcdArbSeen = arb0;
    if (!rb_aux_may_acquire(arb0)) { auxDpcdRefuse = 6; return false; }

    hubpRegs[dwArb] = rb_aux_arb_acquire_word(arb0);

    bool ok = false;
    do {
        hubpRegs[dwInt] = RB_AUX_INT_SW_DONE_ACK;
        unsigned tries = RB_AUX_POLL_TRIES;
        while ((hubpRegs[dwStat] & RB_AUX_SW_DONE) && tries--) {
            IODelay(RB_AUX_POLL_US);
        }
        if (hubpRegs[dwStat] & RB_AUX_SW_DONE) { auxDpcdRefuse = 7; break; }

        hubpRegs[dwSwc] = rb_aux_swc_request_word(wrBytes);
        hubpRegs[dwSwd] = rb_aux_swd_write_word(0, req[0], 1);
        for (uint32_t i = 1; i < hdr; i++) {
            hubpRegs[dwSwd] = rb_aux_swd_write_word(0, req[i], 0);
        }
        if (isWrite) {
            for (uint32_t i = 0; i < wlen; i++) {
                hubpRegs[dwSwd] = rb_aux_swd_write_word(0, wdata[i], 0);
            }
        }

        hubpRegs[dwSwc] = rb_aux_swc_request_word(wrBytes) | RB_AUX_SWC_GO;

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
        const uint32_t code = rb_dpcd_reply_code(first);
        if (replyOut) { *replyOut = code; }

        uint32_t n = rb_dpcd_payload_bytes(replied);
        if (n > rlen) { n = rlen; }
        for (uint32_t i = 0; i < n; i++) {
            const uint8_t b = rb_aux_swd_byte(hubpRegs[dwSwd]);
            if (rbuf) { rbuf[i] = b; }
        }
        if (gotOut) { *gotOut = n; }
        /* A write ACKs with no payload, which is success - rb_dpcd_payload_
         * bytes(1) is 0 and must not read as a failure (rule 14). */
        ok = (code == RB_DPCD_REPLY_ACK) && (isWrite || n > 0);
    } while (0);

    hubpRegs[dwArb] = rb_aux_arb_release_word(hubpRegs[dwArb]);
    return ok;
}

/* Which engine serves this HPD block, read from the hardware (2B9). */
unsigned RBAMDFramebuffer::auxEngineFor(uint32_t hpd)
{
    auxSample();
    return rb_aux_for_hpd(auxControl, RB_AUX_COUNT, hpd);
}

bool RBAMDFramebuffer::auxDpcdRead(uint32_t hpd, uint32_t address,
                                   uint32_t length)
{
    auxDpcdAttempts++;
    auxDpcdRefuse = 0;
    auxDpcdReplyCode = 0xff;
    auxDpcdBytes = 0;
    auxDpcdSuspect = false;
    for (unsigned i = 0; i < RB_DPCD_MAX_READ; i++) { auxDpcdData[i] = 0; }

    if (!auxDpcdArmed)          { auxDpcdRefuse = 1; return false; }
    if (!hpdReady || !hubpRegs) { auxDpcdRefuse = 2; return false; }
    if (length == 0 || length > RB_DPCD_MAX_READ) {
        auxDpcdRefuse = 4; return false;
    }

    const unsigned inst = auxEngineFor(hpd);
    if (inst >= RB_AUX_COUNT) { auxDpcdRefuse = 3; return false; }
    auxDpcdInst = inst;

    uint32_t code = 0xff, got = 0;
    const bool ok = auxTransact(inst, RB_DPCD_ACTION_READ, address,
                                nullptr, 0, auxDpcdData, length, &code, &got);
    auxDpcdReplyCode = code;
    auxDpcdBytes = got;
    auxDpcdSuspect = rb_dpcd_reply_suspect_norepeat(auxDpcdData, got)
                         ? true : false;
    if (ok) { auxDpcdOks++; }
    return ok;
}

/* 2BC: one 128-byte EDID block over I2C-over-AUX.
 *
 * THE PROTOCOL, and why it is three kinds of transaction rather than one.
 * The EDID lives on the sink's own I2C bus at slave address 0x50, reached
 * through AUX. One I2C transaction has to span several AUX ones, and the MOT
 * ("middle of transaction") bit is what holds it open:
 *
 *   1. an I2C WRITE with MOT carrying the byte offset - this sets the EDID
 *      read pointer, and is the only WRITE this project performs over AUX;
 *   2. eight I2C READs of 16 bytes - 16 is the AUX payload maximum, so a
 *      128-byte block cannot be one transaction;
 *   3. the LAST read drops MOT, which ENDS the I2C transaction. Leaving MOT
 *      set on the final chunk holds the sink's DDC bus open and the next
 *      reader finds it busy, so rb_edid_read_action() owns that choice and
 *      the hosttest pins it.
 *
 * DEFER IS NORMAL AND NOT AN ERROR. A sink defers an I2C-over-AUX transaction
 * whenever its own DDC bus is busy, so a single-shot read would report a
 * perfectly good monitor as silent (rule 14's shape: the two look identical
 * in a failed read). Retries are bounded at RB_AUX_DEFER_RETRIES, because the
 * mirror of that mistake is spinning on a sink that always defers.
 *
 * THE RESULT IS CHECKED TWICE BEFORE ANY FIELD OF IT IS BELIEVED: the fixed
 * eight-byte header, and the 128-byte checksum. A partial read can satisfy
 * the header and fail the sum, which is exactly the failure a chunked read
 * can have, so both are published and neither alone is treated as validation.
 */
bool RBAMDFramebuffer::auxEdidRead(uint32_t hpd, uint32_t block)
{
    auxEdidAttempts++;
    auxEdidRefuse = 0;
    auxEdidBytes = 0;
    auxEdidDefers = 0;
    auxEdidHeaderOk = false;
    auxEdidSumOk = false;
    for (unsigned i = 0; i < RB_EDID_BLOCK_LEN; i++) { auxEdidData[i] = 0; }

    if (!auxDpcdArmed)          { auxEdidRefuse = 1; return false; }
    if (!hpdReady || !hubpRegs) { auxEdidRefuse = 2; return false; }

    const unsigned inst = auxEngineFor(hpd);
    if (inst >= RB_AUX_COUNT) { auxEdidRefuse = 3; return false; }
    auxEdidInst = inst;

    const uint32_t chunk = RB_DPCD_MAX_READ;                 /* 16 */
    const uint32_t chunks = RB_EDID_BLOCK_LEN / chunk;       /* 8 */
    uint32_t code = 0xff, got = 0;

    /* Step 1: set the read pointer. A WRITE with MOT. */
    const uint8_t offset = (uint8_t)(block * RB_EDID_BLOCK_LEN);
    bool ok = false;
    for (unsigned r = 0; r < RB_AUX_DEFER_RETRIES; r++) {
        ok = auxTransact(inst, RB_I2C_ACTION_WRITE_MOT, RB_EDID_I2C_ADDR,
                         &offset, 1, nullptr, 0, &code, &got);
        if (ok) { break; }
        if (!rb_dpcd_reply_is_defer(code)) { break; }
        auxEdidDefers++;
        IODelay(500);
    }
    if (!ok) { auxEdidRefuse = 10; return false; }

    /* Step 2 and 3: eight reads, the last without MOT. */
    for (uint32_t c = 0; c < chunks; c++) {
        const uint32_t action = rb_edid_read_action(c, chunks);
        ok = false;
        for (unsigned r = 0; r < RB_AUX_DEFER_RETRIES; r++) {
            ok = auxTransact(inst, action, RB_EDID_I2C_ADDR, nullptr, 0,
                             &auxEdidData[c * chunk], chunk, &code, &got);
            if (ok) { break; }
            if (!rb_dpcd_reply_is_defer(code)) { break; }
            auxEdidDefers++;
            IODelay(500);
        }
        if (!ok) { auxEdidRefuse = 11; auxEdidBytes = c * chunk; return false; }
        auxEdidBytes = c * chunk + got;
    }

    auxEdidHeaderOk = rb_edid_header_ok(auxEdidData, auxEdidBytes) ? true : false;
    auxEdidSumOk = rb_edid_checksum_ok(auxEdidData, auxEdidBytes) ? true : false;
    if (auxEdidHeaderOk && auxEdidSumOk) { auxEdidOks++; }
    return auxEdidHeaderOk && auxEdidSumOk;
}
