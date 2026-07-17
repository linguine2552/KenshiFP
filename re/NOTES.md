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

### 2026-07-17 (cont.) — CameraClass instance + layout NAILED
Chased the CameraClass ctor to its storage site:
- **CameraClass ctor = FUN_1406afbf0, RVA 0x6afbf0** (KenshiLib header 0x6AF410,
  stale). Decompile proves the header field layout is VALID for 1.0.68: it
  stores the Ogre::Camera* arg at this+0x68, builds SceneNode "camera_center"
  at this+0x58 and "camera_node" at this+0x70, and sets hand::vftable at
  this+0x28 (objectCurrentlyFollowing) and +0x80 (inBuilding). So yaw+0x18,
  pitch+0x1C, altitude+0x60, camera+0x68, center+0x58, node+0x70,
  freeCameraMode+0xBF are all trustworthy.
- Caller chain: FUN_140745490 calls setup FUN_14086cf90(&DAT_142133300); the
  setup fn stores scene ctx at holder+0x8 and the **CameraClass* at
  holder+0x10**. holder = 0x142133300, so:
  **CameraClass INSTANCE pointer = *(CameraClass**)(base + 0x2133310).**
- Resolves the earlier +0x58 conflict: DAT_142133308 (RVA 0x2133308) is NOT the
  CameraClass — it's the scene/render context (holder+0x8); its +0x58 is the
  Ogre::Camera used for getViewMatrix, its +0x60 a SceneManager. The real
  CameraClass is a distinct object at holder+0x10.
- Stage-0 client UPDATED to read the true instance (0x2133310) and cross-check
  that CameraClass.camera(+0x68) == sceneCtx.camera(+0x58). Rebuilt.

### 2026-07-17 — STAGE-0 VERIFIED IN-GAME (clean sweep, no crash, 70s+ stable)
Ran isolated (KenshiMP plugins temporarily disabled). Log:
scratchpad/KenshiFP_stage0_pass.log. All anchors confirmed:
- CameraClass instance @ RVA 0x2133310 = LIVE (cam=0x2108b80, stable). Decisive
  cross-check PASSED: CameraClass.camera(+0x68)==sceneCtx.camera(+0x58)
  (ogreCam 0x331c3c0 "MATCH" every tick) -> instance + field layout CONFIRMED.
  center(+0x58)=0x331a710, node(+0x70)=0x3324c90 both valid SceneNodes.
- Player char path: pc=0 in menu/loading, then pc=0x6d66c450 pos
  (-50978.8,1533.5,2932.5) after save load, via getPosition slot 8. CONFIRMED.
- **WASD via Win32 GetAsyncKeyState WORKS under Proton** (digits flip exactly
  with keys: A=0100, W+A=1100, W+D=1001, ...). So the engine `key` global is
  NOT needed for input reading — big simplification.
- F toggle works (FP ON/OFF per press). speedMult pinned 1.0. Hook stable.
- INCIDENTAL: gw fixed at 0x142134110 -> GameWorld object is statically located
  (RVA 0x2134110; this is what the `ou` global points to).
- OPEN (non-blocking): yaw(+0x18)/pitch(+0x1C)/altitude(+0x60) all read 0.000.
  Camera identity is proven via MATCH, so either the camera wasn't rotated or
  these rotation fields live/update elsewhere. Resolve during M2 (mouse-look):
  probe by rotating the camera in-game and watching which offsets change, or
  decompile CameraClass::rotate/rotationUpdate for the real field indices.

### 2026-07-17 — M1 camera-lock functions found (cluster enum before ctor)
Enumerated CameraClass methods in [0x6ad800,0x6af000) via ListAndDumpRange.java;
identified two by field-access signature:
- **followObject(const hand&) = RVA 0x6ae520.** ABI followObject(this /RCX/,
  const hand* /RDX/). Body copies the hand's 5 id dwords (arg+0x8..+0x18) into
  this->objectCurrentlyFollowing ids (this+0x30..+0x40) and zeroes the follow
  offset (this+0x48). Leaf setter, no alloc.
- **stopFollowing() = RVA 0x6ae560.** Resets this+0x30..+0x40 to the null-hand
  constants DAT_141e3a600.. (same ones the ctor used) and clears +0x48.
Character hand = Character+0x58 (ids at +0x60.. == hand+0x8), so
followObject(cam, char+0x58) locks the camera to that character. No separate
"isFollowing" bool exists — a valid followed hand == following; vanilla pan
likely calls stopFollowing, so M1 RE-ASSERTS followObject every frame while FP
on. (Nearby: FUN_1406ae960 @0x6ae960 calls Ogre::Camera vtable+0x108 with a
scaled global — a zoom/FOV setter, unused for now.)
M1 client writes followId to the log ([tick] followId=) to confirm the write.

### 2026-07-17 — M1 VERIFIED + M2 first-person built
M1 verified in-game: followId flips 0000000b (null-hand type=0xb) <-> 00000001
(followed) on F; camera locks onto/tracks the character; no crash. Confirms
followObject/stopFollowing and the whole write path.

CameraClass::update = **RVA 0x6b0f90** (found by branch on follow-hand type at
+0x30: ==0xb null, ==1 following, ==0x5b). Model learned:
- if freeCameraMode(+0xbf): delegates to FUN_1406b0960 (free/orbit) and returns.
- accumulates WASD-pan + rotate/tilt from DAT_142133xxx input flags.
- when following: resolves the object, getPosition, setPosition(center node
  +0x58) = character pos (+ offset). center = the look-at point.
- writes yaw(+0x18)=rot-delta, pitch(+0x1C)=rot-delta  <-- these are PER-FRAME
  DELTAS, ~0 at rest (explains the earlier 0.000 reading; NOT absolute angles).
- FUN_1406afee0 (0x6afee0) applies yaw/pitch to the camera NODE (+0x70) via
  Ogre::Node::yaw/pitch + DegreesToRadians (the tilt/orbit placement).
- cursor: GetCursorPos/SetCursorPos while rotating (drag-rotate).

M2 approach (built, awaiting test): Ogre is a SEPARATE DLL (OgreMain_x64.dll)
whose transform methods are EXPORTS — resolve by mangled name, no RVA needed:
  Node::_setDerivedPosition  ?_setDerivedPosition@Node@Ogre@@QEAAXAEBVVector3@2@@Z
  Node::_setDerivedOrientation ?_setDerivedOrientation@Node@Ogre@@QEAAXAEBVQuaternion@2@@Z
Each frame, AFTER the game's mainLoop (so after update() positioned the camera),
override the camera node (+0x70) WORLD transform: derived pos = charPos +
(0,EYE_HEIGHT,0); derived orientation = quat from mouse-look (v1: absolute
cursor pos -> yaw/pitch, read-only, no recenter). Ogre::Camera(+0x68) is
attached to that node so this drives the view. EYE_HEIGHT=1.7 provisional (tune).
OPEN for the test: does render pick up the override (timing vs update())? is the
look direction/handedness right? is EYE_HEIGHT sane? If the mainLoop-hook
timing loses to update(), escalate to a MinHook trampoline on update() 0x6b0f90.

### Still TODO for M1/M3 (find after boot-test confirms the instance)
- CameraClass::update RVA: instance global 0x2133310 has many readers; update
  takes `this` in RCX so update itself doesn't read the global — its per-frame
  CALLER does. Identify the caller among the 0x2133310 readers, or find
  followObject/rotate/teleport in the 0x6ADxxx–0x6B0xxx cluster (ctor moved
  header 0x6AF410 -> 0x6afbf0, ~+0x7e0, so the cluster shifted; not uniform).
  M1 camera lock can use CameraClass::followObject(hand) or write center-node
  pos each frame — both need the instance (have it) + one RVA.
- InputHandler `key` global: config method FUN_140362430 (0x362430) is
  vtable-dispatched (no call-xrefs). Best next anchor: whichever 0x2133310
  reader is CameraClass::update's caller also derefs `key` for the pan bools
  (+0xDB..0xDE). Only needed if Win32 GetAsyncKeyState proves unreliable under
  Proton — the stage-0 log's WASD field answers that.
- Character::setDestination RVA: no clean string (movement is Havok task-based:
  MoveTo_Addon, Task_MoveToDoor RTTI). Find via the right-click order path or
  by runtime-probing the Character vtable/methods once we have a live Character.
