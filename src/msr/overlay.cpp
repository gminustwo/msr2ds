// Copyright 2026 Samus Returns port
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <span>
#include <utility>
#ifndef MSR_NATIVE
#include <QSettings>
#endif
#include <imgui.h>
#include <imgui_internal.h>
#include <backends/imgui_impl_vulkan.h>
#ifdef MSR_OPENGL
#include <backends/imgui_impl_opengl3.h>
#endif
#include "common/logging/log.h"
#include "msr/button_glyphs.h"
#include "msr/cheats.h"
#include "msr/frame_rate.h"
#include "msr/overlay.h"
#include "msr/presentation.h"

namespace MSR {
namespace {

double Now() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

struct LoaderData {
    VkInstance instance;
    PFN_vkGetInstanceProcAddr get_instance_proc_addr;
};

PFN_vkVoidFunction LoadVulkanFunction(const char* name, void* user_data) {
    const auto* data = static_cast<const LoaderData*>(user_data);
    return data->get_instance_proc_addr(data->instance, name);
}

void CheckResult(VkResult result) {
    if (result != VK_SUCCESS) {
        LOG_ERROR(Frontend, "Settings overlay Vulkan error {}", static_cast<int>(result));
    }
}

/// A control reminder: the game's buttons and the port's controls as tokens ({A}, {Touch}...),
/// named for the device in use (msr/button_glyphs.h Keys: the pad's labels or the keyboard's keys;
/// in the emulator-based build the 3DS names as the game's icons show them, and both devices').
struct ControlHint {
    const char* keys;
    const char* action;
};

// The game offers the map tools, file copy and delete only by touch (its buttons do nothing
// there); touch mode reaches them. (msr-native shows no help text beside its menus, 2026-11-17, the user: "we no longer
// need but I do want them to be re-worked": its D-pad works their touch buttons, and the pause map shows its buttons'
// icons on it, compositor.cpp PauseExtras; only touch mode's own controls are named.)
constexpr ControlHint PauseMapHints[] = {
    {"{R}", "Samus page"},
    {"{B}", "Back to the game"},
    {"{Touch}", "Touch mode: scan, map pins, info, settings"},
};
constexpr ControlHint PauseSamusHints[] = {
    {"{Stick}", "Choose an item"},
    {"{A}", "Read more"},
    {"{L}", "Map"},
    {"{B}", "Back to the game"},
};
constexpr ControlHint MapPinsHints[] = {
    {"{Touch}", "Touch mode: move the pins"},
    {"{B}", "Back to the map"},
};
constexpr ControlHint FileListHints[] = {
    {"{A}", "Open a file"},
    {"{Touch}", "Touch mode: copy or delete an open file"},
};
constexpr ControlHint FileOpenHints[] = {
    {"{Touch}", "Touch mode: copy or delete"},
    {"{B}", "Back to the files"},
};
constexpr ControlHint TouchHints[] = {
    {"{Move}", "Move"},
    {"{Tap}", "Tap; hold to drag"},
    {"{Leave}", "Leave touch mode"},
};
constexpr ControlHint MapPinsTouchHints[] = {
    {"{Move}", "Move"},
    {"Up", "The pins on the map (down: the buttons again)"},
    {"Hold {Tap} on a pin", "Drag it to the map, or to the bin to remove it"},
    {"{Leave}", "Leave touch mode"},
};
constexpr std::span<const ControlHint> HintSets[] = {
    PauseMapHints, PauseSamusHints, MapPinsHints, FileListHints,
    FileOpenHints, TouchHints,      MapPinsTouchHints,
};
enum HintSet { PauseMap, PauseSamus, MapPins, FileList, FileOpen, Touch, MapPinsTouch };

/// The menu's look (2026-11-14, the user: "revamp the menu system. make it look more modern like we did for metroid
/// other m, star wars rogue squadron 2, castlevania rondo of blood"): Other M's visor HUD (otherm-recomp hud_theme.cpp)
/// in Samus Returns' cyan, orange marking what is chosen: a dark glass panel with cut corners, a glowing cyan edge,
/// orange brackets on its two whole corners and faint scan lines, Bahnschrift, and switches for the on/off rows.
/// Every color here is multiplied by `alpha`: the menu fades in (DrawMenu).
namespace Theme {
constexpr ImU32 Cyan = IM_COL32(80, 220, 255, 255);
constexpr ImU32 Orange = IM_COL32(255, 150, 40, 255);
constexpr ImU32 TextDim = IM_COL32(130, 175, 205, 255);
constexpr ImU32 PanelTop = IM_COL32(10, 30, 54, 255);   // (opaque: in HDR the game behind is far brighter than the glass)
constexpr ImU32 PanelBottom = IM_COL32(3, 10, 24, 255);
ImFont* title_font = nullptr;   // the UI font again at twice the size: the header stays sharp at 4K (Overlay())

ImU32 Fade(ImU32 col, float alpha) {
    const float a = static_cast<float>((col >> IM_COL32_A_SHIFT) & 0xFF) * std::clamp(alpha, 0.f, 1.f);
    return (col & ~IM_COL32_A_MASK) | (static_cast<ImU32>(a + .5f) << IM_COL32_A_SHIFT);
}
/// The six corners of a box from `a` to `b` with its top-left and bottom-right corners cut by `cut`.
void ChamferPoints(ImVec2 a, ImVec2 b, float cut, ImVec2 out[6]) {
    cut = std::min(cut, std::min(b.x - a.x, b.y - a.y) * .5f);
    out[0] = {a.x + cut, a.y};
    out[1] = {b.x, a.y};
    out[2] = {b.x, b.y - cut};
    out[3] = {b.x - cut, b.y};
    out[4] = {a.x, b.y};
    out[5] = {a.x, a.y + cut};
}
/// A panel of that shape: a vertical gradient, an edge with a soft glow outside it.
void Panel(ImDrawList* draw, ImVec2 a, ImVec2 b, float cut, ImU32 top, ImU32 bottom, ImU32 edge, float thickness) {
    if (b.x - a.x < 2.f || b.y - a.y < 2.f) return;
    ImVec2 pts[6];
    ChamferPoints(a, b, cut, pts);
    const int start = draw->VtxBuffer.Size;
    draw->AddConvexPolyFilled(pts, 6, top);
    ImGui::ShadeVertsLinearColorGradientKeepAlpha(draw, start, draw->VtxBuffer.Size, a, {a.x, b.y}, top, bottom);
    for (int i = 3; i >= 1; --i) {   // the glow: wider and fainter outward
        draw->AddPolyline(pts, 6, Fade(edge, .07f * static_cast<float>(4 - i)), ImDrawFlags_Closed, thickness * (1.f + 2.5f * i));
    }
    draw->AddPolyline(pts, 6, edge, ImDrawFlags_Closed, thickness);
}
/// Horizontal lines every `step` pixels inside such a panel: the visor's scan lines.
void Scanlines(ImDrawList* draw, ImVec2 a, ImVec2 b, float cut, float step, ImU32 col) {
    step = std::max(2.f, step);
    for (float y = std::floor(a.y) + .5f; y < b.y; y += step) {
        const float l = a.x + std::max(0.f, cut - (y - a.y)), r = b.x - std::max(0.f, cut - (b.y - y));
        if (r > l) draw->AddLine({l, y}, {r, y}, col, 1.f);
    }
}
/// L-shaped brackets just outside the panel's whole corners (top right, bottom left).
void CornerBrackets(ImDrawList* draw, ImVec2 a, ImVec2 b, float length, ImU32 col, float thickness) {
    const float o = thickness * 2.5f;
    const ImVec2 tr[3] = {{b.x - length, a.y - o}, {b.x + o, a.y - o}, {b.x + o, a.y + length}};
    const ImVec2 bl[3] = {{a.x - o, b.y - length}, {a.x - o, b.y + o}, {a.x + length, b.y + o}};
    draw->AddPolyline(tr, 3, col, ImDrawFlags_None, thickness);
    draw->AddPolyline(bl, 3, col, ImDrawFlags_None, thickness);
}
/// Text `px` high with `tracking` pixels between letters, its left middle at `pos`; a `glow` haloes it. Its width.
float Text(ImDrawList* draw, ImFont* font, float px, ImVec2 pos, ImU32 col, const char* text, float tracking,
           ImU32 glow = 0) {
    if (!font) font = ImGui::GetFont();
    const float h = font->CalcTextSizeA(px, FLT_MAX, 0.f, "A").y;
    const auto run = [&](ImVec2 at, ImU32 c) {
        float x = at.x;
        for (const char* p = text; *p; ++p) {
            draw->AddText(font, px, {std::floor(x + .5f), std::floor(at.y - h * .5f + .5f)}, c, p, p + 1);
            x += font->CalcTextSizeA(px, FLT_MAX, 0.f, p, p + 1).x + tracking;
        }
        return x - tracking - at.x;
    };
    if (glow) {
        const float r = std::max(1.f, px * .07f);
        for (int i = 0; i < 8; ++i) {
            const float t = static_cast<float>(i) * .7854f;
            run({pos.x + std::cos(t) * r, pos.y + std::sin(t) * r}, glow);
        }
    }
    return run(pos, col);
}
/// The width Text would draw.
float Measure(ImFont* font, float px, const char* text, float tracking) {
    if (!font) font = ImGui::GetFont();
    float w = 0.f;
    for (const char* p = text; *p; ++p) w += font->CalcTextSizeA(px, FLT_MAX, 0.f, p, p + 1).x + (p[1] ? tracking : 0.f);
    return w;
}
/// A line fading out to both ends, `y` high, from `x0` to `x1`.
void Rule(ImDrawList* draw, float x0, float x1, float y, ImU32 col) {
    const float mid = (x0 + x1) * .5f;
    const ImU32 clear = Fade(col, 0.f);
    draw->AddRectFilledMultiColor({x0, y}, {mid, y + 1.f}, clear, col, col, clear);
    draw->AddRectFilledMultiColor({mid, y}, {x1, y + 1.f}, col, clear, clear, col);
}
} // namespace Theme

/// The menu's ImGui colors, in Theme's scheme: dark glass, cyan lines, orange for what is chosen or focused.
void ApplyStyle() {
    ImGui::StyleColorsDark();
    auto& style = ImGui::GetStyle();
    style.WindowRounding = 0.f;   // (the menu draws its own panel: Theme::Panel)
    style.ChildRounding = 0.f;
    style.FrameRounding = 2.f;
    style.PopupRounding = 2.f;
    style.GrabRounding = 2.f;
    style.ScrollbarRounding = 2.f;
    style.TabRounding = 0.f;
    style.WindowBorderSize = 0.f;
    style.FrameBorderSize = 0.f;
    style.PopupBorderSize = 1.f;
    style.TabBarBorderSize = 1.f;
    style.TabBarOverlineSize = 2.f;
    style.WindowPadding = {26.f, 18.f};
    style.FramePadding = {12.f, 7.f};
    style.ItemSpacing = {10.f, 9.f};
    style.ItemInnerSpacing = {10.f, 6.f};
    style.ScrollbarSize = 10.f;
    style.GrabMinSize = 14.f;
    auto* c = style.Colors;
    const ImVec4 cyan{.31f, .86f, 1.f, 1.f}, orange{1.f, .59f, .16f, 1.f};
    const auto a = [](ImVec4 v, float alpha) { v.w = alpha; return v; };
    c[ImGuiCol_Text] = {.88f, .96f, 1.f, 1.f};
    c[ImGuiCol_TextDisabled] = {.51f, .69f, .80f, 1.f};
    c[ImGuiCol_WindowBg] = {0.f, 0.f, 0.f, 0.f};
    c[ImGuiCol_ChildBg] = {0.f, 0.f, 0.f, 0.f};
    c[ImGuiCol_PopupBg] = {.03f, .08f, .15f, 1.f};
    c[ImGuiCol_Border] = a(cyan, .45f);
    c[ImGuiCol_BorderShadow] = {0.f, 0.f, 0.f, 0.f};
    c[ImGuiCol_FrameBg] = {.06f, .17f, .27f, .85f};
    c[ImGuiCol_FrameBgHovered] = a(cyan, .25f);
    c[ImGuiCol_FrameBgActive] = a(cyan, .40f);
    c[ImGuiCol_TitleBg] = c[ImGuiCol_TitleBgActive] = {.05f, .16f, .27f, 1.f};
    c[ImGuiCol_ScrollbarBg] = {.02f, .05f, .10f, .40f};
    c[ImGuiCol_ScrollbarGrab] = a(cyan, .30f);
    c[ImGuiCol_ScrollbarGrabHovered] = a(cyan, .50f);
    c[ImGuiCol_ScrollbarGrabActive] = a(cyan, .70f);
    c[ImGuiCol_CheckMark] = orange;
    c[ImGuiCol_SliderGrab] = a(cyan, .80f);
    c[ImGuiCol_SliderGrabActive] = orange;
    c[ImGuiCol_Button] = {.07f, .22f, .35f, .90f};
    c[ImGuiCol_ButtonHovered] = a(cyan, .40f);
    c[ImGuiCol_ButtonActive] = a(orange, .70f);
    c[ImGuiCol_Header] = a(cyan, .22f);
    c[ImGuiCol_HeaderHovered] = a(cyan, .38f);
    c[ImGuiCol_HeaderActive] = a(cyan, .55f);
    c[ImGuiCol_Separator] = a(cyan, .35f);
    c[ImGuiCol_Tab] = {0.f, 0.f, 0.f, 0.f};
    c[ImGuiCol_TabHovered] = a(cyan, .22f);
    c[ImGuiCol_TabSelected] = a(cyan, .14f);
    c[ImGuiCol_TabSelectedOverline] = orange;
    c[ImGuiCol_TabDimmed] = {0.f, 0.f, 0.f, 0.f};
    c[ImGuiCol_TabDimmedSelected] = a(cyan, .10f);
    c[ImGuiCol_TabDimmedSelectedOverline] = a(orange, .6f);
    c[ImGuiCol_NavCursor] = orange;
    c[ImGuiCol_PlotHistogram] = cyan;   // the launcher's build progress
    c[ImGuiCol_TextSelectedBg] = a(cyan, .35f);
    c[ImGuiCol_ModalWindowDimBg] = {.01f, .03f, .07f, .60f};
}

/// A switch in place of a checkbox (the menu's on/off rows), as ImGui::Checkbox is called: a pill whose knob slides
/// right and lights it cyan when on; text before a "##" follows it, wrapped to the width there is.
bool OnOff(const char* label, bool* value) {
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems) return false;
    const ImGuiStyle& style = ImGui::GetStyle();
    const ImGuiID id = window->GetID(label);
    const float h = ImGui::GetFrameHeight(), w = h * 1.75f;
    const ImVec2 pos = window->DC.CursorPos;
    const char* label_end = ImGui::FindRenderedTextEnd(label);
    const float wrap = std::max(1.f, ImGui::GetContentRegionAvail().x - w - style.ItemInnerSpacing.x);
    const ImVec2 label_size = ImGui::CalcTextSize(label, label_end, false, wrap);
    const float height = std::max(h, label_size.y + style.FramePadding.y * 2.f);
    const ImRect bb(pos, {pos.x + w + (label_size.x > 0.f ? style.ItemInnerSpacing.x + label_size.x : 0.f), pos.y + height});
    ImGui::ItemSize(bb, style.FramePadding.y);
    if (!ImGui::ItemAdd(bb, id)) return false;
    bool hovered = false, held = false;
    const bool pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held);
    if (pressed) {
        *value = !*value;
        ImGui::MarkItemEdited(id);
    }
    // the knob's place, eased toward on or off (kept with the switch's other state in ImGui's storage)
    float* t = ImGui::GetStateStorage()->GetFloatRef(id ^ 0x70661eu, *value ? 1.f : 0.f);
    *t += ((*value ? 1.f : 0.f) - *t) * std::min(1.f, ImGui::GetIO().DeltaTime * 16.f);
    ImDrawList* draw = window->DrawList;
    const float th = h * .64f, r = th * .5f;
    const ImVec2 a{pos.x + 1.f, pos.y + (h - th) * .5f}, b{pos.x + w - 1.f, a.y + th};
    const ImVec4 off = ImGui::GetStyleColorVec4(hovered ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg);
    const ImVec4 on{.31f, .86f, 1.f, hovered ? .95f : .80f};
    const ImVec4 track{off.x + (on.x - off.x) * *t, off.y + (on.y - off.y) * *t, off.z + (on.z - off.z) * *t,
                       off.w + (on.w - off.w) * *t};
    draw->AddRectFilled(a, b, ImGui::GetColorU32(track), r);
    draw->AddRect(a, b, ImGui::GetColorU32({.31f, .86f, 1.f, .35f + .3f * *t}), r, 0, 1.f);
    const ImVec2 knob{a.x + r + (b.x - a.x - 2.f * r) * *t, a.y + r};
    draw->AddCircleFilled(knob, r - 2.5f, ImGui::GetColorU32(ImVec4{.88f + .12f * *t, .96f + .04f * *t, 1.f, .75f + .25f * *t}));
    ImGui::RenderNavCursor(bb, id);
    if (label_size.x > 0.f) {
        ImGui::RenderTextWrapped({pos.x + w + style.ItemInnerSpacing.x, pos.y + style.FramePadding.y}, label, label_end, wrap);
    }
    return pressed;
}

/// A heading inside a page: its title in small spaced capitals, a rule running on to the right edge.
void Section(const char* title) {
    const float px = ImGui::GetFontSize() * .72f, tracking = px * .18f, h = ImGui::GetFontSize() * 1.6f;
    if (ImGui::GetCursorPosY() > ImGui::GetStyle().ItemSpacing.y * 2.f) ImGui::Dummy({0.f, ImGui::GetFontSize() * .35f});
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float right = at.x + ImGui::GetContentRegionAvail().x, mid = at.y + h * .5f;
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float w = Theme::Text(draw, Theme::title_font, px, {at.x, mid}, ImGui::GetColorU32(ImVec4{.31f, .86f, 1.f, .85f}), title, tracking);
    const ImU32 line = ImGui::GetColorU32(ImVec4{.31f, .86f, 1.f, .35f}), clear = ImGui::GetColorU32(ImVec4{.31f, .86f, 1.f, 0.f});
    draw->AddRectFilledMultiColor({at.x + w + px, mid}, {right, mid + 1.f}, line, clear, clear, line);   // fading out to the right
    ImGui::Dummy({0.f, h});
}

/// A row's label, wrapped before `column` (window local) so no font runs it under the row's
/// control, which follows at `column`.
void Label(const char* text, float column) {
    ImGui::AlignTextToFramePadding();
    ImGui::PushTextWrapPos(column - ImGui::GetStyle().ItemSpacing.x);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::SameLine(column);
}

/// Explanatory text, dimmed and wrapped to the width available.
void Note(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

/// The height Note(text) takes, spacing included.
float NoteHeight(const char* text) {
    return ImGui::CalcTextSize(text, nullptr, false, ImGui::GetContentRegionAvail().x).y +
           ImGui::GetStyle().ItemSpacing.y;
}

/// A video preset's internal resolution, within what the menu offers.
int PresetResolution(const VideoPreset& preset, const EmulatorSettings& e) {
    return std::clamp(preset.resolution, 1, std::max(e.max_resolution, 1));
}

/// The video preset (VideoPresets) the Display tab's settings make, or -1: Custom.
int VideoPresetOf(const PlayerSettings& p, const EmulatorSettings& e) {
    for (std::size_t i = 0; i < VideoPresets.size(); ++i) {
        const auto& preset = VideoPresets[i];
        const bool rate = preset.frame_rate <= 30 ? p.frame_rate == preset.frame_rate
                                                  : p.frame_rate >= preset.frame_rate;
        // power-saving waits count only for the preset that turns them on (Minimum); the others leave them
        if (rate && e.resolution == PresetResolution(preset, e) &&
            (!e.aniso_option || e.aniso == preset.aniso) &&
            (!e.performance_options ||
             (e.fsr == preset.fsr && p.wide_culling != preset.only_3ds_view &&
              (!preset.power_saving || e.power_saving)))) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void ApplyVideoPreset(const VideoPreset& preset, PlayerSettings& p, EmulatorSettings& e) {
    e.resolution = PresetResolution(preset, e);
    if (e.aniso_option) {
        e.aniso = preset.aniso;
    }
    if (e.performance_options) {
        e.fsr = preset.fsr;
        p.wide_culling = !preset.only_3ds_view;
        e.power_saving = e.power_saving || preset.power_saving;
    }
    if (preset.frame_rate <= 30 || p.frame_rate < preset.frame_rate) {
        p.frame_rate = preset.frame_rate;
    }
}

/// What a video preset sets, for the note under the Video preset row.
std::string VideoPresetNote(const VideoPreset& preset, const EmulatorSettings& e) {
    std::string note = std::string(ResolutionNames[PresetResolution(preset, e)]) + " internal resolution";
    if (e.aniso_option) {
        const auto aniso = std::find(AnisoChoices.begin(), AnisoChoices.end(), preset.aniso);
        note += preset.aniso > 1 ? std::string(", ") + AnisoNames[aniso - AnisoChoices.begin()] +
                                       " anisotropic filtering"
                                 : ", no anisotropic filtering";
    }
    if (e.performance_options) {
        note += preset.fsr ? ", FSR 1 upscaling" : ", linear upscaling";
    }
    note += preset.frame_rate <= 30 ? ", 30 FPS (the original)"
                                    : ", " + std::to_string(preset.frame_rate) +
                                          " FPS (a faster rate you chose stays)";
    if (e.performance_options && preset.only_3ds_view) {
        note += preset.power_saving ? "; only the 3DS's view drawn, power-saving waits (below)"
                                    : "; only the 3DS's view drawn (below)";
    }
    return note + ".";
}

} // namespace

#ifndef MSR_NATIVE
void PlayerSettings::Load(const QString& path) {
    QSettings file(path, QSettings::IniFormat);
    widescreen = file.value(QStringLiteral("widescreen"), widescreen).toBool();
    ultrawide = file.value(QStringLiteral("ultrawide"), ultrawide).toBool() && widescreen;   // never on without 16:9
    camera_zoom = std::clamp(file.value(QStringLiteral("camera_zoom"), camera_zoom).toFloat(),
                             CameraZoomMin, CameraZoomMax);
    hud_scale = file.value(QStringLiteral("hud_scale"), hud_scale).toFloat();
    hud_opacity = file.value(QStringLiteral("hud_opacity"), hud_opacity).toFloat();
    show_minimap = file.value(QStringLiteral("show_minimap"), show_minimap).toBool();
    minimap_size = file.value(QStringLiteral("minimap_size"), minimap_size).toFloat();
    minimap_opacity = file.value(QStringLiteral("minimap_opacity"), minimap_opacity).toFloat();
    minimap_brightness = file.value(QStringLiteral("minimap_brightness"), minimap_brightness).toFloat();
    show_counters = file.value(QStringLiteral("show_counters"), show_counters).toBool();
    show_abilities = file.value(QStringLiteral("show_abilities"), show_abilities).toBool();
    hud_within_16_9 = file.value(QStringLiteral("hud_within_16_9"), hud_within_16_9).toBool();
    panel_scale = file.value(QStringLiteral("panel_scale"), panel_scale).toFloat();
    fit_pause = file.value(QStringLiteral("fit_pause"), fit_pause).toBool();
    menu_backdrop = file.value(QStringLiteral("menu_backdrop"), menu_backdrop).toFloat();
    control_hints = file.value(QStringLiteral("control_hints"), control_hints).toBool();
    menu_scale = file.value(QStringLiteral("menu_scale"), menu_scale).toFloat();
    show_fps = file.value(QStringLiteral("show_fps"), show_fps).toBool();
    frame_rate = file.value(QStringLiteral("frame_rate"), frame_rate).toInt();
    bloom = std::clamp(file.value(QStringLiteral("bloom"), bloom).toFloat(), 0.f, 1.f);
    hdr = file.value(QStringLiteral("hdr"), hdr).toBool();
    hdr_peak = file.value(QStringLiteral("hdr_peak"), hdr_peak).toFloat();
    hdr_paper = file.value(QStringLiteral("hdr_paper"), hdr_paper).toFloat();
    fxaa = file.value(QStringLiteral("fxaa"), fxaa).toBool();
    smooth_packs = file.value(QStringLiteral("smooth_packs"), smooth_packs).toBool();
    brightness = std::clamp(file.value(QStringLiteral("brightness"), brightness).toFloat(), .5f, 1.5f);
    contrast = std::clamp(file.value(QStringLiteral("contrast"), contrast).toFloat(), .5f, 1.5f);
    saturation = std::clamp(file.value(QStringLiteral("saturation"), saturation).toFloat(), .5f, 1.5f);
    stereo = std::clamp(file.value(QStringLiteral("stereo"), stereo).toInt(), 0, static_cast<int>(StereoNames.size()) - 1);
    stereo_depth = std::clamp(file.value(QStringLiteral("stereo_depth"), stereo_depth).toFloat(), StereoDepthMin, 1.f);
    stereo_swap = file.value(QStringLiteral("stereo_swap"), stereo_swap).toBool();
    if (std::find(FrameRate::Choices.begin(), FrameRate::Choices.end(), frame_rate) ==
        FrameRate::Choices.end()) {
        frame_rate = 60;
    }
    unlock_galleries = file.value(QStringLiteral("unlock_galleries"), unlock_galleries).toBool();
    unlock_fusion_mode =
        file.value(QStringLiteral("unlock_fusion_mode"), unlock_fusion_mode).toBool();
    unlock_hard_mode = file.value(QStringLiteral("unlock_hard_mode"), unlock_hard_mode).toBool();
    fusion_suit = file.value(QStringLiteral("fusion_suit"), fusion_suit).toBool();
    infinite_energy = file.value(QStringLiteral("infinite_energy"), infinite_energy).toBool();
    infinite_missiles = file.value(QStringLiteral("infinite_missiles"), infinite_missiles).toBool();
    infinite_aeion = file.value(QStringLiteral("infinite_aeion"), infinite_aeion).toBool();
}

void PlayerSettings::Save(const QString& path) const {
    QSettings file(path, QSettings::IniFormat);
    file.setValue(QStringLiteral("widescreen"), widescreen);
    file.setValue(QStringLiteral("ultrawide"), ultrawide);
    file.setValue(QStringLiteral("camera_zoom"), camera_zoom);
    file.setValue(QStringLiteral("hud_scale"), hud_scale);
    file.setValue(QStringLiteral("hud_opacity"), hud_opacity);
    file.setValue(QStringLiteral("show_minimap"), show_minimap);
    file.setValue(QStringLiteral("minimap_size"), minimap_size);
    file.setValue(QStringLiteral("minimap_opacity"), minimap_opacity);
    file.setValue(QStringLiteral("minimap_brightness"), minimap_brightness);
    file.setValue(QStringLiteral("show_counters"), show_counters);
    file.setValue(QStringLiteral("show_abilities"), show_abilities);
    file.setValue(QStringLiteral("hud_within_16_9"), hud_within_16_9);
    file.setValue(QStringLiteral("panel_scale"), panel_scale);
    file.setValue(QStringLiteral("fit_pause"), fit_pause);
    file.setValue(QStringLiteral("menu_backdrop"), menu_backdrop);
    file.setValue(QStringLiteral("control_hints"), control_hints);
    file.setValue(QStringLiteral("menu_scale"), menu_scale);
    file.setValue(QStringLiteral("show_fps"), show_fps);
    file.setValue(QStringLiteral("frame_rate"), frame_rate);
    file.setValue(QStringLiteral("bloom"), bloom);
    file.setValue(QStringLiteral("hdr"), hdr);
    file.setValue(QStringLiteral("hdr_peak"), hdr_peak);
    file.setValue(QStringLiteral("hdr_paper"), hdr_paper);
    file.setValue(QStringLiteral("fxaa"), fxaa);
    file.setValue(QStringLiteral("smooth_packs"), smooth_packs);
    file.setValue(QStringLiteral("brightness"), brightness);
    file.setValue(QStringLiteral("contrast"), contrast);
    file.setValue(QStringLiteral("saturation"), saturation);
    file.setValue(QStringLiteral("stereo"), stereo);
    file.setValue(QStringLiteral("stereo_depth"), stereo_depth);
    file.setValue(QStringLiteral("stereo_swap"), stereo_swap);
    file.setValue(QStringLiteral("unlock_galleries"), unlock_galleries);
    file.setValue(QStringLiteral("unlock_fusion_mode"), unlock_fusion_mode);
    file.setValue(QStringLiteral("unlock_hard_mode"), unlock_hard_mode);
    file.setValue(QStringLiteral("fusion_suit"), fusion_suit);
    file.setValue(QStringLiteral("infinite_energy"), infinite_energy);
    file.setValue(QStringLiteral("infinite_missiles"), infinite_missiles);
    file.setValue(QStringLiteral("infinite_aeion"), infinite_aeion);
}
#endif

PlayerSettings PlayerSettings::AsPlayed() const {
    PlayerSettings s = *this;
    if (s.aspect_4_3) {   // 4:3 mode: the 3DS's width (the player's widescreen settings kept for when it is off)
        s.widescreen = s.ultrawide = s.fill_16_10 = false;
    }
    if (!Presentation::original.load(std::memory_order_relaxed)) return s;
    // 3DS mode: the 3DS's 5:3 view at the game's own camera (the screens are video.c's, set by main.c at the start). The
    // two-screen layouts neither squeeze nor zoom the world's draws (msr_single), so a wider view or a zoom would only
    // widen the camera there. The video and texture settings stay the player's.
    s.widescreen = s.ultrawide = s.fill_16_10 = s.aspect_4_3 = false;
    s.camera_zoom = 1.f;
    return s;
}

void PlayerSettings::Publish() const {
    namespace P = Presentation;
    const PlayerSettings s = AsPlayed();
    P::widescreen = s.widescreen;
    P::ultrawide = s.ultrawide;
    P::fill_16_10 = s.fill_16_10;
    P::aspect_4_3 = s.aspect_4_3;
    P::zoom = s.camera_zoom;
    P::wide_culling = s.wide_culling;
    P::hud_scale = s.hud_scale;
    P::hud_opacity = s.hud_opacity;
    P::show_minimap = s.show_minimap;
    P::minimap_size = s.minimap_size;
    P::minimap_opacity = s.minimap_opacity;
    P::minimap_brightness = s.minimap_brightness;
    P::show_counters = s.show_counters;
    P::show_abilities = s.show_abilities;
    P::hud_within_16_9 = s.hud_within_16_9;
    P::panel_scale = s.panel_scale;
    P::fit_pause = s.fit_pause;
    P::menu_backdrop = s.menu_backdrop;
    P::bloom = s.bloom;
    P::hdr = s.hdr;
    P::hdr_peak = s.hdr_peak;
    P::hdr_paper = s.hdr_paper;
    P::fxaa = s.fxaa;
    P::smooth_packs = s.smooth_packs;
    P::brightness = s.brightness;
    P::contrast = s.contrast;
    P::saturation = s.saturation;
    P::stereo = s.stereo;
    P::stereo_depth = s.stereo_depth;
    P::stereo_swap = s.stereo_swap;
    FrameRate::target = s.frame_rate;
    Cheats::enabled = (s.unlock_galleries ? Cheats::Galleries : 0u) |
                      (s.unlock_fusion_mode ? Cheats::FusionMode : 0u) |
                      (s.unlock_hard_mode ? Cheats::HardMode : 0u) |
                      (s.fusion_suit ? Cheats::FusionSuit : 0u) |
                      (s.infinite_energy ? Cheats::InfiniteEnergy : 0u) |
                      (s.infinite_missiles ? Cheats::InfiniteMissiles : 0u) |
                      (s.infinite_aeion ? Cheats::InfiniteAeion : 0u);
}

Overlay::Overlay() {
    context = ImGui::CreateContext();
    ImGui::SetCurrentContext(context);
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    // The game's layout, Nintendo's: its A is the right face button (Xbox B) and chooses, its B
    // the bottom one (Xbox A) and goes back. msr-native gives the pad's choosing button as the
    // right one (overlay_host.cpp PadKeyOf: an Xbox or PlayStation pad chooses with the bottom one).
    io.ConfigNavSwapGamepadButtons = true;
    io.ConfigNavCursorVisibleAuto = true;
    ApplyStyle();

    // A large font, scaled down per frame, stays sharp from 720p to 4K. Bahnschrift first (Windows 10 and later), the
    // menus' face in the other ports (Theme). Linux distributions keep the same fonts in different folders: SteamOS and
    // Arch, Debian and Ubuntu, then Fedora.
    const char* const fonts[] = {"C:/Windows/Fonts/bahnschrift.ttf",
                                 "C:/Windows/Fonts/segoeui.ttf",
                                 "C:/Windows/Fonts/arial.ttf",
                                 "/usr/share/fonts/noto/NotoSans-Regular.ttf",
                                 "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
                                 "/usr/share/fonts/google-noto/NotoSans-Regular.ttf",
                                 "/usr/share/fonts/TTF/DejaVuSans.ttf",
                                 "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                                 "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf"};
    ImFont* font = nullptr;
    for (const char* path : fonts) {
        std::error_code error;
        if (std::filesystem::exists(path, error)) {
            font = io.Fonts->AddFontFromFileTTF(path, font_size);
            if (font) {
                Theme::title_font = io.Fonts->AddFontFromFileTTF(path, font_size * 2.f);   // the menu's header
                break;
            }
        }
    }
    if (!font) {
        ImFontConfig config;
        config.SizePixels = font_size;
        io.Fonts->AddFontDefault(&config);
    }
}

Overlay::~Overlay() {
    if (context) {
        ImGui::DestroyContext(context);
    }
}

void Overlay::SetVisible(bool show) {
    if (!show && launcher_active.load()) {
        return;   // the launcher is this menu: it stays open
    }
    visible = show;
    // Input goes back to the game only once the closing button is released (InputBridge).
    if (show) {
        Presentation::block_game_input = true;
    }
    if (!show) {
        // Persist anything changed while the menu was open.
        Act(OverlayAction::Kind::Player);
    }
}

void Overlay::ShowStartupHint() {
    hint_started = Now();
}

void Overlay::SetEmulatorSettings(const EmulatorSettings& settings) {
    std::scoped_lock lock{mutex};
    emulator = settings;
}

void Overlay::SetPlayerSettings(const PlayerSettings& settings) {
    std::scoped_lock lock{mutex};
    player = settings;
    player.Publish();
}

PlayerSettings Overlay::GetPlayerSettings() {
    std::scoped_lock lock{mutex};
    return player;
}

void Overlay::Push(const Event& event) {
    std::scoped_lock lock{mutex};
    events.push_back(event);
}

void Overlay::Key(int key, bool down) {
    Push({Event::Type::Key, key, down, 0.f, 0.f});
}

void Overlay::Analog(int key, bool down, float value) {
    Push({Event::Type::Analog, key, down, value, 0.f});
}

void Overlay::MousePosition(float x, float y) {
    Push({Event::Type::Position, 0, false, x, y});
}

void Overlay::MouseButton(int button, bool down) {
    Push({Event::Type::Button, button, down, 0.f, 0.f});
}

void Overlay::MouseWheel(float delta) {
    Push({Event::Type::Wheel, 0, false, 0.f, delta});
}

void Overlay::Text(const char* utf8) {
    std::scoped_lock lock{mutex};
    while (utf8 && *utf8) {
        unsigned int c = 0;
        const int n = ImTextCharFromUtf8(&c, utf8, nullptr);
        if (n <= 0) break;
        utf8 += n;
        events.push_back({Event::Type::Text, static_cast<int>(c), false, 0.f, 0.f});
    }
}

std::vector<OverlayAction> Overlay::TakeActions() {
    std::scoped_lock lock{mutex};
    return std::exchange(actions, {});
}

void Overlay::Act(OverlayAction::Kind kind, int value, std::string text) {
    std::scoped_lock lock{mutex};
    actions.push_back({kind, emulator, value, std::move(text)});
}

void Overlay::SetLauncher(const LauncherSnapshot& snapshot) {
    {
        std::scoped_lock lock{mutex};
        launcher = snapshot;
    }
    launcher_active = snapshot.active;
    if (snapshot.active) {
        visible = true;
    }
}

void Overlay::SetControls(const ControlsSnapshot& snapshot) {
    std::scoped_lock lock{mutex};
    controls = snapshot;
}

void Overlay::SetMods(const ModsSnapshot& snapshot) {
    std::scoped_lock lock{mutex};
    mods = snapshot;
}

void Overlay::PlayerChanged(bool layout) {
    std::scoped_lock lock{mutex};
    player.Publish();
    if (layout) {
        actions.push_back({OverlayAction::Kind::Layout, emulator});
    }
}

void Overlay::EmulatorChanged() {
    std::scoped_lock lock{mutex};
    actions.push_back({OverlayAction::Kind::Emulator, emulator});
}

void Overlay::Init(const OverlayHook::VulkanContext& vulkan) {
    ImGui::SetCurrentContext(context);
    LoaderData loader{vulkan.instance, vulkan.get_instance_proc_addr};
    if (!ImGui_ImplVulkan_LoadFunctions(vulkan.api_version, LoadVulkanFunction, &loader)) {
        LOG_ERROR(Frontend, "Settings overlay: Vulkan functions unavailable");
        return;
    }
    ImGui_ImplVulkan_InitInfo info{};
    info.ApiVersion = vulkan.api_version;
    info.Instance = vulkan.instance;
    info.PhysicalDevice = vulkan.physical_device;
    info.Device = vulkan.device;
    info.QueueFamily = vulkan.queue_family;
    info.Queue = vulkan.queue;
    info.RenderPass = vulkan.render_pass;
    if (!vulkan.render_pass) {
        // msr-native draws its presentation with dynamic rendering
        info.UseDynamicRendering = true;
        info.PipelineRenderingCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR;
        info.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
        info.PipelineRenderingCreateInfo.pColorAttachmentFormats = &vulkan.color_format;
    }
    info.MinImageCount = 2;
    // One spare vertex buffer set beyond the frames the presenter keeps in flight.
    info.ImageCount = std::max(3u, vulkan.image_count + 1);
    info.DescriptorPoolSize = 8;
    info.CheckVkResultFn = CheckResult;
    if (!ImGui_ImplVulkan_Init(&info)) {
        LOG_ERROR(Frontend, "Settings overlay: initialization failed");
        return;
    }
    {
        // Upload the font now; NewFrame would otherwise submit from inside command recording.
        std::scoped_lock lock{*vulkan.queue_mutex};
        ImGui_ImplVulkan_CreateFontsTexture();
    }
    backend_ready = true;
    previous_time = Now();
}

void Overlay::InitOpenGL() {
#ifdef MSR_OPENGL
    ImGui::SetCurrentContext(context);
    if (!ImGui_ImplOpenGL3_Init("#version 330 core")) {
        LOG_ERROR(Frontend, "Settings overlay: OpenGL initialization failed");
        return;
    }
    opengl = true;
    backend_ready = true;
    previous_time = Now();
#endif
}

void Overlay::Shutdown() {
    if (!backend_ready) {
        return;
    }
    ImGui::SetCurrentContext(context);
#ifdef MSR_OPENGL
    if (opengl) {
        ImGui_ImplOpenGL3_Shutdown();
        opengl = backend_ready = false;
        return;
    }
#endif
    ImGui_ImplVulkan_Shutdown();
    backend_ready = false;
}

void Overlay::Draw(VkCommandBuffer command, std::uint32_t width, std::uint32_t height) {
    if (!backend_ready || opengl) {
        return;
    }
    BuildFrame(width, height, ImGui_ImplVulkan_NewFrame);
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), command);
}

void Overlay::DrawOpenGL(std::uint32_t width, std::uint32_t height) {
#ifdef MSR_OPENGL
    if (!backend_ready || !opengl) {
        return;
    }
    BuildFrame(width, height, ImGui_ImplOpenGL3_NewFrame);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
#else
    (void)width;
    (void)height;
#endif
}

void Overlay::BuildFrame(std::uint32_t width, std::uint32_t height, void (*new_frame)()) {
    ImGui::SetCurrentContext(context);
    auto& io = ImGui::GetIO();
    io.DisplaySize = {static_cast<float>(width), static_cast<float>(height)};
    const double now = Now();
    io.DeltaTime = static_cast<float>(std::clamp(now - previous_time, 1e-5, .25));
    previous_time = now;

    // The game's own frames: the display refreshes at least twice per frame at 30 FPS.
    const std::uint64_t frames = FrameRate::game_frames.load(std::memory_order_relaxed);
    if (now - fps_started >= 1.0) {
        fps = static_cast<float>(static_cast<double>(frames - fps_frames) / (now - fps_started));
        fps_frames = frames;
        fps_started = now;
    }

    const bool show = visible.load();
    {
        std::scoped_lock lock{mutex};
        for (const auto& event : events) {
            switch (event.type) {
            case Event::Type::Key:
                io.AddKeyEvent(static_cast<ImGuiKey>(event.code), event.down);
                break;
            case Event::Type::Analog:
                io.AddKeyAnalogEvent(static_cast<ImGuiKey>(event.code), event.down, event.x);
                break;
            case Event::Type::Position:
                io.AddMousePosEvent(event.x, event.y);
                break;
            case Event::Type::Button:
                io.AddMouseButtonEvent(event.code, event.down);
                break;
            case Event::Type::Wheel:
                io.AddMouseWheelEvent(0.f, event.y);
                break;
            case Event::Type::Text:
                io.AddInputCharacter(static_cast<unsigned int>(event.code));
                break;
            }
        }
        events.clear();
    }
    if (!show) {
        io.ClearInputKeys();
        io.AddMouseButtonEvent(0, false);
    }

    // Sizes follow the view: designed at 1080p, scaled by the menu size setting, and small enough
    // for the 780 x 700 menu to fit in the view with a margin.
    const float scale =
        std::min(std::clamp(static_cast<float>(height) / 1080.f, .6f, 3.f) *
                     GetPlayerSettings().menu_scale,
                 std::min(static_cast<float>(width) / 820.f, static_cast<float>(height) / 740.f));
    static const ImGuiStyle base_style = ImGui::GetStyle();
    ImGui::GetStyle() = base_style;
    ImGui::GetStyle().ScaleAllSizes(scale);
    io.FontGlobalScale = 24.f * scale / font_size;

    new_frame();
    ImGui::NewFrame();
    const bool launching = launcher_active.load();   // the launcher: no game behind the menu
    if (!launching) {
        DrawHint(scale, static_cast<float>(width), static_cast<float>(height));
    }
    if (!show) {
        DrawControlHints(static_cast<float>(width), static_cast<float>(height));
    }
    if (GetPlayerSettings().show_fps && !launching) {
        DrawFps(scale, static_cast<float>(width));
    }
    if (show) {
        if (!was_visible) {
            ImGui::SetNextWindowFocus();
        }
        DrawMenu(scale, !was_visible);
    }
    was_visible = show;
    typing.store(show && ImGui::GetIO().WantTextInput);
    ImGui::Render();
}

void Overlay::DrawHint(float scale, float width, float height) {
    const double started = hint_started.load();
    if (started < 0.0) {
        return;
    }
    // Hold while the title comes up, then fade out over two seconds, as in DekUI.
    const float t = static_cast<float>(previous_time - started);
    const float fade = std::clamp((t - 7.f) / 2.f, 0.f, 1.f);
    const float alpha = 1.f - fade * fade * (3.f - 2.f * fade);
    if (alpha <= 0.f) {
        hint_started = -1.0;
        return;
    }
    const char* label = "Press F1 or Select for settings";
    const float size = 22.f * scale;
    const ImVec2 text = ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, 0.f, label);
    const float pad = 10.f * scale;
    const ImVec2 pos{18.f * scale + pad, height - 18.f * scale - pad - text.y};
    auto* draw = ImGui::GetForegroundDrawList();
    draw->AddRectFilled({pos.x - pad, pos.y - pad}, {pos.x + text.x + pad, pos.y + text.y + pad},
                        IM_COL32(4, 14, 22, static_cast<int>(190 * alpha)), 6.f * scale);
    draw->AddText(ImGui::GetFont(), size, pos, IM_COL32(220, 240, 255, static_cast<int>(255 * alpha)),
                  label);
    (void)width;
}

void Overlay::DrawControlHints(float width, float height) {
    namespace P = Presentation;
    // (not a menu that no Start press opened: the game's own screen over play, whose buttons are not the pause menu's)
    const bool menu = static_cast<P::Bottom>(P::bottom.load()) == P::Bottom::Menu &&
                      P::panel_visible.load() && !P::menu_unasked.load();
    const auto page = static_cast<P::Page>(P::page.load());
    // The menu's "Menu help text" option turns off every set, touch mode's too. (msr-native has no such option: touch
    // mode's, the only set it shows, is always on.)
#ifdef MSR_NATIVE
    const bool enabled = true;
#else
    const bool enabled = GetPlayerSettings().control_hints;
#endif
    int set = -1;
    if (enabled && P::touch_mode.load()) {
        set = menu && page == P::Page::MapPins && !P::world_map.load() ? MapPinsTouch : Touch;
    } else if (enabled && menu) {
#ifdef MSR_NATIVE
        (void)page;   // (msr-native: its menus' buttons are on them; touch mode's alone are named)
#else
        switch (page) {
        case P::Page::PauseMap:
            set = PauseMap;
            break;
        case P::Page::PauseSamus:
            set = PauseSamus;
            break;
        case P::Page::MapPins:
            set = MapPins;
            break;
        case P::Page::FileList:
            set = FileList;
            break;
        case P::Page::FileOpen:
            set = FileOpen;
            break;
        default:
            break;
        }
#endif
    }
    // Fade in and out with the panel, keeping the last set while fading out.
    const float step = ImGui::GetIO().DeltaTime * 6.f;
    hint_alpha = std::clamp(hint_alpha + (set >= 0 ? step : -step), 0.f, 1.f);
    if (set >= 0) {
        hint_set = set;
    }
    const float x1 = static_cast<float>(P::panel_x1.load());
    const float y1 = static_cast<float>(P::panel_y1.load());
    if (hint_alpha <= 0.f || hint_set < 0 || x1 <= 0.f) {
        return;
    }

    // A column beside the panel, sized by the view so it fits in the room the 4:3 panel leaves
    // in a 16:9 view; over the view's corner when the window has no room beside it.
    const float size = std::clamp(height * .026f, 14.f, 72.f);
    const float pad = size * .6f;
    const float gap = size * .5f;
    const float room = width - x1 - 2.f * gap;
    const bool beside = room >= size * 6.f;
    const float box_w = beside ? std::min(room, size * 13.f) : std::min(width - 2.f * gap, size * 15.f);
    const float text_w = box_w - 2.f * pad;
    auto* font = ImGui::GetFont();
    const auto hints = HintSets[hint_set];
    std::vector<std::string> keys;
    for (const auto& hint : hints) {
        keys.push_back(ButtonGlyphs::Keys(hint.keys));
    }
    float box_h = pad * 2.f;
    for (std::size_t i = 0; i < hints.size(); ++i) {
        box_h += font->CalcTextSizeA(size, FLT_MAX, text_w, keys[i].c_str()).y +
                 font->CalcTextSizeA(size, FLT_MAX, text_w, hints[i].action).y + size * .5f;
    }
    box_h -= size * .5f;
    const float x = beside ? x1 + gap : width - gap - box_w;
    float y = std::min(y1, height) - gap - box_h;
    // over the panel (fitted to the window, or a narrow window): above its bottom bar (the touch screen's last
    // 30 of its 240 lines), so the box is not over the buttons there
    const float y0 = static_cast<float>(P::panel_y0.load());
    if (!beside && x < x1 && y1 > y0) {
        y = std::min(y, y0 + (y1 - y0) * (210.f / 240.f) - gap - box_h);
    }

    const auto alpha = [this](int value) { return static_cast<int>(value * hint_alpha); };
    auto* draw = ImGui::GetForegroundDrawList();
    draw->AddRectFilled({x, y}, {x + box_w, y + box_h}, IM_COL32(4, 14, 22, alpha(215)),
                        size * .3f);
    draw->AddRectFilled({x, y + size * .4f}, {x + size * .12f, y + box_h - size * .4f},
                        IM_COL32(60, 170, 220, alpha(255)));
    float line_y = y + pad;
    for (std::size_t i = 0; i < hints.size(); ++i) {
        draw->AddText(font, size, {x + pad, line_y}, IM_COL32(110, 205, 250, alpha(255)),
                      keys[i].c_str(), nullptr, text_w);
        line_y += font->CalcTextSizeA(size, FLT_MAX, text_w, keys[i].c_str()).y;
        draw->AddText(font, size, {x + pad, line_y}, IM_COL32(220, 240, 255, alpha(255)),
                      hints[i].action, nullptr, text_w);
        line_y += font->CalcTextSizeA(size, FLT_MAX, text_w, hints[i].action).y + size * .5f;
    }
}

/// msr-native: a gamepad combo's capture (Quick save, Quick load) counting down to when it takes what is held. `until`
/// is on the host's clock (steady_clock, seconds).
static std::string ComboPrompt(double until, bool brief) {
    const double now =
        std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    const int left = std::max(0, static_cast<int>(std::ceil(until - now)));
    char text[96];
    std::snprintf(text, sizeof(text),
                  brief ? "Hold the buttons: %d" : "Hold two gamepad buttons or more: %d   (Esc cancels)", left);
    return text;
}

void Overlay::DrawControls(float scale, float label_width,
                           const std::function<void()>& default_focus) {
    ControlsSnapshot snapshot;
    {
        std::scoped_lock lock{mutex};
        snapshot = controls;
    }
    // One page that scrolls as a whole, in sections (2026-11-14, the user: "the controls is not right. I can't really see
    // much of what I'm seeing when I scroll down on the page": the bindings scrolled in a region two rows high, between
    // the options above and the reference below)
    const bool native = snapshot.profile == "msr-native";
    if (!native) ImGui::TextDisabled("Profile: %s", snapshot.profile.c_str());
    Section("LAYOUT");
    const float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) / 2.f;
    ImGui::BeginDisabled(!snapshot.controller_connected);
    if (ImGui::Button("Gamepad layout", {half, 0.f})) {
        Act(OverlayAction::Kind::GamepadLayout);
    }
    ImGui::EndDisabled();
    if (snapshot.controller_connected) default_focus();
    ImGui::SameLine();
    if (ImGui::Button("Keyboard layout", {half, 0.f})) {
        Act(OverlayAction::Kind::KeyboardLayout);
    }
    if (!snapshot.controller_connected) default_focus();   // (focus stays off a disabled button)
    // msr-native: the Aeion abilities (the 3DS D-pad) on the right stick, the pad's D-pad moving
    // Samus as the left stick does
    if (snapshot.right_stick_option) {
        bool on = snapshot.right_stick_dpad;
        if (OnOff("Aeion abilities on the right stick (the D-pad moves)", &on)) {
            Act(OverlayAction::Kind::RightStickDpad, on ? 1 : 0);
        }
    }
    // msr-native: menus choose with the game's A as bound below (on: the 3DS's layout, A on the right), not with A /
    // Cross, the bottom button (off, the default)
    if (snapshot.menu_layout_option) {
        bool on = snapshot.menu_confirm_right;
        if (OnOff("Nintendo menu layout (the game's A confirms, as bound below)", &on)) {
            Act(OverlayAction::Kind::MenuConfirmRight, on ? 1 : 0);
        }
    }
    // msr-native (#35): the triggers are buttons like the others below; how far one is pulled before it counts. It shows
    // at once and is saved when let go.
    if (snapshot.trigger_option) {
        int press = snapshot.trigger_press;
        Label("Trigger press point", label_width);
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::SliderInt("##triggerpress", &press, 10, 90, "%d%% of its travel")) {
            std::scoped_lock lock{mutex};
            controls.trigger_press = press;
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            Act(OverlayAction::Kind::TriggerPress, press);
        }
    }

    // The reference, at the page's end: msr-native names the keys or buttons of the device in use (ButtonGlyphs::Keys)
    // and aims with the mouse (video.c).
    std::vector<std::string> reference;
    if (native) {
        reference = {
            "Mouse: the left button shoots, the right one held aims at the pointer, the wheel changes beams",
            ButtonGlyphs::Keys("Move: {Stick}        Beams: {Beams}        Morph Ball: {Morph}"),
            ButtonGlyphs::Keys("Super Missiles: hold {R}, then {Beams} (again for Missiles)"),
            ButtonGlyphs::Keys("Settings: {Close}        Fullscreen: F11"),
        };
        // a pad chooses and goes back in menus with its own layout's buttons (msr/glyphs.cpp), whatever A and B do in play
        const u32 dev = ButtonGlyphs::device.load();
        // touch navigation: the keyboard's (a pad's L3 no more: its D-pad reaches the touch screen's buttons)
        if (dev == ButtonGlyphs::Keyboard) reference.push_back(ButtonGlyphs::Keys("Touch navigation: {Touch}"));
        if (dev == ButtonGlyphs::Xbox || dev == ButtonGlyphs::PlayStation || dev == ButtonGlyphs::Nintendo) {
            reference.push_back(ButtonGlyphs::Keys(snapshot.menu_confirm_right
                ? "In menus: {Choose} chooses, {Back} goes back"
                : "In menus: {Choose} chooses, {Back} goes back (the rows above are for play)"));
        }
        if (snapshot.right_stick_dpad) {
            reference.push_back("Aeion abilities: the right stick        The D-pad moves, as the left stick");
        }
    } else {
        reference = {
            "Beams: ZR (right trigger)        Morph Ball: ZL (left trigger)",
            "Settings: F1 or Select        Fullscreen: F11",
            "Touch navigation: Tab, or L3 in menus",
        };
    }
    const auto bind_row = [&](const ControlsSnapshot::Row& row) {
        ImGui::PushID(row.id);
        Label(row.label.c_str(), label_width);
        if (snapshot.capturing == row.id) {
            const std::string prompt = row.id >= ControlsSnapshot::ComboRow ? ComboPrompt(snapshot.combo_until, false)
                                       : row.id >= 100 ? "Move a stick...   (Esc cancels)"
                                                       : "Press a button, trigger or key...   (Esc cancels)";
            // The same ID whatever the label, so focus stays on the row while it changes.
            ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            if (ImGui::Button((prompt + "###bind").c_str(), {-FLT_MIN, 0.f})) {
                Act(OverlayAction::Kind::CancelBind);
            }
            ImGui::PopStyleColor();
        } else if (ImGui::Button((row.binding + "###bind").c_str(), {-FLT_MIN, 0.f})) {
            Act(OverlayAction::Kind::Bind, row.id);
        }
        ImGui::PopID();
    };
    Section("BUTTONS");
    for (const auto& row : snapshot.rows) {
        if (row.id < ControlsSnapshot::ComboRow) bind_row(row);
    }
    if (native) {
        Section("QUICK SAVE AND LOAD");
        for (const auto& row : snapshot.rows) {
            if (row.id >= ControlsSnapshot::ComboRow) bind_row(row);
        }
    }
    Section("REFERENCE");
    for (const auto& line : reference) {
        Note(line.c_str());
    }
}

ModsSnapshot Overlay::ModsCopy() {
    std::scoped_lock lock{mutex};
    return mods;
}

bool Overlay::ModsTabShown() const {
    return was_visible && (tab == 4 || tab == 5);   // DrawMenu's Mods and Textures
}

void Overlay::ChangeMods(OverlayAction::Kind kind, int value,
                         const std::function<void(ModsSnapshot&)>& edit) {
    std::scoped_lock lock{mutex};
    edit(mods);
    actions.push_back({kind, emulator, value});
}

int Overlay::DrawModList(const ModsSnapshot& snapshot,
                         std::vector<ModsSnapshot::Entry> ModsSnapshot::*list,
                         OverlayAction::Kind toggle, OverlayAction::Kind up,
                         OverlayAction::Kind down, const std::function<void()>& default_focus) {
    const auto& entries = snapshot.*list;
    const int count = static_cast<int>(entries.size());
    for (int i = 0; i < count; ++i) {
        const auto& entry = entries[i];
        // By name, so the cursor stays with an entry as it moves.
        ImGui::PushID(entry.name.c_str());
        bool enabled = entry.enabled;
        if (OnOff(entry.name.c_str(), &enabled)) {
            ChangeMods(toggle, i,
                       [list, i, enabled](ModsSnapshot& m) { (m.*list)[i].enabled = enabled; });
        }
        if (i == 0) {
            default_focus();
        }
        // The details follow the name when they fit before the arrows, else go under it.
        const auto& style = ImGui::GetStyle();
        const float arrows_x =
            ImGui::GetContentRegionMax().x - ImGui::GetFrameHeight() * 2.f - style.ItemSpacing.x;
        ImGui::SameLine();
        const float detail_end =
            ImGui::GetCursorPosX() + ImGui::CalcTextSize(entry.detail.c_str()).x;
        const bool beside = detail_end + style.ItemSpacing.x <= arrows_x;
        if (beside) {
            ImGui::TextDisabled("%s", entry.detail.c_str());
            ImGui::SameLine();
        }
        ImGui::SameLine(std::max(ImGui::GetCursorPosX(), arrows_x));
        ImGui::BeginDisabled(i == 0);
        if (ImGui::ArrowButton("##up", ImGuiDir_Up)) {
            ChangeMods(up, i, [list, i](ModsSnapshot& m) {
                std::swap((m.*list)[i], (m.*list)[i - 1]);
            });
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(i + 1 == count);
        if (ImGui::ArrowButton("##down", ImGuiDir_Down)) {
            ChangeMods(down, i, [list, i](ModsSnapshot& m) {
                std::swap((m.*list)[i], (m.*list)[i + 1]);
            });
        }
        ImGui::EndDisabled();
        if (!beside) {
            const float indent = ImGui::GetFrameHeight() + style.ItemInnerSpacing.x;
            ImGui::Indent(indent);
            ImGui::PushTextWrapPos(arrows_x - style.ItemSpacing.x);
            Note(entry.detail.c_str());
            ImGui::PopTextWrapPos();
            ImGui::Unindent(indent);
        }
        ImGui::PopID();
    }
    return count;
}

void Overlay::DrawModsFooter(const ModsSnapshot& snapshot, const char* note) {
    if (snapshot.restart_needed) {
        ImGui::TextColored({.53f, .89f, .6f, 1.f}, "Restart the game to load the changes.");
        ImGui::SameLine();
        if (ImGui::SmallButton("Restart now")) {
            Act(OverlayAction::Kind::Restart);
            SetVisible(false);
        }
    } else {
        Note(note);
    }
}

void Overlay::DrawGameMods(const std::function<void()>& default_focus) {
    const ModsSnapshot snapshot = ModsCopy();
    Note("Game mods, as 3DS emulators take them: romfs, exefs, code patches. The first one listed "
         "wins where mods overlap.");
    // Below the list: the folder button and a line about restarting.
    const char* const mods_note = "Mods load when the game starts.";
    const float footer =
        ImGui::GetFrameHeightWithSpacing() +
        (snapshot.restart_needed ? ImGui::GetTextLineHeightWithSpacing() : NoteHeight(mods_note));
    const float height = std::max(ImGui::GetFrameHeightWithSpacing() * 2.f,
                                  ImGui::GetContentRegionAvail().y - footer);
    ImGui::BeginChild("##mods", {0.f, height}, ImGuiChildFlags_NavFlattened);
    if (snapshot.mods.empty()) {
        ImGui::TextWrapped("No mods yet. Put each mod in its own folder in %s",
                           snapshot.mod_folder.c_str());
    }
    const int count = DrawModList(snapshot, &ModsSnapshot::mods, OverlayAction::Kind::GameMod,
                                  OverlayAction::Kind::GameModUp, OverlayAction::Kind::GameModDown,
                                  default_focus);
    ImGui::EndChild();

#ifndef __ANDROID__   // msr-native's Android app: the folder above, reached from a PC over USB
    if (ImGui::Button("Open the mods folder", {-FLT_MIN, 0.f})) {
        Act(OverlayAction::Kind::GameModsFolder);
    }
#endif
    if (count == 0) {
        default_focus();
    }
    DrawModsFooter(snapshot, mods_note);
}

void Overlay::DrawTexturePacks(float label_width, const std::function<void()>& default_focus) {
    const ModsSnapshot snapshot = ModsCopy();
    Note("Texture packs: the first one listed wins where packs overlap.");
    // Below the list: the folder button, two options and a line about restarting.
    const char* const packs_note =
        "Packs load when the game starts. Dumps go to user/dump/textures.";
    // msr-native, while dumping (#64): how many so far, and a button to their folder
    char dump_note[160] = "";
    if (snapshot.dump && !snapshot.dump_folder.empty()) {
        std::snprintf(dump_note, sizeof(dump_note), "%u textures dumped so far, as the game draws them.", snapshot.dumped);
    }
    const float footer =
#ifdef __ANDROID__
        ImGui::GetFrameHeightWithSpacing() * 3.f + (*dump_note ? NoteHeight(dump_note) : 0.f) +
#else
        ImGui::GetFrameHeightWithSpacing() * (*dump_note ? 4.f : 3.f) + (*dump_note ? NoteHeight(dump_note) : 0.f) +
#endif
        (snapshot.restart_needed ? ImGui::GetTextLineHeightWithSpacing() : NoteHeight(packs_note));
    const float height = std::max(ImGui::GetFrameHeightWithSpacing() * 2.f,
                                  ImGui::GetContentRegionAvail().y - footer);
    ImGui::BeginChild("##packs", {0.f, height}, ImGuiChildFlags_NavFlattened);
    if (snapshot.packs.empty()) {
        ImGui::TextWrapped("No texture packs yet. Put each pack in its own folder in %s",
                           snapshot.folder.c_str());
    }
    const int count = DrawModList(snapshot, &ModsSnapshot::packs, OverlayAction::Kind::TexturePack,
                                  OverlayAction::Kind::TexturePackUp,
                                  OverlayAction::Kind::TexturePackDown, default_focus);
    ImGui::EndChild();

#ifndef __ANDROID__   // msr-native's Android app: the folder above, reached from a PC over USB
    if (ImGui::Button("Open the texture packs folder", {-FLT_MIN, 0.f})) {
        Act(OverlayAction::Kind::ModsFolder);
    }
#endif
    if (count == 0) {
        default_focus();
    }
    const auto toggle = [&](const char* label, const char* id, bool value, OverlayAction::Kind kind,
                            bool ModsSnapshot::*field) {
        Label(label, label_width);
        if (OnOff(id, &value)) {
            ChangeMods(kind, value ? 1 : 0, [field, value](ModsSnapshot& m) { m.*field = value; });
        }
    };
    ImGui::BeginDisabled(!snapshot.texture_options);
    toggle("Preload textures", "##preload", snapshot.preload, OverlayAction::Kind::PreloadTextures,
           &ModsSnapshot::preload);
    ImGui::EndDisabled();
    ImGui::BeginDisabled(!snapshot.dump_option);
    toggle("Dump game textures", "##dump", snapshot.dump, OverlayAction::Kind::DumpTextures,
           &ModsSnapshot::dump);
    ImGui::EndDisabled();
    if (*dump_note) {
        Note(dump_note);
#ifndef __ANDROID__
        if (ImGui::Button("Open the dumped textures folder", {-FLT_MIN, 0.f})) {
            Act(OverlayAction::Kind::DumpFolder);
        }
#endif
    }
    DrawModsFooter(snapshot, packs_note);
}

void Overlay::DrawFps(float scale, float width) {
    char label[32];
    std::snprintf(label, sizeof(label), "%.0f FPS", fps);
    const float size = 18.f * scale;
    const ImVec2 text = ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, 0.f, label);
    const ImVec2 pos{(width - text.x) / 2.f, 8.f * scale};
    auto* draw = ImGui::GetForegroundDrawList();
    draw->AddRectFilled({pos.x - 6.f * scale, pos.y - 3.f * scale},
                        {pos.x + text.x + 6.f * scale, pos.y + text.y + 3.f * scale},
                        IM_COL32(0, 0, 0, 140), 4.f * scale);
    draw->AddText(ImGui::GetFont(), size, pos, IM_COL32(220, 240, 255, 255), label);
}

void Overlay::DrawMenu(float scale, bool appearing) {
    auto& io = ImGui::GetIO();
    // Snapshots edited by the widgets; changes are published or queued below.
    PlayerSettings p;
    EmulatorSettings e;
    LauncherSnapshot l;
    {
        std::scoped_lock lock{mutex};
        p = player;
        e = emulator;
        l = launcher;
    }
    const PlayerSettings before = p;
    bool player_changed = false;
    bool emulator_changed = false;

    // An eighth page, REPORT (a bug report sent to GitHub with a session's log): the launcher's, and the game's menu's
    const int TabCount = l.report_tab ? 8 : 7;
    if (tab >= TabCount) tab = 0;
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false) || ImGui::IsKeyPressed(ImGuiKey_PageUp, false)) {
        requested_tab = (tab + TabCount - 1) % TabCount;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false) || ImGui::IsKeyPressed(ImGuiKey_PageDown, false)) {
        requested_tab = (tab + 1) % TabCount;
    }

    // The menu fades in and rises a little as it opens (0.18 s), the view behind it dimmed (Theme).
    static double opened_at = 0.0;
    if (appearing) opened_at = Now();
    const float rise = static_cast<float>(std::clamp((Now() - opened_at) / .18, 0.0, 1.0));
    const float ease = rise * rise * (3.f - 2.f * rise);
    ImDrawList* back = ImGui::GetBackgroundDrawList();
    if (l.active) {   // the launcher, no game behind it: a deep navy field, faint scan lines and a slow sweep down it
        const ImVec2 d = io.DisplaySize;
        back->AddRectFilledMultiColor({0.f, 0.f}, d, IM_COL32(9, 24, 44, 255), IM_COL32(9, 24, 44, 255),
                                      IM_COL32(2, 6, 14, 255), IM_COL32(2, 6, 14, 255));
        for (float y = .5f; y < d.y; y += std::max(3.f, 4.f * scale)) back->AddLine({0.f, y}, {d.x, y}, IM_COL32(80, 220, 255, 6));
        const float sweep = static_cast<float>(std::fmod(Now(), 7.0) / 7.0), band = .12f * d.y,
                    y = -band + (d.y + 2.f * band) * sweep;
        const ImU32 clear = IM_COL32(80, 220, 255, 0), lit = IM_COL32(80, 220, 255, 14);
        back->AddRectFilledMultiColor({0.f, y - band}, {d.x, y}, clear, clear, lit, lit);
        back->AddRectFilledMultiColor({0.f, y}, {d.x, y + band}, lit, lit, clear, clear);
        back->AddLine({0.f, y}, {d.x, y}, IM_COL32(80, 220, 255, 28), 1.f);
    } else {
        back->AddRectFilled({0.f, 0.f}, io.DisplaySize, IM_COL32(0, 4, 12, static_cast<int>(120.f * ease)));
    }

    const ImVec2 size{780.f * scale, 700.f * scale};   // (the header and the tabs: 60 more than before)
    ImGui::SetNextWindowPos({io.DisplaySize.x * .5f, io.DisplaySize.y * .5f + (1.f - ease) * 16.f * scale},
                            ImGuiCond_Always, {.5f, .5f});
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ease);
    ImGui::Begin("Metroid: Samus Returns##settings", nullptr,
                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar |
                     ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar);
    {   // the panel under everything the window draws, then the header: METROID / SAMUS RETURNS over a fading rule
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImVec2 a = ImGui::GetWindowPos(), b{a.x + size.x, a.y + size.y};
        const float cut = 26.f * scale;
        draw->PushClipRectFullScreen();   // (the edge's glow and the brackets stand outside the window)
        Theme::Panel(draw, a, b, cut, Theme::Fade(Theme::PanelTop, ease), Theme::Fade(Theme::PanelBottom, ease),
                     Theme::Fade(Theme::Cyan, .75f * ease), 1.5f * scale);
        Theme::Scanlines(draw, a, b, cut, 3.f * scale, Theme::Fade(Theme::Cyan, .035f * ease));
        Theme::CornerBrackets(draw, a, b, 38.f * scale, Theme::Fade(Theme::Orange, .9f * ease), 2.f * scale);
        draw->PopClipRect();
        const ImVec2 pad = ImGui::GetStyle().WindowPadding;
        const float header = 44.f * scale, mid = a.y + pad.y + header * .5f;
        const float title = Theme::Text(draw, Theme::title_font, 32.f * scale, {a.x + pad.x + 6.f * scale, mid},
                                        Theme::Fade(Theme::Cyan, ease), "METROID", 5.f * scale,
                                        Theme::Fade(Theme::Cyan, .14f * ease));
        Theme::Text(draw, Theme::title_font, 17.f * scale, {a.x + pad.x + 6.f * scale + title + 16.f * scale, mid + 4.f * scale},
                    Theme::Fade(Theme::TextDim, ease), "SAMUS RETURNS", 4.f * scale);
        const char* page = l.active ? "LAUNCHER" : "SETTINGS";
        const float page_w = Theme::Measure(Theme::title_font, 15.f * scale, page, 3.f * scale);
        Theme::Text(draw, Theme::title_font, 15.f * scale, {b.x - pad.x - page_w, mid + 4.f * scale},
                    Theme::Fade(Theme::Orange, .85f * ease), page, 3.f * scale);
        Theme::Rule(draw, a.x + pad.x, b.x - pad.x, a.y + pad.y + header + 2.f * scale, Theme::Fade(Theme::Cyan, .6f * ease));
        ImGui::Dummy({0.f, header + 6.f * scale});
    }

    // Labels take 40% of the width and wrap when they need more. Each tab is a page that
    // scrolls above the footer (a separator and a line of text) when the view is too short.
    const float label_width = ImGui::GetContentRegionAvail().x * .4f;
    const float footer =
        ImGui::GetTextLineHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y + 1.f;
    const auto row = [&](const char* label) {
        Label(label, label_width);
        ImGui::SetNextItemWidth(-FLT_MIN);
    };
    // The pages' tabs: a strip of their names in capitals, the page shown lit with an orange bar under it. LB / RB,
    // PageUp / PageDown (above) or a click change it; focus stays in the page.
    if (requested_tab >= 0) tab = requested_tab;
    {
        static constexpr const char* names[8] = {"GAME", "DISPLAY", "HUD", "CONTROLS", "MODS", "TEXTURES", "CHEATS", "REPORT"};
        ImDrawList* draw = ImGui::GetWindowDrawList();
        float px = 17.f * scale, track = 2.5f * scale;
        const float pad = 14.f * scale, h = 36.f * scale;
        const ImVec2 start = ImGui::GetCursorScreenPos();
        const float avail = ImGui::GetContentRegionAvail().x;
        float widths[8], total = 0.f;
        for (int i = 0; i < TabCount; ++i) total += widths[i] = Theme::Measure(Theme::title_font, px, names[i], track);
        if (const float room = avail - TabCount * pad * 2.f; total > room && total > 0.f) {   // (the launcher's eight: smaller)
            const float k = std::max(.6f, room / total);
            px *= k;
            track *= k;
            for (int i = 0; i < TabCount; ++i) widths[i] *= k;
            total *= k;
        }
        const float gap = std::max(pad * 2.f, (avail - total) / TabCount);   // the strip spans the panel
        float x = start.x;
        ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
        for (int i = 0; i < TabCount; ++i) {
            const float w = widths[i] + gap;
            ImGui::SetCursorScreenPos({x, start.y});
            ImGui::PushID(i);
            if (ImGui::InvisibleButton("##tab", {w, h})) tab = i;
            const bool hovered = ImGui::IsItemHovered();
            ImGui::PopID();
            const bool on = tab == i;
            if (on || hovered) {
                draw->AddRectFilledMultiColor({x, start.y}, {x + w, start.y + h}, Theme::Fade(Theme::Cyan, 0.f),
                                              Theme::Fade(Theme::Cyan, 0.f), Theme::Fade(Theme::Cyan, (on ? .22f : .10f) * ease),
                                              Theme::Fade(Theme::Cyan, (on ? .22f : .10f) * ease));
            }
            if (on) draw->AddRectFilled({x + 4.f * scale, start.y + h - 3.f * scale}, {x + w - 4.f * scale, start.y + h},
                                        Theme::Fade(Theme::Orange, ease));
            Theme::Text(draw, Theme::title_font, px, {x + gap * .5f, start.y + h * .5f},
                        Theme::Fade(on ? Theme::Cyan : Theme::TextDim, ease), names[i], track,
                        on ? Theme::Fade(Theme::Cyan, .12f * ease) : 0);
            x += w;
        }
        ImGui::PopItemFlag();
        Theme::Rule(draw, start.x, start.x + avail, start.y + h + 1.f, Theme::Fade(Theme::Cyan, .35f * ease));
        ImGui::SetCursorScreenPos({start.x, start.y + h + 8.f * scale});
    }
    // A page newly shown (the menu opened, or another tab) focuses the row it names (default_focus). Opening the menu
    // starts its navigation; another tab moves the focus there itself (ImGui kept it near the old page's focused row).
    static int shown_tab = -1;
    const bool fresh = appearing || shown_tab != tab;
    shown_tab = tab;
    if (fresh && !appearing) ImGui::NavMoveRequestCancel();   // (ImGui's own PageUp / PageDown: a page up or down)
    const auto tab_item = [&](const char*, int index) {
        if (index != tab) return false;
        ImGui::BeginChild("##page", {0.f, -footer}, ImGuiChildFlags_NavFlattened);
        if (fresh && !appearing) ImGui::SetScrollY(0.f);   // (one scrolling region for every page: from its top)
        return true;
    };
    const auto end_tab_item = [] { ImGui::EndChild(); };

    // msr-native's 3DS mode (PlayerSettings::original_3ds): in the launcher its setting, in the game what the game
    // started with (Presentation::original). The Display and HUD tabs' rows that it sets wait while it is on.
    const bool original_running = Presentation::original.load(std::memory_order_relaxed);
    const bool original = l.active ? p.original_3ds : original_running;
    const auto original_row = [&] {
#ifdef MSR_NATIVE
        row("3DS mode");
        player_changed |= OnOff("##original3ds", &p.original_3ds);
        if (!l.active && p.original_3ds != original_running) {
            ImGui::TextColored({.53f, .89f, .6f, 1.f}, p.original_3ds ? "Restart the game to play in 3DS mode."
                                                                       : "Restart the game to leave 3DS mode.");
            ImGui::SameLine();
            if (ImGui::SmallButton("Restart now##original3ds")) {
                Act(OverlayAction::Kind::Restart);
                SetVisible(false);
            }
        } else {
            Note("Both screens, the top one above the touch screen, in the 3DS's 5:3 view, with the game's own HUD, "
                 "map and menus on the touch screen: the mouse (or a touch screen) is the stylus. Your video and "
                 "texture settings still apply; widescreen, the camera zoom and the HUD tab wait while it's on. Off "
                 "by default.");
        }
#endif
    };
    // msr-native's Skip the launcher (PlayerSettings::skip_launcher): the launcher's, under Play (not in the Android app)
    const auto skip_row = [&] {
#if defined(MSR_NATIVE) && !defined(__ANDROID__)
        row("Skip the launcher");
        player_changed |= OnOff("##skiplauncher", &p.skip_launcher);
        Note("From the next start, msr-native goes straight into the game. Quit to the launcher in the game's menu "
             "comes back here; so does an update waiting, or the game failing to start or crashing. Off by default.");
#endif
    };
    // The note over the rows that wait while it is on
    const auto original_note = [&](const char* what) {
        if (!original) return;
        ImGui::PushTextWrapPos(0.f);
        ImGui::TextColored({.43f, .8f, .98f, 1.f}, "%s", l.active ? "3DS mode is on (GAME tab)." : "In 3DS mode (GAME tab).");
        ImGui::PopTextWrapPos();
        Note(what);
        ImGui::Spacing();
    };

    {
        const auto default_focus = [&] {
            if (appearing) {
                ImGui::SetItemDefaultFocus();
            } else if (fresh) {   // (near the page's top, where tab_item scrolled it)
                ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
                ImGui::SetNavCursorVisible(true);
            }
        };
        if (tab_item("Game", 0)) {
            if (l.active) {
                DrawLauncher(l, label_width, default_focus, [&] {
                    original_row();
                    skip_row();
                });
                // The game reads the language when it starts: in the launcher only, as in the emulator-based 1.x
                row("Language");
                static constexpr auto names = [] {
                    std::array<const char*, Languages.size()> n{};
                    for (std::size_t i = 0; i < n.size(); ++i) n[i] = Languages[i].name;
                    return n;
                }();
                int language = static_cast<int>(
                    std::find_if(Languages.begin(), Languages.end(),
                                 [&](const Language& entry) { return entry.value == p.language; }) -
                    Languages.begin());
                if (ImGui::Combo("##language", &language, names.data(), static_cast<int>(names.size())) &&
                    language < static_cast<int>(Languages.size())) {
                    p.language = Languages[language].value;
                    player_changed = true;
                }
                Note("The game's text. German, Italian and Dutch set the 3DS region to Europe, which the game "
                     "needs for them.");
            }
            row("Volume");
            emulator_changed |= ImGui::SliderInt("##volume", &e.volume, 0, 100, "%d%%");
            row("Unlock amiibo galleries");
            player_changed |= OnOff("##galleries", &p.unlock_galleries);
            row("Unlock Fusion Mode");
            player_changed |= OnOff("##fusionmode", &p.unlock_fusion_mode);
            row("Hard Mode from the start");
            player_changed |= OnOff("##hardmode", &p.unlock_hard_mode);
            Note("A new file and every New Game offer Normal and Hard (Fusion too).");
            row("Fusion suit, any difficulty");
            if (Cheats::randomizer.load(std::memory_order_relaxed)) {
                // The randomizer has its own Fusion suit option (msr/cheats.h): kept off.
                bool off = false;
                ImGui::BeginDisabled();
                OnOff("##fusionsuit", &off);
                ImGui::EndDisabled();
                Note("Off with the randomizer. Fusion Mode: chosen when a game starts.");
            } else {
                player_changed |= OnOff("##fusionsuit", &p.fusion_suit);
                Note("Fusion Mode: chosen when a game starts. The suit switches at once.");
            }
#if defined(MSR_NATIVE) && !defined(__ANDROID__)
            // msr-native: Discord Rich Presence (runtime/msr/discord.cpp), on and off at once
            row("Discord Rich Presence");
            player_changed |= OnOff("##discord", &p.discord_presence);
            Note("Your Discord profile shows that you're playing msr2ds, the area you're in and the Metroids left. "
                 "It needs the Discord app open on this computer.");
#endif
            if (!l.active) {   // (the launcher's is under Play)
                original_row();
            }
            ImGui::Spacing();
            const ImVec2 button{-FLT_MIN, 0.f};
            if (l.active) {
#ifdef __ANDROID__
                // msr-native's Android app: no folders to open; a PC reaches this one over USB
                Note(("The user folder (saves, settings, game mods, texture packs, the game file and the native "
                      "code): " + l.folder + ". From a PC over USB: Android/data/io.github.gminustwo.msrnative/files/user.")
                         .c_str());
#else
                if (ImGui::Button("Open the user folder", button)) {
                    Act(OverlayAction::Kind::LauncherFolder);
                }
                Note(("Saves, settings, game mods and texture packs: " + l.folder).c_str());
#endif
                if (!l.discord.empty()) {   // the project's Discord server
                    if (ImGui::Button("Join the msr2ds Discord", button)) {
                        Act(OverlayAction::Kind::LauncherDiscord);
                    }
                    Note("Chat and news. Bug reports go on GitHub Issues, from the REPORT tab.");
                }
            } else {
                if (ImGui::Button("Resume", button)) {
                    SetVisible(false);
                }
                default_focus();
                ImGui::BeginDisabled(!e.quick_saves);
                // msr-native: beside each, its gamepad combo (Back + LB, Back + RB by default); choosing it counts down
                // five seconds and takes what is held then (overlay_host.cpp)
                ControlsSnapshot c;
                {
                    std::scoped_lock lock{mutex};
                    c = controls;
                }
                const float spacing = ImGui::GetStyle().ItemSpacing.x;
                const float combo_width = std::floor((ImGui::GetContentRegionAvail().x - spacing) * 0.45f);
                const auto quick = [&](const char* label, OverlayAction::Kind kind, int i) {
                    if (ImGui::Button(label, {ImGui::GetContentRegionAvail().x - combo_width - spacing, 0.f})) {
                        Act(kind);
                        SetVisible(false);
                    }
                    ImGui::SameLine();
                    const int row = ControlsSnapshot::ComboRow + i;
                    ImGui::PushID(row);
                    if (c.capturing == row) {
                        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
                        if (ImGui::Button((ComboPrompt(c.combo_until, true) + "###combo").c_str(), {-FLT_MIN, 0.f})) {
                            Act(OverlayAction::Kind::CancelBind);
                        }
                        ImGui::PopStyleColor();
                    } else {
                        const std::string text = c.combos[i].empty() ? "Set a gamepad combo" : "Gamepad " + c.combos[i];
                        if (ImGui::Button((text + "###combo").c_str(), {-FLT_MIN, 0.f})) {
                            Act(OverlayAction::Kind::Bind, row);
                        }
                    }
                    ImGui::PopID();
                };
                quick("Quick save", OverlayAction::Kind::QuickSave, 0);
                quick("Quick load", OverlayAction::Kind::QuickLoad, 1);
                ImGui::EndDisabled();
                if (ImGui::Button("Restart", button)) {
                    Act(OverlayAction::Kind::Restart);
                    SetVisible(false);
                }
                if (!l.discord.empty()) {   // the project's Discord server, as in the launcher
                    if (ImGui::Button("Join the msr2ds Discord", button)) {
                        Act(OverlayAction::Kind::LauncherDiscord);
                    }
                    Note("Opens the invite in your browser. Bug reports go on GitHub Issues, from the REPORT tab.");
                }
                if (l.quit_to_launcher) {   // started without the launcher's window: its Quit ends the program
                    if (ImGui::Button("Quit to the launcher", button)) {
                        Act(OverlayAction::Kind::QuitToLauncher);
                        SetVisible(false);
                    }
                }
            }
            if (ImGui::Button("Quit", button)) {
                Act(OverlayAction::Kind::Quit);
            }
            end_tab_item();
        }
        if (tab_item("Display", 1)) {
            original_note("It shows the 3DS's 5:3 view: widescreen, ultrawide, Fill 16:10, 4:3 mode and the camera zoom "
                          "wait until it's off.");
            if (e.deck_preset) {
                // Measured on a Steam Deck OLED with the 4x texture pack (2026-11-03): 3x, the Deck's own 720p,
                // left 40% of the GPU spare at 60 FPS (about 100 without a limit); 4x held 60 with less to spare.
                const bool in_use = e.resolution == 3 && e.vsync && p.frame_rate == 60;
                if (ImGui::Button("Steam Deck: 60 FPS with texture packs", {-FLT_MIN, 0.f})) {
                    e.resolution = 3;
                    e.vsync = true;
                    emulator_changed = true;
                    p.frame_rate = 60;
                    player_changed = true;
                }
                default_focus();
                Note(in_use ? "In use: 3x internal resolution, 60 FPS, V-Sync. For even frames, set the Deck's refresh "
                              "rate to 60 Hz (Quick Access menu, Performance)."
                            : "Sets 3x internal resolution (the Deck's 720p), 60 FPS and V-Sync. For even frames, set "
                              "the Deck's refresh rate to 60 Hz (Quick Access menu, Performance).");
                ImGui::Spacing();
            }
            // Minimum, Medium, High, Ultra, or Custom: the player's own values, brought back once a preset replaced them
            row("Video preset");
            const int preset = VideoPresetOf(p, e);
            if (ImGui::BeginCombo("##preset", preset >= 0 ? VideoPresets[preset].name : "Custom")) {
                for (int i = 0; i < static_cast<int>(VideoPresets.size()); ++i) {
                    if (ImGui::Selectable(VideoPresets[i].name, preset == i) && preset != i) {
                        ApplyVideoPreset(VideoPresets[i], p, e);
                        emulator_changed = player_changed = true;
                    }
                    if (preset == i) {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::BeginDisabled(!p.custom_video);
                if (ImGui::Selectable("Custom", preset < 0) && preset >= 0) {
                    e.resolution = std::clamp(p.custom_resolution, 0, e.max_resolution);
                    if (e.aniso_option) {
                        e.aniso = AnisoChoice(p.custom_aniso);
                    }
                    if (e.performance_options) {
                        e.fsr = p.custom_fsr;
                        p.wide_culling = p.custom_wide_culling;
                        e.power_saving = p.custom_power_saving;
                    }
                    p.frame_rate = p.custom_frame_rate;
                    emulator_changed = player_changed = true;
                }
                ImGui::EndDisabled();
                if (preset < 0) {
                    ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            if (!e.deck_preset) {
                default_focus();
            }
            {
                const int shown = VideoPresetOf(p, e);
                Note(shown >= 0 ? VideoPresetNote(VideoPresets[shown], e).c_str()
                     : e.performance_options
                         ? "Custom: the internal resolution, anisotropic filtering, upscaling, frame rate and the "
                           "options for slower computers as you set them below. They are kept when you choose a "
                           "preset: Custom brings them back."
                         : "Custom: the internal resolution and frame rate as you set them below. They are kept when "
                           "you choose a preset: Custom brings them back.");
            }
            row("Fullscreen");
            emulator_changed |= OnOff("##fullscreen", &e.fullscreen);
            ImGui::BeginDisabled(original);
            ImGui::BeginDisabled(p.aspect_4_3);   // (4:3 mode has the 3DS's width: these wait while it is on)
            // Ultrawide widens the 16:9 view: on, it turns Widescreen on; Widescreen off turns it off
            row("Widescreen (16:9)");
            if (OnOff("##widescreen", &p.widescreen)) {
                player_changed = true;
                p.ultrawide = p.ultrawide && p.widescreen;
                p.fill_16_10 = p.fill_16_10 && p.widescreen;
            }
            if (e.ultrawide_option) {
                row("Ultrawide (up to 32:9)");
                if (OnOff("##ultrawide", &p.ultrawide)) {
                    player_changed = true;
                    p.widescreen = p.widescreen || p.ultrawide;
                }
                Note("In play, a window wider than 16:9 shows more of the level. It needs Widescreen "
                     "(16:9), which it turns on.");
                // #38: the same below 16:9, down to 16:10
                row("Fill 16:10 screens");
                if (OnOff("##fill1610", &p.fill_16_10)) {
                    player_changed = true;
                    p.widescreen = p.widescreen || p.fill_16_10;
                }
                Note("In play, a window taller than 16:9, down to 16:10 (Steam Deck, most laptops, a Mac in "
                     "fullscreen), shows more of the level above and below instead of bars. It needs Widescreen "
                     "(16:9), which it turns on.");
            }
            ImGui::EndDisabled();
            if (e.ultrawide_option) {   // msr-native's compositor
                row("4:3 mode");
                player_changed |= OnOff("##aspect43", &p.aspect_4_3);
                Note("In play, a 4:3 view: the 3DS's width with more of the level above and below. It fills a 4:3 "
                     "screen, with bars at the sides of a wider one; menus and the title keep the game's own shape. "
                     "Widescreen, ultrawide and Fill 16:10 wait while it's on.");
            }
            row("Camera zoom");
            player_changed |= ImGui::SliderFloat("##zoom", &p.camera_zoom, CameraZoomMin,
                                                 CameraZoomMax, "%.2f");
            ImGui::EndDisabled();
            if (e.bloom_option) {
                row("Bloom");
                int bloom = static_cast<int>(std::lround(p.bloom * 100.f));
                if (ImGui::SliderInt("##bloom", &bloom, 0, 100, "%d%%")) {
                    p.bloom = static_cast<float>(bloom) / 100.f;
                    player_changed = true;
                }
            }
            if (e.hdr_option) {
                row("HDR");
                bool hdr = p.hdr && e.hdr_option == 2;
                ImGui::BeginDisabled(e.hdr_option < 2);
                if (OnOff("##hdr", &hdr)) {
                    p.hdr = hdr;
                    player_changed = true;
                }
                ImGui::EndDisabled();
                if (e.hdr_option < 2) {
                    Note("Turn on Use HDR in Windows' display settings first.");
                }
                if (hdr) {
                    row("HDR highlights");
                    int peak = static_cast<int>(p.hdr_peak > 0.f ? p.hdr_peak : e.display_peak);
                    if (ImGui::SliderInt("##hdrpeak", &peak, 400, 4000, "%d nits")) {
                        p.hdr_peak = static_cast<float>(peak);
                        player_changed = true;
                    }
                    row("HDR paper white");
                    int paper = static_cast<int>(p.hdr_paper > 0.f ? p.hdr_paper : e.display_paper);
                    if (ImGui::SliderInt("##hdrpaper", &paper, 80, 500, "%d nits")) {
                        p.hdr_paper = static_cast<float>(paper);
                        player_changed = true;
                    }
                    Note("Highlights: the brightest lights and glows. Paper white: the game's white, menus and HUD.");
                    // 2026-11-15, the user: a reset to the defaults (0: the display's peak up to 1000 nits, and the
                    // brightness Windows gives SDR content)
                    if (p.hdr_peak > 0.f || p.hdr_paper > 0.f) {
                        row("");
                        if (ImGui::Button("Reset HDR to the defaults", {ImGui::GetContentRegionAvail().x, 0.f})) {
                            p.hdr_peak = p.hdr_paper = 0.f;
                            player_changed = true;
                        }
                        char note[160];
                        std::snprintf(note, sizeof note,
                                      "The defaults: highlights at %d nits (the display's peak, up to 1000), paper white "
                                      "at %d nits (Windows' SDR content brightness).",
                                      static_cast<int>(e.display_peak), static_cast<int>(e.display_paper));
                        Note(note);
                    }
                }
            }
            if (e.stereo_option) {   // msr-native: stereoscopic 3D (#17), the presenter's (video.c) with Vulkan only
                const bool vulkan = e.renderer_active != 2;
                row("Stereoscopic 3D");
                ImGui::BeginDisabled(!vulkan);
                if (ImGui::Combo("##stereo", &p.stereo, StereoNames.data(), static_cast<int>(StereoNames.size()))) {
                    player_changed = true;
                }
                ImGui::EndDisabled();
                Note(!vulkan ? "Needs Vulkan: set Graphics API to Automatic or Vulkan, then restart."
                             : "The game's own 3D, as a 3DS shows it with its 3D slider up, for 3D TVs, Acer SpatialLabs "
                               "displays and VR headsets. Half width squeezes each eye into half the screen (3D TVs, "
                               "SpatialLabs). Full width gives each eye half the window at its own shape (VR players, "
                               "a window twice as wide).");
                if (p.stereo && vulkan) {
                    row("3D depth");
                    int depth = static_cast<int>(std::lround(p.stereo_depth * 100.f));
                    if (ImGui::SliderInt("##stereodepth", &depth, static_cast<int>(StereoDepthMin * 100.f), 100, "%d%%")) {
                        p.stereo_depth = static_cast<float>(depth) / 100.f;
                        player_changed = true;
                    }
                    row("Swap eyes");
                    player_changed |= OnOff("##stereoswap", &p.stereo_swap);
                    Note("3D depth is the 3DS's 3D slider. Swap eyes puts the right eye's picture on the left, for "
                         "cross-eyed viewing or a display that takes the eyes the other way round.");
                }
            }
            if (e.bloom_option) {   // msr-native's picture: anti-aliasing, texture packs' filtering, color (#51, #25)
                row("Anti-aliasing (FXAA)");
                player_changed |= OnOff("##fxaa", &p.fxaa);
                Note("Smooths the game's jagged edges, before the picture is scaled to the window. A higher internal "
                     "resolution smooths them too, at a higher cost.");
                row("Smooth texture packs");
                player_changed |= OnOff("##smoothpacks", &p.smooth_packs);
                Note("A texture pack's textures filtered smoothly, also where the game shows its own with sharp "
                     "pixels.");
                auto percent = [&](const char* label, const char* id, float& value) {
                    row(label);
                    int v = static_cast<int>(std::lround(value * 100.f));
                    if (ImGui::SliderInt(id, &v, 50, 150, "%d%%")) {
                        value = static_cast<float>(v) / 100.f;
                        player_changed = true;
                    }
                };
                percent("Brightness", "##brightness", p.brightness);
                percent("Contrast", "##contrast", p.contrast);
                percent("Saturation", "##saturation", p.saturation);
                if (p.brightness != 1.f || p.contrast != 1.f || p.saturation != 1.f) {
                    row("");
                    if (ImGui::Button("Colors as the game draws them", {ImGui::GetContentRegionAvail().x, 0.f})) {
                        p.brightness = p.contrast = p.saturation = 1.f;
                        player_changed = true;
                    }
                }
            }
            row("Internal resolution");
            const int resolutions =
                std::clamp(e.max_resolution + 1, 2, static_cast<int>(ResolutionNames.size()));
            int resolution = std::clamp(e.resolution, 0, resolutions - 1);
            if (ImGui::Combo("##resolution", &resolution, ResolutionNames.data(), resolutions)) {
                e.resolution = resolution;
                emulator_changed = true;
            }
            if (e.aniso_option) {
                row("Anisotropic filtering");
                int aniso = static_cast<int>(
                    std::find(AnisoChoices.begin(), AnisoChoices.end(), AnisoChoice(e.aniso)) - AnisoChoices.begin());
                if (ImGui::Combo("##aniso", &aniso, AnisoNames.data(), static_cast<int>(AnisoNames.size()))) {
                    e.aniso = AnisoChoices[aniso];
                    emulator_changed = true;
                }
            }
            if (e.performance_options) {
                // FSR 1 sharpens the picture to the window's size in two passes there; linear is one plain stretch
                row("Upscaling");
                static constexpr std::array<const char*, 2> upscaling{"FSR 1 (sharper)", "Linear (lighter)"};
                int filter = e.fsr ? 0 : 1;
                if (ImGui::Combo("##upscaling", &filter, upscaling.data(), static_cast<int>(upscaling.size()))) {
                    e.fsr = filter == 0;
                    emulator_changed = true;
                }
            }
            row("Frame rate");
            const auto& choices = FrameRate::Choices;
            int rate = static_cast<int>(std::find(choices.begin(), choices.end(), p.frame_rate) -
                                        choices.begin());
            if (ImGui::Combo("##framerate", &rate, FrameRateNames.data(),
                             static_cast<int>(FrameRateNames.size())) &&
                rate < static_cast<int>(choices.size())) {
                p.frame_rate = choices[rate];
                player_changed = true;
            }
            row("V-Sync");
            emulator_changed |= OnOff("##vsync", &e.vsync);
            row("Show FPS");
            player_changed |= OnOff("##fps", &p.show_fps);
            row("Menu size");
            player_changed |= ImGui::SliderFloat("##menu", &p.menu_scale, .7f, 1.5f, "%.2f");
            if (e.performance_options) {
                ImGui::Spacing();
                ImGui::SeparatorText("For slower computers");
                row("Power-saving waits");
                emulator_changed |= OnOff("##powersaving", &e.power_saving);
                Note("While the game waits (for the next frame, for the graphics), it sleeps instead of checking in a "
                     "loop: more of a slow processor for the game, and on a handheld or laptop more power for the "
                     "graphics. On by default with 4 processor threads or fewer.");
                // checked lightens the load, as the row above: the setting kept is wide_culling, its opposite
                row("Draw only the 3DS's view");
                bool only_3ds = !p.wide_culling;
                if (OnOff("##only3ds", &only_3ds)) {
                    p.wide_culling = !only_3ds;
                    player_changed = true;
                }
                Note("As in 1.x: with widescreen, ultrawide or a zoom below 1, the game draws only what the 3DS's own "
                     "screen would show, so enemies and items at the edges appear as they come within it. Fewer "
                     "draws: about 12% at 16:9, 30% at zoom 0.70, 40% in ultrawide.");
                row("Texture pack size");
                static constexpr std::array<const char*, 3> sizes{"Full", "Half (a quarter of the memory)",
                                                                  "Quarter (a sixteenth)"};
                int size = e.texture_detail >= 4 ? 2 : e.texture_detail >= 2 ? 1 : 0;
                if (ImGui::Combo("##packsize", &size, sizes.data(), static_cast<int>(sizes.size()))) {
                    e.texture_detail = size == 2 ? 4 : size == 1 ? 2 : 1;
                    emulator_changed = true;
                }
                if (!l.active && e.texture_detail != e.texture_detail_running) {
                    ImGui::TextColored({.53f, .89f, .6f, 1.f}, "Restart the game to load the packs at this size.");
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Restart now##packsize")) {
                        Act(OverlayAction::Kind::Restart);
                        SetVisible(false);
                    }
                } else {
                    Note("Texture pack images are shrunk as they load, never below the game's own size: less graphics "
                         "memory and less to upload, for graphics with 2 GB or less. Applies when the game starts.");
                }
                if (e.renderer_option) {
                    row("Graphics API");
                    static constexpr std::array<const char*, 3> apis{"Automatic", "Vulkan", "OpenGL"};
                    if (ImGui::Combo("##renderer", &e.renderer, apis.data(), static_cast<int>(apis.size()))) {
                        emulator_changed = true;
                    }
                    if (!l.active && e.renderer != e.renderer_running) {
                        ImGui::TextColored({.53f, .89f, .6f, 1.f}, "Restart the game to use it.");
                        ImGui::SameLine();
                        if (ImGui::SmallButton("Restart now##renderer")) {
                            Act(OverlayAction::Kind::Restart);
                            SetVisible(false);
                        }
                    } else {
                        Note(e.renderer_active == 2
                                 ? "In use: OpenGL, for graphics without Vulkan 1.3 (no HDR with it). Automatic uses "
                                   "Vulkan where it can."
                                 : "Automatic uses Vulkan where the graphics have Vulkan 1.3, else OpenGL 3.3 (no HDR "
                                   "with it).");
                    }
                }
            }
            end_tab_item();
        }
        if (tab_item("HUD", 2)) {
            original_note("The HUD, the map and the menus are the game's own, on the touch screen: these rows are for "
                          "the single screen, and wait until it's off.");
            ImGui::BeginDisabled(original);
            row("HUD size");
            player_changed |= ImGui::SliderFloat("##hudsize", &p.hud_scale, .6f, 1.6f, "%.2f");
            if (!original) {
                default_focus();
            }
            row("HUD opacity");
            player_changed |= ImGui::SliderFloat("##hudopacity", &p.hud_opacity, .3f, 1.f, "%.2f");
            row("Map");
            player_changed |= OnOff("##map", &p.show_minimap);
            row("Map size");
            player_changed |= ImGui::SliderFloat("##mapsize", &p.minimap_size, .6f, 1.8f, "%.2f");
            row("Map opacity");
            player_changed |= ImGui::SliderFloat("##mapopacity", &p.minimap_opacity, .3f, 1.f, "%.2f");
            row("Map brightness");
            player_changed |= ImGui::SliderFloat("##mapbrightness", &p.minimap_brightness, 1.f, 2.f, "%.2f");
            row("DNA and Metroid counters");
            player_changed |= OnOff("##counters", &p.show_counters);
            row("Aeion abilities");
            player_changed |= OnOff("##abilities", &p.show_abilities);
            if (e.ultrawide_option) {
                row("HUD within 16:9");
                player_changed |= OnOff("##hud169", &p.hud_within_16_9);
                Note("On an ultrawide screen, the HUD nearer the middle.");
            }
            row("Menu panel size");
            player_changed |= ImGui::SliderFloat("##panel", &p.panel_scale, .5f, 1.f, "%.2f");
            row("Fit the pause screen to the window");
            player_changed |= OnOff("##fitpause", &p.fit_pause);
            Note("The pause screen fills the window: its bars and background reach the edges, its buttons and text "
                 "keep their shape, and the map fills its width. On by default.");
            row("Title menu background");
            player_changed |= ImGui::SliderFloat("##backdrop", &p.menu_backdrop, 0.f, 1.f, "%.2f");
#ifndef MSR_NATIVE   // (msr-native shows no help text beside its menus: its option is gone, touch navigation's kept on)
            row("Menu help text");
            player_changed |= OnOff("##hints", &p.control_hints);
            Note("Help text beside the pause menu and file select, and in touch mode.");
#endif
            ImGui::EndDisabled();
            end_tab_item();
        }
        if (tab_item("Controls", 3)) {
            DrawControls(scale, label_width, default_focus);
            end_tab_item();
        }
        if (tab_item("Mods", 4)) {
            DrawGameMods(default_focus);
            end_tab_item();
        }
        if (tab_item("Textures", 5)) {
            DrawTexturePacks(label_width, default_focus);
            end_tab_item();
        }
        if (tab_item("Cheats", 6)) {
            row("Infinite energy");
            player_changed |= OnOff("##energy", &p.infinite_energy);
            default_focus();
            row("Unlimited missiles");
            player_changed |= OnOff("##missiles", &p.infinite_missiles);
            row("Unlimited Aeion");
            player_changed |= OnOff("##aeion", &p.infinite_aeion);
            Note("Kept full every frame. Missiles include Super Missiles and Power Bombs. A save "
                 "keeps what you have when you save.");
            end_tab_item();
        }
        if (l.report_tab && tab_item("Report", 7)) {
            if (fresh) {
                Act(OverlayAction::Kind::LauncherReportLogs);   // (the logs as they are now)
                report_logs_pending = l.report_logs.empty();     // (the game's menu reads them when the tab first opens)
            }
            DrawReport(l, label_width, default_focus);
            end_tab_item();
        }
    }
    requested_tab = -1;

    {   // the footer: a fading rule, then the controls
        const ImVec2 at = ImGui::GetCursorScreenPos();
        Theme::Rule(ImGui::GetWindowDrawList(), at.x, at.x + ImGui::GetContentRegionAvail().x, at.y,
                    Theme::Fade(Theme::Cyan, .45f * ease));
        ImGui::Dummy({0.f, 1.f});
    }
    const std::string help = ButtonGlyphs::Keys(l.active ? "{Choose} choose     {Tabs} tabs"
                                                         : "{Choose} choose     {Back} back     {Tabs} tabs     {Close} close");
    ImGui::TextDisabled("%s", help.c_str());
    ImGui::End();
    ImGui::PopStyleVar();

    // Video settings that make no preset are the player's own (set by hand, the Steam Deck button, an older version's
    // settings.ini): kept, so the Video preset row's Custom brings them back after a preset replaced them
    if (VideoPresetOf(p, e) < 0 &&
        (!p.custom_video || p.custom_resolution != e.resolution || p.custom_aniso != e.aniso ||
         p.custom_fsr != e.fsr || p.custom_frame_rate != p.frame_rate ||
         p.custom_wide_culling != p.wide_culling || p.custom_power_saving != e.power_saving)) {
        p.custom_video = true;
        p.custom_resolution = e.resolution;
        p.custom_aniso = e.aniso;
        p.custom_fsr = e.fsr;
        p.custom_frame_rate = p.frame_rate;
        p.custom_wide_culling = p.wide_culling;
        p.custom_power_saving = e.power_saving;
        player_changed = true;
    }

    {
        std::scoped_lock lock{mutex};
        player = p;
        emulator = e;
    }
    if (player_changed) {
        PlayerChanged(p.widescreen != before.widescreen || p.ultrawide != before.ultrawide || p.fill_16_10 != before.fill_16_10 ||
                      p.aspect_4_3 != before.aspect_4_3 ||
                      p.panel_scale != before.panel_scale || p.fit_pause != before.fit_pause);
        if (p.frame_rate != before.frame_rate) {
            Act(OverlayAction::Kind::FrameRate);
        }
    }
    if (emulator_changed) {
        EmulatorChanged();
    }

    // The focus shown from the start, so Enter, Space or the pad's button works on the focused button at once
    // (ImGui hides it until a direction is pressed, and ignores activation while it is hidden).
    if (appearing) {
        ImGui::SetNavCursorVisible(true);
    }

    // Back out of the menu with B (the bottom face button) or Escape when nothing is being edited.
    if (!ImGui::IsAnyItemActive() && (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown, false) ||
                                      ImGui::IsKeyPressed(ImGuiKey_Escape, false) ||
                                      ImGui::IsKeyPressed(ImGuiKey_GamepadStart, false))) {
        SetVisible(false);
    }
}

void Overlay::DrawLauncher(const LauncherSnapshot& l, float label_width,
                           const std::function<void()>& default_focus, const std::function<void()>& after_play) {
    using Code = LauncherSnapshot::Code;
    using Update = LauncherSnapshot::Update;
    const auto& style = ImGui::GetStyle();
    const ImVec4 good{.53f, .89f, .6f, 1.f}, bad{1.f, .55f, .45f, 1.f}, accent{.43f, .8f, .98f, 1.f};
    // A newer release (msr-native's launcher looks when it starts): put in place here, or its page
    if (l.update != Update::None) {
        ImGui::PushTextWrapPos(0.f);
        ImGui::TextColored(accent, "msr-native %s is available.", l.update_version.c_str());
        ImGui::PopTextWrapPos();
        const float half = (ImGui::GetContentRegionAvail().x - style.ItemSpacing.x) / 2.f;
        if (l.update == Update::Working) {
            if (l.update_progress >= 0.f) {
                ImGui::ProgressBar(l.update_progress, {-FLT_MIN, 0.f});
            }
        } else if (l.update_installable) {
            if (ImGui::Button(l.update == Update::Failed ? "Try again" : "Update now", {half, 0.f})) {
                Act(OverlayAction::Kind::LauncherUpdate);
            }
            ImGui::SameLine();
            if (ImGui::Button("What's new", {half, 0.f})) {
                Act(OverlayAction::Kind::LauncherUpdatePage);
            }
        } else if (ImGui::Button("Open the download page", {-FLT_MIN, 0.f})) {
            Act(OverlayAction::Kind::LauncherUpdatePage);
        }
        if (!l.update_note.empty()) {
            ImGui::PushTextWrapPos(0.f);
            if (l.update == Update::Failed) {
                ImGui::TextColored(bad, "%s", l.update_note.c_str());
            } else {
                Note(l.update_note.c_str());
            }
            ImGui::PopTextWrapPos();
        }
        ImGui::Separator();
        ImGui::Spacing();
    }
    // Play: the first and largest button, focused when it can start the game
    ImGui::BeginDisabled(!l.can_play);
    ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
    if (ImGui::Button("Play", {-FLT_MIN, ImGui::GetFrameHeight() * 1.7f})) {
        Act(OverlayAction::Kind::LauncherPlay);
    }
    ImGui::PopStyleColor();
    ImGui::EndDisabled();
    if (l.can_play) {
        default_focus();
    }
    after_play();
    ImGui::Spacing();

    // The game file: typed or chosen with the file dialog
    if (l.game != game_input_from) {
        game_input_from = l.game;
        std::snprintf(game_input.data(), game_input.size(), "%s", l.game.c_str());
    }
    Label("Game file", label_width);
    const float browse = l.can_browse ? ImGui::CalcTextSize("Browse...").x + style.FramePadding.x * 2.f : 0.f;
    ImGui::SetNextItemWidth(l.can_browse ? -(browse + style.ItemSpacing.x) : -FLT_MIN);
    ImGui::InputTextWithHint("##game", "Samus Returns (USA), a decrypted .3ds, .cci, .cxi or .zcci", game_input.data(),
                             game_input.size());
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        Act(OverlayAction::Kind::LauncherGame, 0, game_input.data());
    }
    if (!l.can_play && l.game.empty()) {
        default_focus();
    }
    if (l.can_browse) {
        ImGui::SameLine();
        if (ImGui::Button("Browse...")) {
            Act(OverlayAction::Kind::LauncherBrowse);
        }
    }
    if (!l.game_note.empty()) {
        ImGui::SetCursorPosX(label_width);
        ImGui::PushTextWrapPos(0.f);
        ImGui::TextColored(bad, "%s", l.game_note.c_str());
        ImGui::PopTextWrapPos();
    }

    // The native code: the game's code recompiled from the game file, built once
    Label("Native code", label_width);
    static constexpr const char* states[] = {"Not built yet", "Ready", "Built by another version: rebuild it",
                                             "Building...", "The build failed"};
    const ImVec4 color = l.code == Code::Ready ? good : l.code == Code::Building ? ImGui::GetStyleColorVec4(ImGuiCol_Text) : bad;
    ImGui::PushTextWrapPos(0.f);
    ImGui::TextColored(color, "%s", states[static_cast<int>(l.code)]);
    ImGui::PopTextWrapPos();
    ImGui::SetCursorPosX(label_width);
    ImGui::BeginGroup();
    const float width = ImGui::GetContentRegionAvail().x;
    if (l.code == Code::Building) {
        if (l.progress >= 0.f) {
            ImGui::ProgressBar(l.progress, {width, 0.f});
        }
        if (ImGui::Button("Cancel", {width, 0.f})) {
            Act(OverlayAction::Kind::LauncherCancel);
        }
    } else if (!l.can_build) {
        // msr-native's Android app: the native code comes from a PC (its launcher's Build for Android)
        if (l.code != Code::Ready) {
            ImGui::PushTextWrapPos(0.f);
            Note("Build it on a PC: in msr-native for Windows or Linux, choose the same game file and click Build for "
                 "Android on its Game tab. Then copy the recomp-android folder it makes (in its user/msr folder) into "
                 "this app's user/msr folder, shown below.");
            ImGui::PopTextWrapPos();
        }
    } else {
        ImGui::BeginDisabled(l.game.empty() || !l.game_note.empty());
        if (ImGui::Button(l.code == Code::Missing ? "Build the native code" : "Rebuild the native code", {width, 0.f})) {
            Act(OverlayAction::Kind::LauncherBuild);
        }
        ImGui::EndDisabled();
        if (!l.can_play && !l.game.empty() && l.game_note.empty()) {
            default_focus();
        }
    }
    if (!l.code_note.empty()) {
        ImGui::PushTextWrapPos(0.f);
        Note(l.code_note.c_str());
        ImGui::PopTextWrapPos();
    }
    if (l.code == Code::Ready && !l.direct.empty()) {
        ImGui::PushTextWrapPos(0.f);
        Note(("To play without this menu, start " + l.direct + " in the program's folder.").c_str());
        ImGui::PopTextWrapPos();
    }
    if (l.android_build && l.code != Code::Building) {   // the Android app's native code, built here for it
        ImGui::Spacing();
        ImGui::BeginDisabled(l.game.empty() || !l.game_note.empty());
        if (ImGui::Button("Build for Android", {width, 0.f})) {
            Act(OverlayAction::Kind::LauncherBuildAndroid);
        }
        ImGui::EndDisabled();
        ImGui::PushTextWrapPos(0.f);
        Note(l.android_note.empty() ? "The native code for msr-native's Android app, from the same game file: a few "
                                      "minutes, then copied to the phone or handheld."
                                    : l.android_note.c_str());
        ImGui::PopTextWrapPos();
    }
    ImGui::EndGroup();
    ImGui::Spacing();

    // msr-native's Android app on an Adreno GPU: the game's Vulkan driver, the system's or one the player installed
    if (l.gpu_driver_option) {
        Label("GPU driver", label_width);
        ImGui::BeginGroup();
        const float half = (ImGui::GetContentRegionAvail().x - style.ItemSpacing.x) / 2.f;
        std::string current = "System driver";
        for (const auto& d : l.gpu_drivers) {
            if (d.first == l.gpu_driver) {
                current = d.second;
            }
        }
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo("##gpudriver", current.c_str())) {
            if (ImGui::Selectable("System driver", l.gpu_driver.empty())) {
                Act(OverlayAction::Kind::LauncherGpuDriver, 0, "");
            }
            for (const auto& d : l.gpu_drivers) {
                if (ImGui::Selectable((d.second + "##" + d.first).c_str(), d.first == l.gpu_driver)) {
                    Act(OverlayAction::Kind::LauncherGpuDriver, 0, d.first);
                }
            }
            ImGui::EndCombo();
        }
        if (ImGui::Button("Install a driver...", {half, 0.f})) {
            Act(OverlayAction::Kind::LauncherGpuDriverInstall);
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(l.gpu_driver.empty());
        if (ImGui::Button("Remove it", {half, 0.f})) {
            Act(OverlayAction::Kind::LauncherGpuDriverRemove, 0, l.gpu_driver);
        }
        ImGui::EndDisabled();
        ImGui::PushTextWrapPos(0.f);
        if (!l.gpu_driver_note.empty()) {
            ImGui::TextColored(l.gpu_driver_bad ? bad : good, "%s", l.gpu_driver_note.c_str());
        }
        Note("For the game: a driver package (.zip) made for emulators like Azahar and Eden, such as Mesa's Turnip. If "
             "the game closes or draws wrongly with it, choose System driver.");
        ImGui::PopTextWrapPos();
        ImGui::EndGroup();
        ImGui::Spacing();
    }

    // msr-native's Android app: the user folder on the phone's storage or the SD card, and a move between them (#39).
    // The confirmation's buttons take the IDs of the one they replace, so a pad's focus stays on the first.
    if (l.place_option) {
        using Place = LauncherSnapshot::Place;
        Label("User folder", label_width);
        ImGui::BeginGroup();
        ImGui::PushTextWrapPos(0.f);
        ImGui::TextUnformatted(l.place_on_sd ? "On the SD card" : "On the phone's storage");
        const float half = (ImGui::GetContentRegionAvail().x - style.ItemSpacing.x) / 2.f;
        if (l.place == Place::Confirm) {
            ImGui::TextColored(accent, "%s", l.place_note.c_str());
            if (ImGui::Button("Move###place_a", {half, 0.f})) {
                Act(OverlayAction::Kind::LauncherPlaceConfirm);
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel###place_b", {half, 0.f})) {
                Act(OverlayAction::Kind::LauncherPlaceCancel);
            }
        } else if (l.place == Place::Moving) {
            ImGui::ProgressBar(l.place_progress >= 0.f ? l.place_progress : 0.f, {-FLT_MIN, 0.f});
            Note(l.place_note.c_str());
        } else {
            ImGui::BeginDisabled(!l.place_on_sd && !l.place_sd);
            if (ImGui::Button(l.place_on_sd ? "Move to the phone's storage###place_a" : "Move to the SD card###place_a",
                              {-FLT_MIN, 0.f})) {
                Act(OverlayAction::Kind::LauncherPlaceMove);
            }
            ImGui::EndDisabled();
            if (!l.place_note.empty()) {
                ImGui::TextColored(l.place_bad ? bad : good, "%s", l.place_note.c_str());
            } else if (!l.place_on_sd && !l.place_sd) {
                Note("No SD card was found.");
            }
        }
        Note("Uninstalling the app deletes the user folder, on the SD card too: keep a copy of your saves.");
        ImGui::PopTextWrapPos();
        ImGui::EndGroup();
        ImGui::Spacing();
    }

    // Saves from a 3DS (a Checkpoint or JKSM backup) or another emulator, in place of the ones here. The
    // confirmation's buttons take the IDs of the two they replace, so a pad's focus stays on the first.
    using Saves = LauncherSnapshot::Saves;
    Label("Saves", label_width);
    ImGui::BeginGroup();
    ImGui::PushTextWrapPos(0.f);
    {
        const float half = (ImGui::GetContentRegionAvail().x - style.ItemSpacing.x) / 2.f;
        if (l.saves == Saves::Confirm) {
            ImGui::TextColored(accent, "Import the saves in %s?", l.saves_from.c_str());
            Note(l.saves_detail.c_str());
            Note("They replace all three files here. The current ones are kept in user/msr/save-backups.");
            if (ImGui::Button("Import###saves_a", {half, 0.f})) {
                Act(OverlayAction::Kind::LauncherImportConfirm);
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel###saves_b", {half, 0.f})) {
                Act(OverlayAction::Kind::LauncherImportCancel);
            }
        } else {
#ifndef __ANDROID__   // msr-native's Android app: no dialogs or folders to open (a PC copies the saves over USB)
            ImGui::BeginDisabled(l.saves == Saves::Searching);
            if (l.can_browse) {
                if (ImGui::Button("Import saves...###saves_a", {half, 0.f})) {
                    Act(OverlayAction::Kind::LauncherImportSaves);
                }
                ImGui::SameLine();
            }
            if (ImGui::Button("Open saves folder###saves_b", {l.can_browse ? half : -FLT_MIN, 0.f})) {
                Act(OverlayAction::Kind::LauncherSavesFolder);
            }
            ImGui::EndDisabled();
#endif
            if (l.saves == Saves::Done) {
                ImGui::TextColored(good, "%s", l.saves_note.c_str());
            } else if (l.saves == Saves::Failed) {
                ImGui::TextColored(bad, "%s", l.saves_note.c_str());
            } else if (l.saves == Saves::Searching) {
                Note(l.saves_note.c_str());
            } else if (l.can_browse) {
                Note("From a 3DS (a Checkpoint or JKSM backup) or another emulator (Citra, Lime3DS, Azahar): "
                     "choose the folder they are in.");
            } else {
                Note(("From a 3DS or another emulator: copy its profile0, profile1 and profile2 folders into " +
                      l.saves_folder + ".").c_str());
            }
        }
    }
    ImGui::PopTextWrapPos();
    ImGui::EndGroup();
    ImGui::Spacing();

    Note((l.version + ": the game's code runs as native code built from your own dump, on a small "
          "3DS runtime of its own. Nothing of the game comes with this program.").c_str());
    ImGui::Separator();
}

// The REPORT tab (msr-native's launcher, launcher.cpp's bug reports section): the repository's bug report form, asked
// here and sent to GitHub Issues with the log of the play session the player chooses. Sending signs them in to GitHub
// with a code; the log goes up first, and no report goes without it.
void Overlay::DrawReport(const LauncherSnapshot& l, float label_width, const std::function<void()>& default_focus) {
    using Report = LauncherSnapshot::Report;
    const auto& style = ImGui::GetStyle();
    const ImVec4 good{.53f, .89f, .6f, 1.f}, bad{1.f, .55f, .45f, 1.f}, accent{.43f, .8f, .98f, 1.f};
    const float half = (ImGui::GetContentRegionAvail().x - style.ItemSpacing.x) / 2.f;
    const auto wrapped = [](const ImVec4& color, const std::string& text) {
        ImGui::PushTextWrapPos(0.f);
        ImGui::TextColored(color, "%s", text.c_str());
        ImGui::PopTextWrapPos();
    };
    // the logs, newest first, as the launcher found them when the tab was shown
    const auto log_list = [&] {
        for (const auto& log : l.report_logs) {
            ImGui::PushTextWrapPos(0.f);
            ImGui::TextUnformatted(log.when.c_str());
            ImGui::SameLine();
            if (log.ended_badly) ImGui::TextColored(bad, "%s", log.detail.c_str());
            else ImGui::TextDisabled("%s", log.detail.c_str());
            ImGui::PopTextWrapPos();
        }
    };

    if (!l.report_option) {   // no app to sign in with in this build, or no curl: the form on GitHub
#ifdef __ANDROID__
        Note("Bug reports go on msr2ds's GitHub Issues, with the log of the play session where the problem happened. "
             "The button opens the bug report form in your browser. The logs are in the user folder's logs folder "
             "(Android/data/io.github.gminustwo.msrnative/files/user/logs): attach the one from that session.");
#else
        Note("Bug reports go on msr2ds's GitHub Issues, with the log of the play session where the problem happened. "
             "The button opens the bug report form in your browser and the logs folder: drag the log of that session "
             "into the form's Session log box.");
#endif
        if (ImGui::Button("Open the bug report form", {-FLT_MIN, 0.f})) {
            Act(OverlayAction::Kind::LauncherReportWebForm);
        }
        default_focus();
        Section("THE SESSIONS' LOGS");
        if (l.report_logs.empty()) Note("No logs yet: each start of the game writes one.");
        log_list();
        return;
    }

    if (l.report == Report::SigningIn) {
        Section("SIGN IN TO GITHUB");
        Note("Enter this code on GitHub's page, which opened in your browser:");
        {   // the code, large, in the menu's orange
            const float px = ImGui::GetFontSize() * 2.2f;
            const ImVec2 at = ImGui::GetCursorScreenPos();
            Theme::Text(ImGui::GetWindowDrawList(), Theme::title_font, px, {at.x, at.y + px * .6f}, Theme::Orange,
                        l.report_code.empty() ? "..." : l.report_code.c_str(), px * .12f, Theme::Fade(Theme::Orange, .15f));
            ImGui::Dummy({0.f, px * 1.25f});
        }
        if (ImGui::Button("Open the page again", {half, 0.f})) {
            Act(OverlayAction::Kind::LauncherReportCodePage);
        }
        default_focus();
        ImGui::SameLine();
        if (ImGui::Button("Copy the code", {half, 0.f})) {
            Act(OverlayAction::Kind::LauncherReportCopyCode);
        }
        if (ImGui::Button("Cancel", {-FLT_MIN, 0.f})) {
            Act(OverlayAction::Kind::LauncherReportCancel);
        }
        std::string page = l.report_code_page;
        if (page.rfind("https://", 0) == 0) page.erase(0, 8);
        Note(("On a phone or another computer: open " + page + " and enter the code there. Sign in with your GitHub "
              "account (a free one is enough) and allow msr2ds bug reports: it may open issues on msr2ds and make a gist "
              "for your log, nothing else. The report goes out under your GitHub name, so you see the replies. " +
              std::string(l.active ? "This launcher keeps the sign-in until it closes." : "The game keeps the sign-in until it closes."))
                 .c_str());
        return;
    }
    if (l.report == Report::Sending) {
        Section("SENDING");
        wrapped(accent, l.report_note + "...");
        Note("The log goes up first, then the issue that links it.");
        return;
    }
    if (l.report == Report::Sent) {
        Section("SENT");
        wrapped(good, l.report_note);
        if (ImGui::Button("Open the issue", {half, 0.f})) {
            Act(OverlayAction::Kind::LauncherReportOpenIssue);
        }
        default_focus();
        ImGui::SameLine();
        if (ImGui::Button("Write another report", {half, 0.f})) {
            Act(OverlayAction::Kind::LauncherReportNew);
            report_title.fill(0);
            report_what.fill(0);
            report_where.fill(0);
            report_setup.fill(0);
            report_media.fill(0);
            report_own_dump = report_log_checked = false;
            report_log.clear();
        }
        Note("On the issue's page you can add screenshots or a clip, and answer questions about it.");
        return;
    }

    // Idle, or Failed: the form's fields
    Note("A bug report for msr2ds's GitHub Issues, sent with the log of the play session where the problem happened. "
         "One problem per report, please.");
    if (l.report == Report::Failed && !l.report_note.empty()) {   // (at the top: the page shows from there again)
        wrapped(bad, l.report_note);
        if (ImGui::Button("Open the bug report form on GitHub instead", {-FLT_MIN, 0.f})) {
            Act(OverlayAction::Kind::LauncherReportWebForm);
        }
    } else if (!l.report_note.empty()) {
        Note(l.report_note.c_str());
    }
    Section("SESSION LOG");
    const LauncherSnapshot::SessionLog* chosen = nullptr;   // by its file name; the newest when none was chosen
    for (const auto& log : l.report_logs) {
        if (log.name == report_log) chosen = &log;
    }
    if (!chosen && !l.report_logs.empty()) chosen = &l.report_logs.front();
    Label("The session", label_width);
    if (!chosen) {
        ImGui::TextDisabled("No logs yet: each start of the game writes one");
    } else {
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo("##reportlog", chosen->when.c_str())) {
            for (const auto& log : l.report_logs) {
                if (ImGui::Selectable((log.when + ": " + log.detail + "##" + log.name).c_str(), &log == chosen)) {
                    report_log = log.name;
                }
                if (&log == chosen) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        default_focus();
        if (report_logs_pending) {   // (they came a frame after the tab opened: the list, not the button below it)
            report_logs_pending = false;
            ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
            ImGui::SetNavCursorVisible(true);
        }
        ImGui::SetCursorPosX(label_width);
        if (chosen->ended_badly) wrapped(bad, chosen->detail);
        else wrapped(ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled), chosen->detail);
    }
    Note(l.active ? "Each start of the game writes its own log, named by when it started (the newest 10 are kept). Choose "
                    "the one from the session where the problem happened, not a later start."
                  : "Each start of the game writes its own log. This session's is first: it goes up as it is now. For a "
                    "problem in an earlier session, choose that one.");
#ifndef __ANDROID__
    if (ImGui::Button("Open the logs folder", {-FLT_MIN, 0.f})) {
        Act(OverlayAction::Kind::LauncherReportLogsFolder);
    }
#endif
    if (!chosen) default_focus();

    const float line = ImGui::GetTextLineHeight();
    const auto note_under = [&](const char* text) {   // a field's explanation, under it in its column
        ImGui::SetCursorPosX(label_width);
        ImGui::PushTextWrapPos(0.f);
        Note(text);
        ImGui::PopTextWrapPos();
    };
    // Leaving a field keeps what was typed: ImGui's Escape (and a pad's B, the way out of a box of several lines)
    // puts back what the field held before, which would throw a written report away
    static std::string typed[5];
    const auto keep = [](std::string& kept, char* buf, std::size_t size) {
        if (ImGui::IsItemActive()) {
            kept = buf;
        } else if (ImGui::IsItemDeactivated() && kept != buf) {
            std::snprintf(buf, size, "%s", kept.c_str());
        }
    };
    Section("THE PROBLEM");
    Label("Title", label_width);
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##reporttitle", "Short: what went wrong, and where", report_title.data(), report_title.size());
    keep(typed[0], report_title.data(), report_title.size());
    Label("What happened?", label_width);
    ImGui::InputTextMultiline("##reportwhat", report_what.data(), report_what.size(), {-FLT_MIN, line * 4.5f});
    keep(typed[1], report_what.data(), report_what.size());
    note_under("What you saw or heard, and what you expected instead.");
    Label("Where, and how to make it happen", label_width);
    ImGui::InputTextMultiline("##reportwhere", report_where.data(), report_where.size(), {-FLT_MIN, line * 4.5f});
    keep(typed[2], report_where.data(), report_where.size());
    note_under("The area (or a menu, a cutscene, the launcher) and the moment. Does it happen every time? The steps to "
               "get there help a lot.");
    Section("IF YOU CAN");
    Label("Your setup", label_width);
    ImGui::InputTextMultiline("##reportsetup", report_setup.data(), report_setup.size(), {-FLT_MIN, line * 3.f});
    keep(typed[3], report_setup.data(), report_setup.size());
    note_under("Your controller and whether you start msr2ds through Steam, your display and aspect ratio, the frame "
               "rate, texture packs or game mods. Your PC, graphics, controllers and settings are in the log already.");
    Label("Screenshots or video", label_width);
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##reportmedia", "Links to them", report_media.data(), report_media.size());
    keep(typed[4], report_media.data(), report_media.size());
    note_under("Or add them on the issue's page once it is sent.");

    Section("BEFORE YOU SEND IT");
    OnOff("I'm playing my own decrypted dump of the USA release (CTR-P-A9AE)##reportdump", &report_own_dump);
    OnOff("The log above is from the play session where the problem happened, not from a later start of the game "
          "(I checked its date and time)##reportlogok",
          &report_log_checked);
    Note("The report and the log go on GitHub, where anyone can read them. The log names no account, and it writes "
         "your home folder as ~ or %USERPROFILE%.");

    std::vector<const char*> missing;
    if (!chosen) missing.push_back("a log");
    if (!report_title[0]) missing.push_back("a title");
    if (!report_what[0]) missing.push_back("what happened");
    if (!report_where[0]) missing.push_back("where and how");
    if (!report_own_dump || !report_log_checked) missing.push_back("both checks");
    ImGui::BeginDisabled(!missing.empty());
    ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
    if (ImGui::Button(l.report_signed_in ? "Send the report" : "Sign in to GitHub and send",
                      {-FLT_MIN, ImGui::GetFrameHeight() * 1.5f}) &&
        missing.empty() && chosen) {
        std::string fields = chosen->name;
        for (const char* text : {report_title.data(), report_what.data(), report_where.data(), report_setup.data(),
                                 report_media.data()}) {
            fields += '\x1f';
            fields += text;
        }
        Act(OverlayAction::Kind::LauncherReportSend, 0, fields);
    }
    ImGui::PopStyleColor();
    ImGui::EndDisabled();
    if (!missing.empty()) {
        std::string needs = "To send it: ";
        for (std::size_t i = 0; i < missing.size(); ++i) {
            needs += (i == 0 ? "" : i + 1 == missing.size() ? " and " : ", ") + std::string(missing[i]);
        }
        Note((needs + ".").c_str());
    } else if (l.report == Report::Failed && !l.report_note.empty()) {
        wrapped(bad, l.report_note);   // (and at the top)
    } else if (!l.report_signed_in) {
        Note("GitHub's sign-in page opens with a code to enter: the report then goes out under your GitHub name.");
    }
}

Overlay& GetOverlay() {
    static Overlay overlay;
    return overlay;
}

} // namespace MSR
