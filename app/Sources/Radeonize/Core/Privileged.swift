import Foundation

/// Runs radeonize-helper.sh as root through the standard macOS administrator prompt.
/// The helper is the only root code path; the app itself never runs as root.
enum Privileged {
    static var helperURL: URL? {
        Bundle.module.url(forResource: "radeonize-helper", withExtension: "sh")
    }

    static func run(_ args: [String]) async throws -> String {
        guard let helper = helperURL?.path else {
            throw RadeonizeError.message("The helper script is missing from this build.")
        }
        let command = (["/bin/sh", quote(helper)] + args.map(quote)).joined(separator: " ")
        // AppleScript string: escape backslashes and double quotes.
        let escaped = command
            .replacingOccurrences(of: "\\", with: "\\\\")
            .replacingOccurrences(of: "\"", with: "\\\"")
        let script = "do shell script \"\(escaped)\" with administrator privileges"

        let result = await Task.detached(priority: .userInitiated) {
            Shell.run("/usr/bin/osascript", ["-e", script])
        }.value

        guard result.status == 0 else {
            let message = result.stderr.trimmingCharacters(in: .whitespacesAndNewlines)
            if message.contains("-128") {
                throw RadeonizeError.message("Cancelled. Nothing was changed.")
            }
            throw RadeonizeError.message(message.isEmpty ? "The helper failed." : message)
        }
        return result.stdout.trimmingCharacters(in: .whitespacesAndNewlines)
    }

    /// POSIX single-quote escaping.
    static func quote(_ s: String) -> String {
        "'" + s.replacingOccurrences(of: "'", with: "'\\''") + "'"
    }
}

/// One saved state the app can restore: an EFI config.plist + kext, or the aux collection.
struct BackupItem: Identifiable, Hashable {
    enum Kind: String { case efi, framebuffer }

    let url: URL
    let kind: Kind
    let date: String

    var id: String { url.path }

    /// For EFI backups, the partition the backup was taken from (written by the helper).
    var efiDevice: String? {
        guard kind == .efi,
              let text = try? String(contentsOf: url.appendingPathComponent("efi-device"), encoding: .utf8) else { return nil }
        let trimmed = text.trimmingCharacters(in: .whitespacesAndNewlines)
        return trimmed.isEmpty ? nil : trimmed
    }

    var title: String {
        switch kind {
        case .efi: return "EFI plugin (\(date))"
        case .framebuffer: return "Framebuffer collection (\(date))"
        }
    }
}

enum BackupStore {
    static var root: URL {
        let dir = AppPaths.support.appendingPathComponent("backups", isDirectory: true)
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        return dir
    }

    /// A fresh, timestamped folder for one backup, e.g. 20261110-101500-efi.
    static func newFolder(_ kind: BackupItem.Kind) -> URL {
        let formatter = DateFormatter()
        formatter.locale = Locale(identifier: "en_US_POSIX")
        formatter.dateFormat = "yyyyMMdd-HHmmss"
        let stamp = formatter.string(from: Date())
        return root.appendingPathComponent("\(stamp)-\(kind.rawValue)", isDirectory: true)
    }

    static func list() -> [BackupItem] {
        let urls = (try? FileManager.default.contentsOfDirectory(at: root, includingPropertiesForKeys: nil)) ?? []
        return urls.compactMap { url in
            let name = url.lastPathComponent
            let parts = name.split(separator: "-")
            guard let last = parts.last, let kind = BackupItem.Kind(rawValue: String(last)) else { return nil }
            let date = parts.dropLast().joined(separator: "-")
            return BackupItem(url: url, kind: kind, date: date)
        }
        .sorted { $0.url.lastPathComponent > $1.url.lastPathComponent }
    }
}
