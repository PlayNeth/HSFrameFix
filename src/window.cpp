#include "common.h"
#include "window.h"
#include "config.h"
#include "log.h"
#include "helpers.h"

// ---------------------------------------------------------------------------
// Closing the window from outside (Alt+F4, the window manager): the game's window
// thread posts WM_QUIT from WM_DESTROY (0x54E37B) and ends, but the game thread
// never finds out and keeps running, hangs, or crashes while shutting down without
// a window. The game destroys its own window only through DestroyWindow at
// 0x54EBBC (after its own quit), so a WM_DESTROY outside that call means the
// window was closed from outside: end the process right there.
// ---------------------------------------------------------------------------
typedef void (WINAPI *PostQuitMessage_t)(int);
typedef BOOL (WINAPI *DestroyWindow_t)(HWND);
static PostQuitMessage_t g_oPostQuit = nullptr;
static DestroyWindow_t g_oDestroyWindow = nullptr;
static volatile LONG g_ownDestroy = 0;
static BOOL WINAPI HkDestroyWindow(HWND h) {
    InterlockedIncrement(&g_ownDestroy);
    BOOL r = g_oDestroyWindow(h);
    InterlockedDecrement(&g_ownDestroy);
    return r;
}
static void WINAPI HkPostQuitMessage(int code) {
    if (!g_ownDestroy && g_oDestroyWindow) {
        Logf("window closed from outside the game - exiting\n");
        TerminateProcess(GetCurrentProcess(), 0);
    }
    g_oPostQuit(code);
}

void InstallCloseFix() {
    if (!g_knownExe || !g_cfg.closeExits) return;
    bool q = false, d = false;
    ForEachImport("USER32.dll", [&](void** slot, const char* nm) {
        if (!strcmp(nm, "PostQuitMessage")) q = PatchPointer(slot, (void*)&HkPostQuitMessage, (void**)&g_oPostQuit);
        else if (!strcmp(nm, "DestroyWindow")) d = PatchPointer(slot, (void*)&HkDestroyWindow, (void**)&g_oDestroyWindow);
    });
    if (!d) g_oDestroyWindow = nullptr;
    Logf("close fix: %s\n", q && d ? "on" : "imports not found - off");
}
