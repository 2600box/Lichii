// SPDX-FileCopyrightText: Copyright (c) 2026 Ivan0xFF
// SPDX-License-Identifier: MIT

// Port of craftycorvid/oot-mod build.py (c5ccd1c): Ocarina of Time ultrawide hooks for main build
// E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3348. Every 16:9 sead::PerspectiveProjection (and the w/h
// aspect handed to shaders) gets the target aspect, full-screen 2D is squeezed into the centre 16:9
// and pre-rendered movies get a centred 16:9 viewport. The emulator then stretches the 16:9 frame
// to the target aspect.

#pragma once

#include <array>
#include <bit>
#include <cstddef>

#include "common/common_types.h"

namespace FileSys::OcarinaUltrawide {

constexpr size_t CaveBase = 0x3C46400;
constexpr size_t CaveEnd = 0x3C47000;
constexpr size_t CaveWords = 121;
static_assert(CaveBase + CaveWords * 4 <= CaveEnd);

struct Hook {
    size_t offset;
    u32 original;
};

constexpr std::array<Hook, 7> Hooks{{
    {0x3485CC0, 0x2D558400}, // sead::PerspectiveProjection::doUpdateMatrix: ldp s0, s1, [x0, #172]
    {0x004F1E8, 0x2D558EA0}, // inlined copies of the matrix builder
    {0x023B9A8, 0x2D558C00},
    {0x033B82C, 0x2D558440},
    {0x025D76C, 0x1E2202A1}, // render view setup: scvtf s1, w21; fdiv s1, s1, s2
    {0x0774368, 0xAD401041}, // 2D canvas: ldp q1, q4, [x2]
    {0x3B5C758, 0xA941A269}, // pre-rendered movie layer draw: ldp x9, x8, [x19, #24]
}};

struct Patch {
    std::array<std::array<u8, 4>, Hooks.size()> branches{};
    std::array<u8, CaveWords * 4> cave{};
};

namespace Detail {

constexpr u32 F32(double value) {
    return std::bit_cast<u32>(static_cast<float>(value));
}

constexpr u32 Movz(u32 rd, u32 imm) { return 0x52800000 | imm << 5 | rd; }
constexpr u32 Movk16(u32 rd, u32 imm) { return 0x72A00000 | imm << 5 | rd; }
constexpr u32 FmovSW(u32 sd, u32 wn) { return 0x1E270000 | wn << 5 | sd; }
constexpr u32 FmovWS(u32 wd, u32 sn) { return 0x1E260000 | sn << 5 | wd; }
constexpr u32 Fsub(u32 sd, u32 sn, u32 sm) { return 0x1E203800 | sm << 16 | sn << 5 | sd; }
constexpr u32 Fadd(u32 sd, u32 sn, u32 sm) { return 0x1E202800 | sm << 16 | sn << 5 | sd; }
constexpr u32 Fmul(u32 sd, u32 sn, u32 sm) { return 0x1E200800 | sm << 16 | sn << 5 | sd; }
constexpr u32 Fdiv(u32 sd, u32 sn, u32 sm) { return 0x1E201800 | sm << 16 | sn << 5 | sd; }
constexpr u32 Fabs(u32 sd, u32 sn) { return 0x1E20C000 | sn << 5 | sd; }
constexpr u32 Fcmp(u32 sn, u32 sm) { return 0x1E202000 | sm << 16 | sn << 5; }
constexpr u32 ScvtfSW(u32 sd, u32 wn) { return 0x1E220000 | wn << 5 | sd; }
constexpr u32 StrS(u32 st, u32 xn, u32 imm) { return 0xBD000000 | (imm / 4) << 10 | xn << 5 | st; }
constexpr u32 LdrS(u32 st, u32 xn, u32 imm) { return 0xBD400000 | (imm / 4) << 10 | xn << 5 | st; }
constexpr u32 LdpS(u32 s1, u32 s2, u32 xn, u32 imm) {
    return 0x2D400000 | ((imm / 4) & 0x7F) << 15 | s2 << 10 | xn << 5 | s1;
}
constexpr u32 LdpQ(u32 q1, u32 q2, u32 xn, u32 imm) {
    return 0xAD400000 | ((imm / 16) & 0x7F) << 15 | q2 << 10 | xn << 5 | q1;
}
constexpr u32 LdpX(u32 x1, u32 x2, u32 xn, u32 imm) {
    return 0xA9400000 | ((imm / 8) & 0x7F) << 15 | x2 << 10 | xn << 5 | x1;
}
constexpr u32 LdrX(u32 xt, u32 xn) { return 0xF9400000 | xn << 5 | xt; }
constexpr u32 AddX(u32 xd, u32 xn, u32 imm) { return 0x91000000 | imm << 10 | xn << 5 | xd; }
constexpr u32 SubX(u32 xd, u32 xn, u32 imm) { return 0xD1000000 | imm << 10 | xn << 5 | xd; }
constexpr u32 CmpW(u32 wn, u32 wm) { return 0x6B00001F | wm << 16 | wn << 5; }
constexpr u32 CmpX(u32 xn, u32 xm) { return 0xEB00001F | xm << 16 | xn << 5; }
constexpr u32 UmovS1(u32 wd, u32 vn) { return 0x0E0C3C00 | vn << 5 | wd; }
constexpr u32 AndAbs(u32 wd, u32 wn) { return 0x12007800 | wn << 5 | wd; }
constexpr u32 Dup4S(u32 vd, u32 wn) { return 0x4E040C00 | wn << 5 | vd; }
constexpr u32 Fmul4S(u32 vd, u32 vn, u32 vm) { return 0x6E20DC00 | vm << 16 | vn << 5 | vd; }

enum Cond : u32 { EQ = 0x0, NE = 0x1, PL = 0x5 };

class Code {
public:
    constexpr void Emit(u32 word) {
        words[size++] = word;
    }

    constexpr size_t Pc() const {
        return CaveBase + size * 4;
    }

    constexpr void LoadF32(u32 wt, u32 st, u32 bits) {
        Emit(Movz(wt, bits & 0xFFFF));
        Emit(Movk16(wt, bits >> 16));
        Emit(FmovSW(st, wt));
    }

    constexpr void Adrp(u32 xd, size_t target) {
        const s64 pages = static_cast<s64>(target >> 12) - static_cast<s64>(Pc() >> 12);
        Emit(0x90000000 | static_cast<u32>(pages & 3) << 29 |
             static_cast<u32>((pages >> 2) & 0x7FFFF) << 5 | xd);
        Emit(AddX(xd, xd, static_cast<u32>(target & 0xFFF)));
    }

    constexpr void Branch(size_t to) {
        Emit(Encode(Pc(), to));
    }

    constexpr size_t Reserve() {
        Emit(0);
        return size - 1;
    }

    // Points the conditional branch reserved at slot to the next emitted word.
    constexpr void Bind(size_t slot, Cond cond) {
        words[slot] = 0x54000000 | static_cast<u32>((size - slot) & 0x7FFFF) << 5 | cond;
    }

    static constexpr u32 Encode(size_t from, size_t to) {
        const s64 delta = static_cast<s64>(to) - static_cast<s64>(from);
        return 0x14000000 | static_cast<u32>((delta / 4) & 0x3FFFFFF);
    }

    std::array<u32, CaveWords> words{};
    size_t size = 0;
};

// if |sa - 16/9| < 0.01: sa = target, stored back to mAspect at [writeback, #176]. NaN is skipped.
constexpr void FixAspect(Code& code, u32 target, u32 sa, u32 wt, u32 sp, u32 sq,
                         int writeback = -1) {
    code.LoadF32(wt, sp, F32(16.0 / 9.0));
    code.Emit(Fsub(sp, sa, sp));
    code.Emit(Fabs(sp, sp));
    code.LoadF32(wt, sq, F32(0.01));
    code.Emit(Fcmp(sp, sq));
    const size_t skip = code.Reserve();
    code.LoadF32(wt, sa, target);
    if (writeback >= 0) {
        code.Emit(StrS(sa, static_cast<u32>(writeback), 176));
    }
    code.Bind(skip, PL);
}

// After ldp q1, q4, [x2] (x2 = row-major projection, x0 = canvas): scale row 0 of the full-screen
// 3840x2160 layout ortho so the HUD fills the centre 16:9. Screens whose layouts redraw a capture
// of the whole frame keep covering it: the L/R item menu and the pause and system menu
// backgrounds, found by the vptr of the UI screen that embeds the canvas at +0x1B8.
constexpr void SquashUi(Code& code, u32 squash) {
    constexpr u32 UiM00 = F32(2.0 / 3840.0);
    constexpr u32 UiM11 = F32(2.0 / 2160.0);
    constexpr std::array<size_t, 2> FullFrameScreens{0x4BED098, 0x4BE4FB0};
    std::array<size_t, 2 + FullFrameScreens.size()> skips{};
    code.Emit(LdpQ(1, 4, 2, 0));
    code.Emit(FmovWS(16, 1));
    code.Emit(Movz(17, UiM00 & 0xFFFF));
    code.Emit(Movk16(17, UiM00 >> 16));
    code.Emit(CmpW(16, 17));
    skips[0] = code.Reserve();
    code.Emit(UmovS1(16, 4));
    code.Emit(AndAbs(16, 16));
    code.Emit(Movz(17, UiM11 & 0xFFFF));
    code.Emit(Movk16(17, UiM11 >> 16));
    code.Emit(CmpW(16, 17));
    skips[1] = code.Reserve();
    code.Emit(SubX(17, 0, 0x1B8));
    code.Emit(LdrX(16, 17));
    for (size_t i = 0; i < FullFrameScreens.size(); ++i) {
        code.Adrp(17, FullFrameScreens[i]);
        code.Emit(CmpX(16, 17));
        skips[2 + i] = code.Reserve();
    }
    code.Emit(Movz(16, squash & 0xFFFF));
    code.Emit(Movk16(16, squash >> 16));
    code.Emit(Dup4S(17, 16));
    code.Emit(Fmul4S(1, 1, 17));
    code.Bind(skips[0], NE);
    code.Bind(skips[1], NE);
    for (size_t i = 0; i < FullFrameScreens.size(); ++i) {
        code.Bind(skips[2 + i], EQ);
    }
}

// After the movie layer builds its full-target sead::Viewport at x29-40 (minX +8, maxX +16):
// shrink it to the centre 16:9.
constexpr void PillarboxMovie(Code& code, u32 margin) {
    code.Emit(SubX(16, 29, 40));
    code.Emit(LdrS(16, 16, 8));
    code.Emit(LdrS(17, 16, 16));
    code.Emit(Fsub(18, 17, 16));
    code.Emit(Movz(17, margin & 0xFFFF));
    code.Emit(Movk16(17, margin >> 16));
    code.Emit(FmovSW(19, 17));
    code.Emit(Fmul(18, 18, 19));
    code.Emit(Fadd(16, 16, 18));
    code.Emit(Fsub(17, 17, 18));
    code.Emit(StrS(16, 16, 8));
    code.Emit(StrS(17, 16, 16));
    code.Emit(LdpX(9, 8, 19, 24));
}

} // namespace Detail

// Builds the hook branches and the cave for a display aspect wider than 16:9.
constexpr Patch Make(double aspect) {
    using namespace Detail;
    const u32 target = F32(aspect);
    const double squash = (16.0 / 9.0) / aspect;
    Code code;
    Patch patch;
    const auto hook = [&](size_t index, size_t resume, auto&& body) {
        const u32 branch = Code::Encode(Hooks[index].offset, code.Pc());
        for (size_t i = 0; i < 4; ++i) {
            patch.branches[index][i] = static_cast<u8>(branch >> (i * 8));
        }
        body();
        code.Branch(resume);
    };
    hook(0, 0x3485CC4, [&] {
        code.Emit(LdpS(0, 1, 0, 172));
        FixAspect(code, target, 1, 16, 16, 17, 0);
    });
    hook(1, 0x004F1EC, [&] {
        code.Emit(LdpS(0, 3, 21, 172));
        FixAspect(code, target, 3, 9, 4, 5, 21);
    });
    hook(2, 0x023B9AC, [&] {
        code.Emit(LdpS(0, 3, 0, 172));
        FixAspect(code, target, 3, 9, 4, 5, 0);
    });
    hook(3, 0x033B830, [&] {
        code.Emit(LdpS(0, 1, 2, 172));
        FixAspect(code, target, 1, 9, 2, 3, 2);
    });
    hook(4, 0x025D774, [&] {
        code.Emit(ScvtfSW(1, 21));
        code.Emit(Fdiv(1, 1, 2));
        FixAspect(code, target, 1, 16, 16, 17);
    });
    hook(5, 0x077436C, [&] { SquashUi(code, F32(squash)); });
    hook(6, 0x3B5C75C, [&] { PillarboxMovie(code, F32((1.0 - squash) / 2.0)); });
    for (size_t i = 0; i < code.size; ++i) {
        for (size_t b = 0; b < 4; ++b) {
            patch.cave[i * 4 + b] = static_cast<u8>(code.words[i] >> (b * 8));
        }
    }
    return patch;
}

} // namespace FileSys::OcarinaUltrawide
