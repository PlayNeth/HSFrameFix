// What the other parts use from fov.cpp.
#pragma once
#include "common.h"
#include "interp.h"

extern CRITICAL_SECTION g_iniLock;
extern bool g_iniLockInit;
void ReplaceIniLine(const char* key, int value);
void FovHotkeys();
DWORD WINAPI WriterThread(LPVOID);
void InstallLoadingAndFov();
