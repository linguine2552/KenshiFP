#!/bin/sh
# Build KenshiFP_x64.dll (mingw + vendored MinHook from the KenshiMP tree).
# Self-contained under Proton: imports only KERNEL32 / msvcrt / USER32.
set -e
cd "$(dirname "$0")"
MH=../third_party/minhook                       # vendored (BSD-2-Clause)
[ -d "$MH" ] || MH=../../KenshiMP/reference/minhook   # dev-tree fallback
# version metadata (legitimacy signal for AV ML) -> COFF object
x86_64-w64-mingw32-windres kenshifp.rc -O coff -o kenshifp_res.o
x86_64-w64-mingw32-gcc -O2 -shared -static-libgcc -o KenshiFP_x64.dll \
    kenshifp_client.c kenshifp_res.o \
    "$MH"/src/hook.c "$MH"/src/buffer.c "$MH"/src/trampoline.c "$MH"/src/hde/hde64.c \
    -I "$MH"/include -I "$MH"/src \
    -DMINHOOK_BUILD -lkernel32 -ldinput8 -ldxguid -lwinmm -lm
echo "built KenshiFP_x64.dll"

# Deploy (uncomment; KENSHI = Steam install dir):
# KENSHI="$HOME/.steam/debian-installation/steamapps/common/Kenshi"
# cp KenshiFP_x64.dll "$KENSHI/"
# add a line 'Plugin=KenshiFP_x64' to "$KENSHI/Plugins_x64.cfg"
# launch Kenshi via Steam, press F in-game, then read KenshiFP.log:
#   find ~/.steam/debian-installation/steamapps/compatdata/233860/pfx -name KenshiFP.log
