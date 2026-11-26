# Warp Shaders, Spike 1: Switch shader bytes against the emulator's cache

**Question (from the Shaders plan):** do the shader bytes in the game on a Switch hash to
the same identity the emulator's shader cache uses?

**Answer: yes, for Fire Emblem: Three Houses 1.2.0 on a Switch 1 against Astris.**

## What was done (2026-11-26)

1. **Read Astris's guest shader cache for FE3H.** That's `guest.toc` and `guest.data`
   under Astris's container,
   `.../Ryujinx/games/010055d009f78000/cache/shader`. It holds 223 shaders, 1,055,960
   bytes of code.
2. **Rebuilt every hash from the bytes alone.** Ryujinx's identity is
   `H(cb1) * 23 ^ H(code)`, where H is the low 32 bits of XXH3-128. **223 of 223**
   reproduced, so anything that captures the same bytes gets the emulator's exact
   identity.
3. **Searched the game's memory on real hardware.** FE3H was at the title screen. Tap
   read the whole heap (`warp tap-dump`, 32 MB at a time, about 1.6 MB/s, around 35
   minutes). **221 of 223** shaders were there byte-for-byte, the first at
   heap+0x35c2f00.
4. **Searched the nxnative port too.** The same check on the Mac port of the same build
   found 220 of 223. Shaders 216 and 217 were missing on both, which suggests they're
   loaded later in the game.

`tools/shader_spike.py` repeats all of this:
- `cache` lists the cache and checks every hash.
- `export` writes each shader's code to a file.
- `search` looks for the shaders in memory dumps.

## What it means for the design

- **Capture doesn't need a hook to get the code.** The bytes are sitting in the game's
  memory, so the Switch module could read them through the cheat service, the same way
  Tap does, with no SaltyNX plugin.
- **Each shader is easy to find.** It's the 0x50-byte shader header, then Maxwell code
  ending in EXIT and a branch to itself, then NOP padding. A `0x12345678` marker starts
  the next block. The scanner would cut code the same way Ryujinx's decoder decides code
  size.
- **The code alone doesn't help the emulator.** At boot, Ryujinx compiles only the
  programs listed in `shared.toc`. Each entry there pairs guest code with specialization
  state (render state that changes the generated host code). That state only exists at
  draw time, so capturing it still needs hooks on the game's graphics calls (Spike 2,
  the plan's "hard part").

## Next

- **Spike 2:** capture draw-time state. That needs a SaltyNX plugin that hooks NVN, or a
  way to rebuild the state from the guest code plus defaults. Either way, its result has
  to be checked by whether Astris compiles the program at boot instead of mid-game.
- **Measurement (Phase 0):** how much stutter Astris actually has in FE3H. That still
  needs someone at Astris.
