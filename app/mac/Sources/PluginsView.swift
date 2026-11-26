import AppKit
import SwiftUI

/// The Plugins window: every plugin, what it does, how it works, its settings and
/// whether it's on; and Get More, the plugin store.
struct PluginsView: View {
  @ObservedObject var model: WarpModel
  @State private var selection: String? = "lens"
  @State private var timer: Timer?

  static let storeTag = "#store"

  var body: some View {
    NavigationSplitView {
      List(selection: $selection) {
        Section("Plugins") {
          ForEach(model.plugins) { p in
            PluginRow(plugin: p).tag(p.id)
          }
        }
        Section("Plugin Store") {
          HStack(spacing: 10) {
            PluginIcon(symbol: "arrow.down.circle", on: true)
            VStack(alignment: .leading, spacing: 2) {
              Text("Get More").font(.body.weight(.medium))
              Text(storeSubtitle).font(.caption).foregroundStyle(.secondary).lineLimit(1)
            }
          }
          .padding(.vertical, 3)
          .tag(Self.storeTag)
        }
      }
      .listStyle(.sidebar)
      .navigationSplitViewColumnWidth(min: 250, ideal: 270, max: 320)
    } detail: {
      if selection == Self.storeTag {
        PluginStoreView(model: model)
      } else if let p = model.plugins.first(where: { $0.id == selection }) {
        PluginDetail(plugin: p, model: model, showStore: { selection = Self.storeTag })
          .id(p.id)
      } else {
        Text("Choose a plugin").foregroundStyle(.secondary)
          .frame(maxWidth: .infinity, maxHeight: .infinity)
      }
    }
    .frame(minWidth: 760, minHeight: 520)
    .onAppear {
      model.windowOpened()
      model.refreshPlugins()
      model.refreshReports()
      // For screenshots and checks: WARP_SELECT=<plugin id> or "store".
      if let s = ProcessInfo.processInfo.environment["WARP_SELECT"]?.lowercased() {
        if s == "store" { selection = Self.storeTag }
        else if model.plugins.contains(where: { $0.id == s }) { selection = s }
      }
      // Live status lines (Presence, Split) and "needs" change while the window is open.
      timer = Timer.scheduledTimer(withTimeInterval: 3, repeats: true) { _ in
        MainActor.assumeIsolated { model.refreshPlugins() }
      }
    }
    .onDisappear {
      timer?.invalidate()
      timer = nil
      model.windowClosed()
    }
  }

  private var storeSubtitle: String {
    let updates = model.pluginStore.packs.filter { $0.update }.count
    if updates > 0 { return updates == 1 ? "1 update" : "\(updates) updates" }
    return "Game profiles and more"
  }
}

/// A plugin's icon: an SF Symbol on a rounded square, tinted when it's on.
struct PluginIcon: View {
  let symbol: String
  let on: Bool
  var size: CGFloat = 28

  var body: some View {
    Image(systemName: symbol)
      .font(.system(size: size * 0.5, weight: .medium))
      .foregroundStyle(on ? Color.white : Color.secondary)
      .frame(width: size, height: size)
      .background(RoundedRectangle(cornerRadius: size * 0.24)
        .fill(on ? AnyShapeStyle(Color.accentColor.gradient) : AnyShapeStyle(.quaternary)))
  }
}

/// On, Off, or what it's waiting for.
struct PluginStatus: View {
  let plugin: Plugin

  var body: some View {
    HStack(spacing: 4) {
      Circle().fill(color).frame(width: 7, height: 7)
      Text(plugin.status == "on" && plugin.statusText == "On" ? "On" : plugin.statusText)
        .lineLimit(1)
    }
    .font(.caption)
    .foregroundStyle(plugin.blocked ? Color.orange : Color.secondary)
  }

  private var color: Color {
    if !plugin.enabled { return Color.secondary.opacity(0.5) }
    return plugin.blocked ? .orange : .green
  }
}

private struct PluginRow: View {
  let plugin: Plugin

  var body: some View {
    HStack(alignment: .top, spacing: 10) {
      PluginIcon(symbol: plugin.icon, on: plugin.enabled)
      VStack(alignment: .leading, spacing: 2) {
        Text(plugin.name).font(.body.weight(.medium))
        Text(plugin.summary).font(.caption).foregroundStyle(.secondary).lineLimit(2)
        PluginStatus(plugin: plugin)
      }
    }
    .padding(.vertical, 3)
  }
}

/// One plugin's page.
struct PluginDetail: View {
  let plugin: Plugin
  @ObservedObject var model: WarpModel
  let showStore: () -> Void
  @Environment(\.openWindow) private var openWindow
  @Environment(\.openURL) private var openURL
  @State private var error: String?

  var body: some View {
    Form {
      Section {
        HStack(alignment: .center, spacing: 14) {
          PluginIcon(symbol: plugin.icon, on: plugin.enabled, size: 52)
          VStack(alignment: .leading, spacing: 3) {
            HStack(spacing: 8) {
              Text(plugin.name).font(.title2.weight(.semibold))
              if plugin.terminalOnly { Tag("Terminal only") }
            }
            Text(plugin.summary).foregroundStyle(.secondary)
              .fixedSize(horizontal: false, vertical: true)
          }
          Spacer(minLength: 12)
          Toggle("On", isOn: Binding(get: { plugin.enabled },
                                     set: { error = model.setPluginEnabled(plugin.id, $0) }))
            .toggleStyle(.switch)
            .labelsHidden()
            .help(plugin.enabled ? "Turn \(plugin.name) off" : "Turn \(plugin.name) on")
        }
        .padding(.vertical, 4)
        if plugin.blocked {
          Label(plugin.statusText, systemImage: "exclamationmark.triangle.fill")
            .foregroundStyle(.orange)
        }
        if let error {
          Label(error, systemImage: "exclamationmark.triangle.fill").foregroundStyle(.orange)
        }
      } footer: {
        Text(plugin.enabled ? "Turning it off: \(plugin.offNote)" : "Off. \(plugin.offNote)")
          .font(.caption).foregroundStyle(.secondary)
      }

      Section("What it does") {
        Text(plugin.what).fixedSize(horizontal: false, vertical: true)
      }
      Section("How it works") {
        Text(plugin.how).fixedSize(horizontal: false, vertical: true)
      }

      PluginSettings(plugin: plugin, model: model, error: $error)

      if !plugin.needs.isEmpty {
        Section("What it needs") {
          ForEach(plugin.needs, id: \.self) { n in
            Label {
              Text(n.text)
            } icon: {
              Image(systemName: n.met == true ? "checkmark.circle.fill"
                                : n.met == false ? "xmark.circle.fill" : "questionmark.circle")
                .foregroundStyle(n.met == true ? Color.green : n.met == false ? Color.orange : Color.secondary)
            }
            .help(n.met == nil ? "WarpPoint can't check this one itself." : "")
          }
        }
      }

      Section("Games") {
        if plugin.anyGame {
          Text("Any game.").foregroundStyle(.secondary)
        } else if plugin.games.isEmpty {
          Text("No game has a profile with what \(plugin.name) needs yet.")
            .foregroundStyle(.secondary)
          Button("Get More…", action: showStore)
        } else {
          ForEach(plugin.games) { g in
            LabeledContent {
              Text(g.sources.joined(separator: ", ")).foregroundStyle(.secondary)
            } label: {
              VStack(alignment: .leading, spacing: 1) {
                Text(g.name.isEmpty ? g.titleId.uppercased() : g.name)
                Text("\(g.titleId.uppercased()) · build \(g.builds.joined(separator: ", "))")
                  .font(.caption.monospaced()).foregroundStyle(.secondary)
              }
            }
          }
          Button("Get More…", action: showStore)
        }
      }

      if !plugin.commands.isEmpty || plugin.docsURL != nil {
        Section("In a terminal") {
          ForEach(plugin.commands, id: \.self) { c in
            Text(c).font(.callout.monospaced()).textSelection(.enabled)
          }
          if let url = plugin.docsURL {
            Button("Read \(URL(fileURLWithPath: plugin.docs).lastPathComponent)") { openURL(url) }
              .buttonStyle(.link)
          }
        }
      }
    }
    .formStyle(.grouped)
  }
}

/// A small grey capsule: "Terminal only", "Tap profile".
struct Tag: View {
  let text: String
  init(_ text: String) { self.text = text }
  var body: some View {
    Text(text)
      .font(.caption.weight(.medium))
      .padding(.horizontal, 6).padding(.vertical, 2)
      .background(Capsule().fill(.quaternary))
      .foregroundStyle(.secondary)
  }
}
