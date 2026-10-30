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
    "-rbgpufbblparm",
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

    CHECK(RB_DEFAULT_ON_COUNT == 45,
          "the table holds the 45 tokens boot 231 ran with");

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
    CHECK(!rb_default_on("-rbgpufbbacklight"), "-rbgpufbbacklight stays OFF (never booted)");
    /* 2xy: -rbgpufbbuiltin changes which IODisplay subclass matches the
     * connection, which reaches the display-prefs key, mirroring and the
     * wrangler's dimming as well as the slider. Never booted either. */
    CHECK(!rb_default_on("-rbgpufbbuiltin"), "-rbgpufbbuiltin stays OFF (never booted)");
    /* 2y0: creates and registers a new IOKit service as a child of the
     * framebuffer, which is a different risk from the other two. */
    CHECK(!rb_default_on("-rbgpufbblparm"), "-rbgpufbblparm stays OFF (never booted)");
    /* 2xz: -rbgpuosforce must never be in the table. It exists to arm the
     * table on an OS where none of the measured offsets were measured, and a
     * default-on override of a safety gate is not an override at all. */
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

    return failures;
}
