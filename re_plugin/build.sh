#!/bin/sh
# Build KenshiFP.dll -- the RE_Kenshi plugin (Path B): the FULL standalone
# client compiled with -DKFP_RE_PLUGIN, so g_rva is resolved by runtime
# signature scan (version-independent) and the entry point is startPlugin()
# (exported as ?startPlugin@@YAXXZ via plugin.def).
#
# NO MinHook: the RE edition installs its hooks through KenshiLib::AddHook
# (RE_Kenshi's own hooking service), so this DLL contains NO inline-hook /
# trampoline / executable-alloc machinery. That keeps its static profile clean
# for antivirus (the "BehavesLike.Injector" / Wacatac ML heuristics score on
# exactly that machinery) -- the actual code patching happens inside
# KenshiLib.dll, which RE_Kenshi ships and manages.
set -e
cd "$(dirname "$0")"
SRC=../client/kenshifp_client.c
# version metadata (legitimacy signal for AV ML) -> COFF object
x86_64-w64-mingw32-windres -DKFP_RE_BUILD ../client/kenshifp.rc -O coff -o kenshifp_res.o
x86_64-w64-mingw32-gcc -O2 -shared -static-libgcc -DKFP_RE_PLUGIN -o KenshiFP.dll \
    "$SRC" kenshifp_res.o plugin.def \
    -lkernel32 -ldinput8 -ldxguid -lwinmm -lm
echo "built KenshiFP.dll (RE_Kenshi plugin, KenshiLib::AddHook, no MinHook)"
x86_64-w64-mingw32-objdump -p KenshiFP.dll | grep -i "startPlugin" | head -1
