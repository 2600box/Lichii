// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/common_types.h"
#include "common/settings_enums.h"

namespace Tegra {

template <typename Timing>
[[nodiscard]] u64 ReadGpuTimestamp(const Timing& timing, bool ampere, Settings::GpuClock clock) {
    if (ampere) {
        return static_cast<u64>(timing.GetGlobalTimeNs().count());
    }
    const u64 ticks = timing.GetGPUTicks();
    switch (clock) {
    case Settings::GpuClock::Boost:
        return ticks / 256;
    case Settings::GpuClock::Overclock:
        return ticks / 512;
    default:
        return ticks;
    }
}

/// Experiment for EDEN_NX2_GPU_TIME_DIV: GPU durations since the last present shrink by divisor,
/// while timestamps stay within one frame of the nanosecond clock that CPU calibrations use.
[[nodiscard]] constexpr u64 DivideSinceAnchor(u64 now, u64 anchor, u64 divisor) {
    if (divisor <= 1 || now <= anchor) {
        return now;
    }
    return anchor + (now - anchor) / divisor;
}

} // namespace Tegra
