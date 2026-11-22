#include "common.h"
#include "startup.h"
#include "config.h"
#include "log.h"
#include "helpers.h"

// ---------------------------------------------------------------------------
// Startup: the error box's number in the log, startup fixes, the CD and disk
// space checks
// ---------------------------------------------------------------------------
// The game has one box for everything that goes wrong while it starts: "Need
// For Speed files are corrupted; please re-install", with a number in its title
// ("Warning #3") that says what failed (0x504530: eax = language, edx = the
// number). The log gets the number, where the box was asked for, and what the
// game's code shows that number to mean there.
extern "C" void __cdecl StartupErrorNote(DWORD code, DWORD ret) {
    const char* what = "?";
    switch (ret) {
        case 0x4F7E81: case 0x4F7EC1: what = "the game's version text couldn't be read"; break;
        case 0x4F80B5: what = "the install folder, level or CD drive couldn't be read (registry / portableInstall)"; break;
        case 0x4F80DF: what = "a data file the game opens first wasn't found (0x450590)"; break;
        case 0x503F85: what = "no \"3D Device Description\" (registry)"; break;
        case 0x503F9B: what = "no \"Thrash Driver\" or \"D3D Device\" (registry / renderer=)"; break;
        case 0x503FEE: what = "\"Thrash Driver\" isn't softtria.dll, voodoo2a.dll or d3da.dll (or the start of one)"; break;
        case 0x504519: what = "the renderer DLL couldn't be loaded or started"; break;
        case 0x504A4E: case 0x504A66: what = "a later startup step failed"; break;
    }
    Logf("startup: the game's \"files are corrupted\" box, Warning #%lu, asked for at 0x%06lX: %s\n",
         (unsigned long)code, (unsigned long)ret - 5, what);
}
extern "C" void StartupErrorStub();
asm(".text\n"
    ".globl _StartupErrorStub\n"
    "_StartupErrorStub:\n"            // jmp from 0x504530; [esp] = the caller's return address
    "    pushal\n"
    "    pushl 32(%esp)\n"
    "    pushl %edx\n"
    "    call _StartupErrorNote\n"
    "    addl $8, %esp\n"
    "    popal\n"
    "    pushl %ecx\n"                // the six bytes the jmp replaced
    "    pushl %esi\n"
    "    pushl %edi\n"
    "    pushl %ebp\n"
    "    movl %esp, %ebp\n"
    "    pushl $0x504536\n"
    "    ret\n"
);
void InstallStartupFixes() {
    if (!g_knownExe) return;
    {
        BYTE j[6] = { 0xE9, 0, 0, 0, 0, 0x90 };
        DWORD rel = (DWORD)&StartupErrorStub - (0x504530 + 5); memcpy(j + 1, &rel, 4);
        if (!PatchBytes(0x504530, j, 6)) Logf("startup: error box logging - patch FAILED\n");
    }
    // [0x825814] is set at 0x4F7E58, just ahead of the argument reader and the CD checks
    if (Rd<DWORD>(0x825814)) Logf("startup: the game had already begun starting when the mod was loaded - "
                                  "the startup fixes below may come too late (the ASI loader decides)\n");
    static const BYTE je[6] = { 0x0F, 0x84, 0xF2, 0x00, 0x00, 0x00 };                // 0x4F833A: je 0x4F8432
    static const BYTE jeOk[6] = { 0x0F, 0x84, 0x71, 0x02, 0x00, 0x00 };              //           je 0x4F85B1
    if (!IsBadReadPtr((void*)0x4F833A, 6) && !memcmp((void*)0x4F833A, je, 6))
        Logf("startup arguments: %s\n", PatchBytes(0x4F833A, jeOk, 6) ? "\"Invalid argument\" with no arguments fixed" : "patch FAILED");
}
// The official exe's two startup CD checks get the bytes the CD-less exe has
// there, if they still hold the original's (VEG's exe and the CD-less one
// don't: nothing to do).
void InstallNoCD() {
    if (!g_knownExe) return;
    static const BYTE jne[2] = { 0x75, 0x22 }, jmp[2] = { 0xEB, 0x7F };              // 0x4F7F24: jne 0x4F7F48 -> jmp 0x4F7FA5
    static const BYTE call[5] = { 0xE8, 0xA4, 0xCA, 0x00, 0x00 }, nops[5] = { 0x90, 0x90, 0x90, 0x90, 0x90 };   // 0x4F80C7: call 0x504B70
    bool has1 = !IsBadReadPtr((void*)0x4F7F24, 2) && !memcmp((void*)0x4F7F24, jne, 2);
    bool has2 = !IsBadReadPtr((void*)0x4F80C7, 5) && !memcmp((void*)0x4F80C7, call, 5);
    if (!has1 && !has2) { Logf("CD check: none in this exe\n"); return; }
    if (!g_cfg.noCDCheck) { Logf("CD check: on (the game's)\n"); return; }
    bool ok = (!has1 || PatchBytes(0x4F7F24, jmp, 2)) && (!has2 || PatchBytes(0x4F80C7, nops, 5));
    Logf("CD check: %s\n", ok ? "off" : "patch FAILED");
}
void InstallDiskSpaceFix() {
    if (!g_knownExe) return;
    if (!g_cfg.noDiskSpaceCheck) { Logf("disk space check: on (the game's)\n"); return; }
    static const BYTE yes[6] = { 0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3 };               // mov eax, 1 / ret
    Logf("disk space check: %s\n", PatchBytes(0x49E800, yes, 6) ? "off" : "patch FAILED");
}
