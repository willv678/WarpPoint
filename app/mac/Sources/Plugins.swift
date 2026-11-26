import Foundation

/// One plugin, as the engine's catalog describes it (host/plugins.cpp).
struct Plugin: Decodable, Identifiable, Hashable {
  struct Need: Decodable, Hashable {
    let kind: String
    let value: String
    let text: String
    let met: Bool?  // nil: WarpPoint can't tell (LiveSplit running, a Switch not seen yet)
  }
  struct Game: Decodable, Hashable, Identifiable {
    let titleId: String
    let name: String
    let builds: [String]
    let sources: [String]
    var id: String { titleId }
  }

  let id: String
  let name: String
  let icon: String
  let since: String
  let summary: String
  let what: String
  let how: String
  let offNote: String
  let window: String
  let terminalOnly: Bool
  let commands: [String]
  let docs: String
  let enabled: Bool
  let `default`: Bool
  let status: String  // on, off, needsSwitch, needsPlugin, needsSetup, needsProfile
  let statusText: String
  let active: Bool
  let needs: [Need]
  let games: [Game]
  let anyGame: Bool

  /// Something to fix before it can work.
  var blocked: Bool { enabled && status != "on" }

  var docsURL: URL? {
    docs.isEmpty ? nil : URL(string: "https://github.com/willv678/WarpPoint/blob/main/\(docs)")
  }
}

/// The plugin store's view (PackStore::storeJson in host/plugin_store.hpp).
struct PluginStore: Decodable {
  struct Preview: Decodable, Hashable {
    struct Field: Decodable, Hashable {
      let label: String
      let group: String
    }
    var fields: [Field] = []
    var lexicon: Bool = false
    var splits: Int = 0

    private enum CodingKeys: String, CodingKey { case fields, lexicon, splits }
    init() {}
    init(from d: Decoder) throws {
      let c = try d.container(keyedBy: CodingKeys.self)
      fields = (try? c.decode([Field].self, forKey: .fields)) ?? []
      lexicon = (try? c.decode(Bool.self, forKey: .lexicon)) ?? false
      splits = (try? c.decode(Int.self, forKey: .splits)) ?? 0
    }
  }
  struct PackGame: Decodable, Hashable {
    let titleId: String
    let name: String
    let builds: [String]
  }
  struct Pack: Decodable, Identifiable, Hashable {
    let id: String
    let name: String
    let kind: String
    let version: String
    let description: String
    let author: String
    let file: String
    let games: [PackGame]
    let preview: Preview
    let supported: Bool
    let installed: Bool
    let installedVersion: String
    let update: Bool

    var kindName: String {
      switch kind {
      case "tap-profile": return "Tap profile"
      default: return kind
      }
    }
  }
  struct Installed: Decodable, Identifiable, Hashable {
    let id: String
    let name: String
    let version: String
    let file: String
  }

  var online = false
  var fetchError = ""
  var fetched = ""
  var problems: [String] = []
  var packs: [Pack] = []
  var orphans: [Installed] = []

  private enum CodingKeys: String, CodingKey { case online, fetchError, fetched, problems, packs, orphans }
  init() {}
  init(from d: Decoder) throws {
    let c = try d.container(keyedBy: CodingKeys.self)
    online = (try? c.decode(Bool.self, forKey: .online)) ?? false
    fetchError = (try? c.decode(String.self, forKey: .fetchError)) ?? ""
    fetched = (try? c.decode(String.self, forKey: .fetched)) ?? ""
    problems = (try? c.decode([String].self, forKey: .problems)) ?? []
    packs = (try? c.decode([Pack].self, forKey: .packs)) ?? []
    orphans = (try? c.decode([Installed].self, forKey: .orphans)) ?? []
  }

  var fetchedDate: Date? { fetched.isEmpty ? nil : ISO8601DateFormatter().date(from: fetched) }
}

/// Where the store's list comes from. WARP_PLUGIN_INDEX overrides it (a file:// URL to a
/// checkout's plugins/index.json, to try packs before they're on GitHub).
enum PluginStoreSource {
  static var indexURL: URL {
    if let s = ProcessInfo.processInfo.environment["WARP_PLUGIN_INDEX"], let u = URL(string: s) { return u }
    return URL(string: "https://raw.githubusercontent.com/willv678/WarpPoint/main/plugins/index.json")!
  }

  /// No cookies, no cache, no credentials: plain GETs to GitHub and nothing else.
  static let session: URLSession = {
    let config = URLSessionConfiguration.ephemeral
    config.timeoutIntervalForRequest = 15
    config.requestCachePolicy = .reloadIgnoringLocalCacheData
    config.httpCookieStorage = nil
    config.urlCredentialStorage = nil
    return URLSession(configuration: config)
  }()

  /// The file at url, or why not ("can't reach GitHub ...").
  static func get(_ url: URL) async -> Result<Data, Error> {
    do {
      let (data, response) = try await session.data(from: url)
      if let http = response as? HTTPURLResponse, http.statusCode != 200 {
        return .failure(StoreError(message: http.statusCode == 404
          ? "GitHub answered, but has no plugin list at that address (404)."
          : "GitHub answered with an error (HTTP \(http.statusCode))."))
      }
      return .success(data)
    } catch let e as URLError where e.code == .notConnectedToInternet {
      return .failure(StoreError(message: "This Mac isn't connected to the internet."))
    } catch let e as URLError where e.code == .timedOut {
      return .failure(StoreError(message: "raw.githubusercontent.com didn't answer in time."))
    } catch {
      return .failure(StoreError(message: "WarpPoint couldn't connect to raw.githubusercontent.com."))
    }
  }

  static func fileURL(_ file: String) -> URL {
    indexURL.deletingLastPathComponent().appendingPathComponent(file)
  }
}

struct StoreError: Error, LocalizedError {
  let message: String
  var errorDescription: String? { message }
}
