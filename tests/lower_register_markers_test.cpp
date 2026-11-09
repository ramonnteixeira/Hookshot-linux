// Synthetic native instructions built from encoding fields, not copied from a game program.
// Check the effective FFMA operands after interpreting the lowering's REGMAP prefixes.
//   python -I -B tests/test_lower_register_markers.py
#include <array>
#include <cstdio>
#include <vector>
#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"

using Shader::Ampere::Instruction;

namespace {
constexpr Instruction kExit{{0x794d, 0, 0x03800000, 0}};

Instruction fma(u32 dst, u32 a, u32 b, u32 c) {
    // @PT FFMA.FTZ Rd, Ra, Rb, Rc; register source form, round-to-nearest.
    return Instruction{{0x7223U | (dst << 16) | (a << 24), b, c | 0x10000U, 0}};
}

bool check(u32 dst, u32 a, u32 b, u32 c, bool uniform) {
    std::vector<Instruction> code;
    // Referencing UR5 reserves the emulated uniform bank, so native GPRs >=216 are extended.
    // Without a uniform reference, GPRs 216..245 are ordinary GPRs and still cannot be markers.
    if (uniform) code.push_back(Instruction{{0x7c02U, 5, 0x08000f00U, 0}}); // MOV R0, UR5
    code.push_back(fma(dst, a, b, c));
    code.push_back(kExit);
    const auto lowered = Shader::Ampere::LowerToMaxwell(code);
    if (!lowered) {
        std::fprintf(stderr, "register markers: lowering failed: %.*s\n",
                     int(lowered.error.size()), lowered.error.data());
        return false;
    }
    std::array<u32, 256> mapping{};
    for (u64 word : lowered.instructions) {
        if ((word >> 51) == (0xefe0ULL >> 3)) {
            const u32 runs = (word >> 40) & 3;
            for (u32 i = 0; i < runs && i < 2; ++i) {
                const u32 number = (word >> (i ? 20 : 0)) & 0xff;
                const u32 target = (word >> (i ? 28 : 8)) & 0xff;
                const u32 length = ((word >> (36 + 2 * i)) & 3) + 1;
                for (u32 k = 0; k < length; ++k) mapping[number + k] = 256 + target + k;
            }
            continue;
        }
        if ((word & 0xff80000000000000ULL) == 0x5980000000000000ULL) {
            const auto reg = [&](u32 shift) {
                const u32 number = (word >> shift) & 0xff;
                return mapping[number] ? mapping[number] : number;
            };
            const auto expected = [&](u32 native) {
                return native >= (uniform ? 216U : 246U) ? native + 256 : native;
            };
            const std::array<u32, 4> got{reg(0), reg(8), reg(20), reg(39)};
            const std::array<u32, 4> want{expected(dst), expected(a), expected(b), expected(c)};
            if (got != want) {
                std::fprintf(stderr,
                    "register markers: FFMA %u,%u,%u,%u (uniform=%d) became %u,%u,%u,%u; expected %u,%u,%u,%u\n",
                    dst, a, b, c, int(uniform), got[0], got[1], got[2], got[3], want[0], want[1], want[2], want[3]);
                return false;
            }
            return true;
        }
        mapping.fill(0); // Prefixes apply to the next non-prefix instruction only.
    }
    std::fprintf(stderr, "register markers: missing FFMA\n");
    return false;
}
} // namespace

int main() {
    u32 cases = 0, failures = 0;
    for (u32 source = 216; source < 238; ++source) {
        for (bool uniform : {false, true}) {
            const u32 dst = uniform ? 238 : 248;
            failures += !check(dst, 17, source, dst, uniform);
            ++cases;
            failures += !check(dst, source, 17, dst, uniform);
            ++cases;
        }
    }
    // Two occupied marker-bank blocks, with a repeated destination/addend that must remain shared.
    failures += !check(248, 220, 216, 248, false);
    ++cases;
    std::printf("register markers: %u cases, %u failures\n", cases, failures);
    return failures ? 1 : 0;
}
