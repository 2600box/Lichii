// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <unordered_map>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <spirv/unified1/spirv.hpp11>

#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"
#include "shader_recompiler/frontend/maxwell/translate_program.h"
#include "shader_recompiler/host_translate_info.h"
#include "shader_recompiler/ir_opt/passes.h"

namespace {
using Native = Shader::Ampere::Instruction;
constexpr Native Exit{{0x794d, 0, 0x03800000, 0}};

Native Store(u32 binding = 0, u32 predicate = 7, u32 dimension = 3) {
    return {{0x02000b9bU | (predicate << 12), (dimension << 29) | (binding << 22) | (12U << 8) | 4U,
             0x0010af00, 0}};
}

u32 Address(u32 index) {
    return 0x50 + index * 8 + ((index + 1) / 3) * 8;
}

class Environment final : public Shader::Environment {
public:
    explicit Environment(std::span<const Native> native)
        : lowered{Shader::Ampere::LowerToMaxwell(native)} {
        INFO(lowered.error);
        REQUIRE(lowered);
        stage = Shader::Stage::Compute;
        start_address = 0x50;
    }

    u64 ReadInstruction(u32 address) override {
        for (u32 i = 0; i < lowered.instructions.size(); ++i) {
            if (Address(i) == address)
                return lowered.instructions[i];
        }
        FAIL("Read outside synthetic FE39 surface program");
        return 0;
    }
    u32 ReadCbufValue(u32 index, u32 offset) override {
        REQUIRE(index < 18);
        REQUIRE(offset == 48);
        return 1 + index;
    }
    Shader::TextureType ReadTextureType(u32) override {
        return Shader::TextureType::Color2D;
    }
    Shader::TexturePixelFormat ReadTexturePixelFormat(u32) override {
        return Shader::TexturePixelFormat::R32_UINT;
    }
    bool IsTexturePixelFormatInteger(u32) override {
        return true;
    }
    u32 TextureBoundBuffer() const override {
        return lowered.required_texture_buffer;
    }
    u32 ReadViewportTransformState() override {
        return 1;
    }
    u32 LocalMemorySize() const override {
        return 0;
    }
    u32 SharedMemorySize() const override {
        return 0;
    }
    std::array<u32, 3> WorkgroupSize() const override {
        return {32, 1, 1};
    }
    bool HasHLEMacroState() const override {
        return false;
    }
    std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32, u32) override {
        return {};
    }
    void Dump(u64, u64) override {}

    Shader::Ampere::LoweredProgram lowered;
};

struct Compiler {
    Shader::ObjectPool<Shader::IR::Inst> instructions;
    Shader::ObjectPool<Shader::IR::Block> blocks;
    Shader::ObjectPool<Shader::Maxwell::Flow::Block> flow_blocks;

    Shader::IR::Program Translate(Environment& env) {
        Shader::Maxwell::Flow::CFG cfg{env, flow_blocks, env.StartAddress()};
        Shader::HostTranslateInfo host{
            .min_ssbo_alignment = 1,
            .support_float64 = true,
            .support_float16 = true,
            .support_int64 = true,
            .support_conditional_barrier = true,
        };
        host.ApplyDescriptorLimitPolicy();
        return Shader::Maxwell::TranslateProgram(instructions, blocks, env, cfg, host);
    }
};

std::vector<Native> Program(Native store) {
    std::vector<Native> code;
    for (u32 reg = 2; reg < 8; ++reg) {
        code.push_back({{0x7802U | (reg << 16), reg + 10, 0xf00, 0}});
    }
    code.push_back(store);
    code.push_back(Exit);
    return code;
}

std::vector<spv::Op> MemoryOperations(std::span<const u32> code) {
    std::unordered_map<u32, u32> constants;
    std::vector<spv::Op> operations;
    for (size_t offset = 5; offset < code.size();) {
        const u32 length = code[offset] >> 16;
        REQUIRE(length > 0);
        REQUIRE(offset + length <= code.size());
        const auto op = static_cast<spv::Op>(code[offset] & 0xffff);
        if (op == spv::Op::OpConstant && length == 4) {
            constants.emplace(code[offset + 2], code[offset + 3]);
        }
        REQUIRE(op != spv::Op::OpControlBarrier);
        if (op == spv::Op::OpMemoryBarrier) {
            REQUIRE(length == 3);
            REQUIRE(constants.at(code[offset + 1]) == static_cast<u32>(spv::Scope::Workgroup));
            const u32 semantics = constants.at(code[offset + 2]);
            REQUIRE((semantics & static_cast<u32>(spv::MemorySemanticsMask::AcquireRelease)) != 0);
            REQUIRE((semantics & static_cast<u32>(spv::MemorySemanticsMask::ImageMemory)) != 0);
            operations.push_back(op);
        } else if (op == spv::Op::OpImageWrite) {
            operations.push_back(op);
        }
        offset += length;
    }
    return operations;
}

std::vector<u32> Emit(Shader::IR::Program& program) {
    Shader::Optimization::VerificationPass(program);
    Shader::Profile profile{};
    profile.supported_spirv = 0x00010300;
    profile.unified_descriptor_binding = true;
    return Shader::Backend::SPIRV::EmitSPIRV(profile, program);
}

std::vector<spv::Op> ExecutedMemoryOperations(std::span<const u32> code) {
    MemoryOperations(code);
    std::unordered_map<u32, bool> conditions;
    std::unordered_map<u32, size_t> labels;
    size_t entry = 0;
    for (size_t offset = 5; offset < code.size(); offset += code[offset] >> 16) {
        const auto op = static_cast<spv::Op>(code[offset] & 0xffff);
        if (op == spv::Op::OpConstantTrue || op == spv::Op::OpConstantFalse) {
            conditions.emplace(code[offset + 2], op == spv::Op::OpConstantTrue);
        } else if (op == spv::Op::OpLabel) {
            labels.emplace(code[offset + 1], offset);
            if (entry == 0)
                entry = offset;
        }
    }
    REQUIRE(entry != 0);
    std::vector<spv::Op> operations;
    size_t steps = 0;
    for (size_t offset = entry; offset < code.size();) {
        REQUIRE(++steps < code.size());
        const auto op = static_cast<spv::Op>(code[offset] & 0xffff);
        if (op == spv::Op::OpReturn)
            return operations;
        if (op == spv::Op::OpBranch) {
            offset = labels.at(code[offset + 1]);
            continue;
        }
        if (op == spv::Op::OpBranchConditional) {
            const bool condition = conditions.at(code[offset + 1]);
            offset = labels.at(code[offset + (condition ? 2 : 3)]);
            continue;
        }
        if (op == spv::Op::OpMemoryBarrier || op == spv::Op::OpImageWrite) {
            operations.push_back(op);
        }
        offset += code[offset] >> 16;
    }
    FAIL("Synthetic SPIR-V did not reach its return");
    return operations;
}

TEST_CASE("FE39 strong CTA surface store emits scoped image memory barriers",
          "[video_core][ampere][fe39_surface]") {
    for (const bool mixed : {false, true}) {
        auto code = Program(Store());
        if (mixed) {
            Native weak = Store(1);
            weak.words[2] = 0x00100f00;
            code.insert(code.begin() + 6, weak);
        }
        Environment env{code};
        Compiler compiler;
        auto program = compiler.Translate(env);
        const auto ops = MemoryOperations(Emit(program));
        std::vector expected{spv::Op::OpMemoryBarrier, spv::Op::OpImageWrite,
                             spv::Op::OpMemoryBarrier};
        if (mixed)
            expected.insert(expected.begin(), spv::Op::OpImageWrite);
        REQUIRE(ops == expected);
        REQUIRE(program.info.image_descriptors.size() == (mixed ? 2 : 1));
        for (const auto& descriptor : program.info.image_descriptors) {
            REQUIRE(descriptor.is_written);
            REQUIRE(descriptor.cbuf_offset == 48);
        }
    }
}

TEST_CASE("FE39 strong CTA surface store preserves all instruction predicates",
          "[video_core][ampere][fe39_surface]") {
    for (u32 predicate = 0; predicate < 16; ++predicate) {
        const auto lowered = Shader::Ampere::LowerToMaxwell(std::array{Store(0, predicate), Exit});
        REQUIRE(lowered);
        REQUIRE(lowered.instructions.size() == (predicate == 15 ? 1 : 4));
        for (size_t i = 0; i + 1 < lowered.instructions.size(); ++i) {
            REQUIRE(((lowered.instructions[i] >> 16) & 15) == predicate);
        }
    }
    for (const u32 predicate : {0U, 8U, 15U}) {
        for (const bool enabled : {false, true}) {
            auto code = Program(Store(0, predicate));
            if (predicate != 15) {
                const bool compare_equal = (predicate == 8) != enabled;
                code.insert(code.begin() + 6,
                            {{0xff00720c, 255, compare_equal ? 0x03f02070U : 0x03f05070U, 0}});
            }
            Environment env{code};
            Compiler compiler;
            auto program = compiler.Translate(env);
            INFO(predicate);
            const auto operations = ExecutedMemoryOperations(Emit(program));
            if (enabled && predicate != 15) {
                REQUIRE(operations == std::vector{spv::Op::OpMemoryBarrier, spv::Op::OpImageWrite,
                                                  spv::Op::OpMemoryBarrier});
            } else {
                REQUIRE(operations.empty());
            }
        }
    }
}

TEST_CASE("FE39 strong CTA surface store rejects unproven scopes and malformed "
          "operands",
          "[video_core][ampere][fe39_surface]") {
    for (u32 order = 1; order < 16; ++order) {
        if (order == 5)
            continue;
        Native store = Store();
        store.words[2] = 0x00100f00U | (order << 13);
        REQUIRE_FALSE(Shader::Ampere::LowerToMaxwell(std::array{store, Exit}));
    }
    for (const u32 bit : {16U, 59U, 60U, 72U, 76U, 81U, 96U, 126U}) {
        Native store = Store();
        store.words[bit / 32] ^= 1U << (bit % 32);
        REQUIRE_FALSE(Shader::Ampere::LowerToMaxwell(std::array{store, Exit}));
    }
    for (const u32 data : {243U, 244U, 245U, 246U, 255U}) {
        Native store = Store();
        store.words[1] = (store.words[1] & ~255U) | data;
        REQUIRE_FALSE(Shader::Ampere::LowerToMaxwell(std::array{store, Exit}));
    }
    for (const u32 dimension : {0U, 2U, 6U, 7U}) {
        REQUIRE_FALSE(Shader::Ampere::LowerToMaxwell(std::array{Store(0, 7, dimension), Exit}));
    }
}
} // namespace
