// Independently authored encoding fields; no shader dump or game program is used.
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"

using Shader::Ampere::Instruction;
using Shader::Ampere::LoweredProgram;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); std::exit(1); } } while (0)

static void field(Instruction& i, unsigned at, unsigned count, uint32_t value) {
    CHECK(count <= 32 && (count == 32 || value < (uint64_t{1} << count)));
    for (unsigned bit = 0; bit < count; ++bit) {
        auto& word = i.words[(at + bit) / 32];
        const uint32_t mask = uint32_t{1} << ((at + bit) % 32);
        word = (word & ~mask) | ((value >> bit & 1) ? mask : 0);
    }
}
static Instruction op(uint32_t code, uint32_t pred = 7) {
    Instruction i{};
    field(i, 0, 12, code); field(i, 12, 4, pred);
    return i;
}
static Instruction suatom(uint32_t d = 13, uint32_t a = 9, uint32_t b = 17,
                          uint32_t bank = 3, uint32_t offset = 0x24, uint32_t pred = 7) {
    auto i = op(0x593, pred);
    field(i, 16, 8, d); field(i, 24, 8, a); field(i, 32, 8, b);
    field(i, 40, 14, offset); field(i, 54, 5, bank);
    field(i, 77, 4, 7); // Strong(GPU), not acquire/release
    field(i, 81, 3, 7); // no fault-predicate destination
    field(i, 84, 3, 1); // normal eviction; U32 ADD, 1D, sample addressing, IGN are zero
    return i;
}
static Instruction exit_op() {
    auto i = op(0x94d); field(i, 87, 3, 7); return i;
}
static LoweredProgram lower(Instruction i) {
    return Shader::Ampere::LowerToMaxwell(std::vector<Instruction>{i, exit_op()});
}
static uint64_t address(unsigned index) { return 0x50 + index * 8 + (index + 1) / 3 * 8; }

static void lowering_checks() {
    const auto r = lower(suatom());
    CHECK(r);
    CHECK(r.required_texture_buffer == 3 && r.instructions.size() == 2);
    const uint64_t w = r.instructions[0];
    CHECK(w >> 55 == 0xea00000000000000ULL >> 55);
    CHECK((w & 255) == 13 && (w >> 8 & 255) == 9 && (w >> 20 & 255) == 17);
    CHECK((w >> 36 & 0x1fff) == 0x24 && (w >> 54 & 1) == 0);
    CHECK((w >> 29 & 15) == 0 && (w >> 33 & 7) == 0 && (w >> 49 & 31) == 0);
    for (uint32_t p : {2U, 10U, 7U}) {
        const auto predicated = lower(suatom(13, 9, 17, 3, 0x24, p));
        CHECK(predicated && (predicated.instructions[0] >> 16 & 15) == p);
    }
    CHECK(lower(suatom(255, 255, 255, 17, 0x1fff)));
    for (auto [at, count, value] : std::vector<std::array<uint32_t, 3>>{
             {0,12,0x793}, {0,12,0xf93}, {54,5,18}, {40,14,0x2000},
             {59,2,1}, {61,3,1}, {64,8,1}, {72,1,1}, {73,3,1}, {76,1,1},
             {77,4,0}, {77,4,5}, {77,4,10}, {81,3,2}, {84,3,0}, {87,4,1},
             {91,1,1}, {95,1,1}, {96,1,1}, {104,1,1}, {126,1,1}, {127,1,1}}) {
        auto bad = suatom(); field(bad, at, count, value);
        const auto refused = lower(bad);
        CHECK(!refused && refused.instructions.empty() && refused.error_offset == 0);
    }
    auto scheduled = suatom(); field(scheduled, 105, 21, 0x155555);
    CHECK(lower(scheduled).instructions == r.instructions);
    const auto same = Shader::Ampere::LowerToMaxwell(std::vector<Instruction>{suatom(), suatom(7), exit_op()});
    CHECK(same && same.required_texture_buffer == 3);
    const auto conflict = Shader::Ampere::LowerToMaxwell(std::vector<Instruction>{suatom(), suatom(7,9,17,2), exit_op()});
    CHECK(!conflict && conflict.error == "native surface descriptor buffer conflict" && conflict.error_offset == 16);
    // The addend alone is high: it must participate in REGMAP discovery, and has scalar width.
    const auto high = lower(suatom(13, 9, 250));
    CHECK(high && high.register_map_words > 0);
    CHECK(Shader::Ampere::RenameableFields(0x593) == 7);
    CHECK(Shader::Ampere::FieldSpan(0x593, 0) == 1 && Shader::Ampere::FieldSpan(0x593, 1) == 1 &&
          Shader::Ampere::FieldSpan(0x593, 2) == 1 && Shader::Ampere::FieldSpan(0x593, 3) == 0);
    // A conditional BRA crosses an instruction expanded by high-register mapping.
    auto branch = op(0x947, 2); field(branch, 32, 32, 16); field(branch, 87, 3, 7);
    const auto relocated = Shader::Ampere::LowerToMaxwell(std::vector<Instruction>{branch, suatom(250,250,250), exit_op()});
    CHECK(relocated && relocated.register_map_words > 0);
    const auto delta = static_cast<int32_t>((relocated.instructions[0] >> 20 & 0xffffff) << 8) >> 8;
    CHECK(address(0) + 8 + delta == address(relocated.source_instruction_offsets[2]));
    // Explicit MEMBAR.SC.GPU remains a separate instruction; SUATOM adds no fences.
    auto barrier = op(0x992); field(barrier, 76, 3, 2);
    const auto barriers = Shader::Ampere::LowerToMaxwell(std::vector<Instruction>{barrier, suatom(), barrier, exit_op()});
    CHECK(barriers && barriers.instructions.size() == 4);
    CHECK(barriers.instructions[0] >> 48 == 0xef98 && barriers.instructions[2] == barriers.instructions[0]);
    std::puts("PASS: direct SUATOM fields, predicates, scalar REGMAP, relocation, descriptor bounds/conflicts and mode refusals");
}

#ifndef SUATOM_LOWERING_ONLY
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <spirv/unified1/spirv.hpp>
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/frontend/maxwell/translate_program.h"
#include "shader_recompiler/host_translate_info.h"

struct Environment final : Shader::Environment {
    std::vector<uint64_t> code;
    uint32_t bank, offset;
    Environment(const LoweredProgram& lowered, uint32_t o) : bank(lowered.required_texture_buffer), offset(o) {
        stage = Shader::Stage::Compute;
        code.resize(0x50 / 8);
        for (auto word : lowered.instructions) {
            if (code.size() % 4 == 0) code.push_back(0);
            code.push_back(word);
        }
    }
    uint64_t ReadInstruction(uint32_t at) override { CHECK(at / 8 < code.size()); return code[at/8]; }
    uint32_t ReadCbufValue(uint32_t b, uint32_t o) override {
        CHECK(b == bank && o == offset); return 0x1234;
    }
    Shader::TextureType ReadTextureType(uint32_t h) override { CHECK(h == 0x1234); return Shader::TextureType::Color1D; }
    Shader::TexturePixelFormat ReadTexturePixelFormat(uint32_t) override { return Shader::TexturePixelFormat::R32_UINT; }
    bool IsTexturePixelFormatInteger(uint32_t h) override { CHECK(h == 0x1234); return true; }
    uint32_t ReadViewportTransformState() override { return 0; }
    uint32_t TextureBoundBuffer() const override { return bank; }
    uint32_t LocalMemorySize() const override { return 0; }
    uint32_t SharedMemorySize() const override { return 4; }
    std::array<uint32_t,3> WorkgroupSize() const override { return {32,1,1}; }
    bool HasHLEMacroState() const override { return false; }
    std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(uint32_t,uint32_t) override { return std::nullopt; }
    void Dump(uint64_t,uint64_t) override {}
};
static Instruction move(uint32_t reg, uint32_t value) {
    auto i = op(0x802); field(i,16,8,reg); field(i,32,32,value); field(i,72,4,15); return i;
}
static void module_check(const std::filesystem::path& directory, const char* name,
                         uint32_t d, uint32_t a, uint32_t b, uint32_t pred = 7,
                         bool branch = false, bool explicit_barriers = false) {
    std::vector<Instruction> source;
    if (d == 239) {
        auto uniform = op(0x882); field(uniform,32,32,42); source.push_back(uniform);
    }
    // Initialize old destination, coordinate and addend, respecting input aliases.
    source.push_back(move(d, 99)); source.push_back(move(a, 3));
    if (b != a) source.push_back(move(b, 11));
    if (pred != 7 || branch) {
        auto tid = op(0x919); field(tid,16,8,1); field(tid,72,8,33); source.push_back(tid);
        auto compare = op(0x20c); field(compare,24,8,1); field(compare,32,8,255);
        field(compare,76,3,5); field(compare,81,3,2); field(compare,84,3,7); field(compare,87,3,7);
        source.push_back(compare); // P2 = threadIdx.x != 0, so its value is not constant
    }
    if (branch) {
        auto jump = op(0x947,2); field(jump,32,32,16); field(jump,87,3,7); source.push_back(jump);
    }
    auto barrier = op(0x992); field(barrier,76,3,2);
    if (explicit_barriers) source.push_back(barrier);
    source.push_back(suatom(d,a,b,3,0x24,pred));
    if (explicit_barriers) source.push_back(barrier);
    // A side effect consumes the old value returned by the actual SUATOM translation.
    auto store = op(0x388); field(store,24,8,255); field(store,32,8,d); field(store,73,3,4);
    source.push_back(store); source.push_back(exit_op());
    const auto lowered = Shader::Ampere::LowerToMaxwell(source); CHECK(lowered);
    if (d == 239) CHECK(lowered.register_map_words > 0);
    Environment env(lowered,0x90);
    Shader::ObjectPool<Shader::IR::Inst> insts;
    Shader::ObjectPool<Shader::IR::Block> blocks;
    Shader::ObjectPool<Shader::Maxwell::Flow::Block> flow_blocks;
    Shader::Maxwell::Flow::CFG cfg{env,flow_blocks,0x50};
    Shader::HostTranslateInfo host{}; host.support_int64 = host.support_float64 = host.support_float16 = true;
    host.support_conditional_barrier = true;
    auto program = Shader::Maxwell::TranslateProgram(insts,blocks,env,cfg,host);
    CHECK(program.info.image_descriptors.size() == 1);
    const auto& descriptor = program.info.image_descriptors[0];
    CHECK(descriptor.cbuf_index == 3 && descriptor.cbuf_offset == 0x90 && descriptor.is_read && descriptor.is_written);
    CHECK(descriptor.format == Shader::ImageFormat::R32_UINT && descriptor.type == Shader::TextureType::Color1D);
    Shader::Profile profile{}; profile.supported_spirv = 0x00010600;
    profile.support_int64 = true; profile.support_descriptor_aliasing = true;
    const auto spirv = Shader::Backend::SPIRV::EmitSPIRV(profile,program);
    std::map<uint32_t,uint32_t> constants;
    std::map<uint32_t,std::vector<uint32_t>> phis;
    std::vector<uint32_t> stored;
    uint32_t atomic = 0, atomic_count = 0, barriers = 0, selects = 0;
    bool texel_pointer = false;
    for (size_t at = 5; at < spirv.size();) {
        const uint32_t count = spirv[at] >> 16; CHECK(count && at + count <= spirv.size());
        const auto opcode = static_cast<spv::Op>(spirv[at] & 0xffff);
        const auto* w = &spirv[at+1];
        if (opcode == spv::Op::OpConstant && count == 4) constants[w[1]] = w[2];
        if (opcode == spv::Op::OpImageTexelPointer) {
            CHECK(constants.at(w[3]) == 3); texel_pointer = true;
        }
        if (opcode == spv::Op::OpAtomicIAdd) {
            CHECK(count == 7 && constants.at(w[3]) == uint32_t(spv::ScopeDevice) && constants.at(w[4]) == 0);
            CHECK(constants.at(w[5]) == (a == b ? 3U : 11U));
            atomic = w[1]; ++atomic_count;
        }
        if (opcode == spv::Op::OpMemoryBarrier) {
            CHECK(constants.at(w[0]) == uint32_t(spv::ScopeDevice));
            CHECK(constants.at(w[1]) & uint32_t(spv::MemorySemanticsImageMemoryMask));
            CHECK(constants.at(w[1]) & uint32_t(spv::MemorySemanticsAcquireReleaseMask));
            ++barriers;
        }
        if (opcode == spv::Op::OpSelectionMerge) ++selects;
        if (opcode == spv::Op::OpPhi) {
            for (uint32_t n = 2; n + 1 < count - 1; n += 2) phis[w[1]].push_back(w[n]);
        }
        if (opcode == spv::Op::OpStore) stored.push_back(w[1]);
        at += count;
    }
    CHECK(atomic_count == 1 && texel_pointer);
    CHECK(barriers == (explicit_barriers ? 2U : 0U));
    CHECK((pred == 7 && !branch) || selects > 0);
    std::set<uint32_t> values{atomic};
    bool changed;
    do {
        changed = false;
        for (const auto& [result,inputs] : phis)
            for (auto input : inputs) if (values.contains(input)) changed |= values.insert(result).second;
    } while (changed);
    bool consumed = false;
    for (auto value : stored) consumed |= values.contains(value);
    CHECK(consumed == (d != 255)); // discarding the result must still retain the atomic side effect
    std::filesystem::create_directories(directory);
    std::ofstream out(directory / (std::string(name) + ".spv"),std::ios::binary);
    out.write(reinterpret_cast<const char*>(spirv.data()),static_cast<std::streamsize>(spirv.size()*4)); CHECK(out.good());
    std::printf("PASS: %s actual lowering/Maxwell/IR/SPIR-V, descriptor and atomic result contract\n",name);
}
#endif

int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--expect-original-rejection") {
        const auto r = lower(suatom());
        CHECK(!r && r.error == "unsupported native opcode" && r.error_offset == 0);
        std::puts("PASS: original production lowering rejects the authored direct SUATOM"); return 0;
    }
    lowering_checks();
#ifndef SUATOM_LOWERING_ONLY
    CHECK(argc == 2);
    const std::filesystem::path directory{argv[1]};
    module_check(directory,"separate",13,9,17);
    module_check(directory,"dest-coordinate-alias",9,9,17);
    module_check(directory,"dest-addend-alias",17,9,17);
    module_check(directory,"all-alias",9,9,9);
    module_check(directory,"high-addend",13,9,250);
    module_check(directory,"high-all-alias",250,250,250);
    module_check(directory,"reserved239-alias",239,239,239);
    module_check(directory,"predicate",13,9,17,2);
    module_check(directory,"inverted-predicate",13,9,17,10);
    module_check(directory,"relocated-branch",250,250,250,7,true);
    module_check(directory,"explicit-barriers",13,9,17,7,false,true);
    module_check(directory,"discard-old-value",255,9,17);
#endif
}
