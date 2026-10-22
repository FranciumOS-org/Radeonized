// rtprobe (2mu) - does the GPU writing a RENDER TARGET land correctly?
//
// Everything measured so far concerns a CPU replace() into a texture. 2ms
// corrected the assistant's glyph atlas, provably and at the right descriptor, and its
// text did not change - so the CPU-upload defect is not what scrambles it.
//
// The split in the user's own reports points elsewhere: Terminal's text is
// readable, the assistant's and Safari's are not. Terminal rasterises text on the CPU
// into a window backing store; the assistant and Safari rasterise on the GPU. So test
// the GPU write path: draw a position-dependent pattern into a texture with a
// render pass, read it back, and see whether the pixels are where they belong.
//
// A clear would be useless here - it writes one value everywhere, so a layout
// disagreement would be invisible. The fragment shader writes a value derived
// from the pixel's own coordinates instead.
import Metal
import IOSurface
setvbuf(stdout, nil, _IONBF, 0)
guard let dev = MTLCreateSystemDefaultDevice() else {
    print("NO METAL DEVICE - unarmed boot? sudo nvram rbgpu-arm=1 && sudo reboot"); exit(2)
}
let q = dev.makeCommandQueue()!

let src = """
#include <metal_stdlib>
using namespace metal;
// A full-screen triangle strip from vertex_id, no buffers.
vertex float4 vs(uint vid [[vertex_id]]) {
    float2 p = float2((vid << 1) & 2, vid & 2) * 2.0 - 1.0;
    return float4(p, 0.0, 1.0);
}
// Writes the pixel's own coordinates: x in the low 12 bits of blue+green,
// y in red, so a displaced row or column is visible in the value itself.
fragment float4 fs(float4 pos [[position]]) {
    uint x = uint(pos.x), y = uint(pos.y);
    return float4(float(y & 255) / 255.0, float((x >> 8) & 255) / 255.0,
                  float(x & 255) / 255.0, 1.0);
}
kernel void rd(texture2d<float, access::read> t [[texture(0)]],
               device uint *o [[buffer(0)]], constant uint2 &p [[buffer(1)]],
               uint2 g [[thread_position_in_grid]]) {
    if (g.x >= t.get_width() || g.y >= t.get_height()) return;
    float4 c = t.read(g);
    o[g.y*p.x+g.x] = (uint(c.w*255.0+0.5)<<24)|(uint(c.r*255.0+0.5)<<16)
                   | (uint(c.g*255.0+0.5)<<8)|uint(c.b*255.0+0.5);
}
"""
let lib = try! dev.makeLibrary(source: src, options: nil)
let rpd = MTLRenderPipelineDescriptor()
rpd.vertexFunction = lib.makeFunction(name: "vs")
rpd.fragmentFunction = lib.makeFunction(name: "fs")
rpd.colorAttachments[0].pixelFormat = .bgra8Unorm
let rps = try! dev.makeRenderPipelineState(descriptor: rpd)
let cps = try! dev.makeComputePipelineState(function: lib.makeFunction(name: "rd")!)

func expected(_ x: Int, _ y: Int) -> UInt32 {
    // ARGB as the compute kernel packs it: A=255, R=y&255, G=(x>>8)&255, B=x&255
    return (255 << 24) | UInt32((y & 255) << 16) | UInt32(((x >> 8) & 255) << 8) | UInt32(x & 255)
}

func readBack(_ t: MTLTexture) -> [UInt32] {
    let w = t.width, h = t.height
    let out = dev.makeBuffer(length: w*h*4, options: .storageModeShared)!
    memset(out.contents(), 0xEE, w*h*4)
    var p = SIMD2<UInt32>(UInt32(w), 0)
    let cb = q.makeCommandBuffer()!, e = cb.makeComputeCommandEncoder()!
    e.setComputePipelineState(cps); e.setTexture(t, index: 0)
    e.setBuffer(out, offset: 0, index: 0); e.setBytes(&p, length: 8, index: 1)
    e.dispatchThreadgroups(MTLSizeMake((w+15)/16,(h+15)/16,1),
                           threadsPerThreadgroup: MTLSizeMake(16,16,1))
    e.endEncoding(); cb.commit(); cb.waitUntilCompleted()
    return Array(UnsafeBufferPointer(start: out.contents().bindMemory(to: UInt32.self,
                                                                     capacity: w*h), count: w*h))
}

func render(into t: MTLTexture) {
    let pass = MTLRenderPassDescriptor()
    pass.colorAttachments[0].texture = t
    pass.colorAttachments[0].loadAction = .clear
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0)
    pass.colorAttachments[0].storeAction = .store
    let cb = q.makeCommandBuffer()!
    let e = cb.makeRenderCommandEncoder(descriptor: pass)!
    e.setRenderPipelineState(rps)
    e.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
    e.endEncoding(); cb.commit(); cb.waitUntilCompleted()
}

func badRowRanges(_ got: [UInt32], _ w: Int, _ h: Int) -> (Int, String) {
    var rows: [Int] = []
    for y in 0..<h {
        for x in 0..<w where got[y*w+x] != expected(x, y) { rows.append(y); break }
    }
    var out: [(Int, Int)] = []
    for y in rows { if let l = out.last, l.1 + 1 == y { out[out.count-1].1 = y } else { out.append((y, y)) } }
    let s = out.isEmpty ? "-" : out.map { $0.0 == $0.1 ? "\($0.0)" : "\($0.0)-\($0.1)" }
                                   .joined(separator: ",")
    return (rows.count, s)
}

func iosurfTexture(_ w: Int, _ h: Int) -> MTLTexture? {
    let bpr = (w * 4 + 255) & ~255
    guard let s = IOSurface(properties: [
        .width: w, .height: h, .bytesPerElement: 4, .bytesPerRow: bpr,
        .pixelFormat: 0x42475241]) else { return nil }
    let d = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .bgra8Unorm,
                                                     width: w, height: h, mipmapped: false)
    d.storageMode = .managed; d.usage = [.shaderRead, .renderTarget]
    return dev.makeTexture(descriptor: d, iosurface: s, plane: 0)
}

func plainTexture(_ w: Int, _ h: Int, _ m: MTLStorageMode) -> MTLTexture? {
    let d = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .bgra8Unorm,
                                                     width: w, height: h, mipmapped: false)
    d.storageMode = m; d.usage = [.shaderRead, .renderTarget]
    return dev.makeTexture(descriptor: d)
}

print("=== 2mu: GPU render into a texture, then read it back (n=6 each)")
print("  the fragment shader writes each pixel's own coordinates, so a")
print("  misplaced row shows up as a wrong value, not just a wrong colour")
print("")
print("  geometry     align  ratio   driver-private  driver-managed  iosurface")
for (w, h) in [(256,48), (256,64), (256,96), (1024,128), (256,128), (256,32), (512,256)] {
    let d = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .bgra8Unorm,
                                                     width: w, height: h, mipmapped: false)
    d.storageMode = .shared; d.usage = [.shaderRead, .renderTarget]
    let sa = dev.heapTextureSizeAndAlign(descriptor: d)
    var cells: [String] = []
    var shapes: [String] = []
    for kind in 0..<3 {
        var fails = 0
        for _ in 0..<6 {
            let t: MTLTexture? = kind == 0 ? plainTexture(w, h, .private)
                               : kind == 1 ? plainTexture(w, h, .managed)
                                           : iosurfTexture(w, h)
            guard let tex = t else { fails = -1; break }
            render(into: tex)
            let (n, s) = badRowRanges(readBack(tex), w, h)
            if n > 0 { fails += 1; if shapes.count < 3 { shapes.append("\(kind):\(s)") } }
        }
        cells.append(fails < 0 ? "   n/a  " : String(format: " %2d/6   ", fails))
    }
    print(String(format: "  %4dx%-4d  %5d  %.2fx  ", w, h, sa.align,
                 Double(sa.size)/Double(w*h*4)) + cells.joined(separator: "       ")
          + (shapes.isEmpty ? "" : "   rows " + shapes.joined(separator: " ")))
}
