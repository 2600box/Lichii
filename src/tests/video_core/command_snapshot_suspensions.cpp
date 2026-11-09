// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "core/core.h"
#include "tests/core/system_test_context.h"
#include "video_core/control/channel_state.h"
#include "video_core/dma_pusher.h"
#include "video_core/memory_manager.h"

TEST_CASE("Entries parsed without a pending acquire keep their submit-time snapshot",
          "[video_core][dma_pusher]") {
    ScopedSystemTestContext context;
    Core::System system;
    Tegra::MemoryManager memory_manager{system};
    Tegra::Control::ChannelState channel_state{0};
    Tegra::DmaPusher dma_pusher{system, memory_manager, channel_state};

    const u64 snapshot_suspensions = dma_pusher.suspensions.load();
    for (int entry = 0; entry < 8; ++entry) {
        REQUIRE_FALSE(dma_pusher.puller.HasPendingAcquire());
        REQUIRE(dma_pusher.puller.ResumeAcquire(dma_pusher));
    }
    REQUIRE(dma_pusher.suspensions.load() == snapshot_suspensions);
}

namespace {

constexpr GPUVAddr ListAddress = 0x100000;

Tegra::CommandList SubmittedList(Tegra::DmaPusher& dma_pusher, u64 sequence) {
    Tegra::CommandList list{1};
    list.command_lists[0].raw = 0;
    list.command_lists[0].addr.Assign(ListAddress);
    list.command_lists[0].size.Assign(2);
    list.submitted_words = {Tegra::CommandHeader{0x11}, Tegra::CommandHeader{0x22}};
    list.submitted_offsets = {0};
    list.submit_sequence = sequence;
    dma_pusher.in_transit.push_back(Tegra::DmaPusher::InTransitList{
        .submit_sequence = sequence,
        .entries = {list.command_lists[0]},
        .offsets = list.submitted_offsets,
        .words = 2,
    });
    return list;
}

} // namespace

TEST_CASE("Lists on their way keep their copies after a release the GPU wrote",
          "[video_core][dma_pusher]") {
    ScopedSystemTestContext context;
    Core::System system;
    Tegra::MemoryManager memory_manager{system};
    Tegra::Control::ChannelState channel_state{0};
    Tegra::DmaPusher dma_pusher{system, memory_manager, channel_state};

    Tegra::CommandList list = SubmittedList(dma_pusher, 1);
    dma_pusher.suspensions.store(2);
    dma_pusher.CopyInTransitAgain(2, false);
    dma_pusher.Push(std::move(list));

    REQUIRE(dma_pusher.in_transit.empty());
    const Tegra::CommandList& queued = dma_pusher.dma_pushbuffer.back();
    REQUIRE(queued.snapshot_suspensions == 2);
    REQUIRE(queued.submitted_words[0].argument == 0x11);
    REQUIRE(queued.submitted_words[1].argument == 0x22);
}

TEST_CASE("Lists on their way are copied again after a release the game wrote",
          "[video_core][dma_pusher]") {
    ScopedSystemTestContext context;
    Core::System system;
    Tegra::MemoryManager memory_manager{system};
    Tegra::Control::ChannelState channel_state{0};
    Tegra::DmaPusher dma_pusher{system, memory_manager, channel_state};

    // Unmapped memory reads as zeros, so the copy taken at the resume differs from the submit's.
    Tegra::CommandList list = SubmittedList(dma_pusher, 1);
    dma_pusher.suspensions.store(2);
    dma_pusher.CopyInTransitAgain(2, true);
    dma_pusher.Push(std::move(list));

    const Tegra::CommandList& queued = dma_pusher.dma_pushbuffer.back();
    REQUIRE(queued.snapshot_suspensions == 2);
    REQUIRE(queued.submitted_words[0].argument == 0);
    REQUIRE(queued.submitted_words[1].argument == 0);
}

TEST_CASE("Push takes the copy of its own list and drops lists that never came",
          "[video_core][dma_pusher]") {
    ScopedSystemTestContext context;
    Core::System system;
    Tegra::MemoryManager memory_manager{system};
    Tegra::Control::ChannelState channel_state{0};
    Tegra::DmaPusher dma_pusher{system, memory_manager, channel_state};

    SubmittedList(dma_pusher, 1);
    Tegra::CommandList list = SubmittedList(dma_pusher, 2);
    SubmittedList(dma_pusher, 3);
    dma_pusher.Push(std::move(list));

    REQUIRE(dma_pusher.in_transit.size() == 1);
    REQUIRE(dma_pusher.in_transit.front().submit_sequence == 3);
    const Tegra::CommandList& queued = dma_pusher.dma_pushbuffer.back();
    REQUIRE(queued.snapshot_suspensions == 0);
    REQUIRE(queued.submitted_words[0].argument == 0x11);
}
