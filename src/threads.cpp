#include "common.h"
#include "threads.h"
#include "config.h"
#include "log.h"
#include "helpers.h"
#include "fov.h"

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
void AffinityWatch() {
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
void InstallDriversFolder() {
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
void InstallGameThreadsOneCore() {
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
