import Foundation

/// Reads the LC_UUID of a thin 64-bit Mach-O file, so the app can check the
/// framebuffer's identity without `dwarfdump` (which needs Command Line Tools).
/// Output matches `dwarfdump --uuid`: upper-case, 8-4-4-4-12.
enum MachOUUID {
    private static let magic64: UInt32 = 0xFEEDFACF
    private static let loadUUID: UInt32 = 0x1B

    static func read(from url: URL) throws -> String {
        let data = try Data(contentsOf: url)

        func u32(_ offset: Int) -> UInt32? {
            guard offset >= 0, offset + 4 <= data.count else { return nil }
            var value: UInt32 = 0
            _ = withUnsafeMutableBytes(of: &value) { data.copyBytes(to: $0, from: offset..<offset + 4) }
            return UInt32(littleEndian: value)
        }

        guard u32(0) == magic64 else {
            throw RadeonizeError.message("\(url.lastPathComponent) is not a thin 64-bit Mach-O file.")
        }
        let commands = Int(u32(16) ?? 0)
        var offset = 32  // size of mach_header_64
        for _ in 0..<commands {
            guard let command = u32(offset), let size = u32(offset + 4), size >= 8 else { break }
            if command == loadUUID, offset + 24 <= data.count {
                let bytes = [UInt8](data[(offset + 8)..<(offset + 24)])
                let hex = bytes.map { String(format: "%02X", $0) }.joined()
                let parts = [0, 8, 12, 16, 20].enumerated().map { i, start -> String in
                    let from = hex.index(hex.startIndex, offsetBy: start)
                    let to = hex.index(from, offsetBy: i == 0 ? 8 : (i == 4 ? 12 : 4))
                    return String(hex[from..<to])
                }
                return parts.joined(separator: "-")
            }
            offset += Int(size)
        }
        throw RadeonizeError.message("\(url.lastPathComponent) has no LC_UUID load command.")
    }
}
