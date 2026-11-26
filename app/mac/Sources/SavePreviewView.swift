import SwiftUI

/// What a game's save slots hold, read from its save on the Mac (host/save_preview.cpp).
struct SavePreviewData: Decodable {
  struct Slot: Decodable, Identifiable, Hashable {
    var index = 0
    var slot = ""
    var file = ""
    var place = ""
    var heading = ""
    var player = ""
    var playTime = ""
    var playSeconds = 0
    var emblem = ""
    var accent = ""
    var tags: [String] = []
    var id: Int { index }

    private enum CodingKeys: String, CodingKey {
      case index, slot, file, place, heading, player, playTime, playSeconds, emblem, accent, tags
    }
    init(from d: Decoder) throws {
      let c = try d.container(keyedBy: CodingKeys.self)
      index = (try? c.decode(Int.self, forKey: .index)) ?? 0
      slot = (try? c.decode(String.self, forKey: .slot)) ?? ""
      file = (try? c.decode(String.self, forKey: .file)) ?? ""
      place = (try? c.decode(String.self, forKey: .place)) ?? ""
      heading = (try? c.decode(String.self, forKey: .heading)) ?? ""
      player = (try? c.decode(String.self, forKey: .player)) ?? ""
      playTime = (try? c.decode(String.self, forKey: .playTime)) ?? ""
      playSeconds = (try? c.decode(Int.self, forKey: .playSeconds)) ?? 0
      emblem = (try? c.decode(String.self, forKey: .emblem)) ?? ""
      accent = (try? c.decode(String.self, forKey: .accent)) ?? ""
      tags = (try? c.decode([String].self, forKey: .tags)) ?? []
    }
  }
  var supported = false
  var error = ""
  var game = ""
  var slots: [Slot] = []

  private enum CodingKeys: String, CodingKey { case supported, error, game, slots }
  init() {}
  init(from d: Decoder) throws {
    let c = try d.container(keyedBy: CodingKeys.self)
    supported = (try? c.decode(Bool.self, forKey: .supported)) ?? false
    error = (try? c.decode(String.self, forKey: .error)) ?? ""
    game = (try? c.decode(String.self, forKey: .game)) ?? ""
    slots = (try? c.decode([Slot].self, forKey: .slots)) ?? []
  }
}

/// The details panel's "Save Slots" section: one card per save slot, laid out like the
/// game's own slot list. GameDetail shows it only for games with a preview parser.
struct SavePreviewSection: View {
  let preview: SavePreviewData
  let source: String  // where the save was read: "the Mac app's save", "Astris's save"

  var body: some View {
    VStack(alignment: .leading, spacing: 8) {
      Text("Save Slots").font(.headline)
      if preview.slots.isEmpty {
        Text(preview.error.isEmpty ? "No saves yet." : preview.error)
          .font(.callout).foregroundStyle(.secondary)
          .fixedSize(horizontal: false, vertical: true)
      } else {
        ForEach(preview.slots) { SlotCard(slot: $0) }
        Text("Read from \(source).")
          .font(.caption).foregroundStyle(.tertiary)
      }
    }
  }
}

/// One save slot as a card: crest colour down the side, where you are, who and how long.
struct SlotCard: View {
  let slot: SavePreviewData.Slot

  private var tint: Color {
    switch slot.accent {
    case "red": return .red
    case "blue": return .blue
    case "yellow": return .yellow
    case "purple": return .purple
    case "green": return .green
    case "gray": return .gray
    default: return .secondary
    }
  }

  var body: some View {
    HStack(alignment: .top, spacing: 10) {
      // The crest: a shield in the house colour.
      ZStack {
        Circle().fill(tint.opacity(0.18))
        Image(systemName: "shield.lefthalf.filled").foregroundStyle(tint)
          .font(.system(size: 15, weight: .semibold))
      }
      .frame(width: 30, height: 30)
      .help(slot.emblem)

      VStack(alignment: .leading, spacing: 3) {
        HStack(spacing: 6) {
          Text(slot.slot.uppercased())
            .font(.caption2.weight(.semibold)).foregroundStyle(.secondary)
            .tracking(0.6)
          ForEach(slot.tags, id: \.self) { tag in
            Text(tag)
              .font(.caption2.weight(.medium))
              .padding(.horizontal, 5).padding(.vertical, 1)
              .background(tint.opacity(0.18), in: Capsule())
          }
          Spacer(minLength: 0)
          if !slot.playTime.isEmpty {
            Label(slot.playTime, systemImage: "clock")
              .labelStyle(.titleAndIcon)
              .font(.caption.monospacedDigit()).foregroundStyle(.secondary)
              .help("Play time (hours:minutes)")
          }
        }
        if !slot.place.isEmpty {
          Text(slot.place).font(.caption).foregroundStyle(.secondary)
            .fixedSize(horizontal: false, vertical: true)
        }
        Text(slot.heading).font(.callout.weight(.semibold))
          .fixedSize(horizontal: false, vertical: true)
        HStack(spacing: 10) {
          if !slot.player.isEmpty {
            Label(slot.player, systemImage: "person.fill")
          }
          if !slot.emblem.isEmpty {
            Text(slot.emblem).foregroundStyle(tint)
          }
        }
        .font(.caption).foregroundStyle(.secondary)
        .lineLimit(1)
      }
    }
    .padding(10)
    .frame(maxWidth: .infinity, alignment: .leading)
    .background(.background.opacity(0.55), in: RoundedRectangle(cornerRadius: 9))
    .overlay(alignment: .leading) {
      UnevenRoundedRectangle(topLeadingRadius: 9, bottomLeadingRadius: 9)
        .fill(tint)
        .frame(width: 3)
    }
    .overlay(RoundedRectangle(cornerRadius: 9).strokeBorder(.separator.opacity(0.5)))
  }
}

/// The details panel's "Live" section: Tap's values while this game runs on the Switch.
/// Shown only when the Switch says this game is running and its profile reads it.
struct LiveSection: View {
  let game: Game
  @ObservedObject var model: WarpModel
  @State private var snap = TapSnapshot()
  @State private var timer: Timer?

  /// The Switch says this game is running (by name; Tap's answer confirms the title id).
  static func isRunning(_ game: Game, _ model: WarpModel) -> Bool {
    guard let now = model.nowPlaying else { return false }
    return now == game.name || now == game.nxnative?.name
  }
  private var matches: Bool { snap.titleId.lowercased() == game.titleId.lowercased() }

  var body: some View {
    VStack(alignment: .leading, spacing: 8) {
      HStack(spacing: 6) {
        Circle().fill(.green).frame(width: 7, height: 7)
        Text("Live").font(.headline)
        Text("on \(model.console ?? "the Switch")").font(.callout).foregroundStyle(.secondary)
      }
      if !matches {
        Text(snap.status.isEmpty ? "Reading the game…" : snap.status)
          .font(.caption).foregroundStyle(.secondary)
          .fixedSize(horizontal: false, vertical: true)
      } else {
        if !snap.status.isEmpty {
          Text(snap.status).font(.caption).foregroundStyle(.secondary)
            .fixedSize(horizontal: false, vertical: true)
        }
        Grid(alignment: .leading, horizontalSpacing: 12, verticalSpacing: 4) {
          ForEach(snap.fields.filter { !$0.lexicon }) { f in
            GridRow {
              Text(f.label).foregroundStyle(.secondary)
              Text(f.value ?? f.note)
                .monospacedDigit()
                .foregroundStyle(f.value == nil ? .tertiary : .primary)
                .lineLimit(1)
            }
          }
        }
        .font(.callout)
        if let line = snap.fields.first(where: { $0.lexicon })?.value, !line.isEmpty {
          Text(line).font(.callout).italic()
            .lineLimit(3)
            .foregroundStyle(.secondary)
        }
      }
    }
    .onAppear(perform: start)
    .onChange(of: game.key) {
      snap = TapSnapshot()
      start()
    }
    .onDisappear(perform: stop)
  }

  private func start() {
    stop()
    poll()
    timer = Timer.scheduledTimer(withTimeInterval: 2, repeats: true) { _ in
      MainActor.assumeIsolated { poll() }
    }
  }

  private func stop() {
    timer?.invalidate()
    timer = nil
  }

  private func poll() { model.tapSnapshot { snap = $0 } }
}
