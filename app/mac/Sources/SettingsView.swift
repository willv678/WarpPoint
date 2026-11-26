import AppKit
import SwiftUI

/// WarpPoint → Settings (⌘,): users, backups, the emulator's folder, login.
struct SettingsView: View {
  @ObservedObject var model: WarpModel
  @State private var error: String?

  private var setup: Setup { model.setup }
  private let keepChoices = [2, 4, 8, 16, 0]

  var body: some View {
    Form {
      Section("Users") {
        UserLinks(setup: setup, choice: { setup.links[$0] ?? "" },
                  choose: { error = model.setLink($0, $1) })
      }
      Section {
        Picker("Keep", selection: Binding(get: { setup.keepBackups },
                                          set: { error = model.setKeepBackups($0) })) {
          ForEach(keepChoices, id: \.self) { n in
            Text(n == 0 ? "Every backup" : "Newest \(n) per save").tag(n)
          }
        }
      } header: {
        Text("Backups")
      } footer: {
        Text("Before WarpPoint replaces a save anywhere, it keeps a copy on this Mac.")
          .font(.caption).foregroundStyle(.secondary)
      }
      Section(setup.emulator.name) {
        LabeledContent("Data folder") {
          Text(setup.emulator.path).lineLimit(1).truncationMode(.middle)
            .foregroundStyle(setup.emulator.found ? .secondary : Color.orange)
            .help(setup.emulator.path)
        }
        HStack {
          Button("Choose…") { chooseFolder() }
          Button("Show in Finder") {
            NSWorkspace.shared.open(URL(fileURLWithPath: setup.emulator.path))
          }
          .disabled(!setup.emulator.found)
        }
      }
      NxNativeSettings(setup: setup,
                       setEnabled: { error = model.setNxNativeEnabled($0) },
                       setUser: { error = model.setNxNativeUser($0) })
      Section {
        Toggle("Copy screenshots and videos to this Mac", isOn: Binding(
          get: { setup.album.enabled },
          set: { error = model.setAlbum(enabled: $0, folder: setup.album.folder) }))
        LabeledContent("Folder") {
          Text(setup.album.folder).lineLimit(1).truncationMode(.middle).foregroundStyle(.secondary)
            .help(setup.album.folder)
        }
        HStack {
          Button("Choose…") { chooseAlbumFolder() }
          Button("Show in Finder") {
            try? FileManager.default.createDirectory(atPath: setup.album.folder,
                                                     withIntermediateDirectories: true)
            NSWorkspace.shared.open(URL(fileURLWithPath: setup.album.folder))
          }
        }
      } header: {
        Text("Album")
      } footer: {
        Text("New captures from your Switch's album land here after each sync, in a folder per game. Nothing on the Switch is changed.")
          .font(.caption).foregroundStyle(.secondary)
      }
      DiscordSettings(setup: setup, set: { error = model.setPresence(enabled: $0, appId: $1) },
                      setMac: { error = model.setPresenceMac($0) })
      SplitSettings(setup: setup, set: { error = model.setSplit(enabled: $0, host: $1, port: $2) })
      Section("General") {
        Toggle("Open WarpPoint when you log in", isOn: $model.openAtLogin)
      }
      if let error {
        Label(error, systemImage: "exclamationmark.triangle.fill").foregroundStyle(.orange)
      }
    }
    .formStyle(.grouped)
    .frame(width: 480)
    .frame(minHeight: 560)
    .onAppear { model.windowOpened() }
    .onDisappear { model.windowClosed() }
  }

  private func chooseAlbumFolder() {
    let panel = NSOpenPanel()
    panel.canChooseDirectories = true
    panel.canChooseFiles = false
    panel.canCreateDirectories = true
    panel.message = "Choose where your Switch's screenshots and videos go."
    if panel.runModal() == .OK, let url = panel.url {
      error = model.setAlbum(enabled: setup.album.enabled, folder: url.path)
    }
  }

  private func chooseFolder() {
    let panel = NSOpenPanel()
    panel.canChooseDirectories = true
    panel.canChooseFiles = false
    panel.message = "Choose the emulator's data folder (the one with \"bis\" and \"system\" in it)."
    if panel.runModal() == .OK, let url = panel.url {
      error = model.setEmulatorPath(url.path)
    }
  }
}

/// The nxnative ports on this Mac and whose saves they hold. Shared by Settings and setup.
struct NxNativeSettings: View {
  let setup: Setup
  let setEnabled: (Bool) -> Void
  let setUser: (String) -> Void
  var showToggle = true

  private var nx: Setup.NxNative { setup.nxnative }

  var body: some View {
    Section {
      if showToggle {
        Toggle("Sync nxnative apps", isOn: Binding(get: { nx.enabled }, set: { setEnabled($0) }))
      }
      if nx.enabled {
        if nx.apps.isEmpty {
          Text("No nxnative apps in Applications yet. When you add one, its game syncs with your Switch like an Astris game.")
            .font(.callout).foregroundStyle(.secondary)
            .fixedSize(horizontal: false, vertical: true)
        } else {
          Picker("Their saves belong to", selection: Binding(get: { nx.user }, set: { setUser($0) })) {
            if nx.user.isEmpty { Text("Choose…").tag("") }
            ForEach(setup.usersByActivity) { u in Text(u.label).tag(u.id) }
            if !nx.user.isEmpty, !setup.consoleUsers.contains(where: { $0.id == nx.user }) {
              Text("User \(nx.user.prefix(6))…").tag(nx.user)
            }
          }
          ForEach(nx.apps, id: \.path) { app in
            LabeledContent {
              HStack(spacing: 6) {
                if !app.cloudFolder.isEmpty {
                  Image(systemName: "icloud").foregroundStyle(.orange)
                    .help("nxnative's own cloud saves are on for this game: \(app.cloudFolder)")
                }
                Text(app.running ? "running" : app.hasSave ? "has a save" : "no save yet")
                  .foregroundStyle(.secondary)
              }
            } label: {
              VStack(alignment: .leading, spacing: 1) {
                Text(app.name)
                Text(app.saveTitleId == app.appTitleId
                     ? "Save \(app.saveTitleId.uppercased())"
                     : "Save \(app.saveTitleId.uppercased()) (app \(app.appTitleId.uppercased()))")
                  .font(.caption.monospaced()).foregroundStyle(.secondary)
              }
            }
          }
        }
      }
    } header: {
      Text("nxnative")
    } footer: {
      Text("Games ported with nxnative run as their own Mac apps. WarpPoint copies their saves to and from your Switch while the app is closed, and never touches nxnative's own cloud-save files.")
        .font(.caption).foregroundStyle(.secondary)
    }
  }
}

/// Warp Presence: the Switch's game in Discord, and games on this Mac.
struct DiscordSettings: View {
  let setup: Setup
  let set: (Bool, String) -> Void
  let setMac: (Bool) -> Void
  @State private var appId = ""

  var body: some View {
    Section {
      Toggle("Show what I'm playing on the Switch", isOn: Binding(
        get: { setup.presence.enabled }, set: { set($0, appId) }))
      Toggle("Show games I play on this Mac", isOn: Binding(
        get: { setup.presence.showMac }, set: { setMac($0) }))
        .disabled(!setup.presence.enabled)
      TextField("Discord Application ID", text: $appId)
        .onSubmit { set(setup.presence.enabled, appId) }
        .onAppear { appId = setup.presence.discordAppId }
      LabeledContent("Status") {
        Text(setup.presence.status).foregroundStyle(.secondary)
      }
    } header: {
      Text("Discord")
    } footer: {
      Text("Discord shows activity under an app you own: make one at discord.com/developers/applications (call it \"Nintendo Switch\"), copy its Application ID here, and press Return. Discord then shows \"Playing Nintendo Switch\" with the game's name. Games on this Mac (nxnative ports and Astris) show too, \"on Mac\" or \"in Astris\", when nothing is playing on the Switch. The game's picture comes from tinfoil.media: WarpPoint asks it once per game whether it has one, sending only the game's id.")
        .font(.caption).foregroundStyle(.secondary)
    }
  }
}

/// Warp Split: LiveSplit auto-splitting from the game's Tap profile.
struct SplitSettings: View {
  let setup: Setup
  let set: (Bool, String, Int) -> Void
  @State private var host = "127.0.0.1"
  @State private var port = "16834"

  var body: some View {
    Section {
      Toggle("Auto-split runs on the Switch", isOn: Binding(
        get: { setup.split.enabled }, set: { set($0, host, Int(port) ?? 16834) }))
      HStack {
        TextField("LiveSplit computer", text: $host)
        TextField("Port", text: $port).frame(width: 70)
      }
      .onSubmit { set(setup.split.enabled, host, Int(port) ?? 16834) }
      .onAppear {
        host = setup.split.host
        port = String(setup.split.port)
      }
      LabeledContent("Status") { Text(setup.split.status).foregroundStyle(.secondary) }
    } header: {
      Text("LiveSplit")
    } footer: {
      Text("In LiveSplit, turn on Control › Start TCP Server. WarpPoint starts, splits and pauses the timer by the rules in the running game's Tap profile (its \"splits\").")
        .font(.caption).foregroundStyle(.secondary)
    }
  }
}
