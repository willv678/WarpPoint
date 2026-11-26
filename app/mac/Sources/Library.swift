import AppKit
import Foundation

/// An nxnative port of a game: a Mac app with the game's save in its own folder.
struct NxApp: Codable, Hashable {
  let name: String
  let path: String
  let appTitleId: String
  let saveTitleId: String
  let saveIdSource: String  // "control data", "setting" or "app id"
  let savePath: String
  let cloudFolder: String  // nxnative's own iCloud sync; "" = off
  let upgradesSave: Bool  // a Switch 2 Edition playing the Switch 1 game's save
  let running: Bool
  let hasSave: Bool
}

/// One save in the library (a game, for the linked user).
struct Game: Codable, Identifiable, Hashable {
  let key: String
  let titleId: String
  let name: String
  let publisher: String
  let icon: String
  let onConsole: Bool
  let inEmulator: Bool
  let status: String  // conflict, synced, held, consoleOnly, emulatorOnly, problem, new, ignored
  let lastText: String
  let lastAt: String
  let ignored: Bool  // "Stop Syncing": WarpPoint leaves it alone
  var savePath: String = ""
  var backupsPath: String = ""
  /// Where on this Mac the save lives: "emulator" or "nxnative".
  let macSide: String
  /// That place's name: "Astris", or "Xenoblade Chronicles (nxnative)".
  let macName: String
  /// The game's nxnative app, when one is installed (even if Astris is chosen).
  let nxnative: NxApp?
  /// Astris can be chosen instead of the app (it's set up for this user).
  let canUseEmulator: Bool
  /// The app's saves go to the console (false: a Switch 2 Edition's, kept on the Mac).
  let sendToConsole: Bool?
  let emulatorHasSave: Bool
  var id: String { key }

  var isNative: Bool { macSide == "nxnative" }
  /// Short name of the Mac side for buttons: "Astris" or the app's name.
  var macShort: String { isNative ? (nxnative?.name ?? macName) : macName }
}

/// Everything the library window shows, as the engine reports it.
struct Library: Codable {
  var console: String = ""
  var consoleSeen: String = ""
  var emulator: String = "Astris"
  /// Everything on this Mac: "Astris", "Astris and nxnative", "nxnative".
  var mac: String = "Astris"
  var emulatorOpen: Bool = false
  /// The console's WarpPoint (1.3+) can make a save for a game it has none for.
  var consoleCanCreate: Bool = false
  var consolePresent: Bool = false
  var keepBackups: Int = 4
  var games: [Game] = []
}

/// A saved copy of a save, kept before WarpPoint overwrote it.
struct Backup: Codable, Identifiable, Hashable {
  let id: String
  let side: String  // "mac" (the emulator's copy) or "switch"
  let reason: String
  let created: String
  let files: UInt64
  let bytes: UInt64
  var note: String = ""     // Warp Chronicle: the player's label
  var pinned: Bool = false  // kept forever
}

enum WarpDates {
  /// The engine writes UTC ISO 8601 times, with or without milliseconds.
  static func parse(_ s: String) -> Date? {
    let f = ISO8601DateFormatter()
    f.formatOptions = [.withInternetDateTime, .withFractionalSeconds]
    if let d = f.date(from: s) { return d }
    f.formatOptions = [.withInternetDateTime]
    return f.date(from: s)
  }
}

/// Game icons are read from disk once and kept.
@MainActor
enum IconCache {
  private static var images: [String: NSImage] = [:]
  static func image(_ path: String) -> NSImage? {
    if path.isEmpty { return nil }
    if let hit = images[path] { return hit }
    let image = NSImage(contentsOfFile: path)
    images[path] = image
    return image
  }
}
