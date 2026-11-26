import AppKit
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
    var saved = ""
    var savedKey = ""  // `saved`, sortable ("202611252225"); "" when the game keeps no time
    var emblem = ""
    var accent = ""
    var tags: [String] = []
    var details: [Detail] = []
    var thumbnail: Thumbnail?
    var id: Int { index }

    struct Detail: Decodable, Hashable {
      var label = ""
      var value = ""
    }
    /// A picture in the save: a JPEG or PNG, or raw RGB, at `offset` in the file at `path`.
    struct Thumbnail: Decodable, Hashable {
      var path = ""
      var offset = 0
      var length = 0
      var format = ""
      var width = 0
      var height = 0
    }

    private enum CodingKeys: String, CodingKey {
      case index, slot, file, place, heading, player, playTime, playSeconds, saved, savedKey, emblem,
           accent,
           tags, details, thumbnail
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
      saved = (try? c.decode(String.self, forKey: .saved)) ?? ""
      savedKey = (try? c.decode(String.self, forKey: .savedKey)) ?? ""
      emblem = (try? c.decode(String.self, forKey: .emblem)) ?? ""
      accent = (try? c.decode(String.self, forKey: .accent)) ?? ""
      tags = (try? c.decode([String].self, forKey: .tags)) ?? []
      details = (try? c.decode([Detail].self, forKey: .details)) ?? []
      thumbnail = try? c.decode(Thumbnail.self, forKey: .thumbnail)
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

/// The details panel's "Save Slots" section: the newest save as a card (laid out like the
/// game's own slot list), and the rest behind "N more saves". GameDetail shows it only for
/// games with a preview parser.
struct SavePreviewSection: View {
  let preview: SavePreviewData
  let source: String  // where the save was read: "the Mac app's save", "Astris's save"
  let gameKey: String  // the game's title id, for SlotRecency

  @State private var ordered: [SavePreviewData.Slot] = []
  @State private var showAll = false

  var body: some View {
    VStack(alignment: .leading, spacing: 8) {
      Text("Save Slots").font(.headline)
      if preview.slots.isEmpty {
        Text(preview.error.isEmpty ? "No saves yet." : preview.error)
          .font(.callout).foregroundStyle(.secondary)
          .fixedSize(horizontal: false, vertical: true)
      } else {
        let slots = ordered.isEmpty ? preview.slots : ordered
        ForEach(showAll ? slots : Array(slots.prefix(1))) { SlotCard(slot: $0) }
        if slots.count > 1 {
          Button {
            withAnimation(.easeInOut(duration: 0.2)) { showAll.toggle() }
          } label: {
            Label(showAll ? "Show only the latest" : "\(slots.count - 1) more \(slots.count == 2 ? "save" : "saves")",
                  systemImage: showAll ? "chevron.up" : "chevron.down")
              .font(.callout)
          }
          .buttonStyle(.borderless)
        }
        Text("Read from \(source).")
          .font(.caption).foregroundStyle(.tertiary)
      }
    }
    .onAppear(perform: reorder)
    .onChange(of: preview.slots) { _, _ in reorder() }
    .onChange(of: gameKey) { _, _ in showAll = false }
  }

  private func reorder() {
    ordered = SlotRecency.newestFirst(preview.slots, game: gameKey)
  }
}

/// Which save slot is newest. Games that keep a save time (Zelda, Xenoblade) say so in
/// `savedKey`. For the rest (Fire Emblem keeps none, and a sync gives every file the same
/// date), the app remembers each slot's contents and when it last saw them change, so the
/// slot you saved in last comes first from the next change on. Until then: the game's order.
enum SlotRecency {
  private static func storeKey(_ game: String) -> String { "slotRecency.\(game)" }

  /// What a slot holds, as far as the card can tell; a new save in that slot changes it.
  private static func fingerprint(_ s: SavePreviewData.Slot) -> String {
    [s.slot, s.place, s.heading, String(s.playSeconds), s.saved, s.tags.joined(separator: ",")]
      .joined(separator: "|")
  }

  /// Records what changed since last time and returns the slots newest first; ties keep
  /// the game's own order.
  static func newestFirst(_ slots: [SavePreviewData.Slot], game: String) -> [SavePreviewData.Slot] {
    let defaults = UserDefaults.standard
    // index -> [fingerprint, seconds since 1970 it was seen to change (0 = not seen yet)]
    var seen = (defaults.dictionary(forKey: storeKey(game)) as? [String: [String]]) ?? [:]
    let now = Date().timeIntervalSince1970
    for s in slots {
      let key = String(s.index), fp = fingerprint(s)
      if let old = seen[key] {
        if old.first != fp { seen[key] = [fp, String(now)] }
      } else {
        seen[key] = [fp, "0"]  // first look: we don't know when it was saved
      }
    }
    if !game.isEmpty { defaults.set(seen, forKey: storeKey(game)) }

    let changed: (SavePreviewData.Slot) -> Double = {
      Double(seen[String($0.index)]?.last ?? "0") ?? 0
    }
    let position = Dictionary(uniqueKeysWithValues: slots.enumerated().map { ($1.index, $0) })
    return slots.sorted { a, b in
      if a.savedKey != b.savedKey, !a.savedKey.isEmpty || !b.savedKey.isEmpty {
        return a.savedKey > b.savedKey  // "" sorts last
      }
      let ca = changed(a), cb = changed(b)
      if ca != cb { return ca > cb }
      return position[a.index, default: 0] < position[b.index, default: 0]
    }
  }
}

/// One save slot as a card: the save's picture (or the crest), where you are, who and how long.
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

  private var picture: NSImage? { slot.thumbnail.flatMap(SlotPicture.image) }

  var body: some View {
    HStack(alignment: .top, spacing: 10) {
      if let picture {
        Image(nsImage: picture)
          .resizable()
          .interpolation(.high)
          .aspectRatio(contentMode: .fill)
          .frame(width: 96, height: 54)
          .clipShape(RoundedRectangle(cornerRadius: 5))
          .overlay(RoundedRectangle(cornerRadius: 5).strokeBorder(.separator.opacity(0.6)))
      } else {
        // The crest: a shield in the house colour, or a plain mark for games without one.
        ZStack {
          Circle().fill(tint.opacity(0.18))
          Image(systemName: slot.emblem.isEmpty ? "bookmark.fill" : "shield.lefthalf.filled")
            .foregroundStyle(tint)
            .font(.system(size: 15, weight: .semibold))
        }
        .frame(width: 30, height: 30)
        .help(slot.emblem)
      }

      VStack(alignment: .leading, spacing: 3) {
        HStack(spacing: 6) {
          Text(slot.slot.uppercased())
            .font(.caption2.weight(.semibold)).foregroundStyle(.secondary)
            .tracking(0.6)
            .lineLimit(1)
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
        ForEach(slot.details, id: \.self) { d in
          HStack(alignment: .firstTextBaseline, spacing: 5) {
            Text(d.label).foregroundStyle(.secondary)
            Text(d.value)
          }
          .font(.caption)
          .fixedSize(horizontal: false, vertical: true)
        }
        if !slot.player.isEmpty || !slot.emblem.isEmpty || !slot.saved.isEmpty {
          HStack(spacing: 10) {
            if !slot.player.isEmpty {
              Label(slot.player, systemImage: "person.fill")
            }
            if !slot.emblem.isEmpty {
              Text(slot.emblem).foregroundStyle(tint)
            }
            if !slot.saved.isEmpty {
              Label(slot.saved, systemImage: "calendar")
                .monospacedDigit()
                .help("Saved")
            }
          }
          .font(.caption).foregroundStyle(.secondary)
          .lineLimit(1)
        }
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

/// Reads a slot's picture from the save, as host/save_preview.hpp describes it, and keeps it
/// until the file changes.
enum SlotPicture {
  private static let cache = NSCache<NSString, NSImage>()

  static func image(_ t: SavePreviewData.Slot.Thumbnail) -> NSImage? {
    let url = URL(fileURLWithPath: t.path)
    let modified = (try? url.resourceValues(forKeys: [.contentModificationDateKey]))?
      .contentModificationDate?.timeIntervalSince1970 ?? 0
    let key = "\(t.path)#\(t.offset)#\(t.length)#\(modified)" as NSString
    if let hit = cache.object(forKey: key) { return hit }
    guard let handle = try? FileHandle(forReadingFrom: url) else { return nil }
    defer { try? handle.close() }
    guard (try? handle.seek(toOffset: UInt64(t.offset))) != nil else { return nil }
    let read = t.length > 0 ? try? handle.read(upToCount: t.length) : try? handle.readToEnd()
    guard let data = read ?? nil, !data.isEmpty else { return nil }

    let image: NSImage?
    if t.format == "rgb" {
      image = rgbImage(data, width: t.width, height: t.height)
    } else {
      image = NSImage(data: data)
    }
    if let image { cache.setObject(image, forKey: key) }
    return image
  }

  /// Raw 8-bit RGB, top row first.
  private static func rgbImage(_ data: Data, width: Int, height: Int) -> NSImage? {
    guard width > 0, height > 0, data.count >= width * height * 3 else { return nil }
    var rgba = [UInt8](repeating: 255, count: width * height * 4)
    data.withUnsafeBytes { (src: UnsafeRawBufferPointer) in
      for i in 0..<(width * height) {
        rgba[i * 4] = src[i * 3]
        rgba[i * 4 + 1] = src[i * 3 + 1]
        rgba[i * 4 + 2] = src[i * 3 + 2]
      }
    }
    guard let provider = CGDataProvider(data: Data(rgba) as CFData),
          let cg = CGImage(width: width, height: height, bitsPerComponent: 8, bitsPerPixel: 32,
                           bytesPerRow: width * 4, space: CGColorSpaceCreateDeviceRGB(),
                           bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.noneSkipLast.rawValue),
                           provider: provider, decode: nil, shouldInterpolate: true,
                           intent: .defaultIntent) else { return nil }
    return NSImage(cgImage: cg, size: NSSize(width: width, height: height))
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
