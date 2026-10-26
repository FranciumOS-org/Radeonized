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
static inline uint32_t rb_agdc_get32(const uint8_t *p) {
    uint32_t v=0; for (unsigned i=0;i<4;i++) v|=(uint32_t)p[i]<<(8*i); return v;
}
static inline uint64_t rb_agdc_get64(const uint8_t *p) {
    uint64_t v=0; for (unsigned i=0;i<8;i++) v|=(uint64_t)p[i]<<(8*i); return v;
}

/* IODetailedTimingInformationV2 field offsets. These are not ours to guess:
 * 2rg read IOPresentmentCreateEndPointDictionary (0x7ff812e9550c), which turns
 * the cached 0x921 timing into CFNumbers under Apple's OWN key names, and the
 * fourteen leaq sites line up field-for-field and in order with the public
 * header - PixelClock 0x28, MinPixelClock 0x30, MaxPixelClock 0x38,
 * HorizontalActive 0x40, HorizontalBlanking 0x44, HorizontalSyncOffset 0x48,
 * HorizontalSyncPulseWidth 0x4c, VerticalActive 0x50, VerticalBlanking 0x54,
 * VerticalSyncOffset 0x58, VerticalSyncPulseWidth 0x5c, HorizontalSyncConfig
 * 0x70, VerticalSyncConfig 0x78, VerticalBlankingExtension 0x84. That is
 * Apple naming the fields rather than us matching offsets, and it is the
 * strongest confirmation yet that the V2 struct sits at reply+0x04. */
#define RB_AGDC_V2_PIXELCLOCK 0x28u
#define RB_AGDC_V2_HACTIVE    0x40u
#define RB_AGDC_V2_HBLANKING  0x44u
#define RB_AGDC_V2_VACTIVE    0x50u
#define RB_AGDC_V2_VBLANKING  0x54u
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
 * i.e. bit 3 unconditionally. Bit 0 stays CLEAR, and 2rg measured what it
 * actually costs rather than assuming: __GatherAGDCLogicalDeviceCapabilities
 * tests reply+0x08 bit 0 at 0x7ff812e979b7 and, when set, calls
 * __GatherAGDCLinkCapabilities(reply+0xa4) - which issues selector 0x925
 * (getPortStatus, 0xc bytes) and then 0x988 (0x24 bytes), and whose ONLY
 * effect is this->0x19c78, a field with four writers and ZERO readers in the
 * whole image. It returns void, r14 (the success code, already zeroed at
 * 0x7ff812e979b4) is callee-saved across it, and every failure path leaves
 * 0x19c78 = 2. So setting bit 0 would be SAFE, not fatal - it is left clear
 * because this bridge knows of no link to describe, not to dodge a hazard. */
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
 * would be hardest to find later.
 *
 * 2rg CORRECTS 2rd's "a cache it never reads": the cache has 24 reads, and it
 * is the SOURCE of the display's advertised capabilities.
 * IOPresentmentCreateEndPointDictionary republishes the whole V2 body as
 * CFNumbers, IOPresentmentGetCurrentEndPointDimensions serves LooksLikeWidth
 * and LooksLikeHeight out of it, and __gatherCapabilities (0x7ff812e95370)
 * COMPUTES the refresh rate from it:
 *
 *     total = (float)(hActive+hBlanking) * (float)(vActive+vBlanking);
 *     refresh = 60.0f;                       // 0x42700000, stored first
 *     if (total > 0.0f) refresh = pixelClock / total;
 *
 * (the ucomiss constant at 0x7ff812ea3468 reads 0.0f, so that branch is only a
 * divide-by-zero guard) and the result is published under RefreshTime,
 * MinRefreshTime and MaxRefreshTime. Which makes a ZERO pixelClock the one
 * remaining poison: it PASSES the > 0.0f guard and yields a published refresh
 * of 0.0 Hz, where a zero total would at least leave Apple's stated 60.0f
 * default. So a zero pixelClock is a refusal too, by the same rule and for a
 * measured reason. A zero hActive/vActive is refused in the caller, and 2rg
 * justifies that independently: GetCurrentEndPointDimensions returns having
 * written NEITHER output when either is zero, leaving its caller's values
 * untouched. */
static inline int rb_agdc_link_config(void *out, size_t size,
                                      const void *timing_v2, uint32_t link_id,
                                      uint64_t flags, uint32_t stream_index,
                                      uint32_t stream_state) {
    if (!out || size != RB_AGDC_LINK_SIZE || !timing_v2) return -1;
    if (flags & RB_AGDC_LINK_PORT) return -1;
    const uint8_t *t = (const uint8_t *)timing_v2;
    /* Refuse before touching the output, so a refusal leaves it as it was. */
    if (!rb_agdc_get64(t+RB_AGDC_V2_PIXELCLOCK)) return -1;
    uint8_t *p = (uint8_t *)out;
    uint32_t index = rb_agdc_get32(p);
    if (index != 0) return -1;            /* one framebuffer, index 0 */
    for (size_t i = 0; i < size; i++) p[i] = 0;
    for (size_t i = 0; i < RB_AGDC_TIMING_SIZE; i++) p[RB_AGDC_TIMING_OFF+i] = t[i];
    rb_agdc_put32(p, index);
    rb_agdc_put32(p+0x04, link_id);       /* overlays __reservedA[0] */
    rb_agdc_put64(p+0x08, flags);         /* overlays __reservedA[1..2] */
    /* +0xa4 and +0xa8 are ONE field to the consumer: 2rg's
     * __GatherAGDCLogicalDeviceCapabilities reads reply+0xa4 with a single
     * movq and passes it as AGDCStreamAddress_t, so the zero at +0xa8 is this
     * address's high half, not padding. */
    rb_agdc_put32(p+0xa4, stream_index);
    rb_agdc_put32(p+0xa8, 0);
    rb_agdc_put32(p+0xac, stream_state);
    return 0;
}

/* 2rf: attribute 0x711 / getFbExtendedCapability. The structure is a QUERY,
 * not a dump: the caller fills in a framebuffer index, a capability type and
 * a capacity, and the handler writes back how many entries it filled.
 * Measured in Apple's getFbExtendedCapability (0xbfecb62) and its per-type
 * fillers, whose symbols are intact:
 *
 *   +0x00  u8   framebuffer index   (Apple reads it with movzbl)
 *   +0x04  u32  capability type
 *   +0x08  u32  count - IN the caller's capacity, OUT the number filled
 *   +0x0c  AGDCFBExtendedCapabilitesEntry_t entries[8], 0x32c each
 *
 * The sizes close the layout exactly: 8 * 0x32c = 0x1960, + 0xc = 0x196c,
 * which is both the length Apple's filter demands and the capacity 8 that
 * IOPresentment pre-loads into +0x08. getFbCapabilityForScaler bzeroes
 * entry+0x08 for 0x324 bytes, which is the same 0x32c.
 *
 * Apple implements only types 1 CSC, 2 Gamma, 4 Degamma, 0x10 Scaler and
 * 0x8000 Cursor; every other type returns kIOReturnUnsupported even on a real
 * AMD Mac. IOPresentment asks for ALL SIXTEEN type bits in turn, so most of
 * those queries fail by design and it tolerates them:
 * __GatherAGDCDisplayPipelineCapabilities (0x7ff812e979dc) returns 0x2006 from
 * exactly two places, both tied to the FIRST query - type 0x10, the scaler -
 * its non-zero IOReturn, and a returned count of zero. Every later query's
 * failure merely resets outLen and moves on, and the last one is preceded by
 * `xorl %r14d,%r14d` so even its failure returns success.
 *
 * So the whole stage needs: answer type 0x10 with count >= 1, refuse the rest.
 * The count must also stay below 9 or IOPresentment clamps it to 8. */
#define RB_AGDC_EXTCAP_SIZE      0x196cu
#define RB_AGDC_EXTCAP_COUNT_OFF 0x08u
#define RB_AGDC_EXTCAP_ENTRY_OFF 0x0cu
#define RB_AGDC_EXTCAP_ENTRY_SZ  0x32cu
#define RB_AGDC_EXTCAP_ENTRIES   8u
#define RB_AGDC_CAP_CSC          0x0001u
#define RB_AGDC_CAP_GAMMA        0x0002u
#define RB_AGDC_CAP_DEGAMMA      0x0004u
#define RB_AGDC_CAP_SCALER       0x0010u
#define RB_AGDC_CAP_CURSOR       0x8000u

/* One scaler entry describing NO SCALING, in Apple's own shape: its
 * no-scaling path (0xbfed05a) writes the identity word, a 1 at entry+0x1c and
 * +0x1a4, the two extents twice over in each of two blocks, and the constants
 * 0x00050005 / 4 - and because the source and destination extents are equal,
 * the entry says "this framebuffer scales 1:1", which is true of ours.
 *
 * NOT ESTABLISHED, and deliberately not guessed: which of the two slots in
 * each pair is width and which is height. Apple takes them from fb->0x720 and
 * fb->0x71c, and updateStreamComposition (0xbfe9270) shows 0x71c pairing with
 * 0xc0 and 0x720 with 0xc4 in a letterbox fit, which fixes the pairing but not
 * the axis names. It cannot change this entry's MEANING: with source and
 * destination equal, transposing both pairs still says 1:1. Both values are
 * published as diag keys so one boot that reaches a consumer can settle it. */
static inline int rb_agdc_scaler_entry(uint8_t *e, uint32_t slotA, uint32_t slotB) {
    rb_agdc_put32(e+0x000, 0x00080004);   /* identity; CSC's is 0x00080005 */
    rb_agdc_put32(e+0x01c, 1);
    rb_agdc_put32(e+0x024, slotA);
    rb_agdc_put32(e+0x028, slotB);
    rb_agdc_put32(e+0x02c, slotA);
    rb_agdc_put32(e+0x030, slotB);
    rb_agdc_put32(e+0x034, 0x00050005);
    rb_agdc_put32(e+0x038, 4);
    rb_agdc_put32(e+0x1a4, 1);
    rb_agdc_put32(e+0x1ac, slotA);
    rb_agdc_put32(e+0x1b0, slotB);
    rb_agdc_put32(e+0x1b4, slotA);
    rb_agdc_put32(e+0x1b8, slotB);
    rb_agdc_put32(e+0x1bc, 0x00050005);
    rb_agdc_put32(e+0x1c0, 4);
    return 0;
}

#define RB_AGDC_EXTCAP_UNSUPPORTED (-2)
/* Returns 0 when it answered, RB_AGDC_EXTCAP_UNSUPPORTED for a type this
 * bridge does not implement (the caller maps that to kIOReturnUnsupported and
 * the output is left exactly as it was), -1 for a malformed request.
 * The index and type are read where Apple reads them, out of the structure
 * itself; +0x00 and +0x04 are preserved and everything from +0x08 is
 * rewritten. hactive/vactive must both be non-zero: with no real timing there
 * is nothing truthful to say about a scaler, so it refuses. */
static inline int rb_agdc_extcap(void *out, size_t size,
                                 uint32_t hactive, uint32_t vactive) {
    if (!out || size != RB_AGDC_EXTCAP_SIZE) return -1;
    uint8_t *p=(uint8_t *)out;
    if (p[0] != 0) return -1;             /* one framebuffer, index 0 */
    uint32_t type=0;
    for (unsigned i=0;i<4;i++) type|=(uint32_t)p[4+i]<<(8*i);
    if (type != RB_AGDC_CAP_SCALER) return RB_AGDC_EXTCAP_UNSUPPORTED;
    if (!hactive || !vactive) return -1;
    for (size_t i=RB_AGDC_EXTCAP_COUNT_OFF;i<size;i++) p[i]=0;
    rb_agdc_put32(p+RB_AGDC_EXTCAP_COUNT_OFF, 1);
    return rb_agdc_scaler_entry(p+RB_AGDC_EXTCAP_ENTRY_OFF, vactive, hactive);
}

/* Validate only the operations we implement. Later AGDC operations legitimately
 * have input data (e.g. 0x921 uses a 0xb0-byte input/output structure).
 * Classifying those as malformed hides the missing operation. */
#define RB_AGDC_BAD_ARGUMENT (-1)
#define RB_AGDC_UNSUPPORTED  (-2)
static inline int rb_agdc_reply(unsigned attr, size_t in_size, void *out,
                                size_t *out_size, uint64_t pci, uint64_t fb,
                                const void *timing_v2) {
    if (attr != 1 && attr != 0x980 && attr != 0x921 && attr != 0x711)
        return RB_AGDC_UNSUPPORTED;
    if (!out_size) return RB_AGDC_BAD_ARGUMENT;
    if (attr == 0x711) {
        /* The scaler entry's extents come from the same real timing as 0x921;
         * without it there is nothing truthful to report. */
        if (!timing_v2) return RB_AGDC_UNSUPPORTED;
        const uint8_t *t=(const uint8_t *)timing_v2;
        const uint32_t h=rb_agdc_get32(t+RB_AGDC_V2_HACTIVE);
        const uint32_t v=rb_agdc_get32(t+RB_AGDC_V2_VACTIVE);
        const int r=rb_agdc_extcap(out,*out_size,h,v);
        if (r == RB_AGDC_EXTCAP_UNSUPPORTED) return RB_AGDC_UNSUPPORTED;
        return r == 0 ? 0 : RB_AGDC_BAD_ARGUMENT;
    }
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
