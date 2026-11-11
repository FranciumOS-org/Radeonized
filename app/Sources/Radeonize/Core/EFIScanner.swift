import Foundation

/// An EFI system partition found on any disk, mounted or not.
struct EFIVolume: Identifiable, Hashable {
    let id: String              // device identifier, e.g. disk4s1
    let mountPoint: String?
    let internalDisk: Bool
    let solidState: Bool?
    let bus: String
    /// True when the partition is mounted and has EFI/OC/config.plist.
    /// False when it is unmounted or unreadable without root.
    let hasOpenCore: Bool

    var mounted: Bool { mountPoint != nil }

    var summary: String {
        var parts: [String] = [id]
        parts.append(internalDisk ? "internal" : "external")
        parts.append(bus)
        if let solidState { parts.append(solidState ? "SSD" : "HDD") }
        parts.append(mounted ? "mounted at \(mountPoint ?? "")" : "not mounted")
        return parts.joined(separator: " · ")
    }
}

enum EFIScanner {
    /// Lists every partition with the EFI content type, without needing root.
    static func scan() -> [EFIVolume] {
        let list = Shell.run("/usr/sbin/diskutil", ["list", "-plist"])
        guard let root = Shell.plist(list.data) as? [String: Any],
              let disks = root["AllDisksAndPartitions"] as? [[String: Any]] else { return [] }

        var ids: [String] = []
        for disk in disks {
            for part in disk["Partitions"] as? [[String: Any]] ?? [] where part["Content"] as? String == "EFI" {
                if let id = part["DeviceIdentifier"] as? String { ids.append(id) }
            }
        }
        return ids.compactMap(describe)
    }

    static func describe(_ id: String) -> EFIVolume? {
        guard let info = diskInfo(id) else { return nil }
        let mount = (info["MountPoint"] as? String).flatMap { $0.isEmpty ? nil : $0 }

        // Partitions do not report bus or SSD state; their parent disk does.
        let whole = (info["ParentWholeDisk"] as? String) ?? id
        let parent = diskInfo(whole) ?? info
        let internalDisk = (parent["Internal"] as? Bool) ?? false
        let solid = parent["SolidState"] as? Bool
        let bus = (parent["BusProtocol"] as? String) ?? "unknown bus"

        var hasOC = false
        if let mount {
            hasOC = FileManager.default.fileExists(atPath: mount + "/EFI/OC/config.plist")
        }
        return EFIVolume(id: id, mountPoint: mount, internalDisk: internalDisk,
                         solidState: solid, bus: bus, hasOpenCore: hasOC)
    }

    private static func diskInfo(_ id: String) -> [String: Any]? {
        let r = Shell.run("/usr/sbin/diskutil", ["info", "-plist", id])
        guard r.status == 0 else { return nil }
        return Shell.plist(r.data) as? [String: Any]
    }
}
