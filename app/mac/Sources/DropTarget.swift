import AppKit
import SwiftUI

/// WarpDrop's drop target: the UFO in the menu bar takes files dragged onto it.
///
/// The menu itself stays SwiftUI's MenuBarExtra (so it looks and behaves exactly as
/// before). The drop target is a transparent view laid over MenuBarExtra's status item:
///   - AppKit finds drag destinations by registered types, not by hitTest, so the view
///     can return nil from hitTest: clicks go straight through to the status item and
///     the menu opens as before, while drags land here.
///   - NSStatusBarButton is the system's own class and can't be subclassed for this, and
///     forwarding drags through the status bar window's delegate didn't receive them.
///   - SwiftUI may rebuild the status item; attach() is called again from the label's
///     .task whenever that happens, and does nothing when it's already in place.
/// Hovering a drag over the UFO shows a small panel under it saying where files go;
/// the panel is a drop target too. Nothing here runs between drags.
@MainActor
final class DropTarget: NSObject {
  static let shared = DropTarget()

  private weak var model: WarpModel?
  private var openSend: (() -> Void)?
  private var target: DragView?
  private var panel: NSPanel?
  private var hideLater: DispatchWorkItem?
  private var overUFO = false
  private var overPanel = false

  func attach(model: WarpModel, openSend: @escaping () -> Void) {
    self.model = model
    self.openSend = openSend
    guard let content = statusWindow()?.contentView else { return }
    if let target, target.superview === content { return }
    target?.removeFromSuperview()
    let view = DragView(frame: content.bounds)
    view.autoresizingMask = [.width, .height]
    view.onEnter = { [weak self] in self?.entered(ufo: true) ?? [] }
    view.onExit = { [weak self] in self?.exited(ufo: true) }
    view.onDrop = { [weak self] urls in self?.dropped(urls) ?? false }
    content.addSubview(view)
    target = view
  }

  /// For screenshots (WARP_SHOW_DROP_PANEL=1): the panel as it looks during a drag.
  /// Waits a moment: at launch the status item isn't in its place yet.
  func showPanelForScreenshot() {
    DispatchQueue.main.asyncAfter(deadline: .now() + 1.5) {
      self.overUFO = true
      self.showPanel()
    }
  }

  // MARK: - Dragging

  private func entered(ufo: Bool) -> NSDragOperation {
    guard let model, model.pluginOn("drop") else { return [] }
    if ufo { overUFO = true } else { overPanel = true }
    hideLater?.cancel()
    showPanel()
    return .copy
  }

  private func exited(ufo: Bool) {
    if ufo { overUFO = false } else { overPanel = false }
    // Moving from the UFO to the panel (or back) passes through neither for a moment.
    hideLater?.cancel()
    let work = DispatchWorkItem { [weak self] in
      guard let self, !self.overUFO, !self.overPanel else { return }
      self.hidePanel()
    }
    hideLater = work
    DispatchQueue.main.asyncAfter(deadline: .now() + 0.35, execute: work)
  }

  private func dropped(_ urls: [URL]) -> Bool {
    overUFO = false
    overPanel = false
    hidePanel()
    guard let model, !urls.isEmpty, model.pluginOn("drop") else { return false }
    model.drop.prepare(urls)
    openSend?()
    NSApp.activate()
    return true
  }

  // MARK: - The panel under the UFO

  private func showPanel() {
    guard let model else { return }
    if panel == nil {
      let p = NSPanel(contentRect: NSRect(x: 0, y: 0, width: 300, height: 76),
                      styleMask: [.borderless, .nonactivatingPanel], backing: .buffered, defer: true)
      p.isFloatingPanel = true
      p.level = .popUpMenu
      p.backgroundColor = .clear
      p.isOpaque = false
      p.hasShadow = true
      p.hidesOnDeactivate = false
      p.collectionBehavior = [.canJoinAllSpaces, .transient]

      let glass = NSVisualEffectView()
      glass.material = .popover
      glass.blendingMode = .behindWindow
      glass.state = .active
      glass.wantsLayer = true
      glass.layer?.cornerRadius = 14
      glass.layer?.masksToBounds = true

      let drop = DragView()
      drop.onEnter = { [weak self] in self?.entered(ufo: false) ?? [] }
      drop.onExit = { [weak self] in self?.exited(ufo: false) }
      drop.onDrop = { [weak self] urls in self?.dropped(urls) ?? false }
      let host = PassThroughHostingView(rootView: DropPanelView(model: model))
      for v in [glass, drop, host] as [NSView] {
        v.translatesAutoresizingMaskIntoConstraints = false
      }
      let root = NSView()
      root.addSubview(glass)
      glass.addSubview(host)
      root.addSubview(drop)
      NSLayoutConstraint.activate([
        glass.leadingAnchor.constraint(equalTo: root.leadingAnchor),
        glass.trailingAnchor.constraint(equalTo: root.trailingAnchor),
        glass.topAnchor.constraint(equalTo: root.topAnchor),
        glass.bottomAnchor.constraint(equalTo: root.bottomAnchor),
        host.leadingAnchor.constraint(equalTo: glass.leadingAnchor),
        host.trailingAnchor.constraint(equalTo: glass.trailingAnchor),
        host.topAnchor.constraint(equalTo: glass.topAnchor),
        host.bottomAnchor.constraint(equalTo: glass.bottomAnchor),
        drop.leadingAnchor.constraint(equalTo: root.leadingAnchor),
        drop.trailingAnchor.constraint(equalTo: root.trailingAnchor),
        drop.topAnchor.constraint(equalTo: root.topAnchor),
        drop.bottomAnchor.constraint(equalTo: root.bottomAnchor),
      ])
      p.contentView = root
      panel = p
    }
    guard let panel else { return }
    panel.appearance = NSApp.appearance
    let size = panel.contentView?.fittingSize ?? NSSize(width: 300, height: 76)
    let width: CGFloat = 300
    let height = max(64, size.height)
    if let status = statusWindow()?.frame, let screen = statusWindow()?.screen ?? NSScreen.main {
      var x = status.minX - 8
      x = min(max(x, screen.visibleFrame.minX + 8), screen.visibleFrame.maxX - width - 8)
      panel.setFrame(NSRect(x: x, y: status.minY - 6 - height, width: width, height: height), display: true)
    }
    panel.orderFrontRegardless()
  }

  private func hidePanel() {
    hideLater?.cancel()
    panel?.orderOut(nil)
  }

  // MARK: - MenuBarExtra's status item

  /// This app's one status item window (MenuBarExtra's). Matched by class name: AppKit
  /// doesn't hand out the status item SwiftUI made.
  private func statusWindow() -> NSWindow? {
    let windows = NSApp.windows.filter { String(describing: type(of: $0)) == "NSStatusBarWindow" }
    return windows.first { $0.frame.height > 0 } ?? windows.first
  }
}

/// A transparent drop target: takes file drags, lets every click through.
final class DragView: NSView {
  var onEnter: () -> NSDragOperation = { [] }
  var onExit: () -> Void = {}
  var onDrop: ([URL]) -> Bool = { _ in false }

  override init(frame: NSRect) {
    super.init(frame: frame)
    registerForDraggedTypes([.fileURL])
  }
  required init?(coder: NSCoder) { fatalError("not used") }

  override func hitTest(_ point: NSPoint) -> NSView? { nil }  // clicks go to what's under it

  override func draggingEntered(_ sender: NSDraggingInfo) -> NSDragOperation {
    hasFiles(sender) ? onEnter() : []
  }
  override func draggingUpdated(_ sender: NSDraggingInfo) -> NSDragOperation {
    hasFiles(sender) ? .copy : []
  }
  override func draggingExited(_ sender: NSDraggingInfo?) { onExit() }
  override func draggingEnded(_ sender: NSDraggingInfo) { onExit() }
  override func performDragOperation(_ sender: NSDraggingInfo) -> Bool {
    let urls = sender.draggingPasteboard.readObjects(forClasses: [NSURL.self],
                                                     options: [.urlReadingFileURLsOnly: true]) as? [URL] ?? []
    return onDrop(urls)
  }

  private func hasFiles(_ info: NSDraggingInfo) -> Bool {
    info.draggingPasteboard.canReadObject(forClasses: [NSURL.self], options: [.urlReadingFileURLsOnly: true])
  }
}

/// SwiftUI content that never takes the mouse (the panel only shows text).
final class PassThroughHostingView<Content: View>: NSHostingView<Content> {
  override func hitTest(_ point: NSPoint) -> NSView? { nil }
}

/// What the panel under the UFO says during a drag.
struct DropPanelView: View {
  @ObservedObject var model: WarpModel

  private var name: String {
    if let c = model.console { return c }
    return model.library.console.isEmpty ? "your Switch" : model.library.console
  }

  private var lines: (title: String, detail: String?, icon: String, tint: Color) {
    if model.plugin("drop")?.status == "needsSwitch" {
      return ("Drop to send to \(name)", "Update WarpPoint on the Switch first (1.12 or later).",
              "exclamationmark.triangle.fill", .orange)
    }
    if model.drop.queue.status == "needsPairing" {
      return ("Drop to send to \(name)", "Pair this Mac with the Switch first.", "lock.fill", .orange)
    }
    if model.console != nil {
      return ("Drop to send to \(name)", nil, "tray.and.arrow.down.fill", .accentColor)
    }
    return ("\(name.prefix(1).uppercased() + name.dropFirst()) is away.", "Files will wait until it's back.",
            "moon.zzz.fill", .secondary)
  }

  var body: some View {
    let l = lines
    HStack(spacing: 12) {
      Image(systemName: l.icon)
        .font(.system(size: 22, weight: .medium))
        .foregroundStyle(l.tint)
        .frame(width: 32)
      VStack(alignment: .leading, spacing: 2) {
        Text(l.title).font(.callout.weight(.semibold)).lineLimit(2)
        if let detail = l.detail {
          Text(detail).font(.caption).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
        }
      }
      Spacer(minLength: 0)
    }
    .padding(.horizontal, 14)
    .padding(.vertical, 14)
    .frame(width: 300, alignment: .leading)
  }
}
