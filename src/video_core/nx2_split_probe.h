// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <cstdlib>
#include <optional>
#include <string>
#ifdef _MSC_VER
#include <intrin.h>
#elif defined(ARCHITECTURE_x86_64)
#include <x86intrin.h>
#endif

#include "common/common_types.h"
#include "common/cpu_features.h"
#include "common/logging.h"
#include "common/thread_cpu_clock.h"

// EDEN_NX2_SPLIT_PROBE=1: how much of the draw preparation on the thread that draws could run on
// another core from a snapshot of the draw's inputs, and how much has to stay in guest order
// (docs/nx2/split-probe.md). The GPU profile's zones are its boundaries, so it is off while
// EDEN_NX2_GPU_PROFILE or EDEN_NX2_STUTTER_TRACE is set.
namespace VideoCommon::Nx2SplitProbe {

/// What a stretch of a draw reads and writes. Serial work reads guest memory or memory tracker
/// state at draw time, or changes state that later commands depend on in order. Parallel work
/// reads the draw's registers and cache lookups and writes only per-draw output, so it could run
/// from a snapshot unless it changes a cache's structure (see Mutation).
enum class Work : u8 {
    None,
    FlushWork,     ///< Worker submission and cache invalidation for the writes since the last draw
    Uniforms,      ///< Uniform data copied from guest memory
    BufferSync,    ///< Memory tracker walk and buffer uploads
    Handles,       ///< Texture handles, SSBO pointers and TSC entries read from guest memory
    ImageViews,    ///< TIC entries read, views looked up, images checked for CPU writes
    Upload,        ///< Texture uploads
    RenderTargets, ///< Render target images, framebuffer and render pass state
    Lock,          ///< The buffer and texture cache mutexes
    Queries,       ///< Query segments, transform feedback and counter enables
    OtherSerial,   ///< Any other zone inside a draw (waits on lists or fences)
    Draw,          ///< Draw parameters, fast-draw checks and vertex layout from registers
    PipelineLookup,
    Configure,     ///< Per-draw bookkeeping and texture buffer bindings
    BufferResolve, ///< Addresses to cached buffers, index and vertex bindings
    StageBindings, ///< Storage and texel buffer bindings
    Descriptors,   ///< Descriptor queue writes, pipeline and descriptor binds
    DynamicState,
    Record,        ///< The draw command
    Idle,          ///< Outside draws: the thread waits for work
    Count,
    Skip = Count,
};

inline constexpr std::array<const char*, static_cast<size_t>(Work::Count)> work_names{
    "",           "flush work",    "uniforms",       "buffer sync",    "handles",
    "image views", "uploads",      "render targets", "locks",          "queries",
    "other",      "draw setup",    "pipeline lookup", "configure",     "buffer resolve",
    "stage bindings", "descriptors", "dynamic state", "record",         "idle",
};

inline constexpr bool IsParallel(Work work) {
    return work >= Work::Draw && work <= Work::Record;
}

/// Changes to a cache's structure. Parallel work that makes one has to fall back to the serial
/// path; serial work that makes one has to wait for parallel work still reading the cache.
enum class Mutation : u8 {
    Buffer,
    Image,
    View,
    Sampler,
    Pipeline,
    Count,
};

inline constexpr std::array<const char*, static_cast<size_t>(Mutation::Count)> mutation_names{
    "buffers", "images", "views", "samplers", "pipelines",
};

inline bool g_enabled{};

/// Called by Nx2Profile::ReadSwitch. Off while the zones measure for the GPU profile or the
/// stutter trace: their clock reads and bookkeeping would land inside the probe's stretches.
inline void ReadSwitch(bool zones_measured) {
    const char* const value = std::getenv("EDEN_NX2_SPLIT_PROBE");
    g_enabled = value != nullptr && value[0] == '1' && !zones_measured;
}

inline u64 Ticks() {
#ifdef ARCHITECTURE_x86_64
    return __rdtsc();
#else
    return static_cast<u64>(Common::g_wall_clock.GetTimeNS().count());
#endif
}

struct ThreadState {
    std::array<u64, static_cast<size_t>(Work::Count)> ticks{};
    std::array<u64, static_cast<size_t>(Mutation::Count)> parallel_mutations{};
    std::array<u64, static_cast<size_t>(Mutation::Count)> serial_mutations{};
    Work current{Work::None};
    u32 depth{}; ///< Stretches open inside the current draw, the draw included
    bool draw_mutated{};
    u64 last{};
    u64 draw_parallel{}; ///< Parallel ticks of the current draw
    u64 forced{};        ///< Parallel ticks of draws that changed a cache's structure
    u64 draws{};
    u64 forced_draws{};
    u64 switches{};
    u64 window_ns{};
    u64 window_ticks{};
    std::optional<u64> window_cpu_ns;
};

inline thread_local ThreadState t_state;

inline void Switch(ThreadState& state, Work next) {
    const u64 now = Ticks();
    const u64 elapsed = now - state.last;
    state.ticks[static_cast<size_t>(state.current)] += elapsed;
    if (IsParallel(state.current)) {
        state.draw_parallel += elapsed;
    }
    state.current = next;
    state.last = now;
    ++state.switches;
}

inline void Report(ThreadState& state) {
    const u64 now_ns = static_cast<u64>(Common::g_wall_clock.GetTimeNS().count());
    if (state.window_ns != 0 && now_ns - state.window_ns < 1'000'000'000ULL) {
        return;
    }
    const u64 now_ticks = Ticks();
    const std::optional<u64> cpu_ns = Common::CurrentThreadCpuTimeNs();
    const auto restart = [&] {
        state.ticks = {};
        state.parallel_mutations = {};
        state.serial_mutations = {};
        state.forced = 0;
        state.draws = 0;
        state.forced_draws = 0;
        state.switches = 0;
        state.window_ns = now_ns;
        state.window_ticks = now_ticks;
        state.window_cpu_ns = cpu_ns;
    };
    if (state.window_ns == 0) {
        restart();
        return;
    }
    const double window = static_cast<double>(now_ns - state.window_ns);
    const double ns_per_tick =
        window / static_cast<double>((std::max<u64>)(now_ticks - state.window_ticks, 1));
    const auto ms = [&](u64 ticks) { return static_cast<double>(ticks) * ns_per_tick * 1e3 / window; };
    const auto per_second = [&](u64 count) { return static_cast<double>(count) * 1e9 / window; };
    const auto add = [](std::string& text, const char* name, double value) {
        text += fmt::format("{} {} {:.1f}", text.empty() ? "" : ",", name, value);
    };
    u64 serial{};
    u64 parallel{};
    std::string serial_text;
    std::string parallel_text;
    for (size_t i = 1; i < static_cast<size_t>(Work::Idle); ++i) {
        if (state.ticks[i] == 0) {
            continue;
        }
        if (IsParallel(static_cast<Work>(i))) {
            parallel += state.ticks[i];
            add(parallel_text, work_names[i], ms(state.ticks[i]));
        } else {
            serial += state.ticks[i];
            add(serial_text, work_names[i], ms(state.ticks[i]));
        }
    }
    const auto mutations = [&](const auto& counts) {
        std::string text;
        for (size_t i = 0; i < counts.size(); ++i) {
            if (counts[i] != 0) {
                add(text, mutation_names[i], per_second(counts[i]));
            }
        }
        return text.empty() ? std::string{" none"} : text;
    };
    const double busy = 1000.0 - ms(state.ticks[static_cast<size_t>(Work::Idle)]);
    const double parallel_ms = ms(parallel - state.forced);
    const double draw_ms = ms(serial + parallel);
    const std::string cpu =
        cpu_ns && state.window_cpu_ns && *cpu_ns >= *state.window_cpu_ns
            ? fmt::format("{:.1f} ms", static_cast<double>(*cpu_ns - *state.window_cpu_ns) * 1e3 / window)
            : std::string{"unavailable"};
    constexpr int calibration_reads = 64;
    const u64 calibration_start = Ticks();
    for (int i = 0; i < calibration_reads; ++i) {
        (void)Ticks();
    }
    const double read_ns =
        static_cast<double>(Ticks() - calibration_start) * ns_per_tick / calibration_reads;
    LOG_INFO(HW_GPU,
             "draw split probe per second: serial {:.1f} ms, parallel {:.1f} ms, forced serial "
             "{:.1f} ms ({:.0f} draws) | {:.0f} draws in {:.1f} ms, busy {:.1f} ms (CPU {}), "
             "parallel {:.1f}% of busy, {:.1f}% of draw time | serial:{} | parallel:{} | "
             "structure changes in parallel work:{} | in serial work:{} | probe clock reads "
             "{:.1f} ms ({:.0f} at {:.1f} ns)",
             ms(serial), parallel_ms, ms(state.forced), per_second(state.forced_draws),
             per_second(state.draws), draw_ms, busy, cpu,
             busy > 0 ? parallel_ms * 100.0 / busy : 0.0,
             draw_ms > 0 ? parallel_ms * 100.0 / draw_ms : 0.0, serial_text, parallel_text, mutations(state.parallel_mutations),
             mutations(state.serial_mutations), per_second(state.switches) * read_ns / 1e6,
             per_second(state.switches), read_ns);
    restart();
}

inline void EndDraw(ThreadState& state) {
    ++state.draws;
    if (state.draw_mutated) {
        state.forced += state.draw_parallel;
        ++state.forced_draws;
    }
    state.draw_parallel = 0;
    state.draw_mutated = false;
    if ((state.draws & 255) == 0) {
        Report(state);
    }
}

/// Starts timing `work` inside a draw (or a draw or an idle wait itself); returns what to restore
/// with Leave, or Skip when nothing is timed.
[[nodiscard]] inline Work Enter(Work work) {
    auto& state = t_state;
    if (state.depth != 0 || work == Work::Draw) {
        ++state.depth;
    } else if (work != Work::Idle) {
        return Work::Skip;
    }
    const Work parent = state.current;
    Switch(state, work);
    return parent;
}

inline void Leave(Work parent) {
    auto& state = t_state;
    Switch(state, parent);
    if (state.depth != 0 && --state.depth == 0) {
        EndDraw(state);
    }
}

/// The rest of the enclosing stretch is `work`.
inline void Mark(Work work) {
    if (g_enabled && t_state.depth != 0) {
        Switch(t_state, work);
    }
}

inline void Mutate(Mutation mutation) {
    if (!g_enabled || t_state.depth == 0) {
        return;
    }
    auto& state = t_state;
    const auto index = static_cast<size_t>(mutation);
    if (IsParallel(state.current)) {
        ++state.parallel_mutations[index];
        state.draw_mutated = true;
    } else {
        ++state.serial_mutations[index];
    }
}

/// A stretch inside a draw that no GPU profile zone covers on its own.
class Segment {
public:
    explicit Segment(Work work) : parent{g_enabled ? Enter(work) : Work::Skip} {}

    ~Segment() {
        if (parent != Work::Skip) {
            Leave(parent);
        }
    }

    Segment(const Segment&) = delete;
    Segment& operator=(const Segment&) = delete;

private:
    Work parent;
};

} // namespace VideoCommon::Nx2SplitProbe
