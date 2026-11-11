import AppKit
import Foundation
import SwiftUI

/// Everything the window shows. Blocking work runs detached; results land on the main actor.
@MainActor
final class AppModel: ObservableObject {
    @Published var compatibility = Compatibility()
    @Published var volumes: [EFIVolume] = []
    @Published var selectedVolume: String?
    @Published var release: ReleaseBundle?
    @Published var backups: [BackupItem] = []
    @Published var status = ""
    @Published var busy = false

    @Published var reportText = ""
    @Published var reportSent: String?

    /// Live view of the operation that is running (or the last one that ran).
    let progress = OperationProgress()

    var selected: EFIVolume? { volumes.first { $0.id == selectedVolume } }
    var owner: String { NSUserName() }

    // MARK: Scan

    func refresh() async {
        busy = true
        defer { busy = false }
        async let compat = Task.detached { CompatibilityCheck.run() }.value
        async let efi = Task.detached { EFIScanner.scan() }.value
        compatibility = await compat
        volumes = await efi
        if selected == nil { selectedVolume = volumes.first(where: { $0.hasOpenCore })?.id ?? volumes.first?.id }
        backups = BackupStore.list()
    }

    // MARK: Release

    func fetchRelease() async {
        let progress = self.progress
        await perform("Downloading the latest Radeonized", slug: "download") {
            let bundle = try await ReleaseFetcher.fetchLatest { event in
                Task { @MainActor in progress.apply(event) }
            }
            self.release = bundle
            return "Verified \(bundle.tag): SHA-256 matched. Unpacked to \(bundle.folder.path)"
        }
    }

    // MARK: Install

    func installEFI() async {
        guard let release, let volume = selected else { return }
        let backup = BackupStore.newFolder(.efi)
        await perform("Radeonizing the EFI on \(volume.id)", slug: "efi-install") {
            let out = try await Privileged.run([
                "efi-install", volume.id, release.efiKext.deletingLastPathComponent().path,
                backup.path, self.owner,
            ], progress: self.progress)
            return out + "\nReboot, then check the plugin with Diagnostics."
        }
    }

    func installFramebuffer() async {
        guard let release else { return }
        let backup = BackupStore.newFolder(.framebuffer)
        await perform("Radeonizing the framebuffer", slug: "fb-install") {
            // The UUID is read here, from the binary we are about to install, and
            // the helper checks it inside the collection after the write.
            let binary = release.framebufferKext.appendingPathComponent("Contents/MacOS/RBAMDFramebuffer")
            let uuid = try MachOUUID.read(from: binary)
            return try await Privileged.run([
                "fb-install", release.framebufferKext.path, backup.path, self.owner, uuid,
            ], progress: self.progress)
        }
    }

    // MARK: Restore

    func restore(_ item: BackupItem) async {
        switch item.kind {
        case .efi:
            guard let device = selected?.id ?? item.efiDevice else {
                status = "No EFI partition selected."
                return
            }
            await perform("Restoring the EFI from \(item.date)", slug: "efi-restore") {
                try await Privileged.run(["efi-restore", device, item.url.path, self.owner], progress: self.progress)
            }
        case .framebuffer:
            await perform("Restoring the framebuffer from \(item.date)", slug: "fb-restore") {
                try await Privileged.run(["fb-restore", item.url.path, self.owner], progress: self.progress)
            }
        }
    }

    // MARK: Report

    func buildReport() async {
        busy = true
        defer { busy = false }
        reportText = await DiagnosticsReport.build()
        reportSent = nil
    }

    func sendReport() async {
        let text = reportText
        busy = true
        defer { busy = false }
        do {
            let id = try await DiagnosticsReport.send(text)
            reportSent = id
            status = "Report sent. Reference: \(id)"
        } catch {
            status = error.localizedDescription
        }
    }

    // MARK: Helpers

    private func perform(_ title: String, slug: String, _ work: () async throws -> String) async {
        busy = true
        defer { busy = false }
        progress.begin(title: title, slug: slug)
        status = title + "…"
        do {
            let message = try await work()
            status = message
            progress.finish(success: true, message: message)
        } catch {
            let text = error.localizedDescription
            status = "Failed: \(text)"
            progress.finish(success: false, message: text)
        }
        await refresh()
    }
}
