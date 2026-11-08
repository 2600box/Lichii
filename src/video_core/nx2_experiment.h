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
    UboInPlace = 4,    ///< ubo-inplace: compare small uniform buffers in guest memory, no read copy
    CbWriteCache = 8,  ///< cb-write-cache: constant buffer words keep their page's translation
    TextureMemo = 16,  ///< texture-memo: a draw's sampled textures are resolved from the last draw's
    TextureMemoVerify = 32, ///< texture-memo-verify: with texture-memo, also resolve in full and compare
    DynamicStateSkip = 64,  ///< dynamic-state-skip: no dynamic state update while no flag is dirty
    GeometryMemo = 128,     ///< geometry-memo: index/vertex lookups kept while their inputs hold
    GeometrySyncSkip = 256, ///< geometry-sync-skip: no upload scan of unwritten index/vertex ranges
    NativePreflush = 512,   ///< native-preflush: Switch 2 CPU reads mark pages for fence downloads
    FastDraw = 1024,        ///< fast-draw: delta draws after light state changes (nx2_fast_draw.h)
    FastDrawVerify = 2048,  ///< fast-draw-verify: resolve those draws in full and count mismatches
    MacroDmaNarrow = 4096,  ///< macro-dma-narrow: macro memory reads wait for memory writers only
    PipelineClean = 8192,   ///< pipeline-clean: no pipeline key refresh while its inputs hold (1c8772da73)
    FlushCoalesce = 16384,  ///< flush-coalesce: adjacent device pages flush in one call (9bd828ce55)
    AliasSkip = 32768,      ///< alias-skip: no alias walk while no alias is newer (e368878151)
    ResetBatch = 65536,     ///< reset-batch: indirect reset records written in merged runs (b33ef6014b)
    AcquireMetSkip = 262144, ///< acquire-met-skip: met GEQUAL acquires skip the drain (fb4d4e47a7)
    DmaReadKnown = 524288,   ///< dma-read-known: macro reads of report memory skip the drain (#243)
    WriterWaitSynced = 1048576, ///< writer-wait-synced: lists wait for writers up to the last sync
    MacroRingRun = 131072,  ///< macro-ring-run: macro writes share one split ring entry (ab22f858eb)
};

inline constexpr std::array<std::pair<Switch, const char*>, 21> switch_files{{
    {BackPicksOff, "back-perf-off"},
    {EntryWaitSkip, "entry-wait-skip"},
    {UboInPlace, "ubo-inplace"},
    {CbWriteCache, "cb-write-cache"},
    {TextureMemo, "texture-memo"},
    {TextureMemoVerify, "texture-memo-verify"},
    {DynamicStateSkip, "dynamic-state-skip"},
    {GeometryMemo, "geometry-memo"},
    {GeometrySyncSkip, "geometry-sync-skip"},
    {NativePreflush, "native-preflush"},
    {FastDraw, "fast-draw"},
    {FastDrawVerify, "fast-draw-verify"},
    {MacroDmaNarrow, "macro-dma-narrow"},
    {PipelineClean, "pipeline-clean"},
    {FlushCoalesce, "flush-coalesce"},
    {AliasSkip, "alias-skip"},
    {ResetBatch, "reset-batch"},
    {AcquireMetSkip, "acquire-met-skip"},
    {DmaReadKnown, "dma-read-known"},
    {WriterWaitSynced, "writer-wait-synced"},
    {MacroRingRun, "macro-ring-run"},
}};

inline std::atomic<u32> g_on{0};
inline std::atomic<u64> g_entry_skips{0};
inline std::atomic<u64> g_entry_misses{0};
inline std::atomic<u64> g_dma_narrowed{0};
inline std::atomic<u64> g_dma_unverified{0};
inline std::atomic<u64> g_dma_differ{0};
inline std::atomic<u64> g_memo_hits{0};
inline std::atomic<u64> g_memo_misses{0};
inline std::atomic<u64> g_memo_mismatches{0};
inline std::atomic<u64> g_dynamic_skips{0};
inline std::atomic<u64> g_dynamic_updates{0};
inline std::atomic<u64> g_ring_runs{0};
inline std::atomic<u64> g_ring_run_writes{0};
inline std::atomic<u64> g_reset_calls{0};
inline std::atomic<u64> g_reset_records{0};
inline std::atomic<u64> g_reset_ns{0};
inline std::atomic<u64> g_reset_runs{0};
inline std::atomic<u64> g_reset_unbatched{0};
inline std::atomic<u64> g_acquire_skips{0};
inline std::atomic<u64> g_acquire_drains{0};
inline std::atomic<u64> g_acquire_polls{0};
inline std::atomic<u64> g_dma_known_reads{0};
inline std::atomic<u64> g_dma_known_drains{0};
inline std::atomic<u64> g_dma_known_checked{0};
inline std::atomic<u64> g_dma_known_unverified{0};
inline std::atomic<u64> g_dma_known_differ{0};
inline std::atomic<u64> g_writer_waits_shortened{0};

/// For counters one thread writes: a plain load and store, not a locked add.
inline void Count(std::atomic<u64>& counter) {
    counter.store(counter.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
}

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
        const u64 hits = g_memo_hits.exchange(0, std::memory_order_relaxed);
        const u64 memo_misses = g_memo_misses.exchange(0, std::memory_order_relaxed);
        if (hits != 0 || memo_misses != 0) {
            LOG_INFO(HW_GPU,
                     "EXPERIMENT texture-memo: {} draws resolved from the memo, {} in full in 10 s; "
                     "{} verify mismatches in total",
                     hits, memo_misses, g_memo_mismatches.load(std::memory_order_relaxed));
        }
        const u64 dynamic_skips = g_dynamic_skips.exchange(0, std::memory_order_relaxed);
        const u64 dynamic_updates = g_dynamic_updates.exchange(0, std::memory_order_relaxed);
        if (dynamic_skips != 0 || dynamic_updates != 0) {
            LOG_INFO(HW_GPU, "EXPERIMENT dynamic-state-skip: {} draws skipped, {} updated in 10 s",
                     dynamic_skips, dynamic_updates);
        }
        const u64 narrowed = g_dma_narrowed.exchange(0, std::memory_order_relaxed);
        if (narrowed != 0) {
            LOG_INFO(HW_GPU,
                     "EXPERIMENT macro-dma-narrow: {} macro memory reads narrowed in 10 s; in total "
                     "{} not checkable, {} differ from the drained read",
                     narrowed, g_dma_unverified.load(std::memory_order_relaxed),
                     g_dma_differ.load(std::memory_order_relaxed));
        }
        const u64 reset_calls = g_reset_calls.exchange(0, std::memory_order_relaxed);
        const u64 reset_records = g_reset_records.exchange(0, std::memory_order_relaxed);
        const u64 reset_ns = g_reset_ns.exchange(0, std::memory_order_relaxed);
        const u64 reset_runs = g_reset_runs.exchange(0, std::memory_order_relaxed);
        const u64 reset_unbatched = g_reset_unbatched.exchange(0, std::memory_order_relaxed);
        if (reset_calls != 0) {
            LOG_INFO(HW_GPU,
                     "EXPERIMENT reset-batch: {} calls, {} records, {} us in the reset loop in 10 s; "
                     "{} batched runs, {} batched calls written per record (overlap or no device "
                     "address)",
                     reset_calls, reset_records, reset_ns / 1000, reset_runs, reset_unbatched);
        }
        const u64 acquire_skips = g_acquire_skips.exchange(0, std::memory_order_relaxed);
        const u64 acquire_drains = g_acquire_drains.exchange(0, std::memory_order_relaxed);
        const u64 acquire_polls = g_acquire_polls.exchange(0, std::memory_order_relaxed);
        if (acquire_skips != 0 || acquire_drains != 0 || acquire_polls != 0) {
            LOG_INFO(HW_GPU,
                     "EXPERIMENT acquire-met-skip: {} acquires passed without a drain, {} drained "
                     "in 10 s; {} polls of waiting acquires without a drain",
                     acquire_skips, acquire_drains, acquire_polls);
        }
        const u64 known_reads = g_dma_known_reads.exchange(0, std::memory_order_relaxed);
        const u64 known_drains = g_dma_known_drains.exchange(0, std::memory_order_relaxed);
        if (known_reads != 0 || known_drains != 0) {
            LOG_INFO(HW_GPU,
                     "EXPERIMENT dma-read-known: {} reads without a drain, {} drained in 10 s; in "
                     "total {} checked, {} not checkable, {} differ",
                     known_reads, known_drains, g_dma_known_checked.load(std::memory_order_relaxed),
                     g_dma_known_unverified.load(std::memory_order_relaxed),
                     g_dma_known_differ.load(std::memory_order_relaxed));
        }
        if (const u64 shortened = g_writer_waits_shortened.exchange(0, std::memory_order_relaxed);
            shortened != 0) {
            LOG_INFO(HW_GPU, "EXPERIMENT writer-wait-synced: {} waits shortened in 10 s", shortened);
        }
        const u64 runs = g_ring_runs.exchange(0, std::memory_order_relaxed);
        const u64 run_writes = g_ring_run_writes.exchange(0, std::memory_order_relaxed);
        if (runs != 0 || run_writes != 0) {
            LOG_INFO(HW_GPU, "EXPERIMENT macro-ring-run: {} run entries carrying {} writes in 10 s",
                     runs, run_writes);
        }
    }
}

} // namespace VideoCommon::Nx2Experiment
