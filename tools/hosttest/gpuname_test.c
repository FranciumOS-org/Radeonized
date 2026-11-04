/* SPDX-License-Identifier: MIT */
/* 2C9: the derived GPU name and carve-out size. Pure logic, so the hosttest
 * decides what the kext will publish - and in particular decides that an
 * UNKNOWN part is named as unknown rather than rounded to a known row. */
#include <stdio.h>
#include <string.h>
#include "rb_gpuname.h"

int gpuname_tests(void);

int gpuname_tests(void)
{
    int bad = 0;
    char buf[RB_GPUNAME_MAX];

    /* THE REAL CHIP. The discovery table captured from this machine
     * (tests/data/rembrandt-680m-*.bin, asserted in hosttest.c) gives
     * NUM_SE 1, NUM_SA_PER_SE 2, NUM_WGP0_PER_SA 3, and nothing harvested. */
    if (rb_gpu_cu_count(1, 2, 3, 0) != 12u) {
        printf("FAIL gpuname: 1 SE x 2 SA x 3 WGP with no harvest is 12 CU, "
               "got %u\n", rb_gpu_cu_count(1, 2, 3, 0));
        bad++;
    }
    if (rb_gpu_name(12u, buf, sizeof(buf)) != 1 ||
        strcmp(buf, "AMD Radeon 680M") != 0) {
        printf("FAIL gpuname: 12 CU must be the 680M, got '%s'\n", buf);
        bad++;
    }
    /* The 660M is half the CUs, and it is the ONLY other established row. */
    if (rb_gpu_cu_count(1, 2, 3, 0x7) != 6u) {
        printf("FAIL gpuname: three WGPs harvested off six leaves 6 CU, "
               "got %u\n", rb_gpu_cu_count(1, 2, 3, 0x7));
        bad++;
    }
    if (rb_gpu_name(6u, buf, sizeof(buf)) != 1 ||
        strcmp(buf, "AMD Radeon 660M") != 0) {
        printf("FAIL gpuname: 6 CU must be the 660M, got '%s'\n", buf);
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
    if (rb_gpu_cu_count(1, 2, 3, 0xFFFFFFFFu) != 0u) {
        printf("FAIL gpuname: every WGP harvested must refuse\n");
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

    printf("gpuname: 12 CU -> 680M, 6 CU -> 660M, an unestablished count "
           "names itself, every refusal path refuses, and the carve-out "
           "comes from the register%s\n", bad ? " FAILED" : ": passed");
    return bad;
}
