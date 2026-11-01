// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <span>
#include <string_view>
#include <utility>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "shader_recompiler/exception.h"
#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"
#include "shader_recompiler/frontend/ir/ir_emitter.h"
#include "shader_recompiler/frontend/ir/program.h"
#include "shader_recompiler/frontend/maxwell/translate/translate.h"
#include "shader_recompiler/host_translate_info.h"
#include "shader_recompiler/ir_opt/passes.h"

namespace {
u32 Address(u32 index) {
    return 0x50 + index * 8 + ((index + 1) / 3) * 8;
}

u64 CbufMove(u32 destination, u32 bank, u32 offset) {
    return 0x4c98000000070000ULL | destination | (u64{offset / 4} << 20) |
           (u64{bank} << 34) | (u64{15} << 39);
}

u64 ImmediateMove(u32 destination, u32 value) {
    return 0x010000000007f000ULL | destination | (u64{value} << 20);
}

u64 Query(u32 destination, u32 source, u32 mask = 1) {
    return 0xdf50000000070000ULL | destination | (u64{source} << 8) |
           (u64{1} << 22) | (u64{mask} << 31);
}

TEST_CASE("Native bindless width queries preserve the Maxwell operands and predicate",
          "[video_core][ampere][txq_provenance]") {
    const u32 source = GENERATE(0U, 4U, 244U);
    const u32 destination = GENERATE(0U, 4U, 5U, 245U, 255U);
    const u32 predicate = GENERATE(0U, 1U, 6U, 7U, 8U, 14U);
    const std::array code{
        Shader::Ampere::Instruction{{0x370 | (predicate << 12) | (destination << 16) |
                                        (source << 24), 0x08000000, 0x1ff, 0}},
        Shader::Ampere::Instruction{{0x794d, 0, 0x03800000, 0}}};
    const auto lowered = Shader::Ampere::LowerToMaxwell(code);
    REQUIRE(lowered);
    REQUIRE(lowered.instructions.size() == 2);
    REQUIRE(lowered.instructions.front() ==
            ((Query(destination, source) & ~(u64{15} << 16)) | (u64{predicate} << 16)));
    REQUIRE(lowered.required_texture_buffer == ~0U);
    REQUIRE(lowered.source_size == sizeof(code));
}

TEST_CASE("Native bindless width queries reject unimplemented encoding fields",
          "[video_core][ampere][txq_provenance]") {
    const u32 word = GENERATE(1U, 2U, 3U);
    const u32 forbidden = word == 3 ? 0xc00001ffU : ~0U;
    for (u32 bit = 0; bit < 32; ++bit) {
        if (!(forbidden & (1U << bit))) continue;
        std::array code{
            Shader::Ampere::Instruction{{0x040a7370, 0x08000000, 0x1ff, 0}},
            Shader::Ampere::Instruction{{0x794d, 0, 0x03800000, 0}}};
        code[0].words[word] ^= 1U << bit;
        CAPTURE(word, bit);
        REQUIRE_FALSE(Shader::Ampere::LowerToMaxwell(code));
    }
}

TEST_CASE("Native bindless width queries reject invalid register pairs",
          "[video_core][ampere][txq_provenance]") {
    const u32 source = GENERATE(1U, 243U, 245U, 246U, 254U, 255U);
    const std::array code{
        Shader::Ampere::Instruction{{0x000a7370 | (source << 24), 0x08000000, 0x1ff, 0}},
        Shader::Ampere::Instruction{{0x794d, 0, 0x03800000, 0}}};
    REQUIRE_FALSE(Shader::Ampere::LowerToMaxwell(code));
}

class QueryEnvironment final : public Shader::Environment {
public:
    explicit QueryEnvironment(std::span<const u64> instructions_) : instructions{instructions_} {}

    u64 ReadInstruction(u32 address) override {
        for (u32 i = 0; i < instructions.size(); ++i) {
            if (address == Address(i)) return instructions[i];
        }
        FAIL("Read outside synthetic TXQ instructions");
        return 0;
    }
    u32 ReadCbufValue(u32 bank, u32 offset) override {
        cbuf_reads.emplace_back(bank, offset);
        if (bank == 2 && offset == 0x80) return 0x123;
        if (bank == 3 && offset == 0x100) return 0x00400000;
        if (bank == 3 && offset == 0x104) return 2;
        FAIL("Read outside synthetic descriptor and pointer slots");
        return 0;
    }
    Shader::TextureType ReadTextureType(u32 handle) override {
        texture_handles.push_back(handle);
        return Shader::TextureType::ColorArray2D;
    }
    Shader::TexturePixelFormat ReadTexturePixelFormat(u32 handle) override {
        texture_handles.push_back(handle);
        return Shader::TexturePixelFormat::A8B8G8R8_UNORM;
    }
    bool IsTexturePixelFormatInteger(u32 handle) override {
        texture_handles.push_back(handle);
        return false;
    }
    u32 TextureBoundBuffer() const override {
        FAIL("Bindless TXQ must not use the bound descriptor bank");
        return 0;
    }
    u32 ReadViewportTransformState() override { return 1; }
    u32 LocalMemorySize() const override { return 0; }
    u32 SharedMemorySize() const override { return 0; }
    std::array<u32, 3> WorkgroupSize() const override { return {1, 1, 1}; }
    bool HasHLEMacroState() const override { return false; }
    std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32, u32) override {
        return std::nullopt;
    }
    void Dump(u64, u64) override {}

    std::vector<std::pair<u32, u32>> cbuf_reads;
    std::vector<u32> texture_handles;

private:
    std::span<const u64> instructions;
};

TEST_CASE("Bindless TXQ keeps direct-cbuf handles distinct from LOD and aliased outputs",
          "[video_core][ampere][txq_provenance]") {
    const u32 destination = GENERATE(4U, 5U, 10U);
    const u32 lod = GENERATE(2U, 5U);
    const u32 mask = GENERATE(1U, 8U);
    const std::array code{CbufMove(4, 2, 0x80), ImmediateMove(5, lod),
                          Query(destination, 4, mask)};
    QueryEnvironment env{code};
    Shader::ObjectPool<Shader::IR::Inst> pool;
    Shader::IR::Block block{pool};
    Shader::Maxwell::Translate(env, &block, Address(0), Address(static_cast<u32>(code.size())));
    Shader::IR::IREmitter observe{block};
    observe.Reference(observe.GetReg(static_cast<Shader::IR::Reg>(destination)));
    auto* const result = &block.back();
    Shader::IR::Program program;
    program.blocks = {&block};
    program.post_order_blocks = {&block};
    Shader::Optimization::SsaRewritePass(program);
    Shader::Optimization::ConstantPropagationPass(env, program);

    const auto query = std::find_if(block.begin(), block.end(), [](const Shader::IR::Inst& inst) {
        return inst.GetOpcode() == Shader::IR::Opcode::BindlessImageQueryDimensions;
    });
    REQUIRE(query != block.end());
    const auto* const handle = query->Arg(0).InstRecursive();
    REQUIRE(handle->GetOpcode() == Shader::IR::Opcode::GetCbufU32);
    REQUIRE(handle->Arg(0).U32() == 2);
    REQUIRE(handle->Arg(1).U32() == 0x80);
    REQUIRE(query->Arg(1).IsImmediate());
    REQUIRE(query->Arg(1).U32() == lod);
    REQUIRE(query->Arg(2).U1() == (mask == 1));
    const auto* const component = result->Arg(0).InstRecursive();
    REQUIRE(component->GetOpcode() == Shader::IR::Opcode::CompositeExtractU32x4);
    REQUIRE(component->Arg(0).InstRecursive() == &*query);
    REQUIRE(component->Arg(1).U32() == (mask == 1 ? 0 : 3));

    Shader::HostTranslateInfo host_info;
    host_info.ApplyDescriptorLimitPolicy();
    Shader::Optimization::GlobalMemoryToStorageBufferPass(program, host_info);
    REQUIRE(program.info.storage_buffers_descriptors.empty());
    Shader::Optimization::TexturePass(env, program, host_info);
    REQUIRE(query->GetOpcode() == Shader::IR::Opcode::ImageQueryDimensions);
    REQUIRE(query->Arg(1).U32() == lod);
    REQUIRE(query->Flags<Shader::IR::TextureInstInfo>().type == Shader::TextureType::ColorArray2D);
    REQUIRE(program.info.texture_descriptors.size() == 1);
    REQUIRE(program.info.texture_descriptors.front().cbuf_index == 2);
    REQUIRE(program.info.texture_descriptors.front().cbuf_offset == 0x80);
    REQUIRE_FALSE(env.texture_handles.empty());
    for (const u32 handle_value : env.texture_handles) REQUIRE(handle_value == 0x123);
    for (const auto& [bank, offset] : env.cbuf_reads) {
        REQUIRE(bank == 2);
        REQUIRE(offset == 0x80);
    }
}

TEST_CASE("Bindless TXQ rejects a loaded descriptor instead of binding its cbuf pointer",
          "[video_core][ampere][txq_provenance]") {
    const bool lower_global_memory = GENERATE(false, true);
    CAPTURE(lower_global_memory);
    const std::array code{
        CbufMove(0, 3, 0x100), CbufMove(1, 3, 0x104),
        0xeed0000000070000ULL | 4 | (u64{0x20} << 20) | (u64{1} << 45) | (u64{5} << 48),
        ImmediateMove(5, 3), Query(8, 4)};
    QueryEnvironment env{code};
    Shader::ObjectPool<Shader::IR::Inst> pool;
    Shader::IR::Block block{pool};
    Shader::Maxwell::Translate(env, &block, Address(0), Address(static_cast<u32>(code.size())));
    Shader::IR::Program program;
    program.blocks = {&block};
    program.post_order_blocks = {&block};
    Shader::Optimization::SsaRewritePass(program);
    Shader::Optimization::ConstantPropagationPass(env, program);
    const auto query = std::find_if(block.begin(), block.end(), [](const Shader::IR::Inst& inst) {
        return inst.GetOpcode() == Shader::IR::Opcode::BindlessImageQueryDimensions;
    });
    REQUIRE(query != block.end());
    const auto* const handle = query->Arg(0).InstRecursive();
    REQUIRE(handle->GetOpcode() == Shader::IR::Opcode::CompositeExtractU32x2);
    REQUIRE(handle->Arg(1).U32() == 0);
    REQUIRE(handle->Arg(0).InstRecursive()->GetOpcode() == Shader::IR::Opcode::LoadGlobal64);
    REQUIRE(query->Arg(1).U32() == 3);

    Shader::HostTranslateInfo host_info;
    host_info.min_ssbo_alignment = 16;
    host_info.ApplyDescriptorLimitPolicy();
    if (lower_global_memory) {
        Shader::Optimization::GlobalMemoryToStorageBufferPass(program, host_info);
        REQUIRE(program.info.storage_buffers_descriptors.size() == 1);
        REQUIRE(program.info.storage_buffers_descriptors.front().cbuf_index == 3);
        REQUIRE(program.info.storage_buffers_descriptors.front().cbuf_offset == 0x100);
        REQUIRE(handle->Arg(0).InstRecursive()->GetOpcode() == Shader::IR::Opcode::LoadStorage64);
    }
    bool rejected = false;
    try {
        Shader::Optimization::TexturePass(env, program, host_info);
    } catch (const Shader::NotImplementedException& error) {
        rejected = true;
        CHECK(std::string_view{error.what()}.find("Failed to track bindless texture constant buffer") !=
              std::string_view::npos);
    }
    for (const auto& descriptor : program.info.texture_descriptors) {
        INFO("Selected cbuf bank " << descriptor.cbuf_index << " offset " << descriptor.cbuf_offset);
        CHECK_FALSE(descriptor.cbuf_index == 3);
    }
    CHECK(rejected);
    CHECK(program.info.texture_descriptors.empty());
    CHECK(env.cbuf_reads.empty());
    CHECK(env.texture_handles.empty());
}
TEST_CASE("Bindless descriptor tracking cannot hide a loaded source behind a cbuf operand",
          "[video_core][ampere][txq_provenance]") {
    const bool loaded_first = GENERATE(false, true);
    QueryEnvironment env{std::span<const u64>{}};
    Shader::ObjectPool<Shader::IR::Inst> pool;
    Shader::IR::Block block{pool};
    Shader::IR::IREmitter ir{block};
    const auto cbuf = ir.GetCbuf(ir.Imm32(2U), ir.Imm32(0x80U));
    const auto loaded = ir.LoadGlobal32(ir.Imm64(0x200400020ULL));
    const auto handle = loaded_first ? ir.BitwiseOr(loaded, cbuf) : ir.BitwiseOr(cbuf, loaded);
    ir.Reference(ir.ImageQueryDimension(handle, ir.Imm32(0U), ir.Imm1(true)));
    Shader::IR::Program program;
    program.blocks = {&block};
    program.post_order_blocks = {&block};
    Shader::HostTranslateInfo host_info;
    host_info.ApplyDescriptorLimitPolicy();
    REQUIRE_THROWS_AS(Shader::Optimization::TexturePass(env, program, host_info),
                      Shader::NotImplementedException);
    REQUIRE(program.info.texture_descriptors.empty());
    REQUIRE(env.cbuf_reads.empty());
    REQUIRE(env.texture_handles.empty());
}
} // namespace
