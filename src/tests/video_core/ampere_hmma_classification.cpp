// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <span>

#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/frontend/ampere/hmma_program.h"
#include "shader_recompiler/frontend/ampere/program_capture.h"

namespace {

using namespace Shader::Ampere;
constexpr Instruction Hmma{{0x0830723c, 12, 0x810, 0x041ff000}};
constexpr Instruction Exit{{0x0000794d, 0, 0x03800000, 0}};

}

TEST_CASE("Native tensor classification ignores only never-executed predicates",
          "[video_core][ampere][hmma][classification]") {
    for (u32 predicate = 0; predicate < 16; ++predicate) {
        auto instruction = Hmma;
        instruction.words[0] = (instruction.words[0] & ~0xf000U) | (predicate << 12);
        const std::array program{instruction, Exit};
        REQUIRE(ContainsHmma(program) == (predicate != 15));
        if (predicate != 7 && predicate != 15) {
            REQUIRE_FALSE(LowerToMaxwell(program, false, true));
        }
        instruction.words[2] = 0;
        REQUIRE(ContainsHmma(std::array{instruction, Exit}) == (predicate != 15));
    }
}

TEST_CASE("Never-executed tensor instructions retain ordinary lowering",
          "[video_core][ampere][hmma][classification]") {
    auto instruction = Hmma;
    instruction.words[0] |= 0x8000U;
    const std::array program{instruction, Exit};
    REQUIRE_FALSE(ContainsHmma(program));
    const auto ordinary = LowerToMaxwell(program);
    const auto enabled = LowerToMaxwell(program, false, true);
    REQUIRE(ordinary);
    REQUIRE(enabled);
    REQUIRE(ordinary.instructions == enabled.instructions);
    REQUIRE(enabled.hmma_sites.empty());
    REQUIRE(enabled.source_size == sizeof(program));
}

TEST_CASE("Never-executed control transfers cannot extend native shader capture",
          "[video_core][ampere][hmma][classification]") {
    for (const u32 opcode : {0x944U, 0x945U, 0x947U, 0x949U, 0x958U}) {
        const std::array program{
            Instruction{{0xf000U | opcode, 32, 0x03800000, 0}}, Exit, Hmma, Exit};
        ProgramCaptureState capture;
        REQUIRE(capture.Observe(program[0], 0) == ProgramCaptureState::Result::Continue);
        REQUIRE(capture.Observe(program[1], 1) == ProgramCaptureState::Result::Complete);
        const auto captured = std::span{program}.first(2);
        REQUIRE_FALSE(ContainsHmma(captured));
        const auto lowered = LowerToMaxwell(captured);
        REQUIRE(lowered);
        REQUIRE(lowered.source_size == captured.size_bytes());
    }
    ProgramCaptureState capture;
    REQUIRE(capture.Observe(Instruction{{0xf947, 0xffffffe0, 0x0383ffff, 0}}, 0) ==
            ProgramCaptureState::Result::Continue);
    REQUIRE(capture.Observe(Exit, 1) == ProgramCaptureState::Result::Complete);
}

TEST_CASE("Possibly executed transfers still capture and reject unsupported tensors",
          "[video_core][ampere][hmma][classification]") {
    auto unsupported = Hmma;
    unsupported.words[2] = 0x10;
    const std::array program{
        Instruction{{0x0947, 16, 0x03800000, 0}}, Exit, unsupported, Exit};
    ProgramCaptureState capture;
    REQUIRE(capture.Observe(program[0], 0) == ProgramCaptureState::Result::Continue);
    REQUIRE(capture.Observe(program[1], 1) == ProgramCaptureState::Result::Continue);
    REQUIRE(capture.Observe(program[2], 2) == ProgramCaptureState::Result::Continue);
    REQUIRE(capture.Observe(program[3], 3) == ProgramCaptureState::Result::Complete);
    REQUIRE(ContainsHmma(program));
    REQUIRE_THROWS_AS(BuildHmmaProgram(program, std::array<u64, 10>{}, 0,
                                     Shader::Stage::Compute, {128, 1, 1}, true),
                      Shader::HmmaRejected);
}
