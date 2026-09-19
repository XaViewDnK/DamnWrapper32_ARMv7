# DamnWrapper32 (ARMv7) — by XaView

An iOS emulator (wrapper) for Android that runs 32-bit ARMv7 iOS games.

Theoretical target is iOS 3.0–5.1.1. If you find a game outside the list below that
works, DM me :)

## Supported games

| Game | Versions | Status | Notes |
|---|---|---|---|
| Action Buggy | 1.11 – 1111 | Playable | Saves not tested |
| Traps N Gems | 1.00 – 1.01 | Playable | Saves not tested |
| Minecraft PE | 0.1.2.0 – 0.10.4 | Playable | 0.1.2-0.6.1 have bad system UI, 0.7.0+ have no problems with UI |
| Wolfenstein 3D | 2.1 | Playable | Menu graphics are drawn with visible stair-stepping |
| Super Monkey Ball 2 | 2.0.0 – 3.1.0 + 1.2.0 Lite | Rough | Saves do not work, touch input is inaccurate, crashes on death. Other versions untested |
| Asphalt 6 | 1.0.2 Free | Rough | Long freezes during drifting |

## Settings worth knowing

- **Native ROOT mmap** — loads the game at its original addresses. Needed by some games,
  requires root.
- **GPU Offload** — moves parts of rendering to the GPU. Experimental; if a game shows
  black areas or crashes, turn it off.
- **OpenGL ES Mode** — picks ES 1.1 or ES 2.0. If the game image only supports one of
  them, the wrapper switches to it automatically.

## Command line

- `-launch packagename_version` — skip the wrapper menus and start the game directly.
  Example: `-launch com.sega.smb2_2.0.0`
- `-novideo` — skip video cutscenes in all games.

## Compiling

NDK r17 (build 4754217), released mid-2018 (an