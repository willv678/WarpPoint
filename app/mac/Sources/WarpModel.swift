import AppKit
import Foundation
import ServiceManagement
import UserNotifications

/// A save that changed on both sides since they last agreed.
struct Conflict: Identifiable, Hashable {
  let device: String
  let key: String
  let name: String
  let icon: String
  var id: String { device + "|" + key }
}

/// One thing a sync did, for the "Recent" list.
struct RecentSync: Identifiable {
  let id = UUID()
  let name: String
  let text: String
  let icon: String
  let ok: Bool
  let date: Date
}

/// The app's view of the engine: owns the C++ core, polls its events on the main
/// thread and turns them into state the menu shows.
@MainActor
final class WarpModel: ObservableObject {
  @Published private(set) var console: String?  // the Switch, while its app is open
  @Published private(set) var localName = "Astris"
  @Published private(set) var syncing = false
  @Published private(set) var waitingForEmulator = false
  @Published private(set) var lastSync: Date?
  @Published private(set) var lastSummary = ""
  @Published private(set) var problem: String?
  @Published private(set) var recent: [RecentSync] = []
  @Published private(set) var conflicts: [Conflict] = []
  @Published private(set) var lift: CGFloat = 0  // the UFO's bob while syncing
  @Published private(set) var beam: CGFloat = 1  // the beam's flicker while syncing
  @Published private(set) var library = Library()
  @Published var libraryMessage: String?
  @Published private(set) var setup = Setup()
  @Published private(set) var reports: [CrashReport] = []
  @Published var newCrashes = 0  // since the Crash Reports window was last opened
  @Published private(set) var nowPlaying: String?  // the game running on the Switch
  @Published private(set) var nowPlayingIcon = ""
  @Published var openAtLogin = SMAppService.mainApp.status == .enabled {
    didSet { setOpenAtLogin(openAtLogin) }
  }

  private let core: OpaquePointer
  private var pump: Timer?
  private var pulse: Timer?
  private var pulseStart = Date()
  private var failuresInARow = 0
  private var libraryTimer: Timer?
  private var openWindows = 0

  /// Windows that show the library or setup call these from onAppear / onDisappear.
  func windowOpened() {
    openWindows += 1
    refreshLibrary()
    refreshSetup()
  }
  func windowClosed() { openWindows = max(0, openWindows - 1) }

  init() {
    let dir = ProcessInfo.processInfo.environment["WARP_DATA_DIR"]
      ?? FileManager.default.homeDirectoryForCurrentUser
        .appendingPathComponent("Library/Application Support/WarpPoint").path
    core = warp_core_create(dir)
    var error: UnsafeMutablePointer<CChar>?
    if !warp_core_start(core, &error) {
      problem = take(error) ?? "WarpPoint couldn't start"
    }
    refreshConflicts()
    // A few times a second is plenty for a status menu, and costs nothing.
    pump = Timer.scheduledTimer(withTimeInterval: 0.25, repeats: true) { [weak self] _ in
      MainActor.assumeIsolated { self?.drainEvents() }
    }
    // While a window that shows them is open, keep the library and setup fresh (is
    // Astris open, new saves). With every window closed the app only refreshes when a
    // sync event arrives, so it sits idle in the menu bar.
    libraryTimer = Timer.scheduledTimer(withTimeInterval: 3, repeats: true) { [weak self] _ in
      MainActor.assumeIsolated {
        guard let self, self.openWindows > 0 else { return }
        self.refreshLibrary()
        self.refreshSetup()
      }
    }
    refreshLibrary()
    refreshSetup()
  }

  var statusLine: String {
    if setup.needsSetup { return "Finish setting up WarpPoint to start syncing." }
    if syncing { return "Syncing with \(console ?? "your Switch")…" }
    guard let console else { return "Open WarpPoint on your Switch to sync." }
    if waitingForEmulator { return "\(console) is here. Syncs when you stop the game in \(localName)." }
    if let lastSync { return "\(console) is here. Last sync \(WarpModel.ago(lastSync))." }
    return "\(console) is here."
  }

  func syncNow() { warp_core_sync_now(core) }

  /// "just now", "5 minutes ago", "1 hour ago", "yesterday".
  static func ago(_ date: Date, now: Date = Date()) -> String {
    if now.timeIntervalSince(date) < 60 { return "just now" }
    let f = RelativeDateTimeFormatter()
    f.unitsStyle = .full
    f.dateTimeStyle = .named
    return f.localizedString(for: date, relativeTo: now)
  }

  /// keepLocal: the emulator's copy wins. Waits for any sync in progress, so it runs
  /// off the main thread.
  func resolve(_ c: Conflict, keepLocal: Bool) {
    let core = self.core
    conflicts.removeAll { $0 == c }
    Task.detached {
      var error: UnsafeMutablePointer<CChar>?
      let ok = warp_core_resolve(core, c.device, c.key, keepLocal, &error)
      let message = ok ? nil : WarpModel.takeString(error)
      await MainActor.run {
        if let message { self.problem = message }
        self.refreshConflicts()
      }
    }
  }

  // MARK: - Setup and Settings

  func refreshSetup() {
    guard let raw = take(warp_core_setup(core)), let data = raw.data(using: .utf8),
          let s = try? JSONDecoder().decode(Setup.self, from: data) else { return }
    setup = s
  }

  /// nil on success, else why not. These are quick (they only wait for a sync in
  /// progress, which is rare), so they run here and the answer shows at once.
  func setLink(_ consoleUser: String, _ profile: String) -> String? {
    defer { refreshSetup(); refreshLibrary() }
    return call { warp_core_set_link(core, consoleUser, profile, &$0) }
  }

  func setKeepBackups(_ keep: Int) -> String? {
    defer { refreshSetup(); refreshLibrary() }
    return call { warp_core_set_keep_backups(core, Int32(keep), &$0) }
  }

  func setEmulatorPath(_ path: String) -> String? {
    defer { refreshSetup(); refreshLibrary() }
    return call { warp_core_set_emulator_path(core, path, &$0) }
  }

  func setSplit(enabled: Bool, host: String, port: Int) -> String? {
    defer { refreshSetup() }
    return call { warp_core_set_split(core, enabled, host, Int32(port), &$0) }
  }

  func setAlbum(enabled: Bool, folder: String) -> String? {
    defer { refreshSetup() }
    return call { warp_core_set_album(core, enabled, folder, &$0) }
  }

  func setPresence(enabled: Bool, appId: String) -> String? {
    defer { refreshSetup() }
    return call { warp_core_set_presence(core, enabled, appId, &$0) }
  }

  func setPresenceMac(_ showMac: Bool) -> String? {
    defer { refreshSetup() }
    return call { warp_core_set_presence_mac(core, showMac, &$0) }
  }

  func setNxNativeUser(_ consoleUser: String) -> String? {
    defer { refreshSetup(); refreshLibrary() }
    return call { warp_core_set_nxnative_user(core, consoleUser, &$0) }
  }

  func setNxNativeEnabled(_ on: Bool) -> String? {
    defer { refreshSetup(); refreshLibrary() }
    return call { warp_core_set_nxnative_enabled(core, on, &$0) }
  }

  /// Sync this game with Astris instead of its nxnative app (or back).
  func setUseEmulator(_ game: Game, _ useEmulator: Bool) {
    let saveId = game.nxnative?.saveTitleId ?? game.titleId
    if let failure = call({ warp_core_set_keep_in_emulator(core, saveId, useEmulator, &$0) }) {
      libraryMessage = "Couldn't: \(failure)"
    } else {
      let place = useEmulator ? library.emulator : (game.nxnative?.name ?? "its nxnative app")
      libraryMessage = "\(game.name) now syncs with \(place)."
    }
    refreshLibrary()
  }

  /// Send a Switch 2 Edition app's saves to the console too (only a Switch 2 reads them).
  func setSendToConsole(_ game: Game, _ send: Bool) {
    let saveId = game.nxnative?.saveTitleId ?? game.titleId
    if let failure = call({ warp_core_set_send_upgraded(core, saveId, send, &$0) }) {
      libraryMessage = "Couldn't: \(failure)"
    }
    refreshLibrary()
  }

  private func call(_ body: (inout UnsafeMutablePointer<CChar>?) -> Bool) -> String? {
    var error: UnsafeMutablePointer<CChar>?
    return body(&error) ? nil : (take(error) ?? "something went wrong")
  }

  // MARK: - Library

  func refreshLibrary() {
    guard let raw = take(warp_core_games(core)), let data = raw.data(using: .utf8),
          let lib = try? JSONDecoder().decode(Library.self, from: data) else { return }
    library = lib
  }

  func backups(for key: String) -> [Backup] {
    guard let raw = take(warp_core_backups(core, key)), let data = raw.data(using: .utf8) else {
      return []
    }
    return (try? JSONDecoder().decode([Backup].self, from: data)) ?? []
  }

  /// Warp Chronicle: label a backup or pin it (kept forever).
  func labelBackup(_ game: Game, _ backup: Backup, note: String, pinned: Bool) -> String? {
    call { warp_core_label_backup(core, game.key, backup.id, note, pinned, &$0) }
  }

  func restore(_ game: Game, backup: Backup) {
    let core = self.core
    libraryMessage = "Restoring \(game.name)…"
    Task.detached {
      var error: UnsafeMutablePointer<CChar>?
      let ok = warp_core_restore(core, game.key, backup.id, &error)
      let message = ok ? "Restored. It goes to your Switch with the next sync."
                       : "Couldn't restore: \(WarpModel.takeString(error) ?? "unknown error")"
      await MainActor.run {
        self.libraryMessage = message
        self.refreshLibrary()
      }
    }
  }

  func setIgnored(_ game: Game, _ ignored: Bool) {
    let core = self.core
    Task.detached {
      var error: UnsafeMutablePointer<CChar>?
      let ok = warp_core_set_ignored(core, game.key, ignored, &error)
      let message = ok ? (ignored ? "Stopped syncing \(game.name)." : "Syncing \(game.name) again.")
                       : "Couldn't: \(WarpModel.takeString(error) ?? "unknown error")"
      await MainActor.run {
        self.libraryMessage = message
        self.refreshLibrary()
      }
    }
  }

  func bringToEmulator(_ game: Game) {
    var error: UnsafeMutablePointer<CChar>?
    if warp_core_bring_to_emulator(core, game.key, &error) {
      libraryMessage = "Bringing \(game.name) over…"
    } else {
      libraryMessage = "Couldn't: \(take(error) ?? "unknown error")"
    }
  }

  func showDataFolder() {
    if let dir = take(warp_core_data_dir(core)) {
      NSWorkspace.shared.open(URL(fileURLWithPath: dir))
    }
  }

  // MARK: - Warp Mods

  func mods(_ titleId: String) -> ModsState {
    guard let raw = take(warp_core_mods(core, titleId)), let data = raw.data(using: .utf8),
          let s = try? JSONDecoder().decode(ModsState.self, from: data) else { return ModsState() }
    return s
  }

  func addMod(_ titleId: String, folder: String, name: String) -> String? {
    call { warp_core_add_mod(core, titleId, folder, name, &$0) }
  }

  func removeMod(_ titleId: String, name: String) -> String? {
    call { warp_core_remove_mod(core, titleId, name, &$0) }
  }

  func setModEnabled(_ titleId: String, name: String, on: Bool) -> String? {
    call { warp_core_set_mod_enabled(core, titleId, name, on, &$0) }
  }

  // MARK: - Warp Tap

  private var tapBusy = false
  /// One look at the Switch's running game, off the main thread; skipped if one is in flight.
  func tapSnapshot(_ done: @escaping (TapSnapshot) -> Void) {
    guard !tapBusy else { return }
    tapBusy = true
    let core = self.core
    Task.detached {
      let raw = WarpModel.takeString(warp_core_tap(core)) ?? "{}"
      let snap = (try? JSONDecoder().decode(TapSnapshot.self, from: Data(raw.utf8))) ?? TapSnapshot()
      await MainActor.run {
        self.tapBusy = false
        done(snap)
      }
    }
  }

  // MARK: - Warp Lens

  func refreshReports() {
    guard let raw = take(warp_core_reports(core)), let data = raw.data(using: .utf8),
          let list = try? JSONDecoder().decode([CrashReport].self, from: data) else { return }
    reports = list
  }

  /// A notification for a crash on the console (asks for permission the first time).
  private func notifyCrash(_ title: String, _ body: String) {
    let center = UNUserNotificationCenter.current()
    center.requestAuthorization(options: [.alert, .sound]) { granted, _ in
      guard granted else { return }
      let content = UNMutableNotificationContent()
      content.title = title
      content.body = body
      center.add(UNNotificationRequest(identifier: UUID().uuidString, content: content, trigger: nil))
    }
  }

  // MARK: - Events

  private func drainEvents() {
    while let raw = warp_core_next_event(core) {
      let text = String(cString: raw)
      warp_free(raw)
      guard let data = text.data(using: .utf8),
            let e = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
            let kind = e["kind"] as? String else { continue }
      handle(kind, e)
    }
  }

  private func handle(_ kind: String, _ e: [String: Any]) {
    let s = { (k: String) in e[k] as? String ?? "" }
    switch kind {
    case "consoleFound":
      console = s("console")
      problem = nil
      refreshLibrary()
      refreshSetup()
    case "consoleLost":
      console = nil
      nowPlaying = nil
      refreshLibrary()
      setSyncing(false)
      waitingForEmulator = false
    case "waiting":
      waitingForEmulator = true
    case "mods":
      recent.insert(RecentSync(name: s("name"), text: s("message"), icon: s("icon"),
                               ok: !s("message").contains("couldn't") && !s("message").contains("didn't"),
                               date: Date()), at: 0)
      recent = Array(recent.prefix(6))
    case "album":
      recent.insert(RecentSync(name: s("name").isEmpty ? "Switch album" : s("name"),
                               text: "copied \(s("message").components(separatedBy: " from ").first ?? s("message")) to this Mac",
                               icon: s("icon"), ok: true, date: Date()), at: 0)
      recent = Array(recent.prefix(6))
    case "playing":
      nowPlaying = s("name").isEmpty ? nil : s("name")
      nowPlayingIcon = s("icon")
      if !s("presence").isEmpty { setup.presence.status = s("presence") }
    case "presence":
      if !s("presence").isEmpty { setup.presence.status = s("presence") }
    case "crash":
      let name = s("name").isEmpty ? "Something" : s("name")
      recent.insert(RecentSync(name: name, text: "crashed on \(s("console"))", icon: s("icon"),
                               ok: false, date: Date()), at: 0)
      recent = Array(recent.prefix(6))
      newCrashes += 1
      refreshReports()
      notifyCrash("\(name) crashed on \(s("console"))", s("message"))
    case "passStarted":
      if !s("local").isEmpty { localName = s("local") }
      waitingForEmulator = false
      setSyncing(true)
    case "synced":
      let action = s("action")
      let ok = e["ok"] as? Bool ?? false
      let skipped = e["skipped"] as? Bool ?? false
      // Only in Astris, or a Switch 2 Edition's save kept on the Mac: nothing to report.
      if e["quiet"] as? Bool ?? false || e["held"] as? Bool ?? false { break }
      // Only what changed or went wrong is worth showing; "in sync" is the quiet norm.
      if (!ok || skipped), libraryMessage?.hasPrefix("Bringing") == true {
        libraryMessage = "\(s("name")): \(s("message"))"
      }
      if action == "push" || action == "pull" || !ok || skipped {
        let why = s("message")
        let text = skipped || !ok ? (why.isEmpty ? s("text") : "\(s("text")): \(why)") : s("text")
        recent.insert(RecentSync(name: s("name"), text: text, icon: s("icon"), ok: ok && !skipped,
                                 date: Date()), at: 0)
        recent = Array(recent.prefix(6))
      }
    case "passFinished":
      setSyncing(false)
      failuresInARow = 0
      lastSync = Date()
      lastSummary = s("message")
      problem = nil
      refreshConflicts()
      refreshLibrary()
      refreshSetup()
      if libraryMessage?.hasPrefix("Bringing") == true { libraryMessage = nil }
    case "passFailed":
      setSyncing(false)
      failuresInARow += 1
      // The first attempt can fail for passing reasons (the Switch app still starting,
      // macOS asking about local network access); AutoSync retries in 2 s.
      if failuresInARow >= 2 { problem = s("message") }
    default:
      break
    }
  }

  private func refreshConflicts() {
    guard let raw = take(warp_core_status(core)),
          let data = raw.data(using: .utf8),
          let j = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
          let list = j["conflicts"] as? [[String: Any]] else { return }
    conflicts = list.map {
      Conflict(device: $0["device"] as? String ?? "", key: $0["key"] as? String ?? "",
               name: $0["name"] as? String ?? "", icon: $0["icon"] as? String ?? "")
    }
  }

  // MARK: - The UFO moves while syncing (like the icon on the portfolio site)

  private func setSyncing(_ on: Bool) {
    syncing = on
    pulse?.invalidate()
    pulse = nil
    lift = 0
    beam = 1
    guard on else { return }
    pulseStart = Date()
    pulse = Timer.scheduledTimer(withTimeInterval: 1.0 / 30, repeats: true) { [weak self] _ in
      MainActor.assumeIsolated {
        guard let self else { return }
        let t = Date().timeIntervalSince(self.pulseStart)
        // Bob 0.7 units every 1.3 s; the beam switches on over 0.35 s, then flickers
        // between full and 55% every 1.6 s.
        self.lift = CGFloat(0.35 * (1 - cos(t * 2 * .pi / 1.3)))
        let on = min(1, t / 0.35)
        let flicker = t < 0.35 ? 1 : 0.775 + 0.225 * cos((t - 0.35) * 2 * .pi / 1.6)
        self.beam = CGFloat(on * flicker)
      }
    }
  }

  // MARK: - Helpers

  private func setOpenAtLogin(_ on: Bool) {
    do {
      if on { try SMAppService.mainApp.register() } else { try SMAppService.mainApp.unregister() }
    } catch {
      problem = "Couldn't change Open at Login: \(error.localizedDescription)"
    }
  }

  private func take(_ p: UnsafeMutablePointer<CChar>?) -> String? { WarpModel.takeString(p) }

  /// A string from the engine, freed after copying.
  nonisolated static func takeString(_ p: UnsafeMutablePointer<CChar>?) -> String? {
    guard let p else { return nil }
    defer { warp_free(p) }
    return String(cString: p)
  }
}
