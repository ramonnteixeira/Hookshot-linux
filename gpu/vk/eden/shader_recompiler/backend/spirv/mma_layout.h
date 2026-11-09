// (hookshot) The device's element order of the 16x8x16 f16 cooperative matrices HMMA_nx2 runs on.
//
// A lane's HMMA fragment registers hold its matrix elements in the console's order (PTX mma.m16n8k16's: two f16
// a register, low half first). The host's cooperative matrices hold each lane's share in an order of the
// implementation's own; vk_coopmat.cpp measures it at start, and only when every lane holds the same positions in
// both does the back end turn HMMA_nx2 on. Element e of the console's fragment is then the device's element
// mma_order_x[e].
#pragma once

#include <array>
#include <cstdint>

namespace Shader::Backend::SPIRV {

inline std::array<uint32_t, 8> mma_order_a{0, 1, 2, 3, 4, 5, 6, 7};
inline std::array<uint32_t, 4> mma_order_b{0, 1, 2, 3};
inline std::array<uint32_t, 4> mma_order_c{0, 1, 2, 3};
inline bool mma_software = false;   // (HMMA_nx2 in software: emit_spirv_warp.cpp, RX_VK_MMA_SOFTWARE=1)
// (amd-fix) float colour outputs carry no NaN or infinity to their target (EmitSetFragColor): one NaN texel spreads
// through bloom/blur/downsample averages into glowing blocks (OoT's flowers on AMD, Hookshot issue 606). On for every
// card but NVIDIA (vk_coopmat.cpp coopmat_init); RX_VK_NO_NAN_GUARD=1 turns it off. Idea from NX2navi patch 0027.
inline bool frag_nan_guard = false;
// (amd-fix) gl_Position decorated Invariant (vertex and mesh outputs): the game draws a depth pre-pass, then the colour
// pass again with depth EQUAL; Maxwell computes the same position in both, AMD's compiler may not without Invariant, and
// the colour pass is then rejected in spots: holes showing the bright sky, blown up by bloom (the glowing flowers).
// On for every card but NVIDIA (vk_coopmat.cpp); RX_VK_NO_INVARIANT=1 off. From NX2navi patch 0007.
inline bool position_invariant = false;

} // namespace Shader::Backend::SPIRV
