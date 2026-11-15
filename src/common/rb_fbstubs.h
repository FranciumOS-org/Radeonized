/* rb_fbstubs.h - why this machine has no Apple framebuffer, read as a count.
 *
 * READ-ONLY. There is no setter in this file and no write anywhere in the
 * path it instruments. It classifies the readability of four pointers and
 * turns six numbers into a verdict; the kext does the reading.
 *
 * WHAT 3ext-c/3ext-d ESTABLISHED, AND WHY A COUNT IS NOW THE SUBJECT.
 * Boot 588 measured ZERO `AMDRadeonX6000_AmdRadeonFramebuffer` instances
 * class-wide in the promoted real-IOFramebuffer configuration, with
 * `AmdGpuWrangler` 1 and `IOResources` 1 as controls and WindowServer agreeing
 * independently (`CGXMappedDisplayStart: FB: 0 of 0 opened`, `No Display
 * drivers loaded`). Apple's framebuffer kext declares six IOKitPersonalities
 * and NOT ONE has `IOClass = AMDRadeonX6000_AmdRadeonFramebuffer`, so the
 * object is never produced by matching, in any category. One construction site
 * exists in the whole 9.5 MB binary:
 *
 *   AmdRadeonController::createFbStubs(unsigned char)      0xbf9fef0
 *     testb %r15b,%r15b / je   -> a zero count skips the entire loop
 *     operator new(0x9328) / ctor / initWithInfo(FbInitInfo&)
 *     movq %r13, 0xc0(%rbx,%r14,8)                         the stub array
 *
 * and one caller, `AmdRadeonController::start`, passing
 * `movzbl 0x120(%rbx)`. That byte is written by exactly one instruction in the
 * binary - an `incb`, inside
 *
 *   AmdRadeonController::createLinks()                     0xbfa0602
 *     movl  $0x6, (%rbx)              in/out count, seeded 6
 *     movq  0x7908(%r14), %rdi        AmdBiosParserHelper
 *     callq *0x190(%rax)              getConnectorList(unsigned int&, AmdLinkDescriptor*)
 *     cmpl  $0x0, (%rbx) / je         ZERO -> create nothing, return success
 *     callq AmdLink::createLink(LinkInitInfo const&)
 *     movq  %rax, 0x128(%r14,%rbx,8)  the LINK array
 *     incb  0x120(%r14)               one per link created
 *     addq  $0x24, %r15               descriptor stride
 *
 * so `+0x120` is the LINK count, `+0x128` the link array, and createFbStubs
 * makes one framebuffer stub PER LINK. The arithmetic closes on itself, which
 * is the reason to trust the layout rather than the names: the stack buffer is
 * bzero'd 0xd8 bytes, the stride is 0x24, and 0xd8 / 0x24 = 6 = the seed.
 *
 * TWO READINGS ARE ALREADY DEAD, FROM EVIDENCE IN HAND (no boots).
 * A first draft of 3ext-d listed four candidates. Two of them were answered by
 * a counter this project has published since 2ph and by the return convention:
 *
 *  - "the controller's start() never reached createLinks". `dal_start_ret` IS
 *    the controller's start return - `hooked_ctl_start_2ph` routes
 *    AmdRadeonControllerNavi2::start - and boot 588 read 1. The claim that it
 *    had never been measured was wrong, and it was wrong in the card as well
 *    as in the journal.
 *  - "a zero count makes createFbStubs fail". It does not. `%r14d` is zeroed
 *    at 0xbf9ff18, BEFORE the count test, so the zero path returns 0; `start`
 *    does `testl %eax,%eax / je` into setupHwServices / copyEFIPropertiesToStubs
 *    / reportBrandingInformation, so these are IOReturn codes and 0 is SUCCESS.
 *    createLinks likewise returns 0 on the no-connector path and 0xE00002BC
 *    when createLink fails. Its caller is createControllerServices() under
 *    start, so `start` returning true also eliminates "createLink failed".
 *
 * What is left is one number: getConnectorList handed back n = 0. The VBIOS
 * display-object table on this disk has SIX paths in it (rev 1.4,
 * supporteddevices 0x0eca, eDP on UNIPHY_A plus four DP including UNIPHY_C /
 * phyid 2 / DP_IS_USB_C 1), against Apple's seed of exactly 6 - so the table is
 * present and sane and the parser is returning nothing from it.
 *
 * THE VERDICT IS THREE THINGS, NEVER ONE NUMBER (310's lesson).
 * Did createLinks RUN, what did the parser SAY, and did any link get created.
 * A single "it failed" cannot distinguish a parser that returns zero from an
 * instrument that never fired, and rule 14 applies hardest here because every
 * interesting value in this path is legitimately zero.
 *
 * THE CONTROL IS IN THE SAME SAMPLE. We call getConnectorCount() (slot 0x188)
 * as well as getConnectorList() (slot 0x190). Two independent answers from the
 * same object: if the count is non-zero while the list returns n 0, the fault
 * is in the list call, and if both are zero the parser genuinely has no
 * connectors. Neither alone would separate those.
 *
 * WHY WE CALL AFTER, NOT BEFORE (rule 16 - the instrument is a suspect).
 * Both are `const` getters, so the answer does not depend on when they are
 * asked, and calling them AFTER the original createLinks has returned means our
 * calls cannot perturb the run we are measuring. A before-call would buy
 * nothing and put our code inside the sequence.
 *
 * EVERY OFFSET HERE IS A 25G342 OFFSET DERIVED FROM OUR OWN BINARY. Rule 41:
 * the kext gates the reads on RB_OSD_KEXTOFF and refuses with a code rather
 * than reading a wrong address on an OS where they were never measured.
 */
#ifndef RB_FBSTUBS_H
#define RB_FBSTUBS_H

#include <stdint.h>

/* ---- the object layout, all from 3ext-c/3ext-d ------------------------- */

#define RB_FBSTUBS_OFF_STUBARR   0xc0u    /* framebuffer stub array          */
#define RB_FBSTUBS_OFF_LINKCNT   0x120u   /* byte: links created             */
#define RB_FBSTUBS_OFF_LINKARR   0x128u   /* link array                      */
#define RB_FBSTUBS_OFF_BIOSP     0x7908u  /* AmdBiosParserHelper*            */

#define RB_FBSTUBS_VT_CONNCOUNT  0x188u   /* getConnectorCount() const       */
#define RB_FBSTUBS_VT_CONNLIST   0x190u   /* getConnectorList(uint&, desc*)  */

#define RB_FBSTUBS_DESC_STRIDE   0x24u    /* AmdLinkDescriptor               */
#define RB_FBSTUBS_SEED          6u       /* what Apple seeds the count with */
#define RB_FBSTUBS_SLOTS         8u       /* our buffer: the seed plus slack */

/* The stub array runs 0xc0..0x118 inclusive, cleared as one run by
 * initializeInternals immediately before the count byte. That is 12 slots, and
 * it is how the array and its count were confirmed against each other. */
#define RB_FBSTUBS_ARR_SLOTS     12u

/* Apple's own success/failure codes on this path, read off the binary. */
#define RB_FBSTUBS_IORET_OK      0x00000000u
#define RB_FBSTUBS_IORET_LINKERR 0xE00002BCu

/* ---- readability: why a reading is absent ------------------------------- */
/* Codes, not bools (rule 14). 0 means every pointer resolved and the two
 * virtual calls were made. */
#define RB_FBSTUBS_W_OK          0u
#define RB_FBSTUBS_W_SELF        1u  /* `this` is not a kernel pointer        */
#define RB_FBSTUBS_W_OSVER       2u  /* offsets unmeasured on this OS         */
#define RB_FBSTUBS_W_BIOSP       3u  /* controller+0x7908 is not a pointer    */
#define RB_FBSTUBS_W_VTABLE      4u  /* its vtable is not a pointer           */
#define RB_FBSTUBS_W_SLOTS       5u  /* a slot does not hold a text address   */

/* Rule 19's floors. A NULL check is not a pointer check, and a refusal must
 * publish the value it refused - the kext does that beside every code. */
#define RB_FBSTUBS_HEAP_FLOOR    0xffffff8000000000ULL
#define RB_FBSTUBS_TEXT_FLOOR    0xffffff7f80000000ULL

static inline int rb_fbstubs_obj_ok(uint64_t v) {
    return v >= RB_FBSTUBS_HEAP_FLOOR && (v & 7u) == 0u;
}

/* A kext function address is above the kext symbol floor but BELOW the kalloc
 * floor; accepting anything above the heap floor would accept a data pointer,
 * which is how a wild vtable slot gets called. */
static inline int rb_fbstubs_text_ok(uint64_t v) {
    return v >= RB_FBSTUBS_TEXT_FLOOR && v < RB_FBSTUBS_HEAP_FLOOR;
}

static inline uint32_t rb_fbstubs_why(int selfOk, int osOk, uint64_t biosp,
                                      uint64_t vtable, uint64_t fnCount,
                                      uint64_t fnList) {
    if (!selfOk)                      { return RB_FBSTUBS_W_SELF; }
    if (!osOk)                        { return RB_FBSTUBS_W_OSVER; }
    if (!rb_fbstubs_obj_ok(biosp))    { return RB_FBSTUBS_W_BIOSP; }
    if (!rb_fbstubs_obj_ok(vtable))   { return RB_FBSTUBS_W_VTABLE; }
    if (!rb_fbstubs_text_ok(fnCount)) { return RB_FBSTUBS_W_SLOTS; }
    if (!rb_fbstubs_text_ok(fnList))  { return RB_FBSTUBS_W_SLOTS; }
    return RB_FBSTUBS_W_OK;
}

/* ---- the verdict ------------------------------------------------------- */
/* Deliberately NOT a success/failure bool. Each value names which of the
 * candidates 3ext-d left open is the live one, and the contaminated and
 * unreadable cases come BEFORE any positive finding (310's rule: check the
 * controls before declaring a result). */
#define RB_FBSTUBS_V_NOT_RUN      0u /* createLinks never called this boot    */
#define RB_FBSTUBS_V_UNREADABLE   1u /* it ran; the pointers did not resolve  */
#define RB_FBSTUBS_V_CONTAMINATED 2u /* the two parser answers DISAGREE       */
#define RB_FBSTUBS_V_LIST_FAILED  3u /* connectors exist, the list call errored */
#define RB_FBSTUBS_V_NO_CONNECTOR 4u /* the parser has none - the live candidate */
#define RB_FBSTUBS_V_LINKS_LOST   5u /* connectors listed, no link survived   */
#define RB_FBSTUBS_V_LINKS_MADE   6u /* links exist - the premise is wrong    */

/* `calls` is seeded at route time, so 0 here is a reading and not an absence.
 * `listRet` is Apple's IOReturn from slot 0x190. `connCount` is slot 0x188's
 * answer and `listN` the count written back through the reference - the two
 * independent answers whose disagreement is what CONTAMINATED reports. */
static inline uint32_t rb_fbstubs_verdict(uint32_t calls, uint32_t why,
                                          uint32_t connCount, uint32_t listRet,
                                          uint32_t listN, uint32_t links) {
    if (calls == 0u)                       { return RB_FBSTUBS_V_NOT_RUN; }
    if (why != RB_FBSTUBS_W_OK)            { return RB_FBSTUBS_V_UNREADABLE; }
    /* Before anything is concluded from a zero: do the two answers agree at
     * all? A non-zero count beside an n of 0 is the list call being at fault,
     * and the reverse is an instrument we do not understand. Either way the
     * sample cannot support a statement about the parser. */
    if ((connCount == 0u) != (listN == 0u)) {
        if (listRet != RB_FBSTUBS_IORET_OK) { return RB_FBSTUBS_V_LIST_FAILED; }
        return RB_FBSTUBS_V_CONTAMINATED;
    }
    if (listRet != RB_FBSTUBS_IORET_OK)    { return RB_FBSTUBS_V_LIST_FAILED; }
    if (listN == 0u)                       { return RB_FBSTUBS_V_NO_CONNECTOR; }
    if (links == 0u)                       { return RB_FBSTUBS_V_LINKS_LOST; }
    return RB_FBSTUBS_V_LINKS_MADE;
}

/* The buffer the kext hands to slot 0x190. Sized from the stride and our slot
 * count rather than written as a number, because 0xd8/0x24 = 6 is the one
 * arithmetic relationship this whole derivation rests on. */
#define RB_FBSTUBS_BUFSZ (RB_FBSTUBS_DESC_STRIDE * RB_FBSTUBS_SLOTS)

/* Apple seeds the in/out count with the CAPACITY. We pass its seed, not our
 * slot count, so the call behaves as the real one does - the two extra slots
 * are slack against a parser that ignores the seed, never a larger ask. */
static inline uint32_t rb_fbstubs_seed(void) { return RB_FBSTUBS_SEED; }

/* A returned count above our buffer's capacity means the parser wrote past the
 * seed; the descriptors are then not ours to read. */
static inline int rb_fbstubs_n_sane(uint32_t n) { return n <= RB_FBSTUBS_SLOTS; }

#endif /* RB_FBSTUBS_H */
