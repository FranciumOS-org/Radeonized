// screenprobe.swift (2mx) — put a KNOWN pattern on the real screen, several
// production paths side by side, and capture the result.
//
// Every probe in this repo before this one was a closed loop: the process wrote
// a texture and read it back itself. All were clean and the screen was not. So
// this one renders into real windows and lets the WindowServer composite them,
// which is the path the symptom actually lives on.
//
// Each run shows two windows carrying the SAME pattern:
//
//   left   drawn by CoreGraphics on the CPU   - the control, and how Terminal
//                                               and System Settings get their
//                                               pixels (those are crisp)
//   right  drawn by the GPU, by one of:
//            direct      a plain CAMetalLayer render
//            msaa        4x multisample, resolved into the drawable - the assistant
//                        allocates 20 multisample textures and this hardware
//                        path has never been measured
//            iosurface   render into an IOSurface and hand that to a CALayer
//                        as its contents, which is how Chromium passes frames
//                        from its GPU process to its browser process
//
// and `big` / `bigmsaa` repeat two of those at 900x880 instead of 512x512,
// because the corrupted things on screen are large (the assistant's window is about
// 1485x885, the desktop is 1920x1080) and the clean measurement so far was at
// 512x512.
//
// Diffing the two windows cancels colour management exactly: the same transform
// reaches both on the same frame.
//
// build: make screenprobe        run: tools/screenprobe.sh
import Cocoa
import Metal
import QuartzCore

setvbuf(stdout, nil, _IONBF, 0)   // rule 14: the first run lost its output to buffering

// argv: <mode> <ready-file> <seconds>
let MODE = CommandLine.arguments.count > 1 ? CommandLine.arguments[1] : "direct"
let READY = CommandLine.arguments.count > 2 ? CommandLine.arguments[2] : ""
let SECONDS = CommandLine.arguments.count > 3 ? Double(CommandLine.arguments[3]) ?? 12.0 : 12.0

let BIG = (MODE == "big" || MODE == "bigmsaa" || MODE == "bigcpu")
let WIDE = BIG ? 900 : 512
let TALL = BIG ? 880 : 512
let MSAA = (MODE == "msaa" || MODE == "bigmsaa")
let VIA_IOSURFACE = (MODE == "iosurface")
// bigcpu draws BOTH windows with CoreGraphics and never touches Metal, which
// separates "a large window backing store corrupts" from "allocating a large
// GPU texture corrupts something else".
let CPUONLY = (MODE == "bigcpu")

// 2my. Across the first sweep the CPU window was intermittently shredded and
// the GPU window never was - in direct-1 and msaa-2 but not in direct-2 or
// msaa-1, so it is not the mode. The one asymmetry built into this probe
// without thinking about it is that the GPU window redraws every 0.5s on a
// timer while the CPU window is drawn once and left alone. On screen in the
// wild the same split holds: the wallpaper, desktop icons and Safari's in-page
// images are drawn once and are broken; Terminal, the widgets and the menu bar
// redraw constantly and are crisp.
//
// So swap it. If the corruption follows "drawn once" rather than "drawn by the
// CPU", then CPU-versus-GPU was never the variable.
//
//   refresh   both windows redraw continuously   -> expect both clean
//   static    neither redraws                    -> expect BOTH to corrupt
//   swap      CPU redraws, GPU drawn once        -> expect the GPU one to corrupt
let CPU_REDRAW = (MODE == "refresh" || MODE == "swap")
let GPU_ONCE = (MODE == "static" || MODE == "swap")

// The one definition of the pattern, shared by the CPU and GPU paths, so a
// difference on screen is a difference in how the pixels travelled rather than
// in what was asked for. The blue channel carries the high bits of x and y,
// which is what makes a row or column identifiable even though red and green
// repeat every 256.
@inline(__always) func patternPixel(_ x: Int, _ y: Int) -> UInt32 {
    if y < 16 {
        let v = UInt32(min(255, x * 256 / WIDE))
        return 0xff00_0000 | (v << 16) | (v << 8) | v
    }
    if (x % 256 == 0) || (y % 256 == 0) { return 0xffff_ffff }
    if (x % 64 == 0) || (y % 64 == 0) { return 0xff80_8080 }
    let r = UInt32(x & 0xff), g = UInt32(y & 0xff)
    let b = UInt32(((x >> 8) & 0xf) | (((y >> 8) & 0xf) << 4))
    return 0xff00_0000 | (r << 16) | (g << 8) | b
}

// --- the CPU window ---------------------------------------------------------
final class CPUView: NSView {
    override func draw(_ dirty: NSRect) {
        var px = [UInt32](repeating: 0, count: WIDE * TALL)
        for y in 0..<TALL { for x in 0..<WIDE { px[y * WIDE + x] = patternPixel(x, y) } }
        px.withUnsafeMutableBytes { raw in
            let bi = CGImageAlphaInfo.noneSkipFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue
            guard let ctx = CGContext(data: raw.baseAddress, width: WIDE, height: TALL,
                                      bitsPerComponent: 8, bytesPerRow: WIDE * 4,
                                      space: CGColorSpaceCreateDeviceRGB(), bitmapInfo: bi),
                  let img = ctx.makeImage(),
                  let cg = NSGraphicsContext.current?.cgContext else { return }
            // CoreGraphics draws images bottom-up. The first version of this
            // tool produced a vertically flipped control because of it.
            cg.saveGState()
            cg.translateBy(x: 0, y: CGFloat(TALL))
            cg.scaleBy(x: 1, y: -1)
            cg.interpolationQuality = .none
            cg.draw(img, in: CGRect(x: 0, y: 0, width: WIDE, height: TALL))
            cg.restoreGState()
        }
    }
}

// --- the GPU side -----------------------------------------------------------
let shaders = """
#include <metal_stdlib>
using namespace metal;
vertex float4 vs(uint vid [[vertex_id]]) {
    float2 p = float2((vid << 1) & 2, vid & 2);
    return float4(p * 2.0 - 1.0, 0.0, 1.0);
}
fragment float4 fs(float4 pos [[position]], constant uint &wide [[buffer(0)]]) {
    uint x = uint(pos.x), y = uint(pos.y);
    if (y < 16u) { float v = float(min(255u, x * 256u / wide)) / 255.0; return float4(v, v, v, 1); }
    if ((x % 256u == 0u) || (y % 256u == 0u)) return float4(1, 1, 1, 1);
    if ((x % 64u  == 0u) || (y % 64u  == 0u)) return float4(0.5020, 0.5020, 0.5020, 1);
    float r = float(x & 0xffu) / 255.0;
    float g = float(y & 0xffu) / 255.0;
    float b = float(((x >> 8) & 0xfu) | (((y >> 8) & 0xfu) << 4)) / 255.0;
    return float4(r, g, b, 1);
}
"""

let dev = MTLCreateSystemDefaultDevice()
var queue: MTLCommandQueue!
var pso: MTLRenderPipelineState!
var msaaTex: MTLTexture?

func buildPipeline() -> Bool {
    guard let dev = dev, let lib = try? dev.makeLibrary(source: shaders, options: nil) else { return false }
    queue = dev.makeCommandQueue()
    let d = MTLRenderPipelineDescriptor()
    d.vertexFunction = lib.makeFunction(name: "vs")
    d.fragmentFunction = lib.makeFunction(name: "fs")
    d.colorAttachments[0].pixelFormat = .bgra8Unorm
    d.rasterSampleCount = MSAA ? 4 : 1
    pso = try? dev.makeRenderPipelineState(descriptor: d)
    if MSAA {
        let td = MTLTextureDescriptor()
        td.textureType = .type2DMultisample
        td.pixelFormat = .bgra8Unorm
        td.width = WIDE; td.height = TALL
        td.sampleCount = 4
        td.storageMode = .private
        td.usage = [.renderTarget]
        msaaTex = dev.makeTexture(descriptor: td)
        if msaaTex == nil { return false }

    }
    return pso != nil
}

// Draw the pattern into `target`, resolving from a multisample texture when
// the mode asks for it.
func renderPattern(into target: MTLTexture, present: MTLDrawable?) {
    let pass = MTLRenderPassDescriptor()
    if MSAA, let ms = msaaTex {
        pass.colorAttachments[0].texture = ms
        pass.colorAttachments[0].resolveTexture = target
        pass.colorAttachments[0].storeAction = .multisampleResolve
    } else {
        pass.colorAttachments[0].texture = target
        pass.colorAttachments[0].storeAction = .store
    }
    pass.colorAttachments[0].loadAction = .clear
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1)
    let cb = queue.makeCommandBuffer()!
    let e = cb.makeRenderCommandEncoder(descriptor: pass)!
    e.setRenderPipelineState(pso)
    var w = UInt32(WIDE)
    e.setFragmentBytes(&w, length: 4, index: 0)
    e.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
    e.endEncoding()
    if target.storageMode == .managed {
        let b = cb.makeBlitCommandEncoder()!; b.synchronize(resource: target); b.endEncoding()
    }
    if let p = present { cb.present(p) }
    cb.commit()
    cb.waitUntilCompleted()
}

// path A: a CAMetalLayer the compositor reads directly
final class MetalView: NSView {
    override func makeBackingLayer() -> CALayer {
        let l = CAMetalLayer()
        l.device = dev
        l.pixelFormat = .bgra8Unorm
        l.framebufferOnly = false
        l.isOpaque = true
        return l
    }
    func render() {
        let l = layer as! CAMetalLayer
        l.drawableSize = CGSize(width: WIDE, height: TALL)
        guard let drw = l.nextDrawable() else { return }
        renderPattern(into: drw.texture, present: drw)
    }
}

// path B: render into an IOSurface and give it to a plain CALayer, which is
// how Chromium's GPU process hands a finished frame to the browser process
final class SurfaceView: NSView {
    var surface: IOSurfaceRef?
    var tex: MTLTexture?
    func prepare() -> Bool {
        guard let dev = dev else { return false }
        let bpr = IOSurfaceAlignProperty(kIOSurfaceBytesPerRow, WIDE * 4)
        let props: [IOSurfacePropertyKey: Any] = [
            .width: WIDE, .height: TALL, .bytesPerElement: 4,
            .bytesPerRow: bpr, .pixelFormat: 0x42475241]
        guard let s = IOSurface(properties: props) else { return false }
        surface = unsafeBitCast(s, to: IOSurfaceRef.self)
        let d = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .bgra8Unorm,
                                                         width: WIDE, height: TALL, mipmapped: false)
        d.storageMode = .managed
        d.usage = [.shaderRead, .renderTarget]
        tex = dev.makeTexture(descriptor: d, iosurface: surface!, plane: 0)
        return tex != nil
    }
    func render() {
        guard let t = tex else { return }
        renderPattern(into: t, present: nil)
        layer?.contents = surface
        layer?.contentsGravity = .topLeft
    }
}

// --- windows ----------------------------------------------------------------
func makeWindow(_ x: Int, _ y: Int) -> NSWindow {
    let w = NSWindow(contentRect: NSRect(x: x, y: y, width: WIDE, height: TALL),
                     styleMask: [.borderless], backing: .buffered, defer: false)
    w.level = .floating
    w.isOpaque = true
    w.backgroundColor = .black
    return w
}

let app = NSApplication.shared
app.setActivationPolicy(.accessory)

guard let screen = NSScreen.main else { print("no screen"); exit(2) }
let sh = Int(screen.frame.height)
let originY = BIG ? 100 : 200
let gap = BIG ? 40 : 40
let leftX = BIG ? 40 : 100
let rightX = leftX + WIDE + gap

let cpuWin = makeWindow(leftX, originY)
let gpuWin = makeWindow(rightX, originY)
let cpuView = CPUView(frame: NSRect(x: 0, y: 0, width: WIDE, height: TALL))
cpuWin.contentView = cpuView

let metalView = MetalView(frame: NSRect(x: 0, y: 0, width: WIDE, height: TALL))
let surfaceView = SurfaceView(frame: NSRect(x: 0, y: 0, width: WIDE, height: TALL))
if CPUONLY {
    gpuWin.contentView = CPUView(frame: NSRect(x: 0, y: 0, width: WIDE, height: TALL))
} else if VIA_IOSURFACE {
    surfaceView.wantsLayer = true
    gpuWin.contentView = surfaceView
} else {
    metalView.wantsLayer = true
    gpuWin.contentView = metalView
}

// On screen first, compile second: the other order lost an entire capture,
// because makeLibrary(source:) compiles the shader at runtime.
cpuWin.orderFrontRegardless()
gpuWin.orderFrontRegardless()

if !CPUONLY {
    guard dev != nil, buildPipeline() else {
        print("NO METAL DEVICE or pipeline failed - this boot has no accelerator (rule 14).")
        print("  mode=\(MODE)")
        exit(2)
    }
    if VIA_IOSURFACE && !surfaceView.prepare() {
        print("IOSurface path unavailable (rule 14: saying so rather than drawing nothing)")
        exit(2)
    }
}

func draw() {
    if CPUONLY { return }
    if VIA_IOSURFACE { surfaceView.render() } else { metalView.render() }
}
draw()

let topY = sh - originY - TALL
print("mode        \(MODE)")
print("CPU window  topleft=(\(leftX),\(topY))  \(WIDE)x\(TALL)")
print("GPU window  topleft=(\(rightX),\(topY))  \(WIDE)x\(TALL)")
print("screen \(Int(screen.frame.width))x\(sh), backingScale \(cpuWin.backingScaleFactor)")
print("decode with: build/out/decodescreen <png> \(leftX) \(topY) \(rightX) \(topY) \(WIDE) \(TALL)")

// Only now is the pattern actually on screen, so only now may it be captured.
if !READY.isEmpty { try? "ready\n".write(toFile: READY, atomically: true, encoding: .utf8) }

print("cpu redraws: \(CPU_REDRAW), gpu drawn once: \(GPU_ONCE)")

Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) { _ in
    if CPU_REDRAW { cpuView.needsDisplay = true }
    if !GPU_ONCE { draw() }
}
Timer.scheduledTimer(withTimeInterval: SECONDS, repeats: false) { _ in app.terminate(nil) }
app.run()
