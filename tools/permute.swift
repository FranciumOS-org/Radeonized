// permute.swift (2mz) — read the exact address permutation out of a scrambled
// screenprobe window.
//
// The scrambled window is the probe's own pattern rearranged into blocks, and
// the rearrangement is identical every time it happens (84 row jumps, 92 column
// jumps, run after run). That is not memory corruption, which would differ each
// time; it is a fixed wrong address mapping - one layout written, another read.
//
// Every pixel of the pattern carries its own coordinates, so the permutation can
// be read off directly rather than guessed. The only obstacle is colour
// management, and the capture solves that itself: the GPU window beside it is
// CLEAN and carries the same pattern, so it gives the measured mapping from
// intended value to captured value for all 256 levels. Inverting that makes the
// scrambled window decodable exactly.
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

// 1. the colour LUT, measured from the CLEAN window rather than assumed.
//    intended value v -> captured value, per channel, median over all pixels
//    that should hold v.
var redSamples = [[Int]](repeating: [], count: 256)
var greenSamples = [[Int]](repeating: [], count: 256)
for y in 20..<TALL where y % 64 != 0 {
    for x in 0..<WIDE where x % 64 != 0 {
        let (r, g, _) = px(gpuX + x, gpuY + y)
        redSamples[x & 0xff].append(r)
        greenSamples[y & 0xff].append(g)
    }
}
func lut(_ s: [[Int]]) -> [Int] {
    s.map { v -> Int in v.isEmpty ? -1 : v.sorted()[v.count / 2] }
}
let rFwd = lut(redSamples), gFwd = lut(greenSamples)

// invert: captured -> intended, nearest
func invert(_ fwd: [Int]) -> [Int] {
    var inv = [Int](repeating: -1, count: 256)
    for c in 0..<256 {
        var best = -1, bestD = Int.max
        for v in 0..<256 where fwd[v] >= 0 {
            let d = abs(fwd[v] - c)
            if d < bestD { bestD = d; best = v }
        }
        inv[c] = best
    }
    return inv
}
let rInv = invert(rFwd), gInv = invert(gFwd)
let identity = (0..<256).allSatisfy { rFwd[$0] == $0 || rFwd[$0] < 0 }
print("colour transform measured from the clean window: \(identity ? "identity" : "non-identity, inverted")")

// 2. decode the scrambled window
var dmap = [String: Int]()
var decoded = [(Int, Int)](repeating: (-1, -1), count: WIDE * TALL)
var undecodable = 0
for y in 20..<TALL {
    for x in 0..<WIDE {
        let (r, g, _) = px(cpuX + x, cpuY + y)
        let sx = rInv[r], sy = gInv[g]
        if sx < 0 || sy < 0 { undecodable += 1; continue }
        decoded[y * WIDE + x] = (sx, sy)
        // red and green only carry the low 8 bits, so the displacement is
        // known modulo 256 - which is plenty for a tile-sized permutation
        var dx = (sx - x) & 0xff, dy = (sy - y) & 0xff
        if dx > 127 { dx -= 256 }
        if dy > 127 { dy -= 256 }
        dmap["\(dx),\(dy)", default: 0] += 1
    }
}
let total = WIDE * (TALL - 20)
print("decoded \(total - undecodable)/\(total) pixels")
print("")
print("displacement (source - position), most common first:")
for (k, v) in dmap.sorted(by: { $0.value > $1.value }).prefix(12) {
    print(String(format: "   d(%-9s) %7d  %5.1f%%", (k as NSString).utf8String!, v,
                 100.0 * Double(v) / Double(total)))
}
print("")

// 3. block structure: the largest tile over which the displacement is constant
print("displacement map, one cell per 8x8 block, rows 24..88 cols 0..128:")
for by in stride(from: 24, to: 88, by: 8) {
    var line = "  "
    for bx in stride(from: 0, to: 128, by: 8) {
        let (sx, sy) = decoded[by * WIDE + bx]
        if sx < 0 { line += "  ....  "; continue }
        var dx = (sx - bx) & 0xff, dy = (sy - by) & 0xff
        if dx > 127 { dx -= 256 }
        if dy > 127 { dy -= 256 }
        line += String(format: "%4d,%-4d", dx, dy)
    }
    print(line)
}
