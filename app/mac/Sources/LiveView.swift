import SwiftUI

/// Warp Tap: the game running on the Switch, live (read-only), as its profile describes.
struct TapSnapshot: Decodable {
  struct Field: Decodable, Identifiable {
    let id: String
    let label: String
    let group: String
    let value: String?
    let note: String
    var lexicon: Bool = false

    private enum CodingKeys: String, CodingKey { case id, label, group, value, note, lexicon }
    init(from d: Decoder) throws {
      let c = try d.container(keyedBy: CodingKeys.self)
      id = try c.decode(String.self, forKey: .id)
      label = (try? c.decode(String.self, forKey: .label)) ?? id
      group = (try? c.decode(String.self, forKey: .group)) ?? ""
      value = try? c.decode(String.self, forKey: .value)
      note = (try? c.decode(String.self, forKey: .note)) ?? ""
      lexicon = (try? c.decode(Bool.self, forKey: .lexicon)) ?? false
    }
  }
  var game: String = ""
  var titleId: String = ""
  var buildId: String = ""
  var supported: Bool = false
  var status: String = ""
  var fields: [Field] = []

  private enum CodingKeys: String, CodingKey { case game, titleId, buildId, supported, status, fields }
  init() {}
  init(from d: Decoder) throws {
    let c = try d.container(keyedBy: CodingKeys.self)
    game = (try? c.decode(String.self, forKey: .game)) ?? ""
    titleId = (try? c.decode(String.self, forKey: .titleId)) ?? ""
    buildId = (try? c.decode(String.self, forKey: .buildId)) ?? ""
    supported = (try? c.decode(Bool.self, forKey: .supported)) ?? false
    status = (try? c.decode(String.self, forKey: .status)) ?? ""
    fields = (try? c.decode([Field].self, forKey: .fields)) ?? []
  }
}

struct LiveView: View {
  @ObservedObject var model: WarpModel
  @State private var snap = TapSnapshot()
  @State private var updated: Date?
  @State private var timer: Timer?

  private var groups: [String] {
    var seen: [String] = []
    for f in snap.fields where !seen.contains(f.group) { seen.append(f.group) }
    return seen
  }

  var body: some View {
    VStack(alignment: .leading, spacing: 16) {
      HStack(spacing: 12) {
        GameArt(path: model.nowPlayingIcon, size: 56)
        VStack(alignment: .leading, spacing: 2) {
          Text(snap.game.isEmpty ? (model.nowPlaying ?? "Nothing running") : snap.game)
            .font(.title2.weight(.semibold))
          Text(model.console.map { "Live from \($0)" } ?? "Switch not connected")
            .foregroundStyle(.secondary)
        }
        Spacer()
      }
      if !snap.status.isEmpty {
        Label(snap.status, systemImage: "info.circle").foregroundStyle(.secondary)
      }
      ForEach(groups, id: \.self) { group in
        GroupBox(group.isEmpty ? "Live" : group) {
          Grid(alignment: .leading, horizontalSpacing: 20, verticalSpacing: 8) {
            ForEach(snap.fields.filter { $0.group == group }) { f in
              GridRow {
                Text(f.label).foregroundStyle(.secondary)
                Text(f.value ?? f.note).font(.title3.monospacedDigit().weight(.medium))
                  .foregroundStyle(f.value == nil ? .tertiary : .primary)
              }
            }
          }
          .frame(maxWidth: .infinity, alignment: .leading)
          .padding(4)
        }
      }
      Spacer(minLength: 0)
      HStack {
        Text("Read-only: WarpPoint only looks at the game, it never changes it.")
        Spacer()
        if let updated { Text("Updated \(updated.formatted(date: .omitted, time: .standard))") }
      }
      .font(.caption).foregroundStyle(.tertiary)
    }
    .padding(20)
    .frame(minWidth: 420, minHeight: 300)
    .onAppear {
      model.windowOpened()
      poll()
      timer = Timer.scheduledTimer(withTimeInterval: 2, repeats: true) { _ in
        MainActor.assumeIsolated { poll() }
      }
    }
    .onDisappear {
      timer?.invalidate()
      timer = nil
      model.windowClosed()
    }
  }

  private func poll() {
    model.tapSnapshot { s in
      snap = s
      updated = Date()
    }
  }
}
