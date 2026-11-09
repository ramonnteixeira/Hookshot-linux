// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Switch 2 (NVN) task and mesh programs. They talk to the hardware through the stage buffer
// (ISBE): a task program writes the number of mesh workgroups at byte 4 of its output region and
// its payload after it; each mesh workgroup reads that region (its own index at byte 0) and
// writes its vertices and primitives to an attribute region in groups of 32, one 32-word line per
// attribute, plus the primitive count and 8-bit vertex indices to a map region. Here the task's
// output region is the task payload, the mesh output regions live in workgroup memory, and the
// epilogue (reached by every invocation, see structured_control_flow.cpp) hands them to Vulkan.

#include <algorithm>
#include <cstdlib>
#include <span>

#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/backend/spirv/emit_spirv_instructions.h"
#include "shader_recompiler/backend/spirv/spirv_emit_context.h"

namespace Shader::Backend::SPIRV {
namespace {
void Barrier(EmitContext& ctx) {
    ctx.OpControlBarrier(ctx.Const(static_cast<u32>(spv::Scope::Workgroup)),
                         ctx.Const(static_cast<u32>(spv::Scope::Workgroup)),
                         ctx.Const(static_cast<u32>(spv::MemorySemanticsMask::AcquireRelease |
                                                    spv::MemorySemanticsMask::WorkgroupMemory)));
}

Id LocalInvocation(EmitContext& ctx) {
    return ctx.OpCompositeExtract(ctx.U32[1], ctx.OpLoad(ctx.U32[3], ctx.local_invocation_id), 0U);
}

Id Clamped(EmitContext& ctx, Id index, u32 count) {
    return ctx.OpUMin(ctx.U32[1], index, ctx.Const(count - 1));
}

Id PushU64(EmitContext& ctx, u32 member);
Id WorkgroupId(EmitContext& ctx);
Id BufferWordAt(EmitContext& ctx, Id base, Id word);
Id BufferWord(EmitContext& ctx, u32 member, Id word);

Id PayloadWord(EmitContext& ctx, Id index) {
    if (ctx.mesh_fallback && ctx.stage == Stage::Mesh && ctx.runtime_info.mesh_fallback_task) {
        // (the task wrote it out: the mesh workgroups are a later dispatch, one payload each)
        const Id slot{WorkgroupId(ctx)};
        const Id first{ctx.OpIAdd(
            ctx.U32[1], ctx.Const(MESH_FALLBACK_CAPACITY),
            ctx.OpIMul(ctx.U32[1], slot, ctx.Const(ctx.mesh_info.payload_words)))};
        return BufferWordAt(ctx, PushU64(ctx, MESH_FALLBACK_PAYLOAD_PUSH_OFFSET),
                            ctx.OpIAdd(ctx.U32[1], first,
                                       Clamped(ctx, index, ctx.mesh_info.payload_words)));
    }
    return ctx.OpAccessChain(ctx.mesh_payload_u32, ctx.mesh_payload,
                             Clamped(ctx, index, ctx.mesh_info.payload_words));
}

Id SharedWord(EmitContext& ctx, Id array, Id index) {
    return ctx.OpAccessChain(ctx.mesh_shared_u32, array, index);
}

// Runs body under `if (condition)`.
template <typename Func>
void If(EmitContext& ctx, Id condition, Func&& body) {
    const Id then_label{ctx.OpLabel()};
    const Id merge_label{ctx.OpLabel()};
    ctx.OpSelectionMerge(merge_label, spv::SelectionControlMask::MaskNone);
    ctx.OpBranchConditional(condition, then_label, merge_label);
    ctx.AddLabel(then_label);
    body();
    ctx.OpBranch(merge_label);
    ctx.AddLabel(merge_label);
}

// Copies the lines of element `index` (a vertex or a primitive) from the attribute region.
void CopyLines(EmitContext& ctx, Id index, u32 base, u32 num_lines,
               const std::vector<EmitContext::MeshLineTarget>& targets) {
    // Word of line 0: base + (index / 32) * num_lines * 32 + index % 32.
    const Id group{ctx.OpShiftRightLogical(ctx.U32[1], index, ctx.Const(5U))};
    const Id slot{ctx.OpBitwiseAnd(ctx.U32[1], index, ctx.Const(31U))};
    const Id first{ctx.OpIAdd(ctx.U32[1],
                              ctx.OpIMul(ctx.U32[1], group, ctx.Const(num_lines * 32)),
                              ctx.OpIAdd(ctx.U32[1], slot, ctx.Const(base)))};
    for (u32 line = 0; line < num_lines; ++line) {
        const EmitContext::MeshLineTarget& target{targets[line]};
        if (!Sirit::ValidId(target.variable)) {
            continue;
        }
        const Id word{ctx.OpLoad(
            ctx.U32[1],
            SharedWord(ctx, ctx.mesh_attributes, ctx.OpIAdd(ctx.U32[1], first, ctx.Const(line * 32))))};
        const Id value{target.is_float ? ctx.OpBitcast(ctx.F32[1], word) : word};
        const Id pointer_type{target.is_float ? ctx.output_f32 : ctx.output_u32};
        const Id pointer{target.element == ~0U
                             ? ctx.OpAccessChain(pointer_type, target.variable, index)
                             : ctx.OpAccessChain(pointer_type, target.variable, index,
                                                 ctx.Const(target.element))};
        ctx.OpStore(pointer, value);
    }
}

// OpenGL-style clip space: z = (z + w) / 2, from the stage buffer's position lines.
void ConvertDepth(EmitContext& ctx, Id vertex) {
    const auto& lines{ctx.mesh_info.vertex_lines};
    const auto z_line{std::ranges::find(lines, static_cast<u32>(IR::Attribute::PositionZ))};
    const auto w_line{std::ranges::find(lines, static_cast<u32>(IR::Attribute::PositionW))};
    if (z_line == lines.end() || w_line == lines.end()) {
        return;
    }
    const u32 num_lines{static_cast<u32>(lines.size())};
    const Id group{ctx.OpShiftRightLogical(ctx.U32[1], vertex, ctx.Const(5U))};
    const Id slot{ctx.OpBitwiseAnd(ctx.U32[1], vertex, ctx.Const(31U))};
    const Id first{ctx.OpIAdd(ctx.U32[1], ctx.OpIMul(ctx.U32[1], group, ctx.Const(num_lines * 32)),
                              slot)};
    const auto read = [&](auto line) {
        const u32 offset{static_cast<u32>(line - lines.begin()) * 32};
        return ctx.OpBitcast(ctx.F32[1],
                             ctx.OpLoad(ctx.U32[1], SharedWord(ctx, ctx.mesh_attributes,
                                                               ctx.OpIAdd(ctx.U32[1], first,
                                                                          ctx.Const(offset)))));
    };
    const Id z{ctx.OpFMul(ctx.F32[1], ctx.OpFAdd(ctx.F32[1], read(z_line), read(w_line)),
                          ctx.Const(0.5f))};
    ctx.OpStore(ctx.OpAccessChain(ctx.output_f32, ctx.output_position, vertex, ctx.Const(2U)), z);
}

/// (hookshot) The fallback's records: one per primitive corner, its validity word and the stage buffer's vertex and
/// primitive lines. A mesh workgroup writes its workgroup index and a fixed number of records for it; the generated
/// vertex stage reads one record per vertex.
u32 FallbackRecordWords(const MeshInfo& mesh) {
    return 1 + static_cast<u32>(mesh.vertex_lines.size()) +
           static_cast<u32>(mesh.primitive_lines.size());
}

Id PushU64(EmitContext& ctx, u32 member) {
    const Id pointer_type{ctx.TypePointer(spv::StorageClass::PushConstant, ctx.U64)};
    return ctx.OpLoad(
        ctx.U64, ctx.OpAccessChain(pointer_type, ctx.rescaling_push_constants, ctx.Const(member)));
}

/// The word at a device address.
Id BufferWordAt(EmitContext& ctx, Id base, Id word) {
    const Id address{
        ctx.OpIAdd(ctx.U64, base,
                   ctx.OpUConvert(ctx.U64, ctx.OpIMul(ctx.U32[1], word, ctx.Const(4U))))};
    return ctx.MeshFallbackWord(address);
}

/// The word at a pushed device address.
Id BufferWord(EmitContext& ctx, u32 member, Id word) {
    return BufferWordAt(ctx, PushU64(ctx, member), word);
}

Id WorkgroupId(EmitContext& ctx) {
    return ctx.OpCompositeExtract(ctx.U32[1], ctx.OpLoad(ctx.U32[3], ctx.workgroup_id), 0U);
}

/// Runs body once for each index invocation, invocation + threads, ... below count.
template <typename Func>
void Strided(EmitContext& ctx, u32 count, u32 threads, Id invocation, Func&& body) {
    const Id header{ctx.OpLabel()};
    const Id continue_block{ctx.OpLabel()};
    const Id merge_block{ctx.OpLabel()};
    const Id index_var{ctx.AddLocalVariable(
        ctx.TypePointer(spv::StorageClass::Function, ctx.U32[1]), spv::StorageClass::Function)};
    ctx.OpStore(index_var, invocation);
    ctx.OpBranch(header);
    ctx.AddLabel(header);
    ctx.OpLoopMerge(merge_block, continue_block, spv::LoopControlMask::MaskNone);
    ctx.OpBranch(continue_block);
    ctx.AddLabel(continue_block);
    const Id index{ctx.OpLoad(ctx.U32[1], index_var)};
    If(ctx, ctx.OpULessThan(ctx.U1, index, ctx.Const(count)), [&] { body(index); });
    const Id next{ctx.OpIAdd(ctx.U32[1], index, ctx.Const(threads))};
    ctx.OpStore(index_var, next);
    ctx.OpBranchConditional(ctx.OpULessThan(ctx.U1, next, ctx.Const(count)), header, merge_block);
    ctx.AddLabel(merge_block);
}

/// The raw stage buffer word of line `line` for element `element` (a vertex or a primitive).
Id LineWord(EmitContext& ctx, Id array, Id element, u32 base, u32 num_lines, u32 line) {
    const Id group{ctx.OpShiftRightLogical(ctx.U32[1], element, ctx.Const(5U))};
    const Id slot{ctx.OpBitwiseAnd(ctx.U32[1], element, ctx.Const(31U))};
    const Id first{ctx.OpIAdd(
        ctx.U32[1], ctx.OpIMul(ctx.U32[1], group, ctx.Const(num_lines * 32)),
        ctx.OpIAdd(ctx.U32[1], slot, ctx.Const(base)))};
    return ctx.OpLoad(
        ctx.U32[1],
        SharedWord(ctx, array, ctx.OpIAdd(ctx.U32[1], first, ctx.Const(line * 32))));
}

/// A mesh workgroup writes its primitive count (a task's flag first bounds it) and its primitives' corners.
void MeshFallbackEpilogue(EmitContext& ctx) {
    const MeshInfo& mesh{ctx.mesh_info};
    const u32 corners{mesh.Corners()};
    const u32 words{FallbackRecordWords(mesh)};
    const u32 per_slot{mesh.max_primitives * corners};
    Barrier(ctx);
    const Id slot{WorkgroupId(ctx)};
    If(ctx, ctx.OpULessThan(ctx.U1, slot, ctx.Const(MESH_FALLBACK_CAPACITY)), [&] {
        Id count{ctx.OpUMin(ctx.U32[1], ctx.OpLoad(ctx.U32[1], ctx.mesh_primitive_count),
                            ctx.Const(mesh.max_primitives))};
        if (ctx.runtime_info.mesh_fallback_task) {
            // (a task thread starts at most one mesh workgroup: the slots it did not start output nothing)
            const Id started{ctx.OpLoad(
                ctx.U32[1], BufferWordAt(ctx, PushU64(ctx, MESH_FALLBACK_PAYLOAD_PUSH_OFFSET), slot))};
            count = ctx.OpSelect(ctx.U32[1], ctx.OpIEqual(ctx.U1, started, ctx.u32_zero_value),
                                 ctx.u32_zero_value, count);
        }
        If(ctx, ctx.OpIEqual(ctx.U1, LocalInvocation(ctx), ctx.u32_zero_value), [&] {
            ctx.OpStore(BufferWord(ctx, ctx.mesh_fallback_records_member_index, slot), count);
        });
        const Id invocation{LocalInvocation(ctx)};
        Strided(ctx, mesh.max_primitives, ctx.mesh_threads, invocation, [&](Id primitive) {
            If(ctx, ctx.OpULessThan(ctx.U1, primitive, count), [&] {
                for (u32 corner = 0; corner < corners; ++corner) {
                    const Id byte{ctx.OpIAdd(
                        ctx.U32[1], ctx.OpIMul(ctx.U32[1], primitive, ctx.Const(corners)),
                        ctx.Const(4U + corner))};
                    // (an index byte the program left unwritten is held to the vertex count)
                    const Id vertex{Clamped(
                        ctx, ctx.OpLoad(ctx.U32[1], SharedWord(ctx, ctx.mesh_map, byte)),
                        mesh.max_vertices)};
                    const Id record{ctx.OpIAdd(
                        ctx.U32[1], ctx.OpIMul(ctx.U32[1], slot, ctx.Const(per_slot)),
                        ctx.OpIAdd(ctx.U32[1],
                                   ctx.OpIMul(ctx.U32[1], primitive, ctx.Const(corners)),
                                   ctx.Const(corner)))};
                    const Id word{ctx.OpIAdd(ctx.U32[1], ctx.Const(MESH_FALLBACK_CAPACITY),
                                             ctx.OpIMul(ctx.U32[1], record, ctx.Const(words)))};
                    ctx.OpStore(BufferWord(ctx, ctx.mesh_fallback_records_member_index, word),
                                ctx.Const(1U));
                    for (u32 line = 0; line < mesh.vertex_lines.size(); ++line) {
                        const Id value{LineWord(ctx, ctx.mesh_attributes, vertex, 0,
                                                static_cast<u32>(mesh.vertex_lines.size()), line)};
                        ctx.OpStore(BufferWord(ctx, ctx.mesh_fallback_records_member_index,
                                               ctx.OpIAdd(ctx.U32[1], word,
                                                          ctx.Const(1U + line))),
                                    value);
                    }
                    for (u32 line = 0; line < mesh.primitive_lines.size(); ++line) {
                        const Id value{LineWord(ctx, ctx.mesh_attributes, primitive,
                                                ctx.mesh_primitive_base,
                                                static_cast<u32>(mesh.primitive_lines.size()), line)};
                        ctx.OpStore(
                            BufferWord(ctx, ctx.mesh_fallback_records_member_index,
                                       ctx.OpIAdd(ctx.U32[1], word,
                                                  ctx.Const(
                                                      static_cast<u32>(1U + mesh.vertex_lines.size()) + line))),
                            value);
                    }
                }
            });
        });
    });
}

void StorePosition(EmitContext& ctx, u32 element, Id value) {
    ctx.OpStore(ctx.OpAccessChain(ctx.output_f32, ctx.output_position, ctx.Const(element)), value);
}

void StoreFallbackOutput(EmitContext& ctx, IR::Attribute attribute, Id value) {
    if (IR::IsGeneric(attribute)) {
        const u32 index{IR::GenericAttributeIndex(attribute)};
        const u32 element{IR::GenericAttributeElement(attribute)};
        const GenericElementInfo& info{ctx.output_generics.at(index).at(element)};
        if (!Sirit::ValidId(info.id)) {
            return;
        }
        if (info.num_components == 1) {
            ctx.OpStore(info.id, value);
        } else {
            ctx.OpStore(ctx.OpAccessChain(ctx.output_f32, info.id,
                                          ctx.Const(element - info.first_element)),
                        value);
        }
        return;
    }
    if (attribute == IR::Attribute::PointSize && Sirit::ValidId(ctx.output_point_size)) {
        ctx.OpStore(ctx.output_point_size, value);
    }
}

u32 task_cap(u32 threads) {   // (RX_VK_TASK_CAP=<n>: mesh workgroups a task may start; 0: one a thread)
    static const char* e = getenv("RX_VK_TASK_CAP");
    const u32 n = e ? static_cast<u32>(atoi(e)) : 0U;
    return n ? n : threads;
}

void TaskEpilogue(EmitContext& ctx) {
    Barrier(ctx);
    // (hookshot) at most one mesh workgroup a task thread: each thread keeps or culls one meshlet (the programs compact them by subgroup
    // ballot). It was 65535, and a payload word a task left unwritten is anything: when the task culled everything, tens of thousands of
    // mesh workgroups for a draw held the GPU past Windows' two seconds, and the device was lost, on about half the first starts
    const Id count{ctx.OpUMin(ctx.U32[1], ctx.OpLoad(ctx.U32[1], PayloadWord(ctx, ctx.Const(1U))),
                              ctx.Const(task_cap(ctx.mesh_threads)))};
    if (ctx.mesh_fallback) {
        // (no OpEmitMeshTasksEXT: the mesh workgroups are a fixed dispatch; each thread that started one writes its
        // payload to that workgroup's slot, so the mesh dispatch reads it back)
        const Id slot{ctx.OpIAdd(ctx.U32[1],
                                 ctx.OpIMul(ctx.U32[1], WorkgroupId(ctx), ctx.Const(ctx.mesh_threads)),
                                 LocalInvocation(ctx))};
        // (the count is how many meshlets this task workgroup started: the first that many threads, not all of them.
        // Native launches one mesh workgroup a started meshlet; the fallback dispatches a fixed task_threads a workgroup
        // and the rest stay culled.)
        const Id started{ctx.OpULessThan(ctx.U1, LocalInvocation(ctx), count)};
        If(ctx, ctx.OpULessThan(ctx.U1, slot, ctx.Const(MESH_FALLBACK_CAPACITY)), [&] {
            ctx.OpStore(BufferWordAt(ctx, PushU64(ctx, MESH_FALLBACK_PAYLOAD_PUSH_OFFSET), slot),
                        started);
            If(ctx, started, [&] {
                const Id first{ctx.OpIAdd(
                    ctx.U32[1], ctx.Const(MESH_FALLBACK_CAPACITY),
                    ctx.OpIMul(ctx.U32[1], slot, ctx.Const(ctx.mesh_info.payload_words)))};
                const Id invocation{LocalInvocation(ctx)};
                Strided(ctx, ctx.mesh_info.payload_words, ctx.mesh_threads, invocation, [&](Id index) {
                    ctx.OpStore(BufferWordAt(ctx, PushU64(ctx, MESH_FALLBACK_PAYLOAD_PUSH_OFFSET),
                                             ctx.OpIAdd(ctx.U32[1], first, index)),
                                ctx.OpLoad(ctx.U32[1], PayloadWord(ctx, index)));
                });
            });
        });
        return;
    }
    const Id one{ctx.Const(1U)};
    // Marker for OpEmitMeshTasksEXT count, 1, 1, payload (PatchMeshInstructions).
    ctx.OpStore(count, one);
    ctx.OpStore(one, ctx.mesh_payload);
    ctx.mesh_tasks_marker = {count, one, one, ctx.mesh_payload};
}

void MeshEpilogue(EmitContext& ctx) {
    const MeshInfo& mesh{ctx.mesh_info};
    if (ctx.mesh_fallback) {
        MeshFallbackEpilogue(ctx);
        return;
    }
    Barrier(ctx);
    const Id count{ctx.OpUMin(ctx.U32[1], ctx.OpLoad(ctx.U32[1], ctx.mesh_primitive_count),
                              ctx.Const(mesh.max_primitives))};
    const Id max_vertices{ctx.Const(mesh.max_vertices)};
    // Marker for OpSetMeshOutputsEXT max_vertices, count (PatchMeshInstructions).
    ctx.OpStore(max_vertices, count);
    ctx.mesh_outputs_marker = {max_vertices, count};

    const Id invocation{LocalInvocation(ctx)};
    const u32 threads{ctx.mesh_threads};
    const bool convert_depth{ctx.runtime_info.convert_depth_mode && !ctx.profile.support_native_ndc &&
                             Sirit::ValidId(ctx.output_position)};
    const u32 vertex_lines{static_cast<u32>(mesh.vertex_lines.size())};
    for (u32 first = 0; first < mesh.max_vertices; first += threads) {
        const Id vertex{ctx.OpIAdd(ctx.U32[1], invocation, ctx.Const(first))};
        const auto copy = [&] {
            for (const auto& d : ctx.mesh_vertex_defaults) {
                for (u32 c = 0; c < 4; ++c) {
                    if (d.missing & (1U << c)) {
                        ctx.OpStore(ctx.OpAccessChain(ctx.output_f32, d.variable, vertex, ctx.Const(c)),
                                    ctx.Const(c == 3 ? 1.0f : 0.0f));
                    }
                }
            }
            CopyLines(ctx, vertex, 0, vertex_lines, ctx.mesh_vertex_targets);
            if (convert_depth) {
                ConvertDepth(ctx, vertex);
            }
        };
        if (first + threads > mesh.max_vertices) {
            If(ctx, ctx.OpULessThan(ctx.U1, vertex, max_vertices), copy);
        } else {
            copy();
        }
    }
    const u32 primitive_lines{static_cast<u32>(mesh.primitive_lines.size())};
    for (u32 first = 0; first < mesh.max_primitives; first += threads) {
        const Id primitive{ctx.OpIAdd(ctx.U32[1], invocation, ctx.Const(first))};
        If(ctx, ctx.OpULessThan(ctx.U1, primitive, count), [&] {
            // Vertex indices: map bytes 4 + 3 * primitive (lines: 4 + 2 * primitive, points: 4 + primitive).
            const u32 corners{mesh.Corners()};
            const Id byte{ctx.OpIAdd(ctx.U32[1], ctx.OpIMul(ctx.U32[1], primitive, ctx.Const(corners)),
                                     ctx.Const(4U))};
            std::array<Id, 3> indices;
            for (u32 element = 0; element < corners; ++element) {
                const Id address{ctx.OpIAdd(ctx.U32[1], byte, ctx.Const(element))};
                // (hookshot) within the vertices: an index byte the program left unwritten is anything, and an index
                // past the vertex count is undefined (a device fault on NVIDIA)
                indices[element] = Clamped(ctx, ctx.OpLoad(ctx.U32[1], SharedWord(ctx, ctx.mesh_map, address)),
                                           mesh.max_vertices);
            }
            const Id output_indices{ctx.TypePointer(spv::StorageClass::Output, ctx.U32[corners])};
            ctx.OpStore(ctx.OpAccessChain(output_indices, ctx.mesh_primitive_indices, primitive),
                        corners == 1 ? indices[0]   // (a point's index is a scalar)
                                     : ctx.OpCompositeConstruct(ctx.U32[corners],
                                                                std::span<const Id>(indices.data(), corners)));
            CopyLines(ctx, primitive, ctx.mesh_primitive_base, primitive_lines,
                      ctx.mesh_primitive_targets);
        });
    }
}
} // Anonymous namespace

void EmitMeshFallbackVertexBody(EmitContext& ctx) {
    // (the fragment stage's inputs for the mesh program's per-primitive generics are Flat: the generated vertex
    // stage's outputs must match, and DefineOutputs only knows the stores, not which of them are per-primitive)
    for (u32 index = 0; index < IR::NUM_GENERICS; ++index) {
        if (!ctx.runtime_info.per_primitive_generics.test(index)) {
            continue;
        }
        for (u32 element = 0; element < 4; ++element) {
            const Id id{ctx.output_generics[index][element].id};
            if (!Sirit::ValidId(id)) {
                continue;
            }
            bool seen{};
            for (u32 before = 0; before < element; ++before) {
                seen |= ctx.output_generics[index][before].id.value == id.value;
            }
            if (!seen) {
                ctx.Decorate(id, spv::Decoration::Flat);
            }
        }
    }
    const MeshInfo& mesh{ctx.mesh_info};
    const u32 corners{mesh.Corners()};
    const u32 vertex_lines{static_cast<u32>(mesh.vertex_lines.size())};
    const u32 primitive_lines{static_cast<u32>(mesh.primitive_lines.size())};
    const u32 words{FallbackRecordWords(mesh)};
    const u32 per_slot{mesh.max_primitives * corners};
    const Id vertex{ctx.OpLoad(ctx.U32[1], ctx.vertex_index)};
    const Id slot{ctx.OpUDiv(ctx.U32[1], vertex, ctx.Const(per_slot))};
    const Id within{ctx.OpUMod(ctx.U32[1], vertex, ctx.Const(per_slot))};
    const Id primitive{ctx.OpUDiv(ctx.U32[1], within, ctx.Const(corners))};
    const Id corner{ctx.OpUMod(ctx.U32[1], within, ctx.Const(corners))};
    const Id count{ctx.OpLoad(
        ctx.U32[1], BufferWord(ctx, ctx.mesh_fallback_records_member_index, slot))};
    const Id record{ctx.OpIAdd(ctx.U32[1], ctx.Const(MESH_FALLBACK_CAPACITY),
                               ctx.OpIMul(ctx.U32[1],
                                          ctx.OpIAdd(ctx.U32[1],
                                                     ctx.OpIMul(ctx.U32[1], slot, ctx.Const(per_slot)),
                                                     ctx.OpIAdd(ctx.U32[1],
                                                                ctx.OpIMul(ctx.U32[1], primitive,
                                                                           ctx.Const(corners)),
                                                                corner)),
                                          ctx.Const(words)))};
    const Id first{record};
    const Id valid{ctx.OpLoad(
        ctx.U32[1], BufferWord(ctx, ctx.mesh_fallback_records_member_index, first))};
    const auto read = [&](u32 word) {
        return ctx.OpBitcast(
            ctx.F32[1],
            ctx.OpLoad(ctx.U32[1], BufferWord(ctx, ctx.mesh_fallback_records_member_index,
                                              ctx.OpIAdd(ctx.U32[1], first, ctx.Const(word)))));
    };
    std::array<Id, 4> position{};
    for (u32 line = 0; line < vertex_lines; ++line) {
        const IR::Attribute attribute{mesh.vertex_lines[line]};
        const Id value{read(1U + line)};
        const u32 raw{static_cast<u32>(attribute)};
        if (raw >= static_cast<u32>(IR::Attribute::PositionX) &&
            raw <= static_cast<u32>(IR::Attribute::PositionW)) {
            position[raw - static_cast<u32>(IR::Attribute::PositionX)] = value;
        } else {
            StoreFallbackOutput(ctx, attribute, value);
        }
    }
    for (u32 line = 0; line < primitive_lines; ++line) {
        StoreFallbackOutput(ctx, IR::Attribute{mesh.primitive_lines[line]},
                            read(1U + vertex_lines + line));
    }
    // (OpenGL-style clip space, as the mesh epilogue's ConvertDepth)
    if (ctx.runtime_info.convert_depth_mode && !ctx.profile.support_native_ndc &&
        Sirit::ValidId(position[2]) && Sirit::ValidId(position[3])) {
        position[2] = ctx.OpFMul(ctx.F32[1], ctx.OpFAdd(ctx.F32[1], position[2], position[3]),
                                 ctx.Const(0.5f));
    }
    for (u32 element = 0; element < 4; ++element) {
        if (Sirit::ValidId(position[element])) {
            StorePosition(ctx, element, position[element]);
        }
    }
    // (a record past the workgroup's primitive count, or one no mesh workgroup wrote: outside the clip volume)
    If(ctx, ctx.OpLogicalOr(ctx.U1, ctx.OpUGreaterThanEqual(ctx.U1, primitive, count),
                            ctx.OpIEqual(ctx.U1, valid, ctx.u32_zero_value)),
       [&] {
           StorePosition(ctx, 0, ctx.f32_zero_value);
           StorePosition(ctx, 1, ctx.f32_zero_value);
           StorePosition(ctx, 2, ctx.f32_zero_value);
           StorePosition(ctx, 3, ctx.Const(-1.0f));
       });
}

Id EmitIsbeRead(EmitContext& ctx, Id address) {
    const Id workgroup{
        ctx.OpCompositeExtract(ctx.U32[1], ctx.OpLoad(ctx.U32[3], ctx.workgroup_id), 0U)};
    if (ctx.stage == Stage::Task) {
        if (ctx.wants_mesh_first && Sirit::ValidId(ctx.rescaling_push_constants)) {
            // (hookshot) + the draw's first workgroup: NVN draws [first, first + count). Without it every draw began at 0: the
            // foliage's draws (first up to ~1900) drew the first records again and again, and the forest had no leaves
            const Id table{ctx.OpLoad(ctx.U64, ctx.OpAccessChain(ctx.TypePointer(spv::StorageClass::PushConstant, ctx.U64),
                                                                  ctx.rescaling_push_constants, ctx.Const(ctx.mesh_first_member_index)))};
            const Id draw{ctx.OpLoad(ctx.U32[1], ctx.draw_index)};
            const Id at{ctx.OpIAdd(ctx.U64, table, ctx.OpUConvert(ctx.U64, ctx.OpIMul(ctx.U32[1], draw, ctx.Const(16U))))};
            const Id member{ctx.OpAccessChain(ctx.mesh_first_member_pointer, ctx.OpConvertUToPtr(ctx.mesh_first_block_pointer, at),
                                              ctx.Const(0U))};
            const Id first{ctx.OpLoad(ctx.U32[1], member, spv::MemoryAccessMask::Aligned, 4U)};
            return ctx.OpIAdd(ctx.U32[1], workgroup, first);
        }
        return workgroup; // the input region holds the workgroup index (VERTEX_ID)
    }
    const Id index{ctx.OpShiftRightLogical(ctx.U32[1], address, ctx.Const(2U))};
    const Id word{ctx.OpLoad(ctx.U32[1], PayloadWord(ctx, index))};
    return ctx.OpSelect(ctx.U32[1], ctx.OpIEqual(ctx.U1, address, ctx.u32_zero_value), workgroup,
                        word);
}

void EmitIsbeWriteAttribute(EmitContext& ctx, Id address, Id value) {
    const Id index{ctx.OpShiftRightLogical(ctx.U32[1], address, ctx.Const(2U))};
    if (ctx.stage == Stage::Task) {
        ctx.OpStore(PayloadWord(ctx, index), value);
        return;
    }
    ctx.OpStore(SharedWord(ctx, ctx.mesh_attributes, Clamped(ctx, index, ctx.mesh_attribute_words)),
                value);
}

void EmitIsbeWriteMapU8(EmitContext& ctx, Id address, Id value) {
    if (ctx.stage != Stage::Mesh) {
        throw NotImplementedException("Stage buffer map write outside mesh programs");
    }
    ctx.OpStore(SharedWord(ctx, ctx.mesh_map, Clamped(ctx, address, ctx.mesh_map_bytes)),
                ctx.OpBitwiseAnd(ctx.U32[1], value, ctx.Const(0xffU)));
}

void EmitIsbeWriteMapU32(EmitContext& ctx, const IR::Value& address, Id value) {
    // The primitive count: the map is written from its end, so its first word is at byte 3.
    if (ctx.stage != Stage::Mesh || !address.IsImmediate() || address.U32() != 3) {
        throw NotImplementedException("32-bit stage buffer map write");
    }
    ctx.OpStore(ctx.mesh_primitive_count, value);
}

void EmitMeshPrologue(EmitContext& ctx) {
    if (ctx.stage == Stage::Task) {
        // (hookshot) the count starts at nothing: task payload memory holds whatever the last task left, and a task that keeps no meshlet
        // never writes it. The barrier: every thread has done this before any of them goes on to write the real count
        ctx.OpStore(PayloadWord(ctx, ctx.Const(1U)), ctx.u32_zero_value);
        Barrier(ctx);
    }
    if (ctx.stage != Stage::Mesh) {
        return;
    }
    // (local) The stage buffer's map and attribute regions start at zero, as they come out on NVIDIA: workgroup memory holds
    // whatever the last workgroup on that unit left, and a vertex or an index byte the program left unwritten was that. On an
    // RX 7700S the foliage drawn into the 2400 x 2400 shadow map around Link came out with hundreds of long thin triangles
    // across the whole map (vertices from other meshlets' leftovers), dark lines on the ground, the water and Link that
    // changed every frame. Zeroed, such a primitive is (0,0,0): no area. Straight-line stores, each clamped into its array
    // (no branch: see below). RX_VK_MESH_NO_ZERO=1: as before
    static const bool no_zero{std::getenv("RX_VK_MESH_NO_ZERO") != nullptr};
    if (!no_zero && ctx.mesh_threads) {
        const Id invocation{LocalInvocation(ctx)};
        const auto zero_all = [&](Id array, u32 count) {
            if (!Sirit::ValidId(array) || count == 0) {
                return;
            }
            for (u32 first = 0; first < count; first += ctx.mesh_threads) {
                const Id at{Clamped(ctx, ctx.OpIAdd(ctx.U32[1], invocation, ctx.Const(first)), count)};
                ctx.OpStore(SharedWord(ctx, array, at), ctx.u32_zero_value);
            }
        };
        zero_all(ctx.mesh_map, ctx.mesh_map_bytes);
        zero_all(ctx.mesh_attributes, ctx.mesh_attribute_words);
    }
    if (ctx.mesh_fallback && !ctx.runtime_info.mesh_fallback_task && ctx.mesh_threads) {
        // (a mesh program with no task reads its workgroup index from the payload: NVN's hardware puts it at byte 0)
        const Id invocation{LocalInvocation(ctx)};
        for (u32 first = 0; first < ctx.mesh_info.payload_words; first += ctx.mesh_threads) {
            const Id at{Clamped(ctx, ctx.OpIAdd(ctx.U32[1], invocation, ctx.Const(first)),
                               ctx.mesh_info.payload_words)};
            ctx.OpStore(SharedWord(ctx, ctx.mesh_payload, at), ctx.u32_zero_value);
        }
        if (ctx.mesh_info.payload_words) {
            ctx.OpStore(SharedWord(ctx, ctx.mesh_payload, ctx.u32_zero_value), WorkgroupId(ctx));
        }
        Barrier(ctx);
    }
    // Every invocation stores the zero: no branch here, the entry block's label is a phi
    // predecessor for later blocks.
    ctx.OpStore(ctx.mesh_primitive_count, ctx.u32_zero_value);
    Barrier(ctx);
}

void EmitMeshEpilogue(EmitContext& ctx) {
    if (ctx.stage == Stage::Task) {
        TaskEpilogue(ctx);
    } else if (ctx.stage == Stage::Mesh) {
        MeshEpilogue(ctx);
    }
}

} // namespace Shader::Backend::SPIRV
