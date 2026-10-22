// permute.swift (2mz/2na) — name the swizzle from a scrambled screenprobe window.
//
// The scrambled window holds the probe's own pattern rearranged, and the
// rearrangement is identical every time it occurs. That is not memory
// corruption, which would differ each time; it is a fixed wrong address
// mapping - one layout written, another read.
//
// Every pixel carries its own coordinates: red is x & 255, green is y & 255,
// and blue carries the high bits of both, so a full (x,y) is recoverable, not
// just one modulo 256. Colour management is measured rather than assumed - the
// CLEAN window beside it in the same capture gives the intended-to-captured
// mapping for all 256 levels, and inverting that makes the scrambled window
// exactly decodable.
//
// With full coordinates the permutation becomes byte addresses, and then the
// question is answerable rather than arguable: does some known GFX10 swizzle
// reproduce the observed map? Each candidate is scored over every pixel, with
// the identity included as a control - a candidate that cannot beat the
// identity has explained nothing.
//
// build: make permute
// run:   build/out/permute <png> <cpuX> <cpuY> <gpuX> <gpuY> <wide> <tall>
import Foundation
import CoreGraphics
import ImageIO

let a = CommandLine.arguments
guard a.count >= 8 else { print("usage: permute <png> cpuX cpuY gpuX gpuY wide tall"); exit(1) }
let cpuX = Int(a[2])!, cpuY = Int(a[3])!, gpuX = Int(a[4])!, gpuY = Int(a[5])!
let WIDE = Int(a[6])!, TALL = Int(a[7])!

guard let isrc = CGImageSourceCreateWithURL(URL(fileURLWithPath: a[1]) as CFURL, nil),
      let img = CGImageSourceCreateImageAtIndex(isrc, 0, nil) else { print("cannot read png"); exit(2) }
let W = img.width, H = img.height
var buf = [UInt8](repeating: 0, count: W * H * 4)
buf.withUnsafeMutableBytes { raw in
    let bi = CGImageAlphaInfo.noneSkipFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue
    let ctx = CGContext(data: raw.baseAddress, width: W, height: H, bitsPerComponent: 8,
                        bytesPerRow: W * 4, space: CGColorSpaceCreateDeviceRGB(), bitmapInfo: bi)!
    ctx.draw(img, in: CGRect(x: 0, y: 0, width: W, height: H))
}
@inline(__always) func px(_ x: Int, _ y: Int) -> (Int, Int, Int) {
    let o = (y * W + x) * 4
    return (Int(buf[o + 2]), Int(buf[o + 1]), Int(buf[o]))
}

// --- the colour LUT, measured from the clean window -------------------------
var rS = [[Int]](repeating: [], count: 256)
var gS = [[Int]](repeating: [], count: 256)
var bS = [[Int]](repeating: [], count: 256)
for y in 20..<TALL where y % 64 != 0 && y % 256 != 0 {
    for x in 0..<WIDE where x % 64 != 0 && x % 256 != 0 {
        let (r, g, b) = px(gpuX + x, gpuY + y)
        rS[x & 0xff].append(r); gS[y & 0xff].append(g)
        bS[((x >> 8) & 0xf) | (((y >> 8) & 0xf) << 4)].append(b)
    }
}
func med(_ s: [[Int]]) -> [Int] { s.map { $0.isEmpty ? -1 : $0.sorted()[$0.count / 2] } }
let rFwd = med(rS), gFwd = med(gS), bFwd = med(bS)
func invert(_ fwd: [Int]) -> [Int] {
    (0..<256).map { c -> Int in
        var best = -1, bestD = Int.max
        for v in 0..<256 where fwd[v] >= 0 {
            let d = abs(fwd[v] - c); if d < bestD { bestD = d; best = v }
        }
        return best
    }
}
let rInv = invert(rFwd), gInv = invert(gFwd), bInv = invert(bFwd)

// --- decode the scrambled window to FULL source coordinates -----------------
var srcX = [Int](repeating: -1, count: WIDE * TALL)
var srcY = [Int](repeating: -1, count: WIDE * TALL)
var decoded = 0
for y in 20..<TALL where y % 64 != 0 && y % 256 != 0 {
    for x in 0..<WIDE where x % 64 != 0 && x % 256 != 0 {
        let (r, g, b) = px(cpuX + x, cpuY + y)
        let lo_x = rInv[r], lo_y = gInv[g], hi = bInv[b]
        if lo_x < 0 || lo_y < 0 || hi < 0 { continue }
        let sx = lo_x | ((hi & 0xf) << 8)
        let sy = lo_y | (((hi >> 4) & 0xf) << 8)
        if sx >= WIDE || sy >= TALL { continue }
        srcX[y * WIDE + x] = sx; srcY[y * WIDE + x] = sy; decoded += 1
    }
}
print("decoded \(decoded) pixels to full coordinates")
print("")

// --- candidate address functions --------------------------------------------
// 32 bits per pixel. A 256-byte micro tile is 8x8 pixels; a 64 KB block is
// 16x16 micro tiles, so 128x128 pixels. Offsets per docs/linux-ref and the
// AddrSwizzleMode list in CLAUDE.md: 0 LINEAR, 9 64KB_S, 22 4KB_D_X,
// 25 64KB_S_X, 27 64KB_R_X.
let PITCH = WIDE * 4

@inline(__always) func bit(_ v: Int, _ i: Int) -> Int { (v >> i) & 1 }

func linear(_ x: Int, _ y: Int) -> Int { y * PITCH + x * 4 }

// 64KB_S, 32bpp: [byte0 byte1 | x0 x1 x2 | y0 y1 y2 | x3 y3 x4 y4 x5 y5 x6 y6]
func sw64KB_S(_ x: Int, _ y: Int) -> Int {
    var o = 0
    o |= bit(x,0) << 2;  o |= bit(x,1) << 3;  o |= bit(x,2) << 4
    o |= bit(y,0) << 5;  o |= bit(y,1) << 6;  o |= bit(y,2) << 7
    o |= bit(x,3) << 8;  o |= bit(y,3) << 9
    o |= bit(x,4) << 10; o |= bit(y,4) << 11
    o |= bit(x,5) << 12; o |= bit(y,5) << 13
    o |= bit(x,6) << 14; o |= bit(y,6) << 15
    let tilesPerRow = (WIDE + 127) / 128
    let t = (y >> 7) * tilesPerRow + (x >> 7)
    return t * 65536 + o
}

// 4KB_D, 32bpp: the same micro tile, 4 KB block = 32x32 pixels
func sw4KB_D(_ x: Int, _ y: Int) -> Int {
    var o = 0
    o |= bit(x,0) << 2;  o |= bit(x,1) << 3;  o |= bit(x,2) << 4
    o |= bit(y,0) << 5;  o |= bit(y,1) << 6;  o |= bit(y,2) << 7
    o |= bit(x,3) << 8;  o |= bit(y,3) << 9
    o |= bit(x,4) << 10; o |= bit(y,4) << 11
    let tilesPerRow = (WIDE + 31) / 32
    let t = (y >> 5) * tilesPerRow + (x >> 5)
    return t * 4096 + o
}

// the _X variants XOR pipe/bank bits in from higher coordinates. GB_ADDR_CONFIG
// on this chip reads 0x242: 4 pipes, 4 packers, 1 shader engine, so 2 pipe bits.
func xorPipes(_ o: Int, _ x: Int, _ y: Int) -> Int {
    var v = o
    v ^= (bit(y,3) ^ bit(y,4)) << 8
    v ^= (bit(y,5) ^ bit(y,6)) << 9
    return v
}
func sw64KB_S_X(_ x: Int, _ y: Int) -> Int {
    let t = sw64KB_S(x, y)
    return (t & ~0xffff) | xorPipes(t & 0xffff, x, y)
}

// --- score every hypothesis over every decoded pixel ------------------------
// Two directions, because which side is tiled is not known in advance:
//   A: the data was written LINEAR and read with f   -> f(x,y) == linear(sx,sy)
//   B: the data was written with f and read LINEAR   -> linear(x,y) == f(sx,sy)
struct Cand { let name: String; let f: (Int, Int) -> Int }
let cands: [Cand] = [
    Cand(name: "identity (control)", f: linear),
    Cand(name: "64KB_S  (mode 9)", f: sw64KB_S),
    Cand(name: "64KB_S_X(mode 25)", f: sw64KB_S_X),
    Cand(name: "4KB_D   (mode 22)", f: sw4KB_D),
]
print("how well each candidate reproduces the observed permutation:")
print("   (direction A: f(dest) == linear(src);  direction B: linear(dest) == f(src))")
print("")
for c in cands {
    var hitA = 0, hitB = 0, n = 0
    for y in 20..<TALL {
        for x in 0..<WIDE {
            let sx = srcX[y * WIDE + x]; if sx < 0 { continue }
            let sy = srcY[y * WIDE + x]
            n += 1
            if c.f(x, y) == linear(sx, sy) { hitA += 1 }
            if linear(x, y) == c.f(sx, sy) { hitB += 1 }
        }
    }
    print(String(format: "   %-20s  A %6.2f%%   B %6.2f%%", (c.name as NSString).utf8String!,
                 100.0 * Double(hitA) / Double(n), 100.0 * Double(hitB) / Double(n)))
}
print("")

// --- characterise the permutation before trying to name it ------------------
// The per-bit probe this replaced used ONE sample per bit and produced a
// ragged table (rule 18: do not build a table from n=1 rows). Everything below
// has every decoded pixel voting.

// 1. is it a bijection? A swizzle rearranges a surface; it does not duplicate
//    or drop pixels. If sources are duplicated, this is not a pure remapping.
var useCount = [Int](repeating: 0, count: WIDE * TALL)
var considered = 0
for y in 20..<TALL { for x in 0..<WIDE {
    let sx = srcX[y * WIDE + x]; if sx < 0 { continue }
    useCount[srcY[y * WIDE + x] * WIDE + sx] += 1; considered += 1
}}
var once = 0, twice = 0, more = 0
for c in useCount { if c == 1 { once += 1 } else if c == 2 { twice += 1 } else if c > 2 { more += 1 } }
print("bijection check over \(considered) decoded pixels:")
print("   sources used exactly once: \(once),  twice: \(twice),  more: \(more)")
print("   -> \(once * 100 / max(1, considered))% of the mapping is one-to-one")
print("")

// 2. over what block is the displacement constant? A tiling permutation is
//    constant over whole micro tiles, so this names the tile.
print("displacement constant over blocks of:")
for b in [2, 4, 8, 16, 32, 64] {
    var good = 0, total = 0
    var by = 24
    while by + b <= TALL {
        var bx = 0
        while bx + b <= WIDE {
            var d: (Int, Int)? = nil
            var uniform = true, any = false
            for yy in by..<(by + b) { for xx in bx..<(bx + b) {
                let sx = srcX[yy * WIDE + xx]; if sx < 0 { continue }
                let dd = (sx - xx, srcY[yy * WIDE + xx] - yy)
                any = true
                if d == nil { d = dd } else if d! != dd { uniform = false }
            }}
            if any { total += 1; if uniform { good += 1 } }
            bx += b
        }
        by += b
    }
    print(String(format: "   %2dx%-2d : %5.1f%% of blocks", b, b,
                 100.0 * Double(good) / Double(max(1, total))))
}
print("")

// 3. is the displacement purely horizontal, purely vertical, or both?
var pureX = 0, pureY = 0, both = 0, none = 0, n3 = 0
for y in 20..<TALL { for x in 0..<WIDE {
    let sx = srcX[y * WIDE + x]; if sx < 0 { continue }
    let dx = sx - x, dy = srcY[y * WIDE + x] - y
    n3 += 1
    if dx == 0 && dy == 0 { none += 1 } else if dy == 0 { pureX += 1 }
    else if dx == 0 { pureY += 1 } else { both += 1 }
}}
print("displacement kind: in place \(none * 100 / max(1,n3))%, pure-X \(pureX * 100 / max(1,n3))%, pure-Y \(pureY * 100 / max(1,n3))%, both \(both * 100 / max(1,n3))%")
print("")

// 4. how far does data travel? A within-tile swizzle keeps everything inside
//    one tile; a pitch or base error moves it much further.
var maxAbsDx = 0, maxAbsDy = 0
var sameTile = 0, n4 = 0
for y in 20..<TALL { for x in 0..<WIDE {
    let sx = srcX[y * WIDE + x]; if sx < 0 { continue }
    let sy = srcY[y * WIDE + x]
    maxAbsDx = max(maxAbsDx, abs(sx - x)); maxAbsDy = max(maxAbsDy, abs(sy - y))
    n4 += 1
    if (sx >> 7) == (x >> 7) && (sy >> 7) == (y >> 7) { sameTile += 1 }
}}
print("travel: max |dx| = \(maxAbsDx), max |dy| = \(maxAbsDy)")
print("        stays inside its own 128x128 tile: \(sameTile * 100 / max(1,n4))%")

print("")
// --- what IS the relationship? ----------------------------------------------
// No tiling mode fits and nothing stays inside its tile, so the remaining
// simple families are a pitch mismatch (the two sides disagree about bytes per
// row), a scale mismatch, or a base offset. All three are visible by just
// looking at the addresses, so look at them.
print("sample of the mapping (dest -> source), with linear byte offsets:")
var shown = 0
var yy = 25
while yy < TALL && shown < 14 {
    var xx = 3
    while xx < WIDE && shown < 14 {
        let sx = srcX[yy * WIDE + xx]
        if sx >= 0 {
            let sy = srcY[yy * WIDE + xx]
            print(String(format: "   (%3d,%3d) -> (%3d,%3d)   Ld=0x%06x  Ls=0x%06x  Ls-Ld=%+d",
                         xx, yy, sx, sy, linear(xx, yy), linear(sx, sy),
                         linear(sx, sy) - linear(xx, yy)))
            shown += 1
        }
        xx += 37
    }
    yy += 31
}
print("")

// a pitch mismatch means  y*Pr + 4x  ==  sy*Pw + 4sx  for one pair (Pr,Pw).
// Score a family of write-side pitches against the observed map.
print("pitch-mismatch fit (read pitch fixed at \(PITCH) bytes):")
var bestP = -1, bestHits = 0
for pw in stride(from: 256, through: 16384, by: 64) {
    var hits = 0, n = 0
    var y2 = 21
    while y2 < TALL {
        var x2 = 1
        while x2 < WIDE {
            let sx = srcX[y2 * WIDE + x2]
            if sx >= 0 {
                n += 1
                if y2 * PITCH + 4 * x2 == srcY[y2 * WIDE + x2] * pw + 4 * sx { hits += 1 }
            }
            x2 += 7
        }
        y2 += 5
    }
    if hits > bestHits { bestHits = hits; bestP = pw }
}
print("   best write pitch \(bestP) bytes, matching \(bestHits) sampled pixels")

// a scale mismatch means  src == dest * k  for some k
print("scale fit:")
for (num, den) in [(2,1),(1,2),(3,2),(2,3),(4,3),(3,4)] {
    var hits = 0, n = 0
    var y3 = 21
    while y3 < TALL {
        var x3 = 1
        while x3 < WIDE {
            let sx = srcX[y3 * WIDE + x3]
            if sx >= 0 {
                n += 1
                if sx == x3 * num / den && srcY[y3 * WIDE + x3] == y3 * num / den { hits += 1 }
            }
            x3 += 7
        }
        y3 += 5
    }
    print(String(format: "   x%d/%d : %5.1f%%", num, den, 100.0 * Double(hits) / Double(max(1,n))))
}
