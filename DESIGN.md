# KenshiFP — First-person mode for Kenshi

Lock the camera to the selected character and drive movement with WASD +
mouse-look, replacing the vanilla RTS-style click-to-move control scheme.

Sibling project to KenshiMP (`../KenshiMP`); uses the **same proven method**:
headless Ghidra string/xref RE → mingw full-replacement or MinHook detour DLL →
Ogre plugin-path load (`Plugins_x64.cfg`) → in-game verify. Target binary is
Steam Kenshi **1.0.68 "Newland" x64** under Proton (prefix compatdata/233860,
base 0x140000000, install maps to drive S: in-process).

## What the engine already gives us

From the KenshiLib headers (`../KenshiMP/reference/KenshiLib_Examples_deps/`).
**Function RVAs in these headers are STALE for 1.0.68** (proven in KenshiMP);
struct/field offsets have so far proven RIGHT (frameSpeedMult +0x700, faction
offsets, getPosition vtable +0x40 all held). Every function below needs its
1.0.68 RVA derived in Ghidra before use; every field offset needs one in-game
sanity read before trust.

### CameraClass (kenshi/CameraClass.h) — the whole camera brain
- `update(bool controlEnabled)` — per-frame camera logic. **Primary detour
  candidate**: when FP mode is on, run our logic instead of (or after) the
  original.
- `followObject(const hand&)` / `stopFollowing()` / `getFollowObject()` —
  engine-native "camera tracks this object". M1 shortcut.
- `rotate(float yaw, float pitch)`, `manuallySetOrientationAndZoom(quat, zoom)`,
  `setZoomDist(float)`, `teleport(pos)`, `toGround(bool)`.
- Fields: `yaw` +0x18, `pitch` +0x1C, `objectCurrentlyFollowing` (hand) +0x28,
  `center` (SceneNode*) +0x58, `altitude` +0x60, `camera` (Ogre::Camera*)
  +0x68, `node` (SceneNode*) +0x70, `inBuilding` +0x80, `currentFloor` +0xBE,
  `freeCameraMode` +0xBF.
- Free-camera mode already exists (`isFreeCameraMode`/`setFreeCameraMode`/
  `updateFreeCamera`) — evidence the update loop already branches on camera
  modes; FP is "one more mode".

### InputHandler (kenshi/InputHandler.h) — key/mouse state, already polled
- Vanilla WASD default-binds to camera pan → the state bools `up` +0xDB,
  `down` +0xDC, `left` +0xDD, `right` +0xDE are ALREADY set by WASD. We read
  those four bools for FP movement and suppress their vanilla camera-pan
  consumption — no OIS hooking needed for v1.
- Mouse: `mPos` +0xFC, `mPosAbs` +0x104, `mSpeed` (Vector3) +0x10C — mSpeed is
  the per-frame delta candidate for mouse-look. `mWheel` +0x118.
- `controlEnabled` +0xD0, modifier bools ctrl/shift/alt +0xD8..0xDA.
- Commands are registered by NAME STRINGS (`addCommand("...")`) and persisted
  in the keybindings config — those strings are our Ghidra anchors for both
  InputHandler’s global instance and the camera-pan consumption sites.

### Movement (kenshi/Character.h, kenshi/CharMovement.h)
- `Character::setDestination(const Ogre::Vector3& pos, bool shift)` — THE
  mouse-click move order. WASD v1 = call this per tick with
  `charPos + camYawDir * step`.
- `AbstractMovementBase` vtable: `faceDirection` +0x30, `lookatPosition`
  +0x38, `getPosition` +0x40 (**confirmed live in 1.0.68** — KenshiMP calls
  it every frame), `_setPositionSimple` +0x28, `getDestination`.
- `Character::getMovementSpeedOrders()` (vtable +0x2C8) — walk/run/sneak
  interplay; shift arg on setDestination likely toggles run.

### Confirmed 1.0.68 anchors inherited from KenshiMP (re/NOTES.md there)
- `GameWorld::mainLoop_GPUSensitiveStuff` RVA **0x788a00** (sole GameWorld
  virtual, once per frame, main thread) — MinHook trampoline proven in-game.
  This is our per-frame tick with a live GameWorld*.
- `GameWorld::player` +0x580 (PlayerInterface*), `PlayerInterface::
  playerCharacters` lektor +0x2B0, `participant` +0x2A0.
- lektor layout: +0x8 count u32, +0x10 stuff T*.
- `setPause` RVA 0x787fb0, `setSpeed` 0x787e40 (useful hook points for
  key-triggered debug, proven pattern).
- Character::getPosition = vtable slot 8, ABI `Vec3* getPos(this RCX, out RDX)`.
  NEVER trust header vtable slots without reading the live vtable (slot-0
  lesson: it was the deleting destructor).

## What we must find in Ghidra (M0 offset table)

| Target | Anchor strategy |
|---|---|
| CameraClass::update | xref from mainLoop / camera-option strings; it reads InputHandler bools + yaw/pitch fields |
| CameraClass instance (global or GameWorld field) | whoever calls update each frame passes `this` |
| CameraClass::rotate / followObject / stopFollowing / setZoomDist | cluster near update in the binary; identify by field writes (+0x18 yaw, +0x1C pitch, +0x28 follow hand) |
| InputHandler instance | xrefs from keybinding command-name strings (`strings` the exe first for exact needles) |
| Character::setDestination | xref from right-click order dispatch; writes destination state (+0x420 area); or vtable walk from a live Character* |
| WASD camera-pan consumption site | reads InputHandler +0xDB..0xDE — this is what we suppress in FP mode |

## Control design

- **Toggle key** enters/leaves FP mode (candidate: reuse an inert-in-KenshiFP
  key or add our own via GetAsyncKeyState in the frame tick — zero engine RE).
- FP requires exactly one selected/first player character
  (playerCharacters[0] for v1).
- **Camera**: v1 chase-cam via engine `followObject` + forced small zoom;
  v2 true FP: detour `update()`, place `node` at character position + eye
  height, feed `yaw`/`pitch` from mouse deltas, skip the original's
  ground-collision/zoom logic while in FP.
- **Movement**: v1 "breadcrumb" scheme — every tick (~100–200 ms) while any
  WASD bool is held, `setDestination(charPos + dir_rel_to_camera_yaw * ~4m,
  shift=run)`; on release issue a stop (setDestination(charPos) or the stop
  order once found). Engine keeps pathfinding, collision, animation — lowest
  risk. v2 (if v1 feels laggy): direct locomotion via CharMovement
  faceDirection + drive, more RE.
- **Mouse-look**: read InputHandler.mSpeed (or raw deltas) in our frame tick →
  write CameraClass yaw/pitch (or call rotate()); recenter/capture cursor with
  Win32 (ClipCursor/SetCursorPos under Wine — verify under Proton).
- Vanilla behaviors to suppress while FP on: WASD camera pan, edge panning,
  mouse-click move orders (optional at first), zoom wheel.

## Milestones

- **M0** — scaffold (done) + 1.0.68 offset table for the targets above
  (headless Ghidra against the shared analyzed project in `../KenshiMP/re`).
- **M1** — camera lock: chase-cam follows selected character (followObject +
  zoom clamp), toggle key, verified in-game.
- **M2** — true first person: update() detour, eye-height placement,
  mouse-look, cursor capture.
- **M3** — WASD movement via breadcrumb setDestination + vanilla pan
  suppression. This is the "playable FP" milestone.
- **M4** — polish: hide own head/body mesh or tune near-clip, indoor floors
  (`currentFloor`/`inBuilding`), combat camera behavior, walk/run toggle,
  interaction fallback (temporarily drop to vanilla cursor mode).

## Delivery / coexistence

- Single C DLL `KenshiFP_x64.dll` (mingw `x86_64-w64-mingw32-gcc`, MinHook
  from `../KenshiMP/reference/minhook`), registered as `Plugin=KenshiFP_x64`
  in `Plugins_x64.cfg`. No server, no Rust bridge — this mod is fully local.
- Can coexist with KenshiMP_x64.dll: both MinHook the mainLoop; chained
  detours are fine (order undefined but both run).
- KenshiFP shares only OUR OWN code with KenshiMP (skeleton, logging, hook
  patterns) — no KenshiCoop-derived code, so the AGPL obligation from
  KenshiMP's reference clone does not bind this repo. License: GPL-3.0 (or
  looser) — decide before publishing.
