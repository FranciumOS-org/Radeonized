// Three client-safe SkyLight queries, arity read from their disassembly first:
//   SLSServerMightHaveAcceleration()            0 args, bool in al
//   SLDisplayUsesOpenGLAcceleration(displayID)  1 arg, attribute 0x10
//   SLSDisplayCopyCurrentMetalDevice(displayID) 1 arg; its FIRST call is
//       SLSAcceleratorForDisplayNumber (0x7ff807e0c67c), so a device coming
//       back here is the accel-id -> MTLDevice pairing, end to end.
#import <Metal/Metal.h>
#import <ApplicationServices/ApplicationServices.h>
#import <dlfcn.h>
#import <stdio.h>
int main(void) {
    void *sl = dlopen("/System/Library/PrivateFrameworks/SkyLight.framework/SkyLight", RTLD_LAZY);
    uint32_t d = CGMainDisplayID();
    int  (*mightHave)(void)        = dlsym(sl, "SLSServerMightHaveAcceleration");
    int  (*usesGL)(uint32_t)       = dlsym(sl, "SLDisplayUsesOpenGLAcceleration");
    id   (*copyMTL)(uint32_t)      = dlsym(sl, "SLSDisplayCopyCurrentMetalDevice");
    printf("SLSServerMightHaveAcceleration()           = %d\n",
           mightHave ? mightHave() : -1);
    printf("SLDisplayUsesOpenGLAcceleration(%u)  = %d\n", d,
           usesGL ? usesGL(d) : -1);
    if (copyMTL) {
        id<MTLDevice> m = copyMTL(d);
        printf("SLSDisplayCopyCurrentMetalDevice(%u) = %s\n", d,
               m ? [[m name] UTF8String] : "NULL");
        if (m) printf("   regID=0x%llx\n", (unsigned long long)[m registryID]);
    }
    return 0;
}
