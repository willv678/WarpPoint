import AppKit
import SwiftUI

/// The inspector beside the grid: one game's status, actions and backups.
struct GameDetail: View {
  let game: Game
  @ObservedObject var model: WarpModel
  @State private var backups: [Backup] = []
  @State private var confirmRestore: Backup?
  @State private var preview = SavePreviewData()

  private var console: String { model.library.console.isEmpty ? "Switch" : model.library.console }
  /// This game's place on the Mac: "Astris" or its nxnative app's name.
  private var emulator: String { game.macShort }

  /// The card's width, and the room its content has inside the padding. The content is
  /// held to that width, so a long label wraps instead of pushing past the card's edge.
  static let width: CGFloat = 340
  private static let padding: CGFloat = 16
  private static let contentWidth = width - 2 * padding

  var body: some View {
    ScrollView(.vertical) {
      VStack(alignment: .leading, spacing: 16) {
        header
        if preview.supported {
          SavePreviewSection(preview: preview,
                             source: game.isNative ? "the Mac app's save" : "\(emulator)'s save",
                             gameKey: game.titleId)
        }
        if LiveSection.isRunning(game, model) {
          LiveSection(game: game, model: model)
        }
        Divider()
        status
        nxnativeSection
        actions
        if let message = model.libraryMessage, message.contains(game.name) || message.hasPrefix("Restore") {
          Text(message).font(.callout).foregroundStyle(.secondary)
            .fixedSize(horizontal: false, vertical: true)
        }
        Divider()
        if model.pluginOn("mods") {
          ModsSection(game: game, model: model)
        } else {
          Text("Mods is off. Turn it on in Plugins to add mods to this game.")
            .font(.callout).foregroundStyle(.secondary)
            .fixedSize(horizontal: false, vertical: true)
        }
        Divider()
        backupList
      }
      .frame(width: Self.contentWidth, alignment: .leading)
      .padding(Self.padding)
    }
    .frame(width: Self.width)
    .onAppear(perform: load)
    .onChange(of: game) { load() }
    .onChange(of: model.lastSync) { load() }
    .alert(item: $confirmRestore) { b in
      Alert(title: Text("Restore this backup to \(emulator)?"),
            message: Text("Your current \(game.name) save in \(emulator) is backed up first, so you can undo this. The next sync carries it to \(console)."),
            primaryButton: .default(Text("Restore")) { model.restore(game, backup: b) },
            secondaryButton: .cancel())
    }
  }

  private func loadBackups() { backups = model.backups(for: game.key) }
  private func load() {
    loadBackups()
    preview = model.savePreview(game.titleId, folder: game.savePath)
  }

  private var header: some View {
    HStack(alignment: .center, spacing: 12) {
      GameArt(path: game.icon, size: 72)
        .shadow(color: .black.opacity(0.2), radius: 4, y: 2)
      VStack(alignment: .leading, spacing: 2) {
        Text(game.name).font(.title3.weight(.semibold))
          .fixedSize(horizontal: false, vertical: true)
        if !game.publisher.isEmpty {
          Text(game.publisher).font(.callout).foregroundStyle(.secondary)
        }
      }
    }
  }

  private var status: some View {
    VStack(alignment: .leading, spacing: 8) {
      HStack(spacing: 6) {
        StatusBadge(status: game.status)
        Text(StatusBadge.text(game.status, model: model, place: game.macName)).font(.headline)
      }
      if !game.lastText.isEmpty, let at = WarpDates.parse(game.lastAt) {
        Text(lastSync(at)).font(.callout).foregroundStyle(.secondary)
      }
      VStack(alignment: .leading, spacing: 4) {
        place(console, has: game.onConsole)
        place(game.macName, has: game.inEmulator)
      }
      .padding(.top, 2)
    }
  }

  /// "Checked 2 minutes ago", or "Copied Astris → Will's Switch 2 minutes ago".
  private func lastSync(_ at: Date) -> String {
    let when = WarpModel.ago(at)
    if game.lastText == "in sync" || game.status == "held" { return "Checked \(when)" }
    let text = game.lastText.replacingOccurrences(of: "->", with: "→")  // older records
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
        .disabled(!model.library.consolePresent || blockedByOpenApp)
        if !model.library.consolePresent {
          Text("Open WarpPoint on your Switch first.").font(.caption).foregroundStyle(.secondary)
        } else if game.isNative, game.nxnative?.running == true {
          Text("Quit \(emulator) first.").font(.caption).foregroundStyle(.secondary)
        } else if !game.isNative, model.library.emulatorOpen {
          Text("Quit \(emulator) first: it keeps its list of saves in memory while it's open.")
            .font(.caption).foregroundStyle(.secondary)
        }
      }
    } else if game.status == "emulatorOnly" {
      Text(model.library.consoleCanCreate
           ? "Not on \(console) yet. WarpPoint makes its save there on the next sync, as long as the game is installed on \(console)."
           : "Not on \(console) yet. Start the game there once, then pick which save to keep on the next sync. (WarpPoint 1.3 on the Switch can make the save itself.)")
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

  private var blockedByOpenApp: Bool {
    game.isNative ? game.nxnative?.running == true : model.library.emulatorOpen
  }

  /// The game's nxnative app: where it is, whether it's open, nxnative's own cloud sync,
  /// and (when Astris has the game too) which of the two WarpPoint syncs.
  @ViewBuilder private var nxnativeSection: some View {
    if let app = game.nxnative {
      VStack(alignment: .leading, spacing: 8) {
        HStack(spacing: 6) {
          NativeBadge()
          Text(app.running ? "Running now" : "Mac app").font(.callout)
            .foregroundStyle(.secondary)
          Spacer(minLength: 0)
        }
        if game.canUseEmulator {
          // A radio group, not a segmented control: an app's full name fits on its own line.
          VStack(alignment: .leading, spacing: 4) {
            Text("Sync \(console) with").font(.callout)
            Picker("Sync with", selection: Binding(
              get: { game.isNative ? "nxnative" : "emulator" },
              set: { model.setUseEmulator(game, $0 == "emulator") })) {
              Text("\(app.name) (Mac app)").tag("nxnative")
              Text(model.library.emulator).tag("emulator")
            }
            .pickerStyle(.radioGroup)
            .labelsHidden()
          }
          Text(game.isNative
               ? "WarpPoint syncs \(console) with the Mac app. \(model.library.emulator)'s copy\(game.emulatorHasSave ? "" : ", if any,") is left as it is."
               : "WarpPoint syncs \(console) with \(model.library.emulator). The Mac app's save is left as it is.")
            .font(.caption).foregroundStyle(.secondary)
            .fixedSize(horizontal: false, vertical: true)
        }
        if game.isNative, app.upgradesSave {
          Toggle("Send its saves to \(console)", isOn: Binding(
            get: { game.sendToConsole ?? false },
            set: { model.setSendToConsole(game, $0) }))
            .toggleStyle(.switch)
            .controlSize(.small)
          Text((game.sendToConsole ?? false)
               ? "Only for a Switch 2: the Switch 1 version of this game crashes on saves the Switch 2 Edition has upgraded."
               : "This is the Switch 2 Edition. It upgrades the save as it loads it, and the Switch 1 version then crashes reading it. So your newer saves stay on this Mac, and \(console)'s saves still come here.")
            .font(.caption).foregroundStyle(.secondary)
            .fixedSize(horizontal: false, vertical: true)
        }
        if game.isNative, !app.cloudFolder.isEmpty {
          Label {
            Text("nxnative's own cloud saves are on for this game. That's fine: after WarpPoint copies a save in, the app sends it to iCloud the next time it starts. If you also play on an iPhone, let each sync finish before switching.")
          } icon: {
            Image(systemName: "icloud").foregroundStyle(.orange)
          }
          .font(.caption)
          .fixedSize(horizontal: false, vertical: true)
        }
        HStack {
          Button("Open App") {
            NSWorkspace.shared.openApplication(at: URL(fileURLWithPath: app.path),
                                               configuration: NSWorkspace.OpenConfiguration())
          }
          .disabled(app.running)
          Button("Show in Finder") {
            NSWorkspace.shared.activateFileViewerSelecting([URL(fileURLWithPath: app.path)])
          }
        }
        .controlSize(.small)
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
            HStack(spacing: 4) {
              if b.pinned { Image(systemName: "pin.fill").foregroundStyle(.orange).font(.caption) }
              Text(WarpDates.parse(b.created).map { $0.formatted(date: .abbreviated, time: .shortened) } ?? b.created)
                .font(.callout.weight(.medium))
            }
            Text(describe(b)).font(.caption).foregroundStyle(.secondary)
              .fixedSize(horizontal: false, vertical: true)
            if model.pluginOn("chronicle") {  // Warp Chronicle: notes and pins
              BackupNote(note: b.note) { note in
                _ = model.labelBackup(game, b, note: note, pinned: b.pinned)
                loadBackups()
              }
            }
          }
          Spacer()
          if model.pluginOn("chronicle") {
            Button {
              _ = model.labelBackup(game, b, note: b.note, pinned: !b.pinned)
              loadBackups()
            } label: { Image(systemName: b.pinned ? "pin.slash" : "pin") }
              .buttonStyle(.borderless)
              .help(b.pinned ? "Let WarpPoint remove it with the older ones" : "Keep this one forever")
          }
          Button("Restore…") { confirmRestore = b }
            .controlSize(.small)
            .disabled(game.isNative ? game.nxnative?.running == true
                                    : model.library.emulatorOpen && !game.inEmulator)
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

/// Warp Chronicle: a backup's note, edited in place ("before the Garreg Mach fight").
private struct BackupNote: View {
  let note: String
  let save: (String) -> Void
  @State private var text = ""
  var body: some View {
    TextField("Add a note", text: $text, axis: .vertical)
      .lineLimit(1...4)
      .textFieldStyle(.plain)
      .font(.caption)
      .foregroundStyle(.primary)
      .onAppear { text = note }
      .onChange(of: note) { text = note }
      .onSubmit { if text != note { save(text) } }
  }
}
