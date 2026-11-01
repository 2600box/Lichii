// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <cstdlib>
#include <span>
#include <string_view>
#include <vector>

#include "common/common_types.h"

namespace Shader::Ampere {

struct Hmma16816F16 {
    u32 destination;
    u32 a;
    u32 b;
    u32 c;
};

struct HmmaDecodeResult {
    Hmma16816F16 instruction{};
    std::string_view error;

    explicit operator bool() const {
        return error.empty();
    }
};

struct HmmaValidationResult {
    u32 error_offset{};
    std::string_view error;

    explicit operator bool() const {
        return error.empty();
    }
};

inline bool ExperimentalHmmaEnabled() {
    const char* value = std::getenv("EDEN_NX2_HMMA_EXPERIMENTAL");
    return value && std::string_view{value} == "1";
}

inline HmmaDecodeResult DecodeHmma(const std::array<u32, 4>& words, u32 register_limit) {
    if (register_limit != 216 && register_limit != 246) {
        return {{}, "HMMA register reservation limit"};
    }
    if ((words[0] & 0xffffU) != 0x723cU || (words[1] & 0xffffff00U) != 0 ||
        (words[2] & 0xffffff00U) != 0x800U || (words[3] & 0xc00001ffU) != 0) {
        return {{}, "HMMA requires unpredicated dense 16816 F16 encoding"};
    }
    const Hmma16816F16 decoded{(words[0] >> 16) & 0xffU, words[0] >> 24,
                              words[1] & 0xffU, words[2] & 0xffU};
    const auto valid_span = [register_limit](u32 first, u32 count) {
        return first % count == 0 && first + count <= register_limit;
    };
    if (!valid_span(decoded.destination, 2) || !valid_span(decoded.a, 4) ||
        !valid_span(decoded.b, 2) || (decoded.c != 255 && !valid_span(decoded.c, 2))) {
        return {{}, "HMMA unaligned or reserved register span"};
    }
    return {decoded, {}};
}

namespace HmmaDetail {

inline bool IsFallthroughOpcode(u32 opcode) {
    switch (opcode) {
    case 0x092:
    case 0x202: case 0x207: case 0x208: case 0x209: case 0x20a: case 0x20b: case 0x20c:
    case 0x210: case 0x211: case 0x212: case 0x213: case 0x217: case 0x219: case 0x220:
    case 0x221: case 0x223: case 0x224: case 0x225: case 0x230: case 0x231: case 0x232:
    case 0x234: case 0x23a: case 0x23e: case 0x240: case 0x245: case 0x287: case 0x28c:
    case 0x290: case 0x291: case 0x292: case 0x299: case 0x2a4: case 0x2a5: case 0x2bd:
    case 0x2be: case 0x300: case 0x301: case 0x304:
    case 0x305: case 0x306: case 0x307: case 0x308: case 0x309: case 0x361: case 0x381:
    case 0x386: case 0x387: case 0x388: case 0x38c: case 0x3a1: case 0x3a8: case 0x3c2:
    case 0x3c4: case 0x419: case 0x421: case 0x423: case 0x424: case 0x430: case 0x431:
    case 0x434: case 0x589: case 0x59f: case 0x621: case 0x623: case 0x624: case 0x630:
    case 0x631: case 0x802: case 0x803: case 0x804: case 0x805: case 0x806: case 0x807:
    case 0x808: case 0x809: case 0x80a: case 0x80b: case 0x80c: case 0x80d: case 0x810:
    case 0x811: case 0x812: case 0x816: case 0x817: case 0x819: case 0x81a: case 0x81c:
    case 0x820: case 0x821: case 0x822: case 0x823: case 0x824: case 0x827: case 0x831:
    case 0x832: case 0x83b: case 0x840: case 0x882: case 0x883: case 0x886: case 0x887:
    case 0x88c: case 0x890: case 0x891: case 0x892: case 0x899: case 0x89c: case 0x8a4:
    case 0x908: case 0x918: case 0x919: case 0x91a:
    case 0x981: case 0x983: case 0x984: case 0x986: case 0x989: case 0x98e: case 0x992:
    case 0x9a8: case 0x9ab: case 0x9c3: case 0xa02: case 0xa07: case 0xa08: case 0xa09:
    case 0xa0a: case 0xa0b: case 0xa0c: case 0xa10: case 0xa11: case 0xa12: case 0xa13:
    case 0xa17: case 0xa20: case 0xa21: case 0xa23: case 0xa24: case 0xa31: case 0xa32:
    case 0xa3e: case 0xa45: case 0xab9: case 0xabb: case 0xb05: case 0xb08: case 0xb1d:
    case 0xb60: case 0xb63: case 0xb66: case 0xb69: case 0xb6c: case 0xb6f: case 0xb82:
    case 0xb97: case 0xb9b: case 0xc02: case 0xc09: case 0xc0b: case 0xc0c: case 0xc10:
    case 0xc11: case 0xc12: case 0xc13: case 0xc17: case 0xc19: case 0xc20: case 0xc23:
    case 0xc24: case 0xc45: case 0xc82: case 0xd00: case 0xd05: case 0xd06: case 0xd08:
    case 0xd09: case 0xe21: case 0xe23: case 0xe24: case 0xf60: case 0xf89: case 0xf8c:
        return true;
    default:
        return false;
    }
}

} // namespace HmmaDetail

inline HmmaValidationResult ValidateHmmaProgram(std::span<const std::array<u32, 4>> code,
                                                u32 register_limit) {
    const auto fail = [](u32 index, std::string_view error) {
        return HmmaValidationResult{index * 16, error};
    };
    if (code.empty() || code.size() > 65536) {
        return fail(0, "HMMA invalid native program extent");
    }
    if (register_limit != 216 && register_limit != 246) {
        return fail(0, "HMMA register reservation limit");
    }
    const u32 end = static_cast<u32>(code.size());
    enum class Kind { Ordinary, Hmma, Branch, SetSync, Sync, Exit };
    struct Node {
        Kind kind{Kind::Ordinary};
        std::array<u32, 2> successors{};
        u32 successor_count{1};
        u32 target{};
        u32 token{};
        u32 scope{};
        u32 divergent_until{};
        u32 depth{};
        std::array<u32, 17> ancestors{};
        bool reachable{};
        bool reaches_hmma{};
    };
    std::vector<Node> nodes(end + 1);
    for (u32 index = 0; index < end; ++index) {
        const auto& words = code[index];
        auto& node = nodes[index];
        node.successors[0] = index + 1;
        const u32 predicate = (words[0] >> 12) & 15;
        if (predicate == 15) {
            continue;
        }
        const u32 opcode = words[0] & 0xfff;
        if (opcode == 0x23c) {
            const auto decoded = DecodeHmma(words, register_limit);
            if (!decoded) {
                return fail(index, decoded.error);
            }
            node.kind = Kind::Hmma;
            continue;
        }
        if (HmmaDetail::IsFallthroughOpcode(opcode)) {
            continue;
        }
        if (words[3] & 0xc00001ffU) {
            return fail(index, "HMMA unsupported native control encoding");
        }
        const u32 condition = (words[2] >> 23) & 15;
        const bool conditional = predicate != 7 || condition != 7;
        switch (opcode) {
        case 0x947: {
            if ((words[0] & 0xffff0fffU) != 0x947 || (words[1] & 3) ||
                (words[2] & ~0x0783ffffU)) {
                return fail(index, "HMMA unsupported BRA mode");
            }
            const u64 raw = words[1] | (u64{words[2] & 0x3ffffU} << 32);
            const s64 relative = static_cast<s64>(raw) -
                                 ((raw & (u64{1} << 49)) ? (s64{1} << 50) : 0);
            const s64 target = s64{index} * 16 + 16 + relative;
            if (target < 0 || target % 16 || target / 16 >= end) {
                return fail(index, "HMMA unresolved or unaligned branch target");
            }
            if (target / 16 <= index) {
                return fail(index, "HMMA native backedge");
            }
            if (condition == 15) {
                break;
            }
            node.kind = Kind::Branch;
            node.target = static_cast<u32>(target / 16);
            node.successors[0] = node.target;
            if (conditional && node.target != index + 1) {
                node.successors[1] = index + 1;
                node.successor_count = 2;
            }
            break;
        }
        case 0x945: {
            if ((words[0] & 0xfff0ffffU) != 0x7945 || (words[1] & 3) ||
                words[2] != 0x03800000U) {
                return fail(index, "HMMA unsupported BSSY mode");
            }
            const s64 relative = static_cast<s64>(words[1]) -
                                 ((words[1] & 0x80000000U) ? (s64{1} << 32) : 0);
            const s64 target = s64{index} * 16 + 16 + relative;
            if (target % 16 || target / 16 <= index + 1 || target / 16 >= end) {
                return fail(index, "HMMA unresolved reconvergence target");
            }
            node.kind = Kind::SetSync;
            node.target = static_cast<u32>(target / 16);
            node.token = (words[0] >> 16) & 15;
            break;
        }
        case 0x941:
            if ((words[0] & 0xfff0ffffU) != 0x7941 || words[1] ||
                words[2] != 0x03800000U) {
                return fail(index, "HMMA unsupported BSYNC mode");
            }
            node.kind = Kind::Sync;
            node.token = (words[0] >> 16) & 15;
            break;
        case 0x94d:
            if ((words[0] & 0xffff0fffU) != 0x94d || words[1] ||
                (words[2] & ~0x07800000U)) {
                return fail(index, "HMMA unsupported EXIT mode");
            }
            if (condition == 15) {
                break;
            }
            node.kind = Kind::Exit;
            node.successors[0] = end;
            if (conditional) {
                node.successors[1] = index + 1;
                node.successor_count = 2;
            }
            break;
        default:
            return fail(index, "HMMA unknown or unsupported native control opcode");
        }
    }
    nodes[end].ancestors.fill(end);
    const auto common_postdominator = [&](u32 left, u32 right) {
        if (nodes[left].depth < nodes[right].depth) {
            std::swap(left, right);
        }
        const u32 difference = nodes[left].depth - nodes[right].depth;
        for (u32 bit = 0; bit < 17; ++bit) {
            if (difference & (1U << bit)) {
                left = nodes[left].ancestors[bit];
            }
        }
        if (left == right) {
            return left;
        }
        for (int bit = 16; bit >= 0; --bit) {
            if (nodes[left].ancestors[bit] != nodes[right].ancestors[bit]) {
                left = nodes[left].ancestors[bit];
                right = nodes[right].ancestors[bit];
            }
        }
        return nodes[left].ancestors[0];
    };
    for (u32 index = end; index-- > 0;) {
        auto& node = nodes[index];
        u32 parent = node.successors[0];
        if (node.successor_count == 2) {
            parent = common_postdominator(parent, node.successors[1]);
        }
        node.ancestors[0] = parent;
        node.depth = nodes[parent].depth + 1;
        for (u32 bit = 1; bit < 17; ++bit) {
            node.ancestors[bit] = nodes[node.ancestors[bit - 1]].ancestors[bit - 1];
        }
        node.reaches_hmma = node.kind == Kind::Hmma;
        for (u32 edge = 0; edge < node.successor_count; ++edge) {
            node.reaches_hmma |= nodes[node.successors[edge]].reaches_hmma;
        }
    }
    nodes[0].reachable = true;
    nodes[0].scope = end;
    u32 first_exit = end;
    for (u32 index = 0; index < end; ++index) {
        auto& node = nodes[index];
        if (!node.reachable) {
            continue;
        }
        u32 scope = node.scope;
        if (scope != end && index >= nodes[scope].target) {
            return fail(index, "HMMA control flow bypasses BSYNC");
        }
        if (node.kind == Kind::SetSync) {
            for (u32 outer = scope; outer != end; outer = nodes[outer].scope) {
                if (nodes[outer].token == node.token || nodes[outer].target <= node.target) {
                    return fail(index, "HMMA overlapping reconvergence scopes");
                }
            }
            scope = index;
        } else if (node.kind == Kind::Sync) {
            if (scope == end || nodes[scope].token != node.token ||
                nodes[scope].target != index + 1) {
                return fail(index, "HMMA unbalanced reconvergence token");
            }
            scope = nodes[scope].scope;
        } else if (node.kind == Kind::Exit) {
            if (scope != end) {
                return fail(index, "HMMA exit inside reconvergence scope");
            }
            first_exit = (std::min)(first_exit, index);
        } else if (node.kind == Kind::Hmma) {
            if (first_exit < index) {
                return fail(index, "HMMA possible lane exit before collective");
            }
            if (node.divergent_until > index) {
                return fail(index, "HMMA collective inside divergent control flow");
            }
        }
        u32 divergent_until = node.divergent_until;
        if (node.kind == Kind::Branch && node.successor_count == 2) {
            const u32 join = node.ancestors[0];
            if (node.reaches_hmma &&
                (scope == end || join == end || nodes[join].kind != Kind::Sync ||
                 nodes[join].token != nodes[scope].token || nodes[scope].target != join + 1)) {
                return fail(index, "HMMA divergent branch lacks matching postdominating BSYNC");
            }
            divergent_until = (std::max)(divergent_until, join);
        }
        for (u32 edge = 0; edge < node.successor_count; ++edge) {
            const u32 target = node.successors[edge];
            if (target == end) {
                if (node.kind != Kind::Exit || edge != 0) {
                    return fail(index, "HMMA native path falls outside captured program");
                }
                continue;
            }
            auto& successor = nodes[target];
            if (successor.reachable && successor.scope != scope) {
                return fail(target, "HMMA inconsistent reconvergence scopes at join");
            }
            successor.reachable = true;
            successor.scope = scope;
            successor.divergent_until = (std::max)(successor.divergent_until, divergent_until);
        }
    }
    return {};
}

} // namespace Shader::Ampere
