/* Vulkan's functions loaded at run time (MSR_VOLK: externals/volk, which every source sees in place of the Vulkan
 * library's functions). The Android app's way; a PC build takes it with -DMSR_VOLK=ON, as a check of it.
 *
 * Android: from the system's libvulkan.so, or for the game from a GPU driver the player installed (the launcher's GPU
 * driver, #58, #66): an Adreno driver package as Azahar, Eden and other emulators take them (a .zip with meta.json and
 * the driver's libraries: Mesa's Turnip and the like), which MsrActivity unpacked into files/gpu_drivers/<name>/ and
 * libadrenotools (externals/libadrenotools) loads in place of the phone's own driver. The launcher always takes the
 * system's, so a driver that fails cannot keep the player from choosing another. That driver's instance has no use for
 * SDL's Vulkan library (the system's), so the window's surface is made here, with the driver's own function. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL.h>
#include <SDL_vulkan.h>
#ifdef __ANDROID__
#include <dlfcn.h>
#include <SDL_syswm.h>
#include <vulkan/vulkan_android.h>
#include "android.h"
#endif

static int custom;   /* the player's GPU driver is the one loaded */

int msr_vk_custom(void) { return custom; }

#ifdef __ANDROID__
/* A string value of meta.json's (a flat object of strings and numbers; \" and \\ are the only escapes kept) */
static void json_string(const char *json, const char *key, char *out, size_t n) {
    char quoted[64]; snprintf(quoted, sizeof quoted, "\"%s\"", key);
    out[0] = 0;
    const char *p = strstr(json, quoted);
    if (!p) return;
    p += strlen(quoted);
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p++ != ':') return;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p++ != '"') return;
    size_t k = 0;
    for (; *p && *p != '"' && k + 1 < n; p++) {
        if (*p == '\\' && p[1]) p++;
        out[k++] = *p;
    }
    out[k] = 0;
}

/* libadrenotools' loader (msr_adrenotools, its own library: native/CMakeLists.txt) */
typedef void *(*open_libvulkan_fn)(int dlopen_mode, int feature_flags, const char *tmp_lib_dir, const char *hook_lib_dir,
                                   const char *custom_driver_dir, const char *custom_driver_name,
                                   const char *file_redirect_dir, void **user_mapping_handle);
#define ADRENOTOOLS_DRIVER_CUSTOM 1

/* The driver in files/gpu_drivers/<id>/, through libadrenotools: 0 when volk has its vkGetInstanceProcAddr. `why`: what
   failed, for the log and the launcher. */
static int load_custom(const char *id, char *why, size_t why_n) {
    if (strchr(id, '/') || !strcmp(id, ".") || !strcmp(id, "..")) { snprintf(why, why_n, "not a driver's folder name"); return -1; }
    char dir[4096], meta[4200];
    snprintf(dir, sizeof dir, "%s/%s/", msr_android_gpu_drivers(), id);   /* (libadrenotools adds the name to it) */
    snprintf(meta, sizeof meta, "%smeta.json", dir);
    static char json[65536];
    FILE *f = fopen(meta, "rb");
    if (!f) { snprintf(why, why_n, "no %s", meta); return -1; }
    const size_t len = fread(json, 1, sizeof json - 1, f); fclose(f); json[len] = 0;
    char lib[256], name[256], version[128];
    json_string(json, "libraryName", lib, sizeof lib);
    json_string(json, "name", name, sizeof name);
    json_string(json, "driverVersion", version, sizeof version);
    if (!lib[0] || strchr(lib, '/')) { snprintf(why, why_n, "its meta.json names no driver library"); return -1; }
    const char *hooks = msr_android_native_libs();
    if (!hooks[0]) { snprintf(why, why_n, "the app's native library folder is not known"); return -1; }
    void *tools = dlopen("libmsr_adrenotools.so", RTLD_NOW | RTLD_LOCAL);
    open_libvulkan_fn open_libvulkan = tools ? (open_libvulkan_fn)dlsym(tools, "adrenotools_open_libvulkan") : NULL;
    if (!open_libvulkan) { snprintf(why, why_n, "libadrenotools: %s", dlerror()); return -1; }
    void *vk = open_libvulkan(RTLD_NOW, ADRENOTOOLS_DRIVER_CUSTOM, NULL, hooks, dir, lib, NULL, NULL);
    if (!vk) { snprintf(why, why_n, "libadrenotools could not load %s", lib); return -1; }
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(vk, "vkGetInstanceProcAddr");
    if (!gipa) { snprintf(why, why_n, "no vkGetInstanceProcAddr with it"); return -1; }
    volkInitializeCustom(gipa);
    fprintf(stderr, "video: GPU driver %s%s%s (%s%s), loaded with libadrenotools\n", name[0] ? name : id, version[0] ? " " : "",
            version, dir, lib);
    return 0;
}
#endif

/* Vulkan's library: the player's GPU driver `driver` (its folder's name; NULL or "": the system's), else the system's.
   0 when loaded; `why` (may be NULL) says why that driver could not be, "" when it was or none was asked for. */
int msr_vk_load(const char *driver, char *why, size_t why_n) {
    char none[8];
    if (!why) { why = none; why_n = sizeof none; }
    why[0] = 0;
    custom = 0;
#ifdef __ANDROID__
    if (driver && *driver) {
        if (!load_custom(driver, why, why_n)) { custom = 1; return 0; }
        fprintf(stderr, "video: the GPU driver %s could not be loaded (%s): the system's\n", driver, why);
    }
#else
    (void)driver;
#endif
    if (volkInitialize() != VK_SUCCESS) { fprintf(stderr, "video: no Vulkan library\n"); return -1; }
    return 0;
}

/* The window's surface, as SDL_Vulkan_CreateSurface (SDL_TRUE when made; else SDL_GetError says why) */
SDL_bool msr_vk_surface(SDL_Window *win, VkInstance inst, VkSurfaceKHR *surf) {
#ifdef __ANDROID__
    if (custom) {
        SDL_SysWMinfo info; SDL_VERSION(&info.version);
        if (!SDL_GetWindowWMInfo(win, &info)) return SDL_FALSE;
        if (!info.info.android.window) { SDL_SetError("the window has no surface yet"); return SDL_FALSE; }
        PFN_vkCreateAndroidSurfaceKHR create = (PFN_vkCreateAndroidSurfaceKHR)vkGetInstanceProcAddr(inst, "vkCreateAndroidSurfaceKHR");
        if (!create) { SDL_SetError("the GPU driver has no vkCreateAndroidSurfaceKHR"); return SDL_FALSE; }
        VkAndroidSurfaceCreateInfoKHR ci = { .sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR, .window = info.info.android.window };
        const VkResult r = create(inst, &ci, NULL, surf);
        if (r != VK_SUCCESS) { SDL_SetError("vkCreateAndroidSurfaceKHR failed (%d)", (int)r); return SDL_FALSE; }
        return SDL_TRUE;
    }
#endif
    return SDL_Vulkan_CreateSurface(win, inst, surf);
}

/* Vulkan 1.3's functions the renderer calls (as vk13_load.c has them without volk): volkLoadDevice loaded the core
   names where the device has 1.3, and the names of the extensions they came from (dynamic rendering, extended dynamic
   state) where it has those; the core names stand for them there */
void msr_vk13_load(VkDevice dev) {
    (void)dev;
#define MSR_VK13_ALIAS(name, alt)                                                                                          \
    if (!name) name = alt;                                                                                                 \
    if (!name) fprintf(stderr, "video: the device has no %s\n", #name);
    MSR_VK13_ALIAS(vkCmdBeginRendering, vkCmdBeginRenderingKHR)
    MSR_VK13_ALIAS(vkCmdEndRendering, vkCmdEndRenderingKHR)
    MSR_VK13_ALIAS(vkCmdSetCullMode, vkCmdSetCullModeEXT)
    MSR_VK13_ALIAS(vkCmdSetFrontFace, vkCmdSetFrontFaceEXT)
    MSR_VK13_ALIAS(vkCmdSetDepthTestEnable, vkCmdSetDepthTestEnableEXT)
    MSR_VK13_ALIAS(vkCmdSetDepthWriteEnable, vkCmdSetDepthWriteEnableEXT)
    MSR_VK13_ALIAS(vkCmdSetDepthCompareOp, vkCmdSetDepthCompareOpEXT)
    MSR_VK13_ALIAS(vkCmdSetStencilTestEnable, vkCmdSetStencilTestEnableEXT)
    MSR_VK13_ALIAS(vkCmdSetStencilOp, vkCmdSetStencilOpEXT)
}
