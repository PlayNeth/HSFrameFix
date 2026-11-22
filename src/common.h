// What every part of HSFrameFix starts from: the system headers and the version.
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

#define HSFF_VERSION "0.8.1"
