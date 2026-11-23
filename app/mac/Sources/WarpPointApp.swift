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
      Image(nsImage: UFOIcon.image(beam: model.beam))
    }
    .menuBarExtraStyle(.window)
  }
}
