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

#define HSFF_VERSION "0.1"

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
    int   logToFile        = 1;
    int   logEverySeconds  = 10;
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

static void LoadConfig() {
    InitAsiDir();
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%sHSFrameFix.ini", g_asiDir);
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
                  "logToFile=1\n"
                  "logEverySeconds=10\n", f);
            fclose(f);
        }
        return;
    }
    char line[256];
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
        else if (!_stricmp(key, "logToFile"))         g_cfg.logToFile = atoi(val);
        else if (!_stricmp(key, "logEverySeconds"))   g_cfg.logEverySeconds = atoi(val);
    }
    fclose(f);
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

static void OpenLog() {
    if (!g_cfg.logToFile) return;
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%sHSFrameFix.log", g_asiDir);
    g_log = fopen(path, "w");
    Logf("HSFrameFix v" HSFF_VERSION " | interpolate=%d copter=%d preciseTicks=%d targetFPS=%d "
         "maxStepDistance=%.1f maxStepAngle=%.0f\n", g_cfg.interpolate, g_cfg.interpolateCopter,
         g_cfg.preciseTicks, g_cfg.targetFPS, g_cfg.maxStepDistance, g_cfg.maxStepAngle);
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
    for (const auto& s : hs::kSigs) {
        if (IsBadReadPtr((void*)s.addr, s.len) || memcmp((void*)s.addr, s.bytes, s.len)) {
            Logf("exe check: bytes at 0x%06lX differ - not the analysed nfs4.exe, disabled\n",
                 (unsigned long)s.addr);
            return false;
        }
    }
    Logf("exe check: nfs4.exe matches\n");
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
// 64 Hz tick timestamps (wrapper around the game's tick handler)
// ---------------------------------------------------------------------------
struct TickStamp { long c64; LONGLONG t; };
static volatile LONG g_stampSeq = 0;
static TickStamp g_stamp = { -1, 0 };
static long g_lastC64Seen = -1;

extern "C" void __cdecl OnTickHandled() {
    long c = Rd<long>(hs::pCount64);
    if (c == g_lastC64Seen) return;
    g_lastC64Seen = c;
    LONGLONG t = g_ticksActive ? g_lastGridTime.load() : Qpc();
    InterlockedIncrement(&g_stampSeq);            // odd = writing
    g_stamp.c64 = c; g_stamp.t = t;
    InterlockedIncrement(&g_stampSeq);
}
static TickStamp ReadStamp() {
    for (;;) {
        LONG s1 = g_stampSeq; MemoryBarrier();
        TickStamp v = g_stamp; MemoryBarrier();
        if (!(s1 & 1) && s1 == g_stampSeq) return v;
        YieldProcessor();
    }
}

// ---------------------------------------------------------------------------
// Transform snapshots
// ---------------------------------------------------------------------------
struct Xform { float p[3]; float m[9]; };
struct Entry { DWORD obj; int kind; Xform x; };    // kind: 0 car, 1 camera, 2 copter (pos only)
static const int kMaxEntries = 128;
struct Snapshot { int n = 0; long step = 0; DWORD activeCam[2] = {0, 0}; Entry e[kMaxEntries]; };

static Snapshot g_snap[2];       // [g_cur] latest, [g_cur^1] previous
static int  g_cur = 0;
static int  g_snapCount = 0;     // valid snapshots (0..2)
static long g_lastStepIdx = 0;
// step-index -> 64 Hz tick offset, minimum over a window (= on-time steps)
static long g_offHist[64]; static int g_offPos = 0, g_offN = 0;

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

extern "C" void __cdecl OnSimStep() {
    long idx = Rd<long>(hs::pSimStepIdx);           // index of the step that just ran
    if (g_snapCount && idx == g_lastStepIdx) {       // replay fast-forward: 2 steps per index
        Collect(g_snap[g_cur]);
        g_steps++;
        return;
    }
    if (g_snapCount && (idx < g_lastStepIdx || idx - g_lastStepIdx > 16)) {
        g_snapCount = 0; g_offN = 0; g_resets++;     // restart / jump: no blending across it
    }
    g_lastStepIdx = idx;
    g_cur ^= 1;
    Snapshot& s = g_snap[g_cur];
    Collect(s);
    s.step = idx;
    if (g_snapCount < 2) g_snapCount++;
    // offset between this step and the 64 Hz counter
    long c = Rd<long>(hs::pCount64);
    g_offHist[g_offPos] = c - idx; g_offPos = (g_offPos + 1) & 63; if (g_offN < 64) g_offN++;
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
static Xform    g_written[kMaxEntries];
static bool     g_wrote[kMaxEntries];
static bool g_applied = false;
static HANDLE g_limTimer = nullptr;
static LONGLONG g_limNext = 0;
static std::atomic<unsigned long> g_renders{0}, g_extraRenders{0}, g_blended{0}, g_rejected{0};
static double g_alphaSum = 0; static unsigned long g_alphaN = 0, g_alphaClamped = 0;
extern "C" { int g_extraEnabled = 0; DWORD g_fnSimStep = 0x51BBE0, g_fnRender = 0x5113E0,
                                     g_fnTickHandler = 0x459E10; }

static void Limiter() {
    if (g_cfg.targetFPS <= 0) return;
    LONGLONG period = g_qpf.QuadPart / g_cfg.targetFPS, now = Qpc();
    if (g_limNext && now < g_limNext) { WaitUntilOn(g_limTimer, g_limNext); now = Qpc(); }
    if (!g_limNext || now - g_limNext > period) g_limNext = now + period; else g_limNext += period;
}

extern "C" void __cdecl PreRender() {
    Limiter();
    g_renders++;
    g_applied = false;
    if (!g_cfg.interpolate || g_snapCount < 2) return;
    const Snapshot& A = g_snap[g_cur ^ 1];
    const Snapshot& B = g_snap[g_cur];
    long span = B.step - A.step;
    if (span <= 0 || span > 8) return;

    // When was step B due? stepIdx + min(offset) = its 64 Hz tick.
    long offMin = 0x7fffffff;
    for (int i = 0; i < g_offN; i++) if (g_offHist[i] < offMin) offMin = g_offHist[i];
    TickStamp ts = ReadStamp();
    if (ts.c64 < 0 || offMin == 0x7fffffff) return;
    LONGLONG p64 = TickPeriodQpc() * 2;
    LONGLONG dueB = ts.t - (LONGLONG)(ts.c64 - (B.step + offMin)) * p64;
    LONGLONG dueA = dueB - (LONGLONG)span * p64;
    LONGLONG renderT = Qpc() - p64;                  // show the world one step behind
    float t = (float)((double)(renderT - dueA) / (double)(dueB - dueA));
    if (t < 0.0f) { t = 0.0f; g_alphaClamped++; }
    if (t >= 1.0f) { t = 1.0f; g_alphaClamped++; }
    g_alphaSum += t; g_alphaN++;
    if (t >= 1.0f) return;                           // already at the latest state

    // Current live values (normally == B) are saved and restored after drawing.
    Collect(g_saved);
    for (int i = 0; i < g_saved.n; i++) {
        g_wrote[i] = false;
        const Entry& cur = g_saved.e[i];
        if (cur.kind == 1) {                           // camera cut: don't blend into it
            bool cut = false;
            for (int v = 0; v < 2; v++)
                if (cur.obj == g_saved.activeCam[v] && A.activeCam[v] != cur.obj) cut = true;
            if (cut) { g_rejected++; continue; }
        }
        const Entry* pa = nullptr;
        for (int j = 0; j < A.n; j++)
            if (A.e[j].obj == cur.obj && A.e[j].kind == cur.kind) { pa = &A.e[j]; break; }
        if (!pa) continue;
        Xform x;
        if (Blend(pa->x, cur.x, cur.kind, t, &x)) {
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
extern "C" void TickHandlerStub();
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
    // registered in place of the game's 128 Hz tick handler
    ".globl _TickHandlerStub\n"
    "_TickHandlerStub:\n"
    "    pushfl\n"
    "    pushal\n"
    "    call *_g_fnTickHandler\n"
    "    call _OnTickHandled\n"
    "    popal\n"
    "    popfl\n"
    "    ret\n");

static void InstallInterpolation() {
    if (!g_knownExe || !g_cfg.interpolate) { Logf("interpolation: off\n"); return; }
    bool ok = true;
    for (DWORD at : hs::callSimStep) ok &= PatchRel32(at, 0xE8, (void*)&StepStub);
    ok &= PatchRel32(hs::callRender, 0xE8, (void*)&RenderStub);
    DWORD stub = (DWORD)&TickHandlerStub;
    ok &= PatchBytes(hs::immRegHandler, &stub, 4);
    ok &= PatchBytes(hs::immUnregHandler, &stub, 4);
    BYTE j[6] = { 0xE9, 0, 0, 0, 0, 0x90 };
    DWORD rel = (DWORD)&ExtraRenderStub - (hs::skipRenderAt + 5);
    memcpy(j + 1, &rel, 4);
    ok &= PatchBytes(hs::skipRenderAt, j, 6);
    g_extraEnabled = ok ? 1 : 0;
    g_limTimer = CreateHiResTimer();
    Logf("interpolation: %s\n", ok ? "installed" : "patch FAILED");
}

// ---------------------------------------------------------------------------
// Stats thread
// ---------------------------------------------------------------------------
static DWORD WINAPI StatsThread(LPVOID) {
    unsigned long ls = 0, lr = 0, le = 0, lf = 0, lb = 0, lrej = 0;
    for (;;) {
        int sec = g_cfg.logEverySeconds > 0 ? g_cfg.logEverySeconds : 10;
        Sleep(sec * 1000);
        unsigned long s = g_steps, r = g_renders, e = g_extraRenders, f = g_tickFires,
                      b = g_blended, rj = g_rejected;
        if (s != ls || r != lr)
            Logf("last %ds: sim %.1f/s, frames %.1f/s (%.1f/s between steps), ticks %.1f/s, "
                 "blended %lu, not blended %lu, avg blend %.2f, clamped %lu, resets %lu\n",
                 sec, (s - ls) / (double)sec, (r - lr) / (double)sec, (e - le) / (double)sec,
                 (f - lf) / (double)sec, b - lb, rj - lrej,
                 g_alphaN ? g_alphaSum / g_alphaN : 0.0, g_alphaClamped, (unsigned long)g_resets);
        g_alphaSum = 0; g_alphaN = 0; g_alphaClamped = 0;
        ls = s; lr = r; le = e; lf = f; lb = b; lrej = rj;
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
        if (g_log) CreateThread(nullptr, 0, StatsThread, nullptr, 0, nullptr);
    } else if (reason == DLL_PROCESS_DETACH) {
        if (reserved) g_logLockInit = false;
        if (g_log) { Logf("=== shutdown ===\n"); fclose(g_log); g_log = nullptr; }
    }
    return TRUE;
}
