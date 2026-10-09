# WarpPoint 🛸

Automatic save sync between a desktop Switch emulator and your homebrewed Nintendo Switch, over your local network.

Play on your Mac (or PC) at full resolution, close the game, pick up your Switch, and keep going from the same save. Then go back the other way. No SD card swapping, FTP, cloud accounts or IP addresses.

* **Real game saves:** Ryujinx-family (Ryujinx, Ryubing, Astris) and yuzu-family emulators ↔ native Switch titles.
* **Homebrew emulator saves too:** any folder ↔ any SD-card folder (RetroArch, etc.).
* **Safe by default:** three-way state comparison, a backup before every overwrite, and conflicts keep both copies.
* **Hands-off on the Switch:** runs as a background sysmodule with a Tesla overlay for status.
* **One host codebase** for macOS, Windows and Linux.

> **Status:** being rebuilt as v2. The v1 prototype (one-way file push) works but is being replaced.
> See [`ROADMAP.md`](ROADMAP.md) for progress and [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) for the design.

## Building

Host (macOS / Linux / Windows):

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build            # unit tests
scripts/e2e_test.sh build         # host + agent end-to-end (macOS / Linux)
```

Switch (requires [devkitPro](https://devkitpro.org/wiki/Getting_Started) with `switch-dev`; see `scripts/setup_devkitpro_mac.sh`):

```sh
make -C switch
```

## Trying it without a Switch

`warppoint-agent` is the Switch agent built for your computer. It serves a folder as if it were the SD card:

```sh
build/agent/warppoint-agent --root /tmp/fake-sd
```

Then point the host at your saves. `warppoint run` creates a config template on first launch
(`~/Library/Application Support/WarpPoint/config.toml` on macOS):

```toml
[unit.retroarch]
host = "~/Library/Application Support/RetroArch/saves"
switch = "sdmc:/retroarch/cores/savefiles"
```

```sh
build/host/warppoint run       # finds the agent automatically and keeps both sides in sync
build/host/warppoint status    # units, known Switches, open conflicts
build/host/warppoint resolve retroarch host   # settle a conflict (or: switch)
```

<pre align="center">


⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⢀⣠⠤⠶⣷⠲⠤⣄⡀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀
⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⢀⣤⠞⢉⠀⠀⠀⠿⠦⠤⢦⣍⠲⣄⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀
⠀⠀⠀⠀⠀⠀⠀⠀⠀⣠⡤⣤⡞⢡⡶⠋⠀⠀⠀⠀⠀⠀⠀⠀⠀⠈⢧⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀
⠀⢀⣤⠴⠒⣾⠿⢟⠛⠻⣿⡿⣭⠿⠁⢰⠰⠀⠀⠀⠄⣄⣀⡀⠀⠀⠘⣇⠀⠀⠀⠀⠀⠀⠀⠀⠀
⢰⣿⣿⣦⡀⠙⠛⠋⠀⠀⠉⠻⠿⢷⣦⣿⣤⣤⣤⣤⣀⣈⠉⠛⠽⣆⡒⣿⣯⣷⣄⠀⠀⠀⠀⠀⠀
⠀⠻⣍⠻⠿⣿⣦⣄⡀⢠⣾⠑⡆⠀⠈⠉⠛⠛⢿⡿⠿⠿⢿⣿⣿⣿⣿⠟⠉⠉⢿⣟⢲⢦⣀⠀⠀
⠀⠀⠈⠙⠲⢤⣈⠉⠛⠷⢿⣏⣀⡀⠀⠀⠀⢰⣏⣳⠀⠀⠀⠀⠀⣸⣓⣦⠀⠀⠈⠛⠟⠃⣈⣷⡀
⠀⠀⠀⠀⠀⠈⢿⣙⡓⣶⣤⣤⣀⡀⠀⠀⠀⠈⠛⠁⠀⠀⠀⠀⠀⠹⣿⣯⣤⣶⣶⣶⣿⠘⡿⢸⡿
⠀⠀⠀⠀⠀⠀⠀⠙⠻⣿⡛⠻⢿⣯⣽⣷⣶⣶⣤⣤⣤⣤⣄⣀⣀⢀⣀⢀⣀⣈⣥⡤⠶⠗⠛⠋⠀
⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠉⠓⠲⣬⣍⣉⡉⠙⠛⠛⠛⠉⠙⠉⠙⠉⣹⣿⠿⠛⠁⠀⠀⠀⠀⠀⠀
⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⢀⡼⠃⠀⠀⠉⠉⠉⠻⡗⠒⠒⠚⠋⠉⠁⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀
⠀⠀⠀⠀⠀⠀⠀⠀⠀⢀⡴⠋⠀⠀⠀⠀⠀⠀⠀⠀⣿⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀
⠀⠀⠀⠀⠀⠀⠀⠀⡴⠋⠂⠀⠀⠀⠀⠀⠀⠀⠀⠀⢸⡀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀
⠀⠀⠀⠀⠀⠀⣠⠞⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠘⡇⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀
⠀⠀⠀⠀⣠⠞⠁⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀ ⠀⠀⣧⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀
⠀⠀⢀⠔⠁⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⢻⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀
⠀⠔⠁⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⢸⡀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀
⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠐⡇⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀
  ⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⣀⣀⣤⣤⣤⣤⣄⡀⠀⠀
⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⢀⣀⣀⣤⣤⣶⣶⣿⣿⢋⣭⣅⣠⣄⠠⣻⣷⣄⠀
⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⢀⣀⣤⣤⣴⣶⣾⣿⠿⠿⠛⠛⠋⠉⠁⣿⣿⣿⠈⢙⡿⣿⣿⣼⣿⣿⢿⣆
⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⢀⣀⡀⠤⠄⣤⣶⣾⠿⠿⠛⠛⠛⠉⠉⠀⠀⠀⠀⠀⠀⠀⠀⢀⣿⣿⣿⢾⣿⣿⣬⣬⠛⠛⠁⠌⣿
⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⢀⣀⣀⠤⠴⢒⣚⠭⠥⠀⠂⡰⠊⠉⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀    ⢸⣿⣿⡏⠀⠀⠹⡿⠟⠀⠀⢸⠀⢹
⠀⠀⠀⠀⠀⠀⠀⠀⠀⣀⣠⠤⠔⣒⣊⠭⠄⠒⠒⠉⠉⠀⠀⠀⠀   ⠀⠀⠀      ⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⢸⣿⣿⡇⢀⣶⣿⣿⣿⣦⡀⢸⠀⢸
⠀⠀⠀⠀⠀⠀⡠⠒⠉⠀⡼⠀⠏⠀⠀⠀⠀⠀⠀⠀⠀⠀                ⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⢸⣿⡿⡇⠟⣿⣯⣤⣿⣿⠇⠈⠀⢸
⠀⠀⠀⠀⣠⠞⠓⣦⡄⢠⠃⠸⠀⠀⠀⠀⠀⠀⠀⠀⠀                 ⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⢸⣿⡇⡇⠀⠘⠿⠿⠟⠁⠀⢰⠀⢸
⠀⠀⠀⢠⢻⣤⣶⣿⠇⡎⠀⡆⠀⠀⠀⠀⠀⠀                    ⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⣸⣿⠃⡇⢠⣤⣄⠀⠀⠀⠀⢸⠀⢸
⠀⠀⠀⡎⠀⠙⠉⠁⢠⠁⢸⠁⠀⠀⠀⠀   ⠀⠀                 ⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀  ⣿⣿⠀⡇⠘⢿⡋⠀⠀⠀⠀⢸⠀⢸
⠀⠀⢠⠁⢠⣴⢆⡀⡼⠀⡇⠀⠀⠀   ⠀⠀⠀                 ⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀  ⠀⣿⣿⢀⡇⢸⣿⡇⠀⠀⢀⣄⡏⠀⡌
⠀⠀⣾⣼⢇⡹⠿⢣⡇⡸⠀⠀    ⠀⠀⠀⠀                 ⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⣿⣿⢸⠁⠀⠀⠀⠀⠀⣨⡞⠀⣰⠃
⠀⣸⠁⠙⠾⠃⠀⣾⠇⡇⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀          ⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⢀⣀⣀⣰⣿⣿⢸⠀⠀⠀⣀⣠⣶⣷⣇⣠⠃⠀
⢀⠇⠀⠀⣶⡿⢰⣿⣼⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀ ⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⢀⣀⣀⣠⣤⣤⣴⣶⣶⣶⣿⣿⣟⣿⣿⣿⣿⣿⣿⣿⣿⣶⣿⣿⣿⣿⣿⡿⠛⠁⠀⠀
⢸⠀⠀⠀⠀⠀⣎⣿⡏⠀⠀⠀⠀⠀⠀⠀⢀⣀⣀⣤⣤⣤⣤⣤⣶⣶⣶⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⠿⠿⠿⠛⠛⠛⠛⠋⠉⠁⠀⠀⠀⠀⠀
⢸⡇⠀⠀⠀⢠⣼⣿⣥⣤⣴⣶⣶⣾⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⡿⠿⠿⠿⠛⠛⠛⠉⠉⠉⠁⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀
⠈⢿⣶⣴⣶⣾⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⡿⠿⠿⠿⠛⠛⠛⠋⠉⠉⠉⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀
⠀⠀⠉⠛⠻⠿⠟⠛⠛⠛⠛⠉⠉⠉⠁⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀
  
</pre>
