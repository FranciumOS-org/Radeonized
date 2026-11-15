/* 3ext-e: why this machine has no Apple framebuffer, as a count.
 *
 * Each case pins something the binary settled and a comment or a journal draft
 * got wrong at least once, or something only a test can hold:
 *  - 0xd8 / 0x24 = 6 is the ONE arithmetic relationship the whole layout
 *    derivation rests on, so it is asserted rather than remarked on;
 *  - 0 is SUCCESS on this path (IOReturn), not failure - a first draft of
 *    3ext-d had createFbStubs failing on a zero count, and it does not;
 *  - the readability codes are codes, and "never ran" is not "unreadable";
 *  - the verdict checks its CONTROLS before any positive finding: the two
 *    parser answers disagreeing outranks either of them being zero;
 *  - a text pointer is not a heap pointer. Accepting anything above the kalloc
 *    floor as a function address is how a wild vtable slot gets called;
 *  - the n sanity bound is against OUR buffer, not against Apple's seed.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "../../src/common/rb_fbstubs.h"

static int fails;
static void ck(int cond, const char *what) {
    if (!cond) { printf("FAIL fbstubs: %s\n", what); fails++; }
}

int fbstubs_tests(void);

int fbstubs_tests(void) {
    fails = 0;

    /* ---- the layout closes on itself ---------------------------------- */
    ck(RB_FBSTUBS_BUFSZ == RB_FBSTUBS_DESC_STRIDE * RB_FBSTUBS_SLOTS,
       "the buffer size is derived from the stride, not written as a number");
    /* Stated as the PRODUCT, not the quotient. A first version of this case
     * divided, and 0xd8 / 0x20 is also 6 under integer division, so a wrong
     * stride passed it - a check that passes for the wrong reason is not
     * coverage, and only the break-it sweep found that. */
    ck(RB_FBSTUBS_DESC_STRIDE * RB_FBSTUBS_SEED == 0xd8u,
       "0x24 * 6 = 0xd8 EXACTLY: the descriptor stride times Apple's seed is "
       "the size it bzero's - the one relationship the layout derivation rests "
       "on, and the quotient form does not pin it");
    ck(RB_FBSTUBS_OFF_LINKCNT + 8u == RB_FBSTUBS_OFF_LINKARR,
       "the link count byte sits immediately before the link array");
    ck(RB_FBSTUBS_OFF_STUBARR + 8u * RB_FBSTUBS_ARR_SLOTS
           == RB_FBSTUBS_OFF_LINKCNT,
       "the stub array's 12 slots run 0xc0..0x118 and end exactly at the count "
       "byte - how initializeInternals' zeroing confirmed both");
    ck(RB_FBSTUBS_VT_CONNCOUNT + 8u == RB_FBSTUBS_VT_CONNLIST,
       "getConnectorCount and getConnectorList are adjacent vtable slots");
    ck(RB_FBSTUBS_SLOTS > RB_FBSTUBS_SEED,
       "our buffer has slack above Apple's seed");
    ck(rb_fbstubs_seed() == RB_FBSTUBS_SEED,
       "we pass Apple's seed, not our capacity - the call must behave as the "
       "real one does");

    /* ---- 0 is SUCCESS on this path ------------------------------------ */
    ck(RB_FBSTUBS_IORET_OK == 0u,
       "createLinks and createFbStubs return IOReturn: 0 is kIOReturnSuccess, "
       "and `start` does testl/je INTO the success path");
    ck(RB_FBSTUBS_IORET_LINKERR != RB_FBSTUBS_IORET_OK,
       "createLink's failure code is distinct from success");

    /* ---- pointer floors: a text address is not a heap address --------- */
    ck(rb_fbstubs_obj_ok(0xffffff8012345678ULL), "a kalloc'd object passes");
    ck(!rb_fbstubs_obj_ok(0ULL), "NULL is refused");
    ck(!rb_fbstubs_obj_ok(0xffffff7f80001000ULL),
       "a kext TEXT address is not an object pointer");
    ck(!rb_fbstubs_obj_ok(0xffffff8012345671ULL),
       "an unaligned pointer is refused - a NULL check is not a pointer check");
    ck(rb_fbstubs_text_ok(0xffffff7f80001000ULL), "a kext symbol passes as text");
    ck(!rb_fbstubs_text_ok(0xffffff8012345678ULL),
       "a HEAP pointer is NOT accepted as a function address: this is the check "
       "that stops a wild vtable slot being called");
    ck(!rb_fbstubs_text_ok(0ULL), "NULL is not text");
    ck(RB_FBSTUBS_TEXT_FLOOR < RB_FBSTUBS_HEAP_FLOOR,
       "rule 19's two floors are ordered, and the text window is bounded above");

    /* ---- readability codes -------------------------------------------- */
    {
        const uint64_t ob = 0xffffff8011110000ULL;  /* an object        */
        const uint64_t vt = 0xffffff8022220000ULL;  /* its vtable       */
        const uint64_t fn = 0xffffff7f80003000ULL;  /* a text address   */
        ck(rb_fbstubs_why(1, 1, ob, vt, fn, fn) == RB_FBSTUBS_W_OK,
           "everything resolving is W_OK");
        ck(rb_fbstubs_why(0, 1, ob, vt, fn, fn) == RB_FBSTUBS_W_SELF,
           "a bad `this` is reported as such");
        ck(rb_fbstubs_why(1, 0, ob, vt, fn, fn) == RB_FBSTUBS_W_OSVER,
           "an unmeasured OS refuses rather than reading a wrong address");
        ck(rb_fbstubs_why(1, 1, 0, vt, fn, fn) == RB_FBSTUBS_W_BIOSP,
           "a NULL bios parser is its own code");
        ck(rb_fbstubs_why(1, 1, ob, 0, fn, fn) == RB_FBSTUBS_W_VTABLE,
           "a bad vtable is its own code");
        ck(rb_fbstubs_why(1, 1, ob, vt, 0, fn) == RB_FBSTUBS_W_SLOTS,
           "a bad conncount slot refuses");
        ck(rb_fbstubs_why(1, 1, ob, vt, fn, ob) == RB_FBSTUBS_W_SLOTS,
           "a connlist slot holding a HEAP address refuses - the case the text "
           "floor exists for");
        ck(rb_fbstubs_why(0, 0, 0, 0, 0, 0) == RB_FBSTUBS_W_SELF,
           "the order is fixed: the innermost failure is not reported when an "
           "outer one makes the read impossible");
    }

    /* ---- the verdict, controls first ---------------------------------- */
    ck(rb_fbstubs_verdict(0, RB_FBSTUBS_W_OK, 5, 0, 5, 5)
           == RB_FBSTUBS_V_NOT_RUN,
       "calls 0 outranks every other value: createLinks not running is not a "
       "statement about the parser (rule 14, and `calls` is seeded at route "
       "time so the zero is a reading)");
    ck(rb_fbstubs_verdict(1, RB_FBSTUBS_W_BIOSP, 5, 0, 5, 5)
           == RB_FBSTUBS_V_UNREADABLE,
       "it ran but nothing resolved - distinct from not running");
    /* The ORDER of the two control checks, which is the case a boot where
     * createLinks never runs actually produces: nothing was read, so `why`
     * holds whatever it was seeded with. "Never called" is the stronger
     * statement and must win. The first version of this file had no case where
     * both controls were tripped, so reordering them fired nothing - the
     * break-it sweep found that, not reasoning. */
    ck(rb_fbstubs_verdict(0, RB_FBSTUBS_W_BIOSP, 0, 0, 0, 0)
           == RB_FBSTUBS_V_NOT_RUN,
       "calls 0 WITH unreadable pointers is still NOT_RUN: when the hook never "
       "fired there was nothing to read, so unreadability says nothing");
    ck(RB_FBSTUBS_V_NOT_RUN != RB_FBSTUBS_V_UNREADABLE,
       "\"never called\" and \"called and unreadable\" are DIFFERENT codes");
    ck(rb_fbstubs_verdict(1, RB_FBSTUBS_W_OK, 5, 0, 0, 0)
           == RB_FBSTUBS_V_CONTAMINATED,
       "getConnectorCount 5 beside a listed n of 0 is a DISAGREEMENT, and it "
       "outranks concluding anything from the zero");
    ck(rb_fbstubs_verdict(1, RB_FBSTUBS_W_OK, 0, 0, 3, 3)
           == RB_FBSTUBS_V_CONTAMINATED,
       "the disagreement is symmetric - count 0 beside n 3 is equally unusable");
    ck(rb_fbstubs_verdict(1, RB_FBSTUBS_W_OK, 5, RB_FBSTUBS_IORET_LINKERR, 0, 0)
           == RB_FBSTUBS_V_LIST_FAILED,
       "a disagreement WITH an error return is attributed to the list call, "
       "not left as contamination");
    ck(rb_fbstubs_verdict(1, RB_FBSTUBS_W_OK, 5, 0xE00002C9u, 5, 0)
           == RB_FBSTUBS_V_LIST_FAILED,
       "an error return is reported even when the two answers agree");
    ck(rb_fbstubs_verdict(1, RB_FBSTUBS_W_OK, 0, 0, 0, 0)
           == RB_FBSTUBS_V_NO_CONNECTOR,
       "both answers zero and no error IS the live candidate: the parser has "
       "no connectors");
    ck(rb_fbstubs_verdict(1, RB_FBSTUBS_W_OK, 5, 0, 5, 0)
           == RB_FBSTUBS_V_LINKS_LOST,
       "connectors listed with no link created separates createLink from the "
       "parser");
    ck(rb_fbstubs_verdict(1, RB_FBSTUBS_W_OK, 5, 0, 5, 5)
           == RB_FBSTUBS_V_LINKS_MADE,
       "links existing means the premise of this whole experiment is wrong, "
       "which must be sayable");

    /* ---- the n bound is against OUR buffer --------------------------- */
    ck(rb_fbstubs_n_sane(RB_FBSTUBS_SLOTS),
       "a count filling our buffer exactly is sane");
    ck(!rb_fbstubs_n_sane(RB_FBSTUBS_SLOTS + 1u),
       "a count past our buffer is refused - the descriptors are not ours to "
       "read, and the bound is OUR capacity, not Apple's seed");
    ck(rb_fbstubs_n_sane(0u),
       "zero is sane: it is the expected answer, not an error");
    ck(rb_fbstubs_n_sane(RB_FBSTUBS_SEED),
       "Apple's seed is within our buffer");

    if (fails == 0) { printf("fbstubs: ok\n"); }
    return fails;
}
