import AppKit
import SwiftUI

/// A plugin's own settings, shown on its page in the Plugins window. Turning a plugin on
/// or off is the page's switch; these are only what's left to set.
struct PluginSettings: View {
  let plugin: Plugin
  @ObservedObject var model: WarpModel
  @Binding var error: String?
  @Environment(\.openWindow) private var openWindow

  var body: some View {
    switch plugin.id {
    case "album":
      AlbumSettings(setup: model.setup, set: { error = model.setAlbum(enabled: model.setup.album.enabled, folder: $0) })
    case "presence":
      DiscordSettings(setup: model.setup,
                      set: { error = model.setPresence(enabled: model.setup.presence.enabled, appId: $0) },
                      setMac: { error = model.setPresenceMac($0) })
    case "split":
      SplitSettings(setup: model.setup,
                    set: { error = model.setSplit(enabled: model.setup.split.enabled, host: $0, port: $1) })
    case "lens":
      Section {
        LabeledContent("Reports on this Mac") { Text("\(model.reports.count)").foregroundStyle(.secondary) }
        open("Open Crash Reports", "reports")
      }
    case "tap":
      Section {
        open("Open Live Game Info", "live")
        LabeledContent("Your own profiles") {
          Button("Show in Finder") { showDataSubfolder("tap") }
        }
      } footer: {
        Text("Profiles you write go in this folder, next to packs from Get More. For the same game and version, a file of yours or a pack wins over the built-in profile.")
          .font(.caption).foregroundStyle(.secondary)
      }
    case "gamedata":
      Section {
        LabeledContent("Last check") {
          Text(model.gameDataChecked.map { WarpModel.ago($0) } ?? "not yet").foregroundStyle(.secondary)
        }
        if let note = model.gameDataNote {
          Text(note).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
        }
        Button("Check Now") { model.updateGameData() }
          .disabled(!plugin.enabled || model.storeLoading)
      } header: {
        Text("Settings")
      }
    case "lexicon":
      Section { open("Open Lexicon", "lexicon") }
    case "mods":
      Section {
        open("Open Library", "library")
      } footer: {
        Text("Add and switch mods in a game's details in the Library.")
          .font(.caption).foregroundStyle(.secondary)
      }
    case "chronicle":
      Section {
        open("Open Library", "library")
      } footer: {
        Text("Notes and pins are in a game's details in the Library, next to each backup.")
          .font(.caption).foregroundStyle(.secondary)
      }
    default:
      EmptyView()
    }
  }

  private func open(_ title: String, _ window: String) -> some View {
    Button(title) {
      openWindow(id: window)
      NSApp.activate()
    }
  }

  private func showDataSubfolder(_ name: String) {
    guard let url = model.dataFolder?.appendingPathComponent(name) else { return }
    try? FileManager.default.createDirectory(at: url, withIntermediateDirectories: true)
    NSWorkspace.shared.open(url)
  }
}

/// Warp Album: where captures go.
struct AlbumSettings: View {
  let setup: Setup
  let set: (String) -> Void

  var body: some View {
    Section {
      LabeledContent("Folder") {
        Text(setup.album.folder).lineLimit(1).truncationMode(.middle).foregroundStyle(.secondary)
          .help(setup.album.folder)
      }
      HStack {
        Button("Choose…") { choose() }
        Button("Show in Finder") {
          try? FileManager.default.createDirectory(atPath: setup.album.folder,
                                                   withIntermediateDirectories: true)
          NSWorkspace.shared.open(URL(fileURLWithPath: setup.album.folder))
        }
      }
    } header: {
      Text("Settings")
    } footer: {
      Text("A folder per game, named by the game. Nothing on the Switch is changed.")
        .font(.caption).foregroundStyle(.secondary)
    }
  }

  private func choose() {
    let panel = NSOpenPanel()
    panel.canChooseDirectories = true
    panel.canChooseFiles = false
    panel.canCreateDirectories = true
    panel.message = "Choose where your Switch's screenshots and videos go."
    if panel.runModal() == .OK, let url = panel.url { set(url.path) }
  }
}

/// Warp Presence: the Switch's game in Discord, and games on this Mac.
struct DiscordSettings: View {
  let setup: Setup
  let set: (String) -> Void
  let setMac: (Bool) -> Void
  @State private var appId = ""

  var body: some View {
    Section {
      Toggle("Show games I play on this Mac", isOn: Binding(
        get: { setup.presence.showMac }, set: { setMac($0) }))
      TextField("Discord Application ID", text: $appId)
        .onSubmit { set(appId) }
        .onAppear { appId = setup.presence.discordAppId }
      LabeledContent("Status") {
        Text(setup.presence.status).foregroundStyle(.secondary)
      }
    } header: {
      Text("Settings")
    } footer: {
      Text("Discord shows activity under an app you own: make one at discord.com/developers/applications (call it \"Nintendo Switch\"), copy its Application ID here, and press Return. Discord then shows \"Playing Nintendo Switch\" with the game's name. Games on this Mac (nxnative ports and Astris) show too, \"on Mac\" or \"in Astris\", when nothing is playing on the Switch. The game's picture comes from tinfoil.media: WarpPoint asks it once per game whether it has one, sending only the game's id.")
        .font(.caption).foregroundStyle(.secondary)
    }
  }
}

/// Warp Split: where LiveSplit's TCP server is.
struct SplitSettings: View {
  let setup: Setup
  let set: (String, Int) -> Void
  @State private var host = "127.0.0.1"
  @State private var port = "16834"

  var body: some View {
    Section {
      HStack {
        TextField("LiveSplit computer", text: $host)
        TextField("Port", text: $port).frame(width: 70)
      }
      .onSubmit { set(host, Int(port) ?? 16834) }
      .onAppear {
        host = setup.split.host
        port = String(setup.split.port)
      }
      LabeledContent("Status") { Text(setup.split.status).foregroundStyle(.secondary) }
    } header: {
      Text("Settings")
    } footer: {
      Text("127.0.0.1 is this Mac. Press Return to save. In LiveSplit, turn on Control › Start TCP Server.")
        .font(.caption).foregroundStyle(.secondary)
    }
  }
}
