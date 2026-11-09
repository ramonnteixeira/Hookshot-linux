// GPU profile (diagnostics): RX_VK_PASS_TIMING=1 (RX_VK_GPU_PROFILE=1 is the same) times the GPU's work with timestamps: every render
// pass, compute dispatch, barrier, texture/buffer copy, mirror sync, write-back and uniform write of the recordings' command buffers,
// and of their upload command buffers, each a pair of timestamps labelled by what it was. Every 600 presents it logs, per frame: the
// kinds of span with their totals, and the labels that took the most GPU time.
// RX_VK_PASS_TIMING=2: also the runs of consecutive draws of one program inside the passes ("prog" lines: GPU ms by program key).
// A timestamp waits for the GPU to finish what came before it, so profiled spans add up to more than an unprofiled frame (the
// passes are ordered by full barriers anyway; the overlap that narrower barriers would allow is not measured here).
#pragma once

#include <string>

#include "vk_internal.h"

namespace mkwvk {

bool gprof_on();
/// A recording begins in slot (its earlier work is done: its timings are collected first); the upload command buffer is the
/// recording's other one (submitted ahead of cmd, only when it has work)
void gprof_begin_recording(int slot, VkCommandBuffer cmd, VkCommandBuffer upload);
/// The recording is going to the GPU (upload_sent: with its upload command buffer)
void gprof_submitted(int slot, bool upload_sent);
/// A timed span begins in cmd (one at a time a command buffer: false when one is open already, or cmd is not a profiled one);
/// gprof_end closes it (its label: what it was; items: draws, copies...; bytes: copied)
bool gprof_begin(VkCommandBuffer cmd);
void gprof_end(VkCommandBuffer cmd, const std::string& label, int items = 1, uint64_t bytes = 0);
/// A draw of program key into the open pass (RX_VK_PASS_TIMING=2): the run of draws of one program is timed
void gprof_run(VkCommandBuffer cmd, uint64_t key);
/// The slot's work completed: its spans are added to the totals
void gprof_collect(int slot);
/// Every 120 presents (frames of them): every 600, the report
void gprof_report(int frames);

/// A span of a scope: closed by the destructor with its label (items and bytes set while it is open)
struct GprofSpan {
    VkCommandBuffer cmd;
    const char* label;
    int items = 1;
    uint64_t bytes = 0;
    bool on = false;
    GprofSpan(VkCommandBuffer c, const char* l) : cmd(c), label(l) {
        if (gprof_on()) on = gprof_begin(c);
    }
    ~GprofSpan() {
        if (on) gprof_end(cmd, label, items, bytes);
    }
    GprofSpan(const GprofSpan&) = delete;
    GprofSpan& operator=(const GprofSpan&) = delete;
};

} // namespace mkwvk
