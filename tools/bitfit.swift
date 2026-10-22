// bitfit.swift (2na) — is the observed permutation an XOR-linear address map,
// and if so what is its bit matrix?
//
// No canned GFX10 swizzle reproduces the scrambled window (permute: 0.00% for
// 64KB_S, 64KB_S_X and 4KB_D in both directions) and nothing stays inside its
// own tile. But the permutation is far from random: each destination row
// gathers from exactly 16 source rows spaced 8 apart, all in one residue class
// mod 8. A regular stride like that is what an XOR-linear address map looks
// like, so test for one directly instead of guessing further named modes.
//
// The surface is WIDE*TALL pixels at 4 bytes each and the pitch is WIDE*4, so
// a byte offset is just 4 * (y*WIDE + x) and the whole question is about the
// pixel index p -> q. If the map is XOR-linear then
//
//     q(p) = q(p0) XOR (columns of the matrix for each bit set in p XOR p0)
//
// Each column is measured by toggling one bit of p over MANY reference points
// and taking the mode, not one sample (rule 18), and the agreement fraction is
// reported so a column that is not consistent cannot pass as one that is. The
// matrix is then used to predict every decoded pixel, which is the real test.
//
// Reads the decoded map from permute's own decode, duplicated here so the two
// tools cannot silently drift apart in what they mean by a coordinate.
//
// build: make bitfit
// run:   build/out/bitfit <png> cpuX cpuY gpuX gpuY wide tall
import Foundation
import CoreGraphics
import ImageIO

let a = CommandLine.arguments
guard a.count >= 8 else { print("usage: bitfit <png> cpuX cpuY gpuX gpuY wide tall"); exit(1) }
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

// colour LUT measured from the clean window, exactly as permute does
var rS = [[Int]](repeating: [], count: 256), gS = [[Int]](repeating: [], count: 256)
var bS = [[Int]](repeating: [], count: 256)
for y in 20..<TALL where y % 64 != 0 && y % 256 != 0 {
    for x in 0..<WIDE where x % 64 != 0 && x % 256 != 0 {
        let (r, g, b) = px(gpuX + x, gpuY + y)
        rS[x & 0xff].append(r); gS[y & 0xff].append(g)
        bS[((x >> 8) & 3) | (((y >> 8) & 3) << 2)].append(b)
    }
}
func med(_ s: [[Int]]) -> [Int] { s.map { $0.isEmpty ? -1 : $0.sorted()[$0.count / 2] } }
func invert(_ fwd: [Int]) -> [Int] {
    (0..<256).map { c -> Int in
        var best = -1, bestD = Int.max
        for v in 0..<256 where fwd[v] >= 0 { let d = abs(fwd[v] - c); if d < bestD { bestD = d; best = v } }
        return best
    }
}
let rInv = invert(med(rS)), gInv = invert(med(gS)), bInv = invert(med(bS))

// p -> q, or -1 where the pixel was not decodable
var q = [Int](repeating: -1, count: WIDE * TALL)
var decoded = 0
for y in 20..<TALL where y % 64 != 0 && y % 256 != 0 {
    for x in 0..<WIDE where x % 64 != 0 && x % 256 != 0 {
        let (r, g, b) = px(cpuX + x, cpuY + y)
        let lx = rInv[r], ly = gInv[g], hi = bInv[b]
        if lx < 0 || ly < 0 || hi < 0 { continue }
        let sx = lx | ((hi & 3) << 8), sy = ly | (((hi >> 2) & 3) << 8)
        if sx >= WIDE || sy >= TALL { continue }
        q[y * WIDE + x] = sy * WIDE + sx
        decoded += 1
    }
}
print("decoded \(decoded) pixels")
let NBITS = Int(ceil(log2(Double(WIDE * TALL))))
print("pixel index is \(NBITS) bits")
print("")

// measure one matrix column per bit, by voting
var col = [Int](repeating: -1, count: NBITS)
var agree = [Double](repeating: 0, count: NBITS)
for i in 0..<NBITS {
    var votes: [Int: Int] = [:]
    var tried = 0
    var p = 0
    while p < WIDE * TALL && tried < 4000 {
        let pp = p ^ (1 << i)
        if q[p] >= 0 && pp < WIDE * TALL && q[pp] >= 0 {
            votes[q[p] ^ q[pp], default: 0] += 1
            tried += 1
        }
        p += 7
    }
    if let best = votes.max(by: { $0.value < $1.value }) {
        col[i] = best.key
        agree[i] = Double(best.value) / Double(max(1, tried))
    }
}
print("matrix column per destination bit (delta in source index when that bit flips):")
for i in 0..<NBITS {
    let c = col[i]
    print(String(format: "   bit %2d -> 0x%05x   agreement %5.1f%%%@", i, c < 0 ? 0 : c,
                 100.0 * agree[i], agree[i] > 0.90 ? "   consistent" : ""))
}
// 0.98 was too strict: a few percent of decode noise is expected and the
// prediction over every pixel is the real test, so report it either way.
let consistent = agree.filter { $0 > 0.90 }.count
print("")
print("\(consistent)/\(NBITS) bits behave consistently")

// the real test: predict every decoded pixel from the matrix
var p0 = -1
for p in 0..<(WIDE * TALL) where q[p] >= 0 { p0 = p; break }
if p0 >= 0 {
    var hit = 0, n = 0
    for p in 0..<(WIDE * TALL) where q[p] >= 0 {
        var pred = q[p0]
        let d = p ^ p0
        for i in 0..<NBITS where (d >> i) & 1 == 1 { pred ^= col[i] }
        n += 1; if pred == q[p] { hit += 1 }
    }
    print(String(format: "XOR-linear model predicts %.2f%% of decoded pixels", 100.0 * Double(hit) / Double(n)))
} else {
    print("no decoded reference pixel; cannot test a prediction")
}
