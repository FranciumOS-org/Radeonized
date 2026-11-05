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
                     ? "PAIRED - System Information should carry "
                       "spdisplays_mtlgpufamilysupport (and NOT the legacy "
                       "spdisplays_metal, which the modern path removes)"
                     : "NOT PAIRED - the reporter writes none of its "
                       "per-device keys, so it falls back to "
                       "\"Metal: Supported\"");
        }
        /* And the outcome, independently of our prediction of it.
         *
         * 2CD: THIS SECTION USED AN UNANCHORED ALTERNATION AND REPORTED THE
         * OPPOSITE OF THE TRUTH. It grepped for
         * `spdisplays_(...|metal|metal_slot|...)` without anchoring to
         * `<key>`, so the VALUE `spdisplays_metal3` matched the bare `metal`
         * branch and printed as `spdisplays_metal` - which is the LEGACY key
         * the modern path removes. The one key whose disappearance proves
         * the mechanism was reported as still present. Rule 26 twice in one
         * expression: an unanchored match, and an alternation ordered so a
         * longer token can never win (the card already records that trap for
         * `rbgpu-diag` vs `rbgpu-diag-prev`).
         *
         * Anchored on `<key>` now, and printing the VALUE beside the key so
         * the family level is read rather than inferred from the key's
         * presence. */
        printf("\nWhat System Information actually carries now "
               "(keys only, anchored):\n");
        fflush(stdout);
        system("/usr/sbin/system_profiler -xml SPDisplaysDataType 2>/dev/null "
               "| /usr/bin/sed -n '/<key>_items<\\/key>/,/^\\t\\t<key>_parentDataType/p' "
               "| /usr/bin/grep -A1 -E '<key>spdisplays_(metal|mtlgpufamilysupport)[a-z_]*</key>' "
               "| /usr/bin/sed -e 's/^[[:space:]]*//' -e 's/^/  /'");
        /* The five keys the pairing loop writes AFTER the family one are
         * gated on `[device location]` being non-zero - `testq %rax,%rax;
         * je` immediately after the family store, measured at the
         * disassembly and confirmed by `location` reading 0
         * (MTLDeviceLocationBuiltIn) on this iGPU. So an integrated GPU gets
         * the family key and NOT the slot/peer set, and their absence is
         * EXPECTED rather than a partial failure. */
        printf("\n  location = %lu: 0 is MTLDeviceLocationBuiltIn, and the\n"
               "  reporter bails right after the family key when it is 0, so\n"
               "  spdisplays_metal_slot/_headless/_peer_group/_peer_index/\n"
               "  _removable are EXPECTED to be absent on an iGPU.\n",
               (unsigned long)[[devs firstObject] location]);
    }
    return 0;
}
