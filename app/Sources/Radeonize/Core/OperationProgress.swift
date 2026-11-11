import Foundation

/// A progress update from the app's own steps (the download, the checks).
enum ProgressEvent: Sendable {
    case step(Int, Int, String)   // step K of N, with its label
    case sub(Double?, String)     // progress inside the current step, if known
}

/// Live state of one operation. The privileged helper writes lines to a log file
/// that the app reads while it runs; the app's own steps report through `apply`.
/// Every line also goes into the log file on disk, so a stuck or failed run can
/// be looked at afterwards.
@MainActor
final class OperationProgress: ObservableObject {
    struct Step: Identifiable, Equatable {
        enum State: Equatable { case pending, running, done, failed }
        let id: Int
        var label: String
        var state: State
    }

    @Published private(set) var title = ""
    @Published private(set) var steps: [Step] = []
    @Published private(set) var log: [String] = []
    @Published private(set) var subFraction: Double?
    @Published private(set) var subText = ""
    @Published private(set) var isRunning = false
    @Published private(set) var startedAt = Date()
    @Published private(set) var lastActivity = Date()
    @Published private(set) var failure: String?
    @Published private(set) var logFile: URL?

    private var readOffset: UInt64 = 0
    private var pending = Data()
    private let maxLines = 2000

    /// 0...1 over all steps, counting partial progress inside the running one.
    var fraction: Double {
        guard !steps.isEmpty else { return 0 }
        let done = Double(steps.filter { $0.state == .done }.count)
        let inside = isRunning ? (subFraction ?? 0) : 0
        return min(1, (done + inside) / Double(steps.count))
    }

    // MARK: Lifecycle

    /// Starts a new operation and creates its log file. The header is written
    /// before the reader starts, so it is not read back twice.
    func begin(title: String, slug: String) {
        self.title = title
        steps = []
        log = []
        subFraction = nil
        subText = ""
        failure = nil
        pending = Data()
        isRunning = true
        startedAt = Date()
        lastActivity = startedAt

        let file = OperationLog.newFile(slug: slug)
        logFile = file
        note("Radeonize: \(title)")
        note("Started \(Self.clock(startedAt))")
        let attributes = try? FileManager.default.attributesOfItem(atPath: file.path)
        readOffset = (attributes?[.size] as? NSNumber)?.uint64Value ?? 0
    }

    func finish(success: Bool, message: String) {
        subFraction = nil
        subText = ""
        if success {
            steps = steps.map { step in
                var done = step
                done.state = .done
                return done
            }
        } else if let index = steps.firstIndex(where: { $0.state == .running }) {
            steps[index].state = .failed
        }
        failure = success ? nil : message
        isRunning = false
        note(success ? "Done." : "Stopped.")
        for line in message.split(separator: "\n", omittingEmptySubsequences: true) {
            note(String(line))
        }
    }

    // MARK: Input

    /// Updates from the app's own steps. Each step start is also written to the log file.
    func apply(_ event: ProgressEvent) {
        // Download callbacks can arrive just after the operation has finished.
        guard isRunning else { return }
        switch event {
        case let .step(number, total, label):
            setStep(number, of: total, label)
            note("▸ \(label)")
        case let .sub(fraction, text):
            subFraction = fraction
            subText = text
            lastActivity = Date()
        }
    }

    /// A line from the app itself: shown, and appended to the log file.
    func note(_ line: String) {
        show(line)
        guard let url = logFile, let data = (line + "\n").data(using: .utf8),
              let handle = try? FileHandle(forWritingTo: url) else { return }
        defer { try? handle.close() }
        _ = try? handle.seekToEnd()
        try? handle.write(contentsOf: data)
    }

    /// Reads whatever the helper has written since the last call. Called on a timer
    /// while the helper runs. Lines are shown here only; the helper already wrote them.
    func poll() {
        guard isRunning, let url = logFile, let handle = try? FileHandle(forReadingFrom: url) else { return }
        defer { try? handle.close() }
        do {
            try handle.seek(toOffset: readOffset)
            let data = (try? handle.readToEnd()) ?? Data()
            guard !data.isEmpty else { return }
            readOffset += UInt64(data.count)
            pending.append(data)
            while let newline = pending.firstIndex(of: 0x0A) {
                let line = String(decoding: pending[pending.startIndex..<newline], as: UTF8.self)
                pending.removeSubrange(pending.startIndex...newline)
                consume(line)
            }
        } catch {
            // The log is best effort: a read that fails just shows nothing new.
        }
    }

    // MARK: Helpers

    /// Helper lines: "@ K/N label" starts step K of N. Anything else is a log line.
    private func consume(_ line: String) {
        if line.hasPrefix("@ "), let space = line.dropFirst(2).firstIndex(of: " ") {
            let counts = line[line.index(line.startIndex, offsetBy: 2)..<space].split(separator: "/")
            let label = String(line[line.index(after: space)...])
            if counts.count == 2, let number = Int(counts[0]), let total = Int(counts[1]) {
                setStep(number, of: total, label)
                show("▸ \(label)")
                return
            }
        }
        show(line)
    }

    private func setStep(_ number: Int, of total: Int, _ label: String) {
        guard total > 0, (1...total).contains(number) else { return }
        if steps.count < total {
            steps += ((steps.count + 1)...total).map { Step(id: $0, label: "Step \($0)", state: .pending) }
        } else if steps.count > total {
            steps.removeLast(steps.count - total)
        }
        for index in steps.indices {
            if index < number - 1 {
                if steps[index].state != .failed { steps[index].state = .done }
            } else if index == number - 1 {
                steps[index].label = label
                steps[index].state = .running
            } else {
                steps[index].state = .pending
            }
        }
        subFraction = nil
        subText = ""
        lastActivity = Date()
    }

    private func show(_ line: String) {
        log.append(line)
        if log.count > maxLines { log.removeFirst(log.count - maxLines) }
        lastActivity = Date()
    }

    private static func clock(_ date: Date) -> String {
        let formatter = DateFormatter()
        formatter.locale = Locale(identifier: "en_US_POSIX")
        formatter.dateFormat = "yyyy-MM-dd HH:mm:ss"
        return formatter.string(from: date)
    }
}

/// Log files for each operation, kept under ~/Library/Application Support/Radeonize/logs.
enum OperationLog {
    static var folder: URL {
        let dir = AppPaths.support.appendingPathComponent("logs", isDirectory: true)
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        return dir
    }

    /// A new, empty log file, e.g. 20261111-101500-efi-install.log.
    static func newFile(slug: String) -> URL {
        let formatter = DateFormatter()
        formatter.locale = Locale(identifier: "en_US_POSIX")
        formatter.dateFormat = "yyyyMMdd-HHmmss"
        let url = folder.appendingPathComponent("\(formatter.string(from: Date()))-\(slug).log")
        FileManager.default.createFile(atPath: url.path, contents: nil)
        return url
    }
}
