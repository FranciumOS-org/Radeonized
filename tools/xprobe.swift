// xprobe.swift (2mx) — CROSS-CONSUMER surface reads.
//
// The blind spot every probe in this repo has shared. texprobe, lawtest,
// rtprobe, blitprobe all write a texture and then read THAT SAME MTLTexture
// object back. If the writer and the reader agree with each other, a layout
// that is consistently wrong still reads back clean. Every "the GPU path is
// clean" result in the journal is of that shape.
//
// The symptom is not of that shape. the assistant's renderer produces a surface and
// the WindowServer composites it - a different process, a different texture
// object wrapping the same IOSurface. Same for Safari: its text is rasterised
// on the CPU into an IOSurface and is crisp, while its in-page images are
// GPU-uploaded and are not. The thing never measured is whether a surface the
// GPU wrote reads the same through a SECOND view of it.
//
// Four directions, with the single-view case kept as a control:
//
//   gpu-write -> gpu-read, same texture      control; 2mu says this is clean
//   gpu-write -> gpu-read, fresh texture     the WindowServer's view
//   gpu-write -> cpu-read, IOSurface bytes   any linear consumer's view
//   cpu-write -> gpu-read, fresh texture     the reverse direction
//
// A failure in rows 2-4 with row 1 clean is a writer/reader layout disagreement
// and would be the first measurement in this session that has the same shape as
// the symptom.
//
// build: swiftc -O tools/xprobe.swift -o build/out/xprobe -framework Metal -framework IOSurface
import Foundation
import Metal
import IOSurface

setvbuf(stdout, nil, _IONBF, 0)

// Rule 14: absence is not a result.
guard let dev = MTLCreateSystemDefaultDevice() else {
    print("NO METAL DEVICE - this boot has no accelerator.")
    print("  Almost certainly an unarmed boot.")
    print("  Arm it with:  sudo nvram rbgpu-arm=1 && sudo reboot")
    exit(2)
}
guard let q = dev.makeCommandQueue() else {
    print("device:", dev.name, "- but makeCommandQueue() returned nil"); exit(2)
}
print("device:", dev.name)
print("")

let TAG: UInt32 = 0xA5

// Each pixel carries its own (x,y) and a tag, so a wrong-but-valid pixel says
// where it came from instead of merely being wrong.
@inline(__always) func want(_ x: Int, _ y: Int) -> UInt32 {
    (TAG << 24) | (UInt32(y) << 12) | UInt32(x)
}

let shaders = """
#include <metal_stdlib>
using namespace metal;

vertex float4 vs(uint vid [[vertex_id]]) {
    // one oversized triangle covering the whole target
    float2 p = float2((vid << 1) & 2, vid & 2);
    return float4(p * 2.0 - 1.0, 0.0, 1.0);
}

fragment float4 fs(float4 pos [[position]]) {
    uint x = uint(pos.x), y = uint(pos.y);
    uint v = (165u << 24) | (y << 12) | x;      // 165 == 0xA5
    return float4(float((v >> 16) & 0xff), float((v >> 8) & 0xff),
                  float(v & 0xff), float((v >> 24) & 0xff)) / 255.0;
}

kernel void rd(texture2d<float, access::read> t [[texture(0)]],
               device uint *o [[buffer(0)]],
               constant uint &w [[buffer(1)]],
               uint2 g [[thread_position_in_grid]]) {
    if (g.x >= t.get_width() || g.y >= t.get_height()) return;
    uint4 c = uint4(round(clamp(t.read(g), 0.0, 1.0) * 255.0));
    o[g.y * w + g.x] = c.z | (c.y << 8) | (c.x << 16) | (c.w << 24);
}
"""
let lib = try! dev.makeLibrary(source: shaders, options: nil)
let rpd = MTLRenderPipelineDescriptor()
rpd.vertexFunction = lib.makeFunction(name: "vs")
rpd.fragmentFunction = lib.makeFunction(name: "fs")
rpd.colorAttachments[0].pixelFormat = .bgra8Unorm
let rps = try! dev.makeRenderPipelineState(descriptor: rpd)
let cps = try! dev.makeComputePipelineState(function: lib.makeFunction(name: "rd")!)

func gpuRender(into t: MTLTexture) {
    let pass = MTLRenderPassDescriptor()
    pass.colorAttachments[0].texture = t
    pass.colorAttachments[0].loadAction = .clear
    pass.colorAttachments[0].storeAction = .store
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0)
    let cb = q.makeCommandBuffer()!
    let e = cb.makeRenderCommandEncoder(descriptor: pass)!
    e.setRenderPipelineState(rps)
    e.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
    e.endEncoding()
    if t.storageMode == .managed {
        let b = cb.makeBlitCommandEncoder()!; b.synchronize(resource: t); b.endEncoding()
    }
    cb.commit(); cb.waitUntilCompleted()
}

func gpuRead(_ t: MTLTexture) -> [UInt32] {
    let w = t.width, h = t.height
    let out = dev.makeBuffer(length: w * h * 4, options: .storageModeShared)!
    memset(out.contents(), 0xEE, w * h * 4)
    var ww = UInt32(w)
    let cb = q.makeCommandBuffer()!, e = cb.makeComputeCommandEncoder()!
    e.setComputePipelineState(cps); e.setTexture(t, index: 0)
    e.setBuffer(out, offset: 0, index: 0); e.setBytes(&ww, length: 4, index: 1)
    e.dispatchThreadgroups(MTLSizeMake((w + 15) / 16, (h + 15) / 16, 1),
                           threadsPerThreadgroup: MTLSizeMake(16, 16, 1))
    e.endEncoding(); cb.commit(); cb.waitUntilCompleted()
    return Array(UnsafeBufferPointer(start: out.contents().bindMemory(to: UInt32.self,
                                                                     capacity: w * h), count: w * h))
}

func cpuRead(_ s: IOSurface, _ w: Int, _ h: Int) -> [UInt32] {
    _ = s.lock(options: [.readOnly], seed: nil)
    defer { _ = s.unlock(options: [.readOnly], seed: nil) }
    let base = s.baseAddress, bpr = s.bytesPerRow
    var o = [UInt32](repeating: 0, count: w * h)
    for y in 0..<h {
        let row = base + y * bpr
        for x in 0..<w { o[y * w + x] = row.load(fromByteOffset: x * 4, as: UInt32.self) }
    }
    return o
}

func cpuWrite(_ s: IOSurface, _ w: Int, _ h: Int) {
    _ = s.lock(options: [], seed: nil)
    defer { _ = s.unlock(options: [], seed: nil) }
    let base = s.baseAddress, bpr = s.bytesPerRow
    for y in 0..<h {
        let row = base + y * bpr
        for x in 0..<w { row.storeBytes(of: want(x, y).littleEndian, toByteOffset: x * 4, as: UInt32.self) }
    }
}

func makeSurface(_ w: Int, _ h: Int) -> IOSurface? {
    let bpr = IOSurfaceAlignProperty(kIOSurfaceBytesPerRow, w * 4)
    return IOSurface(properties: [.width: w, .height: h, .bytesPerElement: 4,
                                  .bytesPerRow: bpr,
                                  .pixelFormat: 0x42475241 /* BGRA */])
}

func wrap(_ s: IOSurface, _ w: Int, _ h: Int) -> MTLTexture? {
    let d = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .bgra8Unorm,
                                                     width: w, height: h, mipmapped: false)
    d.storageMode = .managed; d.usage = [.shaderRead, .renderTarget]
    return dev.makeTexture(descriptor: d, iosurface: s, plane: 0)
}

func score(_ label: String, _ got: [UInt32], _ w: Int, _ h: Int) {
    var bad = 0, first: (Int, Int)? = nil, deltas: [String: Int] = [:]
    for y in 0..<h { for x in 0..<w {
        let g = got[y * w + x]
        if g == want(x, y) { continue }
        bad += 1; if first == nil { first = (x, y) }
        if g >> 24 == TAG {
            deltas["\(Int(g & 0xfff) - x),\(Int((g >> 12) & 0xfff) - y)", default: 0] += 1
        } else { deltas["junk", default: 0] += 1 }
    }}
    var line = label.padding(toLength: 40, withPad: " ", startingAt: 0)
    if bad == 0 { print(line + "OK"); return }
    let top = deltas.sorted { $0.value > $1.value }.prefix(4)
                    .map { "d(\($0.key))x\($0.value)" }.joined(separator: " ")
    line += "BAD \(bad)/\(w*h)"
    if let f = first { line += " first=(\(f.0),\(f.1))" }
    print(line + "  " + top)
}

// the assistant's own geometries, plus a couple of rtprobe's for continuity.
let geoms = [(1024, 128), (1024, 256), (320, 832), (1485, 885), (512, 256), (256, 48)]

for (w, h) in geoms {
    let d = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .bgra8Unorm,
                                                     width: w, height: h, mipmapped: false)
    d.storageMode = .private; d.usage = [.shaderRead, .renderTarget]
    let sa = dev.heapTextureSizeAndAlign(descriptor: d)
    print(String(format: "bgra8 %dx%d  private align=%lu size/px=%.3f  (meta ~%.4f of a plane)",
                 w, h, sa.align, Double(sa.size) / Double(w * h * 4),
                 Double(sa.size) / Double(w * h * 4) / 4.0 - 1.0))

    // 1. control: driver-private texture, one view, write and read by GPU.
    if let t = dev.makeTexture(descriptor: d) {
        gpuRender(into: t)
        score("    gpu-write -> gpu-read  SAME view", gpuRead(t), w, h)
    }

    guard let s = makeSurface(w, h) else { print("    IOSurface UNSUPPORTED\n"); continue }

    // 2. the WindowServer's view: a second MTLTexture over the same IOSurface.
    if let a = wrap(s, w, h) {
        gpuRender(into: a)
        score("    gpu-write -> gpu-read  SAME view (iosurf)", gpuRead(a), w, h)
        if let b = wrap(s, w, h) {
            score("    gpu-write -> gpu-read  FRESH view", gpuRead(b), w, h)
        }
        // 3. any linear consumer: the surface's own bytes.
        score("    gpu-write -> cpu-read  surface bytes", cpuRead(s, w, h), w, h)
    }

    // 4. the reverse direction, which is how Terminal's text gets on screen.
    if let b = wrap(s, w, h) {
        cpuWrite(s, w, h)
        score("    cpu-write -> gpu-read  FRESH view", gpuRead(b), w, h)
    }
    print("")
}
