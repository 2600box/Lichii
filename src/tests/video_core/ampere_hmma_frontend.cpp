// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "ampere_hmma_fixture.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/frontend/maxwell/translate/translate.h"
#include "shader_recompiler/ir_opt/passes.h"

namespace {

Shader::Profile QualifiedProfile() {
    Shader::Profile profile{};
    profile.supported_spirv = 0x00010300;
    profile.unified_descriptor_binding = true;
    profile.support_hmma16816_f16 = true;
    profile.max_compute_shared_memory_size = 49152;
    profile.max_subgroup_size = 32;
    return profile;
}

}

TEST_CASE("Native HMMA snapshots aliased sources before destination writes",
          "[video_core][ampere][hmma]") {
    using namespace Shader;
    for (const u32 destination : {8U, 10U, 12U, 16U, 60U}) {
        for (const u32 c : {60U, 255U}) {
            const auto native = HmmaTest::NativeProgram({0x0800723c | (destination << 16),
                                                        12, 0x800 | c, 0});
            HmmaTest::Environment env{native};
            ObjectPool<IR::Inst> instructions;
            IR::Block block{instructions};
            const u32 address = env.program.sites.front().address;
            Maxwell::Location end{address};
            ++end;
            Maxwell::Translate(env, &block, address, end.Offset());
            bool collective{};
            u32 writes{};
            for (const auto& inst : block.Instructions()) {
                if (inst.GetOpcode() == IR::Opcode::GetRegister) {
                    REQUIRE_FALSE(collective);
                } else if (inst.GetOpcode() == IR::Opcode::Hmma16816F16) {
                    REQUIRE_FALSE(collective);
                    REQUIRE(inst.MayHaveSideEffects());
                    REQUIRE(inst.Arg(0).Type() == IR::Type::U32x4);
                    REQUIRE(inst.Arg(1).Type() == IR::Type::U32x2);
                    REQUIRE(inst.Arg(2).Type() == IR::Type::U32x2);
                    if (c == 255) {
                        REQUIRE(inst.Arg(2).Inst()->Arg(0).U32() == 0);
                        REQUIRE(inst.Arg(2).Inst()->Arg(1).U32() == 0);
                    }
                    collective = true;
                } else if (inst.GetOpcode() == IR::Opcode::SetRegister) {
                    REQUIRE(collective);
                    REQUIRE(static_cast<u32>(inst.Arg(0).Reg()) == destination + writes);
                    ++writes;
                }
            }
            REQUIRE(collective);
            REQUIRE(writes == 2);
            REQUIRE_THROWS_AS(env.ReadInstruction(address), HmmaRejected);
        }
    }
}

TEST_CASE("Native HMMA passes the real CFG, SSA and SPIR-V pipeline",
          "[video_core][ampere][hmma]") {
    using namespace Shader;
    for (const auto words : std::array<std::array<u32, 4>, 4>{{
             {0x0830723c, 12, 0x8ff, 0x041ff000},
             {0x0810723c, 2, 0x8ff, 0x001fe200},
             {0x040c723c, 16, 0x8ff, 0x041fe600},
             {0x0810723c, 12, 0x83c, 0x004fde00},
         }}) {
        HmmaTest::Environment env{HmmaTest::NativeProgram(words)};
        HmmaTest::Compiler compiler;
        auto program = compiler.Translate(env);
        REQUIRE(program.info.uses_hmma);
        REQUIRE(program.workgroup_size == std::array<u32, 3>{128, 1, 1});
        REQUIRE(program.info.storage_buffers_descriptors.size() == 2);
        Optimization::VerificationPass(program);
        const auto code = Backend::SPIRV::EmitSPIRV(QualifiedProfile(), program);
        REQUIRE(code.front() == 0x07230203);
        REQUIRE(code.size() * sizeof(u32) < 32768);
    }
}

TEST_CASE("Native HMMA backend is compact and rejects unqualified profiles",
          "[video_core][ampere][hmma]") {
    using namespace Shader;
    for (const u32 count : {1U, 192U, 288U}) {
        HmmaTest::Environment env{HmmaTest::NativeProgram({0x0810723c, 12, 0x83c, 0}, count)};
        HmmaTest::Compiler compiler;
        auto program = compiler.Translate(env);
        auto profile = QualifiedProfile();
        const auto code = Backend::SPIRV::EmitSPIRV(profile, program);
        REQUIRE(code.size() * sizeof(u32) < 131072);
        profile.support_hmma16816_f16 = false;
        REQUIRE_THROWS_AS(Backend::SPIRV::EmitSPIRV(profile, program), HmmaRejected);
        profile.support_hmma16816_f16 = true;
        profile.max_compute_shared_memory_size = 5119;
        REQUIRE_THROWS_AS(Backend::SPIRV::EmitSPIRV(profile, program), HmmaRejected);
    }
}

TEST_CASE("Native HMMA reconverges finite diamonds in the actual structured pipeline",
          "[video_core][ampere][hmma]") {
    using namespace Shader;
    for (const bool nested : {false, true}) {
        HmmaTest::Environment env{HmmaTest::DiamondProgram(nested)};
        HmmaTest::Compiler compiler;
        auto program = compiler.Translate(env);
        u32 depth{};
        u32 selections{};
        u32 collectives{};
        for (const auto& node : program.syntax_list) {
            if (node.type == IR::AbstractSyntaxNode::Type::If) {
                ++depth;
                ++selections;
            } else if (node.type == IR::AbstractSyntaxNode::Type::EndIf) {
                REQUIRE(depth > 0);
                --depth;
            } else if (node.type == IR::AbstractSyntaxNode::Type::Block) {
                for (const auto& inst : node.data.block->Instructions()) {
                    if (inst.GetOpcode() == IR::Opcode::Hmma16816F16) {
                        REQUIRE(depth == 0);
                        ++collectives;
                    }
                }
            }
        }
        REQUIRE(selections > 0);
        REQUIRE(collectives == 1);
        const auto code = Backend::SPIRV::EmitSPIRV(QualifiedProfile(), program);
        REQUIRE(code.front() == 0x07230203);
    }
}

TEST_CASE("Native HMMA rejects divergent collectives exits and loops before translation",
          "[video_core][ampere][hmma]") {
    using namespace Shader;
    auto code = HmmaTest::DiamondProgram();
    const auto hmma = code[22];
    code[17] = hmma;
    REQUIRE_THROWS_AS(HmmaTest::Environment{code}, HmmaRejected);
    code = HmmaTest::DiamondProgram();
    code[17] = {{0x794d, 0, 0x03800000, 0x000fea00}};
    REQUIRE_THROWS_AS(HmmaTest::Environment{code}, HmmaRejected);
    code = HmmaTest::DiamondProgram();
    code[18] = {{0x7947, 0xfffffff0, 0x0383ffff, 0x000fea00}};
    REQUIRE_THROWS_AS(HmmaTest::Environment{code}, HmmaRejected);
}

TEST_CASE("Native HMMA permits conditional exits only after the final collective",
          "[video_core][ampere][hmma]") {
    using namespace Shader;
    auto code = HmmaTest::DiamondProgram();
    code.insert(code.end() - 1, {{0x094d, 0, 0x03800000, 0x000fea00}});
    HmmaTest::Environment env{code};
    HmmaTest::Compiler compiler;
    auto program = compiler.Translate(env);
    REQUIRE(program.info.uses_hmma);
    REQUIRE_NOTHROW(Backend::SPIRV::EmitSPIRV(QualifiedProfile(), program));
    code = HmmaTest::DiamondProgram();
    code.insert(code.begin() + 22, {{0x094d, 0, 0x03800000, 0x000fea00}});
    REQUIRE_THROWS_AS(HmmaTest::Environment{code}, HmmaRejected);
}
