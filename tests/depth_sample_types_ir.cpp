// SPDX-License-Identifier: GPL-3.0-or-later
// Real Eden IR/pass/emitter regression; no game or GPU.
#include <filesystem>
#include <fstream>
#include <string>
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/frontend/ir/ir_emitter.h"
#include "shader_recompiler/object_pool.h"
#include "shader_recompiler/environment.h"
#include "shader_recompiler/host_translate_info.h"
#include "shader_recompiler/ir_opt/passes.h"
using namespace Shader;
struct TestEnv:Environment {
 bool integer; TextureType type; unsigned queries=0;
 TestEnv(bool i, TextureType t):integer(i),type(t){stage=Stage::Compute;}
 u64 ReadInstruction(u32)override{return 0;}
 u32 ReadCbufValue(u32,u32)override{return 1;}
 TextureType ReadTextureType(u32)override{return type;}
 TexturePixelFormat ReadTexturePixelFormat(u32)override{return {};}
 bool IsTexturePixelFormatInteger(u32)override{queries++;return integer;}
 u32 ReadViewportTransformState()override{return 1;}
 u32 TextureBoundBuffer()const override{return 0;}
 u32 LocalMemorySize()const override{return 0;}
 u32 SharedMemorySize()const override{return 0;}
 std::array<u32,3> WorkgroupSize()const override{return {1,1,1};}
 bool HasHLEMacroState()const override{return false;}
 std::optional<ReplaceConstant> GetReplaceConstBuffer(u32,u32)override{return {};}
 void Dump(u64,u64)override{}
};

int main(int argc, char** argv) {
    try {
        if (argc != 2) return 2;
        std::filesystem::create_directories(argv[1]);
        for (bool array : {false, true}) for (bool integer : {false, true}) {
            ObjectPool<IR::Inst> pool;
            IR::Block block{pool};
            IR::Program program;
            program.stage = Stage::Compute;
            program.workgroup_size = {1, 1, 1};
            program.blocks.push_back(&block);
            program.post_order_blocks.push_back(&block);
            IR::AbstractSyntaxNode body{};
            body.type = IR::AbstractSyntaxNode::Type::Block;
            body.data.block = &block;
            program.syntax_list.push_back(body);
            IR::AbstractSyntaxNode ret{};
            ret.type = IR::AbstractSyntaxNode::Type::Return;
            program.syntax_list.push_back(ret);
            IR::IREmitter ir{block};
            const auto type = array ? TextureType::ColorArray2D : TextureType::Color2D;
            IR::TextureInstInfo flags{};
            flags.type.Assign(type);
            flags.is_depth.Assign(1);
            const auto coords = array
                ? ir.CompositeConstruct(ir.Imm32(.5f), ir.Imm32(.5f), ir.Imm32(0.f))
                : ir.CompositeConstruct(ir.Imm32(.5f), ir.Imm32(.5f));
            (void)ir.ImageSampleDrefExplicitLod(ir.Imm32(0u), coords, ir.Imm32(.5f),
                                             ir.Imm32(0.f), IR::Value{}, flags);
            ir.Epilogue();
            TestEnv env(integer, type);
            Optimization::TexturePass(env, program, HostTranslateInfo{});
            Optimization::CollectShaderInfoPass(env, program);
            Profile profile{};
            profile.supported_spirv = 0x00010600;
            profile.support_int64 = true;
            auto words = Backend::SPIRV::EmitSPIRV(profile, program);
            const auto name = std::string(array ? "array" : "2d") + (integer ? "-integer" : "-float");
            std::ofstream file(std::filesystem::path(argv[1]) / (name + ".spv"), std::ios::binary);
            file.write(reinterpret_cast<const char*>(words.data()), words.size() * sizeof(u32));
            if (!file) return 3;
            std::printf("%s integer-queries=%u\n", name.c_str(), env.queries);
        }
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s\n", e.what());
        return 1;
    }
}

