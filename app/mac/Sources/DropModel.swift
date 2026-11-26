import AppKit
import Foundation
import UserNotifications

/// WarpDrop's state for the app: the files waiting in the Send window, and the transfer
/// queue (host/drop_queue.hpp). Nothing here runs on its own: the queue is re-read when
/// the engine says it changed (a "drop" event), and files are only looked at when dropped.
@MainActor
final class DropModel: ObservableObject {
  /// One dropped file or folder, as the engine classified it (AppCore::dropClassify).
  struct Item: Decodable, Identifiable, Hashable {
    var path: String
    var name: String
    var isDir = false
    var size: Int64 = 0
    var files = 0
    var kind = "file"
    var label = "File"
    var side = "switch"
    var dest = ""
    var titleId = ""
    var game = ""
    var buildId = ""
    var needsGame = false
    var runsAtBoot = false
    var notInstallable = false
    var remembered = false
    var note = ""
    var problem = ""
    var id: String { path }

    private enum CodingKeys: String, CodingKey {
      case path, name, isDir, size, files, kind, label, side, dest, titleId, game, buildId, needsGame,
           runsAtBoot, notInstallable, remembered, note, problem
    }
    init(from d: Decoder) throws {
      let c = try d.container(keyedBy: CodingKeys.self)
      path = try c.decode(String.self, forKey: .path)
      name = (try? c.decode(String.self, forKey: .name)) ?? (path as NSString).lastPathComponent
      isDir = (try? c.decode(Bool.self, forKey: .isDir)) ?? false
      size = (try? c.decode(Int64.self, forKey: .size)) ?? 0
      files = (try? c.decode(Int.self, forKey: .files)) ?? 0
      kind = (try? c.decode(String.self, forKey: .kind)) ?? "file"
      label = (try? c.decode(String.self, forKey: .label)) ?? "File"
      side = (try? c.decode(String.self, forKey: .side)) ?? "switch"
      dest = (try? c.decode(String.self, forKey: .dest)) ?? ""
      titleId = (try? c.decode(String.self, forKey: .titleId)) ?? ""
      game = (try? c.decode(String.self, forKey: .game)) ?? ""
      buildId = (try? c.decode(String.self, forKey: .buildId)) ?? ""
      needsGame = (try? c.decode(Bool.self, forKey: .needsGame)) ?? false
      runsAtBoot = (try? c.decode(Bool.self, forKey: .runsAtBoot)) ?? false
      notInstallable = (try? c.decode(Bool.self, forKey: .notInstallable)) ?? false
      remembered = (try? c.decode(Bool.self, forKey: .remembered)) ?? false
      note = (try? c.decode(String.self, forKey: .note)) ?? ""
      problem = (try? c.decode(String.self, forKey: .problem)) ?? ""
    }
  }

  struct Kind: Decodable, Hashable, Identifiable {
    let id: String
    let label: String
  }

  /// The Send window's view of things.
  struct Sheet: Decodable {
    var console = ""
    var present = false
    var canReceive: Bool?  // nil: no Switch seen yet
    var enabled = true
    var status = "idle"
    var statusText = ""
    var kinds: [Kind] = []
    var items: [Item] = []

    private enum CodingKeys: String, CodingKey { case console, present, canReceive, enabled, status, statusText, kinds, items }
    init() {}
    init(from d: Decoder) throws {
      let c = try d.container(keyedBy: CodingKeys.self)
      console = (try? c.decode(String.self, forKey: .console)) ?? ""
      present = (try? c.decode(Bool.self, forKey: .present)) ?? false
      canReceive = try? c.decode(Bool.self, forKey: .canReceive)
      enabled = (try? c.decode(Bool.self, forKey: .enabled)) ?? true
      status = (try? c.decode(String.self, forKey: .status)) ?? "idle"
      statusText = (try? c.decode(String.self, forKey: .statusText)) ?? ""
      kinds = (try? c.decode([Kind].self, forKey: .kinds)) ?? []
      items = (try? c.decode([Item].self, forKey: .items)) ?? []
    }
  }

  /// A file in the transfer queue (DropQueue::json).
  struct Transfer: Decodable, Identifiable, Hashable {
    let id: String
    let batch: String
    let name: String
    let dest: String
    let folder: String
    let kind: String
    let size: Int64
    let sent: Int64
    let state: String  // queued, sending, needsChoice, done, failed, skipped, cancelled
    let error: String

    var finished: Bool { ["done", "failed", "skipped", "cancelled"].contains(state) }
    var progress: Double { size > 0 ? min(1, Double(sent) / Double(size)) : (state == "done" ? 1 : 0) }
  }

  struct Queue: Decodable {
    var status = "idle"  // idle, waiting, sending, needsUpdate, needsPairing, disabled
    var statusText = ""
    var console = ""
    var items: [Transfer] = []

    private enum CodingKeys: String, CodingKey { case status, statusText, console, items }
    init() {}
    init(from d: Decoder) throws {
      let c = try d.container(keyedBy: CodingKeys.self)
      status = (try? c.decode(String.self, forKey: .status)) ?? "idle"
      statusText = (try? c.decode(String.self, forKey: .statusText)) ?? ""
      console = (try? c.decode(String.self, forKey: .console)) ?? ""
      items = (try? c.decode([Transfer].self, forKey: .items)) ?? []
    }
  }

  /// What the person picked in the Send window, per item path.
  struct Choice: Hashable {
    var kind: String
    var folder: String  // on the SD card, for SD card kinds
    var game: String    // title id (mods, cheats) or save key (saves)
  }

  @Published private(set) var sheet = Sheet()
  @Published var choices: [String: Choice] = [:]
  @Published private(set) var queue = Queue()
  @Published private(set) var loading = false
  @Published private(set) var sending = false
  @Published var sendResult: [String] = []  // errors from the last Send
  /// The Send window asked to close itself after a Send that went through.
  @Published var closeSheet = false

  private let core: OpaquePointer
  private var loaded = false
  private var unzipped: [URL] = []

  init(core: OpaquePointer) { self.core = core }

  // MARK: - The Send window

  /// Files dropped on the UFO (or picked): classify them for the Send window, off the
  /// main thread (a big folder takes a moment to list).
  func prepare(_ urls: [URL]) {
    let paths = urls.map { $0.path }
    sendResult = []
    closeSheet = false
    loading = true
    sheet.items = []
    choices = [:]
    classify(paths.map { ["path": $0] }) { [weak self] s in
      guard let self else { return }
      self.sheet = s
      for item in s.items {
        self.choices[item.path] = Choice(kind: item.kind, folder: DropModel.folder(of: item.dest),
                                         game: item.titleId)
      }
      self.loading = false
    }
  }

  /// The type or game changed for one item: ask the engine where it goes now.
  func update(_ item: Item, kind: String? = nil, game: String? = nil) {
    var c = choices[item.path] ?? Choice(kind: item.kind, folder: DropModel.folder(of: item.dest), game: item.titleId)
    if let kind { c.kind = kind }
    if let game { c.game = game }
    choices[item.path] = c
    let gameId = c.kind == "save" ? "" : c.game
    classify([["path": item.path, "kind": c.kind, "game": gameId]]) { [weak self] s in
      guard let self, let fresh = s.items.first,
            let i = self.sheet.items.firstIndex(where: { $0.path == item.path }) else { return }
      self.sheet.items[i] = fresh
      if kind != nil { self.choices[item.path]?.folder = DropModel.folder(of: fresh.dest) }
    }
  }

  func remove(_ item: Item) {
    sheet.items.removeAll { $0.path == item.path }
    choices[item.path] = nil
  }

  /// Why Send can't go yet, or nil.
  func blocker(_ item: Item) -> String? {
    if !item.problem.isEmpty { return item.problem }
    let c = choices[item.path]
    if item.needsGame && (c?.game ?? "").isEmpty {
      return item.kind == "save" ? "Pick whose save this is." : "Pick the game."
    }
    if item.kind == "save" && !(c?.game ?? "").hasPrefix("game/") { return "Pick whose save this is." }
    return nil
  }

  var canSend: Bool {
    !sheet.items.isEmpty && sheet.items.allSatisfy { blocker($0) == nil } && !loading && !sending
  }

  func send() {
    var list: [[String: Any]] = []
    for item in sheet.items {
      let c = choices[item.path] ?? Choice(kind: item.kind, folder: DropModel.folder(of: item.dest), game: "")
      var path = item.path
      if item.kind == "save", !item.isDir, path.lowercased().hasSuffix(".zip") {
        guard let dir = unzip(URL(fileURLWithPath: path)) else {
          sendResult = ["\(item.name): couldn't unzip it"]
          return
        }
        path = dir.path
      }
      var entry: [String: Any] = ["path": path, "kind": c.kind]
      if item.side == "switch" {
        // The folder may have been edited; the name stays as classified. A folder laid out
        // like the SD card (dest "") goes to the card's root as it is.
        let name = (item.dest as NSString).lastPathComponent
        let folder = c.folder.trimmingCharacters(in: CharacterSet(charactersIn: "/ "))
        entry["dest"] = item.dest.isEmpty ? "" : (folder.isEmpty ? name : folder + "/" + name)
      }
      if c.kind == "save" { entry["key"] = c.game } else if !c.game.isEmpty { entry["game"] = c.game }
      list.append(entry)
    }
    guard let data = try? JSONSerialization.data(withJSONObject: ["items": list]) else {
      sendResult = ["Something went wrong."]
      return
    }
    // Off the main thread: a mod folder is copied and a save imported right here.
    let core = self.core
    let request = String(decoding: data, as: UTF8.self)
    sending = true
    Task.detached {
      let raw = WarpModel.takeString(warp_core_drop_send(core, request)) ?? "{}"
      let j = (try? JSONSerialization.jsonObject(with: Data(raw.utf8)) as? [String: Any]) ?? [:]
      await MainActor.run { self.sent(j) }
    }
  }

  private func sent(_ j: [String: Any]) {
    sending = false
    cleanUpUnzipped()
    let errors = j["errors"] as? [String] ?? (j.isEmpty ? ["Something went wrong."] : [])
    let messages = j["messages"] as? [String] ?? []
    sendResult = errors
    if !messages.isEmpty { notify("WarpDrop", messages.joined(separator: " ")) }
    refreshQueue()
    if errors.isEmpty { closeSheet = true }
  }

  /// "Send Files to Switch…": pick files, then the Send window.
  func pickFiles(then open: @escaping () -> Void) {
    let panel = NSOpenPanel()
    panel.canChooseFiles = true
    panel.canChooseDirectories = true
    panel.allowsMultipleSelection = true
    panel.prompt = "Choose"
    panel.message = "Choose files or folders to send to your Switch."
    NSApp.activate()
    guard panel.runModal() == .OK, !panel.urls.isEmpty else { return }
    prepare(panel.urls)
    open()
  }

  // MARK: - The queue

  func handle(_ kind: String, _ e: [String: Any]) {
    if kind == "dropDone" {
      notify("WarpDrop", e["message"] as? String ?? "Done.")
    }
    refreshQueue()
  }

  func refreshQueue() {
    guard let raw = WarpModel.takeString(warp_core_drop_queue(core)),
          let q = try? JSONDecoder().decode(Queue.self, from: Data(raw.utf8)) else { return }
    queue = q
    loaded = true
  }

  /// The queue as last read; read once the first time something asks.
  func loadQueueOnce() { if !loaded { refreshQueue() } }

  func cancel(_ t: Transfer?) {
    _ = warp_core_drop_cancel(core, t?.id ?? "")
    refreshQueue()
  }

  func choose(_ t: Transfer, _ choice: String, wholeBatch: Bool) {
    _ = warp_core_drop_choose(core, t.id, choice, wholeBatch)
    refreshQueue()
  }

  func retry(_ t: Transfer) {
    _ = warp_core_drop_retry(core, t.id)
    refreshQueue()
  }

  func clearFinished() {
    warp_core_drop_clear(core)
    refreshQueue()
  }

  // MARK: - Helpers

  static func folder(of dest: String) -> String {
    guard let slash = dest.lastIndex(of: "/") else { return "" }
    return String(dest[..<slash])
  }

  static func icon(_ kind: String) -> String {
    switch kind {
    case "homebrew": return "app.badge"
    case "overlay": return "square.stack"
    case "mod": return "shippingbox"
    case "cheats": return "wand.and.stars"
    case "theme": return "paintpalette"
    case "tapProfile": return "gauge.with.dots.needle.33percent"
    case "save": return "externaldrive.badge.timemachine"
    case "payload": return "bolt.horizontal"
    case "gamePackage": return "gamecontroller"
    default: return "doc"
    }
  }

  static func bytes(_ n: Int64) -> String { ByteCountFormatter.string(fromByteCount: n, countStyle: .file) }

  private func classify(_ items: [[String: String]], done: @escaping (Sheet) -> Void) {
    let core = self.core
    guard let data = try? JSONSerialization.data(withJSONObject: ["items": items]) else { return }
    let request = String(decoding: data, as: UTF8.self)
    Task.detached {
      let raw = WarpModel.takeString(warp_core_drop_classify(core, request)) ?? "{}"
      let s = (try? JSONDecoder().decode(Sheet.self, from: Data(raw.utf8))) ?? Sheet()
      await MainActor.run { done(s) }
    }
  }

  /// A zipped save (JKSV can export them), unpacked to a temporary folder with ditto.
  private func unzip(_ zip: URL) -> URL? {
    let dir = FileManager.default.temporaryDirectory
      .appendingPathComponent("WarpDrop-\(UUID().uuidString)")
      .appendingPathComponent(zip.deletingPathExtension().lastPathComponent)
    try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
    let p = Process()
    p.executableURL = URL(fileURLWithPath: "/usr/bin/ditto")
    p.arguments = ["-x", "-k", zip.path, dir.path]
    guard (try? p.run()) != nil else { return nil }
    p.waitUntilExit()
    guard p.terminationStatus == 0 else { return nil }
    unzipped.append(dir.deletingLastPathComponent())
    // A zip of one folder: use that folder.
    let inside = (try? FileManager.default.contentsOfDirectory(at: dir, includingPropertiesForKeys: nil)) ?? []
    let real = inside.filter { $0.lastPathComponent != "__MACOSX" }
    if real.count == 1, (try? real[0].resourceValues(forKeys: [.isDirectoryKey]))?.isDirectory == true {
      return real[0]
    }
    return dir
  }

  private func cleanUpUnzipped() {
    for d in unzipped { try? FileManager.default.removeItem(at: d) }
    unzipped = []
  }

  private func notify(_ title: String, _ body: String) {
    let center = UNUserNotificationCenter.current()
    center.requestAuthorization(options: [.alert]) { granted, _ in
      guard granted else { return }
      let content = UNMutableNotificationContent()
      content.title = title
      content.body = body
      center.add(UNNotificationRequest(identifier: UUID().uuidString, content: content, trigger: nil))
    }
  }
}
