#!/bin/sh
# Build KenshiFP.dll -- the RE_Kenshi plugin (Path B): the FULL standalone
# client compiled with -DKFP_RE_PLUGIN, so g_rva is resolved by runtime
# signature scan (version-independent) and the entry point is startPlugin()
# (exported as ?startPlugin@@YAXXZ via plugin.def). Same MinHook as standalone.
set -e
cd "$(dirname "$0")"
SRC=../client/kenshifp_client.c
MH=../third_party/minhook
[ -d "$MH" ] || MH=../../KenshiMP/reference/minhook
x86_64-w64-mingw32-gcc -O2 -shared -static-libgcc -DKFP_RE_PLUGIN -o KenshiFP.dll \
    "$SRC" plugin.def \
    "$MH"/src/hook.c "$MH"/src/buffer.c "$MH"/src/trampoline.c "$MH"/src/hde/hde64.c \
    -I "$MH"/include -I "$MH"/src \
    -DMINHOOK_BUILD -lkernel32 -ldinput8 -ldxguid -lwinmm -lm
echo "built KenshiFP.dll (RE_Kenshi plugin)"
x86_64-w64-mingw32-objdump -p KenshiFP.dll | grep -i "startPlugin" | head -1
