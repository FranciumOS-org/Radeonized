/* The default-on table (2xw): the configuration itself is data, so the test
 * checks the data, not a behaviour. */
#include <stdio.h>
#include <string.h>

#include "rb_defaults.h"

#define CHECK(cond, what) do { \
    printf((cond) ? "ok   %s\n" : "FAIL %s\n", (what)); \
    failures += !(cond); \
} while (0)

/* Every rbgpu token the sources read, default-on or not. Kept here so the
 * collision check below sees the whole namespace and not just the defaults;
 * a new token that collides with a derived negative one fails the build. */
static const char *const known[] = {
    "-rbgpuoff", "-rbgpustock", "-rbgpuarm", "-rbgpunolinear", "-rbgpumtlkoff",
    "-rbgpumtlknolate", "-rbgpufbmatch", "-rbgpuagdcearly", "-rbgpuagdcpolicy",
    "-rbgpumcnew", "-rbgpumcreg", "-rbgpufbbacklight", "-rbgpufbbuiltin",
    "-rbgpufbblparm", "-rbgpufbdrr", "-rbgpufbmodes", "-rbgpufbdmubbl",
    "-rbgpufbnocrumb",
    "-rbgpufbvramrange", "-rbgpufbvramrangeall", "-rbgpunosentinel",
    "-rbgpunodisppanic", "-rbgpusdmahook", "-rbgpustampraw", "-rbgpuregdump",
    "-rbgpupdefix", "-rbgpubeat", "-rbgpunosmu", "-rbgpunoguard",
    "-rbgpunocapture", "-rbgpunosrm", "-rbgpunorotate", "-rbgpunocaptbl",
    "-rbgpuosforce",
};
#define KNOWN_COUNT (sizeof(known) / sizeof(known[0]))

int defaults_tests(void);

int defaults_tests(void)
{
    int failures = 0;
    unsigned i, j;
    char off[64], back[64];

    printf("\n-- default-on table (2xw) --\n");

    /* 45 was boot 231's token set; 2y2 added the three brightness tokens,
     * each measured working on its own boot (249, 250, 252/253); 2z4 added
     * -rbgpunopg, splitting 2ju's power-gating write off from its clock-gating
     * one so the half with no evidence against it can be disarmed alone. The
     * number is asserted so the table cannot drift by accident - a token
     * arriving or leaving should cost a line in a journal entry, not go
     * unnoticed, and this assertion is what made 2z4 say so. */
    CHECK(RB_DEFAULT_ON_COUNT == 52,
          "the table holds boot 231's 45 tokens, 2y2's three brightness "
          "tokens, 2z4's -rbgpunopg, 2CA's -rbgpufbgpuname and 2CE's "
          "-rbgpumtlname");

    /* 2CE: the Metal name, promoted on two measured boots (417, 418). Pinned
     * by value as well as by the count, because this is the token that
     * decides whether System Information names the chip at all. */
    {
        char o[64];
        CHECK(rb_default_on("-rbgpumtlname") &&
              rb_off_token("-rbgpumtlname", o, sizeof(o)) &&
              strcmp(o, "-rbgpunomtlname") == 0,
              "-rbgpumtlname is default-on and disarmed by -rbgpunomtlname");

    /* 3gl-d: the OpenGL cave. The negative is derived from the token's own
     * name, so this also pins that `-rbgpunoglcave` is what turns it off -
     * the thing rule 30 is about, checked rather than assumed. */
    {
        char off[64];
        CHECK(rb_default_on("-rbgpuglcave") &&
              rb_off_token("-rbgpuglcave", off, sizeof(off)) &&
              strcmp(off, "-rbgpunoglcave") == 0,
              "-rbgpuglcave is default-on and disarmed by -rbgpunoglcave");
    }
    }

    /* 2z4: the new token must be disarmable, and its negative must not
     * collide with anything. The generic prefix and derivation checks below
     * cover every entry, so this only pins the one value a reader would want
     * to see written down. */
    {
        char off[64];
        CHECK(rb_default_on("-rbgpunopg") &&
              rb_off_token("-rbgpunopg", off, sizeof(off)) &&
              strcmp(off, "-rbgpupg") == 0,
              "-rbgpunopg is default-on and disarmed by -rbgpupg");
    }

    /* no duplicates: a token listed twice would hide a typo */
    int dup = 0;
    for (i = 0; i < RB_DEFAULT_ON_COUNT; i++) {
        for (j = i + 1; j < RB_DEFAULT_ON_COUNT; j++) {
            if (!strcmp(rb_default_on_tokens[i], rb_default_on_tokens[j])) {
                printf("     duplicate: %s\n", rb_default_on_tokens[i]);
                dup = 1;
            }
        }
    }
    CHECK(!dup, "no token appears twice");

    /* every entry derives a negative token, and it is not the entry itself */
    int derive_ok = 1;
    for (i = 0; i < RB_DEFAULT_ON_COUNT; i++) {
        if (!rb_off_token(rb_default_on_tokens[i], off, sizeof(off)) ||
            !strcmp(off, rb_default_on_tokens[i])) {
            printf("     no negative for %s\n", rb_default_on_tokens[i]);
            derive_ok = 0;
        }
    }
    CHECK(derive_ok, "every entry derives its own negative token");

    /* rule 30, applied to our own greps: no negative token may equal another
     * token, and none may be a prefix of another (either direction) */
    int collide = 0;
    for (i = 0; i < RB_DEFAULT_ON_COUNT; i++) {
        rb_off_token(rb_default_on_tokens[i], off, sizeof(off));
        for (j = 0; j < RB_DEFAULT_ON_COUNT; j++) {
            if (!strcmp(off, rb_default_on_tokens[j])) {
                printf("     %s negates to an existing token %s\n",
                       rb_default_on_tokens[i], off);
                collide = 1;
            }
        }
        for (j = 0; j < KNOWN_COUNT; j++) {
            if (!strcmp(off, known[j])) {
                printf("     %s negates to a known token %s\n",
                       rb_default_on_tokens[i], off);
                collide = 1;
            }
        }
        for (j = 0; j < RB_DEFAULT_ON_COUNT; j++) {
            size_t a = strlen(off), b = strlen(rb_default_on_tokens[j]);
            if (a != b && !strncmp(off, rb_default_on_tokens[j], a < b ? a : b)) {
                printf("     %s prefixes %s\n", off, rb_default_on_tokens[j]);
                collide = 1;
            }
        }
    }
    CHECK(!collide, "no derived negative token collides with or prefixes another");

    /* the two already-negative entries drop the `no` instead of doubling it */
    rb_off_token("-rbgpunocgcg", off, sizeof(off));
    CHECK(!strcmp(off, "-rbgpucgcg"), "-rbgpunocgcg negates to -rbgpucgcg");
    rb_off_token("-rbgpunogates", off, sizeof(off));
    CHECK(!strcmp(off, "-rbgpugates"), "-rbgpunogates negates to -rbgpugates");
    rb_off_token("-rbgpufbhubp", off, sizeof(off));
    CHECK(!strcmp(off, "-rbgpunofbhubp"), "-rbgpufbhubp negates to -rbgpunofbhubp");
    CHECK(rb_off_token("-rbgpufbhubp", off, sizeof(off)) &&
          rb_off_token(off, back, sizeof(back)) &&
          !strcmp(back, "-rbgpufbhubp"),
          "negating twice returns the original token");

    /* a token that is not in the table is OFF by default - the whole point */
    CHECK(!rb_default_on("-rbgpuagdcearly"), "-rbgpuagdcearly stays OFF (2rz: it wedges)");
    CHECK(!rb_default_on("-rbgpufbmatch"), "-rbgpufbmatch stays OFF (kills the accelerator)");
    CHECK(!rb_default_on("-rbgpupdefix"), "-rbgpupdefix stays OFF (the halt returns)");
    CHECK(!rb_default_on("-rbgpumcreg"), "-rbgpumcreg stays OFF (its failure is a hang)");
    /* 2y2: the three brightness tokens are IN the table now. Each was
     * measured working on this OS on its own boot - 249, 250, 252/253 - which
     * is exactly what this table asks of an entry, and the boot sentinel is
     * the recovery if one turns out wrong. The assertions are kept rather
     * than deleted, inverted, so that a token silently dropping out of the
     * table fails the build instead of quietly reverting the feature. */
    CHECK(rb_default_on("-rbgpufbbacklight"), "-rbgpufbbacklight is default-on (2xs, boot 249)");
    CHECK(rb_default_on("-rbgpufbbuiltin"), "-rbgpufbbuiltin is default-on (2xy, boot 250)");
    CHECK(rb_default_on("-rbgpufbblparm"), "-rbgpufbblparm is default-on (2y1/2y2, boots 252-253)");
    /* Each must still be switchable off from the machine it runs on - rule 37.
     * A default-on feature with no reachable negative is not a default, it is
     * a hardcode. The table-wide loop above already asserts this for every
     * entry; these name the three explicitly so the negative a user would
     * actually type is written down somewhere checkable. */
    rb_off_token("-rbgpufbbacklight", off, sizeof(off));
    CHECK(!strcmp(off, "-rbgpunofbbacklight"),
          "-rbgpufbbacklight negates to -rbgpunofbbacklight");
    rb_off_token("-rbgpufbbuiltin", off, sizeof(off));
    CHECK(!strcmp(off, "-rbgpunofbbuiltin"),
          "-rbgpufbbuiltin negates to -rbgpunofbbuiltin");
    rb_off_token("-rbgpufbblparm", off, sizeof(off));
    CHECK(!strcmp(off, "-rbgpunofbblparm"),
          "-rbgpufbblparm negates to -rbgpunofbblparm");
    /* 2xz: -rbgpuosforce must never be in the table. It exists to arm the
     * table on an OS where none of the measured offsets were measured, and a
     * default-on override of a safety gate is not an override at all. */
    /* 2y5: the refresh-rate experiment WRITES the display pipe's timing on a
     * live raster. It reverts itself, but a default-on timing write is not a
     * thing this project does, and nothing has yet measured whether this
     * panel will even sync to 2707 lines. */
    CHECK(!rb_default_on("-rbgpufbdrr"),
          "-rbgpufbdrr stays OFF - it writes OTG timing on a live panel");
    /* 2y6. The WRITE is no longer the new risk - boot 255 measured it working.
     * ADVERTISING is: getDisplayModeCount() has returned 1 since 2sl and every
     * boot since has been read against that, so a second mode changes what
     * WindowServer sees before any user has asked for anything. 2xx is the
     * standing lesson on what a WindowServer that dislikes its framebuffer
     * costs to diagnose. */
    CHECK(!rb_default_on("-rbgpufbmodes"),
          "-rbgpufbmodes stays OFF - it changes the mode list WindowServer "
          "sees at startup, which no boot has yet done");
    {
        char off[64];
        rb_off_token("-rbgpufbmodes", off, sizeof(off));
        CHECK(!strcmp(off, "-rbgpunofbmodes"),
              "and its derived negative is the usual one");
    }
    CHECK(!rb_default_on("-rbgpuosforce"),
          "-rbgpuosforce stays OFF - a default-on override of a safety gate is not an override");
    CHECK(!rb_default_on(""), "the empty token is not on");
    CHECK(!rb_default_on(NULL), "a NULL token is not on");

    /* the ones the desktop cannot do without */
    CHECK(rb_default_on("-rbgpuownfb") && rb_default_on("-rbgpufbhubp") &&
          rb_default_on("-rbgpufbamtr") && rb_default_on("-rbgpuschedtick") &&
          rb_default_on("-rbgpuvramadj"),
          "the five the accelerated desktop needs are all on");

    /* a short buffer must fail loudly rather than truncate into a token that
     * happens to be somebody else's */
    char tiny[8];
    CHECK(!rb_off_token("-rbgpufbhubp", tiny, sizeof(tiny)) && tiny[0] == '\0',
          "a buffer too small refuses and empties the output");
    CHECK(!rb_off_token("nonsense", off, sizeof(off)),
          "a token without the -rbgpu prefix is refused");

    /* 2xx: our own "0" against the user's. The two keys a pre-2xw build
     * consumed by OVERWRITE hold a literal "0" on every machine that ever ran
     * an armed pre-2xw boot, and boot 236 came up with the scheduler tick off
     * because of it (`tick_armed=0 tick_oneshot=3`). The rule is generational,
     * and these are the four corners of it. */
    CHECK(rb_legacy_zero_key("rbgpu-tick") && rb_legacy_zero_key("rbgpu-fb"),
          "the two overwrite-consumed keys are the legacy-zero keys");
    CHECK(!rb_legacy_zero_key("rbgpu-arm") && !rb_legacy_zero_key("rbgpu-agdc") &&
          !rb_legacy_zero_key("rbgpu-iop"),
          "the three removal-consumed keys are NOT, so a \"0\" in them is the "
          "user's and is always obeyed");
    CHECK(!rb_legacy_zero_key("rbgpu-safe") && !rb_legacy_zero_key(RB_GEN_KEY) &&
          !rb_legacy_zero_key("") && !rb_legacy_zero_key(NULL),
          "the sentinel latch, the generation key and the degenerate inputs "
          "are not legacy-zero keys");
    CHECK(RB_LEGACY_ZERO_COUNT == 2,
          "exactly two keys were ever consumed by overwrite");

    /* the decision table, every cell */
    CHECK(!rb_key_disarms("rbgpu-tick", 0, 0) && !rb_key_disarms("rbgpu-tick", 0, 1),
          "a key that does not read \"0\" never disarms, either generation");
    CHECK(!rb_key_disarms("rbgpu-tick", 1, 0),
          "THE BUG: rbgpu-tick=\"0\" with no generation key is our own spent "
          "one-shot and must NOT disarm the scheduler tick");
    CHECK(!rb_key_disarms("rbgpu-fb", 1, 0),
          "rbgpu-fb=\"0\" with no generation key must NOT decline the panel");
    CHECK(rb_key_disarms("rbgpu-tick", 1, 1) && rb_key_disarms("rbgpu-fb", 1, 1),
          "once the generation key exists, a hand-typed \"0\" in either key "
          "disarms - the migration is one-shot, not a permanent override");
    CHECK(rb_key_disarms("rbgpu-arm", 1, 0) && rb_key_disarms("rbgpu-arm", 1, 1),
          "rbgpu-arm=\"0\" disarms in BOTH generations - it is the documented "
          "first rescue rung and no migration may ignore it");
    CHECK(rb_key_disarms("rbgpu-agdc", 1, 0) && rb_key_disarms("rbgpu-iop", 1, 0),
          "the other two rescue keys are obeyed before the migration too");
    CHECK(rb_key_disarms(NULL, 1, 0) && rb_key_disarms(NULL, 1, 1),
          "an unnamed key reading \"0\" disarms - the safe direction when the "
          "caller cannot say which key it read");

    /* the generation key must not collide with the keys it arbitrates over,
     * and rule 26's prefix trap applies to NVRAM names as well as boot-args */
    CHECK(!rb_streq(RB_GEN_KEY, "rbgpu-gen-prev"),
          "the generation key is matched exactly, not by prefix");
    for (i = 0; i < RB_LEGACY_ZERO_COUNT; i++) {
        CHECK(!rb_streq(rb_legacy_zero_keys[i], RB_GEN_KEY),
              "no legacy-zero key is the generation key");
    }

    /* 2zm: the rbgpufbpubms bound, asserted against LITERALS rather than the
     * macro that defines it - the 2u9 trap. 0 must be INSIDE the range,
     * because 0 is the A side of 2zk's comparison and a bound that refused it
     * would silently leave the throttle on for every boot and make the
     * comparison unrunnable. */
    CHECK(rb_pub_ms_ok(0), "pubms 0 is in range - it is the A side");
    CHECK(rb_pub_ms_ok(500), "pubms 500, the default, is in range");
    CHECK(rb_pub_ms_ok(10000), "pubms 10000 is the inclusive ceiling");
    CHECK(!rb_pub_ms_ok(10001), "pubms 10001 is refused");
    CHECK(!rb_pub_ms_ok(-1), "pubms -1 is refused, so a signed read cannot "
          "wrap into a huge interval");
    CHECK(RB_PUB_MS_MAX == 10000u, "the ceiling is 10000");

    return failures;
}
