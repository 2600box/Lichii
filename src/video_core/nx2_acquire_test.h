// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Test build only (EGLA-86 A/B, EGLA-94 mirror A/B): acquire suspension histogram and semaphore
// mirror counters, off unless EDEN_NX2_ACQUIRE_STATS is set. EDEN_NX2_SEM_MIRROR=0 turns the
// mirror off. Not for nx2.

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <unordered_set>

#include "common/common_types.h"
#include "common/logging.h"
#include "video_core/nx2.h"

namespace VideoCommon::Nx2AcquireTest {

inline const bool g_stats = [] {
    const char* value = std::getenv("EDEN_NX2_ACQUIRE_STATS");
    return value != nullptr && value[0] != '\0' && value[0] != '0';
}();

inline const bool g_mirror = [] {
    const char* value = std::getenv("EDEN_NX2_SEM_MIRROR");
    return value == nullptr || value[0] != '0';
}();

/// True when address is in the first page of a registered two-page semaphore region.
inline bool InFirstPage(u64 address) {
    for (const auto& region : Tegra::g_nx2_semaphore_regions) {
        const u64 start = region.load(std::memory_order_relaxed);
        if (start != 0 && address >= start && address < start + 0x1000) {
            return true;
        }
    }
    return false;
}

/// Set by the puller around its mirrored Query, so the report hook can tell mirrored writes apart.
inline thread_local bool t_mirrored_query = false;

inline u32 RegisteredRegions() {
    u32 count = 0;
    for (const auto& region : Tegra::g_nx2_semaphore_regions) {
        count += region.load(std::memory_order_relaxed) != 0;
    }
    return count;
}

// Buckets: <1 ms, <10 ms, <100 ms, <500 ms, <2 s, >=2 s.
class Stats {
public:
    static Stats& Get() {
        static Stats instance;
        return instance;
    }
    /// A report or semaphore write that is not the mirror's. A direct write into a first-page word
    /// the mirror also targets is the collision of the #979 review (jpasqui f8941169650).
    void FirstPageWrite(u64 address) {
        if (!g_stats || t_mirrored_query || !InFirstPage(address)) {
            return;
        }
        first_page_writes.fetch_add(1, std::memory_order_relaxed);
        std::scoped_lock lock{words_mutex};
        if (direct_words.size() < 4096) {
            direct_words.insert(address & ~u64{3});
        }
    }
    /// A second-page release whose first-page word the mirror writes (or would, with it off).
    void MirrorTarget(u64 address) {
        if (!g_stats || address == 0) {
            return;
        }
        std::scoped_lock lock{words_mutex};
        if (mirror_words.size() < 4096) {
            mirror_words.insert(address & ~u64{3});
        }
    }
    void Shadowed() {
        if (g_stats) {
            shadowed.fetch_add(1, std::memory_order_relaxed);
        }
    }
    void Mirrored() {
        if (g_stats) {
            mirrored.fetch_add(1, std::memory_order_relaxed);
        }
    }
    /// A suspension starts on a first-page word; second_page_meets: the matching second-page word
    /// already satisfies it (the mirror would have).
    void FirstPageSuspend(bool second_page_meets) {
        if (g_stats) {
            first_page_suspends.fetch_add(1, std::memory_order_relaxed);
            second_page_meets_count.fetch_add(second_page_meets ? 1 : 0, std::memory_order_relaxed);
        }
    }
    void Acquire(std::chrono::steady_clock::duration wait, bool first_page = false) {
        if (!g_stats) {
            return;
        }
        const u64 us = static_cast<u64>(
            std::chrono::duration_cast<std::chrono::microseconds>(wait).count());
        const size_t bucket = us < 1000 ? 0 : us < 10000 ? 1 : us < 100000 ? 2 : us < 500000 ? 3
                            : us < 2000000 ? 4 : 5;
        buckets[bucket].fetch_add(1, std::memory_order_relaxed);
        total_us.fetch_add(us, std::memory_order_relaxed);
        for (u64 seen = max_us.load(std::memory_order_relaxed);
             us > seen && !max_us.compare_exchange_weak(seen, us, std::memory_order_relaxed);) {
        }
        if (first_page) {
            first_page_waits.fetch_add(1, std::memory_order_relaxed);
            first_page_total_us.fetch_add(us, std::memory_order_relaxed);
            for (u64 seen = first_page_max_us.load(std::memory_order_relaxed);
                 us > seen &&
                 !first_page_max_us.compare_exchange_weak(seen, us, std::memory_order_relaxed);) {
            }
        }
        const s64 now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now().time_since_epoch())
                               .count();
        s64 last = last_ms.load(std::memory_order_relaxed);
        if (last == 0) {
            last_ms.compare_exchange_strong(last, now_ms);
            return;
        }
        if (now_ms - last < 10000 || !last_ms.compare_exchange_strong(last, now_ms)) {
            return;
        }
        std::array<u64, 6> b{};
        for (size_t i = 0; i < b.size(); ++i) {
            b[i] = buckets[i].exchange(0, std::memory_order_relaxed);
        }
        LOG_INFO(HW_GPU,
                 "EGLA86 acquire waits <1/<10/<100/<500/<2000/>=2000 ms: {} {} {} {} {} {}, total "
                 "{} ms, max {} ms",
                 b[0], b[1], b[2], b[3], b[4], b[5],
                 total_us.exchange(0, std::memory_order_relaxed) / 1000,
                 max_us.exchange(0, std::memory_order_relaxed) / 1000);
        u64 shared_words = 0;
        size_t direct_count = 0, mirror_count = 0;
        {
            std::scoped_lock lock{words_mutex};
            for (const u64 word : direct_words) {
                shared_words += mirror_words.contains(word) ? 1 : 0;
            }
            direct_count = direct_words.size();
            mirror_count = mirror_words.size();
        }
        LOG_INFO(HW_GPU,
                 "EGLA94 words since boot: first-page direct {}, mirror targets {}, both {}; "
                 "shadowed releases {}",
                 direct_count, mirror_count, shared_words,
                 shadowed.exchange(0, std::memory_order_relaxed));
        LOG_INFO(HW_GPU,
                 "EGLA94 mirror {}: mirrored releases {}, first-page direct writes {}, regions {}, "
                 "first-page suspends {} (second page already met {}), first-page waits {} "
                 "total {} ms max {} ms",
                 g_mirror ? "on" : "off", mirrored.exchange(0, std::memory_order_relaxed),
                 first_page_writes.exchange(0, std::memory_order_relaxed),
                 RegisteredRegions(), first_page_suspends.exchange(0, std::memory_order_relaxed),
                 second_page_meets_count.exchange(0, std::memory_order_relaxed),
                 first_page_waits.exchange(0, std::memory_order_relaxed),
                 first_page_total_us.exchange(0, std::memory_order_relaxed) / 1000,
                 first_page_max_us.exchange(0, std::memory_order_relaxed) / 1000);
    }

private:
    std::array<std::atomic<u64>, 6> buckets{};
    std::atomic<u64> total_us{}, max_us{};
    std::atomic<u64> shadowed{}, mirrored{}, first_page_writes{}, first_page_suspends{}, second_page_meets_count{};
    std::atomic<u64> first_page_waits{}, first_page_total_us{}, first_page_max_us{};
    std::atomic<s64> last_ms{};
    std::mutex words_mutex;
    std::unordered_set<u64> direct_words, mirror_words;
};

} // namespace VideoCommon::Nx2AcquireTest
