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

    static func fetchLatest() async throws -> ReleaseBundle {
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
        guard let zipAsset = release.assets.first(where: { $0.name.hasSuffix(".zip") }),
              let shaAsset = release.assets.first(where: { $0.name.hasSuffix(".sha256") }) else {
            throw RadeonizeError.message("Release \(release.tag_name) has no .zip and .sha256 pair.")
        }

        let (shaData, _) = try await URLSession.shared.data(from: shaAsset.browser_download_url)
        let expected = String(decoding: shaData, as: UTF8.self)
            .split(whereSeparator: { $0.isWhitespace }).first.map { String($0).lowercased() }
        guard let expected, expected.count == 64 else {
            throw RadeonizeError.message("The .sha256 file is not a SHA-256 checksum.")
        }

        let (downloaded, _) = try await URLSession.shared.download(from: zipAsset.browser_download_url)
        let zipData = try Data(contentsOf: downloaded, options: .mappedIfSafe)
        let actual = SHA256.hash(data: zipData).map { String(format: "%02x", $0) }.joined()
        guard actual == expected else {
            throw RadeonizeError.message("Checksum mismatch: the download does not match the release's .sha256. Nothing was installed.")
        }

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

enum AppPaths {
    /// ~/Library/Application Support/Radeonize
    static var support: URL {
        let base = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
        let dir = base.appendingPathComponent("Radeonize", isDirectory: true)
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        return dir
    }
}
