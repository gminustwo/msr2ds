// The lease and the surface on the leased panel, for runtime/second_display_lease.c (plain C). New glue for
// MHGU Recomp (MIT, Copyright (c) 2026 Rokk: LICENSE-MHGU-Recomp.txt); the code behind it is Azahar's:
// drm_lease_window.cpp, drm_display_surface.cpp.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int fd;                 // the lease: stays open until drm_lease_release
    uint32_t connector_id;
    uint32_t width, height; // the panel's mode, as the panel scans it out
    int rotation;           // degrees: MSR_SECOND_DISPLAY_ROTATION, else the connector's panel orientation
} DrmLeaseOutput;

// Asks the compositor for a connector (NULL or "": the first internal panel it offers). Blocks up to two timeouts of
// 5 s, so it runs on second_display_lease.c's thread. touch_device: NULL or "" reads no touch; else an evdev path, a device
// name or "auto".
bool drm_lease_acquire(const char *connector, int rotation, const char *touch_device, DrmLeaseOutput *out);
// Would drm_lease_acquire find that connector? Asks the compositor without leasing (blocks as drm_lease_acquire can).
bool drm_lease_offered(const char *connector);
bool drm_lease_alive(void);  // reads the compositor's events without waiting; false once the lease has ended
void drm_lease_release(void);

// CreateDrmDisplaySurface: VK_NULL_HANDLE on failure (logged). The instance has VK_KHR_display,
// VK_EXT_direct_mode_display and VK_EXT_acquire_drm_display.
VkSurfaceKHR drm_lease_create_surface(VkInstance instance, VkPhysicalDevice phys, const DrmLeaseOutput *output);
void drm_lease_release_display(VkPhysicalDevice phys);  // gives back what drm_lease_create_surface acquired

// The touch reader's thread: a finger at panel pixel x, y, or lifted. Defined by the caller (second_display_lease.c).
void second_display_touch(bool down, unsigned x, unsigned y);

#ifdef __cplusplus
}
#endif
