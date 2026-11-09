// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "video_core/nx2.h"

TEST_CASE("Nx2SemaphoreMirror maps second-page releases to the first page", "[video_core][nx2]") {
    constexpr u64 region = 0x2106027000;
    constexpr u64 other = 0x2106100000;
    Tegra::Nx2ClearSemaphoreRegions();
    CHECK(Tegra::Nx2SemaphoreMirror(region + 0x1010) == 0);

    Tegra::Nx2AddSemaphoreRegion(region);
    CHECK(Tegra::Nx2SemaphoreMirror(region + 0x1000) == region);
    CHECK(Tegra::Nx2SemaphoreMirror(region + 0x1010) == region + 0x10);
    CHECK(Tegra::Nx2SemaphoreMirror(region + 0x1ffc) == region + 0xffc);
    CHECK(Tegra::Nx2SemaphoreMirror(region) == 0);
    CHECK(Tegra::Nx2SemaphoreMirror(region + 0xffc) == 0);
    CHECK(Tegra::Nx2SemaphoreMirror(region + 0x2000) == 0);
    CHECK(Tegra::Nx2SemaphoreMirror(other + 0x1010) == 0);

    Tegra::Nx2AddSemaphoreRegion(other);
    Tegra::Nx2AddSemaphoreRegion(region);
    CHECK(Tegra::Nx2SemaphoreMirror(other + 0x1010) == other + 0x10);
    CHECK(Tegra::Nx2SemaphoreMirror(region + 0x1010) == region + 0x10);

    Tegra::Nx2RemoveSemaphoreRegion(region);
    CHECK(Tegra::Nx2SemaphoreMirror(region + 0x1010) == 0);
    CHECK(Tegra::Nx2SemaphoreMirror(other + 0x1010) == other + 0x10);

    Tegra::Nx2ClearSemaphoreRegions();
    CHECK(Tegra::Nx2SemaphoreMirror(other + 0x1010) == 0);
}
