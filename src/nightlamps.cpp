#include "common.h"
#include "nightlamps.h"
#include "config.h"
#include "log.h"
#include "helpers.h"
#include "interp.h"

// ---------------------------------------------------------------------------
// Night lamps on the tracks from Need for Speed III (Hometown, Redrock Ridge,
// Atlantica, Rocky Pass, Country Woods, Lost Canyons, Aquatica, Summit, Empire
// City). Their colours are the day's, which the night's ambient light (the
// track's .ini: AmbientRed, AmbientGreen, AmbientBlue, percent) darkens to a
// few percent, and their lamps are only drawn as glows that light nothing:
// pitch black outside the headlights. This is HPFrameFix's lit night for the
// same tracks, on this game's data. The game's own tracks are left as they are.
//
// The track as loaded (0x4327B0 reads the .frd into one buffer and points the
// blocks into it): block 0x5E05B4 + n * 0x5E8, n up to [0x655B9C];
//  +0x00 the sizes of its 11 polygon lists, +0x2C the lists (13 bytes a
//        polygon: 4 vertices, shorts, then the picture's number, a short),
//        the 5th the full-detail track, the last 4 the objects standing on it;
//  +0x58 the number of vertices, +0x70 them (3 floats), +0x74 their colours
//        (0xAARRGGBB), +0x78 the block's centre;
//  +0x564, +0x56C, +0x574 lists of objects with vertices of their own (a
//        count, then a pointer to entries of 0x34 bytes: +0 the type, +0xC its
//        place, +0x20 the number of vertices, +0x24 them, around the place,
//        +0x28 their colours), and the track's at 0x64F194;
//  +0x5C0 the number of lamps, +0x5C4 them (16 bytes each: a 16.16 place,
//        then the type, a short), which the game only draws as glows
//        (0x436980 -> 0x4E1F10), each type's colour, blinking and size 24
//        bytes at 0x81ACF4 (the same 16 types as in Need for Speed III).
// 0x4323F0 (called at 0x432F17, right after the load) multiplies every one of
// those colours by the ambient light (0x432230). After it, on a night race
// ([0x7C4CF8]) on one of the nine, each steady white, yellow or cyan lamp
// (types 0-4, 8, 13-15) adds its glow's colour to the vertices around it,
// times nightLamps * (1 - (d/r)^2)^2 at a distance d up to its reach r:
// nightLampReach for a street lamp's 4.5 m glow, in proportion for other
// sizes. The blinking and red ones (antennas, warning lights) light nothing.
// More lights are found from the pictures on the track's polygons (NlTrack,
// NlFind): windows, lit signs, tunnel lights and so on.
//
// The nine are known by their number of blocks and of full-detail polygons
// (the same as in Need for Speed III). They are 1/1.3 of that game's size
// (kNlScale: every length below is in its metres, times this). A polygon's
// picture number is not Need for Speed III's (it differs by a track's own
// amount, and on Rocky Pass and Summit not by one amount): the pictures below
// are this game's, found by matching each track's polygons with that game's,
// one by one. The night's ambient light is twice Need for Speed III's on every
// one of them (kNlAmbient).
// ---------------------------------------------------------------------------
static const float kNlScale = 1.0f / 1.3f;
static const float kNlAmbient = 2.0f;
static const DWORD kNlBlocks = 0x5E05B4; static const int kNlBlockSize = 0x5E8;

namespace nl {
struct Lamp { float x, y, z, r, g, b, reach; };
// adds the lamps' light to the colours of n vertices (x, y, z each, around the
// place o); how many it lit
static int Light(const float* v, DWORD* c, int n, const float o[3], const Lamp* L, int nl, float k) {
    int lit = 0;
    for (int i = 0; i < n; i++, v += 3) {
        float x = v[0] + o[0], y = v[1] + o[1], z = v[2] + o[2], add[3] = {};
        for (int j = 0; j < nl; j++) {
            const Lamp& l = L[j];
            float dx = x - l.x, dy = y - l.y, dz = z - l.z;
            float q = (dx * dx + dy * dy + dz * dz) / (l.reach * l.reach);
            if (q >= 1.0f) continue;
            float f = (1.0f - q) * (1.0f - q) * k;
            add[0] += l.r * f; add[1] += l.g * f; add[2] += l.b * f;
        }
        if (add[0] <= 0.0f && add[1] <= 0.0f && add[2] <= 0.0f) continue;
        DWORD old = c[i], out = old & 0xFF000000;
        for (int ch = 0; ch < 3; ch++) {
            int s = 16 - 8 * ch;
            float t = ((old >> s) & 0xFF) + add[ch] * 255.0f + 0.5f;
            out |= (DWORD)(t > 255.0f ? 255.0f : t) << s;
        }
        c[i] = out; lit++;
    }
    return lit;
}
// the lamps that reach any of n vertices around the place o, into out; how many
static int Near(const float* v, int n, const float o[3], const Lamp* L, int nl, Lamp* out) {
    if (n <= 0) return 0;
    float lo[3] = { v[0], v[1], v[2] }, hi[3] = { v[0], v[1], v[2] };
    for (int i = 1; i < n; i++)
        for (int a = 0; a < 3; a++) {
            float t = v[3 * i + a];
            if (t < lo[a]) lo[a] = t;
            if (t > hi[a]) hi[a] = t;
        }
    int m = 0;
    for (int j = 0; j < nl; j++) {
        const Lamp& l = L[j];
        const float p[3] = { l.x, l.y, l.z };
        bool in = true;
        for (int a = 0; a < 3 && in; a++) in = p[a] > lo[a] + o[a] - l.reach && p[a] < hi[a] + o[a] + l.reach;
        if (in) out[m++] = l;
    }
    return m;
}
// n colours (0xAARRGGBB): each channel times mul, plus add (blue, green, red), up to 255
static void Brighten(DWORD* c, int n, float mul, const float add[3]) {
    for (int i = 0; i < n; i++) {
        DWORD out = c[i] & 0xFF000000;
        for (int ch = 0; ch < 3; ch++) {
            float t = ((c[i] >> 8 * ch) & 0xFF) * mul + add[ch] + 0.5f;
            out |= (DWORD)(t > 255.0f ? 255.0f : t) << 8 * ch;
        }
        c[i] = out;
    }
}
}  // namespace nl
static float g_nlK = 1.0f, g_nlReach = 22.0f, g_nlFoundK = 0.0f;   // nightLamps, nightLampReach, nightScenery
static const DWORD kNlTypes = 0xE11F;               // steady white, yellow, cyan: types 0-4, 8, 13-15
static const int kNlMaxLamps = 8192, kNlMaxVerts = 1 << 20;
static nl::Lamp g_nlLamps[kNlMaxLamps], g_nlNear[kNlMaxLamps];
static int g_nlLampN = 0;

enum { kNlWindow, kNlFitting, kNlRoad, kNlBlue, kNlYellow, kNlPole, kNlHead, kNlKinds };
static const struct { float join, reach, k; } kNlKind[kNlKinds] = {     // metres (Need for Speed III's); strength
    { 5, 0, 1 }, { 6, 14, 1 }, { 18, 20, 1 }, { 8, 16, 1 }, { 6, 14, 1 }, { 6, 28, 3 }, { 6, 6, 1.6f },
};                                                                      // (a window's reach: the track's)

// What a polygon with one of a track's pictures gives:
//  - a window (only on a wall, a polygon standing upright): a light 2.5 m in
//    front of its middle, on the side of the block's centre (the road's) for
//    the block's own polygons, away from the object's middle for an object's;
//    with "corners", one at each corner too when they're more than 6 m from
//    the middle (the big panels of the cities' towers, which a light at the
//    middle doesn't reach);
//  - a fitting (a lit panel or sign, a lamp with no light source), a yellow
//    one: a light at its middle, whichever way it faces;
//  - a blue beam or panel: a blue light at its foot, 2 m up;
//  - the road: street lighting 6 m over it, where the track has few lamps of
//    its own (with no colour for it, rcol, only in the tunnels below);
//  - a tunnel (blocks first to last): the lighting over the road there is a
//    fitting's (1) or a yellow one's (4), 5 m up;
//  - a power pole's lamp head: a light for the head itself, and a street
//    lamp's over the road: 6 m over the block's centre's height, at most 8 m
//    from that centre (the heads themselves are 15 to 40 m from the road);
//  - a lamp post's head (it has a light source of the game's): the head lit.
// Lights of a kind within its joining distance of one another become one.
struct NlTrack {
    int track, blocks, polys;                      // its number in Need for Speed III; how it's known (see above)
    short win[40], fit[20], yellow[6], blue[8], road[6], pole[2], head[2];    // pictures (0: the end)
    short lit[4];                                  // animated objects lit all over (a ship's own lights, a
                                                   // turning sign), by their number of vertices
    short tunnel[6][3];                            // first block, last, kind
    float wcol[3], fcol[3], rcol[3];               // the windows', fittings' and road lighting's colours
    float wreach;                                  // a window's reach, m
    float glow;                                    // Need for Speed III's darkened colours x this (a lit city's sky)
    float litk;                                    // the animated objects' light x this
    bool corners;
    short sky[3];                                  // the least light anything has, red, green, blue of 255: the
                                                   // night sky's over a lit city (see NlList)
};
static const char* const kNlName[9] = { "Hometown", "Redrock Ridge", "Atlantica", "Rocky Pass", "Country Woods",
                                        "Lost Canyons", "Aquatica", "Summit", "Empire City" };
static const NlTrack kNlTracks[] = {
    { 0, 120, 11624,                                                                    // Hometown
      { 24, 28, 39, 48, 49, 50, 51, 52, 53, 54, 57, 58, 64, 66, 67, 87, 88, 89, 91, 95, 97 },
      { 146, 148 }, {}, {}, {}, { 161 }, { 29 }, {}, {},
      { 0.8f, 0.58f, 0.3f }, { 0.9f, 0.85f, 0.7f }, {}, 9, 1, 1, false, {} },
    { 1, 183, 18957,                                                                    // Redrock Ridge
      { 13, 63, 82, 89, 91, 117, 121, 122, 124, 126, 127, 135, 136 },
      { 101, 112, 133, 158, 160, 166, 171, 176, 177 }, {}, {}, {}, {}, {}, { 8 }, {},
      { 0.8f, 0.58f, 0.3f }, { 0.9f, 0.85f, 0.7f }, {}, 9, 1, 1, false, {} },
    { 2, 166, 15626,                                                                    // Atlantica (its boat)
      { 9, 10, 11, 12, 13, 16, 17, 18, 19, 21, 25, 26, 30, 31, 32, 33, 36, 38, 52, 53, 58, 90, 93 },
      { 15, 22, 23, 34, 44, 60, 63, 84, 91, 144 }, {}, { 163 }, { 130, 133, 135, 136 }, {}, {}, { 106 }, {},
      { 0.3f, 0.38f, 0.45f }, { 0.9f, 0.9f, 0.82f }, { 0.42f, 0.44f, 0.42f }, 14, 2.5f, 1, true, {} },
    { 3, 186, 19940,                                                                    // Rocky Pass
      { 75, 77, 78, 80, 81, 119, 121, 122, 123, 127, 128, 129, 130, 132, 133, 135, 136, 137, 138, 139, 141, 142,
        143, 152, 154 },
      { 124 }, {}, {}, {}, {}, {}, {}, {},
      { 0.8f, 0.58f, 0.3f }, { 0.9f, 0.85f, 0.7f }, {}, 9, 1, 1, false, {} },
    { 4, 175, 16071,                                                                    // Country Woods (its doughnut)
      { 22, 23, 27, 45, 47, 48, 52, 53, 69, 76, 77, 88, 89, 93, 95, 99, 105, 107, 108, 109, 126, 129 },
      { 54, 55, 56, 64, 65, 66, 71, 72, 73, 101, 102, 103, 112, 130, 133 }, {}, {}, {}, { 142 }, {}, { 50 }, {},
      { 0.8f, 0.58f, 0.3f }, { 0.9f, 0.85f, 0.7f }, {}, 9, 1, 1.7f, false, {} },
    { 5, 212, 25648,                                                                    // Lost Canyons
      { 10, 83, 90, 92, 93, 144, 145, 148, 149, 151, 153, 154, 162, 163 },
      { 69, 113, 160, 183, 185, 191, 196, 200 }, { 60 }, {}, {}, {}, {}, { 8 }, {},
      { 0.8f, 0.58f, 0.3f }, { 0.9f, 0.85f, 0.7f }, {}, 9, 1, 1, false, {} },
    { 6, 180, 17266,                                                                    // Aquatica (its ships)
      { 9, 10, 11, 12, 13, 14, 15, 19, 20, 21, 22, 30, 85, 88, 89, 140, 141 },
      { 61, 63, 81, 86, 87, 139 }, { 26, 93, 95, 96, 131 }, { 134, 135, 142, 143, 165 }, { 110, 113, 115, 116 },
        {}, {}, { 86, 121 },
      { { 6, 16, 4 }, { 22, 27, 4 }, { 39, 44, 4 }, { 123, 125, 4 }, { 151, 153, 4 } },
      { 0.3f, 0.38f, 0.45f }, { 0.9f, 0.9f, 0.82f }, { 0.42f, 0.44f, 0.42f }, 14, 2.5f, 1, true,
      { 60, 76, 100 } },
    { 7, 216, 23942,                                                                    // Summit (its cable cars)
      { 56, 57, 93, 95, 96, 98, 99, 108, 109, 116, 117, 134, 136, 137, 138, 142, 143, 144, 147, 148, 149, 150,
        151, 152, 153, 155, 156, 157, 168, 169 },
      { 139, 140, 190 }, {}, {}, {}, {}, {}, { 72 }, {},
      { 0.8f, 0.58f, 0.3f }, { 0.9f, 0.85f, 0.7f }, {}, 9, 1, 1, false, {} },
    { 8, 154, 13509,                                                                    // Empire City
      { 10, 12, 13, 16, 20, 24, 25, 26, 27, 28, 29, 31, 34, 37, 60, 61, 64, 65, 68, 71, 92, 93, 94, 95, 100,
        101, 104, 105, 197, 198 },
      { 151, 167, 170, 199, 203, 204, 205, 206, 207, 208, 210, 213, 217, 218, 220, 245 }, { 84, 88 },
        { 211, 212 }, {}, {}, {}, {}, {},
      { 0.6f, 0.48f, 0.28f }, { 0.9f, 0.85f, 0.7f }, {}, 12, 1, 1, true, {} },
};
static const NlTrack* g_nlTrack = nullptr;        // the loaded track's, if it's one of the nine
static const int kNlMaxFound = 6144;
static struct { float at[3]; int joined, kind; } g_nlFound[kNlMaxFound];
static int g_nlFoundN = 0;
static bool NlHas(const short* list, size_t n, int pic) {
    for (size_t i = 0; i < n; i++) if (list[i] && list[i] == pic) return true;
    return false;
}
#define NL_HAS(list, pic) NlHas(list, sizeof(list) / sizeof(list[0]), pic)
// a light found: joined to one of its kind within that kind's distance (their places averaged), or a new one
static void NlFound(const float at[3], int kind, float join) {
    for (int j = 0; j < g_nlFoundN; j++) {
        auto& w = g_nlFound[j];
        float dx = w.at[0] - at[0], dy = w.at[1] - at[1], dz = w.at[2] - at[2];
        if (w.kind == kind && dx * dx + dy * dy + dz * dz < join * join) {
            for (int a = 0; a < 3; a++) w.at[a] = (w.at[a] * w.joined + at[a]) / (w.joined + 1);
            w.joined++;
            return;
        }
    }
    if (g_nlFoundN == kNlMaxFound) return;
    auto& w = g_nlFound[g_nlFoundN++];
    memcpy(w.at, at, 12); w.joined = 1; w.kind = kind;
}
static const int kNlPoly = 13;                    // bytes a polygon
// a polygon's 4 vertices (of nv at verts) and their middle; false if it has one outside them
static bool NlQuad(const BYTE* poly, const float* verts, int nv, float v[4][3], float c[3]) {
    short q[4]; memcpy(q, poly, 8);
    for (int k = 0; k < 4; k++) {
        if (q[k] < 0 || q[k] >= nv) return false;
        memcpy(v[k], verts + 3 * (size_t)q[k], 12);
    }
    for (int a = 0; a < 3; a++) c[a] = (v[0][a] + v[1][a] + v[2][a] + v[3][a]) / 4;
    return true;
}
// The lights of m polygons (each at polys[i]) of block b (its centre bc), on nv vertices. A window's light goes
// on the side of the block's centre; for an object's (object: polygons that stand together) away from their
// own middle, unless that middle lies in the wall itself (a wall on its own): then the block's centre's side.
static void NlPolygons(const BYTE* const* polys, int m, const float* verts, int nv, bool object, int b,
                       const float* bc) {
    const NlTrack& T = *g_nlTrack;
    const float s = kNlScale;
    float v[4][3], c[3], mid[3] = {};
    if (object) {
        int k = 0;
        for (int i = 0; i < m; i++)
            if (NlQuad(polys[i], verts, nv, v, c)) { for (int a = 0; a < 3; a++) mid[a] += c[a]; k++; }
        if (!k) return;
        for (float& x : mid) x /= k;
    }
    for (int i = 0; i < m; i++) {
        const BYTE* poly = polys[i];
        short t; memcpy(&t, poly + 8, 2);
        int pic = t;
        int kind = -1, tunnel = 0;
        if (NL_HAS(T.blue, pic)) kind = kNlBlue;
        else if (NL_HAS(T.road, pic)) {
            for (const short* r : T.tunnel) if (r[1] && b >= r[0] && b <= r[1]) tunnel = r[2];
            if (tunnel || T.rcol[0] > 0.0f) kind = kNlRoad;
        }
        if (kind < 0)
            kind = NL_HAS(T.yellow, pic) ? kNlYellow : NL_HAS(T.pole, pic) ? kNlPole : NL_HAS(T.head, pic) ? kNlHead
                 : NL_HAS(T.fit, pic) ? kNlFitting : NL_HAS(T.win, pic) ? kNlWindow : -1;
        if (kind < 0 || !NlQuad(poly, verts, nv, v, c)) continue;
        float join = kNlKind[tunnel ? kNlFitting : kind].join * s;
        if (kind == kNlBlue) {                                        // its foot, 2 m up
            for (int k = 0; k < 4; k++) if (v[k][1] < c[1]) c[1] = v[k][1];
            c[1] += 2.0f * s;
        } else if (kind == kNlPole) {
            NlFound(c, kNlHead, join);                                // (the head itself)
            float dx = bc[0] - c[0], dz = bc[2] - c[2], d = sqrtf(dx * dx + dz * dz);
            if (d > 8.0f * s) { float k = (d - 8.0f * s) / d; c[0] += dx * k; c[2] += dz * k; }
            c[1] = bc[1] + 6.0f * s;
        } else if (kind == kNlRoad) {
            c[1] += (tunnel ? 5.0f : 6.0f) * s;
            if (tunnel) kind = tunnel;
        } else if (kind == kNlWindow) {
            float e1[3], e2[3];
            for (int a = 0; a < 3; a++) { e1[a] = v[1][a] - v[0][a]; e2[a] = v[3][a] - v[0][a]; }
            float nx = e1[1] * e2[2] - e1[2] * e2[1], ny = e1[2] * e2[0] - e1[0] * e2[2];
            float nz = e1[0] * e2[1] - e1[1] * e2[0], h = sqrtf(nx * nx + nz * nz);
            if (h < 1e-6f || fabsf(ny) > h) continue;                 // not a wall
            nx /= h; nz /= h;
            float side = object ? -((mid[0] - c[0]) * nx + (mid[2] - c[2]) * nz) : 0.0f;   // > 0: n points away
            if (fabsf(side) < 0.5f * s) side = (bc[0] - c[0]) * nx + (bc[2] - c[2]) * nz;  // > 0: n points to the road
            if (side < 0) { nx = -nx; nz = -nz; }
            float far2 = 0;
            for (int k = 0; k < 4; k++) {
                float d2 = 0;
                for (int a = 0; a < 3; a++) d2 += (v[k][a] - c[a]) * (v[k][a] - c[a]);
                if (d2 > far2) far2 = d2;
            }
            c[0] += nx * 2.5f * s; c[2] += nz * 2.5f * s;
            NlFound(c, kind, join);
            if (T.corners && far2 > 36.0f * s * s)
                for (int k = 0; k < 4; k++) {
                    const float at[3] = { v[k][0] + nx * 2.5f * s, v[k][1], v[k][2] + nz * 2.5f * s };
                    NlFound(at, kind, join);
                }
            continue;
        }
        NlFound(c, kind, join);
    }
}
// a block's polygon list, checked: its polygons and how many (0 if it isn't sound)
static const BYTE* NlPolyList(const BYTE* B, int j, int* m) {
    int n = *(const int*)(B + 4 * j); const BYTE* p = *(BYTE* const*)(B + 0x2C + 4 * j);
    if (n <= 0 || n > 20000 || !p || IsBadReadPtr(p, (size_t)n * kNlPoly)) { *m = 0; return nullptr; }
    *m = n; return p;
}
// The track's lights found, from the blocks as loaded. The objects standing on a block (its last 4 polygon
// lists) have no bounds of their own here, only their polygons one after another: polygons that share a vertex
// belong to one object.
static void NlFind(int nb) {
    static const BYTE* list[20000]; static int root[20000]; static short owner[32768];
    g_nlFoundN = 0;
    if (!g_nlTrack || !(g_nlFoundK > 0.0f)) return;
    for (int b = 0; b < nb; b++) {
        const BYTE* B = (const BYTE*)(kNlBlocks + b * kNlBlockSize);
        int nv = *(const int*)(B + 0x58); const float* verts = *(const float* const*)(B + 0x70);
        const float* bc = (const float*)(B + 0x78);
        if (nv <= 0 || nv > 32768 || !verts || IsBadReadPtr(verts, (size_t)nv * 12)) continue;
        int m; const BYTE* p = NlPolyList(B, 4, &m);
        for (int i = 0; i < m; i++) list[i] = p + i * kNlPoly;
        if (m) NlPolygons(list, m, verts, nv, false, b, bc);
        for (int j = 7; j < 11; j++) {
            p = NlPolyList(B, j, &m);
            if (!m) continue;
            // owner[v]: a vertex's set (the lowest polygon's number that reaches it), root[i]: a polygon's
            for (int v = 0; v < nv; v++) owner[v] = -1;
            for (int i = 0; i < m; i++) root[i] = i;
            auto find = [](int i) { while (root[i] != i) { root[i] = root[root[i]]; i = root[i]; } return i; };
            for (int i = 0; i < m && i < 32767; i++) {
                short q[4]; memcpy(q, p + i * kNlPoly, 8);
                for (short v : q) {
                    if (v < 0 || v >= nv) continue;
                    if (owner[v] < 0) owner[v] = (short)i;
                    else { int a = find(owner[v]), c = find(i); if (a != c) root[c > a ? c : a] = c > a ? a : c; }
                }
            }
            for (int r = 0; r < m; r++) {
                if (find(r) != r) continue;
                int k = 0;
                for (int i = r; i < m; i++) if (find(i) == r) list[k++] = p + i * kNlPoly;
                NlPolygons(list, k, verts, nv, true, b, bc);
            }
        }
    }
}
// lights one vertex list; adds to *all how many vertices it has, to *lit how many it lit
// Aquatica's colours are painted dark wherever the day's sun doesn't reach (half its vertices, the buildings
// over the tunnels' mouths among them: a fifth of full light). The night's ambient light takes those to a
// fiftieth, and what stands far from every lamp, the tall buildings, was a black shape against the sky. On
// such a track nothing gets less than the sky's own light (sky, times nightScenery up to 1).
static void NlList(const float* v, DWORD* c, int n, const float o[3], int* all, int* lit) {
    static const float kNone[3] = {};
    if (n <= 0 || n > kNlMaxVerts || !v || !c) return;
    *all += n;
    float glow = g_nlTrack->glow / kNlAmbient;      // (the night here is already that much lighter)
    if (glow > 1.0f) nl::Brighten(c, n, glow, kNone);
    if (g_nlTrack->sky[0] && g_nlFoundK > 0.0f) {
        float k = g_nlFoundK < 1.0f ? g_nlFoundK : 1.0f;
        for (int i = 0; i < n; i++) {
            DWORD out = c[i] & 0xFF000000;
            for (int ch = 0; ch < 3; ch++) {
                DWORD x = (c[i] >> (16 - 8 * ch)) & 0xFF, f = (DWORD)(g_nlTrack->sky[ch] * k);
                out |= (x < f ? f : x) << (16 - 8 * ch);
            }
            c[i] = out;
        }
    }
    if (int m = nl::Near(v, n, o, g_nlLamps, g_nlLampN, g_nlNear)) *lit += nl::Light(v, c, n, o, g_nlNear, m, g_nlK);
}
// an object list (a count, then a pointer to entries of 0x34 bytes)
static void NlObjects(const BYTE* list, int* all, int* lit) {
    int n = *(const int*)list; const BYTE* e = *(BYTE* const*)(list + 4);
    if (n <= 0 || n > 100000 || !e) return;
    for (int i = 0; i < n; i++, e += 0x34) {
        int type = *(const int*)e, nv = *(const int*)(e + 0x20);
        DWORD* c = *(DWORD* const*)(e + 0x28);
        if (type == 4) NlList(*(const float* const*)(e + 0x24), c, nv, (const float*)(e + 0xC), all, lit);
        else if ((type == 3 || type == 1) && c && nv > 0 && nv <= kNlMaxVerts && NL_HAS(g_nlTrack->lit, nv)) {
            float k = g_nlFoundK * g_nlTrack->litk;                     // animated (3; Redrock Ridge's sign: 1): its
                                                                        // own lights, warm white
            const float add[3] = { 125.0f * k, 140.0f * k, 140.0f * k };
            nl::Brighten(c, nv, 1.0f, add);
        }
    }
}
// ---------------------------------------------------------------------------
// nightLampsLog: what the track looks like to the game, written out on a key
// (nightLampsLogKey, F10) into HSFrameFix_scene.txt next to the .asi, to tell
// why something on screen has the light it has. NlKeep keeps the blocks'
// colours as they stood after the game's ambient light (0) and after the
// lamps (1); the dump has those and the colours at that moment for every
// vertex, with the camera, every block's lists and every polygon and object
// of the blocks around the camera.
// ---------------------------------------------------------------------------
static DWORD* g_nlKept[2] = {}; static int g_nlKeptN[2] = {}, g_nlKeptBlocks = 0;
static void NlKeep(int which, int nb) {
    if (!g_cfg.nightLampsLog) return;
    int total = 0;
    for (int b = 0; b < nb; b++) { int nv = *(const int*)(kNlBlocks + b * kNlBlockSize + 0x58); if (nv > 0 && nv <= 32768) total += nv; }
    free(g_nlKept[which]); g_nlKept[which] = (DWORD*)malloc((size_t)(total ? total : 1) * 4); g_nlKeptN[which] = 0;
    if (!g_nlKept[which]) return;
    int at = 0;
    for (int b = 0; b < nb; b++) {
        const BYTE* B = (const BYTE*)(kNlBlocks + b * kNlBlockSize);
        int nv = *(const int*)(B + 0x58); const DWORD* c = *(DWORD* const*)(B + 0x74);
        if (nv <= 0 || nv > 32768) continue;
        if (c && !IsBadReadPtr(c, (size_t)nv * 4)) memcpy(g_nlKept[which] + at, c, (size_t)nv * 4);
        else memset(g_nlKept[which] + at, 0, (size_t)nv * 4);
        at += nv;
    }
    g_nlKeptN[which] = at; g_nlKeptBlocks = nb;
}
static void NlDumpObjects(FILE* f, const char* what, int b, const BYTE* list) {
    int n = *(const int*)list; const BYTE* e = *(BYTE* const*)(list + 4);
    if (n <= 0 || n > 100000 || !e || IsBadReadPtr(e, (size_t)n * 0x34)) return;
    for (int i = 0; i < n; i++, e += 0x34) {
        int nv = *(const int*)(e + 0x20), np = *(const int*)(e + 0x2C);
        const float* o = (const float*)(e + 0xC); const float* v = *(const float* const*)(e + 0x24);
        const DWORD* c = *(DWORD* const*)(e + 0x28); const BYTE* p = *(BYTE* const*)(e + 0x30);
        fprintf(f, "O %s %d %d type %d at %.2f %.2f %.2f nv %d np %d anim %d\n", what, b, i, *(const int*)e, o[0], o[1], o[2],
                nv, np, *(const int*)(e + 0x18));
        if (nv <= 0 || nv > 20000 || !v || !c || IsBadReadPtr(v, (size_t)nv * 12) || IsBadReadPtr(c, (size_t)nv * 4)) continue;
        for (int k = 0; k < nv; k++) fprintf(f, "ov %d %.2f %.2f %.2f %08lX\n", k, v[3 * k], v[3 * k + 1], v[3 * k + 2], (unsigned long)c[k]);
        if (np <= 0 || np > 20000 || !p || IsBadReadPtr(p, (size_t)np * kNlPoly)) continue;
        for (int k = 0; k < np; k++, p += kNlPoly) {
            short q[5]; memcpy(q, p, 10);
            fprintf(f, "op %d %d %d %d %d pic %d fl %02X%02X%02X\n", k, q[0], q[1], q[2], q[3], q[4], p[10], p[11], p[12]);
        }
    }
}
void NightLampsDump() {
    static int count = 0;
    char path[MAX_PATH]; snprintf(path, sizeof(path), "%sHSFrameFix_scene.txt", g_asiDir);
    FILE* f = fopen(path, count ? "a" : "w");
    if (!f) { LogfAsync("night lamps log: can't write %s\n", path); return; }
    int nb = Rd<int>(0x655B9C) + 1;
    if (nb <= 0 || nb > 1000) nb = 0;
    DWORD cam = EffectiveCam(0);
    float cp[3] = {}, cm[9] = {};
    if (cam && !IsBadReadPtr((void*)cam, 0x50)) { memcpy(cp, (void*)(cam + 0x14), 12); memcpy(cm, (void*)(cam + 0x2C), 36); }
    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(f, "DUMP %d at %02d:%02d:%02d v" HSFF_VERSION "\n", ++count, st.wHour, st.wMinute, st.wSecond);
    fprintf(f, "track %s blocks %d night %d weather %d ambient %d %d %d nightLamps %.2f reach %.1f scenery %.2f\n",
            g_nlTrack ? kNlName[g_nlTrack->track] : "?", nb, Rd<int>(0x7C4CF8), Rd<int>(0x7C4CFC), Rd<int>(0x5BA2F0),
            Rd<int>(0x5BA2F4), Rd<int>(0x5BA2F8), g_nlK, g_nlReach, g_nlFoundK);
    fprintf(f, "camera %08lX pos %.3f %.3f %.3f rows %.4f %.4f %.4f / %.4f %.4f %.4f / %.4f %.4f %.4f screen %d x %d\n",
            (unsigned long)cam, cp[0], cp[1], cp[2], cm[0], cm[1], cm[2], cm[3], cm[4], cm[5], cm[6], cm[7], cm[8],
            Rd<int>(0x853A98), Rd<int>(0x853A9C));
    for (int t = 0; t < 32; t++) {
        const BYTE* e = (const BYTE*)(0x81ACF4 + 24 * t);
        fprintf(f, "glow %d %08lX blink %d size %.2f on %d off %d\n", t, (unsigned long)*(const DWORD*)e, *(const WORD*)(e + 4),
                *(const float*)(e + 8), *(const int*)(e + 12), *(const int*)(e + 16));
    }
    fprintf(f, "lamps %d (first: the game's light sources, then those found)\n", g_nlLampN);
    for (int i = 0; i < g_nlLampN; i++)
        fprintf(f, "L %d %.2f %.2f %.2f rgb %.3f %.3f %.3f reach %.2f\n", i, g_nlLamps[i].x, g_nlLamps[i].y, g_nlLamps[i].z,
                g_nlLamps[i].r, g_nlLamps[i].g, g_nlLamps[i].b, g_nlLamps[i].reach);
    bool kept = g_nlKeptBlocks == nb && g_nlKept[0] && g_nlKept[1];
    int at = 0;
    for (int b = 0; b < nb; b++) {
        const BYTE* B = (const BYTE*)(kNlBlocks + b * kNlBlockSize);
        const int* n = (const int*)B; const float* bc = (const float*)(B + 0x78);
        int nv = n[0x58 / 4];
        const float* v = *(const float* const*)(B + 0x70); const DWORD* c = *(DWORD* const*)(B + 0x74);
        float dx = bc[0] - cp[0], dz = bc[2] - cp[2];
        bool near_ = dx * dx + dz * dz < 450.0f * 450.0f;
        fprintf(f, "B %d centre %.1f %.1f %.1f nv %d (%d %d %d %d %d) lists %d %d %d %d %d %d %d %d %d %d %d verts %08lX cols %08lX "
                "lights %d sounds %d objs %d %d %d %d %s\n", b, bc[0], bc[1], bc[2], nv, n[0x5C / 4], n[0x60 / 4], n[0x64 / 4],
                n[0x68 / 4], n[0x6C / 4], n[0], n[1], n[2], n[3], n[4], n[5], n[6], n[7], n[8], n[9], n[10],
                (unsigned long)(DWORD)v, (unsigned long)(DWORD)c, n[0x5C0 / 4], n[0x5B8 / 4], n[0x564 / 4], n[0x56C / 4],
                n[0x574 / 4], n[0x57C / 4], near_ ? "near" : "far");
        bool ok = nv > 0 && nv <= 32768 && v && c && !IsBadReadPtr(v, (size_t)nv * 12) && !IsBadReadPtr(c, (size_t)nv * 4);
        if (near_ && ok) {
            // a vertex: place, colour now, as kept after the game's ambient light, as kept after the lamps
            for (int k = 0; k < nv; k++)
                fprintf(f, "v %d %.2f %.2f %.2f %08lX %08lX %08lX\n", k, v[3 * k], v[3 * k + 1], v[3 * k + 2], (unsigned long)c[k],
                        (unsigned long)(kept && at + k < g_nlKeptN[0] ? g_nlKept[0][at + k] : 0),
                        (unsigned long)(kept && at + k < g_nlKeptN[1] ? g_nlKept[1][at + k] : 0));
            for (int j = 0; j < 11; j++) {
                int m; const BYTE* p = NlPolyList(B, j, &m);
                for (int i = 0; i < m; i++, p += kNlPoly) {
                    short q[5]; memcpy(q, p, 10);
                    fprintf(f, "p %d %d %d %d %d %d pic %d fl %02X%02X%02X\n", j, i, q[0], q[1], q[2], q[3], q[4], p[10], p[11], p[12]);
                }
            }
            static const int kLists[4] = { 0x564, 0x56C, 0x574, 0x57C };
            for (int l = 0; l < 4; l++) { char w[8]; snprintf(w, sizeof(w), "b%d", l); NlDumpObjects(f, w, b, B + kLists[l]); }
        }
        if (nv > 0 && nv <= 32768) at += nv;
    }
    NlDumpObjects(f, "track", -1, (const BYTE*)0x64F194);
    fprintf(f, "END %d\n", count);
    fclose(f);
    LogfAsync("night lamps log: scene %d written to HSFrameFix_scene.txt (camera %.1f %.1f %.1f)\n", count, cp[0], cp[1], cp[2]);
}
extern "C" void __cdecl NightLamps() {
    if (!Rd<int>(0x7C4CF8)) return;                 // a day race
    int nb = Rd<int>(0x655B9C) + 1;
    if (nb <= 0 || nb > 1000) return;
    NlKeep(0, nb);
    int polys = 0;
    for (int b = 0; b < nb; b++) polys += *(const int*)(kNlBlocks + b * kNlBlockSize + 4 * 4);
    g_nlTrack = nullptr;
    for (const NlTrack& t : kNlTracks)              // (the polygons: within a hundredth, should a file differ a little)
        if (t.blocks == nb && polys > t.polys - t.polys / 100 && polys < t.polys + t.polys / 100) g_nlTrack = &t;
    if (!g_nlTrack) {
        LogfAsync("night lamps: not one of Need for Speed III's tracks (%d blocks, %d polygons): left as it is\n",
                  nb, polys);
        return;
    }
    const float scale = kNlScale;
    g_nlLampN = 0;
    int skipped = 0;
    for (int b = 0; b < nb; b++) {                  // the game's light sources
        const BYTE* B = (const BYTE*)(kNlBlocks + b * kNlBlockSize);
        int n = *(const int*)(B + 0x5C0); const int* L = *(const int* const*)(B + 0x5C4);
        if (n <= 0 || n > 1000 || !L) continue;
        for (int i = 0; i < n; i++, L += 4) {
            int type = L[3] & 0xFFFF;
            const BYTE* e = (const BYTE*)(0x81ACF4 + 24 * (type & 31));    // its glow: colour, blinking, size
            bool steady = type < 32 && !*(const WORD*)(e + 4) && !(*(const int*)(e + 12) > 0 && *(const int*)(e + 16) > 0);
            if (!steady || !(kNlTypes >> (type & 15) & 1) || g_nlLampN == kNlMaxLamps) { skipped++; continue; }
            DWORD col = *(const DWORD*)e; float size = *(const float*)(e + 8);
            nl::Lamp& l = g_nlLamps[g_nlLampN++];
            l.x = L[0] / 65536.0f; l.y = L[1] / 65536.0f; l.z = L[2] / 65536.0f;
            l.r = ((col >> 16) & 0xFF) / 255.0f; l.g = ((col >> 8) & 0xFF) / 255.0f; l.b = (col & 0xFF) / 255.0f;
            l.reach = g_nlReach * scale * (size > 0.5f && size < 20.0f ? size / 4.5f : 1.0f);
        }
    }
    int street = g_nlLampN;
    NlFind(nb);
    for (int i = 0; i < g_nlFoundN && g_nlLampN < kNlMaxLamps; i++) {    // those found on its polygons
        static const float kBlue[3] = { 0.2f, 0.4f, 1.0f }, kYellow[3] = { 0.95f, 0.72f, 0.25f };
        const NlTrack& T = *g_nlTrack;
        int kind = g_nlFound[i].kind;
        const float* col = kind == kNlWindow ? T.wcol : kind == kNlRoad ? T.rcol : kind == kNlBlue ? kBlue
                         : kind == kNlYellow ? kYellow : T.fcol;
        float k = kNlKind[kind].k * g_nlFoundK / g_nlK;                // (Light multiplies by nightLamps)
        nl::Lamp& l = g_nlLamps[g_nlLampN++];
        l.x = g_nlFound[i].at[0]; l.y = g_nlFound[i].at[1]; l.z = g_nlFound[i].at[2];
        l.r = col[0] * k; l.g = col[1] * k; l.b = col[2] * k;
        l.reach = (kind == kNlWindow ? T.wreach : kNlKind[kind].reach) * scale * g_nlReach / 22.0f;
    }
    int vAll = 0, vLit = 0, oAll = 0, oLit = 0;
    static const float kZero[3] = {};
    static const int kLists[3] = { 0x564, 0x56C, 0x574 };         // (the game leaves +0x57C as it is)
    for (int b = 0; b < nb; b++) {
        const BYTE* B = (const BYTE*)(kNlBlocks + b * kNlBlockSize);
        NlList(*(const float* const*)(B + 0x70), *(DWORD* const*)(B + 0x74), *(const int*)(B + 0x58), kZero,
               &vAll, &vLit);
        for (int list : kLists) NlObjects(B + list, &oAll, &oLit);
    }
    NlObjects((const BYTE*)0x64F194, &oAll, &oLit);
    NlKeep(1, nb);
    LogfAsync("night lamps: %s: %d lamps (%d light nothing), %d more found, vertices lit: track %d of %d, "
              "objects %d of %d\n", kNlName[g_nlTrack->track], street, skipped, g_nlLampN - street,
              vLit, vAll, oLit, oAll);
}
extern "C" { DWORD g_fnAmbient = 0x4323F0; }
extern "C" void NightLampsStub();
asm(".text\n"
    ".globl _NightLampsStub\n"
    "_NightLampsStub:\n"
    "    call *_g_fnAmbient\n"              // the game darkens the track's colours for the night (or not)
    "    pushal\n"
    "    call _NightLamps\n"
    "    popal\n"
    "    ret\n");
void InstallNightLamps() {
    if (!g_knownExe) return;
    float k = g_cfg.nightLamps, reach = g_cfg.nightLampReach, more = g_cfg.nightScenery;
    if (!(k > 0.0f)) { Logf("night lamps: off\n"); return; }
    g_nlK = k > 4.0f ? 4.0f : k;
    g_nlReach = !(reach > 0.0f) ? 22.0f : reach < 2.0f ? 2.0f : reach > 100.0f ? 100.0f : reach;
    g_nlFoundK = !(more > 0.0f) ? 0.0f : more > 4.0f ? 4.0f : more;
    static const BYTE kCall[5] = { 0xE8, 0xD4, 0xF4, 0xFF, 0xFF };      // call 0x4323F0
    bool ok = !memcmp((const void*)0x432F17, kCall, 5) && PatchRel32(0x432F17, 0xE8, (void*)&NightLampsStub);
    Logf("night lamps (Need for Speed III's tracks): x%.2f, reach %.1f m, scenery x%.2f: %s\n", g_nlK, g_nlReach,
         g_nlFoundK, ok ? "on" : "patch FAILED");
}
