// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "frontend_common/nx_archive.h"

using namespace FrontendCommon::NxArchive;

TEST_CASE("NX tools: saved archives get the .nx extension", "[frontend][nx]") {
    CHECK(WithNxExtension("/games/Game") == "/games/Game.nx");
    CHECK(WithNxExtension("/games/Game.nsp") == "/games/Game.nsp.nx");
    CHECK(WithNxExtension("/games/Game.nx") == "/games/Game.nx");
    CHECK(WithNxExtension("/games/Game.NX") == "/games/Game.NX");
    CHECK(WithNxExtension("nx") == "nx.nx");
}
