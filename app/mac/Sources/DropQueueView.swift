import SwiftUI

/// WarpDrop's transfer queue in the menu: files on their way to the Switch, with
/// progress and Cancel; a file that's already there asks Replace, Keep Both or Skip.
/// Shows nothing while the queue is empty.
struct DropQueueSection: View {
  @ObservedObject var drop: DropModel
  @State private var sameForRest = true

  private var active: [DropModel.Transfer] { drop.queue.items.filter { !$0.finished } }
  private var finished: [DropModel.Transfer] { Array(drop.queue.items.filter { $0.finished }.suffix(3).reversed()) }

  var body: some View {
    if !drop.queue.items.isEmpty {
      VStack(alignment: .leading, spacing: 6) {
        HStack {
          Text(title.uppercased()).font(.caption2.weight(.semibold)).foregroundStyle(.secondary)
          Spacer()
          if active.count > 1 {
            Button("Cancel All") { drop.cancel(nil) }
              .buttonStyle(.plain).font(.caption2).foregroundStyle(.secondary)
          } else if active.isEmpty {
            Button("Clear") { drop.clearFinished() }
              .buttonStyle(.plain).font(.caption2).foregroundStyle(.secondary)
          }
        }
        if let note = statusNote {
          Label(note.text, systemImage: note.icon)
            .font(.caption)
            .foregroundStyle(note.warn ? Color.orange : Color.secondary)
            .fixedSize(horizontal: false, vertical: true)
        }
        ForEach(active + finished) { t in row(t) }
      }
    }
  }

  private var title: String {
    let who = drop.queue.console.isEmpty ? "Switch" : drop.queue.console
    return active.isEmpty ? "Sent to \(who)" : "Sending to \(who)"
  }

  private var statusNote: (text: String, icon: String, warn: Bool)? {
    guard !active.isEmpty, !drop.queue.statusText.isEmpty else { return nil }
    switch drop.queue.status {
    case "needsUpdate", "needsPairing", "disabled":
      return (drop.queue.statusText, "exclamationmark.triangle.fill", true)
    case "waiting":
      return (drop.queue.statusText, "clock", false)
    default:
      return nil
    }
  }

  @ViewBuilder private func row(_ t: DropModel.Transfer) -> some View {
    HStack(alignment: .top, spacing: 8) {
      Image(systemName: DropModel.icon(t.kind))
        .font(.system(size: 15))
        .foregroundStyle(.secondary)
        .frame(width: 30, height: 30)
        .background(RoundedRectangle(cornerRadius: 6).fill(.quaternary))
      VStack(alignment: .leading, spacing: 3) {
        Text(t.name).font(.callout.weight(.medium)).lineLimit(1).truncationMode(.middle)
        detail(t)
      }
      Spacer(minLength: 4)
      if !t.finished && t.state != "needsChoice" {
        Button { drop.cancel(t) } label: { Image(systemName: "xmark.circle.fill").foregroundStyle(.tertiary) }
          .buttonStyle(.plain)
          .help("Cancel")
      } else if t.state == "failed" {
        Button("Retry") { drop.retry(t) }.controlSize(.small)
      }
    }
  }

  @ViewBuilder private func detail(_ t: DropModel.Transfer) -> some View {
    switch t.state {
    case "sending":
      ProgressView(value: t.progress).controlSize(.small)
      Text("\(DropModel.bytes(t.sent)) of \(DropModel.bytes(t.size)) to \(t.folder)/")
        .font(.caption).foregroundStyle(.secondary).lineLimit(1).truncationMode(.middle)
    case "queued":
      if t.sent > 0 {
        ProgressView(value: t.progress).controlSize(.small).tint(.secondary)
        Text("\(DropModel.bytes(t.sent)) of \(DropModel.bytes(t.size)), carries on from there")
          .font(.caption).foregroundStyle(.secondary).lineLimit(1)
      } else {
        Text("Waiting, \(DropModel.bytes(t.size)) to \(t.folder)/")
          .font(.caption).foregroundStyle(.secondary).lineLimit(1).truncationMode(.middle)
      }
    case "needsChoice":
      Text(t.error.isEmpty ? "Already on the Switch." : t.error)
        .font(.caption).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
      HStack(spacing: 6) {
        Button("Replace") { drop.choose(t, "replace", wholeBatch: sameForRest) }
        Button("Keep Both") { drop.choose(t, "keepBoth", wholeBatch: sameForRest) }
        Button("Skip") { drop.choose(t, "skip", wholeBatch: sameForRest) }
      }
      .controlSize(.small)
      if drop.queue.items.filter({ $0.batch == t.batch && !$0.finished }).count > 1 {
        Toggle("Same for the rest of these files", isOn: $sameForRest)
          .toggleStyle(.checkbox).font(.caption).controlSize(.small)
      }
    case "done":
      Text("Sent to \(t.folder)/").font(.caption).foregroundStyle(.secondary).lineLimit(1).truncationMode(.middle)
    case "failed":
      Text("Couldn't send: \(t.error)").font(.caption).foregroundStyle(.orange)
        .fixedSize(horizontal: false, vertical: true)
    case "skipped":
      Text("Skipped (already on the Switch)").font(.caption).foregroundStyle(.secondary)
    default:
      Text("Cancelled").font(.caption).foregroundStyle(.secondary)
    }
  }
}
