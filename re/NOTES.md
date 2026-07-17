# KenshiFP RE notes — Kenshi 1.0.68 "Newland" x64

Ghidra project is SHARED with KenshiMP: `../KenshiMP/re/KenshiMP.gpr`
(already fully analyzed). Run queries like:

```sh
~/.local/opt/ghidra_12.1.2_PUBLIC/support/analyzeHeadless ../KenshiMP/re KenshiMP \
  -process kenshi_x64.exe -noanalysis \
  -scriptPath re/scripts -postScript <Script>.java
```

ImageBase 0x140000000. RVAs below are for 1.0.68 unless marked otherwise.

## Offset table

### CONFIRMED (inherited from KenshiMP, proven in-game on 1.0.68)
| What | Value |
|---|---|
| GameWorld::mainLoop_GPUSensitiveStuff (per-frame, main thread, GameWorld* this) | RVA 0x788a00 |
| GameWorld::player (PlayerInterface*) | +0x580 |
| PlayerInterface::playerCharacters (lektor<Character*>) | +0x2B0 |
| PlayerInterface::participant (Faction*) | +0x2A0 |
| lektor: count / stuff | +0x8 u32 / +0x10 T* |
| Character::getPosition | vtable slot 8 (+0x40), ABI `Vec3* (this RCX, Vec3* out RDX)` |
| setPause(GameWorld*, char) | RVA 0x787fb0 |
| setSpeed(uiCtx, float, char) | RVA 0x787e40 |

### FROM KENSHILIB HEADERS — field offsets (plausible, need one in-game sanity read)
CameraClass: yaw +0x18, pitch +0x1C, objectCurrentlyFollowing (hand) +0x28,
center SceneNode* +0x58, altitude +0x60, camera Ogre::Camera* +0x68,
node SceneNode* +0x70, inBuilding +0x80, currentFloor +0xBE,
freeCameraMode +0xBF.

InputHandler: controlEnabled +0xD0, ctrl/shift/alt +0xD8..0xDA,
up/down/left/right +0xDB..0xDE (WASD default-binds pan camera through these),
space +0xDF, mLeft/mRight +0xF3/0xF4, mPos Vec2 +0xFC, mPosAbs +0x104,
mSpeed Vec3 +0x10C, mWheel int +0x118. keyboard OIS::Keyboard* +0xA0.

Character: destination-state cluster ~+0x420 (_destinationInsideBuilding).

### STALE HEADER RVAs — need 1.0.68 derivation before ANY use
(KenshiLib RVAs proven wrong for this build in KenshiMP; listed only as
relative-layout hints — functions that were neighbors likely still cluster.)
- CameraClass::update(bool) [hdr 0x6B05E0], rotate(f,f) [0x6AF940],
  followObject [0x6ADDA0], stopFollowing [0x6ADDE0], teleport [0x6AF190],
  setZoomDist [0x6ADF40], move(Vec3) [0x6ADE30], setFreeCameraMode [0x6AF1E0]
- InputHandler::initialise [0x363210], addCommand [0x362F90/0x363130],
  keyDownEvent [0x360680]
- Character::setDestination(Vec3&, bool) [hdr 0x5C8E30]

## Anchor strategies (M0 work)
1. `strings` the exe for keybinding command names + camera option strings →
   exact needles.
2. Xref needles in Ghidra → InputHandler::addCommand call cluster (gives the
   InputHandler global) and options-read sites (gives CameraClass:
   updateOptionSettings neighborhood).
3. CameraClass::update identified by: reads InputHandler pan bools, reads/
   writes yaw(+0x18)/pitch(+0x1C), called once per frame from the main loop.
   Its caller yields the live CameraClass pointer (likely a GameWorld field or
   global).
4. setDestination: from the right-click order path, or decompile around the
   destination-state field writes (+0x420 cluster), or live-vtable dump from a
   Character* at runtime (KenshiMP pattern).

## Findings log

### 2026-07-17 — M0 recon (headless, shared KenshiMP Ghidra project)
Scripts: re/scripts/FindCameraAnchors.java, FindGlobals.java, DumpFns.java,
FindRefsToGlobal.java (all read-only, run against ../KenshiMP/re).

**Camera command strings (exact needles, verified in 1.0.68 exe via `strings`):**
`camera_forward` @RVA 0x16c3248, `camera_back` 0x16c3238, `camera_left`
0x16c3228, `camera_right` 0x16c3218, `camera_rotate_left` 0x16c31e0,
`camera_rotate_right` 0x16c31c8, `camera_zoom_in` 0x16c2e18, `camera_zoom_out`
0x16c2e00. Vanilla WASD default-binds camera_forward/back/left/right → these
set InputHandler.up/down/left/right (+0xDB..0xDE). Also `Camera Rotate Speed X`
@0x16cf050, `Camera Zoom Speed` @0x16cf018, `camera speed` @0x16ce428.

**Command-registration / config cluster:** FUN_140362430 (RVA 0x362430) — an
InputHandler method (`this`=param_1) that references controls.cfg and the camera
command strings; the addCommand/bind neighborhood (KenshiLib put
InputHandler::initialise 0x363210, addCommand 0x362F90/0x363130 nearby — cluster
confirmed present). Also FUN_1403636c0 (0x3636c0), FUN_1403f0260 (0x3f0260)
reference the same strings; FUN_1403eca90/1403e84f0 reference the option strings.
No direct call-xrefs to FUN_140362430 (likely vtable/initializer-dispatched), so
the InputHandler `key` global was NOT pinned this pass — TODO.

**GameWorld::mainLoop_GPUSensitiveStuff (RVA 0x788a00) decompiled — confirms:**
- param_1 = GameWorld*; `param_1 + 0x700` = frameSpeedMult (matches KenshiMP),
  `+0x580` = player (matches), `+0x8b9` = paused bool (header value; distinct
  from KenshiMP's other pause flag at +0x3D4 — both exist).
- Speed globals written here: DAT_142133798 (= dt * frameSpeedMult),
  DAT_142133794, DAT_142133790 (= raw dt).

**CAMERA GLOBAL — key M0 finding: `DAT_142133308` (RVA 0x2133308).**
Every frame mainLoop does `Ogre::Camera::getViewMatrix(*(Camera**)
(DAT_142133308 + 0x58), true)` → **+0x58 is the live Ogre::Camera***. The global
is a core singleton (100+ read xrefs), and one of its referrers, FUN_1406afbf0
(RVA 0x6afbf0), sits squarely in the CameraClass method cluster (KenshiLib
CameraClass RVAs were 0x6ADxxx–0x6B0xxx) → DAT_142133308 is very likely the
global `CameraClass*` (Kenshi's camera brain), not merely a raw Ogre camera.
CameraClass field offsets to validate against it in-game (KenshiLib header):
yaw +0x18, pitch +0x1C, objectCurrentlyFollowing(hand) +0x28, center +0x58(!),
altitude +0x60, camera(Ogre::Camera*) +0x68, node +0x70, freeCameraMode +0xBF.
NOTE conflict: header says Ogre::Camera at +0x68 but mainLoop reads +0x58 for
getViewMatrix, and +0x58 is header `center`(SceneNode*). Either 1.0.68 layout
shifted or DAT_142133308 is a different (camera-holder) type. **Stage-0 client
logs all these to resolve it empirically before we write anything.**

### Still TODO for M1/M3
- InputHandler `key` global address (for engine-side WASD instead of Win32).
- CameraClass::update RVA + which slot/global holds the CameraClass instance
  (confirm DAT_142133308 identity via the stage-0 log; else find via FUN_1406afbf0).
- Character::setDestination RVA (WASD breadcrumb order path).
- WASD camera-pan consumption site to suppress in FP mode.
