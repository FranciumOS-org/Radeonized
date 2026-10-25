// 2r9: compare backing allocations for a buffer, plain Metal textures and an
// IOSurface texture. Pair with resourcewatch; no driver bytes are changed here.
// A coordinate-coded render + full readback checks that an RT still works under the
// existing system policy. This is NOT an oracle for the original text defect.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <IOSurface/IOSurface.h>
#import <objc/runtime.h>

// Read the driver's *own* image descriptor; do not infer a mode from size.
// The runtime type encoding names the eight WORDs and WORD3's bitfields:
// DST_SEL_[XYZW] (12), BASE_LEVEL (4), LAST_LEVEL (4), SW_MODE (5).
// Resolve the ivar offset at runtime and refuse a changed class/layout.
static void print_swizzle(id<MTLTexture> texture, const char *label) {
    Class cls = object_getClass(texture);
    Ivar iv = class_getInstanceVariable(cls, "_gfx10");
    const char *enc = iv ? ivar_getTypeEncoding(iv) : NULL;
    ptrdiff_t off = iv ? ivar_getOffset(iv) : -1;
    static const char prefix[] = "{GFX10_TextureMembersRec=\"texImgRsrcDesc\"";
    if (strcmp(class_getName(cls), "GFX10_MtlTexture") || !enc ||
        strncmp(enc, prefix, sizeof prefix - 1) ||
        !strstr(enc, "\"DST_SEL_X\"b3\"DST_SEL_Y\"b3\"DST_SEL_Z\"b3\"DST_SEL_W\"b3\"BASE_LEVEL\"b4\"LAST_LEVEL\"b4\"SW_MODE\"b5") ||
        off < 0 || (size_t)off + 32 > class_getInstanceSize(cls)) {
        fprintf(stderr, "texturepath: %s SW_MODE INCONCLUSIVE: layout guard (%s)\n", label, class_getName(cls));
        return;
    }
    uint32_t word3;
    memcpy(&word3, (const unsigned char *)(__bridge const void *)texture + off + 12, sizeof word3);
    fprintf(stderr, "texturepath: %s _gfx10=0x%tx WORD3=0x%08x SW_MODE=%u\n",
        label, off, word3, (word3 >> 20) & 31);
}

int main(int argc, const char *argv[]) {
    setvbuf(stdout, NULL, _IONBF, 0);
    NSUInteger w = 640, h = 480;
    if (argc != 1) {
        char *endw = NULL, *endh = NULL;
        if (argc == 3) { w = strtoul(argv[1], &endw, 10); h = strtoul(argv[2], &endh, 10); }
        if (argc != 3 || !endw || !endh || endw == argv[1] || endh == argv[2] ||
            *endw || *endh || w < 64 || w > 4096 || w % 64 || h < 1 || h > 4096) {
            fprintf(stderr, "usage: texturepath [width height] (width 64..4096, multiple of 64; height 1..4096)\n");
            return 2;
        }
    }
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        if (!dev) { fprintf(stderr, "texturepath: INCONCLUSIVE: no Metal device\n"); return 2; }
        fprintf(stderr, "texturepath: device=%s\n", dev.name.UTF8String);
        const NSUInteger row = w * 4;
        fprintf(stderr, "texturepath: geometry=%lux%lu\n", (unsigned long)w, (unsigned long)h);
        fprintf(stderr, "texturepath: BEGIN shared buffer control\n");
        id<MTLBuffer> control = [dev newBufferWithLength:row*h options:MTLResourceStorageModeShared];
        fprintf(stderr, "texturepath: END shared buffer control %s\n", control ? "ok" : "FAILED");
        if (!control) return 1;

        MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:w height:h mipmapped:NO];
        d.storageMode = MTLStorageModePrivate;
        d.usage = MTLTextureUsageShaderRead;
        fprintf(stderr, "texturepath: BEGIN plain shader-read texture\n");
        id<MTLTexture> sampled = [dev newTextureWithDescriptor:d];
        fprintf(stderr, "texturepath: END plain shader-read texture %s\n", sampled ? "ok" : "FAILED");
        if (!sampled) return 1;
        print_swizzle(sampled, "shader-read");

        d.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
        fprintf(stderr, "texturepath: BEGIN plain render-target texture\n");
        id<MTLTexture> rt = [dev newTextureWithDescriptor:d];
        fprintf(stderr, "texturepath: END plain render-target texture %s allocatedSize=%lu\n", rt ? "ok" : "FAILED", (unsigned long)rt.allocatedSize);
        if (!rt) return 1;
        print_swizzle(rt, "plain RT");

        fprintf(stderr, "texturepath: BEGIN IOSurface render-target texture\n");
        IOSurfaceRef surface = IOSurfaceCreate((__bridge CFDictionaryRef)@{
            (id)kIOSurfaceWidth:@(w), (id)kIOSurfaceHeight:@(h),
            (id)kIOSurfaceBytesPerElement:@4, (id)kIOSurfaceBytesPerRow:@(row),
            (id)kIOSurfacePixelFormat:@((uint32_t)0x42475241),
        });
        if (!surface) { fprintf(stderr, "texturepath: IOSurfaceCreate FAILED\n"); return 1; }
        d.storageMode = MTLStorageModeShared;
        id<MTLTexture> ios = [dev newTextureWithDescriptor:d iosurface:surface plane:0];
        fprintf(stderr, "texturepath: END IOSurface render-target texture %s\n", ios ? "ok" : "FAILED");
        CFRelease(surface);
        if (!ios) return 1;
        print_swizzle(ios, "IOSurface RT");

        fprintf(stderr, "texturepath: BEGIN patterned render and readback\n");
        // A uniform clear cannot expose a permutation of tiles. Encode position
        // in all three channels, then compare EVERY pixel after the GPU blit.
        NSString *source = @"#include <metal_stdlib>\nusing namespace metal;\n"
            "vertex float4 v(uint i [[vertex_id]]) { const float2 p[3] = {float2(-1,-1),float2(3,-1),float2(-1,3)}; return float4(p[i],0,1); }\n"
            "fragment float4 f(float4 p [[position]]) { uint2 q=uint2(p.xy); return float4(float((q.x*5+q.y*11)&255)/255.0f, float((q.x^q.y)&255)/255.0f, float(((q.x>>8)*29+(q.y>>8)*71)&255)/255.0f, 1); }\n";
        NSError *error = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:source options:nil error:&error];
        if (!lib) { fprintf(stderr, "texturepath: shader FAILED: %s\n", error.description.UTF8String); return 1; }
        MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
        pd.vertexFunction = [lib newFunctionWithName:@"v"];
        pd.fragmentFunction = [lib newFunctionWithName:@"f"];
        pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        id<MTLRenderPipelineState> pipeline = [dev newRenderPipelineStateWithDescriptor:pd error:&error];
        if (!pipeline) { fprintf(stderr, "texturepath: pipeline FAILED: %s\n", error.description.UTF8String); return 1; }
        id<MTLCommandQueue> queue = [dev newCommandQueue];
        id<MTLCommandBuffer> cb = [queue commandBuffer];
        if (!cb) { fprintf(stderr, "texturepath: command buffer FAILED\n"); return 1; }
        MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = rt;
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        pass.colorAttachments[0].clearColor = MTLClearColorMake(.25, .5, .75, 1);
        id<MTLRenderCommandEncoder> render = [cb renderCommandEncoderWithDescriptor:pass];
        if (!render) { fprintf(stderr, "texturepath: render encoder FAILED\n"); return 1; }
        [render setRenderPipelineState:pipeline];
        [render drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [render endEncoding];
        id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
        if (!blit) { fprintf(stderr, "texturepath: blit encoder FAILED\n"); return 1; }
        [blit copyFromTexture:rt sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(w,h,1) toBuffer:control destinationOffset:0 destinationBytesPerRow:row destinationBytesPerImage:row*h];
        [blit endEncoding]; [cb commit]; [cb waitUntilCompleted];
        if (cb.status != MTLCommandBufferStatusCompleted) {
            fprintf(stderr, "texturepath: GPU FAILED: %s\n", cb.error.description.UTF8String); return 1;
        }
        const unsigned char *pixels = control.contents;
        NSUInteger bad = 0;
        for (NSUInteger i=0; i<w*h; i++) {
            NSUInteger x = i % w, y = i / w;
            const int expected[4] = {
                (int)(((x >> 8)*29 + (y >> 8)*71) & 255),
                (int)((x ^ y) & 255), (int)((x*5 + y*11) & 255), 255
            };
            for (unsigned c=0; c<4; c++) {
                if (abs((int)pixels[4*i+c] - expected[c]) > 1) { bad++; break; }
            }
        }
        fprintf(stderr, "texturepath: END patterned render pixels=%lu mismatches=%lu %s\n",
            (unsigned long)(w*h), (unsigned long)bad, bad ? "FAIL" : "PASS");
        return bad ? 1 : 0;
    }
}
