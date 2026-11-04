// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "video_core/nx2.h"

using Tegra::Nx2SemaphoreWaiters;

namespace {

constexpr u64 Semaphore = 0xf733e100;

struct ResetWaiters {
    ResetWaiters() {
        Nx2SemaphoreWaiters::Get().Reset();
    }
    ~ResetWaiters() {
        Nx2SemaphoreWaiters::Get().Reset();
    }
};

} // namespace

TEST_CASE("GPU acquires see a held release before its write lands",
          "[video_core][nx2_semaphore_hold]") {
    ResetWaiters reset;
    auto& waiters = Nx2SemaphoreWaiters::Get();
    REQUIRE(waiters.Read(Semaphore, 0x663) == 0x663);
    waiters.Hold(Semaphore, 0x664);
    REQUIRE(waiters.Read(Semaphore, 0x663) == 0x664);
    REQUIRE(waiters.Read(Semaphore + 4, 7) == 7);
    waiters.Drop(Semaphore);
    REQUIRE(waiters.Read(Semaphore, 0x664) == 0x664);
}

TEST_CASE("Held releases to one semaphore show the newest until all have landed",
          "[video_core][nx2_semaphore_hold]") {
    ResetWaiters reset;
    auto& waiters = Nx2SemaphoreWaiters::Get();
    waiters.Hold(Semaphore, 1);
    waiters.Hold(Semaphore, 2);
    waiters.Drop(Semaphore);
    REQUIRE(waiters.Read(Semaphore, 1) == 2);
    waiters.Drop(Semaphore);
    REQUIRE(waiters.Read(Semaphore, 2) == 2);
}

TEST_CASE("A held release latches the acquires it meets", "[video_core][nx2_semaphore_hold]") {
    ResetWaiters reset;
    auto& waiters = Nx2SemaphoreWaiters::Get();
    const int equal{};
    const int greater{};
    waiters.Add(&equal, Semaphore, 2, Nx2SemaphoreWaiters::AcquireEqual);
    waiters.Add(&greater, Semaphore, 3, Nx2SemaphoreWaiters::AcquireGequal);
    waiters.Hold(Semaphore, 2);
    waiters.Hold(Semaphore, 3);
    REQUIRE(waiters.Latched(&equal));
    REQUIRE(waiters.Latched(&greater));
}

TEST_CASE("A new session forgets the releases a stopped one held",
          "[video_core][nx2_semaphore_hold]") {
    ResetWaiters reset;
    auto& waiters = Nx2SemaphoreWaiters::Get();
    const int owner{};
    waiters.Add(&owner, Semaphore, 5, Nx2SemaphoreWaiters::AcquireGequal);
    waiters.Hold(Semaphore, 5);
    waiters.Reset();
    REQUIRE(waiters.Read(Semaphore, 4) == 4);
    REQUIRE_FALSE(waiters.Latched(&owner));
}
