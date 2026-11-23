/* Presentation: SDL2 window + Vulkan swapchain.
 * Each frame the two LCD framebuffers (stored rotated, 240 pixels per line) are decoded into a 400x240 (top) and a
 * 320x240 (bottom) RGBA image. The GPU presenter uploads them as textures and draws them into an off-screen image the
 * size of the window, placed by the screen layout and filtered (nearest, linear, sharp bilinear; FSR/FXAA, see below),
 * then copies that image into the swapchain image. A copy, not a blit or a render into the swapchain: software/linear
 * swapchains (MESA_VK_WSI_DEBUG=sw on Xvfb) support transfers into them. Without Vulkan 1.3 dynamic rendering the
 * screens are scaled on the CPU (nearest) into a staging buffer instead, with the same layouts.
 *
 * Layouts (video_option "layout", F9 cycles): stacked (top above bottom), side (side by side), large (big top screen,
 * small bottom screen at its lower right), top / bottom (one screen), auto: a small state machine picks "stacked"
 * in menus and the auto_gameplay layout (large, or top = bottom screen hidden) while flying:
 *     MENU -> GAMEPLAY   the circle pad has been held for 1.2 s (steering)
 *     GAMEPLAY -> MENU   Start or Select (pause menu), a click/touch on the bottom screen, or 8 s without steering
 * Aspect: keep (letterbox), integer (largest whole-number scale), stretch. The single layout's window opens 16:9
 * (window_aspect: another, e.g. 21:9 or 32:9; the view fills a wider window up to 32:9, compositor.cpp). Fullscreen: borderless (desktop) or
 * exclusive; F11 or Alt+Enter toggles. Mouse/touch input follows the bottom screen wherever the layout puts it.
 * Filters: nearest, linear, sharp bilinear, FXAA (present.frag), FSR 1 = AMD FidelityFX EASU upscale into a per-screen image
 * of the output size, then RCAS sharpening into the window (fsr_easu/fsr_rcas.frag, shaders/fsr/, MIT). F1 or Select opens
 * the settings overlay: the emulator-based build's menu (src/msr/overlay.cpp through msr/overlay_host.cpp), drawn last in
 * the presentation pass; 3D resolution and anisotropic filtering are passed to pica_vk.c.
 *
 * OpenGL (MSR_OPENGL, video_gl.c) presents instead with --renderer=opengl, and with --renderer=auto (the default) where
 * Vulkan has no 1.3 device for the window: everything above but the drawing, which video_gl.c does with the same
 * shaders. The 3D then runs on the CPU (pica_sw.c). */
#include "gpu.h"
#include "input.h"
#include "msr/msr.h"
#include <SDL.h>
#include <SDL_vulkan.h>
#include <SDL_syswm.h>
#include <vulkan/vulkan.h>
#ifdef __APPLE__
#include <vulkan/vulkan_metal.h>   /* VK_EXT_metal_objects: the window's drawable taken outside the queue (video_frame) */
#endif
#include "msr/overlay_host.h"
#include "vk_push.h"
#include "vk_rp.h"
#include "vk_wait.h"
#include "vk_diag.h"
#ifdef MSR_OPENGL
#include "video_gl.h"
#endif
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
extern int g_input_none;

#define TOP_W 400
#define BOT_W 320
#define SCR_H 240
#define MAX_IMAGES 8

static SDL_Window *win;
static VkInstance inst;
static VkSurfaceKHR surf;
static VkPhysicalDevice phys;
static VkDevice dev;
static VkQueue queue;
static uint32_t qfam;
static VkSwapchainKHR swap;
static VkImage sc_images[MAX_IMAGES];
static uint32_t sc_count;
static VkExtent2D sc_extent;
static VkFormat sc_format;
/* HDR output (hdr.frag): the swapchain is HDR10 or scRGB while the player has HDR on and the display is in HDR mode; the
   game is drawn into target as in SDR, the interface (HUD pieces, menus, the settings) into ui_layer, and hdr.frag
   combines them into the swapchain image */
static VkFormat sdr_format;          /* the SDR swapchain format: target and ui_layer have it */
static int hdr_mode;                 /* the swapchain now: 0 SDR, 1 HDR10 (PQ, BT.2020), 2 scRGB (linear BT.709) */
static int hdr_option;               /* 0 no HDR here, 1 the display is not in HDR mode, 2 HDR possible (msr_overlay_hdr) */
static float disp_peak = 1000.f, disp_paper = 200.f;   /* nits: the display's peak, Windows' SDR content white */
static int has_colorspace_ext, has_hdr_metadata;
#ifdef __APPLE__
/* MoltenVK takes the window's next CAMetalDrawable only when a submission first uses the swapchain image, inside
   vkQueueSubmit (vkAcquireNextImageKHR returns at once): there it sleeps until the compositor gives one back, and it did
   so holding queue_lock and the scan-out lock, so the GX thread waited behind the display. On an M3 Max at 6x/120 FPS
   (2026-11-18) the main thread spent 42% of its time in [CAMetalLayer nextDrawable] inside that submission and the GX
   thread a quarter of its own waiting for the two locks: 30-60 FPS in play, at times 120 after half a minute or more
   (what had looked like a cold start). VK_EXT_metal_objects' export of the image's texture takes the drawable at once,
   so video_frame takes it right after the acquire, outside both locks (its wait is the log's "next image"). */
static PFN_vkExportMetalObjectsEXT export_metal;
#endif
int g_vk_attachment_none;   /* pica_vk.c: VK_ATTACHMENT_LOAD_OP_NONE and STORE_OP_NONE can be used (VK_KHR/EXT_load_store_op_none) */
int g_vk_driver_id;         /* pica_vk.c: the device's VkDriverId (0: not known) */
static VkImageView sc_views[MAX_IMAGES];
static VkDeviceMemory stage_mem;
static VkBuffer stage;
static uint8_t *stage_ptr;
static VkDeviceSize stage_size;
static int sc_bgra;
static VkCommandPool pool;
static VkCommandBuffer cmdbuf;
static VkSemaphore sem_acquire, sem_done[MAX_IMAGES];   /* one per swapchain image: reused only after that image is re-acquired */
static VkFence fence;
static uint8_t top_px[TOP_W * SCR_H * 4], bot_px[BOT_W * SCR_H * 4];
/* The queue is shared with the PICA renderer (pica_vk.c), one thread at a time (queue_lock). Some drivers block in
   vkQueuePresentKHR for most of a frame: AMD's on Windows in a window, 14.8 ms of every 16.7 in immediate mode too
   (#49). The GX thread waited for the queue meanwhile, 6-9 ms of each frame, and the game's P3D interrupt (gsp.c) with
   it: on a Ryzen 7 9700X with an RX 9070 XT the game missed every other VBlank (35-40 of 60 FPS, frames of 16.7 and
   33.3 ms). Now a GX submission that finds the queue in use is kept, and whoever holds the queue submits the kept ones
   before its own (queue_acquire) and as it lets go (queue_release): the queue gets them in the order they were made,
   as before, and the GX thread goes on. A wait on a kept submission's fence lasts until its holder lets go and the GPU
   has done it. None are kept while the queue is free. PWR_GX_SUBMIT_WAIT=1: the GX thread waits for the queue (A/B). */
static pthread_mutex_t queue_lock = PTHREAD_MUTEX_INITIALIZER;
#define KEPT_MAX 64
static struct { VkCommandBuffer cbs[2]; uint32_t n; VkFence fence; } kept[KEPT_MAX];
static int nkept;
static pthread_mutex_t kept_lock = PTHREAD_MUTEX_INITIALIZER;   /* kept: taken after queue_lock (holding it, only a try for queue_lock) */
unsigned g_vid_gx_kept;   /* GX submissions kept (main.c's 2 s line) */
static volatile int present_slow;   /* presenting holds the queue long: only then are submissions kept (video_queue_submit) */
static void kept_submit(void) {   /* under queue_lock and kept_lock */
    for (int i = 0; i < nkept; i++) {
        VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = kept[i].n, .pCommandBuffers = kept[i].cbs };
        VkResult r = vkQueueSubmit(queue, 1, &si, kept[i].fence);
        if (r != VK_SUCCESS) {   /* taken as submitted: its fence still signals (after what came before), so no wait on it hangs */
            static int told; if (told++ < 5) fprintf(stderr, "pica_vk: submit failed (%d)\n", (int)r);
            if (r == VK_ERROR_DEVICE_LOST) vk_diag_lost("a held submission of the game's");
            if (kept[i].fence) vkQueueSubmit(queue, 0, NULL, kept[i].fence);
        }
    }
    nkept = 0;
}
static void queue_acquire(void) {
    pthread_mutex_lock(&queue_lock);
    pthread_mutex_lock(&kept_lock); kept_submit(); pthread_mutex_unlock(&kept_lock);
}
static void queue_release(void) {   /* free with none kept: a submission finds it in use (and is kept) or free, under kept_lock */
    pthread_mutex_lock(&kept_lock); kept_submit();
    pthread_mutex_unlock(&queue_lock); pthread_mutex_unlock(&kept_lock);
}
static int vk13;     /* the device has Vulkan 1.3's dynamic rendering */
static int vk_hw;    /* the hardware PICA renderer and the GPU presenter can run: with Vulkan 1.3, or in the 1.1 mode (vk_rp.h) */
static int use_gl;   /* presenting with OpenGL (video_gl.c), not Vulkan */
unsigned g_vid_acquire_fail, g_vid_present_fail, g_vid_presented; int g_vid_last_err;
/* the main thread's waits in presenting (main.c's 2 s line): the last frame's fence, the next image, the present */
double g_vid_wait_fence, g_vid_wait_acquire, g_vid_wait_present;
/* the shortest and longest time between two pictures since main.c's last 2 s line took them (present_wanted; 0: none) */
double g_vid_gap_min, g_vid_gap_max;
static double wait_clock(void) { return (double)SDL_GetPerformanceCounter() / (double)SDL_GetPerformanceFrequency(); }

/* why video_init failed, for the message the player sees (video_explain): Vulkan's reason, OpenGL's */
static char init_error[256], gl_error[256];
#define VK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { fprintf(stderr, "video: %s failed (%d)\n", #x, r_); \
    snprintf(init_error, sizeof init_error, "%s failed (%d)", #x, (int)r_); return -1; } } while (0)

/* ---------------- options ---------------- */
enum { L_STACKED, L_SIDE, L_LARGE, L_TOP, L_BOTTOM, L_SINGLE, L_AUTO, L_COUNT };
static const char *layout_names[L_COUNT] = { "stacked", "side", "large", "top", "bottom", "single", "auto" };
enum { F_NEAREST, F_LINEAR, F_SHARP, F_FSR, F_FXAA, F_COUNT };
static const char *filter_names[F_COUNT] = { "nearest", "linear", "sharp", "fsr", "fxaa" };
static const char *aspect_names[3] = { "keep", "integer", "stretch" };
static const char *fs_names[3] = { "off", "borderless", "exclusive" };
enum { R_AUTO, R_VULKAN, R_OPENGL };
static const char *renderer_names[3] = { "auto", "vulkan", "opengl" };
static int opt_renderer = R_AUTO;   /* the graphics API (video_init): auto = Vulkan with 1.3, else OpenGL, else Vulkan without 1.3 */
static int opt_layout = L_STACKED, opt_filter = F_NEAREST, opt_aspect = 0, opt_fs = 0, opt_auto_gameplay = L_LARGE;
static int opt_res = 1, opt_aniso = 1, opt_fps = 0, opt_vsync = 1, opt_res_match = 0; static float opt_sharp = 0.25f;
static int opt_select_layout = 2;   /* the gamepad's Back/Select button: 0 3DS Select, 1 the next screen layout, 2 these settings (F1),
                                       as the emulator-based build's Select (the game takes Select as Start) */
static int opt_free_look = 0;       /* right stick orbits the chase camera (freelook.c) */
/* The Android app's game: a GPU driver the player installed (--gpu-driver=<its folder's name>, from the launcher's GPU
   driver; vk_load.c), else the system's. gpu_driver_off: it could not open the graphics, the system's opens them. */
static char opt_gpu_driver[128];
static int gpu_driver_off;
#ifdef MSR_VOLK   /* Vulkan's functions loaded at run time (vk_load.c) */
int msr_vk_load(const char *driver, char *why, size_t why_n);
int msr_vk_custom(void);
SDL_bool msr_vk_surface(SDL_Window *win, VkInstance inst, VkSurfaceKHR *surf);
#define vk_create_surface msr_vk_surface
#else
#define vk_create_surface SDL_Vulkan_CreateSurface
#endif
static int opt_drawdist = 1;        /* draw distance multiplier (drawdist.c), read when the game starts */
static int fs_last = 1;   /* the fullscreen mode F11 toggles to */
static float opt_window_aspect = 16.f / 9.f;   /* the single layout's window when it opens ("21:9", "2.37") */

static int pick(const char *v, const char *const *names, int n) { for (int i = 0; i < n; i++) if (!strcmp(v, names[i])) return i; return -1; }
int video_option(const char *name, const char *value) {
    int i;
    if (!strcmp(name, "layout") && (i = pick(value, layout_names, L_COUNT)) >= 0) opt_layout = i;
    else if (!strcmp(name, "filter") && (i = pick(value, filter_names, F_COUNT)) >= 0) opt_filter = i;
    else if (!strcmp(name, "aspect") && (i = pick(value, aspect_names, 3)) >= 0) opt_aspect = i;
    else if (!strcmp(name, "fullscreen") && (i = pick(value, fs_names, 3)) >= 0) { opt_fs = i; if (i) fs_last = i; }
    else if (!strcmp(name, "auto_gameplay") && (i = pick(value, layout_names, L_AUTO)) >= 0) opt_auto_gameplay = i;
    else if (!strcmp(name, "renderer") && (i = pick(value, renderer_names, 3)) >= 0) opt_renderer = i;
    else if (!strcmp(name, "window_aspect")) {
        float a = 0, b = 1; if ((sscanf(value, "%f:%f", &a, &b) < 1) || b <= 0 || a / b < 1.f || a / b > 4.f) return -1;
        opt_window_aspect = a / b;
    }
    else if (!strcmp(name, "fsr_sharpness")) { char *e; double v = strtod(value, &e); if (*e || v < 0 || v > 2) return -1; opt_sharp = (float)v; }
    else if (!strcmp(name, "resolution")) {   /* 0: match the window (video_init, then each resize) */
        char *e; long v = strtol(value, &e, 10); if (e == value || *e || v < 0 || v > 10) return -1;
        opt_res_match = v == 0; if (v) { opt_res = (int)v; pica_vk_quality(opt_res, opt_aniso); }
    }
    else if (!strcmp(name, "aniso")) { i = atoi(value); if (i != 1 && i != 2 && i != 4 && i != 8 && i != 16) return -1; opt_aniso = i; pica_vk_quality(opt_res, opt_aniso); }
    else if (!strcmp(name, "show_fps")) opt_fps = !strcmp(value, "1") || !strcmp(value, "on") || !strcmp(value, "true");   /* the overlay draws it, from the settings */
    else if (!strcmp(name, "vsync")) opt_vsync = !strcmp(value, "1") || !strcmp(value, "on") || !strcmp(value, "true");
    else if (!strcmp(name, "select_layout")) opt_select_layout = !strcmp(value, "settings") || !strcmp(value, "2") ? 2 : !strcmp(value, "1") || !strcmp(value, "on") || !strcmp(value, "true");
    else if (!strcmp(name, "draw_distance")) { i = atoi(value); if (i < 1 || i > 4) return -1; opt_drawdist = i; drawdist_set(i); }
    else if (!strcmp(name, "free_look")) { opt_free_look = !strcmp(value, "1") || !strcmp(value, "on") || !strcmp(value, "true"); freelook_enable(opt_free_look); }
    else if (!strcmp(name, "gpu_driver")) { if (strlen(value) >= sizeof opt_gpu_driver || strchr(value, '/')) return -1; snprintf(opt_gpu_driver, sizeof opt_gpu_driver, "%s", value); }
    else return -1;
    return 0;
}

/* The window and its display, for the log: once the graphics are open, and at each change of the window's mode after that
   (2026-11-19) */
static int display_told;
static void display_line(void) {
    if (!win) return;
    display_told = 1;
    const int d = SDL_GetWindowDisplayIndex(win); SDL_DisplayMode dm; memset(&dm, 0, sizeof dm);
    if (d < 0 || SDL_GetDesktopDisplayMode(d, &dm)) memset(&dm, 0, sizeof dm);
    int ww = 0, wh = 0; SDL_GetWindowSize(win, &ww, &wh);
    char dpi[32] = ""; float ddpi = 0;
    if (d >= 0 && !SDL_GetDisplayDPI(d, &ddpi, NULL, NULL) && ddpi > 0) snprintf(dpi, sizeof dpi, ", %.0f%% scaling", ddpi / 96.f * 100.f);
    fprintf(stderr, "video: display: the window %s, %dx%d; the display %dx%d at %d Hz (%d of %d)%s; layout %s, window aspect %.2f\n",
#ifdef __ANDROID__
            "the whole screen",
#else
            opt_fs == 1 ? "borderless fullscreen" : opt_fs == 2 ? "exclusive fullscreen" : "windowed",
#endif
            ww, wh, dm.w, dm.h, dm.refresh_rate, d + 1, SDL_GetNumVideoDisplays(), dpi, layout_names[opt_layout], opt_window_aspect);
}
static void apply_fullscreen(void) {
#ifdef __ANDROID__
    return;   /* always the whole screen there (video_init) */
#endif
    SDL_SetWindowFullscreen(win, opt_fs == 1 ? SDL_WINDOW_FULLSCREEN_DESKTOP : opt_fs == 2 ? SDL_WINDOW_FULLSCREEN : 0);
    if (display_told) display_line();   /* (the first comes once the graphics are open: the window is still being made here) */
}
/* A window to go fullscreen is made hidden and shown once it is: made at its windowed size, it showed for two frames
   first, a small window with a blank title bar in the middle of the screen (2026-11-14, the user's video: "a flash"
   as the game started from Steam) */
static Uint32 hidden_until_fullscreen(void) {
#ifdef __ANDROID__
    return 0;
#else
    return opt_fs ? SDL_WINDOW_HIDDEN : 0;
#endif
}
static void show_fullscreen(void) {
#ifndef __ANDROID__
    SDL_ShowWindow(win);
    SDL_RaiseWindow(win);
#endif
}

/* the window's size in pixels (larger than its size in window coordinates on a Retina display) */
static void drawable_size(int *w, int *h) {
#ifdef MSR_OPENGL
    if (use_gl) { SDL_GL_GetDrawableSize(win, w, h); return; }
#endif
    SDL_Vulkan_GetDrawableSize(win, w, h);
}

/* ---------------- layouts ---------------- */
typedef struct { float x, y, w, h; int on; } Rect;   /* window pixels */
static int auto_state;   /* 0 = menu, 1 = gameplay */

static int effective_layout(void) { return opt_layout == L_AUTO ? (auto_state ? opt_auto_gameplay : L_STACKED) : opt_layout; }

/* the layout's canvas in 3DS pixels, scaled into the window by the aspect mode */
static void layout_rects(int ww, int wh, Rect *top, Rect *bot) {
    if (effective_layout() == L_SINGLE) {   /* Samus Returns' 16:9 view; the bottom screen is a panel there only in menus */
        float t[4], p[4]; msr_single_layout((float)ww, (float)wh, t, p);
        *top = (Rect){ t[0], t[1], t[2], t[3], 1 };
        *bot = (Rect){ p[0], p[1], p[2], p[3], msr_panel_visible() };
        return;
    }
    float cw, ch; Rect t = { 0, 0, TOP_W, SCR_H, 1 }, b = { 40, SCR_H, BOT_W, SCR_H, 1 };
    switch (effective_layout()) {
    case L_SIDE:   cw = TOP_W + BOT_W; ch = SCR_H; b = (Rect){ TOP_W, 0, BOT_W, SCR_H, 1 }; break;
    case L_LARGE:  cw = TOP_W + BOT_W / 2; ch = SCR_H; b = (Rect){ TOP_W, SCR_H / 2, BOT_W / 2, SCR_H / 2, 1 }; break;
    case L_TOP:    cw = TOP_W; ch = SCR_H; b.on = 0; break;
    case L_BOTTOM: cw = BOT_W; ch = SCR_H; t.on = 0; b = (Rect){ 0, 0, BOT_W, SCR_H, 1 }; break;
    default:       cw = TOP_W; ch = 2 * SCR_H; break;
    }
    float sx = (float)ww / cw, sy = (float)wh / ch;
    if (opt_aspect != 2) { float s = sx < sy ? sx : sy; if (opt_aspect == 1 && s >= 1) s = floorf(s); sx = sy = s; }
    float ox = ((float)ww - cw * sx) / 2, oy = ((float)wh - ch * sy) / 2;
    *top = (Rect){ ox + t.x * sx, oy + t.y * sy, t.w * sx, t.h * sy, t.on };
    *bot = (Rect){ ox + b.x * sx, oy + b.y * sy, b.w * sx, b.h * sy, b.on };
}

/* auto layout: see the comment at the top */
static void auto_update(double now, int bottom_click) {
    static double stick_since = -1, last_stick; static uint32_t prev_btn;
    uint32_t btn = input_buttons(), pressed = btn & ~prev_btn; prev_btn = btn;
    int stick = input_stick_active();
    if (stick) last_stick = now;
    if (!auto_state) {
        if (!stick) stick_since = -1;
        else if (stick_since < 0) stick_since = now;
        else if (now - stick_since >= 1.2) auto_state = 1;
    } else if ((pressed & 0xC) || bottom_click || now - last_stick > 8.0) { auto_state = 0; stick_since = -1; }
}

/* ---------------- Vulkan setup ---------------- */
static uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

typedef struct { VkImage img; VkDeviceMemory mem; VkImageView view; uint32_t w, h; VkFormat fmt; } Img;
static int img_make(Img *im, uint32_t w, uint32_t h, VkFormat fmt, VkImageUsageFlags usage) {
    im->fmt = fmt;
    VkImageCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = fmt, .extent = { w, h, 1 },
        .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    VK(vkCreateImage(dev, &ci, NULL, &im->img));
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(dev, im->img, &mr);
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
    VK(vkAllocateMemory(dev, &ai, NULL, &im->mem)); VK(vkBindImageMemory(dev, im->img, im->mem, 0));
    VkImageViewCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = im->img, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = fmt,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    VK(vkCreateImageView(dev, &vi, NULL, &im->view));
    im->w = w; im->h = h;
    return 0;
}
static void img_free(Img *im) {
    if (!im->img) return;
    if (g_vk_rp) vk_rp_forget(im->view);
    vkDestroyImageView(dev, im->view, NULL); vkDestroyImage(dev, im->img, NULL); vkFreeMemory(dev, im->mem, NULL);
    memset(im, 0, sizeof *im);
}

/* GPU presenter */
static int gpu_ok;
static Img tex_top, tex_bot, target;             /* screens, and the window-sized image drawn into */
static VkFormat target_fmt;
static VkSampler smp_linear;
static VkDescriptorSetLayout dsl;
static VkPipelineLayout play;
static VkPipeline pipe, pipe_easu, pipe_rcas, pipe_rgba, pipe_msr;   /* pipe_msr: Samus Returns' compositor (msr.vert/msr.frag, play_msr) */
static VkPipeline pipe_ui, pipe_msr_ui;   /* the same into the HDR interface layer (coverage in alpha) */
static VkDescriptorSetLayout dsl_hdr; static VkPipelineLayout play_hdr; static VkPipeline pipe_hdr; static VkFormat pipe_hdr_fmt;
static Img ui_layer;   /* HDR, stereo: the interface, over the game (premultiplied, gamma-encoded) */
static VkPipelineLayout play_msr;   /* screens/overlay; FSR upscale into fsr_tmp; FSR sharpen; un-rotate into hi_scr */
static Img fsr_tmp[3], hi_scr[3], tex_toast;   /* FSR intermediates (top, bottom, the right eye's top); the screens at the 3D
                                                  internal resolution (pica_vk scan-out, screen-oriented; [2] the right eye's
                                                  top screen); the layout label */
/* Stereoscopic 3D (#17): the game draws both eyes (gsp.c raises its 3D slider while video_stereo says so) and the
   presenter draws each eye's screens into an image of its own (target: the left eye, target_r: the right), the
   interface once into ui_layer at the same size, and hdr.frag each eye with the interface over it into its half of the
   swapchain image. Everything is laid out for one eye's window (eye_w x eye_h): at half width, the whole window,
   squeezed into its half (3D TVs, Acer SpatialLabs); at full width, half the window: a double-wide window's half at its
   own shape (the game's wider view follows that shape), a usual window's half (narrower than 4:3: a VR player's Full
   SBS) the window's picture at half its size, in the middle of the half (eye_band). The mouse is taken in either half
   (stereo_window_x, stereo_window_y). */
static int stereo_mode;   /* as the swapchain's images were made: 0 off, 1 half width, 2 full width (msr_stereo) */
static Img target_r;
int pica_vk_scanout(uint32_t addr, VkImageView *view, uint32_t *w, uint32_t *h);
void pica_vk_scan_lock(int on);
static VkBuffer up_buf; static VkDeviceMemory up_mem; static uint8_t *up_ptr;   /* uploads: screens, then the layout label */
/* the layout label, in 1x units: drawn at a whole-number scale k (1..4, font_ui.h) that suits the window and shown pixel
   for pixel, so text stays sharp at any window size; the pixel buffer holds the largest scale */
#define TOAST_W 240
#define TOAST_H 22
#define UI_KMAX 4
static uint8_t toast_px[TOAST_W * TOAST_H * 4 * UI_KMAX * UI_KMAX];   /* layout label (next_layout) */
#define UP_TOAST (sizeof top_px + sizeof bot_px)
static VkPushArena push_arena;   /* the stand-in descriptor sets of the frame (vk_push.c), reset at its fence */
typedef struct { float dst[4], src[4]; int mode[4]; float extra[4]; } Push;
typedef struct { float dst[4], src[4], style[4]; int flags[4]; float color[4]; } MsrPush;   /* msr.vert / msr.frag */

static const uint32_t present_vert[] =
#include "present.vert.inc"
;

static VkShaderModule shader(const uint32_t *code, size_t size) {
    VkShaderModuleCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = size, .pCode = code };
    VkShaderModule m = VK_NULL_HANDLE; vkCreateShaderModule(dev, &ci, NULL, &m); return m;
}

/* the Vulkan 1.1 mode's render pass into one colour image of this format (vk_rp.h): the presenter's passes keep their
   attachment in COLOR_ATTACHMENT_OPTIMAL (barrier_img moves it there before and away after, as with dynamic rendering) */
static VkRenderPass pass_for(VkFormat fmt, VkAttachmentLoadOp load) {
    return vk_rp_get(fmt, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, load, VK_FORMAT_UNDEFINED, 0, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE);
}
/* a pass into a colour view (W x H), its viewport and scissor the whole of it; end_pass ends it */
static void begin_pass_view(VkImageView view, VkFormat fmt, VkExtent2D ex, VkAttachmentLoadOp load, float clear_alpha) {
    VkClearValue cv = { .color = { .float32 = { 0, 0, 0, clear_alpha } } };
    if (g_vk_rp) vk_rp_begin(cmdbuf, pass_for(fmt, load), view, VK_NULL_HANDLE, ex, load == VK_ATTACHMENT_LOAD_OP_CLEAR ? &cv : NULL);
    else {
        VkRenderingAttachmentInfo ca = { .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = view, .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .loadOp = load, .storeOp = VK_ATTACHMENT_STORE_OP_STORE, .clearValue = cv };
        VkRenderingInfo ri = { .sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = { { 0, 0 }, ex }, .layerCount = 1, .colorAttachmentCount = 1, .pColorAttachments = &ca };
        vkCmdBeginRendering(cmdbuf, &ri);
    }
    VkViewport vp = { 0, 0, (float)ex.width, (float)ex.height, 0, 1 }; VkRect2D sc = { { 0, 0 }, ex };
    vkCmdSetViewport(cmdbuf, 0, 1, &vp); vkCmdSetScissor(cmdbuf, 0, 1, &sc);
}
static void end_pass(void) { if (g_vk_rp) vkCmdEndRenderPass(cmdbuf); else vkCmdEndRendering(cmdbuf); }

/* one pipeline per fragment shader: present.vert quad (or vs), alpha blending (the screens write alpha 1), dynamic viewport/scissor.
   blend 0: the colour target's alpha is replaced; 1: coverage accumulates in alpha (the HDR interface layer); 2: none */
static VkPipeline make_pipe_ex(const uint32_t *vs, size_t vs_size, const uint32_t *fs, size_t fs_size, VkFormat fmt, VkPipelineLayout layout, int blend);
static VkPipeline make_pipe_with(const uint32_t *vs, size_t vs_size, const uint32_t *fs, size_t fs_size, VkFormat fmt, VkPipelineLayout layout) {
    return make_pipe_ex(vs, vs_size, fs, fs_size, fmt, layout, 0);
}
static VkPipeline make_pipe_ex(const uint32_t *vs, size_t vs_size, const uint32_t *fs, size_t fs_size, VkFormat fmt, VkPipelineLayout layout, int blend) {
    VkShaderModule vm = shader(vs, vs_size), fm = shader(fs, fs_size);
    VkPipeline p = VK_NULL_HANDLE;
    if (vm && fm) {
        VkPipelineShaderStageCreateInfo st[2] = {
            { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vm, .pName = "main" },
            { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fm, .pName = "main" } };
        VkPipelineVertexInputStateCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP };
        VkPipelineViewportStateCreateInfo vp = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1 };
        VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .lineWidth = 1 };
        VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
        VkPipelineColorBlendAttachmentState ba = { .blendEnable = blend != 2, .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA, .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
            .colorBlendOp = VK_BLEND_OP_ADD, .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
            .dstAlphaBlendFactor = blend == 1 ? VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA : VK_BLEND_FACTOR_ZERO, .alphaBlendOp = VK_BLEND_OP_ADD, .colorWriteMask = 0xF };
        VkPipelineColorBlendStateCreateInfo cb = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &ba };
        VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dy = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = dyn };
        VkPipelineRenderingCreateInfo ri = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = 1, .pColorAttachmentFormats = &fmt };
        VkGraphicsPipelineCreateInfo gci = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri, .stageCount = 2, .pStages = st,
            .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vp, .pRasterizationState = &rs, .pMultisampleState = &ms,
            .pColorBlendState = &cb, .pDynamicState = &dy, .layout = layout };
        if (g_vk_rp) { gci.pNext = NULL; gci.renderPass = pass_for(fmt, VK_ATTACHMENT_LOAD_OP_DONT_CARE); }   /* the 1.1 mode (vk_rp.h) */
        if (vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gci, NULL, &p)) p = VK_NULL_HANDLE;
    }
    if (vm) vkDestroyShaderModule(dev, vm, NULL);
    if (fm) vkDestroyShaderModule(dev, fm, NULL);
    return p;
}
static VkPipeline make_pipe(const uint32_t *fs, size_t fs_size, VkFormat fmt) { return make_pipe_with(present_vert, sizeof present_vert, fs, fs_size, fmt, play); }

static int gpu_init(void) {
    if (!vk_hw) return -1;
    if (img_make(&tex_top, TOP_W, SCR_H, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT) ||
        img_make(&tex_bot, BOT_W, SCR_H, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) return -1;
    VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = UP_TOAST + sizeof toast_px, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT };
    VK(vkCreateBuffer(dev, &bi, NULL, &up_buf));
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev, up_buf, &mr);
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
        .memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
    VK(vkAllocateMemory(dev, &mai, NULL, &up_mem)); VK(vkBindBufferMemory(dev, up_buf, up_mem, 0));
    VK(vkMapMemory(dev, up_mem, 0, bi.size, 0, (void **)&up_ptr));
    VkSamplerCreateInfo si = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE };
    VK(vkCreateSampler(dev, &si, NULL, &smp_linear));
    VkDescriptorSetLayoutBinding b = { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
    VkDescriptorSetLayoutCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .flags = vk_push_layout_flags(),
        .bindingCount = 1, .pBindings = &b };
    VK(vkCreateDescriptorSetLayout(dev, &dci, NULL, &dsl));
    VkPushConstantRange pr = { VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push) };
    VkPipelineLayoutCreateInfo pli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pr };
    VK(vkCreatePipelineLayout(dev, &pli, NULL, &play));
    vk_push_layout(play, dsl);
    target_fmt = sdr_format == VK_FORMAT_B8G8R8A8_SRGB ? VK_FORMAT_B8G8R8A8_UNORM : sdr_format == VK_FORMAT_R8G8B8A8_SRGB ? VK_FORMAT_R8G8B8A8_UNORM : sdr_format;
    static const uint32_t fs_present[] =
#include "present.frag.inc"
    ;
    static const uint32_t fs_easu[] =
#include "fsr_easu.frag.inc"
    ;
    static const uint32_t fs_rcas[] =
#include "fsr_rcas.frag.inc"
    ;
    pipe = make_pipe(fs_present, sizeof fs_present, target_fmt);
    pipe_easu = make_pipe(fs_easu, sizeof fs_easu, VK_FORMAT_R8G8B8A8_UNORM);
    pipe_rcas = make_pipe(fs_rcas, sizeof fs_rcas, target_fmt);
    pipe_rgba = make_pipe(fs_present, sizeof fs_present, VK_FORMAT_R8G8B8A8_UNORM);
    if (!pipe || !pipe_easu || !pipe_rcas || !pipe_rgba) return -1;
    {   /* Samus Returns' compositor: its own push constants (MsrPush) */
        static const uint32_t vs_msr[] =
#include "msr.vert.inc"
        ;
        static const uint32_t fs_msr[] =
#include "msr.frag.inc"
        ;
        VkPushConstantRange mr2 = { VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(MsrPush) };
        VkPipelineLayoutCreateInfo ml = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &mr2 };
        VK(vkCreatePipelineLayout(dev, &ml, NULL, &play_msr));
        vk_push_layout(play_msr, dsl);
        pipe_msr = make_pipe_with(vs_msr, sizeof vs_msr, fs_msr, sizeof fs_msr, target_fmt, play_msr);
        pipe_msr_ui = make_pipe_ex(vs_msr, sizeof vs_msr, fs_msr, sizeof fs_msr, target_fmt, play_msr, 1);
        if (!pipe_msr || !pipe_msr_ui) return -1;
    }
    pipe_ui = make_pipe_ex(present_vert, sizeof present_vert, fs_present, sizeof fs_present, target_fmt, play, 1);
    {   /* HDR: two images (game, interface), present.vert's push constants; the pipeline is made for the swapchain's format */
        VkDescriptorSetLayoutBinding hb[2] = { { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
                                               { 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL } };
        VkDescriptorSetLayoutCreateInfo hci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .flags = vk_push_layout_flags(),
            .bindingCount = 2, .pBindings = hb };
        VK(vkCreateDescriptorSetLayout(dev, &hci, NULL, &dsl_hdr));
        VkPipelineLayoutCreateInfo hl = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dsl_hdr, .pushConstantRangeCount = 1, .pPushConstantRanges = &pr };
        VK(vkCreatePipelineLayout(dev, &hl, NULL, &play_hdr));
        vk_push_layout(play_hdr, dsl_hdr);
    }
    if (!pipe_ui) return -1;
    /* tex_toast: made at its first use, at the scale of that moment (ui_img) */
    return 0;
}

static int target_fps = 60;
static int recreate_swapchain(void);
static void match_resolution(int drawable_w, int drawable_h);
/* the refresh rate of the display the window is on (whole hertz, as SDL has it; 0: not known). main.c's next_tick
   checks DWM's against it */
double video_display_hz(void) {
    if (!win) return 0;
    SDL_DisplayMode m; const int i = SDL_GetWindowDisplayIndex(win);
    if (i < 0 || SDL_GetCurrentDisplayMode(i, &m) || m.refresh_rate <= 0) return 0;
    return m.refresh_rate;
}
/* With V-Sync, FIFO unless the frame rate is above the display's (by more than a hertz: 144 FPS on a 143.86 Hz display
   is FIFO), the rule of the emulator-based 1.x (msr/frame_rate.h OutpacesDisplay). Mailbox for any rate above 60 (as
   before) replaced a frame that reached the display a little late (the main loop presents 2 ms before its vblank) with
   the next one: the one before shown twice and that one skipped, a jump when running back and forth (#59, 120 FPS on a
   120 Hz display). FIFO shows it a vblank late instead, and holds the loop to the display where the two clocks drift.
   Faster than the display: mailbox (frames replaced, the game never waits for it), else FIFO. Without V-Sync: mailbox,
   else immediate (tearing). */
static VkPresentModeKHR sc_present_mode;   /* the swapchain's (create_swapchain) */
static VkPresentModeKHR present_mode(void) {
    const double hz = video_display_hz();
    if (opt_vsync && (target_fps <= 60 || (hz > 0 && target_fps <= hz + 1))) return VK_PRESENT_MODE_FIFO_KHR;
    VkPresentModeKHR modes[16]; uint32_t n = 16;
    if (vkGetPhysicalDeviceSurfacePresentModesKHR(phys, surf, &n, modes) != VK_SUCCESS && n == 0) return VK_PRESENT_MODE_FIFO_KHR;
    for (uint32_t i = 0; i < n; i++) if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR) return VK_PRESENT_MODE_MAILBOX_KHR;
    if (!opt_vsync) for (uint32_t i = 0; i < n; i++) if (modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR) return VK_PRESENT_MODE_IMMEDIATE_KHR;
    return VK_PRESENT_MODE_FIFO_KHR;
}
/* the frame rate (also while playing: the overlay's Frame rate) */
static void gl_interval(void);
void video_set_fps(int fps) {
    int before = swap ? (int)present_mode() : -1;
    target_fps = fps;
    gl_interval();
    if (swap && before != (int)present_mode()) recreate_swapchain();
}

/* the surface's formats: SDR (B8G8R8A8_UNORM in the sRGB colour space, else R8G8B8A8_UNORM, else the first: Azahar
   takes only those two, a phone may list another first), and the HDR ones (0 when absent) */
static void surface_formats(VkFormat *sdr, VkColorSpaceKHR *sdr_cs, VkFormat *hdr10, VkFormat *scrgb) {
    uint32_t nf = 0; vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surf, &nf, NULL);
    VkSurfaceFormatKHR fmts[64]; if (nf > 64) nf = 64; vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surf, &nf, fmts);
    *sdr = fmts[0].format; *sdr_cs = fmts[0].colorSpace; *hdr10 = *scrgb = VK_FORMAT_UNDEFINED;
    for (uint32_t i = 0; i < nf; i++)
        if (fmts[i].format == VK_FORMAT_R8G8B8A8_UNORM && fmts[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) { *sdr = fmts[i].format; *sdr_cs = fmts[i].colorSpace; }
    for (uint32_t i = 0; i < nf; i++) {
        if (fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM && fmts[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) { *sdr = fmts[i].format; *sdr_cs = fmts[i].colorSpace; }
        if (fmts[i].colorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT && (fmts[i].format == VK_FORMAT_A2B10G10R10_UNORM_PACK32 || fmts[i].format == VK_FORMAT_A2R10G10B10_UNORM_PACK32)) *hdr10 = fmts[i].format;
        if (fmts[i].colorSpace == VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT && fmts[i].format == VK_FORMAT_R16G16B16A16_SFLOAT) *scrgb = fmts[i].format;
    }
}

int win_hdr_info(void *hwnd, float *peak, float *paper);   /* win/hdr_win.c */
/* whether HDR output is possible on the window's display (at start, and when the settings open: Windows' "Use HDR" may
   have changed), told to the settings overlay */
static void hdr_detect(void) {
    VkFormat sdr, h10, scrgb; VkColorSpaceKHR cs; surface_formats(&sdr, &cs, &h10, &scrgb);
    int formats = has_colorspace_ext && (h10 || scrgb) && !g_vk_rp, opt = 0;   /* (HDR: Vulkan 1.3 only) */
    float peak = 1000.f, paper = 200.f;
#ifdef _WIN32
    SDL_SysWMinfo wi; SDL_VERSION(&wi.version);
    int on = SDL_GetWindowWMInfo(win, &wi) && win_hdr_info(wi.info.win.window, &peak, &paper);
    opt = formats && on ? 2 : 1;
#else
    opt = formats ? 2 : 0;
#endif
    if (opt != hdr_option || (peak < 1000.f ? peak : 1000.f) != disp_peak || paper != disp_paper) {
        fprintf(stderr, "video: HDR %s (display peak %.0f nits, SDR white %.0f nits; swapchain HDR10 %s, scRGB %s)\n",
                opt == 2 ? "possible" : opt == 1 ? "off on this display" : "not available", peak, paper, h10 ? "yes" : "no", scrgb ? "yes" : "no");
        /* the highlights' default stays at 1000 nits however bright the display (a 3,900-nit glow is blinding); the menu goes higher */
        hdr_option = opt; disp_peak = peak < 1000.f ? peak : 1000.f; disp_paper = paper;
        msr_overlay_hdr(opt, disp_peak, paper);
    }
}
/* HDR wanted now: the player's setting, on a display in HDR mode */
static int hdr_wanted(void) { float pk, pp; return msr_hdr(&pk, &pp) && hdr_option == 2 && gpu_ok && vk13; }
/* stereoscopic 3D wanted now: the player's setting, with the GPU presenter and the hardware renderer (the right eye's
   picture is pica_vk's scan-out image) */
static int stereo_wanted(void) { float d; int s; return gpu_ok && vk_hw ? msr_stereo(&d, &s) : 0; }
int video_stereo(void) { return stereo_mode; }
/* one eye's window: its width in swapchain pixels */
static uint32_t eye_w(void) { return stereo_mode == 2 ? (sc_extent.width > 1 ? sc_extent.width / 2 : 1) : sc_extent.width; }
/* Stereo at full width in a usual window (2026-11-22, #17, a Quest 3 through Virtual Desktop's Full SBS: "the pause menu
   and equipment screen, map etc. are still displayed in full height ... which means the top and bottom are cut off"):
   each half of a 2560x1600 desktop was an eye of 1280x1600, and the player showed the middle of it at the desktop's
   shape; play's view (16:10 at its tallest: 1280x800 there) was all in it, the pause screens over the whole half were
   not. A half
   narrower than 4:3 now shows the window's picture at half its size in its middle (1280x800), everything laid out
   there; a double-wide window's half (4:3 or wider) is a picture of its own shape, its whole height. */
static int eye_band(void) {
    return stereo_mode == 2 && sc_extent.height > 1 && (float)eye_w() * 3.f < (float)sc_extent.height * 4.f;
}
/* one eye's window: its height in swapchain pixels (the swapchain's, half of it in a band) */
static uint32_t eye_h(void) { return eye_band() ? sc_extent.height / 2 : sc_extent.height; }
/* stereo: a window x (the mouse) -> one eye's window, the same in either half */
static int stereo_window_x(int x, int ww) {
    if (!stereo_mode || ww < 2) return x;
    const int half = ww / 2, xi = x >= half ? x - half : x;
    return stereo_mode == 1 ? xi * 2 : xi;
}
/* ...and a window y, from the top of the eye's band (outside it: below 0 or past its height) */
static int stereo_window_y(int y, int wh) { return eye_band() ? y - wh / 4 : y; }
/* one eye's window in window coordinates: its height */
static int stereo_window_h(int wh) { return eye_band() ? wh / 2 : wh; }

static int create_swapchain(void) {
    VkSurfaceCapabilitiesKHR caps; vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, surf, &caps);
    if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) { fprintf(stderr, "video: swapchain cannot be a copy target\n"); return -1; }
    VkFormat h10, scrgb; VkColorSpaceKHR cs;
    surface_formats(&sc_format, &cs, &h10, &scrgb);
    hdr_mode = 0;
    if (hdr_wanted() && (h10 || scrgb)) {
        hdr_mode = h10 ? 1 : 2; sc_format = h10 ? h10 : scrgb; cs = h10 ? VK_COLOR_SPACE_HDR10_ST2084_EXT : VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT;
    }
    sc_extent = caps.currentExtent;
    if (sc_extent.width == UINT32_MAX) { int w, h; SDL_Vulkan_GetDrawableSize(win, &w, &h); sc_extent.width = (uint32_t)w; sc_extent.height = (uint32_t)h; }
    VkSurfaceTransformFlagBitsKHR transform = caps.currentTransform;
#ifdef __ANDROID__
    /* a phone held sideways has a 90 degree transform (its panel is portrait): the picture stays upright in landscape
       (identity), sized as the window is (drivers differ on which way round they report the size), and the compositor
       turns it */
    if (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    { int w, h; SDL_Vulkan_GetDrawableSize(win, &w, &h); if (w > 0 && h > 0) { sc_extent.width = (uint32_t)w; sc_extent.height = (uint32_t)h; } }
#endif
    if (!sc_extent.width || !sc_extent.height) return 0;   /* minimised: try again next frame */
    /* opaque, or what the surface has (Android's often only inherit) */
    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(caps.supportedCompositeAlpha & alpha))
        for (uint32_t bit = 1; bit <= VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR; bit <<= 1)
            if (caps.supportedCompositeAlpha & bit) { alpha = (VkCompositeAlphaFlagBitsKHR)bit; break; }
    uint32_t n = caps.minImageCount + 1; if (caps.maxImageCount && n > caps.maxImageCount) n = caps.maxImageCount;
    VkSwapchainCreateInfoKHR ci = { .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = surf, .minImageCount = n,
        .imageFormat = sc_format, .imageColorSpace = cs, .imageExtent = sc_extent, .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .preTransform = transform, .compositeAlpha = alpha,
        .presentMode = present_mode(), .clipped = VK_TRUE, .oldSwapchain = swap };
    sc_present_mode = ci.presentMode;
    VkSwapchainKHR old = swap;
    VkResult sr = vkCreateSwapchainKHR(dev, &ci, NULL, &swap);
    if (sr == VK_ERROR_NATIVE_WINDOW_IN_USE_KHR && !old) {
        /* #53 (Adreno 640, Android 10): the first swapchain refused, the window's surface said to have one already.
           What was asked, for the log; then once more on a surface made again */
        fprintf(stderr, "video: vkCreateSwapchainKHR: the window is in use (images %u of %u-%u, %ux%u, format %d, colour space %d, "
                "usage %#x of %#x, transform %#x of %#x, alpha %#x, present mode %d): its surface again\n", n, caps.minImageCount,
                caps.maxImageCount, sc_extent.width, sc_extent.height, (int)sc_format, (int)cs, (unsigned)ci.imageUsage,
                (unsigned)caps.supportedUsageFlags, (unsigned)transform, (unsigned)caps.supportedTransforms, (unsigned)alpha, (int)ci.presentMode);
        swap = VK_NULL_HANDLE;
        vkDestroySurfaceKHR(inst, surf, NULL); surf = VK_NULL_HANDLE;
        if (!vk_create_surface(win, inst, &surf)) { fprintf(stderr, "video: surface: %s\n", SDL_GetError()); snprintf(init_error, sizeof init_error, "surface: %s", SDL_GetError()); return -1; }
        ci.surface = surf;
        sr = vkCreateSwapchainKHR(dev, &ci, NULL, &swap);
    }
    if (sr != VK_SUCCESS) {
        fprintf(stderr, "video: vkCreateSwapchainKHR(dev, &ci, NULL, &swap) failed (%d)\n", (int)sr);
        snprintf(init_error, sizeof init_error, "vkCreateSwapchainKHR(dev, &ci, NULL, &swap) failed (%d)", (int)sr);
        swap = old;   /* (retired: the next acquire fails and asks for another) */
        return -1;
    }
    if (old) vkDestroySwapchainKHR(dev, old, NULL);
    sc_count = MAX_IMAGES; VK(vkGetSwapchainImagesKHR(dev, swap, &sc_count, sc_images));
    {   /* what the presentation waits on (main.c's "presenting waits" line) */
        static const char *pm[4] = { "immediate", "mailbox", "FIFO", "FIFO relaxed" };
        char hdr[96] = ", SDR";   /* the picture as it is sent: HDR in use, and at what brightness (the settings, else the display's) */
        if (ci.imageColorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT || ci.imageColorSpace == VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT) {
            float pk, pp; msr_hdr(&pk, &pp);
            snprintf(hdr, sizeof hdr, ", HDR on (%s, highlights %.0f nits, paper white %.0f nits%s)",
                     ci.imageColorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT ? "HDR10" : "scRGB", pk > 0 ? pk : disp_peak, pp > 0 ? pp : disp_paper,
                     pk > 0 || pp > 0 ? "" : ": the display's");
        }
        fprintf(stderr, "video: swapchain %ux%u, %u images (at least %u), %s%s\n", sc_extent.width, sc_extent.height, sc_count, caps.minImageCount,
                (unsigned)ci.presentMode < 4 ? pm[ci.presentMode] : "other", hdr);
    }
    sc_bgra = sc_format == VK_FORMAT_B8G8R8A8_UNORM || sc_format == VK_FORMAT_B8G8R8A8_SRGB;
    for (int i = 0; i < MAX_IMAGES; i++) if (sc_views[i]) { if (g_vk_rp) vk_rp_forget(sc_views[i]); vkDestroyImageView(dev, sc_views[i], NULL); sc_views[i] = VK_NULL_HANDLE; }
    if (gpu_ok) {   /* the window-sized image the presenter draws into (stereo: one per eye, each eye_w wide) */
        img_free(&target); img_free(&target_r); img_free(&ui_layer);
        stereo_mode = stereo_wanted();
        const uint32_t ew = eye_w(), eh = eye_h();
        if (img_make(&target, ew, eh, target_fmt, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)) return -1;
        if (stereo_mode && img_make(&target_r, ew, eh, target_fmt, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)) return -1;
        {   /* the log: stereo's layout as made (its depth and eyes: video_frame, as they change) */
            static int told = -1;
            if (stereo_mode) fprintf(stderr, "video: stereoscopic 3D: side by side at %s width, each eye %ux%u%s\n", stereo_mode == 2 ? "full" : "half", ew, eh,
                                     eye_band() ? " (the window's picture at half size, in the middle of its half)" : "");
            else if (told > 0) fprintf(stderr, "video: stereoscopic 3D off\n");
            else if (told < 0) { float d; int s; if (msr_stereo(&d, &s)) fprintf(stderr, "video: stereoscopic 3D chosen, not shown: it needs the hardware renderer\n"); }
            told = stereo_mode;
        }
        if (hdr_mode || stereo_mode) {   /* HDR, stereo: the interface layer, views of the swapchain images (hdr.frag draws into them), the pipeline for this format */
            if (img_make(&ui_layer, ew, eh, target_fmt, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) return -1;   /* (copied: PWR_HDR_DUMP) */
            for (uint32_t i = 0; i < sc_count; i++) {
                VkImageViewCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = sc_images[i], .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = sc_format,
                    .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
                VK(vkCreateImageView(dev, &vi, NULL, &sc_views[i]));
            }
            if (!pipe_hdr || pipe_hdr_fmt != sc_format) {
                static const uint32_t fs_hdr[] =
#include "hdr.frag.inc"
                ;
                if (pipe_hdr) vkDestroyPipeline(dev, pipe_hdr, NULL);
                pipe_hdr = make_pipe_ex(present_vert, sizeof present_vert, fs_hdr, sizeof fs_hdr, sc_format, play_hdr, 2); pipe_hdr_fmt = sc_format;
                if (!pipe_hdr) { fprintf(stderr, "video: %s pipeline failed\n", hdr_mode ? "HDR" : "stereoscopic 3D"); return -1; }
            }
        }
        if (hdr_mode) {
            if (has_hdr_metadata) {   /* what the game's picture reaches: the display tone-maps the rest */
                PFN_vkSetHdrMetadataEXT set = (PFN_vkSetHdrMetadataEXT)vkGetDeviceProcAddr(dev, "vkSetHdrMetadataEXT");
                float pk, pp; msr_hdr(&pk, &pp); if (pk <= 0) pk = disp_peak; if (pp <= 0) pp = disp_paper;
                VkHdrMetadataEXT md = { .sType = VK_STRUCTURE_TYPE_HDR_METADATA_EXT,
                    .displayPrimaryRed = { 0.708f, 0.292f }, .displayPrimaryGreen = { 0.170f, 0.797f }, .displayPrimaryBlue = { 0.131f, 0.046f },
                    .whitePoint = { 0.3127f, 0.3290f }, .maxLuminance = pk, .minLuminance = 0.f, .maxContentLightLevel = pk, .maxFrameAverageLightLevel = pp };
                if (set) set(dev, 1, &swap, &md);
            }
            fprintf(stderr, "video: HDR output (%s)\n", hdr_mode == 1 ? "HDR10" : "scRGB");
        }
        return 0;
    }
    /* CPU path: staging buffer sized to the swapchain */
    if (stage) { vkUnmapMemory(dev, stage_mem); vkDestroyBuffer(dev, stage, NULL); vkFreeMemory(dev, stage_mem, NULL); stage = VK_NULL_HANDLE; }
    stage_size = (VkDeviceSize)sc_extent.width * sc_extent.height * 4;
    VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = stage_size, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT };
    VK(vkCreateBuffer(dev, &bi, NULL, &stage));
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev, stage, &mr);
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
        .memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
    VK(vkAllocateMemory(dev, &mai, NULL, &stage_mem)); VK(vkBindBufferMemory(dev, stage, stage_mem, 0));
    VK(vkMapMemory(dev, stage_mem, 0, stage_size, 0, (void **)&stage_ptr));
    return 0;
}

static int present_due = 1;   /* a picture wanted whether or not the game made a new one (present_wanted) */
static int recreate_swapchain(void) {
    const char *was = stage_set(STAGE_MAIN, "remaking the swapchain");
    queue_acquire(); vk_wait_device(dev); queue_release();
    const int r = create_swapchain();
    present_due = 1;   /* (its images hold nothing yet) */
    stage_set(STAGE_MAIN, was);
    return r;
}
/* the window's surface again, and its swapchain: Android destroys the window's surface while the app is in the
   background (VK_ERROR_SURFACE_LOST_KHR, or the foreground again: SDL_APP_DIDENTERFOREGROUND) */
static int surface_reset;
static int recreate_surface(void) {
    queue_acquire(); vk_wait_device(dev); queue_release();
    if (swap) { vkDestroySwapchainKHR(dev, swap, NULL); swap = VK_NULL_HANDLE; }
    vkDestroySurfaceKHR(inst, surf, NULL); surf = VK_NULL_HANDLE;
    if (!vk_create_surface(win, inst, &surf)) { fprintf(stderr, "video: surface: %s\n", SDL_GetError()); return -1; }
    VkBool32 pres = 0; vkGetPhysicalDeviceSurfaceSupportKHR(phys, qfam, surf, &pres);
    fprintf(stderr, "video: the window's surface again (presentable %d)\n", (int)pres);
    present_due = 1;
    return create_swapchain();
}

/* video_init failed: why, in a message box, as the program has no window (#24: on a computer without a Vulkan driver
   the launcher ended without a word). `log`: the file with the details. */
void video_explain(const char *log) {
    char text[1536];
    snprintf(text, sizeof text,
#if defined(__ANDROID__)
             /* (#54: the PC's text, Mesa and all, on a phone; Android's dialog wraps the lines itself) */
             "msr-native could not open its window: this device's Vulkan graphics driver could not show it.\n\n"
             "Please report it at github.com/gminustwo/msr2ds/issues with your device's name and the file named "
             "below (in Android/data/io.github.gminustwo.msrnative/files/user).\n\n"
#elif defined(__APPLE__)
             "msr-native could not open its window: its Vulkan driver (MoltenVK, inside the app) could not\n"
             "show one. It needs a Mac with Apple Silicon (M1 or newer) and macOS 13 or newer.\n\n"
#else
             "msr-native could not open its window: it needs a graphics driver\n"
#ifdef MSR_OPENGL
             "with Vulkan or OpenGL 3.3, and this computer has none that can show one.\n\n"
#else
             "with Vulkan, and this computer has none that can show one.\n\n"
#endif
#endif
#if defined(__ANDROID__) || defined(__APPLE__)
#elif defined(_WIN32)
             "Update the graphics driver from AMD, NVIDIA or Intel. Graphics older\n"
             "than OpenGL 3.3 (Intel's before its 3rd-generation processors, for\n"
             "example) cannot run msr-native.\n\n"
#elif defined(MSR_OPENGL)
             "Install your system's Vulkan or OpenGL driver (Mesa's for AMD and\n"
             "Intel graphics, or NVIDIA's own).\n\n"
#else
             "Install your system's Vulkan driver (Mesa's for AMD and Intel\n"
             "graphics, or NVIDIA's own).\n\n"
#endif
             "Reason: %s%s%s\nDetails: %s",
             init_error[0] ? init_error : "unknown", gl_error[0] ? "; OpenGL: " : "", gl_error, log && *log ? log : "the log");
    if (win) SDL_HideWindow(win);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "msr-native", text, NULL);
}

/* Vulkan: the window, the instance, the device and the presenter. probe (--renderer=auto): a device without Vulkan 1.3
   returns 1 before the device is made, for OpenGL to try. 0 ok, -1 failed; vk_close then closes what was opened.
   PWR_NO_VULKAN=1 acts as if there were no Vulkan, =13 as if its device had neither 1.3 nor 1.1 (the presenter on the CPU;
   the Vulkan loader ignores its own VK_DRIVER_FILES in an elevated process, so this tests the fallback there too);
   PWR_VK11=1 takes the Vulkan 1.1 mode (vk_rp.h) on any device. */
/* Android, safe levels: a start that ended while opening the graphics (a crash in the driver, or the system's kill) left
   user/msr/graphics-opening.txt behind (written at safe_begin, removed at the first frame shown or a failure that the
   program reports itself), and the next start opens them more simply, one level further each time: 1 without optional
   extensions (attachments neither loaded nor stored, push descriptors, HDR metadata and colour spaces), 2 also in the Vulkan 1.1
   mode (vk_rp.h). The level stays in user/msr/graphics-safe.txt with the version that set it: another version (which
   may have the cause fixed) or deleting the file opens them in full again. The log of the
   start that ended so is kept as <log>-crashed.log (video_keep_crashed_log), since the next start writes its own over it.
   #58: an Adreno 830 (Odin 3) whose launcher ended inside vk_open every time, the same chip as a phone that works. */
static int safe_level;
#ifdef __ANDROID__
#define GFX_OPENING "user/msr/graphics-opening.txt"
#define GFX_SAFE "user/msr/graphics-safe.txt"
static char own_log[512];   /* this process's log (video_keep_crashed_log), named in the marker */
static int safe_open;       /* the marker is there */
void video_keep_crashed_log(const char *log) {
    snprintf(own_log, sizeof own_log, "%s", log);
    FILE *f = fopen(GFX_OPENING, "r"); if (!f) return;
    int level = 0; char ver[32] = "", name[512] = "";   /* the marker: "<level> <version>", then the log's name */
    if (fscanf(f, "%d %31s", &level, ver) < 1) level = 0;
    if (fscanf(f, " %511[^\r\n]", name) != 1) name[0] = 0;
    fclose(f);
    if (!name[0]) return;
    char to[600]; const size_t n = strlen(name);
    snprintf(to, sizeof to, "%.*s-crashed.log", (int)(n > 4 && !strcmp(name + n - 4, ".log") ? n - 4 : n), name);
    remove(to); rename(name, to);
    if ((f = fopen(GFX_OPENING, "w"))) { fprintf(f, "%d %s\n", level, ver); fclose(f); }   /* (kept once) */
}
static void safe_begin(void) {
    int tried = -1, level = 0; char ver[32] = "";
    FILE *f = fopen(GFX_OPENING, "r");
    if (f) {
        if (fscanf(f, "%d %31s", &tried, ver) < 1) tried = 0;
        fclose(f);
        if (strcmp(ver, MSR_NATIVE_VERSION)) tried = -1;   /* another version's start (an update since): not this one's */
    }
    ver[0] = 0;
    if ((f = fopen(GFX_SAFE, "r"))) { if (fscanf(f, "%d %31s", &level, ver) < 1) level = 0; fclose(f); }
    if (strcmp(ver, MSR_NATIVE_VERSION)) level = 0;   /* set by another version */
    if (tried >= 0) {
        const int next = tried + 1 > 2 ? 2 : tried + 1;
        if (next > level) level = next;
        if ((f = fopen(GFX_SAFE, "w"))) { fprintf(f, "%d %s\n", level, MSR_NATIVE_VERSION); fclose(f); }
        fprintf(stderr, "video: the start before this one ended while opening the graphics (at safe level %d; its log may be in a "
                "-crashed.log file beside this one): opening them at safe level %d\n", tried, level);
    }
    if (level < 0) level = 0;
    if (level > 2) level = 2;
    if (level) fprintf(stderr, "video: safe level %d: %s (delete user/msr/graphics-safe.txt to open them in full again)\n", level,
                       level >= 2 ? "the Vulkan 1.1 mode, without optional device extensions" : "without optional device extensions");
    safe_level = level;
    if ((f = fopen(GFX_OPENING, "w"))) { fprintf(f, "%d %s\n%s\n", level, MSR_NATIVE_VERSION, own_log); fclose(f); safe_open = 1; }
}
static void safe_end(void) { if (safe_open) { remove(GFX_OPENING); safe_open = 0; } }
/* The launcher, before video_keep_crashed_log: 1 when the start that ended while opening the graphics was the game's
   (its marker names the game's log), which the launcher then puts down to the player's GPU driver, if one was chosen */
int video_game_ended_opening(void) {
    FILE *f = fopen(GFX_OPENING, "r"); if (!f) return 0;
    int level = 0; char ver[32] = "", name[512] = "";
    if (fscanf(f, "%d %31s", &level, ver) < 1) level = 0;
    if (fscanf(f, " %511[^\r\n]", name) != 1) name[0] = 0;
    fclose(f);
    const char *base = strrchr(name, '/'); base = base ? base + 1 : name;   /* the game's logs: user/logs/msr-native_<date>... (log.c) */
    return !strncmp(base, "msr-native", 10);
}
/* ...and when it did: the next opening (the launcher's, then the game's with the system's driver) takes no safe level */
void video_forget_ended_opening(void) { remove(GFX_OPENING); }
#else
void video_keep_crashed_log(const char *log) { (void)log; }
static void safe_begin(void) {}
static void safe_end(void) {}
#endif

#ifdef MSR_VOLK
/* The player's GPU driver could not be loaded, or could not open the graphics (vk_open, video_init): the Android app's
   launcher shows why under its GPU driver, from user/msr/gpu-driver-failed.txt (its folder's name, then why), which it
   deletes */
static void gpu_driver_failed(const char *why) {
#ifdef __ANDROID__
    FILE *f = fopen("user/msr/gpu-driver-failed.txt", "w");
    if (f) { fprintf(f, "%s\n%s\n", opt_gpu_driver, why); fclose(f); }
#else
    (void)why;
#endif
}
#endif
/* The Vulkan device's vendor (the launcher's GPU driver is for Qualcomm's Adreno GPUs, 0x5143); 0 without Vulkan */
static uint32_t vk_vendor;
unsigned video_gpu_vendor(void) { return vk_vendor; }

static int vk_open(int win_w, int win_h, int probe) {
    const char *no_vk = getenv("PWR_NO_VULKAN");
    if (no_vk && !strcmp(no_vk, "1")) { snprintf(init_error, sizeof init_error, "PWR_NO_VULKAN=1"); return -1; }
    safe_begin();
#ifdef MSR_VOLK
    {   /* Vulkan's library (vk_load.c): for the game, the player's GPU driver where one was chosen */
        char why[300];
        stage_set(STAGE_MAIN, "loading the Vulkan library");
        if (msr_vk_load(gpu_driver_off ? NULL : opt_gpu_driver, why, sizeof why)) { snprintf(init_error, sizeof init_error, "no Vulkan library"); return -1; }
        if (why[0]) gpu_driver_failed(why);
    }
#endif
    stage_set(STAGE_MAIN, "opening the window");
#ifdef __ANDROID__
    const Uint32 win_flags = SDL_WINDOW_VULKAN | SDL_WINDOW_FULLSCREEN;   /* the whole screen, without the system bars */
#elif defined(__APPLE__)
    /* a Retina display's own pixels (the drawable is then larger than the window; input and the overlay map between them) */
    const Uint32 win_flags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
#else
    const Uint32 win_flags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE;
#endif
    win = SDL_CreateWindow("Metroid: Samus Returns", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, win_w, win_h, win_flags | hidden_until_fullscreen());
    if (!win) {   /* no Vulkan library at all (vulkan-1.dll comes with the graphics driver) */
        fprintf(stderr, "video: SDL_CreateWindow: %s\n", SDL_GetError());
        snprintf(init_error, sizeof init_error, "%s", SDL_GetError());
        return -1;
    }
    SDL_SetWindowMinimumSize(win, BOT_W, SCR_H);
    if (opt_fs) { apply_fullscreen(); show_fullscreen(); }
    unsigned ne = 0; SDL_Vulkan_GetInstanceExtensions(win, &ne, NULL);
    const char *exts[20]; if (ne > 16) ne = 16; SDL_Vulkan_GetInstanceExtensions(win, &ne, exts);
    int has_portability_enum = 0;
    {   /* HDR swapchain colour spaces */
        uint32_t n = 0; vkEnumerateInstanceExtensionProperties(NULL, &n, NULL);
        VkExtensionProperties *ep = calloc(n ? n : 1, sizeof *ep); vkEnumerateInstanceExtensionProperties(NULL, &n, ep);
        for (uint32_t i = 0; i < n; i++) if (!strcmp(ep[i].extensionName, VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME)) has_colorspace_ext = 1;
#ifdef __APPLE__
        /* MoltenVK is a portability driver: through the Vulkan loader it is listed only when asked for */
        for (uint32_t i = 0; i < n; i++) if (!strcmp(ep[i].extensionName, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) has_portability_enum = 1;
#endif
        free(ep);
        if (safe_level >= 1) has_colorspace_ext = 0;   /* safe levels: no HDR colour spaces (as Azahar never asks for them) */
        if (has_colorspace_ext) exts[ne++] = VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME;
        if (has_portability_enum) exts[ne++] = VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME;
    }
    /* Vulkan 1.3 where the loader is 1.1 or newer; a Vulkan 1.0 loader (no vkEnumerateInstanceVersion) refuses any
       version but 1.0, and the program then renders on the CPU (#24: a computer whose Vulkan library is 1.0). The 1.1
       function it calls comes from the instance, not vulkan-1.dll, which a 1.0 loader's lacks. */
    uint32_t inst_version = VK_API_VERSION_1_0;
    {
        PFN_vkEnumerateInstanceVersion eiv = (PFN_vkEnumerateInstanceVersion)vkGetInstanceProcAddr(NULL, "vkEnumerateInstanceVersion");
        if (!eiv || eiv(&inst_version) != VK_SUCCESS) inst_version = VK_API_VERSION_1_0;
    }
    /* PWR_VK11=1: the Vulkan 1.1 mode on any device (vk_rp.h), the instance asking for 1.1 only, so that the validation
       layer holds the program to 1.1 (a check of the mode on a PC); also safe level 2 (safe_begin) */
    const int force11 = (getenv("PWR_VK11") && atoi(getenv("PWR_VK11")) == 1) || safe_level >= 2;
    stage_set(STAGE_MAIN, "creating the Vulkan instance");
    VkApplicationInfo ai = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "msr-native",
                             .apiVersion = inst_version < VK_API_VERSION_1_1 ? VK_API_VERSION_1_0 : force11 ? VK_API_VERSION_1_1 : VK_API_VERSION_1_3 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai, .enabledExtensionCount = ne, .ppEnabledExtensionNames = exts,
        .flags = has_portability_enum ? VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR : 0 };
    VK(vkCreateInstance(&ici, NULL, &inst));
#ifdef MSR_VOLK
    volkLoadInstance(inst);
#endif
    if (!vk_create_surface(win, inst, &surf)) {
        fprintf(stderr, "video: surface: %s\n", SDL_GetError());
        snprintf(init_error, sizeof init_error, "surface: %s", SDL_GetError());
        return -1;
    }
    /* the device, of those that can show the window: a discrete GPU before integrated graphics, then the others, in the
       order listed among equals. A laptop's NVIDIA or AMD GPU over its Intel graphics: on Linux, Mesa's device-select
       layer lists the GPU driving the display first, and the first was taken (#56: an Iris Xe over an RTX 3070 Ti, even
       with prime-run); Windows lists them by its power preference. PWR_VK_DEVICE=<n> (its place in the list, from 0) or
       =<part of its name, any case> takes that one where it can show the window. */
    stage_set(STAGE_MAIN, "choosing the Vulkan device");
    uint32_t np = 8; VkPhysicalDevice pds[8]; VK(vkEnumeratePhysicalDevices(inst, &np, pds));
    {
        const char *want = getenv("PWR_VK_DEVICE"); int best = -1;
        for (uint32_t i = 0; i < np; i++) {
            VkPhysicalDeviceProperties dp; vkGetPhysicalDeviceProperties(pds[i], &dp);
            uint32_t nq = 16; VkQueueFamilyProperties qp[16]; vkGetPhysicalDeviceQueueFamilyProperties(pds[i], &nq, qp);
            int fam = -1;
            for (uint32_t q = 0; q < nq && fam < 0; q++) {
                VkBool32 pres = 0; vkGetPhysicalDeviceSurfaceSupportKHR(pds[i], q, surf, &pres);
                if ((qp[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) && pres) fam = (int)q;
            }
            const VkPhysicalDeviceType t = dp.deviceType;
            int score = fam < 0 ? -1 : t == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 4 : t == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 3
                      : t == VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU ? 2 : t == VK_PHYSICAL_DEVICE_TYPE_CPU ? 0 : 1;
            if (want && *want && fam >= 0) {
                char *end; const long n = strtol(want, &end, 10);
                int match = !*end && n == (long)i;
                for (const char *s = dp.deviceName; *end && *s && !match; s++) {   /* the name, without regard to case */
                    size_t k = 0; while (want[k] && s[k] && tolower((unsigned char)s[k]) == tolower((unsigned char)want[k])) k++;
                    match = !want[k];
                }
                if (match) score = 10;
            }
            if (np > 1)
                fprintf(stderr, "video: Vulkan device %u of %u: %s (%s)%s\n", i, np, dp.deviceName, t == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? "discrete"
                        : t == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? "integrated" : t == VK_PHYSICAL_DEVICE_TYPE_CPU ? "software" : "other",
                        fam < 0 ? ", cannot show the window" : "");
            if (score > best) { best = score; phys = pds[i]; qfam = (uint32_t)fam; }
        }
    }
    if (!phys) {
        fprintf(stderr, "video: no Vulkan device can present to the window\n");
        snprintf(init_error, sizeof init_error, "no Vulkan device can show the window (%u found)", np);
        return -1;
    }
    VkPhysicalDeviceProperties pp; vkGetPhysicalDeviceProperties(phys, &pp);
    vk_vendor = pp.vendorID;
    {   /* its kind and memory, for the logs players send (integrated graphics share the computer's memory) */
        VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(phys, &mp);
        double local = 0; for (uint32_t h = 0; h < mp.memoryHeapCount; h++) if (mp.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) local += (double)mp.memoryHeaps[h].size;
        const char *kind = pp.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? "discrete" : pp.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? "integrated"
                         : pp.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU ? "software" : "other";
        const uint32_t v = pp.driverVersion; char driver[32];   /* NVIDIA's and Intel's (Windows) own encodings, else Vulkan's */
        if (pp.vendorID == 0x10DE) snprintf(driver, sizeof driver, "%u.%02u", v >> 22, (v >> 14) & 0xff);
#ifdef _WIN32
        else if (pp.vendorID == 0x8086) snprintf(driver, sizeof driver, "%u.%u", v >> 14, v & 0x3fff);
#endif
        else snprintf(driver, sizeof driver, "%u.%u.%u", v >> 22, (v >> 12) & 0x3ff, v & 0xfff);
        /* how much of a discrete card's memory the processor can write (Resizable BAR: all of it; without, 256 MB), for the
           stutter reports, where uploads without it go the long way */
        double bar = 0;
        for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
            const VkMemoryPropertyFlags f = mp.memoryTypes[i].propertyFlags;
            const double s = (double)mp.memoryHeaps[mp.memoryTypes[i].heapIndex].size;
            if ((f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) && (f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) && s > bar) bar = s;
        }
        char bar_text[64] = "";
        if (pp.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
            snprintf(bar_text, sizeof bar_text, bar > 300.0 * 1048576 ? ", Resizable BAR on (%.1f GB)" : bar > 0 ? ", Resizable BAR off (%.0f MB)"
                     : ", no graphics memory the processor writes", bar > 300.0 * 1048576 ? bar / 1073741824.0 : bar / 1048576.0);
        fprintf(stderr, "video: Vulkan device %s (%s, %.1f GB of graphics memory, driver %s%s)\n", pp.deviceName, kind, local / 1073741824.0, driver, bar_text);
    }
    /* whose driver (pica_vk.c: ARM's gets the depth and stencil state in its pipelines). Vulkan 1.2's driver ID where the
       device has it, else the vendor: ARM's and Qualcomm's own drivers on the phones of before 1.2 (Mesa's report 1.2+) */
    g_vk_driver_id = 0;
    if (pp.apiVersion >= VK_API_VERSION_1_2 && inst_version >= VK_API_VERSION_1_1) {
        PFN_vkGetPhysicalDeviceProperties2 props2 = (PFN_vkGetPhysicalDeviceProperties2)vkGetInstanceProcAddr(inst, "vkGetPhysicalDeviceProperties2");
        VkPhysicalDeviceDriverProperties drv = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES };
        VkPhysicalDeviceProperties2 p2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &drv };
        if (props2) { props2(phys, &p2); g_vk_driver_id = (int)drv.driverID; }
    }
    if (!g_vk_driver_id && pp.vendorID == 0x13B5) g_vk_driver_id = VK_DRIVER_ID_ARM_PROPRIETARY;
    if (!g_vk_driver_id && pp.vendorID == 0x5143) g_vk_driver_id = VK_DRIVER_ID_QUALCOMM_PROPRIETARY;
    float prio = 1;
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = qfam, .queueCount = 1, .pQueuePriorities = &prio };
    /* the hardware PICA renderer (pica_vk.c) and the GPU presenter need Vulkan 1.3 dynamic rendering + extended dynamic state and push descriptors */
    VkPhysicalDeviceVulkan13Features f13 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
    VkPhysicalDeviceFeatures2 f2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f13 };
    uint32_t nde = 0; vkEnumerateDeviceExtensionProperties(phys, NULL, &nde, NULL);
    VkExtensionProperties *dep = calloc(nde ? nde : 1, sizeof *dep); vkEnumerateDeviceExtensionProperties(phys, NULL, &nde, dep);
    int has_push = 0; for (uint32_t i = 0; i < nde; i++) if (!strcmp(dep[i].extensionName, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME)) has_push = 1;
    int has_portability_subset = 0;   /* MoltenVK: a device that has it must enable it */
    for (uint32_t i = 0; i < nde; i++) if (!strcmp(dep[i].extensionName, "VK_KHR_portability_subset")) has_portability_subset = 1;
    for (uint32_t i = 0; i < nde; i++) if (!strcmp(dep[i].extensionName, VK_EXT_HDR_METADATA_EXTENSION_NAME)) has_hdr_metadata = 1;
    int has_khr_none = 0, has_ext_none = 0;   /* attachments neither loaded nor stored (pica_vk.c begin_rendering) */
    for (uint32_t i = 0; i < nde; i++) {
        if (!strcmp(dep[i].extensionName, "VK_KHR_load_store_op_none")) has_khr_none = 1;
        if (!strcmp(dep[i].extensionName, "VK_EXT_load_store_op_none")) has_ext_none = 1;
    }
    const char *op_none = has_khr_none ? "VK_KHR_load_store_op_none" : has_ext_none ? "VK_EXT_load_store_op_none" : NULL;
    int has_metal_objects = 0;   /* MoltenVK: the drawable taken outside the queue (export_metal) */
#ifdef __APPLE__
    for (uint32_t i = 0; i < nde; i++) if (!strcmp(dep[i].extensionName, VK_EXT_METAL_OBJECTS_EXTENSION_NAME)) has_metal_objects = 1;
    if (getenv("PWR_MVK_LATE_DRAWABLE")) has_metal_objects = 0;   /* PWR_MVK_LATE_DRAWABLE=1: MoltenVK takes it in the submission (A/B) */
#endif
    int diag_fault, diag_marks; vk_diag_wanted(dep, nde, &diag_fault, &diag_marks);   /* if the GPU is lost: where (vk_diag.c) */
    free(dep);
    if (safe_level >= 1) { has_push = 0; op_none = NULL; has_hdr_metadata = 0; has_metal_objects = 0; diag_fault = diag_marks = 0; }   /* safe levels: no optional device extensions (safe_begin) */
#ifdef VK_EXT_device_fault
    VkPhysicalDeviceFaultFeaturesEXT fault_have = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT },
                                     fault_on = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT, .deviceFault = VK_TRUE };
    if (diag_fault) f13.pNext = &fault_have;
#else
    diag_fault = 0;
#endif
    PFN_vkGetPhysicalDeviceFeatures2 features2 = inst_version >= VK_API_VERSION_1_1
        ? (PFN_vkGetPhysicalDeviceFeatures2)vkGetInstanceProcAddr(inst, "vkGetPhysicalDeviceFeatures2") : NULL;
    if (pp.apiVersion >= VK_API_VERSION_1_3 && features2 && !force11) features2(phys, &f2);
    /* push descriptors where the device has them, else descriptor sets in their place (vk_push.c) */
    vk13 = pp.apiVersion >= VK_API_VERSION_1_3 && f13.dynamicRendering && f13.shaderDemoteToHelperInvocation && !(no_vk && !strcmp(no_vk, "13")) && !force11;
    /* Without them, the Vulkan 1.1 mode (vk_rp.h: render passes, the depth and stencil state in the pipelines, SPIR-V 1.3):
       phones' GPUs of about 2019-2021 (Adreno 6xx, Mali-G7x) and older PCs' drivers. On a PC --renderer=auto tries OpenGL
       first there (as before the mode existed: video_init), then this. */
    g_vk_rp = !vk13 && inst_version >= VK_API_VERSION_1_1 && pp.apiVersion >= VK_API_VERSION_1_1 && !(no_vk && !strcmp(no_vk, "13"));
    vk_hw = vk13 || g_vk_rp;
    if (probe && !vk13 && !force11) {   /* OpenGL first (video_init) */
        fprintf(stderr, "video: Vulkan %u.%u without 1.3's dynamic rendering (%d) and demote to helper (%d): trying OpenGL\n",
                VK_API_VERSION_MAJOR(pp.apiVersion), VK_API_VERSION_MINOR(pp.apiVersion), (int)f13.dynamicRendering, (int)f13.shaderDemoteToHelperInvocation);
        snprintf(init_error, sizeof init_error, "Vulkan %u.%u, without 1.3", VK_API_VERSION_MAJOR(pp.apiVersion), VK_API_VERSION_MINOR(pp.apiVersion));
        return 1;
    }
    VkPhysicalDeviceVulkan13Features en13 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, .dynamicRendering = VK_TRUE, .shaderDemoteToHelperInvocation = VK_TRUE };
    VkPhysicalDeviceFeatures2 en2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &en13 };
    en2.features.samplerAnisotropy = f2.features.samplerAnisotropy;   /* pica_vk.c: anisotropic filtering option */
    VkPhysicalDeviceFeatures f10 = { 0 }, en10 = { 0 };   /* the 1.1 mode's (Vulkan 1.0's query) */
    if (g_vk_rp) { vkGetPhysicalDeviceFeatures(phys, &f10); en10.samplerAnisotropy = f10.samplerAnisotropy; }
    /* Robust buffer access, as Azahar enables it: a read past a bound buffer range gives zeros, not a GPU fault. The
       GPU vertex shaders read the game's attributes and programs from storage buffers at computed offsets, where a phone's
       GPU faults or hangs on a stray index (#53, #52). PWR_VK_NO_ROBUST=1: without (A/B: its cost). */
    if (vk_hw && !getenv("PWR_VK_NO_ROBUST")) {
        en2.features.robustBufferAccess = f2.features.robustBufferAccess;
        en10.robustBufferAccess = f10.robustBufferAccess;
        if (vk13 ? f2.features.robustBufferAccess : f10.robustBufferAccess) fprintf(stderr, "video: robust buffer access on\n");
    }
    const char *dext[10] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME }; uint32_t ndext = 1;
#ifdef VK_EXT_device_fault
    diag_fault = diag_fault && vk13 && fault_have.deviceFault;
    if (diag_fault) { dext[ndext++] = VK_EXT_DEVICE_FAULT_EXTENSION_NAME; en13.pNext = &fault_on; }
#endif
    if (diag_marks) dext[ndext++] = VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME;
    if (has_portability_subset) dext[ndext++] = "VK_KHR_portability_subset";
    if (vk_hw && has_push) dext[ndext++] = VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME;
    if (vk13 && has_hdr_metadata) dext[ndext++] = VK_EXT_HDR_METADATA_EXTENSION_NAME; else has_hdr_metadata = 0;
#ifdef __APPLE__
    if (has_metal_objects) dext[ndext++] = VK_EXT_METAL_OBJECTS_EXTENSION_NAME;
#endif
    /* the extension (the instance asks for 1.3, so 1.4's core ops are not ours); not through MoltenVK (Metal's load and
       store actions differ) */
    g_vk_attachment_none = vk_hw && !has_portability_subset && op_none;
    if (g_vk_attachment_none) dext[ndext++] = op_none;
    if (vk_hw) fprintf(stderr, "video: depth left alone by passes that don't use it: %s\n", g_vk_attachment_none ? op_none : "no (loaded and stored)");
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = vk13 ? &en2 : NULL, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
        .enabledExtensionCount = ndext, .ppEnabledExtensionNames = dext, .pEnabledFeatures = g_vk_rp ? &en10 : NULL };
    if (g_vk_rp) fprintf(stderr, "video: Vulkan %u.%u without 1.3's dynamic rendering (%d) and demote to helper (%d): the Vulkan 1.1 mode "
                         "(render passes; push descriptors %s)\n", VK_API_VERSION_MAJOR(pp.apiVersion), VK_API_VERSION_MINOR(pp.apiVersion),
                         (int)f13.dynamicRendering, (int)f13.shaderDemoteToHelperInvocation, has_push ? "yes" : "no");
    else if (!vk13) fprintf(stderr, "video: no Vulkan 1.3 dynamic rendering: hardware PICA renderer and GPU presenter unavailable "
                       "(Vulkan %u.%u, dynamic rendering %d, demote to helper %d)\n",
                       VK_API_VERSION_MAJOR(pp.apiVersion), VK_API_VERSION_MINOR(pp.apiVersion), (int)f13.dynamicRendering,
                       (int)f13.shaderDemoteToHelperInvocation);
    stage_set(STAGE_MAIN, "creating the Vulkan device");
    VK(vkCreateDevice(phys, &dci, NULL, &dev));
#ifdef MSR_VOLK
    volkLoadDevice(dev);   /* the device's own functions, and Vulkan 1.3's (msr_vk13_load below) */
#endif
    stage_set(STAGE_MAIN, "setting up the device's functions");
    if (vk_hw) vk_push_init(dev, has_push && !getenv("PWR_VK_NO_PUSH"));   /* PWR_VK_NO_PUSH=1: the stand-in sets (vk_push.c), as on Mali */
    if (g_vk_rp) vk_rp_init(dev);
#ifdef MSR_VK13_FROM_DEVICE
    { extern void msr_vk13_load(VkDevice d); if (vk13) msr_vk13_load(dev); }   /* vk13_load.c */
#endif
    vkGetDeviceQueue(dev, qfam, 0, &queue);
    vk_diag_init(dev, queue, diag_fault, diag_marks);
#ifdef __APPLE__
    export_metal = has_metal_objects ? (PFN_vkExportMetalObjectsEXT)vkGetDeviceProcAddr(dev, "vkExportMetalObjectsEXT") : NULL;
    fprintf(stderr, "video: MoltenVK %s queue submissions; the window's drawable taken %s\n",
            getenv("MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS") && strcmp(getenv("MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS"), "0") ? "synchronous" : "asynchronous",
            export_metal ? "after the acquire (VK_EXT_metal_objects)" : "in the submission");
#endif
    /* swapchain format first (the presenter's target uses it), then the presenter, then the swapchain-sized resources */
    stage_set(STAGE_MAIN, "reading the window's formats");
    { VkFormat h10, scrgb; VkColorSpaceKHR cs; surface_formats(&sdr_format, &cs, &h10, &scrgb); sc_format = sdr_format; }
    stage_set(STAGE_MAIN, "making the presenter (its shaders compiled)");
    gpu_ok = !gpu_init();
    stage_set(STAGE_MAIN, "checking for HDR");
    hdr_detect();
    if (!gpu_ok) fprintf(stderr, "video: GPU presenter unavailable, scaling on the CPU (nearest)\n");
    stage_set(STAGE_MAIN, "making the swapchain");
    if (create_swapchain()) return -1;
    stage_set(STAGE_MAIN, "making the command buffers");
    if (opt_res_match) { int w, h; SDL_Vulkan_GetDrawableSize(win, &w, &h); match_resolution(w, h); }   /* --resolution=0 */

    VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = qfam };
    VK(vkCreateCommandPool(dev, &pci, NULL, &pool));
    VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    VK(vkAllocateCommandBuffers(dev, &cai, &cmdbuf));
    VkSemaphoreCreateInfo si = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VK(vkCreateSemaphore(dev, &si, NULL, &sem_acquire)); for (int i = 0; i < MAX_IMAGES; i++) VK(vkCreateSemaphore(dev, &si, NULL, &sem_done[i]));
    VkFenceCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT };
    VK(vkCreateFence(dev, &fi, NULL, &fence));
    if (gpu_ok) {   /* the settings overlay draws into the presentation's target (its font goes up on the shared queue) */
        stage_set(STAGE_MAIN, "starting the overlay (its shaders compiled, its font uploaded)");
        queue_acquire();
        msr_overlay_vk_init(inst, phys, dev, qfam, queue, target_fmt, sc_count, g_vk_rp ? pass_for(target_fmt, VK_ATTACHMENT_LOAD_OP_DONT_CARE) : VK_NULL_HANDLE);
        queue_release();
    }
    stage_set(STAGE_MAIN, NULL);
    fprintf(stderr, "video: presenting with Vulkan\n");
    display_line();
    return 0;
}
/* vk_open, its stage cleared after it; a failure it reports (or OpenGL to try first) is not a crash: no safe level for it */
static int vk_open_(int win_w, int win_h, int probe) {
    const int r = vk_open(win_w, win_h, probe);
    stage_set(STAGE_MAIN, NULL);
    if (r) safe_end();
    return r;
}
/* what vk_open opened, so that OpenGL can have the window. Past the device (rare: a failure making the swapchain or the
   presenter), the device and the instance are left as they are, unused. */
static void vk_close(void) {
    if (dev) { vk_wait_device(dev); if (swap) vkDestroySwapchainKHR(dev, swap, NULL); }
    if (surf) vkDestroySurfaceKHR(inst, surf, NULL);
    if (inst && !dev) vkDestroyInstance(inst, NULL);
    if (win) SDL_DestroyWindow(win);
    win = NULL; swap = VK_NULL_HANDLE; surf = VK_NULL_HANDLE; inst = VK_NULL_HANDLE; dev = VK_NULL_HANDLE; phys = VK_NULL_HANDLE;
    gpu_ok = vk13 = vk_hw = g_vk_rp = 0;
}

/* OpenGL (video_gl.c): the game's frames are shown without waiting for the display. main.c already paces them (VBlank
   2 ms before each of the display's vblanks), and a swap that waited held up the PICA renderer's context on the GX worker:
   on NVIDIA each write-back waited for the next vblank (7 FPS on the title screen; 27 without the wait, nothing else
   changed). A composited desktop (Windows' DWM) does not tear; exclusive fullscreen can. The launcher, which draws no 3D,
   waits with V-Sync as Vulkan's FIFO does. */
static void gl_interval(void) {
#ifdef MSR_OPENGL
    if (use_gl) vgl_swap_interval(opt_vsync && target_fps <= 60 && msr_overlay_launcher_mode() ? 1 : 0);
#endif
}
#ifdef MSR_OPENGL
static int gl_open(int win_w, int win_h) {
    SDL_GL_ResetAttributes();
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    if (getenv("PWR_GL_DEBUG")) SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_DEBUG_FLAG);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 0);
    win = SDL_CreateWindow("Metroid: Samus Returns", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, win_w, win_h,
                           SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | hidden_until_fullscreen());
    if (!win) { snprintf(gl_error, sizeof gl_error, "%s", SDL_GetError()); fprintf(stderr, "video: OpenGL window: %s\n", gl_error); return -1; }
    SDL_SetWindowMinimumSize(win, BOT_W, SCR_H);
    if (opt_fs) { apply_fullscreen(); show_fullscreen(); }
    if (vgl_init(win, gl_error, sizeof gl_error)) { fprintf(stderr, "video: OpenGL: %s\n", gl_error); return -1; }
    use_gl = 1;
    gl_interval();
    msr_overlay_hdr(0, disp_peak, disp_paper);   /* no HDR output with OpenGL */
    if (opt_res_match) { int w, h; drawable_size(&w, &h); match_resolution(w, h); }   /* --resolution=0 */
    fprintf(stderr, "video: presenting with OpenGL\n");
    display_line();
    return 0;
}
static void gl_close(void) {
    vgl_shutdown();
    if (win) SDL_DestroyWindow(win);
    win = NULL; use_gl = 0;
}
#endif

int video_init(int scale) {
    /* every pad's face buttons by position (A = bottom), as input.c, glyphs.cpp and the settings take them: SDL 2 names a
       Nintendo pad's by label unless told (its A, on the right, came as SDL's A, the bottom button) */
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, "0");
#ifdef SDL_HINT_AUTO_UPDATE_JOYSTICKS
    /* the controllers are updated once a frame by video_frame (SDL_JoystickUpdate), apart from the window's messages
       (SDL_PollEvent, which did both), so main.c's long-frame lines tell the two apart: on Windows a scan of the devices
       after a change in them can take half a second (2026-11-15: a 0.5 s freeze inside SDL_PollEvent on the user's PC) */
    SDL_SetHint(SDL_HINT_AUTO_UPDATE_JOYSTICKS, "0");
#endif
#ifdef __APPLE__
    /* MoltenVK: its errors only in the log (not its device report, nor a warning for each pipeline that Metal always
       restarts primitives); MVK_CONFIG_LOG_LEVEL set by the player wins */
    setenv("MVK_CONFIG_LOG_LEVEL", "1", 0);
    /* MoltenVK: submissions and presents encoded into Metal on a queue of its own (GCD, user-initiated), not on the
       thread that submits. Synchronous (MoltenVK's default), the GX thread encoded the game's command buffers for Metal
       itself, and the main thread's own submission sat in nextDrawable holding the queue (export_metal, above). Its
       vkQueueWaitIdle waits for that queue first (MVKQueue::waitIdle, 1.4.2), so pica_vk.c's idle waits hold. On an M3
       Max at 6x/120 FPS (2026-11-18) either this or export_metal alone held 120 where 2.1.0 made 30-60; with both the
       GX thread's flush took 0.15 ms a frame (0.9 with either alone, 5.6 in 2.1.0). MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS=1
       set by the player wins (A/B) */
    setenv("MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS", "0", 0);
#endif
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | (g_input_none ? 0 : SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK))) { fprintf(stderr, "video: SDL_Init: %s\n", SDL_GetError()); return -1; }
    static const int cw[L_COUNT] = { TOP_W, TOP_W + BOT_W, TOP_W + BOT_W / 2, TOP_W, BOT_W, 0, TOP_W }, chh[L_COUNT] = { 2 * SCR_H, SCR_H, SCR_H, SCR_H, SCR_H, SCR_H, 2 * SCR_H };
    const int win_w = opt_layout == L_SINGLE ? (int)lroundf(SCR_H * scale * opt_window_aspect) : cw[opt_layout] * scale;   /* single: 16:9 (1280x720 at 3) */
    const int win_h = chh[opt_layout] * scale;
    /* auto: Vulkan where it has 1.3, else OpenGL, else Vulkan without 1.3 (its presenter scales on the CPU) */
    int vk = -1;
#ifdef MSR_OPENGL
    if (opt_renderer != R_OPENGL) {
        if (!(vk = vk_open_(win_w, win_h, opt_renderer == R_AUTO))) return 0;
        vk_close();
        if (opt_renderer == R_VULKAN) return -1;
    }
    if (!gl_open(win_w, win_h)) return 0;
    gl_close();
    if (vk == 1) { if (!vk_open_(win_w, win_h, 0)) return 0; vk_close(); }
    return -1;
#else
    if (opt_renderer == R_OPENGL) fprintf(stderr, "video: no OpenGL in this build: Vulkan\n");
    if (!(vk = vk_open_(win_w, win_h, 0))) return 0;
    vk_close();
#ifdef MSR_VOLK
    if (msr_vk_custom()) {   /* the player's GPU driver could not open them: the system's, as the launcher has */
        char why[300]; snprintf(why, sizeof why, "it could not open the graphics: %s", init_error);
        gpu_driver_failed(why);
        gpu_driver_off = 1;
        if (!(vk = vk_open_(win_w, win_h, 0))) return 0;
        vk_close();
    }
#endif
    return -1;
#endif
}

/* LCD framebuffers are rotated: each line holds 240 pixels running bottom-to-top of one screen column */
static void decode_screen(const ScreenSource *s, int w, uint8_t *out) {
    static const int bpp_of[8] = { 4, 3, 2, 2, 2, 0, 0, 0 };
    int bpp = bpp_of[s->format & 7];
    if (!s->valid || !s->addr || !bpp) {   /* no scan-out source yet: dark grey, so screenshots show the screen area */
        for (int i = 0; i < w * SCR_H; i++) { out[4 * i] = out[4 * i + 1] = out[4 * i + 2] = 40; out[4 * i + 3] = 255; }
        return;
    }
    uint32_t stride = s->stride ? s->stride : 240u * (uint32_t)bpp;
    for (int x = 0; x < w; x++) for (int y = 0; y < SCR_H; y++) {
        const uint8_t *p = g_mem + s->addr + (uint32_t)x * stride + (uint32_t)(239 - y) * (uint32_t)bpp;
        uint8_t *d = out + ((size_t)y * (size_t)w + (size_t)x) * 4; uint32_t v;
        switch (s->format & 7) {
        case 0: d[0] = p[3]; d[1] = p[2]; d[2] = p[1]; break;
        case 1: d[0] = p[2]; d[1] = p[1]; d[2] = p[0]; break;
        case 2: v = p[0] | p[1] << 8; d[0] = (uint8_t)((v >> 11) << 3); d[1] = (uint8_t)(((v >> 5) & 63) << 2); d[2] = (uint8_t)((v & 31) << 3); break;
        case 3: v = p[0] | p[1] << 8; d[0] = (uint8_t)((v >> 11) << 3); d[1] = (uint8_t)(((v >> 6) & 31) << 3); d[2] = (uint8_t)(((v >> 1) & 31) << 3); break;
        case 4: v = p[0] | p[1] << 8; d[0] = (uint8_t)((v >> 12) * 17); d[1] = (uint8_t)(((v >> 8) & 15) * 17); d[2] = (uint8_t)(((v >> 4) & 15) * 17); break;
        }
        d[3] = 255;
    }
}

static void barrier_img(VkImage img, VkImageLayout from, VkImageLayout to, VkAccessFlags src, VkAccessFlags dst) {
    VkImageMemoryBarrier b = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = src, .dstAccessMask = dst, .oldLayout = from, .newLayout = to,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    vkCmdPipelineBarrier(cmdbuf, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}

/* ---------------- the settings overlay's display settings, the layout label ---------------- */
#include "font_ui.h"
/* text and boxes in pixels of a label drawn at scale k (cells of 8k x 16k) */
static void text(uint8_t *px, int pw, int ph, int x, int y, const char *str, uint32_t rgb, int k) {
    for (; *str; str++, x += 8 * k) {
        int c = (unsigned char)*str; if (c < 32 || c > 126) c = '?';
        const unsigned char *g = font_ui_glyph(k, c);
        for (int gy = 0; gy < 16 * k; gy++) for (int gx = 0; gx < 8 * k; gx++) {
            int X = x + gx, Y = y + gy; unsigned a = g[gy * 8 * k + gx];
            if (!a || X < 0 || Y < 0 || X >= pw || Y >= ph) continue;
            uint8_t *d = px + ((size_t)Y * (size_t)pw + (size_t)X) * 4;
            for (int ch = 0; ch < 3; ch++) d[ch] = (uint8_t)((d[ch] * (255 - a) + ((rgb >> (16 - 8 * ch)) & 255) * a) / 255);
            if (d[3] < a) d[3] = (uint8_t)a;
        }
    }
}
static void fill(uint8_t *px, int pw, int x0, int y0, int w, int h, uint32_t rgb, int a) {
    for (int y = y0; y < y0 + h; y++) for (int x = x0; x < x0 + w; x++) {
        uint8_t *d = px + ((size_t)y * (size_t)pw + (size_t)x) * 4; d[0] = (uint8_t)(rgb >> 16); d[1] = (uint8_t)(rgb >> 8); d[2] = (uint8_t)rgb; d[3] = (uint8_t)a;
    }
}

void video_set_fullscreen(int on) { opt_fs = on ? fs_last : 0; apply_fullscreen(); }
/* the launcher (msr/launcher.cpp): its window hides while the game runs; the file dialog's owner */
int video_gl(void) { return use_gl; }
void video_show(int on) { if (!win) return; if (on) { SDL_ShowWindow(win); SDL_RaiseWindow(win); } else SDL_HideWindow(win); }
SDL_Window *video_window(void) { return win; }
void video_set_vsync(int on) { opt_vsync = on != 0; gl_interval(); if (swap) recreate_swapchain(); }
/* the 3D resolution: 1..10, or 0 to follow the window: the 240-line multiple nearest the top screen's height as shown
   (FSR takes the rest). By the window's whole height, rounded up, a 1280x800 Steam Deck got 4x: 1600x960 drawn for a
   view of 720 lines (16:9) or 768 (the 3DS's 5:3); 3x now. */
static void match_resolution(int drawable_w, int drawable_h) {
    Rect t, b; layout_rects(drawable_w, drawable_h, &t, &b);
    int lines = t.on && t.h >= 1 ? (int)(t.h + 0.5f) : drawable_h;
    int r = (lines + SCR_H / 2) / SCR_H; r = r < 1 ? 1 : r > 10 ? 10 : r;
    if (r != opt_res) { opt_res = r; pica_vk_quality(opt_res, opt_aniso); }
}
void video_set_resolution(int factor) {
    opt_res_match = factor <= 0;
    if (opt_res_match) { int w, h; drawable_size(&w, &h); match_resolution(w, h); return; }
    opt_res = factor > 10 ? 10 : factor; pica_vk_quality(opt_res, opt_aniso);
}
int video_resolution(void) { return opt_res_match ? 0 : opt_res; }
/* anisotropic filtering: 1 (off), 2, 4, 8 or 16, the largest of them up to `aniso`; pica_vk.c makes samplers for it from
   the next draw (its sampler cache is keyed by the level) */
void video_set_aniso(int aniso) {
    opt_aniso = aniso >= 16 ? 16 : aniso >= 8 ? 8 : aniso >= 4 ? 4 : aniso >= 2 ? 2 : 1; pica_vk_quality(opt_res, opt_aniso);
}
int video_aniso(void) { return opt_aniso; }
int video_filter_fsr(void) { return opt_filter == F_FSR; }
int video_fullscreen(void) { return opt_fs != 0; }
int video_vsync(void) { return opt_vsync; }
/* every gamepad SDL can map is open (Steam can show a pad twice, a player can switch pads); gc is the one used last: its
   sticks are read, its type names the buttons (msr/glyphs.cpp) */
#define MAX_PADS 4
static SDL_GameController *gcs[MAX_PADS], *gc; static SDL_Joystick *joy;
int video_pad_connected(void) { return gc || joy; }
static SDL_GameController *pad_of(SDL_JoystickID id) {
    for (int i = 0; i < MAX_PADS; i++) if (gcs[i] && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(gcs[i])) == id) return gcs[i];
    return NULL;
}
static void pad_used(SDL_GameController *c) {   /* this pad was used: its sticks are read, its type names the buttons */
    if (!c) return;
    gc = c;
    msr_input_device(1, (int)SDL_GameControllerGetType(c));
}
int video_pad_any_held(void) {   /* for a new binding: the button that chose the row comes up first */
    if (gc) for (int b = 0; b < SDL_CONTROLLER_BUTTON_MAX; b++) if (SDL_GameControllerGetButton(gc, (SDL_GameControllerButton)b)) return 1;
    return 0;
}
unsigned video_pad_combo_held(void) {   /* the pad's buttons held now, as combo bits: what a combo's 5 s count ends on */
    unsigned m = 0;
    if (!gc) return 0;
    for (int b = 0; b < SDL_CONTROLLER_BUTTON_MAX; b++) if (SDL_GameControllerGetButton(gc, (SDL_GameControllerButton)b)) m |= input_combo_bit(b);
    const int press = input_trigger_press() * 32767 / 100;
    if (SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > press) m |= input_combo_bit(PAD_TRIGGER_LEFT);
    if (SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > press) m |= input_combo_bit(PAD_TRIGGER_RIGHT);
    return m;
}

/* Quick save and Quick load on gamepad combos (input.c input_combo; Back + LB and Back + RB by default): one goes off in
   play (not while the settings show, nor in the launcher) when the last of its buttons goes down, once until one of them
   comes up. That last button goes no further; the combo's others, which reached the game as they went down, are let
   go there. The Select button (Back, which opens these settings) in a combo with others does its own thing when it
   comes up instead, if no combo went off while it was down: Back + LB saves without opening the settings first. */
static unsigned combo_held, combo_fired;    /* the pad's buttons held (combo bits); the combos gone off (a bit each) */
static SDL_JoystickID combo_pad = -1;
static int combo_trig[2];                   /* the triggers past their press point (as input.c counts them) */
static int select_waiting, select_spent;    /* Select is down, its action left for when it comes up; a combo went off since */
static int combo_event(const SDL_Event *e) {   /* 1: a combo went off, the event goes no further */
    unsigned bit; int down; SDL_JoystickID which;
    if (e->type == SDL_CONTROLLERBUTTONDOWN || e->type == SDL_CONTROLLERBUTTONUP) {
        bit = input_combo_bit(e->cbutton.button); down = e->type == SDL_CONTROLLERBUTTONDOWN; which = e->cbutton.which;
    } else if (e->type == SDL_CONTROLLERAXISMOTION && (e->caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT || e->caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT)) {
        const int t = e->caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT, was = combo_trig[t];
        if (input_trigger_pulled(t, e->caxis.value, &combo_trig[t]) == was) return 0;
        bit = input_combo_bit(t ? PAD_TRIGGER_RIGHT : PAD_TRIGGER_LEFT); down = combo_trig[t]; which = e->caxis.which;
    } else return 0;
    if (which != combo_pad) { combo_pad = which; combo_held = combo_fired = 0; }   /* another pad: its own buttons */
    if (!down) {
        combo_held &= ~bit;
        for (int i = 0; i < COMBO_COUNT; i++) if (input_combo(i) & bit) combo_fired &= ~(1u << i);
        return 0;
    }
    combo_held |= bit;
    if (g_input_none || msr_overlay_visible() || msr_overlay_capturing() || msr_overlay_launcher_mode()) return 0;
    for (int i = 0; i < COMBO_COUNT; i++) {
        const unsigned m = input_combo(i);
        if (!(m & bit) || (combo_held & m) != m || (combo_fired >> i & 1)) continue;
        combo_fired |= 1u << i;
        if (select_waiting) select_spent = 1;
        for (int b = 0; b < SDL_CONTROLLER_BUTTON_MAX; b++) if (m & ~bit & input_combo_bit(b)) input_pad_sdl(b, 0);
        if (m & ~bit & input_combo_bit(PAD_TRIGGER_LEFT)) input_pad_sdl(PAD_TRIGGER_LEFT, 0);
        if (m & ~bit & input_combo_bit(PAD_TRIGGER_RIGHT)) input_pad_sdl(PAD_TRIGGER_RIGHT, 0);
        fprintf(stderr, "native: %s (gamepad combo)\n", i == COMBO_SAVE ? "quick save" : "quick load");
        if (i == COMBO_SAVE) msr_state_quick_save(); else msr_state_quick_load();
        return 1;
    }
    return 0;
}
/* PWR_TEST_PAD=<s>:<buttons>,...: a virtual gamepad (SDL's) that holds those buttons from that run time on, e.g.
   "10:back,10.3:back+lb,10.8:none" (a b x y back guide start l3 r3 lb rb up down left right, lt rt pulled all the way,
   sleft sright sup sdown the left stick pushed all the way): the pad's own events and state, for testing without one
   (the combos, 2026-11-14; the stick, 2026-11-17) */
static double now_s(void);
static void test_pad_frame(void) {
#if SDL_VERSION_ATLEAST(2, 24, 0)
    static int state = -1; static SDL_Joystick *joy_t; static const char *script; static double t0; static unsigned held;
    if (state < 0) {
        state = 0;
        if (!(script = getenv("PWR_TEST_PAD"))) return;
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");   /* (tests run the window in the background) */
        SDL_VirtualJoystickDesc d; SDL_zero(d);
        d.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION; d.type = SDL_JOYSTICK_TYPE_GAMECONTROLLER; d.name = "PWR_TEST_PAD";
        d.naxes = SDL_CONTROLLER_AXIS_MAX; d.nbuttons = SDL_CONTROLLER_BUTTON_MAX;
        d.axis_mask = (1u << SDL_CONTROLLER_AXIS_MAX) - 1; d.button_mask = (1u << SDL_CONTROLLER_BUTTON_MAX) - 1;
        const int idx = SDL_JoystickAttachVirtualEx(&d);
        if (idx < 0 || !(joy_t = SDL_JoystickOpen(idx))) { fprintf(stderr, "native: PWR_TEST_PAD: %s\n", SDL_GetError()); return; }
        t0 = now_s(); state = 1;
    }
    if (state != 1) return;
    static const char *const names[] = { "a", "b", "x", "y", "back", "guide", "start", "l3", "r3", "lb", "rb", "up", "down", "left", "right" };
    static const char *const sticks[] = { "sright", "sleft", "sup", "sdown" };   /* (bits 26-29) */
    unsigned want = held; const double t = now_s() - t0;
    for (const char *p = script; p && *p; ) {   /* the last entry whose time has come */
        char *end; const double at = strtod(p, &end);
        if (end == p || *end != ':' || at > t) break;
        want = 0;
        for (p = end + 1; *p && *p != ','; ) {
            size_t n = strcspn(p, "+,");
            for (int b = 0; b < (int)(sizeof names / sizeof *names); b++) if (strlen(names[b]) == n && !strncmp(p, names[b], n)) want |= 1u << b;
            if (n == 2 && !strncmp(p, "lt", 2)) want |= 1u << 24;
            if (n == 2 && !strncmp(p, "rt", 2)) want |= 1u << 25;
            for (int k = 0; k < 4; k++) if (strlen(sticks[k]) == n && !strncmp(p, sticks[k], n)) want |= 1u << (26 + k);
            p += n; if (*p == '+') p++;
        }
        if (*p == ',') p++;
    }
    if (want == held) return;
    for (int b = 0; b < (int)(sizeof names / sizeof *names); b++) if ((want ^ held) >> b & 1) SDL_JoystickSetVirtualButton(joy_t, b, (want >> b & 1) ? SDL_PRESSED : SDL_RELEASED);
    /* (a trigger at rest is the axis' bottom: SDL reads the virtual pad's full-range axis as 0-32767, so 0 was half pulled
       and a trigger never came up) */
    if ((want ^ held) >> 24 & 1) SDL_JoystickSetVirtualAxis(joy_t, SDL_CONTROLLER_AXIS_TRIGGERLEFT, (want >> 24 & 1) ? 32767 : -32768);
    if ((want ^ held) >> 25 & 1) SDL_JoystickSetVirtualAxis(joy_t, SDL_CONTROLLER_AXIS_TRIGGERRIGHT, (want >> 25 & 1) ? 32767 : -32768);
    if ((want ^ held) >> 26 & 0xF) {   /* the left stick (SDL's y is down) */
        SDL_JoystickSetVirtualAxis(joy_t, SDL_CONTROLLER_AXIS_LEFTX, (Sint16)((want >> 26 & 1 ? 32767 : 0) - (want >> 27 & 1 ? 32767 : 0)));
        SDL_JoystickSetVirtualAxis(joy_t, SDL_CONTROLLER_AXIS_LEFTY, (Sint16)((want >> 29 & 1 ? 32767 : 0) - (want >> 28 & 1 ? 32767 : 0)));
    }
    fprintf(stderr, "native: PWR_TEST_PAD at %.1f s: %#x\n", t, want);
    held = want;
#endif
}
static int select_in_combo(int button) {   /* the Select button is in a combo with others: it acts when it comes up */
    const unsigned bit = input_combo_bit(button);
    for (int i = 0; i < COMBO_COUNT; i++) { const unsigned m = input_combo(i); if ((m & bit) && m != bit) return 1; }
    return 0;
}
/* the Select button's settings or layout action: now (down), or on its way up when it is in a combo */
static int select_acts(int button, int down) {
    if (!select_in_combo(button)) return down;
    if (down) { select_waiting = 1; select_spent = 0; return 0; }
    const int act = select_waiting && !select_spent;
    select_waiting = 0;
    return act;
}

/* A controller, for the log (2026-11-19): its maker and product ids, how it is connected (from the system's device path:
   Windows names Bluetooth's HID service in it, and USB's; XInput says neither), Steam Input's virtual pad (the real one
   behind it), and whether it has a gyro. The path itself is not logged: a Bluetooth one holds the pad's address. */
static void pad_describe(SDL_Joystick *j, int gyro, char *out, size_t n) {
    const unsigned vid = SDL_JoystickGetVendor(j), pid = SDL_JoystickGetProduct(j);
    const char *path = SDL_JoystickPath(j); char p[512] = "";
    if (path) { snprintf(p, sizeof p, "%s", path); for (char *c = p; *c; c++) *c = (char)toupper((unsigned char)*c); }
    const char *how = vid == 0x28DE && pid == 0x11FF ? "Steam Input's virtual pad (the real one is behind Steam)"
                    : strstr(p, "{00001124-") || strstr(p, "BTHENUM") || strstr(p, "_VID&0002") ? "Bluetooth"
                    : strstr(p, "{00001812-") || strstr(p, "BTHLE") ? "Bluetooth LE"
                    : strstr(p, "XINPUT") ? "XInput (USB, Bluetooth or a receiver: Windows does not say)"
                    : strstr(p, "USB") || strstr(p, "HID#VID_") ? "USB or a 2.4 GHz receiver"
                    : "connection unknown";
    snprintf(out, n, "%04x:%04x, %s, %s", vid, pid, how, gyro ? "a gyro" : "no gyro reported");   /* (SDL2: a DualSense on Bluetooth reports one only in its enhanced mode) */
}
/* gamepads: only without --input=none. Controllers with an SDL mapping use the controller API; others fall back to raw joystick
   buttons/axes/hat (0..3 = south,east,west,north as most pads number them, 4/5 = L/R, 6/7 = select/start) */
static void pad_open(int idx) {
    /* PWR_TEST_PAD: its virtual pad alone. It lets controllers be read in the background (test runs), which let a real
       pad plugged into this computer press buttons in the test's window too (2026-11-20). */
    if (getenv("PWR_TEST_PAD")) {
        const char *name = SDL_JoystickNameForIndex(idx);
        if (!name || strcmp(name, "PWR_TEST_PAD")) return;
    }
    if (SDL_IsGameController(idx)) {
        SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(idx);
        if (pad_of(id)) return;
        for (int i = 0; i < MAX_PADS; i++) if (!gcs[i]) {
            if (!(gcs[i] = SDL_GameControllerOpen(idx))) return;
            static const char *const types[] = { "unknown", "Xbox 360", "Xbox One", "PS3", "PS4", "Nintendo Switch Pro", "virtual", "PS5",
                                                 "Amazon Luna", "Google Stadia", "NVIDIA Shield", "Joy-Con (L)", "Joy-Con (R)", "Joy-Con pair" };
            int t = (int)SDL_GameControllerGetType(gcs[i]);
            char about[200]; pad_describe(SDL_GameControllerGetJoystick(gcs[i]), SDL_GameControllerHasSensor(gcs[i], SDL_SENSOR_GYRO), about, sizeof about);
            fprintf(stderr, "pwr: controller opened: %s (%s; %s)\n", SDL_GameControllerName(gcs[i]), t >= 0 && t < (int)(sizeof types / sizeof *types) ? types[t] : "?", about);
            if (!gc) pad_used(gcs[i]);   /* the first one names the buttons until another is used */
            return;
        }
        return;
    }
    if (gc || joy) return;
    if ((joy = SDL_JoystickOpen(idx))) {
        char about[200]; pad_describe(joy, 0, about, sizeof about);
        fprintf(stderr, "pwr: controller opened: %s (joystick, %d buttons, %d axes, %d hats; %s)\n", SDL_JoystickName(joy), SDL_JoystickNumButtons(joy),
                SDL_JoystickNumAxes(joy), SDL_JoystickNumHats(joy), about);
    }
}
static void pad_axes(void) {
    int x, y, rx, ry;
    if (gc) {
        x = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTX); y = -SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTY);
        rx = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTX); ry = -SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTY);
    } else if (joy) {   /* raw pads: axes 0/1 left stick; right stick 3/4 on six-axis pads (2 is a trigger), else 2/3 */
        int na = SDL_JoystickNumAxes(joy), r0 = na >= 6 ? 3 : 2;
        x = na > 0 ? SDL_JoystickGetAxis(joy, 0) : 0; y = na > 1 ? -SDL_JoystickGetAxis(joy, 1) : 0;
        rx = na > r0 ? SDL_JoystickGetAxis(joy, r0) : 0; ry = na > r0 + 1 ? -SDL_JoystickGetAxis(joy, r0 + 1) : 0;
    } else return;
    input_pad_stick(x, y);
    input_pad_rstick(rx, ry);   /* the D-pad when the Aeion abilities are on the right stick (input.c) */
    freelook_stick(rx, ry);
}

/* next screen layout (F9, the gamepad's Back/Select): a label shows the new layout for 1.5 s, and the choice is saved */
static double toast_until; static int toast_dirty; static const char *toast_text;
static double now_s(void);
static void next_layout(void) {
    static const char *nice[L_COUNT] = { "Layout: stacked", "Layout: side by side", "Layout: large top", "Layout: top only", "Layout: bottom only", "Layout: single screen", "Layout: auto" };
    opt_layout = (opt_layout + 1) % L_COUNT;
    toast_text = nice[opt_layout];   /* drawn at the next present, at the UI scale */
    toast_until = now_s() + 1.5; toast_dirty = 1;
    fprintf(stderr, "pwr: layout %s\n", layout_names[opt_layout]);
}

/* hotkeys (never passed to the game): F11 / Alt+Enter fullscreen, F9 next layout. Returns 1 when the key was a hotkey. */
static int hotkey(const SDL_KeyboardEvent *k) {
    if (k->type != SDL_KEYDOWN || k->repeat) return k->keysym.sym == SDLK_F11 || k->keysym.sym == SDLK_F9 || k->keysym.sym == SDLK_F5 || k->keysym.sym == SDLK_F8 || (k->keysym.sym == SDLK_RETURN && (k->keysym.mod & KMOD_ALT));
    if (k->keysym.sym == SDLK_F5) { msr_state_quick_save(); return 1; }   /* the settings' Quick save and Quick load */
    if (k->keysym.sym == SDLK_F8) { msr_state_quick_load(); return 1; }
    if (k->keysym.sym == SDLK_F11 || (k->keysym.sym == SDLK_RETURN && (k->keysym.mod & KMOD_ALT))) {
        opt_fs = opt_fs ? 0 : fs_last; apply_fullscreen(); msr_overlay_fullscreen(opt_fs != 0); return 1;
    }
    if (k->keysym.sym == SDLK_F9) { next_layout(); return 1; }
    return 0;
}

/* CPU path: nearest scale of one screen into its rectangle of the staging buffer */
static void cpu_blit(const uint8_t *src, int sw, Rect r) {
    if (!r.on) return;
    int x0 = (int)r.x, y0 = (int)r.y, w = (int)r.w, h = (int)r.h;
    for (int y = 0; y < h; y++) {
        int dy = y0 + y; if (dy < 0 || dy >= (int)sc_extent.height) continue;
        const uint8_t *srow = src + (size_t)(y * SCR_H / h) * (size_t)sw * 4;
        uint8_t *drow = stage_ptr + (size_t)dy * sc_extent.width * 4;
        for (int x = 0; x < w; x++) {
            int dx = x0 + x; if (dx < 0 || dx >= (int)sc_extent.width) continue;
            const uint8_t *p = srow + (size_t)(x * sw / w) * 4; uint8_t *d = drow + (size_t)dx * 4;
            if (sc_bgra) { d[0] = p[2]; d[1] = p[1]; d[2] = p[0]; } else { d[0] = p[0]; d[1] = p[1]; d[2] = p[2]; }
            d[3] = 255;
        }
    }
}

/* GPU path: draw a texture into a rectangle of the current attachment (W x H) with a pipeline */
/* grade: the player's colours, extra = brightness, contrast, saturation (present.frag) */
static void gpu_quad_view(VkPipeline pl, VkImageView view, uint32_t tw, uint32_t th, VkImageLayout lay, int rot, Rect r, float W, float H, int mode, int alpha, const float extra[4], int grade) {
    if (!r.on || r.w < 1 || r.h < 1) return;
    Push p = { { r.x / W * 2 - 1, r.y / H * 2 - 1, (r.x + r.w) / W * 2 - 1, (r.y + r.h) / H * 2 - 1 },
               { (float)tw, (float)th, r.w / (float)tw, r.h / (float)th }, { mode, alpha, rot, grade }, { 0, 0, 0, 0 } };
    if (extra) memcpy(p.extra, extra, sizeof p.extra);
    VkDescriptorImageInfo ii = { smp_linear, view, lay };
    VkWriteDescriptorSet w = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii };
    vkCmdBindPipeline(cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, pl);
    vk_push(&push_arena, cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, play, 0, 1, &w);
    vkCmdPushConstants(cmdbuf, play, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof p, &p);
    vkCmdDraw(cmdbuf, 4, 1, 0, 0);
}
/* Samus Returns' compositor: one quad of msr_compose's list, sampling the bottom screen */
static void msr_quad(const MsrQuad *q, const Img *bottom, float W, float H) {
    if (q->dst[2] <= 0 || q->dst[3] <= 0) return;
    MsrPush p = { { q->dst[0] / W * 2 - 1, q->dst[1] / H * 2 - 1, (q->dst[0] + q->dst[2]) / W * 2 - 1, (q->dst[1] + q->dst[3]) / H * 2 - 1 },
                  { q->src[0], q->src[1], q->src[2], q->src[3] }, { q->style[0], q->style[1], q->style[2], q->style[3] },
                  { q->feather, q->circle, q->turn, 0 }, { q->color[0], q->color[1], q->color[2], q->color[3] } };
    VkDescriptorImageInfo ii = { smp_linear, bottom->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet w = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii };
    vk_push(&push_arena, cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, play_msr, 0, 1, &w);
    vkCmdPushConstants(cmdbuf, play_msr, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof p, &p);
    vkCmdDraw(cmdbuf, 4, 1, 0, 0);
}
static void gpu_quad(VkPipeline pl, const Img *t, Rect r, float W, float H, int mode, int alpha, const float extra[4]) {
    gpu_quad_view(pl, t->view, t->w, t->h, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, r, W, H, mode, alpha, extra, 0);
}
static void begin_pass_a(const Img *att, VkAttachmentLoadOp load, float clear_alpha) {
    barrier_img(att->img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    begin_pass_view(att->view, att->fmt, (VkExtent2D){ att->w, att->h }, load, clear_alpha);
}
static void begin_pass(const Img *att, VkAttachmentLoadOp load) { begin_pass_a(att, load, 1); }

/* FSR: EASU upscale of a screen into fsr_tmp[i] at the output size (integer rectangle), before the main pass */
static void fsr_upscale(int i, const Img *src, Rect *r) {
    if (!r->on) return;
    r->x = floorf(r->x + 0.5f); r->y = floorf(r->y + 0.5f); r->w = floorf(r->w + 0.5f); r->h = floorf(r->h + 0.5f);
    if (r->w < 1 || r->h < 1) { r->on = 0; return; }
    Img *t = &fsr_tmp[i];
    if (t->w != (uint32_t)r->w || t->h != (uint32_t)r->h) {
        img_free(t);
        if (img_make(t, (uint32_t)r->w, (uint32_t)r->h, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)) { r->on = 0; return; }
    }
    begin_pass(t, VK_ATTACHMENT_LOAD_OP_DONT_CARE);
    float ex[4] = { r->w, r->h, 0, 0 };
    gpu_quad(pipe_easu, src, (Rect){ 0, 0, r->w, r->h, 1 }, r->w, r->h, 0, 0, ex);
    end_pass();
    barrier_img(t->img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
}

/* a screen at the 3D internal resolution: un-rotate pica_vk's scan-out image (framebuffer orientation) into hi_scr[i] */
static const Img *hi_screen(int i, const ScreenSource *s, int screen_w) {
    VkImageView v; uint32_t lw, lh;
    if (!s->valid || (s->format & 7) != 1 || !pica_vk_scanout(s->addr, &v, &lw, &lh) || lw * (uint32_t)screen_w != lh * SCR_H) return NULL;
    Img *t = &hi_scr[i];
    if (t->w != lh || t->h != lw) {
        img_free(t);
        if (img_make(t, lh, lw, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) return NULL;
    }
    begin_pass(t, VK_ATTACHMENT_LOAD_OP_DONT_CARE);
    /* FXAA (#51) and the player's colours (#25) here, on the screen at the internal resolution, before any upscaling */
    float g[4]; const int grade = msr_grade(g);
    gpu_quad_view(pipe_rgba, v, lw, lh, VK_IMAGE_LAYOUT_GENERAL, 1, (Rect){ 0, 0, (float)lh, (float)lw, 1 }, (float)lh, (float)lw, msr_fxaa() ? 4 : 0, 0, grade ? g : NULL, grade);
    end_pass();
    barrier_img(t->img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
    return t;
}

/* Samus Returns: copies of the bottom screen for the compositor's quads that show it as it was (MsrQuad.source), at
   the size of the bottom screen in use; such a quad samples the live screen while its copy has none at that size. */
typedef struct { Img img; int ok; } BottomCopy;
static void bottom_copy(BottomCopy *c, const Img *sb, uint32_t x0) {   /* columns x0 on, every row */
    if (c->img.w != sb->w || c->img.h != sb->h) {
        img_free(&c->img); c->ok = 0;
        if (img_make(&c->img, sb->w, sb->h, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) { memset(&c->img, 0, sizeof c->img); return; }
    }
    barrier_img(sb->img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    barrier_img(c->img.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkImageCopy ic = { .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .srcOffset = { (int32_t)x0, 0, 0 },
                       .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .dstOffset = { (int32_t)x0, 0, 0 }, .extent = { sb->w - x0, sb->h, 1 } };
    vkCmdCopyImage(cmdbuf, sb->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, c->img.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &ic);
    barrier_img(sb->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT);
    barrier_img(c->img.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
    c->ok = 1;
}
static const Img *bottom_kept(const BottomCopy *c, const Img *sb) { return c->ok && c->img.w == sb->w && c->img.h == sb->h ? &c->img : sb; }
/* R held (missiles): the HUD's beam slots from the bottom screen as it was before (msr_beams_held). Each frame R is not
   held, the slots' column (x 250-320) is copied; while it is held, the beam slots' quads (source 1) sample the copy. */
static BottomCopy beam_copy;
static const Img *beam_slots(const Img *sb) {
    if (msr_beams_held()) return bottom_kept(&beam_copy, sb);
    bottom_copy(&beam_copy, sb, sb->w * 250 / BOT_W);
    return sb;
}
/* The menus' panel: the bottom screen as it last read as a menu (msr_menu_live). Each frame it does, all of it is
   copied; while it does not, the panel (source 2) samples the copy as it fades out. */
static BottomCopy menu_copy;
static const Img *menu_panel(const Img *sb) {
    if (!msr_menu_live()) return bottom_kept(&menu_copy, sb);
    bottom_copy(&menu_copy, sb, 0);
    return sb;
}
/* The game's atlases (msr_gui_atlas: its sprites and fonts, the compositor's quads with source 3): each uploaded when it
   is new (once, and again when a texture pack's replacement arrives) from a staging buffer of its own, kept until the
   next one (the frame that used it has finished by then, as for ui_img). Outside the passes. NULL until there is one. */
static Img hud_atlas[MSR_ATLAS_COUNT]; static unsigned hud_atlas_ver[MSR_ATLAS_COUNT];
static VkBuffer hud_stage[MSR_ATLAS_COUNT]; static VkDeviceMemory hud_stage_mem[MSR_ATLAS_COUNT];
static const Img *hud_atlas_img(int a) {
    const uint8_t *px; uint32_t w, h;
    const unsigned ver = msr_gui_atlas(a, &px, &w, &h);
    if (!ver) return NULL;
    if (ver == hud_atlas_ver[a]) return hud_atlas[a].img ? &hud_atlas[a] : NULL;
    hud_atlas_ver[a] = ver;
    img_free(&hud_atlas[a]);
    if (hud_stage[a]) { vkDestroyBuffer(dev, hud_stage[a], NULL); vkFreeMemory(dev, hud_stage_mem[a], NULL); hud_stage[a] = VK_NULL_HANDLE; hud_stage_mem[a] = VK_NULL_HANDLE; }
    const VkDeviceSize n = (VkDeviceSize)w * h * 4;
    VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = n, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT };
    if (vkCreateBuffer(dev, &bi, NULL, &hud_stage[a]) != VK_SUCCESS) { hud_stage[a] = VK_NULL_HANDLE; return NULL; }
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev, hud_stage[a], &mr);
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
        .memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
    void *map = NULL;
    if (ai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(dev, &ai, NULL, &hud_stage_mem[a]) != VK_SUCCESS) {
        vkDestroyBuffer(dev, hud_stage[a], NULL); hud_stage[a] = VK_NULL_HANDLE; hud_stage_mem[a] = VK_NULL_HANDLE; return NULL;
    }
    if (vkBindBufferMemory(dev, hud_stage[a], hud_stage_mem[a], 0) != VK_SUCCESS || vkMapMemory(dev, hud_stage_mem[a], 0, n, 0, &map) != VK_SUCCESS) return NULL;
    memcpy(map, px, (size_t)n);
    vkUnmapMemory(dev, hud_stage_mem[a]);
    if (img_make(&hud_atlas[a], w, h, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) { memset(&hud_atlas[a], 0, sizeof hud_atlas[a]); return NULL; }
    barrier_img(hud_atlas[a].img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkBufferImageCopy cp = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { w, h, 1 } };
    vkCmdCopyBufferToImage(cmdbuf, hud_stage[a], hud_atlas[a].img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
    barrier_img(hud_atlas[a].img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
    return &hud_atlas[a];
}

static void upload(const Img *t, VkDeviceSize off);
/* a UI panel image of exactly w x h (remade when the UI scale changes; the previous frame has finished with it) */
static void ui_img(Img *t, int w, int h) {
    if (t->img && t->w == (uint32_t)w && t->h == (uint32_t)h) return;
    img_free(t);
    if (img_make(t, (uint32_t)w, (uint32_t)h, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) memset(t, 0, sizeof *t);
}
static void upload(const Img *t, VkDeviceSize off) {   /* whole image from up_buf + off */
    barrier_img(t->img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkBufferImageCopy cp = { .bufferOffset = off, .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { t->w, t->h, 1 } };
    vkCmdCopyBufferToImage(cmdbuf, up_buf, t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
    barrier_img(t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
}

static double now_s(void) { return (double)SDL_GetPerformanceCounter() / (double)SDL_GetPerformanceFrequency(); }

/* The mouse in play (keyboard players): the left button shoots (3DS Y), the right one held is Free Aim (L) with the
   circle pad toward the pointer from Samus (msr_samus_view: where the game's camera shows her, then the zoom and the
   16:9 squeeze of msr_world_scale, then the top screen's place in the window). Over the top screen in play the pointer
   is a crosshair; it hides while a gamepad is in use and comes back when the mouse moves. */
static struct { float x, y, ax, ay; int fire, aim; double moved; } mouse = { 0, 0, 1, 0, 0, 0, 0 };
static void mouse_frame(Rect rt) {
    const int play = msr_world_in_play() && !msr_overlay_visible();
    if (!play) mouse.fire = mouse.aim = 0;
    float h, v, sx, sy;
    if (mouse.aim && msr_samus_view(&h, &v)) {
        msr_world_scale(&sx, &sy);   /* clip x is the screen's vertical (the 3DS's top screen is turned), y its horizontal */
        const float px = rt.x + rt.w * .5f * (1.f + h * sy), py = rt.y + rt.h * .5f * (1.f - v * sx);
        const float dx = mouse.x - px, dy = py - mouse.y, d = sqrtf(dx * dx + dy * dy);
        if (d > 3.f) { mouse.ax = dx / d; mouse.ay = dy / d; }   /* on Samus: the last direction */
    }
    input_mouse(mouse.fire, mouse.aim, mouse.ax, mouse.ay);
    static SDL_Cursor *cross, *arrow; static int shown = -1, crossed = -1;
    if (!cross) { cross = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_CROSSHAIR); arrow = SDL_GetDefaultCursor(); }
    const int over = mouse.x >= rt.x && mouse.x < rt.x + rt.w && mouse.y >= rt.y && mouse.y < rt.y + rt.h;
    const int want_cross = play && over, want_shown = !(play && msr_input_pad_active() && now_s() - mouse.moved > 2.0);
    if (cross && want_cross != crossed) { SDL_SetCursor(want_cross ? cross : arrow); crossed = want_cross; }
    if (want_shown != shown) { SDL_ShowCursor(want_shown ? SDL_ENABLE : SDL_DISABLE); shown = want_shown; }
#ifndef __ANDROID__
    /* in play with the mouse (no pad in use), the pointer stays in the window (#33: in a window, a shot beside it took
       the focus away); the settings menu, the game's title and file menus, a pad and leaving the window (Alt+Tab: SDL
       lets go with the focus and holds it again on return) free it */
    static int grabbed = -1;
    const int want_grab = play && !msr_input_pad_active() && (SDL_GetWindowFlags(win) & SDL_WINDOW_INPUT_FOCUS) != 0;
    if (want_grab != grabbed) { SDL_SetWindowGrab(win, want_grab ? SDL_TRUE : SDL_FALSE); grabbed = want_grab; }
#endif
}

#ifdef MSR_OPENGL
/* OpenGL: the frame laid out as the Vulkan path below does it, drawn and presented by video_gl.c */
static int gl_frame(int dw, int dh, const ScreenSource *top, const ScreenSource *bottom) {
    if (dw < 1 || dh < 1 || (SDL_GetWindowFlags(win) & SDL_WINDOW_MINIMIZED)) return 1;
    Rect rt, rb; layout_rects(dw, dh, &rt, &rb);
    int single = effective_layout() == L_SINGLE; MsrFrame mf = { { 0 }, { 0 }, NULL, 0 };
    msr_set_single(single);
    msr_compose(bot_px, now_s(), input_buttons(), (float)dw, (float)dh, &mf);   /* classifies in every layout (the triggers' taps need it) */
    if (single) { rt = (Rect){ mf.top[0], mf.top[1], mf.top[2], mf.top[3], 1 }; rb.on = 0; }
    else mf.count = 0;
    msr_overlay_panel(single ? mf.panel : NULL);   /* the control hints go beside the menus' panel */
    int ks = dh / 480; ks = ks < 1 ? 1 : ks > UI_KMAX ? UI_KMAX : ks;   /* the layout label's scale: by window height */
    VglFrame f = { dw, dh, top_px, bot_px, { rt.x, rt.y, rt.w, rt.h, rt.on }, { rb.x, rb.y, rb.w, rb.h, rb.on }, opt_filter, opt_sharp, &mf,
                   NULL, TOAST_W * ks, TOAST_H * ks, 0 };
    static int toast_w;   /* the label's pixels are made again when it changes or the scale does */
    if (toast_text && (toast_dirty || toast_w != f.toast_w)) {
        fill(toast_px, f.toast_w, 0, 0, f.toast_w, f.toast_h, 0x101820, 210);
        text(toast_px, f.toast_w, f.toast_h, (f.toast_w - 8 * ks * (int)strlen(toast_text)) / 2, 3 * ks, toast_text, 0xffffff, ks);
        f.toast = toast_px; toast_dirty = 0; toast_w = f.toast_w;
    }
    f.toast_show = toast_w && !msr_overlay_visible() && now_s() < toast_until;
    f.scan_top = top->valid && (top->format & 7) == 1 ? top->addr : 0;
    f.scan_bottom = bottom->valid && (bottom->format & 7) == 1 ? bottom->addr : 0;
    vgl_frame(&f);
    g_vid_presented++;
    return 1;
}
#endif

/* PWR_HDR_DUMP=<s>[,<s>...]: at those run times (from the first frame) with HDR on, the HDR pass's two inputs as they
   are, the game's picture and the interface layer (8 bits, gamma-encoded, the layer premultiplied by its coverage), with
   the paper white, peak, knee and desaturation it used: hdr_dump_<n>.txt, _game.rgba and _ui.rgba in the working folder.
   tools/dev/hdr_sim.py works out the HDR picture from them for any settings (2026-11-15: an HDR10 window's screen
   captures look washed out, so the output itself cannot be judged from one) */
static VkBuffer dump_buf; static VkDeviceMemory dump_mem; static uint8_t *dump_ptr; static VkDeviceSize dump_size;
static float dump_paper, dump_peak = -1, dump_knee, dump_desat;   /* dump_peak >= 0: copies recorded this frame, written after it */
static int dump_wanted(void) {   /* 1 when a dump time has come (each once) */
    static const char *list; static int checked, done; static double t0;
    if (!checked) { checked = 1; list = getenv("PWR_HDR_DUMP"); t0 = now_s(); }
    if (!list) return 0;
    const double t = now_s() - t0; const char *p = list;
    for (int i = 0; *p; i++) {
        char *end; const double at = strtod(p, &end);
        if (end == p) break;
        if (i == done && t >= at) { done++; return 1; }
        p = *end == ',' ? end + 1 : end;
    }
    return 0;
}
static int dump_record(void) {   /* the copies, into this frame's command buffer (target and ui_layer: shader-read layout) */
    const VkDeviceSize one = (VkDeviceSize)target.w * target.h * 4;
    if (dump_size < 2 * one) {
        if (dump_buf) { vkUnmapMemory(dev, dump_mem); vkDestroyBuffer(dev, dump_buf, NULL); vkFreeMemory(dev, dump_mem, NULL); dump_buf = VK_NULL_HANDLE; dump_size = 0; }
        VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 2 * one, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT };
        if (vkCreateBuffer(dev, &bi, NULL, &dump_buf) != VK_SUCCESS) return 0;
        VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev, dump_buf, &mr);
        VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
            .memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
        if (vkAllocateMemory(dev, &ai, NULL, &dump_mem) != VK_SUCCESS) { vkDestroyBuffer(dev, dump_buf, NULL); dump_buf = VK_NULL_HANDLE; return 0; }
        vkBindBufferMemory(dev, dump_buf, dump_mem, 0);
        if (vkMapMemory(dev, dump_mem, 0, 2 * one, 0, (void **)&dump_ptr) != VK_SUCCESS) return 0;
        dump_size = 2 * one;
    }
    const Img *im[2] = { &target, &ui_layer };
    for (int i = 0; i < 2; i++) {
        barrier_img(im[i]->img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        VkBufferImageCopy c = { .bufferOffset = one * (VkDeviceSize)i, .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { im[i]->w, im[i]->h, 1 } };
        vkCmdCopyImageToBuffer(cmdbuf, im[i]->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dump_buf, 1, &c);
    }
    return 1;
}
static void dump_write(float paper, float peak, float knee, float desat) {   /* after the frame's fence */
    static int n;
    if (vk_wait_fences(dev, 1, &fence) != VK_SUCCESS) return;
    const size_t one = (size_t)target.w * target.h * 4;
    char fn[64]; const char *part[2] = { "game", "ui" };
    for (int i = 0; i < 2; i++) {
        snprintf(fn, sizeof fn, "hdr_dump_%d_%s.rgba", n, part[i]);
        FILE *f = fopen(fn, "wb"); if (f) { fwrite(dump_ptr + one * (size_t)i, 1, one, f); fclose(f); }
    }
    snprintf(fn, sizeof fn, "hdr_dump_%d.txt", n);
    FILE *f = fopen(fn, "w");
    if (f) { fprintf(f, "width %u\nheight %u\nbgra %d\npaper %.1f\npeak %.1f\nknee %.3f\ndesat %.3f\nmode %d\n", target.w, target.h,
                     target_fmt == VK_FORMAT_B8G8R8A8_UNORM, paper, peak, knee, desat, hdr_mode); fclose(f); }
    fprintf(stderr, "video: HDR inputs written to hdr_dump_%d_*\n", n);
    n++;
}

/* #73: a picture only when there is something new in it: a game frame (a framebuffer swap on either screen, which
   gsp.c takes at the game's VBlank), the menus or the launcher open, a label shown or gone, a new swapchain, or nothing
   presented for 100 ms (a load). The main loop ticks at 60 Hz at 30 FPS (main.c: the game's VBlank every other tick),
   and presenting on every tick showed each game frame twice, 60 presents a second: a frame limiter set to 30 (RivaTuner)
   then slowed the loop to 30 ticks and the game to 15 FPS, and frame generation (Lossless Scaling) took 60 with every
   other frame a copy of the one before. The display shows the same: FIFO holds the last image to the next one, as
   before. Before the game's first swap (the boot logo, from the LCD registers) every tick presents.
   PWR_PRESENT_EVERY_TICK=1: every tick always (A/B). */
static int present_wanted(void) {
    extern unsigned g_gsp_top_swaps, g_gsp_bottom_swaps;
    static int every_tick = -1; if (every_tick < 0) every_tick = getenv("PWR_PRESENT_EVERY_TICK") != NULL;
    static unsigned top, bottom; static double last; static int menus_were, toast_was;
    const unsigned t = __atomic_load_n(&g_gsp_top_swaps, __ATOMIC_ACQUIRE), b = __atomic_load_n(&g_gsp_bottom_swaps, __ATOMIC_ACQUIRE);
    const double now = now_s();
    const int menus = msr_overlay_visible() || msr_overlay_launcher_mode(), toast = toast_text && now < toast_until;
    if (!(every_tick || present_due || menus || menus != menus_were || toast != toast_was || toast_dirty || !t || t != top || b != bottom ||
          now - last >= 0.1)) return 0;
    if (last > 0) {
        const double gap = now - last;
        if (!g_vid_gap_min || gap < g_vid_gap_min) g_vid_gap_min = gap;
        if (gap > g_vid_gap_max) g_vid_gap_max = gap;
    }
    top = t; bottom = b; last = now; menus_were = menus; toast_was = toast; present_due = 0;
    return 1;
}

int video_frame(void) {
    test_pad_frame();
    {   /* PWR_TEST_STALL=<from s>,<every s>,<ms>: the main thread held that long now and then, from that run time on, as a
           controller scan after a Bluetooth reconnect held it (292 ms on 2026-11-19): the game's next frame then measures
           all of it */
        static double from = -2, every, ms, next;
        if (from == -2) { const char *e = getenv("PWR_TEST_STALL"); from = -1; if (e && sscanf(e, "%lf,%lf,%lf", &from, &every, &ms) != 3) from = -1; next = from; }
        if (from >= 0 && now_s() >= next) {
            fprintf(stderr, "test: the main thread held %.0f ms\n", ms);
            k_sleep_ns((int64_t)(ms * 1e6)); next = now_s() + every;
        }
    }
    SDL_Event e; int bottom_click = 0;
    int ww, wh; SDL_GetWindowSize(win, &ww, &wh);
    const int eye_ww = stereo_mode == 2 ? ww / 2 : ww;   /* stereo at full width: one eye's window is half of it */
    const int eye_wh = stereo_window_h(wh);              /* ...in a band, half its height */
    Rect rt, rb; layout_rects(eye_ww, eye_wh, &rt, &rb);   /* window coordinates for touch; the drawing below uses drawable pixels */
    int dw0, dh0; drawable_size(&dw0, &dh0);
    float msx = ww > 0 ? (float)dw0 / (float)ww : 1.f, msy = wh > 0 ? (float)dh0 / (float)wh : 1.f;   /* the overlay's mouse: drawable pixels */
    /* (the stages: main.c's long-frame lines) */
#ifdef SDL_HINT_AUTO_UPDATE_JOYSTICKS
    stage_set(STAGE_MAIN, "updating the controllers (SDL_JoystickUpdate: their state, a scan after a device change)");
    SDL_JoystickUpdate();   /* (not by SDL_PollEvent: video_init) */
#endif
    while (stage_set(STAGE_MAIN, "reading the window's messages (SDL_PollEvent)"), SDL_PollEvent(&e)) {
        stage_set(STAGE_MAIN, "handling a window or controller event");
        if (e.type == SDL_QUIT) return 0;
#ifdef __ANDROID__
        if (e.type == SDL_APP_DIDENTERFOREGROUND) { surface_reset = 1; continue; }   /* a new window surface */
        if ((e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) && e.key.keysym.sym == SDLK_AC_BACK) {
            /* Back: the menu, as F1; in the launcher, out of what is open, as Escape */
            const int launcher = msr_overlay_launcher_mode();
            e.key.keysym.sym = launcher ? SDLK_ESCAPE : SDLK_F1;
            e.key.keysym.scancode = launcher ? SDL_SCANCODE_ESCAPE : SDL_SCANCODE_F1;
        }
#endif
        if (combo_event(&e)) continue;   /* Quick save or Quick load on a gamepad combo: its last button goes no further */
        /* the settings overlay: F1 and Select (its binding) open and close it; while it shows, or waits for a key or button to
           bind, it takes the input */
        if (!msr_overlay_capturing()) {
            int key_toggle = (e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) && (e.key.keysym.sym == SDLK_F1 ||
                             (opt_select_layout == 2 && input_key_control(e.key.keysym.sym) == C_SELECT));
            int pad_toggle = (e.type == SDL_CONTROLLERBUTTONDOWN || e.type == SDL_CONTROLLERBUTTONUP) && !g_input_none &&
                             opt_select_layout == 2 && input_pad_control_now(e.cbutton.button) == C_SELECT;
            if (key_toggle || pad_toggle) {
                if (pad_toggle ? select_acts(e.cbutton.button, e.type == SDL_CONTROLLERBUTTONDOWN) : e.type == SDL_KEYDOWN && !e.key.repeat)
                    msr_overlay_toggle();
                continue;
            }
        }
        if (e.type == SDL_CONTROLLERBUTTONDOWN && !g_input_none) pad_used(pad_of(e.cbutton.which));   /* the settings name its buttons too */
        if (e.type == SDL_MOUSEMOTION) { e.motion.x = stereo_window_x(e.motion.x, ww); e.motion.y = stereo_window_y(e.motion.y, wh); }   /* stereo: in one eye's window */
        else if (e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEBUTTONUP) { e.button.x = stereo_window_x(e.button.x, ww); e.button.y = stereo_window_y(e.button.y, wh); }
        if (msr_overlay_event(&e, msx, msy)) continue;
        switch (e.type) {
        case SDL_QUIT: return 0;
        case SDL_CONTROLLERDEVICEADDED: case SDL_JOYDEVICEADDED: if (!g_input_none) pad_open(e.cdevice.which); break;
        case SDL_CONTROLLERDEVICEREMOVED: {
            SDL_GameController *c = pad_of(e.cdevice.which);
            if (!c) break;
            fprintf(stderr, "pwr: controller removed: %s\n", SDL_GameControllerName(c) ? SDL_GameControllerName(c) : "?");
            for (int i = 0; i < MAX_PADS; i++) if (gcs[i] == c) gcs[i] = NULL;
            SDL_GameControllerClose(c);
            if (gc == c) {
                gc = NULL; input_pad_stick(0, 0); input_pad_release();
                for (int i = 0; i < MAX_PADS; i++) if (gcs[i]) { pad_used(gcs[i]); break; }
                if (!gc) msr_input_device(0, 0);
            }
            break;
        }
        case SDL_JOYDEVICEREMOVED: if (joy && e.jdevice.which == SDL_JoystickInstanceID(joy)) { fprintf(stderr, "pwr: controller removed: %s\n", SDL_JoystickName(joy) ? SDL_JoystickName(joy) : "?"); SDL_JoystickClose(joy); joy = NULL; input_pad_stick(0, 0); input_pad_release(); } break;
        case SDL_CONTROLLERBUTTONDOWN: case SDL_CONTROLLERBUTTONUP: if (g_input_none) break;
            {   /* Samus Returns' touch navigation (T on the keyboard; L3 in menus until 2026-11-17, the user: "Remove the L3
                   stick pointer control. It's no longer needed": the D-pad reaches the touch screen's buttons); while on,
                   the D-pad moves, the menus' choosing button is the finger and their back button leaves (msr_pad_choose,
                   msr_pad_back: the bottom and right ones, a Nintendo pad's A and B; with the Nintendo menu layout, those
                   bound to the game's A and B) */
                int c = e.cbutton.button, down = e.type == SDL_CONTROLLERBUTTONDOWN, choose = msr_pad_choose(), back = msr_pad_back();
                if (msr_touch_active()) {
                    if (down && c == SDL_CONTROLLER_BUTTON_DPAD_UP) msr_touch_step(0, -1);
                    else if (down && c == SDL_CONTROLLER_BUTTON_DPAD_DOWN) msr_touch_step(0, 1);
                    else if (down && c == SDL_CONTROLLER_BUTTON_DPAD_LEFT) msr_touch_step(-1, 0);
                    else if (down && c == SDL_CONTROLLER_BUTTON_DPAD_RIGHT) msr_touch_step(1, 0);
                    else if (c == choose) msr_touch_finger(down);
                    else if (down && c == back) msr_touch_close();
                    input_pad_sdl(c, down);   /* the pad's state stays true; the game sees none of it while blocked */
                    break;
                }
            }
            if (opt_select_layout == 1 && input_pad_control_now(e.cbutton.button) == C_SELECT) {
                if (select_acts(e.cbutton.button, e.type == SDL_CONTROLLERBUTTONDOWN)) next_layout(); break; }
            {   /* ZL and ZR bound to a button: the triggers' taps */
                int c = input_pad_control_now(e.cbutton.button);
                if (c == C_ZL || c == C_ZR) { msr_trigger_hold(c == C_ZR, e.type == SDL_CONTROLLERBUTTONDOWN); if (e.type == SDL_CONTROLLERBUTTONDOWN) msr_trigger(c == C_ZR); break; }
            }
            input_pad_sdl(e.cbutton.button, e.type == SDL_CONTROLLERBUTTONDOWN); break;
        case SDL_CONTROLLERAXISMOTION: if (!g_input_none) {
            {   /* a stick pushed past half way or a trigger pulled: this pad is in use */
                SDL_GameController *c = pad_of(e.caxis.which);
                if (c && c != gc && (e.caxis.value > 16000 || e.caxis.value < -16000)) pad_used(c);
                if (c && c != gc) break;   /* another pad's resting sticks are not read */
            }
            /* the triggers as buttons (#35), past the Controls tab's press point: through the bindings as the others. ZL
               and ZR (LT and RT by default, or any button) are the Morph Ball and the next beam, taps on the HUD
               (msr_trigger) on the press; bound to another control, a trigger is held as that button */
            static int trig[2];
            if (e.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT || e.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT) {
                const int t = e.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT, was = trig[t];
                const int on = input_trigger_pulled(t, e.caxis.value, &trig[t]);
                if (on != was) {
                    const int button = t ? PAD_TRIGGER_RIGHT : PAD_TRIGGER_LEFT, c = input_pad_control_now(button);
                    if (c == C_ZL || c == C_ZR) { msr_trigger_hold(c == C_ZR, on); if (on) msr_trigger(c == C_ZR); }
                    else input_pad_sdl(button, on);
                }
            }
            else pad_axes(); } break;
        case SDL_JOYBUTTONDOWN: case SDL_JOYBUTTONUP: if (!joy || g_input_none || e.jbutton.button >= 8) break;
            if (opt_select_layout && e.jbutton.button == 6) { if (e.type == SDL_JOYBUTTONDOWN) { if (opt_select_layout == 2) msr_overlay_toggle(); else if (!msr_overlay_visible()) next_layout(); } break; }   /* raw pads: button 6 = Back/Select */
            if (!msr_overlay_visible()) input_pad_button(e.jbutton.button, e.type == SDL_JOYBUTTONDOWN); break;
        case SDL_JOYAXISMOTION: if (joy && !g_input_none) pad_axes(); break;
        case SDL_JOYHATMOTION: if (joy && !g_input_none) { input_pad_button(8, e.jhat.value & SDL_HAT_UP); input_pad_button(9, e.jhat.value & SDL_HAT_DOWN);
                                                           input_pad_button(10, e.jhat.value & SDL_HAT_LEFT); input_pad_button(11, e.jhat.value & SDL_HAT_RIGHT); } break;
        case SDL_KEYDOWN: case SDL_KEYUP:
            if (e.type == SDL_KEYDOWN && !e.key.repeat) msr_input_device(0, 0);   /* the keyboard names the buttons now */
            if (hotkey(&e.key)) break;
            {   /* Samus Returns' touch navigation: T toggles it (Tab is Start); while on, the circle pad's keys or the arrows
                   move, A's key or Enter is the finger, B's key or Escape leaves (the pad's layout: A on the right, B below) */
                SDL_Keycode c = e.key.keysym.sym; int down = e.type == SDL_KEYDOWN, ctl = input_key_control(c);
                if (c == SDLK_t && ctl < 0) { if (down && !e.key.repeat) msr_touch_toggle(); break; }
                if (msr_touch_active()) {
                    if (down && (c == SDLK_UP || ctl == C_CUP)) msr_touch_step(0, -1);
                    else if (down && (c == SDLK_DOWN || ctl == C_CDOWN)) msr_touch_step(0, 1);
                    else if (down && (c == SDLK_LEFT || ctl == C_CLEFT)) msr_touch_step(-1, 0);
                    else if (down && (c == SDLK_RIGHT || ctl == C_CRIGHT)) msr_touch_step(1, 0);
                    else if ((c == SDLK_RETURN || c == SDLK_KP_ENTER || ctl == C_A) && !e.key.repeat) msr_touch_finger(down);
                    else if (down && (c == SDLK_ESCAPE || ctl == C_B)) msr_touch_close();
                    if (!down) input_key(c, 0);   /* keys held when it opened still come up */
                    break;
                }
            }
            if (e.key.repeat) break;
            {   /* ZL and ZR (Left Shift and C): the triggers' Morph Ball and next beam */
                int c = input_key_control(e.key.keysym.sym);
                if (c == C_ZL || c == C_ZR) { msr_trigger_hold(c == C_ZR, e.type == SDL_KEYDOWN); if (e.type == SDL_KEYDOWN) msr_trigger(c == C_ZR); break; }
            }
            input_key(e.key.keysym.sym, e.type == SDL_KEYDOWN); break;
        case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP: case SDL_MOUSEMOTION: {
            if (e.type == SDL_MOUSEBUTTONDOWN) msr_input_device(0, 0);
            int down = e.type == SDL_MOUSEMOTION ? (e.motion.state & SDL_BUTTON_LMASK) != 0 : e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT;
            int mx = e.type == SDL_MOUSEMOTION ? e.motion.x : e.button.x, my = e.type == SDL_MOUSEMOTION ? e.motion.y : e.button.y;
            int inside = rb.on && rb.w >= 1 && rb.h >= 1 && mx >= rb.x && mx < rb.x + rb.w && my >= rb.y && my < rb.y + rb.h;
            mouse.x = (float)mx; mouse.y = (float)my;
            if (e.type == SDL_MOUSEMOTION) mouse.moved = now_s();
            if (e.type != SDL_MOUSEMOTION) {   /* in play: the left button shoots (not on the bottom screen), the right one aims */
                int dn = e.type == SDL_MOUSEBUTTONDOWN, play = msr_world_in_play();
                if (e.button.button == SDL_BUTTON_RIGHT) { mouse.aim = dn && play; break; }
                if (e.button.button == SDL_BUTTON_LEFT && (mouse.fire || (dn && play && !inside))) { mouse.fire = dn; break; }
            }
            if (e.type == SDL_MOUSEMOTION && (!down || mouse.fire)) break;
            if (!rb.on || rb.w < 1 || rb.h < 1) { input_touch(0, -1, -1); break; }
            if (e.type == SDL_MOUSEBUTTONDOWN && inside) bottom_click = 1;
            int fx, fy;   /* the pause screen fitted to the window: its pieces where they are drawn (msr/compositor.cpp) */
            if (effective_layout() == L_SINGLE && msr_panel_touch((float)mx, (float)my, (float)eye_ww, (float)eye_wh, &fx, &fy)) input_touch(down, fx, fy);
            else input_touch(down, (int)((mx - rb.x) * BOT_W / rb.w), (int)((my - rb.y) * SCR_H / rb.h));
            break; }
        case SDL_MOUSEWHEEL: {   /* the wheel changes beams (down: the next, up: the one before), a notch at a time: each
                                    is a tap on the HUD's beam slots (msr_trigger), which needs ~200 ms to land */
            static float acc; static double last;
            msr_input_device(0, 0);
            float dy = e.wheel.preciseY;
            if (e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) dy = -dy;
            acc += dy;
            if (fabsf(acc) < 1.f) break;
            const double t = now_s();
            if (t - last >= 0.2) { msr_trigger(acc < 0 ? 1 : 2); last = t; }
            acc = 0;
            break; }
        }
    }
    stage_set(STAGE_MAIN, "reading the screens (the bottom screen's read-back)");
    if (opt_layout == L_AUTO) auto_update(now_s(), bottom_click);
    mouse_frame(rt);
    {   /* touch navigation: the left stick steps or glides; the game's input comes back once nothing is held */
        float sx = 0, sy = 0;
        if (gc) { sx = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTX) / 32767.f; sy = -SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTY) / 32767.f; }
        if (fabsf(sx) < .25f) sx = 0; if (fabsf(sy) < .25f) sy = 0;
        msr_touch_frame(sx, sy, input_buttons() != 0 || input_stick_active());
    }

    ScreenSource top, bottom;
    gsp_screens(&top, &bottom); decode_screen(&top, TOP_W, top_px);
    /* the bottom screen: from its scan-out copy's read-back when there is one (pica_vk.c, pica_gl.c: its guest copy is not
       made then) */
    {
        int read_back = 0;
        if (bottom.valid && (bottom.format & 7) == 1) {
#ifdef MSR_OPENGL
            if (use_gl) { extern int pica_gl_bottom_pixels(uint32_t addr, uint8_t *out); read_back = pica_gl_bottom_pixels(bottom.addr, bot_px); }
            else
#endif
            read_back = gpu_ok && vk_hw && pica_vk_bottom_pixels(bottom.addr, bot_px);
        }
        if (!read_back) decode_screen(&bottom, BOT_W, bot_px);
    }
    {   /* PWR_SEQ=<first presented frame>,<count>[,<every>]: both screens as decoded, one PPM pair per presented frame
           (or every <every>th of them) */
        static long first = -2, count, every = 1; static long n;
        if (first == -2) {
            const char *e = getenv("PWR_SEQ"); first = -1;
            if (e) { first = atol(e); const char *c = strchr(e, ','); count = c ? atol(c + 1) : 30; const char *s = c ? strchr(c + 1, ',') : NULL; if (s && atol(s + 1) > 1) every = atol(s + 1); }
        }
        if (first >= 0 && n >= first && n < first + count && (n - first) % every == 0) {
            { extern void k_ev(int, uint32_t, uint32_t); k_ev(0x30, (uint32_t)(n - first), bottom.addr); }
            char fn[64]; snprintf(fn, sizeof fn, "seq_%04ld_b_%08x.ppm", n - first, bottom.addr); FILE *f = fopen(fn, "wb");
            if (f) { fprintf(f, "P6 %d %d 255\n", BOT_W, SCR_H); for (int i = 0; i < BOT_W * SCR_H; i++) fwrite(bot_px + 4 * i, 1, 3, f); fclose(f); }
            snprintf(fn, sizeof fn, "seq_%04ld_t_%08x.ppm", n - first, top.addr); f = fopen(fn, "wb");
            if (f) { fprintf(f, "P6 %d %d 255\n", TOP_W, SCR_H); for (int i = 0; i < TOP_W * SCR_H; i++) fwrite(top_px + 4 * i, 1, 3, f); fclose(f); }
        }
        n++;
    }
    static int test = -1; if (test < 0) test = getenv("PWR_TESTPATTERN") != NULL;
    if (test) for (int y = 0; y < SCR_H; y++) for (int x = 0; x < 50; x++) {   /* colour bars down the left edge of both screens */
        int band = y * 6 / SCR_H; uint8_t *d = top_px + ((size_t)y * TOP_W + (size_t)x) * 4, *d2 = bot_px + ((size_t)y * BOT_W + (size_t)x) * 4;
        d[0] = d2[0] = (band & 1) ? 255 : 0; d[1] = d2[1] = (band & 2) ? 255 : 0; d[2] = d2[2] = (band & 4) ? 255 : 0;
    }

    /* window resized or swapchain lost: rebuild before drawing */
    static int last_dw = -1, last_dh = -1; int dw, dh; drawable_size(&dw, &dh);
    if (last_dw >= 0 && (dw != last_dw || dh != last_dh)) { if (!use_gl && recreate_swapchain()) return 0; if (opt_res_match) match_resolution(dw, dh); }
    last_dw = dw; last_dh = dh;
    stage_set(STAGE_MAIN, "the settings overlay's actions");
    msr_overlay_frame();   /* the settings overlay's actions: before an image is acquired, as V-Sync rebuilds the swapchain */
    {   /* save states: "Saved", "Loaded"... where the layout label goes */
        const char *m = msr_state_message();
        if (m) { static char shown[64]; snprintf(shown, sizeof shown, "%s", m); toast_text = shown; toast_until = now_s() + 2.0; toast_dirty = 1; }
    }
    if (!present_wanted()) return 1;   /* nothing new to show (#73) */
#ifdef MSR_OPENGL
    if (use_gl) {
        static int told; float d; int s;
        if (!told && msr_stereo(&d, &s)) { told = 1; fprintf(stderr, "video: stereoscopic 3D chosen, not shown: it needs Vulkan\n"); }
        return gl_frame(dw, dh, &top, &bottom);
    }
#endif
    {   /* HDR: the display's state when the settings open (Windows' "Use HDR" may have changed), the player's setting */
        static int was_visible; int vis = msr_overlay_visible();
        if (vis && !was_visible) { stage_set(STAGE_MAIN, "checking for HDR (DXGI, the display configuration)"); hdr_detect(); }
        was_visible = vis;
        if (swap && gpu_ok && hdr_wanted() != (hdr_mode != 0)) { if (recreate_swapchain()) return 0; }
    }
    {   /* stereoscopic 3D turned on, off or to the other width: the eyes' images again (create_swapchain); its depth and
           eyes in the log once they have held for a second (not each step of the menu's slider) */
        if (swap && gpu_ok && stereo_wanted() != stereo_mode) { if (recreate_swapchain()) return 0; }
        static float told_depth = -1, seen_depth = -1; static int told_swap = -1, seen_swap = -1; static double seen_at;
        float d; int s;
        if (stereo_mode && msr_stereo(&d, &s)) {
            if (d != seen_depth || s != seen_swap) { seen_depth = d; seen_swap = s; seen_at = now_s(); }
            else if ((d != told_depth || s != told_swap) && now_s() - seen_at >= 1.0) {
                fprintf(stderr, "video: stereoscopic 3D depth %d%% (the 3DS's 3D slider), eyes %s\n", (int)lroundf(d * 100.f), s ? "swapped" : "as they are");
                told_depth = d; told_swap = s;
            }
        }
    }
    /* minimised, the swapchain waits at size 0 (create_swapchain). SDL keeps the window's size through a minimise, so
       the size check above never sees the restore: try again once the window is back, or the picture froze for good */
    if (swap && (!sc_extent.width || !sc_extent.height) && !(SDL_GetWindowFlags(win) & SDL_WINDOW_MINIMIZED)) { if (recreate_swapchain()) return 0; }
    if (surface_reset) { surface_reset = 0; if (recreate_surface()) return 0; }
    {   /* moved to another display, or its refresh rate changed: the present mode may too (present_mode), checked each second */
        static double checked; const double t = now_s();
        if (swap && t - checked > 1.0) {
            checked = t; stage_set(STAGE_MAIN, "checking the display's refresh rate and present modes");
            if (present_mode() != sc_present_mode && recreate_swapchain()) return 0;
        }
    }
    if (!swap || !sc_extent.width || !sc_extent.height) return 1;

    stage_set(STAGE_MAIN, "drawing the frame (the screens, the HUD, the menus)");
    double w0 = wait_clock();
    const char *stage_was = stage_set(STAGE_MAIN, "waiting for the GPU to finish the last frame shown");
    /* the last frame's fence, waited for in earnest (vk_wait.h): reset while still pending, the sets and the command
       buffer below would be reused under the GPU; the device lost: no picture from now on (the log says so) */
    if (vk_wait_fences(dev, 1, &fence) != VK_SUCCESS) { stage_set(STAGE_MAIN, stage_was); return 1; }
    double w1 = wait_clock(); g_vid_wait_fence += w1 - w0;
    vk_push_reset(&push_arena);   /* the last frame's sets are done with */
    uint32_t idx;
    stage_set(STAGE_MAIN, "waiting for the next swapchain image");
    VkResult r = vkAcquireNextImageKHR(dev, swap, UINT64_MAX, sem_acquire, VK_NULL_HANDLE, &idx);
#ifdef __APPLE__
    if (export_metal && (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR)) {   /* MoltenVK: the drawable now, outside the queue (export_metal) */
        stage_set(STAGE_MAIN, "waiting for the window's next drawable (CAMetalLayer)");
        VkExportMetalTextureInfoEXT ti = { .sType = VK_STRUCTURE_TYPE_EXPORT_METAL_TEXTURE_INFO_EXT, .image = sc_images[idx], .plane = VK_IMAGE_ASPECT_PLANE_0_BIT };
        VkExportMetalObjectsInfoEXT oi = { .sType = VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT, .pNext = &ti };
        export_metal(dev, &oi);
    }
#endif
    stage_set(STAGE_MAIN, stage_was);
    g_vid_wait_acquire += wait_clock() - w1;
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) { g_vid_acquire_fail++; g_vid_last_err = r; }
    if (r == VK_ERROR_OUT_OF_DATE_KHR) return recreate_swapchain() ? 0 : 1;
    if (r == VK_ERROR_SURFACE_LOST_KHR) { surface_reset = 1; return 1; }
    /* any other failure (a timeout, Mali's interrupted wait, the device lost): no image, so nothing to draw into, and
       sem_acquire will never signal; submitting a wait on it stalled the queue for good. Again next frame (as Azahar,
       vk_swapchain.cpp, which never submits after a failed acquire). The fence stays signalled. */
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) return 1;
    vkResetFences(dev, 1, &fence);
    layout_rects((int)eye_w(), (int)eye_h(), &rt, &rb);   /* drawable pixels (differs from window size on HiDPI); stereo: one eye's window */
    /* Samus Returns' single screen: the compositor places the view (the gallery moves it aside) and lists the HUD
       pieces, menus and prompts drawn over it; the bottom screen is never drawn as a screen */
    int single = effective_layout() == L_SINGLE; MsrFrame mf = { { 0 }, { 0 }, NULL, 0 };
    msr_set_single(single);
    msr_compose(bot_px, now_s(), input_buttons(), (float)eye_w(), (float)eye_h(), &mf);   /* classifies in every layout (the triggers' taps need it) */
    if (single) { rt = (Rect){ mf.top[0], mf.top[1], mf.top[2], mf.top[3], 1 }; rb.on = 0; }
    else mf.count = 0;
    msr_overlay_panel(single ? mf.panel : NULL);   /* the control hints go beside the menus' panel */

    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    vkResetCommandBuffer(cmdbuf, 0); vkBeginCommandBuffer(cmdbuf, &bi);
    static uint32_t frame_mark; frame_mark++;
    vk_mark(cmdbuf, VKM_FRAME, frame_mark);   /* (vk_diag.h: where the GPU was, if it is lost) */
    if (gpu_ok && vk_hw) pica_vk_scan_lock(1);   /* until submitted: a 3D resolution change must not free a scan-out image in use */
    if (gpu_ok) {
        float W = (float)eye_w(), H = (float)eye_h();   /* (stereo: one eye's window) */
        memcpy(up_ptr, top_px, sizeof top_px); memcpy(up_ptr + sizeof top_px, bot_px, sizeof bot_px);
        upload(&tex_top, 0); upload(&tex_bot, sizeof top_px);
        /* the layout label's scale: by window height */
        int ks = (int)(H / 480.0f); ks = ks < 1 ? 1 : ks > UI_KMAX ? UI_KMAX : ks;
        /* screens at the 3D internal resolution when pica_vk kept them (its scan-out images); the scan-out copies were submitted earlier */
        VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT, .dstAccessMask = VK_ACCESS_SHADER_READ_BIT };
        vkCmdPipelineBarrier(cmdbuf, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
        const Img *st = hi_screen(0, &top, TOP_W), *sb = hi_screen(1, &bottom, BOT_W);
        /* stereo: the right eye's top screen (pica_vk's scan-out of the game's right framebuffer); without one, the left's */
        const Img *sr = NULL;
        if (stereo_mode && top.right) { ScreenSource r = top; r.addr = top.right; sr = hi_screen(2, &r, TOP_W); }
        if (!st) st = &tex_top; if (!sb) sb = &tex_bot;
        if (!sr) sr = st;
        const Img *sbeams = single ? beam_slots(sb) : sb;   /* R held: the beam slots before it (outside the passes) */
        const Img *smenu = single ? menu_panel(sb) : sb;    /* the menu as last seen */
        const Img *satlas[MSR_ATLAS_COUNT];   /* the game's atlases (its sprites and fonts) */
        for (int a = 0; a < MSR_ATLAS_COUNT; a++) satlas[a] = single ? hud_atlas_img(a) : NULL;
        if (toast_text && (toast_dirty || tex_toast.w != (uint32_t)(TOAST_W * ks))) {
            int w = TOAST_W * ks, h = TOAST_H * ks; ui_img(&tex_toast, w, h);
            fill(toast_px, w, 0, 0, w, h, 0x101820, 210);
            text(toast_px, w, h, (w - 8 * ks * (int)strlen(toast_text)) / 2, 3 * ks, toast_text, 0xffffff, ks);
            memcpy(up_ptr + UP_TOAST, toast_px, (size_t)w * h * 4); upload(&tex_toast, UP_TOAST); toast_dirty = 0;
        }
        const int fsr = opt_filter == F_FSR, layered = hdr_mode || stereo_mode, eyes = stereo_mode ? 2 : 1;
        Rect rt_r = rt;   /* (FSR rounds each to whole pixels) */
        if (fsr) {
            vk_mark(cmdbuf, VKM_FSR, frame_mark);
            fsr_upscale(0, st, &rt); fsr_upscale(1, sb, &rb);
            if (stereo_mode) fsr_upscale(2, sr, &rt_r);
        }
        /* each eye's screens into its picture (without stereo, target alone); the bottom screen's is both eyes' */
        for (int eye = 0; eye < eyes; eye++) {
            const Img *s_top = eye ? sr : st; const Rect r_top = eye ? rt_r : rt;
            Img *dst = eye ? &target_r : &target;
            vk_mark(cmdbuf, VKM_SCREENS, frame_mark);
            begin_pass(dst, VK_ATTACHMENT_LOAD_OP_CLEAR);
            if (fsr) {
                float et[4] = { opt_sharp, 0, r_top.x, r_top.y }, eb[4] = { opt_sharp, 0, rb.x, rb.y };
                if (r_top.on) gpu_quad(pipe_rcas, &fsr_tmp[eye ? 2 : 0], r_top, W, H, 0, 0, et);
                if (rb.on) gpu_quad(pipe_rcas, &fsr_tmp[1], rb, W, H, 0, 0, eb);
            } else {
                int f = opt_filter == F_NEAREST ? 0 : opt_filter == F_SHARP ? 2 : opt_filter == F_FXAA ? 4 : 1;
                /* a screen decoded on the CPU (no copy at the internal resolution: hi_screen) gets FXAA and the colours here */
                float g[4]; const int grade = msr_grade(g), fx = msr_fxaa();
                const int ft = s_top == &tex_top && fx ? 4 : f, fb = sb == &tex_bot && fx ? 4 : f;
                gpu_quad_view(pipe, s_top->view, s_top->w, s_top->h, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, r_top, W, H, ft, 0, g, grade && s_top == &tex_top);
                gpu_quad_view(pipe, sb->view, sb->w, sb->h, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, rb, W, H, fb, 0, g, grade && sb == &tex_bot);
            }
            if (layered) {   /* HDR, stereo: the interface goes into its own layer (HDR: at paper white in hdr.frag) */
                end_pass();
                barrier_img(dst->img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
            }
        }
        if (layered) begin_pass_a(&ui_layer, VK_ATTACHMENT_LOAD_OP_CLEAR, 0);
        if (mf.count) {
            vk_mark(cmdbuf, VKM_HUD, frame_mark);
            vkCmdBindPipeline(cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, layered ? pipe_msr_ui : pipe_msr);
            for (int i = 0; i < mf.count; i++) {
                const int source = mf.quads[i].source;
                const int a = mf.quads[i].atlas;
                const Img *atlas = source == 3 && a >= 0 && a < MSR_ATLAS_COUNT ? satlas[a] : NULL;
                if (source == 3 && !atlas) continue;
                msr_quad(&mf.quads[i], source == 1 ? sbeams : source == 2 ? smenu : source == 3 ? atlas : sb, W, H);
            }
        }
        if (!msr_overlay_visible() && now_s() < toast_until && tex_toast.img)   /* layout label, top centre (pixel for pixel) */
            gpu_quad(layered ? pipe_ui : pipe, &tex_toast, (Rect){ floorf((W - (float)tex_toast.w) / 2), 12, (float)tex_toast.w, (float)tex_toast.h, 1 }, W, H, 0, 1, NULL);
        vk_mark(cmdbuf, VKM_MENU, frame_mark);
        msr_overlay_draw(cmdbuf, eye_w(), eye_h());   /* the settings, the FPS counter, the hints: last */
        end_pass();
        if (layered) {   /* HDR, stereo: each eye's picture with the interface over it, into the swapchain image (stereo: its half) */
            barrier_img(ui_layer.img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
            barrier_img(sc_images[idx], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
            vk_mark(cmdbuf, VKM_HDR, frame_mark);
            begin_pass_view(sc_views[idx], sc_format, sc_extent, eye_band() ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_DONT_CARE, 1);   /* (a band: black around it) */
            float pk, pp; msr_hdr(&pk, &pp); if (pk <= 0) pk = disp_peak; if (pp <= 0) pp = disp_paper;
            /* the knee (the SDR luminance where the highlights' expansion starts) and how far it moves them toward white:
               hdr.frag expand(); PWR_HDR_KNEE, PWR_HDR_DESAT for comparisons */
            static float knee = -1, desat;
            if (knee < 0) { const char *k = getenv("PWR_HDR_KNEE"), *d = getenv("PWR_HDR_DESAT"); knee = k ? (float)atof(k) : 0.75f; desat = d ? (float)atof(d) : 0.5f; }
            const float extra[4] = { pp, pk > pp ? pk : pp, knee, desat };
            /* SDR (stereo without HDR): hdr.frag's mode 0, which an sRGB swapchain needs told (it encodes what is written) */
            const int srgb = !hdr_mode && (sc_format == VK_FORMAT_B8G8R8A8_SRGB || sc_format == VK_FORMAT_R8G8B8A8_SRGB);
            float depth; int swap_eyes = 0;
            if (stereo_mode) msr_stereo(&depth, &swap_eyes);
            vkCmdBindPipeline(cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe_hdr);
            for (int eye = 0; eye < eyes; eye++) {
                const float x0 = !stereo_mode ? -1.f : (eye ^ (swap_eyes != 0)) ? 0.f : -1.f, x1 = !stereo_mode ? 1.f : x0 + 1.f;
                const float y1 = (float)eye_h() / (float)sc_extent.height;   /* (a band: the middle half) */
                Push hp = { { x0, -y1, x1, y1 }, { W, H, 1, 1 }, { hdr_mode, srgb, 0, 0 }, { extra[0], extra[1], extra[2], extra[3] } };
                VkDescriptorImageInfo ii[2] = { { smp_linear, eye ? target_r.view : target.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL }, { smp_linear, ui_layer.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL } };
                VkWriteDescriptorSet wds[2] = { { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii[0] },
                                                { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 1, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii[1] } };
                vk_push(&push_arena, cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, play_hdr, 0, 2, wds);
                vkCmdPushConstants(cmdbuf, play_hdr, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof hp, &hp);
                vkCmdDraw(cmdbuf, 4, 1, 0, 0);
            }
            end_pass();
            if (hdr_mode && dump_wanted() && dump_record()) { dump_paper = extra[0]; dump_peak = extra[1]; dump_knee = extra[2]; dump_desat = extra[3]; }
            barrier_img(sc_images[idx], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        } else {
            barrier_img(target.img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
            barrier_img(sc_images[idx], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
            VkImageCopy ic = { .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                               .extent = { sc_extent.width, sc_extent.height, 1 } };
            vkCmdCopyImage(cmdbuf, target.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sc_images[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &ic);
        }
    } else {
        memset(stage_ptr, 0, (size_t)stage_size);
        cpu_blit(top_px, TOP_W, rt); cpu_blit(bot_px, BOT_W, rb);
        barrier_img(sc_images[idx], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
        VkBufferImageCopy cp = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { sc_extent.width, sc_extent.height, 1 } };
        vkCmdCopyBufferToImage(cmdbuf, stage, sc_images[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
    }
    barrier_img(sc_images[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_WRITE_BIT, 0);
    vk_mark(cmdbuf, VKM_FRAME_END, frame_mark);
    vkEndCommandBuffer(cmdbuf);
    VkPipelineStageFlags ws = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo sub = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1, .pWaitSemaphores = &sem_acquire, .pWaitDstStageMask = &ws,
        .commandBufferCount = 1, .pCommandBuffers = &cmdbuf, .signalSemaphoreCount = 1, .pSignalSemaphores = &sem_done[idx] };
    stage_set(STAGE_MAIN, "waiting for the Vulkan queue (the GX thread submits on it too)");
    queue_acquire();   /* (any GX submissions kept go first, in order) */
    stage_set(STAGE_MAIN, "submitting the frame");
    if (vkQueueSubmit(queue, 1, &sub, fence) == VK_ERROR_DEVICE_LOST) vk_diag_lost("a frame's submission");
    if (gpu_ok && vk_hw) pica_vk_scan_lock(0);
    VkPresentInfoKHR pi = { .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1, .pWaitSemaphores = &sem_done[idx], .swapchainCount = 1, .pSwapchains = &swap, .pImageIndices = &idx };
    double w2 = wait_clock();
    stage_set(STAGE_MAIN, "presenting the frame (vkQueuePresentKHR)");
    {   /* PWR_TEST_LIMITER=<fps>: an outside frame limiter imitated (RivaTuner's cap, #73), each present held until 1/fps
           after the one before */
        static double fps = -1, at; if (fps < 0) { const char *e = getenv("PWR_TEST_LIMITER"); fps = e ? atof(e) : 0; }
        if (fps > 0) { const double due = at + 1.0 / fps, t = wait_clock(); if (t < due) k_sleep_ns((int64_t)((due - t) * 1e9)); at = wait_clock(); }
    }
    r = vkQueuePresentKHR(queue, &pi);
    if (r == VK_ERROR_DEVICE_LOST) vk_diag_lost("the present");
    {   /* PWR_PRESENT_HOLD_MS=<ms>: the queue held that much longer, as a driver that blocks in the present holds it (#49:
           AMD's on Windows, ~15 ms at 60 Hz): the GX thread's kept submissions tested on any GPU */
        static double hold = -1; if (hold < 0) { const char *e = getenv("PWR_PRESENT_HOLD_MS"); hold = e ? atof(e) : 0; }
        if (hold > 0) k_sleep_ns((int64_t)(hold * 1e6));
    }
    {   /* how long presenting holds the queue, averaged over ~20 frames: over 2 ms, the GX thread's submissions are kept */
        static double average; static int told = -1;
        average += (wait_clock() - w2 - average) * .05;
        present_slow = average > .002;
        if (present_slow != told) {
            if (told >= 0 || present_slow)
                fprintf(stderr, "video: presenting holds the queue %.1f ms a frame: the GX thread's submissions %s\n", average * 1e3,
                        present_slow ? "are kept meanwhile" : "wait for it");
            told = present_slow;
        }
    }
    g_vid_wait_present += wait_clock() - w2;
    queue_release();   /* (with what the GX thread submitted meanwhile) */
    if (dump_peak >= 0) { dump_write(dump_paper, dump_peak, dump_knee, dump_desat); dump_peak = -1; }   /* PWR_HDR_DUMP */
    stage_set(STAGE_MAIN, stage_was);
#ifdef __ANDROID__
    if (r == VK_SUBOPTIMAL_KHR) r = VK_SUCCESS;   /* not pre-rotated (create_swapchain: the compositor turns it), every frame */
#endif
    if (r != VK_SUCCESS) { g_vid_present_fail++; g_vid_last_err = r; } else { g_vid_presented++; safe_end(); }   /* (a frame shown: the graphics opened) */
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) recreate_swapchain();
    return 1;
}

int video_vk(VkPhysicalDevice *p, VkDevice *d, VkQueue *q, uint32_t *family) {
    if (!dev || !vk_hw) return -1;
    *p = phys; *d = dev; *q = queue; *family = qfam; return 0;
}
/* the GX thread's use of the queue (pica_vk.c): its waits for it, which presenting holds (main.c's 2 s line) */
double g_vid_wait_queue_gx;
static const char *gx_stage_was;
void video_queue_lock(void) {
    gx_stage_was = stage_set(STAGE_GX, "waiting for the Vulkan queue (presenting uses it too)");
    const double t0 = wait_clock();
    queue_acquire();
    g_vid_wait_queue_gx += wait_clock() - t0;
    stage_set(STAGE_GX, "using the Vulkan queue");
}
void video_queue_unlock(void) { queue_release(); stage_set(STAGE_GX, gx_stage_was); }
/* Presenting holds the queue long (video_frame: the present's time, averaged): only then are the GX thread's
   submissions kept. Elsewhere (a present of 0.07 ms on an RTX 5090, mailbox) it waits for the queue as in 2.0.7: the
   keeping is for the drivers that block in the present (#49), and a GPU lost on 2026-11-14 (an RTX 5090, 9x, HDR, a
   texture pack: "submit failed (-4)" in play) came while submissions were kept, cause unknown. (present_slow, above)
   A GX fence whose submission is kept: the main thread submits it later, and Vulkan lets no other thread use a fence
   while it is being submitted. The GX thread asks before a wait or a status (pica_vk.c gx_fence_*). */
int video_fence_kept(VkFence f) {
    if (!f) return 0;
    pthread_mutex_lock(&kept_lock);
    int found = 0;
    for (int i = 0; i < nkept && !found; i++) found = kept[i].fence == f;
    pthread_mutex_unlock(&kept_lock);
    return found;
}
void video_fence_submitted(VkFence f) {   /* before waiting on it: submitted first (its holder lets go of the queue) */
    if (video_fence_kept(f)) { video_queue_lock(); video_queue_unlock(); }
}
/* the GX thread's submissions (up to two command buffers, no semaphores): at once while the queue is free, else kept for
   its holder to submit (queue_lock, above) while presenting holds it long, else after a wait for it */
VkResult video_queue_submit(uint32_t n, const VkCommandBuffer *cbs, VkFence f) {
    static int wait = -1; if (wait < 0) wait = getenv("PWR_GX_SUBMIT_WAIT") != NULL;
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = n, .pCommandBuffers = cbs };
    pthread_mutex_lock(&kept_lock);
    if (pthread_mutex_trylock(&queue_lock) == 0) {
        kept_submit();   /* (none: the last holder submitted them) */
        VkResult r = vkQueueSubmit(queue, 1, &si, f);
        pthread_mutex_unlock(&queue_lock); pthread_mutex_unlock(&kept_lock);
        if (r == VK_ERROR_DEVICE_LOST) vk_diag_lost("a submission of the game's");
        return r;
    }
    if (!wait && present_slow && n <= 2 && nkept < KEPT_MAX) {
        kept[nkept].n = n; memcpy(kept[nkept].cbs, cbs, n * sizeof *cbs); kept[nkept].fence = f; nkept++;
        g_vid_gx_kept++;
        pthread_mutex_unlock(&kept_lock);
        return VK_SUCCESS;
    }
    pthread_mutex_unlock(&kept_lock);   /* waiting for the queue (its holder takes kept_lock to let go) */
    video_queue_lock();
    VkResult r = vkQueueSubmit(queue, 1, &si, f);
    video_queue_unlock();
    if (r == VK_ERROR_DEVICE_LOST) vk_diag_lost("a submission of the game's");
    return r;
}

void video_shutdown(void) {
    safe_end();
    if (dev) { queue_acquire(); vk_wait_device(dev); queue_release(); if (gpu_ok) msr_overlay_shutdown(); }
#ifdef MSR_OPENGL
    if (use_gl) vgl_shutdown();
#endif
    SDL_Quit();
}
