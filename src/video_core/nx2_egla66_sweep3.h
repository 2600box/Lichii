// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <string>

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

/// GPU::TickWork returns without its mutex when every sync request made has run
/// (jpkto 20628c90ae).
inline bool tickwork{};

inline std::atomic<u64> g_tickwork_skipped{0};

/// GPUDirtyMemoryManager::Gather returns before its locks when nothing was collected, and
/// InvalidateGPUCache keeps one write buffer per thread (AManWithAPlan e073cc0b9e). Exact: no
/// verify mode, the [gpu_dirty_memory] test runs both ways.
inline bool gather{};

/// A small uniform buffer bound at a new address keeps no CPU copy until that address comes back
/// (Quixomatic ed4efcbc76). It only compares fewer binds, so no verify mode.
inline bool ubo_newaddr{};

inline std::atomic<u64> g_ubo_newaddr_uncopied{0};

/// Graphics storage buffers and classic-path uniform buffers skip their upload scan while the
/// memory tracker reports the range synchronized since the last one, as the geometry scans do
/// (ef12df65f7; BufferCache::SynchronizeBindingBuffer). _VERIFY (or EDEN_NX2_PERF_VERIFY) runs
/// each skipped scan anyway and counts a mismatch when it uploads.
inline bool storage_sync_skip{};
inline bool storage_sync_skip_verify{};

inline std::atomic<u64> g_storage_scans{0};
inline std::atomic<u64> g_storage_scans_skipped{0};
inline std::atomic<u64> g_uniform_scans{0};
inline std::atomic<u64> g_uniform_scans_skipped{0};
inline std::atomic<u64> g_sync_skip_mismatches{0};

inline void ReadSwitches() {
    const auto read = [](const char* name) {
        const char* const value = std::getenv(name);
        return value != nullptr && value[0] == '1';
    };
    vstream = read("EDEN_NX2_EGLA66_VSTREAM");
    tickwork = read("EDEN_NX2_EGLA66_TICKWORK");
    gather = read("EDEN_NX2_EGLA66_GATHER");
    ubo_newaddr = read("EDEN_NX2_EGLA66_UBO_NEWADDR");
    storage_sync_skip = read("EDEN_NX2_EGLA66_STORAGE_SYNC_SKIP");
    storage_sync_skip_verify =
        storage_sync_skip &&
        (read("EDEN_NX2_EGLA66_STORAGE_SYNC_SKIP_VERIFY") || read("EDEN_NX2_PERF_VERIFY"));
    LOG_INFO(HW_GPU,
             "EGLA66 sweep 3 gates: vstream {}, tickwork {}, gather {}, ubo-newaddr {}, "
             "storage-sync-skip {} (verify {})",
             vstream, tickwork, gather, ubo_newaddr, storage_sync_skip, storage_sync_skip_verify);
}

/// For counters one thread writes: a plain load and store, not a locked add.
inline void Count(std::atomic<u64>& counter, u64 amount = 1) {
    counter.store(counter.load(std::memory_order_relaxed) + amount, std::memory_order_relaxed);
}

/// The split's back thread, once per loop: logs what the enabled candidates did in the last 10 s.
inline void Report() {
    if (!vstream && !tickwork && !ubo_newaddr && !storage_sync_skip) {
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
    if (tickwork) {
        static u64 skipped{};
        LOG_INFO(HW_GPU, "EGLA66 tickwork: {} ticks returned without the mutex in 10 s",
                 take(g_tickwork_skipped, skipped));
    }
    if (ubo_newaddr) {
        static u64 uncopied{};
        LOG_INFO(HW_GPU,
                 "EGLA66 ubo-newaddr: {} small uniform binds at a new address kept no CPU copy in "
                 "10 s",
                 take(g_ubo_newaddr_uncopied, uncopied));
    }
    if (storage_sync_skip) {
        static u64 storage{};
        static u64 storage_skipped{};
        static u64 uniform{};
        static u64 uniform_skipped{};
        static u64 mismatches{};
        const u64 new_storage = take(g_storage_scans, storage);
        const u64 new_storage_skipped = take(g_storage_scans_skipped, storage_skipped);
        const u64 new_uniform = take(g_uniform_scans, uniform);
        const u64 new_uniform_skipped = take(g_uniform_scans_skipped, uniform_skipped);
        const u64 new_mismatches = take(g_sync_skip_mismatches, mismatches);
        LOG_INFO(HW_GPU,
                 "EGLA66 storage-sync-skip: storage scans {} of {} skipped, classic uniform scans "
                 "{} of {} skipped in 10 s{}",
                 new_storage_skipped, new_storage, new_uniform_skipped, new_uniform,
                 storage_sync_skip_verify
                     ? fmt::format(", {} verify mismatches", new_mismatches)
                     : std::string{});
    }
}

} // namespace VideoCommon::Nx2Egla66Sweep3
