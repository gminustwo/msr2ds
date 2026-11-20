// The title menus' touch screen as the game holds it (2026-11-17, the user: rebuild "press A to start" and the file
// select "from the code of the game ... except we don't pull from the bottom screen"): the game's interface is a tree of
// display objects, which this reads each frame and turns into the sprites and glyphs the compositor draws with the
// game's own pictures (gui_assets.cpp). What the game shows there, its animations and its state (the file chosen, a
// file's numbers, the prompt's pulse) come with it. USA release; found from memory snapshots of the title and the
// file select.
//
// The GUI manager (*0x76F240) keeps at +0x244 a vector (count at +0x248) whose first element, in the title menus, is
// the root of their tree (vtable 0x6E5248); in play, the in-game root (0x6E4C98), which holds the HUD and the pause
// menu (IngameMenuComposition). Display objects (all classes):
//   +0x10, +0x14   drawn position, +0x1C, +0x20 drawn size: fractions of the 320x240 screen
//   +0x24, +0x28   drawn scale        +0x30..+0x3C  drawn color (RGBA; may pass 1)
//   +0x40          the screen it is drawn on (1 the touch screen, 2 the top one)
//   +0x44          low byte: visible  +0xD0  its name record's string field (the CRC-32 name hash 4 bytes before it)
// Containers (0x6E5548) and the root list their children at +0xF8 (count), +0xFC (first node), nodes being
// {child, next}; the game draws them in that order.
// Sprites (0x6E5FF8): +0x114 their texture's path hash, +0x118 u, v, width, height (0..1), +0x12C blend (1 additive),
// +0x150 their sprite sheet item's name hash, +0x15D flip x, +0x15E flip y.
// Labels (0x6E5DAC): +0xF8 the text (UTF-16) and +0xFC its length, +0x148 the font object (its resource +0 holds the
// font file's path hash at +8), +0x15C alignment (0 left, 1 centered, 2 right). Text starts at the box's left, middle
// or right by the glyphs' advances, in lines of 1.2 times the font's size (broken at | or \n, as the game's texts mark
// them, and a word wrapped where it would pass the box), the block centered in the box where it is shorter, else from
// its top; a line's baseline is the font's ascent below its top. @[NAME] embeds a sprite (the buttons' icons), its
// bottom a pixel below the baseline.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>
#include "msr.h"
#include "cpu.h"
extern "C" {
#include "kernel.h"
}

namespace {

using u32 = std::uint32_t;

constexpr u32 GuiManagerPointer = 0x76F240;
constexpr u32 RootVtable = 0x6E5248, IngameRootVtable = 0x6E4C98, ContainerVtable = 0x6E5548, SpriteVtable = 0x6E5FF8,
              LabelVtable = 0x6E5DAC, TextVtable = 0x6E5CA0, GridVtable = 0x6E4DB8;
constexpr u32 ObjectSize = 0x160;   // the fields read here lie below
constexpr std::size_t MaxItems = 6000, MaxNodes = 2400;
// Containers that clip what they hold to their rectangle (by name hash): the pause map's view (MinimapHang)
constexpr u32 ClippingContainers[] = {0x1055A6E5u};

struct Clip { float x0, y0, x1, y1; };

std::vector<MsrGuiItem> tree_items, minimap_items;
// the tabs against an edge not shown this frame (the map pins tool's Delete all corner while no pin is placed): what
// lies in them still goes with them
std::vector<MsrGuiItem> hidden_tabs;
std::vector<MsrGuiItem>* out = &tree_items;   // where the walk puts what it finds
bool skip_faint = false;                       // tile grids: leave out the tiles the game draws all but invisible
float dark_shade = 1.f;                        // ...and their dark tiles' colors times this (the minimap's background)
std::size_t nodes = 0;
// The pause map's world map (WorldMapComposition: SR388's areas, the Teleport Station's map too): its opacity this
// walk. It is there, shown, on the map page as well, at opacity 0.
constexpr u32 WorldMapComposition = 0x26965E43u;
float world_map_alpha = 0.f;
// The pause map's view (MinimapHang's rectangle) this walk, and how far past the screen's sides its clip reaches where
// the view reaches them (the pause screen fitted to a wider window: the map fills it, the compositor's msr_gui_map_margin)
float map_view[4] = {0.f, 0.f, 0.f, 0.f};
bool map_view_seen = false;
float map_margin = 0.f;

bool Valid(u32 address) { return address >= 0x00100000 && k_region_state(address) != 0; }
bool ValidRange(u32 address, u32 size) { return Valid(address) && Valid(address + size - 1); }
float F(u32 address) {
    const u32 bits = rd32(address);
    float f;
    std::memcpy(&f, &bits, 4);
    return std::isfinite(f) ? f : 0.f;
}

u32 NameHash(u32 object) {
    const u32 field = rd32(object + 0xD0);
    return field >= 4 && Valid(field - 4) ? rd32(field - 4) : 0;
}

void Push(const MsrGuiItem& item) {
    if (out->size() < MaxItems) out->push_back(item);
}

// A tile grid (0x6E4DB8: the map's rooms): columns (+0x13C) x rows (+0x140) cells over its rectangle, column by
// column, each a tile of its texture (+0x108, a path hash). The mesh it builds (vtable 0x6E78C0) keeps each cell's u, v
// of four corners (32 bytes: u0 v0 u1 v0 u0 v1 u1 v1) from the pointer at its +0x6C. It is reached as the game reaches
// it, by pointers: the grid's +0xF4 points at its drawing object (its second vtable 0x6E8560 at +0x10, whose methods
// 0x174874 and 0x58074C set and release the mesh), whose +0xE8 points at the render object, whose +0x44 points at the
// mesh (a block of 0x104 bytes made at 0x1748C4; its columns at +0x14, rows at +0x18). Each is a block of the game's
// heap: on a fresh heap they lie one after the other right after the grid, elsewhere in a longer session. (Until
// 2026-11-17 the mesh was looked for right after the grid, the user: "areas 1, 2, 3, 4, 6 are missing their maps in
// game", then by a pointer to it inside the grid, the user: "pick a different map look at it. then hit b to go back to
// the game and then pause to look at the map again it disappears": the surface's grid made again had its drawing
// object elsewhere.) Only the cells inside the clip, and none whose tile is wholly transparent (cached per tile).
constexpr u32 GridMeshVtable = 0x6E78C0, GridDrawVtable = 0x6E8560;

u32 GridMesh(u32 o, u32 cols, u32 rows) {
    const u32 draw = rd32(o + 0xF4);
    if (!ValidRange(draw, 0xEC) || rd32(draw + 0x10) != GridDrawVtable) return 0;
    const u32 render = rd32(draw + 0xE8);
    if (!ValidRange(render, 0x48)) return 0;
    const u32 mesh = rd32(render + 0x44);
    return ValidRange(mesh, 0x70) && rd32(mesh) == GridMeshVtable && rd32(mesh + 0x14) == cols && rd32(mesh + 0x18) == rows
               ? mesh : 0;
}

void Grid(u32 o, const MsrGuiItem& base, const Clip& clip) {
    const u32 cols = rd32(o + 0x13C), rows = rd32(o + 0x140);
    if (cols == 0 || rows == 0 || cols > 256 || rows > 256) return;
    const u32 mesh = GridMesh(o, cols, rows);
    if (!mesh) return;
    const u32 cells = rd32(mesh + 0x6C);
    if (!ValidRange(cells, cols * rows * 32)) return;
    const int atlas = msr_gui_atlas_by_path(rd32(o + 0x108));
    const uint8_t* px = nullptr;
    uint32_t aw = 0, ah = 0;
    const unsigned version = atlas < 0 ? 0 : msr_gui_atlas(atlas, &px, &aw, &ah);
    if (!version || !aw || !ah) return;
    // (u0, v0 bits) -> left out, for the atlas as it is now: wholly transparent (a texture pack's tiles are not the
    // game's), or, on the minimap, all but invisible in the game's own atlas: the map's empty cells round its rooms, a
    // tile of alpha 3 in 255 that the HD pack draws as dashes (2026-11-17, the user: "on the minimap, the dash grid
    // lines that exist outside the map aren't needed"; the pause map is left as it was)
    struct Known { u32 key; bool clear, faint, dark; };
    static std::vector<Known> clear_tiles;
    static int clear_atlas = -1;
    static unsigned clear_version = 0;
    if (atlas != clear_atlas || version != clear_version) {
        clear_tiles.clear();
        clear_atlas = atlas;
        clear_version = version;
    }
    const uint8_t* game_alpha = nullptr;
    uint32_t gw = 0, gh = 0;
    if (!msr_gui_atlas_game_alpha(atlas, &game_alpha, &gw, &gh)) game_alpha = nullptr;
    const float cw = base.w / static_cast<float>(cols), ch = base.h / static_cast<float>(rows);
    const float du = .5f / static_cast<float>(aw), dv = .5f / static_cast<float>(ah);
    for (u32 c = 0; c < cols; ++c) {
        const float x = base.x + cw * static_cast<float>(c);
        if (x + cw <= clip.x0 || x >= clip.x1) continue;
        for (u32 r = 0; r < rows; ++r) {
            const float y = base.y + ch * static_cast<float>(r);
            if (y + ch <= clip.y0 || y >= clip.y1) continue;
            const u32 cell = cells + 32 * (c * rows + r);
            const float u0 = F(cell), v0 = F(cell + 4), u1 = F(cell + 8), v1 = F(cell + 20);
            const u32 key = rd32(cell) ^ (rd32(cell + 4) * 2654435761u);
            auto known = std::find_if(clear_tiles.begin(), clear_tiles.end(), [key](const Known& t) { return t.key == key; });
            Known tile{key, true, false, false};
            if (known == clear_tiles.end()) {
                const int tx0 = static_cast<int>(std::min(u0, u1) * aw), tx1 = static_cast<int>(std::max(u0, u1) * aw);
                const int ty0 = static_cast<int>(std::min(v0, v1) * ah), ty1 = static_cast<int>(std::max(v0, v1) * ah);
                for (int ty = std::max(ty0, 0); ty < std::min(ty1, static_cast<int>(ah)) && tile.clear; ++ty)
                    for (int tx = std::max(tx0, 0); tx < std::min(tx1, static_cast<int>(aw)) && tile.clear; ++tx)
                        tile.clear = px[(static_cast<std::size_t>(ty) * aw + tx) * 4 + 3] == 0;
                // dark: no bright pixel in it (the map's fill round the rooms, not a room, an edge nor a door)
                int brightest = 0;
                for (int ty = std::max(ty0, 0); ty < std::min(ty1, static_cast<int>(ah)); ++ty)
                    for (int tx = std::max(tx0, 0); tx < std::min(tx1, static_cast<int>(aw)); ++tx) {
                        const uint8_t* p = px + (static_cast<std::size_t>(ty) * aw + tx) * 4;
                        if (p[3] > 16) brightest = std::max<int>(brightest, std::max({p[0], p[1], p[2]}));
                    }
                tile.dark = !tile.clear && brightest < 72;
                if (game_alpha) {
                    uint8_t most = 0;
                    const int gx0 = static_cast<int>(std::min(u0, u1) * gw), gx1 = static_cast<int>(std::max(u0, u1) * gw);
                    const int gy0 = static_cast<int>(std::min(v0, v1) * gh), gy1 = static_cast<int>(std::max(v0, v1) * gh);
                    for (int ty = std::max(gy0, 0); ty < std::min(gy1, static_cast<int>(gh)); ++ty)
                        for (int tx = std::max(gx0, 0); tx < std::min(gx1, static_cast<int>(gw)); ++tx)
                            most = std::max(most, game_alpha[static_cast<std::size_t>(ty) * gw + tx]);
                    tile.faint = most <= 8;
                }
                if (clear_tiles.size() < 4096) clear_tiles.push_back(tile);
            } else {
                tile = *known;
            }
            if (tile.clear || (skip_faint && tile.faint)) continue;
            MsrGuiItem it = base;
            it.x = x; it.y = y; it.w = cw; it.h = ch;
            it.uv[0] = u0 + du; it.uv[1] = v0 + dv; it.uv[2] = u1 - du; it.uv[3] = v1 - dv;
            it.atlas = atlas;
            it.additive = 0;
            if (tile.dark)
                for (int k = 0; k < 3; ++k) it.color[k] *= dark_shade;
            Push(it);
        }
    }
}

void Sprite(u32 o, const MsrGuiItem& base) {
    MsrGuiItem it = base;
    // A sprite's drawn scale (+0x24, +0x28): its drawn position is the scaled picture's corner, its drawn size the
    // picture's own. 2026-11-19, the user: the circle on the map "is off" once an area's last Metroid is gone. That ring
    // (ef69a551) pulses from 0.3 to 0.65 of its 40x40 about one center (195, 155 in the trace: at 0.3 its corner was at
    // 189, 149, at 0.65 at 182, 142); drawn at 40x40 from that corner it stood off down and right and never grew.
    const float sx = F(o + 0x24), sy = F(o + 0x28);
    if (sx > 0.f && sx < 64.f) it.w = base.w * sx;
    if (sy > 0.f && sy < 64.f) it.h = base.h * sy;
    it.atlas = msr_gui_atlas_by_path(rd32(o + 0x114));
    if (it.atlas < 0) return;   // a texture not loaded here: left out
    float uv[4] = {F(o + 0x118), F(o + 0x11C), 0.f, 0.f};
    uv[2] = uv[0] + F(o + 0x120);
    uv[3] = uv[1] + F(o + 0x124);
    {   // drawn larger than the game draws it: pulled in by half a texel, so the filtering at its edges does not take in
        // the next sprite (gui_assets.cpp Inset; the footer's ornaments took a line of what lies above them)
        const uint8_t* px = nullptr;
        uint32_t aw = 0, ah = 0;
        if (msr_gui_atlas(it.atlas, &px, &aw, &ah) && aw && ah) {
            const float du = .5f / static_cast<float>(aw), dv = .5f / static_cast<float>(ah);
            if (uv[2] - uv[0] > 2 * du) { uv[0] += du; uv[2] -= du; }
            if (uv[3] - uv[1] > 2 * dv) { uv[1] += dv; uv[3] -= dv; }
        }
    }
    if (rd8(o + 0x15D)) std::swap(uv[0], uv[2]);
    if (rd8(o + 0x15E)) std::swap(uv[1], uv[3]);
    std::memcpy(it.uv, uv, sizeof uv);
    it.additive = rd32(o + 0x12C) == 1;
    it.sprite = rd32(o + 0x150);
    Push(it);
}

// A label's text: CLabel (0x6E5DAC: text +0xF8, its length +0xFC, font +0x148, alignment +0x15C) or the HUD's CText
// (0x6E5CA0: +0x12C, +0x130, +0x11C, +0x120)
struct TextFields { u32 text, length, font, align; };
constexpr TextFields LabelFields{0xF8, 0xFC, 0x148, 0x15C}, TextFieldsHud{0x12C, 0x130, 0x11C, 0x120};

void Label(u32 o, const MsrGuiItem& base, const TextFields& at) {
    const u32 text = rd32(o + at.text), length = rd32(o + at.length);
    if (length == 0 || length > 256 || !ValidRange(text, length * 2)) return;
    const u32 font_object = rd32(o + at.font);
    if (!ValidRange(font_object, 4)) return;
    const u32 resource = rd32(font_object);
    if (!ValidRange(resource, 12)) return;
    const int font = msr_gui_font_by_file(rd32(resource + 8));
    float size = 0.f, ascent = 0.f;
    if (font < 0 || !msr_gui_font(font, &size, &ascent)) return;
    const float sx = F(o + 0x24) > 0.f ? F(o + 0x24) : 1.f, sy = F(o + 0x28) > 0.f ? F(o + 0x28) : 1.f;
    const u32 align = rd32(o + at.align);
    MsrGuiItem labelled = base;   // (a text in no narrower group keeps to the side it is aligned to)
    if (labelled.anchor == MSR_ANCHOR_STRETCH)
        labelled.anchor = align == 1 ? MSR_ANCHOR_MIDDLE : align == 2 ? MSR_ANCHOR_RIGHT : MSR_ANCHOR_LEFT;

    // the text's pieces: glyphs, embedded sprites, spaces and line breaks
    enum Kind { Glyph, Sprite, Space, Break };
    struct Piece { Kind kind; MsrGlyph g; int atlas; float uv[4], w, h, advance; u32 icon; };
    std::vector<Piece> all;
    all.reserve(length);
    for (u32 i = 0; i < length; ++i) {
        const unsigned c = rd16(text + 2 * i);
        Piece p{};
        if (c == '@' && i + 1 < length && rd16(text + 2 * i + 2) == '[') {
            char name[32];
            u32 n = 0, j = i + 2;
            for (; j < length && n < sizeof name - 1; ++j) {
                const unsigned d = rd16(text + 2 * j);
                if (d == ']') break;
                name[n++] = static_cast<char>(d);
            }
            if (j < length && msr_gui_embedded(name, n, &p.atlas, p.uv, &p.w, &p.h)) {
                p.kind = Sprite;
                p.advance = p.w;
                // (a face or shoulder button's icon: its sheet item's name hash, for the compositor's ControlsIcons,
                // which shows play's button in the pause's descriptions: "Press @[Y] rapidly to fire")
                static const struct { const char* name; u32 hash; } Buttons[] = {
                    {"A", 0x2C266174u}, {"B", 0xB52F30CEu}, {"X", 0x484DC9B4u}, {"Y", 0x3F4AF922u}, {"L", 0x52971DC9u}, {"R", 0xA89820AAu}};
                for (const auto& button : Buttons)
                    if (std::strlen(button.name) == n && !std::strncmp(button.name, name, n)) p.icon = button.hash;
                all.push_back(p);
                i = j;
                continue;
            }
        }
        if (c == '\n' || c == '|') {   // (the game's texts break their lines with |: "even|more powerful")
            p.kind = Break;
            all.push_back(p);
            continue;
        }
        if (c == '\r' || !msr_gui_glyph(font, c, &p.g)) continue;
        p.kind = c == ' ' ? Space : Glyph;
        p.advance = p.g.advance;
        all.push_back(p);
    }
    // Words wrapped at the box's width, as the game wraps them (the prologue's pages; a line that fits stays whole:
    // the copy prompt, 245 of its 252)
    struct Line { std::vector<Piece> pieces; float width = 0.f; };
    std::vector<Line> lines(1);
    const float max_width = base.w / sx + .5f;
    const auto trim = [](Line& line) {
        while (!line.pieces.empty() && line.pieces.back().kind == Space) {
            line.width -= line.pieces.back().advance;
            line.pieces.pop_back();
        }
    };
    for (std::size_t i = 0; i < all.size() && lines.size() <= 32;) {
        if (all[i].kind == Break) {
            trim(lines.back());
            lines.emplace_back();
            ++i;
            continue;
        }
        if (all[i].kind == Space) {
            if (!lines.back().pieces.empty()) {
                lines.back().pieces.push_back(all[i]);
                lines.back().width += all[i].advance;
            }
            ++i;
            continue;
        }
        std::size_t end = i;
        float word = 0.f;
        while (end < all.size() && (all[end].kind == Glyph || all[end].kind == Sprite)) word += all[end++].advance;
        if (!lines.back().pieces.empty() && lines.back().width + word > max_width) {
            trim(lines.back());
            lines.emplace_back();
        }
        for (; i < end; ++i) {
            lines.back().pieces.push_back(all[i]);
            lines.back().width += all[i].advance;
        }
    }
    trim(lines.back());
    // Lines apart by the label's line height (CLabel +0x158, as the game works it out: 18 for the 20-pixel fonts, 15
    // for digital_medium; else 0.9 of the size), the block (the lines above the last, and 1.2 times the size for it)
    // centered in the box where it is shorter, else from its top. Measured: one line in boxes 14 to 24 pixels tall
    // starts at their top, the copy prompt's in its 90 is centered, the Samus page's two-line description 15 apart.
    const u32 game_line = at.text == LabelFields.text ? rd32(o + 0x158) : 0;
    const float line_height = (game_line >= 4 && game_line <= 64 ? static_cast<float>(game_line) : std::round(size * .9f)) * sy;
    const float block = line_height * static_cast<float>(lines.size() - 1) + std::round(size * 1.2f) * sy;
    const float top = base.y + std::max(0.f, std::floor((base.h - block) / 2));
    for (std::size_t l = 0; l < lines.size(); ++l) {
        const Line& line = lines[l];
        const float bx = base.x, bw = base.w;
        float pen = align == 1 ? bx + (bw - line.width * sx) / 2 : align == 2 ? bx + bw - line.width * sx : bx;
        const float baseline = top + ascent * sy + line_height * static_cast<float>(l);
        for (const Piece& p : line.pieces) {
            if (p.kind == Space) {
                pen += p.advance * sx;
                continue;
            }
            MsrGuiItem it = labelled;
            if (p.kind == Sprite) {
                it.atlas = p.atlas;
                it.sprite = p.icon;
                std::memcpy(it.uv, p.uv, sizeof p.uv);
                it.x = pen; it.y = baseline + sy - p.h * sy; it.w = p.w * sx; it.h = p.h * sy;
                it.additive = 1;
                pen += p.w * sx;
            } else {
                it.atlas = MSR_ATLAS_FONT;
                std::memcpy(it.uv, p.g.uv, sizeof p.g.uv);
                it.x = pen + p.g.left * sx; it.y = baseline - p.g.top * sy; it.w = p.g.w * sx; it.h = p.g.h * sy;
                it.additive = 0;
                pen += p.g.advance * sx;
            }
            it.glyph = 1;
            Push(it);
        }
    }
}

bool Leaf(u32 vt) { return vt == SpriteVtable || vt == LabelVtable || vt == TextVtable || vt == GridVtable; }

// Where a piece goes on a touch screen drawn wider than 4:3 (2026-11-17, the user: the pause screen stretched to the
// window "looks weird. see if you can work on resizing it to look normal for the resolution of the screen instead"):
// as the 3DS draws it, in the middle, unless the first object on its way down no wider than 60% of the screen is against
// the screen's left or right edge (the tabs at the corners and what they hold go to the window's corners); -1 for a
// wider one (decided below it). The toolbar, the pins' bar, the columns of items and the counters keep together so.
// A piece (leaf) or a group's: its ends where msr_anchor_end puts them; one against an edge and narrower than 40% of the
// screen goes to that edge (a tab), wider it is stretched between its ends (a bar, a bar's half reaching the middle). A
// group (container) decides for what it holds only when it is that narrow (a container's rectangle is not always where
// its children are: a wide one leaves it to them); -1: undecided.
// A line (a sprite at most 12 pixels tall, four times as wide) from an edge into the middle is stretched too, whatever
// its width: the titles' headers run a line from the edge to their tab (HeaderUp, 92 x 10), which kept to the edge
// stood apart from it (2026-11-17, the user: "bars appearing on the top of the screen in some pause screens").
constexpr float TabWidth = 128.f;
int AnchorOfRect(float x, float w, float h, bool leaf, bool sprite) {
    if (!(w > 0.f) || !(h > 0.f) || !(x > -1e4f && x < 1e4f)) return leaf ? MSR_ANCHOR_MIDDLE : -1;
    const int left = msr_anchor_end(x), right = msr_anchor_end(x + w);
    if (!leaf && w > TabWidth) return -1;
    if (left == right) return left;
    if (w > TabWidth || (sprite && h <= 12.f && w >= 4.f * h)) return MSR_ANCHOR_STRETCH;
    return left != MSR_ANCHOR_MIDDLE ? left : right;
}
int AnchorOf(u32 o) {
    const u32 vt = rd32(o);
    return AnchorOfRect(F(o + 0x10) * 320.f, F(o + 0x1C) * 320.f, F(o + 0x20) * 240.f, Leaf(vt), vt == SpriteVtable);
}
// ...the map's view and what it holds, and the pin a finger holds over it (MarkerComposition, MarkerCell): the map's
constexpr u32 MapAnchored[] = {0x1055A6E5u /* MinimapHang */, 0xB9370011u /* MarkerComposition */, 0xCA37DA0Au /* MarkerCell */};
// Containers whose pieces the compositor tells apart (MsrGuiItem.group): the pause's Controls page, whose icons name
// play's buttons (compositor.cpp ControlsIcons)
constexpr u32 GroupContainers[] = {0xB1E17D10u /* ControlsComposition */};

void Walk(u32 o, int depth, Clip clip, int anchor = -1, u32 group = 0) {
    if (depth > 32 || ++nodes > MaxNodes || !ValidRange(o, ObjectSize)) return;
    const u32 vt = rd32(o);
    if (vt < 0x600000 || vt >= 0x800000) return;   // not an object of the game's
    if ((rd32(o + 0x44) & 0xFF) == 0) return;      // hidden, with what it holds
    if (anchor != MSR_ANCHOR_MAP) {
        const u32 name = NameHash(o);
        if (std::find(std::begin(MapAnchored), std::end(MapAnchored), name) != std::end(MapAnchored)) anchor = MSR_ANCHOR_MAP;
        else if (anchor < 0) anchor = AnchorOf(o);
    }
    if (Leaf(vt)) {
        if (rd32(o + 0x40) != 1) return;   // the top screen's
        MsrGuiItem base{};
        base.x = F(o + 0x10) * 320.f;
        base.y = F(o + 0x14) * 240.f;
        base.w = F(o + 0x1C) * 320.f;
        base.h = F(o + 0x20) * 240.f;
        for (int k = 0; k < 4; ++k) base.color[k] = F(o + 0x30 + 4 * k);
        if (base.color[3] <= 0.f) {
            // (a tab's shape only: short, in the top or bottom bar; the game hides other pieces against an edge)
            if (vt == SpriteVtable && (anchor == MSR_ANCHOR_LEFT || anchor == MSR_ANCHOR_RIGHT) && out == &tree_items &&
                base.h <= 40.f && (base.y + base.h <= 40.f || base.y >= 200.f)) {
                base.anchor = anchor;
                hidden_tabs.push_back(base);
            }
            return;
        }
        base.name = NameHash(o);
        base.group = group;
        // (a leaf always decides, AnchorOf; a label stretched keeps to its alignment, Label)
        base.anchor = anchor >= 0 ? anchor : MSR_ANCHOR_MIDDLE;
        base.clip[0] = clip.x0; base.clip[1] = clip.y0; base.clip[2] = clip.x1; base.clip[3] = clip.y1;
        if (base.x >= clip.x1 || base.y >= clip.y1 || base.x + base.w <= clip.x0 || base.y + base.h <= clip.y0) {
            if (vt != LabelVtable && vt != TextVtable) return;   // (a label's text may pass its box)
        }
        if (vt == SpriteVtable) Sprite(o, base);
        else if (vt == LabelVtable) Label(o, base, LabelFields);
        else if (vt == TextVtable) Label(o, base, TextFieldsHud);
        else Grid(o, base, clip);
        return;
    }
    // a container (any other class holding children): +0xF8 count, +0xFC the first {child, next} node
    if (out == &tree_items && NameHash(o) == WorldMapComposition) world_map_alpha = std::max(world_map_alpha, F(o + 0x3C));
    const u32 count = rd32(o + 0xF8);
    u32 node = rd32(o + 0xFC);
    if (count == 0 || count > 2000 || !ValidRange(node, 8)) return;
    for (const u32 clipping : ClippingContainers) {
        if (NameHash(o) != clipping) continue;
        const float x = F(o + 0x10) * 320.f, y = F(o + 0x14) * 240.f, w = F(o + 0x1C) * 320.f, h = F(o + 0x20) * 240.f;
        if (out == &tree_items) {
            map_view[0] = x; map_view[1] = y; map_view[2] = x + w; map_view[3] = y + h;
            map_view_seen = true;
        }
        // (on a wider pause screen the view reaches past the screen's sides where it reaches them: the map fills it)
        const float wide = out == &tree_items ? map_margin : 0.f;
        clip = {std::max(clip.x0, x <= 2.f ? x - wide : x), std::max(clip.y0, y),
                std::min(clip.x1, x + w >= 318.f ? x + w + wide : x + w), std::min(clip.y1, y + h)};
    }
    {
        const u32 name = NameHash(o);
        if (std::find(std::begin(GroupContainers), std::end(GroupContainers), name) != std::end(GroupContainers)) group = name;
    }
    for (int guard = 0; node && guard < 2000 && ValidRange(node, 8); ++guard) {
        Walk(rd32(node), depth + 1, clip, anchor, group);
        node = rd32(node + 4);
    }
}

// The in-game root, or 0
u32 IngameRoot() {
    if (!Valid(GuiManagerPointer)) return 0;
    const u32 manager = rd32(GuiManagerPointer);
    if (!ValidRange(manager, 0x250)) return 0;
    const u32 list = rd32(manager + 0x244), count = rd32(manager + 0x248);
    if (count == 0 || count > 64 || !ValidRange(list, 4)) return 0;
    const u32 root = rd32(list);
    return ValidRange(root, ObjectSize) && rd32(root) == IngameRootVtable ? root : 0;
}

// A container's child by its name hash, or 0
u32 Child(u32 o, u32 name) {
    if (!ValidRange(o, ObjectSize)) return 0;
    const u32 count = rd32(o + 0xF8);
    u32 node = rd32(o + 0xFC);
    if (count == 0 || count > 2000) return 0;
    for (int guard = 0; node && guard < 2000 && ValidRange(node, 8); ++guard) {
        const u32 child = rd32(node);
        if (ValidRange(child, ObjectSize) && NameHash(child) == name) return child;
        node = rd32(node + 4);
    }
    return 0;
}

} // namespace

// The HUD's minimap as the game holds it (2026-11-17, the user: "do the minimap the same way"): in play the touch
// screen's map view is IngameMenuComposition / IngameMinimap / MinimapHang (the pause map's too, moved and grown
// there), which holds the map (its tile grid, doors, items, Samus's icon and the rest). Only that is walked, each
// frame of play (the HUD's other objects are not read); found by its names once and kept while it is still there.
extern "C" int msr_gui_minimap(const MsrGuiItem** items_out, float view[4], float shade) {
    static u32 hang = 0;
    constexpr u32 IngameMenuComposition = 0x5AB5381Fu, IngameMinimap = 0x7FFAF1D6u, MinimapHang = 0x1055A6E5u;
    *items_out = nullptr;
    minimap_items.clear();
    if (!hang || !ValidRange(hang, ObjectSize) || rd32(hang) != ContainerVtable || NameHash(hang) != MinimapHang) {
        hang = 0;
        const u32 root = IngameRoot();
        if (!root) return 0;
        hang = Child(Child(Child(root, IngameMenuComposition), IngameMinimap), MinimapHang);
        if (!hang) return 0;
    }
    if ((rd32(hang + 0x44) & 0xFF) == 0) return 0;
    view[0] = F(hang + 0x10) * 320.f;
    view[1] = F(hang + 0x14) * 240.f;
    view[2] = F(hang + 0x1C) * 320.f;
    view[3] = F(hang + 0x20) * 240.f;
    out = &minimap_items;
    skip_faint = true;
    dark_shade = shade;
    nodes = 0;
    // (MinimapHang clips to its view in Walk; the caller wants the map past it: walked from its children)
    const u32 count = rd32(hang + 0xF8);
    u32 node = rd32(hang + 0xFC);
    for (int guard = 0; count && count < 2000 && node && guard < 2000 && ValidRange(node, 8); ++guard) {
        Walk(rd32(node), 1, Clip{-1e9f, -1e9f, 1e9f, 1e9f});
        node = rd32(node + 4);
    }
    out = &tree_items;
    skip_faint = false;
    dark_shade = 1.f;
    *items_out = minimap_items.data();
    return static_cast<int>(minimap_items.size());
}

extern "C" int msr_gui_tree(const MsrGuiItem** items_out) {
    std::vector<MsrGuiItem>& items = tree_items;
    items.clear();
    hidden_tabs.clear();
    world_map_alpha = 0.f;
    map_view_seen = false;
    out = &tree_items;
    nodes = 0;
    *items_out = nullptr;
    if (!Valid(GuiManagerPointer)) return 0;
    const u32 manager = rd32(GuiManagerPointer);
    if (!ValidRange(manager, 0x250)) return 0;
    const u32 list = rd32(manager + 0x244), count = rd32(manager + 0x248);
    if (count == 0 || count > 64 || !ValidRange(list, 4)) return 0;
    const u32 root = rd32(list);
    if (!ValidRange(root, ObjectSize) || (rd32(root) != RootVtable && rd32(root) != IngameRootVtable)) return 0;
    Walk(root, 0, Clip{-1e9f, -1e9f, 1e9f, 1e9f});
    // a piece lying in one kept to an edge goes with it (a tab's button icons: siblings, not its children), shown or not
    for (const MsrGuiItem& it : items)
        if (!it.glyph && (it.anchor == MSR_ANCHOR_LEFT || it.anchor == MSR_ANCHOR_RIGHT) && it.w <= TabWidth) hidden_tabs.push_back(it);
    for (MsrGuiItem& it : items) {
        if (it.glyph || it.anchor != MSR_ANCHOR_MIDDLE) continue;
        for (const MsrGuiItem& edge : hidden_tabs) {
            if (it.x >= edge.x - 2.f && it.x + it.w <= edge.x + edge.w + 2.f && it.y >= edge.y - 2.f &&
                it.y + it.h <= edge.y + edge.h + 2.f) {
                it.anchor = edge.anchor;
                break;
            }
        }
    }
    *items_out = items.data();
    return static_cast<int>(items.size());
}

// The world map's opacity in msr_gui_tree's last walk (0 when it is not shown)
extern "C" float msr_gui_world_map(void) {
    return world_map_alpha;
}

// The pause map's view in msr_gui_tree's last walk (x0, y0, x1, y1, touch-screen pixels): 1, or 0 where there was none
extern "C" int msr_gui_map_view(float view[4]) {
    if (!map_view_seen) return 0;
    std::memcpy(view, map_view, sizeof map_view);
    return 1;
}

// The pause map's view widened by so many touch-screen pixels at each side it reaches (next walks)
extern "C" void msr_gui_map_margin(float margin) {
    map_margin = margin > 0.f ? margin : 0.f;
}
