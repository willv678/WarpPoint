import SwiftUI

/// The inspector beside the grid: one game's status, actions and backups.
struct GameDetail: View {
  let game: Game
  @ObservedObject var model: WarpModel
  @State private var backups: [Backup] = []
  @State private var confirmRestore: Backup?

  private var console: String { model.library.console.isEmpty ? "Switch" : model.library.console }
  private var emulator: String { model.library.emulator }

  var body: some View {
    ScrollView {
      VStack(alignment: .leading, spacing: 18) {
        header
        status
        actions
        if let message = model.libraryMessage, message.contains(game.name) || message.hasPrefix("Restore") {
          Text(message).font(.callout).foregroundStyle(.secondary)
            .fixedSize(horizontal: false, vertical: true)
        }
        Divider()
        backupList
      }
      .padding(18)
    }
    .onAppear(perform: loadBackups)
    .onChange(of: game) { loadBackups() }
    .onChange(of: model.lastSync) { loadBackups() }
    .alert(item: $confirmRestore) { b in
      Alert(title: Text("Restore this backup to \(emulator)?"),
            message: Text("Your current \(game.name) save in \(emulator) is backed up first, so you can undo this. The next sync carries it to \(console)."),
            primaryButton: .default(Text("Restore")) { model.restore(game, backup: b) },
            secondaryButton: .cancel())
    }
  }

  private func loadBackups() { backups = model.backups(for: game.key) }

  private var header: some View {
    VStack(alignment: .leading, spacing: 10) {
      GameArt(path: game.icon, size: 128)
        .shadow(color: .black.opacity(0.2), radius: 6, y: 3)
      Text(game.name).font(.title2.weight(.semibold)).fixedSize(horizontal: false, vertical: true)
      if !game.publisher.isEmpty {
        Text(game.publisher).foregroundStyle(.secondary)
      }
    }
  }

  private var status: some View {
    VStack(alignment: .leading, spacing: 8) {
      HStack(spacing: 6) {
        StatusBadge(status: game.status)
        Text(StatusBadge.text(game.status, model: model)).font(.headline)
      }
      if !game.lastText.isEmpty, let at = WarpDates.parse(game.lastAt) {
        Text(lastSync(at)).font(.callout).foregroundStyle(.secondary)
      }
      VStack(alignment: .leading, spacing: 4) {
        place(console, has: game.onConsole)
        place(emulator, has: game.inEmulator)
      }
      .padding(.top, 2)
    }
  }

  /// "Checked 2 minutes ago", or "Copied Astris → Will's Switch 2 minutes ago".
  private func lastSync(_ at: Date) -> String {
    let when = WarpModel.ago(at)
    if game.lastText == "in sync" { return "Checked \(when)" }
    let text = game.lastText.replacingOccurrences(of: "->", with: "→")
    return text.prefix(1).uppercased() + text.dropFirst() + " " + when
  }

  private func place(_ name: String, has: Bool) -> some View {
    Label(has ? "Save on \(name)" : "No save on \(name)",
          systemImage: has ? "checkmark" : "minus")
      .font(.callout)
      .foregroundStyle(has ? .primary : .secondary)
  }

  @ViewBuilder private var actions: some View {
    if game.status == "consoleOnly" {
      VStack(alignment: .leading, spacing: 6) {
        Button {
          model.bringToEmulator(game)
        } label: {
          Label("Bring to \(emulator)", systemImage: "arrow.down.circle")
        }
        .buttonStyle(.borderedProminent)
        .disabled(!model.library.consolePresent || model.library.emulatorOpen)
        if !model.library.consolePresent {
          Text("Open WarpPoint on your Switch first.").font(.caption).foregroundStyle(.secondary)
        } else if model.library.emulatorOpen {
          Text("Quit \(emulator) first: it keeps its list of saves in memory while it's open.")
            .font(.caption).foregroundStyle(.secondary)
        }
      }
    } else if game.status == "emulatorOnly" {
      Text("Not on \(console) yet. Start the game there once, then pick which save to keep on the next sync.")
        .font(.callout).foregroundStyle(.secondary)
        .fixedSize(horizontal: false, vertical: true)
    } else if game.status == "conflict", let c = model.conflicts.first(where: { $0.key == game.key }) {
      VStack(alignment: .leading, spacing: 6) {
        HStack {
          Button("Keep \(emulator)") { model.resolve(c, keepLocal: true) }
          Button("Keep \(console)") { model.resolve(c, keepLocal: false) }
        }
        Text("The other copy is backed up first.").font(.caption).foregroundStyle(.secondary)
      }
    }
  }

  private var backupList: some View {
    VStack(alignment: .leading, spacing: 10) {
      Text("Backups").font(.headline)
      if backups.isEmpty {
        Text("None yet. WarpPoint saves a copy here before it ever replaces this save.")
          .font(.callout).foregroundStyle(.secondary)
          .fixedSize(horizontal: false, vertical: true)
      } else {
        let keep = model.library.keepBackups
        Text(keep > 0 ? "WarpPoint keeps the newest \(keep) of each save and removes older ones."
                      : "WarpPoint keeps every backup.")
          .font(.caption).foregroundStyle(.tertiary)
          .fixedSize(horizontal: false, vertical: true)
      }
      ForEach(backups) { b in
        HStack(alignment: .top) {
          VStack(alignment: .leading, spacing: 2) {
            Text(WarpDates.parse(b.created).map { $0.formatted(date: .abbreviated, time: .shortened) } ?? b.created)
              .font(.callout.weight(.medium))
            Text(describe(b)).font(.caption).foregroundStyle(.secondary)
              .fixedSize(horizontal: false, vertical: true)
          }
          Spacer()
          Button("Restore…") { confirmRestore = b }
            .controlSize(.small)
            .disabled(model.library.emulatorOpen && !game.inEmulator)
        }
      }
    }
  }

  private func describe(_ b: Backup) -> String {
    let from = b.side == "switch" ? console : emulator
    let why: String
    switch b.reason {
    case "before-pull": why = "kept before \(console)'s copy replaced it"
    case "before-push": why = "kept before \(emulator)'s copy replaced it"
    case "before-restore": why = "kept before a restore"
    case "manual-pull": why = "copied by hand"
    default: why = b.reason
    }
    let size = ByteCountFormatter.string(fromByteCount: Int64(b.bytes), countStyle: .file)
    return "\(from)'s copy, \(why) · \(size)"
  }
}
