// What the other parts use from config.cpp.
#pragma once
#include "common.h"

struct Config {
    int   interpolate      = 1;
    int   interpolateCopter = 1;
    int   preciseTicks     = 1;
    int   targetFPS        = 0;     // 0 = no limiter (use vsync)
    float maxStepDistance  = 10.0f; // metres per sim step; larger jumps are not blended
    float maxStepAngle     = 90.0f; // degrees per sim step
    float interpDelay      = 1.0f;  // how far behind the newest step frames are shown (steps)
    int   unlockMenus      = 1;
    int   menuTargetFPS    = -1;    // -1 = same as targetFPS, 0 = off
    int   speedUpLoading   = 1;
    int   loadScreenRenderMs = 100;
    // camera FOV offsets (degrees added to the game's FOV)
    int   fovOffset        = 0;
    int   fovOffsetInCar   = 0;     // camera type 1
    int   fovOffsetBumper  = 0;     // type 2
    int   fovOffsetHeli    = 0;     // types 3, 5 (static)
    int   fovOffsetRoof    = 0;     // types 4, 6 (static)
    int   fovHotkeys       = 1;
    int   fovKeyUp         = 0x77;  // F8
    int   fovKeyDown       = 0x76;  // F7
    int   fovKeyReset      = 0x75;  // F6
    int   fovStep          = 1;
    int   glideMenuEffects = 1;
    int   exeCheck         = 1;     // 1 = turn the mod off if the exe differs, 0 = patch anyway
    int   menuMaxFPS       = -1;    // -1 = auto (<= 128, divides the refresh rate), 0 = none
    int   logToFile        = 1;
    int   logEverySeconds  = 10;
    // widescreen
    int   widescreen       = 1;
    int   wsMenus          = 1;     // 0 = original 640x480 mode, 1 = 4:3 centered, 2 = stretched
    int   wsMenuResolution = 1;     // 1 = the resolution chosen in the options, 2 = the desktop's
    int   wsFixFOV         = 1;     // Hor+ 3D
    int   wsHud            = 1;     // 0 = original (stretched), 1 = 4:3 at the screen edges, 2 = 4:3 centered
    int   wsMovies         = 1;     // scale FMVs with the menus
    int   wsUnlockResolutions = 1;  // list non-4:3 display modes
    int   hitchLog         = 0;     // a hitch's lead-up written to HSFrameFix_hitch.txt
    int   nightLampsLog    = 0;     // F10 (nightLampsLogKey) writes the scene to HSFrameFix_scene.txt
    int   nightLampsLogKey = 0x79;
    float nightLamps       = 1.0f;  // night, Need for Speed III's tracks: the lamps light what's around them, x this (0 = off)
    float nightLampReach   = 22.0f; // a street lamp's light reaches this far (m)
    float nightScenery     = 1.0f;  // the lights found on those tracks' polygons, x this (0 = off)
    int   wsLoading        = 1;     // loading screen 4:3 in the middle
    int   ws32bit          = 1;     // list each resolution in 32-bit colour where the renderer offers it
    int   wsLog            = 1;
    int   closeExits       = 1;     // closing the window (Alt+F4, window manager) ends the game
    int   noCDCheck        = 1;     // skip the official exe's two startup CD checks
    int   alternatePixelCenter = 1; // Direct3D: every vertex half a pixel up and left
    int   gameThreadsOneCore = 1;   // the game's threads share one core (sound races)
    int   widenAffinity    = 1;     // a process pinned to one core gets all cores back; only the game's threads stay on one
    int   portableInstall  = 1;     // install folder, level and CD drive answered from the game's own folder
    char  language[16]     = "english";   // used when the registry has no Language
    char  driversFolder[MAX_PATH] = "";   // renderer DLLs; "" = game folder
    char  renderer[16]     = "";    // d3d, software, voodoo, voodoo2; "" = the registry's (d3da.dll if it has none)
    int   noDiskSpaceCheck = 1;     // skip the "less than 1MB free" test (wrong on big disks)
    int   wsRaceWidth      = 0;     // remembered race resolution (the game only saves the width)
    int   wsRaceHeight     = 0;
};
extern Config g_cfg;
extern char g_asiDir[MAX_PATH];
extern char g_iniPath[MAX_PATH];
void LoadConfig();
