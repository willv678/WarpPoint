import AppKit
import Foundation

/// One save in the library (a game, for the linked user).
struct Game: Codable, Identifiable, Hashable {
  let key: String
  let titleId: String
  let name: String
  let publisher: String
  let icon: String
  let onConsole: Bool
  let inEmulator: Bool
  let status: String  // conflict, synced, consoleOnly, emulatorOnly, problem, new, ignored
  let lastText: String
  let lastAt: String
  let ignored: Bool  // "Stop Syncing": WarpPoint leaves it alone
  var savePath: String = ""
  var backupsPath: String = ""
  var id: String { key }
}

/// Everything the library window shows, as the engine reports it.
struct Library: Codable {
  var console: String = ""
  var consoleSeen: String = ""
  var emulator: String = "Astris"
  var emulatorOpen: Bool = false
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
