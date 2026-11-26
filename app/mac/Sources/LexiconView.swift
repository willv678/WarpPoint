import AppKit
import SwiftUI

/// Warp Lexicon: the game's current line of text, live from the Switch (a Tap profile's
/// "lexicon" field), with every line kept below. Select words to Look Up or Translate
/// (macOS's own, in the context menu); save lines for Anki as a CSV.
struct LexiconView: View {
  @ObservedObject var model: WarpModel
  @State private var current = ""
  @State private var lines: [String] = []
  @State private var status = ""
  @State private var saved = 0
  @State private var timer: Timer?

  private static var csv: URL {
    FileManager.default.homeDirectoryForCurrentUser
      .appendingPathComponent("Documents/WarpPoint Lexicon.csv")
  }

  var body: some View {
    VStack(alignment: .leading, spacing: 14) {
      Text(model.nowPlaying ?? "Nothing running").font(.headline)
      if !status.isEmpty {
        Label(status, systemImage: "info.circle").foregroundStyle(.secondary)
      }
      Text(current.isEmpty ? "No text on screen." : current)
        .font(.title2)
        .textSelection(.enabled)
        .frame(maxWidth: .infinity, minHeight: 80, alignment: .topLeading)
        .padding(12)
        .background(.quaternary.opacity(0.5), in: RoundedRectangle(cornerRadius: 10))
      HStack {
        Button("Save Line for Anki") { save(current) }.disabled(current.isEmpty)
        Button("Show CSV") { NSWorkspace.shared.activateFileViewerSelecting([Self.csv]) }
          .disabled(saved == 0 && !FileManager.default.fileExists(atPath: Self.csv.path))
        Spacer()
        if saved > 0 { Text("\(saved) saved").foregroundStyle(.secondary) }
      }
      Text("Earlier").font(.caption.weight(.semibold)).foregroundStyle(.secondary)
      List(lines.reversed(), id: \.self) { l in
        Text(l).textSelection(.enabled)
      }
      Text("Select a word, then right-click for Look Up or Translate. Lines come from the game's Tap profile; a game without a \"lexicon\" field shows nothing here.")
        .font(.caption).foregroundStyle(.tertiary)
    }
    .padding(20)
    .frame(minWidth: 480, minHeight: 360)
    .onAppear {
      model.windowOpened()
      poll()
      timer = Timer.scheduledTimer(withTimeInterval: 1, repeats: true) { _ in
        MainActor.assumeIsolated { poll() }
      }
    }
    .onDisappear {
      timer?.invalidate()
      timer = nil
      model.windowClosed()
    }
  }

  private func poll() {
    model.tapSnapshot { snap in
      let text = snap.fields.first { $0.lexicon }?.value ?? ""
      status = snap.fields.contains { $0.lexicon } ? "" :
        (snap.status.isEmpty ? "This game's profile has no text to show." : snap.status)
      if text != current {
        if !current.isEmpty, lines.last != current { lines.append(current) }
        current = text
      }
    }
  }

  private func save(_ line: String) {
    // One card per line: the line, then an empty back for your own notes.
    let field = "\"" + line.replacingOccurrences(of: "\"", with: "\"\"") + "\""
    let row = field + ",\"\"\n"
    if let h = try? FileHandle(forWritingTo: Self.csv) {
      h.seekToEndOfFile()
      h.write(Data(row.utf8))
      try? h.close()
    } else {
      try? row.write(to: Self.csv, atomically: true, encoding: .utf8)
    }
    saved += 1
  }
}
