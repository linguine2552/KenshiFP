/*
 * KenshiFP client DLL — first-person mode for Kenshi 1.0.68 "Newland" x64.
 *
 * STAGE 0 (this file): read-only anchor validation. Installs the proven
 * per-frame MinHook trampoline on GameWorld::mainLoop_GPUSensitiveStuff, and
 * each frame (throttled) logs the pieces the camera-lock + WASD work depends
 * on, WITHOUT writing any game state yet:
 *   - GameWorld* (the hook's `this`) and its frameSpeedMult/player fields,
 *   - the camera-holder global DAT_142133308 and the Ogre::Camera* at +0x58,
 *   - the first player character and its world position (getPosition slot 8),
 *   - live WASD + toggle-key state via Win32 GetAsyncKeyState.
 * This proves the read path on the main thread before we start driving the
 * camera (M1) and issuing movement orders (M3). Same method as KenshiMP:
 * headless-Ghidra offsets -> mingw DLL -> Ogre plugin-path load -> in-game log.
 *
 * Loaded via Plugins_x64.cfg (Plugin=KenshiFP_x64), no RE_Kenshi.
 * All offsets: see ../re/NOTES.md. Field offsets from KenshiLib headers are
 * treated as provisional until this DLL's log confirms them in-game.
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "MinHook.h"

/* ---- per-frame hook (proven in KenshiMP, same binary) ---- */
#define RVA_MAINLOOP      0x788a00u  /* GameWorld::mainLoop_GPUSensitiveStuff(GameWorld*, float) */
#define GW_FRAMESPEED_OFF 0x700      /* GameWorld::frameSpeedMult (float) */
#define GW_PLAYER         0x580      /* GameWorld::player (PlayerInterface*) */

/* ---- camera (M0 finding, re/NOTES.md) ----
 * DAT_142133308 is the engine's global camera-holder pointer: every frame
 * mainLoop calls Ogre::Camera::getViewMatrix(*(Camera**)(DAT_142133308+0x58)).
 * So +0x58 is the live Ogre::Camera*. Provisionally the global CameraClass*. */
#define RVA_CAM_GLOBAL    0x2133308u
#define CAM_OGRE_CAMERA   0x58       /* Ogre::Camera* used for the view matrix */
/* CameraClass field offsets (KenshiLib header, provisional until logged): */
#define CC_YAW            0x18
#define CC_PITCH          0x1C
#define CC_FOLLOW_HAND    0x28       /* hand objectCurrentlyFollowing */
#define CC_ALTITUDE       0x60
#define CC_FREECAM        0xBF

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
typedef Vec3 *(*get_position_t)(void *self, Vec3 *out);
typedef void (*mainloop_t)(void *gw, float time);

static FILE *g_log;
static uintptr_t g_base;
static mainloop_t g_mainloop_orig;
static DWORD g_last_tick_ms;
static int g_fp_mode;              /* toggled by VK_TOGGLE_FP edge */
static int g_toggle_was_down;

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
    return readable(c, 0x60) ? c : NULL;
}

/* Character::getPosition via live vtable slot 8 (proven ABI). */
static int char_position(void *c, Vec3 *out)
{
    if (!readable(c, 8)) return 0;
    void **vt = *(void ***)c;
    if (!readable(vt, (GETPOS_VTABLE_SLOT + 1) * 8)) return 0;
    get_position_t getpos = (get_position_t)vt[GETPOS_VTABLE_SLOT];
    if (!readable((void *)getpos, 1)) return 0;
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
    /* camera-holder global */
    void *cam_holder = *(void **)(g_base + RVA_CAM_GLOBAL);
    void *ogre_cam = NULL;
    float yaw = 0, pitch = 0, alt = 0;
    unsigned char freecam = 0;
    if (readable(cam_holder, CC_FREECAM + 1)) {
        ogre_cam = *(void **)((uintptr_t)cam_holder + CAM_OGRE_CAMERA);
        yaw      = *(float *)((uintptr_t)cam_holder + CC_YAW);
        pitch    = *(float *)((uintptr_t)cam_holder + CC_PITCH);
        alt      = *(float *)((uintptr_t)cam_holder + CC_ALTITUDE);
        freecam  = *(unsigned char *)((uintptr_t)cam_holder + CC_FREECAM);
    }

    float speedmult = readable(gw, GW_FRAMESPEED_OFF + 4)
        ? *(float *)((uintptr_t)gw + GW_FRAMESPEED_OFF) : -1.0f;

    void *pc = first_player_char(gw);
    Vec3 pos = {0,0,0};
    int havepos = pc && char_position(pc, &pos);

    int w = (GetAsyncKeyState(VK_W) & 0x8000) != 0;
    int a = (GetAsyncKeyState(VK_A) & 0x8000) != 0;
    int s = (GetAsyncKeyState(VK_S) & 0x8000) != 0;
    int d = (GetAsyncKeyState(VK_D) & 0x8000) != 0;

    logline("[tick] fp=%d gw=%p speedMult=%.3f | camHolder=%p ogreCam=%p yaw=%.3f pitch=%.3f alt=%.1f freecam=%u | pc=%p pos=%s(%.1f,%.1f,%.1f) | WASD=%d%d%d%d",
            g_fp_mode, gw, speedmult,
            cam_holder, ogre_cam, yaw, pitch, alt, (unsigned)freecam,
            pc, havepos ? "" : "?", pos.x, pos.y, pos.z, w, a, s, d);
}

static void hooked_mainloop(void *gw, float time)
{
    g_mainloop_orig(gw, time);     /* run the game's frame first */

    poll_input();                  /* every frame: catch toggle edges */

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
    logline("KenshiFP stage-0 loaded; module base %p", (void *)g_base);

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

    logline(ok ? "KenshiFP active: press F in-game to toggle FP intent; watch [tick] lines"
               : "KenshiFP FAILED to install per-frame hook");
}

__declspec(dllexport) void dllStopPlugin(void) { logline("dllStopPlugin"); }

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID reserved)
{
    (void)h; (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(h);
    return TRUE;
}
