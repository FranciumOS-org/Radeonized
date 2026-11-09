// mipprobe (3mip) - does every mip level of a texture hold what was put there?
//
// Why it exists. Spotify drew every album cover as flat green. Its GPU process
// uploads a decoded JPEG as single-channel planes WITH A MIP CHAIN and usage
// ShaderRead only, and a plane that samples as zero in all three channels
// converts to green. No instrument in this tree had ever made a mipmapped
// texture: lawtest, texprobe and blitprobe are all single-level.
//
// Five ways in, two ways out, and level 0 as the in-sample control for each:
//   upBlit   buffer -> texture level L (blit)          read back by blit
//   upCPU    replaceRegion level L (managed only)      read back by blit
//   sample   sample level L with an explicit LOD into an rgba8 target
//   genMip   generateMipmaps from a smooth level 0     read back by blit
//   rtDraw   render into level L as a colour attachment, read back by blit
// `sample` reads the bytes upBlit wrote through a different instrument, so a
// readback defect and a storage defect do not look alike. The readback buffer
// is prefilled with 0xEE, so a copy that wrote NOTHING ("u") is told apart
// from one that wrote zeroes ("z").
//
// Every format x size x storage mode runs four ways: usage ShaderRead or
// RenderTarget|ShaderRead, each flat and mipmapped. The flat rows and the
// render-target rows are the controls - on boot 507 they are all clean and
// only "SR mips" fails, 40 of 40.
//
//   mipprobe            full matrix (5 sizes, 160 rows)
//   mipprobe quick      64x64 and 300x300 only
//   mipprobe verbose    per-level mismatch / zero / unwritten percentages
//   mipprobe unpatch    put the driver's ORIGINAL prologue back in this process
//                       first (tools/mipprobe_unpatch.c). The kext places the
//                       swizzle detour once per pid, so this is an in-boot A/B
//                       of "our fix" against "no fix" with no reboot.
//
// It prints the detour state at start and at end (1 present, 0 original,
// -1 neither - which is what a userspace mtlswizzle.dylib leaves, -2 no row).
// A run whose state is not the one the arm intended measured something else.
//
// build: make mipprobe        exit: 0 clean, 1 a level did not hold, 3 timeout
import Metal
import Foundation

setvbuf(stdout, nil, _IONBF, 0)
guard let dev = MTLCreateSystemDefaultDevice(), let q = dev.makeCommandQueue() else {
    print("no Metal device"); exit(2)
}
print("device: \(dev.name)")
let unpatch = CommandLine.arguments.contains("unpatch")
print("swizzle detour at start: \(rb_detour_state()) (1 present, 0 original, -1 neither, -2 not found)")
if unpatch { print("removing detour in this process: rc \(rb_detour_remove()), now \(rb_detour_state())") }

struct Fmt { let f: MTLPixelFormat; let name: String; let bpp: Int; let bgra: Bool }
let fmts = [Fmt(f: .r8Unorm, name: "r8", bpp: 1, bgra: false),
            Fmt(f: .rg8Unorm, name: "rg8", bpp: 2, bgra: false),
            Fmt(f: .rgba8Unorm, name: "rgba8", bpp: 4, bgra: false),
            Fmt(f: .bgra8Unorm, name: "bgra8", bpp: 4, bgra: true)]
var sizes = [(64, 64), (256, 256), (300, 300), (640, 640), (1024, 128)]
let args = CommandLine.arguments
let quick = args.contains("quick")
if quick { sizes = [(64, 64), (300, 300)] }

func levelCount(_ w: Int, _ h: Int) -> Int { var n = 1; var m = max(w, h); while m > 1 { m /= 2; n += 1 }; return n }
func dim(_ v: Int, _ l: Int) -> Int { max(1, v >> l) }
func pat(_ x: Int, _ y: Int, _ c: Int, _ seed: Int) -> UInt8 { UInt8(((x * 7 + y * 13 + c * 29 + seed * 53) % 251) + 1) }
func fill(_ w: Int, _ h: Int, _ bpp: Int, _ seed: Int) -> [UInt8] {
    var b = [UInt8](repeating: 0, count: w * h * bpp)
    for y in 0..<h { for x in 0..<w { for c in 0..<bpp { b[(y * w + x) * bpp + c] = pat(x, y, c, seed) } } }
    return b
}
func smooth(_ x: Int, _ y: Int, _ c: Int, _ w: Int, _ h: Int) -> Int { 40 + (x * 100) / w + (y * 60) / h + c * 10 }

@discardableResult
func run(_ what: String, _ body: (MTLCommandBuffer) -> Void) -> Bool {
    let cb = q.makeCommandBuffer()!
    body(cb)
    let sem = DispatchSemaphore(value: 0)
    cb.addCompletedHandler { _ in sem.signal() }
    cb.commit()
    if sem.wait(timeout: .now() + 10) == .timedOut {
        print("TIMEOUT in \(what) - command buffer never completed; stopping"); exit(3)
    }
    if cb.status != .completed { print("  [\(what): status \(cb.status.rawValue) \(String(describing: cb.error))]"); return false }
    return true
}

func readLevel(_ t: MTLTexture, _ l: Int, _ bpp: Int) -> [UInt8] {
    let w = dim(t.width, l), h = dim(t.height, l), n = w * h * bpp
    let buf = dev.makeBuffer(length: n, options: .storageModeShared)!
    memset(buf.contents(), 0xEE, n)   // "never written" must not look like zero
    run("read L\(l)") { cb in
        let b = cb.makeBlitCommandEncoder()!
        b.copy(from: t, sourceSlice: 0, sourceLevel: l, sourceOrigin: MTLOrigin(x: 0, y: 0, z: 0),
               sourceSize: MTLSize(width: w, height: h, depth: 1), to: buf, destinationOffset: 0,
               destinationBytesPerRow: w * bpp, destinationBytesPerImage: n)
        b.endEncoding()
    }
    return Array(UnsafeBufferPointer(start: buf.contents().assumingMemoryBound(to: UInt8.self), count: n))
}

func uploadBlit(_ t: MTLTexture, _ l: Int, _ bytes: [UInt8], _ bpp: Int) {
    let w = dim(t.width, l), h = dim(t.height, l)
    let buf = dev.makeBuffer(bytes: bytes, length: bytes.count, options: .storageModeShared)!
    run("upload L\(l)") { cb in
        let b = cb.makeBlitCommandEncoder()!
        b.copy(from: buf, sourceOffset: 0, sourceBytesPerRow: w * bpp, sourceBytesPerImage: bytes.count,
               sourceSize: MTLSize(width: w, height: h, depth: 1), to: t, destinationSlice: 0,
               destinationLevel: l, destinationOrigin: MTLOrigin(x: 0, y: 0, z: 0))
        b.endEncoding()
    }
}

struct Res { var mism = 0, zero = 0, sent = 0, total = 0
    var ok: Bool { mism == 0 }
    var text: String {
        let p = { (n: Int) in total == 0 ? "0" : String(format: "%.0f%%", 100.0 * Double(n) / Double(total)) }
        return "mism \(p(mism)) zero \(p(zero))" + (sent > 0 ? " unwritten \(p(sent))" : "")
    }
}
func cmp(_ got: [UInt8], _ want: [UInt8], tol: Int = 0) -> Res {
    var r = Res(); r.total = want.count
    for i in 0..<want.count {
        if abs(Int(got[i]) - Int(want[i])) > tol { r.mism += 1 }
        if got[i] == 0 { r.zero += 1 }
        if got[i] == 0xEE { r.sent += 1 }
    }
    return r
}
func summarize(_ rs: [Res]) -> String {
    let bad = rs.enumerated().filter { !$0.element.ok }
    if bad.isEmpty { return "ok(\(rs.count))" }
    if CommandLine.arguments.contains("verbose") { return "BAD " + bad.map { "L\($0.offset)[\($0.element.text)]" }.joined(separator: " ") }
    return "BAD " + bad.map { "L\($0.offset)" + ($0.element.zero == $0.element.total ? "z" : $0.element.sent == $0.element.total ? "u" : "") }.joined(separator: ",") + " of \(rs.count)"
}

let src = """
#include <metal_stdlib>
using namespace metal;
struct V { float4 pos [[position]]; float2 uv; };
vertex V vs(uint vid [[vertex_id]]) {
    float2 p = float2((vid << 1) & 2, vid & 2);
    V o; o.pos = float4(p * 2 - 1, 0, 1); o.uv = float2(p.x, 1 - p.y); return o;
}
fragment float4 fs_sample(V in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]],
                          constant float &lod [[buffer(0)]]) {
    return t.sample(s, in.uv, level(lod));
}
fragment float4 fs_pat(V in [[stage_in]]) {
    float2 p = floor(in.pos.xy);
    float v = (fmod(p.x * 7 + p.y * 13, 251.0) + 1) / 255.0;
    return float4(v, v, v, v);
}
"""
let lib: MTLLibrary
do { lib = try dev.makeLibrary(source: src, options: nil) } catch { print("shader compile failed: \(error)"); exit(2) }
var pipes: [String: MTLRenderPipelineState] = [:]
func pipe(_ frag: String, _ f: MTLPixelFormat) -> MTLRenderPipelineState {
    let k = "\(frag)-\(f.rawValue)"
    if let p = pipes[k] { return p }
    let d = MTLRenderPipelineDescriptor()
    d.vertexFunction = lib.makeFunction(name: "vs"); d.fragmentFunction = lib.makeFunction(name: frag)
    d.colorAttachments[0].pixelFormat = f
    let p = try! dev.makeRenderPipelineState(descriptor: d); pipes[k] = p; return p
}
let sd = MTLSamplerDescriptor()
sd.minFilter = .nearest; sd.magFilter = .nearest; sd.mipFilter = .nearest
let samp = dev.makeSamplerState(descriptor: sd)!

func makeTex(_ f: MTLPixelFormat, _ w: Int, _ h: Int, _ mips: Bool, _ mode: MTLStorageMode, _ usage: MTLTextureUsage) -> MTLTexture? {
    let d = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: f, width: w, height: h, mipmapped: mips)
    d.storageMode = mode; d.usage = usage
    return dev.makeTexture(descriptor: d)
}

// Sample level l of t into a fresh single-level rgba8 target of the level's size.
func sampleLevel(_ t: MTLTexture, _ l: Int) -> [UInt8] {
    let w = dim(t.width, l), h = dim(t.height, l)
    let dst = makeTex(.rgba8Unorm, w, h, false, .private, [.renderTarget, .shaderRead])!
    var lod = Float(l)
    run("sample L\(l)") { cb in
        let rp = MTLRenderPassDescriptor()
        rp.colorAttachments[0].texture = dst; rp.colorAttachments[0].loadAction = .clear
        rp.colorAttachments[0].clearColor = MTLClearColor(red: 0.5, green: 0.5, blue: 0.5, alpha: 0.5)
        rp.colorAttachments[0].storeAction = .store
        let e = cb.makeRenderCommandEncoder(descriptor: rp)!
        e.setRenderPipelineState(pipe("fs_sample", .rgba8Unorm))
        e.setFragmentTexture(t, index: 0); e.setFragmentSamplerState(samp, index: 0)
        e.setFragmentBytes(&lod, length: 4, index: 0)
        e.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
        e.endEncoding()
    }
    return readLevel(dst, 0, 4)
}

var anyBad = false
for fm in fmts {
    for (w, h) in sizes {
        for mode in [MTLStorageMode.private, .managed] {
            for (usage, mips) in [(MTLTextureUsage.shaderRead, false), (.shaderRead, true), ([.shaderRead, .renderTarget], false), ([.shaderRead, .renderTarget], true)] {
                let rt = usage.contains(.renderTarget)
                let n = mips ? levelCount(w, h) : 1
                let tag = "\(fm.name) \(w)x\(h) \(mode == .private ? "private" : "managed") \(rt ? "RT|SR" : "SR   ") \(mips ? "mips" : "flat")"
                guard let t = makeTex(fm.f, w, h, mips, mode, usage) else { print("\(tag): ALLOC FAILED"); anyBad = true; continue }
                var out: [String] = []

                // upBlit + sample (same bytes, two readers)
                var up: [Res] = [], sm: [Res] = []
                for l in 0..<n {
                    let lw = dim(w, l), lh = dim(h, l)
                    let want = fill(lw, lh, fm.bpp, l + 1)
                    uploadBlit(t, l, want, fm.bpp)
                }
                for l in 0..<n {
                    let lw = dim(w, l), lh = dim(h, l)
                    let want = fill(lw, lh, fm.bpp, l + 1)
                    up.append(cmp(readLevel(t, l, fm.bpp), want))
                    let got = sampleLevel(t, l)
                    var wantS = [UInt8](repeating: 0, count: lw * lh * fm.bpp), gotS = wantS
                    for i in 0..<(lw * lh) { for c in 0..<fm.bpp {
                        let sc = fm.bgra && c < 3 ? 2 - c : c       // target is RGBA
                        wantS[i * fm.bpp + c] = want[i * fm.bpp + sc]
                        gotS[i * fm.bpp + c] = got[i * 4 + c]
                    } }
                    sm.append(cmp(gotS, wantS))
                }
                out.append("upBlit " + summarize(up)); out.append("sample " + summarize(sm))

                // upCPU (managed only)
                if mode == .managed, let t2 = makeTex(fm.f, w, h, mips, mode, usage) {
                    var rs: [Res] = []
                    for l in 0..<n {
                        let lw = dim(w, l), lh = dim(h, l)
                        let want = fill(lw, lh, fm.bpp, l + 7)
                        want.withUnsafeBytes { t2.replace(region: MTLRegionMake2D(0, 0, lw, lh), mipmapLevel: l, withBytes: $0.baseAddress!, bytesPerRow: lw * fm.bpp) }
                    }
                    for l in 0..<n { rs.append(cmp(readLevel(t2, l, fm.bpp), fill(dim(w, l), dim(h, l), fm.bpp, l + 7))) }
                    out.append("upCPU " + summarize(rs))
                }

                // genMip
                if mips, let t3 = makeTex(fm.f, w, h, mips, mode, usage) {
                    var l0 = [UInt8](repeating: 0, count: w * h * fm.bpp)
                    for y in 0..<h { for x in 0..<w { for c in 0..<fm.bpp { l0[(y * w + x) * fm.bpp + c] = UInt8(smooth(x, y, c, w, h)) } } }
                    uploadBlit(t3, 0, l0, fm.bpp)
                    run("genMip") { cb in let b = cb.makeBlitCommandEncoder()!; b.generateMipmaps(for: t3); b.endEncoding() }
                    var rs: [Res] = []
                    for l in 0..<n {
                        let lw = dim(w, l), lh = dim(h, l)
                        var want = [UInt8](repeating: 0, count: lw * lh * fm.bpp)
                        for y in 0..<lh { for x in 0..<lw { for c in 0..<fm.bpp {
                            let sx = min(w - 1, ((2 * x + 1) << l) / 2), sy = min(h - 1, ((2 * y + 1) << l) / 2)
                            want[(y * lw + x) * fm.bpp + c] = UInt8(smooth(sx, sy, c, w, h))
                        } } }
                        // the last levels of a non-square or non-power-of-two chain average
                        // unevenly; the question here is "is anything there", so be generous
                        rs.append(cmp(readLevel(t3, l, fm.bpp), want, tol: l == 0 ? 0 : 40))
                    }
                    out.append("genMip " + summarize(rs))
                }

                // rtDraw
                if rt, let t4 = makeTex(fm.f, w, h, mips, mode, usage) {
                    var rs: [Res] = []
                    for l in 0..<n {
                        run("rtDraw L\(l)") { cb in
                            let rp = MTLRenderPassDescriptor()
                            rp.colorAttachments[0].texture = t4; rp.colorAttachments[0].level = l
                            rp.colorAttachments[0].loadAction = .dontCare; rp.colorAttachments[0].storeAction = .store
                            let e = cb.makeRenderCommandEncoder(descriptor: rp)!
                            e.setRenderPipelineState(pipe("fs_pat", fm.f))
                            e.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
                            e.endEncoding()
                        }
                    }
                    for l in 0..<n {
                        let lw = dim(w, l), lh = dim(h, l)
                        var want = [UInt8](repeating: 0, count: lw * lh * fm.bpp)
                        for y in 0..<lh { for x in 0..<lw { for c in 0..<fm.bpp { want[(y * lw + x) * fm.bpp + c] = UInt8(((x * 7 + y * 13) % 251) + 1) } } }
                        rs.append(cmp(readLevel(t4, l, fm.bpp), want))
                    }
                    out.append("rtDraw " + summarize(rs))
                }
                let line = out.joined(separator: " | ")
                if line.contains("BAD") { anyBad = true }
                print("\(tag): \(line)")
            }
        }
    }
}
print("swizzle detour at end: \(rb_detour_state())")
print(anyBad ? "RESULT: at least one level did not hold its contents" : "RESULT: every level held its contents")
exit(anyBad ? 1 : 0)
