/* SPDX-License-Identifier: MIT */
#ifndef RB_AGDC_H
#define RB_AGDC_H
#include <stddef.h>
#include <stdint.h>

/* 2ra: reply layouts measured in Apple's AMD framebuffer, not inferred from
 * the consumer. getVendorInfo 0xbffb59a; getGpuCapability 0xbffb712.
 * Capability +0x34/+0x3c are KERNEL OBJECT POINTERS. Apple's filtered API
 * converts them to registry IDs before returning to userspace (0x13dfd167).
 * Never send this kernel form directly to a user client. */
#define RB_AGDC_VENDOR_SIZE 0x2cu
#define RB_AGDC_CAP_SIZE    0xdcu
static inline void rb_agdc_put32(uint8_t *p, uint32_t v) {
    for (unsigned i=0; i<4; i++) p[i]=(uint8_t)(v >> (8*i));
}
static inline void rb_agdc_put64(uint8_t *p, uint64_t v) {
    for (unsigned i=0; i<8; i++) p[i]=(uint8_t)(v >> (8*i));
}
static inline int rb_agdc_vendor(void *out, size_t size) {
    if (!out || size != RB_AGDC_VENDOR_SIZE) return -1;
    uint8_t *p=(uint8_t *)out;
    for (size_t i=0; i<size; i++) p[i]=0;
    rb_agdc_put32(p, 0x30000);
    p[4]='A'; p[5]='M'; p[6]='D';
    rb_agdc_put32(p+0x24, 0x1002);
    rb_agdc_put32(p+0x28, 2); /* per-GPU service; AMD wrangler uses 7 */
    return 0;
}
static inline int rb_agdc_capability(void *out, size_t size,
                                     uint64_t pci, uint64_t framebuffer) {
    if (!out || size != RB_AGDC_CAP_SIZE || !pci || !framebuffer) return -1;
    uint8_t *p=(uint8_t *)out;
    for (size_t i=0; i<size; i++) p[i]=0;
    /* One existing framebuffer. All optional feature masks remain zero;
     * this bridge does not implement links, DSC, VRR, or display modes. */
    rb_agdc_put32(p+0x20, 1);
    rb_agdc_put32(p+0x30, 1);
    rb_agdc_put64(p+0x34, pci);
    rb_agdc_put64(p+0x3c, framebuffer);
    return 0;
}

/* 2rd: attribute 0x921 / getLinkConfig. AGDCLinkConfig_t is 0xb0 bytes and
 * EMBEDS the public IODetailedTimingInformationV2 at +0x04 - measured in
 * Apple's own getLinkConfig (0xc009184), which block-copies its framebuffer's
 * timing to out+0x10 shifted by exactly -0xc, that being sizeof(__reservedA[3]).
 * Confirmed by offsetof, not by counting header lines (2q4's rule): the V2
 * struct is 0xa0 bytes, so 0x04 + 0xa0 = 0xa4, exactly the next written field,
 * and the whole thing sums to the 0xb0 Apple's length check demands.
 *
 * AGDC REUSES the three __reservedA words the header documents as "Init to 0":
 *   +0x04  link id   (0xFFFFFFFF = none)
 *   +0x08  flags     (u64; Apple builds only the low byte)
 * so the timing must be copied FIRST and these overwritten after.
 *
 * The flags byte Apple assembles is
 *   0x08 | (linkId != -1 ? 0x10 : 0x02) | ((link->vt[0x150](0xd) & 1) << 2)
 *        | fb->0x719
 * i.e. bit 3 unconditionally. Bit 0 must stay CLEAR: IOPresentment's
 * 0x7ff812e98af6 reads it and, when set, issues selector 0x925
 * (getPortStatus, 0xc bytes) and then a 0x24-byte follow-up - a port
 * sub-protocol this bridge does not implement. */
#define RB_AGDC_LINK_SIZE    0xb0u
#define RB_AGDC_TIMING_OFF   0x04u
#define RB_AGDC_TIMING_SIZE  0xa0u
#define RB_AGDC_LINK_NONE    0xFFFFFFFFu
#define RB_AGDC_LINK_BIT3    0x08u   /* Apple sets it on every reply */
#define RB_AGDC_LINK_NOLINK  0x02u   /* set iff link id is NONE */
#define RB_AGDC_LINK_VALID   0x10u   /* set iff link id is real */
#define RB_AGDC_LINK_PORT    0x01u   /* claims getPortStatus - never set it */
#define RB_AGDC_STREAM_NONE  0u            /* Apple's value when idx == 0xff */
#define RB_AGDC_STREAM_BAD   0x80000000u   /* translateAmdStreamState default */

/* The framebuffer index is NOT ours to choose: IOPresentment writes it into
 * the buffer itself (this->0x2c = deviceRecord->0x50) and Apple's handler
 * reads it back out of the OUTPUT buffer, because input and output are the
 * same userspace structure. So read it where Apple reads it, bounds-check it
 * against the one framebuffer this bridge advertises, and write it back
 * unchanged.
 *
 * A NULL timing is a REFUSAL, never a zero fill. 2rd measured that
 * IOPresentment validates no timing field - it copies reply+0x10..0x9b into
 * its own cache and returns success either way - so a zero-filled reply would
 * PASS this stage and silently poison that cache, which is the failure that
 * would be hardest to find later. */
static inline int rb_agdc_link_config(void *out, size_t size,
                                      const void *timing_v2, uint32_t link_id,
                                      uint64_t flags, uint32_t stream_index,
                                      uint32_t stream_state) {
    if (!out || size != RB_AGDC_LINK_SIZE || !timing_v2) return -1;
    if (flags & RB_AGDC_LINK_PORT) return -1;
    uint8_t *p = (uint8_t *)out;
    uint32_t index = 0;
    for (unsigned i = 0; i < 4; i++) index |= (uint32_t)p[i] << (8*i);
    if (index != 0) return -1;            /* one framebuffer, index 0 */
    for (size_t i = 0; i < size; i++) p[i] = 0;
    const uint8_t *t = (const uint8_t *)timing_v2;
    for (size_t i = 0; i < RB_AGDC_TIMING_SIZE; i++) p[RB_AGDC_TIMING_OFF+i] = t[i];
    rb_agdc_put32(p, index);
    rb_agdc_put32(p+0x04, link_id);       /* overlays __reservedA[0] */
    rb_agdc_put64(p+0x08, flags);         /* overlays __reservedA[1..2] */
    rb_agdc_put32(p+0xa4, stream_index);
    rb_agdc_put32(p+0xa8, 0);
    rb_agdc_put32(p+0xac, stream_state);
    return 0;
}

/* Validate only the operations we implement. Later AGDC operations legitimately
 * have input data (e.g. 0x921 uses a 0xb0-byte input/output structure).
 * Classifying those as malformed hides the missing operation. */
#define RB_AGDC_BAD_ARGUMENT (-1)
#define RB_AGDC_UNSUPPORTED  (-2)
static inline int rb_agdc_reply(unsigned attr, size_t in_size, void *out,
                                size_t *out_size, uint64_t pci, uint64_t fb,
                                const void *timing_v2) {
    if (attr != 1 && attr != 0x980 && attr != 0x921) return RB_AGDC_UNSUPPORTED;
    if (!out_size) return RB_AGDC_BAD_ARGUMENT;
    if (attr == 0x921) {
        /* Without real timing we do not answer at all. Unsupported leaves
         * IOPresentment exactly where it was in boot 156 and tells no lie. */
        if (!timing_v2) return RB_AGDC_UNSUPPORTED;
        /* Apple's getLinkConfig validates ONLY the output length (cmpq $0xb0
         * at 0xc0091a0) and never looks at inLen, so neither do we: input and
         * output are the same userspace structure and a length check of our
         * own here would invent a refusal the protocol does not have.
         * rb_agdc_link_config enforces the 0xb0 that Apple enforces. */
        (void)in_size;
        return rb_agdc_link_config(out,*out_size,timing_v2,RB_AGDC_LINK_NONE,
                                   RB_AGDC_LINK_BIT3|RB_AGDC_LINK_NOLINK,
                                   RB_AGDC_STREAM_NONE,RB_AGDC_STREAM_BAD);
    }
    if (in_size) return RB_AGDC_BAD_ARGUMENT;
    return attr == 1 ? rb_agdc_vendor(out,*out_size)
                     : rb_agdc_capability(out,*out_size,pci,fb);
}
#endif
