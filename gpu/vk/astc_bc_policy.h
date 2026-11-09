// Which cards keep every large decoded ASTC texture as BC3, the finer blocks (UI, characters) too (vk_texture.cpp).
// tests/test_astc_bc_policy.py
#pragma once
#include <cstdint>

namespace astc_bc_policy {

// The VRAM the driver allows the process, in bytes: under 8.5 GB, every large ASTC texture as BC3 (a quarter of RGBA8).
// Issue 519 set it under 6 GB (an RTX 3050 Laptop, 3.3 GB allowed: 1.75 GB of decoded ASTC in Goron City, paged, 4 fps).
// 8 GB cards are allowed ~7-7.6 GB and fill up the same way: an RX 7700S (7.1 GB allowed) held 2.5 GB of decoded ASTC in
// Hyrule Castle Town's market with the VRAM at the limit; as BC3, 1.3 GB. 10 GB cards and up keep the finer ones as RGBA8
inline bool all_by_vram(uint64_t allowed) {
    return allowed && allowed < (8704ull << 20);
}

} // namespace astc_bc_policy
