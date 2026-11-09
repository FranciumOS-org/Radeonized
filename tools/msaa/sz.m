#import <Metal/Metal.h>
#import <stdio.h>
int main(void){ @autoreleasepool {
  id<MTLDevice> D = MTLCreateSystemDefaultDevice();
  struct { const char*tag; MTLTextureType tt; MTLPixelFormat pf; int sc; } c[] = {
    {"s1 colour BGRA8  (control, allocates)", MTLTextureType2D,            MTLPixelFormatBGRA8Unorm,  1},
    {"s4 colour BGRA8  (THE FAILURE)       ", MTLTextureType2DMultisample, MTLPixelFormatBGRA8Unorm,  4},
    {"s2 colour BGRA8                      ", MTLTextureType2DMultisample, MTLPixelFormatBGRA8Unorm,  2},
    {"s1 depth  D32    (control)           ", MTLTextureType2D,            MTLPixelFormatDepth32Float,1},
    {"s4 depth  D32    (allocates!)        ", MTLTextureType2DMultisample, MTLPixelFormatDepth32Float,4},
  };
  for (int i=0;i<5;i++){
    MTLTextureDescriptor *d=[MTLTextureDescriptor new];
    d.textureType=c[i].tt; d.pixelFormat=c[i].pf; d.width=64; d.height=64;
    d.sampleCount=c[i].sc; d.usage=MTLTextureUsageRenderTarget;
    d.storageMode=MTLStorageModePrivate; d.mipmapLevelCount=1;
    MTLSizeAndAlign sa=[D heapTextureSizeAndAlignWithDescriptor:d];
    id<MTLTexture> t=[D newTextureWithDescriptor:d];
    printf("%s  size %-10llu align %-8llu  alloc %s\n", c[i].tag,
      (unsigned long long)sa.size,(unsigned long long)sa.align, t?"OK":"NULL");
  }
  printf("\nmaxBufferLength      %llu MB\n", (unsigned long long)[D maxBufferLength]>>20);
  printf("recommendedWorkingSet %llu MB\n", (unsigned long long)[D recommendedMaxWorkingSetSize]>>20);
  printf("currentAllocatedSize  %llu MB\n", (unsigned long long)[D currentAllocatedSize]>>20);
  return 0; } }
