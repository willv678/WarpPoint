import AppKit
import SwiftUI

/// What clicking the UFO shows.
struct MenuView: View {
  @ObservedObject var model: WarpModel
  @Environment(\.openWindow) private var openWindow

  var body: some View {
    VStack(alignment: .leading, spacing: 12) {
      header
      if let problem = model.problem {
        Label(problem, systemImage: "exclamationmark.triangle.fill")
          .font(.callout)
          .foregroundStyle(.orange)
          .fixedSize(horizontal: false, vertical: true)
      }
      if !model.conflicts.isEmpty { conflicts }
      if !model.recent.isEmpty { recent }
      Divider()
      actions
    }
    .padding(14)
    .frame(width: 330)
  }

  private var header: some View {
    HStack(alignment: .center, spacing: 10) {
      Image(nsImage: UFOIcon.image(beam: model.beam, size: 30))
        .renderingMode(.template)
        .foregroundStyle(.primary)
      VStack(alignment: .leading, spacing: 2) {
        Text("WarpPoint").font(.headline)
        // Re-read every 15 s so "5 minutes ago" keeps counting while the panel is open.
        TimelineView(.periodic(from: .now, by: 15)) { _ in
          Text(model.statusLine)
            .font(.callout)
            .foregroundStyle(.secondary)
            .fixedSize(horizontal: false, vertical: true)
        }
      }
    }
  }

  private var conflicts: some View {
    VStack(alignment: .leading, spacing: 8) {
      SectionTitle("Needs you")
      ForEach(model.conflicts) { c in
        VStack(alignment: .leading, spacing: 6) {
          HStack(spacing: 8) {
            GameIcon(path: c.icon)
            VStack(alignment: .leading, spacing: 1) {
              Text(c.name).font(.callout.weight(.medium)).lineLimit(1)
              Text("Changed on both sides. Which one do you want to keep?")
                .font(.caption).foregroundStyle(.secondary)
            }
          }
          HStack {
            Button("Keep \(model.localName)") { model.resolve(c, keepLocal: true) }
            Button("Keep \(model.console ?? "Switch")") { model.resolve(c, keepLocal: false) }
          }
          .controlSize(.small)
          Text("The other copy is backed up first.").font(.caption2).foregroundStyle(.tertiary)
        }
      }
    }
  }

  private var recent: some View {
    VStack(alignment: .leading, spacing: 6) {
      SectionTitle("Recent")
      ForEach(model.recent) { r in
        HStack(spacing: 8) {
          GameIcon(path: r.icon)
          VStack(alignment: .leading, spacing: 1) {
            Text(r.name).font(.callout.weight(.medium)).lineLimit(1)
            Text(r.text).font(.caption).foregroundStyle(r.ok ? Color.secondary : Color.orange)
              .lineLimit(2)
          }
          Spacer()
          Text(r.date, style: .time).font(.caption2).foregroundStyle(.tertiary)
        }
      }
    }
  }

  private var actions: some View {
    VStack(alignment: .leading, spacing: 4) {
      MenuButton("Sync Now", disabled: model.console == nil || model.syncing) { model.syncNow() }
      MenuButton("Open Library…") {
        openWindow(id: "library")
        NSApp.activate()  // a menu bar app's window should come to the front
      }
      MenuButton("Show Backups and Data in Finder") { model.showDataFolder() }
      Toggle("Open at Login", isOn: $model.openAtLogin)
        .toggleStyle(.checkbox)
        .padding(.horizontal, 6)
        .padding(.vertical, 3)
      Divider().padding(.vertical, 2)
      MenuButton("Quit WarpPoint") { NSApp.terminate(nil) }
    }
  }
}

private struct SectionTitle: View {
  let text: String
  init(_ text: String) { self.text = text }
  var body: some View {
    Text(text.uppercased()).font(.caption2.weight(.semibold)).foregroundStyle(.secondary)
  }
}

/// A full-width row that highlights on hover, like a native menu item.
private struct MenuButton: View {
  let title: String
  var disabled = false
  let action: () -> Void
  @State private var hover = false

  init(_ title: String, disabled: Bool = false, action: @escaping () -> Void) {
    self.title = title
    self.disabled = disabled
    self.action = action
  }

  var body: some View {
    Button(action: action) {
      Text(title)
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(.horizontal, 6)
        .padding(.vertical, 3)
        .background(RoundedRectangle(cornerRadius: 5).fill(hover && !disabled ? Color.accentColor : .clear))
        .foregroundStyle(disabled ? Color.secondary : hover ? Color.white : Color.primary)
        .contentShape(Rectangle())
    }
    .buttonStyle(.plain)
    .disabled(disabled)
    .onHover { hover = $0 }
  }
}

/// A game's HOME-menu icon from the Switch, or a placeholder until it's known.
private struct GameIcon: View {
  let path: String
  var body: some View {
    Group {
      if !path.isEmpty, let image = NSImage(contentsOfFile: path) {
        Image(nsImage: image).resizable()
      } else {
        RoundedRectangle(cornerRadius: 6).fill(.quaternary)
      }
    }
    .frame(width: 30, height: 30)
    .clipShape(RoundedRectangle(cornerRadius: 6))
  }
}
