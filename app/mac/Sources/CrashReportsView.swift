import AppKit
import SwiftUI

/// Warp Lens: what crashed on the console, newest first, explained, with the report and
/// the screenshot Atmosphère kept.
struct CrashReport: Codable, Identifiable, Hashable {
  let name: String
  let time: UInt64
  let programId: String
  let program: String
  let errorCode: String
  let what: String
  let `where`: String
  let explanation: String
  let image: String
  let file: String
  let icon: String
  var id: String { name }
  var date: Date { Date(timeIntervalSince1970: TimeInterval(time)) }
}

struct CrashReportsView: View {
  @ObservedObject var model: WarpModel
  @State private var selection: String?

  private var selected: CrashReport? { model.reports.first { $0.name == selection } }

  var body: some View {
    HSplitView {
      List(selection: $selection) {
        ForEach(model.reports) { r in
          HStack(spacing: 10) {
            GameArt(path: r.icon, size: 36)
            VStack(alignment: .leading, spacing: 2) {
              Text(r.program.isEmpty ? "Unknown program" : r.program).font(.callout.weight(.medium))
                .lineLimit(1)
              Text(r.date.formatted(date: .abbreviated, time: .shortened))
                .font(.caption).foregroundStyle(.secondary)
            }
          }
          .tag(r.name)
          .padding(.vertical, 2)
        }
      }
      .frame(minWidth: 240, idealWidth: 260)
      Group {
        if let r = selected {
          detail(r)
        } else {
          VStack(spacing: 10) {
            Image(systemName: "checkmark.shield").font(.system(size: 40)).foregroundStyle(.tertiary)
            Text(model.reports.isEmpty ? "No crashes yet" : "Pick a report")
              .font(.title3.weight(.semibold))
            Text("When a game or app crashes on your Switch, WarpPoint brings Atmosphère's report here and says what it means.")
              .multilineTextAlignment(.center).foregroundStyle(.secondary).frame(maxWidth: 340)
          }
          .frame(maxWidth: .infinity, maxHeight: .infinity)
        }
      }
      .frame(minWidth: 380)
    }
    .onAppear {
      model.windowOpened()
      model.refreshReports()
      model.newCrashes = 0
      if selection == nil { selection = model.reports.first?.name }
    }
    .onDisappear { model.windowClosed() }
  }

  private func detail(_ r: CrashReport) -> some View {
    ScrollView {
      VStack(alignment: .leading, spacing: 14) {
        HStack(spacing: 12) {
          GameArt(path: r.icon, size: 64)
          VStack(alignment: .leading, spacing: 3) {
            Text(r.program.isEmpty ? "Unknown program" : r.program).font(.title2.weight(.semibold))
            Text(r.date.formatted(date: .complete, time: .standard)).foregroundStyle(.secondary)
          }
        }
        Text(r.explanation).fixedSize(horizontal: false, vertical: true)
        Grid(alignment: .leading, horizontalSpacing: 12, verticalSpacing: 6) {
          if !r.errorCode.isEmpty { row("Error", r.errorCode) }
          if !r.what.isEmpty { row("Exception", r.what) }
          if !r.where.isEmpty { row("Where", r.where) }
          row("Program ID", r.programId.uppercased())
        }
        .font(.callout)
        if let image = NSImage(contentsOfFile: r.image) {
          Image(nsImage: image).resizable().aspectRatio(contentMode: .fit)
            .clipShape(RoundedRectangle(cornerRadius: 8))
            .frame(maxWidth: 480)
        }
        HStack {
          Button("Open Report") { NSWorkspace.shared.open(URL(fileURLWithPath: r.file)) }
          Button("Show in Finder") {
            NSWorkspace.shared.activateFileViewerSelecting([URL(fileURLWithPath: r.file)])
          }
          Button("Copy Summary") {
            let text = "\(r.program) \(r.errorCode) \(r.what) at \(r.where)\n\(r.explanation)"
            NSPasteboard.general.clearContents()
            NSPasteboard.general.setString(text, forType: .string)
          }
        }
      }
      .padding(20)
      .frame(maxWidth: .infinity, alignment: .leading)
    }
  }

  private func row(_ label: String, _ value: String) -> some View {
    GridRow {
      Text(label).foregroundStyle(.secondary)
      Text(value).textSelection(.enabled).font(.callout.monospaced())
    }
  }
}
