// screenprobe.swift (2mx) — put a KNOWN pattern on the real screen, two ways.
//
// Every probe in this repo so far has been a closed loop: this process writes a
// texture, this process reads it back. All of them are clean, and the screen is
// not. The one path none of them touches is the real one - a window surface
// produced by an app and composited by the WindowServer.
//
// So: two windows, side by side, showing the SAME self-locating pattern.
//
//   left   drawn by CoreGraphics on the CPU  - how Terminal and System
//                                              Settings get their pixels,
//                                              and those are crisp
//   right  drawn by Metal on the GPU         - how the assistant and Safari's
//                                              in-page images get theirs,
//                                              and those are not
//
// Then screencapture the result. If the right window is corrupt and the left is
// not, this is a minimal reproducer under our control, and because the pattern
// encodes its own coordinates the displacement can be read off exactly instead
// of inferred from broken letterforms.
//
// The pattern, per pixel:  R = x & 255,  G = y & 255,  B = (x>>8) | (y>>8)<<4
// plus white grid lines every 64 px (brighter every 256) so the structure is
// visible by eye, and a grey calibration ramp along the top so any colour
// transform the compositor applies can be inverted rather than guessed at.
//
// build: make screenprobe        run: tools/screenprobe.sh
import Cocoa
import Metal
import QuartzCore

let SIDE = 512
let SECONDS = CommandLine.arguments.count > 1 ? Double(CommandLine.arguments[1]) ?? 6.0 : 6.0

// The one definition of the pattern, used by both the CPU and the GPU path.
// Keeping it in one place is the point: a difference on screen is then a
// difference in how the pixels travelled, not in what was asked for.
@inline(__always) func patternPixel(_ x: Int, _ y: Int) -> UInt32 {
    if y < 16 {                                   // calibration ramp
        let v = UInt32(min(255, x * 256 / SIDE))
        return 0xff00_0000 | (v << 16) | (v << 8) | v
    }
    let onGrid = (x % 64 == 0) || (y % 64 == 0)
    let onMajor = (x % 256 == 0) || (y % 256 == 0)
    if onMajor { return 0xffff_ffff }
    if onGrid  { return 0xff80_8080 }
    let r = UInt32(x & 0xff), g = UInt32(y & 0xff)
    let b = UInt32(((x >> 8) & 0xf) | (((y >> 8) & 0xf) << 4))
    return 0xff00_0000 | (r << 16) | (g << 8) | b
}

// --- the CPU window ---------------------------------------------------------
final class CPUView: NSView {
    override var isFlipped: Bool { true }
    override func draw(_ dirty: NSRect) {
        var px = [UInt32](repeating: 0, count: SIDE * SIDE)
        for y in 0..<SIDE { for x in 0..<SIDE { px[y * SIDE + x] = patternPixel(x, y) } }
        px.withUnsafeMutableBytes { raw in
            let cs = CGColorSpaceCreateDeviceRGB()
            let bi = CGBitmapInfo(rawValue: CGImageAlphaInfo.noneSkipFirst.rawValue
                                          | CGBitmapInfo.byteOrder32Little.rawValue)
            guard let ctx = CGContext(data: raw.baseAddress, width: SIDE, height: SIDE,
                                      bitsPerComponent: 8, bytesPerRow: SIDE * 4,
                                      space: cs, bitmapInfo: bi.rawValue),
                  let img = ctx.makeImage(),
                  let cg = NSGraphicsContext.current?.cgContext else { return }
            cg.interpolationQuality = .none
            cg.draw(img, in: CGRect(x: 0, y: 0, width: SIDE, height: SIDE))
        }
    }
}

// --- the GPU window ---------------------------------------------------------
let shaders = """
#include <metal_stdlib>
using namespace metal;
vertex float4 vs(uint vid [[vertex_id]]) {
    float2 p = float2((vid << 1) & 2, vid & 2);
    return float4(p * 2.0 - 1.0, 0.0, 1.0);
}
fragment float4 fs(float4 pos [[position]]) {
    uint x = uint(pos.x), y = uint(pos.y);
    if (y < 16u) { float v = float(min(255u, x * 256u / 512u)) / 255.0; return float4(v, v, v, 1); }
    bool major = (x % 256u == 0u) || (y % 256u == 0u);
    bool grid  = (x % 64u  == 0u) || (y % 64u  == 0u);
    if (major) return float4(1, 1, 1, 1);
    if (grid)  return float4(0.5020, 0.5020, 0.5020, 1);
    float r = float(x & 0xffu) / 255.0;
    float g = float(y & 0xffu) / 255.0;
    float b = float(((x >> 8) & 0xfu) | (((y >> 8) & 0xfu) << 4)) / 255.0;
    return float4(r, g, b, 1);
}
"""

final class GPUView: NSView {
    let dev = MTLCreateSystemDefaultDevice()
    var q: MTLCommandQueue!, pso: MTLRenderPipelineState!
    var metalLayer: CAMetalLayer { layer as! CAMetalLayer }

    override func makeBackingLayer() -> CALayer {
        let l = CAMetalLayer()
        l.device = dev
        l.pixelFormat = .bgra8Unorm
        l.framebufferOnly = false
        l.isOpaque = true
        return l
    }

    func setup() -> Bool {
        guard let dev = dev else { return false }
        wantsLayer = true
        q = dev.makeCommandQueue()
        guard let lib = try? dev.makeLibrary(source: shaders, options: nil) else { return false }
        let d = MTLRenderPipelineDescriptor()
        d.vertexFunction = lib.makeFunction(name: "vs")
        d.fragmentFunction = lib.makeFunction(name: "fs")
        d.colorAttachments[0].pixelFormat = .bgra8Unorm
        pso = try? dev.makeRenderPipelineState(descriptor: d)
        return pso != nil
    }

    func render() {
        metalLayer.drawableSize = CGSize(width: SIDE, height: SIDE)
        guard let drw = metalLayer.nextDrawable() else { return }
        let pass = MTLRenderPassDescriptor()
        pass.colorAttachments[0].texture = drw.texture
        pass.colorAttachments[0].loadAction = .clear
        pass.colorAttachments[0].storeAction = .store
        pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1)
        let cb = q.makeCommandBuffer()!
        let e = cb.makeRenderCommandEncoder(descriptor: pass)!
        e.setRenderPipelineState(pso)
        e.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
        e.endEncoding()
        cb.present(drw)
        cb.commit()
        cb.waitUntilCompleted()
    }
}

// --- windows ----------------------------------------------------------------
func makeWindow(_ x: Int, _ y: Int, _ title: String) -> NSWindow {
    let w = NSWindow(contentRect: NSRect(x: x, y: y, width: SIDE, height: SIDE),
                     styleMask: [.borderless], backing: .buffered, defer: false)
    w.level = .floating
    w.isOpaque = true
    w.backgroundColor = .black
    w.title = title
    return w
}

let app = NSApplication.shared
app.setActivationPolicy(.accessory)

// Placed at a fixed origin so the decoder knows where to look. NSWindow's
// origin is bottom-left; the screenshot's is top-left - the wrapper prints
// both so nothing has to be guessed later.
guard let screen = NSScreen.main else { print("no screen"); exit(2) }
let sh = Int(screen.frame.height)
let originY = 200
let cpuWin = makeWindow(100, originY, "cpu")
let gpuWin = makeWindow(100 + SIDE + 40, originY, "gpu")

let cpuView = CPUView(frame: NSRect(x: 0, y: 0, width: SIDE, height: SIDE))
cpuWin.contentView = cpuView

let gpuView = GPUView(frame: NSRect(x: 0, y: 0, width: SIDE, height: SIDE))
guard gpuView.setup() else {
    print("NO METAL DEVICE or pipeline failed - this boot has no accelerator (rule 14).")
    exit(2)
}
gpuWin.contentView = gpuView

cpuWin.orderFrontRegardless()
gpuWin.orderFrontRegardless()

// Top-left coordinates, which is what the screenshot uses.
let topY = sh - originY - SIDE
print("CPU window  topleft=(100,\(topY))  \(SIDE)x\(SIDE)")
print("GPU window  topleft=(\(100 + SIDE + 40),\(topY))  \(SIDE)x\(SIDE)")
print("screen \(Int(screen.frame.width))x\(sh), backingScale \(cpuWin.backingScaleFactor)")

gpuView.render()
// Redraw a few times: a single frame could be a drawable that was never
// recycled, and the symptom on screen is of a surface in steady use.
Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) { _ in gpuView.render() }
Timer.scheduledTimer(withTimeInterval: SECONDS, repeats: false) { _ in app.terminate(nil) }
app.run()
