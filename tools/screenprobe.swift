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
// 2ng: geometry is the remaining difference between this reproducer and
// Terminal, which is crisp on this machine while ordinary windows here
// scramble about half the time. This project already has a law in which
// corruption depends on geometry (2mn), so sweep it: RB_W and RB_H override
// the window size.
let WIDE = ProcessInfo.processInfo.environment["RB_W"].flatMap { Int($0) } ?? (BIG ? 900 : 512)
let TALL = ProcessInfo.processInfo.environment["RB_H"].flatMap { Int($0) } ?? (BIG ? 880 : 512)
let MSAA = (MODE == "msaa" || MODE == "bigmsaa")
let VIA_IOSURFACE = (MODE == "iosurface" || MODE == "cpusurface" || MODE == "cgsurface")
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

// 2nf: the sweep killed the CPU-versus-GPU reading.
//
//   right window, CAMetalLayer, GPU-written        0 of 4 scrambled
//   right window, IOSurface, GPU-written           0 of 4
//   right window, IOSurface, CPU-written           0 of 4
//   left window,  AppKit's own backing store       7 of 12
//
// A CPU write is not what breaks it. What breaks it is AppKit's own layer
// backing store, and the probe had two differences rolled into one: the
// SURFACE (AppKit's, versus an IOSurface allocated here) and the DRAWING PATH
// (NSGraphicsContext into a view, versus raw stores into memory).
//
// cgsurface separates them: a CGContext over an IOSurface allocated here, drawn
// with the same CoreGraphics call the view uses. Clean means the surface is
// what matters; scrambled means CoreGraphics' drawing path is.
let CG_SURFACE = (MODE == "cgsurface")

// 2ng: is this reproducer even representative?
//
// Terminal draws into an AppKit backing store and is CRISP on this machine,
// while this probe's AppKit window scrambles. Same kind of surface, opposite
// outcomes - so something about these windows differs from a real app's, and
// the obvious candidate is that they are BORDERLESS and at .floating level,
// which the compositor may handle differently from an ordinary window.
//
// If an ordinary titled window at normal level is clean, this reproducer is an
// artefact of its own window configuration and must not be used to justify
// spending boots. If it scrambles too, the reproducer is representative and the
// contradiction with Terminal has to be explained some other way.
let NORMAL_WINDOW = (MODE == "normalwin")
// normalwin changes three things at once - style, level, and the backing
// store's height, since a titled window's frame is taller than its content.
// normallevel keeps the borderless style and only drops the window out of
// .floating, which separates the level from the rest.
let NORMAL_LEVEL = (MODE == "normallevel")

// 2nh: how are the losers distributed?
//
// 2ng established that a surface is right or wrong from creation and stays
// that way, about half and half. That is a property of the ALLOCATION, and
// the shape of its randomness says what kind:
//
//   independent ~50%   a per-surface coin flip - an alignment bit, or which
//                      of two pools the allocation landed in
//   strict alternation two pools used round-robin
//   runs/clusters      a pool that flips state, or an address range
//
// One window per run cannot tell these apart: it is one sample of one
// allocation. grid opens EIGHT windows at once in a single process and
// reports each, so the pattern across simultaneous allocations is visible in
// one capture.
let GRID = (MODE == "grid")
// The first grid ran at 256x256 and came back 48 of 48 scrambled - which says
// nothing about the distribution, because the size sweep had already measured
// 256x256 at 6 of 6. The rate is strongly geometry-dependent (100% at 256 and
// 128, about a third at 800x600), so the grid has to run at a geometry that
// actually sits near half if the pattern across simultaneous allocations is to
// be visible at all. Configurable, and it defaults to 512.
let GRID_SIDE = ProcessInfo.processInfo.environment["RB_GRID"].flatMap { Int($0) } ?? 512
let GRID_N = ProcessInfo.processInfo.environment["RB_GRIDN"].flatMap { Int($0) } ?? 6

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
        // 2ni: bytesPerRow is the one property of a backing store this probe
        // can vary freely. Surfaces allocated here with the system's preferred
        // alignment are clean in every run; CoreAnimation's are not, and its
        // stride is not observable from outside the process. So sweep it: if
        // some stride reproduces the defect in a surface allocated here, that
        // names the trigger.
        let natural = IOSurfaceAlignProperty(kIOSurfaceBytesPerRow, WIDE * 4)
        let bpr = ProcessInfo.processInfo.environment["RB_BPR"].flatMap { Int($0) } ?? natural
        if bpr != natural { print("surface: bytesPerRow \(bpr) (natural would be \(natural))") }
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
        if CG_SURFACE {
            // the same CoreGraphics draw the view does, but into a surface
            // allocated here instead of AppKit's backing store
            guard let s = surface else { return }
            IOSurfaceLock(s, [], nil)
            let bi = CGImageAlphaInfo.noneSkipFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue
            if let ctx = CGContext(data: IOSurfaceGetBaseAddress(s), width: WIDE, height: TALL,
                                   bitsPerComponent: 8, bytesPerRow: IOSurfaceGetBytesPerRow(s),
                                   space: CGColorSpaceCreateDeviceRGB(), bitmapInfo: bi) {
                // No flip here. A CGBitmapContext over an IOSurface already
                // has the surface's first row at the context's own origin, so
                // the transform the VIEW needs is one flip too many here - it
                // produced an upside-down window that the jump metric scored
                // as SCRAMBLED (6 row jumps, 0 column jumps), which was this
                // tool's bug and not the machine's.
                ctx.interpolationQuality = .none
                ctx.draw(CPUView.image, in: CGRect(x: 0, y: 0, width: WIDE, height: TALL))
            }
            IOSurfaceUnlock(s, [], nil)
            layer?.contents = surface
            layer?.contentsGravity = .topLeft
            return
        }
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
    let style: NSWindow.StyleMask = NORMAL_WINDOW ? [.titled, .closable] : [.borderless]
    let w = NSWindow(contentRect: NSRect(x: x, y: y, width: WIDE, height: TALL),
                     styleMask: style, backing: .buffered, defer: false)
    w.level = (NORMAL_WINDOW || NORMAL_LEVEL) ? .normal : .floating
    w.isOpaque = true
    w.backgroundColor = .black
    return w
}

let app = NSApplication.shared
// 2nl: this probe is not a real app - a bare binary with no bundle, running
// as .accessory. Terminal is an AppKit window drawn with CoreGraphics and is
// CRISP on this machine, while this probe's identical-looking window scrambles
// about half the time. That contradiction decides whether any of this matters:
// if CoreAnimation treats a non-bundled accessory process differently, the
// reproducer is an artefact of what it is rather than of what it draws.
//
// RB_REGULAR=1 makes it a regular, activated app instead.
if ProcessInfo.processInfo.environment["RB_REGULAR"] == "1" {
    app.setActivationPolicy(.regular)
    app.activate(ignoringOtherApps: true)
    print("activation policy: regular")
} else {
    app.setActivationPolicy(.accessory)
}

guard let screen = NSScreen.main else { print("no screen"); exit(2) }
let sh = Int(screen.frame.height)
let originY = BIG ? 100 : 200
let gap = BIG ? 40 : 40
let leftX = BIG ? 40 : 100
let rightX = leftX + WIDE + gap

if GRID {
    // 2ni: does merely ALLOCATING before the window is created poison it?
    //
    // 2nh showed the first backing store a process allocates is often right
    // and every later one is wrong. If that is an address or pressure
    // threshold rather than something about CoreAnimation's own bookkeeping,
    // then allocating plain IOSurfaces FIRST - nothing to do with a layer -
    // should push the first window into the bad region too.
    //
    //   RB_PREIOSURF=N   N plain IOSurfaces of the same size, before any window
    //   RB_PREBYTES=N    N megabytes of ordinary memory, touched, before any window
    //
    // If the IOSurfaces poison it and the plain memory does not, the threshold
    // is in the surface allocator rather than in memory pressure at large.
    var keepAlive: [IOSurface] = []
    if let n = ProcessInfo.processInfo.environment["RB_PREIOSURF"].flatMap({ Int($0) }), n > 0 {
        let bpr = IOSurfaceAlignProperty(kIOSurfaceBytesPerRow, GRID_SIDE * 4)
        for _ in 0..<n {
            if let s = IOSurface(properties: [.width: GRID_SIDE, .height: GRID_SIDE,
                                              .bytesPerElement: 4, .bytesPerRow: bpr,
                                              .pixelFormat: 0x42475241]) {
                _ = s.lock(options: [], seed: nil)
                memset(s.baseAddress, 0x40, s.allocationSize)
                _ = s.unlock(options: [], seed: nil)
                keepAlive.append(s)
            }
        }
        print("grid: pre-allocated \(keepAlive.count) IOSurfaces of \(GRID_SIDE)x\(GRID_SIDE)")
    }
    var ballast: [UnsafeMutableRawPointer] = []
    if let mb = ProcessInfo.processInfo.environment["RB_PREBYTES"].flatMap({ Int($0) }), mb > 0 {
        for _ in 0..<mb {
            let p = UnsafeMutableRawPointer.allocate(byteCount: 1 << 20, alignment: 4096)
            memset(p, 0x41, 1 << 20)
            ballast.append(p)
        }
        print("grid: pre-allocated \(ballast.count) MB of ordinary memory")
    }

    var wins: [NSWindow] = []
    for i in 0..<GRID_N {
        // origin overridable, so the grid can be put exactly where the
        // two-window probe puts its window and position ruled in or out
        let ox = ProcessInfo.processInfo.environment["RB_GRIDX"].flatMap { Int($0) } ?? 20
        let oy = ProcessInfo.processInfo.environment["RB_GRIDY"].flatMap { Int($0) } ?? 20
        let perRow = max(1, (1900 - 40) / (GRID_SIDE + 20))
        let gx = ox + (i % perRow) * (GRID_SIDE + 20)
        let gy = oy + (i / perRow) * (GRID_SIDE + 20)
        let w = NSWindow(contentRect: NSRect(x: gx, y: gy, width: GRID_SIDE, height: GRID_SIDE),
                         styleMask: [.borderless], backing: .buffered, defer: false)
        w.level = .floating
        w.isOpaque = true
        w.backgroundColor = .black
        w.contentView = CPUView(frame: NSRect(x: 0, y: 0, width: GRID_SIDE, height: GRID_SIDE))
        w.orderFrontRegardless()
        wins.append(w)
    }
    // the decoder compares two rectangles per call, so emit them in pairs
    let sh2 = Int(NSScreen.main!.frame.height)
    for i in stride(from: 0, to: GRID_N, by: 2) {
        let ox = ProcessInfo.processInfo.environment["RB_GRIDX"].flatMap { Int($0) } ?? 20
        let oy = ProcessInfo.processInfo.environment["RB_GRIDY"].flatMap { Int($0) } ?? 20
        let perRow = max(1, (1900 - 40) / (GRID_SIDE + 20))
        let ax = ox + (i % perRow) * (GRID_SIDE + 20)
        let ay = sh2 - (oy + (i / perRow) * (GRID_SIDE + 20)) - GRID_SIDE
        let j = min(i + 1, GRID_N - 1)
        let bx = ox + (j % perRow) * (GRID_SIDE + 20)
        let by = sh2 - (oy + (j / perRow) * (GRID_SIDE + 20)) - GRID_SIDE
        print("grid pair \(i),\(j): \(ax) \(ay) \(bx) \(by) \(GRID_SIDE) \(GRID_SIDE)")
    }
    // RB_GRIDMETAL=1 creates a Metal device and nothing else. The two-window
    // probe always does this and scrambles about half the time; grid mode
    // never did and scrambles always. If merely having a device changes the
    // rate, that is the variable.
    if ProcessInfo.processInfo.environment["RB_GRIDMETAL"] == "1" {
        if let d = MTLCreateSystemDefaultDevice() {
            print("grid: Metal device created (\(d.name))")
        } else {
            print("grid: NO METAL DEVICE - unarmed boot (rule 14)")
        }
    }
    if !READY.isEmpty { try? "ready\n".write(toFile: READY, atomically: true, encoding: .utf8) }
    Timer.scheduledTimer(withTimeInterval: SECONDS, repeats: false) { _ in app.terminate(nil) }
    app.run()
    exit(0)
}

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

print("cpu redraws: \(CPU_REDRAW), gpu drawn once: \(GPU_ONCE), normal window: \(NORMAL_WINDOW)")

Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) { _ in
    if CPU_REDRAW { cpuView.needsDisplay = true }
    if !GPU_ONCE { draw() }
}
Timer.scheduledTimer(withTimeInterval: SECONDS, repeats: false) { _ in app.terminate(nil) }
app.run()
