# WarpPoint

Automatic LAN save sync between a desktop Switch emulator and a homebrewed Nintendo Switch.
Close the game on the Mac, pick up the Switch, keep playing from the same save. Then go back.

## This is a rebuild

The `rebuild` branch is a from-scratch rewrite, written by hand where it matters, so every
part is understood. The earlier prototypes are kept as reference:

| Tag | What |
| --- | --- |
| `prototype-v1` | July 2026: single-file push with echo suppression |
| `prototype-v2` | 2026-10-09: full v2 design (planner, state store, protocol v2, `.nro` agent). Never hardware-tested |

Read the prototype side by side: `git worktree add ../WarpPoint-prototype prototype-v2`.
Its `docs/ARCHITECTURE.md` is still the target design.

## Build and test

```sh
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```
