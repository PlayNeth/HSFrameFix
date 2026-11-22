#include "common.h"
#include "widescreen.h"
#include "config.h"
#include "log.h"
#include "helpers.h"
#include "fov.h"

// ---------------------------------------------------------------------------
// Widescreen
//
// Menus: the front end always runs in display mode 1 (640x480; [0x5BEF9C]) and
// draws in 640x480 pixels. The menu mode set (0x51A710 from 0x4F773B / 0x504999)
// asks THRASH_setvideomode for the race resolution instead; the game still
// believes it is 640x480, and every pre-transformed vertex, viewport and clear
// going to the Direct3D 7 device is mapped from 640x480 to a centered 4:3 area
// (or stretched). Bars are cleared before each page flip.
// Race: the options' mode list is decided here (see ModeOk), any shape let
// in. 3D: the projection (0x453F10, from 0x539E60) and the fixed-point
// projection / frustum (0x560350) use the view width for a fixed horizontal
// FOV - for views as wide as the screen the horizontal angle is widened so the
// vertical view stays as on a 4:3 screen (Hor+), also for zoomed cameras
// (0x53A04C). HUD: its scales come from the screen width (0x4CE753 and three
// 1/640 constants, plus sizes 9/14/10 and an element at x=224 in 640 units);
// those use the 4:3 width instead, and the element rectangles of the layout
// file (0x7CB0BC, built by 0x4D0550) are placed in 4:3 proportions at the left,
// right or middle of the screen. The rear-view mirror uses one of them.
// VEG's widescreen patch, already in its exe (jumps at the sites below), is undone first.
// ---------------------------------------------------------------------------
namespace ws {
    const DWORD fnSetMode       = 0x51A710;
    const DWORD callModeRace    = 0x4F73DB;
    const DWORD callModeFE[2]   = { 0x4F773B, 0x504999 };
    const DWORD fnSelectedMode  = 0x487840;
    const DWORD pModeW = 0x8539A4, pModeH = 0x8539A8;   // mode the game believes in
    const DWORD pScrW  = 0x853A98, pScrH  = 0x853A9C;   // full-screen view rect w/h
    const DWORD modeFilter      = 0x51A488;             // options list: a row's tests (mov eax, [0x7C3D54])
    const DWORD callFrustum     = 0x539EC0, fnFrustumUnits = 0x560350;
    const DWORD callProj        = 0x539F23, fnProj = 0x453F10;
    const DWORD zoomStore       = 0x53A04C;             // fstp [0x5B0950] (6 bytes)
    const DWORD pFocalX = 0x5B0950, pFocalY = 0x5B0954;
    const DWORD hudWidthLoad    = 0x4CE753;             // fild [0x853A98] (6 bytes)
    const DWORD hudFitW[2]      = { 0x4D2737, 0x4D28F6 };  // fild [0x853A98]: position / lap list width -> fraction
    const DWORD hudInv640[3]    = { 0x59967F, 0x5998CB, 0x599953 };
    const DWORD hudSizes[3]     = { 0x599CCB, 0x599CD7, 0x599CDB };
    const DWORD hudX224[2]      = { 0x4D0A30, 0x4D0B66 }; // float immediates (224.0)
    const DWORD callRects       = 0x4CE7D6, fnRects = 0x4D0550;
    const DWORD pRects          = 0x7CB0BC;
    const DWORD pCurMode        = 0x5BEF94;             // mode the game last set
    const DWORD modeLookupJe    = 0x51A677;             // je (4:3 only) in the width -> mode lookup
    const DWORD callLookupSaved = 0x4C2BB5;             // lookup of the saved resolution
    const DWORD fnLookup        = 0x51A530;

    struct Restore { DWORD addr; const char* orig; int len; };
    const Restore kOtherPatch[] = {        // original bytes at the sites of VEG's widescreen patch
        { 0x453F5D, "\xDD\xD8\xD8\x7D\x10", 5 },
        { 0x46D71C, "\xA3\x0C\x95\x68\x00", 5 },
        { 0x4CE753, "\xDB\x05\x98\x3A\x85\x00", 6 },
        { 0x4D05D1, "\xD8\x8C\x02\xC0\xB0\x7C\x00", 7 },
        { 0x4D0606, "\xD8\x8C\x02\xC8\xB0\x7C\x00", 7 },
        { 0x4D0A2D, "\xC7\x45\xF8\x00\x00\x60\x43", 7 },
        { 0x4D0B65, "\xB9\x00\x00\x60\x43", 5 },
        { 0x51A428, "\x8B\x45\xE0\x8B\x40\x40", 6 },
        { 0x53A086, "\xD9\x1D\x54\x09\x5B\x00", 6 },
        { 0x562B4C, "\x00\x00\x00\x00", 4 },
    };
}

static bool  g_wsOn = false;
static int   g_dispW = 0, g_dispH = 0;          // desktop at startup
// menu mapping 640x480 -> screen
static volatile bool g_xf = false;
static bool  g_xfLoad = false;                  // the mapping is the loading screen's (WsLoadBegin)
static float g_xSrcW = 640, g_xSrcH = 480;      // the size of what is mapped: the menus' 640x480, or the screen
static float g_xsx = 1, g_xsy = 1, g_xox = 0, g_xoy = 0;
static int   g_realW = 0, g_realH = 0, g_realBpp = 16;
static int   g_feCall = 0, g_feTarget = 0, g_curRealIdx = 0;
static bool  g_softDriver = false;
std::atomic<unsigned long> g_wsXfDraws{0}, g_wsBarClears{0}, g_wsMovieFrames{0};
// race
static float g_projR = 1.0f;                   // Hor+ factor of the view being set up
static float g_hudW  = 0.0f;                   // width the HUD lays out in
static float g_hudInv640Orig[3], g_hudSizesOrig[3];
static bool  g_hudPatched = false;

// thrash entry points we wrap
typedef int   (__stdcall *TSetMode_t)(int, int, int);
typedef void  (__stdcall *TVoid_t)();
typedef void* (__stdcall *TLock_t)();
typedef void  (__stdcall *TUnlock_t)(int);
typedef void* (__stdcall *TAbout_t)();
static TSetMode_t g_tSetMode = nullptr;
static TVoid_t    g_tFlip = nullptr;
static TLock_t    g_tLock = nullptr;
static TUnlock_t  g_tUnlock = nullptr;
static TAbout_t   g_tAbout = nullptr;
static HMODULE    g_thrashMod = nullptr;

// a row of the renderer's mode table (count +0x3C, rows +0x40, 40 bytes each:
// w, h, bpp, ?, flags, pages +0x14, depth pages +0x18), rows 1..count
static const int* ModeRow(const BYTE* about, int idx) {
    if (!about || idx < 1 || IsBadReadPtr(about, 0x44)) return nullptr;
    const BYTE* e = *(BYTE* const*)(about + 0x40);
    if (idx > *(const int*)(about + 0x3C) || !e || IsBadReadPtr(e + idx * 40, 40)) return nullptr;
    return (const int*)(e + idx * 40);
}
static bool ModeEntry(int idx, int* w, int* h, int* bpp) {
    const int* m = g_tAbout ? ModeRow((const BYTE*)g_tAbout(), idx) : nullptr;
    if (!m) return false;
    if (w) *w = m[0];
    if (h) *h = m[1];
    if (bpp) *bpp = m[2];
    return true;
}
// the mode of that size for the menus: one as deep as the game's own menu mode
// (mode 1, 640x480) if there is one, since the game goes on drawing and playing
// its movies for that depth; else the first of that size
static int FindMode(int w, int h) {
    if (!g_tAbout) return 0;
    BYTE* a = (BYTE*)g_tAbout();
    if (!a || IsBadReadPtr(a, 0x44)) return 0;
    int n = *(int*)(a + 0x3C), first = 0;
    const int* own = ModeRow(a, 1);
    for (int i = 1; i <= n; i++) {
        const int* m = ModeRow(a, i);
        if (!m || m[0] != w || m[1] != h) continue;
        if (!own || (m[2] > 16) == (own[2] > 16)) return i;
        if (!first) first = i;
    }
    return first;
}

// ---- the options' mode list --------------------------------------------------
// The list (0x51A390, called by 0x487810 every time the options or a mode
// change want it) walks the renderer's modes and lists one if its pages
// (+0x14) cover the buffers asked for ([0x7C3D54] + 2), w*3 == h*4, it is 15 or
// 16-bit, or 32-bit on anything but the software renderer, flags bit 0 is set
// and bit 1 clear, and w*h*2 fits in half the video memory the renderer says
// is free ([0x5BEFA4], asked again after each mode change) shared between the
// buffers. The menu's list (0x487720) holds 20 entries, and the setting itself
// is a place in that list ([0x7C3D20]).
// So what the list holds decides which mode a place stands for: with every
// shape let in, 16 and 32-bit versions of each size and a memory figure that
// moves, the same place came out as another mode the next time the list was
// made, and the size chosen went back to 640x480. ModeOk decides instead, the
// same way every time: the game's page and flag tests, at least 640x480 (the
// software renderer: any size), any shape (wsUnlockResolutions=0: 4:3 only),
// no memory test, each size once (32-bit with ws32bit where there is one, else
// 16-bit), and when there are more than 20, 640x480 plus the 19 largest (the
// software renderer: the 20 smallest).
static bool g_modeKeep[256];
static bool SoftClass() { return (Rd<BYTE>(0x8287C0) & 1) != 0; }   // the renderer class the game goes by
// 0 = not listed, else its rank (the higher one of a size is listed)
static int ModeRank(const int* m) {
    int w = m[0], h = m[1], bpp = m[2], flags = m[4];
    if (m[5] < Rd<int>(0x7C3D54) + 2 || !(flags & 1) || (flags & 2) || w <= 0 || h <= 0) return 0;
    bool soft = SoftClass();
    if (!soft && (w < 640 || h < 480)) return 0;
    if (!g_cfg.wsUnlockResolutions && w * 3 != h * 4) return 0;
    if (bpp == 15 || bpp == 16) return bpp == 16 ? 2 : 1;
    if (bpp == 32 && !soft) return g_cfg.ws32bit ? 3 : 0;
    return 0;
}
static int BuildModeList(const BYTE* about) {
    static bool was[256]; static bool logged = false;
    memset(g_modeKeep, 0, sizeof(g_modeKeep));
    if (!about || IsBadReadPtr(about, 0x44)) return 0;
    int n = *(const int*)(about + 0x3C);
    if (n > 255) n = 255;
    struct M { int i, w, h, rank; } best[256]; int nb = 0;
    for (int i = 1; i <= n; i++) {
        const int* m = ModeRow(about, i);
        int rank = m ? ModeRank(m) : 0;
        if (!rank) continue;
        int k = 0; while (k < nb && !(best[k].w == m[0] && best[k].h == m[1])) k++;
        if (k == nb) best[nb++] = { i, m[0], m[1], rank };
        else if (rank > best[k].rank) best[k] = { i, m[0], m[1], rank };
    }
    bool soft = SoftClass();
    while (nb > 20) {                                // the menu's list holds 20
        int s = -1;
        for (int k = 0; k < nb; k++) {
            if (!soft && best[k].w == 640 && best[k].h == 480) continue;
            int a = best[k].w * best[k].h;
            if (s < 0 || (soft ? a > best[s].w * best[s].h : a < best[s].w * best[s].h)) s = k;
        }
        if (s < 0) break;
        best[s] = best[--nb];
    }
    for (int k = 0; k < nb; k++) g_modeKeep[best[k].i] = true;
    if (g_cfg.wsLog && (!logged || memcmp(was, g_modeKeep, sizeof(was)))) {
        logged = true; memcpy(was, g_modeKeep, sizeof(was));
        char line[512] = {}; int at = 0;
        for (int i = 1; i <= n && at < (int)sizeof(line) - 32; i++) {
            const int* m = g_modeKeep[i] ? ModeRow(about, i) : nullptr;
            if (m) at += snprintf(line + at, sizeof(line) - at, " %d:%dx%dx%d", i, m[0], m[1], m[2]);
        }
        Logf("widescreen: %d of the renderer's %d display modes listed (mode:size):%s\n", nb, n, line);
    }
    return nb;
}
extern "C" int __cdecl ModeOk(int index, const BYTE* table) {
    if (index == 1) BuildModeList(table);           // first row: decide the whole list
    return index >= 0 && index < 256 && g_modeKeep[index];
}

// ---- thrash draw wrappers (menus): map 640x480 vertices to the screen -------
// Every thrash driver of this game generation takes 32-byte pre-transformed
// vertices (x, y at +0/+4) and DWORD indices.
typedef void (__stdcall *TP1_t)(void*);
typedef void (__stdcall *TP2_t)(void*, void*);
typedef void (__stdcall *TP3_t)(void*, void*, void*);
typedef void (__stdcall *TP4_t)(void*, void*, void*, void*);
typedef void (__stdcall *TStrip_t)(int, void*);
typedef void (__stdcall *TMesh_t)(int, void*, DWORD*);
typedef void (__stdcall *TClip_t)(int, int, int, int);
static TP1_t g_tPoint; static TP2_t g_tLine; static TP3_t g_tTri; static TP4_t g_tQuad;
static TStrip_t g_tFan, g_tStrip, g_tLStrip, g_tPStrip;
static TMesh_t g_tTriM, g_tQuadM, g_tLineM, g_tPointM;
static TClip_t g_tClip; static TVoid_t g_tClear;
static int g_clip[4] = { 0, 0, 640, 480 };   // as the game set it

static inline LONG XfX(float x) { return (LONG)lrintf(x * g_xsx + g_xox); }
static inline LONG XfY(float y) { return (LONG)lrintf(y * g_xsy + g_xoy); }
struct XV { float f[8]; };
// alternatePixelCenter: Direct3D (as d3da.dll uses it) takes a pixel's centre to
// be at whole coordinates, where the game's 2D pictures are laid out for pixels
// that start there. So every vertex on its way to the Direct3D renderer is
// moved half a pixel up and left; pictures drawn 1:1 then land texel on pixel.
// (From HPFrameFix, where it was confirmed in game.)
static bool g_apc = false;                        // on for the renderer loaded now
static inline bool WrOn() { return g_xf || g_apc; }
static inline void Xv(XV& d, const void* s) {
    memcpy(&d, s, 32);
    if (g_xf) { d.f[0] = d.f[0] * g_xsx + g_xox; d.f[1] = d.f[1] * g_xsy + g_xoy; }
    if (g_apc) { d.f[0] -= 0.5f; d.f[1] -= 0.5f; }
}
static XV* g_vbuf = nullptr; static int g_vbufN = 0;
static XV* VBuf(int n) {
    if (n > g_vbufN) { free(g_vbuf); g_vbufN = n + 1024; g_vbuf = (XV*)malloc(sizeof(XV) * g_vbufN); if (!g_vbuf) g_vbufN = 0; }
    return g_vbuf;
}
static XV* XArr(const void* v, int n) {
    XV* b = VBuf(n); if (!b) return nullptr;
    for (int i = 0; i < n; i++) Xv(b[i], (const BYTE*)v + i * 32);
    if (g_xf) g_wsXfDraws++;
    return b;
}
static void __stdcall WrPoint(void* a) { if (!WrOn()) return g_tPoint(a); XV x; Xv(x, a); g_tPoint(&x); }
static void __stdcall WrLine(void* a, void* b) { if (!WrOn()) return g_tLine(a, b); XV x, y; Xv(x, a); Xv(y, b); g_tLine(&x, &y); }
static void __stdcall WrTri(void* a, void* b, void* c) {
    if (!WrOn()) return g_tTri(a, b, c);
    XV x, y, z; Xv(x, a); Xv(y, b); Xv(z, c); if (g_xf) g_wsXfDraws++; g_tTri(&x, &y, &z);
}
static void __stdcall WrQuad(void* a, void* b, void* c, void* d) {
    if (!WrOn()) return g_tQuad(a, b, c, d);
    XV x, y, z, w; Xv(x, a); Xv(y, b); Xv(z, c); Xv(w, d); if (g_xf) g_wsXfDraws++; g_tQuad(&x, &y, &z, &w);
}
static void StripCall(TStrip_t f, int n, void* v, int extra) {
    if (!WrOn() || n <= 0 || !v) return f(n, v);
    XV* b = XArr(v, n + extra); if (!b) return f(n, v);
    f(n, b);
}
static void __stdcall WrFan(int n, void* v)    { StripCall(g_tFan, n, v, 2); }
static void __stdcall WrStrip(int n, void* v)  { StripCall(g_tStrip, n, v, 2); }
static void __stdcall WrLStrip(int n, void* v) { StripCall(g_tLStrip, n, v, 1); }
static void __stdcall WrPStrip(int n, void* v) { StripCall(g_tPStrip, n, v, 0); }
static void MeshCall(TMesh_t f, int n, void* v, DWORD* idx, int per) {
    if (!WrOn() || n <= 0 || !v || !idx) return f(n, v, idx);
    DWORD mx = 0;
    for (int i = 0; i < n * per; i++) if (idx[i] > mx) mx = idx[i];
    if (mx > 65535) return f(n, v, idx);
    XV* b = XArr(v, (int)mx + 1); if (!b) return f(n, v, idx);
    f(n, b, idx);
}
static void __stdcall WrTriM(int n, void* v, DWORD* i)   { MeshCall(g_tTriM, n, v, i, 3); }
static void __stdcall WrQuadM(int n, void* v, DWORD* i)  { MeshCall(g_tQuadM, n, v, i, 4); }
static void __stdcall WrLineM(int n, void* v, DWORD* i)  { MeshCall(g_tLineM, n, v, i, 2); }
static void __stdcall WrPointM(int n, void* v, DWORD* i) { MeshCall(g_tPointM, n, v, i, 1); }
static void ClipXf(int x0, int y0, int x1, int y1) {
    if (g_xf) g_tClip(XfX((float)x0), XfY((float)y0), XfX((float)x1), XfY((float)y1));
    else g_tClip(x0, y0, x1, y1);
}
static void __stdcall WrClip(int x0, int y0, int x1, int y1) {
    g_clip[0] = x0; g_clip[1] = y0; g_clip[2] = x1; g_clip[3] = y1;
    ClipXf(x0, y0, x1, y1);
}

// ---- thrash wrappers --------------------------------------------------------
// black bars: the clear only covers the clip rectangle, so clear each bar
static void ClearBars() {
    if (!g_tClip || !g_tClear || g_realW <= 0) return;
    int x0 = XfX(0), y0 = XfY(0), x1 = XfX(g_xSrcW), y1 = XfY(g_xSrcH);
    int r[4][4]; int n = 0;
    if (x0 > 0) { int t[4] = { 0, 0, x0, g_realH }; memcpy(r[n++], t, sizeof t); }
    if (x1 < g_realW) { int t[4] = { x1, 0, g_realW, g_realH }; memcpy(r[n++], t, sizeof t); }
    if (y0 > 0) { int t[4] = { x0, 0, x1, y0 }; memcpy(r[n++], t, sizeof t); }
    if (y1 < g_realH) { int t[4] = { x0, y1, x1, g_realH }; memcpy(r[n++], t, sizeof t); }
    if (!n) return;
    for (int i = 0; i < n; i++) { g_tClip(r[i][0], r[i][1], r[i][2], r[i][3]); g_tClear(); }
    ClipXf(g_clip[0], g_clip[1], g_clip[2], g_clip[3]);
    g_wsBarClears++;
}
static void __stdcall WrFlip() {
    if (g_xf && !g_xfLoad) ClearBars();
    g_tFlip();
}
// The loading screen on a screen wider than 4:3 comes out stretched sideways:
// its picture (0x4E75D0, called three times at 0x4E7A53, once for each of the
// renderer's pages: 20 x 15 tiles and the text over them, their places scaled
// from 640x480 to the whole screen) and its bar (0x4E7B80, once per step of the
// loading: one quad, drawn on two pages). While either draws, its draw calls
// are narrowed to the 4:3 middle of the screen (wsLoading); the pages are
// cleared whole before the picture, which leaves the sides black.
extern "C" void __cdecl WsLoadBegin() {
    int W = g_realW, H = g_realH;
    if (!g_wsOn || !g_cfg.wsLoading || g_softDriver || g_xf || W <= 0 || H <= 0 || W * 3 <= H * 4) return;
    g_xsx = H * 4.0f / (W * 3.0f); g_xox = (W - W * g_xsx) * 0.5f; g_xsy = 1.0f; g_xoy = 0.0f;
    g_xSrcW = (float)W; g_xSrcH = (float)H;
    g_xfLoad = true; g_xf = true;
    static bool logged = false;
    if (!logged) { logged = true; LogfAsync("widescreen: loading screen 4:3 in the middle (%dx%d, x scale %.3f)\n", W, H, g_xsx); }
}
extern "C" void __cdecl WsLoadEnd() {
    if (g_xfLoad) { g_xfLoad = false; g_xf = false; }
}
extern "C" { DWORD g_wsFnLoadPic = 0x4E75D0; }
extern "C" void WsLoadPicStub();
asm(".text\n"
    ".globl _WsLoadPicStub\n"
    "_WsLoadPicStub:\n"
    "    pushal\n"
    "    call _WsLoadBegin\n"
    "    popal\n"
    "    call *_g_wsFnLoadPic\n"
    "    pushal\n"
    "    call _WsLoadEnd\n"
    "    popal\n"
    "    ret\n");
static void* g_lockInfo = nullptr; static DWORD g_lockRet = 0;
static void* __stdcall WrLock() {
    g_lockRet = (DWORD)__builtin_return_address(0);
    void* p = g_tLock();
    g_lockInfo = p;
    return p;
}
// FMVs are copied in software into the top-left 640x480 of the locked frame;
// scale that block in place to the menu area (from the bottom-right, so no
// source pixel is overwritten before it is read) and blank the rest of it.
static const int kMovieW = 636;   // the last 4 columns of the movie picture are never written
static void ScaleLockedFrame() {
    BYTE* bits = *(BYTE**)g_lockInfo; int pitch = *(int*)((BYTE*)g_lockInfo + 4);
    if (!bits || pitch <= 0) return;
    int bp = g_realBpp >= 24 ? 4 : 2;
    if (pitch < g_realW * bp) return;
    // the movie player writes a packed 640x480 picture (rows 640 pixels apart),
    // not rows at the surface pitch
    static BYTE* src = nullptr;
    size_t srcN = (size_t)640 * 480 * bp;
    if (!src) src = (BYTE*)malloc((size_t)640 * 480 * 4);
    if (!src || (size_t)pitch * g_realH < srcN) return;
    memcpy(src, bits, srcN);
    for (int y = 0; y < g_realH; y++) memset(bits + (size_t)y * pitch, 0, (size_t)g_realW * bp);
    int X0 = XfX(0), Y0 = XfY(0), X1 = XfX(640), Y1 = XfY(480);
    if (X0 < 0) X0 = 0;
    if (Y0 < 0) Y0 = 0;
    if (X1 > g_realW) X1 = g_realW;
    if (Y1 > g_realH) Y1 = g_realH;
    for (int y = Y0; y < Y1; y++) {
        int sy = (int)((y - g_xoy) / g_xsy); if (sy < 0) sy = 0; if (sy > 479) sy = 479;
        BYTE* drow = bits + (size_t)y * pitch; const BYTE* srow = src + (size_t)sy * 640 * bp;
        if (bp == 4) {
            DWORD* dr = (DWORD*)drow; const DWORD* sr = (const DWORD*)srow;
            for (int x = X0; x < X1; x++) { int sx = (int)((x - g_xox) / g_xsx); dr[x] = sx >= kMovieW ? 0 : sr[sx < 0 ? 0 : sx]; }
        } else {
            WORD* dr = (WORD*)drow; const WORD* sr = (const WORD*)srow;
            for (int x = X0; x < X1; x++) { int sx = (int)((x - g_xox) / g_xsx); dr[x] = sx >= kMovieW ? 0 : sr[sx < 0 ? 0 : sx]; }
        }
    }
    g_wsMovieFrames++;
}
static unsigned g_lockLogged = 0;
static void __stdcall WrUnlock(int a) {
    if (g_xf && g_lockInfo && g_cfg.wsMovies && !IsBadReadPtr(g_lockInfo, 8)) {
        if (g_lockLogged < 8) { g_lockLogged++; Logf("widescreen: frame lock from 0x%06lX in the menus\n", (unsigned long)g_lockRet); }
        if (g_lockRet == 0x5807E9 || g_lockRet == 0x574011) ScaleLockedFrame();   // movie player
    }
    g_lockInfo = nullptr;
    g_tUnlock(a);
}
static int g_feWantW = 0, g_feWantH = 0;          // the size the menus are meant to have (0: the game's 640x480)
// THRASH_setvideomode(mode, pages, depth buffer): the game passes the pages its
// buffers need (capped at the mode's +0x14) and whether the mode's depth pages
// (+0x18) cover them (0x51A7AC)
static int __stdcall WrSetMode(int idx, int pages, int depth) {
    int use = idx;
    if (g_feCall && g_cfg.wsMenus && !g_softDriver && g_feTarget <= 0 && g_feWantW > 0) {
        g_feTarget = FindMode(g_feWantW, g_feWantH);   // mode list may only be ready now
        if (g_cfg.wsLog) {
            BYTE* a = g_tAbout ? (BYTE*)g_tAbout() : nullptr;
            Logf("widescreen: menu mode for %dx%d: %d (%d modes listed)\n", g_feWantW, g_feWantH, g_feTarget,
                 a && !IsBadReadPtr(a, 0x44) ? *(int*)(a + 0x3C) : -1);
        }
    }
    if (g_feCall && g_feTarget > 0) use = g_feTarget;
    const int* m = g_tAbout ? ModeRow((const BYTE*)g_tAbout(), use) : nullptr;
    if (use != idx && m) {                           // the same two figures for the mode set instead
        if (pages > m[5]) pages = m[5];
        depth = m[6] >= pages;
    }
    int r = g_tSetMode(use, pages, depth);
    g_curRealIdx = use;
    if (m) { g_realW = m[0]; g_realH = m[1]; g_realBpp = m[2]; }
    if (g_cfg.wsLog) Logf("widescreen: display mode %d -> %dx%d, %d bit (game asked for mode %d)\n", use, g_realW, g_realH, g_realBpp, idx);
    return r;
}
typedef FARPROC (WINAPI *GPA_t)(HMODULE, LPCSTR);
static GPA_t g_oGPA = nullptr;
static FARPROC WINAPI HkGetProcAddress(HMODULE m, LPCSTR name) {
    FARPROC p = g_oGPA(m, name);
    if (!p || !name || !HIWORD((DWORD)name) || strncmp(name, "_THRASH_", 8)) return p;
    if (m != g_thrashMod) {
        g_thrashMod = m;
        char fn[MAX_PATH] = {}; GetModuleFileNameA(m, fn, MAX_PATH);
        // the software renderer draws screen-aligned textured quads as unscaled
        // blits, so scaled menus come apart: keep its menus in their own mode
        const char* b = strrchr(fn, '\\'); b = b ? b + 1 : fn;
        g_softDriver = !_strnicmp(b, "soft", 4);
        g_apc = g_cfg.alternatePixelCenter && !_strnicmp(b, "d3d", 3);
        Logf("renderer driver: %s%s\n", fn,
             g_softDriver && g_cfg.widescreen && g_cfg.wsMenus ? " (software renderer: menus stay 640x480)" : "");
        if (g_cfg.alternatePixelCenter)
            Logf("pixel centre: %s\n", g_apc ? "every vertex moved by (-0.5, -0.5)" : "left as it is (Direct3D only)");
    }
    struct W { const char* n; void** real; void* wr; };
    static const W kW[] = {
        { "_THRASH_drawpoint@4", (void**)&g_tPoint, (void*)&WrPoint },
        { "_THRASH_drawline@8", (void**)&g_tLine, (void*)&WrLine },
        { "_THRASH_drawtri@12", (void**)&g_tTri, (void*)&WrTri },
        { "_THRASH_drawquad@16", (void**)&g_tQuad, (void*)&WrQuad },
        { "_THRASH_drawtrifan@8", (void**)&g_tFan, (void*)&WrFan },
        { "_THRASH_drawtristrip@8", (void**)&g_tStrip, (void*)&WrStrip },
        { "_THRASH_drawlinestrip@8", (void**)&g_tLStrip, (void*)&WrLStrip },
        { "_THRASH_drawpointstrip@8", (void**)&g_tPStrip, (void*)&WrPStrip },
        { "_THRASH_drawtrimesh@12", (void**)&g_tTriM, (void*)&WrTriM },
        { "_THRASH_drawquadmesh@12", (void**)&g_tQuadM, (void*)&WrQuadM },
        { "_THRASH_drawlinemesh@12", (void**)&g_tLineM, (void*)&WrLineM },
        { "_THRASH_drawpointmesh@12", (void**)&g_tPointM, (void*)&WrPointM },
        { "_THRASH_clip@16", (void**)&g_tClip, (void*)&WrClip },
    };
    for (const W& w : kW) if (!strcmp(name, w.n)) { *w.real = (void*)p; return (FARPROC)w.wr; }
    if (!strcmp(name, "_THRASH_clearwindow@0")) g_tClear = (TVoid_t)(void*)p;
    if (!g_cfg.widescreen) return p;              // (hooked for alternatePixelCenter alone: the draw calls only)
    if (!strcmp(name, "_THRASH_setvideomode@12")) { g_tSetMode = (TSetMode_t)(void*)p; return (FARPROC)(void*)&WrSetMode; }
    if (!strcmp(name, "_THRASH_pageflip@0"))      { g_tFlip = (TVoid_t)(void*)p;      return (FARPROC)(void*)&WrFlip; }
    if (!strcmp(name, "_THRASH_lockwindow@0"))    { g_tLock = (TLock_t)(void*)p;      return (FARPROC)(void*)&WrLock; }
    if (!strcmp(name, "_THRASH_unlockwindow@4"))  { g_tUnlock = (TUnlock_t)(void*)p;  return (FARPROC)(void*)&WrUnlock; }
    if (!strcmp(name, "_THRASH_about@0"))         g_tAbout = (TAbout_t)(void*)p;
    return p;
}

// ---- mode changes (called around 0x51A710) ---------------------------------
extern "C" { DWORD g_wsFnSetMode = 0x51A710, g_wsFnSelMode = 0x487840, g_wsSelResult = 0; }
extern "C" void WsCallSelMode();
extern "C" void __cdecl WsBeforeFeMode() {
    g_feCall = 1; g_feTarget = 0;
    if (!g_cfg.wsMenus || g_softDriver) return;
    // wsMenuResolution=1: the resolution chosen in the game's options, whatever
    // it is - 640x480 too, which is then simply the game's own menu mode. The
    // first time (startup) the options aren't loaded yet and 640x480 comes
    // back: then it's the size kept in the ini from last time (none yet: the
    // game's 640x480). The desktop's size is only used when it's asked for (2).
    static bool first = true;
    int t = 0, w = 0, h = 0;
    if (g_cfg.wsMenuResolution == 2) { w = g_dispW; h = g_dispH; }
    else {
        WsCallSelMode();
        if (!ModeEntry((int)g_wsSelResult, &w, &h, nullptr)) { w = 0; h = 0; }
        if (first && w <= 640 && h <= 480) { w = g_cfg.wsRaceWidth; h = g_cfg.wsRaceHeight; }
    }
    first = false;
    if (w > 640 || h > 480) { g_feWantW = w; g_feWantH = h; t = FindMode(w, h); }
    else g_feWantW = g_feWantH = 0;
    g_feTarget = t;
    // the game skips the switch when it thinks mode 1 is already set
    if (t > 0 && t != g_curRealIdx) *(int*)ws::pCurMode = -1;
    if (g_cfg.wsLog) Logf("widescreen: menu mode wanted: %d (race mode setting %lu)\n", t, (unsigned long)g_wsSelResult);
}
extern "C" void __cdecl WsAfterFeMode() {
    g_feCall = 0;
    int W = g_realW, H = g_realH;
    bool on = g_cfg.wsMenus && !g_softDriver && W > 0 && H > 0 && (W != 640 || H != 480);
    g_xfLoad = false; g_xSrcW = 640; g_xSrcH = 480;
    if (on) {
        if (g_cfg.wsMenus == 2) { g_xsx = W / 640.0f; g_xsy = H / 480.0f; g_xox = g_xoy = 0; }
        else {
            float s = (float)W / 640.0f < (float)H / 480.0f ? (float)W / 640.0f : (float)H / 480.0f;
            g_xsx = g_xsy = s; g_xox = (W - 640.0f * s) * 0.5f; g_xoy = (H - 480.0f * s) * 0.5f;
        }
    }
    g_xf = on;
    if (g_cfg.wsLog) Logf("widescreen: menus %s (640x480 -> %dx%d, scale %.3f x %.3f, offset %.0f,%.0f)\n",
                          on ? "mapped" : "original", W, H, g_xsx, g_xsy, g_xox, g_xoy);
}
extern "C" void __cdecl WsBeforeRaceMode() { g_feCall = 0; g_xf = false; g_xfLoad = false; }
static void ApplyHud(int W, int H) {
    float k = (W > 0 && H > 0) ? (float)W * 0.75f / (float)H : 1.0f;   // aspect relative to 4:3
    if (!g_cfg.wsHud || k < 1.001f) k = 1.0f;
    g_hudW = (float)W / k;
    if (!g_hudPatched) return;
    for (int i = 0; i < 3; i++) {
        float a = g_hudInv640Orig[i] / k, b = g_hudSizesOrig[i] / k;
        *(float*)ws::hudInv640[i] = a; *(float*)ws::hudSizes[i] = b;
    }
    float x224 = 224.0f + (640.0f * k - 640.0f) * 0.5f;
    for (DWORD at : ws::hudX224) PatchBytes(at, &x224, 4);
}
extern "C" void __cdecl WsAfterRaceMode() {
    int W = Rd<int>(ws::pModeW), H = Rd<int>(ws::pModeH);
    if (W > 0 && H > 0 && (W != g_cfg.wsRaceWidth || H != g_cfg.wsRaceHeight)) {
        g_cfg.wsRaceWidth = W; g_cfg.wsRaceHeight = H;
        ReplaceIniLine("wsRaceWidth", W); ReplaceIniLine("wsRaceHeight", H);
    }
    ApplyHud(W, H);
    if (g_cfg.wsLog) Logf("widescreen: race at %dx%d - HUD lays out %.0f wide, Hor+ %s\n",
                          W, H, g_hudW, g_cfg.wsFixFOV ? "on" : "off");
}

// HUD rectangles from the layout file: [i*5+j] = 4 fractions (y0,x0,y1,x1) then
// the pixel rect. Recompute x in 4:3 proportions, anchored by the rect's middle.
extern "C" void __cdecl WsPostRects() {
    if (!g_cfg.wsHud) return;
    int W = Rd<int>(ws::pScrW), H = Rd<int>(ws::pScrH);
    float w43 = H * 4.0f / 3.0f;
    if (W <= 0 || H <= 0 || w43 >= W - 0.5f) return;
    float extra = W - w43;
    int moved[3] = {};
    for (int i = 0; i < 16; i++) for (int j = 0; j < 5; j++) {
        float* f = (float*)(ws::pRects + i * 0xA0 + j * 0x20);
        float* p = f + 4;
        if (f[1] == 0 && f[3] == 0) continue;
        float c = (f[1] + f[3]) * 0.5f;
        int a = g_cfg.wsHud == 2 ? 1 : c < 0.4f ? 0 : c > 0.6f ? 2 : 1;
        float off = a == 0 ? 0.0f : a == 2 ? extra : extra * 0.5f;
        float x0 = (float)lrintf(w43 * f[1]) + off, x1 = (float)lrintf(w43 * f[3]) + off;
        if (x0 < 0) x0 = 0;
        if (x1 > W - 1) x1 = (float)(W - 1);
        if (g_cfg.wsLog >= 2) Logf("  HUD box %2d/%d: x %.3f-%.3f y %.3f-%.3f -> %s, px %.0f-%.0f\n", i, j,
                              f[1], f[3], f[0], f[2], a == 0 ? "left" : a == 2 ? "right" : "middle", x0, x1);
        p[1] = x0; p[3] = x1;
        moved[a]++;
    }
    if (g_cfg.wsLog) Logf("widescreen: HUD layout - %d boxes left, %d middle, %d right\n", moved[0], moved[1], moved[2]);
}

// Hor+ for views as wide as the screen (not the mirror).
extern "C" { int g_wsFH = 0, g_wsFV = 0, g_wsFW = 0, g_wsFM = 0, g_wsOutH = 0, g_wsOutV = 0;
             DWORD g_wsFnFrustumUnits = 0x560350, g_wsFnProj = 0x453F10; float g_wsProjRf = 1.0f; }
extern "C" void __cdecl WsFrustum() {
    float r = 1.0f;
    int W = Rd<int>(ws::pScrW), H = Rd<int>(ws::pScrH);
    if (g_wsOn && g_cfg.wsFixFOV && !g_wsFM && W > 0 && H > 0 && g_wsFW > W * 0.45f) {
        float w43 = H * 4.0f / 3.0f;
        if (w43 < W) r = w43 / W;
    }
    g_projR = r; g_wsProjRf = r;
    if (r > 0.9999f) g_wsOutH = (g_wsFH << 10) / 360;
    else {
        double a = g_wsFH * 3.14159265358979 / 180.0, na = atan(tan(a) / r);
        g_wsOutH = (int)lround(na * 1024.0 / (2.0 * 3.14159265358979));
    }
    g_wsOutV = (g_wsFV << 10) / 360;
}
typedef void (__stdcall *Proj_t)(float, float, float, float, float, float, int);
extern "C" void __stdcall WsProj(float x, float y, float w, float h, float a, float b, int m) {
    ((Proj_t)ws::fnProj)(x, y, w, h, a, b, m);
    if (g_projR < 0.9999f) {
        *(float*)ws::pFocalX *= g_projR;
        *(float*)ws::pFocalY *= g_projR;
    }
}

// The game saves the width and colour depth of the race resolution and looks
// the mode up from them when the menus start (0x4C2BB5 -> 0x51A530: the 4:3
// mode of that depth closest in width, so 1366 -> 1024x768), then takes that
// mode's place in the options' list (0x4878F0: a mode that isn't listed gives
// the list's first, 640x480). So the answer has to be a listed mode: the size
// remembered in the ini if the width is its width, else that width (4:3 first),
// else the closest width.
extern "C" { DWORD g_wsFnLookup = 0x51A530; }
extern "C" int __cdecl WsFindMode(int w, int bpp) {
    const BYTE* a = g_tAbout ? (const BYTE*)g_tAbout() : nullptr;
    if (w <= 0 || !BuildModeList(a)) return 0;       // (nothing listed: the game's own lookup)
    int best = 0, bestScore = 0;
    for (int i = 1; i < 256; i++) {
        const int* m = g_modeKeep[i] ? ModeRow(a, i) : nullptr;
        if (!m) continue;
        int dw = m[0] > w ? m[0] - w : w - m[0];
        bool saved = w == g_cfg.wsRaceWidth && m[1] == g_cfg.wsRaceHeight;
        int score = dw * 4 + (saved ? 0 : m[0] * 3 == m[1] * 4 ? 1 : 2);
        if (!best || score < bestScore) { best = i; bestScore = score; }
    }
    int mw = 0, mh = 0, mb = 0;
    ModeEntry(best, &mw, &mh, &mb);
    if (g_cfg.wsLog) Logf("widescreen: saved resolution (width %d, %d bit; ini %dx%d) -> mode %d, %dx%dx%d\n",
                          w, bpp, g_cfg.wsRaceWidth, g_cfg.wsRaceHeight, best, mw, mh, mb);
    return best;
}
extern "C" void ModeFilterStub();
extern "C" void WsFindModeStub();
extern "C" void WsFeModeStub();
extern "C" void WsRaceModeStub();
extern "C" void WsFrustumStub();
extern "C" void WsZoomStub();
extern "C" void WsRectsStub();
asm(".text\n"
    ".globl _WsCallSelMode\n"
    "_WsCallSelMode:\n"
    "    pushal\n"
    "    call *_g_wsFnSelMode\n"
    "    movl %eax, _g_wsSelResult\n"
    "    popal\n"
    "    ret\n"
    // call sites of SetVideoMode(eax = mode) for the menus / the race
    // call of the width -> mode lookup for the saved resolution (eax = w, edx = bpp)
    ".globl _WsFindModeStub\n"
    "_WsFindModeStub:\n"
    "    pushl %ecx\n"
    "    pushl %edx\n"
    "    pushl %eax\n"
    "    call _WsFindMode\n"
    "    testl %eax, %eax\n"
    "    jz 1f\n"
    "    addl $8, %esp\n"
    "    popl %ecx\n"
    "    ret\n"
    "1:  popl %eax\n"
    "    popl %edx\n"
    "    popl %ecx\n"
    "    jmp *_g_wsFnLookup\n"
    // the options' mode list, a row's tests (0x51A488: [ebp-0x1C] = row,
    // [ebp-0x20] = the renderer's table): listed -> the call of the list's
    // callback (0x51A4E0, which wants ecx = the buffers), else the next row
    ".globl _ModeFilterStub\n"
    "_ModeFilterStub:\n"
    "    pushal\n"
    "    pushl -0x20(%ebp)\n"
    "    pushl -0x1C(%ebp)\n"
    "    call _ModeOk\n"
    "    addl $8, %esp\n"
    "    testl %eax, %eax\n"
    "    popal\n"
    "    je 1f\n"
    "    movl 0x7C3D54, %ecx\n"
    "    addl $2, %ecx\n"
    "    pushl $0x51A4E0\n"
    "    ret\n"
    "1:  pushl $0x51A3E8\n"
    "    ret\n"
    ".globl _WsFeModeStub\n"
    "_WsFeModeStub:\n"
    "    pushal\n"
    "    call _WsBeforeFeMode\n"
    "    popal\n"
    "    call *_g_wsFnSetMode\n"
    "    pushal\n"
    "    call _WsAfterFeMode\n"
    "    popal\n"
    "    ret\n"
    ".globl _WsRaceModeStub\n"
    "_WsRaceModeStub:\n"
    "    pushal\n"
    "    call _WsBeforeRaceMode\n"
    "    popal\n"
    "    call *_g_wsFnSetMode\n"
    "    pushal\n"
    "    call _WsAfterRaceMode\n"
    "    popal\n"
    "    ret\n"
    // replaces "call 0x560490" in 0x539E60: eax = half FOV, edx = vertical, ebx = w,
    // ecx = h, [ebp+0xC] = mirror flag; tail-calls the unit version 0x560350
    ".globl _WsFrustumStub\n"
    "_WsFrustumStub:\n"
    "    movl %eax, _g_wsFH\n"
    "    movl %edx, _g_wsFV\n"
    "    movl %ebx, _g_wsFW\n"
    "    movl 0xC(%ebp), %eax\n"
    "    movl %eax, _g_wsFM\n"
    "    pushl %ecx\n"
    "    pushl %ebx\n"
    "    call _WsFrustum\n"
    "    popl %ebx\n"
    "    popl %ecx\n"
    "    movl _g_wsOutH, %eax\n"
    "    movl _g_wsOutV, %edx\n"
    "    jmp *_g_wsFnFrustumUnits\n"
    // replaces "fstp [0x5B0950]" of the zoomed-camera focal length (call; nop)
    ".globl _WsZoomStub\n"
    "_WsZoomStub:\n"
    "    fmuls _g_wsProjRf\n"
    "    fstps 0x5B0950\n"
    "    ret\n"
    // call of the HUD rect builder
    ".globl _WsRectsStub\n"
    "_WsRectsStub:\n"
    "    pushal\n"
    "    movl $0x4D0550, %eax\n"
    "    call *%eax\n"
    "    call _WsPostRects\n"
    "    popal\n"
    "    ret\n");

void InstallWidescreen() {
    g_dispW = GetSystemMetrics(SM_CXSCREEN); g_dispH = GetSystemMetrics(SM_CYSCREEN);
    if (g_knownExe && !g_cfg.widescreen && g_cfg.alternatePixelCenter) {   // (the draw wrappers alone)
        ForEachImport("KERNEL32.dll", [&](void** slot, const char* nm) {
            if (!strcmp(nm, "GetProcAddress")) PatchPointer(slot, (void*)&HkGetProcAddress, (void**)&g_oGPA);
        });
        if (!g_oGPA) Logf("pixel centre: GetProcAddress import not found - off\n");
    }
    if (!g_knownExe || !g_cfg.widescreen) { Logf("widescreen: off\n"); return; }
    // VEG's exe has its own widescreen patch: put its sites back first
    if (*(BYTE*)0x53A086 == 0xE9) {
        bool ok = true;
        for (const auto& r : ws::kOtherPatch) ok &= PatchBytes(r.addr, r.orig, r.len);
        Logf("widescreen: the exe's own widescreen patch %s (this mod's widescreen replaces it)\n",
             ok ? "undone" : "could NOT be undone");
    }
    bool ok = true;
    // GetProcAddress of the exe: wrap a few renderer entry points
    ForEachImport("KERNEL32.dll", [&](void** slot, const char* nm) {
        if (!strcmp(nm, "GetProcAddress")) ok &= PatchPointer(slot, (void*)&HkGetProcAddress, (void**)&g_oGPA);
    });
    if (!g_oGPA) { Logf("widescreen: GetProcAddress import not found - off\n"); return; }
    ok &= Rd<BYTE>(ws::modeFilter) == 0xA1 && PatchRel32(ws::modeFilter, 0xE9, (void*)&ModeFilterStub);   // options list
    ok &= PatchRel32(ws::callLookupSaved, 0xE8, (void*)&WsFindModeStub);
    if (g_cfg.wsUnlockResolutions) {
        BYTE j[2] = { 0x90, 0xE9 };                                  // the game's width lookup: any shape (je -> jmp)
        if (*(BYTE*)ws::modeLookupJe == 0x0F) ok &= PatchBytes(ws::modeLookupJe, j, 2);
    }
    static const DWORD kLoadPic[3] = { 0x4E7A53, 0x4E7A6F, 0x4E7A8B };   // the loading picture's three calls
    if (g_cfg.wsLoading)
        for (DWORD at : kLoadPic)
            ok &= Rd<BYTE>(at) == 0xE8 && at + 5 + Rd<DWORD>(at + 1) == g_wsFnLoadPic &&
                  PatchRel32(at, 0xE8, (void*)&WsLoadPicStub);
    for (DWORD at : ws::callModeFE) ok &= PatchRel32(at, 0xE8, (void*)&WsFeModeStub);
    ok &= PatchRel32(ws::callModeRace, 0xE8, (void*)&WsRaceModeStub);
    if (g_cfg.wsFixFOV) {
        ok &= PatchRel32(ws::callFrustum, 0xE8, (void*)&WsFrustumStub);
        ok &= PatchRel32(ws::callProj, 0xE8, (void*)&WsProj);
        BYTE z[6] = { 0xE8, 0, 0, 0, 0, 0x90 };
        DWORD rel = (DWORD)&WsZoomStub - (ws::zoomStore + 5); memcpy(z + 1, &rel, 4);
        ok &= PatchBytes(ws::zoomStore, z, 6);
    }
    if (g_cfg.wsHud) {
        for (int i = 0; i < 3; i++) { g_hudInv640Orig[i] = *(float*)ws::hudInv640[i]; g_hudSizesOrig[i] = *(float*)ws::hudSizes[i]; }
        BYTE f[6] = { 0xD9, 0x05, 0, 0, 0, 0 };          // fld dword [g_hudW]
        DWORD a = (DWORD)&g_hudW; memcpy(f + 2, &a, 4);
        ok &= PatchBytes(ws::hudWidthLoad, f, 6);
        for (DWORD at : ws::hudFitW) if (*(WORD*)at == 0x05DB) ok &= PatchBytes(at, f, 6);
        ok &= PatchRel32(ws::callRects, 0xE8, (void*)&WsRectsStub);
        g_hudPatched = true;
        g_hudW = 640.0f;
    }
    g_wsOn = ok;
    Logf("widescreen: %s (menus %d, menu resolution %d, Hor+ %d, HUD %d, movies %d, all modes %d, 32-bit %d; desktop %dx%d)\n",
         ok ? "installed" : "patch FAILED", g_cfg.wsMenus, g_cfg.wsMenuResolution, g_cfg.wsFixFOV,
         g_cfg.wsHud, g_cfg.wsMovies, g_cfg.wsUnlockResolutions, g_cfg.ws32bit, g_dispW, g_dispH);
}
