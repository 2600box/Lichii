// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <chrono>
#include <cstdlib>

#include "common/common_types.h"
#include "common/logging.h"

/// EGLA-66 sweep 3 test-only candidates for the split GPU back thread's CPU cost (never merge as
/// gates: a proven candidate ships unconditional, a disproven one is dropped). Kept apart from
/// nx2_egla66.h (capy/egla66-ports) so the two test branches merge without conflicts; same
/// pattern. Each is off unless EDEN_NX2_EGLA66_<NAME>=1. ReadSwitches reads them once per GPU
/// creation, before the split starts (after nx2_switches.txt is loaded); off keeps the previous
/// behaviour. Tests set the flags directly. Report() logs each enabled candidate's counters
/// every 10 s.
namespace VideoCommon::Nx2Egla66Sweep3 {

/// A dirty but disabled vertex stream binds nothing without translating its address
/// (Hydro74000 c6c531046f).
inline bool vstream{};

inline std::atomic<u64> g_vstream_skipped{0};

inline void ReadSwitches() {
    const auto read = [](const char* name) {
        const char* const value = std::getenv(name);
        return value != nullptr && value[0] == '1';
    };
    vstream = read("EDEN_NX2_EGLA66_VSTREAM");
    LOG_INFO(HW_GPU, "EGLA66 sweep 3 gates: vstream {}", vstream);
}

/// For counters one thread writes: a plain load and store, not a locked add.
inline void Count(std::atomic<u64>& counter, u64 amount = 1) {
    counter.store(counter.load(std::memory_order_relaxed) + amount, std::memory_order_relaxed);
}

/// The split's back thread, once per loop: logs what the enabled candidates did in the last 10 s.
inline void Report() {
    if (!vstream) {
        return;
    }
    static u32 calls{};
    if ((++calls & 1023) != 0) {
        return;
    }
    using Clock = std::chrono::steady_clock;
    static Clock::time_point last = Clock::now();
    const Clock::time_point now = Clock::now();
    if (now - last < std::chrono::seconds{10}) {
        return;
    }
    last = now;
    const auto take = [](std::atomic<u64>& counter, u64& reported) {
        const u64 value = counter.load(std::memory_order_relaxed);
        const u64 delta = value - reported;
        reported = value;
        return delta;
    };
    if (vstream) {
        static u64 skipped{};
        LOG_INFO(HW_GPU, "EGLA66 vstream: {} disabled vertex streams not translated in 10 s",
                 take(g_vstream_skipped, skipped));
    }
}

} // namespace VideoCommon::Nx2Egla66Sweep3
