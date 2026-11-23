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
      Section("General") {
        Toggle("Open WarpPoint when you log in", isOn: $model.openAtLogin)
      }
      if let error {
        Label(error, systemImage: "exclamationmark.triangle.fill").foregroundStyle(.orange)
      }
    }
    .formStyle(.grouped)
    .frame(width: 480)
    .frame(minHeight: 460)
    .onAppear { model.refreshSetup() }
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
