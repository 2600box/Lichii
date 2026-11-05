// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <cstdlib>

#include "common/common_types.h"
#include "common/cpu_features.h"

namespace VideoCommon::HostGpuProfile {

enum class Phase : size_t {
    Resolve,
    Enqueue,
    Encode,
    Submit,
    Worker,
    Pool,
    Wait,
    Count,
};

inline constexpr std::array names{
    "resolve", "enqueue", "encode", "submit", "worker other", "command pool", "wait",
};
static_assert(names.size() == static_cast<size_t>(Phase::Count));

struct Sample {
    u64 nanoseconds{};
    u64 calls{};
};

class Accounting {
public:
    struct Token {
        Phase phase;
        u64 start;
        u64 parent_children;
    };

    Token Begin(Phase phase, u64 now) {
        const Token token{phase, now, children};
        children = 0;
        return token;
    }

    void End(Token token, u64 now) {
        const u64 elapsed = now - token.start;
        auto& sample = samples[static_cast<size_t>(token.phase)];
        sample.nanoseconds += elapsed - children;
        ++sample.calls;
        children = token.parent_children + elapsed;
    }

    auto TakeSamples() {
        const auto result = samples;
        samples = {};
        return result;
    }

private:
    std::array<Sample, static_cast<size_t>(Phase::Count)> samples{};
    u64 children{};
};

inline const bool enabled = std::getenv("EDEN_NX2_GPU_PROFILE") != nullptr;
inline thread_local Accounting accounting;

class Scope {
public:
    explicit Scope(Phase phase) {
        if (enabled) {
            token = accounting.Begin(phase, Now());
        }
    }

    ~Scope() {
        if (enabled) {
            accounting.End(token, Now());
        }
    }

    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    static u64 Now() {
        return static_cast<u64>(Common::g_wall_clock.GetTimeNS().count());
    }

    Accounting::Token token{};
};

} // namespace VideoCommon::HostGpuProfile
