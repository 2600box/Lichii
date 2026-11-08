// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#endif

#include "common/logging.h"
#include "common/thread.h"
#include "video_core/renderer_vulkan/pipeline_load_trace.h"

namespace Vulkan::LoadTrace {
namespace {

struct Memory {
    u64 commit{};
    u64 working_set{};
    u64 peak_commit{};
    u64 peak_working_set{};
};

Memory ReadMemory() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (!GetProcessMemoryInfo(GetCurrentProcess(),
                              reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                              sizeof(counters))) {
        return {};
    }
    return {counters.PrivateUsage, counters.WorkingSetSize, counters.PeakPagefileUsage,
            counters.PeakWorkingSetSize};
#else
    Memory memory;
    std::ifstream status{"/proc/self/status"};
    std::string key;
    u64 value{};
    std::string unit;
    while (status >> key >> value >> unit) {
        if (key == "VmRSS:") {
            memory.working_set = value << 10;
        } else if (key == "VmHWM:") {
            memory.peak_working_set = value << 10;
        } else if (key == "VmData:") {
            memory.commit = value << 10;
        }
    }
    return memory;
#endif
}

size_t EnvSize(const char* name) {
    const char* const value = std::getenv(name);
    return value != nullptr ? static_cast<size_t>(std::strtoull(value, nullptr, 10)) : 0;
}

bool Enabled() {
    static const bool enabled = [] {
        const char* const value = std::getenv("EDEN_NX2_PIPELINE_LOAD_TRACE");
        return value != nullptr && value[0] == '1';
    }();
    return enabled;
}

constexpr const char* PhaseName(Phase phase) {
    switch (phase) {
    case Phase::Idle:
        return "idle";
    case Phase::Read:
        return "read";
    case Phase::Build:
        return "build";
    case Phase::Warm:
        return "warm";
    case Phase::Game:
        return "game";
    }
    return "?";
}

struct State {
    std::atomic<s64> queued_bytes{};
    std::atomic<s64> in_flight{};
    std::atomic<s64> in_flight_code{};
    std::atomic<s64> done{};
    std::atomic<s64> ours_held{};
    std::atomic<s64> ours_max{};
    std::atomic<Phase> phase{Phase::Idle};
    std::chrono::steady_clock::time_point phase_begin{std::chrono::steady_clock::now()};
    u64 phase_peak_commit{};
    u64 phase_peak_working_set{};
    std::mutex mutex;
    std::condition_variable room;
    std::once_flag started;
    std::jthread sampler;
};

State& Get() {
    static State state;
    return state;
}

constexpr u64 MiB = u64{1} << 20;

void Sample(State& state, bool summary) {
    const Memory memory = ReadMemory();
    const Phase phase = state.phase.load();
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                                         state.phase_begin)
                               .count();
    std::scoped_lock lock{state.mutex};
    state.phase_peak_commit = std::max(state.phase_peak_commit, memory.commit);
    state.phase_peak_working_set = std::max(state.phase_peak_working_set, memory.working_set);
    if (summary) {
        LOG_INFO(Render_Vulkan,
                 "Load trace: phase {} took {:.1f} s, sampled peak commit {} MiB, working set "
                 "{} MiB; process peak commit {} MiB, working set {} MiB",
                 PhaseName(phase), seconds, state.phase_peak_commit / MiB,
                 state.phase_peak_working_set / MiB, memory.peak_commit / MiB,
                 memory.peak_working_set / MiB);
        return;
    }
    LOG_INFO(Render_Vulkan,
             "Load trace: {} +{:.1f} s commit {} MiB ws {} MiB queued {} MiB in_flight {} "
             "(code {} KiB) done {} ours_held {} MiB ours_max_job {} KiB",
             PhaseName(phase), seconds, memory.commit / MiB, memory.working_set / MiB,
             state.queued_bytes.load() / static_cast<s64>(MiB), state.in_flight.load(),
             state.in_flight_code.load() >> 10, state.done.load(),
             state.ours_held.load() / static_cast<s64>(MiB), state.ours_max.load() >> 10);
}

} // Anonymous namespace

void Begin() {
    if (!Enabled()) {
        return;
    }
    State& state = Get();
    std::call_once(state.started, [&state] {
        state.sampler = std::jthread([&state](std::stop_token stop) {
            Common::SetCurrentThreadName("VkLoadTrace");
            std::chrono::steady_clock::time_point game_begin{};
            while (!stop.stop_requested()) {
                const Phase phase = state.phase.load();
                if (phase == Phase::Game) {
                    const auto now = std::chrono::steady_clock::now();
                    if (game_begin == std::chrono::steady_clock::time_point{}) {
                        game_begin = now;
                    }
                    if (now - game_begin > std::chrono::minutes{5}) {
                        Sample(state, true);
                        return;
                    }
                }
                Sample(state, false);
                std::this_thread::sleep_for(phase == Phase::Game ? std::chrono::seconds{2}
                                                                 : std::chrono::milliseconds{500});
            }
        });
    });
}

void SetPhase(Phase phase) {
    if (!Enabled()) {
        return;
    }
    State& state = Get();
    Sample(state, true);
    std::scoped_lock lock{state.mutex};
    state.phase = phase;
    state.phase_begin = std::chrono::steady_clock::now();
    state.phase_peak_commit = 0;
    state.phase_peak_working_set = 0;
    state.done = 0;
}

void Queued(s64 bytes) {
    Get().queued_bytes += bytes;
}

void Started(s64 code_bytes) {
    State& state = Get();
    ++state.in_flight;
    state.in_flight_code += code_bytes;
}

void Finished(s64 bytes, s64 code_bytes) {
    State& state = Get();
    --state.in_flight;
    state.in_flight_code -= code_bytes;
    ++state.done;
    {
        std::scoped_lock lock{state.mutex};
        state.queued_bytes -= bytes;
    }
    state.room.notify_all();
}

size_t Workers() {
    static const size_t workers = EnvSize("EDEN_NX2_LOAD_WORKERS");
    return workers;
}

size_t QueueLimit() {
    static const size_t limit = EnvSize("EDEN_NX2_LOAD_QUEUE_MB") * MiB;
    return limit;
}

void Ours(s64 peak, s64 held) {
    State& state = Get();
    state.ours_held += held;
    s64 max = state.ours_max.load();
    while (peak > max && !state.ours_max.compare_exchange_weak(max, peak)) {
    }
}

bool FreeIrEarly() {
    static const bool enabled = EnvSize("EDEN_NX2_FREE_IR_EARLY") != 0;
    return enabled;
}

size_t WarmLimit() {
    static const size_t limit = EnvSize("EDEN_NX2_WARM_LIMIT_S");
    return limit;
}

void WaitForRoom(size_t limit, const std::stop_token& stop) {
    State& state = Get();
    std::unique_lock lock{state.mutex};
    while (state.queued_bytes.load() > static_cast<s64>(limit) && !stop.stop_requested()) {
        state.room.wait_for(lock, std::chrono::milliseconds{100});
    }
}

} // namespace Vulkan::LoadTrace
