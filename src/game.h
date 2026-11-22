// ---------------------------------------------------------------------------
// Game addresses
// ---------------------------------------------------------------------------
#pragma once
#include "common.h"

namespace hs {
    // timer
    const DWORD fnTimerCallback = 0x548850;   // TIMECALLBACK (stdcall, ret 0x14)
    const DWORD pTimerPeriod    = 0x9E6D00;   // ms per tick, 16.16 fixed point
    const DWORD fnTickHandler   = 0x459E10;   // registered handler (128 Hz)
    const DWORD immRegHandler   = 0x459E7C;   // imm32 of "mov eax, 0x459E10" (register)
    const DWORD immUnregHandler = 0x459EC0;   // imm32 of "mov eax, 0x459E10" (unregister)
    const DWORD pCount64        = 0x6813B0;   // 64 Hz counter
    // main loop
    const DWORD fnSimStep       = 0x51BBE0;
    const DWORD callSimStep[4]  = { 0x51CA61, 0x51CA70, 0x51CA75, 0x51CAC3 };
    const DWORD fnRender        = 0x5113E0;
    const DWORD callRender      = 0x51CB83;
    const DWORD skipRenderAt    = 0x51CB64;   // mov edi,[0x853B7C] / test / jne 0x51CB88
    const DWORD skipRenderCont  = 0x51CB6E;   // normal render decision
    const DWORD skipRenderOut   = 0x51CB88;   // after render
    const DWORD pSimTarget      = 0x853B30;   // target step (64 Hz units)
    const DWORD pSimStepIdx     = 0x853B34;   // next step index
    const DWORD pNoRender       = 0x853B7C;
    const DWORD pRenderArg      = 0x853B24;
    // objects
    const DWORD pCarCount       = 0x661A60;
    const DWORD pCarList        = 0x661A88;   // Car*[16]
    const DWORD pCamTable       = 0x660DF0;   // Camera*[32 types][2 views]
    const DWORD pCamActive      = 0x661950;   // Camera*[2]
    const DWORD pCamOverride    = 0x661958;   // Camera*[2]
    const DWORD pLookMode       = 0x661968;   // int[2]: 0 ahead, 1 back, 2/3 sides
    // front end
    const DWORD menuWaitAt      = 0x538185;   // mov eax,[0x9E6D54] - start of the 2-tick wait
    const DWORD menuWaitDone    = 0x5381A9;   // mov eax,[ebp-8] - after the wait
    // loading bar
    const DWORD fnLoadBar       = 0x4E7B80;   // void LoadBar(float percent), ret 4
    // FOV
    const DWORD fnGetFov        = 0x539E10;   // int GetViewFov()
    const DWORD callGetFov[3]   = { 0x539FA6, 0x539FCB, 0x539FEB };
    const DWORD pSelectedCam    = 0x7C3CA8;   // camera type the player picked, per view
    const DWORD dashFovAt       = 0x4D1DA8;   // mov edx, 80 -> cockpit projection FOV
    const DWORD pCopterCount    = 0x681DD8;
    const DWORD pCopterList     = 0x681DD4;

    struct Sig { DWORD addr; const char* bytes; int len; };
    const Sig kSigs[] = {
        { 0x49E800, "\x52\x55\x89\xE5\x81\xEC\x1C\x01\x00\x00", 10 },          // free disk space test
        { 0x504530, "\x51\x56\x57\x55\x89\xE5\x83\xEC\x3C", 9 },                // the "files are corrupted" box
        { 0x548850, "\x53\x56\x57\x8B\x15\x2C\x6D\x9E\x00\xA1\x00\x6D", 12 }, // timer callback
        { 0x548A46, "\xA3\x00\x6D\x9E\x00", 5 },                              // period store
        { 0x459E7B, "\xB8\x10\x9E\x45\x00", 5 },                              // register handler
        { 0x459EBF, "\xB8\x10\x9E\x45\x00", 5 },                              // unregister handler
        { 0x51C7AE, "\xA1\xB0\x13\x68\x00", 5 },                              // c64 in main loop
        { 0x51BBE0, "\x53\x51\x52\x56\x57\x55\x89\xE5", 8 },                  // sim step
        { 0x51CA61, "\xE8\x7A\xF1\xFF\xFF", 5 },
        { 0x51CA70, "\xE8\x6B\xF1\xFF\xFF", 5 },
        { 0x51CA75, "\xE8\x66\xF1\xFF\xFF", 5 },
        { 0x51CAC3, "\xE8\x18\xF1\xFF\xFF", 5 },
        { 0x5113E0, "\x53\x51\x52\x55\x89\xE5\x83\xEC", 8 },                  // render
        { 0x51CB64, "\x8B\x3D\x7C\x3B\x85\x00\x85\xFF\x75\x1A", 10 },         // skip-render branch
        { 0x51CB79, "\x89\x7D\xFC", 3 },
        { 0x51CB7E, "\xA1\x24\x3B\x85\x00\xE8\x58\x48\xFF\xFF", 10 },         // render call
        { 0x51CB88, "\x83\x3D\xEC\x4B\x7C\x00\x04", 7 },
        { 0x51C7CB, "\xA1\x30\x3B\x85\x00", 5 },                              // sim target
        { 0x51C9AE, "\x8B\x35\x30\x3B\x85\x00", 6 },
        { 0x51C9EF, "\xF6\x05\x34\x3B\x85\x00\x01", 7 },                      // step index
        { 0x44444C, "\x89\x04\x95\x88\x1A\x66\x00", 7 },                      // car list
        { 0x50FCAE, "\x8B\x35\x60\x1A\x66\x00", 6 },                          // car count
        { 0x50F8E9, "\x83\xC0\x08", 3 },                                      // car+8 position
        { 0x50F94C, "\x83\xC0\x34", 3 },                                      // car+0x34 rotation
        { 0x43CCC5, "\x89\xB4\xD0\xF0\x0D\x66\x00", 7 },                      // camera table
        { 0x43BE6A, "\x8D\x70\x14", 3 },                                      // cam+0x14 position
        { 0x43BF48, "\x8D\x70\x2C", 3 },                                      // cam+0x2C rotation
        { 0x43C30D, "\x8B\x80\x58\x19\x66\x00", 6 },                          // camera override
        { 0x43C315, "\x8B\x80\x50\x19\x66\x00", 6 },                          // active camera
        { 0x50FDC1, "\x3B\x05\xD8\x1D\x68\x00", 6 },                          // copter count
        { 0x50FDCF, "\x8B\x04\x85\xD4\x1D\x68\x00", 7 },                      // copter list
        { 0x4416E2, "\x89\x1C\x85\x68\x19\x66\x00", 7 },                      // look mode
        { 0x538179, "\xFF\x15\xF0\x7F\x9E\x00\xFF\x15\xE0\x7F\x9E\x00\xA1\x54\x6D\x9E\x00"
                    "\x89\x45\xF8\xA1\xBC\xA5\x8A\x00\x83\xC0\x02\x3B\x45\xF8\x7E\x0F"
                    "\xA1\x54\x6D\x9E\x00\x89\x45\xF8\xE8\xB9\xE2\xFE\xFF\xEB\xE4\x8B\x45\xF8", 51 }, // menu present + wait
        { 0x4E7B80, "\x53\x51\x52\x56\x57\x55\x89\xE5\x83\xEC\x0C\x31\xC0\xE8", 14 },   // loading bar
        { 0x4E7F8F, "\xC2\x04\x00", 3 },
        { 0x539E10, "\x52\x55\x89\xE5\x83\xEC\x04\xBA\x5A\x00\x00\x00", 12 },       // GetViewFov (90)
        { 0x4D1DA8, "\xBA\x50\x00\x00\x00\xB8\x10\x3A\x85\x00", 10 },          // cockpit FOV 80
        { 0x43DEBE, "\x89\x04\x95\xA8\x3C\x7C\x00", 7 },                       // selected camera slot
        { 0x528C43, "\xF6\x05\xC0\x87\x82\x00\x08\x74\x35", 9 },                // menu colours: D3D halves them
        { 0x5361CD, "\xF6\x05\xC0\x87\x82\x00\x08\x0F\x84\xA1\x00\x00\x00", 13 },
        { 0x49E40E, "\x8A\x1D\xC0\x87\x82\x00\xF6\xC3\x08\x0F\x85\x59\x02\x00\x00", 15 }, // menu glow
        { 0x4AAF39, "\x8A\x0D\xC0\x87\x82\x00\xF6\xC1\x08\x75\x05", 11 },
        { 0x4AAF9B, "\x83\xFE\x02\x7D\x07\xC7\x45\xEC\x02\x00\x00\x00", 12 },   // menu item reveal: min 2 ticks
        { 0x539F8A, "\x89\x45\xFC", 3 },                                      // view -> [ebp-4]
        { 0x539FA6, "\xE8\x65\xFE\xFF\xFF", 5 },
        { 0x539FCB, "\xE8\x40\xFE\xFF\xFF", 5 },
        { 0x539FEB, "\xE8\x20\xFE\xFF\xFF", 5 },
    };
}
