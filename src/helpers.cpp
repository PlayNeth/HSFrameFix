#include "common.h"
#include "helpers.h"
#include "game.h"
#include "config.h"
#include "log.h"

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
bool PatchBytes(DWORD at, const void* bytes, size_t len) {
    DWORD old;
    if (!VirtualProtect((void*)at, len, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy((void*)at, bytes, len);
    DWORD d; VirtualProtect((void*)at, len, old, &d);
    FlushInstructionCache(GetCurrentProcess(), (void*)at, len);
    return true;
}
bool PatchRel32(DWORD at, BYTE op, void* target) {   // E8 call / E9 jmp
    BYTE b[5]; b[0] = op;
    DWORD rel = (DWORD)target - (at + 5);
    memcpy(b + 1, &rel, 4);
    return PatchBytes(at, b, 5);
}
bool PatchPointer(void** slot, void* nv, void** ov) {
    DWORD old;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
    if (ov) *ov = *slot;
    *slot = nv;
    DWORD d; VirtualProtect(slot, sizeof(void*), old, &d);
    return true;
}
// This mod's own threads run on every core the process has, even if they're
// created from a game thread pinned to one core (gameThreadsOneCore).
HANDLE g_modThreads[4]; int g_modThreadN = 0;   // (DllMain only)
HANDLE StartModThread(LPTHREAD_START_ROUTINE fn) {
    HANDLE h = CreateThread(nullptr, 0, fn, nullptr, CREATE_SUSPENDED, nullptr);
    if (!h) return nullptr;
    DWORD_PTR proc = 0, sys = 0;
    if (GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys) && proc) SetThreadAffinityMask(h, proc);
    if (g_modThreadN < 4) g_modThreads[g_modThreadN++] = h;
    ResumeThread(h);
    return h;
}
char g_gameDir[MAX_PATH] = {};             // the folder the game's exe is in, with the closing backslash
bool InitGameDir() {
    if (g_gameDir[0]) return true;
    if (!GetModuleFileNameA(nullptr, g_gameDir, MAX_PATH)) g_gameDir[0] = 0;
    char* sl = strrchr(g_gameDir, '\\');
    if (!sl) { g_gameDir[0] = 0; return false; }
    sl[1] = 0;
    return true;
}

bool g_knownExe = false;
bool CheckExe() {
    if (GetModuleHandleA(nullptr) != (HMODULE)0x400000) {
        Logf("exe check: unexpected image base - disabled\n"); return false;
    }
    int bad = 0, total = 0;
    for (const auto& s : hs::kSigs) {
        total++;
        if (IsBadReadPtr((void*)s.addr, s.len)) {
            Logf("exe check: 0x%06lX unreadable\n", (unsigned long)s.addr); bad++; continue;
        }
        if (!memcmp((void*)s.addr, s.bytes, s.len)) continue;
        bad++;
        char exp[3 * 24 + 1] = {}, got[3 * 24 + 1] = {};
        const BYTE* g = (const BYTE*)s.addr;
        size_t n = s.len < 24 ? s.len : 24;
        for (size_t i = 0; i < n; i++) {
            bool d = g[i] != (BYTE)s.bytes[i];
            snprintf(exp + i * 3, 4, "%02X%c", (BYTE)s.bytes[i], d ? '*' : ' ');
            snprintf(got + i * 3, 4, "%02X%c", g[i], d ? '*' : ' ');
        }
        Logf("exe check: 0x%06lX differs (* = changed byte)\n    expected %s\n    found    %s\n",
             (unsigned long)s.addr, exp, got);
    }
    if (!bad) {
        // Two supported builds share every byte the mod uses. They are told apart
        // by VEG's changes: its CPU detection rewrite at 0x41F405 (mov eax,
        // [0x5C3AAC] instead of the original cmp dword [0x5C3950], 0) and its
        // widescreen patch, which hooks the projection setup (0x53A086 jmp instead
        // of fstp [0x5B0954]). A VEG exe without its widescreen patch, or any
        // other mix, is not supported.
        const BYTE* c = (const BYTE*)0x41F405;
        const BYTE* w = (const BYTE*)0x53A086;
        bool vegCode = c[0] == 0xA1, vanCode = c[0] == 0x83 && c[1] == 0x3D;
        bool vegWs = w[0] == 0xE9, vanWs = w[0] == 0xD9 && w[1] == 0x1D;
        if (vanCode && vanWs) {
            bool cd = Rd<BYTE>(0x4F7F24) == 0x75 || Rd<BYTE>(0x4F80C7) == 0xE8;
            Logf("exe check: nfs4.exe matches (%d checks, official 4.50 patch%s)\n", total,
                 cd ? "" : ", CD check removed");
            return true;
        }
        if (vegCode && vegWs) {
            Logf("exe check: nfs4.exe matches (%d checks, VEG's 4.50 patch)\n", total);
            return true;
        }
        Logf("exe check: all %d checks match, but this isn't a supported build "
             "(0x41F405: %02X %02X, 0x53A086: %02X %02X)\n", total, c[0], c[1], w[0], w[1]);
        if (g_cfg.exeCheck) {
            Logf("exe check: mod disabled (exeCheck=0 in the ini patches anyway)\n");
            return false;
        }
        Logf("exe check: exeCheck=0, patching anyway\n");
        return true;
    }
    if (g_cfg.exeCheck) {
        Logf("exe check: %d of %d checks differ - not the analysed nfs4.exe, mod disabled "
             "(exeCheck=0 in the ini patches anyway)\n", bad, total);
        return false;
    }
    Logf("exe check: %d of %d checks differ - exeCheck=0, patching anyway\n", bad, total);
    return true;
}
