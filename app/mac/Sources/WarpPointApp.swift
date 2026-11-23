import SwiftUI

/// WarpPoint for Mac: a UFO in the menu bar that keeps Switch saves and the emulator in
/// sync on its own. The engine is the C++ core (host/), driven through its C interface.
@main
struct WarpPointApp: App {
  @StateObject private var model = WarpModel()

  var body: some Scene {
    MenuBarExtra {
      MenuView(model: model)
    } label: {
      MenuBarLabel(model: model)
    }
    .menuBarExtraStyle(.window)

    Window("Set Up WarpPoint", id: "setup") {
      SetupView(model: model)
    }
    .windowResizability(.contentSize)

    Settings {
      SettingsView(model: model)
    }

    Window("WarpPoint Library", id: "library") {
      LibraryView(model: model)
    }
    .defaultSize(width: 980, height: 640)
  }
}

/// The UFO in the menu bar. It's on screen from launch, so it's also where the
/// first-run window gets opened from.
struct MenuBarLabel: View {
  @ObservedObject var model: WarpModel
  @Environment(\.openWindow) private var openWindow

  var body: some View {
    Image(nsImage: UFOIcon.image(lift: model.lift, beam: model.beam))
      .task {
        if model.setup.needsSetup {
          openWindow(id: "setup")
          NSApp.activate()
        }
      }
  }
}
