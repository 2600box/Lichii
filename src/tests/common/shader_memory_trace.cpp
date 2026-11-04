// SPDX-License-Identifier: GPL-3.0-or-later

#include <limits>
#include <catch2/catch_test_macros.hpp>

#include "common/shader_memory_trace.h"

namespace {
std::vector<u8> ReferenceBytes() {
    std::vector<u8> bytes(0x1200, 0x5a);
    for (u32 i = 0; i < 64; ++i)
        bytes[i] = static_cast<u8>(i + 1);
    return bytes;
}
} // namespace

TEST_CASE("Shader trace identifies the first lost half-page without modifying samples",
          "[common][shader_memory_trace]") {
    const auto reference = ReferenceBytes();
    Common::ShaderMemoryTrace::Tracker tracker{reference};
    std::vector<Common::ShaderMemoryTrace::Observation> events;
    const auto emit = [&](auto event) { events.push_back(event); };
    tracker.Observe(0x1800, std::span{reference}.first(0x800), emit);
    REQUIRE(events.size() == 1);
    CHECK(events.back().first_difference == reference.size());
    CHECK(events.back().offset == 0);
    CHECK(events.back().size == 0x800);

    tracker.Track(0x9000, 0x800, 0x1000);
    std::vector<u8> lost(0x1000);
    tracker.Observe(0x9000, lost, emit);
    REQUIRE(events.size() == 2);
    CHECK(events.back().offset == 0x800);
    CHECK(events.back().size == 0xa00);
    CHECK(events.back().first_difference == 0x800);
    CHECK(events.back().expected == 0x5a);
    CHECK(events.back().actual == 0);
    CHECK(lost == std::vector<u8>(0x1000));

    tracker.Observe(0x9100, std::span{reference}.subspan(0x900, 0x100), emit);
    REQUIRE(events.size() == 3);
    CHECK(events.back().first_difference == reference.size());
    CHECK(events.back().offset == 0x900);
}

TEST_CASE("Shader trace discovers an embedded upload and ignores unrelated writes",
          "[common][shader_memory_trace]") {
    const auto reference = ReferenceBytes();
    Common::ShaderMemoryTrace::Tracker tracker{reference};
    std::vector<u8> upload(0x80, 0);
    upload.insert(upload.end(), reference.begin(), reference.end());
    std::vector<Common::ShaderMemoryTrace::Observation> events;
    const auto emit = [&](auto event) { events.push_back(event); };
    tracker.Observe(0x10000, upload, emit);
    REQUIRE(events.size() == 1);
    CHECK(events.back().base == 0x10080);
    CHECK(events.back().first_difference == reference.size());
    tracker.Observe(0x50000, std::array<u8, 64>{}, emit);
    CHECK(events.size() == 1);
    tracker.Track(0x10080, 0, 0x800);
    tracker.Observe(0x10880, std::array<u8, 16>{}, emit);
    REQUIRE(events.size() == 2);
    CHECK(events.back().first_difference == 0x800);
}

TEST_CASE("Shader trace rejects incomplete references and overflowing ranges",
          "[common][shader_memory_trace]") {
    size_t events{};
    const auto emit = [&](auto) { ++events; };
    Common::ShaderMemoryTrace::Tracker small{std::vector<u8>(63)};
    small.Observe(0x1000, std::array<u8, 128>{}, emit);
    CHECK(events == 0);
    const auto reference = ReferenceBytes();
    Common::ShaderMemoryTrace::Tracker tracker{reference};
    tracker.Observe(std::numeric_limits<uintptr_t>::max() - 20, reference, emit);
    tracker.Track(std::numeric_limits<uintptr_t>::max() - 20, 0, 64);
    tracker.Observe(0x1000, std::array<u8, 128>{}, emit);
    CHECK(events == 0);
}
