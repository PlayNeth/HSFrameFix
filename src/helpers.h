// What the other parts use from helpers.cpp.
#pragma once
#include "common.h"

bool PatchBytes(DWORD at, const void* bytes, size_t len);
bool PatchRel32(DWORD at, BYTE op, void* target);
bool PatchPointer(void** slot, void* nv, void** ov);
template <typename Fn>
static void ForEachImportIn(HMODULE mod, const char* dll, Fn cb) {
    BYTE* base = (BYTE*)mod;
    auto dos = (PIMAGE_DOS_HEADER)base;
    auto nt = (PIMAGE_NT_HEADERS)(base + dos->e_lfanew);
    DWORD rva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if (!rva) return;
    for (auto imp = (PIMAGE_IMPORT_DESCRIPTOR)(base + rva); imp->Name; ++imp) {
        if (_stricmp((const char*)(base + imp->Name), dll)) continue;
        auto th = (PIMAGE_THUNK_DATA)(base + imp->FirstThunk);
        auto on = (PIMAGE_THUNK_DATA)(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk
                                                                      : imp->FirstThunk));
        for (; th->u1.Function; ++th, ++on) {
            if (IMAGE_SNAP_BY_ORDINAL(on->u1.Ordinal)) continue;
            auto nm = (PIMAGE_IMPORT_BY_NAME)(base + on->u1.AddressOfData);
            cb((void**)&th->u1.Function, (const char*)nm->Name);
        }
    }
}
template <typename Fn>
static void ForEachImport(const char* dll, Fn cb) { ForEachImportIn(GetModuleHandleA(nullptr), dll, cb); }
extern HANDLE g_modThreads[4];
extern int g_modThreadN;
HANDLE StartModThread(LPTHREAD_START_ROUTINE fn);
template <typename T> static inline T Rd(DWORD a) { return *(volatile T*)a; }
extern char g_gameDir[MAX_PATH];
bool InitGameDir();
extern bool g_knownExe;
bool CheckExe();
