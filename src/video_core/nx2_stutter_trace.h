// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>

#include "common/common_types.h"

// EDEN_NX2_STUTTER_TRACE=1: per presented frame, the wall time each GPU thread (front, back,
// VulkanWorker) spent by category, from the EDEN_NX2_GPU_PROFILE zones (nx2_profile.h maps each
// zone to a category). Frames presented more than 33 ms after the previous one are logged with
// their top category (Nx2Stutter); every 10 s the frame-time histogram, pipeline builds and
// lookups (Nx2StutterSummary); and the boot phases with the process memory (Nx2Boot).
// tools/nx2/cache_metrics.py reads them.
namespace VideoCommon::StutterTrace {

enum class Category : u8 {
    GraphicsMiss,  ///< Graphics pipeline built on the GPU thread (environments, translation, SPIR-V)
    ComputeMiss,   ///< Compute pipeline built on the GPU thread
    PipelineWait,  ///< VulkanWorker holding draws and dispatches for pipelines still building
    ShaderCapture, ///< Programs captured, lowered and hashed when the shader cache first sees them
    TextureCache,  ///< Uploads, decoding, downloads, joins and blits, view and target lookups
    BufferCache,   ///< Buffer binding and uploads
    QueryFence,    ///< Waits for GPU ticks, fences and command list readbacks
    Present,       ///< Swapchain frame acquire and present
    Idle,          ///< Waiting for work or for the other GPU thread (never a cause)
    Count,
    None = Count,
};

inline constexpr size_t CategoryCount = static_cast<size_t>(Category::Count);
inline constexpr std::array<std::string_view, CategoryCount> category_names{
    "gfx_miss", "compute_miss", "pipeline_wait", "shader_capture", "texture",
    "buffer",   "query_fence",  "present",       "idle",
};

enum class Thread : u8 {
    Front,  ///< The GPU thread (all of the work when the split is off)
    Back,   ///< GPUBack (gpu_split.h)
    Worker, ///< VulkanWorker
    Count,
};

inline constexpr size_t ThreadCount = static_cast<size_t>(Thread::Count);

/// Pipeline lookups, counted at each pipeline's first use in the session, and native program
/// lookups (shader cache v2), at each program's first bind.
enum class Lookup : u8 {
    GraphicsPrebuilt,   ///< Built from the disk cache at boot
    GraphicsBackground, ///< Built from the disk cache by the background prebuild
    GraphicsRecorded,   ///< In the disk cache but not built (skipped at boot, or evicted)
    GraphicsNew,        ///< Not in the disk cache
    ComputePrebuilt,
    ComputeNew,
    ProgramHit,   ///< Native program from a shader cache v2 record, not lowered
    ProgramMiss,  ///< Native program lowered: no record
    ProgramStale, ///< Native program lowered: a record of another build's lowering
    Count,
};

inline constexpr size_t LookupCount = static_cast<size_t>(Lookup::Count);

[[nodiscard]] inline bool ReadEnv() {
    const char* const value = std::getenv("EDEN_NX2_STUTTER_TRACE");
    return value != nullptr && value[0] == '1';
}

inline bool g_enabled = ReadEnv();

/// Called with Nx2Profile::ReadSwitch, once nx2_switches.txt is applied.
inline void ReadSwitch() {
    g_enabled = ReadEnv();
}

[[nodiscard]] inline bool Enabled() {
    return g_enabled;
}

struct Totals {
    /// Monotonic, each written only by its own thread; frames take the differences.
    std::array<std::array<std::atomic<u64>, CategoryCount>, ThreadCount> ticks{};
    std::array<std::array<std::atomic<u64>, CategoryCount>, ThreadCount> counts{};
    std::array<std::atomic<bool>, ThreadCount> running{};
    std::array<std::atomic<u64>, LookupCount> lookups{};
};

inline Totals g_totals;
inline thread_local size_t t_thread = ThreadCount;

/// Names the calling thread for its lifetime.
class ThreadScope {
public:
    explicit ThreadScope(Thread thread) : index{static_cast<size_t>(thread)} {
        t_thread = index;
        g_totals.running[index].store(true, std::memory_order_relaxed);
    }
    ~ThreadScope() {
        g_totals.running[index].store(false, std::memory_order_relaxed);
        t_thread = ThreadCount;
    }
    ThreadScope(const ThreadScope&) = delete;
    ThreadScope& operator=(const ThreadScope&) = delete;

private:
    size_t index;
};

/// Exclusive zone time in Nx2Profile::Ticks on the calling thread; unnamed threads are ignored.
inline void Add(Category category, u64 ticks) {
    if (t_thread >= ThreadCount || category == Category::None) {
        return;
    }
    const auto index = static_cast<size_t>(category);
    auto& total = g_totals.ticks[t_thread][index];
    total.store(total.load(std::memory_order_relaxed) + ticks, std::memory_order_relaxed);
    auto& count = g_totals.counts[t_thread][index];
    count.store(count.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
}

inline void CountLookup(Lookup lookup) {
    g_totals.lookups[static_cast<size_t>(lookup)].fetch_add(1, std::memory_order_relaxed);
}

using ThreadTicks = std::array<std::array<u64, CategoryCount>, ThreadCount>;

struct Attribution {
    std::array<double, CategoryCount> category_ms{}; ///< Summed over the threads
    std::array<std::optional<double>, ThreadCount> busy_ms{}; ///< Running threads: not idle
    double other_ms{}; ///< Busy time no category explains, on the thread with the most of it
    std::string_view cause; ///< The largest of the categories (Idle excluded) and other
};

/// What one frame's time went to, from the ticks each thread added during it.
[[nodiscard]] Attribution Attribute(const ThreadTicks& ticks,
                                    const std::array<bool, ThreadCount>& running,
                                    double interval_ms, double ms_per_tick);

/// Called by the renderer for each presented frame, by one thread at a time.
void OnPresent();

/// Logs a boot phase with the time and the process memory. Reads the environment itself: the
/// boot starts before the video core applies nx2_switches.txt.
void BootMarker(std::string_view event, std::string_view details = {});

/// The disk pipeline cache file, whose size the summaries log.
void SetCacheFile(std::string path);

} // namespace VideoCommon::StutterTrace
