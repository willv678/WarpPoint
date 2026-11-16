// WarpPoint hello world: proves the build -> nxlink -> run -> read output loop.
//
// Output goes two places:
//   stdout -> the Switch screen (libnx's text console)
//   stderr -> the Mac, over the nxlink connection (only when launched via nxlink)
#include <switch.h>

#include <arpa/inet.h>
#include <cstdarg>
#include <cstdio>
#include <unistd.h>

namespace {

bool g_nxlink = false;  // true when stderr is connected to the Mac

// printf to the screen, and to the Mac too if nxlink is connected.
void say(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  std::vprintf(fmt, args);
  va_end(args);
  if (g_nxlink) {
    va_start(args, fmt);
    std::vfprintf(stderr, fmt, args);
    va_end(args);
  }
}

}  // namespace

int main(int argc, char* argv[]) {
  (void)argc;
  (void)argv;

  consoleInit(nullptr);  // text console on the screen

  // Start the network stack, then connect stderr back to nxlink on the Mac.
  // nxlinkConnectToHost(redirStdout, redirStderr) returns a socket, or -1 if
  // the app wasn't launched by nxlink (for example, from the Homebrew Menu).
  socketInitializeDefault();
  g_nxlink = nxlinkConnectToHost(false, true) >= 0;

  // gethostid() returns this console's IPv4 address in network byte order.
  in_addr ip{};
  ip.s_addr = static_cast<in_addr_t>(gethostid());

  say("hello from WarpPoint\n");
  say("switch ip: %s\n", inet_ntoa(ip));
  say("launched by nxlink: %s\n", g_nxlink ? "yes" : "no");
  say("press + to exit\n");
  consoleUpdate(nullptr);

  // Read controller input so + can quit.
  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  PadState pad;
  padInitializeDefault(&pad);

  while (appletMainLoop()) {
    padUpdate(&pad);
    if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
    consoleUpdate(nullptr);
  }

  say("bye\n");
  socketExit();
  consoleExit(nullptr);
  return 0;
}
