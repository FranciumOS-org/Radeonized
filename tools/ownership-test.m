// A regression test for the 2mr crash: the interposers must preserve Metal's
// +1 ownership. MTLCreateSystemDefaultDevice and MTLCopyAllDevices are declared
// NS_RETURNS_RETAINED; a replacement that is not will have a release inserted on
// the way out while the caller still owns a reference, and the process dies in
// objc_release at the next autorelease-pool pop.
//
// This is the test that should have existed before anyone was asked to launch an
// application with the dylib inserted.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

int main(void)
{
    for (int i = 0; i < 200; i++) {
        @autoreleasepool {
            id<MTLDevice> d = MTLCreateSystemDefaultDevice();
            NSArray<id<MTLDevice>> *all = MTLCopyAllDevices();
            if (!d) { fprintf(stderr, "no device\n"); return 2; }
            (void)all.count;
            // Touch the swizzled path too, at a geometry the law flags.
            MTLTextureDescriptor *td =
                [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                   width:256 height:48 mipmapped:NO];
            td.storageMode = MTLStorageModeManaged;
            td.usage = MTLTextureUsageShaderRead;
            id<MTLTexture> t = [d newTextureWithDescriptor:td];
            (void)t;
        }
    }
    printf("ownership-test: 200 iterations, no crash\n");
    return 0;
}
