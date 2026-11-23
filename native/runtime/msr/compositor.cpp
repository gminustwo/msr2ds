// The single 16:9 screen on msr-native: the top screen fills the view and the bottom screen's HUD pieces, menus and
// prompts are composited onto it, as the emulator-based build does (video_core/renderer_vulkan/renderer_vulkan.cpp
// DrawMsrScreens and ConsumeMsrReadbacks; keep the two in step). The classifier and the shared state are src/msr's
// own (bottom_classifier.h, presentation.h). Differences: the bottom screen is classified the frame it is shown
// (the runtime decodes it on the CPU anyway, no readback delay), fades follow host time (the game runs in real
// time here), and the result is a list of quads video.c draws (msr.vert/msr.frag) instead of Vulkan calls.
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>
#include "msr.h"
#include "msr/bottom_classifier.h"
#include "msr/presentation.h"

extern "C" int msr_overlay_visible(void);   // overlay_host.cpp
extern "C" {
#include "input.h"
}
#include "msr/touch_nav.h"

namespace {

namespace P = MSR::Presentation;
namespace BC = MSR::BottomClassifier;
using s64 = long long;

struct Rect {
    float l = 0, t = 0, r = 0, b = 0;
    float W() const { return r - l; }
    float H() const { return b - t; }
};

struct Vec2 { float x, y; };
struct Vec3 { float r, g, b; };

struct Style {
    float opacity = 1.f;
    bool keyed = false;
    float key_low = 0.f, key_high = 0.f;
    int feather = 0, circle = 0;
    bool over_black = false;
    float gain = 1.f;
    bool hard_borders = false;
};

constexpr float MapWindowWidth = 120.f, MapWindowHeight = 80.f;
// Samus's marker on the HUD's map: where the game keeps it, and how far it may be from there or from its last place
// to be taken at once (it moves a map tile, 10 px, at a time)
constexpr Vec2 MapMiddle{125.f, 135.f};
constexpr float MarkerReach = 24.f;
constexpr float FadePerSecond = 9.f, PromptFadePerSecond = 6.f;
// R held this long: the game shows the missiles in the beam slots (it draws them the next frame or so)
constexpr long long MissileSlotsUs = 120'000;

Rect Source(float x0, float y0, float x1, float y1) { return {x0 / 320.f, y0 / 240.f, x1 / 320.f, y1 / 240.f}; }
float Approach(float v, float target, float step) { return v < target ? std::min(target, v + step) : std::max(target, v - step); }

// renderer_vulkan.h's msr_ members
struct State {
    int candidate = -1, candidate_frames = 0;
    s64 candidate_since_us = 0;
    int page_candidate = -1, page_frames = 0;
    s64 page_since_us = 0;
    float panel_alpha = 0, hud_alpha = 0, prompt_alpha = 0;
    s64 last_us = 0;
    long long start_seen = -1;
    s64 pausing_until_us = 0, resuming_until_us = 0;
    bool paused = false;
    s64 prompt_since_us = 0, prompt_seen_us = 0;
    bool prompt_over_menu = false;   // the prompt showing came straight from a menu: one of its dialogs (msr_compose)
    bool in_play = false;
    s64 hud_seen_us = -1'000'000'000;   // the HUD last showed (the pad's menus' layout, glyphs.cpp): not yet
    bool menu_icons = true;   // the icons in the menus' layout: the game starts in its menus
    float zoom = 1.f;
    float aspect = 0.f;   // the view's aspect, easing to the wanted one (ViewAspectWanted); 0: not yet
    Rect prompt_text, prompt_card, prompt_card_shown;
    s64 prompt_card_us = 0, prompt_tall_us = 0, prompt_foreign_us = 0;
    float prompt_fit = 0;
    float bottom_peak = 0;
    bool bottom_black = false;
    float gallery = 0;
    Vec2 map_marker{125.f, 135.f}, map_center{125.f, 135.f};
    Vec2 marker_far{};          // a marker found away from the last one (Consume), since marker_far_us (-1: none)
    s64 marker_far_us = -1;
    std::array<Vec3, 4> map_backdrop{};
    std::array<s64, 3> beam_seen_us{-1, -1, -1}, beam_empty_us{-1, -1, -1};
    bool beams_grayed = false;
    float beam_gray = 0;
    bool beams_held = false;   // R held: the beam slots as they were (msr_compose)
    s64 beams_up_us = -1;      // R came up then (-1: still held)
    s64 r_down_us = -1;        // R went down then (-1: up)
    int missile_lit = -1;      // R held: the lit one of the slots as the missiles show in them, read this hold (-1: not yet)
    int missile_present = 0;   // ...and the slots with an icon
    int missile_pending = 0;   // an RT (1) or a wheel notch up (2) with R held before that was read: sent once it is
    unsigned buttons = 0;
    bool menu_asked = false;   // a Start press opened the menus over play: they show as usual until the HUD is back
    bool menu_unasked = false; // the menu showing over play came without one (Consume)
    bool fit_wanted = false;   // the pause screen laid out over the whole window (the menu's option), as last decided
    float fit = 0.f;           // ...eased there (0 its usual place, 1 the window)
    // the panel as last drawn from the game's interface, for the mouse's touches (msr_panel_touch): its pieces, where,
    // and its width past 4:3 (0: none, linear as video.c maps it)
    const std::vector<MsrGuiItem>* touch_items = nullptr;
    Rect touch_panel;
    float touch_extra = 0.f;
    bool panel_moved = false;  // the panel was last drawn elsewhere than its centered place (msr_single_layout)...
    Rect panel_drawn;          // ...there, in a window of composed_w x composed_h
    float composed_w = 0, composed_h = 0;
    bool menu_live = false;    // this frame's bottom screen reads as a menu (Consume; msr_menu_live)
    s64 map_focus_until_us = 0;   // the game's map shows a place of its own (msr_map_focus) until then
    bool map_focus_on = false;    // ...as last composed
    s64 map_focus_since_us = 0;   // ...since then
    bool map_focus_capped = false;   // ...for too long: left until the game's map is back on Samus
    bool panel_focus = false;     // the panel shows that map, or fades out from it: live
    bool menu_see_through = false;   // the title menus' backdrop was left out of it, as last seen
    s64 pause_seen_us = -1;          // a pause page last drawn from the game's interface
    s64 pause_bridge_us = -1;        // the touch screen between two pause pages since then (-1: not)
    s64 pause_arrive_us = -1;        // the next page came then (its contents fade in)
    float abilities_alpha = 0.f;     // the Aeion abilities' page over the view (AbilitiesOverlay), faded in
    bool abilities_hold = false;     // ...and no panel for it until the touch screen no longer reads as a menu
    bool abilities_seen = false;     // ...its description's box seen since the page came (the game slides it in and out)
    float abilities_top = 0.f;       // ...the highest it has come up
    bool abilities_going = false;    // ...and it has gone down from there since: it is going (the overlay fades out)
    int abilities_prompt = 0;        // ...the button its box would show (AbilitiesPage.prompt)...
    s64 abilities_prompt_us = 0;     // ...since then
} st;

std::vector<MsrQuad> quads;
s64 t0_us = -1;

void Region(const Rect& src, float x, float y, float w, float h, const Style& s) {
    MsrQuad q{};
    q.dst[0] = x; q.dst[1] = y; q.dst[2] = w; q.dst[3] = h;
    q.src[0] = src.l; q.src[1] = src.t; q.src[2] = src.r; q.src[3] = src.b;
    q.style[0] = s.opacity;
    q.style[1] = s.over_black || !s.keyed ? s.gain : s.key_low;   // (msr.frag: modes 0 and 3 take a gain)
    q.style[2] = s.key_high;
    q.style[3] = s.over_black ? 3.f : s.keyed ? 1.f : 0.f;
    q.feather = s.feather;
    if (s.hard_borders) {   // msr.frag: bits 8-11 = left, top, right, bottom (screen-oriented, unlike the emulator's)
        q.feather |= (src.l <= 0.f ? 0x100 : 0) | (src.t <= 0.f ? 0x200 : 0) | (src.r >= 1.f ? 0x400 : 0) |
                     (src.b >= 1.f ? 0x800 : 0);
    }
    q.circle = s.circle;
    quads.push_back(q);
}

void Fill(float x, float y, float w, float h, float r, float g, float b, float a, int feather = 0) {
    MsrQuad q{};
    q.dst[0] = x; q.dst[1] = y; q.dst[2] = w; q.dst[3] = h;
    q.style[0] = 1.f; q.style[3] = 2.f;
    q.color[0] = r; q.color[1] = g; q.color[2] = b; q.color[3] = a;
    q.feather = feather;
    quads.push_back(q);
}

// The title menus' touch screen rebuilt from the game's interface (gui_tree.cpp, 2026-11-17): its sprites and glyphs
// drawn into (x, y, w, h) as the touch screen's 320x240 would be, with the game's own pictures (gui_assets.cpp), by
// the game's blends (additive ones kept as mode 3 keeps a copy's light). Its backdrop (the blue scanlined fill, the
// emblem and the logo behind it, by their sheet items) is left out when the panel is see-through, as the renderer
// leaves it out of the copy. pulse_floor >= 0: the prompt's pulse, its alpha drawn between that and 1 (the copy's
// gain brought the faded text back the same way). focus: a footer button the D-pad has chosen (its name hash), drawn as
// the game draws its chosen menu buttons.
constexpr std::array<uint32_t, 3> BackdropSprites = {0x0CCA487Fu /* Background */, 0xA87D80B9u /* Circle_Background_01 */,
                                                     0xBD85581Fu /* Circle_Logo */};
// The game fades the touch screen with a black WhiteSquare over all of it (Fade): the copy, drawn over black, faded
// out with it; here the fade takes that much opacity from what it covers instead of darkening the view behind
bool Fader(const MsrGuiItem& it) {
    return !it.glyph && it.sprite == 0x7D14C5DBu /* WhiteSquare */ && it.w >= 300.f && it.h >= 220.f &&
           std::max({it.color[0], it.color[1], it.color[2]}) < .1f;
}
bool Backdrop(const MsrGuiItem& it) {
    return Fader(it) ||
           (!it.glyph && std::find(BackdropSprites.begin(), BackdropSprites.end(), it.sprite) != BackdropSprites.end());
}
constexpr uint32_t CopyBase = 0x33FECAD1u, EraseBase = 0xFAF5513Eu, CopyIcon = 0x96DE1D6Bu, EraseIcon = 0x5FD58684u;
// The pause map's toolbar (2026-11-17: its buttons on the D-pad, pause_nav below; the amiibo one always darkened, as
// the port has no amiibo): bases and icons
constexpr uint32_t AmiiboBase = 0x4CE91F5Au, AmiiboIcon = 0xE9C9C8E0u;
struct ToolbarButton { uint32_t base, icon; };
// The map pins tool's pins (MarkerA-E: red, green, yellow, light blue, blue), between separators 33 pixels apart
constexpr std::array<uint32_t, 5> PinMarkers = {0xB91C48D3u /* MarkerA */, 0x20151969u /* MarkerB */, 0x571229FFu /* MarkerC */, 0xC976BC5Cu /* MarkerD */, 0xBE718CCAu /* MarkerE */};
// ...a pin's sprite (the bar's, and those placed on the map: MarkerX<column>Y<row>, in the map's view), and the tool's
// Delete all button in the bar's right corner
constexpr uint32_t PinSprite = 0xCAF9D1FFu /* Marker_A */;
constexpr uint32_t DeleteAllBase = 0x79EE44DBu /* ButtonSelectMarkersBase */, DeleteAllIcon = 0xDCCE9361u /* ...Icon */;
bool PlacedPin(const MsrGuiItem& it) {
    return !it.glyph && it.sprite == PinSprite && it.clip[2] > it.clip[0] &&
           std::find(PinMarkers.begin(), PinMarkers.end(), it.name) == PinMarkers.end();
}
constexpr std::array<ToolbarButton, 4> PauseToolbar = {{
    {0xD76D5A2Du, 0x724D8D97u},   // ButtonWorldMap
    {0x342C9189u, 0x910C4633u},   // ButtonMarkersEditor (map pins)
    {0xD0F7BFDEu, 0x75D76864u},   // ButtonLegend (information)
    {0xF902FBBDu, 0x5C222C07u},   // ButtonPreferences (settings)
}};
// The open file's choices (ButtonMenuBig1..4: their bases, Button_Selected the chosen, and labels) and the lines the game
// draws around the chosen one
constexpr uint32_t ButtonSelected = 0xDC889D66u, ButtonNormal = 0x2427C639u;
constexpr std::array<uint32_t, 4> MenuBigBases = {0x593D76C2u, 0x1E9D0C12u, 0x23FD25A2u, 0x91DDF9B2u};
constexpr std::array<uint32_t, 4> MenuBigLabels = {0x93E3C4BFu, 0x1577B611u, 0xDE2B65B4u, 0xC32E550Cu};
constexpr std::array<uint32_t, 2> SelectionLines = {0x1605F594u /* SelectionHighlightTop */, 0x09C3EE4Fu /* ...Bot */};

// A touch-screen x in the window, for a piece of the game's interface drawn at x, w (in the window) with extra pixels of
// the touch screen's past 4:3 (0: as the game draws it): kept to the left or right edge, or the middle (the map's too)
float AnchorX(float tx, int anchor, float x, float s, float extra) {
    return x + (tx + (anchor == MSR_ANCHOR_LEFT ? 0.f : anchor == MSR_ANCHOR_RIGHT ? extra : extra / 2)) * s;
}
// ...and for one stretched between its ends: its ends (40% of it each: a bar's tab, its step to the title) keep their
// shape where each end goes (msr_anchor_end), its middle (a bar's straight run) takes the rest
// The Samus page's description line (ButtonOrnament, from the middle to the right edge) starts where its slanted start
// ends, kept to the left edge with the Back tab (gui_tree.cpp LeftAnchored): its left end goes with them
constexpr uint32_t LeftEnded[] = {0xC212104Fu /* ButtonOrnament */};
constexpr float StretchCap = .4f;
float StretchX(const MsrGuiItem& it, float tx, float x, float s, float extra) {
    const float cap = it.w * StretchCap, a = it.x + cap, b = it.x + it.w - cap;
    const bool left_ended = std::find(std::begin(LeftEnded), std::end(LeftEnded), it.name) != std::end(LeftEnded);
    const int left = left_ended ? MSR_ANCHOR_LEFT : msr_anchor_end(it.x), right = msr_anchor_end(it.x + it.w);
    if (tx <= a || b <= a) return AnchorX(tx, left, x, s, extra);
    if (tx >= b) return AnchorX(tx, right, x, s, extra);
    const float from = AnchorX(a, left, x, s, extra), to = AnchorX(b, right, x, s, extra);
    return from + (tx - a) / (b - a) * (to - from);
}
// A piece's place between the anchors' (0 the left edge's, .5 the middle's, 1 the right edge's), eased over 0.1 s when its
// anchor changes (a column the game slides in from past the edge is against it until it lands: it comes in with the
// middle instead of jumping there), by its key (its name and which of the pieces so named it is: the headers' two
// HeaderUp lines, left and right, shared one place and the right one was drawn at the left, across the title);
// GuiQuads' call with seconds updates it once a call.
std::unordered_map<uint32_t, std::pair<float, unsigned>> anchor_places;
unsigned anchor_call = 0;
float AnchorPlace(const MsrGuiItem& it, uint32_t key, float seconds) {
    const float target = it.anchor == MSR_ANCHOR_LEFT ? 0.f : it.anchor == MSR_ANCHOR_RIGHT ? 1.f : .5f;
    if (seconds <= 0.f || !it.name || it.anchor == MSR_ANCHOR_MAP || it.anchor == MSR_ANCHOR_STRETCH) return target;
    if (anchor_places.size() > 4096) anchor_places.clear();
    auto [entry, fresh] = anchor_places.try_emplace(key, target, anchor_call);
    auto& [place, call] = entry->second;
    if (!fresh && call != anchor_call) {
        call = anchor_call;
        const float step = std::min(seconds, .1f) * 10.f;
        place = std::abs(target - place) <= step ? target : place + (target > place ? step : -step);
    }
    return place;
}
float ItemX(const MsrGuiItem& it, float tx, float x, float s, float extra, float place = -1.f) {
    if (it.anchor == MSR_ANCHOR_STRETCH && extra > 0.f) return StretchX(it, tx, x, s, extra);
    if (place < 0.f) return AnchorX(tx, it.anchor, x, s, extra);
    return x + (tx + extra * place) * s;
}

// extra: the touch screen drawn wider than 4:3 by so many of its pixels (the pause screen fitted to the window), its
// pieces where their anchors say (MsrGuiItem.anchor); 0: as the game draws it, w x h
void GuiQuads(const std::vector<MsrGuiItem>& items, float x, float y, float w, float h, float opacity, bool backdrop,
              float pulse_floor = -1.f, uint32_t focus = 0, float content = 1.f, float extra = 0.f, float seconds = 0.f) {
    if (extra < .5f) extra = 0.f;
    if (extra > 0.f && seconds > 0.f) ++anchor_call;
    const float sy = h / 240.f, sx = extra > 0.f ? sy : w / 320.f;
    if (!backdrop) {
        for (const MsrGuiItem& it : items)
            if (Fader(it)) opacity *= 1.f - std::clamp(it.color[3], 0.f, 1.f);
    }
    // On the footer the game keeps its choice: drawn as its other choices are (their base and their text's color, no
    // selection lines), so only the footer's button shows chosen
    const MsrGuiItem* normal_base = nullptr;
    uint32_t chosen_base = 0, chosen_label = 0;
    float label_color[4] = {0.f, 0.f, 0.f, -1.f};
    if (focus) {
        for (const MsrGuiItem& it : items) {
            const auto b = std::find(MenuBigBases.begin(), MenuBigBases.end(), it.name);
            if (it.glyph || b == MenuBigBases.end()) continue;
            if (it.sprite == ButtonNormal && !normal_base) normal_base = &it;
            if (it.sprite == ButtonSelected) {
                chosen_base = it.name;
                chosen_label = MenuBigLabels[static_cast<std::size_t>(b - MenuBigBases.begin())];
            }
        }
        for (const MsrGuiItem& it : items) {
            if (!it.glyph || it.name == chosen_label || label_color[3] >= 0.f) continue;
            if (std::find(MenuBigLabels.begin(), MenuBigLabels.end(), it.name) != MenuBigLabels.end())
                std::copy(it.color, it.color + 4, label_color);
        }
    }
    // each piece's key for its eased place: its name and how many pieces so named came before it (a label's glyphs,
    // in a row, one piece)
    static std::unordered_map<uint32_t, uint32_t> named;   // (main thread)
    named.clear();
    uint32_t key = 0, last_label = 0;
    for (const MsrGuiItem& it : items) {
        if (!(it.glyph && it.name == last_label)) key = it.name ^ (named[it.name]++ * 0x9E3779B9u);
        last_label = it.glyph ? it.name : 0;
        if (!backdrop && Backdrop(it)) continue;
        if (focus && !it.glyph && std::find(SelectionLines.begin(), SelectionLines.end(), it.name) != SelectionLines.end())
            continue;
        MsrQuad q{};
        const bool unchosen_base = focus && normal_base && !it.glyph && it.name == chosen_base;
        const float* uv = unchosen_base ? normal_base->uv : it.uv;
        std::copy(unchosen_base ? normal_base->color : it.color, (unchosen_base ? normal_base->color : it.color) + 4, q.color);
        if (focus && it.glyph && it.name == chosen_label && label_color[3] >= 0.f) std::copy(label_color, label_color + 4, q.color);
        if (pulse_floor >= 0.f) q.color[3] = pulse_floor + (1.f - pulse_floor) * std::min(it.color[3] / .88f, 1.f);
        // the part inside its clip (the pause map's view, opened to the window's width when wider), its texture with it
        // (a turned sprite, gui_tree.cpp: whole, its texture turned in the shader; none is clipped or stretched so far)
        float x0 = it.x, y0 = it.y, x1 = it.x + it.w, y1 = it.y + it.h;
        q.turn = it.turn;
        if (it.clip[2] > it.clip[0] && !it.turn) {
            const bool open = extra > 0.f && it.anchor == MSR_ANCHOR_MAP;
            x0 = std::max(x0, open && it.clip[0] <= 2.f ? -1e9f : it.clip[0]);
            x1 = std::min(x1, open && it.clip[2] >= 318.f ? 1e9f : it.clip[2]);
            y0 = std::max(y0, it.clip[1]);
            y1 = std::min(y1, it.clip[3]);
            if (x1 <= x0 || y1 <= y0) continue;
        }
        const float du = it.w > 0.f ? (uv[2] - uv[0]) / it.w : 0.f, dv = it.h > 0.f ? (uv[3] - uv[1]) / it.h : 0.f;
        const auto u_at = [&](float tx) { return uv[0] + (tx - it.x) * du; };
        q.src[1] = y0 > it.y ? uv[1] + (y0 - it.y) * dv : uv[1];
        q.src[3] = y1 < it.y + it.h ? uv[1] + (y1 - it.y) * dv : uv[3];
        q.dst[1] = y + y0 * sy;
        q.dst[3] = (y1 - y0) * sy;
        // its pieces across: one, or (stretched on a wider screen) its two ends and its middle
        std::array<float, 4> cuts{x0, x1, x1, x1};
        int pieces = 1;
        if (it.anchor == MSR_ANCHOR_STRETCH && extra > 0.f && !it.turn) {
            const float cap = it.w * StretchCap, a = std::clamp(it.x + cap, x0, x1), b = std::clamp(it.x + it.w - cap, x0, x1);
            cuts = {x0, a, std::max(a, b), x1};
            pieces = 3;
        }
        if (!it.glyph && (it.name == AmiiboBase || it.name == AmiiboIcon)) {   // no amiibo here: darkened, and passed over
            for (int k = 0; k < 3; ++k) q.color[k] *= it.name == AmiiboIcon ? .45f : .55f;
        }
        uint32_t base_of_focus = focus == CopyIcon ? CopyBase : focus == EraseIcon ? EraseBase : focus == DeleteAllIcon ? DeleteAllBase : 0;
        for (const auto& button : PauseToolbar)
            if (focus == button.icon) base_of_focus = button.base;
        const bool base = focus && base_of_focus && it.name == base_of_focus;   // (a pin chosen has none)
        const float place = extra > 0.f ? AnchorPlace(it, key, seconds) : -1.f;
        const float mid_x = ItemX(it, it.x + it.w / 2, x, sx, extra, place);
        if (focus && it.name == focus && std::find(PinMarkers.begin(), PinMarkers.end(), focus) != PinMarkers.end()) {
            // a chosen pin: its cell between the separators lit behind it (the pins are small)
            const float cx = mid_x, cy = y + (it.y + it.h / 2) * sy;
            Fill(cx - 15.f * sx, cy - 11.f * sy, 30.f * sx, 22.f * sy, .35f, .75f, .95f, .38f * opacity, 30);
            Fill(cx - 15.f * sx, cy + 10.f * sy, 30.f * sx, std::max(1.f, sy), .55f, .9f, 1.f, .9f * opacity);
        } else if (focus && it.name == focus && PlacedPin(it)) {
            // a pin chosen on the map: a lit square round it, the pin in its own color on it
            const float cx = mid_x, cy = y + (it.y + it.h / 2) * sy;
            const float r = 9.f * sx, rv = 9.f * sy, line = std::max(1.f, sy);
            Fill(cx - r, cy - rv, 2 * r, 2 * rv, .35f, .75f, .95f, .4f * opacity, 31);
            Fill(cx - r, cy - rv, 2 * r, line, .55f, .9f, 1.f, .9f * opacity);
            Fill(cx - r, cy + rv - line, 2 * r, line, .55f, .9f, 1.f, .9f * opacity);
            Fill(cx - r, cy - rv, line, 2 * rv, .55f, .9f, 1.f, .9f * opacity);
            Fill(cx + r - line, cy - rv, line, 2 * rv, .55f, .9f, 1.f, .9f * opacity);
        } else if (focus && it.name == focus) {
            q.color[0] = q.color[1] = q.color[2] = 1.5f;   // chosen: its icon brought to white
        }
        q.style[0] = Backdrop(it) ? opacity : opacity * content;   // (content: a pause page's crossfade)
        q.style[1] = 1.f;   // mode 4's gain
        q.style[3] = it.additive ? 4.f : 5.f;
        q.source = 3;
        q.atlas = it.atlas;
        for (int k = 0; k < pieces; ++k) {
            const float a = cuts[static_cast<std::size_t>(k)], b = cuts[static_cast<std::size_t>(k + 1)];
            if (b <= a) continue;
            MsrQuad piece = q;
            const float from = ItemX(it, a, x, sx, extra, place), to = pieces > 1 ? ItemX(it, b, x, sx, extra, place) : from + (b - a) * sx;
            piece.dst[0] = from;
            piece.dst[2] = to - from;
            piece.src[0] = u_at(a);
            piece.src[2] = u_at(b);
            quads.push_back(piece);
            if (base) {
                // ...and its base lit: itself again, added in the blue of the game's chosen buttons (Button_Selected's)
                piece.color[0] = .35f; piece.color[1] = .7f; piece.color[2] = 1.4f; piece.color[3] = 1.f;
                piece.style[3] = 4.f;
                quads.push_back(piece);
            }
        }
    }
}

// The anchor of the piece at a touch-screen point (the topmost, not a backdrop nor a stretched bar or background): the
// middle where there is none
int AnchorAt(const std::vector<MsrGuiItem>& items, float tx, float ty) {
    for (auto it = items.rbegin(); it != items.rend(); ++it) {
        if (it->anchor == MSR_ANCHOR_STRETCH || Backdrop(*it)) continue;
        if (tx >= it->x && tx < it->x + it->w && ty >= it->y && ty < it->y + it->h) return it->anchor;
    }
    return MSR_ANCHOR_MIDDLE;
}

// The rebuilt touch screen this frame, and the file select's as last seen live (its fade, as video.c's copy); the
// HUD's minimap this frame
std::vector<MsrGuiItem> gui_now, gui_last, gui_minimap;
// PWR_GUI_COPY=1: the touch screen's copy drawn instead, to compare the two (the footer's D-pad still works)
bool GuiCopy() {
    static const bool copy = std::getenv("PWR_GUI_COPY") != nullptr;
    return copy;
}

// The Aeion abilities' page over the view (2026-11-18, the user: after gaining an Aeion ability "it pulls up the bottom
// screen ... draw from the code whats happening. take nothing from the bottom screen. display whatever text appears
// over samus as normal and then make room for this cross to appear"; the four buttons "above the text box with a light
// background behind each box like we do for the metroid symbols"). The touch screen's page (AbilitiesCore, gui_tree.cpp
// msr_gui_abilities) is not shown as the panel: the view stays, dimmed by the game itself, and over it, from the game's
// interface, its title, the four slots on the D-pad as the game lays them out (Scan Pulse up, Lightning Armor left, Beam
// Burst right, Phase Drift down; empty until found, the new one lit), each on a light plate, and below them the
// description (LabelInfoBig: the page the game shows, typed out as it types it, gui_tree.cpp Label) in the game's own
// message box for the top screen (USMComposition: the box "- Lightning Armor -" came in), with its A while a page waits
// for one to turn to the next and its B in the same place on the last page, which closes on B (Back), not A. The game
// slides the page in and out on the touch
// screen; that is not followed here (2026-11-18, the user: "the text starts shaking before it goes away": the label
// moved 2 pixels a frame as it went, read again a moment after its glyphs). The page is drawn as it comes in and
// stands; once its description goes down again (the page going), the overlay fades out on it as it stood.
constexpr std::array<uint32_t, 4> AbilityBases = {0xB4B207DAu /* ScanningPulseBase */, 0x12A95560u /* EnergyShieldBase */,
                                                  0xD98676CBu /* EnergyWaveBase */, 0x343BCA46u /* PhaseDisplacementBase */};
constexpr std::array<uint32_t, 4> AbilityIcons = {0x1192D060u /* ScanningPulseIcon */, 0xB78982DAu /* EnergyShieldIcon */,
                                                  0x7CA6A171u /* EnergyWaveIcon */, 0x911B1DFCu /* PhaseDisplacementIcon */};
constexpr uint32_t AbilityInfo = 0xE61669C8u /* LabelInfoBig */, AbilityTitle = 0x01D973EEu /* LabelTitleAlt */;
// USMComposition's box (TL T TR, L M R, BL B BR), its A (USMABase, USMAIconA, USMAIconB) and its B's icons (USMBIconA,
// USMBIconB). The B shows in the A's place, on its base, on the last page (2026-11-18, the user: "at the end of the text
// box, add the (B) back but put it on the (A) side so users know to hit (B) there"; the game's B corner, the A's base
// unturned at the left, "blurs into the background image there")
constexpr std::array<uint32_t, 14> MessageBox = {0xC969636Du, 0x6543EB3Fu, 0x33665E0Eu, 0x762F7369u, 0x012843FFu,
                                                 0x8C204E0Au, 0xD5F1D6BAu, 0x91975E6Eu, 0x2FFEEBD9u,
                                                 0x02B1C816u, 0xDE1C0FDAu, 0x47155E60u,
                                                 0x58887D74u, 0xC1812CCEu};
bool AbilityPiece(const MsrGuiItem& it) {
    if (it.glyph) return it.name == AbilityInfo || it.name == AbilityTitle;
    return std::find(AbilityBases.begin(), AbilityBases.end(), it.name) != AbilityBases.end() ||
           std::find(AbilityIcons.begin(), AbilityIcons.end(), it.name) != AbilityIcons.end();
}
// The page as one frame of the game's interface holds it: its pieces, the description's box (the label's rectangle on
// the touch screen) and the button its box shows
enum { PromptNone, PromptA, PromptB };
struct AbilitiesPage {
    std::vector<MsrGuiItem> items;
    Rect room{77.f, 208.f, 304.f, 244.f};
    int prompt = PromptNone;
};
// ...this frame's (msr_gui_tree's walk just made, and the label read now): A once the page is typed out and another
// follows, B once the last is, as the game's label counts them
AbilitiesPage AbilitiesNow(const std::vector<MsrGuiItem>& tree) {
    AbilitiesPage page;
    for (const MsrGuiItem& it : tree)
        if (AbilityPiece(it)) page.items.push_back(it);
    MsrGuiItem label{};
    if (msr_gui_named(&AbilityInfo, 1, &label) == 1 && label.w > 0.f && label.h > 0.f)
        page.room = {label.x, label.y, label.x + label.w, label.y + label.h};
    const int typed_out = msr_gui_label_waits(AbilityInfo);
    page.prompt = typed_out == 2 ? PromptA : typed_out == 1 ? PromptB : PromptNone;
    return page;
}
AbilitiesPage abilities_page;   // as drawn (as it came in and stood, until it goes)
void AbilitiesOverlay(const Rect& view, const AbilitiesPage& page, float alpha) {
    std::vector<MsrGuiItem> cross, title, info;
    for (const MsrGuiItem& it : page.items) {
        if (it.glyph) (it.name == AbilityInfo ? info : title).push_back(it);
        else cross.push_back(it);
    }
    if (cross.empty()) return;
    const Rect& room = page.room;
    const auto bounds = [](const std::vector<MsrGuiItem>& v) {
        Rect r{1e9f, 1e9f, -1e9f, -1e9f};
        for (const MsrGuiItem& it : v) r = {std::min(r.l, it.x), std::min(r.t, it.y), std::max(r.r, it.x + it.w), std::max(r.b, it.y + it.h)};
        return r;
    };
    // touch-screen pixels drawn at k window pixels each, (tx, ty) of them at (x, y): GuiQuads' rectangle
    const auto place = [](const std::vector<MsrGuiItem>& v, float tx, float ty, float x, float y, float k, float a) {
        GuiQuads(v, x - tx * k, y - ty * k, 320.f * k, 240.f * k, a, true);
    };
    const float u = view.H() / 240.f;   // the top screen's pixels
    const float k_title = .9f * u, k_cross = .6f * u, k_text = u;
    std::array<MsrGuiItem, MessageBox.size()> box{};
    const bool framed = msr_gui_named(MessageBox.data(), static_cast<int>(MessageBox.size()), box.data()) ==
                        static_cast<int>(MessageBox.size());
    // the box round it: the room for the game's corners (26 x 26 of the top screen's pixels: its interface lays the
    // top screen's 400 out across 320, the corner 20.8 wide), the bottom row for its A
    constexpr float top_x = 400.f / 320.f;
    const float corner_w = (framed ? box[0].w : 20.8f) * top_x * u, corner_h = (framed ? box[0].h : 26.f) * u;
    const float pad_x = corner_w, pad_top = 13.f * u, pad_bottom = corner_h;
    const float box_w = room.W() * k_text + 2.f * pad_x, box_h = room.H() * k_text + pad_top + pad_bottom;
    const Rect cb = bounds(cross), tb = bounds(title);
    const float title_h = title.empty() ? 0.f : tb.H() * k_title + 6.f * u;
    const float total = title_h + cb.H() * k_cross + 10.f * u + box_h;
    const float cx = view.l + view.W() / 2;
    float y = view.t + std::max(4.f * u, (view.H() - total) / 2);
    if (!title.empty()) {
        place(title, tb.l + tb.W() / 2, tb.t, cx, y, k_title, alpha);
        y += title_h;
    }
    // the slots, each on a light plate as the counters are (in the touch screen's blue, the view being dimmed behind it),
    // a little past its frame (the frames meet at their corners, the plates' soft edges there)
    const float cross_x = cx - (cb.l + cb.W() / 2) * k_cross, cross_y = y - cb.t * k_cross;
    for (const MsrGuiItem& it : cross) {
        if (std::find(AbilityBases.begin(), AbilityBases.end(), it.name) == AbilityBases.end()) continue;
        const float grow = 3.f * k_cross;
        Fill(cross_x + it.x * k_cross - grow, cross_y + it.y * k_cross - grow, it.w * k_cross + 2 * grow,
             it.h * k_cross + 2 * grow, .34f, .57f, .84f, .26f * alpha, 30);
    }
    GuiQuads(cross, cross_x, cross_y, 320.f * k_cross, 240.f * k_cross, alpha, true);
    y += cb.H() * k_cross + 10.f * u;
    // the message box: its corners as the game draws them, its edges and middle stretched between
    const float bx = cx - box_w / 2, by = y;
    if (framed) {
        const float xs[4] = {bx, bx + corner_w, bx + box_w - corner_w, bx + box_w};
        const float ys[4] = {by, by + corner_h, by + box_h - corner_h, by + box_h};
        for (int row = 0; row < 3; ++row)
            for (int col = 0; col < 3; ++col) {
                const MsrGuiItem& piece = box[static_cast<std::size_t>(row * 3 + col)];
                if (piece.atlas < 0 || xs[col + 1] <= xs[col] || ys[row + 1] <= ys[row]) continue;
                MsrQuad q{};
                q.dst[0] = xs[col]; q.dst[1] = ys[row]; q.dst[2] = xs[col + 1] - xs[col]; q.dst[3] = ys[row + 1] - ys[row];
                std::copy(piece.uv, piece.uv + 4, q.src);
                q.color[0] = q.color[1] = q.color[2] = q.color[3] = 1.f;   // (its own alpha: 0 while the game hides it)
                q.style[0] = alpha; q.style[1] = 1.f; q.style[3] = piece.additive ? 4.f : 5.f;
                q.source = 3;
                q.atlas = piece.atlas;
                quads.push_back(q);
            }
    } else {
        Fill(bx, by, box_w, box_h, .2f, .45f, .65f, .85f * alpha, 6);
    }
    // the page, in the box's text color (USMLM's)
    for (MsrGuiItem& it : info) {
        it.color[0] = .8f; it.color[1] = .9f; it.color[2] = 1.f; it.color[3] = 1.f;
    }
    place(info, room.l, room.t, bx + pad_x, by + pad_top, k_text, alpha);
    // its A at the bottom right while another page follows, where the game's box has it (from its own box's right and
    // bottom); on the last page its B there instead: the B's mark (USMBIconA, the A's ✓ there) and button (USMBIconB) in
    // the A's places, in the A's colors (the B's are black while the game hides them)
    if (!framed || page.prompt == PromptNone) return;
    const float right = box[2].x + box[2].w, bottom = box[8].y + box[8].h;
    for (std::size_t i = 9; i < 12; ++i) {
        const MsrGuiItem& at = box[i];
        const MsrGuiItem& piece = page.prompt == PromptB && i > 9 ? box[i + 2] : at;
        if (piece.atlas < 0) continue;
        MsrQuad q{};
        q.dst[0] = bx + box_w + (at.x - right) * top_x * u; q.dst[1] = by + box_h + (at.y - bottom) * u;
        q.dst[2] = at.w * top_x * u; q.dst[3] = at.h * u;
        std::copy(piece.uv, piece.uv + 4, q.src);
        std::copy(at.color, at.color + 3, q.color);
        q.color[3] = 1.f;   // (its own alpha: 0 while the game hides it)
        q.style[0] = alpha; q.style[1] = 1.f; q.style[3] = piece.additive ? 4.f : 5.f;
        q.source = 3;
        q.atlas = piece.atlas;
        quads.push_back(q);
    }
}

// The file select's footer on the D-pad (2026-11-17, the user: "allow the player to use the dpad or joystick to select
// copy or delete file as well normally. and when they are on those they become highlighted"; then: "make it work
// sequentially in line with the other options where it scrolls down to the copy file and then back up to the first
// option. if the user wants to delete the file they'll go down to copy file and from there go over to the right").
// The game makes Copy and Delete touch buttons only; they work once a file is open (Continue, New Game, Options), where
// the game lights them. There the D-pad (and stick) walk one list: the choices, then Copy, then round to the first
// choice again (Up the other way); Right from Copy is Delete, Left back; A taps the footer's button. msr_footer_input
// (the input thread) keeps Up and Down from the game and moves its choice itself, by presses of its own.
// From the interface each frame (main thread): whether the footer is lit, where its buttons are, how many choices there
// are and which the game has (the lit ButtonMenuBig bases top to bottom: Button_Selected the chosen, the others
// Button_Normal).
std::atomic<bool> footer_ready{false};
std::atomic<int> footer_choices{0}, footer_game_choice{-1};
std::atomic<int> footer_copy_x{0}, footer_copy_y{0}, footer_erase_x{0}, footer_erase_y{0};

void FooterFromTree() {
    bool copy_lit = false, erase_lit = false;
    std::array<float, 4> ys{};
    int count = 0;
    float chosen = -1.f;
    for (const auto& it : gui_now) {
        if (it.glyph) continue;
        if (it.name == CopyIcon) copy_lit = it.color[0] > .5f && it.color[3] > .5f;
        if (it.name == EraseIcon) erase_lit = it.color[0] > .5f && it.color[3] > .5f;
        if (it.name == CopyBase) { footer_copy_x = static_cast<int>(it.x + it.w / 2); footer_copy_y = static_cast<int>(it.y + it.h / 2); }
        if (it.name == EraseBase) { footer_erase_x = static_cast<int>(it.x + it.w / 2); footer_erase_y = static_cast<int>(it.y + it.h / 2); }
        if ((it.sprite == ButtonSelected || it.sprite == ButtonNormal) && count < 4 &&
            std::find(MenuBigBases.begin(), MenuBigBases.end(), it.name) != MenuBigBases.end()) {
            ys[count++] = it.y;
            if (it.sprite == ButtonSelected) chosen = it.y;
        }
    }
    std::sort(ys.begin(), ys.begin() + count);
    int game = -1;
    for (int k = 0; k < count; ++k)
        if (ys[k] == chosen) game = k;
    const bool open = static_cast<P::Page>(P::page.load(std::memory_order_relaxed)) == P::Page::FileOpen;
    footer_ready = open && copy_lit && erase_lit && count > 0 && game >= 0 &&
                   P::panel_visible.load(std::memory_order_relaxed) && footer_copy_x.load() > 0 && footer_erase_x.load() > 0;
    footer_choices = count;
    footer_game_choice = game;
}

// The pause menu's touch buttons on the D-pad (2026-11-17, the user: "the bottom touch screen elements can be
// navigated with the dpad so the dpad no longer move the map around. the amiibo tab is permanently darkened but still
// appears, so it will also be skipped over"). On the map page the D-pad goes along the toolbar's buttons (the world
// map, map pins, information, settings; amiibo passed over) instead of moving the map, which the stick still moves; A
// taps the one chosen. From the interface each frame (main thread): the page and its buttons where they are lit
// (their icons' names, their bases' middles), and which is the page's own. pause_input (the input thread) does the rest.
struct PauseButton { uint32_t icon; int x, y; };
struct PauseState {
    int page = 0;   // 0 none, 1 the map page, 2 the map pins tool, 3 the world map (the game's own controls)
    int count = 0, current = 0;
    std::array<PauseButton, 8> buttons{};
    // the map pins tool: the pins placed on the map where a finger can take them (left to right; their names), and its
    // Delete all button (icon 0: none)
    int placed_count = 0;
    std::array<PauseButton, 16> placed{};
    PauseButton delete_all{};
    // the map's view (x0, y0, x1, y1), where a finger moving a pin stays
    bool view_known = false;
    std::array<float, 4> view{};
};
std::mutex pause_lock;
PauseState pause_shared;
// A Teleport Station's area map shows, its bar's D-pad buttons choosing the station (msr_station_map)
constexpr uint32_t StationLeft = 0x63B1A204u /* ButtonSelectLeftBase */, StationRight = 0x44C42AABu /* ...RightBase */;
std::atomic<bool> station_map{false};

void PauseFromTree() {
    PauseState next;
    const auto page = static_cast<P::Page>(P::page.load(std::memory_order_relaxed));
    // The world map (2026-11-17, the user: "when I look at the first area on the map, it doesn't show up"): its pixels
    // read as the map pins page, and the pins' D-pad took the game's own from it (and lit a pin of its own). Its
    // controls are the game's; the map's tools come back with the map.
    const bool world_map = !gui_now.empty() && msr_gui_world_map() > .5f;   // (the tree read this frame)
    P::world_map.store(world_map, std::memory_order_relaxed);
    // A map the game opened over play, without Start (a Teleport Station's, an elevator's): its controls are the game's
    // too. A Teleport Station's area map, where Left and Right choose the station, read as the map pins tool, and the
    // pins' D-pad kept Left and Right from the game (#69, #83: the stations could only be clicked).
    const bool own_map = P::menu_unasked.load(std::memory_order_relaxed);
    // ...and on that area map, the station is chosen only with the D-pad, the circle pad moving the map (input.c lets
    // the keys that move the circle pad choose there too)
    bool station = false;
    if (own_map && !world_map)
        for (const auto& it : gui_now)
            if (!it.glyph && (it.name == StationLeft || it.name == StationRight) && it.color[3] > .05f) { station = true; break; }
    station_map.store(station, std::memory_order_relaxed);
    if (world_map) {
        next.page = 3;
    } else if (own_map) {
        next.page = 0;
    } else if (page == P::Page::PauseMap && P::panel_visible.load(std::memory_order_relaxed)) {
        for (const auto& button : PauseToolbar) {
            for (const auto& it : gui_now) {
                if (it.glyph || it.name != button.base) continue;
                if (next.count < static_cast<int>(next.buttons.size()))
                    next.buttons[next.count++] = {button.icon, static_cast<int>(it.x + it.w / 2), static_cast<int>(it.y + it.h / 2)};
                if (it.sprite == 0xCF199064u /* ButtonA_Selected_Alt */ || it.sprite == 0x6DF23C9Cu /* ButtonB_Selected */)
                    next.current = next.count - 1;
                break;
            }
        }
        if (next.count > 0) next.page = 1;
    } else if (page == P::Page::MapPins && P::panel_visible.load(std::memory_order_relaxed)) {
        for (const uint32_t marker : PinMarkers) {
            for (const auto& it : gui_now) {
                if (it.glyph || it.name != marker || next.count >= static_cast<int>(next.buttons.size())) continue;
                next.buttons[next.count++] = {marker, static_cast<int>(it.x + it.w / 2), static_cast<int>(it.y + it.h / 2)};
                break;
            }
        }
        for (const auto& it : gui_now) {
            if (it.glyph) continue;
            if (it.name == DeleteAllBase)
                next.delete_all = {DeleteAllIcon, static_cast<int>(it.x + it.w / 2), static_cast<int>(it.y + it.h / 2)};
            if (!PlacedPin(it) || it.color[3] < .5f || next.placed_count >= static_cast<int>(next.placed.size())) continue;
            const float px = it.x + it.w / 2, py = it.y + it.h / 2;
            if (px < std::max(it.clip[0], 0.f) + 3.f || px > std::min(it.clip[2], 320.f) - 3.f ||
                py < std::max(it.clip[1], 0.f) + 3.f || py > std::min(it.clip[3], 208.f) - 3.f)
                continue;   // out of the view (or under the bar): not to be taken from here
            next.placed[next.placed_count++] = {it.name, static_cast<int>(px), static_cast<int>(py)};
        }
        std::sort(next.placed.begin(), next.placed.begin() + next.placed_count,
                  [](const PauseButton& a, const PauseButton& b) { return a.x != b.x ? a.x < b.x : a.y < b.y; });
        if (next.count > 0 || next.placed_count > 0) next.page = 2;
    }
    next.view_known = !gui_now.empty() && msr_gui_map_view(next.view.data());
    std::lock_guard<std::mutex> guard(pause_lock);
    pause_shared = next;
}

// The pause map's controller buttons shown on it (2026-11-17, the user, of the help text beside the pause screen: "we
// no longer need but I do want them to be re-worked ... add a Dpad left and dpad right icon to the bottom of the screen
// to the left of the little map and to the right of the little gear to indicate which buttons control that. on the
// pin page you can keep the icon for the dpad left and dpad right. When highlighting a single pin make a garbage can
// icon by itself sits to the left of an (X) in a box above the global trash icon on the bottom right. This would look
// like how the checkmark appears left of the (A) but in it's own box"): the game's own D-pad icons (D-LEFT, D-RIGHT)
// beside the toolbar's ends and the pins' bar's, and, a pin on the map chosen, the box the game's popups show "✔ Ⓐ" in
// (ButtonUpWindow) above the Delete all corner, holding the file select's bin (Icon_Erase) and the remove button's icon
// (the 3DS Y's, which the player's device's icons draw as that pad names the button). Added to the interface's pieces
// each frame, beside those they belong to and kept where the layout puts them (their anchors).
void PauseExtras() {
    PauseState state;
    {
        std::lock_guard<std::mutex> guard(pause_lock);
        state = pause_shared;
    }
    if (state.page != 1 && state.page != 2) return;
    const auto find = [](uint32_t name) -> const MsrGuiItem* {
        for (const MsrGuiItem& it : gui_now)
            if (!it.glyph && it.name == name && it.color[3] > .05f) return &it;
        return nullptr;
    };
    std::vector<MsrGuiItem> extra;
    const auto piece = [&extra](int atlas, const float uv[4], float x, float y, float w, float h, int anchor, bool additive,
                                float alpha) {
        MsrGuiItem it{};
        it.atlas = atlas;
        std::copy(uv, uv + 4, it.uv);
        it.x = x; it.y = y; it.w = w; it.h = h;
        it.color[0] = it.color[1] = it.color[2] = additive ? .85f : 1.f;   // (icons a little softer, as the game's)
        it.color[3] = alpha;
        it.additive = additive;
        it.anchor = anchor;
        it.clip[0] = it.clip[1] = -1e9f;
        it.clip[2] = it.clip[3] = 1e9f;
        extra.push_back(it);
    };
    // an icon embedded in the game's texts (@[NAME]), h pixels tall, its middle at cy, its left at x (left_edge) or right
    const auto icon = [&piece](const char* name, float x, float cy, float h, int anchor, float alpha, bool left_edge = true) {
        int atlas = -1;
        float uv[4], w = 0.f, ih = 0.f;
        if (!msr_gui_embedded(name, std::strlen(name), &atlas, uv, &w, &ih) || ih <= 0.f) return;
        const float iw = w * h / ih;
        piece(atlas, uv, left_edge ? x : x - iw, cy - h / 2, iw, h, anchor, true, alpha);
    };
    // The pause screen fitted to a wider window (last frame's extra width; while it was not shown, the setting and the
    // window's shape): the icons in the bars, beside their ends. As the 3DS draws it, they would sit on the corners'
    // buttons there: above the bar instead (2026-11-17, the user: "on the nonwidescreen version of the pause menu, move
    // the dpad icons above the bottom bar").
    const bool wide = st.touch_extra > 0.f
                          ? st.touch_extra >= 30.f
                          : P::fit_pause.load(std::memory_order_relaxed) && st.composed_h > 0.f &&
                                st.composed_w / st.composed_h * 240.f - 320.f >= 30.f;
    if (state.page == 1) {   // the toolbar: its first and last buttons' bases
        const MsrGuiItem *first = nullptr, *last = nullptr;
        const auto consider = [&](uint32_t base) {
            const MsrGuiItem* it = find(base);
            if (!it) return;
            if (!first || it->x < first->x) first = it;
            if (!last || it->x + it->w > last->x + last->w) last = it;
        };
        for (const auto& button : PauseToolbar) consider(button.base);
        consider(AmiiboBase);
        if (first && last) {
            // (above the bar: 2 pixels over its buttons' tops)
            const float cy = wide ? first->y + first->h / 2 : first->y - 9.5f, alpha = std::min(first->color[3], 1.f);
            icon("D-LEFT", first->x - 2.f, cy, 15.f, first->anchor, alpha, false);
            icon("D-RIGHT", last->x + last->w + 2.f, cy, 15.f, last->anchor, alpha);
        }
    } else {   // the pins' bar: its first pin, and its last with the count after it
        const MsrGuiItem* first = find(PinMarkers.front());
        const MsrGuiItem* last = find(PinMarkers.back());
        if (first && last) {
            const float cy = first->y + first->h / 2, alpha = std::min(first->color[3], 1.f);
            float right = last->x + last->w;
            const MsrGuiItem* corner = find(DeleteAllBase);
            const float limit = corner ? corner->x : 300.f;
            for (const MsrGuiItem& it : gui_now)   // (the count, "x10": text in the bar's row before the corner)
                if (it.glyph && it.x > right - 1.f && it.x + it.w < limit && it.y < cy && it.y + it.h > cy)
                    right = std::max(right, it.x + it.w);
            // (the right one where it is with "x10", the widest count, whatever the count: past the last pin by 48;
            // above the bar, 2 pixels over its top, 15 above the pins' middles, and short of the remove button's box)
            float right_x = std::max(right + 6.f, last->x + last->w + 48.f);
            if (!wide) right_x = std::min(right_x, (corner ? corner->x + corner->w - 61.f : 259.f) - 17.f);
            const float icon_cy = wide ? cy : cy - 15.f - 9.5f;
            icon("D-LEFT", first->x - (wide ? 10.f : 6.f), icon_cy, 15.f, first->anchor, alpha, false);
            icon("D-RIGHT", right_x, icon_cy, 15.f, last->anchor, alpha);
        }
        // a pin on the map chosen: its remove button, in a box of its own above the Delete all corner
        const uint32_t focus = P::pause_focus.load(std::memory_order_relaxed);
        const MsrGuiItem* corner = find(DeleteAllBase);
        bool on_map_pin = false;
        for (const MsrGuiItem& it : gui_now)
            if (focus && it.name == focus && PlacedPin(it)) on_map_pin = true;
        int atlas = -1;
        float uv[4];
        if (on_map_pin && corner && msr_gui_sprite(MSR_SPRITE_UP_WINDOW, &atlas, uv)) {
            const float bw = 61.f, bh = 25.f, bx = corner->x + corner->w - bw, by = corner->y - bh - 1.f;
            const float alpha = std::min(corner->color[3], 1.f);
            piece(atlas, uv, bx, by, bw, bh, corner->anchor, false, alpha);
            if (msr_gui_sprite(MSR_SPRITE_ERASE, &atlas, uv)) piece(atlas, uv, bx + 21.f, by + 5.f, 14.f, 15.4f, corner->anchor, true, alpha);
            icon("Y", bx + 40.f, by + 13.f, 15.f, corner->anchor, alpha);
        }
    }
    gui_now.insert(gui_now.end(), extra.begin(), extra.end());
}

// The pause's Controls page with the buttons play is bound to (2026-11-17, the user: "the controls on the pause menu
// should reflect the controls weve mapped to the game"). Its icons (ControlsComposition: the sheet items A, B, X, Y,
// L, R, D beside Use Aeion, Jump, Melee Counter, Shoot, Free Aim, Missile Mode, Select Aeion) are the game's face and
// shoulder icons, which in menus show the menus' layout (A chooses: the check mark's A), so Jump (B) read B though
// play has it on the pad's A. Each shows play's button instead: the face icon that shows it now
// (msr_glyph_play_face), the L or R icon for a shoulder, the game's D-pad icons for a direction, else its name in the
// game's font ("LT", and "RS" for Select Aeion with the Aeion abilities on the right stick). The keyboard's icons
// already show the keys bound in play.
void ControlsIcons() {
    constexpr uint32_t Group = 0xB1E17D10u;   // ControlsComposition
    struct Row { uint32_t sprite; int control; };
    static constexpr Row Rows[] = {{0x2C266174u, C_A}, {0xB52F30CEu, C_B}, {0x484DC9B4u, C_X}, {0x3F4AF922u, C_Y},
                                   {0x52971DC9u, C_L}, {0xA89820AAu, C_R}, {0x5C4C95FBu, C_UP /* D */}};
    static const char* const FaceIcons[4] = {"A", "B", "X", "Y"};
    static const char* const DpadIcons[4] = {"D-UP", "D-DOWN", "D-LEFT", "D-RIGHT"};
    constexpr int PadL1 = 9, PadR1 = 10, PadUp = 11, PadRight = 14;   // (SDL_CONTROLLER_BUTTON_*)
    std::vector<MsrGuiItem> text;
    // an embedded icon in the place of `it` (as tall, its middle where it was)
    const auto swap_icon = [](MsrGuiItem& it, const char* name) {
        int atlas = -1;
        float uv[4], w = 0.f, h = 0.f;
        if (!msr_gui_embedded(name, std::strlen(name), &atlas, uv, &w, &h) || h <= 0.f) return false;
        const float nw = w * it.h / h, mid = it.x + it.w / 2;
        it.atlas = atlas;
        std::copy(uv, uv + 4, it.uv);
        it.x = mid - nw / 2;
        it.w = nw;
        return true;
    };
    // a short name in the place of `it`: the game's digital_medium glyphs, at most 24 pixels wide, its middle there
    const auto name_icon = [&text](MsrGuiItem& it, const char* label) {
        float size = 0.f, ascent = 0.f;
        if (!label || !*label || !msr_gui_font(MSR_FONT_MEDIUM, &size, &ascent)) return;
        std::vector<MsrGlyph> glyphs;
        float width = 0.f;
        for (const char* c = label; *c; ++c) {
            MsrGlyph g;
            if (!msr_gui_glyph(MSR_FONT_MEDIUM, static_cast<unsigned char>(*c), &g)) continue;
            glyphs.push_back(g);
            width += g.advance;
        }
        if (width <= 0.f) return;
        const float s = std::min(it.h / size, 24.f / width);
        float pen = it.x + it.w / 2 - width * s / 2;
        const float baseline = it.y + it.h / 2 + ascent * s / 2;
        for (const MsrGlyph& g : glyphs) {
            MsrGuiItem t = it;
            t.atlas = MSR_ATLAS_FONT;
            std::copy(g.uv, g.uv + 4, t.uv);
            t.x = pen + g.left * s; t.y = baseline - g.top * s; t.w = g.w * s; t.h = g.h * s;
            t.glyph = 1;
            t.additive = 0;
            t.sprite = 0;
            t.color[0] = .8f; t.color[1] = .9f; t.color[2] = 1.f;
            text.push_back(t);
            pen += g.advance * s;
        }
        it.color[3] = 0.f;   // (the icon it stands for, not drawn)
    };
    for (MsrGuiItem& it : gui_now) {
        // the page's icons, and in play the face and shoulder icons a text embeds (the Samus page's descriptions:
        // "Press @[Y] rapidly to fire"; the title's "Press @[A_ALT] to Start" is a menu's, and untagged)
        if (it.glyph ? !(it.sprite && st.in_play) : it.group != Group) continue;
        const auto row = std::find_if(std::begin(Rows), std::end(Rows), [&it](const Row& r) { return r.sprite == it.sprite; });
        if (row == std::end(Rows)) continue;
        int b = -1;
        const int face = msr_glyph_play_face(row->control, &b);
        if (face == -2) continue;   // (the icons follow play's bindings already)
        if (row->control == C_UP) {   // Select Aeion: the D-pad, or the right stick with the Aeion abilities there
            if (input_right_stick_dpad()) name_icon(it, "RS");
            continue;
        }
        if (face >= 0) swap_icon(it, FaceIcons[face]);
        else if (b == PadL1) swap_icon(it, "L");
        else if (b == PadR1) swap_icon(it, "R");
        else if (b >= PadUp && b <= PadRight) swap_icon(it, DpadIcons[b - PadUp]);
        else if (b >= 0) name_icon(it, msr_pad_label(b));
    }
    gui_now.insert(gui_now.end(), text.begin(), text.end());
}

// ConsumeMsrReadbacks: classify the bottom screen, track the beam slots, the map marker and the prompt's text, and
// publish the state once it has held.
void Consume(const BC::Image& image, s64 now) {
    const auto result = BC::Classify(image);
    const int state = static_cast<int>(result.bottom);
    st.bottom_peak = static_cast<float>(BC::Peak(image)) / 255.f;
    st.bottom_black = BC::DarkFraction(image, 6) > .9f;
    st.menu_live = result.bottom == P::Bottom::Menu;
    if (result.page == P::Page::FileList || result.page == P::Page::FileOpen) {
        P::beam_slot.store(-1, std::memory_order_relaxed);
        P::beam_slots.store(0, std::memory_order_relaxed);
        st.beam_seen_us.fill(-1);
        st.beam_empty_us.fill(-1);
    }
    if (result.bottom == P::Bottom::Hud) {
        const auto held = [now](s64& since) {
            if (since < 0 || since > now) since = now;
            return now - since;
        };
        if (!st.beams_held) {   // (R held: the slots show the missiles; the beams are kept as they were)
            int owned = P::beam_slots.load(std::memory_order_relaxed);
            for (std::size_t i = 0; i < st.beam_seen_us.size(); ++i) {
                const int bit = 1 << i;
                if (result.beams.present & bit) {
                    st.beam_empty_us[i] = -1;
                    if (held(st.beam_seen_us[i]) >= 100'000) owned |= bit;
                } else {
                    st.beam_seen_us[i] = -1;
                    if (result.beams.grayed) st.beam_empty_us[i] = -1;
                    else if (held(st.beam_empty_us[i]) >= 1'000'000) owned &= ~bit;
                }
            }
            // Only the beams Samus has (2026-11-14, the user: the Grapple Beam's slot "shouldn't appear at all unless
            // samus already has the beam"): the game draws the slot of one not found yet as a dim striped circle, which
            // reads as present. Her inventory says (cheats.cpp msr_item); where it cannot tell, the picture as before.
            const float ice = msr_item(0x708718, "ITEM_WEAPON_ICE_BEAM"), grapple = msr_item(0x708714, "ITEM_WEAPON_GRAPPLE_BEAM");
            const int unowned = (ice == 0.f ? 2 : 0) | (grapple == 0.f ? 4 : 0);
            static int told;   // (the log, once a slot: a beam's slot the picture took for one Samus has)
            if (owned & unowned & ~told) {
                std::fprintf(stderr, "msr: HUD: the %s slot left out (not found yet)\n", owned & unowned & 4 ? "Grapple Beam's" : "Ice Beam's");
                told |= owned & unowned;
            }
            owned &= ~unowned;
            // Where her inventory tells, it alone says which slots show: the picture's empty slots were the game's
            // redrawing (Beam Burst's frame over two of them; in Morph Ball the Ice Beam's faded out, 2026-11-16)
            if (ice >= 0.f && grapple >= 0.f) owned = 1 | (ice > 0.f ? 2 : 0) | (grapple > 0.f ? 4 : 0);
            P::beam_slots.store(owned, std::memory_order_relaxed);
            // The lit slot and Morph Ball as the game holds them (cheats.cpp msr_samus_beam); where it cannot tell, the
            // picture: the lit slot, grayed too (Morph Ball: the beam can still be changed), unclear: as it was
            int morph = -1;
            const int beam = msr_samus_beam(&morph);
            static const bool trace = std::getenv("PWR_INPUT_TRACE") != nullptr;
            static int traced_beam = -2, traced_morph = -2;
            if (trace && (beam != traced_beam || morph != traced_morph)) {
                std::fprintf(stderr, "msr: HUD: beam %d, Morph Ball %d (the picture: lit %d, grayed %d)\n", beam, morph,
                             result.beams.lit, result.beams.grayed ? 1 : 0);
                traced_beam = beam;
                traced_morph = morph;
            }
            if (beam >= 0) P::beam_slot.store(beam, std::memory_order_relaxed);
            else if (beam == -1 && result.beams.lit >= 0) P::beam_slot.store(result.beams.lit, std::memory_order_relaxed);
            st.beams_grayed = morph >= 0 ? morph == 1 : result.beams.grayed;
        }
        // R held long enough for the game to draw the missiles in the slots (Missile, Super Missile, Power Bomb): the one
        // lit, for msr_trigger's missile choice. Standing, Missile or Super Missile is lit and the Power Bomb's slot dim;
        // in Morph Ball the game lights the Power Bomb's by itself.
        if (st.r_down_us >= 0 && now - st.r_down_us >= MissileSlotsUs) {
            static const bool trace = std::getenv("PWR_INPUT_TRACE") != nullptr;
            if (trace && result.beams.lit >= 0 && result.beams.lit != st.missile_lit)
                std::fprintf(stderr, "msr: missiles: slot %d lit (%.2f s into R)\n", result.beams.lit, (now - st.r_down_us) / 1e6);
            if (result.beams.lit >= 0) st.missile_lit = result.beams.lit;
            st.missile_present = result.beams.present;
        }
        // Samus's marker where the game's minimap draws it (minimap.cpp, 2026-11-12: the red found on the picture was
        // once a Missile Reload Station's icon, and the minimap stayed on it). Without that (another version of the
        // game), the red dot the classifier finds.
        Vec2 drawn{};
        if (msr_map_marker(&drawn.x, &drawn.y)) {
            st.map_marker = drawn;
            st.marker_far_us = -1;
            if (st.hud_alpha <= 0.f) st.map_center = st.map_marker;
        } else if (result.marker.found) {
            // Samus's marker, not something else as red seen while it fades (2026-11-03: a red map pin, which the
            // classifier no longer counts): taken near where it was or the map's middle, where the game keeps it;
            // elsewhere (a new area, a teleport) once found there for 0.75 s without it showing near the last place
            const Vec2 m{result.marker.x, result.marker.y};
            const auto near = [&m](Vec2 p) { return std::hypot(m.x - p.x, m.y - p.y) <= MarkerReach; };
            bool take = st.hud_alpha <= 0.f || near(st.map_marker) || near(MapMiddle);
            if (!take) {
                if (st.marker_far_us < 0 || !near(st.marker_far)) {
                    st.marker_far = m;
                    st.marker_far_us = now;
                }
                take = now - st.marker_far_us >= 750'000;
            }
            if (take) {
                st.map_marker = m;
                st.marker_far_us = -1;
                if (st.hud_alpha <= 0.f) st.map_center = st.map_marker;
            }
        }
        const auto& [mx0, my0, mx1, my1] = BC::MapRect;
        const int half_w = static_cast<int>(MapWindowWidth) / 2, half_h = static_cast<int>(MapWindowHeight) / 2;
        const int wx0 = std::clamp(static_cast<int>(st.map_marker.x) - half_w, mx0, mx1 - 1);
        const int wx1 = std::clamp(static_cast<int>(st.map_marker.x) + half_w, mx0 + 1, mx1);
        const int wy0 = std::clamp(static_cast<int>(st.map_marker.y) - half_h, my0, my1 - 1);
        const int wy1 = std::clamp(static_cast<int>(st.map_marker.y) + half_h, my0 + 1, my1);
        const std::array<std::array<int, 4>, 4> strips{{{mx0, wy0, mx0 + 2, wy1},
                                                        {wx0, my0, wx1, my0 + 2},
                                                        {mx1 - 2, wy0, mx1, wy1},
                                                        {wx0, my1 - 2, wx1, my1}}};
        for (std::size_t i = 0; i < strips.size(); ++i) {
            const auto& [x0, y0, x1, y1] = strips[i];
            const auto [r, g, b] = BC::Backdrop(image, x0, y0, x1, y1);
            st.map_backdrop[i] = {r / 255.f, g / 255.f, b / 255.f};
        }
    }
    if (result.bottom == P::Bottom::Prompt) {
        const auto box = BC::BrightBox(image, 140);
        if (!box.Empty()) {
            const int x0 = std::min(box.x0, 320 - box.x1);
            st.prompt_text = {static_cast<float>(x0), static_cast<float>(box.y0), static_cast<float>(320 - x0),
                              static_cast<float>(box.y1)};
        }
    }
    if (result.bottom != P::Bottom::Prompt) {
        bool other = result.bottom == P::Bottom::Menu || result.bottom == P::Bottom::Hud;
        if (!other) {
            const auto content = BC::BrightBox(image, 4);
            other = !content.Empty() && (content.x0 == 0 || content.y0 == 0 || content.x1 == BC::Width ||
                                         content.y1 == BC::Height);
        }
        if (other) {
            st.prompt_foreign_us = now;
            st.prompt_alpha = 0.f;
        }
    }
    if (state == st.candidate) {
        ++st.candidate_frames;
    } else {
        st.candidate = state;
        st.candidate_frames = 1;
        st.candidate_since_us = now;
    }
    // The page holds the same way (unlike the emulator build, which takes each frame's): menus sliding in or out read
    // as no page for a few frames, and touch navigation then lost its buttons.
    const int page = static_cast<int>(result.page);
    if (page == st.page_candidate) {
        ++st.page_frames;
    } else {
        st.page_candidate = page;
        st.page_frames = 1;
        st.page_since_us = now;
    }
    bool held = st.candidate_frames >= 3 && now - st.candidate_since_us >= 80'000;
    // A menu over play that no Start press asked for (#12, #32). Play's menus open from Start (the pause menu); without
    // one, the bottom screen read as a menu was the HUD misread (picking things up flashed the whole map) or a screen of
    // the game's own over play (an elevator's or a Teleport Station's map). Such a menu shows only once it has held 1.5 s,
    // and then as the pause menu does, over the view (2026-11-12, the user: "just bring the bottom screen entirely to the
    // top screen like we do for the pause screen"; it stood beside the view, then showed its main part as a card at the
    // view's top, "entirely too small"), without the pause menu's control hints (Overlay::DrawControlHints).
    const int menu = static_cast<int>(P::Bottom::Menu);
    const bool menu_new = state == menu && P::bottom.load(std::memory_order_relaxed) != menu;
    bool unasked = false;
    // The misread came straight from the HUD. A menu that comes when the HUD has not shown for a second, after the game's
    // own messages and their fade, is the game's: it waits 0.3 s (2026-11-14, the user: the Aeion abilities' screen after
    // their pickup messages took "a few seconds"; "it should be right after you select (a) to continue").
    if (held && menu_new && st.in_play && !st.menu_asked) {
        const long long start_us = P::start_pressed_us.load(std::memory_order_relaxed);   // -1: none yet
        const s64 wait = now - st.hud_seen_us >= 1'000'000 ? 300'000 : 1'500'000;
        if (start_us >= 0 && now - start_us < 2'000'000) st.menu_asked = true;
        else if (now - st.candidate_since_us < wait) held = false;
        else {
            unasked = true;
            std::fprintf(stderr, "msr: a screen of the game's over play (no Start): shown after %.2f s (the HUD %.1f s before)\n",
                         (now - st.candidate_since_us) / 1e6, (now - st.hud_seen_us) / 1e6);
        }
    }
    if (held) {
        const bool page_held = st.page_frames >= 3 && now - st.page_since_us >= 80'000;
        static const bool trace = std::getenv("PWR_MSR_TRACE") != nullptr;   // PWR_MSR_TRACE: the published states
        if (trace && (P::bottom.load() != state || (page_held && P::page.load() != page)))
            std::fprintf(stderr, "msr: bottom %d page %d%s\n", state, page_held ? page : P::page.load(),
                         menu_new && unasked ? " (no Start)" : "");
        P::bottom.store(state, std::memory_order_relaxed);
        if (page_held) P::page.store(page, std::memory_order_relaxed);
        if (menu_new) st.menu_unasked = unasked;
        if (state != menu) st.menu_unasked = false;
        if (state == static_cast<int>(P::Bottom::Hud)) st.menu_asked = false;
        P::menu_unasked.store(st.menu_unasked, std::memory_order_relaxed);
    }
}

// The view's aspect for a window: 16:9 with widescreen, else the top screen's 5:3. Ultrawide (2026-11-03): in play the
// view takes a wider window's aspect, up to 32:9; the world's squeeze (msr_world_scale) and the camera's field of view
// (camera_fov.cpp) follow it through P::view_aspect. The title menus, the gallery and the prologue stay 16:9: the game
// draws their scenes for its own view only, and their art (the title's logo) flat, as camera-facing quads without depth
// test, which the squeeze leaves as they are (they would stretch to the window's width).
// Fill 16:10 (2026-11-06, #38): in play the view also takes a taller window's aspect, down to 16:10, keeping 16:9's
// width and showing more above and below (P::ViewScaleV); the camera culls for that too.
float ViewAspectWanted(float ww, float wh) {
    if (!P::widescreen.load(std::memory_order_relaxed)) return P::NativeAspect;
    if (!st.in_play || wh <= 0.f) return P::WideAspect;
    const float lo = P::fill_16_10.load(std::memory_order_relaxed) ? P::TallAspect : P::WideAspect;
    const float hi = P::ultrawide.load(std::memory_order_relaxed) ? P::UltraAspect : P::WideAspect;
    return std::clamp(ww / wh, lo, hi);
}

// emu_window.cpp MsrSingleScreenLayout: the view (its aspect in effect, P::view_aspect) centered in the window, the panel
// (the bottom screen's 4:3) centered in it at panel_scale of its height.
void SingleLayout(float ww, float wh, Rect& top, Rect& panel) {
    const float aspect = P::view_aspect.load(std::memory_order_relaxed);   // width over height
    float w = ww, h = std::floor(ww / aspect);
    if (h > wh) { h = wh; w = std::floor(wh * aspect); }
    const float x = std::floor((ww - w) / 2), y = std::floor((wh - h) / 2);
    top = {x, y, x + w, y + h};
    const float ph = std::round(h * std::clamp(P::panel_scale.load(), .3f, 1.f));
    const float pw = std::round(ph * 320.f / 240.f);
    const float px = x + std::floor((w - std::min(pw, w)) / 2), py = y + std::floor((h - ph) / 2);
    panel = {px, py, px + pw, py + ph};
}

s64 NowUs(double now_s) {
    const s64 us = static_cast<s64>(now_s * 1e6);
    if (t0_us < 0) t0_us = us - 1;
    return us - t0_us;
}

} // namespace

extern "C" void msr_single_layout(float ww, float wh, float top[4], float panel[4]) {
    Rect t, p;
    SingleLayout(ww, wh, t, p);
    // Touches (video.c, in window coordinates) go where msr_compose last drew the panel when that is not its centered
    // place: the gallery's, beside the view.
    if (st.panel_moved && st.composed_w > 0.f && st.composed_h > 0.f) {
        const float kx = ww / st.composed_w, ky = wh / st.composed_h;
        p = {st.panel_drawn.l * kx, st.panel_drawn.t * ky, st.panel_drawn.r * kx, st.panel_drawn.b * ky};
    }
    top[0] = t.l; top[1] = t.t; top[2] = t.W(); top[3] = t.H();
    panel[0] = p.l; panel[1] = p.t; panel[2] = p.W(); panel[3] = p.H();
}

extern "C" void msr_set_single(int on) { P::enabled.store(on != 0, std::memory_order_relaxed); }
extern "C" int msr_single(void) { return P::enabled.load(std::memory_order_relaxed); }
extern "C" int msr_panel_visible(void) { return P::panel_visible.load(std::memory_order_relaxed); }

// The mouse on the pause screen fitted to the window (st.touch_extra): the touch-screen point under it, by the anchor
// of the piece drawn there (the topmost, as drawn last); 0 when the panel is drawn as the game draws it (video.c maps
// it to the panel's rectangle then). wx, wy in a window of ww x wh, as msr_single_layout's.
extern "C" int msr_panel_touch(float wx, float wy, float ww, float wh, int* tx, int* ty) {
    if (st.touch_extra <= 0.f || !st.touch_items || st.composed_w <= 0.f || st.composed_h <= 0.f || ww <= 0.f || wh <= 0.f)
        return 0;
    const float X = wx * st.composed_w / ww, Y = wy * st.composed_h / wh;
    const Rect& p = st.touch_panel;
    const float s = p.H() / 240.f, extra = st.touch_extra;
    if (s <= 0.f) return 0;
    const float touch_y = (Y - p.t) / s;
    int anchor = MSR_ANCHOR_MIDDLE;
    for (auto it = st.touch_items->rbegin(); it != st.touch_items->rend(); ++it) {
        if (it->anchor == MSR_ANCHOR_STRETCH || Backdrop(*it) || touch_y < it->y || touch_y >= it->y + it->h) continue;
        const float x0 = AnchorX(it->x, it->anchor, p.l, s, extra), x1 = AnchorX(it->x + it->w, it->anchor, p.l, s, extra);
        if (X >= x0 && X < x1) { anchor = it->anchor; break; }
    }
    const float touch_x = (X - AnchorX(0.f, anchor, p.l, s, extra)) / s;
    *tx = static_cast<int>(std::clamp(touch_x, 0.f, 319.f));
    *ty = static_cast<int>(std::clamp(touch_y, 0.f, 239.f));
    return 1;
}

extern "C" int msr_world_in_play(void) { return st.in_play ? 1 : 0; }
extern "C" int msr_beams_held(void) { return st.beams_held ? 1 : 0; }
extern "C" int msr_menu_live(void) { return st.menu_live ? 1 : 0; }

extern "C" void msr_world_scale(float* sx, float* sy) {
    const float zoom = P::world_zoom.load(std::memory_order_relaxed);
    *sx = P::ViewScaleV() * zoom;   // clip x is the screen's vertical (the 3DS's top screen is turned)
    *sy = P::ViewScale() * zoom;
}

extern "C" void msr_ui_scale(float* sx, float* sy) {
    *sx = P::ViewScaleV();
    *sy = P::ViewScale();
}

extern "C" void msr_compose(const uint8_t* bottom, double now_s, unsigned buttons, float ww, float wh,
                            MsrFrame* out) {
    quads.clear();
    const s64 now_us = NowUs(now_s);
    // the Aeion abilities' page on the touch screen (AbilitiesOverlay), as the game holds it: at once, before the touch
    // screen reads as a menu
    const bool abilities = !GuiCopy() && st.in_play && msr_gui_abilities();
    {   // the touch screen as the game holds it (gui_tree.cpp), while a menu or a prompt shows: the title menus, the
        // pause menu (in play the tree also holds the HUD, many objects, not read for nothing)
        const auto shown = static_cast<P::Bottom>(P::bottom.load(std::memory_order_relaxed));
        // (the map filling the pause screen fitted to a wider window, as wide as last frame's: half its extra a side)
        msr_gui_map_margin(st.touch_extra / 2.f);
        const MsrGuiItem* items = nullptr;
        const int n = shown == P::Bottom::Menu || shown == P::Bottom::Prompt || abilities ? msr_gui_tree(&items) : 0;
        gui_now.assign(items, items + std::max(n, 0));
    }
    // ...as it comes in and stands; once its description goes down again from the highest it came up, it is going (it
    // comes up 2 pixels a frame of the game's and goes down so: by the way it moves, not by frames it holds still, which
    // the window's frames outnumber at 120 FPS: until 2026-11-18 the page was taken for going as it came, the user: "now
    // the message doesnt appear at all")
    bool abilities_on = false;
    if (abilities) {
        AbilitiesPage now = AbilitiesNow(gui_now);
        // its button once the label has read typed out 0.1 s (2026-11-18, the user: "the (A) flashes for a few frames":
        // a page turning, the game writes its page and its count apart, and the label can read typed out a moment)
        if (now.prompt != st.abilities_prompt) {
            st.abilities_prompt = now.prompt;
            st.abilities_prompt_us = now_us;
        }
        if (now_us - st.abilities_prompt_us < 100'000) now.prompt = PromptNone;
        if (!st.abilities_seen || now.room.t < st.abilities_top) st.abilities_top = now.room.t;
        else if (now.room.t > st.abilities_top + 1.f) st.abilities_going = true;
        st.abilities_seen = true;
        if (!st.abilities_going) {
            abilities_page = std::move(now);
            abilities_on = true;
        }
    } else {
        st.abilities_seen = st.abilities_going = false;
    }
    FooterFromTree();
    PauseFromTree();
    PauseExtras();
    ControlsIcons();
    // HID Start rising: the game sees the press (the emulator's HID sets this as the game reads it)
    if ((buttons & 8) && !(st.buttons & 8)) {
        P::start_pressed_us.store(now_us, std::memory_order_relaxed);
        if (st.menu_unasked) {   // Start over a menu without one: the pause menu it opens shows its control hints
            st.menu_unasked = false;
            st.menu_asked = true;
            P::menu_unasked.store(false, std::memory_order_relaxed);
        }
    }
    st.buttons = buttons;
    // R's hold, for the missile choice (msr_trigger): read anew each time, from when the slots show the missiles
    if (!(buttons & 0x100)) {
        st.r_down_us = -1;
        st.missile_lit = -1;
        st.missile_present = 0;
        st.missile_pending = 0;
    } else if (st.r_down_us < 0) {
        st.r_down_us = now_us;
    }
    // R held (missiles, Power Bombs): the game draws them in the beam slots, and the HUD keeps the beams as they were
    // (2026-11-03, the user: "it should just stay with whatever beam you were using"; the slots are the touch screen's
    // missile choice meanwhile, the game's own screen is not changed). video.c draws them from its copy of the bottom
    // screen made before R, and their classification waits. From a press while the HUD shows, until 350 ms after R
    // comes up: the beams come back on the next frame, but the lit one flashes and the Grapple slot fades in for ~20.
    if (buttons & 0x100) {
        if (static_cast<P::Bottom>(P::bottom.load(std::memory_order_relaxed)) == P::Bottom::Hud) st.beams_held = true;
        st.beams_up_us = -1;
    } else if (st.beams_held) {
        if (st.beams_up_us < 0) st.beams_up_us = now_us;
        else if (now_us - st.beams_up_us >= 350'000) st.beams_held = false;
    }
    if (bottom) Consume(BC::Image{bottom, false}, now_us);
    if (st.missile_pending != 0 && st.missile_lit >= 0) {   // an RT that came before the missiles showed
        const int which = st.missile_pending;
        st.missile_pending = 0;
        msr_trigger(which);
    }

    Rect layout_top, layout_panel;
    SingleLayout(ww, wh, layout_top, layout_panel);
    const auto state = static_cast<P::Bottom>(P::bottom.load(std::memory_order_relaxed));
    const bool forced = P::force_panel.load(std::memory_order_relaxed);
    const float seconds = st.last_us != 0 ? std::clamp(static_cast<float>(now_us - st.last_us) / 1e6f, 0.f, .1f) : 0.f;
    st.last_us = now_us;

    constexpr s64 transition_us = 1'500'000;
    const long long start_us = P::start_pressed_us.load(std::memory_order_relaxed);
    if (start_us != st.start_seen) {
        st.start_seen = start_us;
        if (state == P::Bottom::Hud && !forced) st.pausing_until_us = start_us + transition_us;
    }
    if (st.pausing_until_us != 0 && now_us >= st.pausing_until_us) st.pausing_until_us = 0;
    if (state == P::Bottom::Menu && st.pausing_until_us != 0) {
        st.pausing_until_us = 0;
        st.paused = true;
    }
    if (st.paused && state != P::Bottom::Menu) {
        st.paused = false;
        st.resuming_until_us = now_us + transition_us;
    }
    if (state == P::Bottom::Hud) st.resuming_until_us = 0;
    const bool pausing = st.pausing_until_us != 0;
    const bool resuming = now_us < st.resuming_until_us;
    // The game's map showing a place of its own in play (minimap.cpp msr_map_focus: the DNA scanner's way on, when an
    // area's last Metroid dies): the whole bottom screen over the view while it does, as an elevator's map (Consume), and
    // 0.3 s after, so the scanner's moments run together. The panel stays live until it has faded out.
    // At most 15 s at a time: the game counts its time down only in a part of the minimap's update not checked here
    // (PWR_TEST_DNA_SCAN's map stayed so), and a flag left set must not keep the HUD away.
    if (st.in_play && state != P::Bottom::Menu) {
        const int focus = msr_map_focus();
        if (!focus) {
            st.map_focus_capped = false;
        } else if (!st.map_focus_capped) {
            if (!st.map_focus_on) {
                st.map_focus_since_us = now_us;
                std::fprintf(stderr, "msr: the game's map shows %s: the bottom screen over the view (%.2f s)\n",
                             focus & 1 ? "the DNA scanner's way" : "a place of its own", now_us / 1e6);
            }
            if (now_us - st.map_focus_since_us < 15'000'000) {
                st.map_focus_until_us = now_us + 300'000;
            } else {
                st.map_focus_capped = true;
                std::fprintf(stderr, "msr: the game's map kept a place of its own 15 s: the HUD back\n");
            }
        }
    } else {
        st.map_focus_until_us = 0;
    }
    const bool map_focus = now_us < st.map_focus_until_us;
    if (st.map_focus_on && !map_focus)
        std::fprintf(stderr, "msr: the game's map is back on Samus (%.2f s, the bottom screen read as %d)\n", now_us / 1e6,
                     static_cast<int>(state));
    st.map_focus_on = map_focus;
    if (map_focus) st.panel_focus = true;
    else if (state == P::Bottom::Menu || st.panel_alpha <= 0.f) st.panel_focus = false;

    if (state != P::Bottom::Prompt) {
        st.prompt_since_us = now_us;
        st.prompt_over_menu = state == P::Bottom::Menu;
    } else {
        st.prompt_seen_us = now_us;
    }
    // The pad's menus' layout (glyphs.cpp): its buttons choose and go back unless the HUD shows, from a menu at once or
    // anything else held 300 ms (2026-11-03, the user: "in game it also needs to change too": the game's dialogs over
    // play, "Save your progress?", show the emblem below). Its icons follow from a menu, or a prompt held 300 ms (the
    // title's), until the HUD shows; cutscenes, loading and those dialogs keep what was (their text names play's buttons).
    // The game's own HUD counts from its first fade in (its opacity, gui_tree.cpp), where the bottom screen is not read as
    // a menu or a prompt: after a file loads, the game fades it to half while Samus stands in the save station and then
    // to whole, and gives her play's buttons as it does, but the bottom screen read as the HUD only near the end of that
    // fade (83% of it, 2.4 s after it began, 2026-11-20). Until then the pad's bottom button was still 3DS A, which in
    // play uses the Aeion ability (#61: "if you press A after loading a new file, one of the special abilities
    // activate"). The pause menu, the game's screens over play (an elevator's map, the Aeion abilities' page) and its
    // message boxes keep the menus' layout as before.
    const bool game_hud = state != P::Bottom::Menu && state != P::Bottom::Prompt && msr_gui_hud_alpha() > 0.f;
    if (state == P::Bottom::Hud || game_hud) {
        if (state == P::Bottom::Hud) st.hud_seen_us = now_us;
        st.menu_icons = false;
    } else if (state == P::Bottom::Menu || (state == P::Bottom::Prompt && now_us - st.prompt_since_us >= 300'000)) {
        st.menu_icons = true;
    }
    // ...and while the top screen's message box shows over play, the HUD below or not (a Recharge Station's "Ammo is
    // already filled to capacity." keeps the HUD; 2026-11-18, the user: "says to press a. but a does nothing. b is
    // letting you out of that menu": the box's A, drawn in the menus' layout always, was the pad's bottom button, which
    // in play is bound to Jump, 3DS B)
    const bool dialog = st.in_play && msr_gui_dialog();
    msr_buttons_state(dialog || (state != P::Bottom::Hud && !game_hud && (state == P::Bottom::Menu || now_us - st.hud_seen_us >= 300'000)),
                      st.menu_icons);
    const auto page = static_cast<P::Page>(P::page.load(std::memory_order_relaxed));
    // Play ends at the title menus or a prompt (the title's, the prologue's), but not at the pause menu's own dialogs,
    // prompts straight over it (the map pins tool's first use): play went on behind them (2026-11-07, #43: the title
    // menus' backdrop was then left out of the pins tool, msr_skip_bottom_draw).
    if (state == P::Bottom::Hud) {
        st.in_play = true;
    } else if ((state == P::Bottom::Prompt && now_us - st.prompt_since_us >= 300'000 && !st.prompt_over_menu) ||
               (state == P::Bottom::Menu &&
                (page == P::Page::FileList || page == P::Page::FileOpen || page == P::Page::Gallery))) {
        st.in_play = false;
    }
    // the view's aspect: eased between play's (ultrawide) and the menus' 16:9, applied from the next frame's layout
    const float aspect_target = ViewAspectWanted(ww, wh);
    if (st.aspect <= 0.f || std::abs(aspect_target - st.aspect) < .002f) st.aspect = aspect_target;
    else st.aspect += (aspect_target - st.aspect) * (1.f - std::exp(-8.f * seconds));
    P::view_aspect.store(st.aspect, std::memory_order_relaxed);
    const float zoom_target = st.in_play ? P::zoom.load(std::memory_order_relaxed) : 1.f;
    st.zoom += (zoom_target - st.zoom) * (1.f - std::exp(-8.f * seconds));
    if (std::abs(zoom_target - st.zoom) < .001f) st.zoom = zoom_target;
    P::world_zoom.store(st.zoom, std::memory_order_relaxed);

    const bool prompt_on = st.prompt_alpha > 0.f;
    const bool prompt = (state == P::Bottom::Prompt && (prompt_on || now_us - st.prompt_since_us >= 300'000)) ||
                        (prompt_on && state == P::Bottom::Logo && now_us - st.prompt_seen_us < 1'500'000);
    // (the abilities' page over the view: the touch screen's panel kept away until it no longer reads as a menu)
    if (abilities) st.abilities_hold = true;
    else if (state != P::Bottom::Menu) st.abilities_hold = false;
    const bool show_hud = state == P::Bottom::Hud && !forced && !pausing && !map_focus && !abilities;
    // Between two pause pages (2026-11-17, the user: "it moves somewhat slow between the tabs on the pause screen. make
    // them transition smooth between each tab. it should maintain the same frame rate as the main game"): the game
    // shows its emblem on the touch screen for some 0.2 s, which reads as loading, and the panel faded out to the view
    // and back in (half a second). Now the panel stays, its background with it, the last page's contents fading out in
    // 0.12 s and the next page's in as it comes, every frame drawn from the game's interface at the game's rate. Not
    // past 0.8 s, nor once play is back.
    const bool pause_page = state == P::Bottom::Menu &&
                            (page == P::Page::PauseMap || page == P::Page::PauseSamus || page == P::Page::MapPins);
    if (pause_page && !GuiCopy() && !gui_now.empty()) {
        if (st.pause_bridge_us >= 0) st.pause_arrive_us = now_us;
        st.pause_bridge_us = -1;
        st.pause_seen_us = now_us;
    }
    const bool pause_bridge = !pause_page && st.pause_seen_us >= 0 && now_us - st.pause_seen_us < 800'000 &&
                              state != P::Bottom::Hud && state != P::Bottom::Prompt && !gui_last.empty();
    if (pause_bridge && st.pause_bridge_us < 0) st.pause_bridge_us = now_us;
    if (!pause_bridge && !pause_page) st.pause_bridge_us = -1;
    if (state == P::Bottom::Hud) st.pause_seen_us = -1;
    const bool show_panel = (state == P::Bottom::Menu && !st.abilities_hold) || forced || map_focus || pause_bridge;
    const bool show_prompt = prompt && !forced && !pausing && !resuming && now_us - st.prompt_foreign_us >= 300'000;
    const float fade = FadePerSecond * seconds;
    // The pause screen fitted to the window (the menu's "Fit the pause screen to the window", 2026-11-17, the user: "a
    // menu option on our menu that stretches the pause screen to fill the window it's in"; stretched, then: "looks
    // weird. see if you can work on resizing it to look normal for the resolution of the screen instead"): the touch
    // screen's menus over play (the pause pages and what they open, an elevator's, the panel between two pages) laid
    // out over the whole window from the game's interface, as tall as the window: its pieces where their anchors say
    // (gui_tree.cpp: kept to the sides or the middle, bars and backgrounds stretched between their ends, the map's view
    // opened to the width, GuiQuads); touches follow (msr_panel_touch). Not for a screen drawn as the touch screen's copy
    // (it stays 4:3). Decided while the panel shows and kept while it fades out; taken at once when it was hidden,
    // eased in 0.15 s otherwise (the option changed with it shown, a pause's first frames read as another screen).
    const bool fit_on = P::fit_pause.load(std::memory_order_relaxed);
    if (show_panel) {
        const bool from_tree = !GuiCopy() && !gui_now.empty() && state == P::Bottom::Menu;
        st.fit_wanted = fit_on && st.in_play && !forced && !map_focus && (from_tree || pause_bridge);
    }
    if (st.panel_alpha <= 0.f) st.fit = st.fit_wanted ? 1.f : 0.f;
    else st.fit = Approach(st.fit, st.fit_wanted ? 1.f : 0.f, 7.f * seconds);
    st.hud_alpha = Approach(st.hud_alpha, show_hud ? 1.f : 0.f, fade);
    st.panel_alpha = Approach(st.panel_alpha, show_panel ? 1.f : 0.f, fade);
    st.prompt_alpha = Approach(st.prompt_alpha, show_prompt ? 1.f : 0.f, PromptFadePerSecond * seconds);
    P::panel_visible = st.panel_alpha > 0.5f || st.prompt_alpha > 0.5f;

    // Gallery viewer: the view moves aside and the panel stands next to it.
    const bool gallery = state == P::Bottom::Menu && page == P::Page::Gallery;
    st.gallery = Approach(st.gallery, gallery ? 1.f : 0.f, fade);
    const float ease = st.gallery * st.gallery * (3.f - 2.f * st.gallery);
    Rect view_top = layout_top, panel = layout_panel;
    if (ease > 0.f) {
        const Rect& full = layout_top;
        const float fx = full.l, fy = full.t, fw = full.W(), fh = full.H();
        const float gap = fh * .03f;
        const float vw = fw * .72f, vh = fh * .72f;
        const float vx = fx + gap, vy = fy + (fh - vh) / 2;
        const float room = fx + fw - (vx + vw) - 2 * gap;
        const float ph = std::min(room * .75f, vh), pw = ph / .75f;
        const float px = vx + vw + gap + (room - pw) / 2, py = fy + (fh - ph) / 2;
        const auto mix = [ease](const Rect& from, float x, float y, float w, float h) {
            const auto lerp = [ease](float a, float b) { return std::round(a + (b - a) * ease); };
            return Rect{lerp(from.l, x), lerp(from.t, y), lerp(from.r, x + w), lerp(from.b, y + h)};
        };
        view_top = mix(layout_top, vx, vy, vw, vh);
        panel = mix(layout_panel, px, py, pw, ph);
    }
    if (st.fit > 0.f) {   // the pause screen over the window
        const float e = st.fit * st.fit * (3.f - 2.f * st.fit);
        const auto lerp = [e](float a, float b) { return std::round(a + (b - a) * e); };
        panel = Rect{lerp(panel.l, 0.f), lerp(panel.t, 0.f), lerp(panel.r, ww), lerp(panel.b, wh)};
    }
    st.panel_moved = ease > 0.f || st.fit > 0.f;   // (msr_single_layout: touches)
    st.touch_items = nullptr;
    st.touch_extra = 0.f;
    st.panel_drawn = panel;
    st.composed_w = ww;
    st.composed_h = wh;

    // the HUD at the view's corners; on a view wider than 16:9, within its middle 16:9 if the player asks
    float left = layout_top.l, width = layout_top.W();
    const float top_y = layout_top.t, height = layout_top.H();
    if (P::hud_within_16_9.load(std::memory_order_relaxed) && width > std::ceil(height * P::WideAspect)) {
        const float w = std::round(height * P::WideAspect);
        left += std::floor((width - w) / 2);
        width = w;
    }
    const float unit = height / 240.f * std::clamp(P::hud_scale.load(), .5f, 2.f);
    const float margin = 6.f * unit;

    if (st.hud_alpha > 0.f) {
        const float alpha = st.hud_alpha * std::clamp(P::hud_opacity.load(), 0.f, 1.f);
        Style hud;
        hud.opacity = alpha; hud.keyed = true; hud.key_low = .28f; hud.key_high = .5f; hud.feather = 6;
        hud.hard_borders = true;
        const auto piece = [&](float x0, float y0, float x1, float y1, float x, float y, float scale) {
            Region(Source(x0, y0, x1, y1), x, y, (x1 - x0) * unit * scale, (y1 - y0) * unit * scale, hud);
        };
        const float shade = .32f * alpha;

        constexpr float energy_scale = .8f, energy_height = 58.f;
        Fill(left + margin, top_y + margin - 2.f * unit * energy_scale, 230.f * unit * energy_scale,
             (energy_height + 4.f) * unit * energy_scale, 0, 0, 0, shade, 30);
        piece(0, 0, 250, energy_height, left + margin, top_y + margin, energy_scale);

        if (P::show_abilities) {
            st.beam_gray = Approach(st.beam_gray, st.beams_grayed ? 1.f : 0.f, 6.f * seconds);
            const auto keyed = [&](float low, float high, float gray_low, float gray_high) {
                Style s;
                s.opacity = alpha; s.keyed = true;
                s.key_low = low + (gray_low - low) * st.beam_gray;
                s.key_high = high + (gray_high - high) * st.beam_gray;
                s.circle = 38;
                return s;
            };
            const Style unlit_icon = keyed(.3f, .5f, .18f, .28f);
            const Style unlit_grapple = keyed(.3f, .5f, .39f, .47f);
            const Style lit_icon = keyed(.6f, .85f, .35f, .55f);
            const float size = 64.f * unit * .6f;
            const float y = top_y + margin + (energy_height + 2.f) * unit * energy_scale;
            const int lit = P::beam_slot.load(std::memory_order_relaxed);
            const int present = P::beam_slots.load(std::memory_order_relaxed);
            // The usual beam's slot shows her best beam, as the game's does (2026-11-16, the user's recording: Plasma's)
            const auto has = [](uint32_t key, const char* name) { return msr_item(key, name) > 0.f; };
            const int usual = has(0x708724, "ITEM_WEAPON_PLASMA_BEAM")   ? MSR_SPRITE_PLASMA
                              : has(0x708730, "ITEM_WEAPON_SPAZER_BEAM") ? MSR_SPRITE_SPAZER
                              : has(0x70873C, "ITEM_WEAPON_WAVE_BEAM")   ? MSR_SPRITE_WAVE
                                                                         : MSR_SPRITE_POWER;
            float x = left + margin;
            for (int i = 0; i < static_cast<int>(BC::BeamSlotRects.size()); ++i) {
                if (i > 0 && (present & (1 << i)) == 0) continue;
                if (i == lit) Fill(x, y, size, size, .35f, .75f, .95f, .4f * alpha * (1.f - .6f * st.beam_gray), 45);
                float uv[4];
                int atlas = 0;
                if (msr_gui_sprite(i == 0 ? usual : i == 1 ? MSR_SPRITE_ICE : MSR_SPRITE_GRAPPLE, &atlas, uv)) {
                    // The game's own icon (gui_assets.cpp), tinted as the touch screen shows it: the lit one near white,
                    // the others light blue; in Morph Ball all of them grayed alike
                    const float icon = size * .8f, inset = (size - icon) / 2;
                    MsrQuad q{};
                    q.dst[0] = x + inset; q.dst[1] = y + inset; q.dst[2] = icon; q.dst[3] = icon;
                    q.src[0] = uv[0]; q.src[1] = uv[1]; q.src[2] = uv[2]; q.src[3] = uv[3];
                    const bool on = i == lit;
                    q.style[0] = alpha;
                    q.style[1] = (on ? 1.15f : .9f) * (1.f - .45f * st.beam_gray);   // gain
                    q.style[2] = .8f * st.beam_gray;                                    // gray
                    q.style[3] = 4.f;
                    q.color[0] = on ? .72f : .6f; q.color[1] = on ? 1.f : .78f; q.color[2] = 1.f; q.color[3] = 1.f;
                    q.source = 3;
                    q.atlas = atlas;
                    quads.push_back(q);
                } else {   // no atlas (another version of the game): the touch screen's slot
                    const auto& [x0, y0, x1, y1] = BC::BeamSlotRects[i];
                    Region(Source(static_cast<float>(x0), static_cast<float>(y0), static_cast<float>(x1),
                                  static_cast<float>(y1)),
                           x, y, size, size, i == lit ? lit_icon : i == 2 ? unlit_grapple : unlit_icon);
                    quads.back().source = st.beams_held ? 1 : 0;   // R held: the beams before it (video.c)
                }
                x += size + 4.f * unit;
            }
        }

        float map_x = left + width - margin, map_w = 0.f, map_bottom = top_y + margin;
        if (P::show_minimap) {
            const float scale = .675f * std::clamp(P::minimap_size.load(), .5f, 2.f);
            constexpr float source_w = MapWindowWidth, source_h = MapWindowHeight;
            const float w = source_w * unit * scale, h = source_h * unit * scale;
            const float x = left + width - margin - w, y = top_y + margin;
            const float opacity = alpha * std::clamp(P::minimap_opacity.load(), 0.f, 1.f);
            Style map;
            map.opacity = opacity;
            // brighter than the bottom screen's own colors (2026-11-14, the user: on an HDR monitor the map "doesn't
            // appear that bright", its blues at paper white beside the game's highlights expanded to the peak)
            map.gain = std::clamp(P::minimap_brightness.load(), 1.f, 2.f);
            const Vec2 gap{st.map_marker.x - st.map_center.x, st.map_marker.y - st.map_center.y};
            if (std::abs(gap.x) + std::abs(gap.y) < .1f) {
                st.map_center = st.map_marker;
            } else {
                const float k = 1.f - std::exp(-12.f * seconds);
                st.map_center.x += gap.x * k;
                st.map_center.y += gap.y * k;
            }
            const float sx0 = st.map_center.x - source_w / 2, sy0 = st.map_center.y - source_h / 2;
            // The map rebuilt from the game's interface (2026-11-17, the user: "do the minimap the same way while
            // keeping the function where we now track samus exactly"): the touch screen's map view as the game holds
            // it (gui_tree.cpp msr_gui_minimap: the rooms' tiles, doors, items, Samus's icon, the DNA scanner's light),
            // over the game's own background (the Background sprite, tinted as the game tints it), the same window round
            // Samus's marker as before (minimap.cpp) and the same brightness; the map past the touch screen's view too,
            // where the copy had to fill in. Without it (another version of the game), the copy.
            const MsrGuiItem* map_items = nullptr;
            float map_view[4];
            // half the game's tint (2026-11-17, the user: "the minimap i would prefer if it had a darker background"):
            // the background and the map's dark fill round the rooms, the rooms standing out from them over play
            constexpr float shade = .5f;
            const int map_count = GuiCopy() ? 0 : msr_gui_minimap(&map_items, map_view, shade);
            if (map_count > 0) {
                const float k = w / source_w;
                const float bx0 = std::clamp(sx0, 0.f, 320.f - source_w), by0 = std::clamp(sy0, 0.f, 240.f - source_h);
                MsrQuad bg{};
                bg.dst[0] = x; bg.dst[1] = y; bg.dst[2] = w; bg.dst[3] = h;
                bg.src[0] = bx0 / 320.f * .3125f; bg.src[1] = by0 / 240.f * .46875f;
                bg.src[2] = (bx0 + source_w) / 320.f * .3125f; bg.src[3] = (by0 + source_h) / 240.f * .46875f;
                bg.style[0] = opacity; bg.style[1] = 1.f; bg.style[3] = 5.f;
                bg.color[0] = .34f * shade * map.gain; bg.color[1] = .57f * shade * map.gain;
                bg.color[2] = .84f * shade * map.gain; bg.color[3] = 1.f;
                bg.source = 3;
                bg.atlas = MSR_ATLAS_HUD_OPAQUE;
                quads.push_back(bg);
                gui_minimap.assign(map_items, map_items + map_count);
                for (auto& it : gui_minimap) {
                    it.clip[0] = sx0; it.clip[1] = sy0; it.clip[2] = sx0 + source_w; it.clip[3] = sy0 + source_h;
                    for (int c = 0; c < 3; ++c) it.color[c] *= map.gain;
                }
                GuiQuads(gui_minimap, x - sx0 * k, y - sy0 * k, 320.f * k, 240.f * k, opacity, true);
                const float line = std::max(1.f, unit * .75f), fa = .8f * alpha;
                Fill(x - line, y - line, w + 2 * line, line, .35f, .75f, .95f, fa);
                Fill(x - line, y + h, w + 2 * line, line, .35f, .75f, .95f, fa);
                Fill(x - line, y, line, h, .35f, .75f, .95f, fa);
                Fill(x + w, y, line, h, .35f, .75f, .95f, fa);
                map_x = x;
                map_w = w;
                map_bottom = y + h + line;
            } else {
            const auto& [mx0, my0, mx1, my1] = BC::MapRect;
            const float cx0 = std::max(sx0, static_cast<float>(mx0)), cy0 = std::max(sy0, static_cast<float>(my0));
            const float cx1 = std::min(sx0 + source_w, static_cast<float>(mx1));
            const float cy1 = std::min(sy0 + source_h, static_cast<float>(my1));
            const float k = w / source_w;
            const float ix = x + (cx0 - sx0) * k, iy = y + (cy0 - sy0) * k, iw = (cx1 - cx0) * k, ih = (cy1 - cy0) * k;
            const auto backdrop = [&](std::size_t edge, float bx, float by, float bw, float bh) {
                const auto& c = st.map_backdrop[edge];
                const float g = map.gain / std::max(1.f, map.gain * std::max({c.r, c.g, c.b}));   // as msr.frag
                Fill(bx, by, bw, bh, c.r * g, c.g * g, c.b * g, opacity);
            };
            if (ix > x) backdrop(0, x, y, ix - x, h);
            if (iy > y) backdrop(1, ix, y, iw, iy - y);
            if (ix + iw < x + w) backdrop(2, ix + iw, y, x + w - (ix + iw), h);
            if (iy + ih < y + h) backdrop(3, ix, iy + ih, iw, y + h - (iy + ih));
            Region(Source(cx0, cy0, cx1, cy1), ix, iy, iw, ih, map);
            const float line = std::max(1.f, unit * .75f), fa = .8f * alpha;
            Fill(x - line, y - line, w + 2 * line, line, .35f, .75f, .95f, fa);
            Fill(x - line, y + h, w + 2 * line, line, .35f, .75f, .95f, fa);
            Fill(x - line, y, line, h, .35f, .75f, .95f, fa);
            Fill(x + w, y, line, h, .35f, .75f, .95f, fa);
            map_x = x;
            map_w = w;
            map_bottom = y + h + line;
            }
        }

        if (P::show_counters) {
            // The DNA and Metroid counters, rebuilt from the game's state and its own pictures (2026-11-17, the user:
            // "pulling directly from the game's logic", as the beam row): the numbers are Samus's items the game
            // writes into the touch screen's bar (its HUD's UpdateDNACounter, 0x4F7B4C: ITEM_ADN; the Metroids left,
            // ITEM_METROID_TOTAL_COUNT less ITEM_METROID_COUNT), the icons the bar's sprites, and the Metroid radar's
            // layers take the colors its logic gives them each frame (cheats.cpp msr_metroid_radar: its pulse, and the
            // flare). Laid out as gui/scripts/ingamemenulowerspacecomposition.lc lays out the bar, in touch-screen
            // pixels from each group's corner (the bar's info row is at 60, 211; the DNA group starts at 64, 217, the
            // Metroid group at 192, 217); the text is digital_hefty in the bar's color, 0.68 0.83 0.93. Without them
            // (out of play, another version of the game) the bar's two pieces are copied as before.
            const float adn = msr_item(0x7086B8, "ITEM_ADN");
            const float killed = msr_item(0x7086BC, "ITEM_METROID_COUNT");
            const float metroids = msr_item(0x7086C0, "ITEM_METROID_TOTAL_COUNT");
            float font_size = 0.f, ascent = 0.f, probe[4];
            int probe_atlas = 0;
            const bool rebuilt = adn >= 0.f && killed >= 0.f && metroids >= 0.f &&
                                 msr_gui_font(MSR_FONT_HEFTY, &font_size, &ascent) &&
                                 msr_gui_sprite(MSR_SPRITE_DNA_COUNT, &probe_atlas, probe) &&
                                 msr_gui_sprite(MSR_SPRITE_METROID_BASE, &probe_atlas, probe);
            if (rebuilt) {
                char dna_text[16], metroid_text[16];
                std::snprintf(dna_text, sizeof dna_text, "%d", static_cast<int>(adn));
                std::snprintf(metroid_text, sizeof metroid_text, "%d", std::max(0, static_cast<int>(metroids - killed)));
                const auto text_width = [](const char* text) {
                    float w = 0.f;
                    MsrGlyph g;
                    for (; *text; ++text)
                        if (msr_gui_glyph(MSR_FONT_HEFTY, static_cast<unsigned char>(*text), &g)) w += g.advance;
                    return w;
                };
                // each group as wide as the copy was, or its number's room
                const float dna_group = std::max(33.f, 20.f + text_width(dna_text) + 2.f);
                const float metroid_group = std::max(48.f, 28.f + text_width(metroid_text) + 2.f);
                const float natural = (dna_group + 10.f + metroid_group) * unit;
                const float fit = map_w > 0.f ? std::min(1.f, map_w / natural) : 1.f;
                const float k = unit * fit, total = natural * fit, h = 23.f * k;
                const float x = map_w > 0.f ? map_x + (map_w - total) / 2 : map_x - total;
                const float y = map_bottom + 6.f * unit;
                Fill(x - 8.f * unit, y - 3.f * unit, total + 16.f * unit, h + 6.f * unit, 0, 0, 0, shade, 30);

                const auto sprite = [&](int id, float sx, float sy, float sw, float sh, const float color[4], int mode) {
                    float uv[4];
                    int atlas = 0;
                    if (!msr_gui_sprite(id, &atlas, uv) || color[3] <= 0.f) return;
                    MsrQuad q{};
                    q.dst[0] = sx; q.dst[1] = sy; q.dst[2] = sw; q.dst[3] = sh;
                    std::copy(uv, uv + 4, q.src);
                    q.style[0] = alpha; q.style[3] = static_cast<float>(mode);
                    std::copy(color, color + 4, q.color);
                    q.source = 3;
                    q.atlas = atlas;
                    quads.push_back(q);
                };
                static constexpr float TextColor[4] = {.68f, .83f, .93f, 1.f}, White[4] = {1.f, 1.f, 1.f, 1.f};
                const auto text = [&](const char* t, float tx, float ty) {   // tx, ty: the text's top-left
                    MsrGlyph g;
                    for (float pen = tx; *t; ++t) {
                        if (!msr_gui_glyph(MSR_FONT_HEFTY, static_cast<unsigned char>(*t), &g)) continue;
                        MsrQuad q{};
                        q.dst[0] = pen + g.left * k; q.dst[1] = ty + (ascent - g.top) * k;
                        q.dst[2] = g.w * k; q.dst[3] = g.h * k;
                        std::copy(g.uv, g.uv + 4, q.src);
                        q.style[0] = alpha; q.style[3] = 5.f;
                        std::copy(TextColor, TextColor + 4, q.color);
                        q.source = 3;
                        q.atlas = MSR_ATLAS_FONT;
                        quads.push_back(q);
                        pen += g.advance * k;
                    }
                };
                // the radar's layers as its logic colors them: base, color, highlight, flare (none yet: the base alone)
                float radar[4][4] = {{1.f, 1.f, 1.f, 1.f}, {1.f, 1.f, 1.f, 0.f}, {1.f, 1.f, 1.f, 0.f}, {1.f, 1.f, 1.f, 0.f}};
                msr_metroid_radar(radar);
                // the bar's order: the numbers, the icons, then the flares over them
                const float mx = x + (dna_group + 10.f) * k;
                text(dna_text, x + 20.f * k, y);
                text(metroid_text, mx + 28.f * k, y);
                sprite(MSR_SPRITE_DNA_COUNT, x + 2.f * k, y + 2.f * k, 15.f * k, 18.f * k, White, 5);
                sprite(MSR_SPRITE_METROID_BASE, mx + 5.f * k, y + 1.f * k, 20.f * k, 22.f * k, radar[0], 5);
                sprite(MSR_SPRITE_METROID_COLOR, mx + 5.f * k, y + 1.f * k, 20.f * k, 22.f * k, radar[1], 5);
                sprite(MSR_SPRITE_METROID_HIGHLIGHT, mx + 5.f * k, y + 1.f * k, 20.f * k, 22.f * k, radar[2], 5);
                sprite(MSR_SPRITE_FLARE, mx - 7.f * k, y - 4.f * k, 69.f * k, 30.f * k, radar[3], 6);
            } else {
                const float natural = (33.f + 10.f + 48.f) * unit;
                const float fit = map_w > 0.f ? std::min(1.f, map_w / natural) : 1.f;
                const float dna_w = 33.f * unit * fit, gap = 10.f * unit * fit, total = natural * fit;
                const float h = 23.f * unit * fit;
                const float x = map_w > 0.f ? map_x + (map_w - total) / 2 : map_x - total;
                const float y = map_bottom + 6.f * unit;
                Fill(x - 8.f * unit, y - 3.f * unit, total + 16.f * unit, h + 6.f * unit, 0, 0, 0, shade, 30);
                Style counter;
                counter.opacity = alpha; counter.keyed = true; counter.key_low = .38f; counter.key_high = .58f;
                counter.feather = 6;
                Region(Source(64, 217, 97, 240), x, y, dna_w, h, counter);
                Region(Source(192, 217, 240, 240), x + dna_w + gap, y, 48.f * unit * fit, h, counter);
            }
        }
    }

    st.abilities_alpha = Approach(st.abilities_alpha, abilities_on ? 1.f : 0.f, fade);
    if (st.abilities_alpha > 0.f) AbilitiesOverlay(layout_top, abilities_page, st.abilities_alpha);
    else abilities_page = {};

    if (st.prompt_alpha > 0.f) {
        // The title's prompt from the game's interface when the title menus hold it, its card around what it shows;
        // else the touch screen's text as the classifier found it
        Rect rebuilt{1e9f, 1e9f, -1e9f, -1e9f};
        for (const auto& it : gui_now) {
            if (Backdrop(it)) continue;
            rebuilt = {std::min(rebuilt.l, it.x), std::min(rebuilt.t, it.y), std::max(rebuilt.r, it.x + it.w),
                       std::max(rebuilt.b, it.y + it.h)};
        }
        const bool prompt_rebuilt = !GuiCopy() && rebuilt.r > rebuilt.l && rebuilt.b > rebuilt.t;
        const Rect& typed = prompt_rebuilt ? rebuilt : st.prompt_text;
        auto& card = st.prompt_card;
        const auto beyond = [](const Rect& a, const Rect& b) {
            return a.l < b.l - 1.f || a.t < b.t - 1.f || a.r > b.r + 1.f || a.b > b.b + 1.f;
        };
        if (st.prompt_card_us == 0 || std::abs(typed.t - card.t) > 4.f) {
            if (st.prompt_card_us == 0) st.prompt_card_shown = typed;
            card = typed;
            st.prompt_card_us = now_us;
        } else if (beyond(typed, card)) {
            card = {std::min(card.l, typed.l), std::min(card.t, typed.t), std::max(card.r, typed.r),
                    std::max(card.b, typed.b)};
            st.prompt_card_us = now_us;
        } else if (!beyond(card, typed) || now_us - st.prompt_card_us > 500'000) {
            card = typed;
            st.prompt_card_us = now_us;
        }
        const float pad_x = 12.f, pad_y = 7.f;
        const float panel_scale = panel.H() / 240.f;
        const float fit_scale = std::min({panel_scale, height * .52f / 180.f, width * .94f / 320.f});
        if (card.b - card.t > 26.f || height * .8f + (card.b + pad_y - 120.f) * panel_scale > height * .95f) {
            st.prompt_tall_us = now_us;
        }
        const bool tall = st.prompt_tall_us != 0 && now_us - st.prompt_tall_us < 5'000'000;
        st.prompt_fit = Approach(st.prompt_fit, tall ? 1.f : 0.f, 4.f * seconds);
        const float fit = st.prompt_fit * st.prompt_fit * (3.f - 2.f * st.prompt_fit);
        const float scale = panel_scale + (fit_scale - panel_scale) * fit;
        const float short_y = height * .8f - 120.f * panel_scale;
        const float fit_y = std::min(height * .8f, height * .95f - 90.f * fit_scale) - 126.f * fit_scale;
        const float w = 320.f * scale, h = 240.f * scale;
        const float x = left + (width - w) / 2, y = top_y + short_y + (fit_y - short_y) * fit;

        auto& shown = st.prompt_card_shown;
        const float e = std::min(1.f, 12.f * seconds);
        shown = {shown.l + (card.l - shown.l) * e, shown.t + (card.t - shown.t) * e, shown.r + (card.r - shown.r) * e,
                 shown.b + (card.b - shown.b) * e};
        const float card_alpha = std::clamp(P::menu_backdrop.load(std::memory_order_relaxed), 0.f, 1.f);
        Fill(x + (shown.l - pad_x) * scale, y + (shown.t - pad_y) * scale, (shown.r - shown.l + 2 * pad_x) * scale,
             (shown.b - shown.t + 2 * pad_y) * scale, .01f, .035f, .07f, card_alpha * st.prompt_alpha, 4);
        Style text;
        if (prompt_rebuilt) {
            GuiQuads(gui_now, x, y, w, h, st.prompt_alpha, false, .7f);
        } else if (st.bottom_black) {
            const float peak = std::max(st.bottom_peak, .05f);
            text.opacity = st.prompt_alpha * (.7f + .3f * std::min(peak / .88f, 1.f));
            text.over_black = true;
            text.gain = std::clamp(.88f / peak, 1.f, 12.f);
        } else {
            text.opacity = st.prompt_alpha; text.keyed = true; text.key_low = .45f; text.key_high = .65f;
            text.feather = 4;
        }
        if (!prompt_rebuilt) Region({0.f, 0.f, 1.f, 1.f}, x, y, w, h, text);
    } else {
        st.prompt_card_us = 0;
        if (now_us - st.prompt_tall_us >= 5'000'000) st.prompt_fit = 0.f;
    }

    // The panel shows the bottom screen only while it reads as a menu. After that the panel holds and fades out on the
    // menu as last seen: video.c's copy (source 2), with its backdrop as it was then. Live, the panel showed what the
    // game drew there next for those 0.2 s: after a file's Continue, the ship's cockpit flashed over the view
    // (2026-11-12, the user's video). The panel shown by force stays live.
    const bool panel_live = st.menu_live || forced || st.panel_focus;
    if (panel_live) st.menu_see_through = !st.panel_focus && P::backdrop_removed.load(std::memory_order_relaxed);
    if (st.panel_alpha > 0.f) {
        const bool see_through = st.menu_see_through;
        const float card = std::clamp(P::menu_backdrop.load(std::memory_order_relaxed), 0.f, 1.f);
        // the view darkens behind the menus: the pause menu's by 45%, the title menus' (the file select) by 25-60% as
        // the Title menu background setting says (2026-11-03, the user: the file select's background "needs to be
        // darkened"; it was 20%)
        // The DNA scanner's map (st.panel_focus): see-through and the view not darkened, Samus showing behind it
        // (2026-11-14, the user: "have it be [see-through] so you can still see samus behind it")
        const float through = st.panel_focus ? .62f : 1.f;
        if (!st.panel_focus)
            Fill(left, top_y, width, height, 0, 0, 0, (see_through ? .25f + .35f * card : .45f) * st.panel_alpha * (1.f - ease));
        if (see_through) {
            const float grow = panel.H() * .03f;
            Fill(panel.l - grow, panel.t - grow, panel.W() + 2 * grow, panel.H() + 2 * grow, .01f, .035f, .07f,
                 card * st.panel_alpha, 4);
        }
        // The file select rebuilt from the game's interface (gui_tree.cpp), live or as last seen live while it fades;
        // other menus, and the file select where the interface cannot be read, as the touch screen's copy
        const bool file_select = !GuiCopy() && (page == P::Page::FileList || page == P::Page::FileOpen ||
                                                page == P::Page::PauseMap || page == P::Page::PauseSamus ||
                                                page == P::Page::MapPins ||
                                                (fit_on && st.in_play && state == P::Bottom::Menu && !map_focus));
        if (panel_live && st.pause_bridge_us < 0) {
            if (file_select && !gui_now.empty()) gui_last = gui_now;
            else gui_last.clear();
        }
        const bool bridging = st.pause_bridge_us >= 0 && !gui_last.empty();
        const std::vector<MsrGuiItem>* rebuilt =
            bridging ? &gui_last
                     : panel_live ? (file_select && !gui_now.empty() ? &gui_now : nullptr) : (gui_last.empty() ? nullptr : &gui_last);
        // a pause page's crossfade: the last one's contents out, then the next one's in (0.12 s each)
        float content = 1.f;
        if (bridging) content = std::max(0.f, 1.f - static_cast<float>(now_us - st.pause_bridge_us) / 120'000.f);
        else if (st.pause_arrive_us >= 0 && now_us - st.pause_arrive_us < 120'000)
            content = static_cast<float>(now_us - st.pause_arrive_us) / 120'000.f;
        // the panel's width past 4:3, in the touch screen's pixels (fitted to a wider window)
        const float panel_s = panel.H() / 240.f;
        const float extra = rebuilt && panel_s > 0.f ? std::max(0.f, panel.W() / panel_s - 320.f) : 0.f;
        if (rebuilt) {
            const uint32_t focus = P::footer_focus.load(std::memory_order_relaxed);
            GuiQuads(*rebuilt, panel.l, panel.t, panel.W(), panel.H(), st.panel_alpha * through, !see_through, -1.f,
                     focus ? focus : P::pause_focus.load(std::memory_order_relaxed), content, extra, seconds);
            st.touch_items = rebuilt;
            st.touch_panel = panel;
            st.touch_extra = extra >= .5f ? extra : 0.f;
        } else {
            Style full;
            full.opacity = st.panel_alpha * through;
            full.over_black = see_through;
            // (a wider panel than 4:3: the copy in its middle, as the game draws it)
            const float cw = std::min(panel.W(), panel.H() * 4.f / 3.f);
            Region({0.f, 0.f, 1.f, 1.f}, panel.l + std::floor((panel.W() - cw) / 2), panel.t, cw, panel.H(), full);
            quads.back().source = panel_live ? 0 : 2;
        }

        // Touch navigation: a glowing frame around the focused button, or a crosshair cursor.
        if (P::touch_mode.load(std::memory_order_relaxed)) {
            // (fitted to the window: by the anchor of the piece under it)
            const float sx = st.touch_extra > 0.f ? panel_s : panel.W() / 320.f, sy = panel.H() / 240.f, py = panel.t;
            float px = panel.l;
            if (st.touch_extra > 0.f && st.touch_items) {
                const bool frame = P::touch_x1 > P::touch_x0 && P::touch_y1 > P::touch_y0;
                const float tx = frame ? (P::touch_x0 + P::touch_x1) / 2.f : static_cast<float>(P::touch_x);
                const float ty = frame ? (P::touch_y0 + P::touch_y1) / 2.f : static_cast<float>(P::touch_y);
                px = AnchorX(0.f, AnchorAt(*st.touch_items, tx, ty), panel.l, panel_s, st.touch_extra);
            }
            const float a = .95f * st.panel_alpha, line = std::max(2.f, 1.25f * sx);
            const int x0 = P::touch_x0, y0 = P::touch_y0, x1 = P::touch_x1, y1 = P::touch_y1;
            if (x1 > x0 && y1 > y0) {
                const float fx = px + x0 * sx - line, fy = py + y0 * sy - line, fw = (x1 - x0) * sx + 2 * line, fh = (y1 - y0) * sy + 2 * line;
                Fill(fx, fy, fw, fh, 1.f, .78f, .2f, .18f * st.panel_alpha);
                Fill(fx, fy, fw, line, 1.f, .78f, .2f, a);
                Fill(fx, fy + fh - line, fw, line, 1.f, .78f, .2f, a);
                Fill(fx, fy, line, fh, 1.f, .78f, .2f, a);
                Fill(fx + fw - line, fy, line, fh, 1.f, .78f, .2f, a);
            } else {
                const float cx = px + P::touch_x * sx, cy = py + P::touch_y * sy, arm = 7.f * sx;
                Fill(cx - arm, cy - line / 2, 2 * arm, line, 1.f, .78f, .2f, a);
                Fill(cx - line / 2, cy - arm, line, 2 * arm, 1.f, .78f, .2f, a);
            }
        }
    }

    out->top[0] = view_top.l; out->top[1] = view_top.t; out->top[2] = view_top.W(); out->top[3] = view_top.H();
    out->panel[0] = panel.l; out->panel[1] = panel.t; out->panel[2] = panel.W(); out->panel[3] = panel.H();
    out->quads = quads.data();
    out->count = static_cast<int>(quads.size());
}

// vk_rasterizer.cpp SkipMsrBackdrop: the title menus open the bottom screen's frame with two full-screen quads (the
// blue scanlined fill, then the emblem) from a 1024x512 I8 atlas; left out, the menus stand on black, which the
// compositor turns into transparency. Called for each draw into the bottom screen's 240x320 target; a display
// transfer starts the next frame (msr_bottom_frame). Out of play only: once a pin is placed, the pause menu's map pins
// tool opens its frames the same way, and left out there its map stood on black, read as a title menu's (half the
// screen dark): the panel turned see-through and touch navigation lost the tool's buttons (2026-11-07, #43).
namespace {
int bottom_draw = -1;
bool backdrop = false;
} // namespace

extern "C" void msr_bottom_frame(void) { bottom_draw = -1; }

extern "C" int msr_skip_bottom_draw(int quad_backdrop) {
    if (++bottom_draw > 1) return 0;
    if (bottom_draw == 0) {
        backdrop = quad_backdrop != 0 && !msr_world_in_play();
        P::backdrop_removed.store(backdrop, std::memory_order_relaxed);
    }
    return backdrop && quad_backdrop;
}

// input_bridge.cpp CycleBeam / ToggleMorphBall: the game switches beams and morphs only by touch, so RT taps the next
// beam slot the HUD shows and LT the minimap (one tap morphs, the next unmorphs), only while the HUD shows. A tap
// holds 100 ms; the bottom screen shows it a few frames later, so for 500 ms the next RT goes on from its target.
// With R held (missiles) the game shows Missile, Super Missile and Power Bomb in those slots, and chooses Super Missiles
// only by a touch there too (2026-11-16, the user: "i cant seem to access super missiles"): RT, or the mouse wheel,
// then taps Missile or Super Missile, whichever is not lit. Power Bombs need no choice: in Morph Ball R lights them.
namespace {
using Clock = std::chrono::steady_clock;
Clock::time_point tap_until{}, beam_settled{}, missile_settled{};
int tap_x = 0, tap_y = 0, beam_target = -1, missile_target = -1;
bool tap_missile = false;   // the tap is a missile choice: it ends when R comes up (the slots are the beams' again)
} // namespace

extern "C" uint32_t input_buttons(void);   // input.c: the buttons as the game gets them (HID bits, R = 0x100)

extern "C" void msr_trigger(int which) {
    static const bool trace = std::getenv("PWR_INPUT_TRACE") != nullptr;
    if (trace) std::fprintf(stderr, "msr: trigger %d (bottom %d, slots %d lit %d, missile lit %d)\n", which, P::bottom.load(),
                            P::beam_slots.load(), P::beam_slot.load(), st.missile_lit);
    if (static_cast<P::Bottom>(P::bottom.load()) != P::Bottom::Hud) return;
    const auto now = Clock::now();
    if (which == 0) {
        tap_x = 130;   // the middle of the map (x 20-240, y 50-205)
        tap_y = 127;
        tap_missile = false;
    } else if (input_buttons() & 0x100) {
        // R held: the missile choice. Until the slots show the missiles (msr_compose reads them), the press waits.
        if (st.missile_lit < 0) {
            st.missile_pending = which;
            return;
        }
        if (st.missile_lit > 1) return;   // Morph Ball: the Power Bombs, nothing to choose
        // Super Missiles found (her inventory; where it cannot tell, the slot's icon)
        const float super = msr_item(0x708738, "ITEM_WEAPON_SUPER_MISSILE_MAX");
        if (super == 0.f || (super < 0.f && !(st.missile_present & 2))) return;
        const int current = now < missile_settled ? missile_target : st.missile_lit;
        const int next = current == 1 ? 0 : 1;
        const auto& [x0, y0, x1, y1] = BC::BeamSlotRects[next];
        tap_x = (x0 + x1) / 2;
        tap_y = (y0 + y1) / 2;
        tap_missile = true;
        missile_target = next;
        missile_settled = now + std::chrono::milliseconds(500);
    } else {
        tap_missile = false;
        const int present = P::beam_slots.load() | 1;   // the usual beam's slot is always there
        if (present == 1) return;
        int current = now < beam_settled ? beam_target : P::beam_slot.load();
        if (current < 0) current = 0;
        const int slots = static_cast<int>(BC::BeamSlotRects.size()), step = which == 2 ? slots - 1 : 1;   // 2: the one before
        int next = current;
        do {
            next = (next + step) % slots;
        } while ((present & (1 << next)) == 0);
        const auto& [x0, y0, x1, y1] = BC::BeamSlotRects[next];
        tap_x = (x0 + x1) / 2;
        tap_y = (y0 + y1) / 2;
        beam_target = next;
        beam_settled = now + std::chrono::milliseconds(500);
    }
    tap_until = now + std::chrono::milliseconds(100);
}

extern "C" int msr_injected_touch(unsigned buttons, int* x, int* y) {
    // touch navigation's finger, held or just lifted (shared clock: steady nanoseconds, as touch_nav.cpp writes)
    const long long now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
    if (P::touch_hold.load(std::memory_order_relaxed) || now_ns < P::touch_tap_until.load(std::memory_order_relaxed)) {
        *x = P::tap_x.load();
        *y = P::tap_y.load();
        return 1;
    }
    if (Clock::now() >= tap_until) return 0;
    if (tap_missile && !(buttons & 0x100)) return 0;   // R up: the slot under the finger is a beam's now
    *x = tap_x;
    *y = tap_y;
    return 1;
}

// The file select's footer on the D-pad, the input thread's half (input.c, each update, after the circle pad's
// directions are set). While a file is open with its footer lit, Up and Down (and the stick) are this list's: the
// game's choices, then Copy, round to the first choice past it (Up the other way), a step a press, repeating while held
// (350 ms, then every 120 ms). On a choice it presses Up or Down for the game, as many as the game needs to get there
// (the game takes a 50 ms press, one choice a press, and wraps round: from Copy one Down is the first choice), 110 ms
// apart; on the footer the game keeps its choice. The game's choice is counted here while the presses go (the interface
// shows a new choice only after its animation: waiting on it, presses were spent twice and went round past it) and taken
// from the interface again once all is still for 0.4 s. Right and Left go between Copy and Delete on the footer, A taps
// the one chosen (an injected touch; returns 1 while it holds, at x, y). Buttons kept from the game stay kept until
// let go (else the game took them as new presses). P::footer_focus: the chosen footer icon's name hash (GuiQuads).
namespace {
uint32_t footer_prev = 0, footer_held = 0;
bool footer_stick = false;     // the circle pad was kept from the game: until it is centered
bool footer_active = false;    // the list is in use (else it follows the game)
bool footer_on = false;        // the list is on the footer
int footer_game = 0;           // the game's choice, as counted
int footer_queue = 0;          // presses still to give the game (+ Down, - Up)
int footer_held_dir = 0;       // the direction held
Clock::time_point footer_repeat{}, footer_press_until{}, footer_next_press{}, footer_still{}, footer_tap_until{},
    footer_unready{};
int footer_press_dir = 0;
int footer_tap_x = 0, footer_tap_y = 0;
} // namespace

// The pause menu's D-pad, the input thread's half (msr_footer_input calls it first): on the map page Left and Right go
// along the toolbar (wrapping round), A taps the button chosen (an injected touch: returns 1 while it holds, at x, y),
// and the D-pad's directions reach the game no more (the stick still moves the map). The page's own button is chosen
// when the page shows. P::pause_focus: the chosen button's icon name hash (GuiQuads lights it).
namespace {
int pause_focus_index = -1, pause_page = 0;
uint32_t pause_prev = 0, pause_held = 0;
Clock::time_point pause_tap_until{}, pause_drag_last{};
int pause_tap_x = 0, pause_tap_y = 0;
bool pause_dragging = false;
Clock::time_point pause_left{};
int pause_left_page = 0;
float pause_drag_x = 0.f, pause_drag_y = 0.f;
// the map pins tool: on the bar (0) or on the pins placed on the map (1); the map's pin chosen (its name, where it was);
// a pin being removed, by a finger taking it to the bar since then (from x, y)
int pause_row = 0;
uint32_t pause_placed_name = 0;
int pause_placed_x = 0, pause_placed_y = 0;
bool pause_removing = false;
Clock::time_point pause_remove_start{};
float pause_remove_x = 0.f, pause_remove_y = 0.f;
// a pin picked up with A (its finger held until A again; B takes it back to where it was taken from, holds it there
// until then, and lifts it); a drag's pause screen gone since (0: shown)
bool pause_grabbed = false;
float pause_grab_x = 0.f, pause_grab_y = 0.f;
Clock::time_point pause_putback_until{}, pause_gone_since{};
// the stick as the D-pad on the map pins tool: its direction (0 none) and when it steps again, held
uint32_t pause_stick_dir = 0;
Clock::time_point pause_stick_next{};
// the map pins tool's bar choice, and when the tool last showed (a pin dropped, the reading is another page a moment)
int pause_pins_index = 0;
Clock::time_point pause_pins_seen{};
// the map's view as last read (a pin's finger stays in it)
std::array<float, 4> pause_view{};
bool pause_view_known = false;
std::atomic<bool> zl_held{false};   // LT (ZL) held: video.c msr_trigger_hold
} // namespace

// video.c: ZL and ZR (LT and RT by default) pressed and let go (msr_trigger takes the presses)
extern "C" void msr_trigger_hold(int which, int down) {
    if (which == 0) zl_held.store(down != 0, std::memory_order_relaxed);
}

int PauseInput(uint32_t* buttons, uint32_t dpad, int* take, int* cx, int* cy, int* x, int* y) {
    constexpr uint32_t A = 1u, Right = 0x10u, Left = 0x20u, Up = 0x40u, Down = 0x80u, Y = 0x800u;
    PauseState state;
    {
        std::lock_guard<std::mutex> guard(pause_lock);
        state = pause_shared;
    }
    // the buttons, with the D-pad as the menus mean it (input.c: its own, or the circle pad's digital directions)
    const uint32_t b = (*buttons & ~0xF0u) | dpad, pressed = b & ~pause_prev;
    pause_prev = b;
    pause_held &= b;
    const auto now = Clock::now();
    const bool tapping = now < pause_tap_until;
    // A drag goes on until LT is let go (or A places a pin picked up with it), whatever shows meanwhile (the bar changes
    // under a pin taken up, and the touch screen's reading with it: the page seemed to end, the drag stopped and the
    // D-pad went back to the game). Not past the pause screen: gone 0.5 s (back to play), the finger is lifted.
    if (pause_dragging || pause_removing || pause_putback_until.time_since_epoch().count()) {
        if (static_cast<P::Bottom>(P::bottom.load(std::memory_order_relaxed)) != P::Bottom::Menu) {
            if (!pause_gone_since.time_since_epoch().count()) {
                pause_gone_since = now;
            } else if (now - pause_gone_since > std::chrono::milliseconds(500)) {
                pause_dragging = pause_grabbed = pause_removing = false;
                pause_putback_until = pause_gone_since = {};
                return 0;
            }
        } else {
            pause_gone_since = {};
        }
        state.page = 2;
    }
    // a page shows: its own button chosen, unless it is the one just left (a moment's change of the reading)
    if (state.page != pause_page && !tapping) {
        static const bool trace = std::getenv("PWR_MSR_TRACE") != nullptr;
        if (trace) std::fprintf(stderr, "msr: pause D-pad page %d (%d buttons)%s\n", state.page, state.count,
                                state.page == 1 || state.page == 2 ? ": the D-pad is the port's" : "");
        if (state.page == 0) {
            pause_left = now;
            pause_left_page = pause_page;
        } else if (state.page == 2 && now - pause_pins_seen < std::chrono::seconds(2)) {
            pause_focus_index = pause_pins_index;   // (the pins tool again after a moment's other reading)
        } else if (!(state.page == pause_left_page && now - pause_left < std::chrono::milliseconds(1500))) {
            pause_focus_index = state.current;
        }
        pause_page = state.page;
    }
    uint32_t focus = 0;
    if (state.page == 1 && state.count > 0) {
        pause_focus_index = std::clamp(pause_focus_index, 0, state.count - 1);
        if (!tapping) {
            if (pressed & Right) pause_focus_index = (pause_focus_index + 1) % state.count;
            if (pressed & Left) pause_focus_index = (pause_focus_index + state.count - 1) % state.count;
            if (pressed & A) {
                pause_tap_x = state.buttons[pause_focus_index].x;
                pause_tap_y = state.buttons[pause_focus_index].y;
                pause_tap_until = now + std::chrono::milliseconds(150);
                pause_held |= A;
            }
        }
        focus = state.buttons[pause_focus_index].icon;
        *take |= 1;   // the D-pad is the toolbar's here (input.c keeps it from the game; the stick still moves the map)
    } else if (state.page == 2 && (state.count > 0 || state.placed_count > 0 || pause_dragging || pause_removing ||
                                   pause_putback_until.time_since_epoch().count())) {
        // The map pins tool (the user: "the user can cycle the pin selection with the dpad, once they select one they
        // can drag it around the map with the joystick. holding the L2 (LT) buttons allows them to drag the marker
        // around the map"): Left and Right choose a pin; LT held puts a finger on it and the stick moves the finger
        // (160 pixels a second at full tilt, the stick kept from the game meanwhile), and letting go of LT lifts it,
        // where the game places the pin (or, back on the bar, takes it off the map).
        // Then (2026-11-17, the user: "for markers, devise a way to delete the markers placed using only a controller"):
        // Up goes to the pins placed on the map, Left and Right choose one there (left to right), Down goes back to the
        // bar. Y (the left face button: X, Square) removes the one chosen, as a finger would: it takes the pin to the bar,
        // where the game takes pins off the map (its DeleteComposition). LT held moves it as a bar pin is moved. The bar
        // ends with the tool's Delete all button, which A taps (the game asks first).
        // Then (2026-11-17, the user: "they use the circle stick or dpad to go to different markers ... Then they use A
        // to select the marker"): the stick chooses as the D-pad does (past 0x60 of 0x9C, again after 0.4 s held and
        // every 0.15 s after; not while the D-pad is pressed, which under the right stick option comes as the circle
        // pad's too), and the map's pins are chosen by where they lie (the nearest that way; Down past the lowest: the
        // bar). A picks the pin chosen up (its finger down on it), the stick (or the D-pad) moves it, and A again lifts
        // it where it is, as LT let go does; B instead takes it back to where it was taken from (a bar pin to the bar,
        // a map pin to its place) and lifts it there.
        constexpr uint32_t B = 2u;
        const int bar = state.count + (state.delete_all.icon ? 1 : 0);
        // (not while a pin is held, nor while the bar's pins are not shown: hidden under a pin held, coming back after
        // one is dropped; the one chosen would be lost)
        const bool held_pin = pause_dragging || pause_removing || pause_putback_until.time_since_epoch().count();
        const bool bar_shown = state.count > 0 && !held_pin;
        if (bar_shown) pause_focus_index = std::clamp(pause_focus_index, 0, bar - 1);
        const bool hold = zl_held.load(std::memory_order_relaxed);
        if (pause_putback_until.time_since_epoch().count()) {   // B: the finger where the pin was, 0.1 s, lifted
            *take |= 3;
            *buttons &= ~pause_held;
            if (now < pause_putback_until) {
                P::pause_focus.store(0, std::memory_order_relaxed);
                *x = static_cast<int>(pause_grab_x);
                *y = static_cast<int>(pause_grab_y);
                return 1;
            }
            pause_putback_until = {};
            return 0;
        }
        // the D-pad's presses, and the stick's
        uint32_t nav = pressed & (Right | Left | Up | Down);
        if (!pause_dragging) {
            uint32_t dir = 0;
            if (!(b & 0xF0u)) {
                const int ax = std::abs(*cx), ay = std::abs(*cy);
                if (std::max(ax, ay) > (pause_stick_dir ? 0x30 : 0x60)) dir = ax >= ay ? (*cx > 0 ? Right : Left) : (*cy > 0 ? Up : Down);
            }
            if (dir && dir != pause_stick_dir) {
                nav |= dir;
                pause_stick_next = now + std::chrono::milliseconds(400);
            } else if (dir && now >= pause_stick_next) {
                nav |= dir;
                pause_stick_next = now + std::chrono::milliseconds(150);
            }
            pause_stick_dir = dir;
            *take |= 2;   // the stick chooses pins here, not the map's view
        } else {
            pause_stick_dir = 0;
        }
        bool grabbed_now = false;
        if (pause_removing) {
            // a finger on the pin 0.15 s, to the bar in 0.25 s, held there 0.1 s, lifted
            const float t = std::chrono::duration<float>(now - pause_remove_start).count();
            constexpr float to_x = 160.f, to_y = 228.f;
            const float k = std::clamp((t - .15f) / .25f, 0.f, 1.f), e = k * k * (3.f - 2.f * k);
            *take |= 3;
            *buttons &= ~pause_held;
            if (t < .5f) {
                P::pause_focus.store(0, std::memory_order_relaxed);
                *x = static_cast<int>(pause_remove_x + (to_x - pause_remove_x) * e);
                *y = static_cast<int>(pause_remove_y + (to_y - pause_remove_y) * e);
                return 1;
            }
            pause_removing = false;   // lifted (this update has no finger)
            return 0;
        }
        if (pause_row == 1 && state.placed_count == 0 && !pause_dragging) pause_row = 0;
        int placed_index = -1;
        if (pause_row == 1 && state.placed_count > 0) {
            for (int k = 0; k < state.placed_count; ++k)
                if (state.placed[k].icon == pause_placed_name) placed_index = k;
            if (placed_index < 0) {   // gone (removed, moved, out of view): the nearest to where it was
                int best = 1 << 30;
                for (int k = 0; k < state.placed_count; ++k) {
                    const int dx = state.placed[k].x - pause_placed_x, dy = state.placed[k].y - pause_placed_y;
                    if (dx * dx + dy * dy < best) { best = dx * dx + dy * dy; placed_index = k; }
                }
            }
        }
        if (!pause_dragging && pause_row == 0 && bar_shown) {
            if (bar > 0 && (nav & Right)) pause_focus_index = (pause_focus_index + 1) % bar;
            if (bar > 0 && (nav & Left)) pause_focus_index = (pause_focus_index + bar - 1) % bar;
            const bool on_delete_all = pause_focus_index >= state.count && state.delete_all.icon;
            if ((nav & Up) && state.placed_count > 0) {   // to the map's pins: the one nearest above
                pause_row = 1;
                const int from = on_delete_all ? state.delete_all.x : state.count > 0 ? state.buttons[pause_focus_index].x : 160;
                int best = 1 << 30;
                for (int k = 0; k < state.placed_count; ++k)
                    if (std::abs(state.placed[k].x - from) < best) { best = std::abs(state.placed[k].x - from); placed_index = k; }
            } else if (on_delete_all && (pressed & A)) {
                pause_tap_x = state.delete_all.x;
                pause_tap_y = state.delete_all.y;
                pause_tap_until = now + std::chrono::milliseconds(150);
                pause_held |= A;
            } else if (!on_delete_all && state.count > 0 && ((pressed & A) || hold)) {
                pause_dragging = true;
                pause_grabbed = grabbed_now = !hold;
                pause_drag_x = pause_grab_x = static_cast<float>(state.buttons[pause_focus_index].x);
                pause_drag_y = pause_grab_y = static_cast<float>(state.buttons[pause_focus_index].y);
                pause_drag_last = now;
                if (pause_grabbed) pause_held |= A;
            }
        } else if (!pause_dragging && pause_row == 1 && placed_index >= 0) {
            // the nearest pin the way pressed (along it, and twice what it lies aside); Down past the lowest: the bar
            if (nav) {
                const PauseButton& from = state.placed[placed_index];
                int best = -1, best_score = 1 << 30;
                for (int k = 0; k < state.placed_count; ++k) {
                    if (k == placed_index) continue;
                    const int dx = state.placed[k].x - from.x, dy = state.placed[k].y - from.y;
                    const bool across = (nav & (Right | Left)) != 0;
                    const int along = nav & Right ? dx : nav & Left ? -dx : nav & Up ? -dy : dy;
                    const int score = along + 2 * std::abs(across ? dy : dx);
                    if (along > 0 && score < best_score) {
                        best_score = score;
                        best = k;
                    }
                }
                if (best >= 0) placed_index = best;
                else if (nav & Down) pause_row = 0;
            }
            const PauseButton& pin = state.placed[placed_index];
            if (pause_row == 1 && (pressed & Y)) {
                pause_removing = true;
                pause_remove_start = now;
                pause_remove_x = static_cast<float>(pin.x);
                pause_remove_y = static_cast<float>(pin.y);
                pause_held |= Y;
            } else if (pause_row == 1 && ((pressed & A) || hold)) {
                pause_dragging = true;
                pause_grabbed = grabbed_now = !hold;
                pause_drag_x = pause_grab_x = static_cast<float>(pin.x);
                pause_drag_y = pause_grab_y = static_cast<float>(pin.y);
                pause_drag_last = now;
                if (pause_grabbed) pause_held |= A;
            }
            pause_placed_name = pin.icon;
            pause_placed_x = pin.x;
            pause_placed_y = pin.y;
        }
        if (pause_dragging) {
            const float seconds = std::min(.1f, std::chrono::duration<float>(now - pause_drag_last).count());
            pause_drag_last = now;
            float sx = std::abs(*cx) > 0x10 ? static_cast<float>(*cx) / 0x9C : 0.f;
            float sy = std::abs(*cy) > 0x10 ? static_cast<float>(*cy) / 0x9C : 0.f;
            if (pause_grabbed && sx == 0.f && sy == 0.f) {   // (the D-pad moves a pin picked up with A too)
                sx = static_cast<float>(((b & Right) != 0) - ((b & Left) != 0));
                sy = static_cast<float>(((b & Up) != 0) - ((b & Down) != 0));
            }
            // The finger stays in the map's view (6 pixels in; a bar pin rises into it from the bar), and the stick
            // pushed past its edge goes to the game, which moves the map under the pin as its stick moves it on the
            // pause screen (2026-11-17, the user: "allow the circle stick to move the pin around the whole map of the
            // area. So when you drag it towards a corner it drags to that area. Make this smooth like it would be on
            // the regular pause screen"); the rest of the stick is kept from it
            if (state.view_known) {
                pause_view = state.view;
                pause_view_known = true;
            }
            float x0 = 1.f, x1 = 318.f, y0 = 31.f, y1 = 238.f;
            if (pause_view_known) {
                x0 = std::max(pause_view[0], 0.f) + 6.f;
                x1 = std::min(pause_view[2], 320.f) - 6.f;
                y0 = pause_view[1] + 6.f;
                y1 = pause_view[3] - 6.f;
            }
            const bool inside = pause_drag_y <= y1 + .5f;
            const float nx = pause_drag_x + sx * 160.f * seconds, ny = pause_drag_y - sy * 160.f * seconds;
            const bool past_x = (nx > x1 && sx > 0.f) || (nx < x0 && sx < 0.f);
            const bool past_y = inside && ((ny < y0 && sy > 0.f) || (ny > y1 && sy < 0.f));
            pause_drag_x = std::clamp(nx, x0, x1);
            pause_drag_y = std::clamp(ny, y0, inside ? y1 : std::min(pause_drag_y, 238.f));
            if (past_x || past_y) {
                *cx = past_x ? static_cast<int>(sx * 0x9C) : 0;
                *cy = past_y ? static_cast<int>(sy * 0x9C) : 0;
                *take = (*take & ~2) | 1 | 4;   // (the D-pad kept, the stick as given)
            } else {
                *take |= 3;   // the stick moves the finger, not the map
            }
            if (pause_grabbed && !grabbed_now && (pressed & B)) {   // B: back where it was taken from, then lifted
                pause_dragging = pause_grabbed = false;
                pause_putback_until = now + std::chrono::milliseconds(100);
                pause_held |= B;
                P::pause_focus.store(0, std::memory_order_relaxed);
                *buttons &= ~pause_held;
                *x = static_cast<int>(pause_grab_x);
                *y = static_cast<int>(pause_grab_y);
                return 1;
            }
            // placed: A again for a pin picked up with it, LT let go for one dragged with LT (lifted: this update has
            // no finger); a map pin moved is looked for there
            if (pause_grabbed ? !grabbed_now && (pressed & A) : !hold) {
                if (pause_grabbed) pause_held |= A;
                pause_dragging = pause_grabbed = false;
                if (pause_row == 1) {
                    pause_placed_name = 0;
                    pause_placed_x = static_cast<int>(pause_drag_x);
                    pause_placed_y = static_cast<int>(pause_drag_y);
                }
            }
        }
        if (pause_row == 1) focus = pause_placed_name;
        else if (!bar_shown) focus = P::pause_focus.load(std::memory_order_relaxed);   // (the bar hidden: as it was)
        else if (pause_focus_index >= state.count && state.delete_all.icon) focus = state.delete_all.icon;
        else focus = pause_focus_index >= 0 && pause_focus_index < state.count ? state.buttons[pause_focus_index].icon
                                                                                : P::pause_focus.load(std::memory_order_relaxed);
        if (pause_removing) focus = 0;
        pause_pins_seen = now;
        if (bar_shown && !pause_dragging) pause_pins_index = pause_focus_index;
        *take |= 1;
        if (pause_removing) {   // (began this update)
            P::pause_focus.store(0, std::memory_order_relaxed);
            *buttons &= ~pause_held;
            *x = static_cast<int>(pause_remove_x);
            *y = static_cast<int>(pause_remove_y);
            return 1;
        }
        if (pause_dragging) {
            P::pause_focus.store(focus, std::memory_order_relaxed);
            *buttons &= ~pause_held;
            *x = static_cast<int>(pause_drag_x);
            *y = static_cast<int>(pause_drag_y);
            return 1;
        }
    }
    P::pause_focus.store(focus, std::memory_order_relaxed);
    *buttons &= ~pause_held;
    if (now < pause_tap_until) {
        *buttons &= ~A;
        *x = pause_tap_x;
        *y = pause_tap_y;
        return 1;
    }
    return 0;
}

extern "C" int msr_footer_input(uint32_t* buttons, uint32_t dpad, int* take, int* cx, int* cy, int* x, int* y) {
    if (PauseInput(buttons, dpad, take, cx, cy, x, y)) return 1;
    constexpr uint32_t A = 1u, Right = 0x10u | (1u << 28), Left = 0x20u | (1u << 29), Up = 0x40u | (1u << 30),
                       Down = 0x80u | (1u << 31), Pad = 0xF0000000u;
    const uint32_t b = *buttons, pressed = b & ~footer_prev;
    footer_prev = b;
    footer_held &= b;
    if (std::abs(*cx) <= 0x28 && std::abs(*cy) <= 0x28) footer_stick = false;
    const auto now = Clock::now();
    const bool tapping = now < footer_tap_until;
    const int choices = footer_choices.load(std::memory_order_relaxed), game = footer_game_choice.load(std::memory_order_relaxed);
    // ready, or not for long: the interface shows no choice for a frame or two as it animates
    bool ready = footer_ready.load(std::memory_order_relaxed) && choices > 0 && game >= 0;
    if (ready) footer_unready = {};
    else if (footer_active && footer_unready.time_since_epoch().count() == 0) footer_unready = now;
    if (!ready && footer_active && now - footer_unready < std::chrono::milliseconds(300)) ready = true;
    uint32_t focus = P::footer_focus.load(std::memory_order_relaxed);
    if (!tapping && footer_tap_until.time_since_epoch().count()) {   // a tap ended: the game has it now
        footer_tap_until = {};
        footer_active = footer_on = false;
        focus = 0;
    }
    if (!ready && !tapping) {
        footer_active = footer_on = false;
        footer_queue = 0;
        footer_held_dir = 0;
        footer_press_until = {};
        focus = 0;
    }
    if (ready && !tapping) {
        const int n = std::max(choices, 1);
        if (!footer_active) footer_game = std::clamp(game, 0, n - 1);
        // a step: a press, or the held direction repeating
        const int dir = (b & Down) ? 1 : (b & Up) ? -1 : 0;
        int step = 0;
        if (dir && dir != footer_held_dir) {
            step = dir;
            footer_repeat = now + std::chrono::milliseconds(350);
        } else if (dir && now >= footer_repeat) {
            step = dir;
            footer_repeat = now + std::chrono::milliseconds(120);
        }
        footer_held_dir = dir;
        if (step) {
            const int at = footer_on ? n : footer_game;
            const int next = (at + step + n + 1) % (n + 1);
            footer_active = true;
            footer_on = next == n;
            focus = footer_on ? CopyIcon : 0;
            if (!footer_on) {   // the game's presses: the shorter way round
                int d = (next - footer_game + n) % n;
                if (d > n / 2) d -= n;
                footer_queue += d;
                footer_game = next;
            }
            footer_still = now;
        }
        if (footer_on) {
            if (pressed & Left) focus = CopyIcon;
            if (pressed & Right) focus = EraseIcon;
            if (pressed & A) {
                const bool copy = focus == CopyIcon;
                footer_tap_x = copy ? footer_copy_x.load() : footer_erase_x.load();
                footer_tap_y = copy ? footer_copy_y.load() : footer_erase_y.load();
                footer_tap_until = now + std::chrono::milliseconds(150);
            }
            footer_held |= b & (A | Left | Right);
        }
        // the presses, one at a time
        if (footer_queue && now >= footer_next_press) {
            footer_press_dir = footer_queue > 0 ? 1 : -1;
            footer_queue -= footer_press_dir;
            footer_press_until = now + std::chrono::milliseconds(50);
            footer_next_press = now + std::chrono::milliseconds(110);
            footer_still = now;
        }
        // all still for a while, off the footer: the game's own choice again (a press it did not take, the mouse)
        if (footer_active && !footer_on && !footer_queue && !dir && now - footer_still > std::chrono::milliseconds(400))
            footer_active = false;
        footer_held |= b & (Up | Down);
        if (b & Pad & (Up | Down)) footer_stick = true;
    }
    P::footer_focus.store(focus, std::memory_order_relaxed);
    if (focus || now < footer_tap_until) *buttons &= ~(A | Left | Right | Up | Down);
    *buttons &= ~footer_held;
    if (footer_stick || now < footer_tap_until) {
        *cx = 0;
        *cy = 0;
        *buttons &= ~Pad;
    }
    if (ready && now < footer_press_until) *buttons |= footer_press_dir > 0 ? 0x80u : 0x40u;   // the list's own press
    if (now < footer_tap_until) {
        *x = footer_tap_x;
        *y = footer_tap_y;
        return 1;
    }
    return 0;
}

// ---- touch navigation (src/msr/touch_nav.cpp, as the emulator build's input_bridge.cpp drives it): Tab, or L3 while
// a menu shows, puts a highlight on the touch screen's buttons (or a free cursor) that the D-pad, arrows or left
// stick move; the right face button, Return or Space is the finger (hold to drag); the bottom face button or Escape
// leaves. The game gets no other input meanwhile, nor until the button that closed it is released. ----
namespace {
MSR::TouchNavigator touch;
int stick_dx = 0, stick_dy = 0;
Clock::time_point stick_repeat{};
Clock::time_point last_touch_frame{};
} // namespace

extern "C" int msr_touch_active(void) { return touch.Active(); }
extern "C" void msr_touch_toggle(void) { touch.SetActive(!touch.Active()); }
extern "C" void msr_touch_step(int dx, int dy) { if (touch.Active()) touch.Step(dx, dy); }
extern "C" void msr_touch_finger(int down) { if (!touch.Active()) return; if (down) touch.Press(); else touch.Release(); }
extern "C" void msr_touch_close(void) { if (touch.Active()) touch.SetActive(false); }

// Each frame: the screen's buttons followed, the stick stepping between them (repeating while held) or moving the
// free point; and the game's input given back once nothing is held after touch navigation closed.
extern "C" void msr_touch_frame(float x, float y, int any_held) {
    const auto now = Clock::now();
    const float seconds = last_touch_frame.time_since_epoch().count() ? std::min(.1f, std::chrono::duration<float>(now - last_touch_frame).count()) : 0.f;
    last_touch_frame = now;
    if (!touch.Active()) {
        // the settings overlay keeps the game's input while it shows (overlay_host.cpp)
        if (P::block_game_input.load() && !any_held && !msr_overlay_visible()) P::block_game_input = false;
        stick_dx = stick_dy = 0;
        return;
    }
    touch.Update();
    if (touch.Snapping()) {
        const int sx = x > .5f ? 1 : x < -.5f ? -1 : 0, sy = y > .5f ? -1 : y < -.5f ? 1 : 0;
        if (sx != stick_dx || sy != stick_dy) {
            stick_dx = sx;
            stick_dy = sy;
            if (sx || sy) {
                touch.Step(sx, sy);
                stick_repeat = now + std::chrono::milliseconds(350);
            }
        } else if ((sx || sy) && now >= stick_repeat) {
            touch.Step(sx, sy);
            stick_repeat = now + std::chrono::milliseconds(140);
        }
    } else {
        touch.Glide(x, y, seconds);
    }
}
extern "C" int msr_input_blocked(void) { return P::block_game_input.load(std::memory_order_relaxed); }
extern "C" int msr_station_map(void) { return station_map.load(std::memory_order_relaxed) ? 1 : 0; }

