#!/usr/bin/env python3
"""Derive rb_glcave.h's two offsets from a machine's own GL driver bundle.

WHY THIS EXISTS. rb_glcave.h hardcodes two FILE OFFSETS inside one build of
AMDRadeonX6000GLDriver, and the kext refuses to patch any OS whose Darwin major
is not RB_GLCAVE_DARWIN (`glc_on 3`) because a near miss there would write a
jump into the middle of somebody else's instruction. Rule 41: a constant is
only a constant on the OS it was measured on. So a second OS needs its own row,
and this derives one without a boot and without a disassembler.

THE CONTROL IS THE MACHINE IT RUNS ON. Run with no argument on a Darwin 25
machine and it must reproduce rb_glcave.h's existing 0x168b4 / 0xec3d0 exactly.
If it cannot rebuild the row that is already known to work, the row it computes
for another OS is a coincidence and is printed as a refusal instead.

Usage:
  python3 tools/glrow.py                 # this machine's bundle, self-checked
  python3 tools/glrow.py <path-to-binary>
  python3 tools/glrow.py --selftest    # the guards this bundle cannot exercise
"""
import sys, os, struct

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), 're'))

DEFAULT = ("/System/Library/Extensions/AMDRadeonX6000GLDriver.bundle"
           "/Contents/MacOS/AMDRadeonX6000GLDriver")

def hdr_consts():
    """The expected row and the byte patterns, read from rb_glcave.h itself so
    this script cannot drift from the header it feeds."""
    p = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                     '..', 'src', 'common', 'rb_glcave.h')
    txt = open(p).read()
    out = {}
    for name in ('RB_GLCAVE_DARWIN', 'RB_GLCAVE_FN_OFF', 'RB_GLCAVE_CAVE_OFF',
                 'RB_GLCAVE_ANCHOR', 'RB_GLCAVE_CAVE_LEN', 'RB_GLCAVE_FN_LEN'):
        for line in txt.splitlines():
            if line.startswith('#define ' + name):
                v = line.split()[2].rstrip('u')
                out[name] = int(v, 0)
                break
        else:
            raise SystemExit('rb_glcave.h has no %s' % name)
    # rb_glcave_fn_find is the prologue the detour replaces: the shortest
    # unambiguous signature we already trust, since the kext matches on it.
    body = txt.split('rb_glcave_fn_find[RB_GLCAVE_FN_LEN] = {', 1)[1]
    body = body.split('}', 1)[0]
    fn = [int(x, 0) for x in body.replace('\n', ' ').split(',') if x.strip()]
    if len(fn) != out['RB_GLCAVE_FN_LEN']:
        raise SystemExit('fn_find is %d bytes, FN_LEN says %d'
                         % (len(fn), out['RB_GLCAVE_FN_LEN']))
    out['FN_FIND'] = bytes(fn)
    return out

def segments(buf):
    """__TEXT-relative file offsets of each section, from the Mach-O itself.
    A fat file is refused rather than guessed at."""
    magic, = struct.unpack_from('<I', buf, 0)
    if magic in (0xcafebabe, 0xbebafeca):
        raise SystemExit('REFUSING: this is a fat binary. Extract the x86_64 '
                         'slice first (lipo -thin x86_64).')
    if magic != 0xfeedfacf:
        raise SystemExit('REFUSING: not a 64-bit Mach-O (magic %#x)' % magic)
    ncmds, = struct.unpack_from('<I', buf, 16)
    off = 32
    secs = []
    for _ in range(ncmds):
        cmd, size = struct.unpack_from('<II', buf, off)
        if cmd == 0x19:  # LC_SEGMENT_64
            segname = buf[off+8:off+24].rstrip(b'\0').decode()
            nsects, = struct.unpack_from('<I', buf, off+64)
            so = off + 72
            for _s in range(nsects):
                sect = buf[so:so+16].rstrip(b'\0').decode()
                sfileoff, = struct.unpack_from('<I', buf, so+48)
                ssize, = struct.unpack_from('<Q', buf, so+40)
                secs.append((segname, sect, sfileoff, ssize))
                so += 80
        off += size
    return secs

def find_one(buf, pat, lo, hi, what):
    """Exactly one occurrence or a refusal. Rule 26: a search is an instrument,
    and the count is reported either way."""
    hits = []
    i = lo
    while True:
        j = buf.find(pat, i, hi)
        if j < 0:
            break
        hits.append(j)
        i = j + 1
    if len(hits) != 1:
        return None, ('%s: %d occurrences, expected exactly 1%s'
                      % (what, len(hits),
                         ' (' + ', '.join(hex(h) for h in hits) + ')'
                         if 0 < len(hits) <= 8 else ''))
    return hits[0], None

def find_cave(buf, secs, need):
    """A 16-aligned run of `need` zero bytes inside __const. Returned with the
    run's full length, so a caller can see it is not a one-byte-margin fit."""
    for segname, sect, so, ssize in secs:
        if sect != '__const':
            continue
        end = so + ssize
        i = (so + 15) & ~15
        while i + need <= end:
            if buf[i:i+need] == b'\0' * need:
                run = need
                while i + run < end and buf[i+run] == 0:
                    run += 1
                return i, run, '%s,%s' % (segname, sect)
            i += 16
        # fall through to the next __const section
    return None, 0, None

def macho(sections, text_bytes, const_bytes):
    """A minimal 64-bit Mach-O with one LC_SEGMENT_64 and the given sections,
    built only so the three guards below have inputs this machine's own bundle
    cannot provide."""
    nsects = len(sections)
    segsz = 72 + 80 * nsects
    hdr = struct.pack('<IiiIIIII', 0xfeedfacf, 0x01000007, 3, 2, 1, segsz, 0, 0)
    body = bytearray()
    payload_off = 32 + segsz
    data = bytearray()
    sects = b''
    for name, blob in sections:
        off = payload_off + len(data)
        data += blob
        sects += (name.encode().ljust(16, b'\0') + b'__TEXT'.ljust(16, b'\0')
                  + struct.pack('<QQI', 0, len(blob), off)
                  + struct.pack('<IIIIII', 0, 0, 0, 0, 0, 0)
                  + struct.pack('<I', 0))
    seg = (struct.pack('<II', 0x19, segsz) + b'__TEXT'.ljust(16, b'\0')
           + struct.pack('<QQQQiiII', 0, payload_off + len(data), 0,
                         payload_off + len(data), 7, 5, nsects, 0))
    return bytes(hdr) + seg + sects + bytes(data)


def selftest():
    """The three guards the real bundle cannot exercise, because on THIS build
    the prologue occurs exactly once in the whole file and the first aligned
    zero run in __const is already long enough. The break-it sweep found all
    three firing nothing, which is a result about the data, not about the
    guards - so they are given inputs here instead of being left untested."""
    k = hdr_consts()
    pro = k['FN_FIND']
    need = k['RB_GLCAVE_CAVE_LEN']
    fails = 0

    def ck(cond, msg):
        nonlocal fails
        if not cond:
            print('FAIL selftest: %s' % msg); fails += 1

    # GUARD 1: two occurrences must refuse. Not reachable on this bundle.
    text = b'\x90' * 64 + pro + b'\x90' * 64 + pro + b'\x90' * 64
    off, err = find_one(text, pro, 0, len(text), 'two')
    ck(off is None and err and '2 occurrences' in err,
       'TWO prologues must refuse, not pick one')
    # and zero occurrences, the other side of the same guard
    off, err = find_one(b'\x90' * 256, pro, 0, 256, 'none')
    ck(off is None and err and '0 occurrences' in err,
       'ZERO prologues must refuse')
    # exactly one still works, so the guard is not refusing everything
    text1 = b'\x90' * 32 + pro + b'\x90' * 32
    off, err = find_one(text1, pro, 0, len(text1), 'one')
    ck(off == 32 and err is None, 'exactly one is found at its offset')

    # GUARD 2: the __text BOUND. A second copy of the prologue OUTSIDE __text
    # must not be counted - on this bundle there is no second copy anywhere, so
    # the bound carried no information and the sweep correctly fired nothing.
    buf = b'\x90' * 100 + pro + b'\x90' * 100 + pro + b'\x90' * 100
    off, err = find_one(buf, pro, 0, 100 + len(pro) + 50, 'bounded')
    ck(off == 100 and err is None,
       'the bound EXCLUDES a second occurrence outside the range')

    # GUARD 3: the cave's LENGTH and ALIGNMENT. A short aligned run earlier
    # than a long one must be skipped, which this bundle cannot show because
    # its first aligned run is already long enough.
    const = bytearray(b'\xaa' * 16 + b'\0' * 32 + b'\xaa' * 16
                      + b'\0' * (need + 48))
    m = macho([('__text', b'\x90' * 64 + pro + b'\x90' * 64),
               ('__const', bytes(const))], None, None)
    secs = segments(m)
    cave, run, where = find_cave(m, secs, need)
    cstart = [s for s in secs if s[1] == '__const'][0][2]
    ck(cave is not None, 'a long enough run is found')
    if cave is not None:
        # The 16-alignment is on the FILE offset, not the section offset, so
        # the chosen cave is the first aligned file offset from which `need`
        # zero bytes run - NOT necessarily the start of the long run. Asserting
        # +64 was my own error: the property is that the short run at +16..+47
        # is skipped and the result lies inside the long run at +64 onward.
        ck(cave - cstart >= 64,
           'the chosen cave is inside the LONG run (>= +64), got +%d'
           % (cave - cstart))
        ck(not (16 <= cave - cstart < 48),
           'the SHORT 32-byte run at +16..+47 is never chosen for a %d-byte '
           'cave, got +%d' % (need, cave - cstart))
        ck(all(b == 0 for b in m[cave:cave + need]),
           'every byte of the chosen cave is actually zero')
        ck(run >= need, 'the reported run length is at least what was needed')
        ck(cave % 16 == 0, 'the chosen cave is 16-aligned')

    # ALIGNMENT, separately: a long run that starts unaligned must be rejected
    # at its unaligned start and accepted only from the next 16 boundary.
    const2 = bytearray(b'\xaa' * 17 + b'\0' * (need + 64))
    m2 = macho([('__text', b'\x90' * 64 + pro + b'\x90' * 64),
                ('__const', bytes(const2))], None, None)
    secs2 = segments(m2)
    cave2, _run2, _w2 = find_cave(m2, secs2, need)
    c2start = [s for s in secs2 if s[1] == '__const'][0][2]
    ck(cave2 is not None and cave2 % 16 == 0 and cave2 - c2start >= 17,
       'an unaligned run is taken only from its next 16 boundary')

    # A fat binary is refused rather than guessed at.
    try:
        segments(struct.pack('<I', 0xcafebabe) + b'\0' * 64)
        ck(False, 'a fat binary must be refused')
    except SystemExit as e:
        ck('fat binary' in str(e), 'the fat refusal names itself')
    # and a non-Mach-O
    try:
        segments(b'not a macho' + b'\0' * 64)
        ck(False, 'a non-Mach-O must be refused')
    except SystemExit as e:
        ck('Mach-O' in str(e), 'the non-Mach-O refusal names itself')

    # The whole pipeline on a synthetic file, end to end.
    off3, err3 = find_one(m, pro, 0, len(m), 'synthetic')
    ck(off3 is not None and err3 is None, 'the synthetic file parses and scans')

    if fails == 0:
        print('glrow selftest: ok (%d guards exercised that this machine\'s '
              'own bundle cannot)' % 11)
    return fails


def main():
    if len(sys.argv) > 1 and sys.argv[1] == '--selftest':
        return 1 if selftest() else 0
    k = hdr_consts()
    path = sys.argv[1] if len(sys.argv) > 1 else DEFAULT
    self_check = (len(sys.argv) == 1)
    try:
        buf = open(path, 'rb').read()
    except OSError as e:
        raise SystemExit('cannot read %s: %s' % (path, e))

    print('file   : %s' % path)
    print('size   : %d bytes' % len(buf))
    if self_check:
        print('OS     : Darwin %s (this machine)' % os.uname().release)
    secs = segments(buf)
    text = [s for s in secs if s[1] == '__text']
    if not text:
        raise SystemExit('REFUSING: no __text section')
    _, _, toff, tsize = text[0]

    fn, err = find_one(buf, k['FN_FIND'], toff, toff + tsize,
                       "the chooser's prologue in __text")
    if err:
        print('REFUSED - %s' % err)
        print('  The prologue is rb_glcave_fn_find, 15 bytes. Zero occurrences')
        print('  means this build compiles the chooser differently and the row')
        print('  cannot be derived by this method; more than one means the')
        print('  signature is not unique here and a longer one is needed.')
        return 2

    cave, run, where = find_cave(buf, secs, k['RB_GLCAVE_CAVE_LEN'])
    if cave is None:
        print('REFUSED - no 16-aligned %d-byte zero run in __const'
              % k['RB_GLCAVE_CAVE_LEN'])
        return 2

    print('__text : file offset %#x, %d bytes' % (toff, tsize))
    print('chooser: %#x  (RB_GLCAVE_FN_OFF)' % fn)
    print('cave   : %#x  in %s, zero run %d bytes (need %d)'
          % (cave, where, run, k['RB_GLCAVE_CAVE_LEN']))

    if self_check:
        okfn = (fn == k['RB_GLCAVE_FN_OFF'])
        okcv = (cave == k['RB_GLCAVE_CAVE_OFF'])
        print()
        print('SELF-CHECK against rb_glcave.h (Darwin %d):'
              % k['RB_GLCAVE_DARWIN'])
        print('  chooser %#x vs %#x  %s'
              % (fn, k['RB_GLCAVE_FN_OFF'], 'MATCH' if okfn else 'DIFFERS'))
        print('  cave    %#x vs %#x  %s'
              % (cave, k['RB_GLCAVE_CAVE_OFF'], 'MATCH' if okcv else 'DIFFERS'))
        if not (okfn and okcv):
            print('  This machine is the control. The method does NOT reproduce')
            print('  the row that is known to work here, so any row it prints')
            print('  for another OS is a coincidence. Do not use it.')
            return 1
        print('  The method reproduces the working row exactly, which is what')
        print('  licenses the row it computes on another OS.')
    else:
        print()
        print('Paste-ready row (verify the Darwin major on THAT machine):')
        print('  #define RB_GLCAVE_FN_OFF   %#xu' % fn)
        print('  #define RB_GLCAVE_CAVE_OFF %#xu' % cave)
        print('NOTE: these are offsets into THIS file only. The kext still')
        print('needs its Darwin gate widened to that OS, and the cave is')
        print('verified in the live process before the detour is written, so a')
        print('wrong row refuses rather than corrupting the image.')
    return 0

if __name__ == '__main__':
    sys.exit(main())
