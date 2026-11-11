import Foundation

/// A PCI vendor:device pair, read from the device's own registers.
struct GPUID: Hashable {
    let vendor: UInt32
    let device: UInt32

    var label: String { String(format: "%04x:%04x", vendor, device) }
}

/// What this Mac is, and whether Radeonize can work on it.
struct Compatibility {
    static let amdVendor: UInt32 = 0x1002
    static let nvidiaVendor: UInt32 = 0x10de
    /// Rembrandt, the two PCI IDs the driver matches (see IOPCIMatch in src/plugin/Info.plist):
    /// 1681 (Radeon 680M / 660M / 610M) and 164d (second ID for the same die).
    static let rembrandtIDs: Set<GPUID> = [
        GPUID(vendor: 0x1002, device: 0x1681),
        GPUID(vendor: 0x1002, device: 0x164d),
    ]
    /// macOS majors the Radeonized project has measured: Sequoia and Tahoe.
    static let supportedMacOS: Set<Int> = [15, 26]

    var model = "unknown"
    var osVersion = ""
    var osMajor = 0
    var isAppleSilicon = false
    var displays: [GPUID] = []

    var hasRembrandt: Bool { displays.contains { Compatibility.rembrandtIDs.contains($0) } }
    /// An NVIDIA display device on the bus means the chassis is in hybrid mode.
    var hasNVIDIADisplay: Bool { displays.contains { $0.vendor == Compatibility.nvidiaVendor } }
    var osSupported: Bool { Compatibility.supportedMacOS.contains(osMajor) }

    /// Reasons install is blocked. Empty means install may proceed.
    var blockers: [String] {
        var out: [String] = []
        if isAppleSilicon {
            out.append("This is an Apple silicon Mac. Radeonized is for Intel-based (x86) Macs only.")
        }
        if !hasRembrandt {
            out.append("No Rembrandt GPU (1002:1681 or 1002:164d) was found on the PCI bus.")
        }
        if !osSupported {
            out.append("macOS \(osVersion) is not one of the measured versions (Sequoia 15, Tahoe 26).")
        }
        return out
    }

    /// Things the user should know, even when install is allowed.
    var warnings: [String] {
        var out: [String] = []
        if hasNVIDIADisplay {
            out.append("An NVIDIA GPU is also active, so the chassis is probably in hybrid mode. "
                + "The 680M only drives the built-in panel in iGPU-only (Eco) mode; set that in the firmware first.")
        }
        return out
    }
}

enum CompatibilityCheck {
    static func run() -> Compatibility {
        var c = Compatibility()
        let os = ProcessInfo.processInfo.operatingSystemVersion
        c.osMajor = os.majorVersion
        c.osVersion = "\(os.majorVersion).\(os.minorVersion).\(os.patchVersion)"
        c.model = sysctl("hw.model") ?? "unknown"
        c.isAppleSilicon = sysctl("hw.optional.arm64") == "1"
        c.displays = displayDevices()
        return c
    }

    static func sysctl(_ name: String) -> String? {
        let r = Shell.run("/usr/sbin/sysctl", ["-n", name])
        guard r.status == 0 else { return nil }
        let text = r.stdout.trimmingCharacters(in: .whitespacesAndNewlines)
        return text.isEmpty ? nil : text
    }

    /// Display-class (0x03) PCI devices, read from ioreg. Vendor and device IDs are
    /// little-endian Data; class-code is 00 00 03 00 for a display controller.
    static func displayDevices() -> [GPUID] {
        let r = Shell.run("/usr/sbin/ioreg", ["-r", "-c", "IOPCIDevice", "-d", "1", "-a"])
        guard r.status == 0, let root = Shell.plist(r.data) else { return [] }
        var found = Set<GPUID>()

        func le16(_ d: Data) -> UInt32 {
            let b = [UInt8](d)
            return UInt32(b[0]) | (UInt32(b[1]) << 8)
        }

        func walk(_ node: Any) {
            if let dict = node as? [String: Any] {
                if let v = dict["vendor-id"] as? Data, let d = dict["device-id"] as? Data,
                   let cls = dict["class-code"] as? Data, v.count >= 2, d.count >= 2, cls.count >= 3,
                   [UInt8](cls)[2] == 0x03 {
                    found.insert(GPUID(vendor: le16(v), device: le16(d)))
                }
                for child in dict.values { walk(child) }
            } else if let list = node as? [Any] {
                for item in list { walk(item) }
            }
        }
        walk(root)
        return found.sorted { ($0.vendor, $0.device) < ($1.vendor, $1.device) }
    }
}
