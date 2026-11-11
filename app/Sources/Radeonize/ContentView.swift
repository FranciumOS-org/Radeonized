import SwiftUI

/// An action waiting for the user to confirm it.
enum PendingAction: Identifiable {
    case installEFI
    case installFramebuffer
    case restore(BackupItem)
    case sendReport

    var id: String {
        switch self {
        case .installEFI: return "efi"
        case .installFramebuffer: return "fb"
        case .restore(let item): return "restore-\(item.id)"
        case .sendReport: return "send"
        }
    }

    var title: String {
        switch self {
        case .installEFI: return "Install the EFI plugin?"
        case .installFramebuffer: return "Install the framebuffer?"
        case .restore: return "Restore this backup?"
        case .sendReport: return "Send this report?"
        }
    }

    var message: String {
        switch self {
        case .installEFI:
            return "Radeonize saves your current config.plist and kext first, then adds RembrandtGPU.kext after Lilu. Reboot to test."
        case .installFramebuffer:
            return "This rebuilds the system's auxiliary kernel collection. Radeonize saves the current one first and checks the result. Reboot to test. Only continue if you have a way to boot back, such as an OpenCore boot menu entry."
        case .restore:
            return "The saved state goes back as it was. Reboot to take effect."
        case .sendReport:
            return "The text shown in the report box is sent to the developer. Nothing else is sent. Check it first."
        }
    }

    var button: String {
        switch self {
        case .installEFI, .installFramebuffer: return "Install"
        case .restore: return "Restore"
        case .sendReport: return "Send"
        }
    }

    @MainActor
    func run(_ model: AppModel) async {
        switch self {
        case .installEFI: await model.installEFI()
        case .installFramebuffer: await model.installFramebuffer()
        case .restore(let item): await model.restore(item)
        case .sendReport: await model.sendReport()
        }
    }
}

struct ContentView: View {
    @EnvironmentObject var model: AppModel
    @State private var pending: PendingAction?

    var body: some View {
        VStack(spacing: 0) {
            // Outside the form, so the log can still be copied while a step runs.
            ProgressCard(progress: model.progress)

            Form {
                compatibilitySection
                releaseSection
                installSection
                backupSection
                reportSection

                if !model.status.isEmpty {
                    Section("Status") {
                        Text(model.status)
                            .font(.system(.caption, design: .monospaced))
                            .textSelection(.enabled)
                            .frame(maxWidth: .infinity, alignment: .leading)
                    }
                }
            }
            .formStyle(.grouped)
            .disabled(model.busy)
        }
        .alert(item: $pending) { action in
            Alert(
                title: Text(action.title),
                message: Text(action.message),
                primaryButton: .destructive(Text(action.button)) {
                    Task { await action.run(model) }
                },
                secondaryButton: .cancel()
            )
        }
    }

    // MARK: Compatibility

    private var compatibilitySection: some View {
        Section("This Mac") {
            LabeledContent("Model", value: model.compatibility.model)
            LabeledContent("macOS", value: model.compatibility.osVersion)
            LabeledContent("Rembrandt GPU (1002:1681 / 164d)") {
                Text(model.compatibility.hasRembrandt ? "Found" : "Not found")
                    .foregroundStyle(model.compatibility.hasRembrandt ? .green : .red)
            }
            ForEach(model.compatibility.blockers, id: \.self) { text in
                Label(text, systemImage: "xmark.octagon.fill").foregroundStyle(.red)
            }
            ForEach(model.compatibility.warnings, id: \.self) { text in
                Label(text, systemImage: "exclamationmark.triangle.fill").foregroundStyle(.orange)
            }
        }
    }

    // MARK: Release

    private var releaseSection: some View {
        Section("Radeonized release") {
            if let release = model.release {
                LabeledContent("Verified version", value: release.tag)
            } else {
                Text("Not downloaded yet.").foregroundStyle(.secondary)
            }
            Button("Download and verify latest release") {
                Task { await model.fetchRelease() }
            }
        }
    }

    // MARK: Install

    private var installSection: some View {
        Section("Install") {
            if model.volumes.isEmpty {
                Text("No EFI partitions found.").foregroundStyle(.secondary)
            } else {
                Picker("EFI partition", selection: $model.selectedVolume) {
                    ForEach(model.volumes) { volume in
                        Text(volume.summary).tag(Optional(volume.id))
                    }
                }
                if let selected = model.selected, !selected.hasOpenCore {
                    Text("Could not confirm OpenCore on this partition yet. Radeonize will mount it and check before writing.")
                        .font(.caption).foregroundStyle(.secondary)
                }
            }

            Button("Install EFI plugin") { pending = .installEFI }
                .disabled(!canInstall || model.selected == nil)

            Button("Install framebuffer") { pending = .installFramebuffer }
                .disabled(!canInstall)

            Button("Refresh") { Task { await model.refresh() } }
        }
    }

    private var canInstall: Bool {
        model.release != nil && model.compatibility.blockers.isEmpty
    }

    // MARK: Backups

    private var backupSection: some View {
        Section("Backups") {
            if model.backups.isEmpty {
                Text("No backups yet. Each install saves the previous state first.")
                    .foregroundStyle(.secondary)
            }
            ForEach(model.backups) { item in
                HStack {
                    Text(item.title)
                    Spacer()
                    Button("Restore") { pending = .restore(item) }
                }
            }
        }
    }

    // MARK: Report

    private var reportSection: some View {
        Section("Diagnostics report") {
            Text("Builds a report on this Mac. Nothing is sent until you press Send, and you can edit the text first.")
                .font(.caption).foregroundStyle(.secondary)
            Button("Build report") { Task { await model.buildReport() } }

            if !model.reportText.isEmpty {
                TextEditor(text: $model.reportText)
                    .font(.system(.caption, design: .monospaced))
                    .frame(minHeight: 180)
                Button("Send to developer") { pending = .sendReport }
                    .disabled(model.reportSent != nil)
            }
        }
    }
}
