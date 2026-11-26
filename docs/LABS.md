# WarpPoint Labs

Developer tools that use the same link as the player features. They're terminal
commands only, so they never show up in the menu bar app.

## Divergence: the Switch against a Mac port

Divergence reads the same Tap profile from the game on the Switch and from the same game
running in an nxnative app on this Mac, then shows which fields differ. With one save on
both (Warp Sync puts it there), a difference is either a real gameplay divergence or a
porting bug.

```sh
warp lab-launch "three houses"      # start the Mac app with a control file Labs can use
warp lab-tap "three houses"         # the Mac app's live values
warp lab-diff "three houses"        # side by side with the Switch; * marks a difference
warp lab-run "three houses" walk.lab
```

`lab-run` presses the same buttons on both machines and compares at each `check`,
stopping at the first field that differs (exit code 3). Presses go to the Switch through
sys-botbase (port 6000), so it needs that installed; WarpPoint's own module never sends
input to a game.

```
# walk.lab: FE3H, the cursor over the starting units
check
Right
wait 1
check
Up
wait 1
check
```

Steps: a button (`A B X Y L R ZL ZR Plus Minus Up Down Left Right LS RS`), `wait
<seconds>`, `check`, and `#` comments.

How the Mac side works: nxnative runs the game natively, so a game address is a Mac
address. `lab-launch` starts the app with `DM_CTL` set to a control file under
`~/Library/Application Support/WarpPoint/labs/`. Reads are `memdump` lines in that
file, and presses are button lines. The app's log names where the main module was
loaded, and where the heap and alias regions are. `main`, `heap` and `alias` fields can be
read there; `aslr` can't.

Tested on FE3H 1.2.0, Switch 1 against the nxnative app:
- Gold and renown agreed out of battle.
- A five-check cursor walk agreed at every check.
- Moving only the Mac's cursor was reported as "Level: 19 on the Switch, 17 on this Mac".

## Finding Tap fields on the Mac port

Reading the Mac port is much faster than reading the Switch, and a pointer chain from
`main` is the same on both, because it's the same game code. FE3H's dialogue line was
found this way:
1. Show a line on screen in the Mac app.
2. `memdump` the heap and search for the text.
3. One copy sat in a small struct that changed with each line.
4. Search `main` for a pointer to that struct: `[main+0x1B4E108]+0x1B48`.
5. Check the chain on the Switch with `warp tap`, which read the same line.

## Not built

- **Tracy**, a profiler for retail games on hardware. It needs NVN hooks inside the game
  on the Switch, which is a code-injection project of its own.
- **Shaders' measurement tools** wait on the Shaders Phase 0 measurement in Astris.
