import AppKit
import Foundation
import ServiceManagement

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
  @Published private(set) var beam: CGFloat = 0
  @Published var openAtLogin = SMAppService.mainApp.status == .enabled {
    didSet { setOpenAtLogin(openAtLogin) }
  }

  private let core: OpaquePointer
  private var pump: Timer?
  private var pulse: Timer?
  private var pulseStart = Date()
  private var failuresInARow = 0

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
  }

  var statusLine: String {
    if syncing { return "Syncing with \(console ?? "your Switch")…" }
    guard let console else { return "Open WarpPoint on your Switch to sync." }
    if waitingForEmulator { return "\(console) is here. Syncs when you quit \(localName)." }
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

  func showDataFolder() {
    if let dir = take(warp_core_data_dir(core)) {
      NSWorkspace.shared.open(URL(fileURLWithPath: dir))
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
    case "consoleLost":
      console = nil
      setSyncing(false)
      waitingForEmulator = false
    case "waiting":
      waitingForEmulator = true
    case "passStarted":
      if !s("local").isEmpty { localName = s("local") }
      waitingForEmulator = false
      setSyncing(true)
    case "synced":
      let action = s("action")
      let ok = e["ok"] as? Bool ?? false
      let skipped = e["skipped"] as? Bool ?? false
      // Only what changed or went wrong is worth showing; "in sync" is the quiet norm.
      if action == "push" || action == "pull" || !ok || skipped {
        let text = skipped || !ok ? "\(s("text")): \(s("message"))" : s("text")
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

  // MARK: - The beam pulses while syncing

  private func setSyncing(_ on: Bool) {
    syncing = on
    pulse?.invalidate()
    pulse = nil
    beam = 0
    guard on else { return }
    pulseStart = Date()
    pulse = Timer.scheduledTimer(withTimeInterval: 1.0 / 20, repeats: true) { [weak self] _ in
      MainActor.assumeIsolated {
        guard let self else { return }
        let t = Date().timeIntervalSince(self.pulseStart)
        self.beam = CGFloat((1 - cos(t * 2 * .pi / 1.2)) / 2)  // one slow breath per 1.2 s
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
