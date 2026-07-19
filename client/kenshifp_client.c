/*
 * KenshiFP client DLL — first-person mode for Kenshi 1.0.68 "Newland" x64.
 *
 * M1 (this file): CAMERA LOCK. Builds on the verified stage-0 read path. While
 * FP mode is ON (toggle: F), each frame re-asserts CameraClass::followObject on
 * the selected player character so the camera tracks it (a chase-cam lock);
 * on toggle OFF, calls stopFollowing once to release. Everything else is still
 * read-only observation, logged at 1 Hz.
 *
 * Verified stage-0 anchors it stands on (re/NOTES.md):
 *   - per-frame MinHook trampoline on GameWorld::mainLoop_GPUSensitiveStuff,
 *   - CameraClass instance at *(base + 0x2133310) (camera+0x68 == scene cam),
 *   - player char via GameWorld->player->playerCharacters[0] + getPosition,
 *   - WASD/toggle via Win32 GetAsyncKeyState (works under Proton).
 * New M1 writes: followObject (0x6ae520) / stopFollowing (0x6ae560), both tiny
 * leaf setters (no allocation) — safe to call from the main-thread hook.
 *
 * Loaded via Plugins_x64.cfg (Plugin=KenshiFP_x64), no RE_Kenshi.
 * All offsets: see ../re/NOTES.md.
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <setjmp.h>
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>
#include "MinHook.h"

/* ---- per-frame hook (proven in KenshiMP, same binary) ---- */
#define RVA_MAINLOOP      0x788a00u  /* GameWorld::mainLoop_GPUSensitiveStuff(GameWorld*, float) */
#define GW_FRAMESPEED_OFF 0x700      /* GameWorld::frameSpeedMult (float) */
#define GW_PLAYER         0x580      /* GameWorld::player (PlayerInterface*) */

/* ---- camera (M0, re/NOTES.md — confirmed via the CameraClass ctor) ----
 * The global at RVA 0x2133300 is the camera/scene holder struct; the engine
 * fills it in FUN_14086cf90(&holder):
 *   holder+0x08 -> scene/render context (DAT_142133308; its +0x58 is the
 *                  Ogre::Camera used for getViewMatrix each frame),
 *   holder+0x10 -> the CameraClass* (Kenshi's camera brain).
 * So the CameraClass INSTANCE pointer lives at RVA 0x2133310. The ctor
 * (FUN_1406afbf0, RVA 0x6afbf0) proved the header field layout is valid for
 * 1.0.68: camera(Ogre::Camera*)+0x68, center(SceneNode*)+0x58, node+0x70,
 * objectCurrentlyFollowing(hand)+0x28. */
#define RVA_CAM_INSTANCE  0x2133310u /* *(CameraClass**)(base + this) */
/* InputHandler is statically located (its pan bools at +0xDB..0xDE line up with
 * the camera update's DAT_14213344b..e -> base RVA 0x2133370). controlEnabled
 * (+0xD0, RVA 0x2133440) is FALSE while a dialogue/menu grabs input -> release
 * the FP cursor so the player can click UI. */
#define RVA_INPUT_CONTROLENABLED 0x2133440u
#define RVA_INPUT_MWHEEL         0x2133488u /* InputHandler.mWheel (+0x118); OIS wheel delta */
#define RVA_SCENE_CTX     0x2133308u /* scene/render context; +0x58 Ogre::Camera* */
#define SCENE_OGRE_CAMERA 0x58
/* CameraClass field offsets (KenshiLib header, ctor-confirmed layout): */
#define CC_YAW            0x18
#define CC_PITCH          0x1C
#define CC_FOLLOW_HAND    0x28       /* hand objectCurrentlyFollowing */
#define CC_FOLLOW_IDS     0x30       /* the hand's 5 id dwords (what followObject writes) */
#define CC_CENTER         0x58       /* Ogre::SceneNode* center */
#define CC_CAMERA         0x68       /* Ogre::Camera* */
#define CC_NODE           0x70       /* Ogre::SceneNode* */
#define CC_ALTITUDE       0x60
#define CC_FREECAM        0xBF

/* CameraClass methods (M1 writes; RVAs derived in ../re/NOTES.md).
 *   followObject(this /RCX/, const hand* object /RDX/): copies object's 5 id
 *     dwords (object+0x8..+0x18) into this->objectCurrentlyFollowing (+0x30..).
 *   stopFollowing(this /RCX/): resets those ids to the null-hand constants. */
#define RVA_FOLLOW_OBJECT 0x6ae520u
#define RVA_STOP_FOLLOW   0x6ae560u
/* Character::handle (a `hand`) at Character+0x58; ids at +0x60.. (=hand+0x8). */
#define CHAR_HANDLE       0x58

/* ---- M3 WASD movement ----
 * Character->movement (CharMovement*) is at +0x640. CharMovement::setDestination
 * (const Vec3&, UpdatePriority, bool) VERIFIED at RVA 0x661270 (vt[18]/+0x90 of
 * the live CharMovement vtable; Ghidra typed arg2 as Vector3*). UpdatePriority:
 * LOW=0, MED=1, HIGH=2. We breadcrumb a point ahead in the camera heading while
 * WASD is held (~10 Hz), and order the current position on release (halt).
 * NOTE: KenshiCoop found player chars can ignore a bare CharMovement dest; if so
 * this won't move them and we'll switch to the Character-level order path. */
#define CHAR_MOVEMENT      0x640   /* Character::movement (CharMovement*) */
#define RVA_CHARMOVE_SETDEST 0x661270u   /* CharMovement::setDestination (raw move; no navmesh) */
#define RVA_CHAR_SETDEST     0x5c84e0u   /* NOT setDestination: teleport+facing placement
                                          * (3rd arg = Ogre::Quaternion* facing). Unused. */
#define RVA_MOVE_TO_POS      0x5d22b0u   /* Character::moveToPosition (vtable +0x318): the REAL
                                          * right-click walk path. Issues/live-updates a
                                          * Task_Move(0x1d) -- pathfinds, animates, and per-frame
                                          * calls ARE the game's drag-move ("hold right-click"). */
#define CHAR_TASKHOLDER      0x648       /* Character+0x648 -> task holder */
#define TASK_CUR             0x68        /* holder+0x68 = current task */
#define TASK_DESC            0x70        /* task+0x70 = descriptor */
#define TASKDESC_TYPE        0x44        /* descriptor+0x44 = order type id */
#define TASK_MOVE_ID         0x1d        /* Task_Move (player walk order) */
/* --- UI-panel-open detection (decompile-verified 1.0.68). ForgottenGUI is a
 * static INSTANCE at 0x21337b0; other window singletons are static pointers.
 * Everything below is a pure read or a tiny getVisible (widget flag read). --- */
#define RVA_GUI_INV_COUNT   0x2133870u  /* gui+0xC0: inventoryWindowsOpen map SIZE --
                                         * covers inventory, loot, trade, animal,
                                         * research inventories */
#define RVA_GUI_STATS_BEG   0x2133978u  /* gui+0x1C8: stats-windows vector begin */
#define RVA_GUI_STATS_END   0x2133980u  /* gui+0x1D0: vector end */
#define RVA_GUI_WINSTACK    0x21339c0u  /* gui+0x210: open-window stack count */
#define RVA_GUI_DLGWND      0x21337d0u  /* gui+0x20: DialogueWindow* */
#define RVA_DLG_GETVIS      0x721ec0u
#define RVA_ESCMENU_PTR     0x212f4b8u  /* EscMenu singleton */
#define RVA_ESC_GETVIS      0x913040u
#define RVA_OVERVIEW_PTR    0x212f4f8u  /* OverviewWindow: map/factions/squads/etc */
#define RVA_OVW_GETVIS      0x48bb70u
#define RVA_OPTIONS_PTR     0x212f090u  /* OptionsWindow */
#define RVA_OPT_GETVIS      0x3e7240u
#define RVA_PROSPECT_PTR    0x212ea60u  /* ProspectingWindow */
#define RVA_PRO_GETVIS      0x48bf50u
#define RVA_SAVELOAD_PTR    0x212ebd8u  /* save/load dialogs: widget slots +0x100.. */
#define RVA_MSGBOX_COUNT    0x1f29a30u  /* modal message boxes open */
#define RVA_PLAYER_IFACE    0x2134690u  /* PlayerInterface* (GameWorld+0x580, verified) */
#define RVA_CTXMENU_VISIBLE 0x21332d0u  /* game's own per-frame cache byte of
                                         * ContextMenu::isVisible (written each frame
                                         * by the mouse handler; decompile-verified).
                                         * The REAL check = menuGUI->mMainWidget->
                                         * getVisible (widget's OWN flag -- inherited
                                         * visibility never updates, object lifetime
                                         * never ends; both earlier heuristics wedged). */
#define UIMASK_OVERVIEW     0x080       /* fullscreen map/factions/squads screen */
#define RVA_CAM_UPDATE       0x6b0f90u   /* CameraClass::update(bool controlEnabled) -- verified
                                          * M1. Hooked so the FP eye is re-asserted MID-frame:
                                          * the game's follow camera lags 30-100u behind the eye
                                          * while moving, and foliage paging + mesh LOD read that
                                          * stale position right after update() -> flicker. */
#define REISSUE_DIST         1.0f        /* re-issue the move order only when the target moved
                                          * this far -- per-frame re-issue restarts the path
                                          * (stutter) and was the earlier crash cause. */
#define RVA_TERRAIN_PTR      0x2133318u  /* Terrain* singleton (holder+0x18, same setup struct
                                          * as the camera globals). Class lives in
                                          * Plugin_Terrain_x64.dll. */
#define TERRAIN_GETHEIGHT_SYM "?getHeight@Terrain@@QEBA?AUHit@1@AEBVVector3@Ogre@@@Z"
#define TERRAIN_INTERSECT_SYM "?intersect@Terrain@@QEBA?AUHit@1@AEBVRay@Ogre@@@Z"

/* --- Building-interior preload (FP QoL): show interiors when NEAR, not only
 * when inside. All RVAs decompile-verified for 1.0.68. --- */
#define RVA_TOWNMGR_PTR    0x2134100u  /* TownManager* global */
#define RVA_NEAREST_TOWN   0x928890u   /* Town* fn(TownManager*, const Vec3*, int 0) */
#define RVA_INTERIOR_LOAD  0x562540u   /* void fn(BuildingInterior*): idempotent lazy
                                        * graphics load + keep-alive refresh; the fn the
                                        * game itself calls per visible interior. Does
                                        * NOT cut the roof/shell away. */
#define TOWN_GETLIST_VTOFF (0x2d8 / 8) /* Town vtable slot: lektor* fn(Town*, int, void*) */
#define BLDG_LIST_TYPE     0x13        /* list id: buildings with interior layouts */
#define BLDG_POS_X         0x48        /* Building world pos floats +0x48/+0x4C/+0x50 */
#define BLDG_INTERIOR      0x1F0       /* Building::myInterior (BuildingInterior*) */
#define BLDG_DESTROYED     0x1A1       /* Building::destroyed (bool) -- ruins */
#define INTERIOR_RADIUS    400.0f      /* preload interiors within this range (~40 m) */
#define UPDATE_PRIORITY_HIGH 2

/* ---- custom FP character controller (motion drive, NO move orders) ----
 * Re-issuing destinations fought the order system (path/"official position"
 * bookkeeping resets are why the camera lagged while a plain right-click was
 * perfect). Instead we drive CharMovement's motion state directly each frame —
 * the same technique KenshiCoop's applyMotion uses to animate proxy bodies —
 * and rotate the character to the input direction with faceDirection.
 * CharMovement fields (KenshiLib layout, KenshiCoop-proven):   */
#define MV_CURRENTLY_MOVING 0x24   /* bool  */
#define MV_CURRENT_MOTION   0xA8   /* Ogre::Vector3 (velocity) */
#define MV_MAX_SPEED        0xB4   /* float */
#define MV_CURRENT_SPEED    0xB8   /* float */
#define MV_DESIRED_SPEED    0xBC   /* float (keep == current so accel logic doesn't idle us) */
#define MV_WALK_SPEED       0xC0   /* float */
#define MV_FACEDIR_VTOFF    6      /* faceDirection = live vtable slot 6 (+0x30) */
#define MV_MANUALMOVE_VTOFF 14     /* manualMovement(desiredMotion) = slot 14 (+0x70);
                                    * engine-driven velocity move (combat-strafe path):
                                    * ground-clamps + animates + NO auto-turn */
/* CharMovement OVERRIDES the position setters at higher slots than the
 * AbstractMovementBase ones (+0x20/0x28 are the base-class members — calling
 * slot 5 did nothing). The real overrides (header +0xB8/0xC0/0xC8):
 *   slot 23 _setPositionAndTeleport(const Vec3&, int floor)  <- the one KenshiCoop
 *   slot 24 _setPositionDirectionAndTeleport(Vec3&, Quat&)      proves moves the
 *   slot 25 _setPositionSimple(const Vec3&)                     physics proxy too */
#define MV_SETPOSTELE_VTOFF 23
/* Orient-to-direction movement: setDestination makes the character turn to face
 * the target and walk (no strafing). We aim FAR ahead so it commits to a full-
 * speed walk, and only RE-ISSUE when the heading or key set changes (or a slow
 * refresh) — re-issuing a near target every frame made it re-path and creep. */
/* Distance-controls-speed: the move target's distance is set by look pitch, the
 * way right-clicking near your feet walks slowly and clicking far runs. Kenshi
 * accelerates toward farther orders. Look DOWN (pitch +) -> near/slow; look UP
 * (pitch -) -> far/run. */
#define MOVE_NEAR          10.0f   /* target distance looking straight down (slow) */
#define MOVE_FAR           350.0f  /* target distance looking up (full run) */
#define MOVE_RETARGET_FRAC 0.4f    /* re-issue once the char has closed to this fraction of the target */
#define MOVE_KEEPALIVE_MS  2000    /* safety re-issue if nothing else triggers */
#define MOVE_TURN_EPS      0.12f   /* radians of heading change that forces a re-issue */
#define EYE_DROP           (-1.5f) /* offset from the head bone Y (negative = raise); tune */
#define FP_EYE_FORWARD     2.0f    /* move the eye forward (toward look) out of the head; tune */

/* ---- FOV ----
 * Ogre::Frustum::setFOVy(const Radian&) [Camera inherits it]; Radian == {float}.
 * getFOVy returns a const ref (pointer in RAX -> safe ABI, unlike the by-value
 * get* accessors). We capture the default FOV once, force FP_FOV while in first
 * person, and restore on exit. */
#define OGRE_SETFOVY_SYM  "?setFOVy@Frustum@Ogre@@UEAAXAEBVRadian@2@@Z"
#define OGRE_GETFOVY_SYM  "?getFOVy@Frustum@Ogre@@UEBAAEBVRadian@2@XZ"
#define FP_FOV_DEG        70.0f    /* vertical FOV while first-person; tune */
#define DEG2RAD           0.01745329f

/* ---- near clip ----
 * Kenshi's RTS camera sits far from everything, so its near clip is large; at
 * eye level that clips nearby geometry and you see through walls/meshes. Pull it
 * in while first-person. Frustum::setNearClipDistance(float) / getNearClipDistance
 * (returns Real=float by value -> XMM0, safe scalar ABI). Kenshi is ~10 units/m. */
#define OGRE_SETNEARCLIP_SYM "?setNearClipDistance@Frustum@Ogre@@UEAAXM@Z"
#define OGRE_GETNEARCLIP_SYM "?getNearClipDistance@Frustum@Ogre@@UEBAMXZ"
#define FP_NEARCLIP       1.0f     /* world units (~10 cm); clip very-near geometry
                                    * (own head/hair edges) a tiny bit; tune */

/* ---- MyGUI cursor (hide the default arrow, keep contextual sword/speech) ----
 * The visible cursor is a MyGUI sprite (ShowCursor can't touch it). MyGUI is a
 * separate DLL with PointerManager exports. We hook setPointer(const string&) —
 * Kenshi calls it when the cursor type changes — and, while FP is on, hide the
 * cursor when the new pointer == the default (arrow) and show it otherwise. */
#define MYGUI_DLL         "MyGUIEngine_x64.dll"
#define MYGUI_SETPOINTER_SYM  "?setPointer@PointerManager@MyGUI@@QEAAXAEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@@Z"
#define MYGUI_SETVISIBLE_SYM  "?setVisible@PointerManager@MyGUI@@QEAAX_N@Z"
#define MYGUI_GETDEFAULT_SYM  "?getDefaultPointer@PointerManager@MyGUI@@QEBAAEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@XZ"
#define MYGUI_GETINSTANCE_SYM "?getInstancePtr@?$Singleton@VPointerManager@MyGUI@@@MyGUI@@SAPEAVPointerManager@2@XZ"

/* ---- crosshair (replace the hidden default cursor with our image) ----
 * Create a MyGUI ImageBox at screen center showing crosshair.png (deployed to
 * <install>/kenshifp/, registered as an Ogre resource location). Show it while
 * the pointer is the default (gameplay, looking at nothing); the contextual
 * sword/speech cursors still render via the normal (un-hidden) pointer. */
#define MYGUI_GUI_GETINSTANCE_SYM "?getInstancePtr@?$Singleton@VGui@MyGUI@@@MyGUI@@SAPEAVGui@2@XZ"
#define MYGUI_CREATEWIDGET_SYM "?createWidgetT@Gui@MyGUI@@QEAAPEAVWidget@2@AEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@0HHHHUAlign@2@00@Z"
#define MYGUI_SETIMAGETEX_SYM  "?setImageTexture@ImageBox@MyGUI@@QEAAXAEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@@Z"
#define MYGUI_WIDGET_INHVIS_SYM "?getInheritedVisible@Widget@MyGUI@@QEBA_NXZ"
#define MYGUI_WIDGET_SETVIS_SYM "?setVisible@Widget@MyGUI@@UEAAX_N@Z"
#define OGRE_RGM_GETSINGLETON_SYM "?getSingletonPtr@ResourceGroupManager@Ogre@@SAPEAV12@XZ"
#define OGRE_RGM_ADDLOCATION_SYM  "?addResourceLocation@ResourceGroupManager@Ogre@@QEAAXAEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@00_N1@Z"
#define OGRE_RGM_CREATEGROUP_SYM  "?createResourceGroup@ResourceGroupManager@Ogre@@QEAAXAEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@_N@Z"
#define OGRE_RGM_INITGROUP_SYM    "?initialiseResourceGroup@ResourceGroupManager@Ogre@@QEAAXAEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@@Z"
#define CROSSHAIR_SIZE    80         /* px on screen; tune */

/* ---- M2 first-person (Ogre node override) ----
 * Scene graph (from the ctor): root -> center(+0x58) -> node(+0x70) -> camera
 * (+0x68 attached). CameraClass::update keeps `center` at the character and
 * offsets `node` (local, relative to center) up-and-back for 3rd person. For FP
 * we override `node`'s LOCAL transform every frame after update() ran: a small
 * eye-height offset above center + a mouse-look orientation. LOCAL (not world)
 * so it's independent of Kenshi's world coordinate space — setting a *derived*
 * (world) position to the global char coords fought the hierarchy and flung the
 * camera away. OgreMain_x64.dll exports, resolved by mangled name:
 *   Node::setPosition(const Vector3&)     [local]
 *   Node::setOrientation(Quaternion)      [local; 16B struct -> passed by ptr] */
#define OGRE_SETPOS_SYM   "?setPosition@Node@Ogre@@QEAAXAEBVVector3@2@@Z"
#define OGRE_SETORI_SYM   "?setOrientation@Node@Ogre@@QEAAXVQuaternion@2@@Z"
/* World-space transform: set camera node's DERIVED pos/orientation directly, so
 * the parent `center` node's transform can't induce dip or roll. Need center's
 * world position to place the eye = centerWorld + (head-feet) offset. */
#define OGRE_SETDPOS_SYM  "?_setDerivedPosition@Node@Ogre@@QEAAXAEBVVector3@2@@Z"
#define OGRE_GETDPOS_SYM  "?_getDerivedPosition@Node@Ogre@@QEBA?AVVector3@2@XZ"

/* ---- head-bone tracking (real first-person feel: eye = head bone) ----
 * Character::getBoneWorldPosition(this, retVec3, std::string* name) — RVA
 * 0x440360 (confirmed: falls back to getPosition/vtable+0x40 when the bone is
 * absent). Returns the bone position in the SAME game-world frame as
 * getPosition. So we take (head - feet) as a world-axis offset and apply it as
 * the camera node's LOCAL position above `center` (which the engine keeps at
 * the character in Ogre space). This tracks animation AND ragdoll (the head
 * bone follows the physics), and removes the eye-height guesswork/into-ground
 * dip. MEMBER-STRUCT-RETURN ABI: this=RCX, retbuf=RDX, args=R8 (per KenshiCoop;
 * same pattern as the working getPosition — NOT retbuf-first). */
#define RVA_GET_BONE_WORLD 0x440360u

/* ===== version-dependent address tables ===================================
 * KenshiFP's game addresses are build-specific. Steam 1.0.68 is the base
 * target; RE_Kenshi downgrades installs to Steam 1.0.65, so we ship both and
 * pick at load by the mainLoop prologue signature. The RVA_* macros above hold
 * the 1.0.68 values and seed T_1068; below they are redefined to read the
 * active table (g_rva), so all use-sites switch automatically. */
typedef struct {
    uintptr_t MAINLOOP, CAM_INSTANCE, INPUT_CONTROLENABLED, INPUT_MWHEEL, SCENE_CTX,
        FOLLOW_OBJECT, STOP_FOLLOW, CHARMOVE_SETDEST, CHAR_SETDEST, MOVE_TO_POS,
        GUI_INV_COUNT, GUI_STATS_BEG, GUI_STATS_END, GUI_WINSTACK, GUI_DLGWND,
        DLG_GETVIS, ESCMENU_PTR, ESC_GETVIS, OVERVIEW_PTR, OVW_GETVIS,
        OPTIONS_PTR, OPT_GETVIS, PROSPECT_PTR, PRO_GETVIS, SAVELOAD_PTR,
        MSGBOX_COUNT, PLAYER_IFACE, CTXMENU_VISIBLE, CAM_UPDATE, TERRAIN_PTR,
        TOWNMGR_PTR, NEAREST_TOWN, INTERIOR_LOAD, GET_BONE_WORLD;
} addr_table_t;

static const addr_table_t T_1068 = {
    RVA_MAINLOOP, RVA_CAM_INSTANCE, RVA_INPUT_CONTROLENABLED, RVA_INPUT_MWHEEL, RVA_SCENE_CTX,
    RVA_FOLLOW_OBJECT, RVA_STOP_FOLLOW, RVA_CHARMOVE_SETDEST, RVA_CHAR_SETDEST, RVA_MOVE_TO_POS,
    RVA_GUI_INV_COUNT, RVA_GUI_STATS_BEG, RVA_GUI_STATS_END, RVA_GUI_WINSTACK, RVA_GUI_DLGWND,
    RVA_DLG_GETVIS, RVA_ESCMENU_PTR, RVA_ESC_GETVIS, RVA_OVERVIEW_PTR, RVA_OVW_GETVIS,
    RVA_OPTIONS_PTR, RVA_OPT_GETVIS, RVA_PROSPECT_PTR, RVA_PRO_GETVIS, RVA_SAVELOAD_PTR,
    RVA_MSGBOX_COUNT, RVA_PLAYER_IFACE, RVA_CTXMENU_VISIBLE, RVA_CAM_UPDATE, RVA_TERRAIN_PTR,
    RVA_TOWNMGR_PTR, RVA_NEAREST_TOWN, RVA_INTERIOR_LOAD, RVA_GET_BONE_WORLD,
};
/* Steam 1.0.65 (RE_Kenshi's downgrade build). Signature-transplant mapped;
 * 0 = TODO (Ghidra pass in progress) or unused-in-client. Same field order. */
static const addr_table_t T_1065 = {
    0x787e70, 0x21322c0, 0x21323f0, 0, 0x21322b8,
    0x6aed00, 0x6aed40, 0x6607e0, 0, 0x5d1820,
    0x2132810, 0x2132918, 0x2132920, 0x2132960, 0x2132770,
    0 /*DLG_GETVIS*/, 0 /*ESCMENU_PTR*/, 0 /*ESC_GETVIS*/, 0x212e4e8, 0 /*OVW_GETVIS*/,
    0 /*OPTIONS_PTR*/, 0 /*OPT_GETVIS*/, 0 /*PROSPECT_PTR*/, 0 /*PRO_GETVIS*/, 0x212dbc8,
    0x1f28a20, 0x2133630, 0 /*CTXMENU_VISIBLE*/, 0x6b1540, 0x21322c8,
    0x21330a0, 0x9279c0, 0x561ab0, 0x43ffc0,
};
static addr_table_t g_rva;   /* active table, selected at load by build signature */

#undef RVA_MAINLOOP
#undef RVA_CAM_INSTANCE
#undef RVA_INPUT_CONTROLENABLED
#undef RVA_INPUT_MWHEEL
#undef RVA_SCENE_CTX
#undef RVA_FOLLOW_OBJECT
#undef RVA_STOP_FOLLOW
#undef RVA_CHARMOVE_SETDEST
#undef RVA_CHAR_SETDEST
#undef RVA_MOVE_TO_POS
#undef RVA_GUI_INV_COUNT
#undef RVA_GUI_STATS_BEG
#undef RVA_GUI_STATS_END
#undef RVA_GUI_WINSTACK
#undef RVA_GUI_DLGWND
#undef RVA_DLG_GETVIS
#undef RVA_ESCMENU_PTR
#undef RVA_ESC_GETVIS
#undef RVA_OVERVIEW_PTR
#undef RVA_OVW_GETVIS
#undef RVA_OPTIONS_PTR
#undef RVA_OPT_GETVIS
#undef RVA_PROSPECT_PTR
#undef RVA_PRO_GETVIS
#undef RVA_SAVELOAD_PTR
#undef RVA_MSGBOX_COUNT
#undef RVA_PLAYER_IFACE
#undef RVA_CTXMENU_VISIBLE
#undef RVA_CAM_UPDATE
#undef RVA_TERRAIN_PTR
#undef RVA_TOWNMGR_PTR
#undef RVA_NEAREST_TOWN
#undef RVA_INTERIOR_LOAD
#undef RVA_GET_BONE_WORLD
#define RVA_MAINLOOP            (g_rva.MAINLOOP)
#define RVA_CAM_INSTANCE        (g_rva.CAM_INSTANCE)
#define RVA_INPUT_CONTROLENABLED (g_rva.INPUT_CONTROLENABLED)
#define RVA_INPUT_MWHEEL        (g_rva.INPUT_MWHEEL)
#define RVA_SCENE_CTX           (g_rva.SCENE_CTX)
#define RVA_FOLLOW_OBJECT       (g_rva.FOLLOW_OBJECT)
#define RVA_STOP_FOLLOW         (g_rva.STOP_FOLLOW)
#define RVA_CHARMOVE_SETDEST    (g_rva.CHARMOVE_SETDEST)
#define RVA_CHAR_SETDEST        (g_rva.CHAR_SETDEST)
#define RVA_MOVE_TO_POS         (g_rva.MOVE_TO_POS)
#define RVA_GUI_INV_COUNT       (g_rva.GUI_INV_COUNT)
#define RVA_GUI_STATS_BEG       (g_rva.GUI_STATS_BEG)
#define RVA_GUI_STATS_END       (g_rva.GUI_STATS_END)
#define RVA_GUI_WINSTACK        (g_rva.GUI_WINSTACK)
#define RVA_GUI_DLGWND          (g_rva.GUI_DLGWND)
#define RVA_DLG_GETVIS          (g_rva.DLG_GETVIS)
#define RVA_ESCMENU_PTR         (g_rva.ESCMENU_PTR)
#define RVA_ESC_GETVIS          (g_rva.ESC_GETVIS)
#define RVA_OVERVIEW_PTR        (g_rva.OVERVIEW_PTR)
#define RVA_OVW_GETVIS          (g_rva.OVW_GETVIS)
#define RVA_OPTIONS_PTR         (g_rva.OPTIONS_PTR)
#define RVA_OPT_GETVIS          (g_rva.OPT_GETVIS)
#define RVA_PROSPECT_PTR        (g_rva.PROSPECT_PTR)
#define RVA_PRO_GETVIS          (g_rva.PRO_GETVIS)
#define RVA_SAVELOAD_PTR        (g_rva.SAVELOAD_PTR)
#define RVA_MSGBOX_COUNT        (g_rva.MSGBOX_COUNT)
#define RVA_PLAYER_IFACE        (g_rva.PLAYER_IFACE)
#define RVA_CTXMENU_VISIBLE     (g_rva.CTXMENU_VISIBLE)
#define RVA_CAM_UPDATE          (g_rva.CAM_UPDATE)
#define RVA_TERRAIN_PTR         (g_rva.TERRAIN_PTR)
#define RVA_TOWNMGR_PTR         (g_rva.TOWNMGR_PTR)
#define RVA_NEAREST_TOWN        (g_rva.NEAREST_TOWN)
#define RVA_INTERIOR_LOAD       (g_rva.INTERIOR_LOAD)
#define RVA_GET_BONE_WORLD      (g_rva.GET_BONE_WORLD)
/* ========================================================================= */
#define HEAD_BONE_NAME    "Bip01 Head"
/* World-space orientation setter. Used for the look direction so it's immune to
 * the parent `center` node's orientation — setting a *local* orientation made
 * the camera ROLL when yawing (local combined with center's tilt). Position
 * stays LOCAL (small eye offset above center) to avoid world-coordinate issues.
 * NOTE: Ogre's get* accessors return BY VALUE and crashed across our ABI, so we
 * never read node state; FP exit just levels local orientation to identity. */
#define OGRE_SETDORI_SYM  "?_setDerivedOrientation@Node@Ogre@@QEAAXAEBVQuaternion@2@@Z"
#define EYE_HEIGHT        1.7f    /* fallback eye height ONLY when the head bone
                                   * read fails; head-bone offset is used when valid */
#define LOOK_SENS         0.0025f /* radians per mouse pixel */

/* ---- player character / position (proven in KenshiMP) ---- */
#define PI_PLAYERCHARS    0x2B0      /* PlayerInterface::playerCharacters (lektor<Character*>) */
#define LEK_COUNT         0x8        /* lektor::count (u32) */
#define LEK_STUFF         0x10       /* lektor::stuff (T*) */
#define GETPOS_VTABLE_SLOT 8         /* Character::getPosition; ABI Vec3*(this RCX, out RDX) */

/* ---- input (Win32, zero engine dependency for stage 0) ---- */
#define VK_TOGGLE_FP      0xA5       /* Right Alt (VK_RMENU) — enter/leave FP */
#define VK_W 0x57
#define VK_A 0x41
#define VK_S 0x53
#define VK_D 0x44

typedef struct { float x, y, z; } Vec3;
typedef struct { float w, x, y, z; } Quat;   /* Ogre::Quaternion order */
typedef Vec3 *(*get_position_t)(void *self, Vec3 *out);
typedef void (*mainloop_t)(void *gw, float time);
typedef void *(*follow_object_t)(void *cam, void *hand);
typedef void (*stop_follow_t)(void *cam);
typedef void (*node_set_pos_t)(void *node, const Vec3 *v);   /* local setPosition */
typedef void (*node_set_ori_t)(void *node, const Quat *q);   /* local setOrientation */
typedef void (*node_set_dori_t)(void *node, const Quat *q);  /* world _setDerivedOrientation */
typedef void (*cam_set_fovy_t)(void *camera, const float *rad);
typedef const float *(*cam_get_fovy_t)(void *camera);
typedef void (*cam_set_nearclip_t)(void *camera, float dist);
typedef float (*cam_get_nearclip_t)(void *camera);
typedef void (*pm_setpointer_t)(void *pm, const void *name_stdstr);
typedef void (*pm_setvisible_t)(void *pm, char visible);
typedef const void *(*pm_getdefault_t)(void *pm);   /* returns std::string* */
typedef void *(*pm_getinstance_t)(void);
typedef void *(*gui_getinstance_t)(void);
typedef void *(*gui_createwidget_t)(void *gui, const void *type, const void *skin,
                                    int l, int t, int w, int h, int align,
                                    const void *layer, const void *name);
typedef void (*imgbox_setimage_t)(void *imgbox, const void *texname);
typedef void (*widget_setvisible_t)(void *widget, char visible);
typedef char (*widget_inhvis_t)(void *widget);   /* Widget::getInheritedVisible */
typedef void *(*rgm_getsingleton_t)(void);
typedef void (*rgm_addlocation_t)(void *rgm, const void *name, const void *loctype,
                                  const void *group, char recursive, char readonly);
typedef void (*rgm_creategroup_t)(void *rgm, const void *group, char inGlobalPool);
typedef void (*rgm_initgroup_t)(void *rgm, const void *group);
typedef void (*charmove_setdest_t)(void *mv, const Vec3 *dest, int pri, char shift);
typedef void (*char_setdest_t)(void *self, const Vec3 *pos, const void *facingQuat);
typedef void (*move_to_pos_t)(void *ch, void *clickedObj, void *targetObj, const Vec3 *pos);

/* Terrain::getHeight / intersect result (Plugin_Terrain_x64.dll). Retbuf ABI:
 * this=RCX, TerrainHit* ret=RDX, arg=R8. Pure const query, main-thread safe. */
typedef struct {
    unsigned char hit, flag; unsigned short pad;
    Vec3 position;    /* +0x04: ground point (y = height) */
    Vec3 normal;      /* +0x10 */
} TerrainHit;         /* 0x1C bytes */
typedef TerrainHit *(*terrain_getheight_t)(void *self, TerrainHit *ret, const Vec3 *pos);
typedef struct { Vec3 origin; Vec3 dir; } OgreRay;   /* Ogre::Ray, 24 bytes */
typedef TerrainHit *(*terrain_intersect_t)(void *self, TerrainHit *ret, const OgreRay *ray);
typedef void (*cam_update_t)(void *cam, char controlEnabled);
typedef void *(*nearest_town_t)(void *mgr, const Vec3 *pos, int flags);
typedef void *(*town_getlist_t)(void *town, int listType, void *unused);
typedef void (*interior_load_t)(void *buildingInterior);
typedef void (*face_direction_t)(void *mv, const Vec3 *dir);
typedef void (*manual_move_t)(void *mv, const Vec3 *desiredMotion);
/* _setPositionAndTeleport(const Vec3& p, int floor): (this RCX, Vec3* RDX, floor R8) */
typedef void (*set_pos_tele_t)(void *mv, const Vec3 *pos, int floor);
typedef void (*node_set_dpos_t)(void *node, const Vec3 *v);   /* world _setDerivedPosition */
typedef Vec3 *(*node_get_dpos_t)(void *node, Vec3 *ret);      /* world _getDerivedPosition (this=RCX,ret=RDX) */
/* member-struct-return: this=RCX, retbuf=RDX, name=R8 */
typedef Vec3 *(*get_bone_world_t)(void *character, Vec3 *ret, const void *name);

static FILE *g_log;
static uintptr_t g_base;
static mainloop_t g_mainloop_orig;
static void *g_mainloop_target;        /* saved for the re-arm watchdog */
static volatile LONG g_heartbeat;      /* bumped every frame the hook fires */

/* Cooperative hooking. RE_Kenshi's KenshiLib docs: "DO use KenshiLib's built-in
 * function hooking system... DON'T use 3rd-party detour libraries as these can
 * cause issues when multiple plugins hook using different libraries." Our
 * MinHook detours silently never fire alongside RE_Kenshi. Fix: when KenshiLib
 * is loaded (RE_Kenshi present), route our hooks through its exported AddHook
 * (resolved at runtime -- no MSVC link needed); else use MinHook as before. */
#define KLIB_ADDHOOK_SYM "?AddHook@KenshiLib@@YA?AW4HookStatus@1@PEAX0PEAPEAX@Z"
typedef int (*klib_addhook_t)(void *target, void *detour, void **original); /* 0=SUCCESS */
static klib_addhook_t g_klib_addhook;
static int g_wrong_build;               /* set if the exe is neither 1.0.68 nor 1.0.65 */
static int g_build;                     /* 68 or 65 (the detected game build) */
static follow_object_t g_follow_object;
static stop_follow_t g_stop_following;
static node_set_pos_t g_node_set_pos;   /* Ogre::Node::setPosition (local) */
static node_set_ori_t g_node_set_ori;   /* Ogre::Node::setOrientation (local) */
static node_set_dori_t g_node_set_dori; /* Ogre::Node::_setDerivedOrientation (world) */
static node_set_dpos_t g_node_set_dpos; /* Ogre::Node::_setDerivedPosition (world) */
static node_get_dpos_t g_node_get_dpos; /* Ogre::Node::_getDerivedPosition (world) */
static get_bone_world_t g_get_bone_world;   /* Character::getBoneWorldPosition */
static unsigned char g_head_bone[32];   /* MSVC std::string "Bip01 Head" (SSO) */
static int g_ogre_ready;
static DWORD g_last_tick_ms;
static int g_fp_mode;              /* toggled by VK_TOGGLE_FP edge */
static volatile LONG g_toggle_edge;    /* latched keydown from the LL kbd hook */
static volatile LONG g_toggle_ll_down; /* LL-hook autorepeat filter */
static HHOOK g_kbd_hook;
static int g_toggle_was_down;
static int g_prev_fp;             /* g_fp_mode from last frame (camera_lock edge) */
static int g_ovr_prev;            /* was the FP node override active last frame */
static float g_yaw, g_pitch;      /* accumulated mouse-look angles (radians) */
static cam_set_fovy_t g_cam_set_fovy;
static cam_get_fovy_t g_cam_get_fovy;
static int g_fov_saved;
static float g_fov_default;
static cam_set_nearclip_t g_cam_set_nearclip;
static cam_get_nearclip_t g_cam_get_nearclip;
static int g_nearclip_saved;
static float g_nearclip_default;
static pm_setpointer_t g_pm_setpointer_orig;   /* MinHook trampoline */
static pm_setvisible_t g_pm_setvisible;
static pm_getdefault_t g_pm_getdefault;
static pm_getinstance_t g_pm_getinstance;
static gui_getinstance_t g_gui_getinstance;
static gui_createwidget_t g_gui_createwidget;
static imgbox_setimage_t g_imgbox_setimage;
static widget_setvisible_t g_widget_setvisible;
static widget_inhvis_t g_widget_inhvis;   /* Widget::getInheritedVisible */
static rgm_getsingleton_t g_rgm_getsingleton;
static rgm_addlocation_t g_rgm_addlocation;
static rgm_creategroup_t g_rgm_creategroup;
static rgm_initgroup_t g_rgm_initgroup;
static void *g_crosshair;          /* the ImageBox widget */
static int g_pointer_default = 1;  /* current MyGUI pointer is the default (arrow) */
static charmove_setdest_t g_charmove_setdest;  /* CharMovement::setDestination (raw move) */
static char_setdest_t g_char_setdest;          /* teleport+facing placement (unused) */
static move_to_pos_t g_move_to_pos;            /* Character::moveToPosition (walk order) */
static int g_movetopos_dead;                   /* set if moveToPosition ever faults */
static terrain_getheight_t g_terrain_getheight; /* Terrain::getHeight (Plugin_Terrain dll) */
static terrain_intersect_t g_terrain_intersect; /* Terrain::intersect (ray -> ground point) */
static int g_terrain_dead;                     /* set if the terrain query ever faults */
static cam_update_t g_cam_update_orig;         /* CameraClass::update trampoline */
static volatile LONG g_cam_heartbeat;          /* bumped every camera-update call */
static Vec3 g_last_eye;                        /* FP eye from the previous frame */
static Quat g_last_ori;                        /* FP look from the previous frame */
static int  g_have_eye;                        /* re-assert mid-frame only when valid */
static int  g_calib_wait;                      /* frames until T may recalibrate */
static void *g_gw_cache;                       /* GameWorld* for the mid-frame hook */
static float g_frame_dt;                       /* last frame's dt (stutter diag) */
static int g_ui_mask;                          /* which panel checks are open */
static Vec3 g_eye_sm;                          /* smoothed eye (X/Z low-pass) */
static int  g_eye_sm_ok;
static float g_lead_sm;                        /* smoothed W-ray lead distance */
static nearest_town_t g_nearest_town;          /* TownManager -> nearest Town */
static interior_load_t g_interior_load;        /* BuildingInterior lazy load + keep-alive */
static int g_interiors_dead;                   /* set if the interior preload ever faults */
static DWORD g_last_move_ms;
static int g_was_moving;
static float g_speed_scale = 0.6f; /* scrollwheel throttle: walk (low) .. run (high) */
static Vec3 g_last_dest;           /* last issued move-order target (for the re-issue gate) */
static int  g_have_dest;
static int g_dbg_wheel;
static HINSTANCE g_hinst;
static HHOOK g_mouse_hook;
static volatile LONG g_wheel_accum;  /* wheel notches*120, accumulated by the LL hook */
static float g_last_move_dir;      /* heading of last issued destination (radians) */
static int g_last_move_keys;       /* WASD bitmask of last issued destination */
static float g_move_tx, g_move_tz, g_move_dist;  /* last issued target + its distance */
static float g_dbg_center_y, g_dbg_eye_y;  /* diagnostics for eye-height tuning */
static float g_dbg_head_x, g_dbg_center_x;  /* frame check: head vs center X */
static int g_cursor_hidden;        /* our ShowCursor state */
static int g_ui_prev;              /* dialogue/menu open last frame (edge) */
static int g_ui_open;              /* dialogue/menu open THIS frame (read by the setPointer hook) */
static int g_dbg_control;          /* controlEnabled read, for verification */
static float g_tx, g_tz;           /* game->Ogre translation, calibrated when still */
static int g_have_t;
static float g_last_feet_x, g_last_feet_z;

/* True if p points inside the loaded kenshi_x64.exe image (where real vtables
 * and functions live). Used to reject false-positive "objects" before calling
 * through them — e.g. at the main menu there's no player, but a garbage pointer
 * can still pass readable(); its "vtable" won't be in the module. */
static int in_module(const void *p)
{
    uintptr_t a = (uintptr_t)p;
    return a >= g_base && a < g_base + 0x3000000;   /* exe image ~36MB */
}

static void logline(const char *fmt, ...)
{
    if (!g_log) return;
    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(g_log, "[%02u:%02u:%02u.%03u] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list ap; va_start(ap, fmt); vfprintf(g_log, fmt, ap); va_end(ap);
    fputc('\n', g_log); fflush(g_log);
}

/* Cheap pointer sanity check (no MSVC SEH under mingw). Rejects null /
 * non-canonical / unmapped; enough when driven from a live game frame. */
static int readable(const void *p, size_t n)
{
    if (!p) return 0;
    uintptr_t a = (uintptr_t)p;
    if (a < 0x10000 || a >= 0x0008000000000000ULL) return 0;
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof mbi)) return 0;
    if (mbi.State != MEM_COMMIT) return 0;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return 0;
    return (uintptr_t)mbi.BaseAddress + mbi.RegionSize >= a + n;
}

/* First player character (index 0) or NULL. */
static void *first_player_char(void *gw)
{
    if (!readable(gw, GW_PLAYER + 8)) return NULL;
    void *player = *(void **)((uintptr_t)gw + GW_PLAYER);
    if (!readable(player, PI_PLAYERCHARS + LEK_STUFF + 8)) return NULL;
    uint32_t count = *(uint32_t *)((uintptr_t)player + PI_PLAYERCHARS + LEK_COUNT);
    void **stuff   = *(void ***)((uintptr_t)player + PI_PLAYERCHARS + LEK_STUFF);
    if (count == 0 || count > 4096 || !readable(stuff, 8)) return NULL;
    void *c = stuff[0];
    if (!readable(c, 0x60)) return NULL;
    /* Reject anything whose vtable isn't in the exe (menu false positives). */
    void **vt = *(void ***)c;
    if (!readable(vt, (GETPOS_VTABLE_SLOT + 1) * 8) || !in_module(vt)) return NULL;
    return c;
}

/* Character::getPosition via live vtable slot 8 (proven ABI). */
static int char_position(void *c, Vec3 *out)
{
    if (!readable(c, 8)) return 0;
    void **vt = *(void ***)c;
    if (!readable(vt, (GETPOS_VTABLE_SLOT + 1) * 8) || !in_module(vt)) return 0;
    get_position_t getpos = (get_position_t)vt[GETPOS_VTABLE_SLOT];
    if (!in_module((void *)getpos)) return 0;   /* real fn lives in .text */
    Vec3 tmp = {0,0,0};
    getpos(c, &tmp);
    *out = tmp;
    return 1;
}

static int ui_panels_open(void);   /* forward decl (defined after the VEH guard) */

static void poll_input(void)
{
    /* Two detection paths, one toggle:
     *  - g_toggle_edge: latched by the LL keyboard hook at the instant of the
     *    physical keydown. On native Windows a bare Alt press can stall the
     *    game's message loop (system-menu modal), so a per-frame poll misses
     *    the whole press -- the hook doesn't.
     *  - GetAsyncKeyState edge: belt-and-braces for setups without the hook. */
    int down = (GetAsyncKeyState(VK_TOGGLE_FP) & 0x8000) != 0;
    LONG edge = InterlockedExchange(&g_toggle_edge, 0);
    if (edge || (down && !g_toggle_was_down)) {
        /* AltGr guard: on many EU layouts Right Alt is AltGr (typing € @ etc);
         * don't yank the player into FP while they type in an open panel. */
        if (g_fp_mode || !ui_panels_open()) {
            g_fp_mode = !g_fp_mode;
            logline("[input] FP mode toggled -> %s", g_fp_mode ? "ON" : "OFF");
        }
    }
    g_toggle_was_down = down;
}

/* Throttled read-only observation of everything the FP work will drive. */
static void fp_tick(void *gw)
{
    /* CameraClass instance (holder+0x10). Read its ctor-confirmed fields. */
    void *cam = *(void **)(g_base + RVA_CAM_INSTANCE);
    void *ogre_cam = NULL, *center = NULL, *node = NULL;
    float yaw = 0, pitch = 0, alt = 0;
    unsigned char freecam = 0;
    if (readable(cam, CC_FREECAM + 1)) {
        ogre_cam = *(void **)((uintptr_t)cam + CC_CAMERA);
        center   = *(void **)((uintptr_t)cam + CC_CENTER);
        node     = *(void **)((uintptr_t)cam + CC_NODE);
        yaw      = *(float *)((uintptr_t)cam + CC_YAW);
        pitch    = *(float *)((uintptr_t)cam + CC_PITCH);
        alt      = *(float *)((uintptr_t)cam + CC_ALTITUDE);
        freecam  = *(unsigned char *)((uintptr_t)cam + CC_FREECAM);
    }
    /* cross-check: the scene context's Ogre::Camera should equal cam+0x68 */
    void *scene = *(void **)(g_base + RVA_SCENE_CTX);
    void *scene_cam = readable(scene, SCENE_OGRE_CAMERA + 8)
        ? *(void **)((uintptr_t)scene + SCENE_OGRE_CAMERA) : NULL;

    float speedmult = readable(gw, GW_FRAMESPEED_OFF + 4)
        ? *(float *)((uintptr_t)gw + GW_FRAMESPEED_OFF) : -1.0f;

    void *pc = first_player_char(gw);
    Vec3 pos = {0,0,0};
    int havepos = pc && char_position(pc, &pos);

    int w = (GetAsyncKeyState(VK_W) & 0x8000) != 0;
    int a = (GetAsyncKeyState(VK_A) & 0x8000) != 0;
    int s = (GetAsyncKeyState(VK_S) & 0x8000) != 0;
    int d = (GetAsyncKeyState(VK_D) & 0x8000) != 0;

    /* head-bone offset (head - feet) for tuning the eye position. */
    Vec3 hoff = {0,0,0};
    if (havepos && g_get_bone_world) {
        Vec3 head;
        g_get_bone_world(pc, &head, g_head_bone);
        hoff.x = head.x - pos.x; hoff.y = head.y - pos.y; hoff.z = head.z - pos.z;
    }
    (void)ogre_cam; (void)scene_cam; (void)center; (void)node;
    (void)yaw; (void)pitch; (void)alt; (void)freecam; (void)speedmult;

    logline("[tick] fp=%d pos=(%.1f,%.1f,%.1f) eyeY=%.1f look(yaw=%.2f pitch=%.2f) control=%d wheel=%d speed=%.2f | WASD=%d%d%d%d",
            g_fp_mode, pos.x, pos.y, pos.z,
            g_dbg_eye_y, g_yaw, g_pitch, g_dbg_control, g_dbg_wheel, g_speed_scale, w, a, s, d);
}

/* M1 camera lock. Runs every frame. While FP is on, re-assert followObject on
 * the selected character so the camera tracks it and any vanilla pan that
 * cleared the follow is immediately overridden. On the FP->off edge, release. */
static void camera_lock(void *gw)
{
    void *cam = *(void **)(g_base + RVA_CAM_INSTANCE);
    if (!readable(cam, CC_FREECAM + 1)) return;

    if (g_fp_mode) {
        void *pc = first_player_char(gw);   /* v1: squad leader (index 0) */
        if (pc && readable((void *)((uintptr_t)pc + CHAR_HANDLE), 0x20))
            g_follow_object(cam, (void *)((uintptr_t)pc + CHAR_HANDLE));
    } else if (g_prev_fp) {
        g_stop_following(cam);              /* released this frame */
    }
    g_prev_fp = g_fp_mode;
}

/* M2 first-person override. After the game's camera update ran this frame,
 * force the camera node to the character's eye position and a mouse-look
 * orientation. Mouse-look v1: absolute cursor position maps to yaw/pitch
 * (read-only, no cursor recenter -> no fight with the engine's own cursor). */
static void mygui_cursor(int visible);   /* forward decl (defined below) */
static void ensure_crosshair(void);

/* --- DirectInput mouse (FP look deltas) ---------------------------------
 * The one delta source that both FEELS right and COEXISTS with the game:
 *  - RegisterRawInputDevices stole Wine-dinput's registration -> right-click
 *    broke (one raw-input target per device per process).
 *  - Cursor-warp / LL-hook positions ride the laggy coalesced cursor stream
 *    -> sluggish-then-teleport look.
 *  - A second NON-EXCLUSIVE BACKGROUND DirectInput mouse device reads the
 *    same high-rate relative stream the game does; Wine serves both. */
static IDirectInputDevice8A *g_di_mouse;
static int g_di_ready;
static const GUID g_guid_sysmouse =
    {0x6F1D2B60,0xD5A0,0x11CF,{0xBF,0xC7,0x44,0x45,0x53,0x54,0x00,0x00}};
static const GUID g_iid_idi8a =
    {0xBF798030,0x483A,0x4DA2,{0xAA,0x99,0x5D,0x64,0xED,0x36,0x97,0x00}};
typedef HRESULT (WINAPI *di8create_t)(HINSTANCE, DWORD, REFIID, LPVOID *, LPUNKNOWN);

static void ensure_dinput(void)
{
    static int tried;
    if (g_di_ready || tried >= 600) return;   /* retry through early frames */
    tried++;
    HWND w = FindWindowA("OgreD3D11Wnd", NULL);
    if (!w) w = GetForegroundWindow();
    if (!w) return;
    if (!g_di_mouse) {
        HMODULE dll = LoadLibraryA("dinput8.dll");
        di8create_t create = dll ? (di8create_t)GetProcAddress(dll, "DirectInput8Create") : NULL;
        IDirectInput8A *di = NULL;
        if (!create || FAILED(create(g_hinst, DIRECTINPUT_VERSION, &g_iid_idi8a,
                                     (void **)&di, NULL)) || !di) { tried = 600; return; }
        if (FAILED(di->lpVtbl->CreateDevice(di, &g_guid_sysmouse, &g_di_mouse, NULL))
            || !g_di_mouse) { tried = 600; return; }
        g_di_mouse->lpVtbl->SetDataFormat(g_di_mouse, &c_dfDIMouse2);
    }
    g_di_mouse->lpVtbl->SetCooperativeLevel(g_di_mouse, w,
                                            DISCL_BACKGROUND | DISCL_NONEXCLUSIVE);
    if (SUCCEEDED(g_di_mouse->lpVtbl->Acquire(g_di_mouse))) {
        g_di_ready = 1;
        logline("DirectInput mouse acquired (non-exclusive background)");
    }
}

/* Relative deltas since the previous call (mouse axes default to relative). */
static int di_get_deltas(LONG *dx, LONG *dy)
{
    if (!g_di_ready) return 0;
    DIMOUSESTATE2 st;
    if (FAILED(g_di_mouse->lpVtbl->GetDeviceState(g_di_mouse, sizeof st, &st))) {
        g_di_mouse->lpVtbl->Acquire(g_di_mouse);   /* lost: re-acquire, skip frame */
        return 0;
    }
    *dx = st.lX; *dy = st.lY;
    return 1;
}

static void fp_camera_override(void *gw)
{
    if (!g_ogre_ready) return;
    void *cam = *(void **)(g_base + RVA_CAM_INSTANCE);
    if (!readable(cam, CC_FREECAM + 1)) return;
    void *node = *(void **)((uintptr_t)cam + CC_NODE);
    if (!readable(node, 8)) return;

    /* Only drive the override while FP is on AND not in free-cam mode. */
    int active = g_fp_mode && !*(unsigned char *)((uintptr_t)cam + CC_FREECAM);

    if (active) {
        /* [foliage diag] sample the camera node's derived position as the game left
         * it this frame -- i.e. what foliage paging/culling saw during g_mainloop_orig,
         * before our override runs. Compared against the FP eye below. */
        Vec3 cam_pre = { 0, 0, 0 };
        int have_pre = g_node_get_dpos && g_node_get_dpos(node, &cam_pre);

        int cx = GetSystemMetrics(SM_CXSCREEN) / 2;
        int cy = GetSystemMetrics(SM_CYSCREEN) / 2;
        if (cx <= 0) cx = 960;
        if (cy <= 0) cy = 540;

        POINT cur; GetCursorPos(&cur);

        /* Dialogue/menu open? Game control is disabled then -> free the cursor so
         * the player can click UI; don't recenter or turn the camera. */
        int control = readable((void *)(g_base + RVA_INPUT_CONTROLENABLED), 1)
                      ? *(char *)(g_base + RVA_INPUT_CONTROLENABLED) : 1;
        g_dbg_control = control;
        /* Debounce the panel checks: the game transiently creates inventory
         * window state for ~0.5s during some NPC interactions (mask blips
         * 0x005 in the log). Reacting to those churned halt/restart orders
         * into the middle of NPC-driven task changes -> crash. Dialogue
         * (control==0) stays immediate. */
        int panels_now = ui_panels_open();
        static int panel_frames;
        if (panels_now) { if (panel_frames < 1000) panel_frames++; }
        else panel_frames = 0;
        int ui_open = (control == 0) || (panels_now && panel_frames >= 8);
        g_ui_open = ui_open;                    /* read by the setPointer hook */
        if (!g_ovr_prev) { mygui_cursor(0); g_pointer_default = 1; }  /* FP enter */
        ensure_crosshair();
        if (g_crosshair && g_widget_setvisible)
            g_widget_setvisible(g_crosshair, (g_pointer_default && !ui_open) ? 1 : 0);

        if (ui_open) {
            if (g_cursor_hidden) { while (ShowCursor(TRUE) < 0) { } g_cursor_hidden = 0; }
            mygui_cursor(1);                    /* dialogue: keep the game cursor visible */
            /* cursor free; leave look angle frozen */
        } else {
            if (!g_cursor_hidden) { while (ShowCursor(FALSE) >= 0) { } g_cursor_hidden = 1; }
            ensure_dinput();
            if (!g_ovr_prev) {
                g_yaw = 0.0f; g_pitch = 0.0f;   /* FP enter: zero the look */
                LONG jx, jy; di_get_deltas(&jx, &jy);   /* drain pre-FP motion */
            } else if (!g_ui_prev) {            /* skip the delta on the frame a dialogue closes */
                /* DirectInput relative deltas (see ensure_dinput comment);
                 * cursor-warp deltas only as fallback. */
                LONG rdx, rdy;
                if (!di_get_deltas(&rdx, &rdy)) {
                    rdx = cur.x - cx; rdy = cur.y - cy;
                }
                g_yaw   -= (float)rdx * LOOK_SENS;   /* mouse right -> look right */
                g_pitch += (float)rdy * LOOK_SENS;   /* mouse down  -> look down  */
                if (g_pitch >  1.4f) g_pitch =  1.4f;
                if (g_pitch < -1.4f) g_pitch = -1.4f;
            } else {
                LONG jx, jy; di_get_deltas(&jx, &jy);   /* dialogue frame: discard */
            }
            SetCursorPos(cx, cy);  /* recenter so the cursor never drifts to edges */
        }
        g_ui_prev = ui_open;

        /* Fullscreen overview (map/factions/squads) REPURPOSES the render view:
         * keeping our per-frame camera writes running while the map owns the
         * camera crashed the game seconds after opening it. Stand down fully --
         * cursor is already freed above; resume when it closes. (Dialogue and
         * item panels keep the normal 3D view, so the override continues for
         * them and the FP framing is preserved.) */
        if (ui_open && (g_ui_mask & UIMASK_OVERVIEW)) {
            g_have_eye = 0;            /* no mid-frame re-asserts either */
            g_ovr_prev = active;
            return;
        }

        /* Eye position, fully in WORLD space: centerWorld + (head - feet). The
         * head-feet offset (world axes, from getBoneWorldPosition/getPosition)
         * tracks animation + ragdoll; centerWorld comes from the center node's
         * derived position. Both derived -> no dip, and orientation stays roll-
         * free (below). */
        void *center = *(void **)((uintptr_t)cam + CC_CENTER);
        Vec3 centerW;
        if (readable(center, 8) && g_node_get_dpos(center, &centerW)) {
            /* The `center` node bobs/smooths with the vanilla camera, so it is a
             * bad vertical anchor. Take X/Z from it (Ogre space; X/Z are rebased
             * by the floating origin so we must stay in Ogre space + add the head
             * lean), but take Y straight from the head bone (Y is NOT rebased and
             * is stable) -> the eye is welded to head height, no bob. */
            Vec3 eyeW = { centerW.x, centerW.y + EYE_HEIGHT - EYE_DROP, centerW.z };  /* fallback */
            if (g_get_bone_world) {
                void *pc = first_player_char(gw);
                Vec3 feet, head;
                if (pc && char_position(pc, &feet)) {
                    g_get_bone_world(pc, &head, g_head_bone);
                    Vec3 h = { head.x - feet.x, head.y - feet.y, head.z - feet.z };
                    if (h.x*h.x + h.y*h.y + h.z*h.z > 0.25f) {
                        eyeW.y = head.y - EYE_DROP;   /* head-bone Y directly (Y not rebased) */
                        /* head/feet are GAME coords, center is OGRE; they differ by
                         * a constant floating-origin translation T. Calibrate T ONLY
                         * while idle (the center node has caught up); freeze it during
                         * movement, when the center lags and would corrupt T. Then use
                         * head_game + T -> exact, lag-free tracking. */
                        /* Calibrate T only after the center node has settled:
                         * during movement we snap the center to the eye (grass
                         * paging fix), so give it ~0.5s post-stop to lerp back
                         * to its vanilla follow point before trusting it. */
                        if (g_was_moving) {
                            g_calib_wait = 30;
                        } else if (g_calib_wait > 0) {
                            g_calib_wait--;
                        } else {
                            g_tx = centerW.x - feet.x;
                            g_tz = centerW.z - feet.z;
                            g_have_t = 1;
                        }

                        if (g_have_t) {
                            eyeW.x = head.x + g_tx;
                            eyeW.z = head.z + g_tz;
                        } else {                       /* until first calibration */
                            eyeW.x = centerW.x + h.x; eyeW.z = centerW.z + h.z;
                        }
                        g_dbg_head_x = head.x; g_dbg_center_x = centerW.x;
                    }
                }
            }
            /* Push the eye forward (horizontal look dir) so it sits at the face,
             * not inside the head mesh. */
            eyeW.x += sinf(g_yaw) * FP_EYE_FORWARD;
            eyeW.z += cosf(g_yaw) * FP_EYE_FORWARD;

            /* Low-pass the eye X/Z while moving: per-frame Task_Move
             * retargeting during mouse turns makes the body re-face constantly
             * and the head bone swings laterally with each correction --
             * welded raw, that reads as camera jitter. Y stays raw so the
             * natural gait bob survives. */
            if (g_was_moving && g_eye_sm_ok) {
                eyeW.x = g_eye_sm.x + (eyeW.x - g_eye_sm.x) * 0.5f;
                eyeW.z = g_eye_sm.z + (eyeW.z - g_eye_sm.z) * 0.5f;
            }
            g_eye_sm = eyeW; g_eye_sm_ok = 1;

            g_dbg_center_y = centerW.y; g_dbg_eye_y = eyeW.y;   /* for tuning */
            g_node_set_dpos(node, &eyeW);
            g_last_eye = eyeW;             /* for the mid-frame re-assert */

            /* [foliage diag] how far is the game's per-frame camera position (used
             * by grass paging) from our FP eye? A large gap => grass pages around
             * the wrong point => pop-in under the feet. */
            if (have_pre) {
                static int fcnt;
                if (((++fcnt) % 120) == 0) {
                    float ddx = cam_pre.x - eyeW.x, ddy = cam_pre.y - eyeW.y, ddz = cam_pre.z - eyeW.z;
                    logline("[foliage] cam_pre=(%.1f,%.1f,%.1f) fp_eye=(%.1f,%.1f,%.1f) gap=%.1f (dY=%.1f)",
                            cam_pre.x, cam_pre.y, cam_pre.z, eyeW.x, eyeW.y, eyeW.z,
                            sqrtf(ddx*ddx + ddy*ddy + ddz*ddz), ddy);
                }
            }
        }

        /* World look orientation q = q_yaw(Y) * q_pitch(X); no roll. Ogre camera
         * looks down -Z at identity. DERIVED (world) with normal inheritance. */
        float qy = cosf(g_yaw * 0.5f),   sqy = sinf(g_yaw * 0.5f);
        float qp = cosf(g_pitch * 0.5f), sqp = sinf(g_pitch * 0.5f);
        Quat q = { qy * qp, qy * sqp, sqy * qp, -sqy * sqp };
        g_node_set_dori(node, &q);
        g_last_ori = q; g_have_eye = 1;    /* mid-frame re-assert now armed */

        /* FOV: capture default once, then force the FP FOV each frame. */
        void *ogre_cam = *(void **)((uintptr_t)cam + CC_CAMERA);
        if (g_cam_set_fovy && readable(ogre_cam, 8)) {
            if (!g_fov_saved && g_cam_get_fovy) {
                const float *d = g_cam_get_fovy(ogre_cam);
                /* sanity range ~17..115 deg: never trust a transitional value,
                 * it would get restored on exit and poison future captures */
                if (readable((void *)d, 4) && *d > 0.30f && *d < 2.0f) {
                    g_fov_default = *d; g_fov_saved = 1;
                }
            }
            float rad = FP_FOV_DEG * DEG2RAD;
            g_cam_set_fovy(ogre_cam, &rad);
        }

        /* Near clip: capture default once, then pull it in so nearby meshes
         * don't get clipped away (seeing through walls) at eye level. */
        if (g_cam_set_nearclip && readable(ogre_cam, 8)) {
            if (!g_nearclip_saved && g_cam_get_nearclip) {
                g_nearclip_default = g_cam_get_nearclip(ogre_cam);
                g_nearclip_saved = 1;
            }
            g_cam_set_nearclip(ogre_cam, FP_NEARCLIP);
        }
    } else if (g_ovr_prev) {
        /* FP exit: release the cursor, restore FOV, and level the node
         * orientation to identity so update()'s incremental rotations no longer
         * drift off our FP angle (position is left for update() to reset). */
        while (ShowCursor(TRUE) < 0) { }
        g_cursor_hidden = 0; g_ui_prev = 0; g_ui_open = 0;
        mygui_cursor(1);                        /* FP exit: restore the game cursor */
        if (g_crosshair && g_widget_setvisible) g_widget_setvisible(g_crosshair, 0);
        g_have_eye = 0;                         /* stop the mid-frame re-assert */
        g_eye_sm_ok = 0; g_lead_sm = 0.0f;      /* reset smoothing state */
        /* Restore FOV/near-clip but KEEP the cached defaults (g_*_saved stays
         * set): re-capturing on every FP enter meant one bad capture (mid-zoom
         * transition) got restored on exit, then read back as "default" on the
         * next enter -- permanently poisoned FOV. Capture once per session. */
        void *ogre_cam = *(void **)((uintptr_t)cam + CC_CAMERA);
        if (g_fov_saved && g_cam_set_fovy && readable(ogre_cam, 8))
            g_cam_set_fovy(ogre_cam, &g_fov_default);
        if (g_nearclip_saved && g_cam_set_nearclip && readable(ogre_cam, 8))
            g_cam_set_nearclip(ogre_cam, g_nearclip_default);
        Quat ident = { 1.0f, 0.0f, 0.0f, 0.0f };
        g_node_set_ori(node, &ident);
    }
    g_ovr_prev = active;
}

/* M3: drive the followed character with WASD relative to where we're looking.
 * Breadcrumb CharMovement::setDestination a few metres ahead at ~10 Hz; halt at
 * the current position when all keys release. */
/* --- VEH crash guard: poor-man's __try/__except for calls into game code. ---
 * mingw has no MSVC __try, so: arm a flag, setjmp, call the game fn; if it
 * faults (or raises an MSVC C++ exception, 0xE06D7363 -- what KenshiCoop's SEH
 * catches around this very call), the vectored handler longjmps back here.
 * mingw-x64 setjmp passes a NULL frame, so longjmp restores registers without
 * unwinding -- safe across foreign (game) frames. */
static jmp_buf g_guard_jb;
static volatile LONG g_guard_armed;

static LONG CALLBACK veh_guard(EXCEPTION_POINTERS *ep)
{
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (g_guard_armed &&
        (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_ILLEGAL_INSTRUCTION
         || code == EXCEPTION_PRIV_INSTRUCTION || code == 0xE06D7363u /* MSVC C++ throw */)) {
        g_guard_armed = 0;
        longjmp(g_guard_jb, 1);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

/* Any interactive UI panel open? (inventory/loot/trade, stats, window stack,
 * dialogue, ESC menu, overview map/factions/squads, options, prospecting,
 * save/load, modal boxes). Pure reads + tiny getVisible calls, VEH-guarded.
 * Used to free the captured FP cursor whenever the player needs to click UI. */
typedef char (*ui_getvis_t)(void *w);
static int ui_panels_open(void)
{
    static int dead;
    if (dead) return 0;
    uintptr_t B = g_base;

    int mask = 0;

    /* pure static reads */
    if (readable((void *)(B + RVA_GUI_INV_COUNT), 8)
        && *(unsigned long long *)(B + RVA_GUI_INV_COUNT)) mask |= 0x001;
    if (readable((void *)(B + RVA_GUI_STATS_BEG), 16)
        && *(void **)(B + RVA_GUI_STATS_BEG) != *(void **)(B + RVA_GUI_STATS_END)) mask |= 0x002;
    if (readable((void *)(B + RVA_GUI_WINSTACK), 4)) {
        /* The stack holds persistent members (main HUD) even with nothing
         * open -- baseline was 1, which kept the gate stuck. Track the
         * session minimum and flag only counts ABOVE it. */
        unsigned ws = *(unsigned *)(B + RVA_GUI_WINSTACK);
        static unsigned ws_min = 0xffffffffu;
        if (ws < ws_min) ws_min = ws;
        if (ws > ws_min) mask |= 0x004;
    }
    if (readable((void *)(B + RVA_MSGBOX_COUNT), 4)
        && *(unsigned *)(B + RVA_MSGBOX_COUNT)) mask |= 0x008;
    /* right-click context menu: the engine's own cached isVisible byte */
    if (RVA_CTXMENU_VISIBLE && readable((void *)(B + RVA_CTXMENU_VISIBLE), 1)
        && *(unsigned char *)(B + RVA_CTXMENU_VISIBLE)) mask |= 0x400;
    if (readable((void *)(B + RVA_SAVELOAD_PTR), 8)) {
        unsigned char *sv = *(unsigned char **)(B + RVA_SAVELOAD_PTR);
        if (readable(sv, 0x118)
            && (*(void **)(sv + 0x100) || *(void **)(sv + 0x108) || *(void **)(sv + 0x110)))
            mask |= 0x010;
    }

    /* optional singletons: null-check pointer, then a one-line getVisible */
    /* runtime-initialised (RVA_* now read the active version table) */
    const struct { uintptr_t ptr_rva, fn_rva; } chk[] = {
        { RVA_GUI_DLGWND,   RVA_DLG_GETVIS },   /* 0x020 */
        { RVA_ESCMENU_PTR,  RVA_ESC_GETVIS },   /* 0x040 */
        { RVA_OVERVIEW_PTR, RVA_OVW_GETVIS },   /* 0x080 */
        { RVA_OPTIONS_PTR,  RVA_OPT_GETVIS },   /* 0x100 */
        { RVA_PROSPECT_PTR, RVA_PRO_GETVIS },   /* 0x200 */
    };
    if (setjmp(g_guard_jb)) {
        dead = 1;
        logline("ui_panels_open FAULTED -- disabled for this session");
        return 0;
    }
    g_guard_armed = 1;
    int i;
    for (i = 0; i < 5; i++) {
        if (!chk[i].ptr_rva || !chk[i].fn_rva) continue;   /* unmapped on this build */
        if (!readable((void *)(B + chk[i].ptr_rva), 8)) continue;
        void *w = *(void **)(B + chk[i].ptr_rva);
        if (!readable(w, 0x60)) continue;
        if (((ui_getvis_t)(B + chk[i].fn_rva))(w)) mask |= (0x020 << i);
    }
    g_guard_armed = 0;

    static int last_mask = -1;
    if (mask != last_mask) {           /* diag: which check flips the gate */
        last_mask = mask;
        logline("[ui] panel mask=0x%03x", mask);
    }
    g_ui_mask = mask;
    return mask != 0;
}

/* Character::moveToPosition (RVA 0x5d22b0 = vtable +0x318), crash-guarded: the
 * GENUINE right-click walk path. Issues/live-updates a Task_Move(0x1d) --
 * pathfinds, ground-follows, animates, and as a real player order makes a
 * DOWNED character struggle back up. Per-frame calls are exactly the game's
 * drag-move ("hold right-click") path: an active Task_Move just gets its
 * destination Vec3 updated in place.
 * Returns 1 if the call completed, 0 if unavailable or previously faulted. */
static int try_move_to_pos(void *pc, const Vec3 *tgt)
{
    if (g_movetopos_dead || !g_move_to_pos) return 0;
    if (!in_module(*(void ***)pc)) return 0;
    if (setjmp(g_guard_jb)) {          /* longjmp target: the call blew up */
        g_movetopos_dead = 1;
        logline("moveToPosition FAULTED -- disabled for this session");
        return 0;
    }
    g_guard_armed = 1;
    g_move_to_pos(pc, NULL, NULL, tgt);   /* NULL,NULL = plain ground point */
    g_guard_armed = 0;
    return 1;
}

/* Is the character's CURRENT task our walk order (Task_Move 0x1d)? Used to
 * choose moveToPosition's cheap live-update path vs the queue-clearing create
 * path, and to avoid stomping NPC-forced tasks (dialogue, grabs, jobs). */
static int char_task_is_move(void *pc)
{
    void *tholder = readable((void *)((uintptr_t)pc + CHAR_TASKHOLDER), 8)
        ? *(void **)((uintptr_t)pc + CHAR_TASKHOLDER) : NULL;
    if (!readable(tholder, TASK_CUR + 8)) return 0;
    void *cur = *(void **)((uintptr_t)tholder + TASK_CUR);
    if (!readable(cur, TASK_DESC + 8)) return 0;
    void *desc = *(void **)((uintptr_t)cur + TASK_DESC);
    return readable(desc, TASKDESC_TYPE + 4)
        && *(int *)((uintptr_t)desc + TASKDESC_TYPE) == TASK_MOVE_ID;
}

/* Clamp a target point onto the terrain surface via Terrain::getHeight
 * (Plugin_Terrain_x64.dll; pure const query). THE slope fix: the raw mover
 * stalls when the fabricated target Y is underground (uphill) or midair
 * (downhill); an on-ground destination walks fine (right-click parity). */
static void terrain_clamp(Vec3 *p)
{
    if (g_terrain_dead || !g_terrain_getheight) return;
    void *terr = readable((void *)(g_base + RVA_TERRAIN_PTR), 8)
        ? *(void **)(g_base + RVA_TERRAIN_PTR) : NULL;
    if (!readable(terr, 8)) return;
    if (setjmp(g_guard_jb)) {
        g_terrain_dead = 1;
        logline("Terrain::getHeight FAULTED -- disabled for this session");
        return;
    }
    g_guard_armed = 1;
    TerrainHit hit; memset(&hit, 0, sizeof hit);
    g_terrain_getheight(terr, &hit, p);
    g_guard_armed = 0;
    if (hit.hit) {
        static int logged;
        if (!logged) { logged = 1; logline("terrain clamp live: y %.1f -> %.1f", p->y, hit.position.y); }
        p->y = hit.position.y;
    }
}

/* Cast a ray into the terrain (Terrain::intersect -- the SAME function the
 * game's right-click picking uses). Returns 1 + the surface point on hit.
 * With the FP cursor locked to screen center, eye+look ray == "where a
 * right-click would land". */
static int terrain_ray(const Vec3 *origin, const Vec3 *dir, Vec3 *out)
{
    if (g_terrain_dead || !g_terrain_intersect) return 0;
    void *terr = readable((void *)(g_base + RVA_TERRAIN_PTR), 8)
        ? *(void **)(g_base + RVA_TERRAIN_PTR) : NULL;
    if (!readable(terr, 8)) return 0;
    if (setjmp(g_guard_jb)) {
        g_terrain_dead = 1;
        logline("Terrain::intersect FAULTED -- disabled for this session");
        return 0;
    }
    g_guard_armed = 1;
    OgreRay ray; ray.origin = *origin; ray.dir = *dir;
    TerrainHit hit; memset(&hit, 0, sizeof hit);
    g_terrain_intersect(terr, &hit, &ray);
    g_guard_armed = 0;
    if (!hit.hit) return 0;
    *out = hit.position;
    return 1;
}

static void fp_movement(void *gw, float dt)
{
    if (!g_fp_mode || !g_charmove_setdest) return;
    void *pc = first_player_char(gw);
    if (!pc) return;
    void *mv = readable((void *)((uintptr_t)pc + CHAR_MOVEMENT), 8)
        ? *(void **)((uintptr_t)pc + CHAR_MOVEMENT) : NULL;
    if (!readable(mv, 8)) return;

    /* Scrollwheel throttle: consume the wheel captured by our LL hook. */
    LONG wheel = InterlockedExchange(&g_wheel_accum, 0);
    g_dbg_wheel = (int)wheel;
    if (wheel != 0) {
        g_speed_scale += (wheel / 120.0f) * 0.15f;      /* one notch = 0.15 */
        if (g_speed_scale < 0.15f) g_speed_scale = 0.15f;   /* slow walk */
        if (g_speed_scale > 1.0f)  g_speed_scale = 1.0f;    /* full run */
    }

    int w = (GetAsyncKeyState(VK_W) & 0x8000) != 0;
    int s = (GetAsyncKeyState(VK_S) & 0x8000) != 0;
    int a = (GetAsyncKeyState(VK_A) & 0x8000) != 0;
    int d = (GetAsyncKeyState(VK_D) & 0x8000) != 0;
    int keys = w | (s << 1) | (a << 2) | (d << 3);
    float mf = (float)(w - s);     /* forward/back */
    float mr = (float)(d - a);     /* left/right (turns the char, not strafe) */

    (void)dt;
    /* Treat "UI panel open" as keys-released: halts if we were moving, and no
     * WASD while typing/clicking in panels. */
    if (g_ui_open || keys == 0 || (mf == 0.0f && mr == 0.0f)) {
        if (g_was_moving) {         /* release: halt at current position */
            Vec3 here;
            /* Halt ONLY if our walk task still owns the character. If an
             * NPC-forced task (dialogue, grab, arrest) took over, issuing a
             * halt would CLEAR their queue mid-mutation -> crash. */
            if (char_task_is_move(pc) && char_position(pc, &here)) {
                if (!try_move_to_pos(pc, &here))   /* order to current pos = stop */
                    g_charmove_setdest(mv, &here, UPDATE_PRIORITY_HIGH, 0);
            }
            g_was_moving = 0;
            g_have_dest = 0;
            g_lead_sm = 0.0f;           /* fresh lead estimate on next move */
        }
        /* Orient-to-control: while standing still, rotate the body to face the
         * camera look direction via faceDirection (CharMovement vtable slot 6).
         * Idle physics doesn't fight it, so it holds; during movement the order
         * system drives facing toward the path instead. */
        void **mvvt = *(void ***)mv;
        if (in_module(mvvt)) {
            face_direction_t face = (face_direction_t)mvvt[MV_FACEDIR_VTOFF];
            if (in_module((void *)face)) {
                Vec3 lookDir = { sinf(g_yaw), 0.0f, cosf(g_yaw) };
                face(mv, &lookDir);
            }
        }
        return;
    }

    /* Heading relative to camera yaw (negated: W=forward, S=back, A=left, D=right). */
    float th = g_yaw;
    float fx = -sinf(th), fz = -cosf(th);
    float rx =  cosf(th), rz = -sinf(th);
    float dx = fx * mf + rx * mr;
    float dz = fz * mf + rz * mr;
    float len = sqrtf(dx * dx + dz * dz);
    if (len < 0.001f) return;
    dx = -dx / len; dz = -dz / len;

    /* Speed from the scrollwheel throttle (down = walk, up = run). */
    float dist = MOVE_NEAR + g_speed_scale * (MOVE_FAR - MOVE_NEAR);

    Vec3 here;
    if (!char_position(pc, &here)) return;

    /* Order-based move (animates + ground-clamps + turns to face path); re-issue
     * every frame so it holds. Camera stays welded via calibrated T.
     * NOTE: this is the raw CharMovement mover -- no navmesh pathfinding, so it
     * struggles on steep slopes (target Y is fixed). The player-path
     * Character::setDestination (0x5c84e0) DOES pathfind but crashes when called
     * from our post-frame hook without the game's SEH-guarded order setup. */
    /* Destination: replicate right-click as faithfully as possible.
     * Moving forward: cast the camera LOOK ray into the terrain (the very pick
     * right-click uses; FP cursor == screen center) -> the visible surface
     * point ahead. On a steep slope face that's a NEAR, reachable point, so
     * climbing works exactly like drag-move -- a fabricated far target lands
     * beyond the crest / inside the hill and stalls the pathfinder (observed:
     * stall at y=1555 with target y=1511 214u ahead). Capped to `dist` so the
     * scrollwheel speed control (distance = speed) still rules.
     * Strafe/backpedal (look != move dir) or sky-aimed ray: fabricate the
     * point ahead and ground-clamp its height. */
    Vec3 tgt; int via_ray = 0;
    if (mf > 0.0f && mr == 0.0f) {
        float cp = cosf(g_pitch);
        Vec3 ro = { here.x, here.y + 16.0f, here.z };            /* ~eye, game frame */
        Vec3 rd = { dx * cp, -sinf(g_pitch), dz * cp };          /* unit look dir */
        Vec3 hitp;
        if (terrain_ray(&ro, &rd, &hitp)) {
            float vx = hitp.x - here.x, vz = hitp.z - here.z;
            float hd = sqrtf(vx * vx + vz * vz);
            if (hd > 0.5f) {
                if (hd > dist) hd = dist; /* cap: scrollwheel speed still rules */
                if (hd < 25.0f) hd = 25.0f; /* min lead: never ARRIVE mid-hold --
                                             * arrival completes the task and the
                                             * restart gap reads as stutter/stall
                                             * (broke slope climbing) */
                /* Smooth the lead distance: the raw ray distance jumps every
                 * frame while the view sweeps terrain, and Kenshi scales run
                 * speed by target distance -> surge/brake = jittery mouse feel
                 * while moving. Converges in ~0.1s so steep-face near targeting
                 * (the slope fix) engages promptly. */
                if (g_lead_sm <= 0.0f) g_lead_sm = hd;
                g_lead_sm += (hd - g_lead_sm) * 0.3f;
                float hl = sqrtf(vx * vx + vz * vz);
                tgt.x = here.x + (vx / hl) * g_lead_sm;
                tgt.z = here.z + (vz / hl) * g_lead_sm;
                tgt.y = here.y;
                terrain_clamp(&tgt);
                via_ray = 1;
            }
        }
    }
    if (!via_ray) {
        tgt.x = here.x + dx * dist; tgt.y = here.y; tgt.z = here.z + dz * dist;
        terrain_clamp(&tgt);
    }

    /* Primary drive: the REAL right-click walk order (Task_Move) -- pathfinds
     * over slopes, animates, triggers get-up when downed.
     * moveToPosition live-updates the destination when the CURRENT task is
     * already Task_Move (drag-move path, cheap, per-frame safe). When it is
     * NOT (movement start, stagger, or a JOB like Bodyguard holds the task
     * slot), each call CLEARS the queue + allocates a fresh task -- at 60/s
     * that wars with the job system's own queue mutations (heap-churn crash
     * when assigning Bodyguard). So: per-frame updates while the walk task is
     * current; task CREATION allowed instantly a few times, then ~4/s.
     * (An earlier removal of this throttle blamed it for chunky turning --
     * that was actually the warp-mouse input, since fixed by DirectInput.) */
    int cur_is_move = char_task_is_move(pc);
    static int create_streak, issue_cd;
    if (cur_is_move) create_streak = 0;
    if (issue_cd > 0) issue_cd--;
    if (cur_is_move || create_streak < 3 || issue_cd <= 0) {
        if (!try_move_to_pos(pc, &tgt))
            g_charmove_setdest(mv, &tgt, UPDATE_PRIORITY_HIGH, 1);
        if (!cur_is_move) { create_streak++; issue_cd = 15; }
    }

    g_was_moving = 1;
}

/* Read an MSVC std::string into out. Layout: [+0x0] SSO buf or heap ptr,
 * [+0x10] size, [+0x18] capacity; heap when capacity >= 16. */
static void read_mstring(const void *str, char *out, size_t outsz)
{
    out[0] = '\0';
    if (!readable(str, 0x20)) return;
    size_t cap  = *(size_t *)((uintptr_t)str + 0x18);
    size_t size = *(size_t *)((uintptr_t)str + 0x10);
    const char *data = (cap >= 16) ? *(const char **)str : (const char *)str;
    if (size >= outsz) size = outsz - 1;
    if (size > 512 || !readable(data, size + 1)) return;
    memcpy(out, data, size);
    out[size] = '\0';
}

/* Build an MSVC std::string (SSO) in a 32-byte buffer. len must be < 16. */
static void make_mstr(unsigned char *b32, const char *s)
{
    memset(b32, 0, 32);
    size_t len = strlen(s);
    if (len > 15) len = 15;
    memcpy(b32, s, len);
    *(size_t *)(b32 + 0x10) = len;
    *(size_t *)(b32 + 0x18) = 15;   /* SSO capacity */
}

/* Lazily create the crosshair ImageBox once MyGUI is up (retried each FP enter
 * until it succeeds). Registers the image folder as an Ogre resource location. */
static void ensure_crosshair(void)
{
    if (g_crosshair) return;
    if (!g_gui_getinstance || !g_gui_createwidget || !g_imgbox_setimage
        || !g_widget_setvisible) return;
    void *gui = g_gui_getinstance();
    if (!readable(gui, 8)) return;   /* MyGUI not initialised yet */

    /* Register the image folder in Kenshi's "GUI" resource group -- that is the
     * group MyGUI's data manager searches for textures (its ./data/gui/ folder).
     * MyGUI globs the group live (findResourceNames), so a location added now is
     * picked up; "General"/a fresh group are NOT searched by MyGUI. */
    if (g_rgm_getsingleton && g_rgm_addlocation) {
        void *rgm = g_rgm_getsingleton();
        if (readable(rgm, 8)) {
            unsigned char loc[32], ft[32], grp[32];
            make_mstr(loc, "kenshifp"); make_mstr(ft, "FileSystem"); make_mstr(grp, "GUI");
            g_rgm_addlocation(rgm, loc, ft, grp, 0, 1);
        }
    }

    int cx = GetSystemMetrics(SM_CXSCREEN), cy = GetSystemMetrics(SM_CYSCREEN);
    if (cx <= 0) cx = 1920;
    if (cy <= 0) cy = 1080;
    /* Skin MUST be "ImageBox" (defined in Kenshi's common_skins.xml): a widget
     * renders through its skin's sub-items, so an empty skin draws nothing --
     * that was why the widget existed + texture loaded, yet nothing showed. */
    unsigned char type[32], skin[32], layer[32], name[32], tex[32];
    make_mstr(type, "ImageBox"); make_mstr(skin, "ImageBox"); make_mstr(layer, "Pointer");
    make_mstr(name, "FPCrosshair"); make_mstr(tex, "crosshair.png");
    void *w = g_gui_createwidget(gui, type, skin,
                                 cx / 2 - CROSSHAIR_SIZE / 2, cy / 2 - CROSSHAIR_SIZE / 2,
                                 CROSSHAIR_SIZE, CROSSHAIR_SIZE, 0 /*Align::Center*/, layer, name);
    if (readable(w, 8)) {
        g_imgbox_setimage(w, tex);
        g_widget_setvisible(w, 0);
        g_crosshair = w;
        logline("crosshair widget created %p", w);
    }
}

/* MyGUI PointerManager::setPointer hook: while FP is on, hide the default (arrow)
 * cursor — we show our crosshair instead — but keep contextual pointers
 * (sword/speech) visible. */
static void hooked_setpointer(void *pm, const void *name)
{
    g_pm_setpointer_orig(pm, name);
    if (!g_fp_mode || !g_pm_setvisible) return;
    if (g_ui_open) { g_pm_setvisible(pm, 1); g_pointer_default = 0; return; }
    if (!g_pm_getdefault) return;
    char cur[128], def[128];
    read_mstring(name, cur, sizeof cur);
    read_mstring(g_pm_getdefault(pm), def, sizeof def);
    int is_default = (cur[0] != '\0' && strcmp(cur, def) == 0);
    g_pm_setvisible(pm, is_default ? 0 : 1);   /* hide arrow (crosshair instead), show contextual */
    g_pointer_default = is_default;
}

/* Force the MyGUI cursor visible/hidden (used on FP enter/exit). */
static void mygui_cursor(int visible)
{
    if (!g_pm_setvisible || !g_pm_getinstance) return;
    void *pm = g_pm_getinstance();
    if (readable(pm, 8)) g_pm_setvisible(pm, (char)(visible ? 1 : 0));
}

/* FP QoL: preload interiors of nearby buildings while approaching from OUTSIDE
 * (vanilla shows an interior only once a character is inside / build mode).
 * ~1 Hz: nearest Town at the player's position -> its buildings-with-layout
 * list (Town vt slot +0x2d8, list id 0x13) -> for each with a BuildingInterior
 * within INTERIOR_RADIUS, call the game's own idempotent lazy-loader (0x562540:
 * loads graphics once + refreshes the 10s keep-alive; roof stays on). The
 * game's visibility diff only hides hands from its OWN set, so it won't fight
 * this. VEH-guarded like every other game call. */
static void fp_load_nearby_interiors(void *gw)
{
    if (!g_fp_mode || g_interiors_dead || !g_nearest_town || !g_interior_load) return;
    static int cooldown;
    if (cooldown > 0) { cooldown--; return; }
    cooldown = 60;                                  /* ~1 Hz at 60 fps */

    void *pc = first_player_char(gw);
    Vec3 here;
    if (!pc || !char_position(pc, &here)) return;

    void *mgr = readable((void *)(g_base + RVA_TOWNMGR_PTR), 8)
        ? *(void **)(g_base + RVA_TOWNMGR_PTR) : NULL;
    if (!readable(mgr, 8)) return;

    if (setjmp(g_guard_jb)) {
        g_interiors_dead = 1;
        logline("interior preload FAULTED -- disabled for this session");
        return;
    }
    g_guard_armed = 1;
    int loaded = 0;
    void *town = g_nearest_town(mgr, &here, 0);
    if (readable(town, 8) && in_module(*(void ***)town)) {
        town_getlist_t getlist = (town_getlist_t)(*(void ***)town)[TOWN_GETLIST_VTOFF];
        if (in_module((void *)getlist)) {
            void *lek = getlist(town, BLDG_LIST_TYPE, NULL);
            if (readable(lek, 0x18)) {
                unsigned int n = *(unsigned int *)((uintptr_t)lek + 0x8);
                void **data = *(void ***)((uintptr_t)lek + 0x10);
                if (n < 4096 && readable(data, (size_t)n * sizeof(void *))) {
                    for (unsigned int i = 0; i < n; i++) {
                        void *b = data[i];
                        if (!readable(b, BLDG_INTERIOR + 8)) continue;
                        /* skip ruins: the vanilla updater gates on isDestroyed()
                         * too -- refreshing a ruin resurrects its intact
                         * walls/roof at low LOD */
                        if (*(unsigned char *)((uintptr_t)b + BLDG_DESTROYED)) continue;
                        void *bi = *(void **)((uintptr_t)b + BLDG_INTERIOR);
                        if (!readable(bi, 0x30)) continue;   /* no interior layout */
                        float bx = *(float *)((uintptr_t)b + BLDG_POS_X);
                        float bz = *(float *)((uintptr_t)b + BLDG_POS_X + 8);
                        float ddx = bx - here.x, ddz = bz - here.z;
                        if (ddx * ddx + ddz * ddz > INTERIOR_RADIUS * INTERIOR_RADIUS)
                            continue;
                        g_interior_load(bi);
                        loaded++;
                    }
                }
            }
        }
    }
    g_guard_armed = 0;
    static int last_loaded = -1;
    if (loaded != last_loaded) {                    /* log on change only */
        last_loaded = loaded;
        logline("interior preload: %d building(s) in range", loaded);
    }
}

/* CameraClass::update hook: right after the game's follow camera runs, compute
 * and apply the FP camera FRESH (fp_camera_override), so every consumer later
 * in the same frame -- foliage paging, mesh LOD, shadow cascades, culling,
 * render -- sees ONE consistent, current camera. (Re-asserting last frame's
 * eye here instead caused shadow shimmer on distant meshes + rotation jitter
 * while moving: mid-frame passes used a one-frame-stale camera vs the render.)
 * Also snap the CENTER (look-at/focus) node to the eye while moving: it is the
 * lagging source grass paging keys off (proven fix for underfoot pop-in). At
 * idle the center stays vanilla for the floating-origin T calibration. */
static void hooked_cam_update(void *cam, char controlEnabled)
{
    InterlockedIncrement(&g_cam_heartbeat);
    g_cam_update_orig(cam, controlEnabled);
    /* Fully inert unless FP is (or was just) engaged: at the main menu / load
     * screens this hook fires while the game is half-initialised, and running
     * the override there crashed the title screen. */
    if (!g_fp_mode && !g_ovr_prev) return;
    if (g_gw_cache) fp_camera_override(g_gw_cache);   /* fresh eye/ori, mid-frame */
    if (!g_fp_mode || !g_have_eye) return;
    if (!readable(cam, CC_FREECAM + 1)) return;
    if (*(unsigned char *)((uintptr_t)cam + CC_FREECAM)) return;   /* free-cam: hands off */
    if (g_was_moving && g_node_set_dpos) {
        void *center = *(void **)((uintptr_t)cam + CC_CENTER);
        if (readable(center, 8)) g_node_set_dpos(center, &g_last_eye);
    }
}

static void hooked_mainloop(void *gw, float time)
{
    InterlockedIncrement(&g_heartbeat);   /* watchdog: proves the hook is live */
    g_gw_cache = gw;               /* CameraClass::update fires inside the frame */
    g_frame_dt = time;             /* stutter diag */
    g_mainloop_orig(gw, time);     /* run the game's frame first */

    poll_input();                  /* every frame: catch toggle edges */
    if (gw) camera_lock(gw);       /* every frame: assert/release the lock */
    /* Camera now runs mid-frame via the CameraClass::update hook (consistent
     * camera for shadows/LOD/foliage/render); this is only a fallback if that
     * hook failed to install. */
    if (gw && !g_cam_update_orig) fp_camera_override(gw);
    if (gw) fp_movement(gw, time); /* every frame: WASD -> custom motion drive */
    if (gw) fp_load_nearby_interiors(gw); /* ~1 Hz: preload nearby building interiors */

    DWORD now = GetTickCount();
    if (gw && (now - g_last_tick_ms) >= 1000) {   /* 1 Hz observation log */
        g_last_tick_ms = now;
        fp_tick(gw);
    }
}

/* Hook re-arm watchdog. Other mods that hook the game late (notably RE_Kenshi,
 * whose KenshiLib installs many detours during game-data init) can leave our
 * per-frame hook installed-but-not-firing. This thread watches the heartbeat:
 * if it isn't advancing, it re-installs the per-frame hook so we land on top of
 * whatever the other mod did. RE_Kenshi does NOT hook mainLoop itself, so
 * remove+recreate here restores the true original bytes -- it won't disturb
 * its hooks. Once the heartbeat flows, the watchdog goes quiet. */
static DWORD WINAPI hook_watchdog(void *unused)
{
    (void)unused;
    LONG last_main = 0, last_cam = 0;
    int rearms = 0, main_ok = 0, cam_ok = 0;
    for (;;) {
        Sleep(2000);
        LONG hm = g_heartbeat, hc = g_cam_heartbeat;
        int main_flow = (hm != last_main), cam_flow = (hc != last_cam);
        last_main = hm; last_cam = hc;
        if (main_flow && !main_ok) { main_ok = 1; logline("watchdog: per-frame hook LIVE"); }
        if (cam_flow && !cam_ok)   { cam_ok = 1;  logline("watchdog: camera-update hook LIVE"); }
        if (main_flow) continue;                 /* healthy */
        /* per-frame hook not firing. Report ONCE which hooks are alive (RE_Kenshi
         * coexistence diagnostic), try a few re-arms, then stay quiet -- no point
         * flooding the log every 2s forever. */
        if (rearms < 6)
            logline("watchdog: mainloop SILENT (main hb=%ld cam hb=%ld) -- cam %s",
                    (long)hm, (long)hc, cam_flow ? "FIRING" : "silent");
        if (rearms >= 5) continue;               /* re-arm is proven useless if it
                                                  * never takes; stop the churn */
        rearms++;
        MH_DisableHook(g_mainloop_target);
        MH_RemoveHook(g_mainloop_target);
        g_mainloop_orig = NULL;
        MH_STATUS s = MH_CreateHook(g_mainloop_target, (void *)hooked_mainloop,
                                    (void **)&g_mainloop_orig);
        if (s == MH_OK) s = MH_EnableHook(g_mainloop_target);
        logline("watchdog: re-armed per-frame hook (attempt %d, status %d)", rearms, (int)s);
    }
}

/* Low-level keyboard hook: latch the FP-toggle keydown at event time. On
 * native Windows a bare Alt press can freeze the game loop in the system-menu
 * modal until release, so frame-polling GetAsyncKeyState misses the entire
 * press (first Windows user report); an LL hook still receives the event. */
static LRESULT CALLBACK kbd_ll(int code, WPARAM wp, LPARAM lp)
{
    if (code == HC_ACTION) {
        KBDLLHOOKSTRUCT *k = (KBDLLHOOKSTRUCT *)lp;
        if (k->vkCode == VK_TOGGLE_FP) {
            if (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN) {
                if (!g_toggle_ll_down) {           /* filter autorepeat */
                    g_toggle_ll_down = 1;
                    InterlockedExchange(&g_toggle_edge, 1);
                }
            } else if (wp == WM_KEYUP || wp == WM_SYSKEYUP) {
                g_toggle_ll_down = 0;
            }
        }
    }
    return CallNextHookEx(NULL, code, wp, lp);
}

/* Low-level mouse hook: capture the wheel ourselves (the game clears its own
 * mWheel before our per-frame hook can read it). Look deltas come from the
 * DirectInput device instead (see ensure_dinput). */
static LRESULT CALLBACK mouse_ll(int code, WPARAM wp, LPARAM lp)
{
    if (code == HC_ACTION && wp == WM_MOUSEWHEEL) {
        MSLLHOOKSTRUCT *m = (MSLLHOOKSTRUCT *)lp;
        int delta = (short)HIWORD(m->mouseData);   /* +120 / -120 per notch */
        InterlockedExchangeAdd(&g_wheel_accum, delta);
    }
    return CallNextHookEx(NULL, code, wp, lp);
}

/* Install one hook through the active backend. Returns 1 on success.
 * KenshiLib.AddHook does create+enable in one call (SUCCESS==0). */
static int install_hook(void *target, void *detour, void **original)
{
    if (g_klib_addhook)
        return g_klib_addhook(target, detour, original) == 0;
    if (MH_CreateHook(target, detour, original) != MH_OK) return 0;
    return MH_EnableHook(target) == MH_OK;
}

__declspec(dllexport) void dllStartPlugin(void)
{
    g_log = fopen("KenshiFP.log", "w");
    g_base = (uintptr_t)GetModuleHandleA(NULL);

    /* Select the address table by the mainLoop prologue signature. Steam 1.0.68
     * is the base target; RE_Kenshi downgrades to Steam 1.0.65 (its bundled exe),
     * so match both builds. Neither -> unsupported, disable. MUST run before any
     * RVA_* use (they now read the active table g_rva). */
    {
        static const unsigned char SIG_MAINLOOP[8] =
            { 0x48,0x8b,0xc4,0x56,0x57,0x41,0x54,0x48 };
        unsigned char *p68 = (unsigned char *)(g_base + T_1068.MAINLOOP);
        unsigned char *p65 = (unsigned char *)(g_base + T_1065.MAINLOOP);
        if (readable(p68, 8) && memcmp(p68, SIG_MAINLOOP, 8) == 0) { g_rva = T_1068; g_build = 68; }
        else if (readable(p65, 8) && memcmp(p65, SIG_MAINLOOP, 8) == 0) { g_rva = T_1065; g_build = 65; }
        else g_wrong_build = 1;
    }

    g_follow_object  = (follow_object_t)(g_base + RVA_FOLLOW_OBJECT);
    g_stop_following = (stop_follow_t)(g_base + RVA_STOP_FOLLOW);
    g_charmove_setdest = (charmove_setdest_t)(g_base + RVA_CHARMOVE_SETDEST);
    g_char_setdest     = (char_setdest_t)(g_base + RVA_CHAR_SETDEST);
    g_move_to_pos      = (move_to_pos_t)(g_base + RVA_MOVE_TO_POS);
    g_nearest_town     = (nearest_town_t)(g_base + RVA_NEAREST_TOWN);
    g_interior_load    = (interior_load_t)(g_base + RVA_INTERIOR_LOAD);
    AddVectoredExceptionHandler(1, veh_guard);   /* crash guard for game-fn calls */
    {
        HMODULE terr = GetModuleHandleA("Plugin_Terrain_x64.dll");
        if (terr) {
            g_terrain_getheight =
                (terrain_getheight_t)GetProcAddress(terr, TERRAIN_GETHEIGHT_SYM);
            g_terrain_intersect =
                (terrain_intersect_t)GetProcAddress(terr, TERRAIN_INTERSECT_SYM);
        }
        logline("Terrain getHeight=%s intersect=%s",
                g_terrain_getheight ? "ok" : "MISSING",
                g_terrain_intersect ? "ok" : "MISSING");
    }
    g_get_bone_world   = (get_bone_world_t)(g_base + RVA_GET_BONE_WORLD);

    g_mouse_hook = SetWindowsHookExA(WH_MOUSE_LL, mouse_ll, g_hinst, 0);
    logline(g_mouse_hook ? "mouse wheel hook installed" : "mouse wheel hook FAILED (err %lu)",
            GetLastError());
    g_kbd_hook = SetWindowsHookExA(WH_KEYBOARD_LL, kbd_ll, g_hinst, 0);
    logline(g_kbd_hook ? "keyboard hook installed (FP toggle capture)"
                       : "keyboard hook FAILED (err %lu) -- polling fallback", GetLastError());

    /* FP look deltas: DirectInput non-exclusive device, created lazily on the
     * first FP frame (the game window must exist). See ensure_dinput. */
    /* MSVC std::string SSO for the head bone name (size<=15 -> inline buffer). */
    memset(g_head_bone, 0, sizeof g_head_bone);
    memcpy(g_head_bone, HEAD_BONE_NAME, sizeof(HEAD_BONE_NAME) - 1);
    *(size_t *)(g_head_bone + 0x10) = sizeof(HEAD_BONE_NAME) - 1;  /* size */
    *(size_t *)(g_head_bone + 0x18) = 15;                          /* capacity */
    logline("KenshiFP loaded (FP + head-bone + FOV + WASD); module base %p", (void *)g_base);

    /* Report the detected build (selection already happened above). */
    {
        wchar_t exw[MAX_PATH]; char exa[MAX_PATH] = {0};
        if (GetModuleFileNameW(NULL, exw, MAX_PATH))
            WideCharToMultiByte(CP_UTF8, 0, exw, -1, exa, MAX_PATH, NULL, NULL);
        logline("running exe: %s", exa[0] ? exa : "(unknown)");
        if (g_wrong_build) {
            unsigned char *pm = (unsigned char *)(g_base + T_1068.MAINLOOP);
            if (readable(pm, 8))
                logline("bytes @0x%x: %02x %02x %02x %02x %02x %02x %02x %02x",
                        (unsigned)T_1068.MAINLOOP, pm[0],pm[1],pm[2],pm[3],pm[4],pm[5],pm[6],pm[7]);
            logline("*** UNSUPPORTED GAME BUILD -- KenshiFP is DISABLED. ***");
            logline("*** Supported: Steam Kenshi 1.0.68 or 1.0.65 (x64).");
            if (exa[0] && (strstr(exa, "RE_Kenshi") || strstr(exa, "re_kenshi")))
                logline("*** Running RE_Kenshi's bundled exe but its build signature "
                        "was not recognised -- report this log.");
        } else {
            logline("game build detected: Steam 1.0.%d%s", g_build,
                    g_build == 65 ? " (RE_Kenshi downgrade)" : "");
        }
    }

    /* Resolve Ogre node world-transform setters from OgreMain_x64.dll. */
    HMODULE ogre = GetModuleHandleA("OgreMain_x64.dll");
    if (ogre) {
        g_node_set_pos  = (node_set_pos_t)GetProcAddress(ogre, OGRE_SETPOS_SYM);
        g_node_set_ori  = (node_set_ori_t)GetProcAddress(ogre, OGRE_SETORI_SYM);
        g_node_set_dori = (node_set_dori_t)GetProcAddress(ogre, OGRE_SETDORI_SYM);
        g_node_set_dpos = (node_set_dpos_t)GetProcAddress(ogre, OGRE_SETDPOS_SYM);
        g_node_get_dpos = (node_get_dpos_t)GetProcAddress(ogre, OGRE_GETDPOS_SYM);
        g_cam_set_fovy  = (cam_set_fovy_t)GetProcAddress(ogre, OGRE_SETFOVY_SYM);
        g_cam_get_fovy  = (cam_get_fovy_t)GetProcAddress(ogre, OGRE_GETFOVY_SYM);
        g_cam_set_nearclip = (cam_set_nearclip_t)GetProcAddress(ogre, OGRE_SETNEARCLIP_SYM);
        g_cam_get_nearclip = (cam_get_nearclip_t)GetProcAddress(ogre, OGRE_GETNEARCLIP_SYM);
        g_ogre_ready = (g_node_set_dori && g_node_set_dpos && g_node_get_dpos);
        logline("Ogre FOV setters: set=%p get=%p", (void *)g_cam_set_fovy, (void *)g_cam_get_fovy);
    }
    logline(g_ogre_ready ? "Ogre node setters resolved (FP override armed)"
                         : "WARN: Ogre node setters NOT resolved (FP override disabled)");

    /* Cooperative-hooking backend: prefer KenshiLib.AddHook when RE_Kenshi is
     * present (its DLL is loaded), else MinHook. */
    HMODULE klib = GetModuleHandleA("KenshiLib.dll");
    if (klib) g_klib_addhook = (klib_addhook_t)GetProcAddress(klib, KLIB_ADDHOOK_SYM);
    logline(g_klib_addhook ? "KenshiLib detected -- cooperative hooking (RE_Kenshi compat)"
                           : "hooking via MinHook (no KenshiLib present)");

    MH_STATUS mh = g_wrong_build ? MH_ERROR_NOT_INITIALIZED : MH_Initialize();
    int ok = 0;
    if (!g_wrong_build && (mh == MH_OK || mh == MH_ERROR_ALREADY_INITIALIZED || g_klib_addhook)) {
        void *target = (void *)(g_base + RVA_MAINLOOP);
        g_mainloop_target = target;
        ok = install_hook(target, (void *)hooked_mainloop, (void **)&g_mainloop_orig);
        logline(ok ? "per-frame hook installed (mainLoop_GPUSensitiveStuff)"
                   : "per-frame hook FAILED");
        /* re-arm watchdog: only meaningful on the MinHook path (KenshiLib
         * cooperates, nothing to re-arm). Still logs the heartbeat diagnostic. */
        if (ok) CloseHandle(CreateThread(NULL, 0, hook_watchdog, NULL, 0, NULL));

        /* CameraClass::update hook: mid-frame FP-eye re-assert (foliage/LOD fix). */
        {
            void *cu = (void *)(g_base + RVA_CAM_UPDATE);
            int mh2ok = install_hook(cu, (void *)hooked_cam_update,
                                     (void **)&g_cam_update_orig);
            logline(mh2ok ? "camera update hook installed (mid-frame eye)"
                          : "camera update hook FAILED");
        }

        /* MyGUI cursor: resolve + hook setPointer to hide the default arrow. */
        HMODULE mygui = GetModuleHandleA(MYGUI_DLL);
        if (mygui) {
            g_pm_setvisible  = (pm_setvisible_t)GetProcAddress(mygui, MYGUI_SETVISIBLE_SYM);
            g_pm_getdefault  = (pm_getdefault_t)GetProcAddress(mygui, MYGUI_GETDEFAULT_SYM);
            g_pm_getinstance = (pm_getinstance_t)GetProcAddress(mygui, MYGUI_GETINSTANCE_SYM);
            g_gui_getinstance = (gui_getinstance_t)GetProcAddress(mygui, MYGUI_GUI_GETINSTANCE_SYM);
            g_gui_createwidget = (gui_createwidget_t)GetProcAddress(mygui, MYGUI_CREATEWIDGET_SYM);
            g_imgbox_setimage = (imgbox_setimage_t)GetProcAddress(mygui, MYGUI_SETIMAGETEX_SYM);
            g_widget_setvisible = (widget_setvisible_t)GetProcAddress(mygui, MYGUI_WIDGET_SETVIS_SYM);
            g_widget_inhvis = (widget_inhvis_t)GetProcAddress(mygui, MYGUI_WIDGET_INHVIS_SYM);
            HMODULE ogremod = GetModuleHandleA("OgreMain_x64.dll");
            if (ogremod) {
                g_rgm_getsingleton = (rgm_getsingleton_t)GetProcAddress(ogremod, OGRE_RGM_GETSINGLETON_SYM);
                g_rgm_addlocation  = (rgm_addlocation_t)GetProcAddress(ogremod, OGRE_RGM_ADDLOCATION_SYM);
                g_rgm_creategroup  = (rgm_creategroup_t)GetProcAddress(ogremod, OGRE_RGM_CREATEGROUP_SYM);
                g_rgm_initgroup    = (rgm_initgroup_t)GetProcAddress(ogremod, OGRE_RGM_INITGROUP_SYM);
            }
            void *sp = GetProcAddress(mygui, MYGUI_SETPOINTER_SYM);
            MH_STATUS mh3 = sp ? MH_CreateHook(sp, (void *)hooked_setpointer,
                                               (void **)&g_pm_setpointer_orig)
                               : MH_ERROR_NOT_EXECUTABLE;
            if (mh3 == MH_OK) mh3 = MH_EnableHook(sp);
            logline(mh3 == MH_OK ? "MyGUI setPointer hook installed (hide default cursor)"
                                 : "MyGUI setPointer hook FAILED (MH_STATUS %d)", (int)mh3);
        } else {
            logline("MyGUIEngine_x64.dll not found — cursor hide disabled");
        }
    } else if (!g_wrong_build) {
        logline("MH_Initialize failed (MH_STATUS %d)", (int)mh);
    }

    logline(ok ? "KenshiFP active: RIGHT ALT = first-person toggle; mouse = look; WASD = move; wheel = speed"
               : (g_wrong_build ? "KenshiFP inactive (unsupported game build)"
                                : "KenshiFP FAILED to install per-frame hook"));
}

__declspec(dllexport) void dllStopPlugin(void) { logline("dllStopPlugin"); }

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) { g_hinst = h; DisableThreadLibraryCalls(h); }
    return TRUE;
}
