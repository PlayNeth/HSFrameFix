// What the other parts use from interp.cpp.
#pragma once
#include "common.h"

extern std::atomic<unsigned long> g_clockSnaps;
DWORD EffectiveCam(int v);
extern std::atomic<unsigned long> g_steps, g_resets;
extern HANDLE g_limTimer;
extern std::atomic<unsigned long> g_renders, g_extraRenders, g_blended, g_rejected, g_cuts;
extern double g_alphaSum;
extern unsigned long g_alphaN, g_alphaClamped;
extern volatile bool g_clampHave;
extern SYSTEMTIME g_clampFirst;
extern double g_clampAheadMs, g_clampBehindMs;
extern std::atomic<unsigned long> g_menuFrames;
extern "C" void MenuWaitStub();
extern "C" void MenuTimeStub1();
void InstallInterpolation();
