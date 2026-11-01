// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <bit>
#include <random>
#include <unordered_map>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"
#include "shader_recompiler/frontend/maxwell/translate/translate.h"

namespace {
namespace IR = Shader::IR;
using Native = Shader::Ampere::Instruction;
constexpr Native Exit{{0x794d, 0, 0x03800000, 0}};

u32 Address(u32 index) {
    return 0x50 + index * 8 + ((index + 1) / 3) * 8;
}

class Environment final : public Shader::Environment {
public:
    explicit Environment(std::span<const u64> words_) : words{words_} {
        stage = Shader::Stage::Compute;
    }
    u64 ReadInstruction(u32 address) override {
        for (u32 i = 0; i < words.size(); ++i) {
            if (Address(i) == address)
                return words[i];
        }
        FAIL("Read outside synthetic compute39 instructions");
        return 0;
    }
    u32 ReadCbufValue(u32, u32) override {
        FAIL("Unexpected constant buffer");
        return 0;
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
    u32 ReadViewportTransformState() override {
        return 1;
    }
    u32 TextureBoundBuffer() const override {
        return 0;
    }
    u32 LocalMemorySize() const override {
        return 0;
    }
    u32 SharedMemorySize() const override {
        return 1024;
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

private:
    std::span<const u64> words;
};

struct Program {
    explicit Program(Native instruction)
        : lowered{Shader::Ampere::LowerToMaxwell(std::array{instruction, Exit})} {
        INFO(lowered.error);
        REQUIRE(lowered);
        Environment env{lowered.instructions};
        for (u32 i = 0; i + 1 < lowered.instructions.size(); ++i) {
            auto* block = blocks.Create(instructions);
            Shader::Maxwell::Translate(env, block, Address(i), Address(i + 1));
            steps.push_back(block);
        }
    }
    unsigned Count(IR::Opcode opcode) const {
        unsigned count{};
        for (const auto* block : steps) {
            for (const auto& instruction : *block)
                count += instruction.GetOpcode() == opcode;
        }
        return count;
    }
    Shader::Ampere::LoweredProgram lowered;
    Shader::ObjectPool<IR::Inst> instructions;
    Shader::ObjectPool<IR::Block> blocks;
    std::vector<IR::Block*> steps;
};

struct Machine {
    std::array<u32, 256> registers{};
    std::unordered_map<u32, u32> shared;
    unsigned atomic_calls{};

    void Execute(const Program& program, bool predicate) {
        for (size_t step = 0; step < program.steps.size(); ++step) {
            const u32 guard = (program.lowered.instructions[step] >> 16) & 15;
            REQUIRE(((guard & 7) == 7 || (guard & 7) == 0));
            const bool active = ((guard & 7) == 7 || predicate) != ((guard & 8) != 0);
            if (!active)
                continue;
            std::unordered_map<const IR::Inst*, u32> values;
            const auto read = [&](const IR::Value& value) -> u32 {
                if (!value.IsImmediate())
                    return values.at(value.Inst());
                if (value.Type() == IR::Type::U1)
                    return value.U1();
                REQUIRE(value.Type() == IR::Type::U32);
                return value.U32();
            };
            for (const auto& inst : *program.steps[step]) {
                u32 result{};
                const auto u = [&](size_t i) { return read(inst.Arg(i)); };
                const auto s = [&](size_t i) { return std::bit_cast<s32>(u(i)); };
                switch (inst.GetOpcode()) {
                case IR::Opcode::GetRegister: {
                    const u32 reg = static_cast<u32>(inst.Arg(0).Reg());
                    result = reg == 255 ? 0 : registers[reg];
                    break;
                }
                case IR::Opcode::SetRegister:
                    if (const u32 reg = static_cast<u32>(inst.Arg(0).Reg()); reg != 255)
                        registers[reg] = u(1);
                    break;
                case IR::Opcode::Identity:
                    result = u(0);
                    break;
                case IR::Opcode::IAdd32:
                    result = u(0) + u(1);
                    break;
                case IR::Opcode::IMul32:
                    result = u(0) * u(1);
                    break;
                case IR::Opcode::INeg32:
                    result = 0U - u(0);
                    break;
                case IR::Opcode::BitwiseAnd32:
                    result = u(0) & u(1);
                    break;
                case IR::Opcode::BitFieldUExtract:
                    result = static_cast<u32>((u64{u(0)} >> u(1)) & ((u64{1} << u(2)) - 1));
                    break;
                case IR::Opcode::ShiftRightLogical32:
                    result = u(1) < 32 ? u(0) >> u(1) : 0;
                    break;
                case IR::Opcode::ShiftRightArithmetic32:
                    result = static_cast<u32>(s(0) >> std::min(u(1), 31U));
                    break;
                case IR::Opcode::ULessThan:
                    result = u(0) < u(1);
                    break;
                case IR::Opcode::SLessThan:
                    result = s(0) < s(1);
                    break;
                case IR::Opcode::SelectU32:
                    result = u(0) ? u(1) : u(2);
                    break;
                case IR::Opcode::SharedAtomicUMin32:
                case IR::Opcode::SharedAtomicUMax32:
                    REQUIRE(inst.NumArgs() == 2);
                    result = shared.at(u(0));
                    shared.at(u(0)) = inst.GetOpcode() == IR::Opcode::SharedAtomicUMin32
                                          ? std::min(result, u(1))
                                          : std::max(result, u(1));
                    ++atomic_calls;
                    break;
                default:
                    FAIL("Unexpected production IR opcode " << static_cast<u32>(inst.GetOpcode()));
                }
                values[&inst] = result;
            }
        }
    }
};

TEST_CASE("Compute39 signed high multiply executes production Maxwell IR",
          "[ampere][compute39_ir]") {
    constexpr std::array values{0U,          1U,          0xffffU,     0x10000U,
                                0x7fffffffU, 0x80000000U, 0x80000001U, 0xfffffffeU,
                                0xffffffffU, 0x55555555U, 0xaaaaaaaaU};
    for (bool is_signed : {false, true}) {
        for (u32 source : {3U, 255U}) {
            for (u32 destination : {3U, 5U, 255U}) {
                for (u32 guard : {0U, 7U, 8U}) {
                    for (u32 b : values) {
                        Program program{
                            {{0x827U | (guard << 12) | (destination << 16) | (source << 24), b,
                              0x078e00ffU | (u32{is_signed} << 9), 0}}};
                        REQUIRE(program.Count(IR::Opcode::IMul32) == 4);
                        REQUIRE(program.Count(IR::Opcode::ShiftRightArithmetic32) ==
                                u32{is_signed});
                        for (u32 a : values) {
                            for (bool enabled : {false, true}) {
                                Machine machine;
                                machine.registers.fill(0xdeadbeefU);
                                machine.registers[source] = a;
                                const auto before = machine.registers;
                                const u32 input = source == 255 ? 0 : a;
                                const u64 product =
                                    is_signed ? static_cast<u64>(s64{std::bit_cast<s32>(input)} *
                                                                 s64{std::bit_cast<s32>(b)})
                                              : u64{input} * b;
                                const bool active = guard == 7 || enabled != (guard == 8);
                                machine.Execute(program, enabled);
                                for (u32 reg = 0; reg < 246; ++reg) {
                                    const u32 expected = active && reg == destination
                                                             ? static_cast<u32>(product >> 32)
                                                             : before[reg];
                                    REQUIRE(machine.registers[reg] == expected);
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    std::mt19937 random{0x39827};
    for (unsigned i = 0; i < 1000; ++i) {
        const u32 a = random(), b = random();
        Program program{{{0x03037827, b, 0x078e02ff, 0}}};
        Machine machine;
        machine.registers[3] = a;
        machine.Execute(program, true);
        const u64 product =
            static_cast<u64>(s64{std::bit_cast<s32>(a)} * s64{std::bit_cast<s32>(b)});
        REQUIRE(machine.registers[3] == static_cast<u32>(product >> 32));
    }
}

TEST_CASE("Compute39 shared MIN and MAX translate to unsigned atomics with exact byte address and "
          "old result",
          "[ampere][compute39_ir]") {
    const u32 operation = GENERATE(1U, 2U);
    for (u32 address : {4U, 255U}) {
        for (u32 offset : {0U, 4U, 0x7ffffcU, 0x800000U, 0xfffffcU}) {
            for (u32 destination : {4U, 6U, 11U, 255U}) {
                for (u32 guard : {0U, 7U, 8U}) {
                    Program program{
                        {{0x38cU | (guard << 12) | (destination << 16) | (address << 24),
                          (offset << 8) | 11U, operation << 23, 0}}};
                    REQUIRE(program.Count(IR::Opcode::SharedAtomicUMin32) ==
                            unsigned{operation == 1});
                    REQUIRE(program.Count(IR::Opcode::SharedAtomicUMax32) ==
                            unsigned{operation == 2});
                    REQUIRE(program.Count(IR::Opcode::SharedAtomicSMin32) == 0);
                    REQUIRE(program.Count(IR::Opcode::SharedAtomicSMax32) == 0);
                    REQUIRE(program.Count(IR::Opcode::SharedAtomicIAdd32) == 0);
                    const u32 relative = (offset & 0x800000U) ? offset | 0xff000000U : offset;
                    const u32 location = address == 255 ? offset : 0x100U + relative;
                    for (u32 old : {0U, 7U, 0x80000000U, 0xffffffffU}) {
                        for (u32 data : {0U, 5U, 0x7fffffffU, 0xffffffffU}) {
                            for (bool enabled : {false, true}) {
                                Machine machine;
                                machine.registers.fill(0xdeadbeefU);
                                machine.registers[4] = 0x100;
                                machine.registers[11] = data;
                                machine.shared[location] = old;
                                const auto before = machine.registers;
                                const bool active = guard == 7 || enabled != (guard == 8);
                                machine.Execute(program, enabled);
                                REQUIRE(machine.atomic_calls == unsigned{active});
                                REQUIRE(machine.shared.size() == 1);
                                const u32 updated =
                                    operation == 1 ? std::min(old, data) : std::max(old, data);
                                REQUIRE(machine.shared.at(location) == (active ? updated : old));
                                for (u32 reg = 0; reg < 246; ++reg) {
                                    REQUIRE(machine.registers[reg] ==
                                            (active && reg == destination ? old : before[reg]));
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}

TEST_CASE("Compute39 atomics and high multiply reject unimplemented neighboring modes",
          "[ampere][compute39_ir]") {
    const Native multiply{{0x03057827, 23, 0x078e02ff, 0}};
    const Native minimum{{0x0406738c, 0x100b, 1U << 23, 0}};
    const Native maximum{{0x0406738c, 0x100b, 2U << 23, 0}};
    const auto reject = [&](Native bad) {
        const auto result = Shader::Ampere::LowerToMaxwell(std::array{minimum, bad, Exit});
        REQUIRE_FALSE(result);
        REQUIRE(result.error_offset == sizeof(Native));
        REQUIRE(result.instructions.empty());
        REQUIRE(result.source_instruction_offsets.empty());
        REQUIRE(result.hmma_sites.empty());
    };
    for (const Native good : {multiply, minimum, maximum}) {
        for (u32 bit = 0; bit < 32; ++bit) {
            INFO("modifier bit " << bit);
            const bool is_multiply = (good.words[0] & 0xfff) == 0x827;
            if (is_multiply && bit == 9)
                continue;
            auto bad = good;
            bad.words[2] ^= 1U << bit;
            if (!is_multiply && (bad.words[2] == 0 || bad.words[2] == (1U << 23) ||
                                 bad.words[2] == (2U << 23) || bad.words[2] == (6U << 23)))
                continue;
            reject(bad);
        }
    }
    for (u32 operation : {3U, 4U, 5U, 7U, 8U, 9U, 10U, 11U, 12U, 13U, 14U, 15U}) {
        auto bad = minimum;
        bad.words[2] = operation << 23;
        reject(bad);
    }
    for (const Native atomic : {minimum, maximum}) {
        for (u32 type = 1; type < 8; ++type) {
            auto bad = atomic;
            bad.words[2] |= type << 9;
            reject(bad);
        }
        for (u32 stride = 1; stride < 4; ++stride) {
            auto bad = atomic;
            bad.words[2] |= stride << 14;
            reject(bad);
        }
        for (u32 offset : {1U, 2U, 3U, 0xffffffU}) {
            auto bad = atomic;
            bad.words[1] = (offset << 8) | 11U;
            reject(bad);
        }
    }
    for (u32 addend : {0U, 1U, 5U, 245U}) {
        auto bad = multiply;
        bad.words[2] = (bad.words[2] & ~0xffU) | addend;
        reject(bad);
    }
    for (u32 reg : {246U, 250U, 254U}) {
        for (const Native good : {multiply, minimum, maximum}) {
            for (u32 shift : {16U, 24U}) {
                auto bad = good;
                bad.words[0] = (bad.words[0] & ~(0xffU << shift)) | (reg << shift);
                reject(bad);
            }
            if ((good.words[0] & 0xfff) == 0x38c) {
                auto bad = good;
                bad.words[1] = (bad.words[1] & ~0xffU) | reg;
                reject(bad);
            }
        }
    }
}
} // namespace
