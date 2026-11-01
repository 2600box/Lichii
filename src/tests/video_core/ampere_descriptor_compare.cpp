// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"
#include "shader_recompiler/frontend/ir/ir_emitter.h"
#include "shader_recompiler/frontend/ir/program.h"
#include "shader_recompiler/frontend/maxwell/translate/translate.h"
#include "shader_recompiler/host_translate_info.h"
#include "shader_recompiler/ir_opt/passes.h"

namespace {
using Shader::Ampere::Instruction;
using Shader::Ampere::LowerToMaxwell;

constexpr Instruction exit_instruction{{0x794d, 0, 0x03800000, 0}};
constexpr Instruction sample{{0x08107b60, 0x31401009, 0x000e0fff, 0}};

Instruction Store(u32 binding, u32 offset, u32 dimension = 3, u32 predicate = 7,
                  u32 data = 24, u32 coordinate = 12) {
    return {{0xb9b | (predicate << 12) | (coordinate << 24),
             data | (offset << 8) | (binding << 22) | (dimension << 29), 0x00100f00, 0}};
}

Instruction ExtendedNotEqual(u32 destination, u32 low_predicate, u32 predicate = 7) {
    return {{0x20c | (predicate << 12), 1,
             0x03f05100 | (destination << 17) | (low_predicate << 4), 0}};
}

u32 Address(u32 index) {
    return 0x50 + index * 8 + ((index + 1) / 3) * 8;
}

class TestEnvironment final : public Shader::Environment {
public:
    explicit TestEnvironment(std::span<const u64> instructions_) : instructions{instructions_} {}

    u64 ReadInstruction(u32 address) override {
        for (u32 i = 0; i < instructions.size(); ++i) {
            if (address == Address(i)) return instructions[i];
        }
        FAIL("Read outside synthetic instructions");
        return 0;
    }
    u32 ReadCbufValue(u32 index, u32 offset) override {
        REQUIRE(((index == 5 && offset == 0x40) || (index == 2 && offset == 0x80)));
        return index == 5 ? 0x123 : 0x456;
    }
    Shader::TextureType ReadTextureType(u32 handle) override {
        REQUIRE((handle == 0x123 || handle == 0x456));
        return Shader::TextureType::Color2D;
    }
    Shader::TexturePixelFormat ReadTexturePixelFormat(u32 handle) override {
        REQUIRE((handle == 0x123 || handle == 0x456));
        return Shader::TexturePixelFormat::A8B8G8R8_UNORM;
    }
    bool IsTexturePixelFormatInteger(u32 handle) override {
        REQUIRE((handle == 0x123 || handle == 0x456));
        return false;
    }
    u32 TextureBoundBuffer() const override { return 5; }
    u32 ReadViewportTransformState() override { return 1; }
    u32 LocalMemorySize() const override { return 0; }
    u32 SharedMemorySize() const override { return 0; }
    std::array<u32, 3> WorkgroupSize() const override { return {1, 1, 1}; }
    bool HasHLEMacroState() const override { return false; }
    std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32, u32) override {
        return std::nullopt;
    }
    void Dump(u64, u64) override {}

private:
    std::span<const u64> instructions;
};

TEST_CASE("Ampere mixed-bank surface stores retain independent image and sample descriptors",
          "[video_core][ampere][descriptor_compare]") {
    const std::array code{sample, Store(2, 0x20), sample, exit_instruction};
    const auto lowered = LowerToMaxwell(code);
    REQUIRE(static_cast<bool>(lowered));
    REQUIRE(lowered.required_texture_buffer == 5);
    REQUIRE(lowered.source_size == sizeof(code));

    TestEnvironment env{lowered.instructions};
    Shader::ObjectPool<Shader::IR::Inst> pool;
    Shader::IR::Block block{pool};
    Shader::IR::IREmitter seed{block};
    seed.SetReg(Shader::IR::Reg::R8, seed.Imm32(0U));
    seed.SetReg(Shader::IR::Reg::R9, seed.Imm32(0U));
    seed.SetReg(Shader::IR::Reg::R12, seed.Imm32(7U));
    seed.SetReg(Shader::IR::Reg::R13, seed.Imm32(9U));
    for (u32 i = 0; i < 4; ++i) {
        seed.SetReg(static_cast<Shader::IR::Reg>(24 + i), seed.Imm32(11U * (i + 1)));
    }
    Shader::Maxwell::Translate(env, &block, Address(0),
                              Address(static_cast<u32>(lowered.instructions.size() - 1)));
    REQUIRE(std::count_if(block.begin(), block.end(), [](const Shader::IR::Inst& inst) {
        return inst.GetOpcode() == Shader::IR::Opcode::BindlessImageWrite;
    }) == 1);
    Shader::IR::Program program;
    program.blocks = {&block};
    program.post_order_blocks = {&block};
    Shader::Optimization::SsaRewritePass(program);
    Shader::Optimization::ConstantPropagationPass(env, program);
    Shader::HostTranslateInfo host_info;
    host_info.ApplyDescriptorLimitPolicy();
    Shader::Optimization::TexturePass(env, program, host_info);
    REQUIRE(program.info.texture_descriptors.size() == 1);
    REQUIRE(program.info.texture_descriptors.front().cbuf_index == 5);
    REQUIRE(program.info.texture_descriptors.front().cbuf_offset == 0x40);
    REQUIRE(program.info.image_descriptors.size() == 1);
    const auto& image = program.info.image_descriptors.front();
    REQUIRE(image.cbuf_index == 2);
    REQUIRE(image.cbuf_offset == 0x80);
    REQUIRE(image.type == Shader::TextureType::Color2D);
    REQUIRE(image.is_written);
    REQUIRE_FALSE(image.is_read);
    const auto store = std::find_if(block.begin(), block.end(), [](const Shader::IR::Inst& inst) {
        return inst.GetOpcode() == Shader::IR::Opcode::ImageWrite;
    });
    REQUIRE(store != block.end());
    REQUIRE(store->MayHaveSideEffects());
    REQUIRE(store->Flags<Shader::IR::TextureInstInfo>().descriptor_index == 0);
    const auto* coordinates = store->Arg(1).InstRecursive();
    REQUIRE(coordinates->GetOpcode() == Shader::IR::Opcode::CompositeConstructU32x2);
    REQUIRE(coordinates->Arg(0).U32() == 7);
    REQUIRE(coordinates->Arg(1).U32() == 9);
    const auto* color = store->Arg(2).InstRecursive();
    REQUIRE(color->GetOpcode() == Shader::IR::Opcode::CompositeConstructU32x4);
    for (u32 i = 0; i < 4; ++i) REQUIRE(color->Arg(i).U32() == 11U * (i + 1));
}

TEST_CASE("Ampere mixed-bank stores preserve predicates dimensions and word offsets",
          "[video_core][ampere][descriptor_compare]") {
    const u32 binding = GENERATE(0U, 2U, 5U, 17U);
    const u32 offset = GENERATE(0U, 0x20U, 0x1fffU);
    const u32 dimension = GENERATE(1U, 3U, 4U, 5U);
    const u32 predicate = GENERATE(0U, 7U, 8U, 14U, 15U);
    const std::array code{sample, Store(binding, offset, dimension, predicate), exit_instruction};
    const auto lowered = LowerToMaxwell(code);
    REQUIRE(static_cast<bool>(lowered));
    REQUIRE(lowered.required_texture_buffer == 5);
    u32 site = lowered.source_instruction_offsets[1];
    if (predicate == 15) {
        REQUIRE(site == lowered.source_instruction_offsets[2]);
        return;
    }
    const u64 guard = u64{predicate} << 16;
    if (binding != 5) {
        REQUIRE(lowered.instructions[site++] ==
                (0x4c98000000000000ULL | guard | 246 | (u64{15} << 39) |
                 (u64{offset} << 20) | (u64{binding} << 34)));
    }
    REQUIRE(lowered.instructions[site] ==
            (0xeb20000000000000ULL | guard | 24 | (u64{12} << 8) | (u64{15} << 20) |
             (u64{dimension} << 33) |
             (binding == 5 ? (u64{offset} << 36) | (u64{1} << 51) : u64{246} << 39)));
}

TEST_CASE("Ampere mixed-bank stores reject scratch overlap and unsupported modes",
          "[video_core][ampere][descriptor_compare]") {
    for (const auto store : std::array{
             Store(2, 0x20, 3, 7, 244, 12), Store(2, 0x20, 3, 7, 24, 245),
             Store(2, 0x20, 5, 7, 24, 244), Store(18, 0x20), Store(2, 0x2000),
             Store(2, 0x20, 2)}) {
        const auto lowered = LowerToMaxwell(std::array{sample, store, exit_instruction});
        REQUIRE_FALSE(static_cast<bool>(lowered));
        REQUIRE(lowered.error_offset == 16);
        REQUIRE(lowered.instructions.empty());
    }
    auto typed = Store(2, 0x20);
    typed.words[2] ^= 1U << 8;
    REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(std::array{sample, typed, exit_instruction})));
}

TEST_CASE("Ampere extended NE consumes the old predicate when its output aliases it",
          "[video_core][ampere][descriptor_compare]") {
    const u32 a = GENERATE(0U, 1U, 0x80000000U, 0xffffffffU);
    const u32 b = GENERATE(0U, 1U, 0x80000000U, 0xffffffffU);
    const u32 destination = GENERATE(0U, 3U, 6U);
    const u32 low_predicate = GENERATE(0U, 3U, 6U, 7U, 8U, 11U, 14U, 15U);
    const bool low_value = GENERATE(false, true);
    const auto lowered = LowerToMaxwell(
        std::array{ExtendedNotEqual(destination, low_predicate), exit_instruction});
    REQUIRE(static_cast<bool>(lowered));
    REQUIRE(lowered.instructions.size() == 2);
    TestEnvironment env{lowered.instructions};
    Shader::ObjectPool<Shader::IR::Inst> pool;
    Shader::IR::Block block{pool};
    Shader::IR::IREmitter seed{block};
    seed.SetReg(Shader::IR::Reg::R0, seed.Imm32(a));
    seed.SetReg(Shader::IR::Reg::R1, seed.Imm32(b));
    std::array<bool, 7> initial{};
    for (u32 i = 0; i < initial.size(); ++i) {
        initial[i] = i == (low_predicate & 7) ? low_value : i % 2 != 0;
        seed.SetPred(static_cast<Shader::IR::Pred>(i), seed.Imm1(initial[i]));
    }
    Shader::Maxwell::Translate(env, &block, Address(0), Address(1));
    Shader::IR::IREmitter observe{block};
    std::array<Shader::IR::Inst*, 7> results{};
    for (u32 i = 0; i < results.size(); ++i) {
        observe.Reference(observe.GetPred(static_cast<Shader::IR::Pred>(i)));
        results[i] = &block.back();
    }
    Shader::IR::Program program;
    program.blocks = {&block};
    program.post_order_blocks = {&block};
    Shader::Optimization::SsaRewritePass(program);
    Shader::Optimization::ConstantPropagationPass(env, program);
    const bool low = ((low_predicate & 7) == 7 || low_value) != ((low_predicate & 8) != 0);
    const bool expected = ((u64{a} << 32) | u64{low}) != (u64{b} << 32);
    for (u32 i = 0; i < results.size(); ++i) {
        REQUIRE(results[i]->Arg(0).IsImmediate());
        REQUIRE(results[i]->Arg(0).U1() == (i == destination ? expected : initial[i]));
    }
}

TEST_CASE("Ampere extended NE emits one ordinary comparison with the original guard",
          "[video_core][ampere][descriptor_compare]") {
    const u32 predicate = GENERATE(0U, 7U, 8U, 14U, 15U);
    const auto lowered = LowerToMaxwell(
        std::array{ExtendedNotEqual(0, 8, predicate), exit_instruction});
    REQUIRE(static_cast<bool>(lowered));
    REQUIRE(lowered.instructions.size() == (predicate == 15 ? 1 : 2));
    if (predicate != 15) {
        REQUIRE(lowered.instructions.front() ==
                (0x5b60000000000000ULL | (u64{predicate} << 16) | 7 | (u64{1} << 20) |
                 (u64{8} << 39) | (u64{1} << 45) | (u64{5} << 49)));
        REQUIRE(((lowered.instructions.front() >> 43) & 1) == 0);
    }
}

TEST_CASE("Ampere extended NE rejects unproved comparison and predicate combinations",
          "[video_core][ampere][descriptor_compare]") {
    for (const u32 bit : {64U, 73U, 74U, 76U, 79U, 80U, 84U, 87U, 90U, 96U}) {
        auto compare = ExtendedNotEqual(0, 0);
        compare.words[bit / 32] ^= 1U << (bit % 32);
        const auto lowered = LowerToMaxwell(std::array{compare, exit_instruction});
        REQUIRE_FALSE(static_cast<bool>(lowered));
        REQUIRE(lowered.error == "native extended integer comparison");
        REQUIRE(lowered.instructions.empty());
    }
    for (const u32 opcode : {0x80cU, 0xa0cU, 0xc0cU}) {
        auto compare = ExtendedNotEqual(0, 0);
        compare.words[0] = 0x7000 | opcode;
        REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(std::array{compare, exit_instruction})));
    }
}
} // namespace
