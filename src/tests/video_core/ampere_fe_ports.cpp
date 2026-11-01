// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <bit>
#include <span>
#include <unordered_map>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"
#include "shader_recompiler/frontend/ir/modifiers.h"
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
    explicit Environment(std::span<const u64> words_) : words{words_} {}
    u64 ReadInstruction(u32 address) override {
        for (u32 i = 0; i < words.size(); ++i) {
            if (Address(i) == address) return words[i];
        }
        FAIL("Read outside synthetic instruction stream");
        return 0;
    }
    u32 ReadCbufValue(u32, u32) override { FAIL("Unexpected constant folding"); return 0; }
    Shader::TextureType ReadTextureType(u32) override { return Shader::TextureType::Color2D; }
    Shader::TexturePixelFormat ReadTexturePixelFormat(u32) override {
        return Shader::TexturePixelFormat::A8B8G8R8_UNORM;
    }
    bool IsTexturePixelFormatInteger(u32) override { return false; }
    u32 ReadViewportTransformState() override { return 1; }
    u32 TextureBoundBuffer() const override { return 7; }
    u32 LocalMemorySize() const override { return 0; }
    u32 SharedMemorySize() const override { return 0; }
    std::array<u32, 3> WorkgroupSize() const override { return {32, 1, 1}; }
    bool HasHLEMacroState() const override { return false; }
    std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32, u32) override { return {}; }
    void Dump(u64, u64) override {}

private:
    std::span<const u64> words;
};

struct Program {
    explicit Program(Native instruction) : lowered{Shader::Ampere::LowerToMaxwell(
                                              std::array{instruction, Exit})} {
        INFO(lowered.error);
        REQUIRE(lowered);
        Environment env{lowered.instructions};
        for (u32 i = 0; i + 1 < lowered.instructions.size(); ++i) {
            auto* block = blocks.Create(instructions);
            Shader::Maxwell::Translate(env, block, Address(i), Address(i + 1));
            steps.push_back(block);
        }
    }
    Shader::Ampere::LoweredProgram lowered;
    Shader::ObjectPool<IR::Inst> instructions;
    Shader::ObjectPool<IR::Block> blocks;
    std::vector<IR::Block*> steps;
};

struct Machine {
    using Vector = std::array<u64, 4>;
    using Lanes = std::array<Vector, 32>;
    std::array<std::array<u32, IR::NUM_REGS>, 32> registers{};
    std::array<std::array<bool, 7>, 32> predicates{};
    std::unordered_map<u64, u32> constants;
    std::vector<std::pair<u32, u32>> constant_reads;
    struct Sample {
        u32 offset{};
        Vector coordinates{};
        Vector derivatives{};
        Shader::TextureType type{};
    };
    std::vector<Sample> samples;
    const Vector sampled{0x3f000001, 0x3f000002, 0x3f000004, 0x3f000008};

    void Execute(const Program& program, u32 active = 1) {
        for (size_t step = 0; step < program.steps.size(); ++step) {
            const u32 guard = (program.lowered.instructions[step] >> 16) & 15;
            u32 executing{};
            for (u32 lane = 0; lane < 32; ++lane) {
                const bool condition = (guard & 7) == 7 || predicates[lane][guard & 7];
                if ((active & (1U << lane)) && (condition != ((guard & 8) != 0)))
                    executing |= 1U << lane;
            }
            std::unordered_map<const IR::Inst*, Lanes> values;
            const auto read = [&](const IR::Value& value, u32 lane) -> Vector {
                if (!value.IsImmediate()) return values.at(value.Inst())[lane];
                switch (value.Type()) {
                case IR::Type::U1: return {value.U1()};
                case IR::Type::U32: return {value.U32()};
                case IR::Type::U64: return {value.U64()};
                case IR::Type::F32: return {std::bit_cast<u32>(value.F32())};
                default: FAIL("Unexpected immediate type"); return {};
                }
            };
            for (const auto& inst : *program.steps[step]) {
                auto& results = values[&inst];
                for (u32 lane = 0; lane < 32; ++lane) {
                    const bool enabled = (executing & (1U << lane)) != 0;
                    auto& result = results[lane];
                    const auto vector = [&](size_t i) { return read(inst.Arg(i), lane); };
                    const auto arg = [&](size_t i) { return vector(i)[0]; };
                    const auto u = [&](size_t i) { return static_cast<u32>(arg(i)); };
                    const auto s = [&](size_t i) { return std::bit_cast<s32>(u(i)); };
                    switch (inst.GetOpcode()) {
                    case IR::Opcode::GetRegister: {
                        const u32 reg = static_cast<u32>(inst.Arg(0).Reg());
                        result[0] = reg == 255 ? 0 : registers[lane][reg];
                        break;
                    }
                    case IR::Opcode::SetRegister:
                        if (const u32 reg = static_cast<u32>(inst.Arg(0).Reg()); enabled && reg != 255)
                            registers[lane][reg] = u(1);
                        break;
                    case IR::Opcode::GetPred:
                        result[0] = predicates[lane][static_cast<u32>(inst.Arg(0).Pred())]; break;
                    case IR::Opcode::SetPred:
                        if (enabled) predicates[lane][static_cast<u32>(inst.Arg(0).Pred())] = arg(1) != 0;
                        break;
                    case IR::Opcode::GetCbufU32:
                        result[0] = constants.at((u64{u(0)} << 32) | u(1));
                        if (enabled) constant_reads.emplace_back(u(0), u(1));
                        break;
                    case IR::Opcode::Identity:
                    case IR::Opcode::BitCastF32U32:
                    case IR::Opcode::BitCastU32F32: result = vector(0); break;
                    case IR::Opcode::SelectU1:
                    case IR::Opcode::SelectU32: result = vector(arg(0) ? 1 : 2); break;
                    case IR::Opcode::UMin32: result[0] = std::min(u(0), u(1)); break;
                    case IR::Opcode::UMax32: result[0] = std::max(u(0), u(1)); break;
                    case IR::Opcode::SMin32: result[0] = static_cast<u32>(std::min(s(0), s(1))); break;
                    case IR::Opcode::SMax32: result[0] = static_cast<u32>(std::max(s(0), s(1))); break;
                    case IR::Opcode::IAdd32: result[0] = u(0) + u(1); break;
                    case IR::Opcode::ISub32: result[0] = u(0) - u(1); break;
                    case IR::Opcode::INeg32: result[0] = 0U - u(0); break;
                    case IR::Opcode::BitwiseAnd32: result[0] = u(0) & u(1); break;
                    case IR::Opcode::BitwiseOr32: result[0] = u(0) | u(1); break;
                    case IR::Opcode::BitwiseXor32: result[0] = u(0) ^ u(1); break;
                    case IR::Opcode::BitwiseNot32: result[0] = ~u(0); break;
                    case IR::Opcode::LogicalNot: result[0] = !arg(0); break;
                    case IR::Opcode::IEqual: result[0] = u(0) == u(1); break;
                    case IR::Opcode::INotEqual: result[0] = u(0) != u(1); break;
                    case IR::Opcode::ULessThan: result[0] = u(0) < u(1); break;
                    case IR::Opcode::SLessThan: result[0] = s(0) < s(1); break;
                    case IR::Opcode::ShiftLeftLogical32: result[0] = u(1) < 32 ? u(0) << u(1) : 0; break;
                    case IR::Opcode::ShiftRightLogical32: result[0] = u(1) < 32 ? u(0) >> u(1) : 0; break;
                    case IR::Opcode::ShiftLeftLogical64: result[0] = arg(1) < 64 ? arg(0) << arg(1) : 0; break;
                    case IR::Opcode::ShiftRightLogical64: result[0] = arg(1) < 64 ? arg(0) >> arg(1) : 0; break;
                    case IR::Opcode::BitFieldUExtract:
                        result[0] = (arg(0) >> arg(1)) & ((u64{1} << arg(2)) - 1); break;
                    case IR::Opcode::CompositeConstructU32x2:
                    case IR::Opcode::CompositeConstructF32x2:
                    case IR::Opcode::CompositeConstructF32x3:
                    case IR::Opcode::CompositeConstructF32x4:
                        for (size_t i = 0; i < inst.NumArgs(); ++i) result[i] = arg(i);
                        break;
                    case IR::Opcode::CompositeExtractU32x2:
                    case IR::Opcode::CompositeExtractF32x4: result[0] = vector(0)[u(1)]; break;
                    case IR::Opcode::PackUint2x32: result[0] = vector(0)[0] | (vector(0)[1] << 32); break;
                    case IR::Opcode::UnpackUint2x32: result = {u(0), arg(0) >> 32}; break;
                    case IR::Opcode::ConvertF32U16:
                        result[0] = std::bit_cast<u32>(static_cast<float>(u(0) & 0xffff)); break;
                    case IR::Opcode::SubgroupBallot:
                    case IR::Opcode::VoteAny: {
                        u32 ballot{};
                        for (u32 other = 0; other < 32; ++other)
                            if ((executing & (1U << other)) && read(inst.Arg(0), other)[0]) ballot |= 1U << other;
                        result[0] = inst.GetOpcode() == IR::Opcode::VoteAny ? ballot != 0 : ballot;
                        break;
                    }
                    case IR::Opcode::ShuffleIndex:
                        REQUIRE(u(2) == 31);
                        REQUIRE(u(3) == 0);
                        result = read(inst.Arg(0), u(1) & 31);
                        break;
                    case IR::Opcode::GetInBoundsFromOp: result[0] = true; break;
                    case IR::Opcode::BoundImageGradient: {
                        const auto info = inst.Flags<IR::TextureInstInfo>();
                        REQUIRE(info.num_derivatives == 2);
                        REQUIRE_FALSE(info.has_lod_clamp);
                        REQUIRE(inst.Arg(3).IsEmpty());
                        REQUIRE(inst.Arg(4).IsEmpty());
                        if (enabled) samples.push_back({u(0), vector(1), vector(2), info.type.Value()});
                        result = sampled;
                        break;
                    }
                    default:
                        FAIL("Unsupported opcode in FE regression interpreter: " << static_cast<u32>(inst.GetOpcode()));
                    }
                }
            }
        }
    }
};

Native Gradient(u32 mask, u32 destination = 26, u32 high = 255, u32 dimension = 1,
                u32 predicate = 7, u32 coordinates = 4, u32 derivatives = 12) {
    return {{0xb6cU | (predicate << 12) | (destination << 16) | (coordinates << 24),
             derivatives | (32U << 8) | (7U << 22) | (dimension << 29),
             0x001e0000U | (mask << 8) | high, 0}};
}

TEST_CASE("FE TXD masks preserve sampled channels coordinates gradients and split outputs",
          "[video_core][ampere][fe_ports]") {
    const u32 mask = GENERATE(1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 9U, 10U, 11U, 12U, 13U, 14U, 15U);
    const u32 dimension = GENERATE(1U, 5U);
    const u32 destination = GENERATE(4U, 12U, 26U);
    const bool split = GENERATE(false, true);
    const u32 guard = GENERATE(0U, 7U, 8U);
    const bool bit90 = GENERATE(false, true);
    const u32 channels = std::popcount(mask);
    const u32 high = channels > 2 ? (split ? 45U - (channels == 4) : destination + 2) : 255U;
    auto instruction = Gradient(mask, destination, high, dimension, guard);
    instruction.words[2] |= u32{bit90} << 26;
    Program program{instruction};
    REQUIRE(program.lowered.required_texture_buffer == 7);
    REQUIRE(((program.lowered.instructions.front() >> 49) & 1) == bit90);
    for (bool predicate : {false, true}) {
        Machine machine;
        machine.registers[0].fill(0xdeadbeef);
        for (u32 i = 0; i < 2; ++i) machine.registers[0][4 + i] = std::bit_cast<u32>(0.25f + i);
        machine.registers[0][6] = 0xabcd0003;
        for (u32 i = 0; i < 4; ++i) machine.registers[0][12 + i] = std::bit_cast<u32>(2.5f + i);
        machine.predicates[0][0] = predicate;
        const auto before = machine.registers[0];
        const bool executes = guard == 7 || (predicate != (guard == 8));
        machine.Execute(program);
        auto expected = before;
        if (executes) {
            u32 output{};
            for (u32 component = 0; component < 4; ++component) {
                if (!(mask & (1U << component))) continue;
                expected[output < 2 ? destination + output : high + output - 2] = static_cast<u32>(machine.sampled[component]);
                ++output;
            }
        }
        for (u32 reg = 0; reg < 246; ++reg) REQUIRE(machine.registers[0][reg] == expected[reg]);
        REQUIRE(machine.predicates[0][0] == predicate);
        REQUIRE(machine.samples.size() == (executes ? 1U : 0U));
        if (executes) {
            const auto& sample = machine.samples.front();
            REQUIRE(sample.offset == 128);
            REQUIRE(sample.type == (dimension == 5 ? Shader::TextureType::ColorArray2D : Shader::TextureType::Color2D));
            REQUIRE(sample.coordinates[0] == before[4]);
            REQUIRE(sample.coordinates[1] == before[5]);
            if (dimension == 5) REQUIRE(sample.coordinates[2] == std::bit_cast<u32>(3.0f));
            for (u32 i = 0; i < 4; ++i) REQUIRE(sample.derivatives[i] == before[12 + i]);
        }
    }
}

TEST_CASE("FE TXD rejects malformed masks destinations banks and register spans",
          "[video_core][ampere][fe_ports]") {
    const auto accepts = [](Native instruction, bool uniform = false) {
        std::vector code{instruction};
        if (uniform) code.push_back({{0x03057c17, 4, 0x0b800000, 0}});
        code.push_back(Exit);
        return static_cast<bool>(Shader::Ampere::LowerToMaxwell(code));
    };
    for (const auto instruction : {Gradient(0), Gradient(3, 27), Gradient(3, 26, 44),
                                   Gradient(7, 26, 255), Gradient(15, 26, 255),
                                   Gradient(15, 26, 45), Gradient(1, 255), Gradient(3, 26, 255, 2)})
        REQUIRE_FALSE(accepts(instruction));
    for (bool uniform : {false, true}) {
        const u32 limit = uniform ? 216 : 246;
        REQUIRE(accepts(Gradient(1, limit - 1, 255, 1, 7, limit - 2, limit - 4), uniform));
        REQUIRE(accepts(Gradient(7, 26, limit - 1, 5, 7, limit - 3, limit - 4), uniform));
        REQUIRE_FALSE(accepts(Gradient(1, 26, 255, 1, 7, limit - 1, 12), uniform));
        REQUIRE_FALSE(accepts(Gradient(1, 26, 255, 5, 7, limit - 2, 12), uniform));
        REQUIRE_FALSE(accepts(Gradient(1, 26, 255, 1, 7, 4, limit - 3), uniform));
    }
    for (u32 bit : {12U, 20U, 27U}) {
        auto invalid = Gradient(3);
        invalid.words[2] ^= 1U << bit;
        REQUIRE_FALSE(accepts(invalid));
    }
    auto invalid_bank = Gradient(3);
    invalid_bank.words[1] = (invalid_bank.words[1] & ~(31U << 22)) | (18U << 22);
    REQUIRE_FALSE(accepts(invalid_bank));
    auto invalid_offset = Gradient(3);
    invalid_offset.words[1] |= 0x2000U << 8;
    REQUIRE_FALSE(accepts(invalid_offset));
    auto other_bank = Gradient(3);
    other_bank.words[1] ^= (7U ^ 8U) << 22;
    const std::array mixed{Gradient(3), other_bank, Exit};
    REQUIRE_FALSE(Shader::Ampere::LowerToMaxwell(mixed));
}

TEST_CASE("FE full-width IMNMX produces signed and unsigned extrema without alias corruption",
          "[video_core][ampere][fe_ports]") {
    const u32 immediate = GENERATE(0U, 0x7ffffU, 0x80000U, 0x7fffffffU, 0x80000000U, 0xffffffc0U, 0xffffffffU);
    const bool sign = GENERATE(false, true);
    const bool maximum = GENERATE(false, true);
    const u32 destination = GENERATE(3U, 8U);
    const u32 guard = GENERATE(0U, 7U, 8U);
    Program program{Native{{0x817U | (guard << 12) | (destination << 16) | (3U << 24),
                            immediate, 0x03800000U | (u32{sign} << 9) | (u32{maximum} << 26), 0}}};
    for (u32 source : {0U, 0x7ffffU, 0x80000000U, 0xffffffbfU, 0xffffffffU}) {
        for (bool predicate : {false, true}) {
            Machine machine;
            machine.registers[0].fill(0xdeadbeef);
            machine.registers[0][3] = source;
            machine.predicates[0][0] = predicate;
            auto expected = machine.registers[0];
            const bool executes = guard == 7 || (predicate != (guard == 8));
            if (executes) {
                const bool smaller = sign ? std::bit_cast<s32>(source) < std::bit_cast<s32>(immediate) : source < immediate;
                expected[destination] = (smaller != maximum) ? source : immediate;
            }
            machine.Execute(program);
            for (u32 reg = 0; reg < 246; ++reg) REQUIRE(machine.registers[0][reg] == expected[reg]);
            REQUIRE(machine.predicates[0][0] == predicate);
        }
    }
}

Native Shift(u32 opcode, u32 form, bool wrap, u32 destination = 3, u32 predicate = 7) {
    const bool right = form != 0;
    const bool high_result = form == 2;
    return {{opcode | (predicate << 12) | (destination << 16) | ((high_result ? 255U : 3U) << 24),
             opcode == 0xa19 ? (3U << 22) | (0x80U << 6) : 4U,
             0x600U | (right ? 0x1000U : 0U) | (high_result ? 0x10000U : 0U) |
                 (wrap ? 0x800U : 0U) | (high_result ? 3U : 255U) | (opcode == 0xc19 ? 0x08000000U : 0U), 0}};
}

TEST_CASE("FE cbuf SHF preserves count wrapping direction predication and aliased input",
          "[video_core][ampere][fe_ports]") {
    const u32 opcode = GENERATE(0xa19U, 0x219U, 0xc19U);
    const u32 form = GENERATE(0U, 1U, 2U);
    const bool wrap = GENERATE(false, true);
    const u32 destination = GENERATE(3U, 8U);
    const u32 guard = GENERATE(0U, 7U, 8U);
    Program program{Shift(opcode, form, wrap, destination, guard)};
    for (u32 count : {0U, 1U, 31U, 32U, 33U, 63U, 0xffffffffU}) {
        for (bool predicate : {false, true}) {
            Machine machine;
            machine.registers[0].fill(0xdeadbeef);
            machine.registers[0][3] = 0x81234567;
            machine.registers[0][opcode == 0xc19 ? 220 : 4] = count;
            machine.constants[(u64{3} << 32) | 0x80] = count;
            machine.predicates[0][0] = predicate;
            auto expected = machine.registers[0];
            const bool executes = guard == 7 || (predicate != (guard == 8));
            const bool wraps = wrap && (opcode == 0xa19 || form != 1);
            const u32 shift = wraps ? count & 31 : std::min(count, 32U);
            if (executes) expected[destination] = shift == 32 ? 0 : form == 0 ? 0x81234567U << shift : 0x81234567U >> shift;
            machine.Execute(program);
            for (u32 reg = 0; reg < (opcode == 0xc19 ? 216U : 246U); ++reg)
                REQUIRE(machine.registers[0][reg] == expected[reg]);
            REQUIRE(machine.predicates[0][0] == predicate);
            if (opcode == 0xa19 && executes) {
                REQUIRE(machine.constant_reads.size() == 1);
                REQUIRE(machine.constant_reads.front() == std::pair<u32, u32>{3, 0x80});
            }
        }
    }
}

TEST_CASE("FE integer ports reject invalid cbuf modifiers scratch aliases and reduction guards",
          "[video_core][ampere][fe_ports]") {
    const auto reject = [](Native instruction) {
        REQUIRE_FALSE(Shader::Ampere::LowerToMaxwell(std::array{instruction, Exit}));
    };
    for (u32 bit : {6U, 7U, 30U, 31U}) {
        auto instruction = Shift(0xa19, 0, false);
        instruction.words[1] |= 1U << bit;
        reject(instruction);
    }
    auto bank = Shift(0xa19, 0, false);
    bank.words[1] = (18U << 22) | (0x80U << 6);
    reject(bank);
    for (u32 bit : {8U, 12U, 27U}) {
        Native instruction{{0x03087817, 0xffffffc0, 0x03800000U | (1U << bit), 0}};
        reject(instruction);
    }
    for (u32 destination : {28U, 62U, 63U})
        reject({{0x030073c4U | (destination << 16), 0, 0x10000, 0}});
    for (u32 predicate : {0U, 6U, 8U, 14U})
        reject({{0x030503c4U | (predicate << 12), 0, 0x10000, 0}});
    reject({{0x030573c4, 1, 0x10000, 0}});
    reject({{0x030573c4, 0, 0x10001, 0}});
    reject({{0xf6087817, 0xffffffc0, 0x03800000, 0}});
    auto scratch = Shift(0xa19, 0, false);
    scratch.words[0] = (scratch.words[0] & 0x00ffffff) | (246U << 24);
    reject(scratch);
}

TEST_CASE("FE REDUX MIN excludes inactive lanes and preserves existing reductions",
          "[video_core][ampere][fe_ports]") {
    const u32 mode = GENERATE(0x10000U, 0x14000U, 0xc000U, 0x4000U);
    const u32 destination = GENERATE(0U, 21U, 22U, 27U);
    const bool all_ones = GENERATE(false, true);
    const u32 mapped = destination < 22 ? 216 + destination : 218 + destination;
    Program program{Native{{0x030073c4U | (destination << 16), 0, mode, 0}}};
    for (u32 active : {0U, 1U, 0x80000000U, 0x80000001U, 0x55555555U, 0xffffffffU}) {
        Machine machine;
        u32 expected = mode == 0x10000 ? ~0U : 0U;
        for (u32 lane = 0; lane < 32; ++lane) {
            machine.registers[lane].fill(0xcafebeef);
            machine.registers[lane][3] = (active & (1U << lane))
                                            ? (all_ones ? ~0U : 0x80000001U + lane * 17)
                                            : 0;
            if (active & (1U << lane)) {
                const u32 source = machine.registers[lane][3];
                if (mode == 0x10000) expected = std::min(expected, source);
                else if (mode == 0x14000) expected = std::max(expected, source);
                else if (mode == 0x4000) expected |= source;
                else expected += source;
            }
            for (u32 p = 0; p < 7; ++p) machine.predicates[lane][p] = ((lane + p) & 1) != 0;
        }
        const auto predicates = machine.predicates;
        const auto registers = machine.registers;
        machine.Execute(program, active);
        REQUIRE(machine.predicates == predicates);
        for (u32 lane = 0; lane < 32; ++lane) {
            REQUIRE(machine.registers[lane][mapped] == ((active & (1U << lane)) ? expected : 0xcafebeef));
            for (u32 reg = 0; reg < 246; ++reg) {
                if (reg != mapped) REQUIRE(machine.registers[lane][reg] == registers[lane][reg]);
            }
        }
    }
}
}
