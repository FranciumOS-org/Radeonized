#import <Metal/Metal.h>
#import <stdio.h>
int main(void) {
  @autoreleasepool {
    id<MTLDevice> d = MTLCreateSystemDefaultDevice();
    if (!d) { printf("no Metal device\n"); return 1; }
    printf("device: %s\n", [[d name] UTF8String]);
    printf("lowPower %d  removable %d  unified %d\n",
           (int)[d isLowPower], (int)[d isRemovable], (int)[d hasUnifiedMemory]);
    for (int n = 1; n <= 16; n <<= 1)
      printf("supportsTextureSampleCount:%-2d  %s\n", n,
             [d supportsTextureSampleCount:n] ? "YES" : "no");
    /* actually allocate a 4x MSAA colour target and resolve it */
    for (int n = 2; n <= 8; n <<= 1) {
      MTLTextureDescriptor *md = [MTLTextureDescriptor new];
      md.textureType = MTLTextureType2DMultisample;
      md.pixelFormat = MTLPixelFormatBGRA8Unorm;
      md.width = 64; md.height = 64; md.sampleCount = n;
      md.usage = MTLTextureUsageRenderTarget;
      md.storageMode = MTLStorageModePrivate;
      id<MTLTexture> ms = [d newTextureWithDescriptor:md];
      MTLTextureDescriptor *rd =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                              width:64 height:64 mipmapped:NO];
      rd.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
      rd.storageMode = MTLStorageModeShared;
      id<MTLTexture> rv = [d newTextureWithDescriptor:rd];
      printf("sampleCount %d: msaa tex %s  resolve tex %s",
             n, ms ? "ok" : "NULL", rv ? "ok" : "NULL");
      if (!ms || !rv) { printf("\n"); continue; }
      MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
      rp.colorAttachments[0].texture = ms;
      rp.colorAttachments[0].resolveTexture = rv;
      rp.colorAttachments[0].loadAction = MTLLoadActionClear;
      rp.colorAttachments[0].storeAction = MTLStoreActionMultisampleResolve;
      rp.colorAttachments[0].clearColor = MTLClearColorMake(0.2, 0.6, 1.0, 1.0);
      id<MTLCommandQueue> q = [d newCommandQueue];
      id<MTLCommandBuffer> cb = [q commandBuffer];
      id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
      [e endEncoding];
      [cb commit]; [cb waitUntilCompleted];
      printf("  status %ld", (long)[cb status]);
      if ([cb error]) printf("  ERROR %s", [[[cb error] localizedDescription] UTF8String]);
      uint8_t px[4] = {0,0,0,0};
      [rv getBytes:px bytesPerRow:4 fromRegion:MTLRegionMake2D(0,0,1,1) mipmapLevel:0];
      printf("  resolved BGRA [%d,%d,%d,%d] expect ~[255,153,51,255]\n",
             px[0],px[1],px[2],px[3]);
    }
  }
  return 0;
}
