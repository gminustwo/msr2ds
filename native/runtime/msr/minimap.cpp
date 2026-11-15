// Samus's marker on the HUD's map, as the game itself draws it (msr-native), for the compositor's minimap to centre on.
// The picture alone said where it was by its red (bottom_classifier.h FindMapMarker), and a red icon near the map's
// middle could take its place: during a quick scroll a Missile Reload Station's icon was taken for her, and from then on
// either both were seen (not sure, nothing taken) or only the icon while her marker faded, so the minimap stayed on the
// station until it left the map (2026-11-12, the user's video). The game's own minimap knows.
//
// The minimap (Samus Returns USA): the game object (0x76F314) holds a handle at +0x17FC to the HUD's, which holds one at
// +0x550 to the minimap (vtable 0x6E6120); a handle's object is at its +8. LockSamusOnMinimap (0x234038) finds it so
// (0x1A4D30, 0x1AE62C). Each frame the minimap's update (0x506F30) works out Samus's cell on the map from her position
// (+0x188, +0x18C: column, row up from the bottom), gives its icon (`playericon`, held at +0x280, vtable 0x6E5FF8) that
// cell's place in the map (0x1AF928, the interface's SetPosition: +0x50), and places the map so the icon stands at
// (0.375, 0.2917) of the screen, but within the area's bounds (0x18EFFC): near an area's edge the map stops and the icon
// moves instead, a cell (10 px) at a time. Like every object of the game's interface, the icon then keeps where it is
// drawn on the screen once laid out: its top left at +0x10 and its size at +0x1C, in fractions of the 320x240 screen.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "msr.h"
#include "common/common_types.h"
#include "cpu.h"
extern "C" {
#include "kernel.h"
}

namespace {

constexpr u32 GamePointer = 0x76F314;
constexpr u32 HudHandle = 0x17FC, MinimapHandle = 0x550, IconHandle = 0x280;
constexpr u32 MinimapVtable = 0x6E6120, IconVtable = 0x6E5FF8;
constexpr u32 IconRect = 0x10, IconSize = 0x1C;

bool Valid(u32 address) { return address >= 0x00100000 && k_region_state(address) != 0; }
float Rf(u32 a) { u32 w = rd32(a); float f; std::memcpy(&f, &w, 4); return f; }
// the object a handle at owner + offset stands for (0: none)
u32 Held(u32 owner, u32 offset) {
    if (!owner || !Valid(owner + offset + 4)) return 0;
    const u32 handle = rd32(owner + offset);
    return handle && Valid(handle + 12) ? rd32(handle + 8) : 0;
}

} // namespace

namespace {
int Found(int found) {   // PWR_MSR_TRACE: each time the marker comes from the game or stops
    static const bool trace = std::getenv("PWR_MSR_TRACE") != nullptr;
    static int last = -1;
    if (trace && found != last) std::fprintf(stderr, "msr: minimap: %s\n", found ? "the game's marker" : "no marker from the game");
    last = found;
    return found;
}
} // namespace

// Samus's marker on the bottom screen (its middle, in its 320x240 pixels) where the game's minimap draws it; 0 when
// there is none (out of play, another version of the game).
extern "C" int msr_map_marker(float* x, float* y) {
    static int supported = -1;
    if (supported < 0) {
        // the code that finds the minimap and gives its icon the cell, as read above: this game, this version
        supported = rd32(0x1A4D30) == 0xE2800B05u && rd32(0x1A4D34) == 0xE2800FFFu &&   // add r0, r0, #0x17FC
                    rd32(0x1AE62C) == 0xE5900550u &&                                    // ldr r0, [r0, #0x550]
                    rd32(0x507E24) == 0xE5940280u;                                      // ldr r0, [r4, #0x280]
        if (!supported) std::fprintf(stderr, "msr: minimap: not this game's version, the minimap follows the marker's colour\n");
    }
    if (!supported) return 0;
    const u32 minimap = Held(Held(rd32(GamePointer), HudHandle), MinimapHandle);
    if (!minimap || !Valid(minimap + 0x290) || rd32(minimap) != MinimapVtable) return Found(0);
    const u32 icon = Held(minimap, IconHandle);
    if (!icon || !Valid(icon + 0x60) || rd32(icon) != IconVtable) return Found(0);
    const float l = Rf(icon + IconRect), t = Rf(icon + IconRect + 4), w = Rf(icon + IconSize), h = Rf(icon + IconSize + 4);
    if (!std::isfinite(l) || !std::isfinite(t) || !(w > 0.f && w < 1.f) || !(h > 0.f && h < 1.f)) return Found(0);
    if (std::fabs(l) > 4.f || std::fabs(t) > 4.f) return Found(0);
    *x = (l + w * .5f) * 320.f;
    *y = (t + h * .5f) * 240.f;
    return Found(1);
}

// The game's map showing a place of its own choosing instead of Samus's: 1 while the DNA scanner's is, 2 the other such
// moment's, 0 none. When the last Metroid of an area dies, Samus raises her arm and the DNA scanner (the player's
// CADNScannerUserComponent: 0x3C08C0) has the minimap show the way on (0x51053C: +0x22A set, a time at +0x284); the
// scanner and one other caller (0x49B5D4, 0x51019C) use +0x22B the same way. The minimap's update counts the time down and
// clears both when it runs out (0x508000). 2026-11-14, the user: "when you get the last metroid for an area samus pulls up
// something on her wrist and I believe its showing something on the bottom map screen"; the compositor then shows the
// whole bottom screen over the view, as for an elevator's map.
extern "C" int msr_map_focus(void) {
    static int supported = -1;
    if (supported < 0) {
        supported = rd32(0x1A4D30) == 0xE2800B05u && rd32(0x1AE62C) == 0xE5900550u &&
                    rd32(0x510554) == 0xE5C4022Au && rd32(0x510040) == 0xE5C4022Bu &&   // strb r0, [r4, #0x22A / 0x22B]
                    rd32(0x508010) == 0xE5C4622Au && rd32(0x50801C) == 0xE5C4622Bu;     // strb r6, ...: cleared
        if (!supported) std::fprintf(stderr, "msr: minimap: not this game's version, the DNA scanner's map is not shown over the view\n");
    }
    if (!supported) return 0;
    const u32 minimap = Held(Held(rd32(GamePointer), HudHandle), MinimapHandle);
    if (!minimap || !Valid(minimap + 0x290) || rd32(minimap) != MinimapVtable) return 0;
    // PWR_TEST_DNA_SCAN=<s>: that long into play, the minimap set going for 3 s as the scanner sets it (0x51053C: the flags,
    // the time, its rate 1 / time), no area's last Metroid needed (it pans to wherever its target was)
    static const char* test = std::getenv("PWR_TEST_DNA_SCAN");
    if (test) {
        static int frames = 0;
        if (++frames == static_cast<int>(std::atof(test) * 60.f)) {
            const float time = 3.f, rate = 1.f / time;
            std::memcpy(g_mem + minimap + 0x284, &time, 4);
            std::memcpy(g_mem + minimap + 0x288, &rate, 4);
            g_mem[minimap + 0x28C] = 1;
            g_mem[minimap + 0x22A] = 1;
            std::fprintf(stderr, "msr: minimap: the DNA scanner's map set going (PWR_TEST_DNA_SCAN)\n");
        }
    }
    return (rd8(minimap + 0x22A) ? 1 : 0) | (rd8(minimap + 0x22B) ? 2 : 0);
}
