// HSFrameFix.asi
// Frame-rate unlock with interpolation, widescreen and fixes for Need for Speed:
// High Stakes (PC). Version history: CHANGELOG.md.
//
// Supported exes (all version 4.50, image base 0x400000, no ASLR):
//  - the official 4.50 patch, unmodified (nfshs.exe in EA's NFSHSpatch4_50.exe):
//    SHA-1 ac4f57ad3588161e0e0dd3ab582f7ad1f4d532eb (PE timestamp 0x3A8D8E0A).
//    This is the main one;
//  - the same exe with its CD check removed, 7 bytes changed (0x4F7F24: jne
//    -> jmp, 0x4F80C7: a call -> nops): SHA-1
//    b966f01d8d11e27f6d3167b8ec1ad3e3c2d30148. Versions up to 0.7.15 called
//    this one "unmodified";
//  - VEG's patched 4.50: SHA-1 8e10048ebc1b962bbe5dd569b48a791eeaafab5c (PE
//    timestamp 0x57334341). It changes code in place (CPU detection, startup,
//    a built-in widescreen patch) but none of the bytes this mod uses; its
//    widescreen patch is undone at startup, because this mod replaces it.
// Every patch site is byte-checked first, then the build is identified; on any
// other exe the ASI does nothing (exeCheck=0 patches anyway; every differing
// byte range is logged, expected vs found). The log names which build runs.
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
//  - interpolate: after every sim step it snapshots the transforms of all cars
//    (pos car+8, rotation car+0x34), all camera objects (pos cam+0x14,
//    rotation cam+0x2C, table 0x660DF0) and the helicopter (position). Before
//    each render it blends them to "now - one 64 Hz step" (rotations by
//    quaternion slerp), renders, then restores the exact simulation values,
//    so the simulation never sees interpolated data.
//  - The main loop's "nothing new, skip render" branch (0x51CB6C) now renders
//    an interpolated frame instead, so the frame rate follows the display
//    (vsync) or the optional limiter instead of the 64 Hz sim.
//  - Frames are placed on a smoothed simulation clock (follows when steps
//    arrive, averaged) with a 4-snapshot history, and on a smoothed frame clock;
//    mapping steps to the 64 Hz counter would shift by a whole step now and then.
//  - Camera cuts (switching camera, look back/left/right [0x661968], jumps too
//    large to blend) draw the frame at the newest step instead of mixing a
//    newest camera with blended cars.
//  - Menus: every front-end screen presents through 0x538170: flush, page flip,
//    then it spins until the 128 Hz tick counter [0x9E6D54] has advanced by 2
//    (= 64 fps cap), calling the idle work 0x526460 meanwhile. unlockMenus
//    replaces that wait with one idle call per frame (plus the optional
//    limiter). Menu animations are driven by the tick counter, so their speed
//    is unchanged; only the menu item reveal timing is adjusted so it doesn't
//    count at least 2 ticks per frame. menuMaxFPS keeps menus at or below
//    128 fps (auto: a divisor of the refresh rate), since several menu
//    animations speed up past the 128 Hz tick.
//  - glideMenuEffects: the front end's Glide-only effects (menu glow, full
//    brightness highlight colours, widget timing, a transition render state)
//    on Direct3D.
//  - Loading: the loading bar (0x4E7B80, ~48 calls per race load) does a full
//    GPU sync (THRASH_sync(0) = lock/unlock) and two vsync'd page flips every
//    call. speedUpLoading only redraws it every loadScreenRenderMs; the
//    skipped calls still run its callback poll (0x54A790).
//  - Camera FOV: the game uses one FOV for the whole view, from 0x539E10
//    (90 deg, 75 with the narrow view-angle option, +15 in some modes). Its
//    three callers inside the view setup (0x539F80, view in [ebp-4]) are
//    redirected so a per-camera offset is added (camera type of that view's
//    active camera; the rear-view mirror is left alone). Crash / recovery
//    cameras keep the offset of the camera the player picked
//    ([0x7C3CA8+view*4]). The cockpit model (0x4D1C80) uses its own fixed
//    80 degree FOV; it scales with the in-car offset. F6/F7/F8 change the
//    current camera's offset in race; a background thread saves it to the ini.
//  - HSFrameFix.ini: a new file is kIniText, in sections. An existing file is
//    left as it is; each setting it lacks is added at its end with its notes
//    (AppendMissingKeys), so a file from any older version keeps working.
//  - Widescreen: see the Widescreen section.
//  - closeExits: closing the window from outside ends the process.
//  - gameThreadsOneCore / widenAffinity: see "Game threads on one core".
//  - driversFolder: the renderer DLL from a folder of its own (see HkLoadLibraryA).
//  - noCDCheck: the official exe checks for the game CD twice at startup.
//    0x4F7F1D: 0x4F7FE0 asks MCI "status cdaudio ready"; unless that's "true"
//    it shows "Please Insert Game Disk" and asks again, and after it the game
//    looks for D:\Data\Audio\Music\Credits.asf, counting the drive letter
//    up. 0x4F80C7: 0x504B70 shows "you must first install" / "must have the
//    game CD in the CD-ROM drive" unless a drive holds NfsHs.exe. The two
//    sites get the bytes the CD-less exe has there (jne 0x4F7F48 -> jmp
//    0x4F7FA5; the call -> nops), and only if they still hold the original's.
//  - Startup arguments: the argument reader (0x4F8320; the only switch is
//    -PatchRestart) leaves its "argument was valid" variable [ebp-0x14] unset
//    when there are no arguments, and then tests it (0x4F833A: je 0x4F8432).
//    Whatever the stack holds decides: zero, and the game shows its "Invalid
//    argument" list of switches and quits. VEG's exe sets the variable first.
//    Here that je goes straight to the function's "fine" exit (0x4F85B1): it's
//    only taken with no arguments, or when all of them were valid.
//  - portableInstall: the game takes where it's installed from the registry
//    (0x4FC6E0, called at 0x4F80A2; any value missing = "files are corrupted;
//    please re-install"). Under HKLM: "Path" in the App Paths key of NfsHs.exe
//    (the install folder), and in "SOFTWARE\Electronic Arts\Need For Speed High
//    Stakes\1.0" the values "instLev" (0, 1 or 2: how much was copied from the
//    CD) and "SrcDrive" (the CD). From those it fills 15 folder paths
//    (0x826590, 0x104 bytes each; names at [0x5BD2DC], kinds at [0x5BD35C]):
//    kind 0 always in the install folder, 1 there if instLev >= 1, 2 there if
//    instLev = 2, 4 (Data/Audio/Music, Data/Movies) always on SrcDrive.
//    0x4FCA10 reads "Language" there too (2 French, 3 German, 4 Italian, 5
//    Spanish, 6 Swedish, else English), and 0x504C90 reads "Thrash Driver"
//    and "D3D Device" from the key above it. "Thrash Driver" is the renderer
//    DLL's file name: 0x503F9B compares it, as far as it goes and with case,
//    with the DLL names of the table at 0x5BE7AC (12 bytes an entry: display
//    name, DLL, number - Softtri softtria.dll, 3Dfx Voodoo voodoo2a.dll, 3Dfx
//    Voodoo 2 voodoo2a.dll, Direct3D d3da.dll); no match is Warning #2. The
//    registry calls the game imports are wrapped and answer
//    those values from the game's own folder: see the portable install section.
//  - noDiskSpaceCheck: 0x49E800 (eax = bytes wanted; 1 if the drive has them
//    free) multiplies the three GetDiskFreeSpace numbers with 32-bit imul and
//    compares signed, so a big disk fails it ("Your hard drive has less than
//    1MB free space"). It becomes "mov eax, 1 / ret".

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

#define HSFF_VERSION "0.8.0"

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
        { 0x49E800, "\x52\x55\x89\xE5\x81\xEC\x1C\x01\x00\x00", 10 },          // free disk space test
        { 0x504530, "\x51\x56\x57\x55\x89\xE5\x83\xEC\x3C", 9 },                // the "files are corrupted" box
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

// The whole file, as a new one is written. It is also where the notes come from
// when a setting is missing from an existing file (AppendMissingKeys).
static const char kIniText[] =
    "; HSFrameFix.ini (v" HSFF_VERSION ")\n"
    ";\n"
    "; Lines starting with ; are notes. For on/off settings 1 = on, 0 = off.\n"
    "; Delete this file to get a fresh one with the defaults.\n"
    "\n"
    "\n"
    "[Frame rate]\n"
    "\n"
    "; Draw frames at the display's refresh rate, blending car and camera motion between\n"
    "; the game's fixed 64 steps a second. 0 = the game's 64 fps cap\n"
    "interpolate=1\n"
    "\n"
    "; Smooth the police helicopter's movement too\n"
    "interpolateCopter=1\n"
    "\n"
    "; Run the game's 128 Hz timer from a precise clock. Needed for smooth blending\n"
    "preciseTicks=1\n"
    "\n"
    "; Frame limiter for races. 0 = off (use vsync). Without vsync, set it to your\n"
    "; display's refresh rate\n"
    "targetFPS=0\n"
    "\n"
    "; How many 1/64 s steps behind the newest step frames are drawn. 1.0 is smooth with\n"
    "; the least delay; try 1.5 if you still see hitches\n"
    "interpDelay=1.0\n"
    "\n"
    "; Movement bigger than this in one step isn't blended (resets, camera cuts):\n"
    "; a distance in metres, and an angle in degrees\n"
    "maxStepDistance=10\n"
    "maxStepAngle=90\n"
    "\n"
    "\n"
    "[Menus]\n"
    "\n"
    "; Drop the game's 64 fps cap in the menus\n"
    "unlockMenus=1\n"
    "\n"
    "; Frame limiter for the menus: -1 = same as targetFPS, 0 = off (use vsync), N = N fps\n"
    "menuTargetFPS=-1\n"
    "\n"
    "; Highest frame rate in the menus. Their animations count time in 1/128 s, and some\n"
    "; (list slide, video walls) speed up above 128 fps.\n"
    ";   -1 = automatic: the highest rate up to 128 that divides the refresh rate\n"
    ";        (120 Hz -> 120, 144 Hz -> 72, 240 Hz -> 120)\n"
    ";    0 = no ceiling\n"
    ";    N = at most N fps\n"
    "menuMaxFPS=-1\n"
    "\n"
    "; The extra effects the game only draws on its Glide renderer (glow on menu items,\n"
    "; full-brightness highlights) on Direct3D too\n"
    "glideMenuEffects=1\n"
    "\n"
    "\n"
    "[Loading]\n"
    "\n"
    "; Faster loading: the loading bar is redrawn at most every loadScreenRenderMs\n"
    "; milliseconds instead of at every step (0 = at every step)\n"
    "speedUpLoading=1\n"
    "loadScreenRenderMs=100\n"
    "\n"
    "\n"
    "[Camera]\n"
    "\n"
    "; Degrees added to the game's field of view (negative = narrower). fovOffset applies\n"
    "; to every race camera, the others to one camera each\n"
    "fovOffset=0\n"
    "fovOffsetInCar=0\n"
    "fovOffsetBumper=0\n"
    "fovOffsetHeli=0\n"
    "fovOffsetRoof=0\n"
    "\n"
    "; In a race: F8 = wider, F7 = narrower, F6 = reset, for the camera you're in. The\n"
    "; result is saved in the lines above\n"
    "fovHotkeys=1\n"
    "\n"
    "; The keys, as Windows virtual-key codes (F1..F12 = 0x70..0x7B; avoid F10), and how\n"
    "; many degrees one press changes\n"
    "fovKeyUp=0x77\n"
    "fovKeyDown=0x76\n"
    "fovKeyReset=0x75\n"
    "fovStep=1\n"
    "\n"
    "\n"
    "[Graphics]\n"
    "\n"
    "; Renderer:\n"
    ";   d3d      = Direct3D (d3da.dll)\n"
    ";   software = software renderer (softtria.dll)\n"
    ";   voodoo or voodoo2 = 3dfx Glide (voodoo2a.dll)\n"
    ";   Empty = the Windows registry's setting (Direct3D if it has none)\n"
    "renderer=\n"
    "\n"
    "; Folder the renderer DLLs are loaded from: a folder in the game folder, or a full\n"
    "; path. A DLL that isn't there is loaded from the game folder. Empty = the game folder\n"
    "driversFolder=\n"
    "\n"
    "; Direct3D: every vertex is moved half a pixel up and left, so 2D pictures (HUD,\n"
    "; menus, text) land exactly on the pixels and come out sharp. 0 = as the renderer has it\n"
    "alternatePixelCenter=1\n"
    "\n"
    "\n"
    "[Widescreen]\n"
    "\n"
    "; Master switch for the widescreen features (the ws... settings): pick any\n"
    "; resolution in the game's options\n"
    "widescreen=1\n"
    "\n"
    "; Race view: keep the 4:3 view's height and show more at the sides. 0 = the game\n"
    "; stretches the 4:3 picture to the screen's width\n"
    "wsFixFOV=1\n"
    "\n"
    "; List the resolutions that aren't 4:3 in the options (the game hides them)\n"
    "wsUnlockResolutions=1\n"
    "\n"
    "; Race HUD:\n"
    ";   1 = 4:3 proportions, with the side elements at the screen's edges\n"
    ";   2 = 4:3 proportions in the middle\n"
    ";   0 = stretched sideways\n"
    "wsHud=1\n"
    "\n"
    "; Menus (they are 640x480 pictures):\n"
    ";   1 = in 4:3 proportions in the middle of the screen\n"
    ";   2 = stretched to the screen\n"
    ";   0 = the game's own: the display switches to 640x480 for the menus\n"
    "wsMenus=1\n"
    "\n"
    "; Which resolution the menus use with wsMenus on: 1 = the one chosen in the game's\n"
    "; options (at startup, the one used last time), 2 = the desktop's\n"
    "wsMenuResolution=1\n"
    "\n"
    "; Movies (intro, FMVs) are scaled like the menus\n"
    "wsMovies=1\n"
    "\n"
    "; The race resolution last used, kept here by HSFrameFix (the game itself saves only\n"
    "; the width). 0 = none yet\n"
    "wsRaceWidth=0\n"
    "wsRaceHeight=0\n"
    "\n"
    "\n"
    "[Sound]\n"
    "\n"
    "; The game's own threads (main, sound, streaming) all run on one CPU core; its sound\n"
    "; code was written for one CPU and crackles when they run side by side. The system's\n"
    "; threads keep the other cores. 0 = leave them alone\n"
    "gameThreadsOneCore=1\n"
    "\n"
    "; If the whole game was started on one core (taskset, a launcher's single-core\n"
    "; option), the system's sound threads share that core and the sound pops whenever the\n"
    "; game is busy. 1 = the game gets all cores back and only its own threads stay on one.\n"
    "; The same is done if something pins the game later, such as a DirectDraw\n"
    "; wrapper's single-core option. Needs gameThreadsOneCore=1\n"
    "widenAffinity=1\n"
    "\n"
    "\n"
    "[Fixes]\n"
    "\n"
    "; Closing the window (Alt+F4, the title bar, the window manager) ends the game.\n"
    "; 0 = the game keeps running in the background\n"
    "closeExits=1\n"
    "\n"
    "; No test for free disk space at startup. On today's big disks the game's test goes\n"
    "; wrong and says \"Your hard drive has less than 1MB free space\". 0 = the game's test\n"
    "noDiskSpaceCheck=1\n"
    "\n"
    "\n"
    "[Portable]\n"
    "\n"
    "; The game needs no registry entries and can be copied or moved freely: where it's\n"
    "; installed is answered from the folder its exe is in (without the entries the game\n"
    "; stops with \"files are corrupted; please re-install\"). Music and movies are read\n"
    "; from the game folder if Data\\Audio\\Music and Data\\Movies are there, else from the\n"
    "; CD drive the registry names. 0 = the Windows registry as it is\n"
    "portableInstall=1\n"
    "\n"
    "; Language, used when the registry has none: english, german, french, spanish,\n"
    "; italian, swedish\n"
    "language=english\n"
    "\n"
    "; Don't ask for the game CD at startup (the official 4.50 exe does, twice)\n"
    "noCDCheck=1\n"
    "\n"
    "\n"
    "[Advanced]\n"
    "\n"
    "; 1 = if the game's exe isn't one this mod was made for, the mod turns itself off.\n"
    "; 0 = patch anyway (only for a modified exe you know is the same version; patching\n"
    "; the wrong code can crash the game). Every difference is written to the log\n"
    "exeCheck=1\n"
    "\n"
    "; Write HSFrameFix.log next to this file (what was patched, and frame statistics\n"
    "; every logEverySeconds seconds). Send it along when reporting a problem\n"
    "logToFile=1\n"
    "logEverySeconds=10\n"
    "\n"
    "; Widescreen details in the log too\n"
    "wsLog=1\n";
static char g_iniPath[MAX_PATH] = {};

// The settings an existing file has (LoadConfig), and the ones it lacks: a file
// from an older version keeps its layout, and each setting it doesn't have yet
// is added at its end with the notes that stand above it in kIniText.
static char g_iniSeen[96][32]; static int g_iniSeenN = 0;
static bool IniHas(const char* key) {
    for (int i = 0; i < g_iniSeenN; i++) if (!_stricmp(g_iniSeen[i], key)) return true;
    return false;
}
static void IniSeen(const char* key) {
    if (g_iniSeenN < 96 && !IniHas(key)) lstrcpynA(g_iniSeen[g_iniSeenN++], key, 32);
}
static void AppendMissingKeys(const char* path) {
    FILE* a = nullptr;
    const char* notes = nullptr, *notesEnd = nullptr;   // the notes above the settings of this paragraph
    bool together = false;                        // the setting just above, under the same notes, was added
    for (const char* p = kIniText; *p; ) {
        const char* e = strchr(p, '\n');
        const char* next = e ? e + 1 : p + strlen(p);
        size_t len = (size_t)((e ? e : next) - p);
        if (*p == ';') { if (!notes) notes = p; }
        else if (!len || *p == '[') { notes = notesEnd = nullptr; together = false; }
        else {
            if (notes && !notesEnd) notesEnd = p;
            char key[64] = {};
            const char* eq = (const char*)memchr(p, '=', len);
            if (eq && (size_t)(eq - p) < sizeof(key)) memcpy(key, p, (size_t)(eq - p));
            if (key[0] && !IniHas(key)) {
                if (!a) {
                    a = fopen(path, "a");
                    if (!a) return;
                    fputs("\n\n; --- Settings this file didn't have yet, added by HSFrameFix v" HSFF_VERSION " ---\n", a);
                }
                if (!together) {
                    fputs("\n", a);
                    if (notes) fwrite(notes, 1, (size_t)(notesEnd - notes), a);
                }
                fwrite(p, 1, len, a); fputs("\n", a);
                together = true;
            } else together = false;
        }
        p = next;
    }
    if (a) fclose(a);
}

static void LoadConfig() {
    InitAsiDir();
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%sHSFrameFix.ini", g_asiDir);
    strcpy(g_iniPath, path);
    FILE* f = fopen(path, "r");
    if (!f) {
        f = fopen(path, "w");
        if (f) {
            fputs(kIniText, f);
            fclose(f);
        }
        return;
    }
    char line[256];
    g_iniSeenN = 0;
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == ';' || line[0] == '#' || line[0] == '\r' || line[0] == '\n') continue;
        char key[64] = {}, val[64] = {};
        {   // driversFolder: a path - everything after the "=", spaces and all; may be empty
            char dv[MAX_PATH] = {};
            int got = sscanf(line, " %63[^= \t] = %259[^;\r\n]", key, dv);
            if (got >= 1 && strchr(line, '=')) IniSeen(key);      // (a setting with nothing after the "=" counts too)
            if (got >= 1 && !_stricmp(key, "driversFolder")) {
                for (size_t n = strlen(dv); n && (dv[n - 1] == ' ' || dv[n - 1] == '\t'); ) dv[--n] = 0;
                lstrcpynA(g_cfg.driversFolder, dv, sizeof(g_cfg.driversFolder));
                continue;
            }
        }
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
        else if (!_stricmp(key, "speedUpLoading"))    g_cfg.speedUpLoading = atoi(val);
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
        else if (!_stricmp(key, "glideMenuEffects"))  g_cfg.glideMenuEffects = atoi(val);
        else if (!_stricmp(key, "menuMaxFPS"))        g_cfg.menuMaxFPS = atoi(val);
        else if (!_stricmp(key, "exeCheck"))          g_cfg.exeCheck = atoi(val);
        else if (!_stricmp(key, "logToFile"))         g_cfg.logToFile = atoi(val);
        else if (!_stricmp(key, "logEverySeconds"))   g_cfg.logEverySeconds = atoi(val);
        else if (!_stricmp(key, "widescreen"))        g_cfg.widescreen = atoi(val);
        else if (!_stricmp(key, "wsMenus"))           g_cfg.wsMenus = atoi(val);
        else if (!_stricmp(key, "wsMenuResolution"))  g_cfg.wsMenuResolution = atoi(val);
        else if (!_stricmp(key, "wsFixFOV"))          g_cfg.wsFixFOV = atoi(val);
        else if (!_stricmp(key, "wsHud"))             g_cfg.wsHud = atoi(val);
        else if (!_stricmp(key, "wsMovies"))          g_cfg.wsMovies = atoi(val);
        else if (!_stricmp(key, "wsUnlockResolutions")) g_cfg.wsUnlockResolutions = atoi(val);
        else if (!_stricmp(key, "wsLog"))             g_cfg.wsLog = atoi(val);
        else if (!_stricmp(key, "closeExits"))        g_cfg.closeExits = atoi(val);
        else if (!_stricmp(key, "noCDCheck"))         g_cfg.noCDCheck = atoi(val);
        else if (!_stricmp(key, "noDiskSpaceCheck"))  g_cfg.noDiskSpaceCheck = atoi(val);
        else if (!_stricmp(key, "portableInstall"))   g_cfg.portableInstall = atoi(val);
        else if (!_stricmp(key, "alternatePixelCenter")) g_cfg.alternatePixelCenter = atoi(val);
        else if (!_stricmp(key, "gameThreadsOneCore")) g_cfg.gameThreadsOneCore = atoi(val);
        else if (!_stricmp(key, "widenAffinity"))     g_cfg.widenAffinity = atoi(val);
        else if (!_stricmp(key, "language"))          lstrcpynA(g_cfg.language, val, sizeof(g_cfg.language));
        else if (!_stricmp(key, "renderer"))          lstrcpynA(g_cfg.renderer, val, sizeof(g_cfg.renderer));
        else if (!_stricmp(key, "wsRaceWidth"))       g_cfg.wsRaceWidth = atoi(val);
        else if (!_stricmp(key, "wsRaceHeight"))      g_cfg.wsRaceHeight = atoi(val);
    }
    fclose(f);
    AppendMissingKeys(path);
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
static void ForEachImportIn(HMODULE mod, const char* dll, Fn cb) {
    BYTE* base = (BYTE*)mod;
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
template <typename Fn>
static void ForEachImport(const char* dll, Fn cb) { ForEachImportIn(GetModuleHandleA(nullptr), dll, cb); }
// This mod's own threads run on every core the process has, even if they're
// created from a game thread pinned to one core (gameThreadsOneCore).
static HANDLE g_modThreads[4]; static int g_modThreadN = 0;   // (DllMain only)
static HANDLE StartModThread(LPTHREAD_START_ROUTINE fn) {
    HANDLE h = CreateThread(nullptr, 0, fn, nullptr, CREATE_SUSPENDED, nullptr);
    if (!h) return nullptr;
    DWORD_PTR proc = 0, sys = 0;
    if (GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys) && proc) SetThreadAffinityMask(h, proc);
    if (g_modThreadN < 4) g_modThreads[g_modThreadN++] = h;
    ResumeThread(h);
    return h;
}
template <typename T> static inline T Rd(DWORD a) { return *(volatile T*)a; }
static char g_gameDir[MAX_PATH] = {};             // the folder the game's exe is in, with the closing backslash
static bool InitGameDir() {
    if (g_gameDir[0]) return true;
    if (!GetModuleFileNameA(nullptr, g_gameDir, MAX_PATH)) g_gameDir[0] = 0;
    char* sl = strrchr(g_gameDir, '\\');
    if (!sl) { g_gameDir[0] = 0; return false; }
    sl[1] = 0;
    return true;
}

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
        // Two supported builds share every byte the mod uses. They are told apart
        // by VEG's changes: its CPU detection rewrite at 0x41F405 (mov eax,
        // [0x5C3AAC] instead of the original cmp dword [0x5C3950], 0) and its
        // widescreen patch, which hooks the projection setup (0x53A086 jmp instead
        // of fstp [0x5B0954]). A VEG exe without its widescreen patch, or any
        // other mix, is not supported.
        const BYTE* c = (const BYTE*)0x41F405;
        const BYTE* w = (const BYTE*)0x53A086;
        bool vegCode = c[0] == 0xA1, vanCode = c[0] == 0x83 && c[1] == 0x3D;
        bool vegWs = w[0] == 0xE9, vanWs = w[0] == 0xD9 && w[1] == 0x1D;
        if (vanCode && vanWs) {
            bool cd = Rd<BYTE>(0x4F7F24) == 0x75 || Rd<BYTE>(0x4F80C7) == 0xE8;
            Logf("exe check: nfs4.exe matches (%d checks, official 4.50 patch%s)\n", total,
                 cd ? "" : ", CD check removed");
            return true;
        }
        if (vegCode && vegWs) {
            Logf("exe check: nfs4.exe matches (%d checks, VEG's 4.50 patch)\n", total);
            return true;
        }
        Logf("exe check: all %d checks match, but this isn't a supported build "
             "(0x41F405: %02X %02X, 0x53A086: %02X %02X)\n", total, c[0], c[1], w[0], w[1]);
        if (g_cfg.exeCheck) {
            Logf("exe check: mod disabled (exeCheck=0 in the ini patches anyway)\n");
            return false;
        }
        Logf("exe check: exeCheck=0, patching anyway\n");
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
    if (g_ticksActive) {
        g_tickThread = StartModThread(TickThread);
        if (!g_tickThread) g_ticksActive = false;        // no thread: every call goes to the game's timer
    }
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
        // highest rate <= 128 that divides the refresh rate (even pacing), not
        // below 50; otherwise the first rate <= 128 (e.g. 165 Hz -> 55, 144 -> 72)
        int c = 0, first = 0;
        for (int n = 1; n <= 64; n++) {
            int r = hz / n;
            if (r > 128) continue;
            if (!first) first = r;
            if (r < 50) break;
            if (hz % n == 0) { c = r; break; }
        }
        if (!c) c = first ? first : 60;
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

// Called from the game thread (race resolution) and the writer thread (FOV
// hotkeys): serialized, and the file is left alone if it doesn't fit the buffer.
static CRITICAL_SECTION g_iniLock;
static bool g_iniLockInit = false;
static void ReplaceIniLineLocked(const char* key, int value);
static void ReplaceIniLine(const char* key, int value) {
    if (!g_iniPath[0] || !g_iniLockInit) return;
    EnterCriticalSection(&g_iniLock);
    ReplaceIniLineLocked(key, value);
    LeaveCriticalSection(&g_iniLock);
}
static void ReplaceIniLineLocked(const char* key, int value) {
    FILE* f = fopen(g_iniPath, "r");
    if (!f) return;
    static char buf[32768];
    size_t n = 0; char line[256]; bool done = false, full = false; size_t kl = strlen(key);
    while (fgets(line, sizeof(line), f)) {
        char out[256];
        const char* p = line; while (*p == ' ' || *p == '\t') p++;
        if (!done && !_strnicmp(p, key, kl) && (p[kl] == '=' || p[kl] == ' ' || p[kl] == '\t')) {
            snprintf(out, sizeof(out), "%s=%d\n", key, value); done = true;
        } else snprintf(out, sizeof(out), "%s", line);
        size_t l = strlen(out);
        if (n + l < sizeof(buf)) { memcpy(buf + n, out, l); n += l; } else full = true;
    }
    fclose(f);
    if (!done) { char out[128]; snprintf(out, sizeof(out), "%s=%d\n", key, value);
                 size_t l = strlen(out); if (n + l < sizeof(buf)) { memcpy(buf + n, out, l); n += l; } else full = true; }
    if (full) { Logf("ini: too big to update %s (left unchanged)\n", key); return; }
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
static void AffinityWatch();
static DWORD WINAPI WriterThread(LPVOID) {
    for (;;) {
        WaitForSingleObject(g_qEvent, 1000);
        AffinityWatch();
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
// Widescreen
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
// VEG's widescreen patch, already in its exe (jumps at the sites below), is undone first.
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
    const DWORD hudFitW[2]      = { 0x4D2737, 0x4D28F6 };  // fild [0x853A98]: position / lap list width -> fraction
    const DWORD hudInv640[3]    = { 0x59967F, 0x5998CB, 0x599953 };
    const DWORD hudSizes[3]     = { 0x599CCB, 0x599CD7, 0x599CDB };
    const DWORD hudX224[2]      = { 0x4D0A30, 0x4D0B66 }; // float immediates (224.0)
    const DWORD callRects       = 0x4CE7D6, fnRects = 0x4D0550;
    const DWORD pRects          = 0x7CB0BC;
    const DWORD pCurMode        = 0x5BEF94;             // mode the game last set
    const DWORD modeLookupJe    = 0x51A677;             // je (4:3 only) in the width -> mode lookup
    const DWORD callLookupSaved = 0x4C2BB5;             // lookup of the saved resolution
    const DWORD fnLookup        = 0x51A530;

    struct Restore { DWORD addr; const char* orig; int len; };
    const Restore kOtherPatch[] = {        // original bytes at the sites of VEG's widescreen patch
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
static int   g_feCall = 0, g_feTarget = 0, g_curRealIdx = 0;
static bool  g_softDriver = false;
static std::atomic<unsigned long> g_wsXfDraws{0}, g_wsBarClears{0}, g_wsMovieFrames{0};
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

// ---- thrash draw wrappers (menus): map 640x480 vertices to the screen -------
// Every thrash driver of this game generation takes 32-byte pre-transformed
// vertices (x, y at +0/+4) and DWORD indices.
typedef void (__stdcall *TP1_t)(void*);
typedef void (__stdcall *TP2_t)(void*, void*);
typedef void (__stdcall *TP3_t)(void*, void*, void*);
typedef void (__stdcall *TP4_t)(void*, void*, void*, void*);
typedef void (__stdcall *TStrip_t)(int, void*);
typedef void (__stdcall *TMesh_t)(int, void*, DWORD*);
typedef void (__stdcall *TClip_t)(int, int, int, int);
static TP1_t g_tPoint; static TP2_t g_tLine; static TP3_t g_tTri; static TP4_t g_tQuad;
static TStrip_t g_tFan, g_tStrip, g_tLStrip, g_tPStrip;
static TMesh_t g_tTriM, g_tQuadM, g_tLineM, g_tPointM;
static TClip_t g_tClip; static TVoid_t g_tClear;
static int g_clip[4] = { 0, 0, 640, 480 };   // as the game set it

static inline LONG XfX(float x) { return (LONG)lrintf(x * g_xsx + g_xox); }
static inline LONG XfY(float y) { return (LONG)lrintf(y * g_xsy + g_xoy); }
struct XV { float f[8]; };
// alternatePixelCenter: Direct3D (as d3da.dll uses it) takes a pixel's centre to
// be at whole coordinates, where the game's 2D pictures are laid out for pixels
// that start there. So every vertex on its way to the Direct3D renderer is
// moved half a pixel up and left; pictures drawn 1:1 then land texel on pixel.
// (From HPFrameFix, where it was confirmed in game.)
static bool g_apc = false;                        // on for the renderer loaded now
static inline bool WrOn() { return g_xf || g_apc; }
static inline void Xv(XV& d, const void* s) {
    memcpy(&d, s, 32);
    if (g_xf) { d.f[0] = d.f[0] * g_xsx + g_xox; d.f[1] = d.f[1] * g_xsy + g_xoy; }
    if (g_apc) { d.f[0] -= 0.5f; d.f[1] -= 0.5f; }
}
static XV* g_vbuf = nullptr; static int g_vbufN = 0;
static XV* VBuf(int n) {
    if (n > g_vbufN) { free(g_vbuf); g_vbufN = n + 1024; g_vbuf = (XV*)malloc(sizeof(XV) * g_vbufN); if (!g_vbuf) g_vbufN = 0; }
    return g_vbuf;
}
static XV* XArr(const void* v, int n) {
    XV* b = VBuf(n); if (!b) return nullptr;
    for (int i = 0; i < n; i++) Xv(b[i], (const BYTE*)v + i * 32);
    if (g_xf) g_wsXfDraws++;
    return b;
}
static void __stdcall WrPoint(void* a) { if (!WrOn()) return g_tPoint(a); XV x; Xv(x, a); g_tPoint(&x); }
static void __stdcall WrLine(void* a, void* b) { if (!WrOn()) return g_tLine(a, b); XV x, y; Xv(x, a); Xv(y, b); g_tLine(&x, &y); }
static void __stdcall WrTri(void* a, void* b, void* c) {
    if (!WrOn()) return g_tTri(a, b, c);
    XV x, y, z; Xv(x, a); Xv(y, b); Xv(z, c); if (g_xf) g_wsXfDraws++; g_tTri(&x, &y, &z);
}
static void __stdcall WrQuad(void* a, void* b, void* c, void* d) {
    if (!WrOn()) return g_tQuad(a, b, c, d);
    XV x, y, z, w; Xv(x, a); Xv(y, b); Xv(z, c); Xv(w, d); if (g_xf) g_wsXfDraws++; g_tQuad(&x, &y, &z, &w);
}
static void StripCall(TStrip_t f, int n, void* v, int extra) {
    if (!WrOn() || n <= 0 || !v) return f(n, v);
    XV* b = XArr(v, n + extra); if (!b) return f(n, v);
    f(n, b);
}
static void __stdcall WrFan(int n, void* v)    { StripCall(g_tFan, n, v, 2); }
static void __stdcall WrStrip(int n, void* v)  { StripCall(g_tStrip, n, v, 2); }
static void __stdcall WrLStrip(int n, void* v) { StripCall(g_tLStrip, n, v, 1); }
static void __stdcall WrPStrip(int n, void* v) { StripCall(g_tPStrip, n, v, 0); }
static void MeshCall(TMesh_t f, int n, void* v, DWORD* idx, int per) {
    if (!WrOn() || n <= 0 || !v || !idx) return f(n, v, idx);
    DWORD mx = 0;
    for (int i = 0; i < n * per; i++) if (idx[i] > mx) mx = idx[i];
    if (mx > 65535) return f(n, v, idx);
    XV* b = XArr(v, (int)mx + 1); if (!b) return f(n, v, idx);
    f(n, b, idx);
}
static void __stdcall WrTriM(int n, void* v, DWORD* i)   { MeshCall(g_tTriM, n, v, i, 3); }
static void __stdcall WrQuadM(int n, void* v, DWORD* i)  { MeshCall(g_tQuadM, n, v, i, 4); }
static void __stdcall WrLineM(int n, void* v, DWORD* i)  { MeshCall(g_tLineM, n, v, i, 2); }
static void __stdcall WrPointM(int n, void* v, DWORD* i) { MeshCall(g_tPointM, n, v, i, 1); }
static void ClipXf(int x0, int y0, int x1, int y1) {
    if (g_xf) g_tClip(XfX((float)x0), XfY((float)y0), XfX((float)x1), XfY((float)y1));
    else g_tClip(x0, y0, x1, y1);
}
static void __stdcall WrClip(int x0, int y0, int x1, int y1) {
    g_clip[0] = x0; g_clip[1] = y0; g_clip[2] = x1; g_clip[3] = y1;
    ClipXf(x0, y0, x1, y1);
}

// ---- thrash wrappers --------------------------------------------------------
// black bars: the clear only covers the clip rectangle, so clear each bar
static void ClearBars() {
    if (!g_tClip || !g_tClear || g_realW <= 0) return;
    int x0 = XfX(0), y0 = XfY(0), x1 = XfX(640), y1 = XfY(480);
    int r[4][4]; int n = 0;
    if (x0 > 0) { int t[4] = { 0, 0, x0, g_realH }; memcpy(r[n++], t, sizeof t); }
    if (x1 < g_realW) { int t[4] = { x1, 0, g_realW, g_realH }; memcpy(r[n++], t, sizeof t); }
    if (y0 > 0) { int t[4] = { x0, 0, x1, y0 }; memcpy(r[n++], t, sizeof t); }
    if (y1 < g_realH) { int t[4] = { x0, y1, x1, g_realH }; memcpy(r[n++], t, sizeof t); }
    if (!n) return;
    for (int i = 0; i < n; i++) { g_tClip(r[i][0], r[i][1], r[i][2], r[i][3]); g_tClear(); }
    ClipXf(g_clip[0], g_clip[1], g_clip[2], g_clip[3]);
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
static const int kMovieW = 636;   // the last 4 columns of the movie picture are never written
static void ScaleLockedFrame() {
    BYTE* bits = *(BYTE**)g_lockInfo; int pitch = *(int*)((BYTE*)g_lockInfo + 4);
    if (!bits || pitch <= 0) return;
    int bp = g_realBpp >= 24 ? 4 : 2;
    if (pitch < g_realW * bp) return;
    // the movie player writes a packed 640x480 picture (rows 640 pixels apart),
    // not rows at the surface pitch
    static BYTE* src = nullptr;
    size_t srcN = (size_t)640 * 480 * bp;
    if (!src) src = (BYTE*)malloc((size_t)640 * 480 * 4);
    if (!src || (size_t)pitch * g_realH < srcN) return;
    memcpy(src, bits, srcN);
    for (int y = 0; y < g_realH; y++) memset(bits + (size_t)y * pitch, 0, (size_t)g_realW * bp);
    int X0 = XfX(0), Y0 = XfY(0), X1 = XfX(640), Y1 = XfY(480);
    if (X0 < 0) X0 = 0;
    if (Y0 < 0) Y0 = 0;
    if (X1 > g_realW) X1 = g_realW;
    if (Y1 > g_realH) Y1 = g_realH;
    for (int y = Y0; y < Y1; y++) {
        int sy = (int)((y - g_xoy) / g_xsy); if (sy < 0) sy = 0; if (sy > 479) sy = 479;
        BYTE* drow = bits + (size_t)y * pitch; const BYTE* srow = src + (size_t)sy * 640 * bp;
        if (bp == 4) {
            DWORD* dr = (DWORD*)drow; const DWORD* sr = (const DWORD*)srow;
            for (int x = X0; x < X1; x++) { int sx = (int)((x - g_xox) / g_xsx); dr[x] = sx >= kMovieW ? 0 : sr[sx < 0 ? 0 : sx]; }
        } else {
            WORD* dr = (WORD*)drow; const WORD* sr = (const WORD*)srow;
            for (int x = X0; x < X1; x++) { int sx = (int)((x - g_xox) / g_xsx); dr[x] = sx >= kMovieW ? 0 : sr[sx < 0 ? 0 : sx]; }
        }
    }
    g_wsMovieFrames++;
}
static unsigned g_lockLogged = 0;
static void __stdcall WrUnlock(int a) {
    if (g_xf && g_lockInfo && g_cfg.wsMovies && !IsBadReadPtr(g_lockInfo, 8)) {
        if (g_lockLogged < 8) { g_lockLogged++; Logf("widescreen: frame lock from 0x%06lX in the menus\n", (unsigned long)g_lockRet); }
        if (g_lockRet == 0x5807E9 || g_lockRet == 0x574011) ScaleLockedFrame();   // movie player
    }
    g_lockInfo = nullptr;
    g_tUnlock(a);
}
static int g_feWantW = 0, g_feWantH = 0;          // the size the menus are meant to have (0: the game's 640x480)
static int __stdcall WrSetMode(int idx, int a2, int bppCode) {
    int use = idx;
    if (g_feCall && g_cfg.wsMenus && !g_softDriver && g_feTarget <= 0 && g_feWantW > 0) {
        g_feTarget = FindMode(g_feWantW, g_feWantH);   // mode list may only be ready now
        if (g_cfg.wsLog) {
            BYTE* a = g_tAbout ? (BYTE*)g_tAbout() : nullptr;
            Logf("widescreen: menu mode for %dx%d: %d (%d modes listed)\n", g_feWantW, g_feWantH, g_feTarget,
                 a && !IsBadReadPtr(a, 0x44) ? *(int*)(a + 0x3C) : -1);
        }
    }
    if (g_feCall && g_feTarget > 0) use = g_feTarget;
    if (use != idx) {
        // keep the colour depth the game would have had in its own mode: the movie
        // player writes pixels in that format into the locked frame
        int gb = 16, rb = 16, req = bppCode == 2 ? 32 : 16;
        if (ModeEntry(idx, nullptr, nullptr, &gb) && ModeEntry(use, nullptr, nullptr, &rb)) {
            int eff = req <= gb ? req : 16;
            if (eff > rb) eff = 16;
            bppCode = eff == 32 ? 2 : 1;
        }
    }
    int r = g_tSetMode(use, a2, bppCode);
    g_curRealIdx = use;
    int w = 0, h = 0, bpp = 16;
    if (ModeEntry(use, &w, &h, &bpp)) {
        int req = bppCode == 2 ? 32 : 16;
        g_realW = w; g_realH = h; g_realBpp = req <= bpp ? req : 16;
    }
    if (g_cfg.wsLog) Logf("widescreen: display mode %d -> %dx%d, %d bit (game asked for mode %d)\n", use, g_realW, g_realH, g_realBpp, idx);
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
        // the software renderer draws screen-aligned textured quads as unscaled
        // blits, so scaled menus come apart: keep its menus in their own mode
        const char* b = strrchr(fn, '\\'); b = b ? b + 1 : fn;
        g_softDriver = !_strnicmp(b, "soft", 4);
        g_apc = g_cfg.alternatePixelCenter && !_strnicmp(b, "d3d", 3);
        Logf("renderer driver: %s%s\n", fn,
             g_softDriver && g_cfg.widescreen && g_cfg.wsMenus ? " (software renderer: menus stay 640x480)" : "");
        if (g_cfg.alternatePixelCenter)
            Logf("pixel centre: %s\n", g_apc ? "every vertex moved by (-0.5, -0.5)" : "left as it is (Direct3D only)");
    }
    struct W { const char* n; void** real; void* wr; };
    static const W kW[] = {
        { "_THRASH_drawpoint@4", (void**)&g_tPoint, (void*)&WrPoint },
        { "_THRASH_drawline@8", (void**)&g_tLine, (void*)&WrLine },
        { "_THRASH_drawtri@12", (void**)&g_tTri, (void*)&WrTri },
        { "_THRASH_drawquad@16", (void**)&g_tQuad, (void*)&WrQuad },
        { "_THRASH_drawtrifan@8", (void**)&g_tFan, (void*)&WrFan },
        { "_THRASH_drawtristrip@8", (void**)&g_tStrip, (void*)&WrStrip },
        { "_THRASH_drawlinestrip@8", (void**)&g_tLStrip, (void*)&WrLStrip },
        { "_THRASH_drawpointstrip@8", (void**)&g_tPStrip, (void*)&WrPStrip },
        { "_THRASH_drawtrimesh@12", (void**)&g_tTriM, (void*)&WrTriM },
        { "_THRASH_drawquadmesh@12", (void**)&g_tQuadM, (void*)&WrQuadM },
        { "_THRASH_drawlinemesh@12", (void**)&g_tLineM, (void*)&WrLineM },
        { "_THRASH_drawpointmesh@12", (void**)&g_tPointM, (void*)&WrPointM },
        { "_THRASH_clip@16", (void**)&g_tClip, (void*)&WrClip },
    };
    for (const W& w : kW) if (!strcmp(name, w.n)) { *w.real = (void*)p; return (FARPROC)w.wr; }
    if (!strcmp(name, "_THRASH_clearwindow@0")) g_tClear = (TVoid_t)(void*)p;
    if (!g_cfg.widescreen) return p;              // (hooked for alternatePixelCenter alone: the draw calls only)
    if (!strcmp(name, "_THRASH_setvideomode@12")) { g_tSetMode = (TSetMode_t)(void*)p; return (FARPROC)(void*)&WrSetMode; }
    if (!strcmp(name, "_THRASH_pageflip@0"))      { g_tFlip = (TVoid_t)(void*)p;      return (FARPROC)(void*)&WrFlip; }
    if (!strcmp(name, "_THRASH_lockwindow@0"))    { g_tLock = (TLock_t)(void*)p;      return (FARPROC)(void*)&WrLock; }
    if (!strcmp(name, "_THRASH_unlockwindow@4"))  { g_tUnlock = (TUnlock_t)(void*)p;  return (FARPROC)(void*)&WrUnlock; }
    if (!strcmp(name, "_THRASH_about@0"))         g_tAbout = (TAbout_t)(void*)p;
    return p;
}

// ---- mode changes (called around 0x51A710) ---------------------------------
extern "C" { DWORD g_wsFnSetMode = 0x51A710, g_wsFnSelMode = 0x487840, g_wsSelResult = 0; }
extern "C" void WsCallSelMode();
extern "C" void __cdecl WsBeforeFeMode() {
    g_feCall = 1; g_feTarget = 0;
    if (!g_cfg.wsMenus || g_softDriver) return;
    // wsMenuResolution=1: the resolution chosen in the game's options, whatever
    // it is - 640x480 too, which is then simply the game's own menu mode. The
    // first time (startup) the options aren't loaded yet and 640x480 comes
    // back: then it's the size kept in the ini from last time (none yet: the
    // game's 640x480). The desktop's size is only used when it's asked for (2).
    static bool first = true;
    int t = 0, w = 0, h = 0;
    if (g_cfg.wsMenuResolution == 2) { w = g_dispW; h = g_dispH; }
    else {
        WsCallSelMode();
        if (!ModeEntry((int)g_wsSelResult, &w, &h, nullptr)) { w = 0; h = 0; }
        if (first && w <= 640 && h <= 480) { w = g_cfg.wsRaceWidth; h = g_cfg.wsRaceHeight; }
    }
    first = false;
    if (w > 640 || h > 480) { g_feWantW = w; g_feWantH = h; t = FindMode(w, h); }
    else g_feWantW = g_feWantH = 0;
    g_feTarget = t;
    // the game skips the switch when it thinks mode 1 is already set
    if (t > 0 && t != g_curRealIdx) *(int*)ws::pCurMode = -1;
    if (g_cfg.wsLog) Logf("widescreen: menu mode wanted: %d (race mode setting %lu)\n", t, (unsigned long)g_wsSelResult);
}
extern "C" void __cdecl WsAfterFeMode() {
    g_feCall = 0;
    int W = g_realW, H = g_realH;
    bool on = g_cfg.wsMenus && !g_softDriver && W > 0 && H > 0 && (W != 640 || H != 480);
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
    if (W > 0 && H > 0 && (W != g_cfg.wsRaceWidth || H != g_cfg.wsRaceHeight)) {
        g_cfg.wsRaceWidth = W; g_cfg.wsRaceHeight = H;
        ReplaceIniLine("wsRaceWidth", W); ReplaceIniLine("wsRaceHeight", H);
    }
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
        if (g_cfg.wsLog >= 2) Logf("  HUD box %2d/%d: x %.3f-%.3f y %.3f-%.3f -> %s, px %.0f-%.0f\n", i, j,
                              f[1], f[3], f[0], f[2], a == 0 ? "left" : a == 2 ? "right" : "middle", x0, x1);
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

// The game saves only the width of the race resolution and, at startup, picks
// the 4:3 mode closest to it (1366 -> 1024x768). Use the remembered size.
extern "C" { DWORD g_wsFnLookup = 0x51A530; }
extern "C" int __cdecl WsFindMode(int w, int bpp) {
    (void)bpp;
    if (w > 0 && w == g_cfg.wsRaceWidth && g_cfg.wsRaceHeight > 0) {
        int i = FindMode(w, g_cfg.wsRaceHeight);
        if (g_cfg.wsLog) Logf("widescreen: saved resolution %dx%d -> mode %d\n", w, g_cfg.wsRaceHeight, i);
        return i;
    }
    return 0;
}
extern "C" void WsFindModeStub();
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
    // call of the width -> mode lookup for the saved resolution (eax = w, edx = bpp)
    ".globl _WsFindModeStub\n"
    "_WsFindModeStub:\n"
    "    pushl %ecx\n"
    "    pushl %edx\n"
    "    pushl %eax\n"
    "    call _WsFindMode\n"
    "    testl %eax, %eax\n"
    "    jz 1f\n"
    "    addl $8, %esp\n"
    "    popl %ecx\n"
    "    ret\n"
    "1:  popl %eax\n"
    "    popl %edx\n"
    "    popl %ecx\n"
    "    jmp *_g_wsFnLookup\n"
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

// ---------------------------------------------------------------------------
// Closing the window from outside (Alt+F4, the window manager): the game's window
// thread posts WM_QUIT from WM_DESTROY (0x54E37B) and ends, but the game thread
// never finds out and keeps running, hangs, or crashes while shutting down without
// a window. The game destroys its own window only through DestroyWindow at
// 0x54EBBC (after its own quit), so a WM_DESTROY outside that call means the
// window was closed from outside: end the process right there.
// ---------------------------------------------------------------------------
typedef void (WINAPI *PostQuitMessage_t)(int);
typedef BOOL (WINAPI *DestroyWindow_t)(HWND);
static PostQuitMessage_t g_oPostQuit = nullptr;
static DestroyWindow_t g_oDestroyWindow = nullptr;
static volatile LONG g_ownDestroy = 0;
static BOOL WINAPI HkDestroyWindow(HWND h) {
    InterlockedIncrement(&g_ownDestroy);
    BOOL r = g_oDestroyWindow(h);
    InterlockedDecrement(&g_ownDestroy);
    return r;
}
static void WINAPI HkPostQuitMessage(int code) {
    if (!g_ownDestroy && g_oDestroyWindow) {
        Logf("window closed from outside the game - exiting\n");
        TerminateProcess(GetCurrentProcess(), 0);
    }
    g_oPostQuit(code);
}
// The official exe's two startup CD checks get the bytes the CD-less exe has
// there, if they still hold the original's (VEG's exe and the CD-less one
// don't: nothing to do).
// ---------------------------------------------------------------------------
// Portable install: the registry values the game finds itself by (see the
// header) are answered here. RegOpenKeyExA / RegQueryValueExA / RegCloseKey as
// the game imports them are wrapped; the three keys are recognised when
// they're opened (a key that isn't in the registry gets a handle of our own),
// and their values are answered like this:
//  - App Paths\NfsHs.exe "Path": the folder the exe is in.
//  - 1.0 "instLev": 2 if every folder a full install has is in the game
//    folder, 1 if all but Data\Showcase are; otherwise the registry's (2 if
//    it has none).
//  - 1.0 "SrcDrive": the game folder if Data\Audio\Music and Data\Movies are
//    in it, or if the registry has none; otherwise the registry's (the CD).
//  - 1.0 "Language": the registry's; if it has none, language= from the ini.
//  - 1.0 "Variant": the registry's; if it has none, 0.
//  - "Thrash Driver": renderer= from the ini if set (as its DLL's name), else
//    the registry's, else d3da.dll. "D3D Device": the registry's, else 0. "3D Device Description":
//    the registry's, else a neutral name.
// Every other key and value goes to the registry untouched.
// ---------------------------------------------------------------------------
typedef LONG (WINAPI *RegOpen_t)(HKEY, LPCSTR, DWORD, REGSAM, PHKEY);
typedef LONG (WINAPI *RegQuery_t)(HKEY, LPCSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD);
typedef LONG (WINAPI *RegClose_t)(HKEY);
static RegOpen_t g_oRegOpen = nullptr; static RegQuery_t g_oRegQuery = nullptr; static RegClose_t g_oRegClose = nullptr;
enum { kRkAppPath = 1, kRkGame10, kRkGame };
static struct { HKEY h; int kind; bool fake; } g_rk[8];   // the keys of ours the game has open (game threads only)
static bool GameDirHas(const char* sub) {
    char p[MAX_PATH];
    if (snprintf(p, sizeof(p), "%s%s", g_gameDir, sub) >= (int)sizeof(p)) return false;
    DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}
static void PortableLogOnce(int id, const char* fmt, const char* a) {
    static unsigned done = 0;
    if (done & (1u << id)) return;
    done |= 1u << id;
    Logf(fmt, a);
}
static LONG RegAnswer(DWORD type, const void* v, DWORD len, LPDWORD outType, LPBYTE data, LPDWORD cb) {
    if (outType) *outType = type;
    if (!cb) return data ? ERROR_INVALID_PARAMETER : ERROR_SUCCESS;
    DWORD room = *cb; *cb = len;
    if (!data) return ERROR_SUCCESS;
    if (room < len) return ERROR_MORE_DATA;
    memcpy(data, v, len);
    return ERROR_SUCCESS;
}
static LONG RegAnswerSz(const char* s, LPDWORD t, LPBYTE d, LPDWORD cb) { return RegAnswer(REG_SZ, s, (DWORD)strlen(s) + 1, t, d, cb); }
static LONG RegAnswerDw(DWORD v, LPDWORD t, LPBYTE d, LPDWORD cb) { return RegAnswer(REG_DWORD, &v, 4, t, d, cb); }
static LONG WINAPI HkRegOpenKeyExA(HKEY root, LPCSTR sub, DWORD opt, REGSAM sam, PHKEY out) {
    LONG r = g_oRegOpen(root, sub, opt, sam, out);
    int kind = 0;
    if (root == HKEY_LOCAL_MACHINE && sub && out) {
        if (!_stricmp(sub, "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\NfsHs.exe")) kind = kRkAppPath;
        else if (!_stricmp(sub, "SOFTWARE\\Electronic Arts\\Need For Speed High Stakes\\1.0")) kind = kRkGame10;
        else if (!_stricmp(sub, "SOFTWARE\\Electronic Arts\\Need For Speed High Stakes")) kind = kRkGame;
    }
    if (!kind) return r;
    for (int i = 0; i < 8; i++) {
        if (g_rk[i].h) continue;
        bool fake = r != ERROR_SUCCESS;
        if (fake) *out = (HKEY)(ULONG_PTR)(0x7EED0100 + i * 4);    // (not in the registry: a handle of our own)
        g_rk[i].h = *out; g_rk[i].kind = kind; g_rk[i].fake = fake;
        return ERROR_SUCCESS;
    }
    return r;                                     // (no slot free: as the registry has it)
}
static LONG WINAPI HkRegCloseKey(HKEY h) {
    for (int i = 0; i < 8; i++)
        if (g_rk[i].h == h && h) {
            bool fake = g_rk[i].fake;
            g_rk[i].h = nullptr;
            return fake ? ERROR_SUCCESS : g_oRegClose(h);
        }
    return g_oRegClose(h);
}
static LONG WINAPI HkRegQueryValueExA(HKEY h, LPCSTR name, LPDWORD res, LPDWORD type, LPBYTE data, LPDWORD cb) {
    int kind = 0; bool fake = false;
    for (int i = 0; i < 8; i++) if (g_rk[i].h == h && h) { kind = g_rk[i].kind; fake = g_rk[i].fake; }
    if (!kind || !name) return g_oRegQuery(h, name, res, type, data, cb);
    // what the registry itself says (kept in the caller's buffer when it's used as it is)
    DWORD cbIn = cb ? *cb : 0;
    LONG real = fake ? ERROR_FILE_NOT_FOUND : g_oRegQuery(h, name, res, type, data, cb);
    bool have = real == ERROR_SUCCESS;
    if (!have && cb) *cb = cbIn;
    if (kind == kRkAppPath && !_stricmp(name, "Path")) {
        char dir[MAX_PATH]; lstrcpynA(dir, g_gameDir, sizeof(dir));
        size_t n = strlen(dir);
        if (n > 3 && dir[n - 1] == '\\') dir[n - 1] = 0;          // (as the installer writes it: no closing backslash)
        PortableLogOnce(0, "portable install: install folder = %s\n", dir);
        if (cb) *cb = cbIn;
        return RegAnswerSz(dir, type, data, cb);
    }
    if (kind == kRkGame10 && !_stricmp(name, "instLev")) {
        static const char* const kLev1[] = { "Data\\Tracks", "Data\\Cars", "Data\\Audio\\SFX", "Data\\Audio\\Speech",
                                             "Data\\FeArt", "Data\\FeArt\\Track", "Data\\FeArt\\VidWall" };
        bool all1 = true;
        for (const char* f : kLev1) all1 = all1 && GameDirHas(f);
        int lev = all1 ? (GameDirHas("Data\\Showcase") ? 2 : 1) : -1;
        if (lev < 0 && have) { PortableLogOnce(1, "portable install: not every data folder is in the game folder - install level %s\n", "as the registry has it"); return real; }
        if (lev < 0) lev = 2;
        PortableLogOnce(1, "portable install: install level %s\n", lev == 2 ? "2 (everything in the game folder)" : "1 (no Data\\Showcase in the game folder)");
        if (cb) *cb = cbIn;
        return RegAnswerDw((DWORD)lev, type, data, cb);
    }
    if (kind == kRkGame10 && !_stricmp(name, "SrcDrive")) {
        bool local = GameDirHas("Data\\Audio\\Music") && GameDirHas("Data\\Movies");
        if (!local && have) { PortableLogOnce(2, "portable install: music and movies %s\n", "from the CD drive the registry names (not both in the game folder)"); return real; }
        PortableLogOnce(2, "portable install: music and movies %s\n", local ? "from the game folder" : "expected in the game folder (not there, and the registry names no CD drive)");
        if (cb) *cb = cbIn;
        return RegAnswerSz(g_gameDir, type, data, cb);
    }
    if (kind == kRkGame10 && !_stricmp(name, "Language") && !have) {
        static const struct { const char* n; DWORD code; } kLang[] = {
            { "english", 1 }, { "french", 2 }, { "german", 3 }, { "italian", 4 }, { "spanish", 5 }, { "swedish", 6 } };
        DWORD code = 1;
        for (const auto& l : kLang) if (!_stricmp(g_cfg.language, l.n)) code = l.code;
        PortableLogOnce(3, "portable install: no language in the registry - %s\n", g_cfg.language);
        return RegAnswerDw(code, type, data, cb);
    }
    // "Variant" (0x5047A0; without it the game aborts: AMF=20 platform.c(604)): the
    // edition, 0 to 7. 0 is the American one (US English; Spanish prices in pesos),
    // 1 with English gives the British texts, 4, 5 and 6 set special flags.
    if (kind == kRkGame10 && !_stricmp(name, "Variant") && !have) {
        PortableLogOnce(6, "portable install: no edition (Variant) in the registry - %s\n", "0, the American one");
        return RegAnswerDw(0, type, data, cb);
    }
    if (kind == kRkGame && !_stricmp(name, "Thrash Driver")) {
        static const struct { const char* n; const char* v; } kRend[] = {
            { "d3d", "d3da.dll" }, { "software", "softtria.dll" }, { "voodoo", "voodoo2a.dll" }, { "voodoo2", "voodoo2a.dll" } };
        const char* v = nullptr;
        for (const auto& m : kRend) if (!_stricmp(g_cfg.renderer, m.n)) v = m.v;
        if (g_cfg.renderer[0] && !v) PortableLogOnce(4, "renderer: \"%s\" isn't d3d, software, voodoo or voodoo2 - ignored\n", g_cfg.renderer);
        if (!v && have) return real;
        if (!v) v = "d3da.dll";
        PortableLogOnce(5, "renderer: %s\n", v);
        if (cb) *cb = cbIn;
        return RegAnswerSz(v, type, data, cb);
    }
    if (kind == kRkGame && !_stricmp(name, "D3D Device") && !have) return RegAnswerDw(0, type, data, cb);
    // (0x504D50, read just before the renderer; missing = "files are corrupted". A card's
    // name, of which the game only knows a few old ones to work around)
    if (kind == kRkGame && !_stricmp(name, "3D Device Description") && !have) return RegAnswerSz("Primary Display Driver", type, data, cb);
    if (!have) {                                  // (a value of the game's keys nobody answers: worth knowing about)
        static int n = 0;
        if (n++ < 16) Logf("portable install: the game asked for \"%s\" (key %d), which isn't in the registry and isn't answered here\n", name, kind);
    }
    return real;
}
static void InstallPortable() {
    if (!g_knownExe) return;
    if (!g_cfg.portableInstall) { Logf("portable install: off (the registry as it is)\n"); return; }
    if (!InitGameDir()) { Logf("portable install: the game's folder isn't known - off\n"); return; }
    bool o = false, q = false, c = false;
    ForEachImport("ADVAPI32.dll", [&](void** slot, const char* nm) {
        if (!strcmp(nm, "RegOpenKeyExA")) o = PatchPointer(slot, (void*)&HkRegOpenKeyExA, (void**)&g_oRegOpen);
        else if (!strcmp(nm, "RegQueryValueExA")) q = PatchPointer(slot, (void*)&HkRegQueryValueExA, (void**)&g_oRegQuery);
        else if (!strcmp(nm, "RegCloseKey")) c = PatchPointer(slot, (void*)&HkRegCloseKey, (void**)&g_oRegClose);
    });
    Logf("portable install: %s\n", o && q && c ? "on" : "imports not found - off");
}

// ---------------------------------------------------------------------------
// Game threads on one core (from HPFrameFix; as SilentPatch's
// SingleProcAffinity for NFS3): the main thread and every thread the game or
// its sound driver eacsnd.dll creates run on one core; threads of Windows/Wine
// and of this mod keep all cores. The game's sound code shares data between
// its threads with no locking that holds up on several cores: it crackles and
// pops. Pinning the whole process instead would also put DirectSound's mixer
// on that core.
// ---------------------------------------------------------------------------
typedef HANDLE (WINAPI *CreateThread_t)(LPSECURITY_ATTRIBUTES, SIZE_T, LPTHREAD_START_ROUTINE, LPVOID, DWORD, LPDWORD);
static CreateThread_t g_oCreateThread = nullptr;
static volatile DWORD_PTR g_gameCore = 0;
// The game's threads, kept so they can be put back on their core (AffinityWatch)
static CRITICAL_SECTION g_gtLock; static bool g_gtLockInit = false;
static HANDLE g_gameThreads[64]; static int g_gameThreadN = 0;
static void NoteGameThread(HANDLE h) {
    HANDLE d = nullptr;
    if (!g_gtLockInit || !DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &d, 0, FALSE, DUPLICATE_SAME_ACCESS)) return;
    EnterCriticalSection(&g_gtLock);
    for (int i = 0; i < g_gameThreadN; )          // (threads that have ended make room)
        if (WaitForSingleObject(g_gameThreads[i], 0) == WAIT_OBJECT_0) { CloseHandle(g_gameThreads[i]); g_gameThreads[i] = g_gameThreads[--g_gameThreadN]; }
        else i++;
    if (g_gameThreadN < 64) g_gameThreads[g_gameThreadN++] = d; else CloseHandle(d);
    LeaveCriticalSection(&g_gtLock);
}
static HANDLE WINAPI HkCreateThread(LPSECURITY_ATTRIBUTES sa, SIZE_T stack, LPTHREAD_START_ROUTINE fn, LPVOID arg,
                                    DWORD flags, LPDWORD id) {
    HANDLE h = g_oCreateThread(sa, stack, fn, arg, flags | CREATE_SUSPENDED, id);
    if (h) {
        // (under the lock: AffinityWatch either sees this thread in the list or has finished before it's pinned)
        if (g_gtLockInit) EnterCriticalSection(&g_gtLock);
        SetThreadAffinityMask(h, g_gameCore);
        NoteGameThread(h);
        if (g_gtLockInit) LeaveCriticalSection(&g_gtLock);
        if (!(flags & CREATE_SUSPENDED)) ResumeThread(h);
    }
    return h;
}
// Something else can pin the whole process to one core after this mod has
// loaded - a DirectDraw or Direct3D wrapper's "single core" option does that
// when the game starts its renderer. That puts the system's sound threads and
// this mod's own on the game's core after all, and leaves the game's threads
// with a core of ours that the process no longer has. Looked at once a second
// (the log writer's thread): if the process has come down to one core, it gets
// all of them back (widenAffinity), the game's threads go to the core the
// other one picked, and this mod's threads get every core again.
static void AffinityWatch() {
    if (!g_gameCore || !g_cfg.widenAffinity || !g_gtLockInit) return;
    DWORD_PTR proc = 0, sys = 0;
    if (!GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys) || !proc) return;
    if ((proc & (proc - 1)) || !(sys & (sys - 1))) return;        // (not pinned, or there is only one core)
    static int times = 0;
    EnterCriticalSection(&g_gtLock);              // (no game thread is created and pinned half way through this)
    if (!SetProcessAffinityMask(GetCurrentProcess(), sys)) {
        LeaveCriticalSection(&g_gtLock);
        if (times++ < 3) Logf("threads: the process was pinned to one core (mask %lX) after the mod loaded - couldn't be widened\n", (unsigned long)proc);
        return;
    }
    g_gameCore = proc;
    int n = 0;
    for (int i = 0; i < g_gameThreadN; i++) if (SetThreadAffinityMask(g_gameThreads[i], proc)) n++;
    LeaveCriticalSection(&g_gtLock);
    for (int i = 0; i < g_modThreadN; i++) SetThreadAffinityMask(g_modThreads[i], sys);
    if (times++ < 8) Logf("threads: something pinned the whole process to one core (mask %lX) after the mod loaded - "
                          "all cores given back, the game's %d thread(s) kept on that core\n", (unsigned long)proc, n);
}
static bool HookCreateThreadIn(HMODULE mod, const char* what) {
    bool hooked = false;
    ForEachImportIn(mod, "KERNEL32.dll", [&](void** s, const char* nm) {
        if (strcmp(nm, "CreateThread")) return;
        void* old = nullptr;
        if (*s != (void*)&HkCreateThread && PatchPointer(s, (void*)&HkCreateThread, &old)) {
            if (!g_oCreateThread) g_oCreateThread = (CreateThread_t)old;
            hooked = true;
        }
    });
    if (!hooked) Logf("threads: %s - CreateThread import not found\n", what);
    return hooked;
}
// LoadLibraryA as the game imports it. Two things hang on it:
//  - eacsnd.dll, the sound driver, is loaded that way: its threads are the
//    game's too (gameThreadsOneCore);
//  - driversFolder (from HPFrameFix): the renderer DLL, which the game loads by
//    its bare name (0x5040D2), is taken from that folder if it's there, else
//    from the game folder as before. While it loads, that folder is also where
//    its own dependencies (glide2x.dll...) are looked for after the game folder.
typedef HMODULE (WINAPI *LoadLibraryA_t)(LPCSTR);
static LoadLibraryA_t g_oLoadLibraryA = nullptr;
static char g_driversDir[MAX_PATH] = {};          // with the closing backslash; empty = game folder
static bool g_driversExist = false;
static HMODULE WINAPI HkLoadLibraryA(LPCSTR name) {
    static const char* const kRendererDlls[] = { "d3da.dll", "softtria.dll", "voodoo2a.dll" };
    bool renderer = false;
    if (name) for (const char* d : kRendererDlls) renderer = renderer || !_stricmp(name, d);
    if (renderer && g_driversDir[0]) {
        static char seen[4][16]; static int nSeen = 0;            // log each DLL once
        bool first = true;
        for (int i = 0; i < nSeen; i++) if (!_stricmp(seen[i], name)) first = false;
        if (first && nSeen < 4) lstrcpynA(seen[nSeen++], name, 16);
        char path[MAX_PATH];
        if (snprintf(path, sizeof(path), "%s%s", g_driversDir, name) < (int)sizeof(path) &&
            GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) {
            char prev[MAX_PATH]; DWORD pl = GetDllDirectoryA(sizeof(prev), prev);
            SetDllDirectoryA(g_driversDir);
            HMODULE h = g_oLoadLibraryA(path);
            DWORD err = h ? 0 : GetLastError();
            SetDllDirectoryA(pl && pl < sizeof(prev) ? prev : nullptr);
            if (first) Logf("drivers: %s %s (error %lu)\n", path, h ? "loaded" : "FAILED", (unsigned long)err);
            if (h) return h;
        } else if (first && g_driversExist) {
            Logf("drivers: %s not in %s - game folder\n", name, g_driversDir);
        }
    }
    HMODULE h = g_oLoadLibraryA(name);
    if (h && name && g_gameCore) {
        const char* b = name;
        for (const char* c = name; *c; c++) if (*c == '\\' || *c == '/' || *c == ':') b = c + 1;
        if (!_stricmp(b, "eacsnd.dll") || !_stricmp(b, "eacsnd")) {
            static HMODULE hooked = nullptr;
            if (h != hooked && HookCreateThreadIn(h, "eacsnd.dll")) { hooked = h; Logf("threads: eacsnd.dll's threads too\n"); }
        }
    }
    return h;
}
static bool HookLoadLibrary() {                   // once, for whichever of the two wants it
    static int state = -1;
    if (state < 0) {
        void** slot = nullptr;
        ForEachImport("KERNEL32.dll", [&](void** sl, const char* nm) { if (!strcmp(nm, "LoadLibraryA")) slot = sl; });
        state = slot && PatchPointer(slot, (void*)&HkLoadLibraryA, (void**)&g_oLoadLibraryA) ? 1 : 0;
    }
    return state == 1;
}
static void InstallDriversFolder() {
    if (!g_knownExe) return;
    const char* f = g_cfg.driversFolder;
    if (!f[0]) { Logf("drivers: game folder\n"); return; }
    bool absolute = strchr(f, ':') || (f[0] == '\\' && f[1] == '\\');
    if (!absolute && !InitGameDir()) { Logf("drivers: the game's folder isn't known - game folder\n"); return; }
    int n = snprintf(g_driversDir, sizeof(g_driversDir), "%s%s", absolute ? "" : g_gameDir, f);
    if (n <= 0 || n >= (int)sizeof(g_driversDir) - 1) { Logf("drivers: path too long - game folder\n"); g_driversDir[0] = 0; return; }
    while (n && (g_driversDir[n - 1] == '\\' || g_driversDir[n - 1] == '/')) g_driversDir[--n] = 0;
    g_driversDir[n++] = '\\'; g_driversDir[n] = 0;
    if (!HookLoadLibrary()) { g_driversDir[0] = 0; Logf("drivers: LoadLibraryA import not found - game folder\n"); return; }
    DWORD a = GetFileAttributesA(g_driversDir);
    g_driversExist = a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
    Logf("drivers: %s%s\n", g_driversDir, g_driversExist ? "" : " (not there - game folder)");
}
static void InstallGameThreadsOneCore() {
    if (!g_knownExe || !g_cfg.gameThreadsOneCore) { Logf("threads: all cores\n"); return; }
    DWORD_PTR proc = 0, sys = 0;
    if (!GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys) || !proc) { Logf("threads: affinity unknown - all cores\n"); return; }
    // A process that's already on one core has the system's threads on that core
    // too - under Wine that's DirectSound's mixer and the audio driver's threads,
    // which then wait behind the game thread whenever it's busy, and the sound
    // pops. It gets all the system's cores back, and from here on it's the usual
    // arrangement: the game's own threads on the core it was on, the rest free.
    DWORD_PTR was = proc;
    if (!(proc & (proc - 1))) {
        if (!g_cfg.widenAffinity || !(sys & (sys - 1)) || !SetProcessAffinityMask(GetCurrentProcess(), sys)) {
            Logf("threads: the game already has one core (process mask %lX, system mask %lX)%s\n", (unsigned long)proc, (unsigned long)sys,
                 !g_cfg.widenAffinity ? "" : (sys & (sys - 1)) ? " - couldn't be widened" : " - the system shows only one");
            return;
        }
        Logf("threads: the process was on one core (mask %lX) - given all of them (mask %lX)\n", (unsigned long)proc, (unsigned long)sys);
        proc = sys;
    }
    g_gameCore = (was & (was - 1)) ? proc & (~proc + 1) : was;   // (the core it was pinned to, if it was)
    InitializeCriticalSection(&g_gtLock); g_gtLockInit = true;   // (before the hook: HkCreateThread uses it)
    if (!HookCreateThreadIn(GetModuleHandleA(nullptr), "the game")) {
        if (was != proc) SetProcessAffinityMask(GetCurrentProcess(), was);   // (can't pin the game's threads: as it was)
        g_gameCore = 0; return;
    }
    bool ll = HookLoadLibrary();
    SetThreadAffinityMask(GetCurrentThread(), g_gameCore);      // DllMain runs on the game's main thread
    NoteGameThread(GetCurrentThread());
    int core = 0; while (!((g_gameCore >> core) & 1)) core++;
    Logf("threads: the game's threads on core %d, the rest on all cores%s\n", core,
         ll ? "" : " (LoadLibraryA import not found: eacsnd.dll's threads stay free)");
}

// The game has one box for everything that goes wrong while it starts: "Need
// For Speed files are corrupted; please re-install", with a number in its title
// ("Warning #3") that says what failed (0x504530: eax = language, edx = the
// number). The log gets the number, where the box was asked for, and what the
// game's code shows that number to mean there.
extern "C" void __cdecl StartupErrorNote(DWORD code, DWORD ret) {
    const char* what = "?";
    switch (ret) {
        case 0x4F7E81: case 0x4F7EC1: what = "the game's version text couldn't be read"; break;
        case 0x4F80B5: what = "the install folder, level or CD drive couldn't be read (registry / portableInstall)"; break;
        case 0x4F80DF: what = "a data file the game opens first wasn't found (0x450590)"; break;
        case 0x503F85: what = "no \"3D Device Description\" (registry)"; break;
        case 0x503F9B: what = "no \"Thrash Driver\" or \"D3D Device\" (registry / renderer=)"; break;
        case 0x503FEE: what = "\"Thrash Driver\" isn't softtria.dll, voodoo2a.dll or d3da.dll (or the start of one)"; break;
        case 0x504519: what = "the renderer DLL couldn't be loaded or started"; break;
        case 0x504A4E: case 0x504A66: what = "a later startup step failed"; break;
    }
    Logf("startup: the game's \"files are corrupted\" box, Warning #%lu, asked for at 0x%06lX: %s\n",
         (unsigned long)code, (unsigned long)ret - 5, what);
}
extern "C" void StartupErrorStub();
asm(".text\n"
    ".globl _StartupErrorStub\n"
    "_StartupErrorStub:\n"            // jmp from 0x504530; [esp] = the caller's return address
    "    pushal\n"
    "    pushl 32(%esp)\n"
    "    pushl %edx\n"
    "    call _StartupErrorNote\n"
    "    addl $8, %esp\n"
    "    popal\n"
    "    pushl %ecx\n"                // the six bytes the jmp replaced
    "    pushl %esi\n"
    "    pushl %edi\n"
    "    pushl %ebp\n"
    "    movl %esp, %ebp\n"
    "    pushl $0x504536\n"
    "    ret\n"
);
static void InstallStartupFixes() {
    if (!g_knownExe) return;
    {
        BYTE j[6] = { 0xE9, 0, 0, 0, 0, 0x90 };
        DWORD rel = (DWORD)&StartupErrorStub - (0x504530 + 5); memcpy(j + 1, &rel, 4);
        if (!PatchBytes(0x504530, j, 6)) Logf("startup: error box logging - patch FAILED\n");
    }
    // [0x825814] is set at 0x4F7E58, just ahead of the argument reader and the CD checks
    if (Rd<DWORD>(0x825814)) Logf("startup: the game had already begun starting when the mod was loaded - "
                                  "the startup fixes below may come too late (the ASI loader decides)\n");
    static const BYTE je[6] = { 0x0F, 0x84, 0xF2, 0x00, 0x00, 0x00 };                // 0x4F833A: je 0x4F8432
    static const BYTE jeOk[6] = { 0x0F, 0x84, 0x71, 0x02, 0x00, 0x00 };              //           je 0x4F85B1
    if (!IsBadReadPtr((void*)0x4F833A, 6) && !memcmp((void*)0x4F833A, je, 6))
        Logf("startup arguments: %s\n", PatchBytes(0x4F833A, jeOk, 6) ? "\"Invalid argument\" with no arguments fixed" : "patch FAILED");
}
static void InstallNoCD() {
    if (!g_knownExe) return;
    static const BYTE jne[2] = { 0x75, 0x22 }, jmp[2] = { 0xEB, 0x7F };              // 0x4F7F24: jne 0x4F7F48 -> jmp 0x4F7FA5
    static const BYTE call[5] = { 0xE8, 0xA4, 0xCA, 0x00, 0x00 }, nops[5] = { 0x90, 0x90, 0x90, 0x90, 0x90 };   // 0x4F80C7: call 0x504B70
    bool has1 = !IsBadReadPtr((void*)0x4F7F24, 2) && !memcmp((void*)0x4F7F24, jne, 2);
    bool has2 = !IsBadReadPtr((void*)0x4F80C7, 5) && !memcmp((void*)0x4F80C7, call, 5);
    if (!has1 && !has2) { Logf("CD check: none in this exe\n"); return; }
    if (!g_cfg.noCDCheck) { Logf("CD check: on (the game's)\n"); return; }
    bool ok = (!has1 || PatchBytes(0x4F7F24, jmp, 2)) && (!has2 || PatchBytes(0x4F80C7, nops, 5));
    Logf("CD check: %s\n", ok ? "off" : "patch FAILED");
}
static void InstallDiskSpaceFix() {
    if (!g_knownExe) return;
    if (!g_cfg.noDiskSpaceCheck) { Logf("disk space check: on (the game's)\n"); return; }
    static const BYTE yes[6] = { 0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3 };               // mov eax, 1 / ret
    Logf("disk space check: %s\n", PatchBytes(0x49E800, yes, 6) ? "off" : "patch FAILED");
}

static void InstallCloseFix() {
    if (!g_knownExe || !g_cfg.closeExits) return;
    bool q = false, d = false;
    ForEachImport("USER32.dll", [&](void** slot, const char* nm) {
        if (!strcmp(nm, "PostQuitMessage")) q = PatchPointer(slot, (void*)&HkPostQuitMessage, (void**)&g_oPostQuit);
        else if (!strcmp(nm, "DestroyWindow")) d = PatchPointer(slot, (void*)&HkDestroyWindow, (void**)&g_oDestroyWindow);
    });
    if (!d) g_oDestroyWindow = nullptr;
    Logf("close fix: %s\n", q && d ? "on" : "imports not found - off");
}

static void InstallWidescreen() {
    g_dispW = GetSystemMetrics(SM_CXSCREEN); g_dispH = GetSystemMetrics(SM_CYSCREEN);
    if (g_knownExe && !g_cfg.widescreen && g_cfg.alternatePixelCenter) {   // (the draw wrappers alone)
        ForEachImport("KERNEL32.dll", [&](void** slot, const char* nm) {
            if (!strcmp(nm, "GetProcAddress")) PatchPointer(slot, (void*)&HkGetProcAddress, (void**)&g_oGPA);
        });
        if (!g_oGPA) Logf("pixel centre: GetProcAddress import not found - off\n");
    }
    if (!g_knownExe || !g_cfg.widescreen) { Logf("widescreen: off\n"); return; }
    // VEG's exe has its own widescreen patch: put its sites back first
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
    if (g_cfg.wsUnlockResolutions) {
        BYTE b = 0x00; ok &= PatchBytes(ws::modeFilterRel, &b, 1);   // options list: accept any shape
        BYTE j[2] = { 0x90, 0xE9 };                                  // width lookup: any shape (je -> jmp)
        if (*(BYTE*)ws::modeLookupJe == 0x0F) ok &= PatchBytes(ws::modeLookupJe, j, 2);
        ok &= PatchRel32(ws::callLookupSaved, 0xE8, (void*)&WsFindModeStub);
    }
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
        for (DWORD at : ws::hudFitW) if (*(WORD*)at == 0x05DB) ok &= PatchBytes(at, f, 6);
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
        {
            static unsigned long lx = 0, lbc = 0, lmv = 0;
            unsigned long x = g_wsXfDraws, bc = g_wsBarClears, mv = g_wsMovieFrames;
            if (g_cfg.wsLog && (x != lx || mv != lmv))
                Logf("  widescreen: menu draws mapped %lu, bar clears %lu, movie frames scaled %lu\n",
                     x - lx, bc - lbc, mv - lmv);
            lx = x; lbc = bc; lmv = mv;
        }
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
        InitializeCriticalSection(&g_iniLock); g_iniLockInit = true;
        QueryPerformanceFrequency(&g_qpf);
        LoadConfig();
        OpenLog();
        g_knownExe = CheckExe();
        InstallStartupFixes();                    // (first: these run right at the game's start)
        InstallGameThreadsOneCore();
        InstallDriversFolder();
        InstallNoCD();
        InstallPortable();
        InstallDiskSpaceFix();
        timeBeginPeriod(1);
        InstallTickScheduler();
        InstallInterpolation();
        InstallMenuUnlock();
        InitializeCriticalSection(&g_qLock);
        g_qEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);
        InstallLoadingAndFov();
        InstallGlideMenuEffects();
        InstallWidescreen();
        InstallCloseFix();
        if (g_qEvent) StartModThread(WriterThread);
        if (g_log) StartModThread(StatsThread);
    } else if (reason == DLL_PROCESS_DETACH) {
        if (reserved) g_logLockInit = false;
        if (g_log) { Logf("=== shutdown ===\n"); fclose(g_log); g_log = nullptr; }
    }
    return TRUE;
}
