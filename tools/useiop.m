// useiop (2ql): the ORACLE for "why does `Display::GetNextSurface` take the
// dead MP path instead of the DisplaySurfacePool path?"
//
// `CoreDisplay::Display::GetNextSurface` (unslid 0x7ff80408bf86) opens with
// exactly two gates, and they pick between the two worlds:
//
//   0x7ff80408bfd2  callq CoreDisplay::UseIOPresentment()
//   0x7ff80408bfd7  testb %al,%al ; je  0x7ff80408c04e   -> LEGACY
//   0x7ff80408bfdb  cmpb  $0x1, [0x7ff843bac773]         ("displaySurfaceUsesIOSurface")
//   0x7ff80408bfe2  jne   0x7ff80408c04e                 -> LEGACY
//                   fall through                         -> DisplaySurfacePool
//
// LEGACY is `CGXAcquireDisplayDeviceSurface`, which 2qk proved cannot work on
// this machine: it ends in IOAccelSurface::set_id_mode, which rejects every
// surface id below 256 and says so in the kernel log. The pool path is the one
// that calls CD_IOSurfaceCreate and puts a real IOSurface under the desktop.
// `MPAccessSurfaceForDisplayDevice`'s gate C is the complement -
// `if (UseIOPresentment()) return kCGErrorFailure` - so boot 149 reaching
// set_id_mode is already a MEASUREMENT that UseIOPresentment() was false.
//
// UseIOPresentment (unslid 0x7ff804057b40), read instruction by instruction:
//
//   if (gUseIOP != 1) return gUseIOP & 1;              // "useIOP"
//   supported = (useIOP_onIG ? 0x02 : 0)               // byte 0x7ff843bac76e
//             | (useIOP_onNVIDIA ? 0x08 : 0)           //      0x7ff843bad281
//             | (useIOP_onAMD ? 0x04 : 0)              //      0x7ff843bad282
//             | (useIOP_onAPV ? 0x10 : 0);             //      0x7ff843bad283
//   present = gSystemGPUVendors;                       // int  0x7ff843bb1d64
//   if (present == 0 || (supported & present) != present) { gUseIOP = 0; return 0; }
//   return gUseIOP;
//
// All five bytes are CFPreferences booleans in the domain `com.apple.CoreDisplay`,
// read by CGXMappedDisplayStart+950..1050 through one helper
// (0x7ff80405597a = CFPreferencesCopyValue + CFBooleanGetValue).
//
// `present` is built by __GetSystemGPUVendors_block_invoke (0x7ff80412ce2c):
// IOServiceGetMatchingServices(IOServiceMatching("IOFramebuffer")), and for
// each service 0x7ff80412cc38 does IOObjectCopyClass() then four
// CFStringFind()s - "Intel" -> 0x2, "AMD" -> 0x4, "NVDA" -> 0x8,
// "AppleParavirt" -> 0x10, and NO MATCH -> 0x1.
//
// The asymmetry that decides everything: `supported` is assembled only from
// bits 0x2/0x4/0x8/0x10, so bit 0x1 can NEVER be in it. One unclassifiable
// IOFramebuffer therefore makes (supported & present) != present for every
// possible value of every preference.
//
// The DEFAULT for all five preferences is 1, not 0.  CGXMappedDisplayStart
// passes `[0x7ff843bad260]` as the default to all five, that byte is a
// function-local static filled from `[0x7ff843bad738]`, and the only writer of
// `bad738` is the block in `CoreDisplay::VorpalBlade()` (0x7ff8040598b7),
// which stores $1 unconditionally - every path through the block falls into
// the store.  So on a stock machine with no keys set, supported = 0x1e.
//
// RULE 14/26: this program reproduces Apple's code.  `CoreDisplay::UseIOPresentment`
// IS exported and is called too, but its answer in THIS process is worthless
// as a cross-check and is reported as such: CGXMappedDisplayStart never ran
// here, so `gSystemGPUVendors` is still 0 and Apple's function returns 0 from
// the `present == 0` branch - the right answer for the wrong reason.  Only
// WindowServer's own copy would be a real oracle.  What IS measured here, and
// is process-independent, is the vendor mask in section 1: IOObjectCopyClass
// on the live IOFramebuffer services.
#import <Foundation/Foundation.h>
#import <IOKit/IOKitLib.h>
#include <dlfcn.h>
#include <stdio.h>

static int prefbool(CFStringRef key, int dflt, int *found)
{
    CFPropertyListRef v = CFPreferencesCopyValue(key,
            CFSTR("com.apple.CoreDisplay"),
            kCFPreferencesAnyUser, kCFPreferencesCurrentHost);
    if (!v) {
        v = CFPreferencesCopyValue(key, CFSTR("com.apple.CoreDisplay"),
                kCFPreferencesCurrentUser, kCFPreferencesAnyHost);
    }
    if (!v) { *found = 0; return dflt; }
    int r = dflt;
    if (CFGetTypeID(v) == CFBooleanGetTypeID()) { r = CFBooleanGetValue(v) ? 1 : 0; *found = 1; }
    else *found = 0;
    CFRelease(v);
    return r;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);   // 2qc: a fault must not eat the output
    printf("useiop (2ql) - does CoreDisplay take the IOPresentment surface path?\n\n");

    // ---- `present`: exactly __GetSystemGPUVendors_block_invoke -------------
    printf("1. IOFramebuffer services and the vendor bit each contributes\n");
    printf("   (IOObjectCopyClass, then CFStringFind for Intel/AMD/NVDA/AppleParavirt)\n");
    io_iterator_t it = IO_OBJECT_NULL;
    kern_return_t kr = IOServiceGetMatchingServices(kIOMainPortDefault,
                           IOServiceMatching("IOFramebuffer"), &it);
    if (kr != KERN_SUCCESS) {
        printf("   IOServiceGetMatchingServices failed 0x%x - INCONCLUSIVE\n", kr);
        return 3;
    }
    unsigned present = 0;
    int nfb = 0;
    io_object_t o;
    while ((o = IOIteratorNext(it))) {
        CFStringRef cls = IOObjectCopyClass(o);
        unsigned bit = 0x1;
        if (cls) {
            if (CFStringFind(cls, CFSTR("Intel"), kCFCompareCaseInsensitive).location != kCFNotFound)         bit = 0x2;
            else if (CFStringFind(cls, CFSTR("AMD"), kCFCompareCaseInsensitive).location != kCFNotFound)       bit = 0x4;
            else if (CFStringFind(cls, CFSTR("AppleParavirt"), kCFCompareCaseInsensitive).location != kCFNotFound) bit = 0x10;
            else if (CFStringFind(cls, CFSTR("NVDA"), kCFCompareCaseInsensitive).location != kCFNotFound)      bit = 0x8;
        }
        io_name_t nm = {0};
        IORegistryEntryGetName(o, nm);
        printf("   %-24s name=%-16s -> bit 0x%02x%s\n",
               cls ? [(__bridge NSString *)cls UTF8String] : "(no class)",
               nm, bit, bit == 0x1 ? "   <-- UNKNOWN VENDOR" : "");
        present |= bit;
        if (cls) CFRelease(cls);
        IOObjectRelease(o);
        nfb++;
    }
    IOObjectRelease(it);
    if (nfb == 0) {
        printf("   no IOFramebuffer services at all - INCONCLUSIVE (not this machine's state)\n");
        return 3;
    }
    printf("   present mask = 0x%02x  (%d framebuffer%s)\n\n", present, nfb, nfb == 1 ? "" : "s");

    // ---- `supported`: the five preferences --------------------------------
    printf("2. com.apple.CoreDisplay preferences (default 1 - VorpalBlade sets the\n");
    printf("   shared default byte to 1 unconditionally; see the header)\n");
    int f_m = 0, f_ig = 0, f_nv = 0, f_amd = 0, f_apv = 0;
    int useIOP   = prefbool(CFSTR("useIOP"),          1, &f_m);
    int on_ig    = prefbool(CFSTR("useIOP_onIG"),     1, &f_ig);
    int on_nv    = prefbool(CFSTR("useIOP_onNVIDIA"), 1, &f_nv);
    int on_amd   = prefbool(CFSTR("useIOP_onAMD"),    1, &f_amd);
    int on_apv   = prefbool(CFSTR("useIOP_onAPV"),    1, &f_apv);
    printf("   useIOP          = %d  %s\n", useIOP, f_m   ? "(set)" : "(absent -> default)");
    printf("   useIOP_onIG     = %d  %s  bit 0x02\n", on_ig,  f_ig  ? "(set)" : "(absent -> default)");
    printf("   useIOP_onAMD    = %d  %s  bit 0x04\n", on_amd, f_amd ? "(set)" : "(absent -> default)");
    printf("   useIOP_onNVIDIA = %d  %s  bit 0x08\n", on_nv,  f_nv  ? "(set)" : "(absent -> default)");
    printf("   useIOP_onAPV    = %d  %s  bit 0x10\n", on_apv, f_apv ? "(set)" : "(absent -> default)");
    unsigned supported = (on_ig ? 0x2 : 0) | (on_nv ? 0x8 : 0) | (on_amd ? 0x4 : 0) | (on_apv ? 0x10 : 0);
    printf("   supported mask = 0x%02x\n\n", supported);
    printf("   NOTE: these are read as WindowServer reads them, but this process is\n");
    printf("   not WindowServer; a per-user-domain value could differ.  The masks\n");
    printf("   above are the reproduction's inputs, not WindowServer's own bytes.\n\n");

    // ---- the verdict, with every branch named ------------------------------
    printf("3. UseIOPresentment() reproduced\n");
    int verdict;
    const char *why;
    if (useIOP != 1)                              { verdict = 0; why = "`useIOP` is not 1 - returns at the first instruction"; }
    else if (present == 0)                        { verdict = 0; why = "no GPU vendors present"; }
    else if ((supported & present) != present)    { verdict = 0; why = "a present vendor is not in the supported set"; }
    else                                          { verdict = 1; why = "every present vendor is supported"; }
    printf("   reproduced UseIOPresentment() = %d   (%s)\n", verdict, why);

    // ---- Apple's own answer, if it is reachable ---------------------------
    void *h = dlopen("/System/Library/Frameworks/CoreDisplay.framework/CoreDisplay", RTLD_LAZY);
    int (*real)(void) = h ? (int (*)(void))dlsym(h, "_ZN11CoreDisplay16UseIOPresentmentEv") : NULL;
    if (!real) real = h ? (int (*)(void))dlsym(h, "CoreDisplay_UseIOPresentment") : NULL;
    if (real) {
        int r = real() & 1;
        printf("   Apple's UseIOPresentment()    = %d  <-- NOT a cross-check\n", r);
        printf("   In this process CGXMappedDisplayStart never ran, so CoreDisplay's\n");
        printf("   gSystemGPUVendors is still 0 and Apple's function returns 0 from its\n");
        printf("   `present == 0` branch.  A 0 here agrees with the verdict by accident.\n");
        printf("   Only WindowServer's own copy of these globals could confirm it.\n");
        if (r != 0)
            printf("   (A 1 here WOULD be meaningful and would contradict everything above.)\n");
    } else {
        printf("   Apple's UseIOPresentment()    = not exported, reproduction only\n");
    }

    // ---- what it means -----------------------------------------------------
    printf("\n4. Verdict\n");
    if (verdict) {
        printf("   IOPresentment IS enabled -> Display::GetNextSurface takes the\n");
        printf("   DisplaySurfacePool path.  If the desktop still composites in\n");
        printf("   software, the defect is INSIDE the pool, not in this gate.\n");
        return 0;
    }
    if (present & 0x1) {
        printf("   An IOFramebuffer here classifies as UNKNOWN (bit 0x1), and\n");
        printf("   `supported` is built only from bits 0x02/0x04/0x08/0x10 - bit 0x1\n");
        printf("   can never be in it.  So (supported & present) != present holds for\n");
        printf("   EVERY value of EVERY preference: no `defaults write` can turn\n");
        printf("   IOPresentment on while that framebuffer's class name matches none\n");
        printf("   of Intel / AMD / NVDA / AppleParavirt.\n");
        return 1;
    }
    printf("   IOPresentment is off, and the reason is a preference, not the\n");
    printf("   vendor classification: present=0x%02x is fully classifiable and\n", present);
    printf("   supported=0x%02x does not cover it.  Setting the matching\n", supported);
    printf("   `useIOP*` keys in com.apple.CoreDisplay would flip this gate.\n");
    return 2;
}
