import Foundation

/// The machine's own video BIOS, taken out of its ACPI VFCT table and written
/// into the plugin's `Info.plist`.
///
/// A released RembrandtGPU.kext carries no VBIOS: the image belongs to the
/// board vendor, so the only copy that moves is the one already on the board
/// this runs on. Apple's graphics stack reads it as the `ATY,bin_image`
/// property of the PCI device, and the plugin forwards whatever its own
/// personality carries, so putting the image in the installed bundle is all
/// that is needed.
///
/// On an APU there is no ROM to read the image from. It lives in an ACPI table
/// called VFCT, which macOS does not publish in the IO registry on this
/// platform — `ioreg` has no ACPI tables at all under OpenCore. The one way to
/// get at it is OpenCore's own SysReport, which dumps the tables to the EFI
/// partition on the next boot. So the flow costs one extra reboot:
///
///   1. `sysreport on` through the helper, then reboot.
///   2. `imageFromSysReport` reads `EFI/SysReport/ACPI/VFCT*.aml`.
///   3. `inject` writes the image into the staged kext.
///   4. `sysreport off`, then install and reboot as usual.
///
/// Parsing follows Linux's `amdgpu_acpi_vfct_bios()`
/// (drivers/gpu/drm/amd/amdgpu/amdgpu_bios.c) and the check follows its
/// `check_atom_bios()`. The Python at `tools/vfct.py` does the same thing and
/// is the reference this was written against; the two agree byte for byte on
/// this machine's table, sha256 27ed5802…, 44544 bytes.
enum VFCT {
    /// Both Rembrandt PCI ids: 0x1681 is the 680M/660M, 0x164d the plain one.
    /// A table carries only its own board's image, so each is tried in turn.
    static let deviceIDs: [(vendor: UInt16, device: UInt16)] = [(0x1002, 0x1681), (0x1002, 0x164D)]

    static let personality = "RembrandtGPUPeer"
    static let propertyKey = "ATY,bin_image"

    // MARK: - the table

    private static let headerLength = 0x4C      // ACPI header + TableUUID + the two offsets
    private static let imageOffsetField = 0x34  // VBIOSImageOffset, u32
    private static let imageHeaderLength = 28   // VFCT_IMAGE_HEADER

    private struct Image {
        let vendor: UInt16
        let device: UInt16
        let data: Data
    }

    /// Finds the newest VFCT dump on a mounted EFI partition and returns the
    /// Rembrandt image in it.
    static func imageFromSysReport(efiMountPoint: String) throws -> Data {
        let dir = URL(fileURLWithPath: efiMountPoint)
            .appendingPathComponent("SysReport")
            .appendingPathComponent("ACPI")
        let names = (try? FileManager.default.contentsOfDirectory(atPath: dir.path)) ?? []
        let tables = names
            .filter { $0.hasPrefix("VFCT") && $0.hasSuffix(".aml") }
            .sorted()
        guard let name = tables.last else {
            throw RadeonizeError.message(
                "No SysReport/ACPI/VFCT*.aml on the EFI partition. Turn SysReport on and reboot "
                + "once, so OpenCore dumps the ACPI tables, then run this step again.")
        }
        return try image(fromTable: try Data(contentsOf: dir.appendingPathComponent(name)),
                         named: name)
    }

    /// Reads one VFCT table and returns the first image matching a Rembrandt id.
    static func image(fromTable table: Data, named name: String) throws -> Data {
        let images = try parse(table, named: name)
        for id in deviceIDs {
            if let hit = images.first(where: { $0.vendor == id.vendor && $0.device == id.device }) {
                try check(hit.data, named: name)
                return hit.data
            }
        }
        let have = images.map { String(format: "%04x:%04x", $0.vendor, $0.device) }.joined(separator: ", ")
        throw RadeonizeError.message(
            "\(name) carries no Rembrandt image"
            + (have.isEmpty ? " and no images at all." : " (it carries \(have))."))
    }

    private static func parse(_ table: Data, named name: String) throws -> [Image] {
        func u16(_ offset: Int) -> UInt16? {
            guard offset >= 0, offset + 2 <= table.count else { return nil }
            return UInt16(table[table.startIndex + offset]) | UInt16(table[table.startIndex + offset + 1]) << 8
        }
        func u32(_ offset: Int) -> UInt32? {
            guard offset >= 0, offset + 4 <= table.count else { return nil }
            var value: UInt32 = 0
            for i in (0..<4).reversed() { value = value << 8 | UInt32(table[table.startIndex + offset + i]) }
            return value
        }

        guard table.count >= headerLength else {
            throw RadeonizeError.message("\(name) is \(table.count) bytes; too short for a VFCT table.")
        }
        guard Array(table.prefix(4)) == Array("VFCT".utf8) else {
            throw RadeonizeError.message("\(name) does not start with the VFCT signature.")
        }
        guard let declared = u32(4), declared >= UInt32(headerLength), Int(declared) <= table.count else {
            throw RadeonizeError.message("\(name) declares a length that does not fit the file.")
        }

        var images: [Image] = []
        var offset = Int(u32(imageOffsetField) ?? 0)
        guard offset >= headerLength else {
            throw RadeonizeError.message("\(name) points its first image at offset \(offset).")
        }
        let end = Int(declared)
        // Every step is bounded by the table's own declared length, never by a
        // terminator, so a truncated or hostile table cannot walk off the end.
        while offset + imageHeaderLength <= end {
            guard let vendor = u16(offset + 12), let device = u16(offset + 14),
                  let length = u32(offset + 24) else { break }
            let start = offset + imageHeaderLength
            if length == 0 { break }
            guard start + Int(length) <= end else { break }
            images.append(Image(vendor: vendor, device: device,
                                data: table.subdata(in: (table.startIndex + start)..<(table.startIndex + start + Int(length)))))
            offset = start + Int(length)
        }
        return images
    }

    /// Linux's `check_atom_bios()`: an option ROM of at least 0x49 bytes
    /// starting 0x55 0xAA, with a non-zero BIOS header pointer at 0x48 and
    /// "ATOM" or "MOTA" four bytes past it.
    static func check(_ rom: Data, named name: String) throws {
        func fail(_ why: String) -> Error { RadeonizeError.message("The image in \(name) \(why).") }
        guard rom.count >= 0x49 else { throw fail("is \(rom.count) bytes, too short for an option ROM") }
        let b = [UInt8](rom)
        guard b[0] == 0x55, b[1] == 0xAA else { throw fail("does not start 0x55 0xAA") }
        let header = Int(b[0x48]) | Int(b[0x49]) << 8   // the u16 at 0x48
        guard header != 0, header + 8 <= b.count else { throw fail("has no BIOS header pointer at 0x48") }
        let tag = String(bytes: b[(header + 4)..<(header + 8)], encoding: .ascii) ?? ""
        guard tag == "ATOM" || tag == "MOTA" else {
            throw fail("has \"\(tag)\" where ATOMBIOS expects \"ATOM\"")
        }
    }

    // MARK: - the bundle

    /// Writes the image into a staged RembrandtGPU.kext's `Info.plist`, under
    /// the peer's personality, and reads it back to confirm it took.
    static func inject(_ rom: Data, intoKextAt kext: URL) throws {
        let plist = kext.appendingPathComponent("Contents").appendingPathComponent("Info.plist")
        guard FileManager.default.fileExists(atPath: plist.path) else {
            throw RadeonizeError.message("\(kext.lastPathComponent) has no Contents/Info.plist.")
        }
        var format = PropertyListSerialization.PropertyListFormat.xml
        let parsed = try PropertyListSerialization.propertyList(
            from: try Data(contentsOf: plist), options: [], format: &format)
        guard var root = parsed as? [String: Any],
              var personalities = root["IOKitPersonalities"] as? [String: Any],
              var peer = personalities[personality] as? [String: Any] else {
            throw RadeonizeError.message(
                "\(kext.lastPathComponent) has no \(personality) personality; it is not a "
                + "RembrandtGPU.kext this app knows how to prepare.")
        }

        peer[propertyKey] = rom
        personalities[personality] = peer
        root["IOKitPersonalities"] = personalities
        let out = try PropertyListSerialization.data(fromPropertyList: root, format: .xml, options: 0)
        try out.write(to: plist, options: .atomic)

        guard let written = try readImage(fromKextAt: kext), written == rom else {
            throw RadeonizeError.message(
                "Wrote the VBIOS into \(kext.lastPathComponent) but read back something else. "
                + "Nothing was installed.")
        }
    }

    /// The image a staged or installed kext is carrying, if any.
    static func readImage(fromKextAt kext: URL) throws -> Data? {
        let plist = kext.appendingPathComponent("Contents").appendingPathComponent("Info.plist")
        guard let blob = try? Data(contentsOf: plist) else { return nil }
        var format = PropertyListSerialization.PropertyListFormat.xml
        let parsed = try PropertyListSerialization.propertyList(from: blob, options: [], format: &format)
        return ((parsed as? [String: Any])?["IOKitPersonalities"] as? [String: Any])?[personality]
            .flatMap { ($0 as? [String: Any])?[propertyKey] as? Data }
    }
}
