// How long a texture the GPU drew, unused, stays before the eviction pass lets it go (vk_texture.cpp).
// tests/test_keep_rendered.py
#pragma once
#include <cstdint>

namespace keep_rendered {

// Presents unused: 3600 (a minute; a course left behind goes, the game draws it again when it comes back), 600 near what
// the driver allows the process. "Near" is the budget's own line, 97% of it: AMD's driver reports the process's use of the
// device heap at most at what it allows (7244 of 7244 MB while VMA held 7861 MB of blocks), so "past what it allows" never
// held on AMD and the rendered textures stayed 3600 presents with the VRAM full and the driver paging. over_only: as before
inline uint64_t presents(bool known, uint64_t used, uint64_t allowed, bool over_only) {
    if (!known) return 3600;
    const bool near_limit = over_only ? used > allowed : used >= allowed / 100 * 97;
    return near_limit ? 600 : 3600;
}

} // namespace keep_rendered
