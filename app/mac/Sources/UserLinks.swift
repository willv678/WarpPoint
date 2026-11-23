import SwiftUI

/// One row per console user: which emulator profile their saves go to.
struct UserLinks: View {
  let setup: Setup
  /// The current choice for a console user ("" = don't sync).
  let choice: (String) -> String
  let choose: (String, String) -> Void

  var body: some View {
    if setup.consoleUsers.isEmpty {
      Text("Waiting for your Switch's user list. Open WarpPoint on your Switch.")
        .foregroundStyle(.secondary)
    } else {
      ForEach(setup.usersByActivity) { user in
        Picker(selection: Binding(get: { choice(user.id) }, set: { choose(user.id, $0) })) {
          Text("Don't sync").tag("")
          ForEach(setup.emulatorProfiles) { p in
            Text("\(setup.emulator.name): \(p.name)").tag(p.id)
          }
        } label: {
          VStack(alignment: .leading, spacing: 1) {
            Text(user.label)
            Text("\(user.saves) save\(user.saves == 1 ? "" : "s") on \(setup.console.isEmpty ? "the Switch" : setup.console)")
              .font(.caption).foregroundStyle(.secondary)
          }
        }
      }
    }
  }
}
