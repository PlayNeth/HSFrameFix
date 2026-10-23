# HSFrameFix

An ASI plugin for **Need for Speed: High Stakes** (PC, `nfs4.exe`) that unlocks the frame rate with smooth interpolation and adds seamless widescreen.

## What it does

- **Frame rate unlock with interpolation.** The game simulates at a fixed 64 Hz and normally only draws a frame after each simulation step. HSFrameFix renders at your display's refresh rate and blends car, camera and police helicopter motion between steps, so motion is smooth on 60, 120, 144 Hz and higher displays.
- **Precise game timer.** The game's 128 Hz timer runs from a high-resolution clock in place of the original 1 ms timer chain, so steps arrive evenly.
- **Widescreen:**
  - Menus run at the race resolution, shown 4:3 in the middle or stretched.
  - The 3D view is Hor+: the 4:3 vertical view stays, and you see more at the sides.
  - The HUD keeps its 4:3 proportions, with side elements anchored to the screen edges.
  - Intro movies are scaled like the menus.
  - Non-4:3 resolutions such as 1366x768 and 1920x1080 are listed in the options.
- **Camera FOV adjustment** per camera, with in-race hotkeys (F8 wider, F7 narrower, F6 reset).
- **Menus:** the 64 fps menu cap is removed (with a ceiling that keeps menu animations at the right speed). The menu glow effects the game only draws on Glide also appear on Direct3D.
- **Faster loading:** the loading bar is redrawn less often.
- **Closing the window ends the game.** Alt+F4, the title-bar X or the window manager's close no longer leave the game running in the background.

## Requirements

- `nfs4.exe`, the original release (SHA-1 `8e10048ebc1b962bbe5dd569b48a791eeaafab5c`) or the widescreen-patched exe. The exe's own widescreen patch is undone at startup, because this mod replaces it.
- An ASI loader.
- Tested renderers:
  - Direct3D 7 with the `dx7a.dll` driver from Porsche Unleashed.
  - Glide through nGlide.
  - The software renderer (`softtria.dll`); menus stay 640x480 there.

Every patch site is checked against the expected bytes first. On any other exe the mod turns itself off (see `exeCheck`).

## Installation

1. Install an ASI loader for the game.
2. Put `HSFrameFix.asi` in the game folder (or the loader's `scripts` folder).
3. Start the game. `HSFrameFix.ini` and `HSFrameFix.log` are created next to the `.asi`.

## Settings

All settings are in `HSFrameFix.ini`. The most useful ones:

| Key | Default | Meaning |
|---|---|---|
| `interpolate` | `1` | Render at the refresh rate, blending motion between simulation steps. |
| `interpolateCopter` | `1` | Also smooth the police helicopter. |
| `preciseTicks` | `1` | Run the game's 128 Hz timer from a precise clock (needed for smooth blending). |
| `targetFPS` | `0` | Frame limiter; `0` = off (use v-sync). Set it to your refresh rate without v-sync. |
| `interpDelay` | `1.0` | Steps behind the newest step that frames are drawn; raise to 1.5 if you see hitches. |
| `menuMaxFPS` | `-1` | Menu frame ceiling; `-1` = auto (highest rate up to 128 that divides the refresh rate). |
| `fovOffset` | `0` | Degrees added to every race camera's FOV (`fovOffsetInCar`, `fovOffsetBumper`, `fovOffsetHeli`, `fovOffsetRoof` for single cameras). |
| `fovHotkeys` | `1` | F8 / F7 / F6 change the current camera's FOV in race (saved to the INI). |
| `widescreen` | `1` | Master switch for all widescreen features. |
| `wsMenus` | `1` | `1` = menus 4:3 in the middle, `2` = stretched, `0` = original. |
| `wsMenuResolution` | `1` | `1` = menus at the race resolution, `2` = desktop resolution. |
| `wsFixFOV` | `1` | Hor+ 3D view. |
| `wsHud` | `1` | `1` = 4:3 HUD anchored to the screen edges, `2` = 4:3 HUD in the middle, `0` = original (stretched). |
| `wsMovies` | `1` | Scale intro movies like the menus. |
| `wsUnlockResolutions` | `1` | List non-4:3 resolutions in the options. |
| `closeExits` | `1` | Closing the window from outside ends the game. |
| `exeCheck` | `1` | Turn the mod off if the exe differs; `0` patches anyway (only for a modified exe you know is the same version). |

The INI itself has a comment for every key. Version history is in [CHANGELOG.md](CHANGELOG.md).

## Building

A single source file, built with MinGW-w64 (i686):

```
i686-w64-mingw32-g++ -std=c++17 -O2 -shared -s -static-libgcc -static-libstdc++ \
  -o HSFrameFix.asi HSFrameFix.cpp -lwinmm -luser32
```

## License

GPL-3.0. See [LICENSE](LICENSE).
