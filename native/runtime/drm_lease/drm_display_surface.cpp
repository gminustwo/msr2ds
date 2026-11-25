// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.
// Ported from Azahar-Enhanced (https://github.com/virtudude/azahar, branch drm-leasing) commit d16299140,
// src/video_core/renderer_vulkan/vk_platform.cpp CreateDrmDisplaySurface, same calls in the same order, translated from
// the vulkan.hpp wrappers to the C entry points (the translation made for the Cemu port in cemu-drm-lease, branch
// drm-lease-gamepad). Edits for MHGU Recomp: entry points from vkGetInstanceProcAddr, the C functions at the end.

#include <algorithm>
#include <optional>
#include <vector>

#include "drm_lease.h"
#include "drm_lease_glue.h"

namespace Frontend {

static PFN_vkGetDrmDisplayEXT vkGetDrmDisplayEXT;
static PFN_vkAcquireDrmDisplayEXT vkAcquireDrmDisplayEXT;
static PFN_vkReleaseDisplayEXT vkReleaseDisplayEXT;
static PFN_vkGetDisplayModePropertiesKHR vkGetDisplayModePropertiesKHR;
static PFN_vkGetPhysicalDeviceDisplayPropertiesKHR vkGetPhysicalDeviceDisplayPropertiesKHR;
static PFN_vkGetPhysicalDeviceDisplayPlanePropertiesKHR vkGetPhysicalDeviceDisplayPlanePropertiesKHR;
static PFN_vkGetDisplayPlaneSupportedDisplaysKHR vkGetDisplayPlaneSupportedDisplaysKHR;
static PFN_vkGetDisplayPlaneCapabilitiesKHR vkGetDisplayPlaneCapabilitiesKHR;
static PFN_vkCreateDisplayPlaneSurfaceKHR vkCreateDisplayPlaneSurfaceKHR;
static VkDisplayKHR acquired_display;

// Creates a VK_KHR_display surface on a leased DRM connector. The lease fd
// comes from the frontend (drm-lease-v1); the display is acquired through
// VK_EXT_acquire_drm_display and driven directly, bypassing the window system.
static VkSurfaceKHR CreateDrmDisplaySurface(VkInstance instance, VkPhysicalDevice physical_device, const DrmLeaseOutput& info)
{
	if (!vkGetDrmDisplayEXT || !vkAcquireDrmDisplayEXT)
	{
		LOG_ERROR(Frontend, "VK_EXT_acquire_drm_display is not supported by this driver");
		return {};
	}

	VkDisplayKHR display{};
	if (vkGetDrmDisplayEXT(physical_device, info.fd, info.connector_id, &display) != VK_SUCCESS)
	{
		LOG_ERROR(Frontend, "vkGetDrmDisplayEXT failed");
		return {};
	}
	if (vkAcquireDrmDisplayEXT(physical_device, info.fd, display) != VK_SUCCESS)
	{
		LOG_ERROR(Frontend, "vkAcquireDrmDisplayEXT failed");
		return {};
	}
	acquired_display = display;

	// The frontend sized its framebuffer from the DRM preferred mode;
	// Vulkan does not order this list, so match it explicitly.
	uint32_t mode_count = 0;
	vkGetDisplayModePropertiesKHR(physical_device, display, &mode_count, nullptr);
	std::vector<VkDisplayModePropertiesKHR> modes(mode_count);
	vkGetDisplayModePropertiesKHR(physical_device, display, &mode_count, modes.data());
	if (modes.empty())
	{
		LOG_ERROR(Frontend, "Leased display has no modes");
		return {};
	}
	const auto mode_it = std::find_if(modes.begin(), modes.end(), [&](const VkDisplayModePropertiesKHR& m) {
		return m.parameters.visibleRegion.width == info.width &&
			   m.parameters.visibleRegion.height == info.height;
	});
	if (mode_it == modes.end())
	{
		LOG_ERROR(Frontend, "Leased display has no {}x{} mode", info.width, info.height);
		return {};
	}
	const VkDisplayModePropertiesKHR mode = *mode_it;

	uint32_t display_count = 0;
	vkGetPhysicalDeviceDisplayPropertiesKHR(physical_device, &display_count, nullptr);
	std::vector<VkDisplayPropertiesKHR> displays(display_count);
	vkGetPhysicalDeviceDisplayPropertiesKHR(physical_device, &display_count, displays.data());
	const auto display_it = std::find_if(displays.begin(), displays.end(), [&](const VkDisplayPropertiesKHR& d) { return d.display == display; });
	if (display_it == displays.end() || !(display_it->supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR))
	{
		LOG_ERROR(Frontend, "Leased display does not support the identity transform");
		return {};
	}

	uint32_t plane_count = 0;
	vkGetPhysicalDeviceDisplayPlanePropertiesKHR(physical_device, &plane_count, nullptr);
	std::vector<VkDisplayPlanePropertiesKHR> planes(plane_count);
	vkGetPhysicalDeviceDisplayPlanePropertiesKHR(physical_device, &plane_count, planes.data());
	std::optional<uint32_t> plane_index;
	for (uint32_t i = 0; i < planes.size(); i++)
	{
		if (planes[i].currentDisplay && planes[i].currentDisplay != display)
			continue;
		uint32_t supported_count = 0;
		vkGetDisplayPlaneSupportedDisplaysKHR(physical_device, i, &supported_count, nullptr);
		std::vector<VkDisplayKHR> supported(supported_count);
		vkGetDisplayPlaneSupportedDisplaysKHR(physical_device, i, &supported_count, supported.data());
		if (std::find(supported.begin(), supported.end(), display) != supported.end())
		{
			plane_index = i;
			break;
		}
	}
	if (!plane_index)
	{
		LOG_ERROR(Frontend, "No display plane supports the leased display");
		return {};
	}

	VkDisplayPlaneCapabilitiesKHR plane_caps{};
	vkGetDisplayPlaneCapabilitiesKHR(physical_device, mode.displayMode, *plane_index, &plane_caps);
	VkDisplayPlaneAlphaFlagBitsKHR alpha_mode{};
	bool alpha_found = false;
	for (const auto candidate : {VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR, VK_DISPLAY_PLANE_ALPHA_GLOBAL_BIT_KHR,
								 VK_DISPLAY_PLANE_ALPHA_PER_PIXEL_BIT_KHR, VK_DISPLAY_PLANE_ALPHA_PER_PIXEL_PREMULTIPLIED_BIT_KHR})
	{
		if (plane_caps.supportedAlpha & candidate)
		{
			alpha_mode = candidate;
			alpha_found = true;
			break;
		}
	}
	if (!alpha_found)
	{
		LOG_ERROR(Frontend, "Leased display plane advertises no alpha mode");
		return {};
	}

	VkDisplaySurfaceCreateInfoKHR display_ci{};
	display_ci.sType = VK_STRUCTURE_TYPE_DISPLAY_SURFACE_CREATE_INFO_KHR;
	display_ci.displayMode = mode.displayMode;
	display_ci.planeIndex = *plane_index;
	display_ci.planeStackIndex = planes[*plane_index].currentStackIndex;
	display_ci.transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
	display_ci.globalAlpha = 1.0f;
	display_ci.alphaMode = alpha_mode;
	display_ci.imageExtent = mode.parameters.visibleRegion;

	VkSurfaceKHR surface{};
	const VkResult res = vkCreateDisplayPlaneSurfaceKHR(instance, &display_ci, nullptr, &surface);
	if (res != VK_SUCCESS)
	{
		LOG_ERROR(Frontend, "vkCreateDisplayPlaneSurfaceKHR failed: {}", static_cast<int>(res));
		return {};
	}

	LOG_INFO(Frontend, "Created DRM display surface: connector {} mode {}x{} plane {}", info.connector_id,
				mode.parameters.visibleRegion.width, mode.parameters.visibleRegion.height, *plane_index);
	return surface;
}

} // namespace Frontend

extern "C" VkSurfaceKHR drm_lease_create_surface(VkInstance instance, VkPhysicalDevice phys, const DrmLeaseOutput* output)
{
    Frontend::vkGetDrmDisplayEXT = reinterpret_cast<PFN_vkGetDrmDisplayEXT>(vkGetInstanceProcAddr(instance, "vkGetDrmDisplayEXT"));
    Frontend::vkAcquireDrmDisplayEXT = reinterpret_cast<PFN_vkAcquireDrmDisplayEXT>(vkGetInstanceProcAddr(instance, "vkAcquireDrmDisplayEXT"));
    Frontend::vkReleaseDisplayEXT = reinterpret_cast<PFN_vkReleaseDisplayEXT>(vkGetInstanceProcAddr(instance, "vkReleaseDisplayEXT"));
    Frontend::vkGetDisplayModePropertiesKHR = reinterpret_cast<PFN_vkGetDisplayModePropertiesKHR>(vkGetInstanceProcAddr(instance, "vkGetDisplayModePropertiesKHR"));
    Frontend::vkGetPhysicalDeviceDisplayPropertiesKHR = reinterpret_cast<PFN_vkGetPhysicalDeviceDisplayPropertiesKHR>(vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceDisplayPropertiesKHR"));
    Frontend::vkGetPhysicalDeviceDisplayPlanePropertiesKHR = reinterpret_cast<PFN_vkGetPhysicalDeviceDisplayPlanePropertiesKHR>(vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceDisplayPlanePropertiesKHR"));
    Frontend::vkGetDisplayPlaneSupportedDisplaysKHR = reinterpret_cast<PFN_vkGetDisplayPlaneSupportedDisplaysKHR>(vkGetInstanceProcAddr(instance, "vkGetDisplayPlaneSupportedDisplaysKHR"));
    Frontend::vkGetDisplayPlaneCapabilitiesKHR = reinterpret_cast<PFN_vkGetDisplayPlaneCapabilitiesKHR>(vkGetInstanceProcAddr(instance, "vkGetDisplayPlaneCapabilitiesKHR"));
    Frontend::vkCreateDisplayPlaneSurfaceKHR = reinterpret_cast<PFN_vkCreateDisplayPlaneSurfaceKHR>(vkGetInstanceProcAddr(instance, "vkCreateDisplayPlaneSurfaceKHR"));
    return Frontend::CreateDrmDisplaySurface(instance, phys, *output);
}

extern "C" void drm_lease_release_display(VkPhysicalDevice phys)
{
    if (Frontend::acquired_display)
        Frontend::vkReleaseDisplayEXT(phys, Frontend::acquired_display);
    Frontend::acquired_display = VK_NULL_HANDLE;
}
