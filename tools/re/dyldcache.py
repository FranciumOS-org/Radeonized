#!/usr/bin/env python3
"""Read an image out of the dyld shared cache.

macOS ships no extractor and Xcode is not installed on this machine, so this
does the minimum: build a vm->file map from every subcache's mappings, find an
image by path in `imagesText`, and serve bytes at any cache VM address. The
image's Mach-O load commands are then parsed in place, which is enough for
objdump and for symbol lookup.

  dyldcache.py list [substring]          images whose path matches
  dyldcache.py info <substring>          load commands and sections
  dyldcache.py dump <substring> <out>    write a standalone-ish Mach-O
  dyldcache.py read <addr> <len>         hex bytes at a cache VM address
  dyldcache.py syms <substring>          exported symbols (trie)
"""
import os, struct, sys, glob

CACHE = "/System/Volumes/Preboot/Cryptexes/OS/System/Library/dyld/dyld_shared_cache_x86_64h"


class Cache:
    def __init__(self, base=CACHE):
        self.files = []          # (mmap-ish bytes, path)
        self.maps = []           # (vmaddr, size, fileoff, file_index)
        paths = [base] + sorted(glob.glob(base + ".[0-9][0-9]"))
        for p in paths:
            if not os.path.exists(p):
                continue
            fh = open(p, "rb")
            idx = len(self.files)
            self.files.append(fh)
            d = fh.read(0x400)
            if d[:7] != b"dyld_v1":
                continue
            mo, mc = struct.unpack_from("<II", d, 0x10)
            fh.seek(mo)
            mapdata = fh.read(32 * mc)
            for i in range(mc):
                a, s, fo, _mx, _ip = struct.unpack_from("<QQQII", mapdata, 32 * i)
                if s:
                    self.maps.append((a, s, fo, idx))
            if idx == 0:
                self.ito, self.itc = struct.unpack_from("<QQ", d, 0x88)
                self.hdr = d
        self.maps.sort()

    def read(self, addr, n):
        out = b""
        while n > 0:
            for a, s, fo, fi in self.maps:
                if a <= addr < a + s:
                    take = min(n, a + s - addr)
                    self.files[fi].seek(fo + (addr - a))
                    out += self.files[fi].read(take)
                    addr += take
                    n -= take
                    break
            else:
                raise KeyError("addr 0x%x not mapped" % addr)
        return out

    def cstr_at_fileoff(self, off, fi=0):
        self.files[fi].seek(off)
        b = self.files[fi].read(1024)
        return b[: b.index(b"\0")].decode("utf-8", "replace")

    def images(self):
        """[(loadAddress, textSize, path)] from imagesText."""
        f = self.files[0]
        f.seek(self.ito)
        d = f.read(32 * self.itc)
        out = []
        for i in range(self.itc):
            _uuid = d[32 * i : 32 * i + 16]
            la, ts, po = struct.unpack_from("<QII", d, 32 * i + 16)
            out.append((la, ts, self.cstr_at_fileoff(po)))
        return out

    def find(self, sub):
        hits = [im for im in self.images() if sub.lower() in im[2].lower()]
        if not hits:
            sys.exit("no image matches %r" % sub)
        if len(hits) > 1:
            exact = [h for h in hits if os.path.basename(h[2]).lower() == sub.lower()]
            if exact:
                return exact[0]
            sys.stderr.write("ambiguous:\n" + "\n".join("  " + h[2] for h in hits) + "\n")
            sys.exit(1)
        return hits[0]


class Image:
    """Mach-O load commands parsed in place inside the cache."""

    def __init__(self, cache, addr):
        self.c = cache
        self.base = addr
        hdr = cache.read(addr, 32)
        magic, self.cputype, self.cpusub, self.filetype, self.ncmds, self.sizeofcmds, self.flags, _ = \
            struct.unpack_from("<IiiIIIII", hdr, 0)
        assert magic == 0xFEEDFACF, "bad magic 0x%x at 0x%x" % (magic, addr)
        self.cmds = []
        self.segs = []       # (name, vmaddr, vmsize, fileoff, filesize, [sections])
        self.dyld_info = None
        self.symtab = None
        blob = cache.read(addr + 32, self.sizeofcmds)
        off = 0
        for _ in range(self.ncmds):
            cmd, size = struct.unpack_from("<II", blob, off)
            body = blob[off : off + size]
            self.cmds.append((cmd, body))
            if cmd == 0x19:  # LC_SEGMENT_64
                name = body[8:24].rstrip(b"\0").decode()
                va, vs, fo, fs = struct.unpack_from("<QQQQ", body, 24)
                nsects, = struct.unpack_from("<I", body, 64)
                sects = []
                for i in range(nsects):
                    so = 72 + i * 80
                    sn = body[so : so + 16].rstrip(b"\0").decode()
                    sg = body[so + 16 : so + 32].rstrip(b"\0").decode()
                    sa, ss, sfo = struct.unpack_from("<QQI", body, so + 32)
                    sects.append((sn, sg, sa, ss, sfo))
                self.segs.append((name, va, vs, fo, fs, sects))
            elif cmd in (0x22, 0x80000022):  # LC_DYLD_INFO(_ONLY)
                self.dyld_info = struct.unpack_from("<IIIIIIIIII", body, 8)
            elif cmd == 0x02:  # LC_SYMTAB
                self.symtab = struct.unpack_from("<IIII", body, 8)
            elif cmd == 0x80000034:  # LC_DYLD_EXPORTS_TRIE
                self.exports_trie = struct.unpack_from("<II", body, 8)
            off += size

    def section(self, segname, sectname):
        for name, va, vs, fo, fs, sects in self.segs:
            if name != segname:
                continue
            for sn, sg, sa, ss, sfo in sects:
                if sn == sectname:
                    return sa, ss
        return None

    def exports(self):
        """Walk the export trie -> {name: address}."""
        tr = getattr(self, "exports_trie", None)
        if tr is None and self.dyld_info:
            tr = (self.dyld_info[8], self.dyld_info[9])
        if not tr or not tr[1]:
            return {}
        # the trie lives at a file offset relative to the image's first mapping;
        # in the cache, LINKEDIT file offsets are cache-file offsets
        blob = self._linkedit_bytes(tr[0], tr[1])
        out = {}

        def uleb(b, i):
            r = s = 0
            while True:
                c = b[i]; i += 1
                r |= (c & 0x7F) << s
                if not c & 0x80:
                    return r, i
                s += 7

        def walk(off, prefix):
            if off >= len(blob):
                return
            i = off
            term, i = uleb(blob, i)
            if term:
                end = i + term
                flags, i = uleb(blob, i)
                if flags & 0x08:      # reexport
                    _ord, i = uleb(blob, i)
                else:
                    addr, i = uleb(blob, i)
                    out[prefix] = self.base + addr
                i = end
            nkids = blob[i]; i += 1
            for _ in range(nkids):
                e = blob.index(b"\0", i)
                name = blob[i:e].decode("utf-8", "replace")
                i = e + 1
                koff, i = uleb(blob, i)
                walk(koff, prefix + name)

        walk(0, "")
        return out

    def _linkedit_bytes(self, fileoff, size):
        for name, va, vs, fo, fs, _s in self.segs:
            if name == "__LINKEDIT":
                return self.c.read(va + (fileoff - fo), size)
        raise KeyError("no __LINKEDIT")

    def dump(self, path):
        """Write segments at their file offsets; good enough for objdump -d."""
        total = max(fo + fs for _n, _a, _s, fo, fs, _x in self.segs if fs)
        buf = bytearray(total)
        hdrsz = 32 + self.sizeofcmds
        buf[0:hdrsz] = self.c.read(self.base, hdrsz)
        for name, va, vs, fo, fs, _s in self.segs:
            if fs == 0 or name == "__LINKEDIT":
                continue
            try:
                buf[fo : fo + fs] = self.c.read(va, fs)
            except KeyError as e:
                sys.stderr.write("  skip %s: %s\n" % (name, e))
        for name, va, vs, fo, fs, _s in self.segs:
            if name == "__LINKEDIT" and fs:
                try:
                    need = fo + fs
                    if need > len(buf):
                        buf.extend(b"\0" * (need - len(buf)))
                    buf[fo : fo + fs] = self.c.read(va, fs)
                except KeyError as e:
                    sys.stderr.write("  skip __LINKEDIT: %s\n" % e)
        open(path, "wb").write(buf)
        return len(buf)


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    c = Cache()
    op = sys.argv[1]
    if op == "list":
        sub = sys.argv[2].lower() if len(sys.argv) > 2 else ""
        for la, ts, p in c.images():
            if sub in p.lower():
                print("0x%012x  %9d  %s" % (la, ts, p))
    elif op == "read":
        addr = int(sys.argv[2], 0); n = int(sys.argv[3], 0)
        d = c.read(addr, n)
        for i in range(0, len(d), 16):
            print("%012x  " % (addr + i) + " ".join("%02x" % b for b in d[i : i + 16]))
    elif op in ("info", "dump", "syms"):
        la, ts, p = c.find(sys.argv[2])
        im = Image(c, la)
        if op == "info":
            print("%s\n  base 0x%x  filetype %d  ncmds %d" % (p, la, im.filetype, im.ncmds))
            for name, va, vs, fo, fs, sects in im.segs:
                print("  %-16s vm 0x%011x+0x%-9x file 0x%x" % (name, va, vs, fo))
                for sn, sg, sa, ss, sfo in sects:
                    print("      %-20s 0x%011x  0x%x" % (sn, sa, ss))
        elif op == "dump":
            n = im.dump(sys.argv[3])
            print("wrote %s (%d bytes), base 0x%x" % (sys.argv[3], n, la))
        else:
            ex = im.exports()
            print("%d exports" % len(ex))
            for k in sorted(ex, key=lambda k: ex[k]):
                print("0x%012x  %s" % (ex[k], k))
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()


def symtab(cache, im):
    """[(addr, name)] from LC_SYMTAB, whose offsets are cache file offsets."""
    import struct as _s
    sym = im.symtab
    if not sym:
        return []
    symoff, nsyms, stroff, strsize = sym
    le = [s for s in im.segs if s[0] == "__LINKEDIT"][0]
    _n, leva, _vs, lefo, _fs, _x = le

    def at(fo, n):
        return cache.read(leva + (fo - lefo), n)

    nl = at(symoff, 16 * nsyms)
    strs = at(stroff, strsize)
    out = []
    for i in range(nsyms):
        strx, ntype, nsect, ndesc, nval = _s.unpack_from("<IBBHQ", nl, 16 * i)
        if strx >= len(strs):
            continue
        e = strs.index(b"\0", strx)
        name = strs[strx:e].decode("utf-8", "replace")
        if name and nval:
            out.append((nval, name, ntype))
    out.sort()
    return out


def dump_compact(cache, im, path):
    """Write a small Mach-O: same load commands, file offsets repacked
    contiguously so llvm-objdump reads it with the cache's own VM addresses."""
    import struct as _s
    hdrsz = 32 + im.sizeofcmds
    cmds = bytearray(cache.read(im.base, hdrsz))
    body = bytearray()
    cur = (hdrsz + 0xFFF) & ~0xFFF
    # walk the load commands in place, rewriting offsets as we copy segments
    off = 32
    newsym = None
    for _ in range(im.ncmds):
        cmd, size = _s.unpack_from("<II", cmds, off)
        if cmd == 0x19:  # LC_SEGMENT_64
            name = bytes(cmds[off + 8 : off + 24]).rstrip(b"\0").decode()
            va, vs, fo, fs = _s.unpack_from("<QQQQ", cmds, off + 24)
            nsects, = _s.unpack_from("<I", cmds, off + 64)
            if fs:
                try:
                    data = cache.read(va, fs)
                except KeyError:
                    data = b"\0" * fs
                pad = cur - (hdrsz + len(body))
                body += b"\0" * pad
                newfo = cur
                body += data
                cur += fs
                cur = (cur + 0xFFF) & ~0xFFF
                _s.pack_into("<Q", cmds, off + 24 + 16, newfo)
                delta = newfo - fo
                for i in range(nsects):
                    so = off + 72 + i * 80
                    sfo, = _s.unpack_from("<I", cmds, so + 48)
                    if sfo:
                        _s.pack_into("<I", cmds, so + 48, sfo + delta)
                if name == "__LINKEDIT":
                    newsym = (fo, newfo)
        elif cmd == 0x02 and newsym:
            pass
        off += size
    # LC_SYMTAB offsets live inside __LINKEDIT; shift them by its delta
    if newsym:
        oldfo, newfo = newsym
        d = newfo - oldfo
        off = 32
        for _ in range(im.ncmds):
            cmd, size = _s.unpack_from("<II", cmds, off)
            if cmd == 0x02:
                symoff, nsyms, stroff, strsize = _s.unpack_from("<IIII", cmds, off + 8)
                _s.pack_into("<II", cmds, off + 8, symoff + d, nsyms)
                _s.pack_into("<II", cmds, off + 16, stroff + d, strsize)
            elif cmd in (0x0B,):  # LC_DYSYMTAB — indices only, no offsets to fix
                pass
            elif cmd == 0x80000033:  # chained fixups
                fxo, fxs = _s.unpack_from("<II", cmds, off + 8)
                _s.pack_into("<I", cmds, off + 8, fxo + d)
            off += size
    out = bytearray(cmds) + body
    open(path, "wb").write(out)
    return len(out)
