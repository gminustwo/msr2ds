// The amiibo extras on msr-native: src/msr/cheats.h's instruction patches, written at boot for the extras the player
// has on; then cheats.cpp keeps them following the settings each VBlank, as the emulator-based build does
// (video_core/gpu.cpp). With a randomizer the Fusion suit stays off (it has its own option).
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "msr.h"
#include "msr/cheats.h"

#include "cpu.h"
extern "C" {
#include "kernel.h"
}

extern "C" void msr_apply_patches(void) {
    namespace C = MSR::Cheats;
    if (!C::randomizer.load() && (rd32(C::RandomizerHook) & 0xFF000000u) == 0xEB000000u) {   // its bl
        C::randomizer = true;
        std::fprintf(stderr, "msr: randomizer code found: the Fusion suit setting stays off\n");
    }
    const u32 enabled = C::enabled.load() & (C::randomizer.load() ? ~static_cast<u32>(C::FusionSuit) : ~0u);
    u32 bad = 0;
    for (const auto& p : C::Patches) {
        if ((enabled & p.flag) && rd32(p.address) != p.original) {
            std::fprintf(stderr, "msr: patch at %08x does not fit this program (%08x)\n", p.address, rd32(p.address));
            bad |= p.flag;
        }
    }
    u32 written = 0;
    for (const auto& p : C::Patches) {
        if ((enabled & p.flag) && !(bad & p.flag)) {
            wr32(p.address, p.replacement);
            written++;
        }
    }
    std::fprintf(stderr, "msr: extras %s%s%s%s: %u instructions rewritten\n",
                 (enabled & ~bad & C::Galleries) ? "[galleries]" : "",
                 (enabled & ~bad & C::FusionMode) ? "[Fusion Mode]" : "",
                 (enabled & ~bad & C::HardMode) ? "[Hard Mode]" : "",
                 (enabled & ~bad & C::FusionSuit) ? "[Fusion suit]" : "", written);
}

namespace {

// Hard Mode from the start, the rest of it: an empty slot's way through the difficulty list, which the game only has for
// a used slot's New Game (cheats.h's patch at 0x4E30C8 sends a new file's Play to it). Three call hooks on the file
// select (`this`, the menu), each acting only for an empty slot (its summary's +0x54, "has a game", is 0):
// - The state change (0x4E40A0: this, the new state, _, at once): the state left plays its way out, then the new one
//   its way in. The new-file panel's (0xE) fades the cockpit and the whole menu out, as it leads to the game's start, so
//   the change to the list (0x2A) leaves from the state a used slot's New Game leaves to it from ("Start a new game?",
//   0x28). The list's Back goes to a used slot's Continue / New Game / Options (0x12): for an empty one, back to the
//   files (0xD; the new-file panel does not open again from the list).
// - The questions after a difficulty (Normal 0x29, Hard 0x2C, Fusion 0x2E) say "Current data will be erased. Start a new
//   game in ... mode?": the text they copy (0x1913C0, from their returns) becomes the used slot's "Start a new game?".
// - Yes then starts the game as a new file's Play does: the list's Yes handlers store the difficulty in the summary
//   (+0x7C) and clear +0xF9C, which Play sets, and call the title music's fade (0x4C1570) without Play's last argument.
constexpr u32 StateChange = 0x4E40A0, TextCopy = 0x1913C0, MusicFade = 0x4C1570;
constexpr u32 MenuState = 0x138, MenuSlot = 0xF50, SlotSummary = 0x154, SummaryHasGame = 0x54, MenuNewFile = 0xF9C;
constexpr u32 Files = 0xD, NewFilePanel = 0xE, SlotMenu = 0x12, NewGameQuestion = 0x28, DifficultyList = 0x2A;
constexpr u32 StartNewGameText = 0x707AD4;                                // "Start a new game?" (0x28's text global)
constexpr std::array<u32, 3> QuestionTextReturns{0x4DF380, 0x4DDDF0, 0x4DF2DC};   // Normal (from the list), Hard, Fusion
constexpr std::array<u32, 3> YesFadeReturns{0x4E36FC, 0x4E3554, 0x4E3898};        // the same three's Yes handlers

bool Mapped(u32 address) { return address >= 0x00100000u && k_region_state(address) != 0; }

// The menu's slot has no game (a new file).
bool EmptySlot(u32 menu) {
    if (!Mapped(menu + MenuSlot)) return false;
    const u32 slot = rd32(menu + MenuSlot);
    if (!Mapped(slot + SlotSummary)) return false;
    const u32 summary = rd32(slot + SlotSummary);
    return Mapped(summary + SummaryHasGame) && rd8(summary + SummaryHasGame) == 0;
}

bool Returns(const std::array<u32, 3>& returns, u32 lr) {
    return std::find(returns.begin(), returns.end(), lr) != returns.end();
}

void StateChangeHook(CPU* c) {
    const u32 menu = c->r[0];
    if (c->r[3] != 0 || !Mapped(menu + MenuState)) return;
    const u32 now = rd32(menu + MenuState);
    if (c->r[1] == DifficultyList && now == NewFilePanel) {
        wr32(menu + MenuState, NewGameQuestion);
    } else if (c->r[1] == SlotMenu && now == DifficultyList && EmptySlot(menu)) {
        c->r[1] = Files;
    }
}

void TextCopyHook(CPU* c) {
    if (Returns(QuestionTextReturns, c->r[14]) && EmptySlot(c->r[4])) c->r[1] = rd32(StartNewGameText);
}

void MusicFadeHook(CPU* c) {
    if (!Returns(YesFadeReturns, c->r[14]) || !EmptySlot(c->r[0])) return;
    wr8(c->r[0] + MenuNewFile, 1);
    c->r[3] = 1;
}

// A bl to `fn` just before each return address: this game's code.
bool CallsFrom(u32 fn, const u32* returns, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
        const u32 bl = rd32(returns[i] - 4);
        const u32 target = returns[i] + 4 + (static_cast<u32>(static_cast<int32_t>(bl << 8) >> 6));
        if ((bl & 0xFF000000u) != 0xEB000000u || target != fn) return false;
    }
    return true;
}

void HookNewFileDifficulty() {
    static constexpr u32 StateChangeCalls[] = {0x4E3470, 0x4DD4D4};   // the Yes handler's to the list, the list's Back
    if (!CallsFrom(StateChange, StateChangeCalls, 2) || !CallsFrom(TextCopy, QuestionTextReturns.data(), 3) ||
        !CallsFrom(MusicFade, YesFadeReturns.data(), 3)) {
        std::fprintf(stderr, "msr: Hard Mode from the start: not this game's code, a new file starts on Normal\n");
        return;
    }
    rt_add_call_hook(StateChange, StateChangeHook);
    rt_add_call_hook(TextCopy, TextCopyHook);
    rt_add_call_hook(MusicFade, MusicFadeHook);
}

// The package heap 8 MB larger, for the Fusion suit switched in play (cheats.cpp SuitFrame). The game keeps its resource
// packages in a 20 MB heap (0x14480000-0x15880000, a TLSF allocator): the system's, Samus's and her suit family's
// packages first, then the area's, then the area's sub-areas (packs/maps/<area>/subareas/subarearp<n>.pkg, up to 5.4 MB
// each), which take turns in the space left as Samus moves between them. A switch in play loads the other suit family
// while hers is still there, so its four packages (1.1 MB) land right behind the sub-area loaded then, in the sub-areas'
// space, and stay there until the next area load. When that sub-area is loaded again after another one, its old place,
// held in by them, is exactly its size, and TLSF only takes a block from a size class above the request's: the load
// failed and its models never came. On 2026-11-18 an Alpha Metroid in Area 2 fought unseen after the user switched to
// the Fusion suit at the save station (sub-area 0, then 2, then 0 again). After a switch the largest sub-areas need up
// to 5.3 MB more (Area 6b's 5.4 MB one); 8 MB leaves room for switching back and forth.
// The game asks for 56 MB of linear memory at boot (the word at 0x704B18, less 256 KB, read at 0x108EC8) and divides it
// (0x51BBC4): 4.5 MB, the package heap's 20 MB, the main heap (the rest less 28 MB), 2 MB and 1.5 MB, each set up by
// 0x51BB14 (this, start, end). It asks for 8 MB more here, and a call hook on that set-up ends the package heap 8 MB later
// and starts the main heap 8 MB later: the main heap keeps its size and the last two their sizes, at the new end.
constexpr u32 LinearSize = 0x704B18, LinearSizeOriginal = 0x03800000, LinearSizeRead = 0x108EC8;
constexpr u32 HeapSetUp = 0x51BB14, PackageHeapSize = 0x51BC00, MainHeapSize = 0x51BBD0;   // add r5, #0x1400000; sub r7, #0x1c00000
constexpr u32 PackageHeapReturn = 0x51BC14, MainHeapReturn = 0x51BC34;   // the set-up's calls for those two
constexpr u32 PackageHeapExtra = 0x800000;

void HeapSetUpHook(CPU* c) {
    if (c->r[14] == PackageHeapReturn) {
        c->r[2] += PackageHeapExtra;
        std::fprintf(stderr, "msr: package heap %08x-%08x (%u MB, 8 MB more for the Fusion suit switched in play)\n", c->r[1],
                     c->r[2], (c->r[2] - c->r[1]) >> 20);
    } else if (c->r[14] == MainHeapReturn) {
        c->r[1] += PackageHeapExtra;
    }
}

void WidenPackageHeap() {
    static constexpr u32 Returns[] = {PackageHeapReturn, MainHeapReturn};
    if (rd32(LinearSize) != LinearSizeOriginal || rd32(LinearSizeRead) != 0xE5940018u || rd32(PackageHeapSize) != 0xE2855505u ||
        rd32(MainHeapSize) != 0xE2407507u || !CallsFrom(HeapSetUp, Returns, 2)) {
        std::fprintf(stderr, "msr: package heap: not this game's code, left at 20 MB\n");
        return;
    }
    if (!rt_add_call_hook(HeapSetUp, HeapSetUpHook)) return;
    wr32(LinearSize, LinearSizeOriginal + PackageHeapExtra);
}

// Floor contacts above 30 FPS. The collision's contact step (0x5440AC) keeps a contact only when the move goes at least
// 0.1 into the surface: its collector's add (0x6BAEA8, vtable 0x6E76EC) takes the dot product of the contact's normal and
// the move (the frame's displacement plus a 15-unit margin along it, +0x388) and drops the contact below +0x394, the 0.1
// the step stores at its start (0x5440F0). The game was tuned at 30 FPS, where a body rolling along a floor goes into it
// by its frame of gravity, 1100 dt^2 = 1.2 (1.37 with the margin). At 144 FPS that is 0.053 (0.085): Arachnus's roll in
// the Area 2 arena (3500 a second) lost its floor contact every frame and sank 0.05 a frame, and once below the surface
// it stayed below (47 units in a 6 s roll; the user's video of 2026-11-22, not at 30 FPS). The minimum scales with the
// frame time squared like that drop (the step's own frame time, the first word of its parameters): 0.1 at 30 FPS, 0.0043
// at 144. Only a minimum of exactly 0.1 changes; the collector's other user (0x43E9xx) asks for 0.6.
// PWR_NO_CONTACT_FIX=1: the game's 0.1 at every frame rate.
constexpr u32 ContactStep = 0x5440AC, ContactAdd = 0x6BAEA8, ContactCollectorVtable = 0x6E76EC;
constexpr u32 ContactMinimum = 0x394, ContactMinimumBits = 0x3DCCCCCDu;   // the collector's field; 0.1f
float contact_dt;   // the last contact step's frame time (the game's thread)

float Rf(u32 address) { const u32 w = rd32(address); float f; std::memcpy(&f, &w, 4); return f; }

void ContactStepHook(CPU* c) { contact_dt = Mapped(c->r[2]) ? Rf(c->r[2]) : 0.f; }

void ContactAddHook(CPU* c) {
    const float dt = contact_dt;
    const u32 at = c->r[1] + ContactMinimum;
    if (!(dt > 0.f && dt < .03f) || !Mapped(at) || rd32(at) != ContactMinimumBits) return;   // 30 FPS: as the game has it
    const float scale = dt * 30.f, minimum = .1f * scale * scale;
    u32 bits; std::memcpy(&bits, &minimum, 4);
    wr32(at, bits);
}

void ScaleContactMinimum() {
    // the step's push {r0-fp, ip, lr}, its collector at sp+0x10 (ldr r0, =vtable; str r0, [sp, #0x10]), its frame time
    // check (ldr r0, [r0]; cmp r0, #0x34000000) and its vstr s0, [sp, #0x3a4] of the literal 0.1; the add's
    // add r4, r1, #0x390 and vldmia r4, {s2, s3} (the minimum in s3)
    if (rd32(ContactStep) != 0xE92D5FFFu || rd32(0x5440DC) != 0xE59F02D4u || rd32(0x5443B8) != ContactCollectorVtable ||
        rd32(0x5440E0) != 0xE58D0010u || rd32(0x5440F8) != 0xE5900000u || rd32(0x544104) != 0xE350030Du ||
        rd32(0x5440F0) != 0xED8D0AE9u || rd32(0x5443B0) != ContactMinimumBits || rd32(ContactCollectorVtable) != ContactAdd ||
        rd32(0x6BAEAC) != 0xE2814E39u || rd32(0x6BAEC4) != 0xEC941A02u) {
        std::fprintf(stderr, "msr: floor contacts: not this game's code, the collision keeps its 30 FPS minimum\n");
        return;
    }
    if (std::getenv("PWR_NO_CONTACT_FIX")) {
        std::fprintf(stderr, "msr: floor contacts: the game's 0.1 minimum at every frame rate (PWR_NO_CONTACT_FIX)\n");
        return;
    }
    if (rt_add_call_hook(ContactStep, ContactStepHook) && rt_add_call_hook(ContactAdd, ContactAddHook))
        std::fprintf(stderr, "msr: floor contacts: the collision's minimum follows the frame time above 30 FPS\n");
}

// The credits: their update (0x262E98, the credits object's method at 0x6CC37C; it scrolls them, frame_rate.h) runs on
// each frame they show, after the ending, from the menus, or in a state saved during them, and at no other time. While
// it runs the compositor keeps their touch screen's emblem and shows them out of play (msr_credits_showing,
// compositor.cpp: in the single view they showed twice; 2026-11-28).
constexpr u32 CreditsUpdate = 0x262E98, CreditsUpdateSlot = 0x6CC37C;
std::atomic<long long> credits_seen_ns{0};   // when the update last ran (steady clock), 0 never

long long SteadyNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void CreditsUpdateHook(CPU*) { credits_seen_ns.store(SteadyNs(), std::memory_order_relaxed); }

void WatchCredits() {
    // its push {r4-r8, lr} and mov r4, r0, and the credits object's method table holding it
    if (rd32(CreditsUpdate) != 0xE92D41F0u || rd32(CreditsUpdate + 4) != 0xE1A04000u || rd32(CreditsUpdateSlot) != CreditsUpdate) {
        std::fprintf(stderr, "msr: credits: not this game's code, left in the wide view\n");
        return;
    }
    if (!rt_add_call_hook(CreditsUpdate, CreditsUpdateHook)) std::fprintf(stderr, "msr: credits: no hook left\n");
}

} // namespace

// The credits are on: their update ran in the last half second
extern "C" int msr_credits_showing(void) {
    const long long seen = credits_seen_ns.load(std::memory_order_relaxed);
    return seen != 0 && SteadyNs() - seen < 500'000'000LL;
}

// The words patchable.txt lists that native code built before them does not check: the recompiled functions holding
// them are turned off (menu code only), so the interpreter runs them as the patches leave them, whichever native code
// the player has. The words added since a release; a rebuilt module checks them itself, which changes nothing here.
extern "C" void msr_patches_module_loaded(void) {
    static constexpr u32 Unchecked[] = {
        0x4E30C8,   // Hard Mode from the start: a new file's Play (2026-11-03)
        0x25E300,   // the credits' scroll at the frame rate's pace (frame_rate.h StepPatches, main.c; #93, 2026-11-28)
    };
    rt_code_patchable(Unchecked, static_cast<u32>(sizeof Unchecked / sizeof Unchecked[0]));
    HookNewFileDifficulty();
    WidenPackageHeap();
    ScaleContactMinimum();
    WatchCredits();
    msr_draw_lists_install();
}
