// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// TEST ONLY (EGLA-80, never merges): memory trace of the boot-time pipeline builds and the
// switches that A/B their memory use.

#pragma once

#include <cstddef>
#include <stop_token>

#include "common/common_types.h"

namespace Vulkan::LoadTrace {

enum class Phase : int { Idle, Read, Build, Warm, Game };

/// EDEN_NX2_PIPELINE_LOAD_TRACE=1: a thread logs process memory and the counters below every
/// 500 ms during the boot phases, then every 2 s for 5 minutes of play.
void Begin();
void SetPhase(Phase phase);

/// Environment bytes waiting in a build queue or being built
void Queued(s64 bytes);
/// One build started / finished, with the guest code bytes it translates
void Started(s64 code_bytes);
void Finished(s64 bytes, s64 code_bytes);

/// EDEN_NX2_LOAD_WORKERS=N: boot build threads (0 when unset)
size_t Workers();
/// EDEN_NX2_LOAD_QUEUE_MB=M: blocks the reader while queued bytes exceed M MiB (0 when unset)
size_t QueueLimit();
void WaitForRoom(size_t limit, const std::stop_token& stop);

} // namespace Vulkan::LoadTrace
