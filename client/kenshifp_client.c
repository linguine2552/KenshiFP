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
#define RVA_CHARMOVE_SETDEST 0x661270u   /* CharMovement::setDestination (halt use only) */
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

/* ---- FOV ----
 * Ogre::Frustum::setFOVy(const Radian&) [Camera inherits it]; Radian == {float}.
 * getFOVy returns a const ref (pointer in RAX -> safe ABI, unlike the by-value
 * get* accessors). We capture the default FOV once, force FP_FOV while in first
 * person, and restore on exit. */
#define OGRE_SETFOVY_SYM  "?setFOVy@Frustum@Ogre@@UEAAXAEBVRadian@2@@Z"
#define OGRE_GETFOVY_SYM  "?getFOVy@Frustum@Ogre@@UEBAAEBVRadian@2@XZ"
#define FP_FOV_DEG        70.0f    /* vertical FOV while first-person; tune */
#define DEG2RAD           0.01745329f

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
#define VK_TOGGLE_FP      0x46       /* 'F' — enter/leave FP intent */
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
typedef void (*charmove_setdest_t)(void *mv, const Vec3 *dest, int pri, char shift);
typedef void (*face_direction_t)(void *mv, const Vec3 *dir);
/* _setPositionAndTeleport(const Vec3& p, int floor): (this RCX, Vec3* RDX, floor R8) */
typedef void (*set_pos_tele_t)(void *mv, const Vec3 *pos, int floor);
typedef void (*node_set_dpos_t)(void *node, const Vec3 *v);   /* world _setDerivedPosition */
typedef Vec3 *(*node_get_dpos_t)(void *node, Vec3 *ret);      /* world _getDerivedPosition (this=RCX,ret=RDX) */
/* member-struct-return: this=RCX, retbuf=RDX, name=R8 */
typedef Vec3 *(*get_bone_world_t)(void *character, Vec3 *ret, const void *name);

static FILE *g_log;
static uintptr_t g_base;
static mainloop_t g_mainloop_orig;
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
static int g_toggle_was_down;
static int g_prev_fp;             /* g_fp_mode from last frame (camera_lock edge) */
static int g_ovr_prev;            /* was the FP node override active last frame */
static float g_yaw, g_pitch;      /* accumulated mouse-look angles (radians) */
static cam_set_fovy_t g_cam_set_fovy;
static cam_get_fovy_t g_cam_get_fovy;
static int g_fov_saved;
static float g_fov_default;
static charmove_setdest_t g_charmove_setdest;  /* CharMovement::setDestination (halt only) */
static DWORD g_last_move_ms;
static int g_was_moving;
static float g_last_move_dir;      /* heading of last issued destination (radians) */
static int g_last_move_keys;       /* WASD bitmask of last issued destination */
static float g_move_tx, g_move_tz, g_move_dist;  /* last issued target + its distance */
static float g_dbg_center_y, g_dbg_eye_y;  /* diagnostics for eye-height tuning */
static float g_dbg_head_x, g_dbg_center_x;  /* frame check: head vs center X */
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

static void poll_input(void)
{
    int down = (GetAsyncKeyState(VK_TOGGLE_FP) & 0x8000) != 0;
    if (down && !g_toggle_was_down) {
        g_fp_mode = !g_fp_mode;
        logline("[input] FP mode toggled -> %s", g_fp_mode ? "ON" : "OFF");
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

    logline("[tick] fp=%d pos=(%.1f,%.1f,%.1f) posX=%.1f headX=%.1f centerX=%.1f eyeY=%.1f look(yaw=%.2f pitch=%.2f) | WASD=%d%d%d%d",
            g_fp_mode, pos.x, pos.y, pos.z,
            pos.x, g_dbg_head_x, g_dbg_center_x, g_dbg_eye_y, g_yaw, g_pitch, w, a, s, d);
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
        int cx = GetSystemMetrics(SM_CXSCREEN) / 2;
        int cy = GetSystemMetrics(SM_CYSCREEN) / 2;
        if (cx <= 0) cx = 960;
        if (cy <= 0) cy = 540;

        POINT cur; GetCursorPos(&cur);
        if (!g_ovr_prev) {
            /* FP enter: zero the look and capture the cursor (hide + center). */
            g_yaw = 0.0f; g_pitch = 0.0f;
            while (ShowCursor(FALSE) >= 0) { }
        } else {
            /* relative mouse-look: accumulate delta from screen center */
            g_yaw   -= (float)(cur.x - cx) * LOOK_SENS;   /* mouse right -> look right */
            g_pitch += (float)(cur.y - cy) * LOOK_SENS;   /* mouse down  -> look down  */
            if (g_pitch >  1.4f) g_pitch =  1.4f;
            if (g_pitch < -1.4f) g_pitch = -1.4f;
        }
        SetCursorPos(cx, cy);  /* recenter so the cursor never drifts to edges */

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
                        if (!g_was_moving) {
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
            g_dbg_center_y = centerW.y; g_dbg_eye_y = eyeW.y;   /* for tuning */
            g_node_set_dpos(node, &eyeW);
        }

        /* World look orientation q = q_yaw(Y) * q_pitch(X); no roll. Ogre camera
         * looks down -Z at identity. DERIVED (world) with normal inheritance. */
        float qy = cosf(g_yaw * 0.5f),   sqy = sinf(g_yaw * 0.5f);
        float qp = cosf(g_pitch * 0.5f), sqp = sinf(g_pitch * 0.5f);
        Quat q = { qy * qp, qy * sqp, sqy * qp, -sqy * sqp };
        g_node_set_dori(node, &q);

        /* FOV: capture default once, then force the FP FOV each frame. */
        void *ogre_cam = *(void **)((uintptr_t)cam + CC_CAMERA);
        if (g_cam_set_fovy && readable(ogre_cam, 8)) {
            if (!g_fov_saved && g_cam_get_fovy) {
                const float *d = g_cam_get_fovy(ogre_cam);
                if (readable((void *)d, 4)) { g_fov_default = *d; g_fov_saved = 1; }
            }
            float rad = FP_FOV_DEG * DEG2RAD;
            g_cam_set_fovy(ogre_cam, &rad);
        }
    } else if (g_ovr_prev) {
        /* FP exit: release the cursor, restore FOV, and level the node
         * orientation to identity so update()'s incremental rotations no longer
         * drift off our FP angle (position is left for update() to reset). */
        while (ShowCursor(TRUE) < 0) { }
        void *ogre_cam = *(void **)((uintptr_t)cam + CC_CAMERA);
        if (g_fov_saved && g_cam_set_fovy && readable(ogre_cam, 8)) {
            g_cam_set_fovy(ogre_cam, &g_fov_default);
            g_fov_saved = 0;
        }
        Quat ident = { 1.0f, 0.0f, 0.0f, 0.0f };
        g_node_set_ori(node, &ident);
    }
    g_ovr_prev = active;
}

/* M3: drive the followed character with WASD relative to where we're looking.
 * Breadcrumb CharMovement::setDestination a few metres ahead at ~10 Hz; halt at
 * the current position when all keys release. */
static void fp_movement(void *gw, float dt)
{
    if (!g_fp_mode || !g_charmove_setdest) return;
    void *pc = first_player_char(gw);
    if (!pc) return;
    void *mv = readable((void *)((uintptr_t)pc + CHAR_MOVEMENT), 8)
        ? *(void **)((uintptr_t)pc + CHAR_MOVEMENT) : NULL;
    if (!readable(mv, 8)) return;

    int w = (GetAsyncKeyState(VK_W) & 0x8000) != 0;
    int s = (GetAsyncKeyState(VK_S) & 0x8000) != 0;
    int a = (GetAsyncKeyState(VK_A) & 0x8000) != 0;
    int d = (GetAsyncKeyState(VK_D) & 0x8000) != 0;
    int keys = w | (s << 1) | (a << 2) | (d << 3);
    float mf = (float)(w - s);     /* forward/back */
    float mr = (float)(d - a);     /* left/right (turns the char, not strafe) */

    (void)dt;
    if (keys == 0 || (mf == 0.0f && mr == 0.0f)) {
        if (g_was_moving) {         /* release: halt at current position */
            Vec3 here;
            if (char_position(pc, &here))
                g_charmove_setdest(mv, &here, UPDATE_PRIORITY_HIGH, 0);
            g_was_moving = 0;
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

    /* Look pitch -> target distance (down = near/slow, up = far/run). The order
     * system walks the character toward the point — it animates and turns to
     * face it naturally, and the camera (calibrated T) tracks it lag-free. */
    float t = (1.4f - g_pitch) / 2.8f;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    float dist = MOVE_NEAR + t * (MOVE_FAR - MOVE_NEAR);

    Vec3 here;
    if (!char_position(pc, &here)) return;

    /* Re-issue the destination EVERY frame: the low-level CharMovement order does
     * not "hold" like a right-click — the AI cancels it, so it walks a single
     * tick then stops unless we keep re-asserting it. Target sits `dist` ahead in
     * the look/WASD direction, so the character walks continuously toward it. */
    Vec3 tgt = { here.x + dx * dist, here.y, here.z + dz * dist };
    g_charmove_setdest(mv, &tgt, UPDATE_PRIORITY_HIGH, 0);
    g_was_moving = 1;
}

static void hooked_mainloop(void *gw, float time)
{
    g_mainloop_orig(gw, time);     /* run the game's frame first */

    poll_input();                  /* every frame: catch toggle edges */
    if (gw) camera_lock(gw);       /* every frame: assert/release the lock */
    if (gw) fp_camera_override(gw);/* every frame: force FP eye + mouse-look */
    if (gw) fp_movement(gw, time); /* every frame: WASD -> custom motion drive */

    DWORD now = GetTickCount();
    if (gw && (now - g_last_tick_ms) >= 1000) {   /* 1 Hz observation log */
        g_last_tick_ms = now;
        fp_tick(gw);
    }
}

__declspec(dllexport) void dllStartPlugin(void)
{
    g_log = fopen("KenshiFP.log", "w");
    g_base = (uintptr_t)GetModuleHandleA(NULL);
    g_follow_object  = (follow_object_t)(g_base + RVA_FOLLOW_OBJECT);
    g_stop_following = (stop_follow_t)(g_base + RVA_STOP_FOLLOW);
    g_charmove_setdest = (charmove_setdest_t)(g_base + RVA_CHARMOVE_SETDEST);
    g_get_bone_world   = (get_bone_world_t)(g_base + RVA_GET_BONE_WORLD);
    /* MSVC std::string SSO for the head bone name (size<=15 -> inline buffer). */
    memset(g_head_bone, 0, sizeof g_head_bone);
    memcpy(g_head_bone, HEAD_BONE_NAME, sizeof(HEAD_BONE_NAME) - 1);
    *(size_t *)(g_head_bone + 0x10) = sizeof(HEAD_BONE_NAME) - 1;  /* size */
    *(size_t *)(g_head_bone + 0x18) = 15;                          /* capacity */
    logline("KenshiFP loaded (FP + head-bone + FOV + WASD); module base %p", (void *)g_base);

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
        g_ogre_ready = (g_node_set_dori && g_node_set_dpos && g_node_get_dpos);
        logline("Ogre FOV setters: set=%p get=%p", (void *)g_cam_set_fovy, (void *)g_cam_get_fovy);
    }
    logline(g_ogre_ready ? "Ogre node setters resolved (FP override armed)"
                         : "WARN: Ogre node setters NOT resolved (FP override disabled)");

    MH_STATUS mh = MH_Initialize();
    int ok = 0;
    if (mh == MH_OK || mh == MH_ERROR_ALREADY_INITIALIZED) {
        void *target = (void *)(g_base + RVA_MAINLOOP);
        mh = MH_CreateHook(target, (void *)hooked_mainloop, (void **)&g_mainloop_orig);
        if (mh == MH_OK) mh = MH_EnableHook(target);
        ok = (mh == MH_OK);
        logline(ok ? "per-frame hook installed (mainLoop_GPUSensitiveStuff)"
                   : "per-frame hook FAILED (MH_STATUS %d)", (int)mh);
    } else {
        logline("MH_Initialize failed (MH_STATUS %d)", (int)mh);
    }

    logline(ok ? "KenshiFP active: F = first-person; mouse = look; WASD = move"
               : "KenshiFP FAILED to install per-frame hook");
}

__declspec(dllexport) void dllStopPlugin(void) { logline("dllStopPlugin"); }

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID reserved)
{
    (void)h; (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(h);
    return TRUE;
}
