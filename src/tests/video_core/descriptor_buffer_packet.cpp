// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "video_core/renderer_vulkan/descriptor_buffer_packet.h"
#include "video_core/renderer_vulkan/descriptor_packet_budget.h"
#include "video_core/renderer_vulkan/vk_command_chunk.h"

namespace {

using Vulkan::CommandChunk;
using Vulkan::DescriptorAddress;
using Vulkan::DescriptorBufferLayout;
using Vulkan::DescriptorBufferPacket;
using Vulkan::DescriptorPacketBudget;
using Vulkan::DescriptorUpdateEntry;
using Comparison = DescriptorBufferPacket::Comparison;

DescriptorBufferLayout MakeLayout(u32 count = 2) {
    DescriptorBufferLayout layout;
    layout.size = 16 + count * 16;
    layout.bindings.push_back({VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, count, 8, 16});
    return layout;
}

void Encode(const DescriptorBufferLayout& layout, const DescriptorUpdateEntry* entries, u8* out) {
    for (const auto& binding : layout.bindings) {
        for (u32 i = 0; i < binding.count; ++i) {
            const auto& address = (entries++)->address;
            for (VkDeviceSize byte = 0; byte < binding.stride; ++byte) {
                out[binding.offset + i * binding.stride + byte] =
                    static_cast<u8>((address.address + address.range + byte) ^ address.format);
            }
        }
    }
}

class Worker {
public:
    Worker() : thread{[this] { Run(); }} {}

    ~Worker() {
        Drain();
        {
            std::scoped_lock lock{mutex};
            stopping = true;
        }
        ready.notify_one();
        thread.join();
    }

    void Submit(std::unique_ptr<CommandChunk> chunk) {
        {
            std::scoped_lock lock{mutex};
            queue.push_back(std::move(chunk));
        }
        ready.notify_one();
    }

    void Drain() {
        std::unique_lock lock{mutex};
        completed.wait(lock, [this] { return queue.empty() && !executing; });
    }

private:
    void Run() {
        for (;;) {
            std::unique_lock lock{mutex};
            ready.wait(lock, [this] { return stopping || !queue.empty(); });
            if (stopping) {
                return;
            }
            auto chunk = std::move(queue.front());
            queue.pop_front();
            executing = true;
            lock.unlock();
            chunk->ExecuteAll({}, {});
            chunk.reset();
            lock.lock();
            executing = false;
            completed.notify_all();
        }
    }

    std::mutex mutex;
    std::condition_variable ready;
    std::condition_variable completed;
    std::deque<std::unique_ptr<CommandChunk>> queue;
    bool stopping{};
    bool executing{};
    std::thread thread;
};

class PacketScheduler {
public:
    ~PacketScheduler() {
        WaitWorker();
    }

    bool ReserveDescriptorBytes(size_t bytes) {
        ++reservations;
        if (!budget.Reserve(bytes, [this] { WaitWorker(); })) {
            return false;
        }
        pending_bytes += bytes;
        peak_bytes = std::max(peak_bytes, pending_bytes);
        return true;
    }

    template <typename T>
    void Record(T&& command) {
        auto wrapped = [command = std::move(command)](auto cmdbuf, auto) { command(cmdbuf); };
        if (!chunk->Record(wrapped)) {
            Dispatch();
            if (!chunk->Record(wrapped)) {
                throw std::runtime_error{"command exceeds chunk capacity"};
            }
        }
        ++recorded;
    }

    void Dispatch() {
        if (!chunk->Empty()) {
            worker.Submit(std::move(chunk));
            chunk = std::make_unique<CommandChunk>();
        }
    }

    void WaitWorker() {
        Dispatch();
        worker.Drain();
        budget.Reset();
        pending_bytes = 0;
        ++drains;
    }

    size_t reservations{};
    size_t recorded{};
    size_t drains{};
    size_t peak_bytes{};

private:
    Worker worker;
    DescriptorPacketBudget budget;
    std::unique_ptr<CommandChunk> chunk = std::make_unique<CommandChunk>();
    size_t pending_bytes{};
};

struct DestructionCount {
    explicit DestructionCount(size_t& count_) : count{count_} {}
    ~DestructionCount() {
        ++count;
    }
    size_t& count;
};

} // namespace

TEST_CASE("Descriptor packets own inputs across a delayed worker and ordered reuse",
          "[video_core][descriptor_packet]") {
    Worker worker;
    auto layout = MakeLayout();
    std::array entries{DescriptorUpdateEntry{DescriptorAddress{17, 32, VK_FORMAT_R32_UINT}},
                       DescriptorUpdateEntry{DescriptorAddress{91, 64, VK_FORMAT_R32_SFLOAT}}};
    std::vector<u8> ring(layout.size, 0xa5);
    auto expected = ring;
    Encode(layout, entries.data(), expected.data());
    DescriptorBufferPacket packet{layout, entries, ring.data(), false, Encode};
    std::promise<void> entered;
    std::promise<void> release;
    auto gate = release.get_future();
    auto chunk = std::make_unique<CommandChunk>();
    auto block = [&](Vulkan::vk::CommandBuffer, Vulkan::vk::CommandBuffer) {
        entered.set_value();
        gate.wait();
    };
    REQUIRE(chunk->Record(block));
    Comparison result{};
    auto encode = [packet = std::move(packet), &result](auto, auto) {
        result = packet.Execute(Encode);
    };
    REQUIRE(chunk->Record(encode));
    std::vector<u8> observed;
    auto consume = [&](auto, auto) { observed = ring; };
    REQUIRE(chunk->Record(consume));
    worker.Submit(std::move(chunk));
    entered.get_future().wait();
    entries[0].address.address = 999;
    entries[1].address.range = 0;
    layout.bindings.clear();
    layout.size = 0;
    release.set_value();
    worker.Drain();
    REQUIRE(result == Comparison::NotRequested);
    REQUIRE(observed == expected);
    REQUIRE(ring == expected);

    layout = MakeLayout();
    std::fill(ring.begin(), ring.end(), 0x5a);
    expected = ring;
    Encode(layout, entries.data(), expected.data());
    DescriptorBufferPacket next{layout, entries, ring.data(), false, Encode};
    chunk = std::make_unique<CommandChunk>();
    auto reuse = [next = std::move(next)](auto, auto) { next.Execute(Encode); };
    REQUIRE(chunk->Record(reuse));
    REQUIRE(chunk->Record(consume));
    worker.Submit(std::move(chunk));
    worker.Drain();
    REQUIRE(observed == expected);
}

TEST_CASE("Descriptor verification ignores layout holes and preserves destination holes",
          "[video_core][descriptor_packet]") {
    auto layout = MakeLayout(1);
    layout.bindings.push_back({VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, 40, 16});
    layout.size = 64;
    std::array entries{DescriptorUpdateEntry{DescriptorAddress{1, 32, VK_FORMAT_UNDEFINED}},
                       DescriptorUpdateEntry{DescriptorAddress{2, 64, VK_FORMAT_UNDEFINED}}};
    std::vector<u8> destination(layout.size, 0xcd);
    auto expected = destination;
    Encode(layout, entries.data(), expected.data());
    unsigned calls{};
    const auto encode = [&](const auto& packet_layout, const auto* payload, u8* out) {
        std::fill_n(out, packet_layout.size, static_cast<u8>(++calls));
        Encode(packet_layout, payload, out);
    };
    DescriptorBufferPacket packet{layout, entries, destination.data(), true, encode};
    REQUIRE(calls == 1);
    REQUIRE(packet.Execute(encode) == Comparison::Equal);
    REQUIRE(calls == 2);
    REQUIRE(destination == expected);
}

TEST_CASE("Descriptor packets retain heap-backed layouts after source destruction",
          "[video_core][descriptor_packet]") {
    std::vector<u8> destination(40 * 32, 0x9a);
    auto expected = destination;
    std::unique_ptr<DescriptorBufferPacket> packet;
    {
        DescriptorBufferLayout layout;
        layout.size = destination.size();
        std::vector<DescriptorUpdateEntry> entries;
        for (u32 i = 0; i < 40; ++i) {
            layout.bindings.push_back({VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, i * 32, 16});
            entries.emplace_back(DescriptorAddress{i, 32, VK_FORMAT_UNDEFINED});
        }
        Encode(layout, entries.data(), expected.data());
        packet = std::make_unique<DescriptorBufferPacket>(layout, entries, destination.data(),
                                                          false, Encode);
    }
    REQUIRE(packet->Execute(Encode) == Comparison::NotRequested);
    REQUIRE(destination == expected);
}

TEST_CASE("Different opaque descriptor bytes select the producer encoding without proving a bug",
          "[video_core][descriptor_packet]") {
    const auto layout = MakeLayout(1);
    const std::array entries{DescriptorUpdateEntry{DescriptorAddress{1, 32, VK_FORMAT_UNDEFINED}}};
    std::vector<u8> destination(layout.size, 0xcd);
    auto expected = destination;
    Encode(layout, entries.data(), expected.data());
    unsigned calls{};
    const auto encode = [&](const auto& packet_layout, const auto* payload, u8* out) {
        Encode(packet_layout, payload, out);
        out[packet_layout.bindings.front().offset] ^= static_cast<u8>(calls++);
    };
    DescriptorBufferPacket packet{layout, entries, destination.data(), true, encode};
    REQUIRE(packet.Execute(encode) == Comparison::DifferentOpaqueBytes);
    REQUIRE(destination == expected);
}

TEST_CASE("Descriptor budget drains only at its bound and rejects oversized work",
          "[video_core][descriptor_packet]") {
    DescriptorPacketBudget budget;
    unsigned drains{};
    const auto drain = [&] { ++drains; };
    REQUIRE_FALSE(budget.Pending());
    REQUIRE_FALSE(budget.Reserve(DescriptorPacketBudget::Limit + 1, drain));
    REQUIRE_FALSE(budget.Pending());
    REQUIRE(budget.Reserve(DescriptorPacketBudget::Limit, drain));
    REQUIRE(budget.Pending());
    REQUIRE(drains == 0);
    REQUIRE_FALSE(budget.Reserve(DescriptorPacketBudget::Limit + 1, drain));
    REQUIRE(drains == 0);
    REQUIRE(budget.Reserve(1, drain));
    REQUIRE(drains == 1);
    REQUIRE(budget.Reserve(DescriptorPacketBudget::Limit - 1, drain));
    REQUIRE(drains == 1);
    REQUIRE(budget.Reserve(1, drain));
    REQUIRE(drains == 2);
    budget.Reset();
    REQUIRE_FALSE(budget.Pending());
}

TEST_CASE("Descriptor budget bounds queued packets and oversized work encodes directly",
          "[video_core][descriptor_packet]") {
    PacketScheduler scheduler;
    const auto layout = MakeLayout(256);
    std::vector<DescriptorUpdateEntry> entries(256, DescriptorAddress{17, 32, VK_FORMAT_R32_UINT});
    std::vector<std::vector<u8>> outputs(256, std::vector<u8>(layout.size, 0xcc));
    size_t completed{};
    const auto encode = [&](const auto& packet_layout, const auto* payload, u8* out) {
        Encode(packet_layout, payload, out);
        ++completed;
    };
    for (auto& output : outputs) {
        Vulkan::RecordDescriptorBufferPacket(scheduler, layout, entries, output.data(), true, false,
                                             encode, [] {});
    }
    REQUIRE(scheduler.drains > 0);
    scheduler.WaitWorker();
    REQUIRE(scheduler.peak_bytes <= DescriptorPacketBudget::Limit);
    REQUIRE(completed == outputs.size());
    REQUIRE(scheduler.recorded == outputs.size());
    std::vector<u8> direct(layout.size, 0xcc);
    Vulkan::RecordDescriptorBufferPacket(scheduler, layout, entries, direct.data(), false, false,
                                         encode, [] {});
    REQUIRE(completed == outputs.size() + 1);
    REQUIRE(scheduler.recorded == outputs.size());
    REQUIRE(scheduler.reservations == outputs.size());
    for (const auto& output : outputs) {
        REQUIRE(output == direct);
    }
    auto oversized = layout;
    oversized.size = DescriptorPacketBudget::Limit;
    std::vector<u8> fallback(oversized.size, 0xcc);
    Vulkan::RecordDescriptorBufferPacket(scheduler, oversized, entries, fallback.data(), true, true,
                                         encode, [] {});
    REQUIRE(completed == outputs.size() + 2);
    REQUIRE(scheduler.recorded == outputs.size());
    REQUIRE(scheduler.reservations == outputs.size() + 1);
    REQUIRE(std::equal(direct.begin(), direct.end(), fallback.begin()));
}

TEST_CASE("The production packet dispatcher preserves worker order and reports opaque differences",
          "[video_core][descriptor_packet]") {
    PacketScheduler scheduler;
    const auto layout = MakeLayout(1);
    std::array entries{DescriptorUpdateEntry{DescriptorAddress{9, 16, VK_FORMAT_UNDEFINED}}};
    std::vector<u8> destination(layout.size, 0xcc);
    auto expected = destination;
    Encode(layout, entries.data(), expected.data());
    std::promise<void> entered;
    std::promise<void> release;
    auto gate = release.get_future();
    scheduler.Record([&](auto) {
        entered.set_value();
        gate.wait();
    });
    scheduler.Dispatch();
    entered.get_future().wait();
    unsigned calls{};
    unsigned differences{};
    const auto encode = [&](const auto& packet_layout, const auto* payload, u8* out) {
        Encode(packet_layout, payload, out);
        out[8] ^= static_cast<u8>(calls++);
    };
    Vulkan::RecordDescriptorBufferPacket(scheduler, layout, entries, destination.data(), true, true,
                                         encode, [&] { ++differences; });
    entries[0].address.address = 999;
    std::vector<u8> observed;
    scheduler.Record([&](auto) { observed = destination; });
    release.set_value();
    scheduler.WaitWorker();
    REQUIRE(calls == 2);
    REQUIRE(differences == 1);
    REQUIRE(observed == expected);
}

TEST_CASE("Command chunks destroy cancelled and executed owning captures exactly once",
          "[video_core][descriptor_packet]") {
    size_t destroyed{};
    size_t executed{};
    {
        CommandChunk chunk;
        const auto layout = MakeLayout(1);
        const std::array entries{
            DescriptorUpdateEntry{DescriptorAddress{1, 32, VK_FORMAT_UNDEFINED}}};
        std::vector<u8> destination(layout.size);
        DescriptorBufferPacket packet{layout, entries, destination.data(), true, Encode};
        auto command = [packet = std::move(packet),
                        owner = std::make_unique<DestructionCount>(destroyed),
                        &executed](auto, auto) {
            packet.Execute(Encode);
            ++executed;
        };
        REQUIRE(chunk.Record(command));
    }
    REQUIRE(destroyed == 1);
    REQUIRE(executed == 0);
    {
        CommandChunk chunk;
        auto command = [owner = std::make_unique<DestructionCount>(destroyed),
                        &executed](auto, auto) { ++executed; };
        REQUIRE(chunk.Record(command));
        chunk.MarkSubmit();
        chunk.ExecuteAll({}, {});
        REQUIRE(chunk.Empty());
        REQUIRE_FALSE(chunk.HasSubmit());
        REQUIRE(destroyed == 2);
        chunk.ExecuteAll({}, {});
    }
    REQUIRE(destroyed == 2);
    REQUIRE(executed == 1);
}

TEST_CASE("Command chunk destruction releases the throwing and remaining commands",
          "[video_core][descriptor_packet]") {
    size_t destroyed{};
    size_t executed{};
    {
        CommandChunk chunk;
        auto first = [owner = std::make_unique<DestructionCount>(destroyed)](auto, auto) {};
        auto fail = [owner = std::make_unique<DestructionCount>(destroyed)](auto, auto) {
            throw std::runtime_error{"encoding failed"};
        };
        auto last = [owner = std::make_unique<DestructionCount>(destroyed), &executed](auto, auto) {
            ++executed;
        };
        REQUIRE(chunk.Record(first));
        REQUIRE(chunk.Record(fail));
        REQUIRE(chunk.Record(last));
        REQUIRE_THROWS_AS(chunk.ExecuteAll({}, {}), std::runtime_error);
        REQUIRE(destroyed == 1);
    }
    REQUIRE(destroyed == 3);
    REQUIRE(executed == 0);
}

TEST_CASE("A full command chunk does not consume the rejected owning command",
          "[video_core][descriptor_packet]") {
    CommandChunk full;
    auto filler = [payload = std::array<u8, 1024>{}](auto, auto) { (void)payload; };
    size_t recorded{};
    while (full.Record(filler)) {
        ++recorded;
    }
    REQUIRE(recorded > 0);
    size_t destroyed{};
    auto owner = std::make_shared<DestructionCount>(destroyed);
    auto command = [owner, payload = std::array<u8, 2048>{}](auto, auto) { (void)payload; };
    owner.reset();
    REQUIRE_FALSE(full.Record(command));
    REQUIRE(destroyed == 0);
    {
        CommandChunk next;
        REQUIRE(next.Record(command));
        next.ExecuteAll({}, {});
    }
    REQUIRE(destroyed == 1);
}

TEST_CASE("Descriptor packet end-to-end synthetic workload timing",
          "[.][video_core][descriptor_packet_benchmark]") {
    constexpr size_t draws = 4096;
    constexpr size_t repetitions = 7;
    for (const u32 count : {8U, 64U, 256U}) {
        const auto layout = MakeLayout(count);
        std::vector<DescriptorUpdateEntry> entries(count);
        std::vector<u8> destinations(draws * layout.size, 0xcc);
        std::array<double, repetitions> direct_times{};
        std::array<double, repetitions> packet_times{};
        u64 direct_checksum{};
        u64 packet_checksum{};
        for (size_t repetition = 0; repetition < repetitions; ++repetition) {
            for (const bool packet_mode : {repetition % 2 != 0, repetition % 2 == 0}) {
                PacketScheduler scheduler;
                u64 checksum{};
                const auto begin = std::chrono::steady_clock::now();
                for (size_t draw = 0; draw < draws; ++draw) {
                    for (u32 i = 0; i < count; ++i) {
                        entries[i] = DescriptorAddress{draw + i, 32 + i, VK_FORMAT_R32_UINT};
                    }
                    u8* const destination = destinations.data() + draw * layout.size;
                    Vulkan::RecordDescriptorBufferPacket(scheduler, layout, entries, destination,
                                                         packet_mode, false, Encode, [] {});
                    scheduler.Record(
                        [destination, &checksum](auto) { checksum += destination[8]; });
                }
                scheduler.WaitWorker();
                const double milliseconds = std::chrono::duration<double, std::milli>(
                                                std::chrono::steady_clock::now() - begin)
                                                .count();
                (packet_mode ? packet_times : direct_times)[repetition] = milliseconds;
                (packet_mode ? packet_checksum : direct_checksum) = checksum;
            }
            REQUIRE(direct_checksum == packet_checksum);
        }
        std::ranges::sort(direct_times);
        std::ranges::sort(packet_times);
        std::printf("synthetic descriptors=%u draws=%zu direct_median_ms=%.3f "
                    "packet_median_ms=%.3f ratio=%.3f checksum=%llu\n",
                    count, draws, direct_times[repetitions / 2], packet_times[repetitions / 2],
                    packet_times[repetitions / 2] / direct_times[repetitions / 2],
                    static_cast<unsigned long long>(packet_checksum));
    }
}
