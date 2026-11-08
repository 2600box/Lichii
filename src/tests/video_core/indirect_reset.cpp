// SPDX-License-Identifier: GPL-3.0-or-later

#include <limits>
#include <catch2/catch_test_macros.hpp>
#include "video_core/indirect_reset.h"

TEST_CASE("Indirect reset validates list bounds", "[video_core][indirect_reset]") {
    using VideoCommon::IndirectResetRange;
    const auto valid = IndirectResetRange(0x1000, 800, 4, 96);
    REQUIRE(valid);
    REQUIRE(valid->begin == 0x1020);
    REQUIRE(valid->size == 768);
    REQUIRE_FALSE(IndirectResetRange(0, 800, 0, 1));
    REQUIRE_FALSE(IndirectResetRange(0x1000, 800, 0, 0));
    REQUIRE_FALSE(IndirectResetRange(0x1000, 800, 5, 96));
    REQUIRE_FALSE(IndirectResetRange(0x1000, 0xffffffffU, 0, (1U << 20) + 1));
    REQUIRE_FALSE(IndirectResetRange((std::numeric_limits<u64>::max)() - 3, 800, 0, 1));
    REQUIRE_FALSE(IndirectResetRange((std::numeric_limits<u64>::max)() - 3, 800, 1, 1));
}

TEST_CASE("Indirect reset merges records into runs", "[video_core][indirect_reset]") {
    using VideoCommon::IndirectResetRecord;
    using VideoCommon::IndirectResetRun;
    using VideoCommon::IndirectResetRuns;
    const auto same = [](u64 gpu) { return IndirectResetRecord{gpu, gpu + 0x100000}; };
    const auto runs = IndirectResetRuns(
        {same(0x1028), same(0x1000), same(0x2000), same(0x1014), same(0x1000), same(0x1060),
         same(0x2000), same(0x103c)},
        20);
    REQUIRE(runs);
    REQUIRE(runs->size() == 3);
    const auto check = [](const IndirectResetRun& run, u64 gpu, u64 size) {
        REQUIRE(run.gpu_addr == gpu);
        REQUIRE(run.device_addr == gpu + 0x100000);
        REQUIRE(run.size == size);
    };
    check((*runs)[0], 0x1000, 80);
    check((*runs)[1], 0x1060, 20);
    check((*runs)[2], 0x2000, 20);

    const auto single = IndirectResetRuns({same(0x4000)}, 20);
    REQUIRE(single);
    REQUIRE(single->size() == 1);
    check((*single)[0], 0x4000, 20);

    const auto split = IndirectResetRuns({{0x1014, 0x9000}, {0x1000, 0x5000}}, 20);
    REQUIRE(split);
    REQUIRE(split->size() == 2);
    REQUIRE((*split)[0].gpu_addr == 0x1000);
    REQUIRE((*split)[0].device_addr == 0x5000);
    REQUIRE((*split)[0].size == 20);
    REQUIRE((*split)[1].gpu_addr == 0x1014);
    REQUIRE((*split)[1].device_addr == 0x9000);
    REQUIRE((*split)[1].size == 20);

    REQUIRE_FALSE(IndirectResetRuns({same(0x1000), same(0x1008)}, 20));
    REQUIRE_FALSE(IndirectResetRuns({same(0x1028), same(0x1000), same(0x1014), same(0x1030)}, 20));
    REQUIRE(IndirectResetRuns({}, 20)->empty());
}
