// cdchain (2qc): walk CoreDisplay's CGXDisplayDevice list in THIS process and
// ask the exported ops-table thunks what they answer for each device.
//
// 2qa closed branch 5 of the compositor decision ("the accelerator id is 0")
// from WindowServer's os_state dump.  That is a snapshot of a FIELD; this
// CALLS the function the decision calls.  Both thunks are exported, and their
// arity was read from their disassembly before being called (2pz's practice):
//
//   CGXNextDisplayDevice(dev)                 1 arg.  dev==0 -> list head,
//     else returns *dev, so dev->0x00 is `next`.   (unslid 0x7ff8041229ef)
//   CGXAcceleratorForDisplayDevice(dev,w,out) 3 args: ops = dev->0x8, tail
//     jump ops->0x148; dev==0 returns 0x3e9.      (unslid 0x7ff8041291df)
//   CGXServiceForDisplayDevice(dev,w,out)     3 args, ops->0x140.
//
// SLOT 0x140 vs 0x148 MATTERS AND 2qa GOT IT WRONG: `UpdateCGXDisplayDevice`
// calls ops->**0x140**, which the thunk table says is
// CGXServiceForDisplayDevice - NOT the accelerator.  So
//   displayPipeIsAvailable = FindDisplayPipeForService(SERVICE) & 1
// which is what the function's name said all along.  2qa inferred that slot
// from the argument shape (dev, -1, &out) instead of reading the slot.
//
// Field offsets below are Apple's own, decoded in 2q3 from
// CoreDisplay::CreateDisplayDeviceDict: isAcceleratorBacked is bit 22 of
// +0x64, displayPipeIsAvailable is the byte at +0x14c, numDisplayBuffers is
// the int at +0x48, and the MPPriv is +0x160.
//
// LIMIT, stated before running: the device list lives in a CoreDisplay global
// that only WindowServer populates, so an empty list here is EXPECTED and is
// reported as INCONCLUSIVE, not as "no displays".  A populated list would be
// this process's own view.  Calling an ops thunk whose implementation wants
// server state can crash this probe; that costs one process and is itself a
// result.
//
// Exit: 0 walked at least one device, 3 inconclusive.

#import <Foundation/Foundation.h>
#import <dlfcn.h>

typedef void *(*next_fn)(void *);
typedef int   (*q_fn)(void *, int, unsigned int *);

int main(void) {
    @autoreleasepool {
        setvbuf(stdout, NULL, _IONBF, 0);   // a segfault must not eat the output
        printf("== preconditions\n");
        void *h = dlopen("/System/Library/Frameworks/CoreDisplay.framework"
                         "/CoreDisplay", RTLD_LAZY);
        printf("CoreDisplay                  : %s\n", h ? "loaded" : dlerror());
        if (!h) return 3;
        next_fn nextdev = (next_fn)dlsym(RTLD_DEFAULT, "CGXNextDisplayDevice");
        q_fn    accel   = (q_fn)dlsym(RTLD_DEFAULT,
                                      "CGXAcceleratorForDisplayDevice");
        q_fn    svc     = (q_fn)dlsym(RTLD_DEFAULT,
                                      "CGXServiceForDisplayDevice");
        printf("CGXNextDisplayDevice         : %p\n", (void *)nextdev);
        printf("CGXAcceleratorForDisplayDevice: %p\n", (void *)accel);
        printf("CGXServiceForDisplayDevice   : %p\n", (void *)svc);
        if (!nextdev || !accel || !svc) {
            printf("INCONCLUSIVE: a thunk is missing.\n");
            return 3;
        }

        // MEASURED (2qc): CGXNextDisplayDevice(0) does
        //     callq *[0x7ff843bb2280]
        // on an indirect global, and in a client process that global is NULL,
        // so the call segfaults at address 0 before printing anything (the
        // first version of this tool did exactly that: SIGSEGV, zero bytes of
        // output, because stdout to a file is fully buffered). Rule 14: a
        // probe must SAY something on every path, so read the pointer first.
        // The slide is measured here, not assumed (2ps).
        const unsigned long long NEXTDEV_UNSLID = 0x7ff8041229efULL;
        const unsigned long long HOOK_UNSLID    = 0x7ff843bb2280ULL;
        unsigned long long slide =
            (unsigned long long)(uintptr_t)nextdev - NEXTDEV_UNSLID;
        void **hook = (void **)(uintptr_t)(HOOK_UNSLID + slide);
        printf("\ncache slide (measured)       : 0x%llx\n", slide);
        printf("CoreDisplay server hook @%p  : %p\n", (void *)hook, *hook);
        if (*hook == NULL) {
            printf("\nINCONCLUSIVE, and this is the EXPECTED answer.\n");
            printf("CGXNextDisplayDevice(0) calls through that pointer, and "
                   "only\nWindowServer's CoreDisplay initialisation fills it. "
                   "It is NULL here, so\nthe display-device list is not "
                   "reachable from a client process at all -\nnot empty, "
                   "UNREACHABLE. Calling it would segfault, which is what the\n"
                   "first run of this tool did.\n\n");
            printf("So branch 5 of the compositor decision (2qa) cannot be "
                   "re-measured by\nCALLING the function from here. The "
                   "os_state dump stays the evidence,\nand it is sound: 2qa "
                   "established CreateDisplayDeviceDict reads the MPPriv\n"
                   "from dev->0x160, the same field MPDisplayDevicePriv(dev,-1) "
                   "returns.\n");
            return 3;
        }

        printf("\n== the CGXDisplayDevice list in THIS process\n");
        int n = 0;
        for (void *dev = nextdev(NULL); dev && n < 8;
             dev = nextdev(dev), n++) {
            printf("device %d @ %p\n", n, dev);
            unsigned char *b = (unsigned char *)dev;

            unsigned int a = 0, s = 0;
            int ra = accel(dev, -1, &a);
            int rs = svc(dev, -1, &s);
            printf("  CGXAcceleratorForDisplayDevice(dev,-1,&out) rc=%d out=%u"
                   " (0x%x)%s\n", ra, a, a,
                   a == 0 ? "   <-- ZERO: this alone sends the display to "
                            "CompositorSW" : "");
            printf("  CGXServiceForDisplayDevice(dev,-1,&out)     rc=%d out=%u"
                   " (0x%x)%s\n", rs, s, s,
                   s == 0 ? "   <-- ZERO: UpdateCGXDisplayDevice then SKIPS "
                            "the displayPipeIsAvailable store" : "");

            unsigned int f64 = *(unsigned int *)(b + 0x64);
            printf("  dev->0x64 = 0x%08x   isAcceleratorBacked(bit22)=%d "
                   "isPageFlipped(bit23)=%d\n", f64,
                   (f64 >> 22) & 1, (f64 >> 23) & 1);
            printf("  dev->0x48 numDisplayBuffers      = %u\n",
                   *(unsigned int *)(b + 0x48));
            printf("  dev->0x14c displayPipeIsAvailable = %u\n",
                   (unsigned)*(unsigned char *)(b + 0x14c));
            void *priv = *(void **)(b + 0x160);
            printf("  dev->0x160 MPPriv                = %p\n", priv);
            if (priv) {
                unsigned char *p = (unsigned char *)priv;
                printf("    priv->0x58 service     = 0x%x\n",
                       *(unsigned int *)(p + 0x58));
                printf("    priv->0x5c accelerator = 0x%x\n",
                       *(unsigned int *)(p + 0x5c));
                printf("    priv->0xb0             = 0x%08x  (bit22=%d - the "
                       "bit -rbgpufbaccel sets)\n",
                       *(unsigned int *)(p + 0xb0),
                       (*(unsigned int *)(p + 0xb0) >> 22) & 1);
            }
        }
        if (n == 0) {
            printf("the list is EMPTY.\n");
            printf("INCONCLUSIVE, and expected: CGXNextDisplayDevice(0) reads "
                   "a CoreDisplay\nglobal that only WindowServer populates. "
                   "This says nothing about the\ndisplay's real accelerator "
                   "id - it says this probe cannot see it from a\nclient "
                   "process, which is exactly the limit stated in the header.\n");
            return 3;
        }
        printf("\nwalked %d device(s).\n", n);
        return 0;
    }
}
