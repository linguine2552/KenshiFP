# KenshiFP — First-Person Mode for Kenshi

Play Kenshi in true first person: the camera is welded to your character's
head, and the RTS click-to-move scheme is replaced with WASD + mouse-look.
Built as a native code plugin — no game files are modified.

## Showcase

[![KenshiFP showcase video](https://img.youtube.com/vi/fRytSR9aURI/maxresdefault.jpg)](https://youtu.be/fRytSR9aURI)

*Click to watch the mod in action.*

**Supports Steam Kenshi 1.0.68 and 1.0.65 (x64)**, Windows or Linux/Proton.
The mod detects the game build at load and uses the matching addresses; on an
unrecognized build it disables itself cleanly (logged) rather than misbehaving.
**Compatible with [RE_Kenshi](https://www.nexusmods.com/kenshi/mods/2063)**,
which downgrades installs to 1.0.65 — KenshiFP runs on that build too.

## Features

- **True first-person camera** — attached to the head bone; tracks
  animations and ragdolls, roll-free, with FOV and near-clip tuned for FP.
- **WASD movement with full game-parity** — drives the game's own
  click-to-move order system: real pathfinding, slope climbing, walk/run
  animations, and downed characters struggle back to their feet, exactly as
  if you were right-clicking.
- **Look-direction targeting** — holding W walks toward the terrain point
  you're looking at (the same picking right-click uses).
- **Mouse-look via DirectInput** — raw-quality hardware deltas that coexist
  with the game's own input (no stolen registrations, no cursor-warp lag).
- **Scroll wheel speed control** — from a slow walk to a full run.
- **Turn in place** — your character's body follows the camera while idle.
- **Crosshair** — replaces the hidden arrow cursor; the game's contextual
  cursors (attack, talk) still appear when relevant.
- **Smart cursor release** — the mouse frees automatically for dialogue,
  inventory, trade/looting, stats, map/factions, options, save/load,
  message boxes, and the right-click context menu; recaptures on close.
- **Building interiors preload** as you approach from outside — no more
  empty doorways.
- **Foliage fix** — grass no longer vanishes underfoot in first person.

## Controls

| Input | Action |
|---|---|
| **Right Alt** | Toggle first-person on/off for the selected character |
| **Mouse** | Look |
| **W A S D** | Move (camera-relative) |
| **Scroll wheel** | Movement speed (walk ↔ run) |
| Everything else | Vanilla Kenshi bindings |

## Installation

1. Download the release zip.
2. Copy `KenshiFP_x64.dll` and the `kenshifp/` folder into your Kenshi
   install directory (next to `kenshi_x64.exe`).
3. Open `Plugins_x64.cfg` in the same directory and add this line at the
   end of the plugin list:
   ```
   Plugin=KenshiFP_x64
   ```
4. Launch the game, select a character, press **Right Alt**.

To uninstall, remove that line (and optionally the files).

A log (`KenshiFP.log`) is written next to the game exe — include it in any
bug report.

## Compatibility

- **RE_Kenshi:** compatible. RE_Kenshi downgrades installs to Kenshi 1.0.65
  (it relaunches the game from its own bundled executable). KenshiFP ships
  address tables for both 1.0.68 and 1.0.65 and auto-selects the right one at
  load, so it runs whether or not RE_Kenshi is present. Verified with both
  mods loaded together.
- Coexists fine with other mods that don't replace the game executable.

## Known limitations

- **Supports Steam 1.0.68 and 1.0.65 only.** On any other build, KenshiFP
  detects the mismatch and disables itself with a log message rather than
  running against wrong addresses.
- Backpedaling (S) turns the character around rather than walking backward
  facing the camera (the engine's physics drives orientation during
  movement; a facing-locked backpedal is future work).
- Grass billboards shimmer when rotating the view — an engine impostor
  artifact that eye-level viewing makes more visible.
- GOG builds are untested (addresses may match Steam's; try at your own
  risk and check the log).

## Building from source

Cross-compiles from Linux (or MSYS2) with mingw-w64 — no Windows SDK
needed:

```sh
cd client
./build.sh          # produces KenshiFP_x64.dll
```

Requires `x86_64-w64-mingw32-gcc` with `libdinput8`/`libdxguid`
(mingw-w64 ships both). [MinHook](https://github.com/TsudaKageyu/minhook)
(BSD-2-Clause) is vendored under `third_party/minhook/`.

## How it works

The DLL loads through Ogre's plugin path and detours the game's per-frame
update plus the camera update with MinHook. All game structures and
functions were located by reverse engineering the 1.0.68 binary with
headless Ghidra — the offset map and methodology live in `re/NOTES.md` and
`DESIGN.md`. Risky calls into game code run inside a vectored-exception
guard, so a bad pointer degrades a feature (with a log line) instead of
crashing the game.

Field-layout documentation from the
[KenshiLib](https://github.com/KenshiReclaimer/KenshiLib) headers was used
as a reference map during reverse engineering (function addresses there are
stale for 1.0.68; every address this mod calls was re-derived and verified
against the live binary).

## License

GPL-3.0 (see `LICENSE`). Vendored MinHook is BSD-2-Clause (see
`third_party/minhook/LICENSE.txt`).
