import AppKit
import SwiftUI

/// Get More: packs from the WarpPoint repo on GitHub that extend the plugins.
struct PluginStoreView: View {
  @ObservedObject var model: WarpModel
  @Environment(\.openURL) private var openURL

  private var store: PluginStore { model.pluginStore }

  var body: some View {
    ScrollView {
      VStack(alignment: .leading, spacing: 16) {
        header
        if !store.online && !store.fetchError.isEmpty && !model.storeLoading { offline }
        if let message = model.storeMessage {
          Text(message).font(.callout).foregroundStyle(.secondary)
            .fixedSize(horizontal: false, vertical: true)
        }
        if store.packs.isEmpty && store.online {
          Text("No packs yet.").foregroundStyle(.secondary)
        }
        ForEach(store.packs) { pack in
          PackCard(pack: pack, busy: model.storeBusy.contains(pack.id),
                   install: { model.installPack(pack) },
                   remove: { model.removePack(id: pack.id, name: pack.name) })
        }
        if !store.orphans.isEmpty {
          Text("Installed, no longer listed").font(.headline).padding(.top, 6)
          ForEach(store.orphans) { p in
            HStack {
              VStack(alignment: .leading, spacing: 2) {
                Text(p.name).font(.body.weight(.medium))
                Text("Version \(p.version) · \(p.file)").font(.caption).foregroundStyle(.secondary)
              }
              Spacer()
              Button("Remove") { model.removePack(id: p.id, name: p.name) }
            }
            .padding(12)
            .background(RoundedRectangle(cornerRadius: 10).fill(.background.secondary))
          }
        }
        footer
      }
      .padding(20)
      .frame(maxWidth: 640, alignment: .leading)
      .frame(maxWidth: .infinity)
    }
    .onAppear { model.refreshStore() }
  }

  private var header: some View {
    HStack(alignment: .firstTextBaseline) {
      VStack(alignment: .leading, spacing: 3) {
        Text("Get More").font(.title2.weight(.semibold))
        Text(model.pluginOn("gamedata")
             ? "Packs that extend the plugins, from the WarpPoint repository on GitHub. Game Data installs and updates the ones for games in your library by itself; add others here."
             : "Packs that extend the plugins, such as Tap profiles for more games. They come from the WarpPoint repository on GitHub.")
          .foregroundStyle(.secondary)
          .fixedSize(horizontal: false, vertical: true)
      }
      Spacer()
      if model.storeLoading {
        ProgressView().controlSize(.small)
      } else {
        Button { model.refreshStore() } label: { Label("Refresh", systemImage: "arrow.clockwise") }
          .help("Check GitHub for new packs and updates")
      }
    }
  }

  private var offline: some View {
    HStack(alignment: .top, spacing: 10) {
      Image(systemName: "wifi.exclamationmark").font(.title3).foregroundStyle(.orange)
      VStack(alignment: .leading, spacing: 3) {
        Text(store.fetchError.hasPrefix("GitHub answered") ? "No plugin list from GitHub" : "Can't reach GitHub")
          .font(.body.weight(.semibold))
        Text(store.fetchError).foregroundStyle(.secondary)
          .fixedSize(horizontal: false, vertical: true)
        Text(store.fetchedDate.map { "Showing the list from \(WarpModel.ago($0)). Installed packs keep working offline." }
             ?? "Nothing has been downloaded yet. Installed packs keep working offline.")
          .font(.caption).foregroundStyle(.secondary)
      }
    }
    .padding(12)
    .frame(maxWidth: .infinity, alignment: .leading)
    .background(RoundedRectangle(cornerRadius: 10).fill(Color.orange.opacity(0.1)))
  }

  private var footer: some View {
    VStack(alignment: .leading, spacing: 4) {
      Text("Each download is checked against the checksum in the list before it's installed. No account and no tracking: WarpPoint only downloads from raw.githubusercontent.com, when this page opens or you press a button.")
        .foregroundStyle(.secondary)
      Button("How packs work, and how to publish one") {
        openURL(URL(string: "https://github.com/willv678/WarpPoint/blob/main/docs/PLUGINS.md")!)
      }
      .buttonStyle(.link)
    }
    .font(.caption)
    .fixedSize(horizontal: false, vertical: true)
    .padding(.top, 8)
  }
}

private struct PackCard: View {
  let pack: PluginStore.Pack
  let busy: Bool
  let install: () -> Void
  let remove: () -> Void

  /// "Gold, Renown · Level, HP · Line", grouped as the profile groups them.
  private var fieldsByGroup: [(String, [String])] {
    var order: [String] = []
    var map: [String: [String]] = [:]
    for f in pack.preview.fields {
      if map[f.group] == nil { order.append(f.group) }
      map[f.group, default: []].append(f.label)
    }
    return order.map { ($0, map[$0] ?? []) }
  }

  var body: some View {
    VStack(alignment: .leading, spacing: 10) {
      HStack(alignment: .top, spacing: 12) {
        PluginIcon(symbol: "gauge.with.dots.needle.33percent", on: pack.installed, size: 40)
        VStack(alignment: .leading, spacing: 3) {
          HStack(spacing: 6) {
            Text(pack.name).font(.headline)
            Tag(pack.kindName)
          }
          Text(subtitle).font(.caption).foregroundStyle(.secondary)
        }
        Spacer(minLength: 8)
        action
      }
      if !pack.description.isEmpty {
        Text(pack.description).fixedSize(horizontal: false, vertical: true)
      }
      if !pack.preview.fields.isEmpty {
        Grid(alignment: .leading, horizontalSpacing: 12, verticalSpacing: 4) {
          ForEach(fieldsByGroup, id: \.0) { group, labels in
            GridRow {
              Text(group.isEmpty ? "Values" : group).foregroundStyle(.secondary)
              Text(labels.joined(separator: ", "))
            }
          }
          GridRow {
            Text("Works with").foregroundStyle(.secondary)
            Text(worksWith)
          }
        }
        .font(.callout)
        .padding(10)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(RoundedRectangle(cornerRadius: 8).fill(.background.tertiary))
      }
    }
    .padding(14)
    .background(RoundedRectangle(cornerRadius: 12).fill(.background.secondary))
    .overlay(RoundedRectangle(cornerRadius: 12).strokeBorder(.separator.opacity(0.5)))
  }

  private var subtitle: String {
    var parts = ["Version \(pack.version)"]
    if pack.update && !pack.installedVersion.isEmpty { parts.append("you have \(pack.installedVersion)") }
    let builds = pack.games.flatMap { $0.builds }
    if !builds.isEmpty { parts.append("build \(builds.joined(separator: ", "))") }
    if !pack.author.isEmpty { parts.append("by \(pack.author)") }
    return parts.joined(separator: " · ")
  }

  private var worksWith: String {
    var list = ["Tap"]
    if pack.preview.lexicon { list.append("Lexicon") }
    if pack.preview.splits > 0 { list.append("Split (\(pack.preview.splits) rules)") }
    return list.joined(separator: ", ")
  }

  @ViewBuilder private var action: some View {
    if busy {
      ProgressView().controlSize(.small)
    } else if !pack.supported {
      Text("Needs a newer WarpPoint").font(.caption).foregroundStyle(.secondary)
    } else if pack.update {
      HStack {
        Button("Remove", action: remove)
        Button("Update", action: install).buttonStyle(.borderedProminent)
      }
    } else if pack.installed {
      HStack(spacing: 8) {
        Label("Installed", systemImage: "checkmark.circle.fill")
          .foregroundStyle(.green).font(.callout)
        Button("Remove", action: remove)
      }
    } else {
      Button("Install", action: install).buttonStyle(.borderedProminent)
    }
  }
}
