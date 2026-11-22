#include "common.h"
#include "timing.h"
#include "game.h"
#include "config.h"
#include "log.h"
#include "helpers.h"

// ---------------------------------------------------------------------------
// Time
// ---------------------------------------------------------------------------
LARGE_INTEGER g_qpf;

void WaitUntilOn(HANDLE timer, LONGLONG deadline) {
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
HANDLE CreateHiResTimer() {
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
std::atomic<unsigned long> g_tickFires{0}, g_tickResyncs{0};
static bool g_ticksActive = false;

LONGLONG TickPeriodQpc() {
    DWORD fx = Rd<DWORD>(hs::pTimerPeriod);   // ms << 16
    if (fx < 0x10000 || fx > 1000u * 0x10000u) fx = (1000u << 16) / 128;
    return (LONGLONG)((double)fx / 65536.0 * (double)g_qpf.QuadPart / 1000.0 + 0.5);
}

// ---------------------------------------------------------------------------
// hitchLog: what led up to a hitch. Every timer tick, simulation step and frame
// is noted with its time in a ring; when a frame finds the newest step late
// (the interpolator's clamp: the picture holds still for that frame), the
// second before it and a little after are written to HSFrameFix_hitch.txt
// next to the .asi (by the log writer's thread), with the longest gaps. Tells
// a timer that came late (something else had the processor) from a frame or a
// step that took long.
//   T  the 128 Hz tick, v = how late it woke (microseconds)
//   t  the game's tick handler done, v = how long it took (microseconds)
//   S  a simulation step done, v = its number
//   F  a frame about to be drawn
//   C  a frame that found the newest step late, v = by how much (microseconds)
// ---------------------------------------------------------------------------
struct HitchEv { LONGLONG t; long v; char k; };
static const unsigned kHitchRing = 4096;
static HitchEv g_hitch[kHitchRing];
static std::atomic<unsigned> g_hitchN{0};
static std::atomic<LONGLONG> g_hitchAt{0};
static LONGLONG g_hitchLast = 0; static int g_hitchDumps = 0;
void HitchNote(char kind, LONGLONG t, long v) {
    if (!g_cfg.hitchLog) return;
    HitchEv& e = g_hitch[g_hitchN.fetch_add(1) % kHitchRing];
    e.t = t; e.v = v; e.k = kind;
}
void HitchClamp(double aheadMs) {
    if (!g_cfg.hitchLog) return;
    LONGLONG now = Qpc();
    HitchNote('C', now, (long)(aheadMs * 1000.0));
    if (aheadMs < 1.0 || aheadMs > 300.0) return;                // (a pause or a loading stop is no hitch)
    if (g_hitchAt.load() || (g_hitchLast && now - g_hitchLast < 2 * g_qpf.QuadPart)) return;
    g_hitchAt = now;
}
void HitchWrite() {
    LONGLONG at = g_hitchAt.load();
    if (!at || Qpc() - at < g_qpf.QuadPart * 3 / 10) return;      // (0.3 s more first)
    static HitchEv ev[kHitchRing];
    unsigned n = g_hitchN.load(), cnt = n < kHitchRing ? n : kHitchRing, m = 0;
    for (unsigned i = 0; i < cnt; i++) {
        const HitchEv& e = g_hitch[(n - cnt + i) % kHitchRing];
        if (e.t >= at - g_qpf.QuadPart && e.t <= at + g_qpf.QuadPart * 3 / 10) ev[m++] = e;
    }
    for (unsigned i = 1; i < m; i++)                              // by time (two threads write the ring)
        for (unsigned j = i; j > 0 && ev[j].t < ev[j - 1].t; j--) { HitchEv x = ev[j]; ev[j] = ev[j - 1]; ev[j - 1] = x; }
    g_hitchLast = at; g_hitchAt = 0;
    if (g_hitchDumps >= 60) return;
    char path[MAX_PATH]; snprintf(path, sizeof(path), "%sHSFrameFix_hitch.txt", g_asiDir);
    FILE* f = fopen(path, g_hitchDumps++ ? "a" : "w");
    if (!f) return;
    SYSTEMTIME st; GetLocalTime(&st);
    double gap[3] = {}, late = 0, handler = 0; LONGLONG last[3] = {};
    for (unsigned i = 0; i < m; i++) {
        int w = ev[i].k == 'T' ? 0 : ev[i].k == 'S' ? 1 : ev[i].k == 'F' ? 2 : -1;
        if (w >= 0) { if (last[w] && QpcMs(ev[i].t - last[w]) > gap[w]) gap[w] = QpcMs(ev[i].t - last[w]); last[w] = ev[i].t; }
        if (ev[i].k == 'T' && ev[i].v / 1000.0 > late) late = ev[i].v / 1000.0;
        if (ev[i].k == 't' && ev[i].v / 1000.0 > handler) handler = ev[i].v / 1000.0;
    }
    fprintf(f, "HITCH %d written %02d:%02d:%02d.%03d: longest gap between ticks %.2f ms (7.81 is on time), steps %.2f ms (15.63), "
               "frames %.2f ms; latest tick %.2f ms late, longest tick handler %.2f ms\n", g_hitchDumps, st.wHour, st.wMinute,
            st.wSecond, st.wMilliseconds, gap[0], gap[1], gap[2], late, handler);
    for (unsigned i = 0; i < m; i++)
        fprintf(f, "%+9.2f %c %ld\n", QpcMs(ev[i].t - at), ev[i].k, ev[i].v);
    fclose(f);
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
        LONGLONG woke = Qpc();
        HitchNote('T', woke, (long)(QpcMs(woke - next) * 1000.0));
        cb(kFakeId, 0, 0, 0, 0);                      // re-arms itself via timeSetEvent
        if (g_cfg.hitchLog) { LONGLONG done = Qpc(); HitchNote('t', done, (long)(QpcMs(done - woke) * 1000.0)); }
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

void InstallTickScheduler() {
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
