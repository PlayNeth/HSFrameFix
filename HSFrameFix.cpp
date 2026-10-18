// HSFrameFix.asi  v0.1
// Frame-rate unlock with interpolation for Need for Speed: High Stakes (PC)
//
// Analysed exe: nfs4.exe SHA-1 8e10048ebc1b962bbe5dd569b48a791eeaafab5c
// (PE timestamp 0x57334341, image base 0x400000, no ASLR). Every patch site is
// byte-checked first; on any other exe the ASI does nothing.
//
// How the game keeps time (sim.c main loop at 0x51C6D0):
//  - A 128 Hz multimedia-timer chain (callback 0x548850, TimerInit(128) at
//    0x5049FF) wakes a handler thread; handler 0x459E10 counts 128/64/32 Hz
//    ([0x6813AC]/[0x6813B0]/[0x6813B4]).
//  - Input/network frames run at 32 Hz; the simulation (0x51BBE0) steps at
//    64 Hz. The physics timestep is hard-coded (1/64), so the sim rate itself
//    cannot change without altering the game.
//  - The loop only renders (0x5113E0) after a new sim step, so the frame rate
//    is capped at 64 and a 60 Hz display shows 4 double-steps per second.
//  - Cameras are updated by a sim task (0x441A00 in task list [0x853B90]);
//    the renderer only derives the view from camera + car state.
//
// What this ASI does:
//  - preciseTicks: drives the 128 Hz chain from a QPC-based scheduler so tick
//    times are exact (the original uses 1 ms GetTickCount one-shots).
//  - v0.2: frames are placed on a smoothed simulation clock (follows when steps
//    arrive, averaged) with a 4-snapshot history, and on a smoothed frame clock,
//    instead of mapping steps to the 64 Hz counter (that mapping shifted by a
//    whole step now and then, which showed as flicker/hitches).
//  - v0.3: camera cuts (switching camera, look back/left/right [0x661968],
//    jumps too large to blend) draw the frame at the newest step instead of
//    mixing a newest camera with blended cars (jolt when pressing look back).
//  - v0.4: menus. Every front-end screen presents through 0x538170: flush,
//    page flip, then it spins until the 128 Hz tick counter [0x9E6D54] has
//    advanced by 2 (= 64 fps cap), calling the idle work 0x526460 meanwhile.
//    unlockMenus replaces that wait with one idle call per frame (plus the
//    optional limiter), so menus run at the display's refresh rate. Menu
//    animations are driven by the tick counter, so their speed is unchanged.
//  - v0.6.6: verified against the original nfs4.exe release (sha256 17a0f96c3bcfb94f...)
//    as well as the widescreen-patched exe; the log names which one is running.
//  - v0.6.5: exeCheck=0 patches a modified nfs4.exe anyway; every differing byte
//    range is logged (expected vs found) either way.
//  - v0.6.4: menuMaxFPS - menus never run above 128 fps (auto: a divisor of the
//    refresh rate), since several menu animations speed up past the 128 Hz tick.
//  - v0.6.3: only the menu item reveal timing is changed; the list-slide and FeFx
//    timers are original again (a zero step made the list slide end early and loop).
//  - v0.6.2: v0.6.1's menu timing fix froze menu animations when the tick counter
//    had been reset (negative elapsed time); negative now resyncs like the original.
//  - v0.6.1: menu item reveal / glow and two other menu animations no longer run
//    faster above 64 fps (they counted at least 2 ticks per frame).
//  - v0.6: glideMenuEffects - the front end's Glide-only effects (menu glow, full
//    brightness highlight colours, widget timing, a transition render state) on Direct3D.
//  - v0.5.3: crash / recovery cameras keep the FOV offset of the camera the player
//    picked ([0x7C3CA8+view*4]) instead of falling back to the global offset only.
//  - v0.5.2: FOV hotkeys no longer write the ini / log on the game thread (a
//    background thread saves them), so a burst of key presses can't stall a frame.
//    The log now says when clamped frames happened.
//  - v0.5.1: the cockpit model (0x4D1C80) uses its own fixed 80 degree FOV; it now
//    scales with the in-car FOV offset so the dashboard widens with the world.
//  - v0.5: loading. The loading bar (0x4E7B80, ~48 calls per race load) does a
//    full GPU sync (THRASH_sync(0) = lock/unlock) and two vsync'd page flips
//    every call. speedUpLoading only redraws it every loadScreenRenderMs; the
//    skipped calls still run its callback poll (0x54A790).
//    Camera FOV: the game uses one FOV for the whole view, from 0x539E10
//    (90 deg, 75 with the narrow view-angle option, +15 in some modes). Its
//    three callers inside the view setup (0x539F80, view in [ebp-4]) are
//    redirected so a per-camera offset is added (camera type of that view's
//    active camera; the rear-view mirror is left alone). F6/F7/F8 change the
//    current camera's offset in race and save it to the ini.
//  - interpolate: after every sim step it snapshots the transforms of all cars
//    (pos car+8, rotation car+0x34), all camera objects (pos cam+0x14,
//    rotation cam+0x2C, table 0x660DF0) and the helicopter (position). Before
//    each render it blends them to "now - one 64 Hz step" (rotations by
//    quaternion slerp), renders, then restores the exact simulation values,
//    so the simulation never sees interpolated data.
//  - The main loop's "nothing new, skip render" branch (0x51CB6C) now renders
//    an interpolated frame instead, so the frame rate follows the display
//    (vsync) or the optional limiter instead of the 64 Hz sim.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

#define HSFF_VERSION "0.7"

// ---------------------------------------------------------------------------
// Game addresses
// ---------------------------------------------------------------------------
namespace hs {
    // timer
    const DWORD fnTimerCallback = 0x548850;   // TIMECALLBACK (stdcall, ret 0x14)
    const DWORD pTimerPeriod    = 0x9E6D00;   // ms per tick, 16.16 fixed point
    const DWORD fnTickHandler   = 0x459E10;   // registered handler (128 Hz)
    const DWORD immRegHandler   = 0x459E7C;   // imm32 of "mov eax, 0x459E10" (register)
    const DWORD immUnregHandler = 0x459EC0;   // imm32 of "mov eax, 0x459E10" (unregister)
    const DWORD pCount64        = 0x6813B0;   // 64 Hz counter
    // main loop
    const DWORD fnSimStep       = 0x51BBE0;
    const DWORD callSimStep[4]  = { 0x51CA61, 0x51CA70, 0x51CA75, 0x51CAC3 };
    const DWORD fnRender        = 0x5113E0;
    const DWORD callRender      = 0x51CB83;
    const DWORD skipRenderAt    = 0x51CB64;   // mov edi,[0x853B7C] / test / jne 0x51CB88
    const DWORD skipRenderCont  = 0x51CB6E;   // normal render decision
    const DWORD skipRenderOut   = 0x51CB88;   // after render
    const DWORD pSimTarget      = 0x853B30;   // target step (64 Hz units)
    const DWORD pSimStepIdx     = 0x853B34;   // next step index
    const DWORD pNoRender       = 0x853B7C;
    const DWORD pRenderArg      = 0x853B24;
    // objects
    const DWORD pCarCount       = 0x661A60;
    const DWORD pCarList        = 0x661A88;   // Car*[16]
    const DWORD pCamTable       = 0x660DF0;   // Camera*[32 types][2 views]
    const DWORD pCamActive      = 0x661950;   // Camera*[2]
    const DWORD pCamOverride    = 0x661958;   // Camera*[2]
    const DWORD pLookMode       = 0x661968;   // int[2]: 0 ahead, 1 back, 2/3 sides
    // front end
    const DWORD menuWaitAt      = 0x538185;   // mov eax,[0x9E6D54] - start of the 2-tick wait
    const DWORD menuWaitDone    = 0x5381A9;   // mov eax,[ebp-8] - after the wait
    // loading bar
    const DWORD fnLoadBar       = 0x4E7B80;   // void LoadBar(float percent), ret 4
    // FOV
    const DWORD fnGetFov        = 0x539E10;   // int GetViewFov()
    const DWORD callGetFov[3]   = { 0x539FA6, 0x539FCB, 0x539FEB };
    const DWORD pSelectedCam    = 0x7C3CA8;   // camera type the player picked, per view
    const DWORD dashFovAt       = 0x4D1DA8;   // mov edx, 80 -> cockpit projection FOV
    const DWORD pCopterCount    = 0x681DD8;
    const DWORD pCopterList     = 0x681DD4;

    struct Sig { DWORD addr; const char* bytes; int len; };
    const Sig kSigs[] = {
        { 0x548850, "\x53\x56\x57\x8B\x15\x2C\x6D\x9E\x00\xA1\x00\x6D", 12 }, // timer callback
        { 0x548A46, "\xA3\x00\x6D\x9E\x00", 5 },                              // period store
        { 0x459E7B, "\xB8\x10\x9E\x45\x00", 5 },                              // register handler
        { 0x459EBF, "\xB8\x10\x9E\x45\x00", 5 },                              // unregister handler
        { 0x51C7AE, "\xA1\xB0\x13\x68\x00", 5 },                              // c64 in main loop
        { 0x51BBE0, "\x53\x51\x52\x56\x57\x55\x89\xE5", 8 },                  // sim step
        { 0x51CA61, "\xE8\x7A\xF1\xFF\xFF", 5 },
        { 0x51CA70, "\xE8\x6B\xF1\xFF\xFF", 5 },
        { 0x51CA75, "\xE8\x66\xF1\xFF\xFF", 5 },
        { 0x51CAC3, "\xE8\x18\xF1\xFF\xFF", 5 },
        { 0x5113E0, "\x53\x51\x52\x55\x89\xE5\x83\xEC", 8 },                  // render
        { 0x51CB64, "\x8B\x3D\x7C\x3B\x85\x00\x85\xFF\x75\x1A", 10 },         // skip-render branch
        { 0x51CB79, "\x89\x7D\xFC", 3 },
        { 0x51CB7E, "\xA1\x24\x3B\x85\x00\xE8\x58\x48\xFF\xFF", 10 },         // render call
        { 0x51CB88, "\x83\x3D\xEC\x4B\x7C\x00\x04", 7 },
        { 0x51C7CB, "\xA1\x30\x3B\x85\x00", 5 },                              // sim target
        { 0x51C9AE, "\x8B\x35\x30\x3B\x85\x00", 6 },
        { 0x51C9EF, "\xF6\x05\x34\x3B\x85\x00\x01", 7 },                      // step index
        { 0x44444C, "\x89\x04\x95\x88\x1A\x66\x00", 7 },                      // car list
        { 0x50FCAE, "\x8B\x35\x60\x1A\x66\x00", 6 },                          // car count
        { 0x50F8E9, "\x83\xC0\x08", 3 },                                      // car+8 position
        { 0x50F94C, "\x83\xC0\x34", 3 },                                      // car+0x34 rotation
        { 0x43CCC5, "\x89\xB4\xD0\xF0\x0D\x66\x00", 7 },                      // camera table
        { 0x43BE6A, "\x8D\x70\x14", 3 },                                      // cam+0x14 position
        { 0x43BF48, "\x8D\x70\x2C", 3 },                                      // cam+0x2C rotation
        { 0x43C30D, "\x8B\x80\x58\x19\x66\x00", 6 },                          // camera override
        { 0x43C315, "\x8B\x80\x50\x19\x66\x00", 6 },                          // active camera
        { 0x50FDC1, "\x3B\x05\xD8\x1D\x68\x00", 6 },                          // copter count
        { 0x50FDCF, "\x8B\x04\x85\xD4\x1D\x68\x00", 7 },                      // copter list
        { 0x4416E2, "\x89\x1C\x85\x68\x19\x66\x00", 7 },                      // look mode
        { 0x538179, "\xFF\x15\xF0\x7F\x9E\x00\xFF\x15\xE0\x7F\x9E\x00\xA1\x54\x6D\x9E\x00"
                    "\x89\x45\xF8\xA1\xBC\xA5\x8A\x00\x83\xC0\x02\x3B\x45\xF8\x7E\x0F"
                    "\xA1\x54\x6D\x9E\x00\x89\x45\xF8\xE8\xB9\xE2\xFE\xFF\xEB\xE4\x8B\x45\xF8", 51 }, // menu present + wait
        { 0x4E7B80, "\x53\x51\x52\x56\x57\x55\x89\xE5\x83\xEC\x0C\x31\xC0\xE8", 14 },   // loading bar
        { 0x4E7F8F, "\xC2\x04\x00", 3 },
        { 0x539E10, "\x52\x55\x89\xE5\x83\xEC\x04\xBA\x5A\x00\x00\x00", 12 },       // GetViewFov (90)
        { 0x4D1DA8, "\xBA\x50\x00\x00\x00\xB8\x10\x3A\x85\x00", 10 },          // cockpit FOV 80
        { 0x43DEBE, "\x89\x04\x95\xA8\x3C\x7C\x00", 7 },                       // selected camera slot
        { 0x528C43, "\xF6\x05\xC0\x87\x82\x00\x08\x74\x35", 9 },                // menu colours: D3D halves them
        { 0x5361CD, "\xF6\x05\xC0\x87\x82\x00\x08\x0F\x84\xA1\x00\x00\x00", 13 },
        { 0x49E40E, "\x8A\x1D\xC0\x87\x82\x00\xF6\xC3\x08\x0F\x85\x59\x02\x00\x00", 15 }, // menu glow
        { 0x4AAF39, "\x8A\x0D\xC0\x87\x82\x00\xF6\xC1\x08\x75\x05", 11 },
        { 0x4AAF9B, "\x83\xFE\x02\x7D\x07\xC7\x45\xEC\x02\x00\x00\x00", 12 },   // menu item reveal: min 2 ticks
        { 0x539F8A, "\x89\x45\xFC", 3 },                                      // view -> [ebp-4]
        { 0x539FA6, "\xE8\x65\xFE\xFF\xFF", 5 },
        { 0x539FCB, "\xE8\x40\xFE\xFF\xFF", 5 },
        { 0x539FEB, "\xE8\x20\xFE\xFF\xFF", 5 },
    };
}

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------
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
    int   exeCheck         = 1;     // 1 = turn the mod off if nfs4.exe differs, 0 = patch anyway
    int   menuMaxFPS       = -1;    // -1 = auto (<= 128, divides the refresh rate), 0 = none
    int   logToFile        = 1;
    int   logEverySeconds  = 10;
    // widescreen (v0.7)
    int   widescreen       = 1;
    int   wsMenus          = 1;     // 0 = original 640x480 mode, 1 = 4:3 centered, 2 = stretched
    int   wsMenuResolution = 1;     // 1 = race resolution, 2 = desktop resolution
    int   wsFixFOV         = 1;     // Hor+ 3D
    int   wsHud            = 1;     // 0 = original (stretched), 1 = 4:3 at the screen edges, 2 = 4:3 centered
    int   wsMovies         = 1;     // scale FMVs with the menus
    int   wsUnlockResolutions = 1;  // list non-4:3 display modes
    int   wsLog            = 1;
};
static Config g_cfg;
static char g_asiDir[MAX_PATH] = {};

static void InitAsiDir() {
    HMODULE self = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(&InitAsiDir), &self) &&
        GetModuleFileNameA(self, g_asiDir, MAX_PATH)) {
        char* s = strrchr(g_asiDir, '\\');
        if (s) { s[1] = 0; return; }
    }
    g_asiDir[0] = 0;
}

static const char kNewKeysBlock[] =
    "; --- loading: redraw the loading bar at most every N ms (0 = every call) ---\n"
    "speedUpLoading=1\n"
    "loadScreenRenderMs=100\n"
    "; --- camera FOV (degrees added to the game's FOV; negative = narrower) ---\n"
    "; fovOffset applies to every race camera, the others to one camera each\n"
    "fovOffset=0\n"
    "fovOffsetInCar=0\n"
    "fovOffsetBumper=0\n"
    "fovOffsetHeli=0\n"
    "fovOffsetRoof=0\n"
    "; in race: F8 = wider, F7 = narrower, F6 = reset, for the camera you're in (saved here)\n"
    "; keys are Windows virtual-key codes (F1..F12 = 0x70..0x7B; avoid F10)\n"
    "fovHotkeys=1\n"
    "fovKeyUp=0x77\n"
    "fovKeyDown=0x76\n"
    "fovKeyReset=0x75\n"
    "fovStep=1\n";
static const char kGlideBlock[] =
    "; --- menus: the extra effects the game only draws on its Glide renderer (glow on\n"
    ";     menu items, full-brightness highlights, ...), also on Direct3D drivers ---\n"
    "glideMenuEffects=1\n";
static const char kMenuMaxBlock[] =
    "; --- menus: frame rate ceiling. The menu code counts time in 1/128 s ticks and some\n"
    ";     animations (list slide, video walls) speed up above 128 fps.\n"
    ";     -1 = auto: the highest rate <= 128 that divides your refresh rate (120 Hz -> 120,\n"
    ";     144 Hz -> 72, 240 Hz -> 120); 0 = no ceiling; N = at most N fps ---\n"
    "menuMaxFPS=-1\n";
static const char kExeCheckBlock[] =
    "; --- exe check: the mod compares the bytes it patches with the nfs4.exe it was made\n"
    ";     for. 1 = if anything differs, turn the mod off; 0 = patch anyway (every\n"
    ";     difference is still written to the log). Only use 0 with a modified exe you\n"
    ";     know is the same version - patching the wrong code can crash the game ---\n"
    "exeCheck=1\n";
static const char kWsBlock[] =
    "; --- widescreen (v0.7): pick any resolution in the game's options ---\n"
    "widescreen=1\n"
    "; menus are 640x480 pictures: 1 = shown 4:3 in the middle of the screen, 2 = stretched\n"
    ";   to fill it, 0 = original (the display switches to 640x480 for the menus)\n"
    "wsMenus=1\n"
    "; resolution the menus run at: 1 = same as races (no mode switch), 2 = desktop resolution\n"
    "wsMenuResolution=1\n"
    "; 3D view: keep the 4:3 vertical view and show more at the sides (Hor+)\n"
    "wsFixFOV=1\n"
    "; race HUD: 1 = 4:3 proportions with side elements at the screen edges, 2 = 4:3 in the\n"
    ";   middle, 0 = original (stretched sideways)\n"
    "wsHud=1\n"
    "; scale the intro/FMV movies like the menus\n"
    "wsMovies=1\n"
    "; list display modes that are not 4:3 in the options (the game hides them)\n"
    "wsUnlockResolutions=1\n"
    "wsLog=1\n";
static char g_iniPath[MAX_PATH] = {};

static void LoadConfig() {
    InitAsiDir();
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%sHSFrameFix.ini", g_asiDir);
    strcpy(g_iniPath, path);
    FILE* f = fopen(path, "r");
    if (!f) {
        f = fopen(path, "w");
        if (f) {
            fputs("; HSFrameFix.ini (v" HSFF_VERSION ")\n"
                  "; render at the display's refresh rate, blending car and camera motion\n"
                  "; between the game's fixed 64 Hz simulation steps\n"
                  "interpolate=1\n"
                  "; also smooth the police helicopter's movement\n"
                  "interpolateCopter=1\n"
                  "; run the game's 128 Hz timer from a precise clock (needed for smooth blending)\n"
                  "preciseTicks=1\n"
                  "; frame limiter (0 = off, use vsync). Set to your refresh rate if you run without vsync\n"
                  "targetFPS=0\n"
                  "; don't blend jumps bigger than this per 1/64 s step (resets, teleports, camera cuts)\n"
                  "maxStepDistance=10\n"
                  "maxStepAngle=90\n"
                  "; how many 1/64 s steps behind the newest simulation step frames are drawn\n"
                  "; (1.0 = smooth with the least delay; raise to 1.5 if you still see hitches)\n"
                  "interpDelay=1.0\n"
                  "; menus: drop the game's 64 fps menu cap\n"
                  "unlockMenus=1\n"
                  "; menu frame limiter (-1 = same as targetFPS, 0 = off/vsync)\n"
                  "menuTargetFPS=-1\n"
                  "logToFile=1\n"
                  "logEverySeconds=10\n", f);
            fputs(kNewKeysBlock, f);
            fputs(kGlideBlock, f);
            fputs(kMenuMaxBlock, f);
            fputs(kExeCheckBlock, f);
            fputs(kWsBlock, f);
            fclose(f);
        }
        return;
    }
    char line[256];
    bool sawNew = false, sawGlide = false, sawMenuMax = false, sawExeCheck = false, sawWs = false;
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == ';' || line[0] == '#' || line[0] == '\r' || line[0] == '\n') continue;
        char key[64] = {}, val[64] = {};
        if (sscanf(line, " %63[^= \t] = %63s", key, val) < 2) continue;
        if      (!_stricmp(key, "interpolate"))       g_cfg.interpolate = atoi(val);
        else if (!_stricmp(key, "interpolateCopter")) g_cfg.interpolateCopter = atoi(val);
        else if (!_stricmp(key, "preciseTicks"))      g_cfg.preciseTicks = atoi(val);
        else if (!_stricmp(key, "targetFPS"))         g_cfg.targetFPS = atoi(val);
        else if (!_stricmp(key, "maxStepDistance"))   g_cfg.maxStepDistance = (float)atof(val);
        else if (!_stricmp(key, "maxStepAngle"))      g_cfg.maxStepAngle = (float)atof(val);
        else if (!_stricmp(key, "interpDelay"))       g_cfg.interpDelay = (float)atof(val);
        else if (!_stricmp(key, "unlockMenus"))       g_cfg.unlockMenus = atoi(val);
        else if (!_stricmp(key, "menuTargetFPS"))     g_cfg.menuTargetFPS = atoi(val);
        else if (!_stricmp(key, "speedUpLoading"))    { g_cfg.speedUpLoading = atoi(val); sawNew = true; }
        else if (!_stricmp(key, "loadScreenRenderMs")) g_cfg.loadScreenRenderMs = atoi(val);
        else if (!_stricmp(key, "fovOffset"))         g_cfg.fovOffset = atoi(val);
        else if (!_stricmp(key, "fovOffsetInCar"))    g_cfg.fovOffsetInCar = atoi(val);
        else if (!_stricmp(key, "fovOffsetBumper"))   g_cfg.fovOffsetBumper = atoi(val);
        else if (!_stricmp(key, "fovOffsetHeli"))     g_cfg.fovOffsetHeli = atoi(val);
        else if (!_stricmp(key, "fovOffsetRoof"))     g_cfg.fovOffsetRoof = atoi(val);
        else if (!_stricmp(key, "fovHotkeys"))        g_cfg.fovHotkeys = atoi(val);
        else if (!_stricmp(key, "fovKeyUp"))          g_cfg.fovKeyUp = (int)strtol(val, nullptr, 0);
        else if (!_stricmp(key, "fovKeyDown"))        g_cfg.fovKeyDown = (int)strtol(val, nullptr, 0);
        else if (!_stricmp(key, "fovKeyReset"))       g_cfg.fovKeyReset = (int)strtol(val, nullptr, 0);
        else if (!_stricmp(key, "fovStep"))           g_cfg.fovStep = atoi(val);
        else if (!_stricmp(key, "glideMenuEffects"))  { g_cfg.glideMenuEffects = atoi(val); sawGlide = true; }
        else if (!_stricmp(key, "menuMaxFPS"))        { g_cfg.menuMaxFPS = atoi(val); sawMenuMax = true; }
        else if (!_stricmp(key, "exeCheck"))          { g_cfg.exeCheck = atoi(val); sawExeCheck = true; }
        else if (!_stricmp(key, "logToFile"))         g_cfg.logToFile = atoi(val);
        else if (!_stricmp(key, "logEverySeconds"))   g_cfg.logEverySeconds = atoi(val);
        else if (!_stricmp(key, "widescreen"))        { g_cfg.widescreen = atoi(val); sawWs = true; }
        else if (!_stricmp(key, "wsMenus"))           g_cfg.wsMenus = atoi(val);
        else if (!_stricmp(key, "wsMenuResolution"))  g_cfg.wsMenuResolution = atoi(val);
        else if (!_stricmp(key, "wsFixFOV"))          g_cfg.wsFixFOV = atoi(val);
        else if (!_stricmp(key, "wsHud"))             g_cfg.wsHud = atoi(val);
        else if (!_stricmp(key, "wsMovies"))          g_cfg.wsMovies = atoi(val);
        else if (!_stricmp(key, "wsUnlockResolutions")) g_cfg.wsUnlockResolutions = atoi(val);
        else if (!_stricmp(key, "wsLog"))             g_cfg.wsLog = atoi(val);
    }
    fclose(f);
    if (!sawNew) {                        // ini from an older version: add the new keys
        FILE* a = fopen(path, "a");
        if (a) { fputs("\n", a); fputs(kNewKeysBlock, a); fclose(a); }
    }
    if (!sawGlide) {
        FILE* a = fopen(path, "a");
        if (a) { fputs("\n", a); fputs(kGlideBlock, a); fclose(a); }
    }
    if (!sawMenuMax) {
        FILE* a = fopen(path, "a");
        if (a) { fputs("\n", a); fputs(kMenuMaxBlock, a); fclose(a); }
    }
    if (!sawExeCheck) {
        FILE* a = fopen(path, "a");
        if (a) { fputs("\n", a); fputs(kExeCheckBlock, a); fclose(a); }
    }
    if (!sawWs) {
        FILE* a = fopen(path, "a");
        if (a) { fputs("\n", a); fputs(kWsBlock, a); fclose(a); }
    }
}

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------
static FILE* g_log = nullptr;
static CRITICAL_SECTION g_logLock;
static bool g_logLockInit = false;

static void Logf(const char* fmt, ...) {
    if (!g_log) return;
    if (g_logLockInit) EnterCriticalSection(&g_logLock);
    va_list ap; va_start(ap, fmt);
    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(g_log, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fflush(g_log);
    if (g_logLockInit) LeaveCriticalSection(&g_logLock);
}

// Log lines from the game thread go through a queue and are written by a
// background thread, so the game thread never waits on the disk.
static CRITICAL_SECTION g_qLock;
static HANDLE g_qEvent = nullptr;
static char g_q[64][192];
static int g_qHead = 0, g_qTail = 0;
static void LogfAsync(const char* fmt, ...) {
    if (!g_log || !g_qEvent) return;
    char buf[192];
    SYSTEMTIME st; GetLocalTime(&st);
    int n = snprintf(buf, sizeof(buf), "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond,
                     st.wMilliseconds);
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf + n, sizeof(buf) - n, fmt, ap);
    va_end(ap);
    EnterCriticalSection(&g_qLock);
    int next = (g_qHead + 1) % 64;
    if (next != g_qTail) { memcpy(g_q[g_qHead], buf, sizeof(buf)); g_qHead = next; }
    LeaveCriticalSection(&g_qLock);
    SetEvent(g_qEvent);
}
static void DrainLogQueue() {
    for (;;) {
        char line[192];
        EnterCriticalSection(&g_qLock);
        bool any = g_qTail != g_qHead;
        if (any) { memcpy(line, g_q[g_qTail], sizeof(line)); g_qTail = (g_qTail + 1) % 64; }
        LeaveCriticalSection(&g_qLock);
        if (!any) break;
        if (!g_log) continue;
        if (g_logLockInit) EnterCriticalSection(&g_logLock);
        fputs(line, g_log); fflush(g_log);
        if (g_logLockInit) LeaveCriticalSection(&g_logLock);
    }
}

static void OpenLog() {
    if (!g_cfg.logToFile) return;
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%sHSFrameFix.log", g_asiDir);
    g_log = fopen(path, "w");
    Logf("HSFrameFix v" HSFF_VERSION " | interpolate=%d copter=%d preciseTicks=%d targetFPS=%d "
         "maxStepDistance=%.1f maxStepAngle=%.0f interpDelay=%.2f unlockMenus=%d menuTargetFPS=%d\n", g_cfg.interpolate,
         g_cfg.interpolateCopter, g_cfg.preciseTicks, g_cfg.targetFPS, g_cfg.maxStepDistance,
         g_cfg.maxStepAngle, g_cfg.interpDelay, g_cfg.unlockMenus, g_cfg.menuTargetFPS);
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static bool PatchBytes(DWORD at, const void* bytes, size_t len) {
    DWORD old;
    if (!VirtualProtect((void*)at, len, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy((void*)at, bytes, len);
    DWORD d; VirtualProtect((void*)at, len, old, &d);
    FlushInstructionCache(GetCurrentProcess(), (void*)at, len);
    return true;
}
static bool PatchRel32(DWORD at, BYTE op, void* target) {   // E8 call / E9 jmp
    BYTE b[5]; b[0] = op;
    DWORD rel = (DWORD)target - (at + 5);
    memcpy(b + 1, &rel, 4);
    return PatchBytes(at, b, 5);
}
static bool PatchPointer(void** slot, void* nv, void** ov) {
    DWORD old;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
    if (ov) *ov = *slot;
    *slot = nv;
    DWORD d; VirtualProtect(slot, sizeof(void*), old, &d);
    return true;
}
template <typename Fn>
static void ForEachImport(const char* dll, Fn cb) {
    BYTE* base = (BYTE*)GetModuleHandleA(nullptr);
    auto dos = (PIMAGE_DOS_HEADER)base;
    auto nt = (PIMAGE_NT_HEADERS)(base + dos->e_lfanew);
    DWORD rva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if (!rva) return;
    for (auto imp = (PIMAGE_IMPORT_DESCRIPTOR)(base + rva); imp->Name; ++imp) {
        if (_stricmp((const char*)(base + imp->Name), dll)) continue;
        auto th = (PIMAGE_THUNK_DATA)(base + imp->FirstThunk);
        auto on = (PIMAGE_THUNK_DATA)(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk
                                                                      : imp->FirstThunk));
        for (; th->u1.Function; ++th, ++on) {
            if (IMAGE_SNAP_BY_ORDINAL(on->u1.Ordinal)) continue;
            auto nm = (PIMAGE_IMPORT_BY_NAME)(base + on->u1.AddressOfData);
            cb((void**)&th->u1.Function, (const char*)nm->Name);
        }
    }
}
template <typename T> static inline T Rd(DWORD a) { return *(volatile T*)a; }

static bool g_knownExe = false;
static bool CheckExe() {
    if (GetModuleHandleA(nullptr) != (HMODULE)0x400000) {
        Logf("exe check: unexpected image base - disabled\n"); return false;
    }
    int bad = 0, total = 0;
    for (const auto& s : hs::kSigs) {
        total++;
        if (IsBadReadPtr((void*)s.addr, s.len)) {
            Logf("exe check: 0x%06lX unreadable\n", (unsigned long)s.addr); bad++; continue;
        }
        if (!memcmp((void*)s.addr, s.bytes, s.len)) continue;
        bad++;
        char exp[3 * 24 + 1] = {}, got[3 * 24 + 1] = {};
        const BYTE* g = (const BYTE*)s.addr;
        size_t n = s.len < 24 ? s.len : 24;
        for (size_t i = 0; i < n; i++) {
            bool d = g[i] != (BYTE)s.bytes[i];
            snprintf(exp + i * 3, 4, "%02X%c", (BYTE)s.bytes[i], d ? '*' : ' ');
            snprintf(got + i * 3, 4, "%02X%c", g[i], d ? '*' : ' ');
        }
        Logf("exe check: 0x%06lX differs (* = changed byte)\n    expected %s\n    found    %s\n",
             (unsigned long)s.addr, exp, got);
    }
    if (!bad) {
        // Two known builds share every byte the mod uses: the original 1999 release
        // (sha256 17a0f96c...) and the same exe with a widescreen patch, which hooks
        // the projection setup (0x53A086 jmp instead of fstp [0x5B0954]).
        const BYTE* w = (const BYTE*)0x53A086;
        const char* variant = (w[0] == 0xD9 && w[1] == 0x1D) ? "original release"
                            : (w[0] == 0xE9) ? "with widescreen patch" : "unknown variant";
        Logf("exe check: nfs4.exe matches (%d checks, %s)\n", total, variant);
        return true;
    }
    if (g_cfg.exeCheck) {
        Logf("exe check: %d of %d checks differ - not the analysed nfs4.exe, mod disabled "
             "(exeCheck=0 in the ini patches anyway)\n", bad, total);
        return false;
    }
    Logf("exe check: %d of %d checks differ - exeCheck=0, patching anyway\n", bad, total);
    return true;
}

// ---------------------------------------------------------------------------
// Time
// ---------------------------------------------------------------------------
static LARGE_INTEGER g_qpf;
static inline LONGLONG Qpc() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }
static inline double QpcMs(LONGLONG d) { return (double)d * 1000.0 / (double)g_qpf.QuadPart; }

static void WaitUntilOn(HANDLE timer, LONGLONG deadline) {
    for (;;) {
        LONGLONG now = Qpc(), rem = deadline - now;
        if (rem <= 0) return;
        double ms = QpcMs(rem);
        if (ms > 2.0 && timer) {
            LARGE_INTEGER due;
            due.QuadPart = -(LONGLONG)((ms - 1.5) * 10000.0);
            if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
                WaitForSingleObject(timer, INFINITE); continue;
            }
        }
        if (ms > 1.0) Sleep(0); else YieldProcessor();
    }
}
static HANDLE CreateHiResTimer() {
    HANDLE t = CreateWaitableTimerExA(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                      TIMER_ALL_ACCESS);
    if (!t) t = CreateWaitableTimerA(nullptr, FALSE, nullptr);
    return t;
}

// ---------------------------------------------------------------------------
// Precise 128 Hz scheduler (replaces the game's timeSetEvent one-shot chain)
// ---------------------------------------------------------------------------
typedef MMRESULT (WINAPI *timeSetEvent_t)(UINT, UINT, LPTIMECALLBACK, DWORD_PTR, UINT);
typedef MMRESULT (WINAPI *timeKillEvent_t)(UINT);
static timeSetEvent_t  g_origSetEvent = nullptr;
static timeKillEvent_t g_origKillEvent = nullptr;
static const UINT kFakeId = 0x48534646;
static HANDLE g_tickWake = nullptr, g_tickThread = nullptr;
static std::atomic<bool> g_tickArmed{false};
static std::atomic<UINT> g_tickFirstDelay{0};
static std::atomic<LONGLONG> g_lastGridTime{0};   // grid time of the callback being delivered
static std::atomic<unsigned long> g_tickFires{0}, g_tickResyncs{0};
static bool g_ticksActive = false;

static LONGLONG TickPeriodQpc() {
    DWORD fx = Rd<DWORD>(hs::pTimerPeriod);   // ms << 16
    if (fx < 0x10000 || fx > 1000u * 0x10000u) fx = (1000u << 16) / 128;
    return (LONGLONG)((double)fx / 65536.0 * (double)g_qpf.QuadPart / 1000.0 + 0.5);
}

static DWORD WINAPI TickThread(LPVOID) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    HANDLE timer = CreateHiResTimer();
    LONGLONG next = 0;
    auto cb = (void (WINAPI*)(UINT, UINT, DWORD_PTR, DWORD_PTR, DWORD_PTR))hs::fnTimerCallback;
    for (;;) {
        if (!g_tickArmed.load()) { WaitForSingleObject(g_tickWake, INFINITE); next = 0; continue; }
        LONGLONG period = TickPeriodQpc();
        LONGLONG now = Qpc();
        if (!next) next = now + (LONGLONG)g_tickFirstDelay.load() * g_qpf.QuadPart / 1000;
        if (now - next > period * 8) { next = now; g_tickResyncs++; }   // stalled: don't burst
        WaitUntilOn(timer, next);
        if (!g_tickArmed.exchange(false)) continue;   // killed while waiting
        g_lastGridTime = next;
        g_tickFires++;
        cb(kFakeId, 0, 0, 0, 0);                      // re-arms itself via timeSetEvent
        next += period;
    }
}

static MMRESULT WINAPI Hook_timeSetEvent(UINT delay, UINT res, LPTIMECALLBACK cb, DWORD_PTR user, UINT flags) {
    if ((DWORD)cb == hs::fnTimerCallback && g_ticksActive) {
        g_tickFirstDelay = delay;
        g_tickArmed = true;
        SetEvent(g_tickWake);
        return kFakeId;
    }
    return g_origSetEvent(delay, res, cb, user, flags);
}
static MMRESULT WINAPI Hook_timeKillEvent(UINT id) {
    if (id == kFakeId) { g_tickArmed = false; return TIMERR_NOERROR; }
    return g_origKillEvent(id);
}

static void InstallTickScheduler() {
    if (!g_knownExe || !g_cfg.preciseTicks) return;
    g_tickWake = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    bool s = false, k = false;
    ForEachImport("winmm.dll", [&](void** slot, const char* name) {
        void* old = nullptr;
        if (!strcmp(name, "timeSetEvent") && PatchPointer(slot, (void*)&Hook_timeSetEvent, &old)) {
            g_origSetEvent = (timeSetEvent_t)old; s = true;
        } else if (!strcmp(name, "timeKillEvent") && PatchPointer(slot, (void*)&Hook_timeKillEvent, &old)) {
            g_origKillEvent = (timeKillEvent_t)old; k = true;
        }
    });
    g_ticksActive = s && k && g_tickWake;
    if (g_ticksActive) g_tickThread = CreateThread(nullptr, 0, TickThread, nullptr, 0, nullptr);
    Logf("precise ticks: %s\n", g_ticksActive && g_tickThread ? "on" : "FAILED (game timer used)");
}

// ---------------------------------------------------------------------------
// Transform snapshots
// ---------------------------------------------------------------------------
struct Xform { float p[3]; float m[9]; };
struct Entry { DWORD obj; int kind; Xform x; };    // kind: 0 car, 1 camera, 2 copter (pos only)
static const int kMaxEntries = 128;
struct Snapshot { int n = 0; long step = 0; DWORD activeCam[2] = {0, 0}; long lookMode[2] = {0, 0};
                  Entry e[kMaxEntries]; };

static const int kRing = 4;
static Snapshot g_ring[kRing];   // g_ring[g_head] = latest
static int  g_head = 0;
static int  g_ringN = 0;         // valid snapshots (0..kRing)
static long g_lastStepIdx = 0;
// Smoothed sim clock: step index s is shown at time g_clockT0 + s * p64.
// g_clockT0 follows the steps' arrival times with a slow average, so frame-to-
// frame lateness (vsync, input batching) never moves the timeline by a whole step.
static LONGLONG g_clockT0 = 0;
static bool g_clockValid = false;
static std::atomic<unsigned long> g_clockSnaps{0};

static inline bool ReadXform(DWORD obj, int kind, Xform* x) {
    DWORD po = kind == 1 ? 0x14 : 0x08;
    if (IsBadReadPtr((void*)(obj + po), 12)) return false;
    memcpy(x->p, (void*)(obj + po), 12);
    if (kind != 2) {
        DWORD mo = kind == 1 ? 0x2C : 0x34;
        memcpy(x->m, (void*)(obj + mo), 36);
    }
    return true;
}
static inline void WriteXform(DWORD obj, int kind, const Xform& x) {
    DWORD po = kind == 1 ? 0x14 : 0x08;
    memcpy((void*)(obj + po), x.p, 12);
    if (kind != 2) memcpy((void*)(obj + (kind == 1 ? 0x2C : 0x34)), x.m, 36);
}

static DWORD EffectiveCam(int v) {
    DWORD o = Rd<DWORD>(hs::pCamOverride + v * 4);
    return o ? o : Rd<DWORD>(hs::pCamActive + v * 4);
}

static void Collect(Snapshot& s) {
    s.n = 0;
    s.activeCam[0] = EffectiveCam(0); s.activeCam[1] = EffectiveCam(1);
    s.lookMode[0] = Rd<long>(hs::pLookMode); s.lookMode[1] = Rd<long>(hs::pLookMode + 4);
    long nc = Rd<long>(hs::pCarCount);
    if (nc < 0) nc = 0;
    if (nc > 16) nc = 16;
    for (long i = 0; i < nc && s.n < kMaxEntries; i++) {
        DWORD car = Rd<DWORD>(hs::pCarList + i * 4);
        if (!car || IsBadReadPtr((void*)car, 0x60) || !Rd<BYTE>(car + 4)) continue;
        Entry& e = s.e[s.n]; e.obj = car; e.kind = 0;
        if (ReadXform(car, 0, &e.x)) s.n++;
    }
    DWORD seen[70]; int ns = 0;
    auto addCam = [&](DWORD cam) {
        if (!cam || s.n >= kMaxEntries) return;
        for (int k = 0; k < ns; k++) if (seen[k] == cam) return;
        if (ns < 70) seen[ns++] = cam;
        if (IsBadReadPtr((void*)cam, 0x50)) return;
        Entry& e = s.e[s.n]; e.obj = cam; e.kind = 1;
        if (ReadXform(cam, 1, &e.x)) s.n++;
    };
    for (int i = 0; i < 64; i++) addCam(Rd<DWORD>(hs::pCamTable + i * 4));
    for (int v = 0; v < 2; v++) { addCam(Rd<DWORD>(hs::pCamActive + v * 4));
                                  addCam(Rd<DWORD>(hs::pCamOverride + v * 4)); }
    if (g_cfg.interpolateCopter) {
        long nh = Rd<long>(hs::pCopterCount);
        if (nh < 0) nh = 0;
        if (nh > 8) nh = 8;
        for (long i = 0; i < nh && s.n < kMaxEntries; i++) {
            DWORD h = Rd<DWORD>(hs::pCopterList + i * 4);
            if (!h || IsBadReadPtr((void*)h, 0x14) || !Rd<BYTE>(h + 4)) continue;
            Entry& e = s.e[s.n]; e.obj = h; e.kind = 2;
            if (ReadXform(h, 2, &e.x)) s.n++;
        }
    }
}

static std::atomic<unsigned long> g_steps{0}, g_resets{0};

static LONGLONG StepPeriodQpc() { return TickPeriodQpc() * 2; }   // 64 Hz

extern "C" void __cdecl OnSimStep() {
    LONGLONG now = Qpc();
    long idx = Rd<long>(hs::pSimStepIdx);           // index of the step that just ran
    if (g_ringN && idx == g_lastStepIdx) {           // replay fast-forward: 2 steps per index
        Collect(g_ring[g_head]);
        g_steps++;
        return;
    }
    if (g_ringN && (idx < g_lastStepIdx || idx - g_lastStepIdx > 16)) {
        g_ringN = 0; g_clockValid = false; g_resets++;   // restart / jump: no blending across it
    }
    g_lastStepIdx = idx;
    g_head = (g_head + 1) % kRing;
    Snapshot& s = g_ring[g_head];
    Collect(s);
    s.step = idx;
    if (g_ringN < kRing) g_ringN++;

    LONGLONG p = StepPeriodQpc();
    LONGLONG t0 = now - (LONGLONG)idx * p;           // arrival-based origin of this step
    if (!g_clockValid) { g_clockT0 = t0; g_clockValid = true; }
    else {
        LONGLONG d = t0 - g_clockT0;
        if (d > 4 * p || d < -4 * p) { g_clockT0 = t0; g_clockSnaps++; }   // lost sync: snap
        else g_clockT0 += d / 32;                                         // slow follow
    }
    g_steps++;
}

// ---------------------------------------------------------------------------
// Interpolation math
// ---------------------------------------------------------------------------
struct Quat { float w, x, y, z; };
static Quat MatToQuat(const float* m) {   // m row-major 3x3
    Quat q; float tr = m[0] + m[4] + m[8];
    if (tr > 0) { float s = sqrtf(tr + 1.0f) * 2; q.w = 0.25f * s; q.x = (m[7] - m[5]) / s;
                  q.y = (m[2] - m[6]) / s; q.z = (m[3] - m[1]) / s; }
    else if (m[0] > m[4] && m[0] > m[8]) { float s = sqrtf(1.0f + m[0] - m[4] - m[8]) * 2;
                  q.w = (m[7] - m[5]) / s; q.x = 0.25f * s; q.y = (m[1] + m[3]) / s; q.z = (m[2] + m[6]) / s; }
    else if (m[4] > m[8]) { float s = sqrtf(1.0f + m[4] - m[0] - m[8]) * 2;
                  q.w = (m[2] - m[6]) / s; q.x = (m[1] + m[3]) / s; q.y = 0.25f * s; q.z = (m[5] + m[7]) / s; }
    else { float s = sqrtf(1.0f + m[8] - m[0] - m[4]) * 2;
                  q.w = (m[3] - m[1]) / s; q.x = (m[2] + m[6]) / s; q.y = (m[5] + m[7]) / s; q.z = 0.25f * s; }
    return q;
}
static void QuatToMat(const Quat& q, float* m) {
    float xx = q.x*q.x, yy = q.y*q.y, zz = q.z*q.z, xy = q.x*q.y, xz = q.x*q.z, yz = q.y*q.z,
          wx = q.w*q.x, wy = q.w*q.y, wz = q.w*q.z;
    m[0] = 1 - 2*(yy+zz); m[1] = 2*(xy-wz);     m[2] = 2*(xz+wy);
    m[3] = 2*(xy+wz);     m[4] = 1 - 2*(xx+zz); m[5] = 2*(yz-wx);
    m[6] = 2*(xz-wy);     m[7] = 2*(yz+wx);     m[8] = 1 - 2*(xx+yy);
}
static bool IsRotation(const float* m) {
    for (int r = 0; r < 3; r++) {
        float l = m[r*3]*m[r*3] + m[r*3+1]*m[r*3+1] + m[r*3+2]*m[r*3+2];
        if (fabsf(l - 1.0f) > 0.02f) return false;
    }
    float det = m[0]*(m[4]*m[8]-m[5]*m[7]) - m[1]*(m[3]*m[8]-m[5]*m[6]) + m[2]*(m[3]*m[7]-m[4]*m[6]);
    return fabsf(det - 1.0f) < 0.05f;
}
// returns false if the pair should not be blended
static bool Blend(const Xform& a, const Xform& b, int kind, float t, Xform* out) {
    float dx = b.p[0]-a.p[0], dy = b.p[1]-a.p[1], dz = b.p[2]-a.p[2];
    if (dx*dx + dy*dy + dz*dz > g_cfg.maxStepDistance * g_cfg.maxStepDistance) return false;
    for (int i = 0; i < 3; i++) out->p[i] = a.p[i] + (b.p[i] - a.p[i]) * t;
    if (kind == 2) return true;
    if (!memcmp(a.m, b.m, 36)) { memcpy(out->m, b.m, 36); return true; }
    if (!IsRotation(a.m) || !IsRotation(b.m)) {          // not a pure rotation: plain lerp
        for (int i = 0; i < 9; i++) out->m[i] = a.m[i] + (b.m[i] - a.m[i]) * t;
        return true;
    }
    Quat qa = MatToQuat(a.m), qb = MatToQuat(b.m);
    float d = qa.w*qb.w + qa.x*qb.x + qa.y*qb.y + qa.z*qb.z;
    if (d < 0) { qb.w = -qb.w; qb.x = -qb.x; qb.y = -qb.y; qb.z = -qb.z; d = -d; }
    if (d > 1) d = 1;
    float ang = 2.0f * acosf(d) * 57.29578f;
    if (ang > g_cfg.maxStepAngle) return false;
    Quat q;
    if (d > 0.9995f) {
        q.w = qa.w + (qb.w-qa.w)*t; q.x = qa.x + (qb.x-qa.x)*t;
        q.y = qa.y + (qb.y-qa.y)*t; q.z = qa.z + (qb.z-qa.z)*t;
    } else {
        float th = acosf(d), s = sinf(th), wa = sinf((1-t)*th)/s, wb = sinf(t*th)/s;
        q.w = qa.w*wa + qb.w*wb; q.x = qa.x*wa + qb.x*wb; q.y = qa.y*wa + qb.y*wb; q.z = qa.z*wa + qb.z*wb;
    }
    float l = sqrtf(q.w*q.w + q.x*q.x + q.y*q.y + q.z*q.z);
    if (l < 1e-6f) return false;
    q.w /= l; q.x /= l; q.y /= l; q.z /= l;
    QuatToMat(q, out->m);
    return true;
}

// ---------------------------------------------------------------------------
// Render wrapper
// ---------------------------------------------------------------------------
static Snapshot g_saved;          // live values while an interpolated frame is drawn
static const Entry* FindEntry(const Snapshot& s, DWORD obj, int kind) {
    for (int j = 0; j < s.n; j++) if (s.e[j].obj == obj && s.e[j].kind == kind) return &s.e[j];
    return nullptr;
}
static Xform    g_written[kMaxEntries];
static bool     g_wrote[kMaxEntries];
static bool g_applied = false;
static HANDLE g_limTimer = nullptr;
static LONGLONG g_limNext = 0;
static std::atomic<unsigned long> g_renders{0}, g_extraRenders{0}, g_blended{0}, g_rejected{0}, g_cuts{0};
static double g_alphaSum = 0; static unsigned long g_alphaN = 0, g_alphaClamped = 0;
// clamp details for the log: when the first one in the window happened and how far off it was
static volatile bool g_clampHave = false; static SYSTEMTIME g_clampFirst;
static double g_clampAheadMs = 0, g_clampBehindMs = 0;
static void NoteClamp(double steps) {             // >0: frame ahead of newest step, <0: behind history
    double ms = steps * 1000.0 / 64.0;
    if (!g_clampHave) { GetLocalTime(&g_clampFirst); g_clampHave = true; }
    if (ms > g_clampAheadMs) g_clampAheadMs = ms;
    if (-ms > g_clampBehindMs) g_clampBehindMs = -ms;
}
extern "C" { int g_extraEnabled = 0; DWORD g_fnSimStep = 0x51BBE0, g_fnRender = 0x5113E0,
                                     g_fnMenuIdle = 0x526460; }

// Frames reach the screen on the display's schedule, not when the CPU happens
// to start them, so the time used for blending follows a smoothed frame clock.
static LONGLONG g_ftLast = 0, g_ftInterval = 0;
static LONGLONG FrameTime() {
    LONGLONG now = Qpc();
    if (!g_ftLast) { g_ftLast = now; return now; }
    LONGLONG d = now - g_ftLast;
    if (!g_ftInterval) g_ftInterval = d;
    LONGLONG pred = g_ftLast + g_ftInterval;
    LONGLONG err = now - pred;
    LONGLONG ft;
    if (g_ftInterval > 0 && err < g_ftInterval / 2 && err > -g_ftInterval / 2) {
        ft = pred + err / 8;                                  // on schedule: follow gently
        g_ftInterval += (d - g_ftInterval) / 16;
    } else {
        ft = now;                                             // hitch / rate change: resync
        if (d > 0 && d < g_qpf.QuadPart / 10) g_ftInterval = d;
    }
    g_ftLast = ft;
    return ft;
}

static void LimitTo(int fps, LONGLONG& next) {
    if (fps <= 0) return;
    LONGLONG period = g_qpf.QuadPart / fps, now = Qpc();
    if (next && now < next) { WaitUntilOn(g_limTimer, next); now = Qpc(); }
    if (!next || now - next > period) next = now + period; else next += period;
}
static void Limiter() { LimitTo(g_cfg.targetFPS, g_limNext); }

static LONGLONG g_menuNext = 0;
static std::atomic<unsigned long> g_menuFrames{0};
// Menu frame ceiling: auto = the highest rate <= 128 that divides the refresh rate
// (re-read every 2 s, the game may change display mode).
static int MenuCeiling() {
    if (g_cfg.menuMaxFPS >= 0) return g_cfg.menuMaxFPS;
    static int cached = 0; static DWORD lastCheck = 0;
    DWORD now = GetTickCount();
    if (!cached || now - lastCheck > 2000) {
        lastCheck = now;
        DEVMODEA dm = {}; dm.dmSize = sizeof(dm);
        int hz = 0;
        if (EnumDisplaySettingsA(nullptr, ENUM_CURRENT_SETTINGS, &dm)) hz = (int)dm.dmDisplayFrequency;
        if (hz < 30 || hz > 1000) hz = 60;                 // "default"/unknown
        int c = hz;
        for (int n = 1; c > 128; n++) c = hz / (n + 1);
        if (c != cached) { cached = c; LogfAsync("menus: frame ceiling %d fps (display %d Hz)\n", c, hz); }
    }
    return cached;
}
extern "C" void __cdecl MenuFrame() {
    int fps = g_cfg.menuTargetFPS < 0 ? g_cfg.targetFPS : g_cfg.menuTargetFPS;
    int cap = MenuCeiling();
    if (cap > 0 && (fps <= 0 || fps > cap)) fps = cap;
    LimitTo(fps, g_menuNext);
    g_menuFrames++;
}

static void FovHotkeys();
extern "C" void __cdecl PreRender() {
    FovHotkeys();
    Limiter();
    g_renders++;
    g_applied = false;
    if (!g_cfg.interpolate || g_ringN < 2 || !g_clockValid) return;

    // Position on the smoothed sim timeline, one step (interpDelay) behind.
    LONGLONG p = StepPeriodQpc();
    double r = (double)(FrameTime() - g_clockT0) / (double)p - g_cfg.interpDelay;
    // Find consecutive snapshots A.step <= r < B.step (newest first).
    const Snapshot* A = nullptr; const Snapshot* B = nullptr;
    for (int k = 0; k + 1 < g_ringN; k++) {
        const Snapshot& nb = g_ring[(g_head - k + kRing) % kRing];
        const Snapshot& na = g_ring[(g_head - k - 1 + kRing) % kRing];
        if (k == 0 && r >= (double)nb.step) {
            g_alphaClamped++; g_alphaSum += 1; g_alphaN++; NoteClamp(r - (double)nb.step); return;
        }
        if (r >= (double)na.step) { A = &na; B = &nb; break; }
    }
    if (!A) {                                        // older than the history: oldest pair
        int k = g_ringN - 2;
        B = &g_ring[(g_head - k + kRing) % kRing];
        A = &g_ring[(g_head - k - 1 + kRing) % kRing];
        g_alphaClamped++;
        NoteClamp(r - (double)A->step);
        r = (double)A->step;
    }
    long span = B->step - A->step;
    if (span <= 0 || span > 8) return;
    float t = (float)((r - (double)A->step) / (double)span);
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    g_alphaSum += t; g_alphaN++;

    // Camera cut inside this pair (other camera, look back/left/right toggled,
    // or a jump too big to blend): draw the whole frame at the newest step so
    // the camera and the cars stay consistent.
    for (int v = 0; v < 2; v++) {
        if (A->activeCam[v] != B->activeCam[v] || A->lookMode[v] != B->lookMode[v]) { g_cuts++; return; }
        DWORD cam = B->activeCam[v];
        if (!cam) continue;
        const Entry* ca = FindEntry(*A, cam, 1);
        const Entry* cb = FindEntry(*B, cam, 1);
        Xform tmp;
        if (ca && cb && !Blend(ca->x, cb->x, 1, 0.5f, &tmp)) { g_cuts++; return; }
    }
    if (Rd<long>(hs::pLookMode) != B->lookMode[0] || Rd<long>(hs::pLookMode + 4) != B->lookMode[1] ||
        EffectiveCam(0) != B->activeCam[0] || EffectiveCam(1) != B->activeCam[1]) { g_cuts++; return; }

    // Current live values (normally == B) are saved and restored after drawing.
    Collect(g_saved);
    for (int i = 0; i < g_saved.n; i++) {
        g_wrote[i] = false;
        const Entry& cur = g_saved.e[i];
        if (cur.kind == 1) {                           // camera cut: don't blend into it
            bool cut = false;
            for (int v = 0; v < 2; v++)
                if (cur.obj == g_saved.activeCam[v] &&
                    (A->activeCam[v] != cur.obj || B->activeCam[v] != cur.obj)) cut = true;
            if (cut) { g_rejected++; continue; }
        }
        const Entry* pa = FindEntry(*A, cur.obj, cur.kind);
        const Entry* pb = FindEntry(*B, cur.obj, cur.kind);
        if (!pa || !pb) continue;
        Xform x;
        if (Blend(pa->x, pb->x, cur.kind, t, &x)) {
            WriteXform(cur.obj, cur.kind, x);
            g_written[i] = x; g_wrote[i] = true; g_blended++;
        }
        else g_rejected++;
    }
    g_applied = true;
}

extern "C" void __cdecl PostRender() {
    if (!g_applied) return;
    // Put the simulation values back - unless the render itself changed the
    // object (e.g. a camera switch re-initialises the new camera).
    for (int i = 0; i < g_saved.n; i++) {
        if (!g_wrote[i]) continue;
        const Entry& e = g_saved.e[i];
        Xform now;
        if (!ReadXform(e.obj, e.kind, &now)) continue;
        bool same = !memcmp(now.p, g_written[i].p, 12) &&
                    (e.kind == 2 || !memcmp(now.m, g_written[i].m, 36));
        if (same) WriteXform(e.obj, e.kind, e.x);
    }
    g_applied = false;
}

extern "C" void __cdecl NoteExtraRender() { g_extraRenders++; }

// ---------------------------------------------------------------------------
// Stubs (Watcom code: preserve every register and the flags)
// ---------------------------------------------------------------------------
extern "C" void StepStub();
extern "C" void RenderStub();
extern "C" void ExtraRenderStub();
extern "C" void MenuWaitStub();
extern "C" void LoadBarStub();
extern "C" void FovStub();
extern "C" void DashFovStub();
extern "C" void MenuTimeStub1();
asm(".text\n"
    // call site of the sim step: run it, then snapshot
    ".globl _StepStub\n"
    "_StepStub:\n"
    "    call *_g_fnSimStep\n"
    "    pushfl\n"
    "    pushal\n"
    "    call _OnSimStep\n"
    "    popal\n"
    "    popfl\n"
    "    ret\n"
    // call site of the render: eax = argument
    ".globl _RenderStub\n"
    "_RenderStub:\n"
    "    pushfl\n"
    "    pushal\n"
    "    call _PreRender\n"
    "    popal\n"
    "    popfl\n"
    "    call *_g_fnRender\n"         // eax = render argument, untouched
    "    pushfl\n"
    "    pushal\n"
    "    call _PostRender\n"
    "    popal\n"
    "    popfl\n"
    "    ret\n"
    // replaces "mov edi,[0x853B7C] / test edi,edi / jne 0x51CB88" (jumped to, same frame)
    ".globl _ExtraRenderStub\n"
    "_ExtraRenderStub:\n"
    "    movl 0x853B7C, %edi\n"
    "    testl %edi, %edi\n"
    "    jnz 1f\n"
    "    pushl $0x51CB6E\n"           // nothing changed: original render decision
    "    ret\n"
    "1:  cmpl $0, _g_extraEnabled\n"
    "    je 2f\n"
    "    cmpl $0, -4(%ebp)\n"         // special render request pending: leave it to the game
    "    jne 2f\n"
    "    pushfl\n"
    "    pushal\n"
    "    call _NoteExtraRender\n"
    "    popal\n"
    "    popfl\n"
    "    movl 0x853B24, %eax\n"
    "    call _RenderStub\n"
    "    movl 0x853B7C, %edi\n"
    "2:  pushl $0x51CB88\n"
    "    ret\n"
    // replaces the menu's "wait for 2 ticks" loop (jumped to, inside 0x538170)
    ".globl _MenuWaitStub\n"
    "_MenuWaitStub:\n"
    "    pushfl\n"
    "    pushal\n"
    "    call *_g_fnMenuIdle\n"      // the work the wait loop used to do
    "    call _MenuFrame\n"           // optional limiter
    "    popal\n"
    "    popfl\n"
    "    movl 0x9E6D54, %eax\n"
    "    movl %eax, -8(%ebp)\n"
    "    pushl $0x5381A9\n"
    "    ret\n"
    // entry of the loading bar (stack: ret, float percent)
    ".globl _LoadBarStub\n"
    "_LoadBarStub:\n"
    "    pushfl\n"
    "    pushal\n"
    "    pushl 40(%esp)\n"            // percent
    "    call _LoadBarDecide\n"
    "    addl $4, %esp\n"
    "    popal\n"
    "    popfl\n"
    "    cmpl $0, _g_loadDraw\n"
    "    jne 3f\n"
    "    pushal\n"                    // skipped: keep the callback poll it starts with
    "    xorl %eax, %eax\n"
    "    call *_g_fnPoll\n"
    "    popal\n"
    "    ret $4\n"
    "3:  pushl %ebx\n"                 // original prologue, then continue
    "    pushl %ecx\n"
    "    pushl %edx\n"
    "    pushl %esi\n"
    "    pushl %edi\n"
    "    pushl %ebp\n"
    "    pushl $0x4E7B86\n"
    "    ret\n"
    // calls to GetViewFov inside the view setup 0x539F80 (view in [ebp-4])
    ".globl _FovStub\n"
    "_FovStub:\n"
    "    call *_g_fnGetFov\n"
    "    pushfl\n"
    "    pushl %ecx\n"
    "    pushl %edx\n"
    "    pushl -4(%ebp)\n"
    "    pushl %eax\n"
    "    call _FovAdjust\n"
    "    addl $8, %esp\n"
    "    popl %edx\n"
    "    popl %ecx\n"
    "    popfl\n"
    "    ret\n"
    // menu animation timers, called in place of "mov [ebp-x], 2" (return redirected)
    ".globl _MenuTimeStub1\n"
    "_MenuTimeStub1:\n"
    "    cmpl $0, -0x14(%ebp)\n"
    "    jl 1f\n"
    "    movl $0x4AAFE6, (%esp)\n"
    "    ret\n"
    "1:  movl $2, -0x14(%ebp)\n"
    "    movl $0x4AAFA7, (%esp)\n"
    "    ret\n"
    // replaces "mov edx, 80" in the cockpit draw 0x4D1C80
    ".globl _DashFovStub\n"
    "_DashFovStub:\n"
    "    pushfl\n"
    "    pushl %eax\n"
    "    pushl %ecx\n"
    "    call _DashFov\n"
    "    movl %eax, %edx\n"
    "    popl %ecx\n"
    "    popl %eax\n"
    "    popfl\n"
    "    ret\n");

// ---------------------------------------------------------------------------
// Loading bar throttle
// ---------------------------------------------------------------------------
extern "C" { int g_loadDraw = 1; DWORD g_fnPoll = 0x54A790, g_fnGetFov = 0x539E10; }
static LONGLONG g_loadLastCall = 0, g_loadLastDraw = 0, g_loadStart = 0;
static unsigned long g_loadCalls = 0, g_loadDraws = 0;
static bool g_loadActive = false;
static CRITICAL_SECTION g_loadLock;

extern "C" void __cdecl LoadBarDecide(float pct) {
    LONGLONG now = Qpc();
    EnterCriticalSection(&g_loadLock);
    if (!g_loadActive || now - g_loadLastCall > 2 * g_qpf.QuadPart) {   // new load
        g_loadActive = true; g_loadStart = now; g_loadCalls = 0; g_loadDraws = 0; g_loadLastDraw = 0;
    }
    g_loadLastCall = now;
    g_loadCalls++;
    LONGLONG minGap = (LONGLONG)g_cfg.loadScreenRenderMs * g_qpf.QuadPart / 1000;
    bool draw = !g_cfg.speedUpLoading || g_cfg.loadScreenRenderMs <= 0 || !g_loadLastDraw ||
                pct >= 100.0f || now - g_loadLastDraw >= minGap;
    if (draw) { g_loadLastDraw = now; g_loadDraws++; }
    g_loadDraw = draw ? 1 : 0;
    LeaveCriticalSection(&g_loadLock);
}
static void LoadStatsTick() {                 // called by the stats thread
    EnterCriticalSection(&g_loadLock);
    if (g_loadActive && Qpc() - g_loadLastCall > 2 * g_qpf.QuadPart) {
        Logf("load: %lu loading-bar updates, %lu drawn, first->last %.0f ms\n", g_loadCalls,
             g_loadDraws, QpcMs(g_loadLastCall - g_loadStart));
        g_loadActive = false;
    }
    LeaveCriticalSection(&g_loadLock);
}

// ---------------------------------------------------------------------------
// Camera FOV
// ---------------------------------------------------------------------------
static int* FovSlot(int type) {
    switch (type) {
        case 1: return &g_cfg.fovOffsetInCar;
        case 2: return &g_cfg.fovOffsetBumper;
        case 3: case 5: return &g_cfg.fovOffsetHeli;
        case 4: case 6: return &g_cfg.fovOffsetRoof;
    }
    return nullptr;
}
static const char* FovKey(int type) {
    switch (type) {
        case 1: return "fovOffsetInCar";
        case 2: return "fovOffsetBumper";
        case 3: case 5: return "fovOffsetHeli";
        case 4: case 6: return "fovOffsetRoof";
    }
    return "fovOffset";
}
static const char* FovName(int type) {
    switch (type) {
        case 1: return "in-car"; case 2: return "bumper"; case 3: return "heli";
        case 4: return "roof"; case 5: return "static heli"; case 6: return "static roof";
    }
    return "other";
}
static std::atomic<int> g_fovLastType{-1};
static unsigned g_fovSeen = 0;
static volatile LONG g_iniDirty = 0;
static volatile int g_fovGame = 90, g_fovAdj = 90;   // last non-mirror view: game FOV, our FOV

static int CameraTypeForView(DWORD view, long* vidxOut = nullptr) {
    if (!view || IsBadReadPtr((void*)view, 8)) return -1;
    long vidx = Rd<long>(view + 4);
    if (Rd<long>(0x7C4BEC) == 1) vidx &= 1; else vidx = 0;   // same rule as the camera code
    if (vidxOut) *vidxOut = vidx;
    DWORD cam = EffectiveCam((int)vidx);
    if (!cam || IsBadReadPtr((void*)cam, 4)) return -1;
    return Rd<long>(cam);
}

extern "C" int __cdecl FovAdjust(int fov, DWORD view) {
    if (!view || IsBadReadPtr((void*)view, 8) || Rd<long>(view) == 1) return fov;   // mirror
    long vidx = 0;
    int type = CameraTypeForView(view, &vidx);
    // Crash / recovery / other game-driven cameras: keep the offset of the camera
    // the player picked (the game restores that one afterwards, 0x43DEA0).
    int offType = type;
    if (!FovSlot(type)) {
        int sel = Rd<long>(hs::pSelectedCam + vidx * 4);
        if (FovSlot(sel)) offType = sel;
    }
    int add = g_cfg.fovOffset;
    if (int* p = FovSlot(offType)) add += *p;
    if (type >= 0 && type < 32 && !(g_fovSeen & (1u << type))) {
        g_fovSeen |= 1u << type;
        if (offType != type)
            LogfAsync("camera type %d (%s): game FOV %d, offset %+d (from the selected %s camera)\n",
                      type, FovName(type), fov, add, FovName(offType));
        else
            LogfAsync("camera type %d (%s): game FOV %d, offset %+d\n", type, FovName(type), fov, add);
    }
    g_fovLastType = offType;
    int f = fov + add;
    if (f < 20) f = 20;
    if (f > 150) f = 150;
    g_fovGame = fov; g_fovAdj = f;
    return f;
}

// The cockpit model is drawn in its own pass with a fixed 80 degree FOV, on top of
// the world. Scale it by the same tan ratio as the world so both stay aligned.
extern "C" int __cdecl DashFov() {
    const int base = 80;
    int g = g_fovGame, a = g_fovAdj;
    if (g == a || g <= 0 || g >= 180 || a <= 0 || a >= 180) return base;
    const double d2r = 3.14159265358979 / 360.0;   // degrees -> half-angle radians
    double t = tan(base * d2r) * tan(a * d2r) / tan(g * d2r);
    int f = (int)floor(atan(t) / d2r + 0.5);
    if (f < 20) f = 20;
    if (f > 170) f = 170;
    static int logged = -1;
    if (f != logged) { logged = f; LogfAsync("cockpit FOV %d (world %d -> %d)\n", f, g, a); }
    return f;
}

static void ReplaceIniLine(const char* key, int value) {
    if (!g_iniPath[0]) return;
    FILE* f = fopen(g_iniPath, "r");
    if (!f) return;
    static char buf[32768];
    size_t n = 0; char line[256]; bool done = false; size_t kl = strlen(key);
    while (fgets(line, sizeof(line), f)) {
        char out[256];
        const char* p = line; while (*p == ' ' || *p == '\t') p++;
        if (!done && !_strnicmp(p, key, kl) && (p[kl] == '=' || p[kl] == ' ' || p[kl] == '\t')) {
            snprintf(out, sizeof(out), "%s=%d\n", key, value); done = true;
        } else snprintf(out, sizeof(out), "%s", line);
        size_t l = strlen(out);
        if (n + l < sizeof(buf)) { memcpy(buf + n, out, l); n += l; }
    }
    fclose(f);
    if (!done) { char out[128]; snprintf(out, sizeof(out), "%s=%d\n", key, value);
                 size_t l = strlen(out); if (n + l < sizeof(buf)) { memcpy(buf + n, out, l); n += l; } }
    f = fopen(g_iniPath, "w");
    if (f) { fwrite(buf, 1, n, f); fclose(f); }
}

static bool GameHasFocus() {
    HWND w = GetForegroundWindow(); DWORD pid = 0;
    if (w) GetWindowThreadProcessId(w, &pid);
    return pid == GetCurrentProcessId();
}
static void FovHotkeys() {
    if (!g_cfg.fovHotkeys) return;
    static bool prev[3] = {};
    const int keys[3] = { g_cfg.fovKeyUp, g_cfg.fovKeyDown, g_cfg.fovKeyReset };
    bool focus = GameHasFocus();
    for (int k = 0; k < 3; k++) {
        bool down = focus && keys[k] > 0 && keys[k] < 256 && (GetAsyncKeyState(keys[k]) & 0x8000);
        if (down && !prev[k]) {
            int type = g_fovLastType;
            int* slot = FovSlot(type);
            if (!slot) slot = &g_cfg.fovOffset;
            if (k == 0) *slot += g_cfg.fovStep; else if (k == 1) *slot -= g_cfg.fovStep; else *slot = 0;
            if (*slot > 60) *slot = 60;
            if (*slot < -50) *slot = -50;
            g_iniDirty = 1;                       // saved by the writer thread
            if (g_qEvent) SetEvent(g_qEvent);
            LogfAsync("FOV hotkey: %s offset now %+d\n", FovSlot(type) ? FovName(type) : "global", *slot);
        }
        prev[k] = down;
    }
}

// Saves FOV changes to the ini and writes queued log lines, off the game thread.
static DWORD WINAPI WriterThread(LPVOID) {
    for (;;) {
        WaitForSingleObject(g_qEvent, 1000);
        DrainLogQueue();
        if (g_iniDirty) {
            Sleep(500);                          // let a burst of key presses settle
            g_iniDirty = 0;
            ReplaceIniLine("fovOffset", g_cfg.fovOffset);
            ReplaceIniLine("fovOffsetInCar", g_cfg.fovOffsetInCar);
            ReplaceIniLine("fovOffsetBumper", g_cfg.fovOffsetBumper);
            ReplaceIniLine("fovOffsetHeli", g_cfg.fovOffsetHeli);
            ReplaceIniLine("fovOffsetRoof", g_cfg.fovOffsetRoof);
            DrainLogQueue();
        }
    }
}

static void InstallMenuUnlock() {
    if (!g_knownExe || !g_cfg.unlockMenus) { Logf("menus: original 64 fps cap\n"); return; }
    if (!g_limTimer) g_limTimer = CreateHiResTimer();
    bool ok = PatchRel32(hs::menuWaitAt, 0xE9, (void*)&MenuWaitStub);
    // The menu item reveal (and its Glide glow) advances by half the 128 Hz ticks
    // since the last frame but counts at least 2: right at 64 fps, too fast above.
    // When 0..1 tick has passed, skip the update and let the ticks add up instead;
    // negative (tick counter reset) keeps the original "count 2 and resync".
    // (Two other menu timers with the same clamp are left alone: they only differ
    // above 128 fps, and the list slide treats a zero step as "arrived".)
    bool ok2 = PatchRel32(0x4AAFA0, 0xE8, (void*)&MenuTimeStub1);
    Logf("menus: %s, menu animation timing %s\n", ok ? "unlocked" : "patch FAILED",
         ok2 ? "fixed" : "patch FAILED");
}

// The front end checks the renderer class ([0x8287C0], from the driver's thrash.ini
// Type=: d=8 Direct3D, v=4 Glide, s=1 software) in four places and drops effects on
// Direct3D. Make those four behave as on Glide; software keeps its own checks.
static void InstallGlideMenuEffects() {
    if (!g_knownExe || !g_cfg.glideMenuEffects) { Logf("menu effects: original\n"); return; }
    static const BYTE jmp8[]   = { 0xEB };                               // 0x528C4A: je -> jmp
    static const BYTE jmp32[]  = { 0xE9, 0xA2, 0x00, 0x00, 0x00, 0x90 }; // 0x5361D4: je -> jmp 0x53627B
    static const BYTE nop6[]   = { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 }; // 0x49E417: drop "jne if D3D"
    static const BYTE nop2[]   = { 0x90, 0x90 };                         // 0x4AAF42: drop "jne if D3D"
    bool ok = PatchBytes(0x528C4A, jmp8, 1) && PatchBytes(0x5361D4, jmp32, 6) &&
              PatchBytes(0x49E417, nop6, 6) && PatchBytes(0x4AAF42, nop2, 2);
    Logf("menu effects: %s (Glide-style menu glow and colours on any renderer)\n",
         ok ? "on" : "patch FAILED");
}

static void InstallLoadingAndFov() {
    InitializeCriticalSection(&g_loadLock);
    if (!g_knownExe) return;
    if (g_cfg.speedUpLoading) {
        bool ok = PatchRel32(hs::fnLoadBar, 0xE9, (void*)&LoadBarStub);
        Logf("loading: %s (loading bar redrawn at most every %d ms)\n",
             ok ? "sped up" : "patch FAILED", g_cfg.loadScreenRenderMs);
    } else Logf("loading: original\n");
    bool ok = true;
    for (DWORD at : hs::callGetFov) ok &= PatchRel32(at, 0xE8, (void*)&FovStub);
    ok &= PatchRel32(hs::dashFovAt, 0xE8, (void*)&DashFovStub);
    Logf("camera FOV: %s (all %+d, in-car %+d, bumper %+d, heli %+d, roof %+d, hotkeys %s)\n",
         ok ? "hooked" : "patch FAILED", g_cfg.fovOffset, g_cfg.fovOffsetInCar, g_cfg.fovOffsetBumper,
         g_cfg.fovOffsetHeli, g_cfg.fovOffsetRoof, g_cfg.fovHotkeys ? "on" : "off");
}

static void InstallInterpolation() {
    if (!g_knownExe || !g_cfg.interpolate) { Logf("interpolation: off\n"); return; }
    bool ok = true;
    for (DWORD at : hs::callSimStep) ok &= PatchRel32(at, 0xE8, (void*)&StepStub);
    ok &= PatchRel32(hs::callRender, 0xE8, (void*)&RenderStub);
    BYTE j[6] = { 0xE9, 0, 0, 0, 0, 0x90 };
    DWORD rel = (DWORD)&ExtraRenderStub - (hs::skipRenderAt + 5);
    memcpy(j + 1, &rel, 4);
    ok &= PatchBytes(hs::skipRenderAt, j, 6);
    g_extraEnabled = ok ? 1 : 0;
    g_limTimer = CreateHiResTimer();
    Logf("interpolation: %s\n", ok ? "installed" : "patch FAILED");
}

// ---------------------------------------------------------------------------
// Widescreen (v0.7)
//
// Menus: the front end always runs in display mode 1 (640x480; [0x5BEF9C]) and
// draws in 640x480 pixels. The menu mode set (0x51A710 from 0x4F773B / 0x504999)
// asks THRASH_setvideomode for the race resolution instead; the game still
// believes it is 640x480, and every pre-transformed vertex, viewport and clear
// going to the Direct3D 7 device is mapped from 640x480 to a centered 4:3 area
// (or stretched). Bars are cleared before each page flip.
// Race: the mode list hides non-4:3 modes (0x51A4AF: w*3 == h*4); that check is
// skipped. 3D: the projection (0x453F10, from 0x539E60) and the fixed-point
// projection / frustum (0x560350) use the view width for a fixed horizontal
// FOV - for views as wide as the screen the horizontal angle is widened so the
// vertical view stays as on a 4:3 screen (Hor+), also for zoomed cameras
// (0x53A04C). HUD: its scales come from the screen width (0x4CE753 and three
// 1/640 constants, plus sizes 9/14/10 and an element at x=224 in 640 units);
// those use the 4:3 width instead, and the element rectangles of the layout
// file (0x7CB0BC, built by 0x4D0550) are placed in 4:3 proportions at the left,
// right or middle of the screen. The rear-view mirror uses one of them.
// A widescreen patch already in the exe (jumps at the sites below) is undone first.
// ---------------------------------------------------------------------------
namespace ws {
    const DWORD fnSetMode       = 0x51A710;
    const DWORD callModeRace    = 0x4F73DB;
    const DWORD callModeFE[2]   = { 0x4F773B, 0x504999 };
    const DWORD fnSelectedMode  = 0x487840;
    const DWORD pModeW = 0x8539A4, pModeH = 0x8539A8;   // mode the game believes in
    const DWORD pScrW  = 0x853A98, pScrH  = 0x853A9C;   // full-screen view rect w/h
    const DWORD modeFilterRel   = 0x51A4B6;             // low byte of "jmp 0x51A3E8" (2E)
    const DWORD callFrustum     = 0x539EC0, fnFrustumUnits = 0x560350;
    const DWORD callProj        = 0x539F23, fnProj = 0x453F10;
    const DWORD zoomStore       = 0x53A04C;             // fstp [0x5B0950] (6 bytes)
    const DWORD pFocalX = 0x5B0950, pFocalY = 0x5B0954;
    const DWORD hudWidthLoad    = 0x4CE753;             // fild [0x853A98] (6 bytes)
    const DWORD hudInv640[3]    = { 0x59967F, 0x5998CB, 0x599953 };
    const DWORD hudSizes[3]     = { 0x599CCB, 0x599CD7, 0x599CDB };
    const DWORD hudX224[2]      = { 0x4D0A30, 0x4D0B66 }; // float immediates (224.0)
    const DWORD callRects       = 0x4CE7D6, fnRects = 0x4D0550;
    const DWORD pRects          = 0x7CB0BC;

    struct Restore { DWORD addr; const char* orig; int len; };
    const Restore kOtherPatch[] = {        // original bytes at the other widescreen patch's sites
        { 0x453F5D, "\xDD\xD8\xD8\x7D\x10", 5 },
        { 0x46D71C, "\xA3\x0C\x95\x68\x00", 5 },
        { 0x4CE753, "\xDB\x05\x98\x3A\x85\x00", 6 },
        { 0x4D05D1, "\xD8\x8C\x02\xC0\xB0\x7C\x00", 7 },
        { 0x4D0606, "\xD8\x8C\x02\xC8\xB0\x7C\x00", 7 },
        { 0x4D0A2D, "\xC7\x45\xF8\x00\x00\x60\x43", 7 },
        { 0x4D0B65, "\xB9\x00\x00\x60\x43", 5 },
        { 0x51A428, "\x8B\x45\xE0\x8B\x40\x40", 6 },
        { 0x53A086, "\xD9\x1D\x54\x09\x5B\x00", 6 },
        { 0x562B4C, "\x00\x00\x00\x00", 4 },
    };
}

static bool  g_wsOn = false;
static int   g_dispW = 0, g_dispH = 0;          // desktop at startup
// menu mapping 640x480 -> screen
static volatile bool g_xf = false;
static float g_xsx = 1, g_xsy = 1, g_xox = 0, g_xoy = 0;
static int   g_realW = 0, g_realH = 0, g_realBpp = 16;
static int   g_feCall = 0, g_feTarget = 0;
static std::atomic<unsigned long> g_wsXfDraws{0}, g_wsVbDraws{0}, g_wsBarClears{0}, g_wsMovieFrames{0};
// race
static float g_projR = 1.0f;                   // Hor+ factor of the view being set up
static float g_hudW  = 0.0f;                   // width the HUD lays out in
static float g_hudInv640Orig[3], g_hudSizesOrig[3];
static bool  g_hudPatched = false;

// thrash entry points we wrap
typedef int   (__stdcall *TSetMode_t)(int, int, int);
typedef void  (__stdcall *TVoid_t)();
typedef void* (__stdcall *TLock_t)();
typedef void  (__stdcall *TUnlock_t)(int);
typedef void* (__stdcall *TAbout_t)();
static TSetMode_t g_tSetMode = nullptr;
static TVoid_t    g_tFlip = nullptr;
static TLock_t    g_tLock = nullptr;
static TUnlock_t  g_tUnlock = nullptr;
static TAbout_t   g_tAbout = nullptr;
static HMODULE    g_thrashMod = nullptr;

static bool ModeEntry(int idx, int* w, int* h, int* bpp) {
    if (!g_tAbout || idx < 1) return false;
    BYTE* a = (BYTE*)g_tAbout();
    if (!a || IsBadReadPtr(a, 0x44)) return false;
    int n = *(int*)(a + 0x3C);
    BYTE* e = *(BYTE**)(a + 0x40);
    if (idx > n || !e || IsBadReadPtr(e + idx * 40, 40)) return false;
    e += idx * 40;
    if (w) *w = *(int*)e;
    if (h) *h = *(int*)(e + 4);
    if (bpp) *bpp = *(int*)(e + 8);
    return true;
}
static int FindMode(int w, int h) {
    if (!g_tAbout) return 0;
    BYTE* a = (BYTE*)g_tAbout();
    if (!a) return 0;
    int n = *(int*)(a + 0x3C);
    for (int i = 1; i <= n; i++) { int mw, mh; if (ModeEntry(i, &mw, &mh, nullptr) && mw == w && mh == h) return i; }
    return 0;
}

// ---- Direct3D 7 device hooks (vertices / viewport / clear) ----------------
struct D3DVP7 { DWORD x, y, w, h; float minz, maxz; };
struct D3DRC { LONG x1, y1, x2, y2; };
typedef HRESULT (__stdcall *DevClear_t)(void*, DWORD, D3DRC*, DWORD, DWORD, float, DWORD);
typedef HRESULT (__stdcall *DevVp_t)(void*, D3DVP7*);
typedef HRESULT (__stdcall *DevDP_t)(void*, DWORD, DWORD, void*, DWORD, DWORD);
typedef HRESULT (__stdcall *DevDIP_t)(void*, DWORD, DWORD, void*, DWORD, WORD*, DWORD, DWORD);
typedef HRESULT (__stdcall *DevDPVB_t)(void*, DWORD, void*, DWORD, DWORD, DWORD);
typedef HRESULT (__stdcall *DevDIPVB_t)(void*, DWORD, void*, DWORD, DWORD, WORD*, DWORD, DWORD);
static DevClear_t g_oClear = nullptr;
static DevVp_t    g_oSetVp = nullptr;
static DevDP_t    g_oDP = nullptr;
static DevDIP_t   g_oDIP = nullptr;
static DevDPVB_t  g_oDPVB = nullptr;
static DevDIPVB_t g_oDIPVB = nullptr;
static void* g_dev = nullptr;
static D3DVP7 g_lastVp = { 0, 0, 640, 480, 0.0f, 1.0f };   // as the game set it
static BYTE* g_scratch = nullptr; static size_t g_scratchN = 0;

static inline LONG XfX(float x) { return (LONG)lrintf(x * g_xsx + g_xox); }
static inline LONG XfY(float y) { return (LONG)lrintf(y * g_xsy + g_xoy); }
static void XfRect(LONG& x1, LONG& y1, LONG& x2, LONG& y2) {
    LONG a = XfX((float)x1), b = XfY((float)y1), c = XfX((float)x2), d = XfY((float)y2);
    x1 = a; y1 = b; x2 = c; y2 = d;
}
static DWORD FvfSize(DWORD fvf) {
    DWORD s = 0;
    switch (fvf & 0x00E) { case 0x002: s = 12; break; case 0x004: s = 16; break;
                           case 0x006: s = 16; break; case 0x008: s = 20; break;
                           case 0x00A: s = 24; break; case 0x00C: s = 28; break; case 0x00E: s = 32; break; }
    if (fvf & 0x010) s += 12;            // normal
    if (fvf & 0x020) s += 4;             // psize
    if (fvf & 0x040) s += 4;             // diffuse
    if (fvf & 0x080) s += 4;             // specular
    DWORD tc = (fvf >> 8) & 0xF;
    for (DWORD i = 0; i < tc; i++) {
        DWORD f = (fvf >> (16 + i * 2)) & 3;   // 0 = 2D, 1 = 3D, 2 = 4D, 3 = 1D
        s += f == 0 ? 8 : f == 1 ? 12 : f == 2 ? 16 : 4;
    }
    return s;
}
static void* XfVerts(DWORD fvf, const void* v, DWORD n) {
    DWORD sz = FvfSize(fvf);
    size_t need = (size_t)sz * n;
    if (need > g_scratchN) { free(g_scratch); g_scratch = (BYTE*)malloc(need + 4096); g_scratchN = g_scratch ? need + 4096 : 0; }
    if (!g_scratch) return nullptr;
    memcpy(g_scratch, v, need);
    for (DWORD i = 0; i < n; i++) {
        float* p = (float*)(g_scratch + i * sz);
        p[0] = p[0] * g_xsx + g_xox; p[1] = p[1] * g_xsy + g_xoy;
    }
    return g_scratch;
}
static HRESULT __stdcall HkClear(void* d, DWORD n, D3DRC* r, DWORD fl, DWORD col, float z, DWORD st) {
    if (g_xf && n && r) {
        D3DRC tmp[16]; DWORD m = n < 16 ? n : 16;
        for (DWORD i = 0; i < m; i++) { tmp[i] = r[i]; XfRect(tmp[i].x1, tmp[i].y1, tmp[i].x2, tmp[i].y2); }
        return g_oClear(d, m, tmp, fl, col, z, st);
    }
    return g_oClear(d, n, r, fl, col, z, st);
}
static HRESULT __stdcall HkSetVp(void* d, D3DVP7* vp) {
    if (vp) g_lastVp = *vp;
    if (g_xf && vp) {
        D3DVP7 t = *vp;
        LONG x1 = vp->x, y1 = vp->y, x2 = vp->x + vp->w, y2 = vp->y + vp->h;
        XfRect(x1, y1, x2, y2);
        t.x = x1; t.y = y1; t.w = x2 - x1; t.h = y2 - y1;
        return g_oSetVp(d, &t);
    }
    return g_oSetVp(d, vp);
}
static inline bool IsTL(DWORD fvf) { return (fvf & 0x00E) == 0x004; }
static HRESULT __stdcall HkDP(void* d, DWORD pt, DWORD fvf, void* v, DWORD n, DWORD fl) {
    if (g_xf && IsTL(fvf) && v && n) {
        void* t = XfVerts(fvf, v, n);
        if (t) { g_wsXfDraws++; return g_oDP(d, pt, fvf, t, n, fl); }
    }
    return g_oDP(d, pt, fvf, v, n, fl);
}
static HRESULT __stdcall HkDIP(void* d, DWORD pt, DWORD fvf, void* v, DWORD n, WORD* idx, DWORD ni, DWORD fl) {
    if (g_xf && IsTL(fvf) && v && n) {
        void* t = XfVerts(fvf, v, n);
        if (t) { g_wsXfDraws++; return g_oDIP(d, pt, fvf, t, n, idx, ni, fl); }
    }
    return g_oDIP(d, pt, fvf, v, n, idx, ni, fl);
}
static HRESULT __stdcall HkDPVB(void* d, DWORD pt, void* vb, DWORD s, DWORD n, DWORD fl) {
    if (g_xf) g_wsVbDraws++;
    return g_oDPVB(d, pt, vb, s, n, fl);
}
static HRESULT __stdcall HkDIPVB(void* d, DWORD pt, void* vb, DWORD s, DWORD n, WORD* i, DWORD ni, DWORD fl) {
    if (g_xf) g_wsVbDraws++;
    return g_oDIPVB(d, pt, vb, s, n, i, ni, fl);
}
static void HookDevice(void* dev) {
    if (!dev) return;
    g_dev = dev;
    void** vt = *(void***)dev;
    if (vt[10] == (void*)&HkClear) return;           // this vtable is hooked already
    PatchPointer(&vt[10], (void*)&HkClear, (void**)&g_oClear);
    PatchPointer(&vt[13], (void*)&HkSetVp, (void**)&g_oSetVp);
    PatchPointer(&vt[25], (void*)&HkDP, (void**)&g_oDP);
    PatchPointer(&vt[26], (void*)&HkDIP, (void**)&g_oDIP);
    PatchPointer(&vt[31], (void*)&HkDPVB, (void**)&g_oDPVB);
    PatchPointer(&vt[32], (void*)&HkDIPVB, (void**)&g_oDIPVB);
    Logf("widescreen: Direct3D device hooked\n");
}
// IDirect3D7::CreateDevice (vtable 4) and IDirectDraw7::QueryInterface (vtable 0)
static const GUID kIID_D3D7 = { 0xf5049e77, 0x4861, 0x11d2, { 0xa4, 0x07, 0x00, 0xa0, 0xc9, 0x06, 0x29, 0xa8 } };
typedef HRESULT (__stdcall *CreateDev_t)(void*, REFCLSID, void*, void**);
typedef HRESULT (__stdcall *QI_t)(void*, REFIID, void**);
typedef HRESULT (WINAPI *DDCreateEx_t)(GUID*, void**, REFIID, void*);
static CreateDev_t g_oCreateDev = nullptr;
static QI_t g_oDDQI = nullptr;
static DDCreateEx_t g_oDDCreateEx = nullptr;
static HRESULT __stdcall HkCreateDev(void* d3d, REFCLSID c, void* surf, void** out) {
    HRESULT hr = g_oCreateDev(d3d, c, surf, out);
    if (SUCCEEDED(hr) && out && *out) HookDevice(*out);
    return hr;
}
static HRESULT __stdcall HkDDQI(void* dd, REFIID iid, void** out) {
    HRESULT hr = g_oDDQI(dd, iid, out);
    if (SUCCEEDED(hr) && out && *out && IsEqualGUID(iid, kIID_D3D7)) {
        void** vt = *(void***)*out;
        if (vt[4] != (void*)&HkCreateDev) PatchPointer(&vt[4], (void*)&HkCreateDev, (void**)&g_oCreateDev);
    }
    return hr;
}
static HRESULT WINAPI HkDDCreateEx(GUID* g, void** out, REFIID iid, void* u) {
    HRESULT hr = g_oDDCreateEx(g, out, iid, u);
    if (SUCCEEDED(hr) && out && *out) {
        void** vt = *(void***)*out;
        if (vt[0] != (void*)&HkDDQI) PatchPointer(&vt[0], (void*)&HkDDQI, (void**)&g_oDDQI);
    }
    return hr;
}
static void HookModuleDDraw(HMODULE m) {
    BYTE* base = (BYTE*)m;
    auto nt = (PIMAGE_NT_HEADERS)(base + ((PIMAGE_DOS_HEADER)base)->e_lfanew);
    DWORD rva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if (!rva) return;
    for (auto imp = (PIMAGE_IMPORT_DESCRIPTOR)(base + rva); imp->Name; ++imp) {
        if (_stricmp((const char*)(base + imp->Name), "ddraw.dll")) continue;
        auto th = (PIMAGE_THUNK_DATA)(base + imp->FirstThunk);
        auto on = (PIMAGE_THUNK_DATA)(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        for (; th->u1.Function; ++th, ++on) {
            if (IMAGE_SNAP_BY_ORDINAL(on->u1.Ordinal)) continue;
            auto nm = (PIMAGE_IMPORT_BY_NAME)(base + on->u1.AddressOfData);
            if (!strcmp((const char*)nm->Name, "DirectDrawCreateEx") &&
                th->u1.Function != (DWORD)&HkDDCreateEx)
                PatchPointer((void**)&th->u1.Function, (void*)&HkDDCreateEx, (void**)&g_oDDCreateEx);
        }
    }
}

// ---- thrash wrappers --------------------------------------------------------
static void ClearBars() {
    if (!g_dev || !g_oClear || !g_oSetVp || g_realW <= 0) return;
    LONG x0 = XfX(0), y0 = XfY(0), x1 = XfX(640), y1 = XfY(480);
    D3DRC r[4]; DWORD n = 0;
    if (x0 > 0) r[n++] = { 0, 0, x0, g_realH };
    if (x1 < g_realW) r[n++] = { x1, 0, g_realW, g_realH };
    if (y0 > 0) r[n++] = { x0, 0, x1, y0 };
    if (y1 < g_realH) r[n++] = { x0, y1, x1, g_realH };
    if (!n) return;
    D3DVP7 full = { 0, 0, (DWORD)g_realW, (DWORD)g_realH, 0.0f, 1.0f };
    g_oSetVp(g_dev, &full);
    g_oClear(g_dev, n, r, 1 /* D3DCLEAR_TARGET */, 0xFF000000, 1.0f, 0);
    HkSetVp(g_dev, &g_lastVp);              // back to the game's viewport (mapped)
    g_wsBarClears++;
}
static void __stdcall WrFlip() {
    if (g_xf) ClearBars();
    g_tFlip();
}
static void* g_lockInfo = nullptr; static DWORD g_lockRet = 0;
static void* __stdcall WrLock() {
    g_lockRet = (DWORD)__builtin_return_address(0);
    void* p = g_tLock();
    g_lockInfo = p;
    return p;
}
// FMVs are copied in software into the top-left 640x480 of the locked frame;
// scale that block in place to the menu area (from the bottom-right, so no
// source pixel is overwritten before it is read) and blank the rest of it.
static void ScaleLockedFrame() {
    BYTE* bits = *(BYTE**)g_lockInfo; int pitch = *(int*)((BYTE*)g_lockInfo + 4);
    if (!bits || pitch <= 0) return;
    int bp = g_realBpp >= 24 ? 4 : 2;
    int X0 = XfX(0), Y0 = XfY(0), X1 = XfX(640), Y1 = XfY(480);
    if (X0 < 0) X0 = 0; if (Y0 < 0) Y0 = 0; if (X1 > g_realW) X1 = g_realW; if (Y1 > g_realH) Y1 = g_realH;
    if (pitch < X1 * bp) return;
    for (int y = Y1 - 1; y >= Y0; y--) {
        int sy = (int)((y - g_xoy) / g_xsy); if (sy < 0) sy = 0; if (sy > 479) sy = 479;
        BYTE* drow = bits + (size_t)y * pitch; const BYTE* srow = bits + (size_t)sy * pitch;
        if (bp == 4) {
            DWORD* dr = (DWORD*)drow; const DWORD* sr = (const DWORD*)srow;
            for (int x = X1 - 1; x >= X0; x--) { int sx = (int)((x - g_xox) / g_xsx); dr[x] = sr[sx < 0 ? 0 : sx > 639 ? 639 : sx]; }
        } else {
            WORD* dr = (WORD*)drow; const WORD* sr = (const WORD*)srow;
            for (int x = X1 - 1; x >= X0; x--) { int sx = (int)((x - g_xox) / g_xsx); dr[x] = sr[sx < 0 ? 0 : sx > 639 ? 639 : sx]; }
        }
    }
    int hh = 480 < g_realH ? 480 : g_realH, ww = 640 < g_realW ? 640 : g_realW;
    for (int y = 0; y < hh; y++) {
        BYTE* row = bits + (size_t)y * pitch;
        if (y < Y0 || y >= Y1) { memset(row, 0, (size_t)ww * bp); continue; }
        if (X0 > 0) memset(row, 0, (size_t)(X0 < ww ? X0 : ww) * bp);
        if (X1 < ww) memset(row + (size_t)X1 * bp, 0, (size_t)(ww - X1) * bp);
    }
    g_wsMovieFrames++;
}
static unsigned g_lockLogged = 0;
static void __stdcall WrUnlock(int a) {
    if (g_xf && g_lockInfo && g_cfg.wsMovies && !IsBadReadPtr(g_lockInfo, 8)) {
        if (g_lockLogged < 8) { g_lockLogged++; Logf("widescreen: frame lock from 0x%06lX in the menus\n", (unsigned long)g_lockRet); }
        if (g_lockRet == 0x5807E9) ScaleLockedFrame();        // movie player
    }
    g_lockInfo = nullptr;
    g_tUnlock(a);
}
static int __stdcall WrSetMode(int idx, int a2, int bppCode) {
    int use = idx;
    if (g_feCall && g_feTarget > 0) use = g_feTarget;
    int r = g_tSetMode(use, a2, bppCode);
    int w = 0, h = 0, bpp = 16;
    if (ModeEntry(use, &w, &h, &bpp)) {
        int req = bppCode == 2 ? 32 : 16;
        g_realW = w; g_realH = h; g_realBpp = req <= bpp ? req : 16;
    }
    if (g_cfg.wsLog) Logf("widescreen: display mode %d -> %dx%d (game asked for mode %d)\n", use, g_realW, g_realH, idx);
    return r;
}
typedef FARPROC (WINAPI *GPA_t)(HMODULE, LPCSTR);
static GPA_t g_oGPA = nullptr;
static FARPROC WINAPI HkGetProcAddress(HMODULE m, LPCSTR name) {
    FARPROC p = g_oGPA(m, name);
    if (!p || !name || !HIWORD((DWORD)name) || strncmp(name, "_THRASH_", 8)) return p;
    if (m != g_thrashMod) {
        g_thrashMod = m;
        char fn[MAX_PATH] = {}; GetModuleFileNameA(m, fn, MAX_PATH);
        Logf("widescreen: renderer driver %s\n", fn);
        HookModuleDDraw(m);
    }
    if (!strcmp(name, "_THRASH_setvideomode@12")) { g_tSetMode = (TSetMode_t)p; return (FARPROC)&WrSetMode; }
    if (!strcmp(name, "_THRASH_pageflip@0"))      { g_tFlip = (TVoid_t)p;      return (FARPROC)&WrFlip; }
    if (!strcmp(name, "_THRASH_lockwindow@0"))    { g_tLock = (TLock_t)p;      return (FARPROC)&WrLock; }
    if (!strcmp(name, "_THRASH_unlockwindow@4"))  { g_tUnlock = (TUnlock_t)p;  return (FARPROC)&WrUnlock; }
    if (!strcmp(name, "_THRASH_about@0"))         g_tAbout = (TAbout_t)p;
    return p;
}

// ---- mode changes (called around 0x51A710) ---------------------------------
extern "C" { DWORD g_wsFnSetMode = 0x51A710, g_wsFnSelMode = 0x487840, g_wsSelResult = 0; }
extern "C" void WsCallSelMode();
extern "C" void __cdecl WsBeforeFeMode() {
    g_feCall = 1; g_feTarget = 0;
    if (!g_cfg.wsMenus) return;
    int t = 0, w = 0, h = 0;
    if (g_cfg.wsMenuResolution != 2) {
        WsCallSelMode();
        t = (int)g_wsSelResult;
        if (!ModeEntry(t, &w, &h, nullptr) || w < 640 || h < 480) t = 0;
    }
    if (!t && g_dispW) t = FindMode(g_dispW, g_dispH);
    g_feTarget = t;
}
extern "C" void __cdecl WsAfterFeMode() {
    g_feCall = 0;
    int W = g_realW, H = g_realH;
    bool on = g_cfg.wsMenus && W > 0 && H > 0 && (W != 640 || H != 480);
    if (on) {
        if (g_cfg.wsMenus == 2) { g_xsx = W / 640.0f; g_xsy = H / 480.0f; g_xox = g_xoy = 0; }
        else {
            float s = (float)W / 640.0f < (float)H / 480.0f ? (float)W / 640.0f : (float)H / 480.0f;
            g_xsx = g_xsy = s; g_xox = (W - 640.0f * s) * 0.5f; g_xoy = (H - 480.0f * s) * 0.5f;
        }
    }
    g_xf = on;
    if (g_cfg.wsLog) Logf("widescreen: menus %s (640x480 -> %dx%d, scale %.3f x %.3f, offset %.0f,%.0f)\n",
                          on ? "mapped" : "original", W, H, g_xsx, g_xsy, g_xox, g_xoy);
}
extern "C" void __cdecl WsBeforeRaceMode() { g_feCall = 0; g_xf = false; }
static void ApplyHud(int W, int H) {
    float k = (W > 0 && H > 0) ? (float)W * 0.75f / (float)H : 1.0f;   // aspect relative to 4:3
    if (!g_cfg.wsHud || k < 1.001f) k = 1.0f;
    g_hudW = (float)W / k;
    if (!g_hudPatched) return;
    for (int i = 0; i < 3; i++) {
        float a = g_hudInv640Orig[i] / k, b = g_hudSizesOrig[i] / k;
        *(float*)ws::hudInv640[i] = a; *(float*)ws::hudSizes[i] = b;
    }
    float x224 = 224.0f + (640.0f * k - 640.0f) * 0.5f;
    for (DWORD at : ws::hudX224) PatchBytes(at, &x224, 4);
}
extern "C" void __cdecl WsAfterRaceMode() {
    int W = Rd<int>(ws::pModeW), H = Rd<int>(ws::pModeH);
    ApplyHud(W, H);
    if (g_cfg.wsLog) Logf("widescreen: race at %dx%d - HUD lays out %.0f wide, Hor+ %s\n",
                          W, H, g_hudW, g_cfg.wsFixFOV ? "on" : "off");
}

// HUD rectangles from the layout file: [i*5+j] = 4 fractions (y0,x0,y1,x1) then
// the pixel rect. Recompute x in 4:3 proportions, anchored by the rect's middle.
extern "C" void __cdecl WsPostRects() {
    if (!g_cfg.wsHud) return;
    int W = Rd<int>(ws::pScrW), H = Rd<int>(ws::pScrH);
    float w43 = H * 4.0f / 3.0f;
    if (W <= 0 || H <= 0 || w43 >= W - 0.5f) return;
    float extra = W - w43;
    int moved[3] = {};
    for (int i = 0; i < 16; i++) for (int j = 0; j < 5; j++) {
        float* f = (float*)(ws::pRects + i * 0xA0 + j * 0x20);
        float* p = f + 4;
        if (f[1] == 0 && f[3] == 0) continue;
        float c = (f[1] + f[3]) * 0.5f;
        int a = g_cfg.wsHud == 2 ? 1 : c < 0.4f ? 0 : c > 0.6f ? 2 : 1;
        float off = a == 0 ? 0.0f : a == 2 ? extra : extra * 0.5f;
        float x0 = (float)lrintf(w43 * f[1]) + off, x1 = (float)lrintf(w43 * f[3]) + off;
        if (x0 < 0) x0 = 0;
        if (x1 > W - 1) x1 = (float)(W - 1);
        p[1] = x0; p[3] = x1;
        moved[a]++;
    }
    if (g_cfg.wsLog) Logf("widescreen: HUD layout - %d boxes left, %d middle, %d right\n", moved[0], moved[1], moved[2]);
}

// Hor+ for views as wide as the screen (not the mirror).
extern "C" { int g_wsFH = 0, g_wsFV = 0, g_wsFW = 0, g_wsFM = 0, g_wsOutH = 0, g_wsOutV = 0;
             DWORD g_wsFnFrustumUnits = 0x560350, g_wsFnProj = 0x453F10; float g_wsProjRf = 1.0f; }
extern "C" void __cdecl WsFrustum() {
    float r = 1.0f;
    int W = Rd<int>(ws::pScrW), H = Rd<int>(ws::pScrH);
    if (g_wsOn && g_cfg.wsFixFOV && !g_wsFM && W > 0 && H > 0 && g_wsFW > W * 0.45f) {
        float w43 = H * 4.0f / 3.0f;
        if (w43 < W) r = w43 / W;
    }
    g_projR = r; g_wsProjRf = r;
    if (r > 0.9999f) g_wsOutH = (g_wsFH << 10) / 360;
    else {
        double a = g_wsFH * 3.14159265358979 / 180.0, na = atan(tan(a) / r);
        g_wsOutH = (int)lround(na * 1024.0 / (2.0 * 3.14159265358979));
    }
    g_wsOutV = (g_wsFV << 10) / 360;
}
typedef void (__stdcall *Proj_t)(float, float, float, float, float, float, int);
extern "C" void __stdcall WsProj(float x, float y, float w, float h, float a, float b, int m) {
    ((Proj_t)ws::fnProj)(x, y, w, h, a, b, m);
    if (g_projR < 0.9999f) {
        *(float*)ws::pFocalX *= g_projR;
        *(float*)ws::pFocalY *= g_projR;
    }
}

extern "C" void WsFeModeStub();
extern "C" void WsRaceModeStub();
extern "C" void WsFrustumStub();
extern "C" void WsZoomStub();
extern "C" void WsRectsStub();
asm(".text\n"
    ".globl _WsCallSelMode\n"
    "_WsCallSelMode:\n"
    "    pushal\n"
    "    call *_g_wsFnSelMode\n"
    "    movl %eax, _g_wsSelResult\n"
    "    popal\n"
    "    ret\n"
    // call sites of SetVideoMode(eax = mode) for the menus / the race
    ".globl _WsFeModeStub\n"
    "_WsFeModeStub:\n"
    "    pushal\n"
    "    call _WsBeforeFeMode\n"
    "    popal\n"
    "    call *_g_wsFnSetMode\n"
    "    pushal\n"
    "    call _WsAfterFeMode\n"
    "    popal\n"
    "    ret\n"
    ".globl _WsRaceModeStub\n"
    "_WsRaceModeStub:\n"
    "    pushal\n"
    "    call _WsBeforeRaceMode\n"
    "    popal\n"
    "    call *_g_wsFnSetMode\n"
    "    pushal\n"
    "    call _WsAfterRaceMode\n"
    "    popal\n"
    "    ret\n"
    // replaces "call 0x560490" in 0x539E60: eax = half FOV, edx = vertical, ebx = w,
    // ecx = h, [ebp+0xC] = mirror flag; tail-calls the unit version 0x560350
    ".globl _WsFrustumStub\n"
    "_WsFrustumStub:\n"
    "    movl %eax, _g_wsFH\n"
    "    movl %edx, _g_wsFV\n"
    "    movl %ebx, _g_wsFW\n"
    "    movl 0xC(%ebp), %eax\n"
    "    movl %eax, _g_wsFM\n"
    "    pushl %ecx\n"
    "    pushl %ebx\n"
    "    call _WsFrustum\n"
    "    popl %ebx\n"
    "    popl %ecx\n"
    "    movl _g_wsOutH, %eax\n"
    "    movl _g_wsOutV, %edx\n"
    "    jmp *_g_wsFnFrustumUnits\n"
    // replaces "fstp [0x5B0950]" of the zoomed-camera focal length (call; nop)
    ".globl _WsZoomStub\n"
    "_WsZoomStub:\n"
    "    fmuls _g_wsProjRf\n"
    "    fstps 0x5B0950\n"
    "    ret\n"
    // call of the HUD rect builder
    ".globl _WsRectsStub\n"
    "_WsRectsStub:\n"
    "    pushal\n"
    "    movl $0x4D0550, %eax\n"
    "    call *%eax\n"
    "    call _WsPostRects\n"
    "    popal\n"
    "    ret\n");

static void InstallWidescreen() {
    g_dispW = GetSystemMetrics(SM_CXSCREEN); g_dispH = GetSystemMetrics(SM_CYSCREEN);
    if (!g_knownExe || !g_cfg.widescreen) { Logf("widescreen: off\n"); return; }
    // another widescreen patch in the exe: put its sites back first
    if (*(BYTE*)0x53A086 == 0xE9) {
        bool ok = true;
        for (const auto& r : ws::kOtherPatch) ok &= PatchBytes(r.addr, r.orig, r.len);
        Logf("widescreen: the exe's own widescreen patch %s (this mod's widescreen replaces it)\n",
             ok ? "undone" : "could NOT be undone");
    }
    bool ok = true;
    // GetProcAddress of the exe: wrap a few renderer entry points
    ForEachImport("KERNEL32.dll", [&](void** slot, const char* nm) {
        if (!strcmp(nm, "GetProcAddress")) ok &= PatchPointer(slot, (void*)&HkGetProcAddress, (void**)&g_oGPA);
    });
    if (!g_oGPA) { Logf("widescreen: GetProcAddress import not found - off\n"); return; }
    if (g_cfg.wsUnlockResolutions) { BYTE b = 0x00; ok &= PatchBytes(ws::modeFilterRel, &b, 1); }  // -> accept
    else if (*(BYTE*)ws::modeFilterRel == 0x00) { BYTE b = 0x2E; ok &= PatchBytes(ws::modeFilterRel, &b, 1); }
    for (DWORD at : ws::callModeFE) ok &= PatchRel32(at, 0xE8, (void*)&WsFeModeStub);
    ok &= PatchRel32(ws::callModeRace, 0xE8, (void*)&WsRaceModeStub);
    if (g_cfg.wsFixFOV) {
        ok &= PatchRel32(ws::callFrustum, 0xE8, (void*)&WsFrustumStub);
        ok &= PatchRel32(ws::callProj, 0xE8, (void*)&WsProj);
        BYTE z[6] = { 0xE8, 0, 0, 0, 0, 0x90 };
        DWORD rel = (DWORD)&WsZoomStub - (ws::zoomStore + 5); memcpy(z + 1, &rel, 4);
        ok &= PatchBytes(ws::zoomStore, z, 6);
    }
    if (g_cfg.wsHud) {
        for (int i = 0; i < 3; i++) { g_hudInv640Orig[i] = *(float*)ws::hudInv640[i]; g_hudSizesOrig[i] = *(float*)ws::hudSizes[i]; }
        BYTE f[6] = { 0xD9, 0x05, 0, 0, 0, 0 };          // fld dword [g_hudW]
        DWORD a = (DWORD)&g_hudW; memcpy(f + 2, &a, 4);
        ok &= PatchBytes(ws::hudWidthLoad, f, 6);
        ok &= PatchRel32(ws::callRects, 0xE8, (void*)&WsRectsStub);
        g_hudPatched = true;
        g_hudW = 640.0f;
    }
    g_wsOn = ok;
    Logf("widescreen: %s (menus %d, menu resolution %d, Hor+ %d, HUD %d, movies %d, all modes %d; desktop %dx%d)\n",
         ok ? "installed" : "patch FAILED", g_cfg.wsMenus, g_cfg.wsMenuResolution, g_cfg.wsFixFOV,
         g_cfg.wsHud, g_cfg.wsMovies, g_cfg.wsUnlockResolutions, g_dispW, g_dispH);
}

// ---------------------------------------------------------------------------
// Stats thread
// ---------------------------------------------------------------------------
static DWORD WINAPI StatsThread(LPVOID) {
    unsigned long ls = 0, lr = 0, le = 0, lf = 0, lb = 0, lrej = 0, lcut = 0, lmf = 0;
    for (;;) {
        int sec = g_cfg.logEverySeconds > 0 ? g_cfg.logEverySeconds : 10;
        Sleep(sec * 1000);
        LoadStatsTick();
        unsigned long mf = g_menuFrames;
        if (mf != lmf) Logf("last %ds: menu frames %.1f/s\n", sec, (mf - lmf) / (double)sec);
        lmf = mf;
        unsigned long s = g_steps, r = g_renders, e = g_extraRenders, f = g_tickFires,
                      b = g_blended, rj = g_rejected;
        if (s != ls || r != lr)
            Logf("last %ds: sim %.1f/s, frames %.1f/s (%.1f/s between steps), ticks %.1f/s, "
                 "blended %lu, not blended %lu, camera-cut frames %lu, avg blend %.2f, clamped %lu, resets %lu, clock resyncs %lu\n",
                 sec, (s - ls) / (double)sec, (r - lr) / (double)sec, (e - le) / (double)sec,
                 (f - lf) / (double)sec, b - lb, rj - lrej, (unsigned long)g_cuts - lcut,
                 g_alphaN ? g_alphaSum / g_alphaN : 0.0, g_alphaClamped, (unsigned long)g_resets,
                 (unsigned long)g_clockSnaps);
        if (g_clampHave && s != ls)
            Logf("  clamps: first at %02d:%02d:%02d.%03d, frame up to %.1f ms past the newest step, %.1f ms before the oldest\n",
                 g_clampFirst.wHour, g_clampFirst.wMinute, g_clampFirst.wSecond, g_clampFirst.wMilliseconds,
                 g_clampAheadMs, g_clampBehindMs);
        g_clampHave = false; g_clampAheadMs = 0; g_clampBehindMs = 0;
        g_alphaSum = 0; g_alphaN = 0; g_alphaClamped = 0;
        ls = s; lr = r; le = e; lf = f; lb = b; lrej = rj; lcut = g_cuts;
    }
}

// ---------------------------------------------------------------------------
BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        InitializeCriticalSection(&g_logLock); g_logLockInit = true;
        QueryPerformanceFrequency(&g_qpf);
        LoadConfig();
        OpenLog();
        g_knownExe = CheckExe();
        timeBeginPeriod(1);
        InstallTickScheduler();
        InstallInterpolation();
        InstallMenuUnlock();
        InitializeCriticalSection(&g_qLock);
        g_qEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);
        InstallLoadingAndFov();
        InstallGlideMenuEffects();
        InstallWidescreen();
        if (g_qEvent) CreateThread(nullptr, 0, WriterThread, nullptr, 0, nullptr);
        if (g_log) CreateThread(nullptr, 0, StatsThread, nullptr, 0, nullptr);
    } else if (reason == DLL_PROCESS_DETACH) {
        if (reserved) g_logLockInit = false;
        if (g_log) { Logf("=== shutdown ===\n"); fclose(g_log); g_log = nullptr; }
    }
    return TRUE;
}
