// SPDX-License-Identifier: GPL-3.0-or-later
// Synthetic real TexturePass/Collect/Emit fixture, adapted from depth_sample_types_ir.cpp.
#include <filesystem>
#include <fstream>
#include <unordered_set>
#include <unordered_map>
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/frontend/ir/ir_emitter.h"
#include "shader_recompiler/object_pool.h"
#include "shader_recompiler/environment.h"
#include "shader_recompiler/host_translate_info.h"
#include "shader_recompiler/ir_opt/passes.h"
using namespace Shader;
struct TestEnv : Environment {
 TestEnv(){stage=Stage::Fragment;}
 u64 ReadInstruction(u32)override{return 0;}
 u32 ReadCbufValue(u32,u32)override{return 1;}
 TextureType ReadTextureType(u32)override{return TextureType::Color2D;}
 TexturePixelFormat ReadTexturePixelFormat(u32)override{return {};}
 bool IsTexturePixelFormatInteger(u32)override{return false;}
 u32 ReadViewportTransformState()override{return 1;}
 u32 TextureBoundBuffer()const override{return 0;}
 u32 LocalMemorySize()const override{return 0;}
 u32 SharedMemorySize()const override{return 0;}
 std::array<u32,3> WorkgroupSize()const override{return {1,1,1};}
 bool HasHLEMacroState()const override{return false;}
 std::optional<ReplaceConstant> GetReplaceConstBuffer(u32,u32)override{return {};}
 void Dump(u64,u64)override{}
};
struct Location { const IR::Block* block;size_t position; };
using Locations=std::unordered_map<const IR::Inst*,Location>;
// Every fixture graph is acyclic: a definition dominates a use only on all incoming paths.
static bool dominates(const IR::Block* definition,const IR::Block* use,const IR::Block* entry){
 if(definition==use)return true;if(use==entry || use->ImmPredecessors().empty())return false;
 for(const auto* predecessor:use->ImmPredecessors())if(!dominates(definition,predecessor,entry))return false;
 return true;
}
static bool audit(IR::Value value,Location use,const Locations& where,const IR::Block* entry){
 const auto* inst=value.TryInstRecursive();if(!inst)return true;
 const auto definition=where.at(inst);
 if(!dominates(definition.block,use.block,entry) || (definition.block==use.block && definition.position>=use.position))return false;
 for(size_t i=0;i<inst->NumArgs();++i){
  const auto edge=inst->GetOpcode()==IR::Opcode::Phi?Location{inst->PhiBlock(i),size_t(-1)}:definition;
  if(!audit(inst->Arg(i),edge,where,entry))return false;
 }
 return true;
}
static u32 evaluate(IR::Value v,bool taken,const IR::Block* entry){
 if(v.IsImmediate())return v.U32();const auto* n=v.InstRecursive();
 auto arg=[&](size_t i){return evaluate(n->Arg(i),taken,entry);};
 switch(n->GetOpcode()){
 case IR::Opcode::Phi:for(size_t i=0;i<n->NumArgs();++i)if((n->PhiBlock(i)!=entry)==taken)return arg(i);throw std::runtime_error("Phi edge");
 case IR::Opcode::Identity:return arg(0);
 case IR::Opcode::GetCbufU32:{const auto at=arg(1);if(at==0x100)return 1;if(at==0x108)return 2;if(at==0x110)return taken?1:0;throw std::runtime_error("unexpected cbuf address");}
 case IR::Opcode::IAdd32:return arg(0)+arg(1);
 case IR::Opcode::BitwiseAnd32:return arg(0)&arg(1);
 case IR::Opcode::ShiftLeftLogical32:return arg(0)<<arg(1);
 case IR::Opcode::ShiftRightLogical32:return arg(0)>>arg(1);
 case IR::Opcode::UMin32:return std::min(arg(0),arg(1));
 default:throw std::runtime_error("unexpected index evaluation opcode");
 }
}
static void block_node(IR::Program& p,IR::Block& b){IR::AbstractSyntaxNode n{};n.type=IR::AbstractSyntaxNode::Type::Block;n.data.block=&b;p.syntax_list.push_back(n);}
static IR::U32 offset(IR::IREmitter& ir,u32 word){return ir.ShiftLeftLogical(ir.BitwiseAnd(ir.GetCbuf(ir.Imm32(0u),ir.Imm32(word)),ir.Imm32(3u)),ir.Imm32(3u));}
static void emit(const std::filesystem::path& out,int kind){
 const bool diamond=kind!=0;const bool conditional=kind==3;
 const char* name=kind==0?"same-block":kind==1?"diamond":kind==2?"offset-phi":"repeated-condition";
 ObjectPool<IR::Inst> pool;
 IR::Block entry{pool},body{pool},merge{pool},later{pool},end{pool};IR::Program p;p.stage=Stage::Fragment;
 IR::IREmitter a{entry};
 const auto first=offset(a,0x100);
 const auto initial=a.GetCbuf(a.Imm32(0u),a.IAdd(a.Imm32(0x200u),first));
 IR::Value handle=initial;
 if(diamond){
  const auto cond=a.ConditionRef(a.INotEqual(a.GetCbuf(a.Imm32(0u),a.Imm32(0x110u)),a.Imm32(0u)));
  entry.AddBranch(&body);entry.AddBranch(&merge);body.AddBranch(&merge);
  IR::IREmitter b{body};const auto second=offset(b,0x108);
  const auto replacement=b.GetCbuf(b.Imm32(0u),b.IAdd(b.Imm32(0x200u),second));
  auto phi=merge.PrependNewInst(merge.begin(),IR::Opcode::Phi);phi->SetFlags(IR::Type::U32);
  const IR::Value first_value=kind!=2?IR::Value{initial}:IR::Value{first};
  const IR::Value second_value=kind!=2?IR::Value{replacement}:IR::Value{second};
  phi->AddPhiOperand(&entry,first_value);phi->AddPhiOperand(&body,second_value);handle=IR::Value{&*phi};
  if(phi->PhiBlock(0)!=&entry || phi->PhiBlock(1)!=&body || phi->Arg(0)!=first_value || phi->Arg(1)!=second_value)throw std::runtime_error("input Phi semantics");
  if(kind==2){IR::IREmitter m{merge};handle=m.GetCbuf(m.Imm32(0u),m.IAdd(m.Imm32(0x200u),IR::U32{handle}));}
  p.blocks={&entry,&body,&merge};p.post_order_blocks={&merge,&body,&entry};
  block_node(p,entry);IR::AbstractSyntaxNode n{};n.type=IR::AbstractSyntaxNode::Type::If;n.data.if_node={cond,&body,&merge};p.syntax_list.push_back(n);
  block_node(p,body);n={};n.type=IR::AbstractSyntaxNode::Type::EndIf;n.data.end_if.merge=&merge;p.syntax_list.push_back(n);block_node(p,merge);
  if(conditional){
   // Same predicate, through a second ConditionRef. The real CFG still permits entry->merge->later.
   IR::IREmitter m{merge};const auto repeated=m.ConditionRef(cond);
   merge.AddBranch(&later);merge.AddBranch(&end);later.AddBranch(&end);
   n={};n.type=IR::AbstractSyntaxNode::Type::If;n.data.if_node={repeated,&later,&end};p.syntax_list.push_back(n);block_node(p,later);
   p.blocks.push_back(&later);p.blocks.push_back(&end);p.post_order_blocks={&end,&later,&merge,&body,&entry};
  }
 }else{p.blocks={&entry};p.post_order_blocks={&entry};block_node(p,entry);}
 IR::IREmitter sample{conditional?later:diamond?merge:entry};IR::TextureInstInfo flags{};flags.type.Assign(TextureType::Color2D);flags.is_depth.Assign(1);
 const auto value=sample.ImageSampleDrefExplicitLod(handle,sample.CompositeConstruct(sample.Imm32(.5f),sample.Imm32(.5f)),sample.Imm32(.5f),sample.Imm32(0.f),IR::Value{},flags);
 auto* instruction=value.InstRecursive();sample.SetFragColor(0,0,value);
 if(conditional){IR::AbstractSyntaxNode n{};n.type=IR::AbstractSyntaxNode::Type::EndIf;n.data.end_if.merge=&end;p.syntax_list.push_back(n);block_node(p,end);IR::IREmitter finish{end};finish.Epilogue();}else sample.Epilogue();
 IR::AbstractSyntaxNode ret{};ret.type=IR::AbstractSyntaxNode::Type::Return;p.syntax_list.push_back(ret);
 TestEnv env;HostTranslateInfo host{};host.ApplyDescriptorLimitPolicy();
 Optimization::TexturePass(env,p,host);
 Locations where;for(const auto* block:p.blocks){size_t i=0;for(const auto& inst:block->Instructions())where.emplace(&inst,Location{block,i++});}
 const bool safe=audit(instruction->Arg(0),where.at(instruction),where,&entry);
 bool retained=instruction->GetOpcode()==IR::Opcode::ImageSampleDrefExplicitLod && instruction->UseCount()>0;
 bool output=false;for(const auto* block:p.blocks)for(const auto& inst:block->Instructions())if(inst.GetOpcode()==IR::Opcode::SetFragColor && inst.Arg(2).InstRecursive()==instruction)output=true;
 if(!retained || !output)throw std::runtime_error("sample/output removed or opcode changed");
 // A false predicate skips the repeated-condition sample. Do not assert a value for that unreachable use.
 const auto index_false=conditional?0u:evaluate(instruction->Arg(0),false,&entry),index_true=evaluate(instruction->Arg(0),true,&entry);
 const bool semantics=(conditional || index_false==1) && index_true==(diamond?2u:1u);
 std::printf("%s IR_DOMINANCE=%s SELECTION=%s false=%s%u true=%u sample-use=%zu output=1\n",name,safe?"PASS":"FAIL",semantics?"PASS":"FAIL",conditional?"not-sampled:":"",index_false,index_true,size_t(instruction->UseCount()));
 Optimization::CollectShaderInfoPass(env,p);Profile profile{};profile.supported_spirv=0x00010600;profile.support_int64=true;
 const auto words=Backend::SPIRV::EmitSPIRV(profile,p);
 std::ofstream file(out/(std::string(name)+".spv"),std::ios::binary);file.write(reinterpret_cast<const char*>(words.data()),words.size()*sizeof(u32));if(!file)throw std::runtime_error("write");
}
int main(int argc,char**argv){try{if(argc!=2)return 2;std::filesystem::create_directories(argv[1]);for(int kind=0;kind<4;++kind)emit(argv[1],kind);return 0;}catch(const std::exception&e){std::fprintf(stderr,"%s\n",e.what());return 1;}}
