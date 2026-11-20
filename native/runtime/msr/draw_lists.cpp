// The level's instanced pieces past the 3DS's budget (msr-native). The level's static pieces (rock, breakable blocks,
// background props) are instances the scene tests against the view each frame (0x1B9958: a sphere against the view's
// six planes, 0x1D9B1C) and lists, sorted by model, at G+0x155CC4 (G = [0x76F358], the count at G+0x155D20). The
// renderer's batch builder (0x58AC30, once per render pass) then takes the first 300 of them and fills two draw lists
// in the renderer, (sub-mesh, instance transform) pairs grouped by model: list A (+0x6504, count +0x6E64) for the
// sub-meshes whose material has 1 at +0x74, list B (+0x6E68, count +0x77C8) for the rest, 299 entries each. The draw
// loop (0x58B4EC) draws one of them per call: A when the renderer's +0x89E0 is 1, B otherwise.
// The 3DS's view never held more, but the wider view (camera_fov.cpp) does: 312 instances in a room of Area 2 at 21:9
// against 196 at 3DS's frame, and whatever sorted last was left out: the breakable blocks above Samus, there in the 3DS
// view and gone in the wider one (2026-11-19).
// Here the builder is the game's own logic without the 300: the first 299 entries of each list go where the game keeps
// them, the rest wait here; a draw of a list with more waiting calls the game's draw loop again for each next 299, in
// the game's order, and puts the first 299 back for the render pass's next draw of that list. Within the 3DS's budget
// nothing changes.
#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <vector>
#include "msr.h"
#include "common/common_types.h"

#include "cpu.h"

namespace {

constexpr u32 Builder = 0x58AC30, BuilderCall = 0x58F6C0;        // push {r0, r4-fp, lr}; its call in the render pass
constexpr u32 DrawLoop = 0x58B4EC;                               // push {r4-fp, lr}
constexpr u32 ScenePointer = 0x76F358;                           // G
constexpr u32 VisibleArray = 0x155CC4, VisibleCount = 0x155D20;  // in G
constexpr u32 GroupCounts = 0x6050;                              // renderer: instances per model, from index 1
constexpr u32 MaxGroups = 300;
constexpr u32 ListFlag = 0x89E0;
constexpr u32 Room = 299;                                        // entries a list takes
constexpr u32 ListEntries[2] = {0x6504, 0x6E68}, ListCount[2] = {0x6E64, 0x77C8};

struct Entry {
    u32 sub, transform;
};
std::vector<Entry> waiting[2];   // past each list's 299 (the game thread)
u32 waiting_for;                 // the renderer they belong to
thread_local bool drawing;       // in the draw loop's calls from here
std::size_t logged_parts = 1;

void BuilderHook(CPU* c) {
    const u32 r = c->r[0];
    waiting[0].clear();
    waiting[1].clear();
    waiting_for = r;
    const u32 g = rd32(ScenePointer);
    const u32 n = g ? rd32(g + VisibleCount) : 0, list = g ? rd32(g + VisibleArray) : 0;
    u32 groups = 0;
    for (u32 i = 0; i < n;) {
        const u32 model = rd32(rd32(list + 4 * i));
        u32 end = i + 1;
        while (end < n && rd32(rd32(list + 4 * end)) == model) ++end;
        if (++groups <= MaxGroups) wr32(r + GroupCounts + 4 * groups, end - i);
        const u32 res = rd32(model + 0x44), subs = rd32(res + 0x2C), nsubs = rd32(res + 0x28);
        for (u32 s = 0; s < nsubs; ++s) {
            const u32 sub = subs + 20 * s;
            const int b = rd32(rd32(sub + 4) + 0x74) == 1 ? 0 : 1;
            for (u32 k = i; k < end; ++k) {
                const u32 transform = rd32(list + 4 * k) + 4, count = rd32(r + ListCount[b]);
                if (count < Room) {
                    wr32(r + ListEntries[b] + 8 * count, sub);
                    wr32(r + ListEntries[b] + 8 * count + 4, transform);
                    wr32(r + ListCount[b], count + 1);
                } else {
                    waiting[b].push_back({sub, transform});
                }
            }
        }
        i = end;
    }
    const std::size_t parts = 1 + (std::max(waiting[0].size(), waiting[1].size()) + Room - 1) / Room;
    if (parts > logged_parts && logged_parts < 8) {
        logged_parts = parts;
        std::fprintf(stderr, "msr: %u level pieces in view (the 3DS's renderer takes 300): drawn in %zu parts\n", n, parts);
    }
    c->r[15] = c->r[14];   // the game's builder is not run
}

void Call(CPU* c, const CPU& at, u32 renderer, u32 flag) {
    *c = at;
    wr32(renderer + ListFlag, flag);
    c->r[0] = renderer;
    c->r[13] = (at.r[13] - 0x40) & ~7u;
    c->r[14] = EXIT_MAGIC;
    c->r[15] = DrawLoop;
    c->thumb = 0;
    rt_run(c);
}

void DrawHook(CPU* c) {
    if (drawing) return;
    const u32 r = c->r[0], flag = rd32(r + ListFlag);
    const int b = flag == 1 ? 0 : 1;
    if (r != waiting_for || waiting[b].empty()) return;
    const u32 own = rd32(r + ListCount[b]);
    std::vector<Entry> first(own);
    for (u32 k = 0; k < own; ++k) first[k] = {rd32(r + ListEntries[b] + 8 * k), rd32(r + ListEntries[b] + 8 * k + 4)};
    const CPU at = *c;
    drawing = true;
    Call(c, at, r, flag);
    for (std::size_t from = 0; from < waiting[b].size(); from += Room) {
        const u32 m = static_cast<u32>(std::min<std::size_t>(Room, waiting[b].size() - from));
        for (u32 k = 0; k < m; ++k) {
            wr32(r + ListEntries[b] + 8 * k, waiting[b][from + k].sub);
            wr32(r + ListEntries[b] + 8 * k + 4, waiting[b][from + k].transform);
        }
        wr32(r + ListCount[b], m);
        Call(c, at, r, flag);
    }
    for (u32 k = 0; k < own; ++k) {
        wr32(r + ListEntries[b] + 8 * k, first[k].sub);
        wr32(r + ListEntries[b] + 8 * k + 4, first[k].transform);
    }
    wr32(r + ListCount[b], own);
    wr32(r + ListFlag, flag);
    drawing = false;
    *c = at;
    c->r[15] = c->r[14];   // drawn: back to the caller
}

} // namespace

extern "C" void msr_draw_lists_install(void) {
    // this game's code: the builder's and draw loop's first words, the builder's 300 and 299, its offsets in G and the
    // material word, the draw loop's flag and lists, the render pass's call
    const u32 bl = rd32(BuilderCall);
    const bool ours = rd32(Builder) == 0xE92D4FF1u && rd32(0x58AC60) == 0xE3510F4Bu && rd32(0x58AE34) == Room &&
                      rd32(0x58AE24) == ScenePointer && rd32(0x58AE28) == VisibleCount && rd32(0x58AE2C) == VisibleArray &&
                      rd32(0x58AE30) == GroupCounts && rd32(0x58AD8C) == 0xE5900074u && rd32(DrawLoop) == 0xE92D4FF0u &&
                      rd32(0x58B500) == 0xE59419E0u && rd32(0x58B510) == 0xE2868B1Du && rd32(0x58B5A0) == 0xE2868B1Bu &&
                      (bl & 0xFF000000u) == 0xEB000000u && BuilderCall + 8 + (static_cast<u32>(static_cast<int32_t>(bl << 8) >> 6)) == Builder;
    if (!ours) {
        std::fprintf(stderr, "msr: level pieces: not this game's code, the 3DS's budget of 300 stays\n");
        return;
    }
    if (std::getenv("PWR_NO_DRAW_LISTS")) return;   // A/B: the game's own builder
    if (!rt_add_call_hook(Builder, BuilderHook) || !rt_add_call_hook(DrawLoop, DrawHook))
        std::fprintf(stderr, "msr: level pieces: no room for the hooks, the 3DS's budget of 300 stays\n");
}
