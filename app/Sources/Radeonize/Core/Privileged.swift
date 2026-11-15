import Foundation

/// Runs radeonize-helper.sh as root through the standard macOS administrator prompt.
/// The helper is the only root code path; the app itself never runs as root.
enum Privileged {
    /// Where the last lookup looked, so a failure can say so instead of
    /// guessing. Set by `helperURL`.
    private(set) static var searchedPaths: [String] = []

    /// The bundled helper script, or nil.
    ///
    /// ISSUE #6: THIS USED TO BE `Bundle.module`, WHICH TRAPS.
    /// SwiftPM generates `Bundle.module` as a `fatalError` on the failure
    /// path - "unable to find bundle named Radeonize_Radeonize" - so a
    /// resource lookup that comes up empty is not a nil, it is
    /// EXC_BAD_INSTRUCTION (SIGILL) before any of this app's own code runs.
    /// That made the `guard let helper = helperURL?.path` below, and its
    /// "The helper script is missing from this build." message, DEAD CODE:
    /// the process was already gone. A reporter on Sequoia 15.8.3 saw exactly
    /// that, crashing in `NSBundle.module` while reaching for
    /// `Privileged.helperURL`, before the administrator prompt.
    ///
    /// Why the lookup can fail at all is NOT established. The nested
    /// `Radeonize_Radeonize.bundle` that SwiftPM puts the script in is
    /// present in the shipped .app and the shipped DMG, and the ad-hoc
    /// signature seals only two files, so a downloaded and quarantined copy
    /// is a plausible difference from a locally built one - but that is a
    /// candidate, not a finding, and this app has no way to measure it on
    /// someone else's machine.
    ///
    /// So the fix does not depend on knowing: look in every place the script
    /// can legitimately be, prefer the flat copy that `make-app.sh` now
    /// writes straight into `Contents/Resources` with no nested bundle, fall
    /// back to the SwiftPM bundle for `swift run`, and return nil - with the
    /// paths tried recorded - instead of killing the process.
    static var helperURL: URL? {
        let name = "radeonize-helper"
        let ext  = "sh"
        var tried: [String] = []

        func check(_ url: URL?) -> URL? {
            guard let url else { return nil }
            tried.append(url.path)
            return FileManager.default.isReadableFile(atPath: url.path) ? url : nil
        }

        // 1. The flat copy in the app's own Resources. No nested bundle, so
        //    nothing to load, seal or translocate separately.
        if let hit = check(Bundle.main.resourceURL?
                              .appendingPathComponent("\(name).\(ext)")) {
            searchedPaths = tried
            return hit
        }
        // 2. Anything the main bundle will hand us by name.
        if let hit = check(Bundle.main.url(forResource: name, withExtension: ext)) {
            searchedPaths = tried
            return hit
        }
        // 3. The SwiftPM resource bundle, by PATH rather than through
        //    `Bundle.module`, so a miss is a nil and not a trap. Both the
        //    .app layout and `swift build`'s output directory.
        let bundleName = "Radeonize_Radeonize.bundle"
        let roots = [Bundle.main.resourceURL,
                     Bundle.main.bundleURL,
                     Bundle.main.executableURL?.deletingLastPathComponent()]
        for root in roots {
            guard let root else { continue }
            let inner = root.appendingPathComponent(bundleName)
            if let b = Bundle(url: inner),
               let hit = check(b.url(forResource: name, withExtension: ext)) {
                searchedPaths = tried
                return hit
            }
            if let hit = check(inner.appendingPathComponent("\(name).\(ext)")) {
                searchedPaths = tried
                return hit
            }
        }
        searchedPaths = tried
        return nil
    }

    /// Runs one helper command. While it runs, the helper's log file is read every
    /// 0.3 s so the window can show each step as it starts.
    @MainActor
    static func run(_ args: [String], progress: OperationProgress) async throws -> String {
        guard let helper = helperURL?.path else {
            // Reachable now that the lookup returns nil instead of trapping,
            // and it names where it looked so a bug report is actionable.
            let where_ = searchedPaths.isEmpty
                ? "no candidate paths could be formed"
                : searchedPaths.joined(separator: "\n  ")
            throw RadeonizeError.message(
                "The helper script is missing from this build. Looked in:\n  "
                + where_
                + "\n\nRe-download Radeonize, or run the install commands by hand "
                + "from the project README.")
        }
        guard let logFile = progress.logFile?.path else {
            throw RadeonizeError.message("No log file for this step.")
        }
        progress.note("Waiting for your administrator password…")

        let parts = ["RADEONIZE_PROGRESS=\(quote(logFile))", "/bin/sh", quote(helper)] + args.map(quote)
        let command = parts.joined(separator: " ")
        // AppleScript string: escape backslashes and double quotes.
        let escaped = command
            .replacingOccurrences(of: "\\", with: "\\\\")
            .replacingOccurrences(of: "\"", with: "\\\"")
        let script = "do shell script \"\(escaped)\" with administrator privileges"

        let poller = Task { @MainActor in
            while !Task.isCancelled {
                progress.poll()
                try? await Task.sleep(nanoseconds: 300_000_000)
            }
        }
        let result = await Task.detached(priority: .userInitiated) {
            Shell.run("/usr/bin/osascript", ["-e", script])
        }.value
        poller.cancel()
        progress.poll()

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
