/*
 * modeset - list and select display modes from the shell.
 *
 * The Displays panel is the only other way to exercise -rbgpufbmodes, and an
 * A/B across eight modes through a GUI is slow enough to discourage running
 * the experiment. This is pure CoreGraphics userspace: no kext, no reboot.
 *
 *   modeset              list every mode CG reports, marking the current one
 *   modeset <hz>         select the mode whose refresh is nearest <hz>
 *
 * CG reports the rate our framebuffer advertises via getInformationForDisplayMode,
 * so the printed Hz is our own derived number round-tripping back through
 * WindowServer - which is itself worth reading.
 */
#include <ApplicationServices/ApplicationServices.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

int main(int argc, char **argv)
{
    CGDirectDisplayID dpy = CGMainDisplayID();
    CFArrayRef modes = CGDisplayCopyAllDisplayModes(dpy, NULL);
    if (!modes) { fprintf(stderr, "modeset: no mode list\n"); return 1; }

    CGDisplayModeRef cur = CGDisplayCopyDisplayMode(dpy);
    const CFIndex n = CFArrayGetCount(modes);
    double want = (argc > 1) ? atof(argv[1]) : 0.0;
    CGDisplayModeRef best = NULL;
    double bestErr = 1e9;

    for (CFIndex i = 0; i < n; i++) {
        CGDisplayModeRef m = (CGDisplayModeRef)CFArrayGetValueAtIndex(modes, i);
        const double hz = CGDisplayModeGetRefreshRate(m);
        const size_t w = CGDisplayModeGetWidth(m), h = CGDisplayModeGetHeight(m);
        const bool isCur = cur && CGDisplayModeGetIODisplayModeID(m) ==
                                  CGDisplayModeGetIODisplayModeID(cur);
        if (want == 0.0) {
            printf("%s id=%-4d %zux%zu @ %.3f Hz\n", isCur ? "*" : " ",
                   (int)CGDisplayModeGetIODisplayModeID(m), w, h, hz);
        } else if (hz > 0.0) {
            const double err = fabs(hz - want);
            if (err < bestErr) { bestErr = err; best = m; }
        }
    }

    int rc = 0;
    if (want != 0.0) {
        if (!best) { fprintf(stderr, "modeset: no mode with a refresh rate\n"); rc = 1; }
        else {
            CGDisplayConfigRef cfg;
            if (CGBeginDisplayConfiguration(&cfg) != kCGErrorSuccess) {
                fprintf(stderr, "modeset: CGBeginDisplayConfiguration failed\n");
                rc = 1;
            } else {
                CGConfigureDisplayWithDisplayMode(cfg, dpy, best, NULL);
                const CGError e = CGCompleteDisplayConfiguration(cfg, kCGConfigureForSession);
                if (e != kCGErrorSuccess) {
                    fprintf(stderr, "modeset: CGCompleteDisplayConfiguration = %d\n", (int)e);
                    rc = 1;
                } else {
                    printf("id=%d @ %.3f Hz\n",
                           (int)CGDisplayModeGetIODisplayModeID(best),
                           CGDisplayModeGetRefreshRate(best));
                }
            }
        }
    }

    if (cur) { CGDisplayModeRelease(cur); }
    CFRelease(modes);
    return rc;
}
