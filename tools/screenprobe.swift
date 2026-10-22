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
//            msaa        4x multisample, resolved into the drawable - Claude
//                        allocates 20 multisample textures and this hardware
//                        path has never been measured
//            iosurface   render into an IOSurface and hand that to a CALayer
//                        as its contents, which is how Chromium passes frames
//                        from its GPU process to its browser process
//
// and `big` / `bigmsaa` repeat two of those at 900x880 instead of 512x512,
// because the corrupted things on screen are large (Claude's window is about
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
import MachO

setvbuf(stdout, nil, _IONBF, 0)   // rule 14: the first run lost its output to buffering

// argv: <mode> <ready-file> <seconds>
let MODE = CommandLine.arguments.count > 1 ? CommandLine.arguments[1] : "direct"
let READY = CommandLine.arguments.count > 2 ? CommandLine.arguments[2] : ""
let SECONDS = CommandLine.arguments.count > 3 ? Double(CommandLine.arguments[3]) ?? 12.0 : 12.0

// A per-run stamp, written into the BLUE channel of every pixel of both
// windows. Blue is then constant across the whole window, so it survives any
// rearrangement of the pixels: a scrambled window still reports the tag of the
// run that PRODUCED it. If a scrambled capture carries the previous run's tag
// it is a stale surface left behind by a process that has exited - this
// harness's fault, like the four instrument defects before it - and if it
// carries the current run's tag the corruption is live.
// The stamp did its job - every scrambled capture carried the CURRENT run's
// tag, so the corruption is live and not a stale surface - and blue is now
// needed back for the high coordinate bits, which is what names the swizzle.
//
// Those high bits used to be encoded as blue values 0, 1, 16, 17: a ONE level
// difference, which no display pipeline preserves reliably. The bit matrix
// came out ragged in exactly the columns that depend on them. They are now
// spaced 15 levels apart, which is survivable.
@inline(__always) func blueFor(_ x: Int, _ y: Int) -> UInt32 {
    let combo = ((x >> 8) & 3) | (((y >> 8) & 3) << 2)
    return UInt32(8 + combo * 15)
}

let BIG = (MODE == "big" || MODE == "bigmsaa" || MODE == "bigcpu")
let WIDE = BIG ? 900 : 512
let TALL = BIG ? 880 : 512
let MSAA = (MODE == "msaa" || MODE == "bigmsaa")
let VIA_IOSURFACE = (MODE == "iosurface" || MODE == "cpusurface")
// bigcpu draws BOTH windows with CoreGraphics and never touches Metal, which
// separates "a large window backing store corrupts" from "allocating a large
// GPU texture corrupts something else".
let CPUONLY = (MODE == "bigcpu")

// 2nd: the discriminator the table of results now points at.
//
//   CAMetalLayer, GPU-written                 never corrupts
//   explicit IOSurface -> contents, GPU-written  never corrupts
//   AppKit's own backing store, CPU-written   corrupts about half the time
//
// Everything the GPU writes is written AND read with the same tiling, so the
// two sides agree whatever that tiling is. A CPU write is linear by
// construction. If the compositor reads such a surface as tiled, the result is
// exactly the measured permutation: 8x8 micro tiles intact, placement wrong.
//
// So write an explicit IOSurface with the CPU and hand it to a CALayer. If it
// scrambles, the defect is "a CPU-written surface read as tiled" and has
// nothing to do with AppKit; if it stays clean, something about AppKit's own
// backing store is special and that is where to look.
let CPU_SURFACE = (MODE == "cpusurface")

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
    return 0xff00_0000 | (r << 16) | (g << 8) | blueFor(x, y)
}

// --- the CPU window ---------------------------------------------------------
//
// The first version of this built a CGImage over a Swift array's memory inside
// withUnsafeMutableBytes, so the backing buffer died when the closure exited.
// A CGImage made over caller-supplied bytes may reference them rather than
// copy them, and CoreGraphics may defer the draw - which reads freed memory
// and looks exactly like the corruption this tool exists to find. Rule 16: the
// instrument is a suspect, and that one had to be cleared before anything it
// reported could be believed.
//
// The pattern is constant, so it is rendered once into a buffer that lives for
// the life of the process and is never freed, and drawRect only blits it.
final class CPUView: NSView {
    static let image: CGImage = {
        let bytes = WIDE * TALL * 4
        let mem = UnsafeMutableRawPointer.allocate(byteCount: bytes, alignment: 64)
        for y in 0..<TALL {
            let row = mem + y * WIDE * 4
            for x in 0..<WIDE {
                row.storeBytes(of: patternPixel(x, y).littleEndian,
                               toByteOffset: x * 4, as: UInt32.self)
            }
        }
        let bi = CGImageAlphaInfo.noneSkipFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue
        let provider = CGDataProvider(dataInfo: nil, data: mem, size: bytes,
                                      releaseData: { _, _, _ in })!   // never released
        return CGImage(width: WIDE, height: TALL, bitsPerComponent: 8, bitsPerPixel: 32,
                       bytesPerRow: WIDE * 4, space: CGColorSpaceCreateDeviceRGB(),
                       bitmapInfo: CGBitmapInfo(rawValue: bi), provider: provider,
                       decode: nil, shouldInterpolate: false, intent: .defaultIntent)!
    }()

    override func draw(_ dirty: NSRect) {
        guard let cg = NSGraphicsContext.current?.cgContext else { return }
        // CoreGraphics draws images bottom-up; an earlier version produced a
        // vertically flipped control because of it.
        cg.saveGState()
        cg.translateBy(x: 0, y: CGFloat(TALL))
        cg.scaleBy(x: 1, y: -1)
        cg.interpolationQuality = .none
        cg.draw(CPUView.image, in: CGRect(x: 0, y: 0, width: WIDE, height: TALL))
        cg.restoreGState()
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
    uint combo = ((x >> 8) & 3u) | (((y >> 8) & 3u) << 2);
    return float4(r, g, float(8u + combo * 15u) / 255.0, 1);
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
        if CPU_SURFACE {
            // CPU write, straight into the surface's own bytes
            guard let s = surface else { return }
            IOSurfaceLock(s, [], nil)
            let base = IOSurfaceGetBaseAddress(s)
            let bpr = IOSurfaceGetBytesPerRow(s)
            for y in 0..<TALL {
                let row = base + y * bpr
                for x in 0..<WIDE {
                    row.storeBytes(of: patternPixel(x, y).littleEndian,
                                   toByteOffset: x * 4, as: UInt32.self)
                }
            }
            IOSurfaceUnlock(s, [], nil)
        } else {
            guard let t = tex else { return }
            renderPattern(into: t, present: nil)
        }
        layer?.contents = surface
        layer?.contentsGravity = .topLeft
    }
}

// --- is this process's Metal driver actually patched? -----------------------
//
// Rule 20: a hook that reports ARMED is not evidence that a patch applied. The
// 2lm/2nb user patch rewrites `amdMtl_HWL_DetermineSwizzleMode` inside
// AMDRadeonX6000MTLDriver to return ADDR_SW_LINEAR, and Lilu's user patcher
// has no way to report back into the kext. The plugin's own log is a ring
// buffer that had already rotated past the registration line by the time it
// was read, so NVRAM cannot answer this either.
//
// The process can answer it about itself: look at the bytes. Either the
// original prologue is there or the stub is.
func reportMetalDriverPatch() {
    let find: [UInt8] = [0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56,
                         0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x81, 0xEC,
                         0x88, 0x00, 0x00, 0x00, 0x45, 0x89, 0xCD, 0x44,
                         0x89, 0xC3, 0x48, 0x89, 0x4D, 0xD0, 0x49, 0x89]
    let repl: [UInt8] = [0x48, 0x8B, 0x44, 0x24, 0x08, 0xC7, 0x00, 0x00,
                         0x00, 0x00, 0x00, 0x31, 0xC0, 0xC3]

    for i in 0..<_dyld_image_count() {
        guard let nameC = _dyld_get_image_name(i) else { continue }
        let name = String(cString: nameC)
        guard name.contains("AMDRadeonX6000MTLDriver") else { continue }
        guard let hdr = _dyld_get_image_header(i) else { continue }
        let slide = _dyld_get_image_vmaddr_slide(i)

        // walk the load commands for __TEXT
        var base: UnsafeRawPointer? = nil
        var size = 0
        var cmd = UnsafeRawPointer(hdr).advanced(by: MemoryLayout<mach_header_64>.size)
        for _ in 0..<Int(hdr.pointee.ncmds) {
            let lc = cmd.assumingMemoryBound(to: load_command.self)
            if lc.pointee.cmd == UInt32(LC_SEGMENT_64) {
                let seg = cmd.assumingMemoryBound(to: segment_command_64.self)
                let segName = withUnsafeBytes(of: seg.pointee.segname) { raw -> String in
                    String(cString: raw.baseAddress!.assumingMemoryBound(to: CChar.self))
                }
                if segName == "__TEXT" {
                    base = UnsafeRawPointer(bitPattern: UInt(seg.pointee.vmaddr) + UInt(bitPattern: slide))
                    size = Int(seg.pointee.vmsize)
                }
            }
            cmd = cmd.advanced(by: Int(lc.pointee.cmdsize))
        }
        guard let b = base, size > 64 else {
            print("mtl-patch: found the driver but could not read its __TEXT"); return
        }
        let bytes = b.assumingMemoryBound(to: UInt8.self)
        var foundFind = 0, foundRepl = 0
        for o in 0..<(size - 32) {
            if bytes[o] == find[0] {
                var m = true
                for k in 0..<32 where bytes[o + k] != find[k] { m = false; break }
                if m { foundFind += 1 }
            }
            if bytes[o] == repl[0] {
                var m = true
                for k in 0..<14 where bytes[o + k] != repl[k] { m = false; break }
                if m { foundRepl += 1 }
            }
        }
        print("mtl-patch: \(name)")
        print("mtl-patch: original prologue present \(foundFind)x, LINEAR stub present \(foundRepl)x")
        print("mtl-patch: -> \(foundRepl > 0 && foundFind == 0 ? "PATCHED" : foundFind > 0 ? "NOT PATCHED" : "INCONCLUSIVE")")
        return
    }
    print("mtl-patch: AMDRadeonX6000MTLDriver is not loaded in this process")
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

// Only meaningful once Metal has loaded the driver, which creating the device
// and the pipeline has now done.
reportMetalDriverPatch()

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
