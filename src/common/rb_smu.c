/* SPDX-License-Identifier: MIT */
/* Read-only SMU queries. See rb_smu.h. */
#include "rb_smu.h"

/* MP1_SMN_C2PMSG_66 / _82 / _90, base index 0 (mp_13_0_2_offset.h). */
#define MP1_C2PMSG_66 0x0282
#define MP1_C2PMSG_82 0x0292
#define MP1_C2PMSG_90 0x029a

static void zero(void *p, uint32_t n)
{
    volatile uint8_t *b = (volatile uint8_t *)p;
    for (uint32_t i = 0; i < n; i++)
        b[i] = 0;
}

static uint32_t rd(const struct rb_hw *hw, uint32_t dw)
{
    return hw->rreg(hw->ctx, dw * 4);
}

/* The write policy: the three mailbox registers, nothing else. */
static void wr(const struct rb_hw *hw, struct rb_smu_result *r, uint32_t dw, uint32_t v)
{
    if (dw != r->dw_msg && dw != r->dw_arg && dw != r->dw_resp) {
        r->refused_writes++;
        return;
    }
    r->writes++;
    hw->wreg(hw->ctx, dw * 4, v);
}

/* __smu_msg_v1_poll_stat: until the response is non-zero or time runs out. */
static uint32_t poll_resp(const struct rb_hw *hw, const struct rb_smu_result *r,
                          uint32_t timeout_us, uint32_t *polls)
{
    uint32_t v = 0, n;
    for (n = 0; n < timeout_us; n++) {
        v = rd(hw, r->dw_resp);
        if (v != RB_SMU_RESP_NONE)
            break;
        hw->delay_us(hw->ctx, 1);
    }
    if (polls)
        *polls = n;
    return v;
}

static int known_resp(uint32_t v)
{
    return v == RB_SMU_RESP_OK || v == RB_SMU_RESP_FAILED || v == RB_SMU_RESP_UNKNOWN_CMD ||
           v == RB_SMU_RESP_REJ_PREREQ || v == RB_SMU_RESP_REJ_BUSY;
}

/* One message; returns 0 if it came back OK. */
static int xfer(const struct rb_hw *hw, struct rb_smu_result *r, uint32_t timeout_us,
                uint8_t msg, uint32_t arg, uint32_t *out)
{
    struct rb_smu_xfer *x = &r->x[r->nxfers++];
    uint32_t pre;
    x->msg = msg;
    x->arg = arg;

    pre = poll_resp(hw, r, timeout_us, 0);
    if (pre == RB_SMU_RESP_NONE || !known_resp(pre)) {
        x->status = RB_SMU_X_PRE_BUSY;
        x->resp = pre;
        r->fail = "SMU mailbox busy or in an unknown state before sending";
        return -1;
    }

    wr(hw, r, r->dw_resp, 0);
    wr(hw, r, r->dw_arg, arg);
    wr(hw, r, r->dw_msg, msg);

    x->resp = poll_resp(hw, r, timeout_us, &x->polls);
    if (x->resp == RB_SMU_RESP_NONE) {
        x->status = RB_SMU_X_TIMEOUT;
        r->fail = "SMU did not answer within the timeout";
        return -1;
    }
    if (x->resp != RB_SMU_RESP_OK) {
        x->status = RB_SMU_X_FW_ERROR;
        r->fail = "SMU answered with an error";
        return -1;
    }
    x->out = rd(hw, r->dw_arg);
    *out = x->out;
    return 0;
}

int rb_smu_query(const struct rb_hw *hw, const struct rb_disc *d,
                 uint32_t timeout_us, struct rb_smu_result *r)
{
    const struct rb_ip *mp1;
    uint32_t lo = 0, hi = 0;

    if (!r)
        return -1;
    zero(r, sizeof(*r));
    if (!hw || !hw->rreg || !hw->wreg || !hw->delay_us || !d) {
        r->fail = "no hardware access";
        return -1;
    }
    if (!timeout_us)
        timeout_us = RB_SMU_TIMEOUT_US;

    mp1 = rb_disc_find(d, RB_HWID_MP1, 0);
    if (!mp1 || mp1->nbase < 1) {
        r->fail = "no MP1 in the discovery table";
        return -1;
    }
    r->mp1_major = mp1->major;
    r->mp1_minor = mp1->minor;
    r->mp1_rev = mp1->revision;
    if (mp1->major != 13) {
        r->fail = "MP1 is not version 13: mailbox layout unknown";
        return -1;
    }
    r->dw_msg = mp1->base[0] + MP1_C2PMSG_66;
    r->dw_arg = mp1->base[0] + MP1_C2PMSG_82;
    r->dw_resp = mp1->base[0] + MP1_C2PMSG_90;
    if (r->dw_msg >= hw->mmio_len / 4 || r->dw_arg >= hw->mmio_len / 4 ||
        r->dw_resp >= hw->mmio_len / 4) {
        r->fail = "SMU mailbox outside the register BAR";
        return -1;
    }

    r->initial_msg = rd(hw, r->dw_msg);
    r->initial_arg = rd(hw, r->dw_arg);
    r->initial_resp = rd(hw, r->dw_resp);
    if (r->initial_resp != RB_SMU_RESP_OK) {
        r->fail = "SMU mailbox not idle (response is not 1): nothing sent";
        return -1;
    }

    if (xfer(hw, r, timeout_us, RB_SMU_MSG_GetSmuVersion, 0, &r->smu_version))
        return -1;
    r->have_version = 1;
    if (xfer(hw, r, timeout_us, RB_SMU_MSG_GetDriverIfVersion, 0, &r->if_version))
        return -1;
    r->have_if_version = 1;
    /* smu_cmn_get_enabled_mask: argument 0 low word, 1 high word. */
    if (xfer(hw, r, timeout_us, RB_SMU_MSG_GetEnabledSmuFeatures, 0, &lo))
        return -1;
    if (xfer(hw, r, timeout_us, RB_SMU_MSG_GetEnabledSmuFeatures, 1, &hi))
        return -1;
    r->features = ((uint64_t)hi << 32) | lo;
    r->have_features = 1;
    return 0;
}
