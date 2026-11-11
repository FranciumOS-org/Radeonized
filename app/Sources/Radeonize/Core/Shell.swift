import Foundation

/// Runs a command-line tool and captures its output. Everything here is blocking;
/// call it from a detached task, never from the main actor.
enum Shell {
    struct Result {
        let status: Int32
        let data: Data
        let stderr: String

        var stdout: String { String(decoding: data, as: UTF8.self) }
    }

    static func run(_ tool: String, _ args: [String]) -> Result {
        let process = Process()
        process.executableURL = URL(fileURLWithPath: tool)
        process.arguments = args
        let out = Pipe()
        let err = Pipe()
        process.standardOutput = out
        process.standardError = err
        do {
            try process.run()
        } catch {
            return Result(status: -1, data: Data(), stderr: "\(error)")
        }
        // Read both pipes to the end before waiting, so a large output cannot fill
        // a pipe buffer and block the child.
        let data = out.fileHandleForReading.readDataToEndOfFile()
        let errData = err.fileHandleForReading.readDataToEndOfFile()
        process.waitUntilExit()
        return Result(status: process.terminationStatus,
                      data: data,
                      stderr: String(decoding: errData, as: UTF8.self))
    }

    /// Parses a property list from command output, or nil.
    static func plist(_ data: Data) -> Any? {
        try? PropertyListSerialization.propertyList(from: data, options: [], format: nil)
    }
}

enum RadeonizeError: LocalizedError {
    case message(String)

    var errorDescription: String? {
        switch self {
        case .message(let text): return text
        }
    }
}
