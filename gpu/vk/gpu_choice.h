/* Which GPU the Insert menu's GPU device row asks for (vk_main.cpp's pick_device). RX_VK_DEVICE, when set, wins over it
   there. */
#pragma once

namespace mkwvk::gpu_choice {

/// The row's choice as a position in the Vulkan 1.3 devices (0-based), or -1 for Auto. menu: opts.gpu_device (0 Auto,
/// n the n-th usable device); usable: how many there are now. A choice past them (a GPU taken out since) is Auto.
inline int menu_device(int menu, int usable) { return menu > 0 && menu <= usable ? menu - 1 : -1; }

} // namespace mkwvk::gpu_choice
