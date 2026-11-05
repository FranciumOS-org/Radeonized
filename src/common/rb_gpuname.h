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
 *   - `CC_GC_SHADER_ARRAY_CONFIG` (0x100f, BASE_IDX 0) and
 *     `GC_USER_SHADER_ARRAY_CONFIG` (0x1010, BASE_IDX 0) bits 31:16
 *     `INACTIVE_WGPS` - the efuse and VBIOS harvest masks, from
 *     `gc_10_3_0_offset.h` and `gc_10_3_0_sh_mask.h` (both vendored).
 * One WGP is two CUs on RDNA. 2CA below corrects how the mask combines with
 * the geometry: it is PER SHADER ARRAY, not per chip.
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

/* Dword offsets and field shifts, all from the vendored headers named above -
 * `mmCC_GC_SHADER_ARRAY_CONFIG` 0x100f BASE_IDX 0,
 * `mmGC_USER_SHADER_ARRAY_CONFIG` 0x1010 BASE_IDX 0,
 * `mmGRBM_GFX_INDEX` 0x2200 BASE_IDX **1** (a different segment: 2C1's
 * lesson about index conventions, asked of a base index),
 * and `INACTIVE_WGPS__SHIFT` 0x10 with mask 0xFFFF0000 on both config
 * registers. The segment bases are this chip's measured GC seg0 0x1260 and
 * seg1 0xa000, which belong to the caller that owns the aperture. */
#define RB_GC_DW_CC_SHADER_ARRAY_CONFIG    0x100fu
#define RB_GC_DW_USER_SHADER_ARRAY_CONFIG  0x1010u
#define RB_GC_DW_GRBM_GFX_INDEX            0x2200u
/* The carve-out size register. A fixed dword offset into the MMIO BAR, not a
 * segment-relative one - `rb_probe.c` already reads it. Aliased here rather
 * than retyped so the two cannot drift, and the hosttest asserts the alias
 * against `rb_regs.h`'s definition where both headers are visible (2C4's
 * pattern: bounds that are used in three places must be ONE definition). */
#define RB_GPU_DW_RCC_CONFIG_MEMSIZE       0xde3u
#define RB_GC_INACTIVE_WGPS_SHIFT          16u
#define RB_GC_INACTIVE_WGPS_MASK           0xFFFF0000u

static inline uint32_t rb_gpu_inactive_wgps(uint32_t shader_array_config)
{
    return (shader_array_config & RB_GC_INACTIVE_WGPS_MASK) >>
           RB_GC_INACTIVE_WGPS_SHIFT;
}

/* The Rembrandt die's shader geometry. The plugin has no discovery table of
 * its own (`g_disc` is a hand-built two-IP structure for MP1), and the
 * geometry is a property of the DIE rather than the SKU - every 680M/660M
 * part is this same die, harvested - so these are the die's numbers,
 * hosttested against the discovery data captured from this machine. Only the
 * harvest is read live, which is the part that actually distinguishes SKUs.
 * A caller on a chip that is not Rembrandt must not use them. */
#define RB_REMBRANDT_NUM_SE          1u
#define RB_REMBRANDT_NUM_SA_PER_SE   2u
#define RB_REMBRANDT_NUM_WGP_PER_SA  3u

/* One WGP is two compute units on RDNA/RDNA2. */
#define RB_CU_PER_WGP 2u

static inline uint32_t rb_popcount32(uint32_t v)
{
    uint32_t n = 0;
    while (v) { v &= (v - 1u); n++; }
    return n;
}

/* 2CA: THE HARVEST MASK IS PER SHADER ARRAY, WHICH 2C9 GOT WRONG.
 *
 * `gfx_v10_0.c:gfx_v10_0_get_wgp_active_bitmap_per_sh()` - the consumer, read
 * rather than recalled - says three things 2C9's arithmetic did not:
 *
 *   (1) the mask is PER SHADER ARRAY and `max_cu_per_sh >> 1` bits wide, so
 *       on a 3-WGP array only bits 2:0 are WGPs and everything above is
 *       `disabled_mask`'s initial all-ones, i.e. not a harvest reading at
 *       all. 2C9 popcounted the whole 16-bit field against the chip's TOTAL
 *       WGP count, which conflates a per-array mask with a per-chip one.
 *   (2) the real harvest is `CC_GC_SHADER_ARRAY_CONFIG | GC_USER_SHADER_
 *       ARRAY_CONFIG` - an efuse mask ORed with a VBIOS mask. Reading only
 *       the efuse would miss a part harvested in firmware, which is a
 *       plausible way to build a lower SKU off the same die.
 *   (3) Linux reads it once per (SE, SA) behind a `GRBM_GFX_INDEX` write.
 *
 * (3) is the one that bounds what this project can derive. Writing
 * `GRBM_GFX_INDEX` on a chip whose accelerator is live means racing Apple's
 * driver for the register that selects which array every subsequent GC read
 * answers for, so we do not write it - we read it, and refuse unless it is
 * in broadcast. That leaves ONE mask for the whole chip, i.e. the assumption
 * that the harvest is symmetric across arrays.
 *
 * STATE THE CONSEQUENCE RATHER THAN HIDE IT: a symmetric harvest can only
 * kill a MULTIPLE of `num_se * num_sa_per_se` WGPs, so on this chip's 1 x 2
 * x 3 geometry the reachable CU counts are 12, 8, 4 and a refusal - and
 * **6 CU is unreachable**. The 660M's row is therefore reached by GEOMETRY
 * (1 SE x 1 SA x 3 WGP, an array or engine disabled, which the discovery
 * table reports and a harvest register does not) and never by a harvest
 * mask. If a 660M ever reads 8 or 4 CU here, that is the signature of an
 * ASYMMETRIC harvest this derivation cannot see, and the generic name will
 * carry the wrong-but-measured count instead of a confident wrong SKU.
 * Hosttested in both directions, because the whole value of this header is
 * that it does not round an unknown part to a known row.
 */

/* The harvest mask for ONE shader array: the efuse and VBIOS masks ORed as
 * Linux ORs them, then narrowed to the array's own WGP count. Bits above
 * that width are not WGPs and must not count as dead ones. */
static inline uint32_t rb_gpu_harvest_per_sa(uint32_t cc_inactive_wgps,
                                             uint32_t user_inactive_wgps,
                                             uint32_t num_wgp_per_sa)
{
    uint32_t width_mask;

    if (num_wgp_per_sa == 0u || num_wgp_per_sa >= 32u) {
        return 0u;
    }
    width_mask = (1u << num_wgp_per_sa) - 1u;
    return (cc_inactive_wgps | user_inactive_wgps) & width_mask;
}

/* The chip's enabled CU count from its own geometry plus its PER-ARRAY
 * harvest mask (`rb_gpu_harvest_per_sa`'s result - already ORed and already
 * narrowed). The mask is applied to EVERY array, which is the symmetric
 * assumption the comment above bounds.
 *
 * Returns 0 when the geometry or the mask is not usable, which is a REFUSAL
 * and not a zero-CU chip: a caller must treat 0 as "do not publish a derived
 * name" rather than as a measurement (rule 14). */
static inline uint32_t rb_gpu_cu_count(uint32_t num_se, uint32_t num_sa_per_se,
                                       uint32_t num_wgp_per_sa,
                                       uint32_t inactive_wgps_per_sa)
{
    uint32_t arrays, live_per_sa, dead_per_sa;

    if (num_se == 0u || num_sa_per_se == 0u || num_wgp_per_sa == 0u) {
        return 0u;
    }
    /* A plausibility bound, so a garbage table cannot produce a confident
     * name. Navi 2x tops out well inside these. */
    if (num_se > 8u || num_sa_per_se > 4u || num_wgp_per_sa > 16u) {
        return 0u;
    }
    /* Narrow again here rather than trust the caller: this function is the
     * one the hosttest pins and the one the kext calls, and a mask wider
     * than the array is the defect 2CA is correcting. */
    dead_per_sa = rb_gpu_harvest_per_sa(inactive_wgps_per_sa, 0u,
                                        num_wgp_per_sa);
    dead_per_sa = rb_popcount32(dead_per_sa);
    if (dead_per_sa >= num_wgp_per_sa) {
        return 0u;   /* an array with nothing live in it: not a reading we
                      * can turn into a name */
    }
    arrays = num_se * num_sa_per_se;
    live_per_sa = num_wgp_per_sa - dead_per_sa;
    return arrays * live_per_sa * RB_CU_PER_WGP;
}

/* GRBM_GFX_INDEX must be in broadcast for a single harvest read to stand for
 * the chip (2CA). Linux selects each (SE, SA) before reading; we refuse
 * instead of writing that register, because the accelerator is live and owns
 * it. `gc_10_3_0_sh_mask.h`: SA_BROADCAST_WRITES bit 29, INSTANCE_BROADCAST
 * bit 30, SE_BROADCAST_WRITES bit 31 - all three, read not recalled. */
#define RB_GRBM_GFX_INDEX_BCAST_MASK 0xE0000000u

static inline int rb_gpu_harvest_read_valid(uint32_t grbm_gfx_index)
{
    return (grbm_gfx_index & RB_GRBM_GFX_INDEX_BCAST_MASK) ==
           RB_GRBM_GFX_INDEX_BCAST_MASK;
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

/* ===================================================================== 2CB
 * THE METAL NAME IS TWO IOKit PROPERTIES, NOT A DEVICE-ID TABLE - and that
 * is also how "Metal: 3" gets into System Information.
 *
 * 2C9 recorded "the marketing name lives in a device-id -> string table
 * inside Apple's driver and 1002:1681 is not in it". That is WRONG, and
 * AMDRadeonX6000MTLDriver says so in two functions:
 *
 *   familyName()  = lookup(@"ATY,FamilyName") ?: @"Radeon HD"
 *   deviceName()  = lookup(@"ATY,DeviceName") ?: @"GFX10 Family Unknown Prototype"
 *
 * where `lookup` is, byte-exactly (both stubs decoded to their GOT slots and
 * both slots resolved against IOKit's own export table):
 *
 *   IORegistryEntryGetParentEntry(accel_entry, "IOService", &parent)
 *   IORegistryEntryCreateCFProperty(parent, key, kCFAllocatorDefault, 0)
 *
 * So the name is `"AMD " + family + " " + device` - which is exactly the
 * observed "AMD Radeon HD GFX10 Family Unknown Prototype", both halves being
 * the FALLBACKS because neither property exists. There is no table and
 * nothing to patch: the two properties are read from **the accelerator's
 * single parent in the IOService plane**, which on this machine is the
 * `IGPU@0` IOPCIDevice (verified: the MTLDevice's registryID 0x100000507 IS
 * the accelerator node, and its parent is IGPU@0).
 *
 * `GetParentEntry`, NOT `SearchCFProperty` - the lookup is ONE level and not
 * recursive, so the properties must sit on IGPU@0 exactly. Publishing them
 * on our own framebuffer or on the peer would be invisible, and both are
 * siblings of the accelerator rather than ancestors.
 *
 * WHY THIS ALSO ANSWERS "Metal: 3". SPDisplaysReporter pairs each MTLDevice
 * to a GPU dictionary by NAME, four tests (disassembled at 0xa50d-0xa64c):
 *   1  [sppci_model caseInsensitiveCompare:[dev name]] == NSOrderedSame
 *   2  [dev name] contains @"Intel"
 *   3  [dev name] contains (sppci_model with @"AMD" removed)
 *   4  [dev name] contains @"Apple"
 * All four fail here, measured on the real strings, so the reporter bails
 * and writes NONE of its six per-device keys. Make the Metal name equal our
 * `model` and test 1 matches exactly, which is why one fix serves both asks.
 *
 * So the split has to RECONSTRUCT our own model string through Apple's
 * format. `"AMD " + family + " " + device` must come back byte-identical to
 * `model`, which means: require the "AMD " prefix, then cut the remainder at
 * its FIRST space. "AMD Radeon 680M" -> "Radeon" + "680M"; the generic
 * "AMD Radeon Graphics (Rembrandt, 12 CU)" -> "Radeon" + "Graphics
 * (Rembrandt, 12 CU)". A model that does not start with "AMD " is REFUSED
 * rather than published, because Apple prepends that word unconditionally
 * and publishing anything else would make the name disagree with `model` -
 * which is the exact failure this is fixing.
 * ===================================================================== */

/* Split `model` the way AMDRadeonX6000MTLDriver will reassemble it. Returns
 * 1 on success with both buffers NUL-terminated, 0 on refusal with both
 * buffers emptied - a caller must publish nothing on 0 rather than publish
 * one half (rule 14; half a name is a name that does not match). */
static inline int rb_gpu_aty_split(const char *model,
                                   char *family, uint32_t flen,
                                   char *device, uint32_t dlen)
{
    static const char kPrefix[] = "AMD ";
    uint32_t i, j, cut;

    if (!family || !device || flen < 2u || dlen < 2u) {
        return 0;
    }
    family[0] = '\0';
    device[0] = '\0';
    if (!model) {
        return 0;
    }
    /* Apple prepends "AMD " itself, so our model must carry it or the
     * reassembled name cannot equal `model`. */
    for (i = 0; kPrefix[i]; i++) {
        if (model[i] != kPrefix[i]) { return 0; }
    }
    /* Cut the remainder at its first space: everything before is the family,
     * everything after is the device, and the separator is the single space
     * Apple puts back. */
    cut = 0u;
    for (j = i; model[j]; j++) {
        if (model[j] == ' ') { cut = j; break; }
    }
    if (cut == 0u || cut == i) {
        return 0;   /* no space, or an empty family - nothing to split */
    }
    if (model[cut + 1u] == '\0') {
        return 0;   /* trailing space, so the device half would be empty */
    }
    if ((cut - i) + 1u > flen) {
        return 0;
    }
    for (j = 0; i + j < cut; j++) { family[j] = model[i + j]; }
    family[j] = '\0';
    for (j = 0; model[cut + 1u + j]; j++) {
        if (j + 1u >= dlen) { family[0] = '\0'; device[0] = '\0'; return 0; }
        device[j] = model[cut + 1u + j];
    }
    device[j] = '\0';
    return 1;
}

/* What Apple will build from those two halves, so the hosttest can check the
 * round trip rather than trust the split. Returns 1 on success. */
static inline int rb_gpu_aty_join(const char *family, const char *device,
                                  char *out, uint32_t outlen)
{
    static const char kPre[] = "AMD ";
    uint32_t i = 0, j;

    if (!out || outlen < 2u || !family || !device) { return 0; }
    out[0] = '\0';
    for (j = 0; kPre[j]; j++)   { if (i + 1u >= outlen) { return 0; } out[i++] = kPre[j]; }
    for (j = 0; family[j]; j++) { if (i + 1u >= outlen) { return 0; } out[i++] = family[j]; }
    if (i + 1u >= outlen) { return 0; }
    out[i++] = ' ';
    for (j = 0; device[j]; j++) { if (i + 1u >= outlen) { return 0; } out[i++] = device[j]; }
    out[i] = '\0';
    return 1;
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
