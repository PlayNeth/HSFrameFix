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

#include "src/common.h"
#include "src/config.h"
#include "src/log.h"
#include "src/helpers.h"
#include "src/timing.h"
#include "src/interp.h"
#include "src/fov.h"
#include "src/menus.h"
#include "src/widescreen.h"
#include "src/nightlamps.h"
#include "src/window.h"
#include "src/portable.h"
#include "src/threads.h"
#include "src/startup.h"
#include "src/stats.h"

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
        InstallNightLamps();
        InstallCloseFix();
        if (g_qEvent) StartModThread(WriterThread);
        if (g_log) StartModThread(StatsThread);
    } else if (reason == DLL_PROCESS_DETACH) {
        if (reserved) g_logLockInit = false;
        if (g_log) { Logf("=== shutdown ===\n"); fclose(g_log); g_log = nullptr; }
    }
    return TRUE;
}
