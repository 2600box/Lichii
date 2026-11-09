// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "video_core/nx2_back_stages.h"

namespace {

namespace DrawStages = VideoCommon::DrawStages;
using DrawStages::BackRing;

TEST_CASE("Back stages: B consumes every packet in order, joins wait for it", "[back_stages]") {
    BackRing ring;
    std::array<u64, BackRing::Capacity> slots{};
    std::vector<u64> seen;
    seen.reserve(20000);
    std::atomic<u64> out_of_order{};
    ring.Start(
        [&](u32 slot) {
            const u64 value = slots[slot];
            if (!seen.empty() && value != seen.back() + 1) {
                out_of_order.fetch_add(1);
            }
            seen.push_back(value);
        },
        {}, {});
    for (u64 packet = 0; packet < 20000; ++packet) {
        slots[ring.Acquire()] = packet;
        ring.Commit();
        if (packet % 977 == 0) {
            ring.Join();
            REQUIRE(ring.Consumed() == packet + 1);
            REQUIRE_FALSE(ring.InFlight());
        }
    }
    ring.Join();
    REQUIRE(ring.Consumed() == 20000);
    ring.Stop();
    REQUIRE(seen.size() == 20000);
    REQUIRE(out_of_order.load() == 0);
    REQUIRE(seen.front() == 0);
}

TEST_CASE("Back stages: a full ring blocks A until B frees a slot", "[back_stages]") {
    BackRing ring;
    std::array<std::atomic<u64>, BackRing::Capacity> slots{};
    std::atomic<u64> overwritten{};
    std::atomic<u64> consumed{};
    ring.Start(
        [&](u32 slot) {
            std::this_thread::sleep_for(std::chrono::microseconds{20});
            if (slots[slot].load() != consumed.load()) {
                overwritten.fetch_add(1);
            }
            consumed.fetch_add(1);
        },
        {}, {});
    for (u64 packet = 0; packet < 3 * BackRing::Capacity; ++packet) {
        slots[ring.Acquire()].store(packet);
        ring.Commit();
    }
    ring.Join();
    REQUIRE(consumed.load() == 3 * BackRing::Capacity);
    REQUIRE(overwritten.load() == 0);
    REQUIRE(ring.stats.full_waits.load() > 0);
    ring.Stop();
}

TEST_CASE("Back stages: marks reach their value once B consumed the packets before them",
          "[back_stages]") {
    BackRing ring;
    std::atomic<u64> consumed{};
    std::atomic<u64> reached{};
    std::atomic<u64> early{};
    std::atomic<u64> backwards{};
    std::array<u64, 64> mark_packets{};
    ring.Start(
        [&](u32) {
            std::this_thread::sleep_for(std::chrono::microseconds{5});
            consumed.fetch_add(1);
        },
        [&](u64 value) {
            if (value < reached.load()) {
                backwards.fetch_add(1);
            }
            if (consumed.load() < mark_packets[value % mark_packets.size()]) {
                early.fetch_add(1);
            }
            reached.store(value);
        },
        {});
    for (u64 mark = 1; mark <= 40; ++mark) {
        for (u32 packet = 0; packet < 7; ++packet) {
            (void)ring.Acquire();
            ring.Commit();
        }
        mark_packets[mark % mark_packets.size()] = ring.Committed();
        ring.Mark(mark);
    }
    ring.Join();
    // The last mark runs on whichever thread sees it reached last.
    for (int spins = 0; spins < 1000 && reached.load() != 40; ++spins) {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    REQUIRE(reached.load() == 40);
    REQUIRE(early.load() == 0);
    REQUIRE(backwards.load() == 0);
    // Nothing in flight: a mark is reached at once.
    ring.Mark(41);
    REQUIRE(reached.load() == 41);
    ring.Stop();
}

TEST_CASE("Back stages: Stop consumes what A committed", "[back_stages]") {
    std::atomic<u64> consumed{};
    {
        BackRing ring;
        ring.Start([&](u32) { consumed.fetch_add(1); }, {}, {});
        for (u32 packet = 0; packet < 10; ++packet) {
            (void)ring.Acquire();
            ring.Commit();
        }
        ring.Stop();
        REQUIRE(ring.Consumed() == 10);
    }
    REQUIRE(consumed.load() == 10);
}

TEST_CASE("Back stages: joins from the thread that draws count their site", "[back_stages]") {
    BackRing ring;
    std::atomic<bool> release{};
    ring.Start(
        [&](u32) {
            while (!release.load()) {
                std::this_thread::yield();
            }
        },
        {}, {});
    DrawStages::g_counting = true;
    DrawStages::g_threaded = true;
    DrawStages::g_ring = &ring;
    DrawStages::g_counters.joins[static_cast<size_t>(DrawStages::Join::Present)] = 0;
    DrawStages::t_draw_thread = true;
    DrawStages::JoinB(DrawStages::Join::Present);
    REQUIRE(DrawStages::g_counters.joins[static_cast<size_t>(DrawStages::Join::Present)] == 0);
    (void)ring.Acquire();
    ring.Commit();
    REQUIRE(DrawStages::PacketsInFlight());
    std::thread releaser([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
        release.store(true);
    });
    DrawStages::JoinB(DrawStages::Join::Present);
    releaser.join();
    REQUIRE_FALSE(DrawStages::PacketsInFlight());
    REQUIRE(DrawStages::g_counters.joins[static_cast<size_t>(DrawStages::Join::Present)] == 1);
    DrawStages::g_ring = nullptr;
    DrawStages::g_threaded = false;
    DrawStages::g_counting = false;
    DrawStages::t_draw_thread = false;
    ring.Stop();
}

} // namespace
