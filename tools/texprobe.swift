// texprobe.swift — CPU-bitmap -> GPU texture round-trip matrix (no reboot needed, run on an armed boot)
// build: swiftc -O texprobe.swift -o texprobe -framework Metal -framework IOSurface
// Each path is written twice (tag 0xA5, then 0x5A) so stale data from the previous write is detectable.
// BGRA pixels encode their own (x,y), so wrong-but-valid data tells you WHERE it came from.
import Foundation
import Metal
import Dispatch
import IOSurface

setvbuf(stdout, nil, _IONBF, 0)

// Rule 14: absence is not a result. A nil device used to be a force-unwrap
// trap - SIGILL with an empty log, which looks exactly like a crash in the
// measurement. On an UNARMED boot there is no Metal device at all, so say so.
guard let dev = MTLCreateSystemDefaultDevice() else {
    print("NO METAL DEVICE - this boot has no accelerator.")
    print("  Almost certainly an unarmed boot. Check:")
    print("    nvram -p | grep rbgpu-log      # the peer says if arm was unset")
    print("    nvram -p | grep rbgpu-boot     # must have increased since the last capture")
    print("  Arm it with:  sudo nvram rbgpu-arm=1 && sudo reboot")
    exit(2)
}
guard let q = dev.makeCommandQueue() else {
    print("device:", dev.name, "- but makeCommandQueue() returned nil"); exit(2)
}
print("device:", dev.name)

let src = """
#include <metal_stdlib>
using namespace metal;
kernel void rd(texture2d<float, access::read> t [[texture(0)]],
               device uint *o [[buffer(0)]],
               constant uint2 &p [[buffer(1)]],
               uint2 g [[thread_position_in_grid]]) {
    if (g.x >= t.get_width() || g.y >= t.get_height()) return;
    uint4 c = uint4(round(clamp(t.read(g), 0.0, 1.0) * 255.0));
    o[g.y * p.x + g.x] = p.y ? c.x : (c.z | (c.y << 8) | (c.x << 16) | (c.w << 24));
}
"""
let lib = try! dev.makeLibrary(source: src, options: nil)
let pso = try! dev.makeComputePipelineState(function: lib.makeFunction(name: "rd")!)

func pat(_ x: Int, _ y: Int, _ tag: UInt32, _ r8: Bool) -> UInt32 {
    r8 ? UInt32((x &* 5 &+ y &* 11 &+ Int(tag)) & 0xff)
       : (tag << 24) | (UInt32(y) << 12) | UInt32(x)
}

func writeRect(_ dst: UnsafeMutableRawPointer, bpr: Int, rx: Int, ry: Int, rw: Int, rh: Int,
               tag: UInt32, r8: Bool, relative: Bool) {
    let bpp = r8 ? 1 : 4
    for y in 0..<rh {
        let row = dst + (relative ? y : ry + y) * bpr + (relative ? 0 : rx * bpp)
        for x in 0..<rw {
            let v = pat(rx + x, ry + y, tag, r8)
            if r8 { row.storeBytes(of: UInt8(v), toByteOffset: x, as: UInt8.self) }
            else { row.storeBytes(of: v.littleEndian, toByteOffset: x * 4, as: UInt32.self) }
        }
    }
}

// glyph-atlas-like small sub-rect updates
func tiles(_ w: Int, _ h: Int, _ f: (Int, Int, Int, Int) -> Void) {
    var y = 0
    while y < h {
        let th = min(9, h - y); var x = 0
        while x < w { let tw = min(13, w - x); f(x, y, tw, th); x += tw }
        y += th
    }
}

func desc(_ f: MTLPixelFormat, _ w: Int, _ h: Int, _ m: MTLStorageMode) -> MTLTextureDescriptor {
    let d = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: f, width: w, height: h, mipmapped: false)
    d.storageMode = m; d.usage = [.shaderRead]; return d
}

func readTex(_ t: MTLTexture, _ r8: Bool) -> [UInt32] {
    let w = t.width, h = t.height
    let out = dev.makeBuffer(length: w * h * 4, options: .storageModeShared)!
    memset(out.contents(), 0xEE, w * h * 4)
    var p = SIMD2<UInt32>(UInt32(w), r8 ? 1 : 0)
    let cb = q.makeCommandBuffer()!, e = cb.makeComputeCommandEncoder()!
    e.setComputePipelineState(pso); e.setTexture(t, index: 0)
    e.setBuffer(out, offset: 0, index: 0); e.setBytes(&p, length: 8, index: 1)
    e.dispatchThreadgroups(MTLSizeMake((w + 15) / 16, (h + 15) / 16, 1),
                           threadsPerThreadgroup: MTLSizeMake(16, 16, 1))
    e.endEncoding(); cb.commit(); cb.waitUntilCompleted()
    return Array(UnsafeBufferPointer(start: out.contents().bindMemory(to: UInt32.self, capacity: w * h), count: w * h))
}

func check(_ name: String, _ got: [UInt32], _ w: Int, _ h: Int, _ tag: UInt32, _ prev: UInt32, _ r8: Bool) {
    var bad = 0, stale = 0, foreign = 0, moved = 0, first = -1
    var deltas: [String: Int] = [:]
    for y in 0..<h { for x in 0..<w {
        let g = got[y * w + x]
        if g == pat(x, y, tag, r8) { continue }
        bad += 1; if first < 0 { first = y * w + x }
        if r8 { if g == pat(x, y, prev, r8) { stale += 1 } else { foreign += 1 }; continue }
        let gt = g >> 24
        if gt == prev { stale += 1 }
        else if gt != tag { foreign += 1 }
        else {
            moved += 1
            deltas["\(Int(g & 0xfff) - x),\(Int((g >> 12) & 0xfff) - y)", default: 0] += 1
        }
    }}
    let top = deltas.sorted { $0.value > $1.value }.prefix(4).map { "d(\($0.key))x\($0.value)" }.joined(separator: " ")
    var line = name.padding(toLength: 34, withPad: " ", startingAt: 0)
    line += bad == 0 ? "OK" : "BAD \(bad)/\(w*h) stale=\(stale) foreign=\(foreign) moved=\(moved) first=(\(first % w),\(first / w)) \(top)"
    print(line)
}

typealias Made = (MTLTexture, (UInt32) -> Void)
struct Path { let name: String; let make: (Int, Int, Bool) -> Made? }

func fmtFor(_ r8: Bool) -> MTLPixelFormat { r8 ? .r8Unorm : .bgra8Unorm }

let paths: [Path] = [
    Path(name: "linear-buffer-tex") { w, h, r8 in
        let f = fmtFor(r8), bpp = r8 ? 1 : 4, al = dev.minimumLinearTextureAlignment(for: f)
        let bpr = (w * bpp + al - 1) / al * al
        guard let buf = dev.makeBuffer(length: bpr * h, options: .storageModeManaged),
              let t = buf.makeTexture(descriptor: desc(f, w, h, .managed), offset: 0, bytesPerRow: bpr) else { return nil }
        return (t, { tag in
            writeRect(buf.contents(), bpr: bpr, rx: 0, ry: 0, rw: w, rh: h, tag: tag, r8: r8, relative: false)
            buf.didModifyRange(0..<buf.length) })
    },
    Path(name: "managed-replace-full") { w, h, r8 in
        let bpp = r8 ? 1 : 4
        guard let t = dev.makeTexture(descriptor: desc(fmtFor(r8), w, h, .managed)) else { return nil }
        return (t, { tag in
            var a = [UInt8](repeating: 0, count: w * h * bpp)
            a.withUnsafeMutableBytes {
                writeRect($0.baseAddress!, bpr: w * bpp, rx: 0, ry: 0, rw: w, rh: h, tag: tag, r8: r8, relative: false)
                t.replace(region: MTLRegionMake2D(0, 0, w, h), mipmapLevel: 0, withBytes: $0.baseAddress!, bytesPerRow: w * bpp)
            } })
    },
    Path(name: "managed-replace-tiles") { w, h, r8 in
        let bpp = r8 ? 1 : 4
        guard let t = dev.makeTexture(descriptor: desc(fmtFor(r8), w, h, .managed)) else { return nil }
        return (t, { tag in
            tiles(w, h) { rx, ry, rw, rh in
                var a = [UInt8](repeating: 0, count: rw * rh * bpp)
                a.withUnsafeMutableBytes {
                    writeRect($0.baseAddress!, bpr: rw * bpp, rx: rx, ry: ry, rw: rw, rh: rh, tag: tag, r8: r8, relative: true)
                    t.replace(region: MTLRegionMake2D(rx, ry, rw, rh), mipmapLevel: 0, withBytes: $0.baseAddress!, bytesPerRow: rw * bpp)
                }
            } })
    },
    Path(name: "private-blit-full") { w, h, r8 in
        let bpp = r8 ? 1 : 4, bpr = (w * bpp + 255) / 256 * 256
        guard let buf = dev.makeBuffer(length: bpr * h, options: .storageModeManaged),
              let t = dev.makeTexture(descriptor: desc(fmtFor(r8), w, h, .private)) else { return nil }
        return (t, { tag in
            writeRect(buf.contents(), bpr: bpr, rx: 0, ry: 0, rw: w, rh: h, tag: tag, r8: r8, relative: false)
            buf.didModifyRange(0..<buf.length)
            let cb = q.makeCommandBuffer()!, b = cb.makeBlitCommandEncoder()!
            b.copy(from: buf, sourceOffset: 0, sourceBytesPerRow: bpr, sourceBytesPerImage: bpr * h,
                   sourceSize: MTLSizeMake(w, h, 1), to: t, destinationSlice: 0, destinationLevel: 0,
                   destinationOrigin: MTLOriginMake(0, 0, 0))
            b.endEncoding(); cb.commit(); cb.waitUntilCompleted() })
    },
    Path(name: "private-blit-tiles") { w, h, r8 in
        let bpp = r8 ? 1 : 4, bpr = (w * bpp + 255) / 256 * 256
        guard let buf = dev.makeBuffer(length: bpr * h, options: .storageModeManaged),
              let t = dev.makeTexture(descriptor: desc(fmtFor(r8), w, h, .private)) else { return nil }
        return (t, { tag in
            writeRect(buf.contents(), bpr: bpr, rx: 0, ry: 0, rw: w, rh: h, tag: tag, r8: r8, relative: false)
            buf.didModifyRange(0..<buf.length)
            let cb = q.makeCommandBuffer()!, b = cb.makeBlitCommandEncoder()!
            tiles(w, h) { rx, ry, rw, rh in
                b.copy(from: buf, sourceOffset: ry * bpr + rx * bpp, sourceBytesPerRow: bpr, sourceBytesPerImage: bpr * rh,
                       sourceSize: MTLSizeMake(rw, rh, 1), to: t, destinationSlice: 0, destinationLevel: 0,
                       destinationOrigin: MTLOriginMake(rx, ry, 0))
            }
            b.endEncoding(); cb.commit(); cb.waitUntilCompleted() })
    },
    Path(name: "iosurface") { w, h, r8 in   // CALayer-contents / CPU-drawn-layer path
        let bpp = r8 ? 1 : 4
        let pf: UInt32 = r8 ? 0x4C303038 /* L008 */ : 0x42475241 /* BGRA */
        let bpr = IOSurfaceAlignProperty(kIOSurfaceBytesPerRow, w * bpp)
        let props: [IOSurfacePropertyKey: Any] = [.width: w, .height: h, .bytesPerElement: bpp,
                                                  .bytesPerRow: bpr, .pixelFormat: pf]
        guard let s = IOSurface(properties: props),
              let t = dev.makeTexture(descriptor: desc(fmtFor(r8), w, h, .managed), iosurface: s, plane: 0) else { return nil }
        return (t, { tag in
            _ = s.lock(options: [], seed: nil)
            writeRect(s.baseAddress, bpr: s.bytesPerRow, rx: 0, ry: 0, rw: w, rh: h, tag: tag, r8: r8, relative: false)
            _ = s.unlock(options: [], seed: nil) })
    },
]

for r8 in [false, true] {
    for w in [37, 256, 1000] {
        let h = 48
        for p in paths {
            let label = "\(r8 ? "r8" : "bgra8") w=\(w) \(p.name)"
            guard let (t, write) = p.make(w, h, r8) else { print(label, "UNSUPPORTED"); continue }
            var prev: UInt32 = 0
            for (i, tag) in ([0xA5, 0x5A] as [UInt32]).enumerated() {
                write(tag)
                check("\(label) #\(i + 1)", readTex(t, r8), w, h, tag, prev, r8)
                prev = tag
            }
        }
    }
}

// ---------------------------------------------------------------------------
// 2mf: REAL GLYPHS, and the focus/redraw effect.
//
// Everything above uses a synthetic pattern whose pixels encode their own
// (x,y), which is what made 2lo's `y ^ 32` readable. But no measurement has
// ever connected that defect to the glyphs actually on screen - 2lp assumed
// the link and 2me killed every layout explanation for the glyphs.
//
// So: rasterise real text with CoreText into a CPU BGRA buffer, push it
// through the same upload paths, read it back on the GPU and compare to the
// buffer we drew. A mismatch is the on-screen defect reproduced inside a
// program we can instrument completely (2lt: other apps' Metal cannot be).
//
// It also tests the user's observation (2026-10-21): desktop icons and
// iMessage pictures render scrambled while their window is UNFOCUSED and
// correct once clicked, while the Claude app and Safari images do not
// recover. A redraw fixing content means the stored bytes were right and
// something about reading them later was wrong - so each case is read back
// immediately, then again after the GPU has been idle.
//
// Row attribution: for every readback row that does not match its own
// reference row, the nearest matching reference row is reported, so a
// permutation prints as `12->44` rather than just "wrong".
import CoreGraphics
import CoreText

func glyphBitmap(_ w: Int, _ h: Int) -> ([UInt32], Int) {
    let bpr = w * 4
    var px = [UInt32](repeating: 0xff000000, count: w * h)
    px.withUnsafeMutableBytes { raw in
        let cs = CGColorSpaceCreateDeviceRGB()
        let bi = CGImageAlphaInfo.premultipliedFirst.rawValue
              | CGBitmapInfo.byteOrder32Little.rawValue
        guard let ctx = CGContext(data: raw.baseAddress, width: w, height: h,
                                  bitsPerComponent: 8, bytesPerRow: bpr,
                                  space: cs, bitmapInfo: bi) else { return }
        ctx.setFillColor(CGColor(red: 0, green: 0, blue: 0, alpha: 1))
        ctx.fill(CGRect(x: 0, y: 0, width: w, height: h))
        let font = CTFontCreateWithName("Menlo" as CFString, 13, nil)
        let white = CGColor(red: 1, green: 1, blue: 1, alpha: 1)
        let attrs: CFDictionary = [
            kCTFontAttributeName as String: font,
            kCTForegroundColorAttributeName as String: white] as CFDictionary
        var y = h - 14
        while y > 0 {
            let str = "The quick brown fox 0123456789 #\(y)" as CFString
            let a = CFAttributedStringCreate(nil, str, attrs)!
            let line = CTLineCreateWithAttributedString(a)
            ctx.textPosition = CGPoint(x: 2, y: CGFloat(y))
            CTLineDraw(line, ctx)
            y -= 14
        }
    }
    return (px, bpr)
}

func compareRows(_ got: [UInt32], _ want: [UInt32], _ w: Int, _ h: Int) -> (Int, [String]) {
    var bad = 0
    var moves: [String] = []
    for y in 0..<h {
        let g = Array(got[y * w ..< (y + 1) * w])
        let r = Array(want[y * w ..< (y + 1) * w])
        if g == r { continue }
        bad += 1
        var from = -1
        for c in 0..<h where Array(want[c * w ..< (c + 1) * w]) == g { from = c; break }
        if moves.count < 12 {
            moves.append(from >= 0 ? "\(y)<-\(from)" : "\(y)<-?")
        }
    }
    return (bad, moves)
}

print("")
print("=== 2mf: real CoreText glyphs ===")
for w in [256, 1000] {
    let h = 48
    let (want, _) = glyphBitmap(w, h)
    let ink = want.filter { $0 != 0xff000000 }.count
    print("reference w=\(w) h=\(h): \(ink) non-background pixels")
    // `replace(region:)` is illegal on a .private texture - those paths are
    // uploaded by blit instead. Calling replace on one segfaults, which is
    // what truncated 2mf's first run.
    for p in paths where p.name != "linear-buffer-tex" && !p.name.hasPrefix("private-") {
        guard let (t, _) = p.make(w, h, false) else { print("  \(p.name) UNSUPPORTED"); continue }
        // upload the glyph bitmap through this path's own mechanism
        want.withUnsafeBytes { src in
            t.replace(region: MTLRegionMake2D(0, 0, w, h), mipmapLevel: 0,
                      withBytes: src.baseAddress!, bytesPerRow: w * 4)
        }
        let got1 = readTex(t, false)
        let (bad1, mv1) = compareRows(got1, want, w, h)
        // let the GPU go idle, then read the SAME texture again - the user's
        // unfocused/refocused observation says a later read can differ.
        Thread.sleep(forTimeInterval: 2.0)
        let got2 = readTex(t, false)
        let (bad2, mv2) = compareRows(got2, want, w, h)
        let same = got1 == got2
        print(String(format: "  %-22s w=%4d  rows-wrong now=%2d later=%2d  stable=%@",
                     (p.name as NSString).utf8String!, w, bad1, bad2, same ? "YES" : "NO"))
        if bad1 > 0 {
            print("      now:   \(mv1.joined(separator: " "))")
            // rule 16: the instrument is a suspect. The synthetic matrix says
            // this very path round-trips exactly, so before reading a glyph
            // mismatch as a hardware result, show the actual pixels.
            var shown = 0
            for i in 0..<(w * h) where got1[i] != want[i] && shown < 6 {
                print(String(format: "      px (%d,%d) got=%08x want=%08x",
                             i % w, i / w, got1[i], want[i]))
                shown += 1
            }
        }
        if bad2 > 0 && !same { print("      later: \(mv2.joined(separator: " "))") }
    }
}

// ---------------------------------------------------------------------------
// 2mh: the entropy ladder.
//
// 2mg found the defect is content-dependent, not layout-dependent: the SAME
// path (`managed-replace-full`, w=256) round-trips a maximum-entropy
// synthetic pattern exactly and mangles a CoreText glyph bitmap, returning
// `00000000` where `ff000000` was written - absent content, not displaced
// content. The glyph bitmap's distinguishing property is that it is
// overwhelmingly ONE repeated value, i.e. highly compressible.
//
// This walks content from perfectly uniform to pure noise through the same
// path, with `iosurface` beside it as the control that 2mg showed clean.
// If the failure tracks compressibility the defect is colour compression
// (DCC) metadata, which the kernel can reach - unlike userspace layout
// (2lt). If it tracks something else, this says what.
func fill(_ w: Int, _ h: Int, _ f: (Int, Int) -> UInt32) -> [UInt32] {
    var a = [UInt32](repeating: 0, count: w * h)
    for y in 0..<h { for x in 0..<w { a[y * w + x] = f(x, y) } }
    return a
}

var rng: UInt32 = 0x12345678
func rnd() -> UInt32 {                      // deterministic, so runs compare
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng
}

let ladder: [(String, (Int, Int) -> UInt32)] = [
    ("solid-black",   { _, _ in 0xff000000 }),
    ("solid-white",   { _, _ in 0xffffffff }),
    ("two-halves",    { x, _ in x < 128 ? 0xff000000 : 0xffffffff }),
    ("blocks-16",     { x, y in ((x >> 4) ^ (y >> 4)) & 1 == 0 ? 0xff000000 : 0xffffffff }),
    ("blocks-4",      { x, y in ((x >> 2) ^ (y >> 2)) & 1 == 0 ? 0xff000000 : 0xffffffff }),
    ("checker-1",     { x, y in (x ^ y) & 1 == 0 ? 0xff000000 : 0xffffffff }),
    ("lowbit-noise",  { _, _ in 0xff000000 | (rnd() & 0x0f0f0f) }),
    ("full-noise",    { _, _ in 0xff000000 | (rnd() & 0xffffff) }),
    ("xy-pattern",    { x, y in 0xA5000000 | (UInt32(y) << 12) | UInt32(x) }),
]

print("")
print("=== 2mh: entropy ladder (w=256 h=48, bgra8) ===")
let lw = 256, lh = 48
for (name, gen) in ladder {
    rng = 0x12345678
    let want = fill(lw, lh, gen)
    var line = String(format: "  %-14s", (name as NSString).utf8String!)
    for p in paths where p.name == "managed-replace-full" || p.name == "iosurface" {
        guard let (t, _) = p.make(lw, lh, false) else { line += "  \(p.name)=UNSUP"; continue }
        want.withUnsafeBytes { src in
            t.replace(region: MTLRegionMake2D(0, 0, lw, lh), mipmapLevel: 0,
                      withBytes: src.baseAddress!, bytesPerRow: lw * 4)
        }
        let got = readTex(t, false)
        let wrong = (0..<(lw * lh)).reduce(0) { $0 + (got[$1] != want[$1] ? 1 : 0) }
        let zeros = (0..<(lw * lh)).reduce(0) { $0 + (got[$1] == 0 && want[$1] != 0 ? 1 : 0) }
        let tag = p.name == "iosurface" ? "iosurf" : "replace"
        line += String(format: "  %@=%5d/%d(z%d)", tag, wrong, lw * lh, zeros)
    }
    print(line)
}

// 2mi: the ladder refuted the compressibility reading - `full-noise` fails
// while `xy-pattern` (equally high entropy) passes, and `solid-black`
// passes while `solid-white` fails. Neither compressibility nor
// displacement explains that. The two passing cases differ from the failing
// ones in their CONSTANT bits, so vary one channel at a time.
print("")
print("=== 2mi: single-channel sweep (w=256 h=48, managed-replace-full) ===")
let probes: [(String, UInt32)] = [
    ("A=ff RGB=000000", 0xff000000), ("A=ff RGB=000001", 0xff000001),
    ("A=ff RGB=0000ff", 0xff0000ff), ("A=ff RGB=00ff00", 0xff00ff00),
    ("A=ff RGB=ff0000", 0xffff0000), ("A=ff RGB=ffffff", 0xffffffff),
    ("A=00 RGB=000000", 0x00000000), ("A=01 RGB=000000", 0x01000000),
    ("A=7f RGB=000000", 0x7f000000), ("A=a5 RGB=000000", 0xa5000000),
    ("A=fe RGB=000000", 0xfe000000), ("A=a5 RGB=ffffff", 0xa5ffffff),
]
for (name, v) in probes {
    let want = fill(lw, lh, { _, _ in v })
    guard let p = paths.first(where: { $0.name == "managed-replace-full" }),
          let (t, _) = p.make(lw, lh, false) else { continue }
    want.withUnsafeBytes { src in
        t.replace(region: MTLRegionMake2D(0, 0, lw, lh), mipmapLevel: 0,
                  withBytes: src.baseAddress!, bytesPerRow: lw * 4)
    }
    let got = readTex(t, false)
    let wrong = (0..<(lw * lh)).reduce(0) { $0 + (got[$1] != v ? 1 : 0) }
    var sample: UInt32 = v
    for i in 0..<(lw * lh) where got[i] != v { sample = got[i]; break }
    print(String(format: "  %-18s wrong=%5d/%d   first-bad=%08x",
                 (name as NSString).utf8String!, wrong, lw * lh, sample))
}

// 2mj: repeatability. 2mh and 2mi disagreed on IDENTICAL inputs inside one
// process run - `solid-black` 0 wrong in one table, 4096 in the next - so
// neither table measured content. Rule 18 says do not build a table from
// n=1 rows; two were. Run one value many times and report the spread before
// any reading is attached to any of it.
print("")
print("=== 2mj: repeatability, same value 20x (w=256 h=48) ===")
for (name, v) in [("0xff000000", UInt32(0xff000000)), ("0xffffffff", UInt32(0xffffffff))] {
    var hist: [Int: Int] = [:]
    for _ in 0..<20 {
        let want = fill(lw, lh, { _, _ in v })
        guard let p = paths.first(where: { $0.name == "managed-replace-full" }),
              let (t, _) = p.make(lw, lh, false) else { continue }
        want.withUnsafeBytes { src in
            t.replace(region: MTLRegionMake2D(0, 0, lw, lh), mipmapLevel: 0,
                      withBytes: src.baseAddress!, bytesPerRow: lw * 4)
        }
        let got = readTex(t, false)
        let wrong = (0..<(lw * lh)).reduce(0) { $0 + (got[$1] != v ? 1 : 0) }
        hist[wrong, default: 0] += 1
    }
    let keys = hist.keys.sorted()
    print("  \(name): " + keys.map { "\($0)x\(hist[$0]!)" }.joined(separator: "  "))
}
// Does a second read of the SAME texture agree with the first?
print("  same-texture re-read agreement:")
for trial in 0..<5 {
    let v: UInt32 = 0xffffffff
    let want = fill(lw, lh, { _, _ in v })
    guard let p = paths.first(where: { $0.name == "managed-replace-full" }),
          let (t, _) = p.make(lw, lh, false) else { continue }
    want.withUnsafeBytes { src in
        t.replace(region: MTLRegionMake2D(0, 0, lw, lh), mipmapLevel: 0,
                  withBytes: src.baseAddress!, bytesPerRow: lw * 4)
    }
    let a = readTex(t, false), b = readTex(t, false), c = readTex(t, false)
    let wa = (0..<(lw * lh)).reduce(0) { $0 + (a[$1] != v ? 1 : 0) }
    let wb = (0..<(lw * lh)).reduce(0) { $0 + (b[$1] != v ? 1 : 0) }
    let wc = (0..<(lw * lh)).reduce(0) { $0 + (c[$1] != v ? 1 : 0) }
    print(String(format: "    trial %d: read1=%5d read2=%5d read3=%5d  %@",
                 trial, wa, wb, wc, (a == b && b == c) ? "identical" : "DIFFER"))
}

// ---------------------------------------------------------------- 2mk
// 2mj established that the outcome is fixed at upload time, per allocation,
// and then stable. Three questions follow, none of which need a boot.
//   1. Does the upload RACE? Re-read is stable, but the upload itself may
//      not have landed when the read was encoded.
//   2. Does the outcome track the ALLOCATION ADDRESS? Placement heaps give
//      a controlled offset.
//   3. Does the 16-row quantum track SURFACE HEIGHT, or is it 16 KB?
// Every row below is n>=12, because rule 18 was broken twice in 2mg.

// Which rows are wrong, as ranges, so the shape is visible and not just a
// count. A count of 4096 could be 16 contiguous rows or 4096 scattered px.
func badRows(_ got: [UInt32], _ w: Int, _ h: Int, _ v: UInt32) -> [(Int, Int)] {
    var rows: [Int] = []
    for y in 0..<h {
        var bad = false
        for x in 0..<w where got[y * w + x] != v { bad = true; break }
        if bad { rows.append(y) }
    }
    var out: [(Int, Int)] = []
    for y in rows {
        if let l = out.last, l.1 + 1 == y { out[out.count - 1].1 = y } else { out.append((y, y)) }
    }
    return out
}

func rowsStr(_ r: [(Int, Int)]) -> String {
    r.isEmpty ? "-" : r.map { $0.0 == $0.1 ? "\($0.0)" : "\($0.0)-\($0.1)" }.joined(separator: ",")
}

enum Settle { case none, blit, wait, both, twice, pre }

// One upload of a constant value into a fresh managed texture, with the
// chosen settling step between replace() and the read. Returns wrong px
// and the wrong-row ranges.
func managedTrial(_ w: Int, _ h: Int, _ v: UInt32, _ s: Settle,
                  heap: MTLHeap? = nil, offset: Int = 0) -> (Int, [(Int, Int)]) {
    let d = desc(.bgra8Unorm, w, h, heap == nil ? .managed : .shared)
    var t: MTLTexture?
    if let hp = heap { t = hp.makeTexture(descriptor: d, offset: offset) } else { t = dev.makeTexture(descriptor: d) }
    guard let tex = t else { return (-1, []) }
    if s == .pre {                              // settle BEFORE the upload
        let cb = q.makeCommandBuffer()!; cb.commit(); cb.waitUntilCompleted()
    }
    let want = fill(w, h, { _, _ in v })
    let put = { want.withUnsafeBytes { src in
        tex.replace(region: MTLRegionMake2D(0, 0, w, h), mipmapLevel: 0,
                    withBytes: src.baseAddress!, bytesPerRow: w * 4) } }
    put()
    if s == .twice { put() }
    if s == .blit || s == .both {
        let cb = q.makeCommandBuffer()!, b = cb.makeBlitCommandEncoder()!
        b.synchronize(resource: tex)
        b.endEncoding(); cb.commit(); cb.waitUntilCompleted()
    }
    if s == .wait || s == .both { usleep(50_000) }
    let got = readTex(tex, false)
    let wrong = (0..<(w * h)).reduce(0) { $0 + (got[$1] != v ? 1 : 0) }
    return (wrong, badRows(got, w, h, v))
}

func hist(_ xs: [Int]) -> String {
    var h: [Int: Int] = [:]
    for x in xs { h[x, default: 0] += 1 }
    return h.keys.sorted().map { "\($0)x\(h[$0]!)" }.joined(separator: "  ")
}

print("")
print("=== 2mk-1: does the upload race? (w=256 h=48, 0xffffffff, n=16) ===")
for (nm, s) in [("replace -> read", Settle.none),
                ("replace -> synchronize+wait", Settle.blit),
                ("replace -> 50ms sleep", Settle.wait),
                ("replace -> sync + 50ms", Settle.both),
                ("replace twice -> read", Settle.twice),
                ("idle cb -> replace -> read", Settle.pre)] {
    var ws: [Int] = [], shapes: [String] = []
    for _ in 0..<16 {
        let (wrong, rows) = managedTrial(256, 48, 0xffffffff, s)
        ws.append(wrong); if wrong > 0 { shapes.append(rowsStr(rows)) }
    }
    print("  " + nm.padding(toLength: 28, withPad: " ", startingAt: 0) + hist(ws))
    if !shapes.isEmpty {
        print("      rows: " + Set(shapes).sorted().prefix(4).joined(separator: " | "))
    }
}

print("")
print("=== 2mk-3: does the quantum track height? (0xffffffff, n=16 each) ===")
print("  w=256, so one row = 1 KB. 16 rows = 16 KB.")
for h in [16, 32, 48, 64, 96, 128, 192] {
    var ws: [Int] = [], shapes: [String] = []
    for _ in 0..<16 {
        let (wrong, rows) = managedTrial(256, h, 0xffffffff, .none)
        ws.append(wrong / 256); if wrong > 0 { shapes.append(rowsStr(rows)) }
    }
    print("  h=\(String(h).padding(toLength: 4, withPad: " ", startingAt: 0)) wrong-rows: "
          + hist(ws).padding(toLength: 34, withPad: " ", startingAt: 0)
          + "shapes: " + Set(shapes).sorted().prefix(3).joined(separator: " | "))
}

print("")
print("=== 2mk-2: allocation-offset sweep - NOT POSSIBLE on this device ===")
do {
    let sa = dev.heapTextureSizeAndAlign(descriptor: desc(.bgra8Unorm, 256, 48, .private))
    print("  heapTextureSizeAndAlign(256x48 bgra8): size=\(sa.size) align=\(sa.align)")
    print("  A placement heap here rejects BOTH .managed (\"Invalid storageMode\") and")
    print("  .shared (\"Shared storage mode disallowed\"); only .private is accepted, and a")
    print("  .private texture cannot take the CPU replace() that is the failing path.")
    print("  So the offset of the failing allocation cannot be controlled from here.")
    print("  2mk-1 gives the same answer another way: all six settle variants returned")
    print("  IDENTICAL histograms, so the outcome is fixed by the allocation sequence.")
}

// ---------------------------------------------------------------- 2ml
// nullarc (2mk) found, on this same memory topology - an APU whose VRAM is a
// carve-out of system RAM - that the carve-out's mapping "maps write-back
// whatever cache mode it asks for, and plain stores left the frame in the CPU
// cache while the panel scanned the stale DRAM under it, in stripes". If
// replace() leaves its bytes in the CPU's cache and the GPU reads the stale
// DRAM beneath, every 2mj property follows.
//
// The failing path (managed-replace-full) gives no CPU pointer, so clflush is
// out. Instead evict the whole cache by touching a buffer far larger than it,
// between the upload and the read. If the dirty lines are the problem, the
// failure rate must collapse.
//
// Thrashing also takes time, so a sleep of the SAME measured duration is the
// control. Without it this test cannot tell eviction from delay.

let thrashBytes = 128 << 20                    // >> any L2/L3 on this part
var thrashBuf = [UInt8](repeating: 1, count: thrashBytes)

@inline(never) func thrashWrite() {
    thrashBuf.withUnsafeMutableBytes { p in
        let n = p.count, q = p.baseAddress!.assumingMemoryBound(to: UInt64.self)
        for i in stride(from: 0, to: n / 8, by: 8) { q[i] = q[i] &+ 1 }   // one store per 64 B line
    }
}
@inline(never) func thrashRead() -> UInt64 {
    var s: UInt64 = 0
    thrashBuf.withUnsafeBytes { p in
        let n = p.count, q = p.baseAddress!.assumingMemoryBound(to: UInt64.self)
        for i in stride(from: 0, to: n / 8, by: 8) { s = s &+ q[i] }
    }
    return s
}

func now() -> Double { Double(DispatchTime.now().uptimeNanoseconds) / 1e9 }

enum Evict { case none, write, read, sleepEq }

var sinkG: UInt64 = 0
var thrashSecs = 0.0

func evictTrial(_ w: Int, _ h: Int, _ v: UInt32, _ e: Evict) -> (Int, [(Int, Int)]) {
    guard let t = dev.makeTexture(descriptor: desc(.bgra8Unorm, w, h, .managed)) else { return (-1, []) }
    let want = fill(w, h, { _, _ in v })
    want.withUnsafeBytes { src in
        t.replace(region: MTLRegionMake2D(0, 0, w, h), mipmapLevel: 0,
                  withBytes: src.baseAddress!, bytesPerRow: w * 4)
    }
    switch e {
    case .none:    break
    case .write:   let t0 = now(); thrashWrite();          thrashSecs = now() - t0
    case .read:    let t0 = now(); sinkG &+= thrashRead(); thrashSecs = now() - t0
    case .sleepEq: usleep(UInt32(max(1.0, thrashSecs * 1e6)))
    }
    let got = readTex(t, false)
    let wrong = (0..<(w * h)).reduce(0) { $0 + (got[$1] != v ? 1 : 0) }
    return (wrong, badRows(got, w, h, v))
}

print("")
print("=== 2ml: does evicting the CPU cache fix the upload? (w=256 h=48, n=24) ===")
print("  thrash buffer \(thrashBytes >> 20) MB; sleep-equal is the time-only control")
for (nm, e) in [("baseline (nothing)", Evict.none),
                ("evict by WRITING 128 MB", Evict.write),
                ("evict by READING 128 MB", Evict.read),
                ("sleep, same duration", Evict.sleepEq)] {
    var ws: [Int] = [], shapes: [String] = []
    for _ in 0..<24 {
        let (wrong, rows) = evictTrial(256, 48, 0xffffffff, e)
        ws.append(wrong / 256); if wrong > 0 { shapes.append(rowsStr(rows)) }
    }
    let failed = ws.filter { $0 > 0 }.count
    print("  " + nm.padding(toLength: 26, withPad: " ", startingAt: 0)
          + "fail \(failed)/24  wrong-rows: " + hist(ws).padding(toLength: 26, withPad: " ", startingAt: 0)
          + (shapes.isEmpty ? "" : "bands: " + Set(shapes).sorted().prefix(2).joined(separator: " | ")))
    if e == .write || e == .read { print(String(format: "      (thrash took %.1f ms)", thrashSecs * 1e3)) }
}

// Where WE own the backing store, both sides can be compared: write with plain
// CPU stores, then read the same bytes with the GPU and with the CPU. CPU
// correct while the GPU sees whole bands wrong is a coherency gap and nothing
// else. Both wrong would rule it out.
print("")
print("=== 2ml-b: CPU view vs GPU view of one buffer-backed texture (n=24) ===")
do {
    let f = MTLPixelFormat.bgra8Unorm
    let al = dev.minimumLinearTextureAlignment(for: f)
    let w = 256, h = 48, bpr = (w * 4 + al - 1) / al * al
    var gpuFail = 0, cpuFail = 0, both = 0, disagree = 0
    var ex = ""
    for i in 0..<24 {
        let v: UInt32 = 0xff000000 | UInt32(i * 7 + 1)
        guard let buf = dev.makeBuffer(length: bpr * h, options: .storageModeManaged),
              let t = buf.makeTexture(descriptor: desc(f, w, h, .managed), offset: 0, bytesPerRow: bpr)
        else { continue }
        let p = buf.contents().assumingMemoryBound(to: UInt32.self)
        for y in 0..<h { let row = p + y * (bpr / 4); for x in 0..<w { row[x] = v } }
        buf.didModifyRange(0..<buf.length)
        let got = readTex(t, false)                            // GPU's view
        let gWrong = (0..<(w * h)).reduce(0) { $0 + (got[$1] != v ? 1 : 0) }
        var cWrong = 0                                         // CPU's own view
        for y in 0..<h { let row = p + y * (bpr / 4); for x in 0..<w where row[x] != v { cWrong += 1 } }
        if gWrong > 0 { gpuFail += 1 }
        if cWrong > 0 { cpuFail += 1 }
        if gWrong > 0 && cWrong > 0 { both += 1 }
        if (gWrong > 0) != (cWrong > 0) {
            disagree += 1
            if ex.isEmpty { ex = "trial \(i): gpu=\(gWrong) cpu=\(cWrong) bands=\(rowsStr(badRows(got, w, h, v)))" }
        }
    }
    print("  GPU view wrong: \(gpuFail)/24   CPU view wrong: \(cpuFail)/24   both: \(both)   disagreed: \(disagree)")
    if !ex.isEmpty { print("  first disagreement -> " + ex) }
    if gpuFail == 0 && cpuFail == 0 { print("  buffer-backed linear is clean on both sides - the defect needs the driver's own allocation") }
}

// ---------------------------------------------------------------- 2mm
// 2mk-3 says the failing heights are not arbitrary: h=16, 32 and 128 were
// clean 16 times out of 16, while 48, 96 and 192 failed in bands that are
// exactly a THIRD of the surface (16, 32 and 64 rows) and 64 failed whole.
// And heapTextureSizeAndAlign put a 256x48 BGRA texture - 48 KB of pixels -
// in 128 KB with 64 KB alignment, which is a tiled footprint, not a linear
// one, even though the kernel is forcing LINEAR this boot (-rbgpulinear,
// rbgpu-surf forced=38 lin=1).
//
// So ask the driver what footprint it intends for each geometry and line that
// up against which geometries fail. If the clean heights are the ones whose
// footprint equals w*h*4 and the failing ones are padded, the defect is a
// DISAGREEMENT between the client's layout and the kernel's, not a bug in
// either alone.
print("")
print("=== 2mm: the driver's intended footprint per geometry ===")
print("  w     h    pixels    footprint   align   ratio  padded?")
for (w, h) in [(256, 16), (256, 32), (256, 48), (256, 64), (256, 96), (256, 128), (256, 192),
               (256, 256), (128, 48), (512, 48), (64, 64), (1024, 48)] {
    let px = w * h * 4
    let sa = dev.heapTextureSizeAndAlign(descriptor: desc(.bgra8Unorm, w, h, .shared))
    let r = Double(sa.size) / Double(px)
    print(String(format: "  %-5d %-4d %8d  %9d  %6d  %5.2fx  %@",
                 w, h, px, sa.size, sa.align, r, sa.size == px ? "no (linear)" : "YES"))
}
