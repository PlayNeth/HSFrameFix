# HSFrameFix

An unlocked frame rate with smooth interpolation, and seamless widescreen, for **Need for Speed: High Stakes** on PC.

The game simulates at a fixed 64 Hz and only draws a frame after each simulation step. HSFrameFix draws at your display's refresh rate and blends motion between steps, so the game is smooth at 60, 120, 144 Hz and higher. It also adds proper widescreen and per-camera field of view, and fixes a few of the PC version's annoyances. It's a single file, `HSFrameFix.asi`, and it doesn't touch the game's files on disk.

---

## Contents

- [Requirements](#requirements)
- [Installing](#installing)
- [Uninstalling](#uninstalling)
- [What it changes](#what-it-changes)
- [Settings](#settings)
- [Files the mod creates](#files-the-mod-creates)
- [Troubleshooting](#troubleshooting)
- [Building from source](#building-from-source)
- [License](#license)

---

## Requirements

- **Need for Speed: High Stakes** for PC, with `nfs4.exe`: the original release (SHA-1 `8e10048ebc1b962bbe5dd569b48a791eeaafab5c`) or the widescreen-patched exe. The exe's own widescreen patch is undone at startup, because this mod replaces it.
- An **ASI loader**, such as [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader).
- One of the tested renderers:
  - **Direct3D 7**, with the `dx7a.dll` driver from Porsche Unleashed;
  - **Glide**, through nGlide;
  - the **software renderer** (`softtria.dll`), where menus stay 640×480.

It runs on Windows, and on Linux through Wine or Proton.

## Installing

1. Install the ASI loader in the game's folder, following its instructions for this game.
2. Copy **`HSFrameFix.asi`** into the game's folder, or into the loader's `scripts` folder.
3. Start the game.

On the first start the mod creates `HSFrameFix.ini`, with every setting explained, next to the `.asi`. To check it's working, look in `HSFrameFix.log`: its first line names the mod's version and settings.

## Uninstalling

Delete `HSFrameFix.asi`, and the ASI loader if nothing else uses it. You can also delete `HSFrameFix.ini` and `HSFrameFix.log`.

---

## What it changes

Almost everything can be switched off or adjusted in `HSFrameFix.ini`.

### Frame rate and timing

- **Unlocked frame rate with interpolation:** the game is drawn at your display's refresh rate, with cars, the camera and the police helicopter blended between simulation steps.
- **A precise game timer:** the game's 128 Hz timer runs from a high-resolution clock instead of the original 1 ms timer chain, so steps arrive evenly.
- **Unlocked menus:** the 64 fps menu cap is gone. A ceiling keeps menu animations at the right speed, picked to divide your refresh rate evenly (165 Hz gives 55 fps, 144 Hz gives 72, 240 Hz gives 120).

### Widescreen

- **Menus** run at the race resolution, shown 4:3 in the middle or stretched.
- **The 3D view is Hor+:** the 4:3 vertical view stays, and you see more at the sides.
- **The HUD** keeps its 4:3 proportions, with its side elements anchored to the screen edges.
- **Intro movies** are scaled like the menus.
- **Resolutions** such as 1366×768 and 1920×1080 are listed in the options.

### Camera

- **Field of view per camera:** in-car, bumper, helicopter and roof views can each be widened.
- **Hotkeys in a race:** F8 widens the current camera's view, F7 narrows it and F6 resets it. Changes are saved to the INI.

### Convenience and fixes

- **Glide's menu glow effects** also appear on Direct3D.
- **Faster loading:** the loading bar is redrawn less often.
- **Closing the window ends the game.** Alt+F4, the title-bar X or the window manager's close no longer leave the game running, or frozen, in the background. Quitting from the in-game menu is unchanged.

---

## Settings

**`HSFrameFix.ini`** is next to the `.asi`. Every setting has a comment saying what it does. The most useful ones:

| Setting | Default | What it does |
|---|---|---|
| `interpolate` | `1` | Draws at the refresh rate, blending motion between simulation steps. |
| `interpolateCopter` | `1` | Smooths the police helicopter too. |
| `preciseTicks` | `1` | Runs the game's 128 Hz timer from a precise clock (needed for smooth blending). |
| `targetFPS` | `0` | Frame limiter; `0` is off (use v-sync). Without v-sync, set it to your refresh rate. |
| `interpDelay` | `1.0` | How many steps behind the newest one frames are drawn; raise it to `1.5` if you see hitches. |
| `menuMaxFPS` | `-1` | Menu frame ceiling; `-1` picks the highest rate up to 128 that divides the refresh rate. |
| `fovOffset` | `0` | Degrees added to every race camera's view. `fovOffsetInCar`, `fovOffsetBumper`, `fovOffsetHeli` and `fovOffsetRoof` set single cameras. |
| `fovHotkeys` | `1` | The F8 / F7 / F6 hotkeys in a race. |
| `glideMenuEffects` | `1` | Glide's menu glow effects on every renderer. |
| `widescreen` | `1` | Master switch for the widescreen features. |
| `wsMenus` | `1` | `1` menus 4:3 in the middle, `2` stretched, `0` the original 640×480. |
| `wsMenuResolution` | `1` | `1` menus at the race resolution, `2` at the desktop's. |
| `wsFixFOV` | `1` | The Hor+ 3D view. |
| `wsHud` | `1` | `1` 4:3 HUD anchored to the screen edges, `2` 4:3 HUD in the middle, `0` the original (stretched). |
| `wsMovies` | `1` | Scales intro movies like the menus. |
| `wsUnlockResolutions` | `1` | Lists non-4:3 resolutions in the options. |
| `closeExits` | `1` | Closing the window from outside ends the game. |
| `exeCheck` | `1` | Turns the mod off if the exe differs; `0` patches anyway (only for a modified exe you know is the same version). |

[CHANGELOG.md](CHANGELOG.md) has the version history.

## Files the mod creates

All next to the `.asi`:

| File | |
|---|---|
| `HSFrameFix.ini` | Settings. The mod also saves your FOV hotkey changes and your race resolution there. |
| `HSFrameFix.log` | What the mod did; send it with any bug report. |

## Troubleshooting

- **Nothing changed, and there's no `HSFrameFix.log`:** the ASI loader isn't loading. Check its instructions for this game, and that `HSFrameFix.asi` is in the game's folder or the loader's `scripts` folder.
- **The log says the exe differs and the mod is off:** your `nfs4.exe` isn't one the mod was made for. Check it against the SHA-1 above. The mod leaves the game alone to be safe.
- **Occasional hitches:** raise `interpDelay` to `1.5`.
- **Tearing, or the frame rate runs too high:** turn on v-sync, or set `targetFPS` to your refresh rate.
- **Menus stay 640×480:** that's expected on the software renderer.
- **Something feels off:** most features can be switched off in the INI one at a time. Please report bugs with your `HSFrameFix.log`.

## Building from source

The source is a single file, `HSFrameFix.cpp`, built with MinGW-w64, 32-bit:

```
i686-w64-mingw32-g++ -std=c++17 -O2 -shared -s -static-libgcc -static-libstdc++ \
  -o HSFrameFix.asi HSFrameFix.cpp -lwinmm -luser32
```

## License

GPL-3.0. See [LICENSE](LICENSE). Need for Speed: High Stakes is © Electronic Arts; this mod contains no game files.
