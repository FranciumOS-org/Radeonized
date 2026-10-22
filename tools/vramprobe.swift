// vramprobe.swift (2my) — do two surfaces ever land on the SAME memory?
//
// Why. In the 2mx screen sweep a CPU-drawn 900x880 window came out shredded,
// while the identical window at the identical size was clean in the run before
// it. The only difference was that the other window in the same process had
// allocated a 900x880 4x multisample texture. Allocating one large surface
// corrupted an unrelated one. The desktop on this machine shows the same thing
// in the wild: repeated miniature copies of OTHER windows' content embedded in
// it. Both read as two surfaces sharing memory.
//
// That also explains what has been strange all along - there are zero VM
// faults. An aliased mapping is a perfectly valid mapping; nothing faults, the
// data is just somebody else's.
//
// Every probe before this one used one or two textures at a time, so none of
// them could have seen this. This one fills many surfaces with distinct
// patterns, keeps them all alive, and then checks whether any of them is
// holding another's data.
//
// Each texel is (id << 24) | (y << 12) | x, so a wrong texel names the surface
// it came from AND the coordinate it came from. Filling is done by blit from a
// staging buffer, which 2mx measured as correct at these geometries, so a
// miscompare is about WHERE the surface lives, not how it was written.
//
// build: make vramprobe     run: build/out/vramprobe [budgetMB] [tilesize]
import Foundation
import Metal

setvbuf(stdout, nil, _IONBF, 0)

guard let dev = MTLCreateSystemDefaultDevice() else {
    print("NO METAL DEVICE - this boot has no accelerator (rule 14).")
    print("  Arm it with:  sudo nvram rbgpu-arm=1 && sudo reboot")
    exit(2)
}
guard let q = dev.makeCommandQueue() else { print("no queue"); exit(2) }

let budgetMB = CommandLine.arguments.count > 1 ? Int(CommandLine.arguments[1])! : 256
let SIDE = CommandLine.arguments.count > 2 ? Int(CommandLine.arguments[2])! : 1024
let MSAA = CommandLine.arguments.contains("--msaa")
let MIXED = CommandLine.arguments.contains("--mixed")

// the assistant's own descriptor sizes, from logs/iosurftex-the assistant.log. A uniform
// grid of identical tiles is the easiest possible case for an allocator; a
// real application's spread of sizes is what fragments a free list.
let assistantGeoms: [(Int, Int)] = [
    (2048,2048), (1485,885), (1536,256), (1504,256), (1024,256), (1024,128),
    (1024,64), (896,256), (896,64), (832,128), (768,64), (512,1024), (512,512),
    (512,256), (512,192), (512,128), (512,64), (512,32), (448,128), (320,896),
    (320,832), (320,64), (256,128), (256,64), (256,32), (256,16), (192,64),
    (165,165), (128,128), (128,64), (128,32), (128,16), (64,64), (64,32),
    (64,16), (57,57), (49,49), (33,33), (32,32), (32,16), (27,27), (25,25),
    (16,16), (8,8),
]

print("device:", dev.name)
print(String(format: "recommendedMaxWorkingSetSize %.0f MB, allocating up to %d MB in %dx%d tiles%@",
             Double(dev.recommendedMaxWorkingSetSize) / 1048576.0, budgetMB, SIDE, SIDE,
             MSAA ? ", with a 4x multisample texture alongside each" : ""))

// what each surface will be
var geoms: [(Int, Int)] = []
if MIXED {
    var used = 0
    var i = 0
    while used < budgetMB * 1048576 {
        let g = assistantGeoms[i % assistantGeoms.count]
        geoms.append(g); used += g.0 * g.1 * 4; i += 1
    }
} else {
    let n = max(2, budgetMB * 1048576 / (SIDE * SIDE * 4))
    geoms = Array(repeating: (SIDE, SIDE), count: n)
}
print("surfaces: \(geoms.count)\(MIXED ? " (the assistant's own sizes, cycled)" : "")")
print("")

let src = """
#include <metal_stdlib>
using namespace metal;
kernel void rd(texture2d<float, access::read> t [[texture(0)]],
               device uint *o [[buffer(0)]],
               constant uint &w [[buffer(1)]],
               uint2 g [[thread_position_in_grid]]) {
    if (g.x >= t.get_width() || g.y >= t.get_height()) return;
    uint4 c = uint4(round(clamp(t.read(g), 0.0, 1.0) * 255.0));
    o[g.y * w + g.x] = c.z | (c.y << 8) | (c.x << 16) | (c.w << 24);
}
"""
let lib = try! dev.makeLibrary(source: src, options: nil)
let pso = try! dev.makeComputePipelineState(function: lib.makeFunction(name: "rd")!)

@inline(__always) func want(_ id: Int, _ x: Int, _ y: Int) -> UInt32 {
    (UInt32(id & 0xff) << 24) | (UInt32(y & 0xfff) << 12) | UInt32(x & 0xfff)
}

func desc(_ w: Int, _ h: Int) -> MTLTextureDescriptor {
    let d = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .bgra8Unorm,
                                                     width: w, height: h, mipmapped: false)
    d.storageMode = .private; d.usage = [.shaderRead, .renderTarget]
    return d
}

var texes: [(MTLTexture, Int, Int)] = []
var msaaKeepAlive: [MTLTexture] = []
var msaaAsked = 0, allocFailed = 0

let maxW = geoms.map { $0.0 }.max()!, maxH = geoms.map { $0.1 }.max()!
let maxBpr = (maxW * 4 + 255) / 256 * 256
guard let staging = dev.makeBuffer(length: maxBpr * maxH, options: .storageModeManaged) else {
    print("staging buffer failed"); exit(2)
}

// allocate and fill, keeping every surface alive so nothing can be reused
for (id, g) in geoms.enumerated() {
    let (w, h) = g
    guard let t = dev.makeTexture(descriptor: desc(w, h)) else {
        allocFailed += 1
        if allocFailed == 1 { print("first allocation failure at surface \(id) (\(w)x\(h))") }
        continue
    }
    if MSAA {
        msaaAsked += 1
        let md = MTLTextureDescriptor()
        md.textureType = .type2DMultisample
        md.pixelFormat = .bgra8Unorm
        md.width = w; md.height = h; md.sampleCount = 4
        md.storageMode = .private; md.usage = [.renderTarget]
        if let m = dev.makeTexture(descriptor: md) { msaaKeepAlive.append(m) }
    }
    let bpr = (w * 4 + 255) / 256 * 256
    let base = staging.contents()
    for y in 0..<h {
        let row = base + y * bpr
        for x in 0..<w {
            row.storeBytes(of: want(id, x, y).littleEndian, toByteOffset: x * 4, as: UInt32.self)
        }
    }
    staging.didModifyRange(0..<(bpr * h))
    let cb = q.makeCommandBuffer()!, b = cb.makeBlitCommandEncoder()!
    b.copy(from: staging, sourceOffset: 0, sourceBytesPerRow: bpr, sourceBytesPerImage: bpr * h,
           sourceSize: MTLSizeMake(w, h, 1), to: t, destinationSlice: 0,
           destinationLevel: 0, destinationOrigin: MTLOriginMake(0, 0, 0))
    b.endEncoding(); cb.commit(); cb.waitUntilCompleted()
    texes.append((t, w, h))
}
// Rule 14: absence is not a result. Say what actually got allocated, so a
// silent nil cannot be read later as a clean run.
print("filled \(texes.count) surfaces, all still alive; \(allocFailed) allocations refused")
if MSAA { print("multisample textures: asked \(msaaAsked), got \(msaaKeepAlive.count)") }
print("")

// read every surface back and see whose data is in it
let out = dev.makeBuffer(length: maxBpr * maxH, options: .storageModeShared)!
var totalBad = 0, surfacesBad = 0
for (id, entry) in texes.enumerated() {
    let (t, w, h) = entry
    memset(out.contents(), 0xEE, w * h * 4)
    var ww = UInt32(w)
    let cb = q.makeCommandBuffer()!, e = cb.makeComputeCommandEncoder()!
    e.setComputePipelineState(pso); e.setTexture(t, index: 0)
    e.setBuffer(out, offset: 0, index: 0); e.setBytes(&ww, length: 4, index: 1)
    e.dispatchThreadgroups(MTLSizeMake((w + 15) / 16, (h + 15) / 16, 1),
                           threadsPerThreadgroup: MTLSizeMake(16, 16, 1))
    e.endEncoding(); cb.commit(); cb.waitUntilCompleted()
    let got = UnsafeBufferPointer(start: out.contents().bindMemory(to: UInt32.self,
                                                                  capacity: w * h), count: w * h)
    var bad = 0, owners: [Int: Int] = [:], first: (Int, Int)? = nil
    for y in 0..<h { for x in 0..<w {
        let g = got[y * w + x]
        if g == want(id, x, y) { continue }
        bad += 1; if first == nil { first = (x, y) }
        owners[Int(g >> 24), default: 0] += 1
    }}
    if bad > 0 {
        surfacesBad += 1; totalBad += bad
        let who = owners.sorted { $0.value > $1.value }.prefix(3)
                        .map { "id \($0.key)x\($0.value)" }.joined(separator: " ")
        print("surface \(id) (\(w)x\(h)): BAD \(bad)/\(w*h) first=(\(first!.0),\(first!.1))  holding: \(who)")
    }
}
print("")
if surfacesBad == 0 {
    print("ALL \(texes.count) SURFACES HOLD THEIR OWN DATA - no aliasing at this size or pressure")
} else {
    print("\(surfacesBad)/\(texes.count) surfaces corrupted, \(totalBad) texels total")
    print("A surface holding another surface's id is two allocations on one piece of memory.")
}
