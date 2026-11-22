#include "common.h"
#include "log.h"
#include "config.h"

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------
FILE* g_log = nullptr;
CRITICAL_SECTION g_logLock;
bool g_logLockInit = false;

void Logf(const char* fmt, ...) {
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
CRITICAL_SECTION g_qLock;
HANDLE g_qEvent = nullptr;
static char g_q[64][192];
static int g_qHead = 0, g_qTail = 0;
void LogfAsync(const char* fmt, ...) {
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
void DrainLogQueue() {
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

void OpenLog() {
    if (!g_cfg.logToFile) return;
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%sHSFrameFix.log", g_asiDir);
    g_log = fopen(path, "w");
    Logf("HSFrameFix v" HSFF_VERSION " | interpolate=%d copter=%d preciseTicks=%d targetFPS=%d "
         "maxStepDistance=%.1f maxStepAngle=%.0f interpDelay=%.2f unlockMenus=%d menuTargetFPS=%d\n", g_cfg.interpolate,
         g_cfg.interpolateCopter, g_cfg.preciseTicks, g_cfg.targetFPS, g_cfg.maxStepDistance,
         g_cfg.maxStepAngle, g_cfg.interpDelay, g_cfg.unlockMenus, g_cfg.menuTargetFPS);
}
