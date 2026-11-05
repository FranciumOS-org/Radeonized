/* SPDX-License-Identifier: MIT */
/* 2C9: the derived GPU name and carve-out size. Pure logic, so the hosttest
 * decides what the kext will publish - and in particular decides that an
 * UNKNOWN part is named as unknown rather than rounded to a known row. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "rb_gpuname.h"
#include "rb_regs.h"

int gpuname_tests(void);

int gpuname_tests(void)
{
    int bad = 0;
    char buf[RB_GPUNAME_MAX];

    /* THE REAL CHIP. The discovery table captured from this machine
     * (tests/data/rembrandt-680m-*.bin, asserted in hosttest.c) gives
     * NUM_SE 1, NUM_SA_PER_SE 2, NUM_WGP0_PER_SA 3, and nothing harvested -
     * which is what the RB_REMBRANDT_* die constants carry, so the constants
     * the kext uses are pinned against the captured data here rather than
     * written down twice. */
    if (RB_REMBRANDT_NUM_SE != 1u || RB_REMBRANDT_NUM_SA_PER_SE != 2u ||
        RB_REMBRANDT_NUM_WGP_PER_SA != 3u) {
        printf("FAIL gpuname: the Rembrandt die geometry must stay 1 x 2 x 3, "
               "which is what the captured discovery table reads\n");
        bad++;
    }
    if (rb_gpu_cu_count(RB_REMBRANDT_NUM_SE, RB_REMBRANDT_NUM_SA_PER_SE,
                        RB_REMBRANDT_NUM_WGP_PER_SA, 0) != 12u) {
        printf("FAIL gpuname: 1 SE x 2 SA x 3 WGP with no harvest is 12 CU, "
               "got %u\n", rb_gpu_cu_count(1, 2, 3, 0));
        bad++;
    }
    if (rb_gpu_name(12u, buf, sizeof(buf)) != 1 ||
        strcmp(buf, "AMD Radeon 680M") != 0) {
        printf("FAIL gpuname: 12 CU must be the 680M, got '%s'\n", buf);
        bad++;
    }

    /* 2CA: THE MASK IS PER SHADER ARRAY, so one dead WGP costs TWO on this
     * geometry - one in each array. 2C9 popcounted the mask against the
     * chip's total and would have called this 10 CU. */
    if (rb_gpu_cu_count(1, 2, 3, 0x1) != 8u) {
        printf("FAIL gpuname: a per-array mask of one WGP kills one in EACH "
               "array, leaving 8 CU, got %u\n", rb_gpu_cu_count(1, 2, 3, 0x1));
        bad++;
    }
    if (rb_gpu_cu_count(1, 2, 3, 0x3) != 4u) {
        printf("FAIL gpuname: two WGPs per array leaves 4 CU, got %u\n",
               rb_gpu_cu_count(1, 2, 3, 0x3));
        bad++;
    }
    /* AND THE CONSEQUENCE, PINNED: a symmetric per-array harvest can only
     * kill a multiple of (num_se x num_sa_per_se) WGPs, so on 1 x 2 x 3 the
     * reachable counts are 12, 8, 4 and a refusal. 6 CU - the 660M - is
     * UNREACHABLE from a harvest mask, and this assertion is what stops a
     * later change quietly making it reachable by the wrong arithmetic. */
    {
        uint32_t m, hit6 = 0;
        for (m = 0; m < 8u; m++) {
            if (rb_gpu_cu_count(1, 2, 3, m) == 6u) { hit6 = 1; }
        }
        if (hit6) {
            printf("FAIL gpuname: 6 CU must NOT be reachable from a "
                   "symmetric per-array harvest on a 1 x 2 x 3 die - if it "
                   "is, the arithmetic is per-chip again (2CA)\n");
            bad++;
        }
    }
    /* The 660M's row is reached by GEOMETRY instead: one array (or engine)
     * disabled, which the discovery table reports and no harvest register
     * does. It is still the only other established row. */
    if (rb_gpu_cu_count(1, 1, 3, 0) != 6u) {
        printf("FAIL gpuname: 1 SE x 1 SA x 3 WGP is 6 CU, got %u\n",
               rb_gpu_cu_count(1, 1, 3, 0));
        bad++;
    }
    if (rb_gpu_name(6u, buf, sizeof(buf)) != 1 ||
        strcmp(buf, "AMD Radeon 660M") != 0) {
        printf("FAIL gpuname: 6 CU must be the 660M, got '%s'\n", buf);
        bad++;
    }

    /* THE EFUSE AND THE VBIOS MASKS BOTH COUNT, ORed as gfx_v10_0.c ORs
     * them - a part harvested only in firmware must not read as unharvested
     * (2CA (2)). And the field extraction must take bits 31:16. */
    if (rb_gpu_inactive_wgps(0x00030000u) != 0x3u ||
        rb_gpu_inactive_wgps(0x0000FFFFu) != 0u) {
        printf("FAIL gpuname: INACTIVE_WGPS is bits 31:16 and nothing "
               "below them\n");
        bad++;
    }
    if (rb_gpu_harvest_per_sa(0x1u, 0x2u, 3u) != 0x3u) {
        printf("FAIL gpuname: the efuse and VBIOS masks must OR\n");
        bad++;
    }
    /* Bits above the array's own width are not WGPs. The register's field is
     * 16 bits wide on a 3-WGP array, so this is the common case, not an
     * edge one - it is what made 2C9's popcount wrong. */
    if (rb_gpu_harvest_per_sa(0xFFF8u, 0u, 3u) != 0u) {
        printf("FAIL gpuname: bits above the array's WGP count must NOT "
               "read as harvested WGPs\n");
        bad++;
    }
    if (rb_gpu_harvest_per_sa(0x1u, 0u, 0u) != 0u ||
        rb_gpu_harvest_per_sa(0x1u, 0u, 32u) != 0u) {
        printf("FAIL gpuname: an unusable array width must refuse\n");
        bad++;
    }

    /* 2CC: WHICH ARRAY THE HARVEST READ ANSWERED FOR. 2CA required FULL
     * BROADCAST and boot 416 refused on exactly that - the register reads
     * **0**. Two things make 0 the right answer and the old check the wrong
     * question: the bits are named `*_BROADCAST_WRITES` and govern WRITES,
     * and `gfx_v10_0_select_se_sh(se=0, sh=0, instance=0)` writes exactly 0
     * to select (SE0, SA0). So the value that was refused is the value Linux
     * writes to ask this very question. */
    if (!rb_gpu_harvest_sel_valid(0x00000000u, 1, 2)) {
        printf("FAIL gpuname: GRBM_GFX_INDEX 0 is Linux's own "
               "select_se_sh(0,0,0) word and MUST be accepted - boot 416 "
               "refused the whole feature on this\n");
        bad++;
    }
    /* SE0/SA1 is a real array on this die and must be accepted too, with the
     * fields decoded rather than the word compared. */
    if (!rb_gpu_harvest_sel_valid(0x00000100u, 1, 2) ||
        rb_grbm_sa(0x00000100u) != 1u || rb_grbm_se(0x00000100u) != 0u ||
        rb_grbm_instance(0x00000100u) != 0u) {
        printf("FAIL gpuname: SA_INDEX is bits 15:8 and SE0/SA1 is a valid "
               "array on a 1 x 2 die\n");
        bad++;
    }
    if (rb_grbm_se(0x00020300u) != 2u || rb_grbm_sa(0x00020300u) != 3u) {
        printf("FAIL gpuname: SE_INDEX is bits 23:16 and SA_INDEX bits "
               "15:8\n");
        bad++;
    }
    /* REFUSALS. A broadcast read is refused because what it RETURNS is not
     * established by any header in this tree - "not established", not
     * "wrong" - and each of the three bits must refuse on its own. */
    if (rb_gpu_harvest_sel_valid(0x20000000u, 1, 2) ||
        rb_gpu_harvest_sel_valid(0x40000000u, 1, 2) ||
        rb_gpu_harvest_sel_valid(0x80000000u, 1, 2) ||
        rb_gpu_harvest_sel_valid(0xE0000000u, 1, 2)) {
        printf("FAIL gpuname: any *_BROADCAST_WRITES bit must refuse - a "
               "broadcast READ of a per-array register has no established "
               "answer\n");
        bad++;
    }
    /* An array outside the geometry we are about to assume. */
    if (rb_gpu_harvest_sel_valid(0x00010000u, 1, 2) ||
        rb_gpu_harvest_sel_valid(0x00000200u, 1, 2)) {
        printf("FAIL gpuname: a selection outside the die's own geometry "
               "must refuse - SE1 does not exist on a 1-SE die, nor SA2 on "
               "a 2-SA engine\n");
        bad++;
    }
    /* A non-zero instance index is not the shader array we mean. */
    if (rb_gpu_harvest_sel_valid(0x00000001u, 1, 2)) {
        printf("FAIL gpuname: a non-zero INSTANCE_INDEX must refuse\n");
        bad++;
    }
    if (rb_gpu_harvest_sel_valid(0u, 0, 2) ||
        rb_gpu_harvest_sel_valid(0u, 1, 0)) {
        printf("FAIL gpuname: a zero geometry must refuse\n");
        bad++;
    }

    /* BOOT 416's OWN REGISTER WORDS, end to end. This is the assertion that
     * would have caught 2CA before it cost a boot: the live
     * CC_GC_SHADER_ARRAY_CONFIG is **0xFFF80001**, so INACTIVE_WGPS is
     * 0xFFF8 - every bit ABOVE the array's three WGPs set, which is Linux's
     * `disabled_mask` initial value showing through. Narrowed to the array
     * it is 0: nothing harvested, 12 CU, the 680M. 2C9's popcount of the raw
     * field would have counted 13 dead WGPs out of 6 and refused. */
    {
        char nm[RB_GPUNAME_MAX];
        const uint32_t cc_live   = 0xFFF80001u;   /* measured, boot 416 */
        const uint32_t user_live = 0x00000000u;   /* measured, boot 416 */
        const uint32_t idx_live  = 0x00000000u;   /* measured, boot 416 */
        uint32_t m, cu;

        if (rb_gpu_inactive_wgps(cc_live) != 0xFFF8u) {
            printf("FAIL gpuname: boot 416's CC word has INACTIVE_WGPS "
                   "0xFFF8, got 0x%x\n", rb_gpu_inactive_wgps(cc_live));
            bad++;
        }
        if (!rb_gpu_harvest_sel_valid(idx_live, RB_REMBRANDT_NUM_SE,
                                      RB_REMBRANDT_NUM_SA_PER_SE)) {
            printf("FAIL gpuname: boot 416's GRBM_GFX_INDEX must be "
                   "usable\n");
            bad++;
        }
        m = rb_gpu_harvest_per_sa(rb_gpu_inactive_wgps(cc_live),
                                  rb_gpu_inactive_wgps(user_live),
                                  RB_REMBRANDT_NUM_WGP_PER_SA);
        if (m != 0u) {
            printf("FAIL gpuname: narrowed to three WGPs, boot 416's "
                   "harvest is 0 - got 0x%x\n", m);
            bad++;
        }
        cu = rb_gpu_cu_count(RB_REMBRANDT_NUM_SE, RB_REMBRANDT_NUM_SA_PER_SE,
                             RB_REMBRANDT_NUM_WGP_PER_SA, m);
        if (cu != 12u || rb_gpu_name(cu, nm, sizeof(nm)) != 1 ||
            strcmp(nm, "AMD Radeon 680M") != 0) {
            printf("FAIL gpuname: boot 416's own registers must derive "
                   "12 CU / 'AMD Radeon 680M', got %u / '%s'\n", cu, nm);
            bad++;
        }
        /* And the carve-out half, which DID succeed on that boot - it is
         * decided independently of the name for exactly this reason. */
        if (rb_gpu_vram_mb(4096u) != 4096u) {
            printf("FAIL gpuname: boot 416 read memsize 4096 and the "
                   "carve-out must survive a refused NAME\n");
            bad++;
        }
    }

    /* 2CB: THE ATY NAME SPLIT. The whole value is that Apple's reassembly
     * returns our model BYTE-IDENTICALLY - otherwise SPDisplaysReporter's
     * first test (a caseInsensitiveCompare against sppci_model) still fails
     * and we have changed the Metal name without gaining "Metal: 3". So the
     * assertion is the ROUND TRIP, not the halves. */
    {
        char fam[RB_GPUNAME_MAX], dev[RB_GPUNAME_MAX], back[RB_GPUNAME_MAX * 2];
        static const char *const models[] = {
            "AMD Radeon 680M",
            "AMD Radeon 660M",
            "AMD Radeon Graphics (Rembrandt, 12 CU)",
            "AMD Radeon Graphics (Rembrandt, 8 CU)",
        };
        unsigned m;
        for (m = 0; m < sizeof(models) / sizeof(models[0]); m++) {
            if (!rb_gpu_aty_split(models[m], fam, sizeof(fam),
                                  dev, sizeof(dev))) {
                printf("FAIL gpuname: '%s' must split\n", models[m]);
                bad++;
                continue;
            }
            if (!rb_gpu_aty_join(fam, dev, back, sizeof(back)) ||
                strcmp(back, models[m]) != 0) {
                printf("FAIL gpuname: '%s' -> '%s' + '%s' -> '%s', which is "
                       "NOT the model back\n", models[m], fam, dev, back);
                bad++;
            }
        }
        /* And the halves themselves for the one that matters, so a reader
         * can see what lands in the registry. */
        if (!rb_gpu_aty_split("AMD Radeon 680M", fam, sizeof(fam),
                              dev, sizeof(dev)) ||
            strcmp(fam, "Radeon") != 0 || strcmp(dev, "680M") != 0) {
            printf("FAIL gpuname: ATY,FamilyName must be 'Radeon' and "
                   "ATY,DeviceName '680M', got '%s' / '%s'\n", fam, dev);
            bad++;
        }
        /* AND A MODEL WITH SEVERAL SPACES, which is what actually pins the
         * CUT POSITION. "AMD Radeon 680M" has one space, so cutting at the
         * first or the last gives the same answer there - and the round trip
         * is invariant to the cut altogether, since join is family + " " +
         * device whichever space was chosen. A break-it check that moved the
         * cut to the last space was caught only by an unrelated buffer
         * assertion, i.e. it passed for the wrong reason (2BC). */
        if (!rb_gpu_aty_split("AMD Radeon Graphics (Rembrandt, 12 CU)",
                              fam, sizeof(fam), dev, sizeof(dev)) ||
            strcmp(fam, "Radeon") != 0 ||
            strcmp(dev, "Graphics (Rembrandt, 12 CU)") != 0) {
            printf("FAIL gpuname: the cut is the FIRST space - family must "
                   "be 'Radeon' and device 'Graphics (Rembrandt, 12 CU)', "
                   "got '%s' / '%s'\n", fam, dev);
            bad++;
        }
        /* REFUSALS. Apple prepends "AMD " unconditionally, so a model
         * without it cannot round-trip and must not be published at all -
         * publishing one half is a name that does not match (rule 14). */
        {
            static const char *const bogus[] = {
                "Radeon 680M",          /* no "AMD " prefix */
                "AMD",                  /* prefix absent, too short */
                "AMD ",                 /* nothing after the prefix */
                "AMD Radeon",           /* no space to split on */
                "AMD  680M",            /* empty family */
                "AMD Radeon ",          /* empty device */
                (const char *)0,
            };
            unsigned b;
            for (b = 0; b < sizeof(bogus) / sizeof(bogus[0]); b++) {
                if (rb_gpu_aty_split(bogus[b], fam, sizeof(fam),
                                     dev, sizeof(dev)) != 0) {
                    printf("FAIL gpuname: '%s' must REFUSE to split\n",
                           bogus[b] ? bogus[b] : "(null)");
                    bad++;
                }
                if (fam[0] != '\0' || dev[0] != '\0') {
                    printf("FAIL gpuname: a refused split must leave BOTH "
                           "buffers empty, so a caller that ignores the "
                           "code publishes nothing\n");
                    bad++;
                }
            }
        }
        /* A device half that does not fit must refuse and empty BOTH, not
         * leave a correct family beside a truncated device. */
        {
            char tiny[6];
            if (rb_gpu_aty_split("AMD Radeon Graphics (Rembrandt, 12 CU)",
                                 fam, sizeof(fam), tiny, sizeof(tiny)) != 0 ||
                fam[0] != '\0' || tiny[0] != '\0') {
                printf("FAIL gpuname: a device half that does not fit must "
                       "refuse and empty both halves\n");
                bad++;
            }
        }
        /* The join must refuse rather than truncate, for the same reason. */
        {
            char small[8];
            if (rb_gpu_aty_join("Radeon", "680M", small, sizeof(small)) != 0) {
                printf("FAIL gpuname: the join must refuse a short buffer\n");
                bad++;
            }
        }
        /* THE REGRESSION THIS GUARDS: Apple's own fallbacks. "Radeon HD" +
         * "GFX10 Family Unknown Prototype" is what the driver produces with
         * neither property set, and it is what this machine reads today -
         * so the join must reproduce the string we actually measured. */
        if (!rb_gpu_aty_join("Radeon HD", "GFX10 Family Unknown Prototype",
                             back, sizeof(back)) ||
            strcmp(back, "AMD Radeon HD GFX10 Family Unknown Prototype") != 0) {
            printf("FAIL gpuname: the join must reproduce the MEASURED "
                   "fallback name, got '%s'\n", back);
            bad++;
        }
    }

    /* The carve-out register offset is ALIASED, not retyped: rb_probe.c and
     * the plugin must read the same dword or one of them is measuring
     * something else (2C4's lesson about bounds in three places). */
    if (RB_GPU_DW_RCC_CONFIG_MEMSIZE != RB_mmRCC_CONFIG_MEMSIZE) {
        printf("FAIL gpuname: the memsize register alias has drifted from "
               "rb_regs.h - 0x%x vs 0x%x\n",
               RB_GPU_DW_RCC_CONFIG_MEMSIZE, RB_mmRCC_CONFIG_MEMSIZE);
        bad++;
    }

    /* THE CARVE-OUT. 4096 is what this machine really has - Metal's
     * recommendedMaxWorkingSetSize agrees - but it must come from the
     * register, and the register's two bad values must refuse. */
    if (rb_gpu_vram_mb(4096u) != 4096u) {
        printf("FAIL gpuname: 4096 MB is this machine's measured carve-out "
               "and must pass through\n");
        bad++;
    }
    if (rb_gpu_vram_mb(0xFFFFFFFFu) != 0u) {
        printf("FAIL gpuname: an unmapped read (all ones) must refuse - "
               "rb_probe.c already treats it that way\n");
        bad++;
    }
    if (rb_gpu_vram_mb(0u) != 0u) {
        printf("FAIL gpuname: zero MB must refuse, not publish 0 GB\n");
        bad++;
    }
    if (rb_gpu_vram_mb(RB_VRAM_MB_MIN - 1u) != 0u ||
        rb_gpu_vram_mb(RB_VRAM_MB_MAX + 1u) != 0u) {
        printf("FAIL gpuname: out-of-range sizes must refuse\n");
        bad++;
    }
    if (rb_gpu_vram_mb(RB_VRAM_MB_MIN) != RB_VRAM_MB_MIN ||
        rb_gpu_vram_mb(RB_VRAM_MB_MAX) != RB_VRAM_MB_MAX) {
        printf("FAIL gpuname: both bounds must be reachable\n");
        bad++;
    }

    printf("gpuname: 12 CU -> 680M, 6 CU -> 660M by geometry (never by a "
           "harvest mask), the mask is per-array and names the array it came "
           "from, an "
           "unestablished count names itself, every refusal path refuses, "
           "and the carve-out comes from the register%s\n",
           bad ? " FAILED" : ": passed");
    return bad;
}
