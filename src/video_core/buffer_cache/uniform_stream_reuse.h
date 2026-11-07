// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <span>
#include <type_traits>

#include "common/common_types.h"
#include "video_core/nx2_experiment.h"

namespace VideoCommon {

/// What a buffer cache runtime offers for stream uniform reuse.
///  - StreamUniformRef: a small trivially copyable description of one stream allocation (the
///    host buffer, its offset and whatever else the runtime needs to add it to a descriptor
///    payload again, e.g. the device address of the descriptor buffer path).
///  - LastStreamUniformRef(): the allocation made by the latest BindMappedUniformBuffer call.
///  - RebindStreamUniform(ref, size): adds `ref` to the descriptor payload exactly as
///    BindMappedUniformBuffer added the allocation the first time.
template <class R>
concept StreamUniformReusable = requires(R& runtime, u32 size) {
    typename R::StreamUniformRef;
    requires std::is_trivially_copyable_v<typename R::StreamUniformRef>;
    { runtime.LastStreamUniformRef() } -> std::same_as<typename R::StreamUniformRef>;
    runtime.RebindStreamUniform(runtime.LastStreamUniformRef(), size);
    { runtime.CurrentTick() } -> std::convertible_to<u64>;
};

/// EDEN_NX2_UBOREUSE=0 turns the reuse off (every small uniform buffer is copied, as before).
[[nodiscard]] inline bool UniformStreamReuseEnabled() noexcept {
    static const bool enabled = [] {
        const char* const value = std::getenv("EDEN_NX2_UBOREUSE");
        return value == nullptr || value[0] != '0';
    }();
    return enabled;
}

/// Graphics uniform binds by path, for the EDEN_NX2_GPU_PROFILE report (BufferCache::TickFrame).
/// Written and read on the GPU back thread only.
struct UniformBindStats {
    u64 classic{};  ///< bound from the cached host buffer
    u64 streamed{}; ///< copied to the stream buffer outside BindSmallUniformReusing
    u64 reused{};   ///< same bytes, same command buffer: the slot's allocation bound again
    u64 recopied{}; ///< same bytes, new command buffer: copied again
    u64 changed{};  ///< bytes differ from the slot's previous bind
    u64 in_place{}; ///< of the three above: compared in guest memory (ubo-inplace)
    u64 bytes{};    ///< bytes of the streamed and checked binds
};

inline UniformBindStats g_uniform_bind_stats;

template <class Ref>
class UniformStreamReuse {
public:
    static constexpr u32 MaxBytes = 4096; // BufferCache's DEFAULT_SKIP_CACHE_SIZE
    static constexpr size_t NumStages = 5;
    static constexpr size_t NumSlots = 18; // Maxwell constant buffers per stage
    static constexpr u64 PageSize = 4096;  // Core::DEVICE_PAGESIZE, what GetPointer translates
    static_assert(MaxBytes <= PageSize);

    struct Slot {
        Ref ref{};
        u64 tick{};
        u32 size{}; // 0: nothing streamed yet
        std::array<u8, MaxBytes> bytes{};
    };

    void Reset() noexcept {
        for (auto& stage : slots) {
            for (Slot& slot : stage) {
                slot.size = 0;
                slot.tick = 0;
            }
        }
    }

    std::array<std::array<Slot, NumSlots>, NumStages> slots{};
    std::array<u8, MaxBytes> scratch{};
};

template <class Ref>
[[nodiscard]] inline UniformStreamReuse<Ref>& UniformStreamReuseState() noexcept {
    static UniformStreamReuse<Ref> state;
    return state;
}

/// Forgets every slot. Call it when a buffer cache (and so its runtime, scheduler and stream
/// buffer) is created: ticks restart with a new scheduler and the old references are dead.
template <class Runtime>
inline void ResetUniformStreamReuse() noexcept {
    if constexpr (StreamUniformReusable<Runtime>) {
        UniformStreamReuseState<typename Runtime::StreamUniformRef>().Reset();
    }
}

/// The `size` guest bytes at `device_addr` where they lie in host memory, or nullptr when they
/// span two pages that are not contiguous there or a page is unmapped (read them instead).
template <class Reuse, class DeviceMemory>
[[nodiscard]] const u8* GuestBytes(DeviceMemory& device_memory, u64 device_addr, u32 size) {
    if constexpr (requires { device_memory.template GetPointer<u8>(device_addr); }) {
        const u8* const first = device_memory.template GetPointer<u8>(device_addr);
        if (first == nullptr) {
            return nullptr;
        }
        const u64 next_page = (device_addr | (Reuse::PageSize - 1)) + 1;
        if (device_addr + size <= next_page) {
            return first;
        }
        const u8* const second = device_memory.template GetPointer<u8>(next_page);
        return second == first + (next_page - device_addr) ? first : nullptr;
    } else {
        return nullptr;
    }
}

/// Binds the small uniform buffer at `device_addr` for (stage, cbuf `index`) through the stream
/// buffer, reusing the slot's previous allocation when the bytes and the command buffer are the
/// same. Returns false (nothing bound) when the buffer does not qualify, so the caller binds it
/// the classic way.
template <class Runtime, class DeviceMemory>
[[nodiscard]] bool BindSmallUniformReusing(Runtime& runtime, DeviceMemory& device_memory,
                                           size_t stage, u32 index, u32 binding_index,
                                           u64 device_addr, u32 size) {
    using Reuse = UniformStreamReuse<typename Runtime::StreamUniformRef>;
    if (size == 0 || size > Reuse::MaxBytes || stage >= Reuse::NumStages ||
        index >= Reuse::NumSlots) {
        return false;
    }
    Reuse& reuse = UniformStreamReuseState<typename Runtime::StreamUniformRef>();
    typename Reuse::Slot& slot = reuse.slots[stage][index];
    UniformBindStats& stats = g_uniform_bind_stats;
    stats.bytes += size;
    // ubo-inplace: compare against guest memory where it lies instead of reading a copy first.
    // The stream buffer then gets the slot's copy, so it always holds exactly slot.bytes even if
    // the guest writes between the two reads.
    const u8* guest = nullptr;
    if (Nx2Experiment::On(Nx2Experiment::UboInPlace)) {
        guest = GuestBytes<Reuse>(device_memory, device_addr, size);
    }
    const u8* bytes = guest;
    if (bytes == nullptr) {
        device_memory.ReadBlockUnsafe(device_addr, reuse.scratch.data(), size);
        bytes = reuse.scratch.data();
    } else {
        ++stats.in_place;
    }

    const bool same_bytes = slot.size == size && std::memcmp(slot.bytes.data(), bytes, size) == 0;
    if (same_bytes && slot.tick == runtime.CurrentTick()) {
        ++stats.reused;
        runtime.RebindStreamUniform(slot.ref, size);
        return true;
    }
    if (same_bytes) {
        ++stats.recopied;
    } else {
        ++stats.changed;
        slot.size = size;
        std::memcpy(slot.bytes.data(), bytes, size);
    }
    const std::span<u8> span = runtime.BindMappedUniformBuffer(stage, binding_index, size);
    std::memcpy(span.data(), guest != nullptr ? slot.bytes.data() : bytes, size);
    slot.ref = runtime.LastStreamUniformRef();
    // After the allocation: the tick the stream region is tagged with is the one current now.
    slot.tick = runtime.CurrentTick();
    return true;
}

} // namespace VideoCommon
