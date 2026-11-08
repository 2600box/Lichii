// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <atomic>
#include <cstring>
#include <functional>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "core/file_sys/control_metadata.h"
#include "core/file_sys/nx_container.h"
#include "core/file_sys/romfs.h"
#include "core/file_sys/vfs/vfs_real.h"
#include "core/file_sys/vfs/vfs_vector.h"
#include "core/loader/loader.h"
#include "core/loader/nx.h"

namespace {

using FileSys::NxContainer;

constexpr u64 ApplicationId = 0x0100123456780000;
constexpr std::size_t BigSize = (std::size_t{17} << 20) + 12345;
constexpr std::size_t RandomSize = 160 * 1024 + 77;

FileSys::VirtualFile OpenFixture(std::string_view name) {
    static const auto filesystem = std::make_shared<FileSys::RealVfsFilesystem>();
    return filesystem->OpenFile(std::string(LICHII_NX_FIXTURE_DIR) + "/" + std::string(name),
                                FileSys::OpenMode::Read);
}

/// The generator's xorshift64 byte stream (see nx_fixtures/README.md).
std::vector<u8> Xorshift(u64 x, std::size_t count) {
    std::vector<u8> out(count);
    for (auto& byte : out) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        byte = static_cast<u8>(x >> 32);
    }
    return out;
}

const std::vector<u8>& BigPeriod() {
    static const auto period = Xorshift(0x9e3779b97f4a7c15, 1021);
    return period;
}

const std::vector<u8>& RandomContents() {
    static const auto contents = Xorshift(0x2545f4914f6cdd1d, RandomSize);
    return contents;
}

std::vector<u8> Expected(std::string_view file, std::size_t offset, std::size_t length) {
    std::vector<u8> out(length);
    for (std::size_t i = 0; i < length; ++i) {
        out[i] = file == "big.bin" ? BigPeriod()[(offset + i) % BigPeriod().size()]
                                   : RandomContents()[offset + i];
    }
    return out;
}

FileSys::VirtualFile RomFSFile(NxContainer& nx, std::size_t snapshot, std::string_view name) {
    const auto root = nx.GetSnapshotDir(snapshot);
    REQUIRE(root != nullptr);
    const auto romfs = root->GetSubdirectory("romfs");
    REQUIRE(romfs != nullptr);
    return romfs->GetFile(name);
}

std::string ReadText(const FileSys::VirtualFile& file) {
    REQUIRE(file != nullptr);
    const auto bytes = file->ReadAllBytes();
    return std::string(bytes.begin(), bytes.end());
}

/// Reads every file of a tree in full; returns false on any short read.
bool ReadTree(const FileSys::VirtualDir& dir) {
    for (const auto& file : dir->GetFiles()) {
        if (file->ReadAllBytes().size() != file->GetSize()) {
            return false;
        }
    }
    return std::ranges::all_of(dir->GetSubdirectories(), ReadTree);
}

enum class Expect { Accept, Reject, RejectFullVerify };

struct UpstreamFixture {
    std::string_view name;
    Expect expect;
};

constexpr UpstreamFixture UpstreamFixtures[] = {
    {"minimal_valid.nx", Expect::Accept},
    {"nested_vfs.nx", Expect::Accept},
    {"zstd_single.nx", Expect::Accept},
    {"shared_snapshots.nx", Expect::Accept},
    {"deleted_between_versions.nx", Expect::Accept},
    {"multiple_programs.nx", Expect::Accept},
    {"jpeg_icon.nx", Expect::Accept},
    {"localized_metadata_icons.nx", Expect::Accept},
    {"dlc_with_program.nx", Expect::Accept},
    {"empty_files.nx", Expect::Accept},
    {"lost_header_page_0.nx", Expect::Accept},
    {"appended_generation.nx", Expect::Accept},
    {"interrupted_commit.nx", Expect::Accept},
    {"corrupted_manifest.nx", Expect::Reject},
    {"corrupted_hot_index.nx", Expect::Reject},
    {"cyclic_directory.nx", Expect::Reject},
    {"repeated_directory.nx", Expect::Reject},
    {"repeated_file.nx", Expect::Reject},
    {"invalid_path.nx", Expect::Reject},
    {"truncated_archive.nx", Expect::Reject},
    {"truncated_header_area.nx", Expect::Reject},
    {"damaged_both_slots.nx", Expect::Reject},
    {"unknown_mandatory_feature.nx", Expect::Reject},
    {"slot_generation_conflict.nx", Expect::Reject},
    {"corrupted_block_payload.nx", Expect::RejectFullVerify},
    {"corrupted_orphan_block.nx", Expect::RejectFullVerify},
};

} // Anonymous namespace

TEST_CASE("NX: archives are identified by either header slot", "[core][nx]") {
    CHECK(Loader::IdentifyFile(OpenFixture("lichii_synthetic.nx")) == Loader::FileType::NX);
    CHECK(Loader::IdentifyFile(OpenFixture("lost_header_page_0.nx")) == Loader::FileType::NX);
    CHECK(Loader::GuessFromFilename("Game.NX") == Loader::FileType::NX);
    CHECK(Loader::GetFileTypeString(Loader::FileType::NX) == "NX");
    const auto zeros = std::make_shared<FileSys::VectorVfsFile>(std::vector<u8>(8192), "x.nx");
    CHECK_FALSE(NxContainer::IsNxFile(zeros));
}

TEST_CASE("NX: catalog and boot snapshot selection", "[core][nx]") {
    NxContainer nx{OpenFixture("lichii_synthetic.nx")};
    REQUIRE(nx.GetStatus() == Loader::ResultStatus::Success);
    REQUIRE(nx.GetApplications().size() == 1);
    const auto& application = nx.GetApplications()[0];
    CHECK(application.id == ApplicationId);
    CHECK(application.platform == 2);
    CHECK(application.title == "Synthetic Game");
    CHECK(application.publisher == "Synthetic publisher");

    const auto& snapshots = nx.GetSnapshots();
    REQUIRE(snapshots.size() == 3);
    CHECK(snapshots[0].type == NxContainer::SnapshotType::Base);
    CHECK(snapshots[1].type == NxContainer::SnapshotType::Update);
    CHECK(snapshots[1].version == 1);
    CHECK(snapshots[2].type == NxContainer::SnapshotType::Addon);
    CHECK(snapshots[2].addon_title_id == 0x0100123456781001);
    CHECK_FALSE(snapshots[2].program.has_value());

    CHECK(nx.FindBootSnapshot() == 1);
    CHECK(nx.FindBootSnapshot(ApplicationId) == 1);
    CHECK_FALSE(nx.FindBootSnapshot(0x0100000000001000).has_value());
    const auto nacp = nx.ReadMedia(snapshots[1].nacp_media);
    REQUIRE(nacp.has_value());
    CHECK(nacp->size() == sizeof(FileSys::RawNACP));
    CHECK_FALSE(nx.ReadMedia(NxContainer::NoMedia).has_value());
}

TEST_CASE("NX: newer snapshot wins and a base never shadows an update", "[core][nx]") {
    NxContainer nx{OpenFixture("shared_snapshots.nx")};
    REQUIRE(nx.GetStatus() == Loader::ResultStatus::Success);
    const auto boot = nx.FindBootSnapshot();
    REQUIRE(boot.has_value());
    for (const auto& snapshot : nx.GetSnapshots()) {
        if (snapshot.program == nx.GetSnapshots()[*boot].program) {
            CHECK(snapshot.version <= nx.GetSnapshots()[*boot].version);
        }
    }
}

TEST_CASE("NX: loader exposes the update's complete tree and metadata", "[core][nx]") {
    Loader::AppLoader_NX loader{OpenFixture("lichii_synthetic.nx")};
    u64 program_id{};
    REQUIRE(loader.ReadProgramId(program_id) == Loader::ResultStatus::Success);
    CHECK(program_id == ApplicationId);
    std::string title;
    REQUIRE(loader.ReadTitle(title) == Loader::ResultStatus::Success);
    CHECK(title == "Synthetic Game");
    FileSys::NACP nacp;
    REQUIRE(loader.ReadControlData(nacp) == Loader::ResultStatus::Success);
    CHECK(nacp.GetApplicationName() == "Synthetic Game");
    CHECK(nacp.GetVersionString() == "1.2.0");
    CHECK_FALSE(loader.IsRomFSUpdatable());

    FileSys::VirtualFile romfs;
    REQUIRE(loader.ReadRomFS(romfs) == Loader::ResultStatus::Success);
    const auto tree = FileSys::ExtractRomFS(romfs);
    REQUIRE(tree != nullptr);
    CHECK(ReadText(tree->GetFile("shadowed.bin")) == "update version");
    CHECK(ReadText(tree->GetFile("added.bin")) == "new in the update");
    CHECK(ReadText(tree->GetFile("common.bin")) == "base common data");
    const auto deep = tree->GetFileRelative("sub/deep.bin");
    REQUIRE(deep != nullptr);
    CHECK(deep->ReadAllBytes() == std::vector<u8>(5000, 7));
}

TEST_CASE("NX: icon media is read without mounting", "[core][nx]") {
    Loader::AppLoader_NX loader{OpenFixture("jpeg_icon.nx")};
    std::vector<u8> icon;
    REQUIRE(loader.ReadIcon(icon) == Loader::ResultStatus::Success);
    CHECK(icon.size() == 285);
    CHECK(icon[0] == 0xFF);
    CHECK(icon[1] == 0xD8);
}

TEST_CASE("NX: reads span blocks, codecs and the reader's 16 MiB read limit", "[core][nx]") {
    NxContainer nx{OpenFixture("lichii_synthetic.nx")};
    const auto big = RomFSFile(nx, 1, "big.bin");
    const auto random = RomFSFile(nx, 1, "random.bin");
    REQUIRE(big != nullptr);
    REQUIRE(random != nullptr);
    REQUIRE(big->GetSize() == BigSize);
    REQUIRE(random->GetSize() == RandomSize);

    CHECK(big->ReadAllBytes() == Expected("big.bin", 0, BigSize));
    CHECK(random->ReadAllBytes() == RandomContents());

    std::mt19937_64 rng{1234};
    for (int i = 0; i < 200; ++i) {
        const auto& file = (i % 2) ? big : random;
        const std::size_t size = file->GetSize();
        const std::size_t offset = rng() % size;
        const std::size_t length = std::min<std::size_t>(rng() % (3 << 20), size - offset);
        std::vector<u8> out(length);
        REQUIRE(file->Read(out.data(), length, offset) == length);
        REQUIRE(out == Expected(file->GetName(), offset, length));
    }

    std::vector<u8> tail(64);
    CHECK(random->Read(tail.data(), tail.size(), RandomSize - 10) == 10);
    CHECK(random->Read(tail.data(), tail.size(), RandomSize) == 0);
}

TEST_CASE("NX: concurrent reads return the same bytes", "[core][nx]") {
    NxContainer nx{OpenFixture("lichii_synthetic.nx")};
    const FileSys::VirtualFile files[] = {RomFSFile(nx, 1, "big.bin"),
                                          RomFSFile(nx, 1, "random.bin")};
    std::atomic<int> mismatches{0};
    std::vector<std::thread> threads;
    for (unsigned t = 0; t < 8; ++t) {
        threads.emplace_back([&, t] {
            std::mt19937_64 rng{t};
            for (int i = 0; i < 40; ++i) {
                const auto& file = files[(t + i) % 2];
                const std::size_t offset = rng() % file->GetSize();
                const std::size_t length =
                    std::min<std::size_t>(rng() % (1 << 21), file->GetSize() - offset);
                std::vector<u8> out(length);
                if (file->Read(out.data(), length, offset) != length ||
                    out != Expected(file->GetName(), offset, length)) {
                    ++mismatches;
                }
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    CHECK(mismatches == 0);
}

TEST_CASE("NX: files stay readable after the container is destroyed", "[core][nx]") {
    FileSys::VirtualFile file;
    {
        NxContainer nx{OpenFixture("lichii_synthetic.nx")};
        file = RomFSFile(nx, 0, "shadowed.bin");
    }
    CHECK(ReadText(file) == "base version");
}

TEST_CASE("NX: upstream conformance fixtures", "[core][nx]") {
    for (const auto& fixture : UpstreamFixtures) {
        INFO(fixture.name);
        NxContainer nx{OpenFixture(fixture.name)};
        const bool mounted = nx.GetStatus() == Loader::ResultStatus::Success &&
                             nx.Mount() == Loader::ResultStatus::Success;
        if (fixture.expect == Expect::Reject) {
            CHECK_FALSE(mounted);
            continue;
        }
        REQUIRE(mounted);
        const bool verified = nx.VerifyIntegrity({}) == Loader::ResultStatus::Success;
        CHECK(verified == (fixture.expect == Expect::Accept));
        if (fixture.expect == Expect::Accept) {
            for (std::size_t i = 0; i < nx.GetSnapshots().size(); ++i) {
                const auto root = nx.GetSnapshotDir(i);
                REQUIRE(root != nullptr);
                CHECK(ReadTree(root));
            }
        }
    }
}

TEST_CASE("NX: truncated and corrupted archives fail cleanly", "[core][nx]") {
    const auto bytes = OpenFixture("lichii_synthetic.nx")->ReadAllBytes();
    for (const std::size_t size : {std::size_t{0}, std::size_t{100}, std::size_t{4096},
                                   std::size_t{8191}, std::size_t{8192}, bytes.size() / 2,
                                   bytes.size() - 1}) {
        INFO(size);
        NxContainer nx{std::make_shared<FileSys::VectorVfsFile>(
            std::vector<u8>(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(size)))};
        CHECK_FALSE((nx.GetStatus() == Loader::ResultStatus::Success &&
                     nx.Mount() == Loader::ResultStatus::Success));
    }
    std::mt19937_64 rng{99};
    for (int i = 0; i < 64; ++i) {
        auto corrupted = bytes;
        // Past the two header pages: damage to one slot is recovered from the other by design.
        const std::size_t at = 8192 + rng() % (corrupted.size() - 8192);
        corrupted[at] ^= static_cast<u8>(1 + rng() % 255);
        INFO(at);
        NxContainer nx{std::make_shared<FileSys::VectorVfsFile>(std::move(corrupted))};
        if (nx.GetStatus() != Loader::ResultStatus::Success ||
            nx.Mount() != Loader::ResultStatus::Success) {
            continue;
        }
        CHECK(nx.VerifyIntegrity({}) != Loader::ResultStatus::Success);
        const auto big = RomFSFile(nx, 1, "big.bin");
        const auto random = RomFSFile(nx, 1, "random.bin");
        // A read either returns the original bytes or stops short at the damaged block.
        for (const auto& file : {big, random}) {
            const auto read = file->ReadAllBytes();
            const auto expected = Expected(file->GetName(), 0, read.size());
            CHECK(read == expected);
        }
    }
}
