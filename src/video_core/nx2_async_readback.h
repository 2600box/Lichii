// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>

#include "common/common_types.h"
#include "common/logging.h"

// EDEN_NX2_ASYNC_READBACK: how a guest CPU read of GPU-written buffer memory gets its bytes
// (GPU::OnCPURead).
// - unset or 0: the GPU thread downloads them and waits for the host GPU (Scheduler::Finish).
// - 1: the GPU thread submits the downloads and goes on. The reading thread waits for that
//   submission and writes the bytes to guest memory (BufferCache::FinishReadDownloads). Pages
//   the guest reads after each GPU write get their download right behind the write.
// - verify: 1, and every read download is compared with a synchronous flush's copy taken at the
//   same point of the GPU's work (BufferCache::CheckReadDownload).
namespace VideoCore::AsyncReadback {

enum class Mode : u8 { Off, On, Verify };

[[nodiscard]] inline Mode ParseMode(const char* value) {
    if (value == nullptr || value[0] == '\0' || std::strcmp(value, "0") == 0) {
        return Mode::Off;
    }
    return std::strcmp(value, "verify") == 0 ? Mode::Verify : Mode::On;
}

inline std::atomic<Mode> g_mode{ParseMode(std::getenv("EDEN_NX2_ASYNC_READBACK"))};

/// Called by VideoCore::CreateGPU: static initialization runs before the frontend loads
/// nx2_switches.txt, so a switch set only there takes effect from that point on.
inline void ReadSwitch() {
    g_mode.store(ParseMode(std::getenv("EDEN_NX2_ASYNC_READBACK")), std::memory_order_relaxed);
}

[[nodiscard]] inline Mode GetMode() {
    return g_mode.load(std::memory_order_relaxed);
}

[[nodiscard]] inline bool Enabled() {
    return GetMode() != Mode::Off;
}

[[nodiscard]] inline bool Verify() {
    return GetMode() == Mode::Verify;
}

/// Who wrote a read download to guest memory, named by the verify mode's mismatch lines.
enum class Landing : u8 {
    Reader,       ///< The guest thread whose read waited for it (GPU::OnCPURead)
    GpuSideWrite, ///< A write to guest memory from the GPU's side over its bytes
    Completed,    ///< Whichever thread found the host GPU had run it, before a read waited
    Count,
};

inline constexpr std::array<const char*, static_cast<size_t>(Landing::Count)> landing_names{
    "the reading thread",
    "a GPU-side write over it",
    "a thread that found it run by the host GPU",
};

/// Set on a guest thread while GPU::OnCPURead writes its downloads to guest memory.
inline thread_local bool t_reading{};

[[nodiscard]] inline u64 Now() {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count());
}

enum class Read : u8 {
    Sync,     ///< The GPU thread downloaded and waited for the host GPU (unset)
    Async,    ///< The GPU thread submitted the downloads, the reading thread waited for them
    InFlight, ///< An earlier read's or an early download covered it: no flush, only the wait
    Count,
};

/// Totals for the per-second line, from every thread.
struct Counters {
    std::array<std::atomic<u64>, static_cast<size_t>(Read::Count)> reads{};
    /// From the read's request to its bytes in guest memory
    std::array<std::atomic<u64>, static_cast<size_t>(Read::Count)> read_ns{};
    /// The GPU thread's time in the reads' flushes
    std::atomic<u64> flush_ns{};
    /// Reading threads whose download the host GPU had not run yet
    std::atomic<u64> reader_waits{};
    std::atomic<u64> reader_wait_ns{};
    std::atomic<u64> reader_max_wait_ns{};
    /// Writes from the GPU's side that waited for the host GPU to run a read download under them
    std::atomic<u64> gpu_side_waits{};
    std::atomic<u64> gpu_side_wait_ns{};
    std::atomic<u64> window_start{};
};

inline Counters g_counters;

inline void Add(std::atomic<u64>& counter, u64 amount) {
    counter.fetch_add(amount, std::memory_order_relaxed);
}

inline void CountRead(Read kind, u64 ns) {
    Add(g_counters.reads[static_cast<size_t>(kind)], 1);
    Add(g_counters.read_ns[static_cast<size_t>(kind)], ns);
}

/// BufferCache::FinishReadDownloads waited ns for the host GPU.
inline void CountHostWait(u64 ns) {
    if (!t_reading) {
        Add(g_counters.gpu_side_waits, 1);
        Add(g_counters.gpu_side_wait_ns, ns);
        return;
    }
    Add(g_counters.reader_waits, 1);
    Add(g_counters.reader_wait_ns, ns);
    u64 max = g_counters.reader_max_wait_ns.load(std::memory_order_relaxed);
    while (ns > max && !g_counters.reader_max_wait_ns.compare_exchange_weak(
                           max, ns, std::memory_order_relaxed)) {
    }
}

/// Logs the totals every 10 s and resets them, on whichever reading thread comes first.
inline void MaybeReport() {
    auto& c = g_counters;
    const u64 now = Now();
    u64 start = c.window_start.load(std::memory_order_relaxed);
    if (start == 0) {
        c.window_start.compare_exchange_strong(start, now, std::memory_order_relaxed);
        return;
    }
    if (now - start < 10'000'000'000ULL ||
        !c.window_start.compare_exchange_strong(start, now, std::memory_order_relaxed)) {
        return;
    }
    const double window = static_cast<double>(now - start);
    const auto take = [](std::atomic<u64>& counter) {
        return counter.exchange(0, std::memory_order_relaxed);
    };
    const auto per_second = [window](u64 value) {
        return static_cast<double>(value) * 1e9 / window;
    };
    const auto ms_per_second = [window](u64 ns) { return static_cast<double>(ns) * 1e3 / window; };
    std::array<u64, static_cast<size_t>(Read::Count)> reads{};
    std::array<u64, static_cast<size_t>(Read::Count)> read_ns{};
    for (size_t i = 0; i < reads.size(); ++i) {
        reads[i] = take(c.reads[i]);
        read_ns[i] = take(c.read_ns[i]);
    }
    const u64 flush_ns = take(c.flush_ns);
    const u64 reader_waits = take(c.reader_waits);
    const u64 reader_wait_ns = take(c.reader_wait_ns);
    const u64 reader_max_wait_ns = take(c.reader_max_wait_ns);
    const u64 gpu_side_waits = take(c.gpu_side_waits);
    const u64 gpu_side_wait_ns = take(c.gpu_side_wait_ns);
    constexpr auto sync = static_cast<size_t>(Read::Sync);
    constexpr auto async = static_cast<size_t>(Read::Async);
    constexpr auto in_flight = static_cast<size_t>(Read::InFlight);
    static constexpr std::array<const char*, 3> mode_names{"unset", "1", "verify"};
    LOG_INFO(HW_GPU,
             "Async read-back (EDEN_NX2_ASYNC_READBACK={}) per second: reads {:.0f} synchronous "
             "({:.1f} ms), {:.0f} async ({:.1f} ms), {:.0f} only waited for a download on its way "
             "({:.1f} ms) | GPU thread in read flushes {:.1f} ms | readers waited on their fence "
             "{:.0f} times, {:.1f} ms (longest {:.2f} ms) | GPU-side writes fell back to a host "
             "GPU wait {:.0f} times, {:.1f} ms",
             mode_names[static_cast<size_t>(GetMode())], per_second(reads[sync]),
             ms_per_second(read_ns[sync]), per_second(reads[async]), ms_per_second(read_ns[async]),
             per_second(reads[in_flight]), ms_per_second(read_ns[in_flight]),
             ms_per_second(flush_ns), per_second(reader_waits), ms_per_second(reader_wait_ns),
             static_cast<double>(reader_max_wait_ns) / 1e6, per_second(gpu_side_waits),
             ms_per_second(gpu_side_wait_ns));
}

} // namespace VideoCore::AsyncReadback
