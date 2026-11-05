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

    /* THE PRECONDITION FOR READING THE MASK AT ALL (2CA (3)): Linux selects
     * each array with GRBM_GFX_INDEX before reading, and we refuse rather
     * than write that register on a live accelerator. So a single read
     * stands for the chip only in broadcast - all three bits. */
    if (!rb_gpu_harvest_read_valid(0xE0000000u)) {
        printf("FAIL gpuname: full broadcast must be accepted\n");
        bad++;
    }
    if (rb_gpu_harvest_read_valid(0x00000000u) ||
        rb_gpu_harvest_read_valid(0xC0000000u) ||
        rb_gpu_harvest_read_valid(0xA0000000u) ||
        rb_gpu_harvest_read_valid(0x60000000u)) {
        printf("FAIL gpuname: a GRBM_GFX_INDEX missing ANY broadcast bit "
               "means the read answers for one array, not the chip\n");
        bad++;
    }
    /* The low bits are indices and must not affect the verdict when all
     * three broadcast bits are set - a control, so the check cannot be
     * passing by comparing the whole word. */
    if (!rb_gpu_harvest_read_valid(0xE0010203u)) {
        printf("FAIL gpuname: the index fields must not change the "
               "broadcast verdict\n");
        bad++;
    }
    /* THE POINT OF THE WHOLE HEADER: an unestablished CU count must NOT be
     * rounded to a known row. 2 CU is the 610M's rumoured count and the
     * sources disagree on whether it is even Rembrandt, so it has no row -
     * and naming it "660M" would be exactly the substitution rule 17
     * forbids. */
    if (rb_gpu_name_known(2u) != (const char *)0 ||
        rb_gpu_name_known(4u) != (const char *)0 ||
        rb_gpu_name_known(8u) != (const char *)0) {
        printf("FAIL gpuname: an unestablished CU count must have NO known "
               "name - a guessed row is worse than the hardcode it "
               "replaces\n");
        bad++;
    }
    if (rb_gpu_name(2u, buf, sizeof(buf)) != 0 ||
        strcmp(buf, "AMD Radeon Graphics (Rembrandt, 2 CU)") != 0) {
        printf("FAIL gpuname: an unknown part must name its own CU count, "
               "got '%s'\n", buf);
        bad++;
    }
    if (rb_gpu_name(10u, buf, sizeof(buf)) != 0 ||
        strcmp(buf, "AMD Radeon Graphics (Rembrandt, 10 CU)") != 0) {
        printf("FAIL gpuname: two-digit CU counts must render, got '%s'\n", buf);
        bad++;
    }
    /* REFUSALS. Zero CU is not a chip, it is a geometry we could not read,
     * and the caller must be able to tell the difference (rule 14). */
    if (rb_gpu_cu_count(0, 2, 3, 0) != 0u ||
        rb_gpu_cu_count(1, 0, 3, 0) != 0u ||
        rb_gpu_cu_count(1, 2, 0, 0) != 0u) {
        printf("FAIL gpuname: a zero in the geometry must REFUSE, not "
               "multiply out to zero silently\n");
        bad++;
    }
    if (rb_gpu_cu_count(99, 2, 3, 0) != 0u ||
        rb_gpu_cu_count(1, 99, 3, 0) != 0u ||
        rb_gpu_cu_count(1, 2, 99, 0) != 0u) {
        printf("FAIL gpuname: an implausible geometry must refuse rather "
               "than produce a confident CU count\n");
        bad++;
    }
    if (rb_gpu_cu_count(1, 2, 3, 0xFFFFFFFFu) != 0u ||
        rb_gpu_cu_count(1, 2, 3, 0x7u) != 0u) {
        printf("FAIL gpuname: an array with every WGP harvested must refuse "
               "- 0x7 on a 3-WGP array is an empty array, which 2C9 read as "
               "the 660M\n");
        bad++;
    }
    if (rb_gpu_name(0u, buf, sizeof(buf)) != -1 || buf[0] != '\0') {
        printf("FAIL gpuname: 0 CU must return -1 AND leave the buffer "
               "empty, so a caller that ignores the code publishes nothing "
               "rather than a stale name\n");
        bad++;
    }
    /* A short buffer must refuse outright rather than truncate a name into
     * a different name. */
    {
        char small[8];
        if (rb_gpu_name(12u, small, sizeof(small)) != -1) {
            printf("FAIL gpuname: a buffer too small must refuse, not "
                   "truncate\n");
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
           "harvest mask), the mask is per-array and needs broadcast, an "
           "unestablished count names itself, every refusal path refuses, "
           "and the carve-out comes from the register%s\n",
           bad ? " FAILED" : ": passed");
    return bad;
}
