#include "common.h"
#include "loading.h"
#include "config.h"
#include "log.h"
#include "timing.h"
#include "widescreen.h"

// ---------------------------------------------------------------------------
// Loading bar throttle
// ---------------------------------------------------------------------------
extern "C" { int g_loadDraw = 1; DWORD g_fnPoll = 0x54A790, g_fnGetFov = 0x539E10; }
static LONGLONG g_loadLastCall = 0, g_loadLastDraw = 0, g_loadStart = 0;
static unsigned long g_loadCalls = 0, g_loadDraws = 0;
static bool g_loadActive = false;
CRITICAL_SECTION g_loadLock;

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
    if (draw) WsLoadBegin();
}
void LoadStatsTick() {                 // called by the stats thread
    EnterCriticalSection(&g_loadLock);
    if (g_loadActive && Qpc() - g_loadLastCall > 2 * g_qpf.QuadPart) {
        Logf("load: %lu loading-bar updates, %lu drawn, first->last %.0f ms\n", g_loadCalls,
             g_loadDraws, QpcMs(g_loadLastCall - g_loadStart));
        g_loadActive = false;
    }
    LeaveCriticalSection(&g_loadLock);
}
asm(".text\n"
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
    "3:  pushl 4(%esp)\n"             // drawn: the game's own, then the loading screen's 4:3 mapping off
    "    call 4f\n"
    "    pushal\n"
    "    call _WsLoadEnd\n"
    "    popal\n"
    "    ret $4\n"
    "4:  pushl %ebx\n"                 // original prologue, then continue
    "    pushl %ecx\n"
    "    pushl %edx\n"
    "    pushl %esi\n"
    "    pushl %edi\n"
    "    pushl %ebp\n"
    "    pushl $0x4E7B86\n"
    "    ret\n");
