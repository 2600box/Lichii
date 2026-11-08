// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <thread>

#include <catch2/catch_test_macros.hpp>

#include "video_core/nx2_profile.h"
#include "video_core/nx2_stutter_trace.h"

namespace {

using namespace VideoCommon;
using StutterTrace::Category;
using StutterTrace::Thread;

constexpr size_t Index(Category category) {
    return static_cast<size_t>(category);
}

constexpr size_t Index(Thread thread) {
    return static_cast<size_t>(thread);
}

} // Anonymous namespace

TEST_CASE("StutterTrace: a pipeline build on the back thread is the cause", "[video_core]") {
    StutterTrace::ThreadTicks ticks{};
    // 100 ms frame, 1 tick = 1 ms. The front waits 70 ms for the back, which builds a pipeline.
    ticks[Index(Thread::Front)][Index(Category::Idle)] = 70;
    ticks[Index(Thread::Back)][Index(Category::GraphicsMiss)] = 80;
    ticks[Index(Thread::Back)][Index(Category::TextureCache)] = 5;
    ticks[Index(Thread::Worker)][Index(Category::Idle)] = 90;
    const auto frame = StutterTrace::Attribute(ticks, {true, true, true}, 100.0, 1.0);
    REQUIRE(frame.cause == "gfx_miss");
    REQUIRE(frame.category_ms[Index(Category::GraphicsMiss)] == 80.0);
    REQUIRE(frame.busy_ms[Index(Thread::Front)] == 30.0);
    REQUIRE(frame.busy_ms[Index(Thread::Back)] == 100.0);
    // The front's 30 ms is the busy time most left unexplained (the back's is 15 ms).
    REQUIRE(frame.other_ms == 30.0);
}

TEST_CASE("StutterTrace: unexplained busy time is other", "[video_core]") {
    StutterTrace::ThreadTicks ticks{};
    // A 50 ms frame the front spends parsing commands, which no category covers.
    ticks[Index(Thread::Front)][Index(Category::BufferCache)] = 4;
    ticks[Index(Thread::Worker)][Index(Category::PipelineWait)] = 6;
    ticks[Index(Thread::Worker)][Index(Category::Idle)] = 40;
    const auto frame = StutterTrace::Attribute(ticks, {true, false, true}, 50.0, 1.0);
    REQUIRE(frame.cause == "other");
    REQUIRE(frame.other_ms == 46.0);
    // A thread that isn't running (no split) has no busy time.
    REQUIRE_FALSE(frame.busy_ms[Index(Thread::Back)].has_value());
}

TEST_CASE("StutterTrace: ticks convert with the frame's ratio", "[video_core]") {
    StutterTrace::ThreadTicks ticks{};
    // 1024 ticks a millisecond (a power of two keeps the products exact).
    ticks[Index(Thread::Worker)][Index(Category::PipelineWait)] = 30 * 1024;
    ticks[Index(Thread::Worker)][Index(Category::Idle)] = 10 * 1024;
    const auto frame = StutterTrace::Attribute(ticks, {false, false, true}, 40.0, 1.0 / 1024);
    REQUIRE(frame.cause == "pipeline_wait");
    REQUIRE(frame.category_ms[Index(Category::PipelineWait)] == 30.0);
    REQUIRE(frame.busy_ms[Index(Thread::Worker)] == 30.0);
}

TEST_CASE("StutterTrace: zones add their exclusive time on named threads", "[video_core]") {
    const bool was_enabled = StutterTrace::g_enabled;
    const bool zones_were_enabled = Nx2Profile::g_zones_enabled;
    StutterTrace::g_enabled = true;
    Nx2Profile::g_zones_enabled = true;
    auto& totals = StutterTrace::g_totals;
    const auto back = Index(Thread::Back);
    const u64 miss_before = totals.ticks[back][Index(Category::GraphicsMiss)].load();
    const u64 capture_before = totals.ticks[back][Index(Category::ShaderCapture)].load();
    const u64 misses_before = totals.counts[back][Index(Category::GraphicsMiss)].load();
    bool running = false;
    std::thread{[&running] {
        const StutterTrace::ThreadScope named{Thread::Back};
        running = StutterTrace::g_totals.running[Index(Thread::Back)].load();
        const Nx2Profile::Scope miss{Nx2Profile::Zone::GraphicsPipelineMiss};
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
        const Nx2Profile::Scope capture{Nx2Profile::Zone::ShaderCapture};
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }}.join();
    REQUIRE(running);
    REQUIRE_FALSE(totals.running[back].load());
    const u64 miss = totals.ticks[back][Index(Category::GraphicsMiss)].load() - miss_before;
    const u64 capture = totals.ticks[back][Index(Category::ShaderCapture)].load() - capture_before;
    REQUIRE(totals.counts[back][Index(Category::GraphicsMiss)].load() - misses_before == 1);
    REQUIRE(capture > 0);
    REQUIRE(miss > 0);

    // Zones on threads without a ThreadScope count nowhere.
    StutterTrace::ThreadTicks before{};
    for (size_t thread = 0; thread < StutterTrace::ThreadCount; ++thread) {
        before[thread][Index(Category::TextureCache)] =
            totals.ticks[thread][Index(Category::TextureCache)].load();
    }
    std::thread{[] {
        const Nx2Profile::Scope texture{Nx2Profile::Zone::TextureCache};
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }}.join();
    for (size_t thread = 0; thread < StutterTrace::ThreadCount; ++thread) {
        REQUIRE(totals.ticks[thread][Index(Category::TextureCache)].load() ==
                before[thread][Index(Category::TextureCache)]);
    }
    StutterTrace::g_enabled = was_enabled;
    Nx2Profile::g_zones_enabled = zones_were_enabled;
}

TEST_CASE("StutterTrace: every zone has a category or counts as other", "[video_core]") {
    using Nx2Profile::Zone;
    REQUIRE(Nx2Profile::stutter_categories[static_cast<size_t>(Zone::Draw)] == Category::None);
    REQUIRE(Nx2Profile::stutter_categories[static_cast<size_t>(Zone::SplitDrain)] == Category::Idle);
    REQUIRE(Nx2Profile::stutter_categories[static_cast<size_t>(Zone::PipelineWait)] ==
            Category::PipelineWait);
    REQUIRE(Nx2Profile::stutter_categories[static_cast<size_t>(Zone::TextureUpload)] ==
            Category::TextureCache);
    for (const char* name : Nx2Profile::zone_names) {
        REQUIRE(name != nullptr);
    }
}
