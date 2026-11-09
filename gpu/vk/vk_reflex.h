// NVIDIA Reflex through VK_NV_low_latency2 (vk_reflex.cpp): the driver paces the game's frames so that the CPU starts one
// as late as it can and the frame waits less in the queue, for less delay from input to the screen.
#pragma once

#include <vector>

#include "vk_internal.h"

namespace mkwvk {

/// The extensions it needs (VK_NV_low_latency2, VK_KHR_present_id), added to `want` when the device has them
void reflex_device_extensions(std::vector<const char*>& want, const std::vector<VkExtensionProperties>& all);
/// Its feature (presentId) into the device's feature chain (*tail), before the features are queried
void reflex_device_features(void**& tail);
/// After the device is made: whether Reflex can run (the feature there), its functions loaded
void reflex_device_created();
/// Reflex can run on this device (VK_NV_low_latency2 and its functions): the Insert menu's row offers it only then
bool reflex_available();
/// Into a swapchain's creation: latency mode on while the row is on, nothing while it is off (the chained struct lives until
/// the next call)
void reflex_swapchain_info(VkSwapchainCreateInfoKHR& ci);
/// The row was turned on or off since the swapchain was made: it is to be made again (present_image)
bool reflex_swapchain_stale();
/// A swapchain made: its sleep mode set as the menu has it
void reflex_swapchain_created(VkSwapchainKHR swapchain);
/// The frame's markers, at the game's present: its simulation ended and its rendering's submission starts
void reflex_present_begins();
/// Before the present's submit: the rendering's submission ended
void reflex_submit_ends();
/// The present: a VkPresentIdKHR for VkPresentInfoKHR's pNext (or null), and the present's start marker
const void* reflex_present_id(const void* next);
void reflex_presented();
/// After the present: the driver's sleep until the next frame should start, then that frame's start markers
void reflex_sleep();

} // namespace mkwvk
