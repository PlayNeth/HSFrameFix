// What the other parts use from loading.cpp.
#pragma once
#include "common.h"

extern CRITICAL_SECTION g_loadLock;
void LoadStatsTick();
extern "C" void LoadBarStub();
