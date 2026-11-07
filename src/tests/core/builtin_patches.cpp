// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>
#include "common/common_funcs.h"
#include "common/fs/path_util.h"
#include "common/hex_util.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/file_sys/ocarina_ultrawide.h"
#include "core/file_sys/patch_manager.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/vfs/vfs_real.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "core/loader/nso.h"

namespace {

constexpr u64 OcarinaTitleId = 0x0400486024DEC000;
constexpr u64 OtherTitleId = 0x0100000000010000;
constexpr std::string_view MaxResolution = "Max Resolution (built-in)";
constexpr std::string_view DynamicFps = "Dynamic FPS by Dark (built-in)";
constexpr std::string_view OcarinaBuildIdHex = "E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3348";
constexpr FileSys::PatchManager::BuildID OcarinaBuildId{
    0xE0, 0xF3, 0xCC, 0xFA, 0xF5, 0x00, 0x65, 0xAE, 0x4E, 0xA0,
    0x1E, 0xB5, 0xAE, 0xB1, 0x3B, 0x1A, 0x45, 0xDA, 0x33, 0x48,
};
constexpr size_t PatchOffset = sizeof(Loader::NSOHeader) + 0x805BD0;
constexpr u32 TbzInstruction = 0x360041E8;
constexpr u32 BranchInstruction = 0x1400020F;
constexpr u32 NopInstruction = 0xD503201F;
constexpr size_t DynamicCallOffset = sizeof(Loader::NSOHeader) + 0x6E6CC4;
constexpr size_t DynamicRoutineOffset = sizeof(Loader::NSOHeader) + 0x3C46200;
constexpr size_t DynamicStateOffset = sizeof(Loader::NSOHeader) + 0x4E9FE00;
constexpr size_t DynamicRoutineSize = 120;
constexpr u32 DynamicCallInstruction = 0x94D57D4F;
constexpr u32 DynamicOriginalInstruction = 0x1E201828;

constexpr std::array<std::pair<Common::FS::EdenPath, const char*>, 4> TestDirectories{{
    {Common::FS::EdenPath::NANDDir, "nand"},
    {Common::FS::EdenPath::SDMCDir, "sdmc"},
    {Common::FS::EdenPath::LoadDir, "load"},
    {Common::FS::EdenPath::DumpDir, "dump"},
}};

void WriteIps(const std::filesystem::path& path, u32 word, size_t offset = PatchOffset) {
    const bool ips32 = offset > 0xFFFFFF;
    std::vector<u8> ips =
        ips32 ? std::vector<u8>{'I', 'P', 'S', '3', '2'} : std::vector<u8>{'P', 'A', 'T', 'C', 'H'};
    if (ips32) {
        ips.push_back(static_cast<u8>(offset >> 24));
    }
    ips.push_back(static_cast<u8>(offset >> 16));
    ips.push_back(static_cast<u8>(offset >> 8));
    ips.push_back(static_cast<u8>(offset));
    ips.push_back(0);
    ips.push_back(sizeof(word));
    for (size_t i = 0; i < sizeof(word); ++i) {
        ips.push_back(static_cast<u8>(word >> (i * 8)));
    }
    if (ips32) {
        ips.push_back('E');
    }
    ips.insert(ips.end(), {'E', 'O', 'F'});
    std::ofstream file{path, std::ios::binary};
    file.write(reinterpret_cast<const char*>(ips.data()), static_cast<std::streamsize>(ips.size()));
}

class PatchManagerTestContext {
public:
    explicit PatchManagerTestContext(std::optional<u32> user_mod_word = std::nullopt,
                                     size_t user_mod_offset = PatchOffset) {
        for (size_t i = 0; i < TestDirectories.size(); ++i) {
            const auto& [path, name] = TestDirectories[i];
            previous[i] = Common::FS::GetEdenPath(path);
            std::filesystem::create_directories(root / name);
            Common::FS::SetEdenPath(path, root / name);
        }
        const auto title_dir = root / "load" / fmt::format("{:016X}", OcarinaTitleId);
        std::filesystem::create_directories(title_dir);
        if (user_mod_word) {
            const auto exefs = title_dir / "UserMod" / "exefs";
            std::filesystem::create_directories(exefs);
            WriteIps(exefs / fmt::format("{}.ips", OcarinaBuildIdHex), *user_mod_word,
                     user_mod_offset);
        }
        system.SetFilesystem(std::make_shared<FileSys::RealVfsFilesystem>());
        system.SetContentProvider(std::make_unique<FileSys::ContentProviderUnion>());
        system.GetFileSystemController().CreateFactories(*system.GetFilesystem());
    }

    ~PatchManagerTestContext() {
        for (size_t i = 0; i < TestDirectories.size(); ++i) {
            Common::FS::SetEdenPath(TestDirectories[i].first, previous[i]);
        }
        Settings::values.disabled_addons.erase(OcarinaTitleId);
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }

    PatchManagerTestContext(const PatchManagerTestContext&) = delete;
    PatchManagerTestContext& operator=(const PatchManagerTestContext&) = delete;

    FileSys::PatchManager Manager(u64 title_id = OcarinaTitleId) {
        return FileSys::PatchManager{title_id, system.GetFileSystemController(),
                                     system.GetContentProvider()};
    }

private:
    std::array<std::filesystem::path, TestDirectories.size()> previous;
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("builtin-patch-test-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Core::System system;
};

std::vector<u8> MakeMain(const FileSys::PatchManager::BuildID& build_id, u32 word) {
    std::vector<u8> nso(PatchOffset + 0x100);
    Loader::NSOHeader header{};
    header.magic = Common::MakeMagic('N', 'S', 'O', '0');
    header.build_id = build_id;
    std::memcpy(nso.data(), &header, sizeof(header));
    std::memcpy(nso.data() + PatchOffset, &word, sizeof(word));
    return nso;
}

u32 WordAt(const std::vector<u8>& nso, size_t offset = PatchOffset) {
    REQUIRE(nso.size() >= offset + sizeof(u32));
    u32 word;
    std::memcpy(&word, nso.data() + offset, sizeof(word));
    return word;
}

u32 PatchMain(PatchManagerTestContext& context, const FileSys::PatchManager::BuildID& build_id,
              u32 word, const std::string& name = "main", u64 title_id = OcarinaTitleId) {
    return WordAt(context.Manager(title_id).PatchNSO(MakeMain(build_id, word), name));
}

} // Anonymous namespace

TEST_CASE("Built-in Max Resolution patch is gated on build id and original word",
          "[patch_manager]") {
    PatchManagerTestContext context;

    SECTION("Matching build id and original word applies") {
        REQUIRE(PatchMain(context, OcarinaBuildId, TbzInstruction) == BranchInstruction);
    }

    SECTION("Wrong original word skips") {
        REQUIRE(PatchMain(context, OcarinaBuildId, NopInstruction) == NopInstruction);
    }

    SECTION("Other build id skips") {
        auto other = OcarinaBuildId;
        other[0] ^= 1;
        REQUIRE(PatchMain(context, other, TbzInstruction) == TbzInstruction);
    }

    SECTION("Other module skips") {
        REQUIRE(PatchMain(context, OcarinaBuildId, TbzInstruction, "sdk") == TbzInstruction);
    }

    SECTION("Other title skips") {
        REQUIRE(PatchMain(context, OcarinaBuildId, TbzInstruction, "main", OtherTitleId) ==
                TbzInstruction);
    }

    SECTION("Disabled in Add-Ons skips") {
        Settings::values.disabled_addons[OcarinaTitleId] = {std::string{MaxResolution}};
        REQUIRE(PatchMain(context, OcarinaBuildId, TbzInstruction) == TbzInstruction);
    }
}

TEST_CASE("Built-in Max Resolution patch defers to a user mod at the same offset",
          "[patch_manager]") {
    SECTION("A different word from the mod is kept") {
        PatchManagerTestContext context{NopInstruction};
        REQUIRE(PatchMain(context, OcarinaBuildId, TbzInstruction) == NopInstruction);
    }

    SECTION("The same word from the mod is applied once") {
        PatchManagerTestContext context{BranchInstruction};
        REQUIRE(PatchMain(context, OcarinaBuildId, TbzInstruction) == BranchInstruction);
    }

    SECTION("Disabling the built-in patch keeps the mod") {
        PatchManagerTestContext context{BranchInstruction};
        Settings::values.disabled_addons[OcarinaTitleId] = {std::string{MaxResolution}};
        REQUIRE(PatchMain(context, OcarinaBuildId, TbzInstruction) == BranchInstruction);
    }
}

TEST_CASE("Built-in Max Resolution patch is listed and loaded for Ocarina's main",
          "[patch_manager]") {
    PatchManagerTestContext context;
    const auto manager = context.Manager();

    REQUIRE(manager.HasNSOPatch(OcarinaBuildId, "main"));
    REQUIRE_FALSE(manager.HasNSOPatch(OcarinaBuildId, "sdk"));
    REQUIRE_FALSE(context.Manager(OtherTitleId).HasNSOPatch(OcarinaBuildId, "main"));

    const auto builtin_enabled = [&] {
        const auto patches = manager.GetPatches();
        const auto it = std::ranges::find(patches, MaxResolution, &FileSys::Patch::name);
        REQUIRE(it != patches.end());
        REQUIRE(it->type == FileSys::PatchType::Mod);
        return it->enabled;
    };
    REQUIRE(builtin_enabled());

    Settings::values.disabled_addons[OcarinaTitleId] = {std::string{MaxResolution}};
    REQUIRE_FALSE(builtin_enabled());
}

namespace {

std::vector<u8> MakeDynamicMain() {
    auto nso = MakeMain(OcarinaBuildId, TbzInstruction);
    nso.resize(DynamicStateOffset + 16);
    Loader::NSOHeader header;
    std::memcpy(&header, nso.data(), sizeof(header));
    header.segments[0].size = 63201744;
    header.segments[1].location = 63205376;
    header.segments[1].size = 12603520;
    header.segments[2].location = 75812864;
    header.segments[2].size = 5217152;
    header.segments[2].bss_size = 1414272;
    std::memcpy(nso.data(), &header, sizeof(header));
    std::memcpy(nso.data() + DynamicCallOffset, &DynamicOriginalInstruction,
                sizeof(DynamicOriginalInstruction));
    return nso;
}

} // Anonymous namespace

TEST_CASE("Built-in Dynamic FPS defaults on and can be disabled in Add-Ons", "[patch_manager]") {
    PatchManagerTestContext context;
    const auto manager = context.Manager();
    const auto enabled = [&] {
        const auto patches = manager.GetPatches();
        const auto it = std::ranges::find(patches, DynamicFps, &FileSys::Patch::name);
        REQUIRE(it != patches.end());
        REQUIRE(it->type == FileSys::PatchType::Mod);
        return it->enabled;
    };
    REQUIRE(enabled());
    auto nso = MakeDynamicMain();
    auto patched = manager.PatchNSO(nso, "main");
    REQUIRE(WordAt(patched, DynamicCallOffset) == DynamicCallInstruction);
    REQUIRE(WordAt(patched, DynamicRoutineOffset) == 0x1E201828);
    REQUIRE(WordAt(patched, DynamicRoutineOffset + 116) == 0x3F83D70A);
    REQUIRE(
        Common::HexToString(std::span{patched}.subspan(DynamicRoutineOffset, DynamicRoutineSize)) ==
        "2818201ECA9200B04A0138912BE03BD54C0140F94B0100F9CC0200B46B010CCB0DE03BD5"
        "8E0280D2AE09CE9A7F010EEBCB818B9A4C0540F94C0000B5EC030BAA6E010CCB8C0D8E8B"
        "4C0500F98201239EA301239E4218231E4208211EC300001C0309231E4020231E4D000054"
        "4840201EC0035FD60AD7833F");
    REQUIRE(WordAt(patched) == BranchInstruction);
    Settings::values.disabled_addons[OcarinaTitleId] = {std::string{DynamicFps}};
    REQUIRE_FALSE(enabled());
    REQUIRE(WordAt(manager.PatchNSO(nso, "main"), DynamicCallOffset) == DynamicOriginalInstruction);
    REQUIRE(WordAt(manager.PatchNSO(nso, "main"), DynamicRoutineOffset) == 0);

    Settings::values.disabled_addons[OcarinaTitleId] = {std::string{MaxResolution}};
    REQUIRE(enabled());
    patched = manager.PatchNSO(nso, "main");
    REQUIRE(WordAt(patched, DynamicCallOffset) == DynamicCallInstruction);
    REQUIRE(WordAt(patched) == TbzInstruction);

    const auto unrelated = context.Manager(OtherTitleId).GetPatches();
    REQUIRE(std::ranges::find(unrelated, DynamicFps, &FileSys::Patch::name) == unrelated.end());
}

TEST_CASE("Built-in Dynamic FPS rejects mismatched or truncated modules transactionally",
          "[patch_manager]") {
    PatchManagerTestContext context;
    Settings::values.disabled_addons[OcarinaTitleId] = {std::string{MaxResolution}};
    auto nso = MakeDynamicMain();
    std::string module = "main";
    u64 title = OcarinaTitleId;
    SECTION("Wrong build") {
        nso[offsetof(Loader::NSOHeader, build_id)] ^= 1;
    }
    SECTION("Wrong module") {
        module = "sdk";
    }
    SECTION("Wrong title") {
        title = OtherTitleId;
    }
    SECTION("Incomplete header") {
        nso.resize(sizeof(Loader::NSOHeader) - 1);
    }
    SECTION("Incomplete call") {
        nso.resize(DynamicCallOffset + 3);
    }
    SECTION("Incomplete routine") {
        nso.resize(DynamicRoutineOffset + DynamicRoutineSize - 1);
    }
    SECTION("Incomplete timer state") {
        nso.resize(DynamicStateOffset + 15);
    }
    SECTION("Timer state outside BSS") {
        Loader::NSOHeader header;
        std::memcpy(&header, nso.data(), sizeof(header));
        header.segments[2].bss_size = 15;
        std::memcpy(nso.data(), &header, sizeof(header));
    }
    SECTION("Wrong original hook") {
        nso[DynamicCallOffset] ^= 1;
    }
    SECTION("Nonzero code cave") {
        nso[DynamicRoutineOffset + DynamicRoutineSize - 1] = 1;
    }
    SECTION("Nonzero timer state") {
        nso[DynamicStateOffset + 15] = 1;
    }
    SECTION("Code cave outside executable padding") {
        Loader::NSOHeader header;
        std::memcpy(&header, nso.data(), sizeof(header));
        header.segments[0].size = 0x3C46000;
        std::memcpy(nso.data(), &header, sizeof(header));
    }
    SECTION("Code cave overlaps read-only segment") {
        Loader::NSOHeader header;
        std::memcpy(&header, nso.data(), sizeof(header));
        header.segments[1].location = 0x3C46200;
        std::memcpy(nso.data(), &header, sizeof(header));
    }
    REQUIRE(context.Manager(title).PatchNSO(nso, module) == nso);
}

TEST_CASE("Built-in Dynamic FPS leaves both records alone when a user mod changes either",
          "[patch_manager]") {
    size_t offset = DynamicCallOffset;
    u32 mod_word = 0x11223344;
    SECTION("User call takes precedence") {}
    SECTION("Identical call still takes precedence for the whole patch") {
        mod_word = DynamicCallInstruction;
    }
    SECTION("User routine takes precedence") {
        offset = DynamicRoutineOffset;
    }
    SECTION("Last routine byte takes precedence") {
        offset = DynamicRoutineOffset + DynamicRoutineSize - sizeof(u32);
    }
    SECTION("User timer state takes precedence") {
        offset = DynamicStateOffset;
    }
    PatchManagerTestContext context{mod_word, offset};
    Settings::values.disabled_addons[OcarinaTitleId] = {std::string{MaxResolution}};
    const auto nso = MakeDynamicMain();
    auto expected = nso;
    std::memcpy(expected.data() + offset, &mod_word, sizeof(mod_word));
    REQUIRE(context.Manager().PatchNSO(nso, "main") == expected);
}

namespace {

constexpr std::string_view Ultrawide = "Ultrawide (built-in)";
constexpr size_t UltrawideCaveOffset = sizeof(Loader::NSOHeader) + 0x3C46400;

std::vector<u8> MakeUltrawideMain() {
    auto nso = MakeDynamicMain();
    for (const auto& hook : FileSys::OcarinaUltrawide::Hooks) {
        std::memcpy(nso.data() + sizeof(Loader::NSOHeader) + hook.offset, &hook.original,
                    sizeof(hook.original));
    }
    return nso;
}

struct AspectRatioScope {
    explicit AspectRatioScope(Settings::AspectRatio ratio)
        : previous{Settings::values.aspect_ratio.GetValue()} {
        Settings::values.aspect_ratio.SetValue(ratio);
    }

    ~AspectRatioScope() {
        Settings::values.aspect_ratio.SetValue(previous);
    }

    Settings::AspectRatio previous;
};

} // Anonymous namespace

TEST_CASE("Ultrawide generator matches craftycorvid/oot-mod's 64:27 patch", "[patch_manager]") {
    constexpr auto patch = FileSys::OcarinaUltrawide::Make(2560.0 / 1080.0);
    constexpr std::array<u32, 7> branches{0x141F01D0, 0x14EFDC96, 0x14E82AB6, 0x14E42B25,
                                          0x14E7A365, 0x14D34876, 0x1403A795};
    for (size_t i = 0; i < branches.size(); ++i) {
        u32 word;
        std::memcpy(&word, patch.branches[i].data(), sizeof(word));
        REQUIRE(word == branches[i]);
    }
    REQUIRE(Common::HexToString(patch.cave) ==
            "0084552D30C7915270FCA7721002271E3038301E10C2201E50E19A527084A7721102271E"
            "0022311EA5000054D0849652F002A8720102271E01B000BD22FEE017A08E552D29C79152"
            "69FCA7722401271E6438241E84C0201E49E19A526984A7722501271E8020251EA5000054"
            "C9849652E902A8722301271EA3B200BD5C231017008C552D29C7915269FCA7722401271E"
            "6438241E84C0201E49E19A526984A7722501271E8020251EA5000054C9849652E902A872"
            "2301271E03B000BD3CD517174084552D29C7915269FCA7722201271E2238221E42C0201E"
            "49E19A526984A7722301271E4020231EA5000054C9849652E902A8722101271E41B000BD"
            "CDD41B17A102221E2118221E30C7915270FCA7721002271E3038301E10C2201E50E19A52"
            "7084A7721102271E0022311E85000054D0849652F002A8720102271E8E5C1817411040AD"
            "3000261E311191521141A7721F02116BA1020054903C0C0E107A0012D13A9752514EA772"
            "1F02116BE101005411E006D1300240F9317D00F0316202911F0211EB20010054F17C00D0"
            "31C23E911F0211EBA00000541000805210E8A772110E044E21DC316E71B72C17B0A300D1"
            "100A40BD111240BD323A301E1100805211C0A7723302271E520A331E102A321E313A321E"
            "100A00BD111200BD69A241A95F58FC17");
}

TEST_CASE("Built-in Ultrawide defaults off and follows the 21:9 Aspect Ratio", "[patch_manager]") {
    PatchManagerTestContext context;
    AspectRatioScope aspect{Settings::AspectRatio::R21_9};
    const auto manager = context.Manager();
    const auto enabled = [&] {
        const auto patches = manager.GetPatches();
        const auto it = std::ranges::find(patches, Ultrawide, &FileSys::Patch::name);
        REQUIRE(it != patches.end());
        REQUIRE(it->type == FileSys::PatchType::Mod);
        return it->enabled;
    };
    const auto nso = MakeUltrawideMain();
    const auto& first_hook = FileSys::OcarinaUltrawide::Hooks[0];
    const size_t hook_offset = sizeof(Loader::NSOHeader) + first_hook.offset;

    REQUIRE_FALSE(enabled());
    REQUIRE(WordAt(manager.PatchNSO(nso, "main"), hook_offset) == first_hook.original);

    Settings::values.disabled_addons[OcarinaTitleId] = {std::string{Ultrawide}};
    REQUIRE(enabled());
    auto patched = manager.PatchNSO(nso, "main");
    constexpr auto expected = FileSys::OcarinaUltrawide::Make(21.0 / 9.0);
    for (size_t i = 0; i < FileSys::OcarinaUltrawide::Hooks.size(); ++i) {
        const size_t offset =
            sizeof(Loader::NSOHeader) + FileSys::OcarinaUltrawide::Hooks[i].offset;
        REQUIRE(std::equal(expected.branches[i].begin(), expected.branches[i].end(),
                           patched.begin() + offset));
    }
    REQUIRE(std::equal(expected.cave.begin(), expected.cave.end(),
                       patched.begin() + UltrawideCaveOffset));
    REQUIRE(WordAt(patched, DynamicCallOffset) == DynamicCallInstruction);
    REQUIRE(WordAt(patched) == BranchInstruction);

    Settings::values.aspect_ratio.SetValue(Settings::AspectRatio::R16_9);
    REQUIRE(enabled());
    patched = manager.PatchNSO(nso, "main");
    REQUIRE(WordAt(patched, hook_offset) == first_hook.original);
    REQUIRE(WordAt(patched, UltrawideCaveOffset) == 0);

    const auto unrelated = context.Manager(OtherTitleId).GetPatches();
    REQUIRE(std::ranges::find(unrelated, Ultrawide, &FileSys::Patch::name) == unrelated.end());
}

TEST_CASE("Default-off built-ins flip between stored and disabled Add-On lists", "[patch_manager]") {
    std::vector<std::string> stored{std::string{MaxResolution}};
    FileSys::FlipDefaultOffAddons(OcarinaTitleId, stored);
    REQUIRE(stored == std::vector<std::string>{std::string{MaxResolution}, std::string{Ultrawide}});
    FileSys::FlipDefaultOffAddons(OcarinaTitleId, stored);
    REQUIRE(stored == std::vector<std::string>{std::string{MaxResolution}});

    std::vector<std::string> other{std::string{Ultrawide}};
    FileSys::FlipDefaultOffAddons(OtherTitleId, other);
    REQUIRE(other == std::vector<std::string>{std::string{Ultrawide}});
}
