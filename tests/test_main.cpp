// doctest's main, after pointing WarpPoint away from this Mac's own nxnative apps:
// tests only ever see the fake apps they make themselves.
#define DOCTEST_CONFIG_IMPLEMENT
#include "doctest.h"

#include <cstdlib>

int main(int argc, char** argv) {
  setenv("WARPPOINT_NXNATIVE_APPS", "/nonexistent/warppoint-tests/apps", 1);
  setenv("WARPPOINT_NXNATIVE_DATA", "/nonexistent/warppoint-tests/data", 1);
  doctest::Context context;
  context.applyCommandLine(argc, argv);
  return context.run();
}
