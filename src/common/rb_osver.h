/* rb_osver.h - which macOS this is, and what has actually been derived for it.
 *
 * WHY THIS FILE EXISTS (2xz). Until now the project had exactly one answer to
 * "which macOS does this support": whichever one the machine happened to be
 * running. The plugin declared Monterey..Sequoia to Lilu while the machine ran
 * Tahoe, which loaded only because `-lilubetaall` is in boot-args - a flag
 * that silently loosens the version check for EVERY Lilu plugin, and whose
 * removal would have made this one quietly not load at all. Meanwhile the
 * Apple addresses in the tree were derived on macOS 15 and the dyld-cache
 * patch had to be re-derived by hand for macOS 26 (2xt).
 *
 * So "support Ventura and up" is two separate claims and this file keeps them
 * apart, because conflating them is how a wrong address gets written to a
 * chip:
 *
 *   1. THE KEXT LOADS AND THE VERSION-INDEPENDENT HALF RUNS. That is the
 *      whole range, Darwin 22 (Ventura) through RB_OS_MAX_DARWIN. Everything
 *      this project does by SYMBOL NAME is in this half - routing, solving,
 *      the IOFramebuffer subclass, the registers, the SMU, the probes - and
 *      rule 1 already makes each of those check that the name is defined
 *      before it is used, so a renamed or absent symbol fails safe on its own.
 *
 *   2. A PARTICULAR VERSION-DEPENDENT ARTIFACT HAS BEEN MEASURED ON THIS OS.
 *      That is per OS and per artifact, and it is what `derived` below
 *      records. An address inside the dyld shared cache, a byte offset into
 *      an Apple driver's object, an instruction offset inside an Apple
 *      function: none of these can be inferred from the OS version, they can
 *      only be measured on it. A row that does not claim an artifact means
 *      nobody has measured it there - NOT that it is broken, and not that it
 *      works.
 *
 * Rule 14 in version form: "we do not support that OS" and "we have not
 * measured that OS" look identical from a log that says nothing. So every
 * boot publishes its row and its `derived` mask, and any path whose constants
 * came from a measurement declines - loudly, with the OS in the message -
 * when the running OS does not claim that artifact. Rule 17: a constant
 * measured on one OS and used on another without saying so IS the substituted
 * value that cost fifteen milestones.
 *
 * Freestanding and pure: no kernel headers, no allocation, no I/O, so the
 * hosttest decides exactly what the kext decides.
 */
#ifndef RB_OSVER_H
#define RB_OSVER_H

/* Darwin major == Lilu's KernelVersion == the kernel's own major. The
 * marketing numbers stopped tracking anything in 2025, so the Darwin number
 * is the key and the names are only for the log line. */
#define RB_OS_VENTURA  22
#define RB_OS_SONOMA   23
#define RB_OS_SEQUOIA  24
#define RB_OS_TAHOE    25

/* The declared range. The floor is the user's ask; the ceiling is the newest
 * Darwin this tree has a name for, and it is a CEILING on the claim, not on
 * the code - a newer OS gets the unknown-row treatment below rather than a
 * refusal to load, because refusing to load is the one outcome from which
 * nothing can be measured. */
#define RB_OS_MIN_DARWIN RB_OS_VENTURA
#define RB_OS_MAX_DARWIN RB_OS_TAHOE

/* The version-dependent artifacts, one bit each. Add a bit here and the
 * hosttest will require every row to be explicit about it. */
#define RB_OSD_KEXTOFF  (1u << 0)  /* byte offsets into Apple kext objects and
                                    * vtable slots: pm4+0x30, hw->0x368,
                                    * hw+0x3b0, the [0x148]/[0x118]/[0x120]
                                    * accessors, IOFramebuffer's 0x9b0/0xad0/
                                    * 0xae0. Measured, never inferred. */
#define RB_OSD_MTLCAVE  (1u << 1)  /* the dyld-shared-cache swizzle patch:
                                    * AMDRadeonX6000MTLDriver's __TEXT, the
                                    * function, the code cave and the two
                                    * rel32s between them. Per OS BUILD, not
                                    * per OS version - the row is a starting
                                    * point and the byte-pattern compare is
                                    * what actually authorises the write. */
#define RB_OSD_XHCI     (1u << 2)  /* the USB interrupter-0 patch table (2xv).
                                    * tools/xhci_intr0.py already refuses to
                                    * install a table that does not match the
                                    * running kernel, so this bit records the
                                    * claim and that tool enforces it. */
#define RB_OSD_ALL      (RB_OSD_KEXTOFF | RB_OSD_MTLCAVE | RB_OSD_XHCI)

struct rb_os_row {
    int          darwin;
    const char  *name;
    unsigned     derived;
    const char  *note;
};

/* WHAT EACH ROW CLAIMS, and the evidence for it.
 *
 * Sequoia (24) is where everything in this project was first measured: the
 * object offsets, the accessor vtable slots, the cave. Tahoe (25) claims the
 * same object offsets NOT because they were re-derived but because they were
 * RE-MEASURED IN USE - boot 247 is an accelerated desktop on Tahoe running
 * the offsets derived on Sequoia, which is the only kind of evidence that
 * matters for them. The cave is the opposite: Tahoe moved the driver by
 * 0x84E1000 and rebuilt the image, so its row had to be derived from scratch
 * (2xt) and Sequoia's is kept beside it rather than replaced.
 *
 * Ventura (22) and Sonoma (23) claim NOTHING, and that is the honest state:
 * no boot of this project has ever happened on either. They are inside the
 * declared range so that the version-independent half runs and so that a
 * first boot there can MEASURE something; they are not inside any artifact's
 * claim, so no measured constant is used there without an explicit override. */
static const struct rb_os_row rb_os_rows[] = {
    { RB_OS_VENTURA, "Ventura", 0,
      "in range, nothing measured: no boot of this project has happened here" },
    { RB_OS_SONOMA,  "Sonoma",  0,
      "in range, nothing measured: no boot of this project has happened here" },
    { RB_OS_SEQUOIA, "Sequoia", RB_OSD_KEXTOFF | RB_OSD_MTLCAVE,
      "where every offset in the tree was derived; boots 1-231" },
    { RB_OS_TAHOE,   "Tahoe",
      RB_OSD_KEXTOFF | RB_OSD_MTLCAVE | RB_OSD_XHCI,
      "offsets carried over and re-measured in use (boot 247); cave re-derived (2xt); USB table derived (2xv)" },
};

#define RB_OS_ROW_COUNT \
    ((int)(sizeof(rb_os_rows) / sizeof(rb_os_rows[0])))

/* The row for this Darwin major, or 0 if there is none. A newer OS than this
 * tree knows has no row, which is the same answer as an unmeasured one: the
 * caller must then decline or be forced. */
static inline const struct rb_os_row *rb_os_row_for(int darwin)
{
    int i;

    for (i = 0; i < RB_OS_ROW_COUNT; i++) {
        if (rb_os_rows[i].darwin == darwin) {
            return &rb_os_rows[i];
        }
    }
    return 0;
}

/* 1 if the kext declares that it loads here at all. Deliberately a RANGE and
 * not a row lookup: a Darwin newer than RB_OS_MAX_DARWIN is in range and gets
 * an empty `derived` mask, because refusing to load on a new OS is the one
 * outcome that teaches nothing. Below the floor is a flat no - Monterey and
 * earlier were never in scope and their IOGraphics is a different shape. */
static inline int rb_os_in_range(int darwin)
{
    return darwin >= RB_OS_MIN_DARWIN;
}

/* The `derived` mask for this OS: 0 for an OS with no row, including one
 * newer than this tree knows. */
static inline unsigned rb_os_derived_mask(int darwin)
{
    const struct rb_os_row *r = rb_os_row_for(darwin);

    return r ? r->derived : 0u;
}

/* 1 if EVERY artifact in `what` has been measured on this OS. `what` may name
 * several bits; the answer is all-or-nothing on purpose, because a caller
 * that needs two measured things and gets one is in the same position as a
 * caller that got neither. */
static inline int rb_os_derived(int darwin, unsigned what)
{
    if (what == 0u) {
        return 1;
    }
    return (rb_os_derived_mask(darwin) & what) == what;
}

/* The name, for the one log line each boot gets. Never 0, so a caller can
 * print it without a branch - an OS with no row still has a name to print,
 * which is the whole point of rule 14 here. */
static inline const char *rb_os_name(int darwin)
{
    const struct rb_os_row *r = rb_os_row_for(darwin);

    if (r) {
        return r->name;
    }
    return darwin > RB_OS_MAX_DARWIN ? "newer-than-this-tree" : "out-of-range";
}

#endif /* RB_OSVER_H */
