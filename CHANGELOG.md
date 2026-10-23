# HSFrameFix changelog

HSFrameFix is an ASI plugin for **Need for Speed: High Stakes** (`nfs4.exe`):
- frame-rate unlock with interpolation;
- precise game timing;
- unlocked menus;
- faster loading;
- per-camera FOV;
- Glide menu effects on any renderer;
- seamless widescreen.

Works with the original `nfs4.exe` release and with the widescreen-patched exe. The exe's built-in widescreen patch is undone at startup, because this mod replaces it. On any other exe, the mod leaves the game alone.

Tested renderers:
- Direct3D 7, using the `dx7a.dll` driver from Porsche Unleashed.
- Glide through nGlide.
- The software renderer, with the limits described under 0.7.7.

---

## 0.7.14: code review fixes

- **Possible freeze fixed:** if the precise-tick thread can't be created, the game now uses its own timer. Before, the simulation stopped.
- **INI corruption fixed:** saving FOV hotkey changes and saving the race resolution could happen at the same moment and garble lines. INI updates are now done one at a time.
- **Large INI files:** an INI too large for the 32 KB buffer was saved with its end cut off. Now it is left unchanged and a line is logged.
- **Menu frame cap:** the menu limit (at most 128 fps) now picks a rate that divides the refresh rate evenly. Examples: 165 Hz gives 55 fps, 144 Hz gives 72, 240 Hz gives 120. 60 and 120 Hz are unchanged.
- **Cleanup:** removed dead code left over from 0.7.9. The build now has no compiler warnings.

## 0.7.13: closing the window ends the game

- **Close from outside:** closing the game with Alt+F4, the window manager's close (for example `Win+Q` in i3) or the title-bar X now ends the process immediately.
- **Why:** the game runs its window and its simulation on separate threads. When only the window closed, the simulation kept running in the background or froze on the last frame. With older versions it could also crash during shutdown.
- **What stays the same:** quitting from the in-game menu is unchanged, because the mod can tell the game closing its own window apart from a close coming from outside.
- **New INI key:** `closeExits` (default `1`). It isn't added to existing INI files, but the default applies anyway.

## 0.7.12: watchdog on window close

- This build ended the game 3 seconds after its window disappeared. 0.7.13 replaced it, because the game could still crash during that wait.
- The per-race HUD layout lines in the log now only appear with `wsLog=2`.

## 0.7.11: HUD text size restored

- **Fix:** HUD text is back to its 4:3 proportions. 0.7.9 had shrunk it a second time, making text and numbers about 25% too small.

## 0.7.10: position and lap list fit their text

- **Fix:** the bottom-left position list ("POSITION 8/8" and the racer names and times) and the lap list size themselves to their text.
- **What was wrong:** the game converted that width using the full screen width, while the HUD is laid out at 4:3 width. The panel came out too narrow, the times stuck out past it, and the big position number overlapped the "POSITION" label.

## 0.7.9: HUD text size (reverted in 0.7.11)

- An attempt to fix the position-panel overlap by scaling HUD text. It turned out to be the wrong cause and shrank the HUD.

## 0.7.8: HUD layout logging

- Diagnostic only: logs every HUD box and where it is anchored.

## 0.7.7: software renderer

- **Software renderer:** the software renderer (`softtria.dll`) draws menu images as unscaled blits, so scaled menus came apart. With that renderer, menus now stay at 640x480. Races still use widescreen, Hor+ and the 4:3 HUD.

## 0.7.4 – 0.7.6: intro movies in widescreen

- **Intros fixed:** the FMVs (EA logo, intros) are drawn into the frame in software, and they broke once the menus ran at the widescreen resolution. They now scale correctly into the 4:3 menu area.
- **Fixes along the way:**
  - The colour depth now matches what the movie player expects.
  - The movie frame is read at its packed 640-pixel row width.
  - The movie player leaves 4 garbage columns on the right of each frame. They are now blanked.

## 0.7.2 – 0.7.3: menu and race resolution

- **Menus switch back:** returning to the menus from a race now switches to the widescreen menu resolution. Before, the game showed a small 640x480 picture in the top-left corner surrounded by black. The first menus at startup also come up in widescreen.
- **Race resolution kept:** a non-4:3 race resolution (for example 1366x768) is no longer reset to 1024x768. The game only saves the width, so the mod remembers the height in the INI (`wsRaceWidth`/`wsRaceHeight`, written automatically).

## 0.7.1: widescreen through the renderer API

- The widescreen code moved from Direct3D hooks to the game's renderer interface, so it works the same on Direct3D 7 and Glide. The 0.7.0 Direct3D approach broke on nGlide.

## 0.7.0: widescreen

Menus and races run at the same resolution, so there is no display-mode flash between them.

- **Menus:** the front end is made of 640x480 pictures. They are shown 4:3 in the middle of the screen with black bars, or stretched to fill it if you prefer (`wsMenus`).
- **Menu resolution:** menus run at the race resolution, or at the desktop resolution (`wsMenuResolution`).
- **3D view (Hor+):** keeps the 4:3 vertical view and shows more at the sides. The rear-view mirror is left alone.
- **HUD:** keeps its 4:3 proportions. Elements on the left and right are anchored to the screen edges and centre elements stay centred (`wsHud`).
- **Resolutions:** the options menu lists non-4:3 resolutions such as 1366x768 and 1920x1080 (`wsUnlockResolutions`).

## 0.6.6: exe check names the variant

- **Exe variants:** verified against the original `nfs4.exe` release as well as the widescreen-patched exe. The log says which one is running.

## 0.6.5: patch a modified exe on request

- **`exeCheck=0`:** applies the patches to a modified `nfs4.exe` anyway.
- **Mismatch logging:** every byte range that differs from the expected exe is logged, with the expected and found bytes, whether or not `exeCheck` is on.

## 0.6.4: menu frame cap

- **`menuMaxFPS`:** menus never run above 128 fps; the default picks a rate that divides the refresh rate. Several menu animations speed up past the game's 128 Hz tick.

## 0.6.3: menu list slide fixed

- **Menu timing:** only the menu item reveal timing is changed now. The list-slide and FeFx timers are original again: a zero time step made the list slide end early and loop.

## 0.6.2: frozen menu animations fixed

- **Fix:** 0.6.1's menu timing change froze menu animations after the game reset its tick counter (negative elapsed time). Negative time now resyncs, like the original game.

## 0.6.1: menu animation speed

- **Fix:** the menu item reveal and glow, and two other menu animations, no longer run faster above 64 fps. They had counted at least 2 ticks per frame.

## 0.6: Glide menu effects on any renderer

- **`glideMenuEffects`:** brings the front end's Glide-only effects to Direct3D:
  - the menu glow;
  - full-brightness highlight colours;
  - widget timing;
  - a transition render state.

## 0.5.3: FOV in crash and recovery cameras

- **Fix:** crash and recovery cameras keep the FOV offset of the camera you picked. They used to fall back to the global offset only.

## 0.5.2: FOV hotkeys off the game thread

- **No stalls:** FOV hotkeys no longer write the INI or the log on the game thread. A background thread saves them, so a burst of key presses can't stall a frame.
- **Log:** the log now says when clamped frames happened.

## 0.5.1: cockpit FOV

- **Cockpit:** the cockpit model used its own fixed 80° FOV. It now scales with the in-car FOV offset, so the dashboard widens along with the world.

## 0.5: loading speed-up and camera FOV

- **Loading (`speedUpLoading`):**
  - The loading bar did a full GPU sync and two vsync'd page flips on every update, about 48 times per race load.
  - It is now redrawn at most every `loadScreenRenderMs`. Skipped updates still run its callback poll.
- **Camera FOV:**
  - The game uses one FOV for the whole view: 90°, 75° with the narrow view-angle option, +15° in some modes.
  - A per-camera offset is added: `fovOffset`, plus `fovOffsetInCar`, `fovOffsetBumper`, `fovOffsetHeli` and `fovOffsetRoof`. The rear-view mirror is left alone.
  - In a race, F6, F7 and F8 change the current camera's offset and save it to the INI.

## 0.2 – 0.4: smoother clock, camera cuts, unlocked menus

- **0.2, smoother clock:** frames are placed on a smoothed simulation clock with a 4-snapshot history. Mapping steps directly to the 64 Hz counter shifted by a whole step now and then, which showed as flicker and hitches.
- **0.3, camera cuts:** these now draw the frame at the newest step. That includes switching camera, look back/left/right, and jumps too large to blend. Before, the newest camera was mixed with blended cars, which jolted when you pressed look back.
- **0.4, menus (`unlockMenus`):**
  - Every front-end screen used to wait until the 128 Hz tick counter had advanced by 2, a 64 fps cap.
  - That wait is replaced with one idle call per frame, plus the optional limiter, so menus run at the display's refresh rate.
  - Menu animations follow the tick counter, so their speed is unchanged.

## 0.1: frame-rate unlock with interpolation

- **How the game keeps time:** it simulates at 64 Hz and only renders after a new simulation step. That caps it at 64 fps, and on a 60 Hz display four double-steps a second show as judder.
- **`preciseTicks`:** drives the game's 128 Hz timer chain from a high-resolution scheduler, so tick times are exact. The original used 1 ms `GetTickCount` one-shots.
- **`interpolate`:**
  - After every simulation step, it records the position and rotation of all cars, all cameras and the helicopter.
  - Before each frame, it blends them to one 64 Hz step in the past and renders.
  - Then it restores the exact simulation values, so the simulation never sees interpolated data.
- **Rendering between steps:** frames between simulation steps are rendered too, so the frame rate follows the display (vsync) or the optional limiter instead of the 64 Hz simulation.

---

## INI keys added in 0.7 (widescreen and window close)

| Key | Default | Meaning |
|---|---|---|
| `widescreen` | `1` | Master switch for everything below. |
| `wsMenus` | `1` | `1` = menus 4:3 in the middle, `2` = stretched, `0` = original (switches to 640x480). |
| `wsMenuResolution` | `1` | `1` = menus at the race resolution, `2` = desktop resolution. |
| `wsFixFOV` | `1` | Hor+ 3D view. |
| `wsHud` | `1` | `1` = 4:3 HUD anchored to the screen edges, `2` = 4:3 HUD in the middle, `0` = original (stretched). |
| `wsMovies` | `1` | Scale intro movies like the menus. |
| `wsUnlockResolutions` | `1` | List non-4:3 resolutions in the options. |
| `wsLog` | `1` | Widescreen log lines; `2` also logs the HUD layout. |
| `wsRaceWidth` / `wsRaceHeight` | (auto) | Last race resolution, saved by the mod. |
| `closeExits` | `1` | Closing the window from outside ends the game. |

## Build

```
i686-w64-mingw32-g++ -std=c++17 -O2 -shared -s -static-libgcc -static-libstdc++ \
  -o HSFrameFix.asi HSFrameFix.cpp -lwinmm -luser32
```

The plugin is a single file, loaded by an ASI loader. It reads `HSFrameFix.ini` and writes `HSFrameFix.log` next to the `.asi`.
