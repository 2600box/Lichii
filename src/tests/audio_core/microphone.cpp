// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <catch2/catch_test_macros.hpp>
#include "audio_core/sink/input_processing.h"

TEST_CASE("Microphone PCM conversion, gain and level meter", "[audio_core]") {
    using namespace AudioCore::Sink;
    const std::array<s16, 4> stereo{1000, -2000, 20000, -32768};
    const auto amplified = ConvertInputSamples(stereo, 6, 2, 2, 2.0f);
    REQUIRE(amplified == std::vector<s16>({2000, -4000, 32767, -32768, 0, 0}));
    REQUIRE(InputPeak(stereo, 2.0f) == 32768);
    REQUIRE(InputPeak(stereo, 0.0f) == 0);
    REQUIRE(InputPeak(stereo, 0.5f) == 16384);
    REQUIRE(ConvertInputSamples(stereo, 4, 2, 2, 0.0f) == std::vector<s16>(4, 0));
    REQUIRE(ConvertInputSamples({}, 8, 2, 2, 1.0f) == std::vector<s16>(8, 0));
    REQUIRE(ConvertInputSamples(stereo, 12, 2, 6, 1.0f) ==
            std::vector<s16>({1000, -2000, 0, 0, 0, 0, 20000, -32768, 0, 0, 0, 0}));
}
