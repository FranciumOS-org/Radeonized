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
    "-rbgpumcnew", "-rbgpumcreg", "-rbgpufbbacklight", "-rbgpufbnocrumb",
    "-rbgpufbvramrange", "-rbgpufbvramrangeall", "-rbgpunosentinel",
    "-rbgpunodisppanic", "-rbgpusdmahook", "-rbgpustampraw", "-rbgpuregdump",
    "-rbgpupdefix", "-rbgpubeat", "-rbgpunosmu", "-rbgpunoguard",
    "-rbgpunocapture", "-rbgpunosrm", "-rbgpunorotate",
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

    return failures;
}
