// (hookshot) Every Vulkan command that does GPU work, or orders it, counts in work_epoch. full_barrier (vk_texture.cpp) leaves out a
// barrier recorded right after another one on the same command buffer with nothing in between: the second orders nothing the first
// did not. Include this header AFTER the file's other includes: it turns the calls below into a count and the call (a header
// included after it that names one of them with a '(' after it would be changed too).
#pragma once

#include <atomic>

#include "vk_loader.h"

namespace mkwvk {
extern std::atomic<uint64_t> work_epoch;   // (vk_texture.cpp)
inline void note_work() {
    // (an add, not a load and a store: a thread preempted between the two would store a stale count over another thread's, which could
    // make the count equal the one a barrier was recorded at with work recorded since, and the next barrier is left out for it)
    work_epoch.fetch_add(1, std::memory_order_relaxed);
}
} // namespace mkwvk

#define vkCmdPipelineBarrier2(...) (::mkwvk::note_work(), ::vkx::vkCmdPipelineBarrier2(__VA_ARGS__))
#define vkCmdBeginRendering(...) (::mkwvk::note_work(), ::vkx::vkCmdBeginRendering(__VA_ARGS__))
#define vkCmdEndRendering(...) (::mkwvk::note_work(), ::vkx::vkCmdEndRendering(__VA_ARGS__))
#define vkCmdDraw(...) (::mkwvk::note_work(), ::vkx::vkCmdDraw(__VA_ARGS__))
#define vkCmdDrawIndexed(...) (::mkwvk::note_work(), ::vkx::vkCmdDrawIndexed(__VA_ARGS__))
#define vkCmdDrawIndirect(...) (::mkwvk::note_work(), ::vkx::vkCmdDrawIndirect(__VA_ARGS__))
#define vkCmdDrawIndexedIndirect(...) (::mkwvk::note_work(), ::vkx::vkCmdDrawIndexedIndirect(__VA_ARGS__))
#define vkCmdDrawIndirectCount(...) (::mkwvk::note_work(), ::vkx::vkCmdDrawIndirectCount(__VA_ARGS__))
#define vkCmdDrawIndexedIndirectCount(...) (::mkwvk::note_work(), ::vkx::vkCmdDrawIndexedIndirectCount(__VA_ARGS__))
#define vkCmdDrawMeshTasksEXT(...) (::mkwvk::note_work(), ::vkx::vkCmdDrawMeshTasksEXT(__VA_ARGS__))
#define vkCmdDrawMeshTasksIndirectEXT(...) (::mkwvk::note_work(), ::vkx::vkCmdDrawMeshTasksIndirectEXT(__VA_ARGS__))
#define vkCmdDrawMeshTasksIndirectCountEXT(...) (::mkwvk::note_work(), ::vkx::vkCmdDrawMeshTasksIndirectCountEXT(__VA_ARGS__))
#define vkCmdDispatch(...) (::mkwvk::note_work(), ::vkx::vkCmdDispatch(__VA_ARGS__))
#define vkCmdDispatchIndirect(...) (::mkwvk::note_work(), ::vkx::vkCmdDispatchIndirect(__VA_ARGS__))
#define vkCmdCopyBuffer(...) (::mkwvk::note_work(), ::vkx::vkCmdCopyBuffer(__VA_ARGS__))
#define vkCmdCopyBufferToImage(...) (::mkwvk::note_work(), ::vkx::vkCmdCopyBufferToImage(__VA_ARGS__))
#define vkCmdCopyImageToBuffer(...) (::mkwvk::note_work(), ::vkx::vkCmdCopyImageToBuffer(__VA_ARGS__))
#define vkCmdCopyImage(...) (::mkwvk::note_work(), ::vkx::vkCmdCopyImage(__VA_ARGS__))
#define vkCmdBlitImage(...) (::mkwvk::note_work(), ::vkx::vkCmdBlitImage(__VA_ARGS__))
#define vkCmdResolveImage(...) (::mkwvk::note_work(), ::vkx::vkCmdResolveImage(__VA_ARGS__))
#define vkCmdClearColorImage(...) (::mkwvk::note_work(), ::vkx::vkCmdClearColorImage(__VA_ARGS__))
#define vkCmdClearDepthStencilImage(...) (::mkwvk::note_work(), ::vkx::vkCmdClearDepthStencilImage(__VA_ARGS__))
#define vkCmdClearAttachments(...) (::mkwvk::note_work(), ::vkx::vkCmdClearAttachments(__VA_ARGS__))
#define vkCmdFillBuffer(...) (::mkwvk::note_work(), ::vkx::vkCmdFillBuffer(__VA_ARGS__))
#define vkCmdUpdateBuffer(...) (::mkwvk::note_work(), ::vkx::vkCmdUpdateBuffer(__VA_ARGS__))
#define vkCmdBeginQuery(...) (::mkwvk::note_work(), ::vkx::vkCmdBeginQuery(__VA_ARGS__))
#define vkCmdEndQuery(...) (::mkwvk::note_work(), ::vkx::vkCmdEndQuery(__VA_ARGS__))
