# KenshiFP — First-person mode for Kenshi

A native code mod that locks the camera to the selected character and replaces
Kenshi's RTS-style mouse movement with WASD + mouse-look.

Sibling project to [KenshiMP](../KenshiMP) and built with the same pipeline:

1. **RE**: headless Ghidra (12.1.2, `~/.local/opt`) against the shared analyzed
   project `../KenshiMP/re/KenshiMP.gpr` — string/xref queries, no debugger.
2. **Client**: a single C DLL (`KenshiFP_x64.dll`), mingw-built
   (`x86_64-w64-mingw32-gcc`), MinHook detours, imports only
   KERNEL32 + msvcrt so it's self-contained under Proton.
3. **Load**: Ogre plugin path — `Plugin=KenshiFP_x64` in `Plugins_x64.cfg`.
   No RE_Kenshi dependency.
4. **Verify**: in-game under Steam/Proton (Kenshi 1.0.68 x64, prefix
   compatdata/233860), log file written next to the exe.

See `DESIGN.md` for the architecture, engine surface map, and milestones.
See `re/NOTES.md` for the 1.0.68 offset table (the only offsets that may be
trusted; KenshiLib header RVAs are stale for this build).

## Layout

- `DESIGN.md` — goals, engine surface, control design, milestones
- `re/` — RE notes + Ghidra query scripts for the camera/input/movement systems
  (run against the KenshiMP Ghidra project; nothing is duplicated)
- `client/` — the DLL source

## Build & deploy

```sh
cd client
./build.sh                       # mingw + vendored MinHook -> KenshiFP_x64.dll
KENSHI="$HOME/.steam/debian-installation/steamapps/common/Kenshi"
cp KenshiFP_x64.dll "$KENSHI/"
# add a line to "$KENSHI/Plugins_x64.cfg":  Plugin=KenshiFP_x64
# launch Kenshi via Steam, press F in-game, then read the log:
find ~/.steam/debian-installation/steamapps/compatdata/233860/pfx -name KenshiFP.log
```

Coexists with `KenshiMP_x64.dll` (both MinHook `mainLoop`; chained detours are
fine). KenshiFP is fully local — no server, no Rust bridge.

## Status

- **Scaffold + M0 recon** — done (see `re/NOTES.md`). Camera command strings,
  the per-frame hook target, and the camera-holder global `DAT_142133308`
  (RVA 0x2133308, Ogre::Camera at +0x58) are identified.
- **Stage-0 client** — built (`client/KenshiFP_x64.dll`). Read-only: hooks the
  per-frame loop and logs GameWorld, the camera global, the first player
  character + position, and live WASD/toggle input. Validates every anchor
  in-game before any camera write. **Awaiting an in-game launch test.**
- Next: M1 camera lock (drive the camera to follow the selected character),
  then M2 mouse-look, then M3 WASD movement.
