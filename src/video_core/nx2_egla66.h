// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <chrono>
#include <cstdlib>

#include "common/common_types.h"
#include "common/logging.h"

/// EGLA-66 test-only candidates for the split GPU back thread's CPU cost (never merge as gates:
/// a proven candidate ships unconditional, a disproven one is dropped). Each is off unless
/// EDEN_NX2_EGLA66_<NAME>=1. ReadSwitches reads them once per GPU creation, before the split
/// starts (after nx2_switches.txt is loaded); off keeps the previous behaviour. Tests set the
/// flags directly. Report() logs each enabled candidate's counters every 10 s.
namespace VideoCommon::Nx2Egla66 {

/// Consecutive macro writes to one engine share one split ring Run entry (ab22f858eb).
inline bool ring_run{};

inline std::atomic<u64> g_ring_runs{0};
inline std::atomic<u64> g_ring_run_writes{0};

/// While the split's back is idle the front publishes each entry at once, and before it yields
/// in the pending-channel poll (#717 PUBLISH_IDLE and PUBLISH_WAITS, stephenjayakar).
inline bool publish{};

inline std::atomic<u64> g_idle_publishes{0};
inline std::atomic<u64> g_wait_publishes{0};

inline void ReadSwitches() {
    const auto read = [](const char* name) {
        const char* const value = std::getenv(name);
        return value != nullptr && value[0] == '1';
    };
    ring_run = read("EDEN_NX2_EGLA66_RING_RUN");
    publish = read("EDEN_NX2_EGLA66_PUBLISH");
    LOG_INFO(HW_GPU, "EGLA66 gates: ring-run {}, publish {}", ring_run, publish);
}

/// For counters one thread writes: a plain load and store, not a locked add.
inline void Count(std::atomic<u64>& counter, u64 amount = 1) {
    counter.store(counter.load(std::memory_order_relaxed) + amount, std::memory_order_relaxed);
}

/// The split's back thread, once per loop: logs what the enabled candidates did in the last 10 s.
inline void Report() {
    if (!ring_run && !publish) {
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
    if (ring_run) {
        static u64 runs{};
        static u64 writes{};
        const u64 new_runs = take(g_ring_runs, runs);
        const u64 new_writes = take(g_ring_run_writes, writes);
        LOG_INFO(HW_GPU, "EGLA66 ring-run: {} run entries carrying {} macro writes in 10 s",
                 new_runs, new_writes);
    }
    if (publish) {
        static u64 idle{};
        static u64 waits{};
        const u64 new_idle = take(g_idle_publishes, idle);
        const u64 new_waits = take(g_wait_publishes, waits);
        LOG_INFO(HW_GPU,
                 "EGLA66 publish: {} publishes to an idle back, {} before a pending-channel wait "
                 "in 10 s",
                 new_idle, new_waits);
    }
}

} // namespace VideoCommon::Nx2Egla66
