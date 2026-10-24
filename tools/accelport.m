// accelport (2py): the ORACLE for "why does the desktop composite in
// software?" - it computes, in userspace, the exact number macOS uses to
// decide.
//
// The chain, read out of the dyld shared cache (2pu-2py):
//
//   SkyLight  WSGetCompositorForAccelerator(id)   id == 0 -> CompositorSW
//   CoreDisplay  MPAcceleratorForDisplayDevice    *out = mpPriv->accelerator
//   CoreDisplay  Framebuffer::InitializeMPPrivAccelerator
//                                                 mpPriv->accelerator = fb->0x18
//   CoreDisplay  GPU::UpdateAcceleratorForMetal   fb->0x18 = FindAccelerator...()
//   CoreDisplay  FindAcceleratorWithMTLDevice     <- reproduced below
//
// FindAcceleratorWithMTLDevice (unslid 0x7ff80412f8ea) is:
//
//   devices = MTLCopyAllDevices(); if ([devices count] == 0) return 0;
//   pass 1: first d with  [d isLowPower] && ![d isRemovable] -> [d acceleratorPort]
//   pass 2: first d with !([d isRemovable])                  -> [d acceleratorPort]
//   neither -> 0
//
// `acceleratorPort` is a private MTLDevice selector, so it is read through
// respondsToSelector and reported as ABSENT rather than guessed at.
//
// RULE 14/26: a zero here means nothing on its own. An unarmed boot has no
// accelerator at all, so Metal has no device and every number is 0 for a
// reason that has nothing to do with compositing. The program therefore
// prints its own precondition first and refuses to draw a conclusion when it
// does not hold.
#import <Metal/Metal.h>
#import <IOKit/IOKitLib.h>
#import <objc/message.h>
#import <stdio.h>

#define PORT_ABSENT 0xffffffffu

static unsigned accelerator_port(id d) {
    SEL s = sel_getUid("acceleratorPort");
    if (![d respondsToSelector:s]) return PORT_ABSENT;
    return ((unsigned (*)(id, SEL))objc_msgSend)(d, s);
}

// How many IOAccelerator services exist at all. Zero means this boot never
// started the accelerator, which is the precondition, not the finding.
static int accelerator_services(void) {
    io_iterator_t it = IO_OBJECT_NULL;
    if (IOServiceGetMatchingServices(kIOMainPortDefault,
                                     IOServiceMatching("IOAccelerator"),
                                     &it) != KERN_SUCCESS)
        return -1;
    int n = 0;
    for (io_object_t o; (o = IOIteratorNext(it)); IOObjectRelease(o)) n++;
    IOObjectRelease(it);
    return n;
}

int main(void) {
    int services = accelerator_services();
    printf("precondition: IOAccelerator services in this boot = %d\n", services);

    NSArray<id<MTLDevice>> *ds = MTLCopyAllDevices();
    printf("MTLCopyAllDevices()            count = %lu\n", (unsigned long)ds.count);
    id<MTLDevice> dflt = MTLCreateSystemDefaultDevice();
    printf("MTLCreateSystemDefaultDevice() -> %s\n", dflt ? "device" : "NULL");

    for (id<MTLDevice> d in ds) {
        unsigned p = accelerator_port(d);
        printf("  %-24s lowPower=%d removable=%d headless=%d regID=0x%llx\n",
               d.name.UTF8String, (int)d.isLowPower, (int)d.isRemovable,
               (int)d.isHeadless, (unsigned long long)d.registryID);
        if (p == PORT_ABSENT) printf("    acceleratorPort: selector ABSENT\n");
        else                  printf("    acceleratorPort = %u (0x%x)\n", p, p);
    }

    unsigned chosen = 0;
    const char *how = "neither pass matched - returns 0";
    for (id<MTLDevice> d in ds)
        if (d.isLowPower && !d.isRemovable) {
            chosen = accelerator_port(d); how = "pass 1: integrated, not removable"; goto done;
        }
    for (id<MTLDevice> d in ds)
        if (!d.isRemovable) {
            chosen = accelerator_port(d); how = "pass 2: first not removable"; goto done;
        }
done:
    if (chosen == PORT_ABSENT) {
        printf("FindAcceleratorWithMTLDevice(): a device matched [%s] but "
               "acceleratorPort is absent - cannot compute\n", how);
        return 2;
    }
    printf("FindAcceleratorWithMTLDevice() = %u (0x%x)  [%s]\n", chosen, chosen, how);

    if (services <= 0) {
        printf("INCONCLUSIVE: this boot has no accelerator service, so 0 here "
               "says nothing about compositing. Re-run in an armed boot.\n");
        return 3;
    }
    /* 2q8: this used to print "so SkyLight composites with CompositorMetal"
     * on a non-zero id.  That was 2pv's inference and it is MEASURED FALSE:
     * boot 148 reads priv.accelerator = 0x10723 with a live Metal device and
     * the display composite is still CompositorSW (2pz, and the submission
     * counter in tools/gpusubs.sh).  A non-zero id only clears ONE gate. */
    printf("=> mpPriv->accelerator = %u. Non-zero clears the accelId==0 gate "
           "ONLY;\n   it does NOT establish which compositor serves the "
           "display (2pz/2q8).\n   For that: tools/gpusubs.sh, or "
           "sudo tools/compositor_check.sh.\n", chosen);
    return chosen ? 0 : 1;
}
