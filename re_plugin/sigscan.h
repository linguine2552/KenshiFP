/* Runtime signature scanning -- the heart of the RE_Kenshi (Path B) build.
 * Instead of hard-coded per-version RVA tables, every game address is resolved
 * by scanning the live binary's .text for a byte pattern. This is inherently
 * version-independent: the SAME pattern matches whatever Kenshi build RE_Kenshi
 * is running (Steam 1.0.68 / 1.0.65 downgrade / GOG / older), with no prior
 * knowledge of that binary. Patterns use "48 8B ?? C4" text, ?? = wildcard. */
#ifndef KFP_SIGSCAN_H
#define KFP_SIGSCAN_H

#include <windows.h>
#include <stdint.h>
#include <stddef.h>

/* Locate the main executable's .text region (works for any exe name -- GOG or
 * Steam -- because the process main module is always the game exe). */
static int kfp_text_region(uintptr_t *out_base, size_t *out_size)
{
    HMODULE mod = GetModuleHandleA(NULL);          /* the running game exe */
    if (!mod) return 0;
    uintptr_t b = (uintptr_t)mod;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)b;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(b + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
    IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        if (memcmp(sec[i].Name, ".text", 5) == 0) {
            *out_base = b + sec[i].VirtualAddress;
            *out_size = sec[i].Misc.VirtualSize;
            return 1;
        }
    }
    return 0;
}

/* Parse "48 8B ?? C4" into bytes[] + mask[] (mask 1 = compare, 0 = wildcard).
 * Returns pattern length, or 0 on parse error. */
static size_t kfp_parse_pat(const char *pat, unsigned char *bytes, unsigned char *mask, size_t cap)
{
    size_t n = 0;
    while (*pat && n < cap) {
        while (*pat == ' ') pat++;
        if (!*pat) break;
        if (pat[0] == '?') {
            bytes[n] = 0; mask[n] = 0; n++;
            pat++; if (*pat == '?') pat++;
        } else {
            int hi = pat[0], lo = pat[1];
            #define HEXV(c) ((c)>='0'&&(c)<='9'?(c)-'0':((c)|0x20)>='a'&&((c)|0x20)<='f'?((c)|0x20)-'a'+10:-1)
            int h = HEXV(hi), l = HEXV(lo);
            #undef HEXV
            if (h < 0 || l < 0) return 0;
            bytes[n] = (unsigned char)((h << 4) | l); mask[n] = 1; n++;
            pat += 2;
        }
    }
    return n;
}

/* Scan the game .text for a pattern; return the address of the `instance`-th
 * match (0-based), or 0 if not found. */
static uintptr_t kfp_scan(const char *pat, int instance)
{
    uintptr_t base; size_t size;
    if (!kfp_text_region(&base, &size)) return 0;
    unsigned char pb[256], pm[256];
    size_t plen = kfp_parse_pat(pat, pb, pm, sizeof pb);
    if (!plen || plen > size) return 0;
    const unsigned char *d = (const unsigned char *)base;
    int found = 0;
    for (size_t i = 0; i + plen <= size; i++) {
        size_t j = 0;
        for (; j < plen; j++) if (pm[j] && d[i + j] != pb[j]) break;
        if (j == plen) { if (found++ == instance) return base + i; }
    }
    return 0;
}

/* Convenience: first (unique) match. */
static uintptr_t kfp_sig(const char *pat) { return kfp_scan(pat, 0); }

#endif
