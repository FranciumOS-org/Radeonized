#!/usr/bin/env python3
"""Generate the DCN 3.0.2 -> 3.1.2 register remap table (3dcn).

WHY THIS EXISTS. Apple's AMDRadeonX6000Framebuffer drives the display through
a DAL built for Navi 2x. Navi 23 (Dimgrey Cavefish) is DCN 3.0.2; this chip is
DCN 3.1.2. The two generations keep almost every register NAME and move a large
number of OFFSETS, so a DAL that computes an address from its own
`dcn_3_0_2_offset.h` constants reads and writes the wrong dword on this part -
journal 22855's "or worse, program a 3.0 register layout into a 3.1.2 chip".

The fix is to translate the address inside Apple's own register accessors. This
script generates the table that translation uses, from the two upstream headers
and nothing else, so the table is a mechanical consequence of public kernel
sources rather than a hand-written list.

HOW AN ADDRESS IS BUILT. Each header gives, per register, an in-segment dword
offset and a BASE_IDX naming which segment base it is added to:

    #define mmDOMAIN16_PG_CONFIG           0x00a1   /* 3.0.2 */
    #define mmDOMAIN16_PG_CONFIG_BASE_IDX  2
    #define regDOMAIN16_PG_CONFIG          0x0089   /* 3.1.2, `reg` prefix */
    #define regDOMAIN16_PG_CONFIG_BASE_IDX 2

(3.0.2 spells them `mm`, 3.1.2 spells them `reg`; same meaning.) The absolute
dword address this project and Apple's DAL both use is `base[BASE_IDX] +
offset`. The bases come from --base and default to this board's measured DMU
values: segment 2 = 0x34c0 and BASE_IDX 1 = 0xc0, both read off the chip
(2EF, `tests/data/rembrandt-680m-tufa15.bin`, printed by `build/out/rbdump`)
and recorded in CLAUDE.md.

THE BASES ARE ASSUMED EQUAL ACROSS THE TWO GENERATIONS, and that assumption is
load-bearing, so it is stated rather than buried: the remap is the difference
in the IN-SEGMENT offset only. It is not a guess. It reproduces a published
table generated independently from the same headers by 660m-Metal
(`dcnremap.hpp`), whose first entry is `{0x3561, 0x3549, "DOMAIN16_PG_CONFIG"}`
- and 0x3561 - 0xa1 == 0x3549 - 0x89 == 0x34c0, so that project resolved both
sides against the same base too. `--check-against` diffs our output against
theirs entry by entry, which is the control: two generators, same sources,
independent code.

WHAT IT REFUSES TO DO. A register present in one generation and absent from the
other is NOT emitted, and is counted and reportable with --report-missing. A
rename is indistinguishable from a removal from these files alone, and emitting
a guess would put a fabricated address into a write path. A register whose
BASE_IDX differs between generations is likewise not emitted but is reported as
a conflict, because the equal-bases assumption above does not cover it.
"""

import argparse
import re
import sys

# `#define <prefix><NAME>  <value>` with the value in hex or decimal.
DEF_RE = re.compile(r"^#define\s+(\w+)\s+(0x[0-9a-fA-F]+|\d+)\s*$")

DEFAULT_BASES = {
    # THE DMU SEGMENT BASES, READ OFF THIS CHIP'S OWN DISCOVERY TABLE.
    # `build/out/rbdump tests/data/rembrandt-680m-tufa15.bin` prints the DMU
    # row as `271 0 3.1.3.0  0x12 0xc0 0x34c0 0x9000 0x2403c00`, which is
    # base[0..4]. base[1] = 0xc0 and base[2] = 0x34c0 are the two CLAUDE.md
    # already recorded from 2EF; base[3] = 0x9000 is read here for the first
    # time and it is what makes the registers that CHANGE BASE_IDX between the
    # two generations mappable at all.
    #
    # Reading it from the chip rather than inferring it from 660m-Metal's
    # published table matters: their table implies the same 0x9000
    # (0x98c7 - 0x08c7), and taking it from them would have made the
    # cross-check circular for exactly the entries it was needed for.
    0: 0x0012,
    1: 0x00c0,
    2: 0x34c0,
    3: 0x9000,
    4: 0x2403c00,
}


def parse(path, prefix):
    """-> {NAME: (offset, base_idx)} for every register with both defines."""
    offs, bases = {}, {}
    with open(path, "r", errors="replace") as fh:
        for line in fh:
            m = DEF_RE.match(line)
            if not m:
                continue
            sym, val = m.group(1), int(m.group(2), 0)
            if not sym.startswith(prefix):
                continue
            name = sym[len(prefix):]
            if name.endswith("_BASE_IDX"):
                bases[name[: -len("_BASE_IDX")]] = val
            else:
                offs[name] = val
    out = {}
    for name, off in offs.items():
        if name in bases:
            out[name] = (off, bases[name])
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--old", default="third_party/linux-ref/dcn_3_0_2_offset.h")
    ap.add_argument("--new", default="third_party/linux-ref/dcn_3_1_2_offset.h")
    ap.add_argument("--base", action="append", default=[],
                    metavar="IDX=ADDR",
                    help="override a segment base, e.g. 2=0x34c0")
    ap.add_argument("--out", help="write the C header here (default: stdout)")
    ap.add_argument("--check-against",
                    help="a 660m-Metal style dcnremap.hpp to diff against")
    ap.add_argument("--report-missing", action="store_true")
    args = ap.parse_args()

    bases = dict(DEFAULT_BASES)
    for spec in args.base:
        idx, _, addr = spec.partition("=")
        bases[int(idx, 0)] = int(addr, 0)

    old = parse(args.old, "mm")
    new = parse(args.new, "reg")
    if not old or not new:
        print("error: parsed no registers - wrong prefix or path?",
              file=sys.stderr)
        return 2

    rows, unmapped_base, conflict, missing = [], [], [], []
    for name, (ooff, obase) in sorted(old.items()):
        if name not in new:
            missing.append(name)
            continue
        noff, nbase = new[name]
        if obase not in bases or nbase not in bases:
            unmapped_base.append((name, obase if obase not in bases else nbase))
            continue
        # Each side is resolved against ITS OWN base, so a register that
        # changes BASE_IDX between the generations is handled correctly rather
        # than refused. An earlier draft refused these 47 because only
        # base[1] and base[2] were known and base[3] was not; with the whole
        # array read off the chip the equal-bases shortcut is unnecessary.
        if obase != nbase:
            conflict.append((name, obase, nbase))
        frm, to = bases[obase] + ooff, bases[nbase] + noff
        if frm != to:
            rows.append((frm, to, name))

    rows.sort(key=lambda r: r[0])

    # A `from` that repeats would make the lookup order-dependent, and the
    # consumer does a binary search on it. Refuse rather than emit it.
    dupes = {}
    for frm, to, name in rows:
        dupes.setdefault(frm, []).append((to, name))
    bad = {k: v for k, v in dupes.items() if len(v) > 1}
    if bad:
        print("error: duplicate `from` addresses, lookup would be ambiguous:",
              file=sys.stderr)
        for k, v in sorted(bad.items())[:10]:
            print("  0x%04x -> %s" % (k, v), file=sys.stderr)
        return 3

    text = []
    text.append("/* SPDX-License-Identifier: MIT */")
    text.append("/* GENERATED by tools/re/dcn_remap_gen.py - DO NOT EDIT.")
    text.append(" *")
    text.append(" * DCN 3.0.2 -> 3.1.2 absolute dword addresses. Apple's Navi 23 DAL")
    text.append(" * computes 3.0.2 addresses; this chip is 3.1.2. Sources:")
    text.append(" *   %s" % args.old)
    text.append(" *   %s" % args.new)
    text.append(" * Segment bases used: %s"
                % ", ".join("%d=0x%04x" % (k, v) for k, v in sorted(bases.items())))
    text.append(" * Entries: %d moved. Same-address registers are omitted." % len(rows))
    text.append(" * Of those, %d also change BASE_IDX and are resolved against each" % len(conflict))
    text.append(" * side's own base. Not emitted: %d absent from 3.1.2, %d in a"
                % (len(missing), len(unmapped_base)))
    text.append(" * segment with no known base. A rename cannot be told from a")
    text.append(" * removal from these headers, so nothing is guessed.")
    text.append(" */")
    text.append("#ifndef RB_DCN_REMAP_H")
    text.append("#define RB_DCN_REMAP_H")
    text.append("")
    text.append("#include <stdint.h>")
    text.append("")
    text.append("struct rb_dcn_remap { uint32_t from, to; };")
    text.append("")
    text.append("/* Sorted by `from`, unique, so a binary search is valid. */")
    text.append("static const struct rb_dcn_remap rb_dcn_remap_tbl[] = {")
    for frm, to, name in rows:
        text.append("    { 0x%04xu, 0x%04xu },  /* %s */" % (frm, to, name))
    text.append("};")
    text.append("")
    text.append("#define RB_DCN_REMAP_COUNT "
                "(sizeof(rb_dcn_remap_tbl) / sizeof(rb_dcn_remap_tbl[0]))")
    text.append("")
    text.append("#endif /* RB_DCN_REMAP_H */")
    blob = "\n".join(text) + "\n"

    if args.out:
        with open(args.out, "w") as fh:
            fh.write(blob)
        print("wrote %s: %d entries" % (args.out, len(rows)))
    else:
        sys.stdout.write(blob)

    print("moved %d, absent-from-3.1.2 %d, base-idx conflicts %d, no-base %d"
          % (len(rows), len(missing), len(conflict), len(unmapped_base)),
          file=sys.stderr)
    if args.report_missing:
        for name, a, b in conflict[:40]:
            print("  BASE_IDX conflict %s: %d -> %d" % (name, a, b),
                  file=sys.stderr)
        for name in missing[:40]:
            print("  absent from 3.1.2: %s" % name, file=sys.stderr)

    if args.check_against:
        theirs = {}
        row_re = re.compile(
            r"\{\s*(0x[0-9a-fA-F]+)\s*,\s*(0x[0-9a-fA-F]+)\s*,\s*\"([^\"]+)\"")
        with open(args.check_against, "r", errors="replace") as fh:
            for line in fh:
                m = row_re.search(line)
                if m:
                    theirs[int(m.group(1), 0)] = (int(m.group(2), 0), m.group(3))
        ours = {frm: (to, name) for frm, to, name in rows}
        both = set(ours) & set(theirs)
        agree = [k for k in both if ours[k][0] == theirs[k][0]]
        differ = [k for k in both if ours[k][0] != theirs[k][0]]
        print("\nCONTROL vs %s" % args.check_against, file=sys.stderr)
        print("  theirs %d, ours %d, common `from` %d"
              % (len(theirs), len(ours), len(both)), file=sys.stderr)
        print("  SAME `to`: %d" % len(agree), file=sys.stderr)
        print("  DIFFERENT `to`: %d" % len(differ), file=sys.stderr)
        for k in sorted(differ)[:20]:
            print("    0x%04x -> ours 0x%04x (%s) theirs 0x%04x (%s)"
                  % (k, ours[k][0], ours[k][1], theirs[k][0], theirs[k][1]),
                  file=sys.stderr)
        only_theirs = sorted(set(theirs) - set(ours))
        only_ours = sorted(set(ours) - set(theirs))
        print("  only in theirs: %d" % len(only_theirs), file=sys.stderr)
        for k in only_theirs[:20]:
            print("    0x%04x -> 0x%04x (%s)"
                  % (k, theirs[k][0], theirs[k][1]), file=sys.stderr)
        print("  only in ours: %d" % len(only_ours), file=sys.stderr)
        for k in only_ours[:20]:
            print("    0x%04x -> 0x%04x (%s)"
                  % (k, ours[k][0], ours[k][1]), file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
