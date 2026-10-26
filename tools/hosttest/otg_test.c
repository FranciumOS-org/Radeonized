/* SPDX-License-Identifier: MIT */
#include "rb_otg.h"
#include "rb_agdc.h"
#include <stdio.h>
#include <string.h>

static uint64_t le(const uint8_t *p, unsigned n) {
    uint64_t v=0; for (unsigned i=0;i<n;i++) v|=(uint64_t)p[i]<<(8*i); return v;
}

/* This machine's OTG0, measured in boots 2oq and 2os. Not a synthetic case:
 * if the decode is wrong, these numbers stop producing 1920x1080. */
static const struct rb_otg_timing tufa15 = {
    .h_total           = 0x0000083a,
    .h_blank_start_end = 0x007f07ff,
    .h_sync_a          = 0x003c0000,
    .h_sync_a_cntl     = 0,
    .v_total           = 0x00000469,
    .v_blank_start_end = 0x0014044c,
    .v_sync_a          = 0x00050000,
    .v_sync_a_cntl     = 0,
    .refresh_mhz       = 143833,
};

int otg_tests(void) {
    int bad=0;
#define A(x) do { if (!(x)) { printf("  FAIL OTG:%d: %s\n",__LINE__,#x); bad++; } } while (0)
    uint8_t v2[RB_V2_SIZE], original[sizeof v2];

    /* The V2 layout this converter writes must be the one rb_agdc.h already
     * pins from the consumer side, and the struct must close at 0xa0 - which
     * is also AGDC's timing span. Two independent derivations agreeing. */
    A(RB_V2_PIXELCLOCK == RB_AGDC_V2_PIXELCLOCK);
    A(RB_V2_HACTIVE    == RB_AGDC_V2_HACTIVE);
    A(RB_V2_HBLANKING  == RB_AGDC_V2_HBLANKING);
    A(RB_V2_VACTIVE    == RB_AGDC_V2_VACTIVE);
    A(RB_V2_VBLANKING  == RB_AGDC_V2_VBLANKING);
    A(RB_V2_SIZE       == RB_AGDC_TIMING_SIZE);

    /* Malformed requests must refuse without touching the output. */
    memset(v2,0xa5,sizeof v2); memcpy(original,v2,sizeof v2);
    A(rb_otg_to_v2(NULL,v2,sizeof v2) == -RB_OTG_E_ARGS);
    A(rb_otg_to_v2(&tufa15,NULL,sizeof v2) == -RB_OTG_E_ARGS);
    for (size_t n=0;n<sizeof v2+2;n++)
        if (n != RB_V2_SIZE) {
            A(rb_otg_to_v2(&tufa15,v2,n) == -RB_OTG_E_ARGS);
            A(!memcmp(v2,original,sizeof v2));
        }

    /* The real panel. Every field is the measured hardware's own. */
    memset(v2,0xa5,sizeof v2);
    A(rb_otg_to_v2(&tufa15,v2,sizeof v2) == RB_OTG_OK);
    A(le(v2+RB_V2_HACTIVE,4)     == 1920);
    A(le(v2+RB_V2_HBLANKING,4)   == 186);
    A(le(v2+RB_V2_HSYNCOFFSET,4) == 59);
    A(le(v2+RB_V2_HSYNCWIDTH,4)  == 60);
    A(le(v2+RB_V2_VACTIVE,4)     == 1080);
    A(le(v2+RB_V2_VBLANKING,4)   == 49);
    A(le(v2+RB_V2_VSYNCOFFSET,4) == 29);
    A(le(v2+RB_V2_VSYNCWIDTH,4)  == 5);
    /* The porches must close on both axes - the check that establishes the
     * field order in the first place. */
    A(le(v2+RB_V2_HSYNCOFFSET,4) + le(v2+RB_V2_HSYNCWIDTH,4) <= le(v2+RB_V2_HBLANKING,4));
    A(le(v2+RB_V2_HACTIVE,4) + le(v2+RB_V2_HBLANKING,4) == 2106);
    A(le(v2+RB_V2_VACTIVE,4) + le(v2+RB_V2_VBLANKING,4) == 1129);
    /* 2106 * 1129 * 143.833 Hz. Stated as the arithmetic, not as a constant. */
    A(le(v2+RB_V2_PIXELCLOCK,8) == (uint64_t)2106*1129*143833/1000);
    A(le(v2+RB_V2_PIXELCLOCK,8) == le(v2+RB_V2_MINPIXELCLOCK,8));
    A(le(v2+RB_V2_PIXELCLOCK,8) == le(v2+RB_V2_MAXPIXELCLOCK,8));
    /* POL clear means positive, and IOKit's flag is the inverse of OTG's. */
    A(le(v2+RB_V2_HSYNCCONFIG,4) == RB_IO_SYNC_POSITIVE);
    A(le(v2+RB_V2_VSYNCCONFIG,4) == RB_IO_SYNC_POSITIVE);
    /* Nothing outside the fields written may be non-zero: a stale byte here
     * would reach Apple's capability cache. */
    A(le(v2+0x00,4)==0 && le(v2+0x18,4)==0 && le(v2+0x60,4)==0 && le(v2+0x80,4)==0);

    /* Negative polarity must clear the flags rather than leave them set. */
    struct rb_otg_timing neg = tufa15;
    neg.h_sync_a_cntl = RB_OTG_SYNC_POL_NEG;
    neg.v_sync_a_cntl = RB_OTG_SYNC_POL_NEG;
    A(rb_otg_to_v2(&neg,v2,sizeof v2) == RB_OTG_OK);
    A(le(v2+RB_V2_HSYNCCONFIG,4)==0 && le(v2+RB_V2_VSYNCCONFIG,4)==0);

    /* Each refusal must name itself, and must not have written anything.
     * An unprogrammed OTG reads zeros, which is the case that matters most:
     * it must refuse, never publish a zero timing (2rd/2rg - IOPresentment
     * validates no timing field, so a zero reply would poison its cache). */
    struct rb_otg_timing z = {0};
    memset(v2,0xa5,sizeof v2); memcpy(original,v2,sizeof v2);
    A(rb_otg_to_v2(&z,v2,sizeof v2) == -RB_OTG_E_HACTIVE);
    A(!memcmp(v2,original,sizeof v2));

    struct rb_otg_timing t;
    t = tufa15; t.h_blank_start_end = 0x007f007f;       /* halves equal */
    A(rb_otg_to_v2(&t,v2,sizeof v2) == -RB_OTG_E_HACTIVE);
    t = tufa15; t.v_blank_start_end = 0x00140014;
    A(rb_otg_to_v2(&t,v2,sizeof v2) == -RB_OTG_E_VACTIVE);
    t = tufa15; t.h_total = 0;
    A(rb_otg_to_v2(&t,v2,sizeof v2) == -RB_OTG_E_HTOTAL);
    t = tufa15; t.v_total = 0;
    A(rb_otg_to_v2(&t,v2,sizeof v2) == -RB_OTG_E_VTOTAL);
    t = tufa15; t.h_total = 1920;                        /* no room to blank */
    A(rb_otg_to_v2(&t,v2,sizeof v2) == -RB_OTG_E_HTOTAL);
    t = tufa15; t.v_total = 1080;
    A(rb_otg_to_v2(&t,v2,sizeof v2) == -RB_OTG_E_VTOTAL);
    t = tufa15; t.h_sync_a = 0x003c0001;                 /* sync not at 0 */
    A(rb_otg_to_v2(&t,v2,sizeof v2) == -RB_OTG_E_HSYNC);
    t = tufa15; t.h_sync_a = 0;                          /* zero width */
    A(rb_otg_to_v2(&t,v2,sizeof v2) == -RB_OTG_E_HSYNC);
    t = tufa15; t.v_sync_a = 0;
    A(rb_otg_to_v2(&t,v2,sizeof v2) == -RB_OTG_E_VSYNC);
    t = tufa15; t.h_sync_a = 0x07f00000;                 /* wider than the porch */
    A(rb_otg_to_v2(&t,v2,sizeof v2) == -RB_OTG_E_HSYNC);
    t = tufa15; t.refresh_mhz = 0;                       /* never measured */
    A(rb_otg_to_v2(&t,v2,sizeof v2) == -RB_OTG_E_REFRESH);

    /* A refused conversion must leave the AGDC reply refusable too: a V2 body
     * that was never written has a zero pixelClock, which rb_agdc_link_config
     * refuses (2rg). The two guards have to compose. */
    uint8_t link[RB_AGDC_LINK_SIZE];
    memset(link,0,sizeof link);
    uint8_t zero_v2[RB_V2_SIZE]; memset(zero_v2,0,sizeof zero_v2);
    A(rb_agdc_link_config(link,sizeof link,zero_v2,
                          RB_AGDC_LINK_NONE,
                             RB_AGDC_LINK_BIT3|RB_AGDC_LINK_NOLINK,
                             RB_AGDC_STREAM_NONE,RB_AGDC_STREAM_BAD) < 0);
    /* ... and a converted one must be accepted by it. */
    A(rb_otg_to_v2(&tufa15,v2,sizeof v2) == RB_OTG_OK);
    memset(link,0,sizeof link);
    A(rb_agdc_link_config(link,sizeof link,v2,
                          RB_AGDC_LINK_NONE,
                             RB_AGDC_LINK_BIT3|RB_AGDC_LINK_NOLINK,
                             RB_AGDC_STREAM_NONE,RB_AGDC_STREAM_BAD) == 0);
#undef A
    return bad;
}
