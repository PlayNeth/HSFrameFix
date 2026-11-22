// What the other parts use from timing.cpp.
#pragma once
#include "common.h"

extern LARGE_INTEGER g_qpf;
static inline LONGLONG Qpc() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }
static inline double QpcMs(LONGLONG d) { return (double)d * 1000.0 / (double)g_qpf.QuadPart; }
void WaitUntilOn(HANDLE timer, LONGLONG deadline);
HANDLE CreateHiResTimer();
extern std::atomic<unsigned long> g_tickFires, g_tickResyncs;
LONGLONG TickPeriodQpc();
void InstallTickScheduler();
void HitchNote(char kind, LONGLONG t, long v);
void HitchClamp(double aheadMs);
void HitchWrite();
