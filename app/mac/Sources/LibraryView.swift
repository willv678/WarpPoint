import AppKit
import SwiftUI

/// "Open Library…": the games WarpPoint keeps in sync, with details and backups.
/// Games that are only on the Switch live behind "Add Game", not on the main page.
struct LibraryView: View {
  @ObservedObject var model: WarpModel
  @State private var search = ""
  @State private var selection: String?
  @State private var adding = false

  /// The games being synced: everything in the emulator, plus anything in conflict.
  private var myGames: [Game] {
    model.library.games
      .filter { $0.inEmulator || $0.status == "conflict" }
      .filter { search.isEmpty || $0.name.localizedCaseInsensitiveContains(search) }
      .sorted { a, b in
        // Conflicts and problems first, then synced games, then emulator-only; by name.
        let rank = { (g: Game) -> Int in
          switch g.status {
          case "conflict", "problem": return 0
          case "emulatorOnly": return 2
          default: return 1
          }
        }
        if rank(a) != rank(b) { return rank(a) < rank(b) }
        return a.name.localizedCaseInsensitiveCompare(b.name) == .orderedAscending
      }
  }

  private var addable: [Game] { model.library.games.filter { $0.status == "consoleOnly" } }
  private var conflicts: [Game] { model.library.games.filter { $0.status == "conflict" } }
  private var selected: Game? { model.library.games.first { $0.key == selection } }

  var body: some View {
    VStack(spacing: 0) {
      banner
      ForEach(conflicts) { conflictBanner($0) }
      HStack(spacing: 0) {
        Group {
          if myGames.isEmpty && search.isEmpty {
            emptyState
          } else {
            grid
          }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        // The details slide in below the toolbar (not up beside the search field).
        if let game = selected {
          Divider()
          GameDetail(game: game, model: model)
            .frame(width: 330)
            .frame(maxHeight: .infinity)
            .background(Color(nsColor: .windowBackgroundColor))
            .transition(.move(edge: .trailing).combined(with: .opacity))
        }
      }
      .animation(.snappy(duration: 0.22), value: selection)
    }
    .onExitCommand { selection = nil }  // Esc closes the details
    .frame(minWidth: 620, minHeight: 420)
    .searchable(text: $search, placement: .toolbar, prompt: "Search your games")
    .toolbar {
      ToolbarItem(placement: .primaryAction) {
        Button {
          adding = true
        } label: {
          Label("Add Game", systemImage: "plus")
        }
        .help("Bring a game's save over from your Switch")
      }
    }
    .sheet(isPresented: $adding) {
      AddGameSheet(model: model, games: addable) { game in
        adding = false
        model.bringToEmulator(game)
      }
    }
    .onAppear { model.refreshLibrary() }
  }

  private var grid: some View {
    GeometryReader { geo in
      ScrollView {
        LazyVGrid(columns: [GridItem(.adaptive(minimum: 118, maximum: 140), spacing: 20)],
                  spacing: 22) {
          ForEach(myGames) { game in
            GameTile(game: game, selected: game.key == selection, model: model)
              .onTapGesture { selection = game.key }
          }
        }
        .padding(20)
        // Fill the view so a click on empty space reaches us: it closes the details.
        .frame(maxWidth: .infinity, minHeight: geo.size.height, alignment: .topLeading)
        .contentShape(Rectangle())
        .onTapGesture { selection = nil }
      }
    }
  }

  /// Where things stand: the Switch, the emulator, anything in progress.
  private var banner: some View {
    let lib = model.library
    let console = lib.console.isEmpty ? "Your Switch" : lib.console
    let seen: String = {
      if lib.consolePresent { return "\(console) is connected" }
      if let d = WarpDates.parse(lib.consoleSeen) { return "\(console) last seen \(WarpModel.ago(d))" }
      return "Open WarpPoint on your Switch to see its games"
    }()
    return HStack(spacing: 8) {
      Circle().fill(lib.consolePresent ? Color.green : Color.secondary.opacity(0.5))
        .frame(width: 8, height: 8)
      Text(seen)
      Text("·").foregroundStyle(.tertiary)
      Text("\(myGames.count) game\(myGames.count == 1 ? "" : "s") in sync with \(lib.emulator)")
      Spacer()
      if let message = model.libraryMessage {
        Text(message).foregroundStyle(.secondary).lineLimit(1)
      }
    }
    .font(.callout)
    .padding(.horizontal, 20)
    .padding(.vertical, 10)
    .background(.bar)
  }

  /// A save changed on both sides: say so up top, one click from the Keep buttons.
  private func conflictBanner(_ game: Game) -> some View {
    HStack(spacing: 10) {
      Image(systemName: "exclamationmark.triangle.fill").foregroundStyle(.orange)
      Text("**\(game.name)** changed on both sides. Choose which copy to keep.")
      Spacer()
      Button("Choose…") { selection = game.key }
    }
    .font(.callout)
    .padding(.horizontal, 20)
    .padding(.vertical, 9)
    .background(Color.orange.opacity(0.12))
  }

  private var emptyState: some View {
    VStack(spacing: 12) {
      Image(nsImage: UFOIcon.image(size: 64)).renderingMode(.template).foregroundStyle(.secondary)
      Text("No games yet").font(.title3.weight(.semibold))
      Text("Click + to bring a game's save over from your Switch. WarpPoint keeps it in sync from then on.")
        .multilineTextAlignment(.center)
        .foregroundStyle(.secondary)
        .frame(maxWidth: 360)
      Button("Add Game…") { adding = true }
    }
    .frame(maxWidth: .infinity, maxHeight: .infinity)
  }
}

/// "Add Game": the Switch's games that aren't in the emulator yet. Pick one and its
/// save is copied over (that creates it in the emulator), then kept in sync.
struct AddGameSheet: View {
  @ObservedObject var model: WarpModel
  let games: [Game]
  let bring: (Game) -> Void
  @Environment(\.dismiss) private var dismiss
  @State private var search = ""
  @State private var picked: String?

  private var shown: [Game] {
    games
      .filter { search.isEmpty || $0.name.localizedCaseInsensitiveContains(search) }
      .sorted { $0.name.localizedCaseInsensitiveCompare($1.name) == .orderedAscending }
  }
  private var console: String { model.library.console.isEmpty ? "your Switch" : model.library.console }
  private var emulator: String { model.library.emulator }

  /// Why "Bring" can't run right now, if it can't.
  private var blocker: String? {
    if !model.library.consolePresent { return "Open WarpPoint on your Switch first." }
    if model.library.emulatorOpen {
      return "Quit \(emulator) first: it keeps its list of saves in memory while it's open."
    }
    return nil
  }

  var body: some View {
    VStack(alignment: .leading, spacing: 14) {
      VStack(alignment: .leading, spacing: 4) {
        Text("Add a game from \(console)").font(.title2.weight(.semibold))
        Text("WarpPoint copies the game's save into \(emulator), then keeps the two in sync.")
          .foregroundStyle(.secondary)
      }
      TextField("Search \(games.count) games", text: $search)
        .textFieldStyle(.roundedBorder)
      if games.isEmpty {
        Text(model.library.consoleSeen.isEmpty
             ? "Open WarpPoint on your Switch so WarpPoint can see its games."
             : "Every game on \(console) is already here.")
          .foregroundStyle(.secondary)
          .frame(maxWidth: .infinity, maxHeight: .infinity)
      } else {
        ScrollView {
          LazyVGrid(columns: [GridItem(.adaptive(minimum: 96, maximum: 112), spacing: 16)],
                    spacing: 16) {
            ForEach(shown) { game in
              VStack(spacing: 6) {
                GameArt(path: game.icon, size: 88)
                  .overlay(RoundedRectangle(cornerRadius: 12)
                    .strokeBorder(game.key == picked ? Color.accentColor : .clear, lineWidth: 3))
                Text(game.name).font(.caption).multilineTextAlignment(.center).lineLimit(2)
                  .frame(height: 30, alignment: .top)
              }
              .contentShape(Rectangle())
              .onTapGesture { picked = game.key }
              .onTapGesture(count: 2) {
                picked = game.key
                if blocker == nil { bring(game) }
              }
            }
          }
          .padding(4)
        }
      }
      HStack {
        if let blocker {
          Label(blocker, systemImage: "info.circle").font(.callout).foregroundStyle(.secondary)
        }
        Spacer()
        Button("Cancel") { dismiss() }
          .keyboardShortcut(.cancelAction)
        Button("Bring to \(emulator)") {
          if let g = games.first(where: { $0.key == picked }) { bring(g) }
        }
        .keyboardShortcut(.defaultAction)
        .disabled(picked == nil || blocker != nil)
      }
    }
    .padding(20)
    .frame(width: 660, height: 560)
  }
}

/// A game's icon with its name and a small status badge.
private struct GameTile: View {
  let game: Game
  let selected: Bool
  @ObservedObject var model: WarpModel

  var body: some View {
    VStack(spacing: 7) {
      ZStack(alignment: .bottomTrailing) {
        GameArt(path: game.icon, size: 112)
          .overlay(RoundedRectangle(cornerRadius: 16)
            .strokeBorder(selected ? Color.accentColor : .clear, lineWidth: 3))
          .shadow(color: .black.opacity(0.18), radius: 4, y: 2)
        StatusBadge(status: game.status)
          .offset(x: 6, y: 6)
      }
      Text(game.name)
        .font(.callout.weight(.medium))
        .multilineTextAlignment(.center)
        .lineLimit(2)
        .frame(height: 34, alignment: .top)
    }
    .contentShape(Rectangle())
    .help(StatusBadge.text(game.status, model: model))
    .contextMenu {
      Button("Sync Now") { model.syncNow() }
        .disabled(model.console == nil)
      Divider()
      Button("Show Save in Finder") { reveal(game.savePath) }
        .disabled(game.savePath.isEmpty)
      Button("Show Backups in Finder") { reveal(game.backupsPath) }
        .disabled(!FileManager.default.fileExists(atPath: game.backupsPath))
    }
  }

  private func reveal(_ path: String) {
    NSWorkspace.shared.open(URL(fileURLWithPath: path))
  }
}

struct GameArt: View {
  let path: String
  let size: CGFloat
  var body: some View {
    Group {
      if let image = IconCache.image(path) {
        Image(nsImage: image).resizable().interpolation(.high)
      } else {
        ZStack {
          RoundedRectangle(cornerRadius: size * 0.14).fill(.quaternary)
          Image(systemName: "gamecontroller").font(.system(size: size * 0.3))
            .foregroundStyle(.tertiary)
        }
      }
    }
    .frame(width: size, height: size)
    .clipShape(RoundedRectangle(cornerRadius: size * 0.14))
  }
}

struct StatusBadge: View {
  let status: String

  var body: some View {
    let (symbol, color) = StatusBadge.look(status)
    Image(systemName: symbol)
      .font(.system(size: 15, weight: .semibold))
      .foregroundStyle(.white, color)
      .background(Circle().fill(.background).padding(-2))
  }

  static func look(_ status: String) -> (String, Color) {
    switch status {
    case "synced": return ("checkmark.circle.fill", .green)
    case "consoleOnly": return ("arrow.down.circle.fill", .blue)
    case "emulatorOnly": return ("laptopcomputer.circle.fill", .gray)
    case "conflict": return ("exclamationmark.circle.fill", .orange)
    case "problem": return ("xmark.circle.fill", .red)
    default: return ("circle.dotted.circle.fill", .gray)
    }
  }

  @MainActor
  static func text(_ status: String, model: WarpModel) -> String {
    let console = model.library.console.isEmpty ? "the Switch" : model.library.console
    switch status {
    case "synced": return "In sync"
    case "consoleOnly": return "Only on \(console)"
    case "emulatorOnly": return "Only in \(model.library.emulator)"
    case "conflict": return "Changed on both sides: pick one"
    case "problem": return "Didn't sync last time"
    default: return "Not synced yet"
    }
  }
}
