import AppKit
import SwiftUI

/// Warp Mods state for one game, as the engine reports it.
struct ModsState: Decodable {
  struct Mod: Decodable, Identifiable {
    let name: String
    let enabled: Bool
    let files: Int
    let bytes: Int64
    let romfs: Bool
    let exefs: Bool
    var id: String { name }
  }
  var mods: [Mod] = []
  var conflicts: [String] = []
  var exefs: Bool = false
  var `where`: String = ""
}

/// A game's mods: one library on the Mac, applied to the Switch and to this Mac's copy.
struct ModsSection: View {
  let game: Game
  @ObservedObject var model: WarpModel
  @State private var state = ModsState()
  @State private var message: String?

  var body: some View {
    VStack(alignment: .leading, spacing: 8) {
      HStack {
        Text("Mods").font(.headline)
        Spacer()
        Button("Add Mod…") { add() }.controlSize(.small)
      }
      if state.mods.isEmpty {
        Text("Add a mod's folder (one with romfs or exefs in it, or an SD-card release). Turned on, it goes to \(model.library.console.isEmpty ? "the Switch" : model.library.console) and \(state.where.isEmpty ? "this Mac" : state.where) at the next sync.")
          .font(.callout).foregroundStyle(.secondary)
          .fixedSize(horizontal: false, vertical: true)
      }
      ForEach(state.mods) { m in
        HStack {
          Toggle(isOn: Binding(get: { m.enabled }, set: { on in
            message = model.setModEnabled(game.titleId, name: m.name, on: on)
            reload()
          })) {
            VStack(alignment: .leading, spacing: 1) {
              Text(m.name)
              Text("\(m.files) files · \(ByteCountFormatter.string(fromByteCount: m.bytes, countStyle: .file))\(m.exefs ? " · code" : "")")
                .font(.caption).foregroundStyle(.secondary)
            }
          }
          .toggleStyle(.switch).controlSize(.small)
          Spacer()
          Button {
            message = model.removeMod(game.titleId, name: m.name)
            reload()
          } label: { Image(systemName: "trash") }
            .buttonStyle(.borderless)
            .help("Remove from the library (turns it off everywhere at the next sync)")
        }
      }
      ForEach(state.conflicts, id: \.self) { c in
        Label(c, systemImage: "exclamationmark.triangle").font(.caption).foregroundStyle(.orange)
      }
      if state.exefs && game.isNative {
        Text("nxnative apps load RomFS mods only; code (exefs) mods apply on the Switch.")
          .font(.caption).foregroundStyle(.secondary)
      }
      if let message {
        Text(message).font(.caption).foregroundStyle(.orange)
      }
    }
    .onAppear(perform: reload)
    .onChange(of: game.key) { reload() }
  }

  private func reload() { state = model.mods(game.titleId) }

  private func add() {
    let panel = NSOpenPanel()
    panel.canChooseDirectories = true
    panel.canChooseFiles = false
    panel.message = "Choose the mod's folder (with romfs or exefs in it)."
    if panel.runModal() == .OK, let url = panel.url {
      message = model.addMod(game.titleId, folder: url.path, name: url.lastPathComponent)
      reload()
    }
  }
}
