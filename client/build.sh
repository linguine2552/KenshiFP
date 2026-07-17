#!/bin/sh
# Build KenshiFP_x64.dll (mingw + vendored MinHook from the KenshiMP tree).
# Self-contained under Proton: imports only KERNEL32 / msvcrt / USER32.
set -e
cd "$(dirname "$0")"
MH=../../KenshiMP/reference/minhook
x86_64-w64-mingw32-gcc -O2 -shared -static-libgcc -o KenshiFP_x64.dll \
    kenshifp_client.c \
    "$MH"/src/hook.c "$MH"/src/buffer.c "$MH"/src/trampoline.c "$MH"/src/hde/hde64.c \
    -I "$MH"/include -I "$MH"/src \
    -DMINHOOK_BUILD -lkernel32
echo "built KenshiFP_x64.dll"

# Deploy (uncomment; KENSHI = Steam install dir):
# KENSHI="$HOME/.steam/debian-installation/steamapps/common/Kenshi"
# cp KenshiFP_x64.dll "$KENSHI/"
# add a line 'Plugin=KenshiFP_x64' to "$KENSHI/Plugins_x64.cfg"
# launch Kenshi via Steam, press F in-game, then read KenshiFP.log:
#   find ~/.steam/debian-installation/steamapps/compatdata/233860/pfx -name KenshiFP.log
