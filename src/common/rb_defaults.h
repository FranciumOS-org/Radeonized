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
    "-rbgpurlcsub", "-rbgpunogates", "-rbgpunocgcg", "-rbgpupdeenc",
    "-rbgpuvramadj", "-rbgpuvmpteng1", "-rbgpuforce1b", "-rbgpusdmaone",
    "-rbgpuforceenable", "-rbgpumewatch", "-rbgpuautoloadtruth",
    /* the display chain, in the order it was built */
    "-rbgpudisplaypipe", "-rbgpuvbl", "-rbgpuvblpump", "-rbgpufbvbl",
    "-rbgpufbvram", "-rbgpufbaccel", "-rbgpuiop", "-rbgpuagdc",
    "-rbgpuagdccaps", "-rbgpuownfb", "-rbgpufbaty", "-rbgpufbamtr",
    "-rbgpufbhubp", "-rbgpufbflipall", "-rbgpufbflippend", "-rbgpufbnotready",
    "-rbgpufbotgany", "-rbgpufbretire", "-rbgpufbpoll", "-rbgpuschedtick",
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

#endif /* RB_DEFAULTS_H */
