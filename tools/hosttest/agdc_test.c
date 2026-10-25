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
    uint8_t b[RB_AGDC_CAP_SIZE+2], original[sizeof b];
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
    /* Boot 156's next request has input data; it is an unimplemented
     * operation, not a malformed vendor-info request. Refusals don't write. */
    A(rb_agdc_reply(0x921,0xb0,b+1,&out_size,1,2)==RB_AGDC_UNSUPPORTED);
    A(rb_agdc_reply(0x711,0x196c,b+1,&out_size,1,2)==RB_AGDC_UNSUPPORTED);
    A(rb_agdc_reply(3,4,b+1,NULL,1,2)==RB_AGDC_UNSUPPORTED);
    A(rb_agdc_reply(1,4,b+1,&out_size,1,2)==RB_AGDC_BAD_ARGUMENT);
    A(rb_agdc_reply(0x980,1,b+1,&out_size,1,2)==RB_AGDC_BAD_ARGUMENT);
    A(rb_agdc_reply(1,0,b+1,NULL,1,2)==RB_AGDC_BAD_ARGUMENT);
    A(!memcmp(b,original,sizeof b) && out_size==RB_AGDC_VENDOR_SIZE);
    A(rb_agdc_reply(1,0,b+1,&out_size,1,2)==0);
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
    A(rb_agdc_reply(0x980,0,b+1,&out_size,pci,fb)==0);
    A(b[0]==0xa5 && b[sizeof b-1]==0xa5);
    A(le(p+0x20,4)==1 && le(p+0x30,4)==1);
    A(le(p+0x34,8)==pci && le(p+0x3c,8)==fb);
    for (unsigned i=0; i<0x20; i++) A(p[i]==0);
    for (unsigned i=0x44; i<RB_AGDC_CAP_SIZE; i++) A(p[i]==0);
    printf("agdc: bounded replies, AMD identity, kernel pointer layout: %s\n",bad?"FAILED":"passed");
    return bad;
#undef A
}
