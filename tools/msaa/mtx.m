#import <Metal/Metal.h>
#import <stdio.h>
static id<MTLDevice> D;
static void try1(const char *tag, MTLTextureType tt, MTLPixelFormat pf,
                 int w, int h, int sc, MTLTextureUsage u, MTLStorageMode sm) {
  MTLTextureDescriptor *d = [MTLTextureDescriptor new];
  d.textureType = tt; d.pixelFormat = pf;
  d.width = w; d.height = h; d.sampleCount = sc;
  d.usage = u; d.storageMode = sm; d.mipmapLevelCount = 1;
  id<MTLTexture> t = [D newTextureWithDescriptor:d];
  printf("%-46s -> %s\n", tag, t ? "OK" : "NULL");
}
int main(void) { @autoreleasepool {
  D = MTLCreateSystemDefaultDevice();
  printf("device %s\n\n", [[D name] UTF8String]);
  printf("-- controls: single-sample, same shapes --\n");
  try1("2D       BGRA8 64x64  s1 RT private", MTLTextureType2D, MTLPixelFormatBGRA8Unorm, 64,64,1, MTLTextureUsageRenderTarget, MTLStorageModePrivate);
  try1("2D       BGRA8 4096x4096 s1 RT private", MTLTextureType2D, MTLPixelFormatBGRA8Unorm, 4096,4096,1, MTLTextureUsageRenderTarget, MTLStorageModePrivate);
  try1("2D       D32   64x64  s1 RT private", MTLTextureType2D, MTLPixelFormatDepth32Float, 64,64,1, MTLTextureUsageRenderTarget, MTLStorageModePrivate);
  printf("\n-- the subject: multisample, varying one thing at a time --\n");
  try1("2DMS     BGRA8 64x64  s4 RT private", MTLTextureType2DMultisample, MTLPixelFormatBGRA8Unorm, 64,64,4, MTLTextureUsageRenderTarget, MTLStorageModePrivate);
  try1("2DMS     BGRA8 64x64  s2 RT private", MTLTextureType2DMultisample, MTLPixelFormatBGRA8Unorm, 64,64,2, MTLTextureUsageRenderTarget, MTLStorageModePrivate);
  try1("2DMS     RGBA8 64x64  s4 RT private", MTLTextureType2DMultisample, MTLPixelFormatRGBA8Unorm, 64,64,4, MTLTextureUsageRenderTarget, MTLStorageModePrivate);
  try1("2DMS     R8    64x64  s4 RT private", MTLTextureType2DMultisample, MTLPixelFormatR8Unorm, 64,64,4, MTLTextureUsageRenderTarget, MTLStorageModePrivate);
  try1("2DMS     RGBA16F 64x64 s4 RT private", MTLTextureType2DMultisample, MTLPixelFormatRGBA16Float, 64,64,4, MTLTextureUsageRenderTarget, MTLStorageModePrivate);
  try1("2DMS     D32   64x64  s4 RT private", MTLTextureType2DMultisample, MTLPixelFormatDepth32Float, 64,64,4, MTLTextureUsageRenderTarget, MTLStorageModePrivate);
  try1("2DMS     BGRA8 64x64  s4 RT+READ private", MTLTextureType2DMultisample, MTLPixelFormatBGRA8Unorm, 64,64,4, MTLTextureUsageRenderTarget|MTLTextureUsageShaderRead, MTLStorageModePrivate);
  try1("2DMS     BGRA8 16x16  s4 RT private", MTLTextureType2DMultisample, MTLPixelFormatBGRA8Unorm, 16,16,4, MTLTextureUsageRenderTarget, MTLStorageModePrivate);
  try1("2DMSArr  BGRA8 64x64  s4 RT private", MTLTextureType2DMultisampleArray, MTLPixelFormatBGRA8Unorm, 64,64,4, MTLTextureUsageRenderTarget, MTLStorageModePrivate);
  try1("2DMS     D16   64x64  s4 RT private", MTLTextureType2DMultisample, MTLPixelFormatDepth16Unorm, 64,64,4, MTLTextureUsageRenderTarget, MTLStorageModePrivate);
  try1("2DMS     D32S8 64x64  s4 RT private", MTLTextureType2DMultisample, MTLPixelFormatDepth32Float_Stencil8, 64,64,4, MTLTextureUsageRenderTarget, MTLStorageModePrivate);
  try1("2DMS     S8    64x64  s4 RT private", MTLTextureType2DMultisample, MTLPixelFormatStencil8, 64,64,4, MTLTextureUsageRenderTarget, MTLStorageModePrivate);
  try1("2DMS     BGRA8 64x64  s4 RT shared ", MTLTextureType2DMultisample, MTLPixelFormatBGRA8Unorm, 64,64,4, MTLTextureUsageRenderTarget, MTLStorageModeShared);
  try1("2DMS     BGRA8 64x64  s4 RT managed", MTLTextureType2DMultisample, MTLPixelFormatBGRA8Unorm, 64,64,4, MTLTextureUsageRenderTarget, MTLStorageModeManaged);
  printf("\n-- heap path: does the device even size it? --\n");
  MTLTextureDescriptor *d = [MTLTextureDescriptor new];
  d.textureType = MTLTextureType2DMultisample; d.pixelFormat = MTLPixelFormatBGRA8Unorm;
  d.width = 64; d.height = 64; d.sampleCount = 4;
  d.usage = MTLTextureUsageRenderTarget; d.storageMode = MTLStorageModePrivate;
  MTLSizeAndAlign sa = [D heapTextureSizeAndAlignWithDescriptor:d];
  printf("heapTextureSizeAndAlign (s4 MSAA): size %llu align %llu\n",
         (unsigned long long)sa.size, (unsigned long long)sa.align);
  d.sampleCount = 1; d.textureType = MTLTextureType2D;
  sa = [D heapTextureSizeAndAlignWithDescriptor:d];
  printf("heapTextureSizeAndAlign (s1 colour ctrl): size %llu align %llu\n",
         (unsigned long long)sa.size, (unsigned long long)sa.align);
  d.sampleCount = 4; d.textureType = MTLTextureType2DMultisample;
  d.pixelFormat = MTLPixelFormatDepth32Float;
  sa = [D heapTextureSizeAndAlignWithDescriptor:d];
  printf("heapTextureSizeAndAlign (s4 DEPTH, the one that WORKS): size %llu align %llu\n",
         (unsigned long long)sa.size, (unsigned long long)sa.align);
  printf("\nmaxBufferLength %llu  recommendedWorkingSetSize %llu  currentAllocated %llu\n",
         (unsigned long long)[D maxBufferLength],
         (unsigned long long)[D recommendedMaxWorkingSetSize],
         (unsigned long long)[D currentAllocatedSize]);
  return 0; } }
