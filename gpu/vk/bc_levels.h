/* Block-compressed levels whose block grid is not level 0's halved (vk_texture.cpp, AMD). */
#pragma once

#include <algorithm>
#include <cstdint>

namespace mkwvk::bc_levels {

/// The 4 x 4 blocks across `texels`
inline uint32_t blocks(uint32_t texels) {
    return (texels + 3) / 4;
}

/// The first level (from 1) whose block grid is not level 0's halved, or `levels` when every one is: 1365 texels are
/// 342 blocks, but level 2 (341 texels) is 86 blocks, not 342 >> 2 = 85
inline uint32_t first_uneven(uint32_t width, uint32_t height, uint32_t levels) {
    for (uint32_t l = 1; l < levels; l++)
        if (blocks(std::max(1u, width >> l)) != std::max(1u, blocks(width) >> l) ||
            blocks(std::max(1u, height >> l)) != std::max(1u, blocks(height) >> l))
            return l;
    return levels;
}

/// The nearest size from `texels` up whose blocks halve evenly down to its levels of 16 texels or more:
/// 1365 -> 1536, 683 -> 768; 768 and 1536 stay
inline uint32_t even_size(uint32_t texels, uint32_t levels) {
    uint32_t k = 0;
    while (k < levels && (texels >> k) >= 16) k++;
    const uint32_t m = 4u << (k ? k - 1 : 0);
    return (texels + m - 1) / m * m;
}

} // namespace mkwvk::bc_levels
