#include "common.h"
#include "config.h"

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------

Config g_cfg;
char g_asiDir[MAX_PATH] = {};

static void InitAsiDir() {
    HMODULE self = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(&InitAsiDir), &self) &&
        GetModuleFileNameA(self, g_asiDir, MAX_PATH)) {
        char* s = strrchr(g_asiDir, '\\');
        if (s) { s[1] = 0; return; }
    }
    g_asiDir[0] = 0;
}

// The whole file, as a new one is written. It is also where the notes come from
// when a setting is missing from an existing file (AppendMissingKeys).
static const char kIniText[] =
    "; HSFrameFix.ini (v" HSFF_VERSION ")\n"
    ";\n"
    "; Lines starting with ; are notes. For on/off settings 1 = on, 0 = off.\n"
    "; Delete this file to get a fresh one with the defaults.\n"
    "\n"
    "\n"
    "[Frame rate]\n"
    "\n"
    "; Draw frames at the display's refresh rate, blending car and camera motion between\n"
    "; the game's fixed 64 steps a second. 0 = the game's 64 fps cap\n"
    "interpolate=1\n"
    "\n"
    "; Smooth the police helicopter's movement too\n"
    "interpolateCopter=1\n"
    "\n"
    "; Run the game's 128 Hz timer from a precise clock. Needed for smooth blending\n"
    "preciseTicks=1\n"
    "\n"
    "; Frame limiter for races. 0 = off (use vsync). Without vsync, set it to your\n"
    "; display's refresh rate\n"
    "targetFPS=0\n"
    "\n"
    "; How many 1/64 s steps behind the newest step frames are drawn. 1.0 is smooth with\n"
    "; the least delay; try 1.5 if you still see hitches\n"
    "interpDelay=1.0\n"
    "\n"
    "; Movement bigger than this in one step isn't blended (resets, camera cuts):\n"
    "; a distance in metres, and an angle in degrees\n"
    "maxStepDistance=10\n"
    "maxStepAngle=90\n"
    "\n"
    "\n"
    "[Menus]\n"
    "\n"
    "; Drop the game's 64 fps cap in the menus\n"
    "unlockMenus=1\n"
    "\n"
    "; Frame limiter for the menus: -1 = same as targetFPS, 0 = off (use vsync), N = N fps\n"
    "menuTargetFPS=-1\n"
    "\n"
    "; Highest frame rate in the menus. Their animations count time in 1/128 s, and some\n"
    "; (list slide, video walls) speed up above 128 fps.\n"
    ";   -1 = automatic: the highest rate up to 128 that divides the refresh rate\n"
    ";        (120 Hz -> 120, 144 Hz -> 72, 240 Hz -> 120)\n"
    ";    0 = no ceiling\n"
    ";    N = at most N fps\n"
    "menuMaxFPS=-1\n"
    "\n"
    "; The extra effects the game only draws on its Glide renderer (glow on menu items,\n"
    "; full-brightness highlights) on Direct3D too\n"
    "glideMenuEffects=1\n"
    "\n"
    "\n"
    "[Loading]\n"
    "\n"
    "; Faster loading: the loading bar is redrawn at most every loadScreenRenderMs\n"
    "; milliseconds instead of at every step (0 = at every step)\n"
    "speedUpLoading=1\n"
    "loadScreenRenderMs=100\n"
    "\n"
    "\n"
    "[Camera]\n"
    "\n"
    "; Degrees added to the game's field of view (negative = narrower). fovOffset applies\n"
    "; to every race camera, the others to one camera each\n"
    "fovOffset=0\n"
    "fovOffsetInCar=0\n"
    "fovOffsetBumper=0\n"
    "fovOffsetHeli=0\n"
    "fovOffsetRoof=0\n"
    "\n"
    "; In a race: F8 = wider, F7 = narrower, F6 = reset, for the camera you're in. The\n"
    "; result is saved in the lines above\n"
    "fovHotkeys=1\n"
    "\n"
    "; The keys, as Windows virtual-key codes (F1..F12 = 0x70..0x7B; avoid F10), and how\n"
    "; many degrees one press changes\n"
    "fovKeyUp=0x77\n"
    "fovKeyDown=0x76\n"
    "fovKeyReset=0x75\n"
    "fovStep=1\n"
    "\n"
    "\n"
    "[Graphics]\n"
    "\n"
    "; Renderer:\n"
    ";   d3d      = Direct3D (d3da.dll)\n"
    ";   software = software renderer (softtria.dll)\n"
    ";   voodoo or voodoo2 = 3dfx Glide (voodoo2a.dll)\n"
    ";   Empty = the Windows registry's setting (Direct3D if it has none)\n"
    "renderer=\n"
    "\n"
    "; Folder the renderer DLLs are loaded from: a folder in the game folder, or a full\n"
    "; path. A DLL that isn't there is loaded from the game folder. Empty = the game folder\n"
    "driversFolder=\n"
    "\n"
    "; Direct3D: every vertex is moved half a pixel up and left, so 2D pictures (HUD,\n"
    "; menus, text) land exactly on the pixels and come out sharp. 0 = as the renderer has it\n"
    "alternatePixelCenter=1\n"
    "\n"
    "; Night races on the tracks from Need for Speed III (Hometown, Redrock Ridge,\n"
    "; Atlantica, Rocky Pass, Country Woods, Lost Canyons, Aquatica, Summit, Empire City):\n"
    "; the lamps along the track light the road and what's around them, where the game\n"
    "; leaves these tracks pitch black around their glowing lamps (the light is added to\n"
    "; the track's colours as it loads; the headlights work as before).\n"
    "; nightLamps: how bright (0 = off: the game's night). nightLampReach: how far a\n"
    "; street lamp's light reaches, in metres\n"
    "nightLamps=1\n"
    "nightLampReach=22\n"
    "\n"
    "; With nightLamps: the buildings' windows, lit signs, tunnel lights and the like give\n"
    "; light too, the cities get street lighting and a lit sky's glow, and moving things\n"
    "; such as Atlantica's boat are lit. How bright (0 = off)\n"
    "nightScenery=1\n"
    "\n"
    "\n"
    "[Widescreen]\n"
    "\n"
    "; Master switch for the widescreen features (the ws... settings): pick any\n"
    "; resolution in the game's options\n"
    "widescreen=1\n"
    "\n"
    "; Race view: keep the 4:3 view's height and show more at the sides. 0 = the game\n"
    "; stretches the 4:3 picture to the screen's width\n"
    "wsFixFOV=1\n"
    "\n"
    "; List the resolutions that aren't 4:3 in the options (the game hides them). Its list\n"
    "; holds 20: each resolution is listed once, and the largest are kept\n"
    "wsUnlockResolutions=1\n"
    "\n"
    "; List each resolution in 32-bit colour where the renderer offers it. 0 = 16-bit\n"
    "ws32bit=1\n"
    "\n"
    "; Race HUD:\n"
    ";   1 = 4:3 proportions, with the side elements at the screen's edges\n"
    ";   2 = 4:3 proportions in the middle\n"
    ";   0 = stretched sideways\n"
    "wsHud=1\n"
    "\n"
    "; Menus (they are 640x480 pictures):\n"
    ";   1 = in 4:3 proportions in the middle of the screen\n"
    ";   2 = stretched to the screen\n"
    ";   0 = the game's own: the display switches to 640x480 for the menus\n"
    "wsMenus=1\n"
    "\n"
    "; Which resolution the menus use with wsMenus on: 1 = the one chosen in the game's\n"
    "; options (at startup, the one used last time), 2 = the desktop's\n"
    "wsMenuResolution=1\n"
    "\n"
    "; Movies (intro, FMVs) are scaled like the menus\n"
    "wsMovies=1\n"
    "\n"
    "; Loading screen: 1 = in 4:3 proportions in the middle of the screen, 0 = stretched\n"
    "; to the screen's width\n"
    "wsLoading=1\n"
    "\n"
    "; The race resolution last used, kept here by HSFrameFix (the game itself saves only\n"
    "; the width). 0 = none yet\n"
    "wsRaceWidth=0\n"
    "wsRaceHeight=0\n"
    "\n"
    "\n"
    "[Sound]\n"
    "\n"
    "; The game's own threads (main, sound, streaming) all run on one CPU core; its sound\n"
    "; code was written for one CPU and crackles when they run side by side. The system's\n"
    "; threads keep the other cores. 0 = leave them alone\n"
    "gameThreadsOneCore=1\n"
    "\n"
    "; If the whole game was started on one core (taskset, a launcher's single-core\n"
    "; option), the system's sound threads share that core and the sound pops whenever the\n"
    "; game is busy. 1 = the game gets all cores back and only its own threads stay on one.\n"
    "; The same is done if something pins the game later, such as a DirectDraw\n"
    "; wrapper's single-core option. Needs gameThreadsOneCore=1\n"
    "widenAffinity=1\n"
    "\n"
    "\n"
    "[Fixes]\n"
    "\n"
    "; Closing the window (Alt+F4, the title bar, the window manager) ends the game.\n"
    "; 0 = the game keeps running in the background\n"
    "closeExits=1\n"
    "\n"
    "; No test for free disk space at startup. On today's big disks the game's test goes\n"
    "; wrong and says \"Your hard drive has less than 1MB free space\". 0 = the game's test\n"
    "noDiskSpaceCheck=1\n"
    "\n"
    "\n"
    "[Portable]\n"
    "\n"
    "; The game needs no registry entries and can be copied or moved freely: where it's\n"
    "; installed is answered from the folder its exe is in (without the entries the game\n"
    "; stops with \"files are corrupted; please re-install\"). Music and movies are read\n"
    "; from the game folder if Data\\Audio\\Music and Data\\Movies are there, else from the\n"
    "; CD drive the registry names. 0 = the Windows registry as it is\n"
    "portableInstall=1\n"
    "\n"
    "; Language, used when the registry has none: english, german, french, spanish,\n"
    "; italian, swedish\n"
    "language=english\n"
    "\n"
    "; Don't ask for the game CD at startup (the official 4.50 exe does, twice)\n"
    "noCDCheck=1\n"
    "\n"
    "\n"
    "[Advanced]\n"
    "\n"
    "; 1 = if the game's exe isn't one this mod was made for, the mod turns itself off.\n"
    "; 0 = patch anyway (only for a modified exe you know is the same version; patching\n"
    "; the wrong code can crash the game). Every difference is written to the log\n"
    "exeCheck=1\n"
    "\n"
    "; Write HSFrameFix.log next to this file (what was patched, and frame statistics\n"
    "; every logEverySeconds seconds). Send it along when reporting a problem\n"
    "logToFile=1\n"
    "logEverySeconds=10\n"
    "\n"
    "; Widescreen details in the log too\n"
    "wsLog=1\n"
    "\n"
    "; For looking into the night lamps: 1 = the key below (0x79 = F10) writes the track as\n"
    "; the game holds it at that moment (camera, every vertex's colour, every polygon near\n"
    "; the camera) to HSFrameFix_scene.txt next to the .asi, a few megabytes each time\n"
    "nightLampsLog=0\n"
    "nightLampsLogKey=0x79\n"
    "\n"
    "; For looking into hitches: 1 = whenever a frame finds the simulation late, the second\n"
    "; before it (every timer tick, step and frame with its time) is written to\n"
    "; HSFrameFix_hitch.txt next to the .asi\n"
    "hitchLog=0\n";
char g_iniPath[MAX_PATH] = {};

// The settings an existing file has (LoadConfig), and the ones it lacks: a file
// from an older version keeps its layout, and each setting it doesn't have yet
// is added at its end with the notes that stand above it in kIniText.
static char g_iniSeen[96][32]; static int g_iniSeenN = 0;
static bool IniHas(const char* key) {
    for (int i = 0; i < g_iniSeenN; i++) if (!_stricmp(g_iniSeen[i], key)) return true;
    return false;
}
static void IniSeen(const char* key) {
    if (g_iniSeenN < 96 && !IniHas(key)) lstrcpynA(g_iniSeen[g_iniSeenN++], key, 32);
}
static void AppendMissingKeys(const char* path) {
    FILE* a = nullptr;
    const char* notes = nullptr, *notesEnd = nullptr;   // the notes above the settings of this paragraph
    bool together = false;                        // the setting just above, under the same notes, was added
    for (const char* p = kIniText; *p; ) {
        const char* e = strchr(p, '\n');
        const char* next = e ? e + 1 : p + strlen(p);
        size_t len = (size_t)((e ? e : next) - p);
        if (*p == ';') { if (!notes) notes = p; }
        else if (!len || *p == '[') { notes = notesEnd = nullptr; together = false; }
        else {
            if (notes && !notesEnd) notesEnd = p;
            char key[64] = {};
            const char* eq = (const char*)memchr(p, '=', len);
            if (eq && (size_t)(eq - p) < sizeof(key)) memcpy(key, p, (size_t)(eq - p));
            if (key[0] && !IniHas(key)) {
                if (!a) {
                    a = fopen(path, "a");
                    if (!a) return;
                    fputs("\n\n; --- Settings this file didn't have yet, added by HSFrameFix v" HSFF_VERSION " ---\n", a);
                }
                if (!together) {
                    fputs("\n", a);
                    if (notes) fwrite(notes, 1, (size_t)(notesEnd - notes), a);
                }
                fwrite(p, 1, len, a); fputs("\n", a);
                together = true;
            } else together = false;
        }
        p = next;
    }
    if (a) fclose(a);
}

void LoadConfig() {
    InitAsiDir();
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%sHSFrameFix.ini", g_asiDir);
    strcpy(g_iniPath, path);
    FILE* f = fopen(path, "r");
    if (!f) {
        f = fopen(path, "w");
        if (f) {
            fputs(kIniText, f);
            fclose(f);
        }
        return;
    }
    char line[256];
    g_iniSeenN = 0;
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == ';' || line[0] == '#' || line[0] == '\r' || line[0] == '\n') continue;
        char key[64] = {}, val[64] = {};
        {   // driversFolder: a path - everything after the "=", spaces and all; may be empty
            char dv[MAX_PATH] = {};
            int got = sscanf(line, " %63[^= \t] = %259[^;\r\n]", key, dv);
            if (got >= 1 && strchr(line, '=')) IniSeen(key);      // (a setting with nothing after the "=" counts too)
            if (got >= 1 && !_stricmp(key, "driversFolder")) {
                for (size_t n = strlen(dv); n && (dv[n - 1] == ' ' || dv[n - 1] == '\t'); ) dv[--n] = 0;
                lstrcpynA(g_cfg.driversFolder, dv, sizeof(g_cfg.driversFolder));
                continue;
            }
        }
        if (sscanf(line, " %63[^= \t] = %63s", key, val) < 2) continue;
        if      (!_stricmp(key, "interpolate"))       g_cfg.interpolate = atoi(val);
        else if (!_stricmp(key, "interpolateCopter")) g_cfg.interpolateCopter = atoi(val);
        else if (!_stricmp(key, "preciseTicks"))      g_cfg.preciseTicks = atoi(val);
        else if (!_stricmp(key, "targetFPS"))         g_cfg.targetFPS = atoi(val);
        else if (!_stricmp(key, "maxStepDistance"))   g_cfg.maxStepDistance = (float)atof(val);
        else if (!_stricmp(key, "maxStepAngle"))      g_cfg.maxStepAngle = (float)atof(val);
        else if (!_stricmp(key, "interpDelay"))       g_cfg.interpDelay = (float)atof(val);
        else if (!_stricmp(key, "unlockMenus"))       g_cfg.unlockMenus = atoi(val);
        else if (!_stricmp(key, "menuTargetFPS"))     g_cfg.menuTargetFPS = atoi(val);
        else if (!_stricmp(key, "speedUpLoading"))    g_cfg.speedUpLoading = atoi(val);
        else if (!_stricmp(key, "loadScreenRenderMs")) g_cfg.loadScreenRenderMs = atoi(val);
        else if (!_stricmp(key, "fovOffset"))         g_cfg.fovOffset = atoi(val);
        else if (!_stricmp(key, "fovOffsetInCar"))    g_cfg.fovOffsetInCar = atoi(val);
        else if (!_stricmp(key, "fovOffsetBumper"))   g_cfg.fovOffsetBumper = atoi(val);
        else if (!_stricmp(key, "fovOffsetHeli"))     g_cfg.fovOffsetHeli = atoi(val);
        else if (!_stricmp(key, "fovOffsetRoof"))     g_cfg.fovOffsetRoof = atoi(val);
        else if (!_stricmp(key, "fovHotkeys"))        g_cfg.fovHotkeys = atoi(val);
        else if (!_stricmp(key, "fovKeyUp"))          g_cfg.fovKeyUp = (int)strtol(val, nullptr, 0);
        else if (!_stricmp(key, "fovKeyDown"))        g_cfg.fovKeyDown = (int)strtol(val, nullptr, 0);
        else if (!_stricmp(key, "fovKeyReset"))       g_cfg.fovKeyReset = (int)strtol(val, nullptr, 0);
        else if (!_stricmp(key, "fovStep"))           g_cfg.fovStep = atoi(val);
        else if (!_stricmp(key, "glideMenuEffects"))  g_cfg.glideMenuEffects = atoi(val);
        else if (!_stricmp(key, "menuMaxFPS"))        g_cfg.menuMaxFPS = atoi(val);
        else if (!_stricmp(key, "exeCheck"))          g_cfg.exeCheck = atoi(val);
        else if (!_stricmp(key, "logToFile"))         g_cfg.logToFile = atoi(val);
        else if (!_stricmp(key, "logEverySeconds"))   g_cfg.logEverySeconds = atoi(val);
        else if (!_stricmp(key, "widescreen"))        g_cfg.widescreen = atoi(val);
        else if (!_stricmp(key, "wsMenus"))           g_cfg.wsMenus = atoi(val);
        else if (!_stricmp(key, "wsMenuResolution"))  g_cfg.wsMenuResolution = atoi(val);
        else if (!_stricmp(key, "wsFixFOV"))          g_cfg.wsFixFOV = atoi(val);
        else if (!_stricmp(key, "wsHud"))             g_cfg.wsHud = atoi(val);
        else if (!_stricmp(key, "wsMovies"))          g_cfg.wsMovies = atoi(val);
        else if (!_stricmp(key, "wsUnlockResolutions")) g_cfg.wsUnlockResolutions = atoi(val);
        else if (!_stricmp(key, "wsLoading"))         g_cfg.wsLoading = atoi(val);
        else if (!_stricmp(key, "ws32bit"))           g_cfg.ws32bit = atoi(val);
        else if (!_stricmp(key, "hitchLog"))          g_cfg.hitchLog = atoi(val);
        else if (!_stricmp(key, "nightLampsLog"))     g_cfg.nightLampsLog = atoi(val);
        else if (!_stricmp(key, "nightLampsLogKey"))  g_cfg.nightLampsLogKey = (int)strtol(val, nullptr, 0);
        else if (!_stricmp(key, "nightLamps"))        g_cfg.nightLamps = (float)atof(val);
        else if (!_stricmp(key, "nightLampReach"))    g_cfg.nightLampReach = (float)atof(val);
        else if (!_stricmp(key, "nightScenery"))      g_cfg.nightScenery = (float)atof(val);
        else if (!_stricmp(key, "wsLog"))             g_cfg.wsLog = atoi(val);
        else if (!_stricmp(key, "closeExits"))        g_cfg.closeExits = atoi(val);
        else if (!_stricmp(key, "noCDCheck"))         g_cfg.noCDCheck = atoi(val);
        else if (!_stricmp(key, "noDiskSpaceCheck"))  g_cfg.noDiskSpaceCheck = atoi(val);
        else if (!_stricmp(key, "portableInstall"))   g_cfg.portableInstall = atoi(val);
        else if (!_stricmp(key, "alternatePixelCenter")) g_cfg.alternatePixelCenter = atoi(val);
        else if (!_stricmp(key, "gameThreadsOneCore")) g_cfg.gameThreadsOneCore = atoi(val);
        else if (!_stricmp(key, "widenAffinity"))     g_cfg.widenAffinity = atoi(val);
        else if (!_stricmp(key, "language"))          lstrcpynA(g_cfg.language, val, sizeof(g_cfg.language));
        else if (!_stricmp(key, "renderer"))          lstrcpynA(g_cfg.renderer, val, sizeof(g_cfg.renderer));
        else if (!_stricmp(key, "wsRaceWidth"))       g_cfg.wsRaceWidth = atoi(val);
        else if (!_stricmp(key, "wsRaceHeight"))      g_cfg.wsRaceHeight = atoi(val);
    }
    fclose(f);
    AppendMissingKeys(path);
}
