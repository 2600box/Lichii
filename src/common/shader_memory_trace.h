// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <span>
#include <vector>

#include "common/common_types.h"
#include "common/logging.h"

namespace Common::ShaderMemoryTrace {

inline u64 Hash(std::span<const u8> bytes) {
    u64 hash = 14695981039346656037ULL;
    for (const u8 byte : bytes) {
        hash = (hash ^ byte) * 1099511628211ULL;
    }
    return hash;
}

struct Observation {
    uintptr_t base;
    size_t offset;
    size_t size;
    size_t first_difference;
    u8 expected;
    u8 actual;
    u64 hash;
};

class Tracker {
public:
    explicit Tracker(std::vector<u8> reference_) : reference{std::move(reference_)} {}

    void Track(uintptr_t address, size_t offset, size_t size) {
        if (offset >= reference.size() || size > ~uintptr_t{} - address)
            return;
        size = std::min(size, reference.size() - offset);
        for (const auto& range : ranges) {
            if (address >= range.address && address - range.address <= range.size &&
                size <= range.size - (address - range.address) &&
                offset == range.offset + (address - range.address))
                return;
        }
        if (ranges.size() < 128)
            ranges.push_back({address, offset, size});
    }

    template <typename Emit>
    void Observe(uintptr_t address, std::span<const u8> bytes, Emit&& emit) {
        if (reference.size() < 64 || bytes.size() > ~uintptr_t{} - address) {
            return;
        }
        if (ranges.size() < 128) {
            auto cursor = bytes.begin();
            while (cursor != bytes.end() && ranges.size() < 128) {
                const auto match =
                    std::search(cursor, bytes.end(), reference.begin(), reference.begin() + 64);
                if (match == bytes.end()) {
                    break;
                }
                const uintptr_t base = address + static_cast<size_t>(match - bytes.begin());
                Track(base, 0, bytes.end() - match);
                cursor = match + 1;
            }
        }
        for (const auto& range : ranges) {
            const uintptr_t begin = std::max(range.address, address);
            const uintptr_t end = std::min(range.address + range.size, address + bytes.size());
            if (begin >= end) {
                continue;
            }
            const auto observed = bytes.subspan(begin - address, end - begin);
            const size_t offset = range.offset + begin - range.address;
            const auto expected = std::span{reference}.subspan(offset, end - begin);
            const auto mismatch = std::mismatch(observed.begin(), observed.end(), expected.begin());
            const size_t difference = static_cast<size_t>(mismatch.first - observed.begin());
            emit(Observation{range.address, offset, end - begin,
                             difference == observed.size() ? reference.size() : offset + difference,
                             difference == observed.size() ? u8{} : expected[difference],
                             difference == observed.size() ? u8{} : observed[difference],
                             Hash(observed)});
        }
    }

    size_t Size() const {
        return reference.size();
    }
    u64 ReferenceHash() const {
        return Hash(reference);
    }

private:
    struct Range {
        uintptr_t address;
        size_t offset;
        size_t size;
    };
    std::vector<u8> reference;
    std::vector<Range> ranges;
};

inline bool Enabled() {
    static const bool enabled = std::getenv("EDEN_NX2_SHADER_MEMORY_TRACE") != nullptr;
    return enabled;
}

inline thread_local const char* writer{};

class ScopedWriter {
public:
    explicit ScopedWriter(const char* source) : previous{writer} {
        writer = source;
    }
    ~ScopedWriter() {
        writer = previous;
    }

private:
    const char* previous;
};

inline const std::vector<u8>& Reference() {
    static const std::vector<u8> reference = [] {
        std::ifstream file{std::getenv("EDEN_NX2_SHADER_MEMORY_TRACE"),
                           std::ios::binary | std::ios::ate};
        const auto size = file.tellg();
        if (!file || size < 64 || size > 65536) {
            LOG_ERROR(HW_Memory, "shader memory trace: reference must contain 64..65536 bytes");
            return std::vector<u8>{};
        }
        std::vector<u8> bytes(static_cast<size_t>(size));
        file.seekg(0);
        file.read(reinterpret_cast<char*>(bytes.data()), size);
        if (!file) {
            LOG_ERROR(HW_Memory, "shader memory trace: cannot read reference");
            return std::vector<u8>{};
        }
        LOG_INFO(HW_Memory, "shader memory trace: reference={:016x} bytes={} prefix=64",
                 Hash(bytes), bytes.size());
        return bytes;
    }();
    return reference;
}

struct State {
    Tracker tracker{Reference()};
    std::mutex mutex;
    size_t emitted{};
    bool reported_difference{};
};

inline State& GetState() {
    static State state;
    return state;
}

inline bool MatchesPrefix(std::span<const u8> bytes) {
    const auto& reference = Reference();
    return reference.size() >= 64 && bytes.size() >= 64 &&
           std::equal(reference.begin(), reference.begin() + 64, bytes.begin());
}

inline void TrackPage(const void* address, size_t offset, size_t size) {
    if (!address)
        return;
    auto& state = GetState();
    std::scoped_lock lock{state.mutex};
    state.tracker.Track(reinterpret_cast<uintptr_t>(address), offset, size);
}

inline void Observe(const char* source, const void* destination, std::span<const u8> bytes) {
    if (!Enabled() || !destination || bytes.empty())
        return;
    auto& state = GetState();
    std::scoped_lock lock{state.mutex};
    state.tracker.Observe(
        reinterpret_cast<uintptr_t>(destination), bytes, [&](const Observation& sample) {
            const bool different = sample.first_difference != state.tracker.Size();
            if (state.emitted >= 128 && (!different || state.reported_difference)) {
                return;
            }
            ++state.emitted;
            state.reported_difference |= different;
            LOG_INFO(
                HW_Memory,
                "shader memory trace: event={} source={} writer={} host={:#x} reference={:016x} "
                "offset={:#x} size={:#x} hash={:016x} equal={} first_diff={:#x} expected={:02x} "
                "actual={:02x}",
                state.emitted, source, writer ? writer : "none", sample.base,
                state.tracker.ReferenceHash(), sample.offset, sample.size, sample.hash, !different,
                sample.first_difference, sample.expected, sample.actual);
        });
}

inline void NoteUpload(u64 destination, size_t expected, size_t supplied, size_t offset,
                       std::span<const u8> payload) {
    if (!Enabled() || supplied >= expected) {
        return;
    }
    const auto& reference = Reference();
    if (reference.size() < 64 || std::search(payload.begin(), payload.end(), reference.begin(),
                                             reference.begin() + 64) == payload.end()) {
        return;
    }
    static std::atomic<u32> reports{};
    if (reports.fetch_add(1, std::memory_order_relaxed) < 16) {
        LOG_INFO(HW_Memory,
                 "shader memory trace: short inline chunk gpu={:#x} expected={:#x} supplied={:#x} "
                 "offset={:#x}",
                 destination, expected, supplied, offset);
    }
}

} // namespace Common::ShaderMemoryTrace
