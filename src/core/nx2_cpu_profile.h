// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <fmt/format.h>

#include "common/assert.h"
#include "common/common_types.h"
#include "common/logging.h"
#include "core/hardware_properties.h"

/// EDEN_NX2_CPU_PROFILE=1: where the emulated CPU cores spend their time. Each core publishes its
/// state (guest code, an SVC, a rasterizer readback, idle, or emulator work in between) with one
/// relaxed store per transition; a sampler thread reads the states every millisecond and logs
/// the split per core every 10 s, with the sampled time and call rate of the busiest SVCs.
namespace Core::Nx2CpuProfile {

enum class State : u32 {
    Emulator, ///< Kernel and scheduler work between guest code runs
    Guest,    ///< Running guest code in the JIT
    Readback, ///< A guest read waiting for GPU-written memory (HandleRasterizerDownload)
    Idle,     ///< No guest thread to run
    Svc,      ///< Inside an SVC; the low byte above this value is the SVC number
};

inline constexpr size_t NumCores = Hardware::NUM_CPU_CORES;
inline constexpr size_t NumSvcs = 0x100;
inline constexpr u32 SvcBase = static_cast<u32>(State::Svc);

inline bool Enabled() {
    static const bool enabled = std::getenv("EDEN_NX2_CPU_PROFILE") != nullptr;
    return enabled;
}

/// Per-core sample counts and per-SVC call statistics for one window.
class Window {
public:
    void AddSample(size_t core, u32 state) {
        auto& counts = samples[core];
        if (state >= SvcBase) {
            ++counts.svc;
            ++svc_samples[std::min<size_t>(state - SvcBase, NumSvcs - 1)];
        } else {
            ++counts.by_state[state];
        }
    }

    void AddSvcCalls(u32 svc, u64 calls) {
        svc_calls[std::min<size_t>(svc, NumSvcs - 1)] += calls;
    }

    /// One line per active core, then the SVCs ordered by sampled time.
    std::vector<std::string> Report(double seconds) const {
        std::vector<std::string> lines;
        for (size_t core = 0; core < NumCores; ++core) {
            const auto& counts = samples[core];
            const u64 total = counts.svc + counts.by_state[0] + counts.by_state[1] +
                              counts.by_state[2] + counts.by_state[3];
            if (total == 0) {
                continue;
            }
            const auto percent = [total](u64 count) { return 100.0 * count / total; };
            lines.push_back(fmt::format("core {}: guest {:.1f}% | svc {:.1f}% | readback {:.1f}% | "
                                        "emulator {:.1f}% | idle {:.1f}%",
                                        core, percent(counts.by_state[1]), percent(counts.svc),
                                        percent(counts.by_state[2]), percent(counts.by_state[0]),
                                        percent(counts.by_state[3])));
        }
        std::vector<size_t> order;
        for (size_t svc = 0; svc < NumSvcs; ++svc) {
            if (svc_samples[svc] != 0 || svc_calls[svc] != 0) {
                order.push_back(svc);
            }
        }
        std::stable_sort(order.begin(), order.end(), [this](size_t a, size_t b) {
            return svc_samples[a] != svc_samples[b] ? svc_samples[a] > svc_samples[b]
                                                    : svc_calls[a] > svc_calls[b];
        });
        std::string svcs;
        for (size_t i = 0; i < order.size() && i < 8; ++i) {
            const size_t svc = order[i];
            svcs += fmt::format(" | {:#04x}: {} samples, {:.0f} calls/s", svc, svc_samples[svc],
                                svc_calls[svc] / seconds);
        }
        if (!svcs.empty()) {
            lines.push_back("svcs by sampled time" + svcs);
        }
        return lines;
    }

private:
    struct CoreSamples {
        std::array<u64, 4> by_state{};
        u64 svc{};
    };
    std::array<CoreSamples, NumCores> samples{};
    std::array<u64, NumSvcs> svc_samples{};
    std::array<u64, NumSvcs> svc_calls{};
};

struct Shared {
    std::array<std::atomic<u32>, NumCores> states{};
    std::array<std::atomic<bool>, NumCores> registered{};
    std::array<std::atomic<u64>, NumSvcs> svc_calls{};
    std::mutex mutex;
    size_t registered_count{};
    std::jthread sampler;
};

inline Shared& GetShared() {
    static Shared shared;
    return shared;
}

/// Moves the SVC calls counted so far into the window. A call is counted once, when it starts.
inline void DrainSvcCalls(Window& window, Shared& shared) {
    for (size_t svc = 0; svc < NumSvcs; ++svc) {
        window.AddSvcCalls(static_cast<u32>(svc),
                           shared.svc_calls[svc].exchange(0, std::memory_order_relaxed));
    }
}

inline void Sample(std::stop_token stop, Shared& shared) {
    using Clock = std::chrono::steady_clock;
    Window window;
    auto window_start = Clock::now();
    while (!stop.stop_requested()) {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
        for (size_t core = 0; core < NumCores; ++core) {
            if (shared.registered[core].load(std::memory_order_relaxed)) {
                window.AddSample(core, shared.states[core].load(std::memory_order_relaxed));
            }
        }
        const auto now = Clock::now();
        const double seconds = std::chrono::duration<double>(now - window_start).count();
        if (seconds < 10.0) {
            continue;
        }
        DrainSvcCalls(window, shared);
        for (const auto& line : window.Report(seconds)) {
            LOG_INFO(Core, "CPU profile, last {:.0f} s: {}", seconds, line);
        }
        window = {};
        window_start = now;
    }
}

inline thread_local size_t t_core = NumCores;

/// Not inlined: a guest thread's fiber can resume on another core's host thread, and an inlined
/// thread_local address may be reused from before the fiber switch.
YUZU_NO_INLINE inline size_t CurrentCore() {
    return t_core;
}

/// Held by an emulated core's host thread while it runs. The first registration starts the
/// sampler and the last one stops it, so a later session starts with fresh windows and counters.
class CoreRegistration {
public:
    explicit CoreRegistration(size_t core_, bool enabled = Enabled()) {
        if (!enabled || core_ >= NumCores) {
            return;
        }
        core = core_;
        t_core = core;
        auto& shared = GetShared();
        std::scoped_lock lock{shared.mutex};
        shared.states[core].store(static_cast<u32>(State::Emulator), std::memory_order_relaxed);
        shared.registered[core].store(true, std::memory_order_relaxed);
        if (shared.registered_count++ == 0) {
            for (size_t svc = 0; svc < NumSvcs; ++svc) {
                shared.svc_calls[svc].store(0, std::memory_order_relaxed);
            }
            shared.sampler =
                std::jthread([&shared](std::stop_token stop) { Sample(stop, shared); });
        }
    }

    ~CoreRegistration() {
        if (core >= NumCores) {
            return;
        }
        t_core = NumCores;
        auto& shared = GetShared();
        std::jthread sampler;
        std::scoped_lock lock{shared.mutex};
        shared.registered[core].store(false, std::memory_order_relaxed);
        shared.states[core].store(static_cast<u32>(State::Emulator), std::memory_order_relaxed);
        if (--shared.registered_count == 0) {
            sampler = std::move(shared.sampler);
        }
    }

    CoreRegistration(const CoreRegistration&) = delete;
    CoreRegistration& operator=(const CoreRegistration&) = delete;

private:
    size_t core = NumCores;
};

/// Publishes the running fiber's state for the scope and restores its previous one after. Any
/// fiber switch inside the scope must be wrapped in a SwitchScope.
class Scope {
public:
    explicit Scope(u32 state) {
        const size_t core = CurrentCore();
        if (core >= NumCores) {
            return;
        }
        auto& slot = GetShared().states[core];
        previous = slot.load(std::memory_order_relaxed);
        slot.store(state, std::memory_order_relaxed);
        active = true;
    }

    explicit Scope(State state) : Scope{static_cast<u32>(state)} {}

    ~Scope() {
        const size_t core = CurrentCore();
        if (active && core < NumCores) {
            GetShared().states[core].store(previous, std::memory_order_relaxed);
        }
    }

    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    u32 previous{};
    bool active{};
};

/// Wraps a guest thread's fiber switch: the thread takes its state with it, the core shows
/// emulator work until the next fiber publishes its own, and whichever core resumes the thread
/// shows the thread's state again.
class SwitchScope {
public:
    SwitchScope() {
        const size_t core = CurrentCore();
        if (core >= NumCores) {
            return;
        }
        auto& slot = GetShared().states[core];
        saved = slot.load(std::memory_order_relaxed);
        slot.store(static_cast<u32>(State::Emulator), std::memory_order_relaxed);
        active = true;
    }

    ~SwitchScope() {
        const size_t core = CurrentCore();
        if (active && core < NumCores) {
            GetShared().states[core].store(saved, std::memory_order_relaxed);
        }
    }

    SwitchScope(const SwitchScope&) = delete;
    SwitchScope& operator=(const SwitchScope&) = delete;

private:
    u32 saved{};
    bool active{};
};

/// An SVC on an emulated core: its state while it runs, and one call counted when it starts.
class SvcScope {
public:
    explicit SvcScope(u32 svc) : scope{SvcBase + (svc & 0xff)} {
        if (CurrentCore() < NumCores) {
            GetShared().svc_calls[svc & 0xff].fetch_add(1, std::memory_order_relaxed);
        }
    }

private:
    Scope scope;
};

} // namespace Core::Nx2CpuProfile
