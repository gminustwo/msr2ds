/* What the GPU was doing when it was lost (vk_diag.c). The GPU work carries markers (VK_NV_device_diagnostic_checkpoints,
 * NVIDIA's): each batch of the game's, each of its passes, its copies and read-backs, and each part of a frame's
 * presentation. On the first VK_ERROR_DEVICE_LOST the log gets the last markers the GPU reached and, with
 * VK_EXT_device_fault, the driver's account of the fault (the address and the access). */
#pragma once
#include <stdint.h>
#include <vulkan/vulkan.h>

enum {
    VKM_UPLOAD = 1,   /* a batch's uploads (textures, buffers): n the batch */
    VKM_BATCH,        /* a batch's draws: n the batch */
    VKM_PASS,         /* one of its passes: n the pass (vk_diag_pass has its target) */
    VKM_BATCH_END,    /* the batch's draws done: n the batch */
    VKM_COPY,         /* a render target copied to a texture (alias write-back) */
    VKM_SCAN,         /* a screen read back */
    VKM_FRAME,        /* a frame's presentation: n the frame */
    VKM_FSR,          /* its FSR upscaling */
    VKM_SCREENS,      /* the screens drawn into the target */
    VKM_HUD,          /* the single screen's HUD, menus and prompts (compositor.cpp's quads) */
    VKM_MENU,         /* the settings overlay (ImGui) */
    VKM_HDR,          /* HDR output */
    VKM_FRAME_END,    /* the frame done: n the frame */
    VKM_COUNT
};

#ifdef __cplusplus
extern "C" {
#endif
extern PFN_vkCmdSetCheckpointNV g_vk_checkpoint;   /* set by vk_diag_init when the device has the extension */
static inline void vk_mark(VkCommandBuffer cb, unsigned what, uint32_t n) {
    if (g_vk_checkpoint) g_vk_checkpoint(cb, (const void *)(uintptr_t)(((uint32_t)what << 24) | (n & 0xFFFFFFu)));
}
/* a pass's target, for the report (the GX thread; a ring of the last 256) */
void vk_diag_pass(uint32_t n, uint32_t addr, uint32_t w, uint32_t h, int fmt, uint32_t depth_addr, int scale);
/* which of the two extensions the device has (none with PWR_VK_NO_DIAG=1) */
void vk_diag_wanted(const VkExtensionProperties *have, uint32_t nhave, int *fault, int *checkpoints);
void vk_diag_init(VkDevice dev, VkQueue queue, int fault, int checkpoints);
/* VK_ERROR_DEVICE_LOST seen (any thread): the report, once */
void vk_diag_lost(const char *where);
#ifdef __cplusplus
}
#endif
