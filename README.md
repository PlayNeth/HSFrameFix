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

- **Need for Speed: High Stakes** for PC, version 4.50, with one of these `nfs4.exe` builds:
  - the official 4.50 patch, unmodified: the `nfshs.exe` inside EA's `NFSHSpatch4_50.exe` (SHA-1 `ac4f57ad3588161e0e0dd3ab582f7ad1f4d532eb`). This is the main one. The mod removes its CD check and its free disk space check at startup;
  - the same exe with its CD check already removed (SHA-1 `b966f01d8d11e27f6d3167b8ec1ad3e3c2d30148`). Versions up to 0.7.15 listed this one as "unmodified";
  - VEG's 4.50 patch (SHA-1 `8e10048ebc1b962bbe5dd569b48a791eeaafab5c`). Its built-in widescreen patch is undone at startup, because this mod replaces it.
- An **ASI loader**, such as [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader).
- One of the tested renderers:
  - **Direct3D 7**, with the `dx7a.dll` driver from Porsche Unleashed;
  - **Glide**, through [gl2d9](https://github.com/PlayNeth/gl2d9);
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
- **The loading screen** keeps its 4:3 shape in the middle of the screen.
- **Resolutions** such as 1366×768 and 1920×1080 are listed in the options, each once, in 32-bit colour where the renderer offers it. The one you choose stays chosen.

### Night races

- **The nine tracks from Need for Speed III are lit at night.** Hometown, Redrock Ridge, Atlantica, Rocky Pass, Country Woods, Lost Canyons, Aquatica, Summit and Empire City were pitch black outside the headlights. Their lamps, windows, lit signs and tunnel lights now light the road and what's around them. The game's own tracks are left as they are.

### Camera

- **Field of view per camera:** in-car, bumper, helicopter and roof views can each be widened.
- **Hotkeys in a race:** F8 widens the current camera's view, F7 narrows it and F6 resets it. Changes are saved to the INI.

### Convenience and fixes

- **Glide's menu glow effects** also appear on Direct3D.
- **Faster loading:** the loading bar is redrawn less often.
- **Closing the window ends the game.** Alt+F4, the title-bar X or the window manager's close no longer leave the game running, or frozen, in the background. Quitting from the in-game menu is unchanged.

---

## Settings

**`HSFrameFix.ini`** is next to the `.asi`. It is grouped into sections and every setting has a note saying what it does. A file from an older version keeps its layout (new settings are added at its end); delete it to get a fresh one. The most useful settings:

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
| `wsMenuResolution` | `1` | `1` menus at the resolution chosen in the game's options (at startup, the one used last time), `2` at the desktop's. |
| `wsFixFOV` | `1` | The Hor+ 3D view. |
| `wsHud` | `1` | `1` 4:3 HUD anchored to the screen edges, `2` 4:3 HUD in the middle, `0` the original (stretched). |
| `wsMovies` | `1` | Scales intro movies like the menus. |
| `wsLoading` | `1` | `1` loading screen 4:3 in the middle, `0` stretched to the screen's width. |
| `wsUnlockResolutions` | `1` | Lists non-4:3 resolutions in the options. |
| `ws32bit` | `1` | Lists each resolution in 32-bit colour where the renderer offers it; `0` = 16-bit. |
| `nightLamps` | `1` | Night races on the Need for Speed III tracks: how bright their lamps light what's around them; `0` = the game's dark night. |
| `nightLampReach` | `22` | How far a street lamp's light reaches, in metres. |
| `nightScenery` | `1` | Night races on those tracks: light from windows, signs, tunnel lights and the like, the cities' street lighting, and the night sky's dim light on Aquatica; `0` = the lamps only. |
| `closeExits` | `1` | Closing the window from outside ends the game. |
| `noCDCheck` | `1` | The game doesn't ask for its CD at startup (the official 4.50 exe does). |
| `portableInstall` | `1` | The game finds its files from the folder its exe is in. No registry entries are needed (without them the game says "files are corrupted; please re-install"), and the game folder can be copied or moved. Music and movies are read from the game folder if `Data\Audio\Music` and `Data\Movies` are there, else from the CD drive the registry names. |
| `language` | `english` | Used when the registry has no language: `english`, `german`, `french`, `spanish`, `italian`, `swedish`. |
| `renderer` | empty | `d3d` (`d3da.dll`), `software` (`softtria.dll`), `voodoo` or `voodoo2` (both `voodoo2a.dll`). Empty = the registry's setting (`d3da.dll` if it has none). |
| `alternatePixelCenter` | `1` | Direct3D only: every vertex is moved half a pixel up and left, so menus, HUD and text land exactly on the pixels and come out sharp. |
| `gameThreadsOneCore` | `1` | The game's own threads (main, sound, streaming) run on one CPU core, which stops the sound crackling and popping on today's CPUs. The system's threads keep the other cores. |
| `widenAffinity` | `1` | If the whole game was started on one core (`taskset`, a launcher option), it gets all cores back and only its own threads stay on one. Needs `gameThreadsOneCore=1`. |
| `driversFolder` | empty | Folder the renderer DLLs (`d3da.dll`, `softtria.dll`, `voodoo2a.dll`) are loaded from: a folder in the game folder, or a full path. A DLL that isn't there is loaded from the game folder. Empty = the game folder. |
| `noDiskSpaceCheck` | `1` | No test for free disk space at startup. On today's disks the game's test goes wrong and says "Your hard drive has less than 1MB free space". |
| `exeCheck` | `1` | Turns the mod off if the exe differs; `0` patches anyway (only for a modified exe you know is the same version). |

[CHANGELOG.md](CHANGELOG.md) has the version history.

## Files the mod creates

All next to the `.asi`:

| File | |
|---|---|
| `HSFrameFix.ini` | Settings. The mod also saves your FOV hotkey changes and your race resolution there. |
| `HSFrameFix.log` | What the mod did; send it with any bug report. |
| `HSFrameFix_scene.txt` | Only with `nightLampsLog=1`: the track as the game holds it, written when F10 is pressed in a race. |
| `HSFrameFix_hitch.txt` | Only with `hitchLog=1`: the second leading up to each hitch. |

## Troubleshooting

- **Nothing changed, and there's no `HSFrameFix.log`:** the ASI loader isn't loading. Check its instructions for this game, and that `HSFrameFix.asi` is in the game's folder or the loader's `scripts` folder.
- **The log says the exe differs, or isn't a supported build, and the mod is off:** your `nfs4.exe` isn't one of the builds above. Check it against the SHA-1s under Requirements. The mod leaves the game alone to be safe.
- **Occasional hitches:** raise `interpDelay` to `1.5`.
- **Tearing, or the frame rate runs too high:** turn on v-sync, or set `targetFPS` to your refresh rate.
- **Menus stay 640×480:** that's expected on the software renderer.
- **Something feels off:** most features can be switched off in the INI one at a time. Please report bugs with your `HSFrameFix.log`.

## Building from source

The source is `HSFrameFix.cpp` and the files under `src/`: one `.cpp` for each area of the mod, each with a header (`.h`) for what the other files use. It is built with MinGW-w64, 32-bit, from the repository's folder:

```
i686-w64-mingw32-g++ -std=c++17 -O2 -shared -s -static-libgcc -static-libstdc++ \
  -o HSFrameFix.asi HSFrameFix.cpp src/*.cpp -lwinmm -luser32
```

## License

GPL-3.0. See [LICENSE](LICENSE). Need for Speed: High Stakes is © Electronic Arts; this mod contains no game files.
