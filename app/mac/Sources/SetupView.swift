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

      step(4, done: true, title: "Extras") {
        extras
      }

      step(5, done: true, title: "Run in the background") {
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
    .frame(width: 560, height: 820)
    .onAppear {
      model.windowOpened()
      model.refreshPlugins()
      model.setupOpen = true
      prefill()
    }
    .onDisappear {
      model.setupOpen = false
      model.windowClosed()
    }
    .onChange(of: model.setup.consoleUsers) { prefill() }
  }

  // MARK: - Extras: the plugins, at a glance

  /// Plugins in the setup window, two to a row. Labs is Terminal only: one muted line.
  private var shownPlugins: [Plugin] { model.plugins.filter { !$0.terminalOnly } }

  private var extras: some View {
    VStack(alignment: .leading, spacing: 8) {
      Text("Optional plugins. The defaults are fine; nothing here holds up syncing.")
        .foregroundStyle(.secondary)
      VStack(alignment: .leading, spacing: 0) {
        Grid(alignment: .topLeading, horizontalSpacing: 14, verticalSpacing: 10) {
          ForEach(Array(stride(from: 0, to: shownPlugins.count, by: 2)), id: \.self) { i in
            GridRow {
              extraCell(shownPlugins[i])
              if i + 1 < shownPlugins.count {
                extraCell(shownPlugins[i + 1])
              } else {
                Color.clear.gridCellUnsizedAxes([.horizontal, .vertical])
              }
            }
          }
        }
        if let labs = model.plugins.first(where: { $0.terminalOnly }) {
          Divider().padding(.vertical, 8)
          HStack(spacing: 6) {
            Image(systemName: labs.icon)
            Text("\(labs.name): developer tools, in Terminal.")
          }
          .font(.caption)
          .foregroundStyle(.secondary)
        }
      }
      .padding(12)
      .background(RoundedRectangle(cornerRadius: 10).fill(.background.secondary))
      .overlay(RoundedRectangle(cornerRadius: 10).strokeBorder(.separator.opacity(0.6)))
      Button("Change these any time in Plugins.") { showPlugins(nil) }
        .buttonStyle(.link)
        .font(.callout)
    }
  }

  /// A plugin that would do nothing when switched on (no Discord ID, no LiveSplit yet)
  /// gets "Set up…", which opens its page, instead of a switch.
  private func needsSetup(_ p: Plugin) -> Bool {
    !p.enabled && (!p.default || p.needs.contains { $0.kind == "setting" && $0.met == false })
  }

  private func extraCell(_ p: Plugin) -> some View {
    HStack(alignment: .top, spacing: 8) {
      PluginIcon(symbol: p.icon, on: p.enabled, size: 24)
      VStack(alignment: .leading, spacing: 2) {
        HStack(spacing: 6) {
          Text(p.name).font(.callout.weight(.medium))
          Spacer(minLength: 4)
          if needsSetup(p) {
            Button("Set Up…") { showPlugins(p.id) }
              .controlSize(.small)
              .help("Open \(p.name) in Plugins to set it up")
          } else {
            Toggle(p.name, isOn: Binding(get: { p.enabled },
                                         set: { error = model.setPluginEnabled(p.id, $0) }))
              .toggleStyle(.switch)
              .controlSize(.mini)
              .labelsHidden()
          }
        }
        Text(p.summary)
          .font(.caption)
          .foregroundStyle(.secondary)
          .lineLimit(3)
          .fixedSize(horizontal: false, vertical: true)
      }
    }
    .frame(maxWidth: .infinity, alignment: .leading)
    .help(p.summary)
  }

  /// The Plugins window, on a plugin's page when one is given.
  private func showPlugins(_ page: String?) {
    model.pluginsPage = page
    openWindow(id: "plugins")
    NSApp.activate()
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
