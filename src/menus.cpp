#include "common.h"
#include "menus.h"
#include "game.h"
#include "config.h"
#include "log.h"
#include "helpers.h"
#include "timing.h"
#include "interp.h"

// ---------------------------------------------------------------------------
// Menus: the frame cap lifted, the item reveal's timing, Glide's effects on any renderer
// ---------------------------------------------------------------------------
void InstallMenuUnlock() {
    if (!g_knownExe || !g_cfg.unlockMenus) { Logf("menus: original 64 fps cap\n"); return; }
    if (!g_limTimer) g_limTimer = CreateHiResTimer();
    bool ok = PatchRel32(hs::menuWaitAt, 0xE9, (void*)&MenuWaitStub);
    // The menu item reveal (and its Glide glow) advances by half the 128 Hz ticks
    // since the last frame but counts at least 2: right at 64 fps, too fast above.
    // When 0..1 tick has passed, skip the update and let the ticks add up instead;
    // negative (tick counter reset) keeps the original "count 2 and resync".
    // (Two other menu timers with the same clamp are left alone: they only differ
    // above 128 fps, and the list slide treats a zero step as "arrived".)
    bool ok2 = PatchRel32(0x4AAFA0, 0xE8, (void*)&MenuTimeStub1);
    Logf("menus: %s, menu animation timing %s\n", ok ? "unlocked" : "patch FAILED",
         ok2 ? "fixed" : "patch FAILED");
}

// The front end checks the renderer class ([0x8287C0], from the driver's thrash.ini
// Type=: d=8 Direct3D, v=4 Glide, s=1 software) in four places and drops effects on
// Direct3D. Make those four behave as on Glide; software keeps its own checks.
void InstallGlideMenuEffects() {
    if (!g_knownExe || !g_cfg.glideMenuEffects) { Logf("menu effects: original\n"); return; }
    static const BYTE jmp8[]   = { 0xEB };                               // 0x528C4A: je -> jmp
    static const BYTE jmp32[]  = { 0xE9, 0xA2, 0x00, 0x00, 0x00, 0x90 }; // 0x5361D4: je -> jmp 0x53627B
    static const BYTE nop6[]   = { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 }; // 0x49E417: drop "jne if D3D"
    static const BYTE nop2[]   = { 0x90, 0x90 };                         // 0x4AAF42: drop "jne if D3D"
    bool ok = PatchBytes(0x528C4A, jmp8, 1) && PatchBytes(0x5361D4, jmp32, 6) &&
              PatchBytes(0x49E417, nop6, 6) && PatchBytes(0x4AAF42, nop2, 2);
    Logf("menu effects: %s (Glide-style menu glow and colours on any renderer)\n",
         ok ? "on" : "patch FAILED");
}
