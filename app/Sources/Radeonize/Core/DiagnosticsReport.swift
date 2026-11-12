import Foundation

/// Builds a diagnostics report on this Mac. Nothing is collected or sent until the
/// user asks for it, and the user sees (and can edit) the exact text before it goes.
enum DiagnosticsReport {
    static let endpoint = URL(string: "https://reports.example.com/report")!

    static func build() async -> String {
        await Task.detached(priority: .userInitiated) { collect() }.value
    }

    private static func collect() -> String {
        var out = ""
        func section(_ title: String, _ body: String) {
            out += "== \(title) ==\n\(body.isEmpty ? "(nothing)" : body)\n\n"
        }

        let os = ProcessInfo.processInfo.operatingSystemVersionString
        section("Radeonize", "app \(Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String ?? "dev")")
        section("System", "\(os)\nmodel \(CompatibilityCheck.sysctl("hw.model") ?? "unknown")")

        let gpus = CompatibilityCheck.displayDevices().map { "display \($0.label)" }.joined(separator: "\n")
        section("Display devices (vendor:device)", gpus)

        let kexts = Shell.run("/usr/bin/kmutil", ["showloaded"]).stdout
        section("Loaded kexts (Lilu, RembrandtGPU, RBAMD)", filter(kexts, ["lilu", "rembrandt", "rbamd", "rbgpu"]))

        let nvram = Shell.run("/usr/sbin/nvram", ["-p"]).stdout
        section("Boot args and rbgpu NVRAM keys", filter(nvram, ["boot-args", "rbgpu"]))

        // The driver's own verdict. These live in the IO registry, not in the
        // kernel log: the peer publishes them with setProperty and refreshes
        // them periodically, so a key that has not moved yet is not a key that
        // did not move. vbios_src 3 / acpivbios 0 is the VBIOS coming out of
        // this machine's ACPI VFCT table, which is what replaced the old
        // SysReport flow; armed 1 / arm_why 0 says the boot was armed at all,
        // and nothing else here means anything if it was not.
        let peer = Shell.run("/usr/sbin/ioreg", ["-rl", "-c", "RembrandtGPUPeer"]).stdout
        section("Driver diagnostics (RembrandtGPUPeer)", filter(
            peer.replacingOccurrences(of: ",", with: "\n"),
            ["armed", "arm_why", "vbios_src", "acpivb", "hwlibs_gate", "yc_swapped", "yc_refused"]))

        let log = Shell.run("/usr/bin/log", [
            "show", "--last", "15m", "--style", "compact",
            "--predicate", "eventMessage CONTAINS \"RembrandtGPU\" OR eventMessage CONTAINS \"RBAMD\" OR eventMessage CONTAINS \"Lilu\"",
        ]).stdout
        section("Kernel log, last 15 minutes (matching lines)", lastLines(log, 400))

        section("Newest panic report (first 200 lines)", latestPanic())

        section("Last Radeonize operation log (newest 300 lines)", latestOperationLog())

        return redact(out)
    }

    private static func latestOperationLog() -> String {
        let files = (try? FileManager.default.contentsOfDirectory(
            at: OperationLog.folder, includingPropertiesForKeys: [.contentModificationDateKey])) ?? []
        guard let newest = files.filter({ $0.pathExtension == "log" }).max(by: { modified($0) < modified($1) }),
              let text = try? String(contentsOf: newest, encoding: .utf8) else {
            return "no operation has run yet"
        }
        return "\(newest.lastPathComponent)\n" + lastLines(text, 300)
    }

    private static func filter(_ text: String, _ needles: [String]) -> String {
        text.split(separator: "\n", omittingEmptySubsequences: true)
            .filter { line in needles.contains { line.lowercased().contains($0) } }
            .joined(separator: "\n")
    }

    private static func lastLines(_ text: String, _ count: Int) -> String {
        text.split(separator: "\n").suffix(count).joined(separator: "\n")
    }

    private static func latestPanic() -> String {
        let dir = URL(fileURLWithPath: "/Library/Logs/DiagnosticReports")
        let files = (try? FileManager.default.contentsOfDirectory(
            at: dir, includingPropertiesForKeys: [.contentModificationDateKey])) ?? []
        let panics = files.filter { $0.pathExtension == "panic" || $0.lastPathComponent.hasPrefix("kernel") }
        guard let newest = panics.max(by: { modified($0) < modified($1) }),
              let text = try? String(contentsOf: newest, encoding: .utf8) else {
            return "no panic report readable (none, or macOS keeps them root-only)"
        }
        return "\(newest.lastPathComponent)\n" + lastLines(text, 200)
    }

    private static func modified(_ url: URL) -> Date {
        (try? url.resourceValues(forKeys: [.contentModificationDateKey]).contentModificationDate) ?? .distantPast
    }

    /// Removes what identifies the person or the machine: home path, user name,
    /// host name, UUIDs, MAC and IP addresses. The serial number is never collected.
    static func redact(_ text: String) -> String {
        var t = text
        let user = NSUserName()
        t = t.replacingOccurrences(of: NSHomeDirectory(), with: "~")
        if user.count >= 3 { t = t.replacingOccurrences(of: user, with: "<user>") }
        let host = ProcessInfo.processInfo.hostName
        if host.count >= 3 { t = t.replacingOccurrences(of: host, with: "<host>") }

        let patterns: [(String, String)] = [
            (#"[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{12}"#, "<uuid>"),
            (#"([0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}"#, "<mac>"),
            (#"\b\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3}\b"#, "<ip>"),
        ]
        for (pattern, replacement) in patterns {
            t = t.replacingOccurrences(of: pattern, with: replacement, options: .regularExpression)
        }
        return t
    }

    /// Sends the text the user approved. Returns the report ID the server assigned.
    static func send(_ text: String) async throws -> String {
        var request = URLRequest(url: endpoint)
        request.httpMethod = "POST"
        request.setValue("text/plain; charset=utf-8", forHTTPHeaderField: "Content-Type")
        request.setValue("Radeonize", forHTTPHeaderField: "User-Agent")
        request.httpBody = Data(text.utf8)
        request.timeoutInterval = 30

        let (data, response) = try await URLSession.shared.data(for: request)
        let code = (response as? HTTPURLResponse)?.statusCode ?? 0
        guard (200...299).contains(code) else {
            throw RadeonizeError.message("The report server answered \(code). Nothing was kept.")
        }
        return String(decoding: data, as: UTF8.self).trimmingCharacters(in: .whitespacesAndNewlines)
    }
}
