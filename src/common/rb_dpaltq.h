/* rb_dpaltq.h - DMUB_CMD__VBIOS_TRANSMITTER_QUERY_DP_ALT (sub-type 26),
 * read-only: does the chip itself say DP alt mode is granted on a PHY?
 *
 * WHY THIS EXISTS NOW, AND WHY 2C0's REFUSAL NO LONGER HOLDS.
 * 2BF named this command as the way to identify the USB-C PHY without any
 * index-convention inference, and 2C0 then refused to build it: 2zr had
 * measured this firmware NOT answering an inbox query (boot 275's
 * QUERY_FEATURE_CAPS came back byte-identical), so a sweep would reproduce a
 * known failure. **That reading was taken at CW4.** 2DA identified the ring
 * as REGION4 on two independent quantities agreeing on 37, and 2E9 proved a
 * command sent THERE executes - DIG_ENABLE 0 -> 1 on the targeted instance
 * alone. So the reply path has never been exercised at the right address, and
 * 2C0's "it would reproduce a known failure" describes a different address.
 * Rule 39 in reverse: a null taken in a configuration that cannot exhibit the
 * effect does not establish the effect's absence.
 *
 * WHAT IT DECIDES, which is the whole external-display line.
 * PHY2's RDPCSTX2_PHY_CNTL6 reads 0x00063333 - DPALT_DISABLE **asserted and
 * acked** (2C1, reproduced on boots 443/444/447/448) - and
 * dcn31_link_encoder_is_in_alt_mode() returns `disable == 0`, so by that
 * register DP is NOT granted on this PHY and nothing we write can light it.
 * Against that: 2BB read a full DPCD and 2BD a full EDID over AUX2, and on
 * Type-C the AUX pair only exists as AUX once DP alt mode is negotiated, so
 * the CONNECTOR is in alt mode. Both readings are solid and they disagree.
 * Two candidate resolutions, and this command settles which:
 *   (a) the register is right, the PD controller has granted only USB, and
 *       the external display is a firmware/EC matter like 2zw's MUX and 2D8's
 *       dead Type-C - rule 47 for the third time on this chassis;
 *   (b) the register is the WRONG ONE for this part. Linux reads
 *       RDPCSPIPE_PHY_CNTL6 instead of RDPCSTX_PHY_CNTL6 for UNIPHY_C on
 *       YELLOW_CARP_B0 ("B0 phys use a new set of registers"), and whether
 *       this chip is B0 (hw_internal_rev 0x20) is NOT established - our PCI
 *       rev 0x0a is a different field (2BE). RDPCSPIPE is not a way out
 *       either: dpcs_4_2_0_offset.h gives PIPE0/2/4 the SAME offset 0x2d73
 *       and PIPE1/3 the same 0x2e4b, so "RDPCSPIPE2" IS PIPE0's address and
 *       reading it would be 2DD's trap - a plausible answer from a register
 *       that is not the instance asked about.
 * The chip's own answer goes around the whole question, which is 2B9's lesson
 * (read the field that names the fact) applied where no register can.
 *
 * THE CONTROL IS THE ROM, AND IT IS AN INDEPENDENT SOURCE.
 * 2E1 parsed firmware/680m-vbios.rom and read atom_encoder_caps_record bit
 * 0x100, DP_IS_USB_C: **set on display paths 2/3/4, clear on 0/1**. So a
 * live reply must give is_usb = 0,0,1,1,1 across phy_id 0..4. That is four
 * bits of agreement with a source that is not this command and was not
 * derived from it - if the pattern matches, the reply path is verified, and
 * if is_usb is UNIFORM across all five it is a refusal, which is 2BF's own
 * stated control ("the eDP panel is not USB-C, so all-set or all-clear is a
 * refusal") rather than one invented here.
 *
 * THE ONE DELIBERATE DEVIATION FROM LINUX, because without it the null is
 * uninterpretable. query_dp_alt_from_dmub() memsets the command to 0 and
 * sends phy_id in; is_usb, is_dp_alt_disable and is_dp4 are OUT fields. If
 * the firmware does not answer, the slot comes back as we wrote it - three
 * zeros - which is INDISTINGUISHABLE from a real reply of "not USB, alt not
 * disabled, not DP4". So the three OUT bytes are pre-filled with 0xFF: not a
 * valid value for any of them, so "unchanged" is visibly unchanged and
 * "answered" is visibly 0 or 1 (rule 14 at the firmware boundary, and the
 * same move as 2BA's stuck-index check). It is a deviation, so it is
 * TESTABLE: rb_dpaltq_build() takes the fill byte and `dmubbl dpaltq zero`
 * sends Linux-exact zeros, whose own null is the ambiguous one.
 *   A first draft of this comment claimed the NO_REPLY branch "cannot fire"
 *   under the zero fill. **It fires, and the hosttest caught it**: the check
 *   is against `fill`, so with fill 0 a GENUINE reply of "not USB, alt not
 *   disabled, not DP4" is reported as NO_REPLY. The ambiguity does not go
 *   away under the zero fill - it only changes which label it wears, and
 *   the sentinel's value is that the label is then the TRUE one. A comment
 *   is not a test.
 *
 * READ-ONLY on the chip. Every field but phy_id is an output and the command
 * changes no hardware state; the only write is the inbox push 2E9 already
 * did safely. There is no setter here and no action byte - this header
 * cannot express anything but the query.
 */
#ifndef RB_DPALTQ_H
#define RB_DPALTQ_H

#include <stdint.h>
#include "rb_dmub.h"

#define RB_DMUB_CMD_VBIOS_QUERY_DP_ALT   26u

/* Linux sizes THIS payload off its own data struct -
 * `payload_bytes = sizeof(cmd->query_dp_alt.data)` = 4 - and not off the
 * union, which is why the transmitter command's RB_DIG_PAYLOAD is 60 (2E2).
 * Reproduced rather than normalised: the two commands really do declare
 * different payload sizes. */
#define RB_DPALTQ_PAYLOAD                 4u

#define RB_DPALTQ_OFF_PHYID               0u
#define RB_DPALTQ_OFF_IS_USB              1u
#define RB_DPALTQ_OFF_ALT_DISABLE         2u
#define RB_DPALTQ_OFF_IS_DP4              3u

#define RB_DPALTQ_PHY_COUNT               5u
#define RB_DPALTQ_FILL                    0xFFu   /* the sentinel */
#define RB_DPALTQ_FILL_LINUX              0x00u   /* byte-exact control */

/* Refusals, numbered clear of rb_pg's 20-25 and rb_dig's 30-37 so a code is
 * never ambiguous about which surface produced it (rule 14). */
enum {
    RB_DPALTQ_OK            = 0,
    RB_DPALTQ_NOT_ARMED     = 50,
    RB_DPALTQ_BAD_PHY       = 51,
    RB_DPALTQ_PUSH_FAILED   = 52,
    RB_DPALTQ_NO_REPLY      = 53,   /* OUT bytes still the fill byte      */
    RB_DPALTQ_BAD_REPLY     = 54,   /* a byte that is neither 0 nor 1     */
    RB_DPALTQ_PHY_ECHO      = 55    /* phy_id came back as another phy    */
};

struct rb_dpaltq_reply {
    uint32_t phy_id;
    uint32_t is_usb;
    uint32_t is_dp_alt_disable;
    uint32_t is_dp4;
};

static inline void rb_dpaltq_build(uint8_t *buf, uint32_t phy_id,
                                   uint8_t fill)
{
    uint32_t i;

    if (!buf) { return; }
    for (i = 0; i < RB_DMUB_CMD_SIZE; i++) { buf[i] = 0u; }

    rb_dmub_put32(buf, 0, rb_dmub_hdr(RB_DMUB_CMD_VBIOS,
                                      RB_DMUB_CMD_VBIOS_QUERY_DP_ALT,
                                      RB_DPALTQ_PAYLOAD) |
                          RB_DMUB_HDR_RET_STATUS);

    buf[4u + RB_DPALTQ_OFF_PHYID]      = (uint8_t)phy_id;
    buf[4u + RB_DPALTQ_OFF_IS_USB]     = fill;
    buf[4u + RB_DPALTQ_OFF_ALT_DISABLE]= fill;
    buf[4u + RB_DPALTQ_OFF_IS_DP4]     = fill;
}

/* RET_STATUS: dc_dmub_srv_query_caps_cmd sets the bit before sending and
 * accepts the reply only if the firmware CLEARED it (rb_dmub.h:162). Exposed
 * separately rather than folded into the verdict, because a cleared bit with
 * unchanged payload bytes and a set bit with real payload bytes are two
 * different findings about the reply path. */
static inline int rb_dpaltq_ret_cleared(const uint8_t *got)
{
    if (!got) { return 0; }
    return rb_dmub_hdr_ret(rb_dmub_get32(got, 0)) == 0u;
}

static inline int rb_dpaltq_decode(const uint8_t *got, uint32_t phy_sent,
                                   uint8_t fill, struct rb_dpaltq_reply *out)
{
    uint8_t u, d, f;

    if (!got || !out) { return RB_DPALTQ_BAD_REPLY; }

    out->phy_id            = got[4u + RB_DPALTQ_OFF_PHYID];
    out->is_usb            = got[4u + RB_DPALTQ_OFF_IS_USB];
    out->is_dp_alt_disable = got[4u + RB_DPALTQ_OFF_ALT_DISABLE];
    out->is_dp4            = got[4u + RB_DPALTQ_OFF_IS_DP4];

    u = got[4u + RB_DPALTQ_OFF_IS_USB];
    d = got[4u + RB_DPALTQ_OFF_ALT_DISABLE];
    f = got[4u + RB_DPALTQ_OFF_IS_DP4];

    /* All three still the sentinel: the firmware wrote nothing. Checked
     * BEFORE the 0/1 test so "no reply" keeps its own code - with the Linux
     * fill of 0 this branch cannot fire, which is exactly why the sentinel
     * run is the instrument and the zero run is its control. */
    if (u == fill && d == fill && f == fill) { return RB_DPALTQ_NO_REPLY; }
    /* Under the zero fill this is NOT a safe statement - a real all-zero
     * reply lands here too - which is the control run's known defect and
     * the reason the sentinel run is the one to read. */

    if (u > 1u || d > 1u || f > 1u) { return RB_DPALTQ_BAD_REPLY; }

    /* phy_id is an IN field, so it is NOT evidence of a reply on its own -
     * it stays what we wrote even when nothing answers. It is checked only
     * to catch a reply the firmware wrote for a DIFFERENT phy, which would
     * mean the slot we read is not the slot our command went into. */
    if (out->phy_id != phy_sent) { return RB_DPALTQ_PHY_ECHO; }

    return RB_DPALTQ_OK;
}

/* ---- the sweep verdict ----
 *
 * 2E1's ROM read is the expectation: DP_IS_USB_C set on paths 2/3/4, clear
 * on 0/1. Stated as a mask so the hosttest pins it against the journal
 * rather than against this file's own prose. */
#define RB_DPALTQ_ROM_USB_MASK   0x1Cu   /* phys 2,3,4 (2E1) */

enum {
    RB_DPALTQ_V_NONE      = 0,  /* nothing answered                        */
    RB_DPALTQ_V_UNIFORM   = 1,  /* same is_usb on all five - 2BF's refusal */
    RB_DPALTQ_V_DISAGREES = 2,  /* answered, pattern != the ROM's          */
    RB_DPALTQ_V_ROM_MATCH = 3   /* answered AND matches 2E1's ROM read     */
};

static inline uint32_t rb_dpaltq_usb_mask(const struct rb_dpaltq_reply *r,
                                          const uint32_t *refuse)
{
    uint32_t m = 0u;
    uint32_t i;

    if (!r || !refuse) { return 0u; }
    for (i = 0u; i < RB_DPALTQ_PHY_COUNT; i++) {
        if (refuse[i] == RB_DPALTQ_OK && r[i].is_usb) { m |= (1u << i); }
    }
    return m;
}

static inline int rb_dpaltq_verdict(const struct rb_dpaltq_reply *r,
                                    const uint32_t *refuse)
{
    uint32_t answered = 0u;
    uint32_t usb = 0u;
    uint32_t i;

    if (!r || !refuse) { return RB_DPALTQ_V_NONE; }

    for (i = 0u; i < RB_DPALTQ_PHY_COUNT; i++) {
        if (refuse[i] == RB_DPALTQ_OK) { answered |= (1u << i); }
    }
    if (answered == 0u) { return RB_DPALTQ_V_NONE; }

    usb = rb_dpaltq_usb_mask(r, refuse);

    /* 2BF's own control, and it must be judged over the phys that ANSWERED:
     * all-set or all-clear across the answered set carries no information,
     * because the panel's PHY is known not to be USB-C. A single answered
     * phy is therefore uniform by construction and refuses too. */
    if (usb == 0u || usb == answered) { return RB_DPALTQ_V_UNIFORM; }

    if (answered == ((1u << RB_DPALTQ_PHY_COUNT) - 1u) &&
        usb == RB_DPALTQ_ROM_USB_MASK) {
        return RB_DPALTQ_V_ROM_MATCH;
    }
    return RB_DPALTQ_V_DISAGREES;
}

/* The question the sweep exists to answer, kept separate from the
 * reply-path verdict above: is DP granted on the Type-C PHY right now?
 * Only meaningful once the sweep is at least not NONE/UNIFORM, so the
 * caller must check the verdict first - this returns -1 when it cannot say
 * rather than a plausible 0 or 1 (rule 14). */
static inline int rb_dpaltq_dp_granted(const struct rb_dpaltq_reply *r,
                                       const uint32_t *refuse, uint32_t phy)
{
    if (!r || !refuse || phy >= RB_DPALTQ_PHY_COUNT) { return -1; }
    if (refuse[phy] != RB_DPALTQ_OK) { return -1; }
    if (!r[phy].is_usb) { return -1; }   /* not a USB-C PHY: not the question */
    return r[phy].is_dp_alt_disable ? 0 : 1;
}

#endif /* RB_DPALTQ_H */
