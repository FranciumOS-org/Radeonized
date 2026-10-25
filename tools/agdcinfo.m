// agdcinfo (2r5): the ORACLE for the LAST link in the display chain.
//
// IOPresentment's association map is built by ioPresentmentFindAGDCMatch,
// inlined into ioPresentmentCreateAssociationMap (0x7ff812e8a730). Read from
// the cache, every stub resolved by chained-pointer decode + atos, it is:
//
//   m    = IOServiceMatching("AppleGraphicsDeviceControl")   // 0x7ff812e8ac33
//   IOServiceGetMatchingServices(port, m, &it)               // else "gms %#x"
//   IOIteratorIsValid(it) == 1                               // else bail
//   while ((svc = IOIteratorNext(it))) {
//       IORegistryEntryGetName(svc, name);
//       if (!strcmp(name, "AppleGPUWrangler")) { skip; }     // 0x7ff812e8ada1
//       IORegistryEntryGetRegistryEntryID(svc, &rid);
//       IOServiceOpen(svc, mach_task_self_, 2, &conn);       // TYPE 2
//       IOConnectCallMethod(conn, 1, 0,0,0,0, out, &outLen); // kAGDCVendorInfo
//       if (rc || outLen == 0) goto err;                     // 0x7ff812e8ae5e/68
//       if ((unsigned)out[0x28] - 1 > 1) skip;               // must be 1 or 2
//       record (svc, rid) in the map;                        // stride 0x30
//   }
//
// The name test is an EXACT 17-byte compare, done as three xors against
// "AppleGPU", "Wrangler" and a NUL - not a prefix and not a class test.
//
// Selector 1 is kAGDCVendorInfo and selector 0x980 is kAGDCGPUCapability:
// not inferred from 2qm's reading of IOPresentment's string table, but from
// the os_log format strings on those two calls' own paths
// ('kAGDCVendorInfo %#llx %#x %#x' at 0x7ff812ea15ca, 'kAGDCGPUCapability
// %#llx %zu %u' at 0x7ff812ea1625), inside the second inlined query,
// ioPresentmentPopulateIOFramebufferEntries.
//
// WHAT THIS TOOL ANSWERS, in order:
//   1. How many services match "AppleGraphicsDeviceControl", and what each is
//      named - i.e. how many survive the name test.  Zero survivors means the
//      association map is EMPTY and nothing downstream can work.
//   2. Whether IOServiceOpen(MACH_PORT_NULL, ..., 2, ...) returns
//      0x10000003 = MACH_SEND_INVALID_DEST - the exact code boot 150 logged.
//      That is the control for "the map was empty" against "a user client
//      answered badly": two different repairs.
//   3. Whether the wrangler itself would answer selector 1 if it were not
//      skipped by name.  If it would, the gate is a STRING, not a driver.
//
// PRE-STATED (rule 14): unprivileged, 2qm measured the GPUWrangler open
// returning kIOReturnNotPermitted 0xe00002e2.  A NotPermitted here is
// INCONCLUSIVE about the user client and says nothing about question 3; run
// it again with sudo.  Questions 1 and 2 need no privilege at all.
#import <Foundation/Foundation.h>
#import <IOKit/IOKitLib.h>
#include <mach/message.h>
#include <stdio.h>

static const char *rcname(kern_return_t rc)
{
    switch ((unsigned)rc) {
    case 0:                      return "success";
    case 0x10000003u:            return "MACH_SEND_INVALID_DEST";
    case 0xe00002e2u:            return "kIOReturnNotPermitted";
    case 0xe00002beu:            return "kIOReturnNoResources";
    case 0xe00002c2u:            return "kIOReturnBadArgument";
    case 0xe00002c7u:            return "kIOReturnUnsupported";
    case 0xe00002f0u:            return "kIOReturnNoDevice";
    default:                     return "?";
    }
}

// The exact test at 0x7ff812e8ad6c-0x7ff812e8ada1, written the way the code
// does it rather than as strcmp, so a 17th byte cannot slip past.
static bool is_wrangler(const char *n)
{
    char b[64] = {0};
    memcpy(b, n, strnlen(n, sizeof(b) - 1));
    uint64_t lo, hi;
    memcpy(&lo, b, 8);
    memcpy(&hi, b + 8, 8);
    return (lo ^ 0x555047656c707041ull) == 0 &&
           (hi ^ 0x72656c676e617257ull) == 0 && b[16] == 0;
}

static void query(io_service_t svc, const char *label)
{
    io_connect_t conn = 0;
    kern_return_t rc = IOServiceOpen(svc, mach_task_self(), 2, &conn);
    printf("    IOServiceOpen(type 2)        = %#x (%s)\n",
           (unsigned)rc, rcname(rc));
    if (rc != 0)
        return;

    uint8_t out[0x2c];
    size_t outLen = sizeof(out);
    memset(out, 0, sizeof(out));
    rc = IOConnectCallMethod(conn, 1, NULL, 0, NULL, 0,
                             NULL, NULL, out, &outLen);
    printf("    selector 1 kAGDCVendorInfo   = %#x (%s), %zu bytes\n",
           (unsigned)rc, rcname(rc), outLen);
    if (rc == 0 && outLen) {
        uint32_t v28;
        memcpy(&v28, out + 0x28, 4);
        printf("      +0x28 = %u  (must be 1 or 2 to be recorded)\n", v28);
        printf("      bytes:");
        for (size_t i = 0; i < outLen && i < sizeof(out); i++)
            printf(" %02x", out[i]);
        printf("\n");
    }

    uint8_t cap[0xdc];
    size_t capLen = sizeof(cap);
    memset(cap, 0, sizeof(cap));
    rc = IOConnectCallMethod(conn, 0x980, NULL, 0, NULL, 0,
                             NULL, NULL, cap, &capLen);
    printf("    selector 0x980 kAGDCGPUCapability = %#x (%s), %zu bytes\n",
           (unsigned)rc, rcname(rc), capLen);
    if (rc == 0 && capLen >= 0x34) {
        uint32_t n;
        memcpy(&n, cap + 0x30, 4);
        printf("      +0x30 count = %u  (must be non-zero)\n", n);
    }
    IOServiceClose(conn);
    (void)label;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);   // 2qc: a fault must not eat the log

    printf("== 1. IOPresentment's own query: "
           "IOServiceMatching(\"AppleGraphicsDeviceControl\") ==\n");
    CFMutableDictionaryRef m =
        IOServiceMatching("AppleGraphicsDeviceControl");
    if (!m) { printf("  ctm: matching dictionary is NULL\n"); return 3; }

    io_iterator_t it = 0;
    kern_return_t rc = IOServiceGetMatchingServices(kIOMainPortDefault, m, &it);
    if (rc) { printf("  gms %#x\n", (unsigned)rc); return 3; }
    printf("  IOIteratorIsValid = %d (IOPresentment requires 1)\n",
           IOIteratorIsValid(it));

    int total = 0, skipped = 0, candidates = 0;
    io_service_t svc;
    while ((svc = IOIteratorNext(it))) {
        io_name_t name = {0}, cls = {0};
        uint64_t rid = 0;
        IORegistryEntryGetName(svc, name);
        IOObjectGetClass(svc, cls);
        IORegistryEntryGetRegistryEntryID(svc, &rid);
        total++;
        bool skip = is_wrangler(name);
        printf("  [%d] name=\"%s\" class=%s registryID=%#llx -> %s\n",
               total, name, cls, rid,
               skip ? "SKIPPED by the name test" : "CANDIDATE");
        if (skip) {
            skipped++;
            // Question 3: it is skipped, but would it answer?
            printf("    (asking it anyway, to see whether the gate is the"
                   " NAME or the driver)\n");
            query(svc, name);
        } else {
            candidates++;
            query(svc, name);
        }
        IOObjectRelease(svc);
    }
    IOObjectRelease(it);
    printf("  matched %d, skipped %d, candidates %d\n",
           total, skipped, candidates);

    printf("\n== 2. control: IOServiceOpen(MACH_PORT_NULL, ..., 2) ==\n");
    io_connect_t c0 = 0;
    rc = IOServiceOpen(MACH_PORT_NULL, mach_task_self(), 2, &c0);
    printf("  = %#x (%s)   [boot 150 logged %#x]\n",
           (unsigned)rc, rcname(rc), 0x10000003u);

    printf("\n== VERDICT ==\n");
    if (candidates == 0) {
        printf("  The association map is EMPTY: every AGDC service is the\n"
               "  wrangler itself, which IOPresentment skips by name.\n");
        if ((unsigned)rc == 0x10000003u)
            printf("  And an open on the resulting service id 0 returns\n"
                   "  exactly the %#x boot 150 logged, so that log line is\n"
                   "  the EMPTY MAP and not a misbehaving user client.\n",
                   0x10000003u);
        else
            printf("  But the null-port control returned %#x, not %#x, so\n"
                   "  the boot-150 code is NOT explained by an empty map.\n",
                   (unsigned)rc, 0x10000003u);
        return 1;
    }
    printf("  %d candidate(s) survive the name test - the map is not empty,\n"
           "  so the failure is in the user client, not in the match.\n",
           candidates);
    return 0;
}
