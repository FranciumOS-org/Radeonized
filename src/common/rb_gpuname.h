/* SPDX-License-Identifier: MIT */
#ifndef RB_GPUNAME_H
#define RB_GPUNAME_H

#include <stdint.h>

/* 2C9: THE GPU'S OWN NAME AND MEMORY SIZE, DERIVED FROM THE CHIP.
 *
 * WHAT WAS THERE BEFORE, AND WHY IT IS NOT "a patch in OpenCore".
 * `RBAMDFramebuffer::start()` published two LITERALS:
 *
 *     setProperty("VRAM,totalMB", 4096ULL, 32);
 *     setProperty("model", "AMD Radeon 680M");
 *
 * and `kern_start.cpp`'s `rbgpu_vram_publish()` has a third
 * (`static const char kModel[]`) which is dead on this machine - it refuses
 * any object that is not `IONDRVFramebuffer`, and 2sl/2sm/2st displaced that
 * class with ours. The user believed an OpenCore patch was responsible; the
 * EFI backup says otherwise - `DeviceProperties/Add` carries only
 * `acpi-wake-type` and `built-in`, and there is no Radeon kernel patch. So
 * the strings are ours, which is what makes them fixable.
 *
 * WHAT IS ALREADY HONEST: the 4096 MB. Metal's own
 * `recommendedMaxWorkingSetSize` reads **4096 MB** on this machine, and the
 * 2pa comment recorded `RCC_CONFIG_MEMSIZE` and `MMMC_VM_FB_LOCATION_TOP`
 * agreeing. The number is real - it is the *hardcoding* that is wrong, since
 * a different Rembrandt board with a different carve-out would still read
 * 4096. `rb_probe.c` already reads `RB_mmRCC_CONFIG_MEMSIZE` (0xde3) into
 * `memsize_mb`, so the live value exists in shared code.
 *
 * WHAT MACOS CANNOT DO, STATED PLAINLY. The ask was "macOS finds it and
 * shows it itself". It cannot: the marketing name lives in a device-id ->
 * string table inside Apple's own driver, and 1002:1681 is not in it - which
 * is exactly why Metal reports **"AMD Radeon HD GFX10 Family Unknown
 * Prototype"**. There is no code path in macOS that knows what a 680M is. So
 * the choice is not "our string vs Apple's string"; it is "a string we
 * DERIVE from the silicon vs a string we hardcode". This header is the
 * former.
 *
 * THE DERIVATION. A Rembrandt SKU is distinguished by its enabled CU count,
 * and the chip reports its own geometry two ways we already parse:
 *   - the IP discovery GC info table (`rb_discovery.h`: RB_GC_NUM_SE,
 *     RB_GC_NUM_SA_PER_SE, RB_GC_NUM_WGP0_PER_SA), hosttested against real
 *     captured 680M data at `tests/data/rembrandt-680m-*.bin`, which gives
 *     1 x 2 x 3 = 6 WGP = 12 CU; and
 *   - `CC_GC_SHADER_ARRAY_CONFIG` (0x100f, BASE_IDX 0) bits 31:16
 *     `INACTIVE_WGPS`, the harvest mask, from `gc_10_3_0_offset.h` and
 *     `gc_10_3_0_sh_mask.h` (both vendored).
 * One WGP is two CUs on RDNA, so CU = 2 x (total WGP - harvested WGP).
 *
 * THE TABLE HAS TWO ROWS AND NO MORE, ON PURPOSE (rule 17's mirror - every
 * constant read, not recalled):
 *   12 CU -> Radeon 680M   (768 shaders; techpowerup/notebookcheck/cpu-monkey)
 *    6 CU -> Radeon 660M   (384 shaders, half the 680M's CUs; same sources)
 * A 640M was NOT confirmed by any source consulted, and the 610M's sources
 * DISAGREE about whether it is even Rembrandt (one lists Mendocino / Navi
 * III), so neither gets a row. Guessing a row would publish a wrong name,
 * which is strictly worse than the hardcode it replaces.
 *
 * Anything else gets a DERIVED generic name carrying the measured CU count,
 * so an unknown part announces itself instead of being rounded to the
 * nearest known one (rule 14: a refusal path must publish what it refused).
 */

#define RB_GPUNAME_MAX 48

/* One WGP is two compute units on RDNA/RDNA2. */
#define RB_CU_PER_WGP 2u

static inline uint32_t rb_popcount32(uint32_t v)
{
    uint32_t n = 0;
    while (v) { v &= (v - 1u); n++; }
    return n;
}

/* The chip's enabled CU count from its own geometry plus its harvest mask.
 * `inactive_wgps` is CC_GC_SHADER_ARRAY_CONFIG bits 31:16 ALREADY SHIFTED
 * DOWN by the caller - the register read and the field extraction belong to
 * whoever owns the aperture, not here, so this stays pure.
 *
 * Returns 0 when the geometry is not usable, which is a REFUSAL and not a
 * zero-CU chip: a caller must treat 0 as "do not publish a derived name"
 * rather than as a measurement (rule 14). */
static inline uint32_t rb_gpu_cu_count(uint32_t num_se, uint32_t num_sa_per_se,
                                       uint32_t num_wgp_per_sa,
                                       uint32_t inactive_wgps)
{
    if (num_se == 0u || num_sa_per_se == 0u || num_wgp_per_sa == 0u) {
        return 0u;
    }
    /* A plausibility bound, so a garbage table cannot produce a confident
     * name. Navi 2x tops out well inside these. */
    if (num_se > 8u || num_sa_per_se > 4u || num_wgp_per_sa > 16u) {
        return 0u;
    }
    const uint32_t total_wgp = num_se * num_sa_per_se * num_wgp_per_sa;
    const uint32_t dead = rb_popcount32(inactive_wgps);
    if (dead >= total_wgp) {
        return 0u;   /* every WGP harvested: not a chip we are looking at */
    }
    return (total_wgp - dead) * RB_CU_PER_WGP;
}

/* The marketing name for a CU count, or NULL when this project has not
 * established one. NULL is the signal to fall back to the derived generic
 * string - callers must not substitute the nearest row. */
static inline const char *rb_gpu_name_known(uint32_t cu)
{
    switch (cu) {
    case 12u: return "AMD Radeon 680M";
    case 6u:  return "AMD Radeon 660M";
    default:  return (const char *)0;
    }
}

/* Write the name for `cu` into `out`. Returns 1 if the name is a KNOWN
 * marketing name, 0 if it is the derived generic form, and -1 if nothing
 * could be said at all (cu == 0, i.e. the geometry was refused) - in which
 * case `out` is left as an empty string and the caller should publish
 * nothing rather than publish a guess.
 *
 * Deliberately no snprintf: this header is included by kext code where the
 * freestanding subset is all that is available, and the formatting is two
 * decimal digits at most. */
static inline int rb_gpu_name(uint32_t cu, char *out, uint32_t outlen)
{
    static const char kPre[] = "AMD Radeon Graphics (Rembrandt, ";
    uint32_t i = 0, j;

    if (!out || outlen < RB_GPUNAME_MAX) {
        return -1;
    }
    out[0] = '\0';
    if (cu == 0u) {
        return -1;
    }
    {
        const char *known = rb_gpu_name_known(cu);
        if (known) {
            for (i = 0; known[i] && i + 1u < outlen; i++) { out[i] = known[i]; }
            out[i] = '\0';
            return 1;
        }
    }
    for (j = 0; kPre[j] && i + 1u < outlen; j++) { out[i++] = kPre[j]; }
    if (cu >= 100u && i + 1u < outlen) { out[i++] = (char)('0' + (cu / 100u) % 10u); }
    if (cu >= 10u  && i + 1u < outlen) { out[i++] = (char)('0' + (cu / 10u) % 10u); }
    if (i + 1u < outlen) { out[i++] = (char)('0' + cu % 10u); }
    {
        static const char kPost[] = " CU)";
        for (j = 0; kPost[j] && i + 1u < outlen; j++) { out[i++] = kPost[j]; }
    }
    out[i] = '\0';
    return 0;
}

/* The carve-out size in MB, as the chip reports it, with the same refusal
 * discipline `rb_probe.c` already applies to this register: 0xFFFFFFFF is an
 * unmapped read and 0 is a chip that has not been told its own size. Returns
 * 0 to mean "do not publish", never a substituted default - 2C9 exists
 * because a plausible default was published as a measurement for 30-odd
 * milestones. */
#define RB_VRAM_MB_MIN   256u
#define RB_VRAM_MB_MAX 16384u

static inline uint32_t rb_gpu_vram_mb(uint32_t memsize_reg)
{
    if (memsize_reg == 0xFFFFFFFFu) { return 0u; }
    if (memsize_reg < RB_VRAM_MB_MIN || memsize_reg > RB_VRAM_MB_MAX) { return 0u; }
    return memsize_reg;
}

#endif /* RB_GPUNAME_H */
