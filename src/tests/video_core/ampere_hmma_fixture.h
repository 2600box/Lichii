// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "shader_recompiler/environment.h"
#include "shader_recompiler/frontend/ampere/hmma_program.h"
#include "shader_recompiler/frontend/maxwell/translate_program.h"
#include "shader_recompiler/host_translate_info.h"

namespace HmmaTest {

inline std::vector<Shader::Ampere::Instruction> NativeProgram(
    std::array<u32, 4> hmma = {0x0810723c, 12, 0x83c, 0x004fde00}, u32 count = 1) {
    using Shader::Ampere::Instruction;
    const u32 a = hmma[0] >> 24;
    const u32 b = hmma[1] & 255;
    const u32 c = (hmma[2] & 255) == 255 ? 60 : hmma[2] & 255;
    const u32 d = (hmma[0] >> 16) & 255;
    std::vector<Instruction> code{
        {{0x00647919, 0, 0x2100, 0x000e2200}},
        {{0x00657802, 32, 0xf00, 0x000fe200}},
        {{0x00677919, 0, 0x2500, 0x000e2400}},
        {{0x67647211, 100, 0x078e38ff, 0x001fca00}},
        {{0x006c7a02, 0x5800, 0xf00, 0x000fe200}},
        {{0x006d7a02, 0x5900, 0xf00, 0x000fe200}},
        {{0x64667225, 101, 0x078e006c, 0x000fca00}},
        {{0x66007381 | (a << 16), 0, 0x001e8d00, 0x000ea800}},
        {{0x66007381 | (b << 16), 0x1000, 0x001e8b00, 0x000ea200}},
        {{0x66007381 | (c << 16), 0x1800, 0x001e8b00, 0x000ea200}},
        {{0x00657802, 8, 0xf00, 0x000fca00}},
        {{0x006e7a02, 0x5c00, 0xf00, 0x000fe200}},
        {{0x006f7a02, 0x5d00, 0xf00, 0x000fe200}},
        {{0x64687225, 101, 0x078e006e, 0x000fe200}},
    };
    for (u32 i = 0; i < count; ++i) {
        if (i != 0) hmma[2] = (hmma[2] & ~255U) | d;
        code.push_back({hmma});
    }
    code.push_back({{0x68007386, d, 0x0010ab00, 0x000fe200}});
    code.push_back({{0x0000794d, 0, 0x03800000, 0x000fea00}});
    return code;
}

inline std::vector<Shader::Ampere::Instruction> DiamondProgram(bool nested = false) {
    using Shader::Ampere::Instruction;
    const auto branch = [](u32 from, u32 to, u32 predicate = 7) -> Instruction {
        return {{0x947U | (predicate << 12), (to - from - 1) * 16, 0x03800000,
                 0x000fea00}};
    };
    const auto set_sync = [](u32 from, u32 to, u32 token = 0) -> Instruction {
        return {{0x7945U | (token << 16), (to - from - 1) * 16, 0x03800000,
                 0x000fea00}};
    };
    const auto sync = [](u32 token = 0) -> Instruction {
        return {{0x7941U | (token << 16), 0, 0x03800000, 0x000fea00}};
    };
    const Instruction move_a{{0x00087202, 9, 0xf00, 0x000fea00}};
    const Instruction move_b{{0x00087202, 10, 0xf00, 0x000fea00}};
    std::vector<Instruction> selection;
    if (nested) {
        selection = {set_sync(0, 12), branch(1, 10, 0), set_sync(2, 9, 1),
                     branch(3, 6, 0), move_a, branch(5, 8), move_b, move_a,
                     sync(1), branch(9, 11), move_b, sync()};
    } else {
        selection = {set_sync(0, 7), branch(1, 4, 0), move_a, branch(3, 6),
                     move_b, move_b, sync()};
    }
    selection.insert(selection.begin(), {{0x6400720c, 255, 0x03f02070, 0x000fea00}});
    auto code = NativeProgram();
    code.insert(code.begin() + 14, selection.begin(), selection.end());
    return code;
}

class Environment final : public Shader::Environment {
public:
    explicit Environment(std::span<const Shader::Ampere::Instruction> native) {
        stage = Shader::Stage::Compute;
        start_address = 0x50;
        program = Shader::Ampere::BuildHmmaProgram(native, std::array<u64, 10>{}, 0,
                                                   stage, {128, 1, 1}, true);
    }

    Shader::Ampere::HmmaProgram program;

    bool UsesHmma() const override { return true; }

    u64 ReadInstruction(u32 address) override {
        Shader::Ampere::ValidateHmmaFetch(program, address);
        if (Shader::Ampere::FindHmmaSite(program, address) ||
            program.code.at(address / 8) == Shader::Ampere::HmmaGuard) {
            throw Shader::HmmaRejected("Raw HMMA fetch");
        }
        return program.code.at(address / 8);
    }
    Shader::FetchedInstruction FetchInstruction(u32 address) override {
        Shader::Ampere::ValidateHmmaFetch(program, address);
        if (const auto* site = Shader::Ampere::FindHmmaSite(program, address)) {
            return site->instruction;
        }
        return Shader::MaxwellInstructionWord{ReadInstruction(address)};
    }
    u32 ReadCbufValue(u32, u32) override { return 0; }
    Shader::TextureType ReadTextureType(u32) override { throw Shader::HmmaRejected("Unexpected texture"); }
    Shader::TexturePixelFormat ReadTexturePixelFormat(u32) override { throw Shader::HmmaRejected("Unexpected texture"); }
    bool IsTexturePixelFormatInteger(u32) override { return false; }
    u32 ReadViewportTransformState() override { return 0; }
    u32 TextureBoundBuffer() const override { return 0; }
    u32 LocalMemorySize() const override { return 0; }
    u32 SharedMemorySize() const override { return 0; }
    std::array<u32, 3> WorkgroupSize() const override { return {128, 1, 1}; }
    bool HasHLEMacroState() const override { return false; }
    std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32, u32) override { return {}; }
    void Dump(u64, u64) override {}
};

struct Compiler {
    Shader::ObjectPool<Shader::IR::Inst> instructions;
    Shader::ObjectPool<Shader::IR::Block> blocks;
    Shader::ObjectPool<Shader::Maxwell::Flow::Block> flow_blocks;

    Shader::IR::Program Translate(Environment& env) {
        Shader::Maxwell::Flow::CFG cfg{env, flow_blocks, env.StartAddress()};
        const Shader::HostTranslateInfo host{
            .min_ssbo_alignment = 1,
            .support_float64 = true,
            .support_float16 = true,
            .support_int64 = true,
            .support_conditional_barrier = true,
        };
        return Shader::Maxwell::TranslateProgram(instructions, blocks, env, cfg, host);
    }
};

}
