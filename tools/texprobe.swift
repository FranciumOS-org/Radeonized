// texprobe.swift — CPU-bitmap -> GPU texture round-trip matrix (no reboot needed, run on an armed boot)
// build: swiftc -O texprobe.swift -o texprobe -framework Metal -framework IOSurface
// Each path is written twice (tag 0xA5, then 0x5A) so stale data from the previous write is detectable.
// BGRA pixels encode their own (x,y), so wrong-but-valid data tells you WHERE it came from.
import Foundation
import Metal
import IOSurface

let dev = MTLCreateSystemDefaultDevice()!
let q = dev.makeCommandQueue()!
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
