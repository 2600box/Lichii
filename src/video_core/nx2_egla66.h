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
/// EDEN_NX2_EGLA66_<NAME>=1, and each works alone; <NAME>_VERIFY=1 runs the original path and
/// counts where the candidate's result would differ. ReadSwitches reads them all once per GPU
/// creation, before the renderer and the split start (after nx2_switches.txt is loaded), and logs
/// them in one line; off keeps the previous behaviour. Tests set the flags directly. Report() logs
/// the ring-run, publish, state-run, ubo-mask and ubo-rebind counters every 10 s; the acquire,
/// ubo-keep and delta-descriptors owners log their own.
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

/// The split's back runs consecutive 3D state writes in one loop (9b06c4fd38's
/// CallStateMethodRun, AManWithAPlan).
inline bool state_run{};

inline std::atomic<u64> g_state_runs{0};
inline std::atomic<u64> g_state_run_writes{0};

/// The buffer cache visits only uniform buffer slots bound since their last resolve
/// (9b06c4fd38's unresolved_uniform_buffers). _VERIFY checks the slots it skips.
inline bool ubo_mask{};
inline bool ubo_mask_verify{};

inline std::atomic<u64> g_ubo_mask_skipped{0};
inline std::atomic<u64> g_ubo_mask_visited{0};
inline std::atomic<u64> g_ubo_mask_mismatches{0};

/// A uniform buffer bound again to the range it holds keeps its resolved buffer (9b06c4fd38's
/// same-address rebind). _VERIFY resolves kept slots again at the next update and compares.
inline bool ubo_rebind{};
inline bool ubo_rebind_verify{};

inline std::atomic<u64> g_ubo_rebind_kept{0};
inline std::atomic<u64> g_ubo_rebind_bound{0};
inline std::atomic<u64> g_ubo_rebind_mismatches{0};

/// The front passes greater-or-equal semaphore acquires it can decide without draining the
/// split, polls waiting ones without a drain (at most one per 1 ms), and feeds and settles the
/// back while channels wait (GpuSplit::acquire_skip and acquire_feed; jpkto fb4d4e47a7, rdenno
/// #639, PantACRO4life's valve). _PERF4 keeps perf-4's version, which does not feed the back.
/// With publish on too, publish already sends each entry to an idle back, so the feed's early
/// publish rarely fires (GpuSplit::Commit); the settling still runs.
inline bool acquire{};
inline bool acquire_perf4{};
inline bool acquire_verify{};

/// A draw binds a small uniform buffer from the stream allocation the previous draw made for it,
/// without reading guest memory, when nothing between the two wrote its range: delta draws
/// (ubo_keep), and full draws only register writes separate (ubo_keep_full). BufferCache's
/// UniformKeep. _VERIFY alone runs the original path and compares.
inline bool ubo_keep{};
inline bool ubo_keep_verify{};
inline bool ubo_keep_full{};
inline bool ubo_keep_full_verify{};

/// Delta draws push the image descriptor entries kept from the previous draw instead of resolving
/// every view and sampler again (GraphicsPipeline's kept images). _VERIFY alone resolves them and
/// compares.
inline bool delta_descriptors{};
inline bool delta_descriptors_verify{};

inline void ReadSwitches() {
    const auto read = [](const char* name) {
        const char* const value = std::getenv(name);
        return value != nullptr && value[0] == '1';
    };
    ring_run = read("EDEN_NX2_EGLA66_RING_RUN");
    publish = read("EDEN_NX2_EGLA66_PUBLISH");
    state_run = read("EDEN_NX2_EGLA66_STATE_RUN");
    ubo_mask = read("EDEN_NX2_EGLA66_UBO_MASK");
    ubo_mask_verify = ubo_mask && read("EDEN_NX2_EGLA66_UBO_MASK_VERIFY");
    ubo_rebind = read("EDEN_NX2_EGLA66_UBO_REBIND");
    ubo_rebind_verify = ubo_rebind && read("EDEN_NX2_EGLA66_UBO_REBIND_VERIFY");
    acquire = read("EDEN_NX2_EGLA66_ACQUIRE");
    acquire_perf4 = acquire && read("EDEN_NX2_EGLA66_ACQUIRE_PERF4");
    acquire_verify = acquire && read("EDEN_NX2_EGLA66_ACQUIRE_VERIFY");
    ubo_keep = read("EDEN_NX2_EGLA66_UBO_KEEP");
    ubo_keep_verify = read("EDEN_NX2_EGLA66_UBO_KEEP_VERIFY");
    ubo_keep_full = read("EDEN_NX2_EGLA66_UBO_KEEP_FULL");
    ubo_keep_full_verify = read("EDEN_NX2_EGLA66_UBO_KEEP_FULL_VERIFY");
    delta_descriptors = read("EDEN_NX2_EGLA66_DELTA_DESCRIPTORS");
    delta_descriptors_verify = read("EDEN_NX2_EGLA66_DELTA_DESCRIPTORS_VERIFY");
    LOG_INFO(HW_GPU,
             "EGLA66 gates: RING_RUN={:d} PUBLISH={:d} STATE_RUN={:d} UBO_MASK={:d} "
             "UBO_MASK_VERIFY={:d} UBO_REBIND={:d} UBO_REBIND_VERIFY={:d} ACQUIRE={:d} "
             "ACQUIRE_PERF4={:d} ACQUIRE_VERIFY={:d} UBO_KEEP={:d} UBO_KEEP_VERIFY={:d} "
             "UBO_KEEP_FULL={:d} UBO_KEEP_FULL_VERIFY={:d} DELTA_DESCRIPTORS={:d} "
             "DELTA_DESCRIPTORS_VERIFY={:d}",
             ring_run, publish, state_run, ubo_mask, ubo_mask_verify, ubo_rebind,
             ubo_rebind_verify, acquire, acquire_perf4, acquire_verify, ubo_keep, ubo_keep_verify,
             ubo_keep_full, ubo_keep_full_verify, delta_descriptors, delta_descriptors_verify);
}

/// Whether a 10 s counter window ended (and starts the next one).
[[nodiscard]] inline bool WindowEnded(std::chrono::steady_clock::time_point& window_end) {
    const auto now = std::chrono::steady_clock::now();
    if (now < window_end) {
        return false;
    }
    const bool first = window_end == std::chrono::steady_clock::time_point{};
    window_end = now + std::chrono::seconds{10};
    return !first;
}

/// For counters one thread writes: a plain load and store, not a locked add.
inline void Count(std::atomic<u64>& counter, u64 amount = 1) {
    counter.store(counter.load(std::memory_order_relaxed) + amount, std::memory_order_relaxed);
}

/// The split's back thread, once per loop: logs what the enabled candidates did in the last 10 s.
inline void Report() {
    if (!ring_run && !publish && !state_run && !ubo_mask && !ubo_rebind) {
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
    if (state_run) {
        static u64 runs{};
        static u64 writes{};
        const u64 new_runs = take(g_state_runs, runs);
        const u64 new_writes = take(g_state_run_writes, writes);
        LOG_INFO(HW_GPU, "EGLA66 state-run: {} runs carrying {} state writes in 10 s", new_runs,
                 new_writes);
    }
    if (ubo_mask) {
        static u64 skipped{};
        static u64 visited{};
        static u64 mismatches{};
        const u64 new_skipped = take(g_ubo_mask_skipped, skipped);
        const u64 new_visited = take(g_ubo_mask_visited, visited);
        const u64 new_mismatches = take(g_ubo_mask_mismatches, mismatches);
        LOG_INFO(HW_GPU,
                 "EGLA66 ubo-mask: {} enabled uniform slots skipped, {} visited in 10 s{}",
                 new_skipped, new_visited,
                 ubo_mask_verify ? fmt::format(", {} mismatches (verify)", new_mismatches) : "");
    }
    if (ubo_rebind) {
        static u64 kept{};
        static u64 bound{};
        static u64 mismatches{};
        const u64 new_kept = take(g_ubo_rebind_kept, kept);
        const u64 new_bound = take(g_ubo_rebind_bound, bound);
        const u64 new_mismatches = take(g_ubo_rebind_mismatches, mismatches);
        LOG_INFO(HW_GPU,
                 "EGLA66 ubo-rebind: {} uniform binds kept their buffer, {} bound anew in 10 s{}",
                 new_kept, new_bound,
                 ubo_rebind_verify ? fmt::format(", {} mismatches (verify)", new_mismatches) : "");
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
