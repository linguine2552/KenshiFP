KenshiFP (RE_Kenshi edition) - First-Person Mode for Kenshi
============================================================
This edition runs as an RE_Kenshi PLUGIN and works on ALL Kenshi builds that
RE_Kenshi supports -- Steam (any version) AND GOG -- with no version-specific
setup. If you are on standard Steam Kenshi and don't use RE_Kenshi, use the
standalone edition instead.

REQUIREMENTS
  - Kenshi (Steam or GOG)
  - RE_Kenshi (https://www.nexusmods.com/kenshi/mods/2063)

INSTALL
  1. Copy the whole "KenshiFP" folder into your Kenshi "mods" folder:
        <Kenshi install>/mods/KenshiFP/
     (so you have mods/KenshiFP/KenshiFP.dll, KenshiFP.mod, RE_Kenshi.json)
  2. Launch Kenshi. In the mod list, ENABLE "KenshiFP" (tick it), then start.
     RE_Kenshi loads the plugin automatically for enabled mods.
  3. Select a character and press RIGHT ALT to toggle first person.

  Do NOT also add a "Plugin=KenshiFP_x64" line to Plugins_x64.cfg -- that is
  only for the standalone edition. Running both at once conflicts.

CONTROLS
  Right Alt = first person | Mouse = look | WASD = move | Wheel = walk/run speed

CONFIG (optional, hot-reloaded in-game)
  Put a "KenshiFP.ini" in your Kenshi install ROOT (next to kenshi_x64.exe),
  NOT in this mod folder. Settings: fps_cap, fov, near_clip, sensitivity,
  aim_lean, ranged_freeaim, wheel_speed, ko_vignette, key rebinds, and more.
  (See the standalone edition's KenshiFP.ini for the full documented sample.)

A log "KenshiFP.log" is written in your Kenshi install root -- attach it to any
bug report.

Source (GPL-3.0): github.com/linguine2552/KenshiFP
