import CryptoKit
import Foundation

/// A verified, unpacked release: the two kexts Radeonized installs.
struct ReleaseBundle: Hashable {
    let tag: String
    let folder: URL
    let efiKext: URL          // RembrandtGPU.kext -> EFI/OC/Kexts
    let framebufferKext: URL  // RBAMDFramebuffer.kext -> /Library/Extensions
}

/// Downloads the latest GitHub release of Radeonized, checks its SHA-256, and unpacks it.
/// Only the GitHub API and the release assets are contacted.
enum ReleaseFetcher {
    static let repo = "FranciumOS-org/Radeonized"

    /// `report` gets the four steps and the download's byte progress as they happen.
    static func fetchLatest(report: @escaping @Sendable (ProgressEvent) -> Void) async throws -> ReleaseBundle {
        let total = 4
        report(.step(1, total, "Asking GitHub for the latest release"))

        let api = URL(string: "https://api.github.com/repos/\(repo)/releases/latest")!
        var request = URLRequest(url: api)
        request.setValue("application/vnd.github+json", forHTTPHeaderField: "Accept")
        request.setValue("Radeonize", forHTTPHeaderField: "User-Agent")
        let (data, response) = try await URLSession.shared.data(for: request)
        let code = (response as? HTTPURLResponse)?.statusCode ?? 0
        guard code == 200 else {
            throw RadeonizeError.message("No published release yet (GitHub answered \(code)). The project needs a release with a .zip and a .sha256 file first.")
        }

        struct Release: Decodable {
            struct Asset: Decodable {
                let name: String
                let browser_download_url: URL
            }
            let tag_name: String
            let assets: [Asset]
        }
        let release = try JSONDecoder().decode(Release.self, from: data)
        // The checksum has to be THIS zip's. Taking the first .sha256 in the
        // list would pick another asset's sidecar - a release that also ships
        // the app's own .dmg.sha256 would then fail with a checksum mismatch
        // that looks like a corrupted download. So: prefer "<zip name>.sha256"
        // and fall back to a lone .sha256 only when there is exactly one.
        let shaCandidates = release.assets.filter { $0.name.hasSuffix(".sha256") }
        guard let zipAsset = release.assets.first(where: { $0.name.hasSuffix(".zip") }),
              let shaAsset = shaCandidates.first(where: { $0.name == zipAsset.name + ".sha256" })
                  ?? (shaCandidates.count == 1 ? shaCandidates.first : nil) else {
            throw RadeonizeError.message(
                "Release \(release.tag_name) has no .zip with a matching .sha256. "
                + "Expected \(release.assets.first(where: { $0.name.hasSuffix(".zip") })?.name ?? "<name>.zip")"
                + ".sha256 beside it.")
        }

        let (shaData, _) = try await URLSession.shared.data(from: shaAsset.browser_download_url)
        let expected = String(decoding: shaData, as: UTF8.self)
            .split(whereSeparator: { $0.isWhitespace }).first.map { String($0).lowercased() }
        guard let expected, expected.count == 64 else {
            throw RadeonizeError.message("The .sha256 file is not a SHA-256 checksum.")
        }

        report(.step(2, total, "Downloading \(zipAsset.name)"))
        let downloaded = try await Downloader.file(from: zipAsset.browser_download_url) { fraction, text in
            report(.sub(fraction, text))
        }

        report(.step(3, total, "Checking the SHA-256 against the release"))
        let zipData = try Data(contentsOf: downloaded, options: .mappedIfSafe)
        let actual = SHA256.hash(data: zipData).map { String(format: "%02x", $0) }.joined()
        guard actual == expected else {
            throw RadeonizeError.message("Checksum mismatch: the download does not match the release's .sha256. Nothing was installed.")
        }

        report(.step(4, total, "Unpacking the two kexts"))
        let folder = try cacheFolder(for: release.tag_name)
        let zipPath = folder.appendingPathComponent("release.zip")
        try? FileManager.default.removeItem(at: zipPath)
        try FileManager.default.moveItem(at: downloaded, to: zipPath)

        let unpacked = folder.appendingPathComponent("unpacked", isDirectory: true)
        try? FileManager.default.removeItem(at: unpacked)
        let unzip = Shell.run("/usr/bin/ditto", ["-x", "-k", zipPath.path, unpacked.path])
        guard unzip.status == 0 else {
            throw RadeonizeError.message("Could not unpack the release: \(unzip.stderr)")
        }

        guard let efi = findBundle(named: "RembrandtGPU.kext", in: unpacked),
              let fb = findBundle(named: "RBAMDFramebuffer.kext", in: unpacked) else {
            throw RadeonizeError.message("The release does not contain both RembrandtGPU.kext and RBAMDFramebuffer.kext.")
        }
        return ReleaseBundle(tag: release.tag_name, folder: unpacked, efiKext: efi, framebufferKext: fb)
    }

    private static func cacheFolder(for tag: String) throws -> URL {
        let base = AppPaths.support.appendingPathComponent("releases/\(tag)", isDirectory: true)
        try FileManager.default.createDirectory(at: base, withIntermediateDirectories: true)
        return base
    }

    private static func findBundle(named name: String, in root: URL) -> URL? {
        guard let walker = FileManager.default.enumerator(at: root, includingPropertiesForKeys: nil) else { return nil }
        for case let url as URL in walker where url.lastPathComponent == name {
            return url
        }
        return nil
    }
}

/// Downloads one file and reports how far along it is, as bytes arrive.
private final class Downloader: NSObject, URLSessionDownloadDelegate {
    private let onProgress: @Sendable (Double?, String) -> Void
    private var continuation: CheckedContinuation<URL, Error>?
    private var session: URLSession?

    private init(onProgress: @escaping @Sendable (Double?, String) -> Void) {
        self.onProgress = onProgress
    }

    static func file(from url: URL, onProgress: @escaping @Sendable (Double?, String) -> Void) async throws -> URL {
        let downloader = Downloader(onProgress: onProgress)
        return try await withCheckedThrowingContinuation { continuation in
            downloader.continuation = continuation
            let session = URLSession(configuration: .default, delegate: downloader, delegateQueue: nil)
            downloader.session = session
            session.downloadTask(with: url).resume()
        }
    }

    func urlSession(_ session: URLSession, downloadTask: URLSessionDownloadTask,
                    didWriteData bytesWritten: Int64, totalBytesWritten: Int64,
                    totalBytesExpectedToWrite: Int64) {
        let done = ByteCountFormatter.string(fromByteCount: totalBytesWritten, countStyle: .file)
        if totalBytesExpectedToWrite > 0 {
            let all = ByteCountFormatter.string(fromByteCount: totalBytesExpectedToWrite, countStyle: .file)
            let fraction = min(1, Double(totalBytesWritten) / Double(totalBytesExpectedToWrite))
            onProgress(fraction, "\(done) of \(all)")
        } else {
            onProgress(nil, "\(done) so far")
        }
    }

    func urlSession(_ session: URLSession, downloadTask: URLSessionDownloadTask,
                    didFinishDownloadingTo location: URL) {
        let code = (downloadTask.response as? HTTPURLResponse)?.statusCode ?? 0
        guard code == 200 else {
            finish(.failure(RadeonizeError.message("The download answered \(code).")))
            return
        }
        // The system deletes `location` when this method returns, so move it first.
        let kept = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString + ".zip")
        do {
            try FileManager.default.moveItem(at: location, to: kept)
            finish(.success(kept))
        } catch {
            finish(.failure(error))
        }
    }

    func urlSession(_ session: URLSession, task: URLSessionTask, didCompleteWithError error: Error?) {
        if let error { finish(.failure(error)) }
    }

    private func finish(_ result: Result<URL, Error>) {
        guard let continuation else { return }
        self.continuation = nil
        session?.finishTasksAndInvalidate()
        session = nil
        switch result {
        case .success(let url): continuation.resume(returning: url)
        case .failure(let error): continuation.resume(throwing: error)
        }
    }
}

enum AppPaths {
    /// ~/Library/Application Support/Radeonize
    static var support: URL {
        let base = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
        let dir = base.appendingPathComponent("Radeonize", isDirectory: true)
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        return dir
    }
}
