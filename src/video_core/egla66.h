// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <chrono>
#include <cstdlib>

/// Test-only switches for the EGLA-66 back-thread candidates: EDEN_NX2_EGLA66_<NAME>=1 turns one
/// on, EDEN_NX2_EGLA66_<NAME>_VERIFY=1 runs the original path and counts where the candidate's
/// result would differ. Each owner reads its switch once; unset restores the original behaviour.
namespace VideoCommon::Egla66 {

[[nodiscard]] inline bool Env(const char* name) {
    const char* const value = std::getenv(name);
    return value != nullptr && value[0] == '1';
}

/// Whether a 10 s counter window ended (and starts the next one).
[[nodiscard]] inline bool WindowEnded(std::chrono::steady_clock::time_point& window_end) {
    const auto now = std::chrono::steady_clock::now();
    if (now < window_end) {
        return false;
    }
    const bool first = window_end == std::chrono::steady_clock::time_point{};
    window_end = now + std::chrono::seconds{10};
    return !first;
}

} // namespace VideoCommon::Egla66
