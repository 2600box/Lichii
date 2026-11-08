// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <filesystem>
#include <fstream>

#include <catch2/catch_test_macros.hpp>

#include "frontend_common/nx_archive.h"

using namespace FrontendCommon::NxArchive;

namespace {

struct TempDir {
    std::filesystem::path path;
    explicit TempDir(const char* name)
        : path{std::filesystem::temp_directory_path() / name} {
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::permissions(path, std::filesystem::perms::owner_all, ec);
        std::filesystem::remove_all(path, ec);
    }
};

} // Anonymous namespace

TEST_CASE("NX tools: saved archives get the .nx extension", "[frontend][nx]") {
    CHECK(WithNxExtension("/games/Game") == "/games/Game.nx");
    CHECK(WithNxExtension("/games/Game.nsp") == "/games/Game.nsp.nx");
    CHECK(WithNxExtension("/games/Game.nx") == "/games/Game.nx");
    CHECK(WithNxExtension("/games/Game.NX") == "/games/Game.NX");
    CHECK(WithNxExtension("nx") == "nx.nx");
}

TEST_CASE("NX tools: listing a folder never throws", "[frontend][nx]") {
    TempDir dir{"lichii-nx-regular-files"};
    std::ofstream{dir.path / "update.nsp"} << "x";
    std::filesystem::create_directory(dir.path / "sub");
    const auto files = RegularFiles(dir.path);
    REQUIRE(files.size() == 1);
    CHECK(files.front().filename() == "update.nsp");

    CHECK(RegularFiles(dir.path / "missing").empty());
    CHECK(RegularFiles(dir.path / "update.nsp").empty());
    std::filesystem::permissions(dir.path, std::filesystem::perms::none);
    CHECK_NOTHROW(RegularFiles(dir.path));
}
