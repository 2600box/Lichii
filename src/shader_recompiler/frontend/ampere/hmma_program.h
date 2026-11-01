// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <istream>
#include <optional>
#include <ostream>
#include <span>
#include <vector>

#include "shader_recompiler/exception.h"
#include "shader_recompiler/frontend/ampere/hmma.h"
#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"
#include "shader_recompiler/frontend/ampere/program_address_map.h"
#include "shader_recompiler/frontend/ampere/program_override.h"
#include "shader_recompiler/stage.h"

namespace Shader::Ampere {

inline constexpr u32 HmmaProgramRevision = 1;
inline constexpr u32 HmmaMetadataLimit = 65536;
inline constexpr u64 HmmaGuard = HmmaGuardInstruction;

struct HmmaProgramSite {
    u32 address;
    u32 native_offset;
    Hmma16816F16 instruction;
};

struct HmmaProgram {
    std::vector<u64> code;
    std::vector<Instruction> native;
    std::vector<HmmaProgramSite> sites;
    u32 source_header_size{};
    u32 source_size{};
    u32 executable_end{};
    u32 texture_bound = ~0U;
    u64 native_digest{};
};

inline bool ContainsHmma(std::span<const Instruction> native) {
    return std::ranges::any_of(native, [](const Instruction& instruction) {
        return instruction.Bits(0, 12) == 0x23c && instruction.Bits(12, 4) != 15;
    });
}

inline void ValidateHmmaLaunch(Stage stage, std::array<u32, 3> workgroup, bool enabled) {
    if (!enabled || stage != Stage::Compute || workgroup != std::array<u32, 3>{128, 1, 1}) {
        throw HmmaRejected("HMMA requires experimental opt-in and a 128x1x1 compute launch");
    }
}

inline HmmaProgram BuildHmmaProgram(std::span<const Instruction> native,
                                    std::span<const u64> header, u32 source_header_size,
                                    Stage stage, std::array<u32, 3> workgroup, bool enabled) {
    ValidateHmmaLaunch(stage, workgroup, enabled);
    if (header.size_bytes() != 0x50 || source_header_size != 0 || native.empty() ||
        native.size() > HmmaMetadataLimit) {
        throw HmmaRejected("Invalid HMMA native program extent");
    }
    const auto lowered = LowerToMaxwell(native, false, true);
    if (!lowered) {
        throw HmmaRejected("HMMA native program rejected at {:#x}: {}", lowered.error_offset,
                           lowered.error);
    }
    if (lowered.hmma_sites.empty() || lowered.source_size != native.size_bytes() ||
        lowered.source_instruction_offsets.size() != native.size()) {
        throw HmmaRejected("Incomplete HMMA native program provenance");
    }
    HmmaProgram result;
    result.native.assign(native.begin(), native.end());
    result.source_header_size = source_header_size;
    result.source_size = source_header_size + lowered.source_size;
    result.native_digest = NativeProgramKey(native);
    result.texture_bound = lowered.required_texture_buffer;
    result.code.assign(header.begin(), header.end());
    for (const u64 instruction : lowered.instructions) {
        if (result.code.size() % 4 == 0) {
            result.code.push_back(0);
        }
        result.code.push_back(instruction);
    }
    result.executable_end = static_cast<u32>(result.code.size() * sizeof(u64));
    result.code.push_back(0);
    AppendNativeAddressMap(result.code, lowered.source_instruction_offsets);
    for (const auto& site : lowered.hmma_sites) {
        const u32 address = 0x50U + site.lowered_instruction_index * 8 +
                            ((site.lowered_instruction_index + 1) / 3) * 8;
        if (site.native_offset % 16 || site.native_offset / 16 >= native.size() ||
            native[site.native_offset / 16].words != site.native_words ||
            address >= result.executable_end || result.code[address / 8] != HmmaGuard ||
            NativeProgramAddress(result.code, site.native_offset) != address ||
            (!result.sites.empty() && result.sites.back().address >= address)) {
            throw HmmaRejected("Invalid HMMA lowered site provenance");
        }
        const auto decoded = DecodeHmma(site.native_words, 246);
        if (!decoded) {
            throw HmmaRejected("Invalid HMMA site encoding: {}", decoded.error);
        }
        result.sites.push_back({address, site.native_offset, decoded.instruction});
    }
    return result;
}

inline const HmmaProgramSite* FindHmmaSite(const HmmaProgram& program, u32 address) {
    const auto it = std::ranges::lower_bound(program.sites, address, {}, &HmmaProgramSite::address);
    return it != program.sites.end() && it->address == address ? &*it : nullptr;
}

inline void ValidateHmmaFetch(const HmmaProgram& program, u32 address) {
    if (address < 0x50 || address >= program.executable_end || address % 8 || address % 32 == 0) {
        throw HmmaRejected("HMMA instruction fetch outside executable code at {:#x}", address);
    }
}

inline void WriteHmmaProgram(std::ostream& stream, const HmmaProgram* program) {
    const auto write32 = [&](u32 value) {
        std::array<char, 4> bytes{};
        for (u32 i = 0; i < 4; ++i) {
            bytes[i] = static_cast<char>(value >> (8 * i));
        }
        stream.write(bytes.data(), bytes.size());
    };
    write32(program ? HmmaProgramRevision : 0);
    if (!program) {
        return;
    }
    if (program->native.size() > HmmaMetadataLimit || program->sites.size() > HmmaMetadataLimit) {
        throw HmmaRejected("HMMA metadata exceeds cache bounds");
    }
    write32(program->source_header_size);
    write32(program->source_size);
    write32(program->executable_end);
    write32(static_cast<u32>(program->native_digest));
    write32(static_cast<u32>(program->native_digest >> 32));
    write32(static_cast<u32>(program->native.size()));
    write32(static_cast<u32>(program->sites.size()));
    for (const auto& instruction : program->native) {
        for (u32 word : instruction.words) {
            write32(word);
        }
    }
    for (const auto& site : program->sites) {
        write32(site.address);
        write32(site.native_offset);
    }
    if (!stream) {
        throw HmmaRejected("Failed to write HMMA metadata");
    }
}

inline std::optional<HmmaProgram> ReadHmmaProgram(std::istream& stream,
                                                std::span<const u64> cached_code, Stage stage,
                                                std::array<u32, 3> workgroup, bool enabled) try {
    const auto read32 = [&]() {
        std::array<unsigned char, 4> bytes{};
        stream.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
        if (!stream) {
            throw HmmaRejected("Truncated HMMA metadata");
        }
        return u32{bytes[0]} | (u32{bytes[1]} << 8) | (u32{bytes[2]} << 16) |
               (u32{bytes[3]} << 24);
    };
    const u32 revision = read32();
    if (!revision) {
        if (std::ranges::find(cached_code, HmmaGuard) != cached_code.end()) {
            throw HmmaRejected("Missing HMMA metadata for guarded instruction");
        }
        return std::nullopt;
    }
    ValidateHmmaLaunch(stage, workgroup, enabled);
    if (revision != HmmaProgramRevision || cached_code.size() < 10) {
        throw HmmaRejected("Invalid HMMA cache revision or header");
    }
    const u32 source_header_size = read32();
    const u32 source_size = read32();
    const u32 executable_end = read32();
    const u64 digest_low = read32();
    const u64 digest = digest_low | (u64{read32()} << 32);
    const u32 native_count = read32();
    const u32 site_count = read32();
    if (!native_count || native_count > HmmaMetadataLimit || !site_count ||
        site_count > native_count) {
        throw HmmaRejected("Invalid HMMA cache metadata counts");
    }
    std::vector<Instruction> native(native_count);
    for (auto& instruction : native) {
        for (u32& word : instruction.words) {
            word = read32();
        }
    }
    auto program = BuildHmmaProgram(native, cached_code.first(10), source_header_size, stage,
                                    workgroup, enabled);
    if (program.source_size != source_size || program.executable_end != executable_end ||
        program.native_digest != digest || program.sites.size() != site_count ||
        !std::ranges::equal(program.code, cached_code)) {
        throw HmmaRejected("HMMA cached code disagrees with native program");
    }
    for (const auto& site : program.sites) {
        const u32 address = read32();
        const u32 native_offset = read32();
        if (site.address != address || site.native_offset != native_offset) {
            throw HmmaRejected("HMMA cached site disagrees with native program");
        }
    }
    return program;
} catch (const std::ios_base::failure&) {
    throw HmmaRejected("Truncated HMMA cache metadata");
}

} // namespace Shader::Ampere
