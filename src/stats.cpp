#include "common.h"
#include "stats.h"
#include "config.h"
#include "log.h"
#include "timing.h"
#include "interp.h"
#include "loading.h"
#include "widescreen.h"

// ---------------------------------------------------------------------------
// Stats thread
// ---------------------------------------------------------------------------
DWORD WINAPI StatsThread(LPVOID) {
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
