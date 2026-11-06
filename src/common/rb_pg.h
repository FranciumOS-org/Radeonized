/* rb_pg.h - the DCN display power-gating DOMAIN blocks, READ-ONLY.
 *
 * 2DC. Built as the readback half of the one remaining test of whether this
 * firmware EXECUTES a DMUB inbox command. 2DB retired every other
 * instrument: the caps query is unanswered at both candidate addresses and an
 * unimplemented command 6 answers nowhere; an ack is RPTR advancing over
 * whatever the slot held; the ring read-back only says our bytes stuck; and
 * the ABM backlight is a no-op whenever ABM was never initialised, which is
 * the expected state on a machine whose driver does not use DMUB ABM. 2D9's
 * finding is what is left: five of the seven DMUB_CMD__VBIOS sub-types have
 * no reply at all and their product is a REGISTER CHANGE, so the test is a
 * register.
 *
 * WHY DOMAIN_CONTROL AND NOT THE TRANSMITTER. The transmitter command looked
 * better - it is the first real bring-up step for the external display - and
 * it is NOT writable from what is in the tree. Its payload comes in two
 * layouts, and `command_table2.c:init_transmitter_control` chooses between
 * them on the **atom BIOS command-table revision read from ROM** (crev 6 ->
 * v1_6, crev 7 -> v1_7), not on the DCN version; this project cannot read
 * that ROM. Its `action` field is `ATOM_TRANSMITER_ACTION_xxx`, and those
 * values appear in `dmub_cmd.h` only as the words of a comment - nothing
 * vendored defines them. Writing it would mean recalling two constants and a
 * struct layout, which is 2BA's "every constant is read, not recalled" and
 * rule 17's mirror. DOMAIN_CONTROL's payload, by contrast, is defined
 * completely in the vendored header (`inst:6`, `power_gate:1`,
 * `reserved[3]`), it is a DMUB-native command with no atom table behind it,
 * and its effect has a status register.
 *
 * Offsets from docs/linux-ref/dcn_3_1_2_offset.h, fields from
 * dcn_3_1_2_sh_mask.h, both vendored:
 *
 *   regDOMAIN0_PG_CONFIG 0x0080  BASE_IDX 2     stride 2 dwords
 *   regDOMAIN0_PG_STATUS 0x0081  BASE_IDX 2
 *   ... DOMAIN1 0x0082/0x0083, DOMAIN2 0x0084/0x0085, DOMAIN3 0x0086/0x0087
 *
 * BASE_IDX 2 is the base the OTG, HUBP, HPD and AUX reads already come from,
 * and dword 0x80 is FAR below rb_hpd_dw_max(), so this adds **no mapping, no
 * aperture growth and no write surface**. There is no setter here.
 *
 * THE STRIDE DOES NOT CONTINUE. regDOMAIN16_PG_CONFIG is 0x0089, not 0x0088,
 * so DOMAIN16/17/18 are NOT reachable by extending the loop - 0x88 is
 * something else. Only DOMAIN0..3 are exposed, which are the four pipes, and
 * those are what this question needs. Do not raise RB_PG_DOMAIN_COUNT.
 *
 * THE POLARITY OF DOMAIN_PGFSM_PWR_STATUS IS DELIBERATELY NOT WRITTEN DOWN.
 * Linux's wait on it lives in dcn20_hwseq.c, which is NOT vendored, so
 * whether the field reads 0 or 2 for "powered" cannot be established from
 * what is in the tree - and recalling it is how a backwards verdict gets
 * printed with confidence. It is not needed: **DOMAIN0 is the pipe driving
 * the panel right now**, so whatever DOMAIN0's field reads IS "powered", and
 * every other domain is classified by comparing against it. That is 2B8's
 * reference-instance method - the firmware brought one instance up on this
 * die, so one instance is in a known-good state - applied before the fact
 * rather than after.
 *
 * And the verdict that matters is a DIFFERENCE, not an absolute: the test is
 * the same four words read before and after one DOMAIN_CONTROL command. A
 * changed word means the firmware executed it. Nothing about the polarity
 * enters that. */

#ifndef RB_PG_H
#define RB_PG_H

#include <stdint.h>

/* dcn_3_1_2_offset.h, BASE_IDX 2, dword offsets. */
#define RB_PG_DW_DOMAIN0_PG_CONFIG   0x0080u
#define RB_PG_DW_DOMAIN0_PG_STATUS   0x0081u
#define RB_PG_DOMAIN_STRIDE          0x0002u

/* DOMAIN0..3 only - see the stride note above. */
#define RB_PG_DOMAIN_COUNT           4u

/* The pipe whose domain is known powered, because it is scanning the panel. */
#define RB_PG_DOMAIN_REFERENCE       0u

/* dcn_3_1_2_sh_mask.h */
#define RB_PG_STATUS_PGFSM_SHIFT     0x1eu
#define RB_PG_STATUS_PGFSM_MASK      0xc0000000u
#define RB_PG_STATUS_DESIRED_SHIFT   0x1cu
#define RB_PG_STATUS_DESIRED_MASK    0x10000000u
#define RB_PG_CONFIG_FORCEON_MASK    0x00000001u
#define RB_PG_CONFIG_GATE_MASK       0x00000100u

static inline uint32_t rb_pg_config_dw(uint32_t inst)
{
    return RB_PG_DW_DOMAIN0_PG_CONFIG + inst * RB_PG_DOMAIN_STRIDE;
}

static inline uint32_t rb_pg_status_dw(uint32_t inst)
{
    return RB_PG_DW_DOMAIN0_PG_STATUS + inst * RB_PG_DOMAIN_STRIDE;
}

/* The highest dword this file reads, so an aperture bound can be checked
 * against it rather than against a number copied by hand. */
static inline uint32_t rb_pg_dw_max(void)
{
    return rb_pg_status_dw(RB_PG_DOMAIN_COUNT - 1u);
}

static inline uint32_t rb_pg_fsm(uint32_t status)
{
    return (status & RB_PG_STATUS_PGFSM_MASK) >> RB_PG_STATUS_PGFSM_SHIFT;
}

static inline uint32_t rb_pg_desired(uint32_t status)
{
    return (status & RB_PG_STATUS_DESIRED_MASK) >> RB_PG_STATUS_DESIRED_SHIFT;
}

static inline int rb_pg_forced_on(uint32_t config)
{
    return (config & RB_PG_CONFIG_FORCEON_MASK) ? 1 : 0;
}

static inline int rb_pg_gate_requested(uint32_t config)
{
    return (config & RB_PG_CONFIG_GATE_MASK) ? 1 : 0;
}

/* Is this domain in the same power state as the reference domain?
 *
 * The ONLY statement this file makes about power, and it is relative. 1 means
 * "the same state as the pipe that is scanning the panel", i.e. powered. 0
 * means a different state. No constant for "on" appears anywhere. */
static inline int rb_pg_like_reference(uint32_t status, uint32_t refStatus)
{
    return rb_pg_fsm(status) == rb_pg_fsm(refStatus) ? 1 : 0;
}

/* Did one DOMAIN_CONTROL command change anything?
 *
 * The whole verdict of 2DC. `before` and `after` are the four status words
 * read either side of the send. A changed word is the firmware executing an
 * inbox command, which no other instrument this project has can show.
 *
 * Returns a BITMASK of the domains that moved, so "which one" is answerable
 * and a count cannot hide two changes cancelling (rule 14: a last-wins
 * summary cannot express a per-entry fact). */
static inline uint32_t rb_pg_moved_mask(const uint32_t *before,
                                        const uint32_t *after)
{
    uint32_t i, m = 0u;

    if (!before || !after) { return 0u; }
    for (i = 0; i < RB_PG_DOMAIN_COUNT; i++) {
        if (before[i] != after[i]) { m |= (1u << i); }
    }
    return m;
}

/* Which domain is worth asking the firmware to power up.
 *
 * It must NOT be the reference - that one is powering the live panel, and a
 * command naming it is the one command in this family that could cost the
 * display. And it must currently differ from the reference, or a power-up
 * request is a no-op whose null would mean nothing (rule 39: a measurement
 * taken where the effect cannot appear does not locate anything).
 *
 * Returns the instance, or -1 when every domain already matches the
 * reference - in which case this test is NOT AVAILABLE on this boot and must
 * say so rather than picking one anyway. */
static inline int rb_pg_candidate(const uint32_t *status)
{
    uint32_t i;

    if (!status) { return -1; }
    for (i = 0; i < RB_PG_DOMAIN_COUNT; i++) {
        /* The reference is skipped explicitly, but note that it could never
         * be returned anyway: rb_pg_like_reference(ref, ref) is 1 by
         * construction. The hosttest asserts THAT property instead of
         * asserting this branch, because a test of this `continue` passes
         * whether or not it is there - a case that passes for the wrong
         * reason is not coverage (2BC). The real protection for the panel's
         * domain against a HAND-NAMED instance is rb_pg_sel_refuse(). */
        if (i == RB_PG_DOMAIN_REFERENCE) { continue; }
        if (!rb_pg_like_reference(status[i], status[RB_PG_DOMAIN_REFERENCE])) {
            return (int)i;
        }
    }
    return -1;
}

/* Refuse a domain instance a caller named by hand. The reference is refused
 * for the reason above; everything outside the exposed block is refused
 * because its offset is not derivable from this file's stride. */
#define RB_PG_SEL_OK          0u
#define RB_PG_SEL_OUT_OF_RANGE 1u
#define RB_PG_SEL_IS_REFERENCE 2u

static inline uint32_t rb_pg_sel_refuse(uint32_t inst)
{
    if (inst >= RB_PG_DOMAIN_COUNT) { return RB_PG_SEL_OUT_OF_RANGE; }
    if (inst == RB_PG_DOMAIN_REFERENCE) { return RB_PG_SEL_IS_REFERENCE; }
    return RB_PG_SEL_OK;
}

/* 2DD: is a SAMPLE of this block usable at all?
 *
 * Boot 436 read all four STATUS and all four CONFIG words as 0x00000000,
 * 685 samples deep, and the auto-candidate guard refused with code 24 - "no
 * domain differs from DOMAIN0" - which was a TRUE statement about eight zero
 * words and a false statement about the chip. The cause was one missing
 * term: every DCN register in this kext is addressed as
 * `hubpRegs[kRbDmuSeg2 + dword]` (0x34c0, the DMU segment-2 base), and 2DC's
 * pgSample() indexed `hubpRegs[dword]`. 0x80..0x87 is well INSIDE the
 * mapping, so there is no fault and no error - it reads some other block's
 * zeros, which is rule 14's shape at the hardware boundary: a wrong address
 * that happens to be readable answers plausibly and says nothing.
 *
 * So the plausibility check cannot be about polarity (deliberately not
 * recorded - see the header comment) and cannot be about any one field. What
 * it can be is this: DOMAIN0 is the pipe scanning the live panel, so SOMETHING
 * in this block must be non-zero while the desktop is up. Eight identically
 * zero words is the signature of reading the wrong dwords, and the experiment
 * refuses on it rather than producing a verdict from them.
 *
 * It is not a proof of correct addressing - a block of plausible-looking
 * garbage would pass - which is why the raw words are published beside every
 * verdict and why the reference-instance method compares against DOMAIN0
 * rather than against any written-down constant. */
static inline int rb_pg_sample_readable(const uint32_t *status,
                                        const uint32_t *config)
{
    uint32_t i;
    uint32_t any = 0u;

    if (!status || !config) { return 0; }
    for (i = 0; i < RB_PG_DOMAIN_COUNT; i++) {
        any |= status[i];
        any |= config[i];
    }
    return any != 0u ? 1 : 0;
}

#endif /* RB_PG_H */
