// SPDX-License-Identifier: GPL-3.0-or-later

#include <type_traits>
#include <catch2/catch_test_macros.hpp>
#include "video_core/dma_pusher.h"

TEST_CASE("GPFIFO capture configuration is declared by the shared API", "[video_core][gpfifo]") {
    // Do not rely on the x86-only macro JIT header to declare this API.
    STATIC_REQUIRE(std::is_same_v<decltype(Tegra::GpfifoCapture::Configured()), bool>);
    STATIC_REQUIRE(noexcept(Tegra::GpfifoCapture::Configured()));
}
