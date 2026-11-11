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
        await perform("Downloading the latest release") {
            let bundle = try await ReleaseFetcher.fetchLatest()
            self.release = bundle
            return "Verified \(bundle.tag): SHA-256 matched. Unpacked to \(bundle.folder.path)"
        }
    }

    // MARK: Install

    func installEFI() async {
        guard let release, let volume = selected else { return }
        let backup = BackupStore.newFolder(.efi)
        await perform("Installing the EFI plugin on \(volume.id)") {
            let out = try await Privileged.run([
                "efi-install", volume.id, release.efiKext.deletingLastPathComponent().path,
                backup.path, self.owner,
            ])
            return out + "\nReboot, then check the plugin with Diagnostics."
        }
    }

    func installFramebuffer() async {
        guard let release else { return }
        let backup = BackupStore.newFolder(.framebuffer)
        await perform("Installing the framebuffer into the system collection") {
            try await Privileged.run([
                "fb-install", release.framebufferKext.path, backup.path, self.owner,
            ])
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
            await perform("Restoring the EFI from \(item.date)") {
                try await Privileged.run(["efi-restore", device, item.url.path, self.owner])
            }
        case .framebuffer:
            await perform("Restoring the framebuffer collection from \(item.date)") {
                try await Privileged.run(["fb-restore", item.url.path, self.owner])
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

    private func perform(_ title: String, _ work: () async throws -> String) async {
        busy = true
        defer { busy = false }
        status = title + "…"
        do {
            status = try await work()
        } catch {
            status = "Failed: \(error.localizedDescription)"
        }
        await refresh()
    }
}
