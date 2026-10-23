/* SPDX-License-Identifier: MIT */
/*
 * vramtest (2ot): is this GPU actually limited to the "7 MB" that About This
 * Mac prints? No, and this is the falsifier.
 *
 * The 7 MB is 8294400 bytes = 1920 x 1080 x 4 = exactly ONE framebuffer. It
 * is IONDRVFramebuffer's own buffer size reported through IOFBMemorySize,
 * which System Profiler falls back to because no VRAM,totalMB property
 * exists (2or). It is not a memory pool and nothing is running out of it.
 *
 * Boot 133: recommendedMaxWorkingSetSize 4096 MB, the probe measured the
 * carve-out at 4096 MB, and this tool allocated and read back 1024 MB in
 * eight buffers plus a 8192x8192 BGRA8 texture. If the GPU really had 7 MB,
 * none of that would allocate.
 *
 * It allocates a gigabyte, so run it when the machine is idle.
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

int main(void) {
    @autoreleasepool {
        id<MTLDevice> d = MTLCreateSystemDefaultDevice();
        if (!d) { printf("no Metal device\n"); return 1; }
        printf("device: %s\n", d.name.UTF8String);
        printf("recommendedMaxWorkingSetSize = %llu MB\n", d.recommendedMaxWorkingSetSize >> 20);
        printf("currentAllocatedSize         = %lu MB\n", (unsigned long)(d.currentAllocatedSize >> 20));

        NSMutableArray *keep = [NSMutableArray array];
        size_t total = 0;
        for (int i = 0; i < 8; i++) {           // 8 x 128 MB = 1 GB
            const size_t sz = 128ull << 20;
            id<MTLBuffer> b = [d newBufferWithLength:sz options:MTLResourceStorageModeShared];
            if (!b) { printf("  allocation %d of 128 MB FAILED at %zu MB total\n", i, total >> 20); break; }
            // touch first and last page so it is really backed
            volatile uint32_t *p = (volatile uint32_t *)b.contents;
            p[0] = 0xA5A5A5A5u;
            p[(sz / 4) - 1] = 0x5A5A5A5Au;
            if (p[0] != 0xA5A5A5A5u || p[(sz / 4) - 1] != 0x5A5A5A5Au) {
                printf("  buffer %d did not read back\n", i); break;
            }
            [keep addObject:b];
            total += sz;
        }
        printf("allocated and verified: %zu MB in %lu buffers\n",
               total >> 20, (unsigned long)keep.count);
        printf("currentAllocatedSize now     = %lu MB\n", (unsigned long)(d.currentAllocatedSize >> 20));

        // one big texture too, since the defect is about textures
        MTLTextureDescriptor *td = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
            width:8192 height:8192 mipmapped:NO];
        id<MTLTexture> t = [d newTextureWithDescriptor:td];
        printf("8192x8192 BGRA8 texture (256 MB): %s\n", t ? "allocated" : "FAILED");
        return 0;
    }
}
