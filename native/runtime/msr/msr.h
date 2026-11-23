/* Samus Returns' own features on msr-native, as the emulator-based build has them (src/msr): the player's settings,
 * the amiibo extras and cheats. C interface for the runtime; the parts live in runtime/msr/ (C++, sharing src/msr's
 * headers). */
#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Reads the launcher's settings (user/msr/settings.ini, [General]) into the settings overlay (overlay_host.cpp), which
   publishes them: the single-screen presentation's options, the extras, the frame rate. Missing keys keep the main
   build's defaults. Returns 0 when the file was read. */
int msr_settings_load(const char *path);
/* Game frame rate the settings ask for (30, 60, 90, 120 or 144). */
int msr_settings_frame_rate(void);
/* The game's language (a 3DS system language: 1 English, 2 French, 5 Spanish, 3 German, 4 Italian, 8 Dutch) and the
   console region it needs (1 USA, 2 Europe: the game shows German, Italian and Dutch only there) */
int msr_settings_language(void);
int msr_settings_region(void);
int msr_settings_wide_culling(void);          /* the game draws all of a wider view (the menu's "Draw only the 3DS's view" off) */
void msr_state_quick_save(void);             /* state.c: the settings' Quick save and Quick load (main thread) */
void msr_state_quick_load(void);
const char *msr_state_message(void);          /* state.c: "Saved", "Loaded"... once, for the screen */
int msr_fxaa(void);                           /* FXAA on the game's picture (#51) */
int msr_smooth_packs(void);                   /* texture packs' textures filtered smoothly (#51) */
int msr_grade(float *v);                      /* brightness, contrast, saturation into v[0..2] (#25); 0 when all are 1 */
float msr_bloom(void);                        /* the player's Bloom setting, 1 = the game's glow as drawn */
int msr_hdr(float *peak, float *paper);       /* HDR on; its brightness in nits (0 = the display's) */
/* stereoscopic 3D (#17): 0 off, 1 side by side at half width (each eye squeezed into half the window: 3D TVs, Acer
   SpatialLabs), 2 side by side at full width (each eye half the window at its own shape: VR viewers, a double-wide
   window); *depth the 3DS's 3D slider (0..1), *swap the eyes swapped (the right eye on the left) */
int msr_stereo(float *depth, int *swap);
int  msr_settings_save(void);           /* the overlay's player settings back into the settings file; 0 when written */
const char *msr_settings_file(void);    /* settings.ini */

/* msr-native's exit code asking the launcher to start it again (the overlay's Restart) */
#define MSR_EXIT_RESTART 75

/* ---- game mods (mods.cpp, layered_fs.cpp; the launcher's Mods and Textures tabs) ---- */
/* The user folder (ending in '/'), settings.ini and msr-native's folder; then, before the dump is read, the enabled
   mods and texture packs (the packs only when none were given with --texture-pack) */
void msr_mods_init(const char *user, const char *settings, const char *exe);
void msr_mods_boot(int packs_given);
int  msr_mods_exheader(uint8_t *ex, size_t size);             /* 1: the first mod's exheader.bin is in ex */
int  msr_mods_code(uint8_t **code, uint32_t *size, uint32_t room);   /* 1: changed (code.bin, IPS, BPS), -1: does not fit */
/* The RomFS the game reads: the dump's level-3 image, or one rebuilt with the mods' files (msr_romfs_layered) */
int      msr_romfs_open(const char *dump, uint64_t offset, uint64_t size);
int      msr_romfs_layered(void);
uint64_t msr_romfs_size(void);
size_t   msr_romfs_read(uint64_t offset, size_t length, uint8_t *out);
/* A read of the game file for the game failed (fs.c, msr_romfs_read): the log names the file and why (dump_error) */
void     msr_romfs_read_failed(uint64_t offset, uint64_t length);

/* Rewrites the game's instructions for the extras that are on (src/msr/cheats.h Patches: galleries, Fusion Mode, the
   Fusion suit, Hard Mode), after .code is in memory. The recompiled code hands each rewritten word to interp.c. */
void msr_apply_patches(void);
/* Once the native code is loaded: the functions holding the words it does not check (patches added since players
   built theirs) run on the interpreter. */
void msr_patches_module_loaded(void);
void msr_draw_lists_install(void);   /* the level's pieces past the 3DS's budget of 300, for the wider view (draw_lists.cpp) */

/* ---- the single 16:9 screen (compositor.cpp; the emulator-based build's DrawMsrScreens) ---- */
/* One quad over the view, in window (drawable) pixels: a region of the bottom screen (style[3] = mode 0 plain,
   1 luma-keyed, 3 drawn over black), a solid fill (mode 2), or a sprite of one of the game's atlases (source 3; mode 4
   an additive one tinted, 5 tinted by color, 6 additive tinted by color). msr.frag reads these as they are. */
typedef struct {
    float dst[4];     /* x, y, w, h */
    float src[4];     /* bottom screen u0, v0, u1, v1 (0..1, screen-oriented) */
    float style[4];   /* opacity, key low (mode 3: gain), key high, mode */
    int feather;      /* soft edge in percent of the quad; bits 8-11 keep the left, top, right, bottom edges hard */
    int circle;       /* circular mask radius in percent (0: none) */
    int source;       /* 1: the beam slots as held (video.c's copy of the bottom screen before R, msr_beams_held);
                         2: the menu as last seen (video.c's copy of the bottom screen while it read as one, msr_menu_live);
                         3: one of the game's atlases, atlas (msr_gui_atlas; src in it, 0..1) */
    int atlas;        /* source 3: MSR_ATLAS_* */
    float color[4];   /* fill colour */
    int turn;         /* src turned in dst by quarter turns (MsrGuiItem.turn; msr.vert) */
} MsrQuad;

typedef struct {
    float top[4];     /* where the top screen goes (x, y, w, h): the view, moved aside in the gallery */
    float panel[4];   /* the bottom screen's panel (menus; touches map into it while msr_panel_visible()) */
    const MsrQuad *quads;   /* to draw after the top screen, in order; valid until the next call */
    int count;
} MsrFrame;

void msr_set_single(int on);       /* the single-screen layout is on: widescreen squeeze, backdrop skip, compositor */
int  msr_single(void);
/* Each presented frame: classifies the bottom screen (320x240 RGBA rows, or NULL), advances the fades from the
   host time (s) and the 3DS buttons (Start opens the pause menu), lays out a ww x wh window. */
void msr_compose(const uint8_t *bottom, double now, unsigned buttons, float ww, float wh, MsrFrame *out);
void msr_single_layout(float ww, float wh, float top[4], float panel[4]);
/* R held (missiles): the HUD's beam slots come from video.c's copy of the bottom screen made before (quads with
   source 1); video.c copies the slots' column each frame this is 0 */
int  msr_beams_held(void);
/* The bottom screen reads as a menu this frame: video.c copies it, and the panel's quads with source 2 sample that copy
   while it does not (the panel fading out as the game draws something else there) */
int  msr_menu_live(void);
int  msr_panel_visible(void);
/* pica_vk: the clip-space scale of the top screen's world draws (x down the screen, y across: widescreen and zoom) */
void msr_world_scale(float *sx, float *sy);
/* pica_sw.c: the same for the game's interface on the top screen, without the zoom (its own shape in a wider view) */
void msr_ui_scale(float *sx, float *sy);
int msr_world_in_play(void);   /* the compositor's "in play" (the zoom applies) */
void msr_camera_frame(double now);   /* each VBlank: the game's camera gets the field of view the zoomed view needs (camera_fov.cpp) */
float msr_fov_gain(float px);   /* the renderer: how much wider than the game's that field of view is, for a draw with the camera's projection (x scale px) */
int msr_samus_view(float *h, float *v);   /* Samus on the top screen in the game's own view (5:3, zoom 1): h right, v up, -1..1; 0 if none */
int msr_map_marker(float *x, float *y);   /* minimap.cpp: her marker on the HUD's map as the game draws it (bottom screen pixels); 0 if none */
int msr_map_focus(void);   /* minimap.cpp: the game's map shows a place of its own (1: the DNA scanner's way, 2: another); 0 if not */
int msr_input_pad_active(void);   /* the last input came from a gamepad (glyphs.cpp) */
void msr_input_device(int pad, int sdl_type);   /* video.c: the input just used (0 = keyboard/mouse), for the button names and icons (glyphs.cpp) */
void msr_input_bindings_changed(void);
/* Unless the HUD shows, an Xbox or PlayStation pad chooses with its bottom face button and goes back with its right one
   (glyphs.cpp; input.c input_pad_bound): msr_buttons_state (compositor.cpp, each frame) says whether its buttons and its
   icons follow that layout, msr_menu_buttons whether a pad's buttons do now, msr_pad_choose the SDL button that
   chooses (touch mode, the settings). */
void msr_buttons_state(int input, int icons);
int msr_menu_buttons(void);
int msr_pad_choose(void);
int msr_pad_back(void);   /* the SDL button that goes back there */
/* The Controls tab's Nintendo menu layout (off by default): no menus' layout for any pad; menus choose with the button
   bound to the game's A and go back with the one bound to its B */
void msr_set_menu_confirm_right(int on);
int msr_menu_confirm_right(void);
int msr_glyph_sheet(const uint8_t *level0, uint32_t w, uint32_t h, int fmt);   /* pica_vk.c: one of the face-button icon sheets (1, 2) or 0 */
void msr_glyph_patch(int sheet, uint8_t *rgba, uint32_t w, uint32_t h);
unsigned msr_glyph_version(void);
/* The pause's Controls page names play's buttons in a menu (compositor.cpp ControlsIcons): the game's face icon (0-3:
   A, B, X, Y) that shows now the pad button play binds control (C_*) to, -1 when none does (pad_button: that button,
   -1 unbound), -2 when the icons need no change (the keyboard's and an unknown device's follow the bindings) */
int msr_glyph_play_face(int control, int *pad_button);
/* a pad button's (or trigger's: PAD_TRIGGER_*) short name on the device in use: "LT", "RS", "View" ... */
const char *msr_pad_label(int button);
/* pica_vk: each draw into the bottom screen's target (1 if it is a full-screen backdrop quad of the title menus);
   returns 1 to leave it out. msr_bottom_frame at each display transfer. */
int  msr_skip_bottom_draw(int quad_backdrop);
void msr_bottom_frame(void);
/* The pad's triggers (input_bridge.cpp): 0 = LT, Morph Ball (taps the minimap); 1 = RT, the next beam (taps its slot);
   2 = the beam before (the mouse wheel up); with R held, 1 and 2 switch between Missile and Super Missile (taps those
   slots). Only while the HUD shows. input.c then holds the tap msr_injected_touch gives (1 while it lasts; a missile
   tap ends when R, in the buttons the game gets, comes up). */
void msr_trigger(int which);
int  msr_injected_touch(unsigned buttons, int *x, int *y);
/* The menus' touch buttons on the D-pad (compositor.cpp: the file select's Copy and Delete, the pause menu's toolbar
   and map pins): input.c's buttons and circle pad, which it may keep from the game, the D-pad as the menus mean it
   (its bits or the circle pad's digital directions), *take |= 1 to keep that D-pad from the game (|= 2: the analog
   stick too), and its tap at x, y (1 while it holds) */
int  msr_footer_input(uint32_t *buttons, uint32_t dpad, int *take, int *cx, int *cy, int *x, int *y);
void msr_trigger_hold(int which, int down);   /* video.c: ZL (0) or ZR (1) pressed or let go (the map pins' drag) */
/* Touch navigation (src/msr/touch_nav.cpp): a highlight over the touch screen's buttons, moved with the pad or keys */
int  msr_touch_active(void);
void msr_touch_toggle(void);             /* T (keyboard) */
void msr_touch_step(int dx, int dy);
void msr_touch_finger(int down);         /* press (hold to drag) and lift */
void msr_touch_close(void);
void msr_touch_frame(float stick_x, float stick_y, int any_held);   /* each frame: stick (-1..1, y up), anything held */
int  msr_input_blocked(void);            /* the game gets no pad input (touch navigation, until its closing button is up) */
int  msr_station_map(void);              /* a Teleport Station's area map shows, where the D-pad chooses the station */

/* ---- cheats (cheats.cpp; the emulator-based build's ApplyMsrRefills) ---- */
void msr_cheats_frame(double now);   /* each VBlank: the extras' patches follow the settings, refills, setter calls queued */
float msr_item(uint32_t key, const char *name);   /* cheats.cpp: Samus's item by key global and name; -1: cannot tell */
int msr_metroid_radar(float rgba[4][4]);   /* cheats.cpp: the Metroid radar sprites' colors (base, color, highlight, flare); 0: not found yet */
int msr_samus_beam(int *morph);   /* cheats.cpp: her beam (0 usual, 1 Ice, 2 Grapple; -2 another gun in use, -1 cannot tell) and Morph Ball (1/0/-1) */
struct CPU;
int msr_game_hook(struct CPU *c);

/* ---- texture packs (textures.cpp; Azahar's custom textures, as the emulator-based build loads them) ---- */
int  msr_textures_add_pack(const char *dir);   /* the enabled packs, highest priority first, before the game starts */
void msr_textures_start(void);
int  msr_textures_active(void);
/* pica_vk: new bytes of a texture (level 0, w x h, PICA format; content = its hash of all levels): the pack's hash when
   a pack replaces it (its PNG then loads on a worker), else 0. *pending: not known yet (a worker works out the legacy
   hash); ask again later with the same content. */
uint64_t msr_texture_find(const uint8_t *level0, uint32_t w, uint32_t h, int fmt, uint64_t content, int *pending);
/* "Dump game textures" (#64, settings.ini [native] dump_textures; the game's start reads it): the renderers offer a
   texture to msr_texture_dump (as msr_texture_find) when its bytes are new or its dump_gen is not g_msr_dump_gen, which
   is 0 while not dumping and changes each time dumping starts. Each is written once, on a worker. */
extern volatile unsigned g_msr_dump_gen;
void msr_textures_set_dump(int on);
void msr_texture_dump(const uint8_t *level0, uint32_t w, uint32_t h, int fmt, uint64_t content);
unsigned msr_textures_dumped(void);   /* this session's, including those already in the folder */
/* pica_vk: what prepares a replacement's upload on the workers (make: pixels -> its own handle, any thread; drop: a
   handle never taken). Not with PWR_TEXPACK_SYNC=1. */
void msr_textures_set_stager(void *(*make)(const uint8_t *rgba, uint32_t w, uint32_t h), void (*drop)(void *staged));
/* The replacement's RGBA rows (in pica_vk's order): 2 with its prepared upload in *staged (the caller's now), 1 with
   the pixels only (the caller uploads them; always when !want_staged), 0 while loading or being prepared, -1 when it
   cannot be read */
int  msr_texture_take(uint64_t key, int want_staged, void **staged, const uint8_t **rgba, uint32_t *w, uint32_t *h);
void msr_texture_done(uint64_t key);          /* uploaded: the pixels may go */
/* A copy of the replacement's RGBA rows (pica_vk's order: bottom-up), malloc'd for the caller: 1, 0 while loading, -1
   when it cannot be read (gui_assets.cpp: the game's atlases) */
int  msr_texture_copy(uint64_t key, uint8_t **rgba, uint32_t *w, uint32_t *h);

/* ---- the game's interface pictures (gui_assets.cpp): its sprite sheets and fonts, drawn as the compositor's quads
   with source 3. All main thread; they start loading on the first call. ---- */
enum { MSR_ATLAS_HUD_OPAQUE, MSR_ATLAS_HUD, MSR_ATLAS_FONT, MSR_ATLAS_WHITE, MSR_ATLAS_MAINMENU, MSR_ATLAS_COUNT };
/* An atlas (RGBA8, rows top-down) and its version: 0 until it is read, a new one when a pack's replacement arrives.
   The pixels stay valid until the version changes. */
unsigned msr_gui_atlas(int atlas, const uint8_t **rgba, uint32_t *w, uint32_t *h);
/* The game's own alpha of an atlas (w x h of the game's, rows top-down), a texture pack's or not; 0 until it is read */
int msr_gui_atlas_game_alpha(int atlas, const uint8_t **alpha, uint32_t *w, uint32_t *h);
enum {
    MSR_SPRITE_POWER, MSR_SPRITE_ICE, MSR_SPRITE_GRAPPLE, MSR_SPRITE_WAVE, MSR_SPRITE_SPAZER, MSR_SPRITE_PLASMA,   /* beams */
    MSR_SPRITE_DNA_COUNT, MSR_SPRITE_METROID_BASE, MSR_SPRITE_METROID_COLOR, MSR_SPRITE_METROID_HIGHLIGHT,
    MSR_SPRITE_FLARE, MSR_SPRITE_ERASE, MSR_SPRITE_UP_WINDOW, MSR_SPRITE_COUNT
};
/* A sprite's atlas and rectangle in it (u0, v0, u1, v1): 1, or 0 while that atlas is not there */
int msr_gui_sprite(int sprite, int *atlas, float uv[4]);
enum { MSR_FONT_HEFTY, MSR_FONT_BIG, MSR_FONT_MEDIUM, MSR_FONT_SMALL, MSR_FONT_COUNT };   /* digital_hefty, _big, ... */
/* A glyph of a font, in MSR_ATLAS_FONT: its rectangle (uv), size, left bearing, height of its top above the baseline
   and advance, in the font's pixels */
typedef struct { float uv[4]; float w, h, left, top, advance; } MsrGlyph;
int msr_gui_font(int font, float *size, float *ascent);   /* 1 when the font can be drawn; ascent: baseline below the top */
int msr_gui_glyph(int font, unsigned code, MsrGlyph *glyph);
/* By the game's own names' hashes (the packages' CRC-32 of a path): a font file's (MSR_FONT_*), a texture's
   (MSR_ATLAS_*); -1 for one not loaded here */
int msr_gui_font_by_file(uint32_t hash);
int msr_gui_atlas_by_path(uint32_t hash);
/* A sprite a label's text embeds, @[NAME]: its atlas, rectangle and size in its atlas' pixels; 0 if unknown */
int msr_gui_embedded(const char *name, size_t length, int *atlas, float uv[4], float *w, float *h);

/* ---- the title menus' touch screen as the game holds it (gui_tree.cpp) ---- */
/* One sprite or glyph of it, in touch-screen pixels, in the order the game draws them */
typedef struct {
    float x, y, w, h;
    float uv[4];        /* in atlas (flips applied) */
    float color[4];     /* the game's (may pass 1) */
    float clip[4];      /* drawn only inside x0, y0, x1, y1 (the pause map's view) */
    int atlas;          /* MSR_ATLAS_* */
    int additive;       /* blended additively (the game's Additive), else by alpha */
    int glyph;          /* part of a label's text */
    uint32_t name;      /* the object's name hash (CRC-32 of its name: ButtonSelectCopyBase ...) */
    uint32_t sprite;    /* a sprite's sheet item's name hash (Button_Selected ...) */
    int anchor;         /* where it goes on a touch screen drawn wider than 4:3 (MSR_ANCHOR_*) */
    uint32_t group;     /* the named container it lies in where one matters (ControlsComposition: the pause's Controls page) */
    int turn;           /* a sprite the game turns: quarter turns (1 a quarter clockwise on the screen, 3 counterclockwise);
                           x, y, w, h are then the turned picture's rectangle (MsrQuad.turn) */
} MsrGuiItem;
/* MsrGuiItem.anchor: kept to the middle (as the 3DS draws it), to the left or right edge (a piece, or its group, against
   that edge of the screen, or lying in one that is), stretched between its ends (a piece wider than 60% of the screen
   in no narrower group, with an end against an edge: backgrounds, bars, a bar's halves; each end keeps its shape where it goes,
   msr_anchor_end), or the map's (the middle, its view opened to the width) */
enum { MSR_ANCHOR_MIDDLE, MSR_ANCHOR_LEFT, MSR_ANCHOR_RIGHT, MSR_ANCHOR_STRETCH, MSR_ANCHOR_MAP };
/* where an end of a piece at x (touch-screen pixels) goes: against the left or right edge, or with the middle */
static inline int msr_anchor_end(float x) { return x <= 2.f ? MSR_ANCHOR_LEFT : x >= 318.f ? MSR_ANCHOR_RIGHT : MSR_ANCHOR_MIDDLE; }
/* This frame's (main thread): the count, 0 when the game holds no interface tree here (the title menus' root, or in
   play the in-game root with the HUD and the pause menu). Valid until the next call. */
int msr_gui_tree(const MsrGuiItem **items);
/* The Aeion abilities' page the game shows on the touch screen when Samus gains an Aeion ability (AbilitiesCore): 1
   while it shows. Main thread. */
int msr_gui_abilities(void);
/* The top screen's message box (USMComposition: an item's name, "Use the Teleport Station?", a Recharge Station's
   message): 1 while it shows. Main thread. */
int msr_gui_dialog(void);
/* The HUD's opacity as the game holds it (0 to 1), -1 when it has none: in play's buttons from its first fade in after a
   load, before the bottom screen reads as the HUD (compositor.cpp). Main thread. */
float msr_gui_hud_alpha(void);
/* Objects of the in-game interface by their name hashes, shown or not, on either screen: each as msr_gui_tree's pieces
   (a sprite with its picture; anything else its rectangle and color, atlas -1), w 0 for one not found. The count found.
   Main thread. */
int msr_gui_named(const uint32_t *names, int count, MsrGuiItem *items);
/* A paged label of the in-game interface by its name hash: 2 when its page is typed out and another follows on A, 1
   typed out and the last, 0 while it types, -1 for none. Main thread. */
int msr_gui_label_waits(uint32_t name);
/* The pause map's world map (SR388's areas; the Teleport Station's too): its opacity in msr_gui_tree's last call, 0 when
   it is not shown */
float msr_gui_world_map(void);
/* The pause map's view (MinimapHang) in msr_gui_tree's last call: x0, y0, x1, y1 in touch-screen pixels and 1, else 0 */
int msr_gui_map_view(float view[4]);
/* The pause map's view widened by so many touch-screen pixels at each side of the screen it reaches, from the next
   msr_gui_tree call on (the pause screen fitted to a wider window: the map fills it). Main thread. */
void msr_gui_map_margin(float margin);
/* In play: the HUD's minimap (the touch screen's map view and what it holds, not clipped to it), its view's rectangle
   in view (x, y, w, h, touch-screen pixels); 0 where there is none. Its dark fill round the rooms in colors times shade
   (its background's). Main thread; valid until the next call. */
int msr_gui_minimap(const MsrGuiItem **items, float view[4], float shade);
/* The mouse at wx, wy (a window of ww x wh) on the pause screen fitted to the window: the touch-screen point under it in
   tx, ty and 1; 0 where the panel is drawn as the game draws it (mapped to its rectangle then). Main thread. */
int msr_panel_touch(float wx, float wy, float ww, float wh, int *tx, int *ty);
/* layered_fs.cpp: a RomFS file by its path (with the game mods' files), malloc'd for the caller; NULL if not there */
uint8_t *msr_romfs_file(const char *path, size_t *size);
void msr_textures_stats(unsigned *textures, unsigned *loaded, unsigned *failed);    /* dispatch.cpp, at the frame limiter's trap: runs the queued calls; 0 when it was ours */

/* kernel.c: the calling thread below the game's, for work no frame waits for (texture decodes, scans, cache saves) */
void k_background_thread(void);

#ifdef __cplusplus
}
#endif
