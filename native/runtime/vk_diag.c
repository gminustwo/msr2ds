/* What the GPU was doing when it was lost (vk_diag.h). 2026-11-14: twice in one day the RTX 5090 of the player whose
 * machine this was developed on was lost in play (pica_vk: "submit failed (-4)"): the picture froze while the game and
 * its sound went on. Neither came back in a test of the same save, settings (9x, HDR10 at 5120x2160, FSR, the HD pack)
 * and Steam's Vulkan layers. Its driver (610.88) has VK_EXT_device_fault and VK_NV_device_diagnostic_checkpoints: the
 * next one says where. PWR_VK_NO_DIAG=1: neither enabled. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vk_diag.h"

PFN_vkCmdSetCheckpointNV g_vk_checkpoint;
static PFN_vkGetQueueCheckpointDataNV get_checkpoints;
#ifdef VK_EXT_device_fault
static PFN_vkGetDeviceFaultInfoEXT get_fault;
#endif
static VkDevice d_dev;
static VkQueue d_queue;

static struct { uint32_t n, addr, w, h, depth; int fmt, scale; } passes[256];

void vk_diag_pass(uint32_t n, uint32_t addr, uint32_t w, uint32_t h, int fmt, uint32_t depth_addr, int scale) {
    passes[n & 255].n = n; passes[n & 255].addr = addr; passes[n & 255].w = w; passes[n & 255].h = h;
    passes[n & 255].fmt = fmt; passes[n & 255].depth = depth_addr; passes[n & 255].scale = scale;
}

void vk_diag_wanted(const VkExtensionProperties *have, uint32_t nhave, int *fault, int *checkpoints) {
    *fault = *checkpoints = 0;
    if (getenv("PWR_VK_NO_DIAG")) return;
    for (uint32_t i = 0; i < nhave; i++) {
#ifdef VK_EXT_device_fault
        if (!strcmp(have[i].extensionName, VK_EXT_DEVICE_FAULT_EXTENSION_NAME)) *fault = 1;
#endif
        if (!strcmp(have[i].extensionName, VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME)) *checkpoints = 1;
    }
}

void vk_diag_init(VkDevice dev, VkQueue queue, int fault, int checkpoints) {
    d_dev = dev; d_queue = queue;
    if (checkpoints) {
        g_vk_checkpoint = (PFN_vkCmdSetCheckpointNV)vkGetDeviceProcAddr(dev, "vkCmdSetCheckpointNV");
        get_checkpoints = (PFN_vkGetQueueCheckpointDataNV)vkGetDeviceProcAddr(dev, "vkGetQueueCheckpointDataNV");
        if (!get_checkpoints) g_vk_checkpoint = NULL;
    }
#ifdef VK_EXT_device_fault
    if (fault) get_fault = (PFN_vkGetDeviceFaultInfoEXT)vkGetDeviceProcAddr(dev, "vkGetDeviceFaultInfoEXT");
#else
    (void)fault;
#endif
    if (g_vk_checkpoint || fault)
        fprintf(stderr, "video: if the GPU is lost, the log says where (%s%s%s)\n", g_vk_checkpoint ? "checkpoints" : "",
                g_vk_checkpoint && fault ? ", " : "", fault ? "the driver's fault report" : "");
}

static const char *const mark_names[VKM_COUNT] = {
    "?", "uploads of the game's batch", "the game's batch", "a pass of the game's", "the end of the game's batch",
    "a copy of a render target to a texture", "a screen read back", "a frame's presentation", "its FSR upscaling",
    "its screens", "its HUD, menus and prompts", "the settings overlay", "its HDR output", "the end of the frame",
};

static const char *stage_name(VkPipelineStageFlagBits s) {
    switch (s) {
    case VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT: return "started";
    case VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT: return "finished";
    default: return "reached";
    }
}

#ifdef VK_EXT_device_fault
static const char *address_type(VkDeviceFaultAddressTypeEXT t) {
    switch (t) {
    case VK_DEVICE_FAULT_ADDRESS_TYPE_NONE_EXT: return "none";
    case VK_DEVICE_FAULT_ADDRESS_TYPE_READ_INVALID_EXT: return "a read of an invalid address";
    case VK_DEVICE_FAULT_ADDRESS_TYPE_WRITE_INVALID_EXT: return "a write to an invalid address";
    case VK_DEVICE_FAULT_ADDRESS_TYPE_EXECUTE_INVALID_EXT: return "an execution at an invalid address";
    case VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_UNKNOWN_EXT: return "an instruction pointer (unknown)";
    case VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_INVALID_EXT: return "an instruction pointer (invalid)";
    case VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_FAULT_EXT: return "an instruction pointer (faulting)";
    default: return "another kind";
    }
}
#endif

void vk_diag_lost(const char *where) {
    static volatile int once;
    if (__sync_lock_test_and_set(&once, 1)) return;
    fprintf(stderr, "video: the GPU was lost (seen by %s): what it was doing\n", where);
#ifdef VK_EXT_device_fault
    if (get_fault && d_dev) {
        VkDeviceFaultCountsEXT counts = { .sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT };
        VkResult r = get_fault(d_dev, &counts, NULL);
        if (r == VK_SUCCESS || r == VK_INCOMPLETE) {
            VkDeviceFaultAddressInfoEXT addr[16]; VkDeviceFaultVendorInfoEXT vend[16];
            memset(addr, 0, sizeof addr); memset(vend, 0, sizeof vend);
            if (counts.addressInfoCount > 16) counts.addressInfoCount = 16;
            if (counts.vendorInfoCount > 16) counts.vendorInfoCount = 16;
            counts.vendorBinarySize = 0;
            VkDeviceFaultInfoEXT info = { .sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT, .pAddressInfos = addr, .pVendorInfos = vend };
            r = get_fault(d_dev, &counts, &info);
            fprintf(stderr, "video: the driver's fault report (%d): \"%s\"\n", (int)r, info.description);
            for (uint32_t i = 0; i < counts.addressInfoCount; i++)
                fprintf(stderr, "video:   %s, at %#llx (to within %#llx)\n", address_type(addr[i].addressType),
                        (unsigned long long)addr[i].reportedAddress, (unsigned long long)addr[i].addressPrecision);
            for (uint32_t i = 0; i < counts.vendorInfoCount; i++)
                fprintf(stderr, "video:   \"%s\": code %#llx, data %#llx\n", vend[i].description,
                        (unsigned long long)vend[i].vendorFaultCode, (unsigned long long)vend[i].vendorFaultData);
        } else {
            fprintf(stderr, "video: no fault report from the driver (%d)\n", (int)r);
        }
    }
#endif
    if (get_checkpoints && d_queue) {
        VkCheckpointDataNV cp[32];
        uint32_t n = 0;
        get_checkpoints(d_queue, &n, NULL);
        if (n > 32) n = 32;
        for (uint32_t i = 0; i < n; i++) { cp[i].sType = VK_STRUCTURE_TYPE_CHECKPOINT_DATA_NV; cp[i].pNext = NULL; }
        get_checkpoints(d_queue, &n, cp);
        fprintf(stderr, "video: the last markers the GPU reached: %u\n", n);
        for (uint32_t i = 0; i < n; i++) {
            const uint32_t m = (uint32_t)(uintptr_t)cp[i].pCheckpointMarker, what = m >> 24, k = m & 0xFFFFFFu;
            fprintf(stderr, "video:   %s %s %u", stage_name(cp[i].stage), what < VKM_COUNT ? mark_names[what] : "?", k);
            if (what == VKM_PASS && (passes[k & 255].n & 0xFFFFFFu) == k)
                fprintf(stderr, " (into %08x, %ux%u x%d, format %d, depth %08x)", passes[k & 255].addr, passes[k & 255].w,
                        passes[k & 255].h, passes[k & 255].scale, passes[k & 255].fmt, passes[k & 255].depth);
            fputc('\n', stderr);
        }
    }
    fflush(stderr);
}
