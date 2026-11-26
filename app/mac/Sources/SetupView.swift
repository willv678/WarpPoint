import AppKit
import SwiftUI

/// The first-run window: find the emulator, hear the Switch, link users, done.
struct SetupView: View {
  @ObservedObject var model: WarpModel
  @Environment(\.dismissWindow) private var dismissWindow
  @Environment(\.openWindow) private var openWindow
  @State private var pending: [String: String] = [:]  // console user -> profile, not saved yet
  @State private var openAtLogin = true
  @State private var error: String?

  private var setup: Setup { model.setup }

  @State private var nxUser = ""  // the console user whose saves the nxnative apps hold

  private var linked: Bool { pending.values.contains { !$0.isEmpty } }
  private var hasApps: Bool { setup.nxnative.enabled && !setup.nxnative.apps.isEmpty }
  /// Astris with a linked user, or nxnative apps with their user chosen (or both).
  private var ready: Bool { (setup.emulator.found && linked) || (hasApps && !nxUser.isEmpty) }

  var body: some View {
    VStack(alignment: .leading, spacing: 20) {
      HStack(spacing: 14) {
        Image(nsImage: UFOIcon.image(size: 52)).renderingMode(.template).foregroundStyle(.primary)
        VStack(alignment: .leading, spacing: 3) {
          Text("Welcome to WarpPoint").font(.title.weight(.semibold))
          Text("Keeps your Switch saves and \(setup.emulator.name) in sync, on its own.")
            .foregroundStyle(.secondary)
        }
      }

      step(1, done: setup.emulator.found || hasApps, title: hasApps ? "\(setup.emulator.name) and nxnative" : setup.emulator.name) {
        if setup.emulator.found {
          Text("Found \(setup.emulator.name)'s saves.").foregroundStyle(.secondary)
        } else if !hasApps {
          Text("Couldn't find it. Install Astris and open it once, or choose its data folder.")
            .foregroundStyle(.secondary)
          Button("Choose Folder…") { chooseFolder() }
        }
        if hasApps {
          Text("Found \(setup.nxnative.apps.count) nxnative app\(setup.nxnative.apps.count == 1 ? "" : "s"): \(setup.nxnative.apps.map(\.name).joined(separator: ", ")).")
            .foregroundStyle(.secondary)
            .fixedSize(horizontal: false, vertical: true)
        }
      }

      step(2, done: setup.consolePresent || !setup.consoleUsers.isEmpty, title: "Your Switch") {
        if setup.consolePresent {
          Text("\(setup.console) is connected.").foregroundStyle(.secondary)
        } else {
          HStack(spacing: 8) {
            ProgressView().controlSize(.small)
            Text("On your Switch, open the homebrew menu and start WarpPoint. Keep both on the same Wi-Fi.")
              .foregroundStyle(.secondary)
          }
        }
      }

      step(3, done: linked, title: "Whose saves go where") {
        Text(setup.emulator.found
             ? "Pick the \(setup.emulator.name) profile for each Switch user you play as."
             : "Pick the Switch user you play as.")
          .foregroundStyle(.secondary)
        if setup.emulator.found {
          UserLinks(setup: setup, choice: { pending[$0] ?? "" }, choose: { pending[$0] = $1 })
            .disabled(setup.emulatorProfiles.isEmpty)
        }
        if hasApps, !setup.consoleUsers.isEmpty {
          Picker("nxnative apps' saves belong to", selection: $nxUser) {
            Text("Choose…").tag("")
            ForEach(setup.usersByActivity) { u in Text(u.label).tag(u.id) }
          }
        }
      }

      step(4, done: true, title: "Run in the background") {
        Toggle("Open WarpPoint when you log in", isOn: $openAtLogin)
      }

      Spacer(minLength: 0)
      HStack {
        if let error {
          Label(error, systemImage: "exclamationmark.triangle.fill").foregroundStyle(.orange)
        }
        Spacer()
        Button("Start Syncing") { finish() }
          .keyboardShortcut(.defaultAction)
          .controlSize(.large)
          .disabled(!ready)
      }
    }
    .padding(26)
    .frame(width: 560, height: 600)
    .onAppear {
      model.windowOpened()
      prefill()
    }
    .onDisappear { model.windowClosed() }
    .onChange(of: model.setup.consoleUsers) { prefill() }
  }

  private func step<Content: View>(_ n: Int, done: Bool, title: String,
                                   @ViewBuilder content: () -> Content) -> some View {
    HStack(alignment: .top, spacing: 12) {
      ZStack {
        Circle().fill(done ? Color.green : Color.secondary.opacity(0.2)).frame(width: 26, height: 26)
        if done {
          Image(systemName: "checkmark").font(.system(size: 12, weight: .bold)).foregroundStyle(.white)
        } else {
          Text("\(n)").font(.callout.weight(.semibold)).foregroundStyle(.secondary)
        }
      }
      VStack(alignment: .leading, spacing: 6) {
        Text(title).font(.headline)
        content()
      }
      Spacer(minLength: 0)
    }
  }

  /// Start from what's saved, or the likely answer: the busiest Switch user gets the
  /// first emulator profile.
  private func prefill() {
    if !setup.links.isEmpty {
      pending = setup.links
    } else if pending.isEmpty, let top = setup.usersByActivity.first,
              let profile = setup.emulatorProfiles.first {
      pending = [top.id: profile.id]
    }
    if nxUser.isEmpty {
      nxUser = setup.nxnative.user.isEmpty ? (setup.usersByActivity.first?.id ?? "") : setup.nxnative.user
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

  private func finish() {
    if hasApps, !nxUser.isEmpty, let failure = model.setNxNativeUser(nxUser) {
      error = failure
      return
    }
    for user in setup.consoleUsers where setup.emulator.found {
      if let failure = model.setLink(user.id, pending[user.id] ?? "") {
        error = failure
        return
      }
    }
    model.openAtLogin = openAtLogin
    dismissWindow(id: "setup")
    openWindow(id: "library")
    NSApp.activate()
  }
}
