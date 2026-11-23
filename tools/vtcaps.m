/* vtcaps - what VideoToolbox says this machine can do in hardware.
 *
 * 3gvd: the one-bool answer to the question -rbgpuhevc exists to change, and
 * it lives in the repo rather than a scratch directory because /private/tmp is
 * cleared on boot and this tool's whole job is to be run AFTER a reboot (the
 * first draft of this check was a scratchpad binary that a reboot would have
 * deleted - rule 15's shape: an instrument that is not there is not a null
 * result, it is no result).
 *
 * Three readings, each with its own meaning:
 *
 *   DECODE   VTIsHardwareDecodeSupported per codec. This is the one the user
 *            can see: a FaceTime receiver advertises its capabilities and the
 *            SENDER encodes to match, so a Mac claiming no hardware decode is
 *            sent a stream a software decoder can survive - low resolution,
 *            heavy quantisation. Measured `no` for H.264, HEVC, VP9 and AV1 on
 *            boot 633, all four together, which is the control: a per-codec
 *            gap would not be uniform.
 *
 *   ENCODE   a hardware-REQUIRED VTCompressionSessionCreate. The refusal is a
 *            code (-12908 kVTCouldNotFindVideoEncoderErr), not a bool, so it
 *            is printed as one (rule 14).
 *
 *   LIST     VTCopyVideoEncoderList with the IsHardwareAccelerated flag, which
 *            is the independent instrument: boot 633 read 15 encoders and not
 *            one hardware, and a disagreement between LIST and ENCODE is
 *            itself a finding rather than something to average.
 *
 * Reads only. Creates and immediately invalidates sessions; encodes nothing.
 */
#import <Foundation/Foundation.h>
#import <VideoToolbox/VideoToolbox.h>
#include <stdio.h>

static const struct { const char *name; CMVideoCodecType codec; } kCodecs[] = {
    { "H.264", kCMVideoCodecType_H264 },
    { "HEVC",  kCMVideoCodecType_HEVC },
    { "VP9",   'vp09' },
    { "AV1",   'av01' },
};
enum { kCodecCount = sizeof kCodecs / sizeof *kCodecs };

static void cb(void *o, void *s, OSStatus st, VTEncodeInfoFlags f,
               CMSampleBufferRef sb)
{ (void)o; (void)s; (void)st; (void)f; (void)sb; }

int main(int argc, char **argv)
{
    int w = 1920, h = 1088;      /* FaceTime's own reported screen-share size */
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--size") && i + 2 < argc) {
            w = atoi(argv[++i]); h = atoi(argv[++i]);
            if (w < 16 || h < 16 || w > 16384 || h > 16384) { w = 1920; h = 1088; }
        } else {
            fprintf(stderr, "usage: %s [--size W H]\n"
                    "  Prints hardware decode and encode capability per codec.\n"
                    "  Default size 1920x1088, which is what FaceTime's screen\n"
                    "  share asks for (1080 padded to a macroblock multiple).\n",
                    argv[0]);
            return 2;
        }
    }

    printf("vtcaps: %dx%d\n\n", w, h);

    printf("  DECODE (VTIsHardwareDecodeSupported)\n");
    int anyDec = 0;
    for (int i = 0; i < kCodecCount; i++) {
        int ok = VTIsHardwareDecodeSupported(kCodecs[i].codec) ? 1 : 0;
        anyDec |= ok;
        printf("    %-6s %s\n", kCodecs[i].name, ok ? "YES" : "no");
    }

    printf("\n  ENCODE (hardware-required session, err 0 = present)\n");
    int anyEnc = 0;
    for (int i = 0; i < kCodecCount; i++) {
        const void *k[1] = { kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder };
        const void *v[1] = { kCFBooleanTrue };
        CFDictionaryRef spec = CFDictionaryCreate(NULL, k, v, 1,
                                   &kCFTypeDictionaryKeyCallBacks,
                                   &kCFTypeDictionaryValueCallBacks);
        VTCompressionSessionRef s = NULL;
        OSStatus e = VTCompressionSessionCreate(NULL, w, h, kCodecs[i].codec,
                                                spec, NULL, NULL, cb, NULL, &s);
        CFRelease(spec);
        if (s) { VTCompressionSessionInvalidate(s); CFRelease(s); }
        if (e == 0) { anyEnc = 1; }
        printf("    %-6s err %d%s\n", kCodecs[i].name, (int)e,
               e == 0 ? "  (hardware encoder EXISTS)"
                      : (e == -12908 ? "  (-12908 = no such encoder)" : ""));
    }

    printf("\n  LIST (VTCopyVideoEncoderList)\n");
    CFArrayRef list = NULL;
    OSStatus st = VTCopyVideoEncoderList(NULL, &list);
    long total = 0, hw = 0;
    if (st == 0 && list) {
        total = (long)CFArrayGetCount(list);
        for (CFIndex i = 0; i < total; i++) {
            CFDictionaryRef d = CFArrayGetValueAtIndex(list, i);
            CFBooleanRef b = CFDictionaryGetValue(d,
                                 kVTVideoEncoderList_IsHardwareAccelerated);
            if (b && CFBooleanGetValue(b)) {
                hw++;
                CFStringRef nm = CFDictionaryGetValue(d,
                                     kVTVideoEncoderList_EncoderName);
                printf("    hardware: %s\n",
                       nm ? [(__bridge NSString *)nm UTF8String] : "(unnamed)");
            }
        }
        CFRelease(list);
    }
    printf("    err %d, %ld encoders, %ld hardware\n", (int)st, total, hw);

    /* The two instruments must agree, and when they do not that is the
     * finding, not an average (rule 18). */
    printf("\n  verdict: decode %s, encode %s",
           anyDec ? "SOME HARDWARE" : "none", anyEnc ? "SOME HARDWARE" : "none");
    if ((hw > 0) != (anyEnc != 0)) {
        printf("  <-- LIST and ENCODE DISAGREE (%ld hardware listed)", hw);
    }
    printf("\n");
    return 0;
}
