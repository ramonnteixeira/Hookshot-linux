// Authored instructions only. No dump, native program, GPU or game is read.
#include <array>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>
#include <spirv/unified1/spirv.hpp>
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/environment.h"
#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"
#include "shader_recompiler/frontend/maxwell/decode.h"
#include "shader_recompiler/frontend/maxwell/translate/impl/impl.h"
#include "shader_recompiler/ir_opt/passes.h"
#include "shader_recompiler/object_pool.h"

using namespace Shader;
using Native = Ampere::Instruction;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); std::exit(1); } } while (0)

// Mesa NAK sm70_encode.rs: OpF2I base 0x105, immediate form 4,
// source bits 32..63; sign 72, destination width 75..76, NTZ 77,
// rounding 78..79, FTZ 80, source width 84..85.
static u32 mode(bool sign, u32 rounding, bool ftz, u32 dst = 2, u32 src = 2) {
    return (u32{sign} << 8) | (dst << 11) | (1U << 13) |
           (rounding << 14) | (u32{ftz} << 16) | (src << 20);
}
static Native instruction(u32 op, u32 dest, u32 source, u32 flags, u32 pred = 7) {
    return {{{op | (pred << 12) | (dest << 16), source, flags, 0}}};
}
static Ampere::LoweredProgram lower(Native i) {
    const std::array code{i, instruction(0x94d, 0, 0, 0x03800000)};
    return Ampere::LowerToMaxwell(code);
}

struct FixtureEnvironment final : Shader::Environment {
    FixtureEnvironment() { stage = Stage::Compute; }
    u64 ReadInstruction(u32) override { std::abort(); }
    u32 ReadCbufValue(u32, u32) override { return 0; }
    TextureType ReadTextureType(u32) override { return TextureType::Color2D; }
    TexturePixelFormat ReadTexturePixelFormat(u32) override { return TexturePixelFormat::R8_UNORM; }
    bool IsTexturePixelFormatInteger(u32) override { return false; }
    u32 ReadViewportTransformState() override { return 0; }
    u32 TextureBoundBuffer() const override { return 0; }
    u32 LocalMemorySize() const override { return 0; }
    u32 SharedMemorySize() const override { return 0; }
    std::array<u32, 3> WorkgroupSize() const override { return {1, 1, 1}; }
    bool HasHLEMacroState() const override { return false; }
    std::optional<ReplaceConstant> GetReplaceConstBuffer(u32, u32) override { return std::nullopt; }
    void Dump(u64, u64) override { std::abort(); }
};

// Dispatch the real decoder's results to the actual implementations used by
// this lowering. This fixture does not reimplement conversion arithmetic.
static void translate(Maxwell::TranslatorVisitor& visitor, u64 word) {
    switch (Maxwell::Decode(word)) {
    case Maxwell::Opcode::MOV32I: visitor.MOV32I(word); break;
    case Maxwell::Opcode::F2I_reg: visitor.F2I_reg(word); break;
    case Maxwell::Opcode::FSET_reg: visitor.FSET_reg(word); break;
    case Maxwell::Opcode::FMNMX_reg: visitor.FMNMX_reg(word); break;
    case Maxwell::Opcode::LOP_reg: visitor.LOP_reg(word); break;
    case Maxwell::Opcode::EXIT: break;
    default: CHECK(false);
    }
}

static unsigned modules = 0;
static void check_pipeline(const std::filesystem::path& output, u32 literal, bool sign,
                           u32 rounding, bool ftz, u32 dst_width = 2) {
    constexpr u32 dest = 13;
    const auto lowered = lower(instruction(0x905, dest, literal, mode(sign, rounding, ftz, dst_width)));
    if (!lowered) std::fprintf(stderr, "native F2I immediate: %.*s at +%u\n",
                              int(lowered.error.size()), lowered.error.data(), lowered.error_offset);
    CHECK(lowered);
    CHECK(lowered.instructions.size() > 2);
    const u64 first = lowered.instructions.front();
    CHECK(Maxwell::Decode(first) == Maxwell::Opcode::MOV32I);
    CHECK(u32(first >> 20) == literal); // all 32 bits, including former modifier positions
    const u32 scratch = first & 255;
    CHECK(scratch >= 246 && scratch < 255);
    bool converted = false;
    for (u64 word : lowered.instructions) {
        CHECK(((word >> 16) & 15) == 7);
        CHECK(Maxwell::Decode(word) != Maxwell::Opcode::F2I_imm);
        if (Maxwell::Decode(word) == Maxwell::Opcode::F2I_reg) {
            CHECK((word & 255) == dest);
            CHECK(((word >> 8) & 3) == dst_width);
            CHECK(((word >> 10) & 3) == 2);
            CHECK(((word >> 12) & 1) == u32(sign));
            CHECK(((word >> 39) & 3) == rounding);
            CHECK(((word >> 44) & 1) == u32(ftz));
            CHECK(((word >> 45) & 1) == 0 && ((word >> 49) & 1) == 0);
            if (sign) CHECK(((word >> 20) & 255) == scratch);
            converted = true;
        }
    }
    CHECK(converted);

    ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    IR::Program program;
    program.stage = Stage::Compute;
    program.workgroup_size = {1, 1, 1};
    program.shared_memory_size = 4;
    program.blocks.push_back(&block);
    program.post_order_blocks.push_back(&block);
    IR::AbstractSyntaxNode node{};
    node.type = IR::AbstractSyntaxNode::Type::Block;
    node.data.block = &block;
    program.syntax_list.push_back(node);
    IR::AbstractSyntaxNode ret{};
    ret.type = IR::AbstractSyntaxNode::Type::Return;
    program.syntax_list.push_back(ret);
    FixtureEnvironment env;
    Maxwell::TranslatorVisitor visitor{env, block};
    for (u64 word : lowered.instructions) translate(visitor, word);
    visitor.ir.WriteShared(32, visitor.ir.Imm32(0), visitor.X(static_cast<IR::Reg>(dest)));
    visitor.ir.Epilogue();
    const IR::Opcode expected_round[] = {IR::Opcode::FPRoundEven32, IR::Opcode::FPFloor32,
                                         IR::Opcode::FPCeil32, IR::Opcode::FPTrunc32};
    bool saw_round = false, saw_convert = false, saw_nan_zero = false;
    for (const IR::Inst& inst : block.Instructions()) {
        if (inst.GetOpcode() == expected_round[rounding]) {
            CHECK(inst.Flags<IR::FpControl>().fmz_mode == (ftz ? IR::FmzMode::FTZ : IR::FmzMode::None));
            saw_round = true;
        }
        saw_convert |= inst.GetOpcode() == (sign ? IR::Opcode::ConvertS32F32 : IR::Opcode::ConvertU32F32);
        if (inst.GetOpcode() == IR::Opcode::SelectU32 && !inst.Arg(0).IsImmediate() &&
            inst.Arg(0).InstRecursive()->GetOpcode() == IR::Opcode::FPIsNan32 &&
            inst.Arg(1).IsImmediate() && inst.Arg(1).U32() == 0)
            saw_nan_zero = true;
    }
    CHECK(saw_round && saw_convert);
    if (sign) CHECK(saw_nan_zero); // existing NTZ conversion explicitly selects zero for NaN
    Optimization::SsaRewritePass(program);
    Optimization::ConstantPropagationPass(env, program);
    Optimization::DeadCodeEliminationPass(program);
    Optimization::IdentityRemovalPass(program);
    Optimization::CollectShaderInfoPass(env, program);
    Profile profile{};
    profile.supported_spirv = 0x00010600;
    const auto spv = Backend::SPIRV::EmitSPIRV(profile, program);
    CHECK(spv.size() > 5);
    bool spirv_conversion = false, spirv_store = false;
    for (std::size_t at = 5; at < spv.size();) {
        const u32 count = spv[at] >> 16;
        CHECK(count && at + count <= spv.size());
        const auto op = static_cast<spv::Op>(spv[at] & 0xffff);
        spirv_conversion |= op == (sign ? spv::Op::OpConvertFToS : spv::Op::OpConvertFToU);
        spirv_store |= op == spv::Op::OpStore;
        at += count;
    }
    CHECK(spirv_conversion && spirv_store); // result survives the production cleanup passes
    const auto path = output / ("case-" + std::to_string(modules++) + ".spv");
    std::ofstream stream(path, std::ios::binary);
    stream.write(reinterpret_cast<const char*>(spv.data()), spv.size() * sizeof(u32));
    CHECK(stream.good());
}

static void check_source_controls() {
    // Existing source forms remain accepted with their original source handling.
    for (u32 op : {0x305U, 0xb05U, 0xd05U}) {
        const u32 source = op == 0xb05 ? 0x100U << 6 : 19U;
        const u32 flags = mode(true, 3, false) | (op == 0xd05 ? 1U << 27 : 0);
        const auto result = lower(instruction(op, 13, source, flags));
        CHECK(result);
        bool converted = false;
        for (u64 word : result.instructions) converted |= Maxwell::Decode(word) == Maxwell::Opcode::F2I_reg;
        CHECK(converted);
        u64 hash = 14695981039346656037ULL;
        for (u64 word : result.instructions) hash = (hash ^ word) * 1099511628211ULL;
        std::printf("control %03x: %016llx\n", op, static_cast<unsigned long long>(hash));
    }
    // Existing register F16 support is preserved; adding an immediate does not
    // silently add half-precision, double-precision, 64-bit output or non-NTZ modes.
    CHECK(lower(instruction(0x305, 13, 19, mode(true, 3, false, 2, 1))));
}

static void check_controls() {
    check_source_controls();
    CHECK(!lower(instruction(0x905, 13, 0x3f800000, mode(true, 3, false, 2, 1))));
    CHECK(!lower(instruction(0x905, 13, 0x3f800000, mode(true, 3, false, 2, 3))));
    CHECK(!lower(instruction(0x905, 12, 0x3f800000, mode(false, 3, true, 3))));
    CHECK(!lower(instruction(0x911, 12, 0x3f800000, mode(false, 3, true, 3))));
    CHECK(!lower(instruction(0x905, 13, 0x3f800000, mode(true, 3, false) & ~(1U << 13))));
    CHECK(!lower(instruction(0x905, 13, 0x3f800000, mode(true, 3, false) | 1U)));
    auto unused_source = instruction(0x905, 13, 0x3f800000, mode(true, 3, false));
    unused_source.words[0] |= 1U << 24;
    CHECK(!lower(unused_source));
    // The materialization and conversion keep ordinary and inverted guards.
    for (bool sign : {false, true})
        for (u32 pred : {0U, 10U, 7U}) {
            const auto result = lower(instruction(0x905, 13, 0xc1540001, mode(sign, 3, false), pred));
            CHECK(result);
            for (std::size_t n = 0; n + 1 < result.instructions.size(); ++n)
                CHECK(((result.instructions[n] >> 16) & 15) == pred);
        }
    const auto skipped = lower(instruction(0x905, 13, 0xffffffff, 0xffffffff, 15));
    CHECK(skipped && skipped.instructions.size() == 1);
    CHECK(Maxwell::Decode(skipped.instructions.front()) == Maxwell::Opcode::EXIT);
}

int main(int argc, char** argv) {
    CHECK(argc == 2);
    if (std::string(argv[1]) == "--existing-controls") {
        check_source_controls();
        return 0;
    }
    const std::filesystem::path output{argv[1]};
    std::filesystem::create_directories(output);
    // Bit-casts from authored finite values plus chosen zeros, subnormals,
    // infinities and NaN payloads; none originate in a program dump.
    const std::array literals{std::bit_cast<u32>(13.75f), std::bit_cast<u32>(-9.25f),
        0U, 0x80000000U, 1U, 0x80000001U, 0x7f800000U, 0xff800000U, 0x7fc12345U, 0xffa54321U};
    for (bool sign : {false, true})
        for (u32 rounding = 0; rounding < 4; ++rounding)
            for (bool ftz : {false, true})
                check_pipeline(output, literals[(u32{sign} * 8 + rounding * 2 + u32{ftz}) % literals.size()],
                               sign, rounding, ftz);
    for (u32 literal : literals) check_pipeline(output, literal, true, 3, true);
    check_pipeline(output, std::bit_cast<u32>(32768.5f), false, 0, true, 1);
    check_controls();
    std::printf("PASS: %u authored native F2I immediate -> Maxwell -> IR -> SPIR-V cases; source/mode/predicate controls pass\n", modules);
}
