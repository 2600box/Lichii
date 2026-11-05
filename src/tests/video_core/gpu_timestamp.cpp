// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <chrono>

#include <catch2/catch_test_macros.hpp>

#include "video_core/gpu_timestamp.h"

namespace {

struct Timing {
    std::chrono::nanoseconds time;
    u64 gpu_ticks;

    std::chrono::nanoseconds GetGlobalTimeNs() const {
        return time;
    }

    u64 GetGPUTicks() const {
        return gpu_ticks;
    }
};

constexpr std::array Clocks{Settings::GpuClock::Normal, Settings::GpuClock::Boost,
                            Settings::GpuClock::Overclock};

} // namespace

TEST_CASE("Ampere report timestamps use driver and VI nanoseconds", "[video_core][gpu_timestamp]") {
    constexpr u64 HostTimeNs = 293'036'000'000'000;
    const Timing timing{std::chrono::nanoseconds{HostTimeNs}, HostTimeNs * 384 / 625};
    for (const auto clock : Clocks) {
        CAPTURE(clock);
        REQUIRE(Tegra::ReadGpuTimestamp(timing, true, clock) == HostTimeNs);
    }
}

TEST_CASE("Ampere CPU GPU calibration stays valid between samples", "[video_core][gpu_timestamp]") {
    constexpr u64 HostStartNs = 293'036'000'000'000;
    constexpr u64 GuestStartNs = 95'000'000'000;
    constexpr u64 Offset = GuestStartNs - HostStartNs;
    constexpr u64 GpuDurationNs = 5'000'000;
    constexpr u64 PresentDelayNs = 200'000;
    for (const auto clock : Clocks) {
        for (u64 elapsed = 0; elapsed <= 1'000'000'000; elapsed += 31'250'000) {
            CAPTURE(clock, elapsed);
            const u64 start_ns = HostStartNs + elapsed;
            const u64 end_ns = start_ns + GpuDurationNs;
            const Timing begin{std::chrono::nanoseconds{start_ns}, start_ns * 384 / 625};
            const Timing end{std::chrono::nanoseconds{end_ns}, end_ns * 384 / 625};
            const u64 gpu_begin = Tegra::ReadGpuTimestamp(begin, true, clock);
            const u64 gpu_end = Tegra::ReadGpuTimestamp(end, true, clock);
            const u64 present = end_ns + PresentDelayNs + Offset;
            REQUIRE(gpu_begin + Offset == GuestStartNs + elapsed);
            REQUIRE(gpu_end - gpu_begin == GpuDurationNs);
            REQUIRE(present - (gpu_end + Offset) == PresentDelayNs);
        }
    }
}

TEST_CASE("GPU time divisor shrinks durations since a present", "[video_core][gpu_timestamp]") {
    constexpr u64 AnchorNs = 293'036'000'000'000;
    constexpr u64 FrameNs = 50'000'000;
    for (const u64 divisor : {u64{0}, u64{1}}) {
        CAPTURE(divisor);
        REQUIRE(Tegra::DivideSinceAnchor(AnchorNs + FrameNs, AnchorNs, divisor) ==
                AnchorNs + FrameNs);
    }
    const u64 begin = Tegra::DivideSinceAnchor(AnchorNs + 10'000'000, AnchorNs, 4);
    const u64 end = Tegra::DivideSinceAnchor(AnchorNs + 50'000'000, AnchorNs, 4);
    REQUIRE(end - begin == 10'000'000);
    REQUIRE(AnchorNs + FrameNs - end < FrameNs);
    REQUIRE(Tegra::DivideSinceAnchor(AnchorNs - 1, AnchorNs, 4) == AnchorNs - 1);
}

TEST_CASE("GPU time divisor stays monotonic when the anchor moves", "[video_core][gpu_timestamp]") {
    constexpr u64 StartNs = 95'000'000'000;
    constexpr u64 FrameNs = 33'333'333;
    u64 anchor = StartNs;
    u64 last = 0;
    for (u64 now = StartNs; now < StartNs + 10 * FrameNs; now += 1'000'000) {
        if (now - anchor >= FrameNs) {
            anchor = now;
        }
        const u64 stamp = Tegra::DivideSinceAnchor(now, anchor, 4);
        CAPTURE(now, anchor);
        REQUIRE(stamp >= last);
        REQUIRE(stamp <= now);
        last = stamp;
    }
}

TEST_CASE("Maxwell retains its tick units and clock scaling", "[video_core][gpu_timestamp]") {
    for (const u64 ticks : {u64{0}, u64{1}, u64{255}, u64{256}, u64{511},
                           u64{512}, u64{614'400'000}, u64{180'041'318'400'000}}) {
        CAPTURE(ticks);
        const Timing timing{std::chrono::nanoseconds{293'036'000'000'000}, ticks};
        REQUIRE(Tegra::ReadGpuTimestamp(timing, false, Settings::GpuClock::Normal) == ticks);
        REQUIRE(Tegra::ReadGpuTimestamp(timing, false, Settings::GpuClock::Boost) == ticks / 256);
        REQUIRE(Tegra::ReadGpuTimestamp(timing, false, Settings::GpuClock::Overclock) == ticks / 512);
    }
}
