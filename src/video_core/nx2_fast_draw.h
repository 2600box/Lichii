// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <cstdlib>

#include "common/common_types.h"

/// A draw that follows a draw of the same pipeline with only light state changes in between takes
/// the delta path. It keeps the previous draw's pipeline, image views, samplers and texture buffer
/// bindings, and resolves and binds every buffer (index, vertex, uniform, storage, texture) again
/// as a full resolve does, so guest memory written without a notification is streamed as in a
/// full resolve.
/// Light changes are draw parameters, index buffer and vertex stream registers, constant buffer
/// binds and inline constant buffer data (collected in g_delta: the rasterizer resolves in full
/// when they reach the pipeline's texture handles). Anything else that could change what a
/// resolve produces bumps a serial: a 3D register that changes, any 3D method with side effects
/// (uploads, reports, clears, barriers, ...), constant buffer data reaching the texture or sampler
/// headers, any other engine, deferred calls on the split's back (except the ones marked
/// GpuSplit::FastDrawNeutral), GPU::TickWork requests, texture cache writes, unmaps and
/// deletions, shader removals, fence releases, HLE macros and frames. The rasterizer takes the
/// delta path only while the serial, the GPU memory map, the command buffer, the render pass and
/// the bound pipeline are the ones the previous draw left.
/// EDEN_NX2_FAST_DRAW_VERIFY=1 resolves those draws in full and counts where the pipeline, views,
/// samplers or kept buffer bindings differ.
namespace VideoCommon::Nx2FastDraw {

enum class Reason : u8 {
    Register,
    Method,
    ConstantData,
    InlineData,
    OtherEngine,
    Call,
    TickWork,
    Invalidation,
    Fence,
    IndirectReset,
    Frame,
    TextureHeaders,
    DeltaOverflow,
    Map,
    Count,
};

inline constexpr std::array<const char*, static_cast<size_t>(Reason::Count)> reason_names{
    "register",      "method",    "constant buffer data", "inline data",
    "other engine",  "deferred call", "tick work",        "invalidation",
    "fence release", "indirect reset", "frame",           "texture header data",
    "delta overflow", "memory map",
};

inline std::atomic<bool> g_enabled{true};

/// Any thread: whether fast draws run (always, unless a test turned them off). The rasterizer
/// latches it per draw (on the thread that draws) and starts with a full resolve when it turns on.
[[nodiscard]] inline bool Enabled() {
    return g_enabled.load(std::memory_order_relaxed);
}

/// Tests only.
inline void SetEnabledForTest(bool on) {
    g_enabled.store(on, std::memory_order_relaxed);
}

/// EDEN_NX2_FAST_DRAW_VERIFY=1, read once.
[[nodiscard]] inline bool VerifyEnabled() {
    static const bool verify = [] {
        const char* const value = std::getenv("EDEN_NX2_FAST_DRAW_VERIFY");
        return value != nullptr && value[0] == '1';
    }();
    return verify;
}

inline std::atomic<u64> g_serial{1};
/// The last break's reason and, for Register and Method, its 3D method (diagnostics).
inline std::atomic<u32> g_last_break{};

/// EDEN_NX2_EGLA66_UBO_KEEP_FULL: set once by the buffer cache that keeps uniform buffers on full
/// draws. g_uniform_serial then counts the breaks other than register writes.
inline std::atomic<bool> g_track_uniforms{};
inline std::atomic<u64> g_uniform_serial{1};

inline void Break(Reason reason, u32 method = 0) {
    if (Enabled()) {
        g_serial.fetch_add(1, std::memory_order_relaxed);
        g_last_break.store(static_cast<u32>(reason) | (method << 8), std::memory_order_relaxed);
        if (reason != Reason::Register && g_track_uniforms.load(std::memory_order_relaxed)) {
            g_uniform_serial.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

inline u64 Serial() {
    return g_serial.load(std::memory_order_relaxed);
}

/// Changes when anything but a 3D register write broke the fast draw chain (with
/// g_track_uniforms; otherwise it stays put and must not be used).
inline u64 UniformSerial() {
    return g_uniform_serial.load(std::memory_order_relaxed);
}

/// Light changes since the last draw that can reach texture handles. Written and read on the
/// thread that runs the 3D engine and the rasterizer (the split's back).
struct Delta {
    static constexpr size_t MaxWrites = 16;

    std::array<u32, 5> binds{}; ///< Constant buffer slots bound per stage
    std::array<GPUVAddr, MaxWrites> write_begin{};
    std::array<GPUVAddr, MaxWrites> write_end{};
    u32 num_writes{};

    [[nodiscard]] bool Empty() const {
        return num_writes == 0 && (binds[0] | binds[1] | binds[2] | binds[3] | binds[4]) == 0;
    }
};

inline Delta g_delta;

inline void NoteBind(size_t stage, u32 slot) {
    if (Enabled()) {
        g_delta.binds[stage] |= 1U << slot;
    }
}

/// Inline constant buffer data written to [begin, end).
inline void NoteConstantData(GPUVAddr begin, GPUVAddr end) {
    if (!Enabled()) {
        return;
    }
    Delta& delta = g_delta;
    if (delta.num_writes != 0 && delta.write_end[delta.num_writes - 1] == begin) {
        delta.write_end[delta.num_writes - 1] = end;
        return;
    }
    if (delta.num_writes == Delta::MaxWrites) {
        Break(Reason::DeltaOverflow);
        return;
    }
    delta.write_begin[delta.num_writes] = begin;
    delta.write_end[delta.num_writes] = end;
    ++delta.num_writes;
}

} // namespace VideoCommon::Nx2FastDraw
