#include "common.h"
#include "portable.h"
#include "config.h"
#include "log.h"
#include "helpers.h"

// ---------------------------------------------------------------------------
// Portable install: the registry values the game finds itself by (see the
// header) are answered here. RegOpenKeyExA / RegQueryValueExA / RegCloseKey as
// the game imports them are wrapped; the three keys are recognised when
// they're opened (a key that isn't in the registry gets a handle of our own),
// and their values are answered like this:
//  - App Paths\NfsHs.exe "Path": the folder the exe is in.
//  - 1.0 "instLev": 2 if every folder a full install has is in the game
//    folder, 1 if all but Data\Showcase are; otherwise the registry's (2 if
//    it has none).
//  - 1.0 "SrcDrive": the game folder if Data\Audio\Music and Data\Movies are
//    in it, or if the registry has none; otherwise the registry's (the CD).
//  - 1.0 "Language": the registry's; if it has none, language= from the ini.
//  - 1.0 "Variant": the registry's; if it has none, 0.
//  - "Thrash Driver": renderer= from the ini if set (as its DLL's name), else
//    the registry's, else d3da.dll. "D3D Device": the registry's, else 0. "3D Device Description":
//    the registry's, else a neutral name.
// Every other key and value goes to the registry untouched.
// ---------------------------------------------------------------------------
typedef LONG (WINAPI *RegOpen_t)(HKEY, LPCSTR, DWORD, REGSAM, PHKEY);
typedef LONG (WINAPI *RegQuery_t)(HKEY, LPCSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD);
typedef LONG (WINAPI *RegClose_t)(HKEY);
static RegOpen_t g_oRegOpen = nullptr; static RegQuery_t g_oRegQuery = nullptr; static RegClose_t g_oRegClose = nullptr;
enum { kRkAppPath = 1, kRkGame10, kRkGame };
static struct { HKEY h; int kind; bool fake; } g_rk[8];   // the keys of ours the game has open (game threads only)
static bool GameDirHas(const char* sub) {
    char p[MAX_PATH];
    if (snprintf(p, sizeof(p), "%s%s", g_gameDir, sub) >= (int)sizeof(p)) return false;
    DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}
static void PortableLogOnce(int id, const char* fmt, const char* a) {
    static unsigned done = 0;
    if (done & (1u << id)) return;
    done |= 1u << id;
    Logf(fmt, a);
}
static LONG RegAnswer(DWORD type, const void* v, DWORD len, LPDWORD outType, LPBYTE data, LPDWORD cb) {
    if (outType) *outType = type;
    if (!cb) return data ? ERROR_INVALID_PARAMETER : ERROR_SUCCESS;
    DWORD room = *cb; *cb = len;
    if (!data) return ERROR_SUCCESS;
    if (room < len) return ERROR_MORE_DATA;
    memcpy(data, v, len);
    return ERROR_SUCCESS;
}
static LONG RegAnswerSz(const char* s, LPDWORD t, LPBYTE d, LPDWORD cb) { return RegAnswer(REG_SZ, s, (DWORD)strlen(s) + 1, t, d, cb); }
static LONG RegAnswerDw(DWORD v, LPDWORD t, LPBYTE d, LPDWORD cb) { return RegAnswer(REG_DWORD, &v, 4, t, d, cb); }
static LONG WINAPI HkRegOpenKeyExA(HKEY root, LPCSTR sub, DWORD opt, REGSAM sam, PHKEY out) {
    LONG r = g_oRegOpen(root, sub, opt, sam, out);
    int kind = 0;
    if (root == HKEY_LOCAL_MACHINE && sub && out) {
        if (!_stricmp(sub, "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\NfsHs.exe")) kind = kRkAppPath;
        else if (!_stricmp(sub, "SOFTWARE\\Electronic Arts\\Need For Speed High Stakes\\1.0")) kind = kRkGame10;
        else if (!_stricmp(sub, "SOFTWARE\\Electronic Arts\\Need For Speed High Stakes")) kind = kRkGame;
    }
    if (!kind) return r;
    for (int i = 0; i < 8; i++) {
        if (g_rk[i].h) continue;
        bool fake = r != ERROR_SUCCESS;
        if (fake) *out = (HKEY)(ULONG_PTR)(0x7EED0100 + i * 4);    // (not in the registry: a handle of our own)
        g_rk[i].h = *out; g_rk[i].kind = kind; g_rk[i].fake = fake;
        return ERROR_SUCCESS;
    }
    return r;                                     // (no slot free: as the registry has it)
}
static LONG WINAPI HkRegCloseKey(HKEY h) {
    for (int i = 0; i < 8; i++)
        if (g_rk[i].h == h && h) {
            bool fake = g_rk[i].fake;
            g_rk[i].h = nullptr;
            return fake ? ERROR_SUCCESS : g_oRegClose(h);
        }
    return g_oRegClose(h);
}
static LONG WINAPI HkRegQueryValueExA(HKEY h, LPCSTR name, LPDWORD res, LPDWORD type, LPBYTE data, LPDWORD cb) {
    int kind = 0; bool fake = false;
    for (int i = 0; i < 8; i++) if (g_rk[i].h == h && h) { kind = g_rk[i].kind; fake = g_rk[i].fake; }
    if (!kind || !name) return g_oRegQuery(h, name, res, type, data, cb);
    // what the registry itself says (kept in the caller's buffer when it's used as it is)
    DWORD cbIn = cb ? *cb : 0;
    LONG real = fake ? ERROR_FILE_NOT_FOUND : g_oRegQuery(h, name, res, type, data, cb);
    bool have = real == ERROR_SUCCESS;
    if (!have && cb) *cb = cbIn;
    if (kind == kRkAppPath && !_stricmp(name, "Path")) {
        char dir[MAX_PATH]; lstrcpynA(dir, g_gameDir, sizeof(dir));
        size_t n = strlen(dir);
        if (n > 3 && dir[n - 1] == '\\') dir[n - 1] = 0;          // (as the installer writes it: no closing backslash)
        PortableLogOnce(0, "portable install: install folder = %s\n", dir);
        if (cb) *cb = cbIn;
        return RegAnswerSz(dir, type, data, cb);
    }
    if (kind == kRkGame10 && !_stricmp(name, "instLev")) {
        static const char* const kLev1[] = { "Data\\Tracks", "Data\\Cars", "Data\\Audio\\SFX", "Data\\Audio\\Speech",
                                             "Data\\FeArt", "Data\\FeArt\\Track", "Data\\FeArt\\VidWall" };
        bool all1 = true;
        for (const char* f : kLev1) all1 = all1 && GameDirHas(f);
        int lev = all1 ? (GameDirHas("Data\\Showcase") ? 2 : 1) : -1;
        if (lev < 0 && have) { PortableLogOnce(1, "portable install: not every data folder is in the game folder - install level %s\n", "as the registry has it"); return real; }
        if (lev < 0) lev = 2;
        PortableLogOnce(1, "portable install: install level %s\n", lev == 2 ? "2 (everything in the game folder)" : "1 (no Data\\Showcase in the game folder)");
        if (cb) *cb = cbIn;
        return RegAnswerDw((DWORD)lev, type, data, cb);
    }
    if (kind == kRkGame10 && !_stricmp(name, "SrcDrive")) {
        bool local = GameDirHas("Data\\Audio\\Music") && GameDirHas("Data\\Movies");
        if (!local && have) { PortableLogOnce(2, "portable install: music and movies %s\n", "from the CD drive the registry names (not both in the game folder)"); return real; }
        PortableLogOnce(2, "portable install: music and movies %s\n", local ? "from the game folder" : "expected in the game folder (not there, and the registry names no CD drive)");
        if (cb) *cb = cbIn;
        return RegAnswerSz(g_gameDir, type, data, cb);
    }
    if (kind == kRkGame10 && !_stricmp(name, "Language") && !have) {
        static const struct { const char* n; DWORD code; } kLang[] = {
            { "english", 1 }, { "french", 2 }, { "german", 3 }, { "italian", 4 }, { "spanish", 5 }, { "swedish", 6 } };
        DWORD code = 1;
        for (const auto& l : kLang) if (!_stricmp(g_cfg.language, l.n)) code = l.code;
        PortableLogOnce(3, "portable install: no language in the registry - %s\n", g_cfg.language);
        return RegAnswerDw(code, type, data, cb);
    }
    // "Variant" (0x5047A0; without it the game aborts: AMF=20 platform.c(604)): the
    // edition, 0 to 7. 0 is the American one (US English; Spanish prices in pesos),
    // 1 with English gives the British texts, 4, 5 and 6 set special flags.
    if (kind == kRkGame10 && !_stricmp(name, "Variant") && !have) {
        PortableLogOnce(6, "portable install: no edition (Variant) in the registry - %s\n", "0, the American one");
        return RegAnswerDw(0, type, data, cb);
    }
    if (kind == kRkGame && !_stricmp(name, "Thrash Driver")) {
        static const struct { const char* n; const char* v; } kRend[] = {
            { "d3d", "d3da.dll" }, { "software", "softtria.dll" }, { "voodoo", "voodoo2a.dll" }, { "voodoo2", "voodoo2a.dll" } };
        const char* v = nullptr;
        for (const auto& m : kRend) if (!_stricmp(g_cfg.renderer, m.n)) v = m.v;
        if (g_cfg.renderer[0] && !v) PortableLogOnce(4, "renderer: \"%s\" isn't d3d, software, voodoo or voodoo2 - ignored\n", g_cfg.renderer);
        if (!v && have) return real;
        if (!v) v = "d3da.dll";
        PortableLogOnce(5, "renderer: %s\n", v);
        if (cb) *cb = cbIn;
        return RegAnswerSz(v, type, data, cb);
    }
    if (kind == kRkGame && !_stricmp(name, "D3D Device") && !have) return RegAnswerDw(0, type, data, cb);
    // (0x504D50, read just before the renderer; missing = "files are corrupted". A card's
    // name, of which the game only knows a few old ones to work around)
    if (kind == kRkGame && !_stricmp(name, "3D Device Description") && !have) return RegAnswerSz("Primary Display Driver", type, data, cb);
    if (!have) {                                  // (a value of the game's keys nobody answers: worth knowing about)
        static int n = 0;
        if (n++ < 16) Logf("portable install: the game asked for \"%s\" (key %d), which isn't in the registry and isn't answered here\n", name, kind);
    }
    return real;
}
void InstallPortable() {
    if (!g_knownExe) return;
    if (!g_cfg.portableInstall) { Logf("portable install: off (the registry as it is)\n"); return; }
    if (!InitGameDir()) { Logf("portable install: the game's folder isn't known - off\n"); return; }
    bool o = false, q = false, c = false;
    ForEachImport("ADVAPI32.dll", [&](void** slot, const char* nm) {
        if (!strcmp(nm, "RegOpenKeyExA")) o = PatchPointer(slot, (void*)&HkRegOpenKeyExA, (void**)&g_oRegOpen);
        else if (!strcmp(nm, "RegQueryValueExA")) q = PatchPointer(slot, (void*)&HkRegQueryValueExA, (void**)&g_oRegQuery);
        else if (!strcmp(nm, "RegCloseKey")) c = PatchPointer(slot, (void*)&HkRegCloseKey, (void**)&g_oRegClose);
    });
    Logf("portable install: %s\n", o && q && c ? "on" : "imports not found - off");
}
