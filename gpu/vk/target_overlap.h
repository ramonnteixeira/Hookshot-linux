/* Render targets drawn over from another start (vk_texture.cpp's eviction): a frame's transient targets that the game
   makes in a scratch heap whose offsets shift from frame to frame. */
#pragma once

#include <algorithm>
#include <cstdint>

namespace mkwvk::target_overlap {

/// Whether a render target's storage [lo, hi), last used at frame `last_used`, was drawn over by another render target
/// [olo, ohi) last written at `o_written`: one that starts elsewhere (the same start is drawn_over's own case), is not
/// inside it (a view of a part of it), covers an eighth of it or more, and was written after its last use
inline bool shifted_over(uint64_t lo, uint64_t hi, uint64_t last_used, uint64_t olo, uint64_t ohi, uint64_t o_written) {
    if (hi <= lo || ohi <= olo || olo == lo || o_written <= last_used) return false;
    if (olo >= lo && ohi <= hi) return false;
    const uint64_t a = std::max(lo, olo), b = std::min(hi, ohi);
    return b > a && (b - a) * 8 >= hi - lo;
}

} // namespace mkwvk::target_overlap
