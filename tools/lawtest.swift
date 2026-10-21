// lawtest - the 2mn law in one fast instrument.
//
//   a CPU replace() is corrupted iff  align == 65536 AND footprint > pixels
//
// Six geometries, four that the law predicts fail and two it predicts clean,
// n=12 each. Baseline on boot 113 was exactly 36/72, reproducible to the trial
// across six runs, so a change of even one is real. Used to check a knob or an
// interposer in a second and a half.
//
//   make lawtest && ./build/out/lawtest
//   DYLD_INSERT_LIBRARIES=build/out/iosurftex.dylib ./build/out/lawtest
import Metal
setvbuf(stdout, nil, _IONBF, 0)
guard let dev = MTLCreateSystemDefaultDevice() else {
    print("NO METAL DEVICE - unarmed boot? arm with: sudo nvram rbgpu-arm=1 && sudo reboot")
    exit(2)
}
let q = dev.makeCommandQueue()!
let src = """
#include <metal_stdlib>
using namespace metal;
kernel void rd(texture2d<float, access::read> t [[texture(0)]],
               device uint *o [[buffer(0)]], constant uint2 &p [[buffer(1)]],
               uint2 g [[thread_position_in_grid]]) {
    if (g.x >= t.get_width() || g.y >= t.get_height()) return;
    float4 c = t.read(g);
    o[g.y*p.x+g.x] = (uint(c.w*255.0+0.5)<<24)|(uint(c.r*255.0+0.5)<<16)
                   | (uint(c.g*255.0+0.5)<<8)|uint(c.b*255.0+0.5);
}
"""
let pso = try! dev.makeComputePipelineState(
    function: try! dev.makeLibrary(source: src, options: nil).makeFunction(name: "rd")!)

// 2ms: the law and the fix were only ever measured on bgra8Unorm. the assistant's
// glyph atlas is R8Unorm with RenderTarget|ShaderRead, so both the format and
// the usage have to be measured before anything is claimed about them.
var gFormat = MTLPixelFormat.bgra8Unorm
var gBpe = 4
var gUsage: MTLTextureUsage = [.shaderRead]

func trial(_ w: Int, _ h: Int, _ v: UInt32) -> Int {
    let d = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: gFormat,
                                                     width: w, height: h, mipmapped: false)
    d.storageMode = .managed; d.usage = gUsage
    guard let t = dev.makeTexture(descriptor: d) else { return -1 }
    if gBpe == 1 {
        let want = [UInt8](repeating: UInt8(v & 0xff), count: w*h)
        want.withUnsafeBytes { s in
            t.replace(region: MTLRegionMake2D(0,0,w,h), mipmapLevel: 0,
                      withBytes: s.baseAddress!, bytesPerRow: w)
        }
    } else {
        let want = [UInt32](repeating: v, count: w*h)
        want.withUnsafeBytes { s in
            t.replace(region: MTLRegionMake2D(0,0,w,h), mipmapLevel: 0,
                      withBytes: s.baseAddress!, bytesPerRow: w*4)
        }
    }
    let out = dev.makeBuffer(length: w*h*4, options: .storageModeShared)!
    memset(out.contents(), 0xEE, w*h*4)
    var p = SIMD2<UInt32>(UInt32(w), 0)
    let cb = q.makeCommandBuffer()!, e = cb.makeComputeCommandEncoder()!
    e.setComputePipelineState(pso); e.setTexture(t, index: 0)
    e.setBuffer(out, offset: 0, index: 0); e.setBytes(&p, length: 8, index: 1)
    e.dispatchThreadgroups(MTLSizeMake((w+15)/16,(h+15)/16,1),
                           threadsPerThreadgroup: MTLSizeMake(16,16,1))
    e.endEncoding(); cb.commit(); cb.waitUntilCompleted()
    let g = UnsafeBufferPointer(start: out.contents().bindMemory(to: UInt32.self,
                                                                capacity: w*h), count: w*h)
    if gBpe == 1 {
        // The shader returns the red channel in bits 16-23 for an r8 texture.
        let want = (v & 0xff) << 16
        return (0..<(w*h)).reduce(0) { $0 + ((g[$1] & 0x00ff0000) != want ? 1 : 0) }
    }
    return (0..<(w*h)).reduce(0) { $0 + (g[$1] != v ? 1 : 0) }
}

func sweep(_ label: String, _ fmt: MTLPixelFormat, _ bpe: Int, _ usage: MTLTextureUsage,
           _ geoms: [(Int, Int)]) {
    gFormat = fmt; gBpe = bpe; gUsage = usage
    print("")
    print("=== \(label)")
    var fail = 0, runs = 0
    for (w, h) in geoms {
        var fails = 0
        let n = 12
        for i in 0..<n { if trial(w, h, 0xff000000 | UInt32(i*11+1)) > 0 { fails += 1 } }
        fail += fails; runs += n
        let d = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: fmt, width: w,
                                                         height: h, mipmapped: false)
        d.storageMode = .shared; d.usage = usage
        let sa = dev.heapTextureSizeAndAlign(descriptor: d)
        print(String(format: "  %4dx%-4d align %5d ratio %.2fx   fail %2d/%d",
                     w, h, sa.align, Double(sa.size)/Double(w*h*bpe), fails, n))
    }
    print("  subtotal \(fail)/\(runs)")
}

// The atlas geometry the assistant actually asked for, and its neighbours.
sweep("r8Unorm, RenderTarget|ShaderRead (the assistant's glyph atlas shape)",
      .r8Unorm, 1, [.shaderRead, .renderTarget],
      [(1024,128), (1024,48), (512,128), (256,48), (1024,256)])
sweep("r8Unorm, ShaderRead only", .r8Unorm, 1, [.shaderRead],
      [(1024,128), (1024,48), (256,48)])
sweep("bgra8Unorm, RenderTarget|ShaderRead", .bgra8Unorm, 4, [.shaderRead, .renderTarget],
      [(256,48), (256,96)])

// The original bgra8 shaderRead sweep, unchanged, as the stable baseline.
gFormat = .bgra8Unorm; gBpe = 4; gUsage = [.shaderRead]
print("")
print("=== bgra8Unorm, ShaderRead only (the 36/72 baseline)")
var totalFail = 0, totalRuns = 0
for (w, h) in [(256,48), (256,64), (256,96), (1000,48), (256,128), (256,32)] {
    var fails = 0
    let n = 12
    for i in 0..<n { if trial(w, h, 0xff000000 | UInt32(i*11+1)) > 0 { fails += 1 } }
    totalFail += fails; totalRuns += n
    let d = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .bgra8Unorm,
                                                     width: w, height: h, mipmapped: false)
    d.storageMode = .shared; d.usage = [.shaderRead]
    let sa = dev.heapTextureSizeAndAlign(descriptor: d)
    print(String(format: "  %4dx%-4d align %5d ratio %.2fx   fail %2d/%d",
                 w, h, sa.align, Double(sa.size)/Double(w*h*4), fails, n))
}
print("  TOTAL \(totalFail)/\(totalRuns) failed")
