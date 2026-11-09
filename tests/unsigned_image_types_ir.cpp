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
 bool integer; unsigned queries=0;
 explicit TestEnv(bool i):integer(i){stage=Stage::Compute;}
 u64 ReadInstruction(u32)override{return 0;}
 u32 ReadCbufValue(u32,u32)override{return 1;}
 TextureType ReadTextureType(u32)override{return TextureType::Color2D;}
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

struct Case {const char* name; ImageFormat format; int op; bool buffer=false;};
int main(int argc,char**argv){try{
 if(argc!=2)return 2;std::filesystem::create_directories(argv[1]);
 const Case cases[]={
 {"r32-read",ImageFormat::R32_UINT,1},
 {"r32-write",ImageFormat::R32_UINT,2},
 {"buffer-r32-write",ImageFormat::R32_UINT,2,true},
 {"atomic-r32-add",ImageFormat::R32_UINT,3},
 {"atomic-r32-smin",ImageFormat::R32_UINT,4},
 {"typeless-read",ImageFormat::Typeless,1},
 {"typeless-write",ImageFormat::Typeless,2}};
 for(const auto& c:cases)for(bool integer:{false,true}){
 ObjectPool<IR::Inst> pool;IR::Block b{pool};IR::Program p;p.stage=Stage::Compute;p.workgroup_size={1,1,1};p.blocks.push_back(&b);p.post_order_blocks.push_back(&b);
 IR::AbstractSyntaxNode body{};body.type=IR::AbstractSyntaxNode::Type::Block;body.data.block=&b;p.syntax_list.push_back(body);
 IR::AbstractSyntaxNode ret{};ret.type=IR::AbstractSyntaxNode::Type::Return;p.syntax_list.push_back(ret);
 IR::IREmitter ir{b};IR::TextureInstInfo f{};f.type.Assign(c.buffer?TextureType::Buffer:TextureType::Color2D);f.image_format.Assign(c.format);
 {auto xy=c.buffer?IR::Value{ir.Imm32(0u)}:ir.CompositeConstruct(ir.Imm32(0u),ir.Imm32(0u));
 if(c.op==1)(void)ir.ImageRead(ir.Imm32(0u),xy,f);
 if(c.op==2)ir.ImageWrite(ir.Imm32(0u),xy,ir.CompositeConstruct(ir.Imm32(1u),ir.Imm32(2u),ir.Imm32(3u),ir.Imm32(4u)),f);
 if(c.op==3){p.info.uses_atomic_image_u32=true;(void)ir.ImageAtomicIAdd(ir.Imm32(0u),xy,ir.Imm32(1u),f);}
 if(c.op==4){p.info.uses_atomic_image_u32=true;(void)ir.ImageAtomicSMin(ir.Imm32(0u),xy,ir.Imm32(1u),f);}
 }
 ir.Epilogue();TestEnv env(integer);Optimization::TexturePass(env,p,HostTranslateInfo{}); Optimization::CollectShaderInfoPass(env,p);
 Profile profile{};profile.supported_spirv=0x00010600;profile.support_int64=true;profile.support_typeless_image_loads=true;
 auto words=Backend::SPIRV::EmitSPIRV(profile,p);auto name=std::string(c.name)+(integer?"-int":"-float");
 std::ofstream out(std::filesystem::path(argv[1])/(name+".spv"),std::ios::binary);out.write((const char*)words.data(),words.size()*4);
 fprintf(stderr,"%s questions=%u\n",name.c_str(),env.queries);
 }
 return 0;}catch(const std::exception&e){fprintf(stderr,"exception: %s\n",e.what());return 3;}}

