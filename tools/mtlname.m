/* SPDX-License-Identifier: MIT */
/* 2CB: what Metal calls this GPU, and whether SPDisplaysReporter can pair it.
 *
 * The reporter decides "Metal: 3" vs "Metal: Supported" by matching an
 * MTLDevice to a GPU dictionary BY NAME - four tests ORed, disassembled out
 * of SPDisplaysReporter.spreporter at 0xa50d-0xa64c. This runs those exact
 * four tests on the live strings, so the verdict is a measurement rather
 * than a reading of the outcome.
 *
 * Userspace and read-only: it changes nothing and can be run on any boot. */
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>
#import <IOKit/IOKitLib.h>

/* The family constants the reporter tests, in its order. 5002 is checked
 * first and 5001 second; anything else is reported as Metal 2. Written as
 * the raw numbers because that is what the disassembly shows. */
#define RB_FAMILY_METAL4 5002
#define RB_FAMILY_METAL3 5001

static NSString *prop(io_registry_entry_t e, NSString *k)
{
    CFTypeRef v = IORegistryEntryCreateCFProperty(e, (__bridge CFStringRef)k,
                                                  kCFAllocatorDefault, 0);
    if (!v) { return nil; }
    NSString *s = [[NSString alloc] initWithFormat:@"%@", (__bridge id)v];
    CFRelease(v);
    return s;
}

int main(void)
{
    @autoreleasepool {
        NSArray *devs = MTLCopyAllDevices();
        if (devs.count == 0) {
            fprintf(stderr, "no Metal devices\n");
            return 2;
        }
        for (id<MTLDevice> d in devs) {
            NSString *n = [d name];
            printf("Metal device\n");
            printf("  name                 \"%s\"\n", n.UTF8String);
            printf("  registryID           0x%llx  (NOT stable across boots)\n",
                   (unsigned long long)[d registryID]);
            printf("  supportsFamily 5002  %s\n",
                   [d supportsFamily:RB_FAMILY_METAL4] ? "YES" : "no");
            printf("  supportsFamily 5001  %s\n",
                   [d supportsFamily:RB_FAMILY_METAL3] ? "YES" : "no");
            printf("  would report         %s\n",
                   [d supportsFamily:RB_FAMILY_METAL4] ? "spdisplays_metal4" :
                   [d supportsFamily:RB_FAMILY_METAL3] ? "spdisplays_metal3" :
                                                         "spdisplays_metal2");

            /* The name is built as "AMD " + ATY,FamilyName + " " +
             * ATY,DeviceName, read ONE level up from the accelerator in the
             * IOService plane. Read the same two properties the same way, so
             * "the publish did not land" is distinguishable from "it landed
             * and Metal ignored it". */
            io_registry_entry_t accel = (io_registry_entry_t)[d registryID];
            io_registry_entry_t e = IOServiceGetMatchingService(
                kIOMainPortDefault, IORegistryEntryIDMatching([d registryID]));
            printf("\nThe two properties Metal actually reads\n");
            if (!e) {
                printf("  accelerator node     NOT FOUND for this registryID\n");
            } else {
                io_registry_entry_t parent = MACH_PORT_NULL;
                io_name_t pname = {0};
                if (IORegistryEntryGetParentEntry(e, "IOService",
                                                  &parent) == KERN_SUCCESS) {
                    IORegistryEntryGetName(parent, pname);
                    NSString *fam = prop(parent, @"ATY,FamilyName");
                    NSString *dev = prop(parent, @"ATY,DeviceName");
                    printf("  parent (IOService)   %s\n", pname);
                    printf("  ATY,FamilyName       %s\n",
                           fam ? fam.UTF8String : "ABSENT -> falls back to \"Radeon HD\"");
                    printf("  ATY,DeviceName       %s\n",
                           dev ? dev.UTF8String : "ABSENT -> falls back to \"GFX10 Family Unknown Prototype\"");
                    IOObjectRelease(parent);
                } else {
                    printf("  parent (IOService)   NONE - the lookup cannot work\n");
                }
                IOObjectRelease(e);
            }
            (void)accel;

            /* THE FOUR TESTS, run rather than inferred. sppci_model is the
             * GPU dictionary's model string, which on an armed boot is the
             * `model` property our framebuffer publishes. */
            NSString *model = @"AMD Radeon 680M";
            const char *env = getenv("RB_MODEL");
            if (env) { model = [NSString stringWithUTF8String:env]; }
            NSString *stripped =
                [model stringByReplacingOccurrencesOfString:@"AMD"
                                                 withString:@""];
            BOOL t1 = [model caseInsensitiveCompare:n] == NSOrderedSame;
            BOOL t2 = [n rangeOfString:@"Intel"
                               options:NSCaseInsensitiveSearch].location != NSNotFound;
            BOOL t3 = [n rangeOfString:stripped
                               options:NSCaseInsensitiveSearch].location != NSNotFound;
            BOOL t4 = [n rangeOfString:@"Apple"
                               options:NSCaseInsensitiveSearch].location != NSNotFound;
            printf("\nSPDisplaysReporter's four pairing tests, "
                   "sppci_model \"%s\"\n", model.UTF8String);
            printf("  1 equals device name          %s\n", t1 ? "MATCH" : "no");
            printf("  2 name contains \"Intel\"       %s\n", t2 ? "MATCH" : "no");
            printf("  3 name contains \"%s\"  %s\n",
                   stripped.UTF8String, t3 ? "MATCH" : "no");
            printf("  4 name contains \"Apple\"       %s\n", t4 ? "MATCH" : "no");
            printf("\n  VERDICT: %s\n",
                   (t1 || t2 || t3 || t4)
                     ? "PAIRED - System Information should carry all six "
                       "per-device keys including the Metal family"
                     : "NOT PAIRED - the reporter writes NONE of its six "
                       "per-device keys, so it falls back to "
                       "\"Metal: Supported\"");
        }
        /* And the outcome, independently of our prediction of it: the six
         * keys the pairing loop writes. Absence of all six is the signature
         * (2CB); a partial set would mean the reading is incomplete. */
        printf("\nWhat System Information actually carries now:\n");
        fflush(stdout);
        system("/usr/sbin/system_profiler -xml SPDisplaysDataType 2>/dev/null "
               "| /usr/bin/sed -n '/<key>_items/,/<\\/array>/p' "
               "| /usr/bin/grep -oE "
               "'spdisplays_(mtlgpufamilysupport|metalfamily|metal|metal_slot|"
               "metal_headless|metal_peer_group|metal_peer_index|metal_removable)' "
               "| /usr/bin/sort -u | /usr/bin/sed 's/^/  /'");
    }
    return 0;
}
