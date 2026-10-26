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
    printf("agdc: bounded replies, AMD identity, kernel pointer layout, "
           "link config overlay: %s\n",bad?"FAILED":"passed");
    return bad;
#undef A
}
