/* The bottom screen on a panel the compositor leases to the game (drm-lease-v1), driven directly through VK_KHR_display
 * while the game plays in its own window. On a dual-screen handheld in gamescope (the AYN Thor in Armada's Game Mode)
 * that is the bottom panel, which gamescope does not show. The panel's touchscreen is the 3DS touch screen.
 *   MSR_SECOND_DISPLAY=1|0                        on or off whatever the Display tab's Bottom screen on the second
 *                                                 display says (settings.ini [native] bottom_display, off by default:
 *                                                 the game in its window only, as without this file)
 *   MSR_SECOND_DISPLAY_CONNECTOR=<name>           that connector (default: the first internal panel offered)
 *   MSR_SECOND_DISPLAY_ROTATION=0|90|180|270      the picture's turn, counter-clockwise (default: the connector's panel
 *                                                 orientation)
 *   MSR_SECOND_DISPLAY_TOUCH=auto|<path>|<name>|off   the panel's touchscreen (default auto). The device is grabbed: the
 *                                                     compositor stops seeing it while the panel is leased
 *
 * Ported from MHGU Recomp's AArch64 Linux builds (MIT, Copyright (c) 2026 Rokk: drm_lease/LICENSE-MHGU-Recomp.txt):
 * runtime/nvn/vulkan/second_display_drmlease.c at
 * https://github.com/cerroverb/MHGURecomp-aarch64 (branch aarch64-linux-v2, commit 8c10fd6). Its Vulkan instance,
 * device, swapchain set-up, turning the panel's way, touch turning and thread are kept as they are there. Edits for msr2ds:
 * the picture is the 3DS bottom screen (video.c hands it over each frame, second_display_bottom), turned on the processor
 * at its own 320x240 and scaled by the panel's GPU (vkCmdBlitImage, linear filter) to the panel at its 4:3 shape, where
 * MHGU fills the whole panel on the processor (here that took a third of a core at 60 pictures a second on the AYN
 * Thor), the swapchain has the fewest images the panel allows, the thread wakes when the game hands over a new
 * picture (MHGU looks 60 times a second), a touch goes to input_touch in the bottom screen's pixels, the thread starts
 * once when the game's window is open (the Display tab's row: from the game's next start), the physical device is the first one listed (MHGU's
 * matches the game's own device), and the thread's priority is k_background_thread's. The lease and the surface are Azahar's code
 * (drm_lease/README.md).
 *
 * The game's frame never waits for this panel: its thread has a VkInstance and a VkDevice of its own, and the game's
 * side only copies 320x240 pixels under a lock. The panel's vkAcquireNextImageKHR has timeout 0 and a picture that can't
 * be shown at once waits for the next turn. Implicit layers stay off the thread's instance, so no overlay layer can add
 * a lock shared with the game's present. */
#ifdef MSR_DRM_LEASE
#define _GNU_SOURCE
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <vulkan/vulkan.h>

#include "drm_lease/drm_lease.h"
#include "gpu.h"
#include "input.h"

#define SD_LOG(...) fprintf(stderr, "second display: " __VA_ARGS__)
#define MAX_IMAGES 8
#define BOT_W 320
#define BOT_H 240

/* ---- the game's side: the newest bottom screen (video.c, each frame) */
static pthread_mutex_t bottom_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t bottom_new = PTHREAD_COND_INITIALIZER;   /* a new picture: the panel's thread wakes */
static uint8_t bottom_px[BOT_W * BOT_H * 4];   /* RGBA8, as video.c's bot_px */
static uint64_t bottom_seq;                     /* pictures handed over, 0 none yet */
static atomic_bool panel_up;

void second_display_bottom(const uint8_t *rgba) {
    if (!atomic_load_explicit(&panel_up, memory_order_relaxed)) return;
    pthread_mutex_lock(&bottom_lock);
    if (memcmp(bottom_px, rgba, sizeof bottom_px)) {
        memcpy(bottom_px, rgba, sizeof bottom_px), bottom_seq++;
        pthread_cond_signal(&bottom_new);
    }
    pthread_mutex_unlock(&bottom_lock);
}

static struct {
    VkInstance inst;
    VkPhysicalDevice phys;
    VkSurfaceKHR surface;
    VkDevice dev;
    VkQueue queue;
    VkSwapchainKHR swapchain;
    VkFormat format;
    VkExtent2D extent;
    uint32_t count;
    VkImage images[MAX_IMAGES];
    VkSemaphore acquired, rendered[MAX_IMAGES];
    VkFence done;
    VkCommandPool pool;
    VkCommandBuffer cb;
    VkBuffer staging;
    VkDeviceMemory staging_mem;
    uint32_t *staged;       /* the bottom screen turned the panel's way, picture_w x picture_h, RGBA8 */
    VkImage picture;        /* the same on the GPU, scaled from there onto the panel (vkCmdBlitImage) */
    VkDeviceMemory picture_mem;
    uint32_t picture_w, picture_h;
    VkOffset3D place[2];    /* where the picture goes on the panel: its corners */
    DrmLeaseOutput out;
    bool have_lease;
    uint8_t shown[BOT_W * BOT_H * 4];
    uint64_t shown_seq;     /* the bottom screen last staged (bottom_seq) */
    bool pending;           /* the staged picture is not on the panel yet */
    bool busy;              /* the GPU had not finished the last picture (present_once skipped its turn) */
} s;

/* ---- the picture
   Where the point (u, v) of an upright w x h picture lands on its copy turned the panel's way. rotation counts as DRM
   does, counter-clockwise: Right Side Up, Azahar's 270, puts the picture's top at the panel's right. (MHGU's put) */
static void turn(uint32_t w, uint32_t h, uint32_t u, uint32_t v, uint32_t *x, uint32_t *y) {
    bool turned = s.out.rotation == 90 || s.out.rotation == 270;
    uint32_t pw = turned ? h : w, ph = turned ? w : h;
    switch (s.out.rotation) {
    case 90: *x = v, *y = ph - 1 - u; break;
    case 180: *x = pw - 1 - u, *y = ph - 1 - v; break;
    case 270: *x = pw - 1 - v, *y = u; break;
    default: *x = u, *y = v; break;
    }
}

static void upright_size(uint32_t *w, uint32_t *h) {
    bool turned = s.out.rotation == 90 || s.out.rotation == 270;
    *w = turned ? s.extent.height : s.extent.width;
    *h = turned ? s.extent.width : s.extent.height;
}

/* The bottom screen's place in the upright w x h picture: as large as it fits at its own 4:3 shape, centered. */
static void bottom_rect(uint32_t w, uint32_t h, uint32_t *x0, uint32_t *y0, uint32_t *rw, uint32_t *rh) {
    if ((uint64_t)w * BOT_H <= (uint64_t)h * BOT_W) *rw = w, *rh = (uint32_t)((uint64_t)w * BOT_H / BOT_W);
    else *rh = h, *rw = (uint32_t)((uint64_t)h * BOT_W / BOT_H);
    *x0 = (w - *rw) / 2, *y0 = (h - *rh) / 2;
}

/* The picture's size turned the panel's way, and its place on the panel: bottom_rect's corners, turned. */
static void plan_picture(void) {
    bool turned = s.out.rotation == 90 || s.out.rotation == 270;
    s.picture_w = turned ? BOT_H : BOT_W, s.picture_h = turned ? BOT_W : BOT_H;
    uint32_t w, h, x0, y0, rw, rh, ax, ay, bx, by;
    upright_size(&w, &h);
    bottom_rect(w, h, &x0, &y0, &rw, &rh);
    turn(w, h, x0, y0, &ax, &ay);
    turn(w, h, x0 + rw - 1, y0 + rh - 1, &bx, &by);
    s.place[0] = (VkOffset3D){ (int32_t)(ax < bx ? ax : bx), (int32_t)(ay < by ? ay : by), 0 };
    s.place[1] = (VkOffset3D){ (int32_t)(ax < bx ? bx : ax) + 1, (int32_t)(ay < by ? by : ay) + 1, 1 };
}

/* The newest bottom screen, if it is not the one on the panel, turned into the staging buffer. True when it changed.
   Only while the GPU is not reading the buffer (present_once: after the fence). */
static bool stage_bottom(void) {
    pthread_mutex_lock(&bottom_lock);
    bool fresh = bottom_seq != s.shown_seq;
    if (fresh) memcpy(s.shown, bottom_px, sizeof s.shown), s.shown_seq = bottom_seq;
    pthread_mutex_unlock(&bottom_lock);
    if (!fresh) return false;
    const uint32_t *src = (const uint32_t *)s.shown;
    for (uint32_t v = 0; v < BOT_H; v++)
        for (uint32_t u = 0; u < BOT_W; u++) {
            uint32_t x, y;
            turn(BOT_W, BOT_H, u, v, &x, &y);
            s.staged[y * s.picture_w + x] = src[v * BOT_W + u];
        }
    return true;
}

/* ---- Vulkan, all of it on this thread (MHGU's, unchanged apart from the device choice) */
static bool have_instance_exts(const char *const *want, uint32_t n) {
    uint32_t c = 0;
    vkEnumerateInstanceExtensionProperties(NULL, &c, NULL);
    VkExtensionProperties *p = calloc(c ? c : 1, sizeof *p);
    vkEnumerateInstanceExtensionProperties(NULL, &c, p);
    bool all = true;
    for (uint32_t i = 0; i < n; i++) {
        bool found = false;
        for (uint32_t k = 0; k < c; k++) found = found || !strcmp(p[k].extensionName, want[i]);
        if (!found) SD_LOG("the driver has no %s\n", want[i]);
        all = all && found;
    }
    free(p);
    return all;
}

static bool create_instance_for_panel(void) {
    static const char *const ext[] = {
        VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_DISPLAY_EXTENSION_NAME, VK_EXT_DIRECT_MODE_DISPLAY_EXTENSION_NAME,
        VK_EXT_ACQUIRE_DRM_DISPLAY_EXTENSION_NAME,
    };
    uint32_t n = sizeof ext / sizeof *ext;
    if (!have_instance_exts(ext, n)) return false;
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "msr-native second display",
                             .apiVersion = VK_API_VERSION_1_1};
    VkInstanceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app,
                               .enabledExtensionCount = n, .ppEnabledExtensionNames = ext};
    VkResult r = vkCreateInstance(&ci, NULL, &s.inst);
    if (r != VK_SUCCESS) { s.inst = VK_NULL_HANDLE; SD_LOG("vkCreateInstance: %d\n", r); return false; }
    uint32_t c = 1;
    r = vkEnumeratePhysicalDevices(s.inst, &c, &s.phys);
    if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || !c) { s.phys = VK_NULL_HANDLE; SD_LOG("vkEnumeratePhysicalDevices: %d\n", r); return false; }
    return true;
}

/* A device of its own, with one queue that can present on the panel. */
static bool create_device_for_panel(void) {
    uint32_t n = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(s.phys, &n, NULL);
    VkQueueFamilyProperties *qp = calloc(n ? n : 1, sizeof *qp);
    vkGetPhysicalDeviceQueueFamilyProperties(s.phys, &n, qp);
    uint32_t qfam = UINT32_MAX;
    for (uint32_t i = 0; i < n && qfam == UINT32_MAX; i++) {
        VkBool32 present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(s.phys, i, s.surface, &present);
        if (present && (qp[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT))) qfam = i;
    }
    free(qp);
    if (qfam == UINT32_MAX) { SD_LOG("no queue family presents on the panel\n"); return false; }
    float prio = 0.0f;
    VkDeviceQueueCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = qfam,
                                  .queueCount = 1, .pQueuePriorities = &prio};
    const char *ext[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo di = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qi,
                             .enabledExtensionCount = 1, .ppEnabledExtensionNames = ext};
    VkResult r = vkCreateDevice(s.phys, &di, NULL, &s.dev);
    if (r != VK_SUCCESS) { s.dev = VK_NULL_HANDLE; SD_LOG("vkCreateDevice: %d\n", r); return false; }
    vkGetDeviceQueue(s.dev, qfam, 0, &s.queue);
    VkCommandPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                  .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = qfam};
    if (vkCreateCommandPool(s.dev, &pi, NULL, &s.pool) != VK_SUCCESS) { s.pool = VK_NULL_HANDLE; return false; }
    VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = s.pool,
                                      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    return vkAllocateCommandBuffers(s.dev, &ai, &s.cb) == VK_SUCCESS;
}

static VkPresentModeKHR panel_present_mode(void) {
    uint32_t n = 8;
    VkPresentModeKHR m[8];
    vkGetPhysicalDeviceSurfacePresentModesKHR(s.phys, s.surface, &n, m);
    for (uint32_t i = 0; i < n; i++) if (m[i] == VK_PRESENT_MODE_MAILBOX_KHR) return m[i];
    return VK_PRESENT_MODE_FIFO_KHR;   /* (always there; a full queue then shows as acquire giving nothing: skipped) */
}

static bool create_swapchain_for_panel(void) {
    VkSurfaceCapabilitiesKHR caps;
    VkResult r = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(s.phys, s.surface, &caps);
    if (r != VK_SUCCESS) { SD_LOG("surface capabilities: %d\n", r); return false; }
    if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) { SD_LOG("the panel's images can't be copied to\n"); return false; }
    uint32_t nf = 32;
    VkSurfaceFormatKHR f[32];
    r = vkGetPhysicalDeviceSurfaceFormatsKHR(s.phys, s.surface, &nf, f);
    if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || !nf) { SD_LOG("surface formats: %d\n", r); return false; }
    VkSurfaceFormatKHR sf = f[0];
    for (uint32_t i = 0; i < nf; i++)
        if (f[i].format == VK_FORMAT_B8G8R8A8_UNORM) { sf = f[i]; break; }
    /* the fewest images: a picture never waits behind another queued one (the AYN Thor's panel offers FIFO only, from
       2 images: with MHGU's 3 a picture could wait two refreshes, with 2 at most one) */
    uint32_t count = caps.minImageCount > MAX_IMAGES ? MAX_IMAGES : caps.minImageCount;
    s.extent = caps.currentExtent.width != UINT32_MAX ? caps.currentExtent : (VkExtent2D){s.out.width, s.out.height};
    s.format = sf.format;
    VkCompositeAlphaFlagBitsKHR alpha = caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
        ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR : (VkCompositeAlphaFlagBitsKHR)(caps.supportedCompositeAlpha & -caps.supportedCompositeAlpha);
    VkPresentModeKHR mode = panel_present_mode();
    VkSwapchainCreateInfoKHR ci = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = s.surface, .minImageCount = count,
        .imageFormat = sf.format, .imageColorSpace = sf.colorSpace, .imageExtent = s.extent, .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT, .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR, .compositeAlpha = alpha, .presentMode = mode, .clipped = VK_TRUE};
    r = vkCreateSwapchainKHR(s.dev, &ci, NULL, &s.swapchain);
    if (r != VK_SUCCESS) { s.swapchain = VK_NULL_HANDLE; SD_LOG("vkCreateSwapchainKHR: %d\n", r); return false; }
    s.count = MAX_IMAGES;
    r = vkGetSwapchainImagesKHR(s.dev, s.swapchain, &s.count, s.images);
    if (r != VK_SUCCESS && r != VK_INCOMPLETE) { SD_LOG("vkGetSwapchainImagesKHR: %d\n", r); return false; }
    VkSemaphoreCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    if (vkCreateSemaphore(s.dev, &si, NULL, &s.acquired) != VK_SUCCESS) return false;
    for (uint32_t i = 0; i < s.count; i++)
        if (vkCreateSemaphore(s.dev, &si, NULL, &s.rendered[i]) != VK_SUCCESS) return false;
    VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT};
    if (vkCreateFence(s.dev, &fi, NULL, &s.done) != VK_SUCCESS) { s.done = VK_NULL_HANDLE; return false; }
    SD_LOG("swapchain %ux%u, %u images (%u to %u), format %d, present mode %d\n", s.extent.width, s.extent.height, s.count,
           caps.minImageCount, caps.maxImageCount, sf.format, mode);
    return true;
}

static bool memory_for(VkMemoryRequirements mr, VkMemoryPropertyFlags want, VkDeviceMemory *mem) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(s.phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
        if (!(mr.memoryTypeBits & 1u << i) || (mp.memoryTypes[i].propertyFlags & want) != want) continue;
        VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = i};
        if (vkAllocateMemory(s.dev, &ai, NULL, mem) == VK_SUCCESS) return true;
        *mem = VK_NULL_HANDLE;
        return false;
    }
    SD_LOG("no memory of type %#x\n", want);
    return false;
}

/* The staging buffer and the picture's image, for the 320x240 bottom screen. The panel's GPU scales it with a linear
   filter (vkCmdBlitImage), which these formats must allow. */
static bool create_picture(void) {
    VkFormatProperties pf, sf;
    vkGetPhysicalDeviceFormatProperties(s.phys, VK_FORMAT_R8G8B8A8_UNORM, &pf);
    vkGetPhysicalDeviceFormatProperties(s.phys, s.format, &sf);
    VkFormatFeatureFlags src_need = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    if ((pf.optimalTilingFeatures & src_need) != src_need || !(sf.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT)) {
        SD_LOG("the GPU can't scale the picture onto the panel (blit features %#x, %#x)\n", pf.optimalTilingFeatures, sf.optimalTilingFeatures);
        return false;
    }
    plan_picture();
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = (VkDeviceSize)BOT_W * BOT_H * 4,
                             .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    if (vkCreateBuffer(s.dev, &bi, NULL, &s.staging) != VK_SUCCESS) { s.staging = VK_NULL_HANDLE; return false; }
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(s.dev, s.staging, &mr);
    void *p;
    if (!memory_for(mr, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &s.staging_mem) ||
        vkBindBufferMemory(s.dev, s.staging, s.staging_mem, 0) != VK_SUCCESS ||
        vkMapMemory(s.dev, s.staging_mem, 0, VK_WHOLE_SIZE, 0, &p) != VK_SUCCESS) return false;
    s.staged = p;
    memset(s.staged, 0, (size_t)BOT_W * BOT_H * 4);   /* black until the game's first bottom screen */
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                            .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = {s.picture_w, s.picture_h, 1}, .mipLevels = 1,
                            .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                            .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    if (vkCreateImage(s.dev, &ii, NULL, &s.picture) != VK_SUCCESS) { s.picture = VK_NULL_HANDLE; return false; }
    vkGetImageMemoryRequirements(s.dev, s.picture, &mr);
    return memory_for(mr, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &s.picture_mem) &&
           vkBindImageMemory(s.dev, s.picture, s.picture_mem, 0) == VK_SUCCESS;
}

static void image_barrier(VkImage img, VkAccessFlags src, VkAccessFlags dst, VkImageLayout from, VkImageLayout to,
                          VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage) {
    VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = src, .dstAccessMask = dst,
                              .oldLayout = from, .newLayout = to, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                              .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img,
                              .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(s.cb, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1, &b);
}

/* One picture onto the panel, or none: anything not ready at once skips this turn. False: the panel is gone. The panel's
   image is cleared to black and the picture scaled into its place on it. */
static bool present_once(void) {
    VkResult r = vkGetFenceStatus(s.dev, s.done);
    s.busy = r == VK_NOT_READY;
    if (s.busy) return true;   /* (the last copy is still running) */
    if (r != VK_SUCCESS) { SD_LOG("fence: %d\n", r); return false; }
    if (stage_bottom()) s.pending = true;
    if (!s.pending) return true;
    uint32_t img;
    r = vkAcquireNextImageKHR(s.dev, s.swapchain, 0, s.acquired, VK_NULL_HANDLE, &img);
    if (r == VK_NOT_READY || r == VK_TIMEOUT) return true;   /* (pending: presented on a later turn) */
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) { SD_LOG("acquire: %d\n", r); return false; }
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                   .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkResetCommandBuffer(s.cb, 0);
    vkBeginCommandBuffer(s.cb, &bi);
    const VkImageSubresourceRange all = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    const VkImageSubresourceLayers layer = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    image_barrier(s.picture, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy rg = {.imageSubresource = layer, .imageExtent = {s.picture_w, s.picture_h, 1}};
    vkCmdCopyBufferToImage(s.cb, s.staging, s.picture, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rg);
    image_barrier(s.picture, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    image_barrier(s.images[img], 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    const VkClearColorValue black = {.float32 = {0, 0, 0, 1}};
    vkCmdClearColorImage(s.cb, s.images[img], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &all);
    image_barrier(s.images[img], VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkImageBlit blit = {.srcSubresource = layer, .srcOffsets = {{0, 0, 0}, {(int32_t)s.picture_w, (int32_t)s.picture_h, 1}},
                        .dstSubresource = layer, .dstOffsets = {s.place[0], s.place[1]}};
    vkCmdBlitImage(s.cb, s.picture, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s.images[img], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   1, &blit, VK_FILTER_LINEAR);
    image_barrier(s.images[img], VK_ACCESS_TRANSFER_WRITE_BIT, 0, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    vkEndCommandBuffer(s.cb);
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1, .pWaitSemaphores = &s.acquired,
                       .pWaitDstStageMask = &stage, .commandBufferCount = 1, .pCommandBuffers = &s.cb,
                       .signalSemaphoreCount = 1, .pSignalSemaphores = &s.rendered[img]};
    vkResetFences(s.dev, 1, &s.done);
    r = vkQueueSubmit(s.queue, 1, &si, s.done);
    if (r != VK_SUCCESS) { SD_LOG("submit: %d\n", r); return false; }
    VkPresentInfoKHR pi = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1, .pWaitSemaphores = &s.rendered[img],
                           .swapchainCount = 1, .pSwapchains = &s.swapchain, .pImageIndices = &img};
    r = vkQueuePresentKHR(s.queue, &pi);
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) { SD_LOG("present: %d\n", r); return false; }
    s.pending = false;
    return true;
}

static void tear_down(void) {
    atomic_store(&panel_up, false);
    if (s.dev) {
        vkDeviceWaitIdle(s.dev);
        if (s.staging) vkDestroyBuffer(s.dev, s.staging, NULL);
        if (s.staging_mem) vkFreeMemory(s.dev, s.staging_mem, NULL);
        if (s.picture) vkDestroyImage(s.dev, s.picture, NULL);
        if (s.picture_mem) vkFreeMemory(s.dev, s.picture_mem, NULL);
        if (s.done) vkDestroyFence(s.dev, s.done, NULL);
        for (uint32_t i = 0; i < s.count; i++) if (s.rendered[i]) vkDestroySemaphore(s.dev, s.rendered[i], NULL);
        if (s.acquired) vkDestroySemaphore(s.dev, s.acquired, NULL);
        if (s.pool) vkDestroyCommandPool(s.dev, s.pool, NULL);
        if (s.swapchain) vkDestroySwapchainKHR(s.dev, s.swapchain, NULL);
        vkDestroyDevice(s.dev, NULL);
    }
    if (s.inst) {
        if (s.surface) vkDestroySurfaceKHR(s.inst, s.surface, NULL);
        if (s.phys) drm_lease_release_display(s.phys);
        vkDestroyInstance(s.inst, NULL);
    }
    if (s.have_lease) drm_lease_release();
    memset(&s, 0, sizeof s);
}

/* A finger at panel pixel x, y (the touch reader's thread): into the upright picture, as turn() turns it out (MHGU's),
   then into the bottom screen's pixels. A finger on the black beside the screen is a lift. */
void second_display_touch(bool down, unsigned x, unsigned y) {
    uint32_t pw = s.extent.width, ph = s.extent.height, u, v, w, h, x0, y0, rw, rh;
    if (!pw || !ph) return;
    switch (s.out.rotation) {
    case 90: u = ph - 1 - y, v = x; break;
    case 180: u = pw - 1 - x, v = ph - 1 - y; break;
    case 270: u = y, v = pw - 1 - x; break;
    default: u = x, v = y; break;
    }
    upright_size(&w, &h);
    bottom_rect(w, h, &x0, &y0, &rw, &rh);
    if (!down || u < x0 || u >= x0 + rw || v < y0 || v >= y0 + rh) { input_touch(0, -1, -1); return; }
    input_touch(1, (int)((u - x0) * BOT_W / rw), (int)((v - y0) * BOT_H / rh));
}

static bool bring_up(void) {
    const char *rotation = getenv("MSR_SECOND_DISPLAY_ROTATION"), *touch = getenv("MSR_SECOND_DISPLAY_TOUCH");
    if (!touch) touch = "auto";
    if (!strcmp(touch, "off")) touch = NULL;
    if (!drm_lease_acquire(getenv("MSR_SECOND_DISPLAY_CONNECTOR"), rotation ? atoi(rotation) : -1, touch, &s.out)) return false;
    s.have_lease = true;
    if (!create_instance_for_panel()) return false;
    s.surface = drm_lease_create_surface(s.inst, s.phys, &s.out);
    if (!s.surface || !create_device_for_panel() || !create_swapchain_for_panel() || !create_picture()) return false;
    s.pending = true;
    atomic_store(&panel_up, true);
    return present_once();
}

/* The panel's thread between turns: woken by the game's next bottom screen (second_display_bottom), so a picture goes
   to the panel as soon as it is made, or after 100 ms without one (the lease's events are read each turn). A picture
   that could not go yet (the GPU busy, no free panel image) is tried again after 1 ms. */
static void wait_for_picture(void) {
    if (s.pending || s.busy) {
        nanosleep(&(struct timespec){.tv_nsec = 1000000L}, NULL);
        return;
    }
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_nsec += 100000000L;
    if (until.tv_nsec >= 1000000000L) until.tv_sec++, until.tv_nsec -= 1000000000L;
    pthread_mutex_lock(&bottom_lock);
    while (bottom_seq == s.shown_seq && pthread_cond_timedwait(&bottom_new, &bottom_lock, &until) == 0) {}
    pthread_mutex_unlock(&bottom_lock);
}

static void *panel_thread(void *arg) {
    (void)arg;
    /* signals stay with the game's threads, and the scheduler favors them over this one */
    sigset_t set;
    sigfillset(&set);
    sigdelset(&set, SIGSEGV), sigdelset(&set, SIGBUS), sigdelset(&set, SIGILL), sigdelset(&set, SIGFPE), sigdelset(&set, SIGABRT);
    pthread_sigmask(SIG_BLOCK, &set, NULL);
    k_background_thread();

    if (bring_up()) {
        SD_LOG("up: the bottom screen is on the leased panel\n");
        video_bottom_leased(1);   /* the window shows the top screen alone (video.c effective_layout) */
        while (drm_lease_alive() && present_once()) wait_for_picture();
        video_bottom_leased(0);
        SD_LOG("the panel is gone: the game goes on, on its window only\n");
    } else {
        SD_LOG("no panel to lease: the game goes on, on its window only\n");
    }
    tear_down();
    return NULL;
}

/* The Display tab's Bottom screen on the second display row (overlay_host.cpp): shown where the compositor offers a
   panel to lease. Asked once, without leasing, on a thread of its own (drm_lease_offered can wait two timeouts of 5 s):
   -1 not known yet, 0 none, 1 one is offered. Not asked by a game that leases (its own lease would take the offer away
   while the probe looks); the row shows there anyway, its setting being on. */
static atomic_int offered = -1;

static void *probe_thread(void *arg) {
    (void)arg;
    k_background_thread();
    const bool yes = drm_lease_offered(getenv("MSR_SECOND_DISPLAY_CONNECTOR"));
    atomic_store(&offered, yes ? 1 : 0);
    SD_LOG("%s\n", yes ? "a panel is offered (the Display tab's Bottom screen on the second display)"
                       : "no panel offered (the Display tab's row stays hidden)");
    return NULL;
}

void second_display_probe(void) {
    static bool asked;
    if (asked) return;
    asked = true;
    if (!getenv("GAMESCOPE_WAYLAND_DISPLAY") && !getenv("WAYLAND_DISPLAY")) { atomic_store(&offered, 0); return; }
    pthread_t th;
    if (pthread_create(&th, NULL, probe_thread, NULL) != 0) { atomic_store(&offered, 0); return; }
    pthread_detach(th);
}

int second_display_offered(void) {
    return atomic_load(&offered) == 1 || atomic_load_explicit(&panel_up, memory_order_relaxed);
}

/* On with the Display tab's row (settings.ini [native] bottom_display, which the launcher passes as --bottom-display=on)
   or MSR_SECOND_DISPLAY=1; MSR_SECOND_DISPLAY=0 keeps it off whatever the row says. */
void second_display_start(void) {
    const char *e = getenv("MSR_SECOND_DISPLAY");
    const bool on = e && *e ? !strcmp(e, "1") : video_bottom_display() != 0;
    if (!on) { SD_LOG("off (the Display tab's Bottom screen on the second display, or MSR_SECOND_DISPLAY=1)\n"); return; }
    /* (read when the thread's vkCreateInstance runs; the game's instance exists by now, and nothing makes another) */
    setenv("VK_LOADER_LAYERS_DISABLE", "~implicit~", 1);
    pthread_t th;
    if (pthread_create(&th, NULL, panel_thread, NULL) != 0) { SD_LOG("no thread: the game goes on, on its window only\n"); return; }
    pthread_detach(th);
}
#endif
