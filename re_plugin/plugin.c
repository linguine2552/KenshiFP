/* KenshiFP -- RE_Kenshi plugin build (Path B: mingw + runtime signature scan).
 *
 * RE_Kenshi loads any DLL listed in a mod's RE_Kenshi.json ("Plugins":[...])
 * and calls its exported startPlugin() -- MSVC-mangled ?startPlugin@@YAXXZ,
 * which we export via plugin.def. This is the SKELETON: it proves the plugin
 * pipeline (RE_Kenshi -> our mingw DLL -> startPlugin) and runtime signature
 * resolution on the live binary, before the full feature port. */
#include <windows.h>
#include <stdio.h>
#include "sigscan.h"

static void kfp_log(const char *fmt, ...)
{
    FILE *f = fopen("KenshiFP_RE.log", "a");   /* CWD = game dir */
    if (!f) return;
    SYSTEMTIME t; GetLocalTime(&t);
    fprintf(f, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list ap; va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

/* Cross-version anchor: GameWorld::mainLoop_GPUSensitiveStuff prologue.
 * Byte-identical on Steam 1.0.68 and the 1.0.65 downgrade; the intent is that
 * it (or a wildcarded variant) also matches GOG. Unique in .text. */
#define SIG_MAINLOOP "48 8B C4 56 57 41 54 48 83 EC 70 48 C7 44 24 20 FE FF FF FF 48 89 58 10"

/* Exported (as C `startPlugin`, aliased to ?startPlugin@@YAXXZ in plugin.def). */
void startPlugin(void)
{
    kfp_log("KenshiFP RE plugin: startPlugin() called (RE_Kenshi loaded us)");

    uintptr_t base; size_t size;
    if (kfp_text_region(&base, &size))
        kfp_log("game exe .text: base=%p size=0x%zx", (void *)base, size);
    else { kfp_log("FATAL: could not locate game .text region"); return; }

    uintptr_t mainloop = kfp_sig(SIG_MAINLOOP);
    if (mainloop) {
        kfp_log("mainLoop resolved by signature: %p (rva 0x%zx) -- runtime scan WORKS",
                (void *)mainloop, (size_t)(mainloop - (uintptr_t)GetModuleHandleA(NULL)));
    } else {
        kfp_log("mainLoop signature NOT found -- this build needs a wildcarded/GOG sig");
    }

    kfp_log("skeleton OK; ready to port the full feature set to signature resolution");
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID r)
{
    (void)h; (void)r;
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(h);
    return TRUE;
}
