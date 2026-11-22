// What the other parts use from widescreen.cpp.
#pragma once
#include "common.h"

extern std::atomic<unsigned long> g_wsXfDraws, g_wsBarClears, g_wsMovieFrames;
void InstallWidescreen();
extern "C" void __cdecl WsLoadBegin();
extern "C" void __cdecl WsLoadEnd();
