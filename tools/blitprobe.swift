// blitprobe.swift (2mx) — is a GPU blit into a 64 KB-TILED PRIVATE texture correct?
//
// Why this exists. 2mw established that the assistant's content is GPU-produced into
// MTLStorageModePrivate textures and that it issues zero CPU uploads, which
// killed the replaceRegion line of work. The inventory it printed also contains
//
//     2048x2048 fmt=10 usage=0x1 type=2 storage=2 align=65536 size/px=1.00
//
// an R8, SHADER-READ-ONLY, private, 64 KB-aligned texture: Skia's glyph atlas.
// Shader-read-only means no render pass can fill it. Private means no CPU
// upload can. The only way bytes get in is a blit from a staging buffer.
//
// texprobe already has private-blit paths and they came back clean - but they
// ran at w in {37,256,1000}, h=48, and the inventory shows textures that small
// land at align=256 or align=4096. So "the blit is clean" may be a statement
// about an UNTILED destination and say nothing about the tiled one. Rule 22:
// an instrument is not evidence until it has been checked against a case known
// to be present. Hence the `managed-replace` control below, which the law of
// 2mn says MUST fail on the flagged geometries; if it comes back OK the
// detector is broken and nothing else in the run means anything.
//
// Geometries are the assistant's own, copied from logs/iosurftex-the assistant.log.
//
// build: swiftc -O tools/blitprobe.swift -o build/out/blitprobe -framework Metal
import Foundation
import Metal

setvbuf(stdout, nil, _IONBF, 0)

// Rule 14: absence is not a result.
guard let dev = MTLCreateSystemDefaultDevice() else {
    print("NO METAL DEVICE - this boot has no accelerator.")
    print("  Almost certainly an unarmed boot. Check:")
    print("    nvram -p | grep rbgpu-boot     # must have increased since the last capture")
    print("  Arm it with:  sudo nvram rbgpu-arm=1 && sudo reboot")
    exit(2)
}
guard let q = dev.makeCommandQueue() else {
    print("device:", dev.name, "- but makeCommandQueue() returned nil"); exit(2)
}
print("device:", dev.name)
print("")

// --- readback: a COMPUTE kernel, never a blit -------------------------------
// The write paths under test are blits. Reading back with a blit would let a
// wrong forward mapping cancel against a wrong inverse one and report OK.
// A texture fetch in a shader is an independent path.
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
    return Array(UnsafeBufferPointer(start: out.contents().bindMemory(to: UInt32.self,
                                                                     capacity: w * h), count: w * h))
}

// --- patterns ---------------------------------------------------------------
// BGRA carries its own (x,y) in one pixel, so a wrong-but-valid pixel names
// where it came from. R8 has only 256 values and cannot, so it is measured in
// two passes - one whose value is x&255, one whose value is y&255 - and the
// pair recovers the source coordinate mod 256. That is enough to read off a
// displacement, which is the whole question here.
enum Pat { case loc, xr, yr }

@inline(__always) func value(_ x: Int, _ y: Int, _ p: Pat, _ tag: UInt32) -> UInt32 {
    switch p {
    case .loc: return (tag << 24) | (UInt32(y) << 12) | UInt32(x)
    case .xr:  return UInt32(x & 0xff)
    case .yr:  return UInt32(y & 0xff)
    }
}

func writeRect(_ dst: UnsafeMutableRawPointer, _ bpr: Int, _ rx: Int, _ ry: Int,
               _ rw: Int, _ rh: Int, _ p: Pat, _ tag: UInt32, _ bpe: Int, _ relative: Bool) {
    for y in 0..<rh {
        let row = dst + (relative ? y : ry + y) * bpr + (relative ? 0 : rx * bpe)
        for x in 0..<rw {
            let v = value(rx + x, ry + y, p, tag)
            if bpe == 1 { row.storeBytes(of: UInt8(v), toByteOffset: x, as: UInt8.self) }
            else { row.storeBytes(of: v.littleEndian, toByteOffset: x * 4, as: UInt32.self) }
        }
    }
}

// Sub-rectangles, the way a glyph cache fills an atlas: many small uploads at
// coordinates that do not line up with any tile boundary. Sizes are prime-ish
// and scaled so a 2048-square stays under ~2000 copies.
func glyphRects(_ w: Int, _ h: Int, _ f: (Int, Int, Int, Int) -> Void) {
    let tw = w > 512 ? 61 : 13, th = h > 512 ? 37 : 9
    var y = 0
    while y < h {
        let rh = min(th, h - y); var x = 0
        while x < w { let rw = min(tw, w - x); f(x, y, rw, rh); x += rw }
        y += rh
    }
}

func desc(_ f: MTLPixelFormat, _ w: Int, _ h: Int,
          _ m: MTLStorageMode, _ u: MTLTextureUsage) -> MTLTextureDescriptor {
    let d = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: f, width: w, height: h, mipmapped: false)
    d.storageMode = m; d.usage = u; return d
}

// --- the cases: the assistant's own geometries -------------------------------------
struct Case { let w: Int; let h: Int; let f: MTLPixelFormat; let bpe: Int
              let usage: MTLTextureUsage; let note: String }

let cases: [Case] = [
    // The prize. Shader-read-only + private = fillable only by blit.
    Case(w: 2048, h: 2048, f: .r8Unorm, bpe: 1, usage: [.shaderRead],
         note: "Skia glyph atlas"),
    Case(w: 2048, h: 2048, f: .bgra8Unorm, bpe: 4, usage: [.shaderRead],
         note: "image-upload shape"),
    // The R8 atlases the law flags (size/px ~2.0: height padded to the tile).
    Case(w: 1024, h: 128, f: .r8Unorm, bpe: 1, usage: [.shaderRead, .renderTarget], note: ""),
    Case(w:  512, h: 128, f: .r8Unorm, bpe: 1, usage: [.shaderRead, .renderTarget], note: ""),
    Case(w:  165, h: 165, f: .r8Unorm, bpe: 1, usage: [.shaderRead, .renderTarget], note: ""),
    // BGRA, including the worst over-allocation in the inventory (size/px 5.20).
    Case(w:  320, h: 832, f: .bgra8Unorm, bpe: 4, usage: [.shaderRead, .renderTarget], note: ""),
    Case(w: 1024, h: 128, f: .bgra8Unorm, bpe: 4, usage: [.shaderRead, .renderTarget], note: ""),
    Case(w: 1024, h: 256, f: .bgra8Unorm, bpe: 4, usage: [.shaderRead, .renderTarget], note: ""),
    // Untiled controls: the inventory shows these land at align 4096 / 256.
    Case(w:   64, h:  64, f: .r8Unorm, bpe: 1, usage: [.shaderRead, .renderTarget],
         note: "untiled control"),
    Case(w:   32, h:  16, f: .r8Unorm, bpe: 1, usage: [.shaderRead, .renderTarget],
         note: "untiled control"),
]

// --- write paths ------------------------------------------------------------
typealias Writer = (Pat, UInt32) -> Void

func blitPath(_ c: Case, glyphs: Bool) -> (MTLTexture, Writer)? {
    let bpr = (c.w * c.bpe + 255) / 256 * 256
    guard let buf = dev.makeBuffer(length: bpr * c.h, options: .storageModeManaged),
          let t = dev.makeTexture(descriptor: desc(c.f, c.w, c.h, .private, c.usage)) else { return nil }
    return (t, { p, tag in
        writeRect(buf.contents(), bpr, 0, 0, c.w, c.h, p, tag, c.bpe, false)
        buf.didModifyRange(0..<buf.length)
        let cb = q.makeCommandBuffer()!, b = cb.makeBlitCommandEncoder()!
        if glyphs {
            glyphRects(c.w, c.h) { rx, ry, rw, rh in
                b.copy(from: buf, sourceOffset: ry * bpr + rx * c.bpe, sourceBytesPerRow: bpr,
                       sourceBytesPerImage: bpr * rh, sourceSize: MTLSizeMake(rw, rh, 1),
                       to: t, destinationSlice: 0, destinationLevel: 0,
                       destinationOrigin: MTLOriginMake(rx, ry, 0))
            }
        } else {
            b.copy(from: buf, sourceOffset: 0, sourceBytesPerRow: bpr,
                   sourceBytesPerImage: bpr * c.h, sourceSize: MTLSizeMake(c.w, c.h, 1),
                   to: t, destinationSlice: 0, destinationLevel: 0,
                   destinationOrigin: MTLOriginMake(0, 0, 0))
        }
        b.endEncoding(); cb.commit(); cb.waitUntilCompleted()
    })
}

// texture -> texture. The source is a BUFFER-BACKED texture, which is linear by
// construction, so only the destination's tiling is under test.
func tex2texPath(_ c: Case) -> (MTLTexture, Writer)? {
    let al = dev.minimumLinearTextureAlignment(for: c.f)
    let bpr = (c.w * c.bpe + al - 1) / al * al
    guard let buf = dev.makeBuffer(length: bpr * c.h, options: .storageModeManaged),
          let s = buf.makeTexture(descriptor: desc(c.f, c.w, c.h, .managed, [.shaderRead]),
                                  offset: 0, bytesPerRow: bpr),
          let t = dev.makeTexture(descriptor: desc(c.f, c.w, c.h, .private, c.usage)) else { return nil }
    return (t, { p, tag in
        writeRect(buf.contents(), bpr, 0, 0, c.w, c.h, p, tag, c.bpe, false)
        buf.didModifyRange(0..<buf.length)
        let cb = q.makeCommandBuffer()!, b = cb.makeBlitCommandEncoder()!
        b.copy(from: s, sourceSlice: 0, sourceLevel: 0, sourceOrigin: MTLOriginMake(0, 0, 0),
               sourceSize: MTLSizeMake(c.w, c.h, 1), to: t, destinationSlice: 0,
               destinationLevel: 0, destinationOrigin: MTLOriginMake(0, 0, 0))
        b.endEncoding(); cb.commit(); cb.waitUntilCompleted()
    })
}

// The control. The law of 2mn says this MUST be wrong wherever FLAGGED is
// printed. If it is not, the detector is broken and the run proves nothing.
func replacePath(_ c: Case) -> (MTLTexture, Writer)? {
    guard let t = dev.makeTexture(descriptor: desc(c.f, c.w, c.h, .managed, c.usage)) else { return nil }
    return (t, { p, tag in
        var a = [UInt8](repeating: 0, count: c.w * c.h * c.bpe)
        a.withUnsafeMutableBytes {
            writeRect($0.baseAddress!, c.w * c.bpe, 0, 0, c.w, c.h, p, tag, c.bpe, false)
            t.replace(region: MTLRegionMake2D(0, 0, c.w, c.h), mipmapLevel: 0,
                      withBytes: $0.baseAddress!, bytesPerRow: c.w * c.bpe)
        }
    })
}

// --- scoring ----------------------------------------------------------------
@inline(__always) func signed8(_ d: Int) -> Int { let m = d & 0xff; return m > 127 ? m - 256 : m }

func report(_ label: String, _ bad: Int, _ total: Int, _ first: (Int, Int)?,
            _ deltas: [String: Int]) {
    var line = label.padding(toLength: 52, withPad: " ", startingAt: 0)
    if bad == 0 { print(line + "OK"); return }
    let top = deltas.sorted { $0.value > $1.value }.prefix(4)
                    .map { "d(\($0.key))x\($0.value)" }.joined(separator: " ")
    line += "BAD \(bad)/\(total)"
    if let f = first { line += " first=(\(f.0),\(f.1))" }
    print(line + "  " + top)
}

func scoreBGRA(_ label: String, _ t: MTLTexture, _ write: Writer, _ c: Case) {
    write(.loc, 0xA5)
    let got = readTex(t, false)
    var bad = 0, first: (Int, Int)? = nil, deltas: [String: Int] = [:]
    for y in 0..<c.h { for x in 0..<c.w {
        let g = got[y * c.w + x]
        if g == value(x, y, .loc, 0xA5) { continue }
        bad += 1; if first == nil { first = (x, y) }
        if g >> 24 == 0xA5 {
            deltas["\(Int(g & 0xfff) - x),\(Int((g >> 12) & 0xfff) - y)", default: 0] += 1
        } else { deltas["junk", default: 0] += 1 }
    }}
    report(label, bad, c.w * c.h, first, deltas)
}

func scoreR8(_ label: String, _ t: MTLTexture, _ write: Writer, _ c: Case) {
    write(.xr, 0); let gx = readTex(t, true)
    write(.yr, 0); let gy = readTex(t, true)
    var bad = 0, first: (Int, Int)? = nil, deltas: [String: Int] = [:]
    for y in 0..<c.h { for x in 0..<c.w {
        let ax = Int(gx[y * c.w + x]), ay = Int(gy[y * c.w + x])
        if ax == (x & 0xff) && ay == (y & 0xff) { continue }
        bad += 1; if first == nil { first = (x, y) }
        deltas["\(signed8(ax - x)),\(signed8(ay - y))", default: 0] += 1
    }}
    report(label, bad, c.w * c.h, first, deltas)
}

// --- run --------------------------------------------------------------------
for c in cases {
    let d = desc(c.f, c.w, c.h, .private, c.usage)
    let sa = dev.heapTextureSizeAndAlign(descriptor: d)
    let px = c.w * c.h * c.bpe
    let flagged = sa.align == 65536 && sa.size > px
    let fname = c.bpe == 1 ? "r8" : "bgra8"
    print(String(format: "%@ %dx%d usage=0x%x  align=%lu  size/px=%.2f  %@%@",
                 fname, c.w, c.h, c.usage.rawValue, sa.align,
                 Double(sa.size) / Double(px),
                 flagged ? "FLAGGED" : "clean",
                 c.note.isEmpty ? "" : "  (\(c.note))"))

    let paths: [(String, (MTLTexture, Writer)?)] = [
        ("blit-full",        blitPath(c, glyphs: false)),
        ("blit-glyphrects",  blitPath(c, glyphs: true)),
        ("tex2tex",          tex2texPath(c)),
        ("CONTROL replace",  replacePath(c)),
    ]
    for (n, made) in paths {
        guard let (t, write) = made else { print("    " + n + ": UNSUPPORTED"); continue }
        if c.bpe == 1 { scoreR8("    " + n, t, write, c) } else { scoreBGRA("    " + n, t, write, c) }
    }
    print("")
}
