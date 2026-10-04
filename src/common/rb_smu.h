/* SPDX-License-Identifier: MIT */
/*
 * Read-only queries to the SMU (MP1 power-management firmware) of a
 * Rembrandt-class APU, through the driver mailbox Linux uses:
 * MP1_C2PMSG_66 message, _82 argument, _90 response (smu_v13_0_init_msg_ctl),
 * with smu_msg_v1_send_msg's protocol (pm/swsmu/smu_cmn.c):
 *
 *   pre-poll   wait for response != 0 (the previous message finished)
 *   send       response = 0, argument = arg, message = index
 *   post-poll  wait for response != 0; 1 = OK, then read the output from
 *              the argument register
 *
 * Stricter than Linux:
 *   - only the messages in rb_smu.c's allowlist, which all just report state
 *     (version, interface version, enabled-feature mask);
 *   - nothing is sent unless the response register reads exactly 1 (OK, idle);
 *   - the first message that does not come back OK stops the sequence;
 *   - the only registers written are those three mailbox registers, resolved
 *     from the discovery table for an MP1 of major version 13; any other write
 *     is refused before it reaches rb_hw.
 *
 * Message numbers: pm/swsmu/inc/pmfw_if/smu_v13_0_1_ppsmc.h (yellow_carp_ppt.c
 * uses it for MP1 13.0.1, 13.0.3 and 13.0.8).
 */
#ifndef RB_SMU_H
#define RB_SMU_H

#include <stdint.h>
#include "rb_discovery.h"
#include "rb_probe.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RB_SMU_MSG_GetSmuVersion          0x02
#define RB_SMU_MSG_GetDriverIfVersion     0x03
#define RB_SMU_MSG_GetEnabledSmuFeatures  0x12

/* PPSMC_Result_* */
#define RB_SMU_RESP_NONE          0x00
#define RB_SMU_RESP_OK            0x01
#define RB_SMU_RESP_FAILED        0xFF
#define RB_SMU_RESP_UNKNOWN_CMD   0xFE
#define RB_SMU_RESP_REJ_PREREQ    0xFD
#define RB_SMU_RESP_REJ_BUSY      0xFC

/* Linux: adev->usec_timeout (100000) * 20, polled at 1 us. */
#define RB_SMU_TIMEOUT_US 2000000u

enum rb_smu_xfer_status {
    RB_SMU_X_OK = 0,
    RB_SMU_X_PRE_BUSY,      /* pre-poll: response stayed 0 */
    RB_SMU_X_TIMEOUT,       /* sent; no response within the timeout */
    RB_SMU_X_FW_ERROR,      /* sent; response was a failure code */
};

struct rb_smu_xfer {
    uint8_t  msg;
    uint32_t arg;
    uint8_t  status;        /* enum rb_smu_xfer_status */
    uint32_t resp;          /* response register after the post-poll */
    uint32_t out;           /* argument register after an OK response */
    uint32_t polls;         /* 1 us polls the post-poll took */
};

#define RB_SMU_MAX_XFERS 4

struct rb_smu_result {
    const char *fail;       /* why the sequence stopped, or NULL */
    uint32_t mp1_major, mp1_minor, mp1_rev;
    uint32_t dw_msg, dw_arg, dw_resp;
    uint32_t initial_msg, initial_arg, initial_resp;

    uint32_t nxfers;
    struct rb_smu_xfer x[RB_SMU_MAX_XFERS];

    uint8_t  have_version, have_if_version, have_features;
    uint32_t smu_version;   /* program << 24 | major << 16 | minor << 8 | debug */
    uint32_t if_version;
    uint64_t features;      /* FEATURE_*_BIT of smu_v13_0_1_pmfw.h */

    uint32_t writes;
    uint32_t refused_writes;   /* must stay 0 */
};

/* Run the query sequence. `d` is a parsed discovery table (rb_probe_run's).
 * hw->delay_us must be set. timeout_us 0 means RB_SMU_TIMEOUT_US. Returns 0
 * if every query came back OK, -1 otherwise (r->fail says why). */
int rb_smu_query(const struct rb_hw *hw, const struct rb_disc *d,
                 uint32_t timeout_us, struct rb_smu_result *r);

#ifdef __cplusplus
}
#endif

#endif /* RB_SMU_H */
