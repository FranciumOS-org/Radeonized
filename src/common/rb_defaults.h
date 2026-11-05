/* rb_defaults.h - which boot-args are ON when nothing is in boot-args (2xw).
 *
 * Until 2xw every feature was opt-in: a working desktop needed forty-five
 * `-rbgpu*` tokens in boot-args and five NVRAM one-shots re-armed by hand
 * before every boot. That was right while each one could hang the machine. It
 * is wrong now that the configuration has been stable for boots 215-231: the
 * machine is a daily driver and the daily driver's configuration should be the
 * default.
 *
 * So this table IS that configuration - the exact token set boot 231 ran
 * with, nothing added and nothing dropped, copied from `nvram boot-args` so a
 * default boot is byte-for-byte the configuration that was measured, not a
 * tidied-up guess at it.
 *
 * Every entry is turned OFF by its own negative token, derived mechanically by
 * `rb_off_token()`: `-rbgpufbhubp` -> `-rbgpunofbhubp`, and for the two
 * entries that are already negative, `-rbgpunocgcg` -> `-rbgpucgcg`. Nothing
 * here needs a positive token any more, but one still works: a token in
 * boot-args that is already the default is simply redundant, which is what
 * makes this change safe to install without editing boot-args first.
 *
 * `-rbgpustock` turns every entry off at once, and `-rbgpuoff` additionally
 * stops the plugin and makes the framebuffer decline the panel. Those two are
 * the rescue, and rule 38 is why the framebuffer has to honour them: the panel
 * taken with no accelerator underneath freezes on the last frame drawn.
 *
 * Freestanding and pure: no kernel headers, no allocation, no I/O. The
 * hosttest checks the whole table, including that no derived negative token
 * collides with or prefixes another token (rule 30, which is about our own
 * greps - the kernel's own parse is exact).
 */
#ifndef RB_DEFAULTS_H
#define RB_DEFAULTS_H

#include <stddef.h>

#define RB_PREFIX     "-rbgpu"
#define RB_PREFIX_LEN 6

/* The configuration of boot 231, which is the one that works. Grouped the way
 * the project reasons about it; the order has no meaning to the lookup. */
static const char *const rb_default_on_tokens[] = {
    /* the accelerator: ring, page tables, SMU/PSP bring-up, the CP-halt fix */
    "-rbgpusync570", "-rbgpuspoofttl", "-rbgpupsphw", "-rbgpusmubringup",
    "-rbgpustampwait", "-rbgpuptbase", "-rbgpuptfix", "-rbgpugrbmfix",
    "-rbgpurlcsub", "-rbgpunogates", "-rbgpunocgcg", "-rbgpunopg",
    "-rbgpupdeenc",
    "-rbgpuvramadj", "-rbgpuvmpteng1", "-rbgpuforce1b", "-rbgpusdmaone",
    "-rbgpuforceenable", "-rbgpumewatch", "-rbgpuautoloadtruth",
    /* the display chain, in the order it was built */
    "-rbgpudisplaypipe", "-rbgpuvbl", "-rbgpuvblpump", "-rbgpufbvbl",
    "-rbgpufbvram", "-rbgpufbaccel", "-rbgpuiop", "-rbgpuagdc",
    "-rbgpuagdccaps", "-rbgpuownfb", "-rbgpufbaty", "-rbgpufbamtr",
    "-rbgpufbhubp", "-rbgpufbflipall", "-rbgpufbflippend", "-rbgpufbnotready",
    "-rbgpufbotgany", "-rbgpufbretire", "-rbgpufbpoll", "-rbgpuschedtick",
    /* 2CA: the GPU's name and carve-out size read from the chip instead of
     * hardcoded. Default-on because on this machine the derived answer is
     * byte-identical to the literal it replaces, so the only observable
     * change is the new keys - and because a fallback is built in (rule 38),
     * the failure mode is the behaviour of every boot before it. Read-only
     * registers; no new write surface. */
    "-rbgpufbgpuname",
    /* brightness, measured working on this OS across consecutive boots:
     * 2xs the duty cycle (boot 249), 2xy the AppleBacklightDisplay match
     * (boot 250), 2y1 the full-length parameter name (boot 252), 2y2 the
     * `linear-brightness` key CoreBrightness actually writes (boot 253 - the
     * slider and the brightness keys both move the panel). These were opt-in
     * while each was unbooted; the standard this table asks for is a measured
     * boot, and they now have four between them. */
    "-rbgpufbbacklight", "-rbgpufbbuiltin", "-rbgpufbblparm",
    /* instruments. They cost diag-table slots and nothing else, and they are
     * part of the measured configuration, so they stay on: a boot that reads
     * differently from boot 231 should differ in the hardware, not in what we
     * can see of it. */
    "-rbgpuringprobe", "-rbgpuringdump", "-rbgpumcprobe", "-rbgpuaccelprobe",
    "-rbgpuschedprobe", "-rbgpuflipprobe",
};

#define RB_DEFAULT_ON_COUNT \
    (sizeof(rb_default_on_tokens) / sizeof(rb_default_on_tokens[0]))

static inline int rb_streq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == '\0' && *b == '\0';
}

/* 1 if this token is on with nothing in boot-args. */
static inline int rb_default_on(const char *tok)
{
    unsigned i;

    if (!tok) {
        return 0;
    }
    for (i = 0; i < RB_DEFAULT_ON_COUNT; i++) {
        if (rb_streq(tok, rb_default_on_tokens[i])) {
            return 1;
        }
    }
    return 0;
}

/* The token that turns `tok` off, written into `out`. Returns 1 on success, 0
 * if `tok` does not start with `-rbgpu` or `out` is too small - and on 0 the
 * caller must treat the feature as ON only if it can still check something,
 * which in practice means this never fails for a table entry (asserted). */
static inline int rb_off_token(const char *tok, char *out, size_t n)
{
    const char *rest;
    size_t i = 0, j;

    if (!tok || !out || n == 0) {
        return 0;
    }
    out[0] = '\0';
    for (j = 0; j < RB_PREFIX_LEN; j++) {
        if (tok[j] != RB_PREFIX[j]) {
            return 0;
        }
    }
    rest = tok + RB_PREFIX_LEN;
    /* already negative: -rbgpunocgcg -> -rbgpucgcg */
    if (rest[0] == 'n' && rest[1] == 'o' && rest[2] != '\0') {
        rest += 2;
        for (j = 0; j < RB_PREFIX_LEN; j++) {
            if (i + 1 >= n) { out[0] = '\0'; return 0; }
            out[i++] = RB_PREFIX[j];
        }
    } else {
        for (j = 0; j < RB_PREFIX_LEN; j++) {
            if (i + 1 >= n) { out[0] = '\0'; return 0; }
            out[i++] = RB_PREFIX[j];
        }
        if (i + 2 >= n) { out[0] = '\0'; return 0; }
        out[i++] = 'n';
        out[i++] = 'o';
    }
    for (j = 0; rest[j]; j++) {
        if (i + 1 >= n) { out[0] = '\0'; return 0; }
        out[i++] = rest[j];
    }
    out[i] = '\0';
    return 1;
}

/* 2xx: TELLING OUR OWN "0" APART FROM THE USER'S.
 *
 * 2xw inverted the five NVRAM one-shots: `rbgpu-<x>=0` is now a durable
 * disarm and the plugin never writes those keys. But two of them were
 * consumed by OVERWRITE before 2xw - `rbgpu_nvram_consume_oneshot_ow` wrote
 * a literal "0" into the key as the way of spending it (rbgpu-fb in 2wj,
 * rbgpu-tick in 2xg). So on any machine that ever ran an armed pre-2xw boot
 * those two keys already hold exactly the value the new reader treats as "the
 * user turned this off", and the first 2xw boot came up with the scheduler
 * tick disarmed and the panel declined: boot 236's own record reads
 * `tick_armed=0 tick_oneshot=3`, why 3 being "the key reads 0".
 *
 * Nothing in the VALUE can separate the two cases - a hand-typed
 * `sudo nvram rbgpu-tick=0` stores the same byte. So they are separated by
 * GENERATION instead. `rbgpu-gen` is written once, by the first 2xw+ boot
 * that gets far enough to write it; before it exists, a "0" in one of the two
 * overwrite-consumed keys is our own spent one-shot and is ignored, and after
 * it exists every "0" is the user's and is obeyed. The other three keys
 * (`rbgpu-arm`, `rbgpu-agdc`, `rbgpu-iop`) were consumed by REMOVAL, so a "0"
 * in one of them was always hand-written and is obeyed either way - which the
 * live machine confirms: arm/agdc/iop read "1", tick/fb read "0".
 *
 * Rule 37's question, asked the other way round: what does a stale value
 * written by our own earlier build look like to the new reader? */
#define RB_GEN_KEY     "rbgpu-gen"
#define RB_GEN_CURRENT "1"

static const char *const rb_legacy_zero_keys[] = {
    "rbgpu-tick",   /* 2xg: consumed by overwrite before the first tick armed */
    "rbgpu-fb",     /* 2wj: consumed by overwrite so a disarm could not stick */
};
#define RB_LEGACY_ZERO_COUNT \
    ((int)(sizeof(rb_legacy_zero_keys) / sizeof(rb_legacy_zero_keys[0])))

/* 1 if a literal "0" in `var` could have been written by our own pre-2xw
 * consume-by-overwrite path rather than by the user. */
static inline int rb_legacy_zero_key(const char *var)
{
    int i;

    if (!var) {
        return 0;
    }
    for (i = 0; i < RB_LEGACY_ZERO_COUNT; i++) {
        if (rb_streq(var, rb_legacy_zero_keys[i])) {
            return 1;
        }
    }
    return 0;
}

/* The whole decision, in one place because the plugin and the framebuffer are
 * separately armed and must answer it identically (rule 38).
 *   reads_zero  - the key exists and its first byte is '0'
 *   gen_present - RB_GEN_KEY exists in /options
 * Returns 1 if the key disarms this feature. */
static inline int rb_key_disarms(const char *var, int reads_zero,
                                 int gen_present)
{
    if (!reads_zero) {
        return 0;
    }
    if (gen_present) {
        return 1;
    }
    return !rb_legacy_zero_key(var);
}

/* 2zm: the bound on rbgpufbpubms, and on the live set of the same tunable.
 *
 * It lives here rather than inline in the kext for the reason everything else
 * in this header does: the boot-arg parse and the runtime setProperties path
 * are two separately-written callers of one decision, and rule 38 says two
 * such callers must answer identically. The hosttest owns it, so a change to
 * the range cannot land on only one of them.
 *
 * 0 is IN range and means "do not throttle" - the unthrottled pre-2zk
 * behaviour, which is the A side of the comparison rule 44 wants. The ceiling
 * is 10 s because beyond that the periodic publish every 288 vblanks is the
 * only publisher left and the tunable has stopped meaning anything. */
#define RB_PUB_MS_MAX 10000u

static inline int rb_pub_ms_ok(long v)
{
    return v >= 0 && v <= (long)RB_PUB_MS_MAX;
}

#endif /* RB_DEFAULTS_H */
