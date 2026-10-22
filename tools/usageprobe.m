// usageprobe.m (2nq) - locate `usage` in MTLTextureDescriptorPrivate by
// measurement rather than by reading the disassembly's mind.
//
// The disassembly of amdMtl_HWL_DetermineSwizzleMode reads a qword at
// desc+0x68 and tests it against 6 = ShaderWrite|RenderTarget, which LOOKS
// like Metal's usage mask. Rule 23: an inference from a disassembly is an
// instrument too. This creates one texture per usage value, each at a
// distinct width so the hook's dedup cannot fold them together, and lets
// mtlswizzle.dylib dump the descriptor for each. If 0x68 tracks the usage we
// set, it is usage.
#import <Metal/Metal.h>

int main(void) {
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    if (!dev) { fprintf(stderr, "no Metal device\n"); return 1; }
    struct { MTLTextureUsage u; const char *name; } cases[] = {
        { MTLTextureUsageShaderRead,  "ShaderRead(1)" },
        { MTLTextureUsageShaderWrite, "ShaderWrite(2)" },
        { MTLTextureUsageRenderTarget,"RenderTarget(4)" },
        { MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget, "Read|RT(5)" },
        { MTLTextureUsagePixelFormatView, "PixelFormatView(16)" },
    };
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        MTLTextureDescriptor *d = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
            width:(640 + i * 64) height:480 mipmapped:NO];
        d.usage = cases[i].u;
        d.storageMode = MTLStorageModePrivate;
        fprintf(stderr, "usageprobe: width=%u usage=%s\n",
                640 + i * 64, cases[i].name);
        id<MTLTexture> t = [dev newTextureWithDescriptor:d];
        if (!t) fprintf(stderr, "usageprobe:   newTextureWithDescriptor returned nil\n");
    }
    return 0;
}
