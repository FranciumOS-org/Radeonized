/* SPDX-License-Identifier: MIT */
#include "rb_agdc.h"
#include <stdio.h>
#include <string.h>

static uint64_t le(const uint8_t *p, unsigned n) {
    uint64_t v=0; for (unsigned i=0; i<n; i++) v|=(uint64_t)p[i]<<(8*i); return v;
}
int agdc_tests(void) {
    int bad=0;
#define A(x) do { if (!(x)) { printf("  FAIL AGDC:%d: %s\n",__LINE__,#x); bad++; } } while (0)
    uint8_t b[RB_AGDC_LINK_SIZE+RB_AGDC_CAP_SIZE+2], original[sizeof b];
    memset(b,0xa5,sizeof b); memcpy(original,b,sizeof b);
    /* Malformed requests must not touch the buffer, including oversized
     * lengths. The two pointers must exist before any capability is claimed. */
    A(rb_agdc_vendor(NULL,RB_AGDC_VENDOR_SIZE)<0);
    A(rb_agdc_capability(NULL,RB_AGDC_CAP_SIZE,1,2)<0);
    for (size_t n=0; n<sizeof b; n++) {
        if (n!=RB_AGDC_VENDOR_SIZE) {
            A(rb_agdc_vendor(b+1,n)<0); A(!memcmp(b,original,sizeof b));
        }
        if (n!=RB_AGDC_CAP_SIZE) {
            A(rb_agdc_capability(b+1,n,1,2)<0); A(!memcmp(b,original,sizeof b));
        }
    }
    A(rb_agdc_capability(b+1,RB_AGDC_CAP_SIZE,0,2)<0);
    A(rb_agdc_capability(b+1,RB_AGDC_CAP_SIZE,1,0)<0);
    A(!memcmp(b,original,sizeof b));
    size_t out_size=RB_AGDC_VENDOR_SIZE;
    /* 0x711 still has no implementation; it is an unimplemented operation,
     * not a malformed vendor-info request. Refusals don't write. */
    A(rb_agdc_reply(0x711,0x196c,b+1,&out_size,1,2,NULL)==RB_AGDC_UNSUPPORTED);
    A(rb_agdc_reply(3,4,b+1,NULL,1,2,NULL)==RB_AGDC_UNSUPPORTED);
    /* 2rd: 0x921 without real timing must refuse rather than zero-fill. A
     * zero reply would PASS IOPresentment's gather and poison its cache. */
    A(rb_agdc_reply(0x921,RB_AGDC_LINK_SIZE,b+1,&out_size,1,2,NULL)
      ==RB_AGDC_UNSUPPORTED);
    A(rb_agdc_reply(1,4,b+1,&out_size,1,2,NULL)==RB_AGDC_BAD_ARGUMENT);
    A(rb_agdc_reply(0x980,1,b+1,&out_size,1,2,NULL)==RB_AGDC_BAD_ARGUMENT);
    A(rb_agdc_reply(1,0,b+1,NULL,1,2,NULL)==RB_AGDC_BAD_ARGUMENT);
    A(!memcmp(b,original,sizeof b) && out_size==RB_AGDC_VENDOR_SIZE);
    A(rb_agdc_reply(1,0,b+1,&out_size,1,2,NULL)==0);
    memset(b,0xa5,sizeof b);
    A(rb_agdc_vendor(b+1,RB_AGDC_VENDOR_SIZE)==0);
    A(b[0]==0xa5 && b[1+RB_AGDC_VENDOR_SIZE]==0xa5);
    const uint8_t *p=b+1;
    A(le(p,4)==0x30000 && !memcmp(p+4,"AMD\0",4));
    A(le(p+0x24,4)==0x1002 && le(p+0x28,4)==2);
    for (unsigned i=8; i<0x24; i++) A(p[i]==0);
    memset(b,0xa5,sizeof b);
    const uint64_t pci=0xffffff8001234567ULL, fb=0xffffff800abcdef0ULL;
    out_size=RB_AGDC_CAP_SIZE;
    A(rb_agdc_reply(0x980,0,b+1,&out_size,pci,fb,NULL)==0);
    A(b[0]==0xa5 && b[sizeof b-1]==0xa5);
    A(le(p+0x20,4)==1 && le(p+0x30,4)==1);
    A(le(p+0x34,8)==pci && le(p+0x3c,8)==fb);
    for (unsigned i=0; i<0x20; i++) A(p[i]==0);
    for (unsigned i=0x44; i<RB_AGDC_CAP_SIZE; i++) A(p[i]==0);

    /* 2rd: AGDCLinkConfig_t. The timing is a recognisable ramp so the test
     * can prove WHERE it landed, that the two AGDC overlay words replaced
     * __reservedA, and that nothing past 0xb0 moved. */
    uint8_t timing[RB_AGDC_TIMING_SIZE];
    for (unsigned i=0; i<sizeof timing; i++) timing[i]=(uint8_t)i;
    memset(b,0xa5,sizeof b); memcpy(original,b,sizeof b);
    A(rb_agdc_link_config(NULL,RB_AGDC_LINK_SIZE,timing,0,RB_AGDC_LINK_BIT3,0,0)<0);
    for (size_t n=0; n<=RB_AGDC_LINK_SIZE+4; n++) {
        if (n==RB_AGDC_LINK_SIZE) continue;
        A(rb_agdc_link_config(b+1,n,timing,0,RB_AGDC_LINK_BIT3,0,0)<0);
        A(!memcmp(b,original,sizeof b));
    }
    /* No timing is a refusal, not a zero fill. */
    A(rb_agdc_link_config(b+1,RB_AGDC_LINK_SIZE,NULL,0,RB_AGDC_LINK_BIT3,0,0)<0);
    A(!memcmp(b,original,sizeof b));
    /* Bit 0 would claim the getPortStatus sub-protocol (selector 0x925).
     * The index MUST be valid here or the index check refuses first and this
     * assertion passes for the wrong reason - it did, until the deliberate
     * break of the bit-0 guard failed to trip it (rule 26, on a test). */
    memset(b,0xa5,sizeof b); memset(b+1,0,RB_AGDC_LINK_SIZE);
    memcpy(original,b,sizeof b);
    A(rb_agdc_link_config(b+1,RB_AGDC_LINK_SIZE,timing,0,
                          RB_AGDC_LINK_BIT3|RB_AGDC_LINK_PORT,0,0)<0);
    A(!memcmp(b,original,sizeof b));
    /* Same for the timing and size refusals: with a valid index they must
     * still refuse, so each guard is exercised on its own. */
    A(rb_agdc_link_config(b+1,RB_AGDC_LINK_SIZE,NULL,0,RB_AGDC_LINK_BIT3,0,0)<0);
    A(rb_agdc_link_config(b+1,RB_AGDC_LINK_SIZE-1,timing,0,RB_AGDC_LINK_BIT3,0,0)<0);
    A(!memcmp(b,original,sizeof b));
    /* The framebuffer index is IOPresentment's, read out of the output
     * buffer; this bridge advertises exactly one, so only 0 is ours. */
    memset(b+1,0,RB_AGDC_LINK_SIZE); b[1]=1;
    A(rb_agdc_link_config(b+1,RB_AGDC_LINK_SIZE,timing,0,RB_AGDC_LINK_BIT3,0,0)<0);
    A(b[1]==1);
    memset(b,0xa5,sizeof b); memset(b+1,0,RB_AGDC_LINK_SIZE);
    out_size=RB_AGDC_LINK_SIZE;
    /* Apple checks only the OUTPUT length, so any inLen must be accepted -
     * input and output are the same structure. A wrong OUTPUT length must
     * still refuse without writing. */
    A(rb_agdc_reply(0x921,0,b+1,&out_size,pci,fb,timing)==0);
    memset(b,0xa5,sizeof b); memset(b+1,0,RB_AGDC_LINK_SIZE);
    memcpy(original,b,sizeof b);
    out_size=RB_AGDC_LINK_SIZE-1;
    A(rb_agdc_reply(0x921,RB_AGDC_LINK_SIZE,b+1,&out_size,pci,fb,timing)
      ==RB_AGDC_BAD_ARGUMENT);
    A(!memcmp(b,original,sizeof b));
    out_size=RB_AGDC_LINK_SIZE;
    A(rb_agdc_reply(0x921,0x196c,b+1,&out_size,pci,fb,timing)==0);
    A(b[0]==0xa5 && b[1+RB_AGDC_LINK_SIZE]==0xa5);
    A(le(p,4)==0 && le(p+0x04,4)==RB_AGDC_LINK_NONE);
    A(le(p+0x08,8)==(RB_AGDC_LINK_BIT3|RB_AGDC_LINK_NOLINK));
    A(!(le(p+0x08,8)&RB_AGDC_LINK_PORT));
    /* The V2 body begins at +0x04, so its first readable field,
     * horizontalScaledInset at struct+0xc, lands at reply+0x10 - and the
     * three overlaid __reservedA words are gone. */
    for (unsigned i=0x0c; i<RB_AGDC_TIMING_SIZE; i++)
        A(p[RB_AGDC_TIMING_OFF+i]==(uint8_t)i);
    A(p[0x10]==0x0c && p[0xa3]==0x9f);
    A(le(p+0xa4,4)==RB_AGDC_STREAM_NONE && le(p+0xa8,4)==0);
    A(le(p+0xac,4)==RB_AGDC_STREAM_BAD);

    /* 2rg: the fields IOPresentment actually consumes must land at the reply
     * offsets it reads them from - 0x19afc + these, measured in
     * __gatherCapabilities and IOPresentmentCreateEndPointDictionary. Pinning
     * the literals ties our V2 constants to that measurement, so editing
     * either one trips here instead of silently shifting the timing. */
    A(RB_AGDC_TIMING_OFF+RB_AGDC_V2_PIXELCLOCK==0x2c);
    A(RB_AGDC_TIMING_OFF+RB_AGDC_V2_HACTIVE  ==0x44);
    A(RB_AGDC_TIMING_OFF+RB_AGDC_V2_HBLANKING==0x48);
    A(RB_AGDC_TIMING_OFF+RB_AGDC_V2_VACTIVE  ==0x54);
    A(RB_AGDC_TIMING_OFF+RB_AGDC_V2_VBLANKING==0x58);
    /* and the ramp proves the copy put each one there. */
    A(p[0x2c]==RB_AGDC_V2_PIXELCLOCK && p[0x44]==RB_AGDC_V2_HACTIVE);
    A(p[0x48]==RB_AGDC_V2_HBLANKING && p[0x54]==RB_AGDC_V2_VACTIVE);
    A(p[0x58]==RB_AGDC_V2_VBLANKING);

    /* A zero pixelClock is a REFUSAL: it PASSES Apple's `total > 0.0f` guard
     * and publishes a 0.0 Hz RefreshTime, which is the one poisoning left
     * after 2rd. The index must be zero here or the index check refuses first
     * and this passes for the wrong reason - the same trap the bit-0 guard
     * fell into above. The actives stay non-zero so err 4 cannot fire either. */
    uint8_t noclock[RB_AGDC_TIMING_SIZE];
    memcpy(noclock,timing,sizeof noclock);
    memset(noclock+RB_AGDC_V2_PIXELCLOCK,0,8);
    A(le(noclock+RB_AGDC_V2_HACTIVE,4)!=0);
    A(le(noclock+RB_AGDC_V2_VACTIVE,4)!=0);
    memset(b,0xa5,sizeof b); memset(b+1,0,RB_AGDC_LINK_SIZE);
    memcpy(original,b,sizeof b);
    A(rb_agdc_link_config(b+1,RB_AGDC_LINK_SIZE,noclock,0,
                          RB_AGDC_LINK_BIT3,0,0)<0);
    A(!memcmp(b,original,sizeof b));
    out_size=RB_AGDC_LINK_SIZE;
    A(rb_agdc_reply(0x921,0,b+1,&out_size,pci,fb,noclock)==RB_AGDC_BAD_ARGUMENT);
    A(!memcmp(b,original,sizeof b));
    /* The refusal is scoped to 0x921: 0x711 never reads the clock, so a timing
     * without one must still answer the scaler query. */
    static uint8_t nc[RB_AGDC_EXTCAP_SIZE+2];
    memset(nc,0xa5,sizeof nc); memset(nc+1,0,RB_AGDC_EXTCAP_SIZE);
    rb_agdc_put32(nc+1+4,RB_AGDC_CAP_SCALER);
    out_size=RB_AGDC_EXTCAP_SIZE;
    A(rb_agdc_reply(0x711,0,nc+1,&out_size,pci,fb,noclock)==0);
    A(le(nc+1+RB_AGDC_EXTCAP_COUNT_OFF,4)==1);
    A(nc[0]==0xa5 && nc[1+RB_AGDC_EXTCAP_SIZE]==0xa5);
    /* 2rf: AGDCFBGetExtendedCapability_t, 0x196c = 0xc header + 8 * 0x32c. */
    static uint8_t x[RB_AGDC_EXTCAP_SIZE+2], xorig[sizeof x];
    const uint8_t *q=x+1;
    const uint32_t HA=1920, VA=1080;
    memset(x,0xa5,sizeof x); memcpy(xorig,x,sizeof x);
    A(rb_agdc_extcap(NULL,RB_AGDC_EXTCAP_SIZE,HA,VA)<0);
    /* Wrong length refuses without writing. Sampled rather than exhaustive:
     * 0x196c memcmps of 6.5 KB each is not worth the host-test seconds. */
    size_t probe[]={0,1,0xb0,0xdc,RB_AGDC_EXTCAP_SIZE-1,RB_AGDC_EXTCAP_SIZE+1};
    for (unsigned i=0;i<sizeof probe/sizeof probe[0];i++) {
        A(rb_agdc_extcap(x+1,probe[i],HA,VA)<0);
        A(!memcmp(x,xorig,sizeof x));
    }
    /* Index must be 0; the type field is only reached once it is. */
    memset(x,0,sizeof x); x[1]=1;
    rb_agdc_put32(x+1+4,RB_AGDC_CAP_SCALER);
    A(rb_agdc_extcap(x+1,RB_AGDC_EXTCAP_SIZE,HA,VA)<0);
    /* 2sc: the twelve types Apple's own dispatcher does NOT answer are still
     * Unsupported and must not write a byte. 0x20 is in this list on purpose:
     * getFbCapabilityForRotation exists in AMDRadeonX6000Framebuffer and is
     * unreachable from getFbExtendedCapability, so a real Navi 21 Mac refuses
     * it too. The five Apple answers are asserted below. */
    uint32_t types[]={0x8,0x20,0x40,0x80,0x100,0x200,
                      0x400,0x800,0x1000,0x2000,0x4000,0,0x12345};
    for (unsigned i=0;i<sizeof types/sizeof types[0];i++) {
        memset(x,0xa5,sizeof x); memset(x+1,0,RB_AGDC_EXTCAP_SIZE);
        rb_agdc_put32(x+1+4,types[i]);
        memcpy(xorig,x,sizeof x);
        A(rb_agdc_extcap(x+1,RB_AGDC_EXTCAP_SIZE,HA,VA)
          ==RB_AGDC_EXTCAP_UNSUPPORTED);
        A(!memcmp(x,xorig,sizeof x));
    }
    /* 2sc: the four types 2sc added. Each is pre-filled with garbage from the
     * count onward, so a field this code fails to write shows up as 0x5c
     * rather than as a zero that happened to be right. The index and type
     * words at +0x00/+0x04 must survive, and the bytes either side of the
     * structure must not be touched. */
    {
        struct { uint32_t ty, count, f00, off_a, val_a; } cases[]={
            { RB_AGDC_CAP_CSC,     1, 0x00080005, 0x20, RB_AGDC_CSC_PRECISION_MIN },
            { RB_AGDC_CAP_GAMMA,   RB_AGDC_GAMMA_SEGMENTS, 0x00088005, 0x10, 0x10 },
            { RB_AGDC_CAP_DEGAMMA, 2, 0x00080005, 0x18, 0x10 },
            { RB_AGDC_CAP_CURSOR,  1, 0xe5,       0x14, RB_AGDC_CURSOR_DIM_NONE },
        };
        for (unsigned i=0;i<sizeof cases/sizeof cases[0];i++) {
            memset(x,0xa5,sizeof x); memset(x+1,0,RB_AGDC_EXTCAP_SIZE);
            rb_agdc_put32(x+1+4,cases[i].ty);
            memset(x+1+RB_AGDC_EXTCAP_COUNT_OFF,0x5c,
                   RB_AGDC_EXTCAP_SIZE-RB_AGDC_EXTCAP_COUNT_OFF);
            /* These four need no mode, so they must answer even with none. */
            A(rb_agdc_extcap(x+1,RB_AGDC_EXTCAP_SIZE,0,0)==0);
            A(x[0]==0xa5 && x[1+RB_AGDC_EXTCAP_SIZE]==0xa5);
            A(le(q,4)==0 && le(q+4,4)==cases[i].ty);
            A(le(q+RB_AGDC_EXTCAP_COUNT_OFF,4)==cases[i].count);
            const uint8_t *ce=q+RB_AGDC_EXTCAP_ENTRY_OFF;
            A(le(ce+0x00,4)==cases[i].f00);
            A(le(ce+cases[i].off_a,4)==cases[i].val_a);
            /* Nothing from the count onward may still be garbage. */
            for (size_t b=RB_AGDC_EXTCAP_COUNT_OFF;b<RB_AGDC_EXTCAP_SIZE;b++)
                A(q[b]!=0x5c);
        }
        /* Degamma writes a SECOND entry, with the other segment id, and
         * refuses outright if the capacity cannot hold both. */
        memset(x,0xa5,sizeof x); memset(x+1,0,RB_AGDC_EXTCAP_SIZE);
        rb_agdc_put32(x+1+4,RB_AGDC_CAP_DEGAMMA);
        A(rb_agdc_extcap(x+1,RB_AGDC_EXTCAP_SIZE,0,0)==0);
        A(le(q+RB_AGDC_EXTCAP_ENTRY_OFF+RB_AGDC_EXTCAP_ENTRY_SZ+0x18,4)==0x20);
        A(le(q+RB_AGDC_EXTCAP_ENTRY_OFF+0x18,4)==0x10);
        /* 2u9: GAMMA now writes Apple's second entry too, and it is the only
         * entry in the whole reply whose element count is one of Apple's own
         * literals. Check every field of it, because the element count at
         * +0x28 is the FIELD UNDER TEST - 2u7 read CoreDisplay refusing on a
         * zero there - and a silent 0 is exactly the failure this guards. */
        memset(x,0xa5,sizeof x); memset(x+1,0,RB_AGDC_EXTCAP_SIZE);
        rb_agdc_put32(x+1+4,RB_AGDC_CAP_GAMMA);
        A(rb_agdc_extcap(x+1,RB_AGDC_EXTCAP_SIZE,0,0)==0);
        A(le(q+RB_AGDC_EXTCAP_COUNT_OFF,4)==2);
        {
            const uint8_t *g2=q+RB_AGDC_EXTCAP_ENTRY_OFF+RB_AGDC_EXTCAP_ENTRY_SZ;
            A(le(g2+0x00,4)==0x00088005);
            A(le(g2+0x04,4)==1);
            A(le(g2+0x10,4)==1);          /* entry 2's format word, NOT 0x10 */
            A(le(g2+0x18,4)==0x40);
            A(le(g2+0x20,4)==RB_AGDC_GAMMA_MIN_F);
            A(le(g2+0x24,4)==RB_AGDC_GAMMA_MAX_F);
            A(le(g2+0x28,4)==0x200);      /* the element count, non-zero */
            /* And entry 1 must still be entry 1: its format word is 0x10 and
             * its own count is the substitute, unchanged by this addition. */
            A(le(q+RB_AGDC_EXTCAP_ENTRY_OFF+0x10,4)==0x10);
            A(le(q+RB_AGDC_EXTCAP_ENTRY_OFF+0x28,4)==RB_AGDC_GAMMA_CHANNELS);
        }
        /* Two entries means the capacity must really hold two: a reply that
         * publishes a count of 2 into a buffer that fits one is worse than a
         * refusal. The size check is exact, so only the real size is accepted. */
        A(rb_agdc_extcap(x+1,RB_AGDC_EXTCAP_ENTRY_OFF
                              +RB_AGDC_EXTCAP_ENTRY_SZ,0,0)<0);
    }
    /* No real timing: nothing truthful to say about a scaler, so refuse. */
    memset(x,0xa5,sizeof x); memset(x+1,0,RB_AGDC_EXTCAP_SIZE);
    rb_agdc_put32(x+1+4,RB_AGDC_CAP_SCALER); memcpy(xorig,x,sizeof x);
    A(rb_agdc_extcap(x+1,RB_AGDC_EXTCAP_SIZE,0,VA)<0);
    A(rb_agdc_extcap(x+1,RB_AGDC_EXTCAP_SIZE,HA,0)<0);
    A(!memcmp(x,xorig,sizeof x));
    /* The scaler answer. Pre-fill from +0x08 with garbage: everything from
     * the count onward must be REWRITTEN, not merely added to. Without this
     * the success case starts from a zeroed buffer and a missing clear is
     * invisible - which is exactly what a deliberate break showed. */
    memset(x+1+RB_AGDC_EXTCAP_COUNT_OFF,0x5c,
           RB_AGDC_EXTCAP_SIZE-RB_AGDC_EXTCAP_COUNT_OFF);
    A(rb_agdc_extcap(x+1,RB_AGDC_EXTCAP_SIZE,HA,VA)==0);
    A(x[0]==0xa5 && x[1+RB_AGDC_EXTCAP_SIZE]==0xa5);
    A(le(q,4)==0 && le(q+4,4)==RB_AGDC_CAP_SCALER);   /* preserved */
    A(le(q+RB_AGDC_EXTCAP_COUNT_OFF,4)==1);
    const uint8_t *e=q+RB_AGDC_EXTCAP_ENTRY_OFF;
    A(le(e+0x000,4)==0x00080004);
    A(le(e+0x01c,4)==1 && le(e+0x1a4,4)==1);
    /* Source and destination extents are EQUAL in both blocks: that is what
     * makes this entry mean "1:1, no scaling" whichever slot is the width. */
    A(le(e+0x024,4)==le(e+0x02c,4) && le(e+0x028,4)==le(e+0x030,4));
    A(le(e+0x1ac,4)==le(e+0x1b4,4) && le(e+0x1b0,4)==le(e+0x1b8,4));
    A(le(e+0x024,4)==VA && le(e+0x028,4)==HA);
    A(le(e+0x1ac,4)==VA && le(e+0x1b0,4)==HA);
    A(le(e+0x034,4)==0x00050005 && le(e+0x038,4)==4);
    A(le(e+0x1bc,4)==0x00050005 && le(e+0x1c0,4)==4);
    /* Only ONE entry is claimed, so entries 1..7 must be entirely zero -
     * IOPresentment offered room for 8 and a stale byte there would be a
     * capability nobody implements. */
    for (size_t i=RB_AGDC_EXTCAP_ENTRY_SZ;
         i<RB_AGDC_EXTCAP_ENTRIES*RB_AGDC_EXTCAP_ENTRY_SZ; i++)
        A(e[i]==0);
    /* Through the dispatcher, with the timing carrying the active pixels at
     * V2+0x40 and V2+0x50 (offsetof, same pair 0x921 uses). */
    memset(timing,0,sizeof timing);
    rb_agdc_put32(timing+0x40,HA); rb_agdc_put32(timing+0x50,VA);
    memset(x,0xa5,sizeof x); memset(x+1,0,RB_AGDC_EXTCAP_SIZE);
    rb_agdc_put32(x+1+4,RB_AGDC_CAP_SCALER);
    memcpy(xorig,x,sizeof x);
    out_size=RB_AGDC_EXTCAP_SIZE;
    A(rb_agdc_reply(0x711,RB_AGDC_EXTCAP_SIZE,x+1,&out_size,pci,fb,NULL)
      ==RB_AGDC_UNSUPPORTED);
    A(!memcmp(x,xorig,sizeof x));
    A(rb_agdc_reply(0x711,RB_AGDC_EXTCAP_SIZE,x+1,&out_size,pci,fb,timing)==0);
    A(le(q+RB_AGDC_EXTCAP_COUNT_OFF,4)==1);
    A(le(q+RB_AGDC_EXTCAP_ENTRY_OFF+0x024,4)==VA);
    A(le(q+RB_AGDC_EXTCAP_ENTRY_OFF+0x028,4)==HA);
    printf("agdc: bounded replies, AMD identity, kernel pointer layout, "
           "link config overlay, all five capability types: %s\n",bad?"FAILED":"passed");
    return bad;
#undef A
}
