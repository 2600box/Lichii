// SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "shader_recompiler/environment.h"
#include "shader_recompiler/frontend/ir/basic_block.h"
#include "shader_recompiler/frontend/maxwell/decode.h"
#include "shader_recompiler/frontend/maxwell/location.h"
#include "shader_recompiler/frontend/maxwell/translate/impl/impl.h"
#include "shader_recompiler/frontend/maxwell/translate/translate.h"

namespace Shader::Maxwell {

void Translate(Environment& env, IR::Block* block, u32 location_begin, u32 location_end) {
    if (location_begin != location_end) {
        TranslatorVisitor visitor{env, *block};
        for (Location pc = location_begin; pc != location_end; ++pc) {
            const auto fetched = env.FetchInstruction(pc.Offset());
            if (const auto* hmma = std::get_if<Ampere::Hmma16816F16>(&fetched)) {
                auto& ir = visitor.ir;
                const auto reg = [](u32 index) { return static_cast<IR::Reg>(index); };
                const auto a{ir.CompositeConstruct(visitor.X(reg(hmma->a)),
                    visitor.X(reg(hmma->a + 1)), visitor.X(reg(hmma->a + 2)),
                    visitor.X(reg(hmma->a + 3)))};
                const auto b{ir.CompositeConstruct(visitor.X(reg(hmma->b)),
                    visitor.X(reg(hmma->b + 1)))};
                const auto c{ir.CompositeConstruct(hmma->c == 255 ? ir.Imm32(0U) : visitor.X(reg(hmma->c)),
                    hmma->c == 255 ? ir.Imm32(0U) : visitor.X(reg(hmma->c + 1)))};
                const auto result = ir.Hmma16816F16(a, b, c);
                visitor.X(reg(hmma->destination), IR::U32{ir.CompositeExtract(result, 0)});
                visitor.X(reg(hmma->destination + 1), IR::U32{ir.CompositeExtract(result, 1)});
                continue;
            }
            const u64 insn = std::get<MaxwellInstructionWord>(fetched).value;
            Opcode const opcode = Decode(insn);
            switch (opcode) {
#define INST(name, cute, mask) case Opcode::name: visitor.name(insn); break;
#include "shader_recompiler/frontend/maxwell/maxwell.inc"
#undef OPCODE
            }
        }
    }
}

} // namespace Shader::Maxwell
