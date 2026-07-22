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
#define CHAR_ANIM          0x448   /* Character::animation (AnimationClass*) */
#define CHAR_WEAPON_IN_HANDS 0x6D8 /* CharacterHuman::weaponInHands (Weapon*); non-null = drawn */
#define CHAR_MOVEMENT     0x640    /* Character::movement (CharMovement*) */
#define CHAR_RANGEDCOMBAT 0x2F0    /* Character::rangedCombat (RangedCombatClass*) */
#define RC_COMBATMODE     0x36     /* RangedCombatClass::combatMode (bool) */
#define ANIM_SKELETON      0xB8    /* AnimationClass::skeleton (Ogre::OldSkeletonInstance*) */
#define ANIM_RAGDOLL_MASK  0x2f8   /* AnimationClass ragdoll-active-parts bitmask (u32); != 0 = down */
#define GW_FRAMESPEED      0x700   /* GameWorld::frameSpeedMult (float); 1.0 = 1x */
#define RVA_CHARMOVE_SETDEST 0x661270u   /* CharMovement::setDestination (raw move; no navmesh) */
#define RVA_CHAR_SETDEST     0x5c84e0u   /* NOT setDestination: teleport+facing placement
                                          * (3rd arg = Ogre::Quaternion* facing). Unused. */
/* NB: the player move order (playerMoveOrderDefault) is called via the Character
 * VTABLE slot +0x318 (see try_move_to_pos) -- NOT a hard-coded RVA -- so there is
 * deliberately no RVA_MOVE_TO_POS. (The old 0x5d22b0 was never that function.) */
#define CHAR_TASKHOLDER      0x648       /* Character+0x648 -> task holder */
#define TASK_CUR             0x68        /* holder+0x68 = current task */
#define TASK_DESC            0x70        /* task+0x70 = descriptor */
#define TASKDESC_TYPE        0x44        /* descriptor+0x44 = order type id */
#define TASK_MOVE_ID         0x1d        /* Task_Move (player walk order) */
/* Authoritative from KenshiLib Character.h (member/vtable offsets are layout-
 * stable across point releases, unlike RVAs). */
#define CHAR_PRONE_STATE     0xE0        /* _currentProneState (ProneState 0..4) */
#define CHAR_WANTS_GETUP     0xEC        /* playerWantsMeToGetUp (bool) -- the game's OWN get-up req */
#define CHAR_IN_SOMETHING    0x2F8       /* inSomething (UseStuffState: 0 nothing/1 bed/2 prison) */
#define CHAR_VT_PLAYERMOVE   (0x318/8)   /* vtable: playerMoveOrderDefault(Building*,RootObject*,Vector3&) */
#define PS_NORMAL 0                      /* ProneState: 0 normal, 2 crippled, 3 playing-dead */
#define PS_KO 4                          /* ...4 = KO, truly out (cannot get up until recovered) */
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
#define FP_EYE_FORWARD     (g_cfg_eye_fwd)  /* eye forward of the head (config) */
#define FP_MOVE_FORWARD    (g_cfg_move_fwd) /* EXTRA forward push while moving (config) */

/* ---- FOV ----
 * Ogre::Frustum::setFOVy(const Radian&) [Camera inherits it]; Radian == {float}.
 * getFOVy returns a const ref (pointer in RAX -> safe ABI, unlike the by-value
 * get* accessors). We capture the default FOV once, force FP_FOV while in first
 * person, and restore on exit. */
#define OGRE_SETFOVY_SYM  "?setFOVy@Frustum@Ogre@@UEAAXAEBVRadian@2@@Z"
#define OGRE_GETFOVY_SYM  "?getFOVy@Frustum@Ogre@@UEBAAEBVRadian@2@XZ"
#define FP_FOV_DEG        (g_cfg_fov)  /* vertical FOV while first-person (config) */
#define DEG2RAD           0.01745329f

/* ---- near clip ----
 * Kenshi's RTS camera sits far from everything, so its near clip is large; at
 * eye level that clips nearby geometry and you see through walls/meshes. Pull it
 * in while first-person. Frustum::setNearClipDistance(float) / getNearClipDistance
 * (returns Real=float by value -> XMM0, safe scalar ABI). Kenshi is ~10 units/m. */
#define OGRE_SETNEARCLIP_SYM "?setNearClipDistance@Frustum@Ogre@@UEAAXM@Z"
#define OGRE_GETNEARCLIP_SYM "?getNearClipDistance@Frustum@Ogre@@UEBAMXZ"
#define FP_NEARCLIP       (g_cfg_nearclip) /* world units; clip very-near geometry
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
#define OGRE_GETPOS_SYM   "?getPosition@Node@Ogre@@QEBAAEBVVector3@2@XZ"
/* Ogre::OldSkeletonInstance::disableBone(std::string name, bool disable) --
 * Kenshi's own decapitation call; disabling a bone collapses its mesh and
 * PERSISTS across the per-frame animation update. Version-independent export. */
#define OGRE_DISABLEBONE_SYM "?disableBone@OldSkeletonInstance@Ogre@@QEAAXV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@_N@Z"
/* Read the head bone's WORLD orientation, version-independently: skeleton
 * (Character+0x448 anim, +0xB8 skel) -> getBone(name) -> OldBone* -> its
 * derived (world) orientation. Both Ogre exports; getBone takes std::string&,
 * _getDerivedOrientation returns const Quaternion& (safe ABI). */
#define OGRE_GETBONE_SYM  "?getBone@Skeleton@Ogre@@UEBAPEAVOldBone@2@AEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@@Z"
#define OGRE_OLDNODE_GETDORI_SYM "?_getDerivedOrientation@OldNode@Ogre@@UEBAAEBVQuaternion@2@XZ"
#define OGRE_OLDNODE_GETDPOS_SYM "?_getDerivedPosition@OldNode@Ogre@@UEBAAEBVVector3@2@XZ"
#define OGRE_GETDPOS_UPD_SYM  "?_getDerivedPositionUpdated@Node@Ogre@@QEAA?AVVector3@2@XZ"
/* Procedural bone control (additive aim/IK): read animated LOCAL pose, write it
 * back with an offset, flag derived transforms dirty. All OldNode/OldBone. */
#define OGRE_OLDNODE_GETORI_SYM  "?getOrientation@OldNode@Ogre@@UEBAAEBVQuaternion@2@XZ"
#define OGRE_OLDNODE_SETORI_SYM  "?setOrientation@OldNode@Ogre@@UEAAXAEBVQuaternion@2@@Z"
#define OGRE_OLDNODE_NEEDUPD_SYM "?needUpdate@OldNode@Ogre@@UEAAX_N@Z"
#define OGRE_OLDBONE_SETMANUAL_SYM "?setManuallyControlled@OldBone@Ogre@@QEAAX_N@Z"
#define OGRE_GETPARENTSCENENODE_SYM "?getParentSceneNode@MovableObject@Ogre@@QEBAPEAVSceneNode@2@XZ"
#define MYGUI_GETSUBMAIN_SYM "?getSubWidgetMain@SkinItem@MyGUI@@QEAAPEAVISubWidgetRect@2@XZ"
#define MYGUI_SUBSETCOLOUR_SYM "?_setColour@SubSkin@MyGUI@@UEAAXAEBUColour@2@@Z"
/* ---- in-game settings UI (MyGUI, resolved by mangled name from the DLL) ---- */
#define MYGUI_GUI_FINDWIDGET_SYM "?findWidgetT@Gui@MyGUI@@QEAAPEAVWidget@2@AEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@_N@Z"
#define MYGUI_WIDGET_CREATEWIDGET_SYM "?createWidgetT@Widget@MyGUI@@QEAAPEAV12@AEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@0HHHHUAlign@2@0@Z"
#define MYGUI_TABCTRL_ITEMCOUNT_SYM "?getItemCount@TabControl@MyGUI@@QEBA_KXZ"
#define MYGUI_TABCTRL_ITEMAT_SYM "?getItemAt@TabControl@MyGUI@@QEAAPEAVTabItem@2@_K@Z"
/* Hit-testing via MyGUI itself: ask which widget is under the mouse and compare
 * pointers -- avoids the fragile coordinate getters entirely. */
#define MYGUI_INPUT_GETINST_SYM "?getInstancePtr@?$Singleton@VInputManager@MyGUI@@@MyGUI@@SAPEAVInputManager@2@XZ"
#define MYGUI_MOUSEFOCUS_SYM "?getMouseFocusWidget@InputManager@MyGUI@@QEBAPEAVWidget@2@XZ"
#define MYGUI_GETPARENT_SYM  "?getParent@Widget@MyGUI@@QEBAPEAV12@XZ"
#define MYGUI_SCROLL_SETRANGE_SYM "?setScrollRange@ScrollBar@MyGUI@@QEAAX_K@Z"
#define MYGUI_SCROLL_SETPOS_SYM "?setScrollPosition@ScrollBar@MyGUI@@QEAAX_K@Z"
#define MYGUI_SCROLL_GETPOS_SYM "?getScrollPosition@ScrollBar@MyGUI@@QEBA_KXZ"
#define MYGUI_BTN_GETSEL_SYM "?getStateSelected@Button@MyGUI@@QEBA_NXZ"
#define MYGUI_BTN_SETSEL_SYM "?setStateSelected@Button@MyGUI@@QEAAX_N@Z"
#define MYGUI_TEXTBOX_SETCAP_SYM "?setCaption@TextBox@MyGUI@@UEAAXAEBVUString@2@@Z"
#define MYGUI_WINDOW_SETCAP_SYM  "?setCaption@Window@MyGUI@@UEAAXAEBVUString@2@@Z"   /* Window overrides it */
#define MYGUI_USTRING_CTOR_SYM "??0UString@MyGUI@@QEAA@PEBD@Z"    /* UString(const char*) */
#define MYGUI_USTRING_DTOR_SYM "??1UString@MyGUI@@QEAA@XZ"
/* Recursive widget search (vanilla menu widgets carry a layout name-prefix that
 * findWidgetT can't match, so we DFS the widget tree matching the suffix). The
 * Enumerator is returned by value: { bool m_first; Widget** begin; Widget** end }
 * -- read begin(+8)/end(+16) and iterate the pointer range directly. */
#define MYGUI_GUI_GETENUM_SYM "?getEnumerator@Gui@MyGUI@@QEBA?AV?$Enumerator@V?$vector@PEAVWidget@MyGUI@@V?$allocator@PEAVWidget@MyGUI@@@std@@@std@@@2@XZ"
#define MYGUI_WIDGET_GETENUM_SYM "?getEnumerator@Widget@MyGUI@@QEBA?AV?$Enumerator@V?$vector@PEAVWidget@MyGUI@@V?$allocator@PEAVWidget@MyGUI@@@std@@@std@@@2@XZ"
#define MYGUI_WIDGET_GETNAME_SYM "?getName@Widget@MyGUI@@QEBAAEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@XZ"
#define OGRE_ENTITY_GETSKEL_SYM  "?getSkeleton@Entity@Ogre@@QEBAPEAVOldSkeletonInstance@2@XZ"
#define OGRE_ENTITY_GETSKEL_SYM2 "?getSkeleton@Entity@Ogre@@QEBAPEAVSkeletonInstance@2@XZ"

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
        FOLLOW_OBJECT, STOP_FOLLOW, CHARMOVE_SETDEST, CHAR_SETDEST,
        GUI_INV_COUNT, GUI_STATS_BEG, GUI_STATS_END, GUI_WINSTACK, GUI_DLGWND,
        DLG_GETVIS, ESCMENU_PTR, ESC_GETVIS, OVERVIEW_PTR, OVW_GETVIS,
        OPTIONS_PTR, OPT_GETVIS, PROSPECT_PTR, PRO_GETVIS, SAVELOAD_PTR,
        MSGBOX_COUNT, PLAYER_IFACE, CTXMENU_VISIBLE, CAM_UPDATE, TERRAIN_PTR,
        TOWNMGR_PTR, NEAREST_TOWN, INTERIOR_LOAD, GET_BONE_WORLD,
        RANGED_ANIMUPD, GUN_SHOOT, FACE_DIR, GUN_RELOAD, SHEATHE, GUN_CREATEPHYS,
        RC_GETGUN, CAM_MANUAL_SETOZ, SET_DIRECT_MOVE, CHARMOVE_UPDATE;
} addr_table_t;

static const addr_table_t T_1068 = {
    RVA_MAINLOOP, RVA_CAM_INSTANCE, RVA_INPUT_CONTROLENABLED, RVA_INPUT_MWHEEL, RVA_SCENE_CTX,
    RVA_FOLLOW_OBJECT, RVA_STOP_FOLLOW, RVA_CHARMOVE_SETDEST, RVA_CHAR_SETDEST,
    RVA_GUI_INV_COUNT, RVA_GUI_STATS_BEG, RVA_GUI_STATS_END, RVA_GUI_WINSTACK, RVA_GUI_DLGWND,
    RVA_DLG_GETVIS, RVA_ESCMENU_PTR, RVA_ESC_GETVIS, RVA_OVERVIEW_PTR, RVA_OVW_GETVIS,
    RVA_OPTIONS_PTR, RVA_OPT_GETVIS, RVA_PROSPECT_PTR, RVA_PRO_GETVIS, RVA_SAVELOAD_PTR,
    RVA_MSGBOX_COUNT, RVA_PLAYER_IFACE, RVA_CTXMENU_VISIBLE, RVA_CAM_UPDATE, RVA_TERRAIN_PTR,
    RVA_TOWNMGR_PTR, RVA_NEAREST_TOWN, RVA_INTERIOR_LOAD, RVA_GET_BONE_WORLD,
    0x51e4e0,   /* RangedCombatClass::animationUpdate (verified: reads gun+0x28,
                 * me+0x68 -> +0x448 anim, plays "reload 1 phase"; R8 = aimpos) */
    0x43a730,   /* GunClass::shoot(me,target,stat,aimpos&): projectile dir =
                 * getAimDir(aimpos) + skill randomDeviant (verified in decomp) */
    0x665250,   /* CharMovement::faceDirection (derived): stores facing at
                 * this+0xD0; lookatPosition funnels here via vtable+0x30 */
    0x436fb0,   /* GunClass::reloadAmmo: refills ammo(+0x20) from clip(+0x1C),
                 * handles bolt visibility (verified: unique 8b411c894120 body) */
    0x5cc820,   /* CharacterHuman::sheatheWeapon (verified: "hands" slot + anim
                 * at this+0x448); AI re-sheathes our manual draw through this */
    0x436a50,   /* Character gun-visual setup: derives body-entity parent node +
                 * material, calls gun(+0x440)->createPhysical, syncs visibility */
    0x4345c0,   /* RangedCombatClass::getGun: me->getCurrentWeapon()->vt[0x2D8]
                 * ->+0x240 (carried) or turret+0x440 (mounted) */
    0x6ae8e0,   /* CameraClass::manuallySetOrientationAndZoom(quat&, zoom):
                 * center(+0x58) ori + camera(+0x70) local (0,0,z) + manual flag */
    0x333300,   /* CharMovement::setDirectMovement(Vector3& d, float limit):
                 * setMovementMode(MOVE_DIRECTION) + desiredMotion + sqrt(limit).
                 * Engine-native direction-driven locomotion (verified decomp) */
    0x65ffa0,   /* CharMovement::update (via base-vtable+0x58 thunk; contains the
                 * MOVE_DIRECTION consumer branch at +0x11c -- self-verifying) */
};
/* Steam 1.0.65 (RE_Kenshi's downgrade build). Signature-transplant mapped;
 * 0 = TODO (Ghidra pass in progress) or unused-in-client. Same field order. */
static const addr_table_t T_1065 = {
    0x787e70, 0x21322c0, 0x21323f0, 0, 0x21322b8,
    0x6aed00, 0x6aed40, 0x6607e0, 0,
    0x2132810, 0x2132918, 0x2132920, 0x2132960, 0x2132770,
    0x721390, 0x212e4a8, 0x912170, 0x212e4e8, 0x48b0e0,
    0x212e080, 0x3e7100, 0x212da50, 0x48b4c0, 0x212dbc8,
    0x1f28a20, 0x2133630, 0x2132280, 0x6b1540, 0x21322c8,
    0x21330a0, 0x9279c0, 0x561ab0, 0x43ffc0,
    0x51da50,   /* animationUpdate: sig-transplant, matches KenshiLib-0x310 delta */
    0x43a390,   /* GunClass::shoot: sig-transplant (unique 32-byte prologue match) */
    0x6647c0,   /* faceDirection: masked sig-transplant, = KenshiLib-0x310 exactly */
    0x436c10,   /* reloadAmmo: unique body match at same +0x2b, identical prologue */
    0x5cbd90,   /* sheatheWeapon: "hands" xref at +0x3e + identical prologue */
    0x4366b0,   /* gun-visual setup: masked-sig pair match (same 0xe0 sibling gap) */
    0x434220,   /* getGun: unique 24-byte sig (me+0x68 -> getCurrentWeapon call) */
    0x6af0c0,   /* manuallySetOrientationAndZoom: byte-identical prologue, +0x7E0 */
    0x3332c0,   /* setDirectMovement: unique 22-byte prologue match */
    0x65f510,   /* CharMovement::update: unique 28-byte prologue match */
};
static addr_table_t g_rva;   /* active table, selected at load by build signature */

#ifdef KFP_RE_PLUGIN
/* RE_Kenshi build: resolve g_rva by runtime signature scan (version-independent,
 * covers GOG + any build) instead of the hard-coded T_1068/T_1065 tables. */
#include "../re_plugin/rva_sigs.h"
#endif

#undef RVA_MAINLOOP
#undef RVA_CAM_INSTANCE
#undef RVA_INPUT_CONTROLENABLED
#undef RVA_INPUT_MWHEEL
#undef RVA_SCENE_CTX
#undef RVA_FOLLOW_OBJECT
#undef RVA_STOP_FOLLOW
#undef RVA_CHARMOVE_SETDEST
#undef RVA_CHAR_SETDEST
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
#define RVA_RANGED_ANIMUPD      (g_rva.RANGED_ANIMUPD)
#define RVA_GUN_SHOOT           (g_rva.GUN_SHOOT)
#define RVA_FACE_DIR            (g_rva.FACE_DIR)
#define RVA_GUN_RELOAD          (g_rva.GUN_RELOAD)
#define RVA_SHEATHE             (g_rva.SHEATHE)
#define RVA_GUN_CREATEPHYS      (g_rva.GUN_CREATEPHYS)
#define RVA_RC_GETGUN           (g_rva.RC_GETGUN)
#define RVA_CAM_MANUAL_SETOZ    (g_rva.CAM_MANUAL_SETOZ)
#define RVA_SET_DIRECT_MOVE     (g_rva.SET_DIRECT_MOVE)
#define RVA_CHARMOVE_UPDATE     (g_rva.CHARMOVE_UPDATE)
#define RVA_CAM_INSTANCE        (g_rva.CAM_INSTANCE)
#define RVA_INPUT_CONTROLENABLED (g_rva.INPUT_CONTROLENABLED)
#define RVA_INPUT_MWHEEL        (g_rva.INPUT_MWHEEL)
#define RVA_SCENE_CTX           (g_rva.SCENE_CTX)
#define RVA_FOLLOW_OBJECT       (g_rva.FOLLOW_OBJECT)
#define RVA_STOP_FOLLOW         (g_rva.STOP_FOLLOW)
#define RVA_CHARMOVE_SETDEST    (g_rva.CHARMOVE_SETDEST)
#define RVA_CHAR_SETDEST        (g_rva.CHAR_SETDEST)
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
#define LOOK_SENS         (0.0025f * g_cfg_sens) /* radians per mouse count (config-scaled) */

/* ---- player character / position (proven in KenshiMP) ---- */
#define PI_PLAYERCHARS    0x2B0      /* PlayerInterface::playerCharacters (lektor<Character*>) */
#define PI_SELECTED_CHAR  0xF0       /* PlayerInterface::selectedCharacter (hand) */
#define HAND_IDS          0x8        /* hand: vftable(8) then 5 id dwords (+0x8..+0x18) */
#define LEK_COUNT         0x8        /* lektor::count (u32) */
#define LEK_STUFF         0x10       /* lektor::stuff (T*) */
#define GETPOS_VTABLE_SLOT 8         /* Character::getPosition; ABI Vec3*(this RCX, out RDX) */

/* ---- input (Win32, zero engine dependency for stage 0) ---- */
/* ---- user configuration (KenshiFP.ini, hot-reloaded in-game) ------------ */
static int g_cfg_aim_lean = 1;     /* aim_lean: spine bend with weapon drawn */
static int g_cfg_freeaim  = 1;     /* ranged_freeaim: crosshair aim in combat */
static int g_cfg_wheel    = 1;     /* wheel_speed: scrollwheel gait control */
static int g_cfg_vignette = 1;     /* ko_vignette: dark edges while knocked out */
static int g_cfg_key_fp   = 0xA5;  /* key_toggle_fp (VK code; default Right Alt) */
static int g_cfg_key_w    = 0x57;  /* key_forward */
static int g_cfg_key_a    = 0x41;  /* key_left */
static int g_cfg_key_s    = 0x53;  /* key_back */
static int g_cfg_key_d    = 0x44;  /* key_right */
static int g_cfg_key_settings = 0x79;  /* key_settings (VK code; default F10) -- toggles the settings window */
static float g_cfg_fov      = 70.0f;   /* fov: vertical degrees in FP */
static float g_cfg_nearclip = 1.0f;    /* near_clip: world units */
static float g_cfg_sens     = 1.0f;    /* sensitivity: mouse multiplier */
static float g_cfg_eye_fwd  = 2.0f;    /* eye_forward: eye ahead of head bone */
static float g_cfg_move_fwd = 1.5f;    /* move_forward: extra push at full-run speed */
static float g_cfg_move_ref = 60.0f;   /* move_speed_ref: feet units/sec that counts as "full run" */
static float g_cfg_lean     = 0.5f;    /* aim_lean_amount: spine bend gain */
#define VK_TOGGLE_FP      (g_cfg_key_fp)
#define VK_W (g_cfg_key_w)
#define VK_A (g_cfg_key_a)
#define VK_S (g_cfg_key_s)
#define VK_D (g_cfg_key_d)

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
typedef const Vec3 *(*node_get_pos_t)(void *node);            /* LOCAL getPosition (const Vector3&) */
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
static node_get_pos_t  g_node_get_pos;  /* Ogre::Node::getPosition (LOCAL) */
static float g_vanilla_zoom = 40.0f;    /* camera-node local Z = vanilla zoom (saved on FP enter) */
typedef void (*disable_bone_t)(void *skel, const void *name /*std::string*/, char disable);
static disable_bone_t g_disable_bone;   /* OldSkeletonInstance::disableBone */
typedef void *(*skel_getbone_t)(void *skel, const void *name);  /* -> OldBone* */
typedef const Quat *(*oldnode_getdori_t)(void *node);           /* const Quaternion& */
static skel_getbone_t g_skel_getbone;
static oldnode_getdori_t g_oldnode_getdori;
typedef const Vec3 *(*oldnode_getdpos_t)(void *node);   /* skeleton-space derived pos */
static oldnode_getdpos_t g_oldnode_getdpos;
typedef Vec3 *(*node_getdpos_upd_t)(void *node, Vec3 *ret);  /* FORCES transform recompute */
static node_getdpos_upd_t g_node_getdpos_upd;
typedef const Quat *(*oldnode_getori_t)(void *node);           /* const Quaternion& (LOCAL) */
typedef void (*oldnode_setori_t)(void *node, const Quat *q);   /* setOrientation(LOCAL) */
typedef void (*oldnode_needupd_t)(void *node, char force);     /* needUpdate(bool) */
typedef void (*oldbone_setmanual_t)(void *bone, char manual);  /* OldBone::setManuallyControlled */
static oldnode_getori_t   g_oldnode_getori;
static oldnode_setori_t   g_oldnode_setori;
static oldnode_needupd_t  g_oldnode_needupd;
static oldbone_setmanual_t g_oldbone_setmanual;
typedef void *(*get_parent_scenenode_t)(void *movable);
static get_parent_scenenode_t g_get_parent_scenenode;
typedef void *(*entity_getskel_t)(void *entity);
static entity_getskel_t g_entity_getskel;
static int g_anim_ent_off = -1;         /* AnimationClass offset of body Entity* (probed) */
static unsigned char g_bone_spine1[32], g_bone_spine2[32], g_bone_neck[32];  /* SSO std::strings */
static unsigned char g_bone_rootspine[32];      /* "Bip01 Spine": facing ref (not controlled) */
static int g_spine_ready;                       /* spine-bend exports + names resolved */
static int g_spine_manual;                      /* bones set manually-controlled (once) */
static Quat g_spine_rest[3];                    /* captured reference pose the aim pivots around */
static Vec3 g_fwd_local; static int g_have_fwd; /* body forward in root-local frame (calibrated) */
/* --- ranged free-aim (RangedCombatClass::animationUpdate hook) --- */
#define RC_AIMPOS 0x38                          /* RangedCombatClass::currentAimPos (Vector3) */
#define RC_ME     0x68                          /* RangedCombatClass::me (Character*) */
typedef void (*ranged_animupd_t)(void *rc, float ft, Vec3 *aimpos, void *target);
static ranged_animupd_t g_ranged_animupd_orig;
static void *g_player_pc;                       /* player char, cached each frame for hooks */
#define RC_GUN     0x28                         /* RangedCombatClass::gun (GunClass*) */
#define RC_STAT    0x30                         /* RangedCombatClass::currentStat */
#define GUN_AMMO   0x20                         /* GunClass ammo count (shoot decrements it) */
static void manual_fire_update(void *pcx);      /* defined with the hooks below */
static int fp_aim_point(Vec3 *out);
static void make_mstr(unsigned char *b32, const char *s);
static int g_aim_mode;                          /* R-toggled: weapon raised, manual aim */
/* TABLED: out-of-combat manual aim (R raise + LMB fire). The AI task layer owns
 * the draw/aim pipeline too tightly for field-forcing; revisit in the KenshiLib
 * plugin rewrite. In-combat free-aim (pose/projectile/facing hooks) stays ON. */
#define KFP_MANUAL_AIM 0
/* Verbose periodic diagnostics ([tick]/[foliage]/[spine]/[orient]/[ui]).
 * 0 for release: load-time lines and FAULT/error paths always stay. */
#define KFP_DEBUG_LOG 0
#define VK_AIM_TOGGLE 0x52                      /* R */
#define RC_STATE_OFF 0x00                       /* RangedCombatClass::state (SHOOTING=0) */
static float g_head_above = 2.0f;               /* head Y over feet; small => truly prone/KO */
static Quat g_qref; static int g_have_qref;   /* head orientation while upright */
static float g_down_blend;                     /* 0=upright look .. 1=follow head (ragdoll) */
static int   g_is_down;                         /* ragdolled this frame; freezes look input */
static jmp_buf g_guard_jb;                     /* VEH crash-guard (used from get_head_quat on) */
static volatile LONG g_guard_armed;
static int g_head_hidden;               /* our head-hide state (>1x fast-forward) */
static void *g_head_hidden_char;        /* the exact character whose head we hid */
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
static void *g_vignette;           /* fullscreen KO tunnel-vignette ImageBox */
static void *g_black_ov;           /* fullscreen solid-black blackout ImageBox */
typedef void *(*gui_getsubmain_t)(void *widget);
typedef void (*gui_subsetcolour_t)(void *sub, const float *rgba);
static gui_getsubmain_t   g_gui_getsubmain;
static gui_subsetcolour_t g_gui_subsetcolour;

/* ---- in-game settings UI ---- */
typedef void *(*gui_findwidget_t)(void *gui, const void *name_str, char throw_);
typedef void *(*widget_createwidget_t)(void *self, const void *type, const void *skin,
                                       int l, int t, int w, int h, int align, const void *name);
typedef size_t (*tabctrl_itemcount_t)(void *tabctrl);
typedef void *(*tabctrl_itemat_t)(void *tabctrl, size_t index);
typedef void *(*input_getinst_t)(void);
typedef void *(*mousefocus_t)(void *inputmgr);
typedef void *(*getparent_t)(void *widget);
typedef void (*scroll_setrange_t)(void *sb, size_t range);
typedef void (*scroll_setpos_t)(void *sb, size_t pos);
typedef size_t (*scroll_getpos_t)(void *sb);
typedef char (*btn_getsel_t)(void *btn);
typedef void (*btn_setsel_t)(void *btn, char sel);
typedef void (*textbox_setcap_t)(void *tb, const void *ustr);
typedef void *(*ustring_ctor_t)(void *self, const char *cstr);
typedef void (*ustring_dtor_t)(void *self);
typedef void (*getenum_t)(void *self, void *enum_retbuf24);   /* by-value: this=RCX, ret=RDX */
typedef const void *(*widget_getname_t)(void *widget);        /* -> const std::string* */
static gui_findwidget_t      g_gui_findwidget;
static widget_createwidget_t g_widget_createwidget;
static tabctrl_itemcount_t   g_tabctrl_itemcount;
static tabctrl_itemat_t      g_tabctrl_itemat;
static input_getinst_t       g_input_getinst;
static mousefocus_t          g_mousefocus;
static getparent_t           g_widget_getparent;
static scroll_setrange_t     g_scroll_setrange;
static scroll_setpos_t       g_scroll_setpos;
static scroll_getpos_t       g_scroll_getpos;
static btn_getsel_t          g_btn_getsel;
static btn_setsel_t          g_btn_setsel;
static textbox_setcap_t      g_textbox_setcap;
static textbox_setcap_t      g_window_setcap;   /* Window::setCaption (title bar) */
static ustring_ctor_t        g_ustring_ctor;
static ustring_dtor_t        g_ustring_dtor;
static getenum_t             g_gui_getenum, g_widget_getenum;
static widget_getname_t      g_widget_getname;

/* Fade helper: MyGUI has no exported Widget::setAlpha, but the main sub-widget
 * accepts an RGBA colour multiply -- alpha included. VEH-unsafe-free (pure
 * pointer walks + one virtual-free exported call). */
static int readable(const void *p, size_t n);
static int char_prone_state(void *pc);   /* fwd: used by the down-state detect before its def */
static void widget_alpha(void *w, float a)
{
    if (!w || !g_gui_getsubmain || !g_gui_subsetcolour) return;
    void *sub = g_gui_getsubmain(w);
    if (!readable(sub, 8)) return;
    float rgba[4] = { 1.0f, 1.0f, 1.0f, a };
    g_gui_subsetcolour(sub, rgba);
}
static int g_crosshair_red;        /* crosshair currently on the red (enemy) texture */
static int g_pointer_default = 1;  /* current MyGUI pointer is the default (arrow) */
static charmove_setdest_t g_charmove_setdest;  /* CharMovement::setDestination (raw move) */
static char_setdest_t g_char_setdest;          /* teleport+facing placement (unused) */
static int g_movetopos_dead;                   /* set if playerMoveOrderDefault ever faults */
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
static int g_was_direct;           /* current WASD hold uses engine direct drive */
static int g_stuck_frames;         /* direct-driving but body not translating (seated/bed/pinned) */
static int g_eye_from_head;        /* this frame's eye came from the head bone (not fallback) */
/* Direct-drive intent, published by fp_movement and ENFORCED inside the
 * CharMovement::update hook -- applying there (before the original runs) wins
 * the race against combat AI locomotion, which otherwise resets the movement
 * mode every update and freezes WASD during fights. */
static void *g_dm_mv;              /* the player's CharMovement while driving */
static Vec3  g_dm_dir;             /* desired world direction */
static int   g_dm_speed;           /* MoveSpeed: 0 walk / 1 jog / 2 run */
static volatile LONG g_dm_active;  /* WASD held, direct drive on */
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
static int g_ui_moveblock;         /* HARD block (control disabled: dialogue/cutscene); halts WASD */
static int g_settings_open;        /* our settings window is showing -> free the cursor like a panel */
static int g_dbg_control;          /* controlEnabled read, for verification */
static float g_tx, g_tz;           /* game->Ogre translation, calibrated when still */
static int g_have_t;
static float g_prevraw_tx, g_prevraw_tz;  /* prev-frame raw (centerW-feet), for rebase detect */
static int   g_have_prevraw;              /* g_prevraw_* is valid (continuous FP) */
static float g_last_feet_x, g_last_feet_z;  /* prev-frame feet (horizontal), for speed calc */
static int   g_have_last_feet;              /* g_last_feet_* is valid */
static float g_move_speed;                  /* smoothed horizontal feet speed, units/sec */

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
static int char_valid(void *c)
{
    if (!readable(c, 0x60)) return 0;
    /* Reject anything whose vtable isn't in the exe (menu false positives). */
    void **vt = *(void ***)c;
    return readable(vt, (GETPOS_VTABLE_SLOT + 1) * 8) && in_module(vt);
}

/* The character FP controls: the game's CURRENTLY SELECTED squad member
 * (PlayerInterface::selectedCharacter hand, resolved by matching hand ids
 * against the player-characters list), falling back to squad slot 0. Following
 * the selection is what lets camera-locking another squad member swap FP to
 * them -- v1 hardwired stuff[0] and fought every switch attempt. */
static void *first_player_char(void *gw)
{
    if (!readable(gw, GW_PLAYER + 8)) return NULL;
    void *player = *(void **)((uintptr_t)gw + GW_PLAYER);
    if (!readable(player, PI_PLAYERCHARS + LEK_STUFF + 8)) return NULL;
    uint32_t count = *(uint32_t *)((uintptr_t)player + PI_PLAYERCHARS + LEK_COUNT);
    void **stuff   = *(void ***)((uintptr_t)player + PI_PLAYERCHARS + LEK_STUFF);
    if (count == 0 || count > 4096 || !readable(stuff, 8)) return NULL;

    if (readable((void *)((uintptr_t)player + PI_SELECTED_CHAR + HAND_IDS), 20)) {
        uint32_t *sel = (uint32_t *)((uintptr_t)player + PI_SELECTED_CHAR + HAND_IDS);
        if (sel[0] != 0xb) {                 /* type 0xb = null-hand (nobody) */
            for (uint32_t i = 0; i < count && i < 256; i++) {
                void *c = readable(&stuff[i], 8) ? stuff[i] : NULL;
                if (!c || !readable((void *)((uintptr_t)c + CHAR_HANDLE + HAND_IDS), 20))
                    continue;
                uint32_t *h = (uint32_t *)((uintptr_t)c + CHAR_HANDLE + HAND_IDS);
                if (h[0]==sel[0] && h[1]==sel[1] && h[2]==sel[2]
                    && h[3]==sel[3] && h[4]==sel[4] && char_valid(c))
                    return c;
            }
        }
    }
    return char_valid(stuff[0]) ? stuff[0] : NULL;   /* fallback: squad slot 0 */
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

/* Clear the game InputHandler's stuck keyboard-modifier flags. The instance's
 * controlEnabled sits at InputHandler+0xD0 (== RVA_INPUT_CONTROLENABLED, already
 * resolved), and ctrl/shift/alt are the next three bytes (+0xD8/+0xD9/+0xDA).
 * Right Alt is AltGr on many layouts: the OS expands it to Left-Ctrl + Right-Alt,
 * and if that phantom Ctrl's key-UP is missed during an FP toggle, InputHandler
 * ::ctrl latches on -- and since move/order commands test CTRL_MASK, EVERYTHING
 * then reads as Ctrl-held = sneak. Clearing the flags un-sticks it. */
static void clear_input_mods(void)
{
    uintptr_t ce = g_base + RVA_INPUT_CONTROLENABLED;   /* InputHandler + 0xD0 */
    if (readable((void *)(ce + 0xA), 1)) {
        *(unsigned char *)(ce + 8) = 0;    /* ctrl  (+0xD8) */
        *(unsigned char *)(ce + 9) = 0;    /* shift (+0xD9) */
        *(unsigned char *)(ce + 0xA) = 0;  /* alt   (+0xDA) */
    }
}

static int g_clearmod_frames;      /* countdown: keep clearing stuck mods after a toggle */

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
        /* Toggling with Right Alt (AltGr) can leave InputHandler::ctrl stuck ->
         * everything reads as sneak. Scrub the phantom modifiers for a short
         * window (the missed key-up may lag the toggle by a few frames). */
        g_clearmod_frames = 20;
    }
    if (g_clearmod_frames > 0 && !(GetAsyncKeyState(VK_TOGGLE_FP) & 0x8000)) {
        g_clearmod_frames--;
        clear_input_mods();
    }
    g_toggle_was_down = down;

    /* [sneak diag] Dump the game input state so we can SEE what "sneak" is:
     * InputHandler modifier/pan bytes (base 0x2133370: +0xD0 controlEnabled,
     * +0xD4 gameMode, +0xD8 ctrl, +0xD9 shift, +0xDA alt, +0xDB..DE arrows) and
     * the selected character's stealthMode (Character+0xD4). Throttled; any
     * physical key held is noted so we can correlate. */
    if (KFP_DEBUG_LOG) {
        static int sd;
        if ((++sd % 30) == 0) {
            unsigned char *ih = (unsigned char *)(g_base + RVA_INPUT_CONTROLENABLED - 0xD0); /* InputHandler base */
            int anykey = 0;
            for (int vk = 0x08; vk <= 0xFE; vk++)
                if ((GetAsyncKeyState(vk) & 0x8000) && vk != VK_TOGGLE_FP) { anykey = vk; break; }
            int stealth = -1;
            if (g_player_pc && readable((void *)((uintptr_t)g_player_pc + 0xD4), 1))
                stealth = *(unsigned char *)((uintptr_t)g_player_pc + 0xD4);
            if (readable(ih, 0xE0))
                logline("[sneak] fp=%d ctrlEn=%d gameMode=%d ctrl=%d shift=%d alt=%d arrows=%d%d%d%d stealthMode=%d heldVK=0x%02X",
                        g_fp_mode, ih[0xD0], ih[0xD4], ih[0xD8], ih[0xD9], ih[0xDA],
                        ih[0xDB], ih[0xDC], ih[0xDD], ih[0xDE], stealth, anykey);
        }
    }
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

/* --- quaternion helpers (Ogre {w,x,y,z}) --- */
static Quat quat_mul(Quat a, Quat b)
{
    Quat r;
    r.w = a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z;
    r.x = a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y;
    r.y = a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x;
    r.z = a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w;
    return r;
}
static Quat quat_conj(Quat q) { Quat r = { q.w, -q.x, -q.y, -q.z }; return r; }
static Quat quat_norm(Quat q)
{
    float n = sqrtf(q.w*q.w + q.x*q.x + q.y*q.y + q.z*q.z);
    if (n < 1e-6f) { Quat id = {1,0,0,0}; return id; }
    q.w/=n; q.x/=n; q.y/=n; q.z/=n; return q;
}
/* shortest-arc slerp from a to b by t in [0,1] */
static Quat quat_slerp(Quat a, Quat b, float t)
{
    float d = a.w*b.w + a.x*b.x + a.y*b.y + a.z*b.z;
    if (d < 0) { b.w=-b.w; b.x=-b.x; b.y=-b.y; b.z=-b.z; d=-d; }
    if (d > 0.9995f) {   /* nearly identical -> lerp */
        Quat r = { a.w+(b.w-a.w)*t, a.x+(b.x-a.x)*t, a.y+(b.y-a.y)*t, a.z+(b.z-a.z)*t };
        return quat_norm(r);
    }
    float th = acosf(d), s = sinf(th);
    float wa = sinf((1-t)*th)/s, wb = sinf(t*th)/s;
    Quat r = { wa*a.w+wb*b.w, wa*a.x+wb*b.x, wa*a.y+wb*b.y, wa*a.z+wb*b.z };
    return r;
}

/* Head bone WORLD orientation (version-independent, via Ogre exports).
 * VEH-guarded. Returns 1 + the quaternion on success. */
static int get_head_quat(void *pc, Quat *out)
{
    if (!g_skel_getbone || !g_oldnode_getdori || !pc) return 0;
    void *anim = readable((void *)((uintptr_t)pc + CHAR_ANIM), 8)
        ? *(void **)((uintptr_t)pc + CHAR_ANIM) : NULL;
    if (!readable(anim, ANIM_SKELETON + 8)) return 0;
    void *skel = *(void **)((uintptr_t)anim + ANIM_SKELETON);
    if (!readable(skel, 8)) return 0;
    if (setjmp(g_guard_jb)) { g_skel_getbone = NULL; logline("head-orient read FAULTED -- disabled"); return 0; }
    g_guard_armed = 1;
    void *bone = g_skel_getbone(skel, g_head_bone);   /* "Bip01 Head" std::string */
    const Quat *q = readable(bone, 8) ? g_oldnode_getdori(bone) : NULL;
    g_guard_armed = 0;
    if (!readable((void *)q, 16)) return 0;
    *out = *q;
    return 1;
}

/* --- Procedural aim-pitch through the spine/neck (PoC) --------------------
 * Additive: each frame read the bone's ANIMATED local orientation, post-
 * multiply a fractional pitch about a local axis, write it back, and flag the
 * derived transforms dirty so skinning + the head-welded camera pick it up.
 * No setManuallyControlled -> we layer on the live walk/run animation.
 * The local pitch axis + gain + sign are tuned in-game (Biped bones permuted).
 * VEH-guarded; pitch>0 = looking down. */
#define SPINE_BEND_GAIN (g_cfg_lean)  /* total lean as a fraction of look-pitch (config) */
#define SPINE_ROLL_UP   0.30f     /* roll gain, looking up   (pitch<0) */
#define SPINE_ROLL_DOWN 0.30f     /* roll gain, looking down (pitch>0) */
#define SPINE_ROLL_BIAS 0.0f      /* |pitch| roll, SAME direction both ways */

/* Rotate a vector by a quaternion (v' = q * v * conj(q)), efficient form. */
static Vec3 quat_rotvec(Quat q, Vec3 v)
{
    Vec3 u = { q.x, q.y, q.z };
    Vec3 t = { 2.0f*(u.y*v.z - u.z*v.y), 2.0f*(u.z*v.x - u.x*v.z), 2.0f*(u.x*v.y - u.y*v.x) };
    Vec3 r = { v.x + q.w*t.x + (u.y*t.z - u.z*t.y),
               v.y + q.w*t.y + (u.z*t.x - u.x*t.z),
               v.z + q.w*t.z + (u.x*t.y - u.y*t.x) };
    return r;
}

static void bend_spine(void *pc, float pitch)
{
    if (!g_spine_ready || !g_skel_getbone || !pc) return;
    void *anim = readable((void *)((uintptr_t)pc + CHAR_ANIM), 8)
        ? *(void **)((uintptr_t)pc + CHAR_ANIM) : NULL;
    if (!readable(anim, ANIM_SKELETON + 8)) return;
    void *skel = *(void **)((uintptr_t)anim + ANIM_SKELETON);
    if (!readable(skel, 8)) return;

    float total = pitch * SPINE_BEND_GAIN;    /* look down (pitch>0) -> bend forward */
    /* Bend only spine1+spine2; the neck and head animate naturally on top of the
     * lean. Manually controlling the neck warped the neck/head junction (our
     * neck rotation fighting whatever drives the head). */
    struct { unsigned char *nm; float w; const char *tag; } chain[] = {
        { g_bone_spine1, 0.5f, "spine1" },
        { g_bone_spine2, 0.5f, "spine2" },
    };
    const int NB = (int)(sizeof(chain) / sizeof(chain[0]));
    if (setjmp(g_guard_jb)) { g_spine_ready = 0; logline("spine-bend FAULTED -- disabled"); return; }
    g_guard_armed = 1;
    static int dbg;
    int logit = KFP_DEBUG_LOG && ((++dbg) % 120) == 0;

    void *bones[4];
    for (int i = 0; i < NB; i++) {
        bones[i] = g_skel_getbone(skel, chain[i].nm);
        if (!readable(bones[i], 8)) { g_guard_armed = 0; return; }
    }

    /* One-time: capture each bone's current (animated) local pose as the aim
     * reference, then mark the bones manually-controlled so the animation stops
     * resetting them each frame -- otherwise our writes are wiped before the
     * render (only our own head-position read saw them). */
    if (!g_spine_manual) {
        if (!g_oldbone_setmanual) { g_guard_armed = 0; return; }
        for (int i = 0; i < NB; i++) {
            const Quat *lq = g_oldnode_getori(bones[i]);
            g_spine_rest[i] = readable((void *)lq, 16) ? *lq : (Quat){ 1, 0, 0, 0 };
            g_oldbone_setmanual(bones[i], 1);
        }
        g_spine_manual = 1;
        logline("[spine] bones set manuallyControlled; rest poses captured");
    }

    /* WORLD pitch axis = character's LATERAL (left-right) axis. Deriving it from
     * g_yaw alone isn't rotation-agnostic: the body's TRUE facing (set by the
     * game when you turn in place) doesn't track g_yaw through a 180, so the bend
     * inverted when facing behind. Instead: calibrate the lateral direction once
     * in the root spine's LOCAL frame (from g_yaw, valid while idle-facing), then
     * each frame rotate it by the root's CURRENT world orientation -- so it
     * follows the real body facing through any turn. Root spine isn't one of our
     * controlled bones, so its derived reflects the animation's true facing. */
    Vec3 world_axis, fwd_w;
    void *rootb = g_skel_getbone(skel, g_bone_rootspine);
    const Quat *rdq = readable(rootb, 8) ? g_oldnode_getdori(rootb) : NULL;
    if (readable((void *)rdq, 16) && g_have_fwd) {
        /* g_fwd_local is calibrated by calibrate_spine_fwd() while the weapon is
         * SHEATHED and the body idle/level -- the only time "body faces g_yaw"
         * actually holds. NEVER calibrate here: with a weapon drawn the combat
         * stance blades the body sideways, and a skewed axis turns pitch into
         * roll (the session-dependent weapon-roll ghost we chased with gains). */
        Vec3 fw = quat_rotvec(*rdq, g_fwd_local);          /* body forward -> world, tracks facing */
        float hl = sqrtf(fw.x*fw.x + fw.z*fw.z);
        if (hl < 1e-4f) hl = 1e-4f;
        fw.x /= hl; fw.z /= hl;                             /* flatten to horizontal, normalize */
        fwd_w = (Vec3){ fw.x, 0.0f, fw.z };                /* forward axis for roll correction */
        /* lateral = up x forward: guaranteed perpendicular to forward AND
         * horizontal -> pitch about it is pure, no roll (any calibration error
         * becomes an imperceptible yaw offset instead of visible weapon roll). */
        world_axis = (Vec3){ fw.z, 0.0f, -fw.x };
    } else {
        world_axis = (Vec3){ cosf(g_yaw), 0.0f, -sinf(g_yaw) };  /* fallback */
        fwd_w      = (Vec3){ sinf(g_yaw), 0.0f, cosf(g_yaw) };
    }

    /* Rebuild each bone's REST-pose world orientation from the root's live derived
     * and the captured rest locals (NOT the bent derived), and project the axis
     * through that. Reading the bent derived fed the bend back into the axis and
     * leaked pitch into roll; the rest chain is feedback-free -> pure pitch. */
    Quat rest_der = readable((void *)rdq, 16) ? *rdq : (Quat){ 1, 0, 0, 0 };  /* root, live facing */
    for (int i = 0; i < NB; i++) {
        rest_der = quat_norm(quat_mul(rest_der, g_spine_rest[i])); /* bone i rest world orient */
        Vec3 Al = quat_rotvec(quat_conj(rest_der), world_axis);   /* world axis -> rest frame */
        float a = total * chain[i].w;
        float c = cosf(a * 0.5f), s = sinf(a * 0.5f);
        Quat dpitch = { c, Al.x*s, Al.y*s, Al.z*s };
        /* Roll correction about the forward axis, proportional to this bone's
         * pitch, to cancel the CW/CCW weapon twist the arm rig adds. */
        Vec3 Fl = quat_rotvec(quat_conj(rest_der), fwd_w);
        float rollg = (pitch < 0.0f ? SPINE_ROLL_UP : SPINE_ROLL_DOWN);
        float ar = rollg * a + SPINE_ROLL_BIAS * fabsf(a);   /* linear (flips) + bias (same both ways) */
        float cr = cosf(ar * 0.5f), sr = sinf(ar * 0.5f);
        Quat droll = { cr, Fl.x*sr, Fl.y*sr, Fl.z*sr };
        Quat d  = quat_norm(quat_mul(dpitch, droll));
        Quat nq = quat_norm(quat_mul(g_spine_rest[i], d)); /* rest * (pitch + roll correct) */
        g_oldnode_setori(bones[i], &nq);
        g_oldnode_needupd(bones[i], 1);
        if (logit)
            logline("[spine] %s a=%.2f rollg=%.2f Al=(%.2f,%.2f,%.2f) set=(%.3f,%.3f,%.3f,%.3f)",
                    chain[i].tag, a, rollg, Al.x, Al.y, Al.z, nq.w, nq.x, nq.y, nq.z);
    }
    g_guard_armed = 0;
}

/* Calibrate the body-forward axis in root-spine local frame. Call ONLY when
 * "body faces g_yaw" holds: weapon sheathed, idle, looking level (orient-to-
 * control has squared the body to the camera). The result is a skeleton
 * constant, so it persists across draw/sheathe and only refreshes when the
 * conditions are met again. */
static void calibrate_spine_fwd(void *pc)
{
    if (!g_spine_ready || !g_skel_getbone || !pc) return;
    void *anim = readable((void *)((uintptr_t)pc + CHAR_ANIM), 8)
        ? *(void **)((uintptr_t)pc + CHAR_ANIM) : NULL;
    if (!readable(anim, ANIM_SKELETON + 8)) return;
    void *skel = *(void **)((uintptr_t)anim + ANIM_SKELETON);
    if (!readable(skel, 8)) return;
    if (setjmp(g_guard_jb)) { g_guard_armed = 0; return; }
    g_guard_armed = 1;
    void *rootb = g_skel_getbone(skel, g_bone_rootspine);
    const Quat *rdq = readable(rootb, 8) ? g_oldnode_getdori(rootb) : NULL;
    if (readable((void *)rdq, 16)) {
        g_fwd_local = quat_rotvec(quat_conj(*rdq),
                                  (Vec3){ sinf(g_yaw), 0.0f, cosf(g_yaw) });
        if (!g_have_fwd) logline("[spine] fwd axis calibrated (sheathed+idle+level)");
        g_have_fwd = 1;
    }
    g_guard_armed = 0;
}

/* Hand the spine/neck back to the animation. Called when we leave active FP
 * (free cam, FP off) -- otherwise the frozen manual pose flails as the body
 * turns. Re-entering FP re-captures rest poses via bend_spine; the calibrated
 * forward axis persists (it's a skeleton constant, not per-stance state). */
static void release_spine(void *pc)
{
    if (!g_spine_manual) return;
    g_spine_manual = 0;
    if (!g_oldbone_setmanual || !g_skel_getbone || !pc) return;
    void *anim = readable((void *)((uintptr_t)pc + CHAR_ANIM), 8)
        ? *(void **)((uintptr_t)pc + CHAR_ANIM) : NULL;
    if (!readable(anim, ANIM_SKELETON + 8)) return;
    void *skel = *(void **)((uintptr_t)anim + ANIM_SKELETON);
    if (!readable(skel, 8)) return;
    unsigned char *names[3] = { g_bone_spine1, g_bone_spine2, g_bone_neck };
    if (setjmp(g_guard_jb)) { g_guard_armed = 0; return; }
    g_guard_armed = 1;
    for (int i = 0; i < 3; i++) {
        void *b = g_skel_getbone(skel, names[i]);
        if (readable(b, 8)) g_oldbone_setmanual(b, 0);
    }
    g_guard_armed = 0;
    logline("[spine] manual control released");
}

/* Find the character's body Ogre::Entity on the AnimationClass by PROBING its
 * members: the right pointer is the one whose Entity::getSkeleton() equals the
 * skeleton we already know (anim+0xB8). Self-validating -- no vtable-slot or
 * offset guessing. Probed once, then cached. Each candidate call VEH-guarded.
 * NOTE: clobbers g_guard_jb; call OUTSIDE any other guarded region. */
static void *find_body_entity(void *animc)
{
    if (!g_entity_getskel || !readable(animc, ANIM_SKELETON + 8)) return NULL;
    void *skel = *(void **)((uintptr_t)animc + ANIM_SKELETON);
    if (!skel) return NULL;
    if (g_anim_ent_off == -2) return NULL;   /* probe already failed for good */
    if (g_anim_ent_off >= 0)
        return readable((void *)((uintptr_t)animc + g_anim_ent_off), 8)
            ? *(void **)((uintptr_t)animc + g_anim_ent_off) : NULL;
    for (int off = 0; off < 0x300; off += 8) {
        if (!readable((void *)((uintptr_t)animc + off), 8)) break;
        void *p = *(void **)((uintptr_t)animc + off);
        if (!readable(p, 0x100)) continue;
        void *sk;
        if (setjmp(g_guard_jb)) { g_guard_armed = 0; continue; }
        g_guard_armed = 1;
        sk = g_entity_getskel(p);
        g_guard_armed = 0;
        if (sk == skel) {
            g_anim_ent_off = off;
            logline("[aim] body Entity found at anim+0x%x -> %p", off, p);
            return p;
        }
    }
    logline("[aim] body Entity probe FAILED (getskel=%p)", (void *)g_entity_getskel);
    g_anim_ent_off = -2;   /* don't re-probe every frame */
    return NULL;
}

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

static DWORD WINAPI di_poll_thread(void *unused);   /* defined below */
static int g_di_thread_on;

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
        if (!g_di_thread_on) {
            g_di_thread_on = 1;
            CloseHandle(CreateThread(NULL, 0, di_poll_thread, NULL, 0, NULL));
            logline("mouse poll thread started (~1kHz, framerate-independent look)");
        }
    }
}

/* Relative deltas since the previous call (mouse axes default to relative).
 * Called ONLY from the poll thread once it starts (single GetDeviceState
 * consumer -- concurrent calls would split deltas unpredictably). */
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

/* Frame-rate-independent look capture: a dedicated ~1kHz thread owns the
 * DirectInput reads and accumulates counts; each frame consumes the total.
 * Per-frame GetDeviceState sampled the mouse at the FRAME rate, and at high
 * fps that aliases against the mouse's own report rate (beat patterns of
 * 0/2-count frames = visible wobble; felt fine only at the frame rates we
 * developed at). Constant-rate capture decouples feel from fps entirely. */
static volatile LONG g_acc_dx, g_acc_dy;
static DWORD WINAPI di_poll_thread(void *unused)
{
    (void)unused;
    timeBeginPeriod(1);              /* 1ms Sleep granularity for this loop */
    for (;;) {
        LONG dx, dy;
        if (di_get_deltas(&dx, &dy)) {
            if (dx) InterlockedAdd(&g_acc_dx, dx);
            if (dy) InterlockedAdd(&g_acc_dy, dy);
        }
        Sleep(1);
    }
    return 0;
}
/* Consume (and zero) the accumulated deltas. */
static void di_take_acc(LONG *dx, LONG *dy)
{
    *dx = InterlockedExchange(&g_acc_dx, 0);
    *dy = InterlockedExchange(&g_acc_dy, 0);
}

/* Hand the camera back to the vanilla system CONTINUOUSLY: seat the center
 * node on our current FP look direction and the camera node at (0,0,zoom),
 * via the game's own manuallySetOrientationAndZoom -- so the RTS camera (or
 * the map fly-out) resumes facing exactly where the player was looking,
 * with a legit zoom state (no endless zoom-out, no view cut). */
static void vanilla_cam_handoff(void *cam, void *camnode, float zoom)
{
    float qy = cosf(g_yaw * 0.5f),   sqy = sinf(g_yaw * 0.5f);
    float qp = cosf(g_pitch * 0.5f), sqp = sinf(g_pitch * 0.5f);
    Quat q = { qy * qp, qy * sqp, sqy * qp, -sqy * sqp };
    /* vanilla keeps the camera node's LOCAL orientation at identity (only the
     * center rotates); our per-frame derived-ori writes dirtied it, and
     * manuallySetOrientationAndZoom doesn't reset it -- do that here. */
    Quat ident = { 1.0f, 0.0f, 0.0f, 0.0f };
    if (readable(camnode, 8)) g_node_set_ori(camnode, &ident);
    if (setjmp(g_guard_jb)) { g_guard_armed = 0; return; }
    g_guard_armed = 1;
    ((void (*)(void *, const Quat *, float))(g_base + RVA_CAM_MANUAL_SETOZ))(cam, &q, zoom);
    g_guard_armed = 0;
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

    {   /* [map-bug diag] log every state flip that can knock us out of FP */
        static int pa = -1, pf = -1, po = -1, pm = -1;
        int fc = *(unsigned char *)((uintptr_t)cam + CC_FREECAM);
        int ov = (g_ui_mask & UIMASK_OVERVIEW) ? 1 : 0;
        if (KFP_DEBUG_LOG && (active != pa || fc != pf || ov != po || g_fp_mode != pm)) {
            logline("[cam] fp=%d freecam=%d overview=%d active=%d mask=0x%03x",
                    g_fp_mode, fc, ov, active, g_ui_mask);
            pa = active; pf = fc; po = ov; pm = g_fp_mode;
        }
    }

    /* Drive the aim-lean whenever FP is ON -- including free cam, so it stays
     * visible for third-person inspection while you orbit (the rotation-agnostic
     * axis keeps it body-relative). Release when FP is off or while ragdolled,
     * handing the spine back to the animation. Runs before the eye block below
     * so the FP eye still rides the bend. */
    {
        void *pcx = first_player_char(gw);
        /* Character swap (selected a different squad member): release the old
         * character's driven bones and reset all per-character calibrations so
         * FP re-seats cleanly on the new body. */
        static void *pc_prev;
        if (pcx != pc_prev) {
            if (pc_prev && g_spine_manual) release_spine(pc_prev);
            g_have_qref = 0; g_down_blend = 0.0f; g_is_down = 0;  /* ragdoll ref */
            g_have_fwd = 0;                                       /* spine fwd axis */
            g_have_last_feet = 0; g_move_speed = 0.0f;            /* speed-push tracker */
            g_have_prevraw = 0;                                   /* rebase-detect tracker */
            /* NOTE: g_have_t/g_tx/g_tz are NOT reset -- T is the floating-origin
             * translation (ogre vs game coords), scene-global and identical for
             * every character. Resetting it on swap opened an uncalibrated
             * window where the center-snap fallback fed back on itself and the
             * camera glided away while holding WASD (even paused). */
            pc_prev = pcx;
        }
        g_player_pc = pcx;              /* cached for the ranged free-aim hook */
        /* Only lean when a weapon is drawn (in hands) -- the aim-lean is for
         * aiming, and looked odd during normal unarmed/sheathed movement. */
        void *wih = (pcx && readable((void *)((uintptr_t)pcx + CHAR_WEAPON_IN_HANDS), 8))
            ? *(void **)((uintptr_t)pcx + CHAR_WEAPON_IN_HANDS) : NULL;
        int weapon_drawn = (wih != NULL);
        static int prev_drawn = -1;
        if (KFP_DEBUG_LOG && weapon_drawn != prev_drawn) {
            logline("[spine] weaponInHands=%p drawn=%d", wih, weapon_drawn);
            prev_drawn = weapon_drawn;
        }
        if (g_fp_mode && !g_is_down && weapon_drawn && g_cfg_aim_lean) bend_spine(pcx, g_pitch);
        else if (g_spine_manual)                     release_spine(pcx);
        /* Calibrate the forward axis only when the body reliably faces the
         * camera: sheathed (no bladed combat stance), idle, looking level. */
        if (g_fp_mode && !g_is_down && !weapon_drawn && !g_was_moving
            && g_pitch > -0.10f && g_pitch < 0.10f)
            calibrate_spine_fwd(pcx);

        if (KFP_MANUAL_AIM)
            manual_fire_update(pcx);    /* LMB -> GunClass::shoot at crosshair */

        /* R toggles manual aim: raise the ranged weapon with no target needed.
         * While on, force the ranged state machine into aiming every frame
         * (combat updates keep trying to end it without a target); this also
         * lights up the pose/facing overrides, so the weapon tracks the
         * crosshair. Cleared on sheathe / FP off. */
        if (KFP_MANUAL_AIM) {
            static int r_prev;
            static int draw_grace;              /* frames to wait for the draw anim */
            int r = (GetAsyncKeyState(VK_AIM_TOGGLE) & 0x8000) != 0;
            if (r && !r_prev && !weapon_drawn && !g_ui_open && pcx) {
                /* Out of combat the weapon is holstered (weaponInHands NULL) and
                 * the game only draws it in combat -- so R draws it ourselves:
                 * vtable+0x3C8 getThePreferredWeapon, vtable+0x3D8 drawWeapon
                 * (Item*, std::string by-value = ptr to 32B SSO temp). */
                typedef void *(*get_pref_t)(void *pc);
                typedef char (*draw_weap_t)(void *pc, void *item, void *sso_str);
                if (setjmp(g_guard_jb)) { g_guard_armed = 0; logline("[aim] drawWeapon FAULTED"); }
                else {
                    g_guard_armed = 1;
                    void **vt = readable(pcx, 8) ? *(void ***)pcx : NULL;
                    if (readable((void *)((uintptr_t)vt + 0x3D8), 8)) {
                        void *w = ((get_pref_t)vt[0x3C8 / 8])(pcx);
                        if (w) {
                            unsigned char empty[32]; make_mstr(empty, "");
                            ((draw_weap_t)vt[0x3D8 / 8])(pcx, w, empty);
                            g_aim_mode = 1; draw_grace = 90;
                            logline("[aim] drew weapon %p; aim RAISED", w);
                        } else logline("[aim] no preferred weapon to draw");
                    }
                    g_guard_armed = 0;
                }
            } else if (r && !r_prev && weapon_drawn && !g_ui_open) {
                g_aim_mode = !g_aim_mode;
                void *rcd = readable((void *)((uintptr_t)pcx + CHAR_RANGEDCOMBAT), 8)
                    ? *(void **)((uintptr_t)pcx + CHAR_RANGEDCOMBAT) : NULL;
                void *gund = (rcd && readable((void *)((uintptr_t)rcd + RC_GUN), 8))
                    ? *(void **)((uintptr_t)rcd + RC_GUN) : NULL;
                int st = (rcd && readable(rcd, 4)) ? *(int *)rcd : -1;
                int cm = (rcd && readable((void *)((uintptr_t)rcd + RC_COMBATMODE), 1))
                    ? *(unsigned char *)((uintptr_t)rcd + RC_COMBATMODE) : -1;
                int am = (gund && readable((void *)((uintptr_t)gund + GUN_AMMO), 4))
                    ? *(int *)((uintptr_t)gund + GUN_AMMO) : -1;
                int physflag = (gund && readable((void *)((uintptr_t)gund + 0x60), 2))
                    ? *(unsigned char *)((uintptr_t)gund + 0x60)
                      | (*(unsigned char *)((uintptr_t)gund + 0x61) << 8) : -1;
                logline("[aim] manual aim %s | rc=%p gun=%p state=%d combatMode=%d ammo=%d phys/vis=0x%x gpsn=%p",
                        g_aim_mode ? "RAISED" : "lowered", rcd, gund, st, cm, am,
                        physflag, (void *)g_get_parent_scenenode);
            }
            r_prev = r;
            if (draw_grace > 0) draw_grace--;   /* the draw anim needs frames */
            else if (!weapon_drawn) g_aim_mode = 0;
            if (!g_fp_mode) g_aim_mode = 0;
            static int aim_prev;
            if (aim_prev && !g_aim_mode && pcx) {   /* lower: hide gun + stop aim anim */
                void *rc2 = readable((void *)((uintptr_t)pcx + CHAR_RANGEDCOMBAT), 8)
                    ? *(void **)((uintptr_t)pcx + CHAR_RANGEDCOMBAT) : NULL;
                void *g2 = (rc2 && readable((void *)((uintptr_t)rc2 + RC_GUN), 8))
                    ? *(void **)((uintptr_t)rc2 + RC_GUN) : NULL;
                if (setjmp(g_guard_jb)) { g_guard_armed = 0; }
                else {
                    g_guard_armed = 1;
                    if (g2 && readable(g2, 0x10)) {
                        void **gvt = *(void ***)g2;
                        if (readable((void *)((uintptr_t)gvt + 0x10), 8))
                            ((void (*)(void *, char))gvt[1])(g2, 0);
                    }
                    /* animationUpdate's target==NULL branch STOPS the aim anim --
                     * the exact cleanup vanilla runs on combat end. Without it the
                     * driven anim keeps its weight and the pose sticks (hug). */
                    if (rc2 && g_ranged_animupd_orig) {
                        Vec3 z = { 0, 0, 0 };
                        g_ranged_animupd_orig(rc2, 0.016f, &z, NULL);
                        logline("[aim] aim anim stopped (lowered)");
                    }
                    g_guard_armed = 0;
                }
                if (readable((void *)((uintptr_t)rc2 + RC_COMBATMODE), 1))
                    *(unsigned char *)((uintptr_t)rc2 + RC_COMBATMODE) = 0;  /* let AI end combat */
            }
            aim_prev = g_aim_mode;
            if (g_aim_mode && pcx) {
                void *rc = readable((void *)((uintptr_t)pcx + CHAR_RANGEDCOMBAT), 8)
                    ? *(void **)((uintptr_t)pcx + CHAR_RANGEDCOMBAT) : NULL;
                void *gun = (rc && readable((void *)((uintptr_t)rc + RC_GUN), 8))
                    ? *(void **)((uintptr_t)rc + RC_GUN) : NULL;
                if (readable((void *)((uintptr_t)rc + RC_COMBATMODE), 1)) {
                    *(unsigned char *)((uintptr_t)rc + RC_COMBATMODE) = 1;
                    if (readable((void *)((uintptr_t)rc + RC_STATE_OFF), 4))
                        *(int *)((uintptr_t)rc + RC_STATE_OFF) = 3;   /* WAITING = held aim */
                }
                /* The AI task layer is the ONLY caller of updateT->animationUpdate
                 * (verified: 0x438c70 is the sole caller, invoked from AI task
                 * objects), so with no combat task we drive the pose ourselves.
                 * Also reload directly when empty (instant for now). */
                /* Probe/fetch the body entity BEFORE the guarded region below
                 * (the probe uses g_guard_jb itself). */
                void *animx = readable((void *)((uintptr_t)pcx + CHAR_ANIM), 8)
                    ? *(void **)((uintptr_t)pcx + CHAR_ANIM) : NULL;
                void *body_ent = animx ? find_body_entity(animx) : NULL;
                Vec3 aim;
                if (rc && g_ranged_animupd_orig && fp_aim_point(&aim)) {
                    if (setjmp(g_guard_jb)) { g_guard_armed = 0; g_aim_mode = 0;
                        logline("[aim] pose drive FAULTED -- aim mode off"); }
                    else {
                        g_guard_armed = 1;
                        /* Out of combat rc->gun is NULL (setup nulls the cache;
                         * only combat repopulates it) -- fetch it the game's way
                         * and prime the cache so animationUpdate can use it. */
                        if (!gun) {
                            gun = ((void *(*)(void *))(g_base + RVA_RC_GETGUN))(rc);
                            if (gun && readable((void *)((uintptr_t)rc + RC_GUN), 8)) {
                                *(void **)((uintptr_t)rc + RC_GUN) = gun;
                                logline("[aim] gun fetched via getGun: %p", gun);
                            }
                        }
                        if (gun && readable((void *)((uintptr_t)gun + GUN_AMMO), 4)
                            && *(int *)((uintptr_t)gun + GUN_AMMO) <= 0) {
                            typedef void (*gun_reload_t)(void *gun);
                            ((gun_reload_t)(g_base + RVA_GUN_RELOAD))(gun);
                            logline("[aim] reloaded");
                        }
                        /* The wielded crossbow visual is the GUN's own mesh (the
                         * item stays on the back). GunClassPersonal shares the
                         * createPhysical(parentNode, material) impl the turret
                         * wrapper showed us: parent = body entity's scene node.
                         * Body entity = anim->vt[+0x20]() (the same getter draw/
                         * sheathe use); parent via Ogre getParentSceneNode. */
                        if (gun && g_get_parent_scenenode
                            && readable((void *)((uintptr_t)gun + 0x60), 1)
                            && !*(unsigned char *)((uintptr_t)gun + 0x60)) {
                            void *parent = body_ent ? g_get_parent_scenenode(body_ent) : NULL;
                            if (parent && readable(parent, 0x40) && readable(gun, 0x20)) {
                                void **gvt0 = *(void ***)gun;
                                unsigned char mat[32]; make_mstr(mat, "");
                                logline("[aim] createPhysical attempt: gun=%p parent=%p", gun, parent);
                                ((char (*)(void *, void *, void *))gvt0[3])(gun, parent, mat);
                                logline("[aim] createPhysical -> flag=%d",
                                        (int)*(unsigned char *)((uintptr_t)gun + 0x60));
                            } else {
                                logline("[aim] createPhysical skipped: ent=%p parent=%p", body_ent, parent);
                            }
                        }
                        if (gun && readable(gun, 0x118)) {
                            void **gvt = *(void ***)gun;
                            typedef void (*gun_setvis_t)(void *g, char on);
                            typedef void (*gun_update_t)(void *g, const Vec3 *fallback);
                            /* setVisible's SHOW branch is gated on +0x110 == 0
                             * (decomp-verified); clear it while manually aiming. */
                            if (*(unsigned char *)((uintptr_t)gun + 0x110)) {
                                *(unsigned char *)((uintptr_t)gun + 0x110) = 0;
                                logline("[aim] cleared gun+0x110 hide flag");
                            }
                            static int visdbg;
                            if ((++visdbg % 300) == 1)
                                logline("[aim] gun meshes: drawn=%p undrawn=%p bolts=%d",
                                        *(void **)((uintptr_t)gun + 0x100),
                                        *(void **)((uintptr_t)gun + 0x108),
                                        *(int *)((uintptr_t)gun + 0xA8));
                            if (readable((void *)((uintptr_t)gvt + 0x10), 8)) {
                                ((gun_setvis_t)gvt[1])(gun, 1);      /* vt+0x08 setVisible */
                                ((gun_update_t)gvt[2])(gun, &aim);   /* vt+0x10 update */
                            }
                        }
                        /* target must be NON-NULL: the null branch STOPS the aim
                         * anim (arms drop); non-null plays the aim-hold anim,
                         * drives lookatPosition(aimpos) and readyToShoot. It is
                         * only null-checked here, never dereferenced -- pass
                         * the player object as a safe stand-in. */
                        g_ranged_animupd_orig(rc, 0.016f, &aim, pcx);
                        g_guard_armed = 0;
                    }
                }
            }
        }

        /* Ranged lock-on breaker: with an explicit right-click target the
         * facing is driven by a path that bypasses faceDirection, so while in
         * FP ranged combat force the facing member (CharMovement+0xD0,
         * verified via getFacingDirection) to the look direction each frame. */
        if (g_fp_mode && g_cfg_freeaim && pcx) {
            void *rc = readable((void *)((uintptr_t)pcx + CHAR_RANGEDCOMBAT), 8)
                ? *(void **)((uintptr_t)pcx + CHAR_RANGEDCOMBAT) : NULL;
            if (readable((void *)((uintptr_t)rc + RC_COMBATMODE), 1)
                && *(unsigned char *)((uintptr_t)rc + RC_COMBATMODE)
                && readable((void *)((uintptr_t)pcx + CHAR_MOVEMENT), 8)) {
                void *mv = *(void **)((uintptr_t)pcx + CHAR_MOVEMENT);
                if (readable((void *)((uintptr_t)mv + 0xD0), 12)) {
                    Vec3 *fdir = (Vec3 *)((uintptr_t)mv + 0xD0);
                    fdir->x = sinf(g_yaw); fdir->y = 0.0f; fdir->z = cosf(g_yaw);
                }
            }
        }
    }

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
        int ui_open = (control == 0) || (panels_now && panel_frames >= 8) || g_settings_open;
        g_ui_open = ui_open;                    /* read by the setPointer hook */
        /* Movement gate is NARROWER than the cursor gate: a mere side panel
         * (inventory/squad/jobs) frees the cursor + freezes look, but WASD should
         * still walk the character (vanilla lets you pan the camera then too).
         * Only a genuine control-disabled state (dialogue/cutscene) halts WASD. */
        g_ui_moveblock = (control == 0);
        if (!g_ovr_prev) {                                            /* FP enter */
            mygui_cursor(0); g_pointer_default = 1;
            /* Save the vanilla zoom: the camera node's LOCAL position is
             * (0,0,zoom) under the rotating center node (verified in
             * manuallySetOrientationAndZoom). Our FP writes trash the local
             * position, and vanilla never resets it -- restoring this exact
             * value on exit is what prevents the unending zoom-out. */
            if (g_node_get_pos) {
                const Vec3 *lp = g_node_get_pos(node);
                if (readable((void *)lp, 12) && lp->z > 2.0f && lp->z < 2000.0f)
                    g_vanilla_zoom = lp->z;
            }
        }
        /* NB: all MyGUI widget work (crosshair/vignette/blackout create,
         * setVisible, setImageTexture) happens POST-FRAME in fp_gui_update --
         * NEVER here. This hook runs mid-frame (during the game's UI update),
         * and touching widgets then corrupted MyGUI's lists and crashed the
         * order/job/inventory ItemBoxes. */

        if (ui_open) {
            if (g_cursor_hidden) { while (ShowCursor(TRUE) < 0) { } g_cursor_hidden = 0; }
            mygui_cursor(1);                    /* dialogue: keep the game cursor visible */
            /* cursor free; leave look angle frozen */
        } else {
            if (!g_cursor_hidden) { while (ShowCursor(FALSE) >= 0) { } g_cursor_hidden = 1; }
            ensure_dinput();
            if (!g_ovr_prev) {
                /* FP enter (incl. returning from free cam). Don't reset while
                 * ragdolled -- keep the frozen look and head reference so the
                 * follow resumes. g_qref self-recalibrates every upright frame,
                 * so no explicit clear is needed. */
                if (!g_is_down) { g_yaw = 0.0f; g_pitch = 0.0f; }
                g_have_prevraw = 0;   /* don't bridge a rebase across the FP gap */
                LONG jx, jy; di_take_acc(&jx, &jy);     /* drain pre-FP motion */
            } else if (!g_ui_prev) {            /* skip the delta on the frame a dialogue closes */
                /* DirectInput relative deltas (see ensure_dinput comment);
                 * cursor-warp deltas only as fallback. */
                LONG rdx, rdy;
                if (g_di_ready) {
                    di_take_acc(&rdx, &rdy);   /* poll thread accumulates at ~1kHz */
                } else {
                    rdx = cur.x - cx; rdy = cur.y - cy;   /* warp fallback */
                }
                /* Freeze look input while ragdolled: the camera follows the head,
                 * and world-axis yaw/pitch would map to the wrong axes on the
                 * rolled view (mouse-up -> roll etc.). Still drain the deltas so
                 * they don't accumulate and jump the view on get-up. */
                if (!g_is_down) {
                    g_yaw   -= (float)rdx * LOOK_SENS;   /* mouse right -> look right */
                    g_pitch += (float)rdy * LOOK_SENS;   /* mouse down  -> look down  */
                    if (g_pitch >  1.4f) g_pitch =  1.4f;
                    if (g_pitch < -1.4f) g_pitch = -1.4f;
                }
            } else {
                LONG jx, jy; di_take_acc(&jx, &jy);     /* dialogue frame: discard */
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
            if (g_have_eye) {
                /* Hand off ONCE as the map takes over, facing our FP look
                 * direction at minimal zoom -- continuous view (no cut out of
                 * first person) AND a legit zoom state (no endless zoom-out). */
                vanilla_cam_handoff(cam, node, 2.0f);
            }
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
        if (readable(center, 8)
            && (g_node_getdpos_upd ? g_node_getdpos_upd(center, &centerW)   /* FORCED fresh */
                                   : g_node_get_dpos(center, &centerW))) {
            /* The `center` node bobs/smooths with the vanilla camera, so it is a
             * bad vertical anchor. Take X/Z from it (Ogre space; X/Z are rebased
             * by the floating origin so we must stay in Ogre space + add the head
             * lean), but take Y straight from the head bone (Y is NOT rebased and
             * is stable) -> the eye is welded to head height, no bob. */
            Vec3 eyeW = { centerW.x, centerW.y + EYE_HEIGHT - EYE_DROP, centerW.z };  /* fallback */
            g_eye_from_head = 0;
            if (g_get_bone_world) {
                void *pc = first_player_char(gw);
                Vec3 feet, head;
                if (pc && char_position(pc, &feet)) {
                    /* Horizontal ground speed from feet delta / dt (framerate-
                     * independent). Drives the speed-scaled forward push below so
                     * the eye leads faster movement instead of getting left behind. */
                    {
                        float dt = (g_frame_dt > 0.0f && g_frame_dt < 0.25f) ? g_frame_dt : 0.016f;
                        if (g_have_last_feet) {
                            float dfx = feet.x - g_last_feet_x, dfz = feet.z - g_last_feet_z;
                            float inst = sqrtf(dfx*dfx + dfz*dfz) / dt;
                            if (inst > 400.0f) inst = 400.0f;   /* reject teleport/paging jumps */
                            g_move_speed += (inst - g_move_speed) * 0.20f;   /* ~0.15s low-pass */
                        }
                        g_last_feet_x = feet.x; g_last_feet_z = feet.z; g_have_last_feet = 1;
                    }
                    g_get_bone_world(pc, &head, g_head_bone);
                    Vec3 h = { head.x - feet.x, head.y - feet.y, head.z - feet.z };
                    g_head_above = h.y;   /* head height over feet; small => actually prone */
                    if (h.x*h.x + h.y*h.y + h.z*h.z > 0.25f) {
                        eyeW.y = head.y - EYE_DROP;   /* head-bone Y directly (Y not rebased) */
                        /* Floating-origin rebase guard: loading a new map chunk
                         * makes Ogre recenter the scene -- centerW (Ogre coords)
                         * jumps by the rebase delta in a single frame while feet
                         * (game coords) stay continuous, so (centerW-feet) jumps.
                         * During movement g_tx is frozen, so without this the eye
                         * would stay in the OLD Ogre frame and the camera teleports
                         * away. A one-frame jump this large is a rebase (camera lag
                         * drifts only a few units/frame), so shift the frozen T by
                         * it and the weld survives the chunk load seamlessly. */
                        float rawtx = centerW.x - feet.x, rawtz = centerW.z - feet.z;
                        if (g_have_prevraw && g_have_t) {
                            float ddx = rawtx - g_prevraw_tx, ddz = rawtz - g_prevraw_tz;
                            if (ddx*ddx + ddz*ddz > 2500.0f) {   /* >50u/frame = rebase */
                                g_tx += ddx; g_tz += ddz;
                            }
                        }
                        g_prevraw_tx = rawtx; g_prevraw_tz = rawtz; g_have_prevraw = 1;
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
                        g_eye_from_head = 1;
                    }
                    /* [weld diag] temporary: 6 CONSECUTIVE frames every ~5s --
                     * shows whether the head bone animates per frame (bob/lean)
                     * or goes quasi-static under MOVE_DIRECTION. */
                    static int wd;
                    if (KFP_DEBUG_LOG && ((++wd) % 15) == 0) {
                        int kw = (GetAsyncKeyState(VK_W) & 0x8000) != 0;
                        int ka = (GetAsyncKeyState(VK_A) & 0x8000) != 0;
                        int ks = (GetAsyncKeyState(VK_S) & 0x8000) != 0;
                        int kd = (GetAsyncKeyState(VK_D) & 0x8000) != 0;
                        logline("[weld2] k=%d%d%d%d move=%d dir=%d eyeY=%.2f headY=%.2f feetY=%.2f centerY=%.2f tx=%.1f",
                                kw, ka, ks, kd, g_was_moving, g_was_direct,
                                eyeW.y, head.y, feet.y, centerW.y, g_tx);
                    }
                }
            }
            /* [weld diag] why did the head branch fail while moving? */
            if (KFP_DEBUG_LOG && !g_eye_from_head && g_was_moving) {
                static int fl;
                if ((++fl % 30) == 1) {
                    void *pcd = first_player_char(gw);
                    Vec3 fd = {0,0,0}; int cpok = pcd && char_position(pcd, &fd);
                    logline("[weld] FALLBACK while moving: pc=%p charpos=%d gbw=%p feet=(%.1f,%.1f,%.1f)",
                            pcd, cpok, (void *)g_get_bone_world, fd.x, fd.y, fd.z);
                }
            }

            /* Push the eye forward (horizontal look dir) so it sits at the face,
             * not inside the head mesh. */
            /* Speed-scaled forward lead so the camera doesn't get left behind
             * the head. g_move_speed is the head's ON-SCREEN speed (feet delta /
             * real dt), so it already grows with both faster movement AND higher
             * game speed (2x/5x move the head 2x/5x further per real second).
             *
             * But at high game speed the weld lags by more world distance, and
             * we want the lead to grow proportionally. So: divide out the game-
             * speed multiplier to recover the pure gait (walk..run, 0..1), then
             * multiply the lead back by game speed. Standing still -> 0 (no lag,
             * no push); running at 5x -> ~5x the 1x-run lead. */
            float gs = readable((void *)((uintptr_t)gw + GW_FRAMESPEED), 4)
                     ? *(float *)((uintptr_t)gw + GW_FRAMESPEED) : 1.0f;
            if (!(gs >= 1.0f)) gs = 1.0f;          /* paused/garbage -> treat as 1x */
            if (gs > 6.0f) gs = 6.0f;              /* clamp (max button is 5x) */
            /* gait = game-speed-independent movement intensity (0 idle .. 1 run) */
            float gait = g_cfg_move_ref > 1.0f ? (g_move_speed / gs) / g_cfg_move_ref : 0.0f;
            if (gait < 0.0f) gait = 0.0f; else if (gait > 1.25f) gait = 1.25f;
            static float mfwd;
            mfwd += (FP_MOVE_FORWARD * gait * gs - mfwd) * 0.15f;
            float fpush = FP_EYE_FORWARD + mfwd;
            eyeW.x += sinf(g_yaw) * fpush;
            eyeW.z += cosf(g_yaw) * fpush;

            /* Eye follows the head bone RAW (no horizontal smoothing) so the
             * camera stays welded through the run animation's forward lean --
             * a low-pass here made the camera lag the leaning head, most
             * visible at high game speed. Input smoothness comes from
             * DirectInput now, not from damping the eye path. */

            g_dbg_center_y = centerW.y; g_dbg_eye_y = eyeW.y;   /* for tuning */
            g_node_set_dpos(node, &eyeW);
            g_last_eye = eyeW;             /* for the mid-frame re-assert */

            /* [foliage diag] how far is the game's per-frame camera position (used
             * by grass paging) from our FP eye? A large gap => grass pages around
             * the wrong point => pop-in under the feet. */
            if (KFP_DEBUG_LOG && have_pre) {
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

        /* Ragdoll orientation follow: while knocked down, tumble the view with
         * the head. We track the head's world orientation as the "upright"
         * reference WHILE NOT down, and freeze it when down -- so delta =
         * head_now * conj(upright) is the pure world-space rotation the head has
         * undergone from standing (bone-axis convention cancels). Apply that to
         * the look direction, blended in/out so knockdown and get-up are smooth. */
        {
            void *pc2 = first_player_char(gw);
            void *anim = (pc2 && readable((void *)((uintptr_t)pc2 + CHAR_ANIM), 8))
                ? *(void **)((uintptr_t)pc2 + CHAR_ANIM) : NULL;
            int is_down = readable((void *)((uintptr_t)anim + ANIM_RAGDOLL_MASK + 4), 4)
                          && *(unsigned int *)((uintptr_t)anim + ANIM_RAGDOLL_MASK) != 0;
            Quat qh;
            if (pc2 && get_head_quat(pc2, &qh)) {
                /* Discriminate knocked-out (head heavily rolled -> follow + freeze
                 * look) from crawling-crippled (head near-upright, still conscious
                 * -> keep normal look). Tilt = angle between head-now and upright. */
                float tiltdeg = 0.0f;
                if (g_have_qref) {
                    Quat d = quat_norm(quat_mul(qh, quat_conj(g_qref)));
                    float w = d.w; if (w < 0) w = -w; if (w > 1) w = 1;
                    tiltdeg = 2.0f * acosf(w) * 57.29578f;
                }
                /* Require the head to actually be low (body prone), not just the
                 * ragdoll-parts mask + head tilt -- a critically injured but
                 * UPRIGHT character sets the mask and can slump the head past the
                 * tilt threshold while still standing (head stays ~1.6m up).
                 * PS_KO (authoritative prone state) triggers it directly too: the
                 * tilt heuristic needs an UPRIGHT reference (g_qref) captured
                 * before the knockdown, which we DON'T have after switching to a
                 * character who is ALREADY unconscious -- so the vignette used to
                 * vanish on swap. PS_KO doesn't need the reference. */
                int ko = char_prone_state(pc2) == PS_KO;
                int truly_down = ko ||
                                 (is_down && g_have_qref && tiltdeg > 55.0f
                                  && g_head_above < 0.9f);
                g_is_down = truly_down;   /* next frame's look-input freeze reads this */
                static int prev_down = -1;
                if (KFP_DEBUG_LOG && ((int)truly_down != prev_down || (is_down && !truly_down))) {
                    logline("[orient] is_down=%d truly=%d ko=%d tilt=%.0f headY=%.2f",
                            is_down, truly_down, ko, tiltdeg, g_head_above);
                    prev_down = truly_down;
                }
                /* Freeze the upright reference on the raw ragdoll signal (not on
                 * truly_down) so tilt measures from the pre-down pose and can
                 * actually accumulate as the head rolls -- otherwise it collapses
                 * to the per-frame delta (~0) and never crosses the threshold. */
                if (!is_down) { g_qref = qh; g_have_qref = 1; }
                float target = truly_down ? 1.0f : 0.0f;
                g_down_blend += (target - g_down_blend) * 0.15f; /* ~0.3s ease */
                if (g_down_blend > 0.002f && g_have_qref) {
                    Quat delta = quat_norm(quat_mul(qh, quat_conj(g_qref)));
                    Quat ragq = quat_norm(quat_mul(delta, q));   /* head tumble applied to view */
                    /* Head bone's local axes are permuted from camera-frame: seat with
                     * a 90-right yaw then a 90-up pitch, applied in the view's frame. */
                    static const Quat YAW90R  = { 0.70710678f, 0.0f, -0.70710678f, 0.0f }; /* -90 Y */
                    static const Quat PITCH90U = { 0.70710678f, -0.70710678f, 0.0f, 0.0f }; /* -90 X */
                    ragq = quat_norm(quat_mul(ragq, quat_mul(YAW90R, PITCH90U)));
                    q = quat_slerp(q, ragq, g_down_blend);
                }
            } else {
                g_down_blend = 0.0f;
            }
        }
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
        g_cursor_hidden = 0; g_ui_prev = 0; g_ui_open = 0; g_ui_moveblock = 0;
        mygui_cursor(1);                        /* FP exit: restore the game cursor */
        /* widget hides handled post-frame by fp_gui_update (sees !g_fp_mode) */
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
        /* Seat the vanilla camera at our look direction + the saved zoom via
         * the game's own API. Fixes the endless zoom-out (garbage local pos
         * was left as the zoom) and keeps view continuity on exit. */
        vanilla_cam_handoff(cam, node, g_vanilla_zoom);
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
 * unwinding -- safe across foreign (game) frames. (Declarations hoisted above
 * get_head_quat, which also uses the guard.) */

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
    if (KFP_DEBUG_LOG && mask != last_mask) {  /* diag: which check flips the gate */
        last_mask = mask;
        logline("[ui] panel mask=0x%03x", mask);
    }
    g_ui_mask = mask;
    return mask != 0;
}

/* Character::playerMoveOrderDefault(Building* dest, RootObject* subject,
 * Vector3& loc) -- THE vanilla right-click walk order (KenshiLib: vtable +0x318,
 * RVA 0x5D1B30 in the SDK build). Called via the VTABLE SLOT, not a hard-coded
 * RVA: the previous hard-coded 0x5d22b0 was NOT this function (it isn't a
 * Character method in the SDK header at all) -- point releases shift RVAs while
 * the vtable layout stays put, so the slot is correct on every build/edition.
 * Issues/live-updates a Task_Move(0x1d): pathfinds, ground-follows, animates,
 * cancels the current job, and makes a downed/seated/bedded character get up.
 * Crash-guarded. Returns 1 if the call completed, 0 if unavailable/faulted. */
static int try_move_to_pos(void *pc, const Vec3 *tgt)
{
    if (g_movetopos_dead) return 0;
    void **vt = *(void ***)pc;
    if (!in_module(vt)) return 0;
    void (*fn)(void *, void *, void *, const Vec3 *) =
        (void (*)(void *, void *, void *, const Vec3 *))vt[CHAR_VT_PLAYERMOVE];
    if (!in_module((void *)fn)) return 0;
    if (setjmp(g_guard_jb)) {          /* longjmp target: the call blew up */
        g_movetopos_dead = 1;
        g_guard_armed = 0;
        logline("playerMoveOrderDefault FAULTED -- disabled for this session");
        return 0;
    }
    g_guard_armed = 1;
    fn(pc, NULL, NULL, tgt);          /* NULL,NULL = plain ground point */
    g_guard_armed = 0;
    return 1;
}

/* Character::_currentProneState (member 0xE0): 0 normal, 2 crippled, 3 playing
 * dead, 4 KO. Authoritative -- replaces the head-height heuristic for get-up. */
static int char_prone_state(void *pc)
{
    return readable((void *)((uintptr_t)pc + CHAR_PRONE_STATE), 4)
        ? *(int *)((uintptr_t)pc + CHAR_PRONE_STATE) : PS_NORMAL;
}

/* Set the game's OWN "the player wants me up" flag (member 0xEC). This is what
 * a vanilla move order sets to break a character out of playing-dead / a bed /
 * a seat and start the get-up. A plain, safe memory write -- no call. */
static void char_request_getup(void *pc)
{
    if (readable((void *)((uintptr_t)pc + CHAR_WANTS_GETUP), 1))
        *(unsigned char *)((uintptr_t)pc + CHAR_WANTS_GETUP) = 1;
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

/* --- direct-drive locomotion (indoor-reliable movement) --------------------
 * The engine has a NATIVE direction-driven mode (MOVE_DIRECTION): feed
 * CharMovement::setDirectMovement(dir, limit) a direction each frame and the
 * game's own locomotion moves the character -- collision, floors, ramps,
 * platforms all handled by the engine. No destination point, no pathfinder,
 * no terrain ray: this is what makes movement reliable INSIDE buildings,
 * where the click-order + terrain-ray scheme picked unreachable/underground
 * targets. (Approach learned from smokefoolius/Kenshi-Direct-Control.)
 * Prone/downed characters keep the order path: crawling and the get-up flow
 * need the click-order system, and MOVE_DIRECTION is standing-only. */
#define MV_SPEEDORDERS    0x20     /* CharMovement::speedOrders (MoveSpeed) */
#define MV_MOVEMODE       0x378    /* MovementMode: 0 normal, 1 combat, 2 direction */
#define MV_DESIREDMOTION  0x38C    /* Vector3, consumed when mode==2 */
#define MV_HALT_SLOT      (0x98/8) /* vtable: halt() -- cancels current orders */
#define MV_SETSPEED_SLOT  (0xA8/8) /* vtable: setDesiredSpeed(MoveSpeed) */

static void fp_movement(void *gw, float dt)
{
    if (!g_fp_mode || !g_charmove_setdest) {
        InterlockedExchange(&g_dm_active, 0);   /* FP off mid-hold: stand down */
        return;
    }
    void *pc = first_player_char(gw);
    if (!pc) return;
    void *mv = readable((void *)((uintptr_t)pc + CHAR_MOVEMENT), 8)
        ? *(void **)((uintptr_t)pc + CHAR_MOVEMENT) : NULL;
    if (!readable(mv, 8)) return;

    /* Scrollwheel throttle: consume the wheel captured by our LL hook. */
    LONG wheel = InterlockedExchange(&g_wheel_accum, 0);
    g_dbg_wheel = (int)wheel;
    if (!g_cfg_wheel) wheel = 0;   /* wheel speed control disabled in config */
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
    /* Treat a HARD UI block (dialogue/cutscene, control disabled) as keys-
     * released: halt if moving. A plain side panel does NOT block movement --
     * WASD keeps walking the character while inventory/squad/jobs are open,
     * matching vanilla (which still pans the camera then). */
    if (g_ui_moveblock || keys == 0 || (mf == 0.0f && mr == 0.0f)) {
        if (g_was_moving) {         /* release: halt at current position */
            if (g_was_direct) {
                /* direct drive: instant stop -- zero the motion, return the
                 * mode to normal so vanilla ordering owns the character again */
                InterlockedExchange(&g_dm_active, 0);
                if (readable((void *)((uintptr_t)mv + MV_DESIREDMOTION), 12)) {
                    Vec3 *dm = (Vec3 *)((uintptr_t)mv + MV_DESIREDMOTION);
                    dm->x = dm->y = dm->z = 0.0f;
                    *(int *)((uintptr_t)mv + MV_MOVEMODE) = 0;   /* MOVE_NORMAL */
                }
                g_was_direct = 0;
                /* Belt-and-suspenders: if a Task_Move somehow re-appeared during
                 * the hold, replace it with stop-here so releasing WASD leaves us
                 * where we stopped instead of resuming an old destination. */
                Vec3 hpos;
                if (char_task_is_move(pc) && char_position(pc, &hpos))
                    try_move_to_pos(pc, &hpos);
            } else {
                Vec3 here;
                /* Halt ONLY if our walk task still owns the character. If an
                 * NPC-forced task (dialogue, grab, arrest) took over, issuing a
                 * halt would CLEAR their queue mid-mutation -> crash. */
                if (char_task_is_move(pc) && char_position(pc, &here)) {
                    if (!try_move_to_pos(pc, &here))   /* order to current pos = stop */
                        g_charmove_setdest(mv, &here, UPDATE_PRIORITY_HIGH, 0);
                }
            }
            g_was_moving = 0;
            g_have_dest = 0;
            g_lead_sm = 0.0f;           /* fresh lead estimate on next move */
            g_stuck_frames = 0;         /* fresh pinned estimate on next move */
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

    /* Pinned-state detection: MOVE_DIRECTION only drives a STANDING body. When
     * the character is seated (chair/bench/bar), in a bed, or mid get-up, the
     * head is still up (so head_above doesn't flag it) yet setDirectMovement
     * can't translate them -- it just spins them in place. Detect "commanding
     * direct drive but not actually moving" and route to the point-click ORDER
     * path instead, which cancels the holding job and walks us out. Clears as
     * soon as real movement resumes, so normal standing locomotion is untouched
     * (walking is ~26 u/s, far above the 2 u/s pinned floor). */
    if (g_was_direct && g_move_speed < 2.0f) { if (g_stuck_frames < 600) g_stuck_frames++; }
    else if (g_move_speed >= 4.0f) g_stuck_frames = 0;
    int pinned = g_stuck_frames > 15;   /* ~0.25s of no progress under direct drive */

    /* Authoritative state (KenshiLib members): prone state and bed flag tell us
     * directly when the character can't do standing MOVE_DIRECTION locomotion
     * and must instead be issued a get-up move order. */
    int prone = char_prone_state(pc);   /* 0 normal, 2 crippled, 3 play-dead, 4 KO */
    int in_bed = readable((void *)((uintptr_t)pc + CHAR_IN_SOMETHING), 4)
                 && *(int *)((uintptr_t)pc + CHAR_IN_SOMETHING) == 1;   /* IN_BED */
    /* "downed" = anything that isn't normal standing: an authoritative prone
     * state (playing dead / crippled / KO / staying-low), lying in a bed, a
     * physically low head, or the pinned fallback (a SEAT is a job, not a prone
     * state, so only stuck-detection catches it). Route these to the order path,
     * which cancels the holding job and runs the get-up. */
    int downed = (prone != PS_NORMAL) || in_bed || (g_head_above < 0.9f) || pinned;
    if (KFP_DEBUG_LOG) {
        static int lg;
        if ((++lg % 20) == 1) {
            int mode = readable((void *)((uintptr_t)mv + MV_MOVEMODE), 4)
                       ? *(int *)((uintptr_t)mv + MV_MOVEMODE) : -1;
            int engaged = readable((void *)((uintptr_t)pc + 0x250), 1)
                          ? *(unsigned char *)((uintptr_t)pc + 0x250) : -1;   /* _isEngagedWithAPlayer */
            logline("[getup] prone=%d in_bed=%d headY=%.2f pinned=%d spd=%.1f downed=%d dm_active=%ld was_direct=%d mode=%d engaged=%d",
                    prone, in_bed, g_head_above, pinned, g_move_speed, downed,
                    g_dm_active, g_was_direct, mode, engaged);
        }
    }

    /* STANDING: engine-native direct drive -- reliable on floors, platforms,
     * ramps, and everywhere the terrain-ray/click-order scheme wasn't. */
    if (!downed) {
        void **mvvt = *(void ***)mv;
        if (in_module(mvvt)) {
            /* Point-click DISENGAGE on the WASD press edge (outside the setjmp
             * guard below -- try_move_to_pos has its own). This is the vanilla
             * right-click order, and it cancels WHATEVER state currently holds
             * the character: a leftover move order, a chair/bench, a bed, combat
             * lock, playing dead. It is deliberately NOT gated on task type --
             * gating on Task_Move (our own order only) left seated/bedded/jobbed
             * characters pinned, so WASD merely SPUN them in place instead of
             * getting them up. Issue it toward the WASD direction (not the exact
             * spot) so the engine commits to stand-up + step. moveToPosition is
             * the same call a right-click makes and is SEH-guarded, so a fault on
             * a genuinely non-orderable state is contained rather than fatal. */
            if (!g_was_direct) {
                char_request_getup(pc);   /* break a seat before pinned-detect kicks in */
                Vec3 hpos;
                if (char_position(pc, &hpos)) {
                    Vec3 dis = { hpos.x + dx * 6.0f, hpos.y, hpos.z + dz * 6.0f };
                    try_move_to_pos(pc, &dis);
                }
            }
            if (setjmp(g_guard_jb)) { g_guard_armed = 0; return; }
            g_guard_armed = 1;
            if (!g_was_direct)   /* entering direct drive: also stop the mover */
                ((void (*)(void *))mvvt[MV_HALT_SLOT])(mv);
            int spd = g_speed_scale < 0.4f ? 0 : g_speed_scale < 0.8f ? 1 : 2;
            /* Publish the intent; the CharMovement::update hook enforces it
             * per-update (beats combat AI), and we also apply once here so
             * movement works even if that hook failed to install. */
            g_dm_dir.x = dx; g_dm_dir.y = 0.0f; g_dm_dir.z = dz;
            g_dm_speed = spd; g_dm_mv = mv;
            InterlockedExchange(&g_dm_active, 1);
            if (readable((void *)((uintptr_t)mv + MV_SPEEDORDERS), 4))
                *(int *)((uintptr_t)mv + MV_SPEEDORDERS) = spd;   /* WALK/JOG/RUN */
            ((void (*)(void *, int))mvvt[MV_SETSPEED_SLOT])(mv, spd);
            ((void (*)(void *, const Vec3 *, float))(g_base + RVA_SET_DIRECT_MOVE))(mv, &g_dm_dir, 99.0f);
            g_guard_armed = 0;
            g_was_direct = 1;
            g_was_moving = 1;
            return;
        }
    }
    if (g_was_direct) {   /* fell/crippled mid-hold: return mode to normal */
        InterlockedExchange(&g_dm_active, 0);
        if (readable((void *)((uintptr_t)mv + MV_DESIREDMOTION), 12)) {
            Vec3 *dm = (Vec3 *)((uintptr_t)mv + MV_DESIREDMOTION);
            dm->x = dm->y = dm->z = 0.0f;
            *(int *)((uintptr_t)mv + MV_MOVEMODE) = 0;
        }
        g_was_direct = 0;
    }

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
    if (downed) {
        /* GET UP from the ground, a bed, being seated, or playing dead. Set the
         * game's OWN get-up flag (what a real move order sets to break the state)
         * and issue a FAR, un-clamped ground order in the WASD direction. That
         * combination reliably commits the engine to get-up + walk -- the near,
         * terrain-clamped targeting below is tuned for standing slope-climbing and
         * doesn't trigger get-up dependably (and a bed sits above the ground, so
         * clamping to the surface underneath can fabricate an unreachable target).
         * If the character is genuinely KO the engine ignores it until recovery,
         * so it does no harm. */
        char_request_getup(pc);
        tgt.x = here.x + dx * 500.0f; tgt.y = here.y; tgt.z = here.z + dz * 500.0f;
    } else if (mf > 0.0f && mr == 0.0f) {
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
    if (!via_ray && !downed) {
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

/* Build an MSVC std::string in b32 for ANY length. len<16 -> SSO (returns NULL);
 * len>=16 -> data at a heap buffer we malloc and RETURN so the caller frees it
 * AFTER the (copying) consumer call. Never let MSVC free it (allocator mismatch);
 * the consumer copies the content, we free our own buffer. */
static void *make_mstr_long(unsigned char *b32, const char *s)
{
    memset(b32, 0, 32);
    size_t len = strlen(s);
    if (len < 16) {
        memcpy(b32, s, len);
        *(size_t *)(b32 + 0x10) = len;
        *(size_t *)(b32 + 0x18) = 15;
        return NULL;
    }
    char *buf = (char *)malloc(len + 1);
    if (!buf) { *(size_t *)(b32 + 0x18) = 15; return NULL; }
    memcpy(buf, s, len + 1);
    *(char **)b32 = buf;               /* heap data ptr at offset 0 */
    *(size_t *)(b32 + 0x10) = len;     /* size */
    *(size_t *)(b32 + 0x18) = len;     /* capacity >=16 => heap mode */
    return buf;
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
    /* KO vignette: fullscreen, created FIRST so it renders under the
     * crosshair within the same layer. Shown while knocked out. */
    if (!g_vignette) {
        unsigned char vt[32], vs[32], vl[32], vn[32], vtex[32];
        make_mstr(vt, "ImageBox"); make_mstr(vs, "ImageBox"); make_mstr(vl, "Wallpaper");
        make_mstr(vn, "FPVignette"); make_mstr(vtex, "vignette.png");
        void *vw = g_gui_createwidget(gui, vt, vs, 0, 0, cx, cy,
                                      0 /*Align::Default*/, vl, vn);
        if (readable(vw, 8)) {
            g_imgbox_setimage(vw, vtex);
            g_widget_setvisible(vw, 0);
            g_vignette = vw;
        }
    }
    if (!g_black_ov) {
        unsigned char bt[32], bs[32], bl[32], bn[32], btex[32];
        make_mstr(bt, "ImageBox"); make_mstr(bs, "ImageBox"); make_mstr(bl, "Wallpaper");
        make_mstr(bn, "FPBlackout"); make_mstr(btex, "blk0.png");
        void *bw = g_gui_createwidget(gui, bt, bs, 0, 0, cx, cy,
                                      0, bl, bn);
        if (readable(bw, 8)) {
            g_imgbox_setimage(bw, btex);
            g_widget_setvisible(bw, 0);
            g_black_ov = bw;
        }
    }
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
        g_crosshair_red = 0;   /* fresh widget starts on the white texture */
        logline("crosshair widget created %p", w);
    }
}

/* Desired crosshair tint (0 white / 1 red / 2 yellow), published by the
 * setPointer hook and APPLIED post-frame in fp_gui_update -- calling MyGUI
 * setImageTexture from inside the hook (which runs during the game's own UI
 * processing) was a reentrancy hazard that could corrupt MyGUI's widget
 * lists and crash the order/job/inventory ItemBoxes. */
static volatile LONG g_crosshair_want;

/* MyGUI PointerManager::setPointer hook: while FP is on, hide the default (arrow)
 * cursor — we show our crosshair instead — and keep contextual icon pointers
 * (speech/door/loot/...) visible. The vanilla COLORED-ARROW pointers are
 * special-cased: "attk" (red, hostile) and "green" (ally) are suppressed and
 * our crosshair takes their color instead. */
static void hooked_setpointer(void *pm, const void *name)
{
    g_pm_setpointer_orig(pm, name);
    if (!g_fp_mode || !g_pm_setvisible) return;
    if (g_ui_open) { g_pm_setvisible(pm, 1); g_pointer_default = 0; return; }
    if (!g_pm_getdefault) return;
    char cur[128], def[128];
    read_mstring(name, cur, sizeof cur);
    read_mstring(g_pm_getdefault(pm), def, sizeof def);
    /* one-shot state map: record each pointer name the first time it appears
     * (cheap, bounded) -- authoritative record of which states the game uses */
    if (KFP_DEBUG_LOG) {
        static char seen[16][24]; static int nseen;
        int i, hit = 0;
        for (i = 0; i < nseen; i++) if (strcmp(seen[i], cur) == 0) { hit = 1; break; }
        if (!hit && nseen < 16 && cur[0]) {
            snprintf(seen[nseen], sizeof seen[0], "%s", cur); nseen++;
            logline("[ptr] first seen: \"%s\"", cur);
        }
    }
    int is_default = (cur[0] != '\0' && strcmp(cur, def) == 0);
    int tint = (strcmp(cur, "attk") == 0) ? 1
             : (strcmp(cur, "green") == 0) ? 2 : 0;
    InterlockedExchange(&g_crosshair_want, tint);   /* applied post-frame */
    /* colored arrows: hide the vanilla cursor, our tinted crosshair stands in.
     * setVisible on the PointerManager is safe (not a widget-list op). */
    g_pm_setvisible(pm, (is_default || tint) ? 0 : 1);
    g_pointer_default = is_default || tint;
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

/* Collapse (disable=1) or restore (0) a character's head bone via Kenshi's own
 * decapitation call. VEH-guarded. Returns 1 on a completed call. */
static int set_head_disabled(void *pc, int disable)
{
    if (!g_disable_bone || !pc) return 0;
    void *anim = readable((void *)((uintptr_t)pc + CHAR_ANIM), 8)
        ? *(void **)((uintptr_t)pc + CHAR_ANIM) : NULL;
    if (!readable(anim, ANIM_SKELETON + 8)) return 0;
    void *skel = *(void **)((uintptr_t)anim + ANIM_SKELETON);
    if (!readable(skel, 8)) return 0;
    if (setjmp(g_guard_jb)) {
        g_disable_bone = NULL;
        logline("disableBone FAULTED -- head-hide disabled for this session");
        return 0;
    }
    unsigned char name[32];
    make_mstr(name, "Bip01 Head");   /* std::string by value; SSO -> callee dtor is a no-op */
    g_guard_armed = 1;
    g_disable_bone(skel, name, disable ? 1 : 0);
    g_guard_armed = 0;
    return 1;
}

/* Hide the player's head while fast-forwarding (>1x game speed): at high speed
 * the head-welded camera visibly lags the head mesh. Body + shadow stay; only
 * the head vanishes, restored at 1x or on FP exit. Runs every frame so the
 * exit/speed-drop restore always fires. */
static void fp_head_visibility(void *gw)
{
    if (!g_disable_bone) return;
    float speed = readable((void *)((uintptr_t)gw + GW_FRAMESPEED), 4)
        ? *(float *)((uintptr_t)gw + GW_FRAMESPEED) : 1.0f;
    void *pc = first_player_char(gw);
    int want = g_fp_mode && pc && speed > 1.05f;
    if (want && !g_head_hidden) {
        if (set_head_disabled(pc, 1)) { g_head_hidden = 1; g_head_hidden_char = pc; }
    } else if (!want && g_head_hidden) {
        set_head_disabled(g_head_hidden_char ? g_head_hidden_char : pc, 0);
        g_head_hidden = 0; g_head_hidden_char = NULL;
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
    /* Snap ONLY when this frame's eye was anchored to the head bone: a
     * fallback eye derives FROM the center (center.y + EYE_HEIGHT), so
     * snapping center=eye ratchets the camera straight UP every fallback
     * frame (visible even paused). Any head-read hiccup while moving would
     * otherwise launch the camera. */
    if (g_was_moving && g_have_t && g_eye_from_head && g_node_set_dpos) {
        void *center = *(void **)((uintptr_t)cam + CC_CENTER);
        if (readable(center, 8)) {
            g_node_set_dpos(center, &g_last_eye);
            /* CRITICAL ordering: the camera node is a CHILD of the center.
             * _setDerivedPosition converts derived->local against the parent's
             * CACHED derived transform, and after the snap the center's cache
             * is STALE until the scene-graph update -- so a plain re-assert
             * still bakes the wrong local offset and the camera rides up by
             * the center displacement (~6u, "camera rises while holding WASD").
             * Force the center's derived recompute FIRST (the game's own
             * manual-camera code does exactly this), then re-seat the camera. */
            Vec3 tmp;
            if (g_node_getdpos_upd) g_node_getdpos_upd(center, &tmp);
            void *node = *(void **)((uintptr_t)cam + CC_NODE);
            if (readable(node, 8)) {
                g_node_set_dpos(node, &g_last_eye);
                g_node_set_dori(node, &g_last_ori);
            }
        }
    }
}

/* --- optional FPS cap (KenshiFP.ini: fps_cap=N) --------------------------
 * Kenshi has no framerate limiter besides vsync, and vsync's frame pacing adds
 * jitter/latency in FP (also uncapped menus run 1000+ fps = coil whine). We own
 * the frame loop, so cap here: hybrid Sleep/spin against QueryPerformanceCounter
 * for accuracy that plain Sleep can't give. 0 (or no ini) = off. */
static int g_fps_cap;                        /* frames/sec; 0 = disabled */
static void fps_cap_wait(void)
{
    static LARGE_INTEGER freq, next;
    static int inited;
    if (g_fps_cap <= 0) return;
    if (!inited) { QueryPerformanceFrequency(&freq); QueryPerformanceCounter(&next); inited = 1; }
    LONGLONG period = freq.QuadPart / g_fps_cap;
    next.QuadPart += period;
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    if (next.QuadPart < now.QuadPart) { next = now; return; }   /* running behind: no wait */
    /* sleep the bulk (leave ~2ms), spin the remainder for precision */
    while (next.QuadPart - now.QuadPart > (freq.QuadPart / 500)) {
        Sleep(1);
        QueryPerformanceCounter(&now);
    }
    while (now.QuadPart < next.QuadPart) { YieldProcessor(); QueryPerformanceCounter(&now); }
}

static int ini_int(const char *line, const char *key, int *out)
{
    size_t kl = strlen(key);
    while (*line == ' ' || *line == '\t') line++;
    if (strncmp(line, key, kl) != 0) return 0;
    line += kl;
    while (*line == ' ' || *line == '\t') line++;
    if (*line++ != '=') return 0;
    while (*line == ' ' || *line == '\t') line++;
    *out = (int)strtol(line, NULL, 0);   /* decimal or 0x hex */
    return 1;
}

static int ini_float(const char *line, const char *key, float *out)
{
    size_t kl = strlen(key);
    while (*line == ' ' || *line == '\t') line++;
    if (strncmp(line, key, kl) != 0) return 0;
    line += kl;
    while (*line == ' ' || *line == '\t') line++;
    if (*line++ != '=') return 0;
    *out = (float)atof(line);
    return 1;
}

static void load_ini(void)
{
    FILE *f = fopen("KenshiFP.ini", "r");   /* CWD = game dir, same as KenshiFP.log */
    if (!f) return;
    char line[160]; int v; float fv;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == ';' || line[0] == '#') continue;
        if (ini_int(line, "fps_cap", &v))        g_fps_cap = (v >= 15 && v <= 1000) ? v : 0;
        else if (ini_int(line, "aim_lean", &v))       g_cfg_aim_lean = !!v;
        else if (ini_int(line, "ranged_freeaim", &v)) g_cfg_freeaim  = !!v;
        else if (ini_int(line, "wheel_speed", &v))    g_cfg_wheel    = !!v;
        else if (ini_int(line, "ko_vignette", &v))    g_cfg_vignette = !!v;
        else if (ini_int(line, "key_toggle_fp", &v))  { if (v > 0 && v < 255) g_cfg_key_fp = v; }
        else if (ini_int(line, "key_forward", &v))    { if (v > 0 && v < 255) g_cfg_key_w = v; }
        else if (ini_int(line, "key_left", &v))       { if (v > 0 && v < 255) g_cfg_key_a = v; }
        else if (ini_int(line, "key_back", &v))       { if (v > 0 && v < 255) g_cfg_key_s = v; }
        else if (ini_int(line, "key_right", &v))      { if (v > 0 && v < 255) g_cfg_key_d = v; }
        else if (ini_int(line, "key_settings", &v))   { if (v > 0 && v < 255) g_cfg_key_settings = v; }
        else if (ini_float(line, "fov", &fv))             { if (fv >= 40 && fv <= 120) g_cfg_fov = fv; }
        else if (ini_float(line, "near_clip", &fv))       { if (fv >= 0.05f && fv <= 10) g_cfg_nearclip = fv; }
        else if (ini_float(line, "sensitivity", &fv))     { if (fv >= 0.05f && fv <= 10) g_cfg_sens = fv; }
        else if (ini_float(line, "eye_forward", &fv))     { if (fv >= -5 && fv <= 10) g_cfg_eye_fwd = fv; }
        else if (ini_float(line, "move_forward", &fv))    { if (fv >= 0 && fv <= 10) g_cfg_move_fwd = fv; }
        else if (ini_float(line, "move_speed_ref", &fv))  { if (fv >= 5 && fv <= 400) g_cfg_move_ref = fv; }
        else if (ini_float(line, "aim_lean_amount", &fv)) { if (fv >= 0 && fv <= 1.5f) g_cfg_lean = fv; }
    }
    fclose(f);
    if (g_fps_cap) timeBeginPeriod(1);
    logline("config: fps_cap=%d aim_lean=%d freeaim=%d wheel=%d vignette=%d fpkey=0x%02X",
            g_fps_cap, g_cfg_aim_lean, g_cfg_freeaim, g_cfg_wheel, g_cfg_vignette, g_cfg_key_fp);
}

/* Hot reload: re-parse when the file's write time changes (checked ~2 Hz),
 * so settings apply in-game without a restart. */
static void ini_hot_reload(void)
{
    static DWORD last_check; static FILETIME last_wt;
    DWORD now = GetTickCount();
    if (now - last_check < 500) return;
    last_check = now;
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExA("KenshiFP.ini", GetFileExInfoStandard, &fad)) return;
    if (CompareFileTime(&fad.ftLastWriteTime, &last_wt) != 0) {
        last_wt = fad.ftLastWriteTime;
        load_ini();
    }
}

/* All MyGUI widget work, run POST-FRAME (from hooked_mainloop, after the
 * game's frame + UI update completed) so we never touch MyGUI's widget lists
 * mid-update. Reads only globals published during the frame. */
/* ------- in-game settings UI (MyGUI) : M1 = button + toggle window --------- */
static void *g_settings_win;    /* our floating settings Window (Kenshi_WindowCX) */
static void *g_settings_btn;    /* the injected "KenshiFP Settings" options-tab button */
/* g_settings_open declared up top (read by the FP cursor-lock code) */

/* Set a widget's caption from a C string via a temporary MyGUI::UString (a single
 * basic_string<char16> = 0x20 bytes). Construct on the stack, set, destruct. */
/* Set a caption via `setter` (Window::setCaption for the title bar,
 * TextBox::setCaption for plain labels), building a temporary MyGUI::UString. */
static void caption_set(void *w, const char *text, textbox_setcap_t setter)
{
    if (!w || !setter || !g_ustring_ctor) return;   /* dtor optional (tiny leak ok) */
    unsigned char ustr[0x40] = {0};   /* UString (basic_string<u16>) */
    g_ustring_ctor(ustr, text);
    setter(w, ustr);
    if (g_ustring_dtor) g_ustring_dtor(ustr);
}

static int g_settings_dead;     /* a MyGUI call faulted -> stand down permanently */

/* DFS the MyGUI widget tree for a widget whose name (minus its layout prefix,
 * the part after the last '_') equals `target`. The Enumerator returned by
 * getEnumerator is { bool m_first; Widget** begin; Widget** end } -- iterate the
 * begin..end pointer range. Depth-capped; assumes the caller armed g_guard_jb. */
static int g_find_budget;   /* nodes left to visit this search (runaway guard) */
__attribute__((unused))
static void *find_widget_suffix(void **begin, void **end, const char *target, int depth)
{
    if (!begin || !end || depth > 10) return NULL;
    /* sanity the enumerator range: a bad ABI/stale vector would give a wild
     * begin..end and iterating it would read wild memory forever. */
    if (end < begin || (size_t)(end - begin) > 2048) return NULL;
    for (void **it = begin; it != end; ++it) {
        if (--g_find_budget <= 0) return NULL;   /* total-node cap */
        if (!readable(it, 8)) return NULL;
        void *w = *it;
        if (!readable(w, 8)) continue;
        if (g_widget_getname) {
            const void *ns = g_widget_getname(w);
            char nm[128]; read_mstring(ns, nm, sizeof nm);
            const char *suf = strrchr(nm, '_'); suf = suf ? suf + 1 : nm;
            if (strcmp(suf, target) == 0) return w;
        }
        if (g_widget_getenum) {
            unsigned char ce[32];
            g_widget_getenum(w, ce);
            void *found = find_widget_suffix(*(void ***)(ce + 8), *(void ***)(ce + 16), target, depth + 1);
            if (found) return found;
        }
    }
    return NULL;
}

/* Find a vanilla menu widget by name-suffix, walking from the Gui roots. */
__attribute__((unused))
static void *settings_find(void *gui, const char *target)
{
    if (!g_gui_getenum) return NULL;
    g_find_budget = 4000;
    unsigned char ge[32];
    g_gui_getenum(gui, ge);
    return find_widget_suffix(*(void ***)(ge + 8), *(void ***)(ge + 16), target, 0);
}

/* Ensure our floating settings window exists (created lazily the first time the
 * button is clicked -- NOT at load, to avoid touching MyGUI during load/menu
 * transitions). Returns the window or NULL. */
/* Create a child widget on `parent`, handling skin/type/name strings of any
 * length (Kenshi skin names exceed the 15-char SSO limit). */
static void *make_child(void *parent, const char *type, const char *skin,
                        int l, int t, int w, int h, const char *name)
{
    if (!g_widget_createwidget || !parent) return NULL;
    unsigned char ty[32], sk[32], nm[32];
    void *tf = make_mstr_long(ty, type);
    void *sf = make_mstr_long(sk, skin);
    void *nf = make_mstr_long(nm, name);
    void *child = g_widget_createwidget(parent, ty, sk, l, t, w, h, 0, nm);
    if (tf) free(tf);
    if (sf) free(sf);
    if (nf) free(nf);
    return child;
}

/* --- settings model: numeric sliders + toggle checkboxes -------------------- */
typedef struct { const char *label; float *cfg; float lo, hi, def; void *slider, *valtb; int lastpos; } fset_t;
static fset_t g_fsets[] = {
    { "FOV",             &g_cfg_fov,      40.0f, 120.0f, 70.0f, 0, 0, -1 },
    { "Sensitivity",     &g_cfg_sens,      0.1f,   5.0f,  1.0f, 0, 0, -1 },
    { "Near clip",       &g_cfg_nearclip,  0.05f,  3.0f,  1.0f, 0, 0, -1 },
    { "Eye forward",     &g_cfg_eye_fwd,  -2.0f,   8.0f,  2.0f, 0, 0, -1 },
    { "Move forward",    &g_cfg_move_fwd,  0.0f,   6.0f,  1.5f, 0, 0, -1 },
    { "Move speed ref",  &g_cfg_move_ref, 20.0f, 200.0f, 60.0f, 0, 0, -1 },
    { "Aim-lean amount", &g_cfg_lean,      0.0f,   1.5f,  0.5f, 0, 0, -1 },
};
typedef struct { const char *label; int *cfg, def; void *btn; } tset_t;
static tset_t g_tsets[] = {
    { "Aim lean",        &g_cfg_aim_lean, 1, 0 },
    { "Ranged free-aim", &g_cfg_freeaim,  1, 0 },
    { "Wheel speed",     &g_cfg_wheel,    1, 0 },
    { "KO vignette",     &g_cfg_vignette, 1, 0 },
};
static void *g_reset_btn, *g_close_btn;
#define KFP_SCROLL_RANGE 1000
#define FN(a) ((int)(sizeof(a)/sizeof((a)[0])))

/* Is the widget currently under the mouse `target` (or a descendant of it)?
 * Uses MyGUI's own mouse-focus tracking -- no coordinate math. */
static int widget_clicked(void *target)
{
    if (!target || !g_input_getinst || !g_mousefocus) return 0;
    void *im = g_input_getinst();
    if (!im) return 0;
    void *f = g_mousefocus(im);
    for (int i = 0; i < 10 && readable(f, 8); i++) {
        if (f == target) return 1;
        if (!g_widget_getparent) break;
        f = g_widget_getparent(f);
    }
    return 0;
}

/* Push a float setting's current value onto its slider + value label. */
static void settings_apply_slider(fset_t *fs)
{
    if (fs->slider && g_scroll_setpos) {
        float frac = (*fs->cfg - fs->lo) / (fs->hi - fs->lo);
        if (frac < 0) frac = 0; else if (frac > 1) frac = 1;
        int pos = (int)(frac * (KFP_SCROLL_RANGE - 1) + 0.5f);
        g_scroll_setpos(fs->slider, (size_t)pos);
        fs->lastpos = pos;
    }
    if (fs->valtb) { char vb[32]; snprintf(vb, sizeof vb, "%.2f", *fs->cfg); caption_set(fs->valtb, vb, g_textbox_setcap); }
}

static void settings_reset_defaults(void)
{
    for (int i = 0; i < FN(g_fsets); i++) { *g_fsets[i].cfg = g_fsets[i].def; settings_apply_slider(&g_fsets[i]); }
    for (int i = 0; i < FN(g_tsets); i++) {
        *g_tsets[i].cfg = g_tsets[i].def;
        if (g_tsets[i].btn && g_btn_setsel) g_btn_setsel(g_tsets[i].btn, *g_tsets[i].cfg ? 1 : 0);
    }
    if (KFP_DEBUG_LOG) logline("[settings] reset to defaults");
}

/* Build the rows once, on window creation. Sliders drag natively (we poll their
 * value); checkboxes we click-poll (a plain Button doesn't self-toggle). */
static void settings_build_content(void *win)
{
    if (!win || !g_scroll_setrange || !g_scroll_setpos) return;
    const int pad = 16, rowh = 30, labw = 150, sx = 175, sw = 210, vx = 395, vw = 95;
    int y = 14;
    for (int i = 0; i < FN(g_fsets); i++) {
        fset_t *fs = &g_fsets[i];
        char nm[32];
        snprintf(nm, sizeof nm, "KFPlbl%d", i);
        void *lbl = make_child(win, "TextBox", "Kenshi_GenericTextBoxFlat", pad, y, labw, 24, nm);
        if (lbl) caption_set(lbl, fs->label, g_textbox_setcap);
        snprintf(nm, sizeof nm, "KFPsld%d", i);
        void *sld = make_child(win, "ScrollBar", "Kenshi_ScrollBar", sx, y + 2, sw, 18, nm);
        if (sld) {
            g_scroll_setrange(sld, KFP_SCROLL_RANGE);
            float frac = (*fs->cfg - fs->lo) / (fs->hi - fs->lo);
            if (frac < 0) frac = 0; else if (frac > 1) frac = 1;
            int pos = (int)(frac * (KFP_SCROLL_RANGE - 1) + 0.5f);
            g_scroll_setpos(sld, (size_t)pos);
            fs->lastpos = pos;
        }
        fs->slider = sld;
        snprintf(nm, sizeof nm, "KFPval%d", i);
        void *val = make_child(win, "TextBox", "Kenshi_GenericTextBoxFlat", vx, y, vw, 24, nm);
        fs->valtb = val;
        if (val) { char vb[32]; snprintf(vb, sizeof vb, "%.2f", *fs->cfg); caption_set(val, vb, g_textbox_setcap); }
        y += rowh;
    }
    y += 10;
    for (int i = 0; i < FN(g_tsets); i++) {
        tset_t *ts = &g_tsets[i];
        char nm[32];
        snprintf(nm, sizeof nm, "KFPtl%d", i);
        void *lbl = make_child(win, "TextBox", "Kenshi_GenericTextBoxFlat", pad, y, labw, 24, nm);
        if (lbl) caption_set(lbl, ts->label, g_textbox_setcap);
        snprintf(nm, sizeof nm, "KFPck%d", i);
        void *chk = make_child(win, "Button", "Kenshi_TickBoxSkin", sx, y, 24, 24, nm);
        if (chk && g_btn_setsel) g_btn_setsel(chk, *ts->cfg ? 1 : 0);
        ts->btn = chk;
        y += rowh;
    }
    g_reset_btn = make_child(win, "Button", "Kenshi_Button1", pad, y + 8, 190, 32, "KFPreset");
    if (g_reset_btn) caption_set(g_reset_btn, "Reset to Defaults", g_textbox_setcap);
    g_close_btn = make_child(win, "Button", "Kenshi_Button1", pad + 200, y + 8, 110, 32, "KFPclose");
    if (g_close_btn) caption_set(g_close_btn, "Close", g_textbox_setcap);
    if (KFP_DEBUG_LOG) logline("[settings] built %d sliders + %d toggles; reset=%p close=%p", FN(g_fsets), FN(g_tsets), g_reset_btn, g_close_btn);
}

/* Poll widget state -> config, every frame while the window is open. */
static void settings_poll_content(void)
{
    for (int i = 0; i < FN(g_fsets); i++) {
        fset_t *fs = &g_fsets[i];
        if (!fs->slider || !g_scroll_getpos) continue;
        int pos = (int)g_scroll_getpos(fs->slider);
        if (pos == fs->lastpos) continue;           /* only on drag */
        fs->lastpos = pos;
        float frac = pos / (float)(KFP_SCROLL_RANGE - 1);
        if (frac < 0) frac = 0; else if (frac > 1) frac = 1;
        *fs->cfg = fs->lo + frac * (fs->hi - fs->lo);
        if (fs->valtb) { char vb[32]; snprintf(vb, sizeof vb, "%.2f", *fs->cfg); caption_set(fs->valtb, vb, g_textbox_setcap); }
    }
    /* checkbox click-poll: LMB release inside a box toggles its config */
    static int lmb_prev;
    int lmb = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    int released = (lmb_prev && !lmb);
    lmb_prev = lmb;
    for (int i = 0; i < FN(g_tsets); i++) {
        tset_t *ts = &g_tsets[i];
        if (!ts->btn) continue;
        if (released && widget_clicked(ts->btn)) *ts->cfg = !*ts->cfg;
        if (g_btn_setsel) g_btn_setsel(ts->btn, *ts->cfg ? 1 : 0);
    }
    if (released && widget_clicked(g_reset_btn)) settings_reset_defaults();
}

static void settings_ensure_window(void *gui)
{
    if (g_settings_win || !g_gui_createwidget) return;
    unsigned char ty[32], sk[32], nm[32], lay[32];
    make_mstr(ty, "Window"); make_mstr(sk, "Kenshi_WindowCX");
    make_mstr(nm, "KFPSettingsWin"); make_mstr(lay, "Window");
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    /* dynamic height: fit all rows + the reset button + window chrome */
    int content_bottom = 14 + FN(g_fsets) * 30 + 10 + FN(g_tsets) * 30 + 8 + 32;
    int ww = 520, wh = content_bottom + 60;
    int wx = (sw > 0 ? sw / 2 - ww / 2 : 200), wy = (sh > 0 ? sh / 2 - wh / 2 : 150);
    g_settings_win = g_gui_createwidget(gui, ty, sk, wx, wy, ww, wh, 0, lay, nm);
    if (g_settings_win) {
        caption_set(g_settings_win, "KenshiFP Settings", g_window_setcap);
        settings_build_content(g_settings_win);
        g_widget_setvisible(g_settings_win, 0);
    }
    if (KFP_DEBUG_LOG) logline("[settings] window create -> %p", g_settings_win);
}

/* Post-frame (MyGUI-safe), independent of FP mode. FULLY crash-guarded: a fault
 * in any MyGUI call disables the settings UI for the session instead of taking
 * the game down, and the step logs pinpoint which call faulted. */
static void fp_settings_ui(void)
{
    if (g_settings_dead) return;
    if (!g_gui_getinstance || !g_gui_createwidget || !g_widget_createwidget
        || !g_gui_findwidget || !g_widget_setvisible
        || !g_tabctrl_itemcount || !g_tabctrl_itemat) return;
    /* let the game settle past the load screen before we touch the menu GUI */
    static int warmup; if (warmup < 120) { warmup++; return; }

    if (setjmp(g_guard_jb)) {          /* a MyGUI call blew up */
        g_guard_armed = 0;
        g_settings_dead = 1;
        logline("[settings] MyGUI FAULT -- settings UI disabled for this session");
        return;
    }
    g_guard_armed = 1;

    void *gui = g_gui_getinstance();
    if (!gui) { g_guard_armed = 0; return; }

    /* Toggle the settings window with a HOTKEY (default F10). The menu-BUTTON
     * injection was abandoned: creating a widget into the game's options menu at
     * the exact frame it builds hard-crashes the game (a deferred render-time
     * crash the VEH guard can't catch -- confirmed via log: createWidgetT on the
     * mods tab the instant its item-count went 0->6). Our own top-level window is
     * safe (same as the crosshair/vignette widgets). [settings_find + the tree
     * walk are retained for a future, more careful button attempt.] */
    static int key_prev;
    int key = (GetAsyncKeyState(g_cfg_key_settings) & 0x8000) != 0;
    if (key && !key_prev) {
        settings_ensure_window(gui);
        g_settings_open = !g_settings_open;
        if (g_settings_win) g_widget_setvisible(g_settings_win, g_settings_open ? 1 : 0);
        if (KFP_DEBUG_LOG) logline("[settings] hotkey toggle -> %d win=%p", g_settings_open, g_settings_win);
    }
    key_prev = key;

    if (g_settings_open && g_settings_win) {
        settings_poll_content();
        /* Close on Escape, F10, the in-panel Close button, or the title-bar X.
         * The X is MyGUI's own close button -- a widget named "Button" that sits
         * (via the title-bar frame) directly under our window, distinct from our
         * content which lives under the window's "Client" sub-widget. Detect a
         * click on any "Button"-named descendant of the window. */
        static int esc_prev, lmb_prev;
        int esc = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
        int lmb = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        int close = (esc && !esc_prev);
        if (lmb_prev && !lmb) {
            if (widget_clicked(g_close_btn)) close = 1;
            void *im = g_input_getinst ? g_input_getinst() : NULL;
            void *f = (im && g_mousefocus) ? g_mousefocus(im) : NULL;
            if (f && g_widget_getname) {
                char nm[24]; read_mstring(g_widget_getname(f), nm, sizeof nm);
                if (strcmp(nm, "Button") == 0) {          /* MyGUI title-bar X */
                    void *a = f;
                    for (int i = 0; i < 12 && readable(a, 8); i++) {
                        if (a == g_settings_win) { close = 1; break; }
                        if (!g_widget_getparent) break;
                        a = g_widget_getparent(a);
                    }
                }
            }
        }
        esc_prev = esc; lmb_prev = lmb;
        if (close) {
            g_settings_open = 0;
            g_widget_setvisible(g_settings_win, 0);
            if (KFP_DEBUG_LOG) logline("[settings] closed");
        }
    }
    g_guard_armed = 0;
}

static void fp_gui_update(void)
{
    if (!g_gui_getinstance) return;
    fp_settings_ui();                     /* runs regardless of FP mode */
    static int prev_fp;
    if (!g_fp_mode) {                      /* FP off: hide everything on the exit edge */
        if (prev_fp && g_widget_setvisible) {
            if (g_crosshair) g_widget_setvisible(g_crosshair, 0);
            if (g_vignette)  g_widget_setvisible(g_vignette, 0);
            if (g_black_ov)  g_widget_setvisible(g_black_ov, 0);
            /* Reset the KO fade so a re-enter doesn't flash a stale blackout. The
             * static `hidden` guard used to latch on the FIRST exit and never
             * re-arm, so exiting FP while unconscious a second time left the
             * blackout stuck on screen. Edge-trigger on prev_fp fixes that. */
            g_down_blend = 0.0f;
        }
        prev_fp = 0;
        return;
    }
    prev_fp = 1;
    ensure_crosshair();                    /* create (post-frame = safe) */

    /* crosshair tint (deferred from the setPointer hook) + visibility */
    if (g_crosshair && g_imgbox_setimage) {
        int want = (int)g_crosshair_want;
        if (want != g_crosshair_red) {
            g_crosshair_red = want;
            unsigned char tex[32];
            make_mstr(tex, want == 1 ? "xhair_red.png"
                        : want == 2 ? "xhair_ylw.png" : "crosshair.png");
            g_imgbox_setimage(g_crosshair, tex);
        }
    }
    if (g_crosshair && g_widget_setvisible)
        g_widget_setvisible(g_crosshair, (g_pointer_default && !g_ui_open) ? 1 : 0);

    /* Destreza-style unconsciousness: tunnel vignette + stepped blackout fade,
     * with a 1.7s smoothstep wake-up fade on regaining consciousness. */
    if (g_vignette && g_black_ov && g_widget_setvisible) {
        static float wake; static int prev_out;
        int out = g_is_down;
        if (prev_out && !out) wake = 1.0f;
        prev_out = out;
        float dt = (g_frame_dt > 0.0f && g_frame_dt < 0.25f) ? g_frame_dt : 0.016f;
        if (wake > 0.0f) { wake -= dt / 1.7f; if (wake < 0.0f) wake = 0.0f; }
        float wk = wake * wake * (3.0f - 2.0f * wake);
        float black = out ? g_down_blend : 0.0f;
        if (wk > black) black = wk;
        float tun = g_down_blend * 1.3f; if (tun > 1.0f) tun = 1.0f;
        if (!g_cfg_vignette) { black = 0.0f; tun = 0.0f; }
        int step = (int)(black * 10.0f); if (step > 9) step = 9;
        static int prev_step = -1;
        if (black > 0.03f && step != prev_step && g_imgbox_setimage) {
            unsigned char btex[32]; char nm[16];
            snprintf(nm, sizeof nm, "blk%d.png", step);
            make_mstr(btex, nm);
            g_imgbox_setimage(g_black_ov, btex);
            prev_step = step;
        }
        g_widget_setvisible(g_vignette, tun > 0.05f);
        g_widget_setvisible(g_black_ov, black > 0.03f);
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
    if (gw) fp_head_visibility(gw);       /* hide head while fast-forwarding (>1x) */
    fp_gui_update();                      /* post-frame: all MyGUI widget work */

    DWORD now = GetTickCount();
    if (KFP_DEBUG_LOG && gw && (now - g_last_tick_ms) >= 1000) {   /* 1 Hz observation log */
        g_last_tick_ms = now;
        fp_tick(gw);
    }

    ini_hot_reload();             /* apply KenshiFP.ini edits without restart */
    fps_cap_wait();               /* optional user-configured frame limiter */
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
        /* Per-frame hook not firing. DIAGNOSTIC ONLY -- do NOT re-arm: a
         * non-firing hook never starts firing on re-install (wrong address or
         * diverted call), and re-arming churned MinHook state for no gain.
         * Report a few times, then stay quiet. */
        if (rearms < 3) {
            rearms++;
            logline("watchdog: mainloop SILENT (main hb=%ld cam hb=%ld) -- cam %s",
                    (long)hm, (long)hc, cam_flow ? "FIRING" : "silent");
        }
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
/* The FP aim point: 80m along the camera look direction from the head, in game
 * coords. Shared by the ranged free-aim hooks. Returns 0 if unavailable. */
static int fp_aim_point(Vec3 *out)
{
    if (!g_player_pc || !g_get_bone_world) return 0;
    Vec3 head;
    g_get_bone_world(g_player_pc, &head, g_head_bone);       /* game coords */
    float cp = cosf(g_pitch);
    out->x = head.x + sinf(g_yaw) * cp * 80.0f;
    out->y = head.y - sinf(g_pitch)     * 80.0f;
    out->z = head.z + cosf(g_yaw) * cp * 80.0f;
    return 1;
}

/* Ranged free-aim: RangedCombatClass::animationUpdate(this, frameTime, aimpos&,
 * target) is fed the auto-target's position each frame; for the PLAYER in FP
 * mode we substitute the point the camera is looking at, so the arm pose
 * follows the crosshair. Non-player characters pass through. */
static void hooked_ranged_animupd(void *rc, float ft, Vec3 *aimpos, void *target)
{
    Vec3 aim;
    if (g_fp_mode && g_cfg_freeaim && rc
        && readable((void *)((uintptr_t)rc + RC_ME), 8)
        && *(void **)((uintptr_t)rc + RC_ME) == g_player_pc
        && readable(aimpos, sizeof(Vec3))
        && fp_aim_point(&aim)) {
        *aimpos = aim;
        if (readable((void *)((uintptr_t)rc + RC_AIMPOS), sizeof(Vec3)))
            *(Vec3 *)((uintptr_t)rc + RC_AIMPOS) = aim;
        static int logged;
        if (!logged) { logged = 1; logline("[freeaim] pose override LIVE"); }
        if (g_aim_mode) {                     /* R-aim diagnostics: is this even called? */
            static int cnt;
            if ((++cnt % 120) == 1) logline("[aim] animationUpdate running (call %d)", cnt);
        }
    }
    g_ranged_animupd_orig(rc, ft, aimpos, target);
}

/* GunClass::shoot(this, me, target, stat, aimpos&) -- the projectile spawn.
 * Direction = getAimDir(aimpos) + skill-based randomDeviant, so overriding
 * aimpos here makes the bolt fly at the crosshair while target attribution
 * and the accuracy spread stay vanilla. */
typedef void (*gun_shoot_t)(void *gun, void *me, void *target, int stat, const Vec3 *aimpos);
static gun_shoot_t g_gun_shoot_orig;
static void hooked_gun_shoot(void *gun, void *me, void *target, int stat, const Vec3 *aimpos)
{
    Vec3 aim;
    if (g_fp_mode && g_cfg_freeaim && me && me == g_player_pc && fp_aim_point(&aim)) {
        aimpos = &aim;
        static int logged;
        if (!logged) { logged = 1; logline("[freeaim] projectile override LIVE"); }
    }
    g_gun_shoot_orig(gun, me, target, stat, aimpos);
}

/* CharMovement::faceDirection hook: while the player is in RANGED combat mode
 * in FP, the combat AI's auto-face (toward its target, during aim/reload) is
 * redirected to the camera look direction -- the body tracks the crosshair and
 * orient-to-control stays in charge. Melee and everyone else pass through. */
typedef void (*face_dir_t)(void *mv, const Vec3 *dir);
static face_dir_t g_face_dir_orig;
static void hooked_face_direction(void *mv, const Vec3 *dir)
{
    if (g_fp_mode && g_cfg_freeaim && g_player_pc
        && readable((void *)((uintptr_t)g_player_pc + CHAR_MOVEMENT), 8)
        && *(void **)((uintptr_t)g_player_pc + CHAR_MOVEMENT) == mv) {
        void *rc = readable((void *)((uintptr_t)g_player_pc + CHAR_RANGEDCOMBAT), 8)
            ? *(void **)((uintptr_t)g_player_pc + CHAR_RANGEDCOMBAT) : NULL;
        if (readable((void *)((uintptr_t)rc + RC_COMBATMODE), 1)
            && *(unsigned char *)((uintptr_t)rc + RC_COMBATMODE)) {
            Vec3 look = { sinf(g_yaw), 0.0f, cosf(g_yaw) };
            static int logged;
            if (!logged) { logged = 1; logline("[freeaim] facing override LIVE"); }
            g_face_dir_orig(mv, &look);
            return;
        }
    }
    g_face_dir_orig(mv, dir);
}

/* Manual ranged trigger: on a left-click edge (FP, cursor captured, weapon
 * drawn, gun loaded) call GunClass::shoot directly at the crosshair point.
 * target=NULL is explicitly handled by shoot (verified in decomp); ammo,
 * visibility, and tracer bookkeeping all run inside the game's own code. */
static void manual_fire_update(void *pcx)
{
    static int lmb_prev;
    int lmb = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    int edge = lmb && !lmb_prev;
    lmb_prev = lmb;
    if (!edge || !g_fp_mode || g_ui_open || !pcx || !g_gun_shoot_orig) return;
    void *wih = readable((void *)((uintptr_t)pcx + CHAR_WEAPON_IN_HANDS), 8)
        ? *(void **)((uintptr_t)pcx + CHAR_WEAPON_IN_HANDS) : NULL;
    if (!wih) return;                            /* nothing drawn */
    void *rc = readable((void *)((uintptr_t)pcx + CHAR_RANGEDCOMBAT), 8)
        ? *(void **)((uintptr_t)pcx + CHAR_RANGEDCOMBAT) : NULL;
    void *gun = (rc && readable((void *)((uintptr_t)rc + RC_GUN), 8))
        ? *(void **)((uintptr_t)rc + RC_GUN) : NULL;
    if (!gun || !readable((void *)((uintptr_t)gun + GUN_AMMO), 4)) return;
    int ammo = *(int *)((uintptr_t)gun + GUN_AMMO);
    if (ammo <= 0) { logline("[fire] click: gun empty/reloading (ammo=%d)", ammo); return; }
    int stat = readable((void *)((uintptr_t)rc + RC_STAT), 4)
        ? *(int *)((uintptr_t)rc + RC_STAT) : 0;
    Vec3 aim;
    if (!fp_aim_point(&aim)) return;
    if (setjmp(g_guard_jb)) { g_guard_armed = 0; logline("[fire] shoot FAULTED"); return; }
    g_guard_armed = 1;
    g_gun_shoot_orig(gun, pcx, NULL, stat, &aim);
    g_guard_armed = 0;
    logline("[fire] manual shot (ammo %d -> %d)", ammo, ammo - 1);
}

/* sheatheWeapon suppressor: while manual aim (R) is on, the idle AI re-sheathes
 * our drawn weapon within ~20ms (the draw/sheathe flicker). Swallow the call
 * for the player during aim mode; everyone else (and normal play) unaffected. */
typedef void (*sheathe_t)(void *pc);
static sheathe_t g_sheathe_orig;
static void hooked_sheathe(void *pc)
{
    if (g_aim_mode && pc && pc == g_player_pc) {
        static int cnt;
        if ((++cnt % 60) == 1) logline("[aim] suppressed AI sheathe (x%d)", cnt);
        return;
    }
    g_sheathe_orig(pc);
}

/* CharMovement::update hook: re-assert the player's direct-drive intent
 * IMMEDIATELY BEFORE the engine consumes movement state -- combat AI (and the
 * order system) rewrite movementMode inside/around this update, and applying
 * here means our write is always the last one standing. WASD therefore works
 * in combat: the fight's locomotion suggestions lose the race every frame. */
typedef void (*charmove_update_t)(void *mv, float t);
static charmove_update_t g_charmove_update_orig;
#define MV_CHARACTER   0x3A8            /* CharMovement::character (backref) */
#define MV_ANIMOVERRIDE 0x37C           /* CharMovement::animationOverride (bool) */
#define CHAR_VISNEAR   0x1A8            /* Character::isVisibleAndNear */
#define CHAR_ONSCREEN  0x1A9            /* Character::isOnScreen */

/* Force MOVE_DIRECTION + our WASD motion onto the player's CharMovement. Called
 * both BEFORE and AFTER the original update: the original re-enables combat
 * locomotion (animationOverride + movementMode) INSIDE itself when the char is
 * combat-engaged, so a pre-override write alone loses the race every frame (log
 * showed movementMode flickering 2->1->0). Re-asserting after the original is
 * what makes WASD beat combat -- exactly what Kenshi-Direct-Control does. */
static void mv_force_direct(void *mv)
{
    if (readable((void *)((uintptr_t)mv + MV_MOVEMODE), 4))
        *(int *)((uintptr_t)mv + MV_MOVEMODE) = 2;              /* MOVE_DIRECTION */
    if (readable((void *)((uintptr_t)mv + MV_ANIMOVERRIDE), 1))
        *(unsigned char *)((uintptr_t)mv + MV_ANIMOVERRIDE) = 0;
    if (readable((void *)((uintptr_t)mv + MV_SPEEDORDERS), 4))
        *(int *)((uintptr_t)mv + MV_SPEEDORDERS) = g_dm_speed;
    ((void (*)(void *, const Vec3 *, float))(g_base + RVA_SET_DIRECT_MOVE))(mv, &g_dm_dir, 99.0f);
}

static void hooked_charmove_update(void *mv, float t)
{
    int drive = (g_dm_active && mv == g_dm_mv);
    if (KFP_DEBUG_LOG && g_fp_mode && mv == g_dm_mv) {
        static int el;
        if ((++el % 30) == 0) {
            int mode = readable((void *)((uintptr_t)mv + MV_MOVEMODE), 4)
                       ? *(int *)((uintptr_t)mv + MV_MOVEMODE) : -1;   /* mode combat left */
            logline("[enf] dm_active=%ld mode(pre-override)=%d dir=(%.2f,%.2f)",
                    g_dm_active, mode, g_dm_dir.x, g_dm_dir.z);
        }
    }
    if (drive) {
        /* Halt clears any combat chase motion the AI queued, then force our
         * direction. (halt = CharMovement vtable +0x98.) */
        void **vt = *(void ***)mv;
        if (in_module(vt)) ((void (*)(void *))vt[MV_HALT_SLOT])(mv);
        mv_force_direct(mv);
    }
    /* FP camera weld depends on a LIVE skeleton, but Kenshi culls animation
     * for characters it deems off-screen -- and in FP the camera sits inside
     * the head, so looking level/up culls your own body and freezes the head
     * bone (camera detaches). Force the visibility flags for the FP character
     * right before its update, every frame. */
    if (g_fp_mode && g_player_pc
        && readable((void *)((uintptr_t)mv + MV_CHARACTER), 8)
        && *(void **)((uintptr_t)mv + MV_CHARACTER) == g_player_pc
        && readable((void *)((uintptr_t)g_player_pc + CHAR_VISNEAR), 2)) {
        *(unsigned char *)((uintptr_t)g_player_pc + CHAR_VISNEAR)  = 1;
        *(unsigned char *)((uintptr_t)g_player_pc + CHAR_ONSCREEN) = 1;
    }
    g_charmove_update_orig(mv, t);
    /* Re-assert AFTER: the original just re-enabled combat locomotion mid-call.
     * This post-write is the one that actually wins the race. */
    if (drive) mv_force_direct(mv);
}

static int install_hook(void *target, void *detour, void **original)
{
    /* Always MinHook. The KenshiLib AddHook "cooperative" path installed but
     * the detours never fired for some RE_Kenshi users, and the watchdog's
     * MinHook re-arm on a KenshiLib-installed hook mixed backends and crashed.
     * Our source analysis shows MinHook does NOT conflict with RE_Kenshi
     * (disjoint targets, module-private state), and correct 1.0.65 addresses
     * over MinHook are the proven-working path. */
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
#ifdef KFP_RE_PLUGIN
    /* Version-independent: resolve every address by scanning the live binary. */
    {
        int r = kfp_resolve_all(g_base);
        int total = (int)(sizeof(KFP_RTAB) / sizeof(KFP_RTAB[0]));
        logline("RE plugin: resolved %d/%d addresses by signature (base %p)",
                r, total, (void *)g_base);
        g_build = 0;                       /* not a fixed build */
        if (!g_rva.MAINLOOP || !g_rva.CAM_INSTANCE || !g_rva.CAM_UPDATE) {
            g_wrong_build = 1;             /* core resolution failed -> disable */
            logline("*** core address resolution FAILED (mainloop=%p cam=%p camupd=%p)",
                    (void *)g_rva.MAINLOOP, (void *)g_rva.CAM_INSTANCE, (void *)g_rva.CAM_UPDATE);
        }
    }
#else
    {
        static const unsigned char SIG_MAINLOOP[8] =
            { 0x48,0x8b,0xc4,0x56,0x57,0x41,0x54,0x48 };
        unsigned char *p68 = (unsigned char *)(g_base + T_1068.MAINLOOP);
        unsigned char *p65 = (unsigned char *)(g_base + T_1065.MAINLOOP);
        if (readable(p68, 8) && memcmp(p68, SIG_MAINLOOP, 8) == 0) { g_rva = T_1068; g_build = 68; }
        else if (readable(p65, 8) && memcmp(p65, SIG_MAINLOOP, 8) == 0) { g_rva = T_1065; g_build = 65; }
        else g_wrong_build = 1;
    }
#endif

    g_follow_object  = (follow_object_t)(g_base + RVA_FOLLOW_OBJECT);
    g_stop_following = (stop_follow_t)(g_base + RVA_STOP_FOLLOW);
    g_charmove_setdest = (charmove_setdest_t)(g_base + RVA_CHARMOVE_SETDEST);
    g_char_setdest     = (char_setdest_t)(g_base + RVA_CHAR_SETDEST);
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
    /* Spine/neck chain for procedural aim-pitch (Biped naming, all SSO). */
    make_mstr(g_bone_spine1, "Bip01 Spine1");
    make_mstr(g_bone_spine2, "Bip01 Spine2");
    make_mstr(g_bone_neck,   "Bip01 Neck");
    make_mstr(g_bone_rootspine, "Bip01 Spine");   /* readiness computed after Ogre resolves */
    logline("KenshiFP v0.3.0 loaded (FP + head-bone + FOV + WASD); module base %p", (void *)g_base);

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
        g_node_get_pos  = (node_get_pos_t)GetProcAddress(ogre, OGRE_GETPOS_SYM);
        g_disable_bone  = (disable_bone_t)GetProcAddress(ogre, OGRE_DISABLEBONE_SYM);
        g_skel_getbone  = (skel_getbone_t)GetProcAddress(ogre, OGRE_GETBONE_SYM);
        g_oldnode_getdori = (oldnode_getdori_t)GetProcAddress(ogre, OGRE_OLDNODE_GETDORI_SYM);
        g_oldnode_getdpos = (oldnode_getdpos_t)GetProcAddress(ogre, OGRE_OLDNODE_GETDPOS_SYM);
        g_node_getdpos_upd = (node_getdpos_upd_t)GetProcAddress(ogre, OGRE_GETDPOS_UPD_SYM);
        g_oldnode_getori  = (oldnode_getori_t)GetProcAddress(ogre, OGRE_OLDNODE_GETORI_SYM);
        g_oldnode_setori  = (oldnode_setori_t)GetProcAddress(ogre, OGRE_OLDNODE_SETORI_SYM);
        g_oldnode_needupd = (oldnode_needupd_t)GetProcAddress(ogre, OGRE_OLDNODE_NEEDUPD_SYM);
        g_oldbone_setmanual = (oldbone_setmanual_t)GetProcAddress(ogre, OGRE_OLDBONE_SETMANUAL_SYM);
        g_get_parent_scenenode = (get_parent_scenenode_t)GetProcAddress(ogre, OGRE_GETPARENTSCENENODE_SYM);
        g_entity_getskel = (entity_getskel_t)GetProcAddress(ogre, OGRE_ENTITY_GETSKEL_SYM);
        if (!g_entity_getskel)
            g_entity_getskel = (entity_getskel_t)GetProcAddress(ogre, OGRE_ENTITY_GETSKEL_SYM2);
        g_cam_set_fovy  = (cam_set_fovy_t)GetProcAddress(ogre, OGRE_SETFOVY_SYM);
        g_cam_get_fovy  = (cam_get_fovy_t)GetProcAddress(ogre, OGRE_GETFOVY_SYM);
        g_cam_set_nearclip = (cam_set_nearclip_t)GetProcAddress(ogre, OGRE_SETNEARCLIP_SYM);
        g_cam_get_nearclip = (cam_get_nearclip_t)GetProcAddress(ogre, OGRE_GETNEARCLIP_SYM);
        g_ogre_ready = (g_node_set_dori && g_node_set_dpos && g_node_get_dpos);
        g_spine_ready = (g_oldnode_getori && g_oldnode_setori &&
                         g_oldnode_needupd && g_skel_getbone) ? 1 : 0;
        logline("Ogre FOV setters: set=%p get=%p", (void *)g_cam_set_fovy, (void *)g_cam_get_fovy);
        logline("spine-bend: getori=%p setori=%p needupd=%p -> %s",
                (void *)g_oldnode_getori, (void *)g_oldnode_setori,
                (void *)g_oldnode_needupd, g_spine_ready ? "ARMED" : "unavailable");
    }
    logline(g_ogre_ready ? "Ogre node setters resolved (FP override armed)"
                         : "WARN: Ogre node setters NOT resolved (FP override disabled)");

    MH_STATUS mh = g_wrong_build ? MH_ERROR_NOT_INITIALIZED : MH_Initialize();
    int ok = 0;
    if (!g_wrong_build && (mh == MH_OK || mh == MH_ERROR_ALREADY_INITIALIZED)) {
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

        /* Ranged free-aim hook (aim where the FP camera looks). */
        {
            void *ra = (void *)(g_base + RVA_RANGED_ANIMUPD);
            int raok = install_hook(ra, (void *)hooked_ranged_animupd,
                                    (void **)&g_ranged_animupd_orig);
            logline(raok ? "ranged free-aim hook installed (animationUpdate)"
                         : "ranged free-aim hook FAILED");
            void *gs = (void *)(g_base + RVA_GUN_SHOOT);
            int gsok = install_hook(gs, (void *)hooked_gun_shoot,
                                    (void **)&g_gun_shoot_orig);
            logline(gsok ? "projectile aim hook installed (GunClass::shoot)"
                         : "projectile aim hook FAILED");
            void *fd = (void *)(g_base + RVA_FACE_DIR);
            int fdok = install_hook(fd, (void *)hooked_face_direction,
                                    (void **)&g_face_dir_orig);
            logline(fdok ? "facing hook installed (CharMovement::faceDirection)"
                         : "facing hook FAILED");
            void *sh = (void *)(g_base + RVA_SHEATHE);
            int shok = install_hook(sh, (void *)hooked_sheathe,
                                    (void **)&g_sheathe_orig);
            logline(shok ? "sheathe suppressor installed (manual aim hold)"
                         : "sheathe suppressor FAILED");
            void *cu2 = (void *)(g_base + RVA_CHARMOVE_UPDATE);
            int cuok = install_hook(cu2, (void *)hooked_charmove_update,
                                    (void **)&g_charmove_update_orig);
            logline(cuok ? "movement update hook installed (WASD wins combat race)"
                         : "movement update hook FAILED");
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
            g_gui_getsubmain   = (gui_getsubmain_t)GetProcAddress(mygui, MYGUI_GETSUBMAIN_SYM);
            g_gui_subsetcolour = (gui_subsetcolour_t)GetProcAddress(mygui, MYGUI_SUBSETCOLOUR_SYM);
            g_gui_findwidget     = (gui_findwidget_t)GetProcAddress(mygui, MYGUI_GUI_FINDWIDGET_SYM);
            g_widget_createwidget= (widget_createwidget_t)GetProcAddress(mygui, MYGUI_WIDGET_CREATEWIDGET_SYM);
            g_tabctrl_itemcount  = (tabctrl_itemcount_t)GetProcAddress(mygui, MYGUI_TABCTRL_ITEMCOUNT_SYM);
            g_tabctrl_itemat     = (tabctrl_itemat_t)GetProcAddress(mygui, MYGUI_TABCTRL_ITEMAT_SYM);
            g_input_getinst      = (input_getinst_t)GetProcAddress(mygui, MYGUI_INPUT_GETINST_SYM);
            g_mousefocus         = (mousefocus_t)GetProcAddress(mygui, MYGUI_MOUSEFOCUS_SYM);
            g_widget_getparent   = (getparent_t)GetProcAddress(mygui, MYGUI_GETPARENT_SYM);
            g_scroll_setrange    = (scroll_setrange_t)GetProcAddress(mygui, MYGUI_SCROLL_SETRANGE_SYM);
            g_scroll_setpos      = (scroll_setpos_t)GetProcAddress(mygui, MYGUI_SCROLL_SETPOS_SYM);
            g_scroll_getpos      = (scroll_getpos_t)GetProcAddress(mygui, MYGUI_SCROLL_GETPOS_SYM);
            g_btn_getsel         = (btn_getsel_t)GetProcAddress(mygui, MYGUI_BTN_GETSEL_SYM);
            g_btn_setsel         = (btn_setsel_t)GetProcAddress(mygui, MYGUI_BTN_SETSEL_SYM);
            g_textbox_setcap     = (textbox_setcap_t)GetProcAddress(mygui, MYGUI_TEXTBOX_SETCAP_SYM);
            g_window_setcap      = (textbox_setcap_t)GetProcAddress(mygui, MYGUI_WINDOW_SETCAP_SYM);
            g_ustring_ctor       = (ustring_ctor_t)GetProcAddress(mygui, MYGUI_USTRING_CTOR_SYM);
            g_ustring_dtor       = (ustring_dtor_t)GetProcAddress(mygui, MYGUI_USTRING_DTOR_SYM);
            g_gui_getenum        = (getenum_t)GetProcAddress(mygui, MYGUI_GUI_GETENUM_SYM);
            g_widget_getenum     = (getenum_t)GetProcAddress(mygui, MYGUI_WIDGET_GETENUM_SYM);
            g_widget_getname     = (widget_getname_t)GetProcAddress(mygui, MYGUI_WIDGET_GETNAME_SYM);
            if (KFP_DEBUG_LOG)
                logline("[settings] syms: wcreate=%p focus(inst=%p,get=%p,parent=%p) scroll(set=%p,get=%p) btn(get=%p,set=%p) cap=%p wcap=%p",
                    (void*)g_widget_createwidget,(void*)g_input_getinst,(void*)g_mousefocus,(void*)g_widget_getparent,
                    (void*)g_scroll_setpos,(void*)g_scroll_getpos,(void*)g_btn_getsel,(void*)g_btn_setsel,
                    (void*)g_textbox_setcap,(void*)g_window_setcap);
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

    load_ini();                    /* optional KenshiFP.ini (fps_cap=N, ...) */
    logline(ok ? "KenshiFP active: RIGHT ALT = first-person toggle; mouse = look; WASD = move; wheel = speed"
               : (g_wrong_build ? "KenshiFP inactive (unsupported game build)"
                                : "KenshiFP FAILED to install per-frame hook"));
}

__declspec(dllexport) void dllStopPlugin(void) { logline("dllStopPlugin"); }

#ifdef KFP_RE_PLUGIN
/* RE_Kenshi entry point. It LoadLibrary's this DLL and calls startPlugin()
 * (exported as ?startPlugin@@YAXXZ via plugin.def). Same init as the Ogre
 * dllStartPlugin, but g_rva is resolved by signature (see the KFP_RE_PLUGIN
 * branch above). */
void startPlugin(void) { dllStartPlugin(); }
#endif

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) { g_hinst = h; DisableThreadLibraryCalls(h); }
    return TRUE;
}
