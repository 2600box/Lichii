// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// EXPERIMENT (test package only, never merge): live A/B switches for the GPU thread work. A file
// named after a switch in EDEN_NX2_DEBUG_DIR turns it on; Refresh reads them every 250 ms.

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <utility>

#include "common/common_types.h"
#include "common/logging.h"

namespace VideoCommon::Nx2Experiment {

enum Switch : u32 {
    BackPicksOff = 1,  ///< back-perf-off: no empty-draw skip, no clean-word early-out
    EntryWaitSkip = 2, ///< entry-wait-skip: entries on pages read before skip the writer wait
};

inline constexpr std::array<std::pair<Switch, const char*>, 2> switch_files{{
    {BackPicksOff, "back-perf-off"},
    {EntryWaitSkip, "entry-wait-skip"},
}};

inline std::atomic<u32> g_on{0};
inline std::atomic<u64> g_entry_skips{0};
inline std::atomic<u64> g_entry_misses{0};

[[nodiscard]] inline bool On(Switch which) {
    return (g_on.load(std::memory_order_relaxed) & which) != 0;
}

/// Called once a frame from one thread.
inline void Refresh() {
    using Clock = std::chrono::steady_clock;
    static Clock::time_point next_check{};
    static Clock::time_point next_report{};
    const Clock::time_point now = Clock::now();
    if (now < next_check) {
        return;
    }
    next_check = now + std::chrono::milliseconds{250};
    const char* const dir = std::getenv("EDEN_NX2_DEBUG_DIR");
    u32 on = 0;
    if (dir != nullptr) {
        for (const auto& [which, file] : switch_files) {
            std::error_code ec;
            if (std::filesystem::exists(std::filesystem::path{dir} / file, ec)) {
                on |= which;
            }
        }
    }
    const u32 before = g_on.exchange(on, std::memory_order_relaxed);
    for (const auto& [which, file] : switch_files) {
        if (((before ^ on) & which) != 0) {
            LOG_INFO(HW_GPU, "EXPERIMENT {} {}", file, (on & which) != 0 ? "on" : "off");
        }
    }
    if (now >= next_report) {
        next_report = now + std::chrono::seconds{10};
        const u64 skips = g_entry_skips.exchange(0, std::memory_order_relaxed);
        const u64 misses = g_entry_misses.load(std::memory_order_relaxed);
        if (skips != 0 || misses != 0) {
            LOG_INFO(HW_GPU, "EXPERIMENT entry-wait-skip: {} writer waits skipped in 10 s, {} missed GPU writes in total",
                     skips, misses);
        }
    }
}

} // namespace VideoCommon::Nx2Experiment
