// decodescreen.swift (2mx) — read the screenprobe capture and say what the
// compositor did to the pattern.
//
// Two windows carried the SAME pattern through the SAME compositor on the same
// frame, one drawn by the CPU and one by the GPU. Diffing them against each
// other therefore cancels colour management exactly: whatever transform the
// display profile applies, it applies to both. No LUT has to be guessed at and
// no absolute pixel value has to be trusted.
//
// Three readings, cheapest first:
//   1. how many pixels differ between the two windows at all
//   2. for each GPU row, which CPU row it actually matches - a row permutation
//      shows up here as a mapping that is not the identity
//   3. the same by column
//
// Note the CPU window comes out vertically flipped: that is a bug in this
// repo's own CPUView (a flipped CGContext drawing a CGImage bottom-up), not
// something the machine did. It is undone here, and the check that it was
// undone correctly is that the grey calibration strip ends up at the top of
// both, which the tool asserts rather than assumes.
//
// build: make decodescreen      run: build/out/decodescreen <capture.png> [x0 y0 x1 y1 side]
import Foundation
import CoreGraphics
import ImageIO

let a = CommandLine.arguments
guard a.count >= 2 else { print("usage: decodescreen <png> [cpuX cpuY gpuX gpuY wide tall]"); exit(1) }
let cpuX = a.count > 2 ? Int(a[2])! : 100
let cpuY = a.count > 3 ? Int(a[3])! : 368
let gpuX = a.count > 4 ? Int(a[4])! : 652
let gpuY = a.count > 5 ? Int(a[5])! : 368
let WIDE = a.count > 6 ? Int(a[6])! : 512
let TALL = a.count > 7 ? Int(a[7])! : WIDE

guard let isrc = CGImageSourceCreateWithURL(URL(fileURLWithPath: a[1]) as CFURL, nil),
      let img = CGImageSourceCreateImageAtIndex(isrc, 0, nil) else { print("cannot read png"); exit(2) }
let W = img.width, H = img.height
print("capture \(W)x\(H)")

var buf = [UInt8](repeating: 0, count: W * H * 4)
buf.withUnsafeMutableBytes { raw in
    let cs = CGColorSpaceCreateDeviceRGB()
    let bi = CGImageAlphaInfo.noneSkipFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue
    let ctx = CGContext(data: raw.baseAddress, width: W, height: H, bitsPerComponent: 8,
                        bytesPerRow: W * 4, space: cs, bitmapInfo: bi)!
    ctx.draw(img, in: CGRect(x: 0, y: 0, width: W, height: H))
}

// (r,g,b) at an absolute capture coordinate
@inline(__always) func px(_ x: Int, _ y: Int) -> (Int, Int, Int) {
    let o = (y * W + x) * 4
    return (Int(buf[o + 2]), Int(buf[o + 1]), Int(buf[o]))
}

func grab(_ ox: Int, _ oy: Int, flip: Bool) -> [(Int, Int, Int)] {
    var o = [(Int, Int, Int)](repeating: (0, 0, 0), count: WIDE * TALL)
    for y in 0..<TALL {
        let sy = flip ? (TALL - 1 - y) : y
        for x in 0..<WIDE { o[y * WIDE + x] = px(ox + x, oy + sy) }
    }
    return o
}

// The calibration strip is a grey ramp in rows 0..15. Finding it is how the
// flip is confirmed instead of assumed: on a grey pixel r == g == b, and the
// pattern rows below are strongly coloured.
func greyRows(_ im: [(Int, Int, Int)]) -> Int {
    var n = 0
    for y in 0..<TALL {
        var grey = 0, seen = 0
        for x in stride(from: 4, to: WIDE, by: 7) {
            let (r, g, b) = im[y * WIDE + x]
            seen += 1
            if abs(r - g) <= 6 && abs(g - b) <= 6 { grey += 1 }
        }
        if grey > seen * 9 / 10 { n += 1 } else { break }
    }
    return n
}

let gpu = grab(gpuX, gpuY, flip: false)
var cpu = grab(cpuX, cpuY, flip: false)
let unflipped = grab(cpuX, cpuY, flip: true)
print("calibration strip at top:  gpu \(greyRows(gpu)) rows,  cpu \(greyRows(cpu)) rows,  cpu-unflipped \(greyRows(unflipped)) rows")
if greyRows(unflipped) > greyRows(cpu) {
    print("  -> CPU window was vertically flipped (this repo's drawing bug); undone")
    cpu = unflipped
} else {
    print("  -> CPU window not flipped; left as captured")
}
print("")

// 0. which window is broken, judged on its own.
//
// Diffing the two windows says they differ but not which one is wrong, and the
// 2my swap experiment needs to know exactly that. In the pattern green is
// y & 255 and red is x & 255, so in a correct window adjacent rows differ by
// about 1 and so do adjacent columns. A window scrambled into blocks does not.
// This needs no reference image and no colour management, because it only
// looks at differences within one window.
// A first attempt measured the step between ADJACENT rows and called the
// known-shredded window clean: the scrambled blocks are themselves smooth
// gradient fragments, so inside a block adjacent rows still step by 1. It
// failed the rule 22 check against a case known to be present, so it was
// replaced. What a displaced block does change is where a row sits as a whole,
// so this measures the row's own level and counts how often that jumps.
func jumps(_ im: [(Int, Int, Int)]) -> (Int, Int) {
    func med(_ v: [Double]) -> Double { v.sorted()[v.count / 2] }
    var rowLevel: [Int: Double] = [:], colLevel: [Int: Double] = [:]
    for y in 20..<TALL where y % 64 != 0 {
        var d: [Double] = []
        for x in stride(from: 3, to: WIDE, by: 11) where x % 64 != 0 {
            d.append(Double(im[y * WIDE + x].1))
        }
        rowLevel[y] = med(d)
    }
    for x in 0..<WIDE where x % 64 != 0 {
        var d: [Double] = []
        for y in stride(from: 20, to: TALL, by: 11) where y % 64 != 0 {
            d.append(Double(im[y * WIDE + x].0))
        }
        colLevel[x] = med(d)
    }
    // a jump is a step bigger than 8 that is not the pattern's own wrap at 256
    func count(_ lv: [Int: Double], _ n: Int) -> Int {
        var c = 0
        for i in 0..<(n - 1) {
            guard let a = lv[i], let b = lv[i + 1] else { continue }
            if a > 235 && b < 20 { continue }
            if abs(b - a) > 8 { c += 1 }
        }
        return c
    }
    return (count(rowLevel, TALL), count(colLevel, WIDE))
}
// The per-run stamp lives in blue and is the same for every pixel, so it
// survives any rearrangement: a scrambled window still reports the tag of the
// run that produced it. A scrambled window carrying an OLD tag is a stale
// surface, which is this harness's fault and not the machine's.
func tagOf(_ im: [(Int, Int, Int)]) -> Int {
    var h = [Int: Int]()
    for y in 20..<TALL where y % 64 != 0 && y % 256 != 0 {
        for x in 0..<WIDE where x % 64 != 0 && x % 256 != 0 {
            h[im[y * WIDE + x].2, default: 0] += 1
        }
    }
    return h.max { $0.value < $1.value }?.key ?? -1
}
print("run stamp (blue channel):  cpu tag \(tagOf(cpu)),  gpu tag \(tagOf(gpu))")

let (cr, cc) = jumps(cpu), (gr, gc) = jumps(gpu)
func verdict(_ r: Int, _ c: Int) -> String { (r <= 2 && c <= 2) ? "CLEAN" : "SCRAMBLED" }
print(String(format: "window state: cpu %@ (row jumps %d, col jumps %d),  gpu %@ (row jumps %d, col jumps %d)",
             verdict(cr, cc), cr, cc, verdict(gr, gc), gr, gc))
print("")

// 1. raw difference
var diff = 0, firstDiff: (Int, Int)? = nil
var chanMax = 0
var badRows = Set<Int>()
for y in 0..<TALL { for x in 0..<WIDE {
    let (ar, ag, ab) = cpu[y * WIDE + x], (br, bg, bb) = gpu[y * WIDE + x]
    let d = max(abs(ar - br), max(abs(ag - bg), abs(ab - bb)))
    if d > chanMax { chanMax = d }
    if d > 2 { diff += 1; badRows.insert(y); if firstDiff == nil { firstDiff = (x, y) } }
}}
print("pixels differing by more than 2/255 between the two windows: \(diff)/\(WIDE*TALL)")
if !badRows.isEmpty {
    let r = badRows.sorted()
    print("  rows touched: \(r.count)  first few: \(r.prefix(12).map(String.init).joined(separator: ","))")
}
print("  largest single-channel difference anywhere: \(chanMax)/255")
if let f = firstDiff { print("  first at (\(f.0),\(f.1))") }
print("")

// 2. row permutation. In the pattern green is y & 255, so a row's mean green
// identifies it. Grid lines and the calibration strip are skipped.
// Green is y & 255 and red is x & 255, so on their own every row is ambiguous
// with the row 256 away. Blue carries the high bits of both, so folding it in
// at a large weight makes the signature unique. The first version of this tool
// omitted it and reported a spurious "shift -256" for half the rows.
func rowMeans(_ im: [(Int, Int, Int)]) -> [Double] {
    (0..<TALL).map { y in
        var g = 0.0, b = 0.0, n = 0.0
        for x in 0..<WIDE where x % 64 != 0 {
            g += Double(im[y * WIDE + x].1); b += Double(im[y * WIDE + x].2); n += 1
        }
        return g / n + 1000.0 * ((b / n).rounded() / 16.0).rounded()
    }
}
func colMeans(_ im: [(Int, Int, Int)]) -> [Double] {
    (0..<WIDE).map { x in
        var r = 0.0, b = 0.0, n = 0.0
        for y in 16..<TALL where y % 64 != 0 {
            r += Double(im[y * WIDE + x].0); b += Double(im[y * WIDE + x].2); n += 1
        }
        return r / n + 1000.0 * (b / n).truncatingRemainder(dividingBy: 16.0).rounded()
    }
}

func permutation(_ label: String, _ src: [Double], _ dst: [Double], _ lo: Int, _ hi: Int) {
    var shifts: [Int: Int] = [:], worst: [(Int, Int)] = []
    for i in lo..<hi {
        var best = i, bestD = Double.greatestFiniteMagnitude
        for j in lo..<hi {
            let d = abs(src[i] - dst[j])
            if d < bestD { bestD = d; best = j }
        }
        shifts[best - i, default: 0] += 1
        if best != i && worst.count < 8 { worst.append((i, best)) }
    }
    let top = shifts.sorted { $0.value > $1.value }.prefix(5)
                    .map { "shift \($0.key): \($0.value)" }.joined(separator: ",  ")
    print("\(label): \(top)")
    if !worst.isEmpty {
        print("   non-identity: " + worst.map { "\($0.0)->\($0.1)" }.joined(separator: " "))
    }
}

permutation("GPU row -> matching CPU row", rowMeans(gpu), rowMeans(cpu), 16, TALL)
permutation("GPU col -> matching CPU col", colMeans(gpu), colMeans(cpu), 0, WIDE)
