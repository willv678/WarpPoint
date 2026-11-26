import SwiftUI

/// WarpDrop's Send window: each dropped file with what it is and where it goes, all of
/// which can be changed, then Send or Cancel. Opened by a drop on the UFO, or by
/// "Send Files to Switch…" in the menu.
struct SendView: View {
  @ObservedObject var model: WarpModel
  @ObservedObject var drop: DropModel
  @Environment(\.dismiss) private var dismiss

  var body: some View {
    VStack(alignment: .leading, spacing: 0) {
      header
        .padding(16)
      Divider()
      if drop.loading {
        ProgressView("Looking at the files…")
          .frame(maxWidth: .infinity, minHeight: 160)
      } else if drop.sheet.items.isEmpty {
        Text("Drop files on the WarpPoint icon in the menu bar to send them to your Switch.")
          .foregroundStyle(.secondary)
          .frame(maxWidth: .infinity, minHeight: 160)
      } else if drop.sheet.items.count <= 4 {
        rows
      } else {
        ScrollView { rows }
          .frame(height: 520)
      }
      Divider()
      footer.padding(16)
    }
    .frame(width: 620)
    .onAppear { model.windowOpened() }
    .onDisappear { model.windowClosed() }
    .onChange(of: drop.closeSheet) { _, close in
      if close { dismiss() }
    }
  }

  private var rows: some View {
    VStack(spacing: 0) {
      ForEach(drop.sheet.items) { item in
        SendRow(model: model, drop: drop, item: item, kinds: drop.sheet.kinds)
        if item.id != drop.sheet.items.last?.id { Divider().padding(.leading, 56) }
      }
    }
  }

  private var consoleName: String {
    if !drop.sheet.console.isEmpty { return drop.sheet.console }
    return model.console ?? "your Switch"
  }

  private var header: some View {
    HStack(alignment: .center, spacing: 12) {
      Image(nsImage: UFOIcon.image(lift: 0, beam: 1, size: 34))
        .renderingMode(.template)
        .foregroundStyle(.primary)
      VStack(alignment: .leading, spacing: 3) {
        Text("Send to \(consoleName)").font(.title3.weight(.semibold))
        statusLine
      }
      Spacer()
    }
  }

  @ViewBuilder private var statusLine: some View {
    let s = drop.sheet
    if !s.enabled {
      Label("Drop is turned off in Plugins.", systemImage: "pause.circle")
        .font(.callout).foregroundStyle(.orange)
    } else if s.canReceive == false {
      Label("Update WarpPoint on the Switch (1.12 or later) to send files. They'll wait until then.",
            systemImage: "exclamationmark.triangle.fill")
        .font(.callout).foregroundStyle(.orange)
    } else if s.status == "needsPairing" {
      Label("Pair this Mac with the Switch to send files. They'll wait until then.", systemImage: "lock.fill")
        .font(.callout).foregroundStyle(.orange)
    } else if s.present || model.console != nil {
      Text("\(consoleName) is here. Files are checked on the Switch before they're put in place.")
        .font(.callout).foregroundStyle(.secondary)
    } else {
      Text("\(consoleName) is away. Files wait in the menu and go when it's back.")
        .font(.callout).foregroundStyle(.secondary)
    }
  }

  private var footer: some View {
    HStack(alignment: .top) {
      VStack(alignment: .leading, spacing: 4) {
        ForEach(drop.sendResult, id: \.self) { e in
          Label(e, systemImage: "xmark.octagon.fill")
            .font(.caption).foregroundStyle(.red)
            .fixedSize(horizontal: false, vertical: true)
        }
        if drop.sendResult.isEmpty && !drop.sheet.items.isEmpty {
          Text(summary).font(.caption).foregroundStyle(.secondary)
        }
      }
      Spacer()
      Button("Cancel") { dismiss() }
        .keyboardShortcut(.cancelAction)
      Button("Send") { drop.send() }
        .keyboardShortcut(.defaultAction)
        .disabled(!drop.canSend)
    }
  }

  private var summary: String {
    let items = drop.sheet.items
    let bytes = items.reduce(Int64(0)) { $0 + $1.size }
    let count = items.reduce(0) { $0 + max(1, $1.files) }
    let toMac = items.filter { $0.side != "switch" }.count
    var s = "\(count) \(count == 1 ? "file" : "files"), \(DropModel.bytes(bytes))."
    if toMac > 0 { s += " \(toMac) \(toMac == 1 ? "stays" : "stay") on this Mac." }
    return s
  }
}

/// One dropped item: icon, name and size; its type; where it goes; any note.
private struct SendRow: View {
  @ObservedObject var model: WarpModel
  @ObservedObject var drop: DropModel
  let item: DropModel.Item
  let kinds: [DropModel.Kind]

  var body: some View {
    HStack(alignment: .top, spacing: 12) {
      Image(systemName: item.isDir ? "folder" : DropModel.icon(item.kind))
        .font(.system(size: 20))
        .foregroundStyle(.secondary)
        .frame(width: 28, height: 28)
      VStack(alignment: .leading, spacing: 6) {
        HStack(alignment: .firstTextBaseline) {
          Text(item.name).font(.body.weight(.medium)).lineLimit(1).truncationMode(.middle)
          Text(sizeText).font(.caption).foregroundStyle(.secondary)
          Spacer()
          Picker("Type", selection: kindBinding) {
            ForEach(kinds) { k in Text(k.label).tag(k.id) }
          }
          .labelsHidden()
          .fixedSize()
          .controlSize(.small)
          Button {
            drop.remove(item)
          } label: {
            Image(systemName: "xmark.circle.fill").foregroundStyle(.tertiary)
          }
          .buttonStyle(.plain)
          .help("Don't send this one")
        }
        destination
        if item.runsAtBoot {
          Label(item.note, systemImage: "exclamationmark.triangle.fill")
            .font(.caption).foregroundStyle(.orange)
            .fixedSize(horizontal: false, vertical: true)
        } else if !item.note.isEmpty {
          Text(item.note).font(.caption).foregroundStyle(.secondary)
            .fixedSize(horizontal: false, vertical: true)
        }
        if item.remembered {
          Text("Where you sent files like this last time.").font(.caption2).foregroundStyle(.tertiary)
        }
        if !item.problem.isEmpty {
          Label("Can't send this: \(item.problem).", systemImage: "xmark.octagon.fill")
            .font(.caption).foregroundStyle(.red)
            .fixedSize(horizontal: false, vertical: true)
        }
      }
    }
    .padding(.horizontal, 16)
    .padding(.vertical, 12)
  }

  private var sizeText: String {
    item.isDir ? "\(item.files) \(item.files == 1 ? "file" : "files"), \(DropModel.bytes(item.size))"
               : DropModel.bytes(item.size)
  }

  private var kindBinding: Binding<String> {
    Binding(get: { drop.choices[item.path]?.kind ?? item.kind },
            set: { drop.update(item, kind: $0) })
  }

  private var folderBinding: Binding<String> {
    Binding(get: { drop.choices[item.path]?.folder ?? DropModel.folder(of: item.dest) },
            set: { drop.choices[item.path, default: .init(kind: item.kind, folder: "", game: "")].folder = $0 })
  }

  private var gameBinding: Binding<String> {
    Binding(get: { drop.choices[item.path]?.game ?? "" },
            set: { drop.update(item, game: $0) })
  }

  @ViewBuilder private var destination: some View {
    switch item.side {
    case "switch":
      if item.kind == "cheats" && item.needsGame {
        gamePicker(prompt: "For the game")
      } else if item.dest.isEmpty {
        Label("SD card, as laid out in the folder", systemImage: "sdcard").font(.callout).foregroundStyle(.secondary)
      } else {
        HStack(spacing: 6) {
          Image(systemName: "sdcard").foregroundStyle(.secondary)
          TextField("Folder on the SD card", text: folderBinding)
            .textFieldStyle(.roundedBorder)
            .font(.system(.callout, design: .monospaced))
          Text("/ " + (item.dest as NSString).lastPathComponent)
            .font(.system(.callout, design: .monospaced))
            .foregroundStyle(.secondary)
            .lineLimit(1)
            .truncationMode(.middle)
        }
        if item.kind == "cheats" && !item.game.isEmpty {
          Text("For \(item.game) (build \(item.buildId.uppercased()))").font(.caption).foregroundStyle(.secondary)
        }
      }
    case "mods":
      if item.needsGame || (drop.choices[item.path]?.game ?? "").isEmpty {
        gamePicker(prompt: "Mods library, for")
      } else {
        Label("Mods library: \(item.game.isEmpty ? item.titleId : item.game)", systemImage: "shippingbox")
          .font(.callout).foregroundStyle(.secondary)
      }
    case "tap":
      Label("This Mac: WarpPoint's tap folder", systemImage: "laptopcomputer").font(.callout).foregroundStyle(.secondary)
    default:  // backup
      savePicker
    }
  }

  /// Games in the library, one per title.
  private var games: [(id: String, name: String)] {
    var seen = Set<String>()
    return model.library.games
      .filter { seen.insert($0.titleId).inserted }
      .map { ($0.titleId, $0.name) }
      .sorted { $0.name.localizedCaseInsensitiveCompare($1.name) == .orderedAscending }
  }

  private func gamePicker(prompt: String) -> some View {
    Picker(prompt, selection: gameBinding) {
      Text("Choose a game…").tag("")
      ForEach(games, id: \.id) { g in Text(g.name).tag(g.id) }
    }
    .font(.callout)
    .controlSize(.small)
    .fixedSize()
  }

  /// A save goes in as a backup of one game's save (one per user in the library).
  private var savePicker: some View {
    Picker("Backup of", selection: gameBinding) {
      Text("Choose a game…").tag("")
      ForEach(model.library.games.sorted { $0.name.localizedCaseInsensitiveCompare($1.name) == .orderedAscending }) { g in
        Text(model.library.games.filter { $0.titleId == g.titleId }.count > 1 ? "\(g.name) (\(user(g.key)))" : g.name)
          .tag(g.key)
      }
    }
    .font(.callout)
    .controlSize(.small)
    .fixedSize()
  }

  private func user(_ key: String) -> String {
    let id = key.split(separator: "/").last.map(String.init) ?? ""
    return model.setup.consoleUsers.first { $0.id == id }?.name ?? String(id.prefix(6))
  }
}
