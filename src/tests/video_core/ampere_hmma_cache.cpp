// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/frontend/ampere/hmma_program.h"
#include "video_core/shader_environment.h"

namespace {

using namespace Shader::Ampere;
constexpr std::array<u64, 10> Header{};
constexpr std::array<u32, 3> Workgroup{128, 1, 1};
constexpr Instruction Hmma{{0x0830723c, 12, 0x810, 0x041ff000}};
constexpr Instruction Exit{{0x0000794d, 0, 0x03800000, 0}};

HmmaProgram Program() {
    return BuildHmmaProgram(std::array{Hmma, Hmma, Hmma, Exit}, Header, 0,
                            Shader::Stage::Compute, Workgroup, true);
}

std::string Serialize(const HmmaProgram& program) {
    std::ostringstream stream(std::ios::binary);
    WriteHmmaProgram(stream, &program);
    return stream.str();
}

std::optional<HmmaProgram> Deserialize(const std::string& bytes, std::span<const u64> code,
                                      bool enabled = true) {
    std::istringstream stream(bytes, std::ios::binary);
    return ReadHmmaProgram(stream, code, Shader::Stage::Compute, Workgroup, enabled);
}

void Put32(std::string& bytes, size_t offset, u32 value) {
    for (size_t i = 0; i < 4; ++i) {
        bytes.at(offset + i) = static_cast<char>(value >> (8 * i));
    }
}

class ExperimentalOptIn {
public:
    ExperimentalOptIn() {
        if (const char* value = std::getenv("EDEN_NX2_HMMA_EXPERIMENTAL")) {
            previous = value;
        }
        Set("1");
    }

    ~ExperimentalOptIn() {
#ifdef _WIN32
        _putenv_s("EDEN_NX2_HMMA_EXPERIMENTAL", previous ? previous->c_str() : "");
#else
        if (previous) {
            setenv("EDEN_NX2_HMMA_EXPERIMENTAL", previous->c_str(), 1);
        } else {
            unsetenv("EDEN_NX2_HMMA_EXPERIMENTAL");
        }
#endif
    }

    void Set(const char* value) {
#ifdef _WIN32
        REQUIRE(_putenv_s("EDEN_NX2_HMMA_EXPERIMENTAL", value) == 0);
#else
        REQUIRE(setenv("EDEN_NX2_HMMA_EXPERIMENTAL", value, 1) == 0);
#endif
    }

private:
    std::optional<std::string> previous;
};

class LiveEnvironment final : public VideoCommon::GenericEnvironment {
public:
    explicit LiveEnvironment(const HmmaProgram& program) {
        stage = Shader::Stage::Compute;
        start_address = initial_offset = 0x50;
        is_proprietary_driver = true;
        workgroup_size = Workgroup;
        auto lowered = std::make_shared<VideoCommon::LoweredNativeCode>();
        lowered->code = program.code;
        lowered->source_size = program.source_size;
        lowered->texture_bound = program.texture_bound;
        lowered->hmma_program = std::make_shared<HmmaProgram>(program);
        InitializeNativeCode(std::move(lowered), 0);
    }

    void DropMetadata() {
        auto lowered = std::make_shared<VideoCommon::LoweredNativeCode>(*lowered_native);
        lowered->hmma_program.reset();
        lowered_native = std::move(lowered);
    }

    void ExceedResourceCount() {
        for (u32 i = 0; i <= HmmaMetadataLimit; ++i) {
            cbuf_values.emplace(i, i);
        }
    }

    void ExceedSiteCount() {
        auto program = std::make_shared<HmmaProgram>(*lowered_native->hmma_program);
        program->sites.resize(HmmaMetadataLimit + 1);
        auto lowered = std::make_shared<VideoCommon::LoweredNativeCode>(*lowered_native);
        lowered->hmma_program = std::move(program);
        lowered_native = std::move(lowered);
    }

    u32 ReadCbufValue(u32, u32) override { throw Shader::LogicError("Unexpected cbuf read"); }
    Shader::TextureType ReadTextureType(u32) override {
        throw Shader::LogicError("Unexpected texture read");
    }
    Shader::TexturePixelFormat ReadTexturePixelFormat(u32) override {
        throw Shader::LogicError("Unexpected texture format read");
    }
    bool IsTexturePixelFormatInteger(u32) override {
        throw Shader::LogicError("Unexpected texture format query");
    }
    u32 ReadViewportTransformState() override { return 1; }
    std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32, u32) override {
        return std::nullopt;
    }
};

class EnvironmentCacheFile {
public:
    EnvironmentCacheFile() {
        directory = std::filesystem::temp_directory_path() /
                    ("eden-hmma-cache-" + std::to_string(std::random_device{}()));
        REQUIRE(std::filesystem::create_directory(directory));
        path = directory / "environment.bin";
    }

    ~EnvironmentCacheFile() {
        std::error_code error;
        std::filesystem::remove_all(directory, error);
    }

    std::string Save(const VideoCommon::GenericEnvironment& environment) {
        {
            std::ofstream stream(path, std::ios::binary);
            stream.exceptions(std::ios::failbit | std::ios::badbit);
            environment.Serialize(stream);
        }
        std::ifstream stream(path, std::ios::binary);
        return {std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
    }

    VideoCommon::FileEnvironment Load(const std::string& bytes) {
        Write(bytes);
        std::ifstream stream(path, std::ios::binary);
        stream.exceptions(std::ios::failbit | std::ios::badbit);
        VideoCommon::FileEnvironment environment;
        environment.Deserialize(stream);
        return environment;
    }

    void Write(const std::string& bytes) {
        {
            std::ofstream stream(path, std::ios::binary);
            stream.exceptions(std::ios::failbit | std::ios::badbit);
            stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        }
    }

    const std::filesystem::path& Path() const {
        return path;
    }

    std::string Read() const {
        std::ifstream stream(path, std::ios::binary);
        return {std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
    }

private:
    std::filesystem::path directory;
    std::filesystem::path path;
};

} // namespace

TEST_CASE("HMMA cache rebuilds typed sites from complete native source", "[video_core][ampere][hmma]") {
    const auto program = Program();
    const auto loaded = Deserialize(Serialize(program), program.code);
    REQUIRE(loaded);
    REQUIRE(loaded->source_size == 4 * sizeof(Instruction));
    REQUIRE(loaded->source_size != loaded->code.size() * sizeof(u64));
    REQUIRE(loaded->code == program.code);
    REQUIRE(loaded->native_digest == program.native_digest);
    REQUIRE(loaded->sites.size() == 3);
    for (size_t index = 0; index < loaded->sites.size(); ++index) {
        const auto& site = loaded->sites[index];
        REQUIRE(site.native_offset == index * sizeof(Instruction));
        REQUIRE(NativeProgramAddress(loaded->code, site.native_offset) == site.address);
        REQUIRE(loaded->code[site.address / 8] == HmmaGuard);
        REQUIRE(FindHmmaSite(*loaded, site.address) == &site);
        REQUIRE(site.instruction.destination == 48);
        REQUIRE(site.instruction.a == 8);
        REQUIRE(site.instruction.b == 12);
        REQUIRE(site.instruction.c == 16);
    }
    REQUIRE(loaded->sites[2].address == 0x68);
    REQUIRE_THROWS_AS(ValidateHmmaFetch(*loaded, loaded->executable_end), Shader::HmmaRejected);
    REQUIRE_THROWS_AS(ValidateHmmaFetch(*loaded, 0x60), Shader::HmmaRejected);
    REQUIRE_THROWS_AS(ValidateHmmaFetch(*loaded, 0x51), Shader::HmmaRejected);
}

TEST_CASE("HMMA cache rejects mutated metadata and native bytes", "[video_core][ampere][hmma]") {
    const auto program = Program();
    const auto bytes = Serialize(program);
    for (const size_t offset : std::array<size_t, 9>{0, 4, 8, 12, 16, 20, 24, 28, 32}) {
        auto changed = bytes;
        changed[offset] ^= 1;
        REQUIRE_THROWS_AS(Deserialize(changed, program.code), Shader::HmmaRejected);
    }
    for (const u32 count : {0U, HmmaMetadataLimit + 1, ~0U}) {
        auto changed = bytes;
        Put32(changed, 24, count);
        REQUIRE_THROWS_AS(Deserialize(changed, program.code), Shader::HmmaRejected);
        changed = bytes;
        Put32(changed, 28, count);
        REQUIRE_THROWS_AS(Deserialize(changed, program.code), Shader::HmmaRejected);
    }
    const size_t first_site = 32 + program.native.size() * sizeof(Instruction);
    for (const size_t offset : {first_site, first_site + 4, first_site + 8}) {
        auto changed = bytes;
        changed[offset] ^= 8;
        REQUIRE_THROWS_AS(Deserialize(changed, program.code), Shader::HmmaRejected);
    }
    for (size_t size = 0; size < bytes.size(); ++size) {
        REQUIRE_THROWS_AS(Deserialize(bytes.substr(0, size), program.code), Shader::HmmaRejected);
    }
}

TEST_CASE("HMMA cache verifies generated code guards and address map", "[video_core][ampere][hmma]") {
    const auto program = Program();
    const auto bytes = Serialize(program);
    for (const size_t index : std::array<size_t, 4>{program.sites[0].address / 8,
                                                   program.executable_end / 8 - 1,
                                                   program.executable_end / 8 + 1,
                                                   program.code.size() - 1}) {
        auto code = program.code;
        code[index] ^= 1;
        REQUIRE_THROWS_AS(Deserialize(bytes, code), Shader::HmmaRejected);
    }
    REQUIRE_THROWS_AS(Deserialize(std::string(4, '\0'), program.code), Shader::HmmaRejected);
    REQUIRE_THROWS_AS(Deserialize(bytes, program.code, false), Shader::HmmaRejected);
    std::istringstream stream(bytes, std::ios::binary);
    REQUIRE_THROWS_AS(ReadHmmaProgram(stream, program.code, Shader::Stage::Fragment, Workgroup, true),
                      Shader::HmmaRejected);
}

TEST_CASE("HMMA assembly enforces launch and source provenance", "[video_core][ampere][hmma]") {
    const std::array native{Hmma, Exit};
    for (const auto shape : {std::array<u32, 3>{32, 1, 1}, std::array<u32, 3>{128, 2, 1},
                             std::array<u32, 3>{127, 1, 1}}) {
        REQUIRE_THROWS_AS(BuildHmmaProgram(native, Header, 0, Shader::Stage::Compute, shape, true),
                          Shader::HmmaRejected);
    }
    REQUIRE_THROWS_AS(BuildHmmaProgram(native, Header, 0, Shader::Stage::Compute, Workgroup, false),
                      Shader::HmmaRejected);
    REQUIRE_THROWS_AS(BuildHmmaProgram(native, Header, 128, Shader::Stage::Compute, Workgroup, true),
                      Shader::HmmaRejected);
    REQUIRE_THROWS_AS(BuildHmmaProgram(native, Header, 0, Shader::Stage::Fragment, Workgroup, true),
                      Shader::HmmaRejected);
    auto changed = Hmma;
    changed.words[2] = 0x8ff;
    const auto zero_c = BuildHmmaProgram(std::array{changed, Exit}, Header, 0,
                                        Shader::Stage::Compute, Workgroup, true);
    const auto normal_c = BuildHmmaProgram(native, Header, 0, Shader::Stage::Compute, Workgroup, true);
    REQUIRE(zero_c.sites[0].instruction.c == 255);
    REQUIRE(zero_c.code == normal_c.code);
    REQUIRE(zero_c.native_digest != normal_c.native_digest);
}

TEST_CASE("HMMA cache recomputes participation instead of trusting a digest",
          "[video_core][ampere][hmma]") {
    auto program = Program();
    program.native[1] = Exit;
    program.native[1].words[0] &= ~0xf000U;
    program.native_digest = NativeProgramKey(program.native);
    REQUIRE_THROWS_AS(Deserialize(Serialize(program), program.code), Shader::HmmaRejected);
}

TEST_CASE("Non-HMMA cache carries an empty metadata block", "[video_core][ampere][hmma]") {
    std::ostringstream stream(std::ios::binary);
    WriteHmmaProgram(stream, nullptr);
    REQUIRE(stream.str() == std::string(4, '\0'));
    REQUIRE_FALSE(Deserialize(stream.str(), std::array<u64, 1>{0xe30000000007000fULL}));
}

TEST_CASE("Production HMMA environment cache preserves typed fetch and source invalidation extent",
          "[video_core][ampere][hmma][environment]") {
    ExperimentalOptIn opt_in;
    const auto program = Program();
    LiveEnvironment live(program);
    EnvironmentCacheFile file;
    auto cached = file.Load(file.Save(live));
    REQUIRE(live.UsesHmma());
    REQUIRE(cached.UsesHmma());
    REQUIRE(live.SourceSizeBytes() == program.native.size() * sizeof(Instruction));
    REQUIRE(live.SourceSizeBytes() != live.CachedSizeBytes());
    REQUIRE(live.LoweredNative()->source_size == live.SourceSizeBytes());
    REQUIRE(live.CachedSizeBytes() == program.code.size() * sizeof(u64));
    REQUIRE_NOTHROW(live.SetCachedSize(live.CachedSizeBytes()));
    REQUIRE_THROWS_AS(live.SetCachedSize(live.SourceSizeBytes()), Shader::HmmaRejected);
    REQUIRE(live.Analyze() == live.CalculateHash());
    REQUIRE(cached.StartAddress() == live.StartAddress());
    REQUIRE(cached.ShaderStage() == live.ShaderStage());
    REQUIRE(cached.IsProprietaryDriver() == live.IsProprietaryDriver());
    REQUIRE(cached.WorkgroupSize() == live.WorkgroupSize());
    for (const auto& site : program.sites) {
        const auto source = std::get<Hmma16816F16>(live.FetchInstruction(site.address));
        const auto restored = std::get<Hmma16816F16>(cached.FetchInstruction(site.address));
        REQUIRE(source.destination == restored.destination);
        REQUIRE(source.a == restored.a);
        REQUIRE(source.b == restored.b);
        REQUIRE(source.c == restored.c);
        REQUIRE(live.TranslateProgramAddress(site.native_offset) == site.address);
        REQUIRE(cached.TranslateProgramAddress(site.native_offset) == site.address);
        REQUIRE_THROWS_AS(live.ReadInstruction(site.address), Shader::HmmaRejected);
        REQUIRE_THROWS_AS(cached.ReadInstruction(site.address), Shader::HmmaRejected);
    }
    const u32 exit_address = *NativeProgramAddress(program.code, 3 * sizeof(Instruction));
    REQUIRE(std::get<Shader::MaxwellInstructionWord>(live.FetchInstruction(exit_address)).value ==
            std::get<Shader::MaxwellInstructionWord>(cached.FetchInstruction(exit_address)).value);
    for (const u32 address : {program.executable_end, program.executable_end + 8, 0x60U, 0x51U}) {
        REQUIRE_THROWS_AS(live.FetchInstruction(address), Shader::HmmaRejected);
        REQUIRE_THROWS_AS(cached.FetchInstruction(address), Shader::HmmaRejected);
    }
    live.DropMetadata();
    REQUIRE_FALSE(live.UsesHmma());
    REQUIRE_THROWS_AS(live.FetchInstruction(program.sites.front().address), Shader::HmmaRejected);
}

TEST_CASE("Production HMMA cache rejects absent malformed and disabled metadata",
          "[video_core][ampere][hmma][environment]") {
    ExperimentalOptIn opt_in;
    const auto program = Program();
    LiveEnvironment live(program);
    EnvironmentCacheFile file;
    const auto bytes = file.Save(live);
    const size_t metadata_offset = bytes.size() - Serialize(program).size();
    REQUIRE_THROWS_AS(file.Load(bytes.substr(0, metadata_offset)), Shader::HmmaRejected);
    auto missing = bytes.substr(0, metadata_offset + 4);
    Put32(missing, metadata_offset, 0);
    REQUIRE_THROWS_AS(file.Load(missing), Shader::HmmaRejected);
    for (const size_t offset : std::array<size_t, 6>{0, 8, 12, 16, 24, 28}) {
        auto changed = bytes;
        Put32(changed, metadata_offset + offset, ~0U);
        REQUIRE_THROWS_AS(file.Load(changed), Shader::HmmaRejected);
    }
    const size_t first_site = metadata_offset + 32 + program.native.size() * sizeof(Instruction);
    for (const size_t offset : {first_site, first_site + 4}) {
        auto changed = bytes;
        changed[offset] ^= 8;
        REQUIRE_THROWS_AS(file.Load(changed), Shader::HmmaRejected);
    }
    auto changed = bytes;
    Put32(changed, 8, HmmaMetadataLimit + 1);
    REQUIRE_THROWS_AS(file.Load(changed), Shader::HmmaRejected);
    const size_t code_offset = metadata_offset - 4 * sizeof(u32) - program.code.size() * sizeof(u64);
    changed = bytes;
    changed[code_offset + program.sites.front().address] ^= 1;
    REQUIRE_THROWS_AS(file.Load(changed), Shader::HmmaRejected);
    for (const char* value : {"0", "true", "01", ""}) {
        opt_in.Set(value);
        REQUIRE_THROWS_AS(file.Load(bytes), Shader::HmmaRejected);
        REQUIRE_THROWS_AS(LiveEnvironment(program), Shader::HmmaRejected);
    }
}

TEST_CASE("Production HMMA source hashes distinguish identical guards with different native operands",
          "[video_core][ampere][hmma][environment]") {
    ExperimentalOptIn opt_in;
    auto instruction = Hmma;
    const auto first = BuildHmmaProgram(std::array{instruction, Exit}, Header, 0,
                                       Shader::Stage::Compute, Workgroup, true);
    instruction.words[2] = 0x8ff;
    const auto second = BuildHmmaProgram(std::array{instruction, Exit}, Header, 0,
                                        Shader::Stage::Compute, Workgroup, true);
    LiveEnvironment first_environment(first);
    LiveEnvironment second_environment(second);
    REQUIRE(first.code == second.code);
    REQUIRE(first_environment.Analyze() != second_environment.Analyze());
    REQUIRE(first_environment.CalculateHash() != second_environment.CalculateHash());
}

TEST_CASE("Pipeline cache loading contains HMMA rejection after earlier callbacks",
          "[video_core][ampere][hmma][environment]") {
    ExperimentalOptIn opt_in;
    const auto program = Program();
    LiveEnvironment live(program);
    EnvironmentCacheFile file;
    constexpr u32 version = 42;
    constexpr std::array<char, 4> key{'k', 'e', 'y', '0'};
    const std::array<const VideoCommon::GenericEnvironment*, 1> environments{&live};
    VideoCommon::SerializePipeline(std::span<const char>{key},
                                   std::span<const VideoCommon::GenericEnvironment* const>{environments},
                                   file.Path(), version);
    std::ifstream source(file.Path(), std::ios::binary);
    const std::string valid{std::istreambuf_iterator<char>{source}, std::istreambuf_iterator<char>{}};
    source.close();
    auto rejected = valid.substr(12);
    const size_t metadata_offset = rejected.size() - key.size() - Serialize(program).size();
    Put32(rejected, metadata_offset, 0);
    file.Write(valid + rejected);
    u32 callbacks = 0;
    const auto load = [&] {
        VideoCommon::LoadPipelines(
            {}, file.Path(), version,
            [&](std::ifstream& stream, VideoCommon::FileEnvironment environment) {
                std::array<char, 4> restored_key{};
                stream.read(restored_key.data(), restored_key.size());
                REQUIRE(restored_key == key);
                REQUIRE(std::get<Hmma16816F16>(environment.FetchInstruction(program.sites[0].address))
                            .destination == program.sites[0].instruction.destination);
                ++callbacks;
            },
            [](std::ifstream&, std::vector<VideoCommon::FileEnvironment>) {
                FAIL("Unexpected graphics pipeline");
            });
    };
    REQUIRE_NOTHROW(load());
    REQUIRE(callbacks == 1);
    REQUIRE(std::filesystem::exists(file.Path()));
    callbacks = 0;
    opt_in.Set("0");
    REQUIRE_NOTHROW(load());
    REQUIRE(callbacks == 0);
    REQUIRE(std::filesystem::exists(file.Path()));
}

TEST_CASE("Rejected pipeline appends preserve earlier entries and permit subsequent writes",
          "[video_core][ampere][hmma][environment]") {
    ExperimentalOptIn opt_in;
    const auto program = Program();
    LiveEnvironment live(program);
    LiveEnvironment excess_resources(program);
    excess_resources.ExceedResourceCount();
    LiveEnvironment excess_sites(program);
    excess_sites.ExceedSiteCount();
    EnvironmentCacheFile file;
    constexpr u32 version = 42;
    constexpr std::array<char, 4> key{'k', 'e', 'y', '0'};
    const auto append = [&](const std::filesystem::path& path,
                            std::span<const VideoCommon::GenericEnvironment* const> environments) {
        VideoCommon::SerializePipeline(std::span<const char>{key}, environments, path, version);
    };
    const std::array<const VideoCommon::GenericEnvironment*, 1> valid{&live};
    REQUIRE_NOTHROW(append(file.Path(), valid));
    const auto original = file.Read();
    REQUIRE_FALSE(original.empty());
    for (const LiveEnvironment* invalid : {&excess_resources, &excess_sites}) {
        const std::array<const VideoCommon::GenericEnvironment*, 2> partial{&live, invalid};
        REQUIRE_NOTHROW(append(file.Path(), partial));
        REQUIRE(file.Read() == original);
    }
    u32 callbacks = 0;
    const auto load = [&] {
        VideoCommon::LoadPipelines(
            {}, file.Path(), version,
            [&](std::ifstream& stream, VideoCommon::FileEnvironment environment) {
                std::array<char, 4> restored_key{};
                stream.read(restored_key.data(), restored_key.size());
                REQUIRE(restored_key == key);
                REQUIRE(environment.UsesHmma());
                ++callbacks;
            },
            [](std::ifstream&, std::vector<VideoCommon::FileEnvironment>) {
                FAIL("Unexpected graphics pipeline");
            });
    };
    REQUIRE_NOTHROW(load());
    REQUIRE(callbacks == 1);
    REQUIRE_NOTHROW(append(file.Path(), valid));
    callbacks = 0;
    REQUIRE_NOTHROW(load());
    REQUIRE(callbacks == 2);
    EnvironmentCacheFile empty;
    const std::array<const VideoCommon::GenericEnvironment*, 1> rejected{&excess_sites};
    REQUIRE_NOTHROW(append(empty.Path(), rejected));
    REQUIRE(std::filesystem::file_size(empty.Path()) == 0);
    REQUIRE_NOTHROW(append(empty.Path(), valid));
    REQUIRE(empty.Read() == original);
}
