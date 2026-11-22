#include "common.h"
#include "fov.h"
#include "nightlamps.h"
#include "game.h"
#include "config.h"
#include "log.h"
#include "helpers.h"
#include "interp.h"
#include "loading.h"
#include "threads.h"
#include "timing.h"

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
CRITICAL_SECTION g_iniLock;
bool g_iniLockInit = false;
static void ReplaceIniLineLocked(const char* key, int value);
void ReplaceIniLine(const char* key, int value) {
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
void FovHotkeys() {
    if (g_cfg.nightLampsLog) {                    // (the night lamps' scene dump: see nightlamps.cpp)
        static bool was = false;
        int key = g_cfg.nightLampsLogKey;
        bool down = key > 0 && key < 256 && GameHasFocus() && (GetAsyncKeyState(key) & 0x8000);
        if (down && !was) NightLampsDump();
        was = down;
    }
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

DWORD WINAPI WriterThread(LPVOID) {
    for (;;) {
        WaitForSingleObject(g_qEvent, 1000);
        AffinityWatch();
        DrainLogQueue();
        HitchWrite();
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

extern "C" void FovStub();
extern "C" void DashFovStub();
asm(".text\n"
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

void InstallLoadingAndFov() {
    InitializeCriticalSection(&g_loadLock);
    if (!g_knownExe) return;
    bool wide = g_cfg.widescreen && g_cfg.wsLoading;         // (the loading screen's 4:3 shape starts from the same hook)
    if (g_cfg.speedUpLoading || wide) {
        bool ok = PatchRel32(hs::fnLoadBar, 0xE9, (void*)&LoadBarStub);
        if (!g_cfg.speedUpLoading) Logf("loading: %s\n", ok ? "original speed" : "patch FAILED");
        else Logf("loading: %s (loading bar redrawn at most every %d ms)\n",
                  ok ? "sped up" : "patch FAILED", g_cfg.loadScreenRenderMs);
    } else Logf("loading: original\n");
    bool ok = true;
    for (DWORD at : hs::callGetFov) ok &= PatchRel32(at, 0xE8, (void*)&FovStub);
    ok &= PatchRel32(hs::dashFovAt, 0xE8, (void*)&DashFovStub);
    Logf("camera FOV: %s (all %+d, in-car %+d, bumper %+d, heli %+d, roof %+d, hotkeys %s)\n",
         ok ? "hooked" : "patch FAILED", g_cfg.fovOffset, g_cfg.fovOffsetInCar, g_cfg.fovOffsetBumper,
         g_cfg.fovOffsetHeli, g_cfg.fovOffsetRoof, g_cfg.fovHotkeys ? "on" : "off");
}
