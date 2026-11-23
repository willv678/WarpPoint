import Foundation

/// What setup and Settings show, as the engine reports it.
struct Setup: Codable {
  struct Emulator: Codable {
    var name: String = "Astris"
    var path: String = ""
    var found: Bool = false
  }
  struct ConsoleUser: Codable, Identifiable, Hashable {
    let id: String
    let name: String
    let saves: Int
    /// The nickname from the console, or a short form of the id if it's unknown.
    var label: String { name.isEmpty ? "User \(id.prefix(6))…" : name }
  }
  struct Profile: Codable, Identifiable, Hashable {
    let id: String
    let name: String
  }

  var emulator = Emulator()
  var console: String = ""
  var consolePresent: Bool = false
  var consoleUsers: [ConsoleUser] = []
  var emulatorProfiles: [Profile] = []
  var links: [String: String] = [:]  // console user -> emulator profile
  var keepBackups: Int = 4
  var needsSetup: Bool = false

  /// Console users with the most saves first: the main account is almost always on top.
  var usersByActivity: [ConsoleUser] {
    consoleUsers.sorted { $0.saves != $1.saves ? $0.saves > $1.saves : $0.label < $1.label }
  }
}
