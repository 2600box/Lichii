// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "video_core/buffer_cache/buffer_cache_base.h"
#include "video_core/nx2_back_stages.h"
#include "video_core/renderer_vulkan/vk_draw_packet.h"

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

TEST_CASE("Back stages: A and B on two threads through the ring, joins, marks and the cache lock",
          "[back_stages][threaded]") {
    // Stands in for the rasterizer: A fills packet slots and captures commands into each slot's
    // list, B records a slot's list and then its draw into the command buffer. A joins at writes
    // over what a packet in flight reads, at the buffer cache's lock and at direct recordings, and
    // splices its open list after each join, as the rasterizer's after-join hook does.
    struct Slot {
        Vulkan::DynamicStatePacket dynamic;
        std::vector<u64> list;
        u64 packet{};
        u64 read_begin{};
    };
    static constexpr u64 Packets = 6000;
    static constexpr u64 Page = 0x10000;
    BackRing ring;
    std::vector<Slot> slots(BackRing::Capacity);
    std::vector<u64> command_buffer;
    command_buffer.reserve(Packets * 8);
    Vulkan::DynamicStatePacket b_dynamic;
    VideoCommon::BufferCacheLock cache_lock;
    u64 cache_state{};
    std::atomic<u64> out_of_order{};
    std::atomic<u64> wrong_dynamic{};
    std::atomic<u64> mark_reached{};
    std::atomic<u64> marks_early{};
    std::atomic<u64> b_consumed{};
    std::array<u64, 1024> mark_packets{};
    u64 b_next{};

    struct AfterJoin {
        std::vector<Slot>* slots;
        std::vector<u64>* command_buffer;
        u32 slot;
    } after_join{&slots, &command_buffer, 0};

    ring.Start(
        [&](u32 index) {
            std::scoped_lock lock{cache_lock};
            Slot& slot = slots[index];
            if (slot.packet != b_next) {
                out_of_order.fetch_add(1);
            }
            b_next = slot.packet + 1;
            command_buffer.insert(command_buffer.end(), slot.list.begin(), slot.list.end());
            slot.list.clear();
            Vulkan::MergeDynamicState(b_dynamic, slot.dynamic);
            if (b_dynamic.line_width != static_cast<f32>(slot.packet / 3 * 3)) {
                wrong_dynamic.fetch_add(1);
            }
            ++cache_state;
            command_buffer.push_back(slot.packet * 16 + 8);
            b_consumed.fetch_add(1, std::memory_order_release);
        },
        [&](u64 value) {
            if (b_consumed.load(std::memory_order_acquire) <
                mark_packets[value % mark_packets.size()]) {
                marks_early.fetch_add(1);
            }
            mark_reached.store(value);
        },
        {});
    DrawStages::g_threaded = true;
    DrawStages::g_ring = &ring;
    DrawStages::t_draw_thread = true;
    DrawStages::g_after_join_argument = &after_join;
    DrawStages::g_after_join = [](void* argument) {
        auto& state = *static_cast<AfterJoin*>(argument);
        auto& list = (*state.slots)[state.slot].list;
        state.command_buffer->insert(state.command_buffer->end(), list.begin(), list.end());
        list.clear();
    };

    // Another thread takes the lock as CPU invalidations do: it never joins.
    std::atomic<bool> stop_cpu{};
    std::thread cpu([&] {
        while (!stop_cpu.load()) {
            std::scoped_lock lock{cache_lock};
            ++cache_state;
        }
    });

    std::vector<u64> expected;
    expected.reserve(Packets * 8);
    u32 index = ring.Acquire();
    after_join.slot = index;
    u64 marks = 0;
    u64 overlap_joins = 0;
    for (u64 packet = 0; packet < Packets; ++packet) {
        Slot& slot = slots[index];
        slot.packet = packet;
        slot.list.push_back(packet * 16 + 1);
        expected.push_back(packet * 16 + 1);
        slot.dynamic.mask = 0;
        if (packet % 3 == 0) {
            slot.dynamic.Set(Vulkan::DynamicStatePacket::LineWidth);
            slot.dynamic.line_width = static_cast<f32>(packet);
        }
        slot.read_begin = Page * (1 + packet % 32);
        DrawStages::NoteReadAt(DrawStages::VertexSlot(0), slot.read_begin, 0x100,
                               ring.Committed());
        if (packet % 7 == 0) {
            // A write over a page a packet in flight reads waits until B recorded it.
            const u64 page = Page * (1 + (packet + 31) % 32);
            const bool in_flight = DrawStages::PacketsInFlight();
            DrawStages::NoteWrite(DrawStages::Join::Inline, page, 4);
            overlap_joins += in_flight && !DrawStages::PacketsInFlight() ? 1 : 0;
        }
        if (packet % 11 == 0) {
            // The buffer cache's lock joins first: B is idle while A holds it.
            std::scoped_lock lock{cache_lock};
            REQUIRE_FALSE(DrawStages::PacketsInFlight());
            ++cache_state;
        }
        if (packet % 13 == 0) {
            // A records directly once B is idle (a submit, a render pass request): its open
            // list was spliced by the join, so the direct command follows it.
            DrawStages::JoinB(DrawStages::Join::ASchedulerUse);
            REQUIRE(slot.list.empty());
            command_buffer.push_back(packet * 16 + 2);
            expected.push_back(packet * 16 + 2);
        }
        expected.push_back(packet * 16 + 8);
        ring.Commit();
        if (packet % 17 == 0) {
            ++marks;
            mark_packets[marks % mark_packets.size()] = ring.Committed();
            ring.Mark(marks);
        }
        index = ring.Acquire();
        after_join.slot = index;
    }
    DrawStages::JoinB(DrawStages::Join::Frame);
    stop_cpu.store(true);
    cpu.join();
    ring.Stop();
    DrawStages::g_after_join = nullptr;
    DrawStages::g_ring = nullptr;
    DrawStages::g_threaded = false;
    DrawStages::t_draw_thread = false;

    REQUIRE(out_of_order.load() == 0);
    REQUIRE(wrong_dynamic.load() == 0);
    REQUIRE(marks_early.load() == 0);
    REQUIRE(mark_reached.load() == marks);
    REQUIRE(overlap_joins > 0);
    REQUIRE(command_buffer == expected);
}

} // namespace
