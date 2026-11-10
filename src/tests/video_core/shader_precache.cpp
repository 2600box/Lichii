// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_set>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/exception.h"
#include "video_core/pipeline_cache_files.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/shader_cache_v2/cache.h"
#include "video_core/shader_cache_v2/cache_file.h"
#include "video_core/shader_cache_v2/cache_format.h"
#include "video_core/shader_environment.h"
#include "video_core/shader_warm/shader_archives.h"
#include "video_core/shader_warm/shader_warm_builder.h"
#include "video_core/shader_warm/warm_records.h"

namespace {

namespace ShaderWarm = VideoCommon::ShaderWarm;
using Settings::PrecacheShaders;

struct TempDir {
    std::filesystem::path path;
    TempDir() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path() /
               ("eden-shader-precache-" + std::to_string(stamp));
        std::filesystem::create_directories(path);
    }
    ~TempDir() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

void WriteBytes(const std::filesystem::path& file, std::string_view bytes) {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

/// A graphics pipeline record of one vertex (0) or fragment (4) stage, as the Vulkan disk cache
/// and the graphics records of the v2 log hold it: env count, env, key.
std::vector<u8> StageRecord(u32 stage, const ShaderWarm::GraphicsKey& state, u64 stage_hash,
                            u64 attribute_types) {
    ShaderWarm::LoweredCode lowered;
    lowered.code.resize(16 * sizeof(u64));
    for (size_t i = 0; i < lowered.code.size(); ++i) {
        lowered.code[i] = static_cast<u8>(i * 5 + stage_hash);
    }
    const std::vector<u8> program(128 + 16 * 16);
    std::vector<u8> record(sizeof(u32));
    const u32 env_count = 1;
    std::memcpy(record.data(), &env_count, sizeof(env_count));
    const auto env = ShaderWarm::GraphicsEnv(stage, lowered, program);
    record.insert(record.end(), env.begin(), env.end());
    const auto key = ShaderWarm::GraphicsEntryKey(stage, state, stage_hash, attribute_types);
    record.insert(record.end(), key.begin(), key.end());
    return record;
}

} // Anonymous namespace

TEST_CASE("ShaderPrecache: the setting decides whether the boot precaches", "[video_core]") {
    STATIC_REQUIRE_FALSE(ShaderWarm::PrecacheBeforeStart(PrecacheShaders::Off, false));
    STATIC_REQUIRE_FALSE(ShaderWarm::PrecacheBeforeStart(PrecacheShaders::Off, true));
    STATIC_REQUIRE_FALSE(ShaderWarm::PrecacheBeforeStart(PrecacheShaders::Ask, false));
    STATIC_REQUIRE(ShaderWarm::PrecacheBeforeStart(PrecacheShaders::Ask, true));
    STATIC_REQUIRE(ShaderWarm::PrecacheBeforeStart(PrecacheShaders::Always, false));
    STATIC_REQUIRE(ShaderWarm::PrecacheBeforeStart(PrecacheShaders::Always, true));
}

TEST_CASE("ShaderPrecache: launch requests are taken once", "[video_core]") {
    ShaderWarm::RequestWarmBeforeStart(true);
    REQUIRE(ShaderWarm::TakeWarmBeforeStart());
    REQUIRE_FALSE(ShaderWarm::TakeWarmBeforeStart());

    ShaderWarm::RequestSkipWarm();
    REQUIRE(ShaderWarm::TakeSkipWarm());
    REQUIRE_FALSE(ShaderWarm::TakeSkipWarm());

    // A skip pressed during an earlier launch does not skip the next one.
    ShaderWarm::RequestSkipWarm();
    ShaderWarm::RequestWarmBeforeStart(true);
    REQUIRE_FALSE(ShaderWarm::TakeSkipWarm());
    REQUIRE(ShaderWarm::TakeWarmBeforeStart());
}

TEST_CASE("ShaderPrecache: the marker matches only the same file and driver", "[video_core]") {
    TempDir dir;
    const auto warm = dir.path / "warm-compute.bin";
    REQUIRE_FALSE(ShaderWarm::CurrentWarmStamp(warm, 0, 1, 2, 3).has_value());
    REQUIRE_FALSE(ShaderWarm::ReadWarmMarker(warm).has_value());

    WriteBytes(warm, "precache");
    const auto stamp = ShaderWarm::CurrentWarmStamp(warm, 12, 0x1234, 0x2206, 53);
    REQUIRE(stamp.has_value());
    REQUIRE(stamp->size == 8);
    REQUIRE(ShaderWarm::WriteWarmMarker(warm, *stamp));
    REQUIRE(ShaderWarm::WarmMarkerPath(warm) == dir.path / "warm-compute.done");
    // The retry record has a marker of its own, next to the real one.
    REQUIRE(ShaderWarm::WarmMarkerPath(ShaderWarm::WarmRetryPath(warm)) ==
            dir.path / "warm-compute-retry.done");

    const auto marker = ShaderWarm::ReadWarmMarker(warm);
    REQUIRE(marker.has_value());
    REQUIRE(marker->entries == 12);
    // The entry count is informative: a stamp taken before reading the file still matches.
    REQUIRE(marker->Matches(*ShaderWarm::CurrentWarmStamp(warm, 0, 0x1234, 0x2206, 53)));
    REQUIRE_FALSE(marker->Matches(*ShaderWarm::CurrentWarmStamp(warm, 12, 0x1235, 0x2206, 53)));
    REQUIRE_FALSE(marker->Matches(*ShaderWarm::CurrentWarmStamp(warm, 12, 0x1234, 0x2207, 53)));
    REQUIRE_FALSE(marker->Matches(*ShaderWarm::CurrentWarmStamp(warm, 12, 0x1234, 0x2206, 54)));

    WriteBytes(warm, "a rebuilt precache");
    REQUIRE_FALSE(marker->Matches(*ShaderWarm::CurrentWarmStamp(warm, 12, 0x1234, 0x2206, 53)));
}

TEST_CASE("ShaderPrecache: the marker matches only the same variant", "[video_core]") {
    TempDir dir;
    const auto warm = dir.path / "warm-graphics.bin";
    WriteBytes(warm, "precache");
    const u64 variant = 0xfedcba9876543210ULL;
    const auto stamp = ShaderWarm::CurrentWarmStamp(warm, 12, 0x1234, 0x2206, 53, 3, 1, variant);
    REQUIRE(stamp.has_value());
    REQUIRE(ShaderWarm::WriteWarmMarker(warm, *stamp));

    const auto marker = ShaderWarm::ReadWarmMarker(warm);
    REQUIRE(marker.has_value());
    REQUIRE(marker->variant == variant);
    REQUIRE(marker->Matches(*ShaderWarm::CurrentWarmStamp(warm, 0, 0x1234, 0x2206, 53, 3, 1,
                                                          variant)));
    REQUIRE_FALSE(marker->Matches(*ShaderWarm::CurrentWarmStamp(warm, 12, 0x1234, 0x2206, 53, 3, 1,
                                                                variant + 1)));
    REQUIRE_FALSE(
        marker->Matches(*ShaderWarm::CurrentWarmStamp(warm, 12, 0x1234, 0x2206, 53, 3, 1)));
    REQUIRE(ShaderWarm::EmissionStamp() != 0);
}

TEST_CASE("ShaderPrecache: a marker written before the variant is not read", "[video_core]") {
    TempDir dir;
    const auto warm = dir.path / "warm-compute.bin";
    WriteBytes(warm, "precache");
    const auto stamp = ShaderWarm::CurrentWarmStamp(warm, 12, 0x1234, 0x2206, 53, 1, 0);
    REQUIRE(stamp.has_value());
    // The older format: every field up to the link mode, then the line end.
    {
        std::ofstream out(ShaderWarm::WarmMarkerPath(warm), std::ios::trunc);
        out << stamp->size << ' ' << stamp->write_time << ' ' << stamp->entries << ' '
            << stamp->driver << ' ' << stamp->device_id << ' ' << stamp->cache_version << ' '
            << stamp->no_inline << ' ' << stamp->link_mode << '\n';
    }
    REQUIRE_FALSE(ShaderWarm::ReadWarmMarker(warm).has_value());
}

TEST_CASE("ShaderPrecache: the marker records the shader calls mode", "[video_core]") {
    TempDir dir;
    const auto warm = dir.path / "warm-compute.bin";
    WriteBytes(warm, "precache");

    const auto stamp = ShaderWarm::CurrentWarmStamp(warm, 12, 0x1234, 0x2206, 53, 1);
    REQUIRE(stamp.has_value());
    REQUIRE(ShaderWarm::WriteWarmMarker(warm, *stamp));
    const auto marker = ShaderWarm::ReadWarmMarker(warm);
    REQUIRE(marker.has_value());
    REQUIRE(marker->no_inline == 1);
    REQUIRE(marker->Matches(*ShaderWarm::CurrentWarmStamp(warm, 12, 0x1234, 0x2206, 53, 1)));
    REQUIRE_FALSE(marker->Matches(*ShaderWarm::CurrentWarmStamp(warm, 12, 0x1234, 0x2206, 53, 0)));
    REQUIRE_FALSE(marker->Matches(*ShaderWarm::CurrentWarmStamp(warm, 12, 0x1234, 0x2206, 53, 2)));

    // A marker from before the mode was recorded (six fields) no longer reads, so the precache
    // runs once more.
    WriteBytes(ShaderWarm::WarmMarkerPath(warm), "8 1 12 4660 8710 53\n");
    REQUIRE_FALSE(ShaderWarm::ReadWarmMarker(warm).has_value());
}

TEST_CASE("ShaderPrecache: precached pipelines are kept per driver", "[video_core]") {
    TempDir dir;
    const auto file = dir.path / "precache-graphics.bin";
    const ShaderWarm::DriverStamp stamp{0x1234, 0x2206, 53};
    REQUIRE(ShaderWarm::ReadPrecachedPipelines(file, stamp).empty());

    const std::unordered_set<u64> hashes{1, 0xfedcba9876543210ULL, 42};
    REQUIRE(ShaderWarm::WritePrecachedPipelines(file, stamp, hashes));
    REQUIRE(ShaderWarm::ReadPrecachedPipelines(file, stamp) == hashes);
    REQUIRE(ShaderWarm::ReadPrecachedPipelines(file, {0x1235, 0x2206, 53}).empty());
    REQUIRE(ShaderWarm::ReadPrecachedPipelines(file, {0x1234, 0x2207, 53}).empty());
    REQUIRE(ShaderWarm::ReadPrecachedPipelines(file, {0x1234, 0x2206, 54}).empty());
    // A change of the graphics shader calls mode precaches them again.
    REQUIRE(ShaderWarm::ReadPrecachedPipelines(file, {0x1234, 0x2206, 53, 1}).empty());

    // A truncated file keeps the hashes it holds in full.
    std::filesystem::resize_file(file, std::filesystem::file_size(file) - 4);
    REQUIRE(ShaderWarm::ReadPrecachedPipelines(file, stamp).size() == 2);
}

TEST_CASE("ShaderPrecache: the entry count belongs to a file of one size", "[video_core]") {
    TempDir dir;
    const auto warm = dir.path / "warm-compute.bin";
    WriteBytes(warm, "0123456789");
    REQUIRE(ShaderWarm::ReadWarmCount(warm) == 0);

    WriteBytes(ShaderWarm::WarmCountPath(warm), "345 10\n");
    REQUIRE(ShaderWarm::ReadWarmCount(warm) == 345);

    WriteBytes(warm, "01234567890");
    REQUIRE(ShaderWarm::ReadWarmCount(warm) == 0);
}

TEST_CASE("ShaderPrecache: a compute entry reads back like a pipeline cache entry",
          "[video_core]") {
    ShaderWarm::LoweredCode lowered;
    lowered.code.resize(16 * sizeof(u64));
    for (size_t i = 0; i < lowered.code.size(); ++i) {
        lowered.code[i] = static_cast<u8>(i * 7 + 1);
    }
    lowered.texture_buffer = 3;
    const ShaderWarm::ComputeControl control{{64, 2, 1}, 1024, 256};
    ShaderWarm::ComputeKey key_bytes{};
    const auto entry = ShaderWarm::ComputeEntry(lowered, control, {}, key_bytes);

    TempDir dir;
    const auto file = dir.path / "entry.bin";
    WriteBytes(file, std::string_view{reinterpret_cast<const char*>(entry.data()), entry.size()});

    std::ifstream in(file, std::ios::binary);
    in.exceptions(std::ifstream::failbit);
    u32 num_envs{};
    in.read(reinterpret_cast<char*>(&num_envs), sizeof(num_envs));
    REQUIRE(num_envs == 1);
    VideoCommon::FileEnvironment env;
    env.Deserialize(in, static_cast<std::streamoff>(entry.size()));
    Vulkan::ComputePipelineCacheKey key{};
    static_assert(sizeof(key) == sizeof(key_bytes));
    in.read(reinterpret_cast<char*>(&key), sizeof(key));
    REQUIRE(in.tellg() == static_cast<std::streamoff>(entry.size()));

    REQUIRE(env.ShaderStage() == Shader::Stage::Compute);
    REQUIRE(env.WorkgroupSize() == control.workgroup);
    REQUIRE(env.SharedMemorySize() == control.shared_memory);
    REQUIRE(env.LocalMemorySize() == control.local_memory);
    REQUIRE(env.TextureBoundBuffer() == 3);
    REQUIRE(std::memcmp(&key, key_bytes.data(), sizeof(key)) == 0);
    REQUIRE(key.unique_hash == ShaderWarm::StageHash(lowered.code));
    REQUIRE(key.shared_memory_size == control.shared_memory);
    REQUIRE(key.workgroup_size == control.workgroup);

    // The entry has no recorded reads: without guesses they are errors, with them the guesses.
    REQUIRE_THROWS_AS(env.ReadCbufValue(0, 0x40), Shader::LogicError);
    static const VideoCommon::GuessMissing guess{};
    env.SetGuessMissing(&guess);
    REQUIRE(env.ReadCbufValue(0, 0x40) == guess.cbuf_bank0);
    REQUIRE(env.ReadCbufValue(3, 0x40) == guess.cbuf_other);
    REQUIRE(env.ReadTextureType(5) == guess.texture_type);
    REQUIRE(env.IsCbufStorageBufferPointer(0, 0x10) == guess.storage_pointer_bank0);
    REQUIRE(env.IsCbufStorageBufferPointer(1, 0x10) == guess.storage_pointer_other);
}

TEST_CASE("ShaderPrecache: a program naming no texture bank reads bank 0", "[video_core]") {
    ShaderWarm::LoweredCode lowered;
    lowered.code.resize(16 * sizeof(u64));
    ShaderWarm::ComputeKey key{};
    const auto entry = ShaderWarm::ComputeEntry(lowered, {{1, 1, 1}, 0, 0}, {}, key);
    u32 texture_bound{};
    // num_envs, then six u64 counts and local memory before the texture bank.
    std::memcpy(&texture_bound, entry.data() + 4 + 6 * 8 + 4, sizeof(texture_bound));
    REQUIRE(texture_bound == 0);
}

TEST_CASE("ShaderPrecache: every container the archive walker reads is discovered",
          "[video_core]") {
    using ShaderWarm::ClassifyGameFile;
    using ShaderWarm::GameFileKind;
    for (const char* path : {"Shader/Model.product.nin_ounce_nvn2.bfsha.zs",
                             "Shader/Model.bfsha", "Pack/Shaders.nin_ounce_nvn2.release.sarc.zs",
                             "Pack/Shaders.release.sarc", "Shader/Loose.bnsh",
                             "Shader/Loose.bnsh.zs", "Shader/Bundle.cgsb", "Shader/Bundle.cgsb.zs",
                             "Shader/Program.shn", "Shader/Program.shn.zs", "Shader/Archive.sharcb",
                             "Shader/Archive.sharcb.zs", "Shader/UPPER.BNSH", "Effect/Fx.bfsha.zs"}) {
        INFO(path);
        REQUIRE(ClassifyGameFile(path) == GameFileKind::ShaderArchive);
    }
    REQUIRE(ClassifyGameFile("Effect/Fx.nin_ounce_nvn2.vsetb.byml.zs") == GameFileKind::Effect);
    REQUIRE(ClassifyGameFile("Pack/ZsDic.pack.zs") == GameFileKind::ZstdDictionaries);
    for (const char* path :
         {"Effect/Sub/Fx.vsetb.byml.zs", "Model/Fx.vsetb.byml.zs", "Effect/Fx.vsetb.byml",
          "Model/Tex.bntx.zs", "Model/Mesh.bfres.zs", "Asset/assets_a000.dat",
          "Bin/Nvn2-upper_left.ndsr", "Shader/notes.shn.txt", "Shader/bnsh"}) {
        INFO(path);
        REQUIRE(ClassifyGameFile(path) == GameFileKind::Other);
    }
}

TEST_CASE("ShaderPrecache: programs that cannot be lowered are refused", "[video_core]") {
    ShaderWarm::LoweredCode lowered;
    REQUIRE(ShaderWarm::LowerProgram({}, lowered).has_value());
    const std::array<u8, 15> partial{};
    REQUIRE(ShaderWarm::LowerProgram(partial, lowered).has_value());
    // One instruction with no exit: not a complete program.
    const std::array<u8, 16> no_exit{};
    REQUIRE(ShaderWarm::LowerProgram(no_exit, lowered).has_value());
    REQUIRE(lowered.code.empty());
}

TEST_CASE("ShaderPrecache: graphics entries made for a dynamic state the device lacks are found",
          "[video_core]") {
    namespace KeyDynamicState = Vulkan::KeyDynamicState;
    constexpr u32 all = KeyDynamicState::VertexInput | KeyDynamicState::Extended |
                        KeyDynamicState::Extended2 | KeyDynamicState::Extended2LogicOp |
                        KeyDynamicState::Extended3Blend | KeyDynamicState::Extended3Enables |
                        KeyDynamicState::ColorWriteEnable;
    Vulkan::DynamicFeatures features{};
    REQUIRE(Vulkan::MissingKeyDynamicStates(features) == all);
    features.has_dynamic_vertex_input = true;
    features.has_extended_dynamic_state = true;
    features.has_extended_dynamic_state_2 = true;
    features.has_extended_dynamic_state_2_logic_op = true;
    features.has_extended_dynamic_state_3_blend = true;
    features.has_extended_dynamic_state_3_enables = true;
    features.has_color_write_enable = true;
    REQUIRE(Vulkan::MissingKeyDynamicStates(features) == 0);

    Vulkan::FixedPipelineState state;
    std::memset(&state, 0, sizeof(state));
    REQUIRE(Vulkan::KeyDynamicStates(state) == 0);
    state.extended_dynamic_state.Assign(1);
    state.extended_dynamic_state_2.Assign(1);
    REQUIRE(Vulkan::KeyDynamicStates(state) ==
            (KeyDynamicState::Extended | KeyDynamicState::Extended2));
    state.dynamic_vertex_input.Assign(1);
    state.extended_dynamic_state_2_logic_op.Assign(1);
    state.extended_dynamic_state_3_blend.Assign(1);
    state.extended_dynamic_state_3_enables.Assign(1);
    state.color_write_enable_dynamic.Assign(1);
    REQUIRE(Vulkan::KeyDynamicStates(state) == all);

    // A device without EDS3 blending or color write enable: keys naming either are left out.
    features.has_extended_dynamic_state_3_blend = false;
    features.has_color_write_enable = false;
    const u32 missing = Vulkan::MissingKeyDynamicStates(features);
    REQUIRE(missing == (KeyDynamicState::Extended3Blend | KeyDynamicState::ColorWriteEnable));
    REQUIRE((Vulkan::KeyDynamicStates(state) & missing) != 0);
    state.extended_dynamic_state_3_blend.Assign(0);
    REQUIRE((Vulkan::KeyDynamicStates(state) & missing) != 0);
    state.color_write_enable_dynamic.Assign(0);
    REQUIRE((Vulkan::KeyDynamicStates(state) & missing) == 0);

    // Dynamic vertex input keeps the lowest bit: graphics marker bit 2 from earlier markers.
    REQUIRE(KeyDynamicState::VertexInput == 1);
}

TEST_CASE("ShaderPrecache: graphics entries are built without color write enable or the EDS2 "
          "logic op when the device lacks them",
          "[video_core]") {
    namespace KeyDynamicState = Vulkan::KeyDynamicState;
    // The built-in draw state defaults: EDS1, EDS2, the EDS2 logic op, dynamic vertex input and
    // color write enable.
    Vulkan::FixedPipelineState state;
    std::memset(&state, 0, sizeof(state));
    state.extended_dynamic_state.Assign(1);
    state.extended_dynamic_state_2.Assign(1);
    state.extended_dynamic_state_2_logic_op.Assign(1);
    state.dynamic_vertex_input.Assign(1);
    state.color_write_enable_dynamic.Assign(1);
    const u32 made_with = Vulkan::KeyDynamicStates(state);

    // A device with all of them: nothing changes.
    REQUIRE(Vulkan::AdaptKeyDynamicStates(state, 0) == 0);
    REQUIRE(Vulkan::KeyDynamicStates(state) == made_with);

    // A device without color write enable and the EDS2 logic op (AMD on Windows): both cleared,
    // the entry no longer names a state the device lacks.
    Vulkan::DynamicFeatures features{};
    features.has_dynamic_vertex_input = true;
    features.has_extended_dynamic_state = true;
    features.has_extended_dynamic_state_2 = true;
    features.has_extended_dynamic_state_3_blend = true;
    features.has_extended_dynamic_state_3_enables = true;
    const u32 missing = Vulkan::MissingKeyDynamicStates(features);
    REQUIRE(missing == (KeyDynamicState::Extended2LogicOp | KeyDynamicState::ColorWriteEnable));
    REQUIRE(Vulkan::AdaptKeyDynamicStates(state, missing) == missing);
    REQUIRE(state.extended_dynamic_state_2_logic_op.Value() == 0U);
    REQUIRE(state.color_write_enable_dynamic.Value() == 0U);
    REQUIRE((Vulkan::KeyDynamicStates(state) & missing) == 0);
    REQUIRE(Vulkan::KeyDynamicStates(state) == (made_with & ~missing));

    // The states whose static fields the key leaves unfilled stay: those entries are left out.
    Vulkan::FixedPipelineState other;
    std::memset(&other, 0, sizeof(other));
    other.extended_dynamic_state.Assign(1);
    other.extended_dynamic_state_2.Assign(1);
    other.extended_dynamic_state_3_blend.Assign(1);
    other.extended_dynamic_state_3_enables.Assign(1);
    other.dynamic_vertex_input.Assign(1);
    constexpr u32 kept = KeyDynamicState::VertexInput | KeyDynamicState::Extended |
                         KeyDynamicState::Extended2 | KeyDynamicState::Extended3Blend |
                         KeyDynamicState::Extended3Enables;
    REQUIRE(Vulkan::AdaptKeyDynamicStates(other, kept) == 0);
    REQUIRE(Vulkan::KeyDynamicStates(other) == kept);
    REQUIRE((kept & Vulkan::AdaptableKeyDynamicStates) == 0);
}

TEST_CASE("ShaderPrecache: draw state guesses come from the v2 log, else the older cache",
          "[video_core]") {
    namespace V2 = VideoCommon::ShaderCacheV2;
    // Two draw states with the same render target (one color target, format 0xE0)
    ShaderWarm::GraphicsKey state_a{};
    ShaderWarm::GraphicsKey state_b{};
    state_a[56] = state_b[56] = 0xE0;
    state_a[200] = 1;
    state_b[200] = 2;
    const std::vector<std::vector<u8>> live{
        StageRecord(0, state_a, 0x11, 0x5),
        StageRecord(0, state_a, 0x12, 0x6),
        StageRecord(0, state_b, 0x13, 0x7),
        StageRecord(4, state_b, 0x21, 0),
    };

    // The v2 log: those records, plus two damaged ones and a removed one of state B (which would
    // make it the most common vertex state) and a compute record
    std::vector<u8> records;
    u64 record_key = 0x100;
    for (const auto& payload : live) {
        V2::EncodeRecord(records, V2::RecordKind::Graphics, record_key++, payload);
    }
    for (const u64 stage_hash : {0x14, 0x15}) {
        const size_t offset = records.size();
        V2::EncodeRecord(records, V2::RecordKind::Graphics, record_key++,
                         StageRecord(0, state_b, stage_hash, 0x8));
        records[offset + V2::RecordHeaderSize + 40] ^= 0xFF;
    }
    V2::EncodeRecord(records, V2::RecordKind::Graphics, 0x200, StageRecord(0, state_b, 0x16, 0x9));
    V2::EncodeRecord(records, V2::RecordKind::Tombstone, 0x200,
                     V2::TombstonePayload(V2::RecordKind::Graphics));
    V2::EncodeRecord(records, V2::RecordKind::Compute, 0x300, std::vector<u8>(64, 0x5A));

    TempDir dir;
    const auto log = dir.path / V2::FileName;
    const auto old_cache =
        VideoCommon::VersionedPipelineCachePath(dir.path, "vulkan", ShaderWarm::WarmCacheVersion);
    REQUIRE(ShaderWarm::FindRuntimeCaches(dir.path).empty());

    // The older disk cache alone: the same records one after another
    std::vector<u8> old_bytes{'y', 'u', 'z', 'u', 'c', 'a', 'c', 'h'};
    const u32 version = ShaderWarm::WarmCacheVersion;
    old_bytes.resize(12);
    std::memcpy(old_bytes.data() + 8, &version, sizeof(version));
    for (const auto& payload : live) {
        old_bytes.insert(old_bytes.end(), payload.begin(), payload.end());
    }
    WriteBytes(old_cache,
               std::string_view{reinterpret_cast<const char*>(old_bytes.data()), old_bytes.size()});
    REQUIRE(ShaderWarm::FindRuntimeCaches(dir.path) == std::vector{old_cache});

    REQUIRE(V2::CreateLog(log, records) == V2::WriteResult::Written);
    REQUIRE(ShaderWarm::FindRuntimeCaches(dir.path) == std::vector{log});

    const auto expected_vertex = ShaderWarm::GraphicsEntryKey(0, state_a, 0, 0x5);
    for (const auto& file : {log, old_cache}) {
        const ShaderWarm::RuntimeStats stats = ShaderWarm::ReadRuntimeStats({file});
        REQUIRE(stats.occurrences == 4);
        REQUIRE(stats.templates[0].has_value());
        REQUIRE(*stats.templates[0] == expected_vertex);
        REQUIRE(stats.templates[1].has_value());
        REQUIRE(*stats.templates[1] == state_b);
        REQUIRE(stats.vertex_attributes.size() == 3);
        REQUIRE(stats.vertex_attributes.at(0x11) == std::vector<u64>{0x5});
        REQUIRE(stats.vertex_attributes.at(0x13) == std::vector<u64>{0x7});
    }
}
