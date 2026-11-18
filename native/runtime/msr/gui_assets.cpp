// The game's own interface pictures on msr-native: its sprite sheets and fonts, which the compositor draws from the
// state the game holds instead of copying the touch screen. Begun 2026-11-16 for the beam icons (the copy garbled the
// beam row under Beam Burst and grayed it unevenly in Morph Ball, the user's recording), then the DNA and Metroid
// counters (2026-11-17).
//
// Atlases (MSR_ATLAS_*): MTXT wrappers (0x80 bytes) around a CTPK whose first texture is decoded here.
//   texturehudopaque.bctex: 1024x512 L8, white sprites on black that the game draws additively (the beam icons);
//   texturehud.bctex: 512x256 RGBA8 (the counters' icons, the Metroid radar, the flares);
//   system/fonts/textures/defaultfnt.bctex, inside packs/system/system.pkg: 512x256 LA8, the glyphs of every font;
//   white.bctex and texturehudmainmenu.bctex, which the title menus' sprites also take (gui_tree.cpp).
// A package (.pkg) is a table of (name hash, start, end) after a 12-byte header (header size, data size, count); the
// hash is CRC-32 of the path without the final inversion. Files come from the RomFS the game reads (with the game mods'
// files, layered_fs.cpp), on a worker. A texture pack's replacement of an atlas is found by the same hash the renderer
// looks textures up with (textures.cpp) and takes its place once loaded.
//
// Fonts (MSR_FONT_*): gui/scripts/guimanager.lua's AddFont: digital_hefty is system/fonts/mlsix16.bfont and
// digital_big mlsix18 (the game's font objects name them), digital_medium mlsix14, digital_small mlsix10. A label's
// baseline is 0.8 of its font's size below its top (16 for both 20-pixel fonts, measured on the touch screen's counters,
// file select and title). An MFNT holds the atlas' size, the font's size (+0x18), its glyph count (+0x1C)
// and the offset of its glyph records (+0x20): 14 bytes each, x, y, w, h (u16), left bearing and height of the top above
// the baseline (s16), advance (u16), in the atlas' pixels. system/fonts/symbols/glyphtable.buct (MUCT, in the
// discardables package) maps characters to glyph numbers: count at +8, offset at +0xC, (code, glyph) u32 pairs.
//
// The sprites' rectangles are gui/scripts/sprites_*.lc's (USA release), in pixels of the atlas from its top-left.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <utility>
#include <vector>
#include "msr.h"

namespace {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

struct AtlasDef {
    const char* path;      // the RomFS file, or the file's path inside package
    const char* package;   // nullptr: a file of its own
    float coord_w, coord_h;   // the sprites' coordinates are in these
};
constexpr AtlasDef Atlases[MSR_ATLAS_COUNT] = {
    {"gui/textures/texturehudopaque.bctex", nullptr, 1024, 512},
    {"gui/textures/texturehud.bctex", nullptr, 512, 256},
    {"system/fonts/textures/defaultfnt.bctex", "packs/system/system.pkg", 512, 256},
    {"gui/textures/white.bctex", nullptr, 1, 1},
    {"gui/textures/texturehudmainmenu.bctex", nullptr, 256, 256},
};

struct SpriteDef { int atlas; float u, v, w, h; };
constexpr SpriteDef Sprites[MSR_SPRITE_COUNT] = {
    {MSR_ATLAS_HUD_OPAQUE, 570, 0, 50, 50},   // Icon_PowerBeam
    {MSR_ATLAS_HUD_OPAQUE, 320, 0, 50, 50},   // Icon_IceBeam
    {MSR_ATLAS_HUD_OPAQUE, 370, 0, 50, 50},   // Icon_grapplebeam
    {MSR_ATLAS_HUD_OPAQUE, 670, 0, 50, 50},   // Icon_WaveBeam
    {MSR_ATLAS_HUD_OPAQUE, 620, 0, 50, 50},   // Icon_SpazerBeam
    {MSR_ATLAS_HUD_OPAQUE, 520, 0, 50, 50},   // Icon_PlasmaBeam
    {MSR_ATLAS_HUD, 135, 146, 15, 18},        // Icon_MetroidDNACount
    {MSR_ATLAS_HUD, 214, 143, 20, 22},        // Icon_Metroid_Base
    {MSR_ATLAS_HUD, 174, 143, 20, 22},        // Icon_Metroid_Color
    {MSR_ATLAS_HUD, 194, 143, 20, 22},        // Icon_Metroid_Highlight
    {MSR_ATLAS_HUD, 104, 113, 140, 30},       // Flare_01
    {MSR_ATLAS_HUD_OPAQUE, 110, 378, 20, 22}, // Icon_Erase (the file select's Delete: a bin)
    {MSR_ATLAS_HUD, 3, 181, 61, 25},          // ButtonUpWindow (a popup's own "✔ Ⓐ" box)
};

struct FontDef { const char* path; };
constexpr const char* FontPackage = "packs/system/system_discardables.pkg";
constexpr const char* GlyphTablePath = "system/fonts/symbols/glyphtable.buct";
constexpr FontDef Fonts[MSR_FONT_COUNT] = {
    {"system/fonts/mlsix16.bfont"},   // digital_hefty
    {"system/fonts/mlsix18.bfont"},   // digital_big
    {"system/fonts/mlsix14.bfont"},   // digital_medium
    {"system/fonts/mlsix10.bfont"},   // digital_small
};

// The sprites a label's text may embed, @[NAME] (the buttons' icons: "Press @[A_ALT] to Start"), from
// sprites_texturehudopaque.lc, in its 1024x512 pixels
struct Embedded { const char* name; float u, v, w, h; };
constexpr Embedded EmbeddedSprites[] = {
    {"A", 705, 65, 15, 15},       {"A_ALT", 705, 65, 15, 15},  {"B", 690, 65, 15, 15},        {"X", 705, 50, 15, 15},
    {"Y", 690, 50, 15, 15},       {"L", 720, 65, 18, 15},      {"R", 720, 50, 18, 15},        {"LS", 755, 50, 15, 15},
    {"D", 757, 80, 15, 15},       {"D-LEFT", 660, 50, 15, 15}, {"D-RIGHT", 675, 50, 15, 15},  {"D-DOWN", 660, 65, 15, 15},
    {"D-UP", 675, 65, 15, 15},    {"IconBack", 738, 65, 17, 15}, {"IconOK", 755, 65, 17, 15}, {"Arrow_R_L", 738, 50, 17, 15},
    {"Button_Arrow", 775, 63, 9, 9}, {"IconBackingame", 740, 80, 17, 15},
};

struct Glyph { u16 x, y, w, h; std::int16_t left, top; u16 advance; };
struct Font {
    std::vector<Glyph> glyphs;
    float size = 0;
};

struct Atlas {
    // the worker's
    std::vector<u8> decoded;   // RGBA8, rows top-down
    u32 decoded_w = 0, decoded_h = 0;
    std::vector<u8> level0;    // the texture's own bytes (the pack lookup's hash)
    int level0_fmt = -1;
    // the main thread's
    std::vector<u8> rgba;
    u32 w = 0, h = 0;
    unsigned version = 0;
    std::vector<u8> game_alpha;   // the game's own coverage, kept when a pack's pixels come (w x h of the game's)
    u32 game_w = 0, game_h = 0;
    u64 pack_key = 0, content = 0;
    // The player's device's face-button icons (glyphs.cpp, as pica_vk.c draws them into the game's own uploads of the
    // font and interface atlases): which sheet this is (0 none), its pixels before them, and the icons' version drawn
    std::vector<u8> pristine;
    int glyph_sheet = -1;
    unsigned glyph_version = 0;
    int pack_state = 0;   // 0 to look up, 1 waiting for the hash, 2 waiting for the pixels, 3 done
};

std::atomic<int> load_state{0};   // 0 not started, 1 loading, 2 loaded (main thread may take it)
Atlas atlases[MSR_ATLAS_COUNT];
Font fonts[MSR_FONT_COUNT];
std::vector<std::pair<u32, u32>> glyph_table;   // (character, glyph), sorted
bool taken = false;

u32 Le32(const u8* p) { return static_cast<u32>(p[0]) | p[1] << 8 | p[2] << 16 | static_cast<u32>(p[3]) << 24; }
u32 Le16(const u8* p) { return static_cast<u32>(p[0]) | p[1] << 8; }

// The packages' name hash: CRC-32 without the final inversion
u32 NameHash(const char* path) {
    u32 crc = 0xFFFFFFFFu;
    for (const char* c = path; *c; ++c) {
        crc ^= static_cast<u8>(*c);
        for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return crc;
}

// A file: the RomFS's, or one inside a package (read whole). Empty when it is not there.
struct Files {
    std::vector<std::pair<const char*, std::vector<u8>>> packages;   // read once each
    std::vector<u8> Get(const char* path, const char* package) {
        if (!package) {
            std::size_t size = 0;
            u8* file = msr_romfs_file(path, &size);
            std::vector<u8> out(file, file + (file ? size : 0));
            std::free(file);
            return out;
        }
        auto it = std::find_if(packages.begin(), packages.end(), [package](const auto& p) { return std::strcmp(p.first, package) == 0; });
        if (it == packages.end()) {
            std::size_t size = 0;
            u8* file = msr_romfs_file(package, &size);
            packages.emplace_back(package, std::vector<u8>(file, file + (file ? size : 0)));
            std::free(file);
            it = packages.end() - 1;
        }
        const std::vector<u8>& pkg = it->second;
        if (pkg.size() < 12) return {};
        const u32 count = Le32(&pkg[8]), hash = NameHash(path);
        if (count > (pkg.size() - 12) / 12) return {};
        for (u32 i = 0; i < count; ++i) {
            const u8* e = &pkg[12 + 12 * i];
            if (Le32(e) != hash) continue;
            const u32 start = Le32(e + 4), end = Le32(e + 8);
            if (start > end || end > pkg.size()) return {};
            return std::vector<u8>(pkg.begin() + start, pkg.begin() + end);
        }
        return {};
    }
};

// The PICA texel formats a sprite atlas may come in (not ETC1): one texel as RGBA8, the renderer's own expansions
// (pica_sw.c decode_tile_rows); bits per texel in bpp
bool Texel(int fmt, const u8* data, std::size_t index, u8 out[4], u32* bpp) {
    static constexpr u32 Bits[12] = {32, 24, 16, 16, 16, 16, 16, 8, 8, 8, 4, 4};
    if (fmt < 0 || fmt > 11) return false;
    *bpp = Bits[fmt];
    const u8* p = data + index * Bits[fmt] / 8;
    const auto x5 = [](u32 v) { return static_cast<u8>((v << 3) | (v >> 2)); };
    const auto x4 = [](u32 v) { return static_cast<u8>(v * 17); };
    u32 v;
    switch (fmt) {
    case 0: out[0] = p[3]; out[1] = p[2]; out[2] = p[1]; out[3] = p[0]; break;
    case 1: out[0] = p[2]; out[1] = p[1]; out[2] = p[0]; out[3] = 255; break;
    case 2: v = Le16(p); out[0] = x5(v >> 11); out[1] = x5(v >> 6 & 31); out[2] = x5(v >> 1 & 31); out[3] = (v & 1) ? 255 : 0; break;
    case 3: v = Le16(p); out[0] = x5(v >> 11); out[1] = static_cast<u8>((v >> 5 & 63) << 2 | (v >> 9 & 3)); out[2] = x5(v & 31); out[3] = 255; break;
    case 4: v = Le16(p); out[0] = x4(v >> 12); out[1] = x4(v >> 8 & 15); out[2] = x4(v >> 4 & 15); out[3] = x4(v & 15); break;
    case 5: out[0] = out[1] = out[2] = p[1]; out[3] = p[0]; break;
    case 6: out[0] = p[1]; out[1] = p[0]; out[2] = 0; out[3] = 255; break;
    case 7: out[0] = out[1] = out[2] = p[0]; out[3] = 255; break;
    case 8: out[0] = out[1] = out[2] = 255; out[3] = p[0]; break;
    case 9: out[0] = out[1] = out[2] = x4(p[0] >> 4); out[3] = x4(p[0] & 15); break;
    case 10: out[0] = out[1] = out[2] = x4((index & 1) ? p[0] >> 4 : p[0] & 15); out[3] = 255; break;
    default: out[0] = out[1] = out[2] = 255; out[3] = x4((index & 1) ? p[0] >> 4 : p[0] & 15); break;   // A4
    }
    return true;
}

// An MTXT's first texture, decoded by 8x8 tiles in memory order (Morton order inside each). The sprites' coordinates
// count from the top-left of the rows as they lie in memory.
bool Decode(const std::vector<u8>& file, Atlas& a) {
    if (file.size() < 0x80 + 0x40 || std::memcmp(file.data(), "MTXT", 4) != 0 || std::memcmp(&file[0x80], "CTPK", 4) != 0)
        return false;
    const u8* ctpk = &file[0x80];
    const std::size_t ctpk_size = file.size() - 0x80;
    const u32 textures = Le16(ctpk + 6), section = Le32(ctpk + 8);
    const u8* entry = ctpk + 0x20;
    const u32 bytes = Le32(entry + 4), offset = Le32(entry + 8), fmt = Le32(entry + 12);
    const u32 w = Le16(entry + 16), h = Le16(entry + 18);
    u32 bpp = 0;
    u8 probe[4];
    if (textures < 1 || w < 8 || h < 8 || w % 8 != 0 || h % 8 != 0 || w > 4096 || h > 4096 ||
        !Texel(static_cast<int>(fmt), ctpk, 0, probe, &bpp) || static_cast<u64>(w) * h * bpp / 8 > bytes ||
        static_cast<u64>(section) + offset + bytes > ctpk_size)
        return false;
    const u8* data = ctpk + section + offset;
    a.decoded.assign(static_cast<std::size_t>(w) * h * 4, 0);
    std::size_t index = 0;
    for (u32 ty = 0; ty < h / 8; ++ty) {
        for (u32 tx = 0; tx < w / 8; ++tx) {
            for (u32 i = 0; i < 64; ++i, ++index) {
                // Morton order: x from bits 0, 2, 4 and y from bits 1, 3, 5
                const u32 x = (i & 1) | (i >> 1 & 2) | (i >> 2 & 4), y = (i >> 1 & 1) | (i >> 2 & 2) | (i >> 3 & 4);
                u8* out = &a.decoded[((static_cast<std::size_t>(ty) * 8 + y) * w + tx * 8 + x) * 4];
                Texel(static_cast<int>(fmt), data, index, out, &bpp);
            }
        }
    }
    a.level0.assign(data, data + static_cast<std::size_t>(w) * h * bpp / 8);
    a.level0_fmt = static_cast<int>(fmt);
    a.decoded_w = w;
    a.decoded_h = h;
    return true;
}

bool ReadFont(const std::vector<u8>& file, Font& f) {
    if (file.size() < 0x28 || std::memcmp(file.data(), "MFNT", 4) != 0) return false;
    const u32 count = Le32(&file[0x1C]), offset = Le32(&file[0x20]);
    if (count == 0 || count > 4096 || offset > file.size() || (file.size() - offset) / 14 < count) return false;
    f.size = static_cast<float>(Le32(&file[0x18]));
    f.glyphs.resize(count);
    for (u32 i = 0; i < count; ++i) {
        const u8* g = &file[offset + 14 * i];
        f.glyphs[i] = {static_cast<u16>(Le16(g)), static_cast<u16>(Le16(g + 2)), static_cast<u16>(Le16(g + 4)),
                       static_cast<u16>(Le16(g + 6)), static_cast<std::int16_t>(Le16(g + 8)),
                       static_cast<std::int16_t>(Le16(g + 10)), static_cast<u16>(Le16(g + 12))};
    }
    return true;
}

void Load() {
    Files files;
    for (int i = 0; i < MSR_ATLAS_COUNT; ++i) {
        if (i == MSR_ATLAS_WHITE) {   // WhiteSquare (ETC1 in the RomFS, which is not decoded here): white, made here
            atlases[i].decoded.assign(8 * 8 * 4, 255);
            atlases[i].decoded_w = atlases[i].decoded_h = 8;
            continue;
        }
        const bool ok = Decode(files.Get(Atlases[i].path, Atlases[i].package), atlases[i]);
        if (ok) std::fprintf(stderr, "msr: GUI: %s, %ux%u format %d\n", Atlases[i].path, atlases[i].decoded_w, atlases[i].decoded_h, atlases[i].level0_fmt);
        else std::fprintf(stderr, "msr: GUI: %s could not be read: what it draws comes from the touch screen\n", Atlases[i].path);
    }
    const std::vector<u8> table = files.Get(GlyphTablePath, FontPackage);
    if (table.size() >= 0x10 && std::memcmp(table.data(), "MUCT", 4) == 0) {
        const u32 count = Le32(&table[8]), offset = Le32(&table[0xC]);
        if (offset <= table.size() && (table.size() - offset) / 8 >= count) {
            for (u32 i = 0; i < count; ++i) glyph_table.emplace_back(Le32(&table[offset + 8 * i]), Le32(&table[offset + 8 * i + 4]));
            std::sort(glyph_table.begin(), glyph_table.end());
        }
    }
    for (int i = 0; i < MSR_FONT_COUNT; ++i) {
        if (!ReadFont(files.Get(Fonts[i].path, FontPackage), fonts[i]) || glyph_table.empty()) {
            fonts[i] = Font{};
            std::fprintf(stderr, "msr: GUI: %s could not be read\n", Fonts[i].path);
        }
    }
    load_state.store(2);
}

// Main thread: the decoded atlases taken over once, then a texture pack's replacement of each when one loads.
bool Ready() {
    int state = load_state.load();
    if (state == 0 && load_state.compare_exchange_strong(state, 1)) {
        std::thread([] {
            k_background_thread();
            Load();
        }).detach();
        return false;
    }
    if (state != 2) return false;
    if (!taken) {
        taken = true;
        for (Atlas& a : atlases) {
            if (a.decoded.empty()) continue;
            a.rgba.swap(a.decoded);
            a.w = a.decoded_w;
            a.h = a.decoded_h;
            a.version = 1;
            a.game_alpha.resize(static_cast<std::size_t>(a.w) * a.h);
            for (std::size_t p = 0; p < a.game_alpha.size(); ++p) a.game_alpha[p] = a.rgba[p * 4 + 3];
            a.game_w = a.w;
            a.game_h = a.h;
        }
    }
    for (Atlas& a : atlases) {   // the device's button icons, as the game's own uploads of these sheets show them
        if (a.rgba.empty()) continue;
        if (a.glyph_sheet < 0)
            a.glyph_sheet = a.level0.empty() ? 0 : msr_glyph_sheet(a.level0.data(), a.decoded_w, a.decoded_h, a.level0_fmt);
        if (!a.glyph_sheet || a.glyph_version == msr_glyph_version()) continue;
        if (a.pristine.size() != a.rgba.size()) a.pristine = a.rgba;
        a.rgba = a.pristine;
        const std::size_t row = std::size_t{a.w} * 4;
        std::vector<u8> line(row);
        const auto flip = [&] {   // (the sheets' cells count from the bottom row, as the renderer decodes them)
            for (u32 y = 0; y < a.h / 2; ++y) {
                std::memcpy(line.data(), &a.rgba[y * row], row);
                std::memcpy(&a.rgba[y * row], &a.rgba[(a.h - 1 - y) * row], row);
                std::memcpy(&a.rgba[(a.h - 1 - y) * row], line.data(), row);
            }
        };
        flip();
        msr_glyph_patch(a.glyph_sheet, a.rgba.data(), a.w, a.h);
        flip();
        a.glyph_version = msr_glyph_version();
        ++a.version;
    }
    for (int i = 0; i < MSR_ATLAS_COUNT; ++i) {
        Atlas& a = atlases[i];
        if (a.rgba.empty() || a.pack_state == 3 || a.level0.empty()) continue;   // (none for one made here)
        if (a.pack_state < 2) {
            if (!msr_textures_active()) {
                a.pack_state = 3;
                continue;
            }
            if (!a.content) {   // FNV-1a of the bytes: the lookup's memo key
                a.content = 1469598103934665603ull;
                for (const u8 b : a.level0) a.content = (a.content ^ b) * 1099511628211ull;
            }
            int pending = 0;
            a.pack_key = msr_texture_find(a.level0.data(), a.w, a.h, a.level0_fmt, a.content, &pending);
            a.pack_state = pending ? 1 : a.pack_key ? 2 : 3;
        }
        if (a.pack_state == 2) {
            u8* rgba = nullptr;
            u32 w = 0, h = 0;
            const int r = msr_texture_copy(a.pack_key, &rgba, &w, &h);
            if (r < 0) a.pack_state = 3;
            if (r > 0) {
                // the renderer's order is bottom-up: turned back
                a.rgba.resize(static_cast<std::size_t>(w) * h * 4);
                for (u32 y = 0; y < h; ++y)
                    std::memcpy(&a.rgba[static_cast<std::size_t>(y) * w * 4], rgba + static_cast<std::size_t>(h - 1 - y) * w * 4, static_cast<std::size_t>(w) * 4);
                std::free(rgba);
                a.w = w;
                a.h = h;
                a.pristine.clear();     // the pack's: its own pixels before the icons
                a.glyph_version = 0;
                ++a.version;
                a.pack_state = 3;
                std::fprintf(stderr, "msr: GUI: the texture pack's %s, %ux%u\n", Atlases[i].path, w, h);
            }
        }
    }
    return true;
}

// A rectangle of an atlas drawn larger than the game draws it: pulled in by half a texel of the atlas as loaded (a pack's
// may be larger), so the filtering at its edges does not take in the next sprite (the flare's top took a line of the
// sprite above it)
void Inset(int atlas, float u0, float v0, float u1, float v1, float uv[4]) {
    const float du = .5f / static_cast<float>(std::max<u32>(atlases[atlas].w, 1)), dv = .5f / static_cast<float>(std::max<u32>(atlases[atlas].h, 1));
    uv[0] = u0 + du;
    uv[1] = v0 + dv;
    uv[2] = u1 - du;
    uv[3] = v1 - dv;
}

} // namespace

extern "C" unsigned msr_gui_atlas(int atlas, const uint8_t** rgba, uint32_t* w, uint32_t* h) {
    if (atlas < 0 || atlas >= MSR_ATLAS_COUNT || !Ready() || atlases[atlas].rgba.empty()) return 0;
    *rgba = atlases[atlas].rgba.data();
    *w = atlases[atlas].w;
    *h = atlases[atlas].h;
    return atlases[atlas].version;
}

extern "C" int msr_gui_atlas_game_alpha(int atlas, const uint8_t** alpha, uint32_t* w, uint32_t* h) {
    if (atlas < 0 || atlas >= MSR_ATLAS_COUNT || !Ready() || atlases[atlas].game_alpha.empty()) return 0;
    *alpha = atlases[atlas].game_alpha.data();
    *w = atlases[atlas].game_w;
    *h = atlases[atlas].game_h;
    return 1;
}

extern "C" int msr_gui_sprite(int sprite, int* atlas, float uv[4]) {
    if (sprite < 0 || sprite >= MSR_SPRITE_COUNT || !Ready()) return 0;
    const SpriteDef& s = Sprites[sprite];
    const AtlasDef& a = Atlases[s.atlas];
    if (atlases[s.atlas].rgba.empty()) return 0;
    *atlas = s.atlas;
    Inset(s.atlas, s.u / a.coord_w, s.v / a.coord_h, (s.u + s.w) / a.coord_w, (s.v + s.h) / a.coord_h, uv);
    return 1;
}

extern "C" int msr_gui_font(int font, float* size, float* ascent) {
    if (font < 0 || font >= MSR_FONT_COUNT || !Ready() || fonts[font].glyphs.empty() || atlases[MSR_ATLAS_FONT].rgba.empty()) return 0;
    *size = fonts[font].size;
    *ascent = std::round(fonts[font].size * .8f);
    return 1;
}

extern "C" int msr_gui_font_by_file(uint32_t hash) {
    for (int i = 0; i < MSR_FONT_COUNT; ++i)
        if (NameHash(Fonts[i].path) == hash) return i;
    return -1;
}

extern "C" int msr_gui_atlas_by_path(uint32_t hash) {
    for (int i = 0; i < MSR_ATLAS_COUNT; ++i)
        if (!Atlases[i].package && NameHash(Atlases[i].path) == hash) return i;
    return -1;
}

extern "C" int msr_gui_embedded(const char* name, size_t length, int* atlas, float uv[4], float* w, float* h) {
    if (!Ready() || atlases[MSR_ATLAS_HUD_OPAQUE].rgba.empty()) return 0;
    for (const Embedded& e : EmbeddedSprites) {
        if (std::strlen(e.name) != length || std::strncmp(e.name, name, length) != 0) continue;
        const AtlasDef& a = Atlases[MSR_ATLAS_HUD_OPAQUE];
        *atlas = MSR_ATLAS_HUD_OPAQUE;
        Inset(MSR_ATLAS_HUD_OPAQUE, e.u / a.coord_w, e.v / a.coord_h, (e.u + e.w) / a.coord_w, (e.v + e.h) / a.coord_h, uv);
        *w = e.w;
        *h = e.h;
        return 1;
    }
    return 0;
}

extern "C" int msr_gui_glyph(int font, unsigned code, MsrGlyph* out) {
    if (font < 0 || font >= MSR_FONT_COUNT || !Ready() || fonts[font].glyphs.empty()) return 0;
    const auto it = std::lower_bound(glyph_table.begin(), glyph_table.end(), std::make_pair(static_cast<u32>(code), 0u));
    u32 index = it != glyph_table.end() && it->first == code ? it->second : 0;   // glyph 0: the font's "unknown"
    if (index >= fonts[font].glyphs.size()) index = 0;
    const Glyph& g = fonts[font].glyphs[index];
    const AtlasDef& a = Atlases[MSR_ATLAS_FONT];
    Inset(MSR_ATLAS_FONT, g.x / a.coord_w, g.y / a.coord_h, (g.x + g.w) / a.coord_w, (g.y + g.h) / a.coord_h, out->uv);
    out->w = g.w;
    out->h = g.h;
    out->left = g.left;
    out->top = g.top;
    out->advance = g.advance;
    return 1;
}
