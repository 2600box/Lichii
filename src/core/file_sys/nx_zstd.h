// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

#define ZSTD_STATIC_LINKING_ONLY
#include <zstd.h>

namespace FileSys::NxZstd {

/// Largest decoded NX block and Zstandard window (NXFormat 1.0.0 SPEC section 7).
constexpr std::size_t MaxBlockSize = 1024 * 1024;

/// Decoder contexts shared by the concurrent reads of one archive. At most `cap` idle contexts
/// are kept for reuse; extra ones are freed when released.
class DecoderPool {
public:
    explicit DecoderPool(std::size_t cap_) : cap{cap_} {
        idle.reserve(cap);
    }

    ~DecoderPool() {
        for (ZSTD_DCtx* context : idle) {
            ZSTD_freeDCtx(context);
        }
    }

    DecoderPool(const DecoderPool&) = delete;
    DecoderPool& operator=(const DecoderPool&) = delete;

    ZSTD_DCtx* Acquire() {
        {
            std::scoped_lock lock{mutex};
            if (!idle.empty()) {
                ZSTD_DCtx* context = idle.back();
                idle.pop_back();
                return context;
            }
        }
        ZSTD_DCtx* context = ZSTD_createDCtx();
        if (context != nullptr &&
            ZSTD_isError(ZSTD_DCtx_setParameter(context, ZSTD_d_windowLogMax, 20))) {
            ZSTD_freeDCtx(context);
            return nullptr;
        }
        return context;
    }

    void Release(ZSTD_DCtx* context) {
        {
            std::scoped_lock lock{mutex};
            if (idle.size() < cap) {
                idle.push_back(context);
                return;
            }
        }
        ZSTD_freeDCtx(context);
    }

private:
    std::mutex mutex;
    std::vector<ZSTD_DCtx*> idle;
    std::size_t cap;
};

/// nx::ZstdDecoder over a DecoderPool. Accepts exactly one standard frame with no dictionary, a
/// declared content size equal to `output_capacity`, a window of at most 1 MiB and no trailing
/// bytes, which also rules out skippable and concatenated frames.
inline bool Decode(void* pool, const std::uint8_t* frame, std::size_t frame_size,
                   std::uint8_t* output, std::size_t output_capacity, std::size_t* written,
                   std::size_t* consumed) noexcept {
    if (pool == nullptr || frame == nullptr || output == nullptr || written == nullptr ||
        consumed == nullptr || output_capacity == 0 || output_capacity > MaxBlockSize) {
        return false;
    }
    ZSTD_frameHeader header{};
    if (ZSTD_getFrameHeader(&header, frame, frame_size) != 0 || header.frameType != ZSTD_frame ||
        header.dictID != 0 || (frame[4] & 0x03) != 0 ||
        header.frameContentSize != output_capacity || header.windowSize > MaxBlockSize) {
        return false;
    }
    const std::size_t frame_bytes = ZSTD_findFrameCompressedSize(frame, frame_size);
    if (ZSTD_isError(frame_bytes) || frame_bytes != frame_size) {
        return false;
    }
    auto& decoders = *static_cast<DecoderPool*>(pool);
    ZSTD_DCtx* context = decoders.Acquire();
    if (context == nullptr) {
        return false;
    }
    const std::size_t result =
        ZSTD_decompressDCtx(context, output, output_capacity, frame, frame_size);
    decoders.Release(context);
    if (ZSTD_isError(result) || result != output_capacity) {
        return false;
    }
    *written = result;
    *consumed = frame_size;
    return true;
}

} // namespace FileSys::NxZstd
