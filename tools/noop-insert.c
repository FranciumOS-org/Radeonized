// A dylib that does nothing at all, to answer one question: does this app
// refuse ANY inserted library, or only ours? It links nothing beyond libc and
// touches no framework, so if an app dies with this inserted, the insertion
// itself is the cause and no amount of fixing iosurftex will help.
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

__attribute__((constructor)) static void noop_init(void)
{
    const char *lf = getenv("RBGPU_NOOP_LOGFILE");
    FILE *f = (lf && *lf) ? fopen(lf, "a") : stderr;
    if (!f) f = stderr;
    fprintf(f, "noop-insert: loaded in pid %d (%s)\n", getpid(), getprogname());
    fflush(f);
}
