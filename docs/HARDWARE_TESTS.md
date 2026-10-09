# Hardware test checklist

Manual tests on a real Switch, one section per milestone. Note the date and result next to each step.

## M2: `.nro` agent with SD folders

**Setup**
- Build: `cmake --build build` and `make -C switch`
- Copy `switch/warppoint_switch.nro` to `sdmc:/switch/warppoint/` and launch it from the Homebrew Menu.
  Faster for repeated builds: press **Y** in the Homebrew Menu, then run
  `/opt/devkitpro/tools/bin/nxlink switch/warppoint_switch.nro` on the Mac.
- On the Mac, put this in `~/Library/Application Support/WarpPoint/config.toml`:

  ```toml
  [unit.test]
  host = "~/WarpPointTest"
  switch = "sdmc:/warppoint-test"
  ```

**Steps**
1. **App starts.** The Switch shows its name and IP and waits for the computer.
2. **Discovery.** `build/host/warppoint run` prints `Connected to <your Switch>` within a few seconds,
   without a `target` line in the config. If it doesn't, add `target = "<IP shown on Switch>"`
   and note that discovery failed on this network.
3. **Mac → Switch.** `mkdir ~/WarpPointTest && echo hello > ~/WarpPointTest/a.sav`.
   The host logs `dir/test: Mac -> Switch` and the Switch logs `Received dir/test`.
4. **Switch → Mac (read path).** Stop the host, run `rm -rf ~/WarpPointTest`, start the host again.
   It logs `Switch -> Mac` and `~/WarpPointTest/a.sav` is back. The file came from the SD card.
5. **Larger file.** `mkfile -n 20m ~/WarpPointTest/big.bin`, then confirm the push completes and
   the Switch stays responsive.
6. **Reconnect.** Exit the app with **+** and relaunch it. The host reconnects by itself.
7. **Protected folders.** Add a unit with `switch = "sdmc:/atmosphere/test"`. The host reports
   an "invalid unit" error for it and nothing is written.
8. **Optional, RetroArch round trip.** Add
   `[unit.retroarch]` with your Mac RetroArch saves folder and `switch = "sdmc:/retroarch/cores/savefiles"`.
   Save in a game on the Mac, close it, and confirm the save loads on the Switch. Then do the reverse.

**Report back:** which steps passed, plus the host log and a photo of the Switch screen for any failure.
