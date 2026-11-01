// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/frontend/ampere/hmma.h"
#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"

namespace {

using Words = std::array<u32, 4>;
using Shader::Ampere::DecodeHmma;
using Shader::Ampere::ValidateHmmaProgram;

constexpr Words hmma{0x0406723c, 8, 0x80a, 0x004fee00};
constexpr Words nop{0x7918, 0, 0, 0x000fea00};
constexpr Words exit_instruction{0x794d, 0, 0x03800000, 0x000fea00};

Words Branch(u32 from, u32 to, u32 predicate = 7) {
    const u64 relative = static_cast<u64>((s64{to} - from - 1) * 16);
    return {0x947U | (predicate << 12), static_cast<u32>(relative),
            0x03800000U | static_cast<u32>((relative >> 32) & 0x3ffff), 0x000fea00};
}

Words SetSync(u32 from, u32 to, u32 token = 0) {
    return {0x7945U | (token << 16), (to - from - 1) * 16, 0x03800000, 0x000fea00};
}

Words Sync(u32 token = 0) {
    return {0x7941U | (token << 16), 0, 0x03800000, 0x000fea00};
}

std::vector<Words> Diamond() {
    return {SetSync(0, 7), Branch(1, 4, 0), nop, Branch(3, 6), nop,
            nop, Sync(), hmma, exit_instruction};
}

} // namespace

TEST_CASE("Ampere HMMA decodes all supplied fragments", "[shader_recompiler][ampere][hmma]") {
    constexpr std::array<Words, 4> encodings{{
        {0x0830723c, 0x0c, 0x8ff, 0x041ff000},
        {0x0810723c, 0x02, 0x8ff, 0x001fe200},
        {0x040c723c, 0x10, 0x8ff, 0x041fe600},
        hmma,
    }};
    constexpr std::array<u32, 4> destinations{48, 16, 12, 6};
    constexpr std::array<u32, 4> a{8, 8, 4, 4};
    constexpr std::array<u32, 4> b{12, 2, 16, 8};
    for (size_t index = 0; index < encodings.size(); ++index) {
        const auto decoded = DecodeHmma(encodings[index], 246);
        REQUIRE(decoded);
        CHECK(decoded.instruction.destination == destinations[index]);
        CHECK(decoded.instruction.a == a[index]);
        CHECK(decoded.instruction.b == b[index]);
        CHECK(decoded.instruction.c == (index == 3 ? 10U : 255U));
    }
}

TEST_CASE("Ampere HMMA rejects every unsupported encoding bit", "[shader_recompiler][ampere][hmma]") {
    constexpr std::array<u32, 4> variable_masks{0xffff0000U, 0xffU, 0xffU, 0x3ffffe00U};
    for (u32 bit = 0; bit < 128; ++bit) {
        auto words = hmma;
        const u32 mask = 1U << (bit % 32);
        words[bit / 32] ^= mask;
        CAPTURE(bit);
        if (!(variable_masks[bit / 32] & mask)) {
            CHECK_FALSE(DecodeHmma(words, 246));
        } else if (bit >= 105) {
            CHECK(DecodeHmma(words, 246));
        }
    }
    for (u32 predicate = 0; predicate < 16; ++predicate) {
        auto words = hmma;
        words[0] = (words[0] & ~0xf000U) | (predicate << 12);
        CHECK(static_cast<bool>(DecodeHmma(words, 246)) == (predicate == 7));
    }
    CHECK_FALSE(DecodeHmma(hmma, 255));
    CHECK_FALSE(DecodeHmma(hmma, 0));
}

TEST_CASE("Ampere HMMA validates complete aligned register spans", "[shader_recompiler][ampere][hmma]") {
    for (u32 limit : {216U, 246U}) {
        for (u32 operand = 0; operand < 4; ++operand) {
            for (u32 first = 0; first < 256; ++first) {
                auto words = hmma;
                const u32 word = operand < 2 ? 0 : operand - 1;
                const u32 shift = operand < 2 ? 16 + operand * 8 : 0;
                words[word] = (words[word] & ~(0xffU << shift)) | (first << shift);
                const u32 count = operand == 1 ? 4 : 2;
                const bool valid = (first % count == 0 && first + count <= limit) ||
                                   (operand == 3 && first == 255);
                CAPTURE(limit, operand, first);
                CHECK(static_cast<bool>(DecodeHmma(words, limit)) == valid);
            }
        }
    }
}

TEST_CASE("Ampere HMMA allows source and destination aliases", "[shader_recompiler][ampere][hmma]") {
    for (u32 destination : {4U, 6U, 8U, 10U}) {
        auto words = hmma;
        words[0] = (words[0] & ~0xff0000U) | (destination << 16);
        REQUIRE(DecodeHmma(words, 246));
    }
    auto all_alias = hmma;
    all_alias[0] = 0x0404723c;
    all_alias[1] = 4;
    all_alias[2] = 0x804;
    REQUIRE(DecodeHmma(all_alias, 246));
}

TEST_CASE("Ampere HMMA recognizes normalized uniform arithmetic", "[shader_recompiler][ampere][hmma]") {
    const std::array<Words, 5> code{{
        {0x04047291, 6, 0x0f80103f, 0x001fc800},
        {0x05057290, 7, 0x087fe43f, 0x000fc800},
        {0x04047292, 5, 0x0f8efc3f, 0x000fc800},
        hmma,
        exit_instruction,
    }};
    REQUIRE(ValidateHmmaProgram(code, 216));
}

TEST_CASE("Ampere HMMA reserves registers for uniform shifts and reductions",
          "[shader_recompiler][ampere][hmma]") {
    using namespace Shader::Ampere;
    for (const Instruction uniform : std::array<Instruction, 2>{{
             {{0x04007c19, 0, 0x080016ff, 0x000fe200}},
             {{0xff00798e, 4, 0x0c10e100, 0x000fe200}},
         }}) {
        Instruction collective{hmma};
        collective.words[0] = (collective.words[0] & ~0xff0000U) | (216U << 16);
        REQUIRE_FALSE(LowerToMaxwell(std::array{uniform, collective, Instruction{exit_instruction}},
                                    false, true));
        collective.words[0] = (collective.words[0] & ~0xff0000U) | (214U << 16);
        REQUIRE(LowerToMaxwell(std::array{uniform, collective, Instruction{exit_instruction}},
                              false, true));
    }
}

TEST_CASE("Ampere HMMA proves straight line and reconverged diamonds", "[shader_recompiler][ampere][hmma]") {
    const std::array straight{hmma, hmma, exit_instruction};
    REQUIRE(ValidateHmmaProgram(straight, 246));
    const auto diamond = Diamond();
    REQUIRE(ValidateHmmaProgram(diamond, 246));
    const std::array nested{SetSync(0, 12), Branch(1, 10, 0), SetSync(2, 9, 1),
                            Branch(3, 6, 1), nop, Branch(5, 8), nop, nop, Sync(1),
                            Branch(9, 11), nop, Sync(), hmma, exit_instruction};
    REQUIRE(ValidateHmmaProgram(nested, 246));
    const std::array unused_scope{SetSync(0, 2), Sync(), hmma, exit_instruction};
    REQUIRE(ValidateHmmaProgram(unused_scope, 246));
    auto extra_condition = Diamond();
    extra_condition[1][2] = 0;
    REQUIRE(ValidateHmmaProgram(extra_condition, 246));
    auto skipped = straight;
    skipped[0] = {0xffff, 0xffffffff, 0xffffffff, 0xffffffff};
    REQUIRE(ValidateHmmaProgram(skipped, 246));
}

TEST_CASE("Ampere HMMA rejects divergent collectives and early exits", "[shader_recompiler][ampere][hmma]") {
    auto code = Diamond();
    code[2] = hmma;
    const auto divergent = ValidateHmmaProgram(code, 246);
    REQUIRE_FALSE(divergent);
    CHECK(divergent.error_offset == 32);
    code = Diamond();
    code[2] = exit_instruction;
    REQUIRE_FALSE(ValidateHmmaProgram(code, 246));
    auto conditional_exit = exit_instruction;
    conditional_exit[0] = 0x94d;
    const std::array early_exit{conditional_exit, hmma, exit_instruction};
    REQUIRE_FALSE(ValidateHmmaProgram(early_exit, 246));
    const std::array late_exit{hmma, conditional_exit, exit_instruction};
    REQUIRE(ValidateHmmaProgram(late_exit, 246));
    const std::array bypass{Branch(0, 3, 0), hmma, exit_instruction, exit_instruction};
    REQUIRE_FALSE(ValidateHmmaProgram(bypass, 246));
    const std::array late_branch{hmma, Branch(1, 3, 0), exit_instruction, exit_instruction};
    REQUIRE(ValidateHmmaProgram(late_branch, 246));
}

TEST_CASE("Ampere HMMA rejects malformed reconvergence", "[shader_recompiler][ampere][hmma]") {
    auto code = Diamond();
    code[0] = nop;
    REQUIRE_FALSE(ValidateHmmaProgram(code, 246));
    code = Diamond();
    code[6] = Sync(1);
    REQUIRE_FALSE(ValidateHmmaProgram(code, 246));
    code = Diamond();
    code[0] = SetSync(0, 8);
    REQUIRE_FALSE(ValidateHmmaProgram(code, 246));
    code = Diamond();
    code[3] = Branch(3, 7);
    REQUIRE_FALSE(ValidateHmmaProgram(code, 246));
    code = Diamond();
    code[1] = Branch(1, 7, 0);
    REQUIRE_FALSE(ValidateHmmaProgram(code, 246));
    code = Diamond();
    code[2] = SetSync(2, 6);
    REQUIRE_FALSE(ValidateHmmaProgram(code, 246));
    const std::array missing_sync{SetSync(0, 3), nop, hmma, exit_instruction};
    REQUIRE_FALSE(ValidateHmmaProgram(missing_sync, 246));
    const std::array orphan_sync{Sync(), hmma, exit_instruction};
    REQUIRE_FALSE(ValidateHmmaProgram(orphan_sync, 246));
}

TEST_CASE("Ampere HMMA rejects unresolved cyclic and unknown control flow", "[shader_recompiler][ampere][hmma]") {
    for (u32 opcode : {0x944U, 0x949U, 0x950U, 0x958U, 0x942U, 0x948U, 0x95bU, 0xfffU}) {
        const std::array code{Words{opcode | 0x7000U, 0, 0x03800000, 0}, hmma,
                              exit_instruction};
        REQUIRE_FALSE(ValidateHmmaProgram(code, 246));
    }
    const std::array backedge{hmma, Branch(1, 0), exit_instruction};
    REQUIRE_FALSE(ValidateHmmaProgram(backedge, 246));
    const std::array self_loop{hmma, Branch(1, 1), exit_instruction};
    REQUIRE_FALSE(ValidateHmmaProgram(self_loop, 246));
    const std::array unresolved{hmma, Branch(1, 3), exit_instruction};
    REQUIRE_FALSE(ValidateHmmaProgram(unresolved, 246));
    auto unaligned = Diamond();
    unaligned[1][1] += 4;
    REQUIRE_FALSE(ValidateHmmaProgram(unaligned, 246));
    REQUIRE_FALSE(ValidateHmmaProgram(std::array<Words, 1>{hmma}, 246));
    REQUIRE_FALSE(ValidateHmmaProgram({}, 246));
    REQUIRE_FALSE(ValidateHmmaProgram(std::array{hmma, exit_instruction}, 255));
}

TEST_CASE("Ampere HMMA rejects mutations of native control modes", "[shader_recompiler][ampere][hmma]") {
    for (u32 index : {0U, 1U, 6U, 8U}) {
        for (u32 bit : {91U, 96U, 102U, 104U, 126U, 127U}) {
            auto code = Diamond();
            code[index][bit / 32] |= 1U << (bit % 32);
            CAPTURE(index, bit);
            REQUIRE_FALSE(ValidateHmmaProgram(code, 246));
        }
    }
    for (u32 index : {0U, 6U}) {
        auto code = Diamond();
        code[index][0] &= ~0xf000U;
        REQUIRE_FALSE(ValidateHmmaProgram(code, 246));
    }
    auto branch_mode = Diamond();
    branch_mode[1][1] |= 1;
    REQUIRE_FALSE(ValidateHmmaProgram(branch_mode, 246));
    auto exit_mode = Diamond();
    exit_mode[8][2] |= 1U << 20;
    REQUIRE_FALSE(ValidateHmmaProgram(exit_mode, 246));
}

TEST_CASE("Ampere HMMA bounds the complete captured program", "[shader_recompiler][ampere][hmma]") {
    std::vector<Words> code(65536, nop);
    code[65534] = hmma;
    code[65535] = exit_instruction;
    REQUIRE(ValidateHmmaProgram(code, 246));
    code.push_back(exit_instruction);
    REQUIRE_FALSE(ValidateHmmaProgram(code, 246));
}
