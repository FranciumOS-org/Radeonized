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
/* 2sc: the other four types Apple's own dispatcher answers.
 *
 * AMDRadeonX6000_AmdAgdcServices::getFbExtendedCapability (0xbfecb62) is a
 * switch over the type at +0x04 with EXACTLY five arms - 1, 2, 4, 0x10, 0x8000
 * - and kIOReturnUnsupported (0xe00002c7) for everything else. That confirms
 * the five-type list this header already carried. The names come from
 * stringifyFbCapabilityType (0xbff0e70), whose sixteen strings are in bit
 * order, cross-checked on four arms Apple compares explicitly (0x20
 * "Rotation", 0x200 "Mirroring", 0x800 "HDR Scaler", 0x2000 "3D LUT"): so type
 * 4 is "Linearization" (Apple's provider is named Degamma) and 0x10 is
 * "Plane Scaler". getFbCapabilityForRotation EXISTS in the binary and is
 * unreachable from the dispatcher, so a real Navi 21 Mac does not answer 0x20.
 *
 * THE LIMIT OF THIS, STATED ONCE. Apple fills three of the four from queries
 * on objects we do not have - an AmdFbDisplayPath and the AmdAgdcServices
 * device object. Every such input below uses the value Apple's own code
 * produces on its SMALLEST branch, so nothing here claims a capability larger
 * than Apple's minimum, and each one is named. This is a substitute and is
 * labelled as one (rule 17): what the entries describe is a stage that exists
 * and does nothing, which is what an IONDRVFramebuffer with no DCN
 * programming can honestly report. The structural constants - the capability
 * words, the 1.0f, the segment ids - are read out of Apple's code and are not
 * substitutes.
 */
/* path->0x7c4 < 2, the branch Apple takes for the fewest CSC precision bits.
 * >= 2 gives 0x00070007 and >= 3 ORs in 0x01000000. */
#define RB_AGDC_CSC_PRECISION_MIN 0x00050005u
/* this->0x2e798, a device flags qword Apple ORs into the segment id. */
#define RB_AGDC_DEV_FLAGS_NONE    0u
/* vt[0x118](0x17) + 1. Apple writes a second gamma entry only when this is
 * >= 2, so 1 is both the minimum and the single-entry case.
 *
 * 2u9: RAISED FROM 1 TO 2, and this is the one place to put it back. Boot 179
 * measured that answering type 2 at all moves CoreDisplay's complaint from
 * "plane with no gamma" to "plane with no 3-channel gamma table support"
 * (2u6), and 2u7 read the sole emitter: the predicate is a NON-ZERO value
 * behind an accepted format word, and our format word is already accepted.
 * Apple's second entry is the only one of the two whose element count is a
 * LITERAL in its code - 0x200 - rather than a query on an object we lack, so
 * taking the two-entry branch is the one way to answer with a non-empty gamma
 * table WITHOUT inventing a number (rule 17). Entry 1 is unchanged and its
 * substituted count below is untouched.
 *
 * 2v1: AND PUT BACK TO 1, because boot 181 measured it buy NOTHING - 7.01
 * gamma complaints per DisplayPipeState dump against 179's 7.12, `System
 * Gamma` still `Unknown`, `Clean` still absent, with the reply verified sent
 * (`agdc_gamma_entries 2`, `agdc_gamma_elems2 0x200`) and the build verified
 * loaded. So CoreDisplay's `entry+0x30` is not fed from our `+0x28`. The
 * segment count is itself a substitute, and 2 claims a two-segment stage plus
 * one more substituted field (entry 2's 0x40 device flags) for no measured
 * gain - rule 17. `rb_agdc_gamma_entry2()` below is kept and hosttested, so
 * retrying this is one character. */
#define RB_AGDC_GAMMA_SEGMENTS    1u
/* vt[0x1b8](0xf), written as a QWORD to entry 1's +0x28. 2u9 read the widths
 * and this is the entry's ELEMENT COUNT, not a channel count - the name is
 * kept because 2sc coined it and the journal cites it.
 *
 * 2v5: THIS IS THE FIELD, and it is now identified rather than guessed.
 * IOPresentmentCapabilitiesDisplayPipe::IOPresentmentCreateGammaDictionary
 * (0x7ff812e9b8e0) walks THIS BUFFER at stride 0x32c and publishes +0x28 as
 * the dictionary key `MaximumSize`; __fieldsFromDictionary lands that at the
 * CoreDisplay entry's +0x30; and GammaChannelTableEntryCountAndType
 * (0x7ff80405ddd8) logs "plane with no 3-channel gamma table support" when
 * that is ZERO. Boots 179 and 181 both measured the message at ~7 per
 * DisplayPipeState dump with this field at 0.
 *
 * 0x400 = 1024, AND IT IS STILL A SUBSTITUTE (rule 17). Apple reads it from
 * vt[0x1b8](0xf) on an AmdFbDisplayPath we do not have, so no value written
 * here is a hardware fact and no reading downstream of it may be narrated as
 * one. Two things bound the choice: entry 1's Type is 0x10, whose accepted
 * bit is 4 (2u7), and CoreDisplay's own DisplayPipeState dumps already print
 * `LUT: R - [0..0.500489..1] with 1024 entries` for this display - so 1024 is
 * the size of the table CoreDisplay has itself built, not a number of ours.
 * Apple's own segmented second entry uses 0x200 for its different stage. */
/* 2wd: 0x400 IS REFUSED BY APPLE'S OWN ACCELERATOR, and 256 is its ceiling.
 * `AMDRadeonX6000_AMDAccelDisplayPipe::executeTransaction` (0xbe49b54) refuses
 * the whole transaction when the pipe POST-gamma table carries more than 0x100
 * entries - `cmpl $0x100, %r13d; ja` at 0xbe4a350 with
 * `r13 = IOAccelDisplayPipeGammaTable2::getTableCount()` - and that count is
 * `*(uint32_t *)(table->0x10)`, the first dword of the
 * IOAccelDisplayPipeGammaTableData WindowServer itself supplied. So the size is
 * CoreDisplay's choice, the only lever we have on it is this capability, and
 * boot 192 measured `agdc_gamma_elems1 = 1024` against 132 refusals carrying
 * `IOReturn: 0x2c7` all the way up into CoreDisplay.
 * 2wc excluded this ceiling from boot 190's `DisplayPipeState` LUT counts (71
 * of 256 entries, 2 of 1024) - and that was the WRONG TABLE. Those dumps are
 * headed `Plane Index: 0` and their LUT is the PLANE gamma, whose dirty bit
 * (0x4) is CLEAR in the measured dirtyBits 0x3c1; the gate is on bit 0x80, the
 * PIPE post-gamma, which the dumps never print. Rule 33: name the object a
 * number belongs to before retiring a line with it.
 * 256 is still a substitute (rule 17) - Apple reads it from `vt[0x1b8](0xf)` on
 * an AmdFbDisplayPath we do not have - but unlike 0x400 it is bounded by a
 * measured fact about the consumer rather than by the size of a table
 * CoreDisplay happened to print. It is non-zero, so 2u6's "plane with no
 * 3-channel gamma table support" (which fired at ZERO) stays fixed. */
#define RB_AGDC_GAMMA_CHANNELS    0x100u
/* movq $0x200, 0x354(%rbx) in getFbCapabilityForGamma - Apple's LITERAL for
 * the second gamma entry's element count, not a substitute. [2u9] */
#define RB_AGDC_GAMMA_ELEMENTS    0x200u
/* powf(2.0, -25) and powf(2.0, 7) as IEEE-754 single bit patterns: Apple calls
 * AmdMathUtilities::power(2.0, -25) and (2.0, 7) and stores the cvtsd2ss
 * results at entry 2's +0x20 and +0x24. Exact powers of two, so the encodings
 * are exact: exponent 127-25 = 102 = 0x66 and 127+7 = 134 = 0x86. [2u9] */
#define RB_AGDC_GAMMA_MIN_F       0x33000000u
#define RB_AGDC_GAMMA_MAX_F       0x43000000u
/* vt[0x1b8](6), written to BOTH entry+0x14 and entry+0x18. */
#define RB_AGDC_CURSOR_DIM_NONE   0u

/* Type 1 CSC. Apple handles this one INLINE in the dispatcher, at 0xbfecca2. */
static inline int rb_agdc_csc_entry(uint8_t *e) {
    rb_agdc_put32(e+0x00, 0x00080005);
    rb_agdc_put32(e+0x04, 1);
    rb_agdc_put32(e+0x10, 8);
    rb_agdc_put32(e+0x20, RB_AGDC_CSC_PRECISION_MIN);
    rb_agdc_put32(e+0x24, 0xbf800000);    /* -1.0f, Apple's coefficient range */
    rb_agdc_put32(e+0x28, 0x3f800000);    /* +1.0f */
    return 0;
}

/* Type 2 Gamma, AGDCFBGammaSegmentEntry_t. getFbCapabilityForGamma 0xbfecda8.
 * Apple's second entry (only when segments >= 2) carries powf(2.0, -25) and
 * powf(2.0, 7) - the 2.0 is the double at 0xc1f3840 - and a 0x200 at +0x28.
 * With RB_AGDC_GAMMA_SEGMENTS 1 that entry is not written at all. */
static inline int rb_agdc_gamma_entry(uint8_t *e) {
    rb_agdc_put32(e+0x00, 0x00088005);
    rb_agdc_put32(e+0x04, 1);
    rb_agdc_put32(e+0x10, 0x10);
    rb_agdc_put32(e+0x24, 0x3f800000);    /* +1.0f */
    rb_agdc_put32(e+0x28, RB_AGDC_GAMMA_CHANNELS);
    return 0;
}

/* Apple's SECOND gamma entry, written only when the segment count is >= 2.
 * Field for field from 0xbfece6f onward, with the qword writes split:
 *   +0x00 0x00088005          movl  $0x88005, 0x32c(%rbx)
 *   +0x04 1                   movq  $1,       0x330(%rbx)
 *   +0x0c 0                   movl  $0,       0x338(%rbx)
 *   +0x10 1                   movq  $1,       0x33c(%rbx)   (format word)
 *   +0x18 devflags | 0x40     movq  ...,      0x344(%rbx)
 *   +0x20 powf(2.0, -25)      movss %xmm0,    0x34c(%rbx)
 *   +0x24 powf(2.0, 7)        movss %xmm0,    0x350(%rbx)
 *   +0x28 0x200               movq  $0x200,   0x354(%rbx)
 * Only the device-flags term is unavailable (this->0x2e798); 2sc's convention
 * gives it 0, so Apple's own OR leaves 0x40. Everything else is Apple's. */
static inline int rb_agdc_gamma_entry2(uint8_t *e) {
    rb_agdc_put32(e+0x00, 0x00088005);
    rb_agdc_put32(e+0x04, 1);
    rb_agdc_put32(e+0x10, 1);
    rb_agdc_put32(e+0x18, 0x40u | RB_AGDC_DEV_FLAGS_NONE);
    rb_agdc_put32(e+0x20, RB_AGDC_GAMMA_MIN_F);
    rb_agdc_put32(e+0x24, RB_AGDC_GAMMA_MAX_F);
    rb_agdc_put32(e+0x28, RB_AGDC_GAMMA_ELEMENTS);
    return 0;
}

/* Type 4 Degamma/Linearization, AGDCFBLinearizationSegmentEntry_t.
 * getFbCapabilityForDegamma 0xbfed0dc writes TWO entries unconditionally and
 * sets count 2; the segment ids are the two qwords at 0xc1f3860. */
static inline int rb_agdc_degamma_entry(uint8_t *e, unsigned index) {
    const uint32_t seg = (index & 1u) ? 0x20u : 0x10u;
    rb_agdc_put32(e+0x00, 0x00080005);
    rb_agdc_put32(e+0x04, 1);
    rb_agdc_put32(e+0x10, 1);
    rb_agdc_put32(e+0x18, seg | RB_AGDC_DEV_FLAGS_NONE);
    rb_agdc_put32(e+0x24, 0x3f800000);    /* +1.0f */
    rb_agdc_put32(e+0x28, 1);
    return 0;
}

/* Type 0x8000 Cursor. getFbCapabilityForCursor 0xbfed1a6 zeroes +0x04..+0x2b,
 * writes 0xe5 at +0x00 and the same queried dimension at +0x14 and +0x18. */
static inline int rb_agdc_cursor_entry(uint8_t *e) {
    rb_agdc_put32(e+0x00, 0xe5);
    rb_agdc_put32(e+0x14, RB_AGDC_CURSOR_DIM_NONE);
    rb_agdc_put32(e+0x18, RB_AGDC_CURSOR_DIM_NONE);
    return 0;
}

static inline int rb_agdc_extcap(void *out, size_t size,
                                 uint32_t hactive, uint32_t vactive) {
    if (!out || size != RB_AGDC_EXTCAP_SIZE) return -1;
    uint8_t *p=(uint8_t *)out;
    if (p[0] != 0) return -1;             /* one framebuffer, index 0 */
    uint32_t type=0;
    for (unsigned i=0;i<4;i++) type|=(uint32_t)p[4+i]<<(8*i);
    /* The scaler is the only type whose reply needs the real mode, so it keeps
     * its own refusal: with no timing there is nothing truthful to say about
     * extents. The other four are extent-free. */
    if (type == RB_AGDC_CAP_SCALER && (!hactive || !vactive)) return -1;
    if (type != RB_AGDC_CAP_SCALER && type != RB_AGDC_CAP_CSC &&
        type != RB_AGDC_CAP_GAMMA && type != RB_AGDC_CAP_DEGAMMA &&
        type != RB_AGDC_CAP_CURSOR)
        return RB_AGDC_EXTCAP_UNSUPPORTED;
    /* Zero from the count on, as the scaler path has always done: Apple's CSC
     * and cursor arms write individual fields over whatever the caller passed,
     * and zeroing first is strictly more defined than trusting it. */
    for (size_t i=RB_AGDC_EXTCAP_COUNT_OFF;i<size;i++) p[i]=0;
    uint8_t *e=p+RB_AGDC_EXTCAP_ENTRY_OFF;
    if (type == RB_AGDC_CAP_SCALER) {
        rb_agdc_put32(p+RB_AGDC_EXTCAP_COUNT_OFF, 1);
        return rb_agdc_scaler_entry(e, vactive, hactive);
    }
    if (type == RB_AGDC_CAP_CSC) {
        rb_agdc_put32(p+RB_AGDC_EXTCAP_COUNT_OFF, 1);
        return rb_agdc_csc_entry(e);
    }
    if (type == RB_AGDC_CAP_GAMMA) {
        /* Same capacity guard the degamma arm has always had: the count we
         * publish must be a count of entries that actually fit. */
        if ((size_t)RB_AGDC_EXTCAP_ENTRY_OFF
            + (size_t)RB_AGDC_GAMMA_SEGMENTS*RB_AGDC_EXTCAP_ENTRY_SZ > size)
            return -1;
        rb_agdc_put32(p+RB_AGDC_EXTCAP_COUNT_OFF, RB_AGDC_GAMMA_SEGMENTS);
        if (rb_agdc_gamma_entry(e)) return -1;
        if (RB_AGDC_GAMMA_SEGMENTS < 2u) return 0;
        return rb_agdc_gamma_entry2(e+RB_AGDC_EXTCAP_ENTRY_SZ);
    }
    if (type == RB_AGDC_CAP_DEGAMMA) {
        /* Two entries, so the capacity must really hold them. */
        if (size < RB_AGDC_EXTCAP_ENTRY_OFF + 2u*RB_AGDC_EXTCAP_ENTRY_SZ)
            return -1;
        rb_agdc_put32(p+RB_AGDC_EXTCAP_COUNT_OFF, 2);
        if (rb_agdc_degamma_entry(e, 0)) return -1;
        return rb_agdc_degamma_entry(e+RB_AGDC_EXTCAP_ENTRY_SZ, 1);
    }
    rb_agdc_put32(p+RB_AGDC_EXTCAP_COUNT_OFF, 1);
    return rb_agdc_cursor_entry(e);
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
