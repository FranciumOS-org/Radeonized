import AppKit
import SwiftUI

/// The live view of what Radeonize is doing: an overall bar, numbered steps, the
/// current step's own progress, elapsed time, and the raw log. It stays after a
/// run so the outcome and the log can still be read or copied.
struct ProgressCard: View {
    @ObservedObject var progress: OperationProgress

    private let brand = Color(red: 0.86, green: 0.16, blue: 0.16)

    var body: some View {
        if progress.isRunning || !progress.log.isEmpty {
            VStack(alignment: .leading, spacing: 12) {
                header
                overall
                if !progress.steps.isEmpty { stepList }
                if progress.isRunning, let fraction = progress.subFraction {
                    VStack(alignment: .leading, spacing: 4) {
                        ProgressView(value: fraction, total: 1).tint(brand)
                        Text(progress.subText)
                            .font(.caption).foregroundStyle(.secondary).monospacedDigit()
                    }
                }
                timing
                if let failure = progress.failure {
                    Text(failure)
                        .font(.callout).foregroundStyle(.red)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .textSelection(.enabled)
                }
                logBox
            }
            .padding(14)
            .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 12, style: .continuous))
            .overlay(
                RoundedRectangle(cornerRadius: 12, style: .continuous)
                    .strokeBorder(
                        LinearGradient(colors: [brand.opacity(0.6), brand.opacity(0.0)],
                                       startPoint: .topLeading, endPoint: .bottomTrailing),
                        lineWidth: 1)
            )
            .padding(.horizontal, 16)
            .padding(.top, 12)
            .animation(.easeInOut(duration: 0.3), value: progress.fraction)
            .animation(.easeInOut(duration: 0.25), value: progress.steps)
        }
    }

    // MARK: Pieces

    private var header: some View {
        HStack(spacing: 8) {
            statusIcon
            Text(progress.title)
                .font(.headline)
                .lineLimit(2)
            Spacer()
            Text(statusWord)
                .font(.caption.weight(.semibold))
                .foregroundStyle(statusColor)
        }
    }

    private var overall: some View {
        Group {
            if !progress.steps.isEmpty {
                ProgressView(value: progress.fraction, total: 1).tint(brand)
            } else if progress.isRunning {
                ProgressView().progressViewStyle(.linear).tint(brand)
            }
        }
    }

    private var stepList: some View {
        VStack(alignment: .leading, spacing: 7) {
            ForEach(progress.steps) { step in
                HStack(alignment: .firstTextBaseline, spacing: 8) {
                    stepIcon(step.state)
                        .frame(width: 16)
                    Text(step.label)
                        .font(.callout)
                        .fontWeight(step.state == .running ? .semibold : .regular)
                        .foregroundStyle(step.state == .pending ? .secondary : .primary)
                        .frame(maxWidth: .infinity, alignment: .leading)
                }
            }
        }
    }

    /// Elapsed time, and how long since the last output. A long quiet spell is
    /// flagged so a slow step is not mistaken for a stuck one.
    @ViewBuilder
    private var timing: some View {
        if progress.isRunning {
            TimelineView(.periodic(from: .now, by: 1)) { context in
                let elapsed = context.date.timeIntervalSince(progress.startedAt)
                let quiet = context.date.timeIntervalSince(progress.lastActivity)
                VStack(alignment: .leading, spacing: 4) {
                    Text("Elapsed \(clock(elapsed)) · last output \(Int(quiet)) s ago")
                        .font(.caption).foregroundStyle(.secondary).monospacedDigit()
                    if quiet > 30 {
                        Label("No output for \(Int(quiet)) s. Building the kernel collection can take several minutes. Wait before you do anything else.",
                              systemImage: "hourglass")
                            .font(.caption)
                            .foregroundStyle(.orange)
                    }
                }
            }
        } else {
            Text("Took \(clock(progress.lastActivity.timeIntervalSince(progress.startedAt)))")
                .font(.caption).foregroundStyle(.secondary).monospacedDigit()
        }
    }

    private var logBox: some View {
        DisclosureGroup("Log (\(progress.log.count) lines)") {
            ScrollViewReader { proxy in
                ScrollView {
                    Text(progress.log.suffix(400).joined(separator: "\n"))
                        .font(.system(.caption2, design: .monospaced))
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .textSelection(.enabled)
                        .id("end")
                }
                .frame(height: 160)
                .onChange(of: progress.log.count) { _ in
                    proxy.scrollTo("end", anchor: .bottom)
                }
            }
            HStack {
                Button("Copy log") {
                    NSPasteboard.general.clearContents()
                    NSPasteboard.general.setString(progress.log.joined(separator: "\n"), forType: .string)
                }
                Button("Show logs in Finder") {
                    NSWorkspace.shared.open(OperationLog.folder)
                }
                Spacer()
            }
            .controlSize(.small)
        }
        .font(.callout)
    }

    // MARK: Icons and words

    @ViewBuilder
    private var statusIcon: some View {
        if progress.isRunning {
            ProgressView().controlSize(.small)
        } else if progress.failure != nil {
            Image(systemName: "xmark.seal.fill").foregroundStyle(.red)
        } else {
            Image(systemName: "checkmark.seal.fill").foregroundStyle(.green)
        }
    }

    @ViewBuilder
    private func stepIcon(_ state: OperationProgress.Step.State) -> some View {
        switch state {
        case .pending:
            Image(systemName: "circle").foregroundStyle(.tertiary)
        case .running:
            ProgressView().controlSize(.small)
        case .done:
            Image(systemName: "checkmark.circle.fill").foregroundStyle(.green)
        case .failed:
            Image(systemName: "xmark.octagon.fill").foregroundStyle(.red)
        }
    }

    private var statusWord: String {
        if progress.isRunning { return "Working" }
        return progress.failure == nil ? "Done" : "Stopped"
    }

    private var statusColor: Color {
        if progress.isRunning { return .secondary }
        return progress.failure == nil ? .green : .red
    }

    private func clock(_ seconds: TimeInterval) -> String {
        let total = Int(max(0, seconds))
        return String(format: "%d:%02d", total / 60, total % 60)
    }
}
